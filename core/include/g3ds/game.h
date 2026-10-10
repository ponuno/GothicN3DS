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

// "world/<name>/waynet": the paths the world's people walk and the spots
// they stand, sit and work at.
//   WayNetHeader | WayNode[nodeCount] | u16 edge[edgeCount] (padded to 4) |
//   FreePoint[freeCount] | char names[nameBytes] (padded to 4)
// A node's neighbours are edge[firstEdge .. firstEdge + edgeCount); every
// connection is there twice, once from each end. Free points ("FP_SIT_...",
// "FP_ROAM_...") are not part of the net: whoever needs one looks for the
// nearest whose name contains a word and walks straight to it.
constexpr u32 WAYNET_MAGIC = fourcc('G', '3', 'W', 'N');
constexpr u32 WAYNET_VERSION = 1;

struct WayNetHeader {
	u32 magic;
	u32 version;
	u32 nodeCount; // at most 65535
	u32 edgeCount;
	u32 freeCount;
	u32 nameBytes;
	u32 reserved[2];
};
static_assert(sizeof(WayNetHeader) == 32, "WayNetHeader layout");

struct WayNode {
	float pos[3]; // on the ground
	float yaw;    // where somebody standing there faces
	u32 nameHash;
	u32 name; // offset into names (upper case, zero-terminated)
	u32 firstEdge;
	u16 edgeCount;
	u16 reserved;
};
static_assert(sizeof(WayNode) == 32, "WayNode layout");

struct FreePoint {
	float pos[3];
	float yaw;
	u32 name;
};
static_assert(sizeof(FreePoint) == 20, "FreePoint layout");

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
