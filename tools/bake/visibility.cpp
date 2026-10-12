#include "visibility.h"

#include "bvh.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <thread>

namespace g3 {

namespace {

using namespace geo;

struct Box {
	float mn[3], mx[3];
	bool empty() const { return mn[0] > mx[0]; }
};

struct ChunkVis {
	int cx = 0, cz = 0;
	u32 clusters = 1;
	u32 firstBox = 0; // into the flat list of boxes
};

struct Sample {
	V3 p;
};

inline V3 frontNormalOf(const Tri& t) { return cross(t.e1, t.e2) * -1.0f; }

struct Shared {
	const Bvh* bvh;
	const std::vector<Box>* boxes;
	const std::vector<V3>* points;      // surface points of the clusters, 20 or so each: where rays are aimed
	const std::vector<u32>* pointStart; // per box: first point (+1 entry at the end)
	const std::vector<ChunkVis>* chunks;
	const std::vector<u32>* order; // chunk numbers sorted by cell z, x, number
	std::map<std::pair<int, int>, std::pair<u32, u32>> cellRange; // (z, x) -> [begin, end) in `order`
	VisOptions opt;
	float cellSize;
	float skyY, floorY;
	mutable std::atomic<u64> rays {0};
};

// The places a character in the cell may have its eyes at.
void viewingPlaces(const Shared& sh, int cx, int cz, std::vector<V3>& out) {
	out.clear();
	const int n = sh.opt.high ? 3 : 2;
	const float fr3[3] = {0.1f, 0.5f, 0.9f}, fr2[2] = {0.1f, 0.9f};
	for (int iz = 0; iz < n; ++iz)
		for (int ix = 0; ix < n; ++ix) {
			const float x = (float(cx) + (n == 3 ? fr3[ix] : fr2[ix])) * sh.cellSize;
			const float z = (float(cz) + (n == 3 ? fr3[iz] : fr2[iz])) * sh.cellSize;
			// Down from the sky: the ground, and every floor above it that a character could stand on.
			float y = sh.skyY;
			float lastLevel = 1e30f;
			int levels = 0;
			bool top = true;
			for (int hit = 0; hit < 12 && levels < 4; ++hit) {
				float t = 0;
				const Tri* tri = nullptr;
				const V3 o {x, y, z};
				if (sh.bvh->nearest(o, V3 {0, -1, 0}, 1e-3f, y - sh.floorY, t, &tri) <= 0.0f || !tri) break;
				const float hy = y - t;
				const V3 nrm = frontNormalOf(*tri);
				const bool up = nrm.y > 0.5f * length(nrm);
				if ((up || top) && lastLevel - hy >= 200.0f) {
					out.push_back(V3 {x, hy + 170.0f, z});
					if (top) out.push_back(V3 {x, hy + 450.0f, z}); // (the camera behind a character, or one who jumps)
					lastLevel = hy;
					++levels;
				}
				top = false;
				y = hy - 1.0f;
			}
		}
	// A camera on a high spot between the places sees what the places, lower down, do not: from the top of the
	// cell's highest ground (found on a finer lattice), at the height of the camera behind a character.
	float highest = -1e30f;
	for (int iz = 0; iz < 5; ++iz)
		for (int ix = 0; ix < 5; ++ix) {
			float t = 0;
			const V3 o {(float(cx) + 0.05f + 0.225f * float(ix)) * sh.cellSize, sh.skyY, (float(cz) + 0.05f + 0.225f * float(iz)) * sh.cellSize};
			if (sh.bvh->nearest(o, V3 {0, -1, 0}, 1e-3f, sh.skyY - sh.floorY, t) > 0.0f) highest = std::max(highest, sh.skyY - t);
		}
	if (highest > -1e29f)
		for (int iz = 0; iz < n; ++iz)
			for (int ix = 0; ix < n; ++ix)
				out.push_back(V3 {(float(cx) + (n == 3 ? fr3[ix] : fr2[ix])) * sh.cellSize, highest + 450.0f, (float(cz) + (n == 3 ? fr3[iz] : fr2[iz])) * sh.cellSize});
}

// Whether the segment from `s` to the point `p` meets something solid before it gets there (a few cm short:
// the surface the point is on does not count).
bool blockedTo(const Shared& sh, V3 s, V3 p) {
	const V3 d = p - s;
	const float len = length(d);
	if (len < 1.0f) return false;
	const float tMax = 1.0f - 3.0f / len;
	if (tMax <= 1e-4f) return false;
	float t = 0;
	sh.rays.fetch_add(1, std::memory_order_relaxed);
	return sh.bvh->nearest(s, d, 1e-4f, tMax, t, nullptr, true) > 0.0f;
}

bool visibleFrom(const Shared& sh, const std::vector<V3>& places, u32 box, int cx, int cz) {
	const Box& b = (*sh.boxes)[box];
	if (b.empty()) return true;
	// Whatever is near the cell - not near one of its few places: the camera can be anywhere in it - is visible.
	{
		const float x0 = float(cx) * sh.cellSize, x1 = x0 + sh.cellSize, z0 = float(cz) * sh.cellSize, z1 = z0 + sh.cellSize;
		const float dx = std::max({b.mn[0] - x1, x0 - b.mx[0], 0.0f}), dz = std::max({b.mn[2] - z1, z0 - b.mx[2], 0.0f});
		if (std::sqrt(dx * dx + dz * dz) <= sh.opt.nearAlways) return true;
	}
	const u32 first = (*sh.pointStart)[box], last = (*sh.pointStart)[box + 1];
	if (first == last) return true; // (nothing to aim at: say nothing)
	for (const V3& s : places)
		for (u32 k = first; k < last; ++k)
			if (!blockedTo(sh, s, (*sh.points)[k])) return true;
	return false;
}

} // namespace

std::vector<u8> buildVisibility(const SrcScene& scene, const std::vector<AoMaterial>& materials,
                                const std::vector<BakedChunk>& chunks, float cellSize, const VisOptions& opt, VisStats& stats) {
	stats = VisStats {};
	stats.radius = opt.radius;
	if (!opt.enabled || chunks.empty() || cellSize <= 0.0f) return {};
	const auto start = std::chrono::steady_clock::now();

	// ---- what blocks: opaque triangles of the world itself ----
	std::vector<Tri> blockers;
	float skyY = -1e30f, floorY = 1e30f;
	for (const SrcTriangle& t : scene.triangles) {
		const float b = t.material < materials.size() ? materials[t.material].blocks : 1.0f;
		if (b < 1.0f || (t.detail & 3) != DETAIL_ALWAYS) continue;
		const V3 a {t.v[0].pos[0], t.v[0].pos[1], t.v[0].pos[2]};
		const V3 e1 = V3 {t.v[1].pos[0], t.v[1].pos[1], t.v[1].pos[2]} - a, e2 = V3 {t.v[2].pos[0], t.v[2].pos[1], t.v[2].pos[2]} - a;
		if (length(cross(e1, e2)) < 1e-4f) continue;
		blockers.push_back(Tri {a, e1, e2, 1.0f});
		for (int k = 0; k < 3; ++k) {
			skyY = std::max(skyY, t.v[k].pos[1]);
			floorY = std::min(floorY, t.v[k].pos[1]);
		}
	}
	if (blockers.empty()) return {};
	Bvh bvh;
	bvh.build(std::move(blockers));

	// ---- the clusters of the chunks ----
	std::vector<ChunkVis> cv(chunks.size());
	std::vector<Box> boxes;
	for (size_t i = 0; i < chunks.size(); ++i) {
		const BakedChunk& c = chunks[i];
		u32 clusters = 1;
		for (const ChunkBatch& b : c.batches) clusters = std::max<u32>(clusters, u32(b.cluster) + 1);
		cv[i].cx = c.gridX;
		cv[i].cz = c.gridZ;
		cv[i].clusters = clusters;
		cv[i].firstBox = u32(boxes.size());
		boxes.resize(boxes.size() + clusters, Box {{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}});
		for (const ChunkBatch& b : c.batches) {
			if (!b.indexCount) continue;
			Box& box = boxes[cv[i].firstBox + b.cluster];
			for (int a = 0; a < 3; ++a) {
				box.mn[a] = std::min(box.mn[a], c.header.origin[a] + float(b.bbMin[a]) * c.header.posScale - 5.0f);
				box.mx[a] = std::max(box.mx[a], c.header.origin[a] + float(b.bbMax[a]) * c.header.posScale + 5.0f);
			}
		}
	}
	// Where rays are aimed: points on the cluster's own surface - the middles of an even share of its
	// triangles, and the corners that reach farthest up, down and to each side - a little above it. (Its
	// box would do for walls, but a crate in the box is seen over a ridge when the box's corners are not.)
	std::vector<V3> points;
	std::vector<u32> pointStart(boxes.size() + 1, 0);
	for (size_t i = 0; i < chunks.size(); ++i) {
		const BakedChunk& c = chunks[i];
		const float* o = c.header.origin;
		const float sc = c.header.posScale;
		auto world = [&](const WorldVertex& v) { return V3 {o[0] + float(v.x) * sc, o[1] + float(v.y) * sc + 10.0f, o[2] + float(v.z) * sc}; };
		std::vector<std::vector<u32>> tris(cv[i].clusters); // first index of each triangle, per cluster
		for (const ChunkBatch& b : c.batches)
			for (u32 k = 0; k + 2 < b.indexCount; k += 3) tris[b.cluster].push_back(b.firstIndex + k);
		for (u32 cl = 0; cl < cv[i].clusters; ++cl) {
			pointStart[cv[i].firstBox + cl] = u32(points.size());
			const std::vector<u32>& list = tris[cl];
			if (list.empty()) continue;
			const u32 take = std::min<u32>(u32(list.size()), 16u);
			for (u32 k = 0; k < take; ++k) {
				const u32 first = list[size_t(k) * list.size() / take];
				V3 m {0, 0, 0};
				for (int e = 0; e < 3; ++e) m = m + world(c.vertices[c.indices[first + e]]) * (1.0f / 3.0f);
				points.push_back(m);
			}
			// The corners that reach farthest on each axis, both ways.
			u32 ext[6];
			bool have = false;
			for (u32 first : list)
				for (int e = 0; e < 3; ++e) {
					const u32 vi = c.indices[first + e];
					const WorldVertex& v = c.vertices[vi];
					const int val[3] = {v.x, v.y, v.z};
					if (!have) {
						for (u32& x : ext) x = vi;
						have = true;
						continue;
					}
					for (int a = 0; a < 3; ++a) {
						const WorldVertex& lo = c.vertices[ext[a * 2]];
						const WorldVertex& hi = c.vertices[ext[a * 2 + 1]];
						const int vlo[3] = {lo.x, lo.y, lo.z}, vhi[3] = {hi.x, hi.y, hi.z};
						if (val[a] < vlo[a]) ext[a * 2] = vi;
						if (val[a] > vhi[a]) ext[a * 2 + 1] = vi;
					}
				}
			if (have)
				for (u32 vi : ext) points.push_back(world(c.vertices[vi]));
		}
	}
	pointStart[boxes.size()] = u32(points.size());
	std::vector<u32> order(chunks.size());
	for (u32 i = 0; i < order.size(); ++i) order[i] = i;
	std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
		if (cv[a].cz != cv[b].cz) return cv[a].cz < cv[b].cz;
		if (cv[a].cx != cv[b].cx) return cv[a].cx < cv[b].cx;
		return a < b;
	});

	Shared sh;
	sh.bvh = &bvh;
	sh.boxes = &boxes;
	sh.points = &points;
	sh.pointStart = &pointStart;
	sh.chunks = &cv;
	sh.order = &order;
	sh.opt = opt;
	sh.cellSize = cellSize;
	sh.skyY = skyY + 10.0f;
	sh.floorY = floorY - 10.0f;
	for (u32 k = 0; k < order.size();) {
		u32 e = k;
		while (e < order.size() && cv[order[e]].cz == cv[order[k]].cz && cv[order[e]].cx == cv[order[k]].cx) ++e;
		sh.cellRange[{cv[order[k]].cz, cv[order[k]].cx}] = {k, e};
		k = e;
	}

	// ---- one row per cell ----
	struct Row {
		int x, z;
		std::vector<u32> bits; // packed
		u32 count = 0;
		u64 hidden = 0, samples = 0;
	};
	std::vector<std::pair<int, int>> cells; // (z, x), ascending
	for (const auto& kv : sh.cellRange) cells.push_back(kv.first);
	std::vector<Row> rows(cells.size());
	const int reach = int(std::ceil(opt.radius / cellSize)) + 1;
	std::atomic<size_t> next {0};
	auto worker = [&]() {
		std::vector<V3> places;
		for (;;) {
			const size_t r = next.fetch_add(1);
			if (r >= cells.size()) return;
			Row& row = rows[r];
			row.z = cells[r].first;
			row.x = cells[r].second;
			viewingPlaces(sh, row.x, row.z, places);
			row.samples = places.size();
			std::vector<u8> bitsOut;
			for (int dz = -reach; dz <= reach; ++dz)
				for (int dx = -reach; dx <= reach; ++dx) {
					const float gx = float(std::max(std::abs(dx) - 1, 0)) * cellSize, gz = float(std::max(std::abs(dz) - 1, 0)) * cellSize;
					if (std::sqrt(gx * gx + gz * gz) > opt.radius) continue;
					auto it = sh.cellRange.find({row.z + dz, row.x + dx});
					if (it == sh.cellRange.end()) continue;
					for (u32 k = it->second.first; k < it->second.second; ++k) {
						const ChunkVis& c = cv[order[k]];
						for (u32 cl = 0; cl < c.clusters; ++cl) {
							// A row with no places (no ground in the cell) shows everything.
							const bool vis = places.empty() || visibleFrom(sh, places, c.firstBox + cl, row.x, row.z);
							bitsOut.push_back(vis ? 1 : 0);
							row.hidden += !vis;
						}
					}
				}
			row.count = u32(bitsOut.size());
			row.bits.assign((bitsOut.size() + 31) / 32, 0u);
			for (size_t i = 0; i < bitsOut.size(); ++i)
				if (bitsOut[i]) row.bits[i >> 5] |= 1u << (i & 31);
			if (places.empty()) row.count = 0xFFFFFFFFu; // (marked: no row)
		}
	};
	const unsigned threads = std::max(1u, opt.threads);
	std::vector<std::thread> pool;
	for (unsigned t = 1; t < threads; ++t) pool.emplace_back(worker);
	worker();
	for (std::thread& t : pool) t.join();

	// ---- the table ----
	std::vector<VisViewer> viewers;
	std::vector<u32> bits;
	u64 totalBits = 0;
	for (const Row& row : rows) {
		stats.samples += row.samples;
		if (row.count == 0xFFFFFFFFu || row.count == 0) continue;
		if (totalBits > 0xFFFFFFF0ull - row.count) break; // (more than a table can hold)
		VisViewer v {};
		v.x = i16(row.x);
		v.z = i16(row.z);
		v.bitOffset = u32(totalBits);
		viewers.push_back(v);
		// Rows begin on whatever bit the last one ended on: append bit by bit.
		bits.resize((totalBits + row.count + 31) / 32, 0u);
		for (u32 i = 0; i < row.count; ++i)
			if (row.bits[i >> 5] & (1u << (i & 31))) {
				const u64 b = totalBits + i;
				bits[b >> 5] |= 1u << (b & 31);
			}
		totalBits += row.count;
		stats.hidden += row.hidden;
	}
	stats.viewers = u32(viewers.size());
	stats.targets = totalBits;
	if (viewers.empty()) return {};
	std::sort(viewers.begin(), viewers.end(), [](const VisViewer& a, const VisViewer& b) { return a.z != b.z ? a.z < b.z : a.x < b.x; });

	VisHeader h {};
	h.magic = VIS_MAGIC;
	h.version = VIS_VERSION;
	h.chunkCount = u32(chunks.size());
	h.viewerCount = u32(viewers.size());
	h.bitCount = u32(totalBits);
	h.radius = opt.radius;
	h.cellSize = cellSize;
	std::vector<u8> out(visBlobSize(h), 0);
	u8* p = out.data();
	auto put = [&p](const void* src, size_t n) {
		if (n) std::memcpy(p, src, n);
		p += n;
	};
	put(&h, sizeof(h));
	std::vector<i16> cxs(chunks.size()), czs(chunks.size());
	std::vector<u8> counts(chunks.size());
	for (size_t i = 0; i < chunks.size(); ++i) {
		cxs[i] = i16(cv[i].cx);
		czs[i] = i16(cv[i].cz);
		counts[i] = u8(std::min<u32>(cv[i].clusters, 255u));
	}
	put(cxs.data(), cxs.size() * 2);
	put(czs.data(), czs.size() * 2);
	put(counts.data(), counts.size());
	p += visPad(h.chunkCount);
	put(order.data(), order.size() * 4);
	put(viewers.data(), viewers.size() * sizeof(VisViewer));
	put(bits.data(), bits.size() * 4);
	stats.bytes = u32(out.size());
	stats.rays = sh.rays.load();
	stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	return out;
}

} // namespace g3
