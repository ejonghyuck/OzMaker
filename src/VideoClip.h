#pragma once
#include "Clip.h"

// A video file (MP4 and whatever else Media Foundation reads) as a clip. Opening it reads every frame's time from
// the container without decoding; readers decode on demand (on the GPU when the decoder supports it), continuing
// forward when they can and otherwise seeking to the keyframe before the wanted frame. Frames come out turned the
// way the file says to show them (portrait phone videos).
class VideoClip : public Clip {
public:
    // scanSamples: skip the sample tables and read every sample (fallback path, tests).
    static std::shared_ptr<VideoClip> Open(const std::wstring& path, std::wstring& err, bool scanSamples = false);

    int Width() const override { return width_; }
    int Height() const override { return height_; }
    size_t Count() const override { return ts_.size(); }
    int64_t Ts(size_t i) const override { return ts_[i]; }
    int64_t EndTs() const override { return endTs_; }
    std::unique_ptr<FrameReader> NewReader() const override;
    bool SlowSeek() const override { return true; }
    bool HasAudio() const override { return audioRate_ > 0; }
    std::wstring SourcePath() const override { return path_; }
    bool IsVideo() const override { return true; }
    size_t QuickIndexNear(int64_t t, int64_t tolerance) const override;

    // Last keyframe at or before frame i: decoding frame i has to start there.
    size_t KeyBefore(size_t i) const;
    // Frame whose time is closest to t (clip time).
    size_t Nearest(int64_t t) const;
    // Container time of frame 0; clip time = container time - Offset().
    int64_t Offset() const { return offset_; }
    int AudioRate() const { return audioRate_; }
    // Clockwise turn from the decoded picture to the frames handed out: 0, 90, 180 or 270.
    int Rotation() const { return rotation_; }
    int CodedWidth() const { return rotation_ % 180 ? height_ : width_; }
    int CodedHeight() const { return rotation_ % 180 ? width_ : height_; }
    // The MP4 writer's AAC encoder takes only these rates (WebM resamples 44.1 kHz for Opus); other sound can't be
    // carried into an export.
    bool AudioExportable() const { return audioRate_ == 44100 || audioRate_ == 48000; }
    // Device for hardware decoding shared by the readers; null when there is none.
    IMFDXGIDeviceManager* DeviceManager() const;

    // The sound track as interleaved 16-bit stereo, starting at a clip time. Read() always delivers what was asked:
    // gaps and the part after the track ends come out as silence.
    class AudioReader {
    public:
        bool Open(const VideoClip& clip, int64_t t0);
        int Rate() const { return rate_; }
        void Read(int16_t* out, size_t frames);

    private:
        bool Decode();
        ComPtr<IMFSourceReader> rd_;
        int rate_ = 0, channels_ = 0;
        int64_t start_ = 0;  // container time of the first frame handed out
        int64_t queued_ = 0;  // frames handed out or waiting in pending_
        std::vector<int16_t> pending_;
        size_t pendingPos_ = 0;  // in frames
        bool eos_ = false;
    };

private:
    std::wstring path_;
    int width_ = 0, height_ = 0;
    std::vector<int64_t> ts_;
    std::vector<size_t> keys_;  // ascending frame indices
    int64_t offset_ = 0, endTs_ = 0;
    int audioRate_ = 0;
    int rotation_ = 0;

    mutable std::mutex devMtx_;
    mutable bool devTried_ = false;
    mutable ComPtr<ID3D11Device> dev_;
    mutable ComPtr<IMFDXGIDeviceManager> devManager_;
};
