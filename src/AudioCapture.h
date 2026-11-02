#pragma once

struct IAudioClient;
struct IAudioCaptureClient;

// WASAPI recorder producing interleaved 16-bit stereo PCM at a fixed rate on the QPC timeline, so it lines up
// with Windows.Graphics.Capture frames. Sample n of the output plays at origin + n / rate; stretches where nothing
// is playing (loopback delivers no packets then) are filled with silence.
class AudioCapture {
public:
    enum class Source {
        System,   // loopback: everything the default output device plays
        Process,  // loopback: only one process tree (Windows 10 2004+)
        Mic,      // an input device (#21)
    };
    struct Device {
        std::wstring id, name;
        bool isDefault = false;  // Windows' default input
    };
    // Called on the capture thread with consecutive blocks of the output stream.
    using Sink = std::function<void(const int16_t* pcm, size_t frames)>;

    ~AudioCapture() { Stop(0); }

    // Per-process loopback needs Windows 10 build 19041 or later.
    static bool ProcessLoopbackSupported();
    // The process whose audio belongs to a window (UWP frames are resolved to the hosted app).
    static DWORD AudioProcessOf(HWND hwnd);
    // Active input devices (microphones, line in), by name.
    static std::vector<Device> InputDevices();

    // device: Mic's endpoint ID, empty = Windows' default input (followed when it changes).
    bool Start(Source src, DWORD pid, int sampleRate, int64_t originQpc, Sink sink, std::wstring& err,
               const std::wstring& device = {});
    // Stops and pads silence so the stream lasts until endQpc (0 = no padding; while paused, until the pause).
    void Stop(int64_t endQpc);
    // Nothing is delivered while paused; after Resume the stream carries on as if the pause never happened.
    void Pause(int64_t qpc);
    void Resume(int64_t qpc);
    bool Running() const { return thread_.joinable(); }

private:
    void Run();
    // Activates, initializes and starts the client (System: the default output device's loopback).
    HRESULT Open(ComPtr<IAudioClient>& client, ComPtr<IAudioCaptureClient>& cap, HANDLE event);
    void Deliver(const BYTE* data, UINT32 frames, bool silent, int64_t qpc);
    void Append(const int16_t* pcm, size_t frames);  // pcm == nullptr: silence
    void PadTo(int64_t frame);

    Source src_ = Source::System;
    DWORD pid_ = 0;
    std::wstring device_;
    int rate_ = 48000;
    int64_t origin_ = 0;
    Sink sink_;

    std::thread thread_;
    HANDLE stop_ = nullptr;
    HANDLE ready_ = nullptr;
    HRESULT startHr_ = S_OK;

    // Device format when the engine can't convert for us (not for process loopback).
    bool convert_ = false;
    bool srcFloat_ = false;
    int srcChannels_ = 2, srcBits_ = 16, srcRate_ = 48000;
    double resamplePos_ = 0;   // fractional source position of the next output frame
    int16_t lastL_ = 0, lastR_ = 0;
    std::vector<int16_t> scratch_;

    int64_t written_ = 0;  // output frames delivered so far
    std::mutex mtx_;        // origin_, pausedAt_ and the sink: Pause/Resume come from another thread
    int64_t pausedAt_ = 0;  // QPC of the pause in progress, 0 = none
};

// Adds up AudioCapture streams that share an origin (frame n of each plays at the same moment) into one stream:
// frame n goes out once every input has delivered it.
class AudioMix {
public:
    void Reset(int inputs, AudioCapture::Sink out);
    void SetGain(int input, float gain);
    AudioCapture::Sink Input(int input) {
        return [this, input](const int16_t* pcm, size_t frames) { Push(input, pcm, frames); };
    }
    // An input that isn't recorded (or failed to start): the others stop waiting for it.
    void Drop(int input);
    // After the captures stopped: what is left, shorter inputs padded with silence.
    void Flush();

private:
    void Push(int input, const int16_t* pcm, size_t frames);
    void Emit(size_t frames);  // under mtx_

    struct In {
        std::vector<int16_t> q;  // frames not sent out yet, interleaved stereo
        float gain = 1;
        bool live = true;
    };
    std::mutex mtx_;
    std::vector<In> in_;
    AudioCapture::Sink out_;
    std::vector<int16_t> mixed_;
};
