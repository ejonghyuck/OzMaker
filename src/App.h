#pragma once
#include "AudioPreview.h"
#include "Capture.h"
#include "Exporter.h"
#include "Gfx.h"
#include "Overlays.h"
#include "Settings.h"
#include "Ui.h"
#include "WindowList.h"

enum class Screen { Home, Editor, Settings };

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
    };
    static_assert(WM_APP_SHOW == WM_APP + 5 && WM_APP_QUIT == WM_APP + 6);
    enum : UINT_PTR { kTimerRec = 1, kTimerCountdown, kTimerRefresh };
    enum class RecState { Idle, Countdown, Recording };

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
    void RememberPlacement();  // into settings_, for the next start
    void Toast(std::wstring text, bool error = false);
    void ApplyThumbnail();

    // ---- views (AppViews.cpp)
    void DrawFrame(Rect client);
    void DrawHeader(Rect r);
    void DrawHome(Rect r);
    void DrawSourcePanel(Rect r);
    void DrawWindowList(Rect r);
    void DrawMonitorList(Rect r);
    void DrawRegionPanel(Rect r);
    void DrawCapturePanel(Rect r);
    void DrawCapturePreview(Rect r);
    void DrawMonitorLayout(Rect r, const RECT* region, bool pickable);
    void DrawVideoResult(Rect r);
    void DrawEditor(Rect r);
    void DrawPreview(Rect r);
    void DrawTransport(Rect r);
    void DrawTimeline(Rect r);
    void DrawExportPanel(Rect r);
    void DrawSettings(Rect r);
    void DrawToast(Rect client);
    void SettingRow(Rect& area, std::wstring_view title, std::wstring_view desc, float controlW,
                    const std::function<void(Rect)>& control);
    void SettingSection(Rect& area, std::wstring_view title);
    ID2D1Bitmap* IconFor(const WinInfo& w);

    // ---- sources
    void RefreshWindows();
    void SelectTarget(HWND h);
    const WinInfo* FindWindowInfo(HWND h) const;
    void SetSource(int source);
    void StartRegionPick();
    void TakeRegionSnapshot();
    RecTarget WindowTarget(HWND h) const;
    RecTarget MonitorTarget(HMONITOR m) const;
    bool CurrentTarget(RecTarget& t, std::wstring* why) const;
    void SourceSize(const RecTarget& t, int& w, int& h) const;
    void PlannedSize(int sw, int sh, int& ow, int& oh) const;

    // ---- recording
    void StartRecording(const RecTarget& target, bool fromHotkey);
    void BeginCapture();
    void StopRecording();
    void OnVideoSaved(const CaptureResult& r);
    void OnHotkey();
    bool NotifyQuietly() const;  // a fullscreen app is in front of a hotkey recording
    void ReportError(const std::wstring& title, const std::wstring& text);
    bool UseMonitorCapture(HWND target) const;
    void ProbeEncoders();
    std::wstring VideoSummary() const;
    std::wstring AudioSummary(int source) const;

    // ---- editor
    void OpenEditor(std::shared_ptr<Clip> clip);
    bool OpenVideo(const std::wstring& path);
    void ResumeEditor();
    void CloseEditor();
    void AttachReaders();
    void DetachReaders();
    void ShowFrame(size_t index);
    void UploadFrame(const uint8_t* px);
    void SyncAudioPreview();
    void SetPlaying(bool play);
    void Seek(size_t index);
    size_t PlayheadFromClock();
    int64_t RangeEndTs() const;
    void OutputSize(int& w, int& h) const;
    double EffectiveOutFps() const;
    bool ExportAudio() const;
    VideoConfig VideoSettings(int fps) const;
    std::wstring ExportName(Format f) const;
    void StartExport(bool saveAs);
    void OnExportDone();
    void StartThumbnails();
    void StopThumbnails();
    void ResetCrop();
    void ApplyCropAspect();

    // ---- system
    void RegisterRecordHotkey();
    void SetCapturingHotkey(bool on);
    std::wstring HotkeyLabel() const;  // the record hotkey, or a note that there is none
    void TrayAdd();
    void TrayUpdate();
    void TrayRemove();
    void TrayMenu();
    void TrayBalloon(const std::wstring& title, const std::wstring& text);

    // ---- test helpers (SelfTest)
    int RunScreenshots(const std::wstring& dir);
    int RunSelfTest(const std::wstring& dir);
    int RunRecordTest(const std::wstring& args);
    int RunAnalyzeTest(const std::wstring& args);
    int RunVideoTest(const std::wstring& dir, int w, int h);
    int RunEditorTest(const std::wstring& path, const std::wstring& dir);
    std::shared_ptr<FrameStore> MakeSyntheticClip(int w, int h, int frames, double fps);

    // ---- state
    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    Gfx gfx_;
    Ui ui_;
    UiInput input_;
    bool dirty_ = true, tracking_ = false, quitting_ = false;
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
    HTHUMBNAIL thumb_ = nullptr;
    HWND thumbSrc_ = nullptr;
    bool thumbWanted_ = false;
    Rect thumbArea_;

    Capture capture_;
    RecState rec_ = RecState::Idle;
    RecTarget recTarget_;
    bool recFromHotkey_ = false, recFullscreen_ = false, recVideo_ = false;
    std::wstring recNotice_;
    CaptureResult lastVideo_;
    bool showVideo_ = false;
    bool encProbed_ = false;
    std::wstring encName_[2][2];  // [hevc][hardware]
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

    Exporter exporter_;
    ExportResult lastResult_;
    bool hasResult_ = false;
    bool resultOriginal_ = false;  // the card shows the recording itself, not an export

    std::wstring toast_;
    bool toastError_ = false;
    double toastUntil_ = 0;

    bool capturingHotkey_ = false, hotkeyOk_ = true;
    float settingsScroll_ = 0, settingsContentH_ = 0;
    NOTIFYICONDATAW nid_{};
    HICON iconApp_ = nullptr, iconAppSmall_ = nullptr, iconRec_ = nullptr;
    UINT taskbarCreated_ = 0;
};
