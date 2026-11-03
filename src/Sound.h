#pragma once

// Short synthesized cues so recording state is audible inside fullscreen games.
namespace sound {
enum class Cue { Start, Stop, Saved, Tick, Alarm };  // Alarm: the record timer's time is up
void Play(Cue cue);
}  // namespace sound
