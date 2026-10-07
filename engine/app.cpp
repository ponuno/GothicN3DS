#include "app.h"

namespace g3 {

bool App::init(const char* pakPath, const char* worldName) {
	if (!_pak.open(pakPath)) {
		_error = "cannot open data.pak";
		return false;
	}
	if (!_world.open(&_pak, worldName)) {
		_error = "world not found in data.pak";
		return false;
	}
	const WorldHeader& h = _world.header();
	_cam.pos = {h.spawnPos[0], h.spawnPos[1], h.spawnPos[2]};
	_cam.yaw = h.spawnYaw;
	setDrawDistance(_world.config().drawDistance);
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
	_world.close();
	_pak.close();
}

void App::update(const Input& in, float dt) {
	const float turn = 1.8f * dt; // rad/s at full deflection
	_cam.yaw += in.lookX * turn;
	_cam.pitch = clamp(_cam.pitch + in.lookY * turn, -1.5f, 1.5f);

	const float speed = (in.fast ? 4000.0f : 900.0f) * dt; // cm/s
	const Vec3 f = forwardFromYawPitch(_cam.yaw, _cam.pitch);
	const Vec3 r = normalize(cross({0, 1, 0}, f));
	_cam.pos = _cam.pos + f * (in.moveZ * speed) + r * (in.moveX * speed) + Vec3 {0, in.moveY * speed, 0};

	_world.update(_cam.pos);
}

void App::render() {
	gfx::FrameDesc fd {};
	fd.view = viewLH(_cam.pos, _cam.yaw, _cam.pitch);
	fd.fovY = _cam.fovY;
	fd.zNear = _cam.zNear;
	fd.zFar = _cam.zFar;
	fd.fogStart = _env.fogStart;
	fd.fogEnd = _env.fogEnd;
	for (int i = 0; i < 3; ++i) fd.fogColor[i] = fd.clearColor[i] = _env.fogColor[i];

	const Mat4 proj = perspectiveLH(_cam.fovY, float(gfx::SCREEN_W) / float(gfx::SCREEN_H), _cam.zNear, _cam.zFar);
	gfx::frameBegin(fd);
	_world.draw(mul(proj, fd.view), _cam.pos);
	gfx::frameEnd();
}

} // namespace g3
