#include "pch.h"
#include "AnimClip.h"
#include "FrameStore.h"
#include "Util.h"

#if __has_include(<webp/demux.h>)
#include <webp/demux.h>
#define OZ_WEBP_DECODE 1
#endif
#if __has_include(<avif/avif.h>)
#include <avif/avif.h>
#define OZ_AVIF_DECODE 1
#endif

namespace {

constexpr int kMaxEdge = 16384;
constexpr uint64_t kMaxPixels = 8192ull * 8192;

// Browsers show a frame of 10 ms or less for 100 ms (ads used 0 to flash): so does the editor, and so the saved file.
double FrameMs(double ms) { return ms <= 10 ? 100 : ms; }

// Where the decoded canvases go, one after another.
struct Sink {
    std::shared_ptr<FrameStore> store;
    int w = 0, h = 0;
    int64_t ts = 0;  // where the next frame starts, 100 ns
    bool alpha = false;
    const std::atomic<bool>* cancel = nullptr;

    bool Begin(int width, int height, std::wstring& err) {
        if (width <= 0 || height <= 0 || width > kMaxEdge || height > kMaxEdge ||
            (uint64_t)width * height > kMaxPixels) {
            err = util::Format(tr(L"그림이 너무 큽니다 (%d×%d)."), width, height);
            return false;
        }
        w = width;
        h = height;
        store = std::make_shared<FrameStore>();
        return store->Open(w, h, err, false);
    }
    // A premultiplied BGRA canvas, tightly packed, shown for ms.
    bool Add(const uint8_t* bgra, double ms) {
        if (*cancel) return false;
        auto buf = store->Acquire();
        memcpy(buf->data(), bgra, buf->size());
        if (!alpha)
            for (size_t i = 3; i < buf->size(); i += 4)
                if (bgra[i] != 255) {
                    alpha = true;
                    break;
                }
        if (!store->Submit(std::move(buf), ts, true)) return false;
        ts += std::max<int64_t>(1, std::llround(FrameMs(ms) * 1e4));
        return true;
    }
};

bool ReadAll(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart < (1ll << 31);
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD read = 0;
        ok = out.empty() || (ReadFile(f, out.data(), (DWORD)out.size(), &read, nullptr) && read == out.size());
    }
    CloseHandle(f);
    return ok;
}

// ---------------------------------------------------------------- GIF (WIC)

UINT MetaUInt(IWICMetadataQueryReader* q, const wchar_t* name, UINT def = 0) {
    PROPVARIANT v;
    PropVariantInit(&v);
    UINT r = def;
    if (q && SUCCEEDED(q->GetMetadataByName(name, &v))) {
        if (v.vt == VT_UI1) r = v.bVal;
        else if (v.vt == VT_UI2) r = v.uiVal;
        else if (v.vt == VT_UI4) r = v.ulVal;
    }
    PropVariantClear(&v);
    return r;
}

// WIC hands out each frame as it is stored: a rectangle and how to leave it (disposal). Composited here as browsers do.
bool DecodeGif(const std::wstring& path, Sink& out, std::atomic<float>& progress, std::wstring& err) {
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmapDecoder> dec;
    UINT n = 0;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                              &dec)) ||
        FAILED(dec->GetFrameCount(&n)) || n == 0) {
        err = tr(L"GIF 파일을 읽을 수 없습니다.");
        return false;
    }
    ComPtr<IWICMetadataQueryReader> gq;
    dec->GetMetadataQueryReader(&gq);
    int W = (int)MetaUInt(gq.Get(), L"/logscrdesc/Width"), H = (int)MetaUInt(gq.Get(), L"/logscrdesc/Height");
    std::vector<uint8_t> canvas, saved, px;
    int disposal = 0;
    RECT last{};
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IWICBitmapFrameDecode> f;
        ComPtr<IWICFormatConverter> conv;
        UINT fw = 0, fh = 0;
        if (FAILED(dec->GetFrame(i, &f)) || FAILED(f->GetSize(&fw, &fh)) || FAILED(wic->CreateFormatConverter(&conv)) ||
            FAILED(conv->Initialize(f.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                    WICBitmapPaletteTypeCustom))) {
            if (i > 0) break;  // what came before plays; a cut-off file is common
            err = tr(L"GIF 파일을 읽을 수 없습니다.");
            return false;
        }
        ComPtr<IWICMetadataQueryReader> q;
        f->GetMetadataQueryReader(&q);
        const int left = (int)MetaUInt(q.Get(), L"/imgdesc/Left"), top = (int)MetaUInt(q.Get(), L"/imgdesc/Top");
        if (i == 0) {
            if (W <= 0 || H <= 0) W = left + (int)fw, H = top + (int)fh;
            if (!out.Begin(W, H, err)) return false;
            canvas.assign((size_t)W * H * 4, 0);
        }
        px.resize((size_t)fw * fh * 4);
        if (FAILED(conv->CopyPixels(nullptr, fw * 4, (UINT)px.size(), px.data()))) {
            if (i > 0) break;
            err = tr(L"GIF 파일을 읽을 수 없습니다.");
            return false;
        }
        // What the frame before asked to be done once it had been shown.
        if (disposal == 2) {
            for (LONG y = last.top; y < last.bottom; ++y)
                memset(&canvas[((size_t)y * W + last.left) * 4], 0, (size_t)(last.right - last.left) * 4);
        } else if (disposal == 3 && !saved.empty()) {
            canvas = saved;
        }
        disposal = (int)MetaUInt(q.Get(), L"/grctlext/Disposal");
        if (disposal == 3) saved = canvas;
        last = {std::clamp(left, 0, W), std::clamp(top, 0, H), std::clamp(left + (int)fw, 0, W),
                std::clamp(top + (int)fh, 0, H)};
        for (LONG y = last.top; y < last.bottom; ++y) {
            const uint8_t* s = &px[((size_t)(y - top) * fw + (last.left - left)) * 4];
            uint8_t* d = &canvas[((size_t)y * W + last.left) * 4];
            for (LONG x = last.left; x < last.right; ++x, s += 4, d += 4)
                if (s[3]) memcpy(d, s, 4);  // a GIF pixel is either there or not
        }
        if (!out.Add(canvas.data(), MetaUInt(q.Get(), L"/grctlext/Delay") * 10.0)) return false;
        progress = (float)(i + 1) / n;
    }
    return true;
}

// ---------------------------------------------------------------- WebP (libwebp)

bool DecodeWebp(const std::wstring& path, Sink& out, std::atomic<float>& progress, std::wstring& err) {
#if OZ_WEBP_DECODE
    std::vector<uint8_t> file;
    if (!ReadAll(path, file)) {
        err = tr(L"파일을 읽을 수 없습니다: ") + util::FileNameOf(path);
        return false;
    }
    WebPData data{file.data(), file.size()};
    WebPAnimDecoderOptions o;
    if (!WebPAnimDecoderOptionsInit(&o)) {
        err = tr(L"WebP 파일을 읽을 수 없습니다.");
        return false;
    }
    o.color_mode = MODE_bgrA;  // premultiplied
    o.use_threads = 1;
    WebPAnimDecoder* d = WebPAnimDecoderNew(&data, &o);
    WebPAnimInfo info{};
    if (!d || !WebPAnimDecoderGetInfo(d, &info)) {
        if (d) WebPAnimDecoderDelete(d);
        err = tr(L"WebP 파일을 읽을 수 없습니다.");
        return false;
    }
    bool ok = out.Begin((int)info.canvas_width, (int)info.canvas_height, err);
    int prev = 0;
    for (uint32_t i = 0; ok && WebPAnimDecoderHasMoreFrames(d); ++i) {
        uint8_t* buf = nullptr;
        int end = 0;  // when this frame stops showing, ms
        if (!WebPAnimDecoderGetNext(d, &buf, &end)) {
            ok = i > 0;  // what came before plays
            if (!ok) err = tr(L"WebP 파일을 읽을 수 없습니다.");
            break;
        }
        ok = out.Add(buf, end - prev);
        prev = end;
        progress = (float)(i + 1) / std::max(1u, info.frame_count);
    }
    WebPAnimDecoderDelete(d);
    return ok;
#else
    progress = 0;
    err = tr(L"WebP 디코더가 이 빌드에 포함되어 있지 않습니다.");
    return false;
#endif
}

// ---------------------------------------------------------------- AVIF (libavif)

bool DecodeAvif(const std::wstring& path, Sink& out, std::atomic<float>& progress, std::wstring& err) {
#if OZ_AVIF_DECODE
    std::vector<uint8_t> file;
    if (!ReadAll(path, file)) {
        err = tr(L"파일을 읽을 수 없습니다: ") + util::FileNameOf(path);
        return false;
    }
    avifDecoder* d = avifDecoderCreate();
    if (!d) {
        err = tr(L"AVIF 파일을 읽을 수 없습니다.");
        return false;
    }
    d->maxThreads = (int)std::max(1u, std::thread::hardware_concurrency());
    d->ignoreExif = d->ignoreXMP = AVIF_TRUE;
    bool ok = avifDecoderSetIOMemory(d, file.data(), file.size()) == AVIF_RESULT_OK &&
              avifDecoderParse(d) == AVIF_RESULT_OK;
    if (!ok) err = tr(L"AVIF 파일을 읽을 수 없습니다.");
    ok = ok && out.Begin((int)d->image->width, (int)d->image->height, err);
    std::vector<uint8_t> px((size_t)out.w * out.h * 4);
    for (int i = 0; ok; ++i) {
        const avifResult r = avifDecoderNextImage(d);
        if (r == AVIF_RESULT_NO_IMAGES_REMAINING) break;
        avifRGBImage rgb;
        avifRGBImageSetDefaults(&rgb, d->image);
        rgb.format = AVIF_RGB_FORMAT_BGRA;
        rgb.depth = 8;
        rgb.alphaPremultiplied = AVIF_TRUE;
        rgb.pixels = px.data();
        rgb.rowBytes = (uint32_t)out.w * 4;
        if (r != AVIF_RESULT_OK || (int)d->image->width != out.w || (int)d->image->height != out.h ||
            avifImageYUVToRGB(d->image, &rgb) != AVIF_RESULT_OK) {
            ok = i > 0;
            if (!ok) err = tr(L"AVIF 파일을 읽을 수 없습니다.");
            break;
        }
        ok = out.Add(px.data(), d->imageTiming.duration * 1000);
        progress = (float)(i + 1) / std::max(1, d->imageCount);
    }
    avifDecoderDestroy(d);
    return ok;
#else
    progress = 0;
    err = tr(L"AVIF 디코더가 이 빌드에 포함되어 있지 않습니다.");
    return false;
#endif
}

}  // namespace

Format AnimClip::Sniff(const std::wstring& path) {
    uint8_t h[12] = {};
    DWORD read = 0;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return Format::Count;
    ReadFile(f, h, sizeof(h), &read, nullptr);
    CloseHandle(f);
    if (read < 12) return Format::Count;
    if (!memcmp(h, "GIF8", 4)) return Format::Gif;
    if (!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WEBP", 4)) return Format::WebP;
    if (!memcmp(h + 4, "ftyp", 4) && (!memcmp(h + 8, "avif", 4) || !memcmp(h + 8, "avis", 4))) return Format::Avif;
    return Format::Count;
}

std::shared_ptr<AnimClip> AnimClip::Open(const std::wstring& path, const std::atomic<bool>& cancel,
                                         std::atomic<float>& progress, std::wstring& err) {
    const Format f = Sniff(path);
    Sink sink;
    sink.cancel = &cancel;
    bool ok = false;
    switch (f) {
        case Format::Gif: ok = DecodeGif(path, sink, progress, err); break;
        case Format::WebP: ok = DecodeWebp(path, sink, progress, err); break;
        case Format::Avif: ok = DecodeAvif(path, sink, progress, err); break;
        default: err = tr(L"GIF · WebP · AVIF 파일이 아닙니다: ") + util::FileNameOf(path); break;
    }
    if (!sink.store) return nullptr;
    sink.store->Finish(sink.ts);
    if (cancel) return nullptr;
    if (ok && sink.store->Count() < 2) {
        err = tr(L"움직이지 않는 그림입니다 (프레임 1개): ") + util::FileNameOf(path);
        ok = false;
    }
    if (ok && sink.store->WriteFailed()) {
        err = tr(L"임시 파일을 쓸 수 없습니다. 디스크 공간을 확인하세요.");
        ok = false;
    }
    if (!ok) return nullptr;
    auto clip = std::make_shared<AnimClip>();
    clip->store_ = std::move(sink.store);
    clip->path_ = path;
    clip->format_ = f;
    clip->alpha_ = sink.alpha;
    return clip;
}

int AnimClip::Width() const { return store_->Width(); }
int AnimClip::Height() const { return store_->Height(); }
size_t AnimClip::Count() const { return store_->Count(); }
int64_t AnimClip::Ts(size_t i) const { return store_->Ts(i); }
int64_t AnimClip::EndTs() const { return store_->EndTs(); }
std::unique_ptr<FrameReader> AnimClip::NewReader() const { return store_->NewReader(); }
