#pragma once

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
    int qualityVsSpeed = 50;     // 0 = fastest, 100 = best
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
    // "NVIDIA H.264 Encoder MFT" -> "NVIDIA".
    static std::wstring Vendor(const std::wstring& encoderName);
    // Reasonable bitrate for the size and frame rate when the user leaves it on automatic.
    static UINT32 AutoBitrate(int w, int h, int fps, bool hevc);

    // dev must have multithread protection on; it's shared with the encoder.
    bool Open(const std::wstring& path, const VideoConfig& v, const AudioConfig& a, ID3D11Device* dev,
              std::wstring& err);
    bool IsOpen() const { return writer_ != nullptr; }

    // Converts a BGRA frame into a pooled NV12 sample. Don't hold the device lock when calling.
    ComPtr<IMFSample> Convert(ID3D11Texture2D* bgra);
    // Writes queued audio, then the frame. Call from one thread.
    bool WriteVideo(IMFSample* sample, int64_t time, int64_t duration);
    // Thread-safe; queued until the next WriteVideo/Finish. Interleaved 16-bit stereo.
    void PushAudio(const int16_t* pcm, size_t frames);

    bool Finish(std::wstring& err);
    void Abort();

    const std::wstring& EncoderName() const { return encoderName_; }
    bool HardwareEncoder() const { return hardwareUsed_; }
    bool HasAudio() const { return audioStream_ != MAXDWORD; }
    bool Failed() const { return failed_; }
    int64_t VideoFrames() const { return videoFrames_; }

private:
    bool TryOpen(const std::wstring& path, bool hardware, bool codecParams, std::wstring& err);
    bool WriteAudioQueue();
    void Close();

    VideoConfig v_;
    AudioConfig a_;
    std::wstring path_;
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
    DWORD videoStream_ = MAXDWORD, audioStream_ = MAXDWORD;
    std::wstring encoderName_;
    bool hardwareUsed_ = false;
    bool failed_ = false;
    bool started_ = false;
    int64_t videoFrames_ = 0;

    std::mutex audioMtx_;
    std::vector<int16_t> audioQueue_;
    int64_t audioFrames_ = 0;  // written to the sink so far
};
