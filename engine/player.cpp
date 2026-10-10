#include "player.h"

namespace g3 {

void Player::spawn(Vec3 eye) {
	feet = {eye.x, eye.y - config.eyeHeight, eye.z};
	_eyeY = eye.y;
	velocityY = 0;
	grounded = false;
	swimming = false;
	climbing = false;
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

// Ladders work like in the old shooters: inside a ladder's box, forward
// means up and backward means down, and gravity is off. The box reaches a
// little above the ladder, so at the top the way forward is free again and
// the player steps over the edge. Returns true if the player is on a ladder
// this frame.
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

bool Player::climb(const World& world, const Input& in, float dt) {
	const PlayerConfig& cfg = config;
	float y;
	Vec3 n;
	if (!world.ladderAt({feet.x, feet.y + 90.0f, feet.z})) {
		climbing = false;
		_letGo = false;
		return false;
	}
	if (grounded) _letGo = false;
	if (in.jump) {
		// Jumping lets go, and the ladder stays let go until the player has
		// landed or left it.
		_letGo = _letGo || climbing;
		climbing = false;
		return false;
	}
	if (_letGo) return false;
	if (!climbing) {
		if (grounded) {
			// From the floor: by walking into the ladder.
			if (!(in.moveZ > 0.1f && _blocked)) return false;
		} else if (velocityY > 0 || world.rayDown({feet.x, feet.y + cfg.stepHeight, feet.z}, cfg.stepHeight + 100.0f, y, n)) {
			// In the air, but only hopping at the foot of the ladder or about
			// to land at its top.
			return false;
		}
		// Otherwise: caught in mid-air, which is also the way down from the top.
		climbing = true;
	} else if (in.moveZ > 0.1f && !_blocked) {
		// Nothing in the way any more: this is the top. Stay level and walk
		// on until there is floor underfoot.
		velocityY = 0;
		grounded = false;
		if (world.rayDown({feet.x, feet.y + cfg.stepHeight, feet.z}, cfg.stepHeight + 30.0f, y, n) &&
		    n.y >= cfg.minFloorNormal) {
			feet.y = y;
			grounded = true;
			climbing = false;
		}
		return true;
	}
	velocityY = 0;
	const float dy = in.moveZ * cfg.climbSpeed * dt;
	if (dy < 0 && world.rayDown({feet.x, feet.y + cfg.stepHeight, feet.z}, cfg.stepHeight - dy + 1.0f, y, n) &&
	    n.y >= cfg.minFloorNormal) {
		// Back on the floor at the bottom.
		feet.y = y;
		grounded = true;
		climbing = false;
		return true;
	}
	grounded = false;
	if (dy > 0) {
		// A ceiling over the ladder ends the climb.
		const float r = cfg.radius;
		Vec3 head = {feet.x, feet.y + dy + cfg.eyeHeight - r, feet.z};
		const float headY = head.y;
		if (world.pushSphere(head, r) && head.y < headY - 0.5f) return true;
	}
	feet.y += dy;
	collideWalls(world);
	return true;
}

// Where a jump's hands find a ledge. The floor is felt for ahead of the body
// from above (rayDown finds the highest surface under the point): where the
// first point with a floor between the two heights follows a point without
// one (or a much lower one), there is a wall between them, and its edge.
bool Player::probeLedge(const World& world, float yaw, Ledge& out) const {
	out = Ledge();
	const PlayerConfig& cfg = config;
	const Vec3 dir = {sinf(yaw), 0, cosf(yaw)};
	const float top = feet.y + cfg.ledgeReach + 15.0f;
	const float drop = cfg.ledgeReach + 15.0f + 140.0f;
	float prevY = -1e9f;
	bool prevHit = false;
	const float from = cfg.radius - 8.0f, to = cfg.radius + cfg.ledgeRange;
	for (float d = from; d <= to; d += 10.0f) {
		float y;
		Vec3 n;
		const bool hit = world.rayDown({feet.x + dir.x * d, top, feet.z + dir.z * d}, drop, y, n);
		const float rel = hit ? y - feet.y : -1e9f;
		if (hit && rel >= cfg.ledgeMin && rel <= cfg.ledgeReach && n.y >= cfg.minFloorNormal) {
			// A wall only if what came before is clearly lower (or nothing).
			if (d <= from + 1.0f || (prevHit && prevY > y - 35.0f)) return false;
			// The place on top: a body's width in from the edge, on the same floor.
			const float inD = d + 22.0f;
			const Vec3 at = {feet.x + dir.x * inD, 0, feet.z + dir.z * inD};
			float y2;
			Vec3 n2;
			if (!world.rayDown({at.x, top, at.z}, drop, y2, n2) || fabsf(y2 - y) > 25.0f || n2.y < cfg.minFloorNormal) return false;
			// Room to stand there: the body's two spheres are not pushed.
			const float r = cfg.radius;
			const float heights[2] = {cfg.stepHeight + r, cfg.eyeHeight - r};
			for (float h : heights) {
				Vec3 c = {at.x, y2 + h, at.z};
				const Vec3 c0 = c;
				if (world.pushSphere(c, r) && (fabsf(c.x - c0.x) + fabsf(c.z - c0.z) > 4.0f || c.y < c0.y - 4.0f)) return false;
			}
			out.found = true;
			out.top = {at.x, y2, at.z};
			out.height = y2 - feet.y;
			out.gap = max(d - 5.0f - cfg.radius, 0.0f);
			out.yaw = yaw;
			return true;
		}
		prevHit = hit;
		prevY = hit ? y : -1e9f;
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

	// Horizontal move in small steps so thin walls cannot be skipped. Going
	// down a ladder, "back" must not walk the player off it.
	const float forward = (climbing && in.moveZ < 0) ? 0.0f : in.moveZ;
	const float side = drive ? 0.0f : in.moveX; // (a driven character turns with it instead)
	Vec3 wish = {sinf(yaw) * forward + cosf(yaw) * side, 0, cosf(yaw) * forward - sinf(yaw) * side};
	const float len = length(wish);
	if (len > 1.0f) wish = wish * (1.0f / len);
	float speed = in.fast ? cfg.fastSpeed : cfg.walkSpeed;
	if (climbing)
		speed = cfg.walkSpeed * 0.35f; // just enough to step off at the top
	else if (swimming)
		speed = min(speed, cfg.swimSpeed);
	else if (waterDepth > cfg.wadeDepth)
		speed *= cfg.wadeFactor;
	Vec3 delta = wish * (speed * dt);
	if (drive && !climbing) {
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
	// Did something stop the move? (Ladders are climbed by walking into them.)
	const float gained = (feet.x - from.x) * delta.x + (feet.z - from.z) * delta.z;
	_blocked = dist > 0.01f && gained < 0.5f * dist * dist;

	if (swim(world, dt)) {
		climbing = false;
		_step = 'w';
		return;
	}
	if (climb(world, in, dt)) {
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
