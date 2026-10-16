// A picture of the game's interface ("pic/<name>": the texture's name lower case without extension, as
// "startscreen", "menu_ingame", "loading", "progress"): the start menu's background and frame, the pictures of
// its items, the loading screen. PicHeader | texW x texH texels of 16 bits, rows from the top, the picture
// (width x height) in their top left corner - ready for gfx::uiTextureCreateColor (RGB565) or
// gfx::uiTextureCreateRGBA4 (a picture with transparent parts).
#pragma once
#include "g3ds/base.h"

namespace g3 {

constexpr u32 PIC_MAGIC = fourcc('G', '3', 'P', 'C');
constexpr u32 PIC_VERSION = 1;

enum PicFormat : u16 { PIC_RGB565 = 0, PIC_RGBA4 = 1 };

struct PicHeader {
	u32 magic;
	u32 version;
	u16 width, height;   // the picture
	u16 texW, texH;      // the texture it lies in (powers of two, 8..512)
	u16 format;          // PicFormat
	u16 origW, origH;    // the size the game's texture has (what the game's layout measures it by)
	u16 reserved;
};
static_assert(sizeof(PicHeader) == 24, "PicHeader layout");

} // namespace g3
