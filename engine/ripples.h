// Ripples from whoever is in the water (fix141): rings spread over the surface from the player's feet as they wade, swim or
// stand in it - a splash of two wide rings when they step in - and fade out. Each lies on the water's waves like the rain's
// rings (rain.h) and is drawn the same way (a quad with the ring texture, in PASS_BLEND after the water).
#pragma once
#include "g3ds/math.h"
#include "gfx.h"

namespace g3 {

class World;

class Ripples {
public:
	static constexpr u32 MAX_RINGS = 28;

	Ripples() = default;
	~Ripples() { release(); }
	Ripples(const Ripples&) = delete;
	Ripples& operator=(const Ripples&) = delete;

	// Once a frame. `feet`: where the body is; `waterDepth`: the water above the feet (0: dry); `swimming`; `dt`: seconds.
	// Makes the rings: one every so often while moving in the water (faster: more often, wider), a slow one when still.
	void update(const World& world, Vec3 feet, float waterDepth, bool swimming, float dt);
	// In PASS_BLEND, after the water. `skyLight`: the light's colour (the rings are paler in the dark).
	void draw(const World& world, Vec3 camera, const float skyLight[3]);
	// Forgets the rings and frees the meshes and the texture (a world was left).
	void release();
	u32 rings() const { return _count; }

	// How far a ring of age `age` (0..1) has spread and how visible it is (0..1), for a ring of `size` (0..1).
	static float radius(float age, float size) { return (12.0f + (50.0f + 90.0f * size) * age) ; }
	static float opacity(float age, float strength) { return strength * (1.0f - age) * (1.0f - age); }

private:
	struct Ring {
		float x = 0, y = 0, z = 0, weight = 0;
		float age = 0;      // seconds
		float life = 1.6f;  // seconds
		float size = 0;     // 0..1
		float strength = 0; // 0..1
	};
	void spawn(const World& world, float x, float z, float surfaceY, float size, float strength, float life);

	static constexpr u32 MESHES = 3;
	Ring _ring[MAX_RINGS] = {};
	u32 _count = 0;
	Vec3 _last {0, 0, 0};
	bool _haveLast = false;
	bool _wasIn = false;
	float _timer = 0.0f;
	gfx::Texture* _texture = nullptr;
	gfx::Mesh* _meshes[MESHES] = {};
	u32 _meshTurn = 0;
};

} // namespace g3
