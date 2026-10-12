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
struct ByteSink;
struct ByteSource;
} // namespace g3

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
	bool knowsPlayer = false; // Npc_SetKnowsPlayer
	// Npc_SetTalentValue: for the fighting talents (1H, 2H) the chance, in per cent, of NOT striking
	// a critical blow (Gothic 1 reckoning, see Game::meleeDamage).
	i16 talent[16] = {};
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
	// What it thinks of the player (the scripts' ATT_ numbers), -1 for what
	// its guild thinks; a passing mood over that.
	i8 attitude = -1, tempAttitude = -1;
	int target = -1; // Npc_SetTarget: a character, -1 for none
	// Fighting (fight.h): the weapons it has equipped (item instances,
	// SCRIPT_NONE for none), what it holds in its hands (a WeaponMode), and
	// whether it is down.
	u32 melee = 0xFFFFFFFFu, ranged = 0xFFFFFFFFu;
	u32 armorItem = 0xFFFFFFFFu; // the armour it wears (its protection counts)
	u8 weapon = 0;
	u8 down = 0;      // DOWN_ (fight.h)
	int lastHit = -1; // who hit it last, -1 for nobody
	// Magic (magic.h): the spell it is casting (a SPL_ number, -1 for none) and how far it is charged,
	// what struck it last.
	i16 spell = -1, lastHitSpell = -1;
	u8 spellLevel = 0;
};

// A spell as the scripts' C_Spell instance and spellFX arrays describe it (made once per kind).
struct GameSpell {
	bool known = false;     // the scripts have the arrays at all
	char tag[24] = {};      // "Firebolt" (spellFXInstanceNames)
	char letters[4] = {};   // "FBT" (spellFXAniLetters): the casting animations' name
	float timePerMana = 500.0f; // ms
	i32 damagePerLevel = 1, damageType = 0, spellType = 1, collectAlgo = 0, collectType = 0, collectRange = 0;
	bool shoots = false;    // flies at its target (the original decides by the collect algorithm)
	// The particle effects of the spell's visual effect (hashName of the instance names; 0 for none).
	u32 fxInit = 0, fxCast = 0, fxFly = 0, fxCollide = 0;
	u32 fxArea = 0; // area spells (fire rain): the effect the cast key creates around the caster
	bool area = false; // hits everything near the caster (TARGET_COLLECT_ALL_FALLBACK_NONE with a range)
};

// What the engine reads off an item (the scripts' C_ITEM), made once per kind.
struct GameItemInfo {
	u32 symbol;
	i32 mainflag, flags;
	i32 damageType, damageTotal, range;
	i32 damage[8];
	i32 protection[8];
	GameText visual; // its mesh, lower case without extension ("itmw_1h_sword_01")
	GameText name;   // as the player reads it ("Rusty sword")
	// What the inventory shows and does with it.
	GameText description; // the longer name of the info box ("Essence of Healing"), 0 for none
	GameText scheme;      // how it is used (C_ITEM.SCEMENAME: "POTIONFAST", "FOOD"), 0 for none
	GameText text[6];     // the info box's lines: text, and its number (0: none)
	i32 count[6];
	u32 onUse;            // C_ITEM.ON_STATE[0]: what using it does; SCRIPT_NONE for nothing
	u32 onEquip, onUnequip; // rings and amulets: what wearing them does
	i32 condAtr[3], condValue[3]; // what it takes: attribute >= value (value 0: nothing)
	i32 value;            // ore
	i32 material;         // C_ITEM.MATERIAL (MAT_WOOD 0, MAT_METAL 2 ...): what it sounds like
	i32 spell;            // C_ITEM.SPELL (SPL_...) of a rune or scroll
	i32 circle;           // C_ITEM.MAG_CIRCLE of a rune
	i32 munition;         // C_ITEM.MUNITION of a bow: the arrows' instance (-1 for none)
};

// A stack of items a character carries (CreateInvItems).
struct GameItem {
	u32 npc;
	u32 symbol;
	i32 count;
};

// A thing a start-up script put into the world (Wld_InsertItem): which, at
// which named place. The baker places them; the game takes them from the archive.
struct GameDrop {
	u16 world;
	u32 symbol;
	GameText point;
};

// The diary (Log_CreateTopic, Log_SetTopicStatus, Log_AddEntry).
struct GameLogTopic {
	char* name;    // as the scripts give it
	u8 section;    // LOG_MISSION, LOG_NOTE
	u8 status;     // LOG_RUNNING ...; 0 until set
	u32 order;     // when last written to (newest first)
};
struct GameLogEntry {
	u32 topic;     // into the topics
	char* text;
};

// A thing a character can be talked to about (the scripts' C_INFO). The
// engine makes one for every such instance at the start; whom it belongs to
// is found by the character's instance symbol.
struct GameInfo {
	ScriptInstance* object = nullptr;
	u32 symbol = 0;                 // the instance
	u32 npc = SCRIPT_NONE;          // C_INFO.NPC: the instance of the character it belongs to
	i32 nr = 0;
	u32 condition = SCRIPT_NONE;    // functions; SCRIPT_NONE for none
	u32 information = SCRIPT_NONE;
	bool important = false;         // begins the talk by itself when it can
	bool permanent = false;         // may be told again
	bool trade = false;
	bool told = false;
};

// A way to go on in the middle of a talk (Info_AddChoice): the text shown
// and the function that says what follows.
struct GameChoice {
	u32 info;   // into the infos: the one it hangs on
	u32 fn;
	char* text;
};

// A line the scripts print on the screen (Print, PrintScreen).
struct GameMessage {
	char text[96];
	float seconds;
};

// What the diary just did, for a line of news (Game::takeLogNotice).
enum GameLogNoticeKind : u8 { LOGN_NEW_TOPIC = 1, LOGN_ENTRY = 2, LOGN_STATUS = 3 };
struct GameLogNotice {
	u8 kind;
	u8 section;
	u8 status; // for LOGN_STATUS
	char topic[64];
};

// The chapter title card the scripts put up (IntroduceChapter).
struct GameChapter {
	char title[48] = "";    // "Kapitel 1"
	char subtitle[64] = ""; // "Das Minental"
	char texture[40] = "";  // the card's picture ("chapter1.tga"), not baked
	char sound[40] = "";    // what is played with it ("chapter_01.wav")
	i32 delay = 0;          // how long the scripts want it shown, ms
	u32 serial = 0;         // counts the cards put up (the screen shows a new one)
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
	// An animal or a beast, not a person (the guilds past GIL_SEPERATOR_HUM).
	bool isMonster(const GameNpc& n) const { return _humanGuilds > 0 && guildOf(n) > _humanGuilds; }
	const GameNpc* hero() const { return _hero != SCRIPT_NONE ? &_npcs[_hero] : nullptr; }
	const GameRoutine& routine(u32 i) const { return _routines[i]; }
	const char* text(GameText id) const { return id < _textCount ? _texts[id] : ""; }
	// The character's name as the scripts give it ("Thorus").
	const char* nameOf(const GameNpc& n) const;

	// What one guild thinks of another, as the table the scripts fill in
	// (Wld_SetGuildAttitude, Wld_ExchangeGuildAttitudes) says: ATT_HOSTILE (0)
	// .. ATT_FRIENDLY (3); neutral for guilds out of range (a table the scripts left short must not make enemies).
	i32 guildAttitude(i32 guild1, i32 guild2) const;
	// What character `a` thinks of `b` (Npc_GetAttitude): towards the player
	// its own attitude if it has one, else its guild's.
	i32 attitude(u32 a, u32 b) const;
	// How many of an item (instance symbol) a character carries.
	i32 itemCount(u32 npc, u32 itemSymbol) const;
	// Gives (count > 0) or takes (count < 0) items; never below none.
	void giveItems(u32 npc, u32 itemSymbol, i32 count);
	// Every stack of items anybody carries (some with a count of 0: given
	// away), in the order they were first given.
	u32 itemStackCount() const { return _itemCount; }
	const GameItem& itemStack(u32 i) const { return _items[i]; }
	// What the start-up scripts put into the worlds (Wld_InsertItem), in order.
	u32 dropCount() const { return _dropCount; }
	const GameDrop& drop(u32 i) const { return _drops[i]; }
	// ---- talks (game_dialog.cpp) ----
	u32 infoCount() const { return _infoCount; }
	const GameInfo& info(u32 i) const { return _infos[i]; }
	// The info an instance symbol defines, or -1.
	int infoIndex(u32 symbol) const;
	// What the info is called in the menu (C_INFO.DESCRIPTION), UTF-8 is up to the caller (game code page).
	const char* infoDescription(u32 i) const;
	void setInfoTold(u32 i, bool told) {
		if (i < _infoCount) _infos[i].told = told;
	}
	// True if the scripts give the character something to talk about (an info of its own).
	bool hasInfos(u32 npc) const;
	// The infos of a character that can be told now, in the order the menu has
	// them (by NR, then by the functions' order). `important` selects the ones
	// that begin a talk by themselves; the others are the menu. `hero` is who
	// is talked to. Returns how many were put into `out` (at most `max`).
	u32 infosFor(u32 npc, bool important, u32* out, u32 max);
	// True if the info may be told to the hero by this character now: not
	// told yet (or permanent) and its condition holds.
	bool infoAvailable(u32 info, u32 npc);
	// Runs the info's function (what the character says and does) as the
	// original does: SELF the character, OTHER the player's.
	void runInfo(u32 info, u32 npc);
	// Calls a script function with SELF and OTHER (a character, -1 for none);
	// its integer result (0 if it has none).
	i32 callAs(u32 fn, u32 self, int other);
	// The ways on a talk has at the moment (Info_AddChoice), in the order they were added.
	u32 choiceCount() const { return _choiceCount; }
	const GameChoice& choice(u32 i) const { return _choices[i]; }
	u32 choicesOf(u32 info, u32* out, u32 max) const;
	void clearChoices(u32 info);
	void clearAllChoices();
	// How many choices were ever added, and the info the last one hangs on
	// (a talk notices a choice function that has put up new ways on).
	u32 choiceAdds() const { return _choiceAdds; }
	u32 lastChoiceInfo() const { return _lastChoiceInfo; }
	// What the scripts print on the screen, oldest first; false when there is none.
	bool takeMessage(GameMessage& out);
	void pushMessage(const char* text, float seconds);
	// Switches the character's daily routine to another (Npc_ExchangeRoutine):
	// the function "RTN_<name>_<id>"; false if the scripts lack it.
	bool exchangeRoutine(u32 npc, const char* name);
	// Bumped whenever an info is told or a choice changes (menus redraw on it).
	u32 talkVersion() const { return _talkVersion; }
	// The diary.
	u32 logTopicCount() const { return _logTopicCount; }
	const GameLogTopic& logTopic(u32 i) const { return _logTopics[i]; }
	u32 logEntryCount() const { return _logEntryCount; }
	const GameLogEntry& logEntry(u32 i) const { return _logEntries[i]; }
	u32 logVersion() const { return _logVersion; }
	// What the diary has done since it was last asked, oldest first; false when there is nothing.
	bool takeLogNotice(GameLogNotice& out);
	void clearLogNotices() { _noticeCount = 0; }
	// The chapter card put up last, and the story's chapter number (the
	// scripts' Kapitel; 0 if they have none).
	const GameChapter& chapter() const { return _chapter; }
	i32 chapterNumber() const { return _kapitel != SCRIPT_NONE ? _vm.getInt(_kapitel) : 0; }
	// ---- saving (game_save.cpp) ----
	// Everything the scripts and the engine changed since the start: script
	// variables, the characters (members, place, routine, what they carry,
	// attitude, whether they live), what has been told, the diary, the
	// chapter. Characters of a world not entered yet are kept and given their
	// state when they are made.
	void save(ByteSink& out) const;
	bool restore(ByteSource& in);
	// Counts every change of what anybody carries (the inventory redraws on it).
	u32 itemsVersion() const { return _itemsVersion; }
	// True if `symbol` is an item instance of the scripts.
	bool isItem(u32 symbol) const;
	// Runs a function of an item (ON_STATE, ON_EQUIP ...) as the original
	// does: SELF is the character, ITEM the item. False if `fn` is no function.
	bool callItemFunction(u32 npc, u32 itemSymbol, u32 fn);
	// ---- items as the scripts see them (game_items.cpp) ----
	// An object of the item `symbol` for the scripts to look at (what
	// Npc_GetEquippedArmor returns, what Wld_DetectItem puts into ITEM): one
	// per kind of item carried (`loot` -1), or per thing lying in the world
	// (`loot`: its index in the world's table). Its AMOUNT is `amount`. The
	// engine keeps a few and reuses them; one not asked for in a while is
	// given up (variables that still refer to it then refer to nothing, as
	// they would to a thing that is gone). Null if `symbol` is no item.
	ScriptInstance* itemObject(u32 symbol, i32 loot = -1, i32 amount = 1);
	// C_ITEM.MATERIAL of a kind of item (the scripts' MAT_ numbers), -1 if unknown.
	i32 itemMaterial(u32 symbol);
	// The thing in the world an object of itemObject() stands for: its index,
	// -1 for an item carried, -2 if the object is none of them.
	i32 itemObjectLoot(const ScriptInstance* inst) const;
	// The scripts' global ITEM.
	void setItemVar(ScriptInstance* inst) {
		if (_itemVar != SCRIPT_NONE) _vm.setInstance(_itemVar, inst);
	}
	// The best armour a character carries and may wear (the highest value, as
	// the original picks), SCRIPT_NONE for none.
	u32 bestArmor(u32 npc);
	// True if the character meets what the item asks of him (C_ITEM.COND_ATR).
	bool meetsConditions(u32 npc, const GameItemInfo& it) const;

	// How far a perception reaches (Perc_SetRange), cm; -1 if the scripts set none.
	float perceptionRange(i32 perc) const { return perc >= 0 && perc < PERC_TYPES ? _percRange[perc] : -1.0f; }
	enum { PERC_TYPES = 64 };
	// The hit points (attribute 0) of a character: dead at 0.
	i32 hitPoints(const GameNpc& n) const { return attributeOf(n, 0); }
	// The scripts' attributes (ATR_HITPOINTS, ATR_STRENGTH ...); 1 for hit
	// points and 0 for the rest if the scripts lack the member.
	i32 attributeOf(const GameNpc& n, i32 index) const;
	void setAttribute(GameNpc& n, i32 index, i32 value);
	// The talk flag (AIV_INVINCIBLE) off: fighters wait (AI_Wait in ZS_Attack) for as long as the player 'is talking'.
	void clearTalkFlag(u32 npc, bool clear = true);
	i32 talkFlag(u32 npc); // its value (for the debug line), -1: unknown
	u32 _mAivar = SCRIPT_NONE;
	i32 _aivInvincible = -1;
	// Members of a character by name ("FIGHT_TACTIC", "PROTECTION" index 2), 0 if none.
	i32 npcInt(const GameNpc& n, u32 member, u32 index = 0) const;
	// A constant of the scripts by name, or `otherwise`.
	i32 constant(const char* name, i32 otherwise) const;

	// ---- fighting (the rules in fight.cpp) ----
	// An item's numbers, or null if `symbol` is no item.
	const GameItemInfo* itemInfo(u32 symbol);
	// The best melee weapon a character carries (highest damage), SCRIPT_NONE for none.
	u32 bestMelee(u32 npc);
	// Every melee weapon the scripts define, the best one first (for the faults menu's "give").
	u32 bestMeleeOfAll();
	// Damage of a blow from `attacker` to `victim` as Gothic 1 reckons it:
	// per damage type of the weapon in hand (or of the bare hands), strength
	// plus damage minus protection; doubled (DAM_CRITICAL_MULTIPLIER) when
	// the attacker's talent loses the dice (OpenGothic's swordDamage). Never below 0.
	i32 meleeDamage(u32 attacker, u32 victim);
	// How far a blow of `attacker` reaches `victim`, cm (the guilds' fight
	// ranges and the weapon's).
	float meleeRange(u32 attacker, u32 victim);
	// The "G" range: within it a fighter closes in rather than runs.
	float approachRange(u32 attacker, u32 victim);
	// ---- magic (game_magic.cpp) ----
	// The spell with that SPL_ number, or null if the scripts have no such thing.
	const GameSpell* spellInfo(i32 id);
	// Spell_ProcessMana (or _Release) for `caster` with `invested` mana put in so far, and the target if any
	// (-1: none): SPL_DONTINVEST .. SPL_NEXTLEVEL. The spell is the caster's `spell`.
	i32 processMana(u32 caster, int target, i32 invested, bool release);
	// While set, the next blow dealt (Npcs::strike) is this spell's: `perLevel` damage times `level`, of
	// the types of `type`, less the victim's protection. `id` -1 ends it.
	void setSpellStrike(i32 id, i32 level) {
		_strikeSpell = id;
		_strikeLevel = level;
	}
	// The same for an arrow: the damage it carries per damage type (already with the critical hit); the victim's
	// protection is taken off (immune where it is below 0).
	void setArrowStrike(const i32 damage[8]) {
		for (int i = 0; i < 8; ++i) _strikeArrow[i] = damage[i];
		_strikeIsArrow = true;
	}
	void endArrowStrike() { _strikeIsArrow = false; }
	bool spellStriking() const { return _strikeSpell >= 0 || _strikeIsArrow; }
	// The guilds' table of numbers (C_GILVALUES), an entry of it; `otherwise` if none.
	i32 guildValue(const char* member, i32 guild, i32 otherwise);
	// Equips a weapon item (EquipItem, the faults menu): melee or ranged by its kind.
	void equip(u32 npc, u32 itemSymbol);
	// Puts an armour on (its protection counts; the old one's no more);
	// SCRIPT_NONE takes it off.
	void setArmor(u32 npc, u32 armorSymbol) {
		if (npc < _npcCount) wear(_npcs[npc], armorSymbol);
	}
	u32 heroIndex() const { return _hero; }
	// A random number below `bound` from the scripts' own dice.
	u32 dice(u32 bound);
	// The function of a state by name ("ZS_DEAD"), SCRIPT_NONE if none.
	u32 function(const char* name) const;

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
	// Where the C_PARTICLEFX instances (PARTICLEFX.DAT) and the spells' CFx_Base ones (VISUALFX.DAT) are: scripts of
	// their own in the game, else the main one.
	ScriptVm& particleVm() { return _haveParticleVm ? _particleVm : _vm; }
	ScriptVm& visualVm() { return _haveVisualVm ? _visualVm : _vm; }

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
	static void xNothing(ScriptVm& vm, void* user);
	static void xSetGuildAttitude(ScriptVm& vm, void* user);
	static void xGetGuildAttitude(ScriptVm& vm, void* user);
	static void xExchangeGuildAttitudes(ScriptVm& vm, void* user);
	static void xCreateInvItem(ScriptVm& vm, void* user);
	static void xCreateInvItems(ScriptVm& vm, void* user);
	static void xRemoveInvItem(ScriptVm& vm, void* user);
	static void xRemoveInvItems(ScriptVm& vm, void* user);
	static void xHasItems(ScriptVm& vm, void* user);
	static void xGetAttitude(ScriptVm& vm, void* user);
	static void xGetPermAttitude(ScriptVm& vm, void* user);
	static void xSetAttitude(ScriptVm& vm, void* user);
	static void xSetTempAttitude(ScriptVm& vm, void* user);
	static void xIsDead(ScriptVm& vm, void* user);
	static void xSetTarget(ScriptVm& vm, void* user);
	static void xGetTarget(ScriptVm& vm, void* user);
	static void xSetPercRange(ScriptVm& vm, void* user);
	// Fighting, as far as it needs no living actor (npc_fight.cpp has the rest).
	static void xHasEquippedMelee(ScriptVm& vm, void* user);
	static void xHasEquippedRanged(ScriptVm& vm, void* user);
	static void xHasEquippedWeapon(ScriptVm& vm, void* user);
	static void xIsInFightMode(ScriptVm& vm, void* user);
	static void xHasReadiedWeapon(ScriptVm& vm, void* user);
	static void xHasReadiedMelee(ScriptVm& vm, void* user);
	static void xHasReadiedRanged(ScriptVm& vm, void* user);
	static void xChangeAttribute(ScriptVm& vm, void* user);
	static void xSetToFistMode(ScriptVm& vm, void* user);
	static void xSetToFightMode(ScriptVm& vm, void* user);
	static void xReturnMinusOne(ScriptVm& vm, void* user);
	static void xGetTrueGuild(ScriptVm& vm, void* user);
	void addItems(ScriptInstance* npc, i32 symbol, i32 count);
	void wear(GameNpc& n, u32 armorSymbol);
	// Items (game_items.cpp).
	void bindItems();
	static void xIsItem(ScriptVm& vm, void* user);
	static void xGetEquippedMelee(ScriptVm& vm, void* user);
	static void xGetEquippedRanged(ScriptVm& vm, void* user);
	static void xGetEquippedArmor(ScriptVm& vm, void* user);
	static void xHasEquippedArmor(ScriptVm& vm, void* user);
	static void xGetReadiedWeapon(ScriptVm& vm, void* user);
	static void xGetInvItem(ScriptVm& vm, void* user);
	static void xGetInvItemBySlot(ScriptVm& vm, void* user);
	static void xClearInventory(ScriptVm& vm, void* user);
	static void xEquipArmor(ScriptVm& vm, void* user);
	static void xEquipBestArmor(ScriptVm& vm, void* user);
	static void xUnequipArmor(ScriptVm& vm, void* user);
	static void xUnequipWeapons(ScriptVm& vm, void* user);
	struct ItemObject {
		ScriptInstance* inst;
		u32 symbol;
		i32 loot;
		u32 used; // when last asked for
	};
	enum { ITEM_OBJECTS = 24 };
	ItemObject _itemObjects[ITEM_OBJECTS] = {};
	u32 _itemObjectClock = 0;
	u32 _iAmount = SCRIPT_NONE, _iMainFlag = SCRIPT_NONE;

	GameItemInfo* _itemInfos = nullptr;
	u32 _itemInfoCount = 0, _itemInfoCap = 0;
	ScriptInstance* _gilValues = nullptr;
	bool _gilTried = false;
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
	i32* _guildAtt = nullptr; // _guildCount x _guildCount
	u32 _guildCount = 0;
	GameItem* _items = nullptr;
	u32 _itemCount = 0, _itemCap = 0, _itemsVersion = 0;
	GameLogTopic* _logTopics = nullptr;
	u32 _logTopicCount = 0, _logTopicCap = 0;
	GameLogEntry* _logEntries = nullptr;
	u32 _logEntryCount = 0, _logEntryCap = 0, _logVersion = 0;
	// ---- talks ----
	void bindDialogues();
	static void xKnowsInfo(ScriptVm& vm, void* user);
	static void xCheckInfo(ScriptVm& vm, void* user);
	static void xAddChoice(ScriptVm& vm, void* user);
	static void xClearChoices(ScriptVm& vm, void* user);
	static void xExchangeRoutine(ScriptVm& vm, void* user);
	static void xPrint(ScriptVm& vm, void* user);
	static void xPrintScreen(ScriptVm& vm, void* user);
	static void xKnowsPlayer(ScriptVm& vm, void* user);
	static void xSetKnowsPlayer(ScriptVm& vm, void* user);
	static void xIsInCutscene(ScriptVm& vm, void* user);
	static void xSetTalentValue(ScriptVm& vm, void* user);
	static void xGetTalentValue(ScriptVm& vm, void* user);
	GameInfo* _infos = nullptr;
	u32 _infoCount = 0;
	GameChoice* _choices = nullptr;
	u32 _choiceCount = 0, _choiceCap = 0, _choiceAdds = 0, _lastChoiceInfo = 0;
	GameMessage _messages[6] = {};
	u32 _messageFirst = 0, _messageCount = 0;
	u32 _talkVersion = 0;
	u32 _mInfoNpc = SCRIPT_NONE, _mInfoNr = SCRIPT_NONE, _mInfoCondition = SCRIPT_NONE, _mInfoInformation = SCRIPT_NONE,
	    _mInfoDescription = SCRIPT_NONE, _mInfoImportant = SCRIPT_NONE, _mInfoPermanent = SCRIPT_NONE, _mInfoTrade = SCRIPT_NONE,
	    _mId = SCRIPT_NONE;
	PointFinder _find = nullptr;
	void* _findUser = nullptr;
	int logTopicOf(const char* name, bool make);
	static void xLogCreateTopic(ScriptVm& vm, void* user);
	static void xLogSetTopicStatus(ScriptVm& vm, void* user);
	static void xLogAddEntry(ScriptVm& vm, void* user);
	GameLogNotice _notices[8] = {};
	u32 _noticeFirst = 0, _noticeCount = 0;
	void notice(u8 kind, u8 section, u8 status, const char* topic);
	GameChapter _chapter;
	u32 _kapitel = SCRIPT_NONE;
	static void xIntroduceChapter(ScriptVm& vm, void* user);
	// Saved characters whose world has not been entered yet (game_save.cpp).
	struct Pending {
		u32 symbol, ordinal;
		u8* blob;
		u32 size;
	};
	Pending* _pending = nullptr;
	u32 _pendingCount = 0, _pendingCap = 0;
	void restoreNpc(u32 index, ByteSource& in);
	void applyPending(u32 index);
	void freePending();
	GameDrop* _drops = nullptr;
	u32 _dropCount = 0, _dropCap = 0;
	float _percRange[PERC_TYPES];
	World* _worlds = nullptr;
	u32 _worldCount = 0, _worldCap = 0;
	u16 _world = 0;
	u32 _hero = SCRIPT_NONE;
	u32 _seed = 1;
	float _hours = 8.0f;
	i32 _humanGuilds = 0; // GIL_SEPERATOR_HUM (0: not known)
	GameStats _stats;
	GameSpell _spells[64];
	bool _spellsRead[64] = {};
	ScriptVm _particleVm, _visualVm;
	bool _haveParticleVm = false, _haveVisualVm = false;
	u32 _spellNames = SCRIPT_NONE, _spellLetters = SCRIPT_NONE, _spellProcess = SCRIPT_NONE, _spellRelease = SCRIPT_NONE;
	i32 _strikeSpell = -1, _strikeLevel = 0;
	i32 _strikeArrow[8] = {};
	bool _strikeIsArrow = false;
	static void xActiveSpell(ScriptVm& vm, void* user);
	static void xActiveSpellLevel(ScriptVm& vm, void* user);
	static void xActiveSpellCat(ScriptVm& vm, void* user);
	static void xLastHitSpellId(ScriptVm& vm, void* user);
	static void xLastHitSpellCat(ScriptVm& vm, void* user);
	// Symbols of the scripts this code reads.
	u32 _clsNpc = SCRIPT_NONE, _clsItem = SCRIPT_NONE;
	u32 _mRoutine = SCRIPT_NONE, _mName = SCRIPT_NONE, _mVisualChange = SCRIPT_NONE, _mVisualSkin = SCRIPT_NONE,
	    _mWear = SCRIPT_NONE, _mMainFlag = SCRIPT_NONE, _mWp = SCRIPT_NONE, _mStartState = SCRIPT_NONE,
	    _mSensesRange = SCRIPT_NONE, _mGuild = SCRIPT_NONE, _mAttribute = SCRIPT_NONE;
	// Fighting.
	u32 _mProtection = SCRIPT_NONE, _mDamage = SCRIPT_NONE, _mDamageType = SCRIPT_NONE;
	u32 _iFlags = SCRIPT_NONE, _iDamageType = SCRIPT_NONE, _iDamageTotal = SCRIPT_NONE, _iDamage = SCRIPT_NONE, _iRange = SCRIPT_NONE,
	    _iVisual = SCRIPT_NONE, _iProtection = SCRIPT_NONE, _iName = SCRIPT_NONE;
	// The inventory.
	u32 _iDescription = SCRIPT_NONE, _iScheme = SCRIPT_NONE, _iText = SCRIPT_NONE, _iCount = SCRIPT_NONE, _iOnState = SCRIPT_NONE,
	    _iOnEquip = SCRIPT_NONE, _iOnUnequip = SCRIPT_NONE, _iCondAtr = SCRIPT_NONE, _iCondValue = SCRIPT_NONE, _iValue = SCRIPT_NONE,
	    _itemVar = SCRIPT_NONE;
};

} // namespace g3
