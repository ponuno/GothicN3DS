// Rain and snow as things in the world: drops falling through the air round the camera, each a thin quad that stands
// in the world and is seen from every side (so it has depth, moves with the camera's parallax, and stops where it
// lands). A drop falls onto what is under it - the ground, a roof, the water (World::rayDown, World::waterBelow, asked
// once per cell of a grid and kept) and makes a splash there: a small ring on the ground, wider rings on the water, which lie on
// the water's waves (their height and their slope, as the shader lifts the surface). Under a roof there is no rain.
#pragma once
#include "g3ds/math.h"
#include "gfx.h"

namespace g3 {

class World;

class Rain {
public:
	static constexpr u32 MAX_DROPS = 380;
	static constexpr u32 MAX_RINGS = 120;
	static constexpr float RADIUS = 1700.0f; // cm from the camera
	static constexpr float CELL = 130.0f;    // cm: one drop (and its splash) to a cell

	Rain() = default;
	~Rain() { release(); }
	Rain(const Rain&) = delete;
	Rain& operator=(const Rain&) = delete;

	// In PASS_BLEND, after the water. `rain`, `snow`: 0..1 amounts; `sky`: the horizon colour (the drops' brightness follows the light).
	void draw(const World& world, Vec3 camera, float rain, float snow, const float skyLight[3], double seconds);
	// Forgets what was asked of the ground (a world was left) and frees the meshes and the texture.
	void release();
	u32 drops() const { return _drops; }
	u32 rings() const { return _rings; }

	// Whether cell (ix, iz) has a drop at `amount` (0..1), where in the world it falls, its phase (0..1) and speed factor (0.9..1.1).
	static bool cellDrop(int ix, int iz, float amount, float& x, float& z, float& phase, float& speed);

private:
	struct Cell {
		int ix = 0, iz = 0;
		u8 state = 0; // 0 not asked, 1 nothing under it, 2 lands
		u8 water = 0; // lands on water
		float retryAt = 0, x = 0, z = 0, y = 0, weight = 0;
		Vec3 normal {0, 1, 0};
	};
	static constexpr int GRID = 32;
	static constexpr u32 MESHES = 3;
	Cell _cells[GRID * GRID] = {};
	gfx::Texture* _texture = nullptr;
	gfx::Mesh* _meshes[MESHES] = {};
	u32 _meshTurn = 0;
	u32 _drops = 0, _rings = 0;
};

} // namespace g3
