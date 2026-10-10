// The player's character: the model named by "meta/hero", steered and
// animated the way ZenGin does it.
//
//  * The player steers the character, not the camera: forward and back along
//    where it faces, left and right turn it. (The console scheme of the
//    application steers it by a heading instead: Input::steer.)
//  * The character goes where its animation carries it (root motion): the
//    body is moved by what the animation's root does from frame to frame, so
//    starting, stopping, a hop back or a jump look and travel as authored.
//  * Animations are changed the way the engine's model scripts mean it: from
//    state A to state B through the transition "T_A_2_B" if the script has
//    one (an animation, or a fade of scripted length), from standing through
//    "T_STAND_2_B", to standing through "T_A_2_STAND". A transition is played
//    to its end before anything else may follow.
//  * With a weapon in hand the same states have the weapon's word in their
//    names ("S_1HRUNL", "T_FISTRUN_2_FISTRUNL"); sneaking has its own
//    ("S_SNEAKL"). Each is a row of the tables below.
//  * Fighting (fight.h): taking a weapon in hand and putting it away, blows
//    and their combos (a press within a blow's window goes on with the next
//    blow), parrying, hopping back, reeling from a blow, going down.
#pragma once
#include "fight.h"
#include "model.h"
#include "player.h"

namespace g3 {

class Hero {
public:
	// What the character is doing, in broad terms (for display and tests).
	enum Role { IDLE, RUN, WALK, BACK, STRAFE, TURN, JUMP, FALL, SWIM, SWIM_IDLE, ATTACK, PARRY, HIT, DOWN, DRAW, USE, ROLE_COUNT };
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
	bool sneaking() const { return _sneak; }
	float lean() const { return _lean; }
	// The pose as drawn (the animator's, with the hips where the body state wants them).
	const Pose& pose() const { return _drawn; }
	// Height of the hips above the feet at rest: what the camera looks after.
	float hipHeight() const { return _restY; }
	const float* light() const { return _light; }
	// The clip of a name, if the model has it ("S_RUNL").
	const Clip* clip(const char* name) const { return _model.clip(name); }

	// ---- fighting ----
	// What is in the hands (a WeaponMode). It changes half way through
	// taking a weapon in hand or putting it away.
	u8 weapon() const { return _weapon; }
	// Taking a weapon in hand or putting it away right now.
	bool drawing() const { return _drawTo != 0xFF; }
	// Takes `mode` in hand (WEAPON_NONE: puts it away). False if that cannot
	// be done now (in the air, in the water, down) or the model has no way of
	// it (then the hands change at once).
	bool draw(u8 mode);
	// An attack: 0 ahead, 1 from the left, 2 from the right. Pressed again
	// within the window of a blow, the combo goes on with its next one.
	// False if there is no weapon in hand or the body cannot now.
	bool attack(int kind);
	// Fending blows off while held (a parry, again and again).
	void setParry(bool held) { _parryHeld = held; }
	// True for the frame a blow of the character lands (the frame its
	// animation says); the application looks who is hit.
	bool blowLanded() const { return _blowLanded; }
	// A blow is on its way (before it lands): enemies may fend it off.
	bool prehit() const;
	// In the frames where a blow is fended off.
	bool parrying() const;
	// Hopping back: a blow from in front misses.
	bool dodging() const;
	// Hit: reels from in front backwards, from behind forwards.
	void hit(bool fromBehind);
	// Down: knocked out or dead (stays down until getUp()).
	void fall(bool dead, bool fromBehind);
	bool down() const { return _down; }
	void getUp();
	// The blow in play, for the HUD and the tests: which of the combo, and how many landed.
	u8 combo() const { return _blow.combo; }

	// ---- using an item (a potion, food) ----
	// As the model script has it for the item's scheme ("POTIONFAST"): the
	// item to the mouth ("T_<SCHEME>_STAND_2_S0"), a moment there
	// ("S_<SCHEME>_S0"), and down again ("T_<SCHEME>_S0_2_STAND"). The
	// character stands meanwhile and can neither turn, strike nor fend off.
	// Without the animations (an older data.pak) it stands still for
	// `seconds` instead, the item doing its work after `moment` of them.
	// False if it cannot now: hands not empty, not standing on the ground,
	// down, reeling, already using something.
	bool useItem(const char* scheme, float seconds, float moment);
	bool usingItem() const { return _use.phase != USE_NONE; }
	// True for the frame the item does its work (it is drunk, eaten).
	bool useMoment() const { return _use.moment; }
	// How far through the use, 0..1 (for the interface).
	float useProgress() const;
	// True if the model has the animations of the scheme.
	bool canAnimateUse(const char* scheme) const;

private:
	enum State { ST_RUN, ST_RUNL, ST_WALK, ST_WALKL, ST_WALKBL, ST_WALKWL, ST_JUMP, ST_FALLDN, ST_FALL, ST_SWIM, ST_SWIMF, ST_SWIMB, ST_COUNT };
	static constexpr int STAND = ST_COUNT; // the pseudo-state of the transitions' names
	// The rows of the tables: what is in the hands (WeaponMode), and sneaking.
	enum { ROW_SNEAK = WEAPON_MODES, ROWS };
	struct Via {
		const Clip* clip; // a transition animation, or
		float fade;       // (>= 0) a fade of this length
	};

	int row() const;
	int stateOf(const Clip* clip) const;
	bool isTurn(const Clip* clip) const;
	const Clip* stateClip(int state) const;
	const Clip* rowClip(int row, int state) const;
	void go(int state, bool force = false, float forceFade = 0.15f);
	void act(const Clip* clip, float fade);
	void updatePose(float dt);
	const Clip* weaponClip(const char* pattern, u8 mode) const;
	void fightTick(float prevTime, const Clip* prevClip);
	void useTick(float dt);
	void cancelUse();

	Model _model;
	Animator _anim;
	const Clip* _state[ROWS][ST_COUNT] = {};
	Via _via[ROWS][ST_COUNT + 1][ST_COUNT + 1] = {};
	const Clip* _jumpBack[ROWS] = {};
	const Clip* _strafe[ROWS][2][2] = {}; // [row][walk][right]
	const Clip* _turn[ROWS][3][2] = {};   // [row][run, walk, swim][right]
	Body _body = GROUNDED;
	Role _roleNow = IDLE;
	int _base = ST_RUN; // the standing state under a turn on the spot
	Vec3 _feet {0, 0, 0};
	float _yaw = 0, _speed = 0, _airTime = 0, _turnLag = 0, _lean = 0;
	float _restY = 100.0f, _hipMix = 0;
	float _light[3] = {1, 1, 1};
	bool _placed = false, _walkMode = false, _slow = false, _jumpHeld = false, _jumping = false, _leftGround = false;
	bool _sneak = false;
	bool _forceNext = false;
	const Clip* _takeoff = nullptr;
	Vec3 _liftSum {0, 0, 0};
	float _liftTime = 0, _liftY = 0;
	Pose _drawn;
	// Fighting.
	u8 _weapon = WEAPON_NONE;
	u8 _drawTo = 0xFF;      // being taken in hand (0xFF: nothing)
	float _drawAt = 0;      // seconds into the draw where the hands change
	const Clip* _drawClip = nullptr;
	BlowTracker _blow;
	bool _comboBroken = false; // pressed outside the window: no more blows of this attack
	bool _blowLanded = false;
	bool _parryHeld = false;
	const Clip* _reel = nullptr; // being hit
	bool _down = false;
	const Clip* _parryClip[ROWS] = {};
	// Using an item.
	enum UsePhase : u8 { USE_NONE, USE_IN, USE_HOLD, USE_OUT, USE_TIMED };
	struct Use {
		const Clip* in = nullptr;
		const Clip* hold = nullptr;
		const Clip* out = nullptr;
		u8 phase = USE_NONE;
		bool done = false;   // the item has done its work
		bool moment = false; // ... this frame
		float time = 0, seconds = 0, at = 0; // how long so far; all of it, when it works (estimated for the animations)
	};
	Use _use;
};

} // namespace g3
