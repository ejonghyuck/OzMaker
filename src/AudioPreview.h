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

private:
    void Run(std::shared_ptr<const VideoClip> clip, int64_t from, int64_t to, double requested);

    std::thread thread_;
    std::atomic<bool> stop_{false};
};
