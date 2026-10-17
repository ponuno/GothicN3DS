// The saved games, as the original keeps them: ten slots of the player's own
// and the quick one (state.bin: R + START, and the place the game is left at).
//
// A save file begins with a header that the menu reads without loading the
// game: the name the player gave it, the world, the day and hour of the story,
// the chapter, when it was saved and how long had been played, and a picture
// of the moment (THUMB_W x THUMB_H, RGB565). The position itself follows as
// App::saveState has always written it, so a file of before the header (it
// starts with the position's own magic) still loads - its slot just has no
// picture and no name.
#pragma once
#include "g3ds/base.h"

#include <stdint.h>
#include <stdio.h>

namespace g3 {

typedef int64_t i64;

constexpr int SAVE_SLOTS = 10;                // the player's own; 0 is the quick one
constexpr int SAVE_AUTO = SAVE_SLOTS + 1;     // fix208: the one made on leaving the game (autosave.sav beside the quick one)
constexpr int SAVE_ALL = SAVE_SLOTS + 2;      // all of them: 0, 1..SAVE_SLOTS, SAVE_AUTO
constexpr int THUMB_W = 160, THUMB_H = 96;    // the picture of a save (the top screen 2.5 times smaller)
constexpr int THUMB_TEX_W = 256, THUMB_TEX_H = 128; // the texture it is shown with (a power of two; the picture in its corner)
constexpr u32 SAVE_MAGIC = fourcc('G', '3', 'S', 'V');
constexpr u32 SAVE_HEADER_VERSION = 1;

// What the menu shows of a save.
struct SaveInfo {
	bool used = false;     // there is a file
	bool readable = false; // and its header is whole (or it is a file of before the header)
	bool legacy = false;   // a file of before the header: no name, no picture (App fills the world and the hour)
	bool picture = false;  // the file carries a picture
	char title[48] = "";   // UTF-8
	char world[32] = "";   // the world asset's short name ("world", "oldmine")
	i32 day = 0;           // Wld_GetDay: 0 on the first day
	float timeOfDay = 0;   // hours
	i32 chapter = 0;       // the scripts' Kapitel, 0 if none
	u32 playSeconds = 0;   // time played (the pause menu not counted)
	i64 savedAt = 0;       // when, seconds since 1970 by the console's clock (0: unknown)
};

// The header on the card (little-endian, as the console and the PC both are).
struct SaveHeader {
	u32 magic;
	u32 version;
	u32 bytes;       // the header and the picture: where the position starts
	char title[48];
	char world[32];
	i32 day;
	float timeOfDay;
	i32 chapter;
	u32 playSeconds;
	u32 savedAtLo, savedAtHi;
	u16 thumbW, thumbH; // 0 x 0: no picture
	u32 check;          // of everything before it
};
static_assert(sizeof(SaveHeader) == 124, "SaveHeader layout");

u32 saveHeaderCheck(const SaveHeader& h);
// Fills a header from `info` (the picture's size given apart).
SaveHeader makeSaveHeader(const SaveInfo& info, bool picture);
// Writes the header and the picture (THUMB_W x THUMB_H, or none if null).
bool writeSaveHeader(FILE* f, const SaveInfo& info, const u16* thumb);
// Reads what is before the position. A file with the header: true, `info`
// filled, `thumb` (THUMB_W x THUMB_H, may be null) has the picture, and `f`
// stands where the position starts. A file of before the header: true, info
// legacy, `f` back at its start. False for a damaged header (the file is not
// to be loaded) - `f` is then where it was left.
bool readSaveHeader(FILE* f, SaveInfo& info, u16* thumb);
// The same for a path; `info.used` false when there is no file.
bool readSaveInfo(const char* path, SaveInfo& info, u16* thumb);

// For showing. The world: the name the player knows it by (tr()'d), or the
// asset's own name for one not known.
const char* worldTitle(const char* world);
// "День 3, 14:20" (the first day is day 1, as the player counts).
void formatGameTime(char* out, u32 size, i32 day, float timeOfDay);
// "2 ч 05 мин" / "15 мин".
void formatPlayTime(char* out, u32 size, u32 seconds);
// "16.10.2026 22:30", or "" for 0.
void formatSaveDate(char* out, u32 size, i64 savedAt);
// The name a new save gets if the player gives none: "Колония, день 3".
void defaultSaveTitle(char* out, u32 size, const char* world, i32 day);
// Copies a name into `to`, cut (if it must be) between letters of UTF-8, never inside one.
void copySaveTitle(char* to, u32 size, const char* from);
// The short name of a world asset ("world/oldmine" -> "oldmine").
const char* worldShortName(const char* asset);

// Shrinks a picture (any size, `get(x, y, rgb)`) into w x h RGB565, each texel the mean of what it covers
// (the backends' gfx::captureFrame).
template <class Get> void shrinkPicture(int srcW, int srcH, Get get, u16* out, int w, int h) {
	for (int oy = 0; oy < h; ++oy) {
		const int y0 = oy * srcH / h, y1 = (oy + 1) * srcH / h > y0 ? (oy + 1) * srcH / h : y0 + 1;
		for (int ox = 0; ox < w; ++ox) {
			const int x0 = ox * srcW / w, x1 = (ox + 1) * srcW / w > x0 ? (ox + 1) * srcW / w : x0 + 1;
			u32 sum[3] = {0, 0, 0}, n = 0;
			for (int y = y0; y < y1; ++y)
				for (int x = x0; x < x1; ++x) {
					u8 c[3];
					get(x, y, c);
					sum[0] += c[0];
					sum[1] += c[1];
					sum[2] += c[2];
					++n;
				}
			const u32 r = sum[0] / n, g = sum[1] / n, b = sum[2] / n;
			out[oy * w + ox] = u16(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
		}
	}
}

} // namespace g3
