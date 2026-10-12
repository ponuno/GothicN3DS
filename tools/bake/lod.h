// A simplified stand-in for the world meshes of far chunks (g3ds-bake --lod).
//
// Each chunk of tier 0 gets a second set of batches for its opaque world triangles: the same vertices, fewer
// triangles. Edges are collapsed onto one of their own ends (nothing new is made, so the stand-in costs only
// indices), cheapest first by the quadric error, and never where it would matter: the border of the mesh
// (the chunk's edge, a hole), a change of material, a seam of the texture, a vertex also used by something
// that stays as it is. So the stand-in joins the full-detail chunk next to it without a crack.
// The engine draws a chunk with the stand-in once the whole chunk is farther than StreamConfig::lodDistance.
#pragma once
#include "chunker.h"
#include "g3ds/world.h"

#include <vector>

namespace g3 {

struct LodOptions {
	bool enabled = false;
	float ratio = 0.45f;       // triangles to keep, of what is simplified
	float maxError = 100.0f;   // cm: how far (mean, over the planes around a vertex) a collapse may move the surface
	u32 minTriangles = 48;     // a chunk with fewer simplifiable triangles is left alone
	unsigned threads = 1;
};

struct LodStats {
	u32 chunks = 0;           // chunks that got a stand-in
	u32 candidates = 0;       // chunks looked at
	u32 notTierZero = 0;      // ... of them chunks of medium or small objects
	u32 tooFew = 0;           // ... too few simplifiable triangles (opaque world triangles of the always-drawn class)
	u32 tooLittle = 0;        // ... simplified too little to be worth a second picture
	u64 simplifiable = 0;     // triangles that could be simplified, all chunks
	u64 locked = 0;           // ... of which in chunks that stayed as they are
	u64 trianglesBefore = 0;  // of the simplified batches
	u64 trianglesAfter = 0;
	u64 extraBytes = 0;       // indices and batch entries added
	double seconds = 0;
};

// Adds the stand-ins to `chunks` (batches with BATCH_LOD_FAR_ONLY, the ones they replace marked
// BATCH_LOD_FULL_ONLY). `materials`: the world material table (what is not opaque is left alone).
void buildLods(std::vector<BakedChunk>& chunks, const std::vector<WorldMaterial>& materials, const LodOptions& opt, LodStats& stats);

} // namespace g3
