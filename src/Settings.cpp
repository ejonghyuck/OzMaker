#include "pch.h"
#include "Settings.h"
#include "Util.h"

namespace {
// OZMAKER_SETTINGS points test runs at a scratch file instead of the user's settings.
std::wstring IniPath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"OZMAKER_SETTINGS", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return buf;
    return util::AppDataDir() + L"\\settings.ini";
}

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

void WriteInt(const std::wstring& ini, const wchar_t* key, int v) {
    WritePrivateProfileStringW(L"OzMaker", key, std::to_wstring(v).c_str(), ini.c_str());
}
}  // namespace

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
    fpsIdx = ReadInt(ini, L"Fps", fpsIdx, 0, _countof(kFpsOptions) - 1);
    sizeIdx = ReadInt(ini, L"Size", sizeIdx, 0, _countof(kSizeOptions) - 1);
    cursor = ReadInt(ini, L"Cursor", cursor, 0, 1) != 0;
    clientOnly = ReadInt(ini, L"ClientOnly", clientOnly, 0, 1) != 0;
    captureMode = ReadInt(ini, L"CaptureMode", captureMode, 0, 2);
    countdown = ReadInt(ini, L"Countdown", countdown, 0, 2);
    overlayMode = ReadInt(ini, L"Overlay", overlayMode, 0, 2);
    sounds = ReadInt(ini, L"Sounds", sounds, 0, 1) != 0;
    closeToTray = ReadInt(ini, L"CloseToTray", closeToTray, 0, 1) != 0;
    trayHintShown = ReadInt(ini, L"TrayHintShown", trayHintShown, 0, 1) != 0;
    hkMods = (UINT)ReadInt(ini, L"HotkeyMods", (int)hkMods, 0, 0xF);
    hkVk = (UINT)ReadInt(ini, L"HotkeyKey", (int)hkVk, 1, 0xFE);
    format = ReadInt(ini, L"Format", format, 0, _countof(quality) - 1);  // a saved HEIF (3, since removed) becomes AVIF
    for (int i = 0; i < (int)_countof(quality); ++i)
        quality[i] = ReadInt(ini, util::Format(L"Quality%d", i).c_str(), quality[i], 1, 100);
    scaleIdx = ReadInt(ini, L"Scale", scaleIdx, 0, _countof(kScaleOptions) - 1);
    outFpsIdx = ReadInt(ini, L"OutFps", outFpsIdx, 0, _countof(kOutFpsOptions) - 1);
    speedIdx = ReadInt(ini, L"Speed", speedIdx, 0, _countof(kSpeedOptions) - 1);
    loop = ReadInt(ini, L"Loop", loop, 0, 1) != 0;

    wchar_t buf[MAX_PATH * 2] = {};
    GetPrivateProfileStringW(L"OzMaker", L"Monitor", L"", buf, _countof(buf), ini.c_str());
    monitor = buf;
    GetPrivateProfileStringW(L"OzMaker", L"OutDir", L"", buf, _countof(buf), ini.c_str());
    outDir = buf;
    if (outDir.empty()) outDir = util::KnownFolder(FOLDERID_Pictures) + L"\\OzMaker";
}

void Settings::Save() const {
    std::wstring ini = IniPath();
    WriteInt(ini, L"Source", source);
    WriteInt(ini, L"Output", output);
    WriteInt(ini, L"RegionL", region.left);
    WriteInt(ini, L"RegionT", region.top);
    WriteInt(ini, L"RegionR", region.right);
    WriteInt(ini, L"RegionB", region.bottom);
    WritePrivateProfileStringW(L"OzMaker", L"Monitor", monitor.c_str(), ini.c_str());
    WriteInt(ini, L"VideoCodec", vCodec);
    WriteInt(ini, L"VideoHardware", vHardware);
    WriteInt(ini, L"VideoFps", vFpsIdx);
    WriteInt(ini, L"VideoRes", vResIdx);
    WriteInt(ini, L"VideoBitrate", vBitrateIdx);
    WriteInt(ini, L"VideoRateControl", vRateCtrl);
    WriteInt(ini, L"VideoKeyframe", vKeyIdx);
    WriteInt(ini, L"VideoPreset", vPresetIdx);
    WriteInt(ini, L"Audio", audio);
    WriteInt(ini, L"AudioBitrate", aBitrateIdx);
    WriteInt(ini, L"AudioRate", aRateIdx);
    WriteInt(ini, L"Fps", fpsIdx);
    WriteInt(ini, L"Size", sizeIdx);
    WriteInt(ini, L"Cursor", cursor);
    WriteInt(ini, L"ClientOnly", clientOnly);
    WriteInt(ini, L"CaptureMode", captureMode);
    WriteInt(ini, L"Countdown", countdown);
    WriteInt(ini, L"Overlay", overlayMode);
    WriteInt(ini, L"Sounds", sounds);
    WriteInt(ini, L"CloseToTray", closeToTray);
    WriteInt(ini, L"TrayHintShown", trayHintShown);
    WriteInt(ini, L"HotkeyMods", (int)hkMods);
    WriteInt(ini, L"HotkeyKey", (int)hkVk);
    WriteInt(ini, L"Format", format);
    for (int i = 0; i < (int)_countof(quality); ++i) WriteInt(ini, util::Format(L"Quality%d", i).c_str(), quality[i]);
    WriteInt(ini, L"Scale", scaleIdx);
    WriteInt(ini, L"OutFps", outFpsIdx);
    WriteInt(ini, L"Speed", speedIdx);
    WriteInt(ini, L"Loop", loop);
    WritePrivateProfileStringW(L"OzMaker", L"OutDir", outDir.c_str(), ini.c_str());
}
