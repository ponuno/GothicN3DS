// A water texture made here instead of the game's (--water-texture): a seamless pattern of soft caustic light on a
// blue-green body, in the colour asked for (--water-color), half see-through (its alpha is the water's look).
#pragma once
#include "scene.h"

namespace g3 {

// `size` x size texels (a power of two); tiles without a seam.
// `alpha`: the water's opacity at its clearest, 0..255 (the caustics add up to 45 on top).
SrcImage makeWaterTexture(const unsigned char color[3], int size = 128, int alpha = 110);

} // namespace g3
