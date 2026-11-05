#include "pch.h"
#include "Util.h"

#include <cstdarg>
#include <exdisp.h>
#include <shldisp.h>

namespace util {

std::wstring Utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

std::string WideToUtf8(std::wstring_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

int64_t Qpc100ns() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (c.QuadPart / freq) * 10'000'000 + (c.QuadPart % freq) * 10'000'000 / freq;
}

double NowSeconds() { return Qpc100ns() / 1e7; }

std::wstring Format(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}

std::wstring FormatClock(double seconds, bool tenths) {
    if (seconds < 0) seconds = 0;
    int64_t t = (int64_t)(seconds * 10.0 + 1e-6);
    int64_t tenth = t % 10;
    int64_t s = t / 10;
    int64_t h = s / 3600, m = (s / 60) % 60, sec = s % 60;
    if (h > 0) return Format(L"%lld:%02lld:%02lld", h, m, sec);
    if (tenths) return Format(L"%lld:%02lld.%lld", m, sec, tenth);
    return Format(L"%lld:%02lld", m, sec);
}

std::wstring FormatBytes(uint64_t b) {
    if (b < 1024) return Format(L"%llu B", b);
    if (b < 1024 * 1024) return Format(L"%.0f KB", b / 1024.0);
    if (b < 1024ull * 1024 * 1024) return Format(L"%.1f MB", b / (1024.0 * 1024.0));
    return Format(L"%.2f GB", b / (1024.0 * 1024.0 * 1024.0));
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

bool EnsureDir(const std::wstring& dir) {
    if (dir.empty()) return false;
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr) == ERROR_SUCCESS;
}

uint64_t FreeBytes(const std::wstring& dir) {
    ULARGE_INTEGER avail{};
    return GetDiskFreeSpaceExW(dir.c_str(), &avail, nullptr, nullptr) ? avail.QuadPart : UINT64_MAX;
}

std::wstring ExePath() {
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, exe, _countof(exe));
    return std::wstring(exe, n);
}

std::wstring ExeDir() {
    std::wstring p = ExePath();
    return p.substr(0, p.find_last_of(L'\\'));
}

std::wstring AppDataDir() {
    std::wstring d = KnownFolder(FOLDERID_RoamingAppData) + L"\\OzMaker";
    EnsureDir(d);
    return d;
}

std::wstring TimestampName() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return Format(L"%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

std::wstring FileNameOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

static std::wstring LowerExt(const std::wstring& path) {
    std::wstring name = FileNameOf(path);
    size_t dot = name.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? L"" : name.substr(dot);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    return ext;
}

bool IsVideoPath(const std::wstring& path) {
    static const wchar_t* const kExts[] = {L".mp4", L".m4v", L".mov", L".mkv", L".webm", L".avi",
                                           L".wmv", L".3gp", L".ts",  L".mts", L".m2ts"};
    const std::wstring ext = LowerExt(path);
    return std::any_of(std::begin(kExts), std::end(kExts), [&](const wchar_t* e) { return ext == e; });
}

bool IsAnimPath(const std::wstring& path) {
    const std::wstring ext = LowerExt(path);
    return ext == L".gif" || ext == L".webp" || ext == L".avif";
}

uint64_t FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 0;
    return ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

void OpenFolderAndSelect(const std::wstring& path) {
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
    if (pidl) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
    }
}

namespace {

// Shell.Application of the desktop window: its ShellExecute runs in Explorer.
ComPtr<IShellDispatch2> DesktopShell() {
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows)))) return nullptr;
    VARIANT loc{}, empty{};
    loc.vt = VT_I4;
    loc.lVal = CSIDL_DESKTOP;
    long hwnd = 0;
    ComPtr<IDispatch> disp;
    ComPtr<IServiceProvider> sp;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IDispatch> background, app;
    ComPtr<IShellFolderViewDual> folder;
    ComPtr<IShellDispatch2> shell;
    if (FAILED(windows->FindWindowSW(&loc, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp)) || !disp ||
        FAILED(disp.As(&sp)) || FAILED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
        FAILED(browser->QueryActiveShellView(&view)) ||
        FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background))) || FAILED(background.As(&folder)) ||
        FAILED(folder->get_Application(&app)) || !app || FAILED(app.As(&shell)))
        return nullptr;
    return shell;
}

}  // namespace

bool ShellOpen(const std::wstring& path, const std::wstring& args) {
    if (auto shell = DesktopShell()) {
        BSTR file = SysAllocString(path.c_str());
        VARIANT a{}, dir{}, op{}, show{};
        a.vt = VT_BSTR;
        a.bstrVal = SysAllocString(args.c_str());
        show.vt = VT_I4;
        show.lVal = SW_SHOWNORMAL;
        AllowSetForegroundWindow(ASFW_ANY);  // Explorer starts it, for the user who just clicked here
        HRESULT hr = shell->ShellExecute(file, a, dir, op, show);
        SysFreeString(file);
        VariantClear(&a);
        if (SUCCEEDED(hr)) return true;
    }
    ShellExecuteW(nullptr, L"open", path.c_str(), args.empty() ? nullptr : args.c_str(), nullptr, SW_SHOWNORMAL);
    return false;
}

// Where a drive has no Recycle Bin (some network and USB drives), Windows asks before it deletes for good.
bool MoveToRecycleBin(HWND owner, const std::wstring& path) {
    std::wstring from = path;
    from.push_back(L'\0');  // a list, ended by two nulls
    SHFILEOPSTRUCTW op{};
    op.hwnd = owner;
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_SILENT;
    if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted) return false;
    return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
}

bool CopyFileToClipboard(HWND owner, const std::wstring& path, const std::vector<uint8_t>* dib) {
    // CF_HDROP: pasting into Explorer, Discord, messengers etc. pastes the file itself.
    size_t bytes = sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GHND, bytes);
    if (!mem) return false;
    auto* df = (DROPFILES*)GlobalLock(mem);
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    memcpy((uint8_t*)df + sizeof(DROPFILES), path.c_str(), path.size() * sizeof(wchar_t));
    GlobalUnlock(mem);

    if (!OpenClipboard(owner)) {
        GlobalFree(mem);
        return false;
    }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_HDROP, mem) != nullptr;
    // Preferred drop effect = copy, so Explorer pastes a copy instead of moving.
    UINT fmt = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    if (HGLOBAL eff = GlobalAlloc(GHND, sizeof(DWORD))) {
        *(DWORD*)GlobalLock(eff) = DROPEFFECT_COPY;
        GlobalUnlock(eff);
        if (!SetClipboardData(fmt, eff)) GlobalFree(eff);
    }
    if (HGLOBAL pic = dib ? GlobalAlloc(GHND, dib->size()) : nullptr) {
        memcpy(GlobalLock(pic), dib->data(), dib->size());
        GlobalUnlock(pic);
        if (!SetClipboardData(CF_DIB, pic)) GlobalFree(pic);
    }
    CloseClipboard();
    if (!ok) GlobalFree(mem);
    return ok;
}

bool PickFolder(HWND owner, const std::wstring& initial, std::wstring& out) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    ComPtr<IShellItem> init;
    if (!initial.empty() && SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&init))))
        dlg->SetFolder(init.Get());
    if (FAILED(dlg->Show(owner))) return false;
    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return false;
    PWSTR p = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) return false;
    out = p;
    CoTaskMemFree(p);
    return true;
}

bool PickVideoFile(HWND owner, const std::wstring& dir, std::wstring& out) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    COMDLG_FILTERSPEC filters[] = {
        {tr(L"동영상 · 움짤"), L"*.mp4;*.m4v;*.mov;*.mkv;*.webm;*.avi;*.wmv;*.3gp;*.ts;*.mts;*.m2ts;*.gif;*.webp;*.avif"},
        {tr(L"동영상"), L"*.mp4;*.m4v;*.mov;*.mkv;*.webm;*.avi;*.wmv;*.3gp;*.ts;*.mts;*.m2ts"},
        {tr(L"움짤 (GIF · WebP · AVIF)"), L"*.gif;*.webp;*.avif"},
        {tr(L"모든 파일"), L"*.*"}};
    dlg->SetFileTypes(_countof(filters), filters);
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    ComPtr<IShellItem> folder;
    if (!dir.empty() && SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&folder))))
        dlg->SetDefaultFolder(folder.Get());
    if (FAILED(dlg->Show(owner))) return false;
    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return false;
    PWSTR p = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) return false;
    out = p;
    CoTaskMemFree(p);
    return true;
}

bool PickExeFile(HWND owner, std::wstring& out) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    COMDLG_FILTERSPEC filters[] = {{tr(L"프로그램"), L"*.exe"}};
    dlg->SetFileTypes(_countof(filters), filters);
    dlg->SetTitle(tr(L"리플레이를 켤 프로그램"));
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);  // a shortcut gives the program it starts
    ComPtr<IShellItem> folder;
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&folder))))
        dlg->SetDefaultFolder(folder.Get());
    if (FAILED(dlg->Show(owner))) return false;
    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return false;
    PWSTR p = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) return false;
    out = p;
    CoTaskMemFree(p);
    return out.size() > 4 && _wcsicmp(out.c_str() + out.size() - 4, L".exe") == 0;
}

std::wstring SafeFileName(std::wstring_view s, size_t maxLen) {
    std::wstring out;
    for (wchar_t c : s) {
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
        if (c == L' ' && (out.empty() || out.back() == L' ')) continue;
        out += c;
    }
    if (out.size() > maxLen) out.resize(maxLen);
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) out.pop_back();
    return out;
}

namespace {
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
std::wstring AutostartCommand() {
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, exe, _countof(exe));
    return L"\"" + std::wstring(exe, n) + L"\" --tray";
}
}  // namespace

bool AutostartEnabled() {
    DWORD size = 0;
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"OzMaker", RRF_RT_REG_SZ, nullptr, nullptr, &size) == ERROR_SUCCESS;
}

bool SetAutostart(bool on) {
    if (!on) {
        LSTATUS s = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, L"OzMaker");
        return s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND;
    }
    std::wstring cmd = AutostartCommand();
    return RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, L"OzMaker", REG_SZ, cmd.c_str(),
                           (DWORD)((cmd.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool ProcessGone(DWORD pid) {
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!p) return GetLastError() == ERROR_INVALID_PARAMETER;
    bool gone = WaitForSingleObject(p, 0) == WAIT_OBJECT_0;
    CloseHandle(p);
    return gone;
}

std::vector<RECT> SubtractRects(const RECT& whole, const std::vector<RECT>& holes) {
    std::vector<RECT> parts{whole}, next;
    if (IsRectEmpty(&whole)) parts.clear();
    for (const RECT& h : holes) {
        next.clear();
        for (const RECT& p : parts) {
            RECT o;
            if (!IntersectRect(&o, &p, &h)) {
                next.push_back(p);
                continue;
            }
            // The bands above and below the hole, then what is beside it between them.
            if (p.top < o.top) next.push_back({p.left, p.top, p.right, o.top});
            if (o.bottom < p.bottom) next.push_back({p.left, o.bottom, p.right, p.bottom});
            if (p.left < o.left) next.push_back({p.left, o.top, o.left, o.bottom});
            if (o.right < p.right) next.push_back({o.right, o.top, p.right, o.bottom});
        }
        parts.swap(next);
    }
    return parts;
}

std::wstring FormatAgo(double s) {
    if (s < 60) return tr(L"방금");
    if (s < 3600) return Format(tr(L"%d분 전"), (int)(s / 60));
    if (s < 86400) return Format(tr(L"%d시간 전"), (int)(s / 3600));
    if (s < 2 * 86400) return tr(L"어제");
    return Format(tr(L"%d일 전"), (int)(s / 86400));
}

bool PickSavePath(HWND owner, const std::wstring& dir, const std::wstring& name, const wchar_t* ext,
                  const wchar_t* filterName, std::wstring& out) {
    ComPtr<IFileSaveDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    std::wstring spec = std::wstring(L"*.") + ext;
    COMDLG_FILTERSPEC filters[] = {{filterName, spec.c_str()}};
    dlg->SetFileTypes(1, filters);
    dlg->SetDefaultExtension(ext);
    dlg->SetFileName(name.c_str());
    ComPtr<IShellItem> folder;
    if (!dir.empty() && SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&folder))))
        dlg->SetFolder(folder.Get());
    if (FAILED(dlg->Show(owner))) return false;
    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return false;
    PWSTR p = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) return false;
    out = p;
    CoTaskMemFree(p);
    return true;
}

static std::wstring VkName(UINT vk) {
    if (vk >= 'A' && vk <= 'Z') return std::wstring(1, (wchar_t)vk);
    if (vk >= '0' && vk <= '9') return std::wstring(1, (wchar_t)vk);
    if (vk >= VK_F1 && vk <= VK_F24) return Format(L"F%u", vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return Format(L"Num %u", vk - VK_NUMPAD0);
    switch (vk) {
        case VK_SPACE: return L"Space";
        case VK_PAUSE: return L"Pause";
        case VK_SNAPSHOT: return L"PrtSc";
        case VK_SCROLL: return L"ScrLk";
        case VK_INSERT: return L"Insert";
        case VK_DELETE: return L"Delete";
        case VK_HOME: return L"Home";
        case VK_END: return L"End";
        case VK_PRIOR: return L"PgUp";
        case VK_NEXT: return L"PgDn";
        case VK_OEM_3: return L"`";
    }
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    wchar_t buf[64] = {};
    if (sc && GetKeyNameTextW((LONG)(sc << 16), buf, 64) > 0) return buf;
    return Format(L"0x%02X", vk);
}

bool AppsUseLightTheme() {
    DWORD v = 1, size = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v != 0;
}

std::wstring HotkeyToString(UINT mods, UINT vk) {
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl + ";
    if (mods & MOD_ALT) s += L"Alt + ";
    if (mods & MOD_SHIFT) s += L"Shift + ";
    if (mods & MOD_WIN) s += L"Win + ";
    s += VkName(vk);
    return s;
}

}  // namespace util
