// Headless test entry points (command line):
//   --selftest <dir>                      synthetic clip -> frame store round trip -> every available encoder
//   --screenshot <dir>                    renders each screen to PNG for visual review
//   --record-test <title> <sec> <dir> [fps] [maxEdge] [window|display|monitor|region] [clip|mp4] [audio|mute]
//                                         records a real window (or its monitor / its area) and exports it
//   --analyze <file.mp4> <dir> [freq ...] decodes an MP4: timing, size, test-tone levels
//   --editor-test <file.mp4> <dir>        the real window: saved video -> editor -> keys -> trimmed export
//   --video-test <dir> [w h]              synthetic MP4 -> frame index/decoding -> trimmed MP4/GIF exports, checked
//                                         frame by frame and by the audio tone boundaries; editor screenshots
#include "pch.h"
#include "App.h"
#include "Resize.h"
#include "Util.h"
#include "VideoClip.h"

namespace {

// The MP4 colour conversion runs on the GPU's video processor, which machines without a GPU (CI runners) lack.
bool HasVideoProcessor() {
    ComPtr<ID3D11Device> dev;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &dev, nullptr, nullptr)))
        return false;
    ComPtr<ID3D11VideoDevice> vd;
    return SUCCEEDED(dev.As(&vd));
}

// Sum of a GIF's frame delays in 1/100 s, read from its graphic control extensions; -1 if unreadable.
int GifDurationCs(const std::wstring& path) {
    std::vector<uint8_t> d;
    if (FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO)) {
        uint8_t buf[65536];
        for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) d.insert(d.end(), buf, buf + n);
        fclose(f);
    }
    if (d.size() < 13) return -1;
    size_t p = 13;
    if (d[10] & 0x80) p += 3u << ((d[10] & 7) + 1);
    auto skipSubBlocks = [&] {
        while (p < d.size() && d[p]) p += d[p] + 1;
        ++p;
    };
    int total = 0;
    while (p < d.size()) {
        uint8_t b = d[p++];
        if (b == 0x3B) return total;
        if (b == 0x21 && p < d.size()) {
            uint8_t label = d[p++];
            if (label == 0xF9 && p + 4 < d.size()) total += d[p + 2] | (d[p + 3] << 8);
            skipSubBlocks();
        } else if (b == 0x2C && p + 9 <= d.size()) {
            uint8_t flags = d[p + 8];
            p += 9;
            if (flags & 0x80) p += 3u << ((flags & 7) + 1);
            ++p;  // LZW minimum code size
            skipSubBlocks();
        } else {
            return -1;
        }
    }
    return -1;
}

class Log {
public:
    explicit Log(const std::wstring& path) { f_ = _wfsopen(path.c_str(), L"wb", _SH_DENYNO); }
    ~Log() {
        if (f_) fclose(f_);
    }
    void operator()(const std::wstring& s) {
        if (!f_) return;
        std::string u = util::WideToUtf8(s) + "\n";
        fwrite(u.data(), 1, u.size(), f_);
        fflush(f_);
    }

private:
    FILE* f_ = nullptr;
};

void DrawSynthetic(uint8_t* p, int w, int h, double t) {
    double bx = w * (0.5 + 0.35 * std::sin(t * 3.14159)), by = h * (0.5 + 0.25 * std::cos(t * 2.1));
    double br = h / 6.0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* q = p + ((size_t)y * w + x) * 4;
            int r = x * 255 / w, g = y * 255 / h, b = 140;
            if (y > h * 3 / 4 && (x / 6) % 2 == 0) r = g = b = 230;  // static "text" stripes
            double d = std::hypot(x - bx, y - by);
            if (d < br) {
                double s = 0.6 + 0.4 * (1 - d / br);
                r = (int)(255 * s);
                g = (int)(200 * s);
                b = (int)(60 * s);
            }
            q[0] = (uint8_t)b;
            q[1] = (uint8_t)g;
            q[2] = (uint8_t)r;
            q[3] = 255;
        }
}

bool SavePng(IWICImagingFactory* wic, const std::wstring& path, const uint8_t* bgra, int w, int h) {
    ComPtr<IWICBitmap> bmp;
    if (FAILED(wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA, w * 4, w * h * 4,
                                           const_cast<BYTE*>(bgra), &bmp)))
        return false;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(&frame, nullptr)) ||
        FAILED(frame->Initialize(nullptr)))
        return false;
    frame->SetSize(w, h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);
    return SUCCEEDED(frame->WriteSource(bmp.Get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
           SUCCEEDED(enc->Commit());
}

ExportResult ExportSync(Exporter& ex, ExportJob job) {
    ex.Start(std::move(job), nullptr, 0);
    return ex.Take();
}

}  // namespace

std::shared_ptr<FrameStore> App::MakeSyntheticClip(int w, int h, int frames, double fps) {
    auto s = std::make_shared<FrameStore>();
    std::wstring err;
    if (!s->Open(w, h, err)) return nullptr;
    for (int i = 0; i < frames; ++i) {
        auto buf = s->Acquire();
        DrawSynthetic(buf->data(), w, h, i / fps);
        while (!s->Submit(buf, (int64_t)(i * 1e7 / fps))) Sleep(1);
    }
    s->Finish((int64_t)(frames * 1e7 / fps));
    return s;
}

int App::RunSelfTest(const std::wstring& dir) {
    util::EnsureDir(dir);
    Log log(dir + L"\\selftest.log");
    int failures = 0;
    const int W = 480, H = 270, N = 90;
    const double fps = 30;

    double t0 = util::NowSeconds();
    auto clip = MakeSyntheticClip(W, H, N, fps);
    if (!clip) {
        log(L"FAIL: frame store open");
        return 1;
    }
    log(util::Format(L"store: %zu frames, %.2fs, disk %s, dropped %u, build %.0f ms", clip->Count(), clip->Seconds(),
                     util::FormatBytes(clip->DiskBytes()).c_str(), clip->Dropped(), (util::NowSeconds() - t0) * 1000));

    // Round trip in a seek-heavy order (crosses keyframes both ways).
    {
        FrameStore::Reader rd(clip.get());
        std::vector<uint8_t> ref((size_t)W * H * 4);
        size_t order[] = {0, 45, 44, 89, 10, 31, 30, 29, 60, 61};
        for (size_t i : order) {
            DrawSynthetic(ref.data(), W, H, i / fps);
            const uint8_t* px = rd.Get(i);
            bool same = px && memcmp(px, ref.data(), ref.size()) == 0;
            if (!same) ++failures;
            log(util::Format(L"roundtrip frame %zu: %s", i, same ? L"ok" : L"MISMATCH"));
        }
    }

    // PlanFrames sanity.
    auto plan = PlanFrames(*clip, 0, N - 1, 0, 1.0);
    double total = 0;
    for (auto& p : plan) total += p.durationMs;
    log(util::Format(L"plan all: %zu frames, %.1f ms", plan.size(), total));
    if (plan.size() != (size_t)N || std::fabs(total - N * 1000 / fps) > 1) ++failures;
    auto plan15 = PlanFrames(*clip, 0, N - 1, 15, 1.0);
    log(util::Format(L"plan 15fps: %zu frames", plan15.size()));
    if (plan15.size() < 44 || plan15.size() > 46) ++failures;
    // The limit counts output frames: 30 fps at 2× would play at 60 fps, so a 50 fps cap keeps 50 a second
    // of the 1.5 s result and the duration stays half the source.
    auto plan2x = PlanFrames(*clip, 0, N - 1, 50, 2.0);
    double total2x = 0;
    for (auto& p : plan2x) total2x += p.durationMs;
    log(util::Format(L"plan 2x capped at 50fps: %zu frames, %.1f ms", plan2x.size(), total2x));
    if (plan2x.size() < 73 || plan2x.size() > 77 || std::fabs(total2x - N * 500 / fps) > 1) ++failures;

    Exporter ex;
    for (int f = 0; f < (int)Format::Count; ++f) {
        Format fmt = (Format)f;
        if (!FormatAvailable(fmt)) {
            log(std::wstring(FormatName(fmt)) + L": not built");
            continue;
        }
        if (fmt == Format::Mp4 && !HasVideoProcessor()) {
            log(L"MP4: skipped (no GPU video processor)");
            continue;
        }
        ExportJob job;
        job.clip = clip;
        job.first = 0;
        job.last = N - 1;
        job.crop = {0, 0, W, H};
        job.outW = W;
        job.outH = H;
        job.format = fmt;
        job.quality = f < (int)Format::Mp4 ? settings_.quality[f] : 0;
        if (fmt == Format::Mp4) job.fpsLimit = fps;
        job.path = dir + L"\\selftest." + FormatExt(fmt);
        double s = util::NowSeconds();
        ExportResult r = ExportSync(ex, job);
        log(util::Format(L"%s: %s %s, %d frames, %.2fs, %.0f ms %s", FormatName(fmt), r.ok ? L"ok" : L"FAIL",
                         util::FormatBytes(r.bytes).c_str(), r.frames, r.seconds, (util::NowSeconds() - s) * 1000,
                         r.error.c_str()));
        if (!r.ok) ++failures;
    }
    // Cropped, scaled, rate-limited, sped-up GIF.
    {
        ExportJob job;
        job.clip = clip;
        job.first = 10;
        job.last = 70;
        job.crop = {40, 20, 440, 245};
        job.outW = 200;
        job.outH = 112;
        job.fpsLimit = 15;
        job.speed = 2.0;
        job.format = Format::Gif;
        job.quality = 70;
        job.path = dir + L"\\selftest_small.gif";
        ExportResult r = ExportSync(ex, job);
        int cs = GifDurationCs(job.path);
        log(util::Format(L"GIF small: %s %s, %d frames, %.2fs (file plays %.2fs) %s", r.ok ? L"ok" : L"FAIL",
                         util::FormatBytes(r.bytes).c_str(), r.frames, r.seconds, cs / 100.0, r.error.c_str()));
        // 61 source frames (2.03 s) at 2× and 15 output fps: about 15 frames over 1.02 s.
        if (!r.ok || r.frames < 14 || r.frames > 17 || std::fabs(cs / 100.0 - 61 / fps / 2) > 0.03) ++failures;
    }
    // 2× with no limit: the 50 fps cap of GIF delays must not stretch the result back to 1×.
    {
        ExportJob job;
        job.clip = clip;
        job.first = 0;
        job.last = N - 1;
        job.crop = {0, 0, W, H};
        job.outW = W / 2;
        job.outH = H / 2;
        job.fpsLimit = FormatMaxFps(Format::Gif);
        job.speed = 2.0;
        job.format = Format::Gif;
        job.quality = 70;
        job.path = dir + L"\\selftest_2x.gif";
        ExportResult r = ExportSync(ex, job);
        int cs = GifDurationCs(job.path);
        log(util::Format(L"GIF 2x: %s %d frames, file plays %.2fs %s", r.ok ? L"ok" : L"FAIL", r.frames, cs / 100.0,
                         r.error.c_str()));
        if (!r.ok || std::fabs(cs / 100.0 - N / fps / 2) > 0.03) ++failures;
    }
    log(failures ? util::Format(L"RESULT: %d failure(s)", failures) : L"RESULT: all passed");
    return failures ? 2 : 0;
}

int App::RunScreenshots(const std::wstring& dir) {
    testWindowsOnly_ = true;
    util::EnsureDir(dir);
    const float W = 1120, H = 780;
    double clock = util::NowSeconds();
    auto shot = [&](const std::wstring& name, float dpi) {
        dpi_ = (UINT)dpi;
        UINT pw = (UINT)(W * dpi / 96), ph = (UINT)(H * dpi / 96);
        // Several passes on a simulated clock so animated values settle on their targets.
        for (int pass = 0; pass < 10; ++pass) {
            clock += 0.05;
            gfx_.RenderToPng((dir + L"\\" + name).c_str(), pw, ph, dpi, [&](ID2D1DeviceContext* dc) {
                UiInput in;
                ui_.Begin(dc, in, clock);
                thumbWanted_ = false;
                DrawFrame(Rect(0, 0, W, H));
                ui_.End();
            });
        }
    };

    RefreshWindows();
    target_ = nullptr;
    screen_ = Screen::Home;
    settings_.source = kSourceWindow;
    settings_.output = kOutputClip;
    shot(L"home_empty.png", 96);
    if (!windows_.empty()) target_ = windows_.front().hwnd;
    shot(L"home_selected.png", 96);
    shot(L"home_selected_150.png", 144);
    settings_.output = kOutputVideo;
    shot(L"home_video.png", 96);
    lastVideo_ = {};
    lastVideo_.video = lastVideo_.ok = lastVideo_.audio = true;
    lastVideo_.path = settings_.outDir + L"\\OzMaker_20261102_142311.mp4";
    lastVideo_.bytes = 48'213'004;
    lastVideo_.seconds = 74.2;
    lastVideo_.encoder = L"NVIDIA H.264 Encoder MFT";
    lastVideo_.hardware = true;
    showVideo_ = true;
    shot(L"home_video_saved.png", 96);
    showVideo_ = false;
    settings_.source = kSourceDisplay;
    shot(L"home_display.png", 96);
    settings_.source = kSourceRegion;
    RECT savedRegion = settings_.region;
    settings_.region = {};
    settings_.output = kOutputClip;
    shot(L"home_region_empty.png", 96);
    if (!monitors_.empty()) {
        const RECT& m = monitors_.front().rect;
        settings_.region = {m.left + 200, m.top + 150, m.left + 1480, m.top + 870};
    }
    settings_.output = kOutputVideo;
    shot(L"home_region.png", 96);
    settings_.region = savedRegion;
    settings_.source = kSourceWindow;
    settings_.output = kOutputClip;

    // Region picker mid-drag over a fake desktop on the primary monitor.
    for (auto& m : monitors_) {
        if (!m.primary) continue;
        const RECT& mr = m.rect;
        int mw = mr.right - mr.left, mh = mr.bottom - mr.top;
        gfx_.RenderToPng((dir + L"\\region_picker.png").c_str(), mw, mh, 96, [&](ID2D1DeviceContext* dc) {
            Painter p;
            p.Bind(dc);
            p.Fill(Rect(0, 0, (float)mw, (float)mh), theme::Hex(0x2B4C7E));
            p.Fill(Rect::XYWH(mw * 0.18f, mh * 0.16f, mw * 0.5f, mh * 0.55f), theme::Hex(0xF3F4F6), 8);
            p.Fill(Rect::XYWH(mw * 0.18f, mh * 0.16f, mw * 0.5f, 36), theme::Hex(0xD9DCE3), 8);
            p.Fill(Rect::XYWH(mw * 0.3f, mh * 0.3f, mw * 0.25f, mh * 0.3f), theme::Hex(0xFF5E87), 6);
            POINT a{mr.left + mw * 26 / 100, mr.top + mh * 27 / 100}, c{mr.left + mw * 60 / 100, mr.top + mh * 66 / 100};
            regionPicker_.PaintTest(p, mr, a, c);
        });
        gfx_.RenderToPng((dir + L"\\region_adjust.png").c_str(), mw, mh, 96, [&](ID2D1DeviceContext* dc) {
            Painter p;
            p.Bind(dc);
            p.Fill(Rect(0, 0, (float)mw, (float)mh), theme::Hex(0x2B4C7E));
            p.Fill(Rect::XYWH(mw * 0.18f, mh * 0.16f, mw * 0.5f, mh * 0.55f), theme::Hex(0xF3F4F6), 8);
            p.Fill(Rect::XYWH(mw * 0.3f, mh * 0.3f, mw * 0.25f, mh * 0.3f), theme::Hex(0xFF5E87), 6);
            POINT a{mr.left + mw * 26 / 100, mr.top + mh * 27 / 100}, c{mr.left + mw * 60 / 100, mr.top + mh * 66 / 100};
            regionPicker_.PaintTest(p, mr, a, c, true);
        });
    }

    screen_ = Screen::Settings;
    settingsScroll_ = 0;
    shot(L"settings.png", 96);
    settingsScroll_ = 10000;
    shot(L"settings_video.png", 96);
    settingsScroll_ = 0;

    OpenEditor(MakeSyntheticClip(960, 540, 120, 30));
    if (thumbThread_.joinable()) thumbThread_.join();  // let it finish (StopThumbnails would cancel)
    thumbs_.clear();
    for (auto& px : thumbPixels_) thumbs_.push_back(gfx_.CreateBitmap(thumbW_, thumbH_, px.data(), thumbW_ * 4));
    playing_ = false;
    inF_ = 12;
    outF_ = 100;
    playhead_ = 40;
    shot(L"editor.png", 96);
    cropMode_ = true;
    crop_ = {120, 60, 840, 465};
    hasResult_ = true;
    lastResult_.path = settings_.outDir + L"\\OzMaker_20261102_142311.gif";
    lastResult_.bytes = 2'345'678;
    shot(L"editor_crop.png", 96);
    toast_ = L"파일을 클립보드에 복사했습니다 · 메신저나 탐색기에 붙여넣으세요";
    toastUntil_ = clock + 60;
    shot(L"editor_toast.png", 96);
    toast_ = L"동영상 인코더를 시작하지 못했습니다(0x8007000E). 설정 › 동영상에서 하드웨어 인코딩을 끄거나 해상도를 "
             L"낮춰 보세요. 그래픽 드라이버를 최신으로 업데이트하면 해결되는 경우도 많습니다.";
    toastError_ = true;
    shot(L"editor_toast_long.png", 96);
    toastError_ = false;
    toastUntil_ = 0;
    CloseEditor();

    // Files list: two recordings not exported yet and a few recent files (names only, nothing on disk).
    held_ = {{MakeSyntheticClip(320, 180, 30, 30), clock - 40, L"OzMaker Test Window"},
             {clip_, clock - 1500, L"화면 1"}};
    recentFiles_ = {{settings_.outDir + L"\\OzMaker_20261102_142311.gif", 2'345'678, 300, false},
                    {settings_.outDir + L"\\OzMaker_20261102_140205.mp4", 48'213'004, 4000, true},
                    {settings_.outDir + L"\\OzMaker_20261101_221940.webp", 812'004, 90000, false}};
    filesOpen_ = true;
    shot(L"files_popover.png", 96);
    filesOpen_ = false;
    held_.clear();
    recentFiles_.clear();
    return 0;
}

namespace {

// Decodes an MP4 with the Source Reader and logs what a player would see: duration, frame timing, and the level
// of a few test tones in the audio track (to check per-window audio isolation). pngPrefix empty = no snapshots.
bool AnalyzeMp4(Log& log, IWICImagingFactory* wic, const std::wstring& path, const std::vector<double>& freqs,
                const std::wstring& pngPrefix) {
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    ComPtr<IMFAttributes> ra;
    MFCreateAttributes(&ra, 1);
    ra->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    ComPtr<IMFSourceReader> rd;
    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), ra.Get(), &rd);
    if (FAILED(hr)) {
        log(util::Format(L"analyze FAIL: open 0x%08X", (unsigned)hr));
        return false;
    }
    PROPVARIANT var;
    PropVariantInit(&var);
    if (SUCCEEDED(rd->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var)))
        log(util::Format(L"duration %.3fs", var.uhVal.QuadPart / 1e7));
    PropVariantClear(&var);

    // ---- video
    ComPtr<IMFMediaType> native, vt;
    UINT32 w = 0, h = 0, num = 0, den = 1;
    GUID sub{};
    if (SUCCEEDED(rd->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native))) {
        MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &num, &den);
        native->GetGUID(MF_MT_SUBTYPE, &sub);
        UINT32 br = MFGetAttributeUINT32(native.Get(), MF_MT_AVG_BITRATE, 0);
        MFVideoArea na{};
        if (SUCCEEDED(native->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&na, sizeof(na), nullptr)))
            log(util::Format(L"native aperture %ldx%ld", na.Area.cx, na.Area.cy));
        log(util::Format(L"video %s %ux%u @ %.2f fps, header bitrate %.1f Mbps",
                         sub == MFVideoFormat_H264 ? L"H.264" : sub == MFVideoFormat_HEVC ? L"HEVC" : L"?", w, h,
                         den ? (double)num / den : 0.0, br / 1e6));
    } else {
        log(L"analyze FAIL: no video stream");
        return false;
    }
    MFCreateMediaType(&vt);
    vt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    vt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    rd->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, vt.Get());
    INT32 stride = (INT32)w * 4;
    ComPtr<IMFMediaType> cur;
    if (SUCCEEDED(rd->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) {
        stride = (INT32)MFGetAttributeUINT32(cur.Get(), MF_MT_DEFAULT_STRIDE, (UINT32)stride);
        MFVideoArea area{};
        if (SUCCEEDED(cur->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&area, sizeof(area), nullptr)))
            log(util::Format(L"display aperture %ldx%ld at %d,%d", area.Area.cx, area.Area.cy, area.OffsetX.value,
                             area.OffsetY.value));
    }
    rd->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    rd->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    std::vector<int64_t> times;
    std::vector<uint8_t> frame((size_t)w * h * 4), last;
    for (;;) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        if (FAILED(rd->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &s))) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!s) continue;
        times.push_back(ts);
        if (pngPrefix.empty()) continue;
        ComPtr<IMFMediaBuffer> mb;
        BYTE* p = nullptr;
        DWORD len = 0;
        if (FAILED(s->ConvertToContiguousBuffer(&mb)) || FAILED(mb->Lock(&p, nullptr, &len))) continue;
        if (len >= (size_t)w * h * 4) {
            for (UINT32 y = 0; y < h; ++y) {
                UINT32 sy = stride < 0 ? h - 1 - y : y;  // bottom-up RGB
                auto* d = reinterpret_cast<uint32_t*>(frame.data() + (size_t)y * w * 4);
                auto* q = reinterpret_cast<const uint32_t*>(p + (size_t)sy * w * 4);
                for (UINT32 x = 0; x < w; ++x) d[x] = q[x] | 0xFF000000u;
            }
            if (times.size() == 1) SavePng(wic, pngPrefix + L"_first.png", frame.data(), w, h);
            last = frame;
        }
        mb->Unlock();
    }
    if (!last.empty()) SavePng(wic, pngPrefix + L"_last.png", last.data(), w, h);
    if (times.size() >= 2) {
        int64_t mn = INT64_MAX, mx = 0;
        for (size_t i = 1; i < times.size(); ++i) {
            mn = std::min(mn, times[i] - times[i - 1]);
            mx = std::max(mx, times[i] - times[i - 1]);
        }
        log(util::Format(L"video frames %zu, first %.3fs, last %.3fs, step min %.2fms max %.2fms", times.size(),
                         times.front() / 1e7, times.back() / 1e7, mn / 1e4, mx / 1e4));
    } else {
        log(util::Format(L"video frames %zu", times.size()));
    }

    // ---- audio
    ComPtr<IMFMediaType> at;
    if (FAILED(rd->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &native))) {
        log(L"audio: none");
        return true;
    }
    UINT32 rate = MFGetAttributeUINT32(native.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
    UINT32 ch = MFGetAttributeUINT32(native.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0);
    UINT32 abr = MFGetAttributeUINT32(native.Get(), MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 0);
    MFCreateMediaType(&at);
    at->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    at->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    at->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    rd->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, at.Get());
    rd->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    rd->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    PROPVARIANT pos;
    PropVariantInit(&pos);
    pos.vt = VT_I8;
    pos.hVal.QuadPart = 0;
    rd->SetCurrentPosition(GUID_NULL, pos);
    std::vector<float> mono;
    int64_t firstAudio = -1;
    for (;;) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        if (FAILED(rd->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &ts, &s))) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!s) continue;
        if (firstAudio < 0) firstAudio = ts;
        ComPtr<IMFMediaBuffer> mb;
        BYTE* p = nullptr;
        DWORD len = 0;
        if (FAILED(s->ConvertToContiguousBuffer(&mb)) || FAILED(mb->Lock(&p, nullptr, &len))) continue;
        auto* pcm = reinterpret_cast<const int16_t*>(p);
        for (DWORD i = 0; i + ch <= len / 2; i += ch) {
            float v = 0;
            for (UINT32 c = 0; c < ch; ++c) v += pcm[i + c];
            mono.push_back(v / ch / 32768.f);
        }
        mb->Unlock();
    }
    double sumSq = 0;
    for (float v : mono) sumSq += (double)v * v;
    double rms = mono.empty() ? 0 : std::sqrt(sumSq / mono.size());
    log(util::Format(L"audio AAC %u Hz x%u, %.0f kbps, first %.3fs, %.3fs of samples, rms %.4f", rate, ch,
                     abr * 8 / 1000.0, firstAudio / 1e7, rate ? mono.size() / (double)rate : 0.0, rms));
    // Tone levels: Goertzel over 100 ms blocks, averaged (robust to tiny frequency offsets).
    for (double f : freqs) {
        size_t n = rate / 10;
        double coeff = 2 * std::cos(2 * 3.14159265358979 * f / rate), total = 0;
        int blocks = 0;
        for (size_t b = 0; n && b + n <= mono.size(); b += n, ++blocks) {
            double s1 = 0, s2 = 0;
            for (size_t i = 0; i < n; ++i) {
                double s0 = mono[b + i] + coeff * s1 - s2;
                s2 = s1;
                s1 = s0;
            }
            double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
            total += 2 * std::sqrt(std::max(0.0, power)) / n;
        }
        log(util::Format(L"tone %.0f Hz: amplitude %.4f", f, blocks ? total / blocks : 0.0));
    }
    return true;
}

}  // namespace

int App::RunRecordTest(const std::wstring& cmdLine) {
    // --record-test <title> <sec> <dir> [fps] [maxEdge] [window|display|monitor|region] [clip|mp4] [audio|mute]
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW((L"x " + cmdLine).c_str(), &argc);
    std::vector<std::wstring> a(argv + 1, argv + argc);
    LocalFree(argv);
    if (a.size() < 4) return 1;
    std::wstring title = a[1];
    double seconds = _wtof(a[2].c_str());
    std::wstring dir = a[3];
    util::EnsureDir(dir);
    Log log(dir + L"\\record.log");

    auto lower = [](std::wstring s) {
        for (auto& c : s) c = (wchar_t)towlower(c);
        return s;
    };
    HWND target = nullptr;
    bool exact = !title.empty() && title[0] == L'=';  // "=Title" matches the whole title
    if (exact) title = title.substr(1);
    for (auto& w : EnumerateCaptureWindows())
        if (exact ? w.title == title : lower(w.title).find(lower(title)) != std::wstring::npos) {
            target = w.hwnd;
            log(L"target: " + w.title + L" (" + w.exeName + L")");
            break;
        }
    if (!target) {
        log(L"FAIL: no window matching " + title);
        return 1;
    }

    std::wstring mode = a.size() > 6 ? a[6] : L"window";
    // mp4 | mp4hevc | mp4sw | mp4hevcsw
    bool mp4 = a.size() > 7 && a[7].rfind(L"mp4", 0) == 0;
    bool hevc = mp4 && a[7].find(L"hevc") != std::wstring::npos;
    bool software = mp4 && a[7].find(L"sw") != std::wstring::npos;
    bool audio = a.size() > 8 && a[8] == L"audio";
    CaptureOptions o;
    o.target = target;
    o.fps = a.size() > 4 ? _wtoi(a[4].c_str()) : 30;
    o.maxEdge = a.size() > 5 ? _wtoi(a[5].c_str()) : 960;
    o.clientOnly = true;
    o.cursor = true;
    if (mode == L"display") {
        o.monitorMode = true;
    } else if (mode == L"monitor") {
        o.source = CaptureSource::Monitor;
        o.monitor = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
    } else if (mode == L"region") {
        // The target's client area as a screen region: records only that rectangle of the monitor.
        WINDOWINFO wi{sizeof(wi)};
        GetWindowInfo(target, &wi);
        o.source = CaptureSource::Region;
        o.region = wi.rcClient;
        o.monitor = MonitorFromRect(&o.region, MONITOR_DEFAULTTONEAREST);
        log(util::Format(L"region %d,%d %dx%d", o.region.left, o.region.top, o.region.right - o.region.left,
                         o.region.bottom - o.region.top));
    }
    if (mp4) {
        VideoOptions v;
        v.path = dir + L"\\record.mp4";
        v.video.hardware = !software;
        v.video.hevc = hevc;
        v.video.gopFrames = o.fps * 2;
        v.audio.enabled = audio;
        v.audio.sampleRate = 48000;
        v.audio.bitrate = 160000;
        v.processAudio = o.source == CaptureSource::Window;
        v.audioPid = AudioCapture::AudioProcessOf(target);
        o.video = v;
        if (o.maxEdge <= 0) o.maxEdge = 4096;
        for (int hv = 0; hv < 2; ++hv)
            for (int hw = 1; hw >= 0; --hw)
                log(util::Format(L"encoder %s %s: %s", hv ? L"HEVC" : L"H.264", hw ? L"hw" : L"sw",
                                 Mp4Writer::FindEncoder(hv != 0, hw != 0).c_str()));
        log(util::Format(L"process loopback supported: %d, audio pid %u (%s)",
                         AudioCapture::ProcessLoopbackSupported(), v.audioPid,
                         v.processAudio ? L"window only" : L"system"));
    }
    std::wstring err;
    double t0 = util::NowSeconds();
    if (!capture_.Start(o, nullptr, 0, err)) {
        log(L"FAIL: " + err);
        return 1;
    }
    log(util::Format(L"start took %.0f ms", (util::NowSeconds() - t0) * 1000));
    double end = util::NowSeconds() + seconds;
    while (util::NowSeconds() < end) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, 0);
    }
    log(util::Format(L"stopping after %zu frames", capture_.Frames()));
    t0 = util::NowSeconds();
    CaptureResult res = capture_.Stop();
    log(util::Format(L"stop took %.0f ms", (util::NowSeconds() - t0) * 1000));
    if (res.video) {
        log(util::Format(L"mp4 %s: %s, %.2fs, encoder %s (hw %d), audio %d %s %s", res.ok ? L"ok" : L"FAIL",
                         util::FormatBytes(res.bytes).c_str(), res.seconds, res.encoder.c_str(), res.hardware, res.audio,
                         res.notice.c_str(), res.error.c_str()));
        if (!res.ok) return 2;
        // Never keep pictures of a whole monitor (it may show unrelated windows).
        AnalyzeMp4(log, gfx_.Wic(), res.path, {440, 880}, mode == L"monitor" ? L"" : dir + L"\\mp4");
        return 0;
    }
    auto clip = res.clip;
    if (!clip) {
        log(L"FAIL: no frames");
        return 1;
    }
    log(util::Format(L"captured %zu frames %dx%d, %.2fs (%.1f fps avg), dropped %u, disk %s, mode %s", clip->Count(),
                     clip->Width(), clip->Height(), clip->Seconds(), clip->AverageFps(), clip->Dropped(),
                     util::FormatBytes(clip->DiskBytes()).c_str(), mode.c_str()));
    if (mode != L"monitor") {
        FrameStore::Reader rd(clip.get());
        SavePng(gfx_.Wic(), dir + L"\\first.png", rd.Get(0), clip->Width(), clip->Height());
        SavePng(gfx_.Wic(), dir + L"\\last.png", rd.Get(clip->Count() - 1), clip->Width(), clip->Height());
    }
    Exporter ex;
    int failures = 0;
    for (int f = 0; f < (int)Format::Count; ++f) {
        if (!FormatAvailable((Format)f)) continue;
        ExportJob job;
        job.clip = clip;
        job.first = 0;
        job.last = clip->Count() - 1;
        job.crop = {0, 0, clip->Width(), clip->Height()};
        job.outW = clip->Width();
        job.outH = clip->Height();
        job.format = (Format)f;
        job.fpsLimit = std::min(FormatMaxFps((Format)f), (double)o.fps);
        job.quality = f < (int)Format::Mp4 ? settings_.quality[f] : 0;
        job.path = dir + L"\\record." + FormatExt((Format)f);
        double s = util::NowSeconds();
        ExportResult r = ExportSync(ex, job);
        log(util::Format(L"%s: %s %s, %d frames, %.2fs, %.0f ms %s", FormatName((Format)f), r.ok ? L"ok" : L"FAIL",
                         util::FormatBytes(r.bytes).c_str(), r.frames, r.seconds, (util::NowSeconds() - s) * 1000,
                         r.error.c_str()));
        if (!r.ok) ++failures;
    }
    return failures ? 2 : 0;
}

int App::RunAnalyzeTest(const std::wstring& cmdLine) {
    // --analyze <file.mp4> <logdir> [freq ...]
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW((L"x " + cmdLine).c_str(), &argc);
    std::vector<std::wstring> a(argv + 1, argv + argc);
    LocalFree(argv);
    if (a.size() < 3) return 1;
    util::EnsureDir(a[2]);
    Log log(a[2] + L"\\analyze.log");
    std::vector<double> freqs;
    for (size_t i = 3; i < a.size(); ++i) freqs.push_back(_wtof(a[i].c_str()));
    return AnalyzeMp4(log, gfx_.Wic(), a[1], freqs, L"") ? 0 : 1;
}

namespace {

// Frame number as 12 black/white blocks across the top band: survives lossy compression and scaling.
void DrawIndexed(uint8_t* p, int w, int h, int index, double t) {
    DrawSynthetic(p, w, h, t);
    int bw = w / 12, bh = h / 8;
    for (int bit = 0; bit < 12; ++bit) {
        uint8_t v = (index >> bit) & 1 ? 255 : 0;
        for (int y = 0; y < bh; ++y)
            for (int x = bit * bw; x < (bit + 1) * bw; ++x) {
                uint8_t* q = p + ((size_t)y * w + x) * 4;
                q[0] = q[1] = q[2] = v;
                q[3] = 255;
            }
    }
}

int ReadIndex(const uint8_t* p, int w, int h) {
    int bw = w / 12, bh = h / 8, v = 0;
    for (int bit = 0; bit < 12; ++bit) {
        const uint8_t* q = p + ((size_t)(bh / 2) * w + bit * bw + bw / 2) * 4;
        if (q[1] > 128) v |= 1 << bit;
    }
    return v;
}

// Test sound: a tone that steps up every second (440, 880, 1320 Hz), so cuts can be located in the audio.
double ToneAt(double t) { return 440.0 * (1 + (int)t); }

double ToneLevel(const std::vector<int16_t>& stereo, size_t from, size_t n, double f, int rate) {
    double coeff = 2 * std::cos(2 * 3.14159265358979 * f / rate), s1 = 0, s2 = 0;
    for (size_t i = from; i < from + n && i * 2 + 1 < stereo.size(); ++i) {
        double s0 = stereo[i * 2] / 32768.0 + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return 2 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2)) / n;
}

// First time (s) from `after` on where tone b is louder than tone a, in 5 ms steps of 20 ms windows.
double ToneSwitch(const std::vector<int16_t>& st, int rate, double after, double a, double b) {
    size_t win = rate / 50, step = rate / 200;
    for (size_t i = (size_t)(after * rate); (i + win) * 2 <= st.size(); i += step)
        if (ToneLevel(st, i, win, b, rate) > ToneLevel(st, i, win, a, rate)) return (i + win / 2) / (double)rate;
    return -1;
}

bool WriteTestMp4(const std::wstring& path, int W, int H, int frames, int fps, int gop, std::wstring& err) {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Multithread> mt;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &dev, nullptr, &ctx)) ||
        FAILED(ctx.As(&mt))) {
        err = L"no device";
        return false;
    }
    mt->SetMultithreadProtected(TRUE);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W;
    td.Height = H;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    dev->CreateTexture2D(&td, nullptr, &tex);
    VideoConfig v;
    v.width = W;
    v.height = H;
    v.fps = fps;
    v.gopFrames = gop;
    AudioConfig a;
    a.enabled = true;
    Mp4Writer w;
    if (!w.Open(path, v, a, dev.Get(), err)) return false;
    std::vector<uint8_t> px((size_t)W * H * 4);
    std::vector<int16_t> pcm;
    int64_t pushed = 0;
    double phase = 0;
    for (int k = 0; k < frames; ++k) {
        DrawIndexed(px.data(), W, H, k, k / (double)fps);
        ctx->UpdateSubresource(tex.Get(), 0, nullptr, px.data(), W * 4, 0);
        ComPtr<IMFSample> s;
        for (int tries = 0; !(s = w.Convert(tex.Get())) && tries < 2500; ++tries) Sleep(2);
        int64_t want = (int64_t)(k + 1) * a.sampleRate / fps;
        pcm.clear();
        for (int64_t i = pushed; i < want; ++i) {
            phase += 2 * 3.14159265358979 * ToneAt(i / (double)a.sampleRate) / a.sampleRate;
            int16_t x = (int16_t)(0.03 * std::sin(phase) * 32767);  // quiet: the preview may play it
            pcm.push_back(x);
            pcm.push_back(x);
        }
        w.PushAudio(pcm.data(), pcm.size() / 2);
        pushed = want;
        if (!s || !w.WriteVideo(s.Get(), (int64_t)k * 10'000'000 / fps, (int64_t)(k + 1) * 10'000'000 / fps -
                                                                             (int64_t)k * 10'000'000 / fps)) {
            err = L"write failed";
            w.Abort();
            return false;
        }
    }
    return w.Finish(err);
}

// A copy of an MP4 whose video track says to show it turned clockwise by cw degrees, the way phones mark portrait
// video: the display matrix and size in the track header (tkhd) change, the pictures stay as they are.
bool WriteRotatedCopy(const std::wstring& src, const std::wstring& dst, int cw) {
    std::vector<uint8_t> d;
    if (FILE* f = _wfsopen(src.c_str(), L"rb", _SH_DENYNO)) {
        uint8_t buf[65536];
        for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) d.insert(d.end(), buf, buf + n);
        fclose(f);
    }
    auto be32 = [&](size_t p) { return (uint32_t)d[p] << 24 | (uint32_t)d[p + 1] << 16 | (uint32_t)d[p + 2] << 8 | d[p + 3]; };
    auto put32 = [&](size_t p, uint32_t v) {
        for (int i = 0; i < 4; ++i) d[p + i] = (uint8_t)(v >> (24 - 8 * i));
    };
    // Boxes directly inside [from, to): returns the payload offset of the first of that type, 0 if none.
    auto find = [&](size_t from, size_t to, const char* type) -> size_t {
        for (size_t p = from; p + 8 <= to;) {
            uint32_t size = be32(p);
            if (size < 8 || p + size > to) return 0;
            if (!memcmp(&d[p + 4], type, 4)) return p + 8;
            p += size;
        }
        return 0;
    };
    auto end = [&](size_t payload) { return payload - 8 + be32(payload - 8); };
    size_t moov = find(0, d.size(), "moov");
    if (!moov) return false;
    for (size_t p = moov; p + 8 <= end(moov);) {
        size_t trak = find(p, end(moov), "trak");
        if (!trak) return false;
        p = end(trak);
        size_t mdia = find(trak, end(trak), "mdia");
        size_t hdlr = mdia ? find(mdia, end(mdia), "hdlr") : 0;
        if (!hdlr || memcmp(&d[hdlr + 8], "vide", 4)) continue;
        size_t tkhd = find(trak, end(trak), "tkhd");
        if (!tkhd) return false;
        size_t m = tkhd + (d[tkhd] == 1 ? 52 : 40);  // matrix after the version-dependent times
        uint32_t w = be32(m + 36), h = be32(m + 40);  // 16.16
        const uint32_t one = 0x10000, neg = 0xFFFF0000;
        uint32_t a = 0, b = 0, c = 0, e = 0, x = 0, y = 0;
        if (cw == 90) b = one, c = neg, x = h;
        else if (cw == 180) a = neg, e = neg, x = w, y = h;
        else if (cw == 270) b = neg, c = one, y = w;
        else a = e = one;
        uint32_t mat[9] = {a, b, 0, c, e, 0, x, y, 0x40000000};
        for (int i = 0; i < 9; ++i) put32(m + i * 4, mat[i]);
        if (cw % 180) {
            put32(m + 36, h);
            put32(m + 40, w);
        }
        FILE* f = _wfsopen(dst.c_str(), L"wb", _SH_DENYNO);
        if (!f) return false;
        bool ok = fwrite(d.data(), 1, d.size(), f) == d.size();
        fclose(f);
        return ok;
    }
    return false;
}

// A copy of an MP4 whose video sample entry claims a codec nobody has ('avc1' -> 'oz00').
bool WriteUnknownCodecCopy(const std::wstring& src, const std::wstring& dst) {
    std::vector<uint8_t> d;
    if (FILE* f = _wfsopen(src.c_str(), L"rb", _SH_DENYNO)) {
        uint8_t buf[65536];
        for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) d.insert(d.end(), buf, buf + n);
        fclose(f);
    }
    auto at = [&](const char* s, size_t from) {
        auto it = std::search(d.begin() + std::min(from, d.size()), d.end(), s, s + 4);
        return it == d.end() ? SIZE_MAX : (size_t)(it - d.begin());
    };
    size_t stsd = at("stsd", 0), entry = stsd == SIZE_MAX ? SIZE_MAX : at("avc1", stsd);
    if (entry == SIZE_MAX) return false;
    memcpy(&d[entry], "oz00", 4);
    FILE* f = _wfsopen(dst.c_str(), L"wb", _SH_DENYNO);
    if (!f) return false;
    bool ok = fwrite(d.data(), 1, d.size(), f) == d.size();
    fclose(f);
    return ok;
}

std::vector<int16_t> ReadAllAudio(const VideoClip& c, int64_t from, double seconds, int& rate) {
    VideoClip::AudioReader ar;
    std::vector<int16_t> out;
    rate = 0;
    if (!ar.Open(c, from)) return out;
    rate = ar.Rate();
    out.resize((size_t)(seconds * rate) * 2);
    ar.Read(out.data(), out.size() / 2);
    return out;
}

}  // namespace

int App::RunVideoTest(const std::wstring& dir, int W, int H) {
    testWindowsOnly_ = true;
    util::EnsureDir(dir);
    Log log(dir + L"\\video.log");
    int failures = 0;
    auto check = [&](bool ok, const std::wstring& what) {
        if (!ok) ++failures;
        log((ok ? L"ok   " : L"FAIL ") + what);
    };
    const int N = 180, fps = 60;
    std::wstring src = dir + L"\\vt_src.mp4", err;
    double t0 = util::NowSeconds();
    bool wrote = WriteTestMp4(src, W, H, N, fps, 120, err);
    check(wrote, util::Format(L"write source %dx%d %d frames @%d (%.0f ms) %s", W, H, N, fps,
                              (util::NowSeconds() - t0) * 1000, err.c_str()));
    if (!wrote) return 2;

    t0 = util::NowSeconds();
    auto clip = VideoClip::Open(src, err);
    if (!clip) {
        check(false, L"open: " + err);
        return 2;
    }
    check(clip->Count() == (size_t)N && std::fabs(clip->Seconds() - 3.0) < 0.01 && clip->Width() == W &&
              clip->Height() == H && clip->HasAudio(),
          util::Format(L"open: %zu frames, %.3fs, %dx%d, audio %d Hz, keyframe@120 -> %zu, %.1f ms", clip->Count(),
                       clip->Seconds(), clip->Width(), clip->Height(), clip->AudioRate(), clip->KeyBefore(130),
                       (util::NowSeconds() - t0) * 1000));

    // The sample-table index must match reading every sample.
    if (auto scanned = VideoClip::Open(src, err, true)) {
        int64_t worst = std::llabs(scanned->EndTs() - clip->EndTs()) + std::llabs(scanned->Offset() - clip->Offset());
        int keyDiff = 0;
        for (size_t i = 0; i < clip->Count() && i < scanned->Count(); ++i) {
            worst = std::max(worst, std::llabs(scanned->Ts(i) - clip->Ts(i)));
            if (scanned->KeyBefore(i) != clip->KeyBefore(i)) ++keyDiff;
        }
        check(scanned->Count() == clip->Count() && worst <= 2 && keyDiff == 0,
              util::Format(L"sample tables vs. scan: %zu/%zu frames, max diff %lld x100ns, %d keyframe mismatches",
                           clip->Count(), scanned->Count(), worst, keyDiff));
    }
    {
        auto rd = clip->NewReader();
        int bad = 0;
        t0 = util::NowSeconds();
        for (int i = 0; i < N; ++i) {
            const uint8_t* px = rd->Get(i);
            if (!px || ReadIndex(px, W, H) != i) ++bad;
            if (i == 37 && px) SavePng(gfx_.Wic(), dir + L"\\vt_frame37.png", px, W, H);
        }
        check(bad == 0, util::Format(L"sequential decode: %d wrong, %.2f ms/frame", bad,
                                     (util::NowSeconds() - t0) * 1000 / N));
        int order[] = {170, 5, 119, 120, 121, 60, 59, 0, 179, 90, 89};
        std::wstring times;
        bad = 0;
        for (int i : order) {
            t0 = util::NowSeconds();
            const uint8_t* px = rd->Get(i);
            if (!px || ReadIndex(px, W, H) != i) ++bad;
            times += util::Format(L" %d:%.0fms", i, (util::NowSeconds() - t0) * 1000);
        }
        check(bad == 0, util::Format(L"random access: %d wrong,", bad) + times);
    }

    // No decoder for the video: opening says so instead of showing a blank editor.
    {
        std::wstring bad = dir + L"\\vt_unknown_codec.mp4", why;
        bool made = WriteUnknownCodecCopy(src, bad);
        auto c = made ? VideoClip::Open(bad, why) : nullptr;
        check(made && !c && why.find(L"oz00") != std::wstring::npos, L"unknown codec: " + why);
    }

    // Portrait phone video: the frames come out turned the way the file says.
    for (int cw : {90, 180, 270}) {
        std::wstring rotPath = dir + util::Format(L"\\vt_rot%d.mp4", cw);
        auto rot = WriteRotatedCopy(src, rotPath, cw) ? VideoClip::Open(rotPath, err) : nullptr;
        if (!rot) {
            check(false, util::Format(L"rotated %d: can't open (%s)", cw, err.c_str()));
            continue;
        }
        auto ra = clip->NewReader(), rb = rot->NewReader();
        const uint8_t* pa = ra->Get(45);
        const uint8_t* pb = rb->Get(45);
        int bad = 0, points = 0;
        const int RW = cw == 180 ? W : H, RH = cw == 180 ? H : W;
        if (pa && pb && rot->Width() == RW && rot->Height() == RH) {
            for (int y = 0; y < H; y += 7)
                for (int x = 0; x < W; x += 7) {
                    // Where source pixel (x, y) ends up after the clockwise turn.
                    int ox = cw == 90 ? H - 1 - y : cw == 180 ? W - 1 - x : y;
                    int oy = cw == 90 ? x : cw == 180 ? H - 1 - y : W - 1 - x;
                    const uint8_t* s = pa + ((size_t)y * W + x) * 4;
                    const uint8_t* t = pb + ((size_t)oy * RW + ox) * 4;
                    // The video processor that turns the picture resamples colour a little at sharp edges.
                    ++points;
                    for (int k = 0; k < 3; ++k)
                        if (std::abs(s[k] - t[k]) > 40) {
                            ++bad;
                            break;
                        }
                }
            if (cw == 90) SavePng(gfx_.Wic(), dir + L"\\vt_rot90_frame45.png", pb, RW, RH);
        }
        check(pa && pb && rot->Rotation() == cw && rot->Width() == RW && rot->Height() == RH && points > 0 &&
                  bad * 20 < points,
              util::Format(L"rotated %d: %dx%d, turn %d, %d of %d pixels out of place", cw, rot->Width(),
                           rot->Height(), rot->Rotation(), bad, points));
    }

    // Source audio: where does the tone step at 1.000 s land after the AAC round trip?
    int rate = 0;
    auto srcAudio = ReadAllAudio(*clip, 0, 3.0, rate);
    double srcSwitch = ToneSwitch(srcAudio, rate, 0.5, 440, 880);
    check(rate == 48000 && srcSwitch > 0.95 && srcSwitch < 1.05,
          util::Format(L"source audio %d Hz, 440->880 at %.3fs (written at 1.000)", rate, srcSwitch));
    auto seekAudio = ReadAllAudio(*clip, clip->Ts(37), 2.0, rate);
    double seekSwitch = ToneSwitch(seekAudio, rate, 0.1, 440, 880);
    double expect = srcSwitch - clip->Ts(37) / 1e7;
    check(std::fabs(seekSwitch - expect) < 0.015,
          util::Format(L"audio reader from frame 37: switch at %.3fs, expected %.3f", seekSwitch, expect));

    Exporter ex;
    // Trim to frames 37..150 with sound.
    {
        ExportJob job;
        job.clip = clip;
        job.first = 37;
        job.last = 150;
        job.crop = {0, 0, W, H};
        job.outW = W;
        job.outH = H;
        job.fpsLimit = fps;
        job.format = Format::Mp4;
        job.audio.enabled = true;
        job.path = dir + L"\\vt_trim.mp4";
        t0 = util::NowSeconds();
        ExportResult r = ExportSync(ex, job);
        check(r.ok && r.audio && r.frames == 114,
              util::Format(L"trim export: %s, %d frames, %.3fs, audio %d, %s, %.0f ms %s", r.ok ? L"ok" : L"failed",
                           r.frames, r.seconds, r.audio, util::FormatBytes(r.bytes).c_str(),
                           (util::NowSeconds() - t0) * 1000, r.error.c_str()));
        auto out = VideoClip::Open(job.path, err);
        if (!out) {
            check(false, L"open trimmed: " + err);
        } else {
            auto rd = out->NewReader();
            int bad = 0, first = -1, last = -1;
            for (size_t k = 0; k < out->Count(); ++k) {
                const uint8_t* px = rd->Get(k);
                int idx = px ? ReadIndex(px, W, H) : -1;
                if (k == 0) first = idx;
                last = idx;
                if (idx != 37 + (int)k) ++bad;
            }
            check(out->Count() == 114 && bad == 0 && std::fabs(out->Seconds() - 1.9) < 0.01,
                  util::Format(L"trimmed: %zu frames, %.3fs, frames %d..%d, %d out of place", out->Count(),
                               out->Seconds(), first, last, bad));
            auto a = ReadAllAudio(*out, 0, out->Seconds(), rate);
            double sw1 = ToneSwitch(a, rate, 0.05, 440, 880), sw2 = ToneSwitch(a, rate, sw1 + 0.2, 880, 1320);
            double e1 = srcSwitch - clip->Ts(37) / 1e7, e2 = e1 + 1.0;
            // One more AAC round trip than the source: allow its delay once more.
            check(std::fabs(sw1 - e1) < 0.06 && std::fabs(sw2 - e2) < 0.06,
                  util::Format(L"trimmed audio: switches at %.3f / %.3f s, expected %.3f / %.3f (+%.0f ms)", sw1, sw2,
                               e1, e2, (sw1 - e1) * 1000));
        }
    }
    // Cropped, half size, 2x speed at 30 fps: no sound, every 4th source frame.
    {
        ExportJob job;
        job.clip = clip;
        job.first = 37;
        job.last = 150;
        job.crop = {0, 0, W, H / 2};
        job.outW = W / 2;
        job.outH = H / 4;
        job.fpsLimit = 30;
        job.speed = 2.0;
        job.format = Format::Mp4;
        job.audio.enabled = true;  // dropped: speed != 1
        job.path = dir + L"\\vt_fast.mp4";
        ExportResult r = ExportSync(ex, job);
        auto out = r.ok ? VideoClip::Open(job.path, err) : nullptr;
        int bad = 0;
        if (out) {
            auto rd = out->NewReader();
            for (size_t k = 0; k < out->Count(); ++k) {
                const uint8_t* px = rd->Get(k);
                if (!px || ReadIndex(px, out->Width(), out->Height() * 2) != 37 + 4 * (int)k) ++bad;
            }
        }
        check(out && !r.audio && !out->HasAudio() && out->Count() == 29 && bad == 0 && out->Width() == W / 2,
              util::Format(L"2x/30fps/crop export: %zu frames %dx%d, audio %d, %d out of place %s",
                           out ? out->Count() : 0, out ? out->Width() : 0, out ? out->Height() : 0, r.audio, bad,
                           r.error.c_str()));
    }
    // A GIF from the video.
    {
        ExportJob job;
        job.clip = clip;
        job.first = 60;
        job.last = 119;
        job.crop = {100, 50, 500, 300};
        job.outW = 200;
        job.outH = 125;
        job.fpsLimit = 30;
        job.format = Format::Gif;
        job.quality = 80;
        job.path = dir + L"\\vt_cut.gif";
        ExportResult r = ExportSync(ex, job);
        check(r.ok && r.frames == 30, util::Format(L"GIF from video: %d frames, %s %s", r.frames,
                                                   util::FormatBytes(r.bytes).c_str(), r.error.c_str()));
    }

    // Editor on the video (frames come from the fetcher; no window here, so poll it).
    OpenEditor(clip);
    if (thumbThread_.joinable()) thumbThread_.join();
    thumbs_.clear();
    for (auto& px : thumbPixels_) thumbs_.push_back(gfx_.CreateBitmap(thumbW_, thumbH_, px.data(), thumbW_ * 4));
    playing_ = false;
    inF_ = 37;
    outF_ = 150;
    playhead_ = 90;
    fetcher_.Want(playhead_);
    size_t got = SIZE_MAX;
    for (int i = 0; i < 300 && got != playhead_; ++i) {
        size_t idx;
        if (fetcher_.Take(framePx_, idx)) got = idx;
        Sleep(10);
    }
    if (got != SIZE_MAX) {
        UploadFrame(framePx_.data());
        shown_ = got;
    }
    check(got == playhead_ && ReadIndex(framePx_.data(), W, H) == 90, L"editor preview frame via fetcher");
    double clock = util::NowSeconds();
    auto shot = [&](const std::wstring& name) {
        dpi_ = 96;
        for (int pass = 0; pass < 10; ++pass) {
            clock += 0.05;
            gfx_.RenderToPng((dir + L"\\" + name).c_str(), 1120, 780, 96, [&](ID2D1DeviceContext* dc) {
                UiInput in;
                ui_.Begin(dc, in, clock);
                DrawFrame(Rect(0, 0, 1120, 780));
                ui_.End();
            });
        }
    };
    shot(L"editor_video.png");
    hasResult_ = resultOriginal_ = true;
    lastResult_ = {};
    lastResult_.ok = true;
    lastResult_.path = src;
    lastResult_.bytes = util::FileSize(src);
    shot(L"editor_video_saved.png");
    settings_.speedIdx = 3;
    hasResult_ = false;
    shot(L"editor_video_fast.png");
    settings_.speedIdx = 1;
    CloseEditor();
    log(failures ? util::Format(L"RESULT: %d failure(s)", failures) : L"RESULT: all passed");
    return failures ? 2 : 0;
}

// Region picker on the primary monitor, driven with posted messages: drag, move, resize, ratio, keys.
int App::RunRegionTest(const std::wstring& dir) {
    util::EnsureDir(dir);
    Log log(dir + L"\\region.log");
    int failures = 0;
    auto check = [&](bool ok, const std::wstring& what) {
        if (!ok) ++failures;
        log((ok ? L"ok   " : L"FAIL ") + what);
    };
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const int ox = mi.rcMonitor.left, oy = mi.rcMonitor.top;
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    bool done = false, ok = false;
    RECT got{};
    auto pump = [&](double s) {
        double until = util::NowSeconds() + s;
        MSG msg;
        while (util::NowSeconds() < until) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(5);
        }
    };
    auto start = [&] {
        done = ok = false;
        regionPicker_.Start({}, [&](bool o, RECT r) {
            done = true;
            ok = o;
            got = r;
        });
        pump(0.15);
    };
    // Monitor-relative points to the picker's client coordinates (it covers the virtual screen).
    auto post = [&](UINT msg, int x, int y, WPARAM w = 0) {
        PostMessageW(regionPicker_.Hwnd(), msg, w, MAKELPARAM(ox + x - vx, oy + y - vy));
    };
    auto drag = [&](int x0, int y0, int x1, int y1) {
        post(WM_MOUSEMOVE, x0, y0);
        post(WM_LBUTTONDOWN, x0, y0, MK_LBUTTON);
        for (int t = 1; t <= 4; ++t) post(WM_MOUSEMOVE, x0 + (x1 - x0) * t / 4, y0 + (y1 - y0) * t / 4, MK_LBUTTON);
        post(WM_LBUTTONUP, x1, y1);
        pump(0.12);
    };
    auto key = [&](UINT vk) {
        PostMessageW(regionPicker_.Hwnd(), WM_KEYDOWN, vk, 0);
        pump(0.08);
    };
    auto rel = [&](RECT r) {
        return util::Format(L"%d,%d-%d,%d", r.left - ox, r.top - oy, r.right - ox, r.bottom - oy);
    };
    auto is = [&](RECT r, int l, int t, int rr, int b) {
        return r.left - ox == l && r.top - oy == t && r.right - ox == rr && r.bottom - oy == b;
    };

    start();
    drag(200, 150, 1000, 600);
    RECT s = regionPicker_.AdjustedArea();
    check(!done && regionPicker_.Adjusting() && is(s, 200, 150, 1001, 601), L"drag, then adjusting: " + rel(s));
    drag(600, 375, 700, 425);
    s = regionPicker_.AdjustedArea();
    check(is(s, 300, 200, 1101, 651), L"move by 100,50: " + rel(s));
    drag(1101, 651, 1151, 681);
    s = regionPicker_.AdjustedArea();
    check(is(s, 300, 200, 1151, 681), L"bottom-right handle +50,+30: " + rel(s));
    RECT b = regionPicker_.ToolbarButton(3);  // 16:9
    post(WM_LBUTTONDOWN, (b.left + b.right) / 2 - ox, (b.top + b.bottom) / 2 - oy, MK_LBUTTON);
    post(WM_LBUTTONUP, (b.left + b.right) / 2 - ox, (b.top + b.bottom) / 2 - oy);
    pump(0.12);
    s = regionPicker_.AdjustedArea();
    check(is(s, 300, 201, 1151, 680), L"16:9 keeps width and centre: " + rel(s));
    drag(s.right - ox, s.bottom - oy, s.right - ox + 100, s.bottom - oy + 20);
    s = regionPicker_.AdjustedArea();
    double k = (s.right - s.left) / (double)(s.bottom - s.top);
    check(s.left - ox == 300 && s.top - oy == 201 && std::fabs(k - 16.0 / 9) < 0.01 && s.right - ox == 1251,
          util::Format(L"corner drag keeps 16:9 (%.4f): ", k) + rel(s));
    drag(s.right - ox, (s.top + s.bottom) / 2 - oy, s.right - ox - 300, (s.top + s.bottom) / 2 - oy);
    s = regionPicker_.AdjustedArea();
    k = (s.right - s.left) / (double)(s.bottom - s.top);
    check(s.left - ox == 300 && s.right - ox == 951 && std::fabs(k - 16.0 / 9) < 0.01,
          util::Format(L"right edge drag keeps 16:9 (%.4f): ", k) + rel(s));
    RECT before = s;
    key(VK_RIGHT);
    s = regionPicker_.AdjustedArea();
    check(s.left == before.left + 1 && s.right == before.right + 1 && s.top == before.top, L"arrow key moves 1px: " + rel(s));
    key(VK_RETURN);
    check(done && ok && EqualRect(&got, &s) && !regionPicker_.Active(), L"Enter confirms: " + rel(got));

    start();
    post(WM_MOUSEMOVE, 400, 300);
    post(WM_LBUTTONDOWN, 400, 300, MK_LBUTTON);
    post(WM_LBUTTONUP, 400, 300);
    pump(0.12);
    s = regionPicker_.AdjustedArea();
    check(!done && regionPicker_.Adjusting() && EqualRect(&s, &mi.rcMonitor), L"click (no window) takes the screen: " + rel(s));
    key(VK_ESCAPE);
    check(done && !ok && !regionPicker_.Active(), L"Esc cancels");
    log(failures ? util::Format(L"RESULT: %d failure(s)", failures) : L"RESULT: all passed");
    return failures ? 2 : 0;
}

// The post-recording flow in the real window and message loop: a saved MP4 opens in the editor, plays through
// the frame fetcher, gets trimmed with the keyboard and exported, then the editor is closed and reopened.
int App::RunEditorTest(const std::wstring& path, const std::wstring& dir) {
    testWindowsOnly_ = true;
    util::EnsureDir(dir);
    Log log(dir + L"\\editor.log");
    int failures = 0;
    auto check = [&](bool ok, const std::wstring& what) {
        if (!ok) ++failures;
        log((ok ? L"ok   " : L"FAIL ") + what);
    };
    settings_.outDir = dir;
    if (!CreateMainWindow(SW_SHOW)) return 1;
    bool quit = false;
    auto pump = [&](double seconds, const std::function<bool()>& until = nullptr) {
        double end = util::NowSeconds() + seconds;
        while (!quit && util::NowSeconds() < end && !(until && until())) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) quit = true;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            double now = util::NowSeconds();
            if (IsWindowVisible(hwnd_) && (dirty_ || ui_.Animating() || (wake_ > 0 && now >= wake_)))
                Render();
            else
                MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    };
    auto key = [&](UINT vk) {
        PostMessageW(hwnd_, WM_KEYDOWN, vk, 0);
        pump(0.15);
    };
    pump(0.3);

    CaptureResult r;
    r.video = r.ok = r.audio = true;
    r.path = path;
    r.bytes = util::FileSize(path);
    r.seconds = 3.0;
    r.encoder = L"NVIDIA H.264 Encoder MFT";
    OnVideoSaved(r);
    check(screen_ == Screen::Editor && clip_ && clip_->SlowSeek() && hasResult_ && resultOriginal_ &&
              fmt_ == (int)Format::Mp4,
          util::Format(L"saved video opens the editor (%zu frames, format %d)", clip_ ? clip_->Count() : 0, fmt_));
    size_t first = SIZE_MAX;
    pump(0.5, [&] { return shown_ != SIZE_MAX; });
    first = shown_;
    pump(1.0);
    bool fg = GetForegroundWindow() == hwnd_;
    check(first != SIZE_MAX && shown_ != first && playing_,
          util::Format(L"plays through the fetcher: frame %zu -> %zu, foreground %d, sound preview %d", first, shown_,
                       fg, audioPreview_.Active()));
    if (fg) check(audioPreview_.Active(), L"sound preview runs while playing in front");

    key(VK_SPACE);
    check(!playing_ && !audioPreview_.Active(), L"space pauses (sound stops)");
    Seek(40);
    key(VK_LEFT);
    key(VK_LEFT);
    key(VK_LEFT);
    pump(1.0, [&] { return shown_ == 37; });
    check(playhead_ == 37 && shown_ == 37, util::Format(L"arrow keys step back: playhead %zu, shown %zu", playhead_,
                                                        shown_));
    key('I');
    Seek(150);
    key('O');
    check(inF_ == 37 && outF_ == 150, util::Format(L"I/O set the range: %zu..%zu", inF_, outF_));
    check(!hasResult_, L"changing the range hides the original's card");

    StartExport(false);
    pump(30, [&] { return !exporter_.Busy() && hasResult_; });
    check(hasResult_ && !resultOriginal_ && lastResult_.ok && lastResult_.frames == 114 && lastResult_.audio,
          util::Format(L"export: %s, %d frames, %.3fs, audio %d, %s", util::FileNameOf(lastResult_.path).c_str(),
                       lastResult_.frames, lastResult_.seconds, lastResult_.audio,
                       util::FormatBytes(lastResult_.bytes).c_str()));
    std::wstring err;
    auto out = VideoClip::Open(lastResult_.path, err);
    check(out && out->Count() == 114 && out->HasAudio(),
          util::Format(L"exported file: %zu frames, %.3fs", out ? out->Count() : 0, out ? out->Seconds() : 0));

    SetPlaying(true);
    pump(0.3);
    CloseEditor();
    pump(0.2);
    check(screen_ == Screen::Home && !fetcher_.Running() && !audioPreview_.Active() && showVideo_,
          L"closing the editor lets go of the file and shows the video card");
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);  // exclusive
    check(h != INVALID_HANDLE_VALUE, L"video file not held open after closing the editor");
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    ResumeEditor();
    shown_ = SIZE_MAX;
    pump(1.0, [&] { return shown_ != SIZE_MAX; });
    check(screen_ == Screen::Editor && fetcher_.Running() && shown_ != SIZE_MAX, L"editor reopens on the video");
    // Home with the test window's live preview under the saved-video card. The preview is a DWM thumbnail, which
    // only shows up on screen, so grab the screen: just the capture panel (the window list shows other programs).
    if (HWND testWin = FindWindowW(nullptr, L"OzMaker Test Window")) {
        CloseEditor();
        settings_.source = kSourceWindow;
        target_ = testWin;
        showVideo_ = true;
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        Invalidate();
        pump(1.0);
        RECT rc;
        GetClientRect(hwnd_, &rc);
        int skip = (int)(400 * Scale());
        POINT o{skip, 0};
        ClientToScreen(hwnd_, &o);
        int w = rc.right - skip, h = rc.bottom;
        HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(mem, bmp);
        BitBlt(mem, 0, 0, w, h, screen, o.x, o.y, SRCCOPY | CAPTUREBLT);
        auto* px = static_cast<uint32_t*>(bits);
        for (int i = 0; i < w * h; ++i) px[i] |= 0xFF000000u;
        SavePng(gfx_.Wic(), dir + L"\\home_card.png", static_cast<uint8_t*>(bits), w, h);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        log(L"home card with a window preview: home_card.png");
    }
    exporter_.Cancel();
    quitting_ = true;
    DestroyWindow(hwnd_);
    pump(2.0);
    log(failures ? util::Format(L"RESULT: %d failure(s)", failures) : L"RESULT: all passed");
    return failures ? 2 : 0;
}
