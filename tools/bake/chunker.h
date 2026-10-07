// Cuts a triangle soup into grid chunks with quantised, de-duplicated
// vertices and one index range (batch) per material.
#pragma once
#include "g3ds/world.h"
#include "scene.h"

#include <vector>

namespace g3 {

struct BakedChunk {
	int gridX = 0, gridZ = 0, part = 0;
	ChunkHeader header {};
	std::vector<ChunkBatch> batches;
	std::vector<WorldVertex> vertices;
	std::vector<u16> indices;
	float bbMin[3] {}, bbMax[3] {};

	std::vector<u8> serialize() const;
};

struct ChunkerOptions {
	float chunkSize = 4000.0f; // cm
	// Finest position step, cm. 0.5 cm covers a chunk extent of 163 m; chunks
	// reaching farther (a giant triangle) double it until they fit.
	float positionStep = 0.5f;
	u32 maxVertices = CHUNK_MAX_VERTICES;
};

struct ChunkerStats {
	u64 inputTriangles = 0;
	u64 outputTriangles = 0;
	u64 degenerate = 0;    // collapsed by quantisation or zero area
	u64 uvRebased = 0;     // triangles whose UVs needed a per-triangle shift
	u64 uvClamped = 0;     // triangles whose UV span does not fit at all
	u64 vertices = 0;
	u64 uniquePositions = 0; // per chunk: distinct quantised positions ...
	u64 uniquePosUv = 0;     // ... and distinct position + UV pairs (diagnostics)
	u32 splitChunks = 0;   // grid cells that needed more than one part
	u32 maxChunkTriangles = 0;
	u32 maxChunkVertices = 0;
	u32 maxChunkBatches = 0;
	u64 batches = 0;
	u64 detailTriangles[DETAIL_COUNT] = {0, 0, 0};
	float maxPosError = 0; // cm, worst-case quantisation step / 2
};

// `materialRemap[srcMaterial]` gives the world material index, or -1 to drop.
// `materialOrder[worldMaterial]` is a draw-order key: batches of a chunk are
// sorted by it first (opaque, then cut-out, then blended), so the runtime
// changes render state as rarely as possible.
std::vector<BakedChunk> buildChunks(const SrcScene& scene, const std::vector<int>& materialRemap,
                                    const std::vector<u8>& materialOrder, const ChunkerOptions& opt,
                                    ChunkerStats& stats);

} // namespace g3
