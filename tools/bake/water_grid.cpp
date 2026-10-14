#include "water_grid.h"

#include <algorithm>
#include <cmath>

namespace g3 {

namespace {

constexpr int kSuper = 8; // tiles on an edge of a super tile (coarse water is cut at these, not at every tile)
constexpr float kInf = 1e30f;

int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

SrcVertex lerpVertex(const SrcVertex& a, const SrcVertex& b, float t) {
	SrcVertex r = a;
	for (int i = 0; i < 3; ++i) r.pos[i] = a.pos[i] + (b.pos[i] - a.pos[i]) * t;
	for (int i = 0; i < 2; ++i) r.uv[i] = a.uv[i] + (b.uv[i] - a.uv[i]) * t;
	for (int i = 0; i < 4; ++i) r.rgba[i] = u8(std::lround(float(a.rgba[i]) + (float(b.rgba[i]) - float(a.rgba[i])) * t));
	r.occlusion = u8(std::lround(float(a.occlusion) + (float(b.occlusion) - float(a.occlusion)) * t));
	r.wind = 0;
	return r;
}

// Keeps the part of the convex polygon on one side of the line pos[axis] = at (Sutherland-Hodgman).
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
			c.pos[axis] = at; // (exactly on the line, so that both sides get the same corner)
			out.push_back(c);
		}
	}
	poly.swap(out);
}

float distToRect(float x, float z, float x0, float z0, float x1, float z1) {
	const float dx = x < x0 ? x0 - x : x > x1 ? x - x1 : 0.0f;
	const float dz = z < z0 ? z0 - z : z > z1 ? z - z1 : 0.0f;
	return std::sqrt(dx * dx + dz * dz);
}

bool isWater(const SrcScene& scene, const SrcTriangle& t) { return t.material < scene.materials.size() && scene.materials[t.material].water; }

} // namespace

int WaterGrid::tileOf(float v) const { return int(std::floor(v / _tile)); }

WaterGrid::WaterGrid(const SrcScene& scene, const WaterGridOptions& opt) : _opt(opt) {
	if (_opt.cell < 20.0f) return;
	_opt.cellsPerTile = std::clamp(_opt.cellsPerTile, 2, 64);
	_opt.reachTiles = std::max(_opt.reachTiles, 1);
	_tile = _opt.cell * float(_opt.cellsPerTile);
	_opt.fade = std::clamp(_opt.fade, 100.0f, _tile);
	// Where the lattice lies: over what is not water (the world), a margin more; outside it the outermost tiles are
	// unbounded (a sea reaching for hundreds of kilometres is not cut into millions of tiles).
	float lo[2] = {kInf, kInf}, hi[2] = {-kInf, -kInf};
	bool anyWater = false;
	for (const SrcTriangle& t : scene.triangles) {
		if (isWater(scene, t)) {
			anyWater = true;
			continue;
		}
		for (const SrcVertex& v : t.v) {
			lo[0] = std::min(lo[0], v.pos[0]);
			hi[0] = std::max(hi[0], v.pos[0]);
			lo[1] = std::min(lo[1], v.pos[2]);
			hi[1] = std::max(hi[1], v.pos[2]);
		}
	}
	if (!anyWater || lo[0] > hi[0]) return;
	for (int a = 0; a < 2; ++a) {
		_lo[a] = tileOf(lo[a]) - 2;
		_hi[a] = tileOf(hi[a]) + 2;
		if (_hi[a] - _lo[a] > 4000) return; // (a world this size is a mistake of the source)
	}
	// The water's level range per tile (over the tiles of each triangle's box).
	std::unordered_map<uint64_t, std::pair<float, float>> level;
	for (const SrcTriangle& t : scene.triangles) {
		if (!isWater(scene, t)) continue;
		float mn[3] = {kInf, kInf, kInf}, mx[3] = {-kInf, -kInf, -kInf};
		for (const SrcVertex& v : t.v)
			for (int a = 0; a < 3; ++a) {
				mn[a] = std::min(mn[a], v.pos[a]);
				mx[a] = std::max(mx[a], v.pos[a]);
			}
		const int x0 = clampTile(0, tileOf(mn[0])), x1 = clampTile(0, tileOf(mx[0]));
		const int z0 = clampTile(1, tileOf(mn[2])), z1 = clampTile(1, tileOf(mx[2]));
		for (int ix = x0; ix <= x1; ++ix)
			for (int iz = z0; iz <= z1; ++iz) {
				auto ins = level.emplace(key(ix, iz), std::make_pair(mn[1], mx[1]));
				if (!ins.second) {
					ins.first->second.first = std::min(ins.first->second.first, mn[1]);
					ins.first->second.second = std::max(ins.first->second.second, mx[1]);
				}
			}
	}
	// Shore tiles: ground or an object that crosses the water's level there.
	std::unordered_set<uint64_t> shore;
	const float slack = 50.0f;
	for (const SrcTriangle& t : scene.triangles) {
		if (isWater(scene, t)) continue;
		float mn[3] = {kInf, kInf, kInf}, mx[3] = {-kInf, -kInf, -kInf};
		for (const SrcVertex& v : t.v)
			for (int a = 0; a < 3; ++a) {
				mn[a] = std::min(mn[a], v.pos[a]);
				mx[a] = std::max(mx[a], v.pos[a]);
			}
		const int x0 = tileOf(mn[0]), x1 = tileOf(mx[0]), z0 = tileOf(mn[2]), z1 = tileOf(mx[2]);
		if (x1 - x0 > 64 || z1 - z0 > 64 || x1 < _lo[0] || x0 > _hi[0] || z1 < _lo[1] || z0 > _hi[1]) continue;
		for (int ix = x0; ix <= x1; ++ix)
			for (int iz = z0; iz <= z1; ++iz) {
				auto it = level.find(key(ix, iz));
				if (it != level.end() && mx[1] >= it->second.first - slack && mn[1] <= it->second.second + slack) shore.insert(key(ix, iz));
			}
	}
	_stats.shoreTiles = u32(shore.size());
	// The shore tiles and the tiles around them that have water.
	for (uint64_t k : shore) {
		const int ix = int(uint32_t(k >> 32)) - (1 << 30), iz = int(uint32_t(k & 0xFFFFFFFFu)) - (1 << 30);
		for (int dx = -_opt.reachTiles; dx <= _opt.reachTiles; ++dx)
			for (int dz = -_opt.reachTiles; dz <= _opt.reachTiles; ++dz) {
				const int jx = ix + dx, jz = iz + dz;
				if (jx < _lo[0] || jx > _hi[0] || jz < _lo[1] || jz > _hi[1] || !level.count(key(jx, jz))) continue;
				_fine.insert(key(jx, jz));
			}
	}
	for (uint64_t k : _fine) {
		const int ix = int(uint32_t(k >> 32)) - (1 << 30), iz = int(uint32_t(k & 0xFFFFFFFFu)) - (1 << 30);
		_fineSuper.insert(key(floorDiv(ix, kSuper), floorDiv(iz, kSuper)));
	}
	_stats.fineTiles = u32(_fine.size());
	for (const auto& kv : level) _water.insert(kv.first);
	_active = !_fine.empty();
}

float WaterGrid::weightAt(float x, float z) const {
	const int ix = tileOf(x), iz = tileOf(z);
	float best = _opt.fade;
	for (int dx = -1; dx <= 1; ++dx)
		for (int dz = -1; dz <= 1; ++dz) {
			if (fine(ix + dx, iz + dz) || !_water.count(key(ix + dx, iz + dz))) continue; // (only coarse water pulls the weight down)
			// (the unbounded outermost tiles are not special here: nothing is fine out there)
			const float d = distToRect(x, z, float(ix + dx) * _tile, float(iz + dz) * _tile, float(ix + dx + 1) * _tile, float(iz + dz + 1) * _tile);
			best = std::min(best, d);
		}
	return best / _opt.fade;
}

void WaterGrid::emit(const Poly& poly, bool dense, std::vector<SrcTriangle>& out, const SrcTriangle& proto) {
	for (size_t i = 1; i + 1 < poly.size(); ++i) {
		SrcTriangle piece = proto;
		piece.v[0] = poly[0];
		piece.v[1] = poly[i];
		piece.v[2] = poly[i + 1];
		// (A sliver of no area - three corners on a line - is of no use.)
		const float ax = piece.v[1].pos[0] - piece.v[0].pos[0], ay = piece.v[1].pos[1] - piece.v[0].pos[1],
		            az = piece.v[1].pos[2] - piece.v[0].pos[2];
		const float bx = piece.v[2].pos[0] - piece.v[0].pos[0], by = piece.v[2].pos[1] - piece.v[0].pos[1],
		            bz = piece.v[2].pos[2] - piece.v[0].pos[2];
		const float cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
		if (cx * cx + cy * cy + cz * cz < 1e-4f) continue;
		for (SrcVertex& v : piece.v) v.wind = dense ? u8(std::lround(127.0f * weightAt(v.pos[0], v.pos[2]))) : u8(0);
		out.push_back(piece);
	}
}

// One tile's part of a triangle, cut along the cell lines.
void WaterGrid::cutTile(const Poly& poly, int ix, int iz, std::vector<SrcTriangle>& out, const SrcTriangle& proto) {
	float mn[2] = {kInf, kInf}, mx[2] = {-kInf, -kInf};
	for (const SrcVertex& v : poly) {
		mn[0] = std::min(mn[0], v.pos[0]);
		mx[0] = std::max(mx[0], v.pos[0]);
		mn[1] = std::min(mn[1], v.pos[2]);
		mx[1] = std::max(mx[1], v.pos[2]);
	}
	const float cell = _opt.cell;
	// (the tile's own cell lines; the outermost tiles' pieces lie inside their lines' reach anyway)
	const int cx0 = int(std::floor(mn[0] / cell)), cx1 = int(std::floor(mx[0] / cell));
	const int cz0 = int(std::floor(mn[1] / cell)), cz1 = int(std::floor(mx[1] / cell));
	(void)ix;
	(void)iz;
	for (int cx = cx0; cx <= cx1; ++cx) {
		Poly column = poly;
		clipPolygon(column, 0, float(cx) * cell, true);
		if (column.size() >= 3) clipPolygon(column, 0, float(cx + 1) * cell, false);
		if (column.size() < 3) continue;
		for (int cz = cz0; cz <= cz1; ++cz) {
			Poly cellPoly = column;
			clipPolygon(cellPoly, 2, float(cz) * cell, true);
			if (cellPoly.size() >= 3) clipPolygon(cellPoly, 2, float(cz + 1) * cell, false);
			if (cellPoly.size() >= 3) emit(cellPoly, true, out, proto);
		}
	}
}

void WaterGrid::cut(const SrcTriangle& t, std::vector<SrcTriangle>& out) {
	++_stats.waterIn;
	const size_t before = out.size();
	Poly whole(t.v, t.v + 3);
	if (!_active) {
		emit(whole, false, out, t);
		_stats.waterOut += out.size() - before;
		return;
	}
	float mn[2] = {kInf, kInf}, mx[2] = {-kInf, -kInf};
	for (const SrcVertex& v : t.v) {
		mn[0] = std::min(mn[0], v.pos[0]);
		mx[0] = std::max(mx[0], v.pos[0]);
		mn[1] = std::min(mn[1], v.pos[2]);
		mx[1] = std::max(mx[1], v.pos[2]);
	}
	const int tx0 = clampTile(0, tileOf(mn[0])), tx1 = clampTile(0, tileOf(mx[0]));
	const int tz0 = clampTile(1, tileOf(mn[1])), tz1 = clampTile(1, tileOf(mx[1]));
	const int sx0 = floorDiv(tx0, kSuper), sx1 = floorDiv(tx1, kSuper), sz0 = floorDiv(tz0, kSuper), sz1 = floorDiv(tz1, kSuper);
	// The edges of a tile range: open towards the outside of the lattice.
	auto lowEdge = [&](int axis, int tile) { return tile <= _lo[axis] ? -kInf : float(tile) * _tile; };
	auto highEdge = [&](int axis, int tile) { return tile >= _hi[axis] ? kInf : float(tile + 1) * _tile; }; // (last tile of the range)
	auto clipRect = [&](Poly& p, int axis, int firstTile, int lastTile) {
		const int a = axis == 0 ? 0 : 2;
		const float lo = lowEdge(axis, firstTile), hi = highEdge(axis, lastTile);
		if (lo > -kInf) clipPolygon(p, a, lo, true);
		if (p.size() >= 3 && hi < kInf) clipPolygon(p, a, hi, false);
	};
	for (int sx = sx0; sx <= sx1; ++sx)
		for (int sz = sz0; sz <= sz1; ++sz) {
			// (the tiles of this super tile that the lattice has)
			const int fx = std::max(sx * kSuper, _lo[0]), lx = std::min(sx * kSuper + kSuper - 1, _hi[0]);
			const int fz = std::max(sz * kSuper, _lo[1]), lz = std::min(sz * kSuper + kSuper - 1, _hi[1]);
			if (fx > lx || fz > lz) continue;
			Poly sp = whole;
			clipRect(sp, 0, fx, lx);
			if (sp.size() >= 3) clipRect(sp, 1, fz, lz);
			if (sp.size() < 3) continue;
			if (!_fineSuper.count(key(sx, sz))) {
				emit(sp, false, out, t);
				continue;
			}
			for (int ix = fx; ix <= lx; ++ix) {
				for (int iz = fz; iz <= lz; ++iz) {
					Poly tp = sp;
					clipRect(tp, 0, ix, ix);
					if (tp.size() >= 3) clipRect(tp, 1, iz, iz);
					if (tp.size() < 3) continue;
					if (fine(ix, iz))
						cutTile(tp, ix, iz, out, t);
					else
						emit(tp, false, out, t);
				}
			}
		}
	if (out.size() == before) emit(whole, false, out, t); // (cut to nothing: keep it whole)
	_stats.waterOut += out.size() - before;
}

} // namespace g3
