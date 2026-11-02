#pragma once
#include "Clip.h"

// Recorded frames live in a delete-on-close temp file, XOR-delta coded against the previous frame
// (keyframe every kKeyInterval) and XPRESS-compressed by a few worker threads. The OS file cache keeps
// recent data in RAM when available, so the process working set stays small even for long clips.
class FrameStore : public Clip {
public:
    static constexpr size_t kKeyInterval = 30;
    using Buffer = std::vector<uint8_t>;

    FrameStore() = default;
    ~FrameStore();
    FrameStore(const FrameStore&) = delete;
    FrameStore& operator=(const FrameStore&) = delete;

    bool Open(int width, int height, std::wstring& err);
    // Writer side (capture thread). Acquire a frame buffer, fill it with tightly packed BGRA, submit it.
    std::shared_ptr<Buffer> Acquire();
    bool Submit(std::shared_ptr<Buffer> frame, int64_t ts100ns);
    void Finish(int64_t endTs100ns);

    int Width() const override { return width_; }
    int Height() const override { return height_; }
    size_t FrameBytes() const { return frameBytes_; }
    size_t Count() const override;
    int64_t Ts(size_t i) const override { return entries_[i].ts; }
    int64_t EndTs() const override { return endTs_; }
    std::unique_ptr<FrameReader> NewReader() const override { return std::make_unique<Reader>(this); }
    uint64_t DiskBytes() const { return writeOffset_.load(); }
    uint32_t Dropped() const { return dropped_.load(); }

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

    HANDLE file_ = INVALID_HANDLE_VALUE;
    int width_ = 0, height_ = 0;
    size_t frameBytes_ = 0;
    std::atomic<uint64_t> writeOffset_{0};
    std::atomic<uint32_t> dropped_{0};
    int64_t endTs_ = 0;

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::vector<Entry> entries_;
    std::vector<std::thread> workers_;
    std::shared_ptr<Buffer> last_;
    size_t inflight_ = 0, maxInflight_ = 32;
    bool stopping_ = false, finished_ = false;

    std::mutex poolMtx_;
    std::vector<std::unique_ptr<Buffer>> pool_;
};
