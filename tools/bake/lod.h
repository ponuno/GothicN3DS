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
	float ratio = 0.6f;        // triangles to keep, of what is simplified
	float maxError = 30.0f;    // cm: how far (mean, over the planes around a vertex) a collapse may move the surface
	u32 minTriangles = 48;     // a chunk with fewer simplifiable triangles is left alone
	// Chunks of medium and small objects (tiers 1 and 2) get stand-ins too (fix88): crates, mushrooms, bushes and the
	// like are most of the triangles within the draw distance, and a 750-triangle crate 25 m away needs a few dozen.
	// The stand-in keeps the batch's class, layer and size band, so it is drawn exactly as far as the full batch.
	bool objects = true;
	float ratioObjects = 0.4f;
	float maxErrorMedium = 10.0f; // cm (the objects are small: the error is held to a fraction of their size)
	float maxErrorSmall = 5.0f;
	u32 minObjectTriangles = 200;
	unsigned threads = 1;
};

struct LodStats {
	u32 chunks = 0;           // chunks that got a stand-in
	u32 candidates = 0;       // chunks looked at
	u32 notTierZero = 0;      // ... of them chunks of medium or small objects (left alone with objects = false)
	u32 objectChunks = 0;     // ... chunks of medium or small objects that got a stand-in
	u64 objectBytes = 0;
	u32 tooFew = 0;           // ... too few simplifiable triangles (opaque world triangles of the always-drawn class)
	u32 tooLittle = 0;        // ... simplified too little to be worth a second picture
	u64 simplifiable = 0;     // triangles that could be simplified, all chunks
	u64 locked = 0;           // ... of which in chunks that stayed as they are
	u64 trianglesBefore = 0;  // of the simplified batches
	u64 trianglesAfter = 0;
	u64 extraBytes = 0;       // indices and batch entries added
	double seconds = 0;
};

// g3ds-bake --simplify: the geometry itself with fewer triangles, in place - the world's meshes of every
// detail class, the level, the large objects, the medium and the small ones, in every tier (what is cut out
// of a texture, blended, swaying or water is left as it is). The same edge collapse as the stand-ins, with
// a much smaller error allowed, since this is what is drawn close up: a surface is only moved by a few
// centimetres, a place the objects of two detail classes share stays, and so does the border of every mesh.
// Done after the visibility is worked out (from the unsimplified scene) and before the stand-ins, which are
// then made from the simplified chunks.
struct SimplifyOptions {
	bool enabled = false;
	float ratio = 0.6f;       // triangles to keep, of what is simplified
	float maxError = 3.0f;    // cm: how far (mean, over the planes around a vertex) a collapse may move the surface
	// The large objects (the detail class that is always drawn) get a second, harder pass: ratioLarge of their triangles
	// kept (0: no second pass, as before), a collapse allowed to move the surface up to maxErrorLarge cm.
	float ratioLarge = 0.0f;
	float maxErrorLarge = 8.0f;
	u32 minTriangles = 24;    // a chunk with fewer simplifiable triangles is left alone
	unsigned threads = 1;
};

struct SimplifyStats {
	u32 chunks = 0;            // chunks that got simpler
	u32 candidates = 0;        // chunks looked at
	u32 tooFew = 0;            // ... too few simplifiable triangles
	u32 tooLittle = 0;         // ... simplified by less than 3%
	u64 trianglesBefore = 0;   // of the simplified chunks' simplifiable batches
	u64 trianglesAfter = 0;
	u64 verticesBefore = 0;    // all chunks
	u64 verticesAfter = 0;
	double seconds = 0;
};

void buildSimplified(std::vector<BakedChunk>& chunks, const std::vector<WorldMaterial>& materials, const SimplifyOptions& opt,
                     SimplifyStats& stats);

// Adds the stand-ins to `chunks` (batches with BATCH_LOD_FAR_ONLY, the ones they replace marked
// BATCH_LOD_FULL_ONLY). `materials`: the world material table (what is not opaque is left alone).
void buildLods(std::vector<BakedChunk>& chunks, const std::vector<WorldMaterial>& materials, const LodOptions& opt, LodStats& stats);

} // namespace g3
