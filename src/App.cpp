#include "pch.h"
#include "App.h"
#include "Resize.h"
#include "Sound.h"
#include "Util.h"
#include "../res/resource.h"

using namespace theme;

namespace {

constexpr float kMinW = 920, kMinH = 640;

// Red record dot with a white rim (tray icon while recording).
HICON MakeRecIcon(int size) {
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    auto* px = static_cast<uint32_t*>(bits);
    const float c = size / 2.f, outer = size * 0.47f, inner = size * 0.36f;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float aOuter = 0, aInner = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    float fx = x + (sx + 0.5f) / 4 - c, fy = y + (sy + 0.5f) / 4 - c;
                    float d = std::sqrt(fx * fx + fy * fy);
                    aOuter += d <= outer;
                    aInner += d <= inner;
                }
            aOuter /= 16;
            aInner /= 16;
            // inner red over white rim
            float r = 255 * (1 - aInner) + 255 * aInner, g = 255 * (1 - aInner) + 77 * aInner,
                  b = 255 * (1 - aInner) + 94 * aInner;
            uint32_t A = (uint32_t)(aOuter * 255 + 0.5f);
            px[y * size + x] = (A << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

std::vector<std::wstring> SplitArgs(const std::wstring& cmd) {
    std::vector<std::wstring> out;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(cmd.empty() ? L"x" : (L"x " + cmd).c_str(), &argc)) {
        for (int i = 1; i < argc; ++i) out.emplace_back(argv[i]);
        LocalFree(argv);
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------- lifecycle

int App::Run(HINSTANCE inst, int show, const std::wstring& cmdLine) {
    inst_ = inst;
    settings_.Load();
    if (!gfx_.Init()) {
        MessageBoxW(nullptr, L"그래픽 장치를 초기화할 수 없습니다.", L"OzMaker", MB_ICONERROR);
        return 1;
    }
    Fonts::Init(gfx_.DWrite());
    overlay_.Init(inst, gfx_.D2D());
    picker_.Init(inst, gfx_.D2D());
    regionPicker_.Init(inst, gfx_.D2D());

    auto args = SplitArgs(cmdLine);
    if (args.size() >= 2 && args[0] == L"--screenshot") return RunScreenshots(args[1]);
    if (args.size() >= 2 && args[0] == L"--selftest") return RunSelfTest(args[1]);
    if (args.size() >= 2 && args[0] == L"--record-test") return RunRecordTest(cmdLine);
    if (args.size() >= 2 && args[0] == L"--analyze") return RunAnalyzeTest(cmdLine);
    if (args.size() >= 3 && args[0] == L"--editor-test") return RunEditorTest(args[1], args[2]);
    if (args.size() >= 2 && args[0] == L"--video-test")
        return RunVideoTest(args[1], args.size() >= 4 ? _wtoi(args[2].c_str()) : 640,
                            args.size() >= 4 ? _wtoi(args[3].c_str()) : 360);

    if (!CreateMainWindow(show)) return 1;
    if (!Capture::IsSupported())
        Toast(L"이 Windows에서는 화면 캡처(Windows.Graphics.Capture)를 사용할 수 없습니다.", true);
    Loop();
    return 0;
}

bool App::CreateMainWindow(int show) {
    int smX = GetSystemMetrics(SM_CXSMICON), smY = GetSystemMetrics(SM_CYSMICON);
    iconApp_ = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    iconAppSmall_ = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, smX, smY, 0);
    iconRec_ = MakeRecIcon(smX);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst_;
    wc.hIcon = iconApp_;
    wc.hIconSm = iconAppSmall_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"OzMakerMain";
    if (!RegisterClassExW(&wc)) return false;

    POINT origin{0, 0};
    HMONITOR mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    float s = dx / 96.f;
    int w = (int)(1120 * s), h = (int)(780 * s);
    int x = (mi.rcWork.left + mi.rcWork.right - w) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - h) / 2;

    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"OzMaker", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, inst_,
                            this);
    if (!hwnd_) return false;
    dpi_ = GetDpiForWindow(hwnd_);
    BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark))))
        DwmSetWindowAttribute(hwnd_, 19, &dark, sizeof(dark));
    COLORREF caption = RGB(0x0D, 0x0F, 0x13);
    DwmSetWindowAttribute(hwnd_, 35 /*DWMWA_CAPTION_COLOR, Win11*/, &caption, sizeof(caption));
    if (!gfx_.AttachWindow(hwnd_)) return false;

    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    TrayAdd();
    RegisterRecordHotkey();
    RefreshWindows();
    SetTimer(hwnd_, kTimerRefresh, 2500, nullptr);
    ShowWindow(hwnd_, show);
    return true;
}

void App::Loop() {
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        bool visible = hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
        double now = util::NowSeconds();
        if (visible && (dirty_ || ui_.Animating() || (wake_ > 0 && now >= wake_))) {
            Render();
            continue;
        }
        DWORD timeout = INFINITE;
        if (visible && wake_ > 0) timeout = (DWORD)std::clamp((wake_ - now) * 1000.0, 0.0, 1000.0);
        MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

void App::Render() {
    if (!hwnd_) return;
    dirty_ = false;
    if (gfx_.Generation() != iconGen_) {
        // Device was (re)created: drop GPU bitmaps; they are rebuilt lazily.
        iconGen_ = gfx_.Generation();
        icons_.clear();
        frameBmp_.Reset();
        shown_ = SIZE_MAX;
        thumbs_.clear();
        snapBmp_.Reset();
        PostMessageW(hwnd_, WM_APP_THUMBS, 0, 0);
    }
    ID2D1DeviceContext* dc = gfx_.BeginFrame((float)dpi_);
    if (!dc) return;
    double now = util::NowSeconds();
    wake_ = 0;
    ui_.Begin(dc, input_, now);
    input_.ClearEdges();
    RECT rc;
    GetClientRect(hwnd_, &rc);
    thumbWanted_ = false;
    DrawFrame(Rect(0, 0, rc.right / Scale(), rc.bottom / Scale()));
    ui_.End();
    gfx_.EndFrame();
    ApplyThumbnail();
    SyncAudioPreview();
    RequestWake(ui_.WakeAt());
    if (settingsDirty_ && !input_.down) {
        settings_.Save();
        settingsDirty_ = false;
    }

    POINT pt;
    GetCursorPos(&pt);
    if (WindowFromPoint(pt) == hwnd_ && SendMessageW(hwnd_, WM_NCHITTEST, 0, MAKELPARAM(pt.x, pt.y)) == HTCLIENT)
        SetCursor(LoadCursorW(nullptr, ui_.Cursor()));
}

void App::ApplyThumbnail() {
    bool want = thumbWanted_ && target_ && IsWindow(target_) && screen_ == Screen::Home && IsWindowVisible(hwnd_);
    if (!want) {
        if (thumb_) DwmUnregisterThumbnail(thumb_);
        thumb_ = nullptr;
        thumbSrc_ = nullptr;
        return;
    }
    if (thumb_ && thumbSrc_ != target_) {
        DwmUnregisterThumbnail(thumb_);
        thumb_ = nullptr;
    }
    if (!thumb_) {
        if (FAILED(DwmRegisterThumbnail(hwnd_, target_, &thumb_))) {
            thumb_ = nullptr;
            return;
        }
        thumbSrc_ = target_;
    }
    SIZE src{};
    if (settings_.clientOnly) {
        WINDOWINFO wi{sizeof(wi)};
        if (GetWindowInfo(target_, &wi)) src = {wi.rcClient.right - wi.rcClient.left, wi.rcClient.bottom - wi.rcClient.top};
    }
    if (src.cx <= 0 || src.cy <= 0) DwmQueryThumbnailSourceSize(thumb_, &src);
    if (src.cx <= 0 || src.cy <= 0) src = {16, 9};
    float s = Scale();
    float aw = thumbArea_.W() * s, ah = thumbArea_.H() * s;
    float f = std::min({aw / src.cx, ah / src.cy, 1.f});
    int dw = (int)(src.cx * f), dh = (int)(src.cy * f);
    int x = (int)(thumbArea_.l * s + (aw - dw) / 2), y = (int)(thumbArea_.t * s + (ah - dh) / 2);
    DWM_THUMBNAIL_PROPERTIES p{};
    p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
    p.rcDestination = {x, y, x + dw, y + dh};
    p.fVisible = TRUE;
    p.opacity = 255;
    p.fSourceClientAreaOnly = settings_.clientOnly;
    DwmUpdateThumbnailProperties(thumb_, &p);
}

void App::ShowMain(bool activate) {
    if (!hwnd_) return;
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, activate ? SW_RESTORE : SW_SHOWNOACTIVATE);
    ShowWindow(hwnd_, activate ? SW_SHOW : SW_SHOWNA);
    if (activate) SetForegroundWindow(hwnd_);
    if (screen_ == Screen::Home) RefreshWindows();
    Invalidate();
}

void App::HideMain() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
    SyncAudioPreview();
}

void App::Toast(std::wstring text, bool error) {
    toast_ = std::move(text);
    toastError_ = error;
    toastUntil_ = util::NowSeconds() + (error ? 5.0 : 3.0);
    Invalidate();
}

LRESULT CALLBACK App::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd_ = h;
    }
    auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->Handle(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT App::Handle(UINT m, WPARAM w, LPARAM l) {
    const float s = Scale();
    if (m == taskbarCreated_ && taskbarCreated_) {
        TrayAdd();
        return 0;
    }
    switch (m) {
        case WM_MOUSEMOVE:
            input_.mx = GET_X_LPARAM(l) / s;
            input_.my = GET_Y_LPARAM(l) / s;
            if (!tracking_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
                TrackMouseEvent(&tme);
                tracking_ = true;
            }
            Invalidate();
            return 0;
        case WM_MOUSELEAVE:
            tracking_ = false;
            if (!input_.down) input_.mx = input_.my = -1e6f;
            Invalidate();
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            SetCapture(hwnd_);
            input_.mx = GET_X_LPARAM(l) / s;
            input_.my = GET_Y_LPARAM(l) / s;
            input_.down = true;
            input_.pressed = true;
            if (m == WM_LBUTTONDBLCLK) input_.dblclick = true;
            Invalidate();
            return 0;
        case WM_LBUTTONUP:
            ReleaseCapture();
            input_.mx = GET_X_LPARAM(l) / s;
            input_.my = GET_Y_LPARAM(l) / s;
            input_.down = false;
            input_.released = true;
            Invalidate();
            return 0;
        case WM_CAPTURECHANGED:
            if (input_.down) {
                input_.down = false;
                input_.released = true;
                Invalidate();
            }
            return 0;
        case WM_RBUTTONDOWN:
            input_.rpressed = true;
            Invalidate();
            return 0;
        case WM_MOUSEWHEEL:
            input_.wheel += GET_WHEEL_DELTA_WPARAM(w) / (float)WHEEL_DELTA;
            Invalidate();
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (capturingHotkey_) {
                UINT vk = (UINT)w;
                if (vk == VK_ESCAPE) {
                    capturingHotkey_ = false;
                } else if (vk != VK_CONTROL && vk != VK_MENU && vk != VK_SHIFT && vk != VK_LWIN && vk != VK_RWIN) {
                    UINT mods = 0;
                    if (GetKeyState(VK_CONTROL) < 0) mods |= MOD_CONTROL;
                    if (GetKeyState(VK_MENU) < 0) mods |= MOD_ALT;
                    if (GetKeyState(VK_SHIFT) < 0) mods |= MOD_SHIFT;
                    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) mods |= MOD_WIN;
                    bool fkey = vk >= VK_F1 && vk <= VK_F24;
                    if (mods || fkey || vk == VK_PAUSE || vk == VK_SCROLL) {
                        settings_.hkMods = mods;
                        settings_.hkVk = vk;
                        settings_.Save();
                        capturingHotkey_ = false;
                        RegisterRecordHotkey();
                        if (!hotkeyOk_) Toast(L"다른 프로그램이 이미 사용 중인 단축키입니다.", true);
                    }
                }
                Invalidate();
                return 0;
            }
            input_.keys.push_back((UINT)w);
            input_.ctrl = GetKeyState(VK_CONTROL) < 0;
            input_.shift = GetKeyState(VK_SHIFT) < 0;
            input_.alt = GetKeyState(VK_MENU) < 0;
            Invalidate();
            if (m == WM_SYSKEYDOWN) break;
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, ui_.Cursor()));
                return TRUE;
            }
            break;
        case WM_SIZE:
            gfx_.Resize();
            if (w != SIZE_MINIMIZED)
                Render();
            else
                SyncAudioPreview();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
            mmi->ptMinTrackSize = {(LONG)(kMinW * s), (LONG)(kMinH * s)};
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(w);
            auto* r = reinterpret_cast<RECT*>(l);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            icons_.clear();
            Invalidate();
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            EndPaint(hwnd_, &ps);
            Render();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_ACTIVATE:
            if (LOWORD(w) != WA_INACTIVE && screen_ == Screen::Home) RefreshWindows();
            Invalidate();
            break;
        case WM_HOTKEY:
            if (w == 1) OnHotkey();
            return 0;
        case WM_DISPLAYCHANGE:
            monitors_ = EnumerateMonitors();
            Invalidate();
            break;
        case WM_TIMER:
            if (w == kTimerRec) {
                if (rec_ == RecState::Recording) {
                    overlay_.Update(capture_.Elapsed(), capture_.Frames());
                    if (IsWindowVisible(hwnd_)) Invalidate();
                }
            } else if (w == kTimerCountdown) {
                if (rec_ == RecState::Countdown) {
                    if (--countdownLeft_ <= 0) {
                        BeginCapture();
                    } else {
                        overlay_.ShowCountdown(recTarget_.rect, countdownLeft_);
                        if (settings_.sounds) sound::Play(sound::Cue::Tick);
                    }
                }
            } else if (w == kTimerRefresh) {
                if (screen_ == Screen::Home && IsWindowVisible(hwnd_) && !IsIconic(hwnd_) && !picker_.Active() &&
                    !regionPicker_.Active())
                    RefreshWindows();
            }
            return 0;
        case WM_APP_TRAY:
            switch (LOWORD(l)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                case NIN_BALLOONUSERCLICK:
                    ShowMain(true);
                    break;
                case WM_CONTEXTMENU:
                    TrayMenu();
                    break;
            }
            return 0;
        case WM_APP_SHOW:
            ShowMain(true);
            return 0;
        case WM_APP_QUIT:
            quitting_ = true;
            DestroyWindow(hwnd_);
            return 0;
        case WM_APP_CAPTURE_CLOSED:
            if (rec_ == RecState::Recording) StopRecording();
            return 0;
        case WM_APP_EXPORT_DONE:
            OnExportDone();
            return 0;
        case WM_APP_FRAME: {
            size_t idx = 0;
            if (clip_ && fetcher_.Take(framePx_, idx) &&
                framePx_.size() == (size_t)clip_->Width() * clip_->Height() * 4) {
                UploadFrame(framePx_.data());
                shown_ = idx;
                Invalidate();
            }
            return 0;
        }
        case WM_APP_THUMBS: {
            std::lock_guard lk(thumbMtx_);
            thumbs_.clear();
            for (auto& px : thumbPixels_) thumbs_.push_back(gfx_.CreateBitmap(thumbW_, thumbH_, px.data(), thumbW_ * 4));
            Invalidate();
            return 0;
        }
        case WM_CLOSE:
            if (settings_.closeToTray && !quitting_) {
                HideMain();
                if (!settings_.trayHintShown) {
                    settings_.trayHintShown = true;
                    settings_.Save();
                    TrayBalloon(L"OzMaker는 트레이에서 계속 실행됩니다",
                                util::HotkeyToString(settings_.hkMods, settings_.hkVk) +
                                    L" 로 언제든 녹화를 시작할 수 있습니다.");
                }
                return 0;
            }
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            if (rec_ == RecState::Recording) capture_.Stop();
            overlay_.Hide();
            exporter_.Cancel();
            StopThumbnails();
            audioPreview_.Stop();
            fetcher_.Stop();
            if (thumb_) DwmUnregisterThumbnail(thumb_);
            thumb_ = nullptr;
            UnregisterHotKey(hwnd_, 1);
            TrayRemove();
            settings_.Save();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

// ---------------------------------------------------------------- sources

void App::RefreshWindows() {
    windows_ = EnumerateCaptureWindows();
    if (target_ && !IsWindow(target_)) target_ = nullptr;
    for (auto it = icons_.begin(); it != icons_.end();) {
        if (!IsWindow(it->first))
            it = icons_.erase(it);
        else
            ++it;
    }
    monitors_ = EnumerateMonitors();
    Invalidate();
}

void App::SelectTarget(HWND h) {
    target_ = h;
    Invalidate();
}

const WinInfo* App::FindWindowInfo(HWND h) const {
    for (auto& w : windows_)
        if (w.hwnd == h) return &w;
    return nullptr;
}

ID2D1Bitmap* App::IconFor(const WinInfo& w) {
    auto it = icons_.find(w.hwnd);
    if (it != icons_.end()) return it->second.Get();
    ComPtr<ID2D1Bitmap1> bmp;
    if (auto src = GetWindowIconBitmap(gfx_.Wic(), w, (int)std::lround(32 * Scale()))) bmp = gfx_.CreateBitmapFromWic(src.Get());
    icons_[w.hwnd] = bmp;
    return bmp.Get();
}

void App::SetSource(int source) {
    settings_.source = source;
    settingsDirty_ = true;
    if (source == kSourceDisplay) monitors_ = EnumerateMonitors();
    Invalidate();
}

void App::StartRegionPick() {
    if (rec_ != RecState::Idle || regionPicker_.Active()) return;
    HideMain();
    regionPicker_.Start(EnumerateCaptureWindows(), [this](bool ok, RECT r) {
        if (ok) {
            settings_.region = r;
            settings_.source = kSourceRegion;
            settings_.Save();
            TakeRegionSnapshot();  // before our window comes back over it
        }
        ShowMain(true);
    });
}

void App::TakeRegionSnapshot() {
    snapBmp_.Reset();
    RECT r = settings_.region;
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w < 2 || h < 2) return;
    double f = std::min({1.0, 1280.0 / w, 720.0 / h});
    int tw = std::max(1, (int)std::lround(w * f)), th = std::max(1, (int)std::lround(h * f));
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = tw;
    bi.bmiHeader.biHeight = -th;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC mem = CreateCompatibleDC(screen);
    if (dib && mem) {
        HGDIOBJ old = SelectObject(mem, dib);
        SetStretchBltMode(mem, HALFTONE);
        SetBrushOrgEx(mem, 0, 0, nullptr);
        StretchBlt(mem, 0, 0, tw, th, screen, r.left, r.top, w, h, SRCCOPY);
        GdiFlush();
        auto* px = static_cast<uint32_t*>(bits);
        for (size_t i = 0, n = (size_t)tw * th; i < n; ++i) px[i] |= 0xFF000000u;
        snapBmp_ = gfx_.CreateBitmap(tw, th, static_cast<const uint8_t*>(bits), tw * 4);
        snapRect_ = r;
        SelectObject(mem, old);
    }
    if (mem) DeleteDC(mem);
    if (dib) DeleteObject(dib);
    ReleaseDC(nullptr, screen);
}

RecTarget App::WindowTarget(HWND h) const {
    RecTarget t;
    t.source = kSourceWindow;
    t.hwnd = h;
    if (!GetVisibleFrame(h, t.rect)) GetWindowRect(h, &t.rect);
    return t;
}

RecTarget App::MonitorTarget(HMONITOR m) const {
    RecTarget t;
    t.source = kSourceDisplay;
    t.monitor = m;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(m, &mi);
    t.rect = mi.rcMonitor;
    return t;
}

bool App::CurrentTarget(RecTarget& t, std::wstring* why) const {
    auto fail = [&](const wchar_t* msg) {
        if (why) *why = msg;
        return false;
    };
    switch (settings_.source) {
        case kSourceDisplay: {
            const MonitorDesc* m = FindMonitor(monitors_, settings_.monitor);
            if (!m) return fail(L"녹화할 화면을 찾을 수 없습니다.");
            t = MonitorTarget(m->handle);
            return true;
        }
        case kSourceRegion: {
            if (!settings_.HasRegion()) return fail(L"녹화할 영역을 먼저 지정하세요.");
            RECT r = settings_.region, clip{};
            HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
            MONITORINFO mi{sizeof(mi)};
            if (!mon || !GetMonitorInfoW(mon, &mi) || !IntersectRect(&clip, &r, &mi.rcMonitor) ||
                clip.right - clip.left < 16 || clip.bottom - clip.top < 16)
                return fail(L"지정한 영역이 화면 밖에 있습니다. 영역을 다시 지정하세요.");
            t = {};
            t.source = kSourceRegion;
            t.monitor = mon;
            t.rect = clip;
            return true;
        }
        default:
            if (!target_ || !IsWindow(target_)) return fail(L"녹화할 창을 먼저 선택하세요.");
            t = WindowTarget(target_);
            return true;
    }
}

void App::SourceSize(const RecTarget& t, int& w, int& h) const {
    w = t.rect.right - t.rect.left;
    h = t.rect.bottom - t.rect.top;
    if (t.source == kSourceWindow && settings_.clientOnly) {
        WINDOWINFO wi{sizeof(wi)};
        if (GetWindowInfo(t.hwnd, &wi)) {
            w = wi.rcClient.right - wi.rcClient.left;
            h = wi.rcClient.bottom - wi.rcClient.top;
        }
    }
}

void App::PlannedSize(int sw, int sh, int& ow, int& oh) const {
    // Mirrors Capture::Setup.
    double f = 1.0;
    int longE = std::max(1, std::max(sw, sh)), shortE = std::max(1, std::min(sw, sh));
    if (settings_.output == kOutputVideo) {
        f = std::min(f, (settings_.vCodec == 1 ? 8192.0 : 4096.0) / longE);
        if (int res = kVideoResOptions[settings_.vResIdx]) {
            f = std::min(f, res * 16.0 / 9.0 / longE);
            f = std::min(f, (double)res / shortE);
        }
    } else if (int maxE = kSizeOptions[settings_.sizeIdx]) {
        f = std::min(f, (double)maxE / longE);
    }
    ow = std::max(2, (int)std::lround(sw * f) & ~1);
    oh = std::max(2, (int)std::lround(sh * f) & ~1);
}

// ---------------------------------------------------------------- recording

bool App::UseMonitorCapture(HWND target) const {
    if (settings_.captureMode == kCaptureDisplay) return true;
    if (settings_.captureMode == kCaptureWindow) return false;
    return IsFullscreenWindow(target);  // fullscreen games capture most reliably through the display
}

void App::StartRecording(const RecTarget& target, bool fromHotkey) {
    if (rec_ != RecState::Idle) return;
    if (exporter_.Busy()) {
        Toast(L"내보내기가 끝난 뒤에 녹화할 수 있습니다.", true);
        return;
    }
    if (target.source == kSourceWindow && (!target.hwnd || !IsWindow(target.hwnd))) {
        ShowMain(true);
        Toast(L"녹화할 창을 먼저 선택하세요.", true);
        return;
    }
    recTarget_ = target;
    recFromHotkey_ = fromHotkey;
    recVideo_ = settings_.output == kOutputVideo;
    if (target.source == kSourceWindow) {
        if (IsIconic(target.hwnd)) {
            ShowWindow(target.hwnd, SW_RESTORE);
            recTarget_ = WindowTarget(target.hwnd);
        }
        recFullscreen_ = IsFullscreenWindow(target.hwnd);
    } else {
        HWND fg = GetForegroundWindow();
        recFullscreen_ = fg && fg != hwnd_ && IsFullscreenWindow(fg);
    }
    if (!fromHotkey) {
        HideMain();
        if (target.source == kSourceWindow) SetForegroundWindow(target.hwnd);
    }
    int cd = fromHotkey ? 0 : settings_.CountdownSeconds();
    if (cd > 0) {
        rec_ = RecState::Countdown;
        countdownLeft_ = cd;
        overlay_.ShowCountdown(recTarget_.rect, cd);
        if (settings_.sounds) sound::Play(sound::Cue::Tick);
        SetTimer(hwnd_, kTimerCountdown, 1000, nullptr);
        TrayUpdate();
        return;
    }
    BeginCapture();
}

void App::BeginCapture() {
    KillTimer(hwnd_, kTimerCountdown);
    const bool window = recTarget_.source == kSourceWindow;
    if (window && !IsWindow(recTarget_.hwnd)) {
        rec_ = RecState::Idle;
        overlay_.Hide();
        ShowMain(true);
        Toast(L"녹화할 창이 닫혔습니다.", true);
        return;
    }
    if (window) recTarget_.rect = WindowTarget(recTarget_.hwnd).rect;

    CaptureOptions o;
    o.source = window ? CaptureSource::Window
                      : recTarget_.source == kSourceDisplay ? CaptureSource::Monitor : CaptureSource::Region;
    o.target = recTarget_.hwnd;
    o.monitorMode = window && UseMonitorCapture(recTarget_.hwnd);
    o.monitor = recTarget_.monitor;
    o.region = recTarget_.rect;
    o.clientOnly = settings_.clientOnly;
    o.cursor = settings_.cursor;
    recNotice_.clear();
    if (recVideo_) {
        VideoOptions v;
        util::EnsureDir(settings_.outDir);
        std::wstring base = settings_.outDir + L"\\OzMaker_" + util::TimestampName();
        v.path = base + L".mp4";
        for (int i = 2; GetFileAttributesW(v.path.c_str()) != INVALID_FILE_ATTRIBUTES; ++i)
            v.path = base + util::Format(L"_%d.mp4", i);
        o.fps = kVideoFpsOptions[settings_.vFpsIdx];
        v.video = VideoSettings(o.fps);
        v.audio.enabled = settings_.audio;
        v.audio.sampleRate = kAudioRateOptions[settings_.aRateIdx];
        v.audio.bitrate = kAudioBitrateOptions[settings_.aBitrateIdx] * 1000;
        // A window records only its own sound; screens and regions record what the speakers play.
        v.processAudio = window;
        if (window) {
            v.audioPid = AudioCapture::AudioProcessOf(recTarget_.hwnd);
            if (v.audio.enabled && !AudioCapture::ProcessLoopbackSupported()) {
                v.audio.enabled = false;
                recNotice_ = L"이 Windows 버전에서는 창 소리만 따로 녹음할 수 없어 소리 없이 녹화했습니다.";
            }
        }
        o.maxEdge = v.video.hevc ? 8192 : 4096;  // encoder limits
        if (int res = kVideoResOptions[settings_.vResIdx]) {
            o.boxShort = res;
            o.boxLong = res * 16 / 9;
        }
        o.video = std::move(v);
    } else {
        o.fps = kFpsOptions[settings_.fpsIdx];
        o.maxEdge = kSizeOptions[settings_.sizeIdx];
    }
    std::wstring err;
    if (!capture_.Start(o, hwnd_, WM_APP_CAPTURE_CLOSED, err)) {
        rec_ = RecState::Idle;
        overlay_.Hide();
        ShowMain(true);
        Toast(err, true);
        TrayUpdate();
        return;
    }
    rec_ = RecState::Recording;
    bool pill = settings_.overlayMode == kOverlayAlways ||
                (settings_.overlayMode == kOverlayExceptFullscreen && !recFullscreen_);
    if (pill)
        overlay_.ShowPill(recTarget_.rect, recTarget_.source == kSourceRegion,
                          [this] { PostMessageW(hwnd_, WM_HOTKEY, 1, 0); });
    else
        overlay_.Hide();
    if (recTarget_.source == kSourceRegion && settings_.overlayMode != kOverlayNever) overlay_.ShowFrame(recTarget_.rect);
    if (settings_.sounds) sound::Play(sound::Cue::Start);
    SetTimer(hwnd_, kTimerRec, 100, nullptr);
    TrayUpdate();
    Invalidate();
}

void App::StopRecording() {
    if (rec_ == RecState::Countdown) {
        KillTimer(hwnd_, kTimerCountdown);
        rec_ = RecState::Idle;
        overlay_.Hide();
        TrayUpdate();
        ShowMain(true);
        return;
    }
    if (rec_ != RecState::Recording) return;
    KillTimer(hwnd_, kTimerRec);
    CaptureResult res = capture_.Stop();
    rec_ = RecState::Idle;
    overlay_.Hide();
    TrayUpdate();
    if (res.video) {
        OnVideoSaved(res);
        return;
    }
    if (settings_.sounds) sound::Play(sound::Cue::Stop);
    auto clip = res.clip;
    if (!clip) {
        ShowMain(true);
        Toast(recTarget_.source == kSourceWindow
                  ? L"캡처된 화면이 없습니다. 설정에서 캡처 방식을 '화면'으로 바꿔 보세요."
                  : L"캡처된 화면이 없습니다.",
              true);
        return;
    }
    OpenEditor(clip);
    // Don't yank the user out of a fullscreen game: notify via the tray instead.
    HWND fg = GetForegroundWindow();
    if (recFromHotkey_ && fg && fg != hwnd_ && IsFullscreenWindow(fg)) {
        TrayBalloon(L"녹화 완료 · " + util::FormatClock(clip->Seconds(), true),
                    L"클릭하면 편집 화면이 열립니다.");
    } else {
        ShowMain(true);
    }
}

void App::OnVideoSaved(const CaptureResult& r) {
    if (!r.ok) {
        if (settings_.sounds) sound::Play(sound::Cue::Stop);
        ShowMain(true);
        Toast(r.error.empty() ? L"동영상을 저장하지 못했습니다." : r.error, true);
        return;
    }
    if (settings_.sounds) sound::Play(sound::Cue::Saved);
    lastVideo_ = r;
    if (!recNotice_.empty()) lastVideo_.notice = recNotice_;
    showVideo_ = true;
    // The file is already saved; the editor opens on it for trimming, with that file as the current result.
    std::wstring err;
    if (auto video = VideoClip::Open(r.path, err)) {
        OpenEditor(video);
        lastResult_ = {};
        lastResult_.ok = true;
        lastResult_.audio = r.audio;
        lastResult_.path = r.path;
        lastResult_.bytes = r.bytes;
        lastResult_.seconds = r.seconds;
        hasResult_ = resultOriginal_ = true;
    } else {
        if (screen_ == Screen::Settings) screen_ = Screen::Home;
        if (screen_ == Screen::Editor) CloseEditor();
    }
    std::wstring info = util::FileNameOf(r.path) + L" · " + util::FormatBytes(r.bytes);
    HWND fg = GetForegroundWindow();
    if (recFromHotkey_ && fg && fg != hwnd_ && IsFullscreenWindow(fg))
        TrayBalloon(L"동영상 저장 완료 · " + util::FormatClock(r.seconds, false), info);
    else
        ShowMain(true);
    if (!lastVideo_.notice.empty())
        Toast(lastVideo_.notice, true);
    else
        Toast(L"동영상을 저장했습니다 · " + info);
}

void App::OnHotkey() {
    if (rec_ != RecState::Idle) {
        StopRecording();
        return;
    }
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    RecTarget t;
    std::wstring why;
    if (!fg || pid == GetCurrentProcessId()) {
        // Our window is in front: record whatever is selected in it.
        if (CurrentTarget(t, &why)) {
            StartRecording(t, false);
        } else {
            ShowMain(true);
            Toast(why, true);
        }
        return;
    }
    HWND root = GetAncestor(fg, GA_ROOTOWNER);
    if (!root) root = fg;
    switch (settings_.source) {
        case kSourceDisplay:
            StartRecording(MonitorTarget(MonitorFromWindow(root, MONITOR_DEFAULTTONEAREST)), true);
            break;
        case kSourceRegion:
            if (CurrentTarget(t, &why))
                StartRecording(t, true);
            else
                TrayBalloon(L"영역 녹화를 시작할 수 없습니다", why);
            break;
        default:
            StartRecording(WindowTarget(root), true);
    }
}

void App::ProbeEncoders() {
    if (encProbed_) return;
    encProbed_ = true;
    for (int hevc = 0; hevc < 2; ++hevc)
        for (int hw = 0; hw < 2; ++hw) encName_[hevc][hw] = Mp4Writer::FindEncoder(hevc != 0, hw != 0);
}

std::wstring App::VideoSummary() const {
    int hevc = settings_.vCodec;
    std::wstring s = hevc ? L"HEVC" : L"H.264";
    const std::wstring& hw = encName_[hevc][1];
    if (settings_.vHardware && !hw.empty()) {
        s += L" · " + Mp4Writer::Vendor(hw) + L" 가속";
    } else {
        s += L" · CPU 인코딩";
    }
    int mbps = kVideoBitrateOptions[settings_.vBitrateIdx];
    s += mbps ? util::Format(L" · %d Mbps", mbps) : L" · 자동 화질";
    return s;
}

std::wstring App::AudioSummary(int source) const {
    if (!settings_.audio) return L"소리 끔";
    if (source == kSourceWindow)
        return AudioCapture::ProcessLoopbackSupported() ? L"이 창의 소리만 녹음" : L"창 소리 녹음 불가 (Windows 10 2004 이상)";
    return L"시스템 전체 소리 녹음";
}

// ---------------------------------------------------------------- editor

void App::OpenEditor(std::shared_ptr<Clip> clip) {
    StopThumbnails();
    audioPreview_.Stop();
    DetachReaders();
    clip_ = std::move(clip);
    framePx_.clear();
    frameBmp_.Reset();
    shown_ = SIZE_MAX;
    inF_ = 0;
    outF_ = clip_->Count() - 1;
    playhead_ = 0;
    cropMode_ = false;
    cropAspect_ = 0;
    cropDrag_ = -1;
    ResetCrop();
    hasResult_ = resultOriginal_ = false;
    // A video stays a video by default; a clip starts with the last 움짤 format.
    bool video = !clip_->SourcePath().empty();
    fmt_ = video && FormatAvailable(Format::Mp4) ? (int)Format::Mp4 : settings_.format;
    exportAudio_ = true;
    AttachReaders();
    screen_ = Screen::Editor;
    StartThumbnails();
    SetPlaying(true);
    Invalidate();
}

bool App::OpenVideo(const std::wstring& path) {
    std::wstring err;
    auto video = VideoClip::Open(path, err);
    if (!video) {
        Toast(err, true);
        return false;
    }
    OpenEditor(video);
    return true;
}

void App::ResumeEditor() {
    if (!clip_) return;
    AttachReaders();
    screen_ = Screen::Editor;
    SetPlaying(true);
}

// Back to the home screen. The clip stays (temp file only) so "이전 녹화 편집" can reopen it until
// the next recording replaces it. A video's file is let go meanwhile so it can be moved or deleted.
void App::CloseEditor() {
    if (exporter_.Busy()) return;
    playing_ = false;
    cropMode_ = false;
    screen_ = Screen::Home;
    SyncAudioPreview();
    DetachReaders();
    RefreshWindows();
}

void App::AttachReaders() {
    if (!clip_) return;
    if (clip_->SlowSeek()) {
        if (!fetcher_.Running()) {
            fetcher_.Start(clip_, hwnd_, WM_APP_FRAME);
            shown_ = SIZE_MAX;  // ask the new fetcher
        }
    } else if (!reader_) {
        reader_ = clip_->NewReader();
    }
}

void App::DetachReaders() {
    fetcher_.Stop();
    reader_.reset();
}

void App::ShowFrame(size_t index) {
    if (!clip_) return;
    bool fresh = frameBmp_ && frameBmpGen_ == gfx_.Generation();
    if (index == shown_ && fresh) return;
    if (clip_->SlowSeek()) {
        // The fetcher answers with WM_APP_FRAME; until then the last frame stays up.
        if (!fresh && framePx_.size() == (size_t)clip_->Width() * clip_->Height() * 4) UploadFrame(framePx_.data());
        if (fetcher_.Running()) fetcher_.Want(index);
        return;
    }
    if (!reader_) return;
    const uint8_t* px = reader_->Get(index);
    if (!px) return;
    UploadFrame(px);
    shown_ = index;
}

void App::UploadFrame(const uint8_t* px) {
    UINT w = clip_->Width(), h = clip_->Height();
    if (!frameBmp_ || frameBmpGen_ != gfx_.Generation()) {
        frameBmp_ = gfx_.CreateBitmap(w, h, px, w * 4);
        frameBmpGen_ = gfx_.Generation();
    } else {
        frameBmp_->CopyFromMemory(nullptr, px, w * 4);
    }
}

// A video's sound follows the preview: only while it plays at 1x in the editor and OzMaker is in front.
void App::SyncAudioPreview() {
    auto video = std::dynamic_pointer_cast<const VideoClip>(clip_);
    bool want = video && video->HasAudio() && !previewMute_ && playing_ && !scrubbing_ && screen_ == Screen::Editor &&
                kSpeedOptions[settings_.speedIdx] == 1.0 && hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_) &&
                GetForegroundWindow() == hwnd_;
    if (!want) {
        audioPreview_.Stop();
        return;
    }
    if (audioPreview_.Active() && audioEpoch_ == playEpoch_) return;
    audioEpoch_ = playEpoch_;
    int64_t start = clip_->Ts(inF_), end = RangeEndTs();
    int64_t t = playTs_ + (int64_t)((util::NowSeconds() - playWall_) * 1e7);
    audioPreview_.Start(video, std::clamp(t, start, end - 1), end);
}

int64_t App::RangeEndTs() const {
    if (!clip_) return 0;
    return outF_ + 1 < clip_->Count() ? clip_->Ts(outF_ + 1) : clip_->EndTs();
}

void App::SetPlaying(bool play) {
    if (!clip_) return;
    if (play && playhead_ >= outF_) playhead_ = inF_;
    playing_ = play;
    playWall_ = util::NowSeconds();
    playTs_ = clip_->Ts(playhead_);
    ++playEpoch_;
    Invalidate();
}

void App::Seek(size_t index) {
    if (!clip_) return;
    playhead_ = std::min(index, clip_->Count() - 1);
    playWall_ = util::NowSeconds();
    playTs_ = clip_->Ts(playhead_);
    ++playEpoch_;
    Invalidate();
}

size_t App::PlayheadFromClock() {
    double speed = kSpeedOptions[settings_.speedIdx];
    int64_t start = clip_->Ts(inF_), end = RangeEndTs();
    int64_t len = std::max<int64_t>(1, end - start);
    double now = util::NowSeconds();
    int64_t t = playTs_ + (int64_t)((now - playWall_) * 1e7 * speed);
    if (t < start) t = start;
    if (t >= end) {
        if (settings_.loop) {
            t = start + (t - start) % len;
            // Restart the clock at the loop point (the sound restarts with it).
            playTs_ = t;
            playWall_ = now;
            ++playEpoch_;
        } else {
            t = end - 1;
            playing_ = false;
        }
    }
    size_t idx = std::clamp(clip_->IndexAt(t), inF_, outF_);
    if (playing_) {
        int64_t nextTs = idx + 1 <= outF_ ? clip_->Ts(idx + 1) : end;
        RequestWake(now + std::max(0.002, (nextTs - t) / 1e7 / speed));
    }
    return idx;
}

void App::ResetCrop() {
    if (clip_) crop_ = {0, 0, clip_->Width(), clip_->Height()};
}

void App::ApplyCropAspect() {
    static const double aspects[] = {0, 1.0, 4.0 / 3.0, 16.0 / 9.0, 9.0 / 16.0};
    if (!clip_ || cropAspect_ <= 0) return;
    double a = aspects[cropAspect_];
    int W = clip_->Width(), H = clip_->Height();
    int w = W, h = (int)std::lround(W / a);
    if (h > H) {
        h = H;
        w = (int)std::lround(H * a);
    }
    int cx = (crop_.left + crop_.right) / 2, cy = (crop_.top + crop_.bottom) / 2;
    int x = std::clamp(cx - w / 2, 0, W - w), y = std::clamp(cy - h / 2, 0, H - h);
    crop_ = {x, y, x + w, y + h};
}

void App::OutputSize(int& w, int& h) const {
    int cw = crop_.right - crop_.left, ch = crop_.bottom - crop_.top;
    int pct = kScaleOptions[settings_.scaleIdx];
    double s = pct == 33 ? 1.0 / 3.0 : pct / 100.0;
    w = std::max(2, (int)std::lround(cw * s) & ~1);
    h = std::max(2, (int)std::lround(ch * s) & ~1);
}

double App::EffectiveOutFps() const {
    double limit = kOutFpsOptions[settings_.outFpsIdx];
    if (fmt_ == (int)Format::Mp4) {
        // Constant rate: the chosen one, or the recording's own rounded up to a usual rate (29.4 -> 30).
        if (limit > 0) return limit;
        double avg = clip_ ? clip_->AverageFps() : 30.0;
        for (double r : {10.0, 15.0, 20.0, 24.0, 25.0, 30.0, 50.0, 60.0, 90.0, 120.0})
            if (avg <= r * 1.03) return r;
        return 120.0;
    }
    double maxF = FormatMaxFps((Format)fmt_);
    if (limit == 0 || limit > maxF) {
        // Only cap when the recording actually exceeds what the format can play.
        if (clip_ && clip_->AverageFps() * kSpeedOptions[settings_.speedIdx] > maxF + 0.5) return maxF;
        return limit;
    }
    return limit;
}

// MP4 keeps the video's sound unless the speed changes (no time-stretching).
bool App::ExportAudio() const {
    return fmt_ == (int)Format::Mp4 && exportAudio_ && clip_ && clip_->HasAudio() &&
           kSpeedOptions[settings_.speedIdx] == 1.0;
}

VideoConfig App::VideoSettings(int fps) const {
    VideoConfig v;
    v.fps = fps;
    v.hevc = settings_.vCodec == 1;
    v.hardware = settings_.vHardware;
    v.bitrate = (UINT32)kVideoBitrateOptions[settings_.vBitrateIdx] * 1'000'000u;
    v.cbr = settings_.vRateCtrl == 1;
    v.gopFrames = fps * kKeyframeOptions[settings_.vKeyIdx];
    v.qualityVsSpeed = kPresetOptions[settings_.vPresetIdx];
    return v;
}

// Exports of a video are named after it ("<name>_trim.mp4", "<name>.gif"); a fresh clip gets a timestamp.
std::wstring App::ExportName(Format f) const {
    std::wstring ext = FormatExt(f), src = clip_ ? clip_->SourcePath() : std::wstring();
    std::wstring stem = L"OzMaker_" + util::TimestampName();
    if (!src.empty()) {
        stem = util::FileNameOf(src);
        if (size_t dot = stem.rfind(L'.'); dot != std::wstring::npos) stem.resize(dot);
        if (f == Format::Mp4) stem += L"_trim";
    }
    std::wstring name = stem + L"." + ext;
    for (int i = 2; GetFileAttributesW((settings_.outDir + L"\\" + name).c_str()) != INVALID_FILE_ATTRIBUTES; ++i)
        name = stem + util::Format(L"_%d.", i) + ext;
    return name;
}

void App::StartExport(bool saveAs) {
    if (!clip_ || exporter_.Busy()) return;
    Format f = (Format)fmt_;
    if (!FormatAvailable(f)) {
        Toast(std::wstring(FormatName(f)) + L" 인코더가 이 빌드에 포함되어 있지 않습니다.", true);
        return;
    }
    std::wstring name = ExportName(f);
    std::wstring path;
    if (saveAs) {
        if (!util::PickSavePath(hwnd_, settings_.outDir, name, FormatExt(f), FormatName(f), path)) return;
    } else {
        if (!util::EnsureDir(settings_.outDir)) {
            Toast(L"저장 폴더를 만들 수 없습니다: " + settings_.outDir, true);
            return;
        }
        path = settings_.outDir + L"\\" + name;
    }
    if (_wcsicmp(path.c_str(), clip_->SourcePath().c_str()) == 0) {
        Toast(L"편집 중인 원본 파일에는 덮어쓸 수 없습니다. 다른 이름을 고르세요.", true);
        return;
    }
    ExportJob job;
    job.clip = clip_;
    job.first = inF_;
    job.last = outF_;
    job.crop = crop_;
    OutputSize(job.outW, job.outH);
    job.fpsLimit = EffectiveOutFps();
    job.speed = kSpeedOptions[settings_.speedIdx];
    job.format = f;
    job.loop = settings_.loop;
    if (f == Format::Mp4) {
        job.video = VideoSettings((int)std::lround(job.fpsLimit));
        job.audio.enabled = ExportAudio();
        job.audio.bitrate = kAudioBitrateOptions[settings_.aBitrateIdx] * 1000;
    } else {
        job.quality = settings_.quality[fmt_];
    }
    job.path = path;
    settings_.Save();
    hasResult_ = resultOriginal_ = false;
    exporter_.Start(std::move(job), hwnd_, WM_APP_EXPORT_DONE);
    Invalidate();
}

void App::OnExportDone() {
    ExportResult r = exporter_.Take();
    if (r.cancelled) {
        Toast(L"내보내기를 취소했습니다.");
    } else if (!r.ok) {
        Toast(r.error.empty() ? L"내보내기에 실패했습니다." : r.error, true);
    } else {
        lastResult_ = r;
        hasResult_ = true;
        resultOriginal_ = false;
        if (settings_.sounds) sound::Play(sound::Cue::Saved);
    }
    Invalidate();
}

void App::StartThumbnails() {
    StopThumbnails();
    if (!clip_) return;
    thumbCancel_ = false;
    thumbs_.clear();
    {
        std::lock_guard lk(thumbMtx_);
        thumbPixels_.clear();
    }
    auto clip = clip_;
    const int n = 14;
    int th = std::max(16, (int)std::lround(56 * Scale()));
    int tw = std::max(8, (int)std::lround(th * clip->Width() / (double)clip->Height()));
    thumbW_ = tw;
    thumbH_ = th;
    HWND notify = hwnd_;
    thumbThread_ = std::thread([this, clip, n, tw, th, notify] {
        auto rd = clip->NewReader();
        AreaResizer rs;
        std::vector<std::vector<uint8_t>> out;
        for (int i = 0; i < n && !thumbCancel_; ++i) {
            int64_t t = (int64_t)((i + 0.5) / n * clip->EndTs());
            const uint8_t* px = rd->Get(clip->QuickIndexNear(t, clip->EndTs() / n / 2));
            if (!px) break;
            std::vector<uint8_t> buf((size_t)tw * th * 4);
            rs.Run(px, clip->Width() * 4, 0, 0, clip->Width(), clip->Height(), buf.data(), tw, th);
            out.push_back(std::move(buf));
        }
        if (thumbCancel_) return;
        {
            std::lock_guard lk(thumbMtx_);
            thumbPixels_ = std::move(out);
        }
        if (notify) PostMessageW(notify, WM_APP_THUMBS, 0, 0);
    });
}

void App::StopThumbnails() {
    thumbCancel_ = true;
    if (thumbThread_.joinable()) thumbThread_.join();
}

// ---------------------------------------------------------------- system

void App::RegisterRecordHotkey() {
    UnregisterHotKey(hwnd_, 1);
    hotkeyOk_ = RegisterHotKey(hwnd_, 1, settings_.hkMods | MOD_NOREPEAT, settings_.hkVk) != FALSE;
}

void App::TrayAdd() {
    nid_ = {sizeof(nid_)};
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid_.uCallbackMessage = WM_APP_TRAY;
    nid_.hIcon = iconAppSmall_;
    wcscpy_s(nid_.szTip, L"OzMaker");
    Shell_NotifyIconW(NIM_ADD, &nid_);
    nid_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid_);
    TrayUpdate();
}

void App::TrayUpdate() {
    if (!nid_.hWnd) return;
    nid_.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid_.hIcon = rec_ == RecState::Recording ? iconRec_ : iconAppSmall_;
    std::wstring tip = rec_ == RecState::Recording ? L"OzMaker · 녹화 중 (" + util::HotkeyToString(settings_.hkMods, settings_.hkVk) + L" 중지)"
                                              : L"OzMaker · " + util::HotkeyToString(settings_.hkMods, settings_.hkVk) + L" 녹화";
    wcsncpy_s(nid_.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void App::TrayRemove() {
    if (nid_.hWnd) Shell_NotifyIconW(NIM_DELETE, &nid_);
    nid_.hWnd = nullptr;
}

void App::TrayBalloon(const std::wstring& title, const std::wstring& text) {
    if (!nid_.hWnd) return;
    nid_.uFlags = NIF_INFO;
    wcsncpy_s(nid_.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid_.szInfo, text.c_str(), _TRUNCATE);
    nid_.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    nid_.hBalloonIcon = iconApp_;
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
    nid_.szInfo[0] = 0;
}

void App::TrayMenu() {
    HMENU menu = CreatePopupMenu();
    std::wstring hk = util::HotkeyToString(settings_.hkMods, settings_.hkVk);
    AppendMenuW(menu, MF_STRING, 1, L"OzMaker 열기");
    RecTarget current;
    bool canRecord = CurrentTarget(current, nullptr);
    const wchar_t* what = settings_.source == kSourceDisplay  ? L"선택한 화면 녹화\t"
                          : settings_.source == kSourceRegion ? L"지정한 영역 녹화\t"
                                                              : L"선택한 창 녹화\t";
    if (rec_ == RecState::Idle)
        AppendMenuW(menu, MF_STRING | (canRecord ? 0 : MF_GRAYED), 2, (what + hk).c_str());
    else
        AppendMenuW(menu, MF_STRING, 2, (L"녹화 중지\t" + hk).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 3, L"종료");
    SetMenuDefaultItem(menu, 1, FALSE);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
        case 1: ShowMain(true); break;
        case 2:
            if (rec_ == RecState::Idle)
                StartRecording(current, false);
            else
                StopRecording();
            break;
        case 3:
            quitting_ = true;
            DestroyWindow(hwnd_);
            break;
    }
}
