#include "ground.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace g3 {

void GroundSampler::build(const SrcScene& scene, size_t count, bool solidOnly) {
	_scene = &scene;
	count = std::min(count, scene.triangles.size());
	float maxX = -1e30f, maxZ = -1e30f;
	_minX = _minZ = 1e30f;
	for (size_t t = 0; t < count; ++t)
		for (const SrcVertex& v : scene.triangles[t].v) {
			_minX = std::min(_minX, v.pos[0]);
			_minZ = std::min(_minZ, v.pos[2]);
			maxX = std::max(maxX, v.pos[0]);
			maxZ = std::max(maxZ, v.pos[2]);
		}
	if (count == 0) {
		_w = _h = 0;
		return;
	}
	_w = std::max(1, int(std::ceil((maxX - _minX) / _cell)));
	_h = std::max(1, int(std::ceil((maxZ - _minZ) / _cell)));

	auto range = [&](const SrcTriangle& tri, int& x0, int& x1, int& z0, int& z1) {
		float mnx = std::min({tri.v[0].pos[0], tri.v[1].pos[0], tri.v[2].pos[0]});
		float mxx = std::max({tri.v[0].pos[0], tri.v[1].pos[0], tri.v[2].pos[0]});
		float mnz = std::min({tri.v[0].pos[2], tri.v[1].pos[2], tri.v[2].pos[2]});
		float mxz = std::max({tri.v[0].pos[2], tri.v[1].pos[2], tri.v[2].pos[2]});
		x0 = std::clamp(int((mnx - _minX) / _cell), 0, _w - 1);
		x1 = std::clamp(int((mxx - _minX) / _cell), 0, _w - 1);
		z0 = std::clamp(int((mnz - _minZ) / _cell), 0, _h - 1);
		z1 = std::clamp(int((mxz - _minZ) / _cell), 0, _h - 1);
	};

	const auto skipped = [&](const SrcTriangle& tri) {
		if (!solidOnly || tri.material >= scene.materials.size()) return false;
		const SrcMaterial& m = scene.materials[tri.material];
		return m.water || m.noCollision;
	};
	// Counting sort into cells: count, prefix-sum, fill.
	_start.assign(size_t(_w) * _h + 1, 0);
	for (size_t t = 0; t < count; ++t) {
		if (skipped(scene.triangles[t])) continue;
		int x0, x1, z0, z1;
		range(scene.triangles[t], x0, x1, z0, z1);
		for (int z = z0; z <= z1; ++z)
			for (int x = x0; x <= x1; ++x) ++_start[size_t(z) * _w + x + 1];
	}
	for (size_t i = 1; i < _start.size(); ++i) _start[i] += _start[i - 1];
	_items.resize(_start.back());
	std::vector<u32> fill(_start.begin(), _start.end() - 1);
	for (size_t t = 0; t < count; ++t) {
		if (skipped(scene.triangles[t])) continue;
		int x0, x1, z0, z1;
		range(scene.triangles[t], x0, x1, z0, z1);
		for (int z = z0; z <= z1; ++z)
			for (int x = x0; x <= x1; ++x) _items[fill[size_t(z) * _w + x]++] = u32(t);
	}
}

bool GroundSampler::sample(float x, float y, float z, u8 rgba[4], float* height) const {
	if (!_scene || _w == 0) return false;
	const int cx = int((x - _minX) / _cell), cz = int((z - _minZ) / _cell);
	if (x < _minX || z < _minZ || cx >= _w || cz >= _h) return false;

	bool found = false;
	float best = -1e30f;
	const size_t cell = size_t(cz) * _w + cx;
	for (u32 i = _start[cell]; i < _start[cell + 1]; ++i) {
		const SrcTriangle& tri = _scene->triangles[_items[i]];
		const float* a = tri.v[0].pos;
		const float* b = tri.v[1].pos;
		const float* c = tri.v[2].pos;
		// Barycentric coordinates of (x, z) in the triangle's XZ projection.
		const float det = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
		if (std::fabs(det) < 1e-6f) continue; // vertical
		const float w0 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / det;
		const float w1 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / det;
		const float w2 = 1.0f - w0 - w1;
		const float eps = -1e-4f;
		if (w0 < eps || w1 < eps || w2 < eps) continue;
		const float h = a[1] * w0 + b[1] * w1 + c[1] * w2;
		if (h > y || h <= best) continue;
		best = h;
		found = true;
		for (int k = 0; k < 4; ++k) {
			const float v = tri.v[0].rgba[k] * w0 + tri.v[1].rgba[k] * w1 + tri.v[2].rgba[k] * w2;
			rgba[k] = u8(std::clamp(v + 0.5f, 0.0f, 255.0f));
		}
	}
	if (found && height) *height = best;
	return found;
}

} // namespace g3
