#pragma once

// Direct3D 11 + Direct2D renderer for the main window (flip-model swap chain, on-demand presents).
class Gfx {
public:
    bool Init();
    bool AttachWindow(HWND hwnd);
    void Resize();

    // Returns the device context with the back buffer bound and BeginDraw called. DIP scale = dpi / 96.
    ID2D1DeviceContext* BeginFrame(float dpi);
    void EndFrame();

    ID2D1Factory1* D2D() const { return d2dFactory_.Get(); }
    IDWriteFactory* DWrite() const { return dwrite_.Get(); }
    IWICImagingFactory* Wic() const { return wic_.Get(); }
    ID2D1DeviceContext* DC() const { return dc_.Get(); }
    // Bumped whenever the device is recreated; device-dependent resources must be rebuilt.
    uint32_t Generation() const { return generation_; }

    ComPtr<ID2D1Bitmap1> CreateBitmap(UINT w, UINT h, const void* bgra, UINT stride);
    ComPtr<ID2D1Bitmap1> CreateBitmapFromWic(IWICBitmapSource* src);

    // Offscreen render to PNG (used by --screenshot for visual checks).
    bool RenderToPng(const wchar_t* path, UINT w, UINT h, float dpi,
                     const std::function<void(ID2D1DeviceContext*)>& draw);
    // The same into memory: tightly packed premultiplied BGRA (--readme-media).
    bool RenderToPixels(UINT w, UINT h, float dpi, const std::function<void(ID2D1DeviceContext*)>& draw,
                        std::vector<uint8_t>& bgra);

private:
    bool CreateDevice();
    bool CreateSwapChain();
    bool CreateTarget();
    void HandleDeviceLost();

    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IWICImagingFactory> wic_;
    ComPtr<ID3D11Device> d3d_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> dc_;
    ComPtr<IDXGISwapChain1> swap_;
    ComPtr<ID2D1Bitmap1> target_;
    HWND hwnd_ = nullptr;
    UINT width_ = 0, height_ = 0;
    uint32_t generation_ = 0;
};
