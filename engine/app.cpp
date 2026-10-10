#include "app.h"

#include "sys.h"

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
	if (_npcs.load(_pak)) _npcs.enterWorld(_pak, _world, this->worldName(), _player.eye(), _env.timeOfDay);
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
	_camPlaced = false;
}

bool App::enterWorld(u32 worldEntryHash, const Vec3* eye, float yaw) {
	const u32 previous = _world.entryHash();
	const PakEntry* e = _pak.find(worldEntryHash);
	if (!e || e->type != ASSET_WORLD || worldEntryHash == previous) return false;
	const float drawDistance = _world.config().drawDistance;
	// Give the old world's memory back before the new one asks for its own.
	_npcs.leaveWorld();
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
	_portalLock = true;
	++_switches;
	return true;
}

namespace {

constexpr u32 STATE_MAGIC = fourcc('G', '3', 'S', 'T');
constexpr u32 STATE_VERSION = 1;

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
	return (fclose(f) == 0) && ok;
}

bool App::loadState(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	SavedState s {};
	const bool read = fread(&s, sizeof(s), 1, f) == 1;
	fclose(f);
	if (!read || s.magic != STATE_MAGIC || s.version != STATE_VERSION || s.check != stateCheck(s)) return false;
	bool sane = finite(s.yaw) && finite(s.pitch) && s.timeOfDay >= 0.0f && s.timeOfDay < 24.0f &&
	            s.drawDistance >= 1000.0f && s.drawDistance <= 100000.0f;
	for (float v : s.eye) sane = sane && finite(v);
	if (!sane) return false;

	const Vec3 eye = {s.eye[0], s.eye[1], s.eye[2]};
	if (s.world != _world.entryHash()) {
		const u32 switches = _switches;
		if (!enterWorld(s.world, &eye, s.yaw)) return false;
		_switches = switches; // continuing is not travelling
	}
	_player.flying = s.flying != 0;
	_cam.yaw = s.yaw;
	_cam.pitch = clamp(s.pitch, -1.5f, 1.5f);
	teleport(eye); // (the character takes its heading from the camera's)
	_env.timeOfDay = s.timeOfDay;
	setDrawDistance(s.drawDistance);
	// Standing in a portal's box when the game was left must not send the
	// player through it on arrival.
	_portalLock = true;
	return true;
}

void App::setDrawDistance(float cm) {
	StreamConfig& cfg = _world.config();
	cfg.drawDistance = cm;
	cfg.loadRadius = cm + 2000.0f;   // have chunks ready before they become visible
	cfg.unloadRadius = cm + 6000.0f; // hysteresis against load/unload thrash
	_env.fogEnd = cm;
	_env.fogStart = cm * 0.2f;
	// Chunk bounds are tested, not triangles: leave room for a whole chunk.
	_cam.zFar = cm + _world.header().chunkSize * 1.5f;
}

void App::shutdown() {
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
	const double t2 = sys::timeMs();

	const bool wasFlying = _player.flying;
	const bool steered = _hero.loaded() && !wasFlying && !in.toggleFly;
	const float turn = 1.8f * dt; // rad/s at full deflection
	if (steered) {
		// The character is steered; the view follows it (placeCamera).
		// Looking up and down tilts the view, not the character.
		if (thirdPerson())
			_camElevation = clamp(_camElevation - in.lookY * 60.0f * dt, -45.0f, 60.0f);
		else
			_eyePitch = clamp(_eyePitch + in.lookY * turn, -1.4f, 1.4f);
		const Drive drive = _hero.control(in, _player, dt);
		_player.update(_world, in, _hero.yaw(), 0.0f, dt, &drive);
		const SkyColors sky = skyColors();
		const float indoorLight[3] = {1, 1, 1};
		_hero.follow(_player, _world, _world.indoor() ? indoorLight : sky.light, dt);
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
	_times.stream = float(t1 - t0);
	_times.npcs = float(t2 - t1);
	_times.script = _npcs.stats().scriptMs;
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
	const Vec3 want = Vec3 {feet.x, feet.y + _hero.hipHeight(), feet.z} + offset;
	if (!_camPlaced) _camTarget = want;
	else _camTarget = _camTarget + (want - _camTarget) * (1.0f - expf(-_cam.targetPace * dt));
	// The leash has an end: the character cannot leave the picture.
	{
		const Vec3 d = _camTarget - want;
		const float len = length(d), most = 200.0f;
		if (len > most) _camTarget = want + d * (most / len);
	}

	// Where the camera belongs: at the script's range, up by its elevation,
	// behind the character (turned by the script's azimuth).
	float elevation = clamp(def.bestElevation + _camElevation, -30.0f, 85.0f) * rad;
	const float azimuth = heroYaw + def.bestAzimuth * rad;
	auto backwards = [&](float elev) {
		const float ce = cosf(elev);
		return Vec3 {-sinf(azimuth) * ce, sinf(elev), -cosf(azimuth) * ce};
	};
	Vec3 dir = backwards(elevation);
	float range = def.bestRange * 100.0f;
	float room = cameraRoom(_world, _camTarget, dir, range, _cam.boomRadius);
	if (room < _cam.topDownBelow) {
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
	const Vec3 dest = _camTarget + dir * room;
	if (!_camPlaced) _camOrigin = dest;
	else _camOrigin = _camOrigin + (dest - _camOrigin) * (1.0f - expf(-0.25f * def.veloTrans * dt));
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
	_cam.yaw = lookYaw - def.rotOffsetY * rad;
	// The scripts were written for a 4:3 picture. This one is wider and shows
	// less above and below; the view is tipped down by what is missing, so
	// that the lower edge - the character's feet - shows what it shows there.
	const float aspect = float(gfx::SCREEN_W) / float(gfx::SCREEN_H);
	const float wide = atanf(tanf(_cam.fovY * 0.5f) * aspect * 0.75f) - _cam.fovY * 0.5f;
	_cam.pitch = clamp(atan2f(look.y, flat) + def.rotOffsetX * rad - max(wide, 0.0f), -1.5f, 1.5f);
	// How far the view hangs behind the character's turn.
	float lag = heroYaw + def.bestAzimuth * rad - lookYaw;
	while (lag > 3.14159265f) lag -= 6.2831853f;
	while (lag < -3.14159265f) lag += 6.2831853f;
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
		_sky.draw(_cam.pos, _cam.zFar * 0.5f, sky);
	}
	gfx::setPass(gfx::PASS_WORLD);
	const Mat4 viewProj = mul(proj, fd.view);
	_world.draw(viewProj, _cam.pos, u32(u64(_clock * 1000.0)));
	const float indoorLight[3] = {1, 1, 1};
	_npcs.draw(viewProj, indoor ? indoorLight : sky.light);
	if (_heroVisible) _hero.draw();
	gfx::setPass(gfx::PASS_BLEND);
	_world.drawBlended();
	gfx::frameEnd();
}

} // namespace g3
