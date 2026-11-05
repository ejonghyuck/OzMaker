#include "pch.h"
#include "App.h"
#include "Mp4Box.h"
#include "Sound.h"
#include "Util.h"

// 순간 리플레이 (#20): while it is on, the program picked in the 리플레이 tab that was last in front (or a whole
// monitor) is encoded into ReplayRing, which keeps the last span of it in memory. The save key writes that span to
// 저장 폴더 › 리플레이 › the program. Nothing reaches the disk before that, and the ring is let go when the PC locks or
// sleeps.

namespace {

bool SamePath(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

// The tray icon while replay holds something: the app's own with a small red dot at its lower right.
HICON MakeDotIcon(HICON base) {
    ICONINFO ii{};
    if (!base || !GetIconInfo(base, &ii)) return nullptr;
    BITMAP bm{};
    GetObjectW(ii.hbmColor, sizeof(bm), &bm);
    const int w = bm.bmWidth, h = bm.bmHeight;
    HICON icon = nullptr;
    if (ii.hbmColor && w > 0 && h > 0) {
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
        std::vector<uint32_t> px((size_t)w * h), mask((size_t)w * h);
        HDC dc = GetDC(nullptr);
        GetDIBits(dc, ii.hbmColor, 0, h, px.data(), &bi, DIB_RGB_COLORS);
        GetDIBits(dc, ii.hbmMask, 0, h, mask.data(), &bi, DIB_RGB_COLORS);
        // An icon without alpha says what shows through its mask (black: opaque).
        if (std::none_of(px.begin(), px.end(), [](uint32_t c) { return c >> 24; }))
            for (size_t i = 0; i < px.size(); ++i) px[i] |= (mask[i] & 0xFFFFFF) ? 0 : 0xFF000000u;
        // The dot, with a clear ring around it so it reads on any taskbar.
        const float cx = w * 0.74f, cy = h * 0.74f, r = w * 0.25f, ring = r + std::max(1.f, w * 0.07f);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float in = 0, cut = 0;
                for (int sy = 0; sy < 4; ++sy)
                    for (int sx = 0; sx < 4; ++sx) {
                        float fx = x + (sx + 0.5f) / 4 - cx, fy = y + (sy + 0.5f) / 4 - cy, d = std::sqrt(fx * fx + fy * fy);
                        in += d <= r;
                        cut += d <= ring;
                    }
                in /= 16;
                cut /= 16;
                uint32_t& c = px[(size_t)y * w + x];
                float a = (c >> 24) / 255.f * (1 - cut);  // what is left of the icon around the dot
                float A = in + a * (1 - in);
                auto mix = [&](int shift, float dot) {
                    float v = A > 0 ? (dot * in + ((c >> shift) & 0xFF) * a * (1 - in)) / A : 0;
                    return (uint32_t)std::lround(std::clamp(v, 0.f, 255.f)) << shift;
                };
                c = (uint32_t)std::lround(A * 255) << 24 | mix(16, 0xFF) | mix(8, 0x4D) | mix(0, 0x5E);
            }
        void* bits = nullptr;
        HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, dc);
        if (color) {
            memcpy(bits, px.data(), px.size() * 4);
            HBITMAP m = CreateBitmap(w, h, 1, 1, nullptr);
            ICONINFO out{TRUE, 0, 0, m, color};
            icon = CreateIconIndirect(&out);
            DeleteObject(m);
            DeleteObject(color);
        }
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return icon;
}

// How long a plain MP4 lasts, from its mvhd; 0 when that can't be read.
double Mp4Seconds(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    auto read = [&](uint64_t at, uint8_t* p, DWORD n) {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)at;
        DWORD got = 0;
        return SetFilePointerEx(h, li, nullptr, FILE_BEGIN) && ReadFile(h, p, n, &got, nullptr) && got == n;
    };
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    uint64_t pos = 0, end = (uint64_t)size.QuadPart;
    bool inMoov = false;
    double seconds = 0;
    for (int boxes = 0; pos + 8 <= end && boxes < 64; ++boxes) {
        uint8_t hd[16];
        if (!read(pos, hd, 8)) break;
        uint64_t len = mp4::Be32(hd);
        const uint32_t type = mp4::Be32(hd + 4);
        uint32_t head = 8;
        if (len == 1) {
            if (!read(pos + 8, hd + 8, 8)) break;
            len = mp4::Be64(hd + 8);
            head = 16;
        } else if (len == 0) {
            len = end - pos;
        }
        if (len < head || pos + len > end) break;
        if (!inMoov && type == 'moov') {
            inMoov = true;
            end = pos + len;
            pos += head;
            continue;
        }
        if (inMoov && type == 'mvhd') {
            uint8_t b[32];
            if (read(pos + head, b, 32)) {
                const bool v1 = b[0] == 1;
                const uint32_t scale = mp4::Be32(b + (v1 ? 20 : 12));
                const uint64_t dur = v1 ? mp4::Be64(b + 24) : mp4::Be32(b + 16);
                if (scale) seconds = dur / (double)scale;
            }
            break;
        }
        pos += len;
    }
    CloseHandle(h);
    return seconds;
}

}  // namespace

std::wstring App::ProgramName(const std::wstring& exe) {
    std::wstring name;
    DWORD unused = 0, size = GetFileVersionInfoSizeW(exe.c_str(), &unused);
    if (size) {
        std::vector<uint8_t> info(size);
        struct Translation {
            WORD lang, codePage;
        }* langs = nullptr;
        UINT n = 0;
        if (GetFileVersionInfoW(exe.c_str(), 0, size, info.data()) &&
            VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation", (void**)&langs, &n))
            for (UINT i = 0; i < n / sizeof(Translation) && name.empty(); ++i) {
                wchar_t* s = nullptr;
                UINT len = 0;
                std::wstring key = util::Format(L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[i].lang,
                                                langs[i].codePage);
                if (VerQueryValueW(info.data(), key.c_str(), (void**)&s, &len) && s && len > 1)
                    name.assign(s, wcsnlen(s, len));
            }
    }
    while (!name.empty() && iswspace(name.back())) name.pop_back();
    while (!name.empty() && iswspace(name.front())) name.erase(name.begin());
    if (name.empty() || name.size() > 60) {
        name = util::FileNameOf(exe);
        if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0) name.resize(name.size() - 4);
    }
    return name;
}

ID2D1Bitmap* App::ExeIcon(const std::wstring& exe) {
    auto it = exeIcons_.find(exe);
    if (it != exeIcons_.end()) return it->second.Get();
    ComPtr<ID2D1Bitmap1> bmp;
    WinInfo w;
    w.exePath = exe;
    w.exeName = util::FileNameOf(exe);
    if (GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES)
        if (auto src = GetWindowIconBitmap(gfx_.Wic(), w, (int)std::lround(32 * Scale())))
            bmp = gfx_.CreateBitmapFromWic(src.Get());
    exeIcons_[exe] = bmp;
    return bmp.Get();
}

// Named in the UI's language; the list reads both names (RefreshReplayFiles).
std::wstring App::ReplayDir() const {
    return settings_.outDir + (Language() == Lang::English ? L"\\Replays" : L"\\리플레이");
}

App::RpState App::ReplayState() const {
    if (rpStateForTest_ >= 0) return (RpState)rpStateForTest_;
    if (settings_.rpTargets.empty()) return RpState::NoTarget;
    if (!settings_.rpOn) return RpState::Off;
    if (rec_ != RecState::Idle || rpLocked_ || rpAsleep_) return RpState::Paused;
    if (!rpHeld_.path.empty() && rpPause_ != RpPause::None) return RpState::Paused;
    return rpCap_.Running() ? RpState::Holding : RpState::Waiting;
}

double App::ReplayHeldSeconds() const {
    if (rpStateForTest_ >= 0) return rpHeldForTest_;
    const double len = kReplayLenOptions[settings_.rpLenIdx];
    return rpCap_.Running() ? std::min(len, rpCap_.Elapsed()) : std::min(len, rpClosed_.seconds);
}

void App::SetReplayOn(bool on, bool save) {
    settings_.rpOn = on;
    if (save) settings_.Save();
    RegisterReplayHotkey();
    if (on) {
        rpBlockedKey_.clear();
        SetTimer(hwnd_, kTimerReplay, 1000, nullptr);
        ReplayTick();
    } else {
        KillTimer(hwnd_, kTimerReplay);
        ReplayStop(false);
        rpClosed_ = {};
        rpHeld_ = {};
        rpPause_ = RpPause::None;
        rpNotice_.Hide();
    }
    TrayUpdate();
    Invalidate();
}

// What the capture is made with: when any of it changes, what is held starts over with the new settings.
std::wstring App::ReplayConfigKey() const {
    return util::Format(L"%d %d %d %d %d %d %u %d %d %d %d %d %d %ls", settings_.rpFpsIdx, settings_.rpResIdx,
                        settings_.rpAudio, settings_.rpMic, settings_.rpCursor, settings_.vHardware,
                        settings_.VideoBitrate(), settings_.vRateCtrl, settings_.vFrameMode, settings_.vKeyIdx,
                        settings_.vPresetIdx, settings_.captureMode, settings_.clientOnly, settings_.micDevice.c_str());
}

std::vector<WinInfo> App::ReplayWindows() const {
    std::vector<WinInfo> wins = EnumerateCaptureWindows();
    if (testWindowsOnly_)
        std::erase_if(wins, [](const WinInfo& w) { return w.title.find(L"OzMaker Test Window") == std::wstring::npos; });
    return wins;
}

// Of the programs switched on in the list: the one in front, else the one held already, else the one most recently
// in front (the window list is in z-order). A monitor in the list is held while none of them runs.
// Inside one program nothing held is lost to its other windows (#61): see below, and what a closed window left waits
// for the windows open with it, while a window opened since starts over it.
App::RpHeld App::PickReplayTarget() {
    auto findProgram = [&](const std::wstring& exe) -> const ReplayTarget* {
        for (const ReplayTarget& t : settings_.rpTargets)
            if (!t.monitor && t.on && SamePath(t.path, exe)) return &t;
        return nullptr;
    };
    std::vector<WinInfo> wins = ReplayWindows();
    const HWND fg = rpFrontForTest_ ? rpFrontForTest_ : GetForegroundWindow();
    const WinInfo *front = nullptr, *same = nullptr, *first = nullptr, *firstMin = nullptr;
    const ReplayTarget *frontT = nullptr, *sameT = nullptr, *firstT = nullptr, *firstMinT = nullptr;
    bool waiting = false;  // a window of a program that is still open waits for what was kept
    for (const WinInfo& w : wins) {
        if (w.exePath.empty() || w.pid == GetCurrentProcessId()) continue;
        const ReplayTarget* t = findProgram(w.exePath);
        if (!t) continue;
        if (std::find(rpClosed_.left.begin(), rpClosed_.left.end(), w.hwnd) != rpClosed_.left.end()) {
            waiting = true;
            continue;
        }
        if (w.hwnd == fg) front = &w, frontT = t;
        if (w.hwnd == rpHeld_.hwnd) same = &w, sameT = t;
        if (!w.minimized && !first) first = &w, firstT = t;
        if (!firstMin) firstMin = &w, firstMinT = t;
    }
    // The held window stays while another window of its program is in front: a look there loses nothing. Replay moves
    // once that has lasted as long as the replay: by then what was held from before it has left the ring anyway.
    if (front && same && front != same && frontT == sameT) {
        const double now = util::NowSeconds();
        if (rpOther_ != front->hwnd) {
            rpOther_ = front->hwnd;
            rpOtherSince_ = now;
        }
        if (now - rpOtherSince_ < kReplayLenOptions[settings_.rpLenIdx]) front = nullptr;
    } else {
        rpOther_ = nullptr;
    }
    const WinInfo* w = front ? front : same ? same : first ? first : firstMin;
    const ReplayTarget* t = front ? frontT : same ? sameT : first ? firstT : firstMinT;
    RpHeld out;
    if (w) {
        out.path = t->path;
        out.name = t->name;
        out.hwnd = w->hwnd;
        return out;
    }
    if (waiting || testWindowsOnly_) return out;  // tests hold no screen: it would show the user's windows
    const std::vector<MonitorDesc> monitors = EnumerateMonitors();
    for (const ReplayTarget& m : settings_.rpTargets) {
        if (!m.monitor || !m.on) continue;
        if (const MonitorDesc* d = FindMonitor(monitors, m.path)) {
            out.monitor = true;
            out.path = m.path;
            out.name = util::Format(tr(L"화면 %d"), d->number);
            out.mon = d->handle;
            return out;
        }
    }
    return out;
}

void App::ReplayTick() {
    if (!settings_.rpOn) return;
    const double now = util::NowSeconds();
    if (!rpClosed_.path.empty() && now - rpClosed_.at > kReplayKeepClosed) {
        rpClosed_ = {};
        if (!rpCap_.Running()) ClearRing();
    }
    // The span grows or shrinks with 길이: the ring keeps it from a keyframe on, and a little more.
    rpRing_.SetKeep((kReplayLenOptions[settings_.rpLenIdx] + kKeyframeOptions[settings_.vKeyIdx] + 2) * 10'000'000LL);
    if (rec_ != RecState::Idle || rpLocked_ || rpAsleep_) {
        if (rpCap_.Running()) ReplayStop(false);
        TrayUpdate();
        Invalidate();
        return;
    }
    RpHeld want = PickReplayTarget();
    const bool same = !want.path.empty() && want.monitor == rpHeld_.monitor && SamePath(want.path, rpHeld_.path) &&
                      want.hwnd == rpHeld_.hwnd && want.mon == rpHeld_.mon;
    if (rpCap_.Running() && (!same || rpConfig_ != ReplayConfigKey())) {
        // What was held went away, was switched off, or another program came to the front; or the settings changed.
        ReplayStop(!rpHeld_.monitor && !IsWindow(rpHeld_.hwnd));  // a program that closed leaves what was held
    }
    if (want.path.empty()) {
        if (!rpCap_.Running()) {
            rpHeld_ = {};
            rpPause_ = RpPause::None;
        }
        TrayUpdate();
        Invalidate();
        return;
    }
    if (!rpCap_.Running()) {
        rpHeld_ = want;
        const std::wstring key = want.path + L"|" + util::Format(L"%p", (void*)want.hwnd) + L"|" + ReplayConfigKey();
        if (!want.monitor && IsIconic(want.hwnd)) {
            rpPause_ = RpPause::Minimized;  // it starts once the window is open again
        } else if (rpSaving_) {
            // The next tick: a new stream would take the ring from the save in progress.
        } else if (rpBlockedKey_ == key) {
            rpPause_ = RpPause::Blocked;
        } else {
            std::wstring err;
            if (ReplayStart(want, err)) {
                rpPause_ = RpPause::None;
                rpBlockedKey_.clear();
            } else {
                rpPause_ = RpPause::Blocked;
                rpBlocked_ = err;
                rpBlockedKey_ = key;  // not again every second, until the window or the settings change
            }
        }
    } else {
        const bool minimized = !rpHeld_.monitor && IsIconic(rpHeld_.hwnd);
        if (minimized != (rpPause_ == RpPause::Minimized)) {
            rpCap_.SetPaused(minimized);
            rpPause_ = minimized ? RpPause::Minimized : RpPause::None;
        }
    }
    TrayUpdate();
    Invalidate();
}

bool App::ReplayStart(const RpHeld& t, std::wstring& err) {
    CaptureOptions o;
    if (t.monitor) {
        o.source = CaptureSource::Monitor;
        o.monitor = t.mon;
    } else {
        o.source = CaptureSource::Window;
        o.target = t.hwnd;
        o.monitorMode = UseMonitorCapture(t.hwnd);
    }
    o.clientOnly = settings_.clientOnly;
    o.cursor = settings_.rpCursor;
    o.fps = kVideoFpsOptions[settings_.rpFpsIdx];
    o.maxEdge = 4096;  // H.264: the ring takes fragments, which Windows writes only for it
    if (int res = kVideoResOptions[settings_.rpResIdx]) {
        o.boxShort = res;
        o.boxLong = res * 16 / 9;
    }
    VideoOptions v;
    v.video = VideoSettings(o.fps);
    v.video.hevc = false;
    v.sound = settings_.rpAudio;
    v.mic = settings_.rpMic;
    v.micDevice = settings_.micDevice;
    v.micGain = settings_.micVolume / 100.f;
    v.audio.sampleRate = kAudioRateOptions[settings_.aRateIdx];
    v.audio.bitrate = kAudioBitrateOptions[settings_.aBitrateIdx] * 1000;
    // A program records only its own sound; a monitor what the speakers play.
    v.processAudio = !t.monitor;
    if (!t.monitor) {
        v.audioPid = AudioCapture::AudioProcessOf(t.hwnd);
        if (v.sound && !AudioCapture::ProcessLoopbackSupported()) v.sound = false;
    }
    v.audio.enabled = v.sound || v.mic;
    v.ring = &rpRing_;
    o.video = std::move(v);
    rpClosed_ = {};  // the new stream takes the ring
    if (!rpCap_.Start(o, hwnd_, WM_APP_REPLAY_CLOSED, err)) return false;
    rpConfig_ = ReplayConfigKey();
    RegisterShotHotkey();
    return true;
}

void App::ReplayStop(bool keep) {
    const bool was = rpCap_.Running();
    if (was) {
        rpCap_.Stop();  // the last fragments go into the ring
        RegisterShotHotkey();
    }
    keep = keep && was && !rpHeld_.path.empty() && rpRing_.Held() >= 10'000'000;  // a second at least
    if (keep) {
        rpClosed_.path = rpHeld_.path;
        rpClosed_.name = rpHeld_.name;
        rpClosed_.at = util::NowSeconds();
        rpClosed_.seconds = std::min<double>(kReplayLenOptions[settings_.rpLenIdx], rpRing_.Held() / 1e7);
        rpClosed_.left.clear();
        for (const WinInfo& w : ReplayWindows())
            if (w.hwnd != rpHeld_.hwnd && SamePath(w.exePath, rpHeld_.path)) rpClosed_.left.push_back(w.hwnd);
    } else {
        rpClosed_ = {};
        ClearRing();
    }
    rpHeld_ = {};
    rpPause_ = RpPause::None;
}

void App::ClearRing() {
    if (rpSaving_)
        rpClearAfterSave_ = true;  // the save copies what it needs first
    else
        rpRing_.Clear();
}

void App::ReplayClear() {
    ReplayStop(false);
    TrayUpdate();
    Invalidate();
}

void App::OnReplayClosed(bool failed) {
    if (!rpCap_.Running()) return;
    if (failed) {
        rpCap_.Stop();
        RegisterShotHotkey();
        ClearRing();
        rpPause_ = RpPause::Blocked;
        rpBlocked_ = tr(L"동영상 인코더가 멈췄습니다.");
        rpBlockedKey_ = rpHeld_.path + L"|" + util::Format(L"%p", (void*)rpHeld_.hwnd) + L"|" + ReplayConfigKey();
        TrayUpdate();
        Invalidate();
        return;
    }
    ReplayStop(true);
    ReplayTick();
}

// Where the save notice goes and whether it may have buttons: the mouse is the game's when it is full screen, hides
// the pointer or holds it inside its window.
void App::NoticePlace(RECT& rect, bool& full, bool& grabbed, bool& exclusive) const {
    HWND fg = GetForegroundWindow();
    full = grabbed = exclusive = false;
    rect = {};
    if (rpCap_.Running() && !rpHeld_.monitor && IsWindow(rpHeld_.hwnd)) {
        GetVisibleFrame(rpHeld_.hwnd, rect);
        full = IsFullscreenWindow(rpHeld_.hwnd);
    } else {
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(rpHeld_.monitor && rpHeld_.mon ? rpHeld_.mon : MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY), &mi);
        rect = mi.rcMonitor;
        full = true;
    }
    if (fg && fg != hwnd_ && IsFullscreenWindow(fg)) full = true;
    grabbed = full;
    CURSORINFO ci{sizeof(ci)};
    if (GetCursorInfo(&ci) && !(ci.flags & CURSOR_SHOWING)) grabbed = true;
    RECT clip{};
    const RECT virt{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                    GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                    GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    if (GetClipCursor(&clip) && !EqualRect(&clip, &virt)) grabbed = true;
    QUERY_USER_NOTIFICATION_STATE st{};
    exclusive = SUCCEEDED(SHQueryUserNotificationState(&st)) && st == QUNS_RUNNING_D3D_FULL_SCREEN;
}

void App::ReplaySave() {
    if (!settings_.rpOn || rpSaving_) return;
    const bool live = rpCap_.Running();
    RpSave s;
    s.inApp = GetForegroundWindow() == hwnd_ && IsWindowVisible(hwnd_);
    NoticePlace(s.rect, s.full, s.grabbed, s.exclusive);
    if (!live && rpClosed_.path.empty()) {
        s.err = rec_ != RecState::Idle ? tr(L"녹화하는 동안은 리플레이를 담지 않습니다.") : tr(L"아직 담은 장면이 없습니다.");
        rpSave_ = std::move(s);
        ShowReplayResult();
        return;
    }
    const int64_t span = kReplayLenOptions[settings_.rpLenIdx] * 10'000'000LL;
    const int64_t until = live ? (int64_t)(rpCap_.Elapsed() * 1e7) : rpRing_.End();
    s.name = live ? rpHeld_.name : rpClosed_.name;
    s.closed = !live;
    if (s.name.empty()) s.name = tr(L"리플레이");
    const std::wstring stem = util::SafeFileName(s.name, 60);
    const std::wstring dir = ReplayDir() + L"\\" + stem;
    if (!util::EnsureDir(dir)) {
        s.err = tr(L"저장 폴더를 만들 수 없습니다.");
        rpSave_ = std::move(s);
        ShowReplayResult();
        return;
    }
    const std::wstring base = dir + L"\\" + stem + L"_" + util::TimestampName();
    s.path = base + L".mp4";
    for (int i = 2; GetFileAttributesW(s.path.c_str()) != INVALID_FILE_ATTRIBUTES; ++i)
        s.path = base + util::Format(L"_%d.mp4", i);
    rpSave_ = std::move(s);
    rpSaving_ = true;
    if (rpSaveThread_.joinable()) rpSaveThread_.join();
    // The save waits for the fragments the encoder still holds, then writes the file: off the window's thread.
    rpSaveThread_ = std::thread([this, path = rpSave_.path, until, span] {
        rpSaveOut_ = {};
        rpSaveOut_.ok = rpRing_.Save(path, until, span, 2500, rpSaveOut_.seconds, rpSaveOut_.err);
        if (rpSaveOut_.ok) rpSaveOut_.bytes = util::FileSize(path);
        PostMessageW(hwnd_, WM_APP_REPLAY_SAVED, 0, 0);
    });
    Invalidate();
}

void App::OnReplaySaved() {
    if (rpSaveThread_.joinable()) rpSaveThread_.join();
    rpSaving_ = false;
    rpSave_.ok = rpSaveOut_.ok;
    rpSave_.seconds = rpSaveOut_.seconds;
    rpSave_.bytes = rpSaveOut_.bytes;
    if (!rpSaveOut_.ok) rpSave_.err = rpSaveOut_.err;
    if (rpSave_.ok) {
        AddRecent(rpSave_.path);
        rpNewPath_ = rpSave_.path;
        if (rpSave_.closed) rpClosed_ = {};  // saved: what the closed program left goes
        RefreshReplayFiles();
    }
    if (std::exchange(rpClearAfterSave_, false) && !rpCap_.Running()) rpRing_.Clear();
    if (rpSave_.closed && rpSave_.ok && !rpCap_.Running()) rpRing_.Clear();
    ShowReplayResult();
}

// Saved or not: a sound (it follows 효과음), and a notice over the game unless it holds the screen to itself; in the
// window when the save came from there.
void App::ShowReplayResult() {
    const RpSave& s = rpSave_;
    if (s.ok && settings_.sounds) sound::Play(sound::Cue::Saved);
    std::wstring sub = s.ok ? util::Format(tr(L"방금 %s · MP4 · %s"), util::FormatClock(s.seconds, false).c_str(),
                                           util::FormatBytes(s.bytes).c_str())
                            : s.err;
    if (s.inApp) {
        if (s.ok)
            Toast(tr(L"리플레이를 저장했습니다 · ") + sub);
        else
            Toast(tr(L"리플레이를 저장하지 못했습니다. ") + sub, true);
    } else if (!s.exclusive) {
        ReplayNotice::Content c;
        c.title = s.ok ? tr(L"리플레이를 저장했습니다") : tr(L"리플레이를 저장하지 못했습니다");
        c.sub = sub;
        c.ok = s.ok;
        c.buttons = s.ok && !s.grabbed;
        rpNotice_.Show(s.rect, s.full, c, s.grabbed ? 3000 : 5000, [this, path = s.path](int b) {
            if (b == ReplayNotice::kFolder) {
                util::OpenFolderAndSelect(path);
            } else if (b == ReplayNotice::kEdit) {
                ShowMain(true);
                OpenMediaFile(path);
            }
        });
    } else if (!s.ok) {
        TrayBalloon(tr(L"리플레이를 저장하지 못했습니다"), sub);
    }
    Invalidate();
}

HICON App::ReplayTrayIcon() {
    if (!iconReplay_) iconReplay_ = MakeDotIcon(iconAppSmall_);
    return iconReplay_ ? iconReplay_ : iconAppSmall_;
}

void App::RegisterReplayHotkey() {
    UnregisterHotKey(hwnd_, kHotkeyReplay);
    hookReplay_ = false;
    replayHotkeyOk_ = true;
    if (settings_.rpOn && !capturingHotkey_) {
        replayHotkeyOk_ =
            RegisterHotKey(hwnd_, kHotkeyReplay, settings_.rpHkMods | MOD_NOREPEAT, settings_.rpHkVk) != FALSE;
        hookReplay_ = !replayHotkeyOk_ && settings_.rpHkHook;
    }
    SyncKeyHook();
}

void App::AddReplayTarget(ReplayTarget t) {
    for (const ReplayTarget& o : settings_.rpTargets)
        if (o.monitor == t.monitor && SamePath(o.path, t.path)) return;
    if (settings_.rpTargets.size() >= kMaxReplayTargets) {
        Toast(util::Format(tr(L"프로그램은 %zu개까지 넣을 수 있습니다."), kMaxReplayTargets), true);
        return;
    }
    const bool first = settings_.rpTargets.empty();
    settings_.rpTargets.push_back(std::move(t));
    settings_.Save();
    // The first program picked switches replay on: that is what picking it was for.
    if (first && !settings_.rpOn)
        SetReplayOn(true);
    else if (settings_.rpOn)
        ReplayTick();
    Invalidate();
}

void App::RemoveReplayTarget(size_t i) {
    if (i >= settings_.rpTargets.size()) return;
    settings_.rpTargets.erase(settings_.rpTargets.begin() + i);
    settings_.Save();
    if (settings_.rpTargets.empty() && settings_.rpOn)
        SetReplayOn(false);
    else
        ReplayTick();
    Invalidate();
}

// The 리플레이 tab's add popover: the programs that show a window now (full screen ones first), without those in the
// list already and without OzMaker. Their names come from the exe, never from a window title.
void App::ListRunningPrograms() {
    rpRunning_.clear();
    std::vector<WinInfo> wins = EnumerateCaptureWindows();
    if (testWindowsOnly_)
        std::erase_if(wins, [](const WinInfo& w) { return w.title.find(L"OzMaker Test Window") == std::wstring::npos; });
    for (const WinInfo& w : wins) {
        if (w.exePath.empty() || w.pid == GetCurrentProcessId() || IsShellWindow(w.hwnd)) continue;
        bool listed = std::any_of(settings_.rpTargets.begin(), settings_.rpTargets.end(), [&](const ReplayTarget& t) {
            return !t.monitor && SamePath(t.path, w.exePath);
        });
        auto it = std::find_if(rpRunning_.begin(), rpRunning_.end(),
                               [&](const RunningProgram& p) { return SamePath(p.path, w.exePath); });
        if (listed) continue;
        if (it != rpRunning_.end()) {
            it->full = it->full || w.fullscreen;
            continue;
        }
        rpRunning_.push_back({w.exePath, ProgramName(w.exePath), w.fullscreen});
    }
    std::stable_sort(rpRunning_.begin(), rpRunning_.end(),
                     [](const RunningProgram& a, const RunningProgram& b) { return a.full > b.full; });
}

void App::BrowseReplayProgram() {
    std::wstring path;
    if (!util::PickExeFile(hwnd_, path)) return;
    AddReplayTarget({false, path, ProgramName(path), true});
}

// 저장 폴더 › 리플레이, and the same folder under the other language's name (saved before the language changed).
void App::RefreshReplayFiles() {
    std::vector<ReplayFile> old = std::move(rpFiles_);
    rpFiles_.clear();
    std::vector<std::wstring> roots{settings_.outDir + L"\\리플레이", settings_.outDir + L"\\Replays"};
    for (const std::wstring& root : roots) {
        WIN32_FIND_DATAW d;
        HANDLE f = FindFirstFileExW((root + L"\\*").c_str(), FindExInfoBasic, &d, FindExSearchLimitToDirectories,
                                    nullptr, 0);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || d.cFileName[0] == L'.') continue;
            const std::wstring dir = root + L"\\" + d.cFileName;
            WIN32_FIND_DATAW m;
            HANDLE g = FindFirstFileExW((dir + L"\\*.mp4").c_str(), FindExInfoBasic, &m, FindExSearchNameMatch,
                                        nullptr, 0);
            if (g == INVALID_HANDLE_VALUE) continue;
            do {
                if (m.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                ReplayFile r;
                r.path = dir + L"\\" + m.cFileName;
                r.program = d.cFileName;
                r.bytes = (uint64_t)m.nFileSizeHigh << 32 | m.nFileSizeLow;
                r.written = (uint64_t)m.ftLastWriteTime.dwHighDateTime << 32 | m.ftLastWriteTime.dwLowDateTime;
                auto same = std::find_if(old.begin(), old.end(), [&](const ReplayFile& o) {
                    return o.bytes == r.bytes && o.written == r.written && SamePath(o.path, r.path);
                });
                r.seconds = same != old.end() ? same->seconds : Mp4Seconds(r.path);
                rpFiles_.push_back(std::move(r));
            } while (rpFiles_.size() < 2000 && FindNextFileW(g, &m));
            FindClose(g);
        } while (FindNextFileW(f, &d));
        FindClose(f);
    }
    std::sort(rpFiles_.begin(), rpFiles_.end(), [](const ReplayFile& a, const ReplayFile& b) { return a.written > b.written; });
    if (!rpFilter_.empty() && std::none_of(rpFiles_.begin(), rpFiles_.end(),
                                           [&](const ReplayFile& r) { return SamePath(r.program, rpFilter_); }))
        rpFilter_.clear();
    Invalidate();
}

void App::RenameReplay(size_t i, std::wstring name) {
    if (i >= rpFiles_.size()) return;
    while (!name.empty() && iswspace(name.back())) name.pop_back();
    if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".mp4") == 0) name.resize(name.size() - 4);
    name = util::SafeFileName(name, 120);
    if (name.empty()) return;
    const std::wstring& from = rpFiles_[i].path;
    const std::wstring to = from.substr(0, from.find_last_of(L'\\') + 1) + name + L".mp4";
    if (SamePath(from, to)) return;
    if (!MoveFileW(from.c_str(), to.c_str())) {
        Toast(GetLastError() == ERROR_ALREADY_EXISTS ? tr(L"같은 이름의 파일이 있습니다.") : tr(L"이름을 바꾸지 못했습니다."), true);
        return;
    }
    if (SamePath(rpNewPath_, from)) rpNewPath_ = to;
    for (std::wstring& p : settings_.recent)
        if (SamePath(p, from)) p = to;
    settings_.Save();
    RefreshReplayFiles();
}

void App::TrashReplay(size_t i) {
    if (i >= rpFiles_.size()) return;
    const std::wstring path = rpFiles_[i].path;
    if (settings_.deleteMode != kDeleteNow) {
        RemoveReplay(path);
        return;
    }
    // By path: the list can change under the question.
    Ask({tr(L"이 리플레이를 삭제할까요?"), util::FileNameOf(path) + tr(L"\n휴지통을 거치지 않고 지워서 되돌릴 수 없습니다."),
         tr(L"삭제"), true, [this, path] { RemoveReplay(path); }});
}

void App::RemoveReplay(const std::wstring& path) {
    const bool now = settings_.deleteMode == kDeleteNow;
    if (clip_ && SamePath(clip_->SourcePath(), path)) CloseClip();  // the editor lets go of it first
    if (!RemoveFile(path)) {
        Toast(now ? tr(L"파일을 삭제하지 못했습니다. 다른 프로그램에서 열려 있다면 닫고 다시 해 보세요.")
                  : tr(L"휴지통으로 옮기지 못했습니다."),
              true);
        return;
    }
    Toast((now ? tr(L"삭제했습니다 · ") : tr(L"휴지통으로 옮겼습니다 · ")) + util::FileNameOf(path));
    RefreshReplayFiles();
}

void App::EditReplay(const std::wstring& path) {
    if (exporter_.Busy()) {
        Toast(tr(L"내보내기가 끝난 뒤에 열 수 있습니다."), true);
        return;
    }
    OpenMediaFile(path);
}

void App::OpenReplayTab() {
    if (screen_ == Screen::Editor) PauseEditor();
    screen_ = Screen::Replay;
    RefreshReplayFiles();
    Invalidate();
}
