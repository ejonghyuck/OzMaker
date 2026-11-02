#pragma once
#include "AudioCapture.h"
#include "FrameStore.h"
#include "Mp4Writer.h"

enum class CaptureSource { Window, Monitor, Region };

// Record straight to an MP4 file instead of a clip for the editor.
struct VideoOptions {
    std::wstring path;
    VideoConfig video;          // size and frame rate are filled in by Capture
    AudioConfig audio;          // enabled = record sound
    bool processAudio = false;  // only the target process tree (window capture) instead of the system mix
    DWORD audioPid = 0;
};

struct CaptureOptions {
    CaptureSource source = CaptureSource::Window;
    HWND target = nullptr;       // Window
    bool monitorMode = false;    // Window: capture the monitor and crop to the window (robust for fullscreen games);
                                 // other windows over it are blacked out while it isn't in front
    HMONITOR monitor = nullptr;  // Monitor / Region
    RECT region{};               // Region, screen pixels
    bool clientOnly = true;      // Window: drop title bar and borders
    bool cursor = true;
    int fps = 30;
    int maxEdge = 0;             // 0 = original size, else longest edge limit in pixels
    int boxLong = 0, boxShort = 0;  // fit inside this box in either orientation (video presets); 0 = no limit
    std::optional<VideoOptions> video;
};

struct CaptureResult {
    std::shared_ptr<FrameStore> clip;  // clip mode
    bool video = false;                // video mode: the fields below
    bool ok = false;
    bool audio = false;
    bool hardware = false;  // GPU encoder
    std::wstring path, error, notice, encoder;
    double seconds = 0;
    uint64_t bytes = 0;
};

// Windows.Graphics.Capture session. Frames are cropped/scaled on the GPU, then either read back through a small
// staging ring into a FrameStore, or sampled at a constant rate and encoded to MP4 on the GPU.
// Runs on the capture thread pool; Start/Stop on the UI thread.
class Capture {
public:
    Capture() = default;
    ~Capture();

    static bool IsSupported();
    // closedMsg is posted when the source goes away (wParam 0) or the video encoder fails (wParam 1).
    bool Start(const CaptureOptions& opt, HWND notify, UINT closedMsg, std::wstring& err);
    CaptureResult Stop();

    bool Running() const { return running_; }
    bool StoreFailed() const;  // clip mode: the temp file couldn't be written (disk full)
    // While paused nothing is recorded, and the recording continues afterwards as if the pause never happened.
    void SetPaused(bool on);
    bool Paused() const { return pausedAt_ != 0; }
    double Elapsed() const;  // recorded time, pauses left out
    size_t Frames() const;
    size_t Scaled() const { return scaled_; }  // frames scaled on the GPU so far (tests)
    int OutWidth() const { return outW_; }
    int OutHeight() const { return outH_; }

private:
    void OnFrame(winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& pool);
    bool SourceRect(int contentW, int contentH, RECT& r) const;
    void CoveredRects(const RECT& sr, std::vector<RECT>& out) const;
    bool Setup(int srcW, int srcH);
    // covered: parts of sr (relative to it) to black out.
    void Process(ID3D11Texture2D* tex, const RECT& sr, int64_t ts, const std::vector<RECT>& covered);
    void FinishFrame();
    void DrainOne();
    void TickLoop();
    void Fail(std::wstring msg);
    void ReleaseGpu();

    CaptureOptions opt_;
    HWND notify_ = nullptr;
    UINT closedMsg_ = 0;
    RECT monitorRect_{};

    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11DeviceContext1> ctx1_;  // ClearView for the covered parts
    ComPtr<ID3D11Multithread> mt_;  // video mode: the encoder shares the device
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice rtDevice_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
    winrt::event_token frameToken_{}, closedToken_{};
    winrt::Windows::Graphics::SizeInt32 poolSize_{};

    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11Texture2D> mip_;
    ComPtr<ID3D11ShaderResourceView> mipSrv_;
    int mipW_ = 0, mipH_ = 0, mipLevels_ = 0;
    ComPtr<ID3D11Texture2D> out_;
    ComPtr<ID3D11RenderTargetView> outRtv_;
    static constexpr int kRing = 3;
    ComPtr<ID3D11Texture2D> staging_[kRing];
    int ring_ = 0;
    struct Pending { int idx; int64_t ts; };
    std::deque<Pending> pending_;
    std::vector<RECT> covered_;
    // What is left to do to the frame Process took: scale it from mip_ into out_, black out what covers it.
    // Under the device lock. Video mode leaves it to the ticker (finishDue_), which needs one frame per tick.
    struct Finish {
        bool scaled = false;
        int dx = 0, dy = 0, dw = 0, dh = 0, lod = 0;
        std::vector<D3D11_RECT> mask;
    } finish_;
    bool finishDue_ = false;
    std::atomic<size_t> scaled_{0};

    std::shared_ptr<FrameStore> store_;
    int outW_ = 0, outH_ = 0;
    bool setupDone_ = false, setupFailed_ = false;
    int64_t firstTs_ = -1, nextDue_ = 0, interval_ = 0, startQpc_ = 0;
    int64_t lastTs_ = 0, lastArrivalQpc_ = 0;  // the last frame: its time in the clip, when it came (minus pauses)
    std::atomic<int64_t> pausedAt_{0};     // QPC (100 ns) of the pause in progress, 0 = not paused
    std::atomic<int64_t> pausedTotal_{0};  // length of the pauses so far
    std::atomic<size_t> frames_{0};
    mutable std::mutex mtx_;
    std::atomic<bool> running_{false};

    // video mode
    std::unique_ptr<Mp4Writer> writer_;
    AudioCapture audio_;
    std::thread ticker_;
    HANDLE tickStop_ = nullptr, firstFrame_ = nullptr, resumed_ = nullptr;
    std::wstring videoError_, notice_;
    std::atomic<bool> failed_{false};
};
