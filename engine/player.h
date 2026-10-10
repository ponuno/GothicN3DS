// First-person character controller: walks on the world's collision mesh,
// slides along walls, climbs small steps and ladders, falls, jumps, wades
// through shallow water and swims on deep water. A fly mode without collision
// stays available for debugging and for the benchmark flights.
#pragma once
#include "world.h"

namespace g3 {

// What the player asks for. With a character (the game proper) the scheme is
// the original's: the character is steered, not the camera - forward and
// back along where it faces, left and right turn it, a step sideways has its
// own keys. Without one (and when flying) it is a free first-person camera.
struct Input {
	float moveX = 0, moveZ = 0; // character: turn right / forward; free camera: strafe / forward. -1..1
	float moveY = 0;            // fly mode only: down / up, -1..1
	float lookX = 0, lookY = 0; // turn right / look up, -1..1 (a character turns with lookX too)
	float strafe = 0;           // character only: step left (-1) or right (+1)
	bool fast = false;
	bool jump = false;      // held
	bool toggleWalk = false; // pressed this frame: character walks <-> runs
	bool toggleFly = false; // pressed this frame
	bool nextWorld = false; // pressed this frame: jump to the next world of the archive (debug)
	bool toggleCamera = false; // pressed this frame: third person <-> first person
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
	float wadeDepth = 60.0f;   // water above the feet from which walking slows down
	float wadeFactor = 0.65f;  // speed multiplier while wading
	float swimDepth = 125.0f;  // water deeper than this carries the player
	float swimSpeed = 260.0f;  // cm/s
	float floatSpeed = 220.0f; // cm/s the body rises to the surface with
	float climbSpeed = 170.0f; // cm/s up or down a ladder
	float driveGravity = 981.0f; // cm/s^2 for a driven body: the original's
};

// Movement decided elsewhere: by the animated character, which goes where
// its animation carries it. The controller then only resolves that against
// the world - walls, floors, water.
struct Drive {
	Vec3 step {0, 0, 0};     // displacement wanted this frame, world space (y only counts with `lift`)
	bool lift = false;       // the animation carries the body through the air (a jump's take-off): no gravity
	bool launch = false;     // leave the ground now, with `velocity`
	Vec3 velocity {0, 0, 0}; // cm/s
};

class Player {
public:
	PlayerConfig config;
	Vec3 feet {0, 0, 0};
	float velocityY = 0;
	bool grounded = false;
	bool flying = false;
	bool swimming = false;
	bool climbing = false; // on a ladder
	float waterDepth = 0; // water above the feet, 0 on dry land

	// Places the eyes at `eye`; the feet settle on the ground once it is loaded.
	void spawn(Vec3 eye);
	// `drive` null: the player's input moves the body directly.
	void update(const World& world, const Input& in, float yaw, float pitch, float dt, const Drive* drive = nullptr);
	Vec3 eye() const { return {feet.x, _eyeY, feet.z}; }
	// False until the ground under the spawn point has been found.
	bool placed() const { return _placed; }
	// What the controller is in the middle of, for diagnostics (g3ds-host --slope-probe).
	struct Debug {
		bool sliding, resting, blocked;
		Vec3 slideVel, carry;
	};
	Debug debug() const { return Debug {_sliding, _resting, _blocked, _slideVel, _carry}; }

private:
	void walk(const World& world, const Input& in, float yaw, float dt, const Drive* drive);
	void collideWalls(const World& world);
	bool swim(const World& world, float dt);
	bool climb(const World& world, const Input& in, float dt);

	float _eyeY = 0;
	bool _placed = false;
	bool _blocked = false; // the last horizontal move ran into something
	bool _letGo = false;   // jumped off a ladder: do not grab it again before landing
	Vec3 _carry {0, 0, 0}; // driven: the horizontal velocity the body keeps in the air
	// Sliding down a slope too steep to stand on: the lowest the feet have
	// got so far, and for how long they have got no lower. A slide that
	// goes nowhere (a crease between two slopes, a stone in the way) ends
	// with the body standing where it is ...
	float _slideLow = 0, _slideStill = 0;
	Vec3 _slideVel {0, 0, 0}; // cm/s, horizontal
	bool _sliding = false;
	// ... and it may stand there, and walk on at that height, on a floor too
	// steep for anywhere else.
	bool _resting = false;
	float _restY = 0;
	bool standsOn(const Vec3& normal, float y) const {
		return normal.y >= config.minFloorNormal || (_resting && y <= _restY + 10.0f);
	}
};

} // namespace g3
