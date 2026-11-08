#pragma once
#include <strmif.h>

class ReplayRing;

// MFStartup once per process.
void EnsureMediaFoundation();

struct VideoConfig {
    int width = 0, height = 0;   // even
    int fps = 60;
    bool hevc = false;
    bool hardware = true;
    UINT32 bitrate = 0;          // bits per second
    bool cbr = false;
    int gopFrames = 120;
    bool vfr = false;            // a frame only when the picture changes (#55): HoldVideo, keyframes by time
    int qualityVsSpeed = 50;     // 0 = fastest, 100 = best
    bool timelapse = false;  // said in the file (mp4::Unfragment), for 타임랩스 파일 잇기 (#72)
    int quality = 0;             // 1 to 100: by quality where the encoder has that (#71); else the bitrate holds
    bool fragmented = false;     // fragmented MP4 while writing, so a crash leaves a file (#24); Finish makes it plain
};

struct AudioConfig {
    bool enabled = false;
    int sampleRate = 48000;      // 44100 or 48000
    int bitrate = 160000;        // AAC: 96/128/160/192 kbps
};

// H.264/HEVC + AAC MP4 through the Media Foundation sink writer. Frames are BGRA textures on the capture device,
// converted to NV12 by the GPU video processor; with a hardware encoder they never leave video memory.
class Mp4Writer {
public:
    Mp4Writer();
    ~Mp4Writer();

    // Encoder that would be used, e.g. "NVIDIA H.264 Encoder MFT"; empty if the codec isn't available.
    static std::wstring FindEncoder(bool hevc, bool hardware);
    // Whether Windows can play HEVC here (the HEVC Video Extensions, or a decoder that came with the graphics driver).
    static bool HevcDecoderAvailable();
    // "NVIDIA H.264 Encoder MFT" -> "NVIDIA".
    static std::wstring Vendor(const std::wstring& encoderName);
    // Reasonable bitrate for the size and frame rate when the user leaves it on automatic.
    static UINT32 AutoBitrate(int w, int h, int fps, bool hevc);

    // dev must have multithread protection on; it's shared with the encoder. ring: write a fragmented MP4 into it
    // instead of a file (순간 리플레이, #20); path is not used then, and Finish only sends the last fragments there.
    bool Open(const std::wstring& path, const VideoConfig& v, const AudioConfig& a, ID3D11Device* dev,
              std::wstring& err, ReplayRing* ring = nullptr);
    bool IsOpen() const { return writer_ != nullptr; }

    // Converts a BGRA frame into a pooled NV12 sample. Don't hold the device lock when calling.
    ComPtr<IMFSample> Convert(ID3D11Texture2D* bgra);
    // Writes queued audio, then the frame. Call from one thread. key: ask the encoder to make this frame a keyframe.
    bool WriteVideo(IMFSample* sample, int64_t time, int64_t duration, bool key = false);
    // VFR (#55): the frame shows from `time` until the next one, so it goes to the encoder when the next one comes,
    // or at EndVideo. It is a keyframe when a keyframe interval (gopFrames at fps) has gone by since the last one.
    bool HoldVideo(ComPtr<IMFSample> sample, int64_t time);
    // Whether a frame at `time` would be a keyframe: VFR writes one then even when nothing changed.
    bool KeyDue(int64_t time) const;
    // Writes the frame HoldVideo holds, lasting until `end`.
    bool EndVideo(int64_t end);
    // Thread-safe; queued until the next WriteVideo/Finish. Interleaved 16-bit stereo.
    void PushAudio(const int16_t* pcm, size_t frames);

    bool Finish(std::wstring& err);
    void RefuseFinish() { refuseFinish_ = true; }  // tests: Finish finds the encoder gone
    void Abort();
    // After a Finish that worked: something the user should know, e.g. that the file stayed fragmented.
    const std::wstring& Notice() const { return notice_; }

    // At startup: the videos a run that crashed was writing, made plain MP4 (#24). Newest first.
    static std::vector<std::wstring> Recover();

    const std::wstring& EncoderName() const { return encoderName_; }
    bool HardwareEncoder() const { return hardwareUsed_; }
    bool HasAudio() const { return audioStream_ != MAXDWORD; }
    // What Open tried before the encoder it settled on, or before giving up: "hardware+params: BeginWriting
    // 0xC00D36B4; ...". For tests and logs.
    const std::wstring& Tried() const { return tried_; }
    bool Failed() const { return failed_; }
    int64_t VideoFrames() const { return videoFrames_; }
    // End of the last frame written (100 ns): the video's length so far.
    int64_t VideoEnd() const { return videoEnd_; }

private:
    bool TryOpen(const std::wstring& path, bool hardware, bool codecParams, std::wstring& err);
    bool WriteAudioQueue();
    void Close();
    void Mark();
    void Unmark();

    VideoConfig v_;
    AudioConfig a_;
    std::wstring path_;
    ReplayRing* ring_ = nullptr;
    std::wstring marker_;  // fragmented: names path_ while it is written, for Recover
    bool refuseFinish_ = false;
    std::wstring notice_;
    std::wstring tried_;
    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11Multithread> mt_;
    ComPtr<ID3D11VideoDevice> vdev_;
    ComPtr<ID3D11VideoContext> vctx_;
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum_;
    ComPtr<ID3D11VideoProcessor> vp_;
    ComPtr<ID3D11Texture2D> inTex_;
    ComPtr<ID3D11VideoProcessorInputView> inView_;
    std::map<std::pair<ID3D11Texture2D*, UINT>, ComPtr<ID3D11VideoProcessorOutputView>> outViews_;
    std::vector<ComPtr<ID3D11Texture2D>> tex_;

    UINT resetToken_ = 0;
    ComPtr<IMFDXGIDeviceManager> devManager_;
    ComPtr<IMFVideoSampleAllocatorEx> allocator_;
    ComPtr<IMFSinkWriter> writer_;
    ComPtr<ICodecAPI> codecApi_;  // the video encoder's, for forced keyframes; null if it has none
    DWORD videoStream_ = MAXDWORD, audioStream_ = MAXDWORD;
    std::wstring encoderName_;
    bool hardwareUsed_ = false;
    bool failed_ = false;
    bool started_ = false;
    int64_t videoFrames_ = 0;
    int64_t videoEnd_ = 0;
    std::vector<uint32_t> frameDur_;  // VFR: each frame's length as written (100 ns), for Finish to set in the file
    ComPtr<IMFSample> held_;  // VFR: the frame waiting for the next one
    int64_t heldTime_ = 0;
    bool heldKey_ = false;
    int64_t lastKey_ = INT64_MIN / 2;

    std::mutex audioMtx_;
    std::vector<int16_t> audioQueue_;
    int64_t audioFrames_ = 0;  // written to the sink so far
};
