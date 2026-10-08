// What the game's scripts need to know about a world: its named places.
//
// "world/<name>/points" holds every way point and every named marker of the
// world, which is where scripts put characters ("OCR_HUT_12", "SPAWN_...").
//   PointsHeader | WorldPoint[count]      (sorted by nameHash)
#pragma once
#include "g3ds/base.h"

namespace g3 {

constexpr u32 POINTS_MAGIC = fourcc('G', '3', 'P', 'T');
constexpr u32 POINTS_VERSION = 1;

struct PointsHeader {
	u32 magic;
	u32 version;
	u32 count;
	u32 reserved;
	// The world's name in the scripts, upper case: STARTUP_<script> and
	// INIT_<script> populate it ("WORLD", "OLDMINE").
	char script[32];
};
static_assert(sizeof(PointsHeader) == 48, "PointsHeader layout");

struct WorldPoint {
	u32 nameHash;  // hashName of the place's name
	float pos[3];  // on the ground
	float yaw;     // where somebody standing there faces
};
static_assert(sizeof(WorldPoint) == 20, "WorldPoint layout");

// "meta/looks": which baked models show a character of a given look. The key
// is Game::lookKey of the look (model script, body or armour, head and their
// texture variants), computed alike by the baker and by the game.
//   LooksHeader | LookEntry[count]        (sorted by key)
constexpr u32 LOOKS_MAGIC = fourcc('G', '3', 'L', 'K');
constexpr u32 LOOKS_VERSION = 1;

struct LooksHeader {
	u32 magic;
	u32 version;
	u32 count;
	u32 reserved;
};
static_assert(sizeof(LooksHeader) == 16, "LooksHeader layout");

// Pak entry hashes of models. The three share one skeleton: the rig has it
// and the animations but no mesh, body and head have a mesh and no animations.
struct LookEntry {
	u32 key;
	u32 rig;
	u32 body;
	u32 head; // 0: none (a creature)
};
static_assert(sizeof(LookEntry) == 16, "LookEntry layout");

} // namespace g3
