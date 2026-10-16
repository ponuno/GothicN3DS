// Blood on the ground: where a blow lands, a few dark red patches are left under the character. They are kept (a few dozen,
// the oldest goes first), dry darker, and fade away after a minute or so. Drawn in the blended pass, like the puddles (same
// way of making quads on the ground: World::rayDown gives the ground under the place, once, when the stain is made).
#pragma once
#include "g3ds/math.h"
#include "gfx.h"

namespace g3 {

class World;

class BloodStains {
public:
	static constexpr u32 MAX = 32;
	static constexpr float RANGE = 3000.0f; // cm from the camera in which they are drawn
	static constexpr float LIFE = 70.0f;    // s until gone

	BloodStains() = default;
	~BloodStains() { release(); }
	BloodStains(const BloodStains&) = delete;
	BloodStains& operator=(const BloodStains&) = delete;

	// Some patches round `feet` (the place of the one hit), `amount` 0..1 by how hard the blow was. The ground is asked of `world`.
	void add(const World& world, Vec3 feet, float amount, double seconds);
	void draw(Vec3 camera, double seconds);
	void release();
	u32 count() const { return _count; }
	void clear() { _count = 0; }

private:
	struct Stain {
		float x, y, z, radius, angle;
		Vec3 normal;
		double born;
	};
	static constexpr u32 MESHES = 3;
	Stain _stains[MAX] = {};
	u32 _count = 0, _next = 0, _rnd = 12345;
	gfx::Texture* _texture = nullptr;
	gfx::Mesh* _meshes[MESHES] = {};
	u32 _meshTurn = 0;
	u32 rnd();
};

} // namespace g3
