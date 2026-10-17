// fix201: the game's videos (Bink, "INTRO.BIK" ...), baked into "video.pak" beside data.pak (g3ds-bake runs ffmpeg
// over them; the console cannot decode Bink).
//
// Entry "video/<name>" (lower case, without the extension), stored unpacked so that it is read piece by piece:
//   VideoHeader | VideoChunk frames[frameCount] | VideoChunk audio[audioChunks] | the data the chunks point at
// A frame is the picture as ETC1 blocks in the PICA's byte order (g3ds/tex.h): only the blocks that changed since the
// frame before. Its data, LZ4-compressed (frame.size bytes, `raw` when unpacked):
//   u8 changed[ceil(blocks / 8)] (bit b of byte b/8 - block b, rows of blocks from the top, left to right) |
//   u8 block[8] for each changed block, in that order
// The first frame has every block. The blocks are of the picture's own rows from the top; the player puts block (bx,
// by) where a texture of the picture at its top left has it (videoBlockOffset).
// The sound: chunks of IMA ADPCM, mono, as g3ds/sound.h's (imaBytes; each chunk starts its own predictor), audioChunkFrames
// samples each (the last may have fewer: audioFrames says how many in all).
#pragma once
#include "g3ds/base.h"

namespace g3 {

constexpr u32 VIDEO_MAGIC = fourcc('G', '3', 'V', 'D');
constexpr u32 VIDEO_VERSION = 1;
// The largest picture: the top screen; the player's texture is 512 x 256.
constexpr u32 VIDEO_MAX_W = 400, VIDEO_MAX_H = 240;
constexpr u32 VIDEO_TEX_W = 512, VIDEO_TEX_H = 256;

struct VideoHeader {
	u32 magic;
	u32 version;
	u16 width, height;   // multiples of 8, at most VIDEO_MAX_W x VIDEO_MAX_H
	u32 frameCount;
	u32 fpsMilli;        // frames a second x 1000
	u32 audioRate;       // Hz; 0: no sound
	u32 audioFrames;     // samples in all
	u32 audioChunks;
	u32 audioChunkFrames;
	u32 maxFrameRaw;     // the largest frame unpacked (bytes)
	u32 maxFramePacked;  // ... and packed
	u32 reserved[5];
};
static_assert(sizeof(VideoHeader) == 64, "VideoHeader layout");

struct VideoChunk {
	u32 offset; // from the entry's start
	u32 size;   // bytes stored
	u32 raw;    // a frame: bytes unpacked; the sound: samples
};
static_assert(sizeof(VideoChunk) == 12, "VideoChunk layout");

inline u32 videoBlocks(const VideoHeader& h) { return (u32(h.width) / 4u) * (u32(h.height) / 4u); }

// Where block (bx, by) (of 4x4 texels, rows of blocks from the top) is in a VIDEO_TEX_W x VIDEO_TEX_H ETC1 texture that
// has the picture at its top left (rows stored from the bottom, 8x8 tiles in raster order, four blocks a tile in Z
// order: g3ds/tex.h). In bytes.
inline u32 videoBlockOffset(u32 bx, u32 by) {
	const u32 sby = VIDEO_TEX_H / 4u - 1u - by; // (stored block row)
	const u32 tile = (sby >> 1) * (VIDEO_TEX_W / 8u) + (bx >> 1);
	const u32 sub = ((sby & 1u) << 1) | (bx & 1u);
	return (tile * 4u + sub) * 8u;
}

} // namespace g3
