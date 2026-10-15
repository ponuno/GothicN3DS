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

// What the sky shows beyond its colours: the time (the sun and the moon travel, the stars turn), the clouds (0 clear, 1 overcast,
// with fair-weather clouds of their own), the rain (darker clouds) and a flash of lightning (0..1).
struct SkyScene {
	float hour = 12.0f;
	float cloud = 0.0f;
	float rain = 0.0f;
	float flash = 0.0f;
};

class Sky {
public:
	~Sky() { release(); }
	void setScene(const SkyScene& s) { _scene = s; }
	// Unit vectors (x east, y up, z: the dome's) to the sun and to the moon at `hour`: the sun rises in the east at 6, is highest
	// at noon in the south and sets in the west at 18; the moon is opposite it. Below the horizon at night / by day.
	static void sunDirection(float hour, float out[3]);
	static void moonDirection(float hour, float out[3]);
	// Draws the dome. Call between gfx::setPass(PASS_SKY) and PASS_WORLD.
	// (The colony's Barrier is drawn after it: engine/barrier.h.)
	void draw(Vec3 camera, float radius, const SkyColors& colors, float seconds = 0.0f);
	void release();

private:
	void rebuild(const SkyColors& colors, float seconds);
	void drawBodies(Vec3 camera, float radius, const SkyColors& colors, float seconds);
	static constexpr u32 MESHES = 3;
	gfx::Mesh* _meshes[MESHES] = {};
	u32 _turn = 0;
	gfx::Mesh* _bodyMeshes[MESHES] = {};
	gfx::Texture* _dot = nullptr;  // a star
	gfx::Texture* _disc = nullptr; // the sun and the moon
	SkyScene _scene;
	SkyColors _built {};
	SkyScene _builtScene;
	float _builtSeconds = -100.0f;
	bool _hasBuilt = false;
	u32 _indexCount = 0;
};

} // namespace g3
