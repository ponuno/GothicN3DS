// RGBA image -> G3TX (ETC1 / ETC1A4 in PICA-native layout, with mipmaps).
#pragma once
#include "scene.h"

namespace g3 {

struct TexBakeOptions {
	int maxSize = 128;   // longest edge after downscaling; the top screen is only 400x240
	int quality = 1;     // 0 fast, 1 medium, 2 best (slow)
	bool mipmaps = true;
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
