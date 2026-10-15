// fix148: the light of torches, fire places, lanterns and candles (g3ds/lamps.h, "world/<name>/lamps").
//
// Every frame the lamps near the camera are worked out - where, how far they reach, their colour with its flicker
// (ZenGin's range and colour keys; a fire place's light without keys flickers like a flame) - and then:
//  - the people, the beasts, the hero and the things lying about take the light of the lamps near them (lightAt: added
//    to the light they have from the ground under them; a lamp behind a wall does not reach them);
//  - the world takes it round them (pick: up to four lamps for a box of the world, which the world's draws hand to
//    gfx::setLamps; the console draws those with the lamps' vertex shader entry). A lamp whose light the baker put into
//    the vertices (g3ds-bake --lights) adds only its flicker there.
// A world baked before fix148 has no table: the fires among the particle emitters stand in for it (Particles::fireLights).
#pragma once
#include "g3ds/lamps.h"
#include "g3ds/math.h"
#include "g3ds/pak.h"
#include "gfx.h"

namespace g3 {

class Particles;
class World;

struct LampStats {
	u32 lamps = 0;      // in the world (or made from its fires)
	u32 active = 0;     // near the camera this frame
	u32 shining = 0;    // of those, shown (their place is drawn) and lighting something
	u32 boxes = 0;      // boxes of the world given lamps this frame (pick)
	u32 crowded = 0;    // ... of them, ones that had more than four (the weakest left out)
	u32 sightTests = 0; // this frame: whether a lamp sees a character (the rest came from the cache)
	bool table = false;    // the world's table (else: its fires stand in)
	bool baked = false;    // their light is in the vertices too
	bool world = false;    // this frame: the world is lit by them (the menu, the load, the backend)
};

class Lamps {
public:
	static constexpr u32 MAX_LAMPS = 4096;
	static constexpr u32 MAX_ACTIVE = 32;      // nearest to the camera
	static constexpr float REACH = 4500.0f;    // cm from the camera, plus the lamp's own range
	// How bright: the world round a lamp, and a character next to one (colour 255 = the texture at full brightness).
	static constexpr float WORLD_GAIN = 0.85f;
	static constexpr float CHARACTER_GAIN = 0.8f;

	Lamps() = default;
	~Lamps() { leaveWorld(); }
	Lamps(const Lamps&) = delete;
	Lamps& operator=(const Lamps&) = delete;

	// The lamps of world asset `worldName`; without its table, the fires of `fallback` (may be null: none).
	void enterWorld(Pak& pak, const char* worldName, Particles* fallback);
	void leaveWorld();
	bool available() const { return _count > 0; }

	struct Frame {
		Vec3 eye = {0, 0, 0};
		double seconds = 0.0;      // the game's clock: the flicker
		float dt = 0.0f;           // since the last update: lamps come in and go out with their torch (< 0: at once)
		float dayLight[3] = {1, 1, 1}; // the time of day's colour (as FrameDesc::dayLight)
		bool characters = true;    // the menu: lamps light the people (and the things)
		bool world = true;         // ... and the world (off under load, or where the backend has no lamps)
	};
	// Once a frame before drawing. `world` (may be null) says whether a lamp's place is drawn just now.
	void update(const Frame& f, const World* world);

	// Whether the world takes the lamps this frame.
	bool lightsWorld() const { return _stats.world && _activeCount > 0; }
	// The lamps reaching the box (world cm), as a bit set of this frame's active lamps; 0: none. For a first look at a chunk.
	u32 touching(const float bbMin[3], const float bbMax[3]) const;
	// The (at most four) strongest of the lamps in `candidates` (a bit set, as touching gives) reaching the box, packed one
	// active index a byte, in the order of the indices, 0xFF where none; 0xFFFFFFFF: none at all.
	u32 pick(const float bbMin[3], const float bbMax[3], u32 candidates);
	// The set for gfx::setLamps of a key pick gave.
	void setFor(u32 key, gfx::LampSet& out) const;

	// The light the lamps give a character (or a thing) at `p` (its middle), added to `out` (rgb, 1 = full). `sky`: how much of
	// the sky it sees (its ground's daylight weight 0..1). With `world`, a lamp behind a wall gives nothing.
	void lightAt(Vec3 p, float sky, float out[3], const World* world);

	const LampStats& stats() const { return _stats; }
	// For the tests: the active lamp `i` this frame (place, range, colour on the world).
	u32 activeCount() const { return _activeCount; }
	bool active(u32 i, Vec3& pos, float& range, float rgb[3]) const;

private:
	struct Active {
		Vec3 pos;
		float range;       // with its flicker
		float world[3];    // its colour on the world: the light, or (baked) the flicker's difference from the baked light
		float people[3];   // ... on the characters
		u32 lamp;          // index into _lamps
	};
	void sample(const WorldLamp& l, u32 index, double seconds, float& rangeScale, float rgb[3]) const;
	bool sees(u32 lamp, Vec3 lampPos, Vec3 p, const World* world);

	WorldLamp* _lamps = nullptr;
	float* _phase = nullptr; // a lamp's own start in its flicker, seconds
	int* _chunk = nullptr;   // World::pointShown's cache
	u8* _gate = nullptr;     // how much it shines, 0..255: it comes in and goes out with its torch
	u32 _count = 0;
	Active _active[MAX_ACTIVE];
	u32 _activeCount = 0;
	float _dayKeep = 1.0f;
	u32 _frame = 0;
	struct Seen {
		u32 key = 0;
		u32 frame = 0;
		bool blocked = false;
	};
	Seen _seen[256];
	LampStats _stats;
};

} // namespace g3
