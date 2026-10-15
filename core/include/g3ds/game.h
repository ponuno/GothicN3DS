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

// "world/<name>/mobinfo" (optional, one entry for each mob of "mobs", in its order): what the player's character
// may do with a thing of the world. Without the table (a pak baked earlier) nothing is for the player to use,
// the inhabitants still use all.
//   MobInfoHeader | WorldMobInfo[count] | char names[nameBytes] (the first byte is 0: offset 0 is "")
// A thing the world's maker gave no name to (oCMOB.focusName) is not shown to the player and is not for him: it
// is, as in the original, for the inhabitants (a secret lever, a bed in a hut that is only furniture).
constexpr u32 MOBINFO_MAGIC = fourcc('G', '3', 'M', 'I');
constexpr u32 MOBINFO_VERSION = 1;

struct MobInfoHeader {
	u32 magic;
	u32 version;
	u32 count;
	u32 nameBytes;
};
static_assert(sizeof(MobInfoHeader) == 16, "MobInfoHeader layout");

enum MobKind : u8 {
	MOBKIND_INTER = 0, // oCMobInter: a bench, an anvil, a cauldron ...
	MOBKIND_BED,
	MOBKIND_FIRE,
	MOBKIND_SWITCH,
	MOBKIND_WHEEL,     // the grindstone
	MOBKIND_CONTAINER, // chests: the chests' own page
	MOBKIND_DOOR,      // not yet: doors stay open
	MOBKIND_OTHER,
};

struct WorldMobInfo {
	u32 focus;     // the name shown (a script constant's name, "MOBNAME_BENCH", or the words themselves); 0: none
	u32 onState;   // the script function run when the thing is in use (oCMobInter.onStateFunc); 0: none
	u32 condition; // the script function that says whether it may be used; 0: none
	u32 useItem;   // the item instance the user must carry; 0: none
	u32 target;    // the name of what its use sets going (a mover, a trigger); 0: none
	u8 kind;       // MobKind
	u8 rewind;
	u16 reserved;
	// Whose it is (oCMobInter: the owner's script instance, the owner guild's name, "GIL_OCF"); 0: nobody's.
	// Older archives have 0 here.
	u32 owner, ownerGuild;
};
static_assert(sizeof(WorldMobInfo) == 32, "WorldMobInfo layout");

// "world/<name>/chests": the containers of a world (oCMobContainer: chests,
// crates), each one of the mobs of "mobs" (its place, its scheme CHESTBIG or
// CHESTSMALL, the places to stand at). What each holds at the start, and
// whether it is locked and with what.
//   ChestsHeader | WorldChest[count] | char names[nameBytes] (padded to 4)
constexpr u32 CHESTS_MAGIC = fourcc('G', '3', 'C', 'H');
constexpr u32 CHESTS_VERSION = 1;

struct ChestsHeader {
	u32 magic;
	u32 version;
	u32 count;
	u32 nameBytes;
};
static_assert(sizeof(ChestsHeader) == 16, "ChestsHeader layout");

enum WorldChestFlags : u32 {
	CHEST_LOCKED = 1 << 0,
};

struct WorldChest {
	u32 mob;      // index into the world's mobs
	u32 flags;    // WorldChestFlags
	u32 key;      // hashName of the item instance that opens it, 0 for none
	u32 pick;     // offset into names: the combination of the lock ("LRRL"), "" if it cannot be picked
	u32 contents; // offset into names: "ITMI_GOLD:75, ITFO_FISH:2, ITMI_QUARTZ"
	u32 reserved[3];
};
static_assert(sizeof(WorldChest) == 32, "WorldChest layout");

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
constexpr u32 LOOKS_VERSION = 2;

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
	u32 bodyLow; // 0: none; else a body of few triangles (no head) for characters far away (--npc-lod)
};
static_assert(sizeof(LookEntry) == 20, "LookEntry layout");

// "meta/forms" (fix151): what a transforming spell makes the player's character - the whole model of each creature
// the scripts name with Npc_SetActiveSpellInfo, baked like the hero's ("model/form.<instance>").
//   FormsHeader | FormEntry[count]
constexpr u32 FORMS_MAGIC = fourcc('G', '3', 'F', 'M');
constexpr u32 FORMS_VERSION = 1;

struct FormsHeader {
	u32 magic;
	u32 version;
	u32 count;
	u32 reserved;
};
static_assert(sizeof(FormsHeader) == 16, "FormsHeader layout");

struct FormEntry {
	u32 instance; // hashName of the creature's instance name ("WOLF"): ScriptSymbol::nameHash
	u32 model;    // pak entry of its model
};
static_assert(sizeof(FormEntry) == 8, "FormEntry layout");

} // namespace g3
