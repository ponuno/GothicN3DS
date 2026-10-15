// Ripples from whoever is in the water (fix141, richer in fix150): when the body steps in there is a splash (a broken ring of
// foam, two wide ring packets and a foam patch); while wading or swimming every step sends out a packet of concentric crests
// (alternately from the left and the right foot, so they interfere), a V-shaped wake (the Kelvin wake: two diverging arms
// with short curved waves between them) is laid behind the body and flecks of foam are left on the water and fade. Each lies
// on the water's waves like the rain's rings (rain.h) and is drawn the same way (a quad with a texture, in PASS_BLEND after the
// water); all the shapes are tiles of one small texture, so nothing else is needed from the renderer.
#pragma once
#include "g3ds/math.h"
#include "gfx.h"

namespace g3 {

class World;

class Ripples {
public:
	static constexpr u32 MAX_RINGS = 48;

	Ripples() = default;
	~Ripples() { release(); }
	Ripples(const Ripples&) = delete;
	Ripples& operator=(const Ripples&) = delete;

	// Once a frame. `feet`: where the body is; `waterDepth`: the water above the feet (0: dry); `swimming`; `dt`: seconds.
	void update(const World& world, Vec3 feet, float waterDepth, bool swimming, float dt);
	// In PASS_BLEND, after the water. `skyLight`: the light's colour (the shapes are paler in the dark).
	void draw(const World& world, Vec3 camera, const float skyLight[3]);
	// Forgets everything and frees the meshes and the texture (a world was left).
	void release();
	u32 rings() const { return _count; }

	// How far a ring of age `age` (0..1) has spread and how visible it is (0..1), for a ring of `size` (0..1).
	static float radius(float age, float size) { return (12.0f + (50.0f + 90.0f * size) * age) ; }
	static float opacity(float age, float strength) { return strength * (1.0f - age) * (1.0f - age); }

	enum Kind : u8 { RING, WAKE, FOAM, FOAM_RING };
	// Half width (cm) of a shape of `kind` at `age` (0..1) for `size` (0..1); and its visibility, with the quick fade-in.
	static float extent(Kind kind, float age, float size);
	static float visibility(Kind kind, float age, float strength);
	// The tile (0..3) of the texture each kind is cut from, and the texture itself (128x128 coverage, four 64x64 tiles).
	static u32 tileOf(Kind kind) { return u32(kind); }
	static void makeAtlas(u8* out128x128);

private:
	struct Ring {
		float x = 0, y = 0, z = 0, weight = 0;
		float age = 0;      // seconds
		float life = 1.6f;  // seconds
		float size = 0;     // 0..1
		float strength = 0; // 0..1
		float dirX = 0, dirZ = 1; // WAKE: the way the body went; the others: (cos, sin) of the turn of the shape
		Kind kind = RING;
	};
	void spawn(const World& world, Kind kind, float x, float z, float surfaceY, float size, float strength, float life, float dirX = 0.0f, float dirZ = 1.0f);
	float rnd(); // 0..1

	static constexpr u32 MESHES = 3;
	Ring _ring[MAX_RINGS] = {};
	u32 _count = 0;
	Vec3 _last {0, 0, 0};
	bool _haveLast = false;
	bool _wasIn = false;
	float _timer = 0.0f, _wakeTimer = 0.0f, _foamTimer = 0.0f;
	float _dirX = 0.0f, _dirZ = 1.0f; // the way the body goes (smoothed, unit)
	float _leg = 1.0f;                // +1 / -1: which foot sends the next ring
	u32 _seed = 12345u;
	gfx::Texture* _texture = nullptr;
	gfx::Mesh* _meshes[MESHES] = {};
	u32 _meshTurn = 0;
};

} // namespace g3
