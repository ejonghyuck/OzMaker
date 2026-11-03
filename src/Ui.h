#pragma once
#include "Theme.h"

#include <initializer_list>

struct Rect {
    float l = 0, t = 0, r = 0, b = 0;

    Rect() = default;
    Rect(float l_, float t_, float r_, float b_) : l(l_), t(t_), r(r_), b(b_) {}
    static Rect XYWH(float x, float y, float w, float h) { return {x, y, x + w, y + h}; }
    static Rect Centered(float cx, float cy, float w, float h) { return {cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2}; }

    float W() const { return r - l; }
    float H() const { return b - t; }
    float CX() const { return (l + r) * .5f; }
    float CY() const { return (t + b) * .5f; }
    bool Contains(float x, float y) const { return x >= l && x < r && y >= t && y < b; }
    bool Empty() const { return r <= l || b <= t; }
    Rect Inset(float d) const { return {l + d, t + d, r - d, b - d}; }
    Rect Inset(float dx, float dy) const { return {l + dx, t + dy, r - dx, b - dy}; }
    Rect Offset(float dx, float dy) const { return {l + dx, t + dy, r + dx, b + dy}; }
    Rect Intersect(const Rect& o) const { return {std::max(l, o.l), std::max(t, o.t), std::min(r, o.r), std::min(b, o.b)}; }
    Rect Union(const Rect& o) const { return {std::min(l, o.l), std::min(t, o.t), std::max(r, o.r), std::max(b, o.b)}; }

    // RectCut-style layout: slice a strip off one side and shrink this rect.
    Rect CutTop(float h) { Rect o{l, t, r, std::min(b, t + h)}; t = o.b; return o; }
    Rect CutBottom(float h) { Rect o{l, std::max(t, b - h), r, b}; b = o.t; return o; }
    Rect CutLeft(float w) { Rect o{l, t, std::min(r, l + w), b}; l = o.r; return o; }
    Rect CutRight(float w) { Rect o{std::max(l, r - w), t, r, b}; r = o.l; return o; }

    operator D2D1_RECT_F() const { return {l, t, r, b}; }
};

enum : uint8_t { AlignStart = 0, AlignCenter = 1, AlignEnd = 2 };

struct TextStyle {
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    uint8_t h = AlignStart;
    uint8_t v = AlignCenter;
    bool wrap = false;
    bool tabular = false;
};

inline constexpr DWRITE_FONT_WEIGHT kSemibold = DWRITE_FONT_WEIGHT_SEMI_BOLD;
inline constexpr DWRITE_FONT_WEIGHT kBold = DWRITE_FONT_WEIGHT_BOLD;

// Shared DirectWrite text-format cache (device independent).
class Fonts {
public:
    static void Init(IDWriteFactory* f);
    static IDWriteTextFormat* Get(float size, DWRITE_FONT_WEIGHT w, const wchar_t* family = theme::UiFont);
    static ComPtr<IDWriteTextLayout> Layout(std::wstring_view s, float size, const TextStyle& st, float w, float h,
                                            const wchar_t* family = theme::UiFont);
};

// Thin drawing helper over any ID2D1RenderTarget (main window DC or a layered-window DC target).
class Painter {
public:
    void Bind(ID2D1RenderTarget* rt);
    ID2D1RenderTarget* RT() const { return rt_; }
    ID2D1SolidColorBrush* Brush(D2D1_COLOR_F c);

    void Fill(const Rect& r, D2D1_COLOR_F c, float radius = 0);
    void Stroke(const Rect& r, D2D1_COLOR_F c, float radius = 0, float width = 1);
    // Dashed outline: dashes and gaps in pixels.
    void StrokeDashed(const Rect& r, D2D1_COLOR_F c, float radius, float width, float dash, float gap);
    void Line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float w = 1);
    void Circle(float cx, float cy, float r, D2D1_COLOR_F c);
    void Ring(float cx, float cy, float r, D2D1_COLOR_F c, float w);
    void Text(const Rect& r, std::wstring_view s, float size, D2D1_COLOR_F c, const TextStyle& st = {});
    void Icon(const Rect& r, wchar_t glyph, float size, D2D1_COLOR_F c);
    float Measure(std::wstring_view s, float size, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL);
    void PushClip(const Rect& r);
    void PopClip();
    // Brand mark: gradient squircle with a record ring.
    void Logo(const Rect& r);
    // Crosshair glyph drawn with lines (MDL2 has no good "pick window" icon).
    void Crosshair(float cx, float cy, float size, D2D1_COLOR_F c);
    // Bounds of everything drawn, as it is drawn (Ui checks layering with them). Drawing done on the render target
    // directly (bitmaps) reports itself here.
    virtual void Painted(const Rect&) {}

private:
    ID2D1RenderTarget* rt_ = nullptr;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1LinearGradientBrush> logoBrush_;
    ComPtr<ID2D1StrokeStyle> dashStyle_;
    float dash_[2]{};  // the pattern dashStyle_ was made for, in stroke widths
};

struct UiInput {
    float mx = -1e6f, my = -1e6f;  // DIPs
    bool down = false;
    bool pressed = false;
    bool released = false;
    bool dblclick = false;
    bool rpressed = false;
    float wheel = 0;  // notches, + = up
    std::vector<UINT> keys;
    bool ctrl = false, shift = false, alt = false;
    std::wstring chars;      // typed (WM_CHAR, and text the IME finished)
    std::wstring composing;  // what the IME is putting together (a Korean syllable), until it is finished

    void ClearEdges() {
        pressed = released = dblclick = rpressed = false;
        wheel = 0;
        keys.clear();
        chars.clear();
    }
    bool Key(UINT vk) const { return std::find(keys.begin(), keys.end(), vk) != keys.end(); }
};

using UiId = uint32_t;
constexpr UiId HashId(const char* s, uint32_t n = 0) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    h = (h ^ n) * 16777619u;
    return h ? h : 1;
}

enum class BtnStyle { Primary, Secondary, Ghost, Danger };

// What a screen reader is told about a control (UI Automation), and how the keyboard reaches it. Every clickable drawn
// in a frame is a node; a Segmented row is a group node with one choice node per cell.
enum class Role : uint8_t { Button, Toggle, Choice, Group, Tab, Slider, Edit, Item };
struct UiNode {
    UiId id = 0;
    Rect r;  // DIPs, cut to the scroll view it is drawn in
    Rect full, view;  // as laid out, and the scroll view it is in (empty: none)
    Role role = Role::Button;
    std::wstring name;
    std::wstring keys;  // its shortcut, e.g. "Ctrl+Alt+R"
    bool enabled = true;
    bool on = false;  // a toggle that is on, the chosen cell or tab, the open item
    float value = 0, lo = 0, hi = 1;  // a slider
    bool settable = false;  // a Ui::Slider: the arrow keys and a reader set it (the editor's timeline only shows)
    std::wstring text;  // a text field's text
    int pos = 0, count = 0;  // a choice: its place in the row
    UiId group = 0;  // the row a choice is in
    bool blocked = false;  // drawn under a popover or a question, which take all input
};

// Immediate-mode widgets. Rendering happens only when input arrives or an animation is running.
class Ui : public Painter {
public:
    void Begin(ID2D1RenderTarget* rt, const UiInput& in, double now);
    void End();

    bool Animating() const { return animating_; }
    // Windows' "Show animations" (SPI_GETCLIENTAREAANIMATION): off, every Anim is at its target at once.
    void SetMotion(bool on) { motion_ = on; }
    bool Motion() const { return motion_; }
    double WakeAt() const { return wakeAt_; }  // absolute time a redraw is wanted, 0 = none
    void RequestWake(double t) { if (wakeAt_ == 0 || t < wakeAt_) wakeAt_ = t; }
    void RequestRedraw() { animating_ = true; }
    LPCWSTR Cursor() const { return cursor_; }
    void SetCursor(LPCWSTR c) { cursor_ = c; }
    // While input is blocked (a popover or a question over the page), the page sees no keys, clicks or pointer.
    const UiInput& In() const { return blocked_ ? none_ : in_; }
    double Now() const { return now_; }
    float Dt() const { return dt_; }
    void SetInputBlocked(bool b) { blocked_ = b; }

    // Clips drawing and hit testing (scroll views).
    void PushClip(const Rect& r) {
        Painter::PushClip(r);
        clips_.push_back(r);
    }
    void PopClip() {
        Painter::PopClip();
        clips_.pop_back();
    }

    bool Hover(const Rect& r) const;
    bool Click(UiId id, const Rect& r, bool enabled = true);
    bool Active(UiId id) const { return active_ == id; }
    bool ActiveAny() const { return active_ != 0; }
    float Anim(UiId id, float target, float speed = 16.f);
    float AnimFrom(UiId id, float target, float initial, float speed = 16.f);

    bool Button(UiId id, const Rect& r, std::wstring_view label, BtnStyle style, wchar_t icon = 0,
                bool enabled = true, float fontSize = 14.f);
    bool IconButton(UiId id, const Rect& r, wchar_t glyph, std::wstring_view tip = {}, bool enabled = true,
                    float size = 15.f);
    bool Segmented(UiId id, const Rect& r, std::initializer_list<std::wstring_view> items, int& sel,
                   uint32_t disabledMask = 0);
    bool Slider(UiId id, const Rect& r, float& v, float lo, float hi);
    bool Toggle(UiId id, const Rect& r, bool& v);
    bool ToggleRow(UiId id, const Rect& r, std::wstring_view label, bool& v, std::wstring_view sub = {},
                   bool enabled = true);
    void Tooltip(UiId id, const Rect& r, std::wstring_view text);
    void ProgressBar(const Rect& r, float t, D2D1_COLOR_F c);
    // One line of text. A click puts the caret in it; Enter, Esc or a click elsewhere lets go. While it has the focus
    // it takes every key, and the IME's text being composed shows underlined at the caret. True when text changed.
    bool TextField(UiId id, const Rect& r, std::wstring& text, std::wstring_view placeholder, size_t maxLen);
    void Focus(UiId id);  // the field takes the focus, caret at the end, when it is drawn next
    // Puts `s` where field `id` had its caret, over its selection (at the end if it never had the focus), and gives the
    // field the focus back: for a button beside it, whose press took the focus away. False when it doesn't fit.
    bool InsertText(UiId id, std::wstring& text, std::wstring_view s, size_t maxLen);
    bool TextFocused() const { return focus_ != 0; }
    const Rect& CaretRect() const { return caretRect_; }  // for the IME's windows
    // True once after a field let go of the focus with IME text in it: that text went into the field, so the IME
    // should drop it.
    bool TakeImeReset() { return std::exchange(imeReset_, false); }

    // Accessibility. The widgets name their nodes; a control drawn with Click alone is named with Describe after it, or
    // with Declare before it (which also puts it in a list when it is scrolled out and not drawn). Name gives its text
    // to the next node drawn without one (a toggle or slider in a settings row) or to the next Segmented row.
    void Name(std::wstring_view name) { nextName_ = name; }
    UiNode* Describe(UiId id, std::wstring_view name, Role role = Role::Button);
    UiNode& Declare(UiId id, const Rect& r, Role role, std::wstring_view name, bool enabled = true);
    const std::vector<UiNode>& Nodes() const { return shown_; }  // the last frame's, in drawing order
    const UiNode* Shown(UiId id) const;
    // Keyboard focus. Tab and Shift+Tab move it through the nodes in drawing order (into a Segmented row at its chosen
    // cell); once the keyboard moved it, Space and Enter press the focused control and the arrow keys move along a row
    // or a slider. The ring around it shows only then; a click puts the focus where it was, without the ring.
    UiId KeyboardFocus() const { return kbd_; }
    bool FocusRing() const { return ring_; }
    void SetKeyboardFocus(UiId id, bool ring = true);
    void Press(UiId id) { press_ = id; }  // as if clicked, in the next frame (a screen reader's Invoke)
    void SetSliderValue(UiId id, float v) { setId_ = id, setValue_ = v; }
    bool TakeFocusMoved() { return std::exchange(focusMoved_, false); }  // for the screen reader's focus event
    // A scroll view, before it draws: once the keyboard moved the focus to something in it, scrolls so it shows.
    void KeepInView(const Rect& view, float& scroll);

    // Layers. The page is drawn first; anything drawn over it (popovers, toasts, tooltips) goes between BeginFloat
    // and EndFloat, and the bounds of what each float draws are kept for the frame. A native layer is something DWM
    // composites above everything drawn in the window (the live window preview): its owner cuts it out under every
    // float, and anything else drawn over it, which it would hide, is counted as a layering error. Declare a native
    // layer once what lies under it has been drawn.
    void BeginFloat();
    void EndFloat();
    void NativeLayer(const Rect& r) { natives_.push_back(r); }
    const std::vector<Rect>& Floats() const { return floats_; }
    int LayerErrors() const { return layerErrors_; }
    const Rect& FirstLayerError() const { return layerError_; }
    void Painted(const Rect& r) override;

    // Tests (--readme-media): where each clickable was laid out last, for a scripted pointer to go to.
    void KeepRects(bool on) { keepRects_ = on; }
    const Rect* LastRect(UiId id) const {
        auto it = rects_.find(id);
        return it == rects_.end() ? nullptr : &it->second;
    }
    // The id of a Segmented control's cell i.
    static UiId CellId(UiId id, int i) { return (id ^ (uint32_t)(i + 1) * 2654435761u) | 1; }

private:
    UiInput in_, none_;
    double now_ = 0, last_ = 0;
    float dt_ = 0;
    UiId active_ = 0;
    bool pressTaken_ = false;
    bool animating_ = false;
    bool motion_ = true;
    bool blocked_ = false;
    double wakeAt_ = 0;
    LPCWSTR cursor_ = IDC_ARROW;
    std::unordered_map<UiId, float> anim_;
    std::vector<Rect> clips_;
    UiId tipId_ = 0;
    double tipStart_ = 0;
    std::wstring tipText_;
    Rect tipAnchor_;
    bool tipShow_ = false;
    std::vector<Rect> floats_, natives_;
    Rect floatBounds_, layerError_;
    int floatDepth_ = 0, layerErrors_ = 0;
    // The text field with the focus.
    UiId focus_ = 0;
    bool focusDrawn_ = false;  // this frame
    size_t caret_ = 0, anchor_ = 0;  // the selection runs between them
    UiId caretOf_ = 0;               // the field they were last in
    float scrollX_ = 0;
    double caretMoved_ = 0;  // the caret blinks from here
    Rect caretRect_;
    bool imeReset_ = false;
    bool keepRects_ = false;
    std::unordered_map<UiId, Rect> rects_;
    // Accessibility: this frame's nodes, the last frame's, and the keyboard focus.
    UiNode& Node(UiId id, const Rect& r, Role role, bool enabled, std::wstring_view name = {});
    void KeyboardInput();
    std::vector<UiNode> nodes_, shown_;
    std::unordered_map<UiId, size_t> nodeAt_, shownAt_;
    std::wstring nextName_;
    UiId kbd_ = 0, press_ = 0, setId_ = 0;
    float setValue_ = 0;
    int step_ = 0;  // arrow keys on the focused slider
    bool ring_ = false, focusMoved_ = false, blurField_ = false, scrollTo_ = false;
};
