#pragma once

// WebM files (#40): VP9 video from BGRA frames at a constant rate and Opus sound from 16-bit stereo PCM, muxed by
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
    // The next frame (k shows from k / fps for 1 / fps): tightly packed BGRA, alpha ignored.
    bool AddVideo(const uint8_t* bgra, std::wstring& err);
    void PushAudio(const int16_t* pcm, size_t frames);  // interleaved stereo
    bool HasAudio() const;
    bool Finish(std::wstring& err);  // writes what is left and closes the file
    void Abort();                    // closes it as it is (the caller deletes it)

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
