// The new grass (Graphics+ -> Grass; data baked with g3ds-bake --grass, g3ds/world.h G3GR): tufts of two crossed cards
// grown round the camera from the compact list the bake left.
//  * The world's grass is cut into tiles of 16 m. The tiles within the radius (20 m "few", 28 m "many") are kept in a
//    small ring of slots: a tile coming into reach has its tufts read from the card (one tile a frame, a few KB) and is
//    made into one mesh on the main thread (one tile a frame, a few hundred tufts); one going out of reach is dropped.
//  * A tile is one draw call: a cut-out material of the world (alpha test, no blending, no sorting), both faces, the
//    atlas on texture unit 0 with texcoord 0 - nothing the console's GPU has not drawn before (sun_shadow_hang_handoff).
//  * The tops of the cards sway with the world's wind (MATF_WIND and the sway in the vertices' w); the feet stand still.
//  * The last 4 m of the radius the tufts shrink to nothing (worked out when the tile is made; a tile in that band is
//    made again as the camera moves on), so that the grass does not end in a hard edge.
//  * Round the feet of the hero and of the inhabitants and beasts nearest the camera the tufts are pressed aside and
//    down (the tiles under them are made again as they move, within the same budget).
//  * Light: each tuft has the ground's baked light in the colour of its texture, darker at the foot; the sun's baked
//    shadow (--sun-shadow) darkens a tuft by its shade as strongly as the ground is darkened just now.
#pragma once
#include "g3ds/math.h"
#include "g3ds/pak.h"
#include "g3ds/world.h"
#include "gfx.h"

namespace g3 {

class World;

struct GrassStats {
	u32 tiles = 0;      // drawn this frame
	u32 tufts = 0;      // ... their tufts
	u32 triangles = 0;  // ... their triangles
	u32 drawCalls = 0;
	u32 slots = 0;      // tiles held (made or waiting)
	u32 built = 0;      // tiles made this frame
	u32 builtTotal = 0; // since the world was entered
	u32 reads = 0;      // tiles read from the card since the world was entered
	u32 meshBytes = 0;  // the meshes held
	float radius = 0;   // cm, as drawn now
};

class Grass {
public:
	static constexpr u32 SLOTS = 40;
	static constexpr float FADE = 400.0f;        // cm: the band at the edge of the radius where tufts shrink away
	static constexpr float TRAMPLE = 100.0f;     // cm: how far round a man's feet tufts are pressed aside
	static constexpr u32 PRESSERS = 8;           // the hero and the inhabitants nearest the camera that press it
	static constexpr float POS_SCALE = 0.25f;    // cm per step of the meshes' positions (a power of two)
	static constexpr u32 BUILD_TUFTS = 700;      // tufts made into meshes in a frame (at least one tile)

	Grass() = default;
	~Grass() { release(); }
	Grass(const Grass&) = delete;
	Grass& operator=(const Grass&) = delete;

	// Reads the grass of a world ("world/<name>": its "world/<name>/grass"); false (and nothing held) if it has none.
	bool enterWorld(Pak& pak, const char* worldAsset);
	// Frees everything (a world is left, the program ends).
	void release();
	bool available() const { return _table != nullptr; }
	const GrassHeader& header() const { return _hdr; }

	struct Frame {
		Vec3 camera {0, 0, 0};
		float radius = 0.0f;   // cm (0: none drawn, the slots are kept)
		float share = 1.0f;    // of each tile's tufts drawn: 1 all, 0.5 the first half ("few")
		// Who presses the grass aside where they stand: the hero and the nearest inhabitants and beasts (their feet, and
		// how far round them, cm).
		struct Presser {
			Vec3 feet {0, 0, 0};
			float radius = TRAMPLE;
		};
		Presser press[PRESSERS];
		u32 pressCount = 0;
		float darkness = 0.0f; // how dark the sun's shadow is drawn now (gfx::setSunShadow)
		const class Clouds* clouds = nullptr; // fix186: the clouds' shadows of this frame (null: none drawn)
	};
	// Once a frame before drawing: which tiles are wanted, reading and making at most a little of them (from the archive
	// enterWorld read: it must stay open).
	void update(const Frame& f);
	// In PASS_WORLD after the world's own cut-outs (World::draw, which sets this frame's wind): the tiles in view.
	void draw(const World& world, const Frustum& view);
	const GrassStats& stats() const { return _stats; }

	// For the tests: the tufts of tile `t` as the runtime places them (world cm), whether made into a mesh now.
	u32 tileCount() const { return _hdr.tileCount; }
	const GrassTile& tile(u32 t) const { return _table[t]; }
	int findTile(int x, int z) const;
	// Where tuft `g` of tile `t` stands (its foot, world cm).
	void tuftFoot(const GrassTile& t, const GrassTuft& g, float out[3]) const;

private:
	struct Slot {
		int tile = -1;               // index into the table; -1: free
		GrassTuft* tufts = nullptr;  // read from the card (null: not yet)
		gfx::Mesh* mesh = nullptr;
		u32 drawn = 0;               // tufts in the mesh
		u32 bytes = 0;
		u32 usedFrame = 0;           // the last update() that wanted it
		bool wanted = false;
		float bbMin[3] = {0, 0, 0}, bbMax[3] = {0, 0, 0};
		float origin[3] = {0, 0, 0};
		// What it was made for.
		bool built = false;
		float camX = 0, camY = 0, camZ = 0, radius = 0, share = 0, darkness = 0;
		float cloudX = 0, cloudZ = 0, cloudDark = 0; // fix186: the clouds' drift and darkness it was made with
		u8 pressN = 0;               // who pressed it then (their feet on x/z)
		float pressX[PRESSERS] = {}, pressZ[PRESSERS] = {};
		float nearSq = 0, farSq = 0; // the tile's square on x/z: nearest and farthest from the camera then (squared)
	};
	bool readTufts(Slot& s);
	void build(Slot& s, const Frame& f);
	void drop(Slot& s);
	// How much making it again is worth now (0: it is as it should be).
	float staleness(const Slot& s, const Frame& f) const;
	// Which of the frame's pressers reach into the tile (their numbers into `out`); how many.
	u32 pressersIn(const Slot& s, const Frame& f, u8* out) const;

	GrassHeader _hdr {};
	GrassTile* _table = nullptr;
	u32 _dataEntry = 0;
	u32 _textureEntry = 0;
	gfx::Texture* _texture = nullptr;
	bool _textureTried = false;
	Pak* _pak = nullptr;
	Slot _slots[SLOTS];
	u32 _frame = 0;
	Vec3 _camera {0, 0, 0};
	GrassStats _stats;
	WorldVertex* _verts = nullptr; // scratch for making a mesh
	u32 _vertCap = 0;               // tufts it holds
	u16* _indices = nullptr;        // the same pattern for every mesh: 12 to a tuft
	u32 _indexCap = 0;
};

} // namespace g3
