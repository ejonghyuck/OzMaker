#pragma once
#include "Mp4Box.h"

// 순간 리플레이 (#20). The sink writer writes a fragmented MP4 into memory, and the ring keeps its header and the last
// fragments, enough for the replay length. Saving writes the header and the samples from a keyframe on to a file and
// makes it a plain MP4: nothing is encoded again, so it takes a moment.
class ReplayRing {
public:
    // A new stream for the sink writer; what was written before is forgotten.
    ComPtr<IMFByteStream> NewStream();
    // At least this much (100 ns) is kept, from a keyframe on; older fragments are dropped whole.
    void SetKeep(int64_t span);
    void Clear();
    // 100 ns the fragments kept cover, from the first keyframe.
    int64_t Held() const;
    uint64_t Bytes() const;  // in memory
    // Media time (100 ns) up to which fragments came in: the encoder and the sink writer hold the newest frames back.
    int64_t End() const;

    // The fragments up to media time `until` and `span` before it (from the keyframe at or before) as a plain MP4.
    // Waits up to `wait` ms for the fragment that holds `until` when it hasn't come in yet.
    bool Save(const std::wstring& path, int64_t until, int64_t span, DWORD wait, double& seconds,
              std::wstring& err);

    // Tests: writes that went back over bytes written before (the sink writer patching a box), fragments kept.
    size_t Rewrites() const { return rewrites_; }
    size_t Fragments() const;
    std::wstring Describe() const;  // each fragment: start+length, k for a keyframe

private:
    friend class RingStream;
    void Write(uint64_t pos, const uint8_t* p, size_t n);
    void Parse();         // under mtx_: whole boxes out of tail_
    void ReadHeader();    // under mtx_: the video track from the moov
    void AddFragment(const mp4::Bytes& moof, uint64_t moofPos, std::shared_ptr<mp4::Bytes> mdat);

    // A track as the moov and mvex describe it; next: where its next sample starts (its time scale).
    struct TrackInfo {
        uint32_t id = 0, scale = 0, defDesc = 1, defDur = 0, defSize = 0, defFlags = 0;
        uint64_t next = 0;
    };
    struct Sample {
        int64_t start = 0;           // 100 ns
        uint32_t dur = 0, size = 0;  // dur in the track's time scale
        uint32_t flags = 0;          // sample flags (video)
        int32_t cto = 0;             // composition time offset (video)
        size_t at = 0;               // in the fragment's mdat
        bool Sync() const { return !(flags & 0x10000); }  // not sample_is_non_sync
    };
    // A moof and its mdat, read into samples. Saving makes each again with the samples that go into the file, so it
    // can start at any keyframe and end at any frame, whatever fragments Windows' sink made.
    struct Frag {
        std::shared_ptr<mp4::Bytes> mdat;
        std::vector<Sample> video, sound;
        uint32_t videoDesc = 1, soundDesc = 1;
        int64_t firstKey = INT64_MAX;  // the first video keyframe's start
        size_t bytes = 0;
    };
    mutable std::mutex mtx_;
    std::condition_variable grew_;
    uint64_t gen_ = 0;  // NewStream count: a stream of an older one writes nothing
    mp4::Bytes init_;   // ftyp and moov
    bool initDone_ = false;
    mp4::Bytes tail_;   // bytes after the last whole box
    uint64_t tailPos_ = 0, end_ = 0;
    mp4::Bytes moof_;  // waiting for its mdat
    bool hasMoof_ = false;
    uint64_t moofPos_ = 0;
    std::deque<Frag> frags_;
    uint64_t bytes_ = 0;
    int64_t keep_ = 0;
    int64_t videoEnd_ = 0, soundEnd_ = 0;  // 100 ns the fragments came in for
    // When the first frame shows (100 ns): an encoder that reorders frames (B-frames) shows each one later than it
    // decodes, and Windows' sink writes no edit list for that, so the sound goes with the video shown that much later.
    int64_t videoShift_ = 0;
    bool shiftKnown_ = false;
    TrackInfo video_, audio_;
    std::atomic<size_t> rewrites_{0};
};
