// Light shafts ("godrays"): beams of sunlight standing in the air where the sun comes through a gap in what shades
// the ground - between the trees' crowns, past a roof's edge, through a cleft. Nothing is traced through the picture:
//  * The places a beam may stand are fixed in the world: a lattice of cells of CELL cm round the camera, a place in
//    each cell (hashed, so the same every time). A beam does not slide with the camera, it is passed by.
//  * What the sun does at a place is asked of the world a few cells a frame (World::rayDown for the ground,
//    World::sightBlocked towards the sun from the place and from four places round it) and kept until the sun has
//    moved: a beam stands where the sun reaches the ground and something beside it is in shade (the edge of a gap).
//  * Each beam fades in and out over a second or so as it is found, lost, comes into view or goes behind; it is
//    seen best looking towards the sun (light scatters forwards), faintly looking away, never right at the eye.
//  * All beams are one draw call: additive soft quads turned to the camera round their own axis, their brightness in
//    the vertex colour, a texture soft across and fading in at the ground and out towards the sun.
#pragma once
#include "g3ds/math.h"
#include "gfx.h"

namespace g3 {

class World;

class GodRays {
public:
	static constexpr u32 MAX = 22;         // beams drawn at most
	static constexpr float CELL = 520.0f;  // cm: one place for a beam to a cell
	static constexpr float NEAR = 500.0f;  // cm: closer beams fade away (they would fill the screen)
	static constexpr float FAR = 4200.0f;  // cm: no beam farther off (they fade out before)
	GodRays() = default;
	~GodRays() { release(); }
	GodRays(const GodRays&) = delete;
	GodRays& operator=(const GodRays&) = delete;

	// `sun`: unit vector to the sun; `look`: which way the camera looks (unit, horizontal enough); `strength` 0..1 (clear
	// sky, sun not too high); `tint`: the colour of the light (0..255); `seconds`: the clock (fades and shimmer).
	void draw(const World& world, Vec3 camera, const float look[3], const float sun[3], float strength, const float tint[3], double seconds);
	// Forgets what was asked of the world (a world was left) and frees the mesh and the texture.
	void release();
	u32 drawn() const { return _drawn; }

	// How strongly a beam is seen where the sun reaches the ground (lit) and `shadedSides` (0..4) of the places round it are in shade:
	// none in the open (a beam needs a gap to be seen), most at the edge of a gap, less in a gap shaded all round.
	static float beamLevel(bool lit, int shadedSides) {
		static const float k[5] = {0.0f, 0.55f, 1.0f, 0.85f, 0.6f};
		return lit && shadedSides >= 0 && shadedSides <= 4 ? k[shadedSides] : 0.0f;
	}
	// How much of a beam's light reaches an eye looking along `toBeam` (unit) with the sun along `sun` (unit): forward scattering.
	static float scatter(const float toBeam[3], const float sun[3]) {
		const float c = toBeam[0] * sun[0] + toBeam[1] * sun[1] + toBeam[2] * sun[2];
		const float f = c > 0.0f ? c : 0.0f;
		return 0.30f + 0.70f * f * f;
	}

private:
	struct Cell {
		int ix = 0, iz = 0;
		u8 state = 0;     // 0 not asked, 1 no beam here, 2 a beam may stand here
		u8 shaded = 0;    // places round it in shade (0..4)
		float x = 0, z = 0, ground = 0;
		float sunAt[3] = {0, 0, 0}; // the sun it was asked for
		float level = 0;  // shown now, follows the wanted level
		float askedAt = -100.0f;
	};
	static constexpr int GRID = 32;
	static constexpr u32 MESHES = 3;
	Cell _cells[GRID * GRID] = {};
	gfx::Texture* _texture = nullptr;
	gfx::Mesh* _meshes[MESHES] = {};
	u32 _turn = 0;
	u32 _drawn = 0;
	double _last = -1.0;
};

} // namespace g3
