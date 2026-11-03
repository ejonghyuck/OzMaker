#include "pch.h"
#include "FrameStore.h"
#include "Settings.h"
#include "Util.h"

namespace {
constexpr char kMagic[4] = {'O', 'Z', 'F', 'S'};
constexpr uint32_t kVersion = 1;
constexpr uint32_t kRecFrame = 1, kRecEnd = 2, kRecFps = 3;

struct JournalHeader {
    char magic[4];
    uint32_t version;
    int32_t width, height;
    uint32_t keyInterval, reserved;
    uint64_t reserved2;
};
static_assert(sizeof(JournalHeader) == 32);

std::wstring JournalPath(const std::wstring& data) { return data.substr(0, data.find_last_of(L'.')) + L".idx"; }
}  // namespace

FrameStore::~FrameStore() {
    if (!finished_) Finish(endTs_);
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    if (journal_ != INVALID_HANDLE_VALUE) CloseHandle(journal_);
    if (!path_.empty()) {
        DeleteFileW(path_.c_str());
        DeleteFileW(JournalPath(path_).c_str());
    }
}

std::wstring FrameStore::Dir() {
    static const std::wstring dir = [] {
        wchar_t env[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"OZMAKER_SETTINGS", env, MAX_PATH);
        std::wstring d;
        if (n > 0 && n < MAX_PATH) {
            d = Settings::Dir() + L"\\recovery";
        } else {
            wchar_t tmp[MAX_PATH + 1] = {};
            GetTempPathW(_countof(tmp), tmp);
            d = std::wstring(tmp) + L"OzMaker";
        }
        util::EnsureDir(d);
        return d;
    }();
    return dir;
}

void FrameStore::SetNominalFps(double fps) {
    std::lock_guard lk(mtx_);
    nominalFps_ = fps;
    JournalRecord r;
    r.type = kRecFps;
    memcpy(&r.offset, &fps, sizeof(fps));
    Journal(r);
}

void FrameStore::Journal(const JournalRecord& r) {
    if (journal_ == INVALID_HANDLE_VALUE) return;
    OVERLAPPED ov{};
    ov.Offset = (DWORD)journalOffset_;
    ov.OffsetHigh = (DWORD)(journalOffset_ >> 32);
    DWORD written = 0;
    if (WriteFile(journal_, &r, sizeof(r), &written, &ov) && written == sizeof(r)) journalOffset_ += sizeof(r);
}

bool FrameStore::Open(int width, int height, std::wstring& err, bool journal) {
    width_ = width;
    height_ = height;
    frameBytes_ = (size_t)width * height * 4;

    path_ = util::Format(L"%s\\OzMaker_%lu_%llu.frames", Dir().c_str(), GetCurrentProcessId(),
                         (unsigned long long)util::Qpc100ns());
    // Without a journal nothing could take the file back: it goes when its handle does, even in a crash.
    file_ = CreateFileW(path_.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | (journal ? 0 : FILE_FLAG_DELETE_ON_CLOSE), nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        err = tr(L"임시 파일을 만들 수 없습니다.");
        return false;
    }
    if (journal) {
        journal_ = CreateFileW(JournalPath(path_).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        JournalHeader hdr{};
        memcpy(hdr.magic, kMagic, 4);
        hdr.version = kVersion;
        hdr.width = width;
        hdr.height = height;
        hdr.keyInterval = (uint32_t)kKeyInterval;
        DWORD written = 0;
        if (journal_ == INVALID_HANDLE_VALUE || !WriteFile(journal_, &hdr, sizeof(hdr), &written, nullptr) ||
            written != sizeof(hdr)) {
            err = tr(L"임시 파일을 만들 수 없습니다.");
            return false;  // the destructor closes and deletes what was made
        }
        journalOffset_ = sizeof(hdr);
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

bool FrameStore::Submit(std::shared_ptr<Buffer> frame, int64_t ts, bool wait) {
    {
        std::unique_lock lk(mtx_);
        if (wait) room_.wait(lk, [&] { return stopping_ || inflight_ < maxInflight_; });
        if (stopping_ || WriteFailed()) return false;
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
    room_.notify_all();
    for (auto& t : workers_) t.join();
    workers_.clear();
    last_.reset();
    {
        std::lock_guard lk(poolMtx_);
        pool_.clear();
    }
    std::lock_guard lk(mtx_);
    if (size_t bad = firstBad_.load(); bad < entries_.size()) {
        // Later frames are deltas on the lost one, or come after a gap: keep what precedes it.
        entries_.resize(bad);
        endTs = 0;
    }
    int64_t lastTs = entries_.empty() ? 0 : entries_.back().ts;
    endTs_ = std::max(endTs, lastTs + 10'000'000 / 60);
    JournalRecord r;
    r.type = kRecEnd;
    r.ts = endTs_;
    Journal(r);
    finished_ = true;
}

bool FrameStore::Load(const std::wstring& data, const std::wstring& journal) {
    HANDLE j = CreateFileW(journal.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (j == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(j, &size);
    std::vector<uint8_t> buf((size_t)std::min<int64_t>(size.QuadPart, 1ll << 30));
    DWORD read = 0;
    bool ok = !buf.empty() && ReadFile(j, buf.data(), (DWORD)buf.size(), &read, nullptr) && read == buf.size();
    CloseHandle(j);
    JournalHeader hdr{};
    if (!ok || buf.size() < sizeof(hdr)) return false;
    memcpy(&hdr, buf.data(), sizeof(hdr));
    if (memcmp(hdr.magic, kMagic, 4) != 0 || hdr.version != kVersion || hdr.width <= 0 || hdr.height <= 0 ||
        hdr.width > 16384 || hdr.height > 16384 || hdr.keyInterval != kKeyInterval)
        return false;

    file_ = CreateFileW(data.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) return false;
    path_ = data;
    LARGE_INTEGER dataSize{};
    GetFileSizeEx(file_, &dataSize);
    width_ = hdr.width;
    height_ = hdr.height;
    frameBytes_ = (size_t)width_ * height_ * 4;
    int64_t end = 0;
    for (size_t off = sizeof(hdr); off + sizeof(JournalRecord) <= buf.size(); off += sizeof(JournalRecord)) {
        JournalRecord r;
        memcpy(&r, buf.data() + off, sizeof(r));
        if (r.type == kRecFrame && r.size > 0 && r.offset + r.size <= (uint64_t)dataSize.QuadPart) {
            if (r.seq >= entries_.size()) entries_.resize((size_t)r.seq + 1);
            entries_[r.seq] = {r.offset, r.size, r.raw != 0, r.ts};
        } else if (r.type == kRecEnd) {
            end = r.ts;
        } else if (r.type == kRecFps) {
            memcpy(&nominalFps_, &r.offset, sizeof(nominalFps_));
        }
    }
    // Up to the first frame that never reached the disk: the later ones are deltas on it.
    size_t n = 0;
    while (n < entries_.size() && entries_[n].size > 0) ++n;
    entries_.resize(n);
    if (n == 0) return false;
    endTs_ = std::max(end, entries_.back().ts + 10'000'000 / 60);
    finished_ = true;
    return true;
}

std::vector<FrameStore::Recovered> FrameStore::Recover() {
    std::vector<Recovered> out;
    const std::wstring dir = Dir();
    std::vector<std::wstring> journals;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\OzMaker_*.idx").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        journals.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    auto ticks = [](const FILETIME& f) { return (int64_t)((uint64_t)f.dwHighDateTime << 32 | f.dwLowDateTime); };
    for (const std::wstring& name : journals) {
        DWORD pid = wcstoul(name.c_str() + 8, nullptr, 10);  // OzMaker_<pid>_<qpc>.idx
        if (pid == GetCurrentProcessId() || !util::ProcessGone(pid)) continue;
        std::wstring journal = dir + L"\\" + name;
        std::wstring data = journal.substr(0, journal.size() - 4) + L".frames";
        auto s = std::make_shared<FrameStore>();
        if (!s->Load(data, journal)) {
            s.reset();  // drops any handle it opened
            DeleteFileW(data.c_str());
            DeleteFileW(journal.c_str());
            continue;
        }
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        GetFileAttributesExW(journal.c_str(), GetFileExInfoStandard, &fa);
        out.push_back({std::move(s), std::max<int64_t>(0, ticks(nowFt) - ticks(fa.ftLastWriteTime)) / 1e7});
    }
    std::sort(out.begin(), out.end(), [](const Recovered& a, const Recovered& b) { return a.age < b.age; });
    return out;
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
    // Compression can wait; the game being recorded can't.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
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
        if (!WriteAt(off, data, size)) {
            size_t bad = firstBad_.load();
            while (job.seq < bad && !firstBad_.compare_exchange_weak(bad, job.seq)) {
            }
        }
        job.cur.reset();

        std::lock_guard lk(mtx_);
        Entry& e = entries_[job.seq];
        e.offset = off;
        e.size = (uint32_t)size;
        e.raw = raw;
        --inflight_;
        room_.notify_one();
        if (job.seq < firstBad_.load()) {
            JournalRecord r;
            r.type = kRecFrame;
            r.seq = (uint32_t)job.seq;
            r.offset = off;
            r.size = (uint32_t)size;
            r.raw = raw;
            r.ts = e.ts;
            Journal(r);
        }
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
