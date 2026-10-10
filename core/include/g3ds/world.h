// G3WD / G3CH: the static world, pre-cut into a grid of streamable chunks.
//
// The world asset ("world/<name>") is a small always-resident index:
//   WorldHeader | WorldMaterial[materialCount] | u32 textureHash[textureCount] |
//   WorldPortal[portalCount] | WorldLadder[ladderCount] | ChunkInfo[chunkCount]
// Each chunk ("world/<name>/<index>") is loaded on demand:
//   ChunkHeader | ChunkBatch[batchCount] | WorldVertex[vertexCount] | u16[indexCount]
//
// Vertices are 16 bytes and go to the GPU untouched. Positions are quantised
// per chunk (world = origin + xyz * posScale), UVs are fixed point
// (uv = st * uvScale) and the colour is the baked static light. The same
// vertex and index data doubles as the collision mesh (engine/collision.h).
// Front faces are counter-clockwise on screen (the PICA/OpenGL default).
//
// "meta/start" (text) names the world the game starts in.
#pragma once
#include "g3ds/base.h"

namespace g3 {

constexpr u32 WORLD_MAGIC = fourcc('G', '3', 'W', 'D');
constexpr u32 CHUNK_MAGIC = fourcc('G', '3', 'C', 'H');
// 3: texture table, animated/scrolling/blended materials, batch bounds and
//    detail classes, portals between worlds, ladders, indoor worlds.
// 4: chunk-local positions are signed (the origin is the middle of the
//    chunk), on a lattice of 1/8 cm. Same layout as 3.
constexpr u32 WORLD_VERSION = 4;

constexpr u32 CHUNK_MAX_VERTICES = 65535; // 16-bit indices
constexpr float WORLD_UV_SCALE = 1.0f / 1024.0f;
constexpr float WORLD_SCROLL_SCALE = 1.0f / 4096.0f; // WorldMaterial::scroll unit, repeats per second

enum MaterialFlags : u16 {
	MATF_ALPHA_TEST = 1 << 0,   // cut-out: fragments with alpha <= 0.5 are dropped
	MATF_BLEND = 1 << 1,        // drawn in the blended pass: colour * alpha + dst * (1 - alpha)
	MATF_NO_TEXTURE = 1 << 2,   // texture missing at bake time, uses the fallback
	MATF_NO_COLLISION = 1 << 3, // drawn, but the player walks through it (leaves, water, ...)
	MATF_ADD = 1 << 4,          // drawn in the blended pass: colour * alpha + dst (light shafts, glow)
	MATF_WATER = 1 << 5,        // a water surface: the player wades and swims below it
};
constexpr u16 MATF_TRANSLUCENT = MATF_BLEND | MATF_ADD;

// How far away a batch is still drawn. The world mesh and big objects are
// always drawn; smaller objects only near the camera (StreamConfig).
enum DetailClass : u8 {
	DETAIL_ALWAYS = 0,
	DETAIL_MEDIUM = 1,
	DETAIL_SMALL = 2,
	DETAIL_COUNT = 3,
};

struct WorldHeader {
	u32 magic;
	u32 version;
	float chunkSize; // grid cell edge, cm
	u32 chunkCount;
	u32 materialCount;
	u32 triangleCount; // whole world, for statistics
	float bbMin[3];
	float bbMax[3];
	float spawnPos[3]; // camera/player start (eye position)
	float spawnYaw;    // radians, 0 looks down +Z
	u32 textureCount;
	u32 portalCount;
	u32 ladderCount;
	u32 flags;       // WorldFlags
	u8 fogColor[3];  // WORLDF_INDOOR: colour of the fog and of everything not drawn
	u8 reserved8;
	u32 reserved[3];
};
static_assert(sizeof(WorldHeader) == 96, "WorldHeader layout");

enum WorldFlags : u32 {
	// A world without sky (a mine, a temple): nothing is tinted by the time of
	// day and the background is WorldHeader::fogColor.
	WORLDF_INDOOR = 1 << 0,
};

struct WorldMaterial {
	u16 texture;   // index into the texture table; animation frames follow it
	u16 flags;     // MaterialFlags
	u8 group;      // ZenGin material group (sound/footstep class), 0 if unknown
	u8 frameCount; // >= 1; more than one for an animated texture
	u8 opacity;    // alpha multiplier for blended materials, 255 = as in the texture
	u8 reserved;
	u16 frameMs;   // milliseconds per animation frame
	i16 scroll[2]; // texture scrolling in WORLD_SCROLL_SCALE repeats per second
	u16 reserved2;
};
static_assert(sizeof(WorldMaterial) == 14 + 2, "WorldMaterial layout");

// A box that sends the player to another world of the same archive.
struct WorldPortal {
	float bbMin[3];
	float bbMax[3];
	u32 targetWorld;   // pak entry hash of the target world asset
	u32 flags;         // PortalFlags
	float targetPos[3]; // eye position in the target world
	float targetYaw;
};
static_assert(sizeof(WorldPortal) == 48, "WorldPortal layout");

enum PortalFlags : u32 {
	PORTALF_USE_SPAWN = 1 << 0, // the arrival point was not found: use the target world's spawn
};

// A box in which the player climbs instead of walking: a ladder with some
// room in front of it and above its top.
struct WorldLadder {
	float bbMin[3];
	float bbMax[3];
};
static_assert(sizeof(WorldLadder) == 24, "WorldLadder layout");

struct ChunkInfo {
	float bbMin[3];
	float bbMax[3];
	u32 entryHash; // pak entry of the chunk blob
	u32 size;      // decompressed blob size
	u16 vertexCount;
	u16 batchCount;
	u32 indexCount;
};
static_assert(sizeof(ChunkInfo) == 40, "ChunkInfo layout");

struct ChunkHeader {
	u32 magic;
	u16 batchCount;
	u16 vertexCount;
	u32 indexCount;
	float origin[3];
	float posScale;
	float uvScale;
};
static_assert(sizeof(ChunkHeader) == 32, "ChunkHeader layout");

// One draw call's worth: a run of indices with one material. A chunk is cut
// into "clusters" - boxes of some ten metres - and a batch holds what one
// material has in one of them: of a big chunk only the clusters in view are
// drawn. Batches of a material follow each other, so that neighbours in the
// index buffer which are both in view go out as one draw call.
struct ChunkBatch {
	u16 material; // index into the world material table
	u8 detail;    // DetailClass
	u8 cluster;   // which cluster of the chunk it lies in (0 in archives baked without clusters)
	u32 firstIndex;
	u32 indexCount;
	i16 bbMin[3]; // bounds of the batch in the chunk's quantised space
	i16 bbMax[3];
};
static_assert(sizeof(ChunkBatch) == 24, "ChunkBatch layout");

struct WorldVertex {
	i16 x, y, z, w; // w is padding (always 1); the shader forces w = 1.0
	i16 u, v;
	u8 r, g, b; // baked static light
	u8 a;       // how much the time of day tints that light: 255 outdoors, 0 under a lightmap
};
static_assert(sizeof(WorldVertex) == 16, "WorldVertex layout");

inline u32 chunkBlobSize(u32 batches, u32 vertices, u32 indices) {
	return u32(sizeof(ChunkHeader)) + batches * u32(sizeof(ChunkBatch)) + vertices * u32(sizeof(WorldVertex)) +
	       indices * u32(sizeof(u16));
}

inline u32 worldBlobSize(const WorldHeader& h) {
	return u32(sizeof(WorldHeader)) + h.materialCount * u32(sizeof(WorldMaterial)) + h.textureCount * u32(sizeof(u32)) +
	       h.portalCount * u32(sizeof(WorldPortal)) + h.ladderCount * u32(sizeof(WorldLadder)) +
	       h.chunkCount * u32(sizeof(ChunkInfo));
}

} // namespace g3
