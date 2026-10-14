// The world's triggers and movers (g3ds/world.h, "world/<name>/triggers"), as ZenGin has them and OpenGothic's
// world/triggers carries them out:
//
//   * An event goes by name to every trigger of that name (Wld_SendTrigger, a lever's target, another trigger).
//   * zCTrigger sends its target on - when an event reaches it, or when the player's character walks into its box;
//     zCTriggerList sends several (all of them with their delays, the next one, a random one); oCTriggerScript
//     calls a script function; zCTriggerWorldStart sends its target when the world is entered (once, if so).
//   * A trigger can be switched off, may fire a number of times only, after a delay, not again within a while.
//   * zCMover: a gate, a lift, a drawbridge - its mesh moves between its keyframes (open: to the last, closed: back
//     to the first; a loop goes round and round), as fast as its speed says, with its sounds; when it arrives it
//     sends its own target on.
//
// What changes is kept in the game's save (Game::triggerState): a gate opened stays open.
// Not done: movers do not collide (the player walks through them, as before they moved), nobody but the player's
// character touches a trigger, messages to single keys (zCMessageFilter, zCCodeMaster), damage triggers.
#pragma once
#include "g3ds/math.h"
#include "g3ds/pak.h"
#include "g3ds/world.h"
#include "model.h"

namespace g3 {

class Game;
class World;

struct TriggerStats {
	u32 triggers = 0, movers = 0; // in this world
	u32 events = 0;               // events that reached a trigger since the world was entered
	u32 unknown = 0;              // events sent to a name no trigger has
	u32 scripts = 0;              // script functions called
	u32 moving = 0;               // movers on their way right now
	u32 drawn = 0;                // movers and doors drawn in the last frame
	u32 doors = 0, solid = 0;     // doors; movers and doors the player cannot pass through
	u32 carried = 0;              // frames the player's character rode on a mover
	char last[40] = "";           // the last event's name, for the debug line
};

class Triggers {
public:
	Triggers() = default;
	~Triggers() { release(); }
	Triggers(const Triggers&) = delete;
	Triggers& operator=(const Triggers&) = delete;

	// The triggers of "<worldAsset>/triggers" (none in an older archive); what the save kept is put back, and the
	// world's start triggers fire (`game` knows whether it is the first time).
	void enterWorld(Pak& pak, const char* worldAsset, Game& game);
	// The same from a blob (malloc'ed; taken over, also when refused): the tests.
	bool adopt(u8* blob, u32 size, Game& game);
	void leaveWorld() { release(); }
	void release();
	bool loaded() const { return _blob != nullptr; }

	// An event to every trigger named so (upper case; any case is taken).
	void send(const char* name, bool untrigger);
	// Once per frame: the events, the delays, the player's character walking into a trigger's box, the movers, the
	// doors (`others`: where the inhabitants about stand - a door opens for them as they come by, and shuts after).
	void update(float dt, Vec3 heroFeet, Game& game, const Vec3* others = nullptr, u32 otherCount = 0);

	// ---- colliding with movers and doors (World's queries ask these too) ----
	// The highest surface of a solid mover or door under `from`, no lower than from.y - maxDrop; `which` its index.
	bool rayDown(Vec3 from, float maxDrop, float& hitY, Vec3& normal, int* which = nullptr) const;
	// Moves the sphere out of them; true if it was moved.
	bool pushSphere(Vec3& center, float radius) const;
	// True if the segment passes through one of them.
	bool segmentHits(Vec3 a, Vec3 b) const;
	// True if one of them may reach into the box.
	bool boxTouches(const float mn[3], const float mx[3]) const;
	// Riding: where `feet` are in the space of the mover under them (`which`, -1: none) and back after it moved.
	int floorUnder(Vec3 feet, Vec3& local) const;
	Vec3 fromLocal(int which, Vec3 local) const;

	// ---- doors (the player's) ----
	// The door within `reach` in front of `feet` (looking along `yaw`), -1 for none.
	int doorNear(Vec3 feet, float yaw, float reach = 220.0f) const;
	bool doorOpen(u32 i) const { return i < _h.count && _state[i].doorOpen; }
	bool doorLocked(u32 i) const { return i < _h.count && _list[i].kind == TRG_DOOR && (_list[i].flags & TRGF_LOCKED) && !_state[i].unlocked; }
	const char* doorKey(u32 i) const { return i < _h.count ? text(_list[i].target) : ""; }
	const char* doorPick(u32 i) const { return i < _h.count ? text(_list[i].sfx[0]) : ""; }
	void doorUnlock(u32 i);
	// Opens it away from `from` (or shuts it).
	void doorToggle(u32 i, Vec3 from);
	// In PASS_WORLD: the movers within `range` of `eye`, lit as the floor under them.
	void draw(Pak& pak, const World& world, Vec3 eye, const float dayLight[3], float range = 6000.0f);
	// For the next draw() only: leave out what is outside `view` (null: nothing) or less than `minPixels` across.
	void setView(const Frustum* view, float minPixels, float focalPx) {
		_hasView = view != nullptr;
		if (view) _view = *view;
		_minPixels = minPixels;
		_focalPx = focalPx;
	}

	// A mover's sounds and a script trigger's function: the application's.
	struct Hooks {
		void (*sound)(void* user, const char* name, Vec3 at) = nullptr;
		void* user = nullptr;
	};
	void setHooks(const Hooks& h) { _hooks = h; }

	const TriggerStats& stats() const { return _stats; }
	u32 count() const { return _h.count; }
	const WorldTrigger& trigger(u32 i) const { return _list[i]; }
	const char* text(u32 offset) const { return offset < _h.nameBytes ? _names + offset : ""; }
	// A mover's transform now (its mesh's place: rotation and position), false for no mover.
	bool moverTransform(u32 i, Quat& rot, Vec3& pos) const;
	// The mover's state: 0 idle, 1 opening, 2 closing, 3 open for a while, 4 looping; its key now.
	u8 moverState(u32 i) const { return i < _h.count ? _state[i].mover : u8(0); }
	u32 moverFrame(u32 i) const { return i < _h.count ? _state[i].frame : 0u; }

private:
	enum EventType : u8 { EV_TRIGGER, EV_UNTRIGGER, EV_TOUCH, EV_START, EV_START_FIRST };
	enum MoverState : u8 { MV_IDLE, MV_OPEN, MV_CLOSE, MV_OPEN_TIMED, MV_LOOP };
	struct State {
		bool enabled;
		bool inside;     // the player's character is in its box (touch fires on the way in)
		u16 emitCount;
		double emitAt;   // the clock (Triggers::_clock) when it last fired (-1: never)
		float delay;     // a delayed event: seconds left (-1: none)
		u8 delayedType;
		u8 next;         // a list: the next target
		// A mover.
		u8 mover;        // MoverState
		u32 frame, target;
		float frameMs;   // milliseconds into the way from `frame`
		Quat rot;
		Vec3 pos;
		u16 model;       // into the models (NO_MODEL: none, NOT_TRIED)
		// A door: open or shut, which way, how far it is turned now (radians), unlocked; shut again after a passer-by.
		bool doorOpen, unlocked, byOthers;
		float doorSide, doorAngle, doorIdle;
		// Colliding: a sphere about its triangles (its own space).
		Vec3 collCenter;
		float collRadius;
		Vec3 worldCenter; // collCenter where it stands now (refreshCenter: whenever it moves; fix87)
		bool lit;
		float light[3], sky;
		// A door: where its hinge is when shut (worked out once).
		Vec3 hinge;
		bool hingeSet;
	};
	struct Pending {
		u32 hash;
		u8 type;
		float delay;     // seconds
	};
	static constexpr u16 NO_MODEL = 0xFFFF, NOT_TRIED = 0xFFFE;

	void process(u32 i, u8 type);
	void fire(u32 i, u8 type);
	void dispatch(u32 hash, u8 type, float delay = 0.0f);
	void onMover(u32 i, u8 type);
	void startMover(u32 i, u8 before);
	void tickMover(u32 i, float dtMs);
	void arrive(u32 i);
	void pose(u32 i);
	void poseAt(u32 i);
	void refreshCenter(u32 i) {
		if (_coll && _state[i].collRadius > 0.0f) _state[i].worldCenter = toWorld(i, _state[i].collCenter);
	}
	float keyTicks(u32 i, u32 k) const; // ms from key k to the next
	u32 nextKey(u32 i, u32 k) const;
	u32 prevKey(u32 i, u32 k) const;
	void keep(u32 i);
	void sound(u32 i, int which);
	void doorPose(u32 i);
	Vec3 toWorldClosed(u32 i, Vec3 p) const;
	// (A door is solid only when shut: one that swung somewhere odd does not wall anybody in.)
	bool solid(u32 i) const {
		return (_list[i].flags & TRGF_COLLIDES) && _list[i].collCount && _coll && (_list[i].kind != TRG_DOOR || _state[i].doorAngle == 0.0f);
	}
	// A world point in a mover's own space and back (its transform now).
	Vec3 toLocal(u32 i, Vec3 p) const;
	Vec3 toWorld(u32 i, Vec3 p) const;
	const float* _coll = nullptr; // 9 floats per triangle

	Hooks _hooks;
	Game* _game = nullptr;
	u16 _world = 0;
	u8* _blob = nullptr;
	TriggersHeader _h {};
	const WorldTrigger* _list = nullptr;
	const MoverKey* _keys = nullptr;
	const TriggerTarget* _targets = nullptr;
	const char* _names = nullptr;
	u32* _nameHash = nullptr; // per trigger
	State* _state = nullptr;
	// What update() looks at (fix87: not every trigger of the world every frame): the triggers the player's
	// character may walk into, the movers, the doors; those with a delayed event pending.
	u32* _touchList = nullptr;
	u32* _moverList = nullptr;
	u32* _doorList = nullptr;
	u32* _solidList = nullptr; // those with triangles to collide with (asked by every collision query of the world)
	u32 _touchCount = 0, _moverCount = 0, _doorCount = 0, _solidCount = 0;
	enum { DELAYED = 64 };
	u32 _delayed[DELAYED] = {};
	u32 _delayedCount = 0;
	bool _delayedOverflow = false; // more than DELAYED at once: all are looked at until none is left
	double _clock = 0.0;           // seconds of update() since the world was entered
	// The view the next draw() culls by (setView; fix87).
	Frustum _view {};
	bool _hasView = false;
	float _minPixels = 0.0f, _focalPx = 208.0f;
	enum { PENDING = 32, MODELS = 24 };
	Pending _pending[PENDING] = {};
	u32 _pendingCount = 0;
	struct Look {
		u32 hash;
		Model model;
	};
	Look _models[MODELS];
	u32 _modelCount = 0;
	TexturePool _pool;
	u32 _depth = 0;
	TriggerStats _stats;
};

} // namespace g3
