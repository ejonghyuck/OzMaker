#include "pch.h"
#include "FrameStore.h"
#include "Util.h"

FrameStore::~FrameStore() {
    if (!finished_) Finish(endTs_);
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
}

bool FrameStore::Open(int width, int height, std::wstring& err) {
    width_ = width;
    height_ = height;
    frameBytes_ = (size_t)width * height * 4;

    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    std::wstring path = util::Format(L"%sOzMaker_%lu_%llu.tmp", dir, GetCurrentProcessId(),
                                     (unsigned long long)util::Qpc100ns());
    file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        err = L"임시 파일을 만들 수 없습니다.";
        return false;
    }
    // Bound queued (uncompressed) frames to ~768 MB so a slow disk can't balloon memory.
    maxInflight_ = std::clamp<size_t>((768ull << 20) / std::max<size_t>(frameBytes_, 1), 4, 240);
    unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    unsigned n = std::clamp(hw / 4, 1u, 4u);
    for (unsigned i = 0; i < n; ++i) workers_.emplace_back([this] { Worker(); });
    return true;
}

std::shared_ptr<FrameStore::Buffer> FrameStore::Acquire() {
    std::unique_ptr<Buffer> b;
    {
        std::lock_guard lk(poolMtx_);
        if (!pool_.empty()) {
            b = std::move(pool_.back());
            pool_.pop_back();
        }
    }
    if (!b) b = std::make_unique<Buffer>(frameBytes_);
    return std::shared_ptr<Buffer>(b.release(), [this](Buffer* p) {
        std::lock_guard lk(poolMtx_);
        pool_.emplace_back(p);
    });
}

bool FrameStore::Submit(std::shared_ptr<Buffer> frame, int64_t ts) {
    {
        std::lock_guard lk(mtx_);
        if (stopping_) return false;
        if (inflight_ >= maxInflight_) {
            ++dropped_;
            return false;
        }
        Job job;
        job.seq = entries_.size();
        Entry e;
        e.ts = ts;
        entries_.push_back(e);
        if (job.seq % kKeyInterval != 0) job.prev = last_;
        job.cur = frame;
        last_ = std::move(frame);
        ++inflight_;
        jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
    return true;
}

void FrameStore::Finish(int64_t endTs) {
    {
        std::lock_guard lk(mtx_);
        if (finished_) return;
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
    workers_.clear();
    last_.reset();
    {
        std::lock_guard lk(poolMtx_);
        pool_.clear();
    }
    std::lock_guard lk(mtx_);
    int64_t lastTs = entries_.empty() ? 0 : entries_.back().ts;
    endTs_ = std::max(endTs, lastTs + 10'000'000 / 60);
    finished_ = true;
}

size_t FrameStore::Count() const {
    std::lock_guard lk(mtx_);
    return entries_.size();
}

bool FrameStore::WriteAt(uint64_t off, const void* data, size_t n) {
    OVERLAPPED ov{};
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD written = 0;
    return WriteFile(file_, data, (DWORD)n, &written, &ov) && written == n;
}

void FrameStore::Worker() {
    COMPRESSOR_HANDLE comp = nullptr;
    CreateCompressor(COMPRESS_ALGORITHM_XPRESS | COMPRESS_RAW, nullptr, &comp);
    std::vector<uint8_t> delta(frameBytes_);
    std::vector<uint8_t> out(frameBytes_ + frameBytes_ / 8 + 4096);

    for (;;) {
        Job job;
        {
            std::unique_lock lk(mtx_);
            cv_.wait(lk, [&] { return stopping_ || !jobs_.empty(); });
            if (jobs_.empty()) break;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        const uint8_t* src = job.cur->data();
        if (job.prev) {
            auto* a = reinterpret_cast<const uint64_t*>(job.cur->data());
            auto* b = reinterpret_cast<const uint64_t*>(job.prev->data());
            auto* d = reinterpret_cast<uint64_t*>(delta.data());
            size_t n = frameBytes_ / 8;
            for (size_t i = 0; i < n; ++i) d[i] = a[i] ^ b[i];
            for (size_t i = n * 8; i < frameBytes_; ++i) delta[i] = job.cur->data()[i] ^ job.prev->data()[i];
            src = delta.data();
        }
        job.prev.reset();

        SIZE_T outSize = 0;
        bool raw = !comp || !Compress(comp, src, frameBytes_, out.data(), out.size(), &outSize) ||
                   outSize >= frameBytes_;
        const uint8_t* data = raw ? src : out.data();
        size_t size = raw ? frameBytes_ : outSize;
        uint64_t off = writeOffset_.fetch_add(size);
        WriteAt(off, data, size);
        job.cur.reset();

        std::lock_guard lk(mtx_);
        Entry& e = entries_[job.seq];
        e.offset = off;
        e.size = (uint32_t)size;
        e.raw = raw;
        --inflight_;
    }
    if (comp) CloseCompressor(comp);
}

// ---------------------------------------------------------------- Reader

FrameStore::Reader::Reader(const FrameStore* s) : s_(s) {
    CreateDecompressor(COMPRESS_ALGORITHM_XPRESS | COMPRESS_RAW, nullptr, &dec_);
    cur_.resize(s_->frameBytes_);
    tmp_.resize(s_->frameBytes_);
}

FrameStore::Reader::~Reader() {
    if (dec_) CloseDecompressor(dec_);
}

bool FrameStore::Reader::Decode(size_t j, uint8_t* out) {
    const Entry& e = s_->entries_[j];
    if (e.size == 0) return false;
    uint8_t* target = e.raw ? out : (comp_.resize(e.size), comp_.data());
    OVERLAPPED ov{};
    ov.Offset = (DWORD)e.offset;
    ov.OffsetHigh = (DWORD)(e.offset >> 32);
    DWORD read = 0;
    if (!ReadFile(s_->file_, target, e.size, &read, &ov) || read != e.size) return false;
    if (e.raw) return true;
    SIZE_T outSize = 0;
    return dec_ && Decompress(dec_, comp_.data(), e.size, out, s_->frameBytes_, &outSize) &&
           outSize == s_->frameBytes_;
}

const uint8_t* FrameStore::Reader::Get(size_t index) {
    size_t count = s_->entries_.size();
    if (count == 0) return nullptr;
    index = std::min(index, count - 1);
    if (index == curIndex_) return cur_.data();

    size_t key = index - index % kKeyInterval;
    size_t start = (curIndex_ != SIZE_MAX && curIndex_ < index && curIndex_ >= key) ? curIndex_ + 1 : key;
    for (size_t j = start; j <= index; ++j) {
        if (j % kKeyInterval == 0) {
            if (!Decode(j, cur_.data())) memset(cur_.data(), 0, cur_.size());
        } else if (Decode(j, tmp_.data())) {
            auto* d = reinterpret_cast<uint64_t*>(cur_.data());
            auto* t = reinterpret_cast<const uint64_t*>(tmp_.data());
            size_t n = cur_.size() / 8;
            for (size_t i = 0; i < n; ++i) d[i] ^= t[i];
            for (size_t i = n * 8; i < cur_.size(); ++i) cur_[i] ^= tmp_[i];
        }
    }
    curIndex_ = index;
    return cur_.data();
}
