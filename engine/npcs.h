// The inhabitants: the characters the game's scripts put into the worlds
// (game.h), shown where their daily routines have them - and, near the
// player, living their day the way the scripts say.
//
// Models. A character is three models that share a skeleton: the "rig" has
// the skeleton and the animations, body and head have a mesh each
// ("meta/looks" says which three a look needs). Only the models of those
// near the player are in memory, and of a rig's animations only the ones
// somebody has needed so far.
//
// Life. The characters nearest to the player are "actors". An actor is in a
// state - what its routine has it do at this hour ("ZS_Guard"), or what a
// creature starts with - and a state is three script functions: one that
// begins it, one called again and again while it lasts, one that ends it.
// Those functions tell the engine what the character is to do next:
// AI_GotoWP, AI_AlignToWP, AI_PlayAni, AI_Wait ... - a queue of actions the
// actor carries out one after the other: it walks along the way net, turns,
// sits down. When the hour of another routine entry comes the state is
// ended and the next one begun, which is what makes the camp get up in the
// morning. Whoever is too far away to be an actor simply is where his
// routine wants him.
#pragma once
#include "dialog_text.h"
#include "fight.h"
#include "game.h"
#include "mobs.h"
#include "model.h"
#include "rooms.h"
#include "talk.h"
#include "waynet.h"
#include "frame_budget.h"
#include "world.h"

#include <string.h>

namespace g3 {

struct NpcConfig {
	float range = 3000.0f; // cm: how far away a character is still shown (less under load, see update())
	float lodDistance = 1800.0f; // cm: beyond this a character with a coarse body (--npc-lod) is drawn with it, nearer with the full one (0: always full)
	// Under load (the pacer's detail scale below 1) characters are shown no nearer than this share of `range`: 1 keeps the
	// whole range (people never shrink to a dozen metres and come into being in front of the player), less spares the
	// loads and scripts of the crowd in a heavy scene (the console 0.6).
	float reachFloor = 1.0f;
	// Characters farther than farTickDistance (and not fighting or talking) are stepped every farTickEvery-th frame, with the
	// time of those between: their scripts and animation cost a share of what they did (1: every frame; the console 2).
	float farTickDistance = 1500.0f;
	u32 farTickEvery = 1;
	u32 maxShown = 12;     // at most this many, the nearest (up to 16)
	// What the last frame left (platform-set): a model is read only if it fits, by the cost measured of the ones before.
	// Null: no limit.
	const FrameBudget* budget = nullptr;
	u32 loadsPerFrame = 1; // models read from the archive per frame (each is a hitch)
	u32 startsPerFrame = 0; // characters set going per frame (0: all at once): their first steps run scripts, and a crowd at once was a hitch (the console takes one)
	float prefetchRange = 0.0f; // cm beyond the range: the models of those who will come into it are read in the frames with nothing else to load (0: not; the console 1500)
	u32 clipBudget = 3072u * 1024u; // bytes of animations in memory at most
	bool alive = true;     // false: everybody just stands where his routine has him
	bool perceive = true;  // false: nobody notices anybody (Npc_PercEnable is taken note of, nothing more)
	bool fights = true;    // false: the states of attack are not begun, as before fighting
	// A person does not fall on somebody with no cause: not hostile to it, not hit by it
	// (see Npcs::attackJustified). Creatures are left to their scripts.
	bool calmGuard = true;
	// Whoever is within the reach of its senses notices the player's character at once, from
	// every side, and looks again this often (seconds) while it is there.
	float noticeEvery = 0.25f;
	// ... and without a wall to look past within this share of the reach (heard, not seen).
	float hearShare = 0.4f;
	// A creature hurt that its scripts do not answer, or that sees an enemy they do not
	// answer, attacks on its own (Npcs::provoke).
	bool beastsRetaliate = true;
	// Bodies (OpenGothic's NPC capsules): nobody walks through anybody - the player's character through the
	// inhabitants, they through each other and through him; those that leave the way net (fighting, fleeing,
	// going to somebody) do not walk through walls either. The dead and the knocked out lie in nobody's way.
	bool bodies = true;
	// Hidden behind walls: an inhabitant in view whom thin lines from the eye to its head, chest, sides and knees all
	// meet a wall, a roof or the ground on the way (World::sightBlocked) is not drawn. Nearer than `hiddenNear`, cm,
	// everybody is drawn. One who was seen is looked at again every `hiddenRecheck` frames; one hidden, every frame
	// (so that nobody comes out from behind a corner late).
	bool hideOccluded = true;
	float hiddenNear = 400.0f;
	u32 hiddenRecheck = 6;
	// fix91: one whom the player cannot see (the same lines, to its head and chest) is not read from the archive at all
	// until he can be seen or comes within `hiddenNear` (his models would only be built to be left out); and the pose of
	// one hidden farther than 8 m is not worked out unless `poseHidden` (the report and the tests want it; the console not).
	bool skipHiddenLoads = true;
	bool poseHidden = true;
};

// What the inhabitants need of the player's character in a fight, and how
// their blows reach it (the application decides what a blow does to it).
struct HeroLink {
	bool prehit = false;   // a blow of the player's is on its way (the enemies' fight AI fends it off)
	bool running = false;
	bool walking = false;  // on the move, slower than a run: BS_WALK (BS_SNEAK if sneaking)
	bool jumping = false;  // in the air (a jump, a fall): BS_JUMP
	bool swimming = false; // in the water: BS_SWIM
	bool sneaking = false; // the character creeps: creatures then see it only in front of them
	// An inhabitant's blow lands on the player's character.
	void (*hit)(void* user, u32 attacker) = nullptr;
	void* user = nullptr;
};

struct NpcStats {
	u32 total = 0;     // characters the scripts have made, in all worlds
	u32 here = 0;      // in this world, at a place it has
	u32 near = 0;      // within reach
	float reach = 0;   // how far characters are shown right now, cm
	u32 shown = 0;     // drawn in the last frame
	u32 culled = 0;    // near but out of view
	u32 waiting = 0;   // near, their models not read yet
	u32 noModel = 0;   // near, their look has no models in the archive
	u32 models = 0;    // models in memory
	u32 triangles = 0; // drawn in the last frame
	u32 hidden = 0;    // in view, but behind a wall: not drawn (NpcConfig::hideOccluded)
	u32 sightRays = 0; // lines looked along for that in the last frame
	float sightMs = 0; // ... and how long it took
	u32 drawCalls = 0;
	u32 loads = 0;     // models read since the start
	u32 textures = 0;  // textures in memory, and their size
	u32 textureBytes = 0;
	u32 meshes = 0, meshBytes = 0; // shared meshes in memory (fix88), and their size
	u32 meshHits = 0, meshReads = 0; // asked for and found in memory / read from the archive
	u32 meshesWarmed = 0;           // the most worn ones read at the start
	float warmMs = 0;
	// Life.
	u32 actors = 0;       // characters living their day right now
	u32 walking = 0;      // of them on their way somewhere
	u32 clips = 0;        // animations read since the world was entered
	u32 clipBytes = 0;    // animations in memory
	u32 clipsRefused = 0; // animations not read because the budget was spent
	u32 clipsEvicted = 0; // animations let go to make room for others
	u32 clipsMissing = 0; // animations the scripts asked for that the model does not have
	u32 stateCalls = 0;   // script functions of states called since the start
	u32 actions = 0;      // actions carried out since the start
	u32 queueFull = 0;    // actions dropped because a queue was full
	u32 paths = 0;        // ways found through the net
	u32 noPath = 0;       // ways not found (walked straight instead)
	u32 snapped = 0;      // characters put at their goal because they could not get there
	u32 mobsUsed = 0;     // times somebody has got into a bed, onto a bench ...
	u32 mobsInUse = 0;    // things in use right now
	u32 mobsFailed = 0;   // times somebody stood at a thing and had no animation for using it
	float scriptMs = 0;   // milliseconds the last update() spent inside the scripts' functions
	float clockMs = 0;    // ... setting the world's clock on (the routines of everybody)
	float selectMs = 0;   // ... choosing and making the actors (models, grounding)
	float loadMs = 0;     // ... of it: models built (fix89: for finding what a hitch is made of)
	float wakeMs = 0;     // ... of it: new characters' first steps (scripts)
	float groundMs = 0;   // ... of it: characters put on the ground
	float tickMs = 0;     // ... in the actors' own steps
	// Perception.
	u32 perceived = 0;    // reactions called since the start (the player seen, an enemy, a passive perception)
	u32 fightsRefused = 0; // states of fighting the scripts asked for and did not get (fights switched off)
	u32 looking = 0;      // actors looking at somebody right now
	u32 attacksDenied = 0; // attacks of people on somebody they had no cause for, not begun (NpcConfig::calmGuard)
	u32 provoked = 0;     // attacks the engine began for a creature its scripts left standing
	u32 talksStopped = 0; // characters stopped in their tracks to be talked to
	char lastAttack[56] = ""; // the last attack begun or refused: "who>whom state att" (for reports)
	// Fighting.
	u32 fighting = 0;     // actors with a weapon in hand right now
	u32 blows = 0;        // blows that landed since the start ...
	u32 parried = 0;      // ... and that were fended off
	u32 knockedOut = 0, killed = 0;
	u32 moves = 0;        // moves of the fight AI begun
	u32 movesUntabled = 0; // ... of which chosen without a table for the fighter's tactic
	char fightAi = '-';   // where the tables are from: 'F' the game's FIGHT.DAT, 'S' the main script, '-' not read yet
	// Talks.
	u32 talks = 0;        // talks begun (AI_ProcessInfos)
	u32 infosTold = 0;    // infos the player has chosen or been told
	u32 lines = 0;        // lines said (AI_Output)
	u32 linesMissing = 0; // ... of which the archive has no words for
	u32 linesSaid = 0;    // lines said in the world, outside a talk (AI_OutputSVM, AI_Output of a character in a state)
	u32 linesHeard = 0;   // ... of which had a recording to play
	u32 talksAborted = 0; // talks cut short (the character was hurt, walked off ...)
	u32 trades = 0;       // times the wares came up (a talk went on to trade)
	// Things.
	u32 itemsUsed = 0;    // things used up (AI_UseItem: a potion drunk)
	u32 itemsHeld = 0;    // times somebody has taken a thing in hand to hold it (AI_UseItemToState)
	u32 itemsTaken = 0;   // things picked up from the ground (AI_TakeItem)
	u32 itemsMissing = 0; // things the scripts would have used that the character does not carry
};

// What an actor has been told to do.
enum NpcActionKind : u8 {
	NPC_ACT_NONE,
	NPC_ACT_GOTO_NODE,  // i: way node
	NPC_ACT_GOTO_POS,   // pos, i: free point or -1
	NPC_ACT_GOTO_NEXT_FP, // text: the word to look for
	NPC_ACT_GOTO_NPC,   // i: character
	NPC_ACT_ALIGN,      // to the point it stands at
	NPC_ACT_TURN_NPC,   // i: character
	NPC_ACT_PLAY_ANI,   // text: name; i: body state or -1
	NPC_ACT_WAIT,       // f: seconds
	NPC_ACT_STAND_UP,
	NPC_ACT_WALK_MODE,  // i
	NPC_ACT_START_STATE, // fn; i: run the old state's end first; text: way point
	NPC_ACT_CONTINUE_ROUTINE,
	NPC_ACT_TELEPORT,   // pos (xyz, yaw)
	NPC_ACT_USE_MOB,    // text: the kind of thing ("BENCH"); i: the state to take it to, -1: leave it
	NPC_ACT_LOOK,       // i: character to look at, -1: straight ahead again
	NPC_ACT_SAY,        // a line said in the world, outside a talk with the player: i: character spoken to or -1;
	                    // hash: the line (AI_Output), or text: an SVM phrase ("$WHATDOYOUWANT"); order
	NPC_ACT_OUTPUT,     // i: the line's number among the lines said (AI_Output): done when it has been said
	NPC_ACT_PROCESS_INFOS, // the talk begins (AI_ProcessInfos)
	NPC_ACT_STOP_INFOS, // the talk ends once what is said has been said (AI_StopProcessInfos)
	// Fighting (npc_fight.cpp).
	NPC_ACT_ATTACK,     // a move of the fight AI against the target
	NPC_ACT_DRAW,       // i: the WeaponMode to take in hand (WEAPON_NONE: the best there is)
	NPC_ACT_UNDRAW,     // put the weapon away
	NPC_ACT_DODGE,      // a hop back
	NPC_ACT_FLEE,       // f: seconds of running away from the target
	NPC_ACT_FINISH,     // i: the one lying there, to be finished off
	NPC_ACT_WHIRL,      // i: character to whirl round to
	NPC_ACT_TURN_AWAY,  // i: character to turn the back on
	NPC_ACT_EQUIP_BEST, // the best melee weapon it carries
	// Things (npc_items.cpp).
	NPC_ACT_USE_ITEM,   // i: the item; f: the state to take it to (-1: put it away, -2: use it up, AI_UseItem)
	NPC_ACT_TAKE_ITEM,  // i: the thing in the world (Loot's index); fn: its item
};

struct NpcAction {
	u8 kind = NPC_ACT_NONE;
	i32 i = 0;
	float f = 0;
	u32 fn = 0;
	u32 hash = 0;   // NPC_ACT_SAY: hashName() of the line's name (0: text names it, "$WHATDOYOUWANT")
	u32 order = 0;  // NPC_ACT_SAY: the line's number among all the lines the scripts have given (who speaks first)
	bool started = false; // NPC_ACT_SAY: the line has begun
	union {
		char text[32];
		float pos[4];
	};
	NpcAction() { memset(text, 0, sizeof(text)); }
};

// One of the models in memory.
struct NpcPiece {
	u32 hash = 0;
	u32 used = 0; // frame it was last needed in
	bool failed = false; // could not be read (out of memory, say): tried again after a while
	u32 failedAt = 0;
	Model model;
};

// A character near the player.
struct NpcActor {
	enum { QUEUE = 24, PATH = 40 };
	enum Mode : u8 { WALK = 1, RUN = 0 }; // as the scripts count them (NPC_RUN, NPC_WALK)
	enum Phase : u8 { IDLE, TURNING, MOVING, PLAYING, WAITING };

	bool used = false;
	bool ready = false;    // its models are in memory
	u8 shown = 0;          // how much of it is drawn, 0..255: it dissolves in when it comes into range and out when it leaves
	bool leaving = false;  // out of range: still drawn, dissolving, and let go when nothing is left of it
	u32 npc = 0;           // Game's index
	float dist2 = 0;       // to the player, squared
	NpcPiece* rig = nullptr;
	NpcPiece* body = nullptr;
	NpcPiece* head = nullptr;
	NpcPiece* low = nullptr; // the coarse body, if the look has one
	bool far = false;        // beyond the distance of the coarse body (with hysteresis)
	float tickDebt = 0;      // time of the frames its step was left out
	u8 mix = 255;            // the share of the full body drawn, the rest is the coarse one
	Animator anim;
	bool fist = false;     // a creature: its animations are the "fist" ones (S_FISTRUN ...)
	u32 movedSeen = 0;     // GameNpc::moved it was last put on the ground for
	// Where it stands.
	bool grounded = false; // the ground under it has been found
	u8 light[4] = {255, 255, 255, 255};
	// Its state.
	u32 state = 0;         // the function that begins it; 0: between states
	u32 loopFn = SCRIPT_NONE, endFn = SCRIPT_NONE;
	u32 prevState = 0;
	bool started = false;
	bool routineState = false; // begun by the routine or the start-up state, not by AI_StartState alone
	u32 routineFn = 0;         // what the routine wanted when the state began ...
	GameText routineAt = 0;    // ... and where
	float stateTime = 0;       // seconds in the state
	float stateHours = 0;      // game hours in it
	float loopWait = 0;        // seconds to the next call of the loop
	float percTime = 0;        // Npc_SetPercTime, seconds
	int other = -1;            // "other" of its state functions (a character), -1 for nobody
	// What it is doing.
	NpcAction queue[QUEUE];
	u8 queueHead = 0, queueCount = 0;
	NpcAction action; // the one being carried out (NPC_ACT_NONE: none)
	Phase phase = IDLE;
	float wait = 0;        // seconds left of a wait or an animation
	float turnTo = 0;      // yaw to turn to
	u16 path[PATH];
	u8 pathCount = 0, pathAt = 0;
	int goalNode = -1;     // where the way leads (a path may be longer than `path` holds)
	float target[3] = {0, 0, 0};
	float stuck = 0;       // seconds without getting closer
	float bestDist = 0;
	bool carried = false;  // animations move it (a walk, and the steps that end one)
	float settle = 0;      // seconds the steps that end a walk still take
	// How it holds itself.
	u8 walkMode = RUN;
	char stance[24] = "";  // "" standing; else what it is in: "SIT", "LGUARD", "SLEEP" ...
	i32 bodyState = -1;    // as the scripts last said (AI_PlayAniBS), -1: by what it does
	int atNode = -1;       // the way node it stands at
	int atFree = -1;       // the free point it stands at (it holds it)
	bool attached = false; // it stands at a point that says which way to face ...
	float attachYaw = 0;   // ... this way
	const Clip* idle = nullptr; // what it falls back into
	// The thing it uses - a bed, a bench (mobs.h): going there, getting into
	// it state by state ("stand -> S0 -> S1"), staying, getting out again.
	enum MobStage : u8 { MOB_NONE, MOB_COMING, MOB_TURNING, MOB_ENTERING, MOB_IN, MOB_LEAVING };
	int mob = -1;          // Mobs' index, -1 for none
	u8 mobStage = MOB_NONE;
	i8 mobState = -1;      // -1: standing by it; else the state it is in (S0, S1 ...)
	u8 mobKind = 0;        // MobSlotKind of the place it stands at
	float mobPos[3] = {0, 0, 0}; // that place ...
	float mobYaw = 0;            // ... and where one faces there
	int mobFailed = -1;    // a thing it could not get into (no animation for it): not tried again in this state
	// The names of the last animations looked up, for reports: the model
	// keeps only their hashes.
	enum { NAMES = 4 };
	const Clip* named[NAMES] = {};
	char names[NAMES][28] = {};
	u8 nextName = 0;
	// What it perceives: per perception (the scripts' PERC_ numbers) the
	// function that reacts, 0 for none; when it looks next.
	u32 perc[Game::PERC_TYPES] = {};
	float percWait = 0;
	bool heroWasNear = false; // the player was within the reach of its senses a moment ago
	bool warned = false;      // a creature that had no reaction of its own has already been sent to threaten the player (Npcs::perceive)
	// Who it looks at (AI_LookAtNpc), -1 for nobody; how far its head is
	// turned right now, radians (left/right, down/up).
	int lookAt = -1;
	float headYaw = 0, headPitch = 0;
	char said[24] = ""; // the last thing it said (AI_OutputSVM), for reports
	// Fighting (npc_fight.cpp): the steps of the move in hand, when the next
	// may begin, the blow in play, being hit.
	u8 steps[6] = {};
	u8 stepCount = 0, stepAt = 0;
	float faiWait = 0;     // seconds before the next step
	bool hitFlag = false;  // hit since the last move: the next comes from MY_W_STRAFE
	BlowTracker blow;
	float fightStall = 0;  // seconds a fight action has made no headway (the watchdog)
	float stumble = -1;    // seconds of reeling from a blow (no steps meanwhile); below 0 the time since, down to -1
	float animPrev = 0;    // where the animation was the frame before (for the frame a blow lands at)
	const Clip* animClip = nullptr;
	float flee = 0;        // seconds of fleeing left
	float refuseTalk = 0;  // seconds it will not talk (Npc_SetRefuseTalk)
	u8 drawTo = 0xFF;      // WeaponMode being taken in hand (0xFF: none)
	u8 heardWeapon = 0;    // what it had in hand when that last made a sound
	float drawAt = -1.0f;  // seconds into the draw where it changes hands
	bool drawBack = false; // the weapon is being put away: its drawing plays backwards
	bool hitFromBehind = false;
	// A shot (a bow, a spell) under way: the clip, and when in it the arrow leaves or the spell goes off.
	const Clip* shotClip = nullptr;
	float shotAt = -1.0f;
	// A thing in its hand (npc_items.cpp): AI_UseItemToState has it hold an
	// apple, a bottle, a joint in a state of the item's scheme ("S_FOOD_S0"),
	// which is then what it stands in (`stance` "FOOD_S0").
	u32 heldItem = 0xFFFFFFFFu; // the item, SCRIPT_NONE for none
	i8 itemState = -1;          // the state it holds it in, -1: none (standing)
	u8 itemPhase = 0;           // where an AI_UseItem or AI_TakeItem is: 0 going there, 1 in the middle, 2 back
	char heldScheme[16] = "";   // the item's scheme ("FOOD")
	// A body whose model has no way of falling is tipped over onto its side
	// instead (`slump` 0..1, drawn as a lean), so that it still lies.
	bool noLying = false;
	float slump = 0.0f;
	// How wide its body is for colliding, cm (from its model, once drawn; a person's until then).
	float bodyRadius = 30.0f;
	// Hidden behind walls (NpcConfig::hideOccluded): as last looked at, and the frame that was.
	bool occluded = false;
	u32 occlusionFrame = 0;
	// select(): looked at with no models yet - hidden from the eye (fix91), and the frame that was.
	bool unseen = false;
	u32 unseenFrame = 0;
};

class Npcs {
public:
	Npcs();
	~Npcs() { release(); }
	Npcs(const Npcs&) = delete;
	Npcs& operator=(const Npcs&) = delete;

	// Loads the scripts and the table of looks. False if the archive has no
	// scripts: the worlds are then empty, as before.
	bool load(Pak& pak);
	void release();
	bool loaded() const { return _game.loaded(); }

	// The world `asset` ("world/oldmine") has been entered at this time of
	// day (hours): its start-up runs if it has not yet, everybody takes his
	// place, and the models of those around `eye` are read at once.
	void enterWorld(Pak& pak, const World& world, const char* asset, Vec3 eye, float hours);
	// The world is about to be closed: the models go (the next world wants
	// the memory before its people are known).
	void leaveWorld();
	// Once per frame: the clock, who is near, whose models are still to
	// read, and a step of everybody's life. `eye` is the player's.
	void update(Pak& pak, const World& world, Vec3 eye, float hours, float dt);
	// Call in PASS_WORLD. `dayLight` as for the world (1 1 1 indoors). With `world`, those hidden from `eye` behind
	// its walls are left out (NpcConfig::hideOccluded).
	void draw(const Mat4& viewProj, const float dayLight[3], const World* world = nullptr, Vec3 eye = {0, 0, 0});
	// How far a dissolve gets this frame (World::fadeStep(); 255: they come and go at once).
	void setFadeStep(int step) { _fadeStep = step < 1 ? 1 : step > 255 ? 255 : step; }
	// Whose models and archive the weapons in their hands are drawn from.
	void setHeldSource(class Loot* loot, Pak* pak) { _heldLoot = loot; _heldPak = pak; }
	// Models, textures and animations of those about to come into range are read ahead on a thread of its own.
	void setReadAhead(class ReadAhead* ahead) { _ahead = ahead; }

	NpcConfig& config() { return _cfg; }
	const NpcStats& stats() const { return _stats; }
	Game& game() { return _game; }
	const Game& game() const { return _game; }
	const WayNet& wayNet() const { return _net; }
	const Mobs& mobs() const { return _mobs; }
	Mobs& mobs() { return _mobs; }
	// The world's name in the scripts ("WORLD"), "" before a world is entered.
	const char* worldScript() const { return _script; }
	// The actor a character is right now, or null.
	const NpcActor* actorOf(u32 npcIndex) const;
	u32 actorCount() const { return ACTORS; }
	// The characters drawn in the last frame that stand on their feet (for
	// their shadows, Graphics+): where they stand and how wide they are, cm.
	u32 drawnCount() const { return _drawnCount; }
	Vec3 drawnFeet(u32 i) const { return _drawnFeet[i]; }
	float drawnRadius(u32 i) const { return _drawnRadius[i]; }
	// Whether it was drawn, or (in view, but behind a wall) only counted for its feet.
	bool drawnVisible(u32 i) const { return !_drawnHidden[i]; }
	const NpcActor& actor(u32 i) const { return _actors[i]; }
	// What an actor is doing, in a word (for reports).
	const char* doing(const NpcActor& a) const;
	// The name of the function that begins a state ("ZS_SLEEPBED"), "" for
	// none or if the archive does not have the names.
	const char* stateName(u32 fn) const;
	// The name of the animation an actor is in, "" if not known.
	const char* animationName(const NpcActor& a) const;
	// What the scripts are told its body is doing (Npc_GetBodyState).
	i32 bodyState(const NpcActor& a) const { return bodyStateOf(a); }
	// The actor nearest to the player that has its models, or null.
	const NpcActor* nearest() const;
	// ---- fighting (npc_fight.cpp) ----
	void setHeroLink(const HeroLink& link) { _heroLink = link; }
	// Told of the sound events of the clips the characters play.
	void setSoundHook(const SoundEventHook& hook) { _soundHook = hook; }
	// Told of the fight's other sounds, by the scripts' names (C_SFX): `names` is a list
	// "CS_IAM_ME_FL|CS_IAM_UD_FL|CS_IAM" to try in turn, the first one known sounds, at
	// character `npc`.
	struct NamedSoundHook {
		void (*fn)(void* user, const char* names, int npc, float gain) = nullptr;
		void* user = nullptr;
	};
	void setNamedSoundHook(const NamedSoundHook& hook) { _namedSoundHook = hook; }
	// The sounds themselves, as ZenGin and OpenGothic make them (also for the player's character):
	void emitSound(u32 npc, const char* names, float gain = 1.0f);
	void emitSvm(u32 npc, const char* what);              // its voice: "SVM_<voice>_DEAD", "_AARGH"
	void emitHit(u32 attacker, u32 victim);               // a blow lands: "CS_IAM_<weapon>_<armour>", "CS_MAM"
	void emitParry(u32 npc);                              // weapon on weapon: "CS_IAI_ME_ME"
	void emitDraw(u32 npc, u8 from, u8 to, u32 weaponItem); // "DRAWSOUND_ME"/"_WO", "UNDRAWSOUND_..."
	// The voices: `play` is told a line (hashName of its name) is begun by a character (-1: nobody known; `hero`:
	// the player's) and returns how many seconds the recording lasts, 0 if it has none; `stop` ends it early.
	// `say`: a line spoken in the world (a quarrel, a greeting, two guards talking), not in the player's talk: it
	// sounds from the speaker's place. Returns the seconds it lasts, 0 if there is no recording, and -1 if the speaker
	// is too far to be heard (nobody waits for it then). `overlay`: said in passing (AI_OutputSVM_Overlay).
	struct SpeechHook {
		float (*play)(void* user, u32 lineHash, int speaker, bool hero) = nullptr;
		void (*stop)(void* user) = nullptr;
		float (*say)(void* user, u32 lineHash, int speaker, bool overlay) = nullptr;
		void* user = nullptr;
	};
	void setSpeechHook(const SpeechHook& hook) { _speechHook = hook; }
	// Arrows and spells of the inhabitants: the application flies them (as the player's own). `spell` < 0: an arrow
	// (or a bolt) of the shooter's bow; else that spell at `level`. False if it could not.
	struct ShotHook {
		bool (*fire)(void* user, u32 shooter, int target, i32 spell, i32 level) = nullptr;
		void* user = nullptr;
	};
	void setShotHook(const ShotHook& hook) { _shotHook = hook; }
	// Somebody near is after the player's character in a fight state, with a weapon drawn or a beast (the music's
	// "targeted": OpenGothic's WorldObjects::isTargetedBy).
	bool targetsHero() const;
	// Which way the player's character faces (the scripts and the fight AI look at it).
	void setHeroYaw(float yaw) { _heroYaw = yaw; }
	// A perception for one actor only (PERC_ASSESSTHREAT: the player aims at it).
	void perceiveSingle(u32 receiver, const char* percName, int other);
	// A blow of `attacker` (the player's character, or an actor) reaches
	// `victim`: parried, or damage, reactions, maybe down. Returns the damage
	// done, -1 if it was fended off or could not be done.
	i32 strike(u32 attacker, u32 victim, bool fromBehind);
	// The player's character does something the inhabitants about perceive
	// (the scripts' PERC_DRAWWEAPON, PERC_ASSESSFIGHTSOUND ...): a passive
	// perception sent by `sender` with OTHER and VICTIM as given.
	void sendPerception(const char* percName, u32 sender, int other, int victim);
	// The same with the scripts' number.
	void sendPerception(i32 perc, u32 sender, int other, int victim);
	// One line about the nearest character to the player within 15 m (for the FPS counter's debug line):
	// who, state, whether its enemy perception is on, its wait, attitude, distance. False if none.
	bool debugNearest(char* out, size_t size) const;
	// What the last blow on somebody (not the player's character) did to it, and the last external the
	// scripts called that nobody implements (its name hash): for the debug lines. False if nothing yet.
	bool debugLastHit(char* out, size_t size, int row = 0) const;
	u32 lastUnbound() const { return _lastUnbound; }
	u32 unboundCount() const { return _unboundCount; }
	// The distinct unimplemented externals called so far (name hashes, up to 6).
	u32 unboundSeen(u32 i) const { return i < _unboundSeenCount ? _unboundSeen[i] : 0; }
	u32 unboundSeenCount() const { return _unboundSeenCount; }
	// Everybody within the reach of its senses of `source` looks round at its next frame.
	void noticeNow(u32 source);
	// Somebody has gone down (HP, scripts' state ZS_Unconscious or ZS_Dead):
	// called for the player's character too, to tell those about.
	void goDown(u32 npc, int attacker, bool dead, bool fromBehind);
	// The inhabitants' weapon in hand: whether it is drawn, for the HUD.
	bool isDown(u32 npc) const { return npc < _game.npcCount() && _game.npc(npc).down != DOWN_NONE; }
	// Of the actors, the one in front of `eye` looking along `yaw` that best
	// fits (near, in the middle of the view), not down; `except`: skip that
	// one; `side` +1/-1: only those to the right/left of `from`'s direction.
	int pickTarget(const float pos[3], float yaw, float range, float halfAngle, int except = -1, int side = 0, bool hostileOnly = false) const;
	// The living about a point that an area spell hits (all but the player's friends).
	u32 areaTargets(const float pos[3], float range, float elev, u32* out, u32 max) const;

	// ---- talks (npc_dialog.cpp) ----
	// The player talks to a character (A): it hears it as a perception
	// (PERC_ASSESSTALK, the scripts' B_AssessTalk) and its ZS_Talk begins.
	// False if it cannot be talked to now.
	bool talkTo(u32 npc);
	// A character the player could talk to: in sight, on its feet, and with
	// something to say or a mind to listen (the scripts give it infos, or a
	// reaction to PERC_ASSESSTALK).
	bool canTalk(u32 npc) const;
	// A talk is on, or asked for and about to begin (also one a character
	// began by itself: an important info).
	bool talking() const { return _talk.processing || _talk.requested >= 0; }
	int talkPartner() const { return _talk.processing ? _talk.npc : _talk.requested; }
	// What the screens show.
	const TalkView& talkView() const { return _talkView; }
	// The player picks way `index` of the menu; false if there is no menu.
	bool talkChoose(u32 index);
	// A: the line being said is skipped. B: the last way of the menu is taken
	// (by the scripts' convention the one that leaves), or the line is skipped.
	void talkSkip();
	bool talkBack();
	// An info with C_INFO.TRADE was told: the bottom screen is the wares (talkView().trading) until trade is
	// closed (tradeClose), and the talk goes on from the menu.
	bool trading() const { return _talk.processing && _talk.trading; }
	void tradeClose();
	// The words of the lines (for reports and tests).
	DialogTexts& dialogTexts() { return _texts; }
	// Ends the talk at once, without the scripts' say (the world changes, the player is hurt ...).
	void talkAbort();
	// The player's character talks to himself: the scripts' AI_ProcessInfos(hero) - a bed's
	// "SLEEPABIT_S1" puts up his own infos (C_INFO with npc = the hero: "sleep till morning" ...).
	bool selfTalk() const { return _talk.processing && _talk.self; }
	// A bed whose scripts put up nothing: the engine's own menu of the hours to sleep until.
	// False if a talk is on.
	bool openSleepMenu();
	// The hour chosen in it (0..24), once; false while none is.
	bool takeSleep(float& hours);

	// The thing of that kind a character would use now: the nearest free
	// one it can see from where it stands (the scripts' Wld_IsMobAvailable,
	// AI_UseMob), or -1.
	// The scripts sent the player's character somewhere (AI_Teleport: a rune, a quest): where its feet go, once.
	bool takeHeroTeleport(float pos[3], float& yaw);

	// The model of an overlay ("humans_militia": "model/ovl.humans_militia"), read the first time it is asked for;
	// null if the archive has none. Kept for the session (clips of it may be playing anywhere).
	const Model* overlayModel(GameText name);
	// Those of a character, in the order applied (for the player's character: Hero::setOverlays).
	u32 overlayModels(u32 npc, const Model** out, u32 max);
	u32 overlaysLoaded() const { return _overlayCount; }

	// Moves the player's character's feet out of the inhabitants' bodies (`radius` its own); true if moved.
	bool pushHero(Vec3& feet, float radius) const;
	// Bodies pushed apart since the start (for reports).
	u32 bodyPushes() const { return _bodyPushes; }

	// ---- rooms and owners (npc_world.cpp) ----
	// The rooms of the world entered (none in an archive baked before them).
	const Rooms& rooms() const { return _rooms; }
	// The player's character takes a thing from the world: those about perceive a theft (PERC_ASSESSTHEFT,
	// ITEM the thing, as OpenGothic's Npc::takeItem).
	void heroTook(u32 itemSymbol, i32 amount);
	// ... begins to use a thing of the world (Mobs' index; a chest, a bench, a door): PERC_ASSESSUSEMOB
	// (OpenGothic's Interactive), and that thing is what Npc_IsDetectedMobOwnedByNpc asks about.
	void heroUsesMob(int mob);
	// Rooms entered and left by the player's character (PERC_ASSESSENTERROOM, OpenGothic's MoveAlgo) for the
	// character's feet; NpcStats::roomsEntered counts them.
	u32 roomsEntered() const { return _roomsEntered; }

	int visibleMob(const World& world, u32 npc, const char* kind) {
		const World* before = _world;
		_world = &world;
		const int mob = findMob(npc, kind, -1);
		_world = before;
		return mob;
	}

private:
	SoundEventHook _soundHook;
	NamedSoundHook _namedSoundHook;
	SpeechHook _speechHook;
	enum { PIECES = 48, ACTORS = 16 }; // three models each
	typedef NpcPiece Piece;

	static bool findPoint(void* self, const char* name, float pos[3], float& yaw);
	const LookEntry* lookOf(const GameLook& look) const;
	Piece* piece(Pak& pak, u32 hash, u32& loadsLeft);
	// fix89: with a read-ahead a model is only built once its bytes are waiting in memory (it is wished first); the card
	// is read on the frame's thread only for one that has not come after a while (a wish refused or let go).
	bool bytesReady(u32 hash);
	struct Wish {
		u32 hash = 0, since = 0;
	};
	Wish _wishes[32];
	u32 _wishNext = 0;
	void warmMeshes(Pak& pak);
	bool loadFits() { return !_cfg.budget || _cfg.budget->allow(_pieceCostMs, _refusedPieces); }
	float _pieceCostMs = 6.0f; // estimate of what reading one model takes, kept as they are timed
	u32 _refusedPieces = 0;
	void select(Pak& pak, const World& world, Vec3 eye, u32 loadsLeft);
	void retire(NpcActor& a);
	void ground(NpcActor& a, const World& world, float above);

	// ---- life (npc_ai.cpp) ----
	void bindAi();
	void wake(NpcActor& a);
	void tick(NpcActor& a, const World& world, float dt, float hoursPassed);
	void tickState(NpcActor& a);
	void startState(NpcActor& a, u32 fn, bool finalizeOld, const char* wp, bool byRoutine);
	void clearState(NpcActor& a, bool finalize);
	// Stops what it is doing at once (a walk, a wait, an animation of the queue) and drops
	// what was to follow: it stands, ready for whatever comes next.
	void abortActions(NpcActor& a);
	i32 invoke(NpcActor& a, u32 fn, int victim = -1);
	bool begin(NpcActor& a, const World& world);
	bool carryOn(NpcActor& a, const World& world, float dt);
	bool walkTo(NpcActor& a, const float target[3], float radius, float dt, bool last);
	bool turn(NpcActor& a, float yaw, float rate, float dt);
	bool taken(const NpcActor& a, const float pos[3]) const;
	void startWalking(NpcActor& a);
	void stopWalking(NpcActor& a);
	bool standUp(NpcActor& a);
	bool playAni(NpcActor& a, const char* name, i32 bodyState);
	void fallBack(NpcActor& a, float blend);
	void planWay(NpcActor& a, int node);
	void attachTo(NpcActor& a, int node, int freePoint);
	void clearQueue(NpcActor& a);
	bool push(NpcActor* a, const NpcAction& act);
	void pushFront(NpcActor& a, const NpcAction& act);
	// `force`: read even past the budget when nothing can be let go (a fall must not be missing).
	const Clip* clip(NpcActor& a, const char* name, bool force = false);
	bool evictClip(); // lets the animation not played for longest go; false: nothing to let go
	const Clip* modeClip(NpcActor& a, const char* pattern, bool orOtherMode);
	NpcActor* actorFor(ScriptInstance* inst);
	int npcFor(ScriptInstance* inst) const { return _game.indexOf(inst); }
	i32 bodyStateOf(const NpcActor& a) const;
	// The player's character's, as the scripts see it (BS_RUN, BS_JUMP, BS_SWIM ... for the pursuit that gives up).
	i32 heroBodyState() const;
	char _lastHitNote[64] = {};
	float _lastHitAge = 1e9f;
	u32 _lastUnbound = 0, _unboundCount = 0;
	u32 _unboundSeen[6] = {};
	u32 _unboundSeenCount = 0;
	int _lastHitNpc = -1;
	static void unboundThunk(u32 symbol, void* user);
	const Clip* mobClip(NpcActor& a, int from, int to, char* nameOut);
	void releaseMob(NpcActor& a);
	bool useMob(NpcActor& a, const World& world, float dt);
	int findMob(u32 npc, const char* kind, int except) const;

	// ---- fighting (npc_fight.cpp) ----
	void bindFight();
	// The fight AI tables of "script/fight", if the archive has them
	// (FIGHT.DAT); false leaves them to be read from the main script.
	bool loadFightAi(Pak& pak);
	bool beginFight(NpcActor& a, const World& world);   // begin() for the fight actions
	bool carryOnFight(NpcActor& a, const World& world, float dt);
	void tickFight(NpcActor& a, float dt);               // blows landing, being hit, drawing
	bool fetchMove(NpcActor& a);
	bool doStep(NpcActor& a, const World& world, float dt);
	bool startDraw(NpcActor& a, u8 mode);
	bool attackClipPlay(NpcActor& a, int kind);          // 0 ahead, 1 left, 2 right
	bool inFocus(const NpcActor& a, u32 target, float halfAngle) const;
	float distanceTo(const NpcActor& a, u32 target) const;
	bool targetValid(const NpcActor& a) const;
	bool prehit(u32 npc) const;
	void fightIdle(NpcActor& a, float blend);
	const Clip* weaponClip(NpcActor& a, const char* pattern);
	void endFightState(NpcActor& a);
	int nextTarget(const NpcActor& a, bool set);
	// ---- ranged fighting: bows and spells (npc_fight.cpp) ----
	ShotHook _shotHook;
	bool rangedWeapon(const GameNpc& n) const { return n.weapon == WEAPON_BOW || n.weapon == WEAPON_CBOW || n.weapon == WEAPON_MAG; }
	float rangedReach(const GameNpc& n) const;
	bool startShot(NpcActor& a);
	void fireShot(NpcActor& a);
	static void xReadyRanged(ScriptVm& vm, void* user);
	static void xEquipBestRanged(ScriptVm& vm, void* user);
	static void xReadySpell(ScriptVm& vm, void* user);
	static void xUnreadySpell(ScriptVm& vm, void* user);
	static void xIsDrawingSpell(ScriptVm& vm, void* user);
	static void xHasSpell(ScriptVm& vm, void* user);
	static void xSpellIsScroll(ScriptVm& vm, void* user);
	static void xSetSpellInfo(ScriptVm& vm, void* user);
	u32 _shotsFired = 0, _spellsCast = 0;
public:
	u32 shotsFired() const { return _shotsFired; }
	u32 spellsCast() const { return _spellsCast; }
private:
	static void xAttack(ScriptVm& vm, void* user);
	static void xReadyMelee(ScriptVm& vm, void* user);
	static void xDrawWeapon(ScriptVm& vm, void* user);
	static void xRemoveWeapon(ScriptVm& vm, void* user);
	static void xEquipBestMelee(ScriptVm& vm, void* user);
	static void xDodge(ScriptVm& vm, void* user);
	static void xFlee(ScriptVm& vm, void* user);
	static void xFinishingMove(ScriptVm& vm, void* user);
	static void xWhirlAround(ScriptVm& vm, void* user);
	static void xTurnAway(ScriptVm& vm, void* user);
	static void xGetNextTarget(ScriptVm& vm, void* user);
	static void xIsNextTargetAvailable(ScriptVm& vm, void* user);
	static void xNoAction(ScriptVm& vm, void* user);
	// The fight AI tables, per tactic: the moves of each situation (0 = none).
	enum { TACTICS = 40 };
	struct FaiTable {
		bool read;
		u8 moves[FS_COUNT][6];
	};
	FaiTable _fai[TACTICS] = {};
	u8 moveOf(i32 scriptMove) const;
	i32 _scriptMoves[FM_COUNT] = {};
	HeroLink _heroLink;
	float _heroYaw = 0;
	u32 _fnUnconscious = SCRIPT_NONE, _fnDead = SCRIPT_NONE;
	i32 _bsHit = 0, _bsParade = 0, _bsStumble = 0, _bsDead = 0, _bsUnconscious = 0, _bsRun = 3;
	i32 _percDamage = -1, _percOthersDamage = -1, _percFightSound = -1, _percDefeat = -1, _percMurder = -1, _percFighter = -1,
	    _percBody = -1, _percDrawWeapon = -1, _percRemoveWeapon = -1, _percThreat = -1;

	// ---- things in the hands and in the world (npc_items.cpp) ----
	void bindItems();
	bool beginItem(NpcActor& a, const World& world);
	bool carryOnItem(NpcActor& a, const World& world, float dt);
	bool itemStep(NpcActor& a);
	bool takeStep(NpcActor& a, const World& world, float dt);
	void dropHeldItem(NpcActor& a);
	void checkHeldItem(NpcActor& a);
	const Clip* itemClip(NpcActor& a, int from, int to);
	bool lootPosition(const ScriptInstance* inst, float pos[3]) const;
	int lootOf(const ScriptInstance* inst) const;
	static void xUseItem(ScriptVm& vm, void* user);
	static void xUseItemToState(ScriptVm& vm, void* user);
	static void xTakeItem(ScriptVm& vm, void* user);
	static void xGotoItem(ScriptVm& vm, void* user);
	static void xDetectItem(ScriptVm& vm, void* user);
	static void xCanSeeItem(ScriptVm& vm, void* user);
	static void xDistToItem(ScriptVm& vm, void* user);
	i32 _bsItem = 14 | (1 << 15); // BS_ITEMINTERACT
	i32 _bsTakeItem = 17;         // BS_TAKEITEM

	// ---- rooms and owners, and the smaller functions of the scripts (npc_world.cpp) ----
	void bindWorld();
	void trackHeroRoom(Vec3 feet);
	u16 roomOf(const float pos[3]) const;
	int mobUsedBy(int npc) const;
	Rooms _rooms;
	int _heroMob = -1;
	void collide(NpcActor& a, const World& world, const float before[3]);
	enum { OVERLAY_MODELS = 8 };
	struct OverlayModel {
		u32 hash = 0;
		bool failed = false;
		Model model;
	};
	OverlayModel _overlayModels[OVERLAY_MODELS];
	u32 _overlayCount = 0;
	u32 _bodyPushes = 0;
	float _heroTeleport[4] = {0, 0, 0, 0};
	bool _heroTeleportPending = false;
	u32 _roomsEntered = 0;
	u32 _roomCandidate = 0, _roomFrames = 0;
	i32 _percEnterRoom = -1, _percTheft = -1, _percUseMob = -1;
	static void xMobOwnedByNpc(ScriptVm& vm, void* user);
	static void xMobOwnedByGuild(ScriptVm& vm, void* user);
	static void xLookAtTarget(ScriptVm& vm, void* user);
	static void xHasBodyFlag(ScriptVm& vm, void* user);
	static void xNpcPlayAni(ScriptVm& vm, void* user);
	static void xNpcStopAni(ScriptVm& vm, void* user);
	static void xIsInRoutine(ScriptVm& vm, void* user);
	static void xDetectNpcEx(ScriptVm& vm, void* user);
	static void xHeightToItem(ScriptVm& vm, void* user);
	static void xSetNpcsToState(ScriptVm& vm, void* user);

	// ---- talks (npc_dialog.cpp) ----
	void bindDialog();
	void tickTalk(float dt);
	void tickSpeech(float dt);
	void afterTold();
	void openTalk(NpcActor& a);
	void finishTalk();
	void buildMenu();
	void runInfo(u32 info);
	void refreshView();
	bool speechPending(u32 id) const;
	void flushSpeech();
	NpcActor* actorByNpc(u32 npc);
	static void xOutput(ScriptVm& vm, void* user);
	// A line of the player's talk (shown, spoken in turn) / of the world (said where its speaker stands).
	void queueTalkLine(ScriptInstance* speakerInst, u32 hash);
	bool queueWorldLine(ScriptInstance* speakerInst, int target, u32 hash, const char* svm);
	bool talkLineOf(int speaker) const;
	// The name of the line behind an SVM phrase for a character's voice ("$WHATDOYOUWANT" -> "SVM_3_WHATDOYOUWANT"),
	// the phrase itself if it has no "$". False if the voice has none.
	bool svmLine(u32 npc, const char* phrase, char* out, u32 size);
	bool sayMayStart(NpcActor& a, const NpcAction& act) const;
	float saySpeak(NpcActor& a, NpcAction& act);
	struct SvmMemo {
		u32 voice = 0, phraseHash = 0;
		char line[44] = "";
		bool used = false;
	};
	enum { SVM_MEMO = 32 };
	SvmMemo _svmMemo[SVM_MEMO];
	u32 _svmMemoNext = 0;
	u32 _sayOrder = 1;       // the next line's number
	float _overlayBarrier = 0; // seconds until another phrase in passing may be said (one at a time, as OpenGothic)
	static void xProcessInfos(ScriptVm& vm, void* user);
	static void xStopProcessInfos(ScriptVm& vm, void* user);
	static void xInfoManagerHasFinished(ScriptVm& vm, void* user);
	enum { SPEECH = 24 };
	struct SpeechStep {
		u32 hash;
		int speaker; // Game's index
		u32 id;
	};
	SpeechStep _speech[SPEECH] = {};
	u8 _speechHead = 0, _speechCount = 0;
	u32 _speechNext = 1, _speechDone = 1; // numbers below _speechDone have been said
	float _speechWait = 0;                // seconds the first step has waited for its speaker
	struct Saying {
		bool active = false;
		u32 id = 0;
		int speaker = -1;
		float time = 0, total = 0;
	} _say;
	char _sayText[200] = "";
	struct Talk {
		bool processing = false;
		int requested = -1;   // asked for by talkTo(), not yet begun
		float wait = 0;
		int npc = -1;
		u32 state = 0;        // the state of the partner that the talk belongs to (ZS_Talk)
		bool stop = false;    // AI_StopProcessInfos was reached
		int importantLeft = 0; // important infos the talk may still begin with (it is at most a few)
		bool choosing = false;
		bool tradeNext = false; // the info just run is a trade: the wares come up when it has been told
		bool trading = false;   // the wares are up
		int menuInfo = -1;    // the info whose choices are up, -1 for the main menu
		u32 count = 0;
		u32 ref[TALK_ITEMS] = {};
		bool isChoice[TALK_ITEMS] = {};
		bool self = false;    // the player's character with himself (no partner: his own infos)
		bool sleepMenu = false; // the engine's menu of hours (openSleepMenu)
	} _talk;
	float _sleepTo = -1.0f;
	void openSelfTalk();
	TalkView _talkView;
	bool _menuDirty = false;
	DialogTexts _texts;
	i32 _percTalk = -1;
	u32 _fnTalk = SCRIPT_NONE;

	// ---- perception (npc_perc.cpp) ----
	void bindPerception();
	void perceive(NpcActor& a, const World& world);
	void react(NpcActor& a, u32 fn, int other, int victim);
	bool sees(const NpcActor& a, u32 other, bool inFront) const;
	bool seenAllRound(const GameNpc& n, const GameNpc& o) const;
	bool heroNear(const GameNpc& n) const;
	bool refusesState(u32 fn) const;
	// Whether a person may begin an attack on `other`: it is hostile to it (temporarily too), or
	// was hit by it, or has been told to by the scripts' setting a target of its own.
	bool attackJustified(const NpcActor& a, int other) const;
	void noteAttack(const NpcActor& a, int other, u32 fn, bool allowed);
	// Sets a creature on `attacker` when its scripts did nothing about being hit / seeing an
	// enemy. False if it is not one for that or cannot.
	bool provoke(NpcActor& a, int attacker);
	bool inFightState(const NpcActor& a) const { return a.state && refusesState(a.state); }
	void turnHead(NpcActor& a, float dt);
	static void xPercEnable(ScriptVm& vm, void* user);
	static void xPercDisable(ScriptVm& vm, void* user);
	static void xPerceiveAll(ScriptVm& vm, void* user);
	static void xSendSinglePerc(ScriptVm& vm, void* user);
	static void xSendPassivePerc(ScriptVm& vm, void* user);
	static void xLookAtNpc(ScriptVm& vm, void* user);
	static void xStopLookAt(ScriptVm& vm, void* user);
	static void xOutputSvm(ScriptVm& vm, void* user);
	static void xOutputSvmOverlay(ScriptVm& vm, void* user);
	static void xCanSeeNpcFreeLos(ScriptVm& vm, void* user);
	static void xQuickLook(ScriptVm& vm, void* user);
	i32 _percPlayer = 1, _percEnemy = 2; // the scripts' numbers of the active perceptions
	u32 _fnAttackBeast = SCRIPT_NONE, _fnAttackPerson = SCRIPT_NONE; // ZS_MM_Attack, ZS_Attack
	u32 _fnAssessBeast = SCRIPT_NONE; // ZS_MM_AssessEnemy: the creature's own threat (turn, growl, wait) before it attacks
	bool threaten(NpcActor& a, int who);
	enum { FIGHT_STATES = 16 };
	u32 _fightStates[FIGHT_STATES] = {};
	u32 _fightStateCount = 0;
	static float distance(const float a[3], const float b[3]);

	// The engine functions of life the scripts call.
	static void xGotoWp(ScriptVm& vm, void* user);
	static void xGotoFp(ScriptVm& vm, void* user);
	static void xGotoNextFp(ScriptVm& vm, void* user);
	static void xGotoNpc(ScriptVm& vm, void* user);
	static void xAlign(ScriptVm& vm, void* user);
	static void xTurnToNpc(ScriptVm& vm, void* user);
	static void xPlayAni(ScriptVm& vm, void* user);
	static void xPlayAniBs(ScriptVm& vm, void* user);
	static void xWait(ScriptVm& vm, void* user);
	static void xWaitMs(ScriptVm& vm, void* user);
	static void xSetRefuseTalk(ScriptVm& vm, void* user);
	static void xRefuseTalk(ScriptVm& vm, void* user);
	static void xStandUp(ScriptVm& vm, void* user);
	static void xSetWalkMode(ScriptVm& vm, void* user);
	static void xStartState(ScriptVm& vm, void* user);
	static void xContinueRoutine(ScriptVm& vm, void* user);
	static void xTeleport(ScriptVm& vm, void* user);
	static void xClearQueue(ScriptVm& vm, void* user);
	static void xDistToWp(ScriptVm& vm, void* user);
	static void xDistToNpc(ScriptVm& vm, void* user);
	static void xDistToPlayer(ScriptVm& vm, void* user);
	static void xNearestWp(ScriptVm& vm, void* user);
	static void xNextWp(ScriptVm& vm, void* user);
	static void xIsOnFp(ScriptVm& vm, void* user);
	static void xIsFpAvailable(ScriptVm& vm, void* user);
	static void xIsNextFpAvailable(ScriptVm& vm, void* user);
	static void xIsInState(ScriptVm& vm, void* user);
	static void xWasInState(ScriptVm& vm, void* user);
	static void xGetStateTime(ScriptVm& vm, void* user);
	static void xSetStateTime(ScriptVm& vm, void* user);
	static void xGetBodyState(ScriptVm& vm, void* user);
	static void xSetPercTime(ScriptVm& vm, void* user);
	static void xDetectNpc(ScriptVm& vm, void* user);
	static void xCanSeeNpc(ScriptVm& vm, void* user);
	static void xUseMob(ScriptVm& vm, void* user);
	static void xIsMobAvailable(ScriptVm& vm, void* user);
	static void xGetMobState(ScriptVm& vm, void* user);
	static void xGetDetectedMob(ScriptVm& vm, void* user);

	Game _game;
	NpcConfig _cfg;
	NpcStats _stats;
	LookEntry* _looks = nullptr;
	u32 _lookCount = 0;
	WorldPoint* _points = nullptr;
	u32 _pointCount = 0;
	char _script[32] = "";
	WayNet _net;
	Mobs _mobs;
	// What the body is doing while it uses a thing, by the thing's kind: the
	// scripts' own lists ("BENCH,CHAIR,THRONE" is sitting) and numbers.
	char _mobSit[96] = "", _mobLie[96] = "", _mobClimb[96] = "", _mobBusy[160] = "";
	i32 _bsSit = 0, _bsLie = 0, _bsClimb = 0, _bsMob = 0, _bsMobInterrupt = 0;
	char* _stateNames = nullptr; // "script/states"
	u32 _stateNamesSize = 0;
	TexturePool _pool;
	MeshPool _meshes;
	Piece _pieces[PIECES];
	NpcActor _actors[ACTORS];
	Vec3 _drawnFeet[ACTORS] = {};
	float _drawnRadius[ACTORS] = {};
	bool _drawnHidden[ACTORS] = {};
	u32 _drawFrame = 0;
	u32 _drawnCount = 0;
	class Loot* _heldLoot = nullptr;
	class ReadAhead* _ahead = nullptr;
	Pak* _heldPak = nullptr;
	Pak* _pak = nullptr; // the archive of the world entered (animations are read from it at any time)
	const World* _world = nullptr; // ... and the world
	u32 _clipBytes = 0;
	u32 _victim = SCRIPT_NONE; // the scripts' global of that name
	u32 _frame = 0;
	// Who may come into reach (fix87): select() looks at these, not at every character of the game every frame.
	// A sweep through all of them, a slice a frame, makes the next list; the list in use was made with the eye
	// within `kNearMargin` of where it is (else, and after a world change, it is made anew at once).
	u32* _nearList = nullptr;   // candidates in use
	u32* _sweepList = nullptr;  // ... being made
	u32* _seen = nullptr;       // per character: the select() it was last looked at in (no one twice a frame)
	u32 _nearCount = 0, _sweepCount = 0, _sweepPos = 0, _nearCap = 0, _sweepHere = 0, _nearHere = 0;
	u32 _selectStamp = 0;
	u32 _nearNpcs = 0;          // characters there were when the list was made (more since: those are looked at too)
	u16 _nearWorld = 0xFFFF;
	Vec3 _nearEye {0, 0, 0}, _sweepEye {0, 0, 0};
	static constexpr float kNearMargin = 2500.0f; // cm
	bool nearListFits(u32 npcs);
	void freeNearLists();
	float _clock = 0;
	float _hours = -1.0f;
	int _fadeStep = 255;
	float _reach = 3000.0f; // how far characters are shown right now, cm
};

} // namespace g3
