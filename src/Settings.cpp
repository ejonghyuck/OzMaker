#include "pch.h"
#include "Settings.h"
#include "Util.h"

namespace {
// OZMAKER_SETTINGS points test runs at a scratch file instead of the user's settings.
std::wstring TestIniPath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"OZMAKER_SETTINGS", buf, MAX_PATH);
    return n > 0 && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
}

std::wstring IniPath() { return Settings::FilePath(); }

int ReadInt(const std::wstring& ini, const wchar_t* key, int def, int lo, int hi) {
    int v = (int)GetPrivateProfileIntW(L"OzMaker", key, def, ini.c_str());
    return std::clamp(v, lo, hi);
}

// GetPrivateProfileInt clamps negatives to zero; monitors left of / above the primary have negative coordinates.
LONG ReadSigned(const std::wstring& ini, const wchar_t* key) {
    wchar_t buf[32] = {};
    GetPrivateProfileStringW(L"OzMaker", key, L"0", buf, _countof(buf), ini.c_str());
    return std::clamp<long>(wcstol(buf, nullptr, 10), -100000, 100000);
}

// The whole file in one go, as UTF-16 with a BOM (WritePrivateProfileString would create an ANSI file, turning
// characters outside the code page in paths into '?'), through a temporary file so a crash never leaves half of it.
void WriteIni(const std::wstring& ini, const std::wstring& text) {
    std::wstring tmp = ini + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const wchar_t bom = 0xFEFF;
    DWORD n = 0;
    bool ok = WriteFile(h, &bom, sizeof(bom), &n, nullptr) &&
              WriteFile(h, text.data(), (DWORD)(text.size() * sizeof(wchar_t)), &n, nullptr);
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), ini.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        DeleteFileW(tmp.c_str());
}
}  // namespace

void Settings::ApplyPreset(int i, bool video) {
    const ExportPreset& p = kExportPresets[i];
    scaleIdx = video ? p.videoSize : p.animSize;
    outFpsIdx = video ? p.videoFps : p.animFps;
    if (!video) gifLossy = p.gifLossy;
}

void Settings::UseExportKind(bool video) {
    if (video == exportVideo) return;
    ExportKnobs now{exportPreset, scaleIdx, outFpsIdx};
    exportPreset = stashed.preset;
    scaleIdx = stashed.scaleIdx;
    outFpsIdx = stashed.outFpsIdx;
    stashed = now;
    exportVideo = video;
}

int Settings::MatchPreset(bool video, bool gif) const {
    for (int i = 0; i < (int)_countof(kExportPresets); ++i) {
        const ExportPreset& p = kExportPresets[i];
        if (scaleIdx == (video ? p.videoSize : p.animSize) && outFpsIdx == (video ? p.videoFps : p.animFps) &&
            (!gif || gifLossy == p.gifLossy))
            return i;
    }
    return -1;
}

void Settings::Load() {
    std::wstring ini = IniPath();
    source = ReadInt(ini, L"Source", source, 0, 2);
    output = ReadInt(ini, L"Output", output, 0, 1);
    region.left = ReadSigned(ini, L"RegionL");
    region.top = ReadSigned(ini, L"RegionT");
    region.right = ReadSigned(ini, L"RegionR");
    region.bottom = ReadSigned(ini, L"RegionB");
    vCodec = ReadInt(ini, L"VideoCodec", vCodec, 0, 1);
    vHardware = ReadInt(ini, L"VideoHardware", vHardware, 0, 1) != 0;
    vFpsIdx = ReadInt(ini, L"VideoFps", vFpsIdx, 0, _countof(kVideoFpsOptions) - 1);
    vResIdx = ReadInt(ini, L"VideoRes", vResIdx, 0, _countof(kVideoResOptions) - 1);
    vBitrateIdx = ReadInt(ini, L"VideoBitrate", vBitrateIdx, 0, _countof(kVideoBitrateOptions) - 1);
    vRateCtrl = ReadInt(ini, L"VideoRateControl", vRateCtrl, 0, 1);
    vKeyIdx = ReadInt(ini, L"VideoKeyframe", vKeyIdx, 0, _countof(kKeyframeOptions) - 1);
    vPresetIdx = ReadInt(ini, L"VideoPreset", vPresetIdx, 0, _countof(kPresetOptions) - 1);
    audio = ReadInt(ini, L"Audio", audio, 0, 1) != 0;
    aBitrateIdx = ReadInt(ini, L"AudioBitrate", aBitrateIdx, 0, _countof(kAudioBitrateOptions) - 1);
    aRateIdx = ReadInt(ini, L"AudioRate", aRateIdx, 0, _countof(kAudioRateOptions) - 1);
    mic = ReadInt(ini, L"Mic", mic, 0, 1) != 0;
    micVolume = ReadInt(ini, L"MicVolume", micVolume, 0, 200);
    micMonitor = ReadInt(ini, L"MicMonitor", micMonitor, 0, 1) != 0;
    fpsIdx = ReadInt(ini, L"Fps", fpsIdx, 0, _countof(kFpsOptions) - 1);
    sizeIdx = ReadInt(ini, L"Size", sizeIdx, 0, _countof(kSizeOptions) - 1);
    cursor = ReadInt(ini, L"Cursor", cursor, 0, 1) != 0;
    clientOnly = ReadInt(ini, L"ClientOnly", clientOnly, 0, 1) != 0;
    clickFx = ReadInt(ini, L"ClickHighlight", clickFx, 0, 1) != 0;
    captureMode = ReadInt(ini, L"CaptureMode", captureMode, 0, 2);
    countdown = ReadInt(ini, L"Countdown", countdown, 0, 2);
    overlayMode = ReadInt(ini, L"Overlay", overlayMode, 0, 2);
    sounds = ReadInt(ini, L"Sounds", sounds, 0, 1) != 0;
    closeToTray = ReadInt(ini, L"CloseToTray", closeToTray, 0, 1) != 0;
    autoUpdate = ReadInt(ini, L"AutoUpdate", autoUpdate, 0, 1) != 0;
    theme = ReadInt(ini, L"Theme", theme, 0, 2);
    language = ReadInt(ini, L"Language", language, 0, 2);
    trayHintShown = ReadInt(ini, L"TrayHintShown", trayHintShown, 0, 1) != 0;
    settingsTab = ReadInt(ini, L"SettingsTab", settingsTab, 0, kTabCount - 1);
    window.left = ReadSigned(ini, L"WindowL");
    window.top = ReadSigned(ini, L"WindowT");
    window.right = ReadSigned(ini, L"WindowR");
    window.bottom = ReadSigned(ini, L"WindowB");
    windowMax = ReadInt(ini, L"WindowMax", windowMax, 0, 1) != 0;
    hkMods = (UINT)ReadInt(ini, L"HotkeyMods", (int)hkMods, 0, 0xF);
    hkVk = (UINT)ReadInt(ini, L"HotkeyKey", (int)hkVk, 1, 0xFE);
    hkPauseMods = (UINT)ReadInt(ini, L"PauseHotkeyMods", (int)hkPauseMods, 0, 0xF);
    hkPauseVk = (UINT)ReadInt(ini, L"PauseHotkeyKey", (int)hkPauseVk, 1, 0xFE);
    maxLengthIdx = ReadInt(ini, L"MaxLength", maxLengthIdx, 0, _countof(kMaxLengthOptions) - 1);
    format = ReadInt(ini, L"Format", format, 0, _countof(quality) - 1);  // a saved HEIF (3, since removed) becomes AVIF
    for (int i = 0; i < (int)_countof(quality); ++i)
        quality[i] = ReadInt(ini, util::Format(L"Quality%d", i).c_str(), quality[i], 1, 100);
    gifLossy = ReadInt(ini, L"GifLossy", gifLossy, 0, 2);
    // Older versions kept the scale (100/75/50/33/25 %) under "Scale" and a loop switch under "Loop".
    int oldScale = ReadInt(ini, L"Scale", 0, 0, 4);
    scaleIdx = ReadInt(ini, L"ExportSize", oldScale == 4 ? 5 : oldScale, 0, _countof(kExportSizes) - 1);
    outFpsIdx = ReadInt(ini, L"OutFps", outFpsIdx, 0, _countof(kOutFpsOptions) - 1);
    speedIdx = ReadInt(ini, L"Speed", speedIdx, 0, _countof(kSpeedOptions) - 1);
    loopIdx = ReadInt(ini, L"LoopCount", ReadInt(ini, L"Loop", 1, 0, 1) ? 0 : 1, 0, _countof(kLoopOptions) - 1);
    previewVolume = ReadInt(ini, L"PreviewVolume", previewVolume, 0, 100);
    holdLast = ReadInt(ini, L"HoldLast", holdLast, 0, 1) != 0;
    targetIdx = ReadInt(ini, L"TargetSize", targetIdx, 0, _countof(kTargetMB) - 1);
    // A file from before the presets keeps its values and shows the preset they match, if any. A new one starts at
    // 보통. (GetPrivateProfileInt reads -1 back as 0, so the preset is read as text.)
    wchar_t pv[8] = {};
    GetPrivateProfileStringW(L"OzMaker", L"ExportPreset", L"", pv, _countof(pv), ini.c_str());
    if (*pv)
        exportPreset = std::clamp((int)wcstol(pv, nullptr, 10), -1, (int)_countof(kExportPresets) - 1);
    else if ((int)GetPrivateProfileIntW(L"OzMaker", L"ExportSize", (UINT)-1, ini.c_str()) < 0 &&
             (int)GetPrivateProfileIntW(L"OzMaker", L"Scale", (UINT)-1, ini.c_str()) < 0)
        ApplyPreset(exportPreset, false);
    else
        exportPreset = MatchPreset(false, true);
    // MP4's own, from 0.6.3. Before that MP4 used the 움짤 preset; it starts at 고화질 now either way.
    exportVideo = false;
    GetPrivateProfileStringW(L"OzMaker", L"Mp4Preset", L"", pv, _countof(pv), ini.c_str());
    stashed.preset = *pv ? std::clamp((int)wcstol(pv, nullptr, 10), -1, (int)_countof(kExportPresets) - 1) : 0;
    stashed.scaleIdx = ReadInt(ini, L"Mp4Size", kExportPresets[0].videoSize, 0, _countof(kExportSizes) - 1);
    stashed.outFpsIdx = ReadInt(ini, L"Mp4OutFps", kExportPresets[0].videoFps, 0, _countof(kOutFpsOptions) - 1);
    exportDetails = ReadInt(ini, L"ExportDetails", exportDetails, 0, 1) != 0;
    directionIdx = ReadInt(ini, L"Direction", directionIdx, 0, 2);

    wchar_t buf[MAX_PATH * 2] = {};
    GetPrivateProfileStringW(L"OzMaker", L"Monitor", L"", buf, _countof(buf), ini.c_str());
    monitor = buf;
    GetPrivateProfileStringW(L"OzMaker", L"OutDir", L"", buf, _countof(buf), ini.c_str());
    outDir = buf;
    GetPrivateProfileStringW(L"OzMaker", L"MicDevice", L"", buf, _countof(buf), ini.c_str());
    micDevice = buf;
    GetPrivateProfileStringW(L"OzMaker", L"CrashSeen", L"", buf, _countof(buf), ini.c_str());
    crashSeen = buf;
    recent.clear();
    for (size_t i = 0; i < kMaxRecent; ++i) {
        GetPrivateProfileStringW(L"OzMaker", util::Format(L"Recent%zu", i).c_str(), L"", buf, _countof(buf), ini.c_str());
        if (*buf) recent.push_back(buf);
    }
    if (outDir.empty()) outDir = util::KnownFolder(FOLDERID_Pictures) + L"\\OzMaker";
    GetPrivateProfileStringW(L"OzMaker", L"FileName", kNameTemplates[0], buf, _countof(buf), ini.c_str());
    nameTemplate = *buf ? buf : kNameTemplates[0];
}

void Settings::Save() const {
    std::wstring s = L"[OzMaker]\r\n";
    auto put = [&](const wchar_t* key, const std::wstring& v) {
        s += key;
        s += L'=';
        s += v;
        s += L"\r\n";
    };
    auto num = [&](const wchar_t* key, long v) { put(key, std::to_wstring(v)); };
    num(L"Source", source);
    num(L"Output", output);
    num(L"RegionL", region.left);
    num(L"RegionT", region.top);
    num(L"RegionR", region.right);
    num(L"RegionB", region.bottom);
    put(L"Monitor", monitor);
    num(L"VideoCodec", vCodec);
    num(L"VideoHardware", vHardware);
    num(L"VideoFps", vFpsIdx);
    num(L"VideoRes", vResIdx);
    num(L"VideoBitrate", vBitrateIdx);
    num(L"VideoRateControl", vRateCtrl);
    num(L"VideoKeyframe", vKeyIdx);
    num(L"VideoPreset", vPresetIdx);
    num(L"Audio", audio);
    num(L"AudioBitrate", aBitrateIdx);
    num(L"AudioRate", aRateIdx);
    num(L"Mic", mic);
    num(L"MicVolume", micVolume);
    num(L"MicMonitor", micMonitor);
    num(L"Fps", fpsIdx);
    num(L"Size", sizeIdx);
    num(L"Cursor", cursor);
    num(L"ClientOnly", clientOnly);
    num(L"ClickHighlight", clickFx);
    num(L"CaptureMode", captureMode);
    num(L"Countdown", countdown);
    num(L"Overlay", overlayMode);
    num(L"Sounds", sounds);
    num(L"CloseToTray", closeToTray);
    num(L"AutoUpdate", autoUpdate);
    num(L"Theme", theme);
    num(L"Language", language);
    num(L"TrayHintShown", trayHintShown);
    num(L"SettingsTab", settingsTab);
    num(L"WindowL", window.left);
    num(L"WindowT", window.top);
    num(L"WindowR", window.right);
    num(L"WindowB", window.bottom);
    num(L"WindowMax", windowMax);
    num(L"HotkeyMods", (long)hkMods);
    num(L"HotkeyKey", (long)hkVk);
    num(L"PauseHotkeyMods", (long)hkPauseMods);
    num(L"PauseHotkeyKey", (long)hkPauseVk);
    num(L"MaxLength", maxLengthIdx);
    num(L"Format", format);
    for (int i = 0; i < (int)_countof(quality); ++i) num(util::Format(L"Quality%d", i).c_str(), quality[i]);
    num(L"GifLossy", gifLossy);
    const ExportKnobs shown{exportPreset, scaleIdx, outFpsIdx};
    const ExportKnobs& anim = exportVideo ? stashed : shown;
    const ExportKnobs& mp4 = exportVideo ? shown : stashed;
    num(L"ExportSize", anim.scaleIdx);
    num(L"OutFps", anim.outFpsIdx);
    num(L"Speed", speedIdx);
    num(L"LoopCount", loopIdx);
    num(L"PreviewVolume", previewVolume);
    num(L"HoldLast", holdLast);
    num(L"TargetSize", targetIdx);
    num(L"ExportPreset", anim.preset);
    num(L"Mp4Preset", mp4.preset);
    num(L"Mp4Size", mp4.scaleIdx);
    num(L"Mp4OutFps", mp4.outFpsIdx);
    num(L"ExportDetails", exportDetails);
    num(L"Direction", directionIdx);
    put(L"OutDir", outDir);
    put(L"CrashSeen", crashSeen);
    put(L"MicDevice", micDevice);
    put(L"FileName", nameTemplate);
    for (size_t i = 0; i < recent.size() && i < kMaxRecent; ++i) put(util::Format(L"Recent%zu", i).c_str(), recent[i]);
    WriteIni(IniPath(), s);
}

std::wstring Settings::PortableFile(const std::wstring& exeDir) {
    std::wstring p = exeDir + L"\\settings.ini";
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY) ? p : std::wstring();
}

bool Settings::Portable() {
    static const bool portable = TestIniPath().empty() && !PortableFile(util::ExeDir()).empty();
    return portable;
}

std::wstring Settings::FilePath() {
    if (std::wstring t = TestIniPath(); !t.empty()) return t;
    if (Portable()) return PortableFile(util::ExeDir());
    return util::AppDataDir() + L"\\settings.ini";
}

std::wstring Settings::Dir() {
    std::wstring p = FilePath();
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : p.substr(0, slash);
}

void Settings::AddRecent(const std::wstring& path) {
    std::erase_if(recent, [&](const std::wstring& p) { return _wcsicmp(p.c_str(), path.c_str()) == 0; });
    recent.insert(recent.begin(), path);
    if (recent.size() > kMaxRecent) recent.resize(kMaxRecent);
}
