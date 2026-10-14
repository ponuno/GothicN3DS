#include "vcache.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace g3 {

namespace {

constexpr float kCacheDecay = 1.5f;
constexpr float kLastTriScore = 0.75f;
constexpr float kValenceBoostScale = 2.0f;
constexpr float kValenceBoostPower = 0.5f;
constexpr int kMaxCache = 32;

float vertexScore(int cachePos, int liveTriangles, int cacheSize) {
	if (liveTriangles <= 0) return -1.0f;
	float score = 0.0f;
	if (cachePos >= 0) {
		if (cachePos < 3)
			score = kLastTriScore; // the triangle just drawn: its vertices count the same whatever their order
		else {
			const float scaler = 1.0f / float(cacheSize - 3);
			score = std::pow(1.0f - float(cachePos - 3) * scaler, kCacheDecay);
		}
	}
	return score + kValenceBoostScale * std::pow(float(liveTriangles), -kValenceBoostPower);
}

// One batch: `tris` triangles at `idx` (three u16 each), reordered in place.
void reorder(u16* idx, u32 tris, std::vector<u32>& localOf, std::vector<u32>& stampOf, u32& stamp, unsigned cacheSize) {
	if (tris < 3) return;
	++stamp;
	// Compact vertex numbers for this batch.
	std::vector<u16> verts;
	std::vector<u32> local(tris * 3);
	for (u32 i = 0; i < tris * 3; ++i) {
		const u16 v = idx[i];
		if (stampOf[v] != stamp) {
			stampOf[v] = stamp;
			localOf[v] = u32(verts.size());
			verts.push_back(v);
		}
		local[i] = localOf[v];
	}
	const u32 nv = u32(verts.size());
	std::vector<int> live(nv, 0), cachePos(nv, -1);
	std::vector<u32> start(nv + 1, 0);
	for (u32 i = 0; i < tris * 3; ++i) ++start[local[i] + 1];
	for (u32 v = 0; v < nv; ++v) {
		live[v] = int(start[v + 1]);
		start[v + 1] += start[v];
	}
	std::vector<u32> fill(start.begin(), start.end() - 1), list(tris * 3);
	for (u32 t = 0; t < tris; ++t)
		for (int k = 0; k < 3; ++k) list[fill[local[t * 3 + k]]++] = t;
	std::vector<float> vscore(nv), tscore(tris);
	const int cs = int(std::min<unsigned>(cacheSize, kMaxCache));
	for (u32 v = 0; v < nv; ++v) vscore[v] = vertexScore(-1, live[v], cs);
	for (u32 t = 0; t < tris; ++t) tscore[t] = vscore[local[t * 3]] + vscore[local[t * 3 + 1]] + vscore[local[t * 3 + 2]];
	std::vector<char> done(tris, 0);
	std::vector<u16> out;
	out.reserve(tris * 3);
	std::vector<u32> cache; // most recently used first
	u32 scan = 0;
	u32 best = 0;
	bool haveBest = false;
	for (u32 emitted = 0; emitted < tris; ++emitted) {
		if (!haveBest) {
			while (done[scan]) ++scan;
			best = scan;
		}
		done[best] = 1;
		// The triangle's vertices go to the front of the cache.
		std::vector<u32> touched;
		for (int k = 0; k < 3; ++k) {
			const u32 v = local[best * 3 + k];
			out.push_back(verts[v]);
			--live[v];
		}
		for (int k = 2; k >= 0; --k) {
			const u32 v = local[best * 3 + k];
			const auto it = std::find(cache.begin(), cache.end(), v);
			if (it != cache.end()) cache.erase(it);
			cache.insert(cache.begin(), v);
		}
		// New positions and scores for what is (or was) in the cache.
		for (u32 v : cache) touched.push_back(v);
		for (size_t p = cs; p < cache.size(); ++p) cachePos[cache[p]] = -1;
		if (cache.size() > size_t(cs)) cache.resize(size_t(cs));
		float bestScore = -1.0f;
		haveBest = false;
		for (size_t p = 0; p < cache.size(); ++p) cachePos[cache[p]] = int(p);
		for (u32 v : touched) {
			vscore[v] = vertexScore(cachePos[v], live[v], cs);
		}
		for (u32 v : touched) {
			for (u32 e = start[v]; e < start[v + 1]; ++e) {
				const u32 t = list[e];
				if (done[t]) continue;
				tscore[t] = vscore[local[t * 3]] + vscore[local[t * 3 + 1]] + vscore[local[t * 3 + 2]];
			}
		}
		for (u32 v : touched) {
			for (u32 e = start[v]; e < start[v + 1]; ++e) {
				const u32 t = list[e];
				if (done[t]) continue;
				if (tscore[t] > bestScore) {
					bestScore = tscore[t];
					best = t;
					haveBest = true;
				}
			}
		}
	}
	std::copy(out.begin(), out.end(), idx);
}

} // namespace

u64 countCacheMisses(const u16* indices, size_t count, unsigned cacheSize) {
	std::vector<u16> fifo;
	u64 misses = 0;
	size_t head = 0;
	fifo.assign(cacheSize, 0xFFFFu);
	for (size_t i = 0; i < count; ++i) {
		bool hit = false;
		for (u16 c : fifo) hit = hit || c == indices[i];
		if (hit) continue;
		++misses;
		fifo[head] = indices[i];
		head = (head + 1) % cacheSize;
	}
	return misses;
}

void optimizeIndexRange(u16* indices, u32 count, size_t vertexCount, unsigned cacheSize) {
	count -= count % 3;
	if (count < 9) return;
	std::vector<u32> localOf(vertexCount + 1, 0), stampOf(vertexCount + 1, 0);
	u32 stamp = 0;
	reorder(indices, count / 3, localOf, stampOf, stamp, cacheSize);
}

void optimizeVertexCache(BakedChunk& chunk, VertexCacheStats* stats, unsigned cacheSize) {
	std::vector<u32> localOf(chunk.vertices.size() + 1, 0), stampOf(chunk.vertices.size() + 1, 0);
	u32 stamp = 0;
	for (const ChunkBatch& b : chunk.batches) {
		const u32 first = b.firstIndex, count = b.indexCount - b.indexCount % 3;
		if (count < 9 || size_t(first) + count > chunk.indices.size()) continue;
		u16* idx = chunk.indices.data() + first;
		if (stats) {
			stats->triangles += count / 3;
			stats->missesBefore += double(countCacheMisses(idx, count, cacheSize));
		}
		reorder(idx, count / 3, localOf, stampOf, stamp, cacheSize);
		if (stats) stats->missesAfter += double(countCacheMisses(idx, count, cacheSize));
	}
}

} // namespace g3
