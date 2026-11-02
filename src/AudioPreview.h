#pragma once
#include "VideoClip.h"

// Plays a video clip's sound while the editor previews it (waveOut, own thread). The thread skips whatever played
// while it was starting up, so the sound stays on the preview clock.
class AudioPreview {
public:
    ~AudioPreview() { Stop(); }
    // Clip times; plays [from, to) once.
    void Start(std::shared_ptr<const VideoClip> clip, int64_t from, int64_t to);
    void Stop();
    bool Active() const { return thread_.joinable(); }
    // 0..1 on the samples. A change reaches the speaker after what is already queued (up to 160 ms), ramped over 40 ms.
    void SetGain(float g) { gain_ = std::clamp(g, 0.f, 1.f); }
    // Scales stereo frames in place, the gain going from `from` to `to` across them.
    static void Scale(int16_t* s, size_t frames, float from, float to);

private:
    void Run(std::shared_ptr<const VideoClip> clip, int64_t from, int64_t to, double requested);

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<float> gain_{1.f};
};
