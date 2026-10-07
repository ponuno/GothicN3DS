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
	// `worldName` may be null: the archive says which world the game starts in.
	bool init(const char* pakPath, const char* worldName);
	void shutdown();

	void update(const Input& in, float dt);
	void render();

	// Sets draw distance, streaming radii, fog and far plane consistently.
	void setDrawDistance(float cm);
	// Moves the player's eyes to `eye` (keeps walking or flying as it was).
	void teleport(Vec3 eye);
	// Switches to another world of the archive. `eye` null uses its start
	// point. On failure the current world stays.
	bool enterWorld(u32 worldEntryHash, const Vec3* eye, float yaw);

	// Where the player is (world, position, view, time of day, draw distance),
	// so the next start continues from there. loadState returns false and
	// changes nothing if the file is missing, damaged or names a world that is
	// not in this archive.
	bool saveState(const char* path) const;
	bool loadState(const char* path);
	float drawDistance() const { return _world.config().drawDistance; }

	Camera& camera() { return _cam; }
	Environment& environment() { return _env; }
	Player& player() { return _player; }
	World& world() { return _world; }
	const World& world() const { return _world; }
	Pak& pak() { return _pak; }
	SkyColors skyColors() const { return skyColorsAt(_env.timeOfDay); }
	const char* error() const { return _error; }
	// Name of the current world asset ("world/oldmine"), for display.
	const char* worldName() const;
	// How many times the world was switched (portals and the debug key).
	u32 worldSwitches() const { return _switches; }
	// Seconds since init; drives texture animation. Settable for reproducible pictures.
	double clock() const { return _clock; }
	void setClock(double seconds) { _clock = seconds; }
	// True while the camera is below a water surface.
	bool underwater() const { return _underwater; }

private:
	void checkPortals();

	Pak _pak;
	World _world;
	Player _player;
	Sky _sky;
	Camera _cam;
	Environment _env;
	const char* _error = "";
	double _clock = 0;       // seconds since init: drives texture animation
	u32 _switches = 0;
	bool _portalLock = false; // just arrived: ignore portals until the player has left their boxes
	bool _underwater = false;
};

} // namespace g3
