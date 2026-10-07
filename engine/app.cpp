#include "app.h"

namespace g3 {

bool App::init(const char* pakPath, const char* worldName) {
	if (!_pak.open(pakPath)) {
		_error = "cannot open data.pak";
		return false;
	}
	if (!_world.open(&_pak, worldName)) {
		_error = "world not found in data.pak (or baked by an older g3ds-bake)";
		return false;
	}
	const WorldHeader& h = _world.header();
	_cam.yaw = h.spawnYaw;
	_cam.pitch = 0;
	teleport({h.spawnPos[0], h.spawnPos[1], h.spawnPos[2]});
	setDrawDistance(_world.config().drawDistance);
	return true;
}

void App::teleport(Vec3 eye) {
	const bool flying = _player.flying;
	_player.spawn(eye);
	_player.flying = flying;
	_cam.pos = eye;
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
	_sky.release();
	_world.close();
	_pak.close();
}

void App::update(const Input& in, float dt) {
	const float turn = 1.8f * dt; // rad/s at full deflection
	_cam.yaw += in.lookX * turn;
	_cam.pitch = clamp(_cam.pitch + in.lookY * turn, -1.5f, 1.5f);

	_env.timeOfDay += in.timeStep;
	if (_env.dayLength > 0) _env.timeOfDay += dt * 24.0f / _env.dayLength;
	while (_env.timeOfDay >= 24.0f) _env.timeOfDay -= 24.0f;
	while (_env.timeOfDay < 0.0f) _env.timeOfDay += 24.0f;

	// Stream first, so the chunks the player is about to stand on are there.
	_world.update(_cam.pos);
	_player.update(_world, in, _cam.yaw, _cam.pitch, dt);
	_cam.pos = _player.eye();
}

void App::render() {
	const SkyColors sky = skyColors();

	gfx::FrameDesc fd {};
	fd.view = viewLH(_cam.pos, _cam.yaw, _cam.pitch);
	fd.fovY = _cam.fovY;
	fd.zNear = _cam.zNear;
	fd.zFar = _cam.zFar;
	fd.fogStart = _env.fogStart;
	fd.fogEnd = _env.fogEnd;
	for (int i = 0; i < 3; ++i) {
		fd.fogColor[i] = fd.clearColor[i] = sky.horizon[i];
		fd.dayLight[i] = sky.light[i];
	}

	const Mat4 proj = perspectiveLH(_cam.fovY, float(gfx::SCREEN_W) / float(gfx::SCREEN_H), _cam.zNear, _cam.zFar);
	gfx::frameBegin(fd);
	gfx::setPass(gfx::PASS_SKY);
	_sky.draw(_cam.pos, _cam.zFar * 0.5f, sky);
	gfx::setPass(gfx::PASS_WORLD);
	_world.draw(mul(proj, fd.view), _cam.pos);
	gfx::frameEnd();
}

} // namespace g3
