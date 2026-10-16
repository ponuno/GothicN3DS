// The Barrier over the colony, as Gothic 1 draws it (oCBarrier; fix150, redone in fix184 after Inside-Gothic's "Magic
// Barrier", https://ataulien.github.io/Inside-Gothic/barrier/).
//
//  * The dome is the game's mesh MAGICFRONTIER_OUT with its texture (g3ds/barrier.h, baked by g3ds-bake): fixed in the
//    world, centred over the Old Camp (it does not follow the camera like a sky), drawn in the sky pass, additively,
//    with oCBarrier::RenderLayer's wobble, scroll and per-corner flicker worked out on the CPU each frame. It lies further
//    than the far plane, so every corner is put on a sphere round the camera along its own direction: the picture is the
//    same, nothing is clipped, and the world drawn after it covers it where it stands in front.
//  * When: every 20 real minutes, give or take 5 (15..25 min; three times an in-game day of 6000 s). Each time three
//    stages: fade in 0.25 s, 25 s seen, fade out 0.25 s. The first time 8 s after the start (oCBarrier's nextActivation).
//    The menu can show it always.
//  * Thunder (G12Dll's G1Barrier, a reconstruction of oCBarrier, and Inside-Gothic): while it is seen, each of four
//    sectors round the thunder centre (-5008, -5597 cm on XZ: 1 -x -z, 2 +x -z, 3 +x +z, 4 -x +z) strikes when its delay
//    (8, 6, 14, 2 s) is over, its last strike is gone and its last thunder has ended - so sector 4 really strikes every
//    ~4 s. The thunder is MFX_Barriere_Ambient, played at the camera (the menu's "Гром Барьера" can choose another). There
//    is no hum: that "ambient" sound is the thunder.
//  * A strike starts at a corner of the dome higher than 350 m (round its top) and runs down it (Inside-Gothic, Fig. 3):
//    a straight white bolt which at once breaks into a few jagged strands along the same way; they flicker, die back
//    from where they started and are gone in about a third of a second.
//  * The dome: one layer of the game's two (a plainer web), as strong as both at the game's opacity (120 of 255); each
//    corner's alpha as oCBarrier::Init gives it.
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
	// Its thunder (the menu's "Гром Барьера"). As in the game (default): MFX_Barriere_Ambient - oCBarrier plays it for every
	// strike, at the camera (G12Dll's reconstruction of oCBarrier::Render); it is the Barrier's thunder, not a hum. If the
	// game lacks it, the baker's far thunder. Or: the baker's near crack, its long roll, the game's lightning spell, none.
	enum Thunder : u8 { THUNDER_FAR = 0, THUNDER_NEAR = 1, THUNDER_ROLL = 2, THUNDER_MAGIC = 3, THUNDER_NONE = 4, THUNDER_CHOICES = 5 };
	// Inside-Gothic's numbers.
	static constexpr float SHOW_SECONDS = 25.0f;
	static constexpr float FADE_IN = 0.25f, FADE_OUT = 0.25f;
	static constexpr float PERIOD = 1200.0f, PERIOD_SPREAD = 300.0f; // every 20 min +- 5
	static constexpr float FIRST_WAIT = 8.0f;                        // oCBarrier: nextActivation starts at 8000 ms
	static constexpr u32 SECTORS = 4;                                // the quadrants
	static constexpr float THUNDER_SECONDS = 4.0f;
	// The dome: the game draws its texture twice (s = 1 and 1.5); one layer gives a plainer, less branchy web (and half the
	// CPU work), made brighter by DOME_SINGLE_GAIN. 2: as the game.
	static constexpr u32 DOME_LAYERS = 1;
	static constexpr float DOME_OPACITY = 120.0f / 255.0f; // oCBarrier's barrierMaxOpacity
	static constexpr float START_HEIGHT = 35000.0f;        // cm: the strikes start at corners above it (oCBarrier::Init)                   // the thunder sound, when it cannot be asked
	static constexpr float THUNDER_CENTRE_X = -5008.0f, THUNDER_CENTRE_Y = -10500.0f, THUNDER_CENTRE_Z = -5597.0f;
	// The look of a strike.
	static constexpr u32 MAX_BOLTS = 6;
	static constexpr u32 ROOT_POINTS = 6;   // the straight bolt (a little bent: it lies on the dome)
	static constexpr u32 STRANDS = 4;       // the jagged strands it breaks into
	static constexpr u32 STRAND_POINTS = 8;
	static constexpr u32 MAX_POINTS = STRAND_POINTS > ROOT_POINTS ? STRAND_POINTS : ROOT_POINTS;
	static constexpr float ROOT_GROW = 0.04f, ROOT_SHINE = 0.06f, ROOT_FADE_END = 0.2f; // s: up, at full, gone

	Barrier() = default;
	~Barrier() { release(); }
	Barrier(const Barrier&) = delete;
	Barrier& operator=(const Barrier&) = delete;

	// Reads "fx/barrier" and its texture once (later calls do nothing). False if the archive has no dome (baked before fix150).
	bool load(Pak& pak);
	bool hasMesh() const { return _vertexCount > 0; }
	// The time goes on: appearances, lightning, the hum and the thunder. `outdoor`: the sky is seen.
	void update(float dt, u8 mode, u8 thunder, bool outdoor, Vec3 camera, Sound& sound, Pak& pak);
	// Between the sky and the world, in PASS_SKY (Sky::draw leaves it there). `radius`: the sky dome's.
	void draw(Vec3 camera, float radius);
	void release();

	// For the report: how much of it is seen (0..1), seconds to the next appearance (GAME mode), bolts alive.
	float level() const { return _level; }
	float nextIn() const { return _wait; }
	u32 bolts() const;
	u32 thunders() const { return _thunders; }
	const char* thunderName() const { return _thunderName; }
	const char* humName() const { return nullptr; } // (the game's Barrier has no hum: MFX_Barriere_Ambient is its thunder)
	bool ownTexture() const { return _texture && !_madeTexture; }
	u32 corners() const { return _vertexCount; }

	// Inside-Gothic: the quadrants' activation delays (s). Q4's 2 s is shorter than its thunder: it waits for that.
	static constexpr float SECTOR_DELAY[SECTORS] = {8.0f, 6.0f, 14.0f, 2.0f};
	// Its quadrant (0..3 = sectors 1..4 of oCBarrier) of a point (world cm), round the thunder centre:
	// 1 -x -z, 2 +x -z, 3 +x +z, 4 -x +z.
	u32 quadrantOf(float x, float z) const;
	// For the report and the tests.
	const float* thunderCentre() const { return _thunderCentre; }
	u32 startsIn(u32 sector) const { return sector < SECTORS ? _startCount[sector] : 0; }
	u32 strikesIn(u32 sector) const { return sector < SECTORS ? _sectorStrikes[sector] : 0; }

private:
	struct Point {
		float p[3];
	};
	struct Strand {
		float start = 0, life = 0; // seconds after the strike it shows, and for how long
		float seed = 0;
		Point p[STRAND_POINTS];
	};
	struct Bolt {
		bool alive = false;
		u8 sector = 0;
		float age = 0, life = 0;
		u8 strands = 0;
		Point root[ROOT_POINTS];
		Strand strand[STRANDS];
	};
	struct Layer { // the precomputed phases of a corner (radians, wrapped)
		float pu, pv;
	};

	void spawnBolt(u32 sector, Sound& sound, Vec3 camera);
	void onSphere(const float q[3], float out[3]) const; // unit-sphere coordinates of the dome -> world
	void along(const float qs[3], const float qt[3], float omega, float f, float q[3]) const; // the great circle
	bool sectorBoltAlive(u32 sector) const;
	// oCBarrier::Init's alpha of a corner at height y (absolute, cm) of a mesh whose top is at maxY: rising over the lowest
	// 80 m above 0, falling to 0 above 92.5 % of maxY.
	static float gameFade(float y, float maxY) {
		const float topStart = maxY * 0.925f;
		float a = y > topStart && maxY > topStart ? (maxY - y) / (maxY - topStart) : y / 8000.0f;
		return a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
	}
	void toSphere(const float p[3], float q[3]) const;
	void findSounds(Sound& sound, Pak& pak);
	void chooseThunder(u8 choice, Sound& sound);
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
	u32 _domeBuilt = 0, _boltQuads = 0;
	float _thunderCentre[3] = {}; // round which the quadrants are told apart
	// Where the lightning may start in each quadrant: low corners of the dome.
	static constexpr u32 STARTS = 24;
	u32 _starts[SECTORS][STARTS] = {};
	u32 _startCount[SECTORS] = {};

	// The clock.
	double _time = 0;        // seconds the Barrier's own animation has run
	float _wait = FIRST_WAIT; // GAME: seconds until it shows next
	float _shown = -1.0f;    // seconds it has been shown, < 0: not
	float _level = 0;        // 0..1
	float _sectorClock[SECTORS] = {}; // seconds since the quadrant's last strike (or since the Barrier came)
	float _sectorBusy[SECTORS] = {};  // its last thunder when it has no sound to ask: seconds left
	u32 _sectorSound[SECTORS] = {};
	u32 _sectorStrikes[SECTORS] = {};
	Bolt _bolt[MAX_BOLTS];
	u32 _seed = 0x9E3779B9u;
	u32 _thunders = 0;

	// Sounds.
	bool _soundsSearched = false;
	const char* _thunderName = nullptr; // what plays (chooseThunder)
	const char* _gameThunder = nullptr; // the game's own (findSounds), "" if none
	const char* _ownThunder = nullptr;  // a sound of the game's made for the Barrier's lightning, if it has one
	u8 _thunderChoice = 0xFF;
	char _names[512] = {};

	// The lightning's mesh and textures.
	static constexpr u32 BOLT_QUADS = MAX_BOLTS * ((ROOT_POINTS - 1) + STRANDS * (STRAND_POINTS - 1)) * 2;
	gfx::Mesh* _boltMeshes[MESHES] = {};
	gfx::Texture* _streak = nullptr; // soft across, for the strokes
};

} // namespace g3
