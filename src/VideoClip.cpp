#include "pch.h"
#include "VideoClip.h"
#include "Mp4Writer.h"

namespace {

class VideoReader : public FrameReader {
public:
    explicit VideoReader(const VideoClip* c) : c_(c) {}
    const uint8_t* Get(size_t index) override;

private:
    bool Init();
    bool CreateReader(bool gpu);
    void ReadOutputType();
    bool CopyOut(IMFSample* s);
    const uint8_t* Out() const;  // the frame in have_, turned for display

    const VideoClip* c_;
    ComPtr<IMFSourceReader> rd_;
    bool inited_ = false, failed_ = false, eos_ = true;
    size_t pos_ = SIZE_MAX;   // last frame the decoder delivered
    size_t have_ = SIZE_MAX;  // frame in buf_
    std::vector<uint8_t> buf_;  // decoded picture (coded size)
    std::vector<uint8_t> rot_;  // turned for display, when the decoder hands out the picture as coded
    bool turn_ = false;
    UINT32 outW_ = 0, outH_ = 0;
    LONG stride_ = 0;
    int apX_ = 0, apY_ = 0;
};

bool VideoReader::CreateReader(bool gpu) {
    rd_.Reset();
    ComPtr<IMFAttributes> a;
    MFCreateAttributes(&a, 3);
    a->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    if (gpu) {
        IMFDXGIDeviceManager* mgr = c_->DeviceManager();
        if (!mgr) return false;
        a->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, mgr);
        a->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    }
    if (FAILED(MFCreateSourceReaderFromURL(c_->SourcePath().c_str(), a.Get(), &rd_))) return false;
    rd_->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    rd_->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    ComPtr<IMFMediaType> t;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(rd_->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, t.Get()))) {
        rd_.Reset();
        return false;
    }
    ReadOutputType();
    return true;
}

bool VideoReader::Init() {
    if (!CreateReader(true) && !CreateReader(false)) return false;
    buf_.assign((size_t)c_->Width() * c_->Height() * 4, 0);
    return true;
}

void VideoReader::ReadOutputType() {
    ComPtr<IMFMediaType> t;
    if (FAILED(rd_->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &t))) return;
    MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &outW_, &outH_);
    stride_ = (LONG)MFGetAttributeUINT32(t.Get(), MF_MT_DEFAULT_STRIDE, outW_ * 4);
    // The decoded frame can be padded (1080 -> 1088); the picture sits at the display aperture.
    MFVideoArea ap{};
    apX_ = apY_ = 0;
    UINT32 pw = outW_, ph = outH_;
    if (SUCCEEDED(t->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&ap, sizeof(ap), nullptr))) {
        apX_ = std::max(0, (int)ap.OffsetX.value);
        apY_ = std::max(0, (int)ap.OffsetY.value);
        if (ap.Area.cx > 0 && ap.Area.cy > 0) pw = ap.Area.cx, ph = ap.Area.cy;
    }
    // The video processor normally turns the picture already (and clears the attribute); turn it here otherwise.
    const int rot = c_->Rotation();
    if (rot % 180 && c_->Width() != c_->Height())
        turn_ = (int)pw == c_->CodedWidth() && (int)ph == c_->CodedHeight();
    else
        turn_ = rot && MFGetAttributeUINT32(t.Get(), MF_MT_VIDEO_ROTATION, 0) != 0;
    if (turn_ && rot_.empty()) rot_.assign((size_t)c_->Width() * c_->Height() * 4, 0);
}

bool VideoReader::CopyOut(IMFSample* s) {
    ComPtr<IMFMediaBuffer> mb;
    if (FAILED(s->GetBufferByIndex(0, &mb))) return false;
    ComPtr<IMF2DBuffer> b2;
    BYTE* p = nullptr;
    LONG pitch = 0;
    bool locked2d = SUCCEEDED(mb.As(&b2)) && SUCCEEDED(b2->Lock2D(&p, &pitch));
    if (!locked2d) {
        DWORD len = 0;
        if (FAILED(mb->Lock(&p, nullptr, &len))) return false;
        pitch = stride_;
        if (pitch < 0) p += (size_t)(-pitch) * (outH_ - 1);  // bottom-up: start at the top row
    }
    const int W = turn_ ? c_->CodedWidth() : c_->Width(), H = turn_ ? c_->CodedHeight() : c_->Height();
    int cw = std::min(W, (int)outW_ - apX_), ch = std::min(H, (int)outH_ - apY_);
    for (int y = 0; y < H; ++y) {
        auto* dst = reinterpret_cast<uint32_t*>(buf_.data() + (size_t)y * W * 4);
        if (y >= ch || cw <= 0) {
            std::fill(dst, dst + W, 0xFF000000u);
            continue;
        }
        auto* src = reinterpret_cast<const uint32_t*>(p + (ptrdiff_t)(apY_ + y) * pitch + apX_ * 4);
        // RGB32's fourth byte is undefined; the preview bitmaps are premultiplied, so make it opaque.
        for (int x = 0; x < cw; ++x) dst[x] = src[x] | 0xFF000000u;
        std::fill(dst + cw, dst + W, 0xFF000000u);
    }
    if (locked2d)
        b2->Unlock2D();
    else
        mb->Unlock();
    if (turn_) {
        const int rot = c_->Rotation();
        // Destination row by row from the source pixel that lands there.
        auto* src = reinterpret_cast<const uint32_t*>(buf_.data());
        auto* dst = reinterpret_cast<uint32_t*>(rot_.data());
        const int OW = c_->Width(), OH = c_->Height();
        for (int y = 0; y < OH; ++y) {
            uint32_t* row = dst + (size_t)y * OW;
            if (rot == 90)
                for (int x = 0; x < OW; ++x) row[x] = src[(size_t)(H - 1 - x) * W + y];
            else if (rot == 180)
                for (int x = 0; x < OW; ++x) row[x] = src[(size_t)(H - 1 - y) * W + (W - 1 - x)];
            else
                for (int x = 0; x < OW; ++x) row[x] = src[(size_t)x * W + (W - 1 - y)];
        }
    }
    return true;
}

const uint8_t* VideoReader::Out() const {
    if (have_ == SIZE_MAX) return nullptr;
    return turn_ ? rot_.data() : buf_.data();
}

const uint8_t* VideoReader::Get(size_t index) {
    if (failed_ || index >= c_->Count()) return nullptr;
    if (index == have_) return Out();
    if (!inited_) {
        inited_ = true;
        if (!Init()) {
            failed_ = true;
            return nullptr;
        }
    }
    size_t key = c_->KeyBefore(index);
    bool forward = !eos_ && pos_ != SIZE_MAX && index > pos_ && pos_ >= key;
    if (!forward) {
        PROPVARIANT v;
        PropVariantInit(&v);
        v.vt = VT_I8;
        // Just past the keyframe, so rounding can't land the seek on the one before it.
        v.hVal.QuadPart = c_->Ts(key) + c_->Offset() + 1000;
        HRESULT hr = rd_->SetCurrentPosition(GUID_NULL, v);
        if (FAILED(hr)) return Out();
        eos_ = false;
        pos_ = SIZE_MAX;
    }
    for (;;) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        if (FAILED(rd_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &s))) {
            eos_ = true;
            break;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) ReadOutputType();
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            eos_ = true;
            break;
        }
        if (!s) continue;
        size_t idx = c_->Nearest(ts - c_->Offset());
        if (pos_ != SIZE_MAX && idx <= pos_) idx = std::min(pos_ + 1, c_->Count() - 1);
        pos_ = idx;
        if (idx >= index) {
            if (CopyOut(s.Get())) have_ = idx;
            break;
        }
    }
    return Out();
}

// ---- MP4 sample tables: frame times and keyframes of a long video in milliseconds instead of reading every sample

uint32_t Be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint64_t Be64(const uint8_t* p) { return (uint64_t)Be32(p) << 32 | Be32(p + 4); }
constexpr uint32_t Tag(const char (&s)[5]) {
    return (uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 | (uint32_t)s[3];
}

struct Span {
    const uint8_t* p = nullptr;
    size_t n = 0;
};

// Steps through the boxes inside parent: type and payload of the one at pos.
bool NextBox(Span parent, size_t& pos, uint32_t& type, Span& body) {
    if (pos + 8 > parent.n) return false;
    uint64_t size = Be32(parent.p + pos);
    size_t hdr = 8;
    if (size == 1) {
        if (pos + 16 > parent.n) return false;
        size = Be64(parent.p + pos + 8);
        hdr = 16;
    } else if (size == 0) {
        size = parent.n - pos;
    }
    if (size < hdr || size > parent.n - pos) return false;
    type = Be32(parent.p + pos + 4);
    body = {parent.p + pos + hdr, (size_t)size - hdr};
    pos += (size_t)size;
    return true;
}

bool Child(Span parent, uint32_t want, Span& out) {
    size_t pos = 0;
    uint32_t type;
    Span body;
    while (NextBox(parent, pos, type, body))
        if (type == want) {
            out = body;
            return true;
        }
    return false;
}

int64_t To100ns(int64_t v, uint32_t scale) {
    uint64_t a = v < 0 ? (uint64_t)-v : (uint64_t)v;
    uint64_t r = a / scale * 10'000'000 + (a % scale * 10'000'000 + scale / 2) / scale;
    return v < 0 ? -(int64_t)r : (int64_t)r;
}

struct SampleEntry {
    int64_t ts, dur;  // 100 ns; ts relative to the first sample in decode order
    bool key;
};

// The first video track's samples from moov (stts/ctts/stss). False for anything unusual (fragmented files etc.):
// the caller then reads the samples one by one.
bool ReadSampleTables(const std::wstring& path, std::vector<SampleEntry>& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    auto readAt = [&](uint64_t off, void* dst, DWORD n) {
        OVERLAPPED ov{};
        ov.Offset = (DWORD)off;
        ov.OffsetHigh = (DWORD)(off >> 32);
        DWORD got = 0;
        return ReadFile(f, dst, n, &got, &ov) && got == n;
    };
    LARGE_INTEGER fileSize{};
    GetFileSizeEx(f, &fileSize);
    std::vector<uint8_t> moov;
    for (uint64_t pos = 0; pos + 8 <= (uint64_t)fileSize.QuadPart;) {
        uint8_t h[16];
        if (!readAt(pos, h, 8)) break;
        uint64_t size = Be32(h), hdr = 8;
        if (size == 1) {
            if (!readAt(pos + 8, h + 8, 8)) break;
            size = Be64(h + 8);
            hdr = 16;
        } else if (size == 0) {
            size = fileSize.QuadPart - pos;
        }
        if (size < hdr || pos + size > (uint64_t)fileSize.QuadPart) break;
        if (Be32(h + 4) == Tag("moov")) {
            if (size - hdr > (256u << 20)) break;
            moov.resize((size_t)(size - hdr));
            if (!readAt(pos + hdr, moov.data(), (DWORD)moov.size())) moov.clear();
            break;
        }
        pos += size;
    }
    CloseHandle(f);
    if (moov.empty()) return false;

    Span root{moov.data(), moov.size()}, trak, mdia, hdlr, mdhd, minf, stbl, stts, ctts, stss;
    size_t pos = 0;
    uint32_t type;
    while (NextBox(root, pos, type, trak)) {
        if (type != Tag("trak")) continue;
        if (!Child(trak, Tag("mdia"), mdia) || !Child(mdia, Tag("hdlr"), hdlr) || hdlr.n < 12 ||
            Be32(hdlr.p + 8) != Tag("vide"))
            continue;
        if (!Child(mdia, Tag("mdhd"), mdhd) || mdhd.n < 24 || !Child(mdia, Tag("minf"), minf) ||
            !Child(minf, Tag("stbl"), stbl) || !Child(stbl, Tag("stts"), stts) || stts.n < 8)
            return false;
        uint32_t scale = mdhd.p[0] == 1 ? (mdhd.n >= 24 ? Be32(mdhd.p + 20) : 0) : Be32(mdhd.p + 12);
        uint32_t entries = Be32(stts.p + 4);
        if (scale == 0 || stts.n < 8 + (size_t)entries * 8) return false;
        std::vector<int64_t> dts, delta;
        int64_t t = 0;
        for (uint32_t i = 0; i < entries; ++i) {
            uint32_t count = Be32(stts.p + 8 + i * 8), d = Be32(stts.p + 12 + i * 8);
            if (dts.size() + count > 50'000'000) return false;
            for (uint32_t c = 0; c < count; ++c, t += d) {
                dts.push_back(t);
                delta.push_back(d);
            }
        }
        if (dts.empty()) return false;
        std::vector<int64_t> pts = dts;
        if (Child(stbl, Tag("ctts"), ctts) && ctts.n >= 8) {
            uint32_t n = Be32(ctts.p + 4);
            if (ctts.n < 8 + (size_t)n * 8) return false;
            size_t k = 0;
            for (uint32_t i = 0; i < n && k < pts.size(); ++i) {
                uint32_t count = Be32(ctts.p + 8 + i * 8);
                int32_t off = (int32_t)Be32(ctts.p + 12 + i * 8);  // signed in version 1, and in practice in 0 too
                for (uint32_t c = 0; c < count && k < pts.size(); ++c) pts[k++] += off;
            }
        }
        std::vector<uint8_t> key(pts.size(), 1);  // no stss: every sample is a sync sample
        if (Child(stbl, Tag("stss"), stss) && stss.n >= 8) {
            uint32_t n = Be32(stss.p + 4);
            if (stss.n < 8 + (size_t)n * 4) return false;
            std::fill(key.begin(), key.end(), 0);
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t s = Be32(stss.p + 8 + i * 4);
                if (s >= 1 && s <= key.size()) key[s - 1] = 1;
            }
        }
        out.clear();
        out.reserve(pts.size());
        for (size_t k = 0; k < pts.size(); ++k)
            out.push_back({To100ns(pts[k] - pts[0], scale), To100ns(delta[k], scale), key[k] != 0});
        return true;
    }
    return false;
}

// Why a video can't be decoded, by its compression format.
std::wstring NoDecoderMessage(const GUID& subtype) {
    if (subtype == MFVideoFormat_HEVC || subtype == MFVideoFormat_HEVC_ES)
        return L"HEVC(H.265) 영상입니다. Microsoft Store의 'HEVC 비디오 확장'을 설치하면 열 수 있습니다.";
    if (subtype == MFVideoFormat_VP90)
        return L"VP9 영상입니다. Microsoft Store의 'VP9 비디오 확장'을 설치하면 열 수 있습니다.";
    if (subtype == MFVideoFormat_AV1)
        return L"AV1 영상입니다. Microsoft Store의 'AV1 비디오 확장'을 설치하면 열 수 있습니다.";
    // The MPEG-4 source puts a sample entry type it doesn't know into Data1, first letter in the high byte.
    wchar_t cc[5] = {};
    for (int i = 0; i < 4; ++i) {
        wchar_t ch = (wchar_t)((subtype.Data1 >> (24 - 8 * i)) & 0xFF);
        cc[i] = ch >= 0x20 && ch < 0x7F ? ch : L'?';
    }
    return std::wstring(L"이 영상 형식(") + cc + L")을 재생할 코덱이 이 PC에 없습니다.";
}

}  // namespace

std::shared_ptr<VideoClip> VideoClip::Open(const std::wstring& path, std::wstring& err, bool scanSamples) {
    EnsureMediaFoundation();
    ComPtr<IMFSourceReader> rd;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &rd))) {
        err = L"동영상 파일을 열 수 없습니다.";
        return nullptr;
    }
    ComPtr<IMFMediaType> native;
    if (FAILED(rd->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native))) {
        err = L"영상이 들어 있지 않은 파일입니다.";
        return nullptr;
    }
    auto c = std::make_shared<VideoClip>();
    c->path_ = path;
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h);
    MFVideoArea ap{};
    if (SUCCEEDED(native->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&ap, sizeof(ap), nullptr)) &&
        ap.Area.cx > 0 && ap.Area.cy > 0) {
        w = std::min<UINT32>(w ? w : ap.Area.cx, ap.Area.cx);
        h = std::min<UINT32>(h ? h : ap.Area.cy, ap.Area.cy);
    }
    // Phones store portrait video as landscape pictures plus a turn to apply when showing them.
    switch (MFGetAttributeUINT32(native.Get(), MF_MT_VIDEO_ROTATION, MFVideoRotationFormat_0)) {
        case MFVideoRotationFormat_90: c->rotation_ = 90; break;
        case MFVideoRotationFormat_180: c->rotation_ = 180; break;
        case MFVideoRotationFormat_270: c->rotation_ = 270; break;
    }
    c->width_ = (int)(c->rotation_ % 180 ? h : w);
    c->height_ = (int)(c->rotation_ % 180 ? w : h);
    GUID subtype{};
    native->GetGUID(MF_MT_SUBTYPE, &subtype);
    ComPtr<IMFMediaType> at;
    if (SUCCEEDED(rd->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &at)))
        c->audioRate_ = (int)MFGetAttributeUINT32(at.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);

    // Frame times and keyframes from the container's sample tables, else by reading (not decoding) every sample.
    rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    rd->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    std::vector<SampleEntry> es;
    if (!scanSamples && ReadSampleTables(path, es)) {
        // Anchor on the first sample Media Foundation delivers, so times match its timestamps (edit lists etc.).
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        if (SUCCEEDED(rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &s)) && s)
            for (auto& e : es) e.ts += ts;
        else
            es.clear();
    }
    for (bool scan = es.empty(); scan;) {
        DWORD flags = 0;
        LONGLONG ts = 0, dur = 0;
        ComPtr<IMFSample> s;
        if (FAILED(rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &s)) ||
            (flags & MF_SOURCE_READERF_ENDOFSTREAM))
            break;
        if (!s) continue;
        s->GetSampleDuration(&dur);
        es.push_back({ts, dur, MFGetAttributeUINT32(s.Get(), MFSampleExtension_CleanPoint, 0) != 0});
    }
    if (es.empty() || w == 0 || h == 0) {
        err = L"영상 프레임을 읽을 수 없습니다.";
        return nullptr;
    }
    // Asking for decoded frames loads the decoder: without one the editor would only show a blank picture.
    {
        ComPtr<IMFMediaType> t;
        MFCreateMediaType(&t);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        ComPtr<IMFAttributes> a;
        MFCreateAttributes(&a, 1);
        a->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        ComPtr<IMFSourceReader> probe;
        if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), a.Get(), &probe)) ||
            FAILED(probe->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, t.Get()))) {
            err = NoDecoderMessage(subtype);
            return nullptr;
        }
    }
    std::stable_sort(es.begin(), es.end(), [](const SampleEntry& a, const SampleEntry& b) { return a.ts < b.ts; });
    c->offset_ = es.front().ts;
    c->ts_.reserve(es.size());
    for (size_t i = 0; i < es.size(); ++i) {
        c->ts_.push_back(es[i].ts - c->offset_);
        if (es[i].key) c->keys_.push_back(i);
    }
    if (c->keys_.empty() || c->keys_.front() != 0) c->keys_.insert(c->keys_.begin(), 0);
    int64_t last = es.back().dur;
    if (last <= 0) last = es.size() > 1 ? (c->ts_.back() - c->ts_.front()) / (int64_t)(es.size() - 1) : 333333;
    c->endTs_ = c->ts_.back() + last;
    return c;
}

std::unique_ptr<FrameReader> VideoClip::NewReader() const { return std::make_unique<VideoReader>(this); }

size_t VideoClip::KeyBefore(size_t i) const {
    auto it = std::upper_bound(keys_.begin(), keys_.end(), i);
    return it == keys_.begin() ? 0 : *(it - 1);
}

size_t VideoClip::QuickIndexNear(int64_t t, int64_t tolerance) const {
    size_t i = IndexAt(t);
    auto it = std::upper_bound(keys_.begin(), keys_.end(), i);
    size_t best = *(it - 1);  // keys_ starts with 0
    if (it != keys_.end() && ts_[*it] - t < t - ts_[best]) best = *it;
    return std::llabs(ts_[best] - t) <= tolerance ? best : i;
}

size_t VideoClip::Nearest(int64_t t) const {
    auto it = std::lower_bound(ts_.begin(), ts_.end(), t);
    if (it == ts_.end()) return ts_.size() - 1;
    size_t i = (size_t)(it - ts_.begin());
    if (i > 0 && t - ts_[i - 1] < ts_[i] - t) --i;
    return i;
}

IMFDXGIDeviceManager* VideoClip::DeviceManager() const {
    std::lock_guard lk(devMtx_);
    if (!devTried_) {
        devTried_ = true;
        ComPtr<ID3D11Device> dev;
        ComPtr<ID3D11DeviceContext> ctx;
        UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        ComPtr<ID3D11Multithread> mt;
        ComPtr<IMFDXGIDeviceManager> mgr;
        UINT token = 0;
        if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                        D3D11_SDK_VERSION, &dev, nullptr, &ctx)) &&
            SUCCEEDED(ctx.As(&mt)) && SUCCEEDED(MFCreateDXGIDeviceManager(&token, &mgr))) {
            mt->SetMultithreadProtected(TRUE);
            if (SUCCEEDED(mgr->ResetDevice(dev.Get(), token))) {
                dev_ = dev;
                devManager_ = mgr;
            }
        }
    }
    return devManager_.Get();
}

// ---------------------------------------------------------------- audio

bool VideoClip::AudioReader::Open(const VideoClip& clip, int64_t t0) {
    if (!clip.HasAudio()) return false;
    EnsureMediaFoundation();
    if (FAILED(MFCreateSourceReaderFromURL(clip.SourcePath().c_str(), nullptr, &rd_))) return false;
    rd_->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    rd_->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    ComPtr<IMFMediaType> native;
    if (FAILED(rd_->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &native))) return false;
    UINT32 ch = MFGetAttributeUINT32(native.Get(), MF_MT_AUDIO_NUM_CHANNELS, 2);
    UINT32 rate = MFGetAttributeUINT32(native.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
    // Stereo if the decoder can mix down, else whatever it has (converted to stereo here).
    bool ok = false;
    for (UINT32 want : {2u, ch}) {
        ComPtr<IMFMediaType> t;
        MFCreateMediaType(&t);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, want);
        if (rate) t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        if (SUCCEEDED(rd_->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, t.Get()))) {
            ok = true;
            break;
        }
    }
    ComPtr<IMFMediaType> cur;
    if (!ok || FAILED(rd_->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur))) return false;
    channels_ = (int)MFGetAttributeUINT32(cur.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0);
    rate_ = (int)MFGetAttributeUINT32(cur.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
    if (channels_ <= 0 || rate_ <= 0 || MFGetAttributeUINT32(cur.Get(), MF_MT_AUDIO_BITS_PER_SAMPLE, 0) != 16)
        return false;
    start_ = t0 + clip.Offset();
    PROPVARIANT v;
    PropVariantInit(&v);
    v.vt = VT_I8;
    v.hVal.QuadPart = std::max<int64_t>(0, start_);
    if (start_ > 0) rd_->SetCurrentPosition(GUID_NULL, v);
    return true;
}

// Decodes one sample into pending_, lined up with the output position by its timestamp.
bool VideoClip::AudioReader::Decode() {
    DWORD flags = 0;
    LONGLONG ts = 0;
    ComPtr<IMFSample> s;
    if (FAILED(rd_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &ts, &s)) ||
        (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
        eos_ = true;
        return false;
    }
    if (!s) return true;
    ComPtr<IMFMediaBuffer> mb;
    if (FAILED(s->ConvertToContiguousBuffer(&mb))) return true;
    BYTE* p = nullptr;
    DWORD len = 0;
    if (FAILED(mb->Lock(&p, nullptr, &len))) return true;
    const auto* in = reinterpret_cast<const int16_t*>(p);
    int64_t frames = len / (2 * channels_);
    int64_t at = (ts - start_) * rate_ / 10'000'000;  // where this sample belongs in the output
    const int64_t tol = rate_ / 100;
    int64_t skip = 0;
    if (at > queued_ + tol) {
        pending_.insert(pending_.end(), (size_t)(at - queued_) * 2, 0);  // gap: silence
        queued_ = at;
    } else if (at < queued_ - tol) {
        skip = std::min(frames, queued_ - at);  // overlaps what we already have (e.g. before the seek point)
    }
    for (int64_t i = skip; i < frames; ++i) {
        const int16_t* f = in + i * channels_;
        pending_.push_back(f[0]);
        pending_.push_back(channels_ > 1 ? f[1] : f[0]);
    }
    queued_ += frames - skip;
    mb->Unlock();
    return true;
}

void VideoClip::AudioReader::Read(int16_t* out, size_t frames) {
    while (rd_ && !eos_ && pending_.size() / 2 - pendingPos_ < frames) {
        if (pendingPos_ > 0) {  // drop what was handed out already
            pending_.erase(pending_.begin(), pending_.begin() + pendingPos_ * 2);
            pendingPos_ = 0;
        }
        Decode();
    }
    size_t avail = std::min(frames, pending_.size() / 2 - pendingPos_);
    memcpy(out, pending_.data() + pendingPos_ * 2, avail * 4);
    std::fill(out + avail * 2, out + frames * 2, (int16_t)0);
    pendingPos_ += avail;
    if (avail < frames) queued_ += (int64_t)(frames - avail);
}
