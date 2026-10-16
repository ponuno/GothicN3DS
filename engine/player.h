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
	// The console scheme (App): the character turns towards this heading
	// by itself, quickly, instead of being turned by moveX and lookX.
	bool steer = false;
	float steerYaw = 0;
	bool toggleSneak = false; // pressed this frame: sneaking on and off
	// Fighting (the application's business; the controller ignores these).
	bool attack = false;      // pressed: a blow (with a weapon in hand)
	bool parry = false;       // held: fending blows off
	bool drawWeapon = false;  // pressed: the last weapon in hand, or away with it
	bool meleeSlot = false;   // pressed: the melee weapon (or the fists without one)
	bool fistSlot = false;    // pressed: the fists
	bool focus = false;       // pressed: lock on to the enemy in front, or let go
	int blowDir = 0;          // held: the D-Pad's direction of a blow with a weapon in hand: -1 from the left, +1 from the right, 0 ahead
	bool dpadUp = false;      // pressed: D-Pad up (without a weapon in hand: the fists)
	bool magic = false;       // pressed: the magic stance (the chosen spell in hand), or away with it
	bool attackHeld = false;  // held: A (a spell is charged while it is)
	int spellStep = 0;        // pressed: -1 / +1 the previous / next spell (D-Pad left / right in the magic stance)
	bool rangedSlot = false;  // pressed: the bow in hand / away
	bool cycleWeapon = false; // pressed: the next weapon or spell of the quick slots in hand (fix166; food and potions skipped)
	bool dodge = false;       // pressed: D-Pad down - with a weapon in hand a hop back (as the stick pulled back)
	float switchTarget = 0;   // -1 / +1 pressed: the next target to the left / right
	bool confirm = false;     // pressed: A (to get up again after death)
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
	float ledgeReach = 310.0f;   // highest ledge (above the feet) the hands of a jumping body take hold of
	float ledgeMin = 55.0f;      // lowest: below, a jump simply lands on it
	float ledgeRange = 90.0f;    // how far ahead of the body's edge the wall may be, cm
};

// Movement decided elsewhere: by the animated character, which goes where
// its animation carries it. The controller then only resolves that against
// the world - walls, floors, water.
struct Drive {
	Vec3 step {0, 0, 0};     // displacement wanted this frame, world space (y only counts with `lift`)
	bool lift = false;       // the animation carries the body through the air (a jump's take-off): no gravity
	bool launch = false;     // leave the ground now, with `velocity`
	Vec3 velocity {0, 0, 0}; // cm/s
	// The animation puts the body: a climb onto a ledge. The body goes to
	// `position` as it is (no walls, no floor, no gravity); `land` stands it
	// on the ground there (the last frame of the climb).
	bool place = false;
	bool land = false;
	Vec3 position {0, 0, 0};
};

// A ledge within the reach of a jumping body: where its hands take hold,
// where it stands once up.
struct Ledge {
	bool found = false;
	Vec3 top {0, 0, 0};   // the feet's place on top of it
	float height = 0;     // of the top above the feet now, cm
	float gap = 0;        // from the feet to the wall's face, cm (the body is brought to it)
	float yaw = 0;        // the way to the wall (facing it)
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
	// Looks ahead (along `yaw`) for a ledge to take hold of: a wall that
	// rises above the feet by between ledgeMin and ledgeReach and has floor
	// on top of it with room to stand. For a body in a jump.
	// `reach`: how far beyond the body's edge the wall may be (negative: ledgeRange). On the ground the original
	// climbs only what stands within 55 cm of the body's middle (MoveAlgo::climbMove).
	bool probeLedge(const World& world, float yaw, Ledge& out, float reach = -1.0f) const;
	// What the probes found (the console's report): how many were made and how
	// many found a ledge, why the last found none (1 nothing ahead, 2 a slope
	// or already over it, 3 no floor on top, 4 no room to stand, 5 too high,
	// 6 too steep to stand on) and the height of the last candidate.
	struct LedgeStats {
		u32 probes = 0, found = 0;
		u8 why = 0;
		float height = 0;
	};
	const LedgeStats& ledgeStats() const { return _ledgeStats; }
	// ---- ladders ----
	// On a ladder (`climbing`) the body is the ladder's: it is put on the ladder (a glide to the
	// rungs, facing them - LADDER_MOUNT from the foot, LADDER_MOUNT_TOP over the edge from the top),
	// goes up and down it with the stick (LADDER_CLIMB), and off it at the top (LADDER_EXIT_TOP: up
	// over the edge onto the floor there) or at the foot (LADDER_EXIT_FOOT). The phases last as long as
	// the character's animations of them do (set below); a jump lets go of the ladder.
	enum LadderPhase : u8 { LADDER_OFF, LADDER_MOUNT, LADDER_MOUNT_TOP, LADDER_CLIMB, LADDER_EXIT_TOP, LADDER_EXIT_FOOT };
	u8 ladderPhase() const { return climbing ? _lad.phase : u8(LADDER_OFF); }
	// How far through a mount or an exit, 0..1 (1 while climbing).
	float ladderProgress() const { return _lad.length > 0.0f ? clamp(_lad.time / _lad.length, 0.0f, 1.0f) : 1.0f; }
	// The way the character faces on the ladder (towards its rungs), turning to it while it is put on.
	float ladderYaw() const;
	int ladderIndex() const { return climbing ? _lad.index : -1; }
	// The character's animations (seconds; 0: there is none, a short glide instead) and how high
	// the way over the top lifts the body (0: not known).
	float ladderMountTime = 0, ladderMountTopTime = 0, ladderExitTopTime = 0, ladderExitFootTime = 0, ladderExitRise = 0;
	// A ladder to take hold of (the use key): at its foot, or at its top from the floor it leads to.
	// Its index (fromTop says which end), -1 for none.
	int ladderInReach(const World& world, float yaw, bool& fromTop) const;
	// Takes hold of it; false if it cannot be now.
	bool grabLadder(const World& world, int index, bool fromTop, float yaw);
	// Where a ladder's prompt goes: the middle of its rungs at the height of `y` (clamped to the ladder).
	bool ladderPoint(const World& world, int index, float y, Vec3& out) const;
	// An animation is putting the body (Drive::place) this frame.
	bool placedByAnimation() const { return _mantle; }
	// False until the ground under the spawn point has been found.
	bool placed() const { return _placed; }
	// What the controller is in the middle of, for diagnostics (g3ds-host
	// --slope-probe, the console's movement recorder). `step` says which way
	// through walk() the last frame took:
	//   g on the floor   l lifted by the animation   L lift ends on a steep slope
	//   u rising   f falling   n landed   s sliding   r slide came to rest
	//   h hovering: nothing below at all (the edge of the loaded world)
	//   p popped up: the feet were in the rock, with nothing below them
	//   k unstuck: off the ground for long without getting anywhere
	//   w swimming   c on a ladder   F flying   - not placed yet
	struct Debug {
		bool sliding, resting, blocked;
		Vec3 slideVel, carry;
		char step;
		u8 pushes; // spheres pushed out of walls this frame
		float airTime; // seconds off the ground without getting anywhere (see unstick)
	};
	Debug debug() const { return Debug {_sliding, _resting, _blocked, _slideVel, _carry, _step, _pushes, _airTime}; }
	// Times the feet were found in the rock with nothing below them and put
	// on top of it, and times the body was off the ground for 1.5 s without
	// getting 30 cm anywhere and was stood on the nearest floor.
	u32 popUps = 0, unstuck = 0;
	mutable LedgeStats _ledgeStats;
	static constexpr float kStuckTime = 1.5f, kStuckDistance = 30.0f;

private:
	void walk(const World& world, const Input& in, float yaw, float dt, const Drive* drive);
	void collideWalls(const World& world);
	bool swim(const World& world, float dt);
	// The ladder taken hold of: its line of rungs, the side it is climbed from, where the floor at its top is.
	struct LadderRun {
		int index = -1;
		u8 phase = 0;
		float time = 0, length = 0;
		Vec3 from {0, 0, 0}, to {0, 0, 0}; // the glide of a mount or an exit
		float yawFrom = 0, yaw = 0;
		Vec3 foot {0, 0, 0}, head {0, 0, 0};
		Vec3 out {0, 0, 1}; // horizontal, from the rungs towards the climber
		float hang = 40.0f;  // how far from the rungs the body hangs
		bool top = false;    // there is floor at the top to step onto
		Vec3 topAt {0, 0, 0};
	} _lad;
	bool ladderCatch(const World& world, const Input& in, float yaw);
	void ladderTick(const World& world, const Input& in, float dt);
	bool ladderBegin(const World& world, int index, bool fromTop, float yaw, Vec3 dir, bool glideOnly);
	Vec3 ladderSpot(float y) const;
	void ladderPhaseTo(u8 phase, float length, Vec3 to);
	void ladderOff(bool grounded);
	float ladderExitStart() const;
	// The nearest surface above the feet (from step height up to `reach` above
	// them): where feet that got into the rock belong.
	bool floorAbove(const World& world, float reach, float& y) const;
	void standAt(float y);
	void unstick(const World& world, float dt);

	float _eyeY = 0;
	bool _placed = false;
	bool _blocked = false; // the last horizontal move ran into something
	bool _letGo = false;   // jumped off a ladder: do not grab it again before landing
	bool _mantle = false;  // put by an animation this frame (Drive::place)
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
	char _step = '-';
	u8 _pushes = 0;
	// Off the ground: for how long, and from where (see unstick()).
	float _airTime = 0;
	Vec3 _airFrom {0, 0, 0};
	bool standsOn(const Vec3& normal, float y) const {
		return normal.y >= config.minFloorNormal || (_resting && y <= _restY + 10.0f);
	}
};

} // namespace g3
