#include "pch.h"
#include "encoders/Encoder.h"

#if __has_include(<webp/encode.h>) && __has_include(<webp/mux.h>)
#include <webp/encode.h>
#include <webp/mux.h>

namespace {

class WebpEncoder final : public IEncoder {
public:
    ~WebpEncoder() override {
        if (enc_) WebPAnimEncoderDelete(enc_);
    }

    bool Begin(const EncoderParams& p, std::wstring& err) override {
        w_ = p.width;
        h_ = p.height;
        alpha_ = p.alpha;
        path_ = p.path;
        WebPAnimEncoderOptions ao;
        if (!WebPAnimEncoderOptionsInit(&ao)) return Fail(err, L"libwebp 버전이 맞지 않습니다.");
        ao.anim_params.loop_count = p.plays;  // total plays, 0 = forever
        ao.allow_mixed = 0;
        ao.minimize_size = 0;
        enc_ = WebPAnimEncoderNew(w_, h_, &ao);
        if (!enc_) return Fail(err, L"WebP 인코더를 만들 수 없습니다.");

        int q = std::clamp(p.quality, 1, 100);
        if (!WebPConfigInit(&cfg_)) return Fail(err, L"libwebp 버전이 맞지 않습니다.");
        if (q >= 100) {
            WebPConfigLosslessPreset(&cfg_, 5);
        } else {
            WebPConfigPreset(&cfg_, WEBP_PRESET_DEFAULT, (float)q);
            cfg_.method = 4;
            cfg_.use_sharp_yuv = 1;  // red text and thin coloured lines keep their edges (slower RGB to YUV)
        }
        cfg_.thread_level = 1;
        if (!WebPValidateConfig(&cfg_)) return Fail(err, L"WebP 설정이 올바르지 않습니다.");
        return true;
    }

    bool Add(const uint8_t* bgra, double durationMs, std::wstring& err) override {
        WebPPicture pic;
        if (!WebPPictureInit(&pic)) return Fail(err, L"WebP 프레임을 만들 수 없습니다.");
        pic.width = w_;
        pic.height = h_;
        pic.use_argb = 1;
        bool ok = (alpha_ ? WebPPictureImportBGRA(&pic, bgra, w_ * 4) : WebPPictureImportBGRX(&pic, bgra, w_ * 4)) &&
                  WebPAnimEncoderAdd(enc_, &pic, (int)std::llround(t_), &cfg_);
        WebPPictureFree(&pic);
        t_ += std::max(1.0, durationMs);
        if (!ok) return Fail(err, L"WebP 인코딩 실패: " + Utf8(WebPAnimEncoderGetError(enc_)));
        return true;
    }

    bool End(std::wstring& err) override {
        if (!WebPAnimEncoderAdd(enc_, nullptr, (int)std::llround(t_), nullptr))
            return Fail(err, L"WebP 인코딩 실패: " + Utf8(WebPAnimEncoderGetError(enc_)));
        WebPData data;
        WebPDataInit(&data);
        if (!WebPAnimEncoderAssemble(enc_, &data)) return Fail(err, L"WebP 파일을 만들 수 없습니다.");
        bool ok = WriteWholeFile(path_, data.bytes, data.size);
        WebPDataClear(&data);
        if (!ok) return Fail(err, L"파일을 쓸 수 없습니다: " + path_);
        return true;
    }

private:
    static bool Fail(std::wstring& err, std::wstring msg) {
        err = std::move(msg);
        return false;
    }
    static std::wstring Utf8(const char* s) {
        std::wstring out;
        if (s)
            while (*s) out.push_back((wchar_t)(unsigned char)*s++);
        return out;
    }

    WebPAnimEncoder* enc_ = nullptr;
    WebPConfig cfg_{};
    int w_ = 0, h_ = 0;
    bool alpha_ = false;
    double t_ = 0;
    std::wstring path_;
};

}  // namespace

std::unique_ptr<IEncoder> CreateWebpEncoder() { return std::make_unique<WebpEncoder>(); }

bool WriteWebpImage(const std::wstring& path, const uint8_t* bgra, int w, int h, bool alpha, int quality) {
    WebPConfig cfg;
    WebPPicture pic;
    if (!WebPConfigPreset(&cfg, WEBP_PRESET_DEFAULT, (float)quality) || !WebPPictureInit(&pic)) return false;
    cfg.method = 4;
    cfg.use_sharp_yuv = 1;  // as above
    pic.width = w;
    pic.height = h;
    pic.use_argb = 1;
    WebPMemoryWriter out;
    WebPMemoryWriterInit(&out);
    pic.writer = WebPMemoryWrite;
    pic.custom_ptr = &out;
    bool ok = (alpha ? WebPPictureImportBGRA(&pic, bgra, w * 4) : WebPPictureImportBGRX(&pic, bgra, w * 4)) &&
              WebPEncode(&cfg, &pic) && WriteWholeFile(path, out.mem, out.size);
    WebPPictureFree(&pic);
    WebPMemoryWriterClear(&out);
    return ok;
}

#else

std::unique_ptr<IEncoder> CreateWebpEncoder() { return nullptr; }

bool WriteWebpImage(const std::wstring&, const uint8_t*, int, int, bool, int) { return false; }

#endif
