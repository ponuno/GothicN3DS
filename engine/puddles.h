// Puddles in the rain: irregular dark patches on flat open ground that grow as the ground gets wet and shrink as it
// dries, with rings spreading in them while it rains. Nothing is baked: round the camera the ground is asked for
// (World::rayDown) in cells of a fixed grid, a cell has a puddle or not by a hash of its place, and what was asked
// is kept. Ground with a roof or a floor over it, and slopes, get none. Drawn in the blended pass, as the shadows are.
#pragma once
#include "g3ds/math.h"
#include "gfx.h"

namespace g3 {

class World;

class Puddles {
public:
	static constexpr u32 MAX = 32;
	static constexpr float RANGE = 2400.0f;

	Puddles() = default;
	~Puddles() { release(); }
	Puddles(const Puddles&) = delete;
	Puddles& operator=(const Puddles&) = delete;

	// `wet` 0..1 (how wet the ground is), `rain` 0..1 (rings in the puddles), `sky` the colour the puddles reflect,
	// `light` 0..1 how bright the outdoors is. In PASS_BLEND, before the shadows.
	void draw(const World& world, Vec3 camera, float wet, float rain, const u8 sky[3], double seconds);
	// Forgets the ground that was asked (a world was left) and frees the meshes and the texture.
	void release();
	u32 drawn() const { return _drawn; }

	// Where, and how big (cm), the puddle of cell (ix, iz) would be: false if that cell has none. Exposed for tests.
	static bool cellPuddle(int ix, int iz, float& x, float& z, float& radiusX, float& radiusZ, float& angle, float& threshold);
	static constexpr float CELL = 420.0f;

private:
	struct Cell {
		int ix = 0, iz = 0;
		u8 state = 0; // 0 not asked, 1 none, 2 puddle
		float retryAt = 0;
		float x = 0, y = 0, z = 0, rx = 0, rz = 0, angle = 0, threshold = 0;
		Vec3 normal {0, 1, 0};
	};
	static constexpr int GRID = 16;
	static constexpr u32 MESHES = 3;
	Cell _cells[GRID * GRID] = {};
	gfx::Texture* _texture = nullptr;
	gfx::Mesh* _meshes[MESHES] = {};
	u32 _meshTurn = 0;
	u32 _drawn = 0;
};

// Rain on water: rings spreading on the water's surface near the camera (World::waterBelow, kept per cell). Drawn in
// PASS_BLEND after the water, so that the water does not cover them.
class Ripples {
public:
	static constexpr u32 MAX = 80;
	static constexpr float RANGE = 2200.0f;
	static constexpr float CELL = 400.0f;
	Ripples() = default;
	~Ripples() { release(); }
	Ripples(const Ripples&) = delete;
	Ripples& operator=(const Ripples&) = delete;
	// `rain` 0..1; `lift`: cm above the water's level the rings are laid (waves lift the water: half their height).
	void draw(const World& world, Vec3 camera, float rain, const u8 sky[3], double seconds, float lift);
	void release();
	u32 drawn() const { return _drawn; }
	// Whether cell (ix, iz) has a ring, where in it (0..1) and its phase offset (0..1): the same for the same cell.
	static bool cellRing(int ix, int iz, int which, float& fx, float& fz, float& phase, float& period);

private:
	struct Cell {
		int ix = 0, iz = 0;
		u8 state = 0; // 0 not asked, 1 no water, 2 water
		float retryAt = 0, y = 0;
	};
	static constexpr int GRID = 16;
	static constexpr u32 MESHES = 3;
	Cell _cells[GRID * GRID] = {};
	gfx::Texture* _texture = nullptr;
	gfx::Mesh* _meshes[MESHES] = {};
	u32 _meshTurn = 0;
	u32 _drawn = 0;
};

} // namespace g3
