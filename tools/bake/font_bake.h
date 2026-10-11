// The game's fonts (FNT + the texture of each) -> "font/ui" (g3ds/font.h).
#pragma once
#include "scene.h"

#include <string>
#include <vector>

namespace g3 {

// One font as the game has it: 256 cells of a sheet, by the byte of a Windows code page.
struct SrcFontFace {
	std::string name;
	int height = 0;
	struct Cell {
		int width = 0;            // the advance, pixels
		float u0 = 0, v0 = 0, u1 = 0, v1 = 0; // corners in the sheet, 0..1
	};
	std::vector<Cell> cells;
	SrcImage sheet;
};

struct SrcFonts {
	SrcFontFace face[2]; // text, title
	int codepage = 1252;
};

// The Windows code page of the copy's texts (1251 for a Russian one, 1250 Polish, else 1252),
// told from the bytes of some of them.
int detectCodePage(const std::vector<std::string>& texts);

// Packs the two faces into one atlas. Returns false (with the reason in `why`) if they do not fit.
bool bakeFont(const SrcFonts& fonts, std::vector<u8>& out, std::string& report, std::string& why);

} // namespace g3
