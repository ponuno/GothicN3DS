#include "clouds.h"

#include "g3ds/math.h"

#include <stdint.h>

#include <math.h>

namespace g3 {

namespace {

inline float smooth01(float x) {
	x = clamp(x, 0.0f, 1.0f);
	return x * x * (3.0f - 2.0f * x);
}

// 0..1 for a lattice point (the same everywhere, every run).
inline float hashCell(i32 x, i32 z, u32 seed) {
	u32 h = u32(x) * 0x8DA6B343u ^ u32(z) * 0xD8163841u ^ seed * 0xCB1AB31Fu;
	h ^= h >> 13;
	h *= 0x5BD1E995u;
	h ^= h >> 15;
	return float(h & 0xFFFFu) * (1.0f / 65535.0f);
}

// Value noise: the lattice point (cx + u, cz + v) with u, v the place in cells, smoothly between the corners.
float valueNoise(float u, float v, i32 cx, i32 cz, u32 seed) {
	const float fu = floorf(u), fv = floorf(v);
	const i32 ix = i32(fu) + cx, iz = i32(fv) + cz;
	const float tx = smooth01(u - fu), tz = smooth01(v - fv);
	const float a = hashCell(ix, iz, seed), b = hashCell(ix + 1, iz, seed);
	const float c = hashCell(ix, iz + 1, seed), d = hashCell(ix + 1, iz + 1, seed);
	return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * tz;
}

// The drift of one lattice: whole cells (wrapped at 2^20, far beyond any world) and the rest, cm.
struct Drift {
	i32 cells = 0;
	float rest = 0.0f;
};
Drift driftOf(double cm, float cell) {
	const double c = floor(cm / double(cell));
	Drift d;
	d.cells = i32(int64_t(c) & 0xFFFFF);
	d.rest = float(cm - c * double(cell));
	return d;
}


// The noise's value that `share` of the land lies above (measured on a wide sample of the field; linear between).
float thresholdFor(float share) {
	static const float kShare[] = {0.0f, 0.0091f, 0.0422f, 0.1157f, 0.2351f, 0.3883f, 0.4986f, 0.6096f, 0.7625f, 0.8812f, 0.9573f, 0.9908f, 1.0f};
	static const float kAt[] = {0.95f, 0.85f, 0.775f, 0.70f, 0.625f, 0.55f, 0.50f, 0.45f, 0.375f, 0.30f, 0.225f, 0.15f, 0.05f};
	const int n = int(sizeof(kShare) / sizeof(kShare[0]));
	share = clamp(share, kShare[0], kShare[n - 1]);
	for (int i = 1; i < n; ++i)
		if (share <= kShare[i]) {
			const float t = (share - kShare[i - 1]) / (kShare[i] - kShare[i - 1]);
			return kAt[i - 1] + (kAt[i] - kAt[i - 1]) * t;
		}
	return kAt[n - 1];
}

} // namespace

void Clouds::fromWeather(float cloud, float rain, float sunY, float& coverage, float& dark) {
	const float up = smooth01((sunY - 0.05f) / 0.20f);
	cloud = clamp(cloud, 0.0f, 1.0f);
	coverage = 0.22f + 0.50f * cloud;
	// Fair-weather clouds in a clear sky; more of them as it clouds over; when it is overcast the light is dim all over
	// (the sky does that) and no patch stands out; in rain none.
	dark = 0.42f * up * (1.0f - smooth01((cloud - 0.6f) / 0.4f)) * (1.0f - clamp(rain * 1.5f, 0.0f, 1.0f));
}

void Clouds::set(float coverage, float dark, double seconds) {
	_coverage = clamp(coverage, 0.0f, 1.0f);
	_dark = clamp(dark, 0.0f, 1.0f);
	// The threshold that leaves `coverage` of the land under a cloud: the noise's share above each, measured (440 km^2).
	_threshold = thresholdFor(_coverage);
	const double dx = seconds * double(WIND_X), dz = seconds * double(WIND_Z);
	_shiftX = float(fmod(dx, 1.0e7));
	_shiftZ = float(fmod(dz, 1.0e7));
	// (Moving the field along the wind = sampling it against the wind.)
	const Drift d[4] = {driftOf(-dx, BIG), driftOf(-dz, BIG), driftOf(-dx, SMALL), driftOf(-dz, SMALL)};
	for (int i = 0; i < 4; ++i) {
		_cells[i] = d[i].cells;
		_rest[i] = d[i].rest;
	}
}

float Clouds::light(float x, float z) const {
	const float big = valueNoise((x + _rest[0]) * (1.0f / BIG), (z + _rest[1]) * (1.0f / BIG), _cells[0], _cells[1], 17u);
	const float small = valueNoise((x + _rest[2]) * (1.0f / SMALL), (z + _rest[3]) * (1.0f / SMALL), _cells[2], _cells[3], 91u);
	const float n = 0.68f * big + 0.32f * small;
	// Soft edges: some 15-40 m from the sun to the middle of the shade.
	return 1.0f - smooth01((n - (_threshold - 0.11f)) / 0.22f);
}

} // namespace g3
