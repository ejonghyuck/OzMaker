// Headless test entry points (command line):
//   --selftest <dir>                      synthetic clip -> frame store round trip -> every available encoder
//   --screenshot <dir>                    renders each screen to PNG for visual review
//   --record-test <title> <sec> <dir> [fps] [maxEdge] [window|display|monitor|region] [clip|mp4] [audio|mute] [pause] [mic] [click]
//                                         records a real window (or its monitor / its area) and exports it
//   --analyze <file.mp4> <dir> [freq ...] decodes an MP4: timing, size, test-tone levels
//   --editor-test <file.mp4> <dir>        the real window: saved video -> editor -> keys -> trimmed export -> 삭제
//   --video-test <dir> [w h]              synthetic MP4 -> frame index/decoding -> trimmed MP4/GIF exports, checked
//                                         frame by frame and by the audio tone boundaries; editor screenshots
//   --region-test <dir>                   the region picker's adjust mode, driven with synthetic input
//   --preview-test <dir>                  the live window preview on screen, with the files list open over it;
//                                         the window list following another process's window
//   --test-window <sec>                   a pattern window in a process of its own (for --preview-test)
//   --update-test <dir> [url|github]      the updater on a fake release folder (also part of --selftest); url: the
//                                         same served over HTTP; github: one check against the real releases
#include "pch.h"
#include "App.h"
#include "Crash.h"
#include "Mp4Unfragment.h"
#include "Resize.h"
#include "Util.h"
#include "Version.h"
#include "VideoClip.h"

namespace {

// Further down, with the other test helpers.
double ToneLevel(const std::vector<int16_t>& stereo, size_t from, size_t n, double f, int rate);
bool WriteTestMp4(const std::wstring& path, int W, int H, int frames, int fps, int gop, std::wstring& err,
                  bool fragmented = false, bool crash = false);
int ReadIndex(const uint8_t* p, int w, int h);

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

// Eight blocks of saturated colour (nothing in the UI is that colour): a preview of it shows where each part of the
// window went.
const COLORREF kPatternColors[8] = {RGB(230, 40, 40),  RGB(40, 200, 60),  RGB(40, 90, 230),  RGB(240, 220, 40),
                                    RGB(220, 50, 220), RGB(40, 220, 220), RGB(250, 140, 30), RGB(150, 90, 30)};

LRESULT CALLBACK PatternProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        for (int i = 0; i < 8; ++i) {
            RECT c{rc.right * (i % 4) / 4, rc.bottom * (i / 4) / 2, rc.right * (i % 4 + 1) / 4, rc.bottom * (i / 4 + 1) / 2};
            HBRUSH b = CreateSolidBrush(kPatternColors[i]);
            FillRect(dc, &c, b);
            DeleteObject(b);
        }
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// Pixels on screen (BGRX), for checking what DWM composited. Only ever our own window's area.
std::vector<uint32_t> GrabScreen(const RECT& r) {
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
        if (fmt == Format::Gif) job.lossy = settings_.gifLossy;
        if (fmt == Format::Mp4) job.fpsLimit = fps;
        job.path = dir + L"\\selftest." + FormatExt(fmt);
        double s = util::NowSeconds();
        ExportResult r = ExportSync(ex, job);
        log(util::Format(L"%s: %s %s, %d frames, %.2fs, %.0f ms %s", FormatName(fmt), r.ok ? L"ok" : L"FAIL",
                         util::FormatBytes(r.bytes).c_str(), r.frames, r.seconds, (util::NowSeconds() - s) * 1000,
                         r.error.c_str()));
        // The size guess against the real thing. MP4 is VBR (this test picture needs far less than the bitrate),
        // so only 움짤 must be close.
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
        // the preview included. Only a video drops, and it opens once the drop has returned (WM_APP_OPEN).
        {
            std::wstring mp4 = dir + L"\\끌어오기.mp4", gif = dir + L"\\끌어오기.gif";
            for (const std::wstring& f : {mp4, gif}) {
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
            auto gifData = dataFor(gif), mp4Data = dataFor(mp4);
            auto target = MakeDropTarget();  // what CreateMainWindow registers (this test has no main window)
            bool ok = gifData && mp4Data;
            if (ok) ok = SUCCEEDED(target->DragEnter(gifData.Get(), MK_LBUTTON, {0, 0}, &effect)) &&
                         dragKind_ == DragKind::Anim && effect == DROPEFFECT_NONE;
            frames(3);
            const bool gifShown = ui_.LayerErrors() == 0 &&
                                  std::any_of(ui_.Floats().begin(), ui_.Floats().end(),
                                              [](const Rect& f) { return f.W() >= 1120 && f.H() >= 780; });
            target->DragLeave();
            const bool left = dragKind_ == DragKind::None;
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
            good = ok && gifShown && left && clean && uncovered == 0 && dropped;
            log(util::Format(L"drag over the window: %s GIF refused with a cue %d, cue gone on leave %d, video cue over "
                             L"the preview (%zu parts left) %d, the drop opens it afterwards %d",
                             good ? L"ok" : L"FAIL", gifShown, left, uncovered, clean, dropped));
            if (!good) ++failures;
            DeleteFileW(mp4.c_str());
            DeleteFileW(gif.c_str());
        }
        filesOpen_ = false;
        toastUntil_ = 0;
        recentFiles_.clear();
        target_ = nullptr;
        DestroyWindow(standIn);
    }
    failures += UpdaterChecks(dir + L"\\update", log);

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
    // 버리기 in the editor (#37): an unsaved 움짤 recording goes, and the 편집 tab is left with nothing open.
    {
        auto clip = MakeSyntheticClip(320, 180, 10, 30);
        size_t before = held_.size();
        HoldRecording(clip);
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
        HoldRecording(clip);
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
    // 화질 presets: a new settings file starts at 보통; one from before the presets keeps its values and shows the
    // preset they match; a preset gives MP4 and 움짤 their own values. Each case is a file in this test's folder,
    // read through OZMAKER_SETTINGS, which is then put back.
    {
        auto check = [&](bool ok, const wchar_t* what) {
            if (!ok) ++failures;
            log(util::Format(L"presets: %s %s", ok ? L"ok  " : L"FAIL", what));
        };
        wchar_t env[MAX_PATH] = {};
        GetEnvironmentVariableW(L"OZMAKER_SETTINGS", env, MAX_PATH);
        auto load = [&](const wchar_t* name, const char* text) {
            std::wstring ini = dir + L"\\" + name;
            HANDLE f = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
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
        bool video = st.scaleIdx == 4 && st.outFpsIdx == 1 && st.gifLossy == 2;
        st.ApplyPreset(2, false);
        check(video && st.scaleIdx == 5 && st.outFpsIdx == 4 && st.MatchPreset(false, true) == 2 &&
                  st.MatchPreset(true, false) == -1,
              L"저용량 is 640px/30fps for MP4 and 480px/15fps with strong lossy for 움짤");
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
    if (!thumbWanted_ || !target_ || !IsWindow(target_)) return;
    RECT dest, src;
    PreviewLayout(dest, src);
    ComPtr<ID2D1SolidColorBrush> fill, edge;
    dc->CreateSolidColorBrush(D2D1::ColorF(0x2F4A73), &fill);
    dc->CreateSolidColorBrush(D2D1::ColorF(0x6F94CC), &edge);
    const float s = Scale();
    for (const RECT& p : PreviewParts(dest)) {
        D2D1_RECT_F r{p.left / s, p.top / s, p.right / s, p.bottom / s};
        dc->FillRectangle(r, fill.Get());
        dc->DrawRectangle({r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f}, edge.Get(), 1.f);
    }
}

int App::RunScreenshots(const std::wstring& dir) {
    testWindowsOnly_ = true;
    util::EnsureDir(dir);
    Log log(dir + L"\\screenshots.log");
    int layerErrors = 0;
    const float W = 1120, H = 780;
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
                DrawFrame(Rect(0, 0, W, H));
                ui_.End();
                if (pass == 9 && ui_.LayerErrors()) {
                    const Rect& e = ui_.FirstLayerError();
                    log(util::Format(L"FAIL %s: %d layering error(s), first at %.0f,%.0f-%.0f,%.0f", name.c_str(),
                                     ui_.LayerErrors(), e.l, e.t, e.r, e.b));
                    ++layerErrors;
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
        // The recording pill: recording (stop hovered) and paused (resume hovered), at 150 %.
        gfx_.RenderToPng((dir + L"\\rec_pill.png").c_str(), 360, 150, 144, [&](ID2D1DeviceContext* dc) {
            Painter p;
            p.Bind(dc);
            p.Fill(Rect(0, 0, 240, 100), theme::Hex(0x2B4C7E));
            dc->SetTransform(D2D1::Matrix3x2F::Translation(6, 6));
            overlay_.PaintTest(p, 73.4, false, 2);
            dc->SetTransform(D2D1::Matrix3x2F::Translation(6, 54));
            overlay_.PaintTest(p, 73.4, true, 1);
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
        });
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
                                          L"settings_audio.png", L"settings_about.png"};
    for (int i = 0; i < kTabCount; ++i) {
        settings_.settingsTab = i;
        settingsScroll_ = 0;
        micTestShow_ = i == kTabAudio ? 0.6f : -1;  // the mic check running, on a made-up level
        shot(tabShots[i], 96);
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
    fmt_ = (int)Format::Gif;
    settings_.exportDetails = false;
    settings_.ApplyPreset(settings_.exportPreset = 1, false);
    settings_.targetIdx = 3;  // 15MB: the fit is under way in this shot
    shot(L"editor_target.png", 96);
    settings_.targetIdx = 0;
    fitting_ = false;
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
    // Questions inside the window instead of MessageBox.
    ConfirmDiscard();
    shot(L"editor_ask.png", 96);
    Ask({L"이 동영상을 휴지통으로 옮길까요?", L"OzMaker_20261102_142311.mp4\n이 녹화에서 따로 저장한 파일은 그대로 남습니다.",
         L"휴지통으로", true, nullptr});
    shot(L"editor_ask_video.png", 96);
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
    screen_ = Screen::Home;
    keep.reset();
    target_ = nullptr;
    if (standIn) DestroyWindow(standIn);
    log(layerErrors ? util::Format(L"RESULT: %d screen(s) with layering errors", layerErrors) : L"RESULT: all passed");
    return layerErrors ? 2 : 0;
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
    std::wstring err;
    double t0 = util::NowSeconds();
    if (!capture_.Start(o, nullptr, 0, err)) {
        log(L"FAIL: " + err);
        return 1;
    }
    log(util::Format(L"start took %.0f ms", (util::NowSeconds() - t0) * 1000));
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
    toast_ = L"지난번에 OzMaker가 비정상 종료되었습니다. 녹화하던 동영상을 되살렸습니다 · OzMaker_20261102_142311.mp4.";
    toastUntil_ = clock + 60;
    toastError_ = true;
    shot(L"editor_video_recovered.png");
    toastError_ = false;
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

    // The toolbar: its countdown menu, then "저장만" keeps the area without recording.
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

// The live window preview as DWM shows it. A window of colour blocks is picked and the files list opened over the
// preview: on screen (our window only, kept on top) the list must show where it is and the window everywhere else,
// with the title bar hidden and shown, and closing the list must bring back the whole preview.
int App::RunTestWindow(double seconds) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = PatternProc;
    wc.hInstance = inst_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"OzMakerTestPattern";
    RegisterClassExW(&wc);
    HWND h = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"OzMaker Test Window 2", WS_OVERLAPPEDWINDOW, 60, 60,
                             420, 260, nullptr, nullptr, inst_, nullptr);
    if (!h) return 1;
    ShowWindow(h, SW_SHOWNOACTIVATE);
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
        RECT dest, src;
        PreviewLayout(dest, src);
        const LONG dw = dest.right - dest.left, dh = dest.bottom - dest.top;
        {
            SIZE whole{};
            if (!thumb_.empty()) DwmQueryThumbnailSourceSize(thumb_[0], &whole);
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
                           L"thumbnails)", mode.c_str(), under, underN, thumb_.size()));
        check(keptN > 100 && kept >= keptN * 99 / 100,
              util::Format(L"%s: the rest of the preview is as before (%d of %d points)", mode.c_str(), kept, keptN));
        save(base, clientOnly ? L"preview_client.png" : L"preview_window.png");
        save(pop, clientOnly ? L"preview_client_list.png" : L"preview_window_list.png");

        filesOpen_ = false;
        Invalidate();
        pump(0.8);
        auto after = GrabScreen(area);
        int back = 0, backN = 0;
        for (LONG y = dest.top + 1; y < dest.bottom - 1; y += 3)
            for (LONG x = dest.left + 1; x < dest.right - 1; x += 3) {
                ++backN;
                back += same(at(after, x, y), at(base, x, y));
            }
        check(back >= backN * 99 / 100 && thumb_.size() == 1 && !thumbSplit_,
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
