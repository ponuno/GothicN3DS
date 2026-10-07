// The player's character: the model named by "meta/hero", animated from what
// the Player (the controller that owns the position) is doing.
#pragma once
#include "model.h"
#include "player.h"

namespace g3 {

class Hero {
public:
	// What the character can be doing. Each is tried under several clip names
	// (Gothic's); one that the model lacks falls back to a simpler one.
	enum Role { IDLE, RUN, WALK, JUMP, FALL, SWIM, SWIM_IDLE, ROLE_COUNT };

	// False if the archive has no hero (the game then stays first-person).
	bool load(Pak& pak);
	void release();
	bool loaded() const { return _model.loaded(); }

	// Puts the character there, standing still (after a teleport).
	void reset(Vec3 feet, float yaw);
	// `dayLight` is the time-of-day tint the world is drawn with.
	void update(const Player& player, const World& world, const float dayLight[3], float dt);
	void draw() const;

	const Model& model() const { return _model; }
	const Animator& animator() const { return _anim; }
	Role role() const { return _current; }
	bool hasRole(Role r) const { return _role[r] != nullptr; }
	float yaw() const { return _yaw; }
	float speed() const { return _speed; } // horizontal, cm/s
	Vec3 feet() const { return _feet; }
	// Where the model's origin is drawn: the feet, except in deep water,
	// where the body is lifted to lie in the surface.
	Vec3 origin() const { return {_feet.x, _feet.y + _lift, _feet.z}; }
	// How fast the clip of a role carries the character, cm/s; 0 if it does
	// not move (or the role has no clip).
	float roleSpeed(Role r) const { return _role[r] ? _role[r]->header().moveSpeed : 0.0f; }
	const float* light() const { return _light; }

private:
	Model _model;
	Animator _anim;
	const Clip* _role[ROLE_COUNT] = {};
	Role _current = IDLE;
	Vec3 _feet {0, 0, 0};
	float _yaw = 0, _speed = 0, _airTime = 0, _lift = 0;
	float _light[3] = {1, 1, 1};
	bool _placed = false;
};

} // namespace g3
