#include "pch.h"
#include "encoders/Encoder.h"

// GIF encoder tuned for screen/game clips:
//  - only pixels that changed since they were last drawn are re-encoded (the rest are transparent,
//    disposal = "keep"), cropped to the changed bounding box;
//  - each frame gets its own local palette (Wu quantizer + k-means refinement) built from the changed
//    pixels only, so small animated regions get very accurate colors;
//  - serpentine Floyd–Steinberg dithering limited to changed pixels to avoid temporal shimmer.

namespace {

struct Rgb {
    uint8_t r, g, b;
    bool operator==(const Rgb& o) const { return r == o.r && g == o.g && b == o.b; }
};

inline int ColorDist(int dr, int dg, int db) { return 2 * dr * dr + 4 * dg * dg + 3 * db * db; }

// ---------------------------------------------------------------- Wu color quantizer (1991)
class WuQuantizer {
public:
    WuQuantizer() : wt_(kSize), mr_(kSize), mg_(kSize), mb_(kSize), m2_(kSize) {}

    void Reset() {
        std::fill(wt_.begin(), wt_.end(), 0);
        std::fill(mr_.begin(), mr_.end(), 0);
        std::fill(mg_.begin(), mg_.end(), 0);
        std::fill(mb_.begin(), mb_.end(), 0);
        std::fill(m2_.begin(), m2_.end(), 0.0);
    }

    void Add(int r, int g, int b) {
        int i = Idx((r >> 3) + 1, (g >> 3) + 1, (b >> 3) + 1);
        wt_[i] += 1;
        mr_[i] += r;
        mg_[i] += g;
        mb_[i] += b;
        m2_[i] += (double)(r * r + g * g + b * b);
    }

    int Build(int maxColors, Rgb* out) {
        Moments();
        std::vector<Box> cubes(maxColors);
        std::vector<double> vv(maxColors, 0.0);
        cubes[0] = {0, 32, 0, 32, 0, 32, 32 * 32 * 32};
        int next = 0, k = maxColors;
        for (int i = 1; i < k; ++i) {
            if (Cut(cubes[next], cubes[i])) {
                vv[next] = cubes[next].vol > 1 ? Var(cubes[next]) : 0.0;
                vv[i] = cubes[i].vol > 1 ? Var(cubes[i]) : 0.0;
            } else {
                vv[next] = 0.0;
                --i;
            }
            next = 0;
            double temp = vv[0];
            for (int j = 1; j <= i; ++j)
                if (vv[j] > temp) {
                    temp = vv[j];
                    next = j;
                }
            if (temp <= 0.0) {
                k = i + 1;
                break;
            }
        }
        int n = 0;
        for (int i = 0; i < k; ++i) {
            int64_t w = Vol(cubes[i], wt_);
            if (w <= 0) continue;
            out[n++] = {(uint8_t)(Vol(cubes[i], mr_) / w), (uint8_t)(Vol(cubes[i], mg_) / w),
                        (uint8_t)(Vol(cubes[i], mb_) / w)};
        }
        return n;
    }

private:
    static constexpr int N = 33, kSize = N * N * N;
    enum Dir { Red, Green, Blue };
    struct Box { int r0, r1, g0, g1, b0, b1, vol; };
    static int Idx(int r, int g, int b) { return (r * N + g) * N + b; }

    void Moments() {
        for (int r = 1; r < N; ++r) {
            int64_t area[N] = {}, areaR[N] = {}, areaG[N] = {}, areaB[N] = {};
            double area2[N] = {};
            for (int g = 1; g < N; ++g) {
                int64_t line = 0, lineR = 0, lineG = 0, lineB = 0;
                double line2 = 0;
                for (int b = 1; b < N; ++b) {
                    int i = Idx(r, g, b);
                    line += wt_[i];
                    lineR += mr_[i];
                    lineG += mg_[i];
                    lineB += mb_[i];
                    line2 += m2_[i];
                    area[b] += line;
                    areaR[b] += lineR;
                    areaG[b] += lineG;
                    areaB[b] += lineB;
                    area2[b] += line2;
                    int j = Idx(r - 1, g, b);
                    wt_[i] = wt_[j] + area[b];
                    mr_[i] = mr_[j] + areaR[b];
                    mg_[i] = mg_[j] + areaG[b];
                    mb_[i] = mb_[j] + areaB[b];
                    m2_[i] = m2_[j] + area2[b];
                }
            }
        }
    }

    template <typename T>
    static T Vol(const Box& c, const std::vector<T>& m) {
        return m[Idx(c.r1, c.g1, c.b1)] - m[Idx(c.r1, c.g1, c.b0)] - m[Idx(c.r1, c.g0, c.b1)] +
               m[Idx(c.r1, c.g0, c.b0)] - m[Idx(c.r0, c.g1, c.b1)] + m[Idx(c.r0, c.g1, c.b0)] +
               m[Idx(c.r0, c.g0, c.b1)] - m[Idx(c.r0, c.g0, c.b0)];
    }

    static int64_t Bottom(const Box& c, Dir d, const std::vector<int64_t>& m) {
        switch (d) {
            case Red:
                return -m[Idx(c.r0, c.g1, c.b1)] + m[Idx(c.r0, c.g1, c.b0)] + m[Idx(c.r0, c.g0, c.b1)] -
                       m[Idx(c.r0, c.g0, c.b0)];
            case Green:
                return -m[Idx(c.r1, c.g0, c.b1)] + m[Idx(c.r1, c.g0, c.b0)] + m[Idx(c.r0, c.g0, c.b1)] -
                       m[Idx(c.r0, c.g0, c.b0)];
            default:
                return -m[Idx(c.r1, c.g1, c.b0)] + m[Idx(c.r1, c.g0, c.b0)] + m[Idx(c.r0, c.g1, c.b0)] -
                       m[Idx(c.r0, c.g0, c.b0)];
        }
    }

    static int64_t Top(const Box& c, Dir d, int pos, const std::vector<int64_t>& m) {
        switch (d) {
            case Red:
                return m[Idx(pos, c.g1, c.b1)] - m[Idx(pos, c.g1, c.b0)] - m[Idx(pos, c.g0, c.b1)] +
                       m[Idx(pos, c.g0, c.b0)];
            case Green:
                return m[Idx(c.r1, pos, c.b1)] - m[Idx(c.r1, pos, c.b0)] - m[Idx(c.r0, pos, c.b1)] +
                       m[Idx(c.r0, pos, c.b0)];
            default:
                return m[Idx(c.r1, c.g1, pos)] - m[Idx(c.r1, c.g0, pos)] - m[Idx(c.r0, c.g1, pos)] +
                       m[Idx(c.r0, c.g0, pos)];
        }
    }

    double Var(const Box& c) const {
        double dr = (double)Vol(c, mr_), dg = (double)Vol(c, mg_), db = (double)Vol(c, mb_);
        double xx = Vol(c, m2_);
        return xx - (dr * dr + dg * dg + db * db) / (double)Vol(c, wt_);
    }

    double Maximize(const Box& c, Dir d, int first, int last, int* cut, int64_t wr, int64_t wg, int64_t wb,
                    int64_t ww) const {
        int64_t br = Bottom(c, d, mr_), bg = Bottom(c, d, mg_), bb = Bottom(c, d, mb_), bw = Bottom(c, d, wt_);
        double best = 0.0;
        *cut = -1;
        for (int i = first; i < last; ++i) {
            double hr = (double)(br + Top(c, d, i, mr_)), hg = (double)(bg + Top(c, d, i, mg_)),
                   hb = (double)(bb + Top(c, d, i, mb_));
            int64_t hw = bw + Top(c, d, i, wt_);
            if (hw == 0) continue;
            double temp = (hr * hr + hg * hg + hb * hb) / hw;
            hr = wr - hr;
            hg = wg - hg;
            hb = wb - hb;
            hw = ww - hw;
            if (hw == 0) continue;
            temp += (hr * hr + hg * hg + hb * hb) / hw;
            if (temp > best) {
                best = temp;
                *cut = i;
            }
        }
        return best;
    }

    bool Cut(Box& a, Box& b) const {
        int64_t wr = Vol(a, mr_), wg = Vol(a, mg_), wb = Vol(a, mb_), ww = Vol(a, wt_);
        int cr, cg, cb;
        double maxr = Maximize(a, Red, a.r0 + 1, a.r1, &cr, wr, wg, wb, ww);
        double maxg = Maximize(a, Green, a.g0 + 1, a.g1, &cg, wr, wg, wb, ww);
        double maxb = Maximize(a, Blue, a.b0 + 1, a.b1, &cb, wr, wg, wb, ww);
        Dir dir;
        if (maxr >= maxg && maxr >= maxb) {
            dir = Red;
            if (cr < 0) return false;
        } else if (maxg >= maxr && maxg >= maxb) {
            dir = Green;
        } else {
            dir = Blue;
        }
        b.r1 = a.r1;
        b.g1 = a.g1;
        b.b1 = a.b1;
        switch (dir) {
            case Red: b.r0 = a.r1 = cr; b.g0 = a.g0; b.b0 = a.b0; break;
            case Green: b.g0 = a.g1 = cg; b.r0 = a.r0; b.b0 = a.b0; break;
            case Blue: b.b0 = a.b1 = cb; b.r0 = a.r0; b.g0 = a.g0; break;
        }
        a.vol = (a.r1 - a.r0) * (a.g1 - a.g0) * (a.b1 - a.b0);
        b.vol = (b.r1 - b.r0) * (b.g1 - b.g0) * (b.b1 - b.b0);
        return true;
    }

    std::vector<int64_t> wt_, mr_, mg_, mb_;
    std::vector<double> m2_;
};

// ---------------------------------------------------------------- LZW (GIF variant)
class LzwEncoder {
public:
    LzwEncoder() : keys_(kHash), codes_(kHash), gens_(kHash, 0) {}

    void Encode(const uint8_t* data, size_t n, int minCodeSize, std::vector<uint8_t>& out) {
        out_ = &out;
        bitBuf_ = 0;
        bitCount_ = 0;
        blockLen_ = 0;
        out.push_back((uint8_t)minCodeSize);
        const int clear = 1 << minCodeSize, eoi = clear + 1;
        int codeSize = minCodeSize + 1, maxCode = eoi;
        NewTable();
        Put(clear, codeSize);
        if (n > 0) {
            int prefix = data[0];
            for (size_t i = 1; i < n; ++i) {
                int k = data[i];
                uint32_t key = ((uint32_t)prefix << 8) | (uint32_t)k;
                uint32_t h = Hash(key);
                bool found = false;
                while (gens_[h] == gen_) {
                    if (keys_[h] == key) {
                        prefix = codes_[h];
                        found = true;
                        break;
                    }
                    h = (h + 1) & (kHash - 1);
                }
                if (found) continue;
                Put(prefix, codeSize);
                ++maxCode;
                gens_[h] = gen_;
                keys_[h] = key;
                codes_[h] = (uint16_t)maxCode;
                if (maxCode >= (1 << codeSize)) ++codeSize;
                if (maxCode == 4095) {
                    Put(clear, codeSize);
                    NewTable();
                    codeSize = minCodeSize + 1;
                    maxCode = eoi;
                }
                prefix = k;
            }
            Put(prefix, codeSize);
        }
        Put(eoi, codeSize);
        if (bitCount_ > 0) Byte((uint8_t)(bitBuf_ & 0xFF));
        FlushBlock();
        out.push_back(0);
    }

private:
    static constexpr uint32_t kHash = 1 << 14;
    static uint32_t Hash(uint32_t key) { return (key * 2654435761u) >> 18; }
    void NewTable() {
        if (++gen_ == 0) {
            std::fill(gens_.begin(), gens_.end(), 0);
            gen_ = 1;
        }
    }
    void Put(int code, int size) {
        bitBuf_ |= (uint32_t)code << bitCount_;
        bitCount_ += size;
        while (bitCount_ >= 8) {
            Byte((uint8_t)(bitBuf_ & 0xFF));
            bitBuf_ >>= 8;
            bitCount_ -= 8;
        }
    }
    void Byte(uint8_t b) {
        block_[blockLen_++] = b;
        if (blockLen_ == 255) FlushBlock();
    }
    void FlushBlock() {
        if (!blockLen_) return;
        out_->push_back((uint8_t)blockLen_);
        out_->insert(out_->end(), block_, block_ + blockLen_);
        blockLen_ = 0;
    }

    std::vector<uint32_t> keys_;
    std::vector<uint16_t> codes_;
    std::vector<uint32_t> gens_;
    uint32_t gen_ = 0;
    std::vector<uint8_t>* out_ = nullptr;
    uint32_t bitBuf_ = 0;
    int bitCount_ = 0;
    uint8_t block_[255];
    int blockLen_ = 0;
};

// ---------------------------------------------------------------- encoder
class GifEncoder final : public IEncoder {
public:
    bool Begin(const EncoderParams& p, std::wstring& err) override {
        w_ = p.width;
        h_ = p.height;
        if (w_ <= 0 || h_ <= 0 || w_ > 65535 || h_ > 65535) {
            err = L"GIF 크기가 올바르지 않습니다.";
            return false;
        }
        int q = std::clamp(p.quality, 1, 100);
        threshold_ = (int)std::lround((100 - q) * 0.22);
        maxColors_ = q >= 70 ? 256 : 64 + (int)(q / 70.0 * 192);
        dither_ = q >= 60 ? 0.85f : 0.5f + q / 60.f * 0.35f;

        if (_wfopen_s(&f_, p.path.c_str(), L"wb") || !f_) {
            err = L"파일을 열 수 없습니다: " + p.path;
            return false;
        }
        setvbuf(f_, nullptr, _IOFBF, 1 << 20);
        size_t n = (size_t)w_ * h_;
        ref_.assign(n * 3, 0);
        comp_.assign(n * 3, 0);
        changed_.assign(n, 0);
        cache_.assign(1 << 18, 0xFFFF);

        uint8_t hdr[13] = {'G', 'I', 'F', '8', '9', 'a', (uint8_t)(w_ & 255), (uint8_t)(w_ >> 8),
                           (uint8_t)(h_ & 255), (uint8_t)(h_ >> 8), 0x70, 0, 0};
        fwrite(hdr, 1, sizeof(hdr), f_);
        // Without the NETSCAPE block a GIF plays once; with it, once plus the loop count (0 = forever).
        if (p.plays != 1) {
            int repeats = p.plays > 1 ? std::min(p.plays - 1, 0xFFFF) : 0;
            const uint8_t app[] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 3, 1,
                                   (uint8_t)(repeats & 255), (uint8_t)(repeats >> 8), 0};
            fwrite(app, 1, sizeof(app), f_);
        }
        return true;
    }

    bool Add(const uint8_t* bgra, double durationMs, std::wstring&) override {
        double start = timeMs_;
        timeMs_ += std::max(0.0, durationMs);

        int x0 = w_, y0 = h_, x1 = -1, y1 = -1;
        size_t nChanged = 0;
        for (int y = 0; y < h_; ++y) {
            const uint8_t* s = bgra + (size_t)y * w_ * 4;
            const uint8_t* r = ref_.data() + (size_t)y * w_ * 3;
            uint8_t* c = changed_.data() + (size_t)y * w_;
            for (int x = 0; x < w_; ++x, s += 4, r += 3) {
                bool ch = first_;
                if (!ch) {
                    int d = std::max({std::abs(s[2] - r[0]), std::abs(s[1] - r[1]), std::abs(s[0] - r[2])});
                    ch = d > threshold_;
                }
                c[x] = ch;
                if (ch) {
                    ++nChanged;
                    x0 = std::min(x0, x);
                    x1 = std::max(x1, x);
                    y0 = std::min(y0, y);
                    y1 = std::max(y1, y);
                }
            }
        }
        if (nChanged == 0) return true;  // identical: previous frame simply stays longer

        FlushPending(start);
        pendingStartMs_ = start;
        EncodeFrame(bgra, x0, y0, x1, y1, nChanged);
        first_ = false;
        return true;
    }

    bool End(std::wstring& err) override {
        FlushPending(timeMs_);
        fputc(0x3B, f_);
        bool ok = ferror(f_) == 0;
        ok = fclose(f_) == 0 && ok;
        f_ = nullptr;
        if (!ok) err = L"GIF 파일을 쓰는 중 오류가 발생했습니다.";
        return ok;
    }

    ~GifEncoder() override {
        if (f_) fclose(f_);
    }

private:
    int Nearest(int r, int g, int b) {
        uint32_t key = ((uint32_t)(r >> 2) << 12) | ((uint32_t)(g >> 2) << 6) | (uint32_t)(b >> 2);
        uint16_t& c = cache_[key];
        if (c != 0xFFFF) return c;
        int best = 0, bestD = INT_MAX;
        for (int i = 0; i < palN_; ++i) {
            int d = ColorDist(r - pal_[i].r, g - pal_[i].g, b - pal_[i].b);
            if (d < bestD) {
                bestD = d;
                best = i;
            }
        }
        c = (uint16_t)best;
        return best;
    }

    void BuildPalette(const uint8_t* bgra, int x0, int y0, int x1, int y1, size_t nChanged, int maxColors) {
        wu_.Reset();
        size_t step = std::max<size_t>(1, nChanged / 50000);
        samples_.clear();
        size_t seen = 0;
        for (int y = y0; y <= y1; ++y) {
            const uint8_t* s = bgra + ((size_t)y * w_ + x0) * 4;
            const uint8_t* c = changed_.data() + (size_t)y * w_ + x0;
            for (int x = x0; x <= x1; ++x, s += 4, ++c) {
                if (!*c) continue;
                wu_.Add(s[2], s[1], s[0]);
                if (seen++ % step == 0) samples_.push_back({s[2], s[1], s[0]});
            }
        }
        palN_ = wu_.Build(maxColors, pal_);
        if (palN_ == 0) pal_[palN_++] = {0, 0, 0};

        // Two k-means passes over a sample fix the 5-bit histogram's coarse centroids.
        for (int it = 0; it < 2 && palN_ > 1; ++it) {
            int64_t sum[256][3] = {};
            int64_t cnt[256] = {};
            for (const Rgb& p : samples_) {
                int best = 0, bestD = INT_MAX;
                for (int i = 0; i < palN_; ++i) {
                    int d = ColorDist(p.r - pal_[i].r, p.g - pal_[i].g, p.b - pal_[i].b);
                    if (d < bestD) {
                        bestD = d;
                        best = i;
                    }
                }
                sum[best][0] += p.r;
                sum[best][1] += p.g;
                sum[best][2] += p.b;
                ++cnt[best];
            }
            for (int i = 0; i < palN_; ++i)
                if (cnt[i])
                    pal_[i] = {(uint8_t)((sum[i][0] + cnt[i] / 2) / cnt[i]), (uint8_t)((sum[i][1] + cnt[i] / 2) / cnt[i]),
                               (uint8_t)((sum[i][2] + cnt[i] / 2) / cnt[i])};
        }
    }

    void EncodeFrame(const uint8_t* bgra, int x0, int y0, int x1, int y1, size_t nChanged) {
        const bool useTransparency = !first_;
        BuildPalette(bgra, x0, y0, x1, y1, nChanged, useTransparency ? std::min(maxColors_, 255) : maxColors_);
        std::fill(cache_.begin(), cache_.end(), 0xFFFF);
        const int tIdx = useTransparency ? palN_ : -1;
        bool usedT = false;

        const int rw = x1 - x0 + 1, rh = y1 - y0 + 1;
        indices_.resize((size_t)rw * rh);
        errA_.assign((size_t)(rw + 2) * 3, 0.f);
        errB_.assign((size_t)(rw + 2) * 3, 0.f);
        float* errCur = errA_.data();
        float* errNext = errB_.data();
        const float limit = 48.f;

        for (int yy = 0; yy < rh; ++yy) {
            const int y = y0 + yy;
            const bool rev = (yy & 1) != 0;
            const int dir = rev ? -1 : 1;
            std::fill(errNext, errNext + (size_t)(rw + 2) * 3, 0.f);
            for (int k = 0; k < rw; ++k) {
                const int xx = rev ? rw - 1 - k : k;
                const size_t p = (size_t)y * w_ + x0 + xx;
                uint8_t& out = indices_[(size_t)yy * rw + xx];
                if (!changed_[p]) {
                    out = (uint8_t)tIdx;
                    usedT = true;
                    continue;
                }
                const uint8_t* s = bgra + p * 4;
                float* e = errCur + (size_t)(xx + 1) * 3;
                float fr = std::clamp(s[2] + e[0], 0.f, 255.f);
                float fg = std::clamp(s[1] + e[1], 0.f, 255.f);
                float fb = std::clamp(s[0] + e[2], 0.f, 255.f);
                int idx = Nearest((int)(fr + 0.5f), (int)(fg + 0.5f), (int)(fb + 0.5f));
                const Rgb c = pal_[idx];
                uint8_t* cp = comp_.data() + p * 3;
                if (useTransparency && cp[0] == c.r && cp[1] == c.g && cp[2] == c.b) {
                    out = (uint8_t)tIdx;  // already showing this color
                    usedT = true;
                } else {
                    out = (uint8_t)idx;
                    cp[0] = c.r;
                    cp[1] = c.g;
                    cp[2] = c.b;
                }
                uint8_t* rp = ref_.data() + p * 3;
                rp[0] = s[2];
                rp[1] = s[1];
                rp[2] = s[0];

                float er = std::clamp((fr - c.r) * dither_, -limit, limit);
                float eg = std::clamp((fg - c.g) * dither_, -limit, limit);
                float eb = std::clamp((fb - c.b) * dither_, -limit, limit);
                float* n0 = errCur + (size_t)(xx + 1 + dir) * 3;
                float* d0 = errNext + (size_t)(xx + 1 - dir) * 3;
                float* d1 = errNext + (size_t)(xx + 1) * 3;
                float* d2 = errNext + (size_t)(xx + 1 + dir) * 3;
                n0[0] += er * (7.f / 16);
                n0[1] += eg * (7.f / 16);
                n0[2] += eb * (7.f / 16);
                d0[0] += er * (3.f / 16);
                d0[1] += eg * (3.f / 16);
                d0[2] += eb * (3.f / 16);
                d1[0] += er * (5.f / 16);
                d1[1] += eg * (5.f / 16);
                d1[2] += eb * (5.f / 16);
                d2[0] += er * (1.f / 16);
                d2[1] += eg * (1.f / 16);
                d2[2] += eb * (1.f / 16);
            }
            std::swap(errCur, errNext);
        }

        const int colors = palN_ + (usedT ? 1 : 0);
        int bits = 1;
        while ((1 << bits) < colors) ++bits;
        const int tableSize = 1 << bits;

        pending_.clear();
        // Graphic Control Extension; the delay (bytes 4-5) is patched in FlushPending.
        uint8_t gce[8] = {0x21, 0xF9, 0x04, (uint8_t)((1 << 2) | (usedT ? 1 : 0)), 0, 0,
                          (uint8_t)(usedT ? tIdx : 0), 0};
        pending_.insert(pending_.end(), gce, gce + 8);
        uint8_t desc[10] = {0x2C,
                            (uint8_t)(x0 & 255), (uint8_t)(x0 >> 8),
                            (uint8_t)(y0 & 255), (uint8_t)(y0 >> 8),
                            (uint8_t)(rw & 255), (uint8_t)(rw >> 8),
                            (uint8_t)(rh & 255), (uint8_t)(rh >> 8),
                            (uint8_t)(0x80 | (bits - 1))};
        pending_.insert(pending_.end(), desc, desc + 10);
        for (int i = 0; i < tableSize; ++i) {
            Rgb c = i < palN_ ? pal_[i] : Rgb{0, 0, 0};
            pending_.push_back(c.r);
            pending_.push_back(c.g);
            pending_.push_back(c.b);
        }
        lzw_.Encode(indices_.data(), indices_.size(), std::max(2, bits), pending_);
        hasPending_ = true;
    }

    void FlushPending(double endMs) {
        if (!hasPending_) return;
        // Cumulative rounding keeps the total duration exact; GIF players treat <2/100 s as 10/100 s.
        int64_t endCs = (int64_t)std::llround(endMs / 10.0);
        int64_t delay = std::clamp<int64_t>(endCs - emittedCs_, 2, 65535);
        emittedCs_ += delay;
        pending_[4] = (uint8_t)(delay & 255);
        pending_[5] = (uint8_t)(delay >> 8);
        fwrite(pending_.data(), 1, pending_.size(), f_);
        hasPending_ = false;
    }

    FILE* f_ = nullptr;
    int w_ = 0, h_ = 0;
    int threshold_ = 0, maxColors_ = 256;
    float dither_ = 0.85f;
    bool first_ = true;
    double timeMs_ = 0, pendingStartMs_ = 0;
    int64_t emittedCs_ = 0;

    std::vector<uint8_t> ref_, comp_, changed_, indices_, pending_;
    bool hasPending_ = false;
    std::vector<uint16_t> cache_;
    std::vector<float> errA_, errB_;
    std::vector<Rgb> samples_;
    WuQuantizer wu_;
    LzwEncoder lzw_;
    Rgb pal_[256] = {};
    int palN_ = 0;
};

}  // namespace

std::unique_ptr<IEncoder> CreateGifEncoder() { return std::make_unique<GifEncoder>(); }
