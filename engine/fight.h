// Fighting: what is shared by the player's character and the inhabitants.
//
// The rules are Gothic 1's, from its own scripts wherever the scripts have
// them (game.h: Game::meleeDamage, meleeRange): damage per damage type is
// strength plus the weapon's damage minus the victim's protection; the
// reach is the guilds' fight ranges (C_GILVALUES) plus the weapon's. How a
// fight is fought - the moves the inhabitants choose - is the scripts' too:
// the fight AI tables (C_FightAI instances "FA_..._<tactic>") pick the next
// move from what the situation is (npc_fight.cpp). The animations are the
// model script's, with the frames that matter from its event tags
// (g3ds/model.h, ClipFight): where a blow lands, where a combo may go on,
// where a parry holds.
//
// How the original does this is taken from OpenGothic (FightAlgo,
// DamageCalculator, Npc::implAttack and takeDamage) and the scripts of
// Gothic 1 (AI/FAI, Fight.d, Species.d).
#pragma once
#include "g3ds/base.h"
#include "g3ds/model.h"

namespace g3 {

// What a character holds in its hands, as the animations' names say it.
enum WeaponMode : u8 { WEAPON_NONE, WEAPON_FIST, WEAPON_1H, WEAPON_2H, WEAPON_MODES };
// "" "FIST" "1H" "2H": the word in "S_%sRUN".
const char* weaponWord(u8 mode);

// Down and out (GameNpc::down).
enum DownState : u8 { DOWN_NONE, DOWN_UNCONSCIOUS, DOWN_DEAD };

// The moves of the fight AI (the scripts' MOVE_ constants are read by name;
// these are the engine's own numbers for them).
enum FightMove : u8 {
	FM_NONE,
	FM_RUN,          // MOVE_RUN: towards the enemy
	FM_RUNBACK,      // MOVE_RUNBACK
	FM_JUMPBACK,     // MOVE_JUMPBACK
	FM_TURN,         // MOVE_TURN: towards the enemy (and close in)
	FM_STRAFE,       // MOVE_STRAFE: a step aside
	FM_ATTACK,       // MOVE_ATTACK: a blow ahead
	FM_SIDEATTACK,   // MOVE_SIDEATTACK: left, right
	FM_FRONTATTACK,  // MOVE_FRONTATTACK: a side, then ahead
	FM_TRIPLEATTACK, // MOVE_TRIPLEATTACK
	FM_WHIRLATTACK,  // MOVE_WHIRLATTACK
	FM_MASTERATTACK, // MOVE_MASTERATTACK
	FM_TURNTOHIT,    // MOVE_TURNTOHIT
	FM_PARADE,       // MOVE_PARADE
	FM_STANDUP,      // MOVE_STANDUP
	FM_WAIT,         // MOVE_WAIT, MOVE_WAIT_EXT
	FM_WAITLONGER,   // MOVE_WAIT_LONGER
	FM_COUNT
};

// The situations of a fight AI table ("FA_ENEMY_PREHIT_3" ...), in this order.
enum FightSituation : u8 {
	FS_ENEMY_PREHIT,
	FS_ENEMY_STORMPREHIT,
	FS_MY_W_COMBO,
	FS_MY_W_RUNTO,
	FS_MY_W_STRAFE,
	FS_MY_W_FOCUS,
	FS_MY_W_NOFOCUS,
	FS_MY_G_COMBO,
	FS_MY_G_RUNTO,
	FS_MY_G_STRAFE,
	FS_MY_G_FOCUS,
	FS_MY_FK_FOCUS,
	FS_MY_G_FK_NOFOCUS,
	FS_COUNT
};
extern const char* const kFightSituations[FS_COUNT];

// Steps of a move, as they are carried out one after the other.
enum FightStep : u8 { STEP_NONE, STEP_MOVE, STEP_TURN, STEP_ATTACK, STEP_ATTACKL, STEP_ATTACKR, STEP_BLOCK, STEP_JUMPBACK,
                      STEP_STRAFEL, STEP_STRAFER, STEP_WAIT, STEP_WAITLONG };

// Where a fighter's blow is: the clip of an attack and how far through its
// combo; queried each frame to see whether a blow has just landed.
struct BlowTracker {
	const void* clip = nullptr; // the attack clip in play (a Clip*)
	u8 combo = 0;    // blows of the combo begun so far - 1 (0: the first)
	u8 landed = 0;   // blows of it that have landed
	bool wantMore = false; // attack pressed again within the window: go on with the next blow

	void reset() { *this = BlowTracker(); }
};

// The frame of `fight` a blow `index` lands at, as seconds; negative if none.
float blowTime(const ClipFight* fight, u32 index, float fps);
// Where the blow `index` ends (the combo stops there unless carried on), seconds; negative if none.
float blowEnd(const ClipFight* fight, u32 index, float fps);
// True if `time` (seconds) is within the window of blow `index` for carrying on.
bool inComboWindow(const ClipFight* fight, u32 index, float fps, float time);
// True if `time` is within the clip's parry frames.
bool inParry(const ClipFight* fight, float fps, float time);

} // namespace g3
