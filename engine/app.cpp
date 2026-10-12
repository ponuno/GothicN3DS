#include "lang.h"
#include "app.h"
#include "bytes.h"

#include "sys.h"

#include <stdlib.h>
#include <string.h>

namespace g3 {

bool App::init(const char* pakPath, const char* worldName) {
	if (!_pak.open(pakPath)) {
		_error = "cannot open data.pak";
		return false;
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
		_chests.enterWorld(_pak, this->worldName(), _world.entryHash(), _npcs.game());
	}
	HeroLink link;
	link.hit = heroHitThunk;
	link.user = this;
	_npcs.setHeroLink(link);
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
	_particles.leaveWorld();
	_chests.leaveWorld();
	_lootTarget = -1;
	_chestTarget = -1;
	_corpseTarget = -1;
	_world.close();
	gfx::releaseDestroyed();
	if (!_world.open(&_pak, worldEntryHash)) {
		// Broken target: go back to where we were rather than to nowhere.
		if (previous) _world.open(&_pak, previous);
		return false;
	}
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
	_npcs.enterWorld(_pak, _world, worldName(), _player.eye(), _env.timeOfDay);
	if (_npcs.loaded()) {
		_loot.enterWorld(_pak, worldName(), _world.entryHash(), _npcs.game());
		_particles.enterWorld(_pak, worldName(), _npcs.game());
		_chests.enterWorld(_pak, worldName(), _world.entryHash(), _npcs.game());
	}
	_portalLock = true;
	++_switches;
	_sound.stopAll();
	_sound.enterWorld(_pak, worldName());
	_barrierSearched = false;
	_barrierName = nullptr;
	_barrierHum = 0;
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
	if (!_world.isOpen()) return false;
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
	FILE* f = fopen(path, "wb");
	if (!f) return false;
	const bool ok = fwrite(&s, sizeof(s), 1, f) == 1;
	saveInventory(f);
	_loot.saveTaken(f);
	_chests.saveChanged(f);
	bool game = true;
	if (_npcs.loaded()) {
		ByteSink payload;
		_npcs.game().save(payload);
		const u32 head[3] = {GAME_SECTION, u32(payload.size), byteCheck(payload.data, payload.size)};
		game = !payload.failed && fwrite(head, sizeof(head), 1, f) == 1 && (payload.size == 0 || fwrite(payload.data, payload.size, 1, f) == 1);
	}
	return (fclose(f) == 0) && ok && game;
}

bool App::loadState(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) return false;
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
	if (s.world != _world.entryHash()) {
		const u32 switches = _switches;
		if (!enterWorld(s.world, &eye, s.yaw)) {
			fclose(f);
			return false;
		}
		_switches = switches; // continuing is not travelling
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
		if (s.version >= 3) loadGameSection(f);
	}
	fclose(f);
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

void App::shutdown() {
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
	_chests.release();
	_pictures.release();
	_showModel.release();
	_npcs.release();
	_hero.release();
	_sky.release();
	_world.close();
	_pak.close();
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

void App::update(const Input& in, float dt) {
	collisionStatsReset(); // (what collisionStats() says is one frame's worth)
	_clock += dt;
	if (dt > 0.0f) _fps += (clamp(1.0f / dt, 0.0f, 120.0f) - _fps) * min(dt * 3.0f, 1.0f);
	_env.timeOfDay += in.timeStep;
	if (_env.dayLength > 0) _env.timeOfDay += dt * 24.0f / _env.dayLength;
	while (_env.timeOfDay >= 24.0f) _env.timeOfDay -= 24.0f;
	while (_env.timeOfDay < 0.0f) _env.timeOfDay += 24.0f;

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
	const double t0 = sys::timeMs();
	_world.update(_player.eye());
	const double t1 = sys::timeMs();
	_npcs.update(_pak, _world, _player.eye(), _env.timeOfDay, dt);
	const double tLoot0 = sys::timeMs();
	if (_npcs.loaded()) _loot.update(_pak, _world, _npcs.game(), _player.eye());
	const double tLoot1 = sys::timeMs();
	_particles.update(dt, _player.eye());
	const double t2 = sys::timeMs();

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
		itemsUpdate(fightIn, dt); // (a job holds the hands: its frames take the fight keys)
		talkUpdate(fightIn, dt);
		fightInput(fightIn, dt);
		Input heroIn = steerInput(in, dt);
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
			if (!_player.probeLedge(_world, _hero.yaw(), ground)) {
				const float yaw = _hero.yaw(), reach = _player.config.radius + 45.0f;
				const Vec3 dir = {sinf(yaw), 0, cosf(yaw)};
				for (int k = 0; k < 2; ++k) {
					const Vec3 a = {_player.feet.x, _player.feet.y + (k ? 160.0f : 100.0f), _player.feet.z};
					wall = wall || !_world.lineFree(a, a + dir * reach, 10.0f);
				}
			}
		}
		const Drive drive = _hero.control(heroIn, _player, dt, grab ? &ledge : nullptr, ground.found ? &ground : nullptr, wall);
		_player.update(_world, heroIn, _hero.yaw(), 0.0f, dt, &drive);
		const SkyColors sky = skyColors();
		const float indoorLight[3] = {1, 1, 1};
		_hero.follow(_player, _world, _world.indoor() ? indoorLight : sky.light, dt);
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
	_moves.record(_world, _player, in, _hero.loaded() ? "GTAS"[_hero.body()] : '?', dt);
	checkPortals();
	const double t3 = sys::timeMs();
	placeCamera(dt);
	_sound.setListener(_cam.pos, _cam.yaw);
	_sound.update(dt, _env.timeOfDay);
	barrierHum();
	_music.update(dt, _cam.pos, _env.timeOfDay, heroInCombat() ? MUSIC_FIGHT : MUSIC_CALM);
	_times.stream = float(t1 - t0);
	_times.npcs = float(t2 - t1);
	_times.script = _npcs.stats().scriptMs;
	_times.loot = float(tLoot1 - tLoot0);
	_times.player = float(t3 - t2);
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
	const int steps = max(int(range / 22.0f) + 1, 2);
	for (int s = 1; s <= steps; ++s) {
		const float d = range * float(s) / float(steps);
		Vec3 probe = from + dir * d;
		if (world.pushSphere(probe, radius)) return range * float(s - 1) / float(steps);
	}
	return range;
}

// The dialogue shot of the original: the camera hangs behind the character's
// shoulder (a little to the right and up) and looks at the head of the one it
// talks to; the one who speaks gets the frame (the looked-at point moves towards
// them). `look` and `pos` are set when a talk is on; the way from the
// character's head to the camera is kept free of walls.
bool App::talkCamera(Vec3& look, Vec3& pos) {
	if (!_npcs.loaded() || !_npcs.talkView().active || !thirdPerson()) return false;
	const int partner = _npcs.talkPartner();
	if (partner < 0) return false;
	const GameNpc& n = _npcs.game().npc(u32(partner));
	const TalkView& tv = _npcs.talkView();
	const float height = max(_hero.hipHeight() * 1.72f, 120.0f); // (eyes above the feet)
	const Vec3 eye = {_player.feet.x, _player.feet.y + height, _player.feet.z};
	const Vec3 head = {n.pos[0], n.pos[1] + height, n.pos[2]};
	Vec3 axis = {head.x - eye.x, 0.0f, head.z - eye.z};
	const float dist = sqrtf(axis.x * axis.x + axis.z * axis.z);
	if (dist < 20.0f) return false;
	axis = axis * (1.0f / dist);
	const Vec3 right = {axis.z, 0.0f, -axis.x};
	// Whose turn it is: the camera moves a little with it (the partner speaking: the
	// shot is on them; the character: it takes both).
	const bool partnerSpeaks = tv.saying && !tv.heroSays;
	const float share = partnerSpeaks ? 0.88f : tv.saying ? 0.55f : 0.72f;
	look = eye + (head - eye) * share;
	look.y = eye.y - 4.0f + (partnerSpeaks ? 6.0f : 0.0f);
	const float back = clamp(100.0f + dist * 0.12f, 100.0f, 150.0f);
	Vec3 want = eye - axis * back + right * (partnerSpeaks ? 42.0f : 52.0f);
	want.y = eye.y + 12.0f;
	// Not through a wall: the way from the head to the place is free as far as it goes.
	Vec3 d = want - eye;
	const float len = length(d);
	if (len > 1.0f) {
		d = d * (1.0f / len);
		const float room = cameraRoom(_world, eye, d, len, _cam.boomRadius);
		want = eye + d * min(room, len);
	}
	pos = want;
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
	while (lag > 3.14159265f) lag -= 6.2831853f;
	while (lag < -3.14159265f) lag += 6.2831853f;
	if (orbit) lag = _steerLag;
	if (talkCam) lag = 0.0f;
	_hero.setTurnLag(flat < 4.0f ? 0.0f : lag);
	// With the camera inside the character there is nothing to see of it but its inside.
	_heroVisible = length(look) > 45.0f;
}

void App::render() {
	const SkyColors sky = skyColors();

	gfx::FrameDesc fd {};
	fd.view = viewLH(_cam.pos, _cam.yaw, _cam.pitch);
	fd.eye = _cam.pos;
	fd.fovY = _cam.fovY;
	fd.zNear = _cam.zNear;
	fd.zFar = _cam.zFar;
	fd.fogStart = _env.fogStart;
	fd.fogEnd = _env.fogEnd;
	const bool indoor = _world.indoor();
	for (int i = 0; i < 3; ++i) {
		fd.fogColor[i] = fd.clearColor[i] = indoor ? _world.header().fogColor[i] : sky.horizon[i];
		fd.dayLight[i] = sky.light[i];
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
	gfx::frameBegin(fd);
	if (!indoor) {
		gfx::setPass(gfx::PASS_SKY);
		_sky.draw(_cam.pos, _cam.zFar * 0.5f, sky, _settings.barrier ? 1.0f : 0.0f, float(_clock));
	}
	gfx::setPass(gfx::PASS_WORLD);
	const Mat4 viewProj = mul(proj, fd.view);
	_viewProj = viewProj;
	_world.draw(viewProj, _cam.pos, u32(u64(_clock * 1000.0)));
	const float indoorLight[3] = {1, 1, 1};
	_npcs.draw(viewProj, indoor ? indoorLight : sky.light);
	_loot.draw(_cam.pos, indoor ? indoorLight : sky.light);
	drawShowcase();
	if (_heroVisible) {
		_hero.draw();
		drawArrows(_hero.light());
		const GameNpc* hn = heroNpc();
		if (hn && _hero.loaded())
			_loot.drawCarried(_pak, _npcs.game(), _hero.model(), _hero.pose(), _hero.yaw(), _hero.lean(), nullptr, _hero.weapon(),
			                  hn->melee, _hero.feet(), _hero.light());
		if (hn && _hero.loaded())
			_loot.drawRanged(_pak, _npcs.game(), _hero.model(), _hero.pose(), _hero.yaw(), _hero.lean(), nullptr, _hero.weapon(),
			                 hn->ranged, _hero.feet(), _hero.light());
	}
	gfx::setPass(gfx::PASS_BLEND);
	// Graphics+: shadows on the ground, before the water (which then covers those under it).
	_shadows.begin();
	if (_settings.shadows != SHADOWS_OFF) {
		if (_heroVisible && _hero.loaded()) _shadows.add(_hero.feet(), 48.0f);
		if (_settings.shadows == SHADOWS_ALL && !_gfxRelief)
			for (u32 i = 0; i < _npcs.drawnCount(); ++i) _shadows.add(_npcs.drawnFeet(i), _npcs.drawnRadius(i));
		_shadows.draw(_world, _cam.pos, indoor ? 0.7f : 0.6f);
	}
	_world.drawBlended();
	_particles.draw(fd.view, _cam.pos, indoor ? indoorLight : sky.light);
	drawOverlay();
	gfx::frameEnd();
}

// ---- controls ----

namespace {

float wrapYaw(float a) {
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
		h.lookX = in.lookX * (0.5f + 0.1f * float(_settings.cameraSpeed));
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
void App::updateTarget(const Input& in, float dt) {
	if (!_npcs.loaded() || _hero.down()) {
		_target = -1;
		_locked = false;
		return;
	}
	// The weapon put away: the lock goes with it.
	if (_heldBefore != WEAPON_NONE && _hero.weapon() == WEAPON_NONE) _locked = false;
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
		if (_hero.weapon() != WEAPON_NONE) take(WEAPON_NONE);
		else take(_lastWeapon == WEAPON_FIST || _lastWeapon == WEAPON_NONE ? meleeMode() : _lastWeapon);
	}
	if (in.meleeSlot) {
		const u8 want = meleeMode();
		take(_hero.weapon() == want ? u8(WEAPON_NONE) : want);
	}
	// (On the console D-Pad up takes the fists when the hands are empty; with a weapon it is the blow ahead.)
	if (in.fistSlot || (in.dpadUp && (_hero.weapon() == WEAPON_NONE || _hero.weapon() == WEAPON_FIST)))
		take(_hero.weapon() == WEAPON_FIST ? u8(WEAPON_NONE) : u8(WEAPON_FIST));
	_attackHeld = in.attackHeld;
	magicInput(in);
	rangedInput(in);
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
	if (drawing && !puttingAway && mode != WEAPON_NONE && mode != WEAPON_MAG) _lastWeapon = mode;
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
	if ((_hero.parrying() && front && !beast) || (_hero.dodging() && front)) {
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
	_world.config().farDistance = _settings.farCm(); // (before the fog, which follows it)
	setDrawDistance(_settings.drawDistanceCm());
	StreamConfig& cfg = _world.config();
	cfg.detailDistance[DETAIL_MEDIUM] = _settings.mediumObjectsCm();
	cfg.detailDistance[DETAIL_SMALL] = _settings.smallObjectsCm();
	cfg.smallLayers = _settings.smallLayers();
	cfg.visibility = _settings.visibility != 0;
	cfg.lodDistance = _settings.lodDistanceCm();
	cfg.occlusion = _settings.occlusionStrength();
	_windWanted = _settings.windStrength();
	cfg.wind = _gfxRelief ? 0.0f : _windWanted;
	setLanguage(_settings.language);
	_env.dayLength = _settings.dayLengthSeconds();
	NpcConfig& nc = _npcs.config();
	nc.range = _settings.npcRangeCm();
	nc.perceive = _settings.perception != 0;
	nc.alive = _settings.life != 0;
	nc.fights = _settings.fights != 0;
	_scheme = ControlScheme(_settings.scheme);
	_sound.setVolume(float(_settings.soundVolume) / 10.0f);
	_music.setVolume(float(_settings.musicVolume) / 10.0f);
	_speech.setVolume(float(_settings.voiceVolume) / 10.0f);
	const bool third = !_settings.firstPerson;
	if (third != _thirdPerson) {
		_thirdPerson = third;
		_camPlaced = false;
		_eyePitch = 0;
	}
}

void App::setGfxRelief(bool heavy) {
	_gfxRelief = heavy;
	_world.config().wind = heavy ? 0.0f : _windWanted;
}

// The names the Barrier's sound could have in the scripts (the first the sound script knows is used).
void App::barrierHum() {
	if (!_barrierSearched && _sound.ready()) {
		_barrierSearched = true;
		static const char* const kNames[] = {"MFX_BARRIERE_AMBIENT", "MFX_BARRIER_AMBIENT", "BARRIERE_AMBIENT", "BARRIER_AMBIENT", "MFX_BARRIERE_LOOP",
		                                      "BARRIERE_LOOP"};
		for (const char* n : kNames)
			if (_sound.knows(n)) {
				_barrierName = n;
				break;
			}
	}
	const bool want = _barrierName && _settings.barrier && !_world.indoor();
	if (!want) {
		if (_barrierHum) _sound.stop(_barrierHum);
		_barrierHum = 0;
		return;
	}
	if (!_barrierHum || !_sound.playing(_barrierHum)) _barrierHum = _sound.play(_barrierName, nullptr, 0.4f);
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
void App::startSound() {
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
	const float range = event.range > 0.0f ? event.range : 1000.0f;
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
	Npcs::SpeechHook hook;
	hook.play = &App::speechPlayThunk;
	hook.stop = &App::speechStopThunk;
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

void App::soundIdle(float dt) {
	_sound.update(dt, _env.timeOfDay);
	_music.update(dt, _cam.pos, _env.timeOfDay, MUSIC_CALM);
}

} // namespace g3
