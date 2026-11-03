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
std::wstring ExePath();
std::wstring ExeDir();                   // the folder ExePath is in
std::wstring AppDataDir();               // %APPDATA%\OzMaker, created on demand
bool EnsureDir(const std::wstring& dir);
std::wstring TimestampName();            // 20261102_142311
std::wstring FileNameOf(const std::wstring& path);
uint64_t FileSize(const std::wstring& path);
uint64_t FreeBytes(const std::wstring& dir);  // free space on its drive for this user, UINT64_MAX if unknown

void OpenFolderAndSelect(const std::wstring& path);
// Opens a file, folder or link (with args for a program) through Explorer, so that what starts isn't a child of
// OzMaker: recordings leave out OzMaker's process tree (AudioCapture::Source::Others), and a player or browser
// started from here must still be heard in them. False when Explorer couldn't, and OzMaker started it itself.
bool ShellOpen(const std::wstring& path, const std::wstring& args = {});
bool CopyFileToClipboard(HWND owner, const std::wstring& path);
bool MoveToRecycleBin(HWND owner, const std::wstring& path);  // false if the file is still there
bool PickFolder(HWND owner, const std::wstring& initial, std::wstring& out);
bool PickSavePath(HWND owner, const std::wstring& dir, const std::wstring& name,
                  const wchar_t* ext, const wchar_t* filterName, std::wstring& out);
bool PickVideoFile(HWND owner, const std::wstring& dir, std::wstring& out);
std::wstring FormatAgo(double seconds);  // "방금", "3분 전", "어제" ...

std::wstring HotkeyToString(UINT mods, UINT vk);

// Characters Windows doesn't allow in file names become '_'; trimmed to maxLen.
std::wstring SafeFileName(std::wstring_view s, size_t maxLen = 80);
// Start with Windows (HKCU Run, "OzMaker" -> "<this exe>" --tray).
bool AutostartEnabled();
bool SetAutostart(bool on);
// The process has exited. False while it runs, also under another account or elevated (access denied).
bool ProcessGone(DWORD pid);

// What is left of a rectangle with others taken out of it, as non-overlapping rectangles.
std::vector<RECT> SubtractRects(const RECT& whole, const std::vector<RECT>& holes);

}  // namespace util
