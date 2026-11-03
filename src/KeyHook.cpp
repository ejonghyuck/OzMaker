#include "pch.h"
#include "KeyHook.h"

namespace {

KeyHook* g_hook = nullptr;  // one per process: the hook procedure has no context of its own
constexpr UINT kMaskMsg = WM_APP + 1;  // to the hook's thread: press the masking key

bool IsModifier(UINT vk) {
    switch (vk) {
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_MENU: case VK_LMENU: case VK_RMENU:
        case VK_LWIN: case VK_RWIN:
            return true;
    }
    return false;
}

UINT Mods() {
    UINT m = 0;
    if (GetAsyncKeyState(VK_CONTROL) < 0) m |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_MENU) < 0) m |= MOD_ALT;
    if (GetAsyncKeyState(VK_SHIFT) < 0) m |= MOD_SHIFT;
    if (GetAsyncKeyState(VK_LWIN) < 0 || GetAsyncKeyState(VK_RWIN) < 0) m |= MOD_WIN;
    return m;
}

// Switching windows and the task manager stay Windows' while a hotkey is being chosen.
bool SystemCombo(UINT mods, UINT vk) {
    return ((mods & ~MOD_SHIFT) == MOD_ALT && (vk == VK_TAB || vk == VK_ESCAPE)) ||
           ((mods & ~MOD_SHIFT) == MOD_CONTROL && vk == VK_ESCAPE);
}

// Windows opens the Start menu when Win goes up with no key pressed in between, and some windows' menus when Alt
// does: the key it took doesn't count, so a key that means nothing (vkE8) goes in between.
void PressMaskKey() {
    INPUT in[2] = {};
    for (INPUT& i : in) {
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = 0xE8;
        i.ki.dwExtraInfo = KeyHook::kTag;
    }
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

}  // namespace

bool KeyHook::Set(HWND hwnd, UINT captureMsg, std::vector<Combo> combos, bool capture) {
    {
        std::lock_guard lock(mtx_);
        hwnd_ = hwnd;
        captureMsg_ = captureMsg;
        combos_ = std::move(combos);
        capture_ = capture;
    }
    if (thread_.joinable()) return installed_;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready) return false;
    g_hook = this;
    thread_ = std::thread([this, ready] { Run(ready); });
    WaitForSingleObject(ready, INFINITE);
    CloseHandle(ready);
    if (!installed_) Stop();
    return installed_;
}

void KeyHook::Stop() {
    if (thread_.joinable()) {
        PostThreadMessageW(GetThreadId(thread_.native_handle()), WM_QUIT, 0, 0);
        thread_.join();
    }
    installed_ = false;
    std::lock_guard lock(mtx_);
    combos_.clear();
    capture_ = false;
    held_.clear();
}

void KeyHook::Run(HANDLE ready) {
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // the queue Stop posts WM_QUIT to
    // Every key on the PC waits for this procedure: it runs on a thread that does nothing else, ahead of others.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, Proc, GetModuleHandleW(nullptr), 0);
    installed_ = hook != nullptr;
    SetEvent(ready);
    if (!hook) return;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == kMaskMsg)
            PressMaskKey();
        else
            DispatchMessageW(&msg);
    }
    UnhookWindowsHookEx(hook);
}

LRESULT CALLBACK KeyHook::Proc(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && g_hook) {
        const auto& k = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(l);
        bool mask = false;
        if (k.dwExtraInfo != kTag && g_hook->OnKey(k.vkCode, w == WM_KEYDOWN || w == WM_SYSKEYDOWN, mask)) {
            if (mask) PostThreadMessageW(GetCurrentThreadId(), kMaskMsg, 0, 0);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, w, l);
}

bool KeyHook::OnKey(UINT vk, bool down, bool& mask) {
    std::lock_guard lock(mtx_);
    auto held = std::find(held_.begin(), held_.end(), vk);
    if (!down) {
        if (held == held_.end()) return false;
        held_.erase(held);
        return true;
    }
    if (held != held_.end()) return true;  // a repeat: the press was posted once
    if (IsModifier(vk)) return false;
    const UINT mods = Mods();
    auto c = std::find_if(combos_.begin(), combos_.end(), [&](const Combo& c) { return c.vk == vk && c.mods == mods; });
    if (c != combos_.end())
        PostMessageW(hwnd_, WM_HOTKEY, c->id, MAKELPARAM(mods, vk));
    else if (capture_ && GetForegroundWindow() == hwnd_ && Usable(mods, vk) && !SystemCombo(mods, vk))
        PostMessageW(hwnd_, captureMsg_, vk, mods);
    else
        return false;
    held_.push_back(vk);
    mask = (mods & (MOD_ALT | MOD_WIN)) != 0;
    return true;
}
