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

// "world/<name>/mobs": the things of the world its people use - beds,
// benches, ore to pick at ("mobs", as the game calls them).
//   MobsHeader | WorldMob[mobCount] | MobSlot[slotCount] | char names[nameBytes] (padded to 4)
// A mob is of a "scheme" (the first word of its model's name: "BEDHIGH",
// "BENCH"), which is what the scripts ask for (AI_UseMob) and what the
// animations of using it are named after ("T_BENCH_STAND_2_S0"). Its slots
// are the places one stands at to use it, and which way one faces there; a
// mob without any is in the table, but nobody can use it.
constexpr u32 MOBS_MAGIC = fourcc('G', '3', 'M', 'B');
constexpr u32 MOBS_VERSION = 1;

struct MobsHeader {
	u32 magic;
	u32 version;
	u32 mobCount;  // at most 65534
	u32 slotCount;
	u32 nameBytes;
	u32 reserved[3];
};
static_assert(sizeof(MobsHeader) == 32, "MobsHeader layout");

struct WorldMob {
	float pos[3];
	float yaw;
	u32 schemeHash; // hashName of the scheme (upper case)
	u32 scheme;     // offset into names
	u32 firstSlot;
	u8 slotCount;
	u8 states;      // how many states using it goes through after the first (S0): 1 for a bed (S0, S1)
	u16 reserved;
};
static_assert(sizeof(WorldMob) == 32, "WorldMob layout");

enum MobSlotKind : u8 {
	MOB_SLOT_PLAIN = 0,
	MOB_SLOT_FRONT = 1, // the animations used from here have "_FRONT" in their names ...
	MOB_SLOT_BACK = 2,  // ... or "_BACK"
	MOB_SLOT_DIST = 3,  // only a distance: one stands that far from the mob on one's own side of it, facing it
};

struct MobSlot {
	float pos[3];
	float yaw;
	u8 kind; // MobSlotKind
	u8 reserved[3];
};
static_assert(sizeof(MobSlot) == 20, "MobSlot layout");

// "world/<name>/items": the things lying about in a world - placed in the
// world file (oCItem) and by its start-up scripts (Wld_InsertItem), in that
// order (a thing's index is what state.bin keeps of it once taken).
//   ItemsHeader | WorldItem[count]
// Each is drawn with the model "model/item.<visual>", the visual the item's
// script gives (C_ITEM.VISUAL, lower case, without extension).
constexpr u32 ITEMS_MAGIC = fourcc('G', '3', 'I', 'T');
constexpr u32 ITEMS_VERSION = 1;

struct ItemsHeader {
	u32 magic;
	u32 version;
	u32 count;
	u32 fromScripts; // how many of them the start-up scripts put (the last ones)
};
static_assert(sizeof(ItemsHeader) == 16, "ItemsHeader layout");

enum WorldItemFlags : u32 {
	WORLD_ITEM_SCRIPT = 1 << 0, // put there by Wld_InsertItem
};

struct WorldItem {
	float pos[3];
	float rot[4];     // quaternion (x y z w): the item's model space -> the world
	u32 instance;     // hashName of the item's instance ("ITFOAPPLE"): the script symbol
	i32 amount;       // how many lie there (1 mostly)
	u32 flags;        // WorldItemFlags
};
static_assert(sizeof(WorldItem) == 40, "WorldItem layout");

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
