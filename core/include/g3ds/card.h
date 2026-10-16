// The picture of a chapter's card ("card/<name>", the texture IntroduceChapter names, lower case without
// extension): CardHeader | RGB565 rows from the top (w * h * 2 bytes). The baker fits the game's picture into
// the top screen (letterboxed, black around it).
#pragma once
#include "g3ds/base.h"

namespace g3 {

constexpr u32 CARD_MAGIC = fourcc('G', '3', 'C', 'D');
constexpr u32 CARD_VERSION = 1;
constexpr int CARD_W = 400, CARD_H = 240;

struct CardHeader {
	u32 magic;
	u32 version;
	u16 width, height;
	u32 reserved;
};
static_assert(sizeof(CardHeader) == 16, "CardHeader layout");

} // namespace g3
