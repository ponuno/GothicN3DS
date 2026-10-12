// A bounding volume hierarchy over triangles, for the baker's rays (ambient
// occlusion, visibility).
#pragma once
#include "g3ds/base.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace g3 {
namespace geo {

struct V3 {
	float x, y, z;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(V3 a) { return std::sqrt(dot(a, a)); }

// ---- a bounding volume hierarchy over the triangles that block ----

struct Tri {
	V3 a, e1, e2;
	float blocks;
};

struct Node {
	float mn[3], mx[3];
	u32 first; // inner: the left child (the right one follows it); leaf: the first triangle
	u32 count; // 0 for an inner node
};

class Bvh {
public:
	void build(std::vector<Tri> tris) {
		_tris = std::move(tris);
		_nodes.clear();
		if (_tris.empty()) return;
		std::vector<V3> centre(_tris.size());
		for (size_t i = 0; i < _tris.size(); ++i) {
			const Tri& t = _tris[i];
			centre[i] = t.a + (t.e1 + t.e2) * (1.0f / 3.0f);
		}
		std::vector<u32> order(_tris.size());
		for (u32 i = 0; i < u32(order.size()); ++i) order[i] = i;
		_nodes.reserve(_tris.size() / 2 + 1);
		_nodes.push_back(Node {});
		struct Job {
			u32 node, begin, end;
		};
		std::vector<Job> stack = {{0, 0, u32(order.size())}};
		while (!stack.empty()) {
			const Job j = stack.back();
			stack.pop_back();
			Node& n = _nodes[j.node];
			float cmn[3] = {1e30f, 1e30f, 1e30f}, cmx[3] = {-1e30f, -1e30f, -1e30f};
			for (int a = 0; a < 3; ++a) {
				n.mn[a] = 1e30f;
				n.mx[a] = -1e30f;
			}
			for (u32 i = j.begin; i < j.end; ++i) {
				const Tri& t = _tris[order[i]];
				const V3 p[3] = {t.a, t.a + t.e1, t.a + t.e2};
				for (const V3& q : p) {
					const float c[3] = {q.x, q.y, q.z};
					for (int a = 0; a < 3; ++a) {
						n.mn[a] = std::min(n.mn[a], c[a]);
						n.mx[a] = std::max(n.mx[a], c[a]);
					}
				}
				const float c[3] = {centre[order[i]].x, centre[order[i]].y, centre[order[i]].z};
				for (int a = 0; a < 3; ++a) {
					cmn[a] = std::min(cmn[a], c[a]);
					cmx[a] = std::max(cmx[a], c[a]);
				}
			}
			const u32 count = j.end - j.begin;
			int axis = 0;
			for (int a = 1; a < 3; ++a)
				if (cmx[a] - cmn[a] > cmx[axis] - cmn[axis]) axis = a;
			if (count <= 4 || cmx[axis] - cmn[axis] < 1e-3f) {
				n.first = j.begin;
				n.count = count;
				continue;
			}
			const u32 mid = j.begin + count / 2;
			auto key = [&](u32 i) { return axis == 0 ? centre[i].x : axis == 1 ? centre[i].y : centre[i].z; };
			std::nth_element(order.begin() + j.begin, order.begin() + mid, order.begin() + j.end,
			                 [&](u32 l, u32 r) { return key(l) < key(r); });
			const u32 left = u32(_nodes.size());
			n.first = left;
			n.count = 0;
			_nodes.push_back(Node {});
			_nodes.push_back(Node {});
			stack.push_back({left, j.begin, mid});
			stack.push_back({left + 1, mid, j.end});
		}
		std::vector<Tri> sorted(_tris.size());
		for (size_t i = 0; i < order.size(); ++i) sorted[i] = _tris[order[i]];
		_tris.swap(sorted);
	}

	// The nearest triangle along the ray within (tMin, tMax): how much it
	// blocks (0 if none) and how far it is.
	// `hitTri`, if given, receives the triangle hit; `any`: stop at the first one found (for yes/no questions).
	float nearest(V3 o, V3 d, float tMin, float tMax, float& t, const Tri** hitTri = nullptr, bool any = false) const {
		if (_nodes.empty()) return 0.0f;
		const float inv[3] = {1.0f / (std::fabs(d.x) > 1e-12f ? d.x : 1e-12f), 1.0f / (std::fabs(d.y) > 1e-12f ? d.y : 1e-12f),
		                      1.0f / (std::fabs(d.z) > 1e-12f ? d.z : 1e-12f)};
		const float org[3] = {o.x, o.y, o.z};
		float best = tMax, blocks = 0.0f;
		if (hitTri) *hitTri = nullptr;
		u32 stack[64];
		int sp = 0;
		stack[sp++] = 0;
		while (sp > 0) {
			const Node& n = _nodes[stack[--sp]];
			float t0 = tMin, t1 = best;
			bool hit = true;
			for (int a = 0; a < 3 && hit; ++a) {
				float n0 = (n.mn[a] - org[a]) * inv[a], n1 = (n.mx[a] - org[a]) * inv[a];
				if (n0 > n1) std::swap(n0, n1);
				t0 = std::max(t0, n0);
				t1 = std::min(t1, n1);
				hit = t0 <= t1;
			}
			if (!hit) continue;
			if (n.count) {
				for (u32 i = n.first; i < n.first + n.count; ++i) {
					const Tri& tri = _tris[i];
					// Möller-Trumbore, both sides.
					const V3 p = cross(d, tri.e2);
					const float det = dot(tri.e1, p);
					if (std::fabs(det) < 1e-9f) continue;
					const float id = 1.0f / det;
					const V3 s = o - tri.a;
					const float u = dot(s, p) * id;
					if (u < 0.0f || u > 1.0f) continue;
					const V3 q = cross(s, tri.e1);
					const float v = dot(d, q) * id;
					if (v < 0.0f || u + v > 1.0f) continue;
					const float h = dot(tri.e2, q) * id;
					if (h > tMin && h < best) {
						best = h;
						blocks = tri.blocks;
						if (hitTri) *hitTri = &tri;
						if (any) {
							t = best;
							return blocks;
						}
					}
				}
			} else if (sp + 2 <= 64) {
				stack[sp++] = n.first;
				stack[sp++] = n.first + 1;
			}
		}
		t = best;
		return blocks;
	}

	size_t size() const { return _tris.size(); }

private:
	std::vector<Tri> _tris;
	std::vector<Node> _nodes;
};

} // namespace geo
} // namespace g3
