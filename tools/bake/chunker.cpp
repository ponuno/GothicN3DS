#include "chunker.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>

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

struct CellBuilder {
	const SrcScene& scene;
	const std::vector<int>& remap;
	const ChunkerOptions& opt;
	ChunkerStats& stats;
	std::vector<BakedChunk>& out;

	void build(int gx, int gz, std::vector<u32>& tris) {
		// One quantisation frame for the whole cell, so parts of a split cell
		// share exactly the same vertex positions.
		float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
		for (u32 t : tris)
			for (const SrcVertex& v : scene.triangles[t].v)
				for (int a = 0; a < 3; ++a) {
					mn[a] = std::min(mn[a], v.pos[a]);
					mx[a] = std::max(mx[a], v.pos[a]);
				}
		const float extent = std::max({mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2], 1.0f});
		const float posScale = extent / 32767.0f;
		stats.maxPosError = std::max(stats.maxPosError, posScale * 0.5f);

		// Batches are contiguous per material.
		std::stable_sort(tris.begin(), tris.end(), [&](u32 a, u32 b) {
			return remap[scene.triangles[a].material] < remap[scene.triangles[b].material];
		});

		// A shared UV origin per material keeps vertices shareable between triangles.
		std::map<int, std::pair<float, float>> uvBase;
		for (u32 t : tris) {
			const SrcTriangle& tri = scene.triangles[t];
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
		int part = 0;
		u32 cellParts = 0;

		auto begin = [&]() {
			cur = BakedChunk {};
			cur.gridX = gx;
			cur.gridZ = gz;
			cur.part = part++;
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
			if (cur.indices.empty()) return;
			cur.header.batchCount = u16(cur.batches.size());
			cur.header.vertexCount = u16(cur.vertices.size());
			cur.header.indexCount = u32(cur.indices.size());
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
			stats.vertices += cur.vertices.size();
			stats.maxChunkTriangles = std::max(stats.maxChunkTriangles, u32(cur.indices.size() / 3));
			stats.maxChunkVertices = std::max(stats.maxChunkVertices, u32(cur.vertices.size()));
			stats.maxChunkBatches = std::max(stats.maxChunkBatches, u32(cur.batches.size()));
			out.push_back(std::move(cur));
			++cellParts;
		};

		begin();
		for (u32 t : tris) {
			const SrcTriangle& tri = scene.triangles[t];
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
				auto quant = [&](int a) { return i16(std::lround((v.pos[a] - mn[a]) / posScale)); };
				auto quantUv = [&](int a) {
					float s = (v.uv[a] - base[a]) / WORLD_UV_SCALE;
					return i32(std::lround(std::clamp(s, -1.0e6f, 1.0e6f)));
				};
				q[k].x = quant(0);
				q[k].y = quant(1);
				q[k].z = quant(2);
				q[k].w = 1;
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

			if (cur.batches.empty() || cur.batches.back().material != u16(material)) {
				ChunkBatch b {};
				b.material = u16(material);
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
		if (cellParts > 1) ++stats.splitChunks;
	}
};

} // namespace

std::vector<BakedChunk> buildChunks(const SrcScene& scene, const std::vector<int>& materialRemap,
                                    const ChunkerOptions& opt, ChunkerStats& stats) {
	// Assign every triangle to the grid cell of its centroid (XZ plane).
	std::map<std::pair<int, int>, std::vector<u32>> cells;
	for (u32 t = 0; t < scene.triangles.size(); ++t) {
		const SrcTriangle& tri = scene.triangles[t];
		if (materialRemap[tri.material] < 0) continue;
		++stats.inputTriangles;
		const float cx = (tri.v[0].pos[0] + tri.v[1].pos[0] + tri.v[2].pos[0]) / 3.0f;
		const float cz = (tri.v[0].pos[2] + tri.v[1].pos[2] + tri.v[2].pos[2]) / 3.0f;
		cells[{int(std::floor(cx / opt.chunkSize)), int(std::floor(cz / opt.chunkSize))}].push_back(t);
	}

	std::vector<BakedChunk> out;
	CellBuilder builder {scene, materialRemap, opt, stats, out};
	for (auto& kv : cells) builder.build(kv.first.first, kv.first.second, kv.second);
	return out;
}

} // namespace g3
