#include "sky.h"

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

const int kSegments = 16;
// Elevation of each ring in degrees and how far towards the zenith colour it is.
const float kRingElevation[] = {-12.0f, 0.0f, 7.0f, 18.0f, 38.0f};
const float kRingBlend[] = {0.0f, 0.0f, 0.22f, 0.55f, 0.88f};
const int kRings = 5;

// ---- the Barrier ----
// A finer dome (12 rings of 48 points and the zenith) whose colours are the sky's with the
// Barrier's glow mixed in: webs of faint violet lines that drift, and flashes
// that come and go a few times a second.
const int kBarrierSegments = 48;
const float kBarrierElevation[] = {-12.0f, 0.0f, 5.0f, 10.0f, 16.0f, 23.0f, 31.0f, 40.0f, 50.0f, 60.0f, 70.0f, 80.0f};
const int kBarrierRings = int(sizeof(kBarrierElevation) / sizeof(kBarrierElevation[0]));
const int kBarrierSteps = 3; // changes of the pattern per second

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

// Value noise with `wrap` cells round the sky (so that it has no seam), smooth between them.
float noise(float u, float v, int wrap, int t) {
	const int x0 = int(floorf(u)), y0 = int(floorf(v));
	const float fx = u - float(x0), fy = v - float(y0);
	const float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
	auto at = [&](int x, int y) { return hash3(((x % wrap) + wrap) % wrap, y, t); };
	const float a = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * sx;
	const float b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * sx;
	return a + (b - a) * sy;
}

// 0..1: how much the Barrier shines at an azimuth (0..2 pi) and elevation (degrees).
float barrierGlow(float az, float el, int step) {
	const float turn = az * (1.0f / 6.2831853f);
	const float v = (el + 12.0f) * (1.0f / 102.0f);
	// Lines where the noise crosses one half: a web, which drifts slowly.
	const float drift = float(step) * (0.9f / float(kBarrierSteps));
	const float n1 = noise(turn * 8.0f + drift, v * 5.0f + 3.0f, 8, 1);
	const float n2 = noise(turn * 16.0f - drift * 1.7f, v * 9.0f, 16, 2);
	float web = 1.0f - fabsf(n1 - 0.5f) * 5.0f;
	web = web > 0 ? web * web : 0.0f;
	float web2 = 1.0f - fabsf(n2 - 0.5f) * 7.0f;
	web2 = web2 > 0 ? web2 * web2 : 0.0f;
	// Flashes: patches that light up for a step or two.
	const float f = noise(turn * 6.0f, v * 4.0f, 6, 100 + step);
	const float flash = f > 0.72f ? (f - 0.72f) * (1.0f / 0.28f) : 0.0f;
	float g = 0.10f + 0.50f * web + 0.28f * web2 + 0.85f * flash * flash;
	return g > 1.0f ? 1.0f : g;
}

bool same(const SkyColors& a, const SkyColors& b) {
	for (int i = 0; i < 3; ++i)
		if (a.zenith[i] != b.zenith[i] || a.horizon[i] != b.horizon[i]) return false;
	return true;
}

} // namespace

SkyColors skyColorsAt(float hour) {
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
	if (_mesh) gfx::meshDestroy(_mesh);
	_mesh = nullptr;
}

void Sky::draw(Vec3 camera, float radius, const SkyColors& colors, float barrier, float seconds) {
	const int step = barrier > 0.0f ? int(seconds * float(kBarrierSteps)) : 0;
	if (!_mesh || !same(colors, _built) || barrier != _builtBarrier || step != _builtStep) {
		// A unit dome quantised to 16 bits: local = (direction * 0.5 + 0.5) * 32766.
		release();
		const int rings = barrier > 0.0f ? kBarrierRings : kRings;
		const int segs = barrier > 0.0f ? kBarrierSegments : kSegments;
		const float* elevation = barrier > 0.0f ? kBarrierElevation : kRingElevation;
		static WorldVertex verts[kBarrierRings * kBarrierSegments + 1];
		static u16 indices[(kBarrierRings - 1) * kBarrierSegments * 6 + kBarrierSegments * 3];
		// How bright the day is (the light multiplier of the outdoors), for how strong the glow looks against it.
		const float day = (colors.light[0] + colors.light[1] + colors.light[2]) * (1.0f / 3.0f);
		const float strength = barrier * (0.62f - 0.34f * day);
		auto put = [&](WorldVertex& v, float x, float y, float z, float blend, float az, float el) {
			v.x = i16((x * 0.5f + 0.5f) * 32766.0f);
			v.y = i16((y * 0.5f + 0.5f) * 32766.0f);
			v.z = i16((z * 0.5f + 0.5f) * 32766.0f);
			v.w = 1;
			v.u = v.v = 0;
			float c[3];
			for (int k = 0; k < 3; ++k) c[k] = float(colors.horizon[k]) + (float(colors.zenith[k]) - float(colors.horizon[k])) * blend;
			if (barrier > 0.0f) {
				static const float kGlow[3] = {118.0f, 142.0f, 255.0f};
				const float s = barrierGlow(az, el, step) * strength;
				for (int k = 0; k < 3; ++k) c[k] += (kGlow[k] * (0.55f + 0.45f * day) - c[k]) * (s > 1.0f ? 1.0f : s);
			}
			v.r = u8(c[0] < 0 ? 0 : c[0] > 255 ? 255 : c[0]);
			v.g = u8(c[1] < 0 ? 0 : c[1] > 255 ? 255 : c[1]);
			v.b = u8(c[2] < 0 ? 0 : c[2] > 255 ? 255 : c[2]);
			v.a = 0; // final colours: not tinted by the time of day again
		};
		for (int r = 0; r < rings; ++r) {
			const float el = elevation[r] * 0.0174533f;
			const float blend = barrier > 0.0f ? blendAtElevation(elevation[r]) : kRingBlend[r];
			for (int s = 0; s < segs; ++s) {
				const float az = float(s) * 6.2831853f / float(segs);
				put(verts[r * segs + s], cosf(el) * sinf(az), sinf(el), cosf(el) * cosf(az), blend, az, elevation[r]);
			}
		}
		const u16 top = u16(rings * segs);
		put(verts[top], 0, 1, 0, 1.0f, 0.0f, 90.0f);

		u32 n = 0;
		for (int r = 0; r + 1 < rings; ++r) {
			for (int s = 0; s < segs; ++s) {
				const u16 a = u16(r * segs + s), b = u16(r * segs + (s + 1) % segs);
				const u16 c = u16(a + segs), d = u16(b + segs);
				indices[n++] = a; indices[n++] = b; indices[n++] = d;
				indices[n++] = a; indices[n++] = d; indices[n++] = c;
			}
		}
		for (int s = 0; s < segs; ++s) {
			indices[n++] = u16((rings - 1) * segs + s);
			indices[n++] = u16((rings - 1) * segs + (s + 1) % segs);
			indices[n++] = top;
		}
		_mesh = gfx::meshCreate(verts, u32(rings * segs + 1), indices, n);
		_indexCount = n;
		_built = colors;
		_builtBarrier = barrier;
		_builtStep = step;
		if (!_mesh) return;
	}

	const float origin[3] = {camera.x - radius, camera.y - radius, camera.z - radius};
	gfx::setChunkTransform(origin, 2.0f * radius / 32766.0f, 1.0f);
	gfx::setMaterial(nullptr, 0);
	gfx::draw(_mesh, 0, _indexCount);
}

} // namespace g3
