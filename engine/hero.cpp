#include "hero.h"

#include <string.h>

namespace g3 {

namespace {

// Clip names per role, in order of preference. The names are the ones in
// Gothic's HUMANS.MDS; the synthetic test character uses them too.
const char* const kNames[Hero::ROLE_COUNT][4] = {
    {"S_RUN", "S_WALK", nullptr, nullptr},          // IDLE: standing
    {"S_RUNL", "S_WALKL", nullptr, nullptr},        // RUN
    {"S_WALKL", "S_RUNL", nullptr, nullptr},        // WALK
    {"S_JUMP", "S_FALLDN", nullptr, nullptr},       // JUMP: in the air, rising
    {"S_FALLDN", "S_FALL", "S_JUMP", nullptr},      // FALL
    {"S_SWIMF", "S_SWIM", nullptr, nullptr},        // SWIM: forward
    {"S_SWIM", "S_SWIMF", nullptr, nullptr},        // SWIM_IDLE: treading water
};

float wrapAngle(float a) {
	const float pi = 3.14159265f;
	while (a > pi) a -= 2 * pi;
	while (a < -pi) a += 2 * pi;
	return a;
}

} // namespace

bool Hero::load(Pak& pak) {
	release();
	const PakEntry* e = pak.find("meta/hero");
	char name[64] = {};
	if (!e || e->size == 0 || e->size >= sizeof(name) || !pak.read(*e, name)) return false;
	if (!_model.load(pak, name)) {
		logf("hero: model '%s' could not be loaded", name);
		return false;
	}
	for (int r = 0; r < ROLE_COUNT; ++r)
		for (const char* candidate : kNames[r]) {
			if (!candidate || _role[r]) continue;
			_role[r] = _model.clip(candidate);
		}
	_anim.bind(&_model);
	_anim.play(_role[IDLE], 0.0f);
	_current = IDLE;
	return true;
}

void Hero::release() {
	_anim.bind(nullptr);
	_model.release();
	memset(_role, 0, sizeof(_role));
	_placed = false;
}

void Hero::reset(Vec3 feet, float yaw) {
	_feet = feet;
	_yaw = yaw;
	_speed = 0;
	_airTime = 0;
	_lift = 0;
	_placed = false; // the next update takes the position without reading a speed into the jump
}

void Hero::update(const Player& player, const World& world, const float dayLight[3], float dt) {
	if (!loaded() || dt <= 0.0f) return;
	const Vec3 feet = player.feet;
	Vec3 step = feet - _feet;
	// A step this long in one frame is a teleport, not a movement.
	if (!_placed || dot(step, step) > 600.0f * 600.0f) step = {0, 0, 0};
	_placed = true;
	_feet = feet;
	const float speed = sqrtf(step.x * step.x + step.z * step.z) / dt;
	_speed += (speed - _speed) * min(dt * 12.0f, 1.0f); // a frame of jitter must not flip the clip

	// Face where the body is going.
	if (speed > 40.0f) {
		const float target = atan2f(step.x, step.z);
		const float diff = wrapAngle(target - _yaw);
		const float turn = 12.0f * dt;
		_yaw = wrapAngle(_yaw + clamp(diff, -turn, turn));
	}

	// What is the body doing?
	const bool supported = player.grounded || player.swimming || player.climbing;
	_airTime = supported ? 0.0f : _airTime + dt;
	Role want;
	if (player.swimming) {
		want = _speed > 50.0f ? SWIM : SWIM_IDLE;
	} else if (!supported && _airTime > 0.12f) { // steps and kerbs are not falls
		want = player.velocityY > 0.0f ? JUMP : FALL;
	} else if (_speed < 40.0f) {
		want = IDLE;
	} else if (_speed < 260.0f || player.waterDepth > player.config.wadeDepth) {
		want = WALK;
	} else {
		want = RUN;
	}
	const Clip* clip = _role[want] ? _role[want] : _role[IDLE];
	// A clip that leads into the wanted one (the take-off before the jump
	// pose) is left to finish.
	const Clip* cur = _anim.current();
	const bool leadsThere = cur && clip && !cur->loops() && cur->header().next && _model.clip(cur->header().next) == clip;
	if (!leadsThere) _anim.play(clip);
	_current = want;
	// Feet that keep up with the ground: play as fast as the body moves.
	cur = _anim.current();
	float rate = 1.0f;
	if (cur && cur->header().moveSpeed > 1.0f && (want == RUN || want == WALK || want == SWIM))
		rate = clamp(_speed / cur->header().moveSpeed, 0.5f, 1.8f);
	_anim.setRate(rate);
	_anim.update(dt);

	// A swimmer lies in the surface: the hips a hand below it, wherever the
	// clip keeps them above the model's origin. (The controller's "feet" hang
	// deep in the water then.)
	float lift = 0;
	if (player.swimming) lift = player.waterDepth - 22.0f - _anim.pose().pos[0].y;
	_lift += (lift - _lift) * min(dt * 6.0f, 1.0f);

	// Lit like the ground underfoot: its baked light, tinted by the time of
	// day as far as that ground sees the sky.
	float y;
	Vec3 n;
	u8 l[4];
	if (world.rayDown({feet.x, feet.y + 80.0f, feet.z}, 600.0f, y, n, l)) {
		const float w = float(l[3]) * (1.0f / 255.0f);
		for (int i = 0; i < 3; ++i) {
			float target = float(l[i]) * (1.0f / 255.0f) * (1.0f + w * (dayLight[i] - 1.0f));
			target = max(target * 1.15f, 0.22f); // never a silhouette
			_light[i] += (target - _light[i]) * min(dt * 5.0f, 1.0f);
		}
	}
}

void Hero::draw() const {
	if (!loaded()) return;
	_model.draw(_anim.pose(), origin(), _yaw, _light);
}

} // namespace g3
