#pragma once
#include <string>
#include <string_view>

namespace util {

std::wstring Utf8ToWide(std::string_view s);
std::string WideToUtf8(std::wstring_view s);

// QueryPerformanceCounter in 100ns units: the same clock as Direct3D11CaptureFrame::SystemRelativeTime.
int64_t Qpc100ns();
double NowSeconds();

std::wstring FormatClock(double seconds, bool tenths);   // 0:12.3 / 1:02:03
std::wstring FormatBytes(uint64_t bytes);
std::wstring Format(const wchar_t* fmt, ...);

std::wstring KnownFolder(REFKNOWNFOLDERID id);
std::wstring AppDataDir();               // %APPDATA%\OzMaker, created on demand
bool EnsureDir(const std::wstring& dir);
std::wstring TimestampName();            // 20261102_142311
std::wstring FileNameOf(const std::wstring& path);
uint64_t FileSize(const std::wstring& path);

void OpenFolderAndSelect(const std::wstring& path);
void ShellOpen(const std::wstring& path);
bool CopyFileToClipboard(HWND owner, const std::wstring& path);
bool PickFolder(HWND owner, const std::wstring& initial, std::wstring& out);
bool PickSavePath(HWND owner, const std::wstring& dir, const std::wstring& name,
                  const wchar_t* ext, const wchar_t* filterName, std::wstring& out);
bool PickVideoFile(HWND owner, const std::wstring& dir, std::wstring& out);
std::wstring FormatAgo(double seconds);  // "방금", "3분 전", "어제" ...

std::wstring HotkeyToString(UINT mods, UINT vk);

}  // namespace util
