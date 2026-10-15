// RGBA image -> G3TX (ETC1 / ETC1A4 in PICA-native layout, with mipmaps).
#pragma once
#include "scene.h"

#include <vector>

namespace g3 {

struct TexBakeOptions {
	int maxSize = 128;   // longest edge after downscaling; the top screen is only 400x240
	int quality = 1;     // 0 fast, 1 medium, 2 best (slow)
	bool mipmaps = true;
	unsigned threads = 1; // worker threads for the blocks of one image (callers that already run in parallel leave 1)
	// A cut-out's smaller mip levels keep the share of texels that pass the alpha test (their alpha scaled up): thin
	// blades of grass would otherwise wear away from mip level to mip level and the grass thin out with the distance.
	bool keepCoverage = false;
};

struct TexBakeInfo {
	int width = 0, height = 0;
	int srcWidth = 0, srcHeight = 0;
	bool alpha = false;
	u32 mipCount = 0;
};

// Thread-safe. Returns TexHeader + mip data.
std::vector<u8> bakeTexture(const SrcImage& src, const TexBakeOptions& opt, TexBakeInfo* info = nullptr);

} // namespace g3
