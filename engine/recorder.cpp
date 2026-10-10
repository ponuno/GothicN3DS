#include "recorder.h"

namespace g3 {

void MoveRecorder::record(const World& world, const Player& player, const Input& in, char body, float dt) {
	_time += double(dt);
	MoveSample& s = _ring[_next];
	_next = (_next + 1) % kCapacity;
	if (_count < kCapacity) ++_count;
	const Player::Debug d = player.debug();
	s.time = float(_time);
	s.dt = dt;
	s.feet = player.feet;
	s.eyeY = player.eye().y;
	Vec3 n {0, 0, 0};
	float y = 0;
	if (world.rayDown({player.feet.x, player.feet.y + player.config.stepHeight, player.feet.z}, player.config.stepHeight + 300.0f, y, n)) {
		s.floorY = y;
		s.floorNy = n.y;
	} else {
		s.floorY = 0;
		s.floorNy = -1;
	}
	s.velocityY = player.velocityY;
	s.slideX = d.slideVel.x;
	s.slideZ = d.slideVel.z;
	s.carryX = d.carry.x;
	s.carryZ = d.carry.z;
	s.moveX = in.moveX;
	s.moveZ = in.moveZ;
	s.airTime = d.airTime;
	s.flags = u8((player.grounded ? MOVE_GROUNDED : 0) | (d.sliding ? MOVE_SLIDING : 0) | (d.resting ? MOVE_RESTING : 0) |
	             (d.blocked ? MOVE_BLOCKED : 0) | (in.jump ? MOVE_JUMP : 0) | (player.swimming ? MOVE_SWIMMING : 0) |
	             (player.climbing ? MOVE_CLIMBING : 0) | (player.flying ? MOVE_FLYING : 0));
	s.step = d.step;
	s.body = body;
	s.pushes = d.pushes;
}

const char* MoveRecorder::header() {
	return "columns: time s, frame ms, feet x y z, eye y, flags (g on the ground, s sliding, r resting, b blocked,\n"
	       "  j jump held, w swimming, c ladder, F flying), step through the controller (g floor, l lift, L lift on a\n"
	       "  steep slope, u rising, f falling, n landed, s sliding, r slide at rest, h nothing below, p popped out of the\n"
	       "  rock, k unstuck, w swim, c ladder, F fly), body (G ground, T take-off, A air, S swim), wall pushes,\n"
	       "  floor below: height under the feet and normal y (-1: none within 3 m), vertical speed, slide speed x z,\n"
	       "  carried speed x z, stick x z, seconds off the ground";
}

int MoveRecorder::format(char* out, u32 size, const MoveSample& s) {
	char flags[9];
	static const char kFlags[] = "gsrbjwcF";
	for (int i = 0; i < 8; ++i) flags[i] = (s.flags & (1 << i)) ? kFlags[i] : '-';
	flags[8] = 0;
	return snprintf(out, size, "%7.3f %5.1f %8.1f %7.1f %8.1f %7.1f %s %c %c %u %6.1f %5.2f %6.0f %5.0f %5.0f %5.0f %5.0f %5.2f %5.2f %4.2f",
	                double(s.time), double(s.dt * 1000.0f), double(s.feet.x), double(s.feet.y), double(s.feet.z), double(s.eyeY), flags,
	                s.step ? s.step : '?', s.body ? s.body : '?', unsigned(s.pushes), double(s.floorNy >= 0 ? s.floorY - s.feet.y : 0.0f),
	                double(s.floorNy), double(s.velocityY), double(s.slideX), double(s.slideZ), double(s.carryX), double(s.carryZ),
	                double(s.moveX), double(s.moveZ), double(s.airTime));
}

bool MoveRecorder::write(FILE* f, const char* title) const {
	if (!f) return false;
	fprintf(f, "== %s: %u frames\n%s\n", title, unsigned(_count), header());
	char line[256];
	for (u32 i = 0; i < _count; ++i) {
		format(line, sizeof(line), sample(i));
		fprintf(f, "%s\n", line);
	}
	return ferror(f) == 0;
}

} // namespace g3
