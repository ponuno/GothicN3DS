// The things lying about in a world (g3ds/game.h, "world/<name>/items"):
// placed by the world file and by the start-up scripts, drawn near the
// player with the model of their look, picked up by the player's character.
// What has been taken is remembered per world (and kept in state.bin), so a
// thing taken stays taken when the world is entered again.
#pragma once
#include "g3ds/game.h"
#include "g3ds/pak.h"
#include "model.h"
#include "point_grid.h"

#include <stdio.h>

namespace g3 {

class Game;
class World;

struct LootItem {
	u32 symbol;       // the item's instance, SCRIPT_NONE if the scripts lack it
	Vec3 pos;
	Quat rot;
	i32 amount;
	u32 flags;        // WorldItemFlags
	u16 model;        // into the model cache; NO_MODEL until loaded (or none to be had)
	bool taken;
	bool lit;         // `light` is the ground's
	u8 wait;          // frames its model's bytes have been waited for (fix89: read ahead, not on the frame's thread)
	float light[3];   // the baked light of the floor under it (before the time of day)
	float sky;        // how much of the sky that floor sees (the time of day's share)
};
static_assert(sizeof(LootItem) % sizeof(float) == 0, "LootItem: a whole number of floats (PointGrid's stride)");

struct LootStats {
	u32 count = 0;     // things of this world
	u32 taken = 0;     // of which picked up
	u32 drawn = 0;     // drawn in the last frame ...
	u32 drawCalls = 0, triangles = 0; // ... with this many draw calls and triangles
	u32 models = 0;    // looks in memory
	u32 noModel = 0;   // looks the archive has no model for
	float rayMs = 0, loadMs = 0; // the last update: floors looked for (rays), models built (fix90: for finding what a hitch is made of)
};

class Loot {
public:
	static constexpr u16 NO_MODEL = 0xFFFF, NOT_TRIED = 0xFFFE;
	// With a read-ahead (readahead.h) the models of things lying about are read on its thread; the frame builds them
	// once their bytes are there (and reads them itself only after a wait, if they never came).
	void setReadAhead(class ReadAhead* ahead) { _ahead = ahead; }
	Loot() = default;
	~Loot() { release(); }
	Loot(const Loot&) = delete;
	Loot& operator=(const Loot&) = delete;

	// The things of the world asset `worldName` ("world/world"); those taken
	// before stay away. Without the archive's table there are none.
	void enterWorld(Pak& pak, const char* worldName, u32 worldHash, Game& game);
	void leaveWorld();
	void release(); // also forgets what was taken

	// Reads one model a frame for the things near `eye` and takes the light
	// of their floor once it is there.
	void update(Pak& pak, const World& world, Game& game, Vec3 eye);
	// In PASS_WORLD: the things within `range` of `eye` - with `view`, only those in it, and with `minPixels` > 0
	// only those still that many pixels across (`focalPx`: pixels per radian of the picture; fix87).
	void draw(Vec3 eye, const float dayLight[3], float range = 3500.0f, const Frustum* view = nullptr, float minPixels = 0.0f,
	          float focalPx = 208.0f);

	u32 count() const { return _count; }
	const LootItem& item(u32 i) const { return _items[i]; }
	// The nearest thing not taken within `reach` (cm) of `feet`, no more than
	// `halfAngle` off `yaw`; -1 if none.
	// Draws the item `symbol` (a weapon in a hand or on the belt) with `attach`
	// as its place (model space of the character standing at `origin`); reads
	// its model the first time. In PASS_WORLD.
	void drawHeld(Pak& pak, Game& game, u32 symbol, const Mat34& attach, Vec3 origin, const float light[3]);
	// A character's melee weapon `melee` (item instance, SCRIPT_NONE for none):
	// in the hand while a weapon of the hands is drawn (`weapon`: a WeaponMode),
	// else on the belt (one hand) or the back (two).
	void drawCarried(Pak& pak, Game& game, const Model& host, const Pose& pose, float yaw, float lean, const float* scale,
	                 u8 weapon, u32 melee, Vec3 origin, const float light[3]);
	// A character's bow or crossbow `ranged`: in the left hand while one is drawn, else on the back.
	void drawRanged(Pak& pak, Game& game, const Model& host, const Pose& pose, float yaw, float lean, const float* scale,
	                u8 weapon, u32 ranged, Vec3 origin, const float light[3]);
	// A thing held in the right hand (an apple, a bottle: AI_UseItemToState).
	void drawInHand(Pak& pak, Game& game, const Model& host, const Pose& pose, float yaw, const float* scale, u32 symbol, Vec3 origin,
	                const float light[3]);
	int nearest(Vec3 feet, float yaw, float reach, float halfAngle) const;
	// Takes it away (picked up); false if it is gone already.
	bool take(u32 index);

	// What has been taken, all worlds: a block of state.bin.
	void saveTaken(FILE* f) const;
	bool loadTaken(FILE* f);
	const LootStats& stats() const { return _stats; }

private:
	class ReadAhead* _ahead = nullptr;
	struct Taken {
		u32 world;
		u32 index;
	};
	struct Look {
		u32 visual; // GameText of the visual
		Model model;
	};
	bool wasTaken(u32 world, u32 index) const;
	void markTaken(u32 world, u32 index);

	LootItem* _items = nullptr;
	u32 _count = 0;
	PointGrid _grid; // where they lie (fix87: only those near are looked at, not all of them every frame)
	u32 _world = 0; // pak hash of the world asset
	Taken* _taken = nullptr;
	u32 _takenCount = 0, _takenCap = 0;
	enum { LOOKS = 96 };
	Look _looks[LOOKS];
	u32 _lookCount = 0;
	TexturePool _pool;
	LootStats _stats;
};

} // namespace g3
