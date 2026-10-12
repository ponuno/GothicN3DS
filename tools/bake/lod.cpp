#include "lod.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <queue>
#include <thread>
#include <unordered_map>

namespace g3 {

namespace {

struct Tri {
	u16 v[3];
	u16 material;
	u8 cluster;
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
	u32 verFrom, verTo;
	bool operator<(const Candidate& o) const { return cost > o.cost; } // (a min-heap)
};

class Simplifier {
public:
	Simplifier(const BakedChunk& chunk, std::vector<Tri> tris, const std::vector<bool>& usedOutside, float maxError)
	    : _c(chunk), _tris(std::move(tris)), _maxErr2(double(maxError) * double(maxError)) {
		const size_t n = chunk.vertices.size();
		_pos.resize(n);
		for (size_t i = 0; i < n; ++i) {
			const WorldVertex& v = chunk.vertices[i];
			const double s = chunk.header.posScale;
			_pos[i] = {double(v.x) * s, double(v.y) * s, double(v.z) * s};
		}
		_inc.resize(n);
		_locked.assign(n, false);
		_quadric.resize(n);
		_ver.assign(n, 0);
		for (u32 t = 0; t < _tris.size(); ++t)
			for (int k = 0; k < 3; ++k) _inc[_tris[t].v[k]].push_back(t);
		for (size_t i = 0; i < n; ++i)
			if (usedOutside[i]) _locked[i] = true;
		lockBorders();
		for (u32 t = 0; t < _tris.size(); ++t) {
			double nx, ny, nz, d;
			if (!plane(_tris[t], nx, ny, nz, d)) continue;
			for (int k = 0; k < 3; ++k) _quadric[_tris[t].v[k]].add(nx, ny, nz, d);
		}
		_alive = u32(_tris.size());
	}

	// Collapses until `target` triangles are left or nothing may go.
	void run(u32 target) {
		std::priority_queue<Candidate> heap;
		for (u32 v = 0; v < _inc.size(); ++v)
			if (!_inc[v].empty()) push(heap, v);
		while (_alive > target && !heap.empty()) {
			const Candidate c = heap.top();
			heap.pop();
			if (_inc[c.from].empty() || _inc[c.to].empty()) continue;
			// What it costs may have grown since it was queued (its end took in another vertex's planes).
			const double now = cost(c.from, c.to);
			if (now > c.cost * 1.000001 + 1e-9) {
				if (now <= _maxErr2) heap.push({now, c.from, c.to, 0, 0});
				continue;
			}
			if (!legal(c.from, c.to)) continue;
			collapse(c.from, c.to, heap);
		}
	}

	const std::vector<Tri>& tris() const { return _tris; }
	u32 alive() const { return _alive; }

private:
	const BakedChunk& _c;
	std::vector<Tri> _tris;
	double _maxErr2;
	std::vector<P3> _pos;
	std::vector<std::vector<u32>> _inc;
	std::vector<bool> _locked;
	std::vector<Quadric> _quadric;
	std::vector<u32> _ver;
	u32 _alive = 0;

	bool plane(const Tri& t, double& nx, double& ny, double& nz, double& d) const {
		const P3 a = _pos[t.v[0]], b = _pos[t.v[1]], c = _pos[t.v[2]];
		const P3 n = cross(sub(b, a), sub(c, a));
		const double len = std::sqrt(dot(n, n));
		if (len < 1e-9) return false;
		nx = n.x / len;
		ny = n.y / len;
		nz = n.z / len;
		d = -(nx * a.x + ny * a.y + nz * a.z);
		return true;
	}

	// Vertices that stay: the border of the simplified mesh, a change of material, a seam (two vertices at one place).
	void lockBorders() {
		struct Edge {
			u64 key;
			u32 tri;
		};
		std::vector<Edge> edges;
		edges.reserve(_tris.size() * 3);
		for (u32 t = 0; t < _tris.size(); ++t)
			for (int k = 0; k < 3; ++k) {
				const u32 a = _tris[t].v[k], b = _tris[t].v[(k + 1) % 3];
				edges.push_back({(u64(std::min(a, b)) << 32) | std::max(a, b), t});
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
		// Seams: two vertices at the same place.
		std::unordered_map<u64, u32> at;
		for (u32 v = 0; v < _inc.size(); ++v) {
			if (_inc[v].empty()) continue;
			const WorldVertex& w = _c.vertices[v];
			const u64 key = (u64(u16(w.x)) << 32) | (u64(u16(w.y)) << 16) | u64(u16(w.z));
			auto it = at.find(key);
			if (it == at.end())
				at.emplace(key, v);
			else
				_locked[v] = _locked[it->second] = true;
		}
	}

	void neighbours(u32 v, std::vector<u32>& out) const {
		out.clear();
		for (u32 t : _inc[v])
			for (int k = 0; k < 3; ++k) {
				const u32 o = _tris[t].v[k];
				if (o != v) out.push_back(o);
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
			if (c <= _maxErr2) heap.push({c, from, to, _ver[from], _ver[to]});
		}
	}

	bool legal(u32 from, u32 to) const {
		if (_locked[from]) return false;
		std::vector<u32> na, nb;
		neighbours(from, na);
		neighbours(to, nb);
		// The two ends may share only the vertices opposite the edge (the link condition): more would fold the mesh.
		u32 common = 0, shared = 0;
		for (u32 x : na)
			if (std::binary_search(nb.begin(), nb.end(), x)) ++common;
		for (u32 t : _inc[from])
			for (int k = 0; k < 3; ++k)
				if (_tris[t].v[k] == to) {
					++shared;
					break;
				}
		if (shared != 2 || common != 2) return false;
		// No triangle may turn over or collapse to a sliver.
		for (u32 t : _inc[from]) {
			const Tri& tri = _tris[t];
			bool hasTo = false;
			for (int k = 0; k < 3; ++k) hasTo |= tri.v[k] == to;
			if (hasTo) continue;
			P3 before[3], after[3];
			for (int k = 0; k < 3; ++k) {
				before[k] = _pos[tri.v[k]];
				after[k] = tri.v[k] == from ? _pos[to] : before[k];
			}
			const P3 n0 = cross(sub(before[1], before[0]), sub(before[2], before[0]));
			const P3 n1 = cross(sub(after[1], after[0]), sub(after[2], after[0]));
			const double l0 = std::sqrt(dot(n0, n0)), l1 = std::sqrt(dot(n1, n1));
			if (l1 < 1e-6 || l0 < 1e-9) return false;
			if (dot(n0, n1) < 0.3 * l0 * l1) return false;
		}
		return true;
	}

	void collapse(u32 from, u32 to, std::priority_queue<Candidate>& heap) {
		std::vector<u32> around;
		neighbours(from, around);
		const std::vector<u32> incFrom = _inc[from];
		for (u32 t : incFrom) {
			Tri& tri = _tris[t];
			bool hasTo = false;
			for (int k = 0; k < 3; ++k) hasTo |= tri.v[k] == to;
			if (hasTo) {
				tri.alive = false;
				--_alive;
				for (int k = 0; k < 3; ++k) {
					const u32 v = tri.v[k];
					if (v == from) continue;
					auto& list = _inc[v];
					list.erase(std::remove(list.begin(), list.end(), t), list.end());
				}
			} else {
				for (int k = 0; k < 3; ++k)
					if (tri.v[k] == from) tri.v[k] = u16(to);
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
bool simplifyChunk(BakedChunk& c, const std::vector<WorldMaterial>& materials, const LodOptions& opt, u64& before, u64& after) {
	if (c.tier != 0 || c.vertices.empty() || c.vertices.size() > 65535) return false;
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
	if (tris.size() < opt.minTriangles) return false;
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
	if (simp.alive() > u32(double(initial) * 0.85)) return false; // not worth a second picture

	// The stand-ins: by material, then cluster, as the batches themselves are.
	std::vector<const Tri*> kept;
	for (const Tri& t : simp.tris())
		if (t.alive) kept.push_back(&t);
	std::stable_sort(kept.begin(), kept.end(), [](const Tri* l, const Tri* r) {
		if (l->material != r->material) return l->material < r->material;
		return l->cluster < r->cluster;
	});
	if (c.batches.size() + kept.size() > 65000) return false; // (never so many batches)
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
	for (size_t b = 0; b < c.batches.size(); ++b)
		if (take[b]) c.batches[b].detail = withBatchLod(c.batches[b].detail, BATCH_LOD_FULL_ONLY);
	c.batches.insert(c.batches.end(), added.begin(), added.end());
	c.indices.swap(indices);
	c.header.batchCount = u16(c.batches.size());
	c.header.indexCount = u32(c.indices.size());
	before += initial;
	after += kept.size();
	return true;
}

} // namespace

void buildLods(std::vector<BakedChunk>& chunks, const std::vector<WorldMaterial>& materials, const LodOptions& opt, LodStats& stats) {
	stats = LodStats {};
	if (!opt.enabled) return;
	const auto start = std::chrono::steady_clock::now();
	std::atomic<size_t> next {0};
	struct Part {
		u32 chunks = 0, candidates = 0;
		u64 before = 0, after = 0, bytes = 0;
	};
	const unsigned threads = std::max(1u, opt.threads);
	std::vector<Part> parts(threads);
	auto worker = [&](unsigned id) {
		for (;;) {
			const size_t i = next.fetch_add(1);
			if (i >= chunks.size()) return;
			BakedChunk& c = chunks[i];
			const size_t indices0 = c.indices.size(), batches0 = c.batches.size();
			++parts[id].candidates;
			if (simplifyChunk(c, materials, opt, parts[id].before, parts[id].after)) {
				++parts[id].chunks;
				parts[id].bytes += (c.indices.size() - indices0) * sizeof(u16) + (c.batches.size() - batches0) * sizeof(ChunkBatch);
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
		stats.trianglesBefore += p.before;
		stats.trianglesAfter += p.after;
		stats.extraBytes += p.bytes;
	}
	stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

} // namespace g3
