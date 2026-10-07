// G3WD / G3CH: the static world, pre-cut into a grid of streamable chunks.
//
// The world asset ("world/<name>") is a small always-resident index:
//   WorldHeader | WorldMaterial[materialCount] | ChunkInfo[chunkCount]
// Each chunk ("world/<name>/<index>") is loaded on demand:
//   ChunkHeader | ChunkBatch[batchCount] | WorldVertex[vertexCount] | u16[indexCount]
//
// Vertices are 16 bytes and go to the GPU untouched. Positions are quantised
// per chunk (world = origin + xyz * posScale), UVs are fixed point
// (uv = st * uvScale) and the colour is the baked static light. The same
// vertex and index data doubles as the collision mesh (engine/collision.h).
// Front faces are counter-clockwise on screen (the PICA/OpenGL default).
#pragma once
#include "g3ds/base.h"

namespace g3 {

constexpr u32 WORLD_MAGIC = fourcc('G', '3', 'W', 'D');
constexpr u32 CHUNK_MAGIC = fourcc('G', '3', 'C', 'H');
constexpr u32 WORLD_VERSION = 2; // 2: vertex alpha = daylight weight, MATF_NO_COLLISION

constexpr u32 CHUNK_MAX_VERTICES = 65535; // 16-bit indices
constexpr float WORLD_UV_SCALE = 1.0f / 1024.0f;

enum MaterialFlags : u16 {
	MATF_ALPHA_TEST = 1 << 0,
	MATF_WATER = 1 << 1,   // reserved: blended pass
	MATF_NO_TEXTURE = 1 << 2, // texture missing at bake time, uses the fallback
	MATF_NO_COLLISION = 1 << 3, // drawn, but the player walks through it (leaves, water, ...)
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
	float spawnPos[3]; // camera/player start
	float spawnYaw;    // radians, 0 looks down +Z
};
static_assert(sizeof(WorldHeader) == 64, "WorldHeader layout");

struct WorldMaterial {
	u32 textureHash; // pak entry of the G3TX
	u16 flags;       // MaterialFlags
	u16 group;       // ZenGin material group (sound/footstep class), 0 if unknown
};
static_assert(sizeof(WorldMaterial) == 8, "WorldMaterial layout");

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

struct ChunkBatch {
	u16 material; // index into the world material table
	u16 reserved;
	u32 firstIndex;
	u32 indexCount;
};
static_assert(sizeof(ChunkBatch) == 12, "ChunkBatch layout");

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

} // namespace g3
