// Baked visibility (g3ds-bake --vis): for every cell of the chunk grid, which clusters of the chunks
// around it can be seen from it, and which are hidden behind something solid. See VisHeader in
// g3ds/world.h for the table and how the runtime uses it.
//
// From a few places in the cell, at the eye heights a character has there (on every floor a ray down
// from the sky finds, on foot and at the height of the camera behind it), rays go to the corners and
// the middle of each cluster's box. A cluster is hidden if every one of them meets a wall, a roof, the
// ground on the way - and is not near (nothing within `near` is ever hidden). Only what is opaque and
// belongs to the world itself blocks: glass, water, leaves, fences and small objects do not.
#pragma once
#include "chunker.h"
#include "vertex_fx.h"

#include <vector>

namespace g3 {

struct VisOptions {
	bool enabled = false;
	float radius = 15000.0f; // cm: how far from a cell the table goes (farther: everything is drawn)
	float nearAlways = 2500.0f; // cm: what is this near a viewing place is visible, whatever the rays say
	bool high = false;       // 3x3 places per cell and the middles of the faces too, not 2x2 and the corners
	unsigned threads = 1;
};

struct VisStats {
	u32 viewers = 0;     // cells with a row
	u64 targets = 0;     // bits in all (clusters looked at, over all cells)
	u64 hidden = 0;      // of them hidden
	u64 rays = 0;
	u64 samples = 0;     // viewing places over all cells
	u32 bytes = 0;       // the table
	double seconds = 0;
	float radius = 0;
};

// The table (VisHeader ...), empty if there is nothing to say.
std::vector<u8> buildVisibility(const SrcScene& scene, const std::vector<AoMaterial>& materials,
                                const std::vector<BakedChunk>& chunks, float cellSize, const VisOptions& opt, VisStats& stats);

} // namespace g3
