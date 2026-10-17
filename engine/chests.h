// The containers of a world (g3ds/game.h, "world/<name>/chests"): chests and
// crates that hold things, can be locked, and that the player's character
// opens to take things out of and to put things into. What is inside is
// worked out from the world's table when the world is entered; what the
// player has changed (a chest opened, emptied, filled) is remembered per
// world and kept in state.bin.
#pragma once
#include "g3ds/game.h"
#include "g3ds/math.h"
#include "g3ds/pak.h"
#include "mobs.h"

#include <stdio.h>

namespace g3 {

class Game;

class Chests {
public:
	// A stack of things in a chest.
	struct Stack {
		u32 symbol;
		i32 count;
	};
	static constexpr u32 MAX_STACKS = 32;

	Chests() = default;
	~Chests() { release(); }
	Chests(const Chests&) = delete;
	Chests& operator=(const Chests&) = delete;

	// The containers of the world asset `worldName` ("world/world"); what the
	// player changed before is put back. Without the archive's table there are none.
	void enterWorld(Pak& pak, const char* worldName, u32 worldHash, Game& game);
	void leaveWorld();
	void release(); // also forgets what was changed

	u32 count() const { return _count; }
	u32 mob(u32 i) const { return _chests[i].mob; }
	bool locked(u32 i) const { return _chests[i].locked; }
	// The item that opens it (a script symbol), SCRIPT_NONE for none.
	u32 key(u32 i) const { return _chests[i].key; }
	// fix199: the container the world names so (hashName of its vob's name), -1 if none.
	int byName(u32 nameHash) const;
	// How many of `symbol` it holds.
	i32 countOf(u32 i, u32 symbol) const;
	// The combination of the lock ("LRRL"); "" if it cannot be picked.
	const char* pick(u32 i) const { return _chests[i].pick; }
	void unlock(u32 i);

	u32 stackCount(u32 i) const { return _chests[i].n; }
	const Stack& stack(u32 i, u32 k) const { return _chests[i].stacks[k]; }
	// Puts things in (joining a stack of the same); false if there is no room.
	bool add(u32 i, u32 symbol, i32 count);
	// Takes things out: how many were there (up to `count`).
	i32 remove(u32 i, u32 symbol, i32 count);
	// Increases with every change, for what is shown.
	u32 version() const { return _version; }

	// The nearest container within `reach` (cm) of `feet`, no more than
	// `halfAngle` off `yaw`; -1 if none.
	int nearest(const Mobs& mobs, Vec3 feet, float yaw, float reach, float halfAngle) const;

	// Forgets every change, in all worlds, and puts this world's containers back as they were made
	// (before a load: what the state does not name is as it started).
	void resetChanges(Pak& pak, const char* worldName, u32 worldHash, Game& game);
	// What was changed, all worlds: a block of state.bin.
	void saveChanged(FILE* f) const;
	bool loadChanged(FILE* f);
	// For the tests and the report.
	u32 changedCount() const { return _recordCount; }

private:
	struct Chest {
		u32 mob;
		u32 key;
		u32 name; // fix199: hashName of the vob's name, 0: none
		bool locked;
		bool changed;
		char pick[24];
		u8 n;
		Stack stacks[MAX_STACKS];
	};
	struct Record {
		u32 world, index;
		u32 flags; // 1: unlocked
		u32 n;
		Stack stacks[MAX_STACKS];
	};
	void flush() const; // this world's changes into the records
	void apply(); // the records into this world's containers
	Record* recordFor(u32 world, u32 index, bool create) const;

	Chest* _chests = nullptr;
	u32 _count = 0;
	u32 _world = 0;
	mutable Record* _records = nullptr;
	mutable u32 _recordCount = 0, _recordCap = 0;
	u32 _version = 0;
};

} // namespace g3
