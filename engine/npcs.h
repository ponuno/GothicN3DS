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
#include "game.h"
#include "mobs.h"
#include "model.h"
#include "waynet.h"
#include "world.h"

#include <string.h>

namespace g3 {

struct NpcConfig {
	float range = 3000.0f; // cm: how far away a character is still shown (less under load, see update())
	u32 maxShown = 12;     // at most this many, the nearest (up to 16)
	u32 loadsPerFrame = 1; // models read from the archive per frame (each is a hitch)
	u32 clipBudget = 1536u * 1024u; // bytes of animations in memory at most
	bool alive = true;     // false: everybody just stands where his routine has him
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
	u32 drawCalls = 0;
	u32 loads = 0;     // models read since the start
	u32 textures = 0;  // textures in memory, and their size
	u32 textureBytes = 0;
	// Life.
	u32 actors = 0;       // characters living their day right now
	u32 walking = 0;      // of them on their way somewhere
	u32 clips = 0;        // animations read since the world was entered
	u32 clipBytes = 0;    // animations in memory
	u32 clipsRefused = 0; // animations not read because the budget was spent
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
};

struct NpcAction {
	u8 kind = NPC_ACT_NONE;
	i32 i = 0;
	float f = 0;
	u32 fn = 0;
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
	u32 npc = 0;           // Game's index
	float dist2 = 0;       // to the player, squared
	NpcPiece* rig = nullptr;
	NpcPiece* body = nullptr;
	NpcPiece* head = nullptr;
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
	// Call in PASS_WORLD. `dayLight` as for the world (1 1 1 indoors).
	void draw(const Mat4& viewProj, const float dayLight[3]);

	NpcConfig& config() { return _cfg; }
	const NpcStats& stats() const { return _stats; }
	Game& game() { return _game; }
	const Game& game() const { return _game; }
	const WayNet& wayNet() const { return _net; }
	const Mobs& mobs() const { return _mobs; }
	// The world's name in the scripts ("WORLD"), "" before a world is entered.
	const char* worldScript() const { return _script; }
	// The actor a character is right now, or null.
	const NpcActor* actorOf(u32 npcIndex) const;
	u32 actorCount() const { return ACTORS; }
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
	// The thing of that kind a character would use now: the nearest free
	// one it can see from where it stands (the scripts' Wld_IsMobAvailable,
	// AI_UseMob), or -1.
	int visibleMob(const World& world, u32 npc, const char* kind) {
		const World* before = _world;
		_world = &world;
		const int mob = findMob(npc, kind, -1);
		_world = before;
		return mob;
	}

private:
	enum { PIECES = 48, ACTORS = 16 }; // three models each
	typedef NpcPiece Piece;

	static bool findPoint(void* self, const char* name, float pos[3], float& yaw);
	const LookEntry* lookOf(const GameLook& look) const;
	Piece* piece(Pak& pak, u32 hash, u32& loadsLeft);
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
	i32 invoke(NpcActor& a, u32 fn);
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
	const Clip* clip(NpcActor& a, const char* name);
	const Clip* modeClip(NpcActor& a, const char* pattern, bool orOtherMode);
	NpcActor* actorFor(ScriptInstance* inst);
	int npcFor(ScriptInstance* inst) const { return _game.indexOf(inst); }
	i32 bodyStateOf(const NpcActor& a) const;
	const Clip* mobClip(NpcActor& a, int from, int to, char* nameOut);
	void releaseMob(NpcActor& a);
	bool useMob(NpcActor& a, const World& world, float dt);
	int findMob(u32 npc, const char* kind, int except) const;
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
	Piece _pieces[PIECES];
	NpcActor _actors[ACTORS];
	Pak* _pak = nullptr; // while update() or enterWorld() runs
	const World* _world = nullptr; // ... and the world
	u32 _clipBytes = 0;
	u32 _victim = SCRIPT_NONE; // the scripts' global of that name
	u32 _frame = 0;
	float _clock = 0;
	float _hours = -1.0f;
	float _reach = 3000.0f; // how far characters are shown right now, cm
};

} // namespace g3
