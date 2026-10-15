#include "player.h"

namespace g3 {

void Player::spawn(Vec3 eye) {
	feet = {eye.x, eye.y - config.eyeHeight, eye.z};
	_eyeY = eye.y;
	velocityY = 0;
	grounded = false;
	swimming = false;
	climbing = false;
	_lad = LadderRun();
	_letGo = false;
	waterDepth = 0;
	_placed = false;
	_carry = {0, 0, 0};
	_sliding = _resting = false;
	_airTime = 0;
	_airFrom = feet;
	_step = '-';
}

void Player::collideWalls(const World& world) {
	// Two spheres stand in for the body. The lower one starts at step height,
	// so ledges up to that height do not block and gentle slopes never touch it;
	// anything steeper or taller pushes the player back.
	const float r = config.radius;
	const float heights[2] = {config.stepHeight + r, config.eyeHeight - r};
	for (int pass = 0; pass < 3; ++pass) {
		bool moved = false;
		for (float h : heights) {
			Vec3 c = {feet.x, feet.y + h, feet.z};
			if (world.pushSphere(c, r)) {
				if (_pushes < 255) ++_pushes;
				feet.x = c.x;
				feet.z = c.z;
				// A push down means the head hit something: stop rising.
				if (c.y < feet.y + h - 0.01f && velocityY > 0) velocityY = 0;
				moved = true;
			}
		}
		if (!moved) break;
	}
}

// Handles deep water. Returns true if the player is afloat this frame, which
// replaces gravity and the floor logic.
bool Player::swim(const World& world, float dt) {
	const PlayerConfig& cfg = config;
	float surface;
	const float reach = cfg.eyeHeight + 150.0f;
	const bool water = world.waterBelow({feet.x, feet.y + reach, feet.z}, reach + 20.0f, surface);
	waterDepth = water ? max(surface - feet.y, 0.0f) : 0.0f;
	// A little hysteresis keeps the body from bobbing between the two states.
	if (waterDepth <= cfg.swimDepth - (swimming ? 30.0f : 0.0f)) {
		swimming = false;
		return false;
	}

	const float target = surface - cfg.swimDepth; // where the feet float
	float y;
	Vec3 n;
	// Shore: a floor the player can stand on with the head above the water.
	if (world.rayDown({feet.x, max(feet.y, target) + cfg.stepHeight, feet.z}, cfg.stepHeight + 40.0f, y, n) &&
	    y >= target && n.y >= cfg.minFloorNormal) {
		feet.y = y;
		grounded = true;
		swimming = false;
		velocityY = 0;
		waterDepth = max(surface - y, 0.0f);
		return true;
	}
	swimming = true;
	grounded = false;
	velocityY = 0;
	feet.y += clamp(target - feet.y, -cfg.floatSpeed * dt, cfg.floatSpeed * dt);
	return true;
}

bool Player::floorAbove(const World& world, float reach, float& y) const {
	// Upwards in windows of half a metre: the first one with a surface in it
	// holds the lowest surface above (more or less: the topmost of that window).
	Vec3 n;
	const float from = feet.y + config.stepHeight - 1.0f;
	for (float h = 50.0f;; h += 50.0f) {
		h = min(h, reach);
		if (world.rayDown({feet.x, from + h, feet.z}, h, y, n)) return true;
		if (h >= reach) return false;
	}
}

void Player::standAt(float y) {
	feet.y = y;
	grounded = true;
	velocityY = 0;
	_carry = {0, 0, 0};
	_sliding = false;
	// On whatever slope that is, as on the floor a slide came to rest on.
	_resting = true;
	_restY = y;
}

// The safety net under all of the above: a body that has been off the ground
// for 1.5 s and has not got 30 cm anywhere in that time is stuck, whatever
// the cause (a crease the slide cannot leave, feet caught in the rock, a
// take-off that never ends). It is stood on the nearest floor - below it, or
// failing that above it - and can walk off from there.
void Player::unstick(const World& world, float dt) {
	if (grounded || swimming || climbing || flying || !_placed || _mantle) {
		_airTime = 0;
		_airFrom = feet;
		return;
	}
	_airTime += dt;
	if (_airTime < kStuckTime) return;
	const bool still = length(feet - _airFrom) < kStuckDistance;
	_airTime = 0;
	_airFrom = feet;
	if (!still) return;
	float y;
	Vec3 n;
	if (world.rayDown({feet.x, feet.y + config.stepHeight, feet.z}, config.stepHeight * 1.8f, y, n) || floorAbove(world, 400.0f, y)) {
		standAt(y);
		++unstuck;
		_step = 'k';
	}
	// (Nothing anywhere near: the edge of the loaded world. Wait for it.)
}

// ---- ladders ----
//
// A ladder is a line of rungs from its foot to its head, climbed from one side of it, the body
// hanging a little off the rungs and facing them. What the bake knew of the ladder itself (its
// line, which way its face looks, how wide and deep it is: World::ladderInfo) is taken where there
// is such a thing; else the line is the middle of the climbing box (the ladder plus the room the
// bake gave it round about and above: LADDER_BOX_REACH, LADDER_BOX_TOP), its face across the box's
// narrower side. The side it is climbed from is where the climber stands at its foot; at its top,
// the side away from the floor there.

namespace {

struct LadderShape {
	Vec3 foot, head;
	Vec3 n;      // horizontal: the way the face looks (either side); {0,0,0}: not known
	float halfWidth, halfDepth;
	int side;    // +1 / -1: climbed from that side of n (the bake's word), 0: not known
};

LadderShape ladderShape(const World& world, u32 i) {
	LadderShape s {};
	if (const LadderInfo* li = world.ladderInfo(i)) {
		s.foot = {li->foot[0], li->foot[1], li->foot[2]};
		s.head = {li->head[0], li->head[1], li->head[2]};
		s.n = {li->normal[0], 0.0f, li->normal[1]};
		const float len = sqrtf(s.n.x * s.n.x + s.n.z * s.n.z);
		s.n = len > 0.01f ? s.n * (1.0f / len) : Vec3 {0, 0, 0};
		s.halfWidth = li->halfWidth;
		s.halfDepth = min(li->halfDepth, 40.0f);
		s.side = li->side;
		return s;
	}
	const WorldLadder& b = world.ladder(i);
	float lo[3], hi[3];
	for (int a = 0; a < 3; ++a) {
		const float grow = a == 1 ? 0.0f : LADDER_BOX_REACH;
		lo[a] = b.bbMin[a] + grow;
		hi[a] = b.bbMax[a] - (a == 1 ? LADDER_BOX_TOP : grow);
		if (hi[a] < lo[a]) lo[a] = hi[a] = 0.5f * (b.bbMin[a] + b.bbMax[a]);
	}
	const float cx = 0.5f * (lo[0] + hi[0]), cz = 0.5f * (lo[2] + hi[2]);
	const float ex = hi[0] - lo[0], ez = hi[2] - lo[2];
	s.foot = {cx, lo[1], cz};
	s.head = {cx, max(hi[1], lo[1] + 1.0f), cz};
	const float wide = max(ex, ez), deep = min(ex, ez);
	s.halfWidth = max(0.5f * wide, 20.0f);
	s.halfDepth = min(0.5f * deep, 25.0f);
	// A box turned on the slant (or a ladder leaning far) says nothing of the way the face looks.
	s.n = deep < 0.6f * wide ? (ex >= ez ? Vec3 {0, 0, 1} : Vec3 {1, 0, 0}) : Vec3 {0, 0, 0};
	s.side = 0;
	return s;
}

Vec3 flat(Vec3 v) { return {v.x, 0.0f, v.z}; }

Vec3 flatNormalized(Vec3 v, Vec3 otherwise) {
	const float len = sqrtf(v.x * v.x + v.z * v.z);
	return len > 0.01f ? Vec3 {v.x / len, 0.0f, v.z / len} : otherwise;
}

// The middle of the rungs at height y.
Vec3 lineAt(const LadderShape& s, float y) {
	const float t = s.head.y > s.foot.y ? clamp((y - s.foot.y) / (s.head.y - s.foot.y), 0.0f, 1.0f) : 0.0f;
	return {s.foot.x + (s.head.x - s.foot.x) * t, y, s.foot.z + (s.head.z - s.foot.z) * t};
}

float smooth01(float t) {
	t = clamp(t, 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

float wrapPi(float a) {
	while (a > 3.14159265f) a -= 6.2831853f;
	while (a < -3.14159265f) a += 6.2831853f;
	return a;
}

// The floor at a ladder's top, on the side away from the climber: where one steps off. `out` from the rungs to the climber.
bool topFloor(const World& world, const LadderShape& s, Vec3 out, float minNormal, Vec3& at) {
	static const float kAhead[4] = {40.0f, 60.0f, 90.0f, 130.0f};
	for (float d : kAhead) {
		const float px = s.head.x - out.x * (s.halfDepth + d), pz = s.head.z - out.z * (s.halfDepth + d);
		float y;
		Vec3 n;
		if (!world.rayDown({px, s.head.y + 150.0f, pz}, 380.0f, y, n)) continue;
		if (y < s.head.y - 230.0f || y > s.head.y + 80.0f || n.y < minNormal) continue;
		at = {px, y, pz};
		return true;
	}
	return false;
}

// Is there floor near the top of the ladder on this side of it (the side `dir` points to)?
bool floorOnSide(const World& world, const LadderShape& s, Vec3 dir) {
	static const float kAhead[2] = {45.0f, 80.0f};
	for (float d : kAhead) {
		float y;
		Vec3 n;
		const float px = s.head.x + dir.x * (s.halfDepth + d), pz = s.head.z + dir.z * (s.halfDepth + d);
		if (world.rayDown({px, s.head.y + 120.0f, pz}, 330.0f, y, n) && y >= s.head.y - 230.0f && y <= s.head.y + 60.0f) return true;
	}
	return false;
}

} // namespace

float Player::ladderYaw() const {
	if (!climbing) return _lad.yaw;
	if (_lad.phase == LADDER_MOUNT || _lad.phase == LADDER_MOUNT_TOP) {
		const float e = smooth01(_lad.length > 0.0f ? _lad.time / (0.6f * _lad.length) : 1.0f);
		return wrapPi(_lad.yawFrom + wrapPi(_lad.yaw - _lad.yawFrom) * e);
	}
	return _lad.yaw;
}

Vec3 Player::ladderSpot(float y) const {
	const float t = _lad.head.y > _lad.foot.y ? clamp((y - _lad.foot.y) / (_lad.head.y - _lad.foot.y), 0.0f, 1.0f) : 0.0f;
	return {_lad.foot.x + (_lad.head.x - _lad.foot.x) * t + _lad.out.x * _lad.hang, y,
	        _lad.foot.z + (_lad.head.z - _lad.foot.z) * t + _lad.out.z * _lad.hang};
}

// How far below the floor at the top the way over the edge begins (the hands at the top rung).
float Player::ladderExitStart() const { return clamp(ladderExitRise > 1.0f ? ladderExitRise : 120.0f, 60.0f, 200.0f); }

void Player::ladderPhaseTo(u8 phase, float length, Vec3 to) {
	_lad.phase = phase;
	_lad.time = 0;
	_lad.length = length;
	_lad.from = feet;
	_lad.to = to;
}

void Player::ladderOff(bool ground) {
	climbing = false;
	_lad.phase = LADDER_OFF;
	_lad.length = 0;
	velocityY = 0;
	_carry = {0, 0, 0};
	grounded = ground;
	_sliding = _resting = false;
}

bool Player::ladderBegin(const World& world, int index, bool fromTop, float yaw, Vec3 dir, bool glideOnly) {
	if (index < 0 || u32(index) >= world.ladderCount()) return false;
	const LadderShape s = ladderShape(world, u32(index));
	const Vec3 facing = {sinf(yaw), 0.0f, cosf(yaw)};
	// The side it is climbed from.
	Vec3 out;
	const Vec3 rel = flat(feet - lineAt(s, feet.y));
	if (s.n.x == 0.0f && s.n.z == 0.0f) {
		// Which way the face looks is not known: from where the climber is (at the top: the way he goes).
		out = fromTop ? flatNormalized(dir, facing) : flatNormalized(rel, facing * -1.0f);
	} else if (!fromTop) {
		const float d = dot(rel, s.n);
		out = s.n * (fabsf(d) > s.halfDepth + 5.0f || s.side == 0 ? (d >= 0.0f ? 1.0f : -1.0f) : float(s.side));
	} else {
		// From the top: away from the floor there (that side is the one the ladder leans on).
		const bool plus = floorOnSide(world, s, s.n), minus = floorOnSide(world, s, s.n * -1.0f);
		if (plus != minus) out = s.n * (plus ? -1.0f : 1.0f);
		else if (s.side != 0) out = s.n * float(s.side);
		else out = s.n * (dot(dir, s.n) >= 0.0f ? 1.0f : -1.0f);
	}
	_lad = LadderRun();
	_lad.index = index;
	_lad.foot = s.foot;
	_lad.head = s.head;
	_lad.out = out;
	_lad.hang = s.halfDepth + 32.0f;
	_lad.yawFrom = yaw;
	_lad.yaw = atan2f(-out.x, -out.z);
	_lad.top = topFloor(world, s, out, config.minFloorNormal, _lad.topAt);
	climbing = true;
	grounded = false;
	velocityY = 0;
	_carry = {0, 0, 0};
	_sliding = _resting = false;
	_letGo = false;
	if (fromTop && !glideOnly) {
		// Over the edge backwards and down to the top rungs.
		const float topY = _lad.top ? _lad.topAt.y : feet.y;
		float y = topY - ladderExitStart();
		y = clamp(y, _lad.foot.y, max(_lad.head.y - 60.0f, _lad.foot.y));
		ladderPhaseTo(LADDER_MOUNT_TOP, clamp(ladderMountTopTime > 0.0f ? ladderMountTopTime : 0.6f, 0.25f, 2.5f), ladderSpot(y));
	} else {
		// Onto the rungs where the body is (from the foot: as it stands; caught in the air: where it hangs).
		const float y = clamp(feet.y, _lad.foot.y, max(_lad.head.y - 60.0f, _lad.foot.y));
		const float t = glideOnly ? 0.25f : clamp(ladderMountTime > 0.0f ? ladderMountTime : 0.35f, 0.2f, 2.5f);
		ladderPhaseTo(LADDER_MOUNT, t, ladderSpot(glideOnly ? feet.y : y));
	}
	return true;
}

int Player::ladderInReach(const World& world, float yaw, bool& fromTop) const {
	fromTop = false;
	if (climbing || flying || swimming || !grounded) return -1;
	int best = -1;
	float bestD = 1e30f;
	for (u32 i = 0; i < world.ladderCount(); ++i) {
		const WorldLadder& b = world.ladder(i);
		if (feet.x < b.bbMin[0] - 120.0f || feet.x > b.bbMax[0] + 120.0f || feet.z < b.bbMin[2] - 120.0f || feet.z > b.bbMax[2] + 120.0f ||
		    feet.y < b.bbMin[1] - 120.0f || feet.y > b.bbMax[1] + 60.0f)
			continue;
		const LadderShape s = ladderShape(world, i);
		bool top;
		if (fabsf(feet.y - s.foot.y) < 90.0f) top = false;
		else if (feet.y >= s.head.y - 240.0f && feet.y <= s.head.y + 60.0f && s.head.y - s.foot.y > 150.0f) top = true;
		else continue;
		const Vec3 p = lineAt(s, top ? s.head.y : feet.y);
		const Vec3 d = flat(feet - p);
		float dist;
		if (s.n.x != 0.0f || s.n.z != 0.0f) {
			const Vec3 across = {s.n.z, 0.0f, -s.n.x};
			const float along = fabsf(dot(d, s.n)), side = fabsf(dot(d, across));
			if (side > s.halfWidth + 45.0f || along > s.halfDepth + (top ? 130.0f : 110.0f)) continue;
			dist = along + side;
		} else {
			dist = sqrtf(d.x * d.x + d.z * d.z);
			if (dist > 120.0f) continue;
		}
		// In front of the character (close by, any way).
		if (dist > 45.0f) {
			const float a = wrapPi(atan2f(-d.x, -d.z) - yaw);
			if (fabsf(a) > (top ? 1.9f : 1.2f)) continue;
		}
		if (dist < bestD) {
			bestD = dist;
			best = int(i);
			fromTop = top;
		}
	}
	return best;
}

bool Player::grabLadder(const World& world, int index, bool fromTop, float yaw) {
	if (climbing || flying || swimming || !grounded || index < 0) return false;
	return ladderBegin(world, index, fromTop, yaw, {sinf(yaw), 0.0f, cosf(yaw)}, false);
}

bool Player::ladderPoint(const World& world, int index, float y, Vec3& out) const {
	if (index < 0 || u32(index) >= world.ladderCount()) return false;
	const LadderShape s = ladderShape(world, u32(index));
	out = lineAt(s, clamp(y, s.foot.y, s.head.y));
	return true;
}

// Off the ground onto a ladder by itself: walking into it at its foot (facing it), or caught on it
// in the air (stepping off its top, a jump at it).
bool Player::ladderCatch(const World& world, const Input& in, float yaw) {
	if (!world.ladderCount()) return false;
	const WorldLadder* box = world.ladderAt({feet.x, feet.y + 90.0f, feet.z});
	if (!box) {
		_letGo = false;
		return false;
	}
	if (grounded) _letGo = false;
	if (_letGo || in.jump) return false;
	const int index = int(box - &world.ladder(0));
	const LadderShape s = ladderShape(world, u32(index));
	const Vec3 facing = {sinf(yaw), 0.0f, cosf(yaw)};
	if (grounded) {
		if (in.moveZ <= 0.1f || feet.y > s.head.y - 150.0f) return false;
		const Vec3 d = flat(lineAt(s, feet.y) - feet);
		const float dist = sqrtf(d.x * d.x + d.z * d.z);
		if (dist > 5.0f && dot(d, facing) < 0.6f * dist) return false; // not going at it
		if (s.n.x != 0.0f || s.n.z != 0.0f) {
			const Vec3 across = {s.n.z, 0.0f, -s.n.x};
			if (fabsf(dot(d, across)) > s.halfWidth + 25.0f || fabsf(dot(d, s.n)) > s.halfDepth + 75.0f) return false;
		} else if (dist > 90.0f) {
			return false;
		}
		return ladderBegin(world, index, false, yaw, facing, false);
	}
	// In the air: only falling, well above its foot, with nothing to land on just below.
	float y;
	Vec3 n;
	if (velocityY > 0.0f || feet.y < s.foot.y + 100.0f) return false;
	if (world.rayDown({feet.x, feet.y + config.stepHeight, feet.z}, config.stepHeight + 100.0f, y, n)) return false;
	const Vec3 dir = flatNormalized(feet - _airFrom, facing);
	return ladderBegin(world, index, true, yaw, dir, true);
}

void Player::ladderTick(const World& world, const Input& in, float dt) {
	const PlayerConfig& cfg = config;
	velocityY = 0;
	_carry = {0, 0, 0};
	grounded = false;
	_lad.time += dt;
	const float e = smooth01(_lad.length > 0.0f ? _lad.time / _lad.length : 1.0f);
	switch (_lad.phase) {
	case LADDER_MOUNT:
	case LADDER_MOUNT_TOP:
		feet = _lad.from + (_lad.to - _lad.from) * e;
		if (_lad.time >= _lad.length) {
			feet = _lad.to;
			_lad.phase = LADDER_CLIMB;
			_lad.length = 0;
		}
		return;
	case LADDER_EXIT_TOP: {
		// Up over the edge: up first, then onto the floor.
		const float up = smooth01(_lad.length > 0.0f ? _lad.time / (0.7f * _lad.length) : 1.0f);
		const float on = smooth01(_lad.length > 0.0f ? (_lad.time - 0.3f * _lad.length) / (0.7f * _lad.length) : 1.0f);
		feet.y = _lad.from.y + (_lad.to.y - _lad.from.y) * up;
		feet.x = _lad.from.x + (_lad.to.x - _lad.from.x) * on;
		feet.z = _lad.from.z + (_lad.to.z - _lad.from.z) * on;
		if (_lad.time >= _lad.length) {
			feet = _lad.to;
			float fy;
			Vec3 fn;
			if (world.rayDown({feet.x, feet.y + cfg.stepHeight, feet.z}, cfg.stepHeight + 40.0f, fy, fn)) feet.y = fy;
			ladderOff(true);
		}
		return;
	}
	case LADDER_EXIT_FOOT:
		if (_lad.time >= _lad.length) ladderOff(true);
		else grounded = false;
		return;
	default: break;
	}
	// On the rungs.
	if (in.jump) {
		// Jumping lets go, and the ladder stays let go until the body has landed or left it.
		ladderOff(false);
		_letGo = true;
		return;
	}
	const float dy = clamp(in.moveZ, -1.0f, 1.0f) * cfg.climbSpeed * dt;
	float y;
	Vec3 n;
	if (dy < 0.0f && world.rayDown({feet.x, feet.y + cfg.stepHeight, feet.z}, cfg.stepHeight - dy + 1.0f, y, n) && n.y >= cfg.minFloorNormal) {
		// Down at the foot: off, onto the floor.
		feet.y = y;
		ladderPhaseTo(LADDER_EXIT_FOOT, clamp(ladderExitFootTime > 0.0f ? ladderExitFootTime : 0.25f, 0.15f, 2.0f), feet);
		return;
	}
	if (dy > 0.0f) {
		if (_lad.top && feet.y + dy >= _lad.topAt.y - ladderExitStart()) {
			// At the top: over the edge onto the floor there.
			ladderPhaseTo(LADDER_EXIT_TOP, clamp(ladderExitTopTime > 0.0f ? ladderExitTopTime : 0.6f, 0.3f, 2.5f), _lad.topAt);
			return;
		}
		// A ceiling over the ladder (or its end, with no floor to step onto) ends the climb.
		if (!_lad.top && feet.y + dy > _lad.head.y - 120.0f) return;
		const float r = cfg.radius;
		Vec3 head = {feet.x, feet.y + dy + cfg.eyeHeight - r, feet.z};
		const float headY = head.y;
		if (world.pushSphere(head, r) && head.y < headY - 0.5f) return;
	}
	const Vec3 at = ladderSpot(feet.y + dy);
	feet = at;
}

// Where a jump's hands find a ledge. The floor is felt for ahead of the body
// from above (rayDown finds the highest surface under the point): where the
// first point with a floor between the two heights follows a point without
// one (or a much lower one), there is a wall between them, and its edge.
bool Player::probeLedge(const World& world, float yaw, Ledge& out) const {
	out = Ledge();
	const PlayerConfig& cfg = config;
	LedgeStats& st = _ledgeStats;
	++st.probes;
	st.why = 1; // nothing in reach ahead
	const Vec3 dir = {sinf(yaw), 0, cosf(yaw)};
	const float top = feet.y + cfg.ledgeReach + 15.0f;
	const float drop = cfg.ledgeReach + 15.0f + 140.0f;
	const float from = cfg.radius - 8.0f, to = cfg.radius + cfg.ledgeRange;
	// The floor ahead, felt for from above, step by step. The edge is where it
	// first stands clearly above the feet - a wall or a steep rock face, not
	// only a sheer one (the original's climb takes hold of a rough slope too).
	float ys[16];
	bool hits[16];
	int count = 0;
	for (float d = from; d <= to && count < 16; d += 10.0f, ++count) {
		Vec3 n;
		hits[count] = world.rayDown({feet.x + dir.x * d, top, feet.z + dir.z * d}, drop, ys[count], n);
	}
	int edge = -1;
	for (int k = 0; k < count && edge < 0; ++k) {
		if (!hits[k]) continue;
		const float rel = ys[k] - feet.y;
		if (rel > cfg.ledgeReach) {
			st.why = 5; // too high
			st.height = rel;
			break;
		}
		if (rel < cfg.ledgeMin) continue;
		st.height = rel;
		// Steep: a rise of a body's step within a hand's breadth or two (a slope the
		// character could walk up is no ledge).
		const float before = k >= 3 && hits[k - 3] ? ys[k - 3] : (k >= 1 && hits[k - 1] ? ys[k - 1] : feet.y);
		if (ys[k] - before < 30.0f) {
			st.why = 2; // a slope up, or already over it
			continue;
		}
		edge = k;
	}
	if (edge < 0) return false;
	const float d = from + 10.0f * float(edge), y = ys[edge];
	// The place on top: a body's width in from the edge or farther (rock is rough, and a slope
	// goes on up), on a floor a body can stand on, with room.
	const float r = cfg.radius;
	const float heights[2] = {cfg.stepHeight + r, cfg.eyeHeight - r};
	static const float kInside[6] = {22.0f, 40.0f, 60.0f, 80.0f, 100.0f, 130.0f};
	st.why = 3;
	for (float inside : kInside) {
		const float inD = d + inside;
		const Vec3 at = {feet.x + dir.x * inD, 0, feet.z + dir.z * inD};
		float y2;
		Vec3 n2;
		if (!world.rayDown({at.x, top + 40.0f, at.z}, drop + 40.0f, y2, n2) || y2 < y - 12.0f || y2 - feet.y > cfg.ledgeReach + 40.0f || n2.y < 0.45f) continue;
		bool room = true;
		for (float h : heights) {
			Vec3 c = {at.x, y2 + h, at.z};
			const Vec3 c0 = c;
			if (world.pushSphere(c, r) && (fabsf(c.x - c0.x) + fabsf(c.z - c0.z) > 22.0f || c.y < c0.y - 12.0f)) room = false;
		}
		if (!room) {
			st.why = 4;
			continue;
		}
		out.found = true;
		out.top = {at.x, y2, at.z};
		out.height = y2 - feet.y;
		out.gap = max(d - 5.0f - cfg.radius, 0.0f);
		out.yaw = yaw;
		++st.found;
		st.why = 0;
		return true;
	}
	return false;
}

void Player::walk(const World& world, const Input& in, float yaw, float dt, const Drive* drive) {
	const PlayerConfig& cfg = config;

	// The ground under the spawn point may still be streaming in.
	if (!_placed) {
		float y;
		Vec3 n;
		_step = '-';
		if (!world.rayDown({feet.x, feet.y + 200.0f, feet.z}, 3000.0f, y, n)) return;
		feet.y = y;
		grounded = true;
		velocityY = 0;
		_placed = true;
	}

	// A climb onto a ledge: the animation has the body.
	_mantle = drive && drive->place;
	if (_mantle) {
		feet = drive->position;
		velocityY = 0;
		_carry = {0, 0, 0};
		climbing = swimming = false;
		_sliding = false;
		_blocked = false;
		_step = 'm';
		grounded = drive->land;
		if (drive->land) {
			_resting = false;
			_airTime = 0;
		}
		return;
	}

	// On a ladder: the ladder has the body.
	if (climbing) {
		_blocked = false;
		ladderTick(world, in, dt);
		_step = 'c';
		return;
	}

	// Horizontal move in small steps so thin walls cannot be skipped.
	const float forward = in.moveZ;
	const float side = drive ? 0.0f : in.moveX; // (a driven character turns with it instead)
	Vec3 wish = {sinf(yaw) * forward + cosf(yaw) * side, 0, cosf(yaw) * forward - sinf(yaw) * side};
	const float len = length(wish);
	if (len > 1.0f) wish = wish * (1.0f / len);
	float speed = in.fast ? cfg.fastSpeed : cfg.walkSpeed;
	if (swimming)
		speed = min(speed, cfg.swimSpeed);
	else if (waterDepth > cfg.wadeDepth)
		speed *= cfg.wadeFactor;
	Vec3 delta = wish * (speed * dt);
	if (drive) {
		if (grounded || swimming || drive->lift) {
			// On its feet (or carried by a jump's take-off): where the animation goes.
			delta = {drive->step.x, 0, drive->step.z};
			if (dt > 0) _carry = delta * (1.0f / dt);
		} else {
			// In the air: on with what it had.
			delta = _carry * dt;
		}
	}
	const float dist = length(delta);
	const int steps = clamp(int(dist / (cfg.radius * 0.5f)) + 1, 1, 12);
	const Vec3 from = feet;
	for (int i = 0; i < steps; ++i) {
		feet.x += delta.x / float(steps);
		feet.z += delta.z / float(steps);
		collideWalls(world);
	}
	// Did something stop the move?
	const float gained = (feet.x - from.x) * delta.x + (feet.z - from.z) * delta.z;
	_blocked = dist > 0.01f && gained < 0.5f * dist * dist;

	if (swim(world, dt)) {
		climbing = false;
		_step = 'w';
		return;
	}
	if (ladderCatch(world, in, yaw)) {
		_step = 'c';
		return;
	}

	float y;
	Vec3 n;
	if (drive) {
		if (drive->launch) {
			velocityY = drive->velocity.y;
			_carry = {drive->velocity.x, 0, drive->velocity.z};
			grounded = false;
		} else if (drive->lift) {
			// The animation says how high. Rising, the spheres keep the body
			// out of walls and ceilings; coming down, the floor ends it.
			const float dy = drive->step.y;
			velocityY = 0;
			_step = 'l';
			if (dy > 0.01f) {
				feet.y += dy;
				grounded = false;
				collideWalls(world);
			} else if (world.rayDown({feet.x, feet.y + cfg.stepHeight, feet.z}, cfg.stepHeight - dy + 1.0f, y, n)) {
				if (standsOn(n, y)) {
					feet.y = y;
					grounded = true;
				} else {
					// A slope too steep to land on: not into it, at least.
					feet.y = max(feet.y + dy, y);
					grounded = false;
					_step = 'L';
				}
			} else {
				feet.y += dy;
				grounded = false;
			}
			return;
		}
	} else if (grounded && in.jump) {
		velocityY = cfg.jumpSpeed;
		grounded = false;
	}

	if (grounded) {
		// Stay glued to the floor: up a step, or down a slope or small ledge.
		const float snap = cfg.stepHeight * 0.8f;
		if (world.rayDown({feet.x, feet.y + cfg.stepHeight, feet.z}, cfg.stepHeight + snap, y, n) && standsOn(n, y)) {
			feet.y = y;
			_step = 'g';
			if (n.y >= cfg.minFloorNormal) _resting = false;
		} else {
			grounded = false;
			velocityY = 0;
			_resting = false;
		}
	}
	if (grounded) _sliding = false;
	if (!grounded) {
		velocityY = max(velocityY - (drive ? cfg.driveGravity : cfg.gravity) * dt, -4000.0f);
		const float dy = velocityY * dt;
		if (dy > 0) {
			feet.y += dy; // rising: walls and ceilings are handled by the spheres
			collideWalls(world);
			_step = 'u';
		} else if (world.rayDown({feet.x, feet.y + cfg.stepHeight, feet.z}, cfg.stepHeight - dy, y, n)) {
			feet.y = y;
			if (n.y >= cfg.minFloorNormal) {
				_step = 'n';
				grounded = true;
				velocityY = 0;
				_sliding = _resting = false;
			} else {
				// Too steep to stand on: slide downhill. Whatever pace the
				// body had - a jump's, at the slope - is lost on it: else it
				// keeps pressing the body up the slope it slides down, and it
				// hangs there, sliding for ever.
				_carry = {0, 0, 0};
				_step = 's';
				// The slide has a pace of its own that gathers downhill: in a
				// gully between two steep faces the pushes across it cancel
				// out against the rock, and what is left takes the body down
				// along the gully (it used to rock from side to side there,
				// hardly getting any lower).
				if (!_sliding) _slideVel = {n.x * 400.0f, 0, n.z * 400.0f};
				_slideVel.x += n.x * 1600.0f * dt;
				_slideVel.z += n.z * 1600.0f * dt;
				const float pace = sqrtf(_slideVel.x * _slideVel.x + _slideVel.z * _slideVel.z);
				if (pace > 700.0f) _slideVel = _slideVel * (700.0f / pace);
				feet.x += _slideVel.x * dt;
				feet.z += _slideVel.z * dt;
				velocityY = max(velocityY, -600.0f);
				const Vec3 slid = feet;
				collideWalls(world);
				// Whatever ran into rock is stopped: only the pace along it stays.
				const float px = feet.x - slid.x, pz = feet.z - slid.z, pushed = sqrtf(px * px + pz * pz);
				if (pushed > 0.01f) {
					const float into = (_slideVel.x * px + _slideVel.z * pz) / pushed;
					if (into < 0.0f) {
						_slideVel.x -= px / pushed * into;
						_slideVel.z -= pz / pushed * into;
					}
				}
				if (!_sliding || feet.y < _slideLow - 2.0f) {
					_sliding = true;
					_slideLow = feet.y;
					_slideStill = 0;
				} else if ((_slideStill += dt) > 0.25f) {
					// It gets no lower: it stands.
					_step = 'r';
					grounded = true;
					velocityY = 0;
					_sliding = false;
					_resting = true;
					_restY = feet.y;
				}
			}
		} else if (world.rayDown({feet.x, feet.y + cfg.stepHeight, feet.z}, 50000.0f, y, n)) {
			feet.y += dy;
			_step = 'f';
		} else if (floorAbove(world, cfg.eyeHeight, y)) {
			// Nothing below, but rock above: the feet have got into the rock
			// (the spheres keep the body out of walls only from step height
			// up; a jump at a steep rock face can put the feet into it). The
			// game's terrain is a single sheet with nothing under it, so this
			// used to hover here for ever. Out on top of it.
			standAt(y);
			++popUps;
			_step = 'p';
		} else {
			// Nothing below at all (edge of the loaded world): hover instead of
			// falling forever.
			velocityY = 0;
			_step = 'h';
		}
	}
}

void Player::update(const World& world, const Input& in, float yaw, float pitch, float dt, const Drive* drive) {
	if (in.toggleFly) {
		flying = !flying;
		velocityY = 0;
		grounded = false;
		swimming = false;
		climbing = false;
		_lad.phase = LADDER_OFF;
	}

	_pushes = 0;
	if (flying) {
		_step = 'F';
		_airTime = 0;
		const float speed = (in.fast ? config.flyFastSpeed : config.flySpeed) * dt;
		const Vec3 f = forwardFromYawPitch(yaw, pitch);
		const Vec3 r = normalize(cross({0, 1, 0}, f));
		feet = feet + f * (in.moveZ * speed) + r * (in.moveX * speed) + Vec3 {0, in.moveY * speed, 0};
		_eyeY = feet.y + config.eyeHeight;
		_placed = true;
		waterDepth = 0;
		return;
	}

	walk(world, in, yaw, dt, drive);
	unstick(world, dt);
	// Smooth the eye over steps and landings instead of snapping.
	const float target = feet.y + config.eyeHeight;
	const float blend = min(dt * 14.0f, 1.0f);
	_eyeY += (target - _eyeY) * blend;
	if (_eyeY < target - 60.0f) _eyeY = target - 60.0f;
	if (_eyeY > target + 60.0f) _eyeY = target + 60.0f;
}

} // namespace g3
