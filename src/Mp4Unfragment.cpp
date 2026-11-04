#include "pch.h"
#include "Mp4Unfragment.h"
#include "Mp4Box.h"
#include "Util.h"

namespace mp4 {
namespace {

// Top-level boxes that only mean something in a fragmented file.
bool FragmentOnly(uint32_t type) {
    return type == kMoof || type == Tag("mfra") || type == Tag("sidx") || type == Tag("styp");
}

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
        err = util::Format(tr(L"파일을 열 수 없습니다. (%lu)"), GetLastError());
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
            err = tr(L"파일을 읽을 수 없습니다.");
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
            err = tr(L"파일에 쓸 수 없습니다.");
            return false;
        }
        return true;
    }
    if (!src) {
        err = tr(L"MP4 구조를 읽을 수 없습니다.");
        return false;
    }

    std::vector<Box> kids = Children(moov, src->hdr, moov.size());
    std::vector<Track> tracks;
    const Box* mvhd = Find(kids, kMvhd);
    if (!mvhd || mvhd->BodySize() < 24 || !ReadTracks(moov, kids, tracks)) {
        err = tr(L"MP4 구조를 읽을 수 없습니다.");
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
            err = tr(L"MP4 구조를 읽을 수 없습니다.");
            return false;
        }
        if (!f.Write(last.pos + (last.hdr == 16 ? 8 : 0), s, last.hdr == 16 ? 8 : 4)) {
            err = tr(L"파일에 쓸 수 없습니다.");
            return false;
        }
    }
    if (p < end && !f.Truncate(p)) {
        err = tr(L"파일에 쓸 수 없습니다.");
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
        err = tr(L"저장된 화면이 없습니다.");
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
        err = tr(L"MP4 구조를 읽을 수 없습니다.");
        return false;
    }
    // The new moov first. Until the old one is renamed the file plays as it did, and if that is never done, the next
    // call finds both and only renames.
    if (!f.Write(end, o.data(), o.size()) || !renameFragments()) {
        err = tr(L"파일에 쓸 수 없습니다.");
        return false;
    }
    return true;
}

bool SetVideoTiming(const std::wstring& path, const std::vector<uint32_t>& dur100ns, std::wstring& err) {
    err = tr(L"MP4 구조를 읽을 수 없습니다.");
    File f(path);
    if (!f.Ok()) {
        err = util::Format(tr(L"파일을 열 수 없습니다. (%lu)"), GetLastError());
        return false;
    }
    const uint64_t end = f.Size();
    std::vector<Top> top;
    for (uint64_t p = 0; end - p >= 8;) {
        uint8_t h[16];
        const size_t n = (size_t)std::min<uint64_t>(16, end - p);
        if (!f.Read(p, h, n)) return false;
        uint32_t hdr = 8;
        uint64_t size = Be32(h);
        if (size == 1) {
            if (n < 16) return false;
            size = Be64(h + 8);
            hdr = 16;
        } else if (size == 0) {
            size = end - p;
        }
        if (size < hdr || size > end - p) return false;
        top.push_back({Be32(h + 4), p, size, hdr});
        p += size;
    }
    const Top* mt = nullptr;
    for (const Top& t : top)
        if (t.type == kMoov) {
            if (mt) return false;  // which one?
            mt = &t;
        }
    if (!mt || mt->size > (64u << 20)) return false;
    Bytes moov((size_t)mt->size);
    if (!f.Read(mt->pos, moov.data(), moov.size())) return false;
    const std::vector<Box> kids = Children(moov, mt->hdr, moov.size());
    const Box* mvhd = Find(kids, kMvhd);
    if (!mvhd || mvhd->BodySize() < 24 || Find(kids, kMvex)) return false;
    const uint8_t* mh = &moov[mvhd->Body()];
    const uint64_t movieScale = Be32(mh + (mh[0] == 1 ? 20 : 12));

    // The video trak, its time scale, and how many samples its stts counts.
    const Box* trak = nullptr;
    uint64_t scale = 0, samples = 0;
    for (const Box& t : kids) {
        if (t.type != kTrak) continue;
        auto tk = Children(moov, t);
        const Box* mdia = Find(tk, kMdia);
        auto md = mdia ? Children(moov, *mdia) : std::vector<Box>{};
        const Box* hdlr = Find(md, Tag("hdlr"));
        const Box* mdhd = Find(md, kMdhd);
        const Box* minf = Find(md, kMinf);
        if (!hdlr || hdlr->BodySize() < 12 || Be32(&moov[hdlr->Body() + 8]) != Tag("vide")) continue;
        if (trak || !mdhd || mdhd->BodySize() < 24 || !minf) return false;
        const uint8_t* m = &moov[mdhd->Body()];
        scale = Be32(m + (m[0] == 1 ? 20 : 12));
        const Box* stbl = Find(Children(moov, *minf), kStbl);
        auto st = stbl ? Children(moov, *stbl) : std::vector<Box>{};
        const Box* stts = Find(st, Tag("stts"));
        if (!stts || stts->BodySize() < 8) return false;
        const uint32_t entries = Be32(&moov[stts->Body() + 4]);
        if (stts->BodySize() < 8 + (size_t)entries * 8) return false;
        for (uint32_t i = 0; i < entries; ++i) samples += Be32(&moov[stts->Body() + 8 + i * 8]);
        if (const Box* ctts = Find(st, Tag("ctts"))) {
            const uint32_t n = ctts->BodySize() >= 8 ? Be32(&moov[ctts->Body() + 4]) : 0;
            for (uint32_t i = 0; i < n && ctts->BodySize() >= 16 + (size_t)i * 8; ++i)
                if (Be32(&moov[ctts->Body() + 12 + i * 8]) != 0) {
                    err = L"B-frames";
                    return false;
                }
        }
        trak = &t;
    }
    if (!trak || scale == 0 || movieScale == 0 || samples != dur100ns.size()) {
        if (trak && samples != dur100ns.size())
            err = util::Format(L"%llu frames in the file, %zu written", samples, dur100ns.size());
        return false;
    }

    // In the trak's time scale, rounded where each frame ends so the rounding doesn't add up.
    std::vector<uint32_t> dur(dur100ns.size());
    uint64_t at = 0, media = 0;
    for (size_t i = 0; i < dur.size(); ++i) {
        at += dur100ns[i];
        const uint64_t e = (at * scale + 5'000'000) / 10'000'000;
        dur[i] = (uint32_t)std::max<uint64_t>(1, e > media ? e - media : 0);
        media += dur[i];
    }
    const uint64_t videoDur = media * movieScale / scale;
    uint64_t movieDur = videoDur;
    for (const Box& t : kids) {  // the other tracks' lengths as they are
        if (t.type != kTrak || &t == trak) continue;
        auto tk = Children(moov, t);
        const Box* tkhd = Find(tk, kTkhd);
        if (!tkhd || tkhd->BodySize() < 24) continue;
        const uint8_t* h = &moov[tkhd->Body()];
        movieDur = std::max<uint64_t>(movieDur, h[0] == 1 ? (tkhd->BodySize() >= 36 ? Be64(h + 28) : 0) : Be32(h + 20));
    }

    Bytes o;
    Wrap(o, kMoov, [&] {
        for (const Box& x : kids) {
            if (x.type == kMvhd) {
                PutWithDuration(o, moov, x, movieDur);
                continue;
            }
            if (&x != trak) {
                Copy(o, moov, x);
                continue;
            }
            Wrap(o, kTrak, [&] {
                for (const Box& t : Children(moov, x)) {
                    if (t.type == kTkhd) {
                        PutWithDuration(o, moov, t, videoDur);
                    } else if (t.type == Tag("edts")) {
                        // none: the video starts at 0, as the sound does
                    } else if (t.type != kMdia) {
                        Copy(o, moov, t);
                    } else {
                        Wrap(o, kMdia, [&] {
                            for (const Box& y : Children(moov, t)) {
                                if (y.type == kMdhd) {
                                    PutWithDuration(o, moov, y, media);
                                } else if (y.type != kMinf) {
                                    Copy(o, moov, y);
                                } else {
                                    Wrap(o, kMinf, [&] {
                                        for (const Box& z : Children(moov, y)) {
                                            if (z.type != kStbl) {
                                                Copy(o, moov, z);
                                                continue;
                                            }
                                            Wrap(o, kStbl, [&] {
                                                for (const Box& s : Children(moov, z)) {
                                                    if (s.type != Tag("stts")) {
                                                        Copy(o, moov, s);
                                                        continue;
                                                    }
                                                    Wrap(o, Tag("stts"), [&] {
                                                        Put32(o, 0);
                                                        PutRuns(o, dur);
                                                    });
                                                }
                                            });
                                        }
                                    });
                                }
                            }
                        });
                    }
                }
            });
        }
    });
    // The moov is normally the last box: in its place. Otherwise the new one goes at the end and the old one is freed.
    const bool last = mt == &top.back();
    if (last ? !f.Write(mt->pos, o.data(), o.size()) || !f.Truncate(mt->pos + o.size())
             : !f.Write(end, o.data(), o.size()) || !f.Rename(mt->pos, kFree)) {
        err = tr(L"파일에 쓸 수 없습니다.");
        return false;
    }
    err.clear();
    return true;
}

}  // namespace mp4
