// The far layer (G10, g3ds/world.h G3FR): the world simplified by vertex
// clustering on a grid - every corner in a box of the grid becomes one point,
// put where it best fits the planes of the triangles around it (quadric error,
// as Lindstrom's "out-of-core simplification") - coloured with the textures'
// average colour times the baked light.
#pragma once
#include "chunker.h"
#include "scene.h"

#include <array>
#include <string>
#include <vector>

namespace g3 {

struct FarOptions {
	bool enabled = false;
	float cell = 300.0f;        // cm: the finest grid tried
	u32 maxTriangles = 80000;   // the grid grows coarser until the layer fits (memory on the 3DS: ~14 bytes a triangle)
	float tileSize = 16000.0f;  // cm: the edge of a tile (rounded to whole chunks)
};

struct FarStats {
	u64 inputTriangles = 0; // of the world mesh and the big objects, opaque or water
	u32 triangles = 0, vertices = 0, tiles = 0, ranges = 0;
	u32 skirtTriangles = 0; // of the triangles: the strips along the seams between cells (half of them drawn at a time)
	u32 seamEdges = 0;
	u32 crowns = 0;         // solids standing in for the cut-outs (leaves) of a box
	float cell = 0;   // cm, the grid of the seams between cells
	float cellFinest = 0, cellCoarsest = 0; // cm, the grids of the cells (the dense ones finer)
	u32 passes = 0;   // grids tried
	u32 bytes = 0;    // blob size
};

// `include[m]`, `color[m]` (rgb) and `shrink[m]` per source material:
// whether its triangles go into the layer, the colour its texture averages to,
// and for a cut-out (leaves) how much of its texture is solid, square-rooted
// (below 1: it is drawn as part of a crown, a small solid; 1: an ordinary
// solid material).
// `chunks` are the world's chunks as baked (their grid cells name what each
// range stands in for). Returns the G3FR blob, empty if nothing went in.
std::vector<u8> buildFarLayer(const SrcScene& scene, const std::vector<bool>& include, const std::vector<std::array<u8, 3>>& color,
                              const std::vector<float>& shrink, const std::vector<BakedChunk>& chunks, float chunkSize, const FarOptions& opt, FarStats& stats);

} // namespace g3
