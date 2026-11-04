#pragma once

namespace mp4 {

// Recordings are written as fragmented MP4, so a crash leaves a file that plays (#24). This makes one a plain MP4 in
// place: a new moov at the end holds the sample tables of every fragment, and the fragment boxes become `free`. No
// sample data moves, so it takes a moment however long the recording is. A file a crash cut short keeps every sample
// that is whole on disk. A plain file stays as it is; a conversion that was stopped halfway is finished.
// `empty`: the file holds no samples at all.
bool Unfragment(const std::wstring& path, std::wstring& err, bool* empty = nullptr);

// Variable frame rate (#55): how long each video frame of a plain MP4 lasts, in 100 ns and the order written, so the
// file has the times OzMaker meant whatever the encoder made of them, and the video starts with the sound. A new moov
// takes the old one's place; no sample data moves. False, and the file as it was, when the frames don't match up or
// the encoder reordered them (B-frames).
bool SetVideoTiming(const std::wstring& path, const std::vector<uint32_t>& dur100ns, std::wstring& err);

}  // namespace mp4
