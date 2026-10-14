#include "ground_fx.h"

#include "bvh.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <unordered_map>

namespace g3 {

namespace {

using namespace geo;

float smooth01(float x) {
	x = std::clamp(x, 0.0f, 1.0f);
	return x * x * (3.0f - 2.0f * x);
}

bool isWaterTri(const SrcScene& s, const SrcTriangle& t) { return t.material < s.materials.size() && s.materials[t.material].water; }

// What can be stood on or is a floor: not water, no foliage / glass (no collision), not blended.
bool isGround(const SrcScene& s, const SrcTriangle& t) {
	if (t.material >= s.materials.size()) return false;
	const SrcMaterial& m = s.materials[t.material];
	return !m.water && !m.noCollision && m.blend == SrcBlend::Opaque;
}

float normalY(const SrcTriangle& t) {
	const float ax = t.v[1].pos[0] - t.v[0].pos[0], az = t.v[1].pos[2] - t.v[0].pos[2];
	const float bx = t.v[2].pos[0] - t.v[0].pos[0], bz = t.v[2].pos[2] - t.v[0].pos[2];
	const float ay = t.v[1].pos[1] - t.v[0].pos[1], by = t.v[2].pos[1] - t.v[0].pos[1];
	const float nx = -(ay * bz - az * by), ny = -(az * bx - ax * bz), nz = -(ax * by - ay * bx);
	const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
	return len > 1e-6f ? ny / len : 0.0f;
}

void darken(SrcVertex& v, float amount) {
	const float k = 1.0f - std::clamp(amount, 0.0f, 0.9f);
	for (int c = 0; c < 3; ++c) v.rgba[c] = u8(std::lround(float(v.rgba[c]) * k));
}

// The water surface seen from above: its level at a point of the plane.
class WaterMap {
public:
	explicit WaterMap(const SrcScene& scene) : _scene(scene) {
		float mn[2] = {1e30f, 1e30f}, mx[2] = {-1e30f, -1e30f};
		for (const SrcTriangle& t : scene.triangles)
			if (isWaterTri(scene, t))
				for (const SrcVertex& v : t.v) {
					mn[0] = std::min(mn[0], v.pos[0]);
					mx[0] = std::max(mx[0], v.pos[0]);
					mn[1] = std::min(mn[1], v.pos[2]);
					mx[1] = std::max(mx[1], v.pos[2]);
				}
		if (mn[0] > mx[0]) return;
		// (A sea of enormous triangles: cells beyond the walkable land are of no use, the grid is capped.)
		_minX = mn[0];
		_minZ = mn[1];
		_w = int(std::min(400.0f, std::ceil((mx[0] - mn[0]) / _cell) + 1.0f));
		_h = int(std::min(400.0f, std::ceil((mx[1] - mn[1]) / _cell) + 1.0f));
		_cell = std::max(_cell, std::max((mx[0] - mn[0]) / float(_w), (mx[1] - mn[1]) / float(_h)));
		_start.assign(size_t(_w) * size_t(_h) + 1, 0);
		for (int pass = 0; pass < 2; ++pass) {
			std::vector<u32> fill;
			if (pass == 1) {
				for (size_t i = 1; i < _start.size(); ++i) _start[i] += _start[i - 1];
				_items.resize(_start.back());
				fill.assign(_start.begin(), _start.end() - 1);
			}
			for (size_t ti = 0; ti < scene.triangles.size(); ++ti) {
				const SrcTriangle& t = scene.triangles[ti];
				if (!isWaterTri(scene, t)) continue;
				float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
				for (const SrcVertex& v : t.v) {
					lo[0] = std::min(lo[0], v.pos[0]);
					hi[0] = std::max(hi[0], v.pos[0]);
					lo[1] = std::min(lo[1], v.pos[2]);
					hi[1] = std::max(hi[1], v.pos[2]);
				}
				const int x0 = clampX(int((lo[0] - _minX) / _cell)), x1 = clampX(int((hi[0] - _minX) / _cell));
				const int z0 = clampZ(int((lo[1] - _minZ) / _cell)), z1 = clampZ(int((hi[1] - _minZ) / _cell));
				for (int z = z0; z <= z1; ++z)
					for (int x = x0; x <= x1; ++x) {
						const size_t c = size_t(z) * size_t(_w) + size_t(x);
						if (pass == 0) ++_start[c + 1];
						else _items[fill[c]++] = u32(ti);
					}
			}
		}
	}
	bool empty() const { return _w == 0; }
	// The level of the water at (x, z); false if there is none.
	bool level(float x, float z, float& y) const {
		if (_w == 0 || x < _minX || z < _minZ) return false;
		const int cx = int((x - _minX) / _cell), cz = int((z - _minZ) / _cell);
		if (cx >= _w || cz >= _h) return false;
		const size_t c = size_t(cz) * size_t(_w) + size_t(cx);
		for (u32 i = _start[c]; i < _start[c + 1]; ++i) {
			const SrcTriangle& t = _scene.triangles[_items[i]];
			const float* a = t.v[0].pos;
			const float* b = t.v[1].pos;
			const float* d = t.v[2].pos;
			const float det = (b[2] - d[2]) * (a[0] - d[0]) + (d[0] - b[0]) * (a[2] - d[2]);
			if (std::fabs(det) < 1e-6f) continue;
			const float w0 = ((b[2] - d[2]) * (x - d[0]) + (d[0] - b[0]) * (z - d[2])) / det;
			const float w1 = ((d[2] - a[2]) * (x - d[0]) + (a[0] - d[0]) * (z - d[2])) / det;
			const float w2 = 1.0f - w0 - w1;
			if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
			y = a[1] * w0 + b[1] * w1 + d[1] * w2;
			return true;
		}
		return false;
	}

private:
	int clampX(int v) const { return v < 0 ? 0 : v >= _w ? _w - 1 : v; }
	int clampZ(int v) const { return v < 0 ? 0 : v >= _h ? _h - 1 : v; }
	const SrcScene& _scene;
	float _minX = 0, _minZ = 0, _cell = 400.0f;
	int _w = 0, _h = 0;
	std::vector<u32> _start, _items;
};

} // namespace

void bakeWetShore(SrcScene& scene, float strength, GroundFxStats& stats) {
	if (!(strength > 0.0f)) return;
	const WaterMap water(scene);
	if (water.empty()) return;
	const float strong = 0.45f * std::min(strength, 1.0f);
	for (SrcTriangle& t : scene.triangles) {
		if (!isGround(scene, t) || normalY(t) < 0.3f) continue;
		for (SrcVertex& v : t.v) {
			// The water's level beside the corner: under it, or a ring round it (the shore is where the water is not).
			float wl = 0;
			bool found = water.level(v.pos[0], v.pos[2], wl);
			if (!found) {
				static const float kDir[8][2] = {{1, 0}, {0.7071f, 0.7071f}, {0, 1}, {-0.7071f, 0.7071f}, {-1, 0}, {-0.7071f, -0.7071f}, {0, -1}, {0.7071f, -0.7071f}};
				for (const float r : {120.0f, 280.0f}) {
					for (const auto& d : kDir)
						if (water.level(v.pos[0] + d[0] * r, v.pos[2] + d[1] * r, wl)) {
							found = true;
							break;
						}
					if (found) break;
				}
			}
			if (!found) continue;
			// Wet up to 90 cm above the level, fully wet at it and below.
			const float wet = 1.0f - smooth01((v.pos[1] - wl) / 90.0f);
			if (wet <= 0.02f || v.pos[1] < wl - 3000.0f) continue;
			darken(v, strong * wet);
			++stats.wetCorners;
		}
	}
}

void bakeCanopy(SrcScene& scene, const std::vector<char>& plant, float strength, float reach, unsigned threads, GroundFxStats& stats) {
	if (!(strength > 0.0f)) return;
	std::vector<Tri> blockers;
	for (const SrcTriangle& t : scene.triangles) {
		if (t.material >= plant.size() || !plant[t.material]) continue;
		const V3 a {t.v[0].pos[0], t.v[0].pos[1], t.v[0].pos[2]};
		const V3 e1 = V3 {t.v[1].pos[0], t.v[1].pos[1], t.v[1].pos[2]} - a, e2 = V3 {t.v[2].pos[0], t.v[2].pos[1], t.v[2].pos[2]} - a;
		if (length(cross(e1, e2)) < 1e-4f) continue;
		blockers.push_back(Tri {a, e1, e2, 1.0f});
	}
	if (blockers.empty()) return;
	Bvh bvh;
	bvh.build(std::move(blockers));
	// Seven rays: straight up and six round at 28 degrees from it (the sun is never straight over).
	std::vector<V3> dirs;
	dirs.push_back(V3 {0, 1, 0});
	for (int i = 0; i < 6; ++i) {
		const float a = float(i) * 1.0471976f;
		dirs.push_back(V3 {std::sin(0.49f) * std::cos(a), std::cos(0.49f), std::sin(0.49f) * std::sin(a)});
	}
	// Corners of ground triangles, each position once (the answer depends on the place only).
	struct Item {
		u32 tri;
		u8 corner;
	};
	std::vector<Item> items;
	for (size_t ti = 0; ti < scene.triangles.size(); ++ti) {
		const SrcTriangle& t = scene.triangles[ti];
		if (!isGround(scene, t) || normalY(t) < 0.5f) continue;
		for (u8 k = 0; k < 3; ++k) items.push_back(Item {u32(ti), k});
	}
	std::vector<float> shade(items.size(), 0.0f);
	std::atomic<size_t> next {0};
	std::atomic<u64> rays {0};
	auto work = [&]() {
		u64 cast = 0;
		for (;;) {
			const size_t begin = next.fetch_add(1024);
			if (begin >= items.size()) break;
			const size_t end = std::min(items.size(), begin + 1024);
			for (size_t i = begin; i < end; ++i) {
				const SrcVertex& v = scene.triangles[items[i].tri].v[items[i].corner];
				const V3 o {v.pos[0], v.pos[1] + 30.0f, v.pos[2]};
				int hits = 0;
				for (const V3& d : dirs) {
					float t = 0;
					// A plant's leaf within reach above stops the ray (nearer ones weigh a little more).
					if (bvh.nearest(o, d, 0.5f, reach, t) > 0.0f) ++hits;
				}
				cast += dirs.size();
				shade[i] = float(hits) / float(dirs.size());
			}
		}
		rays += cast;
	};
	std::vector<std::thread> pool;
	for (unsigned i = 1; i < std::max(1u, threads); ++i) pool.emplace_back(work);
	work();
	for (std::thread& t : pool) t.join();
	stats.rays += rays.load();
	// Written after all are done (a corner shared by triangles is answered by its own rays, the same for the same place).
	const float strong = 0.45f * std::min(strength, 1.0f);
	for (size_t i = 0; i < items.size(); ++i) {
		if (shade[i] <= 0.0f) continue;
		darken(scene.triangles[items[i].tri].v[items[i].corner], strong * shade[i]);
		++stats.shadedCorners;
	}
}

} // namespace g3
