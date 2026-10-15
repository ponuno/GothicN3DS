// Light shafts ("godrays"): beams of sunlight coming down through the crowns of trees. Nothing is traced through the
// picture:
//  * The beams belong to the trees: the crowns are the boxes of the swaying (leafy) batches of the loaded chunks
//    (World::plantBoxes). Under each crown a few beams stand, each from a place low in the crown along the sun's way down
//    to the ground - so they come out of the leaves, end on the ground, and do not reach into the open sky.
//  * A beam's places are fixed by the crown (hashed from where it stands): it does not slide with the camera.
//  * Whether the sun reaches the crown at all (a hill, a house in the way) is asked of the world a few beams a frame
//    (World::sightBlocked) and kept until the sun has moved; the ground under it likewise (World::rayDown).
//  * Each beam fades in and out over about half a second as it is found, lost, comes into view or goes behind; it is
//    seen best looking towards the sun (light scatters forwards), faintly looking away, never right at the eye.
//  * All beams are one draw call: additive soft quads turned to the camera round their own axis, their brightness in
//    the vertex colour, a texture soft across and fading at both ends.
#pragma once
#include "g3ds/math.h"
#include "gfx.h"

namespace g3 {

class World;

class GodRays {
public:
	static constexpr u32 MAX = 24;          // beams drawn at most
	static constexpr u32 CROWNS = 48;       // crowns looked at in a frame at most
	static constexpr float NEAR = 400.0f;   // cm: closer beams fade away (they would fill the screen)
	static constexpr float FAR = 3600.0f;   // cm: no beam farther off (they fade out before)
	static constexpr float MIN_CROWN = 250.0f; // cm: a leafy box at least this tall is a tree's crown (not a bush or grass)
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
	u32 crowns() const { return _crowns; }

	// How much of a beam's light reaches an eye looking along `toBeam` (unit) with the sun along `sun` (unit): forward scattering.
	static float scatter(const float toBeam[3], const float sun[3]) {
		const float c = toBeam[0] * sun[0] + toBeam[1] * sun[1] + toBeam[2] * sun[2];
		const float f = c > 0.0f ? c : 0.0f;
		return 0.30f + 0.70f * f * f;
	}

private:
	struct Beam {
		u32 id = 0;       // hash of the crown's place and the beam's number; 0: free
		u8 state = 0;     // 0 not asked, 1 no beam (the sun does not reach the crown, no ground), 2 a beam
		float top[3] = {0, 0, 0};    // where it leaves the leaves
		float ground[3] = {0, 0, 0}; // where it meets the ground
		float sunAt[3] = {0, 0, 0};  // the sun it was asked for
		float askedAt = -100.0f;
		float level = 0;  // shown now, follows the wanted level
		u32 seenFrame = 0;
	};
	static constexpr u32 SLOTS = 256;
	Beam _beams[SLOTS] = {};
	gfx::Texture* _texture = nullptr;
	gfx::Mesh* _meshes[3] = {};
	u32 _turn = 0;
	u32 _drawn = 0, _crowns = 0;
	u32 _frame = 0;
	double _last = -1.0;
};

} // namespace g3
