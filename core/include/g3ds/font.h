// "font/ui": the game's own fonts (Fonts.vdf: FONT_OLD_10_WHITE and the like),
// cut up and packed again into one atlas for the overlay (engine/ui.cpp).
// Layout: FontHeader | FontGlyphRec[glyphCount] (by size, then code) |
// coverage[atlasW * atlasH], one byte a texel (0 clear .. 255 solid).
// Codes are Unicode: the baker has turned the font's code page (1251 for a
// Russian copy, else 1252) into them, so the runtime need not know it.
#pragma once
#include "g3ds/base.h"

namespace g3 {

constexpr u32 FONT_MAGIC = fourcc('G', '3', 'F', 'T');
constexpr u32 FONT_VERSION = 1;
constexpr u32 FONT_ATLAS_W = 256;       // the overlay's texture is this wide
constexpr u32 FONT_ATLAS_MAX_H = 512;   // ... and the atlas fits in this much of it

struct FontHeader {
	u32 magic;
	u32 version;
	u16 atlasW, atlasH;
	u16 glyphCount;
	u16 codepage;
	u8 line[2];   // per size (0 text, 1 title): the height of a line
	u8 ascent[2]; // ... and how far the baseline is below its top
	u32 reserved;
};
static_assert(sizeof(FontHeader) == 24, "FontHeader layout");

struct FontGlyphRec {
	u16 code;
	u16 x, y;     // in the atlas
	u8 size;
	u8 w, h;
	i8 left, top; // from the pen at the top of the line
	u8 advance;
};
static_assert(sizeof(FontGlyphRec) == 12, "FontGlyphRec layout");

} // namespace g3
