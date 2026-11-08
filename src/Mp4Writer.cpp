#include "pch.h"
#include "Mp4Writer.h"
#include "FrameStore.h"
#include "Mp4Unfragment.h"
#include "Replay.h"
#include "Util.h"

#include <codecapi.h>
#include <mferror.h>

void EnsureMediaFoundation() {
    static std::once_flag once;
    std::call_once(once, [] { MFStartup(MF_VERSION, MFSTARTUP_LITE); });
}

namespace {

void SetVideoBasics(IMFMediaType* t, const VideoConfig& v) {
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(t, MF_MT_FRAME_SIZE, v.width, v.height);
    MFSetAttributeRatio(t, MF_MT_FRAME_RATE, v.fps, 1);
    MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    // BT.709 limited range, matching what the video processor produces.
    t->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    t->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
}

std::wstring Hr(HRESULT hr) { return util::Format(L"0x%08X", (unsigned)hr); }

}  // namespace

Mp4Writer::Mp4Writer() { EnsureMediaFoundation(); }

Mp4Writer::~Mp4Writer() {
    if (writer_) Abort();
}

std::wstring Mp4Writer::FindEncoder(bool hevc, bool hardware) {
    EnsureMediaFoundation();
    MFT_REGISTER_TYPE_INFO out{MFMediaType_Video, hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264};
    UINT32 flags = MFT_ENUM_FLAG_SORTANDFILTER |
                   (hardware ? MFT_ENUM_FLAG_HARDWARE : MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT);
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    std::wstring name;
    if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &out, &acts, &count))) {
        for (UINT32 i = 0; i < count; ++i) {
            if (i == 0) {
                wchar_t* s = nullptr;
                UINT32 len = 0;
                if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &s, &len))) {
                    name = s;
                    CoTaskMemFree(s);
                }
                if (name.empty()) name = hevc ? L"HEVC" : L"H.264";
            }
            acts[i]->Release();
        }
        CoTaskMemFree(acts);
    }
    return name;
}

bool Mp4Writer::HevcDecoderAvailable() {
    EnsureMediaFoundation();
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_HEVC};
    const UINT32 flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE |
                         MFT_ENUM_FLAG_SORTANDFILTER;
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &in, nullptr, &acts, &count))) return false;
    for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
    return count > 0;
}

std::wstring Mp4Writer::Vendor(const std::wstring& name) {
    std::wstring v = name;
    for (const wchar_t* cut : {L" H.264", L" H264", L" HEVC", L" H.265", L" H265", L" Encoder", L"h264", L"hevc"}) {
        size_t p = v.find(cut);
        if (p != std::wstring::npos && p > 0) v.resize(p);
    }
    return v;
}

UINT32 Mp4Writer::AutoBitrate(int w, int h, int fps, bool hevc) {
    double bpp = hevc ? 0.08 : 0.12;  // bits per pixel per frame; screen content and games stay crisp
    double bits = (double)w * h * fps * bpp;
    bits = std::clamp(bits, 2e6, 80e6);
    return (UINT32)(std::round(bits / 5e5) * 5e5);
}

bool Mp4Writer::Open(const std::wstring& path, const VideoConfig& v, const AudioConfig& a, ID3D11Device* dev,
                     std::wstring& err, ReplayRing* ring) {
    v_ = v;
    a_ = a;
    ring_ = ring;
    path_ = ring ? std::wstring() : path;
    tried_.clear();
    // Windows' fragmented MP4 sink takes H.264 only: it refuses HEVC, even with the encoder's sequence header in
    // the type (--encoder-test). HEVC is written as a plain MP4, which a crash leaves without its index.
    if (v_.hevc) v_.fragmented = false;
    if (ring_) v_.hevc = false, v_.fragmented = true;  // the ring takes fragments
    dev_ = dev;
    dev_->GetImmediateContext(&ctx_);
    ctx_.As(&mt_);
    if (v_.bitrate == 0) v_.bitrate = AutoBitrate(v_.width, v_.height, v_.fps, v_.hevc);

    // GPU colour conversion BGRA -> NV12 (BT.709, limited range).
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate = {(UINT)v_.fps, 1};
    cd.InputWidth = v_.width;
    cd.InputHeight = v_.height;
    cd.OutputFrameRate = {(UINT)v_.fps, 1};
    cd.OutputWidth = v_.width;
    cd.OutputHeight = v_.height;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    HRESULT hr = dev_.As(&vdev_);
    if (SUCCEEDED(hr)) hr = ctx_.As(&vctx_);
    if (SUCCEEDED(hr)) hr = vdev_->CreateVideoProcessorEnumerator(&cd, &vpEnum_);
    if (SUCCEEDED(hr)) hr = vdev_->CreateVideoProcessor(vpEnum_.Get(), 0, &vp_);
    if (FAILED(hr)) {
        err = tr(L"GPU 영상 변환을 시작할 수 없습니다. (") + Hr(hr) + L")";
        Close();
        return false;
    }
    ComPtr<ID3D11VideoContext1> vctx1;
    if (SUCCEEDED(vctx_.As(&vctx1))) {
        vctx1->VideoProcessorSetStreamColorSpace1(vp_.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        vctx1->VideoProcessorSetOutputColorSpace1(vp_.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{}, outCs{};
        in.RGB_Range = 0;  // full
        in.YCbCr_Matrix = 1;
        outCs.YCbCr_Matrix = 1;
        outCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        vctx_->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &in);
        vctx_->VideoProcessorSetOutputColorSpace(vp_.Get(), &outCs);
    }
    RECT full{0, 0, v_.width, v_.height};
    vctx_->VideoProcessorSetStreamFrameFormat(vp_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    vctx_->VideoProcessorSetStreamAutoProcessingMode(vp_.Get(), 0, FALSE);
    vctx_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &full);
    vctx_->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE, &full);
    vctx_->VideoProcessorSetOutputTargetRect(vp_.Get(), TRUE, &full);

    // Encoder and converter share the capture device; NV12 frames come from a recycling pool.
    hr = MFCreateDXGIDeviceManager(&resetToken_, &devManager_);
    if (SUCCEEDED(hr)) hr = devManager_->ResetDevice(dev_.Get(), resetToken_);
    ComPtr<IMFAttributes> aa;
    ComPtr<IMFMediaType> nv12;
    if (SUCCEEDED(hr)) hr = MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&allocator_));
    if (SUCCEEDED(hr)) hr = allocator_->SetDirectXManager(devManager_.Get());
    if (SUCCEEDED(hr)) hr = MFCreateAttributes(&aa, 2);
    if (SUCCEEDED(hr)) {
        aa->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_RENDER_TARGET);
        aa->SetUINT32(MF_SA_D3D11_USAGE, D3D11_USAGE_DEFAULT);
        hr = MFCreateMediaType(&nv12);
    }
    if (SUCCEEDED(hr)) {
        SetVideoBasics(nv12.Get(), v_);
        nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        hr = allocator_->InitializeSampleAllocatorEx(4, 24, aa.Get(), nv12.Get());
    }
    if (FAILED(hr)) {
        err = tr(L"영상 버퍼를 만들 수 없습니다. (") + Hr(hr) + L")";
        Close();
        return false;
    }

    // Hardware first; codec tuning parameters are optional on some drivers; software as the last resort.
    if (TryOpen(path, v_.hardware, true, err) || TryOpen(path, v_.hardware, false, err) ||
        (v_.hardware && TryOpen(path, false, true, err))) {
        if (v_.fragmented && !ring_) Mark();
        return true;
    }
    Close();
    return false;
}

// OzMaker_<pid>_<qpc>.video next to the 움짤 recordings holds the path while the file is written: what a crash
// leaves behind tells the next start which file to convert.
void Mp4Writer::Mark() {
    marker_ = FrameStore::Dir() + util::Format(L"\\OzMaker_%lu_%lld.video", GetCurrentProcessId(), util::Qpc100ns());
    HANDLE h = CreateFileW(marker_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD bytes = (DWORD)(path_.size() * sizeof(wchar_t)), done = 0;
    if (h == INVALID_HANDLE_VALUE || !WriteFile(h, path_.data(), bytes, &done, nullptr)) marker_.clear();
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

void Mp4Writer::Unmark() {
    if (!marker_.empty()) DeleteFileW(marker_.c_str());
    marker_.clear();
}

std::vector<std::wstring> Mp4Writer::Recover() {
    const std::wstring dir = FrameStore::Dir();
    std::vector<std::wstring> markers;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\OzMaker_*.video").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            markers.push_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::vector<std::pair<uint64_t, std::wstring>> found;  // last write, path
    for (const std::wstring& name : markers) {
        DWORD pid = wcstoul(name.c_str() + 8, nullptr, 10);  // OzMaker_<pid>_<qpc>.video
        if (pid == GetCurrentProcessId() || !util::ProcessGone(pid)) continue;
        std::wstring marker = dir + L"\\" + name, path;
        HANDLE f = CreateFileW(marker.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            wchar_t buf[4096];
            DWORD got = 0;
            if (ReadFile(f, buf, sizeof(buf), &got, nullptr)) path.assign(buf, got / sizeof(wchar_t));
            CloseHandle(f);
        }
        DeleteFileW(marker.c_str());
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) continue;
        std::wstring err;
        bool empty = false;
        if (!mp4::Unfragment(path, err, &empty) && empty) {  // nothing reached the disk: no file to offer
            DeleteFileW(path.c_str());
            continue;
        }
        // A file that could not be converted stays fragmented: most players open it as it is.
        found.push_back({(uint64_t)fa.ftLastWriteTime.dwHighDateTime << 32 | fa.ftLastWriteTime.dwLowDateTime, path});
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::wstring> out;
    for (auto& f : found) out.push_back(std::move(f.second));
    return out;
}

bool Mp4Writer::TryOpen(const std::wstring& path, bool hardware, bool codecParams, std::wstring& err) {
    writer_.Reset();
    codecApi_.Reset();
    videoStream_ = audioStream_ = MAXDWORD;
    ComPtr<IMFAttributes> wa;
    HRESULT hr = MFCreateAttributes(&wa, 3);
    if (SUCCEEDED(hr)) {
        wa->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardware);
        wa->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, devManager_.Get());
        wa->SetGUID(MF_TRANSCODE_CONTAINERTYPE,
                    v_.fragmented ? MFTranscodeContainerType_FMPEG4 : MFTranscodeContainerType_MPEG4);
        if (ring_) {
            ComPtr<IMFByteStream> bs = ring_->NewStream();
            hr = bs ? MFCreateSinkWriterFromURL(nullptr, bs.Get(), wa.Get(), &writer_) : E_OUTOFMEMORY;
        } else {
            hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, wa.Get(), &writer_);
        }
    }
    if (FAILED(hr)) {
        err = tr(L"파일을 만들 수 없습니다. (") + Hr(hr) + L")";
        writer_.Reset();
        return false;
    }

    const wchar_t* stage = tr(L"영상 인코더");
    const wchar_t* call = L"AddStream(video)";  // for Tried
    ComPtr<IMFMediaType> vo, vi;
    hr = MFCreateMediaType(&vo);
    if (SUCCEEDED(hr)) {
        SetVideoBasics(vo.Get(), v_);
        vo->SetGUID(MF_MT_SUBTYPE, v_.hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
        vo->SetUINT32(MF_MT_AVG_BITRATE, v_.bitrate);
        vo->SetUINT32(MF_MT_MPEG2_PROFILE, v_.hevc ? eAVEncH265VProfile_Main_420_8 : eAVEncH264VProfile_High);
        hr = writer_->AddStream(vo.Get(), &videoStream_);
    }
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&vi);
    if (SUCCEEDED(hr)) {
        SetVideoBasics(vi.Get(), v_);
        vi->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        ComPtr<IMFAttributes> params;
        if (codecParams && SUCCEEDED(MFCreateAttributes(&params, 5))) {
            params->SetUINT32(CODECAPI_AVEncCommonRateControlMode,
                              v_.cbr ? eAVEncCommonRateControlMode_CBR : eAVEncCommonRateControlMode_UnconstrainedVBR);
            params->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, v_.bitrate);
            params->SetUINT32(CODECAPI_AVEncMPVGOPSize, (UINT32)std::max(1, v_.gopFrames));
            params->SetUINT32(CODECAPI_AVEncCommonQualityVsSpeed, (UINT32)std::clamp(v_.qualityVsSpeed, 0, 100));
            // Variable frame rate: frames in the order shown, so Finish can set how long each lasts (B-frames would
            // reorder them; Windows' software encoder uses them).
            if (v_.vfr) params->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        }
        call = L"SetInputMediaType(video)";
        hr = writer_->SetInputMediaType(videoStream_, vi.Get(), params.Get());
    }
    if (SUCCEEDED(hr) && a_.enabled) {
        stage = tr(L"AAC 인코더");
        ComPtr<IMFMediaType> ao, ai;
        hr = MFCreateMediaType(&ao);
        if (SUCCEEDED(hr)) {
            ao->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            ao->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            ao->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            ao->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, a_.sampleRate);
            ao->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            ao->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, a_.bitrate / 8);
            call = L"AddStream(audio)";
            hr = writer_->AddStream(ao.Get(), &audioStream_);
        }
        if (SUCCEEDED(hr)) hr = MFCreateMediaType(&ai);
        if (SUCCEEDED(hr)) {
            ai->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            ai->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            ai->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            ai->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, a_.sampleRate);
            ai->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            ai->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
            ai->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, a_.sampleRate * 4);
            call = L"SetInputMediaType(audio)";
            hr = writer_->SetInputMediaType(audioStream_, ai.Get(), nullptr);
        }
    }

    // Which encoder did the sink writer pick?
    encoderName_.clear();
    hardwareUsed_ = false;
    ComPtr<IMFSinkWriterEx> wx;
    if (SUCCEEDED(hr) && SUCCEEDED(writer_.As(&wx))) {
        for (DWORD i = 0;; ++i) {
            GUID cat{};
            ComPtr<IMFTransform> t;
            if (FAILED(wx->GetTransformForStream(videoStream_, i, &cat, &t))) break;
            if (cat != MFT_CATEGORY_VIDEO_ENCODER) continue;
            t.As(&codecApi_);
            ComPtr<IMFAttributes> ta;
            if (SUCCEEDED(t->GetAttributes(&ta)) && ta) {
                wchar_t* s = nullptr;
                UINT32 len = 0;
                if (SUCCEEDED(ta->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &s, &len))) {
                    encoderName_ = s;
                    CoTaskMemFree(s);
                }
                hardwareUsed_ = SUCCEEDED(ta->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &len));
            }
        }
    }
    if (encoderName_.empty()) encoderName_ = v_.hevc ? tr(L"Microsoft HEVC (소프트웨어)") : tr(L"Microsoft H.264 (소프트웨어)");
    if (SUCCEEDED(hr)) {
        call = L"BeginWriting";
        hr = writer_->BeginWriting();
    }
    if (FAILED(hr)) {
        tried_ += util::Format(L"%s%s%s: %s ", tried_.empty() ? L"" : L"; ", hardware ? L"hardware" : L"software",
                               codecParams ? L"+params" : L"", call) +
                  Hr(hr);
        // BeginWriting starts the file and every encoder at once: not the last stage that was set up.
        err = wcscmp(call, L"BeginWriting") == 0 ? tr(L"동영상 파일을 쓰기 시작할 수 없습니다. (") + Hr(hr) + L")"
                                                 : util::Format(tr(L"%s를 시작할 수 없습니다. ("), stage) + Hr(hr) + L")";
        writer_.Reset();
        codecApi_.Reset();
        if (!ring_) DeleteFileW(path.c_str());
        videoStream_ = audioStream_ = MAXDWORD;
        return false;
    }
    started_ = true;
    return true;
}

ComPtr<IMFSample> Mp4Writer::Convert(ID3D11Texture2D* bgra) {
    if (!writer_ || failed_) return nullptr;
    if (inTex_.Get() != bgra) {
        inView_.Reset();
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC id{};
        id.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        if (FAILED(vdev_->CreateVideoProcessorInputView(bgra, vpEnum_.Get(), &id, &inView_))) return nullptr;
        inTex_ = bgra;
    }
    ComPtr<IMFSample> sample;
    if (FAILED(allocator_->AllocateSample(&sample))) return nullptr;  // pool empty: the encoder is behind
    ComPtr<IMFMediaBuffer> buf;
    ComPtr<IMFDXGIBuffer> dxgi;
    ComPtr<ID3D11Texture2D> tex;
    UINT sub = 0;
    if (FAILED(sample->GetBufferByIndex(0, &buf)) || FAILED(buf.As(&dxgi)) ||
        FAILED(dxgi->GetResource(IID_PPV_ARGS(&tex))) || FAILED(dxgi->GetSubresourceIndex(&sub)))
        return nullptr;
    auto& view = outViews_[{tex.Get(), sub}];
    if (!view) {
        D3D11_TEXTURE2D_DESC td;
        tex->GetDesc(&td);
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od{};
        if (td.ArraySize > 1) {
            od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
            od.Texture2DArray = {0, sub, 1};
        } else {
            od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        }
        if (FAILED(vdev_->CreateVideoProcessorOutputView(tex.Get(), vpEnum_.Get(), &od, &view))) return nullptr;
        tex_.push_back(tex);  // keep pool textures alive as long as their cached views
    }
    D3D11_VIDEO_PROCESSOR_STREAM st{};
    st.Enable = TRUE;
    st.pInputSurface = inView_.Get();
    // Only the blit holds the device lock: the allocator and the encoder take the DXGI manager's lock first, so
    // holding ours across AllocateSample could deadlock against the encoder thread.
    if (mt_) mt_->Enter();
    HRESULT hr = vctx_->VideoProcessorBlt(vp_.Get(), view.Get(), 0, 1, &st);
    if (mt_) mt_->Leave();
    if (FAILED(hr)) return nullptr;
    DWORD maxLen = 0;
    if (SUCCEEDED(buf->GetMaxLength(&maxLen))) buf->SetCurrentLength(maxLen);
    return sample;
}

bool Mp4Writer::WriteVideo(IMFSample* sample, int64_t time, int64_t duration, bool key) {
    if (!writer_ || failed_) return false;
    if (!WriteAudioQueue()) return false;
    sample->SetSampleTime(time);
    sample->SetSampleDuration(duration);
    if (key && codecApi_) {
        // The request goes with the next frame the encoder takes in: first let it take every frame written before
        // (a moment while recording, where frames come a tick apart at least; an export waits for it here).
        MF_SINK_WRITER_STATISTICS st{sizeof(st)};
        for (int i = 0; i < 50 && SUCCEEDED(writer_->GetStatistics(videoStream_, &st)) &&
                        st.qwNumSamplesEncoded < st.qwNumSamplesReceived;
             ++i)
            Sleep(1);
        VARIANT on;
        VariantInit(&on);
        on.vt = VT_UI4;
        on.ulVal = 1;
        codecApi_->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &on);
    }
    if (FAILED(writer_->WriteSample(videoStream_, sample))) {
        failed_ = true;
        return false;
    }
    ++videoFrames_;
    videoEnd_ = time + duration;
    if (v_.vfr) frameDur_.push_back((uint32_t)std::min<int64_t>(duration, UINT32_MAX));
    return true;
}

bool Mp4Writer::KeyDue(int64_t time) const {
    return time - lastKey_ >= (int64_t)std::max(1, v_.gopFrames) * 10'000'000 / std::max(1, v_.fps);
}

bool Mp4Writer::HoldVideo(ComPtr<IMFSample> sample, int64_t time) {
    if (!EndVideo(time)) return false;
    held_ = std::move(sample);
    heldTime_ = time;
    heldKey_ = KeyDue(time);
    if (heldKey_) lastKey_ = time;
    return true;
}

bool Mp4Writer::EndVideo(int64_t end) {
    if (!held_) return true;
    ComPtr<IMFSample> s = std::move(held_);
    return WriteVideo(s.Get(), heldTime_, std::max<int64_t>(1, end - heldTime_), heldKey_);
}

void Mp4Writer::PushAudio(const int16_t* pcm, size_t frames) {
    std::lock_guard lk(audioMtx_);
    audioQueue_.insert(audioQueue_.end(), pcm, pcm + frames * 2);
}

bool Mp4Writer::WriteAudioQueue() {
    std::vector<int16_t> q;
    {
        std::lock_guard lk(audioMtx_);
        q.swap(audioQueue_);
    }
    if (!HasAudio() || q.empty()) return true;
    size_t frames = q.size() / 2;
    DWORD bytes = (DWORD)(frames * 4);
    ComPtr<IMFMediaBuffer> mb;
    ComPtr<IMFSample> s;
    BYTE* p = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(bytes, &mb);
    if (SUCCEEDED(hr)) hr = mb->Lock(&p, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        memcpy(p, q.data(), bytes);
        mb->Unlock();
        mb->SetCurrentLength(bytes);
        hr = MFCreateSample(&s);
    }
    if (SUCCEEDED(hr)) hr = s->AddBuffer(mb.Get());
    if (SUCCEEDED(hr)) {
        int64_t t0 = audioFrames_ * 10'000'000 / a_.sampleRate;
        int64_t t1 = (audioFrames_ + (int64_t)frames) * 10'000'000 / a_.sampleRate;
        s->SetSampleTime(t0);
        s->SetSampleDuration(t1 - t0);
        hr = writer_->WriteSample(audioStream_, s.Get());
    }
    if (FAILED(hr)) {
        failed_ = true;
        return false;
    }
    audioFrames_ += (int64_t)frames;
    return true;
}

bool Mp4Writer::Finish(std::wstring& err) {
    if (!writer_) {
        err = tr(L"녹화된 화면이 없습니다.");
        Close();
        return false;
    }
    // After a write error the samples written before it are still good: finalize the file with those.
    if (held_) EndVideo(heldTime_ + 10'000'000 / std::max(1, v_.fps));
    if (!failed_) WriteAudioQueue();
    HRESULT hr = videoFrames_ > 0 && !refuseFinish_ ? writer_->Finalize() : E_FAIL;
    if (ring_) {  // the last fragments went into the ring: there is no file to finish
        Close();
        return SUCCEEDED(hr);
    }
    // A fragmented file doesn't need the encoder to end it: what reached the disk plays, as after a crash. A lost
    // graphics device can't finish the file, and hours of recording must not go with it (#69).
    const bool kept = FAILED(hr) && v_.fragmented && videoFrames_ > 0;
    if (FAILED(hr) && !kept) {
        err = videoFrames_ == 0 ? tr(L"녹화된 화면이 없습니다.")
                                : tr(L"동영상 파일을 마무리하지 못했습니다. (") + Hr(hr) + L")";
        Abort();
        return false;
    }
    Close();
    bool plain = true;
    if (v_.fragmented) {
        std::wstring why;
        bool empty = false;
        plain = mp4::Unfragment(path_, why, &empty);
        if (kept && !plain && empty) {  // nothing had reached the disk
            err = tr(L"동영상 파일을 마무리하지 못했습니다. (") + Hr(hr) + L")";
            DeleteFileW(path_.c_str());
            Unmark();
            return false;
        }
        if (!plain)
            notice_ = tr(L"동영상을 일반 MP4로 바꾸지 못했습니다(") + why + tr(L"). 대부분의 플레이어에서는 그대로 재생됩니다.");
    }
    // Variable frame rate (#55): each frame as long as it was meant to be. Some encoders time such frames their own
    // way (Windows' software H.264 encoder shows each one late by its own length).
    std::wstring why;
    if (v_.vfr && plain) mp4::SetVideoTiming(path_, frameDur_, why);
    Unmark();
    return true;
}

void Mp4Writer::Abort() {
    bool had = writer_ != nullptr;
    Close();
    if (had && !path_.empty()) DeleteFileW(path_.c_str());
    Unmark();
}

void Mp4Writer::Close() {
    held_.Reset();
    writer_.Reset();
    codecApi_.Reset();
    outViews_.clear();
    tex_.clear();
    inView_.Reset();
    inTex_.Reset();
    if (allocator_) allocator_->UninitializeSampleAllocator();
    allocator_.Reset();
    devManager_.Reset();
    vp_.Reset();
    vpEnum_.Reset();
    vctx_.Reset();
    vdev_.Reset();
    mt_.Reset();
    ctx_.Reset();
    dev_.Reset();
}
