// The words people say: the game's output units (OU.BIN), baked as texts by
// the name the scripts give them ("DIA_Diego_Hallo_11_00").
//
// "dialog/index"   DialogIndexHeader | u32 firstHash[blockCount]
// "dialog/<k>"     DialogBlockHeader | DialogLine[count] (by hash) | texts
//
// The lines are sorted by the hash of their name (hashName, which ignores
// letter case), cut into blocks of about 24 KB of text. firstHash[k] is the
// hash of block k's first line, so a line is found by looking for the last
// block that begins at or before its hash, and in it by halving. A block is
// an asset of its own so that the console reads one at a time.
//
// The texts stay in the code page of the copy of the game (1251 for a Russian
// one, 1252 for the rest, see scriptText() in engine/ui.h), zero-terminated.
#pragma once
#include "g3ds/base.h"

namespace g3 {

constexpr u32 DIALOG_MAGIC = fourcc('G', '3', 'O', 'U');
constexpr u32 DIALOG_VERSION = 1;

struct DialogIndexHeader {
	u32 magic;
	u32 version;
	u32 blockCount;
	u32 lineCount;
	u32 reserved[4];
};
static_assert(sizeof(DialogIndexHeader) == 32, "DialogIndexHeader layout");

struct DialogBlockHeader {
	u32 count;     // lines
	u32 textBytes; // bytes of texts after the lines
};
static_assert(sizeof(DialogBlockHeader) == 8, "DialogBlockHeader layout");

struct DialogLine {
	u32 hash;
	u32 offset; // into the block's texts
};
static_assert(sizeof(DialogLine) == 8, "DialogLine layout");

} // namespace g3
