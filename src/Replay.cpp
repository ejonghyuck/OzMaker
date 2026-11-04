#include "pch.h"
#include "Replay.h"
#include "Mp4Unfragment.h"
#include "Util.h"
#include <wrl/implements.h>

using namespace mp4;

namespace {

int64_t To100ns(uint64_t t, uint32_t scale) {
    if (!scale) return 0;
    return (int64_t)(t / scale * 10'000'000 + t % scale * 10'000'000 / scale);
}

}  // namespace

// The sink writer's byte stream: an IStream over the ring, which Media Foundation wraps in an IMFByteStream.
class RingStream
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IStream> {
public:
    RingStream(ReplayRing* ring, uint64_t gen) : ring_(ring), gen_(gen) {}

    HRESULT STDMETHODCALLTYPE Read(void*, ULONG, ULONG* read) override {
        if (read) *read = 0;
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Write(const void* p, ULONG n, ULONG* written) override {
        {
            std::lock_guard lk(ring_->mtx_);
            if (ring_->gen_ != gen_) return STG_E_REVERTED;
        }
        ring_->Write(pos_, (const uint8_t*)p, n);
        pos_ += n;
        size_ = std::max(size_, pos_);
        if (written) *written = n;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* newPos) override {
        int64_t base = origin == STREAM_SEEK_SET ? 0 : origin == STREAM_SEEK_CUR ? (int64_t)pos_ : (int64_t)size_;
        int64_t to = base + move.QuadPart;
        if (to < 0) return STG_E_INVALIDFUNCTION;
        pos_ = (uint64_t)to;
        if (newPos) newPos->QuadPart = pos_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER size) override {
        size_ = size.QuadPart;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* st, DWORD) override {
        if (!st) return STG_E_INVALIDPOINTER;
        *st = {};
        st->type = STGTY_STREAM;
        st->cbSize.QuadPart = size_;
        st->grfMode = STGM_WRITE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }

private:
    ReplayRing* ring_;
    uint64_t gen_;
    uint64_t pos_ = 0, size_ = 0;
};

ComPtr<IMFByteStream> ReplayRing::NewStream() {
    uint64_t gen;
    {
        std::lock_guard lk(mtx_);
        gen = ++gen_;
        init_.clear();
        initDone_ = false;
        tail_.clear();
        tailPos_ = end_ = 0;
        moof_.clear();
        hasMoof_ = false;
        frags_.clear();
        bytes_ = 0;
        video_ = audio_ = {};
        videoEnd_ = soundEnd_ = 0;
        videoShift_ = 0;
        shiftKnown_ = false;
        rewrites_ = 0;
    }
    ComPtr<IStream> s = Microsoft::WRL::Make<RingStream>(this, gen);
    ComPtr<IMFByteStream> bs;
    if (!s || FAILED(MFCreateMFByteStreamOnStream(s.Get(), &bs))) return nullptr;
    return bs;
}

void ReplayRing::SetKeep(int64_t span) {
    std::lock_guard lk(mtx_);
    keep_ = span;
}

void ReplayRing::Clear() {
    std::lock_guard lk(mtx_);
    frags_.clear();
    bytes_ = 0;
}

int64_t ReplayRing::Held() const {
    std::lock_guard lk(mtx_);
    for (const Frag& f : frags_)
        if (f.firstKey != INT64_MAX) return videoEnd_ - f.firstKey;
    return 0;
}

uint64_t ReplayRing::Bytes() const {
    std::lock_guard lk(mtx_);
    return bytes_ + tail_.size();
}

int64_t ReplayRing::End() const {
    std::lock_guard lk(mtx_);
    return videoEnd_;
}

size_t ReplayRing::Fragments() const {
    std::lock_guard lk(mtx_);
    return frags_.size();
}

std::wstring ReplayRing::Describe() const {
    std::lock_guard lk(mtx_);
    std::wstring s;
    for (const Frag& f : frags_) {
        size_t keys = 0;
        for (const Sample& x : f.video) keys += x.Sync();
        s += util::Format(L"%.2f:v%zu%s/a%zu ", (f.video.empty() ? f.sound.empty() ? 0 : f.sound[0].start : f.video[0].start) / 1e7,
                          f.video.size(), keys ? L"k" : L"", f.sound.size());
    }
    return s;
}

void ReplayRing::Write(uint64_t pos, const uint8_t* p, size_t n) {
    std::lock_guard lk(mtx_);
    if (pos < end_) ++rewrites_;
    end_ = std::max(end_, pos + n);
    if (pos + n <= tailPos_) return;  // into a box already taken out whole
    if (pos < tailPos_) {
        size_t skip = (size_t)(tailPos_ - pos);
        p += skip;
        n -= skip;
        pos = tailPos_;
    }
    size_t at = (size_t)(pos - tailPos_);
    if (tail_.size() < at + n) tail_.resize(at + n);
    memcpy(&tail_[at], p, n);
    Parse();
}

void ReplayRing::Parse() {
    size_t p = 0;
    while (tail_.size() - p >= 8) {
        uint64_t size = Be32(&tail_[p]);
        size_t hdr = 8;
        if (size == 1) {
            if (tail_.size() - p < 16) break;
            size = Be64(&tail_[p + 8]);
            hdr = 16;
        }
        if (size < hdr || tail_.size() - p < size) break;  // 0 (up to the end) is never whole while writing
        const uint32_t type = Be32(&tail_[p + 4]);
        const uint64_t at = tailPos_ + p;
        if (type == kMoof) {
            moof_.assign(tail_.begin() + p, tail_.begin() + p + (size_t)size);
            moofPos_ = at;
            hasMoof_ = true;
        } else if (type == kMdat) {
            if (hasMoof_ && initDone_)
                AddFragment(moof_, moofPos_,
                            std::make_shared<mp4::Bytes>(tail_.begin() + p, tail_.begin() + p + (size_t)size));
            hasMoof_ = false;
        } else if (!initDone_ && (type == Tag("ftyp") || type == kMoov)) {
            Put(init_, &tail_[p], (size_t)size);
            if (type == kMoov) {
                initDone_ = true;
                ReadHeader();
            }
        }  // anything else (free, sidx, mfra) is left out
        p += (size_t)size;
    }
    if (p) {
        tail_.erase(tail_.begin(), tail_.begin() + p);
        tailPos_ += p;
    }
}

void ReplayRing::ReadHeader() {
    for (const Box& top : Children(init_, 0, init_.size())) {
        if (top.type != kMoov) continue;
        auto kids = Children(init_, top);
        for (const Box& t : kids) {
            if (t.type != kTrak) continue;
            auto tk = Children(init_, t);
            const Box* tkhd = Find(tk, kTkhd);
            const Box* mdia = Find(tk, kMdia);
            if (!tkhd || !mdia || tkhd->BodySize() < 24) continue;
            auto md = Children(init_, *mdia);
            const Box* mdhd = Find(md, kMdhd);
            const Box* hdlr = Find(md, Tag("hdlr"));
            if (!mdhd || !hdlr || mdhd->BodySize() < 24 || hdlr->BodySize() < 12) continue;
            const uint32_t handler = Be32(&init_[hdlr->Body() + 8]);
            TrackInfo* tr = handler == Tag("vide") ? &video_ : handler == Tag("soun") ? &audio_ : nullptr;
            if (!tr) continue;
            const uint8_t* h = &init_[tkhd->Body()];
            const uint8_t* m = &init_[mdhd->Body()];
            tr->id = Be32(h + (h[0] == 1 ? 20 : 12));
            tr->scale = Be32(m + (m[0] == 1 ? 20 : 12));
        }
        if (const Box* mvex = Find(kids, kMvex))
            for (const Box& x : Children(init_, *mvex)) {
                if (x.type != kTrex || x.BodySize() < 24) continue;
                const uint8_t* p = &init_[x.Body()];
                for (TrackInfo* tr : {&video_, &audio_})
                    if (tr->id && tr->id == Be32(p + 4)) {
                        tr->defDesc = Be32(p + 8);
                        tr->defDur = Be32(p + 12);
                        tr->defSize = Be32(p + 16);
                        tr->defFlags = Be32(p + 20);
                    }
            }
    }
}

void ReplayRing::AddFragment(const mp4::Bytes& b, uint64_t moofPos, std::shared_ptr<mp4::Bytes> mdat) {
    Frag f;
    const uint64_t mdatPos = moofPos + b.size(), mdatEnd = mdatPos + mdat->size();
    uint64_t prevEnd = moofPos;  // a traf without a base offset starts where the one before it ended
    for (const Box& top : Children(b, 0, b.size()))
        for (const Box& traf : Children(b, top)) {
            if (traf.type != kTraf) continue;
            auto kids = Children(b, traf);
            const Box* tfhd = Find(kids, kTfhd);
            if (!tfhd || tfhd->BodySize() < 8) continue;
            const uint8_t* p = &b[tfhd->Body()];
            const uint32_t fl = Be32(p) & 0xFFFFFF, id = Be32(p + 4);
            TrackInfo* tr = id == video_.id ? &video_ : id == audio_.id ? &audio_ : nullptr;
            size_t need = 8 + (fl & 0x1 ? 8 : 0) + (fl & 0x2 ? 4 : 0) + (fl & 0x8 ? 4 : 0) + (fl & 0x10 ? 4 : 0) +
                          (fl & 0x20 ? 4 : 0);
            if (!tr || tfhd->BodySize() < need) continue;
            const bool video = tr == &video_;
            p += 8;
            uint64_t base = fl & 0x20000 ? moofPos : prevEnd;  // default-base-is-moof
            if (fl & 0x1) base = Be64(p), p += 8;
            uint32_t desc = tr->defDesc, dDur = tr->defDur, dSize = tr->defSize, dFlags = tr->defFlags;
            if (fl & 0x2) desc = Be32(p), p += 4;
            if (fl & 0x8) dDur = Be32(p), p += 4;
            if (fl & 0x10) dSize = Be32(p), p += 4;
            if (fl & 0x20) dFlags = Be32(p);
            (video ? f.videoDesc : f.soundDesc) = desc;
            uint64_t data = base;
            for (const Box& run : kids) {
                if (run.type != kTrun || run.BodySize() < 8) continue;
                const uint8_t* r = &b[run.Body()];
                const uint8_t* stop = r + run.BodySize();
                const bool signedCto = r[0] != 0;
                const uint32_t rf = Be32(r) & 0xFFFFFF, count = Be32(r + 4);
                r += 8;
                if (rf & 0x1) {
                    if (stop - r < 4) break;
                    data = base + (uint64_t)(int64_t)(int32_t)Be32(r), r += 4;
                }
                const bool hasFirst = (rf & 0x4) != 0;
                uint32_t firstFlags = dFlags;
                if (hasFirst) {
                    if (stop - r < 4) break;
                    firstFlags = Be32(r), r += 4;
                }
                const size_t per = 4 * ((rf >> 8 & 1) + (rf >> 9 & 1) + (rf >> 10 & 1) + (rf >> 11 & 1));
                for (uint32_t i = 0; i < count && (size_t)(stop - r) >= per; ++i) {
                    Sample s;
                    s.dur = dDur;
                    s.size = dSize;
                    s.flags = i == 0 && hasFirst ? firstFlags : dFlags;
                    if (rf & 0x100) s.dur = Be32(r), r += 4;
                    if (rf & 0x200) s.size = Be32(r), r += 4;
                    if (rf & 0x400) s.flags = Be32(r), r += 4;
                    if (rf & 0x800) {
                        const uint32_t c = Be32(r);
                        s.cto = signedCto ? (int32_t)c : (int32_t)std::min<uint32_t>(c, INT32_MAX);
                        r += 4;
                    }
                    s.start = To100ns(tr->next, tr->scale);
                    tr->next += s.dur;
                    if (data >= mdatPos && data + s.size <= mdatEnd) {  // always, from Windows' sink
                        s.at = (size_t)(data - mdatPos);
                        if (video && s.Sync() && f.firstKey == INT64_MAX) f.firstKey = s.start;
                        (video ? f.video : f.sound).push_back(s);
                    }
                    data += s.size;
                }
            }
            prevEnd = data;
        }
    // Windows' sink writes no decode times (tfdt): each track's samples follow each other from 0.
    videoEnd_ = To100ns(video_.next, video_.scale);
    soundEnd_ = To100ns(audio_.next, audio_.scale);
    if (!shiftKnown_ && !f.video.empty()) {
        videoShift_ = INT64_MAX;
        for (const Sample& s : f.video)
            videoShift_ = std::min(videoShift_, s.start + To100ns((uint64_t)std::max(0, s.cto), video_.scale));
        shiftKnown_ = true;
    }
    if (f.video.empty() && f.sound.empty()) return;
    f.bytes = b.size() + mdat->size();
    f.mdat = std::move(mdat);
    bytes_ += f.bytes;
    frags_.push_back(std::move(f));

    // Drop the fragments before the one with the newest keyframe that is `keep_` or more before the end.
    size_t k = SIZE_MAX;
    for (size_t i = 0; i < frags_.size(); ++i)
        if (frags_[i].firstKey <= videoEnd_ - keep_) k = i;
    for (size_t i = 0; k != SIZE_MAX && i < k; ++i) {
        bytes_ -= frags_.front().bytes;
        frags_.pop_front();
    }
    grew_.notify_all();
}

bool ReplayRing::Save(const std::wstring& path, int64_t until, int64_t span, DWORD wait, double& seconds,
                      std::wstring& err) {
    mp4::Bytes init;
    std::vector<Frag> frags;
    uint32_t videoId = 0, videoScale = 0, soundId = 0, soundScale = 0;
    int64_t shift = 0;
    {
        std::unique_lock lk(mtx_);
        // The newest frames are still in the encoder and the sink writer: wait for the fragments up to `until`.
        grew_.wait_for(lk, std::chrono::milliseconds(wait), [&] {
            return videoEnd_ >= until && (!audio_.id || soundEnd_ >= until);
        });
        if (!initDone_ || frags_.empty()) {
            err = tr(L"아직 담은 장면이 없습니다.");
            return false;
        }
        init = init_;
        frags.assign(frags_.begin(), frags_.end());  // the samples are copied; the data is shared
        videoId = video_.id;
        videoScale = video_.scale;
        shift = videoShift_;
        soundId = audio_.id;
        soundScale = audio_.scale;
    }
    // From the newest keyframe at or before until - span (or the first one there is) to the frame that holds `until`.
    int64_t from = INT64_MAX, firstKey = INT64_MAX, to = INT64_MIN;
    for (const Frag& f : frags)
        for (const Sample& s : f.video) {
            if (s.Sync()) {
                firstKey = std::min(firstKey, s.start);
                if (s.start <= until - span) from = s.start;
            }
            if (s.start < until) to = std::max(to, s.start + To100ns(s.dur, videoScale));
        }
    if (from == INT64_MAX) from = firstKey;
    if (from == INT64_MAX || to <= from) {
        err = tr(L"아직 담은 장면이 없습니다.");
        return false;
    }
    seconds = (to - from) / 1e7;

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = tr(L"파일을 만들 수 없습니다. (") + util::Format(L"%lu", GetLastError()) + L")";
        return false;
    }
    bool ok = true;
    auto put = [&](const mp4::Bytes& b) {
        for (size_t done = 0; ok && done < b.size();) {
            DWORD n = (DWORD)std::min<size_t>(b.size() - done, 1 << 24), wrote = 0;
            ok = WriteFile(h, &b[done], n, &wrote, nullptr) && wrote == n;
            done += wrote;
        }
    };
    put(init);
    // Each fragment again with the samples that go in: the video in [from, to), the sound whose samples have their
    // middle in that span as shown (`shift`), so it starts and ends with the picture (within half an AAC frame).
    uint32_t seq = 0;
    for (const Frag& f : frags) {
        std::vector<const Sample*> v, a;
        for (const Sample& s : f.video)
            if (s.start >= from && s.start < to) v.push_back(&s);
        for (const Sample& s : f.sound) {
            const int64_t mid = s.start + To100ns(s.dur, soundScale) / 2;
            if (mid >= from - shift && mid < to - shift) a.push_back(&s);
        }
        if (v.empty() && a.empty()) continue;
        mp4::Bytes moof, mdat;
        size_t vOffset = 0, aOffset = 0, vBytes = 0;
        for (const Sample* s : v) vBytes += s->size;
        auto traf = [&](uint32_t id, uint32_t desc, const std::vector<const Sample*>& list, bool video,
                        size_t& offsetAt) {
            Wrap(moof, kTraf, [&] {
                Wrap(moof, kTfhd, [&] {
                    Put32(moof, 0x020000 | 0x2);  // default-base-is-moof, sample description
                    Put32(moof, id);
                    Put32(moof, desc);
                });
                Wrap(moof, kTrun, [&] {
                    // data offset, durations, sizes; for the video also flags and composition offsets (signed)
                    Put32(moof, video ? 1u << 24 | 0x1 | 0x100 | 0x200 | 0x400 | 0x800 : 0x1 | 0x100 | 0x200);
                    Put32(moof, (uint32_t)list.size());
                    offsetAt = moof.size();
                    Put32(moof, 0);
                    for (const Sample* s : list) {
                        Put32(moof, s->dur);
                        Put32(moof, s->size);
                        if (video) {
                            Put32(moof, s->flags);
                            Put32(moof, (uint32_t)s->cto);
                        }
                    }
                });
            });
        };
        Wrap(moof, kMoof, [&] {
            Wrap(moof, Tag("mfhd"), [&] {
                Put32(moof, 0);
                Put32(moof, ++seq);
            });
            if (!v.empty()) traf(videoId, f.videoDesc, v, true, vOffset);
            if (!a.empty()) traf(soundId, f.soundDesc, a, false, aOffset);
        });
        if (!v.empty()) Set32(&moof[vOffset], (uint32_t)(moof.size() + 8));
        if (!a.empty()) Set32(&moof[aOffset], (uint32_t)(moof.size() + 8 + vBytes));
        Wrap(mdat, kMdat, [&] {
            for (const Sample* s : v) Put(mdat, &(*f.mdat)[s->at], s->size);
            for (const Sample* s : a) Put(mdat, &(*f.mdat)[s->at], s->size);
        });
        put(moof);
        put(mdat);
    }
    if (!ok) err = tr(L"파일을 저장하지 못했습니다. 디스크 공간을 확인하세요.");
    CloseHandle(h);
    if (ok && !mp4::Unfragment(path, err)) ok = false;
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}
