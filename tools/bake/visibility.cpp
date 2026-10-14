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

// A place a character in the cell may have its eyes at, and the height of the floor under it.
struct Place {
	V3 p;
	float floorY;
	u16 room = 0; // the room (BSP sector, 1..) the place is in; 0: outside (fix91)
};

// A room's box (the same boxes the runtime's Rooms uses).
struct RoomB {
	float mn[3], mx[3];
	u16 room;
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
	const std::vector<RoomB>* rooms = nullptr;
	const std::vector<u16>* boxRoom = nullptr; // per cluster box: the room it lies inside of (fix91), 0: none
	std::map<std::pair<int, int>, std::vector<u32>> roomsInCell; // (z, x) -> the rooms (indices) whose middle is in the cell
	VisOptions opt;
	float cellSize;
	float skyY, floorY;
	mutable std::atomic<u64> rays {0};
};

u16 roomOfPoint(const Shared& sh, V3 p);

// The places a character in the cell may have its eyes at.
void viewingPlaces(const Shared& sh, int cx, int cz, std::vector<Place>& out) {
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
					out.push_back({V3 {x, hy + 170.0f, z}, hy});
					if (top) out.push_back({V3 {x, hy + 450.0f, z}, hy}); // (the camera behind a character, or one who jumps)
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
				out.push_back({V3 {(float(cx) + (n == 3 ? fr3[ix] : fr2[ix])) * sh.cellSize, highest + 450.0f, (float(cz) + (n == 3 ? fr3[iz] : fr2[iz])) * sh.cellSize}, highest});
	// (A place of the lattice that falls inside a room is that room's.)
	for (Place& pl : out) pl.room = roomOfPoint(sh, pl.p);
}

// The room a point is in: the smallest box that holds it (the runtime's Rooms::roomAt does the same), 0: none.
u16 roomOfPoint(const Shared& sh, V3 p) {
	u16 best = 0;
	float bestVolume = 1e30f;
	if (!sh.rooms) return 0;
	for (const RoomB& b : *sh.rooms) {
		if (p.x < b.mn[0] || p.x > b.mx[0] || p.z < b.mn[2] || p.z > b.mx[2] || p.y < b.mn[1] - 20.0f || p.y > b.mx[1] + 20.0f) continue;
		const float v = (b.mx[0] - b.mn[0]) * (b.mx[1] - b.mn[1] + 1.0f) * (b.mx[2] - b.mn[2]);
		if (v < bestVolume) {
			bestVolume = v;
			best = b.room;
		}
	}
	return best;
}

// Places inside the rooms whose middle is in the cell: five to a room (the middle and the quarter points), on the
// floor under them. (The cell's own lattice finds few of them: a room is a few metres wide.)
void roomPlaces(const Shared& sh, int cx, int cz, std::vector<Place>& out) {
	auto it = sh.roomsInCell.find({cz, cx});
	if (it == sh.roomsInCell.end()) return;
	for (u32 ri : it->second) {
		const RoomB& rb = (*sh.rooms)[ri];
		const float mx = 0.5f * (rb.mn[0] + rb.mx[0]), mz = 0.5f * (rb.mn[2] + rb.mx[2]);
		const float dx = 0.25f * (rb.mx[0] - rb.mn[0]), dz = 0.25f * (rb.mx[2] - rb.mn[2]);
		const float xs[5] = {mx, mx - dx, mx + dx, mx - dx, mx + dx}, zs[5] = {mz, mz - dz, mz - dz, mz + dz, mz + dz};
		for (int k = 0; k < 5; ++k) {
			float t = 0;
			const Tri* tri = nullptr;
			const V3 o {xs[k], rb.mx[1] - 30.0f, zs[k]};
			if (sh.bvh->nearest(o, V3 {0, -1, 0}, 1e-3f, o.y - sh.floorY, t, &tri) <= 0.0f || !tri) continue;
			const float hy = o.y - t;
			if (hy < rb.mn[1] - 80.0f || hy > rb.mx[1]) continue;
			Place pl {V3 {xs[k], hy + 170.0f, zs[k]}, hy, rb.room};
			out.push_back(pl);
		}
	}
}

// Whether the segment from `s` to the point `p` meets something solid before it gets there (a few cm short:
// the surface the point is on does not count).
// What last blocked the rays from one place: the next ray from there to a neighbouring point is mostly
// stopped by the same wall, which is found without looking through the hierarchy.
struct Occluders {
	const Tri* tri[4] = {};
	int next = 0;
};

bool blockedTo(const Shared& sh, V3 s, V3 p, Occluders& oc) {
	const V3 d = p - s;
	const float len = length(d);
	if (len < 1.0f) return false;
	const float tMax = 1.0f - 3.0f / len;
	if (tMax <= 1e-4f) return false;
	float t = 0;
	sh.rays.fetch_add(1, std::memory_order_relaxed);
	for (const Tri* c : oc.tri)
		if (c && Bvh::meets(*c, s, d, 1e-4f, tMax)) return true;
	const Tri* hit = nullptr;
	if (sh.bvh->nearest(s, d, 1e-4f, tMax, t, &hit, true) <= 0.0f || !hit) return false;
	oc.tri[oc.next] = hit;
	oc.next = (oc.next + 1) & 3;
	return true;
}

bool visibleFrom(const Shared& sh, const std::vector<V3>& places, std::vector<Occluders>& cache, u32 box, int cx, int cz, float eyeLo, float eyeHi,
                 u16 rowRoom) {
	const Box& b = (*sh.boxes)[box];
	if (b.empty()) return true;
	// Whatever is near the cell - not near one of its few places: the camera can be anywhere in it - is visible.
	// (Except what lies inside a room that is not the row's own: a house's inside is not near the street - only what the
	// rays see of it through its doors and windows is visible from outside; fix91.)
	const u16 inRoom = sh.boxRoom ? (*sh.boxRoom)[box] : u16(0);
	if (inRoom == 0 || inRoom == rowRoom) {
		const float x0 = float(cx) * sh.cellSize, x1 = x0 + sh.cellSize, z0 = float(cz) * sh.cellSize, z1 = z0 + sh.cellSize;
		const float dx = std::max({b.mn[0] - x1, x0 - b.mx[0], 0.0f}), dz = std::max({b.mn[2] - z1, z0 - b.mx[2], 0.0f});
		// (and not too far above or below the eyes of this floor: the street under a bridge is not near the bridge's deck)
		if (std::sqrt(dx * dx + dz * dz) <= sh.opt.nearAlways && b.mx[1] >= eyeLo - sh.opt.nearHeight && b.mn[1] <= eyeHi + sh.opt.nearHeight) return true;
	}
	const u32 first = (*sh.pointStart)[box], last = (*sh.pointStart)[box + 1];
	if (first == last) return true; // (nothing to aim at: say nothing)
	for (size_t pi = 0; pi < places.size(); ++pi)
		for (u32 k = first; k < last; ++k)
			if (!blockedTo(sh, places[pi], (*sh.points)[k], cache[pi])) return true;
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
			if (!b.indexCount || batchLod(b.detail) == BATCH_LOD_FAR_ONLY) continue;
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
			if (batchLod(b.detail) != BATCH_LOD_FAR_ONLY)
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
	// ---- rooms (fix91) ----
	std::vector<RoomB> roomBoxes;
	for (const auto& rb : scene.roomBoxes) {
		RoomB b {};
		for (int a = 0; a < 3; ++a) b.mn[a] = rb.bbMin[a], b.mx[a] = rb.bbMax[a];
		b.room = u16(rb.room + 1);
		roomBoxes.push_back(b);
	}
	std::vector<u16> boxRoom(boxes.size(), 0);
	u32 roomClusters = 0;
	for (size_t i = 0; i < boxes.size(); ++i) {
		const Box& bx = boxes[i];
		if (bx.empty()) continue;
		float bestVolume = 1e30f;
		for (const RoomB& rb : roomBoxes) {
			bool inside = true;
			for (int a = 0; a < 3 && inside; ++a) inside = bx.mn[a] >= rb.mn[a] - 30.0f && bx.mx[a] <= rb.mx[a] + 30.0f;
			const float v = (rb.mx[0] - rb.mn[0]) * (rb.mx[1] - rb.mn[1] + 1.0f) * (rb.mx[2] - rb.mn[2]);
			if (inside && v < bestVolume) {
				bestVolume = v;
				boxRoom[i] = rb.room;
			}
		}
		roomClusters += boxRoom[i] != 0;
	}
	sh.rooms = &roomBoxes;
	sh.boxRoom = &boxRoom;
	for (u32 ri = 0; ri < roomBoxes.size(); ++ri) {
		const float mx = 0.5f * (roomBoxes[ri].mn[0] + roomBoxes[ri].mx[0]), mz = 0.5f * (roomBoxes[ri].mn[2] + roomBoxes[ri].mx[2]);
		sh.roomsInCell[{int(std::floor(mz / cellSize)), int(std::floor(mx / cellSize))}].push_back(ri);
	}
	stats.roomClusters = roomClusters;

	for (u32 k = 0; k < order.size();) {
		u32 e = k;
		while (e < order.size() && cv[order[e]].cz == cv[order[k]].cz && cv[order[e]].cx == cv[order[k]].cx) ++e;
		sh.cellRange[{cv[order[k]].cz, cv[order[k]].cx}] = {k, e};
		k = e;
	}

	// ---- one row per floor of each cell ----
	struct Row {
		int x, z;
		u16 room = 0;
		float yMin, yMax;      // the camera heights this row is for
		std::vector<u32> bits; // packed
		u32 count = 0;
		u64 hidden = 0, samples = 0;
	};
	std::vector<std::pair<int, int>> cells; // (z, x), ascending
	for (const auto& kv : sh.cellRange) cells.push_back(kv.first);
	std::vector<std::vector<Row>> cellRows(cells.size());
	const int reach = int(std::ceil(opt.radius / cellSize)) + 1;
	std::atomic<size_t> next {0};
	auto worker = [&]() {
		std::vector<Place> places;
		for (;;) {
			const size_t r = next.fetch_add(1);
			if (r >= cells.size()) return;
			std::vector<Row>& out = cellRows[r];
			const int cz = cells[r].first, cx = cells[r].second;
			viewingPlaces(sh, cx, cz, places);
			roomPlaces(sh, cx, cz, places);
			if (places.empty()) {
				Row row;
				row.x = cx;
				row.z = cz;
				row.yMin = -1.0e30f;
				row.yMax = 1.0e30f;
				row.count = 0xFFFFFFFFu; // (marked: no row - no ground in the cell, everything is drawn)
				out.push_back(std::move(row));
				continue;
			}
			// Rows by room (fix91): the places of one room - or of the outside - see by themselves, and a camera in a room
			// is given the row of that room.
			std::vector<u16> roomIds;
			for (const Place& pl : places)
				if (std::find(roomIds.begin(), roomIds.end(), pl.room) == roomIds.end()) roomIds.push_back(pl.room);
			std::sort(roomIds.begin(), roomIds.end());
			for (u16 roomId : roomIds) {
				// The floors of the cell (in this room): heights closer together than levelGap are one (a slope, a stair),
				// and each floor is seen from its own places.
				struct Level {
					float top, bottom;
					std::vector<V3> eyes;
					float eyeLo = 1e30f, eyeHi = -1e30f;
				};
				std::vector<float> heights;
				for (const Place& pl : places)
					if (pl.room == roomId) heights.push_back(pl.floorY);
				std::sort(heights.begin(), heights.end(), [](float a, float b) { return a > b; });
				std::vector<Level> levels;
				for (float f : heights) {
					if (levels.empty() || levels.back().bottom - f > opt.levelGap)
						levels.push_back(Level {f, f, {}, 1e30f, -1e30f});
					else
						levels.back().bottom = f;
				}
				for (const Place& pl : places) {
					if (pl.room != roomId) continue;
					for (Level& lv : levels)
						if (pl.floorY <= lv.top + 0.01f && pl.floorY >= lv.bottom - 0.01f) {
							lv.eyes.push_back(pl.p);
							lv.eyeLo = std::min(lv.eyeLo, pl.p.y);
							lv.eyeHi = std::max(lv.eyeHi, pl.p.y);
							break;
						}
				}
				for (size_t li = levels.size(); li-- > 0;) { // (the lowest floor first: rows are sorted by height)
					const Level& lv = levels[li];
					Row row;
					row.x = cx;
					row.z = cz;
					row.room = roomId;
					// The floor above claims the heights from a metre under its own floor; the ranges overlap by a little
					// more than that, so that a camera at the edge does not change rows with every step.
					row.yMin = li + 1 < levels.size() ? lv.bottom - 250.0f : lv.bottom - 1500.0f;
					row.yMax = li > 0 ? levels[li - 1].bottom + 50.0f : 1.0e30f;
					row.samples = lv.eyes.size();
					std::vector<Occluders> cache(lv.eyes.size());
					std::vector<u8> bitsOut;
					for (int dz = -reach; dz <= reach; ++dz)
						for (int dx = -reach; dx <= reach; ++dx) {
							const float gx = float(std::max(std::abs(dx) - 1, 0)) * cellSize, gz = float(std::max(std::abs(dz) - 1, 0)) * cellSize;
							if (std::sqrt(gx * gx + gz * gz) > opt.radius) continue;
							auto it = sh.cellRange.find({cz + dz, cx + dx});
							if (it == sh.cellRange.end()) continue;
							for (u32 k = it->second.first; k < it->second.second; ++k) {
								const ChunkVis& c = cv[order[k]];
								for (u32 cl = 0; cl < c.clusters; ++cl) {
									const bool vis = lv.eyes.empty() || visibleFrom(sh, lv.eyes, cache, c.firstBox + cl, cx, cz, lv.eyeLo, lv.eyeHi, roomId);
									bitsOut.push_back(vis ? 1 : 0);
									row.hidden += !vis;
								}
							}
						}
					row.count = u32(bitsOut.size());
					row.bits.assign((bitsOut.size() + 31) / 32, 0u);
					for (size_t i = 0; i < bitsOut.size(); ++i)
						if (bitsOut[i]) row.bits[i >> 5] |= 1u << (i & 31);
					out.push_back(std::move(row));
				}
			}
		}
	};
	const unsigned threads = std::max(1u, opt.threads);
	std::vector<std::thread> pool;
	for (unsigned t = 1; t < threads; ++t) pool.emplace_back(worker);
	worker();
	for (std::thread& t : pool) t.join();

	// ---- the table ----
	std::vector<VisViewer3> viewers;
	std::vector<u32> bits;
	u64 totalBits = 0;
	for (const std::vector<Row>& cellList : cellRows) {
		u32 floorsOfCell = 0;
		for (const Row& row : cellList) {
			stats.samples += row.samples;
			if (row.count == 0xFFFFFFFFu || row.count == 0) continue;
			if (totalBits > 0xFFFFFFF0ull - row.count) break; // (more than a table can hold)
			VisViewer3 v {};
			v.x = i16(row.x);
			v.z = i16(row.z);
			v.bitOffset = u32(totalBits);
			v.yMin = row.yMin;
			v.yMax = row.yMax;
			v.room = row.room;
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
			if (row.room == 0) ++floorsOfCell;
			else ++stats.roomRows;
		}
		stats.cells += floorsOfCell > 0;
		stats.multiFloorCells += floorsOfCell > 1;
	}
	stats.viewers = u32(viewers.size());
	stats.targets = totalBits;
	if (viewers.empty()) return {};
	std::sort(viewers.begin(), viewers.end(), [](const VisViewer3& a, const VisViewer3& b) {
		if (a.z != b.z) return a.z < b.z;
		if (a.x != b.x) return a.x < b.x;
		if (a.room != b.room) return a.room < b.room;
		return a.yMin < b.yMin;
	});

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
	put(viewers.data(), viewers.size() * sizeof(VisViewer3));
	put(bits.data(), bits.size() * 4);
	stats.bytes = u32(out.size());
	stats.rays = sh.rays.load();
	stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	return out;
}

} // namespace g3
