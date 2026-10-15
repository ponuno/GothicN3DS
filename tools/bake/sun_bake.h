// The sun's shadow on the ground, baked (g3ds-bake --sun-shadow; g3ds/world.h G3SN): for each cell of the chunk grid a
// small texture lies flat over the cell, seen from above, and holds how much of the sun of one hour of the day reaches
// the ground there - past trees, houses, rocks and the hills. The runtime darkens the ground under it by day in clear
// weather, the less the farther the sky's sun is from the one baked.
//
// How a texel is found: a ray straight down finds the highest surface there that takes a shadow (the ground, a floor, a
// roof: facing up, not a plant, not cut-out leaves); from it rays go towards the sun (a few, spread over the texel and
// the sun's disc, for a soft edge) and whatever they meet first takes the light, the leaves half of it. A surface turned
// away from the sun is in its own shade. Texels with no surface under them take their neighbours' value.
#pragma once
#include "scene.h"
#include "vertex_fx.h"

#include <string>
#include <vector>

namespace g3 {

struct SunOptions {
	bool enabled = false;
	float hour = 14.5f;   // the sky's sun at this hour (g3ds/world.h sunDirectionAt): afternoon, shadows of some length
	float texel = 32.0f;  // cm: the finest texel (a cell of 40 m: 128 texels)
	u32 maxSize = 256;    // texels at most along a side
	int samples = 6;      // rays towards the sun per texel
	float softness = 1.2f; // degrees: how wide the sun's disc is taken (the softness of the shadows' edges)
	float reach = 12000.0f; // cm: how far towards the sun something still shades
	float leaves = 0.75f;   // how much of the sun cut-out leaves take, 0..1 (fix146: was 0.5; --sun-leaves)
	unsigned threads = 1;
};

// One mask: the cell's place and its texture, ready for the archive.
struct SunMask {
	int gridX = 0, gridZ = 0;
	float x0 = 0, z0 = 0, x1 = 0, z1 = 0; // the area it covers (cm)
	u32 width = 0, height = 0;
	std::vector<u8> texture; // TexHeader + TEX_L4 data
	float lit = 0;           // share of its texels in full sun
};

struct SunStats {
	u32 cells = 0;
	u64 texels = 0, shaded = 0, empty = 0;
	u64 rays = 0;
	u64 bytes = 0;
	double seconds = 0;
};

// What a material is to the sun: `kind[m]` 0 an ordinary surface (casts and takes the shadow), 1 casts half (cut-out
// leaves, plants) and takes none, 2 neither (glass, water, glow: what is see-through).
// `cells`: the x/z extent of each grid cell's chunks (gridX, gridZ, x0, z0, x1, z1 in a SunMask, texture empty).
std::vector<SunMask> bakeSunMasks(const SrcScene& scene, const std::vector<u8>& kind, const std::vector<SunMask>& cells,
                                  const SunOptions& opt, SunStats& stats);

// TEX_L4 texture of w x h values 0..15 (row-major, row y at v = (y + 0.5) / h), clamped at the edges, one level.
std::vector<u8> encodeL4(const std::vector<u8>& values, u32 w, u32 h);

} // namespace g3
