#pragma once

// Decodes frames of a clip. One per consumer thread.
class FrameReader {
public:
    virtual ~FrameReader() = default;
    // Tightly packed BGRA (Width()*4 stride), valid until the next call; null on failure.
    virtual const uint8_t* Get(size_t index) = 0;
};

// Timestamped frames the editor and the exporter work on: a fresh recording (FrameStore) or an MP4 file
// (VideoClip). Timestamps are 100 ns units from the first frame.
class Clip {
public:
    virtual ~Clip() = default;
    virtual int Width() const = 0;
    virtual int Height() const = 0;
    virtual size_t Count() const = 0;
    virtual int64_t Ts(size_t i) const = 0;
    virtual int64_t EndTs() const = 0;
    virtual std::unique_ptr<FrameReader> NewReader() const = 0;
    // Random access may take a while (long-GOP video): fetch preview frames off the UI thread.
    virtual bool SlowSeek() const { return false; }
    virtual bool HasAudio() const { return false; }
    // File the frames come from; empty for a recording that only lives in the temp store.
    virtual std::wstring SourcePath() const { return {}; }

    double Seconds() const { return EndTs() / 1e7; }
    // Index of the frame visible at time t.
    size_t IndexAt(int64_t t) const;
    // A frame within tolerance of t that is quick to decode (thumbnails of long videos).
    virtual size_t QuickIndexNear(int64_t t, int64_t tolerance) const { return IndexAt(t); }
    double AverageFps() const;
};

// Decodes the frame the editor wants on a worker thread, newest request first, so seeking in a long-GOP video
// never blocks the UI. Posts msg to notify whenever a frame is ready.
class FrameFetcher {
public:
    ~FrameFetcher() { Stop(); }
    void Start(std::shared_ptr<Clip> clip, HWND notify, UINT msg);
    void Stop();
    bool Running() const { return thread_.joinable(); }
    void Want(size_t index);
    // Swaps the newest decoded frame into px if there is one not taken yet.
    bool Take(std::vector<uint8_t>& px, size_t& index);

private:
    void Run();

    std::shared_ptr<Clip> clip_;
    HWND notify_ = nullptr;
    UINT msg_ = 0;
    std::thread thread_;
    std::mutex mtx_;
    std::condition_variable cv_;
    size_t want_ = SIZE_MAX, done_ = SIZE_MAX, readyIndex_ = SIZE_MAX;
    bool ready_ = false, stop_ = false;
    std::vector<uint8_t> readyPx_;
};
