#pragma once

// A low-level keyboard hook on a thread of its own (#50). It sees a key before a program that registered it with
// RegisterHotKey does, so it can take a combination that program holds, and keep it from going any further.
// OzMaker runs it only for a combination the user took over by hand, while RegisterHotKey refuses it, and while a
// new hotkey is being chosen (so that a held combination still reaches the settings). It never records or keeps
// keys: a combination it takes is posted to the window, everything else goes on untouched.
class KeyHook {
public:
    struct Combo {
        UINT mods = 0, vk = 0;  // MOD_* and the virtual key, as for RegisterHotKey
        int id = 0;             // posted as WM_HOTKEY with this wParam
    };
    ~KeyHook() { Stop(); }
    // Takes `combos`, and with `capture` any combination that can be a hotkey pressed while `hwnd` is in front
    // (posted as captureMsg: wParam the key, lParam MOD_*). Starts the hook when it isn't running. False when
    // Windows refused it.
    bool Set(HWND hwnd, UINT captureMsg, std::vector<Combo> combos, bool capture);
    void Stop();
    bool Running() const { return thread_.joinable(); }
    // A combination RegisterHotKey would make a sensible hotkey of: with Ctrl, Alt or Win, or a key that does
    // nothing much on its own (F1–F24, Pause, Scroll Lock).
    static bool Usable(UINT mods, UINT vk) {
        return (vk >= VK_F1 && vk <= VK_F24) || vk == VK_PAUSE || vk == VK_SCROLL || (mods & ~MOD_SHIFT);
    }
    static constexpr ULONG_PTR kTag = 0x4F5A4B48;  // 'OZKH': the hook's own key presses, which it lets by

private:
    static LRESULT CALLBACK Proc(int code, WPARAM w, LPARAM l);
    bool OnKey(UINT vk, bool down, bool& mask);
    void Run(HANDLE ready);

    std::thread thread_;
    bool installed_ = false;  // written before `ready` is set, read after
    std::mutex mtx_;          // what the hook reads, set from the window's thread
    HWND hwnd_ = nullptr;
    UINT captureMsg_ = 0;
    std::vector<Combo> combos_;
    bool capture_ = false;
    std::vector<UINT> held_;  // keys whose press it took: their repeats and release go with it
};
