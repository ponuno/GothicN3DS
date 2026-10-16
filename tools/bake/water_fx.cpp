#include "water_fx.h"

#include "ground.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace g3 {

const char* const kFoamTexture = "G3_FOAM_A0.TGA";
const char* const kGlintTexture = "G3_GLINT.TGA";

namespace {

constexpr float kInf = 1e30f;
constexpr float kTile = 3200.0f;        // cm: the unit of "near a shore"
constexpr float kSlack = 50.0f;         // cm: ground this close to the water's level counts as a shore
constexpr float kRasterCell = 200.0f;   // cm: the coarse raster the rivers are found on
constexpr float kFoamPeriod = 600.0f;   // cm of shore line for one repeat of the foam picture
constexpr float kFoamLift = 3.0f;       // cm over the water: no flicker against it
constexpr int kMaxTilesPerTriangle = 90000;
constexpr float kPi = 3.14159265358979f;

using Key = uint64_t;
Key tkey(int ix, int iz) { return (Key(uint32_t(ix + (1 << 30))) << 32) | uint32_t(iz + (1 << 30)); }
void untkey(Key k, int& ix, int& iz) {
	ix = int(uint32_t(k >> 32)) - (1 << 30);
	iz = int(uint32_t(k & 0xFFFFFFFFu)) - (1 << 30);
}
int tileOf(float v) { return int(std::floor(v / kTile)); }

bool isWaterTri(const SrcScene& scene, const SrcTriangle& t) { return t.material < scene.materials.size() && scene.materials[t.material].water; }

// ---- the water's level at a point ----
class WaterLevels {
public:
	WaterLevels(const SrcScene& scene, const int lo[2], const int hi[2]) : _scene(&scene) {
		for (size_t i = 0; i < scene.triangles.size(); ++i) {
			const SrcTriangle& t = scene.triangles[i];
			if (!isWaterTri(scene, t)) continue;
			float mn[3] = {kInf, kInf, kInf}, mx[3] = {-kInf, -kInf, -kInf};
			for (const SrcVertex& v : t.v)
				for (int a = 0; a < 3; ++a) {
					mn[a] = std::min(mn[a], v.pos[a]);
					mx[a] = std::max(mx[a], v.pos[a]);
				}
			const int x0 = std::clamp(tileOf(mn[0]), lo[0], hi[0]), x1 = std::clamp(tileOf(mx[0]), lo[0], hi[0]);
			const int z0 = std::clamp(tileOf(mn[2]), lo[1], hi[1]), z1 = std::clamp(tileOf(mx[2]), lo[1], hi[1]);
			if (double(x1 - x0 + 1) * double(z1 - z0 + 1) > kMaxTilesPerTriangle) continue;
			for (int ix = x0; ix <= x1; ++ix)
				for (int iz = z0; iz <= z1; ++iz) {
					Tile& tile = _tiles[tkey(ix, iz)];
					tile.tris.push_back(u32(i));
					tile.yMin = std::min(tile.yMin, mn[1]);
					tile.yMax = std::max(tile.yMax, mx[1]);
				}
		}
	}
	bool tileHasWater(int ix, int iz) const { return _tiles.count(tkey(ix, iz)) != 0; }
	bool levelRange(int ix, int iz, float& lo, float& hi) const {
		auto it = _tiles.find(tkey(ix, iz));
		if (it == _tiles.end()) return false;
		lo = it->second.yMin;
		hi = it->second.yMax;
		return true;
	}
	// The highest water surface over (x, z).
	bool levelAt(float x, float z, float& level) const {
		auto it = _tiles.find(tkey(tileOf(x), tileOf(z)));
		if (it == _tiles.end()) return false;
		bool found = false;
		for (u32 index : it->second.tris) {
			const SrcTriangle& t = _scene->triangles[index];
			const float ax = t.v[0].pos[0], az = t.v[0].pos[2];
			const float d1x = t.v[1].pos[0] - ax, d1z = t.v[1].pos[2] - az, d2x = t.v[2].pos[0] - ax, d2z = t.v[2].pos[2] - az;
			const float det = d1x * d2z - d2x * d1z;
			if (std::fabs(det) < 1e-3f) continue;
			const float px = x - ax, pz = z - az;
			const float u = (px * d2z - d2x * pz) / det, v = (d1x * pz - px * d1z) / det;
			if (u < 0.0f || v < 0.0f || u + v > 1.0f) continue;
			const float y = t.v[0].pos[1] + u * (t.v[1].pos[1] - t.v[0].pos[1]) + v * (t.v[2].pos[1] - t.v[0].pos[1]);
			if (!found || y > level) level = y;
			found = true;
		}
		return found;
	}

private:
	struct Tile {
		std::vector<u32> tris;
		float yMin = kInf, yMax = -kInf;
	};
	const SrcScene* _scene;
	std::unordered_map<Key, Tile> _tiles;
};

// ---- how deep the water is at a point ----
struct Depth {
	bool water = false; // there is water mesh over the point
	float level = 0.0f; // its surface
	float d = -300.0f;  // level less the ground, clamped to -300..300: > 0 under water, < 0 on the bank
	float bed = 0.0f;   // the ground's height
};

class DepthField {
public:
	DepthField(const WaterLevels& levels, const GroundSampler& ground) : _levels(levels), _ground(ground) {}
	Depth at(float x, float z) const {
		Depth r;
		if (!_levels.levelAt(x, z, r.level)) return r;
		r.water = true;
		u8 rgba[4];
		float top = 0.0f;
		// (The ground from 3 m over the water down: a bank higher than that has no surface below that point to find. That is
		// land, and so is a point with nothing at all.)
		if (!_ground.sample(x, r.level + 300.0f, z, rgba, &top)) {
			r.d = -300.0f;
			r.bed = r.level + 300.0f;
			return r;
		}
		r.bed = top;
		r.d = std::clamp(r.level - top, -300.0f, 300.0f);
		return r;
	}

private:
	const WaterLevels& _levels;
	const GroundSampler& _ground;
};

// ---- shore lines: marching squares on the depth ----
struct EdgePoint {
	float x, z, level;
	float nx, nz; // unit, towards the deeper water
	float slope;  // cm of depth per cm across the shore
};

Key edgeKey(int i, int j, int axis) { return (Key(uint32_t(i + (1 << 20))) << 32) | (uint32_t(j + (1 << 20)) << 1) | uint32_t(axis); }

struct Corner {
	Depth depth;
	float gx = 0.0f, gz = 0.0f; // the gradient of the depth, cm per cm
};

class ShoreFinder {
public:
	ShoreFinder(const DepthField& field, float cell) : _field(field), _cell(cell) {}

	const Corner& corner(int i, int j) {
		auto it = _corners.find(edgeKey(i, j, 0));
		if (it != _corners.end()) return it->second;
		Corner c;
		c.depth = _field.at(float(i) * _cell, float(j) * _cell);
		return _corners.emplace(edgeKey(i, j, 0), c).first->second;
	}

	// The gradient from the neighbours, once they are known.
	void gradient(int i, int j, float& gx, float& gz) {
		const float dxp = corner(i + 1, j).depth.d, dxm = corner(i - 1, j).depth.d;
		const float dzp = corner(i, j + 1).depth.d, dzm = corner(i, j - 1).depth.d;
		gx = (dxp - dxm) / (2.0f * _cell);
		gz = (dzp - dzm) / (2.0f * _cell);
	}

	// The shore segments of the cells of one tile. A cell with a corner that has no water over it is left out (the water
	// mesh ends there, and it is not a shore).
	void tile(int ix, int iz) {
		const int cpt = int(std::lround(kTile / _cell));
		for (int a = 0; a < cpt; ++a)
			for (int b = 0; b < cpt; ++b) cell(ix * cpt + a, iz * cpt + b);
	}

	struct Chain {
		std::vector<EdgePoint> p;
		bool loop = false;
	};

	std::vector<Chain> chains() {
		std::vector<Chain> out;
		std::unordered_set<Key> used;
		auto neighbours = [&](Key k) -> const std::vector<Key>& { return _adj[k]; };
		auto walk = [&](Key start, bool loop) {
			Chain c;
			c.loop = loop;
			Key prev = 0, cur = start;
			bool havePrev = false;
			for (;;) {
				used.insert(cur);
				c.p.push_back(_points[cur]);
				Key next = 0;
				bool found = false;
				for (Key n : neighbours(cur))
					if (!used.count(n) && !(havePrev && n == prev)) {
						next = n;
						found = true;
						break;
					}
				if (!found) break;
				prev = cur;
				havePrev = true;
				cur = next;
			}
			return c;
		};
		// (map order: the same chains whatever the hash does)
		std::map<Key, int> order;
		for (const auto& kv : _adj) order.emplace(kv.first, int(kv.second.size()));
		for (const auto& kv : order)
			if (kv.second == 1 && !used.count(kv.first)) out.push_back(walk(kv.first, false));
		for (const auto& kv : order)
			if (!used.count(kv.first)) out.push_back(walk(kv.first, true));
		return out;
	}

private:
	const EdgePoint& edge(int i, int j, int axis) {
		const Key k = edgeKey(i, j, axis);
		auto it = _points.find(k);
		if (it != _points.end()) return it->second;
		const int i2 = axis == 0 ? i + 1 : i, j2 = axis == 0 ? j : j + 1;
		const Corner &a = corner(i, j), &b = corner(i2, j2);
		float ga[2], gb[2];
		gradient(i, j, ga[0], ga[1]);
		gradient(i2, j2, gb[0], gb[1]);
		const float da = a.depth.d, db = b.depth.d;
		const float t = std::clamp(da / (da - db), 0.0f, 1.0f);
		EdgePoint p;
		p.x = (float(i) + (axis == 0 ? t : 0.0f)) * _cell;
		p.z = (float(j) + (axis == 1 ? t : 0.0f)) * _cell;
		p.level = a.depth.level + (b.depth.level - a.depth.level) * t;
		const float gx = ga[0] + (gb[0] - ga[0]) * t, gz = ga[1] + (gb[1] - ga[1]) * t;
		const float len = std::sqrt(gx * gx + gz * gz);
		p.slope = len;
		p.nx = len > 1e-6f ? gx / len : 1.0f;
		p.nz = len > 1e-6f ? gz / len : 0.0f;
		return _points.emplace(k, p).first->second;
	}

	void link(Key a, Key b) {
		_adj[a].push_back(b);
		_adj[b].push_back(a);
	}

	void cell(int i, int j) {
		const Corner &c00 = corner(i, j), &c10 = corner(i + 1, j), &c11 = corner(i + 1, j + 1), &c01 = corner(i, j + 1);
		if (!c00.depth.water || !c10.depth.water || !c11.depth.water || !c01.depth.water) return;
		const int mask = (c00.depth.d > 0 ? 1 : 0) | (c10.depth.d > 0 ? 2 : 0) | (c11.depth.d > 0 ? 4 : 0) | (c01.depth.d > 0 ? 8 : 0);
		if (mask == 0 || mask == 15) return;
		// The edges: 0 bottom (x-edge at i, j), 1 right (z-edge at i + 1, j), 2 top (x-edge at i, j + 1), 3 left (z-edge at i, j).
		const auto key = [&](int e) {
			switch (e) {
			case 0: return edgeKey(i, j, 0);
			case 1: return edgeKey(i + 1, j, 1);
			case 2: return edgeKey(i, j + 1, 0);
			default: return edgeKey(i, j, 1);
			}
		};
		const auto make = [&](int e) {
			switch (e) {
			case 0: edge(i, j, 0); break;
			case 1: edge(i + 1, j, 1); break;
			case 2: edge(i, j + 1, 0); break;
			default: edge(i, j, 1); break;
			}
		};
		static const int kSeg[16][4] = {
		    {-1, -1, -1, -1}, {3, 0, -1, -1}, {0, 1, -1, -1}, {3, 1, -1, -1}, {1, 2, -1, -1}, {-2, -2, -2, -2}, {0, 2, -1, -1}, {3, 2, -1, -1},
		    {2, 3, -1, -1},   {0, 2, -1, -1}, {-3, -3, -3, -3}, {1, 2, -1, -1}, {3, 1, -1, -1}, {0, 1, -1, -1}, {3, 0, -1, -1}, {-1, -1, -1, -1}};
		int seg[4] = {kSeg[mask][0], kSeg[mask][1], kSeg[mask][2], kSeg[mask][3]};
		if (mask == 5 || mask == 10) {
			const bool centre = 0.25f * (c00.depth.d + c10.depth.d + c11.depth.d + c01.depth.d) > 0.0f;
			if (mask == 5) { // c00 and c11 in the water
				const int s[4] = {centre ? 0 : 3, centre ? 1 : 0, centre ? 2 : 1, centre ? 3 : 2};
				std::copy(s, s + 4, seg);
			} else { // c10 and c01 in the water
				const int s[4] = {centre ? 3 : 0, centre ? 0 : 1, centre ? 1 : 2, centre ? 2 : 3};
				std::copy(s, s + 4, seg);
			}
		}
		for (int s = 0; s + 1 < 4 && seg[s] >= 0; s += 2) {
			make(seg[s]);
			make(seg[s + 1]);
			link(key(seg[s]), key(seg[s + 1]));
		}
	}

	const DepthField& _field;
	float _cell;
	std::unordered_map<Key, Corner> _corners;
	std::unordered_map<Key, EdgePoint> _points;
	std::unordered_map<Key, std::vector<Key>> _adj;
};

float hash01(float x) {
	const float s = std::sin(x * 12.9898f) * 43758.5453f;
	return s - std::floor(s);
}

// ---- foam strips ----
void emitFoam(const ShoreFinder::Chain& chain, int chainId, const WaterFxOptions& opt, u32 material, std::vector<SrcTriangle>& out) {
	const size_t n = chain.p.size();
	if (n < 3) return;
	struct Pt {
		float ax, az, bx, bz, y, u;
	};
	std::vector<Pt> pts(n);
	// Smoothed normals (the field's are noisy at a 1 m lattice), the running length for u.
	float run = 0.0f;
	for (size_t k = 0; k < n; ++k) {
		const EdgePoint& p = chain.p[k];
		float nx = p.nx, nz = p.nz;
		for (int d = -2; d <= 2; ++d) {
			if (!d) continue;
			const long idx = long(k) + d;
			if (idx < 0 || idx >= long(n)) continue;
			nx += chain.p[size_t(idx)].nx * (d == -1 || d == 1 ? 0.7f : 0.35f);
			nz += chain.p[size_t(idx)].nz * (d == -1 || d == 1 ? 0.7f : 0.35f);
		}
		const float len = std::sqrt(nx * nx + nz * nz);
		if (len > 1e-6f) {
			nx /= len;
			nz /= len;
		}
		if (k) run += std::hypot(p.x - chain.p[k - 1].x, p.z - chain.p[k - 1].z);
		const float u = run / kFoamPeriod;
		// Gentle beaches get a wide band of foam, steep banks a narrow one: it reaches to where the water is ~12 cm deep.
		const float reach = std::clamp(12.0f / std::max(p.slope, 0.02f), 0.5f * opt.foamWidth, 2.2f * opt.foamWidth);
		const float wobble = 0.8f + 0.4f * hash01(u * 3.7f + float(chainId) * 1.3f);
		const float w = reach * wobble;
		pts[k] = {p.x - nx * opt.foamBank, p.z - nz * opt.foamBank, p.x + nx * w, p.z + nz * w, p.level + kFoamLift, u};
	}
	auto vertex = [&](float x, float y, float z, float u, float v) {
		SrcVertex s {};
		s.pos[0] = x;
		s.pos[1] = y;
		s.pos[2] = z;
		s.uv[0] = u;
		s.uv[1] = v;
		s.rgba[0] = s.rgba[1] = s.rgba[2] = 255;
		s.rgba[3] = 255;
		return s;
	};
	for (size_t k = 0; k + 1 < n; ++k) {
		const Pt &p = pts[k], &q = pts[k + 1];
		SrcVertex a0 = vertex(p.ax, p.y, p.az, p.u, 0.0f), b0 = vertex(p.bx, p.y, p.bz, p.u, 1.0f);
		SrcVertex a1 = vertex(q.ax, q.y, q.az, q.u, 0.0f), b1 = vertex(q.bx, q.y, q.bz, q.u, 1.0f);
		SrcTriangle t1 {}, t2 {};
		t1.material = t2.material = material;
		t1.v[0] = a0;
		t1.v[1] = b0;
		t1.v[2] = a1;
		t2.v[0] = b0;
		t2.v[1] = b1;
		t2.v[2] = a1;
		for (SrcTriangle* t : {&t1, &t2}) {
			// Front faces look up: -cross(b - a, c - a).y > 0 (the left-handed lattice of the baker).
			const float ax = t->v[1].pos[0] - t->v[0].pos[0], az = t->v[1].pos[2] - t->v[0].pos[2];
			const float bx = t->v[2].pos[0] - t->v[0].pos[0], bz = t->v[2].pos[2] - t->v[0].pos[2];
			const float ny = -(az * bx - ax * bz);
			if (std::fabs(ny) < 1e-3f) continue;
			if (ny < 0.0f) std::swap(t->v[1], t->v[2]);
			out.push_back(*t);
		}
	}
}

// ---- rivers ----
struct Axis {
	float fx = 1.0f, fz = 0.0f; // unit, along the channel (either way)
	float evidence = 0.0f;      // bed rise per cm along (fx, fz): > 0: the water runs the other way
	bool river = false;
};

float lerpf(float a, float b, float t) { return a + (b - a) * t; }

SrcVertex lerpVertex(const SrcVertex& a, const SrcVertex& b, float t) {
	SrcVertex r = a;
	for (int i = 0; i < 3; ++i) r.pos[i] = lerpf(a.pos[i], b.pos[i], t);
	for (int i = 0; i < 2; ++i) r.uv[i] = lerpf(a.uv[i], b.uv[i], t);
	for (int i = 0; i < 4; ++i) r.rgba[i] = u8(std::lround(lerpf(float(a.rgba[i]), float(b.rgba[i]), t)));
	r.occlusion = u8(std::lround(lerpf(float(a.occlusion), float(b.occlusion), t)));
	return r;
}

void clipPolygon(std::vector<SrcVertex>& poly, int axis, float at, bool keepAbove) {
	std::vector<SrcVertex> out;
	const size_t n = poly.size();
	for (size_t i = 0; i < n; ++i) {
		const SrcVertex& a = poly[i];
		const SrcVertex& b = poly[(i + 1) % n];
		const float da = keepAbove ? a.pos[axis] - at : at - a.pos[axis];
		const float db = keepAbove ? b.pos[axis] - at : at - b.pos[axis];
		const bool ia = da >= 0.0f, ib = db >= 0.0f;
		if (ia) out.push_back(a);
		if (ia != ib) {
			SrcVertex c = lerpVertex(a, b, da / (da - db));
			c.pos[axis] = at;
			out.push_back(c);
		}
	}
	poly.swap(out);
}

// The gradient of u and v over (x, z) of a triangle: rows (du/dx, du/dz), (dv/dx, dv/dz). False for a sliver.
bool uvJacobian(const SrcTriangle& t, float j[2][2]) {
	const float d1x = t.v[1].pos[0] - t.v[0].pos[0], d1z = t.v[1].pos[2] - t.v[0].pos[2];
	const float d2x = t.v[2].pos[0] - t.v[0].pos[0], d2z = t.v[2].pos[2] - t.v[0].pos[2];
	const float det = d1x * d2z - d2x * d1z;
	if (std::fabs(det) < 1e-2f) return false;
	for (int c = 0; c < 2; ++c) {
		const float d1 = t.v[1].uv[c] - t.v[0].uv[c], d2 = t.v[2].uv[c] - t.v[0].uv[c];
		j[c][0] = (d1 * d2z - d2 * d1z) / det;
		j[c][1] = (d2 * d1x - d1 * d2x) / det;
	}
	return true;
}

} // namespace

SrcImage makeGlintTexture(int w, int h) {
	SrcImage img;
	img.width = w;
	img.height = h;
	img.rgba.assign(size_t(w) * size_t(h) * 4, 0);
	// Sparkles: a hashed point in most 16 x 16 texel cells, soft round, of varying strength; wrapped, so it tiles.
	const int cell = 16, cx = w / cell, cy = h / cell;
	for (int gy = 0; gy < cy; ++gy)
		for (int gx = 0; gx < cx; ++gx) {
			const float r = hash01(float(gx) * 7.31f + float(gy) * 19.17f + 1.7f);
			if (r < 0.35f) continue;
			const float px = (float(gx) + 0.2f + 0.6f * hash01(r * 91.3f)) * float(cell), py = (float(gy) + 0.2f + 0.6f * hash01(r * 57.9f)) * float(cell);
			const float power = 0.4f + 0.6f * hash01(r * 13.1f);
			for (int dy = -4; dy <= 4; ++dy)
				for (int dx = -4; dx <= 4; ++dx) {
					const float d2 = float(dx * dx + dy * dy);
					// a bright core and a thin cross, as a sun glint looks
					const float core = std::exp(-d2 / 2.2f);
					const float cross = (dx == 0 || dy == 0) ? std::exp(-d2 / 7.0f) * 0.5f : 0.0f;
					const float a = std::clamp((core + cross) * power, 0.0f, 1.0f);
					const int x = (int(px) + dx + w) % w, y = (int(py) + dy + h) % h;
					unsigned char* p = &img.rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
					const unsigned char v = (unsigned char)std::max<int>(p[0], int(a * 255.0f + 0.5f));
					p[0] = p[1] = v;
					p[2] = (unsigned char)std::min(255, int(v) + 6);
					p[3] = 255;
				}
		}
	return img;
}

SrcImage makeFoamTexture(int w, int h, int frame, int frames) {
	SrcImage img;
	img.width = w;
	img.height = h;
	img.rgba.assign(size_t(w) * size_t(h) * 4, 0);
	// The lace is value noise over (u, v, time), periodic in u and in time (so the strip tiles along the shore and the
	// animation loops), a few octaves; it is where the noise lies over a threshold that rises towards the open water. The
	// frames are the lace moving: its noise slides through time, and the whole edge breathes in and out (the swash of a wave:
	// the foam's front goes out and comes back) - so the shore is not a still picture.
	const float phase = frames > 1 ? float(frame) / float(frames) : 0.0f; // 0..1 round the loop
	const float theta = 6.2831853f * phase;
	auto lattice = [&](int ix, int iy, int it, int periodU, int periodT) {
		const int x = ((ix % periodU) + periodU) % periodU, t = ((it % periodT) + periodT) % periodT;
		return hash01(float(x) * 12.7f + float(iy) * 78.2f + float(t) * 31.9f + 3.1f);
	};
	auto noise = [&](float u, float v, int cellsU, int cellsV, int cellsT) {
		const float x = u * float(cellsU), y = v * float(cellsV), t = phase * float(cellsT);
		const int ix = int(std::floor(x)), iy = int(std::floor(y)), it = int(std::floor(t));
		const float fx = x - float(ix), fy = y - float(iy), ft = t - float(it);
		const float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy), st = ft * ft * (3.0f - 2.0f * ft);
		auto plane = [&](int tt) {
			const float a = lattice(ix, iy, tt, cellsU, cellsT), b = lattice(ix + 1, iy, tt, cellsU, cellsT);
			const float c = lattice(ix, iy + 1, tt, cellsU, cellsT), d = lattice(ix + 1, iy + 1, tt, cellsU, cellsT);
			return lerpf(lerpf(a, b, sx), lerpf(c, d, sx), sy);
		};
		return lerpf(plane(it), plane(it + 1), st);
	};
	const float swash = 0.5f + 0.5f * std::sin(theta); // 0..1: the front out .. back
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x) {
			const float u = (float(x) + 0.5f) / float(w);
			// v: 0 = on the bank (hidden by the ground), 0.3 the water's edge, 1 the open water. The edge moves with the swash.
			const float v0 = (float(y) + 0.5f) / float(h);
			const float v = v0 - 0.07f * (swash - 0.5f);
			const float n = 0.55f * noise(u, v, 8, 6, 2) + 0.30f * noise(u, v, 16, 12, 3) + 0.15f * noise(u, v, 32, 24, 4);
			const float lead = std::clamp(v0 / 0.16f, 0.0f, 1.0f);              // fades in from the bank
			const float reach = std::clamp((v - 0.3f) / 0.7f, 0.0f, 1.0f);     // 0 at the edge .. 1 far out
			const float threshold = 0.10f + 0.80f * reach - 0.10f * swash * reach; // the lace gets thinner outwards, thicker as the front comes
			const float lace = std::clamp((n - threshold) / 0.10f + 0.5f, 0.0f, 1.0f);
			const float bump = (v - 0.30f) / 0.10f;
			const float edge = std::exp(-bump * bump) * (0.45f + 0.5f * n);      // a broken line of foam right at the edge
			float a = std::max(lace, edge) * lead;
			a *= 1.0f - std::clamp((v0 - 0.78f) / 0.16f, 0.0f, 1.0f); // nothing at the outer edge
			a *= 0.75f + 0.25f * swash;                                // and a little fainter when it draws back
			unsigned char* p = &img.rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
			p[0] = 244;
			p[1] = 250;
			p[2] = 252;
			p[3] = (unsigned char)std::clamp(a * 235.0f + 0.5f, 0.0f, 255.0f);
		}
	return img;
}

bool makeFoamFrame(const std::string& name, SrcImage& img) {
	std::string up;
	for (char c : name) up += char(std::toupper((unsigned char)c));
	// (a path or not: the name is the file's)
	const size_t slash = up.find_last_of("/\\");
	if (slash != std::string::npos) up.erase(0, slash + 1);
	if (up.compare(0, 9, "G3_FOAM_A") != 0 || up.size() < 14) return false;
	const std::string num = up.substr(9, up.size() - 9 - 4);
	if (up.substr(up.size() - 4) != ".TGA" || num.empty() || num.size() > 2) return false;
	for (char c : num) if (c < '0' || c > '9') return false;
	const int k = std::atoi(num.c_str());
	if (k < 0 || k >= kFoamFrames) return false;
	img = makeFoamTexture(128, 64, k, kFoamFrames);
	return true;
}

void bakeWaterFx(SrcScene& scene, const WaterFxOptions& opt, WaterFxStats& stats) {
	const bool wantFoam = opt.foam > 0.0f, wantFlow = opt.flow > 0.0f;
	if (!wantFoam && !wantFlow) return;
	// The land's box: past it the water is open sea and not looked at.
	float lo3[2] = {kInf, kInf}, hi3[2] = {-kInf, -kInf};
	bool anyWater = false;
	for (const SrcTriangle& t : scene.triangles) {
		if (isWaterTri(scene, t)) {
			anyWater = true;
			continue;
		}
		for (const SrcVertex& v : t.v) {
			lo3[0] = std::min(lo3[0], v.pos[0]);
			hi3[0] = std::max(hi3[0], v.pos[0]);
			lo3[1] = std::min(lo3[1], v.pos[2]);
			hi3[1] = std::max(hi3[1], v.pos[2]);
		}
	}
	if (!anyWater || lo3[0] > hi3[0]) return;
	int lo[2], hi[2];
	for (int a = 0; a < 2; ++a) {
		lo[a] = tileOf(lo3[a]) - 1;
		hi[a] = tileOf(hi3[a]) + 1;
		if (hi[a] - lo[a] > 4000) return;
	}
	WaterLevels levels(scene, lo, hi);
	GroundSampler ground;
	ground.build(scene, scene.triangles.size(), true);
	DepthField field(levels, ground);
	// Shore tiles: ground or an object that crosses the water's level there.
	std::unordered_set<Key> shore;
	for (const SrcTriangle& t : scene.triangles) {
		if (isWaterTri(scene, t)) continue;
		float mn[3] = {kInf, kInf, kInf}, mx[3] = {-kInf, -kInf, -kInf};
		for (const SrcVertex& v : t.v)
			for (int a = 0; a < 3; ++a) {
				mn[a] = std::min(mn[a], v.pos[a]);
				mx[a] = std::max(mx[a], v.pos[a]);
			}
		const int x0 = tileOf(mn[0]), x1 = tileOf(mx[0]), z0 = tileOf(mn[2]), z1 = tileOf(mx[2]);
		if (x1 - x0 > 64 || z1 - z0 > 64 || x1 < lo[0] || x0 > hi[0] || z1 < lo[1] || z0 > hi[1]) continue;
		for (int ix = x0; ix <= x1; ++ix)
			for (int iz = z0; iz <= z1; ++iz) {
				float l0, l1;
				if (levels.levelRange(ix, iz, l0, l1) && mx[1] >= l0 - kSlack && mn[1] <= l1 + kSlack) shore.insert(tkey(ix, iz));
			}
	}
	stats.shoreTiles = u32(shore.size());
	if (shore.empty()) return;
	std::vector<Key> shoreList(shore.begin(), shore.end());
	std::sort(shoreList.begin(), shoreList.end());

	// ---- shore lines (before anything of the water's triangles is changed: the levels look them up by index) ----
	std::vector<ShoreFinder::Chain> foamChains;
	if (wantFoam) {
		ShoreFinder finder(field, opt.cell);
		for (Key k : shoreList) {
			int ix, iz;
			untkey(k, ix, iz);
			finder.tile(ix, iz);
		}
		foamChains = finder.chains();
		// Narrow water gets no foam: puddles, ponds and rivers. Across the shore (the way its normal points, into the water)
		// the open water must go on for `foamMinWidth`; a chain where that fails at half of its points is left bare.
		if (opt.foamMinWidth > 0.0f) {
			std::vector<ShoreFinder::Chain> wide;
			for (ShoreFinder::Chain& c : foamChains) {
				u32 tested = 0, narrow = 0;
				for (size_t k = 0; k < c.p.size(); k += 5) {
					const EdgePoint& e = c.p[k];
					float run = 0.0f;
					while (run < opt.foamMinWidth) {
						const Depth d = field.at(e.x + e.nx * (run + 100.0f), e.z + e.nz * (run + 100.0f));
						if (!(d.water && d.d > 0.0f)) break;
						run += 100.0f;
					}
					++tested;
					narrow += run < opt.foamMinWidth;
				}
				if (tested > 0 && narrow * 2 >= tested) {
					++stats.foamChainsNarrow;
					continue;
				}
				wide.push_back(std::move(c));
			}
			foamChains = std::move(wide);
		}
	}

	// ---- rivers ----
	if (wantFlow) {
		// The coarse raster of the shore tiles and their neighbours: water or not, and the bed.
		struct Cell {
			bool water = false;
			float bed = 0.0f;
		};
		const int rpt = int(std::lround(kTile / kRasterCell));
		std::unordered_map<Key, std::vector<Cell>> raster;
		auto rasterOf = [&](int ix, int iz) -> const std::vector<Cell>& {
			auto it = raster.find(tkey(ix, iz));
			if (it != raster.end()) return it->second;
			std::vector<Cell> cells(size_t(rpt) * size_t(rpt));
			if (levels.tileHasWater(ix, iz))
				for (int a = 0; a < rpt; ++a)
					for (int b = 0; b < rpt; ++b) {
						const Depth d = field.at((float(ix * rpt + a) + 0.5f) * kRasterCell, (float(iz * rpt + b) + 0.5f) * kRasterCell);
						Cell& c = cells[size_t(a) * size_t(rpt) + size_t(b)];
						c.water = d.water && d.d > 0.0f;
						c.bed = d.bed;
					}
			return raster.emplace(tkey(ix, iz), std::move(cells)).first->second;
		};
		std::unordered_map<Key, Axis> axes;
		for (Key k : shoreList) {
			int ix, iz;
			untkey(k, ix, iz);
			// Moments of the water cells within the tile and its neighbours.
			double n = 0, sx = 0, sz = 0, sxx = 0, sxz = 0, szz = 0;
			for (int dx = -1; dx <= 1; ++dx)
				for (int dz = -1; dz <= 1; ++dz) {
					const std::vector<Cell>& cells = rasterOf(ix + dx, iz + dz);
					for (int a = 0; a < rpt; ++a)
						for (int b = 0; b < rpt; ++b) {
							if (!cells[size_t(a) * size_t(rpt) + size_t(b)].water) continue;
							const double x = (double(a) + 0.5) * kRasterCell + double(dx) * kTile, z = (double(b) + 0.5) * kRasterCell + double(dz) * kTile;
							n += 1;
							sx += x;
							sz += z;
							sxx += x * x;
							sxz += x * z;
							szz += z * z;
						}
				}
			if (n < 30) continue;
			const double mx = sx / n, mz = sz / n;
			const double cxx = sxx / n - mx * mx, cxz = sxz / n - mx * mz, czz = szz / n - mz * mz;
			const double tr = cxx + czz, det = cxx * czz - cxz * cxz;
			const double disc = std::sqrt(std::max(0.0, tr * tr / 4.0 - det));
			const double l1 = tr / 2.0 + disc, l2 = std::max(tr / 2.0 - disc, 1e-3);
			const double length = std::sqrt(12.0 * l1), width = std::sqrt(12.0 * l2);
			// A river: long in the window (it runs through), narrow, and much longer than wide.
			if (length < 0.45 * 3.0 * double(kTile) || width > double(opt.riverMaxWidth) || l1 / l2 < 4.0) continue;
			double fx, fz;
			if (std::fabs(cxz) > 1e-9) {
				fx = l1 - czz;
				fz = cxz;
			} else if (cxx >= czz) {
				fx = 1.0;
				fz = 0.0;
			} else {
				fx = 0.0;
				fz = 1.0;
			}
			const double fl = std::hypot(fx, fz);
			fx /= fl;
			fz /= fl;
			// The bed along the channel: regression of its height on the place along the axis.
			double sbs = 0, sss = 0;
			for (int dx = -1; dx <= 1; ++dx)
				for (int dz = -1; dz <= 1; ++dz) {
					const std::vector<Cell>& cells = rasterOf(ix + dx, iz + dz);
					for (int a = 0; a < rpt; ++a)
						for (int b = 0; b < rpt; ++b) {
							const Cell& c = cells[size_t(a) * size_t(rpt) + size_t(b)];
							if (!c.water) continue;
							const double x = (double(a) + 0.5) * kRasterCell + double(dx) * kTile - mx, z = (double(b) + 0.5) * kRasterCell + double(dz) * kTile - mz;
							const double s = x * fx + z * fz;
							sbs += s * c.bed;
							sss += s * s;
						}
				}
			Axis ax;
			ax.fx = float(fx);
			ax.fz = float(fz);
			ax.river = true;
			ax.evidence = sss > 0 ? float(sbs / sss) : 0.0f; // (the s are centred on the cells' mean: their sum is 0)
			axes.emplace(k, ax);
		}
		stats.riverTiles = u32(axes.size());
		if (!axes.empty()) {
			// Which way is down. First the axes of neighbouring tiles are turned to agree (a flood fill: an axis is a line, it has
			// no way of its own), then the bed's slope, added up over the stretch of river round a tile, says which way the
			// water runs along that agreed direction. (A single tile's slope is noisy: a pond beside the river, a rock.)
			std::vector<Key> rivers;
			for (const auto& kv : axes) rivers.push_back(kv.first);
			std::sort(rivers.begin(), rivers.end());
			std::unordered_map<Key, float> turn; // +1 / -1: the axis as it is / turned round
			std::unordered_map<Key, int> component;
			int components = 0;
			for (Key start : rivers) {
				if (turn.count(start)) continue;
				const int id = components++;
				std::vector<Key> stack {start};
				turn[start] = 1.0f;
				component[start] = id;
				while (!stack.empty()) {
					const Key k = stack.back();
					stack.pop_back();
					int ix, iz;
					untkey(k, ix, iz);
					const Axis& a = axes[k];
					for (int dx = -1; dx <= 1; ++dx)
						for (int dz = -1; dz <= 1; ++dz) {
							const Key nk = tkey(ix + dx, iz + dz);
							auto it = axes.find(nk);
							if (it == axes.end() || turn.count(nk)) continue;
							const float along = (a.fx * it->second.fx + a.fz * it->second.fz) * turn[k];
							turn[nk] = along < 0.0f ? -1.0f : 1.0f;
							component[nk] = id;
							stack.push_back(nk);
						}
				}
			}
			std::unordered_map<Key, float> sign;
			const int reach = 5; // tiles
			for (Key k : rivers) {
				int ix, iz;
				untkey(k, ix, iz);
				double sum = 0.0;
				for (int dx = -reach; dx <= reach; ++dx)
					for (int dz = -reach; dz <= reach; ++dz) {
						const Key nk = tkey(ix + dx, iz + dz);
						auto it = axes.find(nk);
						if (it == axes.end() || component[nk] != component[k]) continue;
						// (the rise of the bed along the agreed direction)
						sum += double(it->second.evidence) * double(turn[nk]);
					}
				// Rising along the agreed direction: the water runs the other way. Flat: the agreed direction.
				const float downhill = sum > 1e-4 ? -1.0f : 1.0f;
				sign[k] = downhill * turn[k];
			}
			if (std::getenv("G3_WATERFX_DEBUG"))
				for (Key k : rivers) {
					int ix, iz;
					untkey(k, ix, iz);
					std::fprintf(stderr, "river tile %d %d axis %.2f %.2f evidence %.5f sign %.0f\n", ix, iz, double(axes[k].fx), double(axes[k].fz),
					             double(axes[k].evidence), double(sign[k]));
				}
			for (Key k : rivers) {
				const float fx = axes[k].fx * sign[k], fz = axes[k].fz * sign[k];
				++stats.flowTiles[(int(std::lround(std::atan2(fz, fx) / (kPi / 4.0f))) + 8) % 8];
			}
			// The copies of the water material: one per original, per direction (eight of them), per scroll of its texture.
			struct Variant {
				u32 material;
				int bin;
				int su, sv; // the scroll, in 1/4096 repeats a second
				bool operator<(const Variant& o) const {
					return std::tie(material, bin, su, sv) < std::tie(o.material, o.bin, o.su, o.sv);
				}
			};
			std::map<Variant, u32> variants;
			const size_t count = scene.triangles.size();
			std::vector<SrcTriangle> added;
			for (size_t ti = 0; ti < count; ++ti) {
				const SrcTriangle src = scene.triangles[ti];
				if (!isWaterTri(scene, src)) continue;
				float mn[3] = {kInf, kInf, kInf}, mx3[3] = {-kInf, -kInf, -kInf};
				for (const SrcVertex& v : src.v)
					for (int a = 0; a < 3; ++a) {
						mn[a] = std::min(mn[a], v.pos[a]);
						mx3[a] = std::max(mx3[a], v.pos[a]);
					}
				const int x0 = tileOf(mn[0]), x1 = tileOf(mx3[0]), z0 = tileOf(mn[2]), z1 = tileOf(mx3[2]);
				if (double(x1 - x0 + 1) * double(z1 - z0 + 1) > 256.0) continue; // (the sea's big pieces)
				bool touches = false;
				for (int ix = x0; ix <= x1 && !touches; ++ix)
					for (int iz = z0; iz <= z1; ++iz)
						if (axes.count(tkey(ix, iz))) {
							touches = true;
							break;
						}
				if (!touches) continue;
				float jac[2][2];
				if (!uvJacobian(src, jac)) continue;
				bool first = true;
				for (int ix = x0; ix <= x1; ++ix)
					for (int iz = z0; iz <= z1; ++iz) {
						std::vector<SrcVertex> poly(src.v, src.v + 3);
						clipPolygon(poly, 0, float(ix) * kTile, true);
						if (poly.size() >= 3) clipPolygon(poly, 0, float(ix + 1) * kTile, false);
						if (poly.size() >= 3) clipPolygon(poly, 2, float(iz) * kTile, true);
						if (poly.size() >= 3) clipPolygon(poly, 2, float(iz + 1) * kTile, false);
						if (poly.size() < 3) continue;
						u32 material = src.material;
						auto ax = axes.find(tkey(ix, iz));
						if (ax != axes.end()) {
							const float s = sign[ax->first];
							const float fx = ax->second.fx * s, fz = ax->second.fz * s;
							const int bin = int(std::lround(std::atan2(fz, fx) / (kPi / 4.0f)) + 8) % 8;
							const float ang = float(bin) * (kPi / 4.0f);
							const float dx = std::cos(ang), dz = std::sin(ang);
							// The pattern moves against the scroll: by -J^-1 scroll. To make it go (dx, dz) cm a second the
							// scroll is -J (dx, dz) speed.
							const float scrollU = -(jac[0][0] * dx + jac[0][1] * dz) * opt.flow;
							const float scrollV = -(jac[1][0] * dx + jac[1][1] * dz) * opt.flow;
							Variant v {src.material, bin, int(std::lround(scrollU * 4096.0f)), int(std::lround(scrollV * 4096.0f))};
							auto it = variants.find(v);
							if (it == variants.end()) {
								SrcMaterial m = scene.materials[src.material];
								m.name += "_FLOW" + std::to_string(bin);
								m.scroll[0] = float(v.su) / 4096.0f;
								m.scroll[1] = float(v.sv) / 4096.0f;
								it = variants.emplace(v, u32(scene.materials.size())).first;
								scene.materials.push_back(m);
							}
							material = it->second;
						}
						for (size_t i = 1; i + 1 < poly.size(); ++i) {
							SrcTriangle piece = src;
							piece.material = material;
							piece.v[0] = poly[0];
							piece.v[1] = poly[i];
							piece.v[2] = poly[i + 1];
							const float ax1 = piece.v[1].pos[0] - piece.v[0].pos[0], az1 = piece.v[1].pos[2] - piece.v[0].pos[2];
							const float bx1 = piece.v[2].pos[0] - piece.v[0].pos[0], bz1 = piece.v[2].pos[2] - piece.v[0].pos[2];
							if (std::fabs(ax1 * bz1 - az1 * bx1) < 1.0f) continue; // (a sliver)
							if (material != src.material) ++stats.flowTriangles;
							if (first) {
								scene.triangles[ti] = piece;
								first = false;
							} else {
								added.push_back(piece);
							}
						}
					}
			}
			stats.flowMaterials = u32(variants.size());
			scene.triangles.insert(scene.triangles.end(), added.begin(), added.end());
		}
	}

	// ---- shore foam: the strips, after the water's materials (and so drawn after them) ----
	if (wantFoam) {
		SrcMaterial foam;
		foam.name = "G3_SHORE_FOAM";
		foam.texture = kFoamTexture;
		foam.animFps = 4.0f; // (the frames of the lace, 8 of them: a loop of two seconds)
		foam.blend = SrcBlend::Alpha;
		foam.opacity = u8(std::lround(255.0f * std::min(opt.foam, 1.0f)));
		foam.noCollision = true;
		foam.hasColor = true;
		foam.color[0] = foam.color[1] = foam.color[2] = 255;
		const u32 material = u32(scene.materials.size());
		const size_t before = scene.triangles.size();
		int id = 0;
		for (const ShoreFinder::Chain& c : foamChains) {
			if (c.p.size() < 3) continue;
			float len = 0.0f;
			for (size_t k = 1; k < c.p.size(); ++k) len += std::hypot(c.p[k].x - c.p[k - 1].x, c.p[k].z - c.p[k - 1].z);
			if (len < 200.0f) continue; // (a puddle's edge, a stone in the water)
			emitFoam(c, id++, opt, material, scene.triangles);
			++stats.shoreChains;
			stats.shoreLength += u64(len);
		}
		if (scene.triangles.size() > before) {
			scene.materials.push_back(foam);
			stats.foamTriangles = scene.triangles.size() - before;
		}
	}

	// ---- sun glints: a copy of the water's triangles in an additive material of sparkles (drawn after the water) ----
	if (opt.glint > 0.0f) {
		SrcMaterial g;
		g.name = "G3_WATER_GLINT";
		g.texture = kGlintTexture;
		g.blend = SrcBlend::Add;
		g.opacity = u8(std::lround(255.0f * std::min(opt.glint, 1.0f)));
		g.noCollision = true;
		g.hasColor = true;
		g.color[0] = g.color[1] = g.color[2] = 255;
		g.scroll[0] = 0.010f;
		g.scroll[1] = 0.006f;
		const u32 material = u32(scene.materials.size());
		const size_t count = scene.triangles.size();
		std::vector<SrcTriangle> copies;
		for (size_t ti = 0; ti < count; ++ti) {
			const SrcTriangle& src = scene.triangles[ti];
			if (!isWaterTri(scene, src)) continue;
			SrcTriangle t = src;
			t.material = material;
			for (SrcVertex& v : t.v) {
				// (a different scale from the water's own pattern: a sparkle every ~2 m; white vertex colour)
				v.uv[0] = v.pos[0] / 600.0f;
				v.uv[1] = v.pos[2] / 600.0f;
				v.rgba[0] = v.rgba[1] = v.rgba[2] = 255;
			}
			copies.push_back(t);
		}
		if (!copies.empty()) {
			scene.materials.push_back(g);
			scene.triangles.insert(scene.triangles.end(), copies.begin(), copies.end());
			stats.glintTriangles = copies.size();
		}
	}
}

} // namespace g3
