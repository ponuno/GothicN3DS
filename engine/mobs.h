// The things of a world its people use (g3ds/game.h, "mobs"): where they
// are, who is at which right now.
#pragma once
#include "g3ds/game.h"
#include "g3ds/pak.h"

namespace g3 {

class Mobs {
public:
	// How far away a mob may be to be found, cm (as in the original).
	static constexpr float SEARCH_RANGE = 1000.0f;

	Mobs() = default;
	~Mobs() { release(); }
	Mobs(const Mobs&) = delete;
	Mobs& operator=(const Mobs&) = delete;

	// Loads "<worldAsset>/mobs". False if the world has none or it is damaged.
	bool load(Pak& pak, const char* worldAsset);
	void release();
	bool loaded() const { return _blob != nullptr; }

	u32 count() const { return _h.mobCount; }
	const WorldMob& mob(u32 i) const { return _mobs[i]; }
	const char* scheme(u32 i) const { return _names + _mobs[i].scheme; }
	const MobSlot& slot(u32 mob, u32 k) const { return _slots[_mobs[mob].firstSlot + k]; }
	bool isScheme(u32 mob, const char* scheme) const; // any letter case

	// The nearest mob of a scheme within SEARCH_RANGE of `pos` that has a
	// place to stand at and that nobody else uses (`owner`: whoever asks, any
	// number but 0xFFFF), or -1.
	// `except` (an index, or -1) is passed over.
	int find(const float pos[3], const char* scheme, u32 owner, int except = -1) const;
	// The same, all of them, the nearest first: up to `max` into `out`. Returns how many.
	u32 candidates(const float pos[3], const char* scheme, u32 owner, int except, int* out, u32 max) const;
	// Where somebody coming from `from` stands to use the mob: its slot
	// nearest to him. Returns the slot's number; -1, and the mob's own place,
	// for a mob without any (find() never returns such a one).
	int place(u32 mob, const float from[3], float pos[3], float& yaw) const;

	// One mob per owner, one owner per mob.
	bool hold(u32 mob, u32 owner);
	void drop(u32 owner);
	int heldBy(u32 owner) const;
	int userOf(u32 mob) const; // the owner, or -1
	void dropAll();

private:
	u8* _blob = nullptr;
	MobsHeader _h {};
	const WorldMob* _mobs = nullptr;
	const MobSlot* _slots = nullptr;
	const char* _names = nullptr;
	u16* _user = nullptr; // per mob: owner + 1, 0 for nobody
};

} // namespace g3
