// Platform-independent application layer: owns the pak, the world, the
// player and the sky. Platforms feed it input and call update()/render().
#pragma once
#include "g3ds/pak.h"
#include "chests.h"
#include "hero.h"
#include "inventory.h"
#include "loot.h"
#include "triggers.h"
#include "particles.h"
#include "shadows.h"
#include "puddles.h"
#include "rain.h"
#include "godrays.h"
#include "menu.h"
#include "npcs.h"
#include "panel.h"
#include "player.h"
#include "readahead.h"
#include "recorder.h"
#include "settings.h"
#include "sky.h"
#include "weather.h"
#include "sound.h"
#include "music.h"
#include "speech.h"
#include "ui.h"
#include "world.h"

namespace g3 {

struct Camera {
	Vec3 pos {0, 0, 0};
	float yaw = 0, pitch = 0;
	float fovY = 1.0472f; // 60 degrees
	float zNear = 20.0f;  // cm
	float zFar = 14000.0f;
	// Third person. The view is set up as the game's camera scripts say (the
	// archive carries them, see CameraSet): a range behind the character at
	// an elevation, looking at its hips and then tilted up, so that the
	// character stands in the lower half of the picture. It follows with a
	// lag: the point it looks after trails the character (which is why the
	// view widens when the character runs), and the camera trails the place
	// it should be at (which is why it swings round after a turn).
	float boomRadius = 24.0f;  // how much room the camera keeps from walls (the near plane reaches 30 cm sideways)
	float targetPace = 3.0f;   // 1/s: how eagerly the looked-after point follows the character
	float topDownBelow = 80.0f; // cm: with less room behind than this the camera looks down from above instead
	float topDownRange = 150.0f, topDownElevation = 80.0f;
};

struct Environment {
	float fogStart = 2500.0f; // cm
	float fogEnd = 12000.0f;  // cm; keep it at or below the draw distance
	float timeOfDay = 13.0f;  // hours, 0..24
	float dayLength = 0.0f;   // real seconds per game day; 0 freezes the clock
};

// Where the last update() spent its time, milliseconds.
struct UpdateTimes {
	float stream = 0; // chunks coming and going
	float npcs = 0;   // the inhabitants: who is near, their states and actions ...
	float script = 0; // ... of which inside the scripts' functions
	float player = 0; // the character: steering, walls, ground
	float camera = 0; // finding the camera a place
	float loot = 0;   // the things lying about (inside `npcs`): their light, their models
	// Finer (fix86), for the report: of `npcs` - the inhabitants' own update, the triggers, the particles;
	// of `player` - steering (ledges, overlays, Hero::control), the move (Player::update), the bodies in the way,
	// the character's pose (Hero::follow), the rest (spells, arrows, the recorder, portals).
	float npcUpdate = 0, triggers = 0, particles = 0;
	float heroControl = 0, heroMove = 0, heroBodies = 0, heroFollow = 0, heroRest = 0;
};

// Where render() spent the CPU's time (issuing draw calls), ms (fix86): the world's opaque pass, the
// inhabitants, the things lying about with the movers, the player's character, the rest (sky, shadows,
// water, particles, the overlay).
struct RenderTimes {
	float world = 0, npcs = 0, things = 0, hero = 0, rest = 0;
};

class App {
public:
	// `worldName` may be null: the archive says which world the game starts in.
	bool init(const char* pakPath, const char* worldName);
	void shutdown();

	void update(const Input& in, float dt);
	void render();
	// For finding a fault (fix135, sdmc:/3ds/gothic3ds/debug.txt): parts of the picture left out (RenderSkip bits).
	enum RenderSkip : u32 {
		SKIP_SKY = 1u << 0, SKIP_WORLD = 1u << 1, SKIP_NPCS = 1u << 2, SKIP_THINGS = 1u << 3, SKIP_HERO_SHADOW = 1u << 4,
		SKIP_HERO = 1u << 5, SKIP_GROUND = 1u << 6, SKIP_BLENDED = 1u << 7, SKIP_GODRAYS = 1u << 8, SKIP_RAIN = 1u << 9,
		SKIP_PARTICLES = 1u << 10, SKIP_OVERLAY = 1u << 11,
	};
	void setRenderSkip(u32 skip) { _renderSkip = skip; }
	u32 renderSkip() const { return _renderSkip; }

	// ---- sound (sound.h) ----
	// The platform's output, given before init(); null: no sound. Must outlive the application.
	void setAudio(AudioOut* out) { _audio = out; }
	Music& music() { return _music; }
	const Music& music() const { return _music; }
	Speech& speech() { return _speech; }
	const Speech& speech() const { return _speech; }
	Speech& chatter() { return _chatter; }
	const Speech& chatter() const { return _chatter; }
	Sound& sound() { return _sound; }
	const Sound& sound() const { return _sound; }
	// While update() is not called (the pause menu): lets sounds that ran out go and keeps the volume current.
	void soundIdle(float dt);

	// Sets draw distance, streaming radii, fog and far plane consistently.
	void setDrawDistance(float cm);
	// The pacer's share of the draw distance (the world and its fog; ignored with the far layer).
	void setDistanceScale(float scale);
	// Moves the player's eyes to `eye` (keeps walking or flying as it was).
	void teleport(Vec3 eye);
	// Switches to another world of the archive. `eye` null uses its start
	// point. On failure the current world stays.
	bool enterWorld(u32 worldEntryHash, const Vec3* eye, float yaw);

	// Where the player is (world, position, view, time of day, draw distance),
	// so the next start continues from there. loadState returns false and
	// changes nothing if the file is missing, damaged or names a world that is
	// not in this archive.
	bool saveState(const char* path) const;
	bool loadState(const char* path);
	bool loadGameSection(FILE* f);
	float drawDistance() const { return _world.config().drawDistance; }

	Camera& camera() { return _cam; }
	Environment& environment() { return _env; }
	Weather& weather() { return _weather; }
	const Puddles& puddles() const { return _puddles; }
	const Rain& rain() const { return _rain; }
	const GodRays& godRays() const { return _godRays; }
	Player& player() { return _player; }
	const Hero& hero() const { return _hero; }
	// The characters the game's scripts put into the world; not loaded with
	// an archive that has no scripts.
	FrameBudget& frameBudget() { return _budget; } // what the last frame left for loads (set by the platform)
	Npcs& npcs() { return _npcs; }
	const Npcs& npcs() const { return _npcs; }
	// True if the player steers a character (see Input); false with an
	// archive that has none, and while flying.
	bool steersHero() const { return _hero.loaded() && !_player.flying; }
	// Where the player (the character, or the free camera) faces.
	float heading() const { return _hero.loaded() ? _hero.yaw() : _cam.yaw; }
	const CameraSet& cameraSet() const { return _cams; }
	// Third person needs a character; without one the view is first-person.
	bool thirdPerson() const { return _thirdPerson && _hero.loaded(); }
	void setThirdPerson(bool on) { _thirdPerson = on; }
	// True if the character is drawn this frame (not while flying, and not
	// when a wall has pushed the camera into its head).
	bool heroVisible() const { return _heroVisible; }
	// Graphics+ under load (the platform's frame pacer): the plants stand
	// still and only the player's character keeps a shadow, until there is
	// room again. The occlusion costs nothing in the frame and stays.
	void setGfxRelief(bool heavy);
	void setWindRelief(bool halved);
	bool windRelief() const { return _windRelief; }
	bool gfxRelief() const { return _gfxRelief; }
	const Shadows& shadows() const { return _shadows; }
	// How dark the baked sun's shadow is drawn now (0: not at all): the time of day, the weather, the menu.
	float sunShadowDarkness() const;
	World& world() { return _world; }
	const World& world() const { return _world; }
	Pak& pak() { return _pak; }
	// Reading ahead (readahead.h): bytes of cache, 0 for none. Before init(). The menu's switch turns it on and off.
	void setReadAheadBudget(u32 bytes) { _aheadBudget = bytes; }
	// Stereo 3D: the console's 3D slider, 0..1 (0 or a backend without a second eye: drawn once, as usual).
	void setStereo(float strength) { _stereo = strength; }
	ReadAhead& readAhead() { return _ahead; }
	SkyColors skyColors() const {
		SkyColors c = skyColorsAt(_env.timeOfDay);
		if (!_world.indoor()) _weather.apply(c);
		return c;
	}
	const char* error() const { return _error; }
	// Name of the current world asset ("world/oldmine"), for display.
	const char* worldName() const;
	// How many times the world was switched (portals and the debug key).
	u32 worldSwitches() const { return _switches; }
	// Seconds since init; drives texture animation. Settable for reproducible pictures.
	double clock() const { return _clock; }
	float fps() const { return _fps; }
	void setClock(double seconds) { _clock = seconds; }
	// True while the camera is below a water surface.
	bool underwater() const { return _underwater; }
	const UpdateTimes& times() const { return _times; }
	const RenderTimes& renderTimes() const { return _renderTimes; }
	// The last five seconds of the character controller, frame by frame.
	const MoveRecorder& moves() const { return _moves; }

	// What the player has set (menu.h). Change them, then applySettings().
	Settings& settings() { return _settings; }
	const Settings& settings() const { return _settings; }
	// Draw distance, objects, the inhabitants' switches, the view: what of
	// the settings concerns the application. (Frame pacing and the bottom
	// screen are the platform's.)
	void applySettings();
	// The pause menu: while it is open the game stands still (update() is
	// not called) and render() draws it over the picture.
	Menu& menu() { return _menu; }
	Ui& ui() { return _ui; }
	// A line of text in the middle of the picture for a few seconds.
	void toast(const char* text, float seconds = 2.5f);
	// The chapter card: seconds it is still up, and the last one seen.
	float chapterTime() const { return _chapterTime; }
	u32 chapterSeen() const { return _chapterSeen; }
	// Takes the chapter card down, and the ones put up so far with it (for tests that look at the world alone).
	void skipChapters() {
		if (_npcs.loaded()) _chapterSeen = _npcs.game().chapter().serial;
		_chapterTime = 0.0f;
	}
	const char* toastText() const { return _toastTime > 0.0f ? _toast : ""; }
	// Where the position is saved (the menu and getting up after death load from there).
	void setStatePath(const char* path) { _statePath = path; }
	// Runs what the menu asked for (save, load, faults); returns false for
	// MENU_EXIT, which is the platform's to carry out.
	bool menuAction(MenuAction action, const char* statePath);

	// ---- controls and fighting ----
	// How the sticks steer (Settings::scheme): classic until applySettings().
	ControlScheme scheme() const { return _scheme; }
	void setScheme(ControlScheme s) { _scheme = s; }
	// The character the player has locked on to (or fights with), -1 for none.
	int target() const { return _target; }
	bool targetLocked() const { return _locked; }
	// The player's character in the scripts (its hit points and the like).
	const GameNpc* heroNpc() const;
	i32 heroHitPoints() const;
	i32 heroHitPointsMax() const;
	// The player's character was killed: the overlay says so, A loads the position.
	bool heroDead() const { return _hero.down() && _deadTime > 0.0f; }
	// What fighting has come to so far, for the reports.
	struct FightStats {
		u32 blowsLanded = 0, blowsTaken = 0, parried = 0, dodged = 0;
		i32 lastDamage = -1;
	};
	const FightStats& fightStats() const { return _fight; }

	// ---- the inventory (inventory.cpp; the rules are in inventory.h) ----
	// What the bottom screen shows, brought up to date.
	const InventoryView& inventory();
	// The bottom screen's panel; the platform feeds it and shows its canvas.
	InventoryPanel& panel() { return _panel; }
	// Reads the panel's input and carries out what it asks for (returned: the
	// report and the buttons are the platform's to deal with).
	PanelCommand panelInput(const PanelInput& in, float dt);
	void panelCommand(const PanelCommand& c);
	// What an item is for: drink, eat, put on, take off, take in hand. False
	// if refused (the view's message and a toast say why). The use itself
	// takes its time: see itemBusy().
	bool itemAction(u32 symbol);
	// Quick slots 0..3 (R + D-Pad up, right, down, left).
	bool quickUse(int slot);
	void setQuickSlot(int slot, u32 symbol);
	u32 quickSlot(int slot) const { return slot >= 0 && slot < QUICK_SLOTS ? _quick[slot] : SCRIPT_NONE; }
	// Something is being used or waits for the character's hands.
	bool itemBusy() const { return _job.stage != 0; }
	// In a fight: a weapon in hand, a blow less than 5 s ago, or an enemy
	// close by that is after the character.
	bool heroInCombat() const;
	MusicSituation musicSituation(float dt);
	// Rings and the amulet worn (SCRIPT_NONE for none).
	u32 ring(int i) const { return i >= 0 && i < 2 ? _rings[i] : SCRIPT_NONE; }
	u32 amulet() const { return _amulet; }
	// The faults menu's: a set of things of every kind the scripts define, to
	// try the inventory with. Returns how many kinds were given.
	u32 giveItemKit();
	u32 giveTestKit();
	// ---- magic (app_magic.cpp; the rules are the scripts', see game_magic.cpp) ----
	// The rune or scroll chosen (an item symbol, SCRIPT_NONE for none): A casts it in the magic stance.
	u32 spellItem() const { return _spellItem; }
	// Chooses a rune or scroll and takes the magic stance. False (with a toast) if it cannot now.
	bool selectSpell(u32 item);
	// A spell is being charged or released.
	bool spellCasting() const { return _cast.stage != CAST_NONE; }
	// How a spell is going: the charge so far (mana put in) and its level.
	i32 spellInvested() const { return _cast.invested; }
	u8 spellLevel() const { return _cast.level; }
	u32 boltCount() const;
	struct MagicStats {
		u32 casts = 0;     // spells let go
		u32 teleports = 0; // the scripts sent the player somewhere (AI_Teleport)
		u32 bolts = 0;     // projectiles shot
		u32 hits = 0;      // projectiles that struck somebody
		u32 noMana = 0;    // casts refused for want of mana
		i32 lastDamage = -1;
		i32 lastSpell = -1;
	};
	const MagicStats& magicStats() const { return _magic; }
	// Bow and crossbow: the arrows shot, the ones that hit, and the last damage.
	struct RangedStats {
		u32 shot = 0, hits = 0, misses = 0, noAmmo = 0;
		i32 lastDamage = -1;
	};
	const RangedStats& rangedStats() const { return _ranged; }
	bool aiming() const { return _shot.stage == SHOT_AIM; }
	// The things lying about in this world, and the one the character could
	// pick up now (A, hands empty; -1 for none).
	const Loot& loot() const { return _loot; }
	Loot& loot() { return _loot; }
	const Particles& particles() const { return _particles; }
	Particles& particles() { return _particles; }
	int lootTarget() const { return _lootTarget; }
	// ---- talks (app_talk.cpp; the rules are npcs.h) ----
	// The character a press of A would talk to (-1 for none), and whether a
	// talk is on (the character stands and listens, the bottom screen has the menu).
	int talkTarget() const { return _talkTarget; }
	bool talking() const { return _npcs.talkView().active; }
	// ---- reading (app_doc.cpp) ----
	// A book, a letter or a map the scripts put up (Doc_Show) is read on the top screen, its pages turned
	// on the bottom one (A / R / D-Pad right: on, L / D-Pad left: back, B: close).
	bool reading() const { return _doc.active; }
	u32 readingPage() const { return _doc.page; }
	u32 readingPages() const { return _doc.pages; }
	// Reads an item of the documents' kind (its use function builds the document); false if it gave none.
	bool readItem(u32 symbol);
	// ---- the chests (containers.cpp) ----
	// The containers of this world, the one the character could open now (A; -1
	// for none), and what the character is doing at one.
	const Chests& chests() const { return _chests; }
	Chests& chests() { return _chests; }
	int chestTarget() const { return _chestTarget; }
	// The thing of the world (benches, beds, the grindstone ...) the character could use now (A; a mob's
	// number, -1 for none), and whether he is at one.
	int mobTarget() const { return _mobTarget; }
	int ladderTarget() const { return _ladderTarget; }
	// The name a thing of the world shows (its prompt).
	void mobName(u32 mob, char* out, size_t size) const;
	bool mobInUse() const { return _mobJob.stage != MJ_NONE; }
	// The dead the character could search now (A; a character's index, -1 for none), and the
	// one being searched (-1 for none: the open container is a chest).
	int corpseTarget() const { return _corpseTarget; }
	int corpseOpen() const { return _chest.stage != CH_NONE ? _chest.corpse : -1; }
	enum ChestStage : u8 { CH_NONE, CH_LOCK, CH_GLIDE, CH_OPEN, CH_CLOSING };
	// Whether the character has learnt to sneak (the scripts' NPC_TALENT_SNEAK skill).
	bool heroCanSneak() const;
	u8 chestStage() const { return _chest.stage; }
	int chestIndex() const { return _chest.stage != CH_NONE ? int(_chest.index) : -1; }
	// The platform says R is held: the quick slots are shown on the top screen.
	struct ItemStats {
		u32 used = 0;        // potions and food that did their work
		u32 refused = 0;     // requests turned down (in a fight, too weak ...)
		u32 interrupted = 0; // spoilt by a blow
		u32 changed = 0;     // weapons, armour and rings put on or taken off
		u32 pickedUp = 0;    // things picked up from the ground
	};
	const ItemStats& itemStats() const { return _itemStats; }

private:
	// Reading.
	struct DocReader {
		enum { MAX_SCREENS = 32, LINES = 14, LINE = 100 };
		bool active = false;
		bool map = false;
		u32 page = 0, pages = 0;
		u32 serial = 0; // of the document last taken from the scripts
		char level[40] = "";
		i32 coords[4] = {};
		u8 lineCount[MAX_SCREENS] = {};
		char lines[MAX_SCREENS][LINES][LINE] = {};
	};
	// Falling: the highest the feet were since they left the ground, to see how far they dropped when they land.
	void fallCheck();
	void heroLanded(float drop);
	// The clock the scripts set (Wld_SetTime: the sleep until morning): the time jumps under a dip to black.
	float _timeFade = 0.0f;
	static constexpr float kTimeFadeLength = 1.6f;
	float _fallPeak = 0.0f;
	bool _fallAir = false;
	void docUpdate();
	void docLayout(const GameDocument& d);
	void docClose();
	void docCommand(const PanelCommand& c);
	void drawDocument();
	int docWidth(const char* text) const;
	DocReader _doc;
	// Magic.
	enum CastStage : u8 { CAST_NONE, CAST_INVEST, CAST_SHOOT };
	struct Cast {
		u8 stage = CAST_NONE;
		i16 spell = -1;
		u8 level = 0;
		i32 invested = 0;
		float tick = 0;   // ms since the last mana was put in
		float timer = 0;  // s into the release
		float length = 0; // s the release animation plays
		int target = -1;
		u32 fx = 0;       // the particle effect that follows the hands
		u32 item = 0xFFFFFFFFu;
	};
	struct Bolt {
		bool alive = false;
		bool arrow = false;       // an arrow (gravity, a roll to hit), else a spell
		float pos[3] = {0, 0, 0}, dir[3] = {0, 0, 1};
		float vel[3] = {0, 0, 0}; // cm/s
		i32 dmg[8] = {};          // an arrow: per damage type
		u32 ammo = 0xFFFFFFFFu;   // an arrow: the item it is drawn as
		float hitChance = 1.0f, critChance = 0.0f;
		float stuck = 0;          // s since it came to rest (an arrow stays a moment)
		float travelled = 0, range = 0;
		i16 spell = -1;
		u8 level = 0;
		u32 fx = 0;
		u32 fxCollide = 0;
		int owner = -1;           // who shot it (Game's index); -1: the player's character
	};
	static constexpr u32 MAX_BOLTS = 8;
	// Bow and crossbow: A held aims, let go shoots (then reloads).
	enum ShotStage : u8 { SHOT_NONE, SHOT_AIM, SHOT_SHOOT, SHOT_RELOAD };
	struct Shot {
		u8 stage = SHOT_NONE;
		float time = 0, length = 0;
		bool fired = false;
		bool again = false; // not the first of a run of shots
	};
	Shot _shot;
	void rangedInput(const Input& in);
	void rangedUpdate(float dt);
	void arrowShoot();
	u8 rangedMode();
	void drawArrows(const float light[3]);
	void magicInput(const Input& in);
	void magicUpdate(float dt);
	void castBegin();
	void castRelease(bool letGo);
	void castCommit();
	void castCancel();
	void boltShoot(const GameSpell& sp, i32 spell, i32 level);
	void boltUpdate(float dt);
	void boltImpact(Bolt& b, int victim, Vec3 at);
	// An inhabitant shoots (Npcs::ShotHook): an arrow of its bow (`spell` < 0) or a spell, at `target`.
	static bool npcShotThunk(void* user, u32 shooter, int target, i32 spell, i32 level);
	bool npcShot(u32 shooter, int target, i32 spell, i32 level);
	Bolt* freeBolt();
	Vec3 handPos() const;
	u32 nextSpell(int dir) const;
	Cast _cast;
	Bolt _bolts[MAX_BOLTS];
	u32 _spellItem = 0xFFFFFFFFu;
	bool _attackHeld = false;
	MagicStats _magic;
	RangedStats _ranged;
	void checkPortals();
	void soundEvent(const ClipSfxEvent& event, int npc);
	void startSound();
	void startSpeech();
	float speechBegin(u32 lineHash, int speaker, bool hero);
	static float speechPlayThunk(void* user, u32 lineHash, int speaker, bool hero);
	static void speechStopThunk(void* user);
	float chatterBegin(u32 lineHash, int speaker, bool overlay);
	static float chatterThunk(void* user, u32 lineHash, int speaker, bool overlay);
	static void soundEventThunk(void* user, const ClipSfxEvent& event, int npc);
	// The fight's sounds by name ("CS_IAM_ME_FL|CS_IAM"): the first the scripts know, at character `npc`.
	void namedSound(const char* names, int npc, float gain);
	static void namedSoundThunk(void* user, const char* names, int npc, float gain);
	// How fast the player's character goes over the ground (cm/s, smoothed): a pursuer sees it running.
	float _heroSpeed = 0;
	Vec3 _heroLastFeet {0, 0, 0};
	u8 _heroWeaponHeard = 0; // the hero's hands when that last made a sound
	static bool soundLocate(void* user, ScriptInstance* who, Vec3& pos);
	void placeCamera(float dt);
	bool talkCamera(Vec3& look, Vec3& pos);
	// The parts of a frame of the game with a character.
	Input steerInput(const Input& in, float dt);
	void fightInput(const Input& in, float dt);
	void updateTarget(const Input& in, float dt);
	void heroBlow();
	u8 meleeMode();
	bool heroTake(u8 mode);
	static void heroHitThunk(void* user, u32 attacker);
	void heroHit(u32 attacker);
	void syncHeroNpc();
	void respawn(const char* statePath);
	int heroIndex() const;
	bool projectToScreen(Vec3 p, int& x, int& y) const;

	ReadAhead _ahead; // (before the archive and the world, which ask it: it is the last to go)
	u32 _aheadBudget = 0;
	float _stereo = 0.0f;
	Pak _pak;
	AudioOut* _audio = nullptr;
	ScriptVm _sfxVm, _musicVm; // the sounds (SFX.DAT) and music themes (MUSIC.DAT) of the game: scripts of their own
	Sound _sound;
	Music _music;
	Speech _speech;
	float _musicScan = 0, _musicHold = 0; // (musicSituation)
	Speech _chatter {Speech::WORLD_CHANNEL}; // what the world says, outside the player's talk
	World _world;
	static constexpr u32 kWantMobs = 8;
	Vec3 _wantMobs[kWantMobs];
	u32 _wantMobCount = 0, _wantMobFrame = 0;
	Player _player;
	Hero _hero;
	FrameBudget _budget;
	Npcs _npcs;
	Sky _sky;
	Weather _weather;
	Camera _cam;
	Environment _env;
	const char* _error = "";
	double _clock = 0;       // seconds since init: drives texture animation
	u32 _barrierHum = 0;     // the Barrier's hum playing (a Sound handle)
	const char* _barrierName = nullptr; // the scripts' name of it, found once (null: none)
	bool _barrierSearched = false;
	void weatherSound(float dt);
	static constexpr float kSunShadowDark = 0.42f; // how dark the sun's shadow is at its darkest (gfx::setSunShadow)
	u32 _rainLoop = 0;                  // the rain's sound playing
	const char* _rainName = nullptr;    // the scripts' name of it (null: none)
	const char* _thunderName = nullptr;
	bool _weatherSoundSearched = false;
	float _rainRetry = 0.0f;            // seconds before asking for the rain sound again
	float _thunderWait = -1.0f;         // seconds until the thunder is heard (<0: none)
	u8 _thunderMade = 0;                // how many thunders were made here (engine/weather_sfx.h; 0: the game's own)
	u32 _thunderTurn = 0;
	u32 _renderSkip = 0; // RenderSkip
	// The baker's own thunders (meta/weathersfx "N name" near, "F name" far; tools/bake/weather_sfx_bake.h): a near one when the
	// flash is close (its sound follows soon), a far one otherwise, taking turns within each.
	const char* _thunderNear[4] = {};
	const char* _thunderFar[4] = {};
	u8 _thunderNearCount = 0, _thunderFarCount = 0;
	bool _thunderClose = false;         // the thunder waiting is a near one
	float _fps = 60.0f;      // frames per second, smoothed (the counter of Settings::showFps)
	u32 _switches = 0;
	bool _portalLock = false; // just arrived: ignore portals until the player has left their boxes
	bool _underwater = false;
	bool _thirdPerson = true; // wanted; see thirdPerson()
	bool _heroVisible = false;
	bool _gfxRelief = false;
	bool _windRelief = false;
	float _wavesWanted = 0.0f; // the water's waves as set (cm; StreamConfig::waves is 0 while relieved)
	float _windWanted = 0.0f; // Graphics+ wind as set (StreamConfig::wind is 0 while relieved)
	CameraSet _cams = defaultCameraSet();
	Vec3 _camTarget {0, 0, 0}, _camOrigin {0, 0, 0}; // the looked-after point and the camera, both trailing
	bool _camPlaced = false;   // false: the next frame puts the camera where it belongs at once
	float _camElevation = 0;   // degrees the player has tilted the view away from the script's elevation
	float _eyePitch = 0;       // first person with a character: looking up and down
	UpdateTimes _times;
	RenderTimes _renderTimes;
	MoveRecorder _moves;
	Settings _settings;
	Menu _menu;
	Ui _ui;
	char _toast[96] = "";
	float _toastTime = 0;
	// The chapter card (IntroduceChapter): the last one seen, the one on
	// the screen and for how long it still is.
	u32 _chapterSeen = 0;
	// The diary: what was written before this was seen when it was last looked at; and whether it is looked at now.
	u32 _diarySeen = 0;
	bool _diaryOpen = false;
	GameChapter _chapterCard;
	float _chapterTime = 0, _chapterLength = 0;
	char _menuMessage[48] = "";
	void drawOverlay();
	// Controls: the scheme; the console scheme's camera, which the second
	// stick turns about the character (`_orbitYaw`), and for how long it has
	// been left alone.
	ControlScheme _scheme = CONTROLS_CLASSIC;
	float _orbitYaw = 0;
	float _lookIdle = 10.0f;
	float _steerLag = 0; // how far the character still has to turn (it leans into it)
	// The jump key was held while the hands were busy (inventory, chest, talk,
	// drinking): it does not jump until it has been let go once (B closes the
	// inventory and must not also jump).
	bool _jumpLatch = false;
	// Fighting.
	int _target = -1;
	bool _locked = false;
	float _targetScan = 0, _threatWait = 0;
	u8 _lastWeapon = WEAPON_FIST;
	u8 _heldBefore = 0; // what was in the hands the frame before (WeaponMode)
	float _downTime = 0, _deadTime = 0;
	const char* _statePath = nullptr; // where respawning after death loads from
	FightStats _fight;
	Mat4 _viewProj;
	// The inventory.
	void itemsUpdate(Input& in, float dt);
	void itemMessage(const char* text, bool refusal = true);
	bool startUse(u32 symbol);
	bool equipItem(u32 symbol, const GameItemInfo& it, u8 kind);
	void unequipAll();
	void equipRing(u32 symbol, bool on);
	bool meetsNeeds(const GameItemInfo& it, char* why, u32 size) const;
	void drawItemHud();
	// The inventory's part of the top screen is up (the info box, the hints).
	bool itemOverlay() const;
	void saveInventory(FILE* f) const;
	bool loadInventory(FILE* f);
	enum JobStage : u8 { JOB_NONE, JOB_PUTAWAY, JOB_START, JOB_USING, JOB_SWAP_AWAY, JOB_SWAP_DRAW, JOB_REDRAW };
	struct ItemJob {
		u8 stage = JOB_NONE;
		u8 redraw = 0;      // the weapon to take in hand again afterwards (WEAPON_NONE: none)
		u32 symbol = 0xFFFFFFFFu;
		float wait = 0;     // seconds in this stage
		bool applied = false;
		bool pickup = false; // picking a thing up from the ground (Loot index `loot`)
		u32 loot = 0;
		i32 before[6] = {}; // the attributes before the use (to say what it did)
	};
	ItemJob _job;
	u32 _queued = 0xFFFFFFFFu; // the next request, waiting for this one
	u32 _rings[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
	u32 _amulet = 0xFFFFFFFFu;
	u32 _quick[QUICK_SLOTS] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
	                           0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
	float _calm = 100.0f;      // seconds since the character was last in a fight
	u32 _lastBlows = 0;
	// The pictures of the things (baked "icon/<visual>") and the ones the archive lacks.
	ItemPictures _pictures;
	u16 _noPicture[256] = {};
	u32 _noPictureCount = 0;
	i16 thumbOf(u16 visual);
	// One line of the inventory (or of a chest, `carried` false): name, picture, action, info box.
	void fillEntry(ItemEntry& e, u32 symbol, i32 count, const GameNpc& h, bool carried);
	// The thing turning in the top screen's info box: its model.
	Model _showModel;
	u16 _showVisual = 0;
	bool _showTried = false;
	u32 _showSymbol = 0xFFFFFFFFu;
	void drawShowcase();
	u32 _logStamp = 0;	float _hostileScan = 0;
	u32 _invStamp = 0;         // what the view was made from (a hash)
	u32 _equipVersion = 0;     // counts changes of what is worn
	ItemStats _itemStats;
	InventoryView _inv;
	InventoryPanel _panel;
	Loot _loot;
	Triggers _triggers; // gates, lifts and the triggers that move them (triggers.h)
	static void triggerSoundThunk(void* user, const char* name, Vec3 at);
public:
	const Triggers& triggers() const { return _triggers; }
	Triggers& triggers() { return _triggers; }
private:
	Particles _particles;
	Shadows _shadows;
	Puddles _puddles;
	Rain _rain;
	GodRays _godRays;
	int _lootTarget = -1;
	// The chests.
	Chests _chests;
	int _chestTarget = -1;
	int _corpseTarget = -1;
	struct ChestJob {
		u8 stage = CH_NONE;
		u32 index = 0;
		float time = 0;
		Vec3 from {0, 0, 0}, to {0, 0, 0};
		float yawFrom = 0, yawTo = 0;
		u8 progress = 0, picks = 0;
		int corpse = -1;     // a dead character's index when it is a body that is searched (then `index` is unused)
		bool shown = false;  // the chest page has been put up (closing it ends the use)
		char news[64] = ""; // the lock page's line (a pick broke)
		int door = -1;      // a door's lock being picked (Triggers' index), not a chest's
	};
	ChestJob _chest;
	// The world's things the player uses (interact.cpp): benches, beds, the grindstone, fires, levers ...
	// Only those the world's maker gave a name (a thing without one is for the inhabitants).
	struct MobJob {
		u8 stage = 0; // MJ_*
		u32 mob = 0;
		float time = 0;
		Vec3 from {0, 0, 0}, to {0, 0, 0};
		float yawFrom = 0, yawTo = 0;
		bool stay = false;   // held until the player lets go (a bed, a bench)
		bool ran = false;    // its state function has run
		u8 slotKind = 0;     // the place used (MobSlotKind): which "_FRONT" / "_BACK" animations
		bool bed = false;    // a bed: lying in it, the sleep talk (the scripts', else the engine's menu of hours)
		bool talked = false; // a talk came up while in it: he gets up when it is over
		bool offered = false; // the engine's sleep menu has been put up
	};
	enum MobStage : u8 { MJ_NONE, MJ_GLIDE, MJ_IN, MJ_HELD, MJ_OUT };
	MobJob _mobJob;
	int _mobTarget = -1;
	int _doorTarget = -1; // the door in front (Triggers' index), -1: none
	int _ladderTarget = -1; // a ladder to take hold of with A (World's index), -1: none
	bool _ladderTop = false; // ... from its top
	void doorUse(u32 door);
	void mobUpdate(Input& in, float dt);
	int mobNearest() const;
	bool mobStart(u32 mob);
	void mobEnd(bool hurried);
	void talkUpdate(Input& in, float dt);
	void drawSubtitle();
	// The Barrier's hum: heard outdoors while the setting is on, if the scripts have a sound for it.
	void barrierHum();
	int _talkTarget = -1;
	void chestUpdate(Input& in, float dt);
	void chestCommand(const PanelCommand& c);
	void chestStart(u32 index);
	void corpseStart(u32 npc);
	// What a body has to give: its things (not the armour it wears), in the order it got them.
	u32 corpseStackCount(u32 npc) const;
	bool corpseStack(u32 npc, u32 k, u32& symbol, i32& count) const;
	i32 corpseTake(u32 npc, u32 symbol, i32 amount);
	int corpseNearest(bool any) const;
	const char* corpseName(u32 npc, char* out, size_t size) const;
	void chestBegin();         // walks to the chest and opens it
	void chestEnd(bool hurried);
	void chestMove(u32 symbol, i32 amount, bool toChest);
	void chestView(const GameNpc& hero);
	const char* chestName(u32 index) const; // "Большой сундук" ...
	// ---- trade (trade.cpp) ----
	// The wares of the trader the player talks to: his things, not the money and not what he wears, in the order
	// he got them. `k`-th stack, false past the last.
	bool traderStack(u32 npc, u32 k, u32& symbol, i32& count) const;
	void tradeMove(u32 symbol, i32 amount, bool sell);
	void tradeView(const GameNpc& hero);              // the trade page: the wares into _inv.chest
	void tradeDecoratePack(InventoryView& v);         // prices on the pack's things, the money left out
};

} // namespace g3
