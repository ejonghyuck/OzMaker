#pragma once

// Crop (cx, cy, cw, ch) out of a BGRA image and area-resample it to dw x dh (tightly packed BGRA, alpha 255; with
// alpha, premultiplied BGRA in and out).
class AreaResizer {
public:
    void Run(const uint8_t* src, int srcStride, int cx, int cy, int cw, int ch, uint8_t* dst, int dw, int dh,
             bool alpha = false);

private:
    struct Taps {
        std::vector<int> start, count;
        std::vector<float> w;
        std::vector<size_t> off;
    };
    static void Build(Taps& t, int srcLen, int dstLen);
    template <int N>
    void Resample(const uint8_t* src, int srcStride, int cx, int cy, int ch, uint8_t* dst, int dw, int dh);
    Taps h_, v_;
    int key_[4] = {};
    std::vector<float> tmp_;
};
