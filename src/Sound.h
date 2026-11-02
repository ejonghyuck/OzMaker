#pragma once

// Short synthesized cues so recording state is audible inside fullscreen games.
namespace sound {
enum class Cue { Start, Stop, Saved, Tick };
void Play(Cue cue);
}  // namespace sound
