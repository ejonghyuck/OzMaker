#pragma once
#include "AudioCapture.h"
#include "FrameStore.h"
#include "ImageFile.h"
#include "Mp4Writer.h"

enum class CaptureSource { Window, Monitor, Region };

// Record straight to an MP4 file instead of a clip for the editor.
struct VideoOptions {
    std::wstring path;
    VideoConfig video;          // size and frame rate are filled in by Capture
    AudioConfig audio;          // enabled = an audio track: sound, the mic or both mixed
    bool sound = false;         // what plays
    bool processAudio = false;  // only the target process tree (window capture) instead of the system mix
    DWORD audioPid = 0;
    bool mic = false;           // the microphone mixed in (#21)
    std::wstring micDevice;     // endpoint ID, empty = Windows' default input
    float micGain = 1;
    ReplayRing* ring = nullptr;  // 순간 리플레이 (#20): into memory instead of path; it outlives the capture
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
    bool clickFx = false;        // a ripple where the mouse is pressed, a dot while it is held (#21)
    int fps = 30;
    int maxEdge = 0;             // 0 = original size, else longest edge limit in pixels
    int boxLong = 0, boxShort = 0;  // fit inside this box in either orientation (video presets); 0 = no limit
    std::optional<VideoOptions> video;
    // How a monitor (Monitor, Region, monitorMode) is taken: 0 = desktop duplication where Windows.Graphics.Capture
    // can't drop its yellow border (Windows 10), 1 = duplication wherever it works, 2 = Windows.Graphics.Capture.
    int screenVia = 0;
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
    // Windows 11 leaves the yellow border off only for an app that asked first; turning it off alone is ignored.
    // RequestBorderless asks once, on a thread of its own (the answer is awaited, which the window's thread must not
    // do). BorderlessAccess waits up to waitMs for the answer: an AppCapabilityAccessStatus (4 = allowed), -1 without
    // one, -2 where the border can't be turned off at all (Windows 10).
    static void RequestBorderless();
    static int BorderlessAccess(int waitMs);
    // closedMsg is posted when the source goes away (wParam 0) or the video encoder fails (wParam 1).
    // tag goes out as its lParam: which of several recordings (#48) it is.
    bool Start(const CaptureOptions& opt, HWND notify, UINT closedMsg, std::wstring& err, LPARAM tag = 0);
    CaptureResult Stop();

    bool Running() const { return running_; }
    bool StoreFailed() const;  // clip mode: the temp file couldn't be written (disk full)
    // While paused nothing is recorded, and the recording continues afterwards as if the pause never happened.
    void SetPaused(bool on);
    bool Paused() const { return pausedAt_ != 0; }
    double Elapsed() const;  // recorded time, pauses left out
    size_t Frames() const;
    size_t Scaled() const { return scaled_; }  // frames scaled on the GPU so far (tests)
    // Tests: a click as if the left button went down at `at` (screen pixels) now and up `hold` (100 ns) later.
    void InjectClick(POINT at, int64_t hold);
    size_t ClickFrames() const { return clickFrames_; }  // clip frames added for highlights (tests)
    // Screenshot (#63): the newest picture of the source at its own size, whatever size is recorded, also while paused
    // and when the screen hasn't changed for a while. False before the first one. The window's thread.
    bool Shot(image::Picture& pic);
    // What covers a window in monitorMode stays black in a screenshot too. rects: pixels of the picture.
    static void BlackOut(image::Picture& pic, const std::vector<RECT>& rects);
    int OutWidth() const { return outW_; }
    int OutHeight() const { return outH_; }
    bool Duplicating() const { return dupThread_.joinable(); }  // the monitor comes through desktop duplication
    // Tests: the cursor drawn the way duplication draws it, over grey; detail says what was found.
    static bool CursorDrawCheck(std::wstring& detail);

private:
    void OnFrame(winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& pool);
    // Under mtx_: whether a picture captured at qpc is taken (accept), or only kept under a click highlight (base);
    // ts is its time on the recording's clock. Take hands a picture of the source (cw × ch) on to Process.
    void Admit(int64_t qpc, int64_t& ts, bool& accept, bool& base);
    void Take(ID3D11Texture2D* tex, int cw, int ch, int64_t ts, int64_t qpc, bool accept, bool record = true);
    void KeepShot(ID3D11Texture2D* tex, const RECT& sr, const std::vector<RECT>& covered);

    // Desktop duplication (#20): Windows 10 draws a yellow border around what Windows.Graphics.Capture takes, and
    // has no way to turn it off (nor has Windows 11 when the request wasn't allowed). Duplication has none. dupThread_ copies the screen into desk_ whenever it changes
    // and hands it on at the frame rate, with the cursor drawn on by GDI (duplication leaves it out).
    bool Duplicate();  // (re)opens dup_ on the monitor; false while it can't be duplicated
    void DupLoop();
    bool PushDesktop(int64_t& retryIn);  // false: too soon, again in retryIn (100 ns)
    ID3D11Texture2D* WithCursor();       // under the device lock: desk_, or deskCur_ with the cursor drawn on it
    HMONITOR dupMonitor_ = nullptr;
    ComPtr<IDXGIOutputDuplication> dup_;
    ComPtr<ID3D11Texture2D> desk_, deskCur_;
    int deskW_ = 0, deskH_ = 0;
    bool haveDesk_ = false;
    HCURSOR cursor_ = nullptr;  // the last cursor drawn, its hotspot and size
    POINT hotspot_{};
    SIZE cursorSize_{};
    std::thread dupThread_;
    HANDLE dupStop_ = nullptr;
    bool SourceRect(int contentW, int contentH, RECT& r) const;
    void CoveredRects(const RECT& sr, std::vector<RECT>& out) const;
    bool Setup(int srcW, int srcH);
    // covered: parts of sr (relative to it) to black out. qpc: when the frame was captured. emit = false (clip
    // mode): only keep it in out_, for the click highlight frames in between.
    void Process(ID3D11Texture2D* tex, const RECT& sr, int64_t ts, const std::vector<RECT>& covered, int64_t qpc,
                 bool emit);
    void FinishFrame();
    void DrainOne();
    void TickLoop();
    void Fail(std::wstring msg);
    void ReleaseGpu();

    // Click highlight (#21): clickThread_ polls the mouse; the highlights are drawn with Direct2D over a copy of
    // out_ (fx_). Video mode draws them at each tick. Clip mode draws them on the frames that come in, and while
    // one shows adds frames of its own from out_ (the screen may not change), then one without it.
    struct Click {
        int64_t down = 0, up = 0;  // QPC; up = 0 while held
        POINT at{};                // screen pixels
        bool right = false;
    };
    void ClickLoop();
    bool ClicksShow(int64_t qpc);
    bool DrawClicks(int64_t qpc);       // under the device lock: false when nothing shows at qpc
    void EmitClickFrame(int64_t qpc);  // clip mode, under mtx_
    POINT TextureOrigin(int contentW, int contentH) const;
    void StopClicks();
    std::thread clickThread_;
    HANDLE clickStop_ = nullptr;
    std::mutex clickMtx_;  // clicks_, cursorPos_
    std::deque<Click> clicks_;
    POINT cursorPos_{};
    // Screen pixels -> out_ pixels for the frame in out_ (under the device lock).
    struct ClickMap {
        float ox = 0, oy = 0, fx = 1, fy = 1, dx = 0, dy = 0;
    } clickMap_;
    POINT texOrigin_{};  // screen position of the captured texture's top-left, OnFrame -> Process
    float dpiScale_ = 1;
    std::atomic<bool> fxShown_{false};  // clip mode: the last frame out had a highlight on it
    std::atomic<size_t> clickFrames_{0};
    bool fxFailed_ = false;
    ComPtr<ID3D11Texture2D> fx_;
    ComPtr<ID2D1Factory1> d2d_;
    ComPtr<ID2D1Device> d2dDev_;
    ComPtr<ID2D1DeviceContext> d2dCtx_;
    ComPtr<ID2D1Bitmap1> fxTarget_;
    ComPtr<ID2D1SolidColorBrush> fxBrush_;

    CaptureOptions opt_;
    HWND notify_ = nullptr;
    UINT closedMsg_ = 0;
    LPARAM tag_ = 0;
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
    ComPtr<ID3D11Texture2D> shot_, shotRead_;  // under the device lock, with shotMask_: what a screenshot takes
    std::vector<RECT> shotMask_;
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
    // monitorMode: the last picture with nothing over the window. What covers it while it isn't in front is filled
    // from here instead of black (black only until there was such a picture).
    ComPtr<ID3D11Texture2D> clean_;
    bool haveClean_ = false;
    uint64_t shots_ = 0;  // video mode: frames Process took (under the device lock)
    // Variable frame rate (#55). Windows hands over frames of a window that hasn't changed too, so the ticker takes
    // the picture (Snapshot, into cand_) and compares it on the GPU with the last frame written (prev_): blocks with
    // no pixel changed are discarded, and an occlusion query says whether any were left (Changed).
    ID3D11Texture2D* Snapshot(ID3D11Texture2D* picture, bool compare);  // under the device lock
    bool Changed();                                                      // without it
    void Written();                                                      // under it: cand_ went to the encoder
    ComPtr<ID3D11Texture2D> cand_, prev_;
    ComPtr<ID3D11ShaderResourceView> candSrv_, prevSrv_;
    ComPtr<ID3D11PixelShader> diffPs_;
    ComPtr<ID3D11RenderTargetView> diffRtv_;
    ComPtr<ID3D11Query> diffQuery_;
    UINT diffW_ = 0, diffH_ = 0;
    bool diffReady_ = false;    // the textures and shader above were made
    bool diffPending_ = false;  // a comparison is on its way
    std::atomic<size_t> scaled_{0};

    std::shared_ptr<FrameStore> store_;
    int outW_ = 0, outH_ = 0;
    bool setupDone_ = false, setupFailed_ = false;
    int64_t firstTs_ = -1, nextDue_ = 0, interval_ = 0, startQpc_ = 0;
    int64_t lastTs_ = 0, lastArrivalQpc_ = 0;  // the last frame: its time in the clip, when it came (minus pauses)
    std::atomic<int64_t> pausedAt_{0};     // QPC (100 ns) of the pause in progress, 0 = not paused
    std::atomic<int64_t> pausedTotal_{0};  // length of the pauses so far
    std::atomic<int64_t> stopAt_{0};       // video: the stop press on the recording's clock (minus pauses)
    std::atomic<size_t> frames_{0};
    mutable std::mutex mtx_;
    std::atomic<bool> running_{false};

    // video mode
    std::unique_ptr<Mp4Writer> writer_;
    AudioCapture audio_, mic_;
    AudioMix mix_;  // audio_ (input 0) and mic_ (input 1) into the writer
    std::thread ticker_;
    HANDLE tickStop_ = nullptr, firstFrame_ = nullptr, resumed_ = nullptr;
    std::wstring videoError_, notice_;
    std::atomic<bool> failed_{false};
};
