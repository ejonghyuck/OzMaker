#include "pch.h"
#include "App.h"
#include "Resize.h"
#include "Sound.h"
#include "Util.h"
#include "Version.h"
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
    if (args.size() >= 2 && args[0] == L"--region-test") return RunRegionTest(args[1]);
    if (args.size() >= 2 && args[0] == L"--preview-test") return RunPreviewTest(args[1]);
    if (args.size() >= 2 && args[0] == L"--test-window") return RunTestWindow(_wtof(args[1].c_str()));
    if (args.size() >= 2 && args[0] == L"--update-test") return RunUpdateTest(args[1], args.size() >= 3 ? args[2] : L"");
    if (args.size() >= 2 && args[0] == L"--video-test")
        return RunVideoTest(args[1], args.size() >= 4 ? _wtoi(args[2].c_str()) : 640,
                            args.size() >= 4 ? _wtoi(args[3].c_str()) : 360);

    // --tray: started with Windows (HKCU Run). --updated <pid> <shown>: restarted by the version before (main.cpp).
    const bool updated = args.size() >= 3 && args[0] == L"--updated";
    const bool tray = (!args.empty() && args[0] == L"--tray") || (updated && args[2] == L"0");
    if (updated) Updater::CleanUp(util::ExePath());
    autostart_ = util::AutostartEnabled();
    if (autostart_) util::SetAutostart(true);  // keeps pointing at this exe if it was moved
    if (!CreateMainWindow(tray ? SW_HIDE : show)) return 1;
    updater_.Init(util::ExePath(), hwnd_, WM_APP_UPDATE);
    SetTimer(hwnd_, kTimerUpdate, 15000, nullptr);  // the first check, once startup is out of the way
    if (updated) {
        if (tray)
            TrayBalloon(L"OzMaker를 업데이트했습니다", L"버전 " OZ_VERSION_WSTR);
        else
            Toast(L"OzMaker " OZ_VERSION_WSTR L" 버전으로 업데이트했습니다");
    }
    if (!Capture::IsSupported())
        Toast(L"이 Windows에서는 화면 캡처(Windows.Graphics.Capture)를 사용할 수 없습니다.", true);
    if (!args.empty() && args[0].rfind(L"--", 0) != 0) OpenMediaFile(args[0]);  // "Open with", or a file dropped on the exe
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
    // Shrunk to fit small or highly scaled screens (1920×1080 at 150% leaves under 700 DIP of height).
    const RECT& wa = mi.rcWork;
    int w = std::min((int)(1120 * s), (int)(wa.right - wa.left)), h = std::min((int)(780 * s), (int)(wa.bottom - wa.top));
    int x = (wa.left + wa.right - w) / 2, y = std::max(wa.top, (wa.top + wa.bottom - h) / 2);
    // The last position, as long as its title bar is still on a screen (monitors come and go).
    const RECT& saved = settings_.window;
    bool restored = false;
    if (saved.right - saved.left >= 200 && saved.bottom - saved.top >= 150) {
        RECT bar{saved.left, saved.top, saved.right, saved.top + 32}, vis;
        MONITORINFO sm{sizeof(sm)};
        HMONITOR m = MonitorFromRect(&bar, MONITOR_DEFAULTTONULL);
        if (m && GetMonitorInfoW(m, &sm) && IntersectRect(&vis, &bar, &sm.rcWork) && vis.right - vis.left >= 100) {
            x = saved.left;
            y = std::max(saved.top, sm.rcWork.top);
            w = std::min(saved.right - saved.left, sm.rcWork.right - sm.rcWork.left);
            h = std::min(saved.bottom - saved.top, sm.rcWork.bottom - sm.rcWork.top);
            restored = true;
        }
    }

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
    DragAcceptFiles(hwnd_, TRUE);

    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    TrayAdd();
    RegisterRecordHotkey();
    TrayUpdate();
    if (!hotkeyOk_)
        TrayBalloon(L"녹화 단축키를 쓸 수 없습니다",
                    util::HotkeyToString(settings_.hkMods, settings_.hkVk) +
                        L"을(를) 다른 프로그램이 쓰고 있습니다. 설정에서 다른 조합을 지정하세요.");
    RefreshWindows();
    WatchWindows(true);
    if (restored && settings_.windowMax && show != SW_HIDE && show != SW_SHOWMINIMIZED && show != SW_MINIMIZE)
        show = SW_SHOWMAXIMIZED;
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

// DWM draws thumbnails above everything drawn in this window, so nothing of ours can go over the preview. Instead
// the preview is cut around whatever floats above the page this frame (popovers, toasts, tooltips: Ui floats), as
// a thumbnail per part of the window left showing.
void App::ApplyThumbnail() {
    bool want = thumbWanted_ && target_ && IsWindow(target_) && screen_ == Screen::Home && IsWindowVisible(hwnd_);
    if (!want || thumbSrc_ != target_) ReleaseThumbnails();
    if (!want || (thumb_.empty() && !AddThumbnail())) return;
    // DWM reports the size of what a thumbnail shows (just the client area with 제목 표시줄 제외): that goes first.
    DWM_THUMBNAIL_PROPERTIES area{};
    area.dwFlags = DWM_TNP_SOURCECLIENTAREAONLY;
    area.fSourceClientAreaOnly = settings_.clientOnly;
    DwmUpdateThumbnailProperties(thumb_[0], &area);
    RECT dest, src;
    PreviewLayout(dest, src);
    std::vector<RECT> parts = PreviewParts(dest);
    const bool split = !(parts.size() == 1 && EqualRect(&parts[0], &dest));
    if (!split && thumbSplit_) {  // a thumbnail given a source rect can't be told to show the whole window again
        ReleaseThumbnails();
        if (!AddThumbnail()) return;
    }
    while (thumb_.size() < parts.size() && AddThumbnail()) {
    }
    const LONG dw = std::max(1L, dest.right - dest.left), dh = std::max(1L, dest.bottom - dest.top);
    auto sx = [&](LONG x) { return src.left + (LONG)std::lround((double)(x - dest.left) * (src.right - src.left) / dw); };
    auto sy = [&](LONG y) { return src.top + (LONG)std::lround((double)(y - dest.top) * (src.bottom - src.top) / dh); };
    for (size_t i = 0; i < thumb_.size(); ++i) {
        DWM_THUMBNAIL_PROPERTIES p{};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        p.fVisible = i < parts.size();
        p.opacity = 255;
        p.fSourceClientAreaOnly = settings_.clientOnly;
        if (p.fVisible) {
            const RECT& d = parts[i];
            p.rcDestination = d;
            if (split) {
                p.dwFlags |= DWM_TNP_RECTSOURCE;
                p.rcSource = {sx(d.left), sy(d.top), sx(d.right), sy(d.bottom)};
                thumbSplit_ = true;
            }
        }
        DwmUpdateThumbnailProperties(thumb_[i], &p);
    }
}

bool App::AddThumbnail() {
    HTHUMBNAIL t = nullptr;
    if (FAILED(DwmRegisterThumbnail(hwnd_, target_, &t))) return false;
    thumb_.push_back(t);
    thumbSrc_ = target_;
    return true;
}

void App::ReleaseThumbnails() {
    for (HTHUMBNAIL t : thumb_) DwmUnregisterThumbnail(t);
    thumb_.clear();
    thumbSrc_ = nullptr;
    thumbSplit_ = false;
}

void App::PreviewLayout(RECT& dest, RECT& source) {
    // Laid out by its size on screen. Source rects are in DWM's units: the size it reports for what the thumbnail
    // shows (smaller than on screen for a window Windows scales up, not being DPI aware), from the client area's
    // corner, or for the whole window from its corner including the invisible resize borders, which that size
    // leaves out. Without a thumbnail (screenshots), the size on screen.
    SIZE shown{}, dwm{};
    WINDOWINFO wi{sizeof(wi)};
    if (settings_.clientOnly && GetWindowInfo(target_, &wi))
        shown = {wi.rcClient.right - wi.rcClient.left, wi.rcClient.bottom - wi.rcClient.top};
    if (!thumb_.empty()) DwmQueryThumbnailSourceSize(thumb_[0], &dwm);
    if (shown.cx <= 0 || shown.cy <= 0) shown = dwm;
    if (shown.cx <= 0 || shown.cy <= 0) {
        RECT wr{};
        GetWindowRect(target_, &wr);
        shown = {wr.right - wr.left, wr.bottom - wr.top};
    }
    if (shown.cx <= 0 || shown.cy <= 0) shown = {16, 9};
    if (dwm.cx <= 0 || dwm.cy <= 0) dwm = shown;
    source = {0, 0, dwm.cx, dwm.cy};
    RECT wr, frame;
    if (!settings_.clientOnly && GetWindowRect(target_, &wr) &&
        SUCCEEDED(DwmGetWindowAttribute(target_, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) &&
        frame.right > frame.left) {
        double k = (double)dwm.cx / (frame.right - frame.left);
        OffsetRect(&source, (int)std::lround((frame.left - wr.left) * k), (int)std::lround((frame.top - wr.top) * k));
    }
    float s = Scale();
    float aw = thumbArea_.W() * s, ah = thumbArea_.H() * s;
    float f = std::min({aw / shown.cx, ah / shown.cy, 1.f});
    int dw = (int)(shown.cx * f), dh = (int)(shown.cy * f);
    int x = (int)(thumbArea_.l * s + (aw - dw) / 2), y = (int)(thumbArea_.t * s + (ah - dh) / 2);
    dest = {x, y, x + dw, y + dh};
}

std::vector<RECT> App::PreviewParts(const RECT& dest) {
    float s = Scale();
    std::vector<RECT> holes;
    for (const Rect& f : ui_.Floats())
        holes.push_back({(LONG)std::floor(f.l * s), (LONG)std::floor(f.t * s), (LONG)std::ceil(f.r * s),
                         (LONG)std::ceil(f.b * s)});
    auto parts = util::SubtractRects(dest, holes);
    if (parts.size() > 16) parts.clear();  // nothing floats over it that busily; just don't show it meanwhile
    return parts;
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

void App::RememberPlacement() {
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!hwnd_ || !GetWindowPlacement(hwnd_, &wp)) return;
    // rcNormalPosition is in workspace coordinates: relative to the primary monitor's work area.
    MONITORINFO pm{sizeof(pm)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &pm);
    RECT r = wp.rcNormalPosition;
    OffsetRect(&r, pm.rcWork.left, pm.rcWork.top);
    settings_.window = r;
    settings_.windowMax = IsZoomed(hwnd_) || (IsIconic(hwnd_) && (wp.flags & WPF_RESTORETOMAXIMIZED));
}

void App::Toast(std::wstring text, bool error) {
    toast_ = std::move(text);
    toastError_ = error;
    // Long enough to read: longer messages stay longer.
    toastUntil_ = util::NowSeconds() + std::min(12.0, (error ? 5.0 : 3.0) + toast_.size() / 25.0);
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
                    SetCapturingHotkey(0);
                } else if (vk != VK_CONTROL && vk != VK_MENU && vk != VK_SHIFT && vk != VK_LWIN && vk != VK_RWIN) {
                    UINT mods = 0;
                    if (GetKeyState(VK_CONTROL) < 0) mods |= MOD_CONTROL;
                    if (GetKeyState(VK_MENU) < 0) mods |= MOD_ALT;
                    if (GetKeyState(VK_SHIFT) < 0) mods |= MOD_SHIFT;
                    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) mods |= MOD_WIN;
                    bool fkey = vk >= VK_F1 && vk <= VK_F24;
                    const bool record = capturingHotkey_ == kHotkeyRecord;
                    UINT& hm = record ? settings_.hkMods : settings_.hkPauseMods;
                    UINT& hv = record ? settings_.hkVk : settings_.hkPauseVk;
                    const UINT otherMods = record ? settings_.hkPauseMods : settings_.hkMods;
                    const UINT otherVk = record ? settings_.hkPauseVk : settings_.hkVk;
                    if ((fkey || vk == VK_PAUSE || vk == VK_SCROLL || (mods & ~MOD_SHIFT)) && mods == otherMods &&
                        vk == otherVk) {
                        Toast(record ? L"일시정지 단축키와 같습니다. 다른 조합을 눌러 주세요."
                                     : L"녹화 단축키와 같습니다. 다른 조합을 눌러 주세요.",
                              true);
                    } else if (fkey || vk == VK_PAUSE || vk == VK_SCROLL || (mods & ~MOD_SHIFT)) {
                        // Try it first; the keys are registered for real once capturing ends.
                        if (RegisterHotKey(hwnd_, capturingHotkey_, mods | MOD_NOREPEAT, vk)) {
                            UnregisterHotKey(hwnd_, capturingHotkey_);
                            hm = mods;
                            hv = vk;
                            SetCapturingHotkey(0);
                            settings_.Save();
                            TrayUpdate();
                        } else {
                            // Keep the previous key and let them try another.
                            Toast(util::HotkeyToString(mods, vk) + L"은(는) 다른 프로그램이 쓰고 있습니다. 다른 조합을 눌러 주세요.",
                                  true);
                        }
                    } else if (mods == MOD_SHIFT) {
                        // Shift + a letter would swallow capital letters in every other program.
                        Toast(L"Ctrl, Alt, Win 중 하나와 함께 누르세요. (F1~F12는 단독으로도 됩니다)", true);
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
            if (LOWORD(w) == WA_INACTIVE) SetCapturingHotkey(0);  // keys go elsewhere now; keep the old one
            Invalidate();
            break;
        case WM_HOTKEY:
            if (capturingHotkey_) return 0;
            if (w == kHotkeyRecord)
                OnHotkey();
            else if (w == kHotkeyPause)
                TogglePause();
            return 0;
        case WM_DISPLAYCHANGE:
            monitors_ = EnumerateMonitors();
            Invalidate();
            break;
        case WM_TIMER:
            if (w == kTimerRec) {
                if (rec_ == RecState::Recording) {
                    // A window recorded through the monitor (fullscreen game) never closes its capture item.
                    if ((recTarget_.source == kSourceWindow && !IsWindow(recTarget_.hwnd)) || capture_.StoreFailed()) {
                        StopRecording();
                        return 0;
                    }
                    overlay_.Update(capture_.Elapsed(), capture_.Frames());
                    if (IsWindowVisible(hwnd_)) Invalidate();
                    CheckLimits();
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
            } else if (w == kTimerMicTest) {
                // Only while 설정 › 소리 is in front, and 10 s at most.
                double now = util::NowSeconds();
                if (screen_ != Screen::Settings || settings_.settingsTab != kTabAudio || !IsWindowVisible(hwnd_) ||
                    IsIconic(hwnd_) || rec_ != RecState::Idle || now > micTestUntil_) {
                    StopMicTest();
                } else {
                    micMonitor_.SetGain(settings_.micVolume / 100.f);
                    float peak = std::min(1.f, micPeak_.exchange(0) * settings_.micVolume / 100.f);
                    float pos = peak > 0 ? std::clamp((20 * std::log10(peak) + 60) / 60, 0.f, 1.f) : 0.f;
                    micLevel_ = std::max(pos, micLevel_ - (float)(now - micLevelAt_) * 1.2f);
                    micLevelAt_ = now;
                    Invalidate();
                }
            } else if (w == kTimerUpdate) {
                SetTimer(hwnd_, kTimerUpdate, 3600 * 1000, nullptr);  // from now on hourly: a check is due daily
                CheckForUpdate();
            } else if (w == kTimerRefresh) {
                // Otherwise Home refreshes when it is shown or activated again.
                KillTimer(hwnd_, kTimerRefresh);
                windowsDirty_ = false;
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
        case WM_COPYDATA: {
            auto* cd = reinterpret_cast<const COPYDATASTRUCT*>(l);
            if (!cd || cd->dwData != kCopyOpenFile || cd->cbData < sizeof(wchar_t)) return FALSE;
            pendingOpen_.assign((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t) - 1);
            PostMessageW(hwnd_, WM_APP_OPEN, 0, 0);
            return TRUE;
        }
        case WM_APP_OPEN:
            if (!pendingOpen_.empty()) OpenMediaFile(std::exchange(pendingOpen_, {}));
            return 0;
        case WM_DROPFILES: {
            auto drop = (HDROP)w;
            wchar_t path[MAX_PATH * 4];
            bool got = DragQueryFileW(drop, 0, path, _countof(path)) > 0;
            DragFinish(drop);
            if (got) OpenMediaFile(path);
            return 0;
        }
        case WM_APP_QUIT:
            quitting_ = true;
            DestroyWindow(hwnd_);
            return 0;
        case WM_APP_ANSWER:
            if (auto f = std::exchange(answer_, nullptr)) f();
            return 0;
        case WM_APP_CAPTURE_CLOSED:
            if (rec_ == RecState::Recording) StopRecording();
            return 0;
        case WM_APP_EXPORT_DONE:
            OnExportDone();
            return 0;
        case WM_APP_ESTIMATE:
            OnEstimate();
            return 0;
        case WM_APP_UPDATE:
            if (!UpdateReady() && updater_.Get().state != Updater::State::Available) updateOpen_ = false;
            TrayUpdate();
            Invalidate();
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
            if (!settings_.closeToTray && !quitting_) {
                AskQuit(false, [this] { PostMessageW(hwnd_, WM_APP_QUIT, 0, 0); });
                return 0;
            }
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
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            // Logoff or shutdown ends the process without WM_DESTROY: finish the MP4 being recorded now.
            if (w && rec_ == RecState::Recording) {
                KillTimer(hwnd_, kTimerRec);
                capture_.Stop();
                rec_ = RecState::Idle;
            }
            if (w) {
                RememberPlacement();
                settings_.Save();
            }
            return 0;
        case WM_DESTROY:
            if (rec_ == RecState::Recording) capture_.Stop();
            RememberPlacement();
            overlay_.Hide();
            exporter_.Cancel();
            StopThumbnails();
            audioPreview_.Stop();
            fetcher_.Stop();
            ReleaseThumbnails();
            WatchWindows(false);
            StopMicTest();
            UnregisterHotKey(hwnd_, kHotkeyRecord);
            UnregisterHotKey(hwnd_, kHotkeyPause);
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
    if (testWindowsOnly_)
        std::erase_if(windows_, [](const WinInfo& w) { return w.title.find(L"OzMaker Test Window") == std::wstring::npos; });
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

namespace {
App* g_watcher = nullptr;  // the App whose window list the WinEvent hooks keep current
}

// Other programs' windows appearing, closing, renaming, moving or minimizing refresh the window list, instead of
// polling it. Out of context: the callbacks come in through this thread's message loop.
void App::WatchWindows(bool on) {
    for (HWINEVENTHOOK h : winHooks_) UnhookWinEvent(h);
    winHooks_.clear();
    g_watcher = on ? this : nullptr;
    if (!on) return;
    const std::pair<DWORD, DWORD> ranges[] = {
        {EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE},          // destroy, show, hide
        {EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE},  // title
        {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED},      // other virtual desktops, suspended UWP apps
        {EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND},
        {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND},
    };
    for (auto [from, to] : ranges)
        if (HWINEVENTHOOK h = SetWinEventHook(from, to, nullptr, OnWinEvent, 0, 0,
                                              WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS))
            winHooks_.push_back(h);
}

void CALLBACK App::OnWinEvent(HWINEVENTHOOK, DWORD event, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    App* self = g_watcher;
    if (!self || !h || obj != OBJID_WINDOW || child != CHILDID_SELF) return;
    // A top-level window that may join the list, or one already in it.
    bool appears = event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_UNCLOAKED;
    if (appears ? GetAncestor(h, GA_ROOT) != h : !self->FindWindowInfo(h)) return;
    if (self->windowsDirty_) return;
    self->windowsDirty_ = true;
    SetTimer(self->hwnd_, kTimerRefresh, 400, nullptr);
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

// The picker opens on the area picked before; its toolbar records the area at once or keeps it for Home.
void App::StartRegionPick() {
    if (rec_ != RecState::Idle || regionPicker_.Active()) return;
    HideMain();
    RECT last = settings_.HasRegion() ? settings_.region : RECT{};
    regionPicker_.Start(EnumerateCaptureWindows(), last, settings_.countdown, [this](PickResult res, RECT r) {
        // The toolbar's countdown is the setting in 설정 › 녹화.
        bool save = regionPicker_.Countdown() != settings_.countdown;
        settings_.countdown = regionPicker_.Countdown();
        if (res != PickResult::Cancel) {
            settings_.region = r;
            settings_.source = kSourceRegion;
            save = true;
            TakeRegionSnapshot();  // before our window comes back over it
        }
        if (save) settings_.Save();
        if (res == PickResult::Record) {
            RecTarget t;
            std::wstring why;
            if (CurrentTarget(t, &why)) {
                StartRecording(t, false);  // Home stays hidden; the countdown starts over the area
                return;
            }
            ShowMain(true);
            Toast(why, true);
            return;
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

// After a hotkey start with a fullscreen app in front, don't yank the user out of it: use the tray instead.
bool App::NotifyQuietly() const {
    HWND fg = GetForegroundWindow();
    return recFromHotkey_ && fg && fg != hwnd_ && IsFullscreenWindow(fg);
}

void App::ReportError(const std::wstring& title, const std::wstring& text) {
    if (NotifyQuietly()) {
        TrayBalloon(title, text);
    } else {
        ShowMain(true);
        Toast(text, true);
    }
}

void App::StartMicTest() {
    StopMicTest();
    micPeak_ = 0;
    micLevel_ = 0;
    std::wstring err;
    auto sink = [this](const int16_t* pcm, size_t frames) {
        int peak = 0;
        for (size_t i = 0; i < frames * 2; ++i) peak = std::max(peak, std::abs((int)pcm[i]));
        float v = peak / 32768.f, old = micPeak_.load();
        while (v > old && !micPeak_.compare_exchange_weak(old, v)) {
        }
        micMonitor_.Push(pcm, frames);  // dropped unless it plays
    };
    if (!micTest_.Start(AudioCapture::Source::Mic, 0, 48000, util::Qpc100ns(), sink, err, settings_.micDevice)) {
        Toast(err, true);
        return;
    }
    SyncMicMonitor();
    micTestUntil_ = util::NowSeconds() + 10;
    micLevelAt_ = util::NowSeconds();
    SetTimer(hwnd_, kTimerMicTest, 33, nullptr);
    Invalidate();
}

void App::StopMicTest() {
    if (!micTest_.Running()) return;
    KillTimer(hwnd_, kTimerMicTest);
    micTest_.Stop(0);
    micMonitor_.Stop();
    micLevel_ = 0;
    Invalidate();
}

void App::SyncMicMonitor() {
    bool want = micTest_.Running() && settings_.micMonitor;
    micMonitor_.SetGain(settings_.micVolume / 100.f);  // what the recording would get
    if (want == micMonitor_.Running()) return;
    std::wstring err;
    if (!want)
        micMonitor_.Stop();
    else if (!micMonitor_.Start(48000, err))
        Toast(err, true);
}

void App::StartRecording(const RecTarget& target, bool fromHotkey) {
    if (rec_ != RecState::Idle) return;
    StopMicTest();      // the recording opens the mic itself
    question_.reset();  // a question about this window's state (quit, discard) no longer applies
    recFromHotkey_ = fromHotkey;
    if (exporter_.Busy()) {
        if (fromHotkey)
            TrayBalloon(L"녹화를 시작할 수 없습니다", L"내보내기가 끝난 뒤에 녹화할 수 있습니다.");
        else
            Toast(L"내보내기가 끝난 뒤에 녹화할 수 있습니다.", true);
        return;
    }
    if (target.source == kSourceWindow && (!target.hwnd || !IsWindow(target.hwnd))) {
        ShowMain(true);
        Toast(L"녹화할 창을 먼저 선택하세요.", true);
        return;
    }
    recTarget_ = target;
    if (target.source == kSourceDisplay) {
        recName_ = L"화면";
        for (auto& m : monitors_)
            if (m.handle == target.monitor) recName_ = util::Format(L"화면 %d", m.number);
    } else if (target.source == kSourceRegion) {
        recName_ = L"영역";
    } else {
        int len = GetWindowTextLengthW(target.hwnd);
        recName_.assign(len + 1, L'\0');
        recName_.resize(GetWindowTextW(target.hwnd, recName_.data(), len + 1));
        if (recName_.empty()) recName_ = L"창";
    }
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
    o.clickFx = settings_.clickFx;
    recNotice_.clear();
    if (recVideo_) {
        VideoOptions v;
        util::EnsureDir(settings_.outDir);
        std::wstring base = settings_.outDir + L"\\" + FileStem(recName_);
        v.path = base + L".mp4";
        for (int i = 2; GetFileAttributesW(v.path.c_str()) != INVALID_FILE_ATTRIBUTES; ++i)
            v.path = base + util::Format(L"_%d.mp4", i);
        o.fps = kVideoFpsOptions[settings_.vFpsIdx];
        v.video = VideoSettings(o.fps);
        v.sound = settings_.audio;
        v.mic = settings_.mic;
        v.micDevice = settings_.micDevice;
        v.micGain = settings_.micVolume / 100.f;
        v.audio.sampleRate = kAudioRateOptions[settings_.aRateIdx];
        v.audio.bitrate = kAudioBitrateOptions[settings_.aBitrateIdx] * 1000;
        // A window records only its own sound; screens and regions record what the speakers play.
        v.processAudio = window;
        if (window) {
            v.audioPid = AudioCapture::AudioProcessOf(recTarget_.hwnd);
            if (v.sound && !AudioCapture::ProcessLoopbackSupported()) {
                v.sound = false;
                recNotice_ = v.mic ? L"이 Windows 버전에서는 창 소리만 따로 녹음할 수 없어 마이크 소리만 녹음했습니다."
                                   : L"이 Windows 버전에서는 창 소리만 따로 녹음할 수 없어 소리 없이 녹화했습니다.";
            }
        }
        v.audio.enabled = v.sound || v.mic;
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
    // Free space (#17): too little to start, or a warning that the recording will stop itself.
    uint64_t freeBytes = util::FreeBytes(RecordingDir());
    if (freeBytes < kLowDiskStop) {
        rec_ = RecState::Idle;
        overlay_.Hide();
        TrayUpdate();
        ReportError(L"녹화를 시작하지 못했습니다",
                    L"저장할 드라이브에 남은 공간이 " + util::FormatBytes(freeBytes) + L"뿐입니다. 공간을 비운 뒤 녹화하세요.");
        return;
    }
    std::wstring err;
    if (!capture_.Start(o, hwnd_, WM_APP_CAPTURE_CLOSED, err)) {
        rec_ = RecState::Idle;
        overlay_.Hide();
        TrayUpdate();
        ReportError(L"녹화를 시작하지 못했습니다", err);
        return;
    }
    rec_ = RecState::Recording;
    recPaused_ = false;
    stopReason_.clear();
    nextDiskCheck_ = util::NowSeconds() + 2;
    if (freeBytes < kLowDiskWarn)
        TrayBalloon(L"남은 공간이 적습니다", L"저장할 드라이브에 " + util::FormatBytes(freeBytes) +
                                                L" 남았습니다. 300MB까지 줄면 녹화를 멈추고 저장합니다.");
    RegisterPauseHotkey();
    bool pill = settings_.overlayMode == kOverlayAlways ||
                (settings_.overlayMode == kOverlayExceptFullscreen && !recFullscreen_);
    if (pill)
        overlay_.ShowPill(recTarget_.rect, recTarget_.source == kSourceRegion,
                          [this] { PostMessageW(hwnd_, WM_HOTKEY, kHotkeyRecord, 0); },
                          [this] { PostMessageW(hwnd_, WM_HOTKEY, kHotkeyPause, 0); });
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
    recPaused_ = false;
    RegisterPauseHotkey();  // releases it
    overlay_.Hide();
    TrayUpdate();
    if (res.video) {
        OnVideoSaved(res);
        return;
    }
    if (settings_.sounds) sound::Play(sound::Cue::Stop);
    auto clip = res.clip;
    if (clip) HoldRecording(clip);
    clipName_ = recName_;
    if (!clip) {
        ReportError(L"녹화 실패", recTarget_.source == kSourceWindow
                                      ? L"캡처된 화면이 없습니다. 설정에서 캡처 방식을 '화면'으로 바꿔 보세요."
                                      : L"캡처된 화면이 없습니다.");
        return;
    }
    OpenEditor(clip);
    std::wstring warn;
    if (clip->WriteFailed())
        warn = L"디스크 공간이 부족해 녹화를 멈췄습니다. 그 앞까지만 저장되어 있습니다.";
    else if (clip->Dropped() > 0)
        warn = util::Format(L"디스크가 느려 프레임 %u개를 건너뛰었습니다.", clip->Dropped());
    if (!stopReason_.empty() && !clip->WriteFailed()) warn = stopReason_ + (warn.empty() ? L"" : L" " + warn);
    stopReason_.clear();
    if (NotifyQuietly()) {
        TrayBalloon(L"녹화 완료 · " + util::FormatClock(clip->Seconds(), true),
                    warn.empty() ? L"클릭하면 편집 화면이 열립니다." : warn);
    } else {
        ShowMain(true);
        if (!warn.empty()) Toast(warn, true);
    }
}

void App::OnVideoSaved(const CaptureResult& r) {
    if (!r.ok) {
        if (settings_.sounds) sound::Play(sound::Cue::Stop);
        ReportError(L"동영상 저장 실패", r.error.empty() ? L"동영상을 저장하지 못했습니다." : r.error);
        return;
    }
    if (settings_.sounds) sound::Play(sound::Cue::Saved);
    AddRecent(r.path);
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
    std::wstring reason = std::exchange(stopReason_, {});
    if (NotifyQuietly())
        TrayBalloon(L"동영상 저장 완료 · " + util::FormatClock(r.seconds, false), reason.empty() ? info : reason);
    else
        ShowMain(true);
    if (!lastVideo_.notice.empty())
        Toast(reason.empty() ? lastVideo_.notice : reason + L" " + lastVideo_.notice, true);
    else if (!reason.empty())
        Toast(reason + L" · " + info, true);
    else
        Toast(L"동영상을 저장했습니다 · " + info);
}

void App::TogglePause() {
    if (rec_ != RecState::Recording) return;
    recPaused_ = !recPaused_;
    capture_.SetPaused(recPaused_);
    overlay_.SetPaused(recPaused_);
    if (settings_.sounds) sound::Play(sound::Cue::Tick);
    TrayUpdate();
    Invalidate();
}

std::wstring App::RecordingDir() const {
    if (recVideo_) return settings_.outDir;
    wchar_t tmp[MAX_PATH + 1] = {};
    GetTempPathW(_countof(tmp), tmp);  // where FrameStore keeps the clip
    return tmp;
}

void App::CheckLimits() {
    if (rec_ != RecState::Recording) return;
    int minutes = kMaxLengthOptions[settings_.maxLengthIdx];
    if (minutes > 0 && capture_.Elapsed() >= minutes * 60.0) {
        stopReason_ = (minutes % 60 ? util::Format(L"%d분", minutes) : util::Format(L"%d시간", minutes / 60)) +
                      L"이 되어 녹화를 멈췄습니다.";
        StopRecording();
        return;
    }
    double now = util::NowSeconds();
    if (now < nextDiskCheck_) return;
    nextDiskCheck_ = now + 2;
    if (util::FreeBytes(RecordingDir()) < kLowDiskStop) {
        stopReason_ = L"남은 저장 공간이 300MB까지 줄어 녹화를 멈추고 저장했습니다.";
        StopRecording();
    }
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
    // The desktop or taskbar is "in front" when nothing else is: there is no window to record then.
    const bool shell = IsShellWindow(root);
    switch (settings_.source) {
        case kSourceDisplay: {
            POINT pt{};
            GetCursorPos(&pt);
            StartRecording(MonitorTarget(shell ? MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY)
                                               : MonitorFromWindow(root, MONITOR_DEFAULTTONEAREST)),
                           true);
            break;
        }
        case kSourceRegion:
            if (CurrentTarget(t, &why))
                StartRecording(t, true);
            else
                TrayBalloon(L"영역 녹화를 시작할 수 없습니다", why);
            break;
        default:
            if (shell)
                TrayBalloon(L"녹화할 창이 없습니다", L"녹화할 창을 앞에 띄운 뒤 단축키를 누르세요.");
            else
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
    std::wstring s;
    if (settings_.audio)
        s = source != kSourceWindow                         ? L"시스템 전체 소리 녹음"
            : AudioCapture::ProcessLoopbackSupported() ? L"이 창의 소리만 녹음"
                                                       : L"창 소리 녹음 불가 (Windows 10 2004 이상)";
    if (settings_.mic) s = s.empty() ? L"마이크만 녹음" : s + L" + 마이크";
    return s.empty() ? L"소리 끔" : s;
}

std::wstring App::MicDeviceName() const {
    for (const auto& d : micDevices_)
        if (settings_.micDevice.empty() ? d.isDefault : d.id == settings_.micDevice)
            return settings_.micDevice.empty() ? L"기본 장치 · " + d.name : d.name;
    return settings_.micDevice.empty() ? L"기본 장치" : L"연결되지 않은 장치";
}

// The input devices as a native menu under `anchor` (DIPs): Windows' default, then each by name.
bool App::MicMenu(const Rect& anchor) {
    if (!testWindowsOnly_) micDevices_ = AudioCapture::InputDevices();
    auto escape = [](std::wstring s) {
        for (size_t i = 0; (i = s.find(L'&', i)) != std::wstring::npos; i += 2) s.insert(i, 1, L'&');
        return s;
    };
    HMENU menu = CreatePopupMenu();
    std::wstring def = L"Windows 기본 장치";
    for (const auto& d : micDevices_)
        if (d.isDefault) def += L" (" + d.name + L")";
    AppendMenuW(menu, MF_STRING | (settings_.micDevice.empty() ? MF_CHECKED : 0), 1, escape(def).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < micDevices_.size(); ++i)
        AppendMenuW(menu, MF_STRING | (micDevices_[i].id == settings_.micDevice ? MF_CHECKED : 0), 100 + (UINT)i,
                    escape(micDevices_[i].name).c_str());
    if (micDevices_.empty()) AppendMenuW(menu, MF_STRING | MF_GRAYED, 2, L"연결된 마이크가 없습니다");
    POINT pt{(LONG)std::lround(anchor.l * Scale()), (LONG)std::lround((anchor.b + 4) * Scale())};
    ClientToScreen(hwnd_, &pt);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hwnd_,
                              nullptr);
    DestroyMenu(menu);
    std::wstring pick = settings_.micDevice;
    if (cmd == 1) pick.clear();
    if (cmd >= 100 && cmd - 100 < micDevices_.size()) pick = micDevices_[cmd - 100].id;
    if (pick == settings_.micDevice) return false;
    settings_.micDevice = pick;
    return true;
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
    estKey_.clear();
    fitContent_.clear();
    estValid_ = fitting_ = fitMissed_ = false;
    fitBitrate_ = 0;
    fitCorrection_ = 1;
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

// ---------------------------------------------------------------- files

// A video dropped on the window, picked in the open dialog or the recent list, or given on the command line.
void App::OpenMediaFile(const std::wstring& path) {
    ShowMain(true);
    filesOpen_ = false;
    if (rec_ != RecState::Idle) {
        Toast(L"녹화 중에는 파일을 열 수 없습니다.", true);
        return;
    }
    if (exporter_.Busy()) {
        Toast(L"내보내기가 끝난 뒤에 열 수 있습니다.", true);
        return;
    }
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        Toast(L"파일을 찾을 수 없습니다: " + util::FileNameOf(path), true);
        return;
    }
    std::wstring ext = path.substr(std::min(path.size(), path.find_last_of(L'.')));
    for (auto& c : ext) c = (wchar_t)towlower(c);
    if (ext == L".gif" || ext == L".webp" || ext == L".avif" || ext == L".png" || ext == L".apng") {
        Toast(L"움짤은 편집할 수 없습니다. MP4 같은 동영상 파일을 여세요.", true);
        return;
    }
    if (OpenVideo(path)) AddRecent(path);
}

void App::BrowseForVideo() {
    filesOpen_ = false;
    std::wstring path;
    if (util::PickVideoFile(hwnd_, settings_.outDir, path)) OpenMediaFile(path);
}

void App::AddRecent(const std::wstring& path) {
    settings_.AddRecent(path);
    settings_.Save();
}

void App::RefreshRecent() {
    recentFiles_.clear();
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    auto ticks = [](const FILETIME& f) { return (int64_t)((uint64_t)f.dwHighDateTime << 32 | f.dwLowDateTime); };
    for (auto& p : settings_.recent) {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        RecentFile f;
        f.path = p;
        f.bytes = (uint64_t)fa.nFileSizeHigh << 32 | fa.nFileSizeLow;
        f.age = std::max<int64_t>(0, ticks(nowFt) - ticks(fa.ftLastWriteTime)) / 1e7;
        std::wstring ext = p.substr(std::min(p.size(), p.find_last_of(L'.')));
        for (auto& c : ext) c = (wchar_t)towlower(c);
        f.video = ext != L".gif" && ext != L".webp" && ext != L".avif";
        recentFiles_.push_back(std::move(f));
    }
}

void App::HoldRecording(std::shared_ptr<Clip> clip) {
    held_.insert(held_.begin(), {std::move(clip), util::NowSeconds(), recName_});
    // Bounded: each one keeps a temporary file of compressed frames.
    if (held_.size() > kMaxHeld) {
        held_.resize(kMaxHeld);
        Toast(util::Format(L"저장하지 않은 녹화가 %zu개를 넘어 가장 오래된 것을 지웠습니다.", kMaxHeld), true);
    }
}

void App::DropHeld(const Clip* clip) {
    std::erase_if(held_, [&](const HeldClip& h) { return h.clip.get() == clip; });
}

void App::AskQuit(bool restart, std::function<void()> then) {
    if (held_.empty()) {
        then();
        return;
    }
    Ask({util::Format(L"저장하지 않은 녹화가 %zu개 있습니다", held_.size()),
         restart ? L"다시 시작하면 사라집니다. 남기려면 먼저 편집 화면에서 저장하세요."
                 : L"종료하면 사라집니다. 남기려면 먼저 편집 화면에서 저장하세요.",
         restart ? L"다시 시작해서 업데이트" : L"종료", true, std::move(then)});
}

void App::Ask(Question q) {
    if (!IsWindowVisible(hwnd_) || IsIconic(hwnd_)) ShowMain(true);
    SetPlaying(false);
    filesOpen_ = updateOpen_ = false;
    question_ = std::move(q);
    ++questionSeq_;
    Invalidate();
}

// ---------------------------------------------------------------- updates

// From the timer: at startup, then once a day (an hour after a failed check). The settings call it to check now.
void App::CheckForUpdate() {
    Updater::Status st = updater_.Get();
    double since = util::NowSeconds() - st.checkedAt;
    bool due = st.checkedAt == 0 || since >= 24 * 3600 || (st.state == Updater::State::Failed && since >= 3600);
    if (settings_.autoUpdate && due) updater_.Check();
}

bool App::UpdateReady() const { return updater_.Get().state == Updater::State::Ready; }

void App::InstallUpdate() {
    updateOpen_ = false;
    if (rec_ != RecState::Idle || exporter_.Busy()) {
        Toast(L"녹화 · 내보내기가 끝나면 업데이트할 수 있습니다.", true);
        return;
    }
    AskQuit(true, [this] {
        if (rec_ == RecState::Idle && !exporter_.Busy()) RestartIntoUpdate();
    });
}

void App::RestartIntoUpdate() {
    std::wstring err;
    if (!updater_.Install(err)) {
        Toast(err, true);
        return;
    }
    // The new exe waits until this process has ended, then starts as this one did (main.cpp).
    std::wstring exe = util::ExePath();
    std::wstring cmd = util::Format(L"\"%s\" --updated %lu %d", exe.c_str(), GetCurrentProcessId(),
                                    IsWindowVisible(hwnd_) ? 1 : 0);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        Toast(L"새 버전을 시작하지 못했습니다. OzMaker를 다시 켜면 새 버전으로 시작합니다.", true);
        return;
    }
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    quitting_ = true;
    DestroyWindow(hwnd_);
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

// The editor's 버리기 (a 움짤 recording) and 삭제 (the video just recorded, into the Recycle Bin). Only what this run
// recorded: a video opened by hand gets no button. Files exported from the recording stay.
bool App::CanDiscard() const {
    if (!clip_ || exporter_.Busy()) return false;
    const std::wstring& src = clip_->SourcePath();
    return src.empty() || (!lastVideo_.path.empty() && _wcsicmp(src.c_str(), lastVideo_.path.c_str()) == 0);
}

void App::ConfirmDiscard() {
    if (!CanDiscard()) return;
    bool held = std::any_of(held_.begin(), held_.end(), [&](const HeldClip& h) { return h.clip == clip_; });
    // The editor's clip when asked: the answer applies to that one only.
    auto discard = [this, clip = std::weak_ptr<Clip>(clip_)] {
        if (clip.lock() == clip_) DiscardRecording();
    };
    if (!clip_->SourcePath().empty())
        Ask({L"이 동영상을 휴지통으로 옮길까요?",
             util::FileNameOf(clip_->SourcePath()) + L"\n이 녹화에서 따로 저장한 파일은 그대로 남습니다.", L"휴지통으로",
             true, discard});
    else
        Ask({L"이 녹화를 버릴까요?",
             held ? L"아직 저장하지 않아서 되돌릴 수 없습니다." : L"저장한 파일은 그대로 남습니다.", L"버리기", true,
             discard});
}

void App::DiscardRecording() {
    if (!CanDiscard()) return;
    const std::wstring path = clip_->SourcePath();
    CloseEditor();
    StopThumbnails();
    if (!path.empty()) {
        // Nothing may hold the file: the readers went with the editor; a size estimate in flight gets a moment.
        for (double until = util::NowSeconds() + 3; estimator_.Busy() && util::NowSeconds() < until;) Sleep(10);
        bool gone = testDeleteFiles_ ? DeleteFileW(path.c_str()) != 0 : util::MoveToRecycleBin(hwnd_, path);
        if (!gone) {
            AttachReaders();
            screen_ = Screen::Editor;
            Toast(L"동영상을 휴지통으로 옮기지 못했습니다. 다른 프로그램에서 열려 있다면 닫고 다시 해 보세요.", true);
            Invalidate();
            return;
        }
        showVideo_ = false;
        lastVideo_ = {};
        RefreshRecent();
    }
    DropHeld(clip_.get());
    clip_.reset();
    frameBmp_.Reset();
    hasResult_ = resultOriginal_ = false;
    Toast(path.empty() ? L"녹화를 버렸습니다." : L"휴지통으로 옮겼습니다 · " + util::FileNameOf(path));
    Invalidate();
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
    bool want = video && video->HasAudio() && !previewMute_ && settings_.previewVolume > 0 && playing_ &&
                !scrubbing_ && screen_ == Screen::Editor && kSpeedOptions[settings_.speedIdx] == 1.0 && hwnd_ &&
                IsWindowVisible(hwnd_) && !IsIconic(hwnd_) && GetForegroundWindow() == hwnd_;
    if (!want) {
        audioPreview_.Stop();
        return;
    }
    float v = settings_.previewVolume / 100.f;
    audioPreview_.SetGain(v * v);  // squared, so the steps sound about even
    if (audioPreview_.Active() && audioEpoch_ == playEpoch_) return;
    audioEpoch_ = playEpoch_;
    int64_t start = clip_->Ts(inF_), end = RangeEndTs();
    int64_t t = playTs_ + (int64_t)((util::NowSeconds() - playWall_) * 1e7);
    audioPreview_.Start(video, std::clamp(t, start, end - 1), end);
}

// The speaker button. A volume dragged down to 0 comes back at 50 %.
void App::TogglePreviewMute() {
    if (!previewMute_ && settings_.previewVolume > 0) {
        previewMute_ = true;
        return;
    }
    previewMute_ = false;
    if (settings_.previewVolume == 0) {
        settings_.previewVolume = 50;
        settingsDirty_ = true;
    }
}

// The volume slider and the wheel over it; moving it turns the sound back on.
void App::SetPreviewVolume(int v) {
    v = std::clamp(v, 0, 100);
    previewMute_ = false;
    if (v == settings_.previewVolume) return;
    settings_.previewVolume = v;
    settingsDirty_ = true;
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
        if (kLoopOptions[settings_.loopIdx] != 1) {
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

void App::OutputSize(int& w, int& h, int scaleIdx) const {
    int cw = crop_.right - crop_.left, ch = crop_.bottom - crop_.top;
    const ExportSize& o = kExportSizes[scaleIdx < 0 ? settings_.scaleIdx : scaleIdx];
    double s = o.width ? std::min(1.0, o.width / (double)std::max(1, cw)) : o.percent == 33 ? 1.0 / 3.0 : o.percent / 100.0;
    w = std::max(2, (int)std::lround(cw * s) & ~1);
    h = std::max(2, (int)std::lround(ch * s) & ~1);
}

double App::EffectiveOutFps(int outFpsIdx) const {
    double limit = kOutFpsOptions[outFpsIdx < 0 ? settings_.outFpsIdx : outFpsIdx];
    if (fmt_ == (int)Format::Mp4) {
        // Constant rate: the chosen one, or the source's own rounded up to a usual rate (29.4 -> 30).
        if (limit > 0) return limit;
        double avg = !clip_ ? 30.0 : clip_->NominalFps() > 0 ? clip_->NominalFps() : clip_->AverageFps();
        for (double r : {10.0, 15.0, 20.0, 24.0, 25.0, 30.0, 50.0, 60.0, 90.0, 120.0})
            if (avg <= r * 1.03) return r;
        return 120.0;
    }
    // Output frames per second (after the speed change), capped at what the format plays back reliably.
    double maxF = FormatMaxFps((Format)fmt_);
    return limit == 0 || limit > maxF ? maxF : limit;
}

// MP4 keeps the video's sound unless the speed changes (no time-stretching).
bool App::ExportAudio() const {
    auto* video = dynamic_cast<const VideoClip*>(clip_.get());
    return fmt_ == (int)Format::Mp4 && exportAudio_ && video && video->HasAudio() && video->AudioExportable() &&
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
std::wstring App::FileStem(const std::wstring& what) const {
    std::wstring s = settings_.nameTemplate, ts = util::TimestampName();
    std::wstring name = util::SafeFileName(what.empty() ? L"OzMaker" : what, 60);
    if (name.empty()) name = L"OzMaker";
    auto fill = [&](std::wstring_view key, const std::wstring& v) {
        for (size_t p; (p = s.find(key)) != std::wstring::npos;) s.replace(p, key.size(), v);
    };
    fill(L"{날짜}", ts);
    fill(L"{date}", ts);
    fill(L"{창 이름}", name);
    fill(L"{window}", name);
    s = util::SafeFileName(s, 120);
    return s.empty() ? L"OzMaker_" + ts : s;
}

std::wstring App::ExportName(Format f) const {
    std::wstring ext = FormatExt(f), src = clip_ ? clip_->SourcePath() : std::wstring();
    std::wstring stem = FileStem(clipName_);
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
    ExportJob job = MakeJob(settings_.scaleIdx, settings_.outFpsIdx, f == Format::Mp4 ? 0 : settings_.quality[fmt_]);
    job.path = path;
    settings_.Save();
    hasResult_ = resultOriginal_ = false;
    exportEstimate_ = estValid_ && estKey_ == EstimateKey() ? estBytes_ : 0;
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
        DropHeld(clip_.get());  // exported: nothing to protect any more
        AddRecent(r.path);
        if (settings_.sounds) sound::Play(sound::Cue::Saved);
        uint64_t target = (uint64_t)kTargetMB[settings_.targetIdx] * 1'000'000;
        if (target && r.bytes > target) {
            // The guess was low for this clip: learn by how much and fit again, smaller.
            if (exportEstimate_) fitCorrection_ = std::clamp(fitCorrection_ * r.bytes / (double)exportEstimate_, 1.0, 4.0);
            fitContent_.clear();
            Toast(util::Format(L"목표 %dMB보다 커졌습니다(%s). 더 작게 다시 맞췄으니 한 번 더 저장하세요.",
                               kTargetMB[settings_.targetIdx], util::FormatBytes(r.bytes).c_str()),
                  true);
        }
    }
    Invalidate();
}

ExportJob App::MakeJob(int scaleIdx, int outFpsIdx, int quality) const {
    ExportJob job;
    job.clip = clip_;
    job.first = inF_;
    job.last = outF_;
    job.crop = crop_;
    OutputSize(job.outW, job.outH, scaleIdx);
    job.fpsLimit = EffectiveOutFps(outFpsIdx);
    job.speed = kSpeedOptions[settings_.speedIdx];
    job.format = (Format)fmt_;
    job.plays = kLoopOptions[settings_.loopIdx];
    job.holdLastMs = settings_.holdLast ? kHoldLastMs : 0;
    if (job.format == Format::Mp4) {
        job.video = VideoSettings((int)std::lround(job.fpsLimit));
        if (settings_.targetIdx && fitBitrate_) job.video.bitrate = fitBitrate_;
        job.audio.enabled = ExportAudio();
        job.audio.bitrate = kAudioBitrateOptions[settings_.aBitrateIdx] * 1000;
    } else {
        job.quality = quality;
    }
    return job;
}

std::wstring App::ContentKey() const {
    return util::Format(L"%p %zu %zu %d,%d,%d,%d %d %d %d", clip_.get(), inF_, outF_, crop_.left, crop_.top, crop_.right,
                        crop_.bottom, fmt_, settings_.speedIdx, settings_.targetIdx);
}

std::wstring App::EstimateKey() const {
    return ContentKey() + util::Format(L" %d %d %d %d %d %d %d %u %d %d", settings_.scaleIdx, settings_.outFpsIdx,
                                       fmt_ < 3 ? settings_.quality[fmt_] : 0, settings_.holdLast, ExportAudio(),
                                       settings_.vCodec, settings_.vBitrateIdx, fitBitrate_, settings_.aBitrateIdx,
                                       settings_.vFpsIdx);
}

// Called while the export panel is up: refits when what the fit depends on changed, else re-estimates (a moment
// after the last change, so dragging a slider doesn't start one per frame).
void App::UpdateEstimate() {
    if (!clip_ || !FormatAvailable((Format)fmt_)) return;
    double now = util::NowSeconds();
    if (settings_.targetIdx && ContentKey() != fitContent_) {
        StartFit();
        return;
    }
    if (fitting_) return;
    std::wstring key = EstimateKey();
    if (key != estKey_) {
        estKey_ = key;
        estValid_ = false;
        estDue_ = now + 0.35;
    }
    if (estDue_ > 0 && now >= estDue_) {
        estDue_ = 0;
        estimator_.Start({MakeJob(settings_.scaleIdx, settings_.outFpsIdx, fmt_ < 3 ? settings_.quality[fmt_] : 0)}, 0,
                         ++estSerial_, hwnd_, WM_APP_ESTIMATE);
    } else if (estDue_ > 0) {
        RequestWake(estDue_);
    }
}

// Picks size, frame rate and quality (움짤) or the bitrate (MP4) so the export lands under the target. Starts from
// the best and steps the three down in turns, so none of them drops all the way first.
void App::StartFit() {
    fitContent_ = ContentKey();
    fitMissed_ = false;
    const uint64_t target = fitTargetForTest_ ? fitTargetForTest_ : (uint64_t)kTargetMB[settings_.targetIdx] * 1'000'000;
    const Format f = (Format)fmt_;
    // Sizes and frame rates that make a difference here, largest first.
    std::vector<int> sizes, rates;
    for (int i = 0; i < (int)_countof(kExportSizes); ++i) {
        int w, h;
        OutputSize(w, h, i);
        bool dup = false;
        for (int j : sizes) {
            int w2, h2;
            OutputSize(w2, h2, j);
            dup |= w2 == w;
        }
        if (!dup) sizes.push_back(i);
    }
    std::stable_sort(sizes.begin(), sizes.end(), [&](int a, int b) {
        int wa, ha, wb, hb;
        OutputSize(wa, ha, a);
        OutputSize(wb, hb, b);
        return wa > wb;
    });
    // What each frame-rate choice really gives (a cap above the clip's own rate changes nothing).
    const double speed = kSpeedOptions[settings_.speedIdx];
    auto outRate = [&](int i) {
        if (f == Format::Mp4) return EffectiveOutFps(i);
        auto plan = PlanFrames(*clip_, inF_, outF_, EffectiveOutFps(i), speed);
        double ms = 0;
        for (auto& p : plan) ms += p.durationMs;
        return ms > 0 ? plan.size() * 1000.0 / ms : 0.0;
    };
    std::vector<double> rateOf;
    for (int i = 0; i < (int)_countof(kOutFpsOptions); ++i) rateOf.push_back(outRate(i));
    for (int i = 0; i < (int)_countof(kOutFpsOptions); ++i) {
        bool dup = false;
        for (int j : rates) dup |= std::fabs(rateOf[j] - rateOf[i]) < 0.5;
        if (!dup) rates.push_back(i);
    }
    std::stable_sort(rates.begin(), rates.end(), [&](int a, int b) { return rateOf[a] > rateOf[b]; });

    if (f == Format::Mp4) {
        // The bitrate decides the size; shrink the picture only when that bitrate would starve it.
        fitting_ = false;
        const double sec = std::max(0.1, PlanVideoFrames(*clip_, inF_, outF_, EffectiveOutFps(), speed).size() /
                                             EffectiveOutFps());
        double audio = ExportAudio() ? kAudioBitrateOptions[settings_.aBitrateIdx] * 1000.0 : 0;
        double video = std::max(2e5, target * 8 * 0.9 / fitCorrection_ / sec - audio);
        int pick = sizes.front();
        for (int s : sizes) {
            int w, h;
            OutputSize(w, h, s);
            pick = s;
            if (video / ((double)w * h * EffectiveOutFps()) >= 0.03) break;  // bits per pixel still watchable
        }
        settings_.scaleIdx = pick;
        fitBitrate_ = (UINT32)std::min(video, 80e6);
        estBytes_ = EstimateBytes(MakeJob(pick, settings_.outFpsIdx, 0), std::atomic<bool>{false});
        estValid_ = true;
        estKey_ = EstimateKey();
        fitMissed_ = video <= 2e5 + 1;
        settingsDirty_ = true;
        hasResult_ = false;
        return;
    }

    std::vector<int> qualities = {settings_.quality[fmt_]};
    if (f != Format::Gif)
        for (int q : {70, 55, 40})
            if (q < qualities.back()) qualities.push_back(q);
    fitKnobs_.clear();
    std::vector<ExportJob> jobs;
    size_t si = 0, ri = 0, qi = 0;
    auto add = [&] {
        fitKnobs_.push_back({sizes[si], rates[ri], qualities[qi]});
        jobs.push_back(MakeJob(sizes[si], rates[ri], qualities[qi]));
    };
    add();
    // Frame rate, size, quality, frame rate, ... skipping whichever is already at its lowest.
    for (int step = 0, idle = 0; idle < 3; step = (step + 1) % 3) {
        bool moved = (step == 0 && ri + 1 < rates.size() && ++ri) || (step == 1 && si + 1 < sizes.size() && ++si) ||
                     (step == 2 && qi + 1 < qualities.size() && ++qi);
        if (moved) {
            add();
            idle = 0;
        } else {
            ++idle;
        }
    }
    fitting_ = true;
    estValid_ = false;
    estDue_ = 0;
    // Aim a little under: the estimate is a guess.
    estimator_.Start(std::move(jobs), (uint64_t)(target * 0.9 / fitCorrection_), fitSerial_ = ++estSerial_, hwnd_,
                     WM_APP_ESTIMATE);
}

void App::OnEstimate() {
    Estimator::Result r;
    if (!estimator_.Take(r)) return;
    if (r.serial == fitSerial_ && fitting_) {
        fitting_ = false;
        if (r.chosen >= 0 && r.chosen < (int)fitKnobs_.size()) {
            const FitKnobs& k = fitKnobs_[r.chosen];
            settings_.scaleIdx = k.scaleIdx;
            settings_.outFpsIdx = k.outFpsIdx;
            if (fmt_ < 3) settings_.quality[fmt_] = k.quality;
            fitMissed_ = !r.fits;
            settingsDirty_ = true;
            hasResult_ = false;
        }
        estBytes_ = (uint64_t)(r.bytes * fitCorrection_);
        estValid_ = r.chosen >= 0;
        estKey_ = EstimateKey();
    } else if (r.serial == estSerial_) {
        estBytes_ = (uint64_t)(r.bytes * fitCorrection_);
        estValid_ = r.chosen >= 0;
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
    UnregisterHotKey(hwnd_, kHotkeyRecord);
    hotkeyOk_ = RegisterHotKey(hwnd_, kHotkeyRecord, settings_.hkMods | MOD_NOREPEAT, settings_.hkVk) != FALSE;
}

void App::RegisterPauseHotkey() {
    UnregisterHotKey(hwnd_, kHotkeyPause);
    if (rec_ == RecState::Recording && !capturingHotkey_)
        pauseHotkeyOk_ =
            RegisterHotKey(hwnd_, kHotkeyPause, settings_.hkPauseMods | MOD_NOREPEAT, settings_.hkPauseVk) != FALSE;
}

// While a new key is being chosen the current ones are released, so pressing them again reaches the window (and
// doesn't start a recording).
void App::SetCapturingHotkey(int which) {
    if (which == capturingHotkey_) return;
    capturingHotkey_ = which;
    if (which) {
        UnregisterHotKey(hwnd_, kHotkeyRecord);
        UnregisterHotKey(hwnd_, kHotkeyPause);
    } else {
        RegisterRecordHotkey();
        RegisterPauseHotkey();
    }
    Invalidate();
}

std::wstring App::HotkeyLabel() const {
    return hotkeyOk_ ? util::HotkeyToString(settings_.hkMods, settings_.hkVk) : L"단축키 없음";
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
    std::wstring hk = util::HotkeyToString(settings_.hkMods, settings_.hkVk);
    std::wstring tip = rec_ == RecState::Recording && recPaused_ ? L"OzMaker · 일시정지 (" +
                                                                         util::HotkeyToString(settings_.hkPauseMods, settings_.hkPauseVk) + L" 이어서)"
                       : rec_ == RecState::Recording ? L"OzMaker · 녹화 중" + (hotkeyOk_ ? L" (" + hk + L" 중지)" : L"")
                       : hotkeyOk_                  ? L"OzMaker · " + hk + L" 녹화"
                                                    : L"OzMaker · 녹화 단축키를 쓸 수 없음";
    if (rec_ != RecState::Recording && UpdateReady()) tip += L"\n업데이트 준비 완료";
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
    if (rec_ == RecState::Recording)
        AppendMenuW(menu, MF_STRING, 5,
                    ((recPaused_ ? L"이어서 녹화\t" : L"일시정지\t") +
                     util::HotkeyToString(settings_.hkPauseMods, settings_.hkPauseVk))
                        .c_str());
    if (UpdateReady())
        AppendMenuW(menu, MF_STRING | (rec_ == RecState::Idle ? 0 : MF_GRAYED), 4,
                    (L"새 버전 " + updater_.Get().version + L"으로 업데이트").c_str());
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
        case 4:
            ShowMain(true);
            InstallUpdate();
            break;
        case 5: TogglePause(); break;
        case 2:
            if (rec_ == RecState::Idle)
                StartRecording(current, false);
            else
                StopRecording();
            break;
        case 3: AskQuit(false, [this] { PostMessageW(hwnd_, WM_APP_QUIT, 0, 0); }); break;
    }
}
