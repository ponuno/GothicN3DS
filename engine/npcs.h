// The inhabitants: the characters the game's scripts put into the worlds
// (game.h), shown where their daily routines have them.
//
// Only those near the player are shown, and only their models are in memory.
// A character is three models that share a skeleton: the "rig" has the
// skeleton and the animation it stands about in, body and head have a mesh
// each ("meta/looks" says which three a look needs). Everybody stands; that
// they go about their day is for later.
#pragma once
#include "game.h"
#include "model.h"
#include "world.h"

namespace g3 {

struct NpcConfig {
	float range = 3000.0f; // cm: how far away a character is still shown
	u32 maxShown = 12;     // at most this many, the nearest (up to 16)
	u32 loadsPerFrame = 1; // models read from the archive per frame (each is a hitch)
};

struct NpcStats {
	u32 total = 0;     // characters the scripts have made, in all worlds
	u32 here = 0;      // in this world, at a place it has
	u32 near = 0;      // within range
	u32 shown = 0;     // drawn in the last frame
	u32 culled = 0;    // near but out of view
	u32 waiting = 0;   // near, their models not read yet
	u32 noModel = 0;   // near, their look has no models in the archive
	u32 models = 0;    // models in memory
	u32 triangles = 0; // drawn in the last frame
	u32 drawCalls = 0;
	u32 loads = 0;     // models read since the start
	u32 textures = 0;  // textures in memory, and their size
	u32 textureBytes = 0;
};

class Npcs {
public:
	Npcs() = default;
	~Npcs() { release(); }
	Npcs(const Npcs&) = delete;
	Npcs& operator=(const Npcs&) = delete;

	// Loads the scripts and the table of looks. False if the archive has no
	// scripts: the worlds are then empty, as before.
	bool load(Pak& pak);
	void release();
	bool loaded() const { return _game.loaded(); }

	// The world `asset` ("world/oldmine") has been entered at this time of
	// day (hours): its start-up runs if it has not yet, everybody takes his
	// place, and the models of those around `eye` are read at once.
	void enterWorld(Pak& pak, const World& world, const char* asset, Vec3 eye, float hours);
	// The world is about to be closed: the models go (the next world wants
	// the memory before its people are known).
	void leaveWorld();
	// Once per frame: the clock, who is near, whose models are still to read.
	void update(Pak& pak, const World& world, Vec3 eye, float hours, float dt);
	// Call in PASS_WORLD. `dayLight` as for the world (1 1 1 indoors).
	void draw(const Mat4& viewProj, const float dayLight[3]);

	NpcConfig& config() { return _cfg; }
	const NpcStats& stats() const { return _stats; }
	Game& game() { return _game; }
	const Game& game() const { return _game; }
	// The world's name in the scripts ("WORLD"), "" before a world is entered.
	const char* worldScript() const { return _script; }

private:
	enum { PIECES = 48, SHOWN = 16 }; // three models each
	struct Piece {
		u32 hash = 0;
		u32 used = 0; // frame it was last needed in
		bool failed = false; // could not be read (out of memory, say): tried again after a while
		u32 failedAt = 0;
		Model model;
	};
	// What the game does not keep about a character: where it stands exactly.
	struct Standing {
		u32 moved = 0xFFFFFFFFu; // GameNpc::moved this was made for
		bool grounded = false;
		float y = 0;
		u8 light[4] = {255, 255, 255, 255};
	};
	struct Shown {
		u32 npc;
		float dist2;
		Piece* rig;
		Piece* body;
		Piece* head;
	};

	static bool findPoint(void* self, const char* name, float pos[3], float& yaw);
	const LookEntry* lookOf(const GameLook& look) const;
	Piece* piece(Pak& pak, u32 hash, u32& loadsLeft);
	void select(Pak& pak, const World& world, Vec3 eye, u32 loadsLeft);

	Game _game;
	NpcConfig _cfg;
	NpcStats _stats;
	LookEntry* _looks = nullptr;
	u32 _lookCount = 0;
	WorldPoint* _points = nullptr;
	u32 _pointCount = 0;
	char _script[32] = "";
	Standing* _standing = nullptr;
	u32 _standingCount = 0;
	TexturePool _pool;
	Piece _pieces[PIECES];
	Shown _shown[SHOWN];
	u32 _shownCount = 0;
	u32 _frame = 0;
	float _clock = 0;
	float _hours = -1.0f;
};

} // namespace g3
