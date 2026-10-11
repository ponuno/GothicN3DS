// Time of day and the sky backdrop: a colour-graded dome around the camera
// whose horizon matches the fog, plus the tint applied to outdoor light.
#pragma once
#include "gfx.h"

namespace g3 {

struct SkyColors {
	u8 zenith[3];
	u8 horizon[3]; // also the fog colour, so distant terrain melts into the sky
	float light[3]; // multiplier for the baked light of outdoor vertices
};

// `hour` in 0..24.
SkyColors skyColorsAt(float hour);

class Sky {
public:
	~Sky() { release(); }
	// Draws the dome. Call between gfx::setPass(PASS_SKY) and PASS_WORLD.
	// `barrier` > 0 draws the colony's Barrier too: a dome of faint blue-violet
	// webs and flashes over the sky (its strength, 0..1), moving with `seconds`.
	void draw(Vec3 camera, float radius, const SkyColors& colors, float barrier = 0.0f, float seconds = 0.0f);
	void release();

private:
	gfx::Mesh* _mesh = nullptr;
	SkyColors _built {};
	float _builtBarrier = 0.0f;
	int _builtStep = -1;
	u32 _indexCount = 0;
};

} // namespace g3
