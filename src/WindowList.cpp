#include "pch.h"
#include "WindowList.h"

bool GetVisibleFrame(HWND hwnd, RECT& r) {
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) return true;
    return GetWindowRect(hwnd, &r) != FALSE;
}

bool IsFullscreenWindow(HWND hwnd) {
    RECT wr;
    if (!GetWindowRect(hwnd, &wr)) return false;
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    return wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top && wr.right >= mi.rcMonitor.right &&
           wr.bottom >= mi.rcMonitor.bottom && !(GetWindowLongW(hwnd, GWL_STYLE) & WS_CAPTION);
}

std::wstring GetProcessPath(DWORD pid) {
    std::wstring out;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return out;
    wchar_t buf[MAX_PATH * 2];
    DWORD n = _countof(buf);
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) out.assign(buf, n);
    CloseHandle(h);
    return out;
}

static bool IsCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked;
}

static bool IsCandidate(HWND hwnd) {
    if (!IsWindowVisible(hwnd) || IsCloaked(hwnd)) return false;
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return false;
    if (GetWindow(hwnd, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) return false;
    if (GetWindowTextLengthW(hwnd) == 0) return false;
    wchar_t cls[128] = {};
    GetClassNameW(hwnd, cls, _countof(cls));
    static const wchar_t* skip[] = {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW",
                                    L"Windows.UI.Core.CoreWindow", L"OzMakerOverlay", L"OzMakerPicker"};
    for (auto* s : skip)
        if (!wcscmp(cls, s)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return false;
    RECT r;
    if (!IsIconic(hwnd) && GetVisibleFrame(hwnd, r) && (r.right - r.left < 40 || r.bottom - r.top < 40)) return false;
    return true;
}

bool QueryWindowInfo(HWND hwnd, WinInfo& w) {
    if (!IsWindow(hwnd)) return false;
    w.hwnd = hwnd;
    int len = GetWindowTextLengthW(hwnd);
    w.title.assign(len + 1, L'\0');
    GetWindowTextW(hwnd, w.title.data(), len + 1);
    w.title.resize(wcslen(w.title.c_str()));
    GetWindowThreadProcessId(hwnd, &w.pid);
    w.exePath = GetProcessPath(w.pid);
    size_t slash = w.exePath.find_last_of(L'\\');
    w.exeName = slash == std::wstring::npos ? w.exePath : w.exePath.substr(slash + 1);
    w.minimized = IsIconic(hwnd) != FALSE;
    GetVisibleFrame(hwnd, w.frame);
    w.fullscreen = !w.minimized && IsFullscreenWindow(hwnd);
    return true;
}

std::vector<WinInfo> EnumerateCaptureWindows() {
    std::vector<HWND> hwnds;
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            if (IsCandidate(h)) reinterpret_cast<std::vector<HWND>*>(lp)->push_back(h);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&hwnds));
    std::vector<WinInfo> out;
    out.reserve(hwnds.size());
    for (HWND h : hwnds) {
        WinInfo w;
        if (QueryWindowInfo(h, w)) out.push_back(std::move(w));
    }
    return out;
}

ComPtr<IWICBitmapSource> GetWindowIconBitmap(IWICImagingFactory* wic, const WinInfo& w, int size) {
    HICON icon = nullptr;
    bool owned = false;
    // Prefer a crisp icon from the executable at the requested size; fall back to the window's own icon.
    if (!w.exePath.empty() && !w.exeName.empty() && _wcsicmp(w.exeName.c_str(), L"ApplicationFrameHost.exe") != 0) {
        HICON large = nullptr;
        if (SUCCEEDED(SHDefExtractIconW(w.exePath.c_str(), 0, 0, &large, nullptr, MAKELONG(size, 16))) && large) {
            icon = large;
            owned = true;
        }
    }
    if (!icon) {
        DWORD_PTR res = 0;
        if (SendMessageTimeoutW(w.hwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 60, &res) && res)
            icon = (HICON)res;
        else if (HICON c = (HICON)GetClassLongPtrW(w.hwnd, GCLP_HICON))
            icon = c;
        else if (SendMessageTimeoutW(w.hwnd, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 60, &res) &&
                 res)
            icon = (HICON)res;
    }
    if (!icon) return nullptr;
    ComPtr<IWICBitmap> bmp;
    HRESULT hr = wic->CreateBitmapFromHICON(icon, &bmp);
    if (owned) DestroyIcon(icon);
    if (FAILED(hr)) return nullptr;
    return bmp;
}

// ---------------------------------------------------------------- monitors

namespace {
// "DELL U2720Q" etc. via the display configuration API; empty when the driver doesn't say.
std::wstring FriendlyMonitorName(const std::wstring& device) {
    UINT32 nPaths = 0, nModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPaths, &nModes) != ERROR_SUCCESS) return {};
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nModes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPaths, paths.data(), &nModes, modes.data(), nullptr) !=
        ERROR_SUCCESS)
        return {};
    for (UINT32 i = 0; i < nPaths; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS || device != src.viewGdiDeviceName) continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
        tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tgt.header.size = sizeof(tgt);
        tgt.header.adapterId = paths[i].targetInfo.adapterId;
        tgt.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tgt.header) == ERROR_SUCCESS) return tgt.monitorFriendlyDeviceName;
    }
    return {};
}
}  // namespace

std::vector<MonitorDesc> EnumerateMonitors() {
    std::vector<MonitorDesc> out;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR h, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* list = reinterpret_cast<std::vector<MonitorDesc>*>(lp);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (!GetMonitorInfoW(h, &mi)) return TRUE;
            MonitorDesc d;
            d.handle = h;
            d.device = mi.szDevice;
            d.rect = mi.rcMonitor;
            d.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
            UINT dx = 96, dy = 96;
            GetDpiForMonitor(h, MDT_EFFECTIVE_DPI, &dx, &dy);
            d.dpi = dx;
            DEVMODEW dm{};
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) d.hz = (int)dm.dmDisplayFrequency;
            list->push_back(std::move(d));
            return TRUE;
        },
        (LPARAM)&out);
    // Number displays left to right like the Settings app roughly does.
    std::stable_sort(out.begin(), out.end(), [](const MonitorDesc& a, const MonitorDesc& b) {
        return a.rect.left != b.rect.left ? a.rect.left < b.rect.left : a.rect.top < b.rect.top;
    });
    for (size_t i = 0; i < out.size(); ++i) {
        out[i].number = (int)i + 1;
        out[i].name = FriendlyMonitorName(out[i].device);
    }
    return out;
}

const MonitorDesc* FindMonitor(const std::vector<MonitorDesc>& list, const std::wstring& device) {
    for (auto& m : list)
        if (m.device == device) return &m;
    for (auto& m : list)
        if (m.primary) return &m;
    return list.empty() ? nullptr : &list[0];
}
