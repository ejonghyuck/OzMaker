#include "pch.h"
#include "Overlays.h"
#include "Util.h"

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

using namespace theme;

namespace {
float ScaleForRect(const RECT& r) {
    HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    return dx / 96.f;
}
}  // namespace

// ---------------------------------------------------------------- LayeredWindow

bool LayeredWindow::Create(HINSTANCE inst, ID2D1Factory1* factory, const wchar_t* cls, DWORD exStyle, WNDPROC proc,
                           void* user) {
    factory_ = factory;
    WNDCLASSEXW wc{sizeof(wc)};
    if (!GetClassInfoExW(inst, cls, &wc)) {
        wc = {sizeof(wc)};
        wc.lpfnWndProc = proc;
        wc.hInstance = inst;
        wc.lpszClassName = cls;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
    }
    hwnd_ = CreateWindowExW(exStyle | WS_EX_LAYERED, cls, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, user);
    return hwnd_ != nullptr;
}

void LayeredWindow::Destroy() {
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    rt_.Reset();
    if (memDC_) {
        SelectObject(memDC_, oldBmp_);
        DeleteDC(memDC_);
        memDC_ = nullptr;
    }
    if (dib_) DeleteObject(dib_);
    dib_ = nullptr;
    w_ = h_ = 0;
    painter_ = Painter();  // its brush belonged to the released render target
}

bool LayeredWindow::EnsureSurface(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (w == w_ && h == h_ && dib_) return true;
    if (memDC_) {
        SelectObject(memDC_, oldBmp_);
        DeleteDC(memDC_);
        memDC_ = nullptr;
    }
    if (dib_) DeleteObject(dib_);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    dib_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    memDC_ = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!dib_ || !memDC_) return false;
    oldBmp_ = (HBITMAP)SelectObject(memDC_, dib_);
    w_ = w;
    h_ = h;
    if (!rt_) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        if (FAILED(factory_->CreateDCRenderTarget(&props, &rt_))) return false;
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    return true;
}

void LayeredWindow::Render(const RECT& rect, float scale, bool move, const std::function<void(Painter&)>& draw) {
    int w = rect.right - rect.left, h = rect.bottom - rect.top;
    if (!hwnd_ || !EnsureSurface(w, h)) return;
    RECT rc{0, 0, w, h};
    rt_->BindDC(memDC_, &rc);
    rt_->SetDpi(96.f * scale, 96.f * scale);
    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
    painter_.Bind(rt_.Get());
    draw(painter_);
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) {
        rt_.Reset();
        painter_ = Painter();
        w_ = h_ = 0;
        return;
    }
    POINT src{0, 0}, dst{rect.left, rect.top};
    SIZE sz{w, h};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, nullptr, move ? &dst : nullptr, &sz, memDC_, &src, 0, &bf, ULW_ALPHA);
}

// ---------------------------------------------------------------- RecOverlay

void RecOverlay::Init(HINSTANCE inst, ID2D1Factory1* factory) {
    inst_ = inst;
    factory_ = factory;
}

void RecOverlay::Ensure() {
    if (win_.Hwnd()) return;
    win_.Create(inst_, factory_, L"OzMakerOverlay", WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, Proc, this);
    SetWindowDisplayAffinity(win_.Hwnd(), WDA_EXCLUDEFROMCAPTURE);  // never shows up in recordings
}

void RecOverlay::Place(const RECT& target, int wDip, int hDip, bool center, bool outside) {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&target, MONITOR_DEFAULTTONEAREST), &mi);
    RECT vis;
    if (!IntersectRect(&vis, &target, &mi.rcWork)) vis = mi.rcWork;
    scale_ = ScaleForRect(vis);
    int w = (int)std::lround(wDip * scale_), h = (int)std::lround(hDip * scale_);
    int gap = (int)std::lround(10 * scale_);
    int x = (vis.left + vis.right - w) / 2;
    int y = vis.top + (int)std::lround(14 * scale_);
    if (center) {
        y = (vis.top + vis.bottom - h) / 2;
    } else if (outside) {
        // Above the region, else below it, else inside its top edge.
        if (target.top - gap - h >= mi.rcWork.top)
            y = target.top - gap - h;
        else if (target.bottom + gap + h <= mi.rcWork.bottom)
            y = target.bottom + gap;
    }
    rect_ = {x, y, x + w, y + h};
}

void RecOverlay::ShowCountdown(const RECT& target, int value) {
    Ensure();
    if (mode_ != Mode::Countdown) Place(target, 132, 132, true, false);
    mode_ = Mode::Countdown;
    countdown_ = value;
    Redraw(true);
    ShowWindow(win_.Hwnd(), SW_SHOWNOACTIVATE);
}

namespace {
constexpr float kPillW = 228, kPillH = 40;
}

void RecOverlay::ShowPill(const RECT& target, bool outside, std::function<void()> onStop,
                          std::function<void()> onPause) {
    Ensure();
    onStop_ = std::move(onStop);
    onPause_ = std::move(onPause);
    mode_ = Mode::Pill;
    elapsed_ = 0;
    frames_ = 0;
    hover_ = 0;
    paused_ = false;
    Place(target, (int)kPillW, (int)kPillH, false, outside);
    Redraw(true);
    ShowWindow(win_.Hwnd(), SW_SHOWNOACTIVATE);
}

void RecOverlay::ShowFrame(const RECT& a) {
    // Four thin click-through bars just outside the area, so they never end up in the recording.
    static const wchar_t* kCls = L"OzMakerFrame";
    WNDCLASSEXW wc{sizeof(wc)};
    if (!GetClassInfoExW(inst_, kCls, &wc)) {
        wc = {sizeof(wc)};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = inst_;
        wc.lpszClassName = kCls;
        wc.hbrBackground = CreateSolidBrush(RGB(0x8B, 0x6C, 0xFF));
        RegisterClassExW(&wc);
    }
    int t = std::max(2, (int)std::lround(2 * ScaleForRect(a)));
    RECT bars[4] = {{a.left - t, a.top - t, a.right + t, a.top},
                    {a.left - t, a.bottom, a.right + t, a.bottom + t},
                    {a.left - t, a.top, a.left, a.bottom},
                    {a.right, a.top, a.right + t, a.bottom}};
    for (int i = 0; i < 4; ++i) {
        if (!frame_[i]) {
            frame_[i] = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                                            WS_EX_NOACTIVATE,
                                        kCls, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst_, nullptr);
            SetLayeredWindowAttributes(frame_[i], 0, 220, LWA_ALPHA);
            SetWindowDisplayAffinity(frame_[i], WDA_EXCLUDEFROMCAPTURE);
        }
        const RECT& b = bars[i];
        SetWindowPos(frame_[i], HWND_TOPMOST, b.left, b.top, b.right - b.left, b.bottom - b.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

void RecOverlay::Update(double elapsed, size_t frames) {
    if (mode_ != Mode::Pill) return;
    elapsed_ = elapsed;
    frames_ = frames;
    Redraw(false);
}

void RecOverlay::SetPaused(bool paused) {
    if (paused_ == paused) return;
    paused_ = paused;
    Redraw(false);
}

void RecOverlay::Hide() {
    mode_ = Mode::None;
    onStop_ = nullptr;
    onPause_ = nullptr;
    if (win_.Hwnd()) ShowWindow(win_.Hwnd(), SW_HIDE);
    for (HWND& f : frame_) {
        if (f) DestroyWindow(f);
        f = nullptr;
    }
}

Rect RecOverlay::StopRect() const { return Rect::Centered(kPillW - 21, kPillH / 2, 28, 28); }
Rect RecOverlay::PauseRect() const { return Rect::Centered(kPillW - 53, kPillH / 2, 28, 28); }

int RecOverlay::ButtonAt(float x, float y) const {
    if (PauseRect().Inset(-2).Contains(x, y) && onPause_) return 1;
    if (StopRect().Inset(-2).Contains(x, y)) return 2;
    return 0;
}

void RecOverlay::Redraw(bool move) {
    if (mode_ == Mode::None) return;
    // Keep the current position if the user dragged the pill.
    if (!move) GetWindowRect(win_.Hwnd(), &rect_);
    win_.Render(rect_, scale_, move, [&](Painter& p) { Paint(p); });
}

void RecOverlay::PaintTest(Painter& p, double elapsed, bool paused, int hover) {
    mode_ = Mode::Pill;
    elapsed_ = elapsed;
    paused_ = paused;
    hover_ = hover;
    onPause_ = [] {};
    Paint(p);
    mode_ = Mode::None;
    onPause_ = nullptr;
}

void RecOverlay::Paint(Painter& p) {
    {
        if (mode_ == Mode::Countdown) {
            p.Circle(66, 66, 62, Hex(0x111318, 0.88f));
            p.Ring(66, 66, 62, WithAlpha(White, 0.10f), 1.f);
            TextStyle st;
            st.h = AlignCenter;
            st.weight = kBold;
            p.Text(Rect(0, 0, 132, 124), std::to_wstring(countdown_), 58.f, White, st);
            TextStyle sub;
            sub.h = AlignCenter;
            p.Text(Rect(0, 86, 132, 112), L"녹화 준비", 12.f, TextDim, sub);
            return;
        }
        Rect r(0.5f, 0.5f, kPillW - 0.5f, kPillH - 0.5f);
        p.Fill(r, Hex(0x111318, 0.92f), 20);
        p.Stroke(r, WithAlpha(paused_ ? Warning : White, paused_ ? 0.35f : 0.09f), 20);
        TextStyle label;
        label.weight = kBold;
        if (paused_) {
            p.Fill(Rect::XYWH(14, 14, 3.5f, 12), Warning, 1);
            p.Fill(Rect::XYWH(20.5f, 14, 3.5f, 12), Warning, 1);
            p.Text(Rect(32, 0, 90, 40), L"일시정지", 11.5f, Warning, label);
        } else {
            float pulse = 0.55f + 0.45f * (float)std::cos(elapsed_ * 3.1415926 * 1.6);
            p.Circle(19, 20, 9, WithAlpha(Rec, 0.18f * pulse));
            p.Circle(19, 20, 5, Rec);
            p.Text(Rect(32, 0, 90, 40), L"REC", 11.5f, Rec, label);
        }
        TextStyle tm;
        tm.weight = kSemibold;
        tm.tabular = true;
        p.Text(Rect(paused_ ? 88.f : 66.f, 0, kPillW - 68, 40), util::FormatClock(elapsed_, true), 15.f,
               paused_ ? TextDim : White, tm);
        // Pause / resume, then stop.
        Rect pr = PauseRect();
        p.Circle(pr.CX(), pr.CY(), 14, WithAlpha(White, hover_ == 1 ? 0.22f : 0.10f));
        D2D1_COLOR_F ink = hover_ == 1 ? White : Hex(0xE6E8EE);
        if (paused_) {
            p.Icon(pr.Offset(1, 0), 0xE768 /*Play*/, 11.f, ink);
        } else {
            p.Fill(Rect::XYWH(pr.CX() - 4.5f, pr.CY() - 5.5f, 3, 11), ink, 1);
            p.Fill(Rect::XYWH(pr.CX() + 1.5f, pr.CY() - 5.5f, 3, 11), ink, 1);
        }
        Rect sr = StopRect();
        p.Circle(sr.CX(), sr.CY(), 14, WithAlpha(White, hover_ == 2 ? 0.22f : 0.10f));
        p.Fill(Rect::Centered(sr.CX(), sr.CY(), 10, 10), hover_ == 2 ? White : Hex(0xE6E8EE), 2);
    }
}

LRESULT CALLBACK RecOverlay::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
    auto* self = (RecOverlay*)GetWindowLongPtrW(h, GWLP_USERDATA);
    return self ? self->Handle(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT RecOverlay::Handle(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto toDip = [&](LPARAM lp, bool screen) {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (screen) ScreenToClient(h, &pt);
        return std::pair<float, float>{pt.x / scale_, pt.y / scale_};
    };
    switch (m) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST: {
            if (mode_ != Mode::Pill) return HTTRANSPARENT;
            auto [x, y] = toDip(l, true);
            return ButtonAt(x, y) ? HTCLIENT : HTCAPTION;
        }
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, hover_ ? IDC_HAND : IDC_SIZEALL));
            return TRUE;
        case WM_MOUSEMOVE: {
            auto [x, y] = toDip(l, false);
            int hov = ButtonAt(x, y);
            if (!tracking_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                TrackMouseEvent(&tme);
                tracking_ = true;
            }
            if (hov != hover_) {
                hover_ = hov;
                Redraw(false);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            if (hover_) {
                hover_ = 0;
                Redraw(false);
            }
            return 0;
        case WM_LBUTTONUP: {
            auto [x, y] = toDip(l, false);
            int b = ButtonAt(x, y);
            auto cb = b == 1 ? onPause_ : b == 2 ? onStop_ : nullptr;
            if (cb) cb();
            return 0;
        }
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- PickerOverlay

void PickerOverlay::Init(HINSTANCE inst, ID2D1Factory1* factory) {
    inst_ = inst;
    factory_ = factory;
}

void PickerOverlay::Start(std::vector<WinInfo> candidates, std::function<void(HWND)> done) {
    cands_ = std::move(candidates);
    done_ = std::move(done);
    hover_ = -1;
    virt_ = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
    virt_.right = virt_.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    virt_.bottom = virt_.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (!win_.Hwnd())
        win_.Create(inst_, factory_, L"OzMakerPicker", WS_EX_TOPMOST | WS_EX_TOOLWINDOW, Proc, this);
    active_ = true;
    GetCursorPos(&cursor_);
    OnMove(cursor_);
    Redraw();
    ShowWindow(win_.Hwnd(), SW_SHOW);
    SetForegroundWindow(win_.Hwnd());
    SetFocus(win_.Hwnd());
}

void PickerOverlay::Finish(HWND result) {
    if (!active_) return;
    active_ = false;
    ShowWindow(win_.Hwnd(), SW_HIDE);
    win_.Destroy();  // releases the large screen-sized surface
    auto cb = std::move(done_);
    done_ = nullptr;
    if (cb) cb(result);
}

void PickerOverlay::OnMove(POINT pt) {
    cursor_ = pt;
    int hit = -1;
    for (size_t i = 0; i < cands_.size(); ++i) {
        const WinInfo& w = cands_[i];
        if (w.minimized) continue;
        if (PtInRect(&w.frame, pt)) {
            hit = (int)i;
            break;
        }
    }
    if (hit != hover_) {
        hover_ = hit;
        if (active_) Redraw();
    }
}

void PickerOverlay::Redraw() {
    float scale = ScaleForRect(RECT{cursor_.x, cursor_.y, cursor_.x + 1, cursor_.y + 1});
    float vw = (virt_.right - virt_.left) / scale, vh = (virt_.bottom - virt_.top) / scale;
    auto toDip = [&](const RECT& r) {
        return Rect((r.left - virt_.left) / scale, (r.top - virt_.top) / scale, (r.right - virt_.left) / scale,
                    (r.bottom - virt_.top) / scale);
    };
    win_.Render(virt_, scale, true, [&](Painter& p) {
        const D2D1_COLOR_F dim = Hex(0x05060A, 0.50f);
        Rect all(0, 0, vw, vh);
        if (hover_ < 0) {
            p.Fill(all, dim);
        } else {
            const WinInfo& w = cands_[hover_];
            Rect r = toDip(w.frame);
            p.Fill(Rect(all.l, all.t, all.r, r.t), dim);
            p.Fill(Rect(all.l, r.b, all.r, all.b), dim);
            p.Fill(Rect(all.l, r.t, r.l, r.b), dim);
            p.Fill(Rect(r.r, r.t, all.r, r.b), dim);
            p.Fill(r, Hex(0x000000, 0.004f));  // keeps the hole clickable
            p.Stroke(r.Inset(1.5f), Accent, 0, 3.f);

            std::wstring dims = util::Format(L"%d × %d", w.frame.right - w.frame.left, w.frame.bottom - w.frame.top);
            float tw = std::min(380.f, p.Measure(w.title, 13.f, kSemibold));
            float dw = p.Measure(dims, 12.f);
            Rect label = Rect::XYWH(r.l + 10, r.t + 10, tw + dw + 36, 30);
            if (label.r > r.r - 6) label.r = std::max(label.l + 80, r.r - 6);
            p.Fill(label, Accent, 8);
            TextStyle ts;
            ts.weight = kSemibold;
            p.Text(Rect(label.l + 12, label.t, label.r - dw - 22, label.b), w.title, 13.f, White, ts);
            p.Text(Rect(label.r - dw - 12, label.t, label.r - 8, label.b), dims, 12.f, WithAlpha(White, 0.8f));
        }
        // Instruction pill on the monitor under the cursor.
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint(cursor_, MONITOR_DEFAULTTONEAREST), &mi);
        Rect mon = toDip(mi.rcWork);
        std::wstring msg = L"녹화할 창을 클릭하세요";
        std::wstring hint = L"Esc · 취소";
        float mw = p.Measure(msg, 14.f, kSemibold) + p.Measure(hint, 12.f) + 56;
        Rect pill = Rect::XYWH(mon.CX() - mw / 2, mon.t + 24, mw, 40);
        p.Fill(pill, Hex(0x111318, 0.95f), 20);
        p.Stroke(pill.Inset(0.5f), WithAlpha(White, 0.1f), 20);
        p.Crosshair(pill.l + 22, pill.CY(), 16, Accent);
        TextStyle ts;
        ts.weight = kSemibold;
        p.Text(Rect(pill.l + 38, pill.t, pill.r, pill.b), msg, 14.f, White, ts);
        TextStyle hs;
        hs.h = AlignEnd;
        p.Text(Rect(pill.l, pill.t, pill.r - 16, pill.b), hint, 12.f, TextDim, hs);
    });
}

LRESULT CALLBACK PickerOverlay::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
    auto* self = (PickerOverlay*)GetWindowLongPtrW(h, GWLP_USERDATA);
    return self ? self->Handle(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT PickerOverlay::Handle(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;
        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ClientToScreen(h, &pt);
            OnMove(pt);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ClientToScreen(h, &pt);
            OnMove(pt);
            HWND hit = hover_ >= 0 ? cands_[hover_].hwnd : nullptr;
            if (hit) Finish(hit);
            return 0;
        }
        case WM_RBUTTONDOWN:
            Finish(nullptr);
            return 0;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) Finish(nullptr);
            return 0;
        case WM_KILLFOCUS:
        case WM_ACTIVATEAPP:
            if (m == WM_KILLFOCUS || !w) {
                // Alt-Tab away cancels picking.
                if (active_ && GetForegroundWindow() != h) PostMessageW(h, WM_KEYDOWN, VK_ESCAPE, 0);
            }
            break;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- RegionPicker

namespace {
struct Ratio {
    const wchar_t* label;
    double value;  // width / height, 0 = free
};
// Same choices, same order as the editor's crop bar.
constexpr Ratio kRatios[] = {{L"자유", 0}, {L"1:1", 1}, {L"4:3", 4.0 / 3}, {L"16:9", 16.0 / 9}, {L"9:16", 9.0 / 16}};
constexpr int kMinSide = 16;
}  // namespace

void RegionPicker::Init(HINSTANCE inst, ID2D1Factory1* factory) {
    inst_ = inst;
    factory_ = factory;
}

void RegionPicker::Start(std::vector<WinInfo> windows, std::function<void(bool, RECT)> done) {
    cands_ = std::move(windows);
    done_ = std::move(done);
    hover_ = -1;
    pressed_ = dragging_ = adjusting_ = false;
    op_ = -1;
    ratio_ = 0;
    virt_ = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
    virt_.right = virt_.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    virt_.bottom = virt_.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (!win_.Hwnd())
        win_.Create(inst_, factory_, L"OzMakerRegion", WS_EX_TOPMOST | WS_EX_TOOLWINDOW, Proc, this);
    active_ = true;
    GetCursorPos(&cursor_);
    OnMove(cursor_);
    Redraw();
    ShowWindow(win_.Hwnd(), SW_SHOW);
    SetForegroundWindow(win_.Hwnd());
    SetFocus(win_.Hwnd());
}

void RegionPicker::StartAdjust(RECT area) {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&area, MONITOR_DEFAULTTONEAREST), &mi);
    anchorMon_ = mi.rcMonitor;
    adjusting_ = true;
    op_ = -1;
    sel_ = KeepOnMonitor(area);
    if (ratio_) SetRatio(ratio_);
    Redraw();
}

RECT RegionPicker::KeepOnMonitor(RECT r) const {
    const RECT& m = anchorMon_;
    int w = std::min(r.right - r.left, m.right - m.left), h = std::min(r.bottom - r.top, m.bottom - m.top);
    int x = std::clamp((int)r.left, (int)m.left, (int)m.right - w), y = std::clamp((int)r.top, (int)m.top, (int)m.bottom - h);
    return {x, y, x + w, y + h};
}

// Keeps the centre and the width where the screen allows, else the largest size of that shape that fits.
void RegionPicker::SetRatio(int index) {
    ratio_ = index;
    double k = kRatios[index].value;
    if (k <= 0) return;
    const RECT& m = anchorMon_;
    double w = sel_.right - sel_.left, h = w / k;
    double mw = m.right - m.left, mh = m.bottom - m.top;
    if (h > mh) h = mh, w = h * k;
    if (w > mw) w = mw, h = w / k;
    if (h < kMinSide) h = kMinSide, w = h * k;
    if (w < kMinSide) w = kMinSide, h = w / k;
    int cx = (sel_.left + sel_.right) / 2, cy = (sel_.top + sel_.bottom) / 2;
    int iw = (int)std::lround(w), ih = (int)std::lround(h);
    sel_ = KeepOnMonitor({cx - iw / 2, cy - ih / 2, cx - iw / 2 + iw, cy - ih / 2 + ih});
}

int RegionPicker::HitTest(POINT pt) const {
    const RECT& r = sel_;
    const int g = (int)std::lround(9 * scale_);
    bool nearL = std::abs(pt.x - r.left) <= g, nearR = std::abs(pt.x - r.right) <= g;
    bool nearT = std::abs(pt.y - r.top) <= g, nearB = std::abs(pt.y - r.bottom) <= g;
    bool inX = pt.x > r.left - g && pt.x < r.right + g, inY = pt.y > r.top - g && pt.y < r.bottom + g;
    if (nearT && nearL) return 0;
    if (nearT && nearR) return 2;
    if (nearB && nearR) return 4;
    if (nearB && nearL) return 6;
    if (nearT && inX) return 1;
    if (nearR && inY) return 3;
    if (nearB && inX) return 5;
    if (nearL && inY) return 7;
    return PtInRect(&r, pt) ? 8 : -1;
}

void RegionPicker::DragTo(POINT pt) {
    const RECT& m = anchorMon_;
    const RECT& s = opSel_;
    int dx = pt.x - opStart_.x, dy = pt.y - opStart_.y;
    if (op_ == 8) {
        sel_ = KeepOnMonitor({s.left + dx, s.top + dy, s.right + dx, s.bottom + dy});
        return;
    }
    const bool left = op_ == 0 || op_ == 6 || op_ == 7, right = op_ == 2 || op_ == 3 || op_ == 4;
    const bool top = op_ == 0 || op_ == 1 || op_ == 2, bottom = op_ == 4 || op_ == 5 || op_ == 6;
    double k = kRatios[ratio_].value;
    if (k <= 0) {
        RECT r = s;
        if (left) r.left = std::clamp(s.left + dx, m.left, s.right - kMinSide);
        if (right) r.right = std::clamp(s.right + dx, s.left + kMinSide, m.right);
        if (top) r.top = std::clamp(s.top + dy, m.top, s.bottom - kMinSide);
        if (bottom) r.bottom = std::clamp(s.bottom + dy, s.top + kMinSide, m.bottom);
        sel_ = r;
        return;
    }
    // Fixed shape: the side opposite the handle stays put (the middle of it, for an edge handle).
    double ax = left ? s.right : right ? s.left : (s.left + s.right) / 2.0;
    double ay = top ? s.bottom : bottom ? s.top : (s.top + s.bottom) / 2.0;
    double w, h;
    if ((left || right) && (top || bottom)) {
        w = std::max(std::abs(pt.x - ax), std::abs(pt.y - ay) * k);
    } else if (left || right) {
        w = std::abs(pt.x - ax);
    } else {
        w = std::abs(pt.y - ay) * k;
    }
    // Room on the screen from the fixed side (both ways from the middle).
    double roomW = left ? ax - m.left : right ? m.right - ax : 2 * std::min(ax - m.left, m.right - ax);
    double roomH = top ? ay - m.top : bottom ? m.bottom - ay : 2 * std::min(ay - m.top, m.bottom - ay);
    w = std::clamp(w, std::max<double>(kMinSide, kMinSide * k), std::min(roomW, roomH * k));
    h = w / k;
    double l = left ? ax - w : right ? ax : ax - w / 2, t = top ? ay - h : bottom ? ay : ay - h / 2;
    sel_ = {(LONG)std::lround(l), (LONG)std::lround(t), (LONG)std::lround(l + w), (LONG)std::lround(t + h)};
}

void RegionPicker::Finish(bool ok, RECT area) {
    if (!active_) return;
    active_ = false;
    adjusting_ = false;
    if (GetCapture() == win_.Hwnd()) ReleaseCapture();
    ShowWindow(win_.Hwnd(), SW_HIDE);
    win_.Destroy();
    auto cb = std::move(done_);
    done_ = nullptr;
    if (cb) cb(ok, area);
}

RECT RegionPicker::Selection() const {
    RECT r{std::min(anchor_.x, cursor_.x), std::min(anchor_.y, cursor_.y), std::max(anchor_.x, cursor_.x) + 1,
           std::max(anchor_.y, cursor_.y) + 1};
    RECT out;
    if (!IntersectRect(&out, &r, &anchorMon_)) out = {};
    return out;
}

RECT RegionPicker::HoverArea() const {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(cursor_, MONITOR_DEFAULTTONEAREST), &mi);
    if (hover_ < 0) return mi.rcMonitor;
    RECT out;
    if (!IntersectRect(&out, &cands_[hover_].frame, &mi.rcMonitor)) out = mi.rcMonitor;
    return out;
}

void RegionPicker::OnMove(POINT pt) {
    cursor_ = pt;
    if (adjusting_) {
        if (op_ >= 0) DragTo(pt);
        if (active_) Redraw();
        return;
    }
    if (pressed_ && !dragging_ && (std::abs(pt.x - anchor_.x) > 4 || std::abs(pt.y - anchor_.y) > 4)) dragging_ = true;
    int hit = hover_;
    if (!pressed_) {
        hit = -1;
        for (size_t i = 0; i < cands_.size(); ++i) {
            const WinInfo& w = cands_[i];
            if (!w.minimized && PtInRect(&w.frame, pt)) {
                hit = (int)i;
                break;
            }
        }
    }
    hover_ = hit;
    // The guide lines follow the cursor, so every move redraws.
    if (active_) Redraw();
}

void RegionPicker::Redraw() {
    POINT at = adjusting_ ? POINT{(sel_.left + sel_.right) / 2, (sel_.top + sel_.bottom) / 2} : cursor_;
    scale_ = ScaleForRect(RECT{at.x, at.y, at.x + 1, at.y + 1});
    win_.Render(virt_, scale_, true, [&](Painter& p) { Paint(p, scale_); });
}

void RegionPicker::PaintTest(Painter& p, const RECT& screen, POINT anchor, POINT cursor, bool adjust) {
    virt_ = screen;
    anchor_ = anchor;
    cursor_ = cursor;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &mi);
    anchorMon_ = mi.rcMonitor;
    hover_ = -1;
    if (adjust) {
        adjusting_ = true;
        sel_ = Selection();
        SetRatio(3);  // 16:9
        cursor_ = {(sel_.left + sel_.right) / 2, (sel_.top + sel_.bottom) / 2};
    } else {
        pressed_ = dragging_ = true;
    }
    scale_ = 1;
    Paint(p, 1.f);
    pressed_ = dragging_ = adjusting_ = false;
}

void RegionPicker::Paint(Painter& p, float scale) {
    float vw = (virt_.right - virt_.left) / scale, vh = (virt_.bottom - virt_.top) / scale;
    auto toDip = [&](const RECT& r) {
        return Rect((r.left - virt_.left) / scale, (r.top - virt_.top) / scale, (r.right - virt_.left) / scale,
                    (r.bottom - virt_.top) / scale);
    };
    const D2D1_COLOR_F dim = Hex(0x05060A, 0.55f);
    Rect all(0, 0, vw, vh);
    bool hole = dragging_ || hover_ >= 0 || adjusting_;
    RECT area = dragging_ ? Selection() : adjusting_ ? sel_ : HoverArea();
    if (!hole) {
        p.Fill(all, dim);
    } else {
        Rect r = toDip(area);
        p.Fill(Rect(all.l, all.t, all.r, r.t), dim);
        p.Fill(Rect(all.l, r.b, all.r, all.b), dim);
        p.Fill(Rect(all.l, r.t, r.l, r.b), dim);
        p.Fill(Rect(r.r, r.t, all.r, r.b), dim);
        p.Fill(r, Hex(0x000000, 0.004f));  // keeps the hole clickable
        p.Stroke(r.Inset(-1.f), dragging_ || adjusting_ ? Accent : WithAlpha(Accent, 0.85f), 0, 2.f);
        if (adjusting_) {
            // Thirds as a framing aid, and the handles.
            for (int i = 1; i < 3; ++i) {
                p.Line(r.l + r.W() * i / 3, r.t, r.l + r.W() * i / 3, r.b, WithAlpha(White, 0.18f), 1.f);
                p.Line(r.l, r.t + r.H() * i / 3, r.r, r.t + r.H() * i / 3, WithAlpha(White, 0.18f), 1.f);
            }
            float hx[] = {r.l, r.CX(), r.r, r.r, r.r, r.CX(), r.l, r.l};
            float hy[] = {r.t, r.t, r.t, r.CY(), r.b, r.b, r.b, r.CY()};
            int hot = op_ >= 0 ? op_ : HitTest(cursor_);
            for (int i = 0; i < 8; ++i) {
                float s = i == hot ? 6.f : 4.5f;
                Rect hr = Rect::Centered(hx[i], hy[i], s * 2, s * 2);
                p.Fill(hr, White, 2);
                p.Stroke(hr, Accent, 2, 1.5f);
            }
        }

        // Size in physical pixels: exactly what will be recorded.
        std::wstring dims = util::Format(L"%d × %d", area.right - area.left, area.bottom - area.top);
        std::wstring title = dragging_ || adjusting_ ? L"" : cands_[hover_].title;
        TextStyle ts;
        ts.weight = kSemibold;
        ts.tabular = true;
        float tw = title.empty() ? 0 : std::min(320.f, p.Measure(title, 13.f, kSemibold)) + 12;
        float dw = p.Measure(dims, 13.f, kSemibold);
        float lw = tw + dw + 24;
        Rect label = Rect::XYWH(r.l, r.t - 36, lw, 30);
        if (label.t < 4) label = Rect::XYWH(r.l + 8, r.t + 8, lw, 30);
        if (label.r > vw - 4) label = label.Offset(vw - 4 - label.r, 0);
        p.Fill(label, Accent, 8);
        if (!title.empty()) p.Text(Rect(label.l + 12, label.t, label.l + tw, label.b), title, 13.f, White, ts);
        p.Text(Rect(label.l + 12 + tw, label.t, label.r - 8, label.b), dims, 13.f,
               title.empty() ? White : WithAlpha(White, 0.8f), ts);

        if (adjusting_) {
            // Toolbar below the area (inside it when the screen ends there): ratios, cancel, confirm.
            Rect mon = toDip(anchorMon_);
            const float bh = 34, rw = 54, cw = 64, ow = 84, gap = 10;
            float tw2 = rw * 5 + gap + cw + 6 + ow + 12;
            float ty = r.b + 12;
            if (ty + bh + 12 > mon.b) ty = std::max(r.t + 12, r.b - bh - 12);
            Rect bar = Rect::XYWH(std::clamp(r.CX() - tw2 / 2, mon.l + 8, mon.r - 8 - tw2), ty, tw2, bh + 12);
            p.Fill(bar.Offset(0, 3), Hex(0x000000, 0.35f), 12);
            p.Fill(bar, Hex(0x15181E, 0.97f), 12);
            p.Stroke(bar.Inset(0.5f), WithAlpha(White, 0.1f), 12);
            Rect slots[7];
            float x = bar.l + 6;
            for (int i = 0; i < 5; ++i, x += rw) slots[i] = Rect::XYWH(x, bar.t + 6, rw, bh);
            x += gap;
            slots[5] = Rect::XYWH(x, bar.t + 6, cw, bh);
            slots[6] = Rect::XYWH(x + cw + 6, bar.t + 6, ow, bh);
            float cx = (cursor_.x - virt_.left) / scale, cy = (cursor_.y - virt_.top) / scale;
            TextStyle cs;
            cs.h = AlignCenter;
            cs.weight = kSemibold;
            for (int i = 0; i < 7; ++i) {
                const Rect& b = slots[i];
                bool hot = op_ < 0 && b.Contains(cx, cy);
                if (i == 6)
                    p.Fill(b, hot ? AccentHi : Accent, 8);
                else if (i == ratio_)
                    p.Fill(b, Hex(0x2C3240), 8);
                else if (hot)
                    p.Fill(b, Hex(0x252A34), 8);
                const wchar_t* text = i < 5 ? kRatios[i].label : i == 5 ? L"취소" : L"확인";
                p.Text(b, text, 12.5f, i == 6 || i == ratio_ || hot ? White : TextDim, cs);
                buttons_[i] = {(LONG)std::lround(b.l * scale + virt_.left), (LONG)std::lround(b.t * scale + virt_.top),
                               (LONG)std::lround(b.r * scale + virt_.left), (LONG)std::lround(b.b * scale + virt_.top)};
            }
        }
    }
    if (!dragging_ && !adjusting_) {
        // Guide lines through the cursor help line up the first corner.
        float cx = (cursor_.x - virt_.left) / scale, cy = (cursor_.y - virt_.top) / scale;
        p.Line(0, cy, vw, cy, WithAlpha(White, 0.22f), 1.f);
        p.Line(cx, 0, cx, vh, WithAlpha(White, 0.22f), 1.f);
    }

    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(cursor_, MONITOR_DEFAULTTONEAREST), &mi);
    Rect mon = toDip(mi.rcWork);
    std::wstring msg = adjusting_ ? L"끌어서 옮기거나 모서리로 크기를 조절하세요" : L"드래그해서 녹화할 영역을 지정하세요";
    std::wstring hint = adjusting_ ? L"Enter · 확인   Esc · 취소" : L"클릭 · 창 전체   Esc · 취소";
    float mw = p.Measure(msg, 14.f, kSemibold) + p.Measure(hint, 12.f) + 64;
    Rect pill = Rect::XYWH(mon.CX() - mw / 2, mon.t + 24, mw, 40);
    p.Fill(pill, Hex(0x111318, 0.95f), 20);
    p.Stroke(pill.Inset(0.5f), WithAlpha(White, 0.1f), 20);
    p.Stroke(Rect::Centered(pill.l + 22, pill.CY(), 14, 11), Accent, 2, 1.6f);
    TextStyle ts;
    ts.weight = kSemibold;
    p.Text(Rect(pill.l + 38, pill.t, pill.r, pill.b), msg, 14.f, White, ts);
    TextStyle hs;
    hs.h = AlignEnd;
    p.Text(Rect(pill.l, pill.t, pill.r - 16, pill.b), hint, 12.f, TextDim, hs);
}

LRESULT CALLBACK RegionPicker::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
    auto* self = (RegionPicker*)GetWindowLongPtrW(h, GWLP_USERDATA);
    return self ? self->Handle(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT RegionPicker::Handle(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto screenPt = [&] {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ClientToScreen(h, &pt);
        return pt;
    };
    switch (m) {
        case WM_SETCURSOR: {
            LPCWSTR c = IDC_CROSS;
            if (adjusting_ && !pressed_) {
                POINT pt;
                GetCursorPos(&pt);
                int hit = op_ >= 0 ? op_ : HitTest(pt);
                for (const RECT& b : buttons_)
                    if (op_ < 0 && PtInRect(&b, pt)) hit = 9;
                static const LPCWSTR byHit[] = {IDC_SIZENWSE, IDC_SIZENS, IDC_SIZENESW, IDC_SIZEWE, IDC_SIZENWSE,
                                                IDC_SIZENS,   IDC_SIZENESW, IDC_SIZEWE, IDC_SIZEALL, IDC_HAND};
                if (hit >= 0) c = byHit[hit];
            }
            SetCursor(LoadCursorW(nullptr, c));
            return TRUE;
        }
        case WM_MOUSEMOVE:
            OnMove(screenPt());
            return 0;
        case WM_LBUTTONDOWN: {
            POINT pt = screenPt();
            if (adjusting_) {
                for (int i = 0; i < 7; ++i)
                    if (PtInRect(&buttons_[i], pt)) {
                        if (i < 5) {
                            SetRatio(i);
                            Redraw();
                        } else {
                            Finish(i == 6, sel_);
                        }
                        return 0;
                    }
                int hit = HitTest(pt);
                if (hit >= 0) {
                    // Double-click inside confirms.
                    DWORD now = GetTickCount();
                    if (hit == 8 && now - lastClick_ <= GetDoubleClickTime()) {
                        Finish(true, sel_);
                        return 0;
                    }
                    lastClick_ = hit == 8 ? now : 0;
                    SetCapture(h);
                    op_ = hit;
                    opStart_ = pt;
                    opSel_ = sel_;
                    return 0;
                }
                adjusting_ = false;  // outside: draw a new area
            }
            SetCapture(h);
            pressed_ = true;
            dragging_ = false;
            anchor_ = pt;
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
            anchorMon_ = mi.rcMonitor;  // a region can't span monitors (one capture per monitor)
            OnMove(pt);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (adjusting_ && op_ >= 0) {
                OnMove(screenPt());
                op_ = -1;
                ReleaseCapture();
                Redraw();
                return 0;
            }
            if (!pressed_) return 0;
            OnMove(screenPt());
            pressed_ = false;
            ReleaseCapture();
            if (dragging_) {
                RECT r = Selection();
                dragging_ = false;
                if (r.right - r.left >= kMinSide && r.bottom - r.top >= kMinSide)
                    StartAdjust(r);
                else
                    Redraw();
            } else {
                StartAdjust(HoverArea());
            }
            return 0;
        }
        case WM_RBUTTONDOWN:
            if (adjusting_) {
                // Back to drawing a new area.
                adjusting_ = false;
                op_ = -1;
                ReleaseCapture();
                OnMove(screenPt());
            } else if (dragging_) {
                pressed_ = dragging_ = false;
                ReleaseCapture();
                Redraw();
            } else {
                Finish(false, {});
            }
            return 0;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) {
                Finish(false, {});
            } else if (adjusting_ && w == VK_RETURN) {
                Finish(true, sel_);
            } else if (adjusting_ && op_ < 0 && (w == VK_LEFT || w == VK_RIGHT || w == VK_UP || w == VK_DOWN)) {
                int step = GetKeyState(VK_SHIFT) < 0 ? 10 : 1;
                int dx = w == VK_LEFT ? -step : w == VK_RIGHT ? step : 0, dy = w == VK_UP ? -step : w == VK_DOWN ? step : 0;
                sel_ = KeepOnMonitor({sel_.left + dx, sel_.top + dy, sel_.right + dx, sel_.bottom + dy});
                Redraw();
            }
            return 0;
        case WM_KILLFOCUS:
        case WM_ACTIVATEAPP:
            if (m == WM_KILLFOCUS || !w) {
                if (active_ && GetForegroundWindow() != h) PostMessageW(h, WM_KEYDOWN, VK_ESCAPE, 0);
            }
            break;
    }
    return DefWindowProcW(h, m, w, l);
}
