#pragma once

// Crash reports (#24). An unhandled exception, abort or CRT failure writes a small minidump (threads and stacks, no
// screen contents) and a short text log into <settings folder>\crash. Nothing is sent anywhere: the next start tells
// the user once, and they can attach the files to an issue. Reading a dump needs the release's PDB.
namespace crash {

void Install(const std::wstring& dir);  // first thing in wWinMain; dir is only created when a report is written
const std::wstring& Dir();
// What the app is doing, for the log ("녹화 중 · 동영상"). Must point to a string that lives forever.
void SetState(const wchar_t* what);

struct Report {
    std::wstring dump, log;  // full paths; log may be missing
    std::wstring name;       // crash_<yyyymmdd_hhmmss>_<version>, sorts by time
};
std::vector<Report> List();  // newest first
void Prune(size_t keep);     // deletes all but the newest `keep`

}  // namespace crash
