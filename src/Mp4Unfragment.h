#pragma once

namespace mp4 {

// Recordings are written as fragmented MP4, so a crash leaves a file that plays (#24). This makes one a plain MP4 in
// place: a new moov at the end holds the sample tables of every fragment, and the fragment boxes become `free`. No
// sample data moves, so it takes a moment however long the recording is. A file a crash cut short keeps every sample
// that is whole on disk. A plain file stays as it is; a conversion that was stopped halfway is finished.
// `empty`: the file holds no samples at all.
bool Unfragment(const std::wstring& path, std::wstring& err, bool* empty = nullptr);

}  // namespace mp4
