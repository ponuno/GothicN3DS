#include "puddles.h"

#include "world.h"

#include <math.h>
#include <string.h>

namespace g3 {

namespace {

constexpr int kTexW = 64, kTexH = 32; // left half: a puddle's shape; right half: a ring
constexpr float kPosScale = 0.25f;
constexpr int kRingLevels = 4;

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

bool Puddles::cellPuddle(int ix, int iz, float& x, float& z, float& rx, float& rz, float& angle, float& threshold) {
	const unsigned h = mix(unsigned(ix) * 73856093u ^ unsigned(iz) * 19349663u ^ 0x9E3779B9u);
	if (unit(h) > 0.34f) return false;
	const unsigned h2 = mix(h + 1u), h3 = mix(h + 2u), h4 = mix(h + 3u);
	x = (float(ix) + 0.2f + 0.6f * unit(h2)) * CELL;
	z = (float(iz) + 0.2f + 0.6f * unit(h2 >> 16 | h3 << 16)) * CELL;
	const float r = 70.0f + 110.0f * unit(h3);
	rx = r * (1.0f + 0.5f * unit(h4));
	rz = r;
	angle = unit(h4 >> 8) * 6.2831853f;
	threshold = 0.12f + 0.45f * unit(h2 >> 5); // wetness at which it appears
	return true;
}

void Puddles::release() {
	for (gfx::Mesh*& m : _meshes) {
		if (m) gfx::meshDestroy(m);
		m = nullptr;
	}
	if (_texture) gfx::textureDestroy(_texture);
	_texture = nullptr;
	for (Cell& c : _cells) c = Cell {};
	_drawn = 0;
}

void Puddles::draw(const World& world, Vec3 camera, float wet, float rain, const u8 sky[3], double seconds) {
	_drawn = 0;
	if (!(wet > 0.12f)) return;
	if (!_texture) {
		static u8 coverage[kTexW * kTexH];
		// The shape: a blob with a wavy edge (the same wave all round, so the texture needs no angle), soft rim.
		for (int y = 0; y < kTexH; ++y)
			for (int x = 0; x < kTexW; ++x) {
				float a = 0;
				if (x < kTexW / 2) {
					const float c = float(kTexH - 1) * 0.5f;
					const float dx = (float(x) - c) / c, dy = (float(y) - c) / c;
					const float r = sqrtf(dx * dx + dy * dy);
					const float ang = atan2f(dy, dx);
					const float edge = 0.78f + 0.10f * sinf(ang * 3.0f + 0.7f) + 0.06f * sinf(ang * 5.0f + 2.0f) + 0.04f * sinf(ang * 8.0f);
					a = 1.0f - smooth((r - edge + 0.1f) / 0.2f);
				} else {
					const float c = float(kTexH - 1) * 0.5f;
					const float dx = (float(x - kTexW / 2) - c) / c, dy = (float(y) - c) / c;
					const float r = sqrtf(dx * dx + dy * dy);
					a = clamp(1.0f - fabsf(r - 0.72f) / 0.16f, 0.0f, 1.0f);
					a *= a;
				}
				coverage[y * kTexW + x] = u8(clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
			}
		_texture = gfx::uiTextureCreate(kTexW, kTexH, coverage, true);
		if (!_texture) return;
	}
	constexpr u32 QUADS = MAX * 2; // puddles, then rings
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

	// Look round the camera: up to a few cells asked each frame, the rest from what was kept.
	const int cx = int(floorf(camera.x / CELL)), cz = int(floorf(camera.z / CELL));
	const int reach = int(RANGE / CELL) + 1;
	int budget = 4;
	struct Pick {
		const Cell* c;
		float dist;
	};
	Pick picks[MAX];
	u32 n = 0;
	for (int dz = -reach; dz <= reach; ++dz)
		for (int dx = -reach; dx <= reach; ++dx) {
			const int ix = cx + dx, iz = cz + dz;
			Cell& c = _cells[(unsigned(iz) & (GRID - 1)) * GRID + (unsigned(ix) & (GRID - 1))];
			if (c.ix != ix || c.iz != iz) {
				c = Cell {};
				c.ix = ix;
				c.iz = iz;
			}
			if (c.state == 0 || (c.state == 1 && c.retryAt > 0 && float(seconds) > c.retryAt)) {
				float px, pz, rx, rz, ang, th;
				if (!cellPuddle(ix, iz, px, pz, rx, rz, ang, th)) {
					c.state = 1;
					c.retryAt = 0;
				} else if (budget > 0) {
					--budget;
					// The top surface from well over the camera; with another surface below it (a roof, a floor) there is
					// shelter: no puddle. Slopes get none either.
					float top;
					Vec3 nrm;
					c.state = 1;
					c.retryAt = float(seconds) + 3.0f; // (the chunk may not be there yet)
					if (world.rayDown({px, camera.y + 1500.0f, pz}, 4500.0f, top, nrm) && nrm.y > 0.97f) {
						float below;
						Vec3 n2;
						c.retryAt = 0;
						if (!world.rayDown({px, top - 40.0f, pz}, 3000.0f, below, n2) && fabsf(top - camera.y) < 1800.0f) {
							c.state = 2;
							c.x = px; c.y = top; c.z = pz; c.rx = rx; c.rz = rz; c.angle = ang; c.threshold = th;
							c.normal = nrm;
						}
					}
				} else {
					continue; // (asked next frame)
				}
			}
			if (c.state != 2 || n >= MAX) continue;
			const float ddx = c.x - camera.x, ddz = c.z - camera.z;
			const float dist = sqrtf(ddx * ddx + ddz * ddz);
			if (dist < RANGE) picks[n++] = Pick {&c, dist};
		}
	if (n == 0) return;

	const float origin[3] = {floorf(camera.x), floorf(camera.y), floorf(camera.z)};
	static WorldVertex verts[QUADS * 4];
	u32 puddleQuads = 0;
	u32 ringQuads[kRingLevels] = {};
	static WorldVertex rings[kRingLevels][MAX * 4];
	u8 colour[3];
	for (int k = 0; k < 3; ++k) colour[k] = u8(float(sky[k]) * 0.62f);
	auto put = [&](WorldVertex* out, const Cell& c, float rx, float rz, float lift, float u0, float u1) {
		static const float corner[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
		const float ca = cosf(c.angle), sa = sinf(c.angle);
		for (int k = 0; k < 4; ++k) {
			const float lx = corner[k][0] * rx, lz = corner[k][1] * rz;
			const float dx = lx * ca - lz * sa, dz = lx * sa + lz * ca;
			const float y = c.y + lift - (c.normal.x * dx + c.normal.z * dz) / c.normal.y;
			WorldVertex& o = out[k];
			o.x = i16(clamp((c.x + dx - origin[0]) / kPosScale, -32000.0f, 32000.0f));
			o.y = i16(clamp((y - origin[1]) / kPosScale, -32000.0f, 32000.0f));
			o.z = i16(clamp((c.z + dz - origin[2]) / kPosScale, -32000.0f, 32000.0f));
			o.w = 0;
			o.u = i16(corner[k][0] < 0 ? u0 : u1);
			o.v = i16(corner[k][1] < 0 ? 0 : 1024);
			o.r = colour[0]; o.g = colour[1]; o.b = colour[2];
			o.a = 0;
		}
	};
	for (u32 i = 0; i < n; ++i) {
		const Cell& c = *picks[i].c;
		const float grow = smooth((wet - c.threshold) / 0.35f);
		const float fade = 1.0f - smooth((picks[i].dist - RANGE * 0.7f) / (RANGE * 0.3f));
		const float s = grow * fade;
		if (s < 0.05f) continue;
		const float lift = 1.5f + picks[i].dist * 0.003f;
		put(verts + puddleQuads * 4, c, c.rx * s, c.rz * s, lift, 0, 512);
		++puddleQuads;
		if (rain > 0.1f && s > 0.6f) {
			// A ring growing from a spot in the puddle, and fading as it widens; a second one half a turn later.
			for (int k = 0; k < 2; ++k) {
				const unsigned h = mix(unsigned(c.ix) * 31u + unsigned(c.iz) * 17u + unsigned(k) * 1013u);
				const float period = 1.1f + 0.5f * unit(h);
				float t = float(seconds) / period + unit(h >> 8) + float(k) * 0.5f;
				const unsigned turn = unsigned(floorf(t));
				t -= floorf(t);
				const unsigned hh = mix(h + turn);
				Cell spot = c;
				const float a = unit(hh) * 6.2831853f, d = sqrtf(unit(hh >> 5)) * 0.55f;
				spot.x = c.x + cosf(a) * d * c.rx * s;
				spot.z = c.z + sinf(a) * d * c.rz * s;
				const float r = 8.0f + 38.0f * t;
				const float alpha = rain * (1.0f - t) * (1.0f - t);
				const int lv = int(alpha * float(kRingLevels) + 0.5f);
				if (lv <= 0) continue;
				const int level = min(lv, kRingLevels) - 1;
				spot.angle = 0;
				put(rings[level] + ringQuads[level] * 4, spot, r, r, lift + 0.5f, 512, 1024);
				++ringQuads[level];
			}
		}
	}
	if (puddleQuads == 0) return;
	// One mesh: the puddles first, then the rings of each level.
	u32 total = puddleQuads;
	u32 first[kRingLevels];
	for (int l = 0; l < kRingLevels; ++l) {
		first[l] = total;
		if (total + ringQuads[l] > QUADS) ringQuads[l] = QUADS - total;
		memcpy(verts + total * 4, rings[l], ringQuads[l] * 4 * sizeof(WorldVertex));
		total += ringQuads[l];
	}
	gfx::Mesh* mesh = _meshes[_meshTurn];
	_meshTurn = (_meshTurn + 1) % MESHES;
	gfx::meshUpdate(mesh, verts, total * 4);
	gfx::setChunkTransform(origin, kPosScale, 1.0f / 1024.0f);
	gfx::setMaterial(_texture, MATF_BLEND, clamp(0.35f + 0.35f * wet, 0.0f, 0.7f));
	gfx::draw(mesh, 0, puddleQuads * 6);
	for (int l = 0; l < kRingLevels; ++l) {
		if (!ringQuads[l]) continue;
		gfx::setMaterial(_texture, MATF_BLEND, 0.5f * float(l + 1) / float(kRingLevels));
		gfx::draw(mesh, first[l] * 6, ringQuads[l] * 6);
	}
	_drawn = puddleQuads;
}


// ---- rain on water ----

bool Ripples::cellRing(int ix, int iz, int which, float& fx, float& fz, float& phase, float& period) {
	const unsigned h = mix(unsigned(ix) * 83492791u ^ unsigned(iz) * 2654435761u ^ unsigned(which) * 40503u ^ 0xA5A5u);
	if (unit(h) > 0.8f) return false;
	fx = 0.1f + 0.8f * unit(mix(h + 1u));
	fz = 0.1f + 0.8f * unit(mix(h + 2u));
	phase = unit(mix(h + 3u));
	period = 0.9f + 0.9f * unit(mix(h + 4u));
	return true;
}

void Ripples::release() {
	for (gfx::Mesh*& m : _meshes) {
		if (m) gfx::meshDestroy(m);
		m = nullptr;
	}
	if (_texture) gfx::textureDestroy(_texture);
	_texture = nullptr;
	for (Cell& c : _cells) c = Cell {};
	_drawn = 0;
}

void Ripples::draw(const World& world, Vec3 camera, float rain, const u8 sky[3], double seconds, float lift) {
	_drawn = 0;
	if (!(rain > 0.1f)) return;
	constexpr int kSize = 32;
	if (!_texture) {
		static u8 coverage[kSize * kSize];
		const float c = float(kSize - 1) * 0.5f;
		for (int y = 0; y < kSize; ++y)
			for (int x = 0; x < kSize; ++x) {
				const float dx = (float(x) - c) / c, dy = (float(y) - c) / c;
				const float r = sqrtf(dx * dx + dy * dy);
				float a = clamp(1.0f - fabsf(r - 0.74f) / 0.18f, 0.0f, 1.0f);
				coverage[y * kSize + x] = u8(a * a * 255.0f + 0.5f);
			}
		_texture = gfx::uiTextureCreate(kSize, kSize, coverage, true);
		if (!_texture) return;
	}
	constexpr u32 QUADS = MAX * 2;
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
	const int cx = int(floorf(camera.x / CELL)), cz = int(floorf(camera.z / CELL));
	const int reach = int(RANGE / CELL) + 1;
	int budget = 6;
	const float origin[3] = {floorf(camera.x), floorf(camera.y), floorf(camera.z)};
	static WorldVertex rings[kRingLevels][MAX * 4];
	u32 count[kRingLevels] = {};
	u32 total = 0;
	u8 colour[3];
	for (int k = 0; k < 3; ++k) colour[k] = u8(min(255.0f, float(sky[k]) * 0.4f + 190.0f));
	for (int dz = -reach; dz <= reach; ++dz)
		for (int dx = -reach; dx <= reach; ++dx) {
			const int ix = cx + dx, iz = cz + dz;
			Cell& c = _cells[(unsigned(iz) & (GRID - 1)) * GRID + (unsigned(ix) & (GRID - 1))];
			if (c.ix != ix || c.iz != iz) {
				c = Cell {};
				c.ix = ix;
				c.iz = iz;
			}
			if (c.state == 0 || (c.state == 1 && float(seconds) > c.retryAt)) {
				if (budget <= 0) continue;
				--budget;
				float y;
				c.state = 1;
				c.retryAt = float(seconds) + 4.0f;
				if (world.waterBelow({(float(ix) + 0.5f) * CELL, camera.y + 1200.0f, (float(iz) + 0.5f) * CELL}, 6000.0f, y)) {
					c.state = 2;
					c.y = y;
				}
			}
			if (c.state != 2) continue;
			for (int which = 0; which < 2; ++which) {
				float fx, fz, phase, period;
				if (!cellRing(ix, iz, which, fx, fz, phase, period)) continue;
				const float px = (float(ix) + fx) * CELL, pz = (float(iz) + fz) * CELL;
				const float ddx = px - camera.x, ddz = pz - camera.z;
				const float dist = sqrtf(ddx * ddx + ddz * ddz);
				if (dist > RANGE) continue;
				float t = float(seconds) / period + phase;
				const unsigned turn = unsigned(floorf(t));
				t -= floorf(t);
				// A new ring starts at a new place in the cell each turn.
				const unsigned hh = mix(unsigned(ix) * 31u + unsigned(iz) * 17u + unsigned(which) * 7u + turn * 1013u);
				const float ox = (unit(hh) - 0.5f) * CELL * 0.8f, oz = (unit(hh >> 7) - 0.5f) * CELL * 0.8f;
				const float r = (18.0f + 85.0f * t) * (1.0f + dist * 0.0004f);
				const float alpha = rain * (1.0f - t) * (1.0f - t) * (1.0f - smooth((dist - RANGE * 0.7f) / (RANGE * 0.3f)));
				const int lv = int(alpha * float(kRingLevels) + 0.5f);
				if (lv <= 0 || total >= QUADS) continue;
				const int level = min(lv, kRingLevels) - 1;
				if (count[level] >= MAX) continue;
				WorldVertex* o = rings[level] + count[level] * 4;
				static const float corner[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
				for (int k = 0; k < 4; ++k) {
					WorldVertex& v = o[k];
					v.x = i16(clamp((px + ox + corner[k][0] * r - origin[0]) / kPosScale, -32000.0f, 32000.0f));
					v.y = i16(clamp((c.y + lift + 2.0f + dist * 0.003f - origin[1]) / kPosScale, -32000.0f, 32000.0f));
					v.z = i16(clamp((pz + oz + corner[k][1] * r - origin[2]) / kPosScale, -32000.0f, 32000.0f));
					v.w = 0;
					v.u = i16(corner[k][0] < 0 ? 0 : 1024);
					v.v = i16(corner[k][1] < 0 ? 0 : 1024);
					v.r = colour[0]; v.g = colour[1]; v.b = colour[2];
					v.a = 0;
				}
				++count[level];
				++total;
			}
		}
	if (total == 0) return;
	static WorldVertex verts[QUADS * 4];
	u32 at = 0, first[kRingLevels];
	for (int l = 0; l < kRingLevels; ++l) {
		first[l] = at;
		memcpy(verts + at * 4, rings[l], count[l] * 4 * sizeof(WorldVertex));
		at += count[l];
	}
	gfx::Mesh* mesh = _meshes[_meshTurn];
	_meshTurn = (_meshTurn + 1) % MESHES;
	gfx::meshUpdate(mesh, verts, at * 4);
	gfx::setChunkTransform(origin, kPosScale, 1.0f / 1024.0f);
	for (int l = 0; l < kRingLevels; ++l) {
		if (!count[l]) continue;
		gfx::setMaterial(_texture, MATF_BLEND, 0.85f * float(l + 1) / float(kRingLevels));
		gfx::draw(mesh, first[l] * 6, count[l] * 6);
	}
	_drawn = at;
}

} // namespace g3
