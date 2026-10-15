#include "shadows.h"

#include "world.h"

#include <string.h>

namespace g3 {

namespace {

constexpr int kTexSize = 32;
constexpr float kPosScale = 0.25f;  // cm per unit of a vertex position (82 m around the camera)
constexpr float kMaxHeight = 300.0f; // cm above the ground at which a shadow has faded away
constexpr int kLevels = 8;          // shades of darkness: one draw call each

float smoothstep01(float x) {
	x = clamp(x, 0.0f, 1.0f);
	return x * x * (3.0f - 2.0f * x);
}

} // namespace

void Shadows::add(Vec3 feet, float radius) {
	++_stats.asked;
	if (_count >= MAX || !(radius > 0.0f)) return;
	_spots[_count++] = Spot {feet, radius};
}

void Shadows::release() {
	for (gfx::Mesh*& m : _meshes) {
		if (m) gfx::meshDestroy(m);
		m = nullptr;
	}
	if (_texture) gfx::textureDestroy(_texture);
	_texture = nullptr;
	if (_flat) gfx::textureDestroy(_flat);
	_flat = nullptr;
	_count = 0;
}

void Shadows::draw(const World& world, Vec3 camera, float strength) {
	_stats.drawn = _stats.drawCalls = 0;
	if (_count == 0 || !(strength > 0.0f)) return;
	if (!_texture) {
		// A soft disc: dark in the middle third, fading out to the rim.
		static u8 coverage[kTexSize * kTexSize];
		const float c = float(kTexSize - 1) * 0.5f;
		for (int y = 0; y < kTexSize; ++y)
			for (int x = 0; x < kTexSize; ++x) {
				const float dx = (float(x) - c) / c, dy = (float(y) - c) / c;
				const float r = sqrtf(dx * dx + dy * dy);
				coverage[y * kTexSize + x] = u8(clamp(1.0f - smoothstep01((r - 0.3f) / 0.68f), 0.0f, 1.0f) * 255.0f + 0.5f);
			}
		_texture = gfx::uiTextureCreate(kTexSize, kTexSize, coverage, true);
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

	// Lay each on the ground, darker or fainter, then sort by darkness.
	const float origin[3] = {floorf(camera.x), floorf(camera.y), floorf(camera.z)};
	static WorldVertex verts[MAX * 4];
	u8 level[MAX];
	u32 order[MAX];
	u32 n = 0;
	for (u32 i = 0; i < _count; ++i) {
		const Spot& s = _spots[i];
		const float dist = length(s.feet - camera);
		if (dist > RANGE) continue;
		float ground;
		Vec3 normal;
		if (!world.rayDown({s.feet.x, s.feet.y + 50.0f, s.feet.z}, 50.0f + kMaxHeight, ground, normal) || normal.y < 0.4f) continue;
		const float height = max(s.feet.y - ground, 0.0f);
		if (height >= kMaxHeight) continue;
		const float up = height / kMaxHeight;
		const float alpha = strength * (1.0f - up) * (1.0f - smoothstep01((dist - RANGE * 0.8f) / (RANGE * 0.2f)));
		const int lv = int(alpha * float(kLevels) + 0.5f);
		if (lv <= 0) continue;
		const float r = s.radius * (1.0f + 0.6f * up);
		// A little above the ground, more farther off (the depth buffer is coarser there).
		const float lift = 2.0f + dist * 0.003f;
		static const float corner[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
		for (int k = 0; k < 4; ++k) {
			const float dx = corner[k][0] * r, dz = corner[k][1] * r;
			// On the ground's plane through the point under the feet.
			const float y = ground + lift - (normal.x * dx + normal.z * dz) / normal.y;
			WorldVertex& o = verts[n * 4 + u32(k)];
			o.x = i16(clamp((s.feet.x + dx - origin[0]) / kPosScale, -32000.0f, 32000.0f));
			o.y = i16(clamp((y - origin[1]) / kPosScale, -32000.0f, 32000.0f));
			o.z = i16(clamp((s.feet.z + dz - origin[2]) / kPosScale, -32000.0f, 32000.0f));
			o.w = 0; // (no occlusion, no sway)
			o.u = i16(corner[k][0] < 0 ? 0 : 1024);
			o.v = i16(corner[k][1] < 0 ? 0 : 1024);
			o.r = o.g = o.b = 0; // black: the texture gives the shape, the opacity the darkness
			o.a = 0;
		}
		level[n] = u8(min(lv, kLevels));
		order[n] = n;
		++n;
	}
	if (n == 0) return;
	// Sorted by darkness (insertion sort: a handful), so each shade is one draw call.
	static WorldVertex sorted[MAX * 4];
	for (u32 i = 1; i < n; ++i)
		for (u32 k = i; k > 0 && level[order[k - 1]] > level[order[k]]; --k) {
			const u32 t = order[k];
			order[k] = order[k - 1];
			order[k - 1] = t;
		}
	for (u32 i = 0; i < n; ++i) memcpy(sorted + i * 4, verts + order[i] * 4, 4 * sizeof(WorldVertex));
	gfx::Mesh* mesh = _meshes[_meshTurn];
	_meshTurn = (_meshTurn + 1) % MESHES;
	gfx::meshUpdate(mesh, sorted, n * 4);
	gfx::setChunkTransform(origin, kPosScale, 1.0f / 1024.0f);
	for (u32 first = 0; first < n;) {
		u32 last = first + 1;
		const u8 lv = level[order[first]];
		while (last < n && level[order[last]] == lv) ++last;
		gfx::setMaterial(_texture, MATF_BLEND, float(lv) / float(kLevels));
		gfx::draw(mesh, first * 6, (last - first) * 6);
		++_stats.drawCalls;
		first = last;
	}
	_stats.drawn = n;
}

bool Shadows::cast(const World& world, Vec3 camera, const Model& model, const Pose& pose, Vec3 feet, float yaw, float lean,
                   const float dir[3], float strength) {
	if (!model.loaded() || !(strength > 0.0f) || dir[1] > -0.2f) return false;
	const float dist = length(feet - camera);
	if (dist > RANGE) return false;
	float ground;
	Vec3 n;
	if (!world.rayDown({feet.x, feet.y + 50.0f, feet.z}, 50.0f + kMaxHeight, ground, n) || n.y < 0.5f) return false;
	const float height = max(feet.y - ground, 0.0f);
	if (height >= kMaxHeight) return false;
	const float fade = (1.0f - height / kMaxHeight) * (1.0f - smoothstep01((dist - RANGE * 0.8f) / (RANGE * 0.2f)));
	const float a = strength * fade;
	if (a < 0.03f) return false;
	if (!_flat) {
		static u8 solid[8 * 8];
		memset(solid, 255, sizeof(solid));
		_flat = gfx::uiTextureCreate(8, 8, solid, true);
		if (!_flat) return false;
	}
	// The map q -> q - d (n.q + n.(o - g)) / (n.d) + n lift, q relative to the feet o, g the ground below them.
	const float nd = n.x * dir[0] + n.y * dir[1] + n.z * dir[2];
	if (nd > -0.1f) return false;
	const float nv[3] = {n.x, n.y, n.z};
	float k[9], c[3];
	for (int r = 0; r < 3; ++r)
		for (int col = 0; col < 3; ++col) k[r * 3 + col] = (r == col ? 1.0f : 0.0f) - dir[r] * nv[col] / nd;
	const float off = n.y * (feet.y - ground); // (o - g) is straight above g
	const float lift = 2.0f + dist * 0.003f;
	for (int r = 0; r < 3; ++r) c[r] = -dir[r] * off / nd + nv[r] * lift;
	// Every part of the flattened body lies over others (front and back, an arm over the chest, the armour over the
	// body): drawn once per pixel, it darkens the ground evenly - before, each layer added its darkness, and the
	// shadow came out blotchy, darkest where the limbs crossed.
	gfx::setDrawOnce(true);
	model.drawFlat(pose, feet, yaw, lean, k, c, _flat, clamp(a, 0.0f, 0.9f));
	gfx::setDrawOnce(false);
	++_stats.drawn;
	++_stats.drawCalls;
	return true;
}

} // namespace g3
