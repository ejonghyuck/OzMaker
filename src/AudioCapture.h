#pragma once

// WASAPI loopback recorder producing interleaved 16-bit stereo PCM at a fixed rate on the QPC timeline, so it
// lines up with Windows.Graphics.Capture frames. Sample n of the output plays at origin + n / rate; stretches
// where nothing is playing (loopback delivers no packets then) are filled with silence.
class AudioCapture {
public:
    enum class Source {
        System,   // everything the default output device plays
        Process,  // only one process tree (Windows 10 2004+)
    };
    // Called on the capture thread with consecutive blocks of the output stream.
    using Sink = std::function<void(const int16_t* pcm, size_t frames)>;

    ~AudioCapture() { Stop(0); }

    // Per-process loopback needs Windows 10 build 19041 or later.
    static bool ProcessLoopbackSupported();
    // The process whose audio belongs to a window (UWP frames are resolved to the hosted app).
    static DWORD AudioProcessOf(HWND hwnd);

    bool Start(Source src, DWORD pid, int sampleRate, int64_t originQpc, Sink sink, std::wstring& err);
    // Stops and pads silence so the stream lasts until endQpc (0 = no padding).
    void Stop(int64_t endQpc);
    bool Running() const { return thread_.joinable(); }

private:
    void Run();
    void Deliver(const BYTE* data, UINT32 frames, bool silent, int64_t qpc);
    void Append(const int16_t* pcm, size_t frames);  // pcm == nullptr: silence
    void PadTo(int64_t frame);

    Source src_ = Source::System;
    DWORD pid_ = 0;
    int rate_ = 48000;
    int64_t origin_ = 0;
    Sink sink_;

    std::thread thread_;
    HANDLE stop_ = nullptr;
    HANDLE ready_ = nullptr;
    HRESULT startHr_ = S_OK;

    // Device format when the engine can't convert for us (only for system loopback).
    bool convert_ = false;
    bool srcFloat_ = false;
    int srcChannels_ = 2, srcBits_ = 16, srcRate_ = 48000;
    double resamplePos_ = 0;   // fractional source position of the next output frame
    int16_t lastL_ = 0, lastR_ = 0;
    std::vector<int16_t> scratch_;

    int64_t written_ = 0;  // output frames delivered so far
};
