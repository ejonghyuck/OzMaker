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

inline constexpr D2D1_COLOR_F Bg = Hex(0x0D0F13);
inline constexpr D2D1_COLOR_F Surface = Hex(0x15181E);
inline constexpr D2D1_COLOR_F Surface2 = Hex(0x1C2028);
inline constexpr D2D1_COLOR_F Surface3 = Hex(0x252A34);
inline constexpr D2D1_COLOR_F Border = Hex(0x262B35);
inline constexpr D2D1_COLOR_F BorderStrong = Hex(0x343B48);
inline constexpr D2D1_COLOR_F TextColor = Hex(0xECEEF3);
inline constexpr D2D1_COLOR_F TextDim = Hex(0xA1A8B5);
inline constexpr D2D1_COLOR_F TextFaint = Hex(0x6B7281);
inline constexpr D2D1_COLOR_F Accent = Hex(0x8B6CFF);
inline constexpr D2D1_COLOR_F AccentHi = Hex(0xA08AFF);
inline constexpr D2D1_COLOR_F Accent2 = Hex(0xFF5E87);
inline constexpr D2D1_COLOR_F Rec = Hex(0xFF4D5E);
inline constexpr D2D1_COLOR_F RecHi = Hex(0xFF6B79);
inline constexpr D2D1_COLOR_F Success = Hex(0x34D399);
inline constexpr D2D1_COLOR_F Warning = Hex(0xFBBF24);
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
inline constexpr wchar_t Download = 0xE896;
}  // namespace icon

}  // namespace theme
