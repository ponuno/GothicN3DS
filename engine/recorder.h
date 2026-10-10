// The movement recorder ("black box"): what the character controller did in
// each of the last five seconds' frames, kept in a ring. On the console a
// key combination writes it to the SD card at the moment the character is
// stuck somewhere; g3ds-host --slope-probe prints the same lines, so a
// console's record and a probe's run can be laid side by side.
#pragma once
#include "player.h"

#include <stdio.h>

namespace g3 {

struct MoveSample {
	float time, dt;   // seconds since the recorder started; this frame's step
	Vec3 feet;
	float eyeY;       // where the view is (the console's "pos" shows it): smoothed over steps and landings
	float floorY, floorNy; // the floor under the feet (from step height down 3 m), its normal's y; -1: none
	float velocityY;
	float slideX, slideZ, carryX, carryZ;
	float moveX, moveZ, airTime;
	u8 flags;         // MOVE_* below
	char step;        // Player::Debug::step
	char body;        // the character: G ground, T take-off, A air, S swim; ? none
	u8 pushes;
};

enum : u8 {
	MOVE_GROUNDED = 1, MOVE_SLIDING = 2, MOVE_RESTING = 4, MOVE_BLOCKED = 8,
	MOVE_JUMP = 16, MOVE_SWIMMING = 32, MOVE_CLIMBING = 64, MOVE_FLYING = 128,
};

class MoveRecorder {
public:
	static const u32 kCapacity = 300; // five seconds at 60 fps

	// One frame, after the player's update. `body` as in MoveSample.
	void record(const World& world, const Player& player, const Input& in, char body, float dt);
	void clear() { _count = 0; _next = 0; }
	u32 count() const { return _count; }
	// The i-th oldest sample still kept.
	const MoveSample& sample(u32 i) const { return _ring[(_next + kCapacity - _count + i) % kCapacity]; }

	// The column header and one sample as a line of text (no newline).
	static const char* header();
	static int format(char* out, u32 size, const MoveSample& s);
	// Everything kept, oldest first, after a title line. Returns false if
	// the file could not be written.
	bool write(FILE* f, const char* title) const;

private:
	MoveSample _ring[kCapacity];
	u32 _count = 0, _next = 0;
	double _time = 0;
};

} // namespace g3
