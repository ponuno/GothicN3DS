#include "lod.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <queue>
#include <thread>
#include <unordered_map>

namespace g3 {

namespace {

struct Tri {
	u16 v[3];
	u16 material;
	u8 cluster;
	u8 group = 0; // ChunkBatch::detail without the stand-in bits: what a place may not be shared across
	bool alive = true;
};

struct Quadric {
	double a[10] = {}; // symmetric 4x4: xx xy xz xd yy yz yd zz zd dd
	double planes = 0;
	void add(double nx, double ny, double nz, double d) {
		a[0] += nx * nx; a[1] += nx * ny; a[2] += nx * nz; a[3] += nx * d;
		a[4] += ny * ny; a[5] += ny * nz; a[6] += ny * d;
		a[7] += nz * nz; a[8] += nz * d;
		a[9] += d * d;
		planes += 1.0;
	}
	void add(const Quadric& o) {
		for (int i = 0; i < 10; ++i) a[i] += o.a[i];
		planes += o.planes;
	}
	// Sum of the squared distances of p to the planes.
	double eval(double x, double y, double z) const {
		return a[0] * x * x + 2 * a[1] * x * y + 2 * a[2] * x * z + 2 * a[3] * x + a[4] * y * y + 2 * a[5] * y * z + 2 * a[6] * y +
		       a[7] * z * z + 2 * a[8] * z + a[9];
	}
};

struct P3 {
	double x, y, z;
};
P3 sub(P3 a, P3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
P3 cross(P3 a, P3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double dot(P3 a, P3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

struct Candidate {
	double cost;
	u32 from, to;
	bool operator<(const Candidate& o) const { return cost > o.cost; } // (a min-heap)
};

// Edge collapse on a mesh whose vertices carry texture coordinates and light: polygons that meet at a place
// usually do not share a vertex (each has its own coordinates), so the mesh is treated by position - one node
// per place - and a collapse moves every corner at the place to a vertex at the other one: the vertex of the
// triangle's own neighbourhood whose coordinates and light come closest to what the triangle had there. A
// corner gets, if none of the vertices there fits what the triangle itself had at that place, a new vertex with
// exactly that: the texture and the light of the triangle go on as they were.
class Simplifier {
public:
	// `lockClusters`: what two clusters share stays as it is (stand-ins, which are switched on per cluster).
	// A place shared by two detail groups always stays: one of them may be drawn without the other.
	Simplifier(const BakedChunk& chunk, std::vector<Tri> tris, const std::vector<bool>& usedOutside, float maxError, bool lockClusters = true)
	    : _c(chunk), _tris(std::move(tris)), _maxErr2(double(maxError) * double(maxError)) {
		const size_t n = chunk.vertices.size();
		_vertexCount = n;
		const double scale = chunk.header.posScale;
		_pid.assign(n, 0xFFFFFFFFu);
		std::unordered_map<u64, u32> byPlace;
		for (const Tri& t : _tris)
			for (int k = 0; k < 3; ++k) {
				const u32 v = t.v[k];
				if (_pid[v] != 0xFFFFFFFFu) continue;
				const WorldVertex& w = chunk.vertices[v];
				const u64 key = (u64(u16(w.x)) << 32) | (u64(u16(w.y)) << 16) | u64(u16(w.z));
				auto it = byPlace.find(key);
				if (it == byPlace.end()) {
					it = byPlace.emplace(key, u32(_pos.size())).first;
					_pos.push_back({double(w.x) * scale, double(w.y) * scale, double(w.z) * scale});
					_vertsAt.emplace_back();
				}
				_pid[v] = it->second;
				_vertsAt[it->second].push_back(u16(v));
			}
		// (Vertices of triangles dropped below as degenerate keep their place.)
		const size_t places = _pos.size();
		_inc.resize(places);
		_locked.assign(places, false);
		_quadric.resize(places);
		for (u32 t = 0; t < _tris.size(); ++t) {
			Tri& tri = _tris[t];
			const u32 p0 = _pid[tri.v[0]], p1 = _pid[tri.v[1]], p2 = _pid[tri.v[2]];
			if (p0 == p1 || p1 == p2 || p0 == p2) {
				tri.alive = false; // (a sliver: it is not part of the stand-in)
				continue;
			}
			for (int k = 0; k < 3; ++k) _inc[_pid[tri.v[k]]].push_back(t);
		}
		for (size_t v = 0; v < n; ++v)
			if (_pid[v] != 0xFFFFFFFFu && usedOutside[v]) _locked[_pid[v]] = true;
		lockBorders();
		// The stand-ins are switched on per cluster (a neighbouring cluster may still be drawn in full), so what
		// two clusters share stays as it is: no crack opens between them.
		for (u32 p = 0; p < places; ++p) {
			int first = -1, firstGroup = -1;
			for (u32 t : _inc[p]) {
				if (first < 0) {
					first = _tris[t].cluster;
					firstGroup = _tris[t].group;
				}
				if ((lockClusters && int(_tris[t].cluster) != first) || int(_tris[t].group) != firstGroup) {
					_locked[p] = true;
					break;
				}
			}
		}
		for (u32 t = 0; t < _tris.size(); ++t) {
			double nx, ny, nz, d;
			if (!_tris[t].alive || !plane(_tris[t], nx, ny, nz, d)) continue;
			for (int k = 0; k < 3; ++k) _quadric[_pid[_tris[t].v[k]]].add(nx, ny, nz, d);
		}
		_alive = 0;
		for (const Tri& t : _tris) _alive += t.alive;
	}

	// Collapses until `target` triangles are left or nothing may go.
	void run(u32 target) {
		std::priority_queue<Candidate> heap;
		for (u32 p = 0; p < _inc.size(); ++p)
			if (!_inc[p].empty()) push(heap, p);
		while (_alive > target && !heap.empty()) {
			const Candidate c = heap.top();
			heap.pop();
			if (_inc[c.from].empty() || _inc[c.to].empty()) continue;
			// What it costs may have grown since it was queued (its end took in another place's planes).
			const double now = cost(c.from, c.to);
			if (now > c.cost * 1.000001 + 1e-9) {
				if (now <= _maxErr2) heap.push({now, c.from, c.to});
				continue;
			}
			if (!legal(c.from, c.to)) continue;
			collapse(c.from, c.to, heap);
		}
	}

	const std::vector<Tri>& tris() const { return _tris; }
	// Vertices made for the stand-in (to be appended to the chunk's, from index vertexCount()).
	const std::vector<WorldVertex>& added() const { return _added; }
	u32 alive() const { return _alive; }

private:
	const BakedChunk& _c;
	std::vector<Tri> _tris;
	double _maxErr2;
	std::vector<u32> _pid;                // vertex -> place
	std::vector<P3> _pos;                 // place -> position
	std::vector<std::vector<u16>> _vertsAt; // place -> its vertices
	std::vector<std::vector<u32>> _inc;   // place -> triangles with a corner there
	std::vector<bool> _locked;
	std::vector<Quadric> _quadric;
	u32 _alive = 0;
	size_t _vertexCount = 0;
	std::vector<WorldVertex> _added;
	static constexpr size_t kMaxVertices = 65535;

	const WorldVertex& vertexAt(u32 v) const { return v < _vertexCount ? _c.vertices[v] : _added[v - _vertexCount]; }

	// How far a corner's coordinates and light may be off what the triangle had at that place.
	static constexpr double kUvTolerance = 0.01; // texture repeats
	static constexpr double kLightTolerance = 10.0; // of 255

	bool plane(const Tri& t, double& nx, double& ny, double& nz, double& d) const {
		const P3 a = _pos[_pid[t.v[0]]], b = _pos[_pid[t.v[1]]], c = _pos[_pid[t.v[2]]];
		const P3 n = cross(sub(b, a), sub(c, a));
		const double len = std::sqrt(dot(n, n));
		if (len < 1e-9) return false;
		nx = n.x / len;
		ny = n.y / len;
		nz = n.z / len;
		d = -(nx * a.x + ny * a.y + nz * a.z);
		return true;
	}

	// Places that stay: the border of the simplified mesh and a change of material.
	void lockBorders() {
		struct Edge {
			u64 key;
			u32 tri;
		};
		std::vector<Edge> edges;
		edges.reserve(_tris.size() * 3);
		for (u32 t = 0; t < _tris.size(); ++t) {
			if (!_tris[t].alive) continue;
			for (int k = 0; k < 3; ++k) {
				const u32 a = _pid[_tris[t].v[k]], b = _pid[_tris[t].v[(k + 1) % 3]];
				edges.push_back({(u64(std::min(a, b)) << 32) | std::max(a, b), t});
			}
		}
		std::sort(edges.begin(), edges.end(), [](const Edge& l, const Edge& r) { return l.key < r.key; });
		for (size_t i = 0; i < edges.size();) {
			size_t e = i;
			while (e < edges.size() && edges[e].key == edges[i].key) ++e;
			bool border = (e - i) != 2;
			if (!border && _tris[edges[i].tri].material != _tris[edges[i + 1].tri].material) border = true;
			if (border) {
				_locked[u32(edges[i].key >> 32)] = true;
				_locked[u32(edges[i].key & 0xFFFFFFFFu)] = true;
			}
			i = e;
		}
	}

	void neighbours(u32 p, std::vector<u32>& out) const {
		out.clear();
		for (u32 t : _inc[p])
			for (int k = 0; k < 3; ++k) {
				const u32 o = _pid[_tris[t].v[k]];
				if (o != p) out.push_back(o);
			}
		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());
	}

	double cost(u32 from, u32 to) const {
		Quadric q = _quadric[from];
		q.add(_quadric[to]);
		return q.planes > 0 ? q.eval(_pos[to].x, _pos[to].y, _pos[to].z) / q.planes : 0.0;
	}

	void push(std::priority_queue<Candidate>& heap, u32 from) const {
		if (_locked[from]) return;
		std::vector<u32> nb;
		neighbours(from, nb);
		for (u32 to : nb) {
			const double c = cost(from, to);
			if (c <= _maxErr2) heap.push({c, from, to});
		}
	}

	struct Attr {
		double v[6]; // u, v (repeats), r, g, b, a
	};
	Attr attrOf(const WorldVertex& w) const {
		const double uv = _c.header.uvScale;
		return {{double(w.u) * uv, double(w.v) * uv, double(w.r), double(w.g), double(w.b), double(w.a)}};
	}

	// What triangle `t` (which has a corner at `from`) has at the place `to`: its own mapping carried there.
	// 0: it cannot be told; 1: a vertex there is close enough (`out`); 2: none is, a new one is needed (`made`).
	int chooseVertex(const Tri& t, u32 from, u32 to, u32& out, WorldVertex& made) const {
		int fk = -1;
		for (int k = 0; k < 3; ++k)
			if (_pid[t.v[k]] == from) fk = k;
		const P3 pa = _pos[from];
		const P3 e1 = sub(_pos[_pid[t.v[(fk + 1) % 3]]], pa), e2 = sub(_pos[_pid[t.v[(fk + 2) % 3]]], pa), d = sub(_pos[to], pa);
		const double a11 = dot(e1, e1), a12 = dot(e1, e2), a22 = dot(e2, e2);
		const double det = a11 * a22 - a12 * a12;
		if (det < 1e-9) return 0;
		const double r1 = dot(e1, d), r2 = dot(e2, d);
		const double s = (r1 * a22 - r2 * a12) / det, u = (a11 * r2 - a12 * r1) / det;
		if (std::fabs(s) > 8.0 || std::fabs(u) > 8.0) return 0;
		const Attr A = attrOf(vertexAt(t.v[fk])), X = attrOf(vertexAt(t.v[(fk + 1) % 3])), Y = attrOf(vertexAt(t.v[(fk + 2) % 3]));
		double want[6];
		for (int i = 0; i < 6; ++i) want[i] = A.v[i] + s * (X.v[i] - A.v[i]) + u * (Y.v[i] - A.v[i]);
		double best = 1e30;
		for (u16 cand : _vertsAt[to]) {
			const Attr C = attrOf(vertexAt(cand));
			const double eu = std::max(std::fabs(C.v[0] - want[0]), std::fabs(C.v[1] - want[1]));
			double el = 0;
			for (int i = 2; i < 6; ++i) el = std::max(el, std::fabs(C.v[i] - want[i]));
			if (eu > kUvTolerance || el > kLightTolerance) continue;
			const double score = eu / kUvTolerance + el / kLightTolerance;
			if (score < best) {
				best = score;
				out = cand;
			}
		}
		if (best < 1e29) return 1;
		// A new one: the place's own vertex with the triangle's coordinates and light.
		const double uvScale = _c.header.uvScale > 0 ? _c.header.uvScale : 1.0;
		const double qu = std::floor(want[0] / uvScale + 0.5), qv = std::floor(want[1] / uvScale + 0.5);
		if (qu < -32768 || qu > 32767 || qv < -32768 || qv > 32767) return 0;
		made = vertexAt(_vertsAt[to][0]);
		made.u = i16(qu);
		made.v = i16(qv);
		made.r = u8(int(clampd(std::floor(want[2] + 0.5), 0, 255)));
		made.g = u8(int(clampd(std::floor(want[3] + 0.5), 0, 255)));
		made.b = u8(int(clampd(std::floor(want[4] + 0.5), 0, 255)));
		made.a = u8(int(clampd(std::floor(want[5] + 0.5), 0, 255)));
		return 2;
	}

	static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

	bool legal(u32 from, u32 to) const {
		if (_locked[from]) return false;
		std::vector<u32> na, nb;
		neighbours(from, na);
		neighbours(to, nb);
		// The two ends may share only the places opposite the edge (the link condition): more would fold the mesh.
		u32 common = 0, shared = 0;
		for (u32 x : na)
			if (std::binary_search(nb.begin(), nb.end(), x)) ++common;
		for (u32 t : _inc[from])
			for (int k = 0; k < 3; ++k)
				if (_pid[_tris[t].v[k]] == to) {
					++shared;
					break;
				}
		if (shared != 2 || common != 2) return false;
		u32 needed = 0;
		for (u32 t : _inc[from]) {
			const Tri& tri = _tris[t];
			bool hasTo = false;
			for (int k = 0; k < 3; ++k) hasTo |= _pid[tri.v[k]] == to;
			if (hasTo) continue;
			// No triangle may turn over or collapse to a sliver.
			P3 before[3], after[3];
			for (int k = 0; k < 3; ++k) {
				const u32 p = _pid[tri.v[k]];
				before[k] = _pos[p];
				after[k] = p == from ? _pos[to] : before[k];
			}
			const P3 n0 = cross(sub(before[1], before[0]), sub(before[2], before[0]));
			const P3 n1 = cross(sub(after[1], after[0]), sub(after[2], after[0]));
			const double l0 = std::sqrt(dot(n0, n0)), l1 = std::sqrt(dot(n1, n1));
			if (l1 < 1e-6 || l0 < 1e-9) return false;
			if (dot(n0, n1) < 0.3 * l0 * l1) return false;
			u32 pick = 0;
			WorldVertex made {};
			const int how = chooseVertex(tri, from, to, pick, made);
			if (how == 0) return false;
			needed += how == 2;
		}
		return _vertexCount + _added.size() + needed <= kMaxVertices;
	}

	void collapse(u32 from, u32 to, std::priority_queue<Candidate>& heap) {
		std::vector<u32> around;
		neighbours(from, around);
		const std::vector<u32> incFrom = _inc[from];
		for (u32 t : incFrom) {
			Tri& tri = _tris[t];
			bool hasTo = false;
			for (int k = 0; k < 3; ++k) hasTo |= _pid[tri.v[k]] == to;
			if (hasTo) {
				tri.alive = false;
				--_alive;
				for (int k = 0; k < 3; ++k) {
					const u32 p = _pid[tri.v[k]];
					if (p == from) continue;
					auto& list = _inc[p];
					list.erase(std::remove(list.begin(), list.end(), t), list.end());
				}
			} else {
				u32 pick = 0;
				WorldVertex made {};
				if (chooseVertex(tri, from, to, pick, made) == 2) {
					pick = u32(_vertexCount + _added.size());
					_added.push_back(made);
					_pid.push_back(to);
					_vertsAt[to].push_back(u16(pick));
				}
				for (int k = 0; k < 3; ++k)
					if (_pid[tri.v[k]] == from) tri.v[k] = u16(pick);
				_inc[to].push_back(t);
			}
		}
		_inc[from].clear();
		_quadric[to].add(_quadric[from]);
		push(heap, to);
		for (u32 n : around)
			if (n != to && !_inc[n].empty()) push(heap, n);
	}
};

// One chunk. Returns false when it is left as it is.
enum class Result { Done, NotTierZero, TooFew, TooLittle, Other };

Result simplifyChunk(BakedChunk& c, const std::vector<WorldMaterial>& materials, const LodOptions& opt, u64& before, u64& after, u64& simplifiable) {
	if (c.tier != 0) return Result::NotTierZero;
	if (c.vertices.empty() || c.vertices.size() > 65535) return Result::Other;
	// Which batches are simplified: opaque world triangles of the class that is always drawn.
	std::vector<bool> take(c.batches.size(), false);
	std::vector<Tri> tris;
	for (size_t b = 0; b < c.batches.size(); ++b) {
		const ChunkBatch& cb = c.batches[b];
		if ((cb.detail & 3) != DETAIL_ALWAYS || batchLod(cb.detail) != BATCH_LOD_BOTH || cb.material >= materials.size()) continue;
		const u16 bad = MATF_ALPHA_TEST | MATF_BLEND | MATF_ADD | MATF_WATER | MATF_WIND;
		if (materials[cb.material].flags & bad) continue;
		take[b] = true;
		for (u32 i = 0; i + 2 < cb.indexCount; i += 3) {
			Tri t;
			for (int k = 0; k < 3; ++k) t.v[k] = c.indices[cb.firstIndex + i + k];
			t.material = cb.material;
			t.cluster = cb.cluster;
			tris.push_back(t);
		}
	}
	if (tris.size() < opt.minTriangles) return Result::TooFew;
	simplifiable += tris.size();
	// Vertices that something not simplified also uses stay.
	std::vector<bool> usedOutside(c.vertices.size(), false);
	for (size_t b = 0; b < c.batches.size(); ++b) {
		if (take[b] || batchLod(c.batches[b].detail) == BATCH_LOD_FAR_ONLY) continue;
		const ChunkBatch& cb = c.batches[b];
		for (u32 i = 0; i < cb.indexCount; ++i) usedOutside[c.indices[cb.firstIndex + i]] = true;
	}
	const u32 initial = u32(tris.size());
	Simplifier simp(c, std::move(tris), usedOutside, opt.maxError);
	simp.run(u32(double(initial) * double(opt.ratio)));
	if (simp.alive() > u32(double(initial) * 0.9)) return Result::TooLittle; // not worth a second picture

	// The vertices the stand-in needed that the chunk did not have.
	c.vertices.insert(c.vertices.end(), simp.added().begin(), simp.added().end());
	c.header.vertexCount = u16(c.vertices.size());
	// The stand-ins: by material, then cluster, as the batches themselves are.
	std::vector<const Tri*> kept;
	for (const Tri& t : simp.tris())
		if (t.alive) kept.push_back(&t);
	std::stable_sort(kept.begin(), kept.end(), [](const Tri* l, const Tri* r) {
		if (l->material != r->material) return l->material < r->material;
		return l->cluster < r->cluster;
	});
	std::vector<ChunkBatch> added;
	std::vector<u16> indices = c.indices;
	for (size_t i = 0; i < kept.size();) {
		size_t e = i;
		while (e < kept.size() && kept[e]->material == kept[i]->material && kept[e]->cluster == kept[i]->cluster) ++e;
		ChunkBatch nb {};
		nb.material = kept[i]->material;
		nb.detail = withBatchLod(DETAIL_ALWAYS, BATCH_LOD_FAR_ONLY);
		nb.cluster = kept[i]->cluster;
		nb.firstIndex = u32(indices.size());
		nb.indexCount = u32(e - i) * 3;
		i16 mn[3] = {32767, 32767, 32767}, mx[3] = {-32768, -32768, -32768};
		for (size_t k = i; k < e; ++k)
			for (int a = 0; a < 3; ++a) {
				const u16 vi = kept[k]->v[a];
				indices.push_back(vi);
				const WorldVertex& w = c.vertices[vi];
				const i16 p[3] = {w.x, w.y, w.z};
				for (int d = 0; d < 3; ++d) {
					mn[d] = std::min(mn[d], p[d]);
					mx[d] = std::max(mx[d], p[d]);
				}
			}
		std::memcpy(nb.bbMin, mn, sizeof(mn));
		std::memcpy(nb.bbMax, mx, sizeof(mx));
		added.push_back(nb);
		i = e;
	}
	if (c.batches.size() + added.size() > 65535) { // (never so many batches)
		c.vertices.resize(c.vertices.size() - simp.added().size());
		c.header.vertexCount = u16(c.vertices.size());
		return Result::Other;
	}
	for (size_t b = 0; b < c.batches.size(); ++b)
		if (take[b]) c.batches[b].detail = withBatchLod(c.batches[b].detail, BATCH_LOD_FULL_ONLY);
	c.batches.insert(c.batches.end(), added.begin(), added.end());
	c.indices.swap(indices);
	c.header.batchCount = u16(c.batches.size());
	c.header.indexCount = u32(c.indices.size());
	before += initial;
	after += kept.size();
	return Result::Done;
}

// ---- --simplify: the geometry itself, in place ----

enum class InPlace { Done, TooFew, TooLittle, Other };

// The triangles of every simplifiable batch (any detail class) with fewer of them, written back as the batches
// they came from, in the same order; the vertices nothing uses any more are dropped.
InPlace simplifyInPlace(BakedChunk& c, const std::vector<WorldMaterial>& materials, const SimplifyOptions& opt, u64& before, u64& after) {
	if (c.vertices.empty() || c.vertices.size() > 65535 || c.batches.empty()) return InPlace::Other;
	std::vector<bool> take(c.batches.size(), false);
	std::vector<Tri> tris;
	for (size_t b = 0; b < c.batches.size(); ++b) {
		const ChunkBatch& cb = c.batches[b];
		if (batchLod(cb.detail) != BATCH_LOD_BOTH || cb.material >= materials.size()) continue;
		// What is cut out of a texture, blended, animated by the wind or water stays: its triangles are its shape.
		const u16 bad = MATF_ALPHA_TEST | MATF_BLEND | MATF_ADD | MATF_WATER | MATF_WIND;
		if (materials[cb.material].flags & bad) continue;
		take[b] = true;
		for (u32 i = 0; i + 2 < cb.indexCount; i += 3) {
			Tri t;
			for (int k = 0; k < 3; ++k) t.v[k] = c.indices[cb.firstIndex + i + k];
			t.material = cb.material;
			t.cluster = cb.cluster;
			t.group = u8(cb.detail & 0x3Fu);
			tris.push_back(t);
		}
	}
	if (tris.size() < opt.minTriangles) return InPlace::TooFew;
	std::vector<bool> usedOutside(c.vertices.size(), false);
	for (size_t b = 0; b < c.batches.size(); ++b) {
		if (take[b]) continue;
		const ChunkBatch& cb = c.batches[b];
		for (u32 i = 0; i < cb.indexCount; ++i) usedOutside[c.indices[cb.firstIndex + i]] = true;
	}
	const u32 initial = u32(tris.size());
	Simplifier simp(c, std::move(tris), usedOutside, opt.maxError, /*lockClusters=*/false);
	simp.run(u32(double(initial) * double(opt.ratio)));
	if (simp.alive() > u32(double(initial) * 0.97)) return InPlace::TooLittle;

	c.vertices.insert(c.vertices.end(), simp.added().begin(), simp.added().end());
	// What is left of each (material, cluster, detail) triple, to go where its batch was.
	struct Key {
		u16 material;
		u8 cluster, group;
		bool operator<(const Key& o) const {
			return material != o.material ? material < o.material : cluster != o.cluster ? cluster < o.cluster : group < o.group;
		}
	};
	std::map<Key, std::vector<const Tri*>> kept;
	for (const Tri& t : simp.tris())
		if (t.alive) kept[{t.material, t.cluster, t.group}].push_back(&t);
	std::vector<ChunkBatch> batches;
	std::vector<u16> indices;
	indices.reserve(c.indices.size());
	for (size_t b = 0; b < c.batches.size(); ++b) {
		const ChunkBatch& old = c.batches[b];
		ChunkBatch nb = old;
		nb.firstIndex = u32(indices.size());
		if (!take[b]) {
			for (u32 i = 0; i < old.indexCount; ++i) indices.push_back(c.indices[old.firstIndex + i]);
			batches.push_back(nb);
			continue;
		}
		const auto it = kept.find({old.material, old.cluster, u8(old.detail & 0x3Fu)});
		if (it == kept.end() || it->second.empty()) continue; // (nothing of it is left: it was all slivers)
		i16 mn[3] = {32767, 32767, 32767}, mx[3] = {-32768, -32768, -32768};
		for (const Tri* t : it->second)
			for (int a = 0; a < 3; ++a) {
				const u16 vi = t->v[a];
				indices.push_back(vi);
				const WorldVertex& w = c.vertices[vi];
				const i16 p[3] = {w.x, w.y, w.z};
				for (int d = 0; d < 3; ++d) {
					mn[d] = std::min(mn[d], p[d]);
					mx[d] = std::max(mx[d], p[d]);
				}
			}
		nb.indexCount = u32(it->second.size()) * 3;
		std::memcpy(nb.bbMin, mn, sizeof(mn));
		std::memcpy(nb.bbMax, mx, sizeof(mx));
		batches.push_back(nb);
		it->second.clear(); // (a triple that came twice: its triangles go out once)
	}
	// Drop the vertices nothing uses, keeping the order of the others.
	std::vector<u32> remap(c.vertices.size(), 0xFFFFFFFFu);
	std::vector<WorldVertex> vertices;
	vertices.reserve(c.vertices.size());
	for (u16& i : indices) {
		if (remap[i] == 0xFFFFFFFFu) remap[i] = 0; // (marked; numbered below in vertex order)
	}
	for (size_t v = 0; v < c.vertices.size(); ++v)
		if (remap[v] != 0xFFFFFFFFu) {
			remap[v] = u32(vertices.size());
			vertices.push_back(c.vertices[v]);
		}
	for (u16& i : indices) i = u16(remap[i]);
	before += initial;
	after += simp.alive();
	c.vertices.swap(vertices);
	c.indices.swap(indices);
	c.batches.swap(batches);
	c.header.vertexCount = u16(c.vertices.size());
	c.header.batchCount = u16(c.batches.size());
	c.header.indexCount = u32(c.indices.size());
	return InPlace::Done;
}

} // namespace

void buildLods(std::vector<BakedChunk>& chunks, const std::vector<WorldMaterial>& materials, const LodOptions& opt, LodStats& stats) {
	stats = LodStats {};
	if (!opt.enabled) return;
	const auto start = std::chrono::steady_clock::now();
	std::atomic<size_t> next {0};
	struct Part {
		u32 chunks = 0, candidates = 0, notTier = 0, few = 0, little = 0;
		u64 before = 0, after = 0, bytes = 0, simplifiable = 0;
	};
	const unsigned threads = std::max(1u, opt.threads);
	std::vector<Part> parts(threads);
	auto worker = [&](unsigned id) {
		for (;;) {
			const size_t i = next.fetch_add(1);
			if (i >= chunks.size()) return;
			BakedChunk& c = chunks[i];
			const size_t indices0 = c.indices.size(), batches0 = c.batches.size(), vertices0 = c.vertices.size();
			++parts[id].candidates;
			switch (simplifyChunk(c, materials, opt, parts[id].before, parts[id].after, parts[id].simplifiable)) {
			case Result::Done:
				++parts[id].chunks;
				parts[id].bytes += (c.indices.size() - indices0) * sizeof(u16) + (c.batches.size() - batches0) * sizeof(ChunkBatch) +
				                          (c.vertices.size() - vertices0) * sizeof(WorldVertex);
				break;
			case Result::NotTierZero: ++parts[id].notTier; break;
			case Result::TooFew: ++parts[id].few; break;
			case Result::TooLittle: ++parts[id].little; break;
			default: break;
			}
		}
	};
	std::vector<std::thread> pool;
	for (unsigned t = 1; t < threads; ++t) pool.emplace_back(worker, t);
	worker(0);
	for (std::thread& t : pool) t.join();
	for (const Part& p : parts) {
		stats.chunks += p.chunks;
		stats.candidates += p.candidates;
		stats.notTierZero += p.notTier;
		stats.tooFew += p.few;
		stats.tooLittle += p.little;
		stats.simplifiable += p.simplifiable;
		stats.trianglesBefore += p.before;
		stats.trianglesAfter += p.after;
		stats.extraBytes += p.bytes;
	}
	stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

void buildSimplified(std::vector<BakedChunk>& chunks, const std::vector<WorldMaterial>& materials, const SimplifyOptions& opt,
                     SimplifyStats& stats) {
	stats = SimplifyStats {};
	if (!opt.enabled) return;
	const auto start = std::chrono::steady_clock::now();
	std::atomic<size_t> next {0};
	struct Part {
		u32 chunks = 0, candidates = 0, few = 0, little = 0;
		u64 before = 0, after = 0, verticesBefore = 0, verticesAfter = 0;
	};
	const unsigned threads = std::max(1u, opt.threads);
	std::vector<Part> parts(threads);
	auto worker = [&](unsigned id) {
		for (;;) {
			const size_t i = next.fetch_add(1);
			if (i >= chunks.size()) return;
			BakedChunk& c = chunks[i];
			++parts[id].candidates;
			parts[id].verticesBefore += c.vertices.size();
			switch (simplifyInPlace(c, materials, opt, parts[id].before, parts[id].after)) {
			case InPlace::Done: ++parts[id].chunks; break;
			case InPlace::TooFew: ++parts[id].few; break;
			case InPlace::TooLittle: ++parts[id].little; break;
			default: break;
			}
			parts[id].verticesAfter += c.vertices.size();
		}
	};
	std::vector<std::thread> pool;
	for (unsigned t = 1; t < threads; ++t) pool.emplace_back(worker, t);
	worker(0);
	for (std::thread& t : pool) t.join();
	for (const Part& p : parts) {
		stats.chunks += p.chunks;
		stats.candidates += p.candidates;
		stats.tooFew += p.few;
		stats.tooLittle += p.little;
		stats.trianglesBefore += p.before;
		stats.trianglesAfter += p.after;
		stats.verticesBefore += p.verticesBefore;
		stats.verticesAfter += p.verticesAfter;
	}
	stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

} // namespace g3
