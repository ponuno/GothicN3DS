#include "hero.h"

#include <string.h>

namespace g3 {

namespace {

// The states' names in ZenGin's model scripts: the animation is "S_<name>",
// transitions between two are "T_<a>_2_<b>".
const char* const kShort[] = {"RUN", "RUNL", "WALK", "WALKL", "WALKBL", "WALKWL", "JUMP", "FALLDN", "FALL", "SWIM", "SWIMF", "SWIMB", "STAND"};
// Sneaking has states of its own in place of standing and going.
const char* const kSneak[] = {"SNEAK", "SNEAKL", "SNEAK", "SNEAKL", "SNEAKBL"};

// Turning: the original's 90 degrees a second on the keys (measured by the
// OpenGothic project; "100 by some sources"), and up to twice that with the
// mouse - here the second stick.
const float kTurnSpeed = 1.6581f, kLookTurnSpeed = 3.1416f;
// Steered by a heading (the console scheme): as quick as a body turns.
const float kSteerSpeed = 7.0f;
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

// A state's name in a row: the weapon's word before standing and going
// ("1HRUNL"), sneaking's own words for those, the plain name for the rest.
void stateName(int row, int state, char* out, size_t size) {
	if (state > 4 || row == WEAPON_NONE) {
		snprintf(out, size, "%s", kShort[state]);
	} else if (row == WEAPON_MODES) {
		snprintf(out, size, "%s", kSneak[state]);
	} else {
		snprintf(out, size, "%s%s", weaponWord(u8(row)), kShort[state]);
	}
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
	char buf[64], a[24], b[24];
	for (int r = 0; r < ROWS; ++r) {
		for (int s = 0; s < ST_COUNT; ++s) {
			stateName(r, s, a, sizeof(a));
			snprintf(buf, sizeof(buf), "S_%s", a);
			_state[r][s] = _model.clip(buf);
		}
		// Transitions between the states: animations and fades.
		for (int x = 0; x <= ST_COUNT; ++x)
			for (int y = 0; y <= ST_COUNT; ++y) {
				_via[r][x][y] = {nullptr, -1.0f};
				if (x == y) continue;
				stateName(r, x, a, sizeof(a));
				stateName(r, y, b, sizeof(b));
				snprintf(buf, sizeof(buf), "T_%s_2_%s", a, b);
				const u32 h = hashName(buf);
				if (const Clip* c = _model.clip(h))
					_via[r][x][y].clip = c;
				else if (const ModelBlend* bl = _model.blend(h))
					_via[r][x][y].fade = bl->time;
				else if (r != WEAPON_NONE)
					_via[r][x][y] = _via[WEAPON_NONE][x][y]; // (none of its own: as without a weapon)
			}
		const char* w = r == WEAPON_NONE || r == ROW_SNEAK ? "" : weaponWord(u8(r));
		snprintf(buf, sizeof(buf), "T_%sJUMPB", w);
		_jumpBack[r] = _model.clip(buf);
		if (!_jumpBack[r]) {
			snprintf(buf, sizeof(buf), "T_%sPARADEJUMPB", w);
			_jumpBack[r] = _model.clip(buf);
		}
		if (!_jumpBack[r]) _jumpBack[r] = _model.clip("T_JUMPB");
		for (int side = 0; side < 2; ++side) {
			const char* lr = side ? "R" : "L";
			if (r == ROW_SNEAK) {
				snprintf(buf, sizeof(buf), "T_SNEAKSTRAFE%s", lr);
				_strafe[r][0][side] = _strafe[r][1][side] = _model.clip(buf);
				snprintf(buf, sizeof(buf), "T_SNEAKTURN%s", lr);
				_turn[r][0][side] = _turn[r][1][side] = _model.clip(buf);
			} else {
				snprintf(buf, sizeof(buf), "T_%sRUNSTRAFE%s", w, lr);
				_strafe[r][0][side] = _model.clip(buf);
				snprintf(buf, sizeof(buf), "T_%sWALKSTRAFE%s", w, lr);
				_strafe[r][1][side] = _model.clip(buf);
				snprintf(buf, sizeof(buf), "T_%sRUNTURN%s", w, lr);
				_turn[r][0][side] = _model.clip(buf);
				snprintf(buf, sizeof(buf), "T_%sWALKTURN%s", w, lr);
				_turn[r][1][side] = _model.clip(buf);
			}
			snprintf(buf, sizeof(buf), "T_SWIMTURN%s", lr);
			_turn[r][2][side] = _model.clip(buf);
			for (int k = 0; k < 2; ++k) {
				if (!_strafe[r][1][side]) _strafe[r][1][side] = _strafe[r][0][side];
				if (!_turn[r][1][side]) _turn[r][1][side] = _turn[r][0][side];
				if (r != WEAPON_NONE) {
					if (!_strafe[r][k][side]) _strafe[r][k][side] = _strafe[WEAPON_NONE][k][side];
					if (!_turn[r][k][side]) _turn[r][k][side] = _turn[WEAPON_NONE][k][side];
				}
			}
		}
		snprintf(buf, sizeof(buf), "T_%sPARADE_O", w);
		_parryClip[r] = r == WEAPON_NONE || r == ROW_SNEAK ? nullptr : _model.clip(buf);
		if (!_parryClip[r] && r != WEAPON_NONE && r != ROW_SNEAK) {
			snprintf(buf, sizeof(buf), "T_%sPARADE_0", w);
			_parryClip[r] = _model.clip(buf);
		}
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
	memset(_jumpBack, 0, sizeof(_jumpBack));
	memset(_parryClip, 0, sizeof(_parryClip));
	_takeoff = nullptr;
	_placed = false;
	_weapon = WEAPON_NONE;
	_drawTo = 0xFF;
	_drawClip = nullptr;
	_blow.reset();
	_reel = nullptr;
	_down = false;
	_sneak = false;
	_use = Use();
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
	_drawTo = 0xFF;
	_blow.reset();
	_reel = nullptr;
	_use = Use();
	if (loaded()) {
		_body = GROUNDED;
		_roleNow = _down ? DOWN : IDLE;
		if (!_down) _anim.play(stateClip(_walkMode ? ST_WALK : ST_RUN), 0.0f);
		_anim.takeMotion();
		updatePose(0.0f);
	}
}

int Hero::row() const {
	if (_weapon != WEAPON_NONE) return _weapon;
	return _sneak ? int(ROW_SNEAK) : int(WEAPON_NONE);
}

// A state's clip in a row: its own, else the plain one, else the nearest
// thing the model has.
const Clip* Hero::rowClip(int r, int state) const {
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
	if (_state[r][state]) return _state[r][state];
	if (r != WEAPON_NONE) {
		for (int f : kFallback[state])
			if (f >= 0 && _state[r][f]) return _state[r][f];
	}
	if (_state[WEAPON_NONE][state]) return _state[WEAPON_NONE][state];
	for (int f : kFallback[state])
		if (f >= 0 && _state[WEAPON_NONE][f]) return _state[WEAPON_NONE][f];
	return nullptr;
}

const Clip* Hero::stateClip(int state) const { return rowClip(row(), state); }

int Hero::stateOf(const Clip* clip) const {
	if (!clip) return -1;
	const int r = row();
	for (int s = 0; s < ST_COUNT; ++s)
		if (_state[r][s] == clip) return s;
	for (int rr = 0; rr < ROWS; ++rr)
		for (int s = 0; s < ST_COUNT; ++s)
			if (_state[rr][s] == clip) return s;
	return -1;
}

bool Hero::isStrafe(const Clip* clip) const {
	if (!clip) return false;
	for (const auto& rowStrafes : _strafe)
		for (const auto& pair : rowStrafes)
			for (const Clip* c : pair)
				if (c == clip) return true;
	return false;
}

bool Hero::isTurn(const Clip* clip) const {
	if (!clip) return false;
	for (const auto& rowTurns : _turn)
		for (const auto& pair : rowTurns)
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
	const int r = row();
	Via via = {nullptr, -1.0f};
	if (from >= 0) via = _via[r][from][state];
	if (!via.clip && via.fade < 0 && fromStand) via = _via[r][STAND][state];
	if (!via.clip && via.fade < 0 && from >= 0 && toStand) via = _via[r][from][STAND];
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

// ---- fighting ----

// "T_%s_2_%sRUN" with the weapon's word.
const Clip* Hero::weaponClip(const char* pattern, u8 mode) const {
	char name[48];
	const char* w = weaponWord(mode);
	snprintf(name, sizeof(name), pattern, w, w);
	return _model.clip(name);
}

bool Hero::draw(u8 mode) {
	if (!loaded() || _down || mode >= WEAPON_MODES || _body != GROUNDED || drawing() || usingItem()) return false;
	if (mode == _weapon) return true;
	if (_weapon != WEAPON_NONE && mode != WEAPON_NONE) {
		// Another weapon in hand: away with it, the new one is the next press.
		mode = WEAPON_NONE;
	}
	const Clip* c = nullptr;
	if (mode == WEAPON_NONE) {
		c = weaponClip("T_%sRUN_2_%s", _weapon);
		if (!c) c = weaponClip("T_%sMOVE_2_MOVE", _weapon);
	} else {
		c = weaponClip("T_RUN_2_%s", mode);
		if (!c) c = weaponClip("T_%s_2_%sRUN", mode);
		if (!c) c = weaponClip("T_MOVE_2_%sMOVE", mode);
	}
	_blow.reset();
	_sneak = false;
	if (!c) {
		// No animation for it: the hands change at once.
		_weapon = mode;
		_forceNext = true;
		return true;
	}
	_anim.play(c, 0.15f);
	const ClipFight* f = c->fight();
	_drawTo = mode;
	_drawClip = c;
	_drawAt = f && f->fightMode != CLIP_NO_FRAME ? c->frameTime(f->fightMode) : c->duration() * 0.5f;
	_roleNow = DRAW;
	return true;
}

bool Hero::attack(int kind) {
	if (!loaded() || _weapon == WEAPON_NONE || _down || _body != GROUNDED || drawing() || usingItem()) return false;
	const Clip* cur = _anim.current();
	const bool running = cur && (cur == _state[_weapon][ST_RUNL] || cur == _state[WEAPON_NONE][ST_RUNL]);
	const Clip* c = nullptr;
	if (_weapon == WEAPON_FIST) {
		if (running) c = weaponClip("T_%sATTACKMOVE", _weapon);
		if (!c) c = weaponClip("S_%sATTACK", _weapon);
	} else {
		if (kind == 1) c = weaponClip("T_%sATTACKL", _weapon);
		else if (kind == 2) c = weaponClip("T_%sATTACKR", _weapon);
		else if (running) c = weaponClip("T_%sATTACKMOVE", _weapon);
		if (!c) c = weaponClip("S_%sATTACK", _weapon);
	}
	if (!c) return false;
	if (_blow.clip && cur == _blow.clip) {
		const ClipFight* f = cur->fight();
		const float fps = cur->header().fps, t = _anim.time();
		const bool inWindow = f && inComboWindow(f, _blow.combo, fps, t);
		if (c == cur) {
			// The same attack again: within the window the combo goes on with
			// its next blow, from where this one ends; outside it, it is over.
			if (inWindow && !_comboBroken && _blow.combo + 1u < f->hits) {
				const float end = blowEnd(f, _blow.combo, fps);
				if (end > t) _anim.seek(end);
				++_blow.combo;
				_blow.landed = _blow.combo;
				return true;
			}
			if (!inWindow) _comboBroken = true;
			return false;
		}
		if (!inWindow) return false; // (another blow only where a combo may go on)
	} else if (cur && !cur->loops() && !_anim.finished() && cur != _reel && !isTurn(cur) && stateOf(cur) < 0) {
		// In the middle of something else (a hop back, a step aside, a parry).
		bool parry = false;
		for (const Clip* p : _parryClip) parry = parry || p == cur;
		if (!parry) return false;
	}
	_anim.play(c, 0.08f);
	_blow.reset();
	_blow.clip = c;
	_comboBroken = false;
	_roleNow = ATTACK;
	return true;
}

bool Hero::prehit() const {
	const Clip* c = _anim.current();
	if (!_blow.clip || c != _blow.clip) return false;
	const float hit = clipBlowTime(*c, _blow.combo);
	return hit >= 0.0f && _anim.time() < hit;
}

bool Hero::parrying() const {
	const Clip* c = _anim.current();
	return c && inParry(c->fight(), c->header().fps, _anim.time());
}

bool Hero::dodging() const {
	const Clip* c = _anim.current();
	if (!c) return false;
	bool hop = false;
	for (const Clip* j : _jumpBack) hop = hop || j == c;
	return hop && _anim.time() < c->duration() * 0.7f;
}

void Hero::hit(bool fromBehind) {
	if (!loaded() || _down) return;
	const Clip* c = _model.clip(fromBehind ? "T_STUMBLE" : "T_STUMBLEB");
	if (!c) c = _model.clip("T_GOTHIT");
	if (_drawTo != 0xFF) {
		_weapon = _drawTo; // (the hands have it now)
		_drawTo = 0xFF;
	}
	_blow.reset();
	cancelUse();
	if (!c || _body != GROUNDED) return;
	_anim.play(c, 0.05f);
	_reel = c;
	_roleNow = HIT;
}

void Hero::fall(bool dead, bool fromBehind) {
	if (!loaded()) return;
	const char* name = dead ? (fromBehind ? "T_DEAD" : "T_DEADB") : (fromBehind ? "T_STAND_2_WOUNDED" : "T_STAND_2_WOUNDEDB");
	const Clip* c = _model.clip(name);
	if (!c) c = _model.clip(dead ? "T_DEAD" : "T_STAND_2_WOUNDED");
	if (!c) c = _model.clip(dead ? "S_DEAD" : "S_WOUNDED");
	_down = true;
	cancelUse();
	_weapon = WEAPON_NONE;
	_drawTo = 0xFF;
	_blow.reset();
	_reel = nullptr;
	_sneak = false;
	if (c) _anim.play(c, 0.1f);
	_roleNow = DOWN;
}

void Hero::getUp() {
	if (!_down) return;
	_down = false;
	const Clip* cur = _anim.current();
	const Clip* up = nullptr;
	if (cur && (cur == _model.clip("S_WOUNDEDB") || cur == _model.clip("T_STAND_2_WOUNDEDB"))) up = _model.clip("T_WOUNDEDB_2_STAND");
	if (!up && cur && (cur == _model.clip("S_WOUNDED") || cur == _model.clip("T_STAND_2_WOUNDED"))) up = _model.clip("T_WOUNDED_2_STAND");
	if (up) {
		_anim.play(up, 0.1f);
		_reel = up;
	} else {
		_anim.play(stateClip(ST_RUN), 0.2f);
		_reel = nullptr;
	}
	_roleNow = IDLE;
}

// After the animation has moved on: the hands changing in a draw, a blow
// landing, a blow ending.
void Hero::fightTick(float prevTime, const Clip* prevClip) {
	const Clip* c = _anim.current();
	const float t = _anim.time();
	const bool same = c == prevClip;
	_blowLanded = false;
	if (_drawTo != 0xFF) {
		const bool past = (prevClip == _drawClip && (!same || (prevTime < _drawAt && t >= _drawAt))) || c != _drawClip || _drawAt <= 0.0f;
		if (past) _weapon = _drawTo;
		// The draw goes on through the clips the script lets follow it; it is
		// over when a state of standing or going is reached (or nothing follows).
		if ((c != _drawClip && (!c || c->loops() || stateOf(c) >= 0)) || _anim.finished()) {
			_weapon = _drawTo;
			_drawTo = 0xFF;
			_drawClip = nullptr;
			_forceNext = _anim.finished();
		}
	}
	if (_blow.clip) {
		if (c != _blow.clip) {
			_blow.reset();
		} else {
			const ClipFight* f = c->fight();
			const float fps = c->header().fps;
			const float hit = clipBlowTime(*c, _blow.combo);
			if (hit >= 0.0f && _blow.landed <= _blow.combo && t >= hit && (!same || prevTime < hit || prevTime > t)) {
				_blow.landed = u8(_blow.combo + 1);
				_blowLanded = true;
			}
			const float end = blowEnd(f, _blow.combo, fps);
			if ((end >= 0.0f && t >= end) || _anim.finished()) {
				// Not carried on: the attack stops where the blow ends.
				_blow.reset();
				_anim.play(stateClip(_walkMode ? ST_WALK : ST_RUN), 0.15f);
			}
		}
	}
	if (_reel && (c != _reel || _anim.finished())) _reel = nullptr;
}

// ---- using an item ----

namespace {

// The animations of an item's scheme: "T_<S>_STAND_2_S0" and its fellows.
const Clip* useClip(const Model& m, const char* pattern, const char* scheme) {
	char name[64];
	snprintf(name, sizeof(name), pattern, scheme);
	return m.clip(name);
}

} // namespace

bool Hero::canAnimateUse(const char* scheme) const {
	return loaded() && scheme && *scheme && useClip(_model, "T_%s_STAND_2_S0", scheme) != nullptr;
}

bool Hero::useItem(const char* scheme, float seconds, float moment) {
	const Clip *in = nullptr, *hold = nullptr, *out = nullptr;
	if (loaded() && scheme && *scheme) {
		in = useClip(_model, "T_%s_STAND_2_S0", scheme);
		hold = useClip(_model, "S_%s_S0", scheme);
		out = useClip(_model, "T_%s_S0_2_STAND", scheme);
	}
	return beginUse(in, hold, out, seconds, moment);
}

bool Hero::pickUp(float seconds, float moment) {
	if (!loaded()) return false;
	return beginUse(_model.clip("T_STAND_2_IGET"), _model.clip("S_IGET"), _model.clip("T_IGET_2_STAND"), seconds, moment);
}

bool Hero::beginUse(const Clip* in, const Clip* hold, const Clip* out, float seconds, float moment) {
	if (!loaded() || _down || _reel || _body != GROUNDED || _weapon != WEAPON_NONE || drawing() || usingItem()) return false;
	const Clip* cur = _anim.current();
	if (_blow.clip && cur == _blow.clip) return false;
	_use = Use();
	_sneak = false;
	_use.in = in;
	_use.hold = in ? hold : nullptr;
	_use.out = in ? out : nullptr;
	if (_use.in) {
		_use.phase = USE_IN;
		_use.at = _use.in->duration();
		_use.seconds = _use.at + (_use.hold ? 0.3f : 0.0f) + (_use.out ? _use.out->duration() : 0.0f);
		_anim.play(_use.in, 0.12f);
	} else {
		// No animation for it: standing still for as long as it takes.
		_use.phase = USE_TIMED;
		_use.seconds = max(seconds, 0.1f);
		_use.at = clamp(moment, 0.0f, _use.seconds);
		go(_walkMode ? ST_WALK : ST_RUN, true, 0.15f);
	}
	_roleNow = USE;
	return true;
}

float Hero::useProgress() const {
	if (!usingItem() || _use.seconds <= 0.0f) return 0.0f;
	return clamp(_use.time / _use.seconds, 0.0f, 1.0f);
}

void Hero::cancelUse() { _use = Use(); }

// After the animation has moved on: the item does its work when it is at
// the mouth (or after its time), and the character is free again when it
// is put down.
void Hero::useTick(float dt) {
	_use.moment = false;
	if (_use.phase == USE_NONE) return;
	_use.time += dt;
	const Clip* c = _anim.current();
	switch (_use.phase) {
	case USE_IN:
		if (c != _use.in || _anim.finished()) {
			_use.phase = USE_HOLD;
			_use.time = max(_use.time, _use.at);
			if (_use.hold) _anim.play(_use.hold, 0.1f);
		}
		break;
	case USE_HOLD:
		if (!_use.hold || _use.time >= _use.at + 0.3f) {
			_use.phase = USE_OUT;
			if (_use.out) _anim.play(_use.out, 0.1f);
		}
		break;
	case USE_OUT:
		if (!_use.out || c != _use.out || _anim.finished()) {
			_use.phase = USE_NONE;
			_forceNext = true;
		}
		break;
	case USE_TIMED:
		if (_use.time >= _use.seconds) {
			_use.phase = USE_NONE;
			_forceNext = true;
		}
		break;
	default: break;
	}
	const bool reached = _use.phase == USE_TIMED ? _use.time >= _use.at : _use.phase != USE_IN;
	if (!_use.done && reached) {
		// (A use whose animations ran out before its moment still works once.)
		_use.done = true;
		_use.moment = true;
	}
	if (_use.phase == USE_NONE) {
		const bool moment = _use.moment;
		_use = Use();
		_use.moment = moment;
	}
}

Drive Hero::control(const Input& in, const Player& player, float dt) {
	Drive drive;
	if (!loaded() || dt <= 0.0f) return drive;

	// ---- what the player asks for ----
	if (in.toggleWalk) _walkMode = !_walkMode;
	if (in.toggleSneak && _weapon == WEAPON_NONE) _sneak = !_sneak;
	const int fwd = in.moveZ > 0.3f ? 1 : in.moveZ < -0.3f ? -1 : 0;
	// A stick pushed only part of the way is a walk, too (with some slack,
	// so that a thumb resting on the border does not flicker between the two).
	// (How far the stick is pushed, whichever way: steering while running
	// must not slow the run down.)
	const float push = sqrtf(in.moveZ * in.moveZ + in.moveX * in.moveX);
	if (push > 0.8f) _slow = false;
	if (push < 0.68f) _slow = true;
	const bool walk = _walkMode || _sneak || (fwd > 0 && _slow);
	const int strafe = in.strafe > 0.5f ? 1 : in.strafe < -0.5f ? -1 : 0;
	const bool jumpPressed = in.jump && !_jumpHeld;
	_jumpHeld = in.jump;
	const bool locked = _down || _reel || (_blow.clip && _anim.current() == _blow.clip) || drawing() || usingItem();
	float turnRate = 0;
	if (in.steer) {
		// Towards a heading, as fast as a body turns (the console scheme).
		const float err = wrapAngle(in.steerYaw - _yaw);
		const float stepMax = kSteerSpeed * dt;
		const float step = locked ? 0.0f : clamp(err, -stepMax, stepMax);
		_yaw = wrapAngle(_yaw + step);
		turnRate = step / dt;
	} else {
		turnRate = clamp(in.moveX, -1.0f, 1.0f) * kTurnSpeed + clamp(in.lookX, -1.0f, 1.0f) * kLookTurnSpeed;
		if (locked && (_down || usingItem())) turnRate = 0;
		_yaw = wrapAngle(_yaw + turnRate * dt);
	}
	const bool turning = fabsf(turnRate) >= kTurnAnimSpeed;
	const int right = turnRate > 0 ? 1 : 0;
	const int r = row();

	// ---- which animation ----
	const Clip* cur = _anim.current();
	const bool force = _forceNext;
	_forceNext = false;
	if (_down || _reel || drawing() || (_blow.clip && cur == _blow.clip) || usingItem()) {
		// Down, reeling, taking a weapon in hand, swinging a blow, using an
		// item: the animation has the body until it is through.
		if (_down) _roleNow = DOWN;
		else if (usingItem()) _roleNow = USE;
		else if (_reel) _roleNow = HIT;
		else if (drawing()) _roleNow = DRAW;
		else _roleNow = ATTACK;
	} else {
		switch (_body) {
		case SWIMMING:
			if (fwd > 0) {
				go(ST_SWIMF, force, 0.3f);
				_roleNow = SWIM;
			} else if (fwd < 0 && _state[WEAPON_NONE][ST_SWIMB]) {
				go(ST_SWIMB, force, 0.3f);
				_roleNow = SWIM;
			} else if (turning && _turn[r][2][right] && !force) {
				_base = ST_SWIM;
				act(_turn[r][2][right], fadeOf(_turn[r][2][right]));
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
			const bool armed = _weapon != WEAPON_NONE;
			bool parryNow = false;
			if (armed && _parryHeld && _parryClip[r] && !wading && !player.climbing) {
				// Fending off, again and again while held.
				if (cur != _parryClip[r] || _anim.finished()) {
					if (cur == _parryClip[r]) _anim.restart(0.05f);
					else if (!busy || isTurn(cur)) _anim.play(_parryClip[r], 0.08f);
				}
				parryNow = _anim.current() == _parryClip[r];
			}
			if (parryNow) {
				_roleNow = PARRY;
			} else if (player.climbing) {
				go(idle, force);
				_roleNow = IDLE;
			} else if (jumpPressed && !busy && !wading) {
				// Off the ground: through the take-off the script has for where
				// the body is coming from.
				int from = curState;
				if (from < 0 || from == ST_WALK || from == ST_WALKL || from == ST_WALKBL || from == ST_WALKWL) from = STAND;
				Via via = _via[WEAPON_NONE][from][ST_JUMP];
				if (!via.clip && from == ST_RUN) via = _via[WEAPON_NONE][STAND][ST_JUMP];
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
					float pace = c && (c == _state[r][ST_RUNL] || c == _state[r][ST_WALKL] || c == _state[WEAPON_NONE][ST_RUNL] ||
					                   c == _state[WEAPON_NONE][ST_WALKL])
					                 ? c->header().moveSpeed
					                 : 0.0f;
					if (via.clip && via.clip->header().moveSpeed > pace) pace = via.clip->header().moveSpeed;
					drive.launch = true;
					drive.velocity = turned({0, kThrowUp, pace}, _yaw);
					_anim.play(via.clip ? via.clip : stateClip(ST_JUMP), 0.12f);
					_body = AIRBORNE;
					_jumping = true;
					_airTime = 0;
				}
				_roleNow = JUMP;
			} else if (strafe != 0 && _strafe[r][walk][strafe > 0] && (standing || busy || isStrafe(cur))) {
				// (The game's steps aside with a weapon in hand are loops: one
				// in play is no reason to stand first - that made the
				// character flicker between the two every frame.)
				act(_strafe[r][walk][strafe > 0], fadeOf(_strafe[r][walk][strafe > 0]));
				if (!busy) _roleNow = STRAFE;
			} else if (fwd > 0) {
				go(wading ? ST_WALKWL : walk ? ST_WALKL : ST_RUNL, force);
				if (!busy) _roleNow = (wading || walk) ? WALK : RUN;
			} else if (fwd < 0) {
				if (!armed && (walk || wading) && rowClip(r, ST_WALKBL) && _state[r][ST_WALKBL] != nullptr) {
					go(ST_WALKBL, force);
				} else if (!armed && (walk || wading) && _state[WEAPON_NONE][ST_WALKBL]) {
					go(ST_WALKBL, force);
				} else if (_jumpBack[r]) {
					// As in the original: "back" is a hop back (always, with a weapon in hand).
					if (standing) act(_jumpBack[r], fadeOf(_jumpBack[r]));
					else go(idle, force);
				} else {
					go(idle, force);
				}
				if (!busy) _roleNow = BACK;
			} else if (turning && standing && _turn[r][walk][right]) {
				_base = idle;
				act(_turn[r][walk][right], fadeOf(_turn[r][walk][right]));
				_roleNow = TURN;
			} else {
				go(idle, force);
				if (!busy) _roleNow = IDLE;
			}
			break;
		}
		}
	}

	// ---- on with it, and where it carries the body ----
	const float prevTime = _anim.time();
	const Clip* prevClip = _anim.current();
	_anim.update(dt);
	fightTick(prevTime, prevClip);
	useTick(dt);
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
			cancelUse();
			// (Nobody swims with a sword in hand.)
			_weapon = WEAPON_NONE;
			_drawTo = 0xFF;
			_blow.reset();
			_sneak = false;
		}
	} else if (player.grounded || player.climbing) {
		if (_body == AIRBORNE || _body == SWIMMING) {
			// Landed (or waded out): a transition that was still running is cut short.
			const Clip* cur = _anim.current();
			_forceNext = cur && !cur->loops() && !_anim.finished() && cur != _reel && !_down;
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
			_blow.reset();
			cancelUse();
		} else if (_body == SWIMMING) {
			_body = AIRBORNE;
			_airTime = 0;
		} else if (_body == TAKEOFF) {
			_leftGround = true;
		}
	}

	// A runner leans into the turn, by as much as the camera lags behind it
	// (at most 14.5 degrees, as in the original).
	const Clip* runL = _state[row()][ST_RUNL] ? _state[row()][ST_RUNL] : _state[WEAPON_NONE][ST_RUNL];
	const float lean = (_body == GROUNDED && _anim.current() == runL) ? clamp(_turnLag, -0.253f, 0.253f) : 0.0f;
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
