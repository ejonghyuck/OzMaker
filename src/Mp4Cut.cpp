#include "pch.h"
#include "Mp4Cut.h"
#include "Mp4Box.h"

namespace mp4 {
namespace {

constexpr uint32_t kFtyp = Tag("ftyp"), kEdts = Tag("edts"), kElst = Tag("elst"), kTref = Tag("tref"),
                   kHdlr = Tag("hdlr");

struct File {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~File() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    bool Ok() const { return h != INVALID_HANDLE_VALUE; }
    bool ReadAt(uint64_t at, void* dst, size_t n) const {
        auto* p = static_cast<uint8_t*>(dst);
        while (n > 0) {
            OVERLAPPED ov{};
            ov.Offset = (DWORD)at;
            ov.OffsetHigh = (DWORD)(at >> 32);
            DWORD chunk = (DWORD)std::min<size_t>(n, 1u << 24), done = 0;
            if (!ReadFile(h, p, chunk, &done, &ov) || done == 0) return false;
            p += done;
            at += done;
            n -= done;
        }
        return true;
    }
    bool Write(const void* src, size_t n) const {
        auto* p = static_cast<const uint8_t*>(src);
        while (n > 0) {
            DWORD chunk = (DWORD)std::min<size_t>(n, 1u << 24), done = 0;
            if (!WriteFile(h, p, chunk, &done, nullptr) || done == 0) return false;
            p += done;
            n -= done;
        }
        return true;
    }
};

// The body of a box when it holds at least `need` bytes, else null.
const uint8_t* Body(const Bytes& b, const Box* x, size_t need) {
    return x && x->BodySize() >= need ? &b[x->Body()] : nullptr;
}

uint32_t HandlerOf(const Bytes& moov, const Box& trak) {
    auto tk = Children(moov, trak);
    const Box* mdia = Find(tk, kMdia);
    if (!mdia) return 0;
    auto md = Children(moov, *mdia);
    const uint8_t* h = Body(moov, Find(md, kHdlr), 12);
    return h ? Be32(h + 8) : 0;
}

}  // namespace

bool CutSource::ReadTrack(const Bytes& moov, size_t trak, uint32_t movieScale, TrackData& t, uint32_t& codec) {
    const Box tb = Children(moov, 0, moov.size())[trak];
    auto tk = Children(moov, tb);
    const Box* mdia = Find(tk, kMdia);
    if (!mdia) return false;
    auto md = Children(moov, *mdia);
    const Box* minf = Find(md, kMinf);
    const uint8_t* mh = Body(moov, Find(md, kMdhd), 24);
    if (!minf || !mh) return false;
    t.timescale = Be32(mh + (mh[0] == 1 ? 20 : 12));
    auto mi = Children(moov, *minf);
    const Box* stbl = Find(mi, kStbl);
    if (!stbl || !t.timescale) return false;
    auto st = Children(moov, *stbl);
    auto box = [&](uint32_t type) { return Find(st, type); };

    // One sample description, which the new file keeps as it is.
    const uint8_t* sd = Body(moov, box(kStsd), 16);
    if (!sd || Be32(sd + 4) != 1) return false;
    codec = Be32(sd + 12);

    // Durations (stts) give the decode times.
    const uint8_t* p = Body(moov, box(Tag("stts")), 8);
    if (!p) return false;
    uint32_t n = Be32(p + 4);
    if (box(Tag("stts"))->BodySize() < 8 + (size_t)n * 8) return false;
    auto& s = t.samples;
    s.clear();
    uint64_t dts = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t count = Be32(p + 8 + i * 8), dur = Be32(p + 12 + i * 8);
        if (s.size() + count > 50'000'000) return false;
        for (uint32_t k = 0; k < count; ++k, dts += dur) s.push_back({0, dts, 0, dur, 0, true});
    }
    if (s.empty()) return false;
    // Sizes (stsz; stz2 isn't handled).
    const Box* stsz = box(Tag("stsz"));
    p = Body(moov, stsz, 12);
    if (!p || Be32(p + 8) != s.size()) return false;
    if (uint32_t same = Be32(p + 4)) {
        for (auto& x : s) x.size = same;
    } else {
        if (stsz->BodySize() < 12 + s.size() * 4) return false;
        for (size_t i = 0; i < s.size(); ++i) s[i].size = Be32(p + 12 + i * 4);
    }
    // Composition offsets (ctts), signed in practice whatever the version.
    if (const Box* ctts = box(Tag("ctts"))) {
        p = Body(moov, ctts, 8);
        if (!p) return false;
        n = Be32(p + 4);
        if (ctts->BodySize() < 8 + (size_t)n * 8) return false;
        size_t k = 0;
        for (uint32_t i = 0; i < n && k < s.size(); ++i) {
            uint32_t count = Be32(p + 8 + i * 8);
            int32_t off = (int32_t)Be32(p + 12 + i * 8);
            for (uint32_t c = 0; c < count && k < s.size(); ++c) s[k++].cto = off;
        }
    }
    // Keyframes (stss; without it every sample is one).
    if (const Box* stss = box(Tag("stss"))) {
        p = Body(moov, stss, 8);
        if (!p) return false;
        n = Be32(p + 4);
        if (stss->BodySize() < 8 + (size_t)n * 4) return false;
        for (auto& x : s) x.sync = false;
        for (uint32_t i = 0; i < n; ++i)
            if (uint32_t k = Be32(p + 8 + i * 4); k >= 1 && k <= s.size()) s[k - 1].sync = true;
    }
    // Where each sample is: chunks (stco / co64), samples per chunk (stsc).
    std::vector<uint64_t> chunks;
    if (const Box* stco = box(Tag("stco"))) {
        p = Body(moov, stco, 8);
        if (!p || stco->BodySize() < 8 + (size_t)Be32(p + 4) * 4) return false;
        for (uint32_t i = 0; i < Be32(p + 4); ++i) chunks.push_back(Be32(p + 8 + i * 4));
    } else if (const Box* co64 = box(Tag("co64"))) {
        p = Body(moov, co64, 8);
        if (!p || co64->BodySize() < 8 + (size_t)Be32(p + 4) * 8) return false;
        for (uint32_t i = 0; i < Be32(p + 4); ++i) chunks.push_back(Be64(p + 8 + i * 8));
    } else {
        return false;
    }
    const Box* stsc = box(Tag("stsc"));
    p = Body(moov, stsc, 8);
    if (!p) return false;
    n = Be32(p + 4);
    if (n == 0 || stsc->BodySize() < 8 + (size_t)n * 12) return false;
    size_t next = 0;
    for (uint32_t e = 0; e < n; ++e) {
        uint32_t firstChunk = Be32(p + 8 + e * 12), per = Be32(p + 12 + e * 12);
        uint32_t endChunk = e + 1 < n ? Be32(p + 20 + e * 12) : (uint32_t)chunks.size() + 1;
        if (firstChunk < 1 || endChunk < firstChunk || endChunk > chunks.size() + 1) return false;
        for (uint32_t c = firstChunk; c < endChunk; ++c) {
            uint64_t at = chunks[c - 1];
            for (uint32_t k = 0; k < per && next < s.size(); ++k) {
                s[next].offset = at;
                at += s[next++].size;
            }
        }
    }
    if (next != s.size()) return false;

    // The edit: how long nothing plays first, and the media time shown first. More than that isn't handled.
    t.delay = 0;
    t.start = 0;
    if (const Box* edts = Find(tk, kEdts)) {
        auto ed = Children(moov, *edts);
        const Box* elst = Find(ed, kElst);
        p = Body(moov, elst, 8);
        if (!p) return false;
        const bool v1 = p[0] == 1;
        const size_t per = v1 ? 20 : 12;
        n = Be32(p + 4);
        if (elst->BodySize() < 8 + n * per) return false;
        int edits = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t* e = p + 8 + i * per;
            uint64_t dur = v1 ? Be64(e) : Be32(e);
            int64_t media = v1 ? (int64_t)Be64(e + 8) : (int32_t)Be32(e + 4);
            if (media == -1) {
                if (edits) return false;
                t.delay += dur / (double)movieScale;
                continue;
            }
            if ((int16_t)(Be32(e + (v1 ? 16 : 8)) >> 16) != 1 || ++edits > 1) return false;
            t.start = media;
        }
    }
    return true;
}

std::shared_ptr<const CutSource> CutSource::Open(const std::wstring& path) {
    File f;
    f.h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                      OPEN_EXISTING, 0, nullptr);
    LARGE_INTEGER size{};
    if (!f.Ok() || !GetFileSizeEx(f.h, &size)) return nullptr;
    auto c = std::make_shared<CutSource>();
    // A QuickTime file (or one without a file type) keeps its own kind of sample entries: those are encoded again.
    bool mp4 = false, fragmented = false;
    const uint64_t end = (uint64_t)size.QuadPart;
    for (uint64_t pos = 0; pos + 8 <= end;) {
        uint8_t h[16];
        if (!f.ReadAt(pos, h, 8)) return nullptr;
        uint64_t len = Be32(h), hdr = 8;
        if (len == 1) {
            if (!f.ReadAt(pos + 8, h + 8, 8)) return nullptr;
            len = Be64(h + 8);
            hdr = 16;
        } else if (len == 0) {
            len = end - pos;
        }
        if (len < hdr || len > end - pos) break;
        const uint32_t type = Be32(h + 4);
        if (type == kFtyp && len >= hdr + 4) {
            uint8_t brand[4];
            mp4 = f.ReadAt(pos + hdr, brand, 4) && Be32(brand) != Tag("qt  ");
        } else if (type == kMoov) {
            if (len - hdr > (256u << 20)) return nullptr;
            c->moov_.resize((size_t)(len - hdr));
            if (!f.ReadAt(pos + hdr, c->moov_.data(), c->moov_.size())) return nullptr;
        } else if (type == kMoof) {
            fragmented = true;
        }
        pos += len;
    }
    if (!mp4 || fragmented || c->moov_.empty()) return nullptr;
    const Bytes& moov = c->moov_;
    auto kids = Children(moov, 0, moov.size());
    const uint8_t* mh = Body(moov, Find(kids, kMvhd), 24);
    if (!mh || Find(kids, kMvex)) return nullptr;
    c->movieScale_ = Be32(mh + (mh[0] == 1 ? 20 : 12));
    if (!c->movieScale_) return nullptr;
    bool video = false, audio = false;
    for (size_t i = 0; i < kids.size(); ++i) {
        if (kids[i].type != kTrak) continue;
        const uint32_t handler = HandlerOf(moov, kids[i]);
        uint32_t codec = 0;
        // The first of each, as Media Foundation picks them.
        if (handler == Tag("vide") && !video) {
            if (!ReadTrack(moov, i, c->movieScale_, c->video_, codec)) return nullptr;
            c->avc_ = codec == Tag("avc1") || codec == Tag("avc3");
            if (!c->avc_ && codec != Tag("hvc1") && codec != Tag("hev1")) return nullptr;
            c->video_.trak = i;
            video = true;
        } else if (handler == Tag("soun") && !audio) {
            if (!ReadTrack(moov, i, c->movieScale_, c->audio_, codec) || codec != Tag("mp4a")) return nullptr;
            c->audio_.trak = i;
            audio = true;
        }
    }
    if (!video) return nullptr;
    // Frames in the order they show, as VideoClip numbers them.
    const auto& vs = c->video_.samples;
    c->shown_.resize(vs.size());
    for (size_t i = 0; i < vs.size(); ++i) c->shown_[i] = (uint32_t)i;
    std::stable_sort(c->shown_.begin(), c->shown_.end(), [&](uint32_t a, uint32_t b) {
        return (int64_t)vs[a].dts + vs[a].cto < (int64_t)vs[b].dts + vs[b].cto;
    });
    c->path_ = path;
    return c;
}

size_t CutSource::KeyBefore(size_t frame) const {
    if (shown_.empty()) return 0;
    for (size_t i = std::min(frame, shown_.size() - 1) + 1; i-- > 0;)
        if (video_.samples[shown_[i]].sync) return i;
    return 0;
}

// The new file needs no edit that skips into a track: Media Foundation reads that media time the other way round
// (it delays the track by it), so a cut that needs none plays alike everywhere.
struct CutSource::Plan {
    size_t v0 = 0, v1 = 0;   // the video samples kept, in decode order
    int32_t vShift = 0;      // off every composition offset, so the first frame shown is at 0
    size_t frames = 0;       // shown
    double seconds = 0;      // shown
    bool audio = false;
    size_t a0 = 0, a1 = 0;   // the sound samples kept
    double aDelay = 0;       // seconds before the sound starts
};

bool CutSource::MakePlan(size_t first, size_t last, bool sound, Plan& p) const {
    const size_t n = shown_.size();
    if (first > last || last >= n) return false;
    const auto& vs = video_.samples;
    auto pts = [&](size_t d) { return (int64_t)vs[d].dts + vs[d].cto; };
    // From the keyframe to the last sample a frame of the range needs. Frames after the range that come along (ones
    // the range refers to) are cut off by the edit.
    const size_t key = KeyBefore(first);
    p.v0 = p.v1 = shown_[key];
    for (size_t i = key; i <= last; ++i) {
        if (shown_[i] < p.v0) return false;  // it would need a sample before the keyframe
        p.v1 = std::max<size_t>(p.v1, shown_[i]);
    }
    const int64_t from = pts(p.v0);
    const int64_t to = last + 1 < n ? pts(shown_[last + 1]) : pts(shown_[last]) + vs[shown_[last]].dur;
    p.vShift = vs[p.v0].cto;
    if (to <= from) return false;
    const double vts = video_.timescale;
    p.frames = last - key + 1;
    p.seconds = (to - from) / vts;

    // The sound playing over the same stretch, going by where both tracks sit in the movie. It starts on the AAC
    // frame nearest the cut (no edit into it, see Plan): half a frame (11 ms) off at most.
    p.audio = false;
    if (!sound || audio_.samples.empty()) return true;
    const auto& as = audio_.samples;
    const double ats = audio_.timescale, t0 = video_.delay + (from - video_.start) / vts;
    int64_t a0 = std::llround(audio_.start + (t0 - audio_.delay) * ats);
    int64_t a1 = std::llround(audio_.start + (t0 + p.seconds - audio_.delay) * ats);
    if (a1 <= 0 || a0 >= audio_.End()) return true;
    p.aDelay = a0 < 0 ? -a0 / ats : 0;
    a0 = std::max<int64_t>(a0, 0);
    auto at = [&](int64_t t) {  // the sample playing at t
        auto it = std::upper_bound(as.begin(), as.end(), t,
                                   [](int64_t v, const Sample& s) { return v < (int64_t)s.dts; });
        return (size_t)std::max<ptrdiff_t>(0, it - as.begin() - 1);
    };
    const size_t s0 = at(a0);
    p.a0 = a0 - (int64_t)as[s0].dts > as[s0].dur / 2 && s0 + 1 < as.size() ? s0 + 1 : s0;
    p.a1 = std::max(p.a0, at(a1 - 1));
    p.audio = true;
    return true;
}

uint64_t CutSource::SizeOf(size_t first, size_t last, bool sound) const {
    Plan p;
    if (!MakePlan(first, last, sound, p)) return 0;
    uint64_t b = moov_.size() / 8 + 4096;  // the boxes around the samples, about
    for (size_t i = p.v0; i <= p.v1; ++i) b += video_.samples[i].size + 8;
    if (p.audio)
        for (size_t i = p.a0; i <= p.a1; ++i) b += audio_.samples[i].size + 8;
    return b;
}

bool CutSource::Cut(size_t first, size_t last, bool sound, const std::wstring& out, const std::atomic<bool>& cancel,
                    std::atomic<float>& progress, Result& res, std::wstring& err) const {
    Plan p;
    if (!MakePlan(first, last, sound, p)) {
        err = tr(L"이 구간은 다시 인코딩하지 않고 자를 수 없습니다.");
        return false;
    }
    // The kept samples of each track, as new sample tables.
    struct Kept {
        const TrackData* src;
        size_t a, b;
        int32_t shift;  // off the composition offsets
        double delay;
        Track t;
    };
    std::vector<Kept> kept = {{&video_, p.v0, p.v1, p.vShift, 0, {}}};
    if (p.audio) kept.push_back({&audio_, p.a0, p.a1, 0, p.aDelay, {}});
    // Chunks of about half a second, the tracks taking turns by time, so a player reads the file front to back.
    struct Run {
        size_t track, from, count;
        double t;
    };
    std::vector<Run> runs;
    uint64_t data = 0;
    for (size_t k = 0; k < kept.size(); ++k) {
        Kept& o = kept[k];
        const auto& s = o.src->samples;
        const double ts = o.src->timescale;
        for (size_t i = o.a; i <= o.b;) {
            size_t j = i;
            while (j <= o.b && (s[j].dts - s[i].dts) / ts < 0.5) ++j;
            runs.push_back({k, i, j - i, (s[i].dts - s[o.a].dts) / ts});
            i = j;
        }
        for (size_t i = o.a; i <= o.b; ++i) {
            o.t.dur.push_back(s[i].dur);
            o.t.size.push_back(s[i].size);
            o.t.cto.push_back(s[i].cto - o.shift);
            if (s[i].sync) o.t.sync.push_back((uint32_t)(i - o.a + 1));
            data += s[i].size;
        }
    }
    std::stable_sort(runs.begin(), runs.end(), [](const Run& x, const Run& y) { return x.t < y.t; });

    Bytes ftyp;
    Wrap(ftyp, kFtyp, [&] {
        Put32(ftyp, Tag("isom"));
        Put32(ftyp, 0x200);
        for (uint32_t brand : {Tag("isom"), Tag("iso2"), avc_ ? Tag("avc1") : Tag("iso6"), Tag("mp41")})
            Put32(ftyp, brand);
    });
    const uint64_t movieDur = (uint64_t)std::llround(p.seconds * movieScale_);
    auto putTrak = [&](Bytes& o, const Box& trak, const Kept& k) {
        const uint64_t lead = std::min(movieDur, (uint64_t)std::llround(k.delay * movieScale_));
        Wrap(o, kTrak, [&] {
            for (const Box& x : Children(moov_, trak)) {
                if (x.type == kTkhd) {
                    PutWithDuration(o, moov_, x, movieDur);
                    // What shows: the cut's length, after any wait for the sound to start.
                    Wrap(o, kEdts, [&] {
                        Wrap(o, kElst, [&] {
                            const bool v1 = movieDur > UINT32_MAX;
                            Put32(o, v1 ? 1u << 24 : 0);
                            Put32(o, lead ? 2 : 1);
                            auto entry = [&](uint64_t dur, int64_t media) {
                                if (v1) {
                                    Put64(o, dur);
                                    Put64(o, (uint64_t)media);
                                } else {
                                    Put32(o, (uint32_t)dur);
                                    Put32(o, (uint32_t)(int32_t)media);
                                }
                                Put32(o, 0x00010000);  // rate 1
                            };
                            if (lead) entry(lead, -1);
                            entry(movieDur - lead, 0);
                        });
                    });
                } else if (x.type == kMdia) {
                    Wrap(o, kMdia, [&] {
                        for (const Box& y : Children(moov_, x)) {
                            if (y.type == kMdhd) {
                                PutWithDuration(o, moov_, y, k.t.Duration());
                            } else if (y.type != kMinf) {
                                Copy(o, moov_, y);
                            } else {
                                Wrap(o, kMinf, [&] {
                                    for (const Box& z : Children(moov_, y)) {
                                        if (z.type != kStbl) {
                                            Copy(o, moov_, z);
                                            continue;
                                        }
                                        auto st = Children(moov_, z);
                                        PutStbl(o, moov_, *Find(st, kStsd), k.t);
                                    }
                                });
                            }
                        }
                    });
                } else if (x.type != kEdts && x.type != kTref) {
                    Copy(o, moov_, x);
                }
            }
        });
    };
    // The moov goes first (players can start before the whole file is there); the chunk offsets in it depend on its
    // own size, so it is laid out until that stays put.
    const uint32_t mdatHdr = data + 8 > UINT32_MAX ? 16 : 8;
    auto kids = Children(moov_, 0, moov_.size());
    Bytes moov;
    for (int tries = 0; tries < 4; ++tries) {
        const size_t was = moov.size();
        uint64_t at = ftyp.size() + was + mdatHdr;
        for (Kept& k : kept) k.t.chunks.clear();
        for (const Run& r : runs) {
            kept[r.track].t.chunks.push_back({at, (uint32_t)r.count, 1});
            for (size_t i = 0; i < r.count; ++i) at += kept[r.track].src->samples[r.from + i].size;
        }
        moov.clear();
        Wrap(moov, kMoov, [&] {
            for (size_t i = 0; i < kids.size(); ++i) {
                const Box& x = kids[i];
                if (x.type == kMvhd) {
                    PutWithDuration(moov, moov_, x, movieDur);
                } else if (x.type == kTrak) {
                    for (const Kept& k : kept)
                        if (k.src->trak == i) putTrak(moov, x, k);  // other tracks (text, timecode) are left out
                } else {
                    Copy(moov, moov_, x);
                }
            }
        });
        if (moov.size() == was) break;
    }

    File src, dst;
    src.h = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (!src.Ok()) {
        err = tr(L"원본 동영상을 열 수 없습니다.");
        return false;
    }
    dst.h = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    uint8_t hdr[16];
    if (mdatHdr == 16) {
        Set32(hdr, 1);
        Set32(hdr + 4, kMdat);
        Set64(hdr + 8, data + 16);
    } else {
        Set32(hdr, (uint32_t)(data + 8));
        Set32(hdr + 4, kMdat);
    }
    if (!dst.Ok() || !dst.Write(ftyp.data(), ftyp.size()) || !dst.Write(moov.data(), moov.size()) ||
        !dst.Write(hdr, mdatHdr)) {
        err = tr(L"파일을 쓸 수 없습니다.");
        return false;
    }
    std::vector<uint8_t> buf;
    uint64_t done = 0;
    for (const Run& r : runs) {
        if (cancel) return false;
        const auto& s = kept[r.track].src->samples;
        // Samples next to each other in the source go in one read.
        for (size_t i = r.from, end = r.from + r.count; i < end;) {
            size_t j = i + 1;
            size_t n = s[i].size;
            while (j < end && s[j].offset == s[j - 1].offset + s[j - 1].size && n + s[j].size <= (32u << 20))
                n += s[j++].size;
            buf.resize(n);
            if (!src.ReadAt(s[i].offset, buf.data(), n)) {
                err = tr(L"원본 동영상을 읽을 수 없습니다.");
                return false;
            }
            if (!dst.Write(buf.data(), n)) {
                err = tr(L"파일을 쓸 수 없습니다.");
                return false;
            }
            done += n;
            i = j;
        }
        progress = 0.98f * (float)done / std::max<uint64_t>(1, data);
    }
    res.frames = p.frames;
    res.seconds = p.seconds;
    res.audio = p.audio;
    return true;
}

}  // namespace mp4
