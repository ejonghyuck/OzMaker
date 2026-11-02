#include "pch.h"
#include "Capture.h"
#include "Util.h"
#include "WindowList.h"

#include "ScalePS.h"
#include "ScaleVS.h"

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;
using winrt::Windows::Foundation::Metadata::ApiInformation;

namespace {
// Keeps a sequence of immediate-context calls atomic while the video encoder uses the same device.
struct DeviceLock {
    ID3D11Multithread* mt;
    explicit DeviceLock(ID3D11Multithread* m) : mt(m) {
        if (mt) mt->Enter();
    }
    ~DeviceLock() {
        if (mt) mt->Leave();
    }
};
}  // namespace

Capture::~Capture() {
    if (running_) Stop();
}

bool Capture::IsSupported() {
    try {
        return wgc::GraphicsCaptureSession::IsSupported();
    } catch (...) {
        return false;
    }
}

bool Capture::Start(const CaptureOptions& opt, HWND notify, UINT closedMsg, std::wstring& err) {
    opt_ = opt;
    notify_ = notify;
    closedMsg_ = closedMsg;
    const bool video = opt.video.has_value();
    setupDone_ = setupFailed_ = false;
    failed_ = false;
    videoError_.clear();
    notice_.clear();
    try {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | (video ? D3D11_CREATE_DEVICE_VIDEO_SUPPORT : 0);
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, _countof(levels),
                                       D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
        if (FAILED(hr) && video)
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   levels, _countof(levels), D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
        winrt::check_hresult(hr);
        ctx_.As(&ctx1_);
        if (video && SUCCEEDED(ctx_.As(&mt_))) mt_->SetMultithreadProtected(TRUE);
        ComPtr<IDXGIDevice> dxgi;
        winrt::check_hresult(dev_.As(&dxgi));
        winrt::com_ptr<::IInspectable> insp;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), insp.put()));
        rtDevice_ = insp.as<wgdx::Direct3D11::IDirect3DDevice>();

        winrt::check_hresult(dev_->CreateVertexShader(g_ScaleVS, sizeof(g_ScaleVS), nullptr, &vs_));
        winrt::check_hresult(dev_->CreatePixelShader(g_ScalePS, sizeof(g_ScalePS), nullptr, &ps_));
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        winrt::check_hresult(dev_->CreateSamplerState(&sd, &sampler_));
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        winrt::check_hresult(dev_->CreateBuffer(&bd, nullptr, &cb_));

        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        HMONITOR mon = nullptr;
        if (opt.source == CaptureSource::Window && opt.monitorMode)
            mon = MonitorFromWindow(opt.target, MONITOR_DEFAULTTONEAREST);
        else if (opt.source == CaptureSource::Monitor)
            mon = opt.monitor;
        else if (opt.source == CaptureSource::Region)
            mon = opt.monitor ? opt.monitor : MonitorFromRect(&opt.region, MONITOR_DEFAULTTONEAREST);
        if (mon) {
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(mon, &mi);
            monitorRect_ = mi.rcMonitor;
            winrt::check_hresult(
                interop->CreateForMonitor(mon, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item_)));
        } else {
            winrt::check_hresult(interop->CreateForWindow(opt.target, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                          winrt::put_abi(item_)));
        }

        poolSize_ = item_.Size();
        pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            rtDevice_, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, poolSize_);
        session_ = pool_.CreateCaptureSession(item_);
        if (ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession",
                                              L"IsCursorCaptureEnabled"))
            session_.IsCursorCaptureEnabled(opt.cursor);
        if (ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession",
                                              L"IsBorderRequired")) {
            try {
                session_.IsBorderRequired(false);
            } catch (...) {
            }
        }

        // Clips keep at most `fps` frames; video samples at a fixed rate from frames arriving up to twice as often.
        interval_ = 10'000'000 / std::max(1, opt.fps) / (video ? 2 : 1);
        firstTs_ = -1;
        frames_ = 0;
        running_ = true;
        frameToken_ = pool_.FrameArrived([this](auto const& sender, auto const&) { OnFrame(sender); });
        closedToken_ = item_.Closed([this](auto const&, auto const&) {
            if (notify_) PostMessageW(notify_, closedMsg_, 0, 0);
        });
        startQpc_ = util::Qpc100ns();
        if (video) {
            writer_ = std::make_unique<Mp4Writer>();
            VideoOptions& vo = *opt_.video;
            if (vo.audio.enabled) {
                std::wstring aerr;
                auto src = vo.processAudio ? AudioCapture::Source::Process : AudioCapture::Source::System;
                Mp4Writer* w = writer_.get();
                if (!audio_.Start(src, vo.audioPid, vo.audio.sampleRate, startQpc_,
                                  [w](const int16_t* pcm, size_t n) { w->PushAudio(pcm, n); }, aerr)) {
                    vo.audio.enabled = false;  // never fall back to a different audio source
                    notice_ = aerr + L" 소리 없이 녹화했습니다.";
                }
            }
            tickStop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            firstFrame_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            ticker_ = std::thread([this] { TickLoop(); });
        }
        session_.StartCapture();
    } catch (winrt::hresult_error const& e) {
        running_ = false;
        err = L"캡처를 시작할 수 없습니다. (" + std::wstring(e.message()) + L")";
        if (ticker_.joinable()) {
            SetEvent(tickStop_);
            ticker_.join();
        }
        audio_.Stop(0);
        writer_.reset();
        ReleaseGpu();
        return false;
    }
    return true;
}

double Capture::Elapsed() const { return running_ ? (util::Qpc100ns() - startQpc_) / 1e7 : 0; }

size_t Capture::Frames() const { return frames_.load(); }

bool Capture::SourceRect(int cw, int ch, RECT& r) const {
    RECT full{0, 0, cw, ch};
    r = full;
    if (opt_.source == CaptureSource::Monitor) {
        // whole monitor
    } else if (opt_.source == CaptureSource::Region) {
        RECT reg = opt_.region;
        OffsetRect(&reg, -monitorRect_.left, -monitorRect_.top);
        if (!IntersectRect(&r, &reg, &full)) return false;
    } else if (opt_.monitorMode) {
        RECT wr{};
        if (opt_.clientOnly) {
            WINDOWINFO wi{sizeof(wi)};
            if (!GetWindowInfo(opt_.target, &wi)) return false;
            wr = wi.rcClient;
        } else if (!GetVisibleFrame(opt_.target, wr)) {
            return false;
        }
        OffsetRect(&wr, -monitorRect_.left, -monitorRect_.top);
        if (!IntersectRect(&r, &wr, &full)) return false;
    } else if (opt_.clientOnly) {
        // Window frames are delivered at the window's visible bounds; locate the client area inside them.
        WINDOWINFO wi{sizeof(wi)};
        RECT efb{};
        if (GetWindowInfo(opt_.target, &wi) && GetVisibleFrame(opt_.target, efb)) {
            const RECT* base = nullptr;
            if (efb.right - efb.left == cw && efb.bottom - efb.top == ch)
                base = &efb;
            else if (wi.rcWindow.right - wi.rcWindow.left == cw && wi.rcWindow.bottom - wi.rcWindow.top == ch)
                base = &wi.rcWindow;
            if (base) {
                RECT c = wi.rcClient;
                OffsetRect(&c, -base->left, -base->top);
                if (!IntersectRect(&r, &c, &full)) r = full;
            }
        }
    }
    return r.right - r.left >= 2 && r.bottom - r.top >= 2;
}

// Monitor capture records whatever is on screen over the window. While the window isn't in front (Alt+Tab out of
// a game), the parts other windows cover are blacked out so a browser or messenger never ends up in the recording.
void Capture::CoveredRects(const RECT& sr, std::vector<RECT>& out) const {
    out.clear();
    HWND target = opt_.target;
    DWORD targetPid = 0;
    GetWindowThreadProcessId(target, &targetPid);
    if (HWND fg = GetForegroundWindow()) {
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid == targetPid) return;  // the window itself, or one of its own popups, is in front
    }
    RECT area = sr;  // the recorded part of the monitor, in screen pixels
    OffsetRect(&area, monitorRect_.left, monitorRect_.top);
    for (HWND w = GetTopWindow(nullptr); w && w != target; w = GetWindow(w, GW_HWNDNEXT)) {
        RECT r;
        if (!IsWindowVisible(w) || IsIconic(w) || !GetWindowRect(w, &r) || !IntersectRect(&r, &r, &area)) continue;
        if (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TRANSPARENT) continue;  // click-through overlays
        DWORD pid = 0, affinity = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == targetPid) continue;
        if (GetWindowDisplayAffinity(w, &affinity) && affinity != WDA_NONE) continue;  // not captured anyway
        BOOL cloaked = FALSE;
        if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) continue;
        RECT frame;  // without the invisible resize borders
        if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) &&
            !IntersectRect(&r, &frame, &area))
            continue;
        OffsetRect(&r, -area.left, -area.top);
        out.push_back(r);
    }
}

bool Capture::Setup(int sw, int sh) {
    double f = 1.0;
    int longE = std::max(sw, sh), shortE = std::min(sw, sh);
    if (opt_.maxEdge > 0) f = std::min(f, (double)opt_.maxEdge / longE);
    if (opt_.boxLong > 0) f = std::min(f, (double)opt_.boxLong / longE);
    if (opt_.boxShort > 0) f = std::min(f, (double)opt_.boxShort / shortE);
    int w = (int)std::lround(sw * f), h = (int)std::lround(sh * f);
    // Even dimensions keep 4:2:0 encoders (H.264/HEVC/AVIF/WebP) exact.
    w = std::max(2, w & ~1);
    h = std::max(2, h & ~1);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &out_)) ||
        FAILED(dev_->CreateRenderTargetView(out_.Get(), nullptr, &outRtv_)))
        return false;

    if (!opt_.video) {
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& s : staging_)
            if (FAILED(dev_->CreateTexture2D(&td, nullptr, &s))) return false;
        auto store = std::make_shared<FrameStore>();
        std::wstring err;
        if (!store->Open(w, h, err)) return false;
        store_ = std::move(store);
    }
    outW_ = w;
    outH_ = h;
    return true;
}

void Capture::Fail(std::wstring msg) {
    if (failed_.exchange(true)) return;
    videoError_ = std::move(msg);
    if (notify_) PostMessageW(notify_, closedMsg_, 1, 0);
}

void Capture::Process(ID3D11Texture2D* tex, const RECT& sr, int64_t ts, const std::vector<RECT>& covered) {
    int sw = sr.right - sr.left, sh = sr.bottom - sr.top;
    if (!setupDone_) {
        if (setupFailed_) return;
        if (!Setup(sw, sh)) {
            setupFailed_ = true;
            if (opt_.video) Fail(L"녹화 버퍼를 만들 수 없습니다.");
            return;
        }
        setupDone_ = true;
    }
    const bool video = opt_.video.has_value();
    const bool same = sw - outW_ >= 0 && sw - outW_ <= 1 && sh - outH_ >= 0 && sh - outH_ <= 1;
    const bool mask = !covered.empty() && ctx1_;
    DeviceLock lock(mt_.Get());

    // Clip mode copies into the next staging texture (through out_ when it is scaled or masked); video mode keeps
    // the latest frame in out_.
    ID3D11Texture2D* staging = video ? nullptr : staging_[ring_].Get();
    float fx = 1.f, fy = 1.f;  // source -> output pixels
    int dx = 0, dy = 0;
    if (same) {
        // Same size (an odd edge is trimmed by one pixel): plain copy, no resampling.
        D3D11_BOX box{(UINT)sr.left, (UINT)sr.top, 0, (UINT)(sr.left + outW_), (UINT)(sr.top + outH_), 1};
        ctx_->CopySubresourceRegion(video || mask ? out_.Get() : staging, 0, 0, 0, 0, tex, 0, &box);
    } else {
        float f = std::min((float)outW_ / sw, (float)outH_ / sh);
        int dw = std::clamp((int)std::lround(sw * f), 1, outW_);
        int dh = std::clamp((int)std::lround(sh * f), 1, outH_);
        dx = (outW_ - dw) / 2;
        dy = (outH_ - dh) / 2;
        fx = (float)dw / sw;
        fy = (float)dh / sh;
        float scale = std::max((float)sw / dw, (float)sh / dh);
        int lod = scale > 1.f ? std::max(0, (int)std::lround(std::log2(scale)) - 2) : 0;

        if (!mip_ || mipW_ != sw || mipH_ != sh || mipLevels_ != lod + 1) {
            mip_.Reset();
            mipSrv_.Reset();
            D3D11_TEXTURE2D_DESC td{};
            td.Width = sw;
            td.Height = sh;
            td.MipLevels = lod + 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            td.MiscFlags = lod > 0 ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
            if (FAILED(dev_->CreateTexture2D(&td, nullptr, &mip_)) ||
                FAILED(dev_->CreateShaderResourceView(mip_.Get(), nullptr, &mipSrv_)))
                return;
            mipW_ = sw;
            mipH_ = sh;
            mipLevels_ = lod + 1;
        }
        D3D11_BOX box{(UINT)sr.left, (UINT)sr.top, 0, (UINT)sr.right, (UINT)sr.bottom, 1};
        ctx_->CopySubresourceRegion(mip_.Get(), 0, 0, 0, 0, tex, 0, &box);
        if (lod > 0) ctx_->GenerateMips(mipSrv_.Get());

        struct {
            float fx, fy, lod, pad;
        } params{1.f / dw, 1.f / dh, (float)lod, 0.f};
        ctx_->UpdateSubresource(cb_.Get(), 0, nullptr, &params, 0, 0);
        if (dw != outW_ || dh != outH_) {
            const float black[4] = {0, 0, 0, 1};
            ctx_->ClearRenderTargetView(outRtv_.Get(), black);
        }
        D3D11_VIEWPORT vp{(float)dx, (float)dy, (float)dw, (float)dh, 0.f, 1.f};
        ctx_->OMSetRenderTargets(1, outRtv_.GetAddressOf(), nullptr);
        ctx_->RSSetViewports(1, &vp);
        ctx_->IASetInputLayout(nullptr);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->VSSetShader(vs_.Get(), nullptr, 0);
        ctx_->PSSetShader(ps_.Get(), nullptr, 0);
        ctx_->PSSetShaderResources(0, 1, mipSrv_.GetAddressOf());
        ctx_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
        ctx_->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
        ctx_->Draw(3, 0);
        ID3D11ShaderResourceView* nullSrv = nullptr;
        ctx_->PSSetShaderResources(0, 1, &nullSrv);
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    }
    if (mask) {
        std::vector<D3D11_RECT> rects;
        rects.reserve(covered.size());
        for (const RECT& c : covered) {
            D3D11_RECT r{dx + (LONG)std::floor(c.left * fx), dy + (LONG)std::floor(c.top * fy),
                         dx + (LONG)std::ceil(c.right * fx), dy + (LONG)std::ceil(c.bottom * fy)};
            r.left = std::clamp<LONG>(r.left, 0, outW_);
            r.right = std::clamp<LONG>(r.right, 0, outW_);
            r.top = std::clamp<LONG>(r.top, 0, outH_);
            r.bottom = std::clamp<LONG>(r.bottom, 0, outH_);
            if (r.right > r.left && r.bottom > r.top) rects.push_back(r);
        }
        const float black[4] = {0, 0, 0, 1};
        if (!rects.empty()) ctx1_->ClearView(outRtv_.Get(), black, rects.data(), (UINT)rects.size());
    }
    if (!video && (!same || mask)) ctx_->CopyResource(staging, out_.Get());
    if (video) {
        SetEvent(firstFrame_);
        return;
    }
    pending_.push_back({ring_, ts});
    ring_ = (ring_ + 1) % kRing;
    // Read back with one frame of latency so Map() doesn't stall on the copy just issued.
    while ((int)pending_.size() >= kRing - 1) DrainOne();
}

void Capture::DrainOne() {
    Pending p = pending_.front();
    pending_.pop_front();
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(staging_[p.idx].Get(), 0, D3D11_MAP_READ, 0, &m))) return;
    auto buf = store_->Acquire();
    uint8_t* dst = buf->data();
    const uint8_t* src = static_cast<const uint8_t*>(m.pData);
    for (int y = 0; y < outH_; ++y) {
        auto* d = reinterpret_cast<uint32_t*>(dst + (size_t)y * outW_ * 4);
        auto* s = reinterpret_cast<const uint32_t*>(src + (size_t)y * m.RowPitch);
        for (int x = 0; x < outW_; ++x) d[x] = s[x] | 0xFF000000u;
    }
    ctx_->Unmap(staging_[p.idx].Get(), 0);
    if (store_->Submit(std::move(buf), p.ts)) ++frames_;
}

void Capture::TickLoop() {
    // Constant frame rate for the MP4: every 1/fps the newest captured frame is encoded, repeating it while the
    // source is idle (Windows only delivers frames when something changes).
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    bool coarse = timer == nullptr;
    if (coarse) {
        timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
        timeBeginPeriod(1);
    }
    // The encoder starts here rather than in the frame callback: Windows.Graphics.Capture may hold the device lock
    // while it delivers frames, and the encoder needs that lock from its own threads.
    HANDLE first[2] = {tickStop_, firstFrame_};
    bool open = false;
    if (WaitForMultipleObjects(2, first, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        VideoConfig vc = opt_.video->video;
        vc.width = outW_;
        vc.height = outH_;
        vc.fps = opt_.fps;
        std::wstring err;
        open = writer_->Open(opt_.video->path, vc, opt_.video->audio, dev_.Get(), err);
        if (!open) Fail(err);
    }
    if (open) {
        const int fps = std::max(1, opt_.fps);
        // The ticks before the first frame (encoder start-up) are caught up in a burst showing that frame;
        // once running in real time, a stall skips ticks instead.
        bool synced = false;
        for (int64_t k = 0;; ++k) {
            int64_t due = startQpc_ + k * 10'000'000 / fps;
            int64_t now = util::Qpc100ns();
            if (due > now) {
                synced = true;
                LARGE_INTEGER li;
                li.QuadPart = -(due - now);
                SetWaitableTimer(timer, &li, 0, nullptr, nullptr, FALSE);
                HANDLE waits[2] = {tickStop_, timer};
                if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) break;
            } else if (WaitForSingleObject(tickStop_, 0) == WAIT_OBJECT_0) {
                break;
            } else if (synced) {
                int64_t cur = (now - startQpc_) * fps / 10'000'000;
                if (cur > k + 2) k = cur;
            }
            // Never under mtx_: the frame callback may own the device lock while it waits for mtx_.
            ComPtr<IMFSample> sample = writer_->Convert(out_.Get());
            if (!sample) continue;  // encoder pool momentarily full: this tick is dropped
            int64_t t0 = k * 10'000'000 / fps, t1 = (k + 1) * 10'000'000 / fps;
            if (!writer_->WriteVideo(sample.Get(), t0, t1 - t0)) {
                Fail(L"동영상 인코딩 중 오류가 발생했습니다. 디스크 공간을 확인하세요.");
                break;
            }
            ++frames_;
        }
    }
    if (coarse) timeEndPeriod(1);
    CloseHandle(timer);
}

void Capture::OnFrame(wgc::Direct3D11CaptureFramePool const& pool) {
    std::lock_guard lk(mtx_);
    if (!running_) return;
    wgc::Direct3D11CaptureFrame frame{nullptr};
    try {
        frame = pool.TryGetNextFrame();
    } catch (...) {
        return;
    }
    if (!frame) return;

    auto cs = frame.ContentSize();
    int64_t ts = frame.SystemRelativeTime().count();
    bool accept = false;
    if (firstTs_ < 0) {
        accept = true;
        firstTs_ = ts;
        nextDue_ = ts + interval_;
    } else if (ts + interval_ / 4 >= nextDue_) {
        // Small tolerance so 60 Hz content sampled at 30 fps doesn't lose frames to jitter.
        accept = true;
        nextDue_ = (nextDue_ + interval_ > ts) ? nextDue_ + interval_ : ts + interval_;
    }
    if (accept) {
        RECT sr;
        if (SourceRect(cs.Width, cs.Height, sr)) {
            if (opt_.monitorMode)
                CoveredRects(sr, covered_);
            else
                covered_.clear();
            try {
                auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                ComPtr<ID3D11Texture2D> tex;
                if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(&tex)))) {
                    Process(tex.Get(), sr, ts - firstTs_, covered_);
                    lastTs_ = ts - firstTs_;
                    lastArrivalQpc_ = util::Qpc100ns();
                }
            } catch (...) {
            }
        }
    }
    frame.Close();
    if ((cs.Width != poolSize_.Width || cs.Height != poolSize_.Height) && cs.Width > 0 && cs.Height > 0) {
        poolSize_ = cs;
        try {
            pool.Recreate(rtDevice_, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, cs);
        } catch (...) {
        }
    }
}

CaptureResult Capture::Stop() {
    CaptureResult res;
    if (!running_.exchange(false)) return res;
    int64_t stopQpc = util::Qpc100ns();
    try {
        if (pool_) pool_.FrameArrived(frameToken_);
        if (item_) item_.Closed(closedToken_);
        if (session_) session_.Close();
        if (pool_) pool_.Close();
    } catch (...) {
    }
    if (opt_.video) {
        SetEvent(tickStop_);
        if (ticker_.joinable()) ticker_.join();
        audio_.Stop(stopQpc);  // pads silence up to the stop press
        { std::lock_guard lk(mtx_); }  // lets a frame callback that is still running finish
        res.video = true;
        res.path = opt_.video->path;
        res.notice = notice_;
        res.seconds = (stopQpc - startQpc_) / 1e7;
        std::wstring err = videoError_;
        if (err.empty()) {
            res.ok = writer_->Finish(err);
        } else {
            writer_->Abort();
        }
        res.error = err;
        res.encoder = writer_->EncoderName();
        res.hardware = writer_->HardwareEncoder();
        res.audio = res.ok && writer_->HasAudio();
        if (res.ok) res.bytes = util::FileSize(res.path);
        writer_.reset();
        CloseHandle(tickStop_);
        CloseHandle(firstFrame_);
        tickStop_ = firstFrame_ = nullptr;
    } else {
        std::lock_guard lk(mtx_);
        while (!pending_.empty()) DrainOne();
        if (store_) {
            // Last frame stays on screen until the moment Stop was pressed.
            int64_t endTs = lastTs_ + std::max<int64_t>(0, stopQpc - lastArrivalQpc_);
            store_->Finish(endTs);
            if (store_->Count() > 0) res.clip = store_;
            store_.reset();
        }
    }
    ReleaseGpu();
    return res;
}

void Capture::ReleaseGpu() {
    session_ = nullptr;
    pool_ = nullptr;
    item_ = nullptr;
    rtDevice_ = nullptr;
    for (auto& s : staging_) s.Reset();
    pending_.clear();
    out_.Reset();
    outRtv_.Reset();
    mip_.Reset();
    mipSrv_.Reset();
    mipW_ = mipH_ = mipLevels_ = 0;
    cb_.Reset();
    sampler_.Reset();
    ps_.Reset();
    vs_.Reset();
    if (ctx_) ctx_->ClearState();
    mt_.Reset();
    ctx1_.Reset();
    ctx_.Reset();
    dev_.Reset();
}
