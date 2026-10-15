#include "ripples.h"

#include "world.h"

#include <math.h>
#include <string.h>

namespace g3 {

namespace {
constexpr float kPosScale = 0.25f;
constexpr int kLevels = 4;
constexpr float kMinDepth = 8.0f; // cm of water over the feet from which there is a ripple
} // namespace

void Ripples::release() {
	for (gfx::Mesh*& m : _meshes) {
		if (m) gfx::meshDestroy(m);
		m = nullptr;
	}
	if (_texture) gfx::textureDestroy(_texture);
	_texture = nullptr;
	for (Ring& r : _ring) r = Ring {};
	_count = 0;
	_haveLast = _wasIn = false;
	_timer = 0.0f;
}

void Ripples::spawn(const World& world, float x, float z, float surfaceY, float size, float strength, float life) {
	u32 slot = MAX_RINGS;
	for (u32 i = 0; i < _count; ++i)
		if (_ring[i].age >= _ring[i].life) {
			slot = i;
			break;
		}
	if (slot == MAX_RINGS) {
		if (_count < MAX_RINGS) slot = _count++;
		else { // the oldest goes
			slot = 0;
			for (u32 i = 1; i < _count; ++i)
				if (_ring[i].age / _ring[i].life > _ring[slot].age / _ring[slot].life) slot = i;
		}
	}
	Ring r;
	r.x = x;
	r.z = z;
	r.y = surfaceY;
	r.size = size;
	r.strength = strength;
	r.life = life;
	float y = surfaceY, w = 0.0f;
	if (world.waterBelow(Vec3 {x, surfaceY + 60.0f, z}, 260.0f, y, &w)) {
		r.y = y;
		r.weight = w;
	}
	_ring[slot] = r;
}

void Ripples::update(const World& world, Vec3 feet, float waterDepth, bool swimming, float dt) {
	if (!(dt > 0.0f)) return;
	for (u32 i = 0; i < _count; ++i) _ring[i].age += dt;
	while (_count > 0 && _ring[_count - 1].age >= _ring[_count - 1].life) --_count;
	const bool in = waterDepth > kMinDepth;
	float speed = 0.0f;
	if (_haveLast) {
		const float dx = feet.x - _last.x, dz = feet.z - _last.z;
		speed = sqrtf(dx * dx + dz * dz) / dt;
		if (speed > 3000.0f) speed = 0.0f; // (a jump across the map)
	}
	_last = feet;
	_haveLast = true;
	if (!in) {
		_wasIn = false;
		_timer = 0.0f;
		return;
	}
	const float surface = feet.y + waterDepth;
	if (!_wasIn) { // stepping in: two wide rings
		_wasIn = true;
		spawn(world, feet.x, feet.z, surface, 1.0f, 0.95f, 1.9f);
		spawn(world, feet.x, feet.z, surface, 0.55f, 0.8f, 1.4f);
		_timer = 0.25f;
		return;
	}
	_timer -= dt;
	if (_timer > 0.0f) return;
	const float move = clamp(speed / 480.0f, 0.0f, 1.0f);
	if (swimming || move > 0.08f) {
		const float m = swimming ? max(move, 0.5f) : move;
		spawn(world, feet.x, feet.z, surface, 0.35f + 0.5f * m, 0.55f + 0.35f * m, 1.5f);
		_timer = 0.42f - 0.26f * m;
	} else { // standing: a faint slow ring now and then
		spawn(world, feet.x, feet.z, surface, 0.2f, 0.4f, 1.8f);
		_timer = 1.3f;
	}
}

void Ripples::draw(const World& world, Vec3 camera, const float skyLight[3]) {
	if (_count == 0) return;
	constexpr int kSize = 32;
	if (!_texture) {
		static u8 coverage[kSize * kSize];
		const float c = float(kSize - 1) * 0.5f;
		for (int y = 0; y < kSize; ++y)
			for (int x = 0; x < kSize; ++x) {
				const float dx = (float(x) - c) / c, dy = (float(y) - c) / c;
				const float r = sqrtf(dx * dx + dy * dy);
				const float a = clamp(1.0f - fabsf(r - 0.78f) / 0.2f, 0.0f, 1.0f);
				coverage[y * kSize + x] = u8(a * a * 255.0f + 0.5f);
			}
		_texture = gfx::uiTextureCreate(kSize, kSize, coverage, true);
		if (!_texture) return;
	}
	if (!_meshes[0]) {
		static u16 indices[MAX_RINGS * 6];
		for (u32 q = 0; q < MAX_RINGS; ++q) {
			const u16 b = u16(q * 4);
			const u16 quad[6] = {b, u16(b + 1), u16(b + 2), u16(b + 2), u16(b + 1), u16(b + 3)};
			memcpy(indices + q * 6, quad, sizeof(quad));
		}
		static WorldVertex blank[MAX_RINGS * 4];
		for (gfx::Mesh*& m : _meshes) {
			m = gfx::meshCreate(blank, MAX_RINGS * 4, indices, MAX_RINGS * 6);
			if (!m) {
				release();
				return;
			}
		}
	}
	const float light = clamp((skyLight[0] + skyLight[1] + skyLight[2]) * (1.0f / 3.0f), 0.0f, 1.0f);
	const float bright = 0.30f + 0.70f * light;
	const u8 colour[3] = {u8(clamp(205.0f * bright + 30.0f, 0.0f, 255.0f)), u8(clamp(220.0f * bright + 30.0f, 0.0f, 255.0f)), u8(clamp(238.0f * bright + 20.0f, 0.0f, 255.0f))};
	const float originXYZ[3] = {floorf(camera.x), floorf(camera.y), floorf(camera.z)};
	static WorldVertex levels[kLevels][MAX_RINGS * 4];
	u32 count[kLevels] = {};
	auto put = [&](WorldVertex& o, float x, float y, float z, int u, int v) {
		o.x = i16(clamp((x - originXYZ[0]) / kPosScale, -32000.0f, 32000.0f));
		o.y = i16(clamp((y - originXYZ[1]) / kPosScale, -32000.0f, 32000.0f));
		o.z = i16(clamp((z - originXYZ[2]) / kPosScale, -32000.0f, 32000.0f));
		o.w = 0;
		o.u = i16(u);
		o.v = i16(v);
		o.r = colour[0]; o.g = colour[1]; o.b = colour[2];
		o.a = 0;
	};
	u32 total = 0;
	for (u32 i = 0; i < _count; ++i) {
		const Ring& g = _ring[i];
		if (g.age >= g.life) continue;
		const float age = g.age / g.life;
		const int lv = int(opacity(age, g.strength) * float(kLevels) + 0.5f);
		if (lv <= 0) continue;
		const int level = min(lv, kLevels) - 1;
		const float r = radius(age, g.size);
		float lift = 0.0f, sx = 0.0f, sz = 0.0f;
		if (g.weight > 0.0f) gfx::waveSurfaceAt(g.x, g.z, world.waveSeconds(), world.waveHeight(), g.weight, lift, sx, sz);
		const float dx = g.x - camera.x, dz = g.z - camera.z;
		const float yc = g.y + 2.0f + sqrtf(dx * dx + dz * dz) * 0.003f + lift;
		static const float corner[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
		WorldVertex* o = levels[level] + count[level] * 4;
		for (int k = 0; k < 4; ++k) {
			const float ox = corner[k][0] * r, oz = corner[k][1] * r;
			put(o[k], g.x + ox, yc + sx * ox + sz * oz, g.z + oz, corner[k][0] < 0 ? 0 : 1024, corner[k][1] < 0 ? 0 : 1024);
		}
		++count[level];
		++total;
	}
	if (total == 0) return;
	static WorldVertex verts[MAX_RINGS * 4];
	u32 at = 0, first[kLevels];
	for (int l = 0; l < kLevels; ++l) {
		first[l] = at;
		memcpy(verts + at * 4, levels[l], count[l] * 4 * sizeof(WorldVertex));
		at += count[l];
	}
	gfx::Mesh* mesh = _meshes[_meshTurn];
	_meshTurn = (_meshTurn + 1) % MESHES;
	gfx::meshUpdate(mesh, verts, at * 4);
	gfx::setChunkTransform(originXYZ, kPosScale, 1.0f / 1024.0f);
	for (int l = 0; l < kLevels; ++l) {
		if (!count[l]) continue;
		gfx::setMaterial(_texture, MATF_BLEND, 0.9f * float(l + 1) / float(kLevels));
		gfx::draw(mesh, first[l] * 6, count[l] * 6);
	}
}

} // namespace g3
