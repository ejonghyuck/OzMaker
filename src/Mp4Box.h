#pragma once

// MP4 boxes in memory: reading them out of a buffer and writing new ones (Mp4Unfragment, Mp4Cut).
namespace mp4 {

using Bytes = std::vector<uint8_t>;

constexpr uint32_t Tag(const char (&s)[5]) {
    return (uint32_t)(uint8_t)s[0] << 24 | (uint32_t)(uint8_t)s[1] << 16 | (uint32_t)(uint8_t)s[2] << 8 |
           (uint8_t)s[3];
}
// In the moov's udta of a timelapse (#71): what 타임랩스 파일 잇기 goes by, since one looks like any 30 fps recording.
constexpr uint32_t kUdta = Tag("udta"), kTimelapseMark = Tag("OzTL");
constexpr uint32_t kMoov = Tag("moov"), kMoof = Tag("moof"), kMdat = Tag("mdat"), kFree = Tag("free"),
                   kMvhd = Tag("mvhd"), kMvex = Tag("mvex"), kTrex = Tag("trex"), kTrak = Tag("trak"),
                   kTkhd = Tag("tkhd"), kMdia = Tag("mdia"), kMdhd = Tag("mdhd"), kMinf = Tag("minf"),
                   kStbl = Tag("stbl"), kStsd = Tag("stsd"), kTraf = Tag("traf"), kTfhd = Tag("tfhd"),
                   kTrun = Tag("trun");

inline uint32_t Be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
inline uint64_t Be64(const uint8_t* p) { return (uint64_t)Be32(p) << 32 | Be32(p + 4); }
inline void Set32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (24 - 8 * i));
}
inline void Set64(uint8_t* p, uint64_t v) {
    Set32(p, (uint32_t)(v >> 32));
    Set32(p + 4, (uint32_t)v);
}
inline void Put32(Bytes& o, uint32_t v) {
    o.resize(o.size() + 4);
    Set32(&o[o.size() - 4], v);
}
inline void Put64(Bytes& o, uint64_t v) {
    o.resize(o.size() + 8);
    Set64(&o[o.size() - 8], v);
}
inline void Put(Bytes& o, const uint8_t* p, size_t n) { o.insert(o.end(), p, p + n); }

// A box in a buffer: [pos, pos + size), the first hdr bytes its header.
struct Box {
    uint32_t type = 0;
    size_t pos = 0, size = 0, hdr = 8;
    size_t Body() const { return pos + hdr; }
    size_t BodySize() const { return size - hdr; }
    size_t End() const { return pos + size; }
};

// The boxes in b[begin, end), up to the first one that does not fit.
inline std::vector<Box> Children(const Bytes& b, size_t begin, size_t end) {
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
inline std::vector<Box> Children(const Bytes& b, const Box& parent) {
    return Children(b, parent.Body(), parent.End());
}

inline const Box* Find(const std::vector<Box>& v, uint32_t type) {
    for (const Box& x : v)
        if (x.type == type) return &x;
    return nullptr;
}

inline void Copy(Bytes& o, const Bytes& b, const Box& x) { Put(o, &b[x.pos], x.size); }

// A box of `type` around what fill() writes.
template <class F>
inline void Wrap(Bytes& o, uint32_t type, F&& fill) {
    size_t at = o.size();
    Put32(o, 0);
    Put32(o, type);
    fill();
    Set32(&o[at], (uint32_t)(o.size() - at));
}

// mvhd, tkhd or mdhd with its duration set; made version 1 when the duration needs 64 bits.
inline void PutWithDuration(Bytes& o, const Bytes& b, const Box& x, uint64_t dur) {
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

// A track's samples, for a new sample table (PutStbl).
struct Track {
    uint32_t id = 0, timescale = 0;
    uint32_t defDesc = 1, defDur = 0, defSize = 0, defFlags = 0;  // trex (fragmented files)
    std::vector<uint32_t> dur, size, sync;                         // sync: 1-based sample numbers
    std::vector<int32_t> cto;
    std::vector<Chunk> chunks;  // runs of samples next to each other in the file (a fragment's: one per trun)
    bool cut = false;           // ran past the end of the file: nothing after that counts
    uint64_t Duration() const {
        uint64_t d = 0;
        for (uint32_t x : dur) d += x;
        return d;
    }
};

// (count, value) runs: the entries of stts and ctts.
template <class T>
inline void PutRuns(Bytes& o, const std::vector<T>& v) {
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

inline void PutStbl(Bytes& o, const Bytes& moov, const Box& stsd, const Track& t) {
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

}  // namespace mp4
