#include "chunker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace g3 {

std::vector<u8> BakedChunk::serialize() const {
	std::vector<u8> out(chunkBlobSize(u32(batches.size()), u32(vertices.size()), u32(indices.size())));
	u8* p = out.data();
	auto put = [&p](const void* src, size_t n) {
		if (n) std::memcpy(p, src, n);
		p += n;
	};
	put(&header, sizeof(header));
	put(batches.data(), batches.size() * sizeof(ChunkBatch));
	put(vertices.data(), vertices.size() * sizeof(WorldVertex));
	put(indices.data(), indices.size() * sizeof(u16));
	return out;
}

namespace {

struct VertexKey {
	WorldVertex v;
	bool operator==(const VertexKey& o) const { return std::memcmp(&v, &o.v, sizeof(v)) == 0; }
};

struct VertexKeyHash {
	size_t operator()(const VertexKey& k) const {
		const u8* p = reinterpret_cast<const u8*>(&k.v);
		u64 h = 1469598103934665603ull;
		for (size_t i = 0; i < sizeof(WorldVertex); ++i) h = (h ^ p[i]) * 1099511628211ull;
		return size_t(h);
	}
};

const float kUvLimit = 32767.0f * WORLD_UV_SCALE; // largest representable texture coordinate
const i32 kUvRepeat = 1024;                       // one texture repeat in fixed point (1 / WORLD_UV_SCALE)

// The vertex with its UV reduced to the fraction inside one repeat.
VertexKey fractionKey(WorldVertex v, i32 u, i32 vv) {
	v.u = i16(((u % kUvRepeat) + kUvRepeat) % kUvRepeat);
	v.v = i16(((vv % kUvRepeat) + kUvRepeat) % kUvRepeat);
	return VertexKey {v};
}

using i64 = long long;

// ---- seam repair: clusters of coinciding vertices ----

struct CellKey {
	i64 x, y, z;
	bool operator==(const CellKey& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct CellKeyHash {
	size_t operator()(const CellKey& k) const {
		u64 h = u64(k.x) * 0x9E3779B97F4A7C15ull;
		h = (h ^ (h >> 29) ^ u64(k.y)) * 0xBF58476D1CE4E5B9ull;
		h = (h ^ (h >> 32) ^ u64(k.z)) * 0x94D049BB133111EBull;
		return size_t(h ^ (h >> 31));
	}
};

struct P3 {
	double x, y, z;
};

// Every source position belongs to one cluster. Equal positions always
// share one; with `eps` > 0 so do positions closer than that to the first
// position seen there.
struct Clusters {
	std::vector<P3> pos;
	std::unordered_map<CellKey, u32, CellKeyHash> exact;               // by the bits of the three floats
	std::unordered_map<CellKey, std::vector<u32>, CellKeyHash> nearby; // cells of 2 * eps
	double eps = 0;
	u64 merged = 0;

	u32 find(const float p[3]) {
		u32 bits[3];
		for (int a = 0; a < 3; ++a) {
			const float f = p[a] + 0.0f; // -0 and +0 are the same place
			std::memcpy(&bits[a], &f, 4);
		}
		auto ins = exact.emplace(CellKey {i64(bits[0]), i64(bits[1]), i64(bits[2])}, u32(pos.size()));
		if (!ins.second) return ins.first->second;
		const P3 q {double(p[0]), double(p[1]), double(p[2])};
		if (eps > 0) {
			const double cell = eps * 2.0;
			const double f[3] = {q.x / cell, q.y / cell, q.z / cell};
			i64 lo[3], hi[3];
			for (int a = 0; a < 3; ++a) {
				const i64 c = i64(std::floor(f[a]));
				const bool lower = f[a] - double(c) < 0.5; // anything within eps is in this cell or that neighbour
				lo[a] = lower ? c - 1 : c;
				hi[a] = lower ? c : c + 1;
			}
			for (i64 z = lo[2]; z <= hi[2]; ++z)
				for (i64 y = lo[1]; y <= hi[1]; ++y)
					for (i64 x = lo[0]; x <= hi[0]; ++x) {
						auto it = nearby.find(CellKey {x, y, z});
						if (it == nearby.end()) continue;
						for (u32 c : it->second) {
							const double dx = pos[c].x - q.x, dy = pos[c].y - q.y, dz = pos[c].z - q.z;
							if (dx * dx + dy * dy + dz * dz <= eps * eps) {
								ins.first->second = c;
								++merged;
								return c;
							}
						}
					}
			nearby[CellKey {i64(std::floor(f[0])), i64(std::floor(f[1])), i64(std::floor(f[2]))}].push_back(
			    u32(pos.size()));
		}
		pos.push_back(q);
		return ins.first->second;
	}
};

// A triangle on its way through the chunker: source attributes per corner
// (the position comes from the corner's cluster).
struct WorkTri {
	SrcVertex v[3];
	u32 c[3];
	u32 material;
	u8 detail;
};

struct SplitPoint {
	u32 cluster;
	SrcVertex v; // attributes of the edge's own triangle at that point
};

struct Span {
	const SplitPoint* begin = nullptr;
	const SplitPoint* end = nullptr;
	bool empty() const { return begin == end; }
};

// Finds, for every triangle edge, the vertices that lie on it, and cuts the
// triangles there.
struct Sealer {
	const Clusters& clusters;
	double tol;  // how far from an edge a vertex still counts as lying on it
	double ends; // how far from the edge's ends it has to be (nearer ones are welded)
	ChunkerStats& stats;
	std::vector<WorkTri>& out;
	static constexpr double kCell = 128.0;
	std::unordered_map<CellKey, std::vector<u32>, CellKeyHash> grid;
	// Per undirected edge (smaller cluster first): the clusters on it with
	// their position along the edge, nearest to the smaller cluster first.
	std::unordered_map<u64, std::vector<std::pair<double, u32>>> edges;

	Sealer(const Clusters& c, double tolerance, ChunkerStats& st, std::vector<WorkTri>& o)
	    : clusters(c), tol(tolerance), ends(tolerance * 0.5), stats(st), out(o) {
		for (u32 i = 0; i < u32(c.pos.size()); ++i) grid[cellOf(c.pos[i])].push_back(i);
	}

	static CellKey cellOf(const P3& p) {
		return CellKey {i64(std::floor(p.x / kCell)), i64(std::floor(p.y / kCell)), i64(std::floor(p.z / kCell))};
	}

	const std::vector<std::pair<double, u32>>& onEdge(u32 a, u32 b) {
		const u32 lo = std::min(a, b), hi = std::max(a, b);
		auto ins = edges.emplace((u64(lo) << 32) | hi, std::vector<std::pair<double, u32>>());
		std::vector<std::pair<double, u32>>& list = ins.first->second;
		if (!ins.second) return list;
		const P3 A = clusters.pos[lo], B = clusters.pos[hi];
		const double dx = B.x - A.x, dy = B.y - A.y, dz = B.z - A.z;
		const double len2 = dx * dx + dy * dy + dz * dz, len = std::sqrt(len2);
		if (len <= ends * 2.0) return list;
		// Walk the edge in short pieces so that only cells near it are visited.
		const int pieces = int(std::ceil(len / kCell));
		for (int i = 0; i < pieces; ++i) {
			const double t0 = double(i) / pieces, t1 = double(i + 1) / pieces;
			const P3 p0 {A.x + dx * t0, A.y + dy * t0, A.z + dz * t0}, p1 {A.x + dx * t1, A.y + dy * t1, A.z + dz * t1};
			const CellKey c0 = cellOf(P3 {std::min(p0.x, p1.x) - tol, std::min(p0.y, p1.y) - tol, std::min(p0.z, p1.z) - tol});
			const CellKey c1 = cellOf(P3 {std::max(p0.x, p1.x) + tol, std::max(p0.y, p1.y) + tol, std::max(p0.z, p1.z) + tol});
			for (i64 z = c0.z; z <= c1.z; ++z)
				for (i64 y = c0.y; y <= c1.y; ++y)
					for (i64 x = c0.x; x <= c1.x; ++x) {
						auto it = grid.find(CellKey {x, y, z});
						if (it == grid.end()) continue;
						for (u32 v : it->second) {
							if (v == lo || v == hi) continue;
							const P3& V = clusters.pos[v];
							const double vx = V.x - A.x, vy = V.y - A.y, vz = V.z - A.z;
							const double along = (vx * dx + vy * dy + vz * dz) / len; // cm from A
							// Clear of both ends: a vertex next to a corner is not a T-junction.
							if (along <= ends || along >= len - ends) continue;
							const double off2 = vx * vx + vy * vy + vz * vz - along * along;
							if (off2 > tol * tol) continue;
							list.emplace_back(along / len, v);
						}
					}
		}
		std::sort(list.begin(), list.end());
		list.erase(std::unique(list.begin(), list.end(), [](const auto& l, const auto& r) { return l.second == r.second; }),
		           list.end());
		stats.tJunctions += list.size();
		return list;
	}

	// The points on the edge from corner i to corner j of `t`, in that order.
	void pointsOn(const WorkTri& t, int i, int j, std::vector<SplitPoint>& points) {
		points.clear();
		const std::vector<std::pair<double, u32>>& list = onEdge(t.c[i], t.c[j]);
		if (list.empty()) return;
		const bool forward = t.c[i] < t.c[j];
		for (size_t n = 0; n < list.size(); ++n) {
			const std::pair<double, u32>& e = forward ? list[n] : list[list.size() - 1 - n];
			const float f = float(forward ? e.first : 1.0 - e.first);
			SplitPoint sp;
			sp.cluster = e.second;
			const SrcVertex& a = t.v[i];
			const SrcVertex& b = t.v[j];
			for (int k = 0; k < 3; ++k) sp.v.pos[k] = a.pos[k] + (b.pos[k] - a.pos[k]) * f;
			for (int k = 0; k < 2; ++k) sp.v.uv[k] = a.uv[k] + (b.uv[k] - a.uv[k]) * f;
			for (int k = 0; k < 4; ++k)
				sp.v.rgba[k] = u8(std::lround(float(a.rgba[k]) + (float(b.rgba[k]) - float(a.rgba[k])) * f));
			sp.v.occlusion = u8(std::lround(float(a.occlusion) + (float(b.occlusion) - float(a.occlusion)) * f));
			sp.v.wind = u8(std::lround(float(a.wind) + (float(b.wind) - float(a.wind)) * f));
			points.push_back(sp);
		}
	}

	struct Corner {
		u32 cluster;
		SrcVertex v;
	};

	u32 emitted = 0;

	void split(const WorkTri& src, const Corner& c0, const Corner& c1, const Corner& c2, Span l01, Span l12, Span l20) {
		if (c0.cluster == c1.cluster || c1.cluster == c2.cluster || c0.cluster == c2.cluster) {
			++stats.degenerate; // a sliver that closed up
			return;
		}
		if (l01.empty() && l12.empty() && l20.empty()) {
			WorkTri t = src;
			const Corner* cs[3] = {&c0, &c1, &c2};
			for (int k = 0; k < 3; ++k) {
				t.v[k] = cs[k]->v;
				t.c[k] = cs[k]->cluster;
			}
			out.push_back(t);
			++emitted;
			return;
		}
		// Turn the triangle until its first edge is one that has points.
		if (l01.empty()) {
			if (!l12.empty())
				split(src, c1, c2, c0, l12, l20, l01);
			else
				split(src, c2, c0, c1, l20, l01, l12);
			return;
		}
		// Cut at the middle point: two triangles of the same winding, each
		// keeping its share of the points. The new inner edge belongs to these
		// two alone, so it needs no points of its own.
		const SplitPoint* m = l01.begin + (l01.end - l01.begin) / 2;
		const Corner mid {m->cluster, m->v};
		split(src, c0, mid, c2, Span {l01.begin, m}, Span {}, l20);
		split(src, mid, c1, c2, Span {m + 1, l01.end}, l12, Span {});
	}

	void add(const WorkTri& t) {
		if (t.c[0] == t.c[1] || t.c[1] == t.c[2] || t.c[0] == t.c[2]) {
			++stats.degenerate;
			return;
		}
		pointsOn(t, 0, 1, _l01);
		pointsOn(t, 1, 2, _l12);
		pointsOn(t, 2, 0, _l20);
		if (_l01.empty() && _l12.empty() && _l20.empty()) {
			out.push_back(t);
			return;
		}
		emitted = 0;
		const u64 lost = stats.degenerate;
		auto span = [](const std::vector<SplitPoint>& v) { return Span {v.data(), v.data() + v.size()}; };
		split(t, Corner {t.c[0], t.v[0]}, Corner {t.c[1], t.v[1]}, Corner {t.c[2], t.v[2]}, span(_l01), span(_l12),
		      span(_l20));
		if (emitted > 0) {
			stats.sealTriangles += emitted - 1;
			stats.degenerate = lost; // slivers shaved off a triangle that survived are not lost input
		} else {
			stats.degenerate = lost + 1; // the whole triangle was a sliver
		}
	}

private:
	std::vector<SplitPoint> _l01, _l12, _l20;
};

// ---- quantisation frames ----

// The triangles of one grid cell that share a quantisation frame.
struct Group {
	int gx, gz;
	u8 tier = 0;
	int k; // step = base step * 2^k
	std::vector<u32> tris;
	i64 origin[3] = {0, 0, 0}; // lattice coordinates (base steps) of the chunk origin
};

const i64 kRange = 32767;  // what an i16 coordinate holds either side of the origin
const double kFit = 32000; // what a frame is planned for: leaves room for rounding

i64 floorTo(i64 v, int k) { return (v >> k) << k; } // arithmetic shift: rounds towards minus infinity

struct CellBuilder {
	const std::vector<WorkTri>& work;
	const std::vector<std::array<i64, 3>>& lattice; // per cluster, in base steps
	const std::vector<int>& remap;
	const std::vector<u8>& order;
	const ChunkerOptions& opt;
	ChunkerStats& stats;
	std::vector<BakedChunk>& out;
	std::map<std::pair<int, int>, int> nextPart;

	static u8 classOf(const WorkTri& t) { return (t.detail & 3) < DETAIL_COUNT ? u8(t.detail & 3) : u8(DETAIL_COUNT - 1); }
	static u8 layerOf(const WorkTri& t) { return u8((t.detail >> 2) & 3); }
	static u8 bandOf(const WorkTri& t) { return classOf(t) == DETAIL_SMALL ? u8((t.detail >> 4) & 3) : u8(0); }
	// What a batch is told by: the class, the layer and the size band.
	static u8 detailOf(const WorkTri& t) { return u8(classOf(t) | (layerOf(t) << 2) | (bandOf(t) << 4)); }

	void build(Group& g) {
		std::vector<u32>& tris = g.tris;
		const int gx = g.gx, gz = g.gz;
		// Every chunk quantises on one world-wide lattice: the step is the base
		// step times a power of two and the origin is a multiple of the step. A
		// vertex shared by two chunks then lands on exactly the same world
		// position in both, so their borders have no hairline cracks.
		const double step = double(opt.positionStep) * double(i64(1) << g.k);
		const float posScale = float(step);
		const float mn[3] = {float(double(g.origin[0]) * opt.positionStep), float(double(g.origin[1]) * opt.positionStep),
		                     float(double(g.origin[2]) * opt.positionStep)}; // the chunk origin
		stats.maxPosError = std::max(stats.maxPosError, posScale * 0.5f);

		// Clusters: boxes the triangles are sorted into by their centroid, in
		// an order that keeps neighbours in space near each other (Z-order),
		// so that what is in view is long runs of a material's indices.
		std::unordered_map<u32, u8> clusterOf; // per triangle
		if (opt.clusterSize > 0.0f && !tris.empty()) {
			float size = opt.clusterSize;
			for (;; size *= 2.0f) {
				std::map<std::array<i64, 3>, u64> cells; // cell -> its Z-order code
				i64 low[3] = {INT64_MAX, INT64_MAX, INT64_MAX};
				auto cellOf = [&](u32 t) {
					const WorkTri& tri = work[t];
					std::array<i64, 3> c;
					for (int a = 0; a < 3; ++a)
						c[size_t(a)] = i64(std::floor((double(tri.v[0].pos[a]) + tri.v[1].pos[a] + tri.v[2].pos[a]) / 3.0 / double(size)));
					return c;
				};
				for (u32 t : tris) {
					const std::array<i64, 3> c = cellOf(t);
					for (int a = 0; a < 3; ++a) low[a] = std::min(low[a], c[size_t(a)]);
					cells.emplace(c, 0);
				}
				if (cells.size() > 255) continue; // (one byte names a cluster)
				std::vector<std::pair<u64, std::array<i64, 3>>> ordered;
				for (auto& kv : cells) {
					u64 code = 0;
					for (int bit = 0; bit < 20; ++bit)
						for (int a = 0; a < 3; ++a) code |= u64((u64(kv.first[size_t(a)] - low[a]) >> bit) & 1u) << (bit * 3 + a);
					ordered.emplace_back(code, kv.first);
				}
				std::sort(ordered.begin(), ordered.end());
				for (size_t i = 0; i < ordered.size(); ++i) cells[ordered[i].second] = i;
				for (u32 t : tris) clusterOf[t] = u8(cells[cellOf(t)]);
				stats.clusters += cells.size();
				stats.maxChunkClusters = std::max(stats.maxChunkClusters, u32(cells.size()));
				if (size != opt.clusterSize) ++stats.coarsenedChunks;
				break;
			}
		}
		auto clusterId = [&](u32 t) {
			auto it = clusterOf.find(t);
			return it == clusterOf.end() ? u8(0) : it->second;
		};

		// Batches are contiguous per (draw order, detail class, material, cluster).
		auto sortKey = [&](u32 t) {
			const WorkTri& tri = work[t];
			const u32 m = u32(remap[tri.material]);
			return (u64(order[m]) << 40) | (u64(classOf(tri)) << 32) | (u64(m) << 12) | (u64(clusterId(t)) << 4) | (u64(layerOf(tri)) << 2) |
			       bandOf(tri);
		};
		std::stable_sort(tris.begin(), tris.end(), [&](u32 a, u32 b) { return sortKey(a) < sortKey(b); });

		// A shared UV origin per material keeps vertices shareable between triangles.
		std::map<int, std::pair<float, float>> uvBase;
		for (u32 t : tris) {
			const WorkTri& tri = work[t];
			const int m = remap[tri.material];
			auto it = uvBase.find(m);
			if (it == uvBase.end()) it = uvBase.emplace(m, std::make_pair(1e30f, 1e30f)).first;
			for (const SrcVertex& v : tri.v) {
				it->second.first = std::min(it->second.first, v.uv[0]);
				it->second.second = std::min(it->second.second, v.uv[1]);
			}
		}
		for (auto& kv : uvBase) {
			kv.second.first = std::floor(kv.second.first);
			kv.second.second = std::floor(kv.second.second);
		}

		BakedChunk cur;
		std::unordered_map<VertexKey, u16, VertexKeyHash> lookup;
		// Vertices grouped by everything except the whole-repeat part of the UV.
		std::unordered_map<VertexKey, std::vector<std::pair<i32, i32>>, VertexKeyHash> byFraction;
		// Diagnostics only: how many vertices would remain if light, or light
		// and UVs, did not force a split.
		std::unordered_set<u64> seenPos;
		std::unordered_map<VertexKey, u16, VertexKeyHash> seenPosUv;
		int& part = nextPart[{gx, gz}];
		const int firstPart = part;

		auto begin = [&]() {
			cur = BakedChunk {};
			cur.gridX = gx;
			cur.gridZ = gz;
			cur.part = part++;
			cur.tier = g.tier;
			cur.header.magic = CHUNK_MAGIC;
			std::memcpy(cur.header.origin, mn, sizeof(mn));
			cur.header.posScale = posScale;
			cur.header.uvScale = WORLD_UV_SCALE;
			lookup.clear();
			byFraction.clear();
			seenPos.clear();
			seenPosUv.clear();
		};
		auto flush = [&]() {
			if (cur.indices.empty()) {
				--part; // nothing came of it
				return;
			}
			cur.header.batchCount = u16(cur.batches.size());
			cur.header.vertexCount = u16(cur.vertices.size());
			cur.header.indexCount = u32(cur.indices.size());
			for (ChunkBatch& b : cur.batches) {
				for (int a = 0; a < 3; ++a) {
					b.bbMin[a] = 32767;
					b.bbMax[a] = -32768;
				}
				for (u32 i = 0; i < b.indexCount; ++i) {
					const WorldVertex& v = cur.vertices[cur.indices[b.firstIndex + i]];
					const i16 p[3] = {v.x, v.y, v.z};
					for (int a = 0; a < 3; ++a) {
						b.bbMin[a] = std::min(b.bbMin[a], p[a]);
						b.bbMax[a] = std::max(b.bbMax[a], p[a]);
					}
				}
				stats.detailTriangles[b.detail & 3] += b.indexCount / 3;
			}
			stats.batches += cur.batches.size();
			for (int a = 0; a < 3; ++a) {
				cur.bbMin[a] = 1e30f;
				cur.bbMax[a] = -1e30f;
			}
			// Bounds from the quantised data: exactly what the GPU will draw.
			for (const WorldVertex& v : cur.vertices) {
				const float p[3] = {mn[0] + v.x * posScale, mn[1] + v.y * posScale, mn[2] + v.z * posScale};
				for (int a = 0; a < 3; ++a) {
					cur.bbMin[a] = std::min(cur.bbMin[a], p[a]);
					cur.bbMax[a] = std::max(cur.bbMax[a], p[a]);
				}
			}
			stats.outputTriangles += cur.indices.size() / 3;
			++stats.tierChunks[g.tier];
			stats.tierTriangles[g.tier] += cur.indices.size() / 3;
			stats.tierVertices[g.tier] += cur.vertices.size();
			stats.vertices += cur.vertices.size();
			stats.maxChunkTriangles = std::max(stats.maxChunkTriangles, u32(cur.indices.size() / 3));
			stats.maxChunkVertices = std::max(stats.maxChunkVertices, u32(cur.vertices.size()));
			stats.maxChunkBatches = std::max(stats.maxChunkBatches, u32(cur.batches.size()));
			++stats.stepChunks[std::min(g.k, 7)];
			out.push_back(std::move(cur));
		};

		begin();
		for (u32 t : tris) {
			const WorkTri& tri = work[t];
			const int material = remap[tri.material];

			// Texture coordinates relative to the material's base; fall back to a
			// per-triangle base (costs shared vertices) when the span is too wide.
			float base[2] = {uvBase[material].first, uvBase[material].second};
			float triMin[2] = {1e30f, 1e30f}, triMax[2] = {-1e30f, -1e30f};
			for (const SrcVertex& v : tri.v)
				for (int a = 0; a < 2; ++a) {
					triMin[a] = std::min(triMin[a], v.uv[a]);
					triMax[a] = std::max(triMax[a], v.uv[a]);
				}
			bool rebased = false;
			for (int a = 0; a < 2; ++a) {
				if (triMax[a] - base[a] > kUvLimit) {
					base[a] = std::floor(triMin[a]);
					rebased = true;
				}
			}
			if (rebased) ++stats.uvRebased;
			if (triMax[0] - base[0] > kUvLimit || triMax[1] - base[1] > kUvLimit) ++stats.uvClamped;

			WorldVertex q[3];
			i32 su[3], sv[3]; // texture coordinates in fixed point, before range limiting
			for (int k = 0; k < 3; ++k) {
				const SrcVertex& v = tri.v[k];
				const std::array<i64, 3>& l = lattice[tri.c[k]];
				auto quant = [&](int a) { return i16((l[a] - g.origin[a]) >> g.k); };
				auto quantUv = [&](int a) {
					float s = (v.uv[a] - base[a]) / WORLD_UV_SCALE;
					return i32(std::lround(std::clamp(s, -1.0e6f, 1.0e6f)));
				};
				q[k].x = quant(0);
				q[k].y = quant(1);
				q[k].z = quant(2);
				q[k].w = opt.vertexFx ? packVertexFx(v.occlusion, v.wind) : i16(1);
				su[k] = quantUv(0);
				sv[k] = quantUv(1);
				q[k].u = q[k].v = 0;
				std::memcpy(&q[k].r, v.rgba, 4);
			}

			// Drop triangles that quantisation (or the source) collapsed.
			auto samePos = [](const WorldVertex& a, const WorldVertex& b) {
				return a.x == b.x && a.y == b.y && a.z == b.z;
			};
			if (samePos(q[0], q[1]) || samePos(q[1], q[2]) || samePos(q[0], q[2])) {
				++stats.degenerate;
				continue;
			}

			if (cur.vertices.size() + 3 > opt.maxVertices) {
				flush();
				begin();
			}
			// ZenGin keeps every polygon's UVs near zero, so neighbouring polygons
			// agree at a shared corner only up to whole texture repeats. Move this
			// triangle by the whole-repeat offset that lets it reuse the most
			// vertices already in the chunk (the texture wraps, so nothing changes
			// on screen).
			struct Shift {
				i32 u, v;
			};
			auto fits = [](i32 c) { return c >= -32767 && c <= 32767; };
			std::vector<Shift> candidates = {{0, 0}}; // first, so ties keep the triangle where it is
			for (int k = 0; k < 3; ++k) {
				auto it = byFraction.find(fractionKey(q[k], su[k], sv[k]));
				if (it == byFraction.end()) continue;
				for (const std::pair<i32, i32>& uv : it->second) {
					const Shift s {uv.first - su[k], uv.second - sv[k]};
					bool known = false;
					for (const Shift& c : candidates) known = known || (c.u == s.u && c.v == s.v);
					if (!known && candidates.size() < 16) candidates.push_back(s);
				}
			}
			Shift best {0, 0};
			int bestScore = -1;
			for (const Shift& s : candidates) {
				int score = 0;
				bool ok = true;
				for (int k = 0; k < 3 && ok; ++k) {
					ok = fits(su[k] + s.u) && fits(sv[k] + s.v);
					if (!ok) break;
					WorldVertex c = q[k];
					c.u = i16(su[k] + s.u);
					c.v = i16(sv[k] + s.v);
					score += int(lookup.count(VertexKey {c}));
				}
				if (ok && score > bestScore) {
					best = s;
					bestScore = score;
				}
			}
			for (int k = 0; k < 3; ++k) {
				su[k] += best.u;
				sv[k] += best.v;
				q[k].u = i16(std::clamp(su[k], -32767, 32767));
				q[k].v = i16(std::clamp(sv[k], -32767, 32767));
			}

			if (cur.batches.empty() || cur.batches.back().material != u16(material) ||
			    cur.batches.back().detail != detailOf(tri) || cur.batches.back().cluster != clusterId(t)) {
				ChunkBatch b {};
				b.material = u16(material);
				b.detail = detailOf(tri);
				b.cluster = clusterId(t);
				b.firstIndex = u32(cur.indices.size());
				cur.batches.push_back(b);
			}
			for (int k = 0; k < 3; ++k) {
				const u64 posKey = (u64(u16(q[k].x)) << 32) | (u64(u16(q[k].y)) << 16) | u64(u16(q[k].z));
				stats.uniquePositions += seenPos.insert(posKey).second;
				WorldVertex noLight = q[k];
				noLight.r = noLight.g = noLight.b = noLight.a = 0;
				stats.uniquePosUv += seenPosUv.emplace(VertexKey {noLight}, u16(0)).second;
				auto ins = lookup.emplace(VertexKey {q[k]}, u16(cur.vertices.size()));
				if (ins.second) {
					cur.vertices.push_back(q[k]);
					byFraction[fractionKey(q[k], su[k], sv[k])].emplace_back(q[k].u, q[k].v);
				}
				cur.indices.push_back(ins.first->second);
			}
			cur.batches.back().indexCount += 3;
		}
		flush();
		// More than one part because of the vertex limit (not because giant
		// triangles went into a part of their own).
		if (part - firstPart > 1) ++stats.splitChunks;
	}
};

} // namespace

std::vector<BakedChunk> buildChunks(const SrcScene& scene, const std::vector<int>& materialRemap,
                                    const std::vector<u8>& materialOrder, const ChunkerOptions& opt,
                                    ChunkerStats& stats) {
	const double step0 = double(opt.positionStep);

	// ---- 1. one cluster per place ----
	Clusters clusters;
	// Never finer than the lattice: what rounds to one point is one point.
	clusters.eps = opt.seal ? std::max(double(opt.sealTolerance) * 0.5, step0 * 0.5) : 0.0;
	std::vector<WorkTri> input;
	for (const SrcTriangle& tri : scene.triangles) {
		if (materialRemap[tri.material] < 0) continue;
		++stats.inputTriangles;
		WorkTri w;
		for (int k = 0; k < 3; ++k) {
			w.v[k] = tri.v[k];
			w.c[k] = clusters.find(tri.v[k].pos);
		}
		w.material = tri.material;
		w.detail = tri.detail;
		input.push_back(w);
	}
	stats.welded = clusters.merged;

	// ---- 2. cut edges at the vertices lying on them ----
	std::vector<WorkTri> work;
	if (opt.seal && opt.sealTolerance > 0) {
		work.reserve(input.size() + input.size() / 8);
		Sealer sealer(clusters, double(opt.sealTolerance), stats, work);
		for (const WorkTri& t : input) sealer.add(t);
		input = std::vector<WorkTri>();
	} else {
		for (const WorkTri& t : input) {
			if (t.c[0] == t.c[1] || t.c[1] == t.c[2] || t.c[0] == t.c[2])
				++stats.degenerate;
			else
				work.push_back(t);
		}
		input = std::vector<WorkTri>();
	}

	// ---- 3. grid cells (by centroid, XZ plane) and their quantisation frames ----
	std::map<std::pair<int, int>, std::vector<u32>> cells;
	for (u32 t = 0; t < work.size(); ++t) {
		const P3 &a = clusters.pos[work[t].c[0]], &b = clusters.pos[work[t].c[1]], &c = clusters.pos[work[t].c[2]];
		const double cx = (a.x + b.x + c.x) / 3.0, cz = (a.z + b.z + c.z) / 3.0;
		cells[{int(std::floor(cx / opt.chunkSize)), int(std::floor(cz / opt.chunkSize))}].push_back(t);
	}
	// The step a cell is planned for: fine enough, and still reaching half a
	// cell beyond the cell's own borders.
	int cellK = 0;
	while (kFit * step0 * double(i64(1) << cellK) < double(opt.chunkSize)) ++cellK;
	auto stepFor = [&](double extent, int from) {
		int k = from;
		while (extent > 2.0 * kFit * step0 * double(i64(1) << k)) ++k;
		return k;
	};
	auto extentOf = [&](const std::vector<u32>& tris) {
		double mn[3] = {1e300, 1e300, 1e300}, mx[3] = {-1e300, -1e300, -1e300};
		for (u32 t : tris)
			for (u32 c : work[t].c) {
				const double p[3] = {clusters.pos[c].x, clusters.pos[c].y, clusters.pos[c].z};
				for (int a = 0; a < 3; ++a) {
					mn[a] = std::min(mn[a], p[a]);
					mx[a] = std::max(mx[a], p[a]);
				}
			}
		return std::max({mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]});
	};
	// Each cell by tier (the lowest detail class in it decides how far from the camera the chunk is
	// wanted, so a tier is never more than what it is called for).
	struct CellTier {
		int gx, gz;
		u8 tier;
		std::vector<u32> tris;
	};
	std::vector<CellTier> cellTiers;
	for (auto& kv : cells) {
		std::vector<u32>& tris = kv.second;
		if (!opt.splitDetail) {
			cellTiers.push_back(CellTier {kv.first.first, kv.first.second, 0, std::move(tris)});
			continue;
		}
		std::vector<u32> byClass[DETAIL_COUNT];
		for (u32 t : tris) byClass[CellBuilder::classOf(work[t])].push_back(t);
		// Too few to be worth a chunk: fold into the tier above, small into medium into the world.
		for (int c = DETAIL_COUNT - 1; c > 0; --c)
			if (byClass[c].size() < opt.minTierTriangles) {
				byClass[c - 1].insert(byClass[c - 1].end(), byClass[c].begin(), byClass[c].end());
				byClass[c].clear();
			}
		for (int c = 0; c < DETAIL_COUNT; ++c)
			if (!byClass[c].empty()) cellTiers.push_back(CellTier {kv.first.first, kv.first.second, u8(c), std::move(byClass[c])});
	}
	cells.clear();
	std::vector<Group> groups;
	for (CellTier& ct : cellTiers) {
		std::vector<u32>& tris = ct.tris;
		Group g;
		g.gx = ct.gx;
		g.gz = ct.gz;
		g.tier = ct.tier;
		g.k = stepFor(extentOf(tris), 0);
		if (g.k <= cellK) {
			g.tris = std::move(tris);
			groups.push_back(std::move(g));
			continue;
		}
		// Too large for the planned step: what stays near the cell keeps it,
		// the giant triangles go into a frame of their own.
		std::vector<double> ys;
		for (u32 t : tris)
			for (u32 c : work[t].c) ys.push_back(clusters.pos[c].y);
		std::nth_element(ys.begin(), ys.begin() + ys.size() / 2, ys.end());
		const double centre[3] = {(double(g.gx) + 0.5) * opt.chunkSize, ys[ys.size() / 2], (double(g.gz) + 0.5) * opt.chunkSize};
		const double reach = kFit * step0 * double(i64(1) << cellK);
		Group far = g;
		for (u32 t : tris) {
			bool inside = true;
			for (u32 c : work[t].c) {
				const double p[3] = {clusters.pos[c].x, clusters.pos[c].y, clusters.pos[c].z};
				for (int a = 0; a < 3; ++a) inside = inside && std::fabs(p[a] - centre[a]) <= reach;
			}
			(inside ? g : far).tris.push_back(t);
		}
		if (!g.tris.empty()) {
			g.k = cellK;
			groups.push_back(std::move(g));
		}
		if (!far.tris.empty()) {
			far.k = stepFor(extentOf(far.tris), cellK);
			groups.push_back(std::move(far));
		}
	}

	// ---- 4. lattice positions ----
	// A place used by chunks of different steps must exist on the coarsest of
	// their lattices (which is part of every finer one), or the chunks would
	// disagree about it.
	std::vector<std::array<i64, 3>> lattice(clusters.pos.size());
	std::vector<u8> clusterK(clusters.pos.size());
	for (bool settled = false; !settled;) {
		settled = true;
		std::fill(clusterK.begin(), clusterK.end(), u8(0));
		for (const Group& g : groups)
			for (u32 t : g.tris)
				for (u32 c : work[t].c) clusterK[c] = std::max(clusterK[c], u8(g.k));
		for (size_t c = 0; c < lattice.size(); ++c) {
			const double s = step0 * double(i64(1) << clusterK[c]);
			const double p[3] = {clusters.pos[c].x, clusters.pos[c].y, clusters.pos[c].z};
			for (int a = 0; a < 3; ++a) lattice[c][size_t(a)] = i64(std::llround(p[a] / s)) << clusterK[c];
		}
		for (Group& g : groups) {
			i64 mn[3] = {INT64_MAX, INT64_MAX, INT64_MAX}, mx[3] = {INT64_MIN, INT64_MIN, INT64_MIN};
			for (u32 t : g.tris)
				for (u32 c : work[t].c)
					for (int a = 0; a < 3; ++a) {
						mn[a] = std::min(mn[a], lattice[c][size_t(a)]);
						mx[a] = std::max(mx[a], lattice[c][size_t(a)]);
					}
			bool fits = true;
			for (int a = 0; a < 3; ++a) {
				g.origin[a] = floorTo(mn[a] + (mx[a] - mn[a]) / 2, g.k);
				fits = fits && ((mx[a] - g.origin[a]) >> g.k) <= kRange && ((g.origin[a] - mn[a]) >> g.k) <= kRange;
			}
			if (!fits) {
				if (g.k >= 40) throw std::runtime_error("chunker: geometry does not fit any position step");
				++g.k; // rounding pushed it over the edge: one step coarser, and round again
				settled = false;
			}
		}
	}

	std::vector<BakedChunk> out;
	CellBuilder builder {work, lattice, materialRemap, materialOrder, opt, stats, out, {}};
	for (Group& g : groups) builder.build(g);
	return out;
}

} // namespace g3
