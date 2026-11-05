#pragma once
#include <d2d1.h>
#include <cstdint>

namespace theme {

constexpr D2D1_COLOR_F Hex(uint32_t rgb, float a = 1.f) {
    return {((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a};
}
constexpr D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) { return {c.r, c.g, c.b, c.a * a}; }
constexpr D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

// The colors of a theme (설정 › 일반 › 테마, or the contrast theme Windows is in). Text colors stand out from the
// surfaces they are drawn on by 4.5:1 or more, and OnFill from the fills (tests check it).
struct Palette {
    D2D1_COLOR_F Bg, Surface, Surface2, Surface3, Border, BorderStrong;
    D2D1_COLOR_F TextColor, TextDim, TextFaint;
    D2D1_COLOR_F Accent, AccentHi, Accent2, Rec, RecHi, Success, Warning;
    // Buttons and badges with OnFill text on them; the green update button with UpdateInk.
    D2D1_COLOR_F AccentFill, AccentFillHi, RecFill, RecFillHi, Badge, OnFill, UpdateFill, UpdateFillHi, UpdateInk;
    D2D1_COLOR_F Toast, ToastError, Tip;   // a toast, an error toast, a tooltip
    D2D1_COLOR_F Pill, PillLine;           // the chosen cell of a Segmented row
    D2D1_COLOR_F Preview;                  // under the capture preview and the editor's frame
    D2D1_COLOR_F SavedCard, Dashed;        // the saved-video card; the edit tab's drop zone
    D2D1_COLOR_F Scrim, DropScrim, Shadow;  // under a question, under the drop cue; drop shadows (scaled by alpha)
    D2D1_COLOR_F Link, LinkHi;
    D2D1_COLOR_F ToggleOff, Knob, Disabled, Focus;  // a switch that is off, a knob, a control's text when off, the ring
    bool light, contrast;  // a light theme; Windows' contrast theme (its system colors, controls outlined)
};

constexpr Palette Dark() {
    return {.Bg = Hex(0x0D0F13), .Surface = Hex(0x15181E), .Surface2 = Hex(0x1C2028), .Surface3 = Hex(0x252A34),
            .Border = Hex(0x262B35), .BorderStrong = Hex(0x343B48),
            .TextColor = Hex(0xECEEF3), .TextDim = Hex(0xA1A8B5), .TextFaint = Hex(0x8A91A0),
            .Accent = Hex(0x8B6CFF), .AccentHi = Hex(0xA08AFF), .Accent2 = Hex(0xFF5E87), .Rec = Hex(0xFF4D5E),
            .RecHi = Hex(0xFF6B79), .Success = Hex(0x34D399), .Warning = Hex(0xFBBF24),
            .AccentFill = Hex(0x7A5AF8), .AccentFillHi = Hex(0x6D4AEF), .RecFill = Hex(0xDC3545),
            .RecFillHi = Hex(0xC82333), .Badge = Hex(0xD9266A), .OnFill = Hex(0xFFFFFF), .UpdateFill = Hex(0x34D399),
            .UpdateFillHi = Hex(0x6EE7B7), .UpdateInk = Hex(0x06281C),
            .Toast = Hex(0x222733), .ToastError = Hex(0x3A1A20), .Tip = Hex(0x2B303B),
            .Pill = Hex(0x2C3240), .PillLine = Hex(0x3A4252), .Preview = Hex(0x0A0B0E),
            .SavedCard = Hex(0x14231D), .Dashed = Hex(0x4B3D99),
            .Scrim = Hex(0x000000), .DropScrim = Hex(0x08090C), .Shadow = Hex(0x000000),
            .Link = Hex(0xA08AFF), .LinkHi = Hex(0xC4B5FF),
            .ToggleOff = Hex(0x252A34), .Knob = Hex(0xFFFFFF), .Disabled = Hex(0x8A91A0), .Focus = Hex(0xECEEF3),
            .light = false, .contrast = false};
}
Palette Light();
Palette HighContrast();  // from Windows' system colors
Palette HighContrastBlack();  // Windows 10's "High Contrast Black", for tests

// The colors in use. Overlays over other programs keep the dark ones (or the contrast theme's): Use swaps them in
// while they draw.
inline Palette P = Dark();
inline const D2D1_COLOR_F& Bg = P.Bg;
inline const D2D1_COLOR_F& Surface = P.Surface;
inline const D2D1_COLOR_F& Surface2 = P.Surface2;
inline const D2D1_COLOR_F& Surface3 = P.Surface3;
inline const D2D1_COLOR_F& Border = P.Border;
inline const D2D1_COLOR_F& BorderStrong = P.BorderStrong;
inline const D2D1_COLOR_F& TextColor = P.TextColor;
inline const D2D1_COLOR_F& TextDim = P.TextDim;
inline const D2D1_COLOR_F& TextFaint = P.TextFaint;
inline const D2D1_COLOR_F& Accent = P.Accent;
inline const D2D1_COLOR_F& AccentHi = P.AccentHi;
inline const D2D1_COLOR_F& Accent2 = P.Accent2;
inline const D2D1_COLOR_F& Rec = P.Rec;
inline const D2D1_COLOR_F& RecHi = P.RecHi;
inline const D2D1_COLOR_F& Success = P.Success;
inline const D2D1_COLOR_F& Warning = P.Warning;
inline const D2D1_COLOR_F& AccentFill = P.AccentFill;
inline const D2D1_COLOR_F& AccentFillHi = P.AccentFillHi;
inline const D2D1_COLOR_F& RecFill = P.RecFill;
inline const D2D1_COLOR_F& RecFillHi = P.RecFillHi;
inline const D2D1_COLOR_F& Badge = P.Badge;
inline const D2D1_COLOR_F& OnFill = P.OnFill;
inline const D2D1_COLOR_F& Link = P.Link;
inline const D2D1_COLOR_F& LinkHi = P.LinkHi;
inline const D2D1_COLOR_F& Shadow = P.Shadow;
inline bool Contrast() { return P.contrast; }

class Use {
public:
    explicit Use(const Palette& p) : was_(P) { P = p; }
    ~Use() { P = was_; }
    Use(const Use&) = delete;
    Use& operator=(const Use&) = delete;

private:
    Palette was_;
};
// What overlays over other programs draw with: the dark colors, or the contrast theme's.
inline Palette OverlayPalette() { return P.contrast ? P : Dark(); }

// Contrast ratio of two opaque colors (WCAG 2), 1 to 21.
double ContrastRatio(D2D1_COLOR_F a, D2D1_COLOR_F b);

inline constexpr D2D1_COLOR_F White = Hex(0xFFFFFF);
inline constexpr D2D1_COLOR_F Black = Hex(0x000000);
inline constexpr D2D1_COLOR_F Clear = {0, 0, 0, 0};

inline constexpr const wchar_t* UiFont = L"Segoe UI";
inline constexpr const wchar_t* IconFont = L"Segoe MDL2 Assets";

// Segoe MDL2 Assets code points
namespace icon {
inline constexpr wchar_t Settings = 0xE713;
inline constexpr wchar_t Refresh = 0xE72C;
inline constexpr wchar_t Back = 0xE72B;
inline constexpr wchar_t Close = 0xE711;
inline constexpr wchar_t Play = 0xE768;
inline constexpr wchar_t Pause = 0xE769;
inline constexpr wchar_t Stop = 0xE71A;
inline constexpr wchar_t Folder = 0xE8B7;
inline constexpr wchar_t Copy = 0xE8C8;
inline constexpr wchar_t Save = 0xE74E;
inline constexpr wchar_t Crop = 0xE7A8;
inline constexpr wchar_t Camera = 0xE722;
inline constexpr wchar_t Check = 0xE73E;
inline constexpr wchar_t Warning = 0xE7BA;
inline constexpr wchar_t Info = 0xE946;
inline constexpr wchar_t OpenFile = 0xE8E5;
inline constexpr wchar_t Keyboard = 0xE765;
inline constexpr wchar_t Monitor = 0xE7F4;
inline constexpr wchar_t AppWindow = 0xE737;
inline constexpr wchar_t Prev = 0xE76B;
inline constexpr wchar_t Next = 0xE76C;
inline constexpr wchar_t Delete = 0xE74D;
inline constexpr wchar_t Volume = 0xE767;
inline constexpr wchar_t Mute = 0xE74F;
inline constexpr wchar_t Cut = 0xE8C6;
inline constexpr wchar_t Video = 0xE714;
inline constexpr wchar_t Photo = 0xE91B;
inline constexpr wchar_t Record = 0xE7C8;
inline constexpr wchar_t ChevronDown = 0xE70D;
inline constexpr wchar_t ChevronUp = 0xE70E;
inline constexpr wchar_t Download = 0xE896;
inline constexpr wchar_t Timer = 0xE916;  // stopwatch
inline constexpr wchar_t Font = 0xE8D2;
inline constexpr wchar_t Replay = 0xE81C;  // History: a clock with an arrow back
inline constexpr wchar_t More = 0xE712;
}  // namespace icon

}  // namespace theme
