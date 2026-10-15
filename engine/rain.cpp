#include "rain.h"

#include "world.h"

#include <math.h>
#include <string.h>

namespace g3 {

namespace {

constexpr float kPosScale = 0.25f;
constexpr int kRingLevels = 4;
constexpr float kRainSpeed = 2300.0f; // cm/s (real drops fall at 9 m/s, but on a small screen that reads as a drizzle)
constexpr float kSnowSpeed = 90.0f;

float smooth(float x) {
	x = clamp(x, 0.0f, 1.0f);
	return x * x * (3.0f - 2.0f * x);
}
unsigned mix(unsigned h) {
	h = (h ^ (h >> 15)) * 2246822519u;
	h = (h ^ (h >> 13)) * 3266489917u;
	return h ^ (h >> 16);
}
float unit(unsigned h) { return float(h & 0xFFFFu) * (1.0f / 65535.0f); }

} // namespace

bool Rain::cellDrop(int ix, int iz, float amount, float& x, float& z, float& phase, float& speed) {
	const unsigned h = mix(unsigned(ix) * 73856093u ^ unsigned(iz) * 19349663u ^ 0x7F4A7C15u);
	if (unit(h) >= amount * 0.55f) return false;
	x = (float(ix) + unit(mix(h + 1u))) * CELL;
	z = (float(iz) + unit(mix(h + 2u))) * CELL;
	phase = unit(mix(h + 3u));
	speed = 0.9f + 0.2f * unit(mix(h + 4u));
	return true;
}

void Rain::release() {
	for (gfx::Mesh*& m : _meshes) {
		if (m) gfx::meshDestroy(m);
		m = nullptr;
	}
	if (_texture) gfx::textureDestroy(_texture);
	_texture = nullptr;
	for (Cell& c : _cells) c = Cell {};
	_drops = _rings = 0;
}

void Rain::draw(const World& world, Vec3 camera, float rain, float snow, const float skyLight[3], double seconds) {
	_drops = _rings = 0;
	const float amount = max(rain, snow);
	if (!(amount > 0.03f)) return;
	constexpr int kSize = 32;
	if (!_texture) {
		static u8 coverage[kSize * kSize];
		const float c = float(kSize - 1) * 0.5f;
		for (int y = 0; y < kSize; ++y)
			for (int x = 0; x < kSize; ++x) {
				const float dx = (float(x) - c) / c, dy = (float(y) - c) / c;
				const float r = sqrtf(dx * dx + dy * dy);
				const float a = clamp(1.0f - fabsf(r - 0.74f) / 0.18f, 0.0f, 1.0f);
				coverage[y * kSize + x] = u8(a * a * 255.0f + 0.5f);
			}
		_texture = gfx::uiTextureCreate(kSize, kSize, coverage, true);
		if (!_texture) return;
	}
	constexpr u32 QUADS = MAX_DROPS + MAX_RINGS;
	if (!_meshes[0]) {
		static u16 indices[QUADS * 6];
		for (u32 q = 0; q < QUADS; ++q) {
			const u16 b = u16(q * 4);
			const u16 quad[6] = {b, u16(b + 1), u16(b + 2), u16(b + 2), u16(b + 1), u16(b + 3)};
			memcpy(indices + q * 6, quad, sizeof(quad));
		}
		static WorldVertex blank[QUADS * 4];
		for (gfx::Mesh*& m : _meshes) {
			m = gfx::meshCreate(blank, QUADS * 4, indices, QUADS * 6);
			if (!m) {
				release();
				return;
			}
		}
	}
	const float light = clamp((skyLight[0] + skyLight[1] + skyLight[2]) * (1.0f / 3.0f), 0.0f, 1.0f);
	const float bright = 0.30f + 0.70f * light;
	const bool snowing = snow > rain;
	const float originXYZ[3] = {floorf(camera.x), floorf(camera.y), floorf(camera.z)};
	const float top = camera.y + 800.0f;

	static WorldVertex drops[MAX_DROPS * 4];
	static WorldVertex rings[kRingLevels][MAX_RINGS * 4];
	u32 ringCount[kRingLevels] = {};
	u32 nDrops = 0, nRings = 0;
	auto put = [&](WorldVertex& o, float x, float y, float z, int u, int v, const u8 c[3]) {
		o.x = i16(clamp((x - originXYZ[0]) / kPosScale, -32000.0f, 32000.0f));
		o.y = i16(clamp((y - originXYZ[1]) / kPosScale, -32000.0f, 32000.0f));
		o.z = i16(clamp((z - originXYZ[2]) / kPosScale, -32000.0f, 32000.0f));
		o.w = 0;
		o.u = i16(u);
		o.v = i16(v);
		o.r = c[0]; o.g = c[1]; o.b = c[2];
		o.a = 0;
	};
	const u8 dropColour[3] = {u8(clamp((snowing ? 245.0f : 185.0f) * bright, 0.0f, 255.0f)), u8(clamp((snowing ? 248.0f : 200.0f) * bright, 0.0f, 255.0f)),
	                          u8(clamp((snowing ? 255.0f : 222.0f) * bright, 0.0f, 255.0f))};
	const u8 ringColour[3] = {u8(clamp(200.0f * bright + 30.0f, 0.0f, 255.0f)), u8(clamp(215.0f * bright + 30.0f, 0.0f, 255.0f)), u8(clamp(235.0f * bright + 20.0f, 0.0f, 255.0f))};

	const int cx = int(floorf(camera.x / CELL)), cz = int(floorf(camera.z / CELL));
	const int reach = int(RADIUS / CELL) + 1;
	int budget = 28;
	const float sec = float(seconds);
	for (int dz = -reach; dz <= reach; ++dz)
		for (int dx = -reach; dx <= reach; ++dx) {
			const int ix = cx + dx, iz = cz + dz;
			float x, z, phase, speedK;
			if (!cellDrop(ix, iz, amount, x, z, phase, speedK)) continue;
			const float ddx = x - camera.x, ddz = z - camera.z;
			const float dist = sqrtf(ddx * ddx + ddz * ddz);
			if (dist > RADIUS) continue;
			Cell& c = _cells[(unsigned(iz) & (GRID - 1)) * GRID + (unsigned(ix) & (GRID - 1))];
			if (c.ix != ix || c.iz != iz) {
				c = Cell {};
				c.ix = ix;
				c.iz = iz;
			}
			if (c.state == 0 || (c.state == 1 && sec > c.retryAt)) {
				if (budget <= 0) continue;
				--budget;
				c.state = 1;
				c.retryAt = sec + 3.0f;
				c.x = x;
				c.z = z;
				// What is under it: the highest of the ground (roofs and all) and the water.
				const float from = camera.y + 2500.0f;
				float g = 0, wy = 0, wt = 0;
				Vec3 n {0, 1, 0};
				const bool onGround = world.rayDown({x, from, z}, 7000.0f, g, n);
				const bool onWater = world.waterBelow({x, from, z}, 7000.0f, wy, &wt);
				if (onWater && (!onGround || wy >= g - 1.0f)) {
					c.state = 2; c.water = 1; c.y = wy; c.weight = wt; c.normal = Vec3 {0, 1, 0};
				} else if (onGround) {
					c.state = 2; c.water = 0; c.y = g; c.normal = n.y > 0.2f ? n : Vec3 {0, 1, 0};
				}
			}
			if (c.state != 2) continue;
			const float fade = 1.0f - smooth((dist - RADIUS * 0.75f) / (RADIUS * 0.25f));
			const float H = clamp(top - c.y, 0.0f, 2400.0f);
			if (H < 120.0f) continue; // (a roof right over the head: nothing falls on it where it can be seen)

			// The drop: a quad standing in the world, turned to the camera, as long as it falls in a few frames.
			if (nDrops < MAX_DROPS) {
				const float speed = (snowing ? kSnowSpeed : kRainSpeed) * speedK;
				// Drops fall through a world-fixed lattice of heights (one cycle of CYCLE cm), so neither the camera's own
				// height nor the ground's under a drop changes how fast it falls.
				constexpr float CYCLE = 2400.0f;
				const float a = phase - sec * speed / CYCLE;
				const float u = top / CYCLE - (a - floorf(a));
				const float y = top - CYCLE * (u - floorf(u));
				float px = x, pz = z;
				if (snowing) {
					px += sinf(sec * 0.9f + phase * 40.0f) * 22.0f;
					pz += cosf(sec * 0.7f + phase * 17.0f) * 22.0f;
				}
				float rx = pz - camera.z, rz = -(px - camera.x); // horizontal, across the view direction
				const float rl = sqrtf(rx * rx + rz * rz);
				if (rl > 1.0f) {
					rx /= rl;
					rz /= rl;
					const float len = snowing ? 3.0f + dist * 0.003f : 170.0f + 60.0f * phase; // (the way it falls in about a frame at 30 fps: a streak)
					const float half = (snowing ? 1.8f : 0.7f) + dist * (snowing ? 0.0018f : 0.0020f);
					const float yTop = y + (snowing ? len : len * 0.5f), yBot = max(y - (snowing ? 0.0f : len * 0.5f), c.y);
					if (yTop > c.y + 1.0f) {
						WorldVertex* o = drops + nDrops * 4;
						put(o[0], px - rx * half, yTop, pz - rz * half, 0, 0, dropColour);
						put(o[1], px + rx * half, yTop, pz + rz * half, 0, 0, dropColour);
						put(o[2], px - rx * half, yBot, pz - rz * half, 0, 0, dropColour);
						put(o[3], px + rx * half, yBot, pz + rz * half, 0, 0, dropColour);
						++nDrops;
					}
				}
			}
			// The splash: on the water a ring spreading over its waves, on the ground a small short one (not in snow).
			if (snowing || nRings >= MAX_RINGS) continue;
			const unsigned hc = mix(unsigned(ix) * 2654435761u ^ unsigned(iz) * 40503u);
			const float period = c.water ? 0.9f + 0.9f * unit(hc) : 0.45f + 0.35f * unit(hc);
			float u = sec / period + unit(hc >> 9);
			const unsigned turn = unsigned(floorf(u));
			u -= floorf(u);
			const unsigned hh = mix(hc + turn * 7919u);
			if (unit(hh) > 0.7f * clamp(rain * 1.3f, 0.0f, 1.0f)) continue; // (not every turn has one)
			const float life = c.water ? 1.0f : 0.5f;
			if (u > life) continue;
			const float age = u / life;
			const float ox = (unit(hh >> 3) - 0.5f) * CELL * 0.6f, oz = (unit(hh >> 11) - 0.5f) * CELL * 0.6f;
			const float rx0 = x + ox, rz0 = z + oz;
			const float r = c.water ? (10.0f + 60.0f * age) * (1.0f + dist * 0.0004f) : (3.0f + 17.0f * age) * (1.0f + dist * 0.0006f);
			const float alpha = rain * fade * (1.0f - age) * (1.0f - age) * (c.water ? 1.0f : 0.8f);
			const int lv = int(alpha * float(kRingLevels) + 0.5f);
			if (lv <= 0) continue;
			const int level = min(lv, kRingLevels) - 1;
			if (ringCount[level] >= MAX_RINGS) continue;
			// The plane the ring lies in: the water's, with the waves' height and slope there; or the ground's.
			float yc = c.y + 2.0f + dist * 0.003f, sx = 0.0f, sz = 0.0f;
			if (c.water) {
				float lift, gx, gz;
				gfx::waveSurfaceAt(rx0, rz0, world.waveSeconds(), world.waveHeight(), c.weight, lift, gx, gz);
				yc += lift;
				sx = gx;
				sz = gz;
			} else {
				sx = -c.normal.x / c.normal.y;
				sz = -c.normal.z / c.normal.y;
			}
			static const float corner[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
			WorldVertex* o = rings[level] + ringCount[level] * 4;
			for (int k = 0; k < 4; ++k) {
				const float ddx2 = corner[k][0] * r, ddz2 = corner[k][1] * r;
				put(o[k], rx0 + ddx2, yc + sx * ddx2 + sz * ddz2, rz0 + ddz2, corner[k][0] < 0 ? 0 : 1024, corner[k][1] < 0 ? 0 : 1024, ringColour);
			}
			++ringCount[level];
			++nRings;
		}
	_drops = nDrops;
	_rings = nRings;
	if (nDrops == 0 && nRings == 0) return;

	static WorldVertex verts[QUADS * 4];
	memcpy(verts, drops, nDrops * 4 * sizeof(WorldVertex));
	u32 at = nDrops, first[kRingLevels];
	for (int l = 0; l < kRingLevels; ++l) {
		first[l] = at;
		memcpy(verts + at * 4, rings[l], ringCount[l] * 4 * sizeof(WorldVertex));
		at += ringCount[l];
	}
	gfx::Mesh* mesh = _meshes[_meshTurn];
	_meshTurn = (_meshTurn + 1) % MESHES;
	gfx::meshUpdate(mesh, verts, at * 4);
	gfx::setChunkTransform(originXYZ, kPosScale, 1.0f / 1024.0f);
	if (nDrops) {
		gfx::setMaterial(nullptr, MATF_BLEND, snowing ? 0.85f : 0.5f);
		gfx::draw(mesh, 0, nDrops * 6);
	}
	for (int l = 0; l < kRingLevels; ++l) {
		if (!ringCount[l]) continue;
		gfx::setMaterial(_texture, MATF_BLEND, 0.85f * float(l + 1) / float(kRingLevels));
		gfx::draw(mesh, first[l] * 6, ringCount[l] * 6);
	}
}

} // namespace g3
