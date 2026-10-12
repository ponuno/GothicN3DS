#include "vertex_fx.h"

#include "g3ds/world.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <unordered_map>

namespace g3 {

namespace {

using i64 = long long;

struct V3 {
	float x, y, z;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(V3 a) { return std::sqrt(dot(a, a)); }
inline V3 at(const SrcVertex& v) { return {v.pos[0], v.pos[1], v.pos[2]}; }

// Front faces are counter-clockwise; in this left-handed system the front
// normal is -cross(b - a, c - a) (see the winding check in bake.cpp). Its
// length is twice the area.
inline V3 frontNormal(const SrcTriangle& t) { return cross(at(t.v[1]) - at(t.v[0]), at(t.v[2]) - at(t.v[0])) * -1.0f; }

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
	float nearest(V3 o, V3 d, float tMin, float tMax, float& t) const {
		if (_nodes.empty()) return 0.0f;
		const float inv[3] = {1.0f / (std::fabs(d.x) > 1e-12f ? d.x : 1e-12f), 1.0f / (std::fabs(d.y) > 1e-12f ? d.y : 1e-12f),
		                      1.0f / (std::fabs(d.z) > 1e-12f ? d.z : 1e-12f)};
		const float org[3] = {o.x, o.y, o.z};
		float best = tMax, blocks = 0.0f;
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

struct PosKey {
	i64 x, y, z;
	bool operator==(const PosKey& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct PosKeyHash {
	size_t operator()(const PosKey& k) const {
		return size_t(u64(k.x) * 73856093ull ^ u64(k.y) * 19349663ull ^ u64(k.z) * 83492791ull);
	}
};
// A quarter of a centimetre: corners nearer than that are one place.
inline PosKey posKey(const SrcVertex& v) {
	return PosKey {i64(std::llround(double(v.pos[0]) * 4.0)), i64(std::llround(double(v.pos[1]) * 4.0)),
	               i64(std::llround(double(v.pos[2]) * 4.0))};
}

struct ShotKey {
	u32 place;
	i32 nx, ny, nz;
	bool operator==(const ShotKey& o) const { return place == o.place && nx == o.nx && ny == o.ny && nz == o.nz; }
};
struct ShotKeyHash {
	size_t operator()(const ShotKey& k) const {
		return size_t(u64(k.place) * 2654435761ull ^ u64(u32(k.nx)) * 73856093ull ^ u64(u32(k.ny)) * 19349663ull ^
		              u64(u32(k.nz)) * 83492791ull);
	}
};

struct Shot {
	V3 pos, normal;
	V3 away; // out of the corner: the rays start a little off every surface that meets here
	u8 result = 0;
};

} // namespace

void bakeOcclusion(SrcScene& scene, const std::vector<AoMaterial>& materials, const AoOptions& opt, AoStats& stats) {
	stats = AoStats {};
	if (opt.level == AO_OFF) return;
	const auto start = std::chrono::steady_clock::now();
	const u32 rays = opt.rays ? opt.rays : (opt.level == AO_HIGH ? 40u : 16u);
	const float radius = opt.radius > 0 ? opt.radius : (opt.level == AO_HIGH ? 400.0f : 250.0f);
	stats.rayCount = rays;
	stats.radius = radius;
	auto mat = [&](u32 m) { return m < materials.size() ? materials[m] : AoMaterial {}; };

	// ---- what blocks ----
	std::vector<Tri> blockers;
	for (const SrcTriangle& t : scene.triangles) {
		const float b = mat(t.material).blocks;
		if (b <= 0.0f || (t.detail & 3) == DETAIL_SMALL) continue;
		const V3 a = at(t.v[0]);
		const V3 e1 = at(t.v[1]) - a, e2 = at(t.v[2]) - a;
		if (length(cross(e1, e2)) < 1e-4f) continue;
		blockers.push_back(Tri {a, e1, e2, b});
	}
	stats.occluders = blockers.size();
	Bvh bvh;
	bvh.build(std::move(blockers));

	// ---- one shot per place and smooth normal ----
	// The corners of one place, for the normals of its neighbours.
	std::unordered_map<PosKey, u32, PosKeyHash> placeOf;
	std::vector<std::vector<u32>> facesAt; // per place: triangle index * 3 + corner
	std::vector<V3> faceNormal(scene.triangles.size());
	for (size_t t = 0; t < scene.triangles.size(); ++t) {
		const SrcTriangle& tri = scene.triangles[t];
		faceNormal[t] = frontNormal(tri);
		if (!mat(tri.material).receives) continue;
		for (int k = 0; k < 3; ++k) {
			auto ins = placeOf.emplace(posKey(tri.v[k]), u32(facesAt.size()));
			if (ins.second) facesAt.emplace_back();
			facesAt[ins.first->second].push_back(u32(t * 3 + size_t(k)));
		}
	}
	std::unordered_map<ShotKey, u32, ShotKeyHash> shotOf;
	std::vector<Shot> shots;
	std::vector<u32> cornerShot(scene.triangles.size() * 3, ~0u);
	for (u32 place = 0; place < u32(facesAt.size()); ++place) {
		const std::vector<u32>& corners = facesAt[place];
		// Where a floor meets a wall the corner lies in both: started off the
		// floor alone, the rays towards the wall would start in its plane and
		// pass through it. Off all the faces here together, they do not.
		V3 away {0, 0, 0};
		for (size_t k = 0; k < corners.size() && k < 256; ++k) {
			const V3 m = faceNormal[corners[k] / 3];
			const float ml = length(m);
			if (ml > 1e-6f) away = away + m * (1.0f / ml);
		}
		const float awayLen = length(away);
		for (u32 c : corners) {
			const u32 t = c / 3;
			const float len = length(faceNormal[t]);
			if (len < 1e-6f) continue;
			const V3 n0 = faceNormal[t] * (1.0f / len);
			// The faces here that bend less than 60 degrees away make one smooth surface.
			// (A place shared by very many faces - a seam of the source - keeps its own.)
			V3 sum = n0;
			if (corners.size() <= 256) {
				sum = V3 {0, 0, 0};
				for (u32 o : corners) {
					const V3 m = faceNormal[o / 3];
					const float ml = length(m);
					if (ml < 1e-6f || dot(m, n0) < 0.5f * ml) continue;
					sum = sum + m; // (weighted by area)
				}
			}
			const float sl = length(sum);
			const V3 n = sl > 1e-6f ? sum * (1.0f / sl) : n0;
			const ShotKey key {place, i32(std::lround(n.x * 64.0f)), i32(std::lround(n.y * 64.0f)), i32(std::lround(n.z * 64.0f))};
			auto ins = shotOf.emplace(key, u32(shots.size()));
			if (ins.second) {
				const SrcVertex& v = scene.triangles[t].v[c % 3];
				const V3 off = awayLen > 0.1f ? away * (1.0f / awayLen) : n;
				shots.push_back(Shot {V3 {v.pos[0], v.pos[1], v.pos[2]}, n, dot(off, n) > 0.0f ? off : n, 0});
			}
			cornerShot[c] = ins.first->second;
		}
	}
	stats.unique = shots.size();

	// Directions over the half sphere, as many towards the top as its light
	// is worth (cosine-weighted): a spiral of points on the disc lifted up.
	std::vector<V3> dirs(rays);
	const float golden = 2.39996323f;
	for (u32 i = 0; i < rays; ++i) {
		const float u = (float(i) + 0.5f) / float(rays);
		const float r = std::sqrt(u), phi = float(i) * golden;
		dirs[i] = V3 {r * std::cos(phi), r * std::sin(phi), std::sqrt(std::max(0.0f, 1.0f - u))};
	}

	std::atomic<size_t> next {0};
	std::atomic<u64> rayCount {0};
	auto work = [&]() {
		u64 cast = 0;
		for (;;) {
			const size_t begin = next.fetch_add(512);
			if (begin >= shots.size()) break;
			const size_t end = std::min(shots.size(), begin + 512);
			for (size_t i = begin; i < end; ++i) {
				Shot& s = shots[i];
				const V3 n = s.normal;
				// A frame around the normal, turned by a different angle at every
				// place so that the pattern of the rays does not show.
				const V3 helper = std::fabs(n.y) < 0.9f ? V3 {0, 1, 0} : V3 {1, 0, 0};
				V3 tx = cross(helper, n);
				tx = tx * (1.0f / length(tx));
				const V3 ty = cross(n, tx);
				const u32 h = u32(i64(s.pos.x * 4.0f) * 73856093ll ^ i64(s.pos.y * 4.0f) * 19349663ll ^ i64(s.pos.z * 4.0f) * 83492791ll);
				const float turn = float(h % 6283u) * 0.001f, ct = std::cos(turn), st = std::sin(turn);
				const V3 o = s.pos + n * 1.0f + s.away * 1.5f; // off the surface itself, and out of the corner
				float sum = 0.0f;
				for (const V3& l : dirs) {
					const float lx = l.x * ct - l.y * st, ly = l.x * st + l.y * ct;
					const V3 d = tx * lx + ty * ly + n * l.z;
					float t = 0;
					const float b = bvh.nearest(o, d, 0.5f, radius, t);
					if (b > 0.0f) sum += b * (1.0f - t / radius);
				}
				cast += rays;
				// Near walls rarely stop more than half the rays: lifted so that a
				// corner between floor and wall comes out properly dark.
				const float occl = std::min(1.0f, sum / float(rays) * 1.6f);
				s.result = u8(std::lround(occl * 255.0f));
			}
		}
		rayCount += cast;
	};
	const unsigned threads = std::max(1u, opt.threads);
	std::vector<std::thread> pool;
	for (unsigned i = 1; i < threads; ++i) pool.emplace_back(work);
	work();
	for (std::thread& t : pool) t.join();
	stats.rays = rayCount.load();

	double total = 0;
	for (size_t t = 0; t < scene.triangles.size(); ++t)
		for (int k = 0; k < 3; ++k) {
			const u32 s = cornerShot[t * 3 + size_t(k)];
			SrcVertex& v = scene.triangles[t].v[k];
			v.occlusion = s == ~0u ? u8(0) : shots[s].result;
			if (s == ~0u) continue;
			++stats.corners;
			total += v.occlusion;
			stats.darkCorners += v.occlusion > 127;
		}
	stats.mean = stats.corners ? float(total / double(stats.corners) / 255.0) : 0.0f;
	stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// ---- wind ----

namespace {

// The keyword is the word (or its start, with a plural ending and a number after it: TREES, BUSH01, TREEA1) or
// its end (BIGTREE, BIGTREES). Not just any letters at its start: EICHENHAIN is not an EICHE, STREET is no TREE.
bool wordHas(const std::string& word, const char* kw) {
	const size_t n = std::char_traits<char>::length(kw);
	if (word.size() < n) return false;
	if (word.compare(0, n, kw) == 0) {
		std::string rest = word.substr(n);
		for (const char* plural : {"EN", "ES", "ER", "S", "N", "E"})
			if (rest.compare(0, std::char_traits<char>::length(plural), plural) == 0) {
				rest = rest.substr(std::char_traits<char>::length(plural));
				break;
			}
		if (!rest.empty() && rest[0] >= 'A' && rest[0] <= 'Z') rest = rest.substr(1); // (TREEA1)
		bool digits = true;
		for (char c : rest) digits = digits && c >= '0' && c <= '9';
		if (digits) return true;
	}
	if (word.compare(word.size() - n, n, kw) == 0) return true;
	return word.size() > n && word.back() == 'S' && word.compare(word.size() - n - 1, n, kw) == 0;
}

} // namespace

PlantKind plantKindOf(const std::string& name, const std::vector<std::string>& extra) {
	static const char* const kTree[] = {"TREE", "BAUM", "PALM", "PINE", "TANNE", "FICHTE", "BIRKE", "EICHE", "WILLOW"};
	static const char* const kBush[] = {"BUSH", "BUSCH", "STRAUCH", "SHRUB", "HECKE", "HEDGE", "PLANT", "PFLANZ", "IVY",
	                                    "EFEU", "BRANCH", "ZWEIG", "LEAF", "LEAV", "BLATT", "BLAETTER", "LAUB"};
	static const char* const kGrass[] = {"GRAS", "FARN", "FERN", "REED", "SCHILF", "FLOWER", "BLUME", "HALM", "KRAUT"};
	static const char* const kNot[] = {"STUMP", "STUMPF", "ROOT", "WURZEL", "TRUNK", "STAMM", "LOG"};
	std::vector<std::string> words(1);
	for (char c : name) {
		const char u = (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c;
		if ((u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9'))
			words.back() += u;
		else if (!words.back().empty())
			words.emplace_back();
	}
	PlantKind kind = PlantKind::None;
	for (const std::string& w : words) {
		if (w.empty()) continue;
		for (const char* k : kNot)
			if (wordHas(w, k)) return PlantKind::None;
		for (const char* k : kTree)
			if (wordHas(w, k)) kind = PlantKind::Tree;
		for (const std::string& k : extra)
			if (!k.empty() && wordHas(w, k.c_str())) kind = PlantKind::Tree;
		if (kind == PlantKind::Tree) continue;
		for (const char* k : kBush)
			if (wordHas(w, k)) kind = PlantKind::Bush;
		if (kind == PlantKind::None)
			for (const char* k : kGrass)
				if (wordHas(w, k)) kind = PlantKind::Grass;
	}
	return kind;
}

float plantSway(PlantKind kind, float heightCm) {
	switch (kind) {
	case PlantKind::Tree: return std::clamp(heightCm * 0.015f, 4.0f, 20.0f);
	case PlantKind::Bush: return std::clamp(heightCm * 0.04f, 2.0f, 10.0f);
	case PlantKind::Grass: return std::clamp(heightCm * 0.10f, 1.5f, 8.0f);
	default: return 0.0f;
	}
}

void markSway(SrcTriangle* tris, size_t count, PlantKind kind, float footY, float topY, float swayCm) {
	if (kind == PlantKind::None || !(topY > footY) || swayCm <= 0.0f) return;
	const float span = topY - footY;
	for (size_t t = 0; t < count; ++t)
		for (SrcVertex& v : tris[t].v) {
			const float h = std::clamp((v.pos[1] - footY) / span, 0.0f, 1.0f);
			const float bend = kind == PlantKind::Tree ? h * h : h;
			const long steps = std::lround(swayCm * bend / WORLD_WIND_UNIT);
			v.wind = u8(std::clamp(steps, 0l, 127l));
		}
}

} // namespace g3
