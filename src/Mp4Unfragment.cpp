#include "pch.h"
#include "Mp4Unfragment.h"
#include "Util.h"

namespace mp4 {
namespace {

using Bytes = std::vector<uint8_t>;

constexpr uint32_t Tag(const char (&s)[5]) {
    return (uint32_t)(uint8_t)s[0] << 24 | (uint32_t)(uint8_t)s[1] << 16 | (uint32_t)(uint8_t)s[2] << 8 |
           (uint8_t)s[3];
}
constexpr uint32_t kMoov = Tag("moov"), kMoof = Tag("moof"), kMdat = Tag("mdat"), kFree = Tag("free"),
                   kMvhd = Tag("mvhd"), kMvex = Tag("mvex"), kTrex = Tag("trex"), kTrak = Tag("trak"),
                   kTkhd = Tag("tkhd"), kMdia = Tag("mdia"), kMdhd = Tag("mdhd"), kMinf = Tag("minf"),
                   kStbl = Tag("stbl"), kStsd = Tag("stsd"), kTraf = Tag("traf"), kTfhd = Tag("tfhd"),
                   kTrun = Tag("trun");

// Top-level boxes that only mean something in a fragmented file.
bool FragmentOnly(uint32_t type) {
    return type == kMoof || type == Tag("mfra") || type == Tag("sidx") || type == Tag("styp");
}

uint32_t Be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint64_t Be64(const uint8_t* p) { return (uint64_t)Be32(p) << 32 | Be32(p + 4); }
void Set32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (24 - 8 * i));
}
void Set64(uint8_t* p, uint64_t v) {
    Set32(p, (uint32_t)(v >> 32));
    Set32(p + 4, (uint32_t)v);
}
void Put32(Bytes& o, uint32_t v) {
    o.resize(o.size() + 4);
    Set32(&o[o.size() - 4], v);
}
void Put64(Bytes& o, uint64_t v) {
    o.resize(o.size() + 8);
    Set64(&o[o.size() - 8], v);
}
void Put(Bytes& o, const uint8_t* p, size_t n) { o.insert(o.end(), p, p + n); }

// A box in a buffer: [pos, pos + size), the first hdr bytes its header.
struct Box {
    uint32_t type = 0;
    size_t pos = 0, size = 0, hdr = 8;
    size_t Body() const { return pos + hdr; }
    size_t BodySize() const { return size - hdr; }
    size_t End() const { return pos + size; }
};

// The boxes in b[begin, end), up to the first one that does not fit.
std::vector<Box> Children(const Bytes& b, size_t begin, size_t end) {
    std::vector<Box> out;
    for (size_t p = begin; p + 8 <= end;) {
        Box x;
        x.pos = p;
        x.type = Be32(&b[p + 4]);
        uint64_t size = Be32(&b[p]);
        if (size == 1) {
            if (p + 16 > end) break;
            size = Be64(&b[p + 8]);
            x.hdr = 16;
        } else if (size == 0) {
            size = end - p;
        }
        if (size < x.hdr || size > end - p) break;
        x.size = (size_t)size;
        out.push_back(x);
        p += x.size;
    }
    return out;
}
std::vector<Box> Children(const Bytes& b, const Box& parent) { return Children(b, parent.Body(), parent.End()); }

const Box* Find(const std::vector<Box>& v, uint32_t type) {
    for (const Box& x : v)
        if (x.type == type) return &x;
    return nullptr;
}

void Copy(Bytes& o, const Bytes& b, const Box& x) { Put(o, &b[x.pos], x.size); }

// A box of `type` around what fill() writes.
template <class F>
void Wrap(Bytes& o, uint32_t type, F&& fill) {
    size_t at = o.size();
    Put32(o, 0);
    Put32(o, type);
    fill();
    Set32(&o[at], (uint32_t)(o.size() - at));
}

// mvhd, tkhd or mdhd with its duration set; made version 1 when the duration needs 64 bits.
void PutWithDuration(Bytes& o, const Bytes& b, const Box& x, uint64_t dur) {
    const uint8_t* p = &b[x.Body()];
    size_t n = x.BodySize();
    bool v1 = n > 0 && p[0] == 1;
    // After the version, the creation and modification times: the time scale, or the track ID and a reserved word.
    size_t mid = x.type == kTkhd ? 8 : 4, durAt = 4 + (v1 ? 16 : 8) + mid;
    if (n < durAt + (v1 ? 8 : 4)) {
        Copy(o, b, x);
        return;
    }
    Wrap(o, x.type, [&] {
        size_t at = o.size();
        if (v1 || dur <= UINT32_MAX) {
            Put(o, p, n);
            if (v1)
                Set64(&o[at + durAt], dur);
            else
                Set32(&o[at + durAt], (uint32_t)dur);
            return;
        }
        Put32(o, 1u << 24 | (Be32(p) & 0xFFFFFF));
        Put64(o, Be32(p + 4));
        Put64(o, Be32(p + 8));
        Put(o, p + 12, mid);
        Put64(o, dur);
        Put(o, p + durAt + 4, n - durAt - 4);
    });
}

struct Chunk {
    uint64_t offset;
    uint32_t samples, desc;
};

struct Track {
    uint32_t id = 0, timescale = 0;
    uint32_t defDesc = 1, defDur = 0, defSize = 0, defFlags = 0;  // trex
    std::vector<uint32_t> dur, size, sync;                         // sync: 1-based sample numbers
    std::vector<int32_t> cto;
    std::vector<Chunk> chunks;  // one for each trun
    bool cut = false;           // ran past the end of the file: nothing after that counts
    uint64_t Duration() const {
        uint64_t d = 0;
        for (uint32_t x : dur) d += x;
        return d;
    }
};

// IDs and time scales from the traks, defaults from mvex.
bool ReadTracks(const Bytes& moov, const std::vector<Box>& kids, std::vector<Track>& tracks) {
    for (const Box& t : kids) {
        if (t.type != kTrak) continue;
        auto tk = Children(moov, t);
        const Box* tkhd = Find(tk, kTkhd);
        const Box* mdia = Find(tk, kMdia);
        if (!tkhd || !mdia) return false;
        auto md = Children(moov, *mdia);
        const Box* mdhd = Find(md, kMdhd);
        if (!mdhd || tkhd->BodySize() < 24 || mdhd->BodySize() < 24) return false;
        const uint8_t* h = &moov[tkhd->Body()];
        const uint8_t* m = &moov[mdhd->Body()];
        Track tr;
        tr.id = Be32(h + (h[0] == 1 ? 20 : 12));
        tr.timescale = Be32(m + (m[0] == 1 ? 20 : 12));
        tracks.push_back(tr);
    }
    if (const Box* mvex = Find(kids, kMvex))
        for (const Box& x : Children(moov, *mvex)) {
            if (x.type != kTrex || x.BodySize() < 24) continue;
            const uint8_t* p = &moov[x.Body()];
            for (Track& tr : tracks)
                if (tr.id == Be32(p + 4)) {
                    tr.defDesc = Be32(p + 8);
                    tr.defDur = Be32(p + 12);
                    tr.defSize = Be32(p + 16);
                    tr.defFlags = Be32(p + 20);
                }
        }
    return !tracks.empty();
}

// The samples of one moof (b, at `pos` in the file) onto their tracks. Samples past `end` never reached the disk.
void ReadFragment(const Bytes& b, size_t hdr, uint64_t pos, uint64_t end, std::vector<Track>& tracks) {
    uint64_t prevEnd = pos;  // a traf without a base offset starts where the one before it ended
    for (const Box& traf : Children(b, hdr, b.size())) {
        if (traf.type != kTraf) continue;
        auto kids = Children(b, traf);
        const Box* tfhd = Find(kids, kTfhd);
        if (!tfhd || tfhd->BodySize() < 8) continue;
        const uint8_t* p = &b[tfhd->Body()];
        uint32_t fl = Be32(p) & 0xFFFFFF;
        Track* tr = nullptr;
        for (Track& t : tracks)
            if (t.id == Be32(p + 4)) tr = &t;
        size_t need = 8 + (fl & 0x1 ? 8 : 0) + (fl & 0x2 ? 4 : 0) + (fl & 0x8 ? 4 : 0) + (fl & 0x10 ? 4 : 0) +
                      (fl & 0x20 ? 4 : 0);
        if (!tr || tfhd->BodySize() < need) continue;
        p += 8;
        auto next32 = [&] {
            uint32_t v = Be32(p);
            p += 4;
            return v;
        };
        uint64_t base = fl & 0x20000 ? pos : prevEnd;  // default-base-is-moof
        if (fl & 0x1) {
            base = Be64(p);
            p += 8;
        }
        uint32_t desc = tr->defDesc, dDur = tr->defDur, dSize = tr->defSize, dFlags = tr->defFlags;
        if (fl & 0x2) desc = next32();
        if (fl & 0x8) dDur = next32();
        if (fl & 0x10) dSize = next32();
        if (fl & 0x20) dFlags = next32();
        uint64_t data = base;
        for (const Box& run : kids) {
            if (run.type != kTrun || run.BodySize() < 8) continue;
            const uint8_t* r = &b[run.Body()];
            const size_t len = run.BodySize();
            uint32_t rf = Be32(r) & 0xFFFFFF, count = Be32(r + 4);
            bool signedCto = r[0] != 0, hasFirst = (rf & 0x4) != 0;
            if (len < 8 + (rf & 0x1 ? 4 : 0) + (hasFirst ? 4 : 0)) continue;
            r += 8;
            auto next = [&] {
                uint32_t v = Be32(r);
                r += 4;
                return v;
            };
            if (rf & 0x1) data = base + (uint64_t)(int64_t)(int32_t)next();
            uint32_t first = hasFirst ? next() : 0;
            size_t left = len - (size_t)(r - &b[run.Body()]);
            size_t per = 4 * ((rf >> 8 & 1) + (rf >> 9 & 1) + (rf >> 10 & 1) + (rf >> 11 & 1));
            count = per ? (uint32_t)std::min<size_t>(count, left / per) : std::min(count, 1u << 20);
            uint64_t start = data;
            uint32_t kept = 0;
            for (uint32_t i = 0; i < count && !tr->cut; ++i) {
                uint32_t dur = dDur, size = dSize, flags = i == 0 && hasFirst ? first : dFlags;
                int32_t cto = 0;
                if (rf & 0x100) dur = next();
                if (rf & 0x200) size = next();
                if (rf & 0x400) flags = next();
                if (rf & 0x800) {
                    uint32_t c = next();
                    cto = signedCto ? (int32_t)c : (int32_t)std::min<uint32_t>(c, INT32_MAX);
                }
                if (data > end || size > end - data) {
                    tr->cut = true;
                    break;
                }
                tr->dur.push_back(dur);
                tr->size.push_back(size);
                tr->cto.push_back(cto);
                if (!(flags & 0x10000)) tr->sync.push_back((uint32_t)tr->dur.size());  // not sample_is_non_sync
                data += size;
                ++kept;
            }
            if (kept) tr->chunks.push_back({start, kept, desc});
        }
        prevEnd = data;
    }
}

// (count, value) runs: the entries of stts and ctts.
template <class T>
void PutRuns(Bytes& o, const std::vector<T>& v) {
    size_t at = o.size();
    Put32(o, 0);
    uint32_t entries = 0;
    for (size_t i = 0, j; i < v.size(); i = j) {
        for (j = i; j < v.size() && v[j] == v[i]; ++j) {
        }
        Put32(o, (uint32_t)(j - i));
        Put32(o, (uint32_t)v[i]);
        ++entries;
    }
    Set32(&o[at], entries);
}

void PutStbl(Bytes& o, const Bytes& moov, const Box& stsd, const Track& t) {
    Wrap(o, kStbl, [&] {
        Copy(o, moov, stsd);
        Wrap(o, Tag("stts"), [&] {
            Put32(o, 0);
            PutRuns(o, t.dur);
        });
        bool anyCto = false, negative = false;
        for (int32_t c : t.cto) {
            anyCto |= c != 0;
            negative |= c < 0;
        }
        if (anyCto)
            Wrap(o, Tag("ctts"), [&] {
                Put32(o, negative ? 1u << 24 : 0);
                PutRuns(o, t.cto);
            });
        if (t.sync.size() < t.dur.size())  // no stss: every sample is a sync sample
            Wrap(o, Tag("stss"), [&] {
                Put32(o, 0);
                Put32(o, (uint32_t)t.sync.size());
                for (uint32_t s : t.sync) Put32(o, s);
            });
        Wrap(o, Tag("stsc"), [&] {
            Put32(o, 0);
            size_t at = o.size();
            Put32(o, 0);
            uint32_t entries = 0;
            for (size_t i = 0; i < t.chunks.size(); ++i) {
                if (i > 0 && t.chunks[i].samples == t.chunks[i - 1].samples && t.chunks[i].desc == t.chunks[i - 1].desc)
                    continue;
                Put32(o, (uint32_t)i + 1);
                Put32(o, t.chunks[i].samples);
                Put32(o, t.chunks[i].desc);
                ++entries;
            }
            Set32(&o[at], entries);
        });
        Wrap(o, Tag("stsz"), [&] {
            Put32(o, 0);
            bool same = !t.size.empty() && std::all_of(t.size.begin(), t.size.end(), [&](uint32_t s) {
                return s == t.size[0];
            });
            Put32(o, same ? t.size[0] : 0);
            Put32(o, (uint32_t)t.size.size());
            if (!same)
                for (uint32_t s : t.size) Put32(o, s);
        });
        bool big = std::any_of(t.chunks.begin(), t.chunks.end(), [](const Chunk& c) { return c.offset > UINT32_MAX; });
        Wrap(o, big ? Tag("co64") : Tag("stco"), [&] {
            Put32(o, 0);
            Put32(o, (uint32_t)t.chunks.size());
            for (const Chunk& c : t.chunks)
                if (big)
                    Put64(o, c.offset);
                else
                    Put32(o, (uint32_t)c.offset);
        });
    });
}

// The trak with its durations, and a sample table for what the fragments hold.
bool PutTrak(Bytes& o, const Bytes& moov, const Box& trak, const Track& t, uint64_t movieDur) {
    bool stbl = false;
    Wrap(o, kTrak, [&] {
        for (const Box& x : Children(moov, trak)) {
            if (x.type == kTkhd) {
                PutWithDuration(o, moov, x, movieDur);
            } else if (x.type != kMdia) {
                Copy(o, moov, x);
            } else {
                Wrap(o, kMdia, [&] {
                    for (const Box& y : Children(moov, x)) {
                        if (y.type == kMdhd) {
                            PutWithDuration(o, moov, y, t.Duration());
                        } else if (y.type != kMinf) {
                            Copy(o, moov, y);
                        } else {
                            Wrap(o, kMinf, [&] {
                                for (const Box& z : Children(moov, y)) {
                                    if (z.type != kStbl) {
                                        Copy(o, moov, z);
                                        continue;
                                    }
                                    auto st = Children(moov, z);
                                    if (const Box* stsd = Find(st, kStsd)) {
                                        PutStbl(o, moov, *stsd, t);
                                        stbl = true;
                                    }
                                }
                            });
                        }
                    }
                });
            }
        }
    });
    return stbl;
}

class File {
public:
    explicit File(const std::wstring& path)
        : h_(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr)) {}
    ~File() {
        if (Ok()) CloseHandle(h_);
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    bool Ok() const { return h_ != INVALID_HANDLE_VALUE; }
    uint64_t Size() const {
        LARGE_INTEGER s{};
        return GetFileSizeEx(h_, &s) ? (uint64_t)s.QuadPart : 0;
    }
    bool Read(uint64_t at, void* dst, size_t n) { return Io(at, dst, n, false); }
    bool Write(uint64_t at, const void* src, size_t n) { return Io(at, const_cast<void*>(src), n, true); }
    bool Rename(uint64_t box, uint32_t type) {
        uint8_t t[4];
        Set32(t, type);
        return Write(box + 4, t, 4);
    }
    bool Truncate(uint64_t at) {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)at;
        return SetFilePointerEx(h_, li, nullptr, FILE_BEGIN) && SetEndOfFile(h_);
    }

private:
    bool Io(uint64_t at, void* buf, size_t n, bool write) {
        auto* p = static_cast<uint8_t*>(buf);
        while (n > 0) {
            OVERLAPPED ov{};
            ov.Offset = (DWORD)at;
            ov.OffsetHigh = (DWORD)(at >> 32);
            DWORD chunk = (DWORD)std::min<size_t>(n, 1u << 24), done = 0;
            BOOL ok = write ? WriteFile(h_, p, chunk, &done, &ov) : ReadFile(h_, p, chunk, &done, &ov);
            if (!ok || done == 0) return false;
            p += done;
            at += done;
            n -= done;
        }
        return true;
    }

    HANDLE h_;
};

struct Top {
    uint32_t type;
    uint64_t pos, size;
    uint32_t hdr;
};

}  // namespace

bool Unfragment(const std::wstring& path, std::wstring& err, bool* empty) {
    if (empty) *empty = false;
    File f(path);
    if (!f.Ok()) {
        err = util::Format(L"파일을 열 수 없습니다. (%lu)", GetLastError());
        return false;
    }
    // Top-level boxes. A crash can cut the last one short: a cut mdat keeps what reached the disk, anything else goes.
    uint64_t end = f.Size(), p = 0;
    std::vector<Top> top;
    bool fixLast = false;  // the last box runs to the end of the file: its size goes in its header
    while (end - p >= 8) {
        uint8_t h[16];
        size_t n = (size_t)std::min<uint64_t>(16, end - p);
        if (!f.Read(p, h, n)) {
            err = L"파일을 읽을 수 없습니다.";
            return false;
        }
        uint32_t type = Be32(h + 4), hdr = 8;
        uint64_t size = Be32(h);
        if (size == 1) {
            if (n < 16) break;
            size = Be64(h + 8);
            hdr = 16;
        } else if (size == 0) {
            size = end - p;
            fixLast = true;
        }
        if (size < hdr || (size > end - p && type != kMdat)) break;
        if (size > end - p) {
            size = end - p;
            fixLast = true;
        }
        top.push_back({type, p, size, hdr});
        p += size;
    }

    // The fragmented moov (it has mvex) is what to convert. One without mvex is a plain file, or the moov an earlier
    // conversion appended before it was stopped: then only the renames are left.
    Bytes moov;
    const Top* src = nullptr;
    bool plain = false;
    std::vector<const Top*> fragMoovs;
    for (const Top& t : top) {
        if (t.type != kMoov || t.size > (64u << 20)) continue;
        Bytes b((size_t)t.size);
        if (!f.Read(t.pos, b.data(), b.size())) continue;
        if (!Find(Children(b, t.hdr, b.size()), kMvex)) {
            plain = true;
            continue;
        }
        fragMoovs.push_back(&t);
        if (!src) {
            src = &t;
            moov = std::move(b);
        }
    }
    auto renameFragments = [&] {
        for (const Top& t : top)
            if (FragmentOnly(t.type) && !f.Rename(t.pos, kFree)) return false;
        for (const Top* t : fragMoovs)
            if (!f.Rename(t->pos, kFree)) return false;
        return true;
    };
    if (plain) {
        if (!renameFragments()) {
            err = L"파일에 쓸 수 없습니다.";
            return false;
        }
        return true;
    }
    if (!src) {
        err = L"MP4 구조를 읽을 수 없습니다.";
        return false;
    }

    std::vector<Box> kids = Children(moov, src->hdr, moov.size());
    std::vector<Track> tracks;
    const Box* mvhd = Find(kids, kMvhd);
    if (!mvhd || mvhd->BodySize() < 24 || !ReadTracks(moov, kids, tracks)) {
        err = L"MP4 구조를 읽을 수 없습니다.";
        return false;
    }
    // What a crash cut off: the mdat ends where the file does, and a partial box after the last whole one goes.
    // The moov goes after the last box, so that box must say where it ends.
    if (fixLast) {
        const Top& last = top.back();
        uint8_t s[8];
        if (last.hdr == 16)
            Set64(s, last.size);
        else
            Set32(s, (uint32_t)last.size);
        if (last.hdr == 8 && last.size > UINT32_MAX) {
            err = L"MP4 구조를 읽을 수 없습니다.";
            return false;
        }
        if (!f.Write(last.pos + (last.hdr == 16 ? 8 : 0), s, last.hdr == 16 ? 8 : 4)) {
            err = L"파일에 쓸 수 없습니다.";
            return false;
        }
    }
    if (p < end && !f.Truncate(p)) {
        err = L"파일에 쓸 수 없습니다.";
        return false;
    }
    end = p;

    for (const Top& t : top) {
        if (t.type != kMoof || t.size > (64u << 20)) continue;
        Bytes b((size_t)t.size);
        if (f.Read(t.pos, b.data(), b.size())) ReadFragment(b, t.hdr, t.pos, end, tracks);
    }
    size_t samples = 0;
    for (const Track& t : tracks) samples += t.dur.size();
    if (samples == 0) {
        if (empty) *empty = true;
        err = L"저장된 화면이 없습니다.";
        return false;
    }

    const uint8_t* mh = &moov[mvhd->Body()];
    uint64_t movieScale = Be32(mh + (mh[0] == 1 ? 20 : 12)), movieDur = 0;
    std::vector<uint64_t> trackDur;
    for (const Track& t : tracks) {
        trackDur.push_back(t.timescale ? t.Duration() * movieScale / t.timescale : 0);
        movieDur = std::max(movieDur, trackDur.back());
    }
    Bytes o;
    bool ok = true;
    Wrap(o, kMoov, [&] {
        size_t k = 0;
        for (const Box& x : kids) {
            if (x.type == kMvex) continue;
            if (x.type == kMvhd) {
                PutWithDuration(o, moov, x, movieDur);
            } else if (x.type == kTrak) {
                ok &= PutTrak(o, moov, x, tracks[k], trackDur[k]);
                ++k;
            } else {
                Copy(o, moov, x);
            }
        }
    });
    if (!ok) {
        err = L"MP4 구조를 읽을 수 없습니다.";
        return false;
    }
    // The new moov first. Until the old one is renamed the file plays as it did, and if that is never done, the next
    // call finds both and only renames.
    if (!f.Write(end, o.data(), o.size()) || !renameFragments()) {
        err = L"파일에 쓸 수 없습니다.";
        return false;
    }
    return true;
}

}  // namespace mp4
