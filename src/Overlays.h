#pragma once
#include "Ui.h"
#include "WindowList.h"

// Per-pixel-alpha popup drawn with a Direct2D DC render target and UpdateLayeredWindow.
class LayeredWindow {
public:
    ~LayeredWindow() { Destroy(); }
    bool Create(HINSTANCE inst, ID2D1Factory1* factory, const wchar_t* cls, DWORD exStyle, WNDPROC proc, void* user);
    void Destroy();
    HWND Hwnd() const { return hwnd_; }
    // rect in physical screen pixels; draw in DIPs (scale = DPI / 96). move=false keeps the current position.
    void Render(const RECT& rect, float scale, bool move, const std::function<void(Painter&)>& draw);
    // Redraws only `dirty` (surface pixels) over what Render left, at the same place and scale. draw gets each
    // pass's area in DIPs, to skip what lies outside it.
    void RenderDirty(float scale, const std::vector<RECT>& dirty,
                     const std::function<void(Painter&, const Rect&)>& draw);
    bool HasSurface(int w, int h) const { return dib_ && rt_ && w == w_ && h == h_; }
    const uint32_t* Bits() const { return bits_; }  // the surface, premultiplied BGRA, top-down

private:
    bool EnsureSurface(int w, int h);
    HWND hwnd_ = nullptr;
    ID2D1Factory1* factory_ = nullptr;
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr, oldBmp_ = nullptr;
    const uint32_t* bits_ = nullptr;
    int w_ = 0, h_ = 0;
    ComPtr<ID2D1DCRenderTarget> rt_, soft_;  // soft_: RenderDirty's
    Painter painter_, softPainter_;
};

// "REC 0:12.3 ■" pill pinned to what is being recorded, the pre-roll countdown and, for screen regions, a thin
// frame around the area. None of it shows up in recordings.
class RecOverlay {
public:
    void Init(HINSTANCE inst, ID2D1Factory1* factory);
    // target: the recorded area in screen pixels.
    void ShowCountdown(const RECT& target, int value);
    // outside: place the pill just outside the area (regions) instead of inside its top edge.
    void ShowPill(const RECT& target, bool outside, std::function<void()> onStop, std::function<void()> onPause);
    void SetPaused(bool paused);
    void ShowFrame(const RECT& area);
    void Update(double elapsed, size_t frames);
    // Screenshot tests: the pill drawn at the origin without a window.
    void PaintTest(Painter& p, double elapsed, bool paused, int hover);
    void Hide();
    bool Visible() const { return mode_ != Mode::None; }
    void SetMotion(bool on) { motion_ = on; }  // Windows' animations: off, the REC dot stays still

private:
    enum class Mode { None, Countdown, Pill };
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);
    void Ensure();
    void Place(const RECT& target, int wDip, int hDip, bool center, bool outside);
    void Redraw(bool move);
    void Paint(Painter& p);
    Rect StopRect() const;
    Rect PauseRect() const;
    int ButtonAt(float x, float y) const;  // 1 = pause, 2 = stop, 0 = none

    HINSTANCE inst_ = nullptr;
    ID2D1Factory1* factory_ = nullptr;
    LayeredWindow win_;
    Mode mode_ = Mode::None;
    RECT rect_{};
    float scale_ = 1.f;
    double elapsed_ = 0;
    size_t frames_ = 0;
    int countdown_ = 0;
    int hover_ = 0;  // ButtonAt under the mouse
    bool tracking_ = false, paused_ = false, motion_ = true;
    std::function<void()> onStop_, onPause_;
    HWND frame_[4] = {};
};

// Fullscreen click-to-pick window chooser.
class PickerOverlay {
public:
    void Init(HINSTANCE inst, ID2D1Factory1* factory);
    void Start(std::vector<WinInfo> candidates, std::function<void(HWND)> done);
    void Cancel() { Finish(nullptr); }
    bool Active() const { return active_; }

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);
    void OnMove(POINT screen);
    void Finish(HWND result);
    void Redraw();

    HINSTANCE inst_ = nullptr;
    ID2D1Factory1* factory_ = nullptr;
    LayeredWindow win_;
    std::vector<WinInfo> cands_;
    std::function<void(HWND)> done_;
    RECT virt_{};
    int hover_ = -1;
    POINT cursor_{};
    bool active_ = false;
};

// How the picker ended: closed, the area kept for later (Home shows it), or the area to record now.
enum class PickResult { Cancel, Save, Record };

// Fullscreen drag-to-select for region recording. Clicking without dragging takes the window under the cursor
// (or the whole monitor when there is none). The result always lies on a single monitor.
class RegionPicker {
public:
    // Toolbar slots after the five ratios; the last three are the countdown menu, there only while it is open.
    enum : int { kBarCountdown = 5, kBarCancel, kBarSave, kBarRecord, kBarMenu, kBarSlots = kBarMenu + 3 };
    enum class TestView { Drag, Adjust, Last };

    void Init(HINSTANCE inst, ID2D1Factory1* factory);
    // last: the area picked before. The picker opens on it, ready to record or adjust, if it still lies on one
    // monitor. countdown: Settings::countdown, which the toolbar can change (Countdown() after `done`).
    void Start(std::vector<WinInfo> windows, RECT last, int countdown, std::function<void(PickResult, RECT area)> done);
    void Cancel() { Finish(PickResult::Cancel, {}); }
    bool Active() const { return active_; }
    int Countdown() const { return countdown_; }
    // Tests drive the picker with posted mouse/key messages and read the result back.
    HWND Hwnd() const { return win_.Hwnd(); }
    bool Adjusting() const { return adjusting_; }
    bool MenuOpen() const { return cdOpen_; }
    RECT AdjustedArea() const { return sel_; }
    RECT ToolbarButton(int i) const { return buttons_[i]; }
    // Screenshot tests: paint a mid-drag state over `screen` (virtual-screen pixels) without a window; with Adjust
    // the selection anchor..cursor being fine-tuned; with Last that selection as the area picked before, with the
    // countdown menu open.
    void PaintTest(Painter& p, const RECT& screen, POINT anchor, POINT cursor, TestView view = TestView::Drag);
    // Tests: redraws everything and counts the pixels that differ from what the partial redraws left.
    size_t StalePixels();
    void RedrawAll() {
        full_ = true;
        Redraw();
    }
    struct Stats {
        int full = 0, partial = 0;
        double fullSec = 0, partialSec = 0;
        long long partialPx = 0;
    };
    const Stats& RedrawStats() const { return stats_; }
    void ResetStats() { stats_ = {}; }

private:
    // Where Paint puts things, in DIPs from the virtual screen's top-left.
    struct Layout {
        float scale = 1, vw = 0, vh = 0;
        bool hole = false, adjust = false, lines = false, menu = false;
        RECT area{};  // the hole, screen pixels
        Rect r, label, bar, menuBox, slots[kBarSlots], pill;
        float tw = 0, cx = 0, cy = 0;  // title width, cursor
        std::wstring title, dims, msg, hint;
    };
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);
    void OnMove(POINT screen);
    void Finish(PickResult result, RECT area);
    // After the first drag (or click on a window) the area stays up to be moved, resized and given a ratio.
    void StartAdjust(RECT area);
    int HitTest(POINT pt) const;  // 0..7 handles clockwise from top-left, 8 inside, -1 outside
    int HoverAt(POINT pt) const;  // the window candidate under pt, -1 = none
    int SlotAt(POINT pt) const;   // the toolbar slot under pt, -1 = none
    void DragTo(POINT pt);
    void SetRatio(int index);
    RECT KeepOnMonitor(RECT r) const;
    RECT Selection() const;
    RECT HoverArea() const;
    void Redraw();
    Layout Arrange(float scale) const;
    void Paint(Painter& p, const Layout& L, const Rect& clip);
    std::vector<RECT> Marks(const Layout& L) const;
    RECT ToSurface(const Rect& r, float scale) const;
    RECT ToScreen(const Rect& r, float scale) const;

    HINSTANCE inst_ = nullptr;
    ID2D1Factory1* factory_ = nullptr;
    LayeredWindow win_;
    std::vector<WinInfo> cands_;
    std::function<void(PickResult, RECT)> done_;
    RECT virt_{}, anchorMon_{};
    POINT cursor_{}, anchor_{};
    int hover_ = -1;
    bool pressed_ = false, dragging_ = false, active_ = false;

    bool adjusting_ = false;
    RECT sel_{};
    int ratio_ = 0;     // kRatios
    int op_ = -1;       // HitTest of the press being dragged, -1 = none
    POINT opStart_{};
    RECT opSel_{};
    RECT buttons_[kBarSlots]{};  // toolbar in screen pixels (kBar*); empty where nothing is drawn
    int countdown_ = 1;
    bool cdOpen_ = false;        // the countdown menu over the toolbar
    RECT lastArea_{};            // the area picked before, while the picker shows it
    float scale_ = 1;
    DWORD lastClick_ = 0;
    Layout last_;        // what is on screen
    bool full_ = true;   // the next Redraw draws everything
    Stats stats_;
};
