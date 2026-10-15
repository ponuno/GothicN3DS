#include "ripples.h"

#include "world.h"

#include <math.h>
#include <string.h>

namespace g3 {

namespace {
constexpr float kPosScale = 0.25f;
constexpr int kLevels = 6;
constexpr float kMinDepth = 8.0f; // cm of water over the feet from which there is a ripple
constexpr float kWakeArm = 0.78f; // (tile units) how far out the arms of the wake are where it ends
constexpr float kWakeLength = 1.10f; // half length of a wake quad over its half width (the arms then diverge at 19.5 degrees)
constexpr float kWakeTip = 0.06f;    // (tile units) where the tip is, from the front edge of the tile

float smooth01(float t) {
	t = clamp(t, 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}
float smoothstep(float a, float b, float x) { return smooth01((x - a) / (b - a)); }
float hash2(int x, int y) {
	u32 h = u32(x) * 374761393u + u32(y) * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	h ^= h >> 16;
	return float(h & 0xFFFFu) * (1.0f / 65535.0f);
}
float valueNoise(float x, float y) {
	const float fx = floorf(x), fy = floorf(y);
	const int ix = int(fx), iy = int(fy);
	const float tx = smooth01(x - fx), ty = smooth01(y - fy);
	const float a = hash2(ix, iy), b = hash2(ix + 1, iy), c = hash2(ix, iy + 1), d = hash2(ix + 1, iy + 1);
	return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}
float fbm(float x, float y, int seed) {
	return 0.55f * valueNoise(x + float(seed) * 17.0f, y) + 0.30f * valueNoise(x * 2.1f, y * 2.1f + float(seed) * 5.0f) + 0.15f * valueNoise(x * 4.3f + 9.0f, y * 4.3f);
}
// A soft ridge: 1 on `centre`, 0 at `half` away.
float ridge(float x, float centre, float half) { return smooth01(1.0f - fabsf(x - centre) / half); }
} // namespace

float Ripples::extent(Kind kind, float age, float size) {
	switch (kind) {
	case RING: return radius(age, size);
	case FOAM_RING: { // quick at first, then slowing: a crown of foam thrown out
		const float e = 1.0f - (1.0f - age) * (1.0f - age);
		return 18.0f + (45.0f + 80.0f * size) * e;
	}
	case FOAM: return (22.0f + 34.0f * size) * (0.65f + 0.55f * age); // a patch that swells a little as it thins
	case WAKE: return (48.0f + 70.0f * size) * (0.55f + 0.85f * age); // (half width)
	}
	return 0.0f;
}

float Ripples::visibility(Kind kind, float age, float strength) {
	const float in = smooth01(age / 0.05f);
	switch (kind) {
	case RING: return in * opacity(age, strength);
	case WAKE: return in * strength * (1.0f - age) * (1.0f - age * 0.5f);
	case FOAM: return in * strength * (1.0f - age) * sqrtf(1.0f - age);
	case FOAM_RING: return in * strength * (1.0f - age) * (1.0f - age);
	}
	return 0.0f;
}

// Four tiles of 64x64: 0 ring packet (four crests, fading inwards), 1 wake (V with curved waves between the arms), 2 foam
// patch (bubbly), 3 broken ring of foam. Every tile is empty on its border, so the filtering cannot take from the next one.
void Ripples::makeAtlas(u8* out) {
	constexpr int T = 64, W = 128;
	for (int ty = 0; ty < 2; ++ty)
		for (int tx = 0; tx < 2; ++tx) {
			const int tile = ty * 2 + tx;
			for (int y = 0; y < T; ++y)
				for (int x = 0; x < T; ++x) {
					const float nx = (float(x) + 0.5f) / float(T) * 2.0f - 1.0f; // -1..1
					const float vy = (float(y) + 0.5f) / float(T);               // 0..1
					const float ny = vy * 2.0f - 1.0f;
					const float r = sqrtf(nx * nx + ny * ny);
					const float edge = smooth01((0.96f - r) / 0.12f);
					float a = 0.0f;
					if (tile == 0) {
						a = ridge(r, 0.80f, 0.15f) * 1.00f + ridge(r, 0.63f, 0.12f) * 0.58f + ridge(r, 0.48f, 0.10f) * 0.34f + ridge(r, 0.34f, 0.09f) * 0.18f;
						a *= 0.80f + 0.20f * fbm(nx * 3.0f + 4.0f, ny * 3.0f + 1.0f, 1); // not a perfect circle
						a = min(a, 1.0f) * edge;
					} else if (tile == 1) { // the wake: v runs backwards from the tip
						const float d = vy - kWakeTip;
						if (d > 0.0f) {
							const float fall = powf(clamp(1.0f - d / (1.0f - kWakeTip), 0.0f, 1.0f), 0.85f);
							const float ax = fabsf(nx);
							const float arm = kWakeArm * d / (1.0f - kWakeTip) * 1.0f;
							const float wd = 0.05f + 0.07f * d;
							a = ridge(ax, arm, wd) * 1.0f;                // the outer arms
							a = max(a, ridge(ax, arm * 0.55f, wd * 1.2f) * 0.42f); // an inner pair, fainter
							for (int k = 1; k <= 3; ++k) {                // short curved waves between the arms
								const float yk = 0.19f * float(k) * (1.0f + 0.08f * float(k));
								const float curve = yk + 1.15f * nx * nx;
								if (ax < arm * 0.92f) a = max(a, ridge(d, curve, 0.035f + 0.01f * float(k)) * 0.38f * smooth01((arm - ax) / (0.18f * arm + 0.02f)));
							}
							a *= fall;
							a *= 0.85f + 0.15f * fbm(nx * 4.0f, vy * 4.0f, 2);
						}
						// the bow: a soft bump at the tip
						const float bx = nx / 0.20f, by = (vy - kWakeTip - 0.03f) / 0.07f;
						a = max(a, 0.7f * smooth01(1.0f - sqrtf(bx * bx + by * by)));
						a *= smooth01((0.97f - fabsf(nx)) / 0.1f) * smooth01((0.985f - vy) / 0.08f) * smooth01(vy / 0.04f);
						a = min(a, 1.0f);
					} else if (tile == 2) { // a foam patch
						const float body = smooth01(1.0f - r / 0.95f);
						const float n = fbm(nx * 3.3f + 11.0f, ny * 3.3f + 7.0f, 3);
						const float bubbles = valueNoise(nx * 9.0f + 3.0f, ny * 9.0f + 5.0f);
						a = smoothstep(0.30f, 0.62f, n * 0.85f + body * 0.75f - 0.18f);
						a *= 0.45f + 0.55f * smoothstep(0.25f, 0.6f, bubbles);
						a *= edge;
					} else { // a broken ring of foam
						const float band = ridge(r, 0.72f, 0.24f);
						const float n = fbm(nx * 3.8f + 2.0f, ny * 3.8f + 13.0f, 4);
						const float bubbles = valueNoise(nx * 10.0f + 1.0f, ny * 10.0f + 8.0f);
						a = band * smoothstep(0.32f, 0.58f, n + band * 0.25f - 0.12f);
						a *= 0.4f + 0.6f * smoothstep(0.2f, 0.55f, bubbles);
						a *= edge;
					}
					out[(ty * T + y) * W + tx * T + x] = u8(clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
				}
		}
}

float Ripples::rnd() {
	_seed = _seed * 1664525u + 1013904223u;
	return float((_seed >> 8) & 0xFFFFu) * (1.0f / 65535.0f);
}

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
	_timer = _wakeTimer = _foamTimer = 0.0f;
	_dirX = 0.0f;
	_dirZ = 1.0f;
	_leg = 1.0f;
}

void Ripples::spawn(const World& world, Kind kind, float x, float z, float surfaceY, float size, float strength, float life, float dirX, float dirZ) {
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
	r.kind = kind;
	r.x = x;
	r.z = z;
	r.y = surfaceY;
	r.size = size;
	r.strength = strength;
	r.life = life;
	r.dirX = dirX;
	r.dirZ = dirZ;
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
		const float d = sqrtf(dx * dx + dz * dz);
		speed = d / dt;
		if (speed > 3000.0f) speed = 0.0f; // (a jump across the map)
		else if (speed > 60.0f) { // the way the body goes, smoothed so that the wake does not shiver
			const float k = clamp(dt * 8.0f, 0.0f, 1.0f);
			float nx = _dirX + (dx / d - _dirX) * k, nz = _dirZ + (dz / d - _dirZ) * k;
			const float l = sqrtf(nx * nx + nz * nz);
			if (l > 0.0001f) {
				_dirX = nx / l;
				_dirZ = nz / l;
			}
		}
	}
	_last = feet;
	_haveLast = true;
	if (!in) {
		_wasIn = false;
		_timer = _wakeTimer = _foamTimer = 0.0f;
		return;
	}
	const float surface = feet.y + waterDepth;
	const float shallow = clamp(waterDepth / 70.0f, 0.55f, 1.0f); // wading to the ankles: weaker than to the waist
	const float sideX = -_dirZ, sideZ = _dirX;                    // (to the left of the way the body goes)
	if (!_wasIn) { // stepping in: a splash
		_wasIn = true;
		spawn(world, FOAM_RING, feet.x, feet.z, surface, 0.8f * shallow + 0.2f, 0.9f * shallow, 1.1f);
		spawn(world, RING, feet.x, feet.z, surface, 1.0f, 0.95f * shallow, 1.9f);
		spawn(world, RING, feet.x, feet.z, surface, 0.55f, 0.8f * shallow, 1.4f);
		spawn(world, FOAM, feet.x, feet.z, surface, 0.9f, 0.85f * shallow, 1.5f, cosf(rnd() * 6.2832f), sinf(rnd() * 6.2832f));
		_timer = 0.25f;
		_wakeTimer = 0.2f;
		_foamTimer = 0.3f;
		return;
	}
	const float move = clamp(speed / 480.0f, 0.0f, 1.0f);
	const bool going = swimming || move > 0.08f;
	const float m = swimming ? max(move, 0.5f) : move;

	_timer -= dt;
	if (_timer <= 0.0f) {
		_leg = -_leg;
		const float off = 14.0f * _leg;
		const float px = feet.x + sideX * off, pz = feet.z + sideZ * off;
		if (going) { // a packet of crests from the foot that has just come down
			spawn(world, RING, px, pz, surface, 0.35f + 0.5f * m, (0.55f + 0.35f * m) * shallow, 1.5f);
			_timer = 0.42f - 0.26f * m;
		} else { // standing: a faint slow ring from one leg, then the other
			spawn(world, RING, px, pz, surface, 0.18f + 0.06f * rnd(), 0.42f * shallow, 1.8f);
			_timer = 0.85f + 0.3f * rnd();
		}
	}
	if (going && (swimming || move > 0.22f)) { // the wake: the faster, the wider and the more often
		_wakeTimer -= dt;
		if (_wakeTimer <= 0.0f) {
			const float tipAhead = swimming ? 30.0f : 18.0f;
			spawn(world, WAKE, feet.x + _dirX * tipAhead, feet.z + _dirZ * tipAhead, surface, 0.25f + 0.6f * m, (0.55f + 0.35f * m) * shallow, 1.7f, _dirX, _dirZ);
			_wakeTimer = 0.34f - 0.16f * m;
		}
	}
	if (going) { // flecks of foam left where the legs have churned the water
		_foamTimer -= dt;
		if (_foamTimer <= 0.0f) {
			const float back = 25.0f + 20.0f * rnd();
			const float side = (rnd() - 0.5f) * 40.0f;
			const float a = rnd() * 6.2832f;
			spawn(world, FOAM, feet.x - _dirX * back + sideX * side, feet.z - _dirZ * back + sideZ * side, surface, 0.15f + 0.4f * m, (0.35f + 0.4f * m) * shallow, 1.0f + 0.5f * rnd(), cosf(a), sinf(a));
			_foamTimer = (swimming ? 0.22f : 0.30f) - 0.10f * m;
		}
	}
}

void Ripples::draw(const World& world, Vec3 camera, const float skyLight[3]) {
	if (_count == 0) return;
	if (!_texture) {
		static u8 atlas[128 * 128];
		makeAtlas(atlas);
		_texture = gfx::uiTextureCreate(128, 128, atlas, true);
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
	// crests are pale blue, foam and the wake whiter
	const u8 tint[3][3] = {
		{u8(clamp(205.0f * bright + 30.0f, 0.0f, 255.0f)), u8(clamp(220.0f * bright + 30.0f, 0.0f, 255.0f)), u8(clamp(238.0f * bright + 20.0f, 0.0f, 255.0f))},
		{u8(clamp(222.0f * bright + 28.0f, 0.0f, 255.0f)), u8(clamp(234.0f * bright + 24.0f, 0.0f, 255.0f)), u8(clamp(246.0f * bright + 14.0f, 0.0f, 255.0f))},
		{u8(clamp(238.0f * bright + 16.0f, 0.0f, 255.0f)), u8(clamp(244.0f * bright + 12.0f, 0.0f, 255.0f)), u8(clamp(250.0f * bright + 5.0f, 0.0f, 255.0f))},
	};
	const float originXYZ[3] = {floorf(camera.x), floorf(camera.y), floorf(camera.z)};
	static WorldVertex levels[kLevels][MAX_RINGS * 4];
	u32 count[kLevels] = {};
	auto put = [&](WorldVertex& o, float x, float y, float z, int u, int v, const u8* c) {
		o.x = i16(clamp((x - originXYZ[0]) / kPosScale, -32000.0f, 32000.0f));
		o.y = i16(clamp((y - originXYZ[1]) / kPosScale, -32000.0f, 32000.0f));
		o.z = i16(clamp((z - originXYZ[2]) / kPosScale, -32000.0f, 32000.0f));
		o.w = 0;
		o.u = i16(u);
		o.v = i16(v);
		o.r = c[0]; o.g = c[1]; o.b = c[2];
		o.a = 0;
	};
	u32 total = 0;
	for (u32 i = 0; i < _count; ++i) {
		const Ring& g = _ring[i];
		if (g.age >= g.life) continue;
		const float age = g.age / g.life;
		const int lv = int(visibility(g.kind, age, g.strength) * float(kLevels) + 0.5f);
		if (lv <= 0) continue;
		const int level = min(lv, kLevels) - 1;
		const float e = extent(g.kind, age, g.size);
		float lift = 0.0f, sx = 0.0f, sz = 0.0f;
		if (g.weight > 0.0f) gfx::waveSurfaceAt(g.x, g.z, world.waveSeconds(), world.waveHeight(), g.weight, lift, sx, sz);
		const float dx = g.x - camera.x, dz = g.z - camera.z;
		const float yc = g.y + 2.0f + sqrtf(dx * dx + dz * dz) * 0.003f + lift;
		// the shape's own axes: a wake runs along the way the body went (its tip at the body), the rest are turned as drawn
		float hx = e, hz = e, cx0 = g.x, cz0 = g.z, cs, sn;
		if (g.kind == WAKE) {
			hz = e * kWakeLength;
			cs = -g.dirZ;
			sn = g.dirX;
			// the tip is `kWakeTip` of the tile from its front edge: the centre lies behind it
			const float tipToCentre = (1.0f - 2.0f * kWakeTip) * hz;
			cx0 = g.x - g.dirX * tipToCentre;
			cz0 = g.z - g.dirZ * tipToCentre;
		} else {
			cs = g.dirX;
			sn = g.dirZ;
			if (g.kind == RING || g.kind == FOAM_RING) { cs = 1.0f; sn = 0.0f; }
		}
		const int tile = int(tileOf(g.kind));
		const int u0 = (tile & 1) * 512, v0 = (tile >> 1) * 512;
		const u8* colour = tint[g.kind == RING ? 0 : g.kind == WAKE ? 1 : 2];
		static const float corner[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
		WorldVertex* o = levels[level] + count[level] * 4;
		for (int k = 0; k < 4; ++k) {
			const float lx = corner[k][0] * hx, lz = corner[k][1] * hz;
			const float ox = cs * lx - sn * lz, oz = sn * lx + cs * lz;
			put(o[k], cx0 + ox, yc + sx * ox + sz * oz, cz0 + oz, u0 + (corner[k][0] < 0 ? 0 : 512), v0 + (corner[k][1] < 0 ? 0 : 512), colour);
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
