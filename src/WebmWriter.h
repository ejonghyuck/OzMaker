#pragma once

// WebM files (#40): VP9 video from BGRA frames on a 1 / fps clock and Opus sound from 16-bit stereo PCM, muxed by
// libwebm with cues so players can seek. Encoded on the CPU. A build without libvpx, Opus or libwebm has none
// (Available() is false).
class WebmWriter {
public:
    struct Config {
        int width = 0, height = 0;  // even
        int fps = 30;
        int quality = 70;       // 1-100: constant quality, unless bitrate is set
        uint32_t bitrate = 0;   // video bits per second (용량 맞추기); 0 = by quality
        bool audio = false;
        int sampleRate = 48000;  // of the PCM pushed: 44100 or 48000 (Opus takes 48 kHz, so 44.1 is resampled)
        int audioBitrate = 128000;
    };
    WebmWriter();
    ~WebmWriter();
    WebmWriter(const WebmWriter&) = delete;
    WebmWriter& operator=(const WebmWriter&) = delete;

    static bool Available();
    bool Open(const std::wstring& path, const Config& c, std::wstring& err);
    static constexpr int kKeySeconds = 5;  // the longest time between keyframes, so seeking stays quick
    // The next frame, shown for `ticks` of 1 / fps (more than one: variable frame rate, #55): tightly packed BGRA,
    // alpha ignored. key: make it a keyframe.
    bool AddVideo(const uint8_t* bgra, std::wstring& err, int ticks = 1, bool key = false);
    void PushAudio(const int16_t* pcm, size_t frames);  // interleaved stereo
    bool HasAudio() const;
    bool Finish(std::wstring& err);  // writes what is left and closes the file
    void Abort();                    // closes it as it is (the caller deletes it)

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
