#include "pch.h"
#include "Crash.h"
#include "Util.h"
#include "Version.h"
#include <dbghelp.h>
#include <csignal>

namespace crash {
namespace {

// Codes for the failures that don't come with an exception of their own.
constexpr DWORD kAbort = 0xE0000001, kInvalidParameter = 0xE0000002, kPureCall = 0xE0000003;

std::wstring g_dir;
std::atomic<const wchar_t*> g_state{L"시작"};
std::atomic<bool> g_writing{false};

struct Job {
    EXCEPTION_POINTERS* ep;
    DWORD thread;
};

const wchar_t* CodeName(DWORD c) {
    switch (c) {
        case EXCEPTION_ACCESS_VIOLATION: return L"access violation";
        case EXCEPTION_STACK_OVERFLOW: return L"stack overflow";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return L"illegal instruction";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return L"integer divide by zero";
        case EXCEPTION_IN_PAGE_ERROR: return L"in-page error";
        case 0xE06D7363: return L"unhandled C++ exception";
        case kAbort: return L"abort (std::terminate)";
        case kInvalidParameter: return L"CRT invalid parameter";
        case kPureCall: return L"pure virtual call";
        default: return L"exception";
    }
}

void Append(HANDLE f, const wchar_t* text) {
    char buf[2048];
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, buf, sizeof(buf), nullptr, nullptr);
    DWORD w = 0;
    if (n > 1) WriteFile(f, buf, (DWORD)(n - 1), &w, nullptr);
}

// On a thread of its own: the crashing one may have no stack left. Fixed buffers only, the heap may be broken.
DWORD WINAPI Write(void* p) {
    const Job& job = *static_cast<Job*>(p);
    CreateDirectoryW(g_dir.c_str(), nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t base[MAX_PATH * 2];
    swprintf_s(base, L"%s\\crash_%04d%02d%02d_%02d%02d%02d_%s", g_dir.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour,
               st.wMinute, st.wSecond, OZ_VERSION_WSTR);
    wchar_t path[MAX_PATH * 2];

    // Threads, stacks and the loaded modules: no heap, so nothing of what was on screen.
    swprintf_s(path, L"%s.dmp", base);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{job.thread, job.ep, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                          (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
                          job.ep ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(f);
    }

    swprintf_s(path, L"%s.txt", base);
    f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    const EXCEPTION_RECORD* rec = job.ep ? job.ep->ExceptionRecord : nullptr;
    DWORD code = rec ? rec->ExceptionCode : 0;
    void* addr = rec ? rec->ExceptionAddress : nullptr;
    wchar_t module[MAX_PATH] = L"?";
    uintptr_t offset = (uintptr_t)addr;
    HMODULE mod = nullptr;
    if (addr && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCWSTR)addr, &mod)) {
        wchar_t full[MAX_PATH];
        if (DWORD n = GetModuleFileNameW(mod, full, MAX_PATH)) {
            const wchar_t* name = full + n;
            while (name > full && name[-1] != L'\\') --name;
            wcscpy_s(module, name);
        }
        offset = (uintptr_t)addr - (uintptr_t)mod;
    }
    RTL_OSVERSIONINFOW os{sizeof(os)};
    if (auto get = (LONG(WINAPI*)(RTL_OSVERSIONINFOW*))GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))
        get(&os);
    const wchar_t* state = g_state.load();
    wchar_t text[2048];
    swprintf_s(text,
               L"OzMaker %s crash report\r\n"
               L"Time: %04d-%02d-%02d %02d:%02d:%02d\r\n"
               L"Exception: 0x%08lX (%s) at %s+0x%llX, thread %lu\r\n"
               L"Doing: %s\r\n"
               L"Windows: %lu.%lu.%lu\r\n"
               L"\r\n"
               L"The .dmp file next to this one holds the threads and stacks only, no screen contents.\r\n"
               L"Nothing was sent. To report it, attach both files to an issue:\r\n"
               L"https://github.com/ejonghyuck/OzMaker/issues\r\n",
               OZ_VERSION_WSTR, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, (unsigned long)code,
               CodeName(code), module, (unsigned long long)offset, (unsigned long)job.thread, state ? state : L"?",
               os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber);
    DWORD w = 0;
    WriteFile(f, "\xEF\xBB\xBF", 3, &w, nullptr);
    Append(f, text);
    CloseHandle(f);
    return 0;
}

void ReportNow(EXCEPTION_POINTERS* ep) {
    if (g_writing.exchange(true)) {
        Sleep(INFINITE);  // another thread is already writing the report; it ends the process
    }
    Job job{ep, GetCurrentThreadId()};
    if (HANDLE t = CreateThread(nullptr, 256 * 1024, Write, &job, 0, nullptr)) {
        WaitForSingleObject(t, 30000);
        CloseHandle(t);
    } else {
        Write(&job);
    }
}

[[noreturn]] void ReportAndExit(DWORD code) {
    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = code;
    rec.ExceptionAddress = (void*)ctx.Rip;
    EXCEPTION_POINTERS ep{&rec, &ctx};
    ReportNow(&ep);
    TerminateProcess(GetCurrentProcess(), code);
    for (;;) Sleep(INFINITE);
}

LONG WINAPI OnException(EXCEPTION_POINTERS* ep) {
    ReportNow(ep);
    return EXCEPTION_EXECUTE_HANDLER;  // ends the process, without the Windows error dialog
}

void OnAbort(int) { ReportAndExit(kAbort); }
void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    ReportAndExit(kInvalidParameter);
}
void OnPureCall() { ReportAndExit(kPureCall); }

}  // namespace

void Install(const std::wstring& dir) {
    g_dir = dir;
    SetUnhandledExceptionFilter(OnException);
    // abort() would otherwise fail fast to Windows Error Reporting, past the filter.
    signal(SIGABRT, OnAbort);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_invalid_parameter_handler(OnInvalidParameter);
    _set_purecall_handler(OnPureCall);
    ULONG guarantee = 64 * 1024;  // room for the filter if the main thread's stack overflows
    SetThreadStackGuarantee(&guarantee);
}

const std::wstring& Dir() { return g_dir; }

void SetState(const wchar_t* what) { g_state = what; }

std::vector<Report> List() {
    std::vector<Report> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_dir + L"\\crash_*.dmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring name = fd.cFileName;
        name.resize(name.size() - 4);  // .dmp
        Report r;
        r.name = name;
        r.dump = g_dir + L"\\" + name + L".dmp";
        std::wstring log = g_dir + L"\\" + name + L".txt";
        if (GetFileAttributesW(log.c_str()) != INVALID_FILE_ATTRIBUTES) r.log = log;
        out.push_back(std::move(r));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const Report& a, const Report& b) { return a.name > b.name; });
    return out;
}

void Prune(size_t keep) {
    auto all = List();
    for (size_t i = keep; i < all.size(); ++i) {
        DeleteFileW(all[i].dump.c_str());
        if (!all[i].log.empty()) DeleteFileW(all[i].log.c_str());
    }
}

}  // namespace crash
