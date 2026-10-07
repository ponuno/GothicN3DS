#include "player.h"

namespace g3 {

void Player::spawn(Vec3 eye) {
	feet = {eye.x, eye.y - config.eyeHeight, eye.z};
	_eyeY = eye.y;
	velocityY = 0;
	grounded = false;
	_placed = false;
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

void Player::walk(const World& world, const Input& in, float yaw, float dt) {
	const PlayerConfig& cfg = config;

	// The ground under the spawn point may still be streaming in.
	if (!_placed) {
		float y;
		Vec3 n;
		if (!world.rayDown({feet.x, feet.y + 200.0f, feet.z}, 3000.0f, y, n)) return;
		feet.y = y;
		grounded = true;
		velocityY = 0;
		_placed = true;
	}

	// Horizontal move in small steps so thin walls cannot be skipped.
	Vec3 wish = {sinf(yaw) * in.moveZ + cosf(yaw) * in.moveX, 0, cosf(yaw) * in.moveZ - sinf(yaw) * in.moveX};
	const float len = length(wish);
	if (len > 1.0f) wish = wish * (1.0f / len);
	const Vec3 delta = wish * ((in.fast ? cfg.fastSpeed : cfg.walkSpeed) * dt);
	const float dist = length(delta);
	const int steps = clamp(int(dist / (cfg.radius * 0.5f)) + 1, 1, 12);
	for (int i = 0; i < steps; ++i) {
		feet.x += delta.x / float(steps);
		feet.z += delta.z / float(steps);
		collideWalls(world);
	}

	if (grounded && in.jump) {
		velocityY = cfg.jumpSpeed;
		grounded = false;
	}

	float y;
	Vec3 n;
	if (grounded) {
		// Stay glued to the floor: up a step, or down a slope or small ledge.
		const float snap = cfg.stepHeight * 0.8f;
		if (world.rayDown({feet.x, feet.y + cfg.stepHeight, feet.z}, cfg.stepHeight + snap, y, n) &&
		    n.y >= cfg.minFloorNormal) {
			feet.y = y;
		} else {
			grounded = false;
			velocityY = 0;
		}
	}
	if (!grounded) {
		velocityY = max(velocityY - cfg.gravity * dt, -4000.0f);
		const float dy = velocityY * dt;
		if (dy > 0) {
			feet.y += dy; // rising: walls and ceilings are handled by the spheres
			collideWalls(world);
		} else if (world.rayDown({feet.x, feet.y + cfg.stepHeight, feet.z}, cfg.stepHeight - dy, y, n)) {
			feet.y = y;
			if (n.y >= cfg.minFloorNormal) {
				grounded = true;
				velocityY = 0;
			} else {
				// Too steep to stand on: slide downhill.
				feet.x += n.x * 400.0f * dt;
				feet.z += n.z * 400.0f * dt;
				velocityY = max(velocityY, -600.0f);
				collideWalls(world);
			}
		} else if (world.rayDown({feet.x, feet.y + cfg.stepHeight, feet.z}, 50000.0f, y, n)) {
			feet.y += dy;
		} else {
			// Nothing below at all (edge of the loaded world): hover instead of
			// falling forever.
			velocityY = 0;
		}
	}
}

void Player::update(const World& world, const Input& in, float yaw, float pitch, float dt) {
	if (in.toggleFly) {
		flying = !flying;
		velocityY = 0;
		grounded = false;
	}

	if (flying) {
		const float speed = (in.fast ? config.flyFastSpeed : config.flySpeed) * dt;
		const Vec3 f = forwardFromYawPitch(yaw, pitch);
		const Vec3 r = normalize(cross({0, 1, 0}, f));
		feet = feet + f * (in.moveZ * speed) + r * (in.moveX * speed) + Vec3 {0, in.moveY * speed, 0};
		_eyeY = feet.y + config.eyeHeight;
		_placed = true;
		return;
	}

	walk(world, in, yaw, dt);
	// Smooth the eye over steps and landings instead of snapping.
	const float target = feet.y + config.eyeHeight;
	const float blend = min(dt * 14.0f, 1.0f);
	_eyeY += (target - _eyeY) * blend;
	if (_eyeY < target - 60.0f) _eyeY = target - 60.0f;
	if (_eyeY > target + 60.0f) _eyeY = target + 60.0f;
}

} // namespace g3
