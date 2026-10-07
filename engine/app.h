// Platform-independent application layer: owns the pak, the world, the
// player and the sky. Platforms feed it input and call update()/render().
#pragma once
#include "g3ds/pak.h"
#include "player.h"
#include "sky.h"
#include "world.h"

namespace g3 {

struct Camera {
	Vec3 pos {0, 0, 0}; // follows the player's eyes
	float yaw = 0, pitch = 0;
	float fovY = 1.0472f; // 60 degrees
	float zNear = 20.0f;  // cm
	float zFar = 14000.0f;
};

struct Environment {
	float fogStart = 2500.0f; // cm
	float fogEnd = 12000.0f;  // cm; keep it at or below the draw distance
	float timeOfDay = 13.0f;  // hours, 0..24
	float dayLength = 0.0f;   // real seconds per game day; 0 freezes the clock
};

class App {
public:
	bool init(const char* pakPath, const char* worldName);
	void shutdown();

	void update(const Input& in, float dt);
	void render();

	// Sets draw distance, streaming radii, fog and far plane consistently.
	void setDrawDistance(float cm);
	// Moves the player's eyes to `eye` (keeps walking or flying as it was).
	void teleport(Vec3 eye);

	Camera& camera() { return _cam; }
	Environment& environment() { return _env; }
	Player& player() { return _player; }
	World& world() { return _world; }
	Pak& pak() { return _pak; }
	SkyColors skyColors() const { return skyColorsAt(_env.timeOfDay); }
	const char* error() const { return _error; }

private:
	Pak _pak;
	World _world;
	Player _player;
	Sky _sky;
	Camera _cam;
	Environment _env;
	const char* _error = "";
};

} // namespace g3
