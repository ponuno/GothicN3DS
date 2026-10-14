// Light shafts ("godrays"): slanted beams of light standing on the ground in front of the camera, along the sun's direction, where the
// sun reaches the ground between things that shade it (trees' trunks, walls, the hills). Nothing is traced through the picture: a fan of places
// round the way to the sun is tested with a few rays (World::rayDown for the ground, World::sightBlocked towards the sun, and to each side
// of the place, so that the beams stand at the edges of shade), and a soft additive quad is laid for each. Drawn in the blended pass.
#pragma once
#include "g3ds/math.h"
#include "gfx.h"

namespace g3 {

class World;

class GodRays {
public:
	static constexpr u32 MAX = 14;
	GodRays() = default;
	~GodRays() { release(); }
	GodRays(const GodRays&) = delete;
	GodRays& operator=(const GodRays&) = delete;

	// `sun`: unit vector to the sun; `strength` 0..1 (clear sky, sun not too high); `tint`: the colour of the light (0..255).
	void draw(const World& world, Vec3 camera, const float sun[3], float strength, const float tint[3], double seconds);
	void release();
	u32 drawn() const { return _drawn; }

	// How strongly a beam is seen where the sun is not blocked (lit) and `shadedSides` (0..2) of the places beside it are.
	static float beamLevel(bool lit, int shadedSides) { return lit ? 0.25f + 0.375f * float(shadedSides) : 0.0f; }

private:
	static constexpr u32 MESHES = 3;
	gfx::Texture* _texture = nullptr;
	gfx::Mesh* _meshes[MESHES] = {};
	u32 _turn = 0;
	u32 _drawn = 0;
};

} // namespace g3
