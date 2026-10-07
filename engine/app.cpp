#include "app.h"

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
	// Optional: archives without a character stay first-person. With one, the
	// player moves as fast as its run animation carries it, so the feet do
	// not slide (within reason: a broken clip must not cripple the game).
	_player.config = PlayerConfig {};
	if (_hero.load(_pak)) {
		const float run = _hero.roleSpeed(Hero::RUN), swim = _hero.roleSpeed(Hero::SWIM);
		if (run >= 300.0f && run <= 750.0f) _player.config.walkSpeed = run;
		if (swim >= 120.0f && swim <= 450.0f) _player.config.swimSpeed = swim;
	}
	_boom = _cam.distance;
	const WorldHeader& h = _world.header();
	_cam.yaw = h.spawnYaw;
	_cam.pitch = 0;
	teleport({h.spawnPos[0], h.spawnPos[1], h.spawnPos[2]});
	setDrawDistance(_world.config().drawDistance);
	_clock = 0;
	_switches = 0;
	_portalLock = false;
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
}

bool App::enterWorld(u32 worldEntryHash, const Vec3* eye, float yaw) {
	const u32 previous = _world.entryHash();
	const PakEntry* e = _pak.find(worldEntryHash);
	if (!e || e->type != ASSET_WORLD || worldEntryHash == previous) return false;
	const float drawDistance = _world.config().drawDistance;
	// Give the old world's memory back before the new one asks for its own.
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
	s.yaw = _cam.yaw;
	s.pitch = _cam.pitch;
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
	teleport(eye);
	_cam.yaw = s.yaw;
	_cam.pitch = clamp(s.pitch, -1.5f, 1.5f);
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
	const float turn = 1.8f * dt; // rad/s at full deflection
	_cam.yaw += in.lookX * turn;
	_cam.pitch = clamp(_cam.pitch + in.lookY * turn, -1.5f, 1.5f);
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

	if (in.toggleCamera) _thirdPerson = !_thirdPerson;

	// Stream first, so the chunks the player is about to stand on are there.
	// Around the player, not the camera: the camera may be behind a wall.
	_world.update(_player.eye());
	_player.update(_world, in, _cam.yaw, _cam.pitch, dt);
	checkPortals();
	if (_hero.loaded()) {
		const SkyColors sky = skyColors();
		const float indoorLight[3] = {1, 1, 1};
		_hero.update(_player, _world, _world.indoor() ? indoorLight : sky.light, dt);
	}
	placeCamera(dt);
}

// First person: at the player's eyes. Third person: on a boom behind the
// character that shortens when something is in the way.
void App::placeCamera(float dt) {
	const Vec3 eye = _player.eye();
	_heroVisible = false;
	if (!thirdPerson() || _player.flying) {
		_cam.pos = eye;
		_boom = 0;
		return;
	}
	const Vec3 pivot = {_player.feet.x, _player.feet.y + _cam.pivotHeight, _player.feet.z};
	const Vec3 back = forwardFromYawPitch(_cam.yaw, _cam.pitch) * -1.0f;
	// How far back is there room? March a small sphere along the boom.
	float room = _cam.distance;
	const int steps = 14;
	for (int s = 1; s <= steps; ++s) {
		const float d = _cam.distance * float(s) / float(steps);
		Vec3 probe = pivot + back * d;
		if (_world.pushSphere(probe, _cam.boomRadius)) {
			room = _cam.distance * float(s - 1) / float(steps);
			break;
		}
	}
	// In at once (never show the inside of a wall), out gently.
	_boom = room < _boom ? room : _boom + (room - _boom) * min(dt * 4.0f, 1.0f);
	_cam.pos = pivot + back * _boom;
	// Pressed against the character there is nothing to see of it but its
	// inside: with no room behind, the view is (nearly) its own and it is
	// left out. The camera stays on the boom, so nothing jumps when room
	// comes back.
	_heroVisible = _boom > 70.0f;
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
	_world.draw(mul(proj, fd.view), _cam.pos, u32(u64(_clock * 1000.0)));
	if (_heroVisible) _hero.draw();
	gfx::setPass(gfx::PASS_BLEND);
	_world.drawBlended();
	gfx::frameEnd();
}

} // namespace g3
