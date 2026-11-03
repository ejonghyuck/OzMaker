#ifndef OZMAKER_VERSION_H
#define OZMAKER_VERSION_H
// The one place the version is written: res\OzMaker.rc, the settings and the updater all read it from here.
// app.manifest and vcpkg.json repeat it (tools\package.ps1 warns when they differ).
#define OZ_VERSION_MAJOR 0
#define OZ_VERSION_MINOR 6
#define OZ_VERSION_PATCH 4
#define OZ_VERSION_STR "0.6.4"
#define OZ_VERSION_WSTR L"" OZ_VERSION_STR
#ifndef RC_INVOKED
inline constexpr const wchar_t* kRepoUrl = L"https://github.com/ejonghyuck/OzMaker";
#endif
#endif
