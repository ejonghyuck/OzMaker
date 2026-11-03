#pragma once
#include "Clip.h"
#include "encoders/Encoder.h"

class FrameStore;

// A GIF, WebP or AVIF file opened in the editor (#41). Its frames are composited and decoded into a temp store when it
// opens: premultiplied BGRA with the transparent parts kept, each frame with its own time. The file stays as it is.
class AnimClip : public Clip {
public:
    // Decodes the whole file; cancel and progress (0-1) are for another thread. Null on failure, with err.
    static std::shared_ptr<AnimClip> Open(const std::wstring& path, const std::atomic<bool>& cancel,
                                          std::atomic<float>& progress, std::wstring& err);
    // The format by the file's first bytes; Format::Count if it is none of the three.
    static Format Sniff(const std::wstring& path);

    int Width() const override;
    int Height() const override;
    size_t Count() const override;
    int64_t Ts(size_t i) const override;
    int64_t EndTs() const override;
    std::unique_ptr<FrameReader> NewReader() const override;
    std::wstring SourcePath() const override { return path_; }
    bool HasAlpha() const override { return alpha_; }
    Format FileFormat() const { return format_; }

private:
    std::shared_ptr<FrameStore> store_;
    std::wstring path_;
    Format format_ = Format::Gif;
    bool alpha_ = false;
};
