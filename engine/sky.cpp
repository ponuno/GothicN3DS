#include "sky.h"

#include "g3ds/world.h"

#include <math.h>

namespace g3 {

namespace {

struct Key {
	float hour;
	SkyColors c;
};

// Night, dawn, day, dusk. Linear interpolation in between.
const Key kKeys[] = {
    {0.0f, {{8, 10, 24}, {18, 22, 40}, {0.30f, 0.34f, 0.50f}}},
    {5.0f, {{8, 10, 24}, {18, 22, 40}, {0.30f, 0.34f, 0.50f}}},
    {6.5f, {{58, 70, 112}, {196, 136, 104}, {0.78f, 0.66f, 0.60f}}},
    {9.0f, {{66, 112, 184}, {150, 170, 190}, {1.00f, 1.00f, 1.00f}}},
    {17.0f, {{66, 112, 184}, {150, 170, 190}, {1.00f, 1.00f, 1.00f}}},
    {19.5f, {{58, 58, 100}, {204, 124, 84}, {0.82f, 0.62f, 0.52f}}},
    {21.5f, {{8, 10, 24}, {18, 22, 40}, {0.30f, 0.34f, 0.50f}}},
    {24.0f, {{8, 10, 24}, {18, 22, 40}, {0.30f, 0.34f, 0.50f}}},
};
const int kKeyCount = int(sizeof(kKeys) / sizeof(kKeys[0]));

// Elevation of each ring in degrees and how far towards the zenith colour it is.
const float kRingElevation[] = {-12.0f, 0.0f, 7.0f, 18.0f, 38.0f};
const float kRingBlend[] = {0.0f, 0.0f, 0.22f, 0.55f, 0.88f};
const int kRings = 5;

// How far towards the zenith colour a point of the sky is, by its elevation (the sky's own rings, in between).
float blendAtElevation(float el) {
	if (el <= kRingElevation[0]) return kRingBlend[0];
	for (int i = 0; i + 1 < kRings; ++i)
		if (el <= kRingElevation[i + 1]) return kRingBlend[i] + (kRingBlend[i + 1] - kRingBlend[i]) * (el - kRingElevation[i]) / (kRingElevation[i + 1] - kRingElevation[i]);
	return kRingBlend[kRings - 1] + (1.0f - kRingBlend[kRings - 1]) * (el - kRingElevation[kRings - 1]) / (90.0f - kRingElevation[kRings - 1]);
}

float hash3(int x, int y, int t) {
	unsigned h = unsigned(x) * 374761393u + unsigned(y) * 668265263u + unsigned(t) * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	h ^= h >> 16;
	return float(h & 0xFFFFu) * (1.0f / 65535.0f);
}

bool same(const SkyColors& a, const SkyColors& b) {
	for (int i = 0; i < 3; ++i)
		if (a.zenith[i] != b.zenith[i] || a.horizon[i] != b.horizon[i]) return false;
	return true;
}

const float kPi = 3.14159265f;
// The dome: rings of 64 points at these elevations (degrees), and a point at the zenith.
const float kDomeElevation[] = {-12.0f, 0.0f, 3.0f, 6.0f, 10.0f, 15.0f, 21.0f, 28.0f, 36.0f, 45.0f, 54.0f, 63.0f, 72.0f, 81.0f};
const int kDomeRings = int(sizeof(kDomeElevation) / sizeof(kDomeElevation[0]));
const int kDomeSegments = 64;
const int kDomeVerts = kDomeRings * kDomeSegments + 1;

float smooth01(float x) {
	x = x < 0 ? 0.0f : x > 1 ? 1.0f : x;
	return x * x * (3.0f - 2.0f * x);
}
float clamp01(float x) { return x < 0 ? 0.0f : x > 1 ? 1.0f : x; }

// Value noise on a plane, 0..1, and three octaves of it (the clouds).
float cell(int x, int y) {
	unsigned h = unsigned(x) * 374761393u + unsigned(y) * 668265263u + 1013904223u;
	h = (h ^ (h >> 13)) * 1274126177u;
	h ^= h >> 16;
	return float(h & 0xFFFFu) * (1.0f / 65535.0f);
}
float valueNoise(float u, float v) {
	const int x0 = int(floorf(u)), y0 = int(floorf(v));
	const float fx = u - float(x0), fy = v - float(y0);
	const float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
	const float a = cell(x0, y0) + (cell(x0 + 1, y0) - cell(x0, y0)) * sx;
	const float b = cell(x0, y0 + 1) + (cell(x0 + 1, y0 + 1) - cell(x0, y0 + 1)) * sx;
	return a + (b - a) * sy;
}
float fbm(float u, float v) { return 0.55f * valueNoise(u, v) + 0.30f * valueNoise(u * 2.1f + 7.0f, v * 2.1f + 3.0f) + 0.15f * valueNoise(u * 4.3f + 13.0f, v * 4.3f + 9.0f); }

// Direction on the dome: azimuth az (0 = +z, turning towards +x) and elevation, radians.
void dirOf(float az, float el, float out[3]) {
	out[0] = cosf(el) * sinf(az);
	out[1] = sinf(el);
	out[2] = cosf(el) * cosf(az);
}

} // namespace

void Sky::sunDirection(float hour, float out[3]) { sunDirectionAt(hour, out); } // (g3ds/world.h: the baker's sun too)

void Sky::moonDirection(float hour, float out[3]) {
	const float t = (hour - 6.0f) / 12.0f * kPi + kPi;
	const float el = 62.0f * 0.0174533f * sinf(t);
	const float az = kPi * 0.5f + t;
	dirOf(az, el, out);
}

SkyColors skyColorsAt(float hour) {
	if (!(hour > -1.0e4f && hour < 1.0e4f)) hour = 12.0f; // (fix134: not for ever below)
	while (hour < 0) hour += 24.0f;
	while (hour >= 24.0f) hour -= 24.0f;
	int k = 0;
	while (k + 2 < kKeyCount && hour >= kKeys[k + 1].hour) ++k;
	const Key& a = kKeys[k];
	const Key& b = kKeys[k + 1];
	const float t = clamp((hour - a.hour) / (b.hour - a.hour), 0.0f, 1.0f);
	SkyColors out;
	for (int i = 0; i < 3; ++i) {
		out.zenith[i] = u8(float(a.c.zenith[i]) + (float(b.c.zenith[i]) - float(a.c.zenith[i])) * t + 0.5f);
		out.horizon[i] = u8(float(a.c.horizon[i]) + (float(b.c.horizon[i]) - float(a.c.horizon[i])) * t + 0.5f);
		out.light[i] = a.c.light[i] + (b.c.light[i] - a.c.light[i]) * t;
	}
	return out;
}

void Sky::release() {
	for (u32 i = 0; i < MESHES; ++i) {
		if (_meshes[i]) gfx::meshDestroy(_meshes[i]);
		if (_bodyMeshes[i]) gfx::meshDestroy(_bodyMeshes[i]);
		_meshes[i] = _bodyMeshes[i] = nullptr;
	}
	if (_dot) gfx::textureDestroy(_dot);
	if (_disc) gfx::textureDestroy(_disc);
	_dot = _disc = nullptr;
	_hasBuilt = false;
}

namespace {

struct Dome {
	WorldVertex v[kDomeVerts];
	float dir[kDomeVerts][3];
	float az[kDomeVerts], el[kDomeVerts]; // radians, degrees
	bool ready = false;
};
Dome gDome;

void buildDomeShape() {
	if (gDome.ready) return;
	for (int r = 0; r < kDomeRings; ++r) {
		const float el = kDomeElevation[r] * 0.0174533f;
		for (int s = 0; s < kDomeSegments; ++s) {
			const int i = r * kDomeSegments + s;
			const float az = float(s) * 6.2831853f / float(kDomeSegments);
			dirOf(az, el, gDome.dir[i]);
			gDome.az[i] = az;
			gDome.el[i] = kDomeElevation[r];
		}
	}
	const int top = kDomeVerts - 1;
	gDome.dir[top][0] = 0;
	gDome.dir[top][1] = 1;
	gDome.dir[top][2] = 0;
	gDome.az[top] = 0;
	gDome.el[top] = 90.0f;
	for (int i = 0; i < kDomeVerts; ++i) {
		WorldVertex& v = gDome.v[i];
		v.x = i16((gDome.dir[i][0] * 0.5f + 0.5f) * 32766.0f);
		v.y = i16((gDome.dir[i][1] * 0.5f + 0.5f) * 32766.0f);
		v.z = i16((gDome.dir[i][2] * 0.5f + 0.5f) * 32766.0f);
		v.w = 1;
		v.u = v.v = 0;
		v.a = 0; // final colours: not tinted by the time of day again
	}
	gDome.ready = true;
}

float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

} // namespace

// Colours of every point of the dome: the gradient, the clouds, the glow of the sun and the moon. (The Barrier: engine/barrier.h.)
void Sky::rebuild(const SkyColors& colors, float seconds) {
	buildDomeShape();
	float sun[3], moon[3];
	sunDirection(_scene.hour, sun);
	moonDirection(_scene.hour, moon);
	const float day = (colors.light[0] + colors.light[1] + colors.light[2]) * (1.0f / 3.0f); // how bright the outdoors is, 0.3 .. 1
	const float sunUp = smooth01((sun[1] + 0.10f) / 0.18f);  // 0 well under the horizon, 1 a little over it
	const float moonUp = smooth01((moon[1] + 0.05f) / 0.2f) * (1.0f - clamp01((day - 0.35f) / 0.35f));
	const float cover = clamp01(0.30f + 0.85f * _scene.cloud); // clear days have a few clouds
	const float thr = 0.80f - 0.52f * cover;
	// Cloud tone: white in the sun, grey in rain, dim at night; towards the sun's colour near it.
	const float tone = clamp01(0.35f + 0.65f * clamp01((day - 0.25f) / 0.75f));
	const float grey = clamp01(_scene.cloud * 0.55f + _scene.rain * 0.35f);
	float cloudCol[3];
	for (int k = 0; k < 3; ++k) {
		const float lit = k == 2 ? 255.0f : 250.0f, dull = 140.0f + 8.0f * float(k);
		cloudCol[k] = (lit + (dull - lit) * grey) * tone * (0.55f + 0.45f * clamp01((day - 0.3f) / 0.4f));
	}
	const float sunCol[3] = {255.0f, 190.0f + 50.0f * clamp01(sun[1] * 2.5f), 120.0f + 110.0f * clamp01(sun[1] * 2.5f)};
	const float drift = seconds * 0.010f;
	for (int i = 0; i < kDomeVerts; ++i) {
		const float* d = gDome.dir[i];
		const float el = gDome.el[i];
		float c[3];
		const float blend = i == kDomeVerts - 1 ? 1.0f : blendAtElevation(el);
		for (int k = 0; k < 3; ++k) c[k] = float(colors.horizon[k]) + (float(colors.zenith[k]) - float(colors.horizon[k])) * blend;
		// The glow round the sun (tight and wide), weaker in cloud; and round the moon at night.
		const float ds = dot3(d, sun), dm = dot3(d, moon);
		const float glowSun = sunUp * (0.55f * powf(clamp01(ds), 48.0f) + 0.22f * powf(clamp01(ds), 6.0f) + 0.10f * powf(clamp01(ds), 2.0f)) * (1.0f - 0.65f * _scene.cloud);
		const float glowMoon = moonUp * (0.30f * powf(clamp01(dm), 90.0f) + 0.10f * powf(clamp01(dm), 10.0f)) * (1.0f - 0.7f * _scene.cloud);
		for (int k = 0; k < 3; ++k) {
			c[k] += (sunCol[k] - c[k]) * clamp01(glowSun);
			c[k] += (215.0f + 0.0f * float(k) - c[k]) * clamp01(glowMoon) * (k == 2 ? 1.0f : 0.9f);
		}
		// Clouds on a flat layer: the plane's coordinates, compressed towards the horizon.
		float density = 0.0f;
		if (d[1] > 0.0f && i != kDomeVerts - 1) {
			const float scale = 1.0f / (d[1] + 0.22f);
			const float u = d[0] * scale * 0.95f + drift, v = d[2] * scale * 0.95f + drift * 0.35f;
			const float f = fbm(u, v);
			density = smooth01((f - thr) / 0.24f) * smooth01(d[1] / 0.14f);
		} else if (i == kDomeVerts - 1) {
			density = smooth01((fbm(drift, drift * 0.35f) - thr) / 0.24f);
		}
		if (density > 0.0f) {
			const float near = sunUp * powf(clamp01(ds), 12.0f) * (1.0f - _scene.cloud * 0.5f);
			for (int k = 0; k < 3; ++k) {
				const float cc = cloudCol[k] + (sunCol[k] - cloudCol[k]) * near * 0.5f;
				c[k] += (cc - c[k]) * density * 0.88f;
			}
		}
		if (_scene.flash > 0.0f)
			for (int k = 0; k < 3; ++k) c[k] += (235.0f - c[k]) * _scene.flash * (0.25f + 0.55f * density);
		WorldVertex& v = gDome.v[i];
		v.r = u8(c[0] < 0 ? 0 : c[0] > 255 ? 255 : c[0]);
		v.g = u8(c[1] < 0 ? 0 : c[1] > 255 ? 255 : c[1]);
		v.b = u8(c[2] < 0 ? 0 : c[2] > 255 ? 255 : c[2]);
	}
	if (!_meshes[0]) {
		static u16 indices[(kDomeRings - 1) * kDomeSegments * 6 + kDomeSegments * 3];
		u32 n = 0;
		for (int r = 0; r + 1 < kDomeRings; ++r)
			for (int s = 0; s < kDomeSegments; ++s) {
				const u16 a = u16(r * kDomeSegments + s), b = u16(r * kDomeSegments + (s + 1) % kDomeSegments);
				const u16 c2 = u16(a + kDomeSegments), d2 = u16(b + kDomeSegments);
				indices[n++] = a; indices[n++] = b; indices[n++] = d2;
				indices[n++] = a; indices[n++] = d2; indices[n++] = c2;
			}
		for (int s = 0; s < kDomeSegments; ++s) {
			indices[n++] = u16((kDomeRings - 1) * kDomeSegments + s);
			indices[n++] = u16((kDomeRings - 1) * kDomeSegments + (s + 1) % kDomeSegments);
			indices[n++] = u16(kDomeVerts - 1);
		}
		for (gfx::Mesh*& m : _meshes) {
			m = gfx::meshCreate(gDome.v, u32(kDomeVerts), indices, n);
			if (!m) return;
		}
		_indexCount = n;
	} else {
		_turn = (_turn + 1) % MESHES;
		gfx::meshUpdate(_meshes[_turn], gDome.v, u32(kDomeVerts));
	}
	_built = colors;
	_builtScene = _scene;
	_builtSeconds = seconds;
	_hasBuilt = true;
}

void Sky::draw(Vec3 camera, float radius, const SkyColors& colors, float seconds) {
	const bool moved = !_hasBuilt || !same(colors, _built) || fabsf(_scene.hour - _builtScene.hour) > 0.015f ||
	                   fabsf(_scene.cloud - _builtScene.cloud) > 0.01f || fabsf(_scene.rain - _builtScene.rain) > 0.02f ||
	                   fabsf(_scene.flash - _builtScene.flash) > 0.04f || fabsf(seconds - _builtSeconds) > 1.5f;
	if (moved) rebuild(colors, seconds);
	if (!_meshes[0]) return;
	const float origin[3] = {camera.x - radius, camera.y - radius, camera.z - radius};
	gfx::setChunkTransform(origin, 2.0f * radius / 32766.0f, 1.0f);
	gfx::setMaterial(nullptr, 0);
	gfx::draw(_meshes[_turn], 0, _indexCount);
	drawBodies(camera, radius, colors, seconds);
}

// Stars at night and the discs of the sun and the moon: small quads in front of the dome, cut out by their texture's alpha.
void Sky::drawBodies(Vec3 camera, float radius, const SkyColors& colors, float seconds) {
	float sun[3], moon[3];
	sunDirection(_scene.hour, sun);
	moonDirection(_scene.hour, moon);
	const float day = (colors.light[0] + colors.light[1] + colors.light[2]) * (1.0f / 3.0f);
	const float dark = clamp01((0.62f - day) / 0.28f) * (1.0f - clamp01(_scene.cloud * 1.2f)); // how starry the night is
	const bool showSun = sun[1] > -0.06f, showMoon = moon[1] > -0.04f && day < 0.8f;
	if (dark <= 0.02f && !showSun && !showMoon) return;
	constexpr int STARS = 170;
	constexpr u32 QUADS = STARS + 2;
	if (!_dot) {
		u8 cov[8 * 8];
		for (int y = 0; y < 8; ++y)
			for (int x = 0; x < 8; ++x) {
				const float dx = (float(x) - 3.5f) / 3.5f, dy = (float(y) - 3.5f) / 3.5f;
				cov[y * 8 + x] = u8(clamp01(1.25f - sqrtf(dx * dx + dy * dy)) * 255.0f);
			}
		_dot = gfx::uiTextureCreate(8, 8, cov, true);
		u8 disc[32 * 32];
		for (int y = 0; y < 32; ++y)
			for (int x = 0; x < 32; ++x) {
				const float dx = (float(x) - 15.5f) / 15.5f, dy = (float(y) - 15.5f) / 15.5f;
				disc[y * 32 + x] = u8(clamp01((1.0f - sqrtf(dx * dx + dy * dy)) * 7.0f) * 255.0f);
			}
		_disc = gfx::uiTextureCreate(32, 32, disc, true);
		if (!_dot || !_disc) return;
	}
	if (!_bodyMeshes[0]) {
		static u16 indices[QUADS * 6];
		for (u32 q = 0; q < QUADS; ++q) {
			const u16 b = u16(q * 4);
			const u16 quad[6] = {b, u16(b + 1), u16(b + 2), u16(b + 2), u16(b + 1), u16(b + 3)};
			for (int k = 0; k < 6; ++k) indices[q * 6 + u32(k)] = quad[k];
		}
		static WorldVertex blank[QUADS * 4];
		for (gfx::Mesh*& m : _bodyMeshes) {
			m = gfx::meshCreate(blank, QUADS * 4, indices, QUADS * 6);
			if (!m) return;
		}
	}
	static WorldVertex verts[QUADS * 4];
	u32 n = 0;
	auto quad = [&](const float dir[3], float angle, const float colour[3], bool starTex) {
		// A square facing the camera at the dome's distance, `angle` radians across.
		float right[3] = {dir[2], 0.0f, -dir[0]};
		float rl = sqrtf(right[0] * right[0] + right[2] * right[2]);
		if (rl < 0.05f) {
			right[0] = 1.0f;
			right[2] = 0.0f;
			rl = 1.0f;
		}
		right[0] /= rl;
		right[2] /= rl;
		const float u3[3] = {dir[1] * right[2], dir[2] * right[0] - dir[0] * right[2], -dir[1] * right[0]}; // dir x right
		const float half = tanf(angle * 0.5f) * 0.97f;
		static const float cx[4] = {-1, 1, -1, 1}, cy[4] = {1, 1, -1, -1};
		for (int k = 0; k < 4; ++k) {
			float p[3];
			for (int a = 0; a < 3; ++a) p[a] = dir[a] * 0.97f + (a == 1 ? 0.0f : right[a]) * cx[k] * half + u3[a] * cy[k] * half;
			WorldVertex& v = verts[n * 4 + u32(k)];
			v.x = i16(clamp((p[0] * 0.5f + 0.5f) * 32766.0f, -32000.0f, 32766.0f));
			v.y = i16(clamp((p[1] * 0.5f + 0.5f) * 32766.0f, -32000.0f, 32766.0f));
			v.z = i16(clamp((p[2] * 0.5f + 0.5f) * 32766.0f, -32000.0f, 32766.0f));
			v.w = 1;
			v.u = i16(cx[k] < 0 ? 0 : 1024);
			v.v = i16(cy[k] > 0 ? 0 : 1024);
			v.r = u8(clamp(colour[0], 0.0f, 255.0f));
			v.g = u8(clamp(colour[1], 0.0f, 255.0f));
			v.b = u8(clamp(colour[2], 0.0f, 255.0f));
			v.a = 0;
		}
		(void)starTex;
		++n;
	};
	// Stars: a fixed scatter over the upper sky, turning with the hour; each twinkles.
	u32 starQuads = 0;
	if (dark > 0.02f) {
		const float turn = _scene.hour * 0.2618f; // a full turn a day
		for (int i = 0; i < STARS; ++i) {
			const float r1 = hash3(i, 11, 5), r2 = hash3(i, 23, 9), r3 = hash3(i, 37, 3);
			const float y = 0.06f + 0.94f * r1; // upper sky
			const float az = r2 * 6.2831853f + turn;
			const float cy = sqrtf(1.0f - y * y);
			const float d[3] = {cy * sinf(az), y, cy * cosf(az)};
			const float twinkle = 0.70f + 0.30f * sinf(seconds * (1.3f + 2.0f * r3) + r2 * 40.0f);
			const float k = dark * twinkle * (0.35f + 0.65f * r3) * smooth01(y / 0.18f);
			if (k < 0.04f) continue;
			// Colour: between what the dome is there and white.
			const float blend = blendAtElevation(asinf(y) * 57.2958f);
			float col[3];
			for (int c = 0; c < 3; ++c) {
				const float sky = float(colors.horizon[c]) + (float(colors.zenith[c]) - float(colors.horizon[c])) * blend;
				const float star = c == 2 ? 255.0f : (c == 1 ? 244.0f : 232.0f);
				col[c] = sky + (star - sky) * clamp01(k * 1.15f);
			}
			quad(d, 0.0085f + 0.0075f * r3, col, true);
			++starQuads;
		}
	}
	u32 discQuads = 0;
	auto disc = [&](const float dir[3], float angle, const float body[3]) {
		// The disc as the sky shows it there: behind cloud it is dim.
		const float scale = 1.0f / (dir[1] + 0.22f);
		const float cover = clamp01(0.30f + 0.85f * _scene.cloud), thr = 0.80f - 0.52f * cover;
		const float dens = dir[1] > 0.0f ? smooth01((fbm(dir[0] * scale * 0.95f + seconds * 0.010f, dir[2] * scale * 0.95f + seconds * 0.0035f) - thr) / 0.24f) * smooth01(dir[1] / 0.14f) : 0.0f;
		const float veil = max(clamp01(dens * 0.85f + _scene.cloud * 0.4f), clamp01(_scene.rain * 1.3f));
		// Behind rain cloud (or thick cloud) the sun and the moon are not seen at all.
		if (veil > 0.8f) return;
		float col[3];
		const float blend = blendAtElevation(asinf(clamp(dir[1], -1.0f, 1.0f)) * 57.2958f);
		for (int c = 0; c < 3; ++c) {
			const float sky = float(colors.horizon[c]) + (float(colors.zenith[c]) - float(colors.horizon[c])) * blend;
			col[c] = body[c] + (sky - body[c]) * veil;
		}
		quad(dir, angle, col, false);
		++discQuads;
	};
	if (showSun) {
		const float low = clamp01(1.0f - sun[1] * 3.0f);
		const float colour[3] = {255.0f, 250.0f - 70.0f * low, 235.0f - 140.0f * low};
		disc(sun, 0.095f * (1.0f + 0.35f * low), colour);
	}
	if (showMoon) {
		const float colour[3] = {222.0f, 228.0f, 242.0f};
		disc(moon, 0.085f, colour);
	}
	if (n == 0) return;
	// (the discs were laid after the stars: starQuads quads first)
	gfx::Mesh* mesh = _bodyMeshes[_turn];
	gfx::meshUpdate(mesh, verts, n * 4);
	const float origin[3] = {camera.x - radius, camera.y - radius, camera.z - radius};
	gfx::setChunkTransform(origin, 2.0f * radius / 32766.0f, 1.0f / 1024.0f);
	if (starQuads) {
		gfx::setMaterial(_dot, MATF_ALPHA_TEST);
		gfx::draw(mesh, 0, starQuads * 6);
	}
	if (discQuads) {
		gfx::setMaterial(_disc, MATF_ALPHA_TEST);
		gfx::draw(mesh, starQuads * 6, discQuads * 6);
	}
}

} // namespace g3
