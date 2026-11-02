#include "pch.h"
#include "encoders/Encoder.h"
#include "Mp4Writer.h"

std::unique_ptr<IEncoder> CreateEncoder(Format f) {
    switch (f) {
        case Format::Gif: return CreateGifEncoder();
        case Format::WebP: return CreateWebpEncoder();
        case Format::Avif: return CreateAvifEncoder();
        default: return nullptr;
    }
}

bool FormatAvailable(Format f) {
    if (f == Format::Mp4) {
        static const bool mp4 =
            !Mp4Writer::FindEncoder(false, false).empty() || !Mp4Writer::FindEncoder(false, true).empty();
        return mp4;
    }
    return CreateEncoder(f) != nullptr;
}

const wchar_t* FormatName(Format f) {
    switch (f) {
        case Format::Gif: return L"GIF";
        case Format::WebP: return L"WebP";
        case Format::Avif: return L"AVIF";
        case Format::Mp4: return L"MP4";
        default: return L"";
    }
}

const wchar_t* FormatExt(Format f) {
    switch (f) {
        case Format::Gif: return L"gif";
        case Format::WebP: return L"webp";
        case Format::Avif: return L"avif";
        case Format::Mp4: return L"mp4";
        default: return L"";
    }
}

double FormatMaxFps(Format f) { return f == Format::Gif ? 50.0 : f == Format::Mp4 ? 120.0 : 60.0; }

bool WriteWholeFile(const std::wstring& path, const void* data, size_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    bool ok = true;
    while (size > 0 && ok) {
        DWORD chunk = (DWORD)std::min<size_t>(size, 64u << 20), written = 0;
        ok = WriteFile(h, p, chunk, &written, nullptr) && written == chunk;
        p += chunk;
        size -= chunk;
    }
    CloseHandle(h);
    return ok;
}
