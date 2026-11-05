#include "pch.h"
#include "ImageFile.h"

#include "Caption.h"
#include "Lang.h"
#include "encoders/Encoder.h"

namespace image {

namespace {

constexpr float kJpgQuality = 0.92f;  // of 1
constexpr int kWebpQuality = 90;      // of 100

// Opaque pixels as BGR, rows packed: what the opaque PNG and the JPG are written from. See-through ones over black,
// which is what premultiplied colours already are.
std::vector<uint8_t> ToBgr(const Picture& p) {
    std::vector<uint8_t> out((size_t)p.w * p.h * 3);
    const uint8_t* s = p.px.data();
    uint8_t* d = out.data();
    for (size_t i = 0, n = (size_t)p.w * p.h; i < n; ++i, s += 4, d += 3) {
        d[0] = s[0];
        d[1] = s[1];
        d[2] = s[2];
    }
    return out;
}

// Premultiplied to straight alpha, what the files keep.
std::vector<uint8_t> ToStraight(const Picture& p) {
    std::vector<uint8_t> out(p.px);
    uint8_t* d = out.data();
    for (size_t i = 0, n = (size_t)p.w * p.h; i < n; ++i, d += 4) {
        const unsigned a = d[3];
        if (a == 255) continue;
        if (!a) {
            d[0] = d[1] = d[2] = 0;
            continue;
        }
        for (int c = 0; c < 3; ++c) d[c] = (uint8_t)std::min(255u, (d[c] * 255u + a / 2) / a);
    }
    return out;
}

bool WriteWic(const std::wstring& path, int format, const Picture& p) {
    const bool jpg = format == kJpg, alpha = p.alpha && !jpg;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> bag;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(wic->CreateEncoder(jpg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(&frame, &bag)))
        return false;
    if (jpg && bag) {
        PROPBAG2 name[2] = {};
        name[0].pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        name[1].pstrName = const_cast<LPOLESTR>(L"JpegYCrCbSubsampling");
        VARIANT v[2];
        VariantInit(&v[0]);
        VariantInit(&v[1]);
        v[0].vt = VT_R4;
        v[0].fltVal = kJpgQuality;
        v[1].vt = VT_UI1;
        v[1].bVal = WICJpegYCrCbSubsampling444;  // text keeps its coloured edges
        bag->Write(2, name, v);
    }
    if (FAILED(frame->Initialize(bag.Get())) || FAILED(frame->SetSize(p.w, p.h))) return false;
    WICPixelFormatGUID want = alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR, got = want;
    if (FAILED(frame->SetPixelFormat(&got)) || got != want) return false;
    const std::vector<uint8_t> px = alpha ? ToStraight(p) : ToBgr(p);
    const UINT stride = (UINT)p.w * (alpha ? 4 : 3);
    return SUCCEEDED(frame->WritePixels((UINT)p.h, stride, (UINT)px.size(), const_cast<BYTE*>(px.data()))) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}

}  // namespace

const wchar_t* Ext(int format) {
    switch (format) {
        case kJpg: return L"jpg";
        case kWebp: return L"webp";
        default: return L"png";
    }
}

bool Available(int format) { return format != kWebp || FormatAvailable(::Format::WebP); }

bool Write(const std::wstring& path, int format, const Picture& pic, std::wstring& err) {
    bool ok = false;
    if (format == kWebp) {
        const std::vector<uint8_t> px = pic.alpha ? ToStraight(pic) : pic.px;
        ok = WriteWebpImage(path, px.data(), pic.w, pic.h, pic.alpha, kWebpQuality);
    } else {
        ok = WriteWic(path, format, pic);
    }
    if (!ok) {
        DeleteFileW(path.c_str());
        err = tr(L"이미지를 저장할 수 없습니다: ") + path;
    }
    return ok;
}

bool TakeFrame(const Clip& clip, size_t index, const RECT& crop, const std::wstring& caption, int captionPos,
               Picture& pic, std::wstring& err) {
    const int W = clip.Width(), H = clip.Height();
    const int l = std::clamp((int)crop.left, 0, W), t = std::clamp((int)crop.top, 0, H);
    const int r = std::clamp((int)crop.right, l, W), b = std::clamp((int)crop.bottom, t, H);
    const auto reader = clip.NewReader();
    const uint8_t* src = reader && r > l && b > t ? reader->Get(index) : nullptr;
    if (!src) {
        err = tr(L"프레임을 읽을 수 없습니다.");
        return false;
    }
    pic.w = r - l;
    pic.h = b - t;
    pic.alpha = clip.HasAlpha();
    pic.px.resize((size_t)pic.w * pic.h * 4);
    for (int y = 0; y < pic.h; ++y)
        memcpy(&pic.px[(size_t)y * pic.w * 4], src + ((size_t)(t + y) * W + l) * 4, (size_t)pic.w * 4);
    if (!caption.empty()) {
        caption::Overlay over;
        if (over.Make(caption, captionPos, pic.w, pic.h)) over.Blend(pic.px.data());
    }
    return true;
}

std::vector<uint8_t> ToDib(const Picture& p) {
    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(bi);
    bi.biWidth = p.w;
    bi.biHeight = p.h;  // bottom-up: the one way every program reads
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    bi.biSizeImage = (DWORD)((size_t)p.w * p.h * 4);
    std::vector<uint8_t> out(sizeof(bi) + bi.biSizeImage);
    memcpy(out.data(), &bi, sizeof(bi));
    uint8_t* d = out.data() + sizeof(bi);
    for (int y = p.h - 1; y >= 0; --y) {
        const uint8_t* s = &p.px[(size_t)y * p.w * 4];
        for (int x = 0; x < p.w; ++x, s += 4, d += 4) {
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = 255;
        }
    }
    return out;
}

Saver::~Saver() {
    {
        std::lock_guard lock(mtx_);
        stop_ = true;
        notify_ = nullptr;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Saver::Add(Job job, HWND notify, UINT msg) {
    std::lock_guard lock(mtx_);
    jobs_.push_back(std::move(job));
    notify_ = notify;
    msg_ = msg;
    if (!thread_.joinable()) thread_ = std::thread(&Saver::Run, this);
    cv_.notify_one();
}

bool Saver::Busy() const {
    std::lock_guard lock(mtx_);
    return running_ || !jobs_.empty();
}

bool Saver::Take(Saved& out) {
    std::lock_guard lock(mtx_);
    if (done_.empty()) return false;
    out = std::move(done_.front());
    done_.pop_front();
    return true;
}

void Saver::Run() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // WIC
    std::unique_lock lock(mtx_);
    for (;;) {
        cv_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
        if (jobs_.empty()) break;
        Job job = std::move(jobs_.front());
        jobs_.pop_front();
        ++running_;
        lock.unlock();
        Saved r = job();
        job = nullptr;  // lets go of the clip here, not after the next wait
        lock.lock();
        --running_;
        done_.push_back(std::move(r));
        if (notify_) PostMessageW(notify_, msg_, 0, 0);
    }
    lock.unlock();
    if (SUCCEEDED(co)) CoUninitialize();
}

}  // namespace image
