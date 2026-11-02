#include "pch.h"
#include "Gfx.h"

bool Gfx::Init() {
    D2D1_FACTORY_OPTIONS fo{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo,
                                 reinterpret_cast<void**>(d2dFactory_.GetAddressOf()))))
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return false;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_))))
        return false;
    return CreateDevice();
}

bool Gfx::CreateDevice() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, _countof(levels),
                                   D3D11_SDK_VERSION, &d3d_, nullptr, &ctx_);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, _countof(levels),
                               D3D11_SDK_VERSION, &d3d_, nullptr, &ctx_);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(d3d_.As(&dxgi))) return false;
    if (FAILED(d2dFactory_->CreateDevice(dxgi.Get(), &d2dDevice_))) return false;
    if (FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_))) return false;
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    ++generation_;
    return true;
}

bool Gfx::AttachWindow(HWND hwnd) {
    hwnd_ = hwnd;
    return CreateSwapChain();
}

bool Gfx::CreateSwapChain() {
    ComPtr<IDXGIDevice> dxgiDev;
    d3d_.As(&dxgiDev);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDev->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;

    RECT rc;
    GetClientRect(hwnd_, &rc);
    width_ = std::max<UINT>(1, rc.right - rc.left);
    height_ = std::max<UINT>(1, rc.bottom - rc.top);

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width_;
    sd.Height = height_;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_NONE;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (FAILED(factory->CreateSwapChainForHwnd(d3d_.Get(), hwnd_, &sd, nullptr, nullptr, &swap_))) return false;
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    return CreateTarget();
}

bool Gfx::CreateTarget() {
    ComPtr<IDXGISurface> surf;
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&surf)))) return false;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.f, 96.f);
    return SUCCEEDED(dc_->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &target_));
}

void Gfx::Resize() {
    if (!swap_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    UINT w = std::max<UINT>(1, rc.right - rc.left), h = std::max<UINT>(1, rc.bottom - rc.top);
    if (w == width_ && h == height_) return;
    width_ = w;
    height_ = h;
    dc_->SetTarget(nullptr);
    target_.Reset();
    HRESULT hr = swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        HandleDeviceLost();
        return;
    }
    CreateTarget();
}

ID2D1DeviceContext* Gfx::BeginFrame(float dpi) {
    if (!target_) return nullptr;
    dc_->SetTarget(target_.Get());
    dc_->SetDpi(dpi, dpi);
    dc_->BeginDraw();
    dc_->SetTransform(D2D1::Matrix3x2F::Identity());
    return dc_.Get();
}

void Gfx::EndFrame() {
    HRESULT hr = dc_->EndDraw();
    dc_->SetTarget(nullptr);
    if (hr == D2DERR_RECREATE_TARGET) {
        HandleDeviceLost();
        return;
    }
    hr = swap_->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) HandleDeviceLost();
}

void Gfx::HandleDeviceLost() {
    target_.Reset();
    swap_.Reset();
    dc_.Reset();
    d2dDevice_.Reset();
    ctx_.Reset();
    d3d_.Reset();
    if (CreateDevice() && hwnd_) CreateSwapChain();
}

ComPtr<ID2D1Bitmap1> Gfx::CreateBitmap(UINT w, UINT h, const void* bgra, UINT stride) {
    ComPtr<ID2D1Bitmap1> bmp;
    D2D1_BITMAP_PROPERTIES1 bp =
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    dc_->CreateBitmap(D2D1::SizeU(w, h), bgra, stride, &bp, &bmp);
    return bmp;
}

ComPtr<ID2D1Bitmap1> Gfx::CreateBitmapFromWic(IWICBitmapSource* src) {
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic_->CreateFormatConverter(&conv))) return nullptr;
    if (FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return nullptr;
    ComPtr<ID2D1Bitmap1> bmp;
    dc_->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &bmp);
    return bmp;
}

bool Gfx::RenderToPng(const wchar_t* path, UINT w, UINT h, float dpi,
                      const std::function<void(ID2D1DeviceContext*)>& draw) {
    D2D1_BITMAP_PROPERTIES1 tp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.f, 96.f);
    ComPtr<ID2D1Bitmap1> rt, cpu;
    if (FAILED(dc_->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, &tp, &rt))) return false;
    D2D1_BITMAP_PROPERTIES1 cp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    if (FAILED(dc_->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, &cp, &cpu))) return false;

    dc_->SetTarget(rt.Get());
    dc_->SetDpi(dpi, dpi);
    dc_->BeginDraw();
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    draw(dc_.Get());
    dc_->EndDraw();
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    dc_->SetTarget(nullptr);
    D2D1_POINT_2U origin{0, 0};
    D2D1_RECT_U all{0, 0, w, h};
    cpu->CopyFromBitmap(&origin, rt.Get(), &all);

    D2D1_MAPPED_RECT m;
    if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &m))) return false;
    ComPtr<IWICBitmap> wb;
    HRESULT hr = wic_->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppPBGRA, m.pitch, m.pitch * h, m.bits, &wb);
    cpu->Unmap();
    if (FAILED(hr)) return false;

    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(wic_->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE)) ||
        FAILED(wic_->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(enc->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)))
        return false;
    frame->SetSize(w, h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);
    if (FAILED(frame->WriteSource(wb.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(enc->Commit()))
        return false;
    return true;
}
