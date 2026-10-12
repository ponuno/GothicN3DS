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
	u32 first;  // inner: the left child (the right one follows it); leaf: the first triangle
	u16 count;  // 0 for an inner node
	u8 axis;    // inner: the axis the children were split on (the left one has the lower middles)
	u8 pad;
};

class Bvh {
public:
	// Built by the surface area heuristic over a few bins: rays meet fewer boxes than with halves of equal count.
	void build(std::vector<Tri> tris) {
		_tris = std::move(tris);
		_nodes.clear();
		if (_tris.empty()) return;
		const size_t n = _tris.size();
		std::vector<V3> centre(n);
		struct Box3 {
			float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
			void add(const float* lo, const float* hi) {
				for (int a = 0; a < 3; ++a) {
					mn[a] = std::min(mn[a], lo[a]);
					mx[a] = std::max(mx[a], hi[a]);
				}
			}
			float area() const {
				const float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
				return dx < 0 ? 0.0f : 2.0f * (dx * dy + dy * dz + dz * dx);
			}
		};
		std::vector<Box3> tbox(n);
		for (size_t i = 0; i < n; ++i) {
			const Tri& t = _tris[i];
			centre[i] = t.a + (t.e1 + t.e2) * (1.0f / 3.0f);
			const V3 p[3] = {t.a, t.a + t.e1, t.a + t.e2};
			for (const V3& q : p) {
				const float c[3] = {q.x, q.y, q.z};
				tbox[i].add(c, c);
			}
		}
		std::vector<u32> order(n);
		for (u32 i = 0; i < u32(n); ++i) order[i] = i;
		_nodes.reserve(n / 2 + 1);
		_nodes.push_back(Node {});
		struct Job {
			u32 node, begin, end;
		};
		std::vector<Job> stack = {{0, 0, u32(n)}};
		constexpr int BINS = 12;
		while (!stack.empty()) {
			const Job j = stack.back();
			stack.pop_back();
			Box3 all;
			float cmn[3] = {1e30f, 1e30f, 1e30f}, cmx[3] = {-1e30f, -1e30f, -1e30f};
			for (u32 i = j.begin; i < j.end; ++i) {
				all.add(tbox[order[i]].mn, tbox[order[i]].mx);
				const float c[3] = {centre[order[i]].x, centre[order[i]].y, centre[order[i]].z};
				for (int a = 0; a < 3; ++a) {
					cmn[a] = std::min(cmn[a], c[a]);
					cmx[a] = std::max(cmx[a], c[a]);
				}
			}
			Node node {};
			for (int a = 0; a < 3; ++a) {
				node.mn[a] = all.mn[a];
				node.mx[a] = all.mx[a];
			}
			const u32 count = j.end - j.begin;
			int axis = 0;
			for (int a = 1; a < 3; ++a)
				if (cmx[a] - cmn[a] > cmx[axis] - cmn[axis]) axis = a;
			auto leaf = [&]() {
				node.first = j.begin;
				node.count = u16(count);
				_nodes[j.node] = node;
			};
			if (count <= 2 || cmx[axis] - cmn[axis] < 1e-3f) {
				// (A leaf never holds more than 65535: only a pile on one spot gets here with more than a few.)
				if (count <= 65535u) {
					leaf();
					continue;
				}
			}
			auto key = [&](u32 i) { return axis == 0 ? centre[i].x : axis == 1 ? centre[i].y : centre[i].z; };
			u32 mid = j.begin + count / 2;
			bool split = false;
			if (cmx[axis] - cmn[axis] >= 1e-3f) {
				Box3 bin[BINS];
				u32 cnt[BINS] = {};
				const float scale = float(BINS) / (cmx[axis] - cmn[axis]);
				auto binOf = [&](u32 i) { return std::min(BINS - 1, int((key(i) - cmn[axis]) * scale)); };
				for (u32 i = j.begin; i < j.end; ++i) {
					const int b = binOf(order[i]);
					++cnt[b];
					bin[b].add(tbox[order[i]].mn, tbox[order[i]].mx);
				}
				float leftArea[BINS], rightArea[BINS];
				u32 leftCount[BINS], rightCount[BINS];
				Box3 acc;
				u32 c = 0;
				for (int b = 0; b < BINS; ++b) {
					if (cnt[b]) acc.add(bin[b].mn, bin[b].mx);
					c += cnt[b];
					leftArea[b] = acc.area();
					leftCount[b] = c;
				}
				acc = Box3 {};
				c = 0;
				for (int b = BINS - 1; b >= 0; --b) {
					if (cnt[b]) acc.add(bin[b].mn, bin[b].mx);
					c += cnt[b];
					rightArea[b] = acc.area();
					rightCount[b] = c;
				}
				float bestCost = 1e30f;
				int bestBin = -1;
				for (int b = 0; b + 1 < BINS; ++b) {
					if (!leftCount[b] || !rightCount[b + 1]) continue;
					const float cost = leftArea[b] * float(leftCount[b]) + rightArea[b + 1] * float(rightCount[b + 1]);
					if (cost < bestCost) {
						bestCost = cost;
						bestBin = b;
					}
				}
				if (bestBin >= 0) {
					// Keep a small group whole when splitting does not pay for the extra box.
					const float leafCost = all.area() * float(count);
					if (count <= 4 && bestCost + all.area() * 1.5f >= leafCost) {
						leaf();
						continue;
					}
					const u32* o = order.data();
					u32* part = std::partition(order.data() + j.begin, order.data() + j.end, [&](u32 i) { return binOf(i) <= bestBin; });
					(void)o;
					mid = u32(part - order.data());
					split = mid > j.begin && mid < j.end;
				}
			}
			if (!split) {
				if (count <= 4) {
					leaf();
					continue;
				}
				mid = j.begin + count / 2;
				std::nth_element(order.begin() + j.begin, order.begin() + mid, order.begin() + j.end,
				                 [&](u32 l, u32 r) { return key(l) < key(r); });
			}
			const u32 left = u32(_nodes.size());
			node.first = left;
			node.count = 0;
			node.axis = u8(axis);
			_nodes[j.node] = node;
			_nodes.push_back(Node {});
			_nodes.push_back(Node {});
			stack.push_back({left, j.begin, mid});
			stack.push_back({left + 1, mid, j.end});
		}
		std::vector<Tri> sorted(n);
		for (size_t i = 0; i < n; ++i) sorted[i] = _tris[order[i]];
		_tris.swap(sorted);
	}

	// The nearest triangle along the ray within (tMin, tMax): how much it
	// blocks (0 if none) and how far it is.
	// `hitTri`, if given, receives the triangle hit; `any`: stop at the first one found (for yes/no questions).
	float nearest(V3 o, V3 d, float tMin, float tMax, float& t, const Tri** hitTri = nullptr, bool any = false) const {
		if (_nodes.empty()) return 0.0f;
		const float dir[3] = {d.x, d.y, d.z};
		float inv[3];
		for (int a = 0; a < 3; ++a) inv[a] = 1.0f / (std::fabs(dir[a]) > 1e-12f ? dir[a] : 1e-12f);
		const float org[3] = {o.x, o.y, o.z};
		float best = tMax, blocks = 0.0f;
		if (hitTri) *hitTri = nullptr;
		u32 stack[64];
		int sp = 0;
		stack[sp++] = 0;
		while (sp > 0) {
			const Node& n = _nodes[stack[--sp]];
			float t0 = tMin, t1 = best;
			for (int a = 0; a < 3; ++a) {
				const float n0 = (n.mn[a] - org[a]) * inv[a], n1 = (n.mx[a] - org[a]) * inv[a];
				t0 = std::max(t0, std::min(n0, n1));
				t1 = std::min(t1, std::max(n0, n1));
			}
			if (t0 > t1) continue;
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
				// The child on the ray's side first (it is popped last-pushed first).
				const bool leftNear = dir[n.axis] >= 0.0f;
				stack[sp++] = leftNear ? n.first + 1 : n.first;
				stack[sp++] = leftNear ? n.first : n.first + 1;
			}
		}
		t = best;
		return blocks;
	}

	// Whether one triangle is met by the ray within (tMin, tMax).
	static bool meets(const Tri& tri, V3 o, V3 d, float tMin, float tMax) {
		const V3 p = cross(d, tri.e2);
		const float det = dot(tri.e1, p);
		if (std::fabs(det) < 1e-9f) return false;
		const float id = 1.0f / det;
		const V3 s = o - tri.a;
		const float u = dot(s, p) * id;
		if (u < 0.0f || u > 1.0f) return false;
		const V3 q = cross(s, tri.e1);
		const float v = dot(d, q) * id;
		if (v < 0.0f || u + v > 1.0f) return false;
		const float h = dot(tri.e2, q) * id;
		return h > tMin && h < tMax;
	}

	size_t size() const { return _tris.size(); }

private:
	std::vector<Tri> _tris;
	std::vector<Node> _nodes;
};

} // namespace geo
} // namespace g3
