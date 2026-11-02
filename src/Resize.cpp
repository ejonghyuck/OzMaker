#include "pch.h"
#include "Resize.h"

void AreaResizer::Build(Taps& t, int srcLen, int dstLen) {
    t.start.assign(dstLen, 0);
    t.count.assign(dstLen, 0);
    t.off.assign(dstLen, 0);
    t.w.clear();
    double scale = (double)srcLen / dstLen;
    for (int d = 0; d < dstLen; ++d) {
        double a = d * scale, b = (d + 1) * scale;
        if (scale < 1.0) {  // upscale: fall back to a 1-pixel box around the sample center
            double c = (d + 0.5) * scale;
            a = c - 0.5;
            b = c + 0.5;
        }
        int s0 = std::max(0, (int)std::floor(a));
        int s1 = std::min(srcLen, (int)std::ceil(b));
        t.start[d] = s0;
        t.off[d] = t.w.size();
        double total = 0;
        for (int s = s0; s < s1; ++s) total += std::min<double>(b, s + 1) - std::max<double>(a, s);
        for (int s = s0; s < s1; ++s) {
            double cover = std::min<double>(b, s + 1) - std::max<double>(a, s);
            t.w.push_back((float)(cover / total));
        }
        t.count[d] = s1 - s0;
    }
}

void AreaResizer::Run(const uint8_t* src, int srcStride, int cx, int cy, int cw, int ch, uint8_t* dst, int dw,
                      int dh) {
    if (cw == dw && ch == dh) {
        for (int y = 0; y < dh; ++y) memcpy(dst + (size_t)y * dw * 4, src + (size_t)(cy + y) * srcStride + cx * 4,
                                            (size_t)dw * 4);
        return;
    }
    if (key_[0] != cw || key_[1] != ch || key_[2] != dw || key_[3] != dh) {
        Build(h_, cw, dw);
        Build(v_, ch, dh);
        key_[0] = cw;
        key_[1] = ch;
        key_[2] = dw;
        key_[3] = dh;
    }
    tmp_.resize((size_t)ch * dw * 3);
    for (int y = 0; y < ch; ++y) {
        const uint8_t* row = src + (size_t)(cy + y) * srcStride + (size_t)cx * 4;
        float* out = tmp_.data() + (size_t)y * dw * 3;
        for (int x = 0; x < dw; ++x) {
            const float* w = h_.w.data() + h_.off[x];
            const uint8_t* p = row + (size_t)h_.start[x] * 4;
            float b = 0, g = 0, r = 0;
            for (int k = 0; k < h_.count[x]; ++k, p += 4) {
                b += p[0] * w[k];
                g += p[1] * w[k];
                r += p[2] * w[k];
            }
            out[x * 3 + 0] = b;
            out[x * 3 + 1] = g;
            out[x * 3 + 2] = r;
        }
    }
    std::vector<float> acc((size_t)dw * 3);
    for (int y = 0; y < dh; ++y) {
        std::fill(acc.begin(), acc.end(), 0.f);
        const float* w = v_.w.data() + v_.off[y];
        for (int k = 0; k < v_.count[y]; ++k) {
            const float* row = tmp_.data() + (size_t)(v_.start[y] + k) * dw * 3;
            float wk = w[k];
            for (int i = 0; i < dw * 3; ++i) acc[i] += row[i] * wk;
        }
        uint8_t* out = dst + (size_t)y * dw * 4;
        for (int x = 0; x < dw; ++x) {
            out[x * 4 + 0] = (uint8_t)std::clamp(acc[x * 3 + 0] + 0.5f, 0.f, 255.f);
            out[x * 4 + 1] = (uint8_t)std::clamp(acc[x * 3 + 1] + 0.5f, 0.f, 255.f);
            out[x * 4 + 2] = (uint8_t)std::clamp(acc[x * 3 + 2] + 0.5f, 0.f, 255.f);
            out[x * 4 + 3] = 255;
        }
    }
}
