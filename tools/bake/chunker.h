// Cuts a triangle soup into grid chunks with quantised, de-duplicated
// vertices and one index range (batch) per material and cluster.
#pragma once
#include "g3ds/world.h"
#include "scene.h"

#include <vector>

namespace g3 {

struct BakedChunk {
	int gridX = 0, gridZ = 0, part = 0;
	// The lowest DetailClass the chunk holds (ChunkerOptions::splitDetail; 0 otherwise).
	u8 tier = 0;
	ChunkHeader header {};
	std::vector<ChunkBatch> batches;
	std::vector<WorldVertex> vertices;
	std::vector<u16> indices;
	float bbMin[3] {}, bbMax[3] {};

	std::vector<u8> serialize() const;
};

struct ChunkerOptions {
	float chunkSize = 4000.0f; // cm
	// Finest position step, cm (best a power of two: world positions are then
	// exact in a float). 1/8 cm covers 82 m around the middle of a chunk; a
	// chunk reaching farther keeps its ordinary triangles on the fine lattice
	// and moves the giant ones into a part of their own with a coarser step.
	float positionStep = 0.125f;
	u32 maxVertices = CHUNK_MAX_VERTICES;
	// Seam repair. Rounding positions to a lattice opens gaps where the source
	// only touched: a vertex resting on the edge of a neighbouring triangle
	// (a "T-junction") moves off that edge, and two vertices a hair apart can
	// round to different lattice points. The source has such gaps of its own,
	// too small to see from a third-person camera and plain from 30 cm.
	// With `seal`, vertices closer than half the tolerance become one, and an
	// edge is split at every vertex lying within the tolerance of it, so both
	// sides of the seam use the same lattice points.
	bool seal = true;
	float sealTolerance = 0.5f; // cm
	// A chunk's triangles are sorted into boxes of this edge (by centroid), and
	// every material gets a batch per box: the runtime draws the boxes that are
	// in view instead of the whole chunk. 0: one batch per material, as before.
	float clusterSize = 1000.0f; // cm
	// The vertices' w carries SrcVertex::occlusion and ::wind (WORLDF_AO,
	// WORLDF_WIND); otherwise it is 1, as it always was.
	bool vertexFx = false;
	// Cuts every cell by detail class: the world and the big things (DETAIL_ALWAYS) go into
	// chunks of their own, the medium objects into others, the small ones into others still
	// (WORLDF_TIERS). The runtime keeps the first everywhere around the camera, and the others
	// only near it: most of a chunk's memory is objects that are drawn only close up.
	bool splitDetail = true;
	// A tier with fewer triangles than this in a cell is not worth a chunk (and the bookkeeping that
	// comes with one): small objects go in with the medium ones, those with the world.
	u32 minTierTriangles = 400;
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
	u64 welded = 0;        // distinct source positions merged into a neighbour closer than half the tolerance
	u64 tJunctions = 0;    // vertices found on the edge of another triangle (per edge)
	u64 sealTriangles = 0; // triangles added by splitting those edges
	u32 stepChunks[8] = {0, 0, 0, 0, 0, 0, 0, 0}; // chunks per position step: base, x2, x4, ... (last: all coarser)
	u32 splitChunks = 0;   // grid cells that needed more than one part
	u32 maxChunkTriangles = 0;
	u32 maxChunkVertices = 0;
	u32 maxChunkBatches = 0;
	u64 batches = 0;
	u64 clusters = 0;      // boxes the chunks were cut into (a chunk has 255 at most)
	u32 maxChunkClusters = 0;
	u32 coarsenedChunks = 0; // chunks whose boxes had to be bigger than asked to stay within 255
	u64 detailTriangles[DETAIL_COUNT] = {0, 0, 0};
	// Chunks, triangles and vertices by tier (splitDetail; all in tier 0 without it).
	u32 tierChunks[DETAIL_COUNT] = {0, 0, 0};
	u64 tierTriangles[DETAIL_COUNT] = {0, 0, 0};
	u64 tierVertices[DETAIL_COUNT] = {0, 0, 0};
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
