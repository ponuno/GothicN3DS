#include "far_lod.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <unordered_map>

namespace g3 {

namespace {

using i64 = long long;

// A grid box: three cell coordinates packed into one key.
u64 boxKey(const float p[3], double cell) {
	u64 k = 0;
	for (int a = 0; a < 3; ++a) {
		const i64 c = i64(std::floor(double(p[a]) / cell)) + (i64(1) << 20);
		k = (k << 21) | u64(std::clamp<i64>(c, 0, (i64(1) << 21) - 1));
	}
	return k;
}

i64 floorDiv(i64 a, i64 b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// The corners of an input triangle as the far layer sees them: a cut-out
// (leaves) drawn there solid is shrunk towards its middle, so that a crown of
// leaf cards does not turn into a block.
struct Corners {
	float p[3][3];
};

Corners cornersOf(const SrcTriangle& t, const std::vector<float>& shrink) {
	Corners c;
	for (int v = 0; v < 3; ++v) std::memcpy(c.p[v], t.v[v].pos, sizeof(c.p[v]));
	const float s = t.material < shrink.size() ? shrink[t.material] : 1.0f;
	if (s < 1.0f) {
		for (int a = 0; a < 3; ++a) {
			const float mid = (c.p[0][a] + c.p[1][a] + c.p[2][a]) / 3.0f;
			for (int v = 0; v < 3; ++v) c.p[v][a] = mid + (c.p[v][a] - mid) * s;
		}
	}
	return c;
}

struct OutTri {
	u32 k[3];   // boxes (clusters)
	u32 src;    // source triangle
	int gx, gz; // chunk grid cell of the source triangle
	int tx, tz; // tile
	u32 ci;     // CellMap index of (gx, gz)
};

// Every included triangle reduced to its three boxes; collapsed ones dropped,
// repeated ones once (two faces of a thin wall stay: they face apart).
struct Box {
	double mn[3];
	double size;
	bool seam;
};

struct Clustering {
	std::vector<Box> boxes; // per cluster id
	std::vector<OutTri> tris;
	std::vector<u32> cornerBox; // per input triangle * 3: cluster id (for the quadrics)
};

// How coarse the far layer is where: a grid size per cell of the chunk grid
// (dense places finer), and one for the corners on the seams between cells.
struct Grid {
	double seam = 400.0;
	std::vector<double> size; // per cell (CellMap index)
};

// The chunk grid's cells that have anything of the far layer, numbered.
struct CellMap {
	std::map<std::pair<int, int>, u32> index;
	std::vector<std::pair<int, int>> cells;
	u32 of(int gx, int gz) {
		auto [it, fresh] = index.emplace(std::make_pair(gx, gz), u32(cells.size()));
		if (fresh) cells.push_back({gx, gz});
		return it->second;
	}
};

struct PairHash {
	size_t operator()(const std::pair<u64, u64>& p) const { return size_t(p.first * 0x9E3779B97F4A7C15ull ^ (p.second + (p.first >> 7))); }
};

// A corner's place to the centimetre (to tell the corners the source triangles share).
u64 spotKey(const float p[3]) {
	u64 k = 0;
	for (int a = 0; a < 3; ++a) {
		const i64 c = i64(std::lround(double(p[a]))) + (i64(1) << 20);
		k = (k << 21) | u64(std::clamp<i64>(c, 0, (i64(1) << 21) - 1));
	}
	return k;
}

// What does not change with the grid: each input triangle's grid cell (where
// the chunker put it: by centroid) and which of its corners lie on a seam -
// shared by triangles of different cells. Those are gathered apart from the
// rest of their box and stay on the seam, so that a cell of the far layer
// ends about where the chunk next to it does.
struct Seams {
	std::vector<std::pair<int, int>> cellOf; // per input triangle
	std::vector<u32> cellIndex;              // per input triangle: CellMap index
	std::vector<u8> onSeam;                  // per input triangle * 3
};

std::pair<int, int> cellOfTriangle(const SrcTriangle& t, float chunkSize) {
	const double cx = (double(t.v[0].pos[0]) + t.v[1].pos[0] + t.v[2].pos[0]) / 3.0;
	const double cz = (double(t.v[0].pos[2]) + t.v[1].pos[2] + t.v[2].pos[2]) / 3.0;
	return {int(std::floor(cx / chunkSize)), int(std::floor(cz / chunkSize))};
}

Seams findSeams(const SrcScene& scene, const std::vector<u32>& input, float chunkSize, CellMap& cells) {
	Seams sm;
	sm.cellOf.resize(input.size());
	sm.cellIndex.resize(input.size());
	for (size_t i = 0; i < input.size(); ++i) {
		sm.cellOf[i] = cellOfTriangle(scene.triangles[input[i]], chunkSize);
		sm.cellIndex[i] = cells.of(sm.cellOf[i].first, sm.cellOf[i].second);
	}
	std::unordered_map<u64, i64> spots; // corner -> its cell, or SEAM
	const i64 SEAM = i64(1) << 62;
	spots.reserve(input.size());
	for (size_t i = 0; i < input.size(); ++i) {
		const SrcTriangle& t = scene.triangles[input[i]];
		const i64 ck = (i64(sm.cellOf[i].first) << 32) ^ i64(u32(sm.cellOf[i].second));
		for (int v = 0; v < 3; ++v) {
			auto [it, fresh] = spots.emplace(spotKey(t.v[v].pos), ck);
			if (!fresh && it->second != ck) it->second = SEAM;
		}
	}
	sm.onSeam.resize(input.size() * 3);
	for (size_t i = 0; i < input.size(); ++i)
		for (int v = 0; v < 3; ++v) sm.onSeam[i * 3 + size_t(v)] = spots[spotKey(scene.triangles[input[i]].v[v].pos)] == SEAM;
	return sm;
}

void cluster(const SrcScene& scene, const std::vector<u32>& input, const Seams& sm, const Grid& grid, const std::vector<float>& shrink,
             Clustering& c) {
	c.boxes.clear();
	c.tris.clear();
	c.cornerBox.assign(input.size() * 3, 0);
	std::unordered_map<std::pair<u64, u64>, u32, PairHash> ids;
	ids.reserve(input.size());
	for (size_t i = 0; i < input.size(); ++i) {
		const SrcTriangle& t = scene.triangles[input[i]];
		const Corners cs = cornersOf(t, shrink);
		const u32 ci = sm.cellIndex[i];
		u32 k[3];
		for (int v = 0; v < 3; ++v) {
			// A corner on a seam is in a box of the seams' grid, shared by the
			// cells on both sides; any other belongs to its cell's own grid.
			const bool seam = sm.onSeam[i * 3 + size_t(v)] != 0;
			const double size = seam ? grid.seam : grid.size[ci];
			const std::pair<u64, u64> key {seam ? 0u : u64(ci) + 1u, boxKey(cs.p[v], size)};
			auto [it, fresh] = ids.emplace(key, u32(c.boxes.size()));
			if (fresh) {
				Box bx;
				for (int a = 0; a < 3; ++a) bx.mn[a] = std::floor(double(cs.p[v][a]) / size) * size;
				bx.size = size;
				bx.seam = seam;
				c.boxes.push_back(bx);
			}
			k[v] = it->second;
			c.cornerBox[i * 3 + size_t(v)] = k[v];
		}
		if (k[0] == k[1] || k[1] == k[2] || k[0] == k[2]) continue;
		// The same triangle whichever corner comes first: the smallest id leads.
		int lead = 0;
		if (k[1] < k[lead]) lead = 1;
		if (k[2] < k[lead]) lead = 2;
		OutTri o;
		for (int v = 0; v < 3; ++v) o.k[v] = k[(lead + v) % 3];
		o.src = input[i];
		o.gx = sm.cellOf[i].first;
		o.gz = sm.cellOf[i].second;
		o.tx = o.tz = 0;
		o.ci = ci;
		c.tris.push_back(o);
	}
	std::sort(c.tris.begin(), c.tris.end(), [](const OutTri& a, const OutTri& b) {
		if (a.k[0] != b.k[0]) return a.k[0] < b.k[0];
		if (a.k[1] != b.k[1]) return a.k[1] < b.k[1];
		if (a.k[2] != b.k[2]) return a.k[2] < b.k[2];
		return a.src < b.src;
	});
	c.tris.erase(std::unique(c.tris.begin(), c.tris.end(),
	                         [](const OutTri& a, const OutTri& b) {
		                         return a.k[0] == b.k[0] && a.k[1] == b.k[1] && a.k[2] == b.k[2];
	                         }),
	             c.tris.end());
}

struct Point {
	float pos[3];
	u8 rgba[4];
};

// Where each box's point goes, and its colour.
std::vector<Point> place(const SrcScene& scene, const std::vector<u32>& input, const std::vector<float>& shrink, const Clustering& c,
                         const std::vector<std::array<u8, 3>>& color) {
	const size_t n = c.boxes.size();
	std::vector<double> q(n * 10, 0.0);  // plane quadrics: aa ab ac ad bb bc bd cc cd dd
	std::vector<double> m(n * 4, 0.0);   // weighted sum of positions, weight
	std::vector<double> col(n * 5, 0.0); // weighted sum of colour (squared: roughly linear light), daylight, weight
	for (size_t i = 0; i < input.size(); ++i) {
		const SrcTriangle& t = scene.triangles[input[i]];
		const Corners cs = cornersOf(t, shrink);
		const double ax = cs.p[0][0], ay = cs.p[0][1], az = cs.p[0][2];
		const double ux = cs.p[1][0] - ax, uy = cs.p[1][1] - ay, uz = cs.p[1][2] - az;
		const double vx = cs.p[2][0] - ax, vy = cs.p[2][1] - ay, vz = cs.p[2][2] - az;
		double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
		const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
		const double area = len * 0.5;
		const double w = area + 1.0; // a sliver still counts a little
		const std::array<u8, 3> mc = t.material < color.size() ? color[t.material] : std::array<u8, 3> {128, 128, 128};
		for (int v = 0; v < 3; ++v) {
			const u32 k = c.cornerBox[i * 3 + size_t(v)];
			double* mk = &m[size_t(k) * 4];
			for (int a = 0; a < 3; ++a) mk[a] += w * cs.p[v][a];
			mk[3] += w;
			double* ck = &col[size_t(k) * 5];
			for (int a = 0; a < 3; ++a) {
				const double lit = double(mc[size_t(a)]) * double(t.v[v].rgba[a]) / 255.0;
				ck[a] += w * lit * lit;
			}
			ck[3] += w * t.v[v].rgba[3];
			ck[4] += w;
		}
		if (len <= 1e-9) continue;
		nx /= len;
		ny /= len;
		nz /= len;
		const double d = -(nx * ax + ny * ay + nz * az);
		const double e[10] = {nx * nx, nx * ny, nx * nz, nx * d, ny * ny, ny * nz, ny * d, nz * nz, nz * d, d * d};
		for (int v = 0; v < 3; ++v) {
			double* qk = &q[size_t(c.cornerBox[i * 3 + size_t(v)]) * 10];
			for (int j = 0; j < 10; ++j) qk[j] += area * e[j];
		}
	}
	auto det3 = [](const double M[3][3]) {
		return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
		       M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
	};
	std::vector<Point> out(n);
	for (size_t k = 0; k < n; ++k) {
		const double* mk = &m[k * 4];
		const double mean[3] = {mk[0] / mk[3], mk[1] / mk[3], mk[2] / mk[3]};
		const double* qk = &q[k * 10];
		// Minimise the quadric plus a little pull towards the mean (keeps a
		// flat or edge-only box well defined).
		const double lambda = 1e-3 * (qk[0] + qk[4] + qk[7]) / 3.0 + 1e-12;
		const double A[3][3] = {{qk[0] + lambda, qk[1], qk[2]}, {qk[1], qk[4] + lambda, qk[5]}, {qk[2], qk[5], qk[7] + lambda}};
		const double b[3] = {-qk[3] + lambda * mean[0], -qk[6] + lambda * mean[1], -qk[8] + lambda * mean[2]};
		const double det = det3(A);
		double p[3] = {mean[0], mean[1], mean[2]};
		// (A seam's corners stay where the seam is: at their mean.)
		const Box& bx = c.boxes[k];
		if (std::fabs(det) > 1e-30 && !bx.seam) {
			double s[3];
			for (int col3 = 0; col3 < 3; ++col3) {
				double M[3][3];
				std::memcpy(M, A, sizeof(M));
				for (int r = 0; r < 3; ++r) M[r][col3] = b[r];
				s[col3] = det3(M) / det;
			}
			// Far outside its box, the fit is not to be trusted.
			bool inside = std::isfinite(s[0]) && std::isfinite(s[1]) && std::isfinite(s[2]);
			for (int a = 0; a < 3; ++a)
				inside = inside && s[a] >= bx.mn[a] - bx.size * 0.25 && s[a] <= bx.mn[a] + bx.size * 1.25;
			if (inside) std::memcpy(p, s, sizeof(p));
		}
		Point& o = out[k];
		for (int a = 0; a < 3; ++a) o.pos[a] = float(p[a]);
		const double* ck = &col[k * 5];
		for (int a = 0; a < 3; ++a) o.rgba[a] = u8(std::clamp(std::lround(std::sqrt(ck[a] / ck[4])), 0l, 255l));
		o.rgba[3] = u8(std::clamp(std::lround(ck[3] / ck[4]), 0l, 255l));
	}
	return out;
}

} // namespace

std::vector<u8> buildFarLayer(const SrcScene& scene, const std::vector<bool>& include, const std::vector<std::array<u8, 3>>& color,
                              const std::vector<float>& shrink, const std::vector<BakedChunk>& chunks, float chunkSize,
                              const FarOptions& opt, FarStats& stats) {
	stats = FarStats {};
	// Solid triangles are simplified; cut-outs (leaves, mostly) are gathered
	// into crowns, each drawn as a small solid of the size of its leaves.
	std::vector<u32> input, cutouts;
	for (u32 i = 0; i < scene.triangles.size(); ++i) {
		const SrcTriangle& t = scene.triangles[i];
		if ((t.detail & 3) != DETAIL_ALWAYS || t.material >= include.size() || !include[t.material]) continue;
		(t.material < shrink.size() && shrink[t.material] < 1.0f ? cutouts : input).push_back(i);
	}
	stats.inputTriangles = input.size() + cutouts.size();
	if (stats.inputTriangles == 0 || chunkSize <= 0) return {};

	struct Crown {
		float mn[3], mx[3];
		double col[3], day, w, solid;
		u32 ci;
	};
	CellMap cells;
	const Seams seamCorners = findSeams(scene, input, chunkSize, cells);
	std::vector<u32> cutoutCell(cutouts.size());
	for (size_t i = 0; i < cutouts.size(); ++i) {
		const std::pair<int, int> g = cellOfTriangle(scene.triangles[cutouts[i]], chunkSize);
		cutoutCell[i] = cells.of(g.first, g.second);
	}
	const u32 cellCount = u32(cells.cells.size());
	auto crownBox = [](double size) { return std::max(size * 1.5, 400.0); };
	std::unordered_map<std::pair<u64, u64>, Crown, PairHash> crowns;
	auto gather = [&](const Grid& grid) {
		crowns.clear();
		for (size_t i = 0; i < cutouts.size(); ++i) {
			const SrcTriangle& t = scene.triangles[cutouts[i]];
			const u32 ci = cutoutCell[i];
			float mid[3];
			for (int a = 0; a < 3; ++a) mid[a] = (t.v[0].pos[a] + t.v[1].pos[a] + t.v[2].pos[a]) / 3.0f;
			auto [it, fresh] = crowns.emplace(std::make_pair(u64(ci), boxKey(mid, crownBox(grid.size[ci]))), Crown {});
			Crown& cr = it->second;
			if (fresh) {
				for (int a = 0; a < 3; ++a) {
					cr.mn[a] = 1e30f;
					cr.mx[a] = -1e30f;
				}
				cr.ci = ci;
			}
			const float ux = t.v[1].pos[0] - t.v[0].pos[0], uy = t.v[1].pos[1] - t.v[0].pos[1], uz = t.v[1].pos[2] - t.v[0].pos[2];
			const float vx = t.v[2].pos[0] - t.v[0].pos[0], vy = t.v[2].pos[1] - t.v[0].pos[1], vz = t.v[2].pos[2] - t.v[0].pos[2];
			const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
			const double w = std::sqrt(nx * nx + ny * ny + nz * nz) * 0.5 + 1.0;
			const std::array<u8, 3> mc = color[t.material];
			for (int v = 0; v < 3; ++v) {
				for (int a = 0; a < 3; ++a) {
					cr.mn[a] = std::min(cr.mn[a], t.v[v].pos[a]);
					cr.mx[a] = std::max(cr.mx[a], t.v[v].pos[a]);
					const double lit = double(mc[size_t(a)]) * double(t.v[v].rgba[a]) / 255.0;
					cr.col[a] += w * lit * lit;
				}
				cr.day += w * t.v[v].rgba[3];
				cr.w += w;
			}
			cr.solid += w * 3.0 * shrink[t.material];
		}
	};

	// How coarse, where. The grid is tried at sizes a step of 1.5 apart;
	// for each size every cell's count of triangles is noted. Each cell then
	// gets the finest size within its share of the budget, the shares going
	// with the cell's own detail (its triangles to the power 0.6): a town gets
	// more than as much meadow, though not all it would want.
	const double finestTried = std::max(50.0, double(opt.cell));
	std::vector<double> sizes;
	std::vector<std::vector<u32>> counts; // [size][cell]
	std::vector<double> weight(cellCount, 1.0);
	{
		std::vector<u32> n(cellCount, 0);
		for (u32 ci : seamCorners.cellIndex) ++n[ci];
		for (u32 ci : cutoutCell) ++n[ci];
		for (u32 ci = 0; ci < cellCount; ++ci) weight[ci] = std::pow(double(std::max(n[ci], 1u)), 0.6);
	}
	size_t uniformFit = 0; // the finest size at which all of it fits
	{
		Clustering c;
		for (int l = 0; l < 12; ++l) {
			Grid g;
			const double size = finestTried * std::pow(1.5, l);
			g.seam = size;
			g.size.assign(cellCount, size);
			cluster(scene, input, seamCorners, g, shrink, c);
			gather(g);
			++stats.passes;
			std::vector<u32> count(cellCount, 0);
			u64 total = 0;
			for (const OutTri& t : c.tris) ++count[t.ci];
			for (const auto& kv : crowns) count[kv.second.ci] += 8;
			for (u32 v : count) total += v;
			sizes.push_back(size);
			counts.push_back(std::move(count));
			if (total > opt.maxTriangles) uniformFit = sizes.size();
			// Coarser still would help no cell much.
			if (double(total) < double(opt.maxTriangles) * 0.15 || total < 64) break;
		}
		uniformFit = std::min(uniformFit, sizes.size() - 1);
	}
	// The level of each cell for a share `m` per unit of weight.
	std::vector<u8> level(cellCount, 0);
	auto choose = [&](double m) {
		u64 total = 0;
		for (u32 ci = 0; ci < cellCount; ++ci) {
			u8 l = 0;
			while (l + 1u < sizes.size() && double(counts[l][ci]) > m * weight[ci]) ++l;
			level[ci] = l;
			total += counts[l][ci];
		}
		return total;
	};

	// The layer for a budget: the shares found by halving, then built; if what
	// comes out is too much after all (the seams' corners are on their own
	// grid), again with less.
	auto make = [&](double budget) -> std::vector<u8> {
		double lo = 0.0, hi = 1.0;
		while (double(choose(hi)) < budget && hi < 1e9) hi *= 2.0;
		for (int it = 0; it < 40; ++it) {
			const double mid = (lo + hi) * 0.5;
			(double(choose(mid)) <= budget ? lo : hi) = mid;
		}
		choose(lo);
		Grid grid;
		grid.seam = sizes[uniformFit];
		grid.size.resize(cellCount);
		double finest = 1e30, coarsest = 0;
		for (u32 ci = 0; ci < cellCount; ++ci) {
			grid.size[ci] = sizes[level[ci]];
			finest = std::min(finest, grid.size[ci]);
			coarsest = std::max(coarsest, grid.size[ci]);
		}
		Clustering c;
		cluster(scene, input, seamCorners, grid, shrink, c);
		gather(grid);
		++stats.passes;
		stats.cell = float(grid.seam);
		stats.cellFinest = float(finest);
		stats.cellCoarsest = float(coarsest);
		const double cell = grid.seam;
		std::vector<Point> pts = place(scene, input, shrink, c, color);
		stats.crowns = 0;

		// The crowns: an octahedron each, lighter on top and darker below, as
		// big as the box of its leaves (less where the leaves have holes).
		for (const auto& kv : crowns) {
			const Crown& cr = kv.second;
			// (An octahedron fills a sixth of its box: somewhat bigger than the leaves' box.)
			const float fill = float(std::clamp(cr.solid / cr.w * 1.1, 0.7, 1.0)) * 1.1f;
			float mid[3], half[3];
			float big = 0.0f;
			for (int a = 0; a < 3; ++a) {
				mid[a] = (cr.mn[a] + cr.mx[a]) * 0.5f;
				half[a] = (cr.mx[a] - cr.mn[a]) * 0.5f * fill;
				big = std::max(big, half[a]);
			}
			if (big < 1.0f) continue;
			for (float& h : half) h = std::max(h, big * 0.3f);
			u8 rgb[3];
			for (int a = 0; a < 3; ++a) rgb[a] = u8(std::clamp(std::lround(std::sqrt(cr.col[a] / cr.w)), 0l, 255l));
			const u8 day = u8(std::clamp(std::lround(cr.day / cr.w), 0l, 255l));
			const u32 base = u32(pts.size());
			// 0 top, 1 bottom, 2..5 round the middle (+x, +z, -x, -z).
			const float off[6][3] = {{0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {0, 0, 1}, {-1, 0, 0}, {0, 0, -1}};
			const float shade[6] = {1.15f, 0.55f, 0.9f, 0.9f, 0.9f, 0.9f};
			for (int v = 0; v < 6; ++v) {
				Point p;
				for (int a = 0; a < 3; ++a) p.pos[a] = mid[a] + off[v][a] * half[a];
				for (int a = 0; a < 3; ++a) p.rgba[a] = u8(std::clamp(int(float(rgb[a]) * shade[v] + 0.5f), 0, 255));
				p.rgba[3] = day;
				pts.push_back(p);
			}
			// (In the cell of its leaves, where the chunker has them.)
			const int gx = cells.cells[cr.ci].first, gz = cells.cells[cr.ci].second;
			// Front faces outwards (counter-clockwise seen from outside; see the
			// bake's winding note).
			for (int r = 0; r < 4; ++r) {
				const u32 a = base + 2 + u32(r), b = base + 2 + u32((r + 1) % 4);
				OutTri up {{base + 0, a, b}, ~0u, gx, gz, 0, 0, cr.ci};
				OutTri down {{base + 1, b, a}, ~0u, gx, gz, 0, 0, cr.ci};
				c.tris.push_back(up);
				c.tris.push_back(down);
			}
			++stats.crowns;
		}
		if (c.tris.empty()) return {};

		// Tiles of whole chunk cells; within a tile, triangles by cell.
		const int T = std::max(1, int(std::lround(opt.tileSize / chunkSize)));
		for (OutTri& t : c.tris) {
			t.tx = int(floorDiv(t.gx, T));
			t.tz = int(floorDiv(t.gz, T));
		}
		std::sort(c.tris.begin(), c.tris.end(), [](const OutTri& a, const OutTri& b) {
			if (a.tz != b.tz) return a.tz < b.tz;
			if (a.tx != b.tx) return a.tx < b.tx;
			if (a.gz != b.gz) return a.gz < b.gz;
			if (a.gx != b.gx) return a.gx < b.gx;
			return a.src < b.src;
		});

		// The chunks of each grid cell.
		std::map<std::pair<int, int>, std::vector<u32>> cellChunks;
		for (size_t i = 0; i < chunks.size(); ++i) cellChunks[{chunks[i].gridX, chunks[i].gridZ}].push_back(u32(i));
		std::vector<u32> refs;
		std::map<std::pair<int, int>, std::pair<u32, u32>> cellRefs;
		auto refsOf = [&](int gx, int gz) {
			auto it = cellRefs.find({gx, gz});
			if (it != cellRefs.end()) return it->second;
			std::pair<u32, u32> r {u32(refs.size()), 0};
			auto ch = cellChunks.find({gx, gz});
			if (ch != cellChunks.end()) {
				refs.insert(refs.end(), ch->second.begin(), ch->second.end());
				r.second = u32(ch->second.size());
			}
			cellRefs.emplace(std::make_pair(gx, gz), r);
			return r;
		};

		// Seams. Where a cell of the far layer meets one drawn in full, the two
		// do not meet exactly (the far layer is coarser), and through the slit
		// the sky would show. Along every edge two cells share hangs a strip,
		// drawn (from both sides) only while one of the two is in full and the
		// other not. Only along ground and roofs:
		// through a slit in a wall one sees what is behind it, not the sky.
		const float skirt = float(cell);
		struct SeamEdge {
			u32 a, b; // as the lesser cell's triangle has it
		};
		using CellPair = std::pair<std::pair<int, int>, std::pair<int, int>>;
		std::map<CellPair, std::vector<SeamEdge>> seams;
		u32 seamEdges = 0;
		{
			auto facesUp = [&](u32 a, u32 b, u32 cc) {
				const float* pa = pts[a].pos;
				const float* pb = pts[b].pos;
				const float* pc = pts[cc].pos;
				const float ux = pb[0] - pa[0], uy = pb[1] - pa[1], uz = pb[2] - pa[2];
				const float vx = pc[0] - pa[0], vy = pc[1] - pa[1], vz = pc[2] - pa[2];
				// The front normal is -cross(b - a, c - a) (the bake's winding note).
				const float nx = -(uy * vz - uz * vy), ny = -(uz * vx - ux * vz), nz = -(ux * vy - uy * vx);
				return ny > 0.3f * std::sqrt(nx * nx + ny * ny + nz * nz);
			};
			std::unordered_map<u64, u32> edgeTri; // directed edge -> a triangle that has it
			edgeTri.reserve(c.tris.size() * 3);
			for (u32 ti = 0; ti < c.tris.size(); ++ti)
				for (int e = 0; e < 3; ++e) edgeTri[(u64(c.tris[ti].k[e]) << 32) | c.tris[ti].k[(e + 1) % 3]] = ti;
			for (const OutTri& t : c.tris)
				for (int e = 0; e < 3; ++e) {
					const u32 ea = t.k[e], eb = t.k[(e + 1) % 3], ec = t.k[(e + 2) % 3];
					auto it = edgeTri.find((u64(eb) << 32) | ea);
					if (it == edgeTri.end()) continue;
					const OutTri& o = c.tris[it->second];
					const std::pair<int, int> ca {t.gz, t.gx}, cb {o.gz, o.gx};
					if (!(ca < cb)) continue; // once per edge, by the lesser cell (and none within a cell)
					const float ex = pts[eb].pos[0] - pts[ea].pos[0], ez = pts[eb].pos[2] - pts[ea].pos[2];
					if (std::fabs(ex) + std::fabs(ez) < 1e-3f) continue; // upright: nothing to hang
					u32 oc = o.k[0];
					for (u32 k : o.k)
						if (k != ea && k != eb) oc = k;
					if (!facesUp(ea, eb, ec) && !facesUp(eb, ea, oc)) continue;
					seams[{ca, cb}].push_back({ea, eb});
					++seamEdges;
				}
		}

		std::vector<FarTile> tiles;
		std::vector<FarRange> ranges;
		std::vector<WorldVertex> vertices;
		std::vector<u16> indices;
		std::vector<u32> stamp(pts.size(), 0), skirtStamp(pts.size(), 0);
		std::vector<u16> local(pts.size(), 0), skirtLocal(pts.size(), 0);
		u32 tileId = 0;
		std::vector<Point> tileVerts; // of the open tile, in local order
		FarTile tile {};
		FarRange range {};
		bool tileOpen = false, rangeOpen = false;
		int curTx = 0, curTz = 0, curGx = 0, curGz = 0;
		auto grow = [](float mn[3], float mx[3], const float p[3]) {
			for (int a = 0; a < 3; ++a) {
				mn[a] = std::min(mn[a], p[a]);
				mx[a] = std::max(mx[a], p[a]);
			}
		};
		auto vertexOf = [&](u32 k) {
			if (stamp[k] != tileId) {
				stamp[k] = tileId;
				local[k] = u16(tileVerts.size());
				tileVerts.push_back(pts[k]);
			}
			return local[k];
		};
		auto skirtVertexOf = [&](u32 k) {
			if (skirtStamp[k] != tileId) {
				skirtStamp[k] = tileId;
				skirtLocal[k] = u16(tileVerts.size());
				Point p = pts[k];
				p.pos[1] -= skirt;
				tileVerts.push_back(p);
			}
			return skirtLocal[k];
		};
		auto emptyBox = [](FarRange& r) {
			for (int a = 0; a < 3; ++a) {
				r.bbMin[a] = 1e30f;
				r.bbMax[a] = -1e30f;
			}
		};
		auto closeRange = [&]() {
			if (!rangeOpen) return;
			rangeOpen = false;
			range.indexCount = u32(indices.size()) - tile.firstIndex - range.firstIndex;
			if (range.indexCount) ranges.push_back(range);
		};
		// The strips of the seams whose lesser cell lies in the tile (after its cells).
		u32 stripTriangles = 0;
		auto addStrips = [&]() {
			for (const auto& [pair, list] : seams) {
				const std::pair<int, int>& ca = pair.first; // (gz, gx)
				if (floorDiv(ca.second, T) != curTx || floorDiv(ca.first, T) != curTz) continue;
				if (tileVerts.size() + 4 * list.size() > CHUNK_MAX_VERTICES) continue; // (no room: without it)
				FarRange r {};
				emptyBox(r);
				r.firstIndex = u32(indices.size()) - tile.firstIndex;
				const std::pair<u32, u32> ra = refsOf(ca.second, ca.first), rb = refsOf(pair.second.second, pair.second.first);
				if (rb.second > 255) continue;
				const std::vector<u32> both = [&] {
					std::vector<u32> v(refs.begin() + ra.first, refs.begin() + ra.first + ra.second);
					v.insert(v.end(), refs.begin() + rb.first, refs.begin() + rb.first + rb.second);
					return v;
				}();
				r.firstChunk = u32(refs.size());
				refs.insert(refs.end(), both.begin(), both.end());
				r.chunkCount = u16(ra.second);
				r.seam = 1;
				r.otherCount = u8(rb.second);
				for (const SeamEdge& e : list) {
					const u16 va = vertexOf(e.a), vb = vertexOf(e.b), sa = skirtVertexOf(e.a), sb = skirtVertexOf(e.b);
					const u16 quad[6] = {va, vb, sb, va, sb, sa}; // (drawn from both sides)
					indices.insert(indices.end(), quad, quad + 6);
					for (u16 v : {va, vb, sa, sb}) grow(r.bbMin, r.bbMax, tileVerts[v].pos);
				}
				r.indexCount = u32(list.size() * 6);
				stripTriangles += r.indexCount / 3;
				ranges.push_back(r);
			}
		};
		auto closeTile = [&](bool last) {
			closeRange();
			if (!tileOpen) return;
			if (last) addStrips();
			tileOpen = false;
			tile.indexCount = u32(indices.size()) - tile.firstIndex;
			tile.rangeCount = u32(ranges.size()) - tile.firstRange;
			for (int a = 0; a < 3; ++a) {
				tile.bbMin[a] = 1e30f;
				tile.bbMax[a] = -1e30f;
			}
			for (const Point& p : tileVerts) grow(tile.bbMin, tile.bbMax, p.pos);
			float half = 1.0f;
			for (int a = 0; a < 3; ++a) {
				tile.origin[a] = std::floor((tile.bbMin[a] + tile.bbMax[a]) * 0.5f);
				half = std::max({half, tile.bbMax[a] - tile.origin[a], tile.origin[a] - tile.bbMin[a]});
			}
			tile.posScale = std::max(0.125f, half / 32000.0f);
			tile.firstVertex = u32(vertices.size());
			tile.vertexCount = u32(tileVerts.size());
			for (const Point& p : tileVerts) {
				WorldVertex v {};
				i16* q[3] = {&v.x, &v.y, &v.z};
				for (int a = 0; a < 3; ++a)
					*q[a] = i16(std::clamp(std::lround((p.pos[a] - tile.origin[a]) / tile.posScale), -32767l, 32767l));
				v.w = 1;
				v.r = p.rgba[0];
				v.g = p.rgba[1];
				v.b = p.rgba[2];
				v.a = p.rgba[3];
				vertices.push_back(v);
			}
			if (tile.indexCount) tiles.push_back(tile);
			tileVerts.clear();
		};
		auto openTile = [&]() {
			++tileId;
			tile = FarTile {};
			tile.firstIndex = u32(indices.size());
			tile.firstRange = u32(ranges.size());
			tileOpen = true;
		};
		auto openRange = [&]() {
			range = FarRange {};
			range.firstIndex = u32(indices.size()) - tile.firstIndex;
			const std::pair<u32, u32> r = refsOf(curGx, curGz);
			range.firstChunk = r.first;
			range.chunkCount = u16(r.second);
			emptyBox(range);
			rangeOpen = true;
		};
		for (u32 ti = 0; ti < c.tris.size(); ++ti) {
			const OutTri& t = c.tris[ti];
			if (!tileOpen || t.tx != curTx || t.tz != curTz) {
				closeTile(true);
				curTx = t.tx;
				curTz = t.tz;
				openTile();
			}
			if (!rangeOpen || t.gx != curGx || t.gz != curGz) {
				closeRange();
				curGx = t.gx;
				curGz = t.gz;
				openRange();
			}
			u32 fresh = 0;
			for (u32 k : t.k) fresh += stamp[k] != tileId;
			// (Room kept for the strips of the seams: a quarter of the tile.)
			if (tileVerts.size() + fresh > CHUNK_MAX_VERTICES * 3 / 4) {
				// The tile is full: the rest of it goes on in another one.
				closeTile(false);
				openTile();
				openRange();
			}
			for (u32 k : t.k) {
				indices.push_back(vertexOf(k));
				grow(range.bbMin, range.bbMax, pts[k].pos);
			}
		}
		closeTile(true);

		FarHeader h {};
		h.magic = FAR_MAGIC;
		h.version = FAR_VERSION;
		h.tileCount = u32(tiles.size());
		h.rangeCount = u32(ranges.size());
		h.chunkRefCount = u32(refs.size());
		h.vertexCount = u32(vertices.size());
		h.indexCount = u32(indices.size());
		h.cellSize = float(cell);
		std::vector<u8> blob;
		auto put = [&](const void* p, size_t n) {
			const u8* b = static_cast<const u8*>(p);
			blob.insert(blob.end(), b, b + n);
		};
		put(&h, sizeof(h));
		put(tiles.data(), tiles.size() * sizeof(FarTile));
		put(ranges.data(), ranges.size() * sizeof(FarRange));
		put(refs.data(), refs.size() * sizeof(u32));
		put(vertices.data(), vertices.size() * sizeof(WorldVertex));
		put(indices.data(), indices.size() * sizeof(u16));
		if (blob.size() != farBlobSize(h)) throw std::logic_error("far layer layout");
		stats.triangles = u32(indices.size() / 3);
		stats.skirtTriangles = stripTriangles;
		stats.seamEdges = seamEdges;
		stats.vertices = u32(vertices.size());
		stats.tiles = u32(tiles.size());
		stats.ranges = u32(ranges.size());
		stats.bytes = u32(blob.size());
		return blob;
	};
	double budget = double(opt.maxTriangles);
	std::vector<u8> blob;
	for (int attempt = 0;; ++attempt) {
		blob = make(budget);
		// (The strips are not counted: they are drawn along the seams only.)
		const u32 drawn = stats.triangles - stats.skirtTriangles;
		if (blob.empty() || drawn <= opt.maxTriangles || attempt >= 6) break;
		budget *= double(opt.maxTriangles) / double(drawn) * 0.97;
	}
	return blob;
}

} // namespace g3
