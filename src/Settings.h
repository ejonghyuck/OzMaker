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
// File name patterns offered in the settings; '직접 입력' there takes any other.
inline constexpr const wchar_t* kNameTemplates[] = {L"OzMaker_{날짜}", L"{창 이름}_{날짜}", L"{날짜}"};
// The tokens of a pattern, Korean and English: either works whatever the UI's language. {날짜} is date and time.
enum NameToken { kTokDate, kTokYear, kTokMonth, kTokDay, kTokHour, kTokMinute, kTokSecond, kTokWindow, kTokNumber,
                 kTokCount };
inline constexpr const wchar_t* kNameTokens[kTokCount][2] = {
    {L"{날짜}", L"{date}"},  {L"{연}", L"{year}"},    {L"{월}", L"{month}"},       {L"{일}", L"{day}"},     {L"{시}", L"{hour}"},
    {L"{분}", L"{minute}"}, {L"{초}", L"{second}"}, {L"{창 이름}", L"{window}"}, {L"{번호}", L"{number}"}};
// The token at p[i] (-1: none), and its length.
inline int NameTokenAt(std::wstring_view p, size_t i, size_t& len) {
    for (int k = 0; k < kTokCount; ++k)
        for (std::wstring_view t : kNameTokens[k])
            if (p.compare(i, t.size(), t) == 0) {
                len = t.size();
                return k;
            }
    return -1;
}
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
// Mbps, 0 = automatic, -1 = 직접 입력 (#53: Settings::vBitrateKbps)
inline constexpr int kVideoBitrateOptions[] = {0, 6, 10, 16, 25, 40, 60, -1};
inline constexpr int kVideoBitrateCustom = 7;
inline constexpr int kVideoBitrateMinKbps = 500, kVideoBitrateMaxKbps = 150000;  // 0.5 – 150 Mbps
inline constexpr int kKeyframeOptions[] = {1, 2, 5};                        // seconds
inline constexpr int kPresetOptions[] = {0, 50, 100};                       // quality vs speed
inline constexpr int kAudioBitrateOptions[] = {96, 128, 160, 192};          // kbps (AAC)
inline constexpr int kAudioRateOptions[] = {44100, 48000};

// Recording limits (#17). The record timer (#47): 끔, 30분, 1시간, 2시간, 직접 입력 (Settings::timerMinutes).
inline constexpr int kTimerOptions[] = {0, 30, 60, 120, -1};  // minutes, 0 = off, -1 = timerMinutes
inline constexpr int kTimerCustom = 4;                         // 직접 입력
inline constexpr int kTimerMaxMinutes = 24 * 60;
inline constexpr uint64_t kLowDiskWarn = 1ull << 30;        // warn before recording below this much free space
inline constexpr uint64_t kLowDiskStop = 300ull << 20;      // and stop (saving what there is) below this

// 순간 리플레이 (#20): how much of what just went by it holds, and how long the held span of a program that closed
// stays to be saved.
inline constexpr int kReplayLenOptions[] = {15, 30, 60, 120, 300};  // seconds
inline constexpr int kReplayKeepClosed = 300;                         // seconds
inline constexpr size_t kMaxReplayTargets = 32;
inline constexpr size_t kMaxRecWindows = 4;  // 여러 창 녹화 (#48): checked at once
// What 순간 리플레이 waits for (the 리플레이 tab's list): a program, by its exe, or a whole monitor. No window titles.
struct ReplayTarget {
    bool monitor = false;
    std::wstring path;  // the program's exe; a monitor's device name (MONITORINFOEX::szDevice)
    std::wstring name;  // as shown: the program's description, else its exe's name; "" for a monitor
    bool on = true;     // switched on in the list
};

enum Source { kSourceWindow = 0, kSourceDisplay = 1, kSourceRegion = 2 };
enum Output { kOutputClip = 0, kOutputVideo = 1 };
enum CaptureMode { kCaptureAuto = 0, kCaptureWindow = 1, kCaptureDisplay = 2 };
enum OverlayMode { kOverlayAlways = 0, kOverlayExceptFullscreen = 1, kOverlayNever = 2 };
// The numbers are saved (SettingsTab); the settings show 리플레이 after 녹화.
enum SettingsTab { kTabGeneral = 0, kTabRecord, kTabVideo, kTabAudio, kTabAbout, kTabReplay, kTabCount };
enum ThemeChoice { kThemeSystem = 0, kThemeDark, kThemeLight };
enum LanguageChoice { kLangSystem = 0, kLangKorean, kLangEnglish };
enum StartHide { kHideToTray = 0, kHideMinimize };  // 설정 › 녹화 "녹화를 시작하면" (#46)
enum DeleteMode { kDeleteToBin = 0, kDeleteNow };   // 설정 › 일반 "삭제할 때"
// 자동 is H.264, and HEVC only for a picture longer than H.264 encoders take (4096) on a PC that plays HEVC (#42).
enum CodecChoice { kCodecAuto = 0, kCodecH264, kCodecHevc };

struct Settings {
    // what and how
    int source = kSourceWindow;
    bool multiWindow = false;  // 여러 창 녹화 (#48): windows are checked, up to kMaxRecWindows
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
    int theme = kThemeSystem;    // 시스템 follows Windows' light or dark mode (#26)
    int language = kLangSystem;  // 시스템: Korean when Windows shows Korean, else English
    bool trayHintShown = false;
    int settingsTab = kTabGeneral;  // the settings tab viewed last
    RECT window{};  // main window's restored position (workspace pixels); empty = centre on the primary monitor
    bool windowMax = false;
    UINT hkMods = MOD_CONTROL | MOD_ALT;
    UINT hkVk = 'R';
    UINT hkPauseMods = MOD_CONTROL | MOD_ALT;  // pause / resume, registered only while recording
    UINT hkPauseVk = 'P';
    // Taken over from another program by the user (#50): when RegisterHotKey refuses the key, a keyboard hook gets it.
    bool hkHook = false, hkPauseHook = false;
    int startHide = kHideToTray;  // where the window goes when a recording starts from it
    int deleteMode = kDeleteToBin;  // what 삭제 does to a video or a saved replay
    // The record timer (#47): kTimerOptions, the length 직접 입력 holds, and what happens when the time is up.
    int timerIdx = 0;
    int timerMinutes = 90;   // 1 – kTimerMaxMinutes
    bool timerStop = false;  // false: a notification, the recording goes on; true: it stops and is saved
    std::wstring outDir;

    // video (MP4)
    int vCodec = kCodecAuto;  // CodecChoice
    bool vHardware = true;
    int vFpsIdx = 2;
    int vResIdx = 0;
    int vBitrateIdx = 0;
    int vBitrateKbps = 20000;  // what 직접 입력 holds, in steps of 100 kbps (a tenth of a Mbps)
    int vRateCtrl = 0;  // 0 = VBR, 1 = CBR
    int vFrameMode = 0;  // 0 = CFR, 1 = VFR (#55)
    int vKeyIdx = 1;
    int vPresetIdx = 1;
    bool audio = true;
    int aBitrateIdx = 2;
    int aRateIdx = 1;
    bool mic = false;        // the microphone mixed into the MP4's sound (#21)
    std::wstring micDevice;  // endpoint ID, empty = Windows' default input
    int micVolume = 100;     // %, 0-200
    bool micMonitor = true;  // 마이크 확인 plays the mic back while it runs

    // 순간 리플레이 (#20)
    bool rpOn = false;  // kept: on again at the next start
    int rpLenIdx = 1;   // kReplayLenOptions
    int rpFpsIdx = 2;   // kVideoFpsOptions
    int rpResIdx = 0;   // kVideoResOptions
    bool rpAudio = true, rpMic = false, rpCursor = true;
    UINT rpHkMods = MOD_CONTROL | MOD_ALT;  // registered only while replay is on
    UINT rpHkVk = VK_F10;
    bool rpHkHook = false;
    std::vector<ReplayTarget> rpTargets;

    // export
    int format = 0;                     // 움짤 format (GIF, WebP, AVIF); MP4 and WebM are picked per clip
    int quality[3] = {85, 80, 60};      // per 움짤 format
    int webmQuality = 70;               // WebM (#40): VP9's constant quality, unless 용량 맞추기 sets a bitrate
    int gifLossy = 1;                   // GIF lossy compression: 0 off, 1 light, 2 strong
    int scaleIdx = 0;  // kExportSizes
    int outFpsIdx = 0;
    int speedIdx = 1;
    int loopIdx = 0;   // kLoopOptions
    int previewVolume = 100;  // %, 0-100: the editor's sound preview, not the saved file
    int targetIdx = 0;  // kTargetMB
    bool holdLast = false;
    int exportPreset = 1;        // kExportPresets; -1 = 직접 설정 (the values were picked by hand)
    // 움짤 and MP4 keep their own 화질 (exportPreset, scaleIdx, outFpsIdx). Those fields hold the kind the editor
    // shows (exportVideo); the other kind's wait in `stashed` until the editor switches to it (UseExportKind).
    struct ExportKnobs {
        int preset, scaleIdx, outFpsIdx;
    };
    bool exportVideo = false;
    ExportKnobs stashed{0, 0, 0};  // MP4 starts at 고화질: a video cut at full size isn't encoded again
    // An opened 움짤 file (#41) starts at 고화질 and doesn't change what 움짤 recordings use: while it is in the editor,
    // the recordings' 움짤 화질 waits here, and Save writes that.
    struct AnimKnobs {
        ExportKnobs knobs;
        int gifLossy;
    };
    std::optional<AnimKnobs> recordingAnim;
    bool exportDetails = false;  // the export panel's 자세히 설정 is open
    int directionIdx = 0;        // 움짤: 0 그대로, 1 거꾸로, 2 부메랑
    std::wstring nameTemplate = kNameTemplates[0];  // {날짜}/{date}, {창 이름}/{window}

    std::vector<std::wstring> recent;  // saved and opened files, newest first
    std::wstring crashSeen;            // the newest crash report the user was told about (crash::Report::name)

    void AddRecent(const std::wstring& path);
    void Load();
    void Save() const;
    // 설정 › 일반 › 설정 초기화 (#49): every setting as a new settings file has it, except the save folder, the
    // window's place, what the 녹화 tab has picked, the files lists and what the app noted for itself.
    void Reset();
    // Sets the size, frame rate and (움짤) GIF lossy level to kExportPresets[i] (exportPreset itself stays).
    void ApplyPreset(int i, bool video);
    // Makes exportPreset, scaleIdx and outFpsIdx those of 움짤 or of MP4.
    void UseExportKind(bool video);
    // An opened 움짤 file's own 화질, from 고화질 (the recordings' set aside in recordingAnim), and back.
    void UseFileKnobs();
    void UseRecordingKnobs();
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
    int TimerMinutes() const {  // 0 = off
        return timerIdx == kTimerCustom ? std::clamp(timerMinutes, 1, kTimerMaxMinutes) : kTimerOptions[timerIdx];
    }
    UINT32 VideoBitrate() const {  // bits per second, 0 = 자동
        return vBitrateIdx == kVideoBitrateCustom
                   ? (UINT32)std::clamp(vBitrateKbps, kVideoBitrateMinKbps, kVideoBitrateMaxKbps) * 1000u
                   : (UINT32)kVideoBitrateOptions[vBitrateIdx] * 1'000'000u;
    }
};
