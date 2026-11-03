#include "pch.h"
#include "encoders/Encoder.h"

#if __has_include(<avif/avif.h>)
#include <avif/avif.h>

namespace {

class AvifEncoder final : public IEncoder {
public:
    ~AvifEncoder() override {
        if (img_) avifImageDestroy(img_);
        if (enc_) avifEncoderDestroy(enc_);
    }

    bool Begin(const EncoderParams& p, std::wstring& err) override {
        w_ = p.width;
        h_ = p.height;
        alpha_ = p.alpha;
        path_ = p.path;
        enc_ = avifEncoderCreate();
        if (!enc_) return Fail(err, L"AVIF 인코더를 만들 수 없습니다.");
        enc_->maxThreads = (int)std::max(1u, std::thread::hardware_concurrency());
        enc_->speed = 8;
        enc_->timescale = 1000;
        enc_->repetitionCount = p.plays > 0 ? p.plays - 1 : AVIF_REPETITION_COUNT_INFINITE;  // repeats after the first
        enc_->quality = std::clamp(p.quality, 1, 100);
        enc_->qualityAlpha = AVIF_QUALITY_LOSSLESS;

        // From quality 90 the colour keeps full resolution (4:4:4): red text and thin coloured lines stay sharp.
        img_ = avifImageCreate((uint32_t)w_, (uint32_t)h_, 8,
                               enc_->quality >= 90 ? AVIF_PIXEL_FORMAT_YUV444 : AVIF_PIXEL_FORMAT_YUV420);
        if (!img_) return Fail(err, L"AVIF 이미지를 만들 수 없습니다.");
        img_->yuvRange = AVIF_RANGE_FULL;
        img_->colorPrimaries = AVIF_COLOR_PRIMARIES_BT709;
        img_->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
        img_->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT601;
        return true;
    }

    bool Add(const uint8_t* bgra, double durationMs, std::wstring& err) override {
        avifRGBImage rgb;
        avifRGBImageSetDefaults(&rgb, img_);
        rgb.format = AVIF_RGB_FORMAT_BGRA;
        rgb.ignoreAlpha = alpha_ ? AVIF_FALSE : AVIF_TRUE;  // with alpha, the image gets an alpha plane
        rgb.pixels = const_cast<uint8_t*>(bgra);
        rgb.rowBytes = (uint32_t)w_ * 4;
        avifResult r = avifImageRGBToYUV(img_, &rgb);
        if (r != AVIF_RESULT_OK) return Fail(err, L"AVIF 색 변환 실패");
        uint64_t dur = (uint64_t)std::max<long long>(1, std::llround(durationMs));
        r = avifEncoderAddImage(enc_, img_, dur, AVIF_ADD_IMAGE_FLAG_NONE);
        if (r != AVIF_RESULT_OK) return Fail(err, L"AVIF 인코딩 실패: " + Ascii(avifResultToString(r)));
        return true;
    }

    bool End(std::wstring& err) override {
        avifRWData out = AVIF_DATA_EMPTY;
        avifResult r = avifEncoderFinish(enc_, &out);
        if (r != AVIF_RESULT_OK) return Fail(err, L"AVIF 인코딩 실패: " + Ascii(avifResultToString(r)));
        bool ok = WriteWholeFile(path_, out.data, out.size);
        avifRWDataFree(&out);
        if (!ok) return Fail(err, L"파일을 쓸 수 없습니다: " + path_);
        return true;
    }

private:
    static bool Fail(std::wstring& err, std::wstring msg) {
        err = std::move(msg);
        return false;
    }
    static std::wstring Ascii(const char* s) {
        std::wstring out;
        if (s)
            while (*s) out.push_back((wchar_t)(unsigned char)*s++);
        return out;
    }

    avifEncoder* enc_ = nullptr;
    avifImage* img_ = nullptr;
    int w_ = 0, h_ = 0;
    bool alpha_ = false;
    std::wstring path_;
};

}  // namespace

std::unique_ptr<IEncoder> CreateAvifEncoder() { return std::make_unique<AvifEncoder>(); }

#else

std::unique_ptr<IEncoder> CreateAvifEncoder() { return nullptr; }

#endif
