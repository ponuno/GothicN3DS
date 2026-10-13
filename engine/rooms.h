// The rooms of a world (g3ds/world.h, "world/<name>/rooms"): the BSP sectors
// the scripts name - a hut, a house, a hall - and which of them a point is in.
// What the scripts do with them (whose house it is, who walked in) is the
// game's (Game::roomGuild, PERC_ASSESSENTERROOM).
#pragma once
#include "g3ds/math.h"
#include "g3ds/pak.h"
#include "g3ds/world.h"

namespace g3 {

class Rooms {
public:
	Rooms() = default;
	~Rooms() { release(); }
	Rooms(const Rooms&) = delete;
	Rooms& operator=(const Rooms&) = delete;

	// Loads "<worldAsset>/rooms"; a world without (or an older archive) has no rooms: false.
	bool load(Pak& pak, const char* worldAsset);
	// The same from a blob in memory (the tests); it is copied.
	bool adopt(const u8* data, u32 size);
	void release();

	u32 count() const { return _h.roomCount; }
	u32 boxCount() const { return _h.boxCount; }
	// The room the feet at `p` stand in: 1..count(), 0 for outside.
	u16 roomAt(Vec3 feet) const;
	// Its name (upper case, "" for 0) and hashName of it (0 for 0).
	const char* name(u16 room) const;
	u32 hash(u16 room) const { return room >= 1 && room <= _h.roomCount ? _hashes[room - 1] : 0u; }

private:
	bool build();
	u8* _blob = nullptr;
	RoomsHeader _h {};
	const RoomBox* _boxes = nullptr;
	const u32* _offsets = nullptr;
	const char* _names = nullptr;
	u32* _hashes = nullptr;
	// A grid over the ground plane: each cell lists the boxes that reach into it.
	float _x0 = 0, _z0 = 0, _cell = 1600.0f;
	u32 _gw = 0, _gh = 0;
	u32* _cellStart = nullptr; // _gw * _gh + 1
	u32* _cellItems = nullptr;
};

} // namespace g3
