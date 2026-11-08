#include "pch.h"
#include "Capture.h"
#include "Util.h"
#include "WindowList.h"

#include "DiffPS.h"
#include "ScalePS.h"
#include "ScaleVS.h"

#include <future>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

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

constexpr int64_t kRipple = 5'000'000;  // a click's ripple lasts 0.5 s

// Our own windows (the recording pill): clicks on them aren't highlighted.
bool OwnWindowAt(POINT pt) {
    HWND h = WindowFromPoint(pt);
    DWORD pid = 0;
    if (h) GetWindowThreadProcessId(h, &pid);
    return pid == GetCurrentProcessId();
}

bool BorderToggle() {
    try {
        return ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession",
                                                 L"IsBorderRequired");
    } catch (...) {
        return false;
    }
}

std::once_flag g_borderOnce;
std::shared_future<int> g_border;
}  // namespace

void Capture::RequestBorderless() {
    std::call_once(g_borderOnce, [] {
        std::promise<int> answer;
        g_border = answer.get_future().share();
        if (!BorderToggle()) {
            answer.set_value(-2);
            return;
        }
        std::thread([answer = std::move(answer)]() mutable {
            int status = -1;
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                status = (int)wgc::GraphicsCaptureAccess::RequestAccessAsync(wgc::GraphicsCaptureAccessKind::Borderless)
                             .get();
                winrt::uninit_apartment();
            } catch (...) {
            }
            answer.set_value(status);
        }).detach();
    });
}

int Capture::BorderlessAccess(int waitMs) {
    RequestBorderless();
    if (g_border.wait_for(std::chrono::milliseconds(waitMs)) != std::future_status::ready) return -1;
    return g_border.get();
}

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

bool Capture::Start(const CaptureOptions& opt, HWND notify, UINT closedMsg, std::wstring& err, LPARAM tag) {
    opt_ = opt;
    notify_ = notify;
    closedMsg_ = closedMsg;
    tag_ = tag;
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
        // The encoder and the click highlights share the device, and a screenshot reads it from the window's thread.
        if (SUCCEEDED(ctx_.As(&mt_))) mt_->SetMultithreadProtected(TRUE);
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
        // Windows 11 turns the border off only once the request was allowed; until then monitors go through
        // duplication, as on Windows 10. The answer is usually in by now (asked at startup).
        const bool toggle = BorderToggle();
        const bool borderless = toggle && BorderlessAccess(500) == 4;  // AppCapabilityAccessStatus::Allowed
        bool duplicate = false;
        if (mon) {
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(mon, &mi);
            monitorRect_ = mi.rcMonitor;
            dupMonitor_ = mon;
            cursor_ = nullptr;
            duplicate = opt.screenVia != 2 && (opt.screenVia == 1 || !borderless) && Duplicate();
            if (!duplicate)  // another GPU drives the monitor, a rotated screen, or no duplication left
                winrt::check_hresult(
                    interop->CreateForMonitor(mon, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item_)));
        } else {
            winrt::check_hresult(interop->CreateForWindow(opt.target, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                          winrt::put_abi(item_)));
        }

        if (!duplicate) {
            poolSize_ = item_.Size();
            pool_ = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
                rtDevice_, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, poolSize_);
            session_ = pool_.CreateCaptureSession(item_);
            if (ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession",
                                                  L"IsCursorCaptureEnabled"))
                session_.IsCursorCaptureEnabled(opt.cursor);
            if (toggle) {
                try {
                    session_.IsBorderRequired(false);
                } catch (...) {
                }
            }
        }

        // Clips keep at most `fps` frames. Video takes frames up to twice as often (a copy each; only the newest is
        // scaled, once per tick) and samples them at a fixed rate.
        interval_ = 10'000'000 / std::max(1, opt.fps) / (video ? 2 : 1);
        firstTs_ = -1;
        lastTs_ = 0;
        frames_ = 0;
        scaled_ = 0;
        finishDue_ = false;
        shots_ = 0;
        stopAt_ = 0;
        pausedAt_ = 0;
        pausedTotal_ = 0;
        running_ = true;
        if (!duplicate) {
            frameToken_ = pool_.FrameArrived([this](auto const& sender, auto const&) { OnFrame(sender); });
            closedToken_ = item_.Closed([this](auto const&, auto const&) {
                if (notify_) PostMessageW(notify_, closedMsg_, 0, tag_);
            });
        }
        startQpc_ = util::Qpc100ns();
        if (video) {
            writer_ = std::make_unique<Mp4Writer>();
            VideoOptions& vo = *opt_.video;
            if (vo.audio.enabled) {
                Mp4Writer* w = writer_.get();
                mix_.Reset(2, [w](const int16_t* pcm, size_t n) { w->PushAudio(pcm, n); });
                mix_.SetGain(1, vo.micGain);
                std::wstring aerr, merr;
                bool sound = vo.sound, mic = vo.mic;
                // A source that fails is left out; never a different source in its place.
                // The whole output, but without OzMaker's own sounds (the start and pause cues, the mic check).
                auto src = vo.processAudio                           ? AudioCapture::Source::Process
                           : AudioCapture::ProcessLoopbackSupported() ? AudioCapture::Source::Others
                                                                      : AudioCapture::Source::System;
                if (sound && !audio_.Start(src, vo.audioPid, vo.audio.sampleRate, startQpc_, mix_.Input(0), aerr))
                    sound = false;
                if (!sound) mix_.Drop(0);
                if (mic && !mic_.Start(AudioCapture::Source::Mic, 0, vo.audio.sampleRate, startQpc_, mix_.Input(1), merr,
                                       vo.micDevice))
                    mic = false;
                if (!mic) mix_.Drop(1);
                vo.audio.enabled = sound || mic;
                std::wstring lost = vo.sound && !sound ? aerr : L"";
                if (vo.mic && !mic) lost += (lost.empty() ? L"" : L" ") + merr;
                if (!lost.empty())
                    notice_ = lost + (!vo.audio.enabled ? tr(L" 소리 없이 녹화했습니다.")
                                      : !sound      ? tr(L" 마이크 소리만 녹음했습니다.")
                                                    : tr(L" 마이크 없이 녹화했습니다."));
            }
            tickStop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            firstFrame_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            resumed_ = CreateEventW(nullptr, TRUE, TRUE, nullptr);
            ticker_ = std::thread([this] { TickLoop(); });
        }
        if (opt.clickFx) {
            HMONITOR mon = opt.source == CaptureSource::Window ? MonitorFromWindow(opt.target, MONITOR_DEFAULTTONEAREST)
                                                               : opt.monitor;
            UINT dx = 96, dy = 96;
            if (!mon || FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
            dpiScale_ = dx / 96.f;
            clicks_.clear();
            fxShown_ = false;
            clickFrames_ = 0;
            fxFailed_ = false;
            clickStop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            clickThread_ = std::thread([this] { ClickLoop(); });
        }
        if (duplicate) {
            dupStop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            dupThread_ = std::thread([this] { DupLoop(); });
        } else {
            session_.StartCapture();
        }
    } catch (winrt::hresult_error const& e) {
        running_ = false;
        err = tr(L"캡처를 시작할 수 없습니다. (") + std::wstring(e.message()) + L")";
        if (ticker_.joinable()) {
            SetEvent(tickStop_);
            ticker_.join();
        }
        for (HANDLE* h : {&tickStop_, &firstFrame_, &resumed_})
            if (*h) {
                CloseHandle(*h);
                *h = nullptr;
            }
        audio_.Stop(0);
        mic_.Stop(0);
        StopClicks();
        writer_.reset();
        ReleaseGpu();
        return false;
    }
    return true;
}

double Capture::Elapsed() const {
    if (!running_) return 0;
    int64_t at = pausedAt_, now = at ? at : util::Qpc100ns();
    return (now - startQpc_ - pausedTotal_) / 1e7;
}

void Capture::SetPaused(bool on) {
    if (!running_ || on == Paused()) return;
    int64_t now = util::Qpc100ns();
    if (on) {
        if (resumed_) ResetEvent(resumed_);
        pausedAt_ = now;
        for (AudioCapture* a : {&audio_, &mic_})
            if (opt_.video && a->Running()) a->Pause(now);
    } else {
        // The total first: the ticker reads pausedAt_, then the total.
        pausedTotal_ += now - pausedAt_;
        pausedAt_ = 0;
        for (AudioCapture* a : {&audio_, &mic_})
            if (opt_.video && a->Running()) a->Resume(now);
        if (resumed_) SetEvent(resumed_);
    }
}

size_t Capture::Frames() const { return frames_.load(); }

bool Capture::StoreFailed() const {
    std::lock_guard lk(mtx_);
    return store_ && store_->WriteFailed();
}

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
    haveClean_ = false;
    clean_.Reset();
    if (opt_.monitorMode && FAILED(dev_->CreateTexture2D(&td, nullptr, &clean_))) clean_.Reset();  // black then
    diffReady_ = diffPending_ = false;
    if (opt_.video && opt_.video->video.vfr) {
        // What the comparison needs. Without it every picture that may have changed counts as changed.
        D3D11_TEXTURE2D_DESC cd = td, dd = td;
        cd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        diffW_ = (UINT)(w + 15) / 16;
        diffH_ = (UINT)(h + 15) / 16;
        dd.Width = diffW_;
        dd.Height = diffH_;
        dd.Format = DXGI_FORMAT_R8_UNORM;
        ComPtr<ID3D11Texture2D> diff;
        D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
        diffReady_ = SUCCEEDED(dev_->CreateTexture2D(&cd, nullptr, &cand_)) &&
                     SUCCEEDED(dev_->CreateTexture2D(&cd, nullptr, &prev_)) &&
                     SUCCEEDED(dev_->CreateShaderResourceView(cand_.Get(), nullptr, &candSrv_)) &&
                     SUCCEEDED(dev_->CreateShaderResourceView(prev_.Get(), nullptr, &prevSrv_)) &&
                     SUCCEEDED(dev_->CreateTexture2D(&dd, nullptr, &diff)) &&
                     SUCCEEDED(dev_->CreateRenderTargetView(diff.Get(), nullptr, &diffRtv_)) &&
                     SUCCEEDED(dev_->CreatePixelShader(g_DiffPS, sizeof(g_DiffPS), nullptr, &diffPs_)) &&
                     SUCCEEDED(dev_->CreateQuery(&qd, &diffQuery_));
    }

    if (!opt_.video) {
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& s : staging_)
            if (FAILED(dev_->CreateTexture2D(&td, nullptr, &s))) return false;
        auto store = std::make_shared<FrameStore>();
        std::wstring err;
        if (!store->Open(w, h, err)) return false;
        store->SetNominalFps(opt_.fps);
        store_ = std::move(store);
    }
    outW_ = w;
    outH_ = h;
    return true;
}

void Capture::Fail(std::wstring msg) {
    if (failed_.exchange(true)) return;
    videoError_ = std::move(msg);
    if (notify_) PostMessageW(notify_, closedMsg_, 1, tag_);
}

void Capture::Process(ID3D11Texture2D* tex, const RECT& sr, int64_t ts, const std::vector<RECT>& covered,
                      int64_t qpc, bool emit) {
    int sw = sr.right - sr.left, sh = sr.bottom - sr.top;
    if (!setupDone_) {
        if (setupFailed_) return;
        if (!Setup(sw, sh)) {
            setupFailed_ = true;
            if (opt_.video) Fail(tr(L"녹화 버퍼를 만들 수 없습니다."));
            return;
        }
        setupDone_ = true;
    }
    const bool video = opt_.video.has_value();
    const bool same = sw - outW_ >= 0 && sw - outW_ <= 1 && sh - outH_ >= 0 && sh - outH_ <= 1;
    const bool mask = !covered.empty() && ctx1_;
    const bool keep = clean_ != nullptr;  // out_ is also kept in clean_, so the picture always goes through it
    DeviceLock lock(mt_.Get());

    // Clip mode copies into the next staging texture (through out_ when it is scaled or masked). Video mode keeps
    // the newest frame: in out_ when it is copied as is, else in mip_ until the ticker scales it (FinishFrame), so
    // frames arriving faster than the frame rate aren't all scaled only for most of them to be skipped.
    ID3D11Texture2D* staging = video ? nullptr : staging_[ring_].Get();
    float fx = 1.f, fy = 1.f;  // source -> output pixels
    int dx = 0, dy = 0;
    finish_.scaled = !same;
    if (same) {
        // Same size (an odd edge is trimmed by one pixel): plain copy, no resampling.
        D3D11_BOX box{(UINT)sr.left, (UINT)sr.top, 0, (UINT)(sr.left + outW_), (UINT)(sr.top + outH_), 1};
        ctx_->CopySubresourceRegion(video || mask || keep || opt_.clickFx ? out_.Get() : staging, 0, 0, 0, 0, tex, 0,
                                    &box);
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
        finish_.dx = dx;
        finish_.dy = dy;
        finish_.dw = dw;
        finish_.dh = dh;
        finish_.lod = lod;
    }
    if (opt_.clickFx)
        clickMap_ = {(float)(texOrigin_.x + sr.left), (float)(texOrigin_.y + sr.top), fx, fy, (float)dx, (float)dy};
    finish_.mask.clear();
    if (mask) {
        for (const RECT& c : covered) {
            D3D11_RECT r{dx + (LONG)std::floor(c.left * fx), dy + (LONG)std::floor(c.top * fy),
                         dx + (LONG)std::ceil(c.right * fx), dy + (LONG)std::ceil(c.bottom * fy)};
            r.left = std::clamp<LONG>(r.left, 0, outW_);
            r.right = std::clamp<LONG>(r.right, 0, outW_);
            r.top = std::clamp<LONG>(r.top, 0, outH_);
            r.bottom = std::clamp<LONG>(r.bottom, 0, outH_);
            if (r.right > r.left && r.bottom > r.top) finish_.mask.push_back(r);
        }
    }
    if (video) ++shots_;
    if (video && finish_.scaled) {
        finishDue_ = true;  // the ticker scales it if it is still the newest at the next tick
        SetEvent(firstFrame_);
        return;
    }
    FinishFrame();
    if (video) {
        SetEvent(firstFrame_);
        return;
    }
    if (!emit) return;
    ID3D11Texture2D* from = out_.Get();
    if (opt_.clickFx) {
        fxShown_ = DrawClicks(qpc);
        if (fxShown_) from = fx_.Get();
    }
    if (!same || mask || keep || opt_.clickFx) ctx_->CopyResource(staging, from);
    pending_.push_back({ring_, ts});
    ring_ = (ring_ + 1) % kRing;
    // Read back with one frame of latency so Map() doesn't stall on the copy just issued.
    while ((int)pending_.size() >= kRing - 1) DrainOne();
}

// Under the device lock: the frame Process took, scaled into out_ (when it needs it) and masked.
void Capture::FinishFrame() {
    finishDue_ = false;
    const Finish& f = finish_;
    if (f.scaled) {
        if (f.lod > 0) ctx_->GenerateMips(mipSrv_.Get());
        struct {
            float fx, fy, lod, overWhite;
        } params{1.f / f.dw, 1.f / f.dh, (float)f.lod, 0.f};
        ctx_->UpdateSubresource(cb_.Get(), 0, nullptr, &params, 0, 0);
        if (f.dw != outW_ || f.dh != outH_) {
            const float black[4] = {0, 0, 0, 1};
            ctx_->ClearRenderTargetView(outRtv_.Get(), black);
        }
        D3D11_VIEWPORT vp{(float)f.dx, (float)f.dy, (float)f.dw, (float)f.dh, 0.f, 1.f};
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
        ++scaled_;
    }
    if (!f.mask.empty() && haveClean_) {
        // Another window over the game while it isn't in front (Alt+Tab): never in the recording, and not black
        // either: the game as it was last seen with nothing over it.
        for (const D3D11_RECT& r : f.mask) {
            D3D11_BOX box{(UINT)r.left, (UINT)r.top, 0, (UINT)r.right, (UINT)r.bottom, 1};
            ctx_->CopySubresourceRegion(out_.Get(), 0, r.left, r.top, 0, clean_.Get(), 0, &box);
        }
    } else if (!f.mask.empty()) {
        const float black[4] = {0, 0, 0, 1};
        ctx1_->ClearView(outRtv_.Get(), black, f.mask.data(), (UINT)f.mask.size());
    } else if (clean_) {
        ctx_->CopyResource(clean_.Get(), out_.Get());
        haveClean_ = true;
    }
}

ID3D11Texture2D* Capture::Snapshot(ID3D11Texture2D* picture, bool compare) {
    diffPending_ = false;
    if (!diffReady_) return picture;
    ctx_->CopyResource(cand_.Get(), picture);
    if (!compare) return cand_.Get();
    D3D11_VIEWPORT vp{0.f, 0.f, (float)diffW_, (float)diffH_, 0.f, 1.f};
    ID3D11ShaderResourceView* srv[2] = {candSrv_.Get(), prevSrv_.Get()};
    ctx_->OMSetRenderTargets(1, diffRtv_.GetAddressOf(), nullptr);
    ctx_->RSSetViewports(1, &vp);
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(diffPs_.Get(), nullptr, 0);
    ctx_->PSSetShaderResources(0, 2, srv);
    ctx_->Begin(diffQuery_.Get());
    ctx_->Draw(3, 0);
    ctx_->End(diffQuery_.Get());
    ID3D11ShaderResourceView* none[2] = {};
    ctx_->PSSetShaderResources(0, 2, none);
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    diffPending_ = true;
    return cand_.Get();
}

bool Capture::Changed() {
    if (!diffPending_) return true;
    diffPending_ = false;
    BOOL any = TRUE;
    HRESULT hr;
    while ((hr = ctx_->GetData(diffQuery_.Get(), &any, sizeof(any), 0)) == S_FALSE) SwitchToThread();
    return FAILED(hr) || any;
}

void Capture::Written() {
    if (diffReady_) ctx_->CopyResource(prev_.Get(), cand_.Get());
}

// ---------------------------------------------------------------- click highlight

void Capture::InjectClick(POINT at, int64_t hold) {
    std::lock_guard lk(clickMtx_);
    int64_t now = util::Qpc100ns();
    clicks_.push_back({now, now + hold, at, false});
}

void Capture::StopClicks() {
    if (!clickThread_.joinable()) return;
    SetEvent(clickStop_);
    clickThread_.join();
    CloseHandle(clickStop_);
    clickStop_ = nullptr;
}

POINT Capture::TextureOrigin(int cw, int ch) const {
    if (opt_.source != CaptureSource::Window || opt_.monitorMode) return {monitorRect_.left, monitorRect_.top};
    // Window frames come at the window's visible bounds (else its full rectangle).
    RECT efb{};
    WINDOWINFO wi{sizeof(wi)};
    bool haveEfb = GetVisibleFrame(opt_.target, efb);
    if (haveEfb && efb.right - efb.left == cw && efb.bottom - efb.top == ch) return {efb.left, efb.top};
    if (GetWindowInfo(opt_.target, &wi) && wi.rcWindow.right - wi.rcWindow.left == cw) return {wi.rcWindow.left, wi.rcWindow.top};
    return {efb.left, efb.top};
}

void Capture::ClickLoop() {
    const bool swap = GetSystemMetrics(SM_SWAPBUTTON) != 0;  // left-handed: the physical buttons are swapped
    bool was[2] = {};
    while (WaitForSingleObject(clickStop_, 8) == WAIT_TIMEOUT) {
        const int64_t now = util::Qpc100ns();
        POINT pt{};
        GetCursorPos(&pt);
        {
            std::lock_guard lk(clickMtx_);
            cursorPos_ = pt;
            for (int b = 0; b < 2; ++b) {
                bool down = (GetAsyncKeyState((b == 0) != swap ? VK_LBUTTON : VK_RBUTTON) & 0x8000) != 0;
                if (down && !was[b] && !pausedAt_ && !OwnWindowAt(pt)) clicks_.push_back({now, 0, pt, b == 1});
                if (!down && was[b])
                    for (auto it = clicks_.rbegin(); it != clicks_.rend(); ++it)
                        if (it->right == (b == 1) && !it->up) {
                            it->up = now;
                            break;
                        }
                was[b] = down;
            }
            // Kept a second past the end: the video ticker may draw a little behind.
            while (!clicks_.empty() && clicks_.front().up &&
                   now - std::max(clicks_.front().down + kRipple, clicks_.front().up) > 10'000'000)
                clicks_.pop_front();
        }
        if (!opt_.video && (fxShown_ || ClicksShow(now))) {
            std::lock_guard lk(mtx_);
            EmitClickFrame(now);
        }
    }
}

bool Capture::ClicksShow(int64_t qpc) {
    std::lock_guard lk(clickMtx_);
    for (const Click& c : clicks_)
        if (qpc >= c.down && (qpc - c.down < kRipple || !c.up || qpc < c.up)) return true;
    return false;
}

void Capture::EmitClickFrame(int64_t qpc) {
    if (!running_ || !setupDone_ || firstTs_ < 0 || pausedAt_) return;
    int64_t ts = qpc - pausedTotal_;
    if (ts + interval_ / 4 < nextDue_) return;  // a frame went out a moment ago
    DeviceLock lock(mt_.Get());
    bool fx = DrawClicks(qpc);
    if (!fx && !fxShown_) return;
    fxShown_ = fx;  // after the last frame with a highlight, one without
    nextDue_ = ts + interval_;
    int64_t rel = std::max(ts - firstTs_, lastTs_ + 1);
    ctx_->CopyResource(staging_[ring_].Get(), fx ? fx_.Get() : out_.Get());
    pending_.push_back({ring_, rel});
    ring_ = (ring_ + 1) % kRing;
    while ((int)pending_.size() >= kRing - 1) DrainOne();
    lastTs_ = rel;
    lastArrivalQpc_ = ts;
    ++clickFrames_;
}

bool Capture::DrawClicks(int64_t qpc) {
    if (fxFailed_ || !out_) return false;
    std::vector<Click> show;
    POINT cur;
    {
        std::lock_guard lk(clickMtx_);
        cur = cursorPos_;
        for (const Click& c : clicks_)
            if (qpc >= c.down && (qpc - c.down < kRipple || !c.up || qpc < c.up)) show.push_back(c);
    }
    if (show.empty()) return false;
    if (!fx_) {
        D3D11_TEXTURE2D_DESC td;
        out_->GetDesc(&td);
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGISurface> surf;
        D2D1_FACTORY_OPTIONS fo{};
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        bool ok = SUCCEEDED(dev_->CreateTexture2D(&td, nullptr, &fx_)) && SUCCEEDED(fx_.As(&surf)) &&
                  SUCCEEDED(dev_.As(&dxgi)) &&
                  SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &fo,
                                              reinterpret_cast<void**>(d2d_.GetAddressOf()))) &&
                  SUCCEEDED(d2d_->CreateDevice(dxgi.Get(), &d2dDev_)) &&
                  SUCCEEDED(d2dDev_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dCtx_)) &&
                  SUCCEEDED(d2dCtx_->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &fxTarget_)) &&
                  SUCCEEDED(d2dCtx_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &fxBrush_));
        if (!ok) {
            fxFailed_ = true;  // records on without highlights
            fx_.Reset();
            return false;
        }
        d2dCtx_->SetTarget(fxTarget_.Get());
    }
    ctx_->CopyResource(fx_.Get(), out_.Get());
    const ClickMap& m = clickMap_;
    const float k = std::max(0.5f, m.fx * dpiScale_);  // sizes below are in DIPs of the recorded screen
    auto at = [&](POINT p) { return D2D1::Point2F(m.dx + (p.x - m.ox) * m.fx, m.dy + (p.y - m.oy) * m.fy); };
    d2dCtx_->BeginDraw();
    for (const Click& c : show) {
        const UINT32 rgb = c.right ? 0x60A5FA : 0xFBBF24;  // right: blue, left: amber
        double age = (double)(qpc - c.down) / kRipple;
        if (age < 1) {
            // A ring that grows from where it was pressed, strong at first, then fading out.
            float t = (float)age, e = 1 - (1 - t) * (1 - t) * (1 - t);
            float r = (12 + 22 * e) * k;
            D2D1_ELLIPSE el{at(c.at), r, r};
            fxBrush_->SetColor(D2D1::ColorF(rgb, 0.32f * (1 - t)));
            d2dCtx_->FillEllipse(el, fxBrush_.Get());
            fxBrush_->SetColor(D2D1::ColorF(rgb, 0.95f * (1 - t * t)));
            d2dCtx_->DrawEllipse(el, fxBrush_.Get(), 3.5f * k);
        }
        if (!c.up || qpc < c.up) {
            // Held: a dot that follows the cursor (drags).
            D2D1_ELLIPSE el{at(cur), 9 * k, 9 * k};
            fxBrush_->SetColor(D2D1::ColorF(rgb, 0.45f));
            d2dCtx_->FillEllipse(el, fxBrush_.Get());
            fxBrush_->SetColor(D2D1::ColorF(rgb, 0.9f));
            d2dCtx_->DrawEllipse(el, fxBrush_.Get(), 2 * k);
        }
    }
    if (FAILED(d2dCtx_->EndDraw())) {
        fxFailed_ = true;
        return false;
    }
    return true;
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
    // source is idle (Windows only delivers frames when something changes). Variable frame rate (#55) keeps the
    // ticks but encodes one only when the picture changed, or when a keyframe is due; the frame before lasts until it.
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
    const bool vfr = opt_.video->video.vfr;
    if (WaitForMultipleObjects(2, first, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        VideoConfig vc = opt_.video->video;
        vc.width = outW_;
        vc.height = outH_;
        vc.fps = opt_.fps;
        vc.fragmented = true;  // a crash leaves a file that plays; Finish makes it a plain MP4 (#24)
        std::wstring err;
        open = writer_->Open(opt_.video->path, vc, opt_.video->audio, dev_.Get(), err, opt_.video->ring);
        if (!open) Fail(err);
    }
    if (open) {
        const int fps = std::max(1, opt_.fps);
        // The ticks before the first frame (encoder start-up) are caught up in a burst showing that frame;
        // once running in real time, a stall skips ticks instead.
        // While paused no ticks are encoded; afterwards they continue where they stopped, later by the pause.
        bool synced = false;
        uint64_t checked = ~0ull;  // VFR: shots_ at the last tick
        bool fxChecked = false;    // and whether that tick had a click highlight
        int64_t end = 0;           // where the last tick ends, which the last frame lasts until
        RefusalWatch refusals;
        // An encoder pool that is full passes. A device that is gone, or an encoder that has stopped taking frames,
        // doesn't: REC would stay on for hours with nothing written (#69).
        auto given = [&](ID3D11Texture2D* tex, bool& giveUp) {
            ComPtr<IMFSample> sample;
            if (!refuse_) sample = writer_->Convert(tex);
            if (sample) {
                refusals.Taken();
            } else if (FAILED(dev_->GetDeviceRemovedReason()) || refusals.Refused(util::Qpc100ns())) {
                Fail(tr(L"그래픽 장치가 응답하지 않아 더 쓸 수 없었습니다. 그래픽 드라이버를 확인하거나 PC를 다시 시작해 "
                        L"보세요."));
                giveUp = true;
            }
            return sample;
        };
        for (int64_t k = 0;; ++k) {
            if (pausedAt_) {
                HANDLE waits[2] = {tickStop_, resumed_};
                if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) break;
            }
            int64_t due = startQpc_ + pausedTotal_ + k * 10'000'000 / fps;
            int64_t now = util::Qpc100ns();
            if (due > now) {
                synced = true;
                LARGE_INTEGER li;
                li.QuadPart = -(due - now);
                SetWaitableTimer(timer, &li, 0, nullptr, nullptr, FALSE);
                HANDLE waits[2] = {tickStop_, timer};
                if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) break;
                if (pausedAt_) {
                    --k;  // paused while waiting: this tick comes after the pause
                    continue;
                }
            } else if (WaitForSingleObject(tickStop_, 0) == WAIT_OBJECT_0) {
                break;
            } else if (synced) {
                int64_t cur = (now - startQpc_ - pausedTotal_) * fps / 10'000'000;
                if (cur > k + 2) k = cur;
            }
            const int64_t t0 = k * 10'000'000 / fps, t1 = (k + 1) * 10'000'000 / fps;
            // Never under mtx_: the frame callback may own the device lock while it waits for mtx_.
            ID3D11Texture2D* picture = out_.Get();
            bool maybe = false, keyDue = false;
            {
                DeviceLock lock(mt_.Get());
                if (finishDue_) FinishFrame();
                bool fx = false;
                if (opt_.clickFx && DrawClicks(due)) picture = fx_.Get(), fx = true;
                if (vfr) {
                    // A new capture, or a highlight drawn or just gone, may have changed the picture. It is taken as
                    // it is now (Windows can put a newer one in out_ at any time) and compared with the last frame.
                    maybe = shots_ != checked || fx || fxChecked;
                    checked = shots_;
                    fxChecked = fx;
                    keyDue = writer_->KeyDue(t0);
                    if (maybe || keyDue) picture = Snapshot(picture, maybe && !keyDue);
                }
            }
            if (vfr) {
                end = t1;
                if (!keyDue && !(maybe && Changed())) continue;  // the frame shown goes on
                bool giveUp = false;
                ComPtr<IMFSample> sample = given(picture, giveUp);
                if (giveUp) break;
                if (!sample) {  // encoder pool momentarily full: the next tick tries again
                    checked = ~0ull;
                    continue;
                }
                if (!writer_->HoldVideo(std::move(sample), t0)) {
                    Fail(tr(L"동영상 인코딩 중 오류가 발생했습니다. 디스크 공간을 확인하세요."));
                    break;
                }
                {
                    DeviceLock lock(mt_.Get());
                    Written();
                }
                ++frames_;
                continue;
            }
            bool giveUp = false;
            ComPtr<IMFSample> sample = given(picture, giveUp);
            if (giveUp) break;
            if (!sample) continue;  // encoder pool momentarily full: this tick is dropped
            if (!writer_->WriteVideo(sample.Get(), t0, t1 - t0)) {
                Fail(tr(L"동영상 인코딩 중 오류가 발생했습니다. 디스크 공간을 확인하세요."));
                break;
            }
            ++frames_;
        }
        // The last frame lasts until the stop press, also when the ticker was behind then.
        if (vfr && !failed_ && !writer_->EndVideo(std::max(end, stopAt_.load())))
            Fail(tr(L"동영상 인코딩 중 오류가 발생했습니다. 디스크 공간을 확인하세요."));
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
    const int64_t qpc = frame.SystemRelativeTime().count();
    int64_t ts = 0;
    bool accept = false, base = false;
    Admit(qpc, ts, accept, base);
    if (accept || base || pausedAt_) {
        try {
            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> tex;
            if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(&tex))))
                Take(tex.Get(), cs.Width, cs.Height, ts, qpc, accept, accept || base);
        } catch (...) {
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

void Capture::Admit(int64_t qpc, int64_t& ts, bool& accept, bool& base) {
    ts = qpc - pausedTotal_;  // the recording's own clock
    accept = false;
    if (pausedAt_) {
        // nothing while paused
    } else if (firstTs_ < 0) {
        accept = true;
        firstTs_ = ts;
        nextDue_ = ts + interval_;
    } else if (ts + interval_ / 4 >= nextDue_) {
        // Small tolerance so 60 Hz content sampled at 30 fps doesn't lose frames to jitter.
        accept = true;
        nextDue_ = (nextDue_ + interval_ > ts) ? nextDue_ + interval_ : ts + interval_;
    }
    // A clip frame that comes too soon still becomes the picture the click highlight frames are drawn on.
    base = !accept && !opt_.video && opt_.clickFx && !pausedAt_ && firstTs_ >= 0 && ClicksShow(qpc);
}

void Capture::Take(ID3D11Texture2D* tex, int cw, int ch, int64_t ts, int64_t qpc, bool accept, bool record) {
    RECT sr;
    if (!SourceRect(cw, ch, sr)) return;
    if (opt_.monitorMode)
        CoveredRects(sr, covered_);
    else
        covered_.clear();
    KeepShot(tex, sr, covered_);
    if (!record) return;  // paused: only for a screenshot
    if (opt_.clickFx) texOrigin_ = TextureOrigin(cw, ch);
    int64_t rel = ts - firstTs_;
    if (rel > 0 && rel <= lastTs_) rel = lastTs_ + 1;  // after a click highlight frame of ours
    Process(tex, sr, rel, covered_, qpc, accept);
    if (accept) {
        lastTs_ = rel;
        lastArrivalQpc_ = util::Qpc100ns() - pausedTotal_;  // on the recording's clock, like ts
    }
}

// Before anything is drawn on it or scaled: the cursor is there as recorded, the click highlights are not.
void Capture::KeepShot(ID3D11Texture2D* tex, const RECT& sr, const std::vector<RECT>& covered) {
    const UINT w = (UINT)(sr.right - sr.left), h = (UINT)(sr.bottom - sr.top);
    DeviceLock lock(mt_.Get());
    D3D11_TEXTURE2D_DESC td{};
    if (shot_) shot_->GetDesc(&td);
    if (!shot_ || td.Width != w || td.Height != h) {
        shot_.Reset();
        tex->GetDesc(&td);
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.SampleDesc = {1, 0};
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = 0;
        td.CPUAccessFlags = 0;
        td.MiscFlags = 0;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &shot_))) return;
    }
    const D3D11_BOX box{(UINT)sr.left, (UINT)sr.top, 0, (UINT)sr.right, (UINT)sr.bottom, 1};
    ctx_->CopySubresourceRegion(shot_.Get(), 0, 0, 0, 0, tex, 0, &box);
    shotMask_ = covered;
}

bool Capture::Shot(image::Picture& pic) {
    if (!running_) return false;
    DeviceLock lock(mt_.Get());
    if (!shot_) return false;
    D3D11_TEXTURE2D_DESC td{}, rd{};
    shot_->GetDesc(&td);
    if (shotRead_) shotRead_->GetDesc(&rd);
    if (!shotRead_ || rd.Width != td.Width || rd.Height != td.Height) {
        shotRead_.Reset();
        rd = td;
        rd.Usage = D3D11_USAGE_STAGING;
        rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev_->CreateTexture2D(&rd, nullptr, &shotRead_))) return false;
    }
    ctx_->CopyResource(shotRead_.Get(), shot_.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(shotRead_.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    pic.w = (int)td.Width;
    pic.h = (int)td.Height;
    pic.alpha = false;
    pic.px.resize((size_t)pic.w * pic.h * 4);
    for (int y = 0; y < pic.h; ++y)
        memcpy(&pic.px[(size_t)y * pic.w * 4], (const uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)pic.w * 4);
    ctx_->Unmap(shotRead_.Get(), 0);
    BlackOut(pic, shotMask_);
    return true;
}

void Capture::BlackOut(image::Picture& pic, const std::vector<RECT>& rects) {
    for (const RECT& r : rects) {
        const int l = std::clamp((int)r.left, 0, pic.w), right = std::clamp((int)r.right, l, pic.w);
        const int t = std::clamp((int)r.top, 0, pic.h), b = std::clamp((int)r.bottom, t, pic.h);
        for (int y = t; y < b; ++y) memset(&pic.px[((size_t)y * pic.w + l) * 4], 0, (size_t)(right - l) * 4);
    }
}

// ---------------------------------------------------------------- desktop duplication

bool Capture::Duplicate() {
    dup_.Reset();
    MONITORINFO mi{sizeof(mi)};
    if (!dupMonitor_ || !GetMonitorInfoW(dupMonitor_, &mi)) return false;
    // The monitor's output on the device's adapter: one another GPU drives can't be duplicated with this device.
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIOutput> output;
    if (FAILED(dev_.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter))) return false;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIOutput> o;
        if (FAILED(adapter->EnumOutputs(i, &o))) break;
        DXGI_OUTPUT_DESC od{};
        if (SUCCEEDED(o->GetDesc(&od)) && od.Monitor == dupMonitor_) {
            output = o;
            break;
        }
    }
    if (!output) return false;
    ComPtr<IDXGIOutput5> o5;
    ComPtr<IDXGIOutput1> o1;
    HRESULT hr = E_NOINTERFACE;
    if (SUCCEEDED(output.As(&o5))) {
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};  // an HDR screen comes as SDR, like the pool's
        hr = o5->DuplicateOutput1(dev_.Get(), 0, _countof(formats), formats, &dup_);
    }
    if (FAILED(hr) && SUCCEEDED(output.As(&o1))) hr = o1->DuplicateOutput(dev_.Get(), &dup_);
    if (FAILED(hr)) {
        dup_.Reset();
        return false;
    }
    DXGI_OUTDUPL_DESC dd{};
    dup_->GetDesc(&dd);
    const bool upright = dd.Rotation == DXGI_MODE_ROTATION_IDENTITY || dd.Rotation == DXGI_MODE_ROTATION_UNSPECIFIED;
    if (!upright || dd.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || dd.ModeDesc.Width < 2 || dd.ModeDesc.Height < 2) {
        dup_.Reset();
        return false;
    }
    monitorRect_ = mi.rcMonitor;
    if (!desk_ || deskW_ != (int)dd.ModeDesc.Width || deskH_ != (int)dd.ModeDesc.Height) {
        desk_.Reset();
        deskCur_.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = dd.ModeDesc.Width;
        td.Height = dd.ModeDesc.Height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &desk_))) {
            dup_.Reset();
            return false;
        }
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
        if (opt_.cursor && FAILED(dev_->CreateTexture2D(&td, nullptr, &deskCur_))) deskCur_.Reset();  // no cursor then
        deskW_ = td.Width;
        deskH_ = td.Height;
    }
    haveDesk_ = false;
    return true;
}

void Capture::DupLoop() {
    bool dirty = false;    // desk_ (or the cursor over it) changed since it was last handed on
    int64_t retryIn = -1;  // too soon: when it is due
    bool gone = false;
    while (WaitForSingleObject(dupStop_, 0) == WAIT_TIMEOUT) {
        if (!dup_) {
            // Lost: a mode change, the secure desktop (UAC, the lock screen), a fullscreen switch. The last
            // picture stays until the screen can be duplicated again.
            if (!Duplicate()) {
                MONITORINFO mi{sizeof(mi)};
                if (!gone && !GetMonitorInfoW(dupMonitor_, &mi)) {
                    gone = true;  // unplugged: like Windows.Graphics.Capture's Closed
                    if (notify_) PostMessageW(notify_, closedMsg_, 0, tag_);
                }
                WaitForSingleObject(dupStop_, 250);
                continue;
            }
        }
        UINT wait = 50;
        if (dirty && retryIn >= 0) wait = (UINT)std::clamp<int64_t>(retryIn / 10'000, 0, 50);
        DXGI_OUTDUPL_FRAME_INFO fi{};
        ComPtr<IDXGIResource> res;
        HRESULT hr = dup_->AcquireNextFrame(wait, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            // nothing new
        } else if (FAILED(hr)) {
            dup_.Reset();
            continue;
        } else {
            ComPtr<ID3D11Texture2D> tex;
            if ((fi.LastPresentTime.QuadPart != 0 || !haveDesk_) && running_ && SUCCEEDED(res.As(&tex))) {
                D3D11_TEXTURE2D_DESC td{};
                tex->GetDesc(&td);
                if ((int)td.Width == deskW_ && (int)td.Height == deskH_ && td.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
                    DeviceLock lock(mt_.Get());
                    ctx_->CopyResource(desk_.Get(), tex.Get());
                    haveDesk_ = dirty = true;
                }
            }
            if (fi.LastMouseUpdateTime.QuadPart != 0 && deskCur_ && haveDesk_) dirty = true;  // the cursor moved
            dup_->ReleaseFrame();
        }
        if (dirty) dirty = !PushDesktop(retryIn);
    }
}

bool Capture::PushDesktop(int64_t& retryIn) {
    std::lock_guard lk(mtx_);
    retryIn = -1;
    if (!running_) return true;
    if (pausedAt_) {  // nothing while paused but what a screenshot sees: the next change after it comes through
        DeviceLock lock(mt_.Get());
        Take(WithCursor(), deskW_, deskH_, 0, 0, false, false);
        return true;
    }
    const int64_t qpc = util::Qpc100ns();
    int64_t ts = 0;
    bool accept = false, base = false;
    Admit(qpc, ts, accept, base);
    if (accept || base) {
        DeviceLock lock(mt_.Get());
        Take(WithCursor(), deskW_, deskH_, ts, qpc, accept);
    }
    if (accept) return true;
    retryIn = std::max<int64_t>(0, nextDue_ - interval_ / 4 - ts);
    return false;
}

namespace {
// The cursor as Windows shows it, through GDI (also the inverting kinds, like the text cursor) onto a GDI-compatible
// texture. Under the device lock.
bool DrawCursorOn(ID3D11Texture2D* tex, HCURSOR cursor, int x, int y) {
    ComPtr<IDXGISurface1> s;
    HDC dc = nullptr;
    if (FAILED(tex->QueryInterface(IID_PPV_ARGS(&s))) || FAILED(s->GetDC(FALSE, &dc))) return false;
    const bool ok = DrawIconEx(dc, x, y, cursor, 0, 0, 0, nullptr, DI_NORMAL) != 0;
    s->ReleaseDC(nullptr);
    return ok;
}

bool CursorShape(HCURSOR cursor, POINT& hotspot, SIZE& size) {
    ICONINFO ii{};
    if (!GetIconInfo(cursor, &ii)) return false;
    hotspot = {(LONG)ii.xHotspot, (LONG)ii.yHotspot};
    BITMAP bm{};
    GetObjectW(ii.hbmMask, sizeof(bm), &bm);
    size = {bm.bmWidth, ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2};  // a black-and-white one: AND over XOR
    DeleteObject(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    return true;
}
}  // namespace

ID3D11Texture2D* Capture::WithCursor() {
    if (!deskCur_) return desk_.Get();
    CURSORINFO ci{sizeof(ci)};
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return desk_.Get();  // a game hides it
    if (ci.hCursor != cursor_) {
        if (!CursorShape(ci.hCursor, hotspot_, cursorSize_)) return desk_.Get();
        cursor_ = ci.hCursor;
    }
    const int x = ci.ptScreenPos.x - monitorRect_.left - hotspot_.x, y = ci.ptScreenPos.y - monitorRect_.top - hotspot_.y;
    if (x >= deskW_ || y >= deskH_ || x + cursorSize_.cx <= 0 || y + cursorSize_.cy <= 0) return desk_.Get();
    ctx_->CopyResource(deskCur_.Get(), desk_.Get());
    return DrawCursorOn(deskCur_.Get(), ci.hCursor, x, y) ? deskCur_.Get() : desk_.Get();
}

bool Capture::CursorDrawCheck(std::wstring& detail) {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr,
                                 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        detail = L"no device";
        return false;
    }
    constexpr int kW = 64, kH = 64;
    std::vector<uint32_t> grey(kW * kH, 0xFF505A64u);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kW;
    td.Height = kH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
    D3D11_SUBRESOURCE_DATA init{grey.data(), kW * 4, 0};
    ComPtr<ID3D11Texture2D> tex, staging;
    if (FAILED(dev->CreateTexture2D(&td, &init, &tex))) {
        detail = L"no GDI-compatible texture";
        return false;
    }
    // The arrow and the text cursor (drawn by inverting what is under it), side by side.
    HCURSOR arrow = LoadCursorW(nullptr, IDC_ARROW), beam = LoadCursorW(nullptr, IDC_IBEAM);
    POINT hs{};
    SIZE sz{};
    const bool drawn = CursorShape(arrow, hs, sz) && DrawCursorOn(tex.Get(), arrow, 2, 2) && DrawCursorOn(tex.Get(), beam, 34, 2);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.MiscFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (!drawn || FAILED(dev->CreateTexture2D(&td, nullptr, &staging))) {
        detail = L"not drawn";
        return false;
    }
    ctx->CopyResource(staging.Get(), tex.Get());
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        detail = L"no readback";
        return false;
    }
    int dark[2] = {}, light[2] = {}, changed[2] = {};
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const uint32_t p = static_cast<const uint32_t*>(m.pData)[(size_t)y * m.RowPitch / 4 + x] & 0xFFFFFFu;
            const int side = x < 32 ? 0 : 1, lum = ((p >> 16 & 255) + (p >> 8 & 255) + (p & 255)) / 3;
            if (p == 0x505A64u) continue;
            ++changed[side];
            dark[side] += lum < 40;
            light[side] += lum > 150;
        }
    ctx->Unmap(staging.Get(), 0);
    detail = util::Format(L"arrow %ld×%ld hotspot %ld,%ld: %d px (%d dark, %d light), text cursor %d px", sz.cx, sz.cy,
                          hs.x, hs.y, changed[0], dark[0], light[0], changed[1]);
    // The arrow is white with a black outline; the text cursor turns the grey under it into its inverse.
    return dark[0] > 10 && light[0] > 10 && changed[1] > 10;
}

CaptureResult Capture::Stop() {
    CaptureResult res;
    if (!running_.exchange(false)) return res;
    StopClicks();
    // Stopped while paused: the recording ends where the pause began.
    int64_t stopQpc = pausedAt_ ? pausedAt_.load() : util::Qpc100ns();
    try {
        if (pool_) pool_.FrameArrived(frameToken_);
        if (item_) item_.Closed(closedToken_);
        if (session_) session_.Close();
        if (pool_) pool_.Close();
    } catch (...) {
    }
    if (dupThread_.joinable()) {
        SetEvent(dupStop_);
        dupThread_.join();
        CloseHandle(dupStop_);
        dupStop_ = nullptr;
    }
    if (opt_.video) {
        stopAt_ = stopQpc - startQpc_ - pausedTotal_;
        SetEvent(tickStop_);
        if (ticker_.joinable()) ticker_.join();
        audio_.Stop(stopQpc);  // pads silence up to the stop press
        mic_.Stop(stopQpc);
        mix_.Flush();
        { std::lock_guard lk(mtx_); }  // lets a frame callback that is still running finish
        res.video = true;
        res.path = opt_.video->path;
        res.notice = notice_;
        res.seconds = (stopQpc - startQpc_ - pausedTotal_) / 1e7;
        // An error mid-recording keeps what was encoded before it.
        std::wstring finishErr;
        if (refuseFinish_) writer_->RefuseFinish();
        res.ok = writer_->Finish(finishErr);
        if (res.ok && !writer_->Notice().empty())
            res.notice = res.notice.empty() ? writer_->Notice() : res.notice + L" " + writer_->Notice();
        if (res.ok && !videoError_.empty()) {
            res.seconds = writer_->VideoEnd() / 1e7;
            res.failure = videoError_;
        }
        res.error = res.ok ? std::wstring() : !videoError_.empty() ? videoError_ : finishErr;
        res.encoder = writer_->EncoderName();
        res.hardware = writer_->HardwareEncoder();
        res.audio = res.ok && writer_->HasAudio();
        if (res.ok) res.bytes = util::FileSize(res.path);
        writer_.reset();
        CloseHandle(tickStop_);
        CloseHandle(firstFrame_);
        CloseHandle(resumed_);
        tickStop_ = firstFrame_ = resumed_ = nullptr;
    } else {
        std::lock_guard lk(mtx_);
        while (!pending_.empty()) DrainOne();
        if (store_) {
            // Last frame stays on screen until the moment Stop was pressed.
            int64_t endTs = lastTs_ + std::max<int64_t>(0, stopQpc - pausedTotal_ - lastArrivalQpc_);
            store_->Finish(endTs);
            if (store_->Count() > 0) res.clip = store_;
            store_.reset();
        }
    }
    ReleaseGpu();
    return res;
}

void Capture::ReleaseGpu() {
    fxBrush_.Reset();
    fxTarget_.Reset();
    d2dCtx_.Reset();
    d2dDev_.Reset();
    d2d_.Reset();
    fx_.Reset();
    session_ = nullptr;
    pool_ = nullptr;
    item_ = nullptr;
    rtDevice_ = nullptr;
    dup_.Reset();
    desk_.Reset();
    deskCur_.Reset();
    deskW_ = deskH_ = 0;
    haveDesk_ = false;
    cursor_ = nullptr;
    for (auto& s : staging_) s.Reset();
    pending_.clear();
    out_.Reset();
    outRtv_.Reset();
    clean_.Reset();
    haveClean_ = false;
    mip_.Reset();
    mipSrv_.Reset();
    mipW_ = mipH_ = mipLevels_ = 0;
    cb_.Reset();
    sampler_.Reset();
    ps_.Reset();
    vs_.Reset();
    shot_.Reset();
    shotRead_.Reset();
    if (ctx_) ctx_->ClearState();
    mt_.Reset();
    ctx1_.Reset();
    ctx_.Reset();
    dev_.Reset();
}
