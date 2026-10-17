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
	enum { OVERLAYS = 8 }; // fix202

private:
	const Model* _overlays[OVERLAYS] = {};
	u32 _overlayCount = 0;
	void cacheClips();
	const Clip* lookup(u32 hash) const;
	const Clip* lookup(const char* name) const { return lookup(hashName(name)); }
public:
	// What the character is doing, in broad terms (for display and tests).
	enum Role { IDLE, RUN, WALK, BACK, STRAFE, TURN, JUMP, FALL, SWIM, SWIM_IDLE, ATTACK, PARRY, HIT, DOWN, DRAW, USE, CLIMB, ROLE_COUNT };
	// What the body is in.
	enum Body { GROUNDED, TAKEOFF, AIRBORNE, SWIMMING, MANTLE };

	// False if the archive has no hero (the game then stays first-person).
	bool load(Pak& pak);
	// The same with another model, by its pak entry (fix151: a transformation's creature, "model/form.wolf"); the
	// character is then whatever that model's animations make of him. False, and nothing loaded, if it cannot be read.
	bool loadEntry(Pak& pak, u32 entryHash);
	// The overlays the character wears (Mdl_ApplyOverlayMds: a fight talent's blows, a drunk's walk), the last one
	// first in line: their animations stand in for the model's of the same name. The models must outlive their use.
	void setOverlays(const Model* const* models, u32 count);
	void release();
	bool loaded() const { return _model.loaded(); }
	// Told of the sound events of the clips as they play (footsteps, blows).
	void setSoundHook(const SoundEventHook& hook) { _soundHook = hook; }

	// Puts the character there, standing still and facing `yaw` (after a teleport).
	void reset(Vec3 feet, float yaw);

	// First half of a frame: turns the character, picks and advances its
	// animation from what the player asks for and what the body is in, and
	// says where the animation carries the body. Hand the result to
	// Player::update().
	// `ledge`, if given, is a ledge within the reach of a body in a jump
	// (Player::probeLedge, asked when wantsLedge()): the character takes hold
	// of it and climbs up. `groundLedge`: one in reach of a body standing
	// (asked when wantsJumpUp()): the jump key climbs it as the original does
	// (low, middle or high - hanging from the edge first); `wall`: a wall
	// right ahead without one - the jump goes straight up instead of forward.
	Drive control(const Input& in, const Player& player, float dt, const Ledge* ledge = nullptr, const Ledge* groundLedge = nullptr,
	              bool wall = false);
	// On the ground and free to jump: the application probes for a ledge ahead.
	bool wantsJumpUp(const Player& player) const;
	// In a jump, and so far through it that its hands would take hold of a ledge.
	bool wantsLedge(const Player& player) const;
	bool mantling() const { return _body == MANTLE; }
	// How far through the climb onto a ledge, 0..1.
	float mantleProgress() const;
	// For the tests: the hands' height and the soles' (world, of the pose drawn now); false: no such nodes.
	bool mantleProbe(float& handsY, float& soleY) const;
	float mantleHandsWeight() const; // how much of the climb the hands are on the edge for (0..1)
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
	const float* light() const { return _shown; }
	// fix148: the lamps' light on him (Lamps::lightAt), added to the ground's from now on; and how much sky his ground sees (0..1).
	void setLampLight(const float add[3]) {
		for (int i = 0; i < 3; ++i) {
			_lamp[i] = add[i];
			_shown[i] = _light[i] + add[i];
		}
	}
	float sky() const { return _sky; }
	// The clip of a name, if the model has it ("S_RUNL").
	const Clip* clip(const char* name) const { return _model.clip(name); }

	// ---- fighting ----
	// What is in the hands (a WeaponMode). It changes half way through
	// taking a weapon in hand or putting it away.
	u8 weapon() const { return _weapon; }
	// Taking a weapon in hand or putting it away right now.
	bool drawing() const { return _drawTo != 0xFF; }
	// The mode being taken in hand (the weapon once the hands change), else the one in hand.
	u8 weaponIntent() const { return _drawTo != 0xFF && _drawTo != WEAPON_NONE ? _drawTo : _weapon; }
	// Takes `mode` in hand (WEAPON_NONE: puts it away). False if that cannot
	// be done now (in the air, in the water, down) or the model has no way of
	// it (then the hands change at once).
	bool draw(u8 mode);
	// An attack: 0 ahead, 1 from the left, 2 from the right. Pressed again
	// within the window of a blow, the combo goes on with its next one.
	// False if there is no weapon in hand or the body cannot now.
	bool attack(int kind);
	// fix208: the finishing blow at one lying knocked out ("T_<W>SFINISH", else the blow ahead): its landing is a
	// blowLanded() like any other. False if no melee weapon is in hand or the body cannot now.
	bool finish();
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
	// fix216: a blow of the weapon is in play (the white trail follows the blade)
	bool swinging() const { return _blow.clip && _anim.current() == _blow.clip; }

	// ---- casting (WEAPON_MAG in hand) ----
	// Plays a spell's casting animation, `letters` being the spell's ("FBT", the scripts'
	// spellFXAniLetters): the charging "T_MAGRUN_2_<L>CAST" (it is held at its end) or, with `shoot`, the
	// release "T_MAGRUN_2_<L>SHOOT". The body stands meanwhile. Returns how many seconds the clip plays;
	// 0 if the model has none (then nothing is played).
	float castPlay(const char* letters, bool shoot);
	// A bow or a crossbow in hand (WEAPON_BOW, WEAPON_CBOW): phase 0 aims ("S_<W>AIM", held at its end), 1 lets
	// the arrow go ("S_<W>SHOOT"), 2 reloads ("T_<W>RELOAD"). Returns the seconds the clip plays; 0 if the model has none.
	float rangedPlay(int phase);
	// Back to the stance.
	void castEnd();
	// A casting animation is in play (still or held at its end).
	bool casting() const { return _castHold; }
	// Seconds the casting animation has played.
	float castTime() const { return _castHold && _anim.current() == _castClip ? _anim.time() : 0.0f; }

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
	// Picking a thing up from the ground: "T_STAND_2_IGET", "S_IGET",
	// "T_IGET_2_STAND" as the model has them; the same rules as useItem (the
	// moment is when the hand is at the ground).
	bool pickUp(float seconds, float moment);
	// fix192: putting a thing down on the ground: "T_STAND_2_IDROP", "S_IDROP", "T_IDROP_2_STAND" as the model has
	// them, else the picking up's (the hand goes to the ground all the same); the moment is when it lets go.
	bool putDown(float seconds, float moment);
	// A chest: the character bends to it (the model script's "T_<SCHEME>_STAND_2_S0"
	// and the lid's "T_<SCHEME>_S0_2_S1"; bent over without them) and stays
	// at it, `mobOpen()` from the moment the lid is up, until closeMob() lets
	// it stand again. The same conditions as useItem.
	// `states`: how many states the thing goes through after the first (S0): the
	// steps are played up to the last and back (WorldMob::states), the way
	// OpenGothic's Interactive::implTick walks them. `slotKind`: MOB_SLOT_FRONT /
	// MOB_SLOT_BACK, the place the character stands at ("T_BEDHIGH_FRONT_..."
	// is tried before "T_BEDHIGH_...").
	bool openMob(const char* scheme, int states = 1, int slotKind = 0);
	bool mobOpen() const { return _use.phase == USE_MOB_HOLD && !_use.release; }
	void closeMob();
	// There is a pose that is kept while it is in use (a looping "S_<SCHEME>_S1").
	bool mobHolds() const { return _use.hold != nullptr && _use.hold->loops(); }
	bool atMob() const { return _use.phase == USE_MOB_IN || _use.phase == USE_MOB_HOLD || _use.phase == USE_MOB_OUT; }
	// How far the character is bent over (for the tests).
	float stoop() const { return _stoop; }
	bool usingItem() const { return _use.phase != USE_NONE; }
	// True for the frame the item does its work (it is drunk, eaten).
	bool useMoment() const { return _use.moment; }
	// How far through the use, 0..1 (for the interface).
	float useProgress() const;
	// True if the model has the animations of the scheme.
	bool canAnimateUse(const char* scheme) const;
	// How long the ladder's ways on and off take as the model has them (0: it has none) - Player::ladder*Time -
	// and how high the way over the top lifts the body.
	float ladderMountTime() const { return _ladderMount ? _ladderMount->duration() : 0.0f; }
	float ladderMountTopTime() const {
		return _ladderMountTop ? _ladderMountTop->duration() : _ladderExitTop ? _ladderExitTop->duration() : 0.0f;
	}
	float ladderExitTopTime() const { return _ladderExitTop ? _ladderExitTop->duration() : 0.0f; }
	float ladderExitFootTime() const { return _ladderExitFoot ? _ladderExitFoot->duration() : 0.0f; }
	float ladderExitRise() const;

	void cancelUse(); // drops any use/mantle in progress
private:
	SoundEventHook _soundHook;
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
	bool isStrafe(const Clip* clip) const;
	const Clip* stateClip(int state) const;
	const Clip* rowClip(int row, int state) const;
	void go(int state, bool force = false, float forceFade = 0.15f);
	void act(const Clip* clip, float fade);
	void updatePose(float dt);
	const Clip* weaponClip(const char* pattern, u8 mode) const;
	void fightTick(float prevTime, const Clip* prevClip);
	void useTick(float dt);
	bool beginMantle(const Ledge& ledge, const Player& player, bool fromGround);
	void mantleDrive(Drive& drive, float dt);
	bool beginUse(const Clip* in, const Clip* hold, const Clip* out, float seconds, float moment, bool stoop = false);

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
	float _exitCarry = 0, _exitTime = 1.0f; // (the climb's height left in the pose the next one fades from)
	float _light[3] = {1, 1, 1};
	float _lamp[3] = {0, 0, 0};  // fix148: the lamps' light on him
	float _shown[3] = {1, 1, 1}; // the two together: what he is drawn in
	float _sky = 1.0f;
	float _jumpCool = 0.0f;
	bool _placed = false, _walkMode = false, _slow = false, _jumpHeld = false, _jumping = false, _leftGround = false;
	bool _sneak = false;
	bool _forceNext = false;
	bool _fistOnly = false; // (fix151) a creature's model: no bare states, the fists' stand in
public:
	bool fistOnly() const { return _fistOnly; }
private:
	const Clip* _takeoff = nullptr;
	// Climbing onto a ledge, as in the original: from the ground a low
	// (T_STAND_2_JUMPUPLOW, S_JUMPUPLOW, T_JUMPUPLOW_2_STAND), a middle
	// (..._JUMPUPMID...) or a high one (T_STAND_2_JUMPUP, S_JUMPUP, T_JUMP_2_HANG,
	// S_HANG, T_HANG_2_STAND); from a jump, the last steps of these.
	enum { CLIMB_LOW_IN, CLIMB_LOW, CLIMB_LOW_OUT, CLIMB_MID_IN, CLIMB_MID, CLIMB_MID_OUT, CLIMB_UP_IN, CLIMB_UP, CLIMB_HANG_IN, CLIMB_HANG,
	       CLIMB_HANG_OUT, CLIMB_CLIPS };
	const Clip* _climb[CLIMB_CLIPS] = {};
	// A ladder, as the model script has it for the scheme LADDER: "T_LADDER_S0_2_S1"
	// and on, one for each stretch of rungs. The controller moves the body; the
	// animation is scrubbed by the height climbed (up plays it forward, down back),
	// so the hands and feet never slide on the rungs.
	// Taking hold of it at the foot ("T_LADDER_STAND_2_S0"), letting go there ("T_LADDER_S0_2_STAND"), up over
	// its top onto the floor ("T_LADDER_UP_2_STAND", else the highest "T_LADDER_S<k>_2_STAND") and the way
	// back onto it from the top ("T_LADDER_STAND_2_UP", "T_LADDER_STAND_2_S<k>", else the way up played backwards).
	enum { LADDER_CLIPS = 8 };
	const Clip* _ladder[LADDER_CLIPS] = {};
	float _ladderRise[LADDER_CLIPS] = {};
	int _ladderCount = 0;
	const Clip* _ladderMount = nullptr;
	const Clip* _ladderExitFoot = nullptr;
	const Clip* _ladderExitTop = nullptr;
	const Clip* _ladderMountTop = nullptr; // null: _ladderExitTop backwards
	bool _ladderOn = false;
	bool _ladderSubtract = false; // the clip's own way up is the body's (it is taken out of the pose)
	u8 _ladderPhase = 0;
	float _ladderY0 = 0;
	void ladderTick(const Player& player);
	enum { MANTLE_STEPS = 5 };
	struct Mantle {
		const Clip* seq[MANTLE_STEPS] = {}; // the climb's clips in turn (none: the jump's pose is held)
		float len[MANTLE_STEPS] = {};      // seconds each is played
		float rise[MANTLE_STEPS] = {};     // cm each lifts the body (its root's way up, scaled to the ledge)
		float peak[MANTLE_STEPS] = {};     // the highest its root gets (cm, unscaled)
		float free[MANTLE_STEPS] = {};     // cm more, without the animation's help (the flight up to the edge)
		float scale = 1;
		u8 kind[MANTLE_STEPS] = {};        // 0 flying up, 1 reaching for the edge, 2 hanging on it, 3 pulling over it
		float bodyY = 0;                   // where the body is this frame (the hands are fitted to the edge from it)
		u8 n = 0, at = 0;
		float clipTime = 0;
		Vec3 start {0, 0, 0}, top {0, 0, 0};
		float gap = 0, time = 0, duration = 0;
		float yaw = 0;
		const Clip* current() const { return at < n ? seq[at] : nullptr; }
	} _mantle;
	bool _jumpUp = false; // a jump straight up (at a wall)
	float _groundY = 0;   // where the feet last stood
	// The climb's lift at this moment: what the animation's own rise gives (the
	// pose gives up as much), and what the body is lifted without it.
	void mantleLift(float& anim, float& total, float* raw = nullptr) const;
	Vec3 _liftSum {0, 0, 0};
	float _liftTime = 0, _liftY = 0;
	Pose _drawn;
	// Fighting.
	u8 _weapon = WEAPON_NONE;
	u8 _drawTo = 0xFF;      // being taken in hand (0xFF: nothing)
	float _drawAt = 0;      // seconds into the draw where the hands change
	const Clip* _drawClip = nullptr;
	bool _drawBack = false; // the weapon is being put away: the clip plays backwards
	BlowTracker _blow;
	bool _blowMove = false; // the blow in play was struck on the run
	const Clip* _castClip = nullptr;
	bool _castHold = false; // a casting animation has the body
	bool _comboQueued = false; // pressed just before the window of the next blow opened
	bool goOn(const ClipFight* f, float fps, float t);
	bool _comboBroken = false; // pressed outside the window: no more blows of this attack
	bool _blowLanded = false;
	bool _parryHeld = false;
	const Clip* _reel = nullptr; // being hit
	bool _down = false;
	const Clip* _parryClip[ROWS] = {};
	// Using an item.
	enum { MOB_STEPS = 20 };
	enum UsePhase : u8 { USE_NONE, USE_IN, USE_HOLD, USE_OUT, USE_TIMED, USE_MOB_IN, USE_MOB_HOLD, USE_MOB_OUT };
	struct Use {
		const Clip* in = nullptr;
		const Clip* hold = nullptr;
		const Clip* out = nullptr;
		u8 phase = USE_NONE;
		bool done = false;   // the item has done its work
		bool moment = false; // ... this frame
		float time = 0, seconds = 0, at = 0; // how long so far; all of it, when it works (estimated for the animations)
		float holdFor = 0.3f; // seconds the hold is kept
		bool stoop = false; // no animation for it: bends over instead (a thing from the ground)
		// A chest: the clips to its open lid and the clips back.
		const Clip* seqIn[MOB_STEPS] = {};
		const Clip* seqOut[MOB_STEPS] = {};
		u8 nIn = 0, nOut = 0, at2 = 0;
		float time2 = 0;      // seconds on the way out, without clips
		bool release = false; // closed: on the way back
		bool stoopHold = false; // no animations for the chest: bent over while it is open
		float openAt = 0;       // a body: the page is up this many seconds into the search animation (0: when it is over)
	};
	Use _use;
	float _stoop = 0; // how far bent over, 0..1 (drawn by updatePose)
	int _spine[3] = {-1, -1, -1}, _headNode = -1, _armNodes[2] = {-1, -1}, _handNodes[2] = {-1, -1}, _footNodes[2] = {-1, -1};
	void mobTick(float dt);
};

} // namespace g3
