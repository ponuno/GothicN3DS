// The player's character: the model named by "meta/hero", steered and
// animated the way ZenGin does it.
//
//  * The player steers the character, not the camera: forward and back along
//    where it faces, left and right turn it.
//  * The character goes where its animation carries it (root motion): the
//    body is moved by what the animation's root does from frame to frame, so
//    starting, stopping, a hop back or a jump look and travel as authored.
//  * Animations are changed the way the engine's model scripts mean it: from
//    state A to state B through the transition "T_A_2_B" if the script has
//    one (an animation, or a fade of scripted length), from standing through
//    "T_STAND_2_B", to standing through "T_A_2_STAND". A transition is played
//    to its end before anything else may follow.
#pragma once
#include "model.h"
#include "player.h"

namespace g3 {

class Hero {
public:
	// What the character is doing, in broad terms (for display and tests).
	enum Role { IDLE, RUN, WALK, BACK, STRAFE, TURN, JUMP, FALL, SWIM, SWIM_IDLE, ROLE_COUNT };
	// What the body is in.
	enum Body { GROUNDED, TAKEOFF, AIRBORNE, SWIMMING };

	// False if the archive has no hero (the game then stays first-person).
	bool load(Pak& pak);
	void release();
	bool loaded() const { return _model.loaded(); }

	// Puts the character there, standing still and facing `yaw` (after a teleport).
	void reset(Vec3 feet, float yaw);

	// First half of a frame: turns the character, picks and advances its
	// animation from what the player asks for and what the body is in, and
	// says where the animation carries the body. Hand the result to
	// Player::update().
	Drive control(const Input& in, const Player& player, float dt);
	// Second half, after the controller has moved the body: follows it,
	// notices take-off, landing and water, takes the light of the ground.
	// `dayLight` is the time-of-day tint the world is drawn with.
	void follow(const Player& player, const World& world, const float dayLight[3], float dt);
	// How far the camera hangs behind the character's turn, radians (positive:
	// the character has turned right of it). A runner leans into the turn by it.
	void setTurnLag(float radians) { _turnLag = radians; }
	void draw() const;

	const Model& model() const { return _model; }
	const Animator& animator() const { return _anim; }
	Role role() const { return _roleNow; }
	Body body() const { return _body; }
	float yaw() const { return _yaw; }
	void setYaw(float yaw) { _yaw = yaw; }
	float speed() const { return _speed; } // horizontal, cm/s
	Vec3 feet() const { return _feet; }
	bool walking() const { return _walkMode; }
	float lean() const { return _lean; }
	// The pose as drawn (the animator's, with the hips where the body state wants them).
	const Pose& pose() const { return _drawn; }
	// Height of the hips above the feet at rest: what the camera looks after.
	float hipHeight() const { return _restY; }
	const float* light() const { return _light; }
	// The clip of a name, if the model has it ("S_RUNL").
	const Clip* clip(const char* name) const { return _model.clip(name); }

private:
	enum State { ST_RUN, ST_RUNL, ST_WALK, ST_WALKL, ST_WALKBL, ST_WALKWL, ST_JUMP, ST_FALLDN, ST_FALL, ST_SWIM, ST_SWIMF, ST_SWIMB, ST_COUNT };
	static constexpr int STAND = ST_COUNT; // the pseudo-state of the transitions' names
	struct Via {
		const Clip* clip; // a transition animation, or
		float fade;       // (>= 0) a fade of this length
	};

	int stateOf(const Clip* clip) const;
	bool isTurn(const Clip* clip) const;
	const Clip* stateClip(int state) const;
	void go(int state, bool force = false, float forceFade = 0.15f);
	void act(const Clip* clip, float fade);
	void updatePose(float dt);

	Model _model;
	Animator _anim;
	const Clip* _state[ST_COUNT] = {};
	Via _via[ST_COUNT + 1][ST_COUNT + 1] = {};
	const Clip* _jumpBack = nullptr;
	const Clip* _strafe[2][2] = {}; // [walk][right]
	const Clip* _turn[3][2] = {};   // [run, walk, swim][right]
	Body _body = GROUNDED;
	Role _roleNow = IDLE;
	int _base = ST_RUN; // the standing state under a turn on the spot
	Vec3 _feet {0, 0, 0};
	float _yaw = 0, _speed = 0, _airTime = 0, _turnLag = 0, _lean = 0;
	float _restY = 100.0f, _hipMix = 0;
	float _light[3] = {1, 1, 1};
	bool _placed = false, _walkMode = false, _slow = false, _jumpHeld = false, _jumping = false, _leftGround = false;
	bool _forceNext = false;
	const Clip* _takeoff = nullptr;
	Vec3 _liftSum {0, 0, 0};
	float _liftTime = 0, _liftY = 0;
	Pose _drawn;
};

} // namespace g3
