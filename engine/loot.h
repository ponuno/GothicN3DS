// The things lying about in a world (g3ds/game.h, "world/<name>/items"):
// placed by the world file and by the start-up scripts, drawn near the
// player with the model of their look, picked up by the player's character.
// What has been taken is remembered per world (and kept in state.bin), so a
// thing taken stays taken when the world is entered again.
#pragma once
#include "g3ds/game.h"
#include "g3ds/pak.h"
#include "model.h"

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
	float light[3];   // the baked light of the floor under it (before the time of day)
	float sky;        // how much of the sky that floor sees (the time of day's share)
};

struct LootStats {
	u32 count = 0;     // things of this world
	u32 taken = 0;     // of which picked up
	u32 drawn = 0;     // drawn in the last frame ...
	u32 drawCalls = 0, triangles = 0; // ... with this many draw calls and triangles
	u32 models = 0;    // looks in memory
	u32 noModel = 0;   // looks the archive has no model for
};

class Loot {
public:
	static constexpr u16 NO_MODEL = 0xFFFF, NOT_TRIED = 0xFFFE;
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
	// In PASS_WORLD: the things within `range` of `eye`.
	void draw(Vec3 eye, const float dayLight[3], float range = 3500.0f);

	u32 count() const { return _count; }
	const LootItem& item(u32 i) const { return _items[i]; }
	// The nearest thing not taken within `reach` (cm) of `feet`, no more than
	// `halfAngle` off `yaw`; -1 if none.
	int nearest(Vec3 feet, float yaw, float reach, float halfAngle) const;
	// Takes it away (picked up); false if it is gone already.
	bool take(u32 index);

	// What has been taken, all worlds: a block of state.bin.
	void saveTaken(FILE* f) const;
	bool loadTaken(FILE* f);
	const LootStats& stats() const { return _stats; }

private:
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
