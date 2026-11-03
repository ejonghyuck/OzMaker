#pragma once

inline constexpr int kFpsOptions[] = {10, 15, 20, 30, 50, 60};
inline constexpr int kSizeOptions[] = {0, 1280, 960, 720, 480};  // longest edge, 0 = original
// Export size: a scale of the (cropped) clip, or a width in pixels. Never enlarges.
struct ExportSize {
    int percent, width;
};
inline constexpr ExportSize kExportSizes[] = {{100, 0}, {75, 0}, {50, 0}, {33, 0}, {0, 640}, {0, 480}};
inline constexpr int kLoopOptions[] = {0, 1, 3};  // times a 움짤 plays, 0 = forever
inline constexpr int kTargetMB[] = {0, 5, 10, 15, 20};  // "용량 맞추기", 0 = off
inline constexpr double kHoldLastMs = 1000;       // "끝에서 멈춤": the last frame stays this much longer
// File name patterns offered in the settings; settings.ini may hold any other.
inline constexpr const wchar_t* kNameTemplates[] = {L"OzMaker_{날짜}", L"{창 이름}_{날짜}", L"{날짜}"};
inline constexpr int kOutFpsOptions[] = {0, 30, 25, 20, 15, 10};  // 0 = as recorded
inline constexpr double kSpeedOptions[] = {0.5, 1.0, 1.5, 2.0};
// 화질 in the export panel (고화질, 보통, 저용량): the size (kExportSizes) and frame rate (kOutFpsOptions) for 움짤 and
// for MP4, and the GIF lossy level.
struct ExportPreset {
    int animSize, animFps, videoSize, videoFps, gifLossy;
};
inline constexpr ExportPreset kExportPresets[] = {{0, 0, 0, 0, 1}, {4, 3, 2, 1, 1}, {5, 4, 4, 1, 2}};
inline constexpr size_t kMaxRecent = 12;

// MP4 recording
inline constexpr int kVideoFpsOptions[] = {24, 30, 60};
inline constexpr int kVideoResOptions[] = {0, 2160, 1440, 1080, 720};       // short edge of the 16:9 box, 0 = original
inline constexpr int kVideoBitrateOptions[] = {0, 6, 10, 16, 25, 40, 60};   // Mbps, 0 = automatic
inline constexpr int kKeyframeOptions[] = {1, 2, 5};                        // seconds
inline constexpr int kPresetOptions[] = {0, 50, 100};                       // quality vs speed
inline constexpr int kAudioBitrateOptions[] = {96, 128, 160, 192};          // kbps (AAC)
inline constexpr int kAudioRateOptions[] = {44100, 48000};

// Recording limits (#17)
inline constexpr int kMaxLengthOptions[] = {0, 10, 30, 60};  // minutes, 0 = no limit
inline constexpr uint64_t kLowDiskWarn = 1ull << 30;        // warn before recording below this much free space
inline constexpr uint64_t kLowDiskStop = 300ull << 20;      // and stop (saving what there is) below this

enum Source { kSourceWindow = 0, kSourceDisplay = 1, kSourceRegion = 2 };
enum Output { kOutputClip = 0, kOutputVideo = 1 };
enum CaptureMode { kCaptureAuto = 0, kCaptureWindow = 1, kCaptureDisplay = 2 };
enum OverlayMode { kOverlayAlways = 0, kOverlayExceptFullscreen = 1, kOverlayNever = 2 };
enum SettingsTab { kTabGeneral = 0, kTabRecord, kTabVideo, kTabAudio, kTabAbout, kTabCount };

struct Settings {
    // what and how
    int source = kSourceWindow;
    int output = kOutputClip;
    std::wstring monitor;  // device name of the chosen display (MONITORINFOEX::szDevice)
    RECT region{};         // screen pixels; empty = not chosen yet

    // recording
    int fpsIdx = 3;
    int sizeIdx = 2;
    bool cursor = true;
    bool clientOnly = true;
    bool clickFx = false;  // 설정 › 녹화 "클릭 강조" (#21)
    int captureMode = kCaptureAuto;
    int countdown = 1;  // 0 = none, 1 = 3 s, 2 = 5 s
    int overlayMode = kOverlayExceptFullscreen;
    bool sounds = true;
    bool closeToTray = true;
    bool autoUpdate = true;  // check GitHub for a new version (#31)
    bool trayHintShown = false;
    int settingsTab = kTabGeneral;  // the settings tab viewed last
    RECT window{};  // main window's restored position (workspace pixels); empty = centre on the primary monitor
    bool windowMax = false;
    UINT hkMods = MOD_CONTROL | MOD_ALT;
    UINT hkVk = 'R';
    UINT hkPauseMods = MOD_CONTROL | MOD_ALT;  // pause / resume, registered only while recording
    UINT hkPauseVk = 'P';
    int maxLengthIdx = 0;  // kMaxLengthOptions
    std::wstring outDir;

    // video (MP4)
    int vCodec = 0;  // 0 = H.264, 1 = HEVC
    bool vHardware = true;
    int vFpsIdx = 2;
    int vResIdx = 0;
    int vBitrateIdx = 0;
    int vRateCtrl = 0;  // 0 = VBR, 1 = CBR
    int vKeyIdx = 1;
    int vPresetIdx = 1;
    bool audio = true;
    int aBitrateIdx = 2;
    int aRateIdx = 1;
    bool mic = false;        // the microphone mixed into the MP4's sound (#21)
    std::wstring micDevice;  // endpoint ID, empty = Windows' default input
    int micVolume = 100;     // %, 0-200
    bool micMonitor = true;  // 마이크 확인 plays the mic back while it runs

    // export
    int format = 0;                     // 움짤 format (GIF, WebP, AVIF); MP4 is picked per clip
    int quality[3] = {85, 80, 60};      // per 움짤 format
    int gifLossy = 1;                   // GIF lossy compression: 0 off, 1 light, 2 strong
    int scaleIdx = 0;  // kExportSizes
    int outFpsIdx = 0;
    int speedIdx = 1;
    int loopIdx = 0;   // kLoopOptions
    int previewVolume = 100;  // %, 0-100: the editor's sound preview, not the saved file
    int targetIdx = 0;  // kTargetMB
    bool holdLast = false;
    int exportPreset = 1;        // kExportPresets; -1 = 직접 설정 (the values were picked by hand)
    bool exportDetails = false;  // the export panel's 자세히 설정 is open
    std::wstring nameTemplate = kNameTemplates[0];  // {날짜}/{date}, {창 이름}/{window}

    std::vector<std::wstring> recent;  // saved and opened files, newest first
    std::wstring crashSeen;            // the newest crash report the user was told about (crash::Report::name)

    void AddRecent(const std::wstring& path);
    void Load();
    void Save() const;
    // Sets the size, frame rate and GIF lossy level to kExportPresets[i] (exportPreset itself stays).
    void ApplyPreset(int i, bool video);
    // The preset the size and frame rate (and for GIF the lossy level) match, -1 if none.
    int MatchPreset(bool video, bool gif) const;

    // Where the settings live: OZMAKER_SETTINGS (test runs), settings.ini next to the exe (portable mode, #25), or
    // %APPDATA%\OzMaker\settings.ini.
    static std::wstring FilePath();
    static std::wstring Dir();  // the folder FilePath is in; crash reports go there too
    static bool Portable();     // decided once, at startup
    static std::wstring PortableFile(const std::wstring& exeDir);  // exeDir\settings.ini if it exists, else empty

    bool HasRegion() const { return region.right - region.left >= 16 && region.bottom - region.top >= 16; }
    int CountdownSeconds() const { return countdown == 1 ? 3 : countdown == 2 ? 5 : 0; }
};
