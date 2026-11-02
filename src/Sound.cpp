#include "pch.h"
#include "Sound.h"

namespace sound {
namespace {

constexpr int kRate = 44100;

struct Tone {
    float freq, ms;
};

std::vector<uint8_t> MakeWav(std::initializer_list<Tone> tones, float volume) {
    std::vector<int16_t> pcm;
    for (const Tone& t : tones) {
        int n = (int)(kRate * t.ms / 1000.f);
        for (int i = 0; i < n; ++i) {
            float tt = (float)i / kRate;
            float env = std::min(1.f, i / (kRate * 0.004f)) * std::exp(-4.5f * i / (float)n);
            float s = std::sin(6.2831853f * t.freq * tt) * 0.8f + std::sin(6.2831853f * t.freq * 2 * tt) * 0.2f;
            pcm.push_back((int16_t)(s * env * volume * 32767));
        }
    }
    uint32_t dataBytes = (uint32_t)(pcm.size() * 2);
    std::vector<uint8_t> wav(44 + dataBytes);
    auto put32 = [&](size_t off, uint32_t v) { memcpy(&wav[off], &v, 4); };
    auto put16 = [&](size_t off, uint16_t v) { memcpy(&wav[off], &v, 2); };
    memcpy(&wav[0], "RIFF", 4);
    put32(4, 36 + dataBytes);
    memcpy(&wav[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, 1);
    put32(24, kRate);
    put32(28, kRate * 2);
    put16(32, 2);
    put16(34, 16);
    memcpy(&wav[36], "data", 4);
    put32(40, dataBytes);
    memcpy(&wav[44], pcm.data(), dataBytes);
    return wav;
}

}  // namespace

void Play(Cue cue) {
    static const std::vector<uint8_t> start = MakeWav({{880, 70}, {1318.5f, 110}}, 0.35f);
    static const std::vector<uint8_t> stop = MakeWav({{1318.5f, 70}, {880, 120}}, 0.35f);
    static const std::vector<uint8_t> saved = MakeWav({{1046.5f, 60}, {1568, 140}}, 0.3f);
    static const std::vector<uint8_t> tick = MakeWav({{1174.7f, 45}}, 0.25f);
    const std::vector<uint8_t>* w = cue == Cue::Start ? &start : cue == Cue::Stop ? &stop : cue == Cue::Saved ? &saved : &tick;
    PlaySoundW(reinterpret_cast<LPCWSTR>(w->data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

}  // namespace sound
