#pragma once
#include "Ui.h"

// UI Automation for the immediate-mode UI, so screen readers (Narrator, NVDA) can read and use it. The window is one
// fragment root; every node the last frame drew (Ui::Nodes) is a fragment under it, a Segmented row's cells under the
// row. What a reader does (press, toggle, pick, set a slider, move the focus) goes to Ui as input for the next frame.
// UI Automation calls in on the UI thread (an STA), between frames.
class Access {
public:
    struct Host {
        HWND hwnd = nullptr;
        Ui* ui = nullptr;
        std::function<float()> scale;      // DIPs to pixels
        std::function<void()> invalidate;  // draw a frame
        bool alive = true;
    };

    void Attach(HWND hwnd, Ui* ui, std::function<float()> scale, std::function<void()> invalidate);
    void Detach();
    LRESULT OnGetObject(WPARAM w, LPARAM l);  // WM_GETOBJECT for UiaRootObjectId
    // After each frame: tells the readers where the keyboard focus went, what changed in it, and when the screen
    // changed.
    void AfterFrame();

    // Tests: the root, as a reader gets it.
    IRawElementProviderSimple* Root() const { return root_.Get(); }

private:
    std::shared_ptr<Host> host_;
    ComPtr<IRawElementProviderSimple> root_;
    UiId lastFocus_ = 0;
    bool lastOn_ = false;
    float lastValue_ = 0;
    std::wstring lastText_;
    uint64_t lastTree_ = 0;
};
