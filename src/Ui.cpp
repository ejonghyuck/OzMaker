#include "pch.h"
#include "Ui.h"

using namespace theme;

// ---------------------------------------------------------------- Fonts

namespace {
IDWriteFactory* g_dw = nullptr;
std::unordered_map<uint64_t, ComPtr<IDWriteTextFormat>> g_formats;
std::unordered_map<IDWriteTextFormat*, ComPtr<IDWriteInlineObject>> g_ellipsis;
ComPtr<IDWriteTypography> g_tabular;
}  // namespace

void Fonts::Init(IDWriteFactory* f) {
    g_dw = f;
    if (SUCCEEDED(f->CreateTypography(&g_tabular))) {
        DWRITE_FONT_FEATURE feat{DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1};
        g_tabular->AddFontFeature(feat);
    }
}

IDWriteTextFormat* Fonts::Get(float size, DWRITE_FONT_WEIGHT w, const wchar_t* family) {
    uint64_t key = ((uint64_t)(size * 4) << 32) | ((uint64_t)w << 8) | (family == theme::IconFont ? 1 : 0);
    auto it = g_formats.find(key);
    if (it != g_formats.end()) return it->second.Get();
    ComPtr<IDWriteTextFormat> fmt;
    g_dw->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"ko-kr",
                           &fmt);
    if (fmt) {
        ComPtr<IDWriteInlineObject> ell;
        g_dw->CreateEllipsisTrimmingSign(fmt.Get(), &ell);
        g_ellipsis[fmt.Get()] = ell;
    }
    g_formats[key] = fmt;
    return fmt.Get();
}

ComPtr<IDWriteTextLayout> Fonts::Layout(std::wstring_view s, float size, const TextStyle& st, float w, float h,
                                        const wchar_t* family) {
    IDWriteTextFormat* fmt = Get(size, st.weight, family);
    ComPtr<IDWriteTextLayout> lay;
    if (!fmt || FAILED(g_dw->CreateTextLayout(s.data(), (UINT32)s.size(), fmt, std::max(0.f, w), std::max(0.f, h),
                                              &lay)))
        return nullptr;
    static const DWRITE_TEXT_ALIGNMENT ha[] = {DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_TEXT_ALIGNMENT_CENTER,
                                               DWRITE_TEXT_ALIGNMENT_TRAILING};
    static const DWRITE_PARAGRAPH_ALIGNMENT va[] = {DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                                                    DWRITE_PARAGRAPH_ALIGNMENT_FAR};
    lay->SetTextAlignment(ha[st.h]);
    lay->SetParagraphAlignment(va[st.v]);
    lay->SetWordWrapping(st.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!st.wrap) {
        DWRITE_TRIMMING tr{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        lay->SetTrimming(&tr, g_ellipsis[fmt].Get());
    }
    if (st.tabular && g_tabular) lay->SetTypography(g_tabular.Get(), {0, (UINT32)s.size()});
    return lay;
}

// ---------------------------------------------------------------- Painter

void Painter::Bind(ID2D1RenderTarget* rt) {
    if (rt != rt_) {
        rt_ = rt;
        brush_.Reset();
        logoBrush_.Reset();
    }
    if (!brush_) rt_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush_);
}

ID2D1SolidColorBrush* Painter::Brush(D2D1_COLOR_F c) {
    brush_->SetColor(c);
    return brush_.Get();
}

void Painter::Fill(const Rect& r, D2D1_COLOR_F c, float radius) {
    if (c.a <= 0 || r.Empty()) return;
    Painted(r);
    if (radius > 0)
        rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), Brush(c));
    else
        rt_->FillRectangle(r, Brush(c));
}

void Painter::Stroke(const Rect& r, D2D1_COLOR_F c, float radius, float width) {
    if (c.a <= 0) return;
    Painted(r.Inset(-width / 2));
    if (radius > 0)
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), Brush(c), width);
    else
        rt_->DrawRectangle(r, Brush(c), width);
}

void Painter::StrokeDashed(const Rect& r, D2D1_COLOR_F c, float radius, float width, float dash, float gap) {
    if (c.a <= 0) return;
    const float d[2] = {dash / width, gap / width};
    if (!dashStyle_ || dash_[0] != d[0] || dash_[1] != d[1]) {
        ComPtr<ID2D1Factory> f;
        rt_->GetFactory(&f);
        dashStyle_.Reset();
        D2D1_STROKE_STYLE_PROPERTIES p = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                                     D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10.f,
                                                                     D2D1_DASH_STYLE_CUSTOM, 0.f);
        if (FAILED(f->CreateStrokeStyle(p, d, 2, &dashStyle_))) return Stroke(r, c, radius, width);
        dash_[0] = d[0];
        dash_[1] = d[1];
    }
    Painted(r.Inset(-width / 2));
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), Brush(c), width, dashStyle_.Get());
}

void Painter::Line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float w) {
    Painted(Rect(std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1)).Inset(-w / 2));
    rt_->DrawLine({x0, y0}, {x1, y1}, Brush(c), w);
}

void Painter::Circle(float cx, float cy, float r, D2D1_COLOR_F c) {
    Painted(Rect(cx - r, cy - r, cx + r, cy + r));
    rt_->FillEllipse(D2D1::Ellipse({cx, cy}, r, r), Brush(c));
}

void Painter::Ring(float cx, float cy, float r, D2D1_COLOR_F c, float w) {
    Painted(Rect(cx - r, cy - r, cx + r, cy + r).Inset(-w / 2));
    rt_->DrawEllipse(D2D1::Ellipse({cx, cy}, r, r), Brush(c), w);
}

void Painter::Text(const Rect& r, std::wstring_view s, float size, D2D1_COLOR_F c, const TextStyle& st) {
    if (s.empty() || c.a <= 0) return;
    Painted(r);
    auto lay = Fonts::Layout(s, size, st, r.W(), r.H());
    if (lay) rt_->DrawTextLayout({r.l, r.t}, lay.Get(), Brush(c), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void Painter::Icon(const Rect& r, wchar_t glyph, float size, D2D1_COLOR_F c) {
    Painted(r);
    wchar_t s[2] = {glyph, 0};
    TextStyle st;
    st.h = AlignCenter;
    auto lay = Fonts::Layout({s, 1}, size, st, r.W(), r.H(), theme::IconFont);
    if (lay) rt_->DrawTextLayout({r.l, r.t}, lay.Get(), Brush(c), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

float Painter::Measure(std::wstring_view s, float size, DWRITE_FONT_WEIGHT w) {
    TextStyle st;
    st.weight = w;
    auto lay = Fonts::Layout(s, size, st, 10000.f, 100.f);
    if (!lay) return 0;
    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

void Painter::PushClip(const Rect& r) { rt_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED); }
void Painter::PopClip() { rt_->PopAxisAlignedClip(); }

void Painter::Logo(const Rect& r) {
    if (!logoBrush_) {
        D2D1_GRADIENT_STOP stops[] = {{0.f, Accent}, {1.f, Accent2}};
        ComPtr<ID2D1GradientStopCollection> coll;
        rt_->CreateGradientStopCollection(stops, 2, &coll);
        rt_->CreateLinearGradientBrush({}, coll.Get(), &logoBrush_);
    }
    logoBrush_->SetStartPoint({r.l, r.t});
    logoBrush_->SetEndPoint({r.r, r.b});
    float rad = r.W() * 0.28f;
    Painted(r);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), logoBrush_.Get());
    float cx = r.CX(), cy = r.CY(), s = r.W();
    Ring(cx, cy, s * 0.27f, White, s * 0.085f);
    Circle(cx, cy, s * 0.12f, White);
}

void Painter::Crosshair(float cx, float cy, float s, D2D1_COLOR_F c) {
    float r = s * 0.32f, w = std::max(1.2f, s * 0.09f);
    Ring(cx, cy, r, c, w);
    Line(cx, cy - s * 0.5f, cx, cy - r * 0.45f, c, w);
    Line(cx, cy + r * 0.45f, cx, cy + s * 0.5f, c, w);
    Line(cx - s * 0.5f, cy, cx - r * 0.45f, cy, c, w);
    Line(cx + r * 0.45f, cy, cx + s * 0.5f, cy, c, w);
}

// ---------------------------------------------------------------- Ui

void Ui::Begin(ID2D1RenderTarget* rt, const UiInput& in, double now) {
    Bind(rt);
    in_ = in;
    last_ = now_;
    now_ = now;
    dt_ = last_ > 0 ? (float)std::clamp(now_ - last_, 0.0, 0.05) : 0.016f;
    animating_ = false;
    wakeAt_ = 0;
    pressTaken_ = false;
    cursor_ = IDC_ARROW;
    tipShow_ = false;
    clips_.clear();
    floats_.clear();
    natives_.clear();
    floatDepth_ = layerErrors_ = 0;
}

void Ui::End() {
    if (tipShow_ && !tipText_.empty()) {
        BeginFloat();
        float tw = Measure(tipText_, 12.f) + 16.f;
        Rect r = Rect::XYWH(tipAnchor_.CX() - tw / 2, tipAnchor_.b + 6, tw, 26);
        D2D1_SIZE_F sz = RT()->GetSize();
        if (r.r > sz.width - 4) r = r.Offset(sz.width - 4 - r.r, 0);
        if (r.l < 4) r = r.Offset(4 - r.l, 0);
        if (r.b > sz.height - 4) r = r.Offset(0, -(r.H() + tipAnchor_.H() + 12));
        Fill(r, Hex(0x2B303B), 6);
        Stroke(r.Inset(0.5f), BorderStrong, 6);
        TextStyle st;
        st.h = AlignCenter;
        Text(r, tipText_, 12.f, TextColor, st);
        EndFloat();
    }
    if (!in_.down || in_.released) active_ = 0;
}

void Ui::BeginFloat() {
    if (floatDepth_++ == 0) floatBounds_ = {};
}

void Ui::EndFloat() {
    if (--floatDepth_ == 0 && !floatBounds_.Empty()) floats_.push_back(floatBounds_);
}

void Ui::Painted(const Rect& r) {
    Rect b = clips_.empty() ? r : r.Intersect(clips_.back());
    if (b.Empty()) return;
    if (floatDepth_ > 0) {
        floatBounds_ = floatBounds_.Empty() ? b : floatBounds_.Union(b);
        return;
    }
    for (const Rect& n : natives_) {
        Rect o = b.Intersect(n);
        if (o.W() >= 1 && o.H() >= 1) {  // an anti-aliased edge just touching it doesn't count
            if (layerErrors_++ == 0) layerError_ = b;
            return;
        }
    }
}

bool Ui::Hover(const Rect& r) const {
    if (blocked_) return false;
    if (!r.Contains(in_.mx, in_.my)) return false;
    return clips_.empty() || clips_.back().Contains(in_.mx, in_.my);
}

bool Ui::Click(UiId id, const Rect& r, bool enabled) {
    bool h = enabled && Hover(r);
    if (h) cursor_ = IDC_HAND;
    if (h && in_.pressed && !pressTaken_) {
        active_ = id;
        pressTaken_ = true;
    }
    return active_ == id && in_.released && h;
}

float Ui::AnimFrom(UiId id, float target, float initial, float speed) {
    auto [it, inserted] = anim_.try_emplace(id, initial);
    float& v = it->second;
    if (v != target) {
        float k = 1.f - std::exp(-speed * dt_);
        v += (target - v) * k;
        if (std::fabs(target - v) < 0.002f)
            v = target;
        else
            animating_ = true;
    }
    return v;
}

float Ui::Anim(UiId id, float target, float speed) { return AnimFrom(id, target, target, speed); }

bool Ui::Button(UiId id, const Rect& r, std::wstring_view label, BtnStyle style, wchar_t icon, bool enabled,
                float fs) {
    bool clicked = Click(id, r, enabled);
    bool hot = enabled && Hover(r);
    bool down = enabled && active_ == id && in_.down;
    float h = Anim(id, hot ? 1.f : 0.f);

    D2D1_COLOR_F bg{}, fg{}, border{};
    switch (style) {
        case BtnStyle::Primary: bg = Mix(Accent, AccentHi, h); fg = White; break;
        case BtnStyle::Danger: bg = Mix(Rec, RecHi, h); fg = White; break;
        case BtnStyle::Secondary: bg = Mix(Surface2, Surface3, h); fg = TextColor; border = Border; break;
        case BtnStyle::Ghost: bg = WithAlpha(Surface3, h); fg = Mix(TextDim, TextColor, h); break;
    }
    if (!enabled) {
        bg = style == BtnStyle::Ghost ? Clear : Surface2;
        fg = TextFaint;
        border = Clear;
    }
    if (down) bg = Mix(bg, Black, 0.12f);
    float rad = std::min(9.f, r.H() / 2);
    Fill(r, bg, rad);
    if (border.a > 0) Stroke(r.Inset(0.5f), border, rad);

    float tw = label.empty() ? 0 : Measure(label, fs, kSemibold);
    float iw = icon ? fs + 2 : 0;
    float gap = (icon && !label.empty()) ? 8.f : 0.f;
    float total = tw + iw + gap;
    float x = std::round(r.CX() - total / 2);
    if (icon) Icon(Rect::XYWH(x, r.t, iw, r.H()), icon, fs, fg);
    if (!label.empty()) {
        TextStyle st;
        st.weight = kSemibold;
        Text(Rect(x + iw + gap, r.t, x + total + 4, r.b), label, fs, fg, st);
    }
    return clicked;
}

bool Ui::IconButton(UiId id, const Rect& r, wchar_t glyph, std::wstring_view tip, bool enabled, float size) {
    bool clicked = Click(id, r, enabled);
    bool hot = enabled && Hover(r);
    float h = Anim(id, hot ? 1.f : 0.f);
    bool down = enabled && active_ == id && in_.down;
    D2D1_COLOR_F bg = WithAlpha(down ? BorderStrong : Surface3, h);
    Fill(r, bg, std::min(8.f, r.H() / 2));
    Icon(r, glyph, size, enabled ? Mix(TextDim, TextColor, h) : TextFaint);
    if (!tip.empty()) Tooltip(id, r, tip);
    return clicked;
}

bool Ui::Segmented(UiId id, const Rect& r, std::initializer_list<std::wstring_view> items, int& sel,
                   uint32_t disabledMask) {
    int n = (int)items.size();
    if (n == 0) return false;
    Fill(r, Surface2, 9);
    const float pad = 3;
    float iw = (r.W() - pad * 2) / n;
    if (sel >= 0 && sel < n) {
        float pos = AnimFrom(id, (float)sel, (float)sel, 20.f);
        Rect pill = Rect::XYWH(r.l + pad + pos * iw, r.t + pad, iw, r.H() - pad * 2);
        Fill(pill, Hex(0x2C3240), 7);
        Stroke(pill.Inset(0.5f), Hex(0x3A4252), 7);
    }
    bool changed = false;
    int i = 0;
    for (auto& s : items) {
        Rect cell = Rect::XYWH(r.l + pad + i * iw, r.t + pad, iw, r.H() - pad * 2);
        UiId cid = (id ^ (uint32_t)(i + 1) * 2654435761u) | 1;
        bool en = !(disabledMask & (1u << i));
        if (Click(cid, cell, en) && sel != i) {
            sel = i;
            changed = true;
        }
        float h = Anim(cid, (en && Hover(cell)) ? 1.f : 0.f);
        D2D1_COLOR_F fg = !en ? WithAlpha(TextFaint, 0.6f) : (i == sel ? TextColor : Mix(TextDim, TextColor, h));
        TextStyle st;
        st.h = AlignCenter;
        st.weight = i == sel ? kSemibold : DWRITE_FONT_WEIGHT_NORMAL;
        Text(cell, s, 13.f, fg, st);
        ++i;
    }
    return changed;
}

bool Ui::Slider(UiId id, const Rect& r, float& v, float lo, float hi) {
    bool hov = Hover(r);
    Click(id, r);
    bool changed = false;
    if (active_ == id && in_.down) {
        float t = std::clamp((in_.mx - r.l) / std::max(1.f, r.W()), 0.f, 1.f);
        float nv = lo + t * (hi - lo);
        if (nv != v) {
            v = nv;
            changed = true;
        }
    }
    if (hov) cursor_ = IDC_HAND;
    float t = std::clamp((v - lo) / (hi - lo), 0.f, 1.f);
    float cy = r.CY();
    Fill(Rect(r.l, cy - 2, r.r, cy + 2), Surface3, 2);
    Fill(Rect(r.l, cy - 2, r.l + t * r.W(), cy + 2), Accent, 2);
    float h = Anim(id, (hov || active_ == id) ? 1.f : 0.f);
    float kx = r.l + t * r.W();
    if (h > 0) Circle(kx, cy, 7 + 5 * h, WithAlpha(Accent, 0.25f * h));
    Circle(kx, cy, 7, White);
    return changed;
}

bool Ui::Toggle(UiId id, const Rect& r, bool& v) {
    bool changed = false;
    if (Click(id, r)) {
        v = !v;
        changed = true;
    }
    float t = AnimFrom(id, v ? 1.f : 0.f, v ? 1.f : 0.f, 18.f);
    Fill(r, Mix(Surface3, Accent, t), r.H() / 2);
    float kr = r.H() / 2 - 3;
    float kx = r.l + r.H() / 2 + t * (r.W() - r.H());
    Circle(kx, r.CY(), kr, White);
    return changed;
}

bool Ui::ToggleRow(UiId id, const Rect& r, std::wstring_view label, bool& v, std::wstring_view sub, bool enabled) {
    bool changed = false;
    UiId rowId = id ^ 0x5bd1e995u;
    if (Click(rowId, r, enabled)) {
        v = !v;
        changed = true;
    }
    bool on = v && enabled;
    float h = Anim(rowId, enabled && Hover(r) ? 1.f : 0.f);
    Fill(r, WithAlpha(Surface2, h), 8);
    Rect sw = Rect::XYWH(r.r - 10 - 36, r.CY() - 10, 36, 20);
    float t = AnimFrom(id, on ? 1.f : 0.f, on ? 1.f : 0.f, 18.f);
    Fill(sw, WithAlpha(Mix(Surface3, Accent, t), enabled ? 1.f : 0.5f), 10);
    Circle(sw.l + 10 + t * 16, sw.CY(), 7, WithAlpha(White, enabled ? 1.f : 0.4f));
    Rect tr = Rect(r.l + 10, r.t, sw.l - 8, r.b);
    if (sub.empty()) {
        Text(tr, label, 13.f, enabled ? TextColor : TextFaint);
    } else {
        Text(Rect(tr.l, tr.t + 5, tr.r, tr.CY() + 1), label, 13.f, enabled ? TextColor : TextFaint);
        Text(Rect(tr.l, tr.CY() - 1, tr.r, tr.b - 5), sub, 11.5f, TextFaint);
    }
    return changed;
}

void Ui::Tooltip(UiId id, const Rect& r, std::wstring_view text) {
    if (!Hover(r) || in_.down) {
        if (tipId_ == id) tipId_ = 0;
        return;
    }
    if (tipId_ != id) {
        tipId_ = id;
        tipStart_ = now_;
    }
    double showAt = tipStart_ + 0.45;
    if (now_ >= showAt) {
        tipShow_ = true;
        tipText_ = text;
        tipAnchor_ = r;
    } else {
        RequestWake(showAt);
    }
}

void Ui::ProgressBar(const Rect& r, float t, D2D1_COLOR_F c) {
    Fill(r, Surface3, r.H() / 2);
    t = std::clamp(t, 0.f, 1.f);
    if (t > 0) Fill(Rect(r.l, r.t, r.l + std::max(r.H(), r.W() * t), r.b), c, r.H() / 2);
}
