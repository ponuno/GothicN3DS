// The lamps of a world (fix148): torches, fire places, lanterns, candles - ZenGin's dynamic lights (zCVobLight that are
// not static, and the lights of the fire places' trees, FIRETREE_*.ZEN), with their flicker. The runtime lights the
// characters and the world round them with these (engine/lamps.h).
//
// Pak asset "world/<name>/lamps" (ASSET_META):
//   LampsHeader | WorldLamp[count]
//
// The static lights are not here: ZenGin compiled them into the world's own light (lightmaps and vertex light), which
// the baker keeps. The dynamic ones it did not; g3ds-bake --lights bakes them into the vertices (with shadows) - then
// the lamp carries LAMPF_BAKED and the runtime only adds its flicker to the world, not its light again.
#pragma once
#include "g3ds/base.h"

namespace g3 {

constexpr u32 LAMPS_MAGIC = fourcc('G', '3', 'L', 'M');
constexpr u32 LAMPS_VERSION = 1;
constexpr u32 LAMP_KEYS = 8; // animation keys kept (more are thinned out evenly)

struct LampsHeader {
	u32 magic;
	u32 version;
	u32 count;
	u32 reserved;
};
static_assert(sizeof(LampsHeader) == 16, "LampsHeader layout");

enum LampFlags : u8 {
	LAMPF_FIRE = 1 << 0,         // a fire place's (its tree file's) light: flickers like a flame even without keys
	LAMPF_BAKED = 1 << 1,        // its light is in the world's vertices (g3ds-bake --lights)
	LAMPF_RANGE_SMOOTH = 1 << 2, // the range keys are blended into each other (ZenGin rangeAniSmooth), else stepped
	LAMPF_COLOR_SMOOTH = 1 << 3, // the colour keys likewise (colorAniSmooth)
	LAMPF_MADE = 1 << 4,         // not in the game's data: made by the baker for a fire place whose tree has no light
};

struct WorldLamp {
	float pos[3];  // world cm
	float range;   // cm: where its light ends
	u8 color[3];   // rgb
	u8 flags;      // LampFlags
	u8 rangeKeys;  // 0..LAMP_KEYS: the range's animation (ZenGin rangeAniScale)
	u8 colorKeys;  // 0..LAMP_KEYS: the colour's animation (colorAniList)
	u16 reserved;
	float rangeFps; // keys a second
	float colorFps;
	u8 rangeScale[LAMP_KEYS];   // of the range, in 64ths (64 = 1.0)
	u8 colors[LAMP_KEYS][3];    // rgb
};
static_assert(sizeof(WorldLamp) == 64, "WorldLamp layout");

inline u32 lampsBlobSize(u32 count) { return u32(sizeof(LampsHeader)) + count * u32(sizeof(WorldLamp)); }

} // namespace g3
