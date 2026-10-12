#include "model_simplify.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <queue>

namespace g3 {

namespace {

struct P3 {
	double x, y, z;
};
P3 sub(P3 a, P3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
P3 cross(P3 a, P3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double dot(P3 a, P3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

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
	double eval(double x, double y, double z) const {
		return a[0] * x * x + 2 * a[1] * x * y + 2 * a[2] * x * z + 2 * a[3] * x + a[4] * y * y + 2 * a[5] * y * z + 2 * a[6] * y +
		       a[7] * z * z + 2 * a[8] * z + a[9];
	}
};

struct Candidate {
	double cost;
	u32 from, to;
	bool operator<(const Candidate& o) const { return cost > o.cost; } // (a min-heap)
};

// How far a vertex's texture coordinates, normal and skinning may be off what the triangle has at that place.
const double kUvTolerance = 0.01;
const double kNormalDot = 0.9;
const double kWeightTolerance = 0.2;

// The vertices are welded (every attribute equal); a "place" is a position, and a collapse moves every corner at
// a place to a vertex at another one that fits.
class Mesh {
public:
	Mesh(const SrcModelPart& part, float maxError) : _maxErr2(double(maxError) * double(maxError)) {
		std::map<std::vector<float>, u32> weld;
		std::map<std::vector<float>, u32> places;
		for (const SrcSkinVertex& v : part.verts) {
			std::vector<float> key = {v.pos[0], v.pos[1], v.pos[2], v.normal[0], v.normal[1], v.normal[2], v.uv[0], v.uv[1],
			                          float(v.bone[0]), float(v.bone[1]), v.weight1};
			auto it = weld.find(key);
			if (it == weld.end()) {
				it = weld.emplace(key, u32(_verts.size())).first;
				_verts.push_back(v);
				const std::vector<float> pk = {v.pos[0], v.pos[1], v.pos[2]};
				auto pit = places.find(pk);
				if (pit == places.end()) {
					pit = places.emplace(pk, u32(_pos.size())).first;
					_pos.push_back({double(v.pos[0]), double(v.pos[1]), double(v.pos[2])});
					_vertsAt.emplace_back();
				}
				_pid.push_back(pit->second);
				_vertsAt[pit->second].push_back(it->second);
			}
			_corner.push_back(it->second);
		}
		const size_t places_n = _pos.size(), triCount = _corner.size() / 3;
		_alive.assign(triCount, true);
		_inc.resize(places_n);
		_locked.assign(places_n, false);
		_quadric.resize(places_n);
		for (u32 t = 0; t < triCount; ++t) {
			const u32 p0 = _pid[_corner[t * 3]], p1 = _pid[_corner[t * 3 + 1]], p2 = _pid[_corner[t * 3 + 2]];
			if (p0 == p1 || p1 == p2 || p0 == p2) {
				_alive[t] = false; // a sliver: not part of the result
				continue;
			}
			for (int k = 0; k < 3; ++k) _inc[_pid[_corner[t * 3 + k]]].push_back(t);
		}
		lockBorders();
		for (u32 t = 0; t < triCount; ++t) {
			double nx, ny, nz, d;
			if (!_alive[t] || !plane(t, nx, ny, nz, d)) continue;
			for (int k = 0; k < 3; ++k) _quadric[_pid[_corner[t * 3 + k]]].add(nx, ny, nz, d);
		}
		_count = 0;
		for (bool a : _alive) _count += a;
	}

	u32 count() const { return _count; }

	void run(u32 target) {
		std::priority_queue<Candidate> heap;
		for (u32 p = 0; p < _inc.size(); ++p)
			if (!_inc[p].empty()) push(heap, p);
		while (_count > target && !heap.empty()) {
			const Candidate c = heap.top();
			heap.pop();
			if (_inc[c.from].empty() || _inc[c.to].empty()) continue;
			const double now = cost(c.from, c.to);
			if (now > c.cost * 1.000001 + 1e-9) {
				if (now <= _maxErr2) heap.push({now, c.from, c.to});
				continue;
			}
			if (!legal(c.from, c.to)) continue;
			collapse(c.from, c.to, heap);
		}
	}

	void write(SrcModelPart& part) const {
		std::vector<SrcSkinVertex> out;
		for (u32 t = 0; t < _alive.size(); ++t)
			if (_alive[t])
				for (int k = 0; k < 3; ++k) out.push_back(_verts[_corner[t * 3 + k]]);
		part.verts.swap(out);
	}

private:
	double _maxErr2;
	std::vector<SrcSkinVertex> _verts;
	std::vector<u32> _corner; // three per triangle
	std::vector<bool> _alive;
	std::vector<u32> _pid;
	std::vector<P3> _pos;
	std::vector<std::vector<u32>> _vertsAt;
	std::vector<std::vector<u32>> _inc;
	std::vector<bool> _locked;
	std::vector<Quadric> _quadric;
	u32 _count = 0;

	bool plane(u32 t, double& nx, double& ny, double& nz, double& d) const {
		const P3 a = _pos[_pid[_corner[t * 3]]], b = _pos[_pid[_corner[t * 3 + 1]]], c = _pos[_pid[_corner[t * 3 + 2]]];
		const P3 n = cross(sub(b, a), sub(c, a));
		const double len = std::sqrt(dot(n, n));
		if (len < 1e-12) return false;
		nx = n.x / len;
		ny = n.y / len;
		nz = n.z / len;
		d = -(nx * a.x + ny * a.y + nz * a.z);
		return true;
	}

	void lockBorders() {
		struct Edge {
			u64 key;
			u32 tri;
		};
		std::vector<Edge> edges;
		for (u32 t = 0; t < _alive.size(); ++t) {
			if (!_alive[t]) continue;
			for (int k = 0; k < 3; ++k) {
				const u32 a = _pid[_corner[t * 3 + k]], b = _pid[_corner[t * 3 + (k + 1) % 3]];
				edges.push_back({(u64(std::min(a, b)) << 32) | std::max(a, b), t});
			}
		}
		std::sort(edges.begin(), edges.end(), [](const Edge& l, const Edge& r) { return l.key < r.key; });
		for (size_t i = 0; i < edges.size();) {
			size_t e = i;
			while (e < edges.size() && edges[e].key == edges[i].key) ++e;
			if ((e - i) != 2) {
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
				const u32 o = _pid[_corner[t * 3 + k]];
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

	// The vertex at place `to` that triangle `t` (with a corner at `from`) could use instead of its own corner, or -1.
	// The texture coordinates and the normal are those the triangle's own mapping carries to `to`; the bones and
	// their weights are the corner's own, so the skinning stays what it was.
	int chooseVertex(u32 t, u32 from, u32 to) const {
		int fk = -1;
		for (int k = 0; k < 3; ++k)
			if (_pid[_corner[t * 3 + k]] == from) fk = k;
		if (fk < 0) return -1;
		const SrcSkinVertex& A = _verts[_corner[t * 3 + fk]];
		const SrcSkinVertex& X = _verts[_corner[t * 3 + (fk + 1) % 3]];
		const SrcSkinVertex& Y = _verts[_corner[t * 3 + (fk + 2) % 3]];
		const P3 pa = _pos[from];
		const P3 e1 = sub(_pos[_pid[_corner[t * 3 + (fk + 1) % 3]]], pa), e2 = sub(_pos[_pid[_corner[t * 3 + (fk + 2) % 3]]], pa),
		         d = sub(_pos[to], pa);
		const double a11 = dot(e1, e1), a12 = dot(e1, e2), a22 = dot(e2, e2);
		const double det = a11 * a22 - a12 * a12;
		if (det < 1e-12) return -1;
		const double r1 = dot(e1, d), r2 = dot(e2, d);
		const double s = (r1 * a22 - r2 * a12) / det, u = (a11 * r2 - a12 * r1) / det;
		if (std::fabs(s) > 8.0 || std::fabs(u) > 8.0) return -1;
		double wantUv[2], wantN[3];
		for (int i = 0; i < 2; ++i) wantUv[i] = double(A.uv[i]) + s * double(X.uv[i] - A.uv[i]) + u * double(Y.uv[i] - A.uv[i]);
		for (int i = 0; i < 3; ++i) wantN[i] = double(A.normal[i]) + s * double(X.normal[i] - A.normal[i]) + u * double(Y.normal[i] - A.normal[i]);
		const double nl = std::sqrt(wantN[0] * wantN[0] + wantN[1] * wantN[1] + wantN[2] * wantN[2]);
		if (nl < 1e-9) return -1;
		int best = -1;
		double bestScore = 1e30;
		for (u32 cand : _vertsAt[to]) {
			const SrcSkinVertex& C = _verts[cand];
			const double eu = std::max(std::fabs(double(C.uv[0]) - wantUv[0]), std::fabs(double(C.uv[1]) - wantUv[1]));
			if (eu > kUvTolerance) continue;
			const double cl = std::sqrt(double(C.normal[0]) * C.normal[0] + double(C.normal[1]) * C.normal[1] + double(C.normal[2]) * C.normal[2]);
			if (cl < 1e-9) continue;
			const double nd = (C.normal[0] * wantN[0] + C.normal[1] * wantN[1] + C.normal[2] * wantN[2]) / (cl * nl);
			if (nd < kNormalDot) continue;
			// The same bones (in either order) with nearly the same weights.
			double dw;
			if (C.bone[0] == A.bone[0] && C.bone[1] == A.bone[1]) dw = std::fabs(double(C.weight1) - double(A.weight1));
			else if (C.bone[0] == A.bone[1] && C.bone[1] == A.bone[0]) dw = std::fabs(double(C.weight1) - (1.0 - double(A.weight1)));
			else continue;
			if (dw > kWeightTolerance) continue;
			const double score = eu / kUvTolerance + (1.0 - nd) / (1.0 - kNormalDot) + dw / kWeightTolerance;
			if (score < bestScore) {
				bestScore = score;
				best = int(cand);
			}
		}
		return best;
	}

	bool legal(u32 from, u32 to) const {
		if (_locked[from]) return false;
		std::vector<u32> na, nb;
		neighbours(from, na);
		neighbours(to, nb);
		u32 common = 0, shared = 0;
		for (u32 x : na)
			if (std::binary_search(nb.begin(), nb.end(), x)) ++common;
		for (u32 t : _inc[from])
			for (int k = 0; k < 3; ++k)
				if (_pid[_corner[t * 3 + k]] == to) {
					++shared;
					break;
				}
		if (shared != 2 || common != 2) return false;
		for (u32 t : _inc[from]) {
			bool hasTo = false;
			for (int k = 0; k < 3; ++k) hasTo |= _pid[_corner[t * 3 + k]] == to;
			if (hasTo) continue;
			P3 before[3], after[3];
			for (int k = 0; k < 3; ++k) {
				const u32 p = _pid[_corner[t * 3 + k]];
				before[k] = _pos[p];
				after[k] = p == from ? _pos[to] : before[k];
			}
			const P3 n0 = cross(sub(before[1], before[0]), sub(before[2], before[0]));
			const P3 n1 = cross(sub(after[1], after[0]), sub(after[2], after[0]));
			const double l0 = std::sqrt(dot(n0, n0)), l1 = std::sqrt(dot(n1, n1));
			if (l1 < 1e-9 || l0 < 1e-12) return false;
			if (dot(n0, n1) < 0.3 * l0 * l1) return false;
			if (chooseVertex(t, from, to) < 0) return false;
		}
		return true;
	}

	void collapse(u32 from, u32 to, std::priority_queue<Candidate>& heap) {
		std::vector<u32> around;
		neighbours(from, around);
		const std::vector<u32> incFrom = _inc[from];
		for (u32 t : incFrom) {
			bool hasTo = false;
			for (int k = 0; k < 3; ++k) hasTo |= _pid[_corner[t * 3 + k]] == to;
			if (hasTo) {
				_alive[t] = false;
				--_count;
				for (int k = 0; k < 3; ++k) {
					const u32 p = _pid[_corner[t * 3 + k]];
					if (p == from) continue;
					auto& list = _inc[p];
					list.erase(std::remove(list.begin(), list.end(), t), list.end());
				}
			} else {
				const int pick = chooseVertex(t, from, to);
				for (int k = 0; k < 3; ++k)
					if (_pid[_corner[t * 3 + k]] == from) _corner[t * 3 + k] = u32(pick);
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

} // namespace

void simplifyModelParts(std::vector<SrcModelPart>& parts, const std::vector<bool>& skip, const ModelSimplifyOptions& opt,
                        ModelSimplifyStats& stats) {
	if (!opt.enabled) return;
	for (size_t i = 0; i < parts.size(); ++i) {
		SrcModelPart& part = parts[i];
		++stats.parts;
		if ((i < skip.size() && skip[i]) || part.material.blend != SrcBlend::Opaque || part.verts.size() % 3) continue;
		const u32 triangles = u32(part.verts.size() / 3);
		if (triangles < opt.minTriangles) continue;
		Mesh mesh(part, opt.maxError);
		const u32 initial = mesh.count();
		mesh.run(u32(double(initial) * double(opt.ratio)));
		if (mesh.count() > u32(double(initial) * 0.97)) continue;
		mesh.write(part);
		++stats.simplified;
		stats.before += initial;
		stats.after += mesh.count();
	}
}

} // namespace g3
