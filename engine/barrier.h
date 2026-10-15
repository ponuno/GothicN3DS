// The Barrier over the colony, as Gothic 1 draws it (oCBarrier; fix150). Replaces the sky's own glow of fix12x.
//
//  * The dome is the game's mesh MAGICFRONTIER_OUT with its texture (g3ds/barrier.h, baked by g3ds-bake): fixed over the
//    valley in world coordinates, drawn twice in the sky pass, additively, with oCBarrier::RenderLayer's wobble, scroll
//    and per-corner flicker worked out on the CPU each frame. It lies further than the far plane, so every corner is put
//    on a sphere round the camera along its own direction: the picture is the same, nothing is clipped, and the world
//    drawn after it covers it where it stands in front.
//  * Lightning crawls over it from four sectors round the valley (Inside-Gothic: one every 8, 6, 14 and 4 s, and not
//    before the sector's last thunder has died away), from low on the dome up towards its top, with branches and a flash
//    where it starts; each bolt has its thunder, heard from its side.
//  * As in the game it is not always there: it shows for 25 s about every 20 minutes (15..25; the first time a minute or
//    so after the start), fading in and out. The hum plays while it is seen. The menu can show it always.
#pragma once
#include "g3ds/barrier.h"
#include "g3ds/math.h"
#include "gfx.h"

namespace g3 {

class Pak;
class Sound;

class Barrier {
public:
	enum Mode : u8 { OFF = 0, GAME = 1, ALWAYS = 2 };
	static constexpr float SHOW_SECONDS = 25.0f;
	static constexpr float FADE_IN = 0.5f, FADE_OUT = 1.0f;
	static constexpr u32 SECTORS = 4;
	static constexpr u32 MAX_BOLTS = 6;
	static constexpr u32 BOLT_POINTS = 18; // of the main stroke
	static constexpr u32 BRANCHES = 3;
	static constexpr u32 BRANCH_POINTS = 7;

	Barrier() = default;
	~Barrier() { release(); }
	Barrier(const Barrier&) = delete;
	Barrier& operator=(const Barrier&) = delete;

	// Reads "fx/barrier" and its texture once (later calls do nothing). False if the archive has no dome (baked before fix150).
	bool load(Pak& pak);
	bool hasMesh() const { return _vertexCount > 0; }
	// The time goes on: appearances, lightning, the hum and the thunder. `outdoor`: the sky is seen.
	void update(float dt, u8 mode, bool outdoor, Vec3 camera, Sound& sound, Pak& pak);
	// Between the sky and the world, in PASS_SKY (Sky::draw leaves it there). `radius`: the sky dome's.
	void draw(Vec3 camera, float radius);
	void release();

	// For the report: how much of it is seen (0..1), seconds to the next appearance (GAME mode), bolts alive.
	float level() const { return _level; }
	float nextIn() const { return _wait; }
	u32 bolts() const;
	u32 thunders() const { return _thunders; }
	const char* thunderName() const { return _thunderName; }
	const char* humName() const { return _humName; }
	bool ownTexture() const { return _texture && !_madeTexture; }
	u32 corners() const { return _vertexCount; }

	// Inside-Gothic: the sectors' least delays between two thunders (s).
	static constexpr float SECTOR_DELAY[SECTORS] = {8.0f, 6.0f, 14.0f, 4.0f};

private:
	struct Point {
		float p[3];
	};
	struct Bolt {
		bool alive = false;
		float age = 0, life = 0;
		float seed = 0;
		u8 branches = 0;
		Point main[BOLT_POINTS];
		Point branch[BRANCHES][BRANCH_POINTS];
		u8 branchFrom[BRANCHES] = {}; // the point of the main stroke it leaves from
	};
	struct Layer { // the precomputed phases of a corner (radians, wrapped)
		float pu, pv;
	};

	void spawnBolt(u32 sector, Sound& sound, Vec3 camera);
	void onSphere(const float q[3], float out[3]) const; // unit-sphere coordinates of the dome -> world
	void toSphere(const float p[3], float q[3]) const;
	void findSounds(Sound& sound, Pak& pak);
	void drawDome(Vec3 camera, float radius, bool fresh);
	void drawBolts(Vec3 camera, float radius, bool fresh);
	void drawBoltMesh();
	gfx::Texture* loadTexture();
	float rnd();

	bool _loaded = false; // load() was tried
	u32 _vertexCount = 0, _indexCount = 0;
	float* _pos = nullptr;    // x y z per corner, world cm
	float* _uv = nullptr;     // u v per corner
	float* _fade = nullptr;
	float* _flick = nullptr;  // phase of the corner's flicker: (x + z) wrapped
	Layer* _layers[2] = {};
	u16* _indices = nullptr;
	WorldVertex* _verts = nullptr; // one layer's vertices, rewritten for each
	float _uvScale = 1.0f / 1024.0f;
	float _bbMin[3] = {}, _bbMax[3] = {};
	float _centre[3] = {}, _radii[3] = {1, 1, 1};
	u32 _top = 0;
	u32 _textureHash = 0;
	Pak* _pak = nullptr;
	gfx::Texture* _texture = nullptr;
	bool _textureTried = false;
	bool _madeTexture = false; // none in the archive: a web made here, tinted by the vertices
	static constexpr u32 MESHES = 3;
	gfx::Mesh* _domeMeshes[2][MESHES] = {};
	u32 _turn = 0;
	u32 _frame = 0, _drawnFrame = ~0u; // update() calls, and the one last drawn (the right eye draws the left eye's meshes)
	u32 _domeBuilt = 0, _boltQuads = 0, _streakQuads = 0;
	// Where the lightning may start in each sector: low corners of the dome.
	static constexpr u32 STARTS = 24;
	u32 _starts[SECTORS][STARTS] = {};
	u32 _startCount[SECTORS] = {};

	// The clock.
	double _time = 0;        // seconds the Barrier's own animation has run
	float _wait = 60.0f;     // GAME: seconds until it shows next
	float _shown = -1.0f;    // seconds it has been shown, < 0: not
	float _level = 0;        // 0..1
	float _flash = 0;        // a strike brightens the dome for a moment
	float _sectorClock[SECTORS] = {};
	u32 _sectorSound[SECTORS] = {};
	Bolt _bolt[MAX_BOLTS];
	u32 _seed = 0x9E3779B9u;
	u32 _thunders = 0;

	// Sounds.
	bool _soundsSearched = false;
	const char* _humName = nullptr;
	const char* _thunderName = nullptr;
	u32 _hum = 0;
	char _names[512] = {};

	// The lightning's mesh and textures.
	static constexpr u32 BOLT_QUADS = MAX_BOLTS * ((BOLT_POINTS - 1) + BRANCHES * (BRANCH_POINTS - 1)) * 2 + MAX_BOLTS;
	gfx::Mesh* _boltMeshes[MESHES] = {};
	gfx::Texture* _streak = nullptr; // soft across, for the strokes
	gfx::Texture* _glow = nullptr;   // round, for where they start
};

} // namespace g3
