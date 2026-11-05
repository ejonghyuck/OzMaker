#include "pch.h"
#include "App.h"
#include "AnimClip.h"
#include "Crash.h"
#include "Mp4Cut.h"
#include "Resize.h"
#include "Sound.h"
#include "Util.h"
#include "Version.h"
#include "../res/resource.h"

using namespace theme;

namespace {

constexpr float kMinW = 920, kMinH = 640;

// Red record dot with a white rim (tray icon while recording).
HICON MakeRecIcon(int size) {
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    auto* px = static_cast<uint32_t*>(bits);
    const float c = size / 2.f, outer = size * 0.47f, inner = size * 0.36f;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float aOuter = 0, aInner = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    float fx = x + (sx + 0.5f) / 4 - c, fy = y + (sy + 0.5f) / 4 - c;
                    float d = std::sqrt(fx * fx + fy * fy);
                    aOuter += d <= outer;
                    aInner += d <= inner;
                }
            aOuter /= 16;
            aInner /= 16;
            // inner red over white rim
            float r = 255 * (1 - aInner) + 255 * aInner, g = 255 * (1 - aInner) + 77 * aInner,
                  b = 255 * (1 - aInner) + 94 * aInner;
            uint32_t A = (uint32_t)(aOuter * 255 + 0.5f);
            px[y * size + x] = (A << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

std::vector<std::wstring> SplitArgs(const std::wstring& cmd) {
    std::vector<std::wstring> out;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(cmd.empty() ? L"x" : (L"x " + cmd).c_str(), &argc)) {
        for (int i = 1; i < argc; ++i) out.emplace_back(argv[i]);
        LocalFree(argv);
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------- lifecycle

int App::Run(HINSTANCE inst, int show, const std::wstring& cmdLine) {
    inst_ = inst;
    settings_.Load();
    ApplyLanguage();
    ApplyTheme();
    if (!gfx_.Init()) {
        MessageBoxW(nullptr, tr(L"그래픽 장치를 초기화할 수 없습니다."), L"OzMaker", MB_ICONERROR);
        return 1;
    }
    Fonts::Init(gfx_.DWrite());
    overlay_.Init(inst, gfx_.D2D());
    rpNotice_.Init(inst, gfx_.D2D());
    picker_.Init(inst, gfx_.D2D());
    regionPicker_.Init(inst, gfx_.D2D());

    auto args = SplitArgs(cmdLine);
    // Every flag but --tray and --updated is a test mode. Those draw the same on any PC (CI runs on English
    // Windows): Korean and dark. --screenshot picks its own look.
    if (!args.empty() && args[0].starts_with(L"--") && args[0] != L"--tray" && args[0] != L"--updated") {
        settings_.theme = kThemeDark;
        settings_.language = kLangKorean;
        ApplyLanguage();
        ApplyTheme();
    }
    if (args.size() >= 2 && args[0] == L"--crash-test") return RunCrashTest(args);
    if (args.size() >= 2 && args[0] == L"--screenshot") return RunScreenshots(args[1], args.size() >= 3 ? args[2] : L"");
    if (args.size() >= 2 && args[0] == L"--readme-media") return RunReadmeMedia(args[1]);
    if (args.size() >= 2 && args[0] == L"--selftest") return RunSelfTest(args[1]);
    if (args.size() >= 2 && args[0] == L"--record-test") return RunRecordTest(cmdLine);
    if (args.size() >= 2 && args[0] == L"--analyze") return RunAnalyzeTest(cmdLine);
    if (args.size() >= 3 && args[0] == L"--editor-test") return RunEditorTest(args[1], args[2]);
    if (args.size() >= 2 && args[0] == L"--region-test") return RunRegionTest(args[1]);
    if (args.size() >= 2 && args[0] == L"--preview-test") return RunPreviewTest(args[1]);
    if (args.size() >= 2 && args[0] == L"--encoder-test") return RunEncoderTest(args[1]);
    if (args.size() >= 2 && args[0] == L"--replay-test") return RunReplayTest(args[1]);
    if (args.size() >= 2 && args[0] == L"--multi-test") return RunMultiTest(args[1]);
    if (args.size() >= 7 && args[0] == L"--test-window" && args[2] == L"cover") {
        const int x = _wtoi(args[3].c_str()), y = _wtoi(args[4].c_str());
        const RECT r{x, y, x + _wtoi(args[5].c_str()), y + _wtoi(args[6].c_str())};
        return RunTestWindow(_wtof(args[1].c_str()), false, &r);
    }
    if (args.size() >= 2 && args[0] == L"--test-window")
        return RunTestWindow(_wtof(args[1].c_str()), args.size() >= 3 && args[2] == L"capture");
    if (args.size() >= 2 && args[0] == L"--update-test") return RunUpdateTest(args[1], args.size() >= 3 ? args[2] : L"");
    if (args.size() >= 2 && args[0] == L"--video-test")
        return RunVideoTest(args[1], args.size() >= 4 ? _wtoi(args[2].c_str()) : 640,
                            args.size() >= 4 ? _wtoi(args[3].c_str()) : 360);

    // --tray: started with Windows (HKCU Run). --updated <pid> <shown>: restarted by the version before (main.cpp).
    const bool updated = args.size() >= 3 && args[0] == L"--updated";
    const bool tray = (!args.empty() && args[0] == L"--tray") || (updated && args[2] == L"0");
    if (updated) Updater::CleanUp(util::ExePath());
    // Keeps pointing at this exe if it was moved. A portable copy leaves this PC's startup programs alone.
    autostart_ = !Settings::Portable() && util::AutostartEnabled();
    if (autostart_) util::SetAutostart(true);
    if (!CreateMainWindow(tray ? SW_HIDE : show)) return 1;
    // The PC locking or going to sleep lets go of what 순간 리플레이 held.
    WTSRegisterSessionNotification(hwnd_, NOTIFY_FOR_THIS_SESSION);
    if (settings_.rpOn) SetReplayOn(true, false);  // on as it was left
    updater_.Init(util::ExePath(), hwnd_, WM_APP_UPDATE);
    SetTimer(hwnd_, kTimerUpdate, 15000, nullptr);  // the first check, once startup is out of the way
    if (updated) {
        if (tray)
            TrayBalloon(tr(L"OzMaker를 업데이트했습니다"), util::Format(tr(L"버전 %s"), OZ_VERSION_WSTR));
        else
            Toast(util::Format(tr(L"OzMaker %s 버전으로 업데이트했습니다"), OZ_VERSION_WSTR));
    }
    // A crash report written since the user was last told, and the recordings it left behind: say so once (#24).
    bool crashed = false;
    if (auto reports = crash::List(); !reports.empty() && reports[0].name > settings_.crashSeen) {
        settings_.crashSeen = reports[0].name;
        settings_.Save();
        crashed = true;
    }
    crash::Prune(5);
    size_t clips = RecoverRecordings();
    std::vector<std::wstring> videos = Mp4Writer::Recover();  // newest first
    if (crashed || clips || !videos.empty()) {
        std::wstring title = crashed ? tr(L"지난번에 OzMaker가 비정상 종료되었습니다") : tr(L"지난번 녹화를 되살렸습니다");
        std::wstring body =
            clips && !videos.empty() ? util::Format(tr(L"녹화 %zu개와 동영상 %zu개를 되살렸습니다"), clips, videos.size())
            : clips    ? util::Format(tr(L"녹화 %zu개를 되살렸습니다 · 파일 목록의 \"저장하지 않은 녹화\"에 있습니다"), clips)
            : !videos.empty() ? (videos.size() == 1 ? tr(L"녹화하던 동영상을 되살렸습니다 · ") + util::FileNameOf(videos[0])
                                                    : util::Format(tr(L"녹화하던 동영상 %zu개를 되살렸습니다"), videos.size()))
                              : std::wstring(tr(L"설정 › 정보 › 충돌 기록에서 볼 수 있습니다"));
        // A video is already a file in the save folder: it opens like one that was just saved.
        for (size_t i = videos.size(); i-- > 1;) AddRecent(videos[i]);
        if (!videos.empty()) {
            CaptureResult r;
            r.video = r.ok = true;
            r.path = videos[0];
            r.bytes = util::FileSize(r.path);
            std::wstring err;
            if (auto v = VideoClip::Open(r.path, err)) {
                r.seconds = v->Seconds();
                r.audio = v->HasAudio();
            }
            ShowVideo(r);
        } else if (clips && !tray) {
            clipName_ = held_[0].name;
            OpenEditor(held_[0].clip);
        }
        if (tray)
            TrayBalloon(title, body);
        else if (crashed)
            Alert(title + L". " + body + L".");
        else
            Toast(title + L". " + body + L".");
    }
    if (!Capture::IsSupported())
        Toast(tr(L"이 Windows에서는 화면 캡처(Windows.Graphics.Capture)를 사용할 수 없습니다."), true);
    else
        Capture::RequestBorderless();  // answered before the first recording (Windows 11: no yellow border)
    if (!args.empty() && args[0].rfind(L"--", 0) != 0) OpenMediaFile(args[0]);  // "Open with", or a file dropped on the exe
    Loop();
    return 0;
}

// ---------------------------------------------------------------- drag and drop

// The first of the files dragged over the window. While they are over it, the whole window says what dropping them
// would do (DrawDropCue); a video or a 움짤 can be dropped.
class App::DropTarget : public IDropTarget {
public:
    explicit DropTarget(App* app) : app_(app) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *out = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = --refs_;
        if (!n) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        path_.clear();
        FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM med{};
        if (data && SUCCEEDED(data->GetData(&fmt, &med))) {
            wchar_t p[MAX_PATH * 4];
            if (DragQueryFileW((HDROP)med.hGlobal, 0, p, _countof(p)) > 0) path_ = p;
            ReleaseStgMedium(&med);
        }
        app_->dragKind_ = path_.empty() ? DragKind::None : app_->ClassifyDrop(path_);
        app_->dragName_ = path_.empty() ? L"" : util::FileNameOf(path_);
        app_->Invalidate();
        return DragOver(0, {}, effect);
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        if (!path_.empty()) app_->dragKind_ = app_->ClassifyDrop(path_);  // a recording may have started meanwhile
        *effect = app_->DragOpens() ? (*effect & DROPEFFECT_COPY) : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        app_->dragKind_ = DragKind::None;
        app_->dragName_.clear();
        app_->Invalidate();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject*, DWORD, POINTL, DWORD* effect) override {
        const bool open = app_->DragOpens();
        DragLeave();
        *effect = open ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        // After DoDragDrop returns in the program the file came from: Explorer stays usable while it opens.
        if (open) {
            app_->pendingOpen_ = path_;
            PostMessageW(app_->hwnd_, WM_APP_OPEN, 0, 0);
        }
        return S_OK;
    }

private:
    App* app_;
    ULONG refs_ = 1;
    std::wstring path_;
};

ComPtr<IDropTarget> App::MakeDropTarget() {
    ComPtr<IDropTarget> t;
    t.Attach(new DropTarget(this));
    return t;
}

App::DragKind App::ClassifyDrop(const std::wstring& path) const {
    if (rec_ != RecState::Idle || exporter_.Busy()) return DragKind::Busy;
    if (util::IsVideoPath(path)) return DragKind::Video;
    return util::IsAnimPath(path) ? DragKind::Anim : DragKind::Other;
}

// ---------------------------------------------------------------- window

bool App::CreateMainWindow(int show) {
    int smX = GetSystemMetrics(SM_CXSMICON), smY = GetSystemMetrics(SM_CYSMICON);
    iconApp_ = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    iconAppSmall_ = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, smX, smY, 0);
    iconRec_ = MakeRecIcon(smX);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst_;
    wc.hIcon = iconApp_;
    wc.hIconSm = iconAppSmall_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"OzMakerMain";
    if (!RegisterClassExW(&wc)) return false;

    POINT origin{0, 0};
    HMONITOR mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    float s = dx / 96.f;
    // Shrunk to fit small or highly scaled screens (1920×1080 at 150% leaves under 700 DIP of height).
    const RECT& wa = mi.rcWork;
    int w = std::min((int)(1120 * s), (int)(wa.right - wa.left)), h = std::min((int)(780 * s), (int)(wa.bottom - wa.top));
    int x = (wa.left + wa.right - w) / 2, y = std::max(wa.top, (wa.top + wa.bottom - h) / 2);
    // The last position, as long as its title bar is still on a screen (monitors come and go).
    const RECT& saved = settings_.window;
    bool restored = false;
    if (saved.right - saved.left >= 200 && saved.bottom - saved.top >= 150) {
        RECT bar{saved.left, saved.top, saved.right, saved.top + 32}, vis;
        MONITORINFO sm{sizeof(sm)};
        HMONITOR m = MonitorFromRect(&bar, MONITOR_DEFAULTTONULL);
        if (m && GetMonitorInfoW(m, &sm) && IntersectRect(&vis, &bar, &sm.rcWork) && vis.right - vis.left >= 100) {
            x = saved.left;
            y = std::max(saved.top, sm.rcWork.top);
            w = std::min(saved.right - saved.left, sm.rcWork.right - sm.rcWork.left);
            h = std::min(saved.bottom - saved.top, sm.rcWork.bottom - sm.rcWork.top);
            restored = true;
        }
    }

    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"OzMaker", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, inst_,
                            this);
    if (!hwnd_) return false;
    dpi_ = GetDpiForWindow(hwnd_);
    ApplyTheme();  // the title bar
    if (!gfx_.AttachWindow(hwnd_)) return false;
    access_.Attach(hwnd_, &ui_, [this] { return Scale(); }, [this] { Invalidate(); });
    // No IME while no text field has the focus: in Korean mode it would swallow the letter shortcuts (I, O, J, K, L).
    ImmAssociateContextEx(hwnd_, nullptr, 0);
    dropTarget_ = MakeDropTarget();
    if (FAILED(OleInitialize(nullptr)) || FAILED(RegisterDragDrop(hwnd_, dropTarget_.Get()))) {
        dropTarget_.Reset();
        DragAcceptFiles(hwnd_, TRUE);  // no cue while dragging, but a drop still opens the file
    }

    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    TrayAdd();
    RegisterRecordHotkey();
    TrayUpdate();
    if (!hotkeyOk_)
        TrayBalloon(tr(L"녹화 단축키를 쓸 수 없습니다"),
                    util::HotkeyToString(settings_.hkMods, settings_.hkVk) +
                        tr(L"을(를) 다른 프로그램이 쓰고 있습니다. 설정에서 다른 조합을 지정하세요."));
    RefreshWindows();
    WatchWindows(true);
    if (restored && settings_.windowMax && show != SW_HIDE && show != SW_SHOWMINIMIZED && show != SW_MINIMIZE)
        show = SW_SHOWMAXIMIZED;
    ShowWindow(hwnd_, show);
    return true;
}

void App::Loop() {
    MSG msg;
    for (;;) {
        crash::SetState(Activity());
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        bool visible = hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
        double now = util::NowSeconds();
        if (visible && (dirty_ || ui_.Animating() || (wake_ > 0 && now >= wake_))) {
            Render();
            continue;
        }
        DWORD timeout = INFINITE;
        if (visible && wake_ > 0) timeout = (DWORD)std::clamp((wake_ - now) * 1000.0, 0.0, 1000.0);
        MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

void App::Render() {
    if (!hwnd_) return;
    dirty_ = false;
    if (gfx_.Generation() != iconGen_) {
        // Device was (re)created: drop GPU bitmaps; they are rebuilt lazily.
        iconGen_ = gfx_.Generation();
        icons_.clear();
        frameBmp_.Reset();
        shown_ = SIZE_MAX;
        thumbs_.clear();
        snapBmp_.Reset();
        PostMessageW(hwnd_, WM_APP_THUMBS, 0, 0);
    }
    ID2D1DeviceContext* dc = gfx_.BeginFrame((float)dpi_);
    if (!dc) return;
    double now = util::NowSeconds();
    wake_ = 0;
    ui_.Begin(dc, input_, now);
    input_.ClearEdges();
    RECT rc;
    GetClientRect(hwnd_, &rc);
    thumbWanted_ = false;
    thumbCells_.clear();
    DrawFrame(Rect(0, 0, rc.right / Scale(), rc.bottom / Scale()));
    ui_.End();
    gfx_.EndFrame();
    access_.AfterFrame();
    ApplyThumbnail();
    SyncAudioPreview();
    SyncIme();
    RequestWake(ui_.WakeAt());
    if (settingsDirty_ && !input_.down) {
        settings_.Save();
        settingsDirty_ = false;
    }

    POINT pt;
    GetCursorPos(&pt);
    if (WindowFromPoint(pt) == hwnd_ && SendMessageW(hwnd_, WM_NCHITTEST, 0, MAKELPARAM(pt.x, pt.y)) == HTCLIENT)
        SetCursor(LoadCursorW(nullptr, ui_.Cursor()));
}

// The IME only while a text field has the focus (in Korean mode it takes the letter keys), its windows at the caret.
void App::SyncIme() {
    if (ui_.TakeImeReset()) {
        // The field kept what was being composed when it let go.
        if (HIMC imc = ImmGetContext(hwnd_)) {
            ImmNotifyIME(imc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
            ImmReleaseContext(hwnd_, imc);
        }
        input_.composing.clear();
    }
    const bool on = ui_.TextFocused();
    if (on != imeOn_) {
        ImmAssociateContextEx(hwnd_, nullptr, on ? IACE_DEFAULT : 0);
        imeOn_ = on;
        if (!on) input_.composing.clear();
    }
    if (!on) return;
    if (HIMC imc = ImmGetContext(hwnd_)) {
        const Rect& c = ui_.CaretRect();
        const float s = Scale();
        RECT px{(LONG)(c.l * s), (LONG)(c.t * s), (LONG)(c.r * s), (LONG)(c.b * s)};
        COMPOSITIONFORM cf{CFS_POINT, {px.left, px.top}, {}};
        ImmSetCompositionWindow(imc, &cf);
        CANDIDATEFORM cand{0, CFS_EXCLUDE, {px.left, px.bottom}, px};
        ImmSetCandidateWindow(imc, &cand);
        ImmReleaseContext(hwnd_, imc);
    }
}

// DWM draws thumbnails above everything drawn in this window, so nothing of ours can go over the preview. Instead
// the preview is cut around whatever floats above the page this frame (popovers, toasts, tooltips: Ui floats), as
// a thumbnail per part of the window left showing.
void App::ApplyThumbnail() {
    const bool shown = screen_ == Screen::Home && IsWindowVisible(hwnd_);
    ApplyPreview(thumb_, shown && thumbWanted_ ? target_ : nullptr, thumbArea_);
    if (thumbMore_.size() < thumbCells_.size()) thumbMore_.resize(thumbCells_.size());
    for (size_t i = 0; i < thumbMore_.size(); ++i) {
        const bool on = shown && i < thumbCells_.size();
        ApplyPreview(thumbMore_[i], on ? thumbCells_[i].hwnd : nullptr, on ? thumbCells_[i].area : Rect());
    }
    while (!thumbMore_.empty() && thumbMore_.back().thumb.empty()) thumbMore_.pop_back();
}

void App::ApplyPreview(LivePreview& pv, HWND target, const Rect& area) {
    const bool want = target && IsWindow(target);
    if (!want || pv.src != target) ReleasePreview(pv);
    if (!want || (pv.thumb.empty() && !AddThumbnail(pv, target))) return;
    // DWM reports the size of what a thumbnail shows (just the client area with 제목 표시줄 제외): that goes first.
    DWM_THUMBNAIL_PROPERTIES only{};
    only.dwFlags = DWM_TNP_SOURCECLIENTAREAONLY;
    only.fSourceClientAreaOnly = settings_.clientOnly;
    DwmUpdateThumbnailProperties(pv.thumb[0], &only);
    RECT dest, src;
    PreviewLayout(pv, target, area, dest, src);
    std::vector<RECT> parts = PreviewParts(dest);
    const bool split = !(parts.size() == 1 && EqualRect(&parts[0], &dest));
    if (!split && pv.split) {  // a thumbnail given a source rect can't be told to show the whole window again
        ReleasePreview(pv);
        if (!AddThumbnail(pv, target)) return;
    }
    while (pv.thumb.size() < parts.size() && AddThumbnail(pv, target)) {
    }
    const LONG dw = std::max(1L, dest.right - dest.left), dh = std::max(1L, dest.bottom - dest.top);
    auto sx = [&](LONG x) { return src.left + (LONG)std::lround((double)(x - dest.left) * (src.right - src.left) / dw); };
    auto sy = [&](LONG y) { return src.top + (LONG)std::lround((double)(y - dest.top) * (src.bottom - src.top) / dh); };
    for (size_t i = 0; i < pv.thumb.size(); ++i) {
        DWM_THUMBNAIL_PROPERTIES p{};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        p.fVisible = i < parts.size();
        p.opacity = 255;
        p.fSourceClientAreaOnly = settings_.clientOnly;
        if (p.fVisible) {
            const RECT& d = parts[i];
            p.rcDestination = d;
            if (split) {
                p.dwFlags |= DWM_TNP_RECTSOURCE;
                p.rcSource = {sx(d.left), sy(d.top), sx(d.right), sy(d.bottom)};
                pv.split = true;
            }
        }
        DwmUpdateThumbnailProperties(pv.thumb[i], &p);
    }
}

bool App::AddThumbnail(LivePreview& pv, HWND target) {
    HTHUMBNAIL t = nullptr;
    if (FAILED(DwmRegisterThumbnail(hwnd_, target, &t))) return false;
    pv.thumb.push_back(t);
    pv.src = target;
    return true;
}

void App::ReleasePreview(LivePreview& pv) {
    for (HTHUMBNAIL t : pv.thumb) DwmUnregisterThumbnail(t);
    pv.thumb.clear();
    pv.src = nullptr;
    pv.split = false;
}

void App::ReleaseThumbnails() {
    ReleasePreview(thumb_);
    for (LivePreview& p : thumbMore_) ReleasePreview(p);
    thumbMore_.clear();
}

void App::PreviewLayout(RECT& dest, RECT& source) { PreviewLayout(thumb_, target_, thumbArea_, dest, source); }

void App::PreviewLayout(const LivePreview& pv, HWND target, const Rect& area, RECT& dest, RECT& source) {
    // Laid out by its size on screen. Source rects are in DWM's units: the size it reports for what the thumbnail
    // shows (smaller than on screen for a window Windows scales up, not being DPI aware), from the client area's
    // corner, or for the whole window from its corner including the invisible resize borders, which that size
    // leaves out. Without a thumbnail (screenshots), the size on screen.
    SIZE shown{}, dwm{};
    WINDOWINFO wi{sizeof(wi)};
    if (settings_.clientOnly && GetWindowInfo(target, &wi))
        shown = {wi.rcClient.right - wi.rcClient.left, wi.rcClient.bottom - wi.rcClient.top};
    if (!pv.thumb.empty()) DwmQueryThumbnailSourceSize(pv.thumb[0], &dwm);
    if (shown.cx <= 0 || shown.cy <= 0) shown = dwm;
    if (shown.cx <= 0 || shown.cy <= 0) {
        RECT wr{};
        GetWindowRect(target, &wr);
        shown = {wr.right - wr.left, wr.bottom - wr.top};
    }
    if (shown.cx <= 0 || shown.cy <= 0) shown = {16, 9};
    if (dwm.cx <= 0 || dwm.cy <= 0) dwm = shown;
    source = {0, 0, dwm.cx, dwm.cy};
    RECT wr, frame;
    if (!settings_.clientOnly && GetWindowRect(target, &wr) &&
        SUCCEEDED(DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) &&
        frame.right > frame.left) {
        double k = (double)dwm.cx / (frame.right - frame.left);
        OffsetRect(&source, (int)std::lround((frame.left - wr.left) * k), (int)std::lround((frame.top - wr.top) * k));
    }
    float s = Scale();
    float aw = area.W() * s, ah = area.H() * s;
    float f = std::min({aw / shown.cx, ah / shown.cy, 1.f});
    int dw = (int)(shown.cx * f), dh = (int)(shown.cy * f);
    int x = (int)(area.l * s + (aw - dw) / 2), y = (int)(area.t * s + (ah - dh) / 2);
    dest = {x, y, x + dw, y + dh};
}

std::vector<RECT> App::PreviewParts(const RECT& dest) {
    float s = Scale();
    std::vector<RECT> holes;
    for (const Rect& f : ui_.Floats())
        holes.push_back({(LONG)std::floor(f.l * s), (LONG)std::floor(f.t * s), (LONG)std::ceil(f.r * s),
                         (LONG)std::ceil(f.b * s)});
    auto parts = util::SubtractRects(dest, holes);
    if (parts.size() > 16) parts.clear();  // nothing floats over it that busily; just don't show it meanwhile
    return parts;
}

void App::ShowMain(bool activate) {
    if (!hwnd_) return;
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, activate ? SW_RESTORE : SW_SHOWNOACTIVATE);
    ShowWindow(hwnd_, activate ? SW_SHOW : SW_SHOWNA);
    if (activate) SetForegroundWindow(hwnd_);
    if (rec_ == RecState::Recording) SetRecordingLook(true);  // a taskbar button that only now exists
    if (screen_ == Screen::Home || screen_ == Screen::Replay) RefreshWindows();
    Invalidate();
}

void App::HideMain() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
    SyncAudioPreview();
}

void App::MinimizeMain() {
    if (hwnd_ && IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_MINIMIZE);  // hidden in the tray, it stays there
    SyncAudioPreview();
}

void App::SetRecordingLook(bool on) {
    if (!hwnd_) return;
    // Opened while a screen is being recorded, the window doesn't end up in it (Windows 10 2004 and later). Only then:
    // a window's own capture never shows other windows, and excluded, the open window was once drawn shifted and
    // flickering (see Gfx::CreateSwapChain). Not for replay either, which runs while the window is open.
    SetWindowDisplayAffinity(hwnd_, on && recScreen_ ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
    if (!taskbar_ &&
        SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar_))) &&
        FAILED(taskbar_->HrInit()))
        taskbar_.Reset();
    if (taskbar_) taskbar_->SetOverlayIcon(hwnd_, on ? iconRec_ : nullptr, on ? tr(L"녹화 중") : L"");
}

void App::RememberPlacement() {
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!hwnd_ || !GetWindowPlacement(hwnd_, &wp)) return;
    // rcNormalPosition is in workspace coordinates: relative to the primary monitor's work area.
    MONITORINFO pm{sizeof(pm)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &pm);
    RECT r = wp.rcNormalPosition;
    OffsetRect(&r, pm.rcWork.left, pm.rcWork.top);
    settings_.window = r;
    settings_.windowMax = IsZoomed(hwnd_) || (IsIconic(hwnd_) && (wp.flags & WPF_RESTORETOMAXIMIZED));
}

void App::Toast(std::wstring text, bool error) {
    toast_ = std::move(text);
    toastError_ = error;
    toastSticky_ = false;
    toastTab_ = -1;
    // Long enough to read: longer messages stay longer.
    toastUntil_ = util::NowSeconds() + std::min(12.0, (error ? 5.0 : 3.0) + toast_.size() / 25.0);
    Invalidate();
}

void App::Alert(std::wstring text, int settingsTab) {
    toast_ = std::move(text);
    toastError_ = toastSticky_ = true;
    toastTab_ = settingsTab;
    toastUntil_ = std::numeric_limits<double>::infinity();
    Invalidate();
}

LRESULT CALLBACK App::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd_ = h;
    }
    auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->Handle(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT App::Handle(UINT m, WPARAM w, LPARAM l) {
    const float s = Scale();
    if (m == taskbarCreated_ && taskbarCreated_) {
        TrayAdd();
        return 0;
    }
    switch (m) {
        case WM_MOUSEMOVE:
            input_.mx = GET_X_LPARAM(l) / s;
            input_.my = GET_Y_LPARAM(l) / s;
            if (!tracking_) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
                TrackMouseEvent(&tme);
                tracking_ = true;
            }
            Invalidate();
            return 0;
        case WM_MOUSELEAVE:
            tracking_ = false;
            if (!input_.down) input_.mx = input_.my = -1e6f;
            Invalidate();
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            SetCapture(hwnd_);
            input_.mx = GET_X_LPARAM(l) / s;
            input_.my = GET_Y_LPARAM(l) / s;
            input_.down = true;
            input_.pressed = true;
            if (m == WM_LBUTTONDBLCLK) input_.dblclick = true;
            Invalidate();
            return 0;
        case WM_LBUTTONUP:
            ReleaseCapture();
            input_.mx = GET_X_LPARAM(l) / s;
            input_.my = GET_Y_LPARAM(l) / s;
            input_.down = false;
            input_.released = true;
            Invalidate();
            return 0;
        case WM_CAPTURECHANGED:
            if (input_.down) {
                input_.down = false;
                input_.released = true;
                Invalidate();
            }
            return 0;
        case WM_RBUTTONDOWN:
            input_.rpressed = true;
            Invalidate();
            return 0;
        case WM_MOUSEWHEEL:
            input_.wheel += GET_WHEEL_DELTA_WPARAM(w) / (float)WHEEL_DELTA;
            Invalidate();
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (capturingHotkey_) {
                // The keyboard hook takes the combinations first (KeyHook): this is what it let by, or all of it
                // when the hook couldn't start.
                UINT vk = (UINT)w;
                if (vk == VK_ESCAPE) {
                    SetCapturingHotkey(0);
                } else if (vk != VK_CONTROL && vk != VK_MENU && vk != VK_SHIFT && vk != VK_LWIN && vk != VK_RWIN) {
                    UINT mods = 0;
                    if (GetKeyState(VK_CONTROL) < 0) mods |= MOD_CONTROL;
                    if (GetKeyState(VK_MENU) < 0) mods |= MOD_ALT;
                    if (GetKeyState(VK_SHIFT) < 0) mods |= MOD_SHIFT;
                    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) mods |= MOD_WIN;
                    CaptureHotkey(mods, vk);
                }
                Invalidate();
                return 0;
            }
            input_.keys.push_back((UINT)w);
            input_.ctrl = GetKeyState(VK_CONTROL) < 0;
            input_.shift = GetKeyState(VK_SHIFT) < 0;
            input_.alt = GetKeyState(VK_MENU) < 0;
            Invalidate();
            if (m == WM_SYSKEYDOWN) break;
            return 0;
        case WM_CHAR:
            if (w >= 0x20 && w != 0x7F) {
                input_.chars.push_back((wchar_t)w);
                Invalidate();
            }
            return 0;
        // Text fields show what the IME is composing themselves, at their caret (Ui::TextField).
        case WM_IME_SETCONTEXT:
            l &= ~ISC_SHOWUICOMPOSITIONWINDOW;
            break;
        case WM_IME_STARTCOMPOSITION:
            return 0;
        case WM_IME_COMPOSITION:
            if (HIMC imc = ImmGetContext(hwnd_)) {
                auto read = [&](DWORD what) {
                    LONG n = ImmGetCompositionStringW(imc, what, nullptr, 0);
                    std::wstring s(n > 0 ? n / sizeof(wchar_t) : 0, 0);
                    if (n > 0) ImmGetCompositionStringW(imc, what, s.data(), n);
                    return s;
                };
                if (l & GCS_RESULTSTR) input_.chars += read(GCS_RESULTSTR);
                input_.composing = l & GCS_COMPSTR ? read(GCS_COMPSTR) : std::wstring();
                ImmReleaseContext(hwnd_, imc);
            }
            Invalidate();
            return 0;
        case WM_IME_ENDCOMPOSITION:
            input_.composing.clear();
            Invalidate();
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, ui_.Cursor()));
                return TRUE;
            }
            break;
        case WM_SIZE:
            gfx_.Resize();
            if (w != SIZE_MINIMIZED)
                Render();
            else
                SyncAudioPreview();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
            mmi->ptMinTrackSize = {(LONG)(kMinW * s), (LONG)(kMinH * s)};
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(w);
            auto* r = reinterpret_cast<RECT*>(l);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            icons_.clear();
            Invalidate();
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            EndPaint(hwnd_, &ps);
            Render();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_ACTIVATE:
            if (LOWORD(w) != WA_INACTIVE && (screen_ == Screen::Home || screen_ == Screen::Replay)) RefreshWindows();
            if (LOWORD(w) == WA_INACTIVE) SetCapturingHotkey(0);  // keys go elsewhere now; keep the old one
            Invalidate();
            break;
        case WM_HOTKEY:
            if (capturingHotkey_) return 0;
            if (w == kHotkeyRecord)
                OnHotkey();
            else if (w == kHotkeyPause)
                TogglePause();
            else if (w == kHotkeyReplay)
                ReplaySave();
            return 0;
        case WM_DISPLAYCHANGE:
            monitors_ = EnumerateMonitors();
            Invalidate();
            break;
        case WM_SETTINGCHANGE:  // light or dark mode ("ImmersiveColorSet"), the contrast theme, animations
        case WM_SYSCOLORCHANGE:
        case WM_THEMECHANGED:
            ApplyTheme();
            break;
        case WM_TIMER:
            if (w == kTimerRec) {
                if (rec_ == RecState::Recording) {
                    // A window recorded through the monitor (fullscreen game) never closes its capture item.
                    for (size_t i = 0; i < recs_.size() && rec_ == RecState::Recording; ++i) {
                        const RecSession& s = recs_[i];
                        if (!s.ended && s.capture &&
                            ((s.target.source == kSourceWindow && !IsWindow(s.target.hwnd)) || s.capture->StoreFailed()))
                            EndRecSession(i);
                    }
                    RecSession* lead = rec_ == RecState::Recording ? RecLead() : nullptr;
                    if (!lead) return 0;
                    overlay_.Update(lead->capture->Elapsed(), lead->capture->Frames());
                    if (IsWindowVisible(hwnd_)) Invalidate();
                    CheckLimits();
                }
            } else if (w == kTimerCountdown) {
                if (rec_ == RecState::Countdown) {
                    if (--countdownLeft_ <= 0) {
                        BeginCapture();
                    } else {
                        overlay_.ShowCountdown(recTarget_.rect, countdownLeft_);
                        if (settings_.sounds) sound::Play(sound::Cue::Tick);
                    }
                }
            } else if (w == kTimerMicTest) {
                // Only while 설정 › 소리 is in front, and 10 s at most.
                double now = util::NowSeconds();
                if (screen_ != Screen::Settings || settings_.settingsTab != kTabAudio || !IsWindowVisible(hwnd_) ||
                    IsIconic(hwnd_) || rec_ != RecState::Idle || now > micTestUntil_) {
                    StopMicTest();
                } else {
                    micMonitor_.SetGain(settings_.micVolume / 100.f);
                    float peak = std::min(1.f, micPeak_.exchange(0) * settings_.micVolume / 100.f);
                    float pos = peak > 0 ? std::clamp((20 * std::log10(peak) + 60) / 60, 0.f, 1.f) : 0.f;
                    micLevel_ = std::max(pos, micLevel_ - (float)(now - micLevelAt_) * 1.2f);
                    micLevelAt_ = now;
                    Invalidate();
                }
            } else if (w == kTimerReplay) {
                ReplayTick();
            } else if (w == kTimerAnimLoad) {
                if (animLoad_ && !animLoad_->done) ShowAnimProgress();
            } else if (w == kTimerUpdate) {
                SetTimer(hwnd_, kTimerUpdate, 3600 * 1000, nullptr);  // from now on hourly: a check is due daily
                CheckForUpdate();
            } else if (w == kTimerRefresh) {
                // Otherwise Home refreshes when it is shown or activated again.
                KillTimer(hwnd_, kTimerRefresh);
                windowsDirty_ = false;
                if ((screen_ == Screen::Home || screen_ == Screen::Replay) && IsWindowVisible(hwnd_) && !IsIconic(hwnd_) &&
                    !picker_.Active() &&
                    !regionPicker_.Active())
                    RefreshWindows();
            }
            return 0;
        case WM_APP_TRAY:
            switch (LOWORD(l)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                case NIN_BALLOONUSERCLICK:
                    ShowMain(true);
                    break;
                case WM_CONTEXTMENU:
                    TrayMenu();
                    break;
            }
            return 0;
        case WM_APP_SHOW:
            ShowMain(true);
            return 0;
        case WM_COPYDATA: {
            auto* cd = reinterpret_cast<const COPYDATASTRUCT*>(l);
            if (!cd || cd->dwData != kCopyOpenFile || cd->cbData < sizeof(wchar_t)) return FALSE;
            pendingOpen_.assign((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t) - 1);
            PostMessageW(hwnd_, WM_APP_OPEN, 0, 0);
            return TRUE;
        }
        case WM_APP_OPEN:
            if (!pendingOpen_.empty()) OpenMediaFile(std::exchange(pendingOpen_, {}));
            return 0;
        case WM_DROPFILES: {
            auto drop = (HDROP)w;
            wchar_t path[MAX_PATH * 4];
            bool got = DragQueryFileW(drop, 0, path, _countof(path)) > 0;
            DragFinish(drop);
            if (got) OpenMediaFile(path);
            return 0;
        }
        case WM_APP_QUIT:
            quitting_ = true;
            DestroyWindow(hwnd_);
            return 0;
        case WM_APP_ANSWER:
            if (auto f = std::exchange(answer_, nullptr)) f();
            return 0;
        case WM_APP_HOOKED_KEY:
            if (capturingHotkey_) CaptureHotkey((UINT)l, (UINT)w);
            return 0;
        case WM_APP_CAPTURE_CLOSED:
            // lParam: which of the recording's captures (1-based); its window went away or its encoder failed.
            if (rec_ == RecState::Recording) {
                if (l > 0 && (size_t)l <= recs_.size())
                    EndRecSession((size_t)l - 1);
                else
                    StopRecording();
            }
            return 0;
        case WM_APP_EXPORT_DONE:
            OnExportDone();
            return 0;
        case WM_APP_ANIM_LOADED:
            OnAnimLoaded();
            return 0;
        case WM_APP_REPLAY_CLOSED:
            OnReplayClosed(w != 0);
            return 0;
        case WM_APP_REPLAY_SAVED:
            OnReplaySaved();
            return 0;
        case WM_WTSSESSION_CHANGE:
            if (w == WTS_SESSION_LOCK || w == WTS_SESSION_UNLOCK) {
                rpLocked_ = w == WTS_SESSION_LOCK;
                if (rpLocked_)
                    ReplayClear();
                else
                    ReplayTick();
            }
            return 0;
        case WM_POWERBROADCAST:
            if (w == PBT_APMSUSPEND) {
                rpAsleep_ = true;
                ReplayClear();
            } else if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND) {
                rpAsleep_ = false;
                ReplayTick();
            }
            return TRUE;
        case WM_APP_ESTIMATE:
            OnEstimate();
            return 0;
        case WM_APP_UPDATE:
            if (!UpdateReady() && updater_.Get().state != Updater::State::Available) updateOpen_ = false;
            TrayUpdate();
            Invalidate();
            return 0;
        case WM_APP_FRAME: {
            size_t idx = 0;
            if (clip_ && fetcher_.Take(framePx_, idx) &&
                framePx_.size() == (size_t)clip_->Width() * clip_->Height() * 4) {
                UploadFrame(framePx_.data());
                shown_ = idx;
                Invalidate();
            }
            return 0;
        }
        case WM_APP_THUMBS: {
            std::lock_guard lk(thumbMtx_);
            thumbs_.clear();
            for (auto& px : thumbPixels_) thumbs_.push_back(gfx_.CreateBitmap(thumbW_, thumbH_, px.data(), thumbW_ * 4));
            Invalidate();
            return 0;
        }
        case WM_CLOSE:
            if (!settings_.closeToTray && settings_.rpOn && !quitting_) {
                // 순간 리플레이 goes on in the tray: closing the window doesn't take what it holds.
                HideMain();
                if (!std::exchange(rpTrayHint_, true))
                    TrayBalloon(tr(L"순간 리플레이는 트레이에서 계속 담습니다"),
                                tr(L"끝내려면 트레이 아이콘을 오른쪽 클릭하고 종료를 누르세요."));
                return 0;
            }
            if (!settings_.closeToTray && !quitting_) {
                AskQuit(false, [this] { PostMessageW(hwnd_, WM_APP_QUIT, 0, 0); });
                return 0;
            }
            if (settings_.closeToTray && !quitting_) {
                HideMain();
                if (!settings_.trayHintShown) {
                    settings_.trayHintShown = true;
                    settings_.Save();
                    TrayBalloon(tr(L"OzMaker는 트레이에서 계속 실행됩니다"),
                                util::HotkeyToString(settings_.hkMods, settings_.hkVk) +
                                    tr(L" 로 언제든 녹화를 시작할 수 있습니다."));
                }
                return 0;
            }
            DestroyWindow(hwnd_);
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            // Logoff or shutdown ends the process without WM_DESTROY: finish the MP4 being recorded now.
            if (w && rec_ == RecState::Recording) {
                KillTimer(hwnd_, kTimerRec);
                for (RecSession& s : recs_)
                    if (s.capture) s.capture->Stop();
                recs_.clear();
                rec_ = RecState::Idle;
            }
            if (w) {
                KillTimer(hwnd_, kTimerReplay);
                if (rpSaveThread_.joinable()) rpSaveThread_.join();  // a replay being saved is finished
                rpCap_.Stop();
            }
            if (w) {
                RememberPlacement();
                settings_.Save();
            }
            return 0;
        case WM_GETOBJECT:
            if (static_cast<long>(l) == static_cast<long>(UiaRootObjectId) && access_.Root())
                return access_.OnGetObject(w, l);
            break;
        case WM_DESTROY:
            access_.Detach();
            keyHook_.Stop();
            for (RecSession& s : recs_)
                if (s.capture) s.capture->Stop();
            recs_.clear();
            KillTimer(hwnd_, kTimerReplay);
            if (rpSaveThread_.joinable()) rpSaveThread_.join();
            rpCap_.Stop();
            rpRing_.Clear();
            rpNotice_.Hide();
            WTSUnRegisterSessionNotification(hwnd_);
            RememberPlacement();
            overlay_.Hide();
            exporter_.Cancel();
            animLoad_.reset();  // stops and waits for it
            StopThumbnails();
            audioPreview_.Stop();
            fetcher_.Stop();
            ReleaseThumbnails();
            WatchWindows(false);
            StopMicTest();
            UnregisterHotKey(hwnd_, kHotkeyRecord);
            UnregisterHotKey(hwnd_, kHotkeyPause);
            UnregisterHotKey(hwnd_, kHotkeyReplay);
            if (dropTarget_) {
                RevokeDragDrop(hwnd_);
                dropTarget_.Reset();
                OleUninitialize();
            }
            TrayRemove();
            settings_.Save();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

// ---------------------------------------------------------------- sources

void App::RefreshWindows() {
    windows_ = EnumerateCaptureWindows();
    if (testWindowsOnly_)
        std::erase_if(windows_, [](const WinInfo& w) { return w.title.find(L"OzMaker Test Window") == std::wstring::npos; });
    if (target_ && !IsWindow(target_)) target_ = nullptr;
    std::erase_if(picks_, [](HWND h) { return !IsWindow(h); });
    if (MultiPicking()) target_ = picks_.empty() ? nullptr : picks_[0];
    for (auto it = icons_.begin(); it != icons_.end();) {
        if (!IsWindow(it->first))
            it = icons_.erase(it);
        else
            ++it;
    }
    monitors_ = EnumerateMonitors();
    Invalidate();
}

namespace {
App* g_watcher = nullptr;  // the App whose window list the WinEvent hooks keep current
}

// Other programs' windows appearing, closing, renaming, moving or minimizing refresh the window list, instead of
// polling it. Out of context: the callbacks come in through this thread's message loop.
void App::WatchWindows(bool on) {
    for (HWINEVENTHOOK h : winHooks_) UnhookWinEvent(h);
    winHooks_.clear();
    g_watcher = on ? this : nullptr;
    if (!on) return;
    const std::pair<DWORD, DWORD> ranges[] = {
        {EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE},          // destroy, show, hide
        {EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE},  // title
        {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED},      // other virtual desktops, suspended UWP apps
        {EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND},
        {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND},
    };
    for (auto [from, to] : ranges)
        if (HWINEVENTHOOK h = SetWinEventHook(from, to, nullptr, OnWinEvent, 0, 0,
                                              WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS))
            winHooks_.push_back(h);
}

void CALLBACK App::OnWinEvent(HWINEVENTHOOK, DWORD event, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    App* self = g_watcher;
    if (!self || !h || obj != OBJID_WINDOW || child != CHILDID_SELF) return;
    // A top-level window that may join the list, or one already in it.
    bool appears = event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_UNCLOAKED;
    if (appears ? GetAncestor(h, GA_ROOT) != h : !self->FindWindowInfo(h)) return;
    if (self->windowsDirty_) return;
    self->windowsDirty_ = true;
    SetTimer(self->hwnd_, kTimerRefresh, 400, nullptr);
}

void App::SelectTarget(HWND h) {
    target_ = h;
    Invalidate();
}

// The switch over the window list. On, the window picked is the first one checked; off, the first one checked is
// the window picked.
void App::SetMultiWindow(bool on) {
    if (rec_ != RecState::Idle || settings_.multiWindow == on) return;
    settings_.multiWindow = on;
    settingsDirty_ = true;
    if (on) {
        picks_.clear();
        if (target_ && IsWindow(target_)) picks_.push_back(target_);
    } else if (!picks_.empty()) {
        target_ = picks_[0];
    }
    Invalidate();
}

void App::TogglePick(HWND h) {
    if (rec_ != RecState::Idle || !h) return;
    if (auto it = std::ranges::find(picks_, h); it != picks_.end()) {
        picks_.erase(it);
    } else if (picks_.size() >= kMaxRecWindows) {
        Toast(util::Format(tr(L"한 번에 %zu개까지 녹화합니다. 다른 창의 체크를 풀어 주세요."), kMaxRecWindows), true);
        return;
    } else {
        picks_.push_back(h);
    }
    target_ = picks_.empty() ? nullptr : picks_[0];
    Invalidate();
}

std::vector<RecTarget> App::PickedTargets() const {
    std::vector<RecTarget> ts;
    for (HWND h : picks_)
        if (IsWindow(h)) ts.push_back(WindowTarget(h));
    return ts;
}

const WinInfo* App::FindWindowInfo(HWND h) const {
    for (auto& w : windows_)
        if (w.hwnd == h) return &w;
    return nullptr;
}

ID2D1Bitmap* App::IconFor(const WinInfo& w) {
    auto it = icons_.find(w.hwnd);
    if (it != icons_.end()) return it->second.Get();
    ComPtr<ID2D1Bitmap1> bmp;
    if (auto src = GetWindowIconBitmap(gfx_.Wic(), w, (int)std::lround(32 * Scale()))) bmp = gfx_.CreateBitmapFromWic(src.Get());
    icons_[w.hwnd] = bmp;
    return bmp.Get();
}

void App::SetSource(int source) {
    settings_.source = source;
    settingsDirty_ = true;
    if (source == kSourceDisplay) monitors_ = EnumerateMonitors();
    Invalidate();
}

// The picker opens on the area picked before; its toolbar records the area at once or keeps it for Home.
void App::StartRegionPick() {
    if (rec_ != RecState::Idle || regionPicker_.Active()) return;
    HideMain();
    RECT last = settings_.HasRegion() ? settings_.region : RECT{};
    regionPicker_.Start(EnumerateCaptureWindows(), last, settings_.countdown, [this](PickResult res, RECT r) {
        // The toolbar's countdown is the setting in 설정 › 녹화.
        bool save = regionPicker_.Countdown() != settings_.countdown;
        settings_.countdown = regionPicker_.Countdown();
        if (res != PickResult::Cancel) {
            settings_.region = r;
            settings_.source = kSourceRegion;
            save = true;
            TakeRegionSnapshot();  // before our window comes back over it
        }
        if (save) settings_.Save();
        if (res == PickResult::Record) {
            RecTarget t;
            std::wstring why;
            if (CurrentTarget(t, &why)) {
                // Hidden for the picking, not by the setting: 작업 표시줄로 최소화 holds for this start too.
                if (settings_.startHide == kHideMinimize) ShowWindow(hwnd_, SW_SHOWMINNOACTIVE);
                StartRecording(t, false);  // Home stays hidden; the countdown starts over the area
                return;
            }
            ShowMain(true);
            Toast(why, true);
            return;
        }
        ShowMain(true);
    });
}

void App::TakeRegionSnapshot() {
    snapBmp_.Reset();
    RECT r = settings_.region;
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w < 2 || h < 2) return;
    double f = std::min({1.0, 1280.0 / w, 720.0 / h});
    int tw = std::max(1, (int)std::lround(w * f)), th = std::max(1, (int)std::lround(h * f));
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = tw;
    bi.bmiHeader.biHeight = -th;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC mem = CreateCompatibleDC(screen);
    if (dib && mem) {
        HGDIOBJ old = SelectObject(mem, dib);
        SetStretchBltMode(mem, HALFTONE);
        SetBrushOrgEx(mem, 0, 0, nullptr);
        StretchBlt(mem, 0, 0, tw, th, screen, r.left, r.top, w, h, SRCCOPY);
        GdiFlush();
        auto* px = static_cast<uint32_t*>(bits);
        for (size_t i = 0, n = (size_t)tw * th; i < n; ++i) px[i] |= 0xFF000000u;
        snapBmp_ = gfx_.CreateBitmap(tw, th, static_cast<const uint8_t*>(bits), tw * 4);
        snapRect_ = r;
        SelectObject(mem, old);
    }
    if (mem) DeleteDC(mem);
    if (dib) DeleteObject(dib);
    ReleaseDC(nullptr, screen);
}

RecTarget App::WindowTarget(HWND h) const {
    RecTarget t;
    t.source = kSourceWindow;
    t.hwnd = h;
    if (!GetVisibleFrame(h, t.rect)) GetWindowRect(h, &t.rect);
    return t;
}

RecTarget App::MonitorTarget(HMONITOR m) const {
    RecTarget t;
    t.source = kSourceDisplay;
    t.monitor = m;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(m, &mi);
    t.rect = mi.rcMonitor;
    return t;
}

bool App::CurrentTarget(RecTarget& t, std::wstring* why) const {
    auto fail = [&](const wchar_t* msg) {
        if (why) *why = msg;
        return false;
    };
    switch (settings_.source) {
        case kSourceDisplay: {
            const MonitorDesc* m = FindMonitor(monitors_, settings_.monitor);
            if (!m) return fail(tr(L"녹화할 화면을 찾을 수 없습니다."));
            t = MonitorTarget(m->handle);
            return true;
        }
        case kSourceRegion: {
            if (!settings_.HasRegion()) return fail(tr(L"녹화할 영역을 먼저 지정하세요."));
            RECT r = settings_.region, clip{};
            HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
            MONITORINFO mi{sizeof(mi)};
            if (!mon || !GetMonitorInfoW(mon, &mi) || !IntersectRect(&clip, &r, &mi.rcMonitor) ||
                clip.right - clip.left < 16 || clip.bottom - clip.top < 16)
                return fail(tr(L"지정한 영역이 화면 밖에 있습니다. 영역을 다시 지정하세요."));
            t = {};
            t.source = kSourceRegion;
            t.monitor = mon;
            t.rect = clip;
            return true;
        }
        default:
            if (!target_ || !IsWindow(target_)) return fail(tr(L"녹화할 창을 먼저 선택하세요."));
            t = WindowTarget(target_);
            return true;
    }
}

void App::SourceSize(const RecTarget& t, int& w, int& h) const {
    w = t.rect.right - t.rect.left;
    h = t.rect.bottom - t.rect.top;
    if (t.source == kSourceWindow && settings_.clientOnly) {
        WINDOWINFO wi{sizeof(wi)};
        if (GetWindowInfo(t.hwnd, &wi)) {
            w = wi.rcClient.right - wi.rcClient.left;
            h = wi.rcClient.bottom - wi.rcClient.top;
        }
    }
    if (sourceForTest_.cx > 0) w = sourceForTest_.cx, h = sourceForTest_.cy;
}

void App::PlannedSize(int sw, int sh, int& ow, int& oh, bool* hevc) const {
    // Mirrors Capture::Setup, with the encoder's limit (BeginCapture's maxEdge) last: the codec goes by the size the
    // 해상도 box leaves.
    double f = 1.0;
    int longE = std::max(1, std::max(sw, sh)), shortE = std::max(1, std::min(sw, sh));
    bool useHevc = false;
    if (settings_.output == kOutputVideo) {
        if (int res = kVideoResOptions[settings_.vResIdx]) {
            f = std::min(f, res * 16.0 / 9.0 / longE);
            f = std::min(f, (double)res / shortE);
        }
        useHevc = UseHevc((int)std::lround(longE * f));
        f = std::min(f, (useHevc ? 8192.0 : 4096.0) / longE);
    } else if (int maxE = kSizeOptions[settings_.sizeIdx]) {
        f = std::min(f, (double)maxE / longE);
    }
    ow = std::max(2, (int)std::lround(sw * f) & ~1);
    oh = std::max(2, (int)std::lround(sh * f) & ~1);
    if (hevc) *hevc = useHevc;
}

// ---------------------------------------------------------------- recording

bool App::UseMonitorCapture(HWND target) const {
    if (settings_.captureMode == kCaptureDisplay) return true;
    if (settings_.captureMode == kCaptureWindow) return false;
    return IsFullscreenWindow(target);  // fullscreen games capture most reliably through the display
}

// After a hotkey start with a fullscreen app in front, don't yank the user out of it: use the tray instead.
bool App::NotifyQuietly() const {
    HWND fg = GetForegroundWindow();
    return recFromHotkey_ && fg && fg != hwnd_ && IsFullscreenWindow(fg);
}

void App::ReportError(const std::wstring& title, const std::wstring& text, int settingsTab) {
    if (NotifyQuietly()) {
        TrayBalloon(title, text);
    } else {
        ShowMain(true);
        Alert(text, settingsTab);
    }
}

void App::StartMicTest() {
    StopMicTest();
    micPeak_ = 0;
    micLevel_ = 0;
    std::wstring err;
    auto sink = [this](const int16_t* pcm, size_t frames) {
        int peak = 0;
        for (size_t i = 0; i < frames * 2; ++i) peak = std::max(peak, std::abs((int)pcm[i]));
        float v = peak / 32768.f, old = micPeak_.load();
        while (v > old && !micPeak_.compare_exchange_weak(old, v)) {
        }
        micMonitor_.Push(pcm, frames);  // dropped unless it plays
    };
    if (!micTest_.Start(AudioCapture::Source::Mic, 0, 48000, util::Qpc100ns(), sink, err, settings_.micDevice)) {
        Toast(err, true);
        return;
    }
    SyncMicMonitor();
    micTestUntil_ = util::NowSeconds() + 10;
    micLevelAt_ = util::NowSeconds();
    SetTimer(hwnd_, kTimerMicTest, 33, nullptr);
    Invalidate();
}

void App::StopMicTest() {
    if (!micTest_.Running()) return;
    KillTimer(hwnd_, kTimerMicTest);
    micTest_.Stop(0);
    micMonitor_.Stop();
    micLevel_ = 0;
    Invalidate();
}

void App::SyncMicMonitor() {
    bool want = micTest_.Running() && settings_.micMonitor;
    micMonitor_.SetGain(settings_.micVolume / 100.f);  // what the recording would get
    if (want == micMonitor_.Running()) return;
    std::wstring err;
    if (!want)
        micMonitor_.Stop();
    else if (!micMonitor_.Start(48000, err))
        Toast(err, true);
}

void App::StartRecording(const RecTarget& target, bool fromHotkey) {
    // 여러 창 녹화 (#48): what is selected is every checked window, the first of which is target_.
    if (MultiPicking() && target.source == kSourceWindow && target.hwnd == target_ && picks_.size() > 1) {
        StartRecording(PickedTargets(), fromHotkey);
        return;
    }
    StartRecording(std::vector<RecTarget>{target}, fromHotkey);
}

void App::StartRecording(const std::vector<RecTarget>& targets, bool fromHotkey) {
    if (rec_ != RecState::Idle || targets.empty()) return;
    StopMicTest();      // the recording opens the mic itself
    question_.reset();  // a question about this window's state (quit, discard) no longer applies
    recFromHotkey_ = fromHotkey;
    if (exporter_.Busy()) {
        if (fromHotkey)
            TrayBalloon(tr(L"녹화를 시작할 수 없습니다"), tr(L"내보내기가 끝난 뒤에 녹화할 수 있습니다."));
        else
            Toast(tr(L"내보내기가 끝난 뒤에 녹화할 수 있습니다."), true);
        return;
    }
    recs_.clear();
    for (const RecTarget& target : targets) {
        if (target.source == kSourceWindow && (!target.hwnd || !IsWindow(target.hwnd))) continue;
        RecSession s;
        s.target = target;
        if (target.source == kSourceDisplay) {
            s.name = tr(L"화면");
            for (auto& m : monitors_)
                if (m.handle == target.monitor) s.name = util::Format(tr(L"화면 %d"), m.number);
        } else if (target.source == kSourceRegion) {
            s.name = tr(L"영역");
        } else {
            int len = GetWindowTextLengthW(target.hwnd);
            s.name.assign(len + 1, L'\0');
            s.name.resize(GetWindowTextW(target.hwnd, s.name.data(), len + 1));
            if (s.name.empty()) s.name = tr(L"창");
        }
        recs_.push_back(std::move(s));
    }
    if (recs_.empty()) {
        ShowMain(true);
        Toast(tr(L"녹화할 창을 먼저 선택하세요."), true);
        return;
    }
    const RecTarget& lead = recs_[0].target;
    recVideo_ = settings_.output == kOutputVideo;
    if (lead.source == kSourceWindow) {
        for (RecSession& s : recs_)
            if (IsIconic(s.target.hwnd)) {
                ShowWindow(s.target.hwnd, SW_RESTORE);
                s.target = WindowTarget(s.target.hwnd);
            }
        recFullscreen_ = IsFullscreenWindow(lead.hwnd);
    } else {
        HWND fg = GetForegroundWindow();
        recFullscreen_ = fg && fg != hwnd_ && IsFullscreenWindow(fg);
    }
    recTarget_ = lead;
    recName_ = recs_[0].name;
    if (!fromHotkey) {
        if (settings_.startHide == kHideMinimize)
            MinimizeMain();
        else
            HideMain();
        if (lead.source == kSourceWindow) SetForegroundWindow(lead.hwnd);
    }
    int cd = fromHotkey ? 0 : settings_.CountdownSeconds();
    if (cd > 0) {
        rec_ = RecState::Countdown;
        countdownLeft_ = cd;
        overlay_.ShowCountdown(recTarget_.rect, cd);
        if (settings_.sounds) sound::Play(sound::Cue::Tick);
        SetTimer(hwnd_, kTimerCountdown, 1000, nullptr);
        TrayUpdate();
        return;
    }
    BeginCapture();
}

std::wstring App::RecFileStem(const std::wstring& name, bool several) const {
    std::wstring stem = FileStem(name);
    const std::wstring& p = settings_.nameTemplate;
    if (several && p.find(kNameTokens[kTokWindow][0]) == std::wstring::npos &&
        p.find(kNameTokens[kTokWindow][1]) == std::wstring::npos) {
        std::wstring what = util::SafeFileName(name, 60);
        stem = util::SafeFileName(stem + L"_" + (what.empty() ? std::wstring(L"OzMaker") : what), 120);
    }
    return stem;
}

void App::BeginCapture() {
    KillTimer(hwnd_, kTimerCountdown);
    const bool window = recTarget_.source == kSourceWindow;
    const size_t asked = recs_.size();
    // A window closed during the countdown is left out; with none left there is nothing to record.
    std::erase_if(recs_, [](const RecSession& s) { return s.target.source == kSourceWindow && !IsWindow(s.target.hwnd); });
    if (recs_.empty()) {
        rec_ = RecState::Idle;
        overlay_.Hide();
        ShowMain(true);
        Alert(asked > 1 ? tr(L"녹화할 창이 모두 닫혔습니다.") : tr(L"녹화할 창이 닫혔습니다."));
        TrayUpdate();
        return;
    }
    const bool several = recs_.size() > 1;
    for (RecSession& s : recs_)
        if (window) s.target.rect = WindowTarget(s.target.hwnd).rect;
    recTarget_ = recs_[0].target;
    recName_ = recs_[0].name;

    recNotice_.clear();
    std::vector<std::wstring> paths;  // taken by the ones before, whose files don't exist yet
    auto taken = [&](const std::wstring& path) {
        return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES ||
               std::ranges::any_of(paths, [&](const std::wstring& q) { return _wcsicmp(q.c_str(), path.c_str()) == 0; });
    };
    // Each thing recorded: its own capture, and (MP4) its own file and its program's sound.
    auto optionsFor = [&](const RecSession& s) {
        const RecTarget& t = s.target;
        CaptureOptions o;
        o.source = window ? CaptureSource::Window : t.source == kSourceDisplay ? CaptureSource::Monitor : CaptureSource::Region;
        o.target = t.hwnd;
        // Several windows go through their windows always: the screen would take whatever covers each.
        o.monitorMode = window && !several && UseMonitorCapture(t.hwnd);
        o.monitor = t.monitor;
        o.region = t.rect;
        o.clientOnly = settings_.clientOnly;
        o.cursor = settings_.cursor;
        o.clickFx = settings_.clickFx;
        if (recVideo_) {
            VideoOptions v;
            std::wstring base = settings_.outDir + L"\\" + RecFileStem(s.name, several);
            v.path = base + L".mp4";
            for (int i = 2; taken(v.path); ++i) v.path = base + util::Format(L"_%d.mp4", i);
            paths.push_back(v.path);
            o.fps = kVideoFpsOptions[settings_.vFpsIdx];
            v.video = VideoSettings(o.fps);
            int sw = 0, sh = 0, ow = 0, oh = 0;
            SourceSize(t, sw, sh);
            PlannedSize(sw, sh, ow, oh, &v.video.hevc);  // 자동: HEVC past 4096 (#42)
            v.sound = settings_.audio;
            v.mic = settings_.mic;
            v.micDevice = settings_.micDevice;
            v.micGain = settings_.micVolume / 100.f;
            v.audio.sampleRate = kAudioRateOptions[settings_.aRateIdx];
            v.audio.bitrate = kAudioBitrateOptions[settings_.aBitrateIdx] * 1000;
            // A window records only its own sound; screens and regions record what the speakers play.
            v.processAudio = window;
            if (window) {
                v.audioPid = AudioCapture::AudioProcessOf(t.hwnd);
                if (v.sound && !AudioCapture::ProcessLoopbackSupported()) {
                    v.sound = false;
                    recNotice_ = v.mic ? tr(L"이 Windows 버전에서는 창 소리만 따로 녹음할 수 없어 마이크 소리만 녹음했습니다.")
                                       : tr(L"이 Windows 버전에서는 창 소리만 따로 녹음할 수 없어 소리 없이 녹화했습니다.");
                }
            }
            v.audio.enabled = v.sound || v.mic;
            o.maxEdge = v.video.hevc ? 8192 : 4096;  // encoder limits
            if (int res = kVideoResOptions[settings_.vResIdx]) {
                o.boxShort = res;
                o.boxLong = res * 16 / 9;
            }
            o.video = std::move(v);
        } else {
            o.fps = kFpsOptions[settings_.fpsIdx];
            o.maxEdge = kSizeOptions[settings_.sizeIdx];
        }
        return o;
    };
    if (recVideo_) util::EnsureDir(settings_.outDir);
    std::vector<CaptureOptions> opts;
    for (const RecSession& s : recs_) opts.push_back(optionsFor(s));
    recScreen_ = !window || opts[0].monitorMode;
    // Free space (#17): too little to start, or a warning that the recording will stop itself.
    uint64_t freeBytes = util::FreeBytes(RecordingDir());
    if (freeBytes < kLowDiskStop) {
        rec_ = RecState::Idle;
        recs_.clear();
        overlay_.Hide();
        TrayUpdate();
        ReportError(tr(L"녹화를 시작하지 못했습니다"),
                    tr(L"저장할 드라이브에 남은 공간이 ") + util::FormatBytes(freeBytes) + tr(L"뿐입니다. 공간을 비운 뒤 녹화하세요."));
        return;
    }
    // 순간 리플레이 rests while recording, and starts over afterwards (ReplayTick).
    if (settings_.rpOn) ReplayStop(false);
    std::wstring err;
    for (size_t i = 0; i < recs_.size(); ++i) {
        auto cap = std::make_unique<Capture>();
        // The tag tells which of several stopped by itself (WM_APP_CAPTURE_CLOSED).
        if (cap->Start(opts[i], hwnd_, WM_APP_CAPTURE_CLOSED, err, (LPARAM)(i + 1))) {
            recs_[i].capture = std::move(cap);
            continue;
        }
        // One that won't start stops the rest: what they began goes.
        for (size_t k = 0; k < i; ++k) {
            CaptureResult r = recs_[k].capture->Stop();
            if (r.video && !r.path.empty()) DeleteFileW(r.path.c_str());
        }
        const std::wstring which = several ? recs_[i].name + L": " : L"";
        recs_.clear();
        rec_ = RecState::Idle;
        overlay_.Hide();
        TrayUpdate();
        // The MP4 encoder refusing to start is most often the graphics card's: the video settings hold the way out.
        if (recVideo_ && err.find(tr(L"인코더")) != std::wstring::npos)
            ReportError(tr(L"녹화를 시작하지 못했습니다"),
                        which + err + tr(L" 하드웨어 인코딩을 끄거나 해상도를 낮춰 보세요. 그래픽 드라이버를 업데이트하면 "
                                         L"해결되는 경우도 많습니다."),
                        kTabVideo);
        else
            ReportError(tr(L"녹화를 시작하지 못했습니다"), which + err);
        return;
    }
    rec_ = RecState::Recording;
    recPaused_ = false;
    stopReason_.clear();
    timerFiredFor_ = 0;
    nextDiskCheck_ = util::NowSeconds() + 2;
    SetRecordingLook(true);
    if (freeBytes < kLowDiskWarn)
        TrayBalloon(tr(L"남은 공간이 적습니다"), tr(L"저장할 드라이브에 ") + util::FormatBytes(freeBytes) +
                                                tr(L" 남았습니다. 300MB까지 줄면 녹화를 멈추고 저장합니다."));
    RegisterPauseHotkey();
    bool pill = settings_.overlayMode == kOverlayAlways ||
                (settings_.overlayMode == kOverlayExceptFullscreen && !recFullscreen_);
    if (pill)
        overlay_.ShowPill(recTarget_.rect, recTarget_.source == kSourceRegion,
                          [this] { PostMessageW(hwnd_, WM_HOTKEY, kHotkeyRecord, 0); },
                          [this] { PostMessageW(hwnd_, WM_HOTKEY, kHotkeyPause, 0); });
    else
        overlay_.Hide();
    overlay_.SetTimer(settings_.TimerMinutes() * 60, false);
    overlay_.SetCount((int)recs_.size());
    if (recTarget_.source == kSourceRegion && settings_.overlayMode != kOverlayNever) overlay_.ShowFrame(recTarget_.rect);
    if (settings_.sounds) sound::Play(sound::Cue::Start);
    SetTimer(hwnd_, kTimerRec, 100, nullptr);
    TrayUpdate();
    Invalidate();
}

RecSession* App::RecLead() {
    for (RecSession& s : recs_)
        if (!s.ended && s.capture) return &s;
    return nullptr;
}

double App::RecElapsed() const {
    if (recElapsedForTest_ >= 0) return recElapsedForTest_;
    for (const RecSession& s : recs_)
        if (!s.ended && s.capture) return s.capture->Elapsed();
    return 0;
}

void App::EndRecSession(size_t i) {
    if (rec_ != RecState::Recording || i >= recs_.size() || recs_[i].ended || !recs_[i].capture) return;
    size_t live = 0;
    for (const RecSession& s : recs_) live += !s.ended && s.capture;
    if (live <= 1) {  // the last one: the recording is over
        StopRecording();
        return;
    }
    RecSession& s = recs_[i];
    const bool lead = RecLead() == &s;
    s.result = s.capture->Stop();
    s.capture.reset();
    s.ended = true;
    overlay_.SetCount((int)live - 1);
    if (lead) {
        if (RecSession* next = RecLead()) {
            recTarget_ = next->target;
            if (IsWindow(recTarget_.hwnd)) recTarget_ = WindowTarget(recTarget_.hwnd);
            recName_ = next->name;
            overlay_.MoveTo(recTarget_.rect);
        }
    }
    TrayUpdate();
    Invalidate();
}

void App::StopRecording() {
    if (rec_ == RecState::Countdown) {
        KillTimer(hwnd_, kTimerCountdown);
        rec_ = RecState::Idle;
        recs_.clear();
        overlay_.Hide();
        TrayUpdate();
        ShowMain(true);
        return;
    }
    if (rec_ != RecState::Recording) return;
    KillTimer(hwnd_, kTimerRec);
    // Several end together: each holds still while the ones before it finish their files.
    if (recs_.size() > 1)
        for (RecSession& s : recs_)
            if (!s.ended && s.capture) s.capture->SetPaused(true);
    for (RecSession& s : recs_)
        if (!s.ended && s.capture) s.result = s.capture->Stop();
    std::vector<RecSession> done = std::move(recs_);
    recs_.clear();
    rec_ = RecState::Idle;
    recPaused_ = false;
    SetRecordingLook(false);
    RegisterPauseHotkey();  // releases it
    overlay_.Hide();
    TrayUpdate();
    if (done.size() > 1) {
        StopSeveral(done);
        return;
    }
    const CaptureResult& res = done[0].result;
    lastVideos_.clear();
    if (res.video) {
        OnVideoSaved(res);
        return;
    }
    if (settings_.sounds) sound::Play(sound::Cue::Stop);
    auto clip = res.clip;
    if (clip) HoldRecording(clip, done[0].name);
    clipName_ = done[0].name;
    if (!clip) {
        if (done[0].target.source == kSourceWindow)
            ReportError(tr(L"녹화 실패"), tr(L"캡처된 화면이 없습니다. 녹화 설정에서 창 캡처 방식을 '화면'으로 바꿔 보세요."),
                        kTabRecord);
        else
            ReportError(tr(L"녹화 실패"), tr(L"캡처된 화면이 없습니다."));
        return;
    }
    OpenClipWarn(clip, 1);
}

// The clip just recorded in the editor, and what went wrong while recording it. held: how many clips it made.
void App::OpenClipWarn(const std::shared_ptr<FrameStore>& clip, size_t held) {
    OpenEditor(clip);
    std::wstring warn;
    if (clip->WriteFailed())
        warn = tr(L"디스크 공간이 부족해 녹화를 멈췄습니다. 그 앞까지만 저장되어 있습니다.");
    else if (clip->Dropped() > 0)
        warn = util::Format(tr(L"디스크가 느려 프레임 %u개를 건너뛰었습니다."), clip->Dropped());
    if (!stopReason_.empty() && !clip->WriteFailed()) warn = stopReason_ + (warn.empty() ? L"" : L" " + warn);
    stopReason_.clear();
    const std::wstring done = held > 1 ? util::Format(tr(L"창 %zu개를 녹화했습니다. 첫 창을 편집 탭에서 열었고, 나머지는 "
                                                          L"‘저장하지 않은 녹화’에 있습니다"),
                                                       held)
                                       : tr(L"녹화를 편집 탭에서 열었습니다");
    if (NotifyQuietly()) {
        TrayBalloon(tr(L"녹화 완료 · ") + util::FormatClock(clip->Seconds(), true),
                    warn.empty() ? tr(L"클릭하면 편집 탭이 열립니다.") : warn);
    } else {
        ShowMain(true);
        Toast(warn.empty() ? done : warn, !warn.empty());
    }
}

// 여러 창 녹화: a file (MP4) or a held clip (움짤) for each window, the first one shown.
void App::StopSeveral(std::vector<RecSession>& done) {
    std::wstring failed;
    size_t failures = 0;
    if (done[0].result.video || recVideo_) {
        lastVideos_.clear();
        for (RecSession& s : done) {
            if (s.result.video && s.result.ok) {
                lastVideos_.push_back({s.result, s.ended});
            } else {
                ++failures;
                if (failed.empty())
                    failed = s.name + L": " + (s.result.error.empty() ? tr(L"동영상을 저장하지 못했습니다.") : s.result.error);
            }
        }
        if (lastVideos_.empty()) {
            if (settings_.sounds) sound::Play(sound::Cue::Stop);
            ReportError(tr(L"동영상 저장 실패"), failed);
            return;
        }
        OnVideosSaved();
        if (failures) Alert(util::Format(tr(L"창 %zu개는 저장하지 못했습니다. "), failures) + failed);
        return;
    }
    // 움짤: each into 저장하지 않은 녹화, the first window's at the top; the first opens.
    std::shared_ptr<FrameStore> first;
    size_t held = 0;
    for (size_t i = done.size(); i-- > 0;) {
        if (!done[i].result.clip) {
            ++failures;
            continue;
        }
        HoldRecording(done[i].result.clip, done[i].name);
        first = done[i].result.clip;
        clipName_ = done[i].name;
        ++held;
    }
    if (settings_.sounds) sound::Play(sound::Cue::Stop);
    if (!first) {
        ReportError(tr(L"녹화 실패"), tr(L"캡처된 화면이 없습니다."));
        return;
    }
    OpenClipWarn(first, held);
}

// The file is already saved: the card on the 녹화 tab shows it. Its 편집 opens it in the 편집 tab (EditLastVideo).
void App::ShowVideo(const CaptureResult& r) {
    AddRecent(r.path);
    lastVideo_ = r;
    showVideo_ = true;
    if (screen_ == Screen::Editor)
        CloseEditor();
    else
        screen_ = Screen::Home;
}

// The video card's 편집: the video just recorded in the editor, with the file itself as the current result.
void App::EditLastVideo() {
    if (clip_ && _wcsicmp(clip_->SourcePath().c_str(), lastVideo_.path.c_str()) == 0) {
        ResumeEditor();
        return;
    }
    if (exporter_.Busy()) return;
    std::wstring err;
    auto video = VideoClip::Open(lastVideo_.path, err);
    if (!video) {
        Toast(err, true);
        return;
    }
    OpenEditor(video);
    lastResult_ = {};
    lastResult_.ok = true;
    lastResult_.audio = lastVideo_.audio;
    lastResult_.path = lastVideo_.path;
    lastResult_.bytes = lastVideo_.bytes;
    lastResult_.seconds = lastVideo_.seconds;
    hasResult_ = resultOriginal_ = true;
}

// A row of the card for several windows: that file becomes the one the card's 편집 (and the editor's 삭제) is about.
void App::EditVideoAt(size_t i) {
    if (i >= lastVideos_.size()) return;
    lastVideo_ = lastVideos_[i].r;
    EditLastVideo();
}

void App::OnVideoSaved(const CaptureResult& r) {
    if (!r.ok) {
        if (settings_.sounds) sound::Play(sound::Cue::Stop);
        ReportError(tr(L"동영상 저장 실패"), r.error.empty() ? tr(L"동영상을 저장하지 못했습니다.") : r.error);
        return;
    }
    if (settings_.sounds) sound::Play(sound::Cue::Saved);
    ShowVideo(r);
    if (!recNotice_.empty()) lastVideo_.notice = recNotice_;
    std::wstring info = util::FileNameOf(r.path) + L" · " + util::FormatBytes(r.bytes);
    std::wstring reason = std::exchange(stopReason_, {});
    if (NotifyQuietly())
        TrayBalloon(tr(L"동영상 저장 완료 · ") + util::FormatClock(r.seconds, false), reason.empty() ? info : reason);
    else
        ShowMain(true);
    if (!lastVideo_.notice.empty())
        Alert(reason.empty() ? lastVideo_.notice : reason + L" " + lastVideo_.notice);
    else if (!reason.empty())
        Alert(reason + L" · " + info);
    else
        Toast(tr(L"동영상을 저장했습니다 · ") + info);
}

void App::OnVideosSaved() {
    if (settings_.sounds) sound::Play(sound::Cue::Saved);
    // The first window's file at the top of 최근 파일.
    for (size_t i = lastVideos_.size(); i-- > 1;) AddRecent(lastVideos_[i].r.path);
    ShowVideo(lastVideos_[0].r);
    if (!recNotice_.empty()) lastVideo_.notice = recNotice_;
    uint64_t bytes = 0;
    double longest = 0;
    for (const SavedVideo& v : lastVideos_) {
        bytes += v.r.bytes;
        longest = std::max(longest, v.r.seconds);
    }
    const std::wstring info = util::Format(tr(L"창 %zu개"), lastVideos_.size()) + L" · " + util::FormatBytes(bytes);
    std::wstring reason = std::exchange(stopReason_, {});
    if (NotifyQuietly())
        TrayBalloon(tr(L"동영상 저장 완료 · ") + util::FormatClock(longest, false), reason.empty() ? info : reason);
    else
        ShowMain(true);
    if (!lastVideo_.notice.empty())
        Alert(reason.empty() ? lastVideo_.notice : reason + L" " + lastVideo_.notice);
    else if (!reason.empty())
        Alert(reason + L" · " + info);
    else
        Toast(tr(L"동영상을 저장했습니다 · ") + info);
}

void App::TogglePause() {
    if (rec_ != RecState::Recording) return;
    recPaused_ = !recPaused_;
    for (RecSession& s : recs_)
        if (!s.ended && s.capture) s.capture->SetPaused(recPaused_);
    overlay_.SetPaused(recPaused_);
    if (settings_.sounds) sound::Play(sound::Cue::Tick);
    TrayUpdate();
    Invalidate();
}

std::wstring App::RecordingDir() const {
    if (recVideo_) return settings_.outDir;
    return FrameStore::Dir();
}

std::wstring App::TimerWords(int minutes) const {
    const int h = minutes / 60, m = minutes % 60;
    std::wstring s = h ? util::Format(tr(L"%d시간"), h) : L"";
    if (m || !h) s += (s.empty() ? L"" : L" ") + util::Format(tr(L"%d분"), m);
    return s;
}

void App::CheckLimits() {
    if (rec_ != RecState::Recording) return;
    // The record timer (#47), on the recorded time (pauses left out): once for each length it is set to.
    const int timer = settings_.TimerMinutes() * 60;
    const RecSession* lead = RecLead();
    if (timer > 0 && timerFiredFor_ != timer && lead && lead->capture->Elapsed() >= timer) {
        timerFiredFor_ = timer;
        const std::wstring words = TimerWords(timer / 60);
        if (settings_.timerStop) {
            stopReason_ = util::Format(tr(L"%s이 되어 녹화를 멈췄습니다."), words.c_str());
            StopRecording();
            return;
        }
        if (settings_.sounds) sound::Play(sound::Cue::Alarm);
        if (GetForegroundWindow() == hwnd_)
            Toast(util::Format(tr(L"녹화 타이머: %s이 지났습니다. 녹화는 계속됩니다."), words.c_str()));
        else
            TrayBalloon(util::Format(tr(L"녹화 타이머: %s이 지났습니다"), words.c_str()),
                        util::Format(overlay_.Visible() ? tr(L"녹화는 계속됩니다. 멈추려면 녹화 중 표시의 ■ 또는 %s")
                                                        : tr(L"녹화는 계속됩니다. 멈추려면 %s"),
                                     HotkeyLabel().c_str()));
    }
    overlay_.SetTimer(timer, timer > 0 && timerFiredFor_ == timer);
    double now = util::NowSeconds();
    if (now < nextDiskCheck_) return;
    nextDiskCheck_ = now + 2;
    if (util::FreeBytes(RecordingDir()) < kLowDiskStop) {
        stopReason_ = tr(L"남은 저장 공간이 300MB까지 줄어 녹화를 멈추고 저장했습니다.");
        StopRecording();
    }
}

void App::OnHotkey() {
    if (rec_ != RecState::Idle) {
        StopRecording();
        return;
    }
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    RecTarget t;
    std::wstring why;
    if (!fg || pid == GetCurrentProcessId()) {
        // Our window is in front: record whatever is selected in it.
        if (CurrentTarget(t, &why)) {
            StartRecording(t, false);
        } else {
            ShowMain(true);
            Toast(why, true);
        }
        return;
    }
    HWND root = GetAncestor(fg, GA_ROOTOWNER);
    if (!root) root = fg;
    // The desktop or taskbar is "in front" when nothing else is: there is no window to record then.
    const bool shell = IsShellWindow(root);
    switch (settings_.source) {
        case kSourceDisplay: {
            POINT pt{};
            GetCursorPos(&pt);
            StartRecording(MonitorTarget(shell ? MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY)
                                               : MonitorFromWindow(root, MONITOR_DEFAULTTONEAREST)),
                           true);
            break;
        }
        case kSourceRegion:
            if (CurrentTarget(t, &why))
                StartRecording(t, true);
            else
                TrayBalloon(tr(L"영역 녹화를 시작할 수 없습니다"), why);
            break;
        default:
            // 여러 창 녹화: the checked windows, whatever is in front.
            if (MultiPicking() && !picks_.empty()) {
                if (auto ts = PickedTargets(); !ts.empty()) {
                    StartRecording(ts, true);
                    break;
                }
            }
            if (shell)
                TrayBalloon(tr(L"녹화할 창이 없습니다"), tr(L"녹화할 창을 앞에 띄운 뒤 단축키를 누르세요."));
            else
                StartRecording(WindowTarget(root), true);
    }
}

void App::ProbeEncoders() const {
    if (encProbed_) return;
    encProbed_ = true;
    for (int hevc = 0; hevc < 2; ++hevc)
        for (int hw = 0; hw < 2; ++hw) encName_[hevc][hw] = Mp4Writer::FindEncoder(hevc != 0, hw != 0);
}

bool App::HevcEncoder() const {
    if (hevcForTest_ >= 0) return hevcForTest_ > 0;
    ProbeEncoders();
    return !encName_[1][0].empty() || !encName_[1][1].empty();
}

bool App::HevcPlayable() const {
    if (hevcForTest_ >= 0) return hevcForTest_ == 1;
    if (hevcPlayable_ < 0) hevcPlayable_ = Mp4Writer::HevcDecoderAvailable() ? 1 : 0;
    return hevcPlayable_ == 1;
}

bool App::UseHevc(int edge) const {
    if (settings_.vCodec == kCodecH264 || !HevcEncoder()) return false;
    // 자동 past 4096: what H.264 would make smaller keeps its size, if Windows here can play it back.
    return settings_.vCodec == kCodecHevc || (edge > 4096 && HevcPlayable());
}

std::wstring App::VideoSummary(bool hevc, bool bigHevc) const {
    ProbeEncoders();
    std::wstring s = !hevc ? L"H.264" : bigHevc ? tr(L"HEVC (4096보다 커서)") : L"HEVC";
    const std::wstring& hw = encName_[hevc][1];
    if (settings_.vHardware && !hw.empty()) {
        s += L" · " + Mp4Writer::Vendor(hw) + tr(L" 가속");
    } else {
        s += tr(L" · CPU 인코딩");
    }
    const UINT32 bps = settings_.VideoBitrate();
    s += bps ? L" · " + MbpsText((int)(bps / 1000)) + L" Mbps" : std::wstring(tr(L" · 자동 화질"));
    if (settings_.vFrameMode == 1) s += tr(L" · 가변 프레임");
    return s;
}

std::wstring App::AudioSummary(int source) const {
    std::wstring s;
    if (settings_.audio)
        s = source != kSourceWindow                         ? tr(L"시스템 전체 소리 녹음")
            : AudioCapture::ProcessLoopbackSupported() ? (MultiPicking() ? tr(L"창마다 그 창의 소리만 녹음") : tr(L"이 창의 소리만 녹음"))
                                                       : tr(L"창 소리 녹음 불가 (Windows 10 2004 이상)");
    if (settings_.mic) s = s.empty() ? tr(L"마이크만 녹음") : s + tr(L" + 마이크");
    return s.empty() ? tr(L"소리 끔") : s;
}

std::wstring App::MicDeviceName() const {
    for (const auto& d : micDevices_)
        if (settings_.micDevice.empty() ? d.isDefault : d.id == settings_.micDevice)
            return settings_.micDevice.empty() ? tr(L"기본 장치 · ") + d.name : d.name;
    return settings_.micDevice.empty() ? tr(L"기본 장치") : tr(L"연결되지 않은 장치");
}

// The input devices for the 마이크 장치 list: Windows' default, then each by name.
std::vector<MenuItem> App::MicItems() {
    std::vector<MenuItem> items;
    std::wstring def = tr(L"Windows 기본 장치");
    for (const auto& d : micDevices_)
        if (d.isDefault) def += L" (" + d.name + L")";
    items.push_back({def, 0});
    for (size_t i = 0; i < micDevices_.size(); ++i) items.push_back({micDevices_[i].name, (int)i + 1, i == 0});
    if (micDevices_.empty()) items.push_back({tr(L"연결된 마이크가 없습니다"), -2, true, false});
    return items;
}

std::wstring App::MbpsText(int kbps) {
    kbps = (kbps + 50) / 100 * 100;
    return kbps % 1000 ? util::Format(L"%d.%d", kbps / 1000, kbps % 1000 / 100) : std::to_wstring(kbps / 1000);
}

// Each language by its own name; "System" says which one Windows gives.
std::wstring App::LanguageName(int choice) const {
    switch (choice) {
        case kLangKorean: return L"한국어";
        case kLangEnglish: return L"English";
        default:
            return util::Format(tr(L"시스템 (%s)"),
                                LanguageName(SystemLanguage() == Lang::Korean ? kLangKorean : kLangEnglish).c_str());
    }
}

// ---------------------------------------------------------------- editor

void App::OpenEditor(std::shared_ptr<Clip> clip) {
    StopThumbnails();
    audioPreview_.Stop();
    DetachReaders();
    clip_ = std::move(clip);
    framePx_.clear();
    frameBmp_.Reset();
    shown_ = SIZE_MAX;
    inF_ = 0;
    outF_ = clip_->Count() - 1;
    playhead_ = 0;
    playing_ = playBack_ = false;
    caption_.clear();
    captionPos_ = 0;
    captionOpen_ = false;
    cropMode_ = false;
    cropAspect_ = 0;
    cropDrag_ = -1;
    ResetCrop();
    hasResult_ = resultOriginal_ = false;
    estKey_.clear();
    fitContent_.clear();
    estValid_ = fitting_ = fitMissed_ = false;
    fitBitrate_ = 0;
    fitCorrection_ = 1;
    // A video stays a video by default, an opened 움짤 starts in its own format and at 고화질 (#41), a recording
    // starts with the last 움짤 format.
    const bool video = clip_->IsVideo();
    auto* anim = dynamic_cast<const AnimClip*>(clip_.get());
    cutSource_ = video ? mp4::CutSource::Open(clip_->SourcePath()) : nullptr;
    if (cutSource_ && cutSource_->Frames() != clip_->Count()) cutSource_.reset();  // numbered differently: don't
    fmt_ = video && FormatAvailable(Format::Mp4)            ? (int)Format::Mp4
           : anim && FormatAvailable(anim->FileFormat()) ? (int)anim->FileFormat()
                                                           : settings_.format;
    settings_.UseRecordingKnobs();  // a 움짤 file open before this one had its own
    if (anim) settings_.UseFileKnobs();
    settings_.UseExportKind(IsVideoFormat(fmt_));
    if (settings_.exportPreset >= 0) settings_.ApplyPreset(settings_.exportPreset, IsVideoFormat(fmt_));
    exportAudio_ = true;
    AttachReaders();
    screen_ = Screen::Editor;
    StartThumbnails();
    SetPlaying(true);
    Invalidate();
}

// ---------------------------------------------------------------- files

// A video dropped on the window, picked in the open dialog or the recent list, or given on the command line.
void App::OpenMediaFile(const std::wstring& path) {
    ShowMain(true);
    filesOpen_ = false;
    if (rec_ != RecState::Idle) {
        Toast(tr(L"녹화 중에는 파일을 열 수 없습니다."), true);
        return;
    }
    if (exporter_.Busy()) {
        Toast(tr(L"내보내기가 끝난 뒤에 열 수 있습니다."), true);
        return;
    }
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        Toast(tr(L"파일을 찾을 수 없습니다: ") + util::FileNameOf(path), true);
        return;
    }
    animLoad_.reset();  // a file opened after it wins
    KillTimer(hwnd_, kTimerAnimLoad);
    if (util::IsAnimPath(path) || AnimClip::Sniff(path) != Format::Count) {
        OpenAnimFile(path);
        return;
    }
    if (OpenVideo(path)) {
        AddRecent(path);
        Toast(tr(L"편집 탭에서 열었습니다: ") + util::FileNameOf(path));
    }
}

// A 움짤 decodes on a worker; the toast says how far it got, and the editor opens it once it is done.
void App::OpenAnimFile(const std::wstring& path) {
    animLoad_ = std::make_unique<AnimLoad>();
    AnimLoad* l = animLoad_.get();
    l->path = path;
    l->thread = std::thread([l, hwnd = hwnd_] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // WIC
        l->clip = AnimClip::Open(l->path, l->cancel, l->progress, l->err);
        if (SUCCEEDED(co)) CoUninitialize();
        l->done = true;
        PostMessageW(hwnd, WM_APP_ANIM_LOADED, 0, 0);
    });
    SetTimer(hwnd_, kTimerAnimLoad, 150, nullptr);
    ShowAnimProgress();
}

void App::ShowAnimProgress() {
    Toast(util::Format(tr(L"여는 중… %d%% · %s"), (int)(animLoad_->progress * 100),
                       util::FileNameOf(animLoad_->path).c_str()));
}

void App::OnAnimLoaded() {
    if (!animLoad_ || !animLoad_->done) return;
    KillTimer(hwnd_, kTimerAnimLoad);
    auto l = std::move(animLoad_);
    l->thread.join();
    if (!l->clip) {
        Toast(l->err, true);
        return;
    }
    if (rec_ != RecState::Idle || exporter_.Busy()) {  // started while it decoded
        Toast(tr(L"녹화나 내보내기가 끝난 뒤에 다시 열어 주세요: ") + util::FileNameOf(l->path), true);
        return;
    }
    ShowMain(true);
    filesOpen_ = false;
    OpenEditor(l->clip);
    AddRecent(l->path);
    Toast(tr(L"편집 탭에서 열었습니다: ") + util::FileNameOf(l->path));
}

void App::BrowseForVideo() {
    filesOpen_ = false;
    std::wstring path;
    if (util::PickVideoFile(hwnd_, settings_.outDir, path)) OpenMediaFile(path);
}

void App::AddRecent(const std::wstring& path) {
    settings_.AddRecent(path);
    settings_.Save();
}

void App::RefreshRecent() {
    recentFiles_.clear();
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    auto ticks = [](const FILETIME& f) { return (int64_t)((uint64_t)f.dwHighDateTime << 32 | f.dwLowDateTime); };
    for (auto& p : settings_.recent) {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        RecentFile f;
        f.path = p;
        f.bytes = (uint64_t)fa.nFileSizeHigh << 32 | fa.nFileSizeLow;
        f.age = std::max<int64_t>(0, ticks(nowFt) - ticks(fa.ftLastWriteTime)) / 1e7;
        f.video = !util::IsAnimPath(p);
        recentFiles_.push_back(std::move(f));
    }
}

void App::HoldRecording(std::shared_ptr<Clip> clip, const std::wstring& name) {
    held_.insert(held_.begin(), {std::move(clip), util::NowSeconds(), name});
    // Bounded: each one keeps a temporary file of compressed frames.
    if (held_.size() > kMaxHeld) {
        held_.resize(kMaxHeld);
        Toast(util::Format(tr(L"저장하지 않은 녹화가 %zu개를 넘어 가장 오래된 것을 지웠습니다."), kMaxHeld), true);
    }
}

size_t App::RecoverRecordings() {
    auto found = FrameStore::Recover();  // newest first
    double now = util::NowSeconds();
    for (auto it = found.rbegin(); it != found.rend(); ++it)
        held_.insert(held_.begin(), {it->clip, now - it->age, tr(L"복구한 녹화")});
    if (held_.size() > kMaxHeld) held_.resize(kMaxHeld);  // the oldest go, with their files
    return found.size();
}

void App::DropHeld(const Clip* clip) {
    std::erase_if(held_, [&](const HeldClip& h) { return h.clip.get() == clip; });
}

void App::OpenHeld(size_t i) {
    if (i >= held_.size()) return;
    filesOpen_ = false;
    auto clip = held_[i].clip;
    clipName_ = held_[i].name;
    if (clip != clip_)
        OpenEditor(clip);
    else
        ResumeEditor();
}

void App::ConfirmDropHeld(size_t i) {
    if (i >= held_.size()) return;
    const HeldClip& h = held_[i];
    Ask({tr(L"이 녹화를 버릴까요?"),
         h.name + L" · " + util::FormatClock(h.clip->Seconds(), true) + tr(L"\n버리면 되돌릴 수 없습니다."), tr(L"버리기"), true,
         [this, clip = std::weak_ptr<Clip>(h.clip)] {
             auto c = clip.lock();
             if (!c) return;
             if (c == clip_) CloseClip();
             DropHeld(c.get());
             Toast(tr(L"녹화를 버렸습니다."));
             Invalidate();
         }});
}

void App::AskQuit(bool restart, std::function<void()> then) {
    if (held_.empty()) {
        then();
        return;
    }
    Ask({util::Format(tr(L"저장하지 않은 녹화가 %zu개 있습니다"), held_.size()),
         restart ? tr(L"다시 시작하면 사라집니다. 남기려면 먼저 편집 탭에서 저장하세요.")
                 : tr(L"종료하면 사라집니다. 남기려면 먼저 편집 탭에서 저장하세요."),
         restart ? tr(L"다시 시작해서 업데이트") : tr(L"종료"), true, std::move(then)});
}

void App::Ask(Question q) {
    if (!IsWindowVisible(hwnd_) || IsIconic(hwnd_)) ShowMain(true);
    SetPlaying(false);
    filesOpen_ = updateOpen_ = false;
    question_ = std::move(q);
    ++questionSeq_;
    Invalidate();
}

// ---------------------------------------------------------------- updates

// From the timer: at startup, then once a day (an hour after a failed check). The settings call it to check now.
void App::CheckForUpdate() {
    Updater::Status st = updater_.Get();
    double since = util::NowSeconds() - st.checkedAt;
    bool due = st.checkedAt == 0 || since >= 24 * 3600 || (st.state == Updater::State::Failed && since >= 3600);
    if (settings_.autoUpdate && due) updater_.Check();
}

bool App::UpdateReady() const { return updater_.Get().state == Updater::State::Ready; }

void App::InstallUpdate() {
    updateOpen_ = false;
    if (rec_ != RecState::Idle || exporter_.Busy()) {
        Toast(tr(L"녹화 · 내보내기가 끝나면 업데이트할 수 있습니다."), true);
        return;
    }
    AskQuit(true, [this] {
        if (rec_ == RecState::Idle && !exporter_.Busy()) RestartIntoUpdate();
    });
}

void App::RestartIntoUpdate() {
    std::wstring err;
    if (!updater_.Install(err)) {
        Alert(err);
        return;
    }
    // The new exe waits until this process has ended, then starts as this one did (main.cpp).
    std::wstring exe = util::ExePath();
    std::wstring cmd = util::Format(L"\"%s\" --updated %lu %d", exe.c_str(), GetCurrentProcessId(),
                                    IsWindowVisible(hwnd_) ? 1 : 0);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        Alert(tr(L"새 버전을 시작하지 못했습니다. OzMaker를 다시 켜면 새 버전으로 시작합니다."));
        return;
    }
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    quitting_ = true;
    DestroyWindow(hwnd_);
}

bool App::OpenVideo(const std::wstring& path) {
    std::wstring err;
    auto video = VideoClip::Open(path, err);
    if (!video) {
        Toast(err, true);
        return false;
    }
    OpenEditor(video);
    return true;
}

// The 편집 tab: the open file where it was left, playing again, or with none open, what there is to open.
void App::ResumeEditor() {
    screen_ = Screen::Editor;
    if (!clip_) {
        RefreshRecent();
        Invalidate();
        return;
    }
    AttachReaders();
    SetPlaying(true);
}

// The 녹화 tab. The open file stays open in the 편집 tab (the dot on the tab) until its 닫기 or the next one opened;
// a video's file is let go meanwhile so it can be moved or deleted.
void App::CloseEditor() {
    if (exporter_.Busy()) return;
    PauseEditor();
    screen_ = Screen::Home;
    RefreshWindows();
}

void App::PauseEditor() {
    playing_ = false;
    cropMode_ = false;
    SyncAudioPreview();
    DetachReaders();
}

// The file bar's 닫기: the 편집 tab with nothing open. A recording not saved yet stays among 저장하지 않은 녹화.
void App::CloseClip() {
    if (!clip_ || exporter_.Busy()) return;
    PauseEditor();
    StopThumbnails();
    clip_.reset();
    cutSource_.reset();
    frameBmp_.Reset();
    hasResult_ = resultOriginal_ = false;
    settings_.UseRecordingKnobs();
    RefreshRecent();
    Invalidate();
}

const wchar_t* App::Activity() const {
    if (rec_ == RecState::Recording) return recVideo_ ? tr(L"녹화 중 · 동영상") : tr(L"녹화 중 · 움짤");
    if (rec_ == RecState::Countdown) return tr(L"녹화 카운트다운");
    if (exporter_.Busy()) return tr(L"내보내기 중");
    switch (screen_) {
        case Screen::Editor: return clip_ ? tr(L"편집 탭") : tr(L"편집 탭 · 연 파일 없음");
        case Screen::Settings: return tr(L"설정");
        case Screen::Replay: return tr(L"리플레이 탭");
        default: return tr(L"녹화 탭");
    }
}

// The editor's 버리기 (a 움짤 recording) and 삭제 (the video just recorded, into the Recycle Bin). Only what this run
// recorded: a video opened by hand gets no button. Files exported from the recording stay.
bool App::CanDiscard() const {
    if (!clip_ || exporter_.Busy()) return false;
    const std::wstring& src = clip_->SourcePath();
    return src.empty() || (!lastVideo_.path.empty() && _wcsicmp(src.c_str(), lastVideo_.path.c_str()) == 0);
}

void App::ConfirmDiscard() {
    if (!CanDiscard()) return;
    bool held = std::any_of(held_.begin(), held_.end(), [&](const HeldClip& h) { return h.clip == clip_; });
    // The editor's clip when asked: the answer applies to that one only.
    auto discard = [this, clip = std::weak_ptr<Clip>(clip_)] {
        if (clip.lock() == clip_) DiscardRecording();
    };
    const std::wstring name = util::FileNameOf(clip_->SourcePath());
    if (name.empty())
        Ask({tr(L"이 녹화를 버릴까요?"),
             ClipTitle() + L" · " + util::FormatClock(clip_->Seconds(), true) +
                 (held ? tr(L"\n버리면 되돌릴 수 없습니다.") : tr(L"\n저장한 파일은 그대로 남습니다.")),
             tr(L"버리기"), true, discard});
    else if (settings_.deleteMode == kDeleteNow)
        Ask({tr(L"이 동영상을 삭제할까요?"),
             name + tr(L"\n휴지통을 거치지 않고 지워서 되돌릴 수 없습니다.") + tr(L"\n이 녹화에서 따로 저장한 파일은 그대로 남습니다."),
             tr(L"삭제"), true, discard});
    else
        Ask({tr(L"이 동영상을 휴지통으로 옮길까요?"), name + tr(L"\n이 녹화에서 따로 저장한 파일은 그대로 남습니다."),
             tr(L"휴지통으로"), true, discard});
}

bool App::RemoveFile(const std::wstring& path) {
    if (settings_.deleteMode == kDeleteNow || testDeleteFiles_) return DeleteFileW(path.c_str()) != 0;
    return util::MoveToRecycleBin(hwnd_, path);
}

void App::DiscardRecording() {
    if (!CanDiscard()) return;
    const std::wstring path = clip_->SourcePath();
    PauseEditor();
    StopThumbnails();
    if (!path.empty()) {
        // Nothing may hold the file: the readers went with the editor; a size estimate in flight gets a moment.
        for (double until = util::NowSeconds() + 3; estimator_.Busy() && util::NowSeconds() < until;) Sleep(10);
        if (!RemoveFile(path)) {
            AttachReaders();
            Toast(settings_.deleteMode == kDeleteNow
                      ? tr(L"파일을 삭제하지 못했습니다. 다른 프로그램에서 열려 있다면 닫고 다시 해 보세요.")
                      : tr(L"동영상을 휴지통으로 옮기지 못했습니다. 다른 프로그램에서 열려 있다면 닫고 다시 해 보세요."),
                  true);
            Invalidate();
            return;
        }
        // 여러 창 녹화: the card keeps the other files.
        std::erase_if(lastVideos_, [&](const SavedVideo& v) { return _wcsicmp(v.r.path.c_str(), path.c_str()) == 0; });
        if (!lastVideos_.empty()) {
            lastVideo_ = lastVideos_[0].r;
            if (lastVideos_.size() == 1) lastVideos_.clear();
        } else {
            showVideo_ = false;
            lastVideo_ = {};
        }
        RefreshRecent();
    }
    DropHeld(clip_.get());
    clip_.reset();
    frameBmp_.Reset();
    hasResult_ = resultOriginal_ = false;
    RefreshRecent();
    if (path.empty())
        Toast(tr(L"녹화를 버렸습니다."));
    else
        Toast((settings_.deleteMode == kDeleteNow ? tr(L"삭제했습니다 · ") : tr(L"휴지통으로 옮겼습니다 · ")) +
              util::FileNameOf(path));
    Invalidate();
}

void App::AttachReaders() {
    if (!clip_) return;
    if (clip_->SlowSeek()) {
        if (!fetcher_.Running()) {
            fetcher_.Start(clip_, hwnd_, WM_APP_FRAME);
            shown_ = SIZE_MAX;  // ask the new fetcher
        }
    } else if (!reader_) {
        reader_ = clip_->NewReader();
    }
}

void App::DetachReaders() {
    fetcher_.Stop();
    reader_.reset();
}

void App::ShowFrame(size_t index) {
    if (!clip_) return;
    bool fresh = frameBmp_ && frameBmpGen_ == gfx_.Generation();
    if (index == shown_ && fresh) return;
    if (clip_->SlowSeek()) {
        // The fetcher answers with WM_APP_FRAME; until then the last frame stays up.
        if (!fresh && framePx_.size() == (size_t)clip_->Width() * clip_->Height() * 4) UploadFrame(framePx_.data());
        if (fetcher_.Running()) fetcher_.Want(index);
        return;
    }
    if (!reader_) return;
    const uint8_t* px = reader_->Get(index);
    if (!px) return;
    UploadFrame(px);
    shown_ = index;
}

void App::UploadFrame(const uint8_t* px) {
    UINT w = clip_->Width(), h = clip_->Height();
    if (!frameBmp_ || frameBmpGen_ != gfx_.Generation()) {
        frameBmp_ = gfx_.CreateBitmap(w, h, px, w * 4);
        frameBmpGen_ = gfx_.Generation();
    } else {
        frameBmp_->CopyFromMemory(nullptr, px, w * 4);
    }
}

// A video's sound follows the preview: only while it plays forward at 1x in the editor and OzMaker is in front.
void App::SyncAudioPreview() {
    auto video = std::dynamic_pointer_cast<const VideoClip>(clip_);
    bool want = video && video->HasAudio() && !previewMute_ && settings_.previewVolume > 0 && playing_ &&
                !playBack_ && !scrubbing_ && screen_ == Screen::Editor && PreviewRate() == 1.0 && hwnd_ &&
                IsWindowVisible(hwnd_) && !IsIconic(hwnd_) && GetForegroundWindow() == hwnd_;
    if (!want) {
        audioPreview_.Stop();
        return;
    }
    float v = settings_.previewVolume / 100.f;
    audioPreview_.SetGain(v * v);  // squared, so the steps sound about even
    if (audioPreview_.Active() && audioEpoch_ == playEpoch_) return;
    audioEpoch_ = playEpoch_;
    int64_t start = clip_->Ts(inF_), end = RangeEndTs();
    int64_t t = playTs_ + (int64_t)((util::NowSeconds() - playWall_) * 1e7);
    audioPreview_.Start(video, std::clamp(t, start, end - 1), end);
}

// The speaker button. A volume dragged down to 0 comes back at 50 %.
void App::TogglePreviewMute() {
    if (!previewMute_ && settings_.previewVolume > 0) {
        previewMute_ = true;
        return;
    }
    previewMute_ = false;
    if (settings_.previewVolume == 0) {
        settings_.previewVolume = 50;
        settingsDirty_ = true;
    }
}

// The volume slider and the wheel over it; moving it turns the sound back on.
void App::SetPreviewVolume(int v) {
    v = std::clamp(v, 0, 100);
    previewMute_ = false;
    if (v == settings_.previewVolume) return;
    settings_.previewVolume = v;
    settingsDirty_ = true;
}

int64_t App::RangeEndTs() const {
    if (!clip_) return 0;
    return outF_ + 1 < clip_->Count() ? clip_->Ts(outF_ + 1) : clip_->EndTs();
}

void App::SetPlaying(bool play) {
    if (!clip_) return;
    if (play && !playing_) {
        // Normal playback, the way the export goes.
        playBack_ = PreviewBackwards();
        if (!playBack_ && playhead_ >= outF_) playhead_ = inF_;
        if (playBack_ && playhead_ <= inF_) playhead_ = outF_;
    }
    shuttle_ = 0;
    playing_ = play;
    RestartClock();
}

void App::Seek(size_t index) {
    if (!clip_) return;
    playhead_ = std::min(index, clip_->Count() - 1);
    RestartClock();
}

// L plays forward, J backward, again for 2× and 4× (as in editing programs). It stops at the end of the range.
void App::Shuttle(int dir) {
    if (!clip_) return;
    int s = playing_ && shuttle_ * dir > 0 ? std::clamp(shuttle_ * 2, -4, 4) : dir;
    if (dir > 0 && playhead_ >= outF_) playhead_ = inF_;
    if (dir < 0 && playhead_ <= inF_) playhead_ = outF_;
    playing_ = true;
    playBack_ = dir < 0;
    shuttle_ = s;
    RestartClock();
}

void App::ResetRange() {
    if (!clip_) return;
    inF_ = 0;
    outF_ = clip_->Count() - 1;
    hasResult_ = false;
    Toast(tr(L"구간을 처음으로 되돌렸습니다"));
}

void App::RestartClock() {
    playWall_ = util::NowSeconds();
    // Backwards a frame starts at its end.
    const size_t i = playhead_;
    playTs_ = !playBack_ ? clip_->Ts(i) : (i + 1 < clip_->Count() ? clip_->Ts(i + 1) : clip_->EndTs()) - 1;
    ++playEpoch_;
    Invalidate();
}

bool App::PreviewBackwards() const { return !IsVideoFormat(fmt_) && settings_.directionIdx == 1; }

void App::FollowDirection() {
    if (!clip_ || shuttle_) return;
    const bool boomerang = !IsVideoFormat(fmt_) && settings_.directionIdx == 2;
    if (!boomerang) playBack_ = PreviewBackwards();
    RestartClock();
}

size_t App::PlayheadFromClock() {
    const double rate = PreviewRate();
    const bool boomerang = !shuttle_ && !IsVideoFormat(fmt_) && settings_.directionIdx == 2;
    const bool once = kLoopOptions[settings_.loopIdx] == 1;
    int64_t start = clip_->Ts(inF_), end = RangeEndTs();
    int64_t len = std::max<int64_t>(1, end - start);
    double now = util::NowSeconds();
    int64_t step = (int64_t)((now - playWall_) * 1e7 * rate);
    int64_t t = playBack_ ? playTs_ - step : playTs_ + step;
    // Restart the clock at a loop or turn (the sound restarts with it).
    auto restart = [&](int64_t at) {
        playTs_ = at;
        playWall_ = now;
        ++playEpoch_;
    };
    auto stop = [&](int64_t at) {
        t = at;
        playing_ = false;
        shuttle_ = 0;
    };
    if (!playBack_ && t < start) {
        t = start;  // the range moved past the playhead
        restart(t);
    } else if (playBack_ && t >= end) {
        t = end - 1;
        restart(t);
    } else if (!playBack_ && t >= end) {
        if (shuttle_ || (once && !boomerang)) {
            stop(end - 1);
        } else if (boomerang) {
            t = std::max(start, end - 1 - (t - end) % len);
            playBack_ = true;
            restart(t);
        } else {
            t = start + (t - start) % len;
            restart(t);
        }
    } else if (playBack_ && t < start) {
        if (shuttle_ || once) {
            stop(start);
        } else if (boomerang) {
            t = std::min(end - 1, start + (start - t) % len);
            playBack_ = false;
            restart(t);
        } else {
            t = end - 1 - (start - t - 1) % len;
            restart(t);
        }
    }
    t = std::clamp(t, start, end - 1);
    size_t idx = std::clamp(clip_->IndexAt(t), inF_, outF_);
    if (playing_) {
        // Until the next frame comes up, the way the clock runs.
        int64_t left = playBack_ ? t - clip_->Ts(idx) + 1 : (idx + 1 <= outF_ ? clip_->Ts(idx + 1) : end) - t;
        RequestWake(now + std::max(0.002, left / 1e7 / rate));
    }
    return idx;
}

void App::ResetCrop() {
    if (clip_) crop_ = {0, 0, clip_->Width(), clip_->Height()};
}

void App::ApplyCropAspect() {
    static const double aspects[] = {0, 1.0, 4.0 / 3.0, 16.0 / 9.0, 9.0 / 16.0};
    if (!clip_ || cropAspect_ <= 0) return;
    double a = aspects[cropAspect_];
    int W = clip_->Width(), H = clip_->Height();
    int w = W, h = (int)std::lround(W / a);
    if (h > H) {
        h = H;
        w = (int)std::lround(H * a);
    }
    int cx = (crop_.left + crop_.right) / 2, cy = (crop_.top + crop_.bottom) / 2;
    int x = std::clamp(cx - w / 2, 0, W - w), y = std::clamp(cy - h / 2, 0, H - h);
    crop_ = {x, y, x + w, y + h};
}

void App::OutputSize(int& w, int& h, int scaleIdx) const {
    int cw = crop_.right - crop_.left, ch = crop_.bottom - crop_.top;
    const ExportSize& o = kExportSizes[scaleIdx < 0 ? settings_.scaleIdx : scaleIdx];
    double s = o.width ? std::min(1.0, o.width / (double)std::max(1, cw)) : o.percent == 33 ? 1.0 / 3.0 : o.percent / 100.0;
    w = std::max(2, (int)std::lround(cw * s) & ~1);
    h = std::max(2, (int)std::lround(ch * s) & ~1);
}

double App::EffectiveOutFps(int outFpsIdx) const {
    double limit = kOutFpsOptions[outFpsIdx < 0 ? settings_.outFpsIdx : outFpsIdx];
    if (IsVideoFormat(fmt_)) {
        // Constant rate: the chosen one, or the source's own rounded up to a usual rate (29.4 -> 30).
        if (limit > 0) return limit;
        double avg = !clip_ ? 30.0 : clip_->NominalFps() > 0 ? clip_->NominalFps() : clip_->AverageFps();
        for (double r : {10.0, 15.0, 20.0, 24.0, 25.0, 30.0, 50.0, 60.0, 90.0, 120.0})
            if (avg <= r * 1.03) return r;
        return 120.0;
    }
    // Output frames per second (after the speed change), capped at what the format plays back reliably.
    double maxF = FormatMaxFps((Format)fmt_);
    return limit == 0 || limit > maxF ? maxF : limit;
}

// MP4 and WebM keep the video's sound unless the speed changes (no time-stretching).
bool App::ExportAudio() const {
    auto* video = dynamic_cast<const VideoClip*>(clip_.get());
    return IsVideoFormat(fmt_) && exportAudio_ && video && video->HasAudio() && video->AudioExportable() &&
           kSpeedOptions[settings_.speedIdx] == 1.0;
}

int App::ExportQuality() const {
    if (fmt_ == (int)Format::WebM) return settings_.webmQuality;
    return fmt_ < (int)Format::Mp4 ? settings_.quality[fmt_] : 0;
}

const wchar_t* App::CopyBlocker() const {
    if (!clip_ || !cutSource_ || fmt_ != (int)Format::Mp4) return nullptr;
    if (settings_.targetIdx) return tr(L"용량에 맞추려고");
    if (settings_.scaleIdx != 0 || settings_.outFpsIdx != 0) return tr(L"크기나 프레임 레이트를 바꿔서");
    if (crop_.left != 0 || crop_.top != 0 || crop_.right != clip_->Width() || crop_.bottom != clip_->Height())
        return tr(L"화면을 잘라서");
    if (kSpeedOptions[settings_.speedIdx] != 1.0) return tr(L"재생 속도를 바꿔서");
    if (!caption_.empty()) return tr(L"자막을 넣어서");
    return L"";
}

VideoConfig App::VideoSettings(int fps) const {
    VideoConfig v;
    v.fps = fps;
    v.hevc = UseHevc(0);  // HEVC when picked; 자동 goes by the size (PlannedSize, MakeJob)
    v.hardware = settings_.vHardware;
    v.bitrate = settings_.VideoBitrate();
    v.cbr = settings_.vRateCtrl == 1;
    v.vfr = settings_.vFrameMode == 1;
    v.gopFrames = fps * kKeyframeOptions[settings_.vKeyIdx];
    v.qualityVsSpeed = kPresetOptions[settings_.vPresetIdx];
    return v;
}

// In one pass, so a window title that looks like a token stays as it is.
std::wstring App::FillName(std::wstring_view pattern, const std::wstring& what, const SYSTEMTIME& t, int number) {
    std::wstring name = util::SafeFileName(what.empty() ? L"OzMaker" : what, 60);
    if (name.empty()) name = L"OzMaker";
    const std::wstring date =
        util::Format(L"%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const std::wstring values[kTokCount] = {date,
                                            util::Format(L"%04d", t.wYear),
                                            util::Format(L"%02d", t.wMonth),
                                            util::Format(L"%02d", t.wDay),
                                            util::Format(L"%02d", t.wHour),
                                            util::Format(L"%02d", t.wMinute),
                                            util::Format(L"%02d", t.wSecond),
                                            name,
                                            util::Format(L"%03d", number)};
    std::wstring s;
    for (size_t i = 0, len = 0; i < pattern.size();) {
        if (int k = NameTokenAt(pattern, i, len); k >= 0) {
            s += values[k];
            i += len;
        } else {
            s += pattern[i++];
        }
    }
    s = util::SafeFileName(s, 120);
    return s.empty() ? L"OzMaker_" + date : s;
}

// Exports of a video are named after it ("<name>_trim.mp4", "<name>.gif"); a fresh clip gets the pattern of the
// settings. Its {번호} is the first one no file in the save folder has yet, whatever the extension.
std::wstring App::FileStem(const std::wstring& what, bool findNumber) const {
    SYSTEMTIME t;
    GetLocalTime(&t);
    const std::wstring& p = settings_.nameTemplate;
    std::wstring s = FillName(p, what, t, 1);
    if (!findNumber || (p.find(kNameTokens[kTokNumber][0]) == std::wstring::npos &&
                        p.find(kNameTokens[kTokNumber][1]) == std::wstring::npos))
        return s;
    auto taken = [&](const std::wstring& stem) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((settings_.outDir + L"\\" + stem + L".*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return false;
        FindClose(h);
        return true;
    };
    for (int n = 2; n < 100000 && taken(s); ++n) s = FillName(p, what, t, n);
    return s;
}

std::wstring App::ExportName(Format f) const {
    std::wstring ext = FormatExt(f), src = clip_ ? clip_->SourcePath() : std::wstring();
    std::wstring stem = FileStem(clipName_);
    if (!src.empty()) {
        stem = util::FileNameOf(src);
        const size_t dot = stem.rfind(L'.');
        // A video saved as a video, or a 움짤 file in its own format: the same kind of file again, cut.
        const bool same = dot != std::wstring::npos && _wcsicmp(stem.c_str() + dot + 1, ext.c_str()) == 0;
        if (dot != std::wstring::npos) stem.resize(dot);
        if (same || (clip_->IsVideo() && IsVideoFormat(f))) stem += L"_trim";
    }
    std::wstring name = stem + L"." + ext;
    for (int i = 2; GetFileAttributesW((settings_.outDir + L"\\" + name).c_str()) != INVALID_FILE_ATTRIBUTES; ++i)
        name = stem + util::Format(L"_%d.", i) + ext;
    return name;
}

void App::StartExport(bool saveAs) {
    if (!clip_ || exporter_.Busy()) return;
    Format f = (Format)fmt_;
    if (!FormatAvailable(f)) {
        Toast(std::wstring(FormatName(f)) + tr(L" 인코더가 이 빌드에 포함되어 있지 않습니다."), true);
        return;
    }
    std::wstring name = ExportName(f);
    std::wstring path;
    if (saveAs) {
        if (!util::PickSavePath(hwnd_, settings_.outDir, name, FormatExt(f), FormatName(f), path)) return;
    } else {
        if (!util::EnsureDir(settings_.outDir)) {
            Toast(tr(L"저장 폴더를 만들 수 없습니다: ") + settings_.outDir, true);
            return;
        }
        path = settings_.outDir + L"\\" + name;
    }
    if (_wcsicmp(path.c_str(), clip_->SourcePath().c_str()) == 0) {
        Toast(tr(L"편집 중인 원본 파일에는 덮어쓸 수 없습니다. 다른 이름을 고르세요."), true);
        return;
    }
    ExportJob job = MakeJob(settings_.scaleIdx, settings_.outFpsIdx, ExportQuality());
    job.path = path;
    if (const wchar_t* why = CopyBlocker(); why && !*why) job.cut = cutSource_;
    settings_.Save();
    hasResult_ = resultOriginal_ = false;
    exportEstimate_ = estValid_ && estKey_ == EstimateKey() ? estBytes_ : 0;
    exportStarted_ = util::NowSeconds();
    exporter_.Start(std::move(job), hwnd_, WM_APP_EXPORT_DONE);
    Invalidate();
}

void App::OnExportDone() {
    ExportResult r = exporter_.Take();
    if (r.cancelled) {
        Toast(tr(L"내보내기를 취소했습니다."));
    } else if (!r.ok) {
        Alert(r.error.empty() ? tr(L"내보내기에 실패했습니다.") : r.error);
    } else {
        lastResult_ = r;
        hasResult_ = true;
        resultOriginal_ = false;
        DropHeld(clip_.get());  // exported: nothing to protect any more
        AddRecent(r.path);
        if (settings_.sounds) sound::Play(sound::Cue::Saved);
        uint64_t target = (uint64_t)kTargetMB[settings_.targetIdx] * 1'000'000;
        if (target && r.bytes > target) {
            // The guess was low for this clip: learn by how much and fit again, smaller.
            if (exportEstimate_) fitCorrection_ = std::clamp(fitCorrection_ * r.bytes / (double)exportEstimate_, 1.0, 4.0);
            fitContent_.clear();
            Toast(util::Format(tr(L"목표 %dMB보다 커졌습니다(%s). 더 작게 다시 맞췄으니 한 번 더 저장하세요."),
                               kTargetMB[settings_.targetIdx], util::FormatBytes(r.bytes).c_str()),
                  true);
        }
    }
    Invalidate();
}

ExportJob App::MakeJob(int scaleIdx, int outFpsIdx, int quality) const {
    ExportJob job;
    job.clip = clip_;
    job.first = inF_;
    job.last = outF_;
    job.crop = crop_;
    OutputSize(job.outW, job.outH, scaleIdx);
    job.fpsLimit = EffectiveOutFps(outFpsIdx);
    job.speed = kSpeedOptions[settings_.speedIdx];
    job.format = (Format)fmt_;
    job.plays = kLoopOptions[settings_.loopIdx];
    job.holdLastMs = settings_.holdLast ? kHoldLastMs : 0;
    job.direction = IsVideoFormat(job.format) ? 0 : settings_.directionIdx;
    job.caption = caption_;
    job.captionPos = captionPos_;
    if (job.format == Format::Mp4) {
        job.video = VideoSettings((int)std::lround(job.fpsLimit));
        // As the encoder takes it (#42): 자동 keeps a size past 4096 in HEVC; otherwise it is made smaller.
        const int edge = std::max(job.outW, job.outH);
        job.video.hevc = UseHevc(edge);
        if (double fit = (job.video.hevc ? 8192.0 : 4096.0) / edge; fit < 1) {
            job.outW = std::max(2, (int)std::lround(job.outW * fit) & ~1);
            job.outH = std::max(2, (int)std::lround(job.outH * fit) & ~1);
        }
    } else if (job.format == Format::WebM) {
        job.quality = quality;  // VP9 by quality (#40); a bitrate only to fit a size
    }
    if (IsVideoFormat(job.format)) {
        job.video.vfr = settings_.vFrameMode == 1;  // MP4 and WebM alike (#55)
        if (settings_.targetIdx && fitBitrate_) job.video.bitrate = fitBitrate_;
        job.audio.enabled = ExportAudio();
        job.audio.bitrate = kAudioBitrateOptions[settings_.aBitrateIdx] * 1000;
    } else {
        job.quality = quality;
        if (job.format == Format::Gif) job.lossy = settings_.gifLossy;
    }
    return job;
}

std::wstring App::ContentKey() const {
    return util::Format(L"%p %zu %zu %d,%d,%d,%d %d %d %d %d %d", clip_.get(), inF_, outF_, crop_.left, crop_.top,
                        crop_.right, crop_.bottom, fmt_, settings_.speedIdx, settings_.targetIdx, settings_.gifLossy,
                        settings_.directionIdx) +
           util::Format(L" %d ", captionPos_) + caption_;
}

std::wstring App::EstimateKey() const {
    return ContentKey() + util::Format(L" %d %d %d %d %d %d %u %u %d %d", settings_.scaleIdx, settings_.outFpsIdx,
                                       ExportQuality(), settings_.holdLast, ExportAudio(),
                                       settings_.vCodec, settings_.VideoBitrate(), fitBitrate_, settings_.aBitrateIdx,
                                       settings_.vFpsIdx);
}

// Called while the export panel is up: refits when what the fit depends on changed, else re-estimates (a moment
// after the last change, so dragging a slider doesn't start one per frame).
void App::UpdateEstimate() {
    if (!clip_ || !FormatAvailable((Format)fmt_)) return;
    double now = util::NowSeconds();
    if (settings_.targetIdx && ContentKey() != fitContent_) {
        StartFit();
        return;
    }
    if (fitting_) return;
    std::wstring key = EstimateKey();
    if (key != estKey_) {
        estKey_ = key;
        estValid_ = false;
        estDue_ = now + 0.35;
    }
    if (estDue_ > 0 && now >= estDue_) {
        estDue_ = 0;
        estimator_.Start({MakeJob(settings_.scaleIdx, settings_.outFpsIdx, ExportQuality())}, 0,
                         ++estSerial_, hwnd_, WM_APP_ESTIMATE);
    } else if (estDue_ > 0) {
        RequestWake(estDue_);
    }
}

// Picks size, frame rate and quality (움짤) or the bitrate (MP4, WebM) so the export lands under the target. Starts from
// the best and steps the three down in turns, so none of them drops all the way first.
void App::StartFit() {
    fitContent_ = ContentKey();
    fitMissed_ = false;
    const uint64_t target = fitTargetForTest_ ? fitTargetForTest_ : (uint64_t)kTargetMB[settings_.targetIdx] * 1'000'000;
    const Format f = (Format)fmt_;
    // Sizes and frame rates that make a difference here, largest first.
    std::vector<int> sizes, rates;
    for (int i = 0; i < (int)_countof(kExportSizes); ++i) {
        int w, h;
        OutputSize(w, h, i);
        bool dup = false;
        for (int j : sizes) {
            int w2, h2;
            OutputSize(w2, h2, j);
            dup |= w2 == w;
        }
        if (!dup) sizes.push_back(i);
    }
    std::stable_sort(sizes.begin(), sizes.end(), [&](int a, int b) {
        int wa, ha, wb, hb;
        OutputSize(wa, ha, a);
        OutputSize(wb, hb, b);
        return wa > wb;
    });
    // What each frame-rate choice really gives (a cap above the clip's own rate changes nothing).
    const double speed = kSpeedOptions[settings_.speedIdx];
    auto outRate = [&](int i) {
        if (IsVideoFormat(f)) return EffectiveOutFps(i);
        auto plan = PlanFrames(*clip_, inF_, outF_, EffectiveOutFps(i), speed);
        double ms = 0;
        for (auto& p : plan) ms += p.durationMs;
        return ms > 0 ? plan.size() * 1000.0 / ms : 0.0;
    };
    std::vector<double> rateOf;
    for (int i = 0; i < (int)_countof(kOutFpsOptions); ++i) rateOf.push_back(outRate(i));
    for (int i = 0; i < (int)_countof(kOutFpsOptions); ++i) {
        bool dup = false;
        for (int j : rates) dup |= std::fabs(rateOf[j] - rateOf[i]) < 0.5;
        if (!dup) rates.push_back(i);
    }
    std::stable_sort(rates.begin(), rates.end(), [&](int a, int b) { return rateOf[a] > rateOf[b]; });

    if (IsVideoFormat(f)) {
        // The bitrate decides the size; shrink the picture only when that bitrate would starve it.
        fitting_ = false;
        const double sec = std::max(0.1, PlanVideoFrames(*clip_, inF_, outF_, EffectiveOutFps(), speed).size() /
                                             EffectiveOutFps());
        double audio = ExportAudio() ? kAudioBitrateOptions[settings_.aBitrateIdx] * 1000.0 : 0;
        double video = std::max(2e5, target * 8 * 0.9 / fitCorrection_ / sec - audio);
        int pick = sizes.front();
        for (int s : sizes) {
            int w, h;
            OutputSize(w, h, s);
            pick = s;
            if (video / ((double)w * h * EffectiveOutFps()) >= 0.03) break;  // bits per pixel still watchable
        }
        settings_.scaleIdx = pick;
        fitBitrate_ = (UINT32)std::min(video, 80e6);
        estBytes_ = EstimateBytes(MakeJob(pick, settings_.outFpsIdx, ExportQuality()), std::atomic<bool>{false});
        estValid_ = true;
        estKey_ = EstimateKey();
        fitMissed_ = video <= 2e5 + 1;
        settingsDirty_ = true;
        hasResult_ = false;
        return;
    }

    std::vector<int> qualities = {settings_.quality[fmt_]};
    if (f != Format::Gif)
        for (int q : {70, 55, 40})
            if (q < qualities.back()) qualities.push_back(q);
    fitKnobs_.clear();
    std::vector<ExportJob> jobs;
    size_t si = 0, ri = 0, qi = 0;
    auto add = [&] {
        fitKnobs_.push_back({sizes[si], rates[ri], qualities[qi]});
        jobs.push_back(MakeJob(sizes[si], rates[ri], qualities[qi]));
    };
    add();
    // Frame rate, size, quality, frame rate, ... skipping whichever is already at its lowest.
    for (int step = 0, idle = 0; idle < 3; step = (step + 1) % 3) {
        bool moved = (step == 0 && ri + 1 < rates.size() && ++ri) || (step == 1 && si + 1 < sizes.size() && ++si) ||
                     (step == 2 && qi + 1 < qualities.size() && ++qi);
        if (moved) {
            add();
            idle = 0;
        } else {
            ++idle;
        }
    }
    fitting_ = true;
    estValid_ = false;
    estDue_ = 0;
    // Aim a little under: the estimate is a guess.
    estimator_.Start(std::move(jobs), (uint64_t)(target * 0.9 / fitCorrection_), fitSerial_ = ++estSerial_, hwnd_,
                     WM_APP_ESTIMATE);
}

void App::OnEstimate() {
    Estimator::Result r;
    if (!estimator_.Take(r)) return;
    if (r.serial == fitSerial_ && fitting_) {
        fitting_ = false;
        if (r.chosen >= 0 && r.chosen < (int)fitKnobs_.size()) {
            const FitKnobs& k = fitKnobs_[r.chosen];
            settings_.scaleIdx = k.scaleIdx;
            settings_.outFpsIdx = k.outFpsIdx;
            if (fmt_ < 3) settings_.quality[fmt_] = k.quality;
            fitMissed_ = !r.fits;
            settingsDirty_ = true;
            hasResult_ = false;
        }
        estBytes_ = (uint64_t)(r.bytes * fitCorrection_);
        estValid_ = r.chosen >= 0;
        estKey_ = EstimateKey();
    } else if (r.serial == estSerial_) {
        estBytes_ = (uint64_t)(r.bytes * fitCorrection_);
        estValid_ = r.chosen >= 0;
    }
    Invalidate();
}

void App::StartThumbnails() {
    StopThumbnails();
    if (!clip_) return;
    thumbCancel_ = false;
    thumbs_.clear();
    {
        std::lock_guard lk(thumbMtx_);
        thumbPixels_.clear();
    }
    auto clip = clip_;
    const int n = 14;
    int th = std::max(16, (int)std::lround(56 * Scale()));
    int tw = std::max(8, (int)std::lround(th * clip->Width() / (double)clip->Height()));
    thumbW_ = tw;
    thumbH_ = th;
    HWND notify = hwnd_;
    thumbThread_ = std::thread([this, clip, n, tw, th, notify] {
        auto rd = clip->NewReader();
        AreaResizer rs;
        std::vector<std::vector<uint8_t>> out;
        for (int i = 0; i < n && !thumbCancel_; ++i) {
            int64_t t = (int64_t)((i + 0.5) / n * clip->EndTs());
            const uint8_t* px = rd->Get(clip->QuickIndexNear(t, clip->EndTs() / n / 2));
            if (!px) break;
            std::vector<uint8_t> buf((size_t)tw * th * 4);
            rs.Run(px, clip->Width() * 4, 0, 0, clip->Width(), clip->Height(), buf.data(), tw, th);
            out.push_back(std::move(buf));
        }
        if (thumbCancel_) return;
        {
            std::lock_guard lk(thumbMtx_);
            thumbPixels_ = std::move(out);
        }
        if (notify) PostMessageW(notify, WM_APP_THUMBS, 0, 0);
    });
}

void App::StopThumbnails() {
    thumbCancel_ = true;
    if (thumbThread_.joinable()) thumbThread_.join();
}

// ---------------------------------------------------------------- system

// A combination taken over from another program (#50) is registered like any other while that program leaves it
// free; only while it holds it does the keyboard hook take it.
void App::RegisterRecordHotkey() {
    UnregisterHotKey(hwnd_, kHotkeyRecord);
    hookRecord_ = false;
    if (!capturingHotkey_) {
        hotkeyOk_ = RegisterHotKey(hwnd_, kHotkeyRecord, settings_.hkMods | MOD_NOREPEAT, settings_.hkVk) != FALSE;
        hookRecord_ = !hotkeyOk_ && settings_.hkHook;
    }
    SyncKeyHook();
}

void App::RegisterPauseHotkey() {
    UnregisterHotKey(hwnd_, kHotkeyPause);
    hookPause_ = false;
    if (rec_ == RecState::Recording && !capturingHotkey_) {
        pauseHotkeyOk_ =
            RegisterHotKey(hwnd_, kHotkeyPause, settings_.hkPauseMods | MOD_NOREPEAT, settings_.hkPauseVk) != FALSE;
        hookPause_ = !pauseHotkeyOk_ && settings_.hkPauseHook;
    }
    SyncKeyHook();
}

void App::SyncKeyHook() {
    std::vector<KeyHook::Combo> combos;
    if (hookRecord_) combos.push_back({settings_.hkMods, settings_.hkVk, kHotkeyRecord});
    if (hookPause_) combos.push_back({settings_.hkPauseMods, settings_.hkPauseVk, kHotkeyPause});
    if (hookReplay_) combos.push_back({settings_.rpHkMods, settings_.rpHkVk, kHotkeyReplay});
    if (combos.empty() && !capturingHotkey_) {
        keyHook_.Stop();
        return;
    }
    const bool ok = keyHook_.Set(hwnd_, WM_APP_HOOKED_KEY, std::move(combos), capturingHotkey_ != 0);
    if (hookRecord_) hotkeyOk_ = ok;
    if (hookPause_) pauseHotkeyOk_ = ok;
    if (hookReplay_) replayHotkeyOk_ = ok;
}

// While a new key is being chosen the current ones are released, so pressing them again reaches the window (and
// doesn't start a recording), and the keyboard hook passes on what another program holds (SyncKeyHook).
void App::SetCapturingHotkey(int which) {
    if (which == capturingHotkey_) return;
    capturingHotkey_ = which;
    if (which) hkConflict_ = {};
    RegisterRecordHotkey();
    RegisterPauseHotkey();
    RegisterReplayHotkey();
    Invalidate();
}

App::HotkeySlot App::Hotkey(int which) {
    if (which == kHotkeyPause) return {settings_.hkPauseMods, settings_.hkPauseVk, settings_.hkPauseHook};
    if (which == kHotkeyReplay) return {settings_.rpHkMods, settings_.rpHkVk, settings_.rpHkHook};
    return {settings_.hkMods, settings_.hkVk, settings_.hkHook};
}

void App::CaptureHotkey(UINT mods, UINT vk) {
    auto [hm, hv, hook] = Hotkey(capturingHotkey_);
    // Two of OzMaker's keys can't be the same combination.
    int clash = 0;
    for (int other : {kHotkeyRecord, kHotkeyPause, kHotkeyReplay})
        if (other != capturingHotkey_ && Hotkey(other).mods == mods && Hotkey(other).vk == vk) clash = other;
    if (KeyHook::Usable(mods, vk) && clash) {
        Toast(clash == kHotkeyRecord  ? tr(L"녹화 단축키와 같습니다. 다른 조합을 눌러 주세요.")
              : clash == kHotkeyPause ? tr(L"일시정지 단축키와 같습니다. 다른 조합을 눌러 주세요.")
                                      : tr(L"리플레이 저장 단축키와 같습니다. 다른 조합을 눌러 주세요."),
              true);
    } else if (KeyHook::Usable(mods, vk)) {
        // Try it first; the keys are registered for real once capturing ends.
        if (RegisterHotKey(hwnd_, capturingHotkey_, mods | MOD_NOREPEAT, vk)) {
            UnregisterHotKey(hwnd_, capturingHotkey_);
            hm = mods;
            hv = vk;
            hook = false;
            SetCapturingHotkey(0);
            settings_.Save();
            TrayUpdate();
        } else if (hook && mods == hm && vk == hv) {
            SetCapturingHotkey(0);  // the one already taken over
        } else {
            // Another program holds it. The panel under the row offers to take it over, and only its button does.
            const HotkeyConflict c{capturingHotkey_, mods, vk};
            SetCapturingHotkey(0);
            hkConflict_ = c;
        }
    } else if (mods == MOD_SHIFT) {
        // Shift + a letter would swallow capital letters in every other program.
        Toast(tr(L"Ctrl, Alt, Win 중 하나와 함께 누르세요. (F1~F12는 단독으로도 됩니다)"), true);
    }
    Invalidate();
}

void App::TakeHotkey() {
    const HotkeyConflict c = std::exchange(hkConflict_, {});
    if (!c.which) return;
    auto [hm, hv, hook] = Hotkey(c.which);
    hm = c.mods;
    hv = c.vk;
    hook = true;
    RegisterRecordHotkey();
    RegisterPauseHotkey();
    RegisterReplayHotkey();
    settings_.Save();
    TrayUpdate();
    if ((c.which == kHotkeyRecord && !hotkeyOk_) || (c.which == kHotkeyReplay && settings_.rpOn && !replayHotkeyOk_))
        Toast(tr(L"키보드 후크를 시작하지 못해 이 조합을 쓸 수 없습니다."), true);
    else
        Toast(util::Format(tr(L"이제 OzMaker가 %s을(를) 먼저 받습니다"), util::HotkeyToString(c.mods, c.vk).c_str()));
}

void App::ResetSettings() {
    settings_.Reset();
    nameCustom_ = false;
    timerOpen_ = false;
    SetCapturingHotkey(0);
    hkConflict_ = {};
    RegisterRecordHotkey();
    RegisterPauseHotkey();
    SetReplayOn(settings_.rpOn, false);  // off again, with its key
    ApplyTheme();
    ApplyLanguage();
    settings_.Save();
    settingsDirty_ = false;
    Toast(tr(L"설정을 처음으로 되돌렸습니다"));
}

std::wstring App::HotkeyLabel() const {
    return hotkeyOk_ ? util::HotkeyToString(settings_.hkMods, settings_.hkVk) : tr(L"단축키 없음");
}

void App::TrayAdd() {
    nid_ = {sizeof(nid_)};
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid_.uCallbackMessage = WM_APP_TRAY;
    nid_.hIcon = iconAppSmall_;
    wcscpy_s(nid_.szTip, L"OzMaker");
    Shell_NotifyIconW(NIM_ADD, &nid_);
    nid_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid_);
    TrayUpdate();
}

void App::ApplyTheme() {
    HIGHCONTRASTW hc{sizeof(hc)};
    const bool contrast =
        contrastForTest_ || (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) &&
                             (hc.dwFlags & HCF_HIGHCONTRASTON));
    const bool light =
        settings_.theme == kThemeLight || (settings_.theme == kThemeSystem && util::AppsUseLightTheme());
    theme::P = contrastForTest_ ? theme::HighContrastBlack()
               : contrast       ? theme::HighContrast()
               : light          ? theme::Light()
                                : theme::Dark();
    BOOL motion = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &motion, 0);
    ui_.SetMotion(motion != FALSE);
    overlay_.SetMotion(motion != FALSE);
    if (!hwnd_) return;
    BOOL dark = !theme::P.light;
    if (FAILED(DwmSetWindowAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark))))
        DwmSetWindowAttribute(hwnd_, 19, &dark, sizeof(dark));
    // Windows 11 draws the title bar in the page's color; in a contrast theme, in its own.
    const D2D1_COLOR_F bg = theme::P.Bg;
    COLORREF caption = contrast ? 0xFFFFFFFF /*DWMWA_COLOR_DEFAULT*/
                                : RGB(std::lround(bg.r * 255), std::lround(bg.g * 255), std::lround(bg.b * 255));
    DwmSetWindowAttribute(hwnd_, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
    Invalidate();
}

void App::ApplyLanguage() {
    SetLanguage(settings_.language == kLangKorean    ? Lang::Korean
                : settings_.language == kLangEnglish ? Lang::English
                                                     : SystemLanguage());
    TrayUpdate();
    Invalidate();
}

void App::TrayUpdate() {
    if (!nid_.hWnd) return;
    nid_.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    const RpState rp = ReplayState();
    nid_.hIcon = rec_ == RecState::Recording ? iconRec_ : rp == RpState::Holding ? ReplayTrayIcon() : iconAppSmall_;
    std::wstring hk = util::HotkeyToString(settings_.hkMods, settings_.hkVk);
    std::wstring tip = rec_ == RecState::Recording && recPaused_ ? tr(L"OzMaker · 일시정지 (") +
                                                                         util::HotkeyToString(settings_.hkPauseMods, settings_.hkPauseVk) + tr(L" 이어서)")
                       : rec_ == RecState::Recording ? tr(L"OzMaker · 녹화 중") + (hotkeyOk_ ? L" (" + hk + tr(L" 중지)") : L"")
                       : hotkeyOk_                  ? L"OzMaker · " + hk + tr(L" 녹화")
                                                    : tr(L"OzMaker · 녹화 단축키를 쓸 수 없음");
    if (rec_ == RecState::Idle && rp == RpState::Holding)
        tip += tr(L"\n리플레이 담는 중") +
               (replayHotkeyOk_ ? L" · " + util::HotkeyToString(settings_.rpHkMods, settings_.rpHkVk) + tr(L" 저장") : L"");
    else if (rec_ == RecState::Idle && rp == RpState::Waiting)
        tip += tr(L"\n리플레이 기다리는 중");
    else if (rec_ == RecState::Idle && rp == RpState::Paused)
        tip += tr(L"\n리플레이 멈춤");
    if (rec_ != RecState::Recording && UpdateReady()) tip += tr(L"\n업데이트 준비 완료");
    wcsncpy_s(nid_.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void App::TrayRemove() {
    if (nid_.hWnd) Shell_NotifyIconW(NIM_DELETE, &nid_);
    nid_.hWnd = nullptr;
}

void App::TrayBalloon(const std::wstring& title, const std::wstring& text) {
    if (!nid_.hWnd) return;
    nid_.uFlags = NIF_INFO;
    wcsncpy_s(nid_.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid_.szInfo, text.c_str(), _TRUNCATE);
    nid_.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    nid_.hBalloonIcon = iconApp_;
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
    nid_.szInfo[0] = 0;
}

void App::TrayMenu() {
    HMENU menu = CreatePopupMenu();
    std::wstring hk = util::HotkeyToString(settings_.hkMods, settings_.hkVk);
    AppendMenuW(menu, MF_STRING, 1, tr(L"OzMaker 열기"));
    RecTarget current;
    bool canRecord = CurrentTarget(current, nullptr);
    const wchar_t* what = settings_.source == kSourceDisplay  ? tr(L"선택한 화면 녹화\t")
                          : settings_.source == kSourceRegion ? tr(L"지정한 영역 녹화\t")
                          : MultiPicking() && picks_.size() > 1 ? tr(L"체크한 창 녹화\t")
                                                              : tr(L"선택한 창 녹화\t");
    if (rec_ == RecState::Idle)
        AppendMenuW(menu, MF_STRING | (canRecord ? 0 : MF_GRAYED), 2, (what + hk).c_str());
    else
        AppendMenuW(menu, MF_STRING, 2, (tr(L"녹화 중지\t") + hk).c_str());
    if (rec_ == RecState::Recording)
        AppendMenuW(menu, MF_STRING, 5,
                    ((recPaused_ ? tr(L"이어서 녹화\t") : tr(L"일시정지\t")) +
                     util::HotkeyToString(settings_.hkPauseMods, settings_.hkPauseVk))
                        .c_str());
    if (settings_.rpOn && rec_ == RecState::Idle) {
        const bool can = rpCap_.Running() || !rpClosed_.path.empty();
        AppendMenuW(menu, MF_STRING | (can && !rpSaving_ ? 0 : MF_GRAYED), 6,
                    (tr(L"리플레이 저장\t") + util::HotkeyToString(settings_.rpHkMods, settings_.rpHkVk)).c_str());
    }
    if (UpdateReady())
        AppendMenuW(menu, MF_STRING | (rec_ == RecState::Idle ? 0 : MF_GRAYED), 4,
                    util::Format(tr(L"새 버전 %s으로 업데이트"), updater_.Get().version.c_str()).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 3, tr(L"종료"));
    SetMenuDefaultItem(menu, 1, FALSE);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
        case 1: ShowMain(true); break;
        case 4:
            ShowMain(true);
            InstallUpdate();
            break;
        case 5: TogglePause(); break;
        case 6: ReplaySave(); break;
        case 2:
            if (rec_ == RecState::Idle)
                StartRecording(current, false);
            else
                StopRecording();
            break;
        case 3: AskQuit(false, [this] { PostMessageW(hwnd_, WM_APP_QUIT, 0, 0); }); break;
    }
}
