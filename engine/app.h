// Platform-independent application layer: owns the pak, the world and a
// free-fly camera. Platforms feed it input and call update()/render().
#pragma once
#include "g3ds/pak.h"
#include "world.h"

namespace g3 {

struct Input {
	float moveX = 0, moveZ = 0; // strafe / forward, -1..1
	float moveY = 0;            // down / up, -1..1
	float lookX = 0, lookY = 0; // turn right / look up, -1..1
	bool fast = false;
};

struct Camera {
	Vec3 pos {0, 0, 0};
	float yaw = 0, pitch = 0;
	float fovY = 1.0472f; // 60 degrees
	float zNear = 20.0f;  // cm
	float zFar = 14000.0f;
};

struct Environment {
	float fogStart = 2500.0f; // cm
	float fogEnd = 12000.0f;  // cm; keep it at or below the draw distance
	u8 fogColor[3] = {150, 170, 190};
};

class App {
public:
	bool init(const char* pakPath, const char* worldName);
	void shutdown();

	void update(const Input& in, float dt);
	void render();

	// Sets draw distance, streaming radii, fog and far plane consistently.
	void setDrawDistance(float cm);

	Camera& camera() { return _cam; }
	Environment& environment() { return _env; }
	World& world() { return _world; }
	Pak& pak() { return _pak; }
	const char* error() const { return _error; }

private:
	Pak _pak;
	World _world;
	Camera _cam;
	Environment _env;
	const char* _error = "";
};

} // namespace g3
