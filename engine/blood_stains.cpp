#include "blood_stains.h"

#include "world.h"

#include <math.h>
#include <string.h>

namespace g3 {

namespace {

constexpr int kTexW = 32, kTexH = 32;
constexpr float kPosScale = 0.25f;

float smooth(float x) {
	x = clamp(x, 0.0f, 1.0f);
	return x * x * (3.0f - 2.0f * x);
}

} // namespace

u32 BloodStains::rnd() {
	_rnd = _rnd * 1664525u + 1013904223u;
	return _rnd >> 8;
}

void BloodStains::release() {
	for (gfx::Mesh*& m : _meshes) {
		if (m) gfx::meshDestroy(m);
		m = nullptr;
	}
	if (_texture) gfx::textureDestroy(_texture);
	_texture = nullptr;
	_count = _next = 0;
}

void BloodStains::add(const World& world, Vec3 feet, float amount, double seconds) {
	const int n = amount > 0.6f ? 3 : amount > 0.25f ? 2 : 1;
	for (int k = 0; k < n; ++k) {
		const float a = float(rnd() & 1023) * (6.2831853f / 1024.0f);
		const float d = 15.0f + float(rnd() & 63) * 1.1f;
		const float x = feet.x + cosf(a) * d, z = feet.z + sinf(a) * d;
		float top;
		Vec3 nrm;
		// Ground close to the feet: not a ledge far below or a roof far above.
		if (!world.rayDown({x, feet.y + 80.0f, z}, 220.0f, top, nrm) || nrm.y < 0.8f) continue;
		float wy;
		if (world.waterBelow({x, feet.y + 80.0f, z}, 400.0f, wy) && wy >= top - 5.0f) continue; // (water washes it away)
		Stain& s = _stains[_next];
		_next = (_next + 1) % MAX;
		if (_count < MAX) ++_count;
		s.x = x;
		s.y = top;
		s.z = z;
		s.normal = nrm;
		s.radius = (14.0f + float(rnd() & 31)) * (0.7f + 0.6f * amount);
		s.angle = float(rnd() & 1023) * (6.2831853f / 1024.0f);
		s.born = seconds;
	}
}

void BloodStains::draw(Vec3 camera, double seconds) {
	if (_count == 0) return;
	if (!_texture) {
		static u8 coverage[kTexW * kTexH];
		for (int y = 0; y < kTexH; ++y)
			for (int x = 0; x < kTexW; ++x) {
				const float c = float(kTexH - 1) * 0.5f;
				const float dx = (float(x) - c) / c, dy = (float(y) - c) / c;
				const float r = sqrtf(dx * dx + dy * dy);
				const float ang = atan2f(dy, dx);
				// A splash: a blob with a ragged edge and a few drops round it.
				const float edge = 0.55f + 0.14f * sinf(ang * 5.0f + 0.4f) + 0.09f * sinf(ang * 9.0f + 1.7f) + 0.05f * sinf(ang * 14.0f);
				float a = 1.0f - smooth((r - edge + 0.08f) / 0.16f);
				const float drops = 0.5f + 0.5f * sinf(ang * 7.0f + 2.0f);
				if (drops > 0.8f) a = max(a, 1.0f - smooth((fabsf(r - 0.86f) - 0.04f) / 0.06f));
				coverage[y * kTexW + x] = u8(clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
			}
		_texture = gfx::uiTextureCreate(kTexW, kTexH, coverage, true);
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
	const float origin[3] = {floorf(camera.x), floorf(camera.y), floorf(camera.z)};
	static WorldVertex verts[MAX * 4];
	u32 quads = 0;
	float alphaSum = 0.0f;
	static const float corner[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
	for (u32 i = 0; i < _count; ++i) {
		const Stain& s = _stains[i];
		const float age = float(seconds - s.born);
		if (age < 0.0f || age > LIFE) continue;
		const float dx0 = s.x - camera.x, dz0 = s.z - camera.z;
		const float dist = sqrtf(dx0 * dx0 + dz0 * dz0);
		if (dist > RANGE) continue;
		// It spreads for a moment, dries darker, and goes out at the end.
		const float grow = 0.45f + 0.55f * smooth(age / 0.5f);
		const float fade = 1.0f - smooth((age - LIFE * 0.7f) / (LIFE * 0.3f));
		const float near = 1.0f - smooth((dist - RANGE * 0.7f) / (RANGE * 0.3f));
		const float dry = smooth(age / 25.0f);
		alphaSum += fade * near;
		const u8 r = u8(150.0f - 70.0f * dry), g = u8(14.0f - 6.0f * dry), b = u8(14.0f - 6.0f * dry);
		const float rad = s.radius * grow * (0.4f + 0.6f * fade);
		const float ca = cosf(s.angle), sa = sinf(s.angle);
		WorldVertex* out = verts + quads * 4;
		for (int k = 0; k < 4; ++k) {
			const float lx = corner[k][0] * rad, lz = corner[k][1] * rad;
			const float dx = lx * ca - lz * sa, dz = lx * sa + lz * ca;
			const float y = s.y + 1.2f + dist * 0.003f - (s.normal.x * dx + s.normal.z * dz) / s.normal.y;
			WorldVertex& o = out[k];
			o.x = i16(clamp((s.x + dx - origin[0]) / kPosScale, -32000.0f, 32000.0f));
			o.y = i16(clamp((y - origin[1]) / kPosScale, -32000.0f, 32000.0f));
			o.z = i16(clamp((s.z + dz - origin[2]) / kPosScale, -32000.0f, 32000.0f));
			o.w = 0;
			o.u = i16(corner[k][0] < 0 ? 0 : 1024);
			o.v = i16(corner[k][1] < 0 ? 0 : 1024);
			o.r = r; o.g = g; o.b = b;
			o.a = 0;
		}
		++quads;
	}
	if (quads == 0) return;
	gfx::Mesh* mesh = _meshes[_meshTurn];
	_meshTurn = (_meshTurn + 1) % MESHES;
	gfx::meshUpdate(mesh, verts, quads * 4);
	gfx::setChunkTransform(origin, kPosScale, 1.0f / 1024.0f);
	gfx::setMaterial(_texture, MATF_BLEND, 0.78f);
	gfx::draw(mesh, 0, quads * 6);
}

} // namespace g3
