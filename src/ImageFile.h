#pragma once
#include "Clip.h"

// Still images: a frame of the editor (#62) and what a screenshot saves (#63), in the format of 설정 › 일반.
namespace image {

enum Format { kPng = 0, kJpg, kWebp, kFormats };
const wchar_t* Ext(int format);
bool Available(int format);  // WebP needs libwebp in the build

// Tightly packed BGRA: premultiplied when alpha, otherwise the alpha byte means nothing.
struct Picture {
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    bool alpha = false;
};

// JPG and WebP at one high quality. JPG can't be see-through: what is shows over black. No file is left on failure.
bool Write(const std::wstring& path, int format, const Picture& pic, std::wstring& err);
// The frame as the editor shows it, at full size: cropped (crop in clip pixels) with the caption on (Caption.h).
bool TakeFrame(const Clip& clip, size_t index, const RECT& crop, const std::wstring& caption, int captionPos,
               Picture& pic, std::wstring& err);
// The picture as CF_DIB (opaque), for what takes an image from the clipboard and not a file.
std::vector<uint8_t> ToDib(const Picture& pic);

struct Saved {
    bool ok = false;
    std::wstring path, error;
    Picture pic;
    const Clip* clip = nullptr;  // the clip it came from, if any
};

// Runs saves on a worker thread, one at a time in the order they came, so the UI never waits for a decoder or a disk.
// Posts msg after each one. What is waiting at exit still gets written.
class Saver {
public:
    using Job = std::function<Saved()>;
    ~Saver();
    void Add(Job job, HWND notify, UINT msg);
    bool Busy() const;
    bool Take(Saved& out);  // the next finished one

private:
    void Run();

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::thread thread_;
    std::deque<Job> jobs_;
    std::deque<Saved> done_;
    int running_ = 0;
    bool stop_ = false;
    HWND notify_ = nullptr;
    UINT msg_ = 0;
};

}  // namespace image
