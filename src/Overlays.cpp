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

// Overlays look the same over any program, whatever the theme (dark), except in a contrast theme: its colors then.
D2D1_COLOR_F Panel(float a) { return Contrast() ? Bg : Hex(0x111318, a); }  // the pills and the countdown
D2D1_COLOR_F Bar() { return Contrast() ? Bg : Hex(0x15181E, 0.97f); }      // the region bar and its menu
D2D1_COLOR_F Ink() { return Contrast() ? TextColor : White; }                // text on them
D2D1_COLOR_F Edge(float a) { return Contrast() ? Border : WithAlpha(White, a); }
D2D1_COLOR_F Chosen() { return Contrast() ? Accent : Hex(0x2C3240); }  // a chosen button on the bar
D2D1_COLOR_F Hot() { return Contrast() ? Bg : Hex(0x252A34); }         // under the pointer
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
    bits_ = nullptr;
    w_ = h_ = 0;
    painter_ = Painter();  // its brush belonged to the released render target
    soft_.Reset();
    softPainter_ = Painter();
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
    bits_ = (const uint32_t*)bits;
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

void LayeredWindow::RenderDirty(float scale, const std::vector<RECT>& dirty,
                                const std::function<void(Painter&, const Rect&)>& draw) {
    if (!hwnd_ || !dib_ || !rt_) return;
    // Small passes are much cheaper on the CPU: the GPU target waits for its copy back every time. Its
    // antialiasing differs a little from the GPU's, so callers redraw each thing whole, never part of it.
    if (!soft_) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        if (FAILED(factory_->CreateDCRenderTarget(&props, &soft_))) return;
        soft_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    // A pass starts where a whole number of pixels is an exact number of DIPs, so it draws on the same grid as a
    // full render: 1 px at 100 %, 5 px at 125 %, 3 px at 150 %.
    int k = (int)std::lround(scale * 4);
    if (k <= 0 || std::fabs(k - scale * 4) > 1e-4f) k = 1;
    while (k % 2 == 0) k /= 2;
    const RECT surface{0, 0, w_, h_};
    for (RECT d : dirty) {
        if (!IntersectRect(&d, &d, &surface)) continue;
        d.left -= d.left % k;
        d.top -= d.top % k;
        soft_->BindDC(memDC_, &d);
        soft_->SetDpi(96.f * scale, 96.f * scale);
        soft_->BeginDraw();
        float ox = d.left / scale, oy = d.top / scale;
        soft_->SetTransform(D2D1::Matrix3x2F::Translation(-ox, -oy));
        soft_->Clear(D2D1::ColorF(0, 0, 0, 0));
        softPainter_.Bind(soft_.Get());
        draw(softPainter_, Rect(ox, oy, d.right / scale, d.bottom / scale));
        soft_->SetTransform(D2D1::Matrix3x2F::Identity());
        if (FAILED(soft_->EndDraw())) {
            soft_.Reset();
            softPainter_ = Painter();
            w_ = h_ = 0;  // the next Render starts over
            return;
        }
        POINT src{0, 0};
        SIZE sz{w_, h_};
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UPDATELAYEREDWINDOWINFO info{sizeof(info)};
        info.hdcSrc = memDC_;
        info.pptSrc = &src;
        info.psize = &sz;
        info.pblend = &bf;
        info.dwFlags = ULW_ALPHA;
        info.prcDirty = &d;
        UpdateLayeredWindowIndirect(hwnd_, &info);
    }
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
        theme::Use colors(theme::OverlayPalette());
        if (mode_ == Mode::Countdown) {
            p.Circle(66, 66, 62, Panel(0.88f));
            p.Ring(66, 66, 62, Edge(0.10f), 1.f);
            TextStyle st;
            st.h = AlignCenter;
            st.weight = kBold;
            p.Text(Rect(0, 0, 132, 124), std::to_wstring(countdown_), 58.f, Ink(), st);
            TextStyle sub;
            sub.h = AlignCenter;
            p.Text(Rect(0, 86, 132, 112), tr(L"녹화 준비"), 12.f, TextDim, sub);
            return;
        }
        Rect r(0.5f, 0.5f, kPillW - 0.5f, kPillH - 0.5f);
        p.Fill(r, Panel(0.92f), 20);
        p.Stroke(r, paused_ ? WithAlpha(Warning, Contrast() ? 1.f : 0.35f) : Edge(0.09f), 20);
        TextStyle label;
        label.weight = kBold;
        if (paused_) {
            p.Fill(Rect::XYWH(14, 14, 3.5f, 12), Warning, 1);
            p.Fill(Rect::XYWH(20.5f, 14, 3.5f, 12), Warning, 1);
            p.Text(Rect(32, 0, 90, 40), tr(L"일시정지"), 11.5f, Warning, label);
        } else {
            // Steady with Windows' animations off.
            float pulse = motion_ ? 0.55f + 0.45f * (float)std::cos(elapsed_ * 3.1415926 * 1.6) : 1.f;
            p.Circle(19, 20, 9, WithAlpha(Rec, 0.18f * pulse));
            p.Circle(19, 20, 5, Rec);
            p.Text(Rect(32, 0, 90, 40), L"REC", 11.5f, Rec, label);
        }
        TextStyle tm;
        tm.weight = kSemibold;
        tm.tabular = true;
        p.Text(Rect(paused_ ? 88.f : 66.f, 0, kPillW - 68, 40), util::FormatClock(elapsed_, true), 15.f,
               paused_ ? TextDim : Ink(), tm);
        // Pause / resume, then stop.
        Rect pr = PauseRect();
        if (Contrast())
            p.Ring(pr.CX(), pr.CY(), 13.5f, Border, hover_ == 1 ? 2.f : 1.f);
        else
            p.Circle(pr.CX(), pr.CY(), 14, WithAlpha(White, hover_ == 1 ? 0.22f : 0.10f));
        D2D1_COLOR_F ink = Contrast() ? TextColor : hover_ == 1 ? White : Hex(0xE6E8EE);
        if (paused_) {
            p.Icon(pr.Offset(1, 0), 0xE768 /*Play*/, 11.f, ink);
        } else {
            p.Fill(Rect::XYWH(pr.CX() - 4.5f, pr.CY() - 5.5f, 3, 11), ink, 1);
            p.Fill(Rect::XYWH(pr.CX() + 1.5f, pr.CY() - 5.5f, 3, 11), ink, 1);
        }
        Rect sr = StopRect();
        if (Contrast())
            p.Ring(sr.CX(), sr.CY(), 13.5f, Border, hover_ == 2 ? 2.f : 1.f);
        else
            p.Circle(sr.CX(), sr.CY(), 14, WithAlpha(White, hover_ == 2 ? 0.22f : 0.10f));
        p.Fill(Rect::Centered(sr.CX(), sr.CY(), 10, 10), Contrast() ? TextColor : hover_ == 2 ? White : Hex(0xE6E8EE), 2);
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
        theme::Use colors(theme::OverlayPalette());
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
            p.Text(Rect(label.l + 12, label.t, label.r - dw - 22, label.b), w.title, 13.f, OnFill, ts);
            p.Text(Rect(label.r - dw - 12, label.t, label.r - 8, label.b), dims, 12.f, WithAlpha(OnFill, 0.8f));
        }
        // Instruction pill on the monitor under the cursor.
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint(cursor_, MONITOR_DEFAULTTONEAREST), &mi);
        Rect mon = toDip(mi.rcWork);
        std::wstring msg = tr(L"녹화할 창을 클릭하세요");
        std::wstring hint = tr(L"Esc · 취소");
        float mw = p.Measure(msg, 14.f, kSemibold) + p.Measure(hint, 12.f) + 56;
        Rect pill = Rect::XYWH(mon.CX() - mw / 2, mon.t + 24, mw, 40);
        p.Fill(pill, Panel(0.95f), 20);
        p.Stroke(pill.Inset(0.5f), Edge(0.1f), 20);
        p.Crosshair(pill.l + 22, pill.CY(), 16, Accent);
        TextStyle ts;
        ts.weight = kSemibold;
        p.Text(Rect(pill.l + 38, pill.t, pill.r, pill.b), msg, 14.f, Ink(), ts);
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
constexpr const wchar_t* kCountdowns[] = {L"없음", L"3초", L"5초"};  // Settings::countdown
constexpr int kMinSide = 16;
}  // namespace

void RegionPicker::Init(HINSTANCE inst, ID2D1Factory1* factory) {
    inst_ = inst;
    factory_ = factory;
}

void RegionPicker::Start(std::vector<WinInfo> windows, RECT last, int countdown,
                         std::function<void(PickResult, RECT)> done) {
    cands_ = std::move(windows);
    done_ = std::move(done);
    hover_ = -1;
    pressed_ = dragging_ = adjusting_ = cdOpen_ = false;
    op_ = -1;
    ratio_ = 0;
    countdown_ = std::clamp(countdown, 0, 2);
    lastArea_ = {};
    virt_ = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
    virt_.right = virt_.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    virt_.bottom = virt_.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (!win_.Hwnd())
        win_.Create(inst_, factory_, L"OzMakerRegion", WS_EX_TOPMOST | WS_EX_TOOLWINDOW, Proc, this);
    active_ = true;
    full_ = true;
    GetCursorPos(&cursor_);
    hover_ = HoverAt(cursor_);
    // The area picked before comes back, to record again or adjust, while it still lies whole on one monitor.
    MONITORINFO mi{sizeof(mi)};
    HMONITOR mon = MonitorFromRect(&last, MONITOR_DEFAULTTONULL);
    RECT in{};
    if (mon && GetMonitorInfoW(mon, &mi) && IntersectRect(&in, &last, &mi.rcMonitor) && EqualRect(&in, &last) &&
        last.right - last.left >= kMinSide && last.bottom - last.top >= kMinSide) {
        lastArea_ = last;
        StartAdjust(last);
    } else {
        Redraw();
    }
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

int RegionPicker::HoverAt(POINT pt) const {
    for (size_t i = 0; i < cands_.size(); ++i)
        if (!cands_[i].minimized && PtInRect(&cands_[i].frame, pt)) return (int)i;
    return -1;
}

int RegionPicker::SlotAt(POINT pt) const {
    for (int i = 0; i < kBarSlots; ++i)
        if (PtInRect(&buttons_[i], pt)) return i;
    return -1;
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

void RegionPicker::Finish(PickResult result, RECT area) {
    if (!active_) return;
    active_ = false;
    adjusting_ = cdOpen_ = false;
    if (GetCapture() == win_.Hwnd()) ReleaseCapture();
    ShowWindow(win_.Hwnd(), SW_HIDE);
    win_.Destroy();
    auto cb = std::move(done_);
    done_ = nullptr;
    if (cb) cb(result, area);
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
    if (!pressed_) hover_ = HoverAt(pt);
    // The guide lines follow the cursor, so every move redraws.
    if (active_) Redraw();
}

namespace {
long long Area(const RECT& r) { return (long long)(r.right - r.left) * (r.bottom - r.top); }
}

// The picker covers the whole virtual screen, so a mouse move redraws only what changed: the guide lines, the
// edges of the area, its label and toolbar, the hint. Elsewhere it is the same dim as before.
void RegionPicker::Redraw() {
    POINT at = adjusting_ ? POINT{(sel_.left + sel_.right) / 2, (sel_.top + sel_.bottom) / 2} : cursor_;
    float scale = ScaleForRect(RECT{at.x, at.y, at.x + 1, at.y + 1});
    Layout L = Arrange(scale);
    for (int i = 0; i < kBarSlots; ++i)
        buttons_[i] = L.adjust && (i < kBarMenu || L.menu) ? ToScreen(L.slots[i], scale) : RECT{};
    const int w = virt_.right - virt_.left, h = virt_.bottom - virt_.top;
    bool full = full_ || scale != scale_ || !win_.HasSurface(w, h);
    std::vector<RECT> dirty;
    if (!full) {
        dirty = Marks(last_);
        for (const RECT& m : Marks(L)) dirty.push_back(m);
        // The area itself turns from dim to clear (and back) where it moved.
        if (last_.hole != L.hole || !EqualRect(&last_.area, &L.area)) {
            for (const Layout* l : {&last_, &L})
                if (l->hole) dirty.push_back(ToSurface(l->r.Inset(-11), l->scale));
        }
        // Fewer, larger passes: each one costs about as much as this many pixels on top of its own.
        const long long overhead = 40000;
        for (bool merged = true; merged;) {
            merged = false;
            for (size_t i = 0; i < dirty.size() && !merged; ++i)
                for (size_t j = i + 1; j < dirty.size(); ++j) {
                    RECT u;
                    UnionRect(&u, &dirty[i], &dirty[j]);
                    if (Area(u) <= Area(dirty[i]) + Area(dirty[j]) + overhead) {
                        dirty[i] = u;
                        dirty.erase(dirty.begin() + j);
                        merged = true;
                        break;
                    }
                }
        }
        long long px = 0;
        for (const RECT& d : dirty) px += Area(d);
        full = px * 2 > (long long)w * h;
        if (!full) stats_.partialPx += px;
    }
    scale_ = scale;
    full_ = false;
    double t0 = util::NowSeconds();
    if (full) {
        win_.Render(virt_, scale, true, [&](Painter& p) { Paint(p, L, Rect(0, 0, L.vw, L.vh)); });
        ++stats_.full;
        stats_.fullSec += util::NowSeconds() - t0;
    } else {
        win_.RenderDirty(scale, dirty, [&](Painter& p, const Rect& clip) { Paint(p, L, clip); });
        ++stats_.partial;
        stats_.partialSec += util::NowSeconds() - t0;
    }
    last_ = std::move(L);
}

size_t RegionPicker::StalePixels() {
    const int w = virt_.right - virt_.left, h = virt_.bottom - virt_.top;
    if (!active_ || !win_.HasSurface(w, h)) return SIZE_MAX;
    GdiFlush();
    std::vector<uint32_t> partial(win_.Bits(), win_.Bits() + (size_t)w * h);
    // One pass over everything on the same (CPU) target: the GPU antialiases a little differently.
    win_.RenderDirty(scale_, {RECT{0, 0, w, h}}, [&](Painter& p, const Rect& clip) { Paint(p, last_, clip); });
    GdiFlush();
    const uint32_t* now = win_.Bits();
    size_t stale = 0;
    for (size_t i = 0; i < partial.size(); ++i) {
        uint32_t a = partial[i], b = now[i];
        for (int s = 0; s < 32 && a != b; s += 8)
            if (std::abs((int)((a >> s) & 0xFF) - (int)((b >> s) & 0xFF)) > 2) {
                ++stale;
                break;
            }
    }
    return stale;
}

RECT RegionPicker::ToSurface(const Rect& r, float scale) const {
    return {(LONG)std::floor(r.l * scale), (LONG)std::floor(r.t * scale), (LONG)std::ceil(r.r * scale),
            (LONG)std::ceil(r.b * scale)};
}

RECT RegionPicker::ToScreen(const Rect& r, float scale) const {
    return {(LONG)std::lround(r.l * scale + virt_.left), (LONG)std::lround(r.t * scale + virt_.top),
            (LONG)std::lround(r.r * scale + virt_.left), (LONG)std::lround(r.b * scale + virt_.top)};
}

// What differs from the plain dim, in surface pixels. Paint draws nothing else.
std::vector<RECT> RegionPicker::Marks(const Layout& L) const {
    std::vector<Rect> m;
    if (L.hole) {
        const Rect& r = L.r;
        const float e = 9;  // the stroke, the handles
        m.push_back(Rect(r.l - e, r.t - e, r.r + e, r.t + e));
        m.push_back(Rect(r.l - e, r.b - e, r.r + e, r.b + e));
        m.push_back(Rect(r.l - e, r.t - e, r.l + e, r.b + e));
        m.push_back(Rect(r.r - e, r.t - e, r.r + e, r.b + e));
        if (L.adjust) {
            for (int i = 1; i < 3; ++i) {
                float x = r.l + r.W() * i / 3, y = r.t + r.H() * i / 3;
                m.push_back(Rect(x - 1, r.t, x + 1, r.b));
                m.push_back(Rect(r.l, y - 1, r.r, y + 1));
            }
            m.push_back(Rect(L.bar.l, L.bar.t, L.bar.r, L.bar.b + 3));  // with its shadow
            if (L.menu) m.push_back(Rect(L.menuBox.l, L.menuBox.t, L.menuBox.r, L.menuBox.b + 3));
        }
        m.push_back(L.label);
    }
    if (L.lines) {
        m.push_back(Rect(0, L.cy - 1, L.vw, L.cy + 1));
        m.push_back(Rect(L.cx - 1, 0, L.cx + 1, L.vh));
    }
    m.push_back(L.pill);
    std::vector<RECT> out;
    out.reserve(m.size());
    for (const Rect& r : m) out.push_back(ToSurface(r.Inset(-2), L.scale));  // antialiasing
    return out;
}

RegionPicker::Layout RegionPicker::Arrange(float scale) const {
    Layout L;
    L.scale = scale;
    L.vw = (virt_.right - virt_.left) / scale;
    L.vh = (virt_.bottom - virt_.top) / scale;
    L.adjust = adjusting_;
    L.lines = !dragging_ && !adjusting_;
    L.cx = (cursor_.x - virt_.left) / scale;
    L.cy = (cursor_.y - virt_.top) / scale;
    auto toDip = [&](const RECT& r) {
        return Rect((r.left - virt_.left) / scale, (r.top - virt_.top) / scale, (r.right - virt_.left) / scale,
                    (r.bottom - virt_.top) / scale);
    };
    Painter m;  // measures only
    L.hole = dragging_ || hover_ >= 0 || adjusting_;
    if (L.hole) {
        L.area = dragging_ ? Selection() : adjusting_ ? sel_ : HoverArea();
        const Rect r = L.r = toDip(L.area);
        // Size in physical pixels: exactly what will be recorded.
        L.dims = util::Format(L"%d × %d", L.area.right - L.area.left, L.area.bottom - L.area.top);
        if (!dragging_ && !adjusting_)
            L.title = cands_[hover_].title;
        else if (adjusting_ && EqualRect(&sel_, &lastArea_))
            L.title = tr(L"직전 영역");
        L.tw = L.title.empty() ? 0 : std::min(320.f, m.Measure(L.title, 13.f, kSemibold)) + 12;
        float lw = L.tw + m.Measure(L.dims, 13.f, kSemibold) + 24;
        L.label = Rect::XYWH(r.l, r.t - 36, lw, 30);
        if (L.label.t < 4) L.label = Rect::XYWH(r.l + 8, r.t + 8, lw, 30);
        if (L.label.r > L.vw - 4) L.label = L.label.Offset(L.vw - 4 - L.label.r, 0);
        if (adjusting_) {
            // Toolbar below the area (inside it when the screen ends there): ratios | countdown | cancel, save only,
            // record.
            Rect mon = toDip(anchorMon_);
            const float bh = 34, rw = 54, gap = 13;
            const float cdw = 74, cw = 56, sw = std::max(72.f, m.Measure(tr(L"영역만 저장"), 12.5f) + 28), ow = 80;
            float tw = 6 + rw * 5 + gap + cdw + gap + cw + 6 + sw + 6 + ow + 6;
            float ty = r.b + 12;
            if (ty + bh + 12 > mon.b) ty = std::max(r.t + 12, r.b - bh - 12);
            const float lo = mon.l + 8, hi = std::max(lo, mon.r - 8 - tw);
            L.bar = Rect::XYWH(std::clamp(r.CX() - tw / 2, lo, hi), ty, tw, bh + 12);
            float x = L.bar.l + 6;
            for (int i = 0; i < 5; ++i, x += rw) L.slots[i] = Rect::XYWH(x, L.bar.t + 6, rw, bh);
            x += gap;
            L.slots[kBarCountdown] = Rect::XYWH(x, L.bar.t + 6, cdw, bh);
            x += cdw + gap;
            L.slots[kBarCancel] = Rect::XYWH(x, L.bar.t + 6, cw, bh);
            x += cw + 6;
            L.slots[kBarSave] = Rect::XYWH(x, L.bar.t + 6, sw, bh);
            x += sw + 6;
            L.slots[kBarRecord] = Rect::XYWH(x, L.bar.t + 6, ow, bh);
            // The countdown menu opens over its button (under the toolbar when the screen ends above it).
            L.menu = cdOpen_;
            if (L.menu) {
                const float iw = 52, mw = iw * 3 + 12, mh = bh + 12;
                const Rect& c = L.slots[kBarCountdown];
                float my = L.bar.t - 6 - mh;
                if (my < mon.t + 8) my = L.bar.b + 6;
                const float mlo = mon.l + 8, mhi = std::max(mlo, mon.r - 8 - mw);
                L.menuBox = Rect::XYWH(std::clamp(c.CX() - mw / 2, mlo, mhi), my, mw, mh);
                for (int i = 0; i < 3; ++i) L.slots[kBarMenu + i] = Rect::XYWH(L.menuBox.l + 6 + i * iw, my + 6, iw, bh);
            }
        }
    }
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(cursor_, MONITOR_DEFAULTTONEAREST), &mi);
    Rect mon = toDip(mi.rcWork);
    L.msg = !adjusting_                       ? tr(L"드래그해서 녹화할 영역을 지정하세요")
            : EqualRect(&sel_, &lastArea_) ? tr(L"직전 영역입니다 · 밖에서 드래그하면 새로 지정합니다")
                                           : tr(L"끌어서 옮기거나 모서리로 크기를 조절하세요");
    L.hint = adjusting_ ? tr(L"Enter · 녹화   Esc · 취소") : tr(L"클릭 · 창 전체   Esc · 취소");
    float mw = m.Measure(L.msg, 14.f, kSemibold) + m.Measure(L.hint, 12.f) + 64;
    L.pill = Rect::XYWH(mon.CX() - mw / 2, mon.t + 24, mw, 40);
    return L;
}

void RegionPicker::PaintTest(Painter& p, const RECT& screen, POINT anchor, POINT cursor, TestView view) {
    virt_ = screen;
    anchor_ = anchor;
    cursor_ = cursor;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &mi);
    anchorMon_ = mi.rcMonitor;
    hover_ = -1;
    if (view != TestView::Drag) {
        adjusting_ = true;
        sel_ = Selection();
        if (view == TestView::Adjust) {
            SetRatio(3);  // 16:9
        } else {
            ratio_ = 0;
            lastArea_ = sel_;
            countdown_ = 1;
            cdOpen_ = true;
        }
        cursor_ = {(sel_.left + sel_.right) / 2, (sel_.top + sel_.bottom) / 2};
    } else {
        pressed_ = dragging_ = true;
    }
    scale_ = 1;
    Layout L = Arrange(1.f);
    Paint(p, L, Rect(0, 0, L.vw, L.vh));
    pressed_ = dragging_ = adjusting_ = cdOpen_ = false;
    lastArea_ = {};
}

// clip: the part being redrawn; what lies outside it is skipped (the target clips the rest).
void RegionPicker::Paint(Painter& p, const Layout& L, const Rect& clip) {
    auto meets = [&](const Rect& a) { return !a.Inset(-2).Intersect(clip).Empty(); };
    theme::Use colors(theme::OverlayPalette());
    const D2D1_COLOR_F dim = Hex(0x05060A, 0.55f);
    Rect all(0, 0, L.vw, L.vh);
    if (!L.hole) {
        p.Fill(all, dim);
    } else {
        const Rect& r = L.r;
        p.Fill(Rect(all.l, all.t, all.r, r.t), dim);
        p.Fill(Rect(all.l, r.b, all.r, all.b), dim);
        p.Fill(Rect(all.l, r.t, r.l, r.b), dim);
        p.Fill(Rect(r.r, r.t, all.r, r.b), dim);
        p.Fill(r, Hex(0x000000, 0.004f));  // keeps the hole clickable
        p.Stroke(r.Inset(-1.f), dragging_ || L.adjust ? Accent : WithAlpha(Accent, 0.85f), 0, 2.f);
        if (L.adjust) {
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

        if (meets(L.label)) {
            TextStyle ts;
            ts.weight = kSemibold;
            ts.tabular = true;
            const Rect& label = L.label;
            p.Fill(label, Accent, 8);
            if (!L.title.empty()) p.Text(Rect(label.l + 12, label.t, label.l + L.tw, label.b), L.title, 13.f, OnFill, ts);
            p.Text(Rect(label.l + 12 + L.tw, label.t, label.r - 8, label.b), L.dims, 13.f,
                   L.title.empty() ? OnFill : WithAlpha(OnFill, 0.8f), ts);
        }

        if (L.adjust && meets(Rect(L.bar.l, L.bar.t, L.bar.r, L.bar.b + 3))) {
            const Rect& bar = L.bar;
            p.Fill(bar.Offset(0, 3), WithAlpha(Shadow, 0.35f), 12);
            p.Fill(bar, Bar(), 12);
            p.Stroke(bar.Inset(0.5f), Edge(0.1f), 12);
            // Dividers: ratios | countdown | the ways out.
            for (int i : {(int)kBarCountdown, (int)kBarCancel}) {
                float x = std::round(L.slots[i].l - 6.5f) + 0.5f;
                p.Line(x, bar.t + 14, x, bar.b - 14, Edge(0.1f), 1.f);
            }
            TextStyle cs;
            cs.h = AlignCenter;
            cs.weight = kSemibold;
            TextStyle ls;
            ls.weight = kSemibold;
            for (int i = 0; i < kBarMenu; ++i) {
                const Rect& b = L.slots[i];
                bool hot = op_ < 0 && b.Contains(L.cx, L.cy);
                bool on = i == ratio_ || (i == kBarCountdown && L.menu);
                if (i == kBarRecord)
                    p.Fill(b, hot ? RecFillHi : RecFill, 8);
                else if (on)
                    p.Fill(b, Chosen(), 8);
                else if (hot)
                    p.Fill(b, Hot(), 8);
                if (Contrast() && hot && !on && i != kBarRecord) p.Stroke(b.Inset(0.5f), Border, 8);
                const D2D1_COLOR_F fg = i == kBarRecord || (on && Contrast()) ? OnFill : on || hot ? Ink() : TextDim;
                if (i == kBarCountdown || i == kBarRecord) {
                    // A mark, then the word, centred together.
                    const wchar_t* text = i == kBarRecord ? tr(L"녹화") : tr(kCountdowns[countdown_]);
                    float w = p.Measure(text, 12.5f, kSemibold), x = b.CX() - (w + 18) / 2;
                    if (i == kBarCountdown) {
                        p.Stroke(b.Inset(0.5f), on ? Clear : Edge(0.12f), 8);
                        p.Icon(Rect::XYWH(x - 1, b.t, 15, b.H()), icon::Timer, 12.f, fg);
                    } else {
                        p.Circle(x + 5, b.CY(), 4.5f, OnFill);
                    }
                    p.Text(Rect(x + 18, b.t, x + 18 + w + 4, b.b), text, 12.5f, fg, ls);
                } else {
                    p.Text(b, i < 5 ? tr(kRatios[i].label) : i == kBarCancel ? tr(L"취소") : tr(L"영역만 저장"), 12.5f, fg,
                           cs);
                }
            }
        }
        if (L.menu && meets(Rect(L.menuBox.l, L.menuBox.t, L.menuBox.r, L.menuBox.b + 3))) {
            const Rect& box = L.menuBox;
            p.Fill(box.Offset(0, 3), WithAlpha(Shadow, 0.35f), 12);
            p.Fill(box, Bar(), 12);
            p.Stroke(box.Inset(0.5f), Edge(0.1f), 12);
            TextStyle cs;
            cs.h = AlignCenter;
            cs.weight = kSemibold;
            for (int i = 0; i < 3; ++i) {
                const Rect& b = L.slots[kBarMenu + i];
                bool hot = op_ < 0 && b.Contains(L.cx, L.cy);
                if (i == countdown_)
                    p.Fill(b, Chosen(), 8);
                else if (hot)
                    p.Fill(b, Hot(), 8);
                if (Contrast() && hot && i != countdown_) p.Stroke(b.Inset(0.5f), Border, 8);
                p.Text(b, tr(kCountdowns[i]), 12.5f,
                       i == countdown_ && Contrast() ? OnFill : i == countdown_ || hot ? Ink() : TextDim, cs);
            }
        }
    }
    if (L.lines) {
        // Guide lines through the cursor help line up the first corner.
        p.Line(0, L.cy, L.vw, L.cy, WithAlpha(White, 0.22f), 1.f);
        p.Line(L.cx, 0, L.cx, L.vh, WithAlpha(White, 0.22f), 1.f);
    }

    if (meets(L.pill)) {
        const Rect& pill = L.pill;
        p.Fill(pill, Panel(0.95f), 20);
        p.Stroke(pill.Inset(0.5f), Edge(0.1f), 20);
        p.Stroke(Rect::Centered(pill.l + 22, pill.CY(), 14, 11), Accent, 2, 1.6f);
        TextStyle ts;
        ts.weight = kSemibold;
        p.Text(Rect(pill.l + 38, pill.t, pill.r, pill.b), L.msg, 14.f, Ink(), ts);
        TextStyle hs;
        hs.h = AlignEnd;
        p.Text(Rect(pill.l, pill.t, pill.r - 16, pill.b), L.hint, 12.f, TextDim, hs);
    }
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
                if (op_ < 0 && SlotAt(pt) >= 0) hit = 9;
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
                int slot = SlotAt(pt);
                if (cdOpen_) {
                    // Any click closes the menu: one on an item picks it, one on its button only closes it.
                    cdOpen_ = false;
                    if (slot >= kBarMenu) countdown_ = slot - kBarMenu;
                    Redraw();
                    if (slot >= kBarMenu || slot == kBarCountdown) return 0;
                }
                if (slot >= 0) {
                    if (slot < 5) {
                        SetRatio(slot);
                        Redraw();
                    } else if (slot == kBarCountdown) {
                        cdOpen_ = true;
                        Redraw();
                    } else if (slot == kBarCancel) {
                        Finish(PickResult::Cancel, {});
                    } else {
                        Finish(slot == kBarRecord ? PickResult::Record : PickResult::Save, sel_);
                    }
                    return 0;
                }
                int hit = HitTest(pt);
                if (hit >= 0) {
                    // Double-click inside records.
                    DWORD now = GetTickCount();
                    if (hit == 8 && now - lastClick_ <= GetDoubleClickTime()) {
                        Finish(PickResult::Record, sel_);
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
                hover_ = HoverAt(pt);  // what a click without a drag takes
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
                adjusting_ = cdOpen_ = false;
                op_ = -1;
                ReleaseCapture();
                OnMove(screenPt());
            } else if (dragging_) {
                pressed_ = dragging_ = false;
                ReleaseCapture();
                Redraw();
            } else {
                Finish(PickResult::Cancel, {});
            }
            return 0;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE && cdOpen_) {
                cdOpen_ = false;
                Redraw();
            } else if (w == VK_ESCAPE) {
                Finish(PickResult::Cancel, {});
            } else if (adjusting_ && w == VK_RETURN) {
                Finish(PickResult::Record, sel_);
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
                if (active_ && GetForegroundWindow() != h) {
                    cdOpen_ = false;  // so the Escape closes the picker, not only the menu
                    PostMessageW(h, WM_KEYDOWN, VK_ESCAPE, 0);
                }
            }
            break;
    }
    return DefWindowProcW(h, m, w, l);
}
