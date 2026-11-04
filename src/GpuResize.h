#pragma once

namespace caption {
class Overlay;
}

// The MP4 export's crop and scale on the GPU (#23), with what the CPU does after AreaResizer there: the caption laid
// over (Overlay::Blend) and an opened 움짤's see-through parts shown over white. The crop is uploaded and scaled like
// a recording's frames (a box filter over mips), or copied when the size stays (an odd edge trimmed by one pixel).
class GpuResizer {
public:
    // Into `out` (the w×h render target) from a cw×ch crop, with `cap` made for w×h. False when something can't be
    // made: the CPU does it then.
    bool Init(ID3D11Device* dev, ID3D11Texture2D* out, int cw, int ch, bool alpha, const caption::Overlay& cap);
    // Uploads the crop (its top-left pixel, rows `stride` bytes apart) and draws it into `out`. Under the device lock
    // when the encoder shares the device.
    void Draw(ID3D11DeviceContext* ctx, const uint8_t* crop, int stride);

private:
    ComPtr<ID3D11Texture2D> src_;
    ComPtr<ID3D11ShaderResourceView> srcSrv_, capSrv_;
    ComPtr<ID3D11RenderTargetView> outRtv_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> scalePs_, loadPs_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Buffer> frameCb_, capCb_;
    ComPtr<ID3D11BlendState> over_;
    int w_ = 0, h_ = 0, lod_ = 0, capTop_ = 0, capBottom_ = 0;
    bool copy_ = false;
};
