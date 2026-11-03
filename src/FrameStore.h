#pragma once
#include "Clip.h"

// Recorded frames live in a temp file, XOR-delta coded against the previous frame (keyframe every kKeyInterval)
// and XPRESS-compressed by a few worker threads. The OS file cache keeps recent data in RAM when available, so the
// process working set stays small even for long clips.
//
// Crash-safe (#24): a journal next to the file records each frame once its data is written, so a recording outlives a
// crash or a forced shutdown. Dropping the store deletes both files; the next start takes back what a process that
// has gone left behind (Recover).
class FrameStore : public Clip {
public:
    static constexpr size_t kKeyInterval = 30;
    using Buffer = std::vector<uint8_t>;

    FrameStore() = default;
    ~FrameStore();
    FrameStore(const FrameStore&) = delete;
    FrameStore& operator=(const FrameStore&) = delete;

    // journal = false: frames decoded from a file (#41), which a crash need not leave behind.
    bool Open(int width, int height, std::wstring& err, bool journal = true);

    // Where recordings live: %TEMP%\OzMaker, or next to the scratch settings in a test run.
    static std::wstring Dir();
    struct Recovered {
        std::shared_ptr<FrameStore> clip;  // finished, ready to edit; its files go when it is dropped
        double age = 0;                    // seconds since it was last written
    };
    // What processes that have gone left in Dir(), newest first. Files that hold no frame are deleted.
    static std::vector<Recovered> Recover();
    const std::wstring& Path() const { return path_; }
    // Writer side (capture thread). Acquire a frame buffer, fill it with tightly packed BGRA, submit it.
    std::shared_ptr<Buffer> Acquire();
    // wait: while the workers are behind, wait for them instead of dropping the frame (a file, not a live picture).
    bool Submit(std::shared_ptr<Buffer> frame, int64_t ts100ns, bool wait = false);
    void Finish(int64_t endTs100ns);

    int Width() const override { return width_; }
    int Height() const override { return height_; }
    size_t FrameBytes() const { return frameBytes_; }
    size_t Count() const override;
    int64_t Ts(size_t i) const override { return entries_[i].ts; }
    int64_t EndTs() const override { return endTs_; }
    double NominalFps() const override { return nominalFps_; }
    void SetNominalFps(double fps);
    std::unique_ptr<FrameReader> NewReader() const override { return std::make_unique<Reader>(this); }
    uint64_t DiskBytes() const { return writeOffset_.load(); }
    uint32_t Dropped() const { return dropped_.load(); }
    // A write failed (disk full): no more frames are taken, and Finish keeps only those before the failure.
    bool WriteFailed() const { return firstBad_.load() != SIZE_MAX; }

    // Sequential/random-access decoder. One per consumer thread.
    class Reader : public FrameReader {
    public:
        explicit Reader(const FrameStore* s);
        ~Reader() override;
        const uint8_t* Get(size_t index) override;
    private:
        bool Decode(size_t j, uint8_t* out);
        const FrameStore* s_;
        DECOMPRESSOR_HANDLE dec_ = nullptr;
        std::vector<uint8_t> cur_, tmp_, comp_;
        size_t curIndex_ = SIZE_MAX;
    };

private:
    struct Entry {
        uint64_t offset = 0;
        uint32_t size = 0;
        bool raw = false;
        int64_t ts = 0;
    };
    struct Job {
        size_t seq = 0;
        std::shared_ptr<Buffer> cur, prev;
    };
    void Worker();
    bool WriteAt(uint64_t off, const void* data, size_t n);
    // The journal: a header, then one record per frame on disk, the end time and the frame rate.
    struct JournalRecord {
        uint32_t type = 0;  // kRecFrame, kRecEnd, kRecFps
        uint32_t seq = 0;
        uint64_t offset = 0;  // kRecFps: the double's bits
        uint32_t size = 0, raw = 0;
        int64_t ts = 0;
    };
    void Journal(const JournalRecord& r);  // mtx_ held
    bool Load(const std::wstring& data, const std::wstring& journal);

    std::wstring path_;  // the data file; the journal is the same name with .idx
    HANDLE journal_ = INVALID_HANDLE_VALUE;
    uint64_t journalOffset_ = 0;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    int width_ = 0, height_ = 0;
    size_t frameBytes_ = 0;
    std::atomic<uint64_t> writeOffset_{0};
    std::atomic<uint32_t> dropped_{0};
    std::atomic<size_t> firstBad_{SIZE_MAX};  // lowest frame whose write failed
    int64_t endTs_ = 0;
    double nominalFps_ = 0;

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::condition_variable room_;  // a worker finished a frame
    std::deque<Job> jobs_;
    std::vector<Entry> entries_;
    std::vector<std::thread> workers_;
    std::shared_ptr<Buffer> last_;
    size_t inflight_ = 0, maxInflight_ = 32;
    bool stopping_ = false, finished_ = false;

    std::mutex poolMtx_;
    std::vector<std::unique_ptr<Buffer>> pool_;
};
