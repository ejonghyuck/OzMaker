#pragma once

namespace mp4 {

// A video cut without encoding it again (#22): the samples of the range go into a new MP4 as they are, so it takes a
// moment and the picture stays as it was. Decoding has to start at a keyframe, so the cut starts at the one at or
// before the first frame. Works on a plain (not fragmented) MP4 with H.264 or HEVC video and AAC sound, or none;
// anything else is encoded again instead.
class CutSource {
public:
    // Reads the sample tables. Null when the file can't be cut this way.
    static std::shared_ptr<const CutSource> Open(const std::wstring& path);

    size_t Frames() const { return shown_.size(); }  // in the order they show, as VideoClip numbers them
    bool HasAudio() const { return !audio_.samples.empty(); }
    size_t KeyBefore(size_t frame) const;  // where a cut starting at frame starts
    // The size of the file a cut of frames [first, last] makes (sound: keep it, if there is any).
    uint64_t SizeOf(size_t first, size_t last, bool sound) const;

    struct Result {
        size_t frames = 0;  // shown
        double seconds = 0;
        bool audio = false;
    };
    bool Cut(size_t first, size_t last, bool sound, const std::wstring& out, const std::atomic<bool>& cancel,
             std::atomic<float>& progress, Result& res, std::wstring& err) const;

private:
    struct Sample {
        uint64_t offset, dts;
        uint32_t size, dur;
        int32_t cto;
        bool sync;
    };
    struct TrackData {
        std::vector<Sample> samples;  // in decode order
        uint32_t timescale = 0;
        size_t trak = 0;     // the trak among the moov's boxes
        double delay = 0;    // seconds of nothing before the track starts (an empty edit)
        int64_t start = 0;   // the media time shown first (its edit)
        int64_t End() const { return samples.empty() ? 0 : (int64_t)(samples.back().dts + samples.back().dur); }
    };
    struct Plan;
    bool MakePlan(size_t first, size_t last, bool sound, Plan& p) const;
    // The samples and edit of the trak that is the moov's box number `trak`; codec: its sample entry's type.
    static bool ReadTrack(const std::vector<uint8_t>& moov, size_t trak, uint32_t movieScale, TrackData& t,
                          uint32_t& codec);

    std::wstring path_;
    std::vector<uint8_t> moov_;  // the moov's contents
    TrackData video_, audio_;
    std::vector<uint32_t> shown_;  // decode index of each frame, in the order they show
    uint32_t movieScale_ = 0;
    bool avc_ = false;  // H.264 (the file type lists it)
};

}  // namespace mp4
