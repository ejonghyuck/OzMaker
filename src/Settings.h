#pragma once

inline constexpr int kFpsOptions[] = {10, 15, 20, 30, 50, 60};
inline constexpr int kSizeOptions[] = {0, 1280, 960, 720, 480};  // longest edge, 0 = original
inline constexpr int kScaleOptions[] = {100, 75, 50, 33, 25};    // export scale %
inline constexpr int kOutFpsOptions[] = {0, 30, 25, 20, 15, 10};  // 0 = as recorded
inline constexpr double kSpeedOptions[] = {0.5, 1.0, 1.5, 2.0};
inline constexpr size_t kMaxRecent = 12;

// MP4 recording
inline constexpr int kVideoFpsOptions[] = {24, 30, 60};
inline constexpr int kVideoResOptions[] = {0, 2160, 1440, 1080, 720};       // short edge of the 16:9 box, 0 = original
inline constexpr int kVideoBitrateOptions[] = {0, 6, 10, 16, 25, 40, 60};   // Mbps, 0 = automatic
inline constexpr int kKeyframeOptions[] = {1, 2, 5};                        // seconds
inline constexpr int kPresetOptions[] = {0, 50, 100};                       // quality vs speed
inline constexpr int kAudioBitrateOptions[] = {96, 128, 160, 192};          // kbps (AAC)
inline constexpr int kAudioRateOptions[] = {44100, 48000};

enum Source { kSourceWindow = 0, kSourceDisplay = 1, kSourceRegion = 2 };
enum Output { kOutputClip = 0, kOutputVideo = 1 };
enum CaptureMode { kCaptureAuto = 0, kCaptureWindow = 1, kCaptureDisplay = 2 };
enum OverlayMode { kOverlayAlways = 0, kOverlayExceptFullscreen = 1, kOverlayNever = 2 };

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
    int captureMode = kCaptureAuto;
    int countdown = 1;  // 0 = none, 1 = 3 s, 2 = 5 s
    int overlayMode = kOverlayExceptFullscreen;
    bool sounds = true;
    bool closeToTray = true;
    bool trayHintShown = false;
    RECT window{};  // main window's restored position (workspace pixels); empty = centre on the primary monitor
    bool windowMax = false;
    UINT hkMods = MOD_CONTROL | MOD_ALT;
    UINT hkVk = 'R';
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

    // export
    int format = 0;                     // 움짤 format (GIF, WebP, AVIF); MP4 is picked per clip
    int quality[3] = {85, 80, 60};      // per 움짤 format
    int scaleIdx = 0;
    int outFpsIdx = 0;
    int speedIdx = 1;
    bool loop = true;

    std::vector<std::wstring> recent;  // saved and opened files, newest first

    void AddRecent(const std::wstring& path);
    void Load();
    void Save() const;
    bool HasRegion() const { return region.right - region.left >= 16 && region.bottom - region.top >= 16; }
    int CountdownSeconds() const { return countdown == 1 ? 3 : countdown == 2 ? 5 : 0; }
};
