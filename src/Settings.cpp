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
    window.left = ReadSigned(ini, L"WindowL");
    window.top = ReadSigned(ini, L"WindowT");
    window.right = ReadSigned(ini, L"WindowR");
    window.bottom = ReadSigned(ini, L"WindowB");
    windowMax = ReadInt(ini, L"WindowMax", windowMax, 0, 1) != 0;
    hkMods = (UINT)ReadInt(ini, L"HotkeyMods", (int)hkMods, 0, 0xF);
    hkVk = (UINT)ReadInt(ini, L"HotkeyKey", (int)hkVk, 1, 0xFE);
    format = ReadInt(ini, L"Format", format, 0, _countof(quality) - 1);  // a saved HEIF (3, since removed) becomes AVIF
    for (int i = 0; i < (int)_countof(quality); ++i)
        quality[i] = ReadInt(ini, util::Format(L"Quality%d", i).c_str(), quality[i], 1, 100);
    // Older versions kept the scale (100/75/50/33/25 %) under "Scale" and a loop switch under "Loop".
    int oldScale = ReadInt(ini, L"Scale", 0, 0, 4);
    scaleIdx = ReadInt(ini, L"ExportSize", oldScale == 4 ? 5 : oldScale, 0, _countof(kExportSizes) - 1);
    outFpsIdx = ReadInt(ini, L"OutFps", outFpsIdx, 0, _countof(kOutFpsOptions) - 1);
    speedIdx = ReadInt(ini, L"Speed", speedIdx, 0, _countof(kSpeedOptions) - 1);
    loopIdx = ReadInt(ini, L"LoopCount", ReadInt(ini, L"Loop", 1, 0, 1) ? 0 : 1, 0, _countof(kLoopOptions) - 1);
    holdLast = ReadInt(ini, L"HoldLast", holdLast, 0, 1) != 0;
    targetIdx = ReadInt(ini, L"TargetSize", targetIdx, 0, _countof(kTargetMB) - 1);

    wchar_t buf[MAX_PATH * 2] = {};
    GetPrivateProfileStringW(L"OzMaker", L"Monitor", L"", buf, _countof(buf), ini.c_str());
    monitor = buf;
    GetPrivateProfileStringW(L"OzMaker", L"OutDir", L"", buf, _countof(buf), ini.c_str());
    outDir = buf;
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
    num(L"Fps", fpsIdx);
    num(L"Size", sizeIdx);
    num(L"Cursor", cursor);
    num(L"ClientOnly", clientOnly);
    num(L"CaptureMode", captureMode);
    num(L"Countdown", countdown);
    num(L"Overlay", overlayMode);
    num(L"Sounds", sounds);
    num(L"CloseToTray", closeToTray);
    num(L"TrayHintShown", trayHintShown);
    num(L"WindowL", window.left);
    num(L"WindowT", window.top);
    num(L"WindowR", window.right);
    num(L"WindowB", window.bottom);
    num(L"WindowMax", windowMax);
    num(L"HotkeyMods", (long)hkMods);
    num(L"HotkeyKey", (long)hkVk);
    num(L"Format", format);
    for (int i = 0; i < (int)_countof(quality); ++i) num(util::Format(L"Quality%d", i).c_str(), quality[i]);
    num(L"ExportSize", scaleIdx);
    num(L"OutFps", outFpsIdx);
    num(L"Speed", speedIdx);
    num(L"LoopCount", loopIdx);
    num(L"HoldLast", holdLast);
    num(L"TargetSize", targetIdx);
    put(L"OutDir", outDir);
    put(L"FileName", nameTemplate);
    for (size_t i = 0; i < recent.size() && i < kMaxRecent; ++i) put(util::Format(L"Recent%zu", i).c_str(), recent[i]);
    WriteIni(IniPath(), s);
}

void Settings::AddRecent(const std::wstring& path) {
    std::erase_if(recent, [&](const std::wstring& p) { return _wcsicmp(p.c_str(), path.c_str()) == 0; });
    recent.insert(recent.begin(), path);
    if (recent.size() > kMaxRecent) recent.resize(kMaxRecent);
}
