#pragma once
#include "Clip.h"
#include "Mp4Writer.h"
#include "encoders/Encoder.h"

struct ExportJob {
    std::shared_ptr<Clip> clip;
    size_t first = 0, last = 0;  // inclusive frame range
    RECT crop{};                 // in clip pixels
    int outW = 0, outH = 0;
    double fpsLimit = 0;         // most frames per second of output (after speed); 0 = all; MP4: the constant rate
    double speed = 1.0;
    Format format = Format::Gif;
    int quality = 80;
    int plays = 0;               // 움짤: times it plays, 0 = forever
    int lossy = 0;               // GIF: lossy compression, 0 off, 1 light, 2 strong
    double holdLastMs = 0;       // 움짤: extra time on the last frame
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

// Size of an export without writing it: a few short stretches are encoded for real and scaled up to the whole
// length (움짤), or bitrate × length (MP4). 0 when cancelled or on failure.
uint64_t EstimateBytes(const ExportJob& job, const std::atomic<bool>& cancel);

// EstimateBytes on a worker thread over candidate jobs, best first. With a target it stops at the first that fits
// (fitting an export into a size); without one it estimates the first only. A new request abandons the running one.
class Estimator {
public:
    struct Result {
        uint32_t serial = 0;
        int chosen = -1;  // candidate index; the last one when none fit
        uint64_t bytes = 0;
        bool fits = false;
    };
    ~Estimator();
    void Start(std::vector<ExportJob> candidates, uint64_t target, uint32_t serial, HWND notify, UINT msg);
    bool Busy() const { return busy_; }
    bool Take(Result& out);  // the finished result, once

private:
    void Run();

    std::thread thread_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<ExportJob> pending_;
    uint64_t pendingTarget_ = 0;
    uint32_t pendingSerial_ = 0;
    bool hasPending_ = false, quit_ = false, ready_ = false;
    std::atomic<bool> cancel_{false}, busy_{false};
    Result result_;
    HWND notify_ = nullptr;
    UINT msg_ = 0;
};

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
