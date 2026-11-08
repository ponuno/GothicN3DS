// The white trail a blade leaves in the air while it is swung (fix225).
//
// The original engine does not use a particle effect for this: it sweeps the weapon itself. Every frame of a blow the
// blade's two ends (the hilt and the point) are remembered; the surface between the places the blade has just passed
// through is the trail. This is that surface: a ribbon of quads, one per pair of samples, white and additive, fading
// with age. It follows the shape of the weapon (its longest side), so a sword leaves a long arc and a club a shorter,
// wider one. The hero and the inhabitants both use it.
#pragma once
#include "g3ds/math.h"
#include "gfx.h"

namespace g3 {

class BladeTrail {
public:
	static constexpr u32 HERO = 0xFFFFFFFEu; // the player's character (0xFFFFFFFF is a free slot)
	static constexpr u32 MAX_TRAILS = 9;   // the player's character and up to eight inhabitants at once
	static constexpr u32 SAMPLES = 16;     // per blade: ~0.18 s at 60 fps
	static constexpr float LIFE = 0.18f;   // seconds a sample is seen
	static constexpr float MIN_STEP = 0.8f; // cm the blade must move before another sample is kept

	BladeTrail() = default;
	~BladeTrail() { release(); }
	BladeTrail(const BladeTrail&) = delete;
	BladeTrail& operator=(const BladeTrail&) = delete;

	// `id`: the character (0xFFFFFFFF for the player's), `base` the hilt end, `tip` the point, in world cm. `on`:
	// the blade is being swung this frame; false lets the samples already taken fade out.
	void add(u32 id, Vec3 base, Vec3 tip, double now, bool on);
	// Draws the trails (PASS_BLEND; the caller has set it). `eye` is the camera.
	void draw(Vec3 eye, double now);
	void release();
	u32 trails() const { return _count; }
	u32 quads() const { return _quads; }

private:
	struct Sample {
		Vec3 base, tip;
		double time;
	};
	struct Trail {
		u32 id = 0xFFFFFFFFu;
		Sample s[SAMPLES] = {};
		u32 head = 0, count = 0;
		bool on = false;
		double last = 0.0;
	};
	Trail* find(u32 id);
	Trail* alloc(u32 id);

	Trail _trail[MAX_TRAILS] = {};
	u32 _count = 0;
	u32 _quads = 0;
	gfx::Texture* _texture = nullptr;
	gfx::Mesh* _mesh = nullptr;
};

} // namespace g3
