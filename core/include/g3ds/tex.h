// G3TX: textures stored exactly the way the PICA200 samples them, so the
// 3DS side only has to memcpy each mip level into a C3D_Tex.
//
// PICA layout for ETC1 / ETC1A4 (see docs/FORMATS.md):
//  * rows are stored bottom-up (stored row = height - 1 - image row);
//  * the image is cut into 8x8 tiles in raster order;
//  * each tile holds four 4x4 ETC1 blocks in Z order (TL, TR, BL, BR);
//  * a block is the standard big-endian ETC1 block with its 8 bytes reversed;
//  * ETC1A4 puts 8 bytes of 4-bit alpha (column-major nibbles) before it.
#pragma once
#include "g3ds/base.h"

namespace g3 {

constexpr u32 TEX_MAGIC = fourcc('G', '3', 'T', 'X');
constexpr u32 TEX_MIN_SIZE = 8;    // PICA minimum
constexpr u32 TEX_MAX_SIZE = 1024; // PICA maximum

enum TexFormat : u8 {
	TEX_ETC1 = 0,   // 4 bpp, opaque
	TEX_ETC1A4 = 1, // 8 bpp, 4-bit alpha
	// 4 bpp, one channel (the sun's mask of a world, g3ds-bake --sun-shadow): 8x8 tiles as above, the texels of a tile in
	// Morton order (x bit, y bit, x bit ...: as the PICA200 reads them), two to a byte, the first in the low nibble.
	TEX_L4 = 2,
};

enum TexFlags : u16 {
	TEXF_ALPHA_TEST = 1 << 0, // has cut-out alpha: draw with alpha test
	TEXF_CLAMP = 1 << 1,      // not repeated: the edge texels go on beyond it
};

struct TexHeader {
	u32 magic;
	u16 width;  // power of two, 8..1024
	u16 height; // power of two, 8..1024
	u8 format;  // TexFormat
	u8 mipCount;
	u16 flags;
	u32 dataSize; // bytes of mip data following the header
};
static_assert(sizeof(TexHeader) == 16, "TexHeader layout");

inline u32 texBytesPerBlock(u8 format) { return format == TEX_ETC1A4 ? 16 : 8; } // (TEX_L4: 4x4 texels of 4 bits, 8 bytes too)

// TEX_L4: the place of texel (x, image row y) of a w x h level, in nibbles from the level's start.
inline u32 texL4Nibble(u32 w, u32 h, u32 x, u32 y) {
	const u32 sy = h - 1u - y, ty = sy & 7u; // (rows stored from the bottom, as the ETC1 textures)
	const u32 at = (x & 1u) | ((ty & 1u) << 1) | ((x & 2u) << 1) | ((ty & 2u) << 2) | ((x & 4u) << 2) | ((ty & 4u) << 3);
	return ((sy >> 3) * (w >> 3) + (x >> 3)) * 64u + at;
}

inline u32 texLevelSize(const TexHeader& h, u32 level) {
	u32 w = h.width >> level, hh = h.height >> level;
	return (w / 4) * (hh / 4) * texBytesPerBlock(h.format);
}

inline u32 texLevelOffset(const TexHeader& h, u32 level) {
	u32 off = 0;
	for (u32 i = 0; i < level; ++i) off += texLevelSize(h, i);
	return off;
}

// Largest mip count PICA accepts for a size (smallest level is 8 texels).
inline u32 texMaxMipCount(u32 w, u32 h) {
	u32 n = 1, m = w < h ? w : h;
	while (m > TEX_MIN_SIZE) {
		m >>= 1;
		++n;
	}
	return n;
}

bool texValidate(const TexHeader& h, u32 availableBytes);

// Decodes one standard (big-endian) ETC1 block to 4x4 RGBA, row-major.
void etc1DecodeBlock(const u8 block[8], u8 outRGBA[64]);

// Decodes a mip level to top-down RGBA8 (w*h*4 bytes). Used by the software
// backend and the tests; the 3DS never decodes.
// An 8x8 ETC1 texture of one colour (header and 32 bytes of data), for what is drawn until the real texture is there.
void texSolid(u8 r, u8 g, u8 b, TexHeader& header, u8 data[32]);
void texDecodeLevel(const TexHeader& h, const u8* data, u32 level, u8* outRGBA);

} // namespace g3
