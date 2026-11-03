#include "pch.h"
#include "Access.h"

namespace {

using Host = Access::Host;
constexpr int kOptions = ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading;

void SetBstr(VARIANT* v, std::wstring_view s) {
    v->vt = VT_BSTR;
    v->bstrVal = SysAllocStringLen(s.data(), (UINT)s.size());
}
void SetBool(VARIANT* v, bool b) {
    v->vt = VT_BOOL;
    v->boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
}
void SetInt(VARIANT* v, int i) {
    v->vt = VT_I4;
    v->lVal = i;
}

// The row a node is drawn in (a Segmented row, the tabs), or 0 for the window.
UiId ParentOf(const Ui& ui, const UiNode& n) { return n.group && ui.Shown(n.group) ? n.group : 0; }

std::vector<UiId> ChildrenOf(const Ui& ui, UiId parent) {
    std::vector<UiId> out;
    for (const UiNode& n : ui.Nodes())
        if (n.id != parent && ParentOf(ui, n) == parent) out.push_back(n.id);
    return out;
}

bool Usable(const UiNode& n) { return n.enabled && !n.blocked; }

IRawElementProviderSimple* MakeNode(const std::shared_ptr<Host>& host, IRawElementProviderFragmentRoot* root, UiId id);

class RootProvider final : public IRawElementProviderSimple,
                   public IRawElementProviderFragment,
                   public IRawElementProviderFragmentRoot {
public:
    explicit RootProvider(std::shared_ptr<Host> host) : host_(std::move(host)) {}

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple))
            *out = static_cast<IRawElementProviderSimple*>(this);
        else if (riid == __uuidof(IRawElementProviderFragment))
            *out = static_cast<IRawElementProviderFragment*>(this);
        else if (riid == __uuidof(IRawElementProviderFragmentRoot))
            *out = static_cast<IRawElementProviderFragmentRoot*>(this);
        else
            return *out = nullptr, E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    IFACEMETHODIMP_(ULONG) Release() override {
        ULONG n = --refs_;
        if (!n) delete this;
        return n;
    }

    // IRawElementProviderSimple: the window itself comes from its host provider.
    IFACEMETHODIMP get_ProviderOptions(ProviderOptions* ret) override {
        *ret = (ProviderOptions)kOptions;
        return S_OK;
    }
    IFACEMETHODIMP GetPatternProvider(PATTERNID, IUnknown** ret) override {
        *ret = nullptr;
        return S_OK;
    }
    IFACEMETHODIMP GetPropertyValue(PROPERTYID id, VARIANT* ret) override {
        ret->vt = VT_EMPTY;
        if (id == UIA_ProviderDescriptionPropertyId) SetBstr(ret, L"OzMaker");
        return S_OK;
    }
    IFACEMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple** ret) override {
        *ret = nullptr;
        return host_->alive ? UiaHostProviderFromHwnd(host_->hwnd, ret) : UIA_E_ELEMENTNOTAVAILABLE;
    }

    // IRawElementProviderFragment
    IFACEMETHODIMP Navigate(NavigateDirection dir, IRawElementProviderFragment** ret) override {
        *ret = nullptr;
        if (!host_->alive) return UIA_E_ELEMENTNOTAVAILABLE;
        if (dir != NavigateDirection_FirstChild && dir != NavigateDirection_LastChild) return S_OK;
        auto kids = ChildrenOf(*host_->ui, 0);
        if (kids.empty()) return S_OK;
        ComPtr<IRawElementProviderSimple> p;
        p.Attach(MakeNode(host_, this, dir == NavigateDirection_FirstChild ? kids.front() : kids.back()));
        return p.CopyTo(ret);
    }
    IFACEMETHODIMP GetRuntimeId(SAFEARRAY** ret) override {
        *ret = nullptr;
        return S_OK;
    }
    IFACEMETHODIMP get_BoundingRectangle(UiaRect* ret) override {
        *ret = {};
        return S_OK;
    }
    IFACEMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY** ret) override {
        *ret = nullptr;
        return S_OK;
    }
    IFACEMETHODIMP SetFocus() override { return S_OK; }
    IFACEMETHODIMP get_FragmentRoot(IRawElementProviderFragmentRoot** ret) override {
        AddRef();
        *ret = this;
        return S_OK;
    }

    // IRawElementProviderFragmentRoot
    IFACEMETHODIMP ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** ret) override {
        *ret = nullptr;
        if (!host_->alive) return UIA_E_ELEMENTNOTAVAILABLE;
        POINT p{(LONG)x, (LONG)y};
        ScreenToClient(host_->hwnd, &p);
        const float s = host_->scale(), dx = p.x / s, dy = p.y / s;
        const auto& nodes = host_->ui->Nodes();
        // The last drawn is on top; a row's cells are drawn after the row.
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
            if (!it->r.Contains(dx, dy)) continue;
            ComPtr<IRawElementProviderSimple> n;
            n.Attach(MakeNode(host_, this, it->id));
            return n.CopyTo(ret);
        }
        return S_OK;
    }
    IFACEMETHODIMP GetFocus(IRawElementProviderFragment** ret) override {
        *ret = nullptr;
        if (!host_->alive) return UIA_E_ELEMENTNOTAVAILABLE;
        const UiId id = host_->ui->KeyboardFocus();
        if (!id || !host_->ui->Shown(id)) return S_OK;
        ComPtr<IRawElementProviderSimple> n;
        n.Attach(MakeNode(host_, this, id));
        return n.CopyTo(ret);
    }

private:
    std::atomic<ULONG> refs_{1};
    std::shared_ptr<Host> host_;
};

class NodeProvider final : public IRawElementProviderSimple,
                   public IRawElementProviderFragment,
                   public IInvokeProvider,
                   public IToggleProvider,
                   public ISelectionItemProvider,
                   public ISelectionProvider,
                   public IRangeValueProvider,
                   public IValueProvider {
public:
    NodeProvider(std::shared_ptr<Host> host, IRawElementProviderFragmentRoot* root, UiId id)
        : host_(std::move(host)), root_(root), id_(id) {}

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple))
            *out = static_cast<IRawElementProviderSimple*>(this);
        else if (riid == __uuidof(IRawElementProviderFragment))
            *out = static_cast<IRawElementProviderFragment*>(this);
        else if (riid == __uuidof(IInvokeProvider))
            *out = static_cast<IInvokeProvider*>(this);
        else if (riid == __uuidof(IToggleProvider))
            *out = static_cast<IToggleProvider*>(this);
        else if (riid == __uuidof(ISelectionItemProvider))
            *out = static_cast<ISelectionItemProvider*>(this);
        else if (riid == __uuidof(ISelectionProvider))
            *out = static_cast<ISelectionProvider*>(this);
        else if (riid == __uuidof(IRangeValueProvider))
            *out = static_cast<IRangeValueProvider*>(this);
        else if (riid == __uuidof(IValueProvider))
            *out = static_cast<IValueProvider*>(this);
        else
            return *out = nullptr, E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    IFACEMETHODIMP_(ULONG) Release() override {
        ULONG n = --refs_;
        if (!n) delete this;
        return n;
    }

    // IRawElementProviderSimple
    IFACEMETHODIMP get_ProviderOptions(ProviderOptions* ret) override {
        *ret = (ProviderOptions)kOptions;
        return S_OK;
    }
    IFACEMETHODIMP GetPatternProvider(PATTERNID pattern, IUnknown** ret) override {
        *ret = nullptr;
        const UiNode* n = Get();
        if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
        bool has = false;
        switch (n->role) {
            case Role::Button: has = pattern == UIA_InvokePatternId; break;
            case Role::Item: has = pattern == UIA_InvokePatternId || pattern == UIA_SelectionItemPatternId; break;
            case Role::Toggle: has = pattern == UIA_TogglePatternId; break;
            case Role::Choice:
            case Role::Tab: has = pattern == UIA_SelectionItemPatternId; break;
            case Role::Group: has = pattern == UIA_SelectionPatternId && !ChildrenOf(*host_->ui, id_).empty(); break;
            case Role::Slider: has = pattern == UIA_RangeValuePatternId; break;
            case Role::Edit: has = pattern == UIA_ValuePatternId; break;
        }
        if (has) {
            *ret = static_cast<IRawElementProviderSimple*>(this);
            AddRef();
        }
        return S_OK;
    }
    IFACEMETHODIMP GetPropertyValue(PROPERTYID id, VARIANT* ret) override {
        ret->vt = VT_EMPTY;
        const UiNode* n = Get();
        if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
        const bool inRow = n->role == Role::Choice || n->role == Role::Tab;
        switch (id) {
            case UIA_NamePropertyId: SetBstr(ret, n->name); break;
            case UIA_ControlTypePropertyId: SetInt(ret, ControlType(*n)); break;
            case UIA_LocalizedControlTypePropertyId:
                if (n->role == Role::Toggle) SetBstr(ret, tr(L"토글 스위치"));
                break;
            case UIA_AutomationIdPropertyId: {
                wchar_t s[16];
                swprintf_s(s, L"%08x", id_);
                SetBstr(ret, s);
                break;
            }
            case UIA_IsEnabledPropertyId: SetBool(ret, Usable(*n)); break;
            case UIA_IsKeyboardFocusablePropertyId: SetBool(ret, Usable(*n) && n->role != Role::Group); break;
            case UIA_HasKeyboardFocusPropertyId:
                SetBool(ret, host_->ui->KeyboardFocus() == id_ && ::GetFocus() == host_->hwnd);
                break;
            case UIA_IsOffscreenPropertyId: SetBool(ret, n->r.Empty()); break;
            case UIA_AcceleratorKeyPropertyId:
                if (!n->keys.empty()) SetBstr(ret, n->keys);
                break;
            case UIA_PositionInSetPropertyId:
                if (inRow) SetInt(ret, n->pos + 1);
                break;
            case UIA_SizeOfSetPropertyId:
                if (inRow) SetInt(ret, n->count);
                break;
            case UIA_ProviderDescriptionPropertyId: SetBstr(ret, L"OzMaker"); break;
        }
        return S_OK;
    }
    IFACEMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple** ret) override {
        *ret = nullptr;
        return S_OK;
    }

    // IRawElementProviderFragment
    IFACEMETHODIMP Navigate(NavigateDirection dir, IRawElementProviderFragment** ret) override {
        *ret = nullptr;
        const UiNode* n = Get();
        if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
        const Ui& ui = *host_->ui;
        UiId to = 0;
        const UiId parent = ParentOf(ui, *n);
        switch (dir) {
            case NavigateDirection_Parent:
                if (!parent) return root_->QueryInterface(IID_PPV_ARGS(ret));
                to = parent;
                break;
            case NavigateDirection_NextSibling:
            case NavigateDirection_PreviousSibling: {
                auto sib = ChildrenOf(ui, parent);
                auto it = std::find(sib.begin(), sib.end(), id_);
                if (it == sib.end()) break;
                if (dir == NavigateDirection_NextSibling && it + 1 != sib.end()) to = it[1];
                if (dir == NavigateDirection_PreviousSibling && it != sib.begin()) to = it[-1];
                break;
            }
            case NavigateDirection_FirstChild:
            case NavigateDirection_LastChild: {
                auto kids = ChildrenOf(ui, id_);
                if (!kids.empty()) to = dir == NavigateDirection_FirstChild ? kids.front() : kids.back();
                break;
            }
        }
        if (!to) return S_OK;
        ComPtr<IRawElementProviderSimple> p;
        p.Attach(MakeNode(host_, root_.Get(), to));
        return p.CopyTo(ret);
    }
    IFACEMETHODIMP GetRuntimeId(SAFEARRAY** ret) override {
        int rid[] = {UiaAppendRuntimeId, (int)id_};
        *ret = SafeArrayCreateVector(VT_I4, 0, 2);
        if (!*ret) return E_OUTOFMEMORY;
        for (LONG i = 0; i < 2; ++i) SafeArrayPutElement(*ret, &i, &rid[i]);
        return S_OK;
    }
    IFACEMETHODIMP get_BoundingRectangle(UiaRect* ret) override {
        *ret = {};
        const UiNode* n = Get();
        if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
        if (n->r.Empty()) return S_OK;
        POINT o{};
        ClientToScreen(host_->hwnd, &o);
        const float s = host_->scale();
        *ret = {o.x + n->r.l * s, o.y + n->r.t * s, n->r.W() * s, n->r.H() * s};
        return S_OK;
    }
    IFACEMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY** ret) override {
        *ret = nullptr;
        return S_OK;
    }
    IFACEMETHODIMP SetFocus() override {
        const UiNode* n = Get();
        if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
        if (!Usable(*n) || n->role == Role::Group) return UIA_E_INVALIDOPERATION;
        host_->ui->SetKeyboardFocus(id_);
        host_->invalidate();
        return S_OK;
    }
    IFACEMETHODIMP get_FragmentRoot(IRawElementProviderFragmentRoot** ret) override {
        return root_.CopyTo(ret);
    }

    // Invoke, Toggle
    IFACEMETHODIMP Invoke() override { return Press(); }
    IFACEMETHODIMP Toggle() override { return Press(); }
    IFACEMETHODIMP get_ToggleState(ToggleState* ret) override {
        const UiNode* n = Get();
        if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
        *ret = n->on ? ToggleState_On : ToggleState_Off;
        return S_OK;
    }

    // SelectionItem: a cell of a row, picked by pressing it.
    IFACEMETHODIMP Select() override {
        const UiNode* n = Get();
        if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
        return n->on ? S_OK : Press();
    }
    IFACEMETHODIMP AddToSelection() override { return Select(); }
    IFACEMETHODIMP RemoveFromSelection() override {
        const UiNode* n = Get();
        if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
        return n->on ? UIA_E_INVALIDOPERATION : S_OK;
    }
    IFACEMETHODIMP get_IsSelected(BOOL* ret) override {
        const UiNode* n = Get();
        if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
        *ret = n->on;
        return S_OK;
    }
    IFACEMETHODIMP get_SelectionContainer(IRawElementProviderSimple** ret) override {
        *ret = nullptr;
        const UiNode* n = Get();
        if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
        if (const UiId row = ParentOf(*host_->ui, *n)) *ret = MakeNode(host_, root_.Get(), row);
        return S_OK;
    }

    // Selection: the row.
    IFACEMETHODIMP GetSelection(SAFEARRAY** ret) override {
        *ret = nullptr;
        if (!Get()) return UIA_E_ELEMENTNOTAVAILABLE;
        std::vector<UiId> on;
        for (UiId c : ChildrenOf(*host_->ui, id_))
            if (host_->ui->Shown(c)->on) on.push_back(c);
        *ret = SafeArrayCreateVector(VT_UNKNOWN, 0, (ULONG)on.size());
        if (!*ret) return E_OUTOFMEMORY;
        for (LONG i = 0; i < (LONG)on.size(); ++i) {
            ComPtr<IRawElementProviderSimple> p;
            p.Attach(MakeNode(host_, root_.Get(), on[i]));
            SafeArrayPutElement(*ret, &i, p.Get());  // takes its own reference
        }
        return S_OK;
    }
    IFACEMETHODIMP get_CanSelectMultiple(BOOL* ret) override {
        *ret = FALSE;
        return S_OK;
    }
    IFACEMETHODIMP get_IsSelectionRequired(BOOL* ret) override {
        *ret = TRUE;
        return S_OK;
    }

    // RangeValue: a slider.
    IFACEMETHODIMP SetValue(double v) override {
        const UiNode* n = Get();
        if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
        if (!n->settable) return UIA_E_INVALIDOPERATION;  // a text field is typed in; the timeline only shows
        if (!Usable(*n)) return UIA_E_ELEMENTNOTENABLED;
        if (v < n->lo || v > n->hi) return E_INVALIDARG;
        host_->ui->SetSliderValue(id_, (float)v);
        host_->invalidate();
        return S_OK;
    }
    IFACEMETHODIMP get_Value(double* ret) override { return Range(ret, [](const UiNode& n) { return n.value; }); }
    IFACEMETHODIMP get_Minimum(double* ret) override { return Range(ret, [](const UiNode& n) { return n.lo; }); }
    IFACEMETHODIMP get_Maximum(double* ret) override { return Range(ret, [](const UiNode& n) { return n.hi; }); }
    IFACEMETHODIMP get_SmallChange(double* ret) override {
        return Range(ret, [](const UiNode& n) { return (n.hi - n.lo) / 20; });
    }
    IFACEMETHODIMP get_LargeChange(double* ret) override {
        return Range(ret, [](const UiNode& n) { return (n.hi - n.lo) / 5; });
    }
    IFACEMETHODIMP get_IsReadOnly(BOOL* ret) override {
        const UiNode* n = Get();
        if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
        *ret = !Usable(*n) || (n->role == Role::Slider && !n->settable);
        return S_OK;
    }

    // Value: a text field.
    IFACEMETHODIMP SetValue(LPCWSTR) override { return UIA_E_INVALIDOPERATION; }
    IFACEMETHODIMP get_Value(BSTR* ret) override {
        const UiNode* n = Get();
        if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
        *ret = SysAllocStringLen(n->text.data(), (UINT)n->text.size());
        return S_OK;
    }

private:
    const UiNode* Get() const { return host_->alive ? host_->ui->Shown(id_) : nullptr; }

    HRESULT Press() {
        const UiNode* n = Get();
        if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
        if (!Usable(*n)) return UIA_E_ELEMENTNOTENABLED;
        host_->ui->Press(id_);
        host_->invalidate();
        return S_OK;
    }

    template <class F>
    HRESULT Range(double* ret, F f) {
        const UiNode* n = Get();
        if (!n) return UIA_E_ELEMENTNOTAVAILABLE;
        *ret = f(*n);
        return S_OK;
    }

    int ControlType(const UiNode& n) const {
        switch (n.role) {
            case Role::Toggle: return UIA_ButtonControlTypeId;  // a switch, as Windows' own are
            case Role::Choice: return UIA_RadioButtonControlTypeId;
            case Role::Tab: return UIA_TabItemControlTypeId;
            case Role::Group: {
                auto kids = ChildrenOf(*host_->ui, id_);
                return !kids.empty() && host_->ui->Shown(kids[0])->role == Role::Tab ? UIA_TabControlTypeId
                                                                                      : UIA_GroupControlTypeId;
            }
            case Role::Slider: return UIA_SliderControlTypeId;
            case Role::Edit: return UIA_EditControlTypeId;
            case Role::Item: return UIA_ListItemControlTypeId;
            default: return UIA_ButtonControlTypeId;
        }
    }

    std::atomic<ULONG> refs_{1};
    std::shared_ptr<Host> host_;
    ComPtr<IRawElementProviderFragmentRoot> root_;
    UiId id_;
};

IRawElementProviderSimple* MakeNode(const std::shared_ptr<Host>& host, IRawElementProviderFragmentRoot* root, UiId id) {
    return new NodeProvider(host, root, id);
}

}  // namespace

void Access::Attach(HWND hwnd, Ui* ui, std::function<float()> scale, std::function<void()> invalidate) {
    host_ = std::make_shared<Host>();
    host_->hwnd = hwnd;
    host_->ui = ui;
    host_->scale = std::move(scale);
    host_->invalidate = std::move(invalidate);
    root_.Attach(new RootProvider(host_));
}

void Access::Detach() {
    if (!host_) return;
    host_->alive = false;
    UiaReturnRawElementProvider(host_->hwnd, 0, 0, nullptr);
    UiaDisconnectProvider(root_.Get());
    root_.Reset();
}

LRESULT Access::OnGetObject(WPARAM w, LPARAM l) {
    if (!root_ || !host_->alive) return 0;
    return UiaReturnRawElementProvider(host_->hwnd, w, l, root_.Get());
}

void Access::AfterFrame() {
    if (!host_ || !host_->alive) return;
    Ui& ui = *host_->ui;
    const bool moved = ui.TakeFocusMoved();
    uint64_t tree = 1469598103934665603ull;
    for (const UiNode& n : ui.Nodes())
        if (!ParentOf(ui, n)) tree = (tree ^ n.id) * 1099511628211ull;
    const UiNode* f = ui.Shown(ui.KeyboardFocus());
    const bool listening = UiaClientsAreListening();
    if (listening && tree != lastTree_)
        UiaRaiseStructureChangedEvent(root_.Get(), StructureChangeType_ChildrenInvalidated, nullptr, 0);
    if (listening && f && ::GetFocus() == host_->hwnd) {
        ComPtr<IRawElementProviderFragmentRoot> root;
        root_.As(&root);
        ComPtr<IRawElementProviderSimple> p;
        p.Attach(MakeNode(host_, root.Get(), f->id));
        if (moved || f->id != lastFocus_) {
            UiaRaiseAutomationEvent(p.Get(), UIA_AutomationFocusChangedEventId);
        } else {
            VARIANT was{}, now{};
            if (f->on != lastOn_) {
                const bool cell = f->role == Role::Choice || f->role == Role::Tab;
                if (cell) {
                    SetBool(&was, lastOn_);
                    SetBool(&now, f->on);
                    UiaRaiseAutomationPropertyChangedEvent(p.Get(), UIA_SelectionItemIsSelectedPropertyId, was, now);
                } else if (f->role == Role::Toggle) {
                    SetInt(&was, lastOn_ ? ToggleState_On : ToggleState_Off);
                    SetInt(&now, f->on ? ToggleState_On : ToggleState_Off);
                    UiaRaiseAutomationPropertyChangedEvent(p.Get(), UIA_ToggleToggleStatePropertyId, was, now);
                }
            }
            if (f->role == Role::Slider && f->value != lastValue_) {
                was.vt = now.vt = VT_R8;
                was.dblVal = lastValue_;
                now.dblVal = f->value;
                UiaRaiseAutomationPropertyChangedEvent(p.Get(), UIA_RangeValueValuePropertyId, was, now);
            }
            if (f->role == Role::Edit && f->text != lastText_) {
                SetBstr(&was, lastText_);
                SetBstr(&now, f->text);
                UiaRaiseAutomationPropertyChangedEvent(p.Get(), UIA_ValueValuePropertyId, was, now);
                VariantClear(&was);
                VariantClear(&now);
            }
        }
    }
    lastTree_ = tree;
    lastFocus_ = f ? f->id : 0;
    lastOn_ = f && f->on;
    lastValue_ = f ? f->value : 0;
    lastText_ = f ? f->text : std::wstring();
}
