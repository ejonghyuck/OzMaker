#include "pch.h"
#include "GpuResize.h"
#include "Caption.h"

#include "LoadPS.h"
#include "ScalePS.h"
#include "ScaleVS.h"

namespace {
// ScalePS's and LoadPS's constants.
struct Params {
    float fx, fy, lod, mode;  // mode: 0 opaque, 1 premultiplied alpha over white, 2 as it is (LoadPS: the caption)
};
}  // namespace

bool GpuResizer::Init(ID3D11Device* dev, ID3D11Texture2D* out, int cw, int ch, bool alpha,
                      const caption::Overlay& cap) {
    D3D11_TEXTURE2D_DESC od;
    out->GetDesc(&od);
    w_ = (int)od.Width;
    h_ = (int)od.Height;
    copy_ = cw - w_ >= 0 && cw - w_ <= 1 && ch - h_ >= 0 && ch - h_ <= 1;
    const float scale = std::max((float)cw / w_, (float)ch / h_);
    lod_ = !copy_ && scale > 1.f ? std::max(0, (int)std::lround(std::log2(scale)) - 2) : 0;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = copy_ ? w_ : cw;
    td.Height = copy_ ? h_ : ch;
    td.MipLevels = lod_ + 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (lod_ > 0 ? D3D11_BIND_RENDER_TARGET : 0);
    td.MiscFlags = lod_ > 0 ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    const Params frame{1.f / w_, 1.f / h_, (float)lod_, alpha ? 1.f : 0.f}, over{0.f, 0.f, 0.f, 2.f};
    D3D11_BUFFER_DESC bd{sizeof(Params), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA fd{&frame, 0, 0}, cd{&over, 0, 0};
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &src_)) ||
        FAILED(dev->CreateShaderResourceView(src_.Get(), nullptr, &srcSrv_)) ||
        FAILED(dev->CreateRenderTargetView(out, nullptr, &outRtv_)) ||
        FAILED(dev->CreateVertexShader(g_ScaleVS, sizeof(g_ScaleVS), nullptr, &vs_)) ||
        FAILED(dev->CreatePixelShader(g_ScalePS, sizeof(g_ScalePS), nullptr, &scalePs_)) ||
        FAILED(dev->CreatePixelShader(g_LoadPS, sizeof(g_LoadPS), nullptr, &loadPs_)) ||
        FAILED(dev->CreateSamplerState(&sd, &sampler_)) || FAILED(dev->CreateBuffer(&bd, &fd, &frameCb_)) ||
        FAILED(dev->CreateBuffer(&bd, &cd, &capCb_)))
        return false;

    over_.Reset();
    capSrv_.Reset();
    if (cap.Empty()) return true;
    // The caption, premultiplied, blended over the rows it covers.
    D3D11_TEXTURE2D_DESC ct = td;
    ct.Width = w_;
    ct.Height = h_;
    ct.MipLevels = 1;
    ct.Usage = D3D11_USAGE_IMMUTABLE;
    ct.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ct.MiscFlags = 0;
    D3D11_SUBRESOURCE_DATA px{cap.Pixels(), (UINT)w_ * 4, 0};
    D3D11_BLEND_DESC bl{};
    D3D11_RENDER_TARGET_BLEND_DESC& rt = bl.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11Texture2D> capTex;
    if (FAILED(dev->CreateTexture2D(&ct, &px, &capTex)) ||
        FAILED(dev->CreateShaderResourceView(capTex.Get(), nullptr, &capSrv_)) ||
        FAILED(dev->CreateBlendState(&bl, &over_)))
        return false;
    capTop_ = cap.Top();
    capBottom_ = cap.Bottom();
    return true;
}

void GpuResizer::Draw(ID3D11DeviceContext* ctx, const uint8_t* crop, int stride) {
    ctx->UpdateSubresource(src_.Get(), 0, nullptr, crop, stride, 0);
    if (lod_ > 0) ctx->GenerateMips(srcSrv_.Get());
    D3D11_VIEWPORT vp{0.f, 0.f, (float)w_, (float)h_, 0.f, 1.f};
    ctx->OMSetRenderTargets(1, outRtv_.GetAddressOf(), nullptr);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetShader(copy_ ? loadPs_.Get() : scalePs_.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, srcSrv_.GetAddressOf());
    ctx->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    ctx->PSSetConstantBuffers(0, 1, frameCb_.GetAddressOf());
    ctx->Draw(3, 0);
    if (over_) {
        // LoadPS reads the caption at the output pixel, so the viewport only narrows it to the band.
        D3D11_VIEWPORT band{0.f, (float)capTop_, (float)w_, (float)(capBottom_ - capTop_), 0.f, 1.f};
        ctx->RSSetViewports(1, &band);
        ctx->OMSetBlendState(over_.Get(), nullptr, 0xffffffff);
        ctx->PSSetShader(loadPs_.Get(), nullptr, 0);
        ctx->PSSetShaderResources(0, 1, capSrv_.GetAddressOf());
        ctx->PSSetConstantBuffers(0, 1, capCb_.GetAddressOf());
        ctx->Draw(3, 0);
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    }
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
}
