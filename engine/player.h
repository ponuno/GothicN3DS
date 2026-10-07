// First-person character controller: walks on the world's collision mesh,
// slides along walls, climbs small steps, falls and jumps. A fly mode without
// collision stays available for debugging and for the benchmark flights.
#pragma once
#include "world.h"

namespace g3 {

struct Input {
	float moveX = 0, moveZ = 0; // strafe / forward, -1..1
	float moveY = 0;            // fly mode only: down / up, -1..1
	float lookX = 0, lookY = 0; // turn right / look up, -1..1
	bool fast = false;
	bool jump = false;      // held
	bool toggleFly = false; // pressed this frame
	float timeStep = 0;     // hours to add to the time of day this frame
};

struct PlayerConfig {
	float radius = 30.0f;      // cm
	float eyeHeight = 170.0f;  // cm above the feet
	float stepHeight = 45.0f;  // tallest ledge climbed without jumping
	float walkSpeed = 480.0f;  // cm/s
	float fastSpeed = 1100.0f; // cm/s (debug sprint)
	float flySpeed = 900.0f, flyFastSpeed = 4000.0f;
	float gravity = 1900.0f;   // cm/s^2
	float jumpSpeed = 560.0f;  // cm/s
	float minFloorNormal = 0.6f; // floors steeper than ~53 degrees are slides
};

class Player {
public:
	PlayerConfig config;
	Vec3 feet {0, 0, 0};
	float velocityY = 0;
	bool grounded = false;
	bool flying = false;

	// Places the eyes at `eye`; the feet settle on the ground once it is loaded.
	void spawn(Vec3 eye);
	void update(const World& world, const Input& in, float yaw, float pitch, float dt);
	Vec3 eye() const { return {feet.x, _eyeY, feet.z}; }
	// False until the ground under the spawn point has been found.
	bool placed() const { return _placed; }

private:
	void walk(const World& world, const Input& in, float yaw, float dt);
	void collideWalls(const World& world);

	float _eyeY = 0;
	bool _placed = false;
};

} // namespace g3
