// Platform-independent application layer: owns the pak, the world, the
// player and the sky. Platforms feed it input and call update()/render().
#pragma once
#include "g3ds/pak.h"
#include "chests.h"
#include "hero.h"
#include "inventory.h"
#include "loot.h"
#include "menu.h"
#include "npcs.h"
#include "panel.h"
#include "player.h"
#include "recorder.h"
#include "settings.h"
#include "sky.h"
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
};

class App {
public:
	// `worldName` may be null: the archive says which world the game starts in.
	bool init(const char* pakPath, const char* worldName);
	void shutdown();

	void update(const Input& in, float dt);
	void render();

	// ---- sound (sound.h) ----
	// The platform's output, given before init(); null: no sound. Must outlive the application.
	void setAudio(AudioOut* out) { _audio = out; }
	Music& music() { return _music; }
	const Music& music() const { return _music; }
	Speech& speech() { return _speech; }
	const Speech& speech() const { return _speech; }
	Sound& sound() { return _sound; }
	const Sound& sound() const { return _sound; }
	// While update() is not called (the pause menu): lets sounds that ran out go and keeps the volume current.
	void soundIdle(float dt);

	// Sets draw distance, streaming radii, fog and far plane consistently.
	void setDrawDistance(float cm);
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
	Player& player() { return _player; }
	const Hero& hero() const { return _hero; }
	// The characters the game's scripts put into the world; not loaded with
	// an archive that has no scripts.
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
	World& world() { return _world; }
	const World& world() const { return _world; }
	Pak& pak() { return _pak; }
	SkyColors skyColors() const { return skyColorsAt(_env.timeOfDay); }
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
	// Rings and the amulet worn (SCRIPT_NONE for none).
	u32 ring(int i) const { return i >= 0 && i < 2 ? _rings[i] : SCRIPT_NONE; }
	u32 amulet() const { return _amulet; }
	// The faults menu's: a set of things of every kind the scripts define, to
	// try the inventory with. Returns how many kinds were given.
	u32 giveItemKit();
	// The things lying about in this world, and the one the character could
	// pick up now (A, hands empty; -1 for none).
	const Loot& loot() const { return _loot; }
	Loot& loot() { return _loot; }
	int lootTarget() const { return _lootTarget; }
	// ---- talks (app_talk.cpp; the rules are npcs.h) ----
	// The character a press of A would talk to (-1 for none), and whether a
	// talk is on (the character stands and listens, the bottom screen has the menu).
	int talkTarget() const { return _talkTarget; }
	bool talking() const { return _npcs.talkView().active; }
	// ---- the chests (containers.cpp) ----
	// The containers of this world, the one the character could open now (A; -1
	// for none), and what the character is doing at one.
	const Chests& chests() const { return _chests; }
	Chests& chests() { return _chests; }
	int chestTarget() const { return _chestTarget; }
	// The dead the character could search now (A; a character's index, -1 for none), and the
	// one being searched (-1 for none: the open container is a chest).
	int corpseTarget() const { return _corpseTarget; }
	int corpseOpen() const { return _chest.stage != CH_NONE ? _chest.corpse : -1; }
	enum ChestStage : u8 { CH_NONE, CH_LOCK, CH_GLIDE, CH_OPEN, CH_CLOSING };
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
	void checkPortals();
	void soundEvent(const ClipSfxEvent& event, int npc);
	void startSound();
	void startSpeech();
	float speechBegin(u32 lineHash, int speaker, bool hero);
	static float speechPlayThunk(void* user, u32 lineHash, int speaker, bool hero);
	static void speechStopThunk(void* user);
	static void soundEventThunk(void* user, const ClipSfxEvent& event, int npc);
	// The fight's sounds by name ("CS_IAM_ME_FL|CS_IAM"): the first the scripts know, at character `npc`.
	void namedSound(const char* names, int npc, float gain);
	static void namedSoundThunk(void* user, const char* names, int npc, float gain);
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

	Pak _pak;
	AudioOut* _audio = nullptr;
	ScriptVm _sfxVm, _musicVm; // the sounds (SFX.DAT) and music themes (MUSIC.DAT) of the game: scripts of their own
	Sound _sound;
	Music _music;
	Speech _speech;
	World _world;
	Player _player;
	Hero _hero;
	Npcs _npcs;
	Sky _sky;
	Camera _cam;
	Environment _env;
	const char* _error = "";
	double _clock = 0;       // seconds since init: drives texture animation
	u32 _barrierHum = 0;     // the Barrier's hum playing (a Sound handle)
	const char* _barrierName = nullptr; // the scripts' name of it, found once (null: none)
	bool _barrierSearched = false;
	float _fps = 60.0f;      // frames per second, smoothed (the counter of Settings::showFps)
	u32 _switches = 0;
	bool _portalLock = false; // just arrived: ignore portals until the player has left their boxes
	bool _underwater = false;
	bool _thirdPerson = true; // wanted; see thirdPerson()
	bool _heroVisible = false;
	CameraSet _cams = defaultCameraSet();
	Vec3 _camTarget {0, 0, 0}, _camOrigin {0, 0, 0}; // the looked-after point and the camera, both trailing
	bool _camPlaced = false;   // false: the next frame puts the camera where it belongs at once
	float _camElevation = 0;   // degrees the player has tilted the view away from the script's elevation
	float _eyePitch = 0;       // first person with a character: looking up and down
	UpdateTimes _times;
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
	int _lootTarget = -1;
	// The chests.
	Chests _chests;
	int _chestTarget = -1;
	int _corpseTarget = -1;
	bool _corpseBlocked = false; // a body in reach, but the weapon is drawn
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
	};
	ChestJob _chest;
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
};

} // namespace g3
