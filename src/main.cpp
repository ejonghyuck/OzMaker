#include "pch.h"
#include "App.h"

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    // Everything after the program name, re-joined by App::Run's own parser.
    std::wstring cmd;
    std::wstring first;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            std::wstring a = argv[i];
            if (i == 1) first = a;
            if (!cmd.empty()) cmd += L' ';
            cmd += a.find(L' ') != std::wstring::npos ? L"\"" + a + L"\"" : a;
        }
        LocalFree(argv);
    }
    if (first == L"--exit") {  // ask a running instance to quit (scripts, installers)
        if (HWND h = FindWindowW(L"OzMakerMain", nullptr)) PostMessageW(h, WM_APP + 6, 0, 0);
        return 0;
    }
    bool testMode = first.rfind(L"--", 0) == 0;
    HANDLE mutex = nullptr;
    if (!testMode) {
        mutex = CreateMutexW(nullptr, TRUE, L"Local\\OzMaker.SingleInstance");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (HWND h = FindWindowW(L"OzMakerMain", nullptr)) PostMessageW(h, WM_APP + 5, 0, 0);
            return 0;
        }
    }
    int rc;
    {
        auto app = std::make_unique<App>();
        rc = app->Run(inst, show, cmd);
    }
    if (mutex) CloseHandle(mutex);
    return rc;
}
