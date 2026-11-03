#include "pch.h"
#include "Theme.h"

namespace theme {

Palette Light() {
    return {.Bg = Hex(0xF3F4F7), .Surface = Hex(0xFFFFFF), .Surface2 = Hex(0xEEF0F4), .Surface3 = Hex(0xE2E5EB),
            .Border = Hex(0xE1E4EA), .BorderStrong = Hex(0xCDD2DA),
            .TextColor = Hex(0x1A1D24), .TextDim = Hex(0x4E5563), .TextFaint = Hex(0x5D6472),
            .Accent = Hex(0x6D4AEF), .AccentHi = Hex(0x5B3BE0), .Accent2 = Hex(0xD9266A), .Rec = Hex(0xC02435),
            .RecHi = Hex(0xA81E2E), .Success = Hex(0x03704F), .Warning = Hex(0xA14807),
            .AccentFill = Hex(0x6D4AEF), .AccentFillHi = Hex(0x5B3BE0), .RecFill = Hex(0xD92D41),
            .RecFillHi = Hex(0xC02435), .Badge = Hex(0xD9266A), .OnFill = Hex(0xFFFFFF), .UpdateFill = Hex(0x047857),
            .UpdateFillHi = Hex(0x065F46), .UpdateInk = Hex(0xFFFFFF),
            .Toast = Hex(0xFFFFFF), .ToastError = Hex(0xFDEDEF), .Tip = Hex(0xFFFFFF),
            .Pill = Hex(0xFFFFFF), .PillLine = Hex(0xCDD2DA), .Preview = Hex(0xE4E7EC),
            .SavedCard = Hex(0xECFDF5), .Dashed = Hex(0xB9A8FF),
            .Scrim = Hex(0x101828, 0.6f), .DropScrim = Hex(0xF3F4F7), .Shadow = Hex(0x101828, 0.4f),
            .Link = Hex(0x5B3BE0), .LinkHi = Hex(0x4527C9),
            .ToggleOff = Hex(0x8A91A0), .Knob = Hex(0xFFFFFF), .Disabled = Hex(0x5D6472), .Focus = Hex(0x1A1D24),
            .light = true, .contrast = false};
}

namespace {

D2D1_COLOR_F FromRef(COLORREF c) { return Hex((GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c)); }

double Luminance(D2D1_COLOR_F c) {
    auto lin = [](float x) { return x <= 0.04045f ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4); };
    return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b);
}

// Everything is the window's color, its text, or the highlight: controls get outlines in its text color instead of
// shades of gray, and what is chosen is the highlight with the highlight's text on it.
Palette FromSystem(COLORREF window, COLORREF text, COLORREF highlight, COLORREF highlightText, COLORREF hotlight,
                   COLORREF gray) {
    const D2D1_COLOR_F w = FromRef(window), t = FromRef(text), h = FromRef(highlight), ht = FromRef(highlightText);
    const D2D1_COLOR_F clear = {0, 0, 0, 0};
    return {.Bg = w, .Surface = w, .Surface2 = w, .Surface3 = w, .Border = t, .BorderStrong = t,
            .TextColor = t, .TextDim = t, .TextFaint = t,
            .Accent = h, .AccentHi = h, .Accent2 = h, .Rec = t, .RecHi = t, .Success = t, .Warning = t,
            .AccentFill = h, .AccentFillHi = h, .RecFill = h, .RecFillHi = h, .Badge = h, .OnFill = ht,
            .UpdateFill = h, .UpdateFillHi = h, .UpdateInk = ht,
            .Toast = w, .ToastError = w, .Tip = w, .Pill = h, .PillLine = h, .Preview = w,
            .SavedCard = w, .Dashed = t,
            .Scrim = WithAlpha(w, 0.6f), .DropScrim = w, .Shadow = clear,
            .Link = FromRef(hotlight), .LinkHi = FromRef(hotlight),
            .ToggleOff = w, .Knob = t, .Disabled = FromRef(gray), .Focus = t,
            .light = Luminance(w) > 0.5, .contrast = true};
}

}  // namespace

Palette HighContrast() {
    return FromSystem(GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOWTEXT), GetSysColor(COLOR_HIGHLIGHT),
                      GetSysColor(COLOR_HIGHLIGHTTEXT), GetSysColor(COLOR_HOTLIGHT), GetSysColor(COLOR_GRAYTEXT));
}

Palette HighContrastBlack() {
    return FromSystem(RGB(0, 0, 0), RGB(255, 255, 255), RGB(0x1A, 0xEB, 0xFF), RGB(0, 0, 0), RGB(255, 255, 0),
                      RGB(0x3F, 0xF2, 0x3F));
}

double ContrastRatio(D2D1_COLOR_F a, D2D1_COLOR_F b) {
    double la = Luminance(a), lb = Luminance(b);
    if (la < lb) std::swap(la, lb);
    return (la + 0.05) / (lb + 0.05);
}

}  // namespace theme
