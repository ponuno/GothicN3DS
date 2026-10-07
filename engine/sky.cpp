#include "sky.h"

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

void Sky::draw(Vec3 camera, float radius, const SkyColors& colors) {
	if (!_mesh || !same(colors, _built)) {
		// A unit dome quantised to 16 bits: local = (direction * 0.5 + 0.5) * 32766.
		release();
		WorldVertex verts[kRings * kSegments + 1];
		u16 indices[(kRings - 1) * kSegments * 6 + kSegments * 3];
		auto put = [&](WorldVertex& v, float x, float y, float z, float blend) {
			v.x = i16((x * 0.5f + 0.5f) * 32766.0f);
			v.y = i16((y * 0.5f + 0.5f) * 32766.0f);
			v.z = i16((z * 0.5f + 0.5f) * 32766.0f);
			v.w = 1;
			v.u = v.v = 0;
			v.r = u8(float(colors.horizon[0]) + (float(colors.zenith[0]) - float(colors.horizon[0])) * blend);
			v.g = u8(float(colors.horizon[1]) + (float(colors.zenith[1]) - float(colors.horizon[1])) * blend);
			v.b = u8(float(colors.horizon[2]) + (float(colors.zenith[2]) - float(colors.horizon[2])) * blend);
			v.a = 0; // final colours: not tinted by the time of day again
		};
		for (int r = 0; r < kRings; ++r) {
			const float el = kRingElevation[r] * 0.0174533f;
			for (int s = 0; s < kSegments; ++s) {
				const float az = float(s) * 6.2831853f / float(kSegments);
				put(verts[r * kSegments + s], cosf(el) * sinf(az), sinf(el), cosf(el) * cosf(az), kRingBlend[r]);
			}
		}
		const u16 top = u16(kRings * kSegments);
		put(verts[top], 0, 1, 0, 1.0f);

		u32 n = 0;
		for (int r = 0; r + 1 < kRings; ++r) {
			for (int s = 0; s < kSegments; ++s) {
				const u16 a = u16(r * kSegments + s), b = u16(r * kSegments + (s + 1) % kSegments);
				const u16 c = u16(a + kSegments), d = u16(b + kSegments);
				indices[n++] = a; indices[n++] = b; indices[n++] = d;
				indices[n++] = a; indices[n++] = d; indices[n++] = c;
			}
		}
		for (int s = 0; s < kSegments; ++s) {
			indices[n++] = u16((kRings - 1) * kSegments + s);
			indices[n++] = u16((kRings - 1) * kSegments + (s + 1) % kSegments);
			indices[n++] = top;
		}
		_mesh = gfx::meshCreate(verts, u32(kRings * kSegments + 1), indices, n);
		_indexCount = n;
		_built = colors;
		if (!_mesh) return;
	}

	const float origin[3] = {camera.x - radius, camera.y - radius, camera.z - radius};
	gfx::setChunkTransform(origin, 2.0f * radius / 32766.0f, 1.0f);
	gfx::setMaterial(nullptr, 0);
	gfx::draw(_mesh, 0, _indexCount);
}

} // namespace g3
