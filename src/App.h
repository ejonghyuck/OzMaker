#pragma once
#include "Access.h"
#include "AudioPreview.h"
#include "Capture.h"
#include "Exporter.h"
#include "Gfx.h"
#include "KeyHook.h"
#include "Overlays.h"
#include "Settings.h"
#include "Ui.h"
#include "Updater.h"
#include "WindowList.h"

enum class Screen { Home, Editor, Settings };

class AnimClip;

// An item of App::Dropdown's list.
struct MenuItem {
    std::wstring text;
    int value = 0;
    bool line = false;  // a separator above it
    bool enabled = true;
};

// What a recording captures: a window, a whole monitor or a fixed screen area.
struct RecTarget {
    int source = kSourceWindow;
    HWND hwnd = nullptr;
    HMONITOR monitor = nullptr;
    RECT rect{};  // screen pixels: window frame / monitor / region
};

class App {
public:
    int Run(HINSTANCE inst, int show, const std::wstring& cmdLine);

private:
    enum : UINT {
        WM_APP_TRAY = WM_APP + 1,
        WM_APP_CAPTURE_CLOSED,
        WM_APP_EXPORT_DONE,
        WM_APP_THUMBS,
        WM_APP_SHOW,  // = WM_APP + 5, sent by a second instance (main.cpp)
        WM_APP_QUIT,  // = WM_APP + 6, sent by --exit (main.cpp)
        WM_APP_FRAME,
        WM_APP_OPEN,  // a file handed over by a second instance (main.cpp), opened after its SendMessage returns
        WM_APP_ESTIMATE,
        WM_APP_UPDATE,
        WM_APP_ANSWER,  // a question was answered yes: what it asked for runs outside the frame that drew the button
        WM_APP_HOOKED_KEY,  // the keyboard hook took a combination while a hotkey is being chosen (#50)
        WM_APP_ANIM_LOADED,  // a 움짤 file finished decoding (#41)
    };
    static constexpr ULONG_PTR kCopyOpenFile = 0x4F5A4F50;  // WM_COPYDATA tag ('OZOP'), shared with main.cpp
    static_assert(WM_APP_SHOW == WM_APP + 5 && WM_APP_QUIT == WM_APP + 6);
    enum : UINT_PTR { kTimerRec = 1, kTimerCountdown, kTimerRefresh, kTimerUpdate, kTimerMicTest, kTimerAnimLoad };
    enum class RecState { Idle, Countdown, Recording };
    enum : int { kHotkeyRecord = 1, kHotkeyPause = 2 };  // RegisterHotKey ids

    // ---- window plumbing (App.cpp)
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT, WPARAM, LPARAM);
    bool CreateMainWindow(int show);
    void Loop();
    void Render();
    void Invalidate() { dirty_ = true; }
    void RequestWake(double t) { if (t > 0 && (wake_ == 0 || t < wake_)) wake_ = t; }
    float Scale() const { return dpi_ / 96.f; }
    void ShowMain(bool activate);
    void HideMain();
    void MinimizeMain();  // a recording started: to the taskbar (설정 › 녹화 › 녹화를 시작하면, #46)
    // While recording: the window stays out of the recording, and its taskbar button wears a red dot.
    void SetRecordingLook(bool on);
    void RememberPlacement();  // into settings_, for the next start
    void Toast(std::wstring text, bool error = false);
    // A failure the user may not have seen happen (recording, saving, exporting): it stays until closed, and can
    // offer the settings tab that fixes it.
    void Alert(std::wstring text, int settingsTab = -1);
    void SyncIme();
    // The live window preview (DWM thumbnails, which DWM draws above everything in this window).
    void ApplyThumbnail();
    bool AddThumbnail();
    void ReleaseThumbnails();
    void PreviewLayout(RECT& dest, RECT& source);  // where it goes (client pixels), what of the window it shows
    std::vector<RECT> PreviewParts(const RECT& dest);  // dest less whatever floats above it this frame

    // ---- views (AppViews.cpp)
    void DrawFrame(Rect client);
    void DrawHeader(Rect r);
    void DrawTabs(Rect r);  // 녹화 | 편집 in the middle of the header
    void DrawHome(Rect r);
    void DrawSourcePanel(Rect r);
    void DrawWindowList(Rect r);
    void DrawMonitorList(Rect r);
    void DrawRegionPanel(Rect r);
    void DrawCapturePanel(Rect r);
    void DrawTimerChip(Rect& line);  // the record timer's button on the 녹화 tab, at the line's right (#47)
    void AskReset();
    void DrawTimerPopover(Rect client);
    bool TimerFields(Rect r);    // 직접 입력's hours and minutes; true when they changed
    void DrawCapturePreview(Rect r);
    void DrawMonitorLayout(Rect r, const RECT* region, bool pickable);
    void DrawVideoResult(Rect r);
    void DrawEditor(Rect r);
    void DrawEditHome(Rect r);  // the 편집 tab with no file open: where to drop one, what to open
    void DrawFileBar(Rect r);   // over the editor: the open file and 닫기
    void DrawPreview(Rect r);
    void DrawTransport(Rect r);
    void DrawCaptionBar(Rect r);  // under the playback controls while 자막 is on
    void DrawTimeline(Rect r);
    void DrawExportPanel(Rect r);
    void OpenSettings(int tab = -1);  // -1: the tab viewed last
    void DrawSettings(Rect r);
    void SettingsGeneral(Rect& area, bool& changed);
    void SettingsRecord(Rect& area, bool& changed);
    void SettingsVideo(Rect& area, bool& changed);
    void SettingsAudio(Rect& area, bool& changed);
    void SettingsAbout(Rect& area);
    void DrawToast(Rect client);
    void DrawFilesButton(Rect& right);
    void DrawFilesPopover(Rect client);
    void DrawDropCue(Rect client);
    void DrawUpdateButton(Rect& right);
    void DrawUpdatePopover(Rect client);
    // A row of the settings; `below` more of it under the title and control is returned, for the caller to fill.
    Rect SettingRow(Rect& area, std::wstring_view title, std::wstring_view desc, float controlW,
                    const std::function<void(Rect)>& control, float below = 0);
    void SettingSection(Rect& area, std::wstring_view title);
    ID2D1Bitmap* IconFor(const WinInfo& w);

    // ---- sources
    void RefreshWindows();
    void WatchWindows(bool on);
    static void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND h, LONG obj, LONG child, DWORD, DWORD);
    void SelectTarget(HWND h);
    const WinInfo* FindWindowInfo(HWND h) const;
    void SetSource(int source);
    void StartRegionPick();
    void TakeRegionSnapshot();
    RecTarget WindowTarget(HWND h) const;
    RecTarget MonitorTarget(HMONITOR m) const;
    bool CurrentTarget(RecTarget& t, std::wstring* why) const;
    void SourceSize(const RecTarget& t, int& w, int& h) const;
    // What a recording of sw×sh comes out at, and (MP4) whether it is HEVC.
    void PlannedSize(int sw, int sh, int& ow, int& oh, bool* hevc = nullptr) const;

    // ---- recording
    void StartRecording(const RecTarget& target, bool fromHotkey);
    void BeginCapture();
    void StopRecording();
    void OnVideoSaved(const CaptureResult& r);
    void ShowVideo(const CaptureResult& r);
    void EditLastVideo();  // the video card's 편집
    void OnHotkey();
    void TogglePause();
    // Every recording tick: the record timer (#47) and the free space (#17). Stops with a reason when one says so.
    void CheckLimits();
    std::wstring TimerWords(int minutes) const;  // "3시간 20분"
    std::wstring RecordingDir() const;  // where the recording grows: the MP4's folder, or the clip's temp file
    bool NotifyQuietly() const;  // a fullscreen app is in front of a hotkey recording
    void ReportError(const std::wstring& title, const std::wstring& text, int settingsTab = -1);
    bool UseMonitorCapture(HWND target) const;
    void ProbeEncoders() const;
    bool HevcEncoder() const;   // this PC has an HEVC encoder
    bool HevcPlayable() const;  // and plays HEVC (the HEVC Video Extensions)
    // The codec of an MP4 whose longest edge is `edge` before the encoder's limit: 자동 takes HEVC only past what
    // H.264 encoders take (#42).
    bool UseHevc(int edge) const;
    // The 녹화 tab's (or the export's) encoding in one line. bigHevc: 자동 picked HEVC for the size.
    std::wstring VideoSummary(bool hevc, bool bigHevc = false) const;
    std::wstring AudioSummary(int source) const;
    std::wstring MicDeviceName() const;
    std::vector<MenuItem> MicItems();  // Windows' default, then each input device; the value is its micDevices_ index + 1
    std::wstring LanguageName(int choice) const;  // a LanguageChoice
    static std::wstring MbpsText(int kbps);       // "12.5", "20"
    // A drop-down list drawn by the app (#53: not a native menu, so it looks like the rest of it). The field shows
    // `label`; a click opens the items under it as a popover. True in the frame an item was picked: `value` is its
    // value. `name` is what a screen reader calls the field.
    bool Dropdown(UiId id, const Rect& r, std::wstring_view label, std::wstring_view name,
                  std::vector<MenuItem> items, int& value, D2D1_COLOR_F ink);
    void DrawMenuPopover(Rect client);
    void DrawAlphaBacking(const Rect& dst);

    // ---- files: opening videos, recent files, recordings not exported yet
    void OpenMediaFile(const std::wstring& path);
    void OpenAnimFile(const std::wstring& path);
    void ShowAnimProgress();
    void OnAnimLoaded();
    void BrowseForVideo();
    void AddRecent(const std::wstring& path);
    void RefreshRecent();
    void HoldRecording(std::shared_ptr<Clip> clip);
    void DropHeld(const Clip* clip);
    void OpenHeld(size_t i);
    void ConfirmDropHeld(size_t i);  // asks, then lets held_[i] go
    size_t RecoverRecordings();  // at startup: what a crash left behind, back among the unsaved recordings (#24)
    // Asks about unsaved recordings, if there are any, then (on yes) runs `then`.
    void AskQuit(bool restart, std::function<void()> then);
    const wchar_t* Activity() const;  // for crash reports
    bool CanDiscard() const;  // the editor's 버리기 / 삭제: a recording from this run, nothing exporting
    void ConfirmDiscard();
    void DiscardRecording();

    // A question inside the window instead of a MessageBox: a card over the dimmed window, Enter for yes, Esc (or a
    // click outside) for no. The window comes to the front for it.
    struct Question {
        std::wstring title, body, yes;
        bool danger = false;  // yes destroys something: a red button
        std::function<void()> onYes;
        wchar_t glyph = 0;  // the mark beside the title, if not the usual
        // Lists under the title in place of the body: a heading and its lines each.
        std::vector<std::pair<std::wstring, std::vector<std::wstring>>> lists;
    };
    void Ask(Question q);
    void DrawQuestion(Rect client);
    std::optional<Question> question_;
    std::function<void()> answer_;  // onYes of the question just answered, for WM_APP_ANSWER
    uint32_t questionSeq_ = 0;      // a new question fades in again

    // ---- editor
    void OpenEditor(std::shared_ptr<Clip> clip);
    bool OpenVideo(const std::wstring& path);
    void ResumeEditor();  // the 편집 tab
    void CloseEditor();   // the 녹화 tab
    void CloseClip();     // the file bar's 닫기
    std::wstring ClipTitle() const;  // the open file's name: a video's file, a recording's window
    void PauseEditor();
    void AttachReaders();
    void DetachReaders();
    void ShowFrame(size_t index);
    void UploadFrame(const uint8_t* px);
    void SyncAudioPreview();
    void TogglePreviewMute();
    void SetPreviewVolume(int v);
    void SetPlaying(bool play);
    void Seek(size_t index);
    void Shuttle(int dir);  // J (-1) and L (+1)
    void ResetRange();      // R: the whole clip again
    void RestartClock();    // the preview clock from the playhead, now
    void FollowDirection();  // after the format or 재생 방향 changed
    bool PreviewBackwards() const;  // normal playback starts backwards (거꾸로)
    double PreviewRate() const { return shuttle_ ? std::abs(shuttle_) : kSpeedOptions[settings_.speedIdx]; }
    size_t PlayheadFromClock();
    int64_t RangeEndTs() const;
    void OutputSize(int& w, int& h, int scaleIdx = -1) const;  // -1 = the current setting
    double EffectiveOutFps(int outFpsIdx = -1) const;
    ExportJob MakeJob(int scaleIdx, int outFpsIdx, int quality) const;  // without a path
    std::wstring EstimateKey() const;  // everything the size of the export depends on
    std::wstring ContentKey() const;   // what a size fit depends on (not the settings it picks)
    void UpdateEstimate();
    void StartFit();
    void OnEstimate();
    bool ExportAudio() const;
    int ExportQuality() const;  // the 품질 of the format chosen: the 움짤 one, WebM's (#40), 0 for MP4
    // MP4 of a video: why the range has to be encoded again; "" when it is copied as it is (Mp4Cut.h); null when
    // copying can't be done here at all.
    const wchar_t* CopyBlocker() const;
    VideoConfig VideoSettings(int fps) const;
    std::wstring ExportName(Format f) const;
    // settings_.nameTemplate filled in for a clip of `what` (a window's title, "화면 1"); without findNumber, {번호}
    // is 001 without looking in the save folder (the settings' example).
    std::wstring FileStem(const std::wstring& what, bool findNumber = true) const;
    static std::wstring FillName(std::wstring_view pattern, const std::wstring& what, const SYSTEMTIME& t, int number);
    void StartExport(bool saveAs);
    void OnExportDone();
    void StartThumbnails();
    void StopThumbnails();
    void ResetCrop();
    void ApplyCropAspect();

    // ---- system
    void RegisterRecordHotkey();
    void RegisterPauseHotkey();  // only while recording, so the keys stay free for other programs otherwise
    void SetCapturingHotkey(int which);  // kHotkeyRecord / kHotkeyPause, 0 = none
    void CaptureHotkey(UINT mods, UINT vk);  // a combination pressed while choosing one
    void TakeHotkey();  // the conflict panel's 그래도 OzMaker가 쓰기 (#50)
    void SyncKeyHook();  // the keyboard hook on for what needs it, off otherwise
    void ResetSettings();  // 설정 › 일반 › 설정 초기화, once asked (#49)
    std::wstring HotkeyLabel() const;  // the record hotkey, or a note that there is none
    void TrayAdd();
    void TrayUpdate();
    // The colors (설정 › 일반 › 테마, Windows' light or dark mode and its contrast theme) and Windows' animation
    // setting, with the title bar to match; again whenever Windows' settings change.
    void ApplyTheme();
    void ApplyLanguage();  // 설정 › 일반 › 언어
    void TrayRemove();
    void TrayMenu();
    void TrayBalloon(const std::wstring& title, const std::wstring& text);

    // ---- updates (#31)
    void CheckForUpdate();  // when one is due (timer), or now from the settings
    bool UpdateReady() const;
    void InstallUpdate();   // asks about unsaved recordings, then RestartIntoUpdate
    void RestartIntoUpdate();  // swap the exe and restart into it

    // ---- test helpers (SelfTest)
    // look: "" (dark, Korean), "light", "contrast" (High Contrast Black) or "en" (English).
    int RunScreenshots(const std::wstring& dir, const std::wstring& look);
    int RunSelfTest(const std::wstring& dir);
    int RunCrashTest(const std::vector<std::wstring>& args);
    int RunRecordTest(const std::wstring& args);
    int RunAnalyzeTest(const std::wstring& args);
    int RunVideoTest(const std::wstring& dir, int w, int h);
    int RunEncoderTest(const std::wstring& dir);
    int RunEditorTest(const std::wstring& path, const std::wstring& dir);
    int RunRegionTest(const std::wstring& dir);
    int RunPreviewTest(const std::wstring& dir);
    int RunTestWindow(double seconds);
    int RunUpdateTest(const std::wstring& dir, const std::wstring& base);
    int RunReadmeMedia(const std::wstring& dir);
    void DrawPreviewStandIn(ID2D1DeviceContext* dc);
    std::shared_ptr<FrameStore> MakeSyntheticClip(int w, int h, int frames, double fps, bool finish = true);

    // ---- state
    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    Gfx gfx_;
    Ui ui_;
    Access access_;  // what screen readers see of ui_
    UiInput input_;
    bool dirty_ = true, tracking_ = false, quitting_ = false;
    bool imeOn_ = false;  // the window has an IME context (a text field has the focus)
    double wake_ = 0;
    Screen screen_ = Screen::Home;
    Screen settingsReturn_ = Screen::Home;
    Settings settings_;
    bool settingsDirty_ = false;  // saved once the mouse is released (sliders change every frame)

    std::vector<WinInfo> windows_;
    HWND target_ = nullptr;
    float listScroll_ = 0;
    std::vector<MonitorDesc> monitors_;
    RegionPicker regionPicker_;
    ComPtr<ID2D1Bitmap1> snapBmp_;  // region preview, taken right after picking (our window hidden)
    RECT snapRect_{};
    std::unordered_map<HWND, ComPtr<ID2D1Bitmap1>> icons_;
    uint32_t iconGen_ = 0;
    // One thumbnail of target_; while something floats over the preview, several, each showing the part of the
    // window around it (source rects set: thumbSplit_).
    std::vector<HTHUMBNAIL> thumb_;
    HWND thumbSrc_ = nullptr;
    bool thumbSplit_ = false;
    bool thumbWanted_ = false;
    Rect thumbArea_;

    Capture capture_;
    RecState rec_ = RecState::Idle;
    RecTarget recTarget_;
    bool recFromHotkey_ = false, recFullscreen_ = false, recVideo_ = false;
    std::wstring recNotice_;
    std::wstring stopReason_;  // why the recording stopped by itself (length limit, disk space), shown after it
    bool recPaused_ = false;
    double nextDiskCheck_ = 0;
    std::wstring recName_;  // what is being recorded, in words (window title, "화면 1", "영역")
    std::wstring clipName_;  // the same for the clip in the editor (export file names)
    CaptureResult lastVideo_;
    bool showVideo_ = false;
    mutable bool encProbed_ = false;
    mutable std::wstring encName_[2][2];  // [hevc][hardware]
    mutable int hevcPlayable_ = -1;       // probed once; -1 = not yet
    int timerFiredFor_ = 0;  // the timer length (s) whose time came in this recording: once per length
    bool timerOpen_ = false;  // the timer's popover on the 녹화 tab
    Rect timerChip_;
    std::wstring timerText_[2];  // 직접 입력's hours and minutes as typed
    // The open Dropdown: its field, its items, and the value picked from it for the field to take next frame.
    struct OpenMenu {
        UiId owner = 0;
        Rect anchor;
        std::wstring name;
        std::vector<MenuItem> items;
        int value = 0;
        bool seen = false;  // its field was drawn this frame (it closes when the field goes)
        bool keys = false;  // opened from the keyboard: the focus ring goes into the list
    } menu_;
    UiId menuPicked_ = 0;
    int menuPickedValue_ = 0;
    std::wstring brText_;  // 설정 › 동영상 비트레이트 직접 입력, as typed
    std::wstring brNote_;  // what became of a number out of range
    bool brTypingWas_ = false;  // the field had the focus last frame: it lets go when that changes
    int countdownLeft_ = 0;
    RecOverlay overlay_;
    PickerOverlay picker_;

    // editor
    std::shared_ptr<Clip> clip_;
    std::unique_ptr<FrameReader> reader_;  // clips with fast random access
    FrameFetcher fetcher_;                 // videos: decoded off the UI thread
    std::vector<uint8_t> framePx_;         // last frame from the fetcher
    ComPtr<ID2D1Bitmap1> frameBmp_;
    uint32_t frameBmpGen_ = 0;
    size_t shown_ = SIZE_MAX, playhead_ = 0, inF_ = 0, outF_ = 0;
    bool playing_ = false;
    bool playBack_ = false;  // the preview runs backwards: 거꾸로, a boomerang's way back, J
    int shuttle_ = 0;        // J / K / L: the preview's speed while it plays, - backwards; 0 = normal playback
    double playWall_ = 0;
    int64_t playTs_ = 0;
    uint32_t playEpoch_ = 0, audioEpoch_ = 0;  // bumped whenever the preview clock jumps
    AudioPreview audioPreview_;
    bool previewMute_ = false;
    int fmt_ = 0;  // export format (Format)
    bool exportAudio_ = true;
    RECT crop_{};
    bool cropMode_ = false;
    int cropAspect_ = 0;
    int cropDrag_ = -1;
    std::shared_ptr<const mp4::CutSource> cutSource_;  // a video that can be cut without encoding it again
    // A 움짤 file decoding on a worker (#41). Dropping it stops the worker and waits for it.
    struct AnimLoad {
        std::wstring path;
        std::thread thread;
        std::atomic<bool> cancel{false}, done{false};
        std::atomic<float> progress{0};
        std::shared_ptr<AnimClip> clip;
        std::wstring err;
        ~AnimLoad() {
            cancel = true;
            if (thread.joinable()) thread.join();
        }
    };
    std::unique_ptr<AnimLoad> animLoad_;
    ComPtr<ID2D1BitmapBrush1> checker_;  // under an opened 움짤's transparent parts
    UINT checkerGen_ = 0;
    std::wstring caption_;      // 자막: one line over the whole clip (#22), kept for the clip open
    int captionPos_ = 0;        // caption::Pos
    bool captionOpen_ = false;  // the caption bar is up
    RECT cropStart_{};
    float dragX_ = 0, dragY_ = 0;
    Rect previewDst_;
    bool scrubbing_ = false, resumeAfterScrub_ = false;

    std::thread thumbThread_;
    std::atomic<bool> thumbCancel_{false};
    std::mutex thumbMtx_;
    std::vector<std::vector<uint8_t>> thumbPixels_;
    int thumbW_ = 0, thumbH_ = 0;
    std::vector<ComPtr<ID2D1Bitmap1>> thumbs_;

    // Recordings stay here until exported, so a new one doesn't silently replace them.
    struct HeldClip {
        std::shared_ptr<Clip> clip;
        double when = 0;
        std::wstring name;
    };
    static constexpr size_t kMaxHeld = 5;
    std::vector<HeldClip> held_;  // newest first
    struct RecentFile {
        std::wstring path;
        uint64_t bytes = 0;
        double age = 0;  // seconds since written
        bool video = false;
    };
    std::vector<RecentFile> recentFiles_;  // existing files of settings_.recent, refreshed when the list opens
    bool filesOpen_ = false;
    Rect filesBtn_;
    float filesScroll_ = 0;
    std::wstring pendingOpen_;
    float editListScroll_[2]{};  // the 편집 tab's lists: 저장하지 않은 녹화, 최근 파일

    // Files dragged over the window (OLE): what dropping them would do, shown over the whole window before the drop.
    class DropTarget;
    enum class DragKind { None, Video, Anim, Other, Busy };
    DragKind ClassifyDrop(const std::wstring& path) const;
    bool DragOpens() const { return dragKind_ == DragKind::Video || dragKind_ == DragKind::Anim; }
    ComPtr<IDropTarget> MakeDropTarget();
    ComPtr<IDropTarget> dropTarget_;
    DragKind dragKind_ = DragKind::None;
    std::wstring dragName_;

    // Export size estimate and "용량 맞추기" (EstimateBytes on the estimator's thread).
    struct FitKnobs {
        int scaleIdx, outFpsIdx, quality;
    };
    Estimator estimator_;
    uint32_t estSerial_ = 0, fitSerial_ = 0;
    std::wstring estKey_, fitContent_;
    double estDue_ = 0;
    uint64_t estBytes_ = 0;
    bool estValid_ = false, fitting_ = false, fitMissed_ = false;
    std::vector<FitKnobs> fitKnobs_;
    UINT32 fitBitrate_ = 0;     // MP4 and WebM under a target: the video bitrate that fits
    double fitCorrection_ = 1;  // actual / estimated, after an export came out over the target
    uint64_t exportEstimate_ = 0;
    double exportStarted_ = 0;  // util::NowSeconds() when the export started: WebM's time left (#40)
    uint64_t fitTargetForTest_ = 0;  // self-test: a target below the smallest preset
    bool testDeleteFiles_ = false;   // self-test: 삭제 deletes its copy outright, not into the Recycle Bin
    bool portableForTest_ = false;   // screenshots: the settings as a portable copy shows them
    bool contrastForTest_ = false;   // screenshots: as in Windows' "High Contrast Black"
    double recElapsedForTest_ = -1;  // README pictures: the time a made-up recording shows (-1: the real one)
    int hevcForTest_ = -1;  // screenshots: 0 no HEVC, 1 HEVC that plays, 2 an encoder without the extension
    SIZE sourceForTest_{};  // screenshots: the size the target has (a 5K screen), zero: its own
    double RecElapsed() const { return recElapsedForTest_ >= 0 ? recElapsedForTest_ : capture_.Elapsed(); }

    Updater updater_;
    bool updateOpen_ = false;  // the popover under the update button
    Rect updateBtn_;

    Exporter exporter_;
    ExportResult lastResult_;
    bool hasResult_ = false;
    bool resultOriginal_ = false;  // the card shows the recording itself, not an export

    std::wstring toast_;
    bool toastError_ = false;
    bool toastSticky_ = false;  // an Alert: stays until closed
    int toastTab_ = -1;         // an Alert's "… 설정 열기" button: the settings tab it opens (-1: none)
    double toastUntil_ = 0;
    Rect toastRect_;  // where the toast was drawn last frame: the page takes no input under an Alert

    int capturingHotkey_ = 0;  // the hotkey being chosen in the settings
    bool hotkeyOk_ = true, pauseHotkeyOk_ = true;  // it works: registered, or (taken over) through the hook
    // A combination another program holds, pressed while choosing: the panel under the row offers to take it (#50).
    struct HotkeyConflict {
        int which = 0;  // kHotkeyRecord / kHotkeyPause, 0 = none
        UINT mods = 0, vk = 0;
    } hkConflict_;
    KeyHook keyHook_;
    bool hookRecord_ = false, hookPause_ = false;  // taken over and refused by RegisterHotKey: the hook has them
    ComPtr<ITaskbarList3> taskbar_;
    std::vector<HWINEVENTHOOK> winHooks_;
    bool windowsDirty_ = false;  // a window event came in; kTimerRefresh refreshes the list once for the burst
    std::vector<AudioCapture::Device> micDevices_;  // 설정 › 소리, listed when the tab is opened
    bool micListed_ = false;
    // 설정 › 소리 "마이크 확인": the chosen mic opened for up to 10 s to show its level. Nothing is recorded.
    void StartMicTest();
    void StopMicTest();
    std::atomic<float> micPeak_{0};  // loudest sample since the meter last looked, 0..1, before the mic volume
    AudioMonitor micMonitor_;        // plays the mic back while testing (settings_.micMonitor)
    AudioCapture micTest_;           // after micPeak_ and micMonitor_: its thread writes to them until it is destroyed
    void SyncMicMonitor();           // micMonitor_ on while testing with settings_.micMonitor
    float micLevel_ = 0;             // the meter: 0..1 over -60..0 dB, falling back slowly
    double micTestUntil_ = 0, micLevelAt_ = 0;
    float micTestShow_ = -1;         // screenshots: a made-up level, no device opened
    bool testWindowsOnly_ = false;  // test runs list only "OzMaker Test Window": other titles are the user's business
    float settingsScroll_ = 0, settingsContentH_ = 0;
    bool nameCustom_ = false;  // 파일 이름 › 직접 입력 picked, even while the pattern is one of the three
    float exportScroll_ = 0, exportContentH_ = 0;
    // The export panel's frame count and length, kept while the range, rate and speed stay: working them out walks
    // every frame of the range, and the panel is drawn many times a second.
    struct PlanSummary {
        const Clip* clip = nullptr;
        size_t count = 0, first = 0, last = 0;
        double fps = 0, speed = 0;
        bool video = false;  // MP4 or WebM
        int direction = 0;
        size_t frames = 0;
        double ms = 0;
    };
    PlanSummary planCache_;
    bool autostart_ = false;
    NOTIFYICONDATAW nid_{};
    HICON iconApp_ = nullptr, iconAppSmall_ = nullptr, iconRec_ = nullptr;
    UINT taskbarCreated_ = 0;
};
