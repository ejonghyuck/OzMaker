#pragma once

// The editor's caption (#22): one line of text over the whole clip, white with a dark outline, at the bottom or the
// top of the picture. The preview and the export draw it the same way, sized to the picture, so what the editor shows
// is what gets saved.
namespace caption {

enum Pos { kBottom = 0, kTop = 1 };

// Draws the caption over `frame` (the picture, in the target's units).
void Draw(ID2D1RenderTarget* rt, IDWriteFactory* dw, std::wstring_view text, int pos, const D2D1_RECT_F& frame);

// The caption over a w×h frame, made once and laid over every frame of an export.
class Overlay {
public:
    bool Make(std::wstring_view text, int pos, int w, int h);  // false (and nothing to lay over) on failure
    bool Empty() const { return px_.empty(); }
    void Blend(uint8_t* bgra) const;  // a w×h frame, rows w*4 bytes apart

private:
    int w_ = 0, h_ = 0, top_ = 0, bottom_ = 0;  // rows with something in them
    std::vector<uint8_t> px_;                   // premultiplied BGRA
};

}  // namespace caption
