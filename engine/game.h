// The game as its scripts set it up: who is in the world, what they look
// like, where they are at what time of day.
//
// Game runs the scripts' start-up for a world (STARTUP_<world>, then
// INIT_<world>, as the original does) on the script machine and plays the
// engine's part in it: Wld_InsertNpc makes a character, the Mdl_ functions
// its code calls give it a body, TA_Min fills in its daily routine. What
// comes out is a list of characters with a look and a place. Nothing here
// draws or loads models; the baker runs the same code to find out which
// models a world needs.
#pragma once
#include "g3ds/game.h"
#include "g3ds/pak.h"
#include "script.h"

namespace g3 {

// A text the game keeps (names of models, of places): Game::text(id). 0 is "".
typedef u16 GameText;

struct GameLook {
	GameText script = 0; // model script without extension, lower case: "humans"
	GameText body = 0;   // body mesh: "hum_body_naked0"
	GameText head = 0;   // head mesh, 0 for a creature
	GameText armor = 0;  // mesh of the armour worn instead of the bare body, 0 for none
	i16 bodyTex = 0, skin = 0, headTex = 0, teethTex = 0; // texture variants
	// Not part of what makes two looks the same model:
	float scale[3] = {1, 1, 1};
	float fatness = 0;
	// The same meshes and textures (size and fatness aside).
	bool operator==(const GameLook& o) const;
};

struct GameRoutine {
	u16 start, stop; // minutes of the day; stop <= start runs over midnight
	u32 state;       // the script function of what is done (ZS_Guard, ...)
	GameText point;
};

struct GameNpc {
	ScriptInstance* object = nullptr;
	u32 symbol = 0;        // the instance it was made from
	GameLook look;
	GameText spawn = 0;    // where the script put it
	u32 routineFirst = 0;  // into Game::routine()
	u16 routineCount = 0;
	u16 world = 0;         // the world it lives in (Game::worldIndex)
	bool hero = false;
	// Where it is now.
	bool placed = false;   // false: the world has no such place
	GameText want = 0;     // where its routine wants it
	GameText at = 0;       // where it is (its spawn place if the world lacks the other)
	float pos[3] = {0, 0, 0};
	float yaw = 0;
	u32 state = 0;         // what it is doing by its routine (a script function), 0 for none
	u32 moved = 0;         // counts its changes of place
	// Somebody else moves it about (it is near the player and lives its day,
	// see npcs.h): a new time of day changes what its routine wants, not
	// where it is.
	bool active = false;
};

struct GameStats {
	u32 npcs = 0;        // characters the scripts have put into the worlds
	u32 withRoutine = 0;
	u32 items = 0;       // Wld_InsertItem calls
	u32 unplaced = 0;    // characters whose place the current world does not have
	u32 notNpc = 0;      // Wld_InsertNpc with something that is no character
	u32 worlds = 0;      // start-ups run
	u32 moves = 0;       // changes of place since the start (routines moving on)
};

class Game {
public:
	// Finds a named place of the world being entered.
	typedef bool (*PointFinder)(void* user, const char* name, float pos[3], float& yaw);

	Game() = default;
	~Game() { release(); }
	Game(const Game&) = delete;
	Game& operator=(const Game&) = delete;

	// Loads "script/gothic" and makes the player's character (PC_HERO).
	// False if the archive has no scripts or they cannot be used.
	bool load(Pak& pak);
	// The same with a machine image handed over (the baker, the tests).
	bool adopt(u8* image, u32 size);
	void release();
	bool loaded() const { return _vm.loaded(); }

	// Entering the world the scripts call `script` ("WORLD"): its start-up
	// runs the first time, its INIT every time, and everybody is put where
	// the time of day (hours, 0..24) wants him.
	void enterWorld(const char* script, float hours, PointFinder find, void* user);
	// A new time of day: characters whose routine has moved on change place.
	// Returns how many did.
	u32 setTime(float hours, PointFinder find, void* user);
	u16 currentWorld() const { return _world; }

	u32 npcCount() const { return _npcCount; }
	const GameNpc& npc(u32 i) const { return _npcs[i]; }
	GameNpc& npcMut(u32 i) { return _npcs[i]; }
	// The character a script object belongs to, or -1.
	int indexOf(const ScriptInstance* inst) const;
	// What the character does when left to itself: the state of its routine
	// at this hour, or - without a routine - the state the scripts gave it to
	// start with (creatures). A script function, 0 for none.
	u32 behaviour(const GameNpc& n) const;
	// Puts a character where its routine wants it right now and takes it
	// out of anybody's hands (`active` off).
	void settle(u32 index, PointFinder find, void* user);
	// Where the player is, for the scripts' questions about the hero.
	void setHeroPlace(const float pos[3], float yaw);
	float hours() const { return _hours; }
	// The way point the scripts read as the character's ("self.wp").
	void setWayPoint(GameNpc& n, const char* name) {
		if (_mWp != SCRIPT_NONE && n.object && name) _vm.setString(_mWp, 0, n.object, name);
	}
	// Members of the characters' class the engine reads.
	i32 guildOf(const GameNpc& n) const { return _mGuild != SCRIPT_NONE && n.object ? _vm.getInt(_mGuild, 0, n.object) : 0; }
	float sensesRange(const GameNpc& n) const;
	const GameNpc* hero() const { return _hero != SCRIPT_NONE ? &_npcs[_hero] : nullptr; }
	const GameRoutine& routine(u32 i) const { return _routines[i]; }
	const char* text(GameText id) const { return id < _textCount ? _texts[id] : ""; }
	// The character's name as the scripts give it ("Thorus").
	const char* nameOf(const GameNpc& n) const;

	// A number that stands for a look's meshes and textures: the same in the
	// baker and on the console, which is how baked models are found.
	u32 lookKey(const GameLook& look) const;
	// The engine functions that only mean something to characters living
	// their day (npcs.h binds them): their names, upper case. Here so that
	// the baker, which runs no lives, can tell what is done from what is not.
	static const char* const* lifeFunctions(u32& count);
	// How many different looks the characters have.
	u32 countLooks() const;
	// No "script: ..." lines in the log (the tests break things on purpose).
	void setQuiet(bool quiet) { _vm.setQuiet(quiet); }

	const GameStats& stats() const { return _stats; }
	ScriptVm& vm() { return _vm; }
	const ScriptVm& vm() const { return _vm; }

private:
	struct Armor {
		u32 symbol;
		GameText mesh;
		i16 skin;
		bool wearable;
	};
	struct World {
		u32 nameHash;
		bool started;
	};

	bool bindAll();
	GameText intern(const char* text, bool model);
	GameNpc* npcOf(ScriptInstance* inst);
	u32 addNpc(u32 symbol, bool hero);
	const Armor* armorOf(u32 itemSymbol);
	void runIfPresent(const char* prefix, const char* script);
	void place(GameNpc& n, float hours, PointFinder find, void* user);
	void placeNow(GameNpc& n, PointFinder find, void* user);
	const GameRoutine* routineAt(const GameNpc& n, float hours) const;

	// The engine functions the scripts call.
	static void xInsertNpc(ScriptVm& vm, void* user);
	static void xInsertItem(ScriptVm& vm, void* user);
	static void xSetVisual(ScriptVm& vm, void* user);
	static void xSetVisualBody(ScriptVm& vm, void* user);
	static void xSetModelScale(ScriptVm& vm, void* user);
	static void xSetModelFatness(ScriptVm& vm, void* user);
	static void xApplyOverlay(ScriptVm& vm, void* user);
	static void xTaMin(ScriptVm& vm, void* user);
	static void xTa(ScriptVm& vm, void* user);
	static void xEquipItem(ScriptVm& vm, void* user);
	static void xGetNpc(ScriptVm& vm, void* user);
	static void xGetInstanceId(ScriptVm& vm, void* user);
	static void xIsValid(ScriptVm& vm, void* user);
	static void xRandom(ScriptVm& vm, void* user);
	static void xStrCmp(ScriptVm& vm, void* user);
	static void xConcat(ScriptVm& vm, void* user);
	static void xIntToString(ScriptVm& vm, void* user);
	static void xIntToFloat(ScriptVm& vm, void* user);
	static void xFloatToInt(ScriptVm& vm, void* user);
	static void xIsPlayer(ScriptVm& vm, void* user);
	static void xSetTime(ScriptVm& vm, void* user);
	static void xIsTime(ScriptVm& vm, void* user);

	ScriptVm _vm;
	GameNpc* _npcs = nullptr;
	u32 _npcCount = 0, _npcCap = 0;
	GameRoutine* _routines = nullptr;
	u32 _routineCount = 0, _routineCap = 0;
	char** _texts = nullptr;
	u32* _textHash = nullptr;
	u32 _textCount = 0, _textCap = 0;
	Armor* _armors = nullptr;
	u32 _armorCount = 0, _armorCap = 0;
	World* _worlds = nullptr;
	u32 _worldCount = 0, _worldCap = 0;
	u16 _world = 0;
	u32 _hero = SCRIPT_NONE;
	u32 _seed = 1;
	float _hours = 8.0f;
	GameStats _stats;
	// Symbols of the scripts this code reads.
	u32 _clsNpc = SCRIPT_NONE, _clsItem = SCRIPT_NONE;
	u32 _mRoutine = SCRIPT_NONE, _mName = SCRIPT_NONE, _mVisualChange = SCRIPT_NONE, _mVisualSkin = SCRIPT_NONE,
	    _mWear = SCRIPT_NONE, _mMainFlag = SCRIPT_NONE, _mWp = SCRIPT_NONE, _mStartState = SCRIPT_NONE,
	    _mSensesRange = SCRIPT_NONE, _mGuild = SCRIPT_NONE;
};

} // namespace g3
