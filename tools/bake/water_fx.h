// Water that looks like water (fix140), made on the scene before it is chunked:
//  - shore foam (--water-foam): a strip of soft white lace along every shore line, drawn over the water's edge and a little
//    of the bank, so that the water meets the ground in a fringe instead of a jagged line;
//  - river flow (--river-flow): water in narrow channels is moved along the channel, downhill, by its own copies of the
//    water material whose texture scrolls that way (the game's water scrolls one way everywhere: a river looks like a pond).
// Both work on the source scene (triangles and materials), so the chunker, the far layer and the rest see ordinary
// geometry and materials; nothing of it is known to the runtime.
#pragma once
#include "scene.h"
#include <string>

namespace g3 {

struct WaterFxOptions {
	float foam = 0.0f;        // 0..1: how strong the shore foam is (0: none)
	float foamWidth = 110.0f; // cm: how far the foam reaches into the water
	float foamBank = 35.0f;   // cm: and onto the bank (the ground over the strip hides what lies above the water)
	float cell = 100.0f;      // cm: the lattice the shore line is found on
	float flow = 0.0f;        // cm/s: the speed of the water in rivers (0: no flow)
	float glint = 0.0f;       // 0..1: sun glints, a second additive layer of sparkles over the water (0: none)
	float riverMaxWidth = 1400.0f; // cm: a channel narrower than this, and long, is a river
};

struct WaterFxStats {
	u32 shoreTiles = 0;     // tiles where the ground meets the water
	u32 shoreChains = 0;    // shore lines found (a chain of segments)
	u64 shoreLength = 0;    // cm
	u64 foamTriangles = 0;
	u64 glintTriangles = 0;
	u32 riverTiles = 0;     // tiles taken for a river
	u32 flowMaterials = 0;  // copies of the water material made
	u64 flowTriangles = 0;  // triangles moved to those
	u32 flowTiles[8] = {0, 0, 0, 0, 0, 0, 0, 0}; // river tiles by the way the water runs: 0 = +x, then 45 degrees on, in the x-z plane
};

// Texture name of the foam (the baker makes the picture itself: makeFoamTexture).
extern const char* const kFoamTexture;
extern const char* const kGlintTexture;
// The glints' picture: sparse bright points on black, tiles both ways (additive, so black is nothing).
SrcImage makeGlintTexture(int w = 128, int h = 128);
// The foam's picture: `w` x `h` texels (powers of two), tiles along u; v = 0 is the bank, 1 the open water. White lace on a
// clear background that fades out at both edges of the strip.
SrcImage makeFoamTexture(int w = 128, int h = 64, int frame = 0, int frames = 1);
// The foam is animated: kFoamFrames pictures, G3_FOAM_A0.TGA .. G3_FOAM_A7.TGA (the lace moves, the front goes out and back).
constexpr int kFoamFrames = 8;
// Makes the picture of a foam frame by its name (any case, with or without a path); false for any other name.
bool makeFoamFrame(const std::string& name, SrcImage& img);

// Adds the foam strips (a material of its own, drawn after the water) and the flow copies of the water materials.
// Outdoor scenes only (the caller checks). Appends to scene.materials and scene.triangles; the ground the foam and the
// flow look at is the scene as it is given.
void bakeWaterFx(SrcScene& scene, const WaterFxOptions& opt, WaterFxStats& stats);

} // namespace g3
