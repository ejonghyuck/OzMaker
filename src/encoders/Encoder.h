#pragma once

// Mp4 has no IEncoder: the exporter drives Mp4Writer directly.
enum class Format { Gif = 0, WebP, Avif, Mp4, Count };

struct EncoderParams {
    int width = 0, height = 0;
    int quality = 80;  // 1..100
    int plays = 0;     // times the animation plays, 0 = forever
    int lossy = 0;     // GIF: 0 off, 1 light, 2 strong (lossy LZW, as gifsicle --lossy)
    std::wstring path;
};

class IEncoder {
public:
    virtual ~IEncoder() = default;
    virtual bool Begin(const EncoderParams& p, std::wstring& err) = 0;
    // Tightly packed BGRA (alpha ignored); durationMs = how long this frame stays on screen.
    virtual bool Add(const uint8_t* bgra, double durationMs, std::wstring& err) = 0;
    virtual bool End(std::wstring& err) = 0;
};

std::unique_ptr<IEncoder> CreateEncoder(Format f);
bool FormatAvailable(Format f);
const wchar_t* FormatName(Format f);
const wchar_t* FormatExt(Format f);
// Highest frame rate the format plays back reliably (GIF delays are 1/100 s with a 2/100 s floor).
double FormatMaxFps(Format f);

std::unique_ptr<IEncoder> CreateGifEncoder();
std::unique_ptr<IEncoder> CreateWebpEncoder();
std::unique_ptr<IEncoder> CreateAvifEncoder();

bool WriteWholeFile(const std::wstring& path, const void* data, size_t size);
