// Rain and thunder recordings that come with the baker (tools/bake/weather_sfx/, built into g3ds-bake): Gothic 1 has no
// sound of its own for either. They go into data.pak as samples ("sfx/g3_rain", "sfx/g3_thunder_near", ...) and are
// listed first in meta/weathersfx, so the game takes them before anything of its own (--no-weather-sfx: not baked).
//   G3_RAIN          a 10 s loop of light rain (22050 Hz; its end crossfaded into its start)
//   G3_THUNDER_NEAR  a close crack and its rumble (16000 Hz, 15.5 s): heard when the flash is near (little delay)
//   G3_THUNDER_ROLL  a long distant roll (16000 Hz, 11.7 s)
//   G3_THUNDER_FAR   a distant boom (16000 Hz, 8 s)
// The recordings are from Mixkit (mixkit.co, the Mixkit Sound Effects Free License: free to use in a project, not to
// be redistributed as sound files on their own), cut, mixed to one channel and levelled for the game.
#pragma once
#include "pak_writer.h"

#include <string>

namespace g3 {

// Adds the samples to `pak` (at most `maxRate` Hz) and their lines to `meta` (meta/weathersfx: "L name" the rain's loop,
// "N name" near thunder, "F name" far thunder); returns a line for the report.
std::string bakeWeatherSfx(PakWriter& pak, u32 maxRate, std::string& meta);

} // namespace g3
