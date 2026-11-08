#include "blade_trail.h"

#include <math.h>
#include <string.h>

namespace g3 {

namespace {

constexpr u32 kMaxQuads = BladeTrail::MAX_TRAILS * (BladeTrail::SAMPLES - 1);
constexpr u32 kMaxVerts = kMaxQuads * 4;

float smooth01(float x) {
	x = clamp(x, 0.0f, 1.0f);
	return x * x * (3.0f - 2.0f * x);
}

float dist2(Vec3 a, Vec3 b) {
	const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
	return dx * dx + dy * dy + dz * dz;
}

} // namespace

BladeTrail::Trail* BladeTrail::find(u32 id) {
	for (Trail& t : _trail)
		if (t.id == id) return &t;
	return nullptr;
}

BladeTrail::Trail* BladeTrail::alloc(u32 id) {
	// A free slot, else the one whose last sample is the oldest.
	Trail* best = nullptr;
	bool wasFree = false;
	for (Trail& t : _trail) {
		if (t.id == 0xFFFFFFFFu) {
			best = &t;
			wasFree = true;
			break;
		}
		if (!best || t.last < best->last) best = &t;
	}
	if (!best) return nullptr;
	*best = Trail();
	best->id = id;
	if (wasFree) ++_count;
	return best;
}

void BladeTrail::add(u32 id, Vec3 base, Vec3 tip, double now, bool on) {
	Trail* t = find(id);
	if (!t) {
		if (!on) return;
		t = alloc(id);
		if (!t) return;
	}
	if (!on) {
		t->on = false;
		return;
	}
	t->on = true;
	if (t->count) {
		const Sample& last = t->s[(t->head + SAMPLES - 1) % SAMPLES];
		if (now - last.time < 0.004 && dist2(last.base, base) + dist2(last.tip, tip) < MIN_STEP * MIN_STEP) return;
	}
	Sample& s = t->s[t->head];
	s.base = base;
	s.tip = tip;
	s.time = now;
	t->head = (t->head + 1) % SAMPLES;
	if (t->count < SAMPLES) ++t->count;
	t->last = now;
}

void BladeTrail::draw(Vec3 eye, double now) {
	_quads = 0;
	if (_count == 0) return;
	if (!_texture) {
		// A soft white ribbon: bright along the blade, fading out at the hilt and the point.
		static u8 cov[16 * 16];
		for (int y = 0; y < 16; ++y)
			for (int x = 0; x < 16; ++x) {
				const float u = (float(x) + 0.5f) / 16.0f;
				const float a = smooth01(u / 0.18f) * smooth01((1.0f - u) / 0.18f);
				cov[y * 16 + x] = u8(clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
			}
		_texture = gfx::uiTextureCreate(16, 16, cov, true);
		if (!_texture) return;
	}
	if (!_mesh) {
		static u16 indices[kMaxQuads * 6];
		for (u32 q = 0; q < kMaxQuads; ++q) {
			const u16 b = u16(q * 4);
			const u16 quad[6] = {b, u16(b + 1), u16(b + 2), u16(b + 2), u16(b + 1), u16(b + 3)};
			memcpy(indices + q * 6, quad, sizeof(quad));
		}
		static WorldVertex blank[kMaxVerts];
		_mesh = gfx::meshCreate(blank, kMaxVerts, indices, kMaxQuads * 6);
		if (!_mesh) return;
	}
	static WorldVertex verts[kMaxVerts];
	u32 quads = 0;
	for (Trail& t : _trail) {
		if (t.id == 0xFFFFFFFFu || t.count < 2) continue;
		// The samples still young enough, oldest first.
		u32 n = 0;
		Vec3 base[SAMPLES], tip[SAMPLES];
		float age[SAMPLES];
		for (u32 k = 0; k < t.count; ++k) {
			const Sample& s = t.s[(t.head + SAMPLES - t.count + k) % SAMPLES];
			const float a = float(now - s.time);
			if (a < 0.0f || a > LIFE) continue;
			base[n] = s.base;
			tip[n] = s.tip;
			age[n] = a;
			++n;
		}
		if (n < 2) continue;
		for (u32 k = 0; k + 1 < n && quads < kMaxQuads; ++k) {
			WorldVertex* v = verts + quads * 4;
			const Vec3 b[2] = {base[k], base[k + 1]};
			const Vec3 p[2] = {tip[k], tip[k + 1]};
			const float fade[2] = {powf(1.0f - age[k] / LIFE, 1.4f), powf(1.0f - age[k + 1] / LIFE, 1.4f)};
			for (int e = 0; e < 2; ++e) {
				const float f = fade[e] * 0.92f;
				const u8 c = u8(clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
				// base (u 0) and tip (u 1), then the next sample's pair
				WorldVertex& a = v[e * 2];
				a.x = i16(clamp(b[e].x - eye.x, -32000.0f, 32000.0f));
				a.y = i16(clamp(b[e].y - eye.y, -32000.0f, 32000.0f));
				a.z = i16(clamp(b[e].z - eye.z, -32000.0f, 32000.0f));
				a.w = 1;
				a.u = 0;
				a.v = i16(clamp((float(e) * 1024.0f), 0.0f, 1024.0f));
				a.r = a.g = a.b = c;
				a.a = 0;
				WorldVertex& q = v[e * 2 + 1];
				q.x = i16(clamp(p[e].x - eye.x, -32000.0f, 32000.0f));
				q.y = i16(clamp(p[e].y - eye.y, -32000.0f, 32000.0f));
				q.z = i16(clamp(p[e].z - eye.z, -32000.0f, 32000.0f));
				q.w = 1;
				q.u = 1024;
				q.v = i16(clamp((float(e) * 1024.0f), 0.0f, 1024.0f));
				q.r = q.g = q.b = c;
				q.a = 0;
			}
			++quads;
		}
		// (The samples that have faded are dropped; a blade that stopped goes when nothing is left.)
		if (!t.on && n < 2) {
			t = Trail();
			if (_count) --_count;
		}
	}
	if (!quads) return;
	_quads = quads;
	gfx::meshUpdate(_mesh, verts, quads * 4);
	const float origin[3] = {eye.x, eye.y, eye.z};
	gfx::setChunkTransform(origin, 1.0f, 1.0f / 1024.0f);
	gfx::setTwoSided(true);
	gfx::setMaterial(_texture, MATF_ADD, 1.0f);
	gfx::draw(_mesh, 0, quads * 6);
	gfx::setTwoSided(false);
}

void BladeTrail::release() {
	if (_mesh) gfx::meshDestroy(_mesh);
	if (_texture) gfx::textureDestroy(_texture);
	_mesh = nullptr;
	_texture = nullptr;
	_count = _quads = 0;
	for (Trail& t : _trail) t = Trail();
}

} // namespace g3
