// "Graphics+" G3: a soft round shadow on the ground under the player's
// character and the people and beasts near it. No light is traced: a dark
// disc lies on the ground where a character stands - enough for the eye to
// see that it stands there, and not floats.
//
// Each frame the characters drawn are handed in (add); draw() finds the
// ground under each (World::rayDown), lays a quad on it - tilted as the
// ground is, a little above it - and draws them in the blended pass, with a
// soft disc of a texture made here (no data). A character high above the
// ground (a jump) has a fainter, wider shadow; one far off fades out.
#pragma once
#include "g3ds/math.h"
#include "gfx.h"

namespace g3 {

class World;

struct ShadowStats {
	u32 asked = 0;     // characters handed in
	u32 drawn = 0;     // shadows laid on the ground
	u32 drawCalls = 0;
};

class Shadows {
public:
	static constexpr u32 MAX = 24;
	static constexpr float RANGE = 2500.0f; // cm from the camera: no shadow farther off (it fades out before)

	Shadows() = default;
	~Shadows() { release(); }
	Shadows(const Shadows&) = delete;
	Shadows& operator=(const Shadows&) = delete;

	// Forgets the characters of the last frame.
	void begin() {
		_count = 0;
		_stats = ShadowStats {};
	}
	// A character standing at `feet` (on the ground or above it), as wide as `radius` cm.
	void add(Vec3 feet, float radius);
	// In PASS_BLEND, before the water: the water then covers what is under it.
	// `strength`: 0..1, how dark the middle of a shadow is.
	void draw(const World& world, Vec3 camera, float strength);
	// The meshes and the texture (also when leaving: they are made again when needed).
	void release();

	const ShadowStats& stats() const { return _stats; }

private:
	struct Spot {
		Vec3 feet;
		float radius;
	};
	static constexpr u32 MESHES = 3; // in turn: the GPU may still be drawing the one before
	Spot _spots[MAX] = {};
	u32 _count = 0;
	gfx::Texture* _texture = nullptr;
	gfx::Mesh* _meshes[MESHES] = {};
	u32 _meshTurn = 0;
	ShadowStats _stats;
};

} // namespace g3
