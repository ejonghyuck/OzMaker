#include "pch.h"
#include "WebmWriter.h"

#if __has_include(<vpx/vp8cx.h>) && __has_include(<opus/opus.h>) && __has_include(<mkvmuxer/mkvmuxer.h>) && \
    __has_include(<libyuv.h>)
#include <libyuv.h>
#include <numeric>
#include <mkvmuxer/mkvmuxer.h>
#include <mkvmuxer/mkvwriter.h>
#include <opus/opus.h>
#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>

namespace {

// Any rate to 48 kHz for Opus: a windowed sinc (Blackman, 16 taps each side), one set of taps per phase of the ratio
// (44.1 to 48 kHz has 160), so each output sample costs 32 multiply-adds a channel.
class Resampler {
public:
    void Init(int inRate) {
        const int g = std::gcd(inRate, 48000);
        up_ = 48000 / g;
        down_ = inRate / g;
        taps_.assign((size_t)up_ * kTaps, 0.f);
        const double cutoff = 0.95 * std::min(1.0, 48000.0 / inRate);  // of the input's Nyquist
        for (int p = 0; p < up_; ++p) {
            const double frac = (double)p / up_;
            float sum = 0;
            for (int k = 0; k < kTaps; ++k) {
                const double x = k - (kTaps / 2 - 1) - frac;  // input sample k of the window, from the output point
                const double w = std::fabs(x) >= kTaps / 2 ? 0
                                 : 0.42 + 0.5 * std::cos(3.14159265358979 * x / (kTaps / 2)) +
                                       0.08 * std::cos(2 * 3.14159265358979 * x / (kTaps / 2));
                const double s = x == 0 ? 1 : std::sin(3.14159265358979 * cutoff * x) / (3.14159265358979 * cutoff * x);
                taps_[(size_t)p * kTaps + k] = (float)(s * w);
                sum += taps_[(size_t)p * kTaps + k];
            }
            for (int k = 0; k < kTaps; ++k) taps_[(size_t)p * kTaps + k] /= sum;
        }
        in_.assign((size_t)(kTaps / 2 - 1) * 2, 0.f);  // silence before the first sample
        base_ = 0;
        phase_ = 0;
    }
    // Appends to out (interleaved stereo at 48 kHz) what the input so far gives.
    void Push(const int16_t* pcm, size_t frames, std::vector<int16_t>& out) {
        for (size_t i = 0; i < frames * 2; ++i) in_.push_back(pcm[i]);
        const size_t have = in_.size() / 2;
        while (base_ + kTaps <= have) {
            const float* t = &taps_[(size_t)phase_ * kTaps];
            const float* s = &in_[base_ * 2];
            float l = 0, r = 0;
            for (int k = 0; k < kTaps; ++k) {
                l += s[k * 2] * t[k];
                r += s[k * 2 + 1] * t[k];
            }
            out.push_back((int16_t)std::clamp(std::lround(l), -32768l, 32767l));
            out.push_back((int16_t)std::clamp(std::lround(r), -32768l, 32767l));
            phase_ += down_;
            base_ += phase_ / up_;
            phase_ %= up_;
        }
        if (base_ > 4096) {  // drop what no output needs any more
            in_.erase(in_.begin(), in_.begin() + (ptrdiff_t)base_ * 2);
            base_ = 0;
        }
    }

private:
    static constexpr int kTaps = 32;
    int up_ = 1, down_ = 1;
    std::vector<float> taps_, in_;
    size_t base_ = 0;  // input frame the window starts at
    int phase_ = 0;    // where the output point is between two input frames, in 1/up_
};

}  // namespace

struct WebmWriter::Impl {
    Config c;
    FILE* f = nullptr;
    std::unique_ptr<mkvmuxer::MkvWriter> writer;
    mkvmuxer::Segment segment;
    uint64_t vtrack = 0, atrack = 0;
    vpx_codec_ctx_t vpx{};
    bool vpxOpen = false;
    vpx_image_t img{};
    std::vector<uint8_t> i420;
    int64_t ticks = 0;  // where the video frames given end, in 1 / fps
    OpusEncoder* opus = nullptr;
    Resampler resampler;
    std::vector<int16_t> pcm;  // 48 kHz stereo waiting for a whole Opus frame
    int64_t audioSamples = 0;  // encoded so far, per channel at 48 kHz
    int lookahead = 0;         // samples the encoder holds back (the decoder skips as many at the start)
    struct Packet {
        uint64_t ns;
        std::vector<uint8_t> data;
        int64_t discardNs = 0;  // the end of the last packet, which is only padding
    };
    std::deque<Packet> audioQ;  // encoded sound waiting for the video to get that far (the muxer writes in time order)
    bool failed = false;

    static constexpr int kOpusFrame = 960;  // 20 ms

    ~Impl() { Close(); }
    void Close() {
        if (vpxOpen) vpx_codec_destroy(&vpx);
        vpxOpen = false;
        if (opus) opus_encoder_destroy(opus);
        opus = nullptr;
        writer.reset();
        if (f) fclose(f);
        f = nullptr;
    }
    void WriteAudioUpTo(uint64_t ns) {
        while (!audioQ.empty() && audioQ.front().ns <= ns) {
            const Packet& p = audioQ.front();
            failed |= !(p.discardNs ? segment.AddFrameWithDiscardPadding(p.data.data(), p.data.size(), p.discardNs,
                                                                         atrack, p.ns, true)
                                    : segment.AddFrame(p.data.data(), p.data.size(), atrack, p.ns, true));
            audioQ.pop_front();
        }
    }
    // Writes what the encoder has ready; false if it had nothing.
    bool Drain() {
        bool any = false;
        vpx_codec_iter_t it = nullptr;
        while (const vpx_codec_cx_pkt_t* pkt = vpx_codec_get_cx_data(&vpx, &it)) {
            if (pkt->kind != VPX_CODEC_CX_FRAME_PKT) continue;
            any = true;
            const uint64_t ns = (uint64_t)pkt->data.frame.pts * 1'000'000'000ull / (uint64_t)c.fps;
            WriteAudioUpTo(ns);
            failed |= !segment.AddFrame(static_cast<const uint8_t*>(pkt->data.frame.buf), pkt->data.frame.sz, vtrack,
                                        ns, (pkt->data.frame.flags & VPX_FRAME_IS_KEY) != 0);
        }
        return any;
    }
    void EncodeAudio(bool flush) {
        if (flush && pcm.size() % (kOpusFrame * 2)) pcm.resize((pcm.size() / (kOpusFrame * 2) + 1) * kOpusFrame * 2, 0);
        size_t at = 0;
        uint8_t out[4000];
        for (; at + kOpusFrame * 2 <= pcm.size(); at += kOpusFrame * 2) {
            const int n = opus_encode(opus, &pcm[at], kOpusFrame, out, sizeof(out));
            if (n < 0) {
                failed = true;
                break;
            }
            audioQ.push_back({(uint64_t)audioSamples * 1'000'000'000ull / 48000, std::vector<uint8_t>(out, out + n)});
            audioSamples += kOpusFrame;
        }
        pcm.erase(pcm.begin(), pcm.begin() + (ptrdiff_t)at);
    }
};

WebmWriter::WebmWriter() = default;
WebmWriter::~WebmWriter() = default;
bool WebmWriter::Available() { return true; }
bool WebmWriter::HasAudio() const { return impl_ && impl_->opus; }

bool WebmWriter::Open(const std::wstring& path, const Config& cfg, std::wstring& err) {
    impl_ = std::make_unique<Impl>();
    Impl& m = *impl_;
    m.c = cfg;
    if (_wfopen_s(&m.f, path.c_str(), L"wb") || !m.f) {
        err = tr(L"파일을 열 수 없습니다: ") + path;
        return false;
    }
    setvbuf(m.f, nullptr, _IOFBF, 1 << 20);
    m.writer = std::make_unique<mkvmuxer::MkvWriter>(m.f);
    if (!m.segment.Init(m.writer.get())) {
        err = tr(L"WebM 파일을 만들 수 없습니다.");
        return false;
    }
    m.segment.set_mode(mkvmuxer::Segment::kFile);
    m.segment.OutputCues(true);
    m.segment.GetSegmentInfo()->set_writing_app("OzMaker");
    m.vtrack = m.segment.AddVideoTrack(cfg.width, cfg.height, 0);
    auto* vt = static_cast<mkvmuxer::VideoTrack*>(m.segment.GetTrackByNumber(m.vtrack));
    if (!vt) {
        err = tr(L"WebM 파일을 만들 수 없습니다.");
        return false;
    }
    vt->set_codec_id(mkvmuxer::Tracks::kVp9CodecId);
    vt->set_frame_rate(cfg.fps);
    mkvmuxer::Colour colour;  // as libyuv converts: BT.601, studio range
    colour.set_matrix_coefficients(mkvmuxer::Colour::kSmpte170MMc);
    colour.set_range(mkvmuxer::Colour::kBroadcastRange);
    vt->SetColour(colour);
    m.segment.CuesTrack(m.vtrack);

    vpx_codec_iface_t* iface = vpx_codec_vp9_cx();
    vpx_codec_enc_cfg_t vc;
    if (vpx_codec_enc_config_default(iface, &vc, 0) != VPX_CODEC_OK) {
        err = tr(L"VP9 인코더를 만들 수 없습니다.");
        return false;
    }
    const int threads = (int)std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    vc.g_w = cfg.width;
    vc.g_h = cfg.height;
    vc.g_timebase = {1, cfg.fps};
    vc.g_threads = threads;
    vc.g_lag_in_frames = 16;
    vc.kf_mode = VPX_KF_AUTO;
    vc.kf_max_dist = cfg.fps * kKeySeconds;  // in frames: variable frame rate also asks for them by time
    if (cfg.bitrate) {
        vc.rc_end_usage = VPX_VBR;
        vc.rc_target_bitrate = std::max(50u, cfg.bitrate / 1000);
    } else {
        vc.rc_end_usage = VPX_Q;
    }
    if (vpx_codec_enc_init(&m.vpx, iface, &vc, 0) != VPX_CODEC_OK) {
        err = tr(L"VP9 인코더를 만들 수 없습니다.");
        return false;
    }
    m.vpxOpen = true;
    int tiles = 0;  // log2 of the tile columns, each at least 256 wide
    while (tiles < 4 && (cfg.width >> (tiles + 1)) >= 256 && (1 << (tiles + 1)) <= threads) ++tiles;
    vpx_codec_control(&m.vpx, VP8E_SET_CPUUSED, 5);
    vpx_codec_control(&m.vpx, VP9E_SET_ROW_MT, 1);
    vpx_codec_control(&m.vpx, VP9E_SET_TILE_COLUMNS, tiles);
    vpx_codec_control(&m.vpx, VP9E_SET_TUNE_CONTENT, VP9E_CONTENT_SCREEN);
    vpx_codec_control(&m.vpx, VP9E_SET_COLOR_SPACE, VPX_CS_BT_601);
    vpx_codec_control(&m.vpx, VP9E_SET_COLOR_RANGE, VPX_CR_STUDIO_RANGE);
    vpx_codec_control(&m.vpx, VP8E_SET_ENABLEAUTOALTREF, 1);
    // 품질 100 is close to lossless (4), 1 the smallest (63).
    if (!cfg.bitrate)
        vpx_codec_control(&m.vpx, VP8E_SET_CQ_LEVEL, std::clamp((int)std::lround(4 + (100 - cfg.quality) * 0.59), 0, 63));
    const size_t cw = (size_t)(cfg.width + 1) / 2, ch = (size_t)(cfg.height + 1) / 2;
    m.i420.resize((size_t)cfg.width * cfg.height + cw * ch * 2);
    vpx_img_wrap(&m.img, VPX_IMG_FMT_I420, cfg.width, cfg.height, 1, m.i420.data());

    if (cfg.audio) {
        int e = 0;
        m.opus = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &e);
        if (!m.opus || e != OPUS_OK) {
            err = tr(L"Opus 인코더를 만들 수 없습니다.");
            return false;
        }
        opus_encoder_ctl(m.opus, OPUS_SET_BITRATE(cfg.audioBitrate));
        opus_int32 lookahead = 0;
        opus_encoder_ctl(m.opus, OPUS_GET_LOOKAHEAD(&lookahead));
        m.lookahead = lookahead;
        m.atrack = m.segment.AddAudioTrack(48000, 2, 0);
        auto* at = static_cast<mkvmuxer::AudioTrack*>(m.segment.GetTrackByNumber(m.atrack));
        if (!at) {
            err = tr(L"WebM 파일을 만들 수 없습니다.");
            return false;
        }
        // OpusHead (RFC 7845): version 1, stereo, the samples to skip, the input rate, no gain, one stream.
        const uint32_t rate = (uint32_t)cfg.sampleRate;
        const uint8_t head[19] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2,
                                  (uint8_t)(lookahead & 255), (uint8_t)(lookahead >> 8),
                                  (uint8_t)rate, (uint8_t)(rate >> 8), (uint8_t)(rate >> 16), (uint8_t)(rate >> 24),
                                  0, 0, 0};
        at->set_codec_id(mkvmuxer::Tracks::kOpusCodecId);
        at->SetCodecPrivate(head, sizeof(head));
        at->set_codec_delay((uint64_t)lookahead * 1'000'000'000ull / 48000);
        at->set_seek_pre_roll(80'000'000ull);  // 80 ms, as the Opus mapping asks
        if (cfg.sampleRate != 48000) m.resampler.Init(cfg.sampleRate);
    }
    return true;
}

bool WebmWriter::AddVideo(const uint8_t* bgra, std::wstring& err, int ticks, bool key) {
    Impl& m = *impl_;
    const int w = m.c.width, h = m.c.height;
    libyuv::ARGBToI420(bgra, w * 4, m.img.planes[VPX_PLANE_Y], m.img.stride[VPX_PLANE_Y], m.img.planes[VPX_PLANE_U],
                       m.img.stride[VPX_PLANE_U], m.img.planes[VPX_PLANE_V], m.img.stride[VPX_PLANE_V], w, h);
    ticks = std::max(1, ticks);
    if (vpx_codec_encode(&m.vpx, &m.img, m.ticks, ticks, key ? VPX_EFLAG_FORCE_KF : 0, VPX_DL_GOOD_QUALITY) !=
        VPX_CODEC_OK) {
        err = tr(L"VP9 인코딩에 실패했습니다.");
        return false;
    }
    m.ticks += ticks;
    m.Drain();
    if (m.failed) err = tr(L"WebM 파일을 쓸 수 없습니다.");
    return !m.failed;
}

void WebmWriter::PushAudio(const int16_t* pcm, size_t frames) {
    Impl& m = *impl_;
    if (!m.opus || !frames) return;
    if (m.c.sampleRate == 48000)
        m.pcm.insert(m.pcm.end(), pcm, pcm + frames * 2);
    else
        m.resampler.Push(pcm, frames, m.pcm);
    m.EncodeAudio(false);
}

bool WebmWriter::Finish(std::wstring& err) {
    Impl& m = *impl_;
    if (m.opus && m.c.sampleRate != 48000) {
        const int16_t silence[32] = {};
        m.resampler.Push(silence, 16, m.pcm);  // the last samples, through the rest of the window
    }
    if (m.opus) {
        // Silence as long as the encoder holds back, so every sample put in comes out of the decoder; what the last
        // packet has past that is marked to be dropped.
        const int64_t content = m.audioSamples + (int64_t)m.pcm.size() / 2;
        m.pcm.resize(m.pcm.size() + (size_t)m.lookahead * 2, 0);
        m.EncodeAudio(true);
        const int64_t extra = m.audioSamples - m.lookahead - content;
        if (extra > 0 && !m.audioQ.empty()) m.audioQ.back().discardNs = extra * 1'000'000'000ll / 48000;
    }
    // No picture, until the encoder has given up every frame it held back.
    while (vpx_codec_encode(&m.vpx, nullptr, m.ticks, 1, 0, VPX_DL_GOOD_QUALITY) == VPX_CODEC_OK && m.Drain()) {
    }
    m.WriteAudioUpTo(UINT64_MAX);
    // To the end of the last frame, not its start, which is what the muxer would take.
    m.segment.set_duration(m.ticks * 1e9 / m.c.fps / (double)m.segment.GetSegmentInfo()->timecode_scale());
    bool ok = !m.failed && m.segment.Finalize();
    m.writer.reset();
    ok = m.f && fclose(m.f) == 0 && ok;
    m.f = nullptr;
    if (!ok) err = tr(L"WebM 파일을 쓸 수 없습니다.");
    impl_.reset();
    return ok;
}

void WebmWriter::Abort() { impl_.reset(); }

#else

struct WebmWriter::Impl {};
WebmWriter::WebmWriter() = default;
WebmWriter::~WebmWriter() = default;
bool WebmWriter::Available() { return false; }
bool WebmWriter::HasAudio() const { return false; }
bool WebmWriter::Open(const std::wstring&, const Config&, std::wstring& err) {
    err = tr(L"WebM 인코더가 이 빌드에 포함되어 있지 않습니다.");
    return false;
}
bool WebmWriter::AddVideo(const uint8_t*, std::wstring&, int, bool) { return false; }
void WebmWriter::PushAudio(const int16_t*, size_t) {}
bool WebmWriter::Finish(std::wstring&) { return false; }
void WebmWriter::Abort() {}

#endif
