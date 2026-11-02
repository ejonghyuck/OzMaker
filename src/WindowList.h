#pragma once

struct WinInfo {
    HWND hwnd = nullptr;
    std::wstring title;
    std::wstring exePath;
    std::wstring exeName;
    DWORD pid = 0;
    RECT frame{};        // visible bounds (DWM extended frame bounds), physical pixels
    bool minimized = false;
    bool fullscreen = false;
};

struct MonitorDesc {
    HMONITOR handle = nullptr;
    std::wstring device;  // GDI device name (MONITORINFOEX::szDevice)
    std::wstring name;    // friendly name from the EDID when available
    RECT rect{};          // physical pixels
    int number = 0;       // 1-based, left to right
    bool primary = false;
    UINT dpi = 96;
    int hz = 0;
};

std::vector<MonitorDesc> EnumerateMonitors();
const MonitorDesc* FindMonitor(const std::vector<MonitorDesc>& list, const std::wstring& device);

// Top-level windows that make sense to record, in z-order (topmost first).
std::vector<WinInfo> EnumerateCaptureWindows();
bool QueryWindowInfo(HWND hwnd, WinInfo& out);

bool GetVisibleFrame(HWND hwnd, RECT& r);
bool IsFullscreenWindow(HWND hwnd);
std::wstring GetProcessPath(DWORD pid);

// Best icon for a window as a WIC bitmap (caller converts to D2D). May return null.
ComPtr<IWICBitmapSource> GetWindowIconBitmap(IWICImagingFactory* wic, const WinInfo& w, int size);
