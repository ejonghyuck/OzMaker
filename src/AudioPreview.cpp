#include "pch.h"
#include "AudioPreview.h"
#include "Util.h"

void AudioPreview::Start(std::shared_ptr<const VideoClip> clip, int64_t from, int64_t to) {
    Stop();
    if (!clip || !clip->HasAudio() || to <= from) return;
    stop_ = false;
    double requested = util::NowSeconds();
    thread_ = std::thread([this, clip = std::move(clip), from, to, requested] { Run(clip, from, to, requested); });
}

void AudioPreview::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void AudioPreview::Run(std::shared_ptr<const VideoClip> clip, int64_t from, int64_t to, double requested) {
    VideoClip::AudioReader reader;
    if (!reader.Open(*clip, from)) return;
    const int rate = reader.Rate();
    int64_t left = (to - from) * rate / 10'000'000;
    // The preview clock kept running while the decoder started: drop that much.
    int64_t late = (int64_t)((util::NowSeconds() - requested) * rate);
    if (late > 0 && late < left) {
        std::vector<int16_t> skip((size_t)late * 2);
        reader.Read(skip.data(), (size_t)late);
        left -= late;
    }

    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = rate * 4;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HWAVEOUT wo = nullptr;
    if (!ev || waveOutOpen(&wo, WAVE_MAPPER, &wf, (DWORD_PTR)ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        if (ev) CloseHandle(ev);
        return;
    }
    constexpr int kBufs = 4;
    const size_t chunk = (size_t)rate / 25;  // 40 ms
    std::vector<int16_t> data[kBufs];
    WAVEHDR hdr[kBufs]{};
    bool queued[kBufs]{};
    auto fill = [&](int i) {
        if (left <= 0 || stop_) return false;
        size_t n = (size_t)std::min<int64_t>((int64_t)chunk, left);
        data[i].resize(n * 2);
        reader.Read(data[i].data(), n);
        left -= (int64_t)n;
        hdr[i] = {};
        hdr[i].lpData = reinterpret_cast<LPSTR>(data[i].data());
        hdr[i].dwBufferLength = (DWORD)(n * 4);
        if (waveOutPrepareHeader(wo, &hdr[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) return false;
        if (waveOutWrite(wo, &hdr[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            waveOutUnprepareHeader(wo, &hdr[i], sizeof(WAVEHDR));
            return false;
        }
        return true;
    };
    for (int i = 0; i < kBufs; ++i) queued[i] = fill(i);
    while (!stop_) {
        WaitForSingleObject(ev, 20);
        bool any = false;
        for (int i = 0; i < kBufs; ++i) {
            if (queued[i] && (hdr[i].dwFlags & WHDR_DONE)) {
                waveOutUnprepareHeader(wo, &hdr[i], sizeof(WAVEHDR));
                queued[i] = fill(i);
            }
            any |= queued[i];
        }
        if (!any) break;
    }
    waveOutReset(wo);
    for (int i = 0; i < kBufs; ++i)
        if (queued[i]) waveOutUnprepareHeader(wo, &hdr[i], sizeof(WAVEHDR));
    waveOutClose(wo);
    CloseHandle(ev);
}
