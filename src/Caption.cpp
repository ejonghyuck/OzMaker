#include "pch.h"
#include "Caption.h"
#include "Theme.h"

namespace caption {

void Draw(ID2D1RenderTarget* rt, IDWriteFactory* dw, std::wstring_view text, int pos, const D2D1_RECT_F& frame) {
    const float W = frame.right - frame.left, H = frame.bottom - frame.top;
    if (text.empty() || W < 8 || H < 8) return;
    // Sized by the shorter side, so a portrait video gets the same size of letters as a landscape one; shrunk to fit
    // when the line is too long for the width.
    const float base = std::min(W, H);
    ComPtr<IDWriteTextLayout> lay;
    DWRITE_TEXT_METRICS m{};
    auto layout = [&](float size) {
        ComPtr<IDWriteTextFormat> fmt;
        lay.Reset();
        if (FAILED(dw->CreateTextFormat(theme::UiFont, nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, size, L"ko-kr", &fmt)) ||
            FAILED(dw->CreateTextLayout(text.data(), (UINT32)text.size(), fmt.Get(), 100000.f, size * 2, &lay)))
            return false;
        lay->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        lay->GetMetrics(&m);
        return true;
    };
    float size = std::max(8.f, base * 0.075f);
    if (!layout(size)) return;
    const float maxW = W * 0.92f;
    if (m.width > maxW && !layout(size = std::max(6.f, size * maxW / m.width))) return;

    const float margin = base * 0.06f;
    const float x = frame.left + (W - m.width) / 2 - m.left;
    const float y = pos == kTop ? frame.top + margin : frame.bottom - margin - m.height;
    ComPtr<ID2D1SolidColorBrush> shadow, dark, white;
    if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.35f), &shadow)) ||
        FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1.f), &dark)) ||
        FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1.f), &white)))
        return;
    // The outline: the line stamped all around in black, then white on top. A soft shadow under it for light pictures.
    const float o = std::max(1.f, size * 0.07f);
    rt->DrawTextLayout({x, y + o * 1.8f}, lay.Get(), shadow.Get());
    for (int i = 0; i < 16; ++i) {
        float a = i * 3.14159265f / 8;
        rt->DrawTextLayout({x + o * std::cos(a), y + o * std::sin(a)}, lay.Get(), dark.Get());
    }
    rt->DrawTextLayout({x, y}, lay.Get(), white.Get());
}

bool Overlay::Make(std::wstring_view text, int pos, int w, int h) {
    px_.clear();
    w_ = w;
    h_ = h;
    if (text.empty() || w <= 0 || h <= 0) return false;
    // WIC needs COM on this thread (an export runs on its own).
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICBitmap> bmp;
        ComPtr<ID2D1Factory> d2d;
        ComPtr<IDWriteFactory> dw;
        ComPtr<ID2D1RenderTarget> rt;
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
            SUCCEEDED(wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)) &&
            SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf())) &&
            SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                          reinterpret_cast<IUnknown**>(dw.GetAddressOf()))) &&
            SUCCEEDED(d2d->CreateWicBitmapRenderTarget(bmp.Get(), props, &rt))) {
            rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);  // no ClearType on a see-through layer
            rt->BeginDraw();
            rt->Clear(D2D1::ColorF(0, 0, 0, 0));
            Draw(rt.Get(), dw.Get(), text, pos, D2D1::RectF(0, 0, (float)w, (float)h));
            if (SUCCEEDED(rt->EndDraw())) {
                px_.resize((size_t)w * h * 4);
                WICRect all{0, 0, w, h};
                if (FAILED(bmp->CopyPixels(&all, w * 4, (UINT)px_.size(), px_.data()))) px_.clear();
            }
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
    // Only the rows the line covers get blended.
    top_ = h;
    bottom_ = 0;
    for (int y = 0; y < h && !px_.empty(); ++y) {
        const uint8_t* row = &px_[(size_t)y * w * 4];
        for (int x = 0; x < w; ++x)
            if (row[x * 4 + 3]) {
                top_ = std::min(top_, y);
                bottom_ = y + 1;
                break;
            }
    }
    if (bottom_ <= top_) px_.clear();
    return !px_.empty();
}

void Overlay::Blend(uint8_t* bgra) const {
    if (px_.empty()) return;
    for (int y = top_; y < bottom_; ++y) {
        const uint8_t* s = &px_[(size_t)y * w_ * 4];
        uint8_t* d = bgra + (size_t)y * w_ * 4;
        for (int x = 0; x < w_; ++x, s += 4, d += 4) {
            const unsigned a = s[3];
            if (!a) continue;
            const unsigned k = 255 - a;
            d[0] = (uint8_t)(s[0] + (d[0] * k + 127) / 255);
            d[1] = (uint8_t)(s[1] + (d[1] * k + 127) / 255);
            d[2] = (uint8_t)(s[2] + (d[2] * k + 127) / 255);
            d[3] = (uint8_t)(a + (d[3] * k + 127) / 255);  // over a transparent part of an opened 움짤 too
        }
    }
}

}  // namespace caption
