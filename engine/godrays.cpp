#include "godrays.h"

#include "world.h"

#include <math.h>
#include <string.h>

namespace g3 {

namespace {

constexpr float kPosScale = 0.25f;
constexpr int kLevels = 3;

unsigned mix(unsigned h) {
	h = (h ^ (h >> 15)) * 2246822519u;
	h = (h ^ (h >> 13)) * 3266489917u;
	return h ^ (h >> 16);
}
float unit(unsigned h) { return float(h & 0xFFFFu) * (1.0f / 65535.0f); }

} // namespace

void GodRays::release() {
	for (gfx::Mesh*& m : _meshes) {
		if (m) gfx::meshDestroy(m);
		m = nullptr;
	}
	if (_texture) gfx::textureDestroy(_texture);
	_texture = nullptr;
	_drawn = 0;
}

void GodRays::draw(const World& world, Vec3 camera, const float sun[3], float strength, const float tint[3], double seconds) {
	_drawn = 0;
	if (!(strength > 0.03f) || sun[1] < 0.03f) return;
	if (!_texture) {
		constexpr int W = 16, H = 32;
		static u8 cov[W * H];
		for (int y = 0; y < H; ++y)
			for (int x = 0; x < W; ++x) {
				const float u = (float(x) + 0.5f) / float(W), v = (float(y) + 0.5f) / float(H);
				const float across = 1.0f - fabsf(2.0f * u - 1.0f);
				const float along = sinf(3.14159265f * v);
				cov[y * W + x] = u8(clamp(across * across * sqrtf(along), 0.0f, 1.0f) * 255.0f);
			}
		_texture = gfx::uiTextureCreate(W, H, cov, true);
		if (!_texture) return;
	}
	if (!_meshes[0]) {
		static u16 indices[MAX * 6];
		for (u32 q = 0; q < MAX; ++q) {
			const u16 b = u16(q * 4);
			const u16 quad[6] = {b, u16(b + 1), u16(b + 2), u16(b + 2), u16(b + 1), u16(b + 3)};
			memcpy(indices + q * 6, quad, sizeof(quad));
		}
		static WorldVertex blank[MAX * 4];
		for (gfx::Mesh*& m : _meshes) {
			m = gfx::meshCreate(blank, MAX * 4, indices, MAX * 6);
			if (!m) {
				release();
				return;
			}
		}
	}
	// The way to the sun along the ground.
	float hx = sun[0], hz = sun[2];
	const float hl = sqrtf(hx * hx + hz * hz);
	if (hl < 0.05f) return; // (the sun overhead: no side to look to)
	hx /= hl;
	hz /= hl;
	const float px = -hz, pz = hx; // across it
	const float origin[3] = {floorf(camera.x), floorf(camera.y), floorf(camera.z)};
	static WorldVertex rings[kLevels][MAX * 4];
	u32 count[kLevels] = {};
	const float sunLen = 4500.0f;
	for (u32 i = 0; i < MAX; ++i) {
		const unsigned h = mix(i * 2654435761u + 31u);
		const float a = (float(i) / float(MAX - 1) - 0.5f) * 1.7f + (unit(h) - 0.5f) * 0.12f; // radians off the sun's way
		const float dist = 900.0f + 2600.0f * unit(h >> 7);
		const float ca = cosf(a), sa = sinf(a);
		const float dx = hx * ca + px * sa, dz = hz * ca + pz * sa;
		const float x = camera.x + dx * dist, z = camera.z + dz * dist;
		float ground;
		Vec3 n;
		if (!world.rayDown({x, camera.y + 1500.0f, z}, 4500.0f, ground, n) || n.y < 0.5f) continue;
		const Vec3 base = {x, ground + 20.0f, z};
		const Vec3 toSun = {sun[0], sun[1], sun[2]};
		const Vec3 eyeUp = {0, 140.0f, 0};
		if (world.sightBlocked(base + eyeUp, base + toSun * sunLen)) continue;
		// The places beside it: in shade (the beam stands at the edge of a shadow).
		int shaded = 0;
		for (int side = -1; side <= 1; side += 2) {
			const Vec3 o = {x + px * 260.0f * float(side), ground + 160.0f, z + pz * 260.0f * float(side)};
			if (world.sightBlocked(o, o + toSun * sunLen)) ++shaded;
		}
		const float shimmer = 0.85f + 0.15f * sinf(float(seconds) * 0.7f + float(i) * 1.9f);
		const float level = beamLevel(true, shaded) * strength * shimmer;
		const int lv = int(level * float(kLevels) + 0.5f);
		if (lv <= 0) continue;
		const int li = min(lv, kLevels) - 1;
		if (count[li] >= MAX) continue;
		// A quad along the sun's direction, turned across the view.
		const float len = 1500.0f + 1100.0f * unit(h >> 3);
		const Vec3 top = base + toSun * len;
		float vx = base.x - camera.x, vy = base.y - camera.y, vz = base.z - camera.z;
		// right = axis x view
		float rx = sun[1] * vz - sun[2] * vy, ry = sun[2] * vx - sun[0] * vz, rz = sun[0] * vy - sun[1] * vx;
		const float rl = sqrtf(rx * rx + ry * ry + rz * rz);
		if (rl < 1e-3f) continue;
		rx /= rl; ry /= rl; rz /= rl;
		const float w0 = 70.0f + 80.0f * unit(h >> 11), w1 = w0 * 1.9f;
		const u8 col[3] = {u8(clamp(tint[0], 0.0f, 255.0f)), u8(clamp(tint[1], 0.0f, 255.0f)), u8(clamp(tint[2], 0.0f, 255.0f))};
		WorldVertex* o = rings[li] + count[li] * 4;
		const Vec3 pts[4] = {{base.x - rx * w0, base.y - ry * w0, base.z - rz * w0}, {base.x + rx * w0, base.y + ry * w0, base.z + rz * w0},
		                     {top.x - rx * w1, top.y - ry * w1, top.z - rz * w1}, {top.x + rx * w1, top.y + ry * w1, top.z + rz * w1}};
		for (int k = 0; k < 4; ++k) {
			o[k].x = i16(clamp((pts[k].x - origin[0]) / kPosScale, -32000.0f, 32000.0f));
			o[k].y = i16(clamp((pts[k].y - origin[1]) / kPosScale, -32000.0f, 32000.0f));
			o[k].z = i16(clamp((pts[k].z - origin[2]) / kPosScale, -32000.0f, 32000.0f));
			o[k].w = 0;
			o[k].u = i16((k & 1) ? 1024 : 0);
			o[k].v = i16(k < 2 ? 1024 : 0);
			o[k].r = col[0]; o[k].g = col[1]; o[k].b = col[2];
			o[k].a = 0;
		}
		++count[li];
	}
	static WorldVertex verts[MAX * 4];
	u32 at = 0, first[kLevels];
	for (int l = 0; l < kLevels; ++l) {
		first[l] = at;
		memcpy(verts + at * 4, rings[l], count[l] * 4 * sizeof(WorldVertex));
		at += count[l];
	}
	if (at == 0) return;
	gfx::Mesh* mesh = _meshes[_turn];
	_turn = (_turn + 1) % MESHES;
	gfx::meshUpdate(mesh, verts, at * 4);
	gfx::setChunkTransform(origin, kPosScale, 1.0f / 1024.0f);
	for (int l = 0; l < kLevels; ++l) {
		if (!count[l]) continue;
		gfx::setMaterial(_texture, MATF_ADD, 0.16f * float(l + 1));
		gfx::draw(mesh, first[l] * 6, count[l] * 6);
	}
	_drawn = at;
}

} // namespace g3
