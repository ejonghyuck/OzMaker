#include "pch.h"
#include "AudioCapture.h"
#include "Util.h"

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <avrt.h>
#include <mmdeviceapi.h>

namespace {

struct ActivateHandler : winrt::implements<ActivateHandler, IActivateAudioInterfaceCompletionHandler> {
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT hr = E_PENDING;
    ComPtr<IAudioClient> client;

    ~ActivateHandler() { CloseHandle(done); }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT act = E_FAIL;
        ComPtr<IUnknown> unk;
        hr = op->GetActivateResult(&act, &unk);
        if (SUCCEEDED(hr)) hr = act;
        if (SUCCEEDED(hr)) hr = unk.As(&client);
        SetEvent(done);
        return S_OK;
    }
};

DWORD OsBuild() {
    using Fn = LONG(WINAPI*)(OSVERSIONINFOW*);
    static DWORD build = [] {
        OSVERSIONINFOW vi{sizeof(vi)};
        if (auto fn = (Fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) fn(&vi);
        return vi.dwBuildNumber;
    }();
    return build;
}

constexpr REFERENCE_TIME kBufferDuration = 5'000'000;  // 500 ms of headroom if the thread is starved

// PKEY_Device_FriendlyName ("Microphone (USB Audio)").
constexpr PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

}  // namespace

bool AudioCapture::ProcessLoopbackSupported() { return OsBuild() >= 19041; }

DWORD AudioCapture::AudioProcessOf(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, _countof(cls));
    if (wcscmp(cls, L"ApplicationFrameWindow") == 0) {
        // UWP: the frame belongs to ApplicationFrameHost; the app itself owns the hosted CoreWindow.
        struct Ctx { DWORD host, app; } ctx{pid, 0};
        EnumChildWindows(
            hwnd,
            [](HWND child, LPARAM lp) -> BOOL {
                auto* c = reinterpret_cast<Ctx*>(lp);
                DWORD p = 0;
                GetWindowThreadProcessId(child, &p);
                if (p != c->host) {
                    c->app = p;
                    return FALSE;
                }
                return TRUE;
            },
            (LPARAM)&ctx);
        if (ctx.app) pid = ctx.app;
    }
    return pid;
}

std::vector<AudioCapture::Device> AudioCapture::InputDevices() {
    std::vector<Device> out;
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDeviceCollection> all;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
        FAILED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &all)))
        return out;
    std::wstring def;
    ComPtr<IMMDevice> d;
    LPWSTR id = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &d)) && SUCCEEDED(d->GetId(&id))) {
        def = id;
        CoTaskMemFree(id);
    }
    UINT n = 0;
    all->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IMMDevice> dev;
        ComPtr<IPropertyStore> props;
        if (FAILED(all->Item(i, &dev)) || FAILED(dev->GetId(&id))) continue;
        Device e;
        e.id = id;
        CoTaskMemFree(id);
        e.isDefault = e.id == def;
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && SUCCEEDED(props->GetValue(kFriendlyName, &pv)) &&
            pv.vt == VT_LPWSTR)
            e.name = pv.pwszVal;
        PropVariantClear(&pv);
        if (e.name.empty()) e.name = L"이름 없는 장치";
        out.push_back(std::move(e));
    }
    return out;
}

bool AudioCapture::Start(Source src, DWORD pid, int sampleRate, int64_t originQpc, Sink sink, std::wstring& err,
                         const std::wstring& device) {
    Stop(0);
    src_ = src;
    pid_ = pid;
    device_ = device;
    rate_ = sampleRate;
    origin_ = originQpc;
    sink_ = std::move(sink);
    written_ = 0;
    pausedAt_ = 0;
    convert_ = false;
    resamplePos_ = 0;
    lastL_ = lastR_ = 0;
    startHr_ = E_FAIL;
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this] { Run(); });
    WaitForSingleObject(ready_, INFINITE);  // activation itself gives up after a few seconds
    if (FAILED(startHr_)) {
        thread_.join();
        CloseHandle(stop_);
        CloseHandle(ready_);
        stop_ = ready_ = nullptr;
        sink_ = nullptr;
        err = util::Format(src == Source::Process ? L"이 창의 소리를 녹음할 수 없습니다. (0x%08X)"
                           : src == Source::Mic       ? L"마이크를 녹음할 수 없습니다. (0x%08X)"
                                                      : L"시스템 소리를 녹음할 수 없습니다. (0x%08X)",
                           (unsigned)startHr_);
        return false;
    }
    return true;
}

void AudioCapture::Stop(int64_t endQpc) {
    if (!thread_.joinable()) return;
    SetEvent(stop_);
    thread_.join();
    if (endQpc > 0) PadTo(((pausedAt_ ? pausedAt_ : endQpc) - origin_) * rate_ / 10'000'000);
    pausedAt_ = 0;
    CloseHandle(stop_);
    CloseHandle(ready_);
    stop_ = ready_ = nullptr;
    sink_ = nullptr;
}

void AudioCapture::Pause(int64_t qpc) {
    std::lock_guard lk(mtx_);
    if (!pausedAt_) pausedAt_ = qpc;
}

void AudioCapture::Resume(int64_t qpc) {
    std::lock_guard lk(mtx_);
    if (!pausedAt_) return;
    PadTo((pausedAt_ - origin_) * rate_ / 10'000'000);  // the stream reaches the pause, then the clock skips it
    origin_ += qpc - pausedAt_;
    pausedAt_ = 0;
}

HRESULT AudioCapture::Open(ComPtr<IAudioClient>& client, ComPtr<IAudioCaptureClient>& cap, HANDLE event) {
    client.Reset();
    cap.Reset();
    convert_ = false;
    resamplePos_ = 0;
    WAVEFORMATEX want{};
    want.wFormatTag = WAVE_FORMAT_PCM;
    want.nChannels = 2;
    want.nSamplesPerSec = rate_;
    want.wBitsPerSample = 16;
    want.nBlockAlign = 4;
    want.nAvgBytesPerSec = rate_ * 4;

    HRESULT hr = S_OK;
    if (src_ == Source::Process) {
        AUDIOCLIENT_ACTIVATION_PARAMS ap{};
        ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        ap.ProcessLoopbackParams.TargetProcessId = pid_;
        ap.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        PROPVARIANT pv{};
        pv.vt = VT_BLOB;
        pv.blob.cbSize = sizeof(ap);
        pv.blob.pBlobData = reinterpret_cast<BYTE*>(&ap);
        auto handler = winrt::make_self<ActivateHandler>();
        ComPtr<IActivateAudioInterfaceAsyncOperation> op;
        hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv,
                                         handler.get(), &op);
        if (SUCCEEDED(hr))
            hr = WaitForSingleObject(handler->done, 5000) == WAIT_OBJECT_0 ? handler->hr
                                                                         : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        if (SUCCEEDED(hr)) {
            client = handler->client;
            // The virtual loopback device takes whatever PCM format it is asked for.
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                    kBufferDuration, 0, &want, nullptr);
        }
    } else {
        // System: the default output device, looped back. Mic: the chosen input device, else the default one.
        const bool mic = src_ == Source::Mic;
        const DWORD loopback = mic ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK;
        ComPtr<IMMDeviceEnumerator> en;
        ComPtr<IMMDevice> dev;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
        if (SUCCEEDED(hr))
            hr = mic && !device_.empty() ? en->GetDevice(device_.c_str(), &dev)
                                         : en->GetDefaultAudioEndpoint(mic ? eCapture : eRender, eConsole, &dev);
        if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
        if (SUCCEEDED(hr)) {
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    loopback | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                    kBufferDuration, 0, &want, nullptr);
            if (FAILED(hr)) {
                // The engine wouldn't convert: take its mix format and convert here.
                client.Reset();
                WAVEFORMATEX* mix = nullptr;
                hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
                if (SUCCEEDED(hr)) hr = client->GetMixFormat(&mix);
                if (SUCCEEDED(hr)) {
                    bool isFloat = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
                    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
                        isFloat = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
                    srcFloat_ = isFloat;
                    srcChannels_ = mix->nChannels;
                    srcBits_ = mix->wBitsPerSample;
                    srcRate_ = (int)mix->nSamplesPerSec;
                    if ((isFloat && srcBits_ != 32) || (!isFloat && srcBits_ != 16 && srcBits_ != 32) || srcChannels_ < 1)
                        hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
                    else
                        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, loopback | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                kBufferDuration, 0, mix, nullptr);
                    convert_ = true;
                    CoTaskMemFree(mix);
                }
            }
        }
    }
    if (SUCCEEDED(hr)) hr = client->SetEventHandle(event);
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&cap));
    if (SUCCEEDED(hr)) hr = client->Start();
    if (FAILED(hr)) {
        cap.Reset();
        client.Reset();
    }
    return hr;
}

void AudioCapture::Run() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    DWORD taskIdx = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Audio", &taskIdx);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> cap;
    HRESULT hr = Open(client, cap, event);
    startHr_ = hr;
    SetEvent(ready_);

    if (SUCCEEDED(hr)) {
        const int64_t idlePad = 1'000'000;  // pad silence this far behind real time when nothing is playing
        int64_t lastPacket = util::Qpc100ns(), lastRetry = 0;
        bool alive = true;
        HANDLE waits[2] = {stop_, event};
        while (WaitForMultipleObjects(2, waits, FALSE, 20) != WAIT_OBJECT_0) {
            bool got = false;
            if (!alive && util::Qpc100ns() - lastRetry > 5'000'000) {
                // The device went away (headset unplugged, output switched): follow the new default device.
                // The gap is filled with silence below.
                lastRetry = util::Qpc100ns();
                if (client) client->Stop();
                alive = SUCCEEDED(Open(client, cap, event));
                lastL_ = lastR_ = 0;
            }
            const int frameRate = convert_ ? srcRate_ : rate_;
            while (alive) {
                UINT32 next = 0;
                if (FAILED(cap->GetNextPacketSize(&next))) {
                    alive = false;
                    lastRetry = util::Qpc100ns();
                    break;
                }
                if (next == 0) break;
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                UINT64 devPos = 0, qpcPos = 0;
                if (FAILED(cap->GetBuffer(&data, &frames, &flags, &devPos, &qpcPos))) {
                    alive = false;
                    lastRetry = util::Qpc100ns();
                    break;
                }
                int64_t qpc = (int64_t)qpcPos;
                if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) || qpc == 0)
                    qpc = util::Qpc100ns() - (int64_t)frames * 10'000'000 / frameRate;
                {
                    std::lock_guard lk(mtx_);
                    if (!pausedAt_ || qpc < pausedAt_) Deliver(data, frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0, qpc);
                }
                cap->ReleaseBuffer(frames);
                got = true;
            }
            int64_t now = util::Qpc100ns();
            if (got) {
                lastPacket = now;
            } else if (now - lastPacket > idlePad) {
                std::lock_guard lk(mtx_);
                int64_t upTo = now - idlePad;
                if (pausedAt_) upTo = std::min(upTo, pausedAt_);
                PadTo((upTo - origin_) * rate_ / 10'000'000);
            }
        }
        if (client) client->Stop();
    }
    cap.Reset();
    client.Reset();
    CloseHandle(event);
    if (task) AvRevertMmThreadCharacteristics(task);
}

void AudioCapture::Deliver(const BYTE* data, UINT32 frames, bool silent, int64_t qpc) {
    const int16_t* pcm = silent ? nullptr : reinterpret_cast<const int16_t*>(data);
    size_t n = frames;
    if (convert_) {
        // Downmix to stereo and resample linearly to the output rate.
        scratch_.clear();
        auto fetch = [&](int64_t i, int ch) -> float {
            if (i < 0) return (ch ? lastR_ : lastL_) / 32768.f;
            if (silent) return 0.f;
            int c = std::min(ch, srcChannels_ - 1);
            size_t k = (size_t)i * srcChannels_ + c;
            if (srcFloat_) return reinterpret_cast<const float*>(data)[k];
            if (srcBits_ == 16) return reinterpret_cast<const int16_t*>(data)[k] / 32768.f;
            return reinterpret_cast<const int32_t*>(data)[k] / 2147483648.f;
        };
        auto to16 = [](float v) { return (int16_t)std::clamp((int)std::lround(v * 32767.f), -32768, 32767); };
        const double step = (double)srcRate_ / rate_;
        double p = resamplePos_;
        while (p < (double)frames - 1) {
            int64_t i0 = (int64_t)std::floor(p);
            float f = (float)(p - i0);
            for (int ch = 0; ch < 2; ++ch)
                scratch_.push_back(to16(fetch(i0, ch) * (1 - f) + fetch(i0 + 1, ch) * f));
            p += step;
        }
        resamplePos_ = p - frames;
        if (frames > 0) {
            lastL_ = to16(fetch(frames - 1, 0));
            lastR_ = to16(fetch(frames - 1, 1));
        }
        pcm = scratch_.data();
        n = scratch_.size() / 2;
    }
    if (n == 0) return;

    int64_t target = (qpc - origin_) * rate_ / 10'000'000;
    const int64_t tol = rate_ / 50;  // 20 ms of timestamp jitter is absorbed
    if (target > written_ + tol) {
        PadTo(target);
    } else if (target < written_ - tol) {
        // Overlaps what was already written (late packet or clock drift): drop the overlap.
        int64_t skip = std::min<int64_t>(written_ - target, (int64_t)n);
        if (pcm) pcm += skip * 2;
        n -= (size_t)skip;
    }
    if (n > 0) Append(pcm, n);
}

void AudioCapture::PadTo(int64_t frame) {
    while (written_ < frame) {
        size_t n = (size_t)std::min<int64_t>(frame - written_, rate_ / 10);
        Append(nullptr, n);
    }
}

void AudioCapture::Append(const int16_t* pcm, size_t frames) {
    if (!pcm) {
        static const std::vector<int16_t> zeros(48000 / 10 * 2 * 2);
        size_t left = frames;
        while (left > 0) {
            size_t n = std::min(left, zeros.size() / 2);
            if (sink_) sink_(zeros.data(), n);
            left -= n;
        }
    } else if (sink_) {
        sink_(pcm, frames);
    }
    written_ += frames;
}

// ---------------------------------------------------------------- AudioMix

void AudioMix::Reset(int inputs, AudioCapture::Sink out) {
    std::lock_guard lk(mtx_);
    in_.assign(inputs, In{});
    out_ = std::move(out);
}

void AudioMix::SetGain(int input, float gain) {
    std::lock_guard lk(mtx_);
    in_[input].gain = gain;
}

void AudioMix::Drop(int input) {
    std::lock_guard lk(mtx_);
    in_[input].live = false;
    in_[input].q.clear();
    size_t n = SIZE_MAX;
    for (const In& i : in_)
        if (i.live) n = std::min(n, i.q.size() / 2);
    if (n != SIZE_MAX) Emit(n);
}

void AudioMix::Flush() {
    std::lock_guard lk(mtx_);
    size_t n = 0;
    for (const In& i : in_)
        if (i.live) n = std::max(n, i.q.size() / 2);
    for (In& i : in_)
        if (i.live) i.q.resize(n * 2, 0);
    Emit(n);
}

void AudioMix::Push(int input, const int16_t* pcm, size_t frames) {
    std::lock_guard lk(mtx_);
    In& in = in_[input];
    if (!in.live) return;
    in.q.insert(in.q.end(), pcm, pcm + frames * 2);
    size_t n = SIZE_MAX;
    for (const In& i : in_)
        if (i.live) n = std::min(n, i.q.size() / 2);
    Emit(n);
}

void AudioMix::Emit(size_t frames) {
    if (frames == 0 || !out_) return;
    mixed_.resize(frames * 2);
    for (size_t k = 0; k < frames * 2; ++k) {
        float s = 0;
        for (const In& i : in_)
            if (i.live) s += i.q[k] * i.gain;
        mixed_[k] = (int16_t)std::clamp((int)std::lround(s), -32768, 32767);
    }
    for (In& i : in_)
        if (i.live) i.q.erase(i.q.begin(), i.q.begin() + frames * 2);
    out_(mixed_.data(), frames);
}
