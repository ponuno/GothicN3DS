#include "lang.h"
#include "app.h"
#include "trace.h"
#include "weather_sfx.h"
#include "stereo.h"
#include "bytes.h"
#include "saves.h"

#include "sys.h"

#include <stdlib.h>
#include <string.h>

namespace g3 {

// The original climbs from the ground only what stands within 55 cm of the body's middle (MoveAlgo::climbMove): beyond
// the body's edge (30 cm) that is this much. Further off, a jump is a jump forward.
static const float kGroundClimbReach = 25.0f;

// A fire burns where its torch is drawn (Particles::setGate).
static bool emitterShown(void* world, Vec3 pos, Vec3 eye, int& chunk, bool wasOn) {
	return static_cast<const World*>(world)->pointShown(pos, eye, chunk, wasOn);
}

bool App::init(const char* pakPath, const char* worldName) {
	if (!_pak.open(pakPath)) {
		_error = "cannot open data.pak";
		return false;
	}
	// The read-ahead has an archive handle of its own; the world's loader and the inhabitants ask it for their reads.
	if (_aheadBudget && _ahead.start(pakPath, _aheadBudget, true)) {
		_ahead.setEnabled(_settings.readAhead != 0);
		_ahead.attach(_pak);
		_world.setReadAhead(&_ahead);
		_npcs.setReadAhead(&_ahead);
		_loot.setReadAhead(&_ahead);
	}
	char start[64] = "world";
	if (worldName && *worldName) {
		snprintf(start, sizeof(start), "%s", worldName);
	} else if (const PakEntry* e = _pak.find("meta/start")) {
		char buf[64] = {};
		if (e->size > 0 && e->size < sizeof(buf) && _pak.read(*e, buf)) memcpy(start, buf, sizeof(start));
	}
	if (!_world.open(&_pak, start)) {
		_error = "world not found in data.pak (or baked by another g3ds-bake version)";
		_pak.close();
		return false;
	}
	_grass.enterWorld(_pak, this->worldName()); // (Graphics+: the new grass, if the world has it)
	loadFogZones();
	// Optional: archives without a character stay first-person.
	_hero.load(_pak);
	// The game's own camera set-ups, if the archive has them.
	_cams = defaultCameraSet();
	if (const PakEntry* e = _pak.find("meta/camera")) {
		CameraSet set;
		if (e->size == sizeof(set) && _pak.read(*e, &set) && set.magic == CAMERA_MAGIC && set.version == CAMERA_VERSION &&
		    set.modeCount == CAMERA_MODE_COUNT) {
			bool sane = true;
			for (const CameraMode& m : set.mode)
				sane = sane && m.bestRange >= 0.5f && m.bestRange <= 20.0f && m.bestElevation > -90.0f && m.bestElevation < 90.0f &&
				       m.rotOffsetX > -90.0f && m.rotOffsetX < 90.0f && m.veloTrans > 0.0f && m.veloTrans < 1000.0f;
			if (sane) _cams = set;
		}
	}
	const WorldHeader& h = _world.header();
	_cam.yaw = h.spawnYaw;
	_cam.pitch = 0;
	teleport({h.spawnPos[0], h.spawnPos[1], h.spawnPos[2]});
	setDrawDistance(_world.config().drawDistance);
	_clock = 0;
	_switches = 0;
	_portalLock = false;
	// The game's scripts, if the archive has them, and through them the
	// people of this world.
	_npcs.setHeldSource(&_loot, &_pak);
	if (_npcs.load(_pak)) {
		_npcs.enterWorld(_pak, _world, this->worldName(), _player.eye(), _env.timeOfDay);
		_loot.enterWorld(_pak, this->worldName(), _world.entryHash(), _npcs.game());
		_particles.enterWorld(_pak, this->worldName(), _npcs.game());
		_particles.setGate(emitterShown, &_world);
		_chests.enterWorld(_pak, this->worldName(), _world.entryHash(), _npcs.game());
		_triggers.enterWorld(_pak, this->worldName(), _npcs.game());
	}
	{
		Triggers::Hooks th;
		th.sound = triggerSoundThunk;
		th.user = this;
		_triggers.setHooks(th);
	}
	HeroLink link;
	link.hit = heroHitThunk;
	link.user = this;
	_npcs.setHeroLink(link);
	_world.setDynamic(&_triggers);
	// fix148: the lamps (after the particles: a world baked without their table takes its fires for them).
	_lamps.enterWorld(_pak, this->worldName(), _npcs.loaded() ? &_particles : nullptr);
	_world.setLamps(&_lamps);
	_loot.setLamps(&_lamps);
	_lampClock = -1.0;
	Npcs::ShotHook shots;
	shots.fire = npcShotThunk;
	shots.user = this;
	_npcs.setShotHook(shots);
	Npcs::BloodHook blood;
	blood.splash = bloodThunk;
	blood.user = this;
	_npcs.setBloodHook(blood);
	startSound();
	startSpeech();
	// The overlay (menus, the HUD): without it the game still runs.
	// The game's own font, if the archive has it (and Ui::init() puts it in the texture).
	ui::clearGameFont();
	if (const PakEntry* fe = _pak.find("font/ui")) {
		if (void* fd = _pak.readAlloc(*fe)) {
			ui::setGameFont(fd, fe->size);
			free(fd);
		}
	}
	_ui.init();
	// The bottom screen's inventory (a picture of its own, drawn by the processor).
	_panel.init();
	_panel.setPictures(&_pictures);
	_target = -1;
	_locked = false;
	_fight = FightStats();
	return true;
}

const char* App::worldName() const {
	const PakEntry* e = _world.isOpen() ? _pak.find(_world.entryHash()) : nullptr;
	return e ? _pak.name(*e) : "";
}

void App::teleport(Vec3 eye) {
	const bool flying = _player.flying;
	_player.spawn(eye);
	_player.flying = flying;
	_cam.pos = eye;
	_hero.reset(_player.feet, _cam.yaw);
	_orbitYaw = _cam.yaw;
	_camPlaced = false;
	_target = -1;
	_locked = false;
	_fallAir = false; // (put down somewhere: no fall)
}

bool App::enterWorld(u32 worldEntryHash, const Vec3* eye, float yaw) {
	const u32 previous = _world.entryHash();
	const PakEntry* e = _pak.find(worldEntryHash);
	if (!e || e->type != ASSET_WORLD || worldEntryHash == previous) return false;
	const float drawDistance = _world.config().drawDistance;
	// Give the old world's memory back before the new one asks for its own.
	_npcs.leaveWorld();
	chestEnd(true);
	_loot.leaveWorld();
	_triggers.leaveWorld();
	_particles.leaveWorld();
	_lamps.leaveWorld();
	_chests.leaveWorld();
	_lootTarget = -1;
	_chestTarget = -1;
	_mobTarget = -1;
	_ladderTarget = -1;
	_mobJob = MobJob();
	_corpseTarget = -1;
	_world.close();
	_grass.release();
	gfx::releaseDestroyed();
	if (!_world.open(&_pak, worldEntryHash)) {
		// Broken target: go back to where we were rather than to nowhere.
		if (previous && _world.open(&_pak, previous)) _grass.enterWorld(_pak, worldName()), loadFogZones();
		return false;
	}
	_grass.enterWorld(_pak, worldName());
	loadFogZones();
	const WorldHeader& h = _world.header();
	if (eye) {
		_cam.yaw = yaw;
		teleport(*eye);
	} else {
		_cam.yaw = h.spawnYaw;
		teleport({h.spawnPos[0], h.spawnPos[1], h.spawnPos[2]});
	}
	_cam.pitch = 0;
	setDrawDistance(drawDistance); // the far plane depends on the chunk size
	_world.preload(_cam.pos);
	progress(0.4f); // (loading a save into another world: its screen)
	_npcs.enterWorld(_pak, _world, worldName(), _player.eye(), _env.timeOfDay);
	if (_npcs.loaded()) {
		_loot.enterWorld(_pak, worldName(), _world.entryHash(), _npcs.game());
		_particles.enterWorld(_pak, worldName(), _npcs.game());
		_particles.setGate(emitterShown, &_world);
		_chests.enterWorld(_pak, worldName(), _world.entryHash(), _npcs.game());
		_triggers.enterWorld(_pak, worldName(), _npcs.game());
	}
	_lamps.enterWorld(_pak, worldName(), _npcs.loaded() ? &_particles : nullptr);
	_lampClock = -1.0;
	_portalLock = true;
	++_switches;
	_sound.stopAll();
	_sound.enterWorld(_pak, worldName());
	_rainLoop = 0;
	_thunderWait = -1.0f;
	_ripples.release();
	_rain.release();    // (what they asked of the ground was the other world's)
	_godRays.release();
	_music.enterWorld(_pak, worldName());
	return true;
}

namespace {

constexpr u32 STATE_MAGIC = fourcc('G', '3', 'S', 'T');
// 2: the inventory follows (App::saveInventory); a file of version 1 still
// loads, leaving the inventory as it is.
// 3: the scripts' and the characters' state follows (Game::save), in a
// section of its own; a file of version 2 still loads without it.
constexpr u32 STATE_VERSION = 3;
constexpr u32 GAME_SECTION = fourcc('G', 'S', 'C', 'R');

struct SavedState {
	u32 magic;
	u32 version;
	u32 world; // pak hash of the world asset
	u32 flying;
	float eye[3];
	float yaw, pitch;
	float timeOfDay;
	float drawDistance;
	u32 check; // sum of all preceding words
};
static_assert(sizeof(SavedState) == 48, "SavedState layout");

u32 stateCheck(const SavedState& s) {
	u32 words[sizeof(SavedState) / 4];
	memcpy(words, &s, sizeof(s));
	u32 sum = 0x9E3779B9u;
	for (size_t i = 0; i + 1 < sizeof(SavedState) / 4; ++i) sum = (sum ^ words[i]) * 16777619u;
	return sum;
}

bool finite(float v) { return v == v && v > -1.0e7f && v < 1.0e7f; }

} // namespace

bool App::saveState(const char* path) const {
	// (A quick save, and the one on leaving: the name made up, the picture of the menu's opening or of the frame on show.)
	return const_cast<App*>(this)->writeSave(path, nullptr);
}

// The position after the header (App::writeSave): as state.bin has always held it.
bool App::writeState(FILE* f) {
	SavedState s {};
	s.magic = STATE_MAGIC;
	s.version = STATE_VERSION;
	s.world = _world.entryHash();
	s.flying = _player.flying ? 1 : 0;
	const Vec3 eye = _player.eye();
	s.eye[0] = eye.x;
	s.eye[1] = eye.y;
	s.eye[2] = eye.z;
	s.yaw = steersHero() ? _hero.yaw() : _cam.yaw;
	s.pitch = steersHero() ? 0.0f : _cam.pitch;
	s.timeOfDay = _env.timeOfDay;
	s.drawDistance = _world.config().drawDistance;
	s.check = stateCheck(s);
	const bool ok = fwrite(&s, sizeof(s), 1, f) == 1;
	saveInventory(f);
	_loot.saveTaken(f);
	_chests.saveChanged(f);
	progress(0.35f);
	bool game = true;
	if (_npcs.loaded()) {
		_npcs.game().prepareSave(); // (fix195: LeGo writes its PermMem, as on the original's save)
		ByteSink payload;
		_npcs.game().save(payload);
		progress(0.65f);
		const u32 head[3] = {GAME_SECTION, u32(payload.size), byteCheck(payload.data, payload.size)};
		game = !payload.failed && fwrite(head, sizeof(head), 1, f) == 1 && (payload.size == 0 || fwrite(payload.data, payload.size, 1, f) == 1);
	}
	return ok && game;
}

// What the slots' menu shows of a file of before the header: the world and the hour of its position.
static bool peekState(const char* path, u32& world, float& timeOfDay) {
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	SavedState s {};
	const bool ok = fread(&s, sizeof(s), 1, f) == 1 && s.magic == STATE_MAGIC && s.check == stateCheck(s);
	fclose(f);
	if (!ok) return false;
	world = s.world;
	timeOfDay = s.timeOfDay >= 0.0f && s.timeOfDay < 24.0f ? s.timeOfDay : 0.0f;
	return true;
}

void App::refreshSlots() {
	char path[200];
	for (int k = 0; k <= SAVE_SLOTS; ++k) {
		slotPath(k, path, sizeof(path));
		SaveInfo& info = _slots[k];
		readSaveInfo(path, info, nullptr);
		if (!info.legacy) continue;
		u32 world = 0;
		if (!peekState(path, world, info.timeOfDay)) {
			info.readable = false;
			continue;
		}
		const PakEntry* e = _pak.find(world);
		snprintf(info.world, sizeof(info.world), "%s", e ? worldShortName(_pak.name(*e)) : "");
	}
}

bool App::loadState(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	// The header of a saved game (or none, in a file of before it): what follows is the position.
	SaveInfo header;
	if (!readSaveHeader(f, header, nullptr)) {
		fclose(f);
		return false;
	}
	heroTransformBack(); // (fix151: what is loaded is the character himself)
	progress(0.05f);
	SavedState s {};
	const bool read = fread(&s, sizeof(s), 1, f) == 1;
	if (!read || s.magic != STATE_MAGIC || (s.version != 1 && s.version != STATE_VERSION) || s.check != stateCheck(s)) {
		fclose(f);
		return false;
	}
	bool sane = finite(s.yaw) && finite(s.pitch) && s.timeOfDay >= 0.0f && s.timeOfDay < 24.0f &&
	            s.drawDistance >= 1000.0f && s.drawDistance <= 100000.0f;
	for (float v : s.eye) sane = sane && finite(v);
	if (!sane) {
		fclose(f);
		return false;
	}

	const Vec3 eye = {s.eye[0], s.eye[1], s.eye[2]};
	const bool otherWorld = s.world != _world.entryHash();
	// (Far off in the same world: the place is read in whole below, behind the progress screen, not chunk by chunk in view.)
	const Vec3 was = _player.eye();
	const float far2 = (eye.x - was.x) * (eye.x - was.x) + (eye.z - was.z) * (eye.z - was.z);
	if (otherWorld) {
		progress(0.1f);
		const u32 switches = _switches;
		if (!enterWorld(s.world, &eye, s.yaw)) {
			fclose(f);
			return false;
		}
		_switches = switches; // continuing is not travelling
		progress(0.6f);
	}
	_player.flying = s.flying != 0;
	_hero.getUp();
	if (GameNpc* h = _npcs.loaded() && heroIndex() >= 0 ? &_npcs.game().npcMut(u32(heroIndex())) : nullptr) h->down = DOWN_NONE;
	_deadTime = _downTime = 0;
	_cam.yaw = s.yaw;
	_cam.pitch = clamp(s.pitch, -1.5f, 1.5f);
	teleport(eye); // (the character takes its heading from the camera's)
	_env.timeOfDay = s.timeOfDay;
	setDrawDistance(s.drawDistance);
	// Standing in a portal's box when the game was left must not send the
	// player through it on arrival.
	_portalLock = true;
	// What was carried and worn (version 2).
	if (s.version >= 2 && loadInventory(f) && _loot.loadTaken(f)) {
		// (The containers as they were made, then the changes the state names.)
		chestEnd(true);
		if (_npcs.loaded()) _chests.resetChanges(_pak, worldName(), _world.entryHash(), _npcs.game());
		_chests.loadChanged(f);
		progress(0.7f);
		if (s.version >= 3) loadGameSection(f);
		progress(0.85f);
	}
	fclose(f);
	if (!header.legacy) _playSeconds = double(header.playSeconds);
	if (_progress.on && !otherWorld && far2 > _world.config().drawDistance * _world.config().drawDistance * 0.25f) {
		_world.preload(_cam.pos);
		progress(0.95f);
	}
	progress(1.0f);
	return true;
}

// The scripts' state (version 3): what the story has made of the world.
bool App::loadGameSection(FILE* f) {
	u32 head[3];
	if (!_npcs.loaded() || fread(head, sizeof(head), 1, f) != 1 || head[0] != GAME_SECTION || head[1] > (64u << 20)) return false;
	u8* bytes = static_cast<u8*>(malloc(head[1] ? head[1] : 1u));
	if (!bytes) return false;
	bool ok = (head[1] == 0 || fread(bytes, head[1], 1, f) == 1) && byteCheck(bytes, head[1]) == head[2];
	if (ok) {
		_npcs.leaveWorld(); // (the actors are made again from the state restored)
		ByteSource in(bytes, head[1]);
		ok = _npcs.game().restore(in);
		if (ok) _npcs.game().afterRestore(); // (fix195: LeGo reads its PermMem back, INIT_GLOBAL as the original runs it)
		_chapterSeen = _npcs.game().chapter().serial;
		_diarySeen = _npcs.game().logVersion();
		_lootTarget = -1;
		_target = -1;
		_locked = false;
	}
	free(bytes);
	return ok;
}

void App::setDrawDistance(float cm) {
	StreamConfig& cfg = _world.config();
	cfg.drawDistance = cm;
	cfg.loadRadius = cm + 2000.0f;   // have chunks ready before they become visible
	cfg.unloadRadius = cm + 6000.0f; // hysteresis against load/unload thrash
	// Chunk bounds are tested, not triangles: leave room for a whole chunk.
	_cam.zFar = cm + _world.header().chunkSize * 1.5f;
	cfg.distanceScale = 1.0f;
	_env.fogEnd = cm;
	_env.fogStart = cm * 0.2f;
	// The far layer (G10): the fog moves out to its end, and starts later -
	// past the full-detail chunks it would hide what the far layer is for.
	if (_world.hasFarLayer() && cfg.farDistance > cm) {
		_env.fogEnd = cfg.farDistance;
		_env.fogStart = max(cm * 0.5f, cfg.farDistance * 0.2f);
		_cam.zFar = cfg.farDistance + _world.header().chunkSize * 4.0f;
	}
}

void App::setDistanceScale(float scale) {
	StreamConfig& cfg = _world.config();
	scale = clamp(scale, 0.3f, 1.0f);
	if (_world.farActive()) scale = 1.0f;
	if (cfg.distanceScale == scale) return;
	cfg.distanceScale = scale;
	_env.fogEnd = cfg.drawDistance * scale;
	_env.fogStart = cfg.drawDistance * scale * 0.2f;
}

void App::setFarScale(float scale) {
	StreamConfig& cfg = _world.config();
	const float base = _settings.farCm();
	if (base <= 0.0f || !_world.hasFarLayer()) return;
	_farScaleSet = scale;
	float d = max(max(base * clamp(scale, 0.0f, 1.0f), 25000.0f), cfg.drawDistance + 1000.0f);
	if (_enclosed) d = cfg.drawDistance + 1500.0f; // (under a roof: the shortest, whatever the floor of the pacer)
	if (cfg.farDistance == d) return;
	cfg.farDistance = min(d, base);
	_env.fogEnd = cfg.farDistance;
	_env.fogStart = max(cfg.drawDistance * 0.5f, cfg.farDistance * 0.2f);
	_cam.zFar = cfg.farDistance + _world.header().chunkSize * 4.0f;
}

void App::updateEnclosed(float dt) {
	_enclosedClock += dt;
	if (_enclosedClock < 0.3f) return;
	_enclosedClock = 0.0f;
	bool now = false;
	if (_world.farActive() && !_world.indoor() && !_underwater) {
		const Vec3 e {_cam.pos.x, _cam.pos.y, _cam.pos.z};
		const auto blocked = [&](float dx, float dy, float dz) { return _world.sightBlocked(e, Vec3 {e.x + dx, e.y + dy, e.z + dz}); };
		if (blocked(0, 700, 0)) {
			const int walls = int(blocked(900, 0, 0)) + int(blocked(-900, 0, 0)) + int(blocked(0, 0, 900)) + int(blocked(0, 0, -900));
			now = walls >= 3;
		}
	}
	// Two answers alike in a row before it changes: a doorway is not a flicker of the far layer.
	if (now == _enclosed) {
		_enclosedVotes = 0;
		return;
	}
	if (++_enclosedVotes < 2) return;
	_enclosed = now;
	_enclosedVotes = 0;
	_farScaleSet += 0.0f;
	_world.config().farDistance = 0.0f; // (setFarScale sees the change)
	setFarScale(_farScaleSet);
}

void App::shutdown() {
	chapterPictureFree();
	freeSlotPicture();
	free(_savePic);
	_savePic = nullptr;
	_savePicOk = false;
	_chatter.shutdown();
	_speech.shutdown();
	_music.shutdown();
	_sound.shutdown();
	_sfxVm.release();
	_musicVm.release();
	_ui.release();
	_panel.release();
	_loot.release();
	_particles.release();
	_shadows.release();
	_puddles.release();
	_bloodStains.release();
	_rain.release();
	_ripples.release();
	_godRays.release();
	_grass.release();
	_lamps.leaveWorld();
	_chests.release();
	_pictures.release();
	_showModel.release();
	_npcs.release();
	_hero.release();
	_sky.release();
	_barrier.release();
	_world.close();
	_pak.close();
	_ahead.stop();
}

void App::checkPortals() {
	// The box is tested at hip height, so a portal lying on the floor counts.
	const Vec3 probe = {_player.feet.x, _player.feet.y + 90.0f, _player.feet.z};
	const WorldPortal* portal = _world.portalAt(probe);
	if (!portal) {
		_portalLock = false;
		return;
	}
	if (_portalLock || _player.flying) return;
	const WorldPortal target = *portal; // the table goes away with the old world
	const Vec3 eye = {target.targetPos[0], target.targetPos[1], target.targetPos[2]};
	if (!enterWorld(target.targetWorld, (target.flags & PORTALF_USE_SPAWN) ? nullptr : &eye, target.targetYaw))
		_portalLock = true; // do not retry a broken portal every frame
}

void App::update(const Input& given, float dt) {
	_playSeconds += double(dt);
	collisionStatsReset(); // (what collisionStats() says is one frame's worth)
	if (_npcs.loaded()) _settings.chapter = u8(clamp<i32>(_npcs.game().chapterNumber(), 0, 6)); // (the menu's chapter slider follows the game)
	// (fix152) A camera flight that holds the player: the keys do nothing but A, which skips it (the clock goes on).
	Input in = given;
	if (_triggers.cutsceneActive() && !_triggers.cutsceneMovable() && !_player.flying) {
		if (given.confirm && _triggers.cutsceneTime() > 0.5f) _triggers.cutsceneStop();
		in = Input {};
		in.timeStep = given.timeStep;
		in.toggleFly = given.toggleFly;
	}
	_clock += dt;
	updateEnclosed(dt);
	if (_bloodPain > 0.0f) _bloodPain = max(_bloodPain - dt, 0.0f);
	if (dt > 0.0f) _fps += (clamp(1.0f / dt, 0.0f, 120.0f) - _fps) * min(dt * 3.0f, 1.0f);
	_env.timeOfDay += in.timeStep;
	if (_env.dayLength > 0) _env.timeOfDay += dt * 24.0f / _env.dayLength;
	if (!(_env.timeOfDay > -1.0e4f && _env.timeOfDay < 1.0e4f)) _env.timeOfDay = 12.0f; // (fix134: not for ever below)
	while (_env.timeOfDay >= 24.0f) _env.timeOfDay -= 24.0f;
	while (_env.timeOfDay < 0.0f) _env.timeOfDay += 24.0f;
	trace("update: weather");
	_weather.setClock(_npcs.game().day(), _env.timeOfDay); // fix187: the game's rain schedule
	_weather.update(dt);
	_world.config().windWeather = _world.indoor() ? 1.0f : _weather.windScale();

	if (in.nextWorld) {
		// Debug: the next world asset in the archive, at its start point.
		u32 first = 0, next = 0;
		bool seenCurrent = false;
		for (u32 i = 0; i < _pak.count(); ++i) {
			const PakEntry& e = _pak.entry(i);
			if (e.type != ASSET_WORLD) continue;
			if (!first) first = e.hash;
			if (seenCurrent && !next) next = e.hash;
			if (e.hash == _world.entryHash()) seenCurrent = true;
		}
		enterWorld(next ? next : first, nullptr, 0);
	}

	if (in.toggleCamera) {
		_thirdPerson = !_thirdPerson;
		_camPlaced = false;
		_eyePitch = 0;
	}

	// Stream first, so the chunks the player is about to stand on are there.
	// Around the player, not the camera: the camera may be behind a wall.
	// The things the player may use (a bench, a bed, a barrel) within a few steps: the chunks they stand in come first.
	if (_npcs.loaded() && _npcs.mobs().loaded() && (_wantMobFrame++ % 10) == 0) {
		const Mobs& mobs = _npcs.mobs();
		const Vec3 pf = _player.feet;
		float best[kWantMobs];
		_wantMobCount = 0;
		for (u32 i = 0; i < mobs.count(); ++i) {
			const WorldMob& m = mobs.mob(i);
			const float dx = m.pos[0] - pf.x, dz = m.pos[2] - pf.z;
			const float d2 = dx * dx + dz * dz;
			if (d2 > 2500.0f * 2500.0f || !mobs.forPlayer(i)) continue;
			u32 at = _wantMobCount < kWantMobs ? _wantMobCount++ : kWantMobs;
			if (at == kWantMobs) { // (full: replaces the farthest, if this one is nearer)
				u32 far = 0;
				for (u32 k = 1; k < kWantMobs; ++k)
					if (best[k] > best[far]) far = k;
				if (best[far] <= d2) continue;
				at = far;
			}
			best[at] = d2;
			_wantMobs[at] = {m.pos[0], m.pos[1], m.pos[2]};
		}
	}
	for (u32 i = 0; i < _wantMobCount; ++i) _world.wantAt(_wantMobs[i]);
	const double t0 = sys::timeMs();
	trace("update: world");
	// (fix152: in a camera flight the world is there about the camera, which may be far from the character.)
	_world.update(_triggers.cutsceneActive() ? _cam.pos : _player.eye());
	const double t1 = sys::timeMs();
	trace("update: npcs");
	_npcs.update(_pak, _world, _player.eye(), _env.timeOfDay, dt);
	// fix195: a mod's Ikarus / LeGo: the time it reads, its frame functions (LeGo's FF_*, timers).
	_ikarusMs = 0.0f;
	if (_npcs.loaded() && _npcs.game().ikarus().active()) {
		trace("update: ikarus");
		const double ti = sys::timeMs();
		_npcs.game().ikarus().frame(dt * 1000.0f, false);
		_ikarusMs = float(sys::timeMs() - ti);
	}
	trace("update: loot");
	const double tLoot0 = sys::timeMs();
	_times.npcUpdate = float(tLoot0 - t1);
	if (_npcs.loaded()) _loot.update(_pak, _world, _npcs.game(), _player.eye());
	const double tTrig0 = sys::timeMs();
	trace("update: triggers");
	if (_npcs.loaded()) {
		// Riding a lift or a ferry: where the feet are on it before it moves, and after.
		Vec3 rideLocal {0, 0, 0};
		const int ride = _player.grounded && !_player.climbing ? _triggers.floorUnder(_player.feet, rideLocal) : -1;
		Vec3 others[16];
		u32 otherCount = 0;
		for (u32 k = 0; k < _npcs.drawnCount() && otherCount < 16; ++k) others[otherCount++] = _npcs.drawnFeet(k);
		_triggers.update(dt, _player.feet, _npcs.game(), others, otherCount);
		_npcs.game().setCutscene(_triggers.cutsceneActive() && !_triggers.cutsceneMovable(), _npcs.talking() ? _npcs.talkPartner() : -1);
		if (ride >= 0) {
			const Vec3 to = _triggers.fromLocal(ride, rideLocal);
			const Vec3 d = to - _player.feet;
			if (dot(d, d) < 200.0f * 200.0f) _player.feet = to; // (a jump of the mover is no ride)
		}
	}
	const double tLoot1 = sys::timeMs();
	_particles.setFadeStep(_world.fadeStep());
	trace("update: particles");
	_particles.update(dt, _player.eye());
	trace("update: hero");
	const double t2 = sys::timeMs();
	_times.triggers = float(tLoot1 - tTrig0);
	_times.particles = float(t2 - tLoot1);
	double tCtl = t2, tMove = t2, tBodies = t2, tFollow = t2; // (the character's parts; all t2 without one)

	const bool wasFlying = _player.flying;
	const bool steered = _hero.loaded() && !wasFlying && !in.toggleFly;
	const float turn = 1.8f * dt; // rad/s at full deflection
	if (_toastTime > 0.0f) _toastTime -= dt;
	if (steered) {
		// The character is steered; the view follows it (placeCamera).
		// Looking up and down tilts the view, not the character.
		updateTarget(in, dt);
		Input fightIn = in;
		const bool inventoryOpen = _panel.open();
		if (inventoryOpen && (_hero.down() || _player.swimming)) _panel.setCursor(false);
		if (_panel.open()) {
			// The inventory is open: the hands are in it, not on a weapon.
			fightIn.attack = fightIn.parry = fightIn.drawWeapon = fightIn.meleeSlot = fightIn.fistSlot = false;
			fightIn.magic = fightIn.attackHeld = false;
			fightIn.spellStep = 0;
			fightIn.focus = false;
		}
		chestUpdate(fightIn, dt);
		mobUpdate(fightIn, dt);
		itemsUpdate(fightIn, dt); // (a job holds the hands: its frames take the fight keys)
		torchUpdate(dt);          // (fix192)
		talkUpdate(fightIn, dt);
		docUpdate();
		// fix192: a map on the top screen moves with the stick (the character stands while it is up).
		if (_doc.active && _doc.map && _doc.pic.loaded() && (fabsf(in.moveX) > 0.2f || fabsf(in.moveZ) > 0.2f))
			docMapPan(in.moveX * 320.0f * dt, -in.moveZ * 320.0f * dt);
		fightInput(fightIn, dt);
		Input heroIn = steerInput(in, dt);
		// Sneaking is learnt (a teacher sets the talent's skill), as OpenGothic's Npc::canSneak has it.
		if (heroIn.toggleSneak && !_hero.sneaking() && !heroCanSneak()) {
			heroIn.toggleSneak = false;
			if (_hero.weapon() == WEAPON_NONE) itemMessage(tr("Вы не умеете красться"));
		}
		// D-Pad down in the fighting stance: a hop back, as the stick pulled back does.
		if (in.dodge && _hero.weapon() != WEAPON_NONE) {
			heroIn.moveZ = -1.0f;
			heroIn.strafe = 0;
		}
		// (A job that has the weapon put away and the item to take stands the character too: no step in the frame between.)
		const bool handsBusy = _hero.usingItem() || _job.stage == JOB_PUTAWAY || _job.stage == JOB_START || _panel.open() || _chest.stage != CH_NONE || talking();
		if (handsBusy) {
			// Drinking or eating, or looking into the inventory: the character
			// stands (it cannot jump off either). The camera still turns.
			heroIn.moveX = heroIn.moveZ = heroIn.strafe = 0;
			_jumpLatch = true;
		}
		// The key that closed the inventory (B) is still down in the frames after:
		// it jumps only once it has been let go and pressed again.
		if (_jumpLatch) {
			if (!heroIn.jump && !handsBusy) _jumpLatch = false;
			heroIn.jump = false;
		}
		// A jump near a wall: the hands take hold of the ledge, if there is one in reach.
		Ledge ledge;
		const bool grab = _hero.wantsLedge(_player) && _player.probeLedge(_world, _hero.yaw(), ledge);
		// The jump key on the ground: a ledge ahead is climbed, a wall is jumped up at
		// (the original's jump up), anything else is a jump forward.
		Ledge ground;
		bool wall = false;
		if (heroIn.jump && _hero.wantsJumpUp(_player)) {
			if (!_player.probeLedge(_world, _hero.yaw(), ground, kGroundClimbReach)) {
				const float yaw = _hero.yaw(), reach = _player.config.radius + kGroundClimbReach;
				const Vec3 dir = {sinf(yaw), 0, cosf(yaw)};
				for (int k = 0; k < 2; ++k) {
					const Vec3 a = {_player.feet.x, _player.feet.y + (k ? 160.0f : 100.0f), _player.feet.z};
					wall = wall || !_world.lineFree(a, a + dir * reach, 10.0f);
				}
			}
		}
		// What the character wears (Mdl_ApplyOverlayMds: a learnt fight talent, a potion's sprint).
		if (_npcs.loaded() && heroIndex() >= 0) {
			const Model* worn[4];
			// (fix151: a creature wears none of the human skeleton's.)
			u32 count = heroTransformed() ? 0u : _npcs.overlayModels(u32(heroIndex()), worn, 4);
			// fix192: the torch in the hand has the model's own way of carrying it (HUMANS_TORCH.MDS: OpenGothic's
			// Npc::setTorch puts it on), if the game has it.
			if (torchShown() && count < 4)
				if (const Model* t = _npcs.overlayModelNamed("humans_torch")) worn[count++] = t;
			_hero.setOverlays(worn, count);
		}
		const Drive drive = _hero.control(heroIn, _player, dt, grab ? &ledge : nullptr, ground.found ? &ground : nullptr, wall);
		tCtl = sys::timeMs();
		// The ladder's ways on and off take as long as the character's animations of them.
		_player.ladderMountTime = _hero.ladderMountTime();
		_player.ladderMountTopTime = _hero.ladderMountTopTime();
		_player.ladderExitTopTime = _hero.ladderExitTopTime();
		_player.ladderExitFootTime = _hero.ladderExitFootTime();
		_player.ladderExitRise = _hero.ladderExitRise();
		_player.update(_world, heroIn, _hero.yaw(), 0.0f, dt, &drive);
		// Using a thing: the body stays at the floor of its place (see MobJob::floorY).
		if ((_mobJob.stage == MJ_GLIDE || _mobJob.stage == MJ_IN || _mobJob.stage == MJ_HELD) && !_player.climbing && !_player.swimming && _player.grounded)
			_player.feet.y = _mobJob.floorY;
		// On a ladder the character faces its rungs (turning to them as it takes hold).
		if (_player.climbing) _hero.setYaw(_player.ladderYaw());
		tMove = sys::timeMs();
		// The inhabitants' bodies are in the way (not while climbing, using a thing, or lying).
		if (_npcs.loaded() && !_player.climbing && !_player.swimming && _mobJob.stage == MJ_NONE && _chest.stage == CH_NONE && !_hero.down()) {
			Vec3 feet = _player.feet;
			if (_npcs.pushHero(feet, _player.config.radius)) {
				// (Not into a wall: the body is put back out of it, sideways.)
				Vec3 chest = {feet.x, feet.y + 100.0f, feet.z};
				const Vec3 c0 = chest;
				_world.pushSphere(chest, _player.config.radius);
				feet.x += chest.x - c0.x;
				feet.z += chest.z - c0.z;
				_player.feet = feet;
			}
		}
		fallCheck();
		tBodies = sys::timeMs();
		const SkyColors sky = skyColors();
		const float indoorLight[3] = {1, 1, 1};
		_hero.follow(_player, _world, _world.indoor() ? indoorLight : sky.light, dt);
		tFollow = sys::timeMs();
		if (_hero.blowLanded() && _hero.weapon() != WEAPON_MAG && _hero.weapon() != WEAPON_BOW && _hero.weapon() != WEAPON_CBOW) heroBlow();
		magicUpdate(dt);
		rangedUpdate(dt);
		if (dt > 0.0f) {
			const float dx = _player.feet.x - _heroLastFeet.x, dz = _player.feet.z - _heroLastFeet.z;
			const float v = sqrtf(dx * dx + dz * dz) / dt;
			// (A teleport is no run.)
			_heroSpeed += ((v < 2000.0f ? v : 0.0f) - _heroSpeed) * min(1.0f, dt * 4.0f);
		}
		_heroLastFeet = _player.feet;
		syncHeroNpc();
	} else {
		// A free first-person camera: no character, or flying.
		_cam.yaw += in.lookX * turn;
		_cam.pitch = clamp(_cam.pitch + in.lookY * turn, -1.5f, 1.5f);
		_player.update(_world, in, _cam.yaw, _cam.pitch, dt);
		if (_hero.loaded()) {
			// Leaving or entering flight: the character goes on from where
			// the camera is and faces where it looks.
			if (!_player.flying || !wasFlying) {
				_hero.reset(_player.feet, _cam.yaw);
				_camPlaced = false;
				_camElevation = 0;
				_eyePitch = 0;
			} else {
				_hero.setYaw(_cam.yaw);
			}
		}
	}
	_ripples.update(_world, _player.feet, _player.flying ? 0.0f : _player.waterDepth, _player.swimming, dt);
	_moves.record(_world, _player, in, _hero.loaded() ? "GTAS"[_hero.body()] : '?', dt);
	trace("update: portals");
	checkPortals();
	const double t3 = sys::timeMs();
	trace("update: camera");
	placeCamera(dt);
	trace("update: lamps");
	updateLamps();
	trace("update: sound");
	_sound.setListener(_cam.pos, _cam.yaw);
	_sound.update(dt, _env.timeOfDay);
	trace("update: weather sound");
	weatherSound(dt);
	trace("update: barrier");
	_barrier.update(dt, _settings.barrier, _settings.barrierThunder, !_world.indoor(), _cam.pos, _sound, _pak);
	trace("update: music");
	_music.setSystemTheme(nullptr);
	_music.update(dt, _cam.pos, _env.timeOfDay, musicSituation(dt));
	_times.stream = float(t1 - t0);
	_times.npcs = float(t2 - t1);
	_times.script = _npcs.stats().scriptMs + _ikarusMs; // (fix195: with Ikarus' frame functions)
	_times.loot = float(tLoot1 - tLoot0);
	_times.player = float(t3 - t2);
	_times.heroControl = float(tCtl - t2);
	_times.heroMove = float(tMove - tCtl);
	_times.heroBodies = float(tBodies - tMove);
	_times.heroFollow = float(tFollow - tBodies);
	_times.heroRest = float(t3 - max(tFollow, max(tBodies, max(tMove, tCtl))));
	_times.camera = float(sys::timeMs() - t3);
}

// How far a small sphere gets from `from` along the unit vector `dir`, up to
// `range`: the room a camera has there.
static float cameraRoom(const World& world, Vec3 from, Vec3 dir, float range, float radius) {
	// Nothing anywhere near the way (the open country): no need to feel along it.
	const Vec3 to = from + dir * range;
	const float mn[3] = {min(from.x, to.x) - radius, min(from.y, to.y) - radius, min(from.z, to.z) - radius};
	const float mx[3] = {max(from.x, to.x) + radius, max(from.y, to.y) + radius, max(from.z, to.z) + radius};
	if (world.boxFree(mn, mx)) return range;
	// Steps of 1.5 radii (a wall between two of them touches the nearer sphere, as in World::lineFree), then the
	// step with the wall in it is halved a few times: about half the probes of the old 22 cm steps (fix88; this
	// was ~7 ms in tunnels and narrow streets).
	const float stepLen = max(radius * 1.5f, 8.0f);
	const int steps = max(int(range / stepLen) + 1, 2);
	for (int s = 1; s <= steps; ++s) {
		const float d = range * float(s) / float(steps);
		Vec3 probe = from + dir * d;
		if (world.pushSphere(probe, radius)) {
			float lo = range * float(s - 1) / float(steps), hi = d;
			for (int i = 0; i < 3; ++i) {
				const float mid = (lo + hi) * 0.5f;
				Vec3 p = from + dir * mid;
				if (world.pushSphere(p, radius)) hi = mid;
				else lo = mid;
			}
			return lo;
		}
	}
	return range;
}

// The dialogue shot, as OpenGothic's DialogMenu::dialogCamera sets it (the game's
// CAMMODDIALOG gives it nothing but the way the picture is kept clear): the
// camera looks at the middle between the two hips, a little above (50 cm), from
// as far as they are apart, level. It stands behind the one who is not speaking
// and turned 45 degrees from them: the character speaks, the shot is over the
// shoulder of the partner (turned from the partner's facing); the partner speaks,
// it is over the character's (turned from the character's facing the other way).
// `look` and `pos` are set when a talk is on. The way from the looked-at point
// to the camera is kept free of walls.
bool App::talkCamera(Vec3& look, Vec3& pos) {
	if (!_npcs.loaded() || !_npcs.talkView().active || !thirdPerson()) return false;
	const int partner = _npcs.talkPartner();
	if (partner < 0) return false;
	const GameNpc& n = _npcs.game().npc(u32(partner));
	const TalkView& tv = _npcs.talkView();
	const float hip = _hero.hipHeight();
	const Vec3 a = {_player.feet.x, _player.feet.y + hip, _player.feet.z};
	const Vec3 b = {n.pos[0], n.pos[1] + hip, n.pos[2]};
	const Vec3 span = b - a;
	const float dist = length(span);
	if (dist < 20.0f) return false;
	look = {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f + 50.0f, (a.z + b.z) * 0.5f};
	// (OpenGothic turns by angle = 90 - yaw; "other - 45" and "hero + 45" there are these here.)
	const bool partnerSpeaks = tv.saying && !tv.heroSays;
	const float yaw = partnerSpeaks ? _hero.yaw() - 0.78539816f : n.yaw + 0.78539816f;
	const Vec3 d = {-sinf(yaw), 0.0f, -cosf(yaw)}; // behind the way the shot looks
	const float room = cameraRoom(_world, look, d, dist, _cam.boomRadius);
	pos = look + d * min(room, dist);
	return true;
}

// Free camera and first person: at the player's eyes. Third person: behind
// the character, as the game's camera scripts set it up (see Camera).
void App::placeCamera(float dt) {
	const Vec3 eye = _player.eye();
	_heroVisible = false;
	if (!_hero.loaded() || _player.flying) {
		_cam.pos = eye;
		return;
	}
	// (fix152) A camera flight has the view: along its keys, at its target; the usual camera starts afresh after it.
	{
		Vec3 csPos, csLook;
		bool csHasLook = false;
		if (_triggers.cutscene(csPos, csLook, csHasLook)) {
			_cam.pos = csPos;
			if (csHasLook) {
				const Vec3 d = csLook - csPos;
				const float flat = sqrtf(d.x * d.x + d.z * d.z);
				if (flat > 0.01f) _cam.yaw = atan2f(d.x, d.z);
				_cam.pitch = clamp(atan2f(d.y, flat), -1.5f, 1.5f);
			}
			_camPlaced = false;
			_heroVisible = true;
			_hero.setTurnLag(0.0f);
			return;
		}
	}
	const float heroYaw = _hero.yaw();
	if (!thirdPerson()) {
		_cam.pos = eye;
		_cam.yaw = heroYaw;
		_cam.pitch = _eyePitch;
		_hero.setTurnLag(0.0f);
		return;
	}
	const float rad = 0.0174532925f;
	const CameraMode& def = _cams.mode[_player.swimming ? CAMERA_SWIM : CAMERA_NORMAL];
	const float sy = sinf(heroYaw), cy = cosf(heroYaw);

	// The point the camera looks after: the character's hips (plus the
	// script's offset, which is given in the character's own axes).
	const Vec3 feet = _player.feet;
	const Vec3 offset = {def.targetOffsetX * cy + def.targetOffsetZ * sy, def.targetOffsetY, -def.targetOffsetX * sy + def.targetOffsetZ * cy};
	Vec3 want = Vec3 {feet.x, feet.y + _hero.hipHeight(), feet.z} + offset;
	// In a talk the camera is the game's dialogue shot: over the shoulder of the
	// character, on the one it talks to.
	Vec3 talkDest {0, 0, 0};
	const bool talkCam = talkCamera(want, talkDest);
	if (!_camPlaced) _camTarget = want;
	else _camTarget = _camTarget + (want - _camTarget) * (1.0f - expf((talkCam ? -5.0f : -_cam.targetPace) * dt));
	// The leash has an end: the character cannot leave the picture.
	{
		const Vec3 d = _camTarget - want;
		const float len = length(d), most = 200.0f;
		if (len > most) _camTarget = want + d * (most / len);
	}

	// Where the camera belongs: at the script's range, up by its elevation,
	// behind the character (turned by the script's azimuth).
	float elevation = clamp(def.bestElevation + _camElevation, -30.0f, 85.0f) * rad;
	// Behind the character (classic), or wherever the second stick has
	// turned the camera about it (console).
	const bool orbit = _scheme == CONTROLS_CONSOLE;
	const float azimuth = (orbit ? _orbitYaw : heroYaw) + def.bestAzimuth * rad;
	auto backwards = [&](float elev) {
		const float ce = cosf(elev);
		return Vec3 {-sinf(azimuth) * ce, sinf(elev), -cosf(azimuth) * ce};
	};
	Vec3 dir = backwards(elevation);
	float range = def.bestRange * 100.0f;
	float room = talkCam ? range : cameraRoom(_world, _camTarget, dir, range, _cam.boomRadius);
	if (!talkCam && room < _cam.topDownBelow) {
		// No room behind (the back to a wall): the game looks down from above
		// then - steeply, or straight down if even that is in the wall.
		dir = backwards(_cam.topDownElevation * rad);
		room = cameraRoom(_world, _camTarget, dir, _cam.topDownRange, _cam.boomRadius);
		if (room < _cam.topDownRange * 0.8f) {
			const Vec3 up = backwards(89.5f * rad);
			const float above = cameraRoom(_world, _camTarget, up, _cam.topDownRange, _cam.boomRadius);
			if (above > room) {
				dir = up;
				room = above;
			}
		}
	}
	const Vec3 dest = talkCam ? talkDest : _camTarget + dir * room;
	if (!_camPlaced) _camOrigin = dest;
	else _camOrigin = _camOrigin + (dest - _camOrigin) * (1.0f - expf((talkCam ? -4.0f : -0.25f * def.veloTrans) * dt));
	_camPlaced = true;
	// Trailing must not take the camera through a wall: the way from the
	// looked-after point to the camera stays free.
	{
		const Vec3 d = _camOrigin - _camTarget;
		const float len = length(d);
		if (len > 1.0f) {
			const Vec3 unit = d * (1.0f / len);
			const float free = cameraRoom(_world, _camTarget, unit, len, _cam.boomRadius);
			if (free < len) _camOrigin = _camTarget + unit * free;
		}
	}

	// The view: at the looked-after point, then turned by the script's
	// offset - up, which puts the character into the lower half of the picture.
	const Vec3 look = _camTarget - _camOrigin;
	const float flat = sqrtf(look.x * look.x + look.z * look.z);
	const float lookYaw = flat < 4.0f ? heroYaw : atan2f(look.x, look.z);
	_cam.pos = _camOrigin;
	const float rotX = talkCam ? 0.0f : def.rotOffsetX, rotY = talkCam ? 0.0f : def.rotOffsetY;
	_cam.yaw = lookYaw - rotY * rad;
	// The scripts were written for a 4:3 picture. This one is wider and shows
	// less above and below; the view is tipped down by what is missing, so
	// that the lower edge - the character's feet - shows what it shows there.
	const float aspect = float(gfx::SCREEN_W) / float(gfx::SCREEN_H);
	const float wide = atanf(tanf(_cam.fovY * 0.5f) * aspect * 0.75f) - _cam.fovY * 0.5f;
	_cam.pitch = clamp(atan2f(look.y, flat) + rotX * rad - (talkCam ? 0.25f : 1.0f) * max(wide, 0.0f), -1.5f, 1.5f);
	// How far the view hangs behind the character's turn (console: how far
	// the character still has to turn to where it is steered).
	float lag = heroYaw + def.bestAzimuth * rad - lookYaw;
	if (!(fabsf(lag) < 1.0e4f)) lag = 0.0f; // (fix134: an angle gone to infinity would loop here for ever)
	while (lag > 3.14159265f) lag -= 6.2831853f;
	while (lag < -3.14159265f) lag += 6.2831853f;
	if (orbit) lag = _steerLag;
	if (talkCam) lag = 0.0f;
	_hero.setTurnLag(flat < 4.0f ? 0.0f : lag);
	// With the camera inside the character there is nothing to see of it but its inside.
	_heroVisible = length(look) > 45.0f;
}

void App::loadFogZones() {
	_bloodStains.clear(); // (a world was entered: the stains of the last one are not here)
	free(_fogZones);
	_fogZones = nullptr;
	_fogZoneCount = 0;
	char name[96];
	snprintf(name, sizeof(name), "%s/fog", worldName());
	const PakEntry* e = _pak.find(name);
	if (!e || e->size < sizeof(FogZonesHeader)) return;
	u8* blob = static_cast<u8*>(_pak.readAlloc(*e));
	if (!blob) return;
	FogZonesHeader h;
	memcpy(&h, blob, sizeof(h));
	if (h.magic == FOG_ZONES_MAGIC && h.version == FOG_ZONES_VERSION && h.count > 0 && h.count <= u32(FOG_ZONES_MAX) &&
	    e->size == sizeof(FogZonesHeader) + h.count * sizeof(FogZone)) {
		_fogZones = static_cast<FogZone*>(malloc(h.count * sizeof(FogZone)));
		if (_fogZones) {
			memcpy(_fogZones, blob + sizeof(FogZonesHeader), h.count * sizeof(FogZone));
			_fogZoneCount = h.count;
		}
	}
	free(blob);
}

void App::render() {
	if (_startActive) {
		renderStartMenu();
		return;
	}
	// The picture a save will carry: the frame on show as the menu opens (the game, not the menu over it).
	if (_menu.isOpen() && !_menuShot) {
		captureSavePicture();
		_menuShot = true;
	} else if (!_menu.isOpen()) {
		_menuShot = false;
		_slotsPage = -1;
		if (_slotPic) freeSlotPicture();
	}
	const SkyColors sky = skyColors();

	gfx::FrameDesc fd {};
	fd.view = viewLH(_cam.pos, _cam.yaw, _cam.pitch);
	fd.eye = _cam.pos;
	fd.fovY = _cam.fovY;
	fd.zNear = _cam.zNear;
	fd.zFar = _cam.zFar;
	const bool indoor = _world.indoor();
	const float fogScale = indoor ? 1.0f : _weather.fogScale();
	fd.fogStart = _env.fogStart * fogScale;
	fd.fogEnd = _env.fogEnd * fogScale;
	for (int i = 0; i < 3; ++i) {
		fd.fogColor[i] = fd.clearColor[i] = indoor ? _world.header().fogColor[i] : sky.horizon[i];
		fd.dayLight[i] = sky.light[i];
	}

	// Fog zones: in the swamp, the orc city ... the fog takes the zone's colour (by the light of the hour) and ends nearer; at the
	// zone's edge it fades into the world's own. (Not indoors: a mine has its one background colour.)
	if (!indoor && _fogZoneCount > 0) {
		const float day = clamp((sky.light[0] + sky.light[1] + sky.light[2]) / 3.0f, 0.0f, 1.0f);
		float col[3] = {float(fd.fogColor[0]), float(fd.fogColor[1]), float(fd.fogColor[2])};
		float end = fd.fogEnd;
		for (u32 i = 0; i < _fogZoneCount; ++i) {
			const FogZone& z = _fogZones[i];
			const float k = fogZoneWeight(z, _cam.pos.x, _cam.pos.y, _cam.pos.z);
			if (k <= 0.0f) continue;
			const float top = float(max(max(z.color[0], z.color[1]), max(z.color[2], u8(1))));
			for (int c = 0; c < 3; ++c) {
				const float target = (z.flags & FOGZ_OVERRIDE_COLOR) ? float(z.color[c]) * day : col[c] * (0.4f + 0.6f * float(z.color[c]) / top);
				col[c] += (target - col[c]) * k;
			}
			if (z.range > 500.0f && z.range < end) end += (z.range - end) * k;
		}
		if (end < fd.fogEnd) {
			fd.fogStart *= end / fd.fogEnd;
			fd.fogEnd = end;
		}
		for (int c = 0; c < 3; ++c) fd.fogColor[c] = fd.clearColor[c] = u8(clamp(col[c], 0.0f, 255.0f));
	}

	// Below a water surface the view drowns in murky green-blue after a few metres.
	float surface;
	_underwater = _world.waterBelow({_cam.pos.x, _cam.pos.y + 600.0f, _cam.pos.z}, 600.0f, surface);
	if (_underwater) {
		static const float murk[3] = {30.0f, 78.0f, 86.0f}, tint[3] = {0.45f, 0.80f, 0.85f};
		fd.fogStart = 0.0f;
		fd.fogEnd = min(fd.fogEnd, 1600.0f);
		for (int i = 0; i < 3; ++i) {
			const float light = indoor ? 0.5f : sky.light[i];
			fd.fogColor[i] = fd.clearColor[i] = u8(clamp(murk[i] * light, 0.0f, 255.0f));
			fd.dayLight[i] = sky.light[i] * tint[i];
		}
	}

	const Mat4 proj = perspectiveLH(_cam.fovY, float(gfx::SCREEN_W) / float(gfx::SCREEN_H), _cam.zNear, _cam.zFar);
	// Stereo 3D: the scene drawn once for each eye (the culling and the level of detail go by the middle view for both).
	const bool stereo = _stereo > 0.0f && gfx::stereoActive();
	gfx::FrameDesc eyeDesc[2] = {fd, fd};
	if (stereo) {
		StereoEye l, r;
		stereoEyes(_cam.pos, _cam.yaw, _stereo, l, r);
		eyeDesc[0].eye = l.pos;
		eyeDesc[0].view = viewLH(l.pos, l.yaw, _cam.pitch);
		eyeDesc[1].eye = r.pos;
		eyeDesc[1].view = viewLH(r.pos, r.yaw, _cam.pitch);
	}
	_renderTimes = RenderTimes {};
	gfx::setSunShadow(indoor ? 0.0f : sunShadowDarkness());
	// fix186: the clouds' shadows - the field of this frame, then a few of the sun's masks written anew with it.
	bool cloudsDrawn = false;
	{
		float sun[3];
		Sky::sunDirection(_env.timeOfDay, sun);
		float coverage = 0.0f, dark = 0.0f;
		Clouds::fromWeather(_weather.cloud(), _weather.rain(), sun[1], coverage, dark);
		if (_cloudCoverage >= 0.0f) coverage = _cloudCoverage;
		_world.config().clouds = _settings.sunShadows != 0 && _settings.cloudShadows != 0 && !(_renderSkip & SKIP_CLOUDS);
		const bool on = !indoor && _world.config().clouds && _world.hasSunMasks() && !_underwater;
		_clouds.set(coverage, on ? min(dark * _settings.cloudShadowScale(), 0.85f) : 0.0f, _clock);
		_world.updateClouds(_clouds, indoor ? 0.0f : sunShadowDarkness());
		cloudsDrawn = on && _clouds.active();
	}
	// Graphics+: the new grass - which tiles round the camera, made into meshes a little a frame (once, for both eyes).
	{
		Grass::Frame gf;
		gf.camera = _cam.pos;
		gf.radius = indoor || _underwater || (_renderSkip & SKIP_GRASS) ? 0.0f : grassRadius();
		gf.share = _settings.grassShare();
		gf.darkness = indoor ? 0.0f : sunShadowDarkness();
		gf.clouds = cloudsDrawn ? &_clouds : nullptr;
		// Who presses it aside: the hero first, then the inhabitants and beasts nearest the camera (within 15 m).
		if (_hero.loaded() && _heroVisible && !_player.flying) gf.press[gf.pressCount++] = Grass::Frame::Presser {_hero.feet(), Grass::TRAMPLE};
		float nearSq[Grass::PRESSERS];
		for (u32 i = 0; i < _npcs.drawnCount(); ++i) {
			if (!_npcs.drawnVisible(i)) continue;
			const Vec3 p = _npcs.drawnFeet(i);
			const float dx = p.x - _cam.pos.x, dy = p.y - _cam.pos.y, dz = p.z - _cam.pos.z;
			const float d = dx * dx + dy * dy + dz * dz;
			if (d > 1500.0f * 1500.0f) continue;
			const u32 first = gf.pressCount && _hero.loaded() && _heroVisible && !_player.flying ? 1u : 0u;
			u32 k = gf.pressCount;
			if (k == Grass::PRESSERS) {
				if (nearSq[k - 1] <= d) continue;
				--k;
			} else {
				++gf.pressCount;
			}
			while (k > first && nearSq[k - 1] > d) {
				gf.press[k] = gf.press[k - 1];
				nearSq[k] = nearSq[k - 1];
				--k;
			}
			gf.press[k] = Grass::Frame::Presser {p, clamp(_npcs.drawnRadius(i) * 1.5f, 60.0f, 200.0f)};
			nearSq[k] = d;
		}
		_grass.update(gf);
	}
	const auto drawEye = [&](const gfx::FrameDesc& d) {
		const double r0 = sys::timeMs();
		if (!indoor && !(_renderSkip & SKIP_SKY)) {
			gfx::setPass(gfx::PASS_SKY);
			_sky.setScene(SkyScene {_env.timeOfDay, _weather.cloud(), _weather.rain(), _weather.flash()});
			_sky.draw(_cam.pos, _cam.zFar * 0.5f, sky, float(_clock));
			_barrier.draw(_cam.pos, _cam.zFar * 0.5f);
		}
		gfx::setPass(gfx::PASS_WORLD);
		const Mat4 viewProj = mul(proj, fd.view);
		_viewProj = viewProj;
		const double r1 = sys::timeMs();
		// (The room the player's character is in picks the row of the visibility table: fix91.)
		_world.setCameraRoom(_npcs.rooms().roomAt(_player.feet));
		trace("render: world");
		if (!(_renderSkip & SKIP_WORLD)) _world.draw(viewProj, _cam.pos, u32(u64(_clock * 1000.0)));
		trace("render: grass");
		if (!(_renderSkip & (SKIP_GRASS | SKIP_WORLD))) _grass.draw(_world, frustumFromViewProj(viewProj));
		trace("render: npcs");
		const double r2 = sys::timeMs();
		const float indoorLight[3] = {1, 1, 1};
		_npcs.setFadeStep(_world.fadeStep());
		if (!(_renderSkip & SKIP_NPCS)) _npcs.draw(viewProj, indoor ? indoorLight : sky.light, &_world, _cam.pos);
		const double r3 = sys::timeMs();
		{
			// (Only what is in view and not too small to see: fix87.)
			const Frustum view = frustumFromViewProj(viewProj);
			if (!(_renderSkip & SKIP_THINGS)) _loot.draw(_cam.pos, indoor ? indoorLight : sky.light, 3500.0f, &view, _world.config().sizePixels, _world.config().focalPx);
			_triggers.setView(&view, _world.config().sizePixels, _world.config().focalPx);
		}
		trace("render: triggers");
		if (!(_renderSkip & SKIP_THINGS)) _triggers.draw(_pak, _world, _cam.pos, indoor ? indoorLight : sky.light, min(_world.drawReach() + 2000.0f, 20000.0f));
		drawShowcase();
		const double r4 = sys::timeMs();
		trace("render: hero shadow");
		_shadows.begin();
		// (Always: some 14 draw calls. It used to be given up at the half rate, so at the menu's steady 30 fps it never showed.)
		const bool castHero = _settings.shadows == SHADOWS_HERO_CAST;
		if (castHero && _heroVisible && _hero.loaded() && !(_renderSkip & SKIP_HERO_SHADOW)) {
			// The light: the sun by day, the moon at night, a lamp overhead indoors; fainter under cloud and rain.
			float lightDir[3] = {0.25f, -0.96f, 0.1f};
			float strength = 0.5f;
			if (!indoor) {
				float sun[3], moon[3];
				Sky::sunDirection(_env.timeOfDay, sun);
				Sky::moonDirection(_env.timeOfDay, moon);
				const float su = clamp((sun[1] - 0.02f) / 0.25f, 0.0f, 1.0f), mu = clamp((moon[1] - 0.02f) / 0.25f, 0.0f, 1.0f);
				const float s = su * su * (3.0f - 2.0f * su), m = 0.35f * mu * mu * (3.0f - 2.0f * mu);
				const float* to = s >= m ? sun : moon;
				strength = max(s, m) * 0.6f * (1.0f - 0.85f * _weather.cloud()) * (1.0f - 0.4f * _weather.rain());
				// (Never longer than about two and a half times the height: a low sun is held at 24 degrees.)
				const float e = max(to[1], 0.4f), ce = sqrtf(1.0f - e * e);
				const float h = sqrtf(to[0] * to[0] + to[2] * to[2]);
				const float hx = h > 1e-4f ? to[0] / h : 0.0f, hz = h > 1e-4f ? to[2] / h : 1.0f;
				lightDir[0] = -hx * ce;
				lightDir[1] = -e;
				lightDir[2] = -hz * ce;
			}
			// Before the hero, in the blended state: the hero drawn after it covers whatever of it would lie over his feet
			// (it is drawn a little towards the eye, see Shadows::cast).
			gfx::setPass(gfx::PASS_BLEND);
			_shadows.cast(_world, _cam.pos, _hero.model(), _hero.pose(), _hero.feet(), _hero.yaw(), _hero.lean(), lightDir, strength);
			gfx::setPass(gfx::PASS_WORLD);
		}
		trace("render: hero");
		if (_heroVisible && !(_renderSkip & SKIP_HERO)) {
			_hero.draw();
			drawArrows(_hero.light());
			const GameNpc* hn = heroNpc();
			if (hn && _hero.loaded())
				_loot.drawCarried(_pak, _npcs.game(), _hero.model(), _hero.pose(), _hero.yaw(), _hero.lean(), nullptr, _hero.weapon(),
				                  hn->melee, _hero.feet(), _hero.light());
			if (hn && _hero.loaded())
				_loot.drawRanged(_pak, _npcs.game(), _hero.model(), _hero.pose(), _hero.yaw(), _hero.lean(), nullptr, _hero.weapon(),
				                 hn->ranged, _hero.feet(), _hero.light());
			if (hn) drawHeldItems(); // (fix192: the torch, the thing in use)
		}
		const double r5 = sys::timeMs();
		gfx::setPass(gfx::PASS_BLEND);
		trace("render: puddles, shadows");
		// Graphics+: shadows on the ground, before the water (which then covers those under it).
		if (!indoor && _settings.wetGround && _weather.wet() > 0.12f && !(_renderSkip & SKIP_GROUND)) {
			_puddles.draw(_world, _cam.pos, _weather.wet(), _weather.rain(), sky.horizon, _clock);
		}
		_bloodStains.draw(_cam.pos, _clock);
		// Round ones: under the hero (unless his own shape is thrown), and with "all near" or his shape under the people and beasts.
		if (_settings.shadows != SHADOWS_OFF && !(_renderSkip & SKIP_GROUND)) {
			if (_heroVisible && _hero.loaded() && _settings.shadows != SHADOWS_HERO_CAST) _shadows.add(_hero.feet(), 48.0f);
			if ((_settings.shadows == SHADOWS_ALL || _settings.shadows == SHADOWS_HERO_CAST) && !_gfxRelief)
				for (u32 i = 0; i < _npcs.drawnCount(); ++i)
					if (_npcs.drawnVisible(i)) _shadows.add(_npcs.drawnFeet(i), _npcs.drawnRadius(i));
			_shadows.draw(_world, _cam.pos, indoor ? 0.7f : 0.6f);
		}
		trace("render: blended");
		if (!(_renderSkip & SKIP_BLENDED)) _world.drawBlended();
		trace("render: god rays");
		if (!indoor && _settings.godRays && !_underwater && !(_renderSkip & SKIP_GODRAYS)) {
			float sunDir[3];
			Sky::sunDirection(_env.timeOfDay, sunDir);
			const float el = sunDir[1];
			const float low = clamp((el - 0.03f) / 0.09f, 0.0f, 1.0f), high = 1.0f - clamp((el - 0.55f) / 0.25f, 0.0f, 1.0f);
			const float clear = clamp(1.0f - _weather.cloud() * 1.3f, 0.0f, 1.0f) * (1.0f - clamp(_weather.rain() * 2.0f, 0.0f, 1.0f));
			const float warm = clamp(1.0f - el * 2.5f, 0.0f, 1.0f);
			const float tint[3] = {255.0f, 235.0f - 55.0f * warm, 190.0f - 80.0f * warm};
			const float look[3] = {sinf(_cam.yaw), 0.0f, cosf(_cam.yaw)};
			_godRays.draw(_world, _cam.pos, look, sunDir, low * high * clear, tint, _clock);
		}
		trace("render: ripples");
		if (!(_renderSkip & SKIP_RAIN)) _ripples.draw(_world, _cam.pos, indoor ? indoorLight : sky.light);
		trace("render: rain");
		if (!indoor && (_weather.rain() > 0.03f || _weather.snow() > 0.03f) && !(_renderSkip & SKIP_RAIN)) _rain.draw(_world, _cam.pos, _weather.rain(), _weather.snow(), sky.light, _clock,
			                                  0.04f + 0.10f * clamp((_weather.windScale() - 0.55f) / 1.35f, 0.0f, 1.0f)); // fix187: slanted by the wind
		trace("render: particles");
		if (!(_renderSkip & SKIP_PARTICLES)) _particles.draw(d.view, _cam.pos, indoor ? indoorLight : sky.light);
		trace("render: overlay");
		if (!(_renderSkip & SKIP_OVERLAY)) drawOverlay();
		const double r6 = sys::timeMs();
		_renderTimes.world += float(r2 - r1);
		_renderTimes.npcs += float(r3 - r2);
		_renderTimes.things += float(r4 - r3);
		_renderTimes.hero += float(r5 - r4);
		_renderTimes.rest += float((r1 - r0) + (r6 - r5));
	};
	trace("render: frame begin");
	gfx::frameBegin(eyeDesc[0]);
	drawEye(eyeDesc[0]);
	if (stereo) {
		gfx::eyeBegin(eyeDesc[1]);
		drawEye(eyeDesc[1]);
	}
	trace("render: frame end");
	gfx::frameEnd();
	trace("render: done");
}

// ---- controls ----

namespace {

float wrapYaw(float a) {
	if (!(fabsf(a) < 1.0e4f)) a = 0.0f; // (fix134: an angle gone to infinity would loop here for ever)
	while (a > 3.14159265f) a -= 6.2831853f;
	while (a < -3.14159265f) a += 6.2831853f;
	return a;
}

} // namespace

// What the character is asked to do this frame, from the sticks. Classic: as
// they are (the controller and the character read them so). Console: the
// stick says which way to go as the camera sees it - the character turns
// that way by itself and runs; the second stick turns the camera about it.
// Locked on to an enemy with a weapon in hand, the character keeps facing it
// and the stick steps towards it, away from it (a hop back) and round it.
Input App::steerInput(const Input& in, float dt) {
	Input h = in;
	const float speed = (0.7f + 0.35f * float(_settings.cameraSpeed)) * dt; // rad: 2.45 a second at the middle setting
	const float tilt = _settings.invertY ? -in.lookY : in.lookY;
	_lookIdle = (fabsf(in.lookX) > 0.1f || fabsf(in.lookY) > 0.1f) ? 0.0f : _lookIdle + dt;
	_steerLag = 0;
	if (_scheme == CONTROLS_CLASSIC) {
		// On a ladder the character does not turn: forward is up and back is down.
		if (_player.climbing) h.moveX = h.lookX = 0;
		if (thirdPerson()) _camElevation = clamp(_camElevation - tilt * 60.0f * dt, -45.0f, 60.0f);
		else _eyePitch = clamp(_eyePitch + tilt * 1.8f * dt, -1.4f, 1.4f);
		// Locked on with a weapon in hand: the character keeps facing the
		// enemy (the camera behind it, so the enemy ahead); left and right
		// step round it instead of turning.
		const GameNpc* t = _target >= 0 && _npcs.loaded() ? &_npcs.game().npc(u32(_target)) : nullptr;
		if (thirdPerson() && _locked && t && _hero.weapon() != WEAPON_NONE) {
			h.steer = true;
			h.steerYaw = atan2f(t->pos[0] - _player.feet.x, t->pos[2] - _player.feet.z);
			if (h.strafe == 0.0f && fabsf(in.moveX) > 0.5f && fabsf(in.moveX) > fabsf(in.moveZ)) h.strafe = in.moveX > 0 ? 1.0f : -1.0f;
			if (h.strafe != 0.0f) h.moveZ = 0;
			h.moveX = h.lookX = 0;
		}
		return h;
	}
	if (!thirdPerson()) {
		// Through the eyes: the second stick turns the character, the first
		// one walks and steps aside.
		_eyePitch = clamp(_eyePitch + tilt * 1.8f * dt, -1.4f, 1.4f);
		h.lookX = _player.climbing ? 0.0f : in.lookX * (0.5f + 0.1f * float(_settings.cameraSpeed));
		h.moveX = 0;
		h.strafe = in.moveX > 0.5f ? 1.0f : in.moveX < -0.5f ? -1.0f : 0.0f;
		return h;
	}
	_camElevation = clamp(_camElevation - tilt * 60.0f * dt * (0.5f + 0.1f * float(_settings.cameraSpeed)), -45.0f, 60.0f);
	h.moveX = h.lookX = 0;
	h.strafe = 0;
	const float x = in.moveX, z = in.moveZ;
	const float m = min(sqrtf(x * x + z * z), 1.0f);
	const float heroYaw = _hero.yaw();
	const GameNpc* t = _target >= 0 && _npcs.loaded() ? &_npcs.game().npc(u32(_target)) : nullptr;
	const bool combat = _locked && t && _hero.weapon() != WEAPON_NONE;
	if (_locked && t) {
		// The camera comes round behind the character, the enemy ahead.
		const float toward = atan2f(t->pos[0] - _player.feet.x, t->pos[2] - _player.feet.z);
		_orbitYaw = wrapYaw(_orbitYaw + clamp(wrapYaw(toward - _orbitYaw), -4.0f * dt, 4.0f * dt));
	} else {
		_orbitYaw = wrapYaw(_orbitYaw + in.lookX * speed);
	}
	if (_player.climbing) {
		// On a ladder the stick says up or down - as the camera sees it, towards the wall or away from it - and
		// the character stays facing the ladder (it must not turn away to go down it, which it does when steered).
		// The camera comes round behind the climber by itself (taken hold of from the top, he has turned to it).
		if (fabsf(in.lookX) < 0.1f) _orbitYaw = wrapYaw(_orbitYaw + clamp(wrapYaw(heroYaw - _orbitYaw), -2.5f * dt, 2.5f * dt));
		float up = 0.0f;
		if (m > 0.2f) up = m * cosf(wrapYaw(_orbitYaw + atan2f(x, z) - heroYaw));
		h.steer = false;
		h.moveX = h.lookX = 0;
		h.moveZ = fabsf(up) >= 0.3f ? up : 0.0f;
		return h;
	}
	if (combat) {
		const float toward = atan2f(t->pos[0] - _player.feet.x, t->pos[2] - _player.feet.z);
		h.steer = true;
		h.steerYaw = toward;
		if (m > 0.2f) {
			const float rel = wrapYaw(_orbitYaw + atan2f(x, z) - heroYaw);
			const float fwd = m * cosf(rel), side = m * sinf(rel);
			h.moveZ = fabsf(fwd) >= 0.3f ? fwd : 0.0f;
			h.strafe = fabsf(side) > 0.5f && fabsf(side) > fabsf(fwd) ? (side > 0 ? 1.0f : -1.0f) : 0.0f;
			if (h.strafe != 0.0f) h.moveZ = 0;
		} else {
			h.moveZ = 0;
		}
		_steerLag = 0;
		return h;
	}
	if (m > 0.2f) {
		const float dir = wrapYaw(_orbitYaw + atan2f(x, z));
		const float err = wrapYaw(dir - heroYaw);
		h.steer = true;
		h.steerYaw = dir;
		// Facing far away from it: round first, then off (no circles).
		h.moveZ = fabsf(err) < 1.2f ? m : 0.0f;
		_steerLag = clamp(err, -0.3f, 0.3f);
		// The camera swings round behind a runner when the second stick is left alone.
		if (_settings.autoCamera && _lookIdle > 0.8f && h.moveZ > 0.0f) {
			const float rate = 0.9f * clamp(_hero.speed() / 450.0f, 0.0f, 1.2f) * dt;
			_orbitYaw = wrapYaw(_orbitYaw + clamp(wrapYaw(heroYaw - _orbitYaw), -rate, rate));
		}
	} else {
		h.moveZ = 0;
	}
	return h;
}

int App::heroIndex() const {
	if (!_npcs.loaded()) return -1;
	const GameNpc* h = _npcs.game().hero();
	return h ? int(h - &_npcs.game().npc(0)) : -1;
}

const GameNpc* App::heroNpc() const {
	const int i = heroIndex();
	return i >= 0 ? &_npcs.game().npc(u32(i)) : nullptr;
}

void App::bloodThunk(void* user, u32 victim, u32 attacker, i32 damage) { static_cast<App*>(user)->bloodAt(victim, attacker, damage); }

// Blood where a blow lands: the guild's own emitter (GIL_VALUES blood_emitter) if the scripts have one, else the engine's.
void App::bloodAt(u32 victim, u32 attacker, i32 damage) {
	if (!_npcs.loaded() || damage <= 0) return;
	Game& g = _npcs.game();
	if (victim >= g.npcCount()) return;
	const GameNpc& v = g.npc(victim);
	const i32 guild = g.guildOf(v);
	if (g.guildValue("BLOOD_DISABLED", guild, 0)) return;
	Vec3 at {v.pos[0], v.pos[1] + (g.isMonster(v) ? 70.0f : 120.0f), v.pos[2]};
	// Not for what is out of sight (or far): the pool is small.
	const Vec3 d {at.x - _cam.pos.x, at.y - _cam.pos.y, at.z - _cam.pos.z};
	if (d.x * d.x + d.y * d.y + d.z * d.z > 2500.0f * 2500.0f) return;
	(void)attacker;
	_bloodStains.add(_world, Vec3 {v.pos[0], v.pos[1], v.pos[2]}, clamp(float(damage) / 60.0f, 0.1f, 1.0f), _clock);
	const char* name = g.guildString("BLOOD_EMITTER", guild);
	float secs = 0.12f;
	if (name && *name && _particles.play(hashName(name), at, 0.0f, 0.6f)) return;
	_particles.play(Particles::bloodSplash(), at, 0.0f, secs + (damage > 30 ? 0.06f : 0.0f));
}

// Low health: the edge of the picture reddens and beats with the heart; a blow taken flashes it.
void App::drawLowHealth() {
	const i32 hp = heroHitPoints(), mx = heroHitPointsMax();
	if (mx <= 0) return;
	float low = 0.0f;
	if (hp > 0) low = clamp((0.35f - float(hp) / float(mx)) / 0.35f, 0.0f, 1.0f);
	const float flash = clamp(_bloodPain / 0.45f, 0.0f, 1.0f);
	float a = low * (0.45f + 0.25f * sinf(float(_clock) * (4.0f + 3.0f * low))) + flash * 0.5f;
	if (_hero.down() && hp <= 0) a = max(a, 0.6f);
	if (a <= 0.01f) return;
	const int W = gfx::SCREEN_W, H = gfx::SCREEN_H;
	// A few frames inside each other, the outer the reddest.
	for (int i = 0; i < 5; ++i) {
		const int t = 3 + i * 4, x = i * 5;
		const u32 al = u32(clamp(a * (1.0f - 0.18f * float(i)), 0.0f, 1.0f) * 70.0f);
		if (al == 0) continue;
		const u32 col = 0xB00000u << 8 | al;
		_ui.rect(0, 0, W, t + x, col);
		_ui.rect(0, H - t - x, W, H, col);
		_ui.rect(0, t + x, t + x, H - t - x, col);
		_ui.rect(W - t - x, t + x, W, H - t - x, col);
	}
}

i32 App::heroHitPoints() const {
	const GameNpc* h = heroNpc();
	return h ? _npcs.game().hitPoints(*h) : 0;
}

i32 App::heroHitPointsMax() const {
	const GameNpc* h = heroNpc();
	return h ? _npcs.game().attributeOf(*h, _npcs.game().constant("ATR_HITPOINTS_MAX", 1)) : 0;
}

// The scripts see what the player's character does: what it holds, which way it faces.
void App::syncHeroNpc() {
	const int i = heroIndex();
	if (i < 0) return;
	GameNpc& h = _npcs.game().npcMut(u32(i));
	// (Others see the weapon the moment it is reached for, not when the hands change.)
	h.weapon = _hero.weaponIntent();
	if (_hero.weapon() != _heroWeaponHeard) {
		// The hands have just changed: the sword's ring as it leaves the sheath.
		if (!_hero.down()) _npcs.emitDraw(u32(i), _heroWeaponHeard, _hero.weapon(), h.melee);
		_heroWeaponHeard = _hero.weapon();
	}
	_npcs.setHeroYaw(_hero.yaw());
	HeroLink link;
	link.hit = heroHitThunk;
	link.user = this;
	link.prehit = _hero.prehit();
	// Running as the scripts see it (BS_RUN): on the move faster than a walk, whatever the
	// animation (a blow taken on the run, a weapon in hand) - the pursuit's clock goes on.
	link.running = _hero.role() == Hero::RUN || (_heroSpeed > 250.0f && !_hero.sneaking());
	link.walking = !link.running && _heroSpeed > 40.0f;
	link.jumping = _hero.body() == Hero::AIRBORNE || _hero.body() == Hero::TAKEOFF;
	link.swimming = _player.swimming;
	link.sneaking = _hero.sneaking();
	_npcs.setHeroLink(link);
	// Finished off by somebody while it lay there.
	if (h.down == DOWN_DEAD && !(_hero.down() && _deadTime > 0.0f)) {
		_hero.fall(true, false);
		_deadTime = 0.001f;
	}
}

// Who is fought with: the one locked on to, else the nearest in front.
// fix148: the lamps near the camera and their flicker, once a step (the frame drawn twice - stereo, a still picture - stays
// the same); the hero takes their light.
void App::updateLamps() {
	const SkyColors sky = skyColors();
	Lamps::Frame lf;
	lf.eye = _cam.pos;
	lf.seconds = _clock;
	lf.dt = _lampClock < 0.0 ? -1.0f : float(clamp(_clock - _lampClock, 0.0, 1.0));
	_lampClock = _clock;
	for (int i = 0; i < 3; ++i) lf.dayLight[i] = sky.light[i];
	lf.characters = _settings.lamps >= 1 && !(_renderSkip & SKIP_LAMPS);
	lf.world = _settings.lamps >= 2 && !_gfxRelief && !_underwater;
	// fix192: the torch in the hand is a lamp of its own (a little above its flame).
	{
		static const float kTorch[3] = {255.0f, 176.0f, 96.0f};
		_lamps.setCarried(_torch.lit && _torch.shown && _torch.tipKnown, _torch.tip + Vec3 {0.0f, 12.0f, 0.0f}, 950.0f, kTorch);
	}
	_lamps.update(lf, &_world);
	float add[3] = {0.0f, 0.0f, 0.0f};
	if (_hero.loaded()) _lamps.lightAt({_hero.feet().x, _hero.feet().y + 90.0f, _hero.feet().z}, _hero.sky(), add, &_world);
	_hero.setLampLight(add);
}

void App::updateTarget(const Input& in, float dt) {
	if (!_npcs.loaded() || _hero.down()) {
		_target = -1;
		_locked = false;
		return;
	}
	// The weapon put away: the lock goes with it. A lock needs a weapon in hand at all - one taken with
	// ZR on an empty hand would stay (the C-stick is given to the target switch then and the camera is held
	// on the target, so it could not be turned until a weapon was drawn and put away).
	if (_hero.weapon() == WEAPON_NONE) _locked = false;
	_heldBefore = _hero.weapon();
	const float feet[3] = {_player.feet.x, _player.feet.y, _player.feet.z};
	const float aim = _scheme == CONTROLS_CONSOLE && thirdPerson() && !_locked ? _orbitYaw : _hero.yaw();
	if (_target >= 0) {
		const GameNpc& t = _npcs.game().npc(u32(_target));
		const float dx = t.pos[0] - feet[0], dz = t.pos[2] - feet[2];
		if (t.down != DOWN_NONE || !t.placed || !_npcs.actorOf(u32(_target)) || dx * dx + dz * dz > 2500.0f * 2500.0f) {
			_target = -1;
			_locked = false;
		}
	}
	if (in.focus) {
		if (_locked) {
			_locked = false;
		} else if (_hero.weapon() == WEAPON_NONE) {
			// (No weapon, no lock: only the camera comes round behind the character.)
			if (_scheme == CONTROLS_CONSOLE) _orbitYaw = _hero.yaw();
		} else {
			// The enemies first, then anybody.
			int t = _npcs.pickTarget(feet, aim, 1500.0f, 1.0f, -1, 0, true);
			if (t < 0) t = _npcs.pickTarget(feet, aim, 1500.0f, 1.0f);
			if (t >= 0) {
				_target = t;
				_locked = true;
			} else if (_scheme == CONTROLS_CONSOLE) {
				_orbitYaw = _hero.yaw(); // nobody there: the camera behind the character
			}
		}
	}
	if (_locked && in.switchTarget != 0.0f && _target >= 0) {
		// The next one to that side of the one locked on to.
		const GameNpc& t = _npcs.game().npc(u32(_target));
		const float from = atan2f(t.pos[0] - feet[0], t.pos[2] - feet[2]);
		const int next = _npcs.pickTarget(feet, from, 1500.0f, 1.6f, _target, in.switchTarget > 0 ? 1 : -1);
		if (next >= 0) _target = next;
	}
	// Not locked: what is in front is the target ("focus"), checked now and then.
	if (!_locked) {
		_targetScan -= dt;
		if (_targetScan <= 0.0f) {
			_targetScan = 0.25f;
			const bool armed = _hero.weapon() != WEAPON_NONE;
			int t = armed ? _npcs.pickTarget(feet, _hero.yaw(), 800.0f, 0.9f, -1, 0, true) : -1;
			if (t < 0) t = _npcs.pickTarget(feet, _hero.yaw(), armed ? 600.0f : 500.0f, armed ? 0.9f : 0.6f);
			_target = t;
			if (armed && _settings.autoTarget && t >= 0 &&
			    _npcs.game().attitude(u32(t), u32(heroIndex())) == _npcs.game().constant("ATT_HOSTILE", 0))
				_locked = true;
		}
	}
	// A weapon pointed at somebody: he perceives the threat.
	_threatWait -= dt;
	if (_target >= 0 && _hero.weapon() != WEAPON_NONE && _hero.weapon() != WEAPON_FIST && _threatWait <= 0.0f && heroIndex() >= 0) {
		const GameNpc& t = _npcs.game().npc(u32(_target));
		const float dx = t.pos[0] - feet[0], dz = t.pos[2] - feet[2];
		if (dx * dx + dz * dz < 500.0f * 500.0f) {
			_threatWait = 2.0f;
			_npcs.perceiveSingle(u32(_target), "PERC_ASSESSTHREAT", heroIndex());
		}
	}
}

// Weapons, blows, parrying; getting up again.
void App::fightInput(const Input& in, float dt) {
	if (_hero.down()) {
		if (_deadTime > 0.0f) {
			_deadTime += dt;
			if (in.confirm && _deadTime > 1.5f) respawn(_statePath);
		} else if ((_downTime -= dt) <= 0.0f) {
			// Come to: up again, with the hit points the blow left.
			_hero.getUp();
			if (heroIndex() >= 0) _npcs.game().npcMut(u32(heroIndex())).down = DOWN_NONE;
		}
		return;
	}
	if (!_npcs.loaded() && !_hero.loaded()) return;
	const auto take = [&](u8 mode) { heroTake(mode); };
	if (in.drawWeapon) {
		// As the original's space: away with it, or the last one used in hand - weapon, bow or spell (fix166).
		if (_hero.weapon() != WEAPON_NONE) {
			_switchTo = SCRIPT_NONE;
			take(WEAPON_NONE);
		} else {
			heroTakeLast();
		}
	}
	if (in.meleeSlot) {
		const u8 want = meleeMode();
		take(_hero.weapon() == want ? u8(WEAPON_NONE) : want);
	}
	// (On the console D-Pad up takes the fists when the hands are empty; with a weapon it is the blow ahead.)
	if (in.fistSlot) // (fix178: the D-Pad up no longer takes the fists)
		take(_hero.weapon() == WEAPON_FIST ? u8(WEAPON_NONE) : u8(WEAPON_FIST));
	if (in.cycleWeapon) heroCycleWeapon();
	heroSwitchTick();
	_attackHeld = in.attackHeld;
	magicInput(in);
	rangedInput(in);
	// (fix151) A creature fights with what it has: the attack key bares its teeth at once.
	if (in.attack && heroTransformed() && _hero.weapon() == WEAPON_NONE && !_hero.drawing()) take(WEAPON_FIST);
	if (in.attack && _hero.weapon() != WEAPON_NONE && _hero.weapon() != WEAPON_MAG && _hero.weapon() != WEAPON_BOW &&
	    _hero.weapon() != WEAPON_CBOW) {
		// The direction of the blow is the D-Pad's, held as A is pressed (left: from the left,
		// right: from the right, else ahead); the stick only moves.
		_hero.attack(in.blowDir < 0 ? 1 : in.blowDir > 0 ? 2 : 0);
	}
	_hero.setParry(in.parry && _hero.weapon() != WEAPON_NONE && _hero.weapon() != WEAPON_MAG);
}

// What the melee weapon on the belt is held as: one hand, two, or fists without one.
u8 App::meleeMode() {
	const GameNpc* h = heroNpc();
	if (!h || h->melee == SCRIPT_NONE) return WEAPON_FIST;
	const GameItemInfo* w = _npcs.game().itemInfo(h->melee);
	const i32 twoHanded = _npcs.game().constant("ITEM_2HD_SWD", 1 << 16) | _npcs.game().constant("ITEM_2HD_AXE", 1 << 17);
	return w && (w->flags & twoHanded) ? u8(WEAPON_2H) : u8(WEAPON_1H);
}

// Takes `mode` in hand (WEAPON_NONE: puts it away), as the player's key does;
// the others perceive it. False if the character cannot now.
bool App::heroTake(u8 mode) {
	const u8 before = _hero.weapon();
	if (!_hero.draw(mode)) return false;
	const int hi = heroIndex();
	const bool drawing = _hero.drawing() ? true : mode != WEAPON_NONE;
	const bool puttingAway = mode == WEAPON_NONE || (before != WEAPON_NONE && _hero.drawing() && before != mode);
	if (drawing && !puttingAway && mode != WEAPON_NONE) _lastWeapon = mode; // (fix166: the magic stance and the bow too, for X)
	if (hi >= 0) {
		// The scripts ask "is he in fight mode?" at once: the weapon is already reached for.
		if (!puttingAway) _npcs.game().npcMut(u32(hi)).weapon = _hero.weaponIntent();
		_npcs.sendPerception(puttingAway ? "PERC_ASSESSREMOVEWEAPON" : "PERC_DRAWWEAPON", u32(hi), hi, -1);
		// Whoever is about looks round now, rather than at its next turn: creatures too.
		if (!puttingAway) _npcs.noticeNow(u32(hi));
	}
	return true;
}

// A blow of the player's character lands: on the target if it is within
// reach in front, else on whoever is.
void App::heroBlow() {
	const int hi = heroIndex();
	if (hi < 0) return;
	Game& g = _npcs.game();
	const float feet[3] = {_player.feet.x, _player.feet.y, _player.feet.z};
	const float yaw = _hero.yaw();
	const auto reaches = [&](int t) {
		if (t < 0) return false;
		const GameNpc& n = g.npc(u32(t));
		const float dx = n.pos[0] - feet[0], dz = n.pos[2] - feet[2];
		const float d = sqrtf(dx * dx + dz * dz);
		return n.down == DOWN_NONE && fabsf(n.pos[1] - feet[1]) < 150.0f && d <= g.meleeRange(u32(hi), u32(t)) + 40.0f &&
		       (d < 1.0f || fabsf(wrapYaw(atan2f(dx, dz) - yaw)) < 1.2f);
	};
	int victim = reaches(_target) ? _target : -1;
	if (victim < 0) {
		const int t = _npcs.pickTarget(feet, yaw, 400.0f, 1.2f);
		if (reaches(t)) victim = t;
	}
	if (victim < 0) return;
	const GameNpc& v = g.npc(u32(victim));
	const float fromVictim = atan2f(feet[0] - v.pos[0], feet[2] - v.pos[2]);
	const bool behind = fabsf(wrapYaw(fromVictim - v.yaw)) > 1.5708f;
	const i32 dmg = _npcs.strike(u32(hi), u32(victim), behind);
	if (dmg >= 0) {
		++_fight.blowsLanded;
		_fight.lastDamage = dmg;
	} else {
		++_fight.parried;
	}
	if (!_locked) _target = victim;
}

void App::heroHitThunk(void* user, u32 attacker) { static_cast<App*>(user)->heroHit(attacker); }

// An inhabitant's blow reaches the player's character: fended off, or it
// hurts, and maybe it goes down (knocked out by one not hostile to it, dead
// otherwise - the rules are the same as for everybody, see Npcs::strike).
void App::heroHit(u32 attacker) {
	const int hi = heroIndex();
	if (hi < 0 || _hero.down()) return;
	Game& g = _npcs.game();
	GameNpc& h = g.npcMut(u32(hi));
	const GameNpc& a = g.npc(attacker);
	const float toAttacker = atan2f(a.pos[0] - _player.feet.x, a.pos[2] - _player.feet.z);
	const float off = fabsf(wrapYaw(toAttacker - _hero.yaw()));
	const bool front = off < 1.05f, fromBehind = off > 1.5708f;
	const bool beast = a.weapon == WEAPON_FIST && g.guildOf(a) >= g.constant("GIL_SEPERATOR_HUM", 16);
	// (An arrow or a spell is not parried.)
	if (!g.spellStriking() && ((_hero.parrying() && front && !beast) || (_hero.dodging() && front))) {
		(_hero.parrying() ? _fight.parried : _fight.dodged) += 1;
		if (_hero.parrying()) _npcs.emitParry(u32(hi));
		_npcs.sendPerception("PERC_ASSESSFIGHTSOUND", u32(hi), int(attacker), hi);
		return;
	}
	const i32 dmg = _settings.immortal ? 0 : g.meleeDamage(attacker, u32(hi));
	i32 hp = max(g.hitPoints(h) - dmg, i32(0));
	g.setAttribute(h, 0, hp);
	h.lastHit = int(attacker);
	++_fight.blowsTaken;
	_npcs.emitHit(attacker, u32(hi));
	if (dmg > 0) {
		bloodAt(u32(hi), attacker, dmg);
		_bloodPain = 0.45f;
	}
	if (hp > 1 && dmg > 0 && (_fight.blowsTaken & 1)) _npcs.emitSvm(u32(hi), "AARGH");
	_hero.hit(fromBehind);
	_npcs.sendPerception("PERC_ASSESSFIGHTSOUND", u32(hi), int(attacker), hi);
	if (hp <= 1 && g.attitude(u32(hi), attacker) != g.constant("ATT_HOSTILE", 0)) {
		g.setAttribute(h, 0, max(hp, i32(1)));
		_hero.fall(false, fromBehind);
		_downTime = float(g.constant("HAI_TIME_UNCONSCIOUS", 20));
		_npcs.goDown(u32(hi), int(attacker), false, fromBehind);
		_target = -1;
		_locked = false;
	} else if (hp <= 0) {
		_hero.fall(true, fromBehind);
		_deadTime = 0.001f;
		_npcs.goDown(u32(hi), int(attacker), true, fromBehind);
		_target = -1;
		_locked = false;
	}
}

// Falls hurt as OpenGothic's MoveAlgo/DamageCalculator have it, with the height the feet really dropped (the
// highest point since they left the ground to the one they land on): the scripts' table says from how high and
// how much per metre. Water, ladders, a ledge climbed, a slide down a slope and the unsticking of a body that got
// into the rock are no falls.
void App::fallCheck() {
	const Player::Debug dbg = _player.debug();
	const bool air = !_player.grounded && !_player.swimming && !_player.climbing && !_player.flying && !_hero.mantling() &&
	                 !_player.placedByAnimation() && !dbg.sliding && dbg.step != 'k' && dbg.step != 'p' && dbg.step != 'h' && dbg.step != '-';
	if (air) {
		_fallPeak = _fallAir ? max(_fallPeak, _player.feet.y) : _player.feet.y;
		_fallAir = true;
		return;
	}
	const bool landed = _fallAir && _player.grounded && !_player.swimming;
	const float drop = _fallPeak - _player.feet.y;
	_fallAir = false;
	if (landed && drop > 0.0f) heroLanded(drop);
}

void App::heroLanded(float drop) {
	const int hi = heroIndex();
	if (hi < 0 || _hero.down() || !_npcs.loaded()) return;
	Game& g = _npcs.game();
	const i32 dmg = _settings.immortal ? 0 : g.fallDamage(u32(hi), drop);
	if (dmg <= 0) return;
	GameNpc& h = g.npcMut(u32(hi));
	const i32 hp = max(g.hitPoints(h) - dmg, i32(0));
	g.setAttribute(h, 0, hp);
	++_fight.blowsTaken;
	_fight.lastDamage = dmg;
	if (hp > 0) {
		// Hurt, on his feet (OpenGothic: "SVM_%d_AARGH", the fall's stumble).
		_npcs.emitSvm(u32(hi), "AARGH");
		_hero.hit(false);
	} else {
		_hero.fall(true, false);
		_deadTime = 0.001f;
		_npcs.goDown(u32(hi), -1, true, false);
		_target = -1;
		_locked = false;
	}
}

// After death: back to the saved position (or the world's start), whole again.
void App::respawn(const char* statePath) {
	_hero.getUp();
	_deadTime = _downTime = 0;
	const int hi = heroIndex();
	if (hi >= 0) {
		GameNpc& h = _npcs.game().npcMut(u32(hi));
		h.down = DOWN_NONE;
		_npcs.game().setAttribute(h, 0, max(heroHitPointsMax(), i32(1)));
	}
	if (!(statePath && loadState(statePath))) {
		const WorldHeader& wh = _world.header();
		_cam.yaw = wh.spawnYaw;
		teleport({wh.spawnPos[0], wh.spawnPos[1], wh.spawnPos[2]});
	}
	toast(tr("Вы снова на ногах"));
}

// ---- settings and the menu ----

void App::applySettings() {
	_settings.sanitize();
	// (fix168) The chapter slider moved: the chapter is changed (the scripts' own change if they have it).
	if (_settings.chapter != 255 && _npcs.loaded()) {
		Game& g = _npcs.game();
		if (i32(_settings.chapter) != g.chapterNumber()) {
			const int how = g.debugSetChapter(i32(_settings.chapter));
			_invStamp = 0;
			if (how == 1) snprintf(_menuMessage, sizeof(_menuMessage), tr("Глава %d: скриптами игры"), int(_settings.chapter));
			else if (how == 2) snprintf(_menuMessage, sizeof(_menuMessage), tr("Глава %d: только номер (нет B_Kapitelwechsel)"), int(_settings.chapter));
			else snprintf(_menuMessage, sizeof(_menuMessage), "%s", tr("Нет героя или номера главы"));
		}
	}
	_world.config().farDistance = _settings.farCm(); // (before the fog, which follows it)
	_world.config().farFillNear = _settings.farFill != 0;
	_world.config().lodFadeMs = _settings.lodFadeMs();
	setDrawDistance(_settings.drawDistanceCm());
	StreamConfig& cfg = _world.config();
	cfg.detailDistance[DETAIL_MEDIUM] = _settings.mediumObjectsCm();
	cfg.detailDistance[DETAIL_SMALL] = _settings.smallObjectsCm();
	cfg.smallLayers = _settings.smallLayers();
	cfg.sizePixels = _settings.objectPixels();
	cfg.focalPx = float(gfx::SCREEN_H) * 0.5f / tanf(_cam.fovY * 0.5f);
	cfg.visibility = _settings.visibility != 0;
	cfg.lodDistance = _settings.lodDistanceCm();
	cfg.occlusion = _settings.occlusionStrength();
	_windWanted = _settings.windStrength();
	cfg.wind = _windRelief ? 0.0f : _windWanted;
	_wavesWanted = _settings.waveHeightCm();
	cfg.waves = _wavesWanted;
	cfg.sunMasks = _settings.sunShadows != 0;
	cfg.clouds = cfg.sunMasks && _settings.cloudShadows != 0 && !(_renderSkip & SKIP_CLOUDS);
	setLanguage(_settings.language);
	_ahead.setEnabled(_settings.readAhead != 0);
	_env.dayLength = _settings.dayLengthSeconds();
	_weather.configure(_settings.weather, _settings.wetGround != 0);
	NpcConfig& nc = _npcs.config();
	nc.range = _settings.npcRangeCm();
	nc.perceive = _settings.perception != 0;
	nc.alive = _settings.life != 0;
	nc.fights = _settings.fights != 0;
	_scheme = ControlScheme(_settings.scheme);
	_sound.setVolume(float(_settings.soundVolume) / 10.0f);
	_music.setVolume(float(_settings.musicVolume) / 10.0f);
	_speech.setVolume(float(_settings.voiceVolume) / 10.0f);
	_chatter.setVolume(float(_settings.voiceVolume) / 10.0f);
	const bool third = !_settings.firstPerson;
	if (third != _thirdPerson) {
		_thirdPerson = third;
		_camPlaced = false;
		_eyePitch = 0;
	}
}

// How dark the baked sun's shadow (g3ds-bake --sun-shadow) makes the ground now: full by day in clear weather while the
// sky's sun is near the one baked (within an hour and a half or so), fading out farther from it, under cloud and rain,
// and as the sun goes down. (The masks hold where the shade lies for that one sun: three hours off, it would lie wrong.)
float App::grassRadius() const {
	const float r = _settings.grassRadiusCm();
	if (r <= 0.0f) return 0.0f;
	// Under load the grass gives way first: as soon as the pacer takes objects away, the radius shrinks twice as fast as
	// their distance (to half at most), and to 60 % of that while Graphics+ is relieved.
	const float detail = clamp(_world.config().detailScale, 0.0f, 1.0f);
	return r * clamp(1.0f - 2.0f * (1.0f - detail), 0.5f, 1.0f) * (_gfxRelief ? 0.6f : 1.0f);
}

float App::sunShadowDarkness() const {
	if (!_settings.sunShadows || !_world.hasSunMasks() || _world.indoor()) return 0.0f;
	float sun[3];
	Sky::sunDirection(_env.timeOfDay, sun);
	const float* baked = _world.sunMaskDirection();
	const float agree = sun[0] * baked[0] + sun[1] * baked[1] + sun[2] * baked[2];
	auto smooth = [](float x) {
		x = clamp(x, 0.0f, 1.0f);
		return x * x * (3.0f - 2.0f * x);
	};
	// fix146: full while the sun is within about 25 degrees of the baked one (was 22), gone at about 57 (was 50); the strong
	// settings hold it a little longer and keep more of it under cloud.
	const u8 level = _settings.sunShadows > 3 ? u8(1) : _settings.sunShadows;
	const float wide = level >= 2 ? 0.05f : 0.0f;
	const float near = smooth((agree - (0.55f - wide)) / 0.35f);
	const float up = smooth((sun[1] - 0.05f) / 0.20f);
	const float cloudy = level >= 2 ? 0.75f : 0.9f;
	const float clear = (1.0f - cloudy * _weather.cloud()) * (1.0f - 0.6f * _weather.rain());
	return kSunShadowDark[level] * near * up * clamp(clear, 0.0f, 1.0f);
}

void App::setGfxRelief(bool heavy) {
	_gfxRelief = heavy;
}

// The plants stand still only when the pacer has halved the frame rate; fewer objects alone no longer stop them.
void App::setWindRelief(bool halved) {
	_windRelief = halved;
	_world.config().wind = halved ? 0.0f : _windWanted;
}

void App::weatherSound(float dt) {
	if (!_weatherSoundSearched && _sound.ready()) {
		_weatherSoundSearched = true;
		// Gothic 1 has no sound of rain or thunder of its own; these are names other builds and mods use, and the
		// baker's list (meta/weathersfx: "R name" / "T name") comes first. A spell's sound (MFX_FIRERAIN_RAIN,
		// MFX_THUNDERBALL_*: the only matches the baker finds in Gothic 1) is never taken for the weather.
		static const char* const kRain[] = {"RAIN_LOOP", "AMB_RAIN", "AMBIENT_RAIN", "RAIN", "REGEN_LOOP", "REGEN", "RAIN_01", "RAIN_A1", "WEATHER_RAIN"};
		static const char* const kThunder[] = {"THUNDER", "THUNDER_01", "THUNDER_A1", "THUNDER_1", "AMB_THUNDER", "DONNER", "THUNDER_LOUD", "WEATHER_THUNDER"};
		if (const PakEntry* e = _pak.find("meta/weathersfx")) {
			static char text[2048];
			if (e->size > 0 && e->size < sizeof(text) && _pak.read(*e, text)) {
				text[e->size] = 0;
				const char* rainAny = nullptr;
				const char* rainOwn = nullptr; // "L name": the baker's own loop, taken before all else
				for (char* p = text; *p;) {
					char* end = strchr(p, '\n');
					if (end) *end = 0;
					const char kind = p[0];
					const char* name = p + 2;
					if (p[1] == ' ' && *name && !weatherNameIsSpell(name) && _sound.knows(name)) {
						if ((kind == 'L' || kind == 'N' || kind == 'F') && !_ownWeatherSfx) {
							// (debug.txt noweathersfx: not taken)
						} else if (kind == 'L') {
							if (!rainOwn) rainOwn = name;
						} else if (kind == 'N') {
							if (_thunderNearCount < 4) _thunderNear[_thunderNearCount++] = name;
						} else if (kind == 'F') {
							if (_thunderFarCount < 4) _thunderFar[_thunderFarCount++] = name;
						} else if (kind == 'R') {
							if (_sound.isLoop(name) && !_rainName) _rainName = name;
							else if (!rainAny) rainAny = name;
						} else if (kind == 'T' && !_thunderName) {
							_thunderName = name;
						}
					}
					if (!end) break;
					p = end + 1;
				}
				if (rainOwn) _rainName = rainOwn;
				if (!_rainName) _rainName = rainAny;
				// (The baker's thunders come before a name of the game's.)
				if (_thunderNearCount) _thunderName = _thunderNear[0];
				else if (_thunderFarCount) _thunderName = _thunderFar[0];
			}
		}
		if (!_rainName)
			for (const char* n : kRain)
				if (_sound.knows(n) && (!_rainName || _sound.isLoop(n))) {
					_rainName = n;
					if (_sound.isLoop(n)) break;
				}
		if (!_thunderName)
			for (const char* n : kThunder)
				if (_sound.knows(n)) {
					_thunderName = n;
					break;
				}
		// None in the game: made here (engine/weather_sfx.h).
		if (!_rainName) {
			static i16 rain[RAIN_SFX_FRAMES];
			makeRainSound(rain);
			if (_sound.addSynthetic("G3_RAIN_MADE", rain, RAIN_SFX_FRAMES, RAIN_SFX_RATE, true)) _rainName = "G3_RAIN_MADE";
		}
		if (!_thunderName) {
			static i16 thunder[THUNDER_SFX_FRAMES];
			static const char* const kMade[] = {"G3_THUNDER_MADE_A", "G3_THUNDER_MADE_B"};
			for (u32 k = 0; k < 2; ++k) {
				makeThunderSound(thunder, 7u + k * 31u);
				if (_sound.addSynthetic(kMade[k], thunder, THUNDER_SFX_FRAMES, THUNDER_SFX_RATE, false)) {
					if (!_thunderName) _thunderName = kMade[k];
					_thunderMade = u8(k + 1);
				}
			}
		}
	}
	float delay = 0;
	if (_weather.takeThunder(delay)) {
		_thunderWait = delay;
		// (Sound covers about 340 m a second: a flash heard within 1.3 s is less than half a kilometre away.)
		_thunderClose = delay < 1.3f;
	}
	if (_thunderWait >= 0.0f) {
		_thunderWait -= dt;
		if (_thunderWait < 0.0f) {
			const char* name = _thunderName;
			// The made ones take turns.
			if (_thunderMade == 2 && (++_thunderTurn & 1u)) name = "G3_THUNDER_MADE_B";
			float gain = _world.indoor() ? 0.35f : 0.9f;
			if (_thunderNearCount || _thunderFarCount) {
				const bool useNear = _thunderNearCount && (_thunderClose || !_thunderFarCount);
				name = useNear ? _thunderNear[++_thunderTurn % _thunderNearCount] : _thunderFar[++_thunderTurn % _thunderFarCount];
				if (!useNear) gain *= 0.8f;
			}
			if (name) _sound.play(name, nullptr, gain);
			_thunderWait = -1.0f;
		}
	}
	// Snow falls silently; the rain is heard in proportion, muffled indoors.
	const float amount = _weather.rain() * (_world.indoor() ? 0.3f : 1.0f);
	if (!_rainName || amount < 0.02f) {
		if (_rainLoop) _sound.stop(_rainLoop);
		_rainLoop = 0;
		return;
	}
	const float gain = 0.15f + 0.65f * amount;
	_rainRetry -= dt;
	// playLoop's handle holds while the sample is read, so one request is all; again only if the loop was lost
	// (its voice taken by louder sounds, or its sample never came).
	if (_rainLoop && _sound.playing(_rainLoop)) {
		_sound.setHandleGain(_rainLoop, gain);
	} else if (_rainRetry <= 0.0f) {
		if (_rainLoop) _sound.stop(_rainLoop);
		_rainLoop = _sound.playLoop(_rainName, gain);
		_rainRetry = 1.0f;
	}
}

void App::toast(const char* text, float seconds) {
	snprintf(_toast, sizeof(_toast), "%s", text ? text : "");
	_toastTime = seconds;
}

bool App::menuAction(MenuAction action, const char* statePath) {
	_menuMessage[0] = 0;
	if (statePath) _statePath = statePath;
	switch (action) {
	case MENU_SAVE:
		snprintf(_menuMessage, sizeof(_menuMessage), "%s", statePath && saveState(statePath) ? tr("Позиция сохранена") : tr("Не удалось сохранить"));
		return true;
	case MENU_LOAD:
		if (statePath && loadState(statePath)) {
			_menu.close();
			toast(tr("Позиция загружена"));
		} else {
			snprintf(_menuMessage, sizeof(_menuMessage), "%s", tr("Сохранённой позиции нет"));
		}
		return true;
	case MENU_SAVE_SLOT: saveSlot(_menu.saveSlot(), nullptr); return true;
	case MENU_LOAD_SLOT: loadSlot(_menu.saveSlot()); return true;
	case MENU_EXIT: return false;
	case MENU_NEXT_WORLD: {
		u32 first = 0, next = 0;
		bool seenCurrent = false;
		for (u32 i = 0; i < _pak.count(); ++i) {
			const PakEntry& e = _pak.entry(i);
			if (e.type != ASSET_WORLD) continue;
			if (!first) first = e.hash;
			if (seenCurrent && !next) next = e.hash;
			if (e.hash == _world.entryHash()) seenCurrent = true;
		}
		enterWorld(next ? next : first, nullptr, 0);
		_menu.close();
		return true;
	}
	case MENU_HOUR:
		_env.timeOfDay += 1.0f;
		if (_env.timeOfDay >= 24.0f) _env.timeOfDay -= 24.0f;
		snprintf(_menuMessage, sizeof(_menuMessage), tr("Время %02d:%02d"), int(_env.timeOfDay), int(_env.timeOfDay * 60.0f) % 60);
		return true;
	case MENU_FLY:
		_player.flying = !_player.flying;
		if (!_player.flying) {
			_hero.reset(_player.feet, _cam.yaw);
			_camPlaced = false;
		}
		_menu.close();
		toast(_player.flying ? tr("Полёт: стик — вперёд, L/R — вниз/вверх") : tr("Полёт выключен"));
		return true;
	case MENU_HEAL: {
		const int hi = heroIndex();
		if (hi >= 0) _npcs.game().setAttribute(_npcs.game().npcMut(u32(hi)), 0, max(heroHitPointsMax(), i32(1)));
		snprintf(_menuMessage, sizeof(_menuMessage), "%s", tr("Здоровье восстановлено"));
		return true;
	}
	case MENU_GIVE_WEAPON: {
		const int hi = heroIndex();
		const u32 best = hi >= 0 ? _npcs.game().bestMeleeOfAll() : SCRIPT_NONE;
		if (best == SCRIPT_NONE) {
			snprintf(_menuMessage, sizeof(_menuMessage), "%s", tr("Оружия в скриптах нет"));
			return true;
		}
		_npcs.game().equip(u32(hi), best);
		const GameItemInfo* it = _npcs.game().itemInfo(best);
		snprintf(_menuMessage, sizeof(_menuMessage), "%.40s", it && it->name ? _npcs.game().text(it->name) : tr("Оружие выдано"));
		return true;
	}
	case MENU_RESET_GRAPHICS:
		_settings.resetGraphics();
		applySettings();
		snprintf(_menuMessage, sizeof(_menuMessage), "%s", tr("Графика сброшена"));
		return true;
	case MENU_GIVE_ITEMS: {
		const u32 n = giveItemKit();
		if (n) snprintf(_menuMessage, sizeof(_menuMessage), tr("Выдано: %lu видов"), static_cast<unsigned long>(n));
		else snprintf(_menuMessage, sizeof(_menuMessage), "%s", tr("Предметов в скриптах нет"));
		return true;
	}
	case MENU_LEVEL_UP: {
		const i32 level = _npcs.game().debugLevelUp();
		_invStamp = 0;
		if (level) snprintf(_menuMessage, sizeof(_menuMessage), tr("Уровень: %ld"), static_cast<long>(level));
		else snprintf(_menuMessage, sizeof(_menuMessage), "%s", tr("Нет героя"));
		return true;
	}
	case MENU_GIVE_SPELLKIT: {
		const u32 n = giveSpellKit();
		if (n) snprintf(_menuMessage, sizeof(_menuMessage), tr("Заклинания: %lu видов"), static_cast<unsigned long>(n));
		else snprintf(_menuMessage, sizeof(_menuMessage), "%s", tr("Предметов в скриптах нет"));
		return true;
	}
	case MENU_GIVE_CRAFTKIT: {
		const u32 n = giveCraftKit();
		if (n) snprintf(_menuMessage, sizeof(_menuMessage), tr("Для ремёсел: %lu видов"), static_cast<unsigned long>(n));
		else snprintf(_menuMessage, sizeof(_menuMessage), "%s", tr("Предметов в скриптах нет"));
		return true;
	}
	case MENU_LEARN_ALL: {
		const u32 n = _npcs.game().debugLearnAll();
		_invStamp = 0;
		if (n) snprintf(_menuMessage, sizeof(_menuMessage), tr("Изучено: %lu"), static_cast<unsigned long>(n));
		else snprintf(_menuMessage, sizeof(_menuMessage), "%s", tr("Нет героя"));
		return true;
	}
	case MENU_GIVE_TESTKIT: {
		const u32 n = giveTestKit();
		if (n) snprintf(_menuMessage, sizeof(_menuMessage), tr("Тест-набор: %lu видов"), static_cast<unsigned long>(n));
		else snprintf(_menuMessage, sizeof(_menuMessage), "%s", tr("Предметов в скриптах нет"));
		return true;
	}
	default: return true;
	}
}

// Where a point of the world is on the screen, in pixels; false if behind the eye.
bool App::projectToScreen(Vec3 p, int& x, int& y) const {
	const Vec4 c = transform(_viewProj, {p.x, p.y, p.z, 1.0f});
	if (c.w <= 1.0f) return false;
	x = int((c.x / c.w * 0.5f + 0.5f) * float(gfx::SCREEN_W));
	y = int((0.5f - c.y / c.w * 0.5f) * float(gfx::SCREEN_H));
	return x > -50 && x < gfx::SCREEN_W + 50 && y > -50 && y < gfx::SCREEN_H + 50;
}

} // namespace g3

// ---- sound ----

namespace g3 {

void App::soundEventThunk(void* user, const ClipSfxEvent& event, int npc) { static_cast<App*>(user)->soundEvent(event, npc); }

bool App::soundLocate(void* user, ScriptInstance* who, Vec3& pos) {
	App& app = *static_cast<App*>(user);
	if (!app._npcs.loaded()) return false;
	const int i = app._npcs.game().indexOf(who);
	if (i < 0) return false;
	const GameNpc& n = app._npcs.game().npc(u32(i));
	pos = {n.pos[0], n.pos[1], n.pos[2]};
	return true;
}

// Starts the sound engine on the scripts' machine and tells it of the world.
void App::triggerSoundThunk(void* user, const char* name, Vec3 at) {
	App& app = *static_cast<App*>(user);
	app._sound.play(name, &at, 1.0f, 3000.0f);
}

void App::startSound() {
	// (fix154) The hook carries the model script's item tags too, which have nothing to do with sound: set before the
	// sound can fail to start.
	{
		SoundEventHook early;
		early.fn = &App::soundEventThunk;
		early.user = this;
		_hero.setSoundHook(early);
		_npcs.setSoundHook(early);
	}
	if (!_audio || !_npcs.loaded()) return;
	// A few megabytes of samples at hand: the footsteps, the doors, the blows.
	// The names of the sounds are instances of the game's sound script; an older archive has none of its own
	// and they are looked for in the game's script.
	ScriptVm* sfxDefs = _sfxVm.load(_pak, "script/sfx") ? &_sfxVm : &_npcs.game().vm();
	if (!_sound.init(_pak, sfxDefs, _audio, 3u * 1024u * 1024u, true)) return;
	_sound.setVolume(float(_settings.soundVolume) / 10.0f);
	_sound.bindScript(_npcs.game().vm(), &App::soundLocate, this);
	_sound.enterWorld(_pak, worldName());
	// Music: music.pak beside data.pak, if the player has made one.
	{
		char path[320];
		snprintf(path, sizeof(path), "%s", _pak.path());
		char* slash = strrchr(path, '/');
		char* bslash = strrchr(path, '\\');
		if (bslash > slash) slash = bslash;
		snprintf(slash ? slash + 1 : path, sizeof(path) - (slash ? u32(slash + 1 - path) : 0), "music.pak");
		ScriptVm* musicDefs = _musicVm.load(_pak, "script/music") ? &_musicVm : &_npcs.game().vm();
		if (_music.init(path, musicDefs, _audio, true)) {
			_music.setVolume(float(_settings.musicVolume) / 10.0f);
			_music.enterWorld(_pak, worldName());
		}
	}
	SoundEventHook hook;
	hook.fn = &App::soundEventThunk;
	hook.user = this;
	_hero.setSoundHook(hook);
	_npcs.setSoundHook(hook);
	Npcs::NamedSoundHook named;
	named.fn = &App::namedSoundThunk;
	named.user = this;
	_npcs.setNamedSoundHook(named);
}

void App::namedSoundThunk(void* user, const char* names, int npc, float gain) { static_cast<App*>(user)->namedSound(names, npc, gain); }

void App::namedSound(const char* names, int npc, float gain) {
	if (!_sound.ready() || !names) return;
	Vec3 at = _player.feet;
	if (npc >= 0 && npc != heroIndex() && _npcs.loaded()) {
		const GameNpc& n = _npcs.game().npc(u32(npc));
		at = {n.pos[0], n.pos[1], n.pos[2]};
	}
	at.y += 100.0f; // (the height of the blow, the mouth)
	const float range = 2500.0f; // (as OpenGothic's)
	if (length(at - _cam.pos) > range * 1.2f) return;
	char name[48];
	for (const char* p = names; *p;) {
		const char* end = strchr(p, '|');
		const size_t n = end ? size_t(end - p) : strlen(p);
		const size_t k = n < sizeof(name) - 1 ? n : sizeof(name) - 1;
		memcpy(name, p, k);
		name[k] = 0;
		if (_sound.knows(name)) {
			_sound.play(name, &at, gain, range);
			return;
		}
		if (!end) break;
		p = end + 1;
	}
	// None is there: asking for the first notes it in the sound report (what the archive lacks).
	char first[48];
	const char* bar = strchr(names, '|');
	const size_t len = bar ? size_t(bar - names) : strlen(names);
	snprintf(first, sizeof(first), "%.*s", int(len < sizeof(first) - 1 ? len : sizeof(first) - 1), names);
	_sound.play(first, &at, gain, range);
}

// A footstep, a swish of the sword: an event of the model script's, at the
// moment the clip reaches its frame.
void App::soundEvent(const ClipSfxEvent& event, int npc) {
	// (fix154) The model script's item tags: the player's only; the inhabitants' items go by the scripts.
	if (event.kind >= CLIP_ITEM_CREATE) {
		if (npc < 0) itemEvent(event);
		return;
	}
	// The player's footsteps are heard (OpenGothic's tickAnimationTags): whoever is about and
	// listens (a sleeping creature, a guard) gets PERC_ASSESSQUIETSOUND - unless he creeps.
	if (npc < 0 && event.kind == CLIP_SFX_GROUND && !_hero.sneaking() && _npcs.loaded()) {
		const int hi = heroIndex();
		if (hi >= 0) _npcs.sendPerception("PERC_ASSESSQUIETSOUND", u32(hi), hi, hi);
	}
	if (!_sound.ready()) return;
	Vec3 at = _player.feet;
	if (npc >= 0) {
		if (!_npcs.loaded()) return;
		const GameNpc& n = _npcs.game().npc(u32(npc));
		at = {n.pos[0], n.pos[1], n.pos[2]};
	}
	float range = event.range > 0.0f ? event.range : 1000.0f;
	// fix191: a creature's growl is heard where its threat is made (the scripts' AIV_MM_PercRange, 12-30 m): OpenGothic gives a
	// tag without a range 35 m (Sound::Sound), and the threat turns a creature at 9-11 m, where a range of 10 m is silence.
	// Footsteps (ground tags) keep theirs.
	if (npc >= 0 && event.kind == CLIP_SFX_PLAIN) range = event.range > 0.0f && event.range > 2500.0f ? event.range : 2500.0f;
	// Out of earshot: not worth finding the ground for.
	if (length(at - _cam.pos) > range * 1.2f) return;
	if (event.kind == CLIP_SFX_GROUND) {
		u8 group = 0;
		if (npc < 0 && _player.swimming) {
			group = 5;
		} else {
			float y;
			Vec3 normal;
			if (!_world.rayDown({at.x, at.y + 40.0f, at.z}, 140.0f, y, normal, nullptr, &group)) group = 0;
		}
		_sound.playGround(event.name, group, &at, 1.0f, range);
	} else {
		_sound.play(event.name, &at, 1.0f, range);
	}
}

// Voices: speech.pak beside data.pak, if the player has made one.
void App::startSpeech() {
	if (!_audio) return;
	char path[320];
	snprintf(path, sizeof(path), "%s", _pak.path());
	char* slash = strrchr(path, '/');
	char* bslash = strrchr(path, '\\');
	if (bslash > slash) slash = bslash;
	snprintf(slash ? slash + 1 : path, sizeof(path) - (slash ? u32(slash + 1 - path) : 0), "speech.pak");
	if (!_speech.init(path, _audio)) return;
	_speech.setVolume(float(_settings.voiceVolume) / 10.0f);
	// (A second reader of the same archives for what the world says: it has a channel of its own.)
	if (_chatter.init(path, _audio)) _chatter.setVolume(float(_settings.voiceVolume) / 10.0f);
	Npcs::SpeechHook hook;
	hook.play = &App::speechPlayThunk;
	hook.stop = &App::speechStopThunk;
	hook.say = &App::chatterThunk;
	hook.user = this;
	_npcs.setSpeechHook(hook);
}

float App::speechPlayThunk(void* user, u32 lineHash, int speaker, bool hero) {
	return static_cast<App*>(user)->speechBegin(lineHash, speaker, hero);
}

void App::speechStopThunk(void* user) { static_cast<App*>(user)->_speech.stop(); }

// A line begins. The player's character is heard in full; another by its distance
// (a talk is close, a shout across the camp is not).
float App::speechBegin(u32 lineHash, int speaker, bool hero) {
	if (!_speech.ready()) return 0.0f;
	float gain = 1.0f;
	if (!hero && speaker >= 0 && _npcs.loaded()) {
		const GameNpc& n = _npcs.game().npc(u32(speaker));
		const float dx = n.pos[0] - _cam.pos.x, dy = n.pos[1] - _cam.pos.y, dz = n.pos[2] - _cam.pos.z;
		const float d = sqrtf(dx * dx + dy * dy + dz * dz);
		gain = d <= 1000.0f ? 1.0f : (d >= 3000.0f ? 0.0f : 1.0f - (d - 1000.0f) / 2000.0f);
	}
	return _speech.play(lineHash, gain);
}

float App::chatterThunk(void* user, u32 lineHash, int speaker, bool overlay) {
	return static_cast<App*>(user)->chatterBegin(lineHash, speaker, overlay);
}

// A line said in the world: from where its speaker stands, as loud as the distance allows (OpenGothic: the dialogue
// sound has a range of 20 m, heard within that and a little more), more on the side it comes from. Returns how
// long it is, 0 if it has no recording, -1 if the speaker is too far to hear.
float App::chatterBegin(u32 lineHash, int speaker, bool overlay) {
	(void)overlay;
	if (speaker < 0 || !_npcs.loaded()) return -1.0f;
	const GameNpc& n = _npcs.game().npc(u32(speaker));
	const float dx = n.pos[0] - _cam.pos.x, dy = n.pos[1] - _cam.pos.y, dz = n.pos[2] - _cam.pos.z;
	const float d = sqrtf(dx * dx + dy * dy + dz * dz);
	if (d > 2800.0f) return -1.0f;
	if (!_chatter.ready()) return 0.0f;
	const float gain = d <= 800.0f ? 1.0f : 1.0f - (d - 800.0f) / 2000.0f;
	const float flat = sqrtf(dx * dx + dz * dz);
	float pan = 0.0f;
	if (flat > 1.0f) pan = (dx * cosf(_cam.yaw) - dz * sinf(_cam.yaw)) / flat;
	pan *= clamp(flat / 250.0f, 0.0f, 1.0f);
	// (A line already sounding is not cut by another: this one is only measured, so its speaker still waits it out.)
	return _chatter.play(lineHash, gain, pan, false);
}

// The menu (and the screens between worlds) have a theme of their own, as in the original (the scripts'
// SYS_MENU / SYS_LOADING); without it in the scripts the world's goes on.
void App::soundIdle(float dt) {
	_sound.update(dt, _env.timeOfDay);
	_music.setSystemTheme("SYS_MENU");
	_music.update(dt, _cam.pos, _env.timeOfDay, MUSIC_CALM);
}

// OpenGothic (WorldSound::tickSoundZone): while something that is after the hero has a weapon drawn and attacks, the
// music is the threat's with nothing in the hero's hand and the fight's with a weapon drawn; the hero lying dead
// counts. Otherwise the calm one, whatever he holds. Held for a few seconds so that a pause in a fight is no end of it.
MusicSituation App::musicSituation(float dt) {
	_musicScan -= dt;
	if (_musicHold > 0.0f) _musicHold -= dt;
	if (_musicScan <= 0.0f) {
		_musicScan = 0.5f;
		if (_hero.loaded() && _hero.down()) _musicHold = 3.0f;
		else if (_npcs.loaded() && _npcs.targetsHero()) _musicHold = 3.0f;
		// The second stage begins when the enemy comes to attack (armed, in its fight state, within 6 m), not at the first blow.
		_musicAttack = _musicHold > 0.0f && _npcs.loaded() && _npcs.attacksHero(600.0f);
	}
	if (_musicHold <= 0.0f) return MUSIC_CALM;
	return _hero.weapon() == WEAPON_NONE && !_musicAttack ? MUSIC_THREAT : MUSIC_FIGHT;
}

} // namespace g3
