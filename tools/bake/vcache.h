// Triangle order for the GPU's vertex cache (Tom Forsyth's "Linear-Speed Vertex Cache Optimisation").
//
// A chunk's batches draw their triangles in the order the baker met them, mostly object by object, which is good for
// whole objects and bad for big meshes: a vertex is transformed again every time its triangle comes after the cache
// has forgotten it. Reordering the triangles of a batch (never the triangles' own corners: the winding stays, and a
// batch keeps its range of indices) makes neighbours follow each other, so the vertex shader runs fewer times
// for the same picture. The vertices themselves stay where they are.
#pragma once

#include "chunker.h"

namespace g3 {

struct VertexCacheStats {
	u64 triangles = 0;
	double missesBefore = 0, missesAfter = 0; // vertex shader runs in a FIFO cache of `cacheSize`, summed over the batches
};

// Reorders the triangles of every batch of `chunk` (the same triangles, the same winding, the same index range).
void optimizeVertexCache(BakedChunk& chunk, VertexCacheStats* stats = nullptr, unsigned cacheSize = 16);

// Vertex shader runs for `count` indices in a FIFO cache of `cacheSize` entries.
u64 countCacheMisses(const u16* indices, size_t count, unsigned cacheSize);

} // namespace g3
