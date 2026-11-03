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

// N = 3: colour only, alpha comes out 255. N = 4: premultiplied BGRA, alpha averaged with the rest.
template <int N>
void AreaResizer::Resample(const uint8_t* src, int srcStride, int cx, int cy, int ch, uint8_t* dst, int dw, int dh) {
    tmp_.resize((size_t)ch * dw * N);
    for (int y = 0; y < ch; ++y) {
        const uint8_t* row = src + (size_t)(cy + y) * srcStride + (size_t)cx * 4;
        float* out = tmp_.data() + (size_t)y * dw * N;
        for (int x = 0; x < dw; ++x) {
            const float* w = h_.w.data() + h_.off[x];
            const uint8_t* p = row + (size_t)h_.start[x] * 4;
            float sum[N] = {};
            for (int k = 0; k < h_.count[x]; ++k, p += 4)
                for (int c = 0; c < N; ++c) sum[c] += p[c] * w[k];
            for (int c = 0; c < N; ++c) out[x * N + c] = sum[c];
        }
    }
    std::vector<float> acc((size_t)dw * N);
    for (int y = 0; y < dh; ++y) {
        std::fill(acc.begin(), acc.end(), 0.f);
        const float* w = v_.w.data() + v_.off[y];
        for (int k = 0; k < v_.count[y]; ++k) {
            const float* row = tmp_.data() + (size_t)(v_.start[y] + k) * dw * N;
            float wk = w[k];
            for (int i = 0; i < dw * N; ++i) acc[i] += row[i] * wk;
        }
        uint8_t* out = dst + (size_t)y * dw * 4;
        for (int x = 0; x < dw; ++x) {
            for (int c = 0; c < N; ++c) out[x * 4 + c] = (uint8_t)std::clamp(acc[x * N + c] + 0.5f, 0.f, 255.f);
            if (N == 3) out[x * 4 + 3] = 255;
        }
    }
}

void AreaResizer::Run(const uint8_t* src, int srcStride, int cx, int cy, int cw, int ch, uint8_t* dst, int dw,
                      int dh, bool alpha) {
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
    if (alpha)
        Resample<4>(src, srcStride, cx, cy, ch, dst, dw, dh);
    else
        Resample<3>(src, srcStride, cx, cy, ch, dst, dw, dh);
}
