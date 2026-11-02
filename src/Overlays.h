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

private:
    bool EnsureSurface(int w, int h);
    HWND hwnd_ = nullptr;
    ID2D1Factory1* factory_ = nullptr;
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr, oldBmp_ = nullptr;
    int w_ = 0, h_ = 0;
    ComPtr<ID2D1DCRenderTarget> rt_;
    Painter painter_;
};

// "REC 0:12.3 ■" pill pinned to what is being recorded, the pre-roll countdown and, for screen regions, a thin
// frame around the area. None of it shows up in recordings.
class RecOverlay {
public:
    void Init(HINSTANCE inst, ID2D1Factory1* factory);
    // target: the recorded area in screen pixels.
    void ShowCountdown(const RECT& target, int value);
    // outside: place the pill just outside the area (regions) instead of inside its top edge.
    void ShowPill(const RECT& target, bool outside, std::function<void()> onStop);
    void ShowFrame(const RECT& area);
    void Update(double elapsed, size_t frames);
    void Hide();
    bool Visible() const { return mode_ != Mode::None; }

private:
    enum class Mode { None, Countdown, Pill };
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);
    void Ensure();
    void Place(const RECT& target, int wDip, int hDip, bool center, bool outside);
    void Redraw(bool move);
    Rect StopRect() const;

    HINSTANCE inst_ = nullptr;
    ID2D1Factory1* factory_ = nullptr;
    LayeredWindow win_;
    Mode mode_ = Mode::None;
    RECT rect_{};
    float scale_ = 1.f;
    double elapsed_ = 0;
    size_t frames_ = 0;
    int countdown_ = 0;
    bool hoverStop_ = false, tracking_ = false;
    std::function<void()> onStop_;
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

// Fullscreen drag-to-select for region recording. Clicking without dragging takes the window under the cursor
// (or the whole monitor when there is none). The result always lies on a single monitor.
class RegionPicker {
public:
    void Init(HINSTANCE inst, ID2D1Factory1* factory);
    void Start(std::vector<WinInfo> windows, std::function<void(bool ok, RECT area)> done);
    void Cancel() { Finish(false, {}); }
    bool Active() const { return active_; }
    // Tests drive the picker with posted mouse/key messages and read the result back.
    HWND Hwnd() const { return win_.Hwnd(); }
    bool Adjusting() const { return adjusting_; }
    RECT AdjustedArea() const { return sel_; }
    RECT ToolbarButton(int i) const { return buttons_[i]; }
    // Screenshot tests: paint a mid-drag state over `screen` (virtual-screen pixels) without a window, or with
    // `adjust` the selection anchor..cursor being fine-tuned.
    void PaintTest(Painter& p, const RECT& screen, POINT anchor, POINT cursor, bool adjust = false);

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);
    void OnMove(POINT screen);
    void Finish(bool ok, RECT area);
    // After the first drag (or click on a window) the area stays up to be moved, resized and given a ratio.
    void StartAdjust(RECT area);
    int HitTest(POINT pt) const;  // 0..7 handles clockwise from top-left, 8 inside, -1 outside
    void DragTo(POINT pt);
    void SetRatio(int index);
    RECT KeepOnMonitor(RECT r) const;
    RECT Selection() const;
    RECT HoverArea() const;
    void Redraw();
    void Paint(Painter& p, float scale);

    HINSTANCE inst_ = nullptr;
    ID2D1Factory1* factory_ = nullptr;
    LayeredWindow win_;
    std::vector<WinInfo> cands_;
    std::function<void(bool, RECT)> done_;
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
    RECT buttons_[7]{};  // toolbar in screen pixels: five ratios, cancel, confirm
    float scale_ = 1;
    DWORD lastClick_ = 0;
};
