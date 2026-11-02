#include "pch.h"
#include "App.h"

#include <tlhelp32.h>

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    // Everything after the program name, re-joined by App::Run's own parser.
    std::wstring cmd;
    std::wstring first, second;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            std::wstring a = argv[i];
            if (i == 1) first = a;
            if (i == 2) second = a;
            if (!cmd.empty()) cmd += L' ';
            cmd += a.find(L' ') != std::wstring::npos ? L"\"" + a + L"\"" : a;
        }
        LocalFree(argv);
    }
    if (first == L"--report-parent") {  // the self-test: what started this process (util::ShellOpen)
        DWORD parent = 0, me = GetCurrentProcessId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe{sizeof(pe)};
        for (BOOL ok = snap != INVALID_HANDLE_VALUE && Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
            if (pe.th32ProcessID == me) parent = pe.th32ParentProcessID;
        if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
        std::string text = std::to_string(parent);
        HANDLE f = CreateFileW(second.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD done = 0;
        if (f != INVALID_HANDLE_VALUE) {
            WriteFile(f, text.data(), (DWORD)text.size(), &done, nullptr);
            CloseHandle(f);
        }
        return 0;
    }
    if (first == L"--exit") {  // ask a running instance to quit (scripts, installers)
        if (HWND h = FindWindowW(L"OzMakerMain", nullptr)) PostMessageW(h, WM_APP + 6, 0, 0);
        return 0;
    }
    const bool tray = first == L"--tray";  // started with Windows: stay in the tray
    // Started by the previous version once it had put this exe in its place (App::InstallUpdate). That one still
    // holds the single-instance mutex and the hotkey until it has gone, so wait for it.
    const bool updated = first == L"--updated";
    if (updated) {
        if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)_wtoi(second.c_str()))) {
            WaitForSingleObject(old, 20000);
            CloseHandle(old);
        }
    }
    bool testMode = first.rfind(L"--", 0) == 0 && !tray && !updated;
    HANDLE mutex = nullptr;
    if (!testMode) {
        mutex = CreateMutexW(nullptr, TRUE, L"Local\\OzMaker.SingleInstance");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (HWND h = FindWindowW(L"OzMakerMain", nullptr)) {
                // We were just started by the user, so we may hand the foreground over to the running instance.
                DWORD pid = 0;
                GetWindowThreadProcessId(h, &pid);
                AllowSetForegroundWindow(pid);
                if (!first.empty() && !tray && !updated) {
                    // A file to open ("Open with", dropped on the exe): the running instance opens it.
                    wchar_t full[MAX_PATH * 4];
                    DWORD n = GetFullPathNameW(first.c_str(), _countof(full), full, nullptr);
                    std::wstring path = n > 0 && n < _countof(full) ? std::wstring(full, n) : first;
                    COPYDATASTRUCT cd{0x4F5A4F50, (DWORD)((path.size() + 1) * sizeof(wchar_t)), (void*)path.c_str()};
                    DWORD_PTR res = 0;
                    SendMessageTimeoutW(h, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 5000, &res);
                } else if (!tray) {
                    PostMessageW(h, WM_APP + 5, 0, 0);
                }
            }
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
