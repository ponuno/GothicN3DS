// A water texture made here instead of the game's (--water-texture): a seamless pattern of soft caustic light on a
// blue-green body, in the colour asked for (--water-color), half see-through (its alpha is the water's look).
#pragma once
#include "scene.h"

namespace g3 {

// `size` x size texels (a power of two); tiles without a seam.
SrcImage makeWaterTexture(const unsigned char color[3], int size = 128);

} // namespace g3
