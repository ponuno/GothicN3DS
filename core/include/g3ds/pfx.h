// Particle effects.
//
// The effects themselves are the game's own scripts (C_PARTICLEFX instances: how many particles a
// second, where they start, how fast they fly, how long they live, what they look like); the
// engine reads them from the baked scripts. What the bake adds:
//   "tex/<name>"            the picture of every effect (ASSET_TEXTURE, as any texture)
//   "world/<name>/emitters" where a world has effects of its own (a fire, a smoke, a torch):
//   EmittersHeader | WorldEmitter[count]
#pragma once
#include "g3ds/base.h"

namespace g3 {

constexpr u32 EMITTERS_MAGIC = fourcc('G', '3', 'P', 'E');
constexpr u32 EMITTERS_VERSION = 1;

struct EmittersHeader {
	u32 magic;
	u32 version;
	u32 count;
	u32 reserved;
};
static_assert(sizeof(EmittersHeader) == 16, "EmittersHeader layout");

struct WorldEmitter {
	u32 effect; // hashName() of the script instance's name ("FIRE_SMALL")
	float pos[3];
	float yaw;  // the way the object that carries it faces
};
static_assert(sizeof(WorldEmitter) == 20, "WorldEmitter layout");

inline u32 emittersBlobSize(u32 count) { return u32(sizeof(EmittersHeader)) + count * u32(sizeof(WorldEmitter)); }

} // namespace g3
