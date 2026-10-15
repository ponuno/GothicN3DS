// A water texture made here instead of the game's (--water-texture): a seamless wind-rippled surface (fix151; was caustics) on a
// dark blue-green body, in the colour asked for (--water-color), mostly opaque (its alpha is the water's look).
#pragma once
#include "scene.h"

namespace g3 {

// `size` x size texels (a power of two); tiles without a seam.
// `alpha`: the water's opacity at its clearest, 0..255 (the mirrored crests add up to 55 on top, the glints 60).
SrcImage makeWaterTexture(const unsigned char color[3], int size = 128, int alpha = 110);

} // namespace g3
