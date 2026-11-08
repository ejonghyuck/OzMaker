// Headless test entry points (command line):
//   --selftest <dir>                      synthetic clip -> frame store round trip -> every available encoder
//   --screenshot <dir>                    renders each screen to PNG for visual review
//   --record-test <title> <sec> <dir> [fps] [maxEdge] [window|display|monitor|region] [clip|mp4] [audio|mute] [pause] [mic] [click] [vfr]
//                                         records a real window (or its monitor / its area) and exports it
//   --analyze <file.mp4> <dir> [freq ...] decodes an MP4: timing, size, test-tone levels
//   --editor-test <file.mp4> <dir>        the real window: saved video -> editor -> keys -> trimmed export -> 삭제
//   --video-test <dir> [w h]              synthetic MP4 -> frame index/decoding -> trimmed MP4/GIF exports, checked
//                                         frame by frame and by the audio tone boundaries; editor screenshots
//   --region-test <dir>                   the region picker's adjust mode, driven with synthetic input
//   --preview-test <dir>                  the live window preview on screen, with the files list open over it;
//                                         the window list following another process's window
//   --test-window <sec> cover <x> <y> <w> <h>  a plain white window over others there, never in front (--replay-test)
//   --test-window <sec> [capture]         a pattern window in a process of its own (for --preview-test); capture:
//                                         grey, one Windows lets --record-test take, a white square blinking every 1.5 s
//   --multi-test <dir>                    여러 창 녹화 (#48) in the app: two test windows recorded at once, as 움짤 and
//                                         as MP4 while one of them closes partway
//   --update-test <dir> [url|github]      the updater on a fake release folder (also part of --selftest); url: the
//                                         same served over HTTP; github: one check against the real releases
#include "pch.h"
#include "App.h"
#include "AnimClip.h"
#include "Caption.h"
#include "Crash.h"
#include "GpuResize.h"
#include "Mp4Cut.h"
#include "Mp4Unfragment.h"
#include "Replay.h"
#include "Resize.h"
#include "Util.h"
#include "Version.h"
#include "VideoClip.h"
#include "WebmWriter.h"

#include <fstream>
#if __has_include(<webp/decode.h>)
#include <webp/decode.h>
#endif

#if __has_include(<mkvparser/mkvparser.h>) && __has_include(<vpx/vp8dx.h>) && __has_include(<opus/opus.h>) && \
    __has_include(<libyuv.h>)
#include <libyuv.h>
#include <mkvparser/mkvparser.h>
#include <opus/opus.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#endif

namespace {

// Further down, with the other test helpers.
double ToneLevel(const std::vector<int16_t>& stereo, size_t from, size_t n, double f, int rate);
double ToneSwitch(const std::vector<int16_t>& st, int rate, double after, double a, double b);
std::vector<int16_t> ReadAllAudio(const VideoClip& c, int64_t from, double seconds, int& rate);
bool WriteTestMp4(const std::wstring& path, int W, int H, int frames, int fps, int gop, std::wstring& err,
                  bool fragmented = false, bool crash = false);
bool ProbeMp4(const std::wstring& path, const VideoConfig& v, bool sound, std::wstring& err, std::wstring& got);
bool WriteVfrMp4(const std::wstring& path, bool hardware, std::wstring& err, std::wstring& got, bool fragmented = true);
class Log;
int ReplayRingCheck(const std::wstring& dir, bool hardware, Log& log);
int ReadIndex(const uint8_t* p, int w, int h);
void DrawIndexed(uint8_t* p, int w, int h, int index, double t);

// What a WebM file holds (#40), read back with libwebm's parser and decoded with libvpx and Opus, so the check
// doesn't depend on the WebM decoders Windows may or may not have.
struct WebmContents {
    bool ok = false;
    std::wstring err;
    int width = 0, height = 0;
    double seconds = 0;              // the segment's duration
    std::vector<int> indexes;        // ReadIndex of each frame
    std::vector<double> times;       // when each frame starts (s)
    std::vector<int> keys;           // the frames that are keyframes
    std::vector<uint8_t> first;      // the first frame, BGRA
    std::vector<int16_t> audio;      // 48 kHz stereo, the encoder's pre-skip dropped
};
bool WebmReaderBuilt();
WebmContents ReadWebm(const std::wstring& path);

struct TopBox {
    std::string type;
    uint64_t pos, size;
};

// The top-level boxes of an MP4 file, up to the first that does not fit. `fragmented`: its moov has an mvex.
std::vector<TopBox> TopBoxes(const std::wstring& path, bool* fragmented) {
    std::vector<uint8_t> d;
    if (FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO)) {
        uint8_t buf[65536];
        for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) d.insert(d.end(), buf, buf + n);
        fclose(f);
    }
    auto be32 = [&](size_t p) { return (uint32_t)d[p] << 24 | (uint32_t)d[p + 1] << 16 | (uint32_t)d[p + 2] << 8 | d[p + 3]; };
    std::vector<TopBox> out;
    *fragmented = false;
    for (size_t p = 0; p + 8 <= d.size();) {
        uint64_t size = be32(p);
        if (size == 1 && p + 16 <= d.size()) size = (uint64_t)be32(p + 8) << 32 | be32(p + 12);
        if (size < 8 || size > d.size() - p) break;
        TopBox b{std::string(d.begin() + p + 4, d.begin() + p + 8), p, size};
        if (b.type == "moov") {
            static const char mvex[] = "mvex";
            auto from = d.begin() + p, to = from + (size_t)size;
            *fragmented |= std::search(from, to, mvex, mvex + 4) != to;
        }
        out.push_back(b);
        p += (size_t)size;
    }
    return out;
}

bool CutFile(const std::wstring& path, uint64_t at) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)at;
    bool ok = SetFilePointerEx(f, li, nullptr, FILE_BEGIN) && SetEndOfFile(f);
    CloseHandle(f);
    return ok;
}

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

// A made-up 움짤 with see-through parts (#41): premultiplied frames of their own lengths.
class AlphaClip : public Clip {
public:
    explicit AlphaClip(std::shared_ptr<FrameStore> s) : s_(std::move(s)) {}
    int Width() const override { return s_->Width(); }
    int Height() const override { return s_->Height(); }
    size_t Count() const override { return s_->Count(); }
    int64_t Ts(size_t i) const override { return s_->Ts(i); }
    int64_t EndTs() const override { return s_->EndTs(); }
    std::unique_ptr<FrameReader> NewReader() const override { return s_->NewReader(); }
    bool HasAlpha() const override { return true; }

private:
    std::shared_ptr<FrameStore> s_;
};

// Frame i of it: a red square moving right over nothing, a half-clear green band along the top; frame 3 is clear all
// over. Premultiplied BGRA.
void DrawAlphaFrame(uint8_t* px, int w, int h, int i) {
    memset(px, 0, (size_t)w * h * 4);
    if (i == 3) return;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = px + ((size_t)y * w + x) * 4;
            if (y < 8) p[1] = p[3] = 200;  // green at 200 of 255
            if (x >= 8 + i * 12 && x < 32 + i * 12 && y >= 20 && y < 44) p[0] = p[1] = 0, p[2] = p[3] = 255;
        }
}

// A sticker for the screenshots, at t (0-1) of its loop: a ball rolling left and right over nothing, with a soft
// shadow under it. Premultiplied BGRA.
void DrawSticker(uint8_t* px, int w, int h, double t) {
    memset(px, 0, (size_t)w * h * 4);
    const double r = h * 0.28, cx = w * 0.3 + w * 0.4 * (0.5 - 0.5 * std::cos(t * 6.2832)), cy = h * 0.42;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = px + ((size_t)y * w + x) * 4;
            const double sx = (x + 0.5 - cx) / (r * 1.1), sy = (y + 0.5 - (cy + r * 1.25)) / (r * 0.22);
            const double shadow = std::clamp(1 - (sx * sx + sy * sy), 0.0, 1.0) * 0.35;
            const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
            const double ball = std::clamp(r - d + 0.5, 0.0, 1.0);
            const double light = 1 - 0.35 * std::clamp((x - cx + y - cy) / (2 * r) + 0.5, 0.0, 1.0);
            const double a = ball + shadow * (1 - ball);
            p[0] = (uint8_t)std::lround(250 * light * ball);
            p[1] = (uint8_t)std::lround(139 * light * ball);
            p[2] = (uint8_t)std::lround(167 * light * ball);
            p[3] = (uint8_t)std::lround(255 * a);
        }
}

// NETSCAPE loop count of a GIF: -1 = no loop block (plays once), 0 = forever, n = repeats after the first play.
int GifLoopCount(const std::wstring& path) {
    std::vector<uint8_t> d;
    if (FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO)) {
        uint8_t buf[4096];
        size_t n = fread(buf, 1, sizeof(buf), f);
        d.assign(buf, buf + n);
        fclose(f);
    }
    const char tag[] = "NETSCAPE2.0";
    auto it = std::search(d.begin(), d.end(), tag, tag + 11);
    if (it == d.end() || d.end() - it < 15) return -1;
    return it[13] | (it[14] << 8);
}

// A hidden window to point the live preview at where no real one is wanted (offscreen renders).
HWND MakeStandInWindow(HINSTANCE inst) {
    return CreateWindowExW(0, L"STATIC", L"OzMaker Test Window", WS_POPUP, 0, 0, 800, 450, nullptr, nullptr, inst,
                           nullptr);
}

// A hidden window that answers UI Automation from an Access (GWLP_USERDATA), for the screen reader test.
LRESULT CALLBACK AccessTestProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_GETOBJECT && static_cast<long>(l) == static_cast<long>(UiaRootObjectId))
        if (auto* a = reinterpret_cast<Access*>(GetWindowLongPtrW(h, GWLP_USERDATA))) return a->OnGetObject(w, l);
    return DefWindowProcW(h, m, w, l);
}

// Eight blocks of saturated colour (nothing in the UI is that colour): a preview of it shows where each part of the
// window went.
const COLORREF kPatternColors[8] = {RGB(230, 40, 40),  RGB(40, 200, 60),  RGB(40, 90, 230),  RGB(240, 220, 40),
                                    RGB(220, 50, 220), RGB(40, 220, 220), RGB(250, 140, 30), RGB(150, 90, 30)};

// --test-window capture: grey (no colour the click check takes for its highlight), and a white square that shows
// while this is odd.
bool g_patternPlain = false;
int g_patternBlinks = 0;

LRESULT CALLBACK PatternProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_TIMER) {
        ++g_patternBlinks;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        for (int i = 0; i < 8; ++i) {
            RECT c{rc.right * (i % 4) / 4, rc.bottom * (i / 4) / 2, rc.right * (i % 4 + 1) / 4, rc.bottom * (i / 4 + 1) / 2};
            HBRUSH b = CreateSolidBrush(g_patternPlain ? RGB(70, 80, 90) : kPatternColors[i]);
            FillRect(dc, &c, b);
            DeleteObject(b);
        }
        if (g_patternBlinks & 1) {  // in a corner, clear of the middle where the click check looks
            RECT sq{8, 8, 48, 48};
            FillRect(dc, &sq, (HBRUSH)GetStockObject(WHITE_BRUSH));
        }
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// Whether only our own windows show over r (sampled densely). Another program in front, a game for one, must never be
// grabbed: the test window being topmost doesn't keep a fullscreen game below it.
bool OnlyOursOver(const RECT& r, DWORD alsoPid = 0) {
    const DWORD self = GetCurrentProcessId();
    for (int gy = 0; gy <= 16; ++gy)
        for (int gx = 0; gx <= 16; ++gx) {
            POINT pt{r.left + (r.right - 1 - r.left) * gx / 16, r.top + (r.bottom - 1 - r.top) * gy / 16};
            HWND h = WindowFromPoint(pt);
            DWORD pid = 0;
            if (h) GetWindowThreadProcessId(h, &pid);
            if (pid != self && (!alsoPid || pid != alsoPid)) return false;
        }
    return true;
}

// Pixels on screen (BGRX), for checking what DWM composited. Only ever our own window's area, and nothing when
// another program's window is over it (before or after the grab): empty then.
std::vector<uint32_t> GrabScreen(const RECT& r) {
    if (!OnlyOursOver(r)) return {};
    int w = r.right - r.left, h = r.bottom - r.top;
    std::vector<uint32_t> px((size_t)w * h);
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
    BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
    for (size_t i = 0; i < px.size(); ++i) px[i] = static_cast<uint32_t*>(bits)[i] | 0xFF000000u;
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    if (!OnlyOursOver(r)) return {};
    return px;
}

bool NearColor(uint32_t bgrx, COLORREF c, int tol) {
    return std::abs((int)(bgrx >> 16 & 255) - GetRValue(c)) <= tol && std::abs((int)(bgrx >> 8 & 255) - GetGValue(c)) <= tol &&
           std::abs((int)(bgrx & 255) - GetBValue(c)) <= tol;
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

// Every frame of a GIF as shown (RGB, w × h): frames drawn over the one before, transparent pixels left as they
// were (OzMaker writes disposal 1 only). Empty if the file can't be read.
std::vector<std::vector<uint8_t>> DecodeGif(const std::wstring& path, int& w, int& h) {
    std::vector<std::vector<uint8_t>> frames;
    std::vector<uint8_t> d;
    if (FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO)) {
        uint8_t buf[65536];
        for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) d.insert(d.end(), buf, buf + n);
        fclose(f);
    }
    if (d.size() < 13 || memcmp(d.data(), "GIF89a", 6) != 0) return frames;
    w = d[6] | d[7] << 8;
    h = d[8] | d[9] << 8;
    size_t p = 13;
    std::vector<uint8_t> global;
    if (d[10] & 0x80) {
        size_t n = 3u << ((d[10] & 7) + 1);
        global.assign(d.begin() + p, d.begin() + p + n);
        p += n;
    }
    std::vector<uint8_t> canvas((size_t)w * h * 3, 0);
    int transparent = -1;
    auto subBlocks = [&](std::vector<uint8_t>* out) {
        while (p < d.size() && d[p]) {
            if (out) out->insert(out->end(), d.begin() + p + 1, d.begin() + std::min(d.size(), p + 1 + d[p]));
            p += d[p] + 1;
        }
        ++p;
    };
    while (p < d.size()) {
        uint8_t b = d[p++];
        if (b == 0x3B) return frames;
        if (b == 0x21 && p < d.size()) {
            uint8_t label = d[p++];
            if (label == 0xF9 && p + 4 < d.size()) transparent = (d[p + 1] & 1) ? d[p + 4] : -1;
            subBlocks(nullptr);
            continue;
        }
        if (b != 0x2C || p + 9 > d.size()) return {};
        int fx = d[p] | d[p + 1] << 8, fy = d[p + 2] | d[p + 3] << 8, fw = d[p + 4] | d[p + 5] << 8,
            fh = d[p + 6] | d[p + 7] << 8, flags = d[p + 8];
        p += 9;
        std::vector<uint8_t> table = global;
        if (flags & 0x80) {
            size_t n = 3u << ((flags & 7) + 1);
            table.assign(d.begin() + p, d.begin() + p + n);
            p += n;
        }
        int minCode = d[p++];
        std::vector<uint8_t> data, idx;
        subBlocks(&data);
        // LZW
        const int clear = 1 << minCode, eoi = clear + 1;
        std::vector<int> prefix(4096), suffix(4096), first(4096), len(4096);
        int size = minCode + 1, next = eoi + 1, prev = -1;
        for (int i = 0; i < clear; ++i) prefix[i] = -1, suffix[i] = first[i] = i, len[i] = 1;
        uint32_t bits = 0;
        int nbits = 0;
        std::vector<uint8_t> str;
        for (size_t i = 0; i < data.size() || nbits >= size;) {
            while (nbits < size && i < data.size()) bits |= (uint32_t)data[i++] << nbits, nbits += 8;
            if (nbits < size) break;
            int code = bits & ((1 << size) - 1);
            bits >>= size;
            nbits -= size;
            if (code == clear) {
                size = minCode + 1;
                next = eoi + 1;
                prev = -1;
                continue;
            }
            if (code == eoi) break;
            int c = code;
            if (code >= next) {
                if (code > next || prev < 0) return {};
                c = prev;  // KwKwK: the code being defined, prev + its own first pixel
            }
            str.clear();
            for (int k = c; k >= 0; k = prefix[k]) str.push_back((uint8_t)suffix[k]);
            std::reverse(str.begin(), str.end());
            if (code >= next) str.push_back(str[0]);
            idx.insert(idx.end(), str.begin(), str.end());
            if (prev >= 0 && next < 4096) {
                prefix[next] = prev;
                suffix[next] = str[0];
                first[next] = first[prev];
                len[next] = len[prev] + 1;
                if (++next == (1 << size) && size < 12) ++size;
            }
            prev = code;
        }
        if (idx.size() < (size_t)fw * fh) return {};
        for (int y = 0; y < fh; ++y)
            for (int x = 0; x < fw; ++x) {
                int v = idx[(size_t)y * fw + x];
                if (v == transparent || fx + x >= w || fy + y >= h || (size_t)v * 3 + 2 >= table.size()) continue;
                uint8_t* c = canvas.data() + ((size_t)(fy + y) * w + fx + x) * 3;
                c[0] = table[v * 3];
                c[1] = table[v * 3 + 1];
                c[2] = table[v * 3 + 2];
            }
        frames.push_back(canvas);
        transparent = -1;
    }
    return frames;
}

// A stretch of video as a camera or a video file gives it (BGRA): a soft gradient with a few shapes and a striped
// "sign", grain in every frame (±3), and a square moving across. clean: the same without the grain.
void DrawGrainy(uint8_t* p, int w, int h, int t, bool clean, int amp = 3) {
    const int sx = 20 + t * 6, sy = h / 2 - 20;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int r = 60 + x * 120 / w, g = 80 + y * 100 / h, b = 120 + (x + y) * 60 / (w + h);
            if (x > w * 3 / 5 && y < h / 3) r = 200, g = 70, b = 60;                        // a red shape
            if (y > h * 3 / 4 && (x / 5) % 2 == 0 && x < w / 2) r = g = b = 225;             // stripes
            if (std::hypot(x - w * 0.8, y - h * 0.75) < h / 8.0) r = 240, g = 210, b = 90;  // a yellow disc
            if (x >= sx && x < sx + 40 && y >= sy && y < sy + 40) r = 30, g = 200, b = 230;  // the moving square
            if (!clean) {
                uint32_t n = (uint32_t)(x * 73856093u ^ y * 19349663u ^ (uint32_t)t * 83492791u);
                n = n * 1103515245u + 12345u;
                r += (int)(n >> 8) % (2 * amp + 1) - amp;
                g += (int)(n >> 12) % (2 * amp + 1) - amp;
                b += (int)(n >> 16) % (2 * amp + 1) - amp;
            }
            uint8_t* q = p + ((size_t)y * w + x) * 4;
            q[0] = (uint8_t)std::clamp(b, 0, 255);
            q[1] = (uint8_t)std::clamp(g, 0, 255);
            q[2] = (uint8_t)std::clamp(r, 0, 255);
            q[3] = 255;
        }
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

// The updater end to end on files in dir: a fake release folder (OZMAKER_UPDATE_URL) and a copy standing in for
// the installed exe. Never touches this exe. Returns the number of failures. base: serve the release folder from
// there instead (an HTTP server pointed at dir\release) to go through WinHTTP.
int UpdaterChecks(const std::wstring& dir, Log& log, const std::wstring& base = {}) {
    int failures = 0;
    auto check = [&](bool ok, const std::wstring& what) {
        log((ok ? L"update: ok   " : L"update: FAIL ") + what);
        if (!ok) ++failures;
    };
    using U = Updater;
    check(U::ParseVersion(L"0.4.1") < U::ParseVersion(L"0.4.10") && U::ParseVersion(L"0.10.0") > U::ParseVersion(L"0.9.9") &&
              U::ParseVersion(L"1.0.0") > U::ParseVersion(L"0.99.99"),
          L"version order");
    check(!U::ParseVersion(L"1.0") && !U::ParseVersion(L"1.0.0.0") && !U::ParseVersion(L"v1.0.0") &&
              !U::ParseVersion(L"") && !U::ParseVersion(L"1..0") && !U::ParseVersion(L"1.0.") && !U::ParseVersion(L"1.0.x"),
          L"malformed versions rejected");
    check(U::ParseVersion(OZ_VERSION_WSTR) == U::CurrentVersion(), L"current version " OZ_VERSION_WSTR);
    check(U::Sha256("abc", 3) == L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", L"SHA-256 test vector");

    const std::wstring rel = dir + L"\\release", inst = dir + L"\\install", exe = inst + L"\\OzMaker.exe";
    util::EnsureDir(rel);
    util::EnsureDir(inst);
    auto write = [](const std::wstring& path, const void* p, size_t n) {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD w = 0;
        bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, p, (DWORD)n, &w, nullptr) && w == n;
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        return ok;
    };
    auto sameAs = [](const std::wstring& path, const std::vector<uint8_t>& want) {
        uint64_t n = 0;
        return U::Sha256File(path, &n) == U::Sha256(want.data(), want.size()) && n == want.size();
    };
    auto exists = [](const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; };
    std::vector<uint8_t> oldExe(300'000), newExe(1'234'567);
    for (size_t i = 0; i < oldExe.size(); ++i) oldExe[i] = (uint8_t)(i * 7);
    for (size_t i = 0; i < newExe.size(); ++i) newExe[i] = (uint8_t)(i * 13 + (i >> 10));
    const std::wstring sha = U::Sha256(newExe.data(), newExe.size());
    write(exe, oldExe.data(), oldExe.size());
    write(rel + L"\\OzMaker.exe", newExe.data(), newExe.size());
    auto info = [&](const std::string& version, const std::wstring& sum, size_t size) {
        std::string s = "; test release\r\nversion=" + version + "\r\nexe=OzMaker.exe\r\nsize=" + std::to_string(size) +
                        "\r\nsha256=" + util::WideToUtf8(sum) + "\r\n";
        write(rel + L"\\update.txt", s.data(), s.size());
    };
    for (const wchar_t* left : {L".new", L".new.part", L".old"}) DeleteFileW((exe + left).c_str());
    DeleteFileW((rel + L"\\update.txt").c_str());
    SetEnvironmentVariableW(L"OZMAKER_UPDATE_URL", base.empty() ? rel.c_str() : base.c_str());
    auto run = [&](const std::wstring& target) {
        U u;
        u.Init(target, nullptr, 0);
        u.Check();
        for (int i = 0; i < 1500 && u.Busy(); ++i) Sleep(20);
        return u.Get();
    };

    U::Status st = run(exe);
    check(st.state == U::State::Failed && !exists(exe + L".new"), L"no update.txt: failed, nothing downloaded");
    info(OZ_VERSION_STR, sha, newExe.size());
    st = run(exe);
    check(st.state == U::State::UpToDate, L"same version: up to date");
    write(rel + L"\\update.txt", "version=99.0.0\r\nexe=../x.exe\r\n", 30);
    st = run(exe);
    check(st.state == U::State::Failed, L"bad update.txt: failed");
    info("99.0.0", std::wstring(64, L'0'), newExe.size());
    st = run(exe);
    check(st.state == U::State::Failed && !exists(exe + L".new") && !exists(exe + L".new.part") && sameAs(exe, oldExe),
          L"wrong SHA-256: failed, nothing kept: " + st.error);
    info("99.0.0", sha, newExe.size() - 1);
    st = run(exe);
    check(st.state == U::State::Failed && !exists(exe + L".new"), L"wrong size: failed");
    info("99.0.0", sha, newExe.size());
    st = run(inst + L"\\missing\\OzMaker.exe");
    check(st.state == U::State::Available && st.version == L"99.0.0", L"folder not writable: available, not downloaded");
    st = run(exe);
    check(st.state == U::State::Ready && st.version == L"99.0.0" && sameAs(exe + L".new", newExe),
          L"newer version: downloaded and verified");

    {
        U u;
        u.Init(exe, nullptr, 0);
        u.Check();
        for (int i = 0; i < 1500 && u.Busy(); ++i) Sleep(20);
        check(u.Get().state == U::State::Ready, L"second check reuses the download");
        std::wstring err;
        bool ok = u.Install(err);
        check(ok && sameAs(exe, newExe) && sameAs(exe + L".old", oldExe) && !exists(exe + L".new"),
              L"install swaps the exe, keeps .old " + err);
        U::CleanUp(exe);
        check(!exists(exe + L".old") && sameAs(exe, newExe), L"clean-up removes .old");
    }
    {
        // A staged file that changed after it was checked is not installed.
        write(exe, oldExe.data(), oldExe.size());
        U u;
        u.Init(exe, nullptr, 0);
        u.Check();
        for (int i = 0; i < 1500 && u.Busy(); ++i) Sleep(20);
        write(exe + L".new", oldExe.data(), oldExe.size());
        std::wstring err;
        bool ok = u.Install(err);
        check(!ok && sameAs(exe, oldExe) && !exists(exe + L".old"), L"tampered download refused: " + err);
    }
    SetEnvironmentVariableW(L"OZMAKER_UPDATE_URL", nullptr);
    return failures;
}

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

// The made-up game in the README pictures (--readme-media), drawn at 320×180 and scaled up by whole pixels. Flat
// colours and a still background, with only the hero, the stars and the counter moving, keep GIF frames small. The
// hero runs on the spot and jumps once a second for the star coming at it.
constexpr int kDemoW = 320, kDemoH = 180;

struct DemoCanvas {
    int w, h;
    std::vector<uint32_t> px;  // BGRA
    DemoCanvas(int w, int h, uint32_t fill) : w(w), h(h), px((size_t)w * h, fill) {}
    void Box(int x0, int y0, int x1, int y1, uint32_t c) {
        x0 = std::max(x0, 0), y0 = std::max(y0, 0), x1 = std::min(x1, w), y1 = std::min(y1, h);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) px[(size_t)y * w + x] = c;
    }
    // The top half of an ellipse standing on `base`: a hill.
    void Hill(int cx, int base, int rx, int ry, uint32_t c) {
        for (int y = base - ry; y < base; ++y) {
            double dy = (base - y) / (double)ry;
            int hw = (int)(rx * std::sqrt(std::max(0.0, 1 - dy * dy)));
            Box(cx - hw, y, cx + hw, y + 1, c);
        }
    }
    void Diamond(int cx, int cy, int r, uint32_t c) {
        for (int dy = -r; dy <= r; ++dy) {
            int hw = r - std::abs(dy);
            Box(cx - hw, cy + dy, cx + hw + 1, cy + dy + 1, c);
        }
    }
    // A 3×5 digit, s pixels to a dot.
    void Digit(int x, int y, int d, int s, uint32_t c) {
        static const uint16_t font[10] = {0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7292, 0x7BEF, 0x7BCF};
        for (int i = 0; i < 15; ++i)
            if (font[d] >> (14 - i) & 1) Box(x + i % 3 * s, y + i / 3 * s, x + i % 3 * s + s, y + i / 3 * s + s, c);
    }
    // The hero: an orange block with one eye, 16×16 from its top left.
    void Hero(int x, int y) {
        const uint32_t body = 0xFFFF8A3D, shade = 0xFFE06A20, ink = 0xFF1D2433;
        Box(x + 1, y, x + 15, y + 16, body);
        Box(x, y + 1, x + 16, y + 15, body);
        Box(x + 13, y + 2, x + 15, y + 15, shade);
        Box(x + 2, y + 13, x + 15, y + 15, shade);
        Box(x + 10, y + 4, x + 13, y + 9, ink);
    }
};

std::vector<uint32_t> DemoFrame(double t) {
    const uint32_t ink = 0xFF1D2433;
    const int ground = 150;
    DemoCanvas c(kDemoW, kDemoH, 0xFF4FB3E8);   // sky
    c.Box(280, 14, 298, 32, 0xFFFFE27A);         // sun
    const int clouds[2][2] = {{58, 36}, {196, 26}};
    for (const auto& cl : clouds) {
        c.Box(cl[0] - 22, cl[1] - 4, cl[0] + 22, cl[1] + 4, 0xFFF4FBFF);
        c.Box(cl[0] - 12, cl[1] - 9, cl[0] + 10, cl[1] - 3, 0xFFF4FBFF);
    }
    c.Hill(250, ground, 86, 48, 0xFF5DB45C);
    c.Hill(64, ground, 104, 38, 0xFF6CC36A);
    c.Box(0, ground, kDemoW, ground + 5, 0xFF4FA54A);  // grass
    c.Box(0, ground + 5, kDemoW, kDemoH, 0xFF8A5A3B);  // earth
    for (int y = ground + 11; y < kDemoH; y += 10)
        for (int x = y * 7 % 23; x < kDemoW; x += 23) c.Box(x, y, x + 4, y + 2, 0xFF7A4E33);

    // A jump takes 0.6 s of every second; the star of jump k meets the hero at its top, at k + kMeet.
    const int hx = 72, rise = 46, speed = 110;
    const double kUp = 0.15, kLen = 0.6, kMeet = kUp + kLen / 2;
    double v = (t - std::floor(t) - kUp) / kLen;
    int lift = v > 0 && v < 1 ? (int)std::lround(rise * 4 * v * (1 - v)) : 0;
    int hy = ground - 18 - lift;
    const int starY = ground - 10 - rise;
    int caught = t >= kMeet ? (int)std::floor(t - kMeet) + 1 : 0;
    for (int k = caught; k < caught + 4; ++k) {
        int sx = hx + 8 + (int)std::lround(speed * (k + kMeet - t));
        c.Diamond(sx, starY, 5, 0xFFE0A800);
        c.Diamond(sx, starY, 3, 0xFFFFD23F);
    }
    if (double since = t - (caught - 1 + kMeet); caught > 0 && since < 0.25) {
        int d = 7 + (int)(since * 40);  // a sparkle where it was caught
        const int dirs[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
        for (const auto& s : dirs) {
            int x = hx + 8 + s[0] * d, y = starY + s[1] * d;
            c.Box(x - 1, y - 1, x + 1, y + 1, 0xFFFFFFFF);
        }
    }
    c.Hero(hx, hy);
    if (!lift) {  // running: the feet take turns
        bool step = (int)(t * 8) % 2;
        c.Box(hx + (step ? 2 : 4), hy + 16, hx + (step ? 6 : 8), hy + 18, ink);
        c.Box(hx + (step ? 10 : 8), hy + 16, hx + (step ? 14 : 12), hy + 18, ink);
    } else {
        c.Box(hx + 3, hy + 16, hx + 7, hy + 18, ink);
        c.Box(hx + 9, hy + 16, hx + 13, hy + 18, ink);
    }
    // The star counter, top left.
    c.Diamond(14, 14, 5, 0xFFE0A800);
    c.Diamond(14, 14, 3, 0xFFFFD23F);
    int n = 12 + caught;
    for (int i = 0; i < 3; ++i) {
        int d = n / (i == 0 ? 100 : i == 1 ? 10 : 1) % 10;
        c.Digit(25 + i * 8 + 1, 9 + 1, d, 2, ink);
        c.Digit(25 + i * 8, 9, d, 2, 0xFFFFFFFF);
    }
    return std::move(c.px);
}

// The scene scaled to w×h, nearest pixel.
void DrawDemoScene(uint8_t* p, int w, int h, double t) {
    auto src = DemoFrame(t);
    auto* d = reinterpret_cast<uint32_t*>(p);
    for (int y = 0; y < h; ++y) {
        const uint32_t* row = src.data() + (size_t)(y * kDemoH / h) * kDemoW;
        for (int x = 0; x < w; ++x) d[(size_t)y * w + x] = row[x * kDemoW / w];
    }
}

// The game's icon in the window list: the hero, 32×32, clear around it.
std::vector<uint32_t> DemoIcon() {
    DemoCanvas c(16, 16, 0);
    c.Hero(0, 0);
    std::vector<uint32_t> out(32 * 32);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) out[y * 32 + x] = c.px[y / 2 * 16 + x / 2];
    return out;
}

// A PNG or JPG back as straight BGRA.
bool ReadImageFile(const std::wstring& path, std::vector<uint8_t>& px, int& w, int& h) {
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    UINT fw = 0, fh = 0;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(frame->GetSize(&fw, &fh)) ||
        FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    w = (int)fw;
    h = (int)fh;
    px.resize((size_t)fw * fh * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, fw * 4, (UINT)px.size(), px.data()));
}

// A still WebP back as straight BGRA (a 움짤 file needs two frames to open as a clip).
bool ReadWebpFile(const std::wstring& path, std::vector<uint8_t>& px, int& w, int& h, bool& alpha) {
#if __has_include(<webp/decode.h>)
    std::ifstream f(path, std::ios::binary);
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    WebPBitstreamFeatures ft;
    if (data.empty() || WebPGetFeatures(data.data(), data.size(), &ft) != VP8_STATUS_OK) return false;
    alpha = ft.has_alpha != 0;
    uint8_t* out = WebPDecodeBGRA(data.data(), data.size(), &w, &h);
    if (!out) return false;
    px.assign(out, out + (size_t)w * h * 4);
    WebPFree(out);
    return true;
#else
    return false;
#endif
}

// The largest difference of a colour byte between two BGRA pictures of the same size, the alpha left out.
int WorstColour(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size()) return 256;
    int worst = 0;
    for (size_t i = 0; i < a.size(); ++i)
        if (i % 4 != 3) worst = std::max(worst, std::abs((int)a[i] - (int)b[i]));
    return worst;
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

std::shared_ptr<FrameStore> App::MakeSyntheticClip(int w, int h, int frames, double fps, bool finish) {
    auto s = std::make_shared<FrameStore>();
    std::wstring err;
    if (!s->Open(w, h, err)) return nullptr;
    for (int i = 0; i < frames; ++i) {
        auto buf = s->Acquire();
        DrawSynthetic(buf->data(), w, h, i / fps);
        while (!s->Submit(buf, (int64_t)(i * 1e7 / fps))) Sleep(1);
    }
    if (finish) s->Finish((int64_t)(frames * 1e7 / fps));
    return s;
}

int App::RunSelfTest(const std::wstring& dir) {
    testWindowsOnly_ = true;
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

    // An encoder that is behind for a moment goes on; one that keeps refusing ends the recording (#69).
    {
        const int64_t s = 10'000'000;
        RefusalWatch w;
        bool early = false;
        for (int64_t t = s; t < 5 * s; t += s / 30) early |= w.Refused(t);
        const bool late = w.Refused(6 * s);
        w.Taken();
        const bool fresh = !w.Refused(7 * s);
        const bool slept = !w.Refused(3600 * s);  // refused before the PC slept and after it woke: nothing long
        const bool ok = !early && late && fresh && slept;
        if (!ok) ++failures;
        log(util::Format(L"refusals: %s (under the limit %d, over it %d, after a frame %d, after sleep %d)",
                         ok ? L"ok" : L"FAIL", !early, late, fresh, slept));
        const bool words = util::FormatSpan(45) == L"45초" && util::FormatSpan(38 * 60) == L"38분" &&
                           util::FormatSpan(80 * 60) == L"1시간 20분" && util::FormatSpan(3600) == L"1시간";
        if (!words) ++failures;
        log(words ? L"span words: ok" : L"FAIL span words");
    }

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
    // 재생 방향 (#22): backwards is the same frames the other way round; a boomerang goes there and back without
    // either end twice in a row, so its loop runs on smoothly.
    {
        auto back = PlanFrames(*clip, 10, 19, 0, 1.0, 1), boom = PlanFrames(*clip, 10, 19, 0, 1.0, 2);
        bool rev = back.size() == 10, there = boom.size() == 18;
        for (size_t k = 0; rev && k < back.size(); ++k) rev = back[k].index == 19 - k;
        for (size_t k = 0; there && k < boom.size(); ++k) there = boom[k].index == (k < 10 ? 10 + k : 28 - k);
        log(util::Format(L"plan backwards / boomerang: %s %zu / %zu frames", rev && there ? L"ok" : L"FAIL",
                         back.size(), boom.size()));
        if (!rev || !there) ++failures;
    }

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
        job.quality = fmt == Format::WebM ? settings_.webmQuality : f < (int)Format::Mp4 ? settings_.quality[f] : 0;
        if (fmt == Format::Gif) job.lossy = settings_.gifLossy;
        if (IsVideoFormat(fmt)) job.fpsLimit = fps;
        job.path = dir + L"\\selftest." + FormatExt(fmt);
        double s = util::NowSeconds();
        ExportResult r = ExportSync(ex, job);
        log(util::Format(L"%s: %s %s, %d frames, %.2fs, %.0f ms %s", FormatName(fmt), r.ok ? L"ok" : L"FAIL",
                         util::FormatBytes(r.bytes).c_str(), r.frames, r.seconds, (util::NowSeconds() - s) * 1000,
                         r.error.c_str()));
        // The size guess against the real thing. MP4 is VBR (this test picture needs far less than the bitrate),
        // so only 움짤 and WebM (by quality, guessed from real encodes) must be close.
        {
            std::atomic<bool> cancel{false};
            s = util::NowSeconds();
            uint64_t est = EstimateBytes(job, cancel);
            double ratio = r.bytes ? est / (double)r.bytes : 0;
            bool good = fmt == Format::Mp4 ? est > 0 : ratio > 0.6 && ratio < 1.6;
            log(util::Format(L"%s estimate: %s %s vs %s (x%.2f), %.0f ms", FormatName(fmt), good ? L"ok" : L"FAIL",
                             util::FormatBytes(est).c_str(), util::FormatBytes(r.bytes).c_str(), ratio,
                             (util::NowSeconds() - s) * 1000));
            if (!good) ++failures;
        }
        if (!r.ok) ++failures;
        // WebM read back (#40): every frame there, at the size written.
        if (fmt == Format::WebM && r.ok && WebmReaderBuilt()) {
            WebmContents wc = ReadWebm(job.path);
            bool good = wc.ok && (int)wc.indexes.size() == r.frames && wc.width == W && wc.height == H &&
                        std::fabs(wc.seconds - r.frames / fps) < 0.002;
            log(util::Format(L"WebM read back: %s %zu frames %dx%d, %.3fs %s", good ? L"ok" : L"FAIL",
                             wc.indexes.size(), wc.width, wc.height, wc.seconds, wc.err.c_str()));
            if (!good) ++failures;
        }
        // AVIF from quality 90 keeps the colour at full resolution (4:4:4, #23).
        if (fmt == Format::Avif) {
            job.quality = 95;
            job.path = dir + L"\\selftest_444.avif";
            ExportResult hi = ExportSync(ex, job);
            bool good = hi.ok && hi.bytes > r.bytes;
            log(util::Format(L"AVIF 4:4:4: %s %s at quality 95 (%s at %d) %s", good ? L"ok" : L"FAIL",
                             util::FormatBytes(hi.bytes).c_str(), util::FormatBytes(r.bytes).c_str(),
                             settings_.quality[f], hi.error.c_str()));
            if (!good) ++failures;
        }
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
    // 움짤 files opened in the editor (#41): a clip with see-through parts saved as GIF, WebP and AVIF and read back
    // (AnimClip): as many frames, as long each, still see-through where it was (where the square was a frame before
    // too: GIF clears it), colours close. As MP4 the clear parts are white.
    {
        const int aw = 96, ah = 64, an = 6;
        const double lens[an] = {100, 40, 250, 70, 120, 500};
        auto store = std::make_shared<FrameStore>();
        std::wstring err;
        bool made = store->Open(aw, ah, err, false);
        int64_t ts = 0;
        for (int i = 0; made && i < an; ++i) {
            auto buf = store->Acquire();
            DrawAlphaFrame(buf->data(), aw, ah, i);
            made = store->Submit(buf, ts, true);
            ts += std::llround(lens[i] * 1e4);
        }
        store->Finish(ts);
        auto aclip = std::make_shared<AlphaClip>(store);
        for (Format fmt : {Format::Gif, Format::WebP, Format::Avif, Format::Mp4, Format::WebM}) {
            if (!FormatAvailable(fmt) || (fmt == Format::Mp4 && !HasVideoProcessor())) continue;
            ExportJob job;
            job.clip = aclip;
            job.last = an - 1;
            job.crop = {0, 0, aw, ah};
            job.outW = aw;
            job.outH = ah;
            job.format = fmt;
            job.quality = fmt == Format::Mp4 ? 0 : 90;
            if (IsVideoFormat(fmt)) job.fpsLimit = 50;
            job.path = dir + L"\\selftest_alpha." + FormatExt(fmt);
            ExportResult r = ExportSync(ex, job);
            std::wstring got;
            bool ok = r.ok;
            if (ok && fmt == Format::Mp4) {
                auto v = VideoClip::Open(job.path, err);
                auto rd = v ? v->NewReader() : nullptr;
                const uint8_t* px = rd ? rd->Get(0) : nullptr;
                const uint8_t* corner = px ? px + ((size_t)(ah - 1) * aw + aw - 1) * 4 : nullptr;
                ok = corner && corner[0] > 235 && corner[1] > 235 && corner[2] > 235;
                if (corner) got = util::Format(L"clear corner %d,%d,%d", corner[2], corner[1], corner[0]);
            } else if (ok && fmt == Format::WebM) {
                WebmContents wc = ReadWebm(job.path);
                const uint8_t* corner =
                    wc.ok && wc.width == aw && wc.height == ah ? wc.first.data() + ((size_t)(ah - 1) * aw + aw - 1) * 4
                                                               : nullptr;
                ok = !WebmReaderBuilt() || (corner && corner[0] > 235 && corner[1] > 235 && corner[2] > 235);
                got = corner ? util::Format(L"clear corner %d,%d,%d", corner[2], corner[1], corner[0]) : wc.err;
            } else if (ok) {
                std::atomic<bool> cancel{false};
                std::atomic<float> progress{0};
                auto back = AnimClip::Open(job.path, cancel, progress, err);
                ok = back && back->Count() == (size_t)an && back->HasAlpha() && back->FileFormat() == fmt &&
                     back->Width() == aw && back->Height() == ah;
                got = back ? util::Format(L"%zu frames, alpha %d", back->Count(), back->HasAlpha()) : err;
                auto rd = ok ? back->NewReader() : nullptr;
                for (int i = 0; ok && i < an; ++i) {
                    const int64_t end = i + 1 < an ? back->Ts(i + 1) : back->EndTs();
                    const double ms = (end - back->Ts(i)) / 1e4;
                    const uint8_t* px = rd->Get(i);
                    auto at = [&](int x, int y) { return px + ((size_t)y * aw + x) * 4; };
                    // In this frame's square; in the last one's but not this one's.
                    const uint8_t *sq = at(20 + i * 12, 32), *was = at(i * 12, 32), *band = at(aw - 4, 3);
                    const bool square = i == 3 ? sq[3] == 0 : sq[3] > 240 && sq[2] > 200 && sq[1] < 60;
                    const bool cleared = i == 0 || was[3] == 0;
                    // The band: GIF keeps it (200 ≥ half), the others keep it half clear.
                    const bool half = i == 3 ? band[3] == 0
                                      : fmt == Format::Gif ? band[3] == 255
                                                           : band[3] > 170 && band[3] < 230;
                    if (!px || std::fabs(ms - lens[i]) > 10 || !square || !cleared || !half) {
                        got += util::Format(L" · frame %d: %.0f ms, square a%d r%d g%d, behind a%d, band a%d", i, ms,
                                            sq[3], sq[2], sq[1], was[3], band[3]);
                        ok = false;
                    }
                }
            }
            log(util::Format(L"%s with alpha: %s %s · %s %s", FormatName(fmt), ok ? L"ok" : L"FAIL",
                             util::FormatBytes(r.bytes).c_str(), got.c_str(), r.error.c_str()));
            if (!ok) ++failures;
        }
        // In the editor it starts in its own format at 고화질, while the 움짤 recordings keep their 화질 (저용량 here),
        // in the settings file too, and get it back when it closes. Saved as a GIF again, its name gets _trim.
        std::atomic<bool> cancel{false};
        std::atomic<float> progress{0};
        if (auto gif = AnimClip::Open(dir + L"\\selftest_alpha.gif", cancel, progress, err)) {
            const Settings was = settings_;
            settings_.UseExportKind(false);
            settings_.exportPreset = 2;
            settings_.ApplyPreset(2, false);
            OpenEditor(gif);
            const bool own = fmt_ == (int)Format::Gif && settings_.exportPreset == 0 && settings_.scaleIdx == 0 &&
                             settings_.outFpsIdx == 0;
            settings_.Save();
            const int saved = (int)GetPrivateProfileIntW(L"OzMaker", L"ExportPreset", -9, Settings::FilePath().c_str());
            const std::wstring gifName = ExportName(Format::Gif), webpName = ExportName(Format::WebP);
            const bool named = gifName.rfind(L"selftest_alpha_trim", 0) == 0 && webpName.rfind(L"selftest_alpha", 0) == 0 &&
                               webpName.find(L"_trim") == std::wstring::npos;
            CloseClip();
            const bool back = settings_.exportPreset == 2 && !settings_.recordingAnim;
            const bool ok = own && saved == 2 && named && back;
            log(util::Format(L"opened 움짤 in the editor: %s own format at 고화질 %d, recordings' 화질 saved %d, back %d, "
                             L"names %s %s",
                             ok ? L"ok" : L"FAIL", own, saved, back, gifName.c_str(), webpName.c_str()));
            if (!ok) ++failures;
            StopThumbnails();
            settings_ = was;
            settings_.Save();
        } else {
            log(L"opened 움짤 in the editor: FAIL " + err);
            ++failures;
        }
    }
    // WebM sound (#40): one second of 30 fps video with 44.1 kHz sound, which goes to Opus at 48 kHz. Read back, every
    // frame is in order, the tone still steps from 440 to 880 Hz at 0.500 s, and the sound lasts as long as it did.
    if (FormatAvailable(Format::WebM) && WebmReaderBuilt()) {
        const int ww = 192, wh = 128, wn = 30;
        WebmWriter::Config wcfg;
        wcfg.width = ww;
        wcfg.height = wh;
        wcfg.fps = 30;
        wcfg.audio = true;
        wcfg.sampleRate = 44100;
        const std::wstring wpath = dir + L"\\selftest_sound.webm";
        WebmWriter w;
        std::wstring werr;
        bool ok = w.Open(wpath, wcfg, werr);
        std::vector<uint8_t> px((size_t)ww * wh * 4);
        std::vector<int16_t> pcm(1470 * 2);  // one frame's sound
        for (int i = 0; ok && i < wn; ++i) {
            DrawIndexed(px.data(), ww, wh, i, i / 30.0);
            ok = w.AddVideo(px.data(), werr);
            for (int k = 0; k < 1470; ++k) {
                const double t = (i * 1470 + k) / 44100.0;
                pcm[k * 2] = pcm[k * 2 + 1] = (int16_t)(12000 * std::sin(2 * 3.14159265358979 * (t < 0.5 ? 440 : 880) * t));
            }
            w.PushAudio(pcm.data(), 1470);
        }
        ok = ok && w.Finish(werr);
        WebmContents wc = ok ? ReadWebm(wpath) : WebmContents{};
        int wrong = 0;
        for (size_t i = 0; i < wc.indexes.size(); ++i) wrong += wc.indexes[i] != (int)i;
        const double step = ToneSwitch(wc.audio, 48000, 0.1, 440, 880), len = wc.audio.size() / 2 / 48000.0;
        ok = ok && wc.ok && wc.indexes.size() == (size_t)wn && !wrong && std::fabs(step - 0.5) < 0.02 &&
             std::fabs(len - 1.0) < 0.005;
        log(util::Format(L"WebM sound: %s %zu frames (%d out of place), 440->880 at %.3fs (0.500), %.4fs of sound, "
                         L"%.2fs long %s",
                         ok ? L"ok" : L"FAIL", wc.indexes.size(), wrong, step, len, wc.seconds,
                         (wc.err.empty() ? werr : wc.err).c_str()));
        if (!ok) ++failures;
    }
    // 가변 프레임 레이트 (#55). The plan as frames: a source frame that repeats is one frame, and a keyframe interval
    // starts a new one, which is a keyframe; constant frame rate is a frame a tick.
    {
        const std::vector<size_t> plan{0, 0, 0, 1, 1, 2, 2, 2, 2, 2, 2, 2, 3};
        std::wstring got;
        for (const VideoRun& r : PlanVideoRuns(plan, true, 4)) got += util::Format(L"%zu+%zu%s ", r.k, r.n, r.key ? L"K" : L"");
        const auto cfr = PlanVideoRuns(plan, false, 4);
        bool each = cfr.size() == plan.size();
        for (size_t k = 0; each && k < cfr.size(); ++k) each = cfr[k].k == k && cfr[k].n == 1 && !cfr[k].key;
        const bool ok = got == L"0+3K 3+1 4+1K 5+3 8+4K 12+1K " && each;
        log(util::Format(L"VFR plan: %s %s(keyframes every 4 ticks), constant: a frame a tick %d", ok ? L"ok" : L"FAIL",
                         got.c_str(), each));
        if (!ok) ++failures;
    }
    // A clip that changes at 0, 0.1, 0.2, 6.0 and 6.1 s and ends at 6.9, saved at 30 fps. As MP4 with keyframes every
    // second, read back: a frame where the picture changes and one a second while it is still, each a keyframe, at
    // the times written, the last one lasting to the end. As WebM (keyframes every 5 s): the same, with one still
    // frame at 5 s. Constant frame rate has 207 frames and is larger.
    struct VfrFrame {
        double t;
        int index;
        bool key;
    };
    const std::vector<VfrFrame> vfrMp4{{0, 0, true},   {0.1, 1, false}, {0.2, 2, false}, {1, 2, true},  {2, 2, true},
                                       {3, 2, true},   {4, 2, true},    {5, 2, true},    {6, 3, true},  {6.1, 4, false}};
    const std::vector<VfrFrame> vfrWebm{{0, 0, true}, {0.1, 1, false}, {0.2, 2, false},
                                        {5, 2, true}, {6, 3, false},   {6.1, 4, false}};
    // slow: the times as SetVideoTiming made them, that many times as long.
    auto readVfrMp4 = [&](const std::wstring& path, std::wstring& got, int slow = 1) {
        std::wstring err;
        auto v = VideoClip::Open(path, err);
        if (!v) {
            got = err;
            return false;
        }
        auto rd = v->NewReader();
        bool ok = v->Count() == vfrMp4.size() && std::llabs(v->EndTs() - 69'000'000 * slow) < 20'000;
        for (size_t i = 0; i < v->Count(); ++i) {
            const uint8_t* px = rd->Get(i);
            const int index = px ? ReadIndex(px, v->Width(), v->Height()) : -1;
            const bool key = v->KeyBefore(i) == i;
            got += util::Format(L"%.2f%s#%d ", v->Ts(i) / 1e7, key ? L"K" : L"", index);
            ok = ok && i < vfrMp4.size() && std::fabs(v->Ts(i) / 1e7 - vfrMp4[i].t * slow) < 0.002 &&
                 index == vfrMp4[i].index && key == vfrMp4[i].key;
        }
        got += util::Format(L"end %.3f", v->EndTs() / 1e7);
        return ok;
    };
    {
        auto store = std::make_shared<FrameStore>();
        std::wstring err;
        const double starts[] = {0, 0.1, 0.2, 6.0, 6.1};
        bool made = store->Open(320, 180, err, false);
        for (int i = 0; made && i < 5; ++i) {
            auto buf = store->Acquire();
            DrawIndexed(buf->data(), 320, 180, i, i / 10.0);
            made = store->Submit(buf, std::llround(starts[i] * 1e7), true);
        }
        store->Finish(69'000'000);
        ExportJob job;
        job.clip = store;
        job.last = 4;
        job.crop = {0, 0, 320, 180};
        job.outW = 320;
        job.outH = 180;
        job.fpsLimit = 30;
        job.video.gopFrames = 30;
        job.video.vfr = true;
        if (HasVideoProcessor()) {
            job.format = Format::Mp4;
            job.path = dir + L"\\selftest_vfr.mp4";
            ExportResult r = ExportSync(ex, job);
            std::wstring got;
            const bool read = r.ok && readVfrMp4(job.path, got);
            job.video.vfr = false;
            job.path = dir + L"\\selftest_cfr.mp4";
            ExportResult c = ExportSync(ex, job);
            job.video.vfr = true;
            const bool ok = read && r.frames == (int)vfrMp4.size() && c.ok && c.frames == 207 && r.bytes < c.bytes;
            log(util::Format(L"MP4 variable frame rate: %s %s · %d frames %s, constant %d frames %s %s", ok ? L"ok" : L"FAIL",
                             got.c_str(), r.frames, util::FormatBytes(r.bytes).c_str(), c.frames,
                             util::FormatBytes(c.bytes).c_str(), (r.error + c.error).c_str()));
            if (!ok) ++failures;
            // What Finish does to a variable frame rate MP4, on a copy: each frame twice as long, read back as such.
            // With a frame too few the file stays as it was.
            const std::wstring slow = dir + L"\\selftest_vfr_slow.mp4";
            std::vector<uint32_t> dur;
            for (size_t i = 0; i < vfrMp4.size(); ++i)
                dur.push_back((uint32_t)std::llround(((i + 1 < vfrMp4.size() ? vfrMp4[i + 1].t : 6.9) - vfrMp4[i].t) * 2e7));
            std::wstring why, gotSlow;
            const bool copied = r.ok && CopyFileW((dir + L"\\selftest_vfr.mp4").c_str(), slow.c_str(), FALSE);
            const uint64_t before = util::FileSize(slow);
            const std::vector<uint32_t> fewer(dur.begin(), dur.end() - 1);
            const bool refused = copied && !mp4::SetVideoTiming(slow, fewer, why) && util::FileSize(slow) == before;
            const std::wstring refusal = why;
            const bool set = copied && mp4::SetVideoTiming(slow, dur, why) && readVfrMp4(slow, gotSlow, 2);
            log(util::Format(L"MP4 frame times set afterwards: %s %s · a frame too few: refused %d (%s) %s",
                             set && refused ? L"ok" : L"FAIL", gotSlow.c_str(), refused, refusal.c_str(), why.c_str()));
            if (!set || !refused) ++failures;
        } else {
            log(L"MP4 variable frame rate: skipped (no GPU video processor)");
        }
        if (FormatAvailable(Format::WebM) && WebmReaderBuilt()) {
            job.format = Format::WebM;
            job.quality = 70;
            job.path = dir + L"\\selftest_vfr.webm";
            ExportResult r = ExportSync(ex, job);
            WebmContents wc = r.ok ? ReadWebm(job.path) : WebmContents{};
            std::wstring got;
            bool ok = wc.ok && wc.indexes.size() == vfrWebm.size() && wc.times.size() == vfrWebm.size() &&
                      std::fabs(wc.seconds - 6.9) < 0.002 && r.frames == (int)vfrWebm.size();
            for (size_t i = 0; i < wc.times.size() && i < wc.indexes.size(); ++i) {
                const bool key = std::find(wc.keys.begin(), wc.keys.end(), (int)i) != wc.keys.end();
                got += util::Format(L"%.2f%s#%d ", wc.times[i], key ? L"K" : L"", wc.indexes[i]);
                ok = ok && i < vfrWebm.size() && std::fabs(wc.times[i] - vfrWebm[i].t) < 0.002 &&
                     wc.indexes[i] == vfrWebm[i].index && key == vfrWebm[i].key;
            }
            log(util::Format(L"WebM variable frame rate: %s %send %.3f · %s %s %s", ok ? L"ok" : L"FAIL", got.c_str(),
                             wc.seconds, util::FormatBytes(r.bytes).c_str(), wc.err.c_str(), r.error.c_str()));
            if (!ok) ++failures;
        }
    }
    // Recording (#55): what the ticker writes, through each encoder here. Fragmented and made plain as recording does;
    // the sound still steps at 4.5 s, in the middle of the still part.
    if (HasVideoProcessor()) {
        for (int variant = 0; variant < 3; ++variant) {
            const bool hardware = variant == 0, frag = variant < 2;
            if (hardware && Mp4Writer::FindEncoder(false, true).empty()) {
                log(L"VFR recording (hardware): skipped (no hardware encoder)");
                continue;
            }
            const std::wstring path = dir + (hardware ? L"\\selftest_vfr_rec_hw.mp4" : frag ? L"\\selftest_vfr_rec_sw.mp4" : L"\\selftest_vfr_rec_swplain.mp4");
            std::wstring err, enc, got;
            const bool wrote = WriteVfrMp4(path, hardware, err, enc, frag);
            bool ok = wrote && readVfrMp4(path, got);
            double step = -1;
            if (wrote) {
                if (auto v = VideoClip::Open(path, err)) {
                    int rate = 0;
                    const std::vector<int16_t> st = ReadAllAudio(*v, 0, 6.9, rate);
                    step = ToneSwitch(st, rate, 0.5, 440, 880);
                }
                ok = ok && std::fabs(step - 4.5) < 0.02;
            }
            if (!frag) enc += L" plain";
            // Cut without encoding (#22) from the keyframe at 2 s to the frame at 6.0: as long each, the sound along.
            if (ok && variant == 1) {
                std::wstring cut = dir + L"\\selftest_vfr_cut.mp4", why;
                std::atomic<bool> stop{false};
                std::atomic<float> progress{0};
                mp4::CutSource::Result cres;
                auto src = mp4::CutSource::Open(path);
                const bool made = src && src->KeyBefore(4) == 4 && src->Cut(4, 8, true, cut, stop, progress, cres, why);
                auto out = made ? VideoClip::Open(cut, why) : nullptr;
                std::wstring seen;
                bool cutOk = out && out->Count() == 5 && out->HasAudio() && std::llabs(out->EndTs() - 41'000'000) < 20'000;
                if (out) {
                    auto rd = out->NewReader();
                    for (size_t i = 0; i < out->Count(); ++i) {
                        const uint8_t* px = rd->Get(i);
                        const int index = px ? ReadIndex(px, out->Width(), out->Height()) : -1;
                        seen += util::Format(L"%.2f#%d ", out->Ts(i) / 1e7, index);
                        cutOk = cutOk && std::fabs(out->Ts(i) / 1e7 - (double)i) < 0.002 && index == (i < 4 ? 2 : 3);
                    }
                    seen += util::Format(L"end %.3f", out->EndTs() / 1e7);
                }
                log(util::Format(L"VFR recording cut as it is: %s %s %s", cutOk ? L"ok" : L"FAIL", seen.c_str(), why.c_str()));
                if (!cutOk) ++failures;
            }
            log(util::Format(L"VFR recording (%s): %s %s · 440->880 at %.3fs (4.500) · %s %s",
                             hardware ? L"hardware" : L"software", ok ? L"ok" : L"FAIL", got.c_str(), step, enc.c_str(),
                             err.c_str()));
            if (!ok) ++failures;
        }
    }
    // 순간 리플레이 (#20): the memory ring and the file saved from it.
    if (HasVideoProcessor()) {
        if (!Mp4Writer::FindEncoder(false, true).empty()) failures += ReplayRingCheck(dir, true, log);
        failures += ReplayRingCheck(dir, false, log);
    }
    // Desktop duplication (#20) leaves the cursor out: it is drawn through GDI, the arrow and the inverting text
    // cursor. Without a GDI-compatible texture no cursor is drawn (the recording goes on), so that only skips.
    {
        std::wstring detail;
        const bool ok = Capture::CursorDrawCheck(detail);
        const bool skip = !ok && (detail == L"no device" || detail == L"no GDI-compatible texture");
        log(util::Format(L"cursor drawn for desktop duplication: %s %s", ok ? L"ok" : skip ? L"skipped" : L"FAIL",
                         detail.c_str()));
        if (!ok && !skip) ++failures;
    }
    // Windows 11 leaves the yellow border off only for an app that asked (GraphicsCaptureAccess). The answer must
    // come; if it isn't "allowed", monitor captures go through desktop duplication as on Windows 10.
    {
        const int s = Capture::BorderlessAccess(5000);
        if (s == -2) {
            log(L"borderless capture: skipped (Windows 10: the border can't be turned off)");
        } else {
            log(util::Format(L"borderless capture: %s (status %d%s)", s >= 0 ? L"ok" : L"FAIL, no answer in 5 s", s,
                             s == 4 ? L", allowed" : L", not allowed: monitors go through duplication"));
            if (s < 0) ++failures;
        }
    }
    // GIF from grainy video (#23), light grain (±3) and heavier (±6): the size at each lossy level, how much of the
    // still background changes from frame to frame (the shimmer), and how close the frames stay to the picture
    // without grain (PSNR). Read back with DecodeGif.
    for (int amp : {3, 6}) {
        const int gw = 320, gh = 180, gn = 30;
        std::vector<std::vector<uint8_t>> clean(gn, std::vector<uint8_t>((size_t)gw * gh * 4));
        std::vector<uint8_t> grainy((size_t)gw * gh * 4);
        for (int t = 0; t < gn; ++t) DrawGrainy(clean[t].data(), gw, gh, t, true);
        uint64_t bytes[3] = {};
        double flicker[3] = {}, psnr[3] = {};
        bool decoded = true;
        for (int level = 0; level < 3; ++level) {
            std::wstring path = dir + util::Format(L"\\selftest_grain%d_lossy%d.gif", amp, level);
            auto enc = CreateEncoder(Format::Gif);
            EncoderParams p;
            p.width = gw;
            p.height = gh;
            p.quality = 85;
            p.lossy = level;
            p.path = path;
            std::wstring err;
            bool ok = enc->Begin(p, err);
            for (int t = 0; t < gn && ok; ++t) {
                DrawGrainy(grainy.data(), gw, gh, t, false, amp);
                ok = enc->Add(grainy.data(), 1000.0 / 15, err);
            }
            ok = ok && enc->End(err);
            enc.reset();
            bytes[level] = util::FileSize(path);
            int dw = 0, dh = 0;
            auto frames = DecodeGif(path, dw, dh);
            if (!ok || dw != gw || dh != gh || frames.size() != (size_t)gn) {
                decoded = false;
                continue;
            }
            double se = 0;
            size_t moved = 0, still = 0;
            for (int t = 0; t < gn; ++t) {
                const uint8_t* c = clean[t].data();
                const uint8_t* f = frames[t].data();
                for (size_t i = 0, n = (size_t)gw * gh; i < n; ++i)
                    for (int k = 0; k < 3; ++k) {
                        double e = (double)f[i * 3 + k] - c[i * 4 + 2 - k];
                        se += e * e;
                    }
                if (t == 0) continue;
                // Still background: away from where the square was in this frame or the one before.
                const int x0 = 20 + (t - 1) * 6 - 3, x1 = 20 + t * 6 + 40 + 3, y0 = gh / 2 - 20 - 3, y1 = gh / 2 + 20 + 3;
                for (int y = 0; y < gh; ++y)
                    for (int x = 0; x < gw; ++x) {
                        if (x >= x0 && x < x1 && y >= y0 && y < y1) continue;
                        size_t i = ((size_t)y * gw + x) * 3;
                        ++still;
                        moved += memcmp(frames[t].data() + i, frames[t - 1].data() + i, 3) != 0;
                    }
            }
            flicker[level] = 100.0 * moved / std::max<size_t>(1, still);
            psnr[level] = 10 * std::log10(255.0 * 255.0 / (se / ((double)gw * gh * 3 * gn)));
        }
        // Before #23 the light grain flickered on about half the still background (54 %, 936 KB at quality 85).
        bool good = decoded && flicker[0] < (amp <= 3 ? 1.0 : 20.0) && bytes[1] < bytes[0] * 0.9 &&
                    bytes[2] < bytes[1] && psnr[0] > (amp <= 3 ? 39.0 : 34.0) && psnr[1] > psnr[0] - 2 &&
                    psnr[2] > psnr[0] - 5;
        if (!good) ++failures;
        log(util::Format(L"gif grain ±%d: %s sizes %s / %s / %s (lossy off / light / strong), still background "
                         L"changing per frame %.2f%% / %.2f%% / %.2f%%, PSNR %.2f / %.2f / %.2f dB",
                         amp, good ? L"ok" : L"FAIL", util::FormatBytes(bytes[0]).c_str(),
                         util::FormatBytes(bytes[1]).c_str(), util::FormatBytes(bytes[2]).c_str(), flicker[0],
                         flicker[1], flicker[2], psnr[0], psnr[1], psnr[2]));
    }
    // Loop count and the hold on the last frame, read back from the files.
    for (int plays : {0, 1, 3}) {
        ExportJob job;
        job.clip = clip;
        job.first = 0;
        job.last = 29;
        job.crop = {0, 0, W, H};
        job.outW = W / 4;
        job.outH = H / 4;
        job.fpsLimit = FormatMaxFps(Format::Gif);
        job.format = Format::Gif;
        job.plays = plays;
        job.holdLastMs = plays == 3 ? kHoldLastMs : 0;
        job.path = dir + util::Format(L"\\selftest_plays%d.gif", plays);
        ExportResult r = ExportSync(ex, job);
        int loops = GifLoopCount(job.path), cs = GifDurationCs(job.path);
        int wantLoops = plays == 0 ? 0 : plays == 1 ? -1 : plays - 1;
        double wantSec = 30 / fps + job.holdLastMs / 1000;
        bool good = r.ok && loops == wantLoops && std::fabs(cs / 100.0 - wantSec) < 0.03;
        log(util::Format(L"GIF plays %d: %s loop block %d, file plays %.2fs (want %.2fs)", plays, good ? L"ok" : L"FAIL",
                         loops, cs / 100.0, wantSec));
        if (!good) ++failures;
    }
    // 용량 맞추기: a target well under the full-size GIF makes the fit step size/frame rate down; the export with
    // what it picked must land under the target without shrinking far more than needed.
    {
        OpenEditor(MakeSyntheticClip(W, H, N, fps));
        playing_ = false;
        fmt_ = (int)Format::Gif;
        settings_.scaleIdx = settings_.outFpsIdx = 0;
        settings_.targetIdx = 1;
        fitTargetForTest_ = 120'000;
        double s = util::NowSeconds();
        UpdateEstimate();  // starts the fit
        while (fitting_ && util::NowSeconds() - s < 120) {
            Sleep(10);
            OnEstimate();  // no window to notify here: poll for the result
        }
        double fitMs = (util::NowSeconds() - s) * 1000;
        ExportJob job = MakeJob(settings_.scaleIdx, settings_.outFpsIdx, settings_.quality[0]);
        job.path = dir + L"\\selftest_fit.gif";
        ExportResult r = ExportSync(ex, job);
        bool good = !fitting_ && r.ok && r.bytes <= fitTargetForTest_ && r.bytes > fitTargetForTest_ / 4;
        log(util::Format(L"fit 120 KB: %s %s (estimate %s), %dx%d %.0ffps, %zu candidates, %.0f ms", good ? L"ok" : L"FAIL",
                         util::FormatBytes(r.bytes).c_str(), util::FormatBytes(estBytes_).c_str(), job.outW, job.outH,
                         job.fpsLimit, fitKnobs_.size(), fitMs));
        if (!good) ++failures;
        fitTargetForTest_ = 0;
        settings_.targetIdx = 0;
        CloseEditor();
        StopThumbnails();
        clip_.reset();
    }
    // Layering. DWM draws the live window preview above everything in the window, so what floats above the page
    // (popovers, toasts, tooltips) is cut out of it, and nothing else may be drawn over it.
    {
        // The cut: every pixel of the preview is in exactly one part, or under a hole.
        RECT whole{100, 50, 740, 410};
        std::vector<RECT> holes = {{600, 20, 900, 200}, {300, 380, 500, 470}, {350, 150, 420, 220}, {0, 0, 10, 10}};
        auto parts = util::SubtractRects(whole, holes);
        int bad = 0;
        for (LONG y = whole.top; y < whole.bottom; ++y)
            for (LONG x = whole.left; x < whole.right; ++x) {
                POINT pt{x, y};
                int in = 0;
                bool hole = false;
                for (const RECT& p : parts) in += PtInRect(&p, pt) ? 1 : 0;
                for (const RECT& h : holes) hole |= PtInRect(&h, pt) != FALSE;
                bad += hole ? in != 0 : in != 1;
            }
        log(util::Format(L"preview cut: %s %zu parts, %d pixels wrong", bad ? L"FAIL" : L"ok", parts.size(), bad));
        if (bad) ++failures;
    }
    {
        // Home with the files list open over the preview, a recent file's tooltip running past the list's edge, and
        // a toast: three floats, all of them clear of the preview's parts, and no layering errors.
        HWND standIn = MakeStandInWindow(inst_);
        target_ = standIn;
        screen_ = Screen::Home;
        settings_.source = kSourceWindow;
        settings_.output = kOutputClip;
        showVideo_ = false;
        dpi_ = 96;
        recentFiles_ = {{dir + L"\\아주 긴 폴더 이름으로 경로가 목록보다 넓어지는 경우\\OzMaker_20261102_142311.gif", 2'345'678,
                         300, false}};
        filesOpen_ = true;
        filesScroll_ = 0;
        double clock = util::NowSeconds();
        toast_ = L"파일을 복사했습니다. 메신저나 폴더에 붙여넣으세요.";
        toastUntil_ = clock + 60;
        UiInput in;
        auto frames = [&](int passes, const std::function<void()>& after = nullptr) {
            for (int pass = 0; pass < passes; ++pass) {
                clock += 0.05;
                gfx_.RenderToPng((dir + L"\\selftest_layers.png").c_str(), 1120, 780, 96, [&](ID2D1DeviceContext* dc) {
                    ui_.Begin(dc, in, clock);
                    thumbWanted_ = false;
                    thumbCells_.clear();
                    DrawFrame(Rect(0, 0, 1120, 780));
                    if (after) after();
                    ui_.End();
                });
            }
        };
        frames(1);
        // The pointer on the first recent file (the list sits under the files button).
        in.mx = filesBtn_.r - 300;
        in.my = filesBtn_.b + 8 + 8 + 30 + 26;
        frames(12);
        RECT dest, src;
        PreviewLayout(dest, src);
        auto parts = PreviewParts(dest);
        const auto& fl = ui_.Floats();
        int clash = 0;
        for (const Rect& f : fl) {
            RECT h{(LONG)std::floor(f.l), (LONG)std::floor(f.t), (LONG)std::ceil(f.r), (LONG)std::ceil(f.b)}, o;
            for (const RECT& p : parts) clash += IntersectRect(&o, &p, &h) ? 1 : 0;
        }
        RECT pop{}, o;
        if (!fl.empty()) pop = {(LONG)fl[0].l, (LONG)fl[0].t, (LONG)fl[0].r, (LONG)fl[0].b};
        bool over = IntersectRect(&o, &pop, &dest) != FALSE;
        bool tipOut = fl.size() == 3 && (fl[2].l < fl[0].l || fl[2].r > fl[0].r || fl[2].b > fl[0].b);
        bool good = thumbWanted_ && ui_.LayerErrors() == 0 && fl.size() == 3 && over && tipOut && clash == 0 &&
                    !parts.empty();
        log(util::Format(L"layers, files list + tooltip + toast: %s %zu floats (list over the preview %d, tooltip past "
                         L"the list %d), %zu preview parts, %d overlaps, %d layering errors",
                         good ? L"ok" : L"FAIL", fl.size(), over, tipOut, parts.size(), clash, ui_.LayerErrors()));
        if (!good) ++failures;
        // Drawing over the preview outside a float is caught.
        frames(1, [&] { ui_.Fill(thumbArea_.Inset(40), theme::Rec); });
        good = ui_.LayerErrors() > 0;
        log(util::Format(L"layers, page drawn over the preview: %s %d error(s) caught", good ? L"ok" : L"FAIL",
                         ui_.LayerErrors()));
        if (!good) ++failures;
        // A question in place of a MessageBox: it covers the window, the preview with it; Esc says no, Enter yes,
        // and yes runs afterwards (WM_APP_ANSWER), not in the frame.
        {
            filesOpen_ = false;
            toastUntil_ = 0;
            int yes = 0;
            auto ask = [&] {
                Ask({L"저장하지 않은 녹화가 1개 있습니다", L"종료하면 사라집니다. 남기려면 먼저 편집 탭에서 저장하세요.",
                     L"종료", true, [&] { ++yes; }});
            };
            ask();
            frames(8);
            PreviewLayout(dest, src);
            size_t left = PreviewParts(dest).size(), floats = ui_.Floats().size();
            in.keys = {VK_ESCAPE};
            frames(1);
            in.keys.clear();
            bool no = !question_ && !answer_ && yes == 0;
            ask();
            frames(3);
            in.keys = {VK_RETURN};
            frames(1);
            in.keys.clear();
            bool later = !question_ && answer_ && yes == 0;
            if (auto f = std::exchange(answer_, nullptr)) f();
            good = thumbWanted_ && ui_.LayerErrors() == 0 && floats >= 1 && left == 0 && no && later && yes == 1;
            log(util::Format(L"question: %s %zu preview parts left uncovered, %d layering errors, Esc no %d, "
                             L"Enter yes after the frame %d",
                             good ? L"ok" : L"FAIL", left, ui_.LayerErrors(), no, later && yes == 1));
            if (!good) ++failures;
        }
        // Files dragged over the window (OLE, as Explorer drags them): the whole window says what a drop would do,
        // the preview included. A video or a 움짤 (#41) drops, and opens once the drop has returned (WM_APP_OPEN).
        {
            std::wstring mp4 = dir + L"\\끌어오기.mp4", gif = dir + L"\\끌어오기.gif", txt = dir + L"\\끌어오기.txt";
            for (const std::wstring& f : {mp4, gif, txt}) {
                HANDLE h = CreateFileW(f.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
                if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            }
            auto dataFor = [](const std::wstring& path) {
                ComPtr<IShellItem> item;
                ComPtr<IDataObject> data;
                if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))))
                    item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
                return data;
            };
            const DWORD all = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
            DWORD effect = all;
            auto gifData = dataFor(gif), mp4Data = dataFor(mp4), txtData = dataFor(txt);
            auto target = MakeDropTarget();  // what CreateMainWindow registers (this test has no main window)
            bool ok = gifData && mp4Data && txtData;
            if (ok) ok = SUCCEEDED(target->DragEnter(txtData.Get(), MK_LBUTTON, {0, 0}, &effect)) &&
                         dragKind_ == DragKind::Other && effect == DROPEFFECT_NONE;
            const std::wstring txtEnter = util::Format(L"data %d/%d/%d, text kind %d effect %lu", !!gifData, !!mp4Data,
                                                       !!txtData, (int)dragKind_, effect);
            frames(3);
            const bool txtShown = ui_.LayerErrors() == 0 &&
                                  std::any_of(ui_.Floats().begin(), ui_.Floats().end(),
                                              [](const Rect& f) { return f.W() >= 1120 && f.H() >= 780; });
            target->DragLeave();
            const bool left = dragKind_ == DragKind::None;
            effect = all;
            if (ok) ok = SUCCEEDED(target->DragEnter(gifData.Get(), MK_LBUTTON, {0, 0}, &effect)) &&
                         dragKind_ == DragKind::Anim && effect == DROPEFFECT_COPY;
            effect = all;
            if (ok) ok = SUCCEEDED(target->Drop(gifData.Get(), MK_LBUTTON, {0, 0}, &effect));
            const bool gifDropped = ok && effect == DROPEFFECT_COPY && pendingOpen_ == gif;
            pendingOpen_.clear();  // an empty stand-in, not a 움짤
            effect = all;
            if (ok) ok = SUCCEEDED(target->DragEnter(mp4Data.Get(), MK_LBUTTON, {0, 0}, &effect)) &&
                         dragKind_ == DragKind::Video && effect == DROPEFFECT_COPY && dragName_ == L"끌어오기.mp4";
            frames(3);
            PreviewLayout(dest, src);
            const size_t uncovered = PreviewParts(dest).size();
            const bool clean = thumbWanted_ && ui_.LayerErrors() == 0;
            effect = all;
            if (ok) ok = SUCCEEDED(target->Drop(mp4Data.Get(), MK_LBUTTON, {0, 0}, &effect));
            const bool dropped = ok && effect == DROPEFFECT_COPY && dragKind_ == DragKind::None && pendingOpen_ == mp4;
            pendingOpen_.clear();  // an empty stand-in, not a video: nothing for WM_APP_OPEN to open
            good = ok && txtShown && left && gifDropped && clean && uncovered == 0 && dropped;
            log(util::Format(L"drag over the window: %s text refused with a cue %d, cue gone on leave %d, GIF dropped "
                             L"%d, video cue over the preview (%zu parts left) %d, the drop opens it afterwards %d%s",
                             good ? L"ok" : L"FAIL", txtShown, left, gifDropped, uncovered, clean, dropped,
                             good ? L"" : (L" (" + txtEnter + L")").c_str()));
            if (!good) ++failures;
            DeleteFileW(mp4.c_str());
            DeleteFileW(gif.c_str());
            DeleteFileW(txt.c_str());
        }
        // A failure (an Alert) stays until it is closed, in a float like any toast. Its button opens the settings
        // tab that fixes it; a plain toast still goes by itself.
        {
            filesOpen_ = false;
            screen_ = Screen::Home;
            const int tabWas = settings_.settingsTab;
            settings_.settingsTab = kTabGeneral;
            ui_.KeepRects(true);
            auto press = [&](UiId id) {
                const Rect* b = ui_.LastRect(id);
                if (!b) return false;
                in.mx = b->CX(), in.my = b->CY();
                in.pressed = in.down = true;
                frames(1);
                in.pressed = in.down = false;
                in.released = true;
                frames(1);
                in.released = false;
                return true;
            };
            Alert(L"영상 인코더를 시작할 수 없습니다. (0x8007000E) 하드웨어 인코딩을 끄거나 해상도를 낮춰 보세요.",
                  kTabVideo);
            frames(3);
            clock += 60;  // long past the time any toast stays
            frames(2);
            const bool stays = clock < toastUntil_ && ui_.LastRect(HashId("toast.act")) && ui_.LayerErrors() == 0;
            const bool opened = press(HashId("toast.act")) && screen_ == Screen::Settings &&
                                settings_.settingsTab == kTabVideo && clock >= toastUntil_;
            screen_ = Screen::Home;
            Alert(L"내보내기에 실패했습니다.");
            frames(3);
            const bool closed = press(HashId("toast.close")) && clock >= toastUntil_;
            // Home's record button lies under the alert: the clicks were the alert's only.
            const bool pageUntouched = rec_ == RecState::Idle;
            Toast(L"파일을 복사했습니다.", true);
            const bool goes = !toastSticky_ && toastUntil_ < clock + 60;
            good = stays && opened && closed && pageUntouched && goes;
            log(util::Format(L"alert: %s stays past a toast's time %d, its button opens 설정 › 동영상 %d, closes %d, "
                             L"the page under it untouched %d, a plain error toast still goes %d",
                             good ? L"ok" : L"FAIL", stays, opened, closed, pageUntouched, goes));
            if (!good) ++failures;

            // 설정 › 일반 › 파일 이름 › 직접 입력: picking it opens the field with the caret in it; a token's button
            // puts the token where the caret was (here the start) and gives the field the focus back.
            const std::wstring nameWas = settings_.nameTemplate;
            screen_ = Screen::Settings;
            settings_.settingsTab = kTabGeneral;
            settingsScroll_ = 0;
            settings_.nameTemplate = L"_{날짜}";
            nameCustom_ = true;
            ui_.Focus(HashId("set.nameText"));
            frames(2);
            const bool fieldShown = ui_.TextFocused();  // a field not drawn loses the focus
            in.keys = {VK_HOME};
            frames(1);
            in.keys.clear();
            const bool pressed = press(HashId("set.nameTok", kTokWindow));
            const std::wstring pattern = settings_.nameTemplate;
            const bool inserted = pressed && pattern == L"{창 이름}_{날짜}" && ui_.TextFocused();
            nameCustom_ = false;
            settings_.nameTemplate = kNameTemplates[0];
            frames(2);
            const bool presetCloses = !ui_.TextFocused();
            good = fieldShown && inserted && presetCloses;
            log(util::Format(L"file name field: %s 직접 입력 shows it focused %d, a token goes in at the caret %d, "
                             L"a preset hides it %d",
                             good ? L"ok" : L"FAIL", fieldShown, inserted, presetCloses));
            if (!inserted) log(L"  pattern: " + pattern);
            if (!good) ++failures;
            settings_.nameTemplate = nameWas;
            ui_.KeepRects(false);
            settings_.settingsTab = tabWas;
            screen_ = Screen::Home;
        }
        filesOpen_ = false;
        toastUntil_ = 0;
        recentFiles_.clear();
        target_ = nullptr;
        DestroyWindow(standIn);
    }
    failures += UpdaterChecks(dir + L"\\update", log);

    // File names (설정 › 일반 › 파일 이름): the tokens in either language, filled once (a window title that looks like
    // a token stays one), characters a name can't hold, an empty pattern, and {번호} past the numbers in the folder.
    {
        SYSTEMTIME t{};
        t.wYear = 2026, t.wMonth = 11, t.wDay = 3, t.wHour = 15, t.wMinute = 24, t.wSecond = 50;
        const bool tokens =
            FillName(L"{창 이름} {연}-{월}-{일} {시}.{분}.{초}", L"데모 게임", t, 1) == L"데모 게임 2026-11-03 15.24.50" &&
            FillName(L"{window}_{date}_{number}", L"데모 게임", t, 7) == L"데모 게임_20261103_152450_007";
        const bool once = FillName(L"{창 이름}", L"{date}", t, 1) == L"{date}";
        const bool unsafe = FillName(L"클립: {창 이름}?", L"a/b", t, 1) == L"클립_ a_b_";
        const bool empty = FillName(L"", L"데모 게임", t, 1) == L"OzMaker_20261103_152450";
        const std::wstring outWas = settings_.outDir, nameWas = settings_.nameTemplate;
        settings_.outDir = dir + L"\\names";
        util::EnsureDir(settings_.outDir);
        const wchar_t* made[] = {L"\\클립_001.gif", L"\\클립_002.mp4"};
        for (const wchar_t* f : made) {
            HANDLE h = CreateFileW((settings_.outDir + f).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
        settings_.nameTemplate = L"클립_{번호}";
        const bool number = FileStem(L"데모 게임") == L"클립_003" && FileStem(L"데모 게임", false) == L"클립_001";
        for (const wchar_t* f : made) DeleteFileW((settings_.outDir + f).c_str());
        RemoveDirectoryW(settings_.outDir.c_str());
        settings_.outDir = outWas;
        settings_.nameTemplate = nameWas;
        const bool good = tokens && once && unsafe && empty && number;
        log(util::Format(L"file names: %s tokens %d, filled once %d, unsafe characters %d, empty %d, {번호} past the "
                         L"folder's %d",
                         good ? L"ok" : L"FAIL", tokens, once, unsafe, empty, number));
        if (!good) ++failures;
    }

    // Mic mixing (#21), on made-up streams: frame n of each input adds up (the mic with its gain) once both have
    // it, an input left out never holds the other back, Flush pads the shorter one, loud sums clip.
    {
        std::vector<int16_t> out;
        auto collect = [&](const int16_t* p, size_t n) { out.insert(out.end(), p, p + n * 2); };
        auto mcheck = [&](bool ok, const wchar_t* what) {
            if (!ok) ++failures;
            log(util::Format(L"mix: %s %s", ok ? L"ok  " : L"FAIL", what));
        };
        AudioMix mix;
        mix.Reset(2, collect);
        mix.SetGain(1, 0.5f);
        std::vector<int16_t> sound(200, 1000), mic(100, 3000);  // 100 and 50 frames
        mix.Input(0)(sound.data(), 100);
        mcheck(out.empty(), L"waits for the other input");
        mix.Input(1)(mic.data(), 50);
        mcheck(out.size() == 100 && out.front() == 2500 && out.back() == 2500, L"adds up the frames both have, mic at 50 %");
        mix.Flush();
        mcheck(out.size() == 200 && out[100] == 1000 && out.back() == 1000, L"flush pads the shorter input");
        out.clear();
        mix.Reset(2, collect);
        mix.Drop(0);
        mix.Input(1)(mic.data(), 50);
        mcheck(out.size() == 100 && out.front() == 3000, L"an input left out doesn't hold back the other");
        out.clear();
        mix.Reset(2, collect);
        std::vector<int16_t> loud(2, 30000), low(2, -30000);
        mix.Input(0)(loud.data(), 1);
        mix.Input(1)(loud.data(), 1);
        mix.Input(0)(low.data(), 1);
        mix.Input(1)(low.data(), 1);
        mcheck(out.size() == 4 && out[0] == 32767 && out[2] == -32768, L"loud sums clip");
    }
    // The editor's preview volume (#36): the samples scale by the gain, ramped from the one before.
    {
        auto vcheck = [&](bool ok, const wchar_t* what) {
            if (!ok) ++failures;
            log(util::Format(L"preview volume: %s %s", ok ? L"ok  " : L"FAIL", what));
        };
        std::vector<int16_t> s{1000, -1000, 32767, -32768};
        AudioPreview::Scale(s.data(), 2, 0.25f, 0.25f);
        vcheck(s[0] == 250 && s[1] == -250 && s[2] == 8192 && s[3] == -8192, L"scales both channels");
        std::vector<int16_t> ramp(8, 1000);
        AudioPreview::Scale(ramp.data(), 4, 1.f, 0.f);
        vcheck(ramp[0] == 1000 && ramp[1] == 1000 && ramp[2] == 750 && ramp[4] == 500 && ramp[7] == 250,
               L"ramps from the last gain");
        std::vector<int16_t> full{1234, -1234};
        AudioPreview::Scale(full.data(), 1, 1.f, 1.f);
        vcheck(full[0] == 1234 && full[1] == -1234, L"100 % leaves the samples alone");
    }
    // OzMaker's own sounds stay out of a recording of everything that plays (the start cue was recorded), and the
    // mic check plays the mic back. A quiet made-up tone stands in for the mic: a loopback of OzMaker alone hears
    // it, the recording's source (everything but OzMaker) doesn't. Whatever else plays on this PC is in that
    // source too: measured at the tone's pitch before the tone, it can't be told apart when it is that loud.
    // Only levels are kept.
    if (!AudioCapture::ProcessLoopbackSupported()) {
        log(L"own sound: skipped (needs Windows 10 2004 or later)");
    } else {
        std::mutex m;
        std::vector<int16_t> self, others;
        auto into = [&m](std::vector<int16_t>& v) {
            return [&m, &v](const int16_t* p, size_t n) {
                std::lock_guard lk(m);
                v.insert(v.end(), p, p + n * 2);
            };
        };
        AudioMonitor mon;
        AudioCapture a, b;
        std::wstring err;
        const int64_t origin = util::Qpc100ns();
        bool started = mon.Start(48000, err) &&
                       a.Start(AudioCapture::Source::Process, GetCurrentProcessId(), 48000, origin, into(self), err) &&
                       b.Start(AudioCapture::Source::Others, 0, 48000, origin, into(others), err);
        if (!started) {
            log(L"own sound: skipped (no audio output: " + err + L")");
        } else {
            const double f = 3517;  // high, where music has little
            const size_t total = 48000 * 6 / 10;  // 0.6 s, pushed in 10 ms blocks a little ahead of real time
            Sleep(600);
            size_t mark;  // frames of the recording's source before the tone
            {
                std::lock_guard lk(m);
                mark = others.size() / 2;
            }
            std::vector<int16_t> block(480 * 2);
            double phase = 0, t0 = util::NowSeconds();
            for (size_t pushed = 0; pushed < total; Sleep(2)) {
                size_t due = (size_t)((util::NowSeconds() - t0) * 48000) + 960;
                for (; pushed < due && pushed < total; pushed += 480) {
                    for (size_t i = 0; i < 480; ++i) {
                        phase += 2 * 3.14159265358979 * f / 48000;
                        block[2 * i] = block[2 * i + 1] = (int16_t)(0.02 * std::sin(phase) * 32767);
                    }
                    mon.Push(block.data(), 480);
                }
            }
            Sleep(300);
            mon.Stop();
            a.Stop(0);
            b.Stop(0);
            auto loudest = [&](const std::vector<int16_t>& v, size_t from, size_t to) {  // frames
                double best = 0;
                for (size_t i = from; i + 19200 <= to && (i + 19200) * 2 <= v.size(); i += 2400)
                    best = std::max(best, ToneLevel(v, i, 19200, f, 48000));
                return best;
            };
            double heard = loudest(self, 0, self.size() / 2), before = loudest(others, 0, mark),
                   recorded = loudest(others, mark, others.size() / 2);
            // Without the exclusion the recording hears the tone as loudly as OzMaker's own loopback does.
            bool ok = heard > 0.005 && recorded < heard * 0.5;
            if (heard > 0.005 && before >= heard * 0.25) {
                log(util::Format(L"own sound: skipped (other sound at the test pitch, %.4f)", before));
            } else {
                if (!ok) ++failures;
                log(util::Format(L"own sound: %s the mic playback comes out of OzMaker (level %.4f) and stays out of "
                                 L"the recording's sound (%.4f, %.4f before it from whatever else plays)",
                                 ok ? L"ok  " : L"FAIL", heard, recorded, before));
            }
        }
    }
    // What OzMaker opens (a video in its player, a page in the browser) is Explorer's child, not this process's:
    // the recording sound leaves out OzMaker's process tree and must still hear that player or browser.
    {
        std::wstring out = dir + L"\\parent.txt";
        DeleteFileW(out.c_str());
        if (!util::ShellOpen(util::ExePath(), L"--report-parent \"" + out + L"\"")) {
            log(L"open through Explorer: skipped (no Explorer desktop)");
        } else {
            DWORD parent = 0;
            for (double until = util::NowSeconds() + 10; util::NowSeconds() < until; Sleep(50)) {
                std::string text;
                if (FILE* f = _wfsopen(out.c_str(), L"rb", _SH_DENYNO)) {
                    char buf[32] = {};
                    text.assign(buf, fread(buf, 1, sizeof(buf) - 1, f));
                    fclose(f);
                }
                if (!text.empty()) {
                    parent = (DWORD)strtoul(text.c_str(), nullptr, 10);
                    break;
                }
            }
            bool ok = parent != 0 && parent != GetCurrentProcessId();
            if (!ok) ++failures;
            log(util::Format(L"open through Explorer: %s what it starts is not OzMaker's child (parent %s)",
                             ok ? L"ok  " : L"FAIL", parent == 0 ? L"unknown" : parent == GetCurrentProcessId() ? L"OzMaker" : L"another process"));
        }
    }
    // 여러 창 녹화 (#48): check boxes up to kMaxRecWindows in the order checked, the first one the window picked; the
    // switch hands the selection over both ways; each file is named after its window when the pattern doesn't say.
    {
        std::vector<HWND> ws;
        for (size_t i = 0; i <= kMaxRecWindows; ++i)
            ws.push_back(CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr));
        const int source = settings_.source;
        const bool multi = settings_.multiWindow;
        const HWND was = target_;
        settings_.source = kSourceWindow;
        settings_.multiWindow = false;
        target_ = ws[2];
        SetMultiWindow(true);
        const bool handed = MultiPicking() && picks_.size() == 1 && picks_[0] == ws[2];
        TogglePick(ws[2]);
        const bool none = picks_.empty() && !target_;
        toast_.clear();
        for (HWND h : ws) TogglePick(h);  // the last one is one too many
        const bool capped = picks_.size() == kMaxRecWindows && picks_[0] == ws[0] && picks_.back() == ws[kMaxRecWindows - 1] &&
                            target_ == ws[0] && !toast_.empty() && toastError_;
        TogglePick(ws[0]);
        const bool next = picks_.size() == kMaxRecWindows - 1 && target_ == ws[1] && PickedTargets().size() == kMaxRecWindows - 1;
        SetMultiWindow(false);
        const bool back = !MultiPicking() && target_ == ws[1];
        bool ok = handed && none && capped && next && back;
        if (!ok) ++failures;
        log(util::Format(L"multi-window: %s checked in order, %zu at most, the first one picked (handed %d, none %d, capped %d, "
                         L"next %d, back %d)",
                         ok ? L"ok  " : L"FAIL", kMaxRecWindows, handed, none, capped, next, back));
        const std::wstring pattern = settings_.nameTemplate;
        settings_.nameTemplate = L"OzMaker_{날짜}";
        const std::wstring a = RecFileStem(L"데모 게임", true), one = RecFileStem(L"데모 게임", false);
        settings_.nameTemplate = L"{창 이름}_{날짜}";
        const std::wstring b = RecFileStem(L"데모 게임", true);
        settings_.nameTemplate = L"{window}_{number}";
        const std::wstring c = RecFileStem(L"데모 게임", true);
        settings_.nameTemplate = pattern;
        ok = a.starts_with(L"OzMaker_") && a.ends_with(L"_데모 게임") && !one.ends_with(L"_데모 게임") &&
             b.starts_with(L"데모 게임_") && !b.ends_with(L"_데모 게임") && c.starts_with(L"데모 게임_0");
        if (!ok) ++failures;
        log(util::Format(L"multi-window names: %s %s · %s · %s", ok ? L"ok  " : L"FAIL", a.c_str(), b.c_str(), c.c_str()));
        for (HWND h : ws) DestroyWindow(h);
        picks_.clear();
        toast_.clear();
        toastUntil_ = 0;
        settings_.source = source;
        settings_.multiWindow = multi;
        target_ = was;
    }
    // 버리기 in the editor (#37): an unsaved 움짤 recording goes, and the 편집 tab is left with nothing open.
    {
        auto clip = MakeSyntheticClip(320, 180, 10, 30);
        size_t before = held_.size();
        HoldRecording(clip, L"OzMaker Test Window");
        OpenEditor(clip);
        bool can = CanDiscard();
        DiscardRecording();
        bool ok = can && !clip_ && held_.size() == before && screen_ == Screen::Editor && clip.use_count() == 1;
        if (!ok) ++failures;
        log(util::Format(L"discard: %s an unsaved recording goes from the editor", ok ? L"ok  " : L"FAIL"));
        CloseEditor();
    }
    // The 녹화 | 편집 tabs: the open file waits in the 편집 tab while the 녹화 tab shows; 닫기 leaves the 편집 tab
    // with nothing open and the recording still among those not saved; 버리기 on that list asks first.
    {
        auto clip = MakeSyntheticClip(320, 180, 10, 30);
        size_t before = held_.size();
        HoldRecording(clip, L"OzMaker Test Window");
        OpenEditor(clip);
        CloseEditor();
        const bool away = screen_ == Screen::Home && clip_ == clip;
        ResumeEditor();
        const bool back = screen_ == Screen::Editor && clip_ == clip && playing_;
        CloseClip();
        const bool closed = screen_ == Screen::Editor && !clip_ && !playing_ && held_.size() == before + 1;
        CloseEditor();
        ResumeEditor();
        const bool empty = screen_ == Screen::Editor && !clip_;
        OpenHeld(0);
        const bool reopened = screen_ == Screen::Editor && clip_ == clip;
        CloseEditor();
        ConfirmDropHeld(0);
        const bool asked = question_.has_value() && held_.size() == before + 1;
        if (question_) {
            auto yes = std::move(question_->onYes);
            question_.reset();
            if (yes) yes();
        }
        const bool dropped = held_.size() == before && !clip_ && clip.use_count() == 1;
        bool ok = away && back && closed && empty && reopened && asked && dropped;
        if (!ok) ++failures;
        log(util::Format(L"tabs: %s 녹화 keeps the file open %d, 편집 brings it back %d, 닫기 keeps the recording %d, "
                         L"편집 with nothing open %d, reopened from the list %d, 버리기 asks %d and drops it %d",
                         ok ? L"ok  " : L"FAIL", away, back, closed, empty, reopened, asked, dropped));
        toastUntil_ = 0;
    }
    // The preview plays the way the export goes (#22): 거꾸로 backwards, a boomerang turns at the end; J / L shuttle
    // through the range at 1×, 2×, 4× and stop at its end, K stops, R gives back the whole clip.
    {
        auto pcheck = [&](bool ok, const std::wstring& what) {
            if (!ok) ++failures;
            log((ok ? L"preview: ok   " : L"preview: FAIL ") + what);
        };
        OpenEditor(MakeSyntheticClip(320, 180, 60, 30));
        fmt_ = (int)Format::Gif;
        settings_.speedIdx = 1;
        settings_.loopIdx = 0;
        settings_.directionIdx = 1;
        SetPlaying(false);
        Seek(30);
        SetPlaying(true);
        playWall_ -= 0.2;
        size_t back = PlayheadFromClock();
        pcheck(playBack_ && back >= 23 && back <= 25, util::Format(L"거꾸로 runs backwards: 30 -> %zu", back));
        settings_.directionIdx = 2;
        SetPlaying(false);
        Seek(57);
        SetPlaying(true);
        playWall_ -= 0.2;
        size_t turned = PlayheadFromClock();
        pcheck(playBack_ && playing_ && turned >= 54 && turned <= 57,
               util::Format(L"부메랑 turns at the end: 57 -> %zu, backwards %d", turned, playBack_));
        settings_.directionIdx = 0;
        SetPlaying(false);
        Seek(10);
        Shuttle(1);
        Shuttle(1);
        const int twice = shuttle_;
        playWall_ -= 0.1;
        size_t fast = PlayheadFromClock();
        Shuttle(-1);
        const bool backward = shuttle_ == -1 && playBack_;
        SetPlaying(false);  // K
        pcheck(twice == 2 && fast >= 15 && fast <= 17 && backward && !playing_ && shuttle_ == 0,
               util::Format(L"L twice is 2× (10 -> %zu), J goes back, K stops", fast));
        Seek(55);
        Shuttle(1);
        playWall_ -= 1.0;
        size_t end = PlayheadFromClock();
        pcheck(!playing_ && end == 59 && shuttle_ == 0, util::Format(L"a shuttle stops at the end: %zu", end));
        inF_ = 10;
        outF_ = 20;
        ResetRange();
        pcheck(inF_ == 0 && outF_ == 59, L"R gives back the whole clip");
        CloseEditor();
        CloseClip();
        toastUntil_ = 0;
    }
    // The caption's text field (#22): typed text, Korean text the IME finished, keys, what the IME was still
    // composing kept when the field lets go, and the length limit.
    {
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICBitmap> bmp;
        ComPtr<ID2D1RenderTarget> rt;
        bool drew = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                               IID_PPV_ARGS(&wic))) &&
                    SUCCEEDED(wic->CreateBitmap(400, 60, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)) &&
                    SUCCEEDED(gfx_.D2D()->CreateWicBitmapRenderTarget(bmp.Get(), D2D1::RenderTargetProperties(), &rt));
        Ui ui;
        UiInput in;
        std::wstring text;
        double now = 1;
        const UiId id = HashId("test.field");
        auto frame = [&] {
            rt->BeginDraw();
            ui.Begin(rt.Get(), in, now += 0.05);
            ui.TextField(id, Rect(10, 10, 390, 46), text, L"", 10);
            ui.End();
            drew &= SUCCEEDED(rt->EndDraw());
            in.ClearEdges();
        };
        if (drew) {
            in.mx = 50;
            in.my = 20;
            in.pressed = in.down = true;
            frame();
            in.down = false;
            in.released = true;
            frame();
            const bool focused = ui.TextFocused();
            in.chars = L"ab";
            frame();
            in.chars = L"한글";
            frame();
            in.keys = {VK_LEFT};
            frame();
            in.keys = {VK_BACK};
            frame();
            const bool edited = text == L"ab글";
            in.keys = {VK_END};
            in.composing = L"가";
            frame();
            const bool composing = text == L"ab글";
            in.mx = 1000;
            in.pressed = in.down = true;
            frame();
            in.down = false;
            in.composing.clear();
            const bool kept = text == L"ab글가" && !ui.TextFocused() && ui.TakeImeReset();
            ui.Focus(id);
            in.chars = L"0123456789";
            frame();
            const bool limited = text == L"ab글가012345";
            in.keys = {VK_ESCAPE};
            frame();
            bool ok = focused && edited && composing && kept && limited && !ui.TextFocused();
            if (!ok) ++failures;
            log(util::Format(L"text field: %s focus %d, typing and keys %d, composing shown not kept %d, kept on "
                             L"leaving %d, 10 at most %d: \"%s\"",
                             ok ? L"ok  " : L"FAIL", focused, edited, composing, kept, limited, text.c_str()));
        } else {
            ++failures;
            log(L"text field: FAIL no render target");
        }
    }
    // Keyboard and screen readers (#26), on a few controls of each kind. Tab and Shift+Tab go through them in drawing
    // order, a row of choices as one stop at its chosen cell; Space and Enter press, the arrow keys pick along a row
    // (past a disabled cell) or move a slider; a text field takes the keys while it has the focus. The ring shows
    // only once the keyboard moved the focus, in a float; a click moves the focus without it; a question takes the
    // focus over. Then UI Automation, as a screen reader uses it, on a hidden window of its own showing them.
    {
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICBitmap> bmp;
        ComPtr<ID2D1RenderTarget> rt;
        bool drew = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                               IID_PPV_ARGS(&wic))) &&
                    SUCCEEDED(wic->CreateBitmap(400, 260, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)) &&
                    SUCCEEDED(gfx_.D2D()->CreateWicBitmapRenderTarget(bmp.Get(), D2D1::RenderTargetProperties(), &rt));
        Ui ui;
        UiInput in;
        double now = 1;
        int sel = 0, pressedB = 0, asked = 0;
        std::atomic<int> pressedA{0};
        bool on = false, question = false, aOff = false;
        float vol = 50;
        std::wstring text;
        const UiId idA = HashId("kb.a"), idRow = HashId("kb.row"), idToggle = HashId("kb.toggle"),
                   idVol = HashId("kb.vol"), idField = HashId("kb.field"), idB = HashId("kb.b"), idQ = HashId("kb.q");
        auto frame = [&](std::vector<UINT> keys = {}, bool shift = false) {
            if (!drew) return;
            in.keys = std::move(keys);
            in.shift = shift;
            rt->BeginDraw();
            ui.Begin(rt.Get(), in, now += 0.05);
            ui.SetInputBlocked(question);
            pressedA += ui.Button(idA, Rect(10, 10, 110, 40), L"단추 A", BtnStyle::Secondary, 0, !aOff);
            ui.Name(L"줄");
            ui.Segmented(idRow, Rect(10, 50, 310, 86), {L"하나", L"둘", L"셋"}, sel, 0b010);
            ui.Name(L"토글");
            ui.Toggle(idToggle, Rect(10, 96, 54, 120), on);
            ui.Name(L"음량");
            ui.Slider(idVol, Rect(10, 130, 210, 150), vol, 0.f, 100.f);
            ui.TextField(idField, Rect(10, 160, 210, 196), text, L"입력", 20);
            pressedB += ui.Button(idB, Rect(10, 206, 110, 236), L"단추 B", BtnStyle::Primary);
            if (question) {
                ui.SetInputBlocked(false);
                ui.BeginFloat();
                asked += ui.Button(idQ, Rect(200, 206, 300, 236), L"예", BtnStyle::Primary);
                ui.EndFloat();
            }
            ui.End();
            drew &= SUCCEEDED(rt->EndDraw());
            in.ClearEdges();
            in.shift = false;
        };
        frame();
        frame({VK_TAB});
        const bool first = ui.KeyboardFocus() == idA && ui.FocusRing() && ui.Floats().size() == 1;
        aOff = true;  // where the focus is turns off: Tab goes on to the one after it
        frame();
        frame({VK_TAB});
        aOff = false;
        const bool row = ui.KeyboardFocus() == Ui::CellId(idRow, 0);
        frame({VK_RIGHT});
        const bool picked = sel == 2 && ui.KeyboardFocus() == Ui::CellId(idRow, 2);
        frame({VK_TAB});
        frame({VK_SPACE});
        const bool toggled = on && ui.KeyboardFocus() == idToggle;
        frame({VK_TAB});
        frame({VK_RIGHT});
        const float stepped = vol;
        frame({VK_END});
        const bool slid = stepped == 55 && vol == 100;
        frame({VK_TAB});
        const bool field = ui.TextFocused() && ui.KeyboardFocus() == idField && ui.Floats().empty();
        in.chars = L"a b";
        frame({'A', VK_SPACE, 'B'});
        frame({VK_TAB});
        const bool typed = !ui.TextFocused() && ui.KeyboardFocus() == idB && text == L"a b";
        frame({VK_RETURN});
        const bool entered = pressedB == 1;
        frame({VK_TAB}, true);
        const bool back = ui.TextFocused() && ui.KeyboardFocus() == idField;
        frame({VK_TAB}, true);
        frame({VK_TAB}, true);
        frame({VK_TAB}, true);
        const bool backRow = ui.KeyboardFocus() == Ui::CellId(idRow, 2) && !ui.TextFocused() && sel == 2;
        frame({VK_ESCAPE});
        const bool let = !ui.FocusRing() && ui.KeyboardFocus() == Ui::CellId(idRow, 2);
        frame({VK_TAB});
        in.mx = 50;
        in.my = 25;
        in.pressed = in.down = true;
        frame();
        in.down = false;
        in.released = true;
        frame();
        const bool clicked = pressedA == 1 && ui.KeyboardFocus() == idA && !ui.FocusRing();
        // The nodes a screen reader is told about.
        const UiNode *nA = ui.Shown(idA), *nRow = ui.Shown(idRow), *nCell = ui.Shown(Ui::CellId(idRow, 2)),
                     *nToggle = ui.Shown(idToggle), *nVol = ui.Shown(idVol), *nField = ui.Shown(idField);
        const bool named = nA && nA->name == L"단추 A" && nA->role == Role::Button && nRow && nRow->name == L"줄" &&
                           nRow->role == Role::Group && nCell && nCell->name == L"셋" && nCell->role == Role::Choice &&
                           nCell->pos == 2 && nCell->count == 3 && nCell->group == idRow && nCell->on && nToggle &&
                           nToggle->name == L"토글" && nToggle->role == Role::Toggle && nToggle->on && nVol &&
                           nVol->name == L"음량" && nVol->value == 100 && nField && nField->role == Role::Edit &&
                           nField->name == L"입력" && nField->text == L"a b";
        frame({VK_TAB});
        question = true;
        frame();
        const bool takenOver = ui.KeyboardFocus() == idQ;
        frame({VK_RETURN});
        const bool answered = asked == 1 && pressedA == 1 && pressedB == 1;
        question = false;
        frame();
        bool ok = drew && first && row && picked && toggled && slid && field && typed && entered && back && backRow &&
                  let && clicked && named && takenOver && answered;
        if (!ok) ++failures;
        log(util::Format(L"keyboard: %s first stop with a ring %d, on past a disabled one to a row %d, arrows past a "
                         L"disabled cell %d, Space toggles %d, arrows and End on a slider %d (%.0f, %.0f), a field "
                         L"takes the keys %d, typed and left %d, Enter presses %d, Shift+Tab back %d and on %d, Esc "
                         L"lets go %d, a click moves it without the ring %d, named %d, a question takes it %d, "
                         L"answered %d",
                         ok ? L"ok  " : L"FAIL", first, row, picked, toggled, slid, stepped, vol, field, typed,
                         entered, back, backRow, let, clicked, named, takenOver, answered));

        // UI Automation: a client on a thread of its own (MTA), as a screen reader is; this thread draws a frame
        // every 30 ms and answers it meanwhile, as the app does.
        sel = 0;
        on = false;
        vol = 50;
        const int pressedBefore = pressedA;
        WNDCLASSW wc{};
        wc.lpfnWndProc = AccessTestProc;
        wc.hInstance = inst_;
        wc.lpszClassName = L"OzMakerAccessTest";
        RegisterClassW(&wc);
        HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"OzMaker Access Test", WS_POPUP, 100, 100,
                                    400, 260, nullptr, nullptr, inst_, nullptr);
        Access access;
        access.Attach(hwnd, &ui, [] { return 1.f; }, [] {});
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)&access);
        frame();
        std::atomic<bool> done{false};
        std::wstring seen;  // what the client found, for the log
        bool tree = false, kinds = false, place = false, box = false, invoked = false, toggledU = false,
             selected = false, set = false, value = false, focused = false;
        std::thread client([&] {
            if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
                done = true;
                return;
            }
            {
                ComPtr<IUIAutomation> uia;
                ComPtr<IUIAutomationElement> root;
                ComPtr<IUIAutomationCondition> all;
                ComPtr<IUIAutomationElementArray> found;
                if (SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                               IID_PPV_ARGS(&uia))) &&
                    SUCCEEDED(uia->ElementFromHandle(hwnd, &root)) && SUCCEEDED(uia->CreateTrueCondition(&all)) &&
                    SUCCEEDED(root->FindAll(TreeScope_Descendants, all.Get(), &found)) && found) {
                    std::map<std::wstring, ComPtr<IUIAutomationElement>> byName;
                    std::map<std::wstring, CONTROLTYPEID> type;
                    int n = 0;
                    found->get_Length(&n);
                    for (int i = 0; i < n; ++i) {
                        ComPtr<IUIAutomationElement> e;
                        BSTR name = nullptr;
                        CONTROLTYPEID t = 0;
                        if (FAILED(found->GetElement(i, &e)) || FAILED(e->get_CurrentName(&name))) continue;
                        e->get_CurrentControlType(&t);
                        std::wstring s = name ? name : L"";
                        SysFreeString(name);
                        seen += (seen.empty() ? L"" : L", ") + s;
                        byName[s] = e;
                        type[s] = t;
                    }
                    tree = n == 9;  // 단추 A, 줄 (하나 둘 셋), 토글, 음량, 입력, 단추 B
                    kinds = type[L"단추 A"] == UIA_ButtonControlTypeId && type[L"줄"] == UIA_GroupControlTypeId &&
                            type[L"셋"] == UIA_RadioButtonControlTypeId && type[L"토글"] == UIA_ButtonControlTypeId &&
                            type[L"음량"] == UIA_SliderControlTypeId && type[L"입력"] == UIA_EditControlTypeId;
                    auto get = [&](const wchar_t* name) {
                        auto it = byName.find(name);
                        return it == byName.end() ? nullptr : it->second.Get();
                    };
                    // Waits for the next frames to show what was done.
                    auto until = [&](const std::function<bool()>& f) {
                        for (int k = 0; k < 100; ++k) {
                            if (f()) return true;
                            Sleep(20);
                        }
                        return false;
                    };
                    if (auto* e = get(L"셋")) {
                        VARIANT pos{}, size{};
                        e->GetCurrentPropertyValue(UIA_PositionInSetPropertyId, &pos);
                        e->GetCurrentPropertyValue(UIA_SizeOfSetPropertyId, &size);
                        ComPtr<IUIAutomationTreeWalker> walk;
                        ComPtr<IUIAutomationElement> parent;
                        BSTR pn = nullptr;
                        uia->get_RawViewWalker(&walk);
                        if (walk) walk->GetParentElement(e, &parent);
                        if (parent) parent->get_CurrentName(&pn);
                        place = pos.vt == VT_I4 && pos.lVal == 3 && size.vt == VT_I4 && size.lVal == 3 && pn &&
                                std::wstring(pn) == L"줄";
                        SysFreeString(pn);
                    }
                    if (auto* e = get(L"단추 A")) {
                        RECT r{};
                        e->get_CurrentBoundingRectangle(&r);
                        box = r.left == 110 && r.top == 110 && r.right == 210 && r.bottom == 140;
                        ComPtr<IUIAutomationInvokePattern> p;
                        e->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&p));
                        invoked = p && SUCCEEDED(p->Invoke()) && until([&] { return pressedA == pressedBefore + 1; });
                    }
                    if (auto* e = get(L"토글")) {
                        ComPtr<IUIAutomationTogglePattern> p;
                        e->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(&p));
                        toggledU = p && SUCCEEDED(p->Toggle()) && until([&] {
                                       ToggleState s = ToggleState_Off;
                                       return SUCCEEDED(p->get_CurrentToggleState(&s)) && s == ToggleState_On;
                                   });
                    }
                    if (auto* e = get(L"셋")) {
                        ComPtr<IUIAutomationSelectionItemPattern> p;
                        e->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&p));
                        selected = p && SUCCEEDED(p->Select()) && until([&] {
                                       BOOL s = FALSE;
                                       return SUCCEEDED(p->get_CurrentIsSelected(&s)) && s;
                                   });
                    }
                    if (auto* e = get(L"음량")) {
                        ComPtr<IUIAutomationRangeValuePattern> p;
                        e->GetCurrentPatternAs(UIA_RangeValuePatternId, IID_PPV_ARGS(&p));
                        set = p && SUCCEEDED(p->SetValue(30)) && until([&] {
                                  double v = 0;
                                  return SUCCEEDED(p->get_CurrentValue(&v)) && v == 30;
                              });
                    }
                    if (auto* e = get(L"입력")) {
                        ComPtr<IUIAutomationValuePattern> p;
                        BSTR v = nullptr;
                        e->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&p));
                        value = p && SUCCEEDED(p->get_CurrentValue(&v)) && v && std::wstring(v) == L"a b";
                        SysFreeString(v);
                    }
                    if (auto* e = get(L"단추 B")) focused = SUCCEEDED(e->SetFocus());
                }
            }
            CoUninitialize();
            done = true;
        });
        const double deadline = util::NowSeconds() + 30;
        while (!done && util::NowSeconds() < deadline) {
            MsgWaitForMultipleObjectsEx(0, nullptr, 30, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            frame();
        }
        if (!done) {
            // Stuck in a call: it would write here after this is gone, so the test ends now.
            log(L"screen reader: FAIL no answer in 30 s");
            ExitProcess(1);
        }
        client.join();
        frame();
        focused &= ui.KeyboardFocus() == idB;
        access.Detach();
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        DestroyWindow(hwnd);
        ok = done && tree && kinds && place && box && invoked && toggledU && selected && set && value && focused &&
             sel == 2 && on && vol == 30;
        if (!ok) ++failures;
        log(util::Format(L"screen reader: %s the controls %d, their kinds %d, a cell's place in its row %d, where one "
                         L"is %d; Invoke %d, Toggle %d, Select %d, a slider set %d, a field's text %d, the focus "
                         L"set %d (%s)",
                         ok ? L"ok  " : L"FAIL", tree, kinds, place, box, invoked, toggledU, selected, set, value,
                         focused, seen.c_str()));
    }
    // The caption over an export (#22): white with a dark outline, at the bottom or the top, nothing elsewhere.
    for (int pos : {caption::kBottom, caption::kTop}) {
        const int cw = 320, ch = 180;
        caption::Overlay cap;
        bool made = cap.Make(L"자막 Caption", pos, cw, ch);
        std::vector<uint8_t> f((size_t)cw * ch * 4);
        for (size_t i = 0; i < f.size(); i += 4) f[i] = f[i + 1] = f[i + 2] = 128, f[i + 3] = 255;
        cap.Blend(f.data());
        int white[3] = {}, dark = 0;  // white pixels by third of the height
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x) {
                const uint8_t* p = &f[((size_t)y * cw + x) * 4];
                if (p[0] > 240 && p[1] > 240 && p[2] > 240) ++white[y * 3 / ch];
                if (p[0] < 40 && p[1] < 40 && p[2] < 40) ++dark;
            }
        const int where = pos == caption::kTop ? 0 : 2;
        bool ok = made && white[where] > 40 && white[1] == 0 && white[2 - where] == 0 && dark > 40;
        if (!ok) ++failures;
        log(util::Format(L"caption %s: %s white %d/%d/%d by third, dark %d", pos == caption::kTop ? L"top" : L"bottom",
                         ok ? L"ok" : L"FAIL", white[0], white[1], white[2], dark));
    }
    // The MP4 export's frames on the GPU (#23) against the CPU's (AreaResizer, Overlay::Blend, over white): the same
    // where the size stays (an odd edge trimmed), close where it is scaled (a box filter over mips against an exact
    // one), with a caption and with see-through parts. And how long 1920×1080 to 1280×720 takes either way.
    {
        ComPtr<ID3D11Device> gdev;
        ComPtr<ID3D11DeviceContext> gctx;
        bool made = false;
        for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP})
            if (!made)
                made = SUCCEEDED(D3D11CreateDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr,
                                                   0, D3D11_SDK_VERSION, &gdev, nullptr, &gctx));
        const int SW = 2000, SH = 1200, stride = SW * 4;
        std::vector<uint8_t> solid((size_t)SW * SH * 4), see(solid.size());
        DrawSynthetic(solid.data(), SW, SH, 0.3);
        // See-through, premultiplied: fading out to the right, and a clear band with nothing in it.
        for (int y = 0; y < SH; ++y)
            for (int x = 0; x < SW; ++x) {
                const size_t i = ((size_t)y * SW + x) * 4;
                const unsigned a = y % 200 < 30 ? 0 : 255 - x * 200 / SW;
                for (int c = 0; c < 3; ++c) see[i + c] = (uint8_t)((solid[i + c] * a + 127) / 255);
                see[i + 3] = (uint8_t)a;
            }
        struct Case {
            const wchar_t* name;
            int cx, cy, cw, ch, w, h;
            bool alpha;
            const wchar_t* text;
            double psnr;  // at least; 0: the same, give or take 1
        };
        const Case cases[] = {
            {L"same size", 100, 50, 640, 360, 640, 360, false, L"", 0},
            {L"same size, caption, see-through", 100, 50, 640, 360, 640, 360, true, L"자막 Caption", 0},
            {L"odd edge trimmed", 101, 51, 641, 361, 640, 360, false, L"", 0},
            {L"1.5x, caption", 0, 0, 1920, 1080, 1280, 720, false, L"자막 Caption", 38},
            {L"2.4x", 37, 11, 1537, 865, 640, 360, false, L"", 38},
            {L"6x (mips), see-through", 0, 0, 1920, 1080, 320, 180, true, L"자막", 38},
        };
        std::wstring got;
        bool all = made;
        double gpuMs = 0, cpuMs = 0;
        for (const Case& c : cases) {
            if (!made) break;
            const uint8_t* src = c.alpha ? see.data() : solid.data();
            caption::Overlay cap;
            cap.Make(c.text, caption::kBottom, c.w, c.h);
            // The CPU's frame, cropped as the GPU does when it copies.
            const bool copy = c.cw - c.w >= 0 && c.cw - c.w <= 1 && c.ch - c.h >= 0 && c.ch - c.h <= 1;
            std::vector<uint8_t> want((size_t)c.w * c.h * 4), have(want.size());
            AreaResizer resizer;
            auto onCpu = [&] {
                resizer.Run(src, stride, c.cx, c.cy, copy ? c.w : c.cw, copy ? c.h : c.ch, want.data(), c.w, c.h,
                            c.alpha);
                cap.Blend(want.data());
                if (c.alpha)
                    for (size_t i = 0; i < want.size(); i += 4) {
                        for (int k = 0; k < 3; ++k) want[i + k] = (uint8_t)std::min(255, want[i + k] + 255 - want[i + 3]);
                        want[i + 3] = 255;
                    }
            };
            D3D11_TEXTURE2D_DESC td{};
            td.Width = c.w;
            td.Height = c.h;
            td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> out, staging;
            D3D11_TEXTURE2D_DESC sd = td;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            GpuResizer gpu;
            const uint8_t* crop = src + (size_t)c.cy * stride + (size_t)c.cx * 4;
            bool ok = SUCCEEDED(gdev->CreateTexture2D(&td, nullptr, &out)) &&
                      SUCCEEDED(gdev->CreateTexture2D(&sd, nullptr, &staging)) &&
                      gpu.Init(gdev.Get(), out.Get(), c.cw, c.ch, c.alpha, cap);
            D3D11_MAPPED_SUBRESOURCE m{};
            auto readBack = [&] {
                gctx->CopyResource(staging.Get(), out.Get());
                if (FAILED(gctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
                for (int y = 0; y < c.h; ++y)
                    memcpy(&have[(size_t)y * c.w * 4], (const uint8_t*)m.pData + (size_t)y * m.RowPitch, c.w * 4);
                gctx->Unmap(staging.Get(), 0);
                return true;
            };
            if (ok) {
                gpu.Draw(gctx.Get(), crop, stride);
                ok = readBack();
            }
            onCpu();
            int most = 0;
            double se = 0;
            for (size_t i = 0; ok && i < want.size(); i += 4)
                for (int k = 0; k < 3; ++k) {
                    const int d = std::abs(want[i + k] - have[i + k]);
                    most = std::max(most, d);
                    se += d * d;
                }
            const double psnr = se > 0 ? 10 * std::log10(255.0 * 255.0 / (se / (want.size() / 4 * 3))) : 99;
            ok = ok && (c.psnr > 0 ? psnr >= c.psnr : most <= 1);
            got += util::Format(L"%s %s %.1f dB (most %d); ", c.name, ok ? L"ok" : L"FAIL", psnr, most);
            all = all && ok;
            // 1920×1080 to 1280×720 with the caption: 20 frames each way.
            if (ok && c.w == 1280) {
                const int n = 20;
                int64_t t = util::Qpc100ns();
                for (int i = 0; i < n; ++i) gpu.Draw(gctx.Get(), crop, stride);
                readBack();
                gpuMs = (util::Qpc100ns() - t) / 1e4 / n;
                t = util::Qpc100ns();
                for (int i = 0; i < n; ++i) onCpu();
                cpuMs = (util::Qpc100ns() - t) / 1e4 / n;
            }
        }
        if (!all) ++failures;
        log(util::Format(L"MP4 export frames on the GPU: %s %s1920x1080 to 1280x720 a frame: GPU %.1f ms, CPU %.1f ms",
                         all ? L"ok" : L"FAIL", made ? got.c_str() : L"no device; ", gpuMs, cpuMs));
    }
    // Portable mode (#25): a settings.ini next to the exe is the settings file; a test run keeps its scratch file.
    {
        auto pcheck = [&](bool ok, const wchar_t* what) {
            if (!ok) ++failures;
            log(util::Format(L"portable: %s %s", ok ? L"ok  " : L"FAIL", what));
        };
        std::wstring exeDir = dir + L"\\portable";
        util::EnsureDir(exeDir);
        DeleteFileW((exeDir + L"\\settings.ini").c_str());
        pcheck(Settings::PortableFile(exeDir).empty(), L"no settings.ini next to the exe: not portable");
        HANDLE f = CreateFileW((exeDir + L"\\settings.ini").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0,
                               nullptr);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        pcheck(Settings::PortableFile(exeDir) == exeDir + L"\\settings.ini", L"an empty settings.ini turns it on");
        wchar_t env[MAX_PATH] = {};
        GetEnvironmentVariableW(L"OZMAKER_SETTINGS", env, MAX_PATH);
        pcheck(!Settings::Portable() && Settings::FilePath() == env, L"a test run keeps OZMAKER_SETTINGS");
    }
    // 화질 presets: a new settings file starts at 보통 for 움짤 and 고화질 for MP4; one from before the presets keeps
    // its values and shows the preset they match; 움짤 and MP4 keep their own. Each case is a file in this test's
    // folder, read through OZMAKER_SETTINGS, which is then put back.
    {
        auto check = [&](bool ok, const wchar_t* what) {
            if (!ok) ++failures;
            log(util::Format(L"presets: %s %s", ok ? L"ok  " : L"FAIL", what));
        };
        wchar_t env[MAX_PATH] = {};
        GetEnvironmentVariableW(L"OZMAKER_SETTINGS", env, MAX_PATH);
        auto load = [&](const wchar_t* name, const char* text) {  // text null: the file as it is
            std::wstring ini = dir + L"\\" + name;
            HANDLE f = text ? CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr)
                            : INVALID_HANDLE_VALUE;
            if (f != INVALID_HANDLE_VALUE) {
                DWORD n = 0;
                WriteFile(f, text, (DWORD)strlen(text), &n, nullptr);
                CloseHandle(f);
            }
            SetEnvironmentVariableW(L"OZMAKER_SETTINGS", ini.c_str());
            Settings st;
            st.Load();
            SetEnvironmentVariableW(L"OZMAKER_SETTINGS", env);
            return st;
        };
        Settings fresh = load(L"preset_new.ini", "");
        check(fresh.exportPreset == 1 && fresh.scaleIdx == 4 && fresh.outFpsIdx == 3 && !fresh.exportDetails,
              L"a new settings file starts at 보통 (640px, 20fps) with 자세히 설정 folded");
        fresh.UseExportKind(true);
        bool mp4High = fresh.exportPreset == 0 && fresh.scaleIdx == 0 && fresh.outFpsIdx == 0;
        fresh.UseExportKind(false);
        check(mp4High && fresh.exportPreset == 1 && fresh.scaleIdx == 4 && fresh.outFpsIdx == 3,
              L"MP4 starts at 고화질 and switching back gives 움짤 its own values");
        Settings old = load(L"preset_old.ini", "[OzMaker]\r\nExportSize=0\r\nOutFps=0\r\n");
        check(old.exportPreset == 0 && old.scaleIdx == 0 && old.outFpsIdx == 0,
              L"an older file at full size keeps it and shows 고화질");
        Settings odd = load(L"preset_odd.ini", "[OzMaker]\r\nExportSize=1\r\nOutFps=2\r\n");
        check(odd.exportPreset == -1 && odd.scaleIdx == 1 && odd.outFpsIdx == 2,
              L"other older values stay, shown as 직접 설정함");
        Settings custom = load(L"preset_custom.ini", "[OzMaker]\r\nExportPreset=-1\r\nExportSize=4\r\nOutFps=3\r\n");
        check(custom.exportPreset == -1, L"직접 설정 stays even when the values match a preset");
        Settings st;
        st.ApplyPreset(2, true);
        bool video = st.scaleIdx == 4 && st.outFpsIdx == 1 && st.gifLossy == 1;
        st.ApplyPreset(2, false);
        check(video && st.scaleIdx == 5 && st.outFpsIdx == 4 && st.gifLossy == 2 && st.MatchPreset(false, true) == 2 &&
                  st.MatchPreset(true, false) == -1,
              L"저용량 is 640px/30fps for MP4 and 480px/15fps with strong lossy for 움짤");
        // Saved with MP4 shown: each kind's values land under its own keys.
        {
            Settings both = load(L"preset_both.ini", "");
            both.UseExportKind(true);
            both.exportPreset = 2;
            both.ApplyPreset(2, true);
            std::wstring ini = dir + L"\\preset_both.ini";
            SetEnvironmentVariableW(L"OZMAKER_SETTINGS", ini.c_str());
            both.Save();
            SetEnvironmentVariableW(L"OZMAKER_SETTINGS", env);
        }
        Settings back = load(L"preset_both.ini", nullptr);
        bool anim = back.exportPreset == 1 && back.scaleIdx == 4 && back.outFpsIdx == 3;
        back.UseExportKind(true);
        check(anim && back.exportPreset == 2 && back.scaleIdx == 4 && back.outFpsIdx == 1,
              L"움짤 and MP4 values are saved apart");

        // 순간 리플레이 (#20): off in a new file; what was set comes back with its list of programs, and 설정 초기화
        // turns it off but keeps the list.
        {
            Settings rp = load(L"replay.ini", "");
            const bool fresh0 = !rp.rpOn && rp.rpLenIdx == 1 && rp.rpTargets.empty() && rp.rpHkVk == VK_F10 && !rp.rpMic;
            rp.rpOn = rp.rpMic = rp.rpHkHook = true;
            rp.rpLenIdx = 3;
            rp.rpTargets = {{false, L"C:\\Games\\A b\\game.exe", L"Game | One", false}, {true, L"\\\\.\\DISPLAY2", L"", true}};
            std::wstring ini = dir + L"\\replay.ini";
            SetEnvironmentVariableW(L"OZMAKER_SETTINGS", ini.c_str());
            rp.Save();
            SetEnvironmentVariableW(L"OZMAKER_SETTINGS", env);
            Settings got = load(L"replay.ini", nullptr);
            const auto& t = got.rpTargets;
            check(fresh0 && got.rpOn && got.rpMic && got.rpHkHook && got.rpLenIdx == 3 && t.size() == 2 && !t[0].monitor &&
                      !t[0].on && t[0].path == L"C:\\Games\\A b\\game.exe" && t[0].name == L"Game | One" &&
                      t[1].monitor && t[1].on && t[1].path == L"\\\\.\\DISPLAY2",
                  L"순간 리플레이: off at first; the settings and the programs come back");
            got.Reset();
            check(!got.rpOn && got.rpTargets.size() == 2, L"설정 초기화 turns replay off and keeps its programs");
        }

        // 0.6.5: 최대 녹화 길이 becomes a record timer that stops (#47), H.264 becomes 자동 and HEVC stays (#42),
        // and a file written since keeps its own over the old keys.
        Settings ten = load(L"timer_old10.ini", "[OzMaker]\r\nMaxLength=1\r\nVideoCodec=1\r\n");
        check(ten.timerIdx == kTimerCustom && ten.TimerMinutes() == 10 && ten.timerStop && ten.vCodec == kCodecHevc,
              L"최대 녹화 길이 10분: a 10-minute timer that stops; HEVC stays");
        Settings hour = load(L"timer_old60.ini", "[OzMaker]\r\nMaxLength=3\r\nVideoCodec=0\r\n");
        check(hour.timerIdx == 2 && hour.TimerMinutes() == 60 && hour.timerStop && hour.vCodec == kCodecAuto,
              L"최대 녹화 길이 1시간: the 1시간 timer that stops; H.264 becomes 자동");
        check(fresh.TimerMinutes() == 0 && !fresh.timerStop && fresh.vCodec == kCodecAuto &&
                  fresh.startHide == kHideToTray && !fresh.hkHook && !fresh.hkPauseHook &&
                  fresh.deleteMode == kDeleteToBin && fresh.imageFormat == image::kPng &&
                  fresh.hkShotVk == VK_F9 && fresh.hkShotMods == (MOD_CONTROL | MOD_ALT) && !fresh.hkShotHook,
              L"a new file: no timer (notifying once set), 자동, hidden to the tray, no hotkey taken over, 삭제 to the bin, "
              L"images as PNG, screenshots on Ctrl + Alt + F9");
        check(load(L"image_webp.ini", "[OzMaker]\r\nImageFormat=2\r\n").imageFormat == image::kWebp &&
                  load(L"image_odd.ini", "[OzMaker]\r\nImageFormat=7\r\n").imageFormat == image::kPng,
              L"이미지 형식: WebP is read back; a value it doesn't know is PNG");
        check(load(L"delete_now.ini", "[OzMaker]\r\nDeleteMode=1\r\n").deleteMode == kDeleteNow &&
                  load(L"delete_odd.ini", "[OzMaker]\r\nDeleteMode=7\r\n").deleteMode == kDeleteToBin,
              L"삭제할 때: 바로 삭제 is read back; a value it doesn't know stays with the Recycle Bin");
        Settings kept = load(L"timer_kept.ini", "[OzMaker]\r\nTimer=4\r\nTimerMinutes=200\r\nTimerStop=0\r\nMaxLength=2\r\n"
                                                "Codec=1\r\nVideoCodec=1\r\nHotkeyHook=1\r\nStartHide=1\r\n");
        check(kept.timerIdx == kTimerCustom && kept.TimerMinutes() == 200 && !kept.timerStop &&
                  kept.vCodec == kCodecH264 && kept.hkHook && kept.startHide == kHideMinimize,
              L"a 0.6.5 file keeps its timer, codec, taken-over hotkey and start over the old keys");

        // 프레임 레이트 방식 (#55): a file from before it has none and stays 고정.
        Settings vfrOld = load(L"vfr_old.ini", "[OzMaker]\r\nVideoRateControl=1\r\n");
        Settings vfrOn = load(L"vfr_on.ini", "[OzMaker]\r\nVideoFrameRateMode=1\r\n");
        check(fresh.vFrameMode == 0 && vfrOld.vFrameMode == 0 && vfrOld.vRateCtrl == 1 && vfrOn.vFrameMode == 1,
              L"프레임 레이트 방식: 고정 in a new file and in one from before it, 가변 read back");

        // 설정 초기화 (#49) keeps the save folder, the window's place, the 녹화 tab's pick and the files lists, and
        // puts back the rest, hotkeys taken over or not; the editor's kind of 화질 stays the one it shows.
        Settings r = load(L"reset.ini", "");
        r.outDir = L"X:\\Clips";
        r.window = {10, 20, 810, 620};
        r.windowMax = true;
        r.source = kSourceRegion;
        r.region = {0, 0, 640, 480};
        r.monitor = L"\\\\.\\DISPLAY2";
        r.recent = {L"X:\\Clips\\a.gif"};
        r.crashSeen = L"crash_x";
        r.trayHintShown = true;
        r.hkMods = MOD_ALT;
        r.hkVk = VK_F9;
        r.hkHook = r.hkPauseHook = true;
        r.timerIdx = 2;
        r.timerStop = true;
        r.theme = kThemeLight;
        r.language = kLangEnglish;
        r.vCodec = kCodecHevc;
        r.vFrameMode = 1;
        r.startHide = kHideMinimize;
        r.deleteMode = kDeleteNow;
        r.nameTemplate = L"{날짜}";
        r.output = kOutputVideo;
        r.fpsIdx = 0;
        r.UseExportKind(true);
        r.exportPreset = 2;
        r.ApplyPreset(2, true);
        r.Reset();
        const Settings d;
        check(r.outDir == L"X:\\Clips" && r.window.right == 810 && r.windowMax && r.source == kSourceRegion &&
                  r.region.right == 640 && r.monitor == L"\\\\.\\DISPLAY2" && r.recent.size() == 1 &&
                  r.crashSeen == L"crash_x" && r.trayHintShown,
              L"설정 초기화 keeps the save folder, the window's place, the 녹화 tab's pick and the files lists");
        check(r.hkMods == d.hkMods && r.hkVk == d.hkVk && !r.hkHook && !r.hkPauseHook && r.TimerMinutes() == 0 &&
                  !r.timerStop && r.theme == d.theme && r.language == d.language && r.vCodec == kCodecAuto &&
                  r.vFrameMode == 0 && r.startHide == kHideToTray && r.deleteMode == kDeleteToBin &&
                  r.nameTemplate == d.nameTemplate && r.output == d.output && r.fpsIdx == d.fpsIdx,
              L"and puts back the rest: hotkeys (taken over or not), timer, theme, language, codec, frame rate mode, start, "
              L"삭제");
        const bool mp4First = r.exportVideo && r.exportPreset == 0 && r.scaleIdx == 0 && r.outFpsIdx == 0;
        r.UseExportKind(false);
        check(mp4First && r.exportPreset == 1 && r.scaleIdx == 4 && r.outFpsIdx == 3,
              L"with MP4 and 움짤 at their first 화질, the editor's kind as it was");
    }
    // Crash reports (#24): a child run that crashes on purpose leaves a dump and a log and ends without the Windows
    // error dialog. It shares this run's OZMAKER_SETTINGS, so the report lands in this test's folder.
    {
        crash::Prune(0);
        for (const wchar_t* kind : {L"access", L"abort"}) {
            std::wstring cmd = L"\"" + util::ExePath() + L"\" --crash-test " + kind;
            STARTUPINFOW si{sizeof(si)};
            PROCESS_INFORMATION pi{};
            DWORD code = 0, waited = WAIT_FAILED;
            if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                waited = WaitForSingleObject(pi.hProcess, 30000);
                if (waited != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
                GetExitCodeProcess(pi.hProcess, &code);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
            auto reports = crash::List();
            std::string text;
            if (!reports.empty() && !reports[0].log.empty()) {
                HANDLE f = CreateFileW(reports[0].log.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                                       nullptr);
                if (f != INVALID_HANDLE_VALUE) {
                    char buf[4096];
                    DWORD n = 0;
                    if (ReadFile(f, buf, sizeof(buf), &n, nullptr)) text.assign(buf, n);
                    CloseHandle(f);
                }
            }
            const char* want = kind[1] == L'c' ? "(access violation) at OzMaker.exe+" : "(abort";
            bool ok = waited == WAIT_OBJECT_0 && reports.size() == 1 && util::FileSize(reports[0].dump) > 0 &&
                      text.find(want) != std::string::npos && text.find("OzMaker " OZ_VERSION_STR) != std::string::npos;
            if (!ok) ++failures;
            log(util::Format(L"crash report: %s %s: a dump and a log (exit 0x%08lX, %zu report(s))", ok ? L"ok  " : L"FAIL",
                             kind, code, reports.size()));
            crash::Prune(0);
        }
    }
    // Crash-safe 움짤 recording (#24): what a run that crashed mid-recording had on disk comes back frame for frame,
    // and goes with its files once dropped. This run's own recordings stay where they are.
    {
        auto rcheck = [&](bool ok, const std::wstring& what) {
            if (!ok) ++failures;
            log((ok ? L"recovery: ok   " : L"recovery: FAIL ") + what);
        };
        std::wstring cmd = L"\"" + util::ExePath() + L"\" --crash-test record";
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            if (WaitForSingleObject(pi.hProcess, 30000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        crash::Prune(0);
        auto mine = MakeSyntheticClip(32, 32, 3, 30);  // alive in this process: not taken
        auto found = FrameStore::Recover();
        int bad = 0;
        if (found.size() == 1 && found[0].clip->Count() == 45) {
            FrameStore::Reader rd(found[0].clip.get());
            std::vector<uint8_t> want((size_t)160 * 90 * 4);
            for (int i = 0; i < 45; ++i) {
                DrawSynthetic(want.data(), 160, 90, i / 30.0);
                const uint8_t* px = rd.Get(i);
                if (!px || memcmp(px, want.data(), want.size()) != 0) ++bad;
            }
        }
        rcheck(found.size() == 1 && found[0].clip->Count() == 45 && found[0].clip->Width() == 160 && bad == 0 &&
                   std::fabs(found[0].clip->NominalFps() - 30) < 0.01,
               util::Format(L"a crashed recording comes back: %zu found, %zu frames, %d differ", found.size(),
                            found.empty() ? 0 : found[0].clip->Count(), bad));
        std::wstring data = found.empty() ? L"" : found[0].clip->Path();
        found.clear();
        rcheck(!data.empty() && GetFileAttributesW(data.c_str()) == INVALID_FILE_ATTRIBUTES,
               L"dropping it deletes its files");
        std::wstring own = mine->Path();
        mine.reset();
        rcheck(GetFileAttributesW(own.c_str()) == INVALID_FILE_ATTRIBUTES, L"a recording dropped normally leaves nothing");
    }
    // Crash-safe MP4 recording (#24): a recording is fragmented while it is written. Finished, it is a plain MP4 with
    // every frame; after a crash mid-recording the next start makes what reached the disk a plain MP4 too.
    if (!HasVideoProcessor()) {
        log(L"video recovery: skipped (no GPU video processor)");
    } else {
        auto vcheck = [&](bool ok, const std::wstring& what) {
            if (!ok) ++failures;
            log((ok ? L"video recovery: ok   " : L"video recovery: FAIL ") + what);
        };
        // plain: one moov without mvex, no moof, nothing after the last box. tables: the sample tables give the
        // times and keyframes that reading every sample gives. wrong: frames not in their place.
        struct Seen {
            bool plain = false, tables = false, audio = false;
            size_t frames = 0;
            int wrong = 0;
        };
        auto inspect = [&](const std::wstring& path) {
            Seen s;
            bool frag = true;
            auto boxes = TopBoxes(path, &frag);
            int moovs = 0, moofs = 0;
            for (const TopBox& b : boxes) {
                moovs += b.type == "moov";
                moofs += b.type == "moof";
            }
            s.plain = !frag && moovs == 1 && moofs == 0 && !boxes.empty() &&
                      boxes.back().pos + boxes.back().size == util::FileSize(path);
            std::wstring err;
            auto clip = VideoClip::Open(path, err), scanned = VideoClip::Open(path, err, true);
            if (!clip || !scanned) return s;
            s.frames = clip->Count();
            s.audio = clip->HasAudio();
            s.tables = scanned->Count() == clip->Count();
            for (size_t i = 0; s.tables && i < clip->Count(); ++i)
                s.tables = std::llabs(scanned->Ts(i) - clip->Ts(i)) <= 2 && scanned->KeyBefore(i) == clip->KeyBefore(i);
            auto rd = clip->NewReader();
            for (size_t i = 0; i < clip->Count(); ++i) {
                const uint8_t* px = rd->Get(i);
                if (!px || ReadIndex(px, clip->Width(), clip->Height()) != (int)i) ++s.wrong;
            }
            return s;
        };
        auto describe = [](const Seen& s) {
            return util::Format(L"%zu frames, %d wrong, plain %d, tables %d, audio %d", s.frames, s.wrong, s.plain,
                                s.tables, s.audio);
        };
        const int N = 120;
        std::wstring finished = dir + L"\\video_finished.mp4", err;
        bool wrote = WriteTestMp4(finished, 320, 180, N, 60, 30, err, true);
        Seen fin = inspect(finished);
        vcheck(wrote && fin.plain && fin.frames == N && fin.wrong == 0 && fin.tables && fin.audio,
               L"a finished recording is a plain MP4 with every frame: " + describe(fin) + L" " + err);
        // Cut without encoding again (#22): the frames from the keyframe before the range on, each in its place, with
        // the sound; the editor picks it for a video saved as MP4 at 고화질 and says why when it can't.
        {
            std::wstring copy = dir + L"\\video_cut_copy.mp4", why;
            auto src = mp4::CutSource::Open(finished);
            auto vc = VideoClip::Open(finished, why);
            bool keys = src && vc && src->Frames() == vc->Count();
            for (size_t i = 0; keys && i < vc->Count(); ++i) keys = src->KeyBefore(i) == vc->KeyBefore(i);
            std::atomic<bool> stop{false};
            std::atomic<float> progress{0};
            mp4::CutSource::Result res;
            const uint64_t guess = src ? src->SizeOf(40, 99, true) : 0;
            bool cut = keys && src->KeyBefore(40) == 30 && src->Cut(40, 99, true, copy, stop, progress, res, why);
            auto out = cut ? VideoClip::Open(copy, why) : nullptr;
            int wrong = 0;
            if (out) {
                auto rd = out->NewReader();
                for (size_t k = 0; k < out->Count(); ++k) {
                    const uint8_t* px = rd->Get(k);
                    if (!px || ReadIndex(px, out->Width(), out->Height()) != 30 + (int)k) ++wrong;
                }
            }
            const uint64_t bytes = util::FileSize(copy);
            vcheck(out && out->Count() == 70 && wrong == 0 && out->HasAudio() && res.frames == 70 && res.audio &&
                       std::fabs(out->Seconds() - 70 / 60.0) < 0.02 && bytes > 0 &&
                       std::fabs((double)guess - bytes) < bytes * 0.15,
                   util::Format(L"cut as it is: keyframes as VideoClip has them %d, %zu frames from 30, %d out of "
                                L"place, %.3fs, audio %d, %s (guessed %s) %s",
                                keys, out ? out->Count() : 0, wrong, out ? out->Seconds() : 0.0, out && out->HasAudio(),
                                util::FormatBytes(bytes).c_str(), util::FormatBytes(guess).c_str(), why.c_str()));
            // The sound stays with the picture: the tone that changes at 1 s in the source changes 0.5 s in.
            if (out && vc) {
                int rate = 0, outRate = 0;
                auto before = ReadAllAudio(*vc, 0, 2.0, rate), after = ReadAllAudio(*out, 0, out->Seconds(), outRate);
                double was = ToneSwitch(before, rate, 0.5, 440, 880), now = ToneSwitch(after, outRate, 0.1, 440, 880);
                double want = was - vc->Ts(30) / 1e7;
                vcheck(was > 0 && std::fabs(now - want) < 0.02,
                       util::Format(L"the cut's sound in step: tone changes at %.3fs, expected %.3fs (%d / %d Hz)", now,
                                    want, rate, outRate));
            }

            OpenEditor(VideoClip::Open(finished, why));
            const wchar_t* direct = CopyBlocker();
            const bool picked = cutSource_ && direct && !*direct;
            std::wstring reasons;
            auto reason = [&] {
                const wchar_t* r = CopyBlocker();
                reasons += std::wstring(r ? r : L"(null)") + L"/";
            };
            settings_.speedIdx = 2;
            reason();
            settings_.speedIdx = 1;
            caption_ = L"자막";
            reason();
            caption_.clear();
            crop_.right -= 16;
            reason();
            ResetCrop();
            settings_.scaleIdx = 2;
            reason();
            settings_.scaleIdx = 0;
            inF_ = 40;
            outF_ = 99;
            ExportJob job = MakeJob(settings_.scaleIdx, settings_.outFpsIdx, 0);
            job.path = dir + L"\\video_cut_editor.mp4";
            if (const wchar_t* w = CopyBlocker(); w && !*w) job.cut = cutSource_;
            ExportResult r = ExportSync(ex, job);
            vcheck(picked && reasons == L"재생 속도를 바꿔서/자막을 넣어서/화면을 잘라서/크기나 프레임 레이트를 바꿔서/" &&
                       job.cut && r.ok && r.frames == 70 && r.audio,
                   util::Format(L"the editor cuts a video at 고화질 as it is (%d), else says why: %s; export %d, %d "
                                L"frames, audio %d %s",
                                picked, reasons.c_str(), r.ok, r.frames, r.audio, r.error.c_str()));
            CloseEditor();
            CloseClip();
        }

        // A child process that crashes mid-recording; it has gone, so the next start takes its file.
        std::wstring crashed = dir + L"\\video_crashed.mp4", cutMdat = dir + L"\\video_cut_mdat.mp4",
                     cutMoof = dir + L"\\video_cut_moof.mp4", cutAll = dir + L"\\video_cut_all.mp4";
        for (const std::wstring& p : {crashed, cutMdat, cutMoof, cutAll}) DeleteFileW(p.c_str());
        std::wstring cmd = L"\"" + util::ExePath() + L"\" --crash-test video \"" + crashed + L"\"";
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        crash::Prune(0);
        auto markers = [] {
            size_t n = 0;
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((FrameStore::Dir() + L"\\OzMaker_*.video").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do ++n;
                while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            return n;
        };
        // Copies cut where a crash can cut: in the middle of an mdat, of a moof, and before the first fragment.
        bool frag = false;
        auto boxes = TopBoxes(crashed, &frag);
        std::vector<size_t> moofs;
        for (size_t i = 0; i < boxes.size(); ++i)
            if (boxes[i].type == "moof") moofs.push_back(i);
        bool copied = false;
        if (moofs.size() >= 3 && moofs[moofs.size() - 2] + 1 < boxes.size()) {
            const TopBox& moof = boxes[moofs[moofs.size() - 2]];
            const TopBox& mdat = boxes[moofs[moofs.size() - 2] + 1];
            copied = mdat.type == "mdat" && CopyFileW(crashed.c_str(), cutMdat.c_str(), FALSE) &&
                     CopyFileW(crashed.c_str(), cutMoof.c_str(), FALSE) &&
                     CopyFileW(crashed.c_str(), cutAll.c_str(), FALSE) && CutFile(cutMdat, mdat.pos + mdat.size / 2) &&
                     CutFile(cutMoof, moof.pos + moof.size / 2) && CutFile(cutAll, boxes[moofs[0]].pos + 4);
        }
        size_t before = markers();
        auto videos = Mp4Writer::Recover();
        Seen back = inspect(crashed);
        vcheck(frag && before == 1 && videos.size() == 1 && _wcsicmp(videos[0].c_str(), crashed.c_str()) == 0 &&
                   markers() == 0 && back.plain && back.frames >= N / 2 && back.frames <= N && back.wrong == 0 &&
                   back.tables && back.audio,
               util::Format(L"a recording a crash cut off comes back (fragmented %d, %zu marker(s), %zu found): ", frag,
                            before, videos.size()) +
                   describe(back));
        for (const std::wstring& cut : {cutMdat, cutMoof}) {
            std::wstring why;
            bool ok = mp4::Unfragment(cut, why);
            Seen s = inspect(cut);
            vcheck(copied && ok && s.plain && s.frames > 0 && s.frames < back.frames && s.wrong == 0 && s.tables,
                   L"cut short in " + std::wstring(cut == cutMdat ? L"an mdat" : L"a moof") + L": " + describe(s) +
                       L" " + why);
        }
        {
            std::wstring why;
            bool empty = false;
            bool ok = mp4::Unfragment(cutAll, why, &empty);
            vcheck(copied && !ok && empty, L"cut before any fragment: nothing to keep (" + why + L")");
        }
        uint64_t size = util::FileSize(cutMoof);
        std::wstring why;
        vcheck(mp4::Unfragment(cutMoof, why) && util::FileSize(cutMoof) == size && inspect(cutMoof).plain,
               L"converting a plain MP4 again changes nothing");
    }

    // HEVC while recording: Windows' fragmented MP4 sink refuses HEVC, so it has to be written as a plain MP4. These
    // are the settings that failed for a user (0.6.1–0.6.3: "AAC 인코더를 시작할 수 없습니다").
    if (!HasVideoProcessor() || (Mp4Writer::FindEncoder(true, true).empty() && Mp4Writer::FindEncoder(true, false).empty())) {
        log(L"HEVC recording: skipped (no HEVC encoder or GPU video processor)");
    } else {
        VideoConfig v;
        v.width = 2474;
        v.height = 1392;
        v.fps = 30;
        v.hevc = true;
        v.cbr = true;
        v.gopFrames = 150;
        v.fragmented = true;  // what recording asks for
        std::wstring err, got;
        const bool ok = ProbeMp4(dir + L"\\hevc.mp4", v, true, err, got);
        log(util::Format(L"HEVC recording: %s · %s", ok ? L"ok" : L"FAIL", (ok ? got : err + L" · " + got).c_str()));
        if (!ok) ++failures;
        DeleteFileW((dir + L"\\hevc.mp4").c_str());
    }

    // 대비 (#26): text is 4.5:1 or more on each panel it sits on, and so is the ink on each fill, in each palette.
    {
        using Named = std::pair<const D2D1_COLOR_F*, const wchar_t*>;
        const std::pair<const wchar_t*, theme::Palette> palettes[] = {
            {L"dark", theme::Dark()}, {L"light", theme::Light()}, {L"contrast", theme::HighContrastBlack()}};
        for (const auto& [name, p] : palettes) {
            double worst = 100;
            std::wstring at;
            auto need = [&](const Named& fg, const Named& bg) {
                double r = theme::ContrastRatio(*fg.first, *bg.first);
                if (r < worst) worst = r, at = std::wstring(fg.second) + L" on " + bg.second;
            };
            const Named panels[] = {{&p.Bg, L"Bg"}, {&p.Surface, L"Surface"}, {&p.Surface2, L"Surface2"},
                                    {&p.Surface3, L"Surface3"}};
            const Named inks[] = {{&p.TextColor, L"TextColor"}, {&p.TextDim, L"TextDim"},   {&p.TextFaint, L"TextFaint"},
                                  {&p.AccentHi, L"AccentHi"},   {&p.Link, L"Link"},         {&p.Success, L"Success"},
                                  {&p.Warning, L"Warning"}};
            for (const Named& bg : panels)
                for (const Named& fg : inks) need(fg, bg);
            for (const Named& bg : {panels[0], panels[1], panels[2]}) need({&p.Rec, L"Rec"}, bg);
            for (const Named& fill : {Named{&p.AccentFill, L"AccentFill"}, Named{&p.AccentFillHi, L"AccentFillHi"},
                                      Named{&p.RecFill, L"RecFill"}, Named{&p.RecFillHi, L"RecFillHi"},
                                      Named{&p.Badge, L"Badge"}})
                need({&p.OnFill, L"OnFill"}, fill);
            for (const Named& fill : {Named{&p.UpdateFill, L"UpdateFill"}, Named{&p.UpdateFillHi, L"UpdateFillHi"}})
                need({&p.UpdateInk, L"UpdateInk"}, fill);
            const bool ok = worst >= 4.5;
            log(util::Format(L"contrast %s: %s, lowest %.2f:1 (%s)", name, ok ? L"ok" : L"FAIL", worst, at.c_str()));
            if (!ok) ++failures;
        }
    }

    // 코덱 자동 (#42): H.264 takes a longest edge up to 4096; past that, 자동 records and exports HEVC where this PC
    // writes and plays it, and otherwise makes the picture smaller. On made-up encoders (hevcForTest_).
    {
        const Settings was = settings_;
        settings_.output = kOutputVideo;
        struct Case {
            int codec, hevcHere, w, h, res, ow, oh;
            bool hevc;
            const wchar_t* what;
        };
        const Case cases[] = {
            {kCodecAuto, 1, 3840, 2160, 0, 3840, 2160, false, L"자동, 4K: H.264"},
            {kCodecAuto, 1, 5120, 2160, 0, 5120, 2160, true, L"자동, 5K: HEVC at full size"},
            {kCodecAuto, 2, 5120, 2160, 0, 4096, 1728, false, L"자동, 5K, an HEVC encoder but nothing to play it: H.264, smaller"},
            {kCodecAuto, 0, 5120, 2160, 0, 4096, 1728, false, L"자동, 5K, no HEVC: H.264, smaller"},
            {kCodecAuto, 1, 7680, 4320, 1, 3840, 2160, false, L"자동, 8K in the 2160p box: H.264"},
            {kCodecH264, 1, 5120, 2160, 0, 4096, 1728, false, L"H.264, 5K: smaller"},
            {kCodecHevc, 1, 1920, 1080, 0, 1920, 1080, true, L"HEVC: HEVC"},
            {kCodecHevc, 0, 1920, 1080, 0, 1920, 1080, false, L"HEVC with no HEVC encoder: H.264"},
        };
        for (const Case& c : cases) {
            settings_.vCodec = c.codec;
            settings_.vResIdx = c.res;
            hevcForTest_ = c.hevcHere;
            int ow = 0, oh = 0;
            bool hevc = false;
            PlannedSize(c.w, c.h, ow, oh, &hevc);
            const bool ok = ow == c.ow && oh == c.oh && hevc == c.hevc;
            log(util::Format(L"codec: %s %s -> %dx%d %s", ok ? L"ok  " : L"FAIL", c.what, ow, oh, hevc ? L"HEVC" : L"H.264"));
            if (!ok) ++failures;
        }
        // An export to MP4 goes by the same rule: a 4800×200 strip, wider than H.264 takes and light to make.
        OpenEditor(MakeSyntheticClip(4800, 200, 4, 30));
        playing_ = false;
        fmt_ = (int)Format::Mp4;
        settings_.vCodec = kCodecAuto;
        hevcForTest_ = 0;
        const ExportJob smaller = MakeJob(0, 0, 0);
        hevcForTest_ = 1;
        const ExportJob full = MakeJob(0, 0, 0);
        const bool ok = smaller.outW == 4096 && smaller.outH == 170 && !smaller.video.hevc && full.outW == 4800 &&
                        full.outH == 200 && full.video.hevc;
        log(util::Format(L"codec: %s export of 4800x200 to MP4: %dx%d %s without HEVC, %dx%d %s with it",
                         ok ? L"ok  " : L"FAIL", smaller.outW, smaller.outH, smaller.video.hevc ? L"HEVC" : L"H.264",
                         full.outW, full.outH, full.video.hevc ? L"HEVC" : L"H.264"));
        if (!ok) ++failures;
        CloseEditor();
        StopThumbnails();
        clip_.reset();
        fmt_ = (int)Format::Gif;
        hevcForTest_ = -1;
        settings_ = was;
    }
    // The record timer (#47): its length in words, and 직접 입력 kept between a minute and 24 hours.
    {
        Settings t;
        t.timerIdx = kTimerCustom;
        t.timerMinutes = 0;
        const int lo = t.TimerMinutes();
        t.timerMinutes = 5000;
        const int hi = t.TimerMinutes();
        t.timerIdx = 3;
        const bool ok = lo == 1 && hi == kTimerMaxMinutes && t.TimerMinutes() == 120 && TimerWords(200) == L"3시간 20분" &&
                        TimerWords(30) == L"30분" && TimerWords(120) == L"2시간";
        log(util::Format(L"timer: %s 1 min – 24 h, \"%s\"", ok ? L"ok" : L"FAIL", TimerWords(200).c_str()));
        if (!ok) ++failures;
    }
    // Taking over a hotkey (#50): a combination another window registered reaches OzMaker through the keyboard hook,
    // and that window no longer gets it. F23, which keyboards don't have, pressed with SendInput: first without the
    // hook (the other window gets it, so input reaches this desktop), then with it. Skipped where input can't be sent.
    {
        HWND other = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        HWND mine = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        const bool registered = other && mine && RegisterHotKey(other, 7, MOD_NOREPEAT, VK_F23);
        auto press = [] {
            INPUT in[2] = {};
            for (INPUT& i : in) {
                i.type = INPUT_KEYBOARD;
                i.ki.wVk = VK_F23;
            }
            in[1].ki.dwFlags = KEYEVENTF_KEYUP;
            return SendInput(2, in, sizeof(INPUT)) == 2;
        };
        int toOther = 0, toMine = 0;
        auto pump = [&](double seconds) {
            toOther = toMine = 0;
            for (double until = util::NowSeconds() + seconds; util::NowSeconds() < until;) {
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_HOTKEY && msg.hwnd == other) ++toOther;
                    if (msg.message == WM_HOTKEY && msg.hwnd == mine && msg.wParam == 9) ++toMine;
                    DispatchMessageW(&msg);
                }
                Sleep(10);
            }
        };
        bool reaches = false;
        if (registered && press()) {
            pump(0.5);
            reaches = toOther == 1;
        }
        if (!reaches) {
            log(L"hotkey takeover: skipped, no keyboard input reaches this desktop");
        } else {
            KeyHook hook;
            const bool set = hook.Set(mine, WM_APP + 77, {{0, VK_F23, 9}}, false);
            press();
            pump(0.5);
            hook.Stop();
            const bool ok = set && toMine == 1 && toOther == 0;
            log(util::Format(L"hotkey takeover: %s hook %d, OzMaker got it %d time(s), the other window %d",
                             ok ? L"ok" : L"FAIL", set, toMine, toOther));
            if (!ok) ++failures;
        }
        if (registered) UnregisterHotKey(other, 7);
        if (other) DestroyWindow(other);
        if (mine) DestroyWindow(mine);
    }

    // English (#26): each format string keeps its fields in the same order, and a lookup finds the text.
    {
        auto bad = MismatchedTranslations();
        SetLanguage(Lang::English);
        const bool found = wcscmp(tr(L"녹화"), L"Record") == 0 && wcscmp(tr(L"16:9"), L"16:9") == 0;
        const bool none = MissingTranslations().empty();
        SetLanguage(Lang::Korean);
        const bool ok = bad.empty() && found && none && wcscmp(tr(L"녹화"), L"녹화") == 0;
        log(util::Format(L"English: %s, %zu texts, %zu with other fields", ok ? L"ok" : L"FAIL", TranslationCount(),
                         bad.size()));
        for (const std::wstring& k : bad) log(L"  fields differ: " + k);
        if (!ok) ++failures;
    }
    // Frame images (#62): what is written reads back, and the frame is cut as the editor shows it.
    {
        auto check = [&](bool ok, const std::wstring& what) {
            log((ok ? L"ok   " : L"FAIL ") + what);
            if (!ok) ++failures;
        };
        std::wstring err;
        const int w = 64, h = 48;
        image::Picture pic;
        pic.w = w;
        pic.h = h;
        pic.px.resize((size_t)w * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t* p = &pic.px[((size_t)y * w + x) * 4];
                p[0] = (uint8_t)(x * 4);
                p[1] = (uint8_t)(y * 5);
                p[2] = 128;
                p[3] = 0;  // opaque whatever this says
            }
        image::Picture see = pic;  // left half clear, right half red at half strength
        see.alpha = true;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t* p = &see.px[((size_t)y * w + x) * 4];
                p[0] = p[1] = 0;
                p[2] = p[3] = x < w / 2 ? 0 : 128;
            }
        std::vector<uint8_t> got;
        int gw = 0, gh = 0;
        auto allOpaque = [&] {
            for (size_t i = 3; i < got.size(); i += 4)
                if (got[i] != 255) return false;
            return true;
        };
        auto at = [&](int x, int y) { return &got[((size_t)y * gw + x) * 4]; };
        const std::wstring png = dir + L"\\image.png", jpg = dir + L"\\image.jpg", webp = dir + L"\\image.webp";
        const std::wstring seePng = dir + L"\\image_see.png", seeJpg = dir + L"\\image_see.jpg";

        bool ok = image::Write(png, image::kPng, pic, err) && ReadImageFile(png, got, gw, gh);
        check(ok && gw == w && gh == h && WorstColour(got, pic.px) == 0 && allOpaque(),
              L"image: an opaque PNG reads back pixel for pixel");
        ok = image::Write(jpg, image::kJpg, pic, err) && ReadImageFile(jpg, got, gw, gh);
        int worst = ok ? WorstColour(got, pic.px) : 256;
        check(ok && gw == w && gh == h && worst <= 10 && allOpaque(), util::Format(L"image: JPG, off by %d at most", worst));
        ok = image::Write(seePng, image::kPng, see, err) && ReadImageFile(seePng, got, gw, gh);
        check(ok && at(5, 5)[3] == 0 && at(w - 5, 5)[3] == 128 && at(w - 5, 5)[2] == 255 && at(w - 5, 5)[0] == 0,
              L"image: a see-through PNG keeps its alpha, as straight colour");
        ok = image::Write(seeJpg, image::kJpg, see, err) && ReadImageFile(seeJpg, got, gw, gh);
        check(ok && allOpaque() && at(5, 5)[2] < 12 && std::abs(at(w - 5, 5)[2] - 128) < 12,
              L"image: JPG shows what is see-through over black");
        if (image::Available(image::kWebp)) {
            bool alpha = true;
            ok = image::Write(webp, image::kWebp, pic, err) && ReadWebpFile(webp, got, gw, gh, alpha);
            worst = ok ? WorstColour(got, pic.px) : 256;
            check(ok && gw == w && gh == h && !alpha && worst <= 24, util::Format(L"image: WebP, off by %d at most", worst));
            const std::wstring seeWebp = dir + L"\\image_see.webp";
            ok = image::Write(seeWebp, image::kWebp, see, err) && ReadWebpFile(seeWebp, got, gw, gh, alpha);
            check(ok && alpha && at(5, 5)[3] == 0 && std::abs(at(w - 5, 5)[3] - 128) <= 2,
                  L"image: a see-through WebP keeps its alpha");
        } else {
            log(L"image: WebP skipped, no libwebp in this build");
        }
        const std::wstring lost = dir + L"\\no such folder\\image.png";
        check(!image::Write(lost, image::kPng, pic, err) && !err.empty(), L"image: a folder that isn't there says so");

        const std::vector<uint8_t> dib = image::ToDib(see);
        const auto* bi = (const BITMAPINFOHEADER*)dib.data();
        const uint8_t* bits = dib.data() + sizeof(BITMAPINFOHEADER);
        check(dib.size() == sizeof(BITMAPINFOHEADER) + (size_t)w * h * 4 && bi->biWidth == w && bi->biHeight == h &&
                  bi->biBitCount == 32 && bits[3] == 255 && bits[((size_t)w - 1) * 4 + 2] == 128,
              L"image: the clipboard picture is bottom-up and opaque");

        auto clip = MakeSyntheticClip(320, 180, 10, 10.0);
        auto reader = clip ? clip->NewReader() : nullptr;
        const uint8_t* src = reader ? reader->Get(4) : nullptr;
        std::vector<uint8_t> frame4;
        if (src) frame4.assign(src, src + (size_t)320 * 180 * 4);
        const RECT all{0, 0, 320, 180}, crop{40, 30, 200, 130};
        image::Picture whole, cut, titled;
        check(!frame4.empty() && image::TakeFrame(*clip, 4, all, L"", 0, whole, err) && whole.w == 320 && whole.h == 180 &&
                  !whole.alpha && whole.px == frame4,
              L"image: a frame with nothing set is the frame itself");
        bool rows = !frame4.empty() && image::TakeFrame(*clip, 4, crop, L"", 0, cut, err) && cut.w == 160 && cut.h == 100;
        for (int y = 0; rows && y < cut.h; ++y)
            rows = !memcmp(&cut.px[(size_t)y * 160 * 4], &frame4[((size_t)(30 + y) * 320 + 40) * 4], (size_t)160 * 4);
        check(rows, L"image: the crop cuts the frame at full size");
        const bool made = !frame4.empty() && image::TakeFrame(*clip, 4, all, L"버그는 여기", caption::kBottom, titled, err);
        const size_t half = (size_t)320 * 90 * 4;
        check(made && !memcmp(titled.px.data(), frame4.data(), half) &&
                  memcmp(titled.px.data() + half, frame4.data() + half, half) != 0,
              L"image: the caption lies over the bottom of the frame");
        check(clip && !image::TakeFrame(*clip, 4, RECT{50, 50, 50, 90}, L"", 0, cut, err) && !err.empty(),
              L"image: an empty crop is not saved");
        image::Picture see2;
        check(clip && image::TakeFrame(AlphaClip(clip), 0, all, L"", 0, see2, err) && see2.alpha,
              L"image: a see-through clip stays see-through");

        // A screenshot leaves black what covers the window, as the recording does (#63); rects can reach past the edge.
        image::Picture covered;
        covered.w = 40;
        covered.h = 30;
        covered.px.assign((size_t)40 * 30 * 4, 200);
        Capture::BlackOut(covered, {RECT{10, 5, 20, 15}, RECT{35, 25, 90, 90}, RECT{-5, -5, 2, 2}});
        auto dark = [&](int x, int y) { return covered.px[((size_t)y * 40 + x) * 4] == 0; };
        check(dark(10, 5) && dark(19, 14) && dark(39, 29) && dark(0, 0) && !dark(20, 5) && !dark(10, 15) && !dark(34, 25),
              L"shot: what covers the window is black, and nothing else");
        image::Picture none;
        check(!Capture().Shot(none), L"shot: nothing to take without a capture");

        std::atomic<int> ran{0};
        std::wstring order;
        {
            image::Saver saver;
            for (int i = 0; i < 3; ++i)
                saver.Add(
                    [i, &ran] {
                        Sleep(15);
                        ++ran;
                        image::Saved r;
                        r.ok = true;
                        r.path = std::to_wstring(i);
                        return r;
                    },
                    nullptr, 0);
            const double until = util::NowSeconds() + 5;
            while (saver.Busy() && util::NowSeconds() < until) Sleep(5);
            image::Saved r;
            while (saver.Take(r)) order += r.path;
            saver.Add(
                [&ran] {
                    Sleep(15);
                    ++ran;
                    return image::Saved{};
                },
                nullptr, 0);
        }
        check(order == L"012" && ran == 4, L"image: saves run in order, and what waits at exit is still written");
    }

    log(failures ? util::Format(L"RESULT: %d failure(s)", failures) : L"RESULT: all passed");
    return failures ? 2 : 0;
}

// A crash on purpose, for the self-test (#24): `access` and `abort` for the crash report; `record` with a 움짤
// recording on disk and `video <path>` in the middle of an MP4, for recovery.
int App::RunCrashTest(const std::vector<std::wstring>& args) {
    if (args[1] == L"abort") std::abort();
    std::shared_ptr<FrameStore> rec;
    if (args[1] == L"record") {  // 45 frames on disk, never finished
        rec = MakeSyntheticClip(160, 90, 45, 30, false);
        if (rec) rec->SetNominalFps(30);  // as Capture does
        Sleep(1000);
    }
    if (args[1] == L"video" && args.size() >= 3) {  // 2 s written as a recording is, never finished
        std::wstring err;
        WriteTestMp4(args[2], 320, 180, 120, 60, 30, err, true, true);
    }
    volatile int* nowhere = nullptr;
    *nowhere = 1;
    return 1;
}

// What DWM puts over the window preview, for screenshots (drawn offscreen, without it): the parts it would show,
// drawn last as DWM does, in a flat colour that hides whatever was wrongly left under them.
void App::DrawPreviewStandIn(ID2D1DeviceContext* dc) {
    ComPtr<ID2D1SolidColorBrush> fill, edge;
    dc->CreateSolidColorBrush(D2D1::ColorF(0x2F4A73), &fill);
    dc->CreateSolidColorBrush(D2D1::ColorF(0x6F94CC), &edge);
    const float s = Scale();
    auto draw = [&](const RECT& dest) {
        for (const RECT& p : PreviewParts(dest)) {
            D2D1_RECT_F r{p.left / s, p.top / s, p.right / s, p.bottom / s};
            dc->FillRectangle(r, fill.Get());
            dc->DrawRectangle({r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f}, edge.Get(), 1.f);
        }
    };
    RECT dest, src;
    if (thumbWanted_ && target_ && IsWindow(target_)) {
        PreviewLayout(dest, src);
        draw(dest);
    }
    for (const PreviewCell& c : thumbCells_)  // 여러 창 녹화's grid
        if (IsWindow(c.hwnd)) {
            PreviewLayout(LivePreview{}, c.hwnd, c.area, dest, src);
            draw(dest);
        }
}

int App::RunScreenshots(const std::wstring& dir, const std::wstring& look) {
    testWindowsOnly_ = true;
    util::EnsureDir(dir);
    Log log(dir + L"\\screenshots.log");
    // The same shots whatever Windows is set to: dark and Korean unless asked for another look.
    settings_.theme = look == L"light" ? kThemeLight : kThemeDark;
    settings_.language = look == L"en" ? kLangEnglish : kLangKorean;
    contrastForTest_ = look == L"contrast";
    ApplyLanguage();
    ApplyTheme();
    if (!look.empty()) log(L"look: " + look);
    int layerErrors = 0;
    size_t controls = 0, shots = 0;  // named for screen readers
    float W = 1120, H = 780;
    double clock = util::NowSeconds();
    UiInput mouse;  // where the pointer is in the next shot
    auto shot = [&](const std::wstring& name, float dpi) {
        dpi_ = (UINT)dpi;
        UINT pw = (UINT)(W * dpi / 96), ph = (UINT)(H * dpi / 96);
        // Several passes on a simulated clock so animated values settle on their targets.
        for (int pass = 0; pass < 10; ++pass) {
            clock += 0.05;
            gfx_.RenderToPng((dir + L"\\" + name).c_str(), pw, ph, dpi, [&](ID2D1DeviceContext* dc) {
                ui_.Begin(dc, mouse, clock);
                thumbWanted_ = false;
                thumbCells_.clear();
                DrawFrame(Rect(0, 0, W, H));
                ui_.End();
                if (pass == 9 && ui_.LayerErrors()) {
                    const Rect& e = ui_.FirstLayerError();
                    log(util::Format(L"FAIL %s: %d layering error(s), first at %.0f,%.0f-%.0f,%.0f", name.c_str(),
                                     ui_.LayerErrors(), e.l, e.t, e.r, e.b));
                    ++layerErrors;
                }
                // Every control a screen reader can reach has a name (#26).
                if (pass == 9) {
                    std::wstring unnamed;
                    controls += ui_.Nodes().size();
                    ++shots;
                    for (const UiNode& n : ui_.Nodes())
                        if (n.name.empty() && !n.full.Empty()) {
                            const Rect& r = n.full;
                            unnamed += util::Format(L" %08x at %.0f,%.0f-%.0f,%.0f", n.id, r.l, r.t, r.r, r.b);
                        }
                    if (!unnamed.empty()) {
                        log(L"FAIL " + name + L": controls without a name:" + unnamed);
                        ++layerErrors;
                    }
                }
                DrawPreviewStandIn(dc);
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
    settings_.vFrameMode = 1;  // 가변 프레임 (#55) on the line under the options
    shot(L"home_video_vfr.png", 96);
    settings_.vFrameMode = 0;
    // The record timer's popover (#47), on 직접 입력 at 3시간 20분, stopping.
    settings_.timerIdx = kTimerCustom;
    settings_.timerMinutes = 200;
    settings_.timerStop = true;
    timerOpen_ = true;
    shot(L"home_timer.png", 96);
    timerOpen_ = false;
    // While recording: the time the timer is set to after the time, and once it is up, in amber.
    rec_ = RecState::Recording;
    recElapsedForTest_ = 12034.5;
    timerFiredFor_ = 200 * 60;
    shot(L"home_recording_timer.png", 96);
    rec_ = RecState::Idle;
    recElapsedForTest_ = -1;
    timerFiredFor_ = 0;
    settings_.timerIdx = 0;
    settings_.timerStop = false;
    // 코덱 자동 on a 5K screen (#42): HEVC at full size where this PC plays it; else H.264, made smaller.
    settings_.source = kSourceDisplay;
    sourceForTest_ = {5120, 2160};
    hevcForTest_ = 1;
    shot(L"home_hevc.png", 96);
    hevcForTest_ = 2;
    shot(L"home_hevc_noext.png", 96);
    hevcForTest_ = -1;
    sourceForTest_ = {};
    settings_.source = kSourceWindow;
    lastVideo_ = {};
    lastVideo_.video = lastVideo_.ok = lastVideo_.audio = true;
    lastVideo_.path = settings_.outDir + L"\\OzMaker_20261102_142311.mp4";
    lastVideo_.bytes = 48'213'004;
    lastVideo_.seconds = 74.2;
    lastVideo_.encoder = L"NVIDIA H.264 Encoder MFT";
    lastVideo_.hardware = true;
    showVideo_ = true;
    shot(L"home_video_saved.png", 96);
    lastShots_ = 3;  // 스크린샷 (#63): their own card over it
    lastShot_ = ImageDir() + L"\\OzMaker_20261102_142311.png";
    shot(L"home_video_shots.png", 96);
    lastShots_ = 0;
    // A long recording's gaps (#69): one, and more than fit; and one that stopped by itself.
    tm noon{};
    noon.tm_year = 126;
    noon.tm_mon = 10;
    noon.tm_mday = 7;
    noon.tm_hour = 13;
    noon.tm_min = 12;
    noon.tm_isdst = -1;
    const int64_t at = (int64_t)mktime(&noon);
    lastVideo_.seconds = 6120;
    lastVideo_.bytes = 5'200'000'000;
    lastVideo_.gaps = {{at, 38 * 60, CaptureGap::Why::Asleep}};
    shot(L"home_video_gap.png", 96);
    for (int i = 1; i < 5; ++i)
        lastVideo_.gaps.push_back({at + i * 1800, 80.0 * i, i % 2 ? CaptureGap::Why::Locked : CaptureGap::Why::Asleep});
    shot(L"home_video_gaps.png", 96);
    lastVideo_.gaps.clear();
    lastVideo_.seconds = 760;
    lastVideo_.bytes = 1'100'000'000;
    lastVideo_.failure = tr(L"그래픽 장치가 응답하지 않아 더 쓸 수 없었습니다. 그래픽 드라이버를 확인하거나 PC를 다시 시작해 보세요.");
    shot(L"home_video_failed.png", 96);
    lastVideo_.failure.clear();
    lastVideo_.seconds = 74.2;
    lastVideo_.bytes = 48'213'004;
    showVideo_ = false;
    // 여러 창 녹화 (#48): the switch on with nothing checked; two windows side by side; four checked (the fifth row
    // dimmed) in a 2 × 2 grid; the card for three files, one closed early and one encoded on the CPU.
    {
        const std::vector<WinInfo> realWins = windows_;
        const HWND realTarget = target_;
        const bool en = look == L"en";
        const wchar_t* titles[] = {en ? L"Demo Game - 1P" : L"데모 게임 - 1P", en ? L"Demo Game - 2P" : L"데모 게임 - 2P",
                                   en ? L"Chat" : L"채팅", en ? L"Map" : L"지도", en ? L"Notes" : L"메모"};
        std::vector<WinInfo> fake;
        for (int i = 0; i < 5; ++i) {
            WinInfo w;
            w.hwnd = MakeStandInWindow(inst_);
            w.title = titles[i];
            w.exeName = i < 2 ? L"DemoGame.exe" : i == 2 ? L"Chat.exe" : i == 3 ? L"Map.exe" : L"Memo.exe";
            w.frame = {0, 0, 1280, 720};
            SetWindowPos(w.hwnd, nullptr, 0, 0, 1280, 720, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            icons_[w.hwnd] = nullptr;
            fake.push_back(w);
        }
        windows_ = fake;
        settings_.multiWindow = true;
        picks_.clear();
        target_ = nullptr;
        shot(L"home_multi_empty.png", 96);
        picks_ = {fake[0].hwnd, fake[1].hwnd};
        target_ = picks_[0];
        shot(L"home_multi_two.png", 96);
        picks_ = {fake[0].hwnd, fake[1].hwnd, fake[2].hwnd, fake[3].hwnd};
        target_ = picks_[0];
        shot(L"home_multi_full.png", 96);
        shot(L"home_multi_full_150.png", 144);
        picks_ = {fake[0].hwnd, fake[1].hwnd, fake[2].hwnd};
        target_ = picks_[0];
        shot(L"home_multi_three.png", 96);
        lastVideos_.clear();
        for (int i = 0; i < 3; ++i) {
            CaptureResult r = lastVideo_;
            r.path = settings_.outDir + L"\\OzMaker_20261105_201500_" + util::SafeFileName(titles[i], 60) + L".mp4";
            r.seconds = i == 1 ? 41.7 : 74.2;
            r.bytes = i == 1 ? 21'004'512 : i == 2 ? 30'118'230 : 48'213'004;
            r.hardware = i != 2;
            if (i == 2) r.encoder = L"H264 Encoder MFT";
            lastVideos_.push_back({r, i == 1});
        }
        lastVideo_ = lastVideos_[0].r;
        showVideo_ = true;
        shot(L"home_multi_saved.png", 96);
        lastShots_ = 6;
        shot(L"home_multi_shots.png", 96);
        lastShots_ = 0;
        lastVideos_[0].r.gaps = lastVideos_[2].r.gaps = {{at, 38 * 60, CaptureGap::Why::Locked}};
        lastVideos_[1].r.failure = L"-";
        shot(L"home_multi_gap.png", 96);
        lastVideo_.gaps.clear();
        showVideo_ = false;
        lastVideos_.clear();
        settings_.multiWindow = false;
        picks_.clear();
        for (const WinInfo& w : fake) {
            icons_.erase(w.hwnd);
            DestroyWindow(w.hwnd);
        }
        windows_ = realWins;
        target_ = realTarget;
    }
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
        // The recording pill: recording (stop hovered) and paused (resume hovered), then with the record timer
        // (#47) and with its time up, and recording three windows (#48), at 150 %.
        gfx_.RenderToPng((dir + L"\\rec_pill.png").c_str(), 510, 342, 144, [&](ID2D1DeviceContext* dc) {
            Painter p;
            p.Bind(dc);
            p.Fill(Rect(0, 0, 340, 228), theme::Hex(0x2B4C7E));
            dc->SetTransform(D2D1::Matrix3x2F::Translation(6, 6));
            overlay_.PaintTest(p, 73.4, false, 2);
            dc->SetTransform(D2D1::Matrix3x2F::Translation(6, 50));
            overlay_.PaintTest(p, 73.4, true, 1);
            dc->SetTransform(D2D1::Matrix3x2F::Translation(6, 94));
            overlay_.PaintTest(p, 4271.2, false, 0, 200 * 60);
            dc->SetTransform(D2D1::Matrix3x2F::Translation(6, 138));
            overlay_.PaintTest(p, 12034.5, false, 0, 200 * 60, true);
            dc->SetTransform(D2D1::Matrix3x2F::Translation(6, 182));
            overlay_.PaintTest(p, 73.4, false, 0, 0, false, 3);  // 여러 창 녹화 (#48): three windows
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
        });
        // 순간 리플레이's save notice (#20): with 편집 · 폴더 (편집 under the pointer), over a game that holds the mouse
        // (no buttons), and when the save failed, at 150 %.
        {
            ReplayNotice::Content saved{tr(L"리플레이를 저장했습니다"),
                                        util::Format(tr(L"방금 %s · MP4 · %s"), L"0:30", util::FormatBytes(48ull << 20).c_str()),
                                        true, true};
            ReplayNotice::Content quiet = saved;
            quiet.buttons = false;
            ReplayNotice::Content failed{tr(L"리플레이를 저장하지 못했습니다"),
                                         tr(L"파일을 저장하지 못했습니다. 디스크 공간을 확인하세요."), false, false};
            ReplayNotice::Content taken{tr(L"스크린샷을 저장했습니다"), ImageDirName() + L"\\OzMaker_20261105_142311.png"};
            taken.glyph = theme::icon::Camera;
            const float nw = std::max({ReplayNotice::Width(saved), ReplayNotice::Width(quiet), ReplayNotice::Width(failed),
                                       ReplayNotice::Width(taken)}) + 12;
            const float nh = ReplayNotice::kHeight * 4 + 30;
            gfx_.RenderToPng((dir + L"\\replay_notice.png").c_str(), (UINT)(nw * 1.5f), (UINT)(nh * 1.5f), 144,
                             [&](ID2D1DeviceContext* dc) {
                                 Painter p;
                                 p.Bind(dc);
                                 p.Fill(Rect(0, 0, nw, nh), theme::Hex(0x2B4C7E));
                                 dc->SetTransform(D2D1::Matrix3x2F::Translation(6, 6));
                                 rpNotice_.PaintTest(p, saved, ReplayNotice::kEdit);
                                 dc->SetTransform(D2D1::Matrix3x2F::Translation(6, 12 + ReplayNotice::kHeight));
                                 rpNotice_.PaintTest(p, quiet, 0);
                                 dc->SetTransform(D2D1::Matrix3x2F::Translation(6, 18 + ReplayNotice::kHeight * 2));
                                 rpNotice_.PaintTest(p, failed, 0);
                                 dc->SetTransform(D2D1::Matrix3x2F::Translation(6, 24 + ReplayNotice::kHeight * 3));
                                 rpNotice_.PaintTest(p, taken, 0);
                                 dc->SetTransform(D2D1::Matrix3x2F::Identity());
                             });
        }
        gfx_.RenderToPng((dir + L"\\region_adjust.png").c_str(), mw, mh, 96, [&](ID2D1DeviceContext* dc) {
            Painter p;
            p.Bind(dc);
            p.Fill(Rect(0, 0, (float)mw, (float)mh), theme::Hex(0x2B4C7E));
            p.Fill(Rect::XYWH(mw * 0.18f, mh * 0.16f, mw * 0.5f, mh * 0.55f), theme::Hex(0xF3F4F6), 8);
            p.Fill(Rect::XYWH(mw * 0.3f, mh * 0.3f, mw * 0.25f, mh * 0.3f), theme::Hex(0xFF5E87), 6);
            POINT a{mr.left + mw * 26 / 100, mr.top + mh * 27 / 100}, c{mr.left + mw * 60 / 100, mr.top + mh * 66 / 100};
            regionPicker_.PaintTest(p, mr, a, c, RegionPicker::TestView::Adjust);
        });
        // Opened again on the area picked before, with the toolbar's countdown menu open.
        gfx_.RenderToPng((dir + L"\\region_last.png").c_str(), mw, mh, 96, [&](ID2D1DeviceContext* dc) {
            Painter p;
            p.Bind(dc);
            p.Fill(Rect(0, 0, (float)mw, (float)mh), theme::Hex(0x2B4C7E));
            p.Fill(Rect::XYWH(mw * 0.18f, mh * 0.16f, mw * 0.5f, mh * 0.55f), theme::Hex(0xF3F4F6), 8);
            p.Fill(Rect::XYWH(mw * 0.3f, mh * 0.3f, mw * 0.25f, mh * 0.3f), theme::Hex(0xFF5E87), 6);
            POINT a{mr.left + mw * 30 / 100, mr.top + mh * 30 / 100}, c{mr.left + mw * 58 / 100, mr.top + mh * 62 / 100};
            regionPicker_.PaintTest(p, mr, a, c, RegionPicker::TestView::Last);
        });
    }

    // A new version downloaded: the green button, its tooltip, and the popover it opens.
    {
        Updater::Status st;
        st.state = Updater::State::Ready;
        st.version = L"0.5.0";
        updater_.SetForTest(st);
        screen_ = Screen::Home;
        shot(L"home_update.png", 96);
        mouse.mx = updateBtn_.CX();
        mouse.my = updateBtn_.CY();
        shot(L"home_update_tip.png", 96);
        mouse = {};
        updateOpen_ = true;
        shot(L"home_update_popover.png", 96);
        updateOpen_ = false;
        screen_ = Screen::Settings;
        settings_.settingsTab = kTabAbout;
        shot(L"settings_about_update.png", 96);
        updater_.SetForTest({});
    }

    screen_ = Screen::Settings;
    const wchar_t* tabShots[kTabCount] = {L"settings.png", L"settings_record.png", L"settings_video.png",
                                          L"settings_audio.png", L"settings_about.png", L"settings_replay_off.png"};
    for (int i = 0; i < kTabCount; ++i) {
        settings_.settingsTab = i;
        settingsScroll_ = 0;
        micTestShow_ = i == kTabAudio ? 0.6f : -1;  // the mic check running, on a made-up level
        shot(tabShots[i], 96);
    }
    {
        // 파일 이름 › 직접 입력: the field, the token buttons and the line under them, then a pattern with a
        // character a file name can't hold.
        const std::wstring nameWas = settings_.nameTemplate;
        const bool en = Language() == Lang::English;
        settings_.settingsTab = kTabGeneral;
        settingsScroll_ = 0;
        nameCustom_ = true;
        settings_.nameTemplate = en ? L"{window} {year}-{month}-{day} {hour}.{minute}.{second}"
                                    : L"{창 이름} {연}-{월}-{일} {시}.{분}.{초}";
        shot(L"settings_filename.png", 96);
        settings_.nameTemplate = en ? L"Clip: {window}_{number}" : L"클립: {창 이름}_{번호}";
        shot(L"settings_filename_warn.png", 96);
        nameCustom_ = false;
        settings_.nameTemplate = nameWas;
        settings_.deleteMode = kDeleteNow;
        shot(L"settings_delete.png", 96);
        settings_.deleteMode = kDeleteToBin;
    }
    {
        // 동영상 › 코덱 on HEVC: its line says a crash leaves no recording. On made-up encoders, so it shows the same
        // on a PC without HEVC.
        const int codecWas = settings_.vCodec;
        settings_.settingsTab = kTabVideo;
        settingsScroll_ = 0;
        settings_.vCodec = kCodecHevc;
        hevcForTest_ = 1;
        shot(L"settings_video_hevc.png", 96);
        hevcForTest_ = -1;
        settings_.vCodec = codecWas;
        // 비트레이트 (#53): 직접 입력 at 12.5 Mbps, its list open, then 200 typed and the field let go: it becomes
        // 150 Mbps and the line under the row says so.
        settings_.vBitrateIdx = kVideoBitrateCustom;
        settings_.vBitrateKbps = 12500;
        shot(L"settings_video_bitrate.png", 96);
        menu_ = {};
        menu_.owner = HashId("set.vbr");
        shot(L"settings_video_bitrate_menu.png", 96);
        menu_ = {};
        brText_ = L"200";
        brTypingWas_ = true;
        shot(L"settings_video_bitrate_note.png", 96);
        brNote_.clear();
        settings_.vBitrateIdx = 0;
        settings_.vBitrateKbps = 20000;
        // 프레임 레이트 방식 on 가변 (#55): the line under it, and 프레임 레이트 says it is the highest rate.
        settings_.vFrameMode = 1;
        shot(L"settings_video_vfr.png", 96);
        settings_.vFrameMode = 0;
        // 일반 › 언어, its list open: drawn like the rest of the app, not a Windows menu.
        settings_.settingsTab = kTabGeneral;
        menu_.owner = HashId("set.language");
        shot(L"settings_language_menu.png", 96);
        menu_ = {};
    }
    {
        // 설정 › 녹화 with 0.6.5's rows: the record timer on 직접 입력 (3시간 20분, stopping), 작업 표시줄로 최소화
        // and a pause hotkey taken over (가져옴); then the panel a held combination opens (#46 #47 #50).
        settings_.settingsTab = kTabRecord;
        settingsScroll_ = 0;
        settings_.timerIdx = kTimerCustom;
        settings_.timerMinutes = 200;
        settings_.timerStop = true;
        settings_.startHide = kHideMinimize;
        settings_.hkPauseHook = true;
        shot(L"settings_record_timer.png", 96);
        hkConflict_ = {kHotkeyRecord, MOD_ALT, VK_F9};
        shot(L"settings_record_conflict.png", 96);
        hkConflict_ = {};
        settings_.timerIdx = 0;
        settings_.timerStop = false;
        settings_.startHide = kHideToTray;
        settings_.hkPauseHook = false;
        // 설정 › 일반 › 설정 초기화, and what it asks (#49).
        settings_.settingsTab = kTabGeneral;
        settingsScroll_ = 10000;
        shot(L"settings_reset_row.png", 96);
        AskReset();
        shot(L"settings_reset.png", 96);
        question_.reset();
        settingsScroll_ = 0;
    }
    settings_.settingsTab = kTabAudio;  // the mic rows at the bottom, testing, with the playback switch
    settingsScroll_ = 10000;
    micTestShow_ = 0.6f;
    shot(L"settings_audio_mic.png", 96);
    settingsScroll_ = 0;
    micTestShow_ = -1;
    settings_.settingsTab = kTabGeneral;
    portableForTest_ = true;  // as a copy with settings.ini next to the exe shows it
    shot(L"settings_portable.png", 96);
    settings_.settingsTab = kTabAbout;
    shot(L"settings_about_portable.png", 96);
    portableForTest_ = false;
    {
        // A crash report on file (an empty stand-in in this test's own crash folder).
        CreateDirectoryW(crash::Dir().c_str(), nullptr);
        std::wstring fake = crash::Dir() + L"\\crash_20261102_142311_" OZ_VERSION_WSTR L".dmp";
        HANDLE f = CreateFileW(fake.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        shot(L"settings_about_crash.png", 96);
        DeleteFileW(fake.c_str());
    }
    {
        // The keyboard (#26): the ring where Tab took the focus, the page scrolled to show it.
        auto press = [&](const std::wstring& name, int tabs) {
            for (int k = 0; k < tabs; ++k) {
                UiInput in = mouse;
                in.keys = {VK_TAB};
                clock += 0.05;
                gfx_.RenderToPng((dir + L"\\" + name).c_str(), (UINT)W, (UINT)H, 96, [&](ID2D1DeviceContext* dc) {
                    ui_.Begin(dc, in, clock);
                    thumbWanted_ = false;
                    thumbCells_.clear();
                    DrawFrame(Rect(0, 0, W, H));
                    ui_.End();
                });
            }
            shot(name, 96);
        };
        settings_.settingsTab = kTabRecord;
        settingsScroll_ = 0;
        shot(L"settings_keyboard.png", 96);
        press(L"settings_keyboard.png", 9);
        screen_ = Screen::Home;
        shot(L"home_keyboard.png", 96);
        press(L"home_keyboard.png", 4);
        ui_.SetKeyboardFocus(0, false);
        screen_ = Screen::Settings;
    }
    settings_.settingsTab = kTabGeneral;

    OpenEditor(MakeSyntheticClip(960, 540, 120, 30));
    if (thumbThread_.joinable()) thumbThread_.join();  // let it finish (StopThumbnails would cancel)
    thumbs_.clear();
    for (auto& px : thumbPixels_) thumbs_.push_back(gfx_.CreateBitmap(thumbW_, thumbH_, px.data(), thumbW_ * 4));
    playing_ = false;
    inF_ = 12;
    outF_ = 100;
    playhead_ = 40;
    {
        // The estimate shows once it is ready; the screenshot doesn't wait for the worker.
        std::atomic<bool> cancel{false};
        estBytes_ = EstimateBytes(MakeJob(settings_.scaleIdx, settings_.outFpsIdx, settings_.quality[fmt_]), cancel);
        estKey_ = EstimateKey();
        estValid_ = true;
    }
    clipName_ = L"OzMaker Test Window";
    held_ = {{clip_, clock - 40, clipName_}};
    shot(L"editor.png", 96);
    // 자세히 설정 open, with 강하게 (the longest hint), which no preset has at this size: 직접 설정함.
    settings_.exportDetails = true;
    settings_.gifLossy = 2;
    settings_.exportPreset = settings_.MatchPreset(false, true);
    shot(L"editor_details.png", 96);
    fmt_ = (int)Format::Mp4;  // MP4: the same preset gives other values; 인코딩 instead of 품질
    settings_.ApplyPreset(settings_.exportPreset = 1, true);
    shot(L"editor_mp4.png", 96);
    settings_.vFrameMode = 1;  // 가변 (#55): on the 자세히 summary and the 인코딩 line
    shot(L"editor_mp4_vfr.png", 96);
    settings_.vFrameMode = 0;
    // WebM (#40): chosen on the second row, its 품질 and the VP9 line; under 용량 맞추기, the bitrate instead.
    fmt_ = (int)Format::WebM;
    shot(L"editor_webm.png", 96);
    settings_.targetIdx = 2;
    shot(L"editor_webm_target.png", 96);
    settings_.targetIdx = 0;
    fitBitrate_ = 0;
    fmt_ = (int)Format::Gif;
    settings_.exportDetails = false;
    settings_.ApplyPreset(settings_.exportPreset = 1, false);
    settings_.targetIdx = 3;  // 15MB: the fit is under way in this shot
    shot(L"editor_target.png", 96);
    settings_.targetIdx = 0;
    fitting_ = false;
    // 자막 (#22): the bar open, a syllable still being composed (IME) at the caret, the caption over the preview;
    // L pressed twice, so 2× next to the time.
    captionOpen_ = true;
    caption_ = L"회의를 시작합니";
    ui_.Focus(HashId("cap.text"));
    mouse.composing = L"다";
    Shuttle(1);
    Shuttle(1);
    shot(L"editor_caption.png", 96);
    mouse.composing.clear();
    SetPlaying(false);
    Seek(40);
    captionOpen_ = false;
    caption_.clear();
    // 재생 방향 (#22) at the end of 자세히 설정, scrolled down to it.
    settings_.exportDetails = true;
    settings_.directionIdx = 2;
    shot(L"editor_direction.png", 96);  // lays the panel out: how far it scrolls
    exportScroll_ = 10000;
    shot(L"editor_direction.png", 96);
    exportScroll_ = 0;
    settings_.exportDetails = false;
    settings_.directionIdx = 0;
    // 이미지로 저장 (#62): the card over the summary, a long name wrapping; then the bar in the narrowest window.
    hasResult_ = false;
    hasImage_ = true;
    lastImage_.path = ImageDir() + L"\\Starfall Arena_20261105_151208_0m11s100.png";
    shot(L"editor_image_saved.png", 96);
    W = 920;
    H = 640;
    shot(L"editor_image_narrow.png", 96);
    W = 1120;
    H = 780;
    hasImage_ = false;
    cropMode_ = true;
    crop_ = {120, 60, 840, 465};
    hasResult_ = true;
    lastResult_.path = settings_.outDir + L"\\OzMaker_20261102_142311.gif";
    lastResult_.bytes = 2'345'678;
    shot(L"editor_crop.png", 96);
    toast_ = L"파일을 클립보드에 복사했습니다 · 메신저나 탐색기에 붙여넣으세요";
    toastUntil_ = clock + 60;
    shot(L"editor_toast.png", 96);
    // A failure: it stays until closed, with the settings tab that fixes it (what StartRecording reports).
    Alert(L"영상 인코더를 시작할 수 없습니다. (0x8007000E) 하드웨어 인코딩을 끄거나 해상도를 낮춰 보세요. 그래픽 "
          L"드라이버를 업데이트하면 해결되는 경우도 많습니다.",
          kTabVideo);
    shot(L"editor_toast_long.png", 96);
    toastError_ = toastSticky_ = false;
    toastUntil_ = 0;
    // Questions inside the window instead of MessageBox.
    ConfirmDiscard();
    shot(L"editor_ask.png", 96);
    Ask({L"이 동영상을 휴지통으로 옮길까요?", L"OzMaker_20261102_142311.mp4\n이 녹화에서 따로 저장한 파일은 그대로 남습니다.",
         L"휴지통으로", true, nullptr});
    shot(L"editor_ask_video.png", 96);
    Ask({tr(L"이 동영상을 삭제할까요?"),
         L"OzMaker_20261102_142311.mp4" + std::wstring(tr(L"\n휴지통을 거치지 않고 지워서 되돌릴 수 없습니다.")) +
             tr(L"\n이 녹화에서 따로 저장한 파일은 그대로 남습니다."),
         tr(L"삭제"), true, nullptr});
    shot(L"editor_ask_delete.png", 96);
    question_.reset();
    CloseEditor();

    // Files list: two recordings not exported yet and a few recent files (names only, nothing on disk).
    held_ = {{MakeSyntheticClip(320, 180, 30, 30), clock - 40, L"OzMaker Test Window"},
             {clip_, clock - 1500, L"화면 1"}};
    recentFiles_ = {{settings_.outDir + L"\\OzMaker_20261102_142311.gif", 2'345'678, 300, false},
                    {settings_.outDir + L"\\OzMaker_20261102_140205.mp4", 48'213'004, 4000, true},
                    {settings_.outDir + L"\\OzMaker_20261101_221940.webp", 812'004, 90000, false}};
    filesOpen_ = true;
    shot(L"files_popover.png", 96);
    // The same over a window preview, with a recent file's tooltip and a toast: the preview (stand-in colour) is
    // cut around all three.
    HWND standIn = windows_.empty() ? MakeStandInWindow(inst_) : nullptr;
    target_ = standIn ? standIn : windows_.front().hwnd;
    settings_.source = kSourceWindow;
    recentFiles_[0].path = settings_.outDir + L"\\아주 긴 폴더 이름으로 경로가 목록보다 넓어지는 경우\\OzMaker_20261102_142311.gif";
    toast_ = L"파일을 복사했습니다. 메신저나 폴더에 붙여넣으세요.";
    toastUntil_ = clock + 60;
    shot(L"home_layers.png", 96);  // places the list (filesBtn_)
    mouse.mx = filesBtn_.r - 300;
    mouse.my = filesBtn_.b + 8 + 8 + 30 + 2 * 52 + 30 + 26;  // past the two recordings: the first recent file
    shot(L"home_layers.png", 96);
    mouse = {};
    toastUntil_ = 0;
    filesOpen_ = false;
    // Files dragged over the window, over the preview: a video, and a 움짤 that can't be opened yet.
    dragKind_ = DragKind::Video;
    dragName_ = L"회의_녹화.mp4";
    shot(L"drop_video.png", 96);
    dragKind_ = DragKind::Anim;
    dragName_ = L"반응.gif";
    shot(L"drop_gif.png", 96);
    dragKind_ = DragKind::None;
    dragName_.clear();
    // The 편집 tab with nothing open: the two recordings and the recent files, then with none of either.
    auto keep = std::exchange(clip_, nullptr);
    screen_ = Screen::Editor;
    shot(L"edit_home.png", 96);
    mouse.mx = 1005;  // the GIF's 편집, which says why it is off
    mouse.my = 60 + 20 + 250 + 16 + 12 + 28 + 26;
    shot(L"edit_home_tip.png", 96);
    mouse = {};
    held_.clear();
    recentFiles_.clear();
    shot(L"edit_home_empty.png", 96);
    // A video saved as MP4 (#22): at 고화질 it is cut as it is, from the keyframe before the range (the yellow line);
    // at 1.5× it is encoded again, and the panel says why.
    if (std::wstring video = dir + L"\\shot_video.mp4", err; WriteTestMp4(video, 960, 540, 180, 30, 60, err)) {
        OpenEditor(VideoClip::Open(video, err));
        if (thumbThread_.joinable()) thumbThread_.join();
        thumbs_.clear();
        for (auto& px : thumbPixels_) thumbs_.push_back(gfx_.CreateBitmap(thumbW_, thumbH_, px.data(), thumbW_ * 4));
        playing_ = false;
        inF_ = 75;
        outF_ = 150;
        playhead_ = 100;
        if (auto rd = clip_->NewReader(); const uint8_t* px = rd->Get(playhead_)) {  // no fetcher thread here
            UploadFrame(px);
            shown_ = playhead_;
        }
        shot(L"editor_mp4_copy.png", 96);
        W = 1000;  // the bar with sound: 이미지로 저장 is an icon, the volume keeps its slider
        shot(L"editor_mp4_image_icon.png", 96);
        W = 920;
        H = 640;
        shot(L"editor_mp4_narrow.png", 96);
        W = 1120;
        H = 780;
        settings_.speedIdx = 2;
        std::atomic<bool> cancel{false};
        estBytes_ = EstimateBytes(MakeJob(settings_.scaleIdx, settings_.outFpsIdx, 0), cancel);
        estKey_ = EstimateKey();
        estValid_ = true;
        shot(L"editor_mp4_reencode.png", 96);
        settings_.speedIdx = 1;
        CloseClip();
    } else {
        log(L"editor_mp4_copy.png: skipped, no test video (" + err + L")");
    }
    {
        // An opened 움짤 file with see-through parts (#41): its own format, the file bar, the checkerboard under it;
        // as MP4, white.
        const int sw = 240, sh = 160, sn = 24;
        auto store = std::make_shared<FrameStore>();
        std::wstring err;
        bool made = store->Open(sw, sh, err, false);
        for (int i = 0; made && i < sn; ++i) {
            auto buf = store->Acquire();
            DrawSticker(buf->data(), sw, sh, i / (double)sn);
            made = store->Submit(buf, (int64_t)(i * 1e7 / 12), true);
        }
        store->Finish((int64_t)(sn * 1e7 / 12));
        const Format f = FormatAvailable(Format::WebP) ? Format::WebP : Format::Gif;
        ExportJob job;
        job.clip = std::make_shared<AlphaClip>(store);
        job.last = sn - 1;
        job.crop = {0, 0, sw, sh};
        job.outW = sw;
        job.outH = sh;
        job.format = f;
        job.quality = 90;
        job.path = dir + L"\\스티커." + FormatExt(f);
        Exporter ex;
        std::atomic<bool> cancel{false};
        std::atomic<float> progress{0};
        auto anim = made && ExportSync(ex, job).ok ? AnimClip::Open(job.path, cancel, progress, err) : nullptr;
        if (anim) {
            OpenEditor(anim);
            if (thumbThread_.joinable()) thumbThread_.join();
            thumbs_.clear();
            for (auto& px : thumbPixels_) thumbs_.push_back(gfx_.CreateBitmap(thumbW_, thumbH_, px.data(), thumbW_ * 4));
            playing_ = false;
            playhead_ = 6;
            estBytes_ = EstimateBytes(MakeJob(settings_.scaleIdx, settings_.outFpsIdx, settings_.quality[fmt_]), cancel);
            estKey_ = EstimateKey();
            estValid_ = true;
            shot(L"editor_anim.png", 96);
            fmt_ = (int)Format::Mp4;
            shot(L"editor_anim_mp4.png", 96);
            CloseClip();
        } else {
            log(L"editor_anim.png: skipped (" + err + L")");
        }
        DeleteFileW(job.path.c_str());
    }
    // 순간 리플레이 (#20): the 리플레이 tab in each state, over made-up programs and saved replays, with the add popover,
    // a row's menu and a name being typed; then 설정 › 리플레이. Nothing is held: the states are made up.
    {
        const std::vector<ReplayTarget> targets = settings_.rpTargets;
        const bool wasOn = settings_.rpOn;
        screen_ = Screen::Replay;
        rpFiles_.clear();
        settings_.rpTargets.clear();
        rpStateForTest_ = (int)RpState::NoTarget;
        shot(L"replay_none.png", 96);
        settings_.rpTargets = {{false, L"C:\\Games\\Starfall\\starfall.exe", L"Starfall Arena", true},
                               {false, L"C:\\Games\\PixelRacer\\pixelracer.exe", L"Pixel Racer", true},
                               {false, L"C:\\Games\\Moonlit\\moonlit.exe", L"Moonlit Tactics", false}};
        settings_.rpOn = false;
        rpRunningForTest_ = 0;
        rpStateForTest_ = (int)RpState::Off;
        shot(L"replay_off.png", 96);
        settings_.rpOn = true;
        rpRunningForTest_ = -1;
        rpStateForTest_ = (int)RpState::Waiting;
        shot(L"replay_waiting.png", 96);
        rpClosed_ = {settings_.rpTargets[1].path, settings_.rpTargets[1].name, util::NowSeconds(), 30};
        rpHeldForTest_ = 30;
        shot(L"replay_closed.png", 96);
        rpClosed_.left = {hwnd_};
        shot(L"replay_closed_window.png", 96);
        rpClosed_ = {};
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        const uint64_t now = (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
        int n = 0;
        auto file = [&](const std::wstring& program, double ago, double seconds, uint64_t mb) {
            ReplayFile f;
            f.program = program;
            f.path = L"C:\\Replays\\" + program + util::Format(L"\\%s_%d.mp4", program.c_str(), ++n);
            f.bytes = mb << 20;
            f.written = now - (uint64_t)(ago * 1e7);
            f.seconds = seconds;
            return f;
        };
        rpFiles_ = {file(L"Starfall Arena", 40, 30, 48), file(L"Starfall Arena", 760, 30, 51),
                    file(L"Pixel Racer", 3700, 30, 31), file(L"Starfall Arena", 26 * 3600, 28, 39),
                    file(util::Format(tr(L"화면 %d"), 1), 27 * 3600, 60, 62), file(L"Pixel Racer", 75 * 3600, 30, 33)};
        rpNewPath_ = rpFiles_[0].path;
        rpRunningForTest_ = 0;
        rpStateForTest_ = (int)RpState::Holding;
        rpHeldForTest_ = 16;
        shot(L"replay_holding.png", 96);
        settings_.rpMic = true;
        rpRunningForTest_ = 1;
        rpPause_ = RpPause::Minimized;
        rpStateForTest_ = (int)RpState::Paused;
        shot(L"replay_paused.png", 96);
        rpPause_ = RpPause::None;
        rpStateForTest_ = (int)RpState::Holding;
        shot(L"replay_holding_mic.png", 96);
        settings_.rpMic = false;
        rpRunningForTest_ = 0;
        rpRunning_ = {{L"C:\\Games\\Orbit\\orbit.exe", L"Orbit Drift", true}, {L"C:\\Tools\\memo.exe", L"Memo", false}};
        rpAddOpen_ = true;
        shot(L"replay_add.png", 96);
        rpAddOpen_ = false;
        rpRunning_.clear();
        rpFileMenu_ = 1;
        shot(L"replay_menu.png", 96);
        settings_.deleteMode = kDeleteNow;
        shot(L"replay_menu_delete.png", 96);
        rpFileMenu_ = -1;
        TrashReplay(1);
        shot(L"replay_ask_delete.png", 96);
        question_.reset();
        settings_.deleteMode = kDeleteToBin;
        rpRenaming_ = 1;
        rpRenameFocused_ = false;
        rpRenameText_ = L"Starfall Arena_20261104_151208";
        ui_.Focus(HashId("rp.rename"));
        shot(L"replay_rename.png", 96);
        rpRenaming_ = -1;
        screen_ = Screen::Settings;
        settings_.settingsTab = kTabReplay;
        settingsScroll_ = 0;
        shot(L"settings_replay.png", 96);
        settingsScroll_ = 10000;
        shot(L"settings_replay_end.png", 96);
        settingsScroll_ = 0;
        settings_.settingsTab = kTabGeneral;
        rpStateForTest_ = rpRunningForTest_ = -1;
        rpHeldForTest_ = 0;
        rpFiles_.clear();
        rpNewPath_.clear();
        settings_.rpTargets = targets;
        settings_.rpOn = wasOn;
    }
    screen_ = Screen::Home;
    keep.reset();
    target_ = nullptr;
    if (standIn) DestroyWindow(standIn);
    log(util::Format(L"screen reader: %zu controls in %zu shots, each with a name unless a line above says so",
                     controls, shots));
    // In English, every text shown has its English (tr looked each up).
    for (const std::wstring& s : MissingTranslations()) {
        log(L"FAIL no English for: " + s);
        ++layerErrors;
    }
    log(layerErrors ? util::Format(L"RESULT: %d problem(s): layering errors, unnamed controls or missing English",
                                   layerErrors)
                    : L"RESULT: all passed");
    return layerErrors ? 2 : 0;
}

// README pictures (--readme-media <dir>): the 녹화 tab, the area picker, the editor and the export panel as PNGs at
// twice the pixels, and hero.gif, a pointer going from picking a window to copying the saved 움짤. All drawn offscreen
// like --screenshot, over made-up windows (the game above): nothing is captured from the screen. OzMaker's own
// encoder saves the GIF.
int App::RunReadmeMedia(const std::wstring& dir) {
    testWindowsOnly_ = true;
    util::EnsureDir(dir);
    Log log(dir + L"\\readme-media.log");
    int failures = 0;
    bool layerFailed = false;
    const float W = 1120, H = 780;
    double clock = util::NowSeconds();
    double scene = 0;  // the game's clock
    UiInput mouse;
    ui_.KeepRects(true);
    settings_.source = kSourceWindow;
    settings_.output = kOutputClip;

    // Made-up windows; hidden stand-ins give them handles (the preview takes its shape, 16:9, from the first).
    std::vector<WinInfo> wins;
    const wchar_t* titles[] = {L"데모 게임", L"여행 계획 - 메모", L"사진 보기"};
    const wchar_t* exes[] = {L"DemoGame.exe", L"Memo.exe", L"Viewer.exe"};
    const SIZE sizes[] = {{1280, 720}, {960, 720}, {1200, 800}};
    for (int i = 0; i < 3; ++i) {
        WinInfo w;
        w.hwnd = MakeStandInWindow(inst_);
        w.title = titles[i];
        w.exeName = exes[i];
        w.frame = {0, 0, sizes[i].cx, sizes[i].cy};
        SetWindowPos(w.hwnd, nullptr, 0, 0, sizes[i].cx, sizes[i].cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        icons_[w.hwnd] = nullptr;
        wins.push_back(w);
    }
    const HWND game = wins[0].hwnd;
    std::vector<uint32_t> icon = DemoIcon();
    icons_[game] = gfx_.CreateBitmap(32, 32, icon.data(), 32 * 4);

    // The pointer (hero.gif only): an arrow, and an amber ripple where it clicks, as 클릭 강조 draws it.
    struct Ripple {
        float x, y;
        double at;
    };
    std::vector<Ripple> ripples;
    D2D1_POINT_2F ptr{-100, -100};
    bool showPointer = false;
    ComPtr<ID2D1PathGeometry> arrow;
    {
        ComPtr<ID2D1GeometrySink> sink;
        gfx_.D2D()->CreatePathGeometry(&arrow);
        arrow->Open(&sink);
        const D2D1_POINT_2F pts[] = {{0, 16.5f}, {4, 12.6f}, {6.8f, 18.8f}, {9.4f, 17.7f}, {6.7f, 11.7f}, {11.8f, 11.7f}};
        sink->BeginFigure({0, 0}, D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLines(pts, 6);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
    }

    std::vector<uint8_t> px;
    auto render = [&](float dpi) {
        dpi_ = (UINT)dpi;
        UINT pw = (UINT)(W * dpi / 96), ph = (UINT)(H * dpi / 96);
        return gfx_.RenderToPixels(pw, ph, dpi, [&](ID2D1DeviceContext* dc) {
            windows_ = wins;  // nothing here lists the real ones
            ui_.Begin(dc, mouse, clock);
            thumbWanted_ = false;
            thumbCells_.clear();
            DrawFrame(Rect(0, 0, W, H));
            ui_.End();
            if (ui_.LayerErrors() && !layerFailed) {
                layerFailed = true;
                ++failures;
                const Rect& e = ui_.FirstLayerError();
                log(util::Format(L"FAIL layering error at %.0f,%.0f-%.0f,%.0f", e.l, e.t, e.r, e.b));
            }
            // The game where the live preview goes (on screen DWM draws that).
            if (thumbWanted_ && target_) {
                RECT dest, src;
                PreviewLayout(dest, src);
                std::vector<uint32_t> frame = DemoFrame(scene);
                auto bmp = gfx_.CreateBitmap(kDemoW, kDemoH, frame.data(), kDemoW * 4);
                const float s = Scale();
                D2D1_RECT_F d{dest.left / s, dest.top / s, dest.right / s, dest.bottom / s};
                for (const RECT& p : PreviewParts(dest)) {
                    dc->PushAxisAlignedClip({p.left / s, p.top / s, p.right / s, p.bottom / s}, D2D1_ANTIALIAS_MODE_ALIASED);
                    dc->DrawBitmap(bmp.Get(), d, 1.f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr);
                    dc->PopAxisAlignedClip();
                }
            }
            if (showPointer) {
                ComPtr<ID2D1SolidColorBrush> b;
                dc->CreateSolidColorBrush(D2D1::ColorF(0xFBBF24), &b);
                for (const Ripple& r : ripples) {
                    float a = (float)((clock - r.at) / 0.5);
                    if (a < 0 || a >= 1) continue;
                    float e = 1 - (1 - a) * (1 - a) * (1 - a), rad = 12 + 22 * e;
                    D2D1_ELLIPSE el{{r.x, r.y}, rad, rad};
                    b->SetColor(D2D1::ColorF(0xFBBF24, 0.32f * (1 - a)));
                    dc->FillEllipse(el, b.Get());
                    b->SetColor(D2D1::ColorF(0xFBBF24, 0.95f * (1 - a * a)));
                    dc->DrawEllipse(el, b.Get(), 3.5f);
                }
                if (mouse.down) {  // held: a dot under the pointer
                    D2D1_ELLIPSE el{ptr, 9, 9};
                    b->SetColor(D2D1::ColorF(0xFBBF24, 0.45f));
                    dc->FillEllipse(el, b.Get());
                }
                dc->SetTransform(D2D1::Matrix3x2F::Scale(1.4f, 1.4f) * D2D1::Matrix3x2F::Translation(ptr.x, ptr.y));
                b->SetColor(D2D1::ColorF(D2D1::ColorF::White));
                dc->FillGeometry(arrow.Get(), b.Get());
                b->SetColor(D2D1::ColorF(0x111111));
                dc->DrawGeometry(arrow.Get(), b.Get(), 0.8f);
                dc->SetTransform(D2D1::Matrix3x2F::Identity());
            }
        }, px);
    };
    // A PNG of part of the window (DIPs) at twice the pixels, after a few passes so animated values settle. No
    // pointer, nothing hovered.
    auto still = [&](const wchar_t* name, Rect part) {
        UiInput keep = std::exchange(mouse, {});
        bool pointer = std::exchange(showPointer, false);
        for (int pass = 0; pass < 10; ++pass) {
            clock += 0.05;
            render(192);
        }
        mouse = keep;
        showPointer = pointer;
        const int sw = (int)(W * 2);
        RECT r{(LONG)(part.l * 2), (LONG)(part.t * 2), (LONG)(part.r * 2), (LONG)(part.b * 2)};
        int w = r.right - r.left, h = r.bottom - r.top;
        std::vector<uint8_t> out((size_t)w * h * 4);
        for (int y = 0; y < h; ++y)
            memcpy(out.data() + (size_t)y * w * 4, px.data() + ((size_t)(r.top + y) * sw + r.left) * 4, (size_t)w * 4);
        bool ok = SavePng(gfx_.Wic(), dir + L"\\" + name, out.data(), w, h);
        if (!ok) ++failures;
        log(util::Format(L"%s: %s %dx%d", name, ok ? L"ok" : L"FAIL", w, h));
    };
    // The size estimate (and what 용량 맞추기 picks) is worked out on a thread: wait for it, so frames show it.
    auto settle = [&] {
        for (double until = util::NowSeconds() + 30; util::NowSeconds() < until;) {
            UpdateEstimate();
            Sleep(5);
            OnEstimate();  // no window to notify here: poll for the result
            if (!fitting_ && !estimator_.Busy() && estValid_ && estKey_ == EstimateKey()) break;
        }
    };
    // What the game window showed from `from` for `seconds`, as a recording of it (30 fps, 960×540).
    auto recordGame = [&](double from, double seconds) {
        auto s = std::make_shared<FrameStore>();
        std::wstring err;
        if (!s->Open(960, 540, err)) return s;
        int n = (int)std::lround(seconds * 30);
        for (int i = 0; i < n; ++i) {
            auto buf = s->Acquire();
            DrawDemoScene(buf->data(), 960, 540, from + i / 30.0);
            while (!s->Submit(buf, (int64_t)(i * 1e7 / 30))) Sleep(1);
        }
        s->Finish((int64_t)(n * 1e7 / 30));
        return s;
    };
    auto openRecording = [&](std::shared_ptr<Clip> clip) {
        OpenEditor(std::move(clip));
        if (thumbThread_.joinable()) thumbThread_.join();  // let it finish (StopThumbnails would cancel)
        thumbs_.clear();
        for (auto& p : thumbPixels_) thumbs_.push_back(gfx_.CreateBitmap(thumbW_, thumbH_, p.data(), thumbW_ * 4));
        clipName_ = L"데모 게임";
        held_ = {{clip_, clock, clipName_}};
        settle();
    };

    // ---- 녹화 tab: the game picked, mid-jump.
    screen_ = Screen::Home;
    target_ = game;
    scene = 1.33;
    still(L"record.png", Rect(0, 0, W, H));

    // ---- The area picker over a made-up desktop with the game's window on it, 16:9 fixed.
    monitors_ = EnumerateMonitors();
    for (auto& m : monitors_) {
        if (!m.primary) continue;
        const RECT& mr = m.rect;
        const int mw = mr.right - mr.left, mh = mr.bottom - mr.top;
        const int gw = std::min(960, mw * 6 / 10), gh = gw * 9 / 16, gx = (mw - gw) / 2, gy = (mh - gh) / 2 - 40;
        // The area over the right of the game, clear of its counter (the size label goes above the area).
        const int ax = gx + 200, ay = gy + 32 + 70, aw = gw - 240;
        POINT a{mr.left + ax, mr.top + ay}, c{mr.left + ax + aw, mr.top + ay + aw * 9 / 16};
        // The game's window and a margin; the bar under the area stays inside it.
        const int cx = std::max(0, gx - 40), cy = std::max(0, gy - 40);
        const int cw = std::min(mw - cx, gw + 80), ch = std::min(mh - cy, gh + 32 + 80);
        bool ok = gfx_.RenderToPixels(cw * 2, ch * 2, 192, [&](ID2D1DeviceContext* dc) {
            dc->SetTransform(D2D1::Matrix3x2F::Translation((float)-cx, (float)-cy));
            Painter p;
            p.Bind(dc);
            p.Fill(Rect(0, 0, (float)mw, (float)mh), theme::Hex(0x2B4C7E));
            p.Fill(Rect::XYWH((float)gx, (float)gy, (float)gw, 32.f + gh), theme::Hex(0xE6E8EC), 8);
            p.Text(Rect::XYWH(gx + 14.f, (float)gy, 300.f, 32.f), L"데모 게임", 13.f, theme::Hex(0x1D2433));
            std::vector<uint32_t> frame = DemoFrame(2.4);
            auto bmp = gfx_.CreateBitmap(kDemoW, kDemoH, frame.data(), kDemoW * 4);
            dc->DrawBitmap(bmp.Get(), D2D1::RectF((float)gx, gy + 32.f, (float)(gx + gw), (float)(gy + 32 + gh)), 1.f,
                           D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr);
            regionPicker_.PaintTest(p, mr, a, c, RegionPicker::TestView::Adjust);
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
        }, px);
        ok = ok && SavePng(gfx_.Wic(), dir + L"\\region.png", px.data(), cw * 2, ch * 2);
        if (!ok) ++failures;
        log(util::Format(L"region.png: %s %dx%d", ok ? L"ok" : L"FAIL", cw * 2, ch * 2));
        break;
    }

    // ---- hero.gif, 15 frames a second. The pointer moves onto controls where the last frame laid them out. Clicks
    // that only change what is on screen go to the app as they are; record, stop, save and copy are acted out
    // instead, so no capture starts, nothing is written and the clipboard is left alone.
    const double kFps = 15;
    auto store = std::make_shared<FrameStore>();
    std::wstring err;
    if (!store->Open((int)W, (int)H, err)) {
        log(L"hero.gif: FAIL " + err);
        return 1;
    }
    int frames = 0;
    double heroT = 0;
    std::function<void()> each;  // per frame: the recording time, the playhead
    auto frame = [&] {
        mouse.mx = ptr.x;
        mouse.my = ptr.y;
        if (each) each();
        render(96);
        mouse.ClearEdges();
        mouse.composing.clear();
        auto buf = store->Acquire();
        auto* d = reinterpret_cast<uint32_t*>(buf->data());
        auto* s = reinterpret_cast<const uint32_t*>(px.data());
        for (size_t i = 0; i < (size_t)(W * H); ++i) d[i] = s[i] | 0xFF000000u;
        while (!store->Submit(buf, (int64_t)std::llround(frames * 1e7 / kFps))) Sleep(1);
        ++frames;
        heroT += 1 / kFps;
        clock += 1 / kFps;
        scene += 1 / kFps;
    };
    auto wait = [&](double seconds) {
        for (double end = heroT + seconds - 1e-6; heroT < end;) frame();
    };
    auto moveTo = [&](float x, float y, double seconds) {
        D2D1_POINT_2F from = ptr;
        int n = std::max(1, (int)std::lround(seconds * kFps));
        for (int i = 1; i <= n; ++i) {
            float u = i / (float)n, e = u * u * (3 - 2 * u);
            ptr = {from.x + (x - from.x) * e, from.y + (y - from.y) * e};
            frame();
        }
    };
    auto moveOnto = [&](UiId id, double seconds) {
        if (const Rect* r = ui_.LastRect(id)) return moveTo(r->CX(), r->CY(), seconds);
        log(util::Format(L"hero.gif: FAIL no control %08X on screen at %.1fs", id, heroT));
        ++failures;
    };
    auto click = [&](bool real) {
        ripples.push_back({ptr.x, ptr.y, clock});
        if (!real) return frame();
        mouse.pressed = mouse.down = true;
        frame();
        mouse.down = false;
        mouse.released = true;
        frame();
    };
    double playFrom = 0;
    auto play = [&] {  // the playhead where playback would have it on this clock
        playing_ = true;
        playBack_ = false;
        shuttle_ = 0;
        const int64_t start = clip_->Ts(inF_), len = std::max<int64_t>(1, RangeEndTs() - start);
        playTs_ = start + (int64_t)std::fmod((heroT - playFrom) * 1e7, (double)len);
        playWall_ = util::NowSeconds();
    };

    screen_ = Screen::Home;
    target_ = nullptr;
    showPointer = true;
    ptr = {600, 470};
    scene = 0.2;
    wait(0.3);
    moveOnto(HashId("win", (uint32_t)(uintptr_t)game), 0.6);
    click(true);  // picks the game: the preview comes up
    wait(0.5);
    moveOnto(HashId("home.rec"), 0.6);
    wait(0.15);
    click(false);
    const double recFrom = scene;
    rec_ = RecState::Recording;
    each = [&] { recElapsedForTest_ = scene - recFrom; };
    wait(2.0);
    click(false);  // stop: the editor opens on the recording, playing
    each = nullptr;
    rec_ = RecState::Idle;
    recElapsedForTest_ = -1;
    openRecording(recordGame(recFrom, scene - recFrom));
    playFrom = heroT;
    each = play;
    wait(0.6);
    // The end of the range dragged in.
    moveOnto(HashId("tl.out"), 0.5);
    each = nullptr;
    playing_ = false;
    ripples.push_back({ptr.x, ptr.y, clock});
    mouse.pressed = mouse.down = true;
    if (const Rect* track = ui_.LastRect(HashId("tl.scrub")))
        moveTo(track->l + track->W() * 0.7f, ptr.y, 0.7);
    mouse.down = false;
    mouse.released = true;
    frame();
    settle();
    playFrom = heroT;
    each = play;
    wait(0.3);
    // 자막, typed through the IME (each syllable shows being composed first).
    moveOnto(HashId("tr.caption"), 0.5);
    click(true);
    for (wchar_t ch : std::wstring(L"보스 처치!")) {
        if (ch >= 0xAC00 && ch <= 0xD7A3) {
            mouse.composing = std::wstring(1, ch);
            frame();
        }
        mouse.chars = std::wstring(1, ch);
        frame();
    }
    mouse.keys.push_back(VK_RETURN);
    frame();
    settle();
    wait(0.3);
    moveOnto(Ui::CellId(HashId("ex.target"), 2), 0.6);
    click(true);  // 10MB
    settle();
    wait(0.4);
    // export.png now, while the panel shows the estimate (the saved file's card takes its place). The still moves the
    // clock on half a second; nothing on screen is moving then.
    const Rect* save = ui_.LastRect(HashId("ex.save"));
    const float split = save ? save->l - 18 : 784;
    still(L"export.png", Rect(split - 16, 60, W, H));
    moveOnto(HashId("ex.save"), 0.5);
    click(false);
    {
        // The real export of this range, for the size on the card; the file goes away again.
        ExportJob job = MakeJob(settings_.scaleIdx, settings_.outFpsIdx, settings_.quality[fmt_]);
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        job.path = std::wstring(tmp) + L"OzMaker_readme_result.gif";
        Exporter ex;
        ExportResult r = ExportSync(ex, job);
        DeleteFileW(job.path.c_str());
        if (!r.ok) ++failures;
        log(util::Format(L"saved range: %s %s, %d frames", r.ok ? L"ok" : L"FAIL", util::FormatBytes(r.bytes).c_str(),
                         r.frames));
        lastResult_ = r;
        lastResult_.path = settings_.outDir + L"\\OzMaker_20261103_142311.gif";
        hasResult_ = true;
        resultOriginal_ = false;
        DropHeld(clip_.get());
    }
    wait(0.6);
    moveOnto(HashId("res.copy"), 0.5);
    click(false);
    toast_ = L"파일을 클립보드에 복사했습니다 · 메신저나 탐색기에 붙여넣으세요";
    toastError_ = false;
    toastUntil_ = clock + 30;
    wait(1.2);
    each = nullptr;
    showPointer = false;
    store->Finish((int64_t)std::llround(frames * 1e7 / kFps));
    {
        ExportJob job;
        job.clip = store;
        job.first = 0;
        job.last = store->Count() - 1;
        job.crop = {0, 0, (LONG)W, (LONG)H};
        job.outW = (int)W;
        job.outH = (int)H;
        job.format = Format::Gif;
        job.quality = 90;
        job.holdLastMs = 1000;
        job.path = dir + L"\\hero.gif";
        Exporter ex;
        ExportResult r = ExportSync(ex, job);
        if (!r.ok) ++failures;
        log(util::Format(L"hero.gif: %s %s, %d frames, %.1fs", r.ok ? L"ok" : L"FAIL",
                         util::FormatBytes(r.bytes).c_str(), r.frames, r.seconds));
    }

    // ---- The editor as the GIF ends: the range dragged in, the caption.
    toastUntil_ = 0;
    playing_ = false;
    Seek(inF_ + (outF_ - inF_) * 2 / 5);
    still(L"edit.png", Rect(0, 60, split, H));

    held_.clear();
    CloseClip();
    target_ = nullptr;
    windows_.clear();
    for (const WinInfo& w : wins) DestroyWindow(w.hwnd);
    log(failures ? util::Format(L"RESULT: %d failure(s)", failures) : L"RESULT: all passed");
    return failures ? 1 : 0;
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
    bool pause = a.size() > 9 && a[9] == L"pause";  // paused for the middle third: the result is a third shorter
    // click (anywhere after the mode): a made-up click at the window's centre a quarter of the way in, then the
    // frames are checked for its highlight. Never a real click: that would land on whatever is on screen.
    bool click = std::find(a.begin() + std::min<size_t>(a.size(), 8), a.end(), L"click") != a.end();
    // vfr (anywhere after the mode): a variable frame rate MP4 (#55), checked for how few frames a still window gets.
    bool vfr = mp4 && std::find(a.begin() + std::min<size_t>(a.size(), 8), a.end(), L"vfr") != a.end();
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
        v.video.vfr = vfr;
        v.sound = audio;
        // mic (anywhere after the mode): the default input mixed in. Never in automated runs: it records the room.
        v.mic = std::find(a.begin() + std::min<size_t>(a.size(), 8), a.end(), L"mic") != a.end();
        v.audio.enabled = v.sound || v.mic;
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
    o.clickFx = click;
    // dda | wgc (anywhere after the mode): how a monitor is taken, desktop duplication or Windows.Graphics.Capture.
    if (std::find(a.begin() + std::min<size_t>(a.size(), 7), a.end(), L"dda") != a.end()) o.screenVia = 1;
    if (std::find(a.begin() + std::min<size_t>(a.size(), 7), a.end(), L"wgc") != a.end()) o.screenVia = 2;
    Capture capture_;  // its own: the app's recordings are App::recs_
    std::wstring err;
    double t0 = util::NowSeconds();
    if (!capture_.Start(o, nullptr, 0, err)) {
        log(L"FAIL: " + err);
        return 1;
    }
    log(util::Format(L"start took %.0f ms, %s", (util::NowSeconds() - t0) * 1000,
                     capture_.Duplicating() ? L"desktop duplication" : L"Windows.Graphics.Capture"));
    double begin = util::NowSeconds(), end = begin + seconds, clickAt = -1;
    int phase = 0;
    while (util::NowSeconds() < end) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, 0);
        double t = util::NowSeconds() - begin;
        if (click && clickAt < 0 && t >= seconds / 4) {
            RECT f{};
            GetVisibleFrame(target, f);
            capture_.InjectClick({(f.left + f.right) / 2, (f.top + f.bottom) / 2}, 1'500'000);
            clickAt = t;
            log(util::Format(L"click at %.2fs", t));
        }
        if (pause && phase == 0 && t >= seconds / 3) {
            capture_.SetPaused(true);
            log(util::Format(L"paused at %.2fs (recorded %.2fs)", t, capture_.Elapsed()));
            phase = 1;
        } else if (pause && phase == 1 && t >= seconds * 2 / 3) {
            capture_.SetPaused(false);
            log(util::Format(L"resumed at %.2fs (recorded %.2fs)", t, capture_.Elapsed()));
            phase = 2;
        }
    }
    if (pause) log(util::Format(L"recorded %.2fs of %.2fs", capture_.Elapsed(), seconds));
    log(util::Format(L"stopping after %zu frames (%zu scaled on the GPU)", capture_.Frames(), capture_.Scaled()));
    if (click) log(util::Format(L"%zu frames added for the click highlight", capture_.ClickFrames()));
    t0 = util::NowSeconds();
    CaptureResult res = capture_.Stop();
    log(util::Format(L"stop took %.0f ms", (util::NowSeconds() - t0) * 1000));
    // The highlight around the middle of the frame (the window's centre): amber while the ripple plays, gone
    // before and after it.
    auto clickCheck = [&](const Clip& c) {
        auto rd = c.NewReader();
        const int w = c.Width(), h = c.Height();
        int during = 0, duringN = 0, outside = 0;
        size_t best = 0;
        int bestN = 0;
        for (size_t i = 0; i < c.Count(); ++i) {
            const uint8_t* px = rd->Get(i);
            if (!px) continue;
            int n = 0;
            for (int y = std::max(0, h / 2 - 70); y < std::min(h, h / 2 + 70); y += 2)
                for (int x = std::max(0, w / 2 - 70); x < std::min(w, w / 2 + 70); x += 2) {
                    const uint8_t* p = px + ((size_t)y * w + x) * 4;
                    if (p[2] - p[0] > 60 && p[1] - p[0] > 25 && p[2] >= p[1]) ++n;  // amber, also half blended
                }
            double t = c.Ts(i) / 1e7;
            if (t > clickAt + 0.08 && t < clickAt + 0.35) {
                ++duringN;
                during += n > 20;
            } else if (t < clickAt - 0.2 || t > clickAt + 0.9) {
                outside += n > 20;
            }
            if (n > bestN) {
                bestN = n;
                best = i;
            }
        }
        bool ok = duringN > 0 && during == duringN && outside == 0;
        log(util::Format(L"%s click highlight: %d of %d frames in the ripple show it, %d frames outside it do",
                         ok ? L"ok  " : L"FAIL", during, duringN, outside));
        if (mode != L"monitor" && bestN > 0) {
            if (const uint8_t* px = rd->Get(best)) SavePng(gfx_.Wic(), dir + L"\\click.png", px, w, h);
        }
        return ok;
    };
    if (res.video) {
        log(util::Format(L"mp4 %s: %s, %.2fs, encoder %s (hw %d), audio %d %s %s", res.ok ? L"ok" : L"FAIL",
                         util::FormatBytes(res.bytes).c_str(), res.seconds, res.encoder.c_str(), res.hardware, res.audio,
                         res.notice.c_str(), res.error.c_str()));
        if (!res.ok) return 2;
        // Never keep pictures of a whole monitor (it may show unrelated windows).
        AnalyzeMp4(log, gfx_.Wic(), res.path, {440, 880}, mode == L"monitor" ? L"" : dir + L"\\mp4");
        if (click) {
            std::wstring verr;
            auto vc = VideoClip::Open(res.path, verr);
            if (!vc || !clickCheck(*vc)) return 2;
        }
        if (vfr) {
            // Frames only where the window changed (the cursor, a click's highlight), and one every keyframe interval
            // (2 s) while it is still: far fewer than a constant rate, none apart by more than that, and as long.
            std::wstring verr;
            auto vc = VideoClip::Open(res.path, verr);
            if (!vc) {
                log(L"FAIL vfr: " + verr);
                return 2;
            }
            int64_t longest = 0, keyGap = 0, lastKey = 0;
            std::wstring times;
            for (size_t i = 0; i < vc->Count(); ++i) {
                const int64_t end = i + 1 < vc->Count() ? vc->Ts(i + 1) : vc->EndTs();
                longest = std::max(longest, end - vc->Ts(i));
                if (vc->KeyBefore(i) == i) {
                    keyGap = std::max(keyGap, vc->Ts(i) - lastKey);
                    lastKey = vc->Ts(i);
                }
                if (i < 40) times += util::Format(L" %.2f%s", vc->Ts(i) / 1e7, vc->KeyBefore(i) == i ? L"K" : L"");
            }
            keyGap = std::max(keyGap, vc->EndTs() - lastKey);
            const size_t constant = (size_t)std::lround(res.seconds * o.fps);
            const bool ok = vc->Count() * 3 < constant && longest <= 20'000'000 + 10'000'000 / o.fps &&
                            keyGap <= 20'000'000 + 10'000'000 / o.fps && std::fabs(vc->EndTs() / 1e7 - res.seconds) < 0.1;
            log(util::Format(L"%s vfr: %zu frames (constant: %zu), longest %.3fs, keyframes at most %.3fs apart, %.3fs "
                             L"long (recorded %.3fs):%s",
                             ok ? L"ok  " : L"FAIL", vc->Count(), constant, longest / 1e7, keyGap / 1e7,
                             vc->EndTs() / 1e7, res.seconds, times.c_str()));
            if (!ok) return 2;
        }
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
    if (click && !clickCheck(*clip)) ++failures;
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
    if (rate < 200) return -1;  // no sound read
    size_t win = rate / 50, step = rate / 200;
    for (size_t i = (size_t)(after * rate); (i + win) * 2 <= st.size(); i += step)
        if (ToneLevel(st, i, win, b, rate) > ToneLevel(st, i, win, a, rate)) return (i + win / 2) / (double)rate;
    return -1;
}

// crash: the process crashes once the frames are written, the file as a crash leaves it (for the recovery test).
bool WriteTestMp4(const std::wstring& path, int W, int H, int frames, int fps, int gop, std::wstring& err,
                  bool fragmented, bool crash) {
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
    v.fragmented = fragmented;
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
    if (crash) {
        Sleep(1500);  // the sink writes the fragments that are complete
        volatile int* nowhere = nullptr;
        *nowhere = 1;
    }
    return w.Finish(err);
}

// An MP4 in these settings, a second long. got: the encoder it took and what was tried before.
bool ProbeMp4(const std::wstring& path, const VideoConfig& v, bool sound, std::wstring& err, std::wstring& got) {
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
    td.Width = v.width;
    td.Height = v.height;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    dev->CreateTexture2D(&td, nullptr, &tex);
    AudioConfig a;
    a.enabled = sound;
    Mp4Writer w;
    const bool opened = w.Open(path, v, a, dev.Get(), err);
    got = opened ? w.EncoderName() + (w.HardwareEncoder() ? L" (hardware)" : L" (software)") : L"";
    if (!w.Tried().empty()) got += L" after " + w.Tried();
    if (!opened) return false;
    std::vector<int16_t> pcm((size_t)a.sampleRate / v.fps * 2);
    for (int k = 0; k < v.fps; ++k) {
        ComPtr<IMFSample> s;
        for (int tries = 0; !(s = w.Convert(tex.Get())) && tries < 2500; ++tries) Sleep(2);
        if (sound) w.PushAudio(pcm.data(), pcm.size() / 2);
        if (!s || !w.WriteVideo(s.Get(), (int64_t)k * 10'000'000 / v.fps, 10'000'000 / v.fps)) {
            err = L"write failed";
            w.Abort();
            return false;
        }
    }
    return w.Finish(err);
}

// Variable frame rate as recording writes it (#55): 30 fps ticks over 6.9 s, a picture that changes at 0, 0.1, 0.2, 6.0
// and 6.1 s and stays still between, keyframes every second, and sound that steps from 440 to 880 Hz at 4.5 s. Like
// the recording ticker, a frame goes in a tick's time after the one before, so the encoder has taken that one.
bool WriteVfrMp4(const std::wstring& path, bool hardware, std::wstring& err, std::wstring& got, bool fragmented) {
    const int W = 320, H = 180, fps = 30, ticks = 207;
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
    v.gopFrames = fps;
    v.hardware = hardware;
    v.vfr = true;
    v.fragmented = fragmented;  // as recording writes it; Finish makes it plain
    AudioConfig a;
    a.enabled = true;
    Mp4Writer w;
    if (!w.Open(path, v, a, dev.Get(), err)) return false;
    got = w.EncoderName() + (w.HardwareEncoder() ? L" (hardware)" : L" (software)");
    std::vector<uint8_t> px((size_t)W * H * 4);
    std::vector<int16_t> pcm;
    int64_t pushed = 0;
    double phase = 0;
    int shown = -1;
    for (int k = 0; k < ticks; ++k) {
        const int index = k < 3 ? 0 : k < 6 ? 1 : k < 180 ? 2 : k < 183 ? 3 : 4;
        const int64_t t0 = (int64_t)k * 10'000'000 / fps;
        if (index != shown || w.KeyDue(t0)) {
            DrawIndexed(px.data(), W, H, index, index / 10.0);
            ctx->UpdateSubresource(tex.Get(), 0, nullptr, px.data(), W * 4, 0);
            Sleep(1000 / fps);
            ComPtr<IMFSample> s;
            for (int tries = 0; !(s = w.Convert(tex.Get())) && tries < 2500; ++tries) Sleep(2);
            if (!s || !w.HoldVideo(s, t0)) {
                err = L"write failed";
                w.Abort();
                return false;
            }
            shown = index;
        }
        const int64_t want = (int64_t)(k + 1) * a.sampleRate / fps;
        pcm.clear();
        for (int64_t i = pushed; i < want; ++i) {
            phase += 2 * 3.14159265358979 * (i < 4.5 * a.sampleRate ? 440 : 880) / a.sampleRate;
            const int16_t x = (int16_t)(0.03 * std::sin(phase) * 32767);  // quiet: the preview may play it
            pcm.push_back(x);
            pcm.push_back(x);
        }
        w.PushAudio(pcm.data(), pcm.size() / 2);
        pushed = want;
    }
    if (!w.EndVideo((int64_t)ticks * 10'000'000 / fps)) {
        err = L"write failed";
        w.Abort();
        return false;
    }
    return w.Finish(err);
}

// 순간 리플레이 (#20): 13 s at 30 fps into a replay ring (keyframes every second, a picture numbered by the second, a
// tone that steps from 440 to 880 Hz at 9.5 s), then the 5 s before 12 s saved. The file must start at the keyframe at
// 7 s and end at 12 s, the sound along with the picture (the step at 2.5 s), without a write going back.
int ReplayRingCheck(const std::wstring& dir, bool hardware, Log& log) {
    const int W = 320, H = 180, fps = 30, frames = 13 * fps;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Multithread> mt;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &dev, nullptr, &ctx)) ||
        FAILED(ctx.As(&mt))) {
        log(L"replay ring: FAIL no device");
        return 1;
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
    v.gopFrames = fps;
    v.hardware = hardware;
    AudioConfig a;
    a.enabled = true;
    ReplayRing ring;
    ring.SetKeep(6 * 10'000'000LL);
    Mp4Writer w;
    std::wstring err;
    if (!w.Open(L"", v, a, dev.Get(), err, &ring)) {
        log(L"replay ring: FAIL open " + err + L" " + w.Tried());
        return 1;
    }
    const std::wstring enc = w.EncoderName() + (w.HardwareEncoder() ? L" (hardware)" : L" (software)");
    std::vector<uint8_t> px((size_t)W * H * 4);
    std::vector<int16_t> pcm;
    int64_t pushed = 0;
    double phase = 0;
    bool wrote = true;
    for (int k = 0; k < frames && wrote; ++k) {
        DrawIndexed(px.data(), W, H, k / fps, k / (double)fps);
        ctx->UpdateSubresource(tex.Get(), 0, nullptr, px.data(), W * 4, 0);
        ComPtr<IMFSample> s;
        for (int tries = 0; !(s = w.Convert(tex.Get())) && tries < 2500; ++tries) Sleep(2);
        const int64_t want = (int64_t)(k + 1) * a.sampleRate / fps;
        pcm.clear();
        for (int64_t i = pushed; i < want; ++i) {
            phase += 2 * 3.14159265358979 * (i < 9.5 * a.sampleRate ? 440 : 880) / a.sampleRate;
            const int16_t x = (int16_t)(0.03 * std::sin(phase) * 32767);
            pcm.push_back(x);
            pcm.push_back(x);
        }
        w.PushAudio(pcm.data(), pcm.size() / 2);
        pushed = want;
        wrote = s && w.WriteVideo(s.Get(), (int64_t)k * 10'000'000 / fps, 10'000'000 / fps);
    }
    const std::wstring path = dir + (hardware ? L"\\selftest_replay_hw.mp4" : L"\\selftest_replay_sw.mp4");
    double seconds = 0;
    std::wstring saveErr;
    const size_t held = ring.Fragments();
    const std::wstring frags = ring.Describe();
    const double heldSec = ring.Held() / 1e7, mb = ring.Bytes() / 1e6;
    const bool saved = wrote && ring.Save(path, 12 * 10'000'000LL, 5 * 10'000'000LL, 3000, seconds, saveErr);
    w.Abort();
    bool ok = saved && ring.Rewrites() == 0;
    std::wstring seen;
    if (saved) {
        if (auto c = VideoClip::Open(path, err)) {
            auto rd = c->NewReader();
            const uint8_t* px0 = rd->Get(0);  // the reader's buffer: read it before the next Get
            const int i0 = px0 ? ReadIndex(px0, W, H) : -1;
            const uint8_t* px1 = c->Count() ? rd->Get(c->Count() - 1) : nullptr;
            const int i1 = px1 ? ReadIndex(px1, W, H) : -1;
            int rate = 0;
            const std::vector<int16_t> st = ReadAllAudio(*c, 0, 5.0, rate);
            const double step = ToneSwitch(st, rate, 0.5, 440, 880);
            seen = util::Format(L"%zu frames, %.3f s (file %.3f), #%d..#%d, 440->880 at %.3f s (2.500)", c->Count(),
                                c->EndTs() / 1e7, seconds, i0, i1, step);
            ok = ok && c->Count() == 150 && std::fabs(c->EndTs() / 1e7 - 5.0) < 0.05 && i0 == 7 && i1 == 11 &&
                 c->HasAudio() && std::fabs(step - 2.5) < 0.02;
        } else {
            seen = L"can't open: " + err;
            ok = false;
        }
    }
    log(util::Format(L"replay ring (%s): %s · %s · held %zu fragments %.2f s %.1f MB, rewrites %zu · %s · %s %s",
                     hardware ? L"hardware" : L"software", ok ? L"ok" : L"FAIL", enc.c_str(), held, heldSec, mb,
                     ring.Rewrites(), frags.c_str(), seen.c_str(), saveErr.c_str()));
    return ok ? 0 : 1;
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

#if __has_include(<mkvparser/mkvparser.h>) && __has_include(<vpx/vp8dx.h>) && __has_include(<opus/opus.h>) && \
    __has_include(<libyuv.h>)
class WebmFile : public mkvparser::IMkvReader {
public:
    explicit WebmFile(FILE* f) : f_(f) {
        _fseeki64(f_, 0, SEEK_END);
        size_ = _ftelli64(f_);
    }
    int Read(long long pos, long len, unsigned char* buf) override {
        if (pos < 0 || len < 0 || pos + len > size_) return -1;
        _fseeki64(f_, pos, SEEK_SET);
        return fread(buf, 1, len, f_) == (size_t)len ? 0 : -1;
    }
    int Length(long long* total, long long* available) override {
        if (total) *total = size_;
        if (available) *available = size_;
        return 0;
    }

private:
    FILE* f_;
    long long size_ = 0;
};

bool WebmReaderBuilt() { return true; }

WebmContents ReadWebm(const std::wstring& path) {
    WebmContents c;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) {
        c.err = L"can't open";
        return c;
    }
    WebmFile file(f);
    long long pos = 0;
    mkvparser::EBMLHeader ebml;
    mkvparser::Segment* raw = nullptr;
    if (ebml.Parse(&file, pos) < 0 || !ebml.m_docType || strcmp(ebml.m_docType, "webm") != 0 ||
        mkvparser::Segment::CreateInstance(&file, pos, raw) != 0 || !raw) {
        fclose(f);
        c.err = L"not a WebM file";
        return c;
    }
    std::unique_ptr<mkvparser::Segment> seg(raw);
    vpx_codec_ctx_t vpx{};
    vpx_codec_dec_cfg_t cfg{1, 0, 0};
    int oerr = 0;
    OpusDecoder* opus = opus_decoder_create(48000, 2, &oerr);
    long long videoNum = -1, audioNum = -1;
    size_t preSkip = 0;
    if (seg->Load() < 0 || vpx_codec_dec_init(&vpx, vpx_codec_vp9_dx(), &cfg, 0) != VPX_CODEC_OK || !opus) {
        c.err = L"can't load it";
    } else {
        c.seconds = seg->GetInfo()->GetDuration() / 1e9;
        const mkvparser::Tracks* tracks = seg->GetTracks();
        for (unsigned long i = 0; i < tracks->GetTracksCount(); ++i) {
            const mkvparser::Track* t = tracks->GetTrackByIndex(i);
            if (!t || !t->GetCodecId()) continue;
            if (t->GetType() == mkvparser::Track::kVideo && strcmp(t->GetCodecId(), "V_VP9") == 0) {
                videoNum = t->GetNumber();
                c.width = (int)static_cast<const mkvparser::VideoTrack*>(t)->GetWidth();
                c.height = (int)static_cast<const mkvparser::VideoTrack*>(t)->GetHeight();
            } else if (t->GetType() == mkvparser::Track::kAudio && strcmp(t->GetCodecId(), "A_OPUS") == 0) {
                audioNum = t->GetNumber();
                size_t n = 0;
                const unsigned char* head = t->GetCodecPrivate(n);
                if (head && n >= 12) preSkip = head[10] | head[11] << 8;  // OpusHead
            }
        }
        std::vector<unsigned char> buf;
        std::vector<int16_t> pcm(5760 * 2);
        std::vector<uint8_t> bgra;
        c.ok = videoNum >= 0;
        for (const mkvparser::Cluster* cl = seg->GetFirst(); c.ok && cl && !cl->EOS(); cl = seg->GetNext(cl)) {
            const mkvparser::BlockEntry* be = nullptr;
            if (cl->GetFirst(be) < 0) break;
            while (c.ok && be && !be->EOS()) {
                const mkvparser::Block* b = be->GetBlock();
                for (int k = 0; c.ok && k < b->GetFrameCount(); ++k) {
                    const mkvparser::Block::Frame& fr = b->GetFrame(k);
                    buf.resize((size_t)fr.len);
                    if (fr.Read(&file, buf.data()) < 0) {
                        c.ok = false;
                        c.err = L"can't read a block";
                    } else if (b->GetTrackNumber() == videoNum) {
                        if (vpx_codec_decode(&vpx, buf.data(), (unsigned)buf.size(), nullptr, 0) != VPX_CODEC_OK) {
                            c.ok = false;
                            c.err = L"VP9 decoding failed";
                        }
                        if (b->IsKey()) c.keys.push_back((int)c.times.size());
                        c.times.push_back(b->GetTime(cl) / 1e9);
                        vpx_codec_iter_t it = nullptr;
                        while (vpx_image_t* img = vpx_codec_get_frame(&vpx, &it)) {
                            const int w = (int)img->d_w, h = (int)img->d_h;
                            bgra.resize((size_t)w * h * 4);
                            libyuv::I420ToARGB(img->planes[0], img->stride[0], img->planes[1], img->stride[1],
                                               img->planes[2], img->stride[2], bgra.data(), w * 4, w, h);
                            c.indexes.push_back(ReadIndex(bgra.data(), w, h));
                            if (c.first.empty()) c.first = bgra;
                        }
                    } else if (b->GetTrackNumber() == audioNum) {
                        int n = opus_decode(opus, buf.data(), (opus_int32)buf.size(), pcm.data(), 5760, 0);
                        n -= (int)std::min<long long>(n, b->GetDiscardPadding() * 48000 / 1'000'000'000);
                        if (n > 0) c.audio.insert(c.audio.end(), pcm.begin(), pcm.begin() + n * 2);
                    }
                }
                if (cl->GetNext(be, be) < 0) break;
            }
        }
        c.audio.erase(c.audio.begin(), c.audio.begin() + std::min(c.audio.size(), preSkip * 2));
    }
    vpx_codec_destroy(&vpx);
    if (opus) opus_decoder_destroy(opus);
    fclose(f);
    return c;
}
#else
bool WebmReaderBuilt() { return false; }
WebmContents ReadWebm(const std::wstring&) { return {}; }
#endif

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
    // The same trim as WebM (#40), read back with libwebm: every frame in order, the sound where it was.
    if (FormatAvailable(Format::WebM) && WebmReaderBuilt()) {
        ExportJob job;
        job.clip = clip;
        job.first = 37;
        job.last = 150;
        job.crop = {0, 0, W, H};
        job.outW = W;
        job.outH = H;
        job.fpsLimit = fps;
        job.format = Format::WebM;
        job.quality = 70;
        job.audio.enabled = true;
        job.path = dir + L"\\vt_trim.webm";
        t0 = util::NowSeconds();
        ExportResult r = ExportSync(ex, job);
        WebmContents wc = r.ok ? ReadWebm(job.path) : WebmContents{};
        int bad = 0;
        for (size_t k = 0; k < wc.indexes.size(); ++k) bad += wc.indexes[k] != 37 + (int)k;
        double sw1 = ToneSwitch(wc.audio, 48000, 0.05, 440, 880), sw2 = ToneSwitch(wc.audio, 48000, sw1 + 0.2, 880, 1320);
        double e1 = srcSwitch - clip->Ts(37) / 1e7, e2 = e1 + 1.0;
        check(r.ok && r.audio && wc.ok && wc.indexes.size() == 114 && bad == 0 && std::fabs(sw1 - e1) < 0.03 &&
                  std::fabs(sw2 - e2) < 0.03,
              util::Format(L"WebM trim: %zu frames, %d out of place, audio %d, switches at %.3f / %.3f s, expected "
                           L"%.3f / %.3f, %s, %.0f ms %s",
                           wc.indexes.size(), bad, r.audio, sw1, sw2, e1, e2, util::FormatBytes(r.bytes).c_str(),
                           (util::NowSeconds() - t0) * 1000, (r.error + wc.err).c_str()));
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
    settings_.previewVolume = 60;
    shot(L"editor_video.png");
    hasResult_ = resultOriginal_ = true;
    lastResult_ = {};
    lastResult_.ok = true;
    lastResult_.path = src;
    lastResult_.bytes = util::FileSize(src);
    lastVideo_.path = src;  // as if just recorded: 삭제 shows
    shot(L"editor_video_saved.png");
    // The notice at the start after a crash mid-recording (#24).
    Alert(L"지난번에 OzMaker가 비정상 종료되었습니다. 녹화하던 동영상을 되살렸습니다 · OzMaker_20261102_142311.mp4.");
    shot(L"editor_video_recovered.png");
    toastError_ = toastSticky_ = false;
    toastUntil_ = 0;
    settings_.speedIdx = 3;
    hasResult_ = false;
    shot(L"editor_video_fast.png");
    settings_.speedIdx = 1;
    settings_.previewVolume = 100;
    lastVideo_ = {};
    CloseEditor();
    log(failures ? util::Format(L"RESULT: %d failure(s)", failures) : L"RESULT: all passed");
    return failures ? 2 : 0;
}

// Region picker on the primary monitor, driven with posted messages: drag, move, resize, ratio, keys, the
// toolbar's countdown, save and record, and opening on the area picked before.
// The 동영상 settings a user reported failing (HEVC · CBR · 5 s keyframes at 2474×1392, "AAC 인코더를 시작할 수
// 없습니다"), then each with one thing changed: which encoder takes each, and where the ones that fail stop.
int App::RunEncoderTest(const std::wstring& dir) {
    testWindowsOnly_ = true;
    util::EnsureDir(dir);
    Log log(dir + L"\\encoder-test.log");
    VideoConfig base;
    base.width = 2474;
    base.height = 1392;
    base.fps = 30;
    base.hevc = true;
    base.cbr = true;
    base.gopFrames = 150;
    base.fragmented = true;  // as while recording
    const std::pair<const wchar_t*, std::function<void(VideoConfig&, bool&)>> cases[] = {
        {L"as reported", [](VideoConfig&, bool&) {}},
        {L"H.264", [](VideoConfig& v, bool&) { v.hevc = false; }},
        {L"VBR", [](VideoConfig& v, bool&) { v.cbr = false; }},
        {L"keyframes 2 s", [](VideoConfig& v, bool&) { v.gopFrames = 60; }},
        {L"2480x1392 (width a multiple of 16)", [](VideoConfig& v, bool&) { v.width = 2480; }},
        {L"1920x1080", [](VideoConfig& v, bool&) { v.width = 1920, v.height = 1080; }},
        {L"no sound", [](VideoConfig&, bool& sound) { sound = false; }},
        {L"not fragmented", [](VideoConfig& v, bool&) { v.fragmented = false; }},
        {L"software only", [](VideoConfig& v, bool&) { v.hardware = false; }},
        // 설정 › 동영상 › 비트레이트 직접 입력 (#53): its ends.
        {L"150 Mbps (the custom maximum)", [](VideoConfig& v, bool&) { v.bitrate = kVideoBitrateMaxKbps * 1000u; }},
        {L"H.264 VBR 150 Mbps",
         [](VideoConfig& v, bool&) { v.hevc = v.cbr = false, v.bitrate = kVideoBitrateMaxKbps * 1000u; }},
        {L"H.264 0.5 Mbps (the custom minimum)",
         [](VideoConfig& v, bool&) { v.hevc = false, v.bitrate = kVideoBitrateMinKbps * 1000u; }},
        {L"software H.264 150 Mbps",
         [](VideoConfig& v, bool&) { v.hevc = v.hardware = false, v.bitrate = kVideoBitrateMaxKbps * 1000u; }},
    };
    int failures = 0;
    for (const auto& [name, change] : cases) {
        VideoConfig v = base;
        bool sound = true;
        change(v, sound);
        std::wstring err, got;
        const bool ok = ProbeMp4(dir + L"\\probe.mp4", v, sound, err, got);
        const std::wstring rate = v.bitrate ? util::Format(L"%.1f Mbps", v.bitrate / 1e6) : L"auto";
        log(util::Format(L"%s: %s · %s %dx%d %s %s, keyframes %d · %s", name, ok ? L"ok" : L"FAIL",
                         v.hevc ? L"HEVC" : L"H.264", v.width, v.height, v.cbr ? L"CBR" : L"VBR", rate.c_str(),
                         v.gopFrames, (ok ? got : err + L" · " + got).c_str()));
        if (!ok) ++failures;
    }
    DeleteFileW((dir + L"\\probe.mp4").c_str());
    log(failures ? util::Format(L"RESULT: %d failure(s)", failures) : L"RESULT: all passed");
    return failures ? 2 : 0;
}

// Desktop duplication (#20: no yellow border on Windows 10), as a region over a topmost window of ours, so nothing
// else is on it: kept as a clip and as an MP4, each must show the window's blocks of colour where they are, and the
// square that blinks in its corner (the screen's changes come through).
static int DuplicationCheck(HINSTANCE inst, const std::wstring& dir, Log& log) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = PatternProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"OzMakerTestPattern";
    RegisterClassExW(&wc);
    g_patternPlain = false;
    g_patternBlinks = 0;
    HWND win = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName,
                               L"OzMaker Test Window", WS_POPUP, 40, 40, 480, 272, nullptr, nullptr, inst, nullptr);
    if (!win) {
        log(L"FAIL desktop duplication: no test window");
        return 1;
    }
    ShowWindow(win, SW_SHOWNOACTIVATE);
    SetTimer(win, 1, 300, nullptr);
    // A white window over the second block, kept out of captures like the REC pill and the save notice: it must not
    // show in what duplication takes either.
    WNDCLASSEXW cc{sizeof(cc)};
    cc.lpfnWndProc = DefWindowProcW;
    cc.hInstance = inst;
    cc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    cc.lpszClassName = L"OzMakerTestCover";
    RegisterClassExW(&cc);
    HWND cover = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, cc.lpszClassName, L"", WS_POPUP,
                                 40 + 150, 40 + 48, 60, 40, nullptr, nullptr, inst, nullptr);
    if (cover) {
        SetWindowDisplayAffinity(cover, WDA_EXCLUDEFROMCAPTURE);
        ShowWindow(cover, SW_SHOWNOACTIVATE);
    }
    auto pump = [](double seconds) {
        const double end = util::NowSeconds() + seconds;
        MSG msg;
        while (util::NowSeconds() < end) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
            MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, 0);
        }
    };
    pump(0.5);
    RECT r;
    GetWindowRect(win, &r);
    const int w = r.right - r.left, h = r.bottom - r.top;
    // The blocks of colour (BGRA) at their centres, and whether the square in the corner shows.
    auto blocks = [&](const uint8_t* px, int pw, int ph) {
        int n = 0;
        for (int i = 0; i < 8; ++i) {
            const int x = pw * (2 * (i % 4) + 1) / 8, y = ph * (2 * (i / 4) + 1) / 4;
            const uint8_t* p = px + ((size_t)y * pw + x) * 4;
            const COLORREF c = kPatternColors[i];
            n += std::abs(p[2] - GetRValue(c)) < 40 && std::abs(p[1] - GetGValue(c)) < 40 && std::abs(p[0] - GetBValue(c)) < 40;
        }
        return n;
    };
    auto square = [](const uint8_t* px, int pw, int ph) {
        const int x = 28 * pw / 480, y = 28 * ph / 272;
        const uint8_t* p = px + ((size_t)y * pw + x) * 4;
        return p[0] > 200 && p[1] > 200 && p[2] > 200;
    };
    int failures = 0;
    for (bool mp4 : {false, true}) {
        CaptureOptions o;
        o.source = CaptureSource::Region;
        o.region = r;
        o.monitor = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
        o.cursor = true;  // drawn only where the pointer is: one block may be under it
        o.fps = 30;
        o.screenVia = 1;
        if (mp4) {
            VideoOptions v;
            v.path = dir + L"\\duplication.mp4";
            v.video.hardware = true;
            v.video.gopFrames = 60;
            o.video = v;
            o.maxEdge = 4096;
        }
        Capture cap;
        std::wstring err;
        if (!OnlyOursOver(r)) {
            log(L"FAIL desktop duplication: another program's window is over the test window, nothing taken");
            ++failures;
            break;
        }
        if (!cap.Start(o, nullptr, 0, err)) {
            log(L"FAIL desktop duplication: " + err);
            ++failures;
            continue;
        }
        const bool dup = cap.Duplicating();
        pump(2.2);
        const size_t frames = cap.Frames();
        CaptureResult res = cap.Stop();
        if (!OnlyOursOver(r)) {
            // It may have been in what was taken: nothing of it is looked at or kept.
            if (res.video) DeleteFileW(res.path.c_str());
            log(L"FAIL desktop duplication: another program's window came over the test window, what was taken is gone");
            ++failures;
            break;
        }
        int best = 0, on = 0, off = 0, covered = 0;
        size_t count = 0;
        std::wstring got;
        auto look = [&](const uint8_t* px, int pw, int ph) {
            best = std::max(best, blocks(px, pw, ph));
            (square(px, pw, ph) ? on : off)++;
            const uint8_t* c = px + ((size_t)(68 * ph / 272) * pw + 180 * pw / 480) * 4;
            covered += cover && std::min({c[0], c[1], c[2]}) > 200;
        };
        if (mp4) {
            std::wstring verr;
            if (auto vc = res.ok ? VideoClip::Open(res.path, verr) : nullptr) {
                count = vc->Count();
                auto rd = vc->NewReader();
                for (size_t i = 0; i < count; i += 3)
                    if (const uint8_t* px = rd->Get(i)) look(px, vc->Width(), vc->Height());
                got = util::Format(L"%.2f s", vc->Seconds());
            } else {
                got = res.error + verr;
            }
        } else if (res.clip) {
            count = res.clip->Count();
            auto rd = res.clip->NewReader();
            for (size_t i = 0; i < count; ++i)
                if (const uint8_t* px = rd->Get(i)) look(px, res.clip->Width(), res.clip->Height());
            got = util::Format(L"%dx%d", res.clip->Width(), res.clip->Height());
        }
        const bool ok = dup && count >= 3 && best >= 7 && on > 0 && off > 0 && covered == 0;
        if (!ok) ++failures;
        log(util::Format(L"%s desktop duplication (%s): %s, %zu frames (%zu taken) %s, blocks %d/8, square on %d off %d, "
                         L"excluded window seen in %d",
                         ok ? L"ok  " : L"FAIL", mp4 ? L"MP4" : L"clip", dup ? L"duplicated" : L"not duplicated", count,
                         frames, got.c_str(), best, on, off, covered));
    }
    if (cover) DestroyWindow(cover);
    DestroyWindow(win);
    return failures;
}

// Window capture of a window of ours that another window of ours covers in part while it isn't in front (Alt+Tab
// out of a game): the covered part must come out as the window drew it, never black. Once drawn with GDI, once
// through a flip-model swap chain like a game's (redrawn every frame). (Tried once at the size of the whole monitor
// too, the same: left out, it hides the screen.)
static int CoveredCaptureCheck(HINSTANCE inst, Log& log) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = PatternProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"OzMakerTestPattern";
    RegisterClassExW(&wc);
    WNDCLASSEXW fc{sizeof(fc)};
    fc.lpfnWndProc = DefWindowProcW;
    fc.hInstance = inst;
    fc.lpszClassName = L"OzMakerTestFlip";
    RegisterClassExW(&fc);
    WNDCLASSEXW cc{sizeof(cc)};
    cc.lpfnWndProc = DefWindowProcW;
    cc.hInstance = inst;
    cc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    cc.lpszClassName = L"OzMakerTestCover";
    RegisterClassExW(&cc);
    g_patternPlain = false;
    int failures = 0;
    for (bool flip : {false, true}) {
        const RECT wr{60, 60, 60 + 480, 60 + 272};
        const int W = wr.right - wr.left, H = wr.bottom - wr.top;
        // Windows won't capture a window that can't be activated: an ordinary one, shown without taking the focus.
        HWND win = CreateWindowExW(0, flip ? fc.lpszClassName : wc.lpszClassName, L"OzMaker Test Window", WS_POPUP,
                                   wr.left, wr.top, W, H, nullptr, nullptr, inst, nullptr);
        ShowWindow(win, SW_SHOWNOACTIVATE);
        ComPtr<ID3D11Device> dev;
        ComPtr<ID3D11DeviceContext> ctx;
        ComPtr<ID3D11DeviceContext1> ctx1;
        ComPtr<IDXGISwapChain1> swap;
        ComPtr<ID3D11RenderTargetView> rtv;
        if (flip) {
            D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                              D3D11_SDK_VERSION, &dev, nullptr, &ctx);
            ComPtr<IDXGIDevice> dxgi;
            ComPtr<IDXGIAdapter> ad;
            ComPtr<IDXGIFactory2> factory;
            if (dev && SUCCEEDED(dev.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&ad)) &&
                SUCCEEDED(ad->GetParent(IID_PPV_ARGS(&factory)))) {
                DXGI_SWAP_CHAIN_DESC1 sd{};
                sd.Width = W;
                sd.Height = H;
                sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                sd.SampleDesc.Count = 1;
                sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                sd.BufferCount = 2;
                sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                factory->CreateSwapChainForHwnd(dev.Get(), win, &sd, nullptr, nullptr, &swap);
                ctx.As(&ctx1);
            }
        }
        // The game's frame: the eight blocks, drawn again each time like a game draws.
        auto present = [&] {
            if (!swap || !ctx1) return;
            ComPtr<ID3D11Texture2D> back;
            swap->GetBuffer(0, IID_PPV_ARGS(&back));
            rtv.Reset();
            dev->CreateRenderTargetView(back.Get(), nullptr, &rtv);
            for (int i = 0; i < 8; ++i) {
                const COLORREF c = kPatternColors[i];
                const float col[4] = {GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, 1.f};
                const D3D11_RECT r{W * (i % 4) / 4, H * (i / 4) / 2, W * (i % 4 + 1) / 4, H * (i / 4 + 1) / 2};
                ctx1->ClearView(rtv.Get(), col, &r, 1);
            }
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            rtv.Reset();
            swap->Present(1, 0);
        };
        auto pump = [&](double seconds) {
            const double end = util::NowSeconds() + seconds;
            MSG msg;
            while (util::NowSeconds() < end) {
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
                present();
                if (!swap) MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, 0);
            }
        };
        pump(0.4);
        // Over the second block, in front of the window (which isn't in front itself: it never takes the focus).
        HWND cover = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, cc.lpszClassName, L"", WS_POPUP,
                                     wr.left + W * 3 / 8 - 45, wr.top + H / 4 - 35, 90, 70, nullptr, nullptr, inst, nullptr);
        ShowWindow(cover, SW_SHOWNOACTIVATE);
        pump(0.3);
        CaptureOptions o;
        o.source = CaptureSource::Window;
        o.target = win;
        o.clientOnly = true;
        o.cursor = false;
        o.fps = 30;
        Capture cap;
        std::wstring err;
        int black = 0, right = 0, white = 0, seen = 0, blocksBest = 0;
        if (cap.Start(o, nullptr, 0, err)) {
            pump(1.2);
            CaptureResult res = cap.Stop();
            if (res.clip) {
                auto rd = res.clip->NewReader();
                const int pw = res.clip->Width(), ph = res.clip->Height();
                for (size_t i = 0; i < res.clip->Count(); ++i) {
                    const uint8_t* px = rd->Get(i);
                    if (!px) continue;
                    const uint8_t* p = px + ((size_t)(ph / 4) * pw + pw * 3 / 8) * 4;
                    int nb = 0;
                    for (int b = 0; b < 8; ++b) {
                        const uint8_t* q = px + ((size_t)(ph * (2 * (b / 4) + 1) / 4) * pw + pw * (2 * (b % 4) + 1) / 8) * 4;
                        const COLORREF k = kPatternColors[b];
                        nb += std::abs(q[2] - GetRValue(k)) < 40 && std::abs(q[1] - GetGValue(k)) < 40 &&
                              std::abs(q[0] - GetBValue(k)) < 40;
                    }
                    blocksBest = std::max(blocksBest, nb);
                    const COLORREF c = kPatternColors[1];
                    ++seen;
                    black += p[0] < 30 && p[1] < 30 && p[2] < 30;
                    white += p[0] > 220 && p[1] > 220 && p[2] > 220;
                    right += std::abs(p[2] - GetRValue(c)) < 40 && std::abs(p[1] - GetGValue(c)) < 40 &&
                             std::abs(p[0] - GetBValue(c)) < 40;
                }
            }
        } else {
            log(L"FAIL covered window capture: " + err);
        }
        const bool ok = seen > 0 && right == seen;
        if (!ok) ++failures;
        log(util::Format(L"%s covered window capture (%s): %d frames, under the cover: %d as drawn, %d black, %d white, "
                         L"blocks %d/8",
                         ok ? L"ok  " : L"FAIL", flip ? L"flip-model swap chain" : L"GDI", seen, right, black, white,
                         blocksBest));
        DestroyWindow(cover);
        swap.Reset();
        DestroyWindow(win);
    }

    // Through the screen (monitorMode, the way a fullscreen game is taken), by desktop duplication and by
    // Windows.Graphics.Capture: a window of ours over the target while it isn't in front must never show, and its
    // place is the target as last seen with nothing over it, not black. The target stays topmost, so nothing but the
    // cover is ever over it.
    for (int via : {1, 2}) {
        HWND win = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"OzMaker Test Window", WS_POPUP, 60, 60, 480, 272,
                                   nullptr, nullptr, inst, nullptr);
        ShowWindow(win, SW_SHOWNOACTIVATE);
        auto pump = [](double seconds) {
            const double end = util::NowSeconds() + seconds;
            MSG msg;
            while (util::NowSeconds() < end) {
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
                MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, 0);
            }
        };
        pump(0.4);
        CaptureOptions o;
        o.source = CaptureSource::Window;
        o.target = win;
        o.monitorMode = true;
        o.clientOnly = true;
        o.cursor = false;
        o.fps = 30;
        o.screenVia = via;
        Capture cap;
        std::wstring err;
        RECT tr;
        GetWindowRect(win, &tr);
        if (!OnlyOursOver(tr)) {
            log(L"FAIL covered screen capture: another program's window is over the test window, nothing taken");
            ++failures;
            DestroyWindow(win);
            break;
        }
        if (!cap.Start(o, nullptr, 0, err)) {
            log(L"FAIL covered screen capture: " + err);
            ++failures;
            DestroyWindow(win);
            continue;
        }
        const bool dup = cap.Duplicating();
        pump(0.6);
        const size_t before = cap.Frames();
        // The cover is another program's window (one of the target's own process would count as its popup).
        std::wstring cmd = L"\"" + util::ExePath() + L"\" --test-window 1.6 cover 195 93 90 70";
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        const bool spawned = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
        pump(0.5);  // it starts and shows
        // Changes go on under the cover, so frames keep coming while it is there.
        const size_t covered = cap.Frames();
        SetTimer(win, 1, 200, nullptr);
        pump(0.8);
        const bool clear = OnlyOursOver(tr, spawned ? pi.dwProcessId : 0);  // only the cover may be another's
        CaptureResult res = cap.Stop();
        if (spawned) {
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        if (!clear) {
            log(L"FAIL covered screen capture: another program's window came over the test window, nothing looked at");
            ++failures;
            DestroyWindow(win);
            break;
        }
        int right = 0, black = 0, white = 0, after = 0;
        if (res.clip) {
            auto rd = res.clip->NewReader();
            const int pw = res.clip->Width(), ph = res.clip->Height();
            for (size_t i = std::min(covered + 2, res.clip->Count()); i < res.clip->Count(); ++i) {
                const uint8_t* px = rd->Get(i);
                if (!px) continue;
                const uint8_t* p = px + ((size_t)(ph / 4) * pw + pw * 3 / 8) * 4;
                const COLORREF c = kPatternColors[1];
                ++after;
                black += p[0] < 30 && p[1] < 30 && p[2] < 30;
                white += p[0] > 220 && p[1] > 220 && p[2] > 220;
                right += std::abs(p[2] - GetRValue(c)) < 40 && std::abs(p[1] - GetGValue(c)) < 40 &&
                         std::abs(p[0] - GetBValue(c)) < 40;
            }
        }
        const bool ok = spawned && after > 0 && right == after;
        if (!ok) ++failures;
        log(util::Format(L"%s covered screen capture (%s): %zu frames before the cover, %d after it: under it %d as last "
                         L"seen, %d black, %d of the cover",
                         ok ? L"ok  " : L"FAIL", dup ? L"desktop duplication" : L"Windows.Graphics.Capture", before, after,
                         right, black, white));
        DestroyWindow(win);
    }
    return failures;
}

int App::RunReplayTest(const std::wstring& dir) {
    testWindowsOnly_ = true;
    util::EnsureDir(dir);
    Log log(dir + L"\\replay-test.log");
    int failures = 0;
    failures += CoveredCaptureCheck(inst_, log);
    if (!Mp4Writer::FindEncoder(false, true).empty()) failures += ReplayRingCheck(dir, true, log);
    failures += ReplayRingCheck(dir, false, log);
    failures += DuplicationCheck(inst_, dir, log);

    // The whole of it in the app: replay on for a program (the test window, in a process of its own: other windows
    // are left out), what it holds saved as the save key does, and again what is left once the program closed.
    auto check = [&](bool ok, const std::wstring& what) {
        if (!ok) ++failures;
        log((ok ? L"ok   " : L"FAIL ") + what);
    };
    settings_.outDir = dir;
    settings_.sounds = false;
    settings_.rpTargets = {{false, util::ExePath(), ProgramName(util::ExePath()), true}};
    settings_.rpMic = false;
    if (!CreateMainWindow(SW_HIDE)) return 1;
    auto pump = [&](double seconds, const std::function<bool()>& until) {
        const double end = util::NowSeconds() + seconds;
        MSG msg;
        while (util::NowSeconds() < end && !(until && until())) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    };
    SetReplayOn(true, false);
    check(ReplayState() == RpState::Waiting, L"replay app: waits while the program doesn't run");
    std::wstring cmd = L"\"" + util::ExePath() + L"\" --test-window 9 capture";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        check(false, L"replay app: can't start --test-window");
    } else {
        pump(8, [&] { return rpCap_.Running(); });
        check(rpCap_.Running() && ReplayState() == RpState::Holding && rpHeld_.name == settings_.rpTargets[0].name,
              L"replay app: holds the program once its window opens");
        // Excluded from capture, the open window was drawn shifted and flickering: replay leaves it as it is.
        DWORD affinity = WDA_MONITOR;
        check(GetWindowDisplayAffinity(hwnd_, &affinity) && affinity == WDA_NONE,
              L"replay app: the app window isn't excluded from capture while holding");
        pump(1.0, nullptr);
        // 스크린샷 (#63): what is held, without stopping it.
        check(shotHotkeyOk_, L"shot: the key is registered while the replay holds a program");
        lastShot_.clear();
        PostMessageW(hwnd_, WM_HOTKEY, kHotkeyShot, 0);
        pump(5, [&] { return !lastShot_.empty() && !shotsWaiting_; });
        {
            std::vector<uint8_t> px;
            int iw = 0, ih = 0;
            RECT cr{};
            GetClientRect(rpHeld_.hwnd, &cr);
            const bool read = ReadImageFile(lastShot_, px, iw, ih);
            const std::vector<uint8_t> black(px.size(), 0);
            check(read && iw == cr.right && ih == cr.bottom && WorstColour(px, black) > 30 && rpCap_.Running() &&
                      lastShot_.rfind(ImageDir() + L"\\", 0) == 0,
                  util::Format(L"shot: the hotkey saves what the replay holds, %dx%d, and it goes on (%s)", iw, ih,
                               util::FileNameOf(lastShot_).c_str()));
        }
        pump(2.5, nullptr);
        ReplaySave();
        pump(8, [&] { return !rpSaving_; });
        const std::wstring first = rpSave_.path;
        double seconds = 0;
        bool audio = false;
        std::wstring err;
        if (auto v = VideoClip::Open(first, err)) {
            seconds = v->Seconds();
            audio = v->HasAudio();
        }
        check(rpSave_.ok && seconds > 2.5 && seconds < 5.5,
              util::Format(L"replay app: saved %.2f s%s (%s)", seconds, audio ? L" with sound" : L"",
                           rpSave_.ok ? util::FileNameOf(first).c_str() : rpSave_.err.c_str()));
        check(first.rfind(ReplayDir() + L"\\" + util::SafeFileName(settings_.rpTargets[0].name, 60) + L"\\", 0) == 0,
              L"replay app: saved in 저장 폴더 › 리플레이 › the program");
        check(!rpFiles_.empty() && rpFiles_[0].path == first && std::abs(rpFiles_[0].seconds - seconds) < 0.05 &&
                  rpNewPath_ == first,
              L"replay app: the saved list shows it first and new, its length read from the file");
        check(rpNotice_.Visible(), L"replay app: the save notice shows over the program");
        rpNotice_.Hide();
        // 삭제할 때 › 바로 삭제: on a copy, so what follows keeps its file. The menu asks; the list follows.
        const std::wstring spare = first.substr(0, first.size() - 4) + L"_spare.mp4";
        check(CopyFileW(first.c_str(), spare.c_str(), FALSE) != 0, L"replay app: a copy to delete");
        RefreshReplayFiles();
        const auto row = [&] {
            return std::find_if(rpFiles_.begin(), rpFiles_.end(), [&](const auto& f) { return f.path == spare; });
        };
        settings_.deleteMode = kDeleteNow;
        const size_t listed = rpFiles_.size();
        TrashReplay((size_t)(row() - rpFiles_.begin()));
        const bool asked = question_.has_value() && question_->danger &&
                           GetFileAttributesW(spare.c_str()) != INVALID_FILE_ATTRIBUTES;
        auto yes = asked ? std::move(question_->onYes) : nullptr;
        question_.reset();
        if (yes) yes();
        check(asked && GetFileAttributesW(spare.c_str()) == INVALID_FILE_ATTRIBUTES && row() == rpFiles_.end() &&
                  rpFiles_.size() == listed - 1,
              L"replay app: 바로 삭제 asks first, then the file and its row are gone");
        settings_.deleteMode = kDeleteToBin;
        // The program closes: what was held stays to be saved, and goes once it is.
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        pump(4, [&] { return !rpCap_.Running(); });
        pump(1.2, nullptr);
        check(!rpCap_.Running() && !rpClosed_.path.empty() && ReplayState() == RpState::Waiting && rpRing_.Held() > 0,
              util::Format(L"replay app: the program closed, %.1f s of it kept", rpClosed_.seconds));
        ReplaySave();
        pump(8, [&] { return !rpSaving_; });
        check(rpSave_.ok && rpSave_.path != first && rpClosed_.path.empty() && rpRing_.Bytes() == 0,
              util::Format(L"replay app: saved what the closed program left (%.2f s), then let it go", rpSave_.seconds));
        rpNotice_.Hide();
    }
    // #61: windows of one program, each in a process of its own. A look at the other window keeps what is held, and
    // replay moves once that lasted as long as the replay. What a closed window left waits for the windows open with
    // it until it is saved; a window opened since starts over it.
    {
        std::vector<PROCESS_INFORMATION> procs;
        auto spawn = [&]() -> HWND {
            std::vector<HWND> had;
            for (const WinInfo& w : ReplayWindows()) had.push_back(w.hwnd);
            std::wstring line = L"\"" + util::ExePath() + L"\" --test-window 60 capture";
            STARTUPINFOW s{sizeof(s)};
            PROCESS_INFORMATION p{};
            if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &s, &p)) return nullptr;
            procs.push_back(p);
            HWND made = nullptr;
            pump(8, [&] {
                for (const WinInfo& w : ReplayWindows())
                    if (std::find(had.begin(), had.end(), w.hwnd) == had.end()) made = w.hwnd;
                return made != nullptr;
            });
            return made;
        };
        auto close = [&](HWND win) {
            DWORD pid = 0;
            GetWindowThreadProcessId(win, &pid);
            for (const PROCESS_INFORMATION& p : procs)
                if (p.dwProcessId == pid) {
                    TerminateProcess(p.hProcess, 0);
                    WaitForSingleObject(p.hProcess, 3000);
                }
        };
        const HWND a = spawn();
        pump(8, [&] { return rpCap_.Running() && rpHeld_.hwnd == a; });
        rpFrontForTest_ = a;
        const HWND b = spawn();
        pump(2.5, nullptr);
        const int64_t before = rpRing_.Held();
        rpFrontForTest_ = b;
        pump(2.5, nullptr);
        check(a && b && rpCap_.Running() && rpHeld_.hwnd == a && rpRing_.Held() > before,
              util::Format(L"replay windows: a look at the program's other window keeps what is held (%.1f s, then %.1f s)",
                           before / 1e7, rpRing_.Held() / 1e7));
        rpOtherSince_ -= kReplayLenOptions[settings_.rpLenIdx];
        pump(3, [&] { return rpCap_.Running() && rpHeld_.hwnd == b; });
        check(rpCap_.Running() && rpHeld_.hwnd == b, L"replay windows: it moves once that lasted as long as the replay");
        pump(2.5, nullptr);
        rpFrontForTest_ = a;
        close(b);
        pump(4, [&] { return !rpCap_.Running(); });
        pump(2.5, nullptr);
        check(!rpCap_.Running() && rpClosed_.left == std::vector<HWND>{a} && ReplayState() == RpState::Waiting &&
                  rpRing_.Held() > 10'000'000,
              util::Format(L"replay windows: the held window closed: %.1f s kept, the window still open waits", rpClosed_.seconds));
        ReplaySave();
        pump(8, [&] { return !rpSaving_; });
        rpNotice_.Hide();
        check(rpSave_.ok && rpClosed_.path.empty(), L"replay windows: what the closed window left is saved");
        pump(4, [&] { return rpCap_.Running(); });
        check(rpCap_.Running() && rpHeld_.hwnd == a, L"replay windows: then the window still open is held");
        const HWND c = spawn();
        pump(2.5, nullptr);
        rpFrontForTest_ = c;
        close(a);
        pump(4, [&] { return !rpCap_.Running(); });
        const bool kept = !rpClosed_.path.empty() && rpClosed_.left == std::vector<HWND>{c};
        const HWND d = spawn();
        pump(4, [&] { return rpCap_.Running(); });
        check(kept && rpCap_.Running() && rpHeld_.hwnd == d && rpClosed_.path.empty(),
              L"replay windows: a window opened since starts at once, over what was kept");
        rpFrontForTest_ = nullptr;
        for (const PROCESS_INFORMATION& p : procs) {
            TerminateProcess(p.hProcess, 0);
            CloseHandle(p.hProcess);
            CloseHandle(p.hThread);
        }
        pump(3, [&] { return !rpCap_.Running(); });
    }
    SetReplayOn(false, false);
    check(!rpCap_.Running() && rpRing_.Bytes() == 0, L"replay app: switched off, nothing is left in memory");
    quitting_ = true;
    DestroyWindow(hwnd_);
    pump(0.5, nullptr);
    log(failures ? util::Format(L"RESULT: %d failure(s)", failures) : L"RESULT: all passed");
    return failures ? 2 : 0;
}

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
    bool done = false;
    PickResult res = PickResult::Cancel;
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
    auto start = [&](std::vector<WinInfo> cands = {}, RECT last = {}) {
        done = false;
        res = PickResult::Cancel;
        regionPicker_.Start(std::move(cands), last, 1, [&](PickResult o, RECT r) {
            done = true;
            res = o;
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
    auto click = [&](RECT b) {  // a toolbar button, in screen pixels
        post(WM_LBUTTONDOWN, (b.left + b.right) / 2 - ox, (b.top + b.bottom) / 2 - oy, MK_LBUTTON);
        post(WM_LBUTTONUP, (b.left + b.right) / 2 - ox, (b.top + b.bottom) / 2 - oy);
        pump(0.12);
    };
    auto rel = [&](RECT r) {
        return util::Format(L"%d,%d-%d,%d", r.left - ox, r.top - oy, r.right - ox, r.bottom - oy);
    };
    auto is = [&](RECT r, int l, int t, int rr, int b) {
        return r.left - ox == l && r.top - oy == t && r.right - ox == rr && r.bottom - oy == b;
    };

    // Mouse moves redraw only what changed; that must leave the same pixels as drawing everything.
    auto stale = [&](const wchar_t* what) {
        size_t n = regionPicker_.StalePixels();
        check(n == 0, util::Format(L"%s: %zu pixel(s) differ from a full redraw", what, n));
    };

    start();
    for (int i = 0; i < 6; ++i) post(WM_MOUSEMOVE, 300 + i * 37, 200 + i * 23);
    pump(0.08);
    stale(L"guide lines");
    drag(200, 150, 1000, 600);
    RECT s = regionPicker_.AdjustedArea();
    check(!done && regionPicker_.Adjusting() && is(s, 200, 150, 1001, 601), L"drag, then adjusting: " + rel(s));
    stale(L"after the drag");
    drag(600, 375, 700, 425);
    s = regionPicker_.AdjustedArea();
    check(is(s, 300, 200, 1101, 651), L"move by 100,50: " + rel(s));
    stale(L"after the move");
    drag(1101, 651, 1151, 681);
    s = regionPicker_.AdjustedArea();
    check(is(s, 300, 200, 1151, 681), L"bottom-right handle +50,+30: " + rel(s));
    post(WM_MOUSEMOVE, 1151, 681);
    post(WM_MOUSEMOVE, 700, 500);
    pump(0.08);
    stale(L"handle hover");
    click(regionPicker_.ToolbarButton(3));  // 16:9
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
    stale(L"after the ratio drags");
    RECT before = s;
    key(VK_RIGHT);
    s = regionPicker_.AdjustedArea();
    check(s.left == before.left + 1 && s.right == before.right + 1 && s.top == before.top, L"arrow key moves 1px: " + rel(s));
    key(VK_RETURN);
    check(done && res == PickResult::Record && EqualRect(&got, &s) && !regionPicker_.Active(),
          L"Enter records: " + rel(got));

    start();
    post(WM_MOUSEMOVE, 400, 300);
    post(WM_LBUTTONDOWN, 400, 300, MK_LBUTTON);
    post(WM_LBUTTONUP, 400, 300);
    pump(0.12);
    s = regionPicker_.AdjustedArea();
    check(!done && regionPicker_.Adjusting() && EqualRect(&s, &mi.rcMonitor), L"click (no window) takes the screen: " + rel(s));
    key(VK_ESCAPE);
    check(done && res == PickResult::Cancel && !regionPicker_.Active(), L"Esc cancels");

    // The toolbar: its countdown menu, then "영역만 저장" keeps the area without recording.
    start();
    drag(200, 150, 1000, 600);
    click(regionPicker_.ToolbarButton(RegionPicker::kBarCountdown));
    check(regionPicker_.MenuOpen(), L"the countdown button opens its menu");
    stale(L"countdown menu");
    key(VK_ESCAPE);
    check(!done && !regionPicker_.MenuOpen() && regionPicker_.Countdown() == 1, L"Esc closes only the menu");
    click(regionPicker_.ToolbarButton(RegionPicker::kBarCountdown));
    click(regionPicker_.ToolbarButton(RegionPicker::kBarMenu + 2));
    check(!done && !regionPicker_.MenuOpen() && regionPicker_.Countdown() == 2, L"the menu picks 5 s and closes");
    stale(L"menu closed");
    s = regionPicker_.AdjustedArea();
    click(regionPicker_.ToolbarButton(RegionPicker::kBarSave));
    check(done && res == PickResult::Save && EqualRect(&got, &s), L"save only: " + rel(got));

    // Opened on the area picked before: the record button takes it as it is.
    RECT last{ox + 320, oy + 240, ox + 960, oy + 600};
    start({}, last);
    s = regionPicker_.AdjustedArea();
    check(!done && regionPicker_.Adjusting() && EqualRect(&s, &last), L"opens on the last area: " + rel(s));
    stale(L"last area");
    click(regionPicker_.ToolbarButton(RegionPicker::kBarRecord));
    check(done && res == PickResult::Record && EqualRect(&got, &last), L"record button: " + rel(got));
    // A click outside it starts over: no window there, so the whole monitor.
    start({}, last);
    post(WM_MOUSEMOVE, 100, 100);
    post(WM_LBUTTONDOWN, 100, 100, MK_LBUTTON);
    post(WM_LBUTTONUP, 100, 100);
    pump(0.12);
    s = regionPicker_.AdjustedArea();
    check(!done && regionPicker_.Adjusting() && EqualRect(&s, &mi.rcMonitor), L"click outside it takes the screen: " + rel(s));
    key(VK_ESCAPE);
    // Not when it no longer lies on a screen.
    start({}, RECT{vx - 3000, vy - 3000, vx - 2000, vy - 2400});
    check(!done && !regionPicker_.Adjusting(), L"a last area off the screens opens empty");
    key(VK_ESCAPE);

    // Hovering windows (made up here, not the user's): the area and its label follow the one under the cursor.
    WinInfo wa, wb;
    wa.title = L"OzMaker Test Window";
    wa.frame = {ox + 100, oy + 120, ox + 700, oy + 520};
    wb.title = L"OzMaker Test Window 2";
    wb.frame = {ox + 900, oy + 200, ox + 1300, oy + 640};
    start({wa, wb});
    // First onto this monitor: a move to a monitor of another scale draws everything (on the GPU).
    post(WM_MOUSEMOVE, 800, 700);
    post(WM_MOUSEMOVE, 300, 300);
    pump(0.05);
    stale(L"hovering a window");
    post(WM_MOUSEMOVE, 1000, 400);
    post(WM_MOUSEMOVE, 1010, 410);
    pump(0.05);
    stale(L"hovering another window");
    post(WM_MOUSEMOVE, 800, 700);
    pump(0.05);
    stale(L"hovering nothing");
    regionPicker_.ResetStats();
    for (int i = 0; i < 40; ++i) post(WM_MOUSEMOVE, 760 + i * 5, 650 + i * 3);
    pump(0.3);
    for (int i = 0; i < 5; ++i) regionPicker_.RedrawAll();
    const auto& st = regionPicker_.RedrawStats();
    double vpx = (double)GetSystemMetrics(SM_CXVIRTUALSCREEN) * GetSystemMetrics(SM_CYVIRTUALSCREEN);
    log(util::Format(L"redraws: %d partial, %.2f ms each, %.2f%% of the screen; %d full, %.2f ms each", st.partial,
                     st.partial ? st.partialSec * 1000 / st.partial : 0.0,
                     st.partial ? st.partialPx * 100.0 / st.partial / vpx : 0.0, st.full,
                     st.full ? st.fullSec * 1000 / st.full : 0.0));
    check(st.partial >= 40, util::Format(L"guide line moves redraw in part (%d of 40)", st.partial));
    key(VK_ESCAPE);
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
    check(screen_ == Screen::Home && showVideo_ && !clip_ && lastVideo_.path == path,
          L"saved video shows its card on the 녹화 tab");
    EditLastVideo();
    check(screen_ == Screen::Editor && clip_ && clip_->SlowSeek() && hasResult_ && resultOriginal_ &&
              fmt_ == (int)Format::Mp4,
          util::Format(L"the card's 편집 opens it in the 편집 tab (%zu frames, format %d)", clip_ ? clip_->Count() : 0,
                       fmt_));
    size_t first = SIZE_MAX;
    pump(0.5, [&] { return shown_ != SIZE_MAX; });
    first = shown_;
    pump(1.0);
    bool fg = GetForegroundWindow() == hwnd_;
    check(first != SIZE_MAX && shown_ != first && playing_,
          util::Format(L"plays through the fetcher: frame %zu -> %zu, foreground %d, sound preview %d", first, shown_,
                       fg, audioPreview_.Active()));
    if (fg) check(audioPreview_.Active(), L"sound preview runs while playing in front");
    // The volume (#36): 0 stops the sound like mute, and the speaker button brings it back at 50 %.
    settings_.previewVolume = 0;
    SyncAudioPreview();
    check(!audioPreview_.Active(), L"volume 0 stops the sound preview");
    TogglePreviewMute();
    SyncAudioPreview();
    check(!previewMute_ && settings_.previewVolume == 50 && (!fg || audioPreview_.Active()),
          util::Format(L"the speaker button turns a volume of 0 back on (%d%%)", settings_.previewVolume));
    TogglePreviewMute();
    SyncAudioPreview();
    check(previewMute_ && settings_.previewVolume == 50 && !audioPreview_.Active(),
          L"the speaker button mutes and keeps the volume");
    SetPreviewVolume(70);
    SyncAudioPreview();
    check(!previewMute_ && settings_.previewVolume == 70 && (!fg || audioPreview_.Active()),
          L"moving the volume turns the sound back on");
    SetPreviewVolume(140);
    check(settings_.previewVolume == 100, L"the volume stops at 100 %");

    key(VK_SPACE);
    check(!playing_ && !audioPreview_.Active(), L"space pauses (sound stops)");
    Seek(40);
    key(VK_LEFT);
    key(VK_LEFT);
    key(VK_LEFT);
    pump(1.0, [&] { return shown_ == 37; });
    check(playhead_ == 37 && shown_ == 37, util::Format(L"arrow keys step back: playhead %zu, shown %zu", playhead_,
                                                        shown_));
    // 이미지로 저장 (#62): S saves the frame on screen, any frame and not only a keyframe, without asking.
    {
        auto saveImage = [&] {
            hasImage_ = false;
            key('S');
            pump(5.0, [&] { return hasImage_; });
            return hasImage_ ? lastImage_.path : std::wstring();
        };
        std::wstring stem = util::FileNameOf(path);
        stem.resize(stem.rfind(L'.'));
        const int64_t ms = clip_->Ts(37) / 10'000;
        stem = ImageDir() + L"\\" + stem +
               util::Format(L"_%dm%02ds%03d", (int)(ms / 60'000), (int)(ms / 1000 % 60), (int)(ms % 1000));
        for (const wchar_t* end : {L".png", L"_2.png", L"_3.png", L".jpg"}) DeleteFileW((stem + end).c_str());  // a run before
        settings_.imageFormat = image::kPng;
        std::vector<uint8_t> px;
        int iw = 0, ih = 0;
        std::wstring img = saveImage();
        const bool read = ReadImageFile(img, px, iw, ih);
        const int off = WorstColour(px, framePx_);
        check(img == stem + L".png" && read && iw == clip_->Width() && ih == clip_->Height() && off <= 2 && !hasResult_,
              util::Format(L"S saves the frame on screen: %s, %dx%d, off by %d", util::FileNameOf(img).c_str(), iw, ih, off));
        check(saveImage() == stem + L"_2.png", L"the same frame again gets a number");
        crop_ = {10, 20, 210, 120};
        img = saveImage();
        check(ReadImageFile(img, px, iw, ih) && iw == 200 && ih == 100, L"a crop that is set is in the image");
        ResetCrop();
        settings_.imageFormat = image::kJpg;
        img = saveImage();
        check(img == stem + L".jpg" && ReadImageFile(img, px, iw, ih) && iw == clip_->Width(),
              L"이미지 형식 JPG is what S writes then");
        settings_.imageFormat = image::kPng;
        check(hasImage_, L"its card stays in the panel");
    }
    key('I');
    Seek(150);
    key('O');
    check(inF_ == 37 && outF_ == 150, util::Format(L"I/O set the range: %zu..%zu", inF_, outF_));
    check(!hasResult_, L"changing the range hides the original's card");

    // MP4 at 고화질 with nothing changed is cut as it is (#22), from the keyframe before I.
    const wchar_t* why = CopyBlocker();
    const bool copies = cutSource_ && why && !*why;
    const int frames = copies ? (int)(150 - cutSource_->KeyBefore(37) + 1) : 114;
    StartExport(false);
    check(!hasImage_, L"saving the clip takes the image's card away");
    pump(30, [&] { return !exporter_.Busy() && hasResult_; });
    check(hasResult_ && !resultOriginal_ && lastResult_.ok && lastResult_.frames == frames && lastResult_.audio,
          util::Format(L"export%s: %s, %d frames, %.3fs, audio %d, %s", copies ? L" as it is" : L"",
                       util::FileNameOf(lastResult_.path).c_str(), lastResult_.frames, lastResult_.seconds,
                       lastResult_.audio, util::FormatBytes(lastResult_.bytes).c_str()));
    std::wstring err;
    auto out = VideoClip::Open(lastResult_.path, err);
    check(out && (int)out->Count() == frames && out->HasAudio(),
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
    // 삭제 in the editor (#37), on a copy: the video just recorded goes with its home card (the test deletes it
    // outright instead of filling the Recycle Bin); a video opened by hand gets no button.
    std::wstring copy = dir + L"\\delete_test.mp4";
    if (CopyFileW(path.c_str(), copy.c_str(), FALSE)) {
        r.path = copy;
        r.bytes = util::FileSize(copy);
        OnVideoSaved(r);
        EditLastVideo();
        pump(0.5);
        check(screen_ == Screen::Editor && CanDiscard(), L"the video just recorded can be deleted in the editor");
        testDeleteFiles_ = true;
        // Delete asks inside the window; the editor under the question takes no keys; Esc says no, Enter yes.
        key(VK_DELETE);
        pump(0.3);
        bool asked = question_.has_value() && question_->danger;
        key(VK_SPACE);
        pump(0.3);
        bool still = !playing_ && question_.has_value();
        key(VK_ESCAPE);
        pump(0.3);
        check(asked && still && !question_ && screen_ == Screen::Editor && GetFileAttributesW(copy.c_str()) != INVALID_FILE_ATTRIBUTES,
              util::Format(L"Delete asks in the window (asked %d, the editor ignored Space %d); Esc keeps the video",
                           asked, still));
        key(VK_DELETE);
        pump(0.3);
        key(VK_RETURN);
        pump(0.5);
        check(screen_ == Screen::Editor && !clip_ && !showVideo_ && !question_ &&
                  GetFileAttributesW(copy.c_str()) == INVALID_FILE_ATTRIBUTES && util::FileSize(path) > 0,
              L"Enter on the question: 삭제 removes the video and its card; the 편집 tab has nothing open");
        check(OpenVideo(path) && !CanDiscard(), L"a video opened by hand has no 삭제");
        CloseEditor();
        // 삭제할 때 › 바로 삭제: it says so; a file another program holds stays, open as it was, and goes once let go.
        testDeleteFiles_ = false;
        settings_.deleteMode = kDeleteNow;
        check(CopyFileW(path.c_str(), copy.c_str(), FALSE) != 0, L"copy for the 바로 삭제 check");
        r.path = copy;
        r.bytes = util::FileSize(copy);
        OnVideoSaved(r);
        EditLastVideo();
        pump(0.5);
        key(VK_DELETE);
        pump(0.3);
        const bool says = question_.has_value() && question_->danger && question_->yes == tr(L"삭제") &&
                          question_->title == tr(L"이 동영상을 삭제할까요?");
        HANDLE other = CreateFileW(copy.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        key(VK_RETURN);
        pump(0.5);
        const bool stayed = other != INVALID_HANDLE_VALUE && clip_ && toastError_ && CanDiscard() &&
                            GetFileAttributesW(copy.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (other != INVALID_HANDLE_VALUE) CloseHandle(other);
        check(says && stayed, util::Format(L"바로 삭제 asks and says it can't be undone (%d); a held file stays (%d)", says, stayed));
        key(VK_DELETE);
        pump(0.3);
        key(VK_RETURN);
        pump(0.5);
        check(!clip_ && !showVideo_ && !question_ && GetFileAttributesW(copy.c_str()) == INVALID_FILE_ATTRIBUTES,
              L"바로 삭제 removes the file itself");
        settings_.deleteMode = kDeleteToBin;
    } else {
        check(false, L"copy for the 삭제 check");
    }
    exporter_.Cancel();
    quitting_ = true;
    DestroyWindow(hwnd_);
    pump(2.0);
    log(failures ? util::Format(L"RESULT: %d failure(s)", failures) : L"RESULT: all passed");
    return failures ? 2 : 0;
}

// 여러 창 녹화 (#48) in the app, through two test windows in processes of their own (no other window is listed):
// both as 움짤, held with the first one open; then as MP4 while the first window's process ends partway, which ends
// only its file, the pill going on to the other; the card lists both and each row's 편집 opens its own.
int App::RunMultiTest(const std::wstring& dir) {
    testWindowsOnly_ = true;
    util::EnsureDir(dir);
    Log log(dir + L"\\multi-test.log");
    int failures = 0;
    auto check = [&](bool ok, const std::wstring& what) {
        if (!ok) ++failures;
        log((ok ? L"ok   " : L"FAIL ") + what);
    };
    settings_.outDir = dir;
    settings_.sounds = false;
    settings_.mic = false;  // never the real microphone: it records the room
    settings_.audio = true;
    settings_.rpOn = false;
    settings_.source = kSourceWindow;
    settings_.multiWindow = true;
    settings_.nameTemplate = kNameTemplates[0];
    settings_.sizeIdx = (int)std::size(kSizeOptions) - 1;  // recorded small: a screenshot isn't
    if (!CreateMainWindow(SW_HIDE)) return 1;
    auto pump = [&](double seconds, const std::function<bool()>& until) {
        const double end = util::NowSeconds() + seconds;
        MSG msg;
        while (util::NowSeconds() < end && !(until && until())) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    };
    // The first window's process ends while the MP4s are recorded.
    const int lives[2] = {9, 30};
    PROCESS_INFORMATION pis[2]{};
    for (int i = 0; i < 2; ++i) {
        std::wstring cmd = L"\"" + util::ExePath() + util::Format(L"\" --test-window %d capture", lives[i]);
        STARTUPINFOW si{sizeof(si)};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pis[i])) {
            log(L"FAIL can't start --test-window");
            return 1;
        }
    }
    auto cleanup = [&] {
        for (PROCESS_INFORMATION& pi : pis) {
            if (!pi.hProcess) continue;
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 0);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            pi = {};
        }
    };
    HWND wins[2] = {};
    double nextList = 0;
    pump(8, [&] {
        if (util::NowSeconds() < nextList) return false;
        nextList = util::NowSeconds() + 0.2;
        RefreshWindows();
        for (const WinInfo& w : windows_) {
            DWORD pid = 0;
            GetWindowThreadProcessId(w.hwnd, &pid);
            for (int i = 0; i < 2; ++i)
                if (pid == pis[i].dwProcessId) wins[i] = w.hwnd;
        }
        return wins[0] && wins[1];
    });
    check(wins[0] && wins[1], L"multi: both test windows are listed");
    if (!wins[0] || !wins[1]) {
        cleanup();
        return 1;
    }
    for (HWND w : wins) SetWindowPos(w, nullptr, 0, 0, 800, 500, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    TogglePick(wins[0]);
    TogglePick(wins[1]);
    check(MultiPicking() && picks_.size() == 2 && target_ == wins[0], L"multi: both checked, the first one picked");

    // 움짤: a clip of each, held; the first window's opens.
    settings_.output = kOutputClip;
    const size_t heldBefore = held_.size();
    StartRecording(PickedTargets(), true);
    check(rec_ == RecState::Recording && recs_.size() == 2 && !recScreen_,
          L"multi clip: two windows recording, each through its window");
    pump(1.0, nullptr);
    // 스크린샷 (#63): one for each window, the same name getting a number, at the size of the window and not of the
    // recording; also while paused.
    auto images = [&] {
        size_t n = 0;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((ImageDir() + L"\\*.png").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return n;
        do {
            ++n;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return n;
    };
    auto press = [&] {
        PostMessageW(hwnd_, WM_HOTKEY, kHotkeyShot, 0);
        pump(0.1, nullptr);
        pump(5, [&] { return !shotsWaiting_; });
    };
    const size_t before = images();  // a run before may have left some
    press();
    {
        std::vector<uint8_t> px;
        int iw = 0, ih = 0;
        const bool read = ReadImageFile(lastShot_, px, iw, ih);
        const int out = recs_[0].capture->OutWidth();
        check(images() == before + 2 && recShots_ == 2 && read && iw > out && rpNotice_.Visible(),
              util::Format(L"shot: one for each window, %d wide from a recording %d wide, with the notice", iw, out));
        press();
        check(images() == before + 4, L"shot: pressed again right away, nothing is overwritten");
    }
    TogglePause();
    pump(0.4, nullptr);
    press();
    check(images() == before + 6 && recShots_ == 6, L"shot: it also takes while paused");
    TogglePause();
    pump(0.6, nullptr);
    StopRecording();
    check(recShots_ == 0 && lastShots_ == 0, L"shot: a clip has no card: the message says how many");
    const bool two = held_.size() == heldBefore + 2;
    const size_t f0 = two ? held_[0].clip->Count() : 0, f1 = two ? held_[1].clip->Count() : 0;
    check(two && f0 > 10 && f1 > 10 && clip_ && clip_ == held_[0].clip && screen_ == Screen::Editor,
          util::Format(L"multi clip: both held (%zu and %zu frames), the first one open in the editor", f0, f1));
    CloseClip();
    held_.clear();
    CloseEditor();

    // MP4: a file each; the first window closes partway.
    settings_.output = kOutputVideo;
    StartRecording(PickedTargets(), true);
    check(rec_ == RecState::Recording && recs_.size() == 2, L"multi mp4: two windows recording");
    pump(1.0, nullptr);
    press();
    pump(15, [&] { return rec_ != RecState::Recording || recs_[0].ended; });
    const bool ended = rec_ == RecState::Recording && recs_.size() == 2 && recs_[0].ended && !recs_[1].ended &&
                       RecLead() == &recs_[1] && recTarget_.hwnd == wins[1];
    check(ended, L"multi mp4: the first window closed: only its file ended, the pill went on to the other");
    press();
    check(recShots_ == 3, L"shot: only the window still recorded gives one");
    pump(2.5, nullptr);
    StopRecording();
    check(lastShots_ == 3, L"shot: the video's result counts them");
    const bool freed = RegisterHotKey(hwnd_, 99, settings_.hkShotMods, settings_.hkShotVk) != FALSE;
    if (freed) UnregisterHotKey(hwnd_, 99);
    check(freed, L"shot: the key is free for other programs once nothing is captured");
    check(lastVideos_.size() == 2 && showVideo_ && lastVideos_[0].early && !lastVideos_[1].early,
          util::Format(L"multi mp4: the card lists %zu files, the first one closed early", lastVideos_.size()));
    if (lastVideos_.size() == 2) {
        double secs[2] = {};
        bool sound[2] = {};
        for (int i = 0; i < 2; ++i) {
            std::wstring err;
            if (auto v = VideoClip::Open(lastVideos_[i].r.path, err)) {
                secs[i] = v->Seconds();
                sound[i] = v->HasAudio();
            }
        }
        const std::wstring n0 = util::FileNameOf(lastVideos_[0].r.path), n1 = util::FileNameOf(lastVideos_[1].r.path);
        check(secs[0] > 1.5 && secs[1] > secs[0] + 1.5,
              util::Format(L"multi mp4: %.2f s and %.2f s (sound %d %d)", secs[0], secs[1], sound[0], sound[1]));
        check(n0 != n1 && n0.find(L"_OzMaker Test Window") != std::wstring::npos &&
                  n1.find(L"_OzMaker Test Window") != std::wstring::npos,
              L"multi mp4: each named after its window: " + n0 + L", " + n1);
        EditVideoAt(1);
        check(screen_ == Screen::Editor && clip_ && clip_->SourcePath() == lastVideos_[1].r.path &&
                  lastVideo_.path == lastVideos_[1].r.path,
              L"multi mp4: the second row's 편집 opens the second file");
        CloseEditor();
    }
    cleanup();
    lastVideos_.clear();
    lastVideo_ = {};
    picks_.clear();
    quitting_ = true;
    DestroyWindow(hwnd_);
    pump(1.0, nullptr);
    log(failures ? util::Format(L"RESULT: %d failure(s)", failures) : L"RESULT: all passed");
    return failures ? 2 : 0;
}

// What keeps a long recording safe (#69), on a test window. Lock and sleep are the app's own messages sent to it: the
// PC is neither locked nor put to sleep, and whether frames come through them is for the user to try by hand.
int App::RunSafetyTest(const std::wstring& dir) {
    testWindowsOnly_ = true;
    util::EnsureDir(dir);
    Log log(dir + L"\\safety-test.log");
    int failures = 0;
    auto check = [&](bool ok, const std::wstring& what) {
        if (!ok) ++failures;
        log((ok ? L"ok   " : L"FAIL ") + what);
    };
    settings_.outDir = dir;
    settings_.sounds = false;
    settings_.mic = false;  // never the real microphone: it records the room
    settings_.audio = false;
    settings_.rpOn = false;
    settings_.source = kSourceWindow;
    settings_.multiWindow = false;
    settings_.nameTemplate = kNameTemplates[0];
    settings_.sizeIdx = (int)std::size(kSizeOptions) - 1;
    if (!CreateMainWindow(SW_HIDE)) return 1;
    auto pump = [&](double seconds, const std::function<bool()>& until) {
        const double end = util::NowSeconds() + seconds;
        MSG msg;
        while (util::NowSeconds() < end && !(until && until())) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    };
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + util::ExePath() + L"\" --test-window 120 capture";
    STARTUPINFOW si{sizeof(si)};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        log(L"FAIL can't start --test-window");
        return 1;
    }
    HWND win = nullptr;
    double nextList = 0;
    pump(8, [&] {
        if (util::NowSeconds() < nextList) return false;
        nextList = util::NowSeconds() + 0.2;
        RefreshWindows();
        for (const WinInfo& w : windows_) {
            DWORD pid = 0;
            GetWindowThreadProcessId(w.hwnd, &pid);
            if (pid == pi.dwProcessId) win = w.hwnd;
        }
        return win != nullptr;
    });
    auto cleanup = [&] {
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    };
    check(win != nullptr, L"the test window is listed");
    if (!win) {
        cleanup();
        return 1;
    }
    SetWindowPos(win, nullptr, 0, 0, 800, 500, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    const auto awake = [] {
        const EXECUTION_STATE both = ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED;
        return (SetThreadExecutionState(0) & both) == both;
    };
    auto session = [&](WPARAM what) { SendMessageW(hwnd_, WM_WTSSESSION_CHANGE, what, 0); };
    auto power = [&](WPARAM what) { SendMessageW(hwnd_, WM_POWERBROADCAST, what, 0); };

    // Staying awake: while recording, not while paused, and let go of at the end.
    settings_.output = kOutputVideo;
    check(!awake(), L"awake: nothing asked for before recording");
    StartRecording(WindowTarget(win), true);
    check(rec_ == RecState::Recording && awake(), L"awake: asked for while recording");
    pump(0.6, nullptr);
    TogglePause();
    check(!awake(), L"awake: let go of while paused");
    session(WTS_SESSION_LOCK);  // a pause holds nothing: no gap
    pump(1.1, nullptr);
    session(WTS_SESSION_UNLOCK);
    TogglePause();
    check(awake(), L"awake: asked for again when it goes on");

    // The gaps: the lock, sleep, sleep inside a lock (one stretch), and a lock still on when it stops.
    session(WTS_SESSION_LOCK);
    pump(2.1, nullptr);
    session(WTS_SESSION_UNLOCK);
    pump(0.3, nullptr);
    power(PBT_APMSUSPEND);
    pump(1.1, nullptr);
    power(PBT_APMRESUMEAUTOMATIC);
    pump(0.3, nullptr);
    session(WTS_SESSION_LOCK);
    pump(0.6, nullptr);
    power(PBT_APMSUSPEND);
    pump(0.6, nullptr);
    power(PBT_APMRESUMESUSPEND);
    pump(0.6, nullptr);
    session(WTS_SESSION_UNLOCK);
    pump(0.3, nullptr);
    session(WTS_SESSION_LOCK);
    pump(1.1, nullptr);
    StopRecording();
    session(WTS_SESSION_UNLOCK);
    check(!awake(), L"awake: let go of when it stops");
    {
        const std::vector<CaptureGap>& g = lastVideo_.gaps;
        using Why = CaptureGap::Why;
        const bool shape = g.size() == 4 && g[0].why == Why::Locked && g[1].why == Why::Asleep &&
                           g[2].why == Why::Asleep && g[3].why == Why::Locked;
        std::wstring lengths;
        for (const CaptureGap& x : g) lengths += util::Format(L" %.0f", x.seconds);
        check(shape && lastVideo_.ok && showVideo_, util::Format(L"gaps: %zu on the result:", g.size()) + lengths);
        check(shape && g[0].seconds >= 1 && g[0].seconds <= 4 && g[2].seconds >= 1 && g[2].seconds <= 4 &&
                  g[0].at < g[1].at && g[1].at <= g[2].at,
              L"gaps: in order, as long as the stretches were");
    }

    // 움짤: staying awake too, and no card for the gaps: the message says it.
    settings_.output = kOutputClip;
    StartRecording(WindowTarget(win), true);
    check(rec_ == RecState::Recording && awake(), L"clip: awake while recording");
    pump(0.4, nullptr);
    session(WTS_SESSION_LOCK);
    pump(1.1, nullptr);
    session(WTS_SESSION_UNLOCK);
    pump(0.4, nullptr);
    StopRecording();
    check(!awake() && clip_ && toast_.find(L"구간이 1개") != std::wstring::npos, L"clip: the message tells of the gap: " + toast_);
    CloseClip();
    held_.clear();
    CloseEditor();

    // The encoder stops taking frames: not at once, then the recording ends by itself and keeps what it has. Each
    // frame rate way; and once with a file the encoder can't finish, as after a lost device.
    settings_.output = kOutputVideo;
    auto stopped = [&](const wchar_t* name, bool vfr, bool finishFails) {
        const int was = settings_.vFrameMode;
        settings_.vFrameMode = vfr ? 1 : 0;
        StartRecording(WindowTarget(win), true);
        pump(1.5, nullptr);
        const double from = util::NowSeconds();
        if (!recs_.empty() && recs_[0].capture) recs_[0].capture->RefuseFrames(finishFails);
        pump(1, nullptr);
        check(rec_ == RecState::Recording, std::wstring(name) + L": goes on for a moment");
        pump(12, [&] { return rec_ != RecState::Recording; });
        const double took = util::NowSeconds() - from;
        settings_.vFrameMode = was;
        check(rec_ == RecState::Idle && !awake(), util::Format(L"%s: ended by itself after %.1f s", name, took));
        check(took >= 4.5 && took < 7, std::wstring(name) + L": after about five seconds");
        std::wstring err;
        auto v = VideoClip::Open(lastVideo_.path, err);
        const double kept = v ? v->Seconds() : 0;
        const size_t frames = v ? v->Count() : 0;
        v.reset();
        check(showVideo_ && lastVideo_.ok && !lastVideo_.failure.empty() && lastVideo_.bytes > 0,
              std::wstring(name) + L": the card says so: " + lastVideo_.failure);
        check(kept > 0.5 && kept < 3 && frames >= 2,  // the test window changes every 1.5 s: VFR writes few
              util::Format(L"%s: the file plays: %.2f s, %zu frames, card says %.2f s", name, kept, frames, lastVideo_.seconds));
    };
    stopped(L"encoder gone", false, false);
    stopped(L"encoder gone, variable frame rate", true, false);
    stopped(L"encoder gone, file not finished", false, true);

    cleanup();
    lastVideo_ = {};
    quitting_ = true;
    DestroyWindow(hwnd_);
    pump(1.0, nullptr);
    log(failures ? util::Format(L"RESULT: %d failure(s)", failures) : L"RESULT: all passed");
    return failures ? 2 : 0;
}

// The live window preview as DWM shows it. A window of colour blocks is picked and the files list opened over the
// preview: on screen (our window only, kept on top) the list must show where it is and the window everywhere else,
// with the title bar hidden and shown, and closing the list must bring back the whole preview.
int App::RunTestWindow(double seconds, bool capture, const RECT* cover) {
    if (cover) {
        // Another program's window over the one --replay-test takes through the screen, the way a browser is over
        // a game after Alt+Tab: white, topmost, never in front.
        WNDCLASSEXW cc{sizeof(cc)};
        cc.lpfnWndProc = DefWindowProcW;
        cc.hInstance = inst_;
        cc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
        cc.lpszClassName = L"OzMakerTestCover";
        RegisterClassExW(&cc);
        HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, cc.lpszClassName, L"", WS_POPUP,
                                 cover->left, cover->top, cover->right - cover->left, cover->bottom - cover->top, nullptr,
                                 nullptr, inst_, nullptr);
        if (!h) return 1;
        ShowWindow(h, SW_SHOWNOACTIVATE);
        const double end = util::NowSeconds() + seconds;
        while (util::NowSeconds() < end) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
            MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, 0);
        }
        DestroyWindow(h);
        return 0;
    }
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = PatternProc;
    wc.hInstance = inst_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"OzMakerTestPattern";
    RegisterClassExW(&wc);
    // Windows won't capture a window that can't be activated; it still opens without taking the focus.
    HWND h = CreateWindowExW(capture ? 0 : WS_EX_NOACTIVATE, wc.lpszClassName, L"OzMaker Test Window 2",
                             WS_OVERLAPPEDWINDOW, 60, 60, 420, 260, nullptr, nullptr, inst_, nullptr);
    if (!h) return 1;
    ShowWindow(h, SW_SHOWNOACTIVATE);
    g_patternPlain = capture;
    if (capture) SetTimer(h, 1, 1500, nullptr);
    double end = util::NowSeconds() + seconds;
    while (util::NowSeconds() < end) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, 0);
    }
    DestroyWindow(h);
    return 0;
}

int App::RunUpdateTest(const std::wstring& dir, const std::wstring& base) {
    util::EnsureDir(dir);
    Log log(dir + L"\\updatetest.log");
    if (base == L"github") {
        // The real release: one check, reported (what it finds depends on the latest release).
        Updater u;
        u.Init(dir + L"\\OzMaker.exe", nullptr, 0);
        u.Check();
        for (int i = 0; i < 3000 && u.Busy(); ++i) Sleep(20);
        Updater::Status st = u.Get();
        log(util::Format(L"github: state %d, version %s, error %s", (int)st.state, st.version.c_str(), st.error.c_str()));
        return 0;
    }
    int failures = UpdaterChecks(dir, log, base);
    log(failures ? util::Format(L"RESULT: %d failure(s)", failures) : L"RESULT: all passed");
    return failures ? 1 : 0;
}

int App::RunPreviewTest(const std::wstring& dir) {
    testWindowsOnly_ = true;
    util::EnsureDir(dir);
    Log log(dir + L"\\preview.log");
    int failures = 0;
    auto check = [&](bool ok, const std::wstring& what) {
        if (!ok) ++failures;
        log((ok ? L"ok   " : L"FAIL ") + what);
    };
    settings_.outDir = dir;
    if (!CreateMainWindow(SW_SHOW)) return 1;
    bool quit = false;
    auto pump = [&](double seconds) {
        double end = util::NowSeconds() + seconds;
        while (!quit && util::NowSeconds() < end) {
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
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = PatternProc;
    wc.hInstance = inst_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"OzMakerTestPattern";
    RegisterClassExW(&wc);
    RECT wr, fr{0, 0, 800, 450};
    GetWindowRect(hwnd_, &wr);
    AdjustWindowRectEx(&fr, WS_OVERLAPPEDWINDOW, FALSE, 0);
    HWND pattern = CreateWindowExW(0, wc.lpszClassName, L"OzMaker Test Window", WS_OVERLAPPEDWINDOW, wr.left + 40,
                                   wr.top + 40, fr.right - fr.left, fr.bottom - fr.top, nullptr, nullptr, inst_, nullptr);
    ShowWindow(pattern, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);  // the screen there is ours
    target_ = pattern;
    screen_ = Screen::Home;
    settings_.source = kSourceWindow;
    showVideo_ = false;
    recentFiles_ = {{dir + L"\\OzMaker_20261102_142311.gif", 2'345'678, 300, false}};
    RECT client;
    GetClientRect(hwnd_, &client);
    POINT o{0, 0};
    ClientToScreen(hwnd_, &o);
    const RECT area{o.x, o.y, o.x + client.right, o.y + client.bottom};
    const LONG cw = client.right;
    auto at = [&](const std::vector<uint32_t>& px, LONG x, LONG y) { return px[(size_t)y * cw + x]; };
    auto same = [](uint32_t a, uint32_t b) {
        for (int sh = 0; sh < 24; sh += 8)
            if (std::abs((int)(a >> sh & 255) - (int)(b >> sh & 255)) > 24) return false;
        return true;
    };
    auto isPattern = [](uint32_t c) {
        for (COLORREF k : kPatternColors)
            if (NearColor(c, k, 40)) return true;
        return false;
    };
    auto save = [&](std::vector<uint32_t>& px, const wchar_t* name) {
        SavePng(gfx_.Wic(), dir + L"\\" + name, reinterpret_cast<const uint8_t*>(px.data()), cw, client.bottom);
    };
    pump(0.5);
    for (bool clientOnly : {true, false}) {
        const std::wstring mode = clientOnly ? L"title bar hidden" : L"whole window";
        settings_.clientOnly = clientOnly;
        filesOpen_ = false;
        Invalidate();
        pump(0.8);
        auto base = GrabScreen(area);
        if (base.empty()) {
            check(false, mode + L": another program's window is over the test window, nothing was grabbed");
            filesOpen_ = false;
            Invalidate();
            pump(0.3);
            break;
        }
        RECT dest, src;
        PreviewLayout(dest, src);
        const LONG dw = dest.right - dest.left, dh = dest.bottom - dest.top;
        {
            SIZE whole{};
            if (!thumb_.thumb.empty()) DwmQueryThumbnailSourceSize(thumb_.thumb[0], &whole);
            RECT pw, pf{}, pc;
            GetWindowRect(pattern, &pw);
            DwmGetWindowAttribute(pattern, DWMWA_EXTENDED_FRAME_BOUNDS, &pf, sizeof(pf));
            GetClientRect(pattern, &pc);
            log(util::Format(L"     %s: DWM size %dx%d, window %dx%d, frame %dx%d, client %dx%d -> source %d,%d-%d,%d at "
                             L"%dx%d", mode.c_str(), whole.cx, whole.cy, pw.right - pw.left, pw.bottom - pw.top,
                             pf.right - pf.left, pf.bottom - pf.top, pc.right, pc.bottom, src.left, src.top, src.right,
                             src.bottom, dw, dh));
        }
        int shown = 0;
        for (int gy = 1; gy < 10; ++gy)
            for (int gx = 1; gx < 10; ++gx) shown += isPattern(at(base, dest.left + dw * gx / 10, dest.top + dh * gy / 10));
        check(dw > 100 && shown >= 81 * 8 / 10, util::Format(L"%s: the preview shows the window (%d of 81 points)", mode.c_str(), shown));
        if (clientOnly) {
            int found = 0;
            for (int i = 0; i < 8; ++i)
                found += NearColor(at(base, dest.left + dw * (2 * (i % 4) + 1) / 8, dest.top + dh * (2 * (i / 4) + 1) / 4),
                                   kPatternColors[i], 40);
            check(found == 8, util::Format(L"%s: each block in its place (%d of 8)", mode.c_str(), found));
        }

        filesOpen_ = true;
        Invalidate();
        pump(0.8);
        auto pop = GrabScreen(area);
        if (pop.empty()) {
            check(false, mode + L": another program's window is over the test window, nothing was grabbed");
            filesOpen_ = false;
            Invalidate();
            pump(0.3);
            break;
        }
        Rect f = ui_.Floats().empty() ? Rect() : ui_.Floats()[0];  // the list's
        const float s = Scale();
        RECT hole{(LONG)std::floor(f.l * s), (LONG)std::floor(f.t * s), (LONG)std::ceil(f.r * s), (LONG)std::ceil(f.b * s)};
        RECT inner = hole, outer = hole;
        InflateRect(&inner, -3, -3);
        InflateRect(&outer, 3, 3);
        {
            std::wstring ps;
            for (const RECT& p : PreviewParts(dest)) ps += util::Format(L" %d,%d-%d,%d", p.left, p.top, p.right, p.bottom);
            log(util::Format(L"     list %d,%d-%d,%d over %d,%d-%d,%d; parts%s", hole.left, hole.top, hole.right,
                             hole.bottom, dest.left, dest.top, dest.right, dest.bottom, ps.c_str()));
        }
        int under = 0, underN = 0, kept = 0, keptN = 0;
        for (LONG y = dest.top + 1; y < dest.bottom - 1; y += 3)
            for (LONG x = dest.left + 1; x < dest.right - 1; x += 3) {
                POINT pt{x, y};
                if (PtInRect(&inner, pt)) {
                    ++underN;
                    under += isPattern(at(pop, x, y));
                } else if (!PtInRect(&outer, pt)) {
                    ++keptN;
                    kept += same(at(pop, x, y), at(base, x, y));
                }
            }
        // (Text on the list has ClearType's coloured fringes; the preview showing through would be most of it.)
        check(underN > 100 && under <= underN / 50,
              util::Format(L"%s: the files list shows above the preview (%d of %d points under it show the window, %zu "
                           L"thumbnails)", mode.c_str(), under, underN, thumb_.thumb.size()));
        check(keptN > 100 && kept >= keptN * 99 / 100,
              util::Format(L"%s: the rest of the preview is as before (%d of %d points)", mode.c_str(), kept, keptN));
        save(base, clientOnly ? L"preview_client.png" : L"preview_window.png");
        save(pop, clientOnly ? L"preview_client_list.png" : L"preview_window_list.png");

        filesOpen_ = false;
        Invalidate();
        pump(0.8);
        auto after = GrabScreen(area);
        if (after.empty()) {
            check(false, mode + L": another program's window is over the test window, nothing was grabbed");
            filesOpen_ = false;
            Invalidate();
            pump(0.3);
            break;
        }
        int back = 0, backN = 0;
        for (LONG y = dest.top + 1; y < dest.bottom - 1; y += 3)
            for (LONG x = dest.left + 1; x < dest.right - 1; x += 3) {
                ++backN;
                back += same(at(after, x, y), at(base, x, y));
            }
        check(back >= backN * 99 / 100 && thumb_.thumb.size() == 1 && !thumb_.split,
              util::Format(L"%s: closing the list brings the whole preview back (%d of %d points)", mode.c_str(), back,
                           backN));
    }
    recentFiles_.clear();
    target_ = nullptr;
    DestroyWindow(pattern);

    // The window list follows other programs' windows as they come and go, without polling. The hooks skip our
    // own process, so the window comes from a second OzMaker.
    {
        auto listed = [&] {
            for (const WinInfo& w : windows_)
                if (w.title == L"OzMaker Test Window 2") return true;
            return false;
        };
        std::wstring cmd = L"\"" + util::ExePath() + L"\" --test-window 2.5";
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            double t0 = util::NowSeconds(), seen = -1, gone = -1;
            while (util::NowSeconds() - t0 < 8 && gone < 0) {
                pump(0.05);
                bool in = listed();
                if (in && seen < 0) seen = util::NowSeconds() - t0;
                if (!in && seen >= 0) gone = util::NowSeconds() - t0;
            }
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            // It starts (a second or so) and stays 2.5 s: listed well before a 2.5 s poll would, gone after.
            check(seen >= 0 && seen < 2.5 && gone > seen + 2,
                  util::Format(L"window list: another process's window listed at %.2fs, gone at %.2fs", seen, gone));
        } else {
            check(false, L"window list: can't start --test-window");
        }
    }
    quitting_ = true;
    DestroyWindow(hwnd_);
    pump(1.0);
    log(failures ? util::Format(L"RESULT: %d failure(s)", failures) : L"RESULT: all passed");
    return failures ? 2 : 0;
}
