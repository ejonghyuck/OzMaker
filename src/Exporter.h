#pragma once
#include "Clip.h"
#include "Mp4Writer.h"
#include "encoders/Encoder.h"

struct ExportJob {
    std::shared_ptr<Clip> clip;
    size_t first = 0, last = 0;  // inclusive frame range
    RECT crop{};                 // in clip pixels
    int outW = 0, outH = 0;
    double fpsLimit = 0;         // 0 = keep recorded rate; MP4: the constant output rate
    double speed = 1.0;
    Format format = Format::Gif;
    int quality = 80;
    bool loop = true;
    VideoConfig video;           // MP4: codec and bitrate; size and rate come from the fields above
    AudioConfig audio;           // MP4: enabled = keep the clip's sound (only at speed 1)
    std::wstring path;
};

struct ExportResult {
    bool ok = false, cancelled = false;
    bool audio = false;  // MP4 with sound
    std::wstring path, error;
    uint64_t bytes = 0;
    int frames = 0;
    double seconds = 0;
};

struct PlannedFrame {
    size_t index;
    double durationMs;
};
// Frames to emit for a range after frame-rate limiting and speed change.
std::vector<PlannedFrame> PlanFrames(const Clip& s, size_t first, size_t last, double fpsLimit, double speed);
// MP4: the source frame shown at each tick of a constant output rate.
std::vector<size_t> PlanVideoFrames(const Clip& s, size_t first, size_t last, double fps, double speed);

class Exporter {
public:
    ~Exporter();
    void Start(ExportJob job, HWND notify, UINT doneMsg);
    void Cancel() { cancel_ = true; }
    bool Busy() const { return busy_; }
    float Progress() const { return progress_; }
    ExportResult Take();

private:
    void Run(ExportJob job);
    bool RunImage(const ExportJob& job, const std::wstring& tmp, ExportResult& res);
    bool RunMp4(const ExportJob& job, const std::wstring& tmp, ExportResult& res);

    std::thread thread_;
    std::atomic<bool> cancel_{false}, busy_{false};
    std::atomic<float> progress_{0};
    ExportResult result_;
    HWND notify_ = nullptr;
    UINT doneMsg_ = 0;
};
