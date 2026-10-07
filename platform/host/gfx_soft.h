// Host-only extras of the software backend.
#pragma once
#include "gfx.h"

namespace g3 {
namespace gfx {

struct SoftStats {
	u32 trianglesSubmitted;
	u32 trianglesClipped;   // fully outside the frustum
	u32 trianglesBackface;  // wound clockwise on screen
	u32 trianglesRasterised;
	u32 pixelsTested;       // fragments that reached the depth test
	u32 pixelsWritten;      // fragments that passed it (overdraw = written / screen)
	u32 skinnedTriangles;   // of characters ...
	u32 skinnedBackface;    // ... turned away from the camera ...
	u32 skinnedPixels;      // ... and the fragments of theirs that passed the depth test
};

// RGBA8, SCREEN_W x SCREEN_H, row 0 at the top.
const u8* softFramebuffer();
SoftStats softStats();
// Debug aid for checking the winding of freshly imported data.
void softSetCulling(bool enabled);
// Test aid: mesh and texture creation fails once the total would exceed
// `bytes` (0 = no limit), like a full linear heap on the console.
void softSetMemoryLimit(u32 bytes);

} // namespace gfx
} // namespace g3
