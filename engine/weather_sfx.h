// Stand-in weather sounds made here, for a game whose sound files have none (Gothic 1 has no rain or thunder sample:
// the baker's guesses found only the spells MFX_FIRERAIN_* and MFX_THUNDERBALL_*). No data is read.
//  * rain: a seamless loop of falling water - broadband noise shaded like rain on leaves and ground, with
//    single drops ticking in it and a slow swell; it loops without a seam (the end is crossfaded into the start).
//  * thunder: a crack and a long rumble that rolls away (a one-shot), a little different each time it is made.
#pragma once
#include "g3ds/base.h"

namespace g3 {

constexpr u32 RAIN_SFX_RATE = 16000;
constexpr u32 RAIN_SFX_FRAMES = RAIN_SFX_RATE * 3; // 3 s loop, 96 KB
constexpr u32 THUNDER_SFX_RATE = 11025;
constexpr u32 THUNDER_SFX_FRAMES = THUNDER_SFX_RATE * 6; // 6 s, 132 KB

// `out` holds RAIN_SFX_FRAMES samples.
void makeRainSound(i16* out);
// `out` holds THUNDER_SFX_FRAMES samples; `seed` picks the shape of the rumble.
void makeThunderSound(i16* out, u32 seed);

// Whether a sound name the baker or the guesses offer for the weather is really a spell's or an effect's (MFX_..., SPELL_...)
// and must not be taken for the rain or the thunder.
bool weatherNameIsSpell(const char* name);

} // namespace g3
