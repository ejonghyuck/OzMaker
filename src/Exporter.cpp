#include "pch.h"
#include "Exporter.h"
#include "Resize.h"
#include "Util.h"
#include "VideoClip.h"

std::vector<PlannedFrame> PlanFrames(const Clip& s, size_t first, size_t last, double fpsLimit, double speed) {
    std::vector<PlannedFrame> out;
    size_t count = s.Count();
    if (count == 0 || first > last || first >= count) return out;
    last = std::min(last, count - 1);
    int64_t rangeEnd = last + 1 < count ? s.Ts(last + 1) : s.EndTs();
    int64_t interval = fpsLimit > 0 ? (int64_t)(1e7 / fpsLimit) : 0;
    int64_t nextDue = 0;
    std::vector<size_t> sel;
    for (size_t i = first; i <= last; ++i) {
        int64_t t = s.Ts(i);
        if (sel.empty() || interval == 0 || t + interval / 4 >= nextDue) {
            sel.push_back(i);
            nextDue = (sel.size() > 1 && nextDue + interval > t) ? nextDue + interval : t + interval;
        }
    }
    speed = speed > 0 ? speed : 1.0;
    out.reserve(sel.size());
    for (size_t k = 0; k < sel.size(); ++k) {
        int64_t end = k + 1 < sel.size() ? s.Ts(sel[k + 1]) : rangeEnd;
        double ms = std::max<int64_t>(0, end - s.Ts(sel[k])) / 1e4 / speed;
        out.push_back({sel[k], ms});
    }
    return out;
}

std::vector<size_t> PlanVideoFrames(const Clip& s, size_t first, size_t last, double fps, double speed) {
    std::vector<size_t> out;
    size_t count = s.Count();
    if (count == 0 || first > last || first >= count || fps <= 0) return out;
    last = std::min(last, count - 1);
    speed = speed > 0 ? speed : 1.0;
    int64_t start = s.Ts(first), end = last + 1 < count ? s.Ts(last + 1) : s.EndTs();
    size_t n = std::max<size_t>(1, (size_t)std::llround((end - start) / 1e7 / speed * fps));
    double step = speed * 1e7 / fps;    // source time per output frame
    int64_t tol = (int64_t)(step / 8);  // absorbs timestamp rounding so equal rates map 1:1
    out.reserve(n);
    for (size_t k = 0; k < n; ++k) {
        int64_t t = start + (int64_t)std::llround(k * step);
        out.push_back(std::clamp(s.IndexAt(t + tol), first, last));
    }
    return out;
}

Exporter::~Exporter() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
}

void Exporter::Start(ExportJob job, HWND notify, UINT doneMsg) {
    if (thread_.joinable()) thread_.join();
    notify_ = notify;
    doneMsg_ = doneMsg;
    cancel_ = false;
    progress_ = 0;
    busy_ = true;
    result_ = {};
    thread_ = std::thread([this, job = std::move(job)]() mutable { Run(std::move(job)); });
}

ExportResult Exporter::Take() {
    if (thread_.joinable()) thread_.join();
    return result_;
}

void Exporter::Run(ExportJob job) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    ExportResult res;
    res.path = job.path;
    std::wstring tmp = job.path + L".part";
    bool ok = job.format == Format::Mp4 ? RunMp4(job, tmp, res) : RunImage(job, tmp, res);
    if (ok) {
        ok = MoveFileExW(tmp.c_str(), job.path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
        if (!ok) res.error = L"파일을 저장할 수 없습니다: " + job.path;
    }
    res.ok = ok;
    if (!ok) DeleteFileW(tmp.c_str());
    if (ok) res.bytes = util::FileSize(job.path);
    progress_ = 1.f;
    result_ = res;
    busy_ = false;
    if (notify_) PostMessageW(notify_, doneMsg_, 0, 0);
}

bool Exporter::RunImage(const ExportJob& job, const std::wstring& tmp, ExportResult& res) {
    auto plan = PlanFrames(*job.clip, job.first, job.last, job.fpsLimit, job.speed);
    auto enc = CreateEncoder(job.format);
    if (plan.empty()) {
        res.error = L"내보낼 프레임이 없습니다.";
        return false;
    }
    if (!enc) {
        res.error = std::wstring(FormatName(job.format)) + L" 인코더가 이 빌드에 포함되어 있지 않습니다.";
        return false;
    }
    EncoderParams p;
    p.width = job.outW;
    p.height = job.outH;
    p.quality = job.quality;
    p.loop = job.loop;
    p.path = tmp;
    if (!enc->Begin(p, res.error)) return false;
    auto reader = job.clip->NewReader();
    AreaResizer resizer;
    std::vector<uint8_t> frame((size_t)job.outW * job.outH * 4);
    int cx = job.crop.left, cy = job.crop.top;
    int cw = job.crop.right - job.crop.left, ch = job.crop.bottom - job.crop.top;
    bool ok = true;
    for (size_t k = 0; k < plan.size() && ok; ++k) {
        if (cancel_) {
            res.cancelled = true;
            ok = false;
            break;
        }
        const uint8_t* px = reader->Get(plan[k].index);
        if (!px) {
            res.error = L"녹화 데이터를 읽을 수 없습니다.";
            ok = false;
            break;
        }
        resizer.Run(px, job.clip->Width() * 4, cx, cy, cw, ch, frame.data(), job.outW, job.outH);
        ok = enc->Add(frame.data(), plan[k].durationMs, res.error);
        res.seconds += plan[k].durationMs / 1000.0;
        progress_ = 0.97f * (float)(k + 1) / plan.size();
    }
    if (ok) ok = enc->End(res.error);
    enc.reset();  // closes any open file handle before the rename
    res.frames = (int)plan.size();
    return ok;
}

// Re-encodes the range at a constant frame rate: frames are cropped/scaled on the CPU (same path as the image
// formats), uploaded, then converted to NV12 and encoded on the GPU. The clip's sound is cut to the same range.
bool Exporter::RunMp4(const ExportJob& job, const std::wstring& tmp, ExportResult& res) {
    const double rate = job.fpsLimit > 0 ? job.fpsLimit : job.clip->AverageFps();
    const int fps = std::clamp((int)std::lround(rate), 1, 120);
    auto plan = PlanVideoFrames(*job.clip, job.first, job.last, fps, job.speed);
    if (plan.empty()) {
        res.error = L"내보낼 프레임이 없습니다.";
        return false;
    }
    const int W = job.outW & ~1, H = job.outH & ~1;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Multithread> mt;
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                                 &dev, nullptr, &ctx)) ||
        FAILED(ctx.As(&mt))) {
        res.error = L"동영상 인코딩에 쓸 그래픽 장치를 만들 수 없습니다.";
        return false;
    }
    mt->SetMultithreadProtected(TRUE);  // the encoder shares the device
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W;
    td.Height = H;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex))) {
        res.error = L"동영상 인코딩에 쓸 그래픽 메모리가 부족합니다.";
        return false;
    }

    VideoConfig v = job.video;
    v.width = W;
    v.height = H;
    v.fps = fps;
    AudioConfig a = job.audio;
    VideoClip::AudioReader audio;
    auto* vc = dynamic_cast<const VideoClip*>(job.clip.get());
    a.enabled = a.enabled && vc && vc->HasAudio() && std::fabs(job.speed - 1.0) < 1e-6 &&
                audio.Open(*vc, job.clip->Ts(job.first)) && (audio.Rate() == 44100 || audio.Rate() == 48000);
    if (a.enabled) a.sampleRate = audio.Rate();

    Mp4Writer w;
    if (!w.Open(tmp, v, a, dev.Get(), res.error)) return false;
    res.audio = w.HasAudio();
    auto reader = job.clip->NewReader();
    AreaResizer resizer;
    std::vector<uint8_t> frame((size_t)W * H * 4);
    std::vector<int16_t> pcm;
    int64_t audioPushed = 0;
    int cx = job.crop.left, cy = job.crop.top;
    int cw = job.crop.right - job.crop.left, ch = job.crop.bottom - job.crop.top;
    size_t uploaded = SIZE_MAX;
    for (size_t k = 0; k < plan.size(); ++k) {
        if (cancel_) {
            res.cancelled = true;
            w.Abort();
            return false;
        }
        if (plan[k] != uploaded) {
            const uint8_t* px = reader->Get(plan[k]);
            if (!px) {
                res.error = L"영상 프레임을 읽을 수 없습니다.";
                w.Abort();
                return false;
            }
            resizer.Run(px, job.clip->Width() * 4, cx, cy, cw, ch, frame.data(), W, H);
            ctx->UpdateSubresource(tex.Get(), 0, nullptr, frame.data(), W * 4, 0);
            uploaded = plan[k];
        }
        // The sample pool runs dry while the encoder is behind: wait for it.
        ComPtr<IMFSample> s = w.Convert(tex.Get());
        for (int tries = 0; !s && !w.Failed() && tries < 2500; ++tries) {
            Sleep(2);
            s = w.Convert(tex.Get());
        }
        int64_t t0 = (int64_t)k * 10'000'000 / fps, t1 = (int64_t)(k + 1) * 10'000'000 / fps;
        if (res.audio) {
            int64_t want = t1 * a.sampleRate / 10'000'000;
            if (want > audioPushed) {
                size_t n = (size_t)(want - audioPushed);
                pcm.resize(n * 2);
                audio.Read(pcm.data(), n);
                w.PushAudio(pcm.data(), n);
                audioPushed = want;
            }
        }
        if (!s || !w.WriteVideo(s.Get(), t0, t1 - t0)) {
            res.error = L"동영상 인코더가 프레임을 받지 못했습니다.";
            w.Abort();
            return false;
        }
        progress_ = 0.98f * (float)(k + 1) / plan.size();
    }
    if (!w.Finish(res.error)) return false;
    res.frames = (int)plan.size();
    res.seconds = plan.size() / (double)fps;
    return true;
}
