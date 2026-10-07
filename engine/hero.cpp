#include "hero.h"

#include <string.h>

namespace g3 {

namespace {

// The states' names in ZenGin's model scripts: the animation is "S_<name>",
// transitions between two are "T_<a>_2_<b>".
const char* const kShort[] = {"RUN", "RUNL", "WALK", "WALKL", "WALKBL", "WALKWL", "JUMP", "FALLDN", "FALL", "SWIM", "SWIMF", "SWIMB", "STAND"};

// Turning: the original's 90 degrees a second on the keys (measured by the
// OpenGothic project; "100 by some sources"), and up to twice that with the
// mouse - here the second stick.
const float kTurnSpeed = 1.6581f, kLookTurnSpeed = 3.1416f;
// Below this the engine does not bother with the turn-on-the-spot animation.
const float kTurnAnimSpeed = 0.5236f; // 30 degrees a second
// A change of animation that the script gives no time for still gets this
// much fade: nothing may jump.
const float kMinFade = 0.1f;
// A jump whose take-off animation does not lift the body (or a model without
// one) is thrown instead: this fast upwards, for about 85 cm.
const float kThrowUp = 408.0f;

float wrapAngle(float a) {
	const float pi = 3.14159265f;
	while (a > pi) a -= 2 * pi;
	while (a < -pi) a += 2 * pi;
	return a;
}

Vec3 turned(Vec3 v, float yaw) {
	const float s = sinf(yaw), c = cosf(yaw);
	return {v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}

float fadeOf(const Clip* c) { return c ? max(max(c->header().blendIn, c->header().blendOut), kMinFade) : kMinFade; }

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
	char buf[48];
	for (int s = 0; s < ST_COUNT; ++s) {
		snprintf(buf, sizeof(buf), "S_%s", kShort[s]);
		_state[s] = _model.clip(buf);
	}
	// Transitions between the states: animations and fades.
	for (int a = 0; a <= ST_COUNT; ++a)
		for (int b = 0; b <= ST_COUNT; ++b) {
			_via[a][b] = {nullptr, -1.0f};
			if (a == b) continue;
			snprintf(buf, sizeof(buf), "T_%s_2_%s", kShort[a], kShort[b]);
			const u32 h = hashName(buf);
			if (const Clip* c = _model.clip(h))
				_via[a][b].clip = c;
			else if (const ModelBlend* bl = _model.blend(h))
				_via[a][b].fade = bl->time;
		}
	_jumpBack = _model.clip("T_JUMPB");
	_strafe[0][0] = _model.clip("T_RUNSTRAFEL");
	_strafe[0][1] = _model.clip("T_RUNSTRAFER");
	_strafe[1][0] = _model.clip("T_WALKSTRAFEL");
	_strafe[1][1] = _model.clip("T_WALKSTRAFER");
	_turn[0][0] = _model.clip("T_RUNTURNL");
	_turn[0][1] = _model.clip("T_RUNTURNR");
	_turn[1][0] = _model.clip("T_WALKTURNL");
	_turn[1][1] = _model.clip("T_WALKTURNR");
	_turn[2][0] = _model.clip("T_SWIMTURNL");
	_turn[2][1] = _model.clip("T_SWIMTURNR");
	for (int side = 0; side < 2; ++side) {
		if (!_strafe[1][side]) _strafe[1][side] = _strafe[0][side];
		if (!_turn[1][side]) _turn[1][side] = _turn[0][side];
	}
	_restY = _model.node(0).restPos[1];
	_anim.bind(&_model);
	_anim.play(stateClip(ST_RUN), 0.0f);
	_body = GROUNDED;
	_roleNow = IDLE;
	_base = ST_RUN;
	_drawn = _anim.pose();
	return true;
}

void Hero::release() {
	_anim.bind(nullptr);
	_model.release();
	memset(_state, 0, sizeof(_state));
	memset(_via, 0, sizeof(_via));
	memset(_strafe, 0, sizeof(_strafe));
	memset(_turn, 0, sizeof(_turn));
	_jumpBack = _takeoff = nullptr;
	_placed = false;
}

void Hero::reset(Vec3 feet, float yaw) {
	_feet = feet;
	_yaw = yaw;
	_speed = 0;
	_airTime = 0;
	_lean = _turnLag = 0;
	_hipMix = 0;
	_jumping = _leftGround = _forceNext = false;
	_takeoff = nullptr;
	_placed = false; // the next follow() takes the position without reading a speed into the jump
	if (loaded()) {
		_body = GROUNDED;
		_roleNow = IDLE;
		_anim.play(stateClip(_walkMode ? ST_WALK : ST_RUN), 0.0f);
		_anim.takeMotion();
		updatePose(0.0f);
	}
}

// A state's clip, or the nearest thing the model has.
const Clip* Hero::stateClip(int state) const {
	static const int kFallback[ST_COUNT][3] = {
	    {ST_WALK, -1, -1},            // RUN
	    {ST_WALKL, -1, -1},           // RUNL
	    {ST_RUN, -1, -1},             // WALK
	    {ST_RUNL, -1, -1},            // WALKL
	    {-1, -1, -1},                 // WALKBL (the hop back stands in)
	    {ST_WALKL, ST_RUNL, -1},      // WALKWL
	    {ST_FALLDN, ST_RUN, -1},      // JUMP
	    {ST_FALL, ST_JUMP, ST_RUN},   // FALLDN
	    {ST_FALLDN, ST_JUMP, ST_RUN}, // FALL
	    {ST_SWIMF, ST_RUN, -1},       // SWIM
	    {ST_SWIM, ST_RUNL, -1},       // SWIMF
	    {ST_SWIM, ST_SWIMF, -1},      // SWIMB
	};
	if (_state[state]) return _state[state];
	for (int f : kFallback[state])
		if (f >= 0 && _state[f]) return _state[f];
	return nullptr;
}

int Hero::stateOf(const Clip* clip) const {
	if (!clip) return -1;
	for (int s = 0; s < ST_COUNT; ++s)
		if (_state[s] == clip) return s;
	return -1;
}

bool Hero::isTurn(const Clip* clip) const {
	if (!clip) return false;
	for (const auto& pair : _turn)
		for (const Clip* c : pair)
			if (c == clip) return true;
	return false;
}

// Towards a state, the way the model script means it.
void Hero::go(int state, bool force, float forceFade) {
	const Clip* target = stateClip(state);
	const Clip* cur = _anim.current();
	if (!target || cur == target) return;
	// A transition is played to its end.
	const bool busy = cur && !cur->loops() && !_anim.finished();
	if (busy && !force) return;
	if (force) {
		_anim.play(target, forceFade);
		return;
	}
	int from = stateOf(cur);
	if (from < 0 && isTurn(cur)) from = _base;
	// Anything else on the ground that has run out (a hop back, a step
	// sideways without a follower) counts as standing.
	const bool fromStand = from == ST_RUN || from == ST_WALK || (from < 0 && _body == GROUNDED);
	const bool toStand = state == ST_RUN || state == ST_WALK;
	Via via = {nullptr, -1.0f};
	if (from >= 0) via = _via[from][state];
	if (!via.clip && via.fade < 0 && fromStand) via = _via[STAND][state];
	if (!via.clip && via.fade < 0 && from >= 0 && toStand) via = _via[from][STAND];
	if (via.clip && via.clip != cur)
		_anim.play(via.clip, fadeOf(via.clip));
	else
		_anim.play(target, via.fade >= 0 ? max(via.fade, kMinFade) : fadeOf(target));
}

// A single action (a hop back, a step sideways, a turn on the spot).
void Hero::act(const Clip* clip, float fade) {
	const Clip* cur = _anim.current();
	if (!clip) return;
	if (cur == clip) {
		// Still asked for when it has run out: once more.
		if (!clip->loops() && _anim.finished()) _anim.restart(fade);
		return;
	}
	if (cur && !cur->loops() && !_anim.finished()) return;
	_anim.play(clip, fade);
}

Drive Hero::control(const Input& in, const Player& player, float dt) {
	Drive drive;
	if (!loaded() || dt <= 0.0f) return drive;

	// ---- what the player asks for ----
	if (in.toggleWalk) _walkMode = !_walkMode;
	const int fwd = in.moveZ > 0.3f ? 1 : in.moveZ < -0.3f ? -1 : 0;
	// A stick pushed only part of the way is a walk, too (with some slack,
	// so that a thumb resting on the border does not flicker between the two).
	// (How far the stick is pushed, whichever way: steering while running
	// must not slow the run down.)
	const float push = sqrtf(in.moveZ * in.moveZ + in.moveX * in.moveX);
	if (push > 0.8f) _slow = false;
	if (push < 0.68f) _slow = true;
	const bool walk = _walkMode || (fwd > 0 && _slow);
	const int strafe = in.strafe > 0.5f ? 1 : in.strafe < -0.5f ? -1 : 0;
	const bool jumpPressed = in.jump && !_jumpHeld;
	_jumpHeld = in.jump;
	const float turnRate = clamp(in.moveX, -1.0f, 1.0f) * kTurnSpeed + clamp(in.lookX, -1.0f, 1.0f) * kLookTurnSpeed;
	_yaw = wrapAngle(_yaw + turnRate * dt);
	const bool turning = fabsf(turnRate) >= kTurnAnimSpeed;
	const int right = turnRate > 0 ? 1 : 0;

	// ---- which animation ----
	const Clip* cur = _anim.current();
	const bool force = _forceNext;
	_forceNext = false;
	switch (_body) {
	case SWIMMING:
		if (fwd > 0) {
			go(ST_SWIMF, force, 0.3f);
			_roleNow = SWIM;
		} else if (fwd < 0 && _state[ST_SWIMB]) {
			go(ST_SWIMB, force, 0.3f);
			_roleNow = SWIM;
		} else if (turning && _turn[2][right] && !force) {
			_base = ST_SWIM;
			act(_turn[2][right], fadeOf(_turn[2][right]));
			_roleNow = SWIM_IDLE;
		} else {
			go(ST_SWIM, force, 0.3f);
			_roleNow = SWIM_IDLE;
		}
		break;
	case AIRBORNE:
		_airTime += dt;
		if (_jumping) {
			go(ST_JUMP, force, 0.15f);
			_roleNow = JUMP;
		} else if (_airTime > 0.25f) { // a kerb is not a fall
			go(_airTime > 1.2f ? ST_FALL : ST_FALLDN, stateOf(cur) != ST_FALL && stateOf(cur) != ST_FALLDN, 0.2f);
			_roleNow = FALL;
		}
		break;
	case TAKEOFF:
		_roleNow = JUMP;
		break;
	case GROUNDED: {
		const bool wading = player.waterDepth > player.config.wadeDepth;
		const int idle = walk ? ST_WALK : ST_RUN;
		const int curState = stateOf(cur);
		const bool standing = curState == ST_RUN || curState == ST_WALK || isTurn(cur) || !cur || (!cur->loops() && _anim.finished());
		const bool busy = cur && !cur->loops() && !_anim.finished();
		if (player.climbing) {
			go(idle, force);
			_roleNow = IDLE;
		} else if (jumpPressed && !busy && !wading) {
			// Off the ground: through the take-off the script has for where
			// the body is coming from.
			int from = curState;
			if (from < 0 || from == ST_WALK || from == ST_WALKL || from == ST_WALKBL || from == ST_WALKWL) from = STAND;
			Via via = _via[from][ST_JUMP];
			if (!via.clip && from == ST_RUN) via = _via[STAND][ST_JUMP];
			if (via.clip && via.clip->rise() >= 20.0f) {
				// The animation carries the body up and forward.
				_anim.play(via.clip, fadeOf(via.clip));
				_takeoff = via.clip;
				_body = TAKEOFF;
				_liftSum = {0, 0, 0};
				_liftTime = 0;
				_liftY = 0;
				_leftGround = false;
			} else {
				// No lift in the data: thrown - forward as fast as the take-off
				// animation travels, or else keeping the pace it has.
				const Clip* c = _anim.current();
				float pace = c && (c == _state[ST_RUNL] || c == _state[ST_WALKL]) ? c->header().moveSpeed : 0.0f;
				if (via.clip && via.clip->header().moveSpeed > pace) pace = via.clip->header().moveSpeed;
				drive.launch = true;
				drive.velocity = turned({0, kThrowUp, pace}, _yaw);
				_anim.play(via.clip ? via.clip : stateClip(ST_JUMP), 0.12f);
				_body = AIRBORNE;
				_jumping = true;
				_airTime = 0;
			}
			_roleNow = JUMP;
		} else if (strafe != 0 && _strafe[walk][strafe > 0] && (standing || busy)) {
			act(_strafe[walk][strafe > 0], fadeOf(_strafe[walk][strafe > 0]));
			if (!busy) _roleNow = STRAFE;
		} else if (fwd > 0) {
			go(wading ? ST_WALKWL : walk ? ST_WALKL : ST_RUNL, force);
			if (!busy) _roleNow = (wading || walk) ? WALK : RUN;
		} else if (fwd < 0) {
			if ((walk || wading) && _state[ST_WALKBL]) {
				go(ST_WALKBL, force);
			} else if (_jumpBack) {
				// As in the original: without a weapon, "back" is a hop back.
				if (standing) act(_jumpBack, fadeOf(_jumpBack));
				else go(idle, force);
			} else {
				go(idle, force);
			}
			if (!busy) _roleNow = BACK;
		} else if (turning && standing && _turn[walk][right]) {
			_base = idle;
			act(_turn[walk][right], fadeOf(_turn[walk][right]));
			_roleNow = TURN;
		} else {
			go(idle, force);
			if (!busy) _roleNow = IDLE;
		}
		break;
	}
	}

	// ---- on with it, and where it carries the body ----
	_anim.update(dt);
	const Vec3 moved = turned(_anim.takeMotion(), _yaw);
	switch (_body) {
	case TAKEOFF:
		if (_anim.current() == _takeoff && !_anim.finished()) {
			// The body is as high above where it stood as the animation has
			// risen above where it began. The crouch before the leap goes
			// below that: the body stays on the ground and the pose shows it.
			const float lift = max(_takeoff->motion(_anim.time()).y, 0.0f);
			drive.lift = true;
			drive.step = {moved.x, lift - _liftY, moved.z};
			_liftY = lift;
			_liftSum = _liftSum + moved;
			_liftTime += dt;
		} else {
			// The take-off is over: the body flies on with the pace it had
			// through it, as in the original.
			drive.launch = true;
			drive.velocity = _liftTime > 0 ? _liftSum * (1.0f / _liftTime) : Vec3 {0, 0, 0};
			_body = AIRBORNE;
			_jumping = true;
			_airTime = 0;
			_takeoff = nullptr;
		}
		break;
	case AIRBORNE:
		break; // ballistic: the controller keeps the body's pace
	default:
		if (player.climbing) {
			// Ladders have no animation yet: the controller's own slow step.
			const float pace = max(in.moveZ, 0.0f) * 120.0f * dt;
			drive.step = {sinf(_yaw) * pace, 0, cosf(_yaw) * pace};
		} else {
			drive.step = {moved.x, 0, moved.z};
		}
		break;
	}
	updatePose(dt);
	return drive;
}

// The pose that is drawn: the animator's, with the hips at their rest height
// while the body is carried by something else than its legs (in a jump the
// animation's height has gone into the body's position; in the water the
// body floats at a depth of its own).
void Hero::updatePose(float dt) {
	const bool fixHips = _body == SWIMMING || _body == TAKEOFF || (_body == AIRBORNE && _jumping);
	const float want = fixHips ? 1.0f : 0.0f;
	// (Into a jump at once - the height is in the body from the first frame - out of it softly.)
	if (dt <= 0.0f || (_body == TAKEOFF)) _hipMix = want;
	else _hipMix += clamp(want - _hipMix, -dt * 6.0f, dt * 6.0f);
	_drawn = _anim.pose();
	_drawn.pos[0].x = 0;
	_drawn.pos[0].z = 0;
	if (_body == TAKEOFF && _takeoff && _anim.current() == _takeoff) {
		// Only what lifts the body has gone into its position: the crouch
		// before the leap is still the pose's to show.
		_drawn.pos[0].y -= max(_takeoff->motion(_anim.time()).y, 0.0f);
		return;
	}
	_drawn.pos[0].y += (_restY - _drawn.pos[0].y) * _hipMix;
}

void Hero::follow(const Player& player, const World& world, const float dayLight[3], float dt) {
	if (!loaded() || dt <= 0.0f) return;
	const Vec3 feet = player.feet;
	Vec3 step = feet - _feet;
	// A step this long in one frame is a teleport, not a movement.
	if (!_placed || dot(step, step) > 600.0f * 600.0f) step = {0, 0, 0};
	_placed = true;
	_feet = feet;
	const float speed = sqrtf(step.x * step.x + step.z * step.z) / dt;
	_speed += (speed - _speed) * min(dt * 12.0f, 1.0f);

	// What is the body in now?
	if (player.swimming) {
		if (_body != SWIMMING) {
			_body = SWIMMING;
			_jumping = false;
			_takeoff = nullptr;
			_forceNext = true; // whatever it was doing, it swims now
		}
	} else if (player.grounded || player.climbing) {
		if (_body == AIRBORNE || _body == SWIMMING) {
			// Landed (or waded out): a transition that was still running is cut short.
			const Clip* cur = _anim.current();
			_forceNext = cur && !cur->loops() && !_anim.finished();
			_body = GROUNDED;
			_jumping = false;
		} else if (_body == TAKEOFF && _leftGround) {
			_body = GROUNDED; // came down again before the take-off was over
			_takeoff = nullptr;
			_forceNext = true;
		}
	} else {
		if (_body == GROUNDED) {
			_body = AIRBORNE;
			_airTime = 0;
			_jumping = false;
		} else if (_body == SWIMMING) {
			_body = AIRBORNE;
			_airTime = 0;
		} else if (_body == TAKEOFF) {
			_leftGround = true;
		}
	}

	// A runner leans into the turn, by as much as the camera lags behind it
	// (at most 14.5 degrees, as in the original).
	const float lean = (_body == GROUNDED && _anim.current() == _state[ST_RUNL]) ? clamp(_turnLag, -0.253f, 0.253f) : 0.0f;
	_lean += clamp(lean - _lean, -0.61f * dt, 0.61f * dt);

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
	_model.draw(_drawn, _feet, _yaw, _light, _lean);
}

} // namespace g3
