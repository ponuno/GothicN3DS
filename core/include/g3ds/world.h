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
//    Later, same version and layout: WORLDF_AO / WORLDF_WIND and MATF_WIND
//    (what the vertex's w carries then); a runtime that knows nothing of
//    them draws such a world as before.
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
	MATF_WIND = 1 << 6,         // some of its vertices sway in the wind (WorldVertex::w, high byte)
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
	// Pak entry of the world's far layer (FarHeader below; g3ds-bake
	// --far-lod), 0 if it has none. A runtime that knows nothing of it reads
	// the field as reserved and draws the world as before.
	u32 farEntry;
	// Pak entry of the world's visibility table (VisHeader below; g3ds-bake
	// --vis), 0 if it has none. Ignored by a runtime that does not know it.
	u32 visEntry;
	u32 reserved;
};
static_assert(sizeof(WorldHeader) == 96, "WorldHeader layout");

enum WorldFlags : u32 {
	// A world without sky (a mine, a temple): nothing is tinted by the time of
	// day and the background is WorldHeader::fogColor.
	WORLDF_INDOOR = 1 << 0,
	// The vertices carry baked ambient occlusion in the low byte of
	// WorldVertex::w (g3ds-bake --ao). Without it w is 1 and means nothing.
	WORLDF_AO = 1 << 1,
	// Plants carry how far they sway in the high byte of WorldVertex::w
	// (g3ds-bake --wind); their materials have MATF_WIND.
	WORLDF_WIND = 1 << 2,
	// The chunks are cut by detail class (g3ds-bake, on by default): after the
	// chunk table follows one byte per chunk, its tier (the lowest DetailClass
	// of what it holds). Tier 0 is the world itself and the big things; a chunk
	// of tier 1 or 2 holds only medium and small objects, is loaded only when
	// the camera comes within their reach, and is freed again when it leaves.
	// Without the flag every chunk is tier 0 and holds everything. A runtime
	// that does not know the flag sees a blob of the wrong size and refuses it.
	WORLDF_TIERS = 1 << 3,
	// Chunks with batches for a simplified stand-in (see batchLod): the engine draws those instead of the
	// full-detail ones of a chunk that lies far enough away (g3ds-bake --lod).
	WORLDF_LOD = 1 << 4,
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

// "world/<name>/rooms" (optional): the world's rooms - the sectors of its BSP
// tree (a hut, a house, a hall), as the scripts know them by name
// (Wld_AssignRoomToGuild, Wld_GetPlayerPortalGuild). A room is the boxes of
// the BSP leaves that belong to it and to no other sector (OpenGothic's
// World::roomAt); a point in none of them is outside.
//   RoomsHeader | RoomBox[boxCount] | u32 nameOffset[roomCount] | char names[nameBytes] (padded to 4)
// Rooms are numbered from 1 (0: outside); names are upper case.
constexpr u32 ROOMS_MAGIC = fourcc('G', '3', 'R', 'M');
constexpr u32 ROOMS_VERSION = 1;

struct RoomsHeader {
	u32 magic;
	u32 version;
	u32 roomCount;
	u32 boxCount;
	u32 nameBytes;
	u32 reserved[3];
};
static_assert(sizeof(RoomsHeader) == 32, "RoomsHeader layout");

struct RoomBox {
	float bbMin[3];
	float bbMax[3];
	u16 room; // 1..roomCount
	u16 reserved;
};
static_assert(sizeof(RoomBox) == 28, "RoomBox layout");

// "world/<name>/triggers" (optional): the world's triggers and movers, as ZenGin (and OpenGothic's
// world/triggers) has them. An event goes by name to every trigger of that name; a trigger sends its own on to
// its target. A mover (a gate, a lift, a drawbridge) moves its mesh ("model/item.<visual>") between its
// keyframes when it is triggered; it is no part of the chunks then.
//   TriggersHeader | WorldTrigger[count] | MoverKey[keyCount] | TriggerTarget[targetCount] |
//   float collision[collisionTris * 9] (version 2) | char names[nameBytes] (padded to 4)
// A door (TRG_DOOR, version 2) has two keys: its place (closed), and in the second key's position the point it
// turns about (its own space); its key item and combination are `target` and `sfx[0]`.
constexpr u32 TRIGGERS_MAGIC = fourcc('G', '3', 'T', 'R');
constexpr u32 TRIGGERS_VERSION = 2; // (1 is read too: no collision, no doors)

enum TriggerKind : u8 {
	TRG_TRIGGER = 1,    // zCTrigger: sends its target on (touched or triggered)
	TRG_LIST = 2,       // zCTriggerList: several targets, with delays (all, the next, a random one)
	TRG_SCRIPT = 3,     // oCTriggerScript: calls a script function (WorldTrigger::target)
	TRG_WORLDSTART = 4, // zCTriggerWorldStart: sends its target when the world is entered
	TRG_MOVER = 5,      // zCMover
	TRG_DOOR = 6,       // oCMobDoor: the player opens and shuts it (and the inhabitants walking through)
};

enum TriggerFlags : u16 {
	TRGF_START_ENABLED = 1 << 0,
	TRGF_SEND_UNTRIGGER = 1 << 1,
	TRGF_REACT_TRIGGER = 1 << 2,
	TRGF_REACT_TOUCH = 1 << 3,
	TRGF_RESPOND_PC = 1 << 4,
	TRGF_RESPOND_NPC = 1 << 5,
	TRGF_FIRE_ONCE = 1 << 6,   // a world start: only the first time the world is entered
	TRGF_AUTO_ROTATE = 1 << 7, // a mover: turned along its way
	TRGF_COLLIDES = 1 << 8,    // a mover or door: the player cannot pass through it (its triangles: collFirst, collCount)
	TRGF_LOCKED = 1 << 9,      // a door: locked
};

struct TriggersHeader {
	u32 magic;
	u32 version;
	u32 count;
	u32 keyCount;
	u32 targetCount;
	u32 nameBytes;
	u32 collisionTris; // version 2
	u32 reserved;
};
static_assert(sizeof(TriggersHeader) == 32, "TriggersHeader layout");

struct WorldTrigger {
	u8 kind;      // TriggerKind
	u8 behavior;  // a mover: 0 toggle, 1 trigger control, 2 open timed, 3 loop, 4 single keys; a list: 0 all, 1 next, 2 random
	u16 flags;    // TriggerFlags
	u32 name;     // offset into names (upper case)
	u32 target;   // offset into names: the target (upper case), or the script function
	u32 visual;   // offset into names: a mover's mesh (lower case, no extension), 0: none
	i32 maxActivations; // -1: no limit
	float retriggerDelay, fireDelay; // seconds
	float bbMin[3], bbMax[3];
	u32 first;    // a mover: its first key; a list: its first target
	u16 count;    // ... and how many
	u8 speedMode; // a mover: zenkit::MoverSpeedType
	u8 lerp;      // a mover: 0 curve, 1 linear
	float speed;  // a mover: cm per millisecond (rotations: see the runtime)
	float stayOpen; // a mover: seconds an "open timed" one stays open
	u32 sfx[5];   // a mover's sounds: open start, open end, close start, close end, moving (offsets; 0: none)
	u32 collFirst; // its collision triangles (TRGF_COLLIDES)
	u32 collCount;
};
static_assert(sizeof(WorldTrigger) == 96, "WorldTrigger layout");

struct MoverKey {
	float pos[3];
	float rot[4]; // x y z w, rotating column vectors (world)
};
static_assert(sizeof(MoverKey) == 28, "MoverKey layout");

struct TriggerTarget {
	u32 name;    // offset into names
	float delay; // seconds
};
static_assert(sizeof(TriggerTarget) == 8, "TriggerTarget layout");

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

// ChunkBatch::detail bits 4-5: how big the small objects of the batch are (g3ds-bake, by their bounding radius):
// band 0 60 cm and more, 1 from 30, 2 from 15, 3 less. The runtime draws a band only as far as its smallest things
// are still a few pixels across (StreamConfig::sizePixels); band 0 - and everything of an older archive, where the
// bits are 0 - as far as its class. Other classes have band 0.
constexpr u32 SIZE_BANDS = 4;
inline u8 sizeBandOf(float radiusCm) { return radiusCm >= 60.0f ? 0 : radiusCm >= 30.0f ? 1 : radiusCm >= 15.0f ? 2 : 3; }
// The least radius of a band, cm (band 3: about the middle of what it holds; band 0: none, it is not limited).
inline float sizeBandRadius(u8 band) { return band == 1 ? 30.0f : band == 2 ? 15.0f : band == 3 ? 8.0f : 0.0f; }
inline u8 batchSizeBand(u8 detail) { return u8((detail >> 4) & 3u); }

// ChunkBatch::detail: bits 0-1 the DetailClass, 2-3 the layer of small objects, 4-5 their size band, 6-7 which of a chunk's two
// pictures the batch belongs to (g3ds-bake --lod): a chunk close to the camera is drawn with its ordinary
// batches, one farther away with simplified stand-ins for some of them.
enum BatchLod : u8 {
	BATCH_LOD_BOTH = 0,      // drawn either way
	BATCH_LOD_FULL_ONLY = 1, // has a stand-in: drawn only when the chunk is near
	BATCH_LOD_FAR_ONLY = 2,  // the stand-in: drawn only when the chunk is far (not part of the collision mesh)
};
inline u8 batchLod(u8 detail) { return u8(detail >> 6); }
inline u8 withBatchLod(u8 detail, u8 lod) { return u8((detail & 0x3Fu) | (lod << 6)); }

struct WorldVertex {
	i16 x, y, z;
	// World vertices of an archive baked with --ao or --wind (WORLDF_AO,
	// WORLDF_WIND): bits 0-7 how much of the sky the corner does not see
	// (ambient occlusion, 0 = open, 255 = shut in), bits 8-14 how far it sways
	// in the wind, in quarters of a centimetre (0 = it stands still).
	// Otherwise 1, meaning nothing. The shader forces the position's w to 1.0.
	i16 w;
	i16 u, v;
	u8 r, g, b; // baked static light
	u8 a;       // how much the time of day tints that light: 255 outdoors, 0 under a lightmap
};
static_assert(sizeof(WorldVertex) == 16, "WorldVertex layout");

// WorldVertex::w of a world with WORLDF_AO / WORLDF_WIND.
constexpr float WORLD_WIND_UNIT = 0.25f; // cm of sway per step of the wind byte
inline i16 packVertexFx(u8 occlusion, u8 wind) { return i16(u16(occlusion) | (u16(wind > 127 ? 127 : wind) << 8)); }
inline u8 vertexOcclusion(i16 w) { return u8(u16(w) & 0xFFu); }
inline u8 vertexWind(i16 w) { return u8((u16(w) >> 8) & 0x7Fu); }

// ---- G3FR: the far layer ----
// A coarse copy of the whole world - its ground, buildings and big objects,
// no small things, one flat colour per corner (the texture's average times the
// baked light), no texture - drawn where the chunks are not: beyond the draw
// distance, and in their place where they are not loaded yet. It is cut into
// tiles of a few chunks' edge, one mesh each; a tile is made of runs of
// indices ("ranges"), each covering one grid cell of the chunks, so that the
// cells whose chunks are drawn in full are left out of it.
//   FarHeader | FarTile[tileCount] | FarRange[rangeCount] | u32 chunk[chunkRefCount] |
//   WorldVertex[vertexCount] | u16 index[indexCount]
// Vertices: positions as in a chunk (origin + xyz * posScale of their tile),
// u = v = 0, w = 1, rgb the colour, a the daylight weight. Indices are local
// to the tile's vertices; a range's firstIndex is local to the tile's indices.
constexpr u32 FAR_MAGIC = fourcc('G', '3', 'F', 'R');
constexpr u32 FAR_VERSION = 1;

struct FarHeader {
	u32 magic;
	u32 version;
	u32 tileCount;
	u32 rangeCount;
	u32 chunkRefCount;
	u32 vertexCount;
	u32 indexCount;
	float cellSize; // cm: the edge of the boxes the world was simplified in (the coarseness)
	u32 reserved[4];
};
static_assert(sizeof(FarHeader) == 48, "FarHeader layout");

struct FarTile {
	float bbMin[3], bbMax[3];
	float origin[3];
	float posScale;
	u32 firstVertex, vertexCount; // into the vertex array (vertexCount <= CHUNK_MAX_VERTICES)
	u32 firstIndex, indexCount;   // into the index array
	u32 firstRange, rangeCount;
};
static_assert(sizeof(FarTile) == 64, "FarTile layout");

// A range is a cell's piece of the layer - or a seam (FarRange::seam): the
// strips hanging down along the edges two cells share (against the sky
// showing through the slit where a chunk drawn in full meets the coarser far
// layer). A seam is drawn only while one of its cells is drawn in full and
// the other not: the first half of its indices faces the first cell, the
// second half the other (the one in full is where the eye is).
struct FarRange {
	float bbMin[3], bbMax[3];
	u32 firstIndex, indexCount; // local to the tile's indices
	u32 firstChunk;             // into the chunk list: the chunks of the cell (then those of the other cell of a seam)
	u16 chunkCount;             // 0: a cell without chunks, never drawn in full
	u8 seam;                    // 1: a seam (0: a cell's piece)
	u8 otherCount;              // a seam: the other cell's chunks
};
static_assert(sizeof(FarRange) == 40, "FarRange layout");

inline u32 farBlobSize(const FarHeader& h) {
	return u32(sizeof(FarHeader)) + h.tileCount * u32(sizeof(FarTile)) + h.rangeCount * u32(sizeof(FarRange)) +
	       h.chunkRefCount * u32(sizeof(u32)) + h.vertexCount * u32(sizeof(WorldVertex)) + h.indexCount * u32(sizeof(u16));
}

// ---- visibility (g3ds-bake --vis) ----
//
// Which parts of the world can be seen from which cell, worked out at bake time. A cell is a square of
// the chunk grid (WorldHeader::chunkSize); a part is a cluster of a chunk (ChunkBatch::cluster). For
// every cell the table says, for every cluster of every chunk within `radius` of it, whether it can be
// seen from somewhere in the cell - from a few eye heights, at a few places - or is hidden behind
// something solid. The camera in a cell does not draw what the cell's row marks hidden. What lies
// farther than `radius`, and everything in a cell the table has no row for, is drawn as usual.
// The estimate leans to "visible": what is near, or not certainly hidden, is marked visible.
constexpr u32 VIS_MAGIC = fourcc('G', '3', 'V', 'S');
constexpr u32 VIS_VERSION = 1;

struct VisHeader {
	u32 magic;
	u32 version;
	u32 chunkCount;  // must be the world's
	u32 viewerCount; // cells with a row
	u32 bitCount;    // all rows together
	float radius;    // cm: chunks whose cell reaches no farther than this from the viewing cell have bits
	float cellSize;  // cm
	u32 reserved;
};
static_assert(sizeof(VisHeader) == 32, "VisHeader layout");

struct VisViewer {
	i16 x, z;       // the cell: floor(position / cellSize)
	u32 bitOffset;  // where its row begins in the bit array
};
static_assert(sizeof(VisViewer) == 8, "VisViewer layout");

// The blob: VisHeader | i16 cellX[chunkCount] | i16 cellZ[chunkCount] | u8 clusters[chunkCount] (clusters in
// the chunk; 0: none) | padding to 4 | u32 order[chunkCount] (chunks sorted by cellZ, cellX, number) |
// VisViewer[viewerCount] (sorted by z, x) | u32 bits[ceil(bitCount / 32)].
// A row holds one bit per cluster (1: visible) of the chunks within reach of the viewing cell: its cells
// in rows of z, then x, and in each cell the chunks in `order`, and each chunk's clusters in turn. A cell
// is within reach when the gap between it and the viewing cell (squares of cellSize) is at most `radius`.
inline u32 visPad(u32 n) { return (4u - (n & 3u)) & 3u; }
inline u32 visBlobSize(const VisHeader& h) {
	return u32(sizeof(VisHeader)) + h.chunkCount * 4u + h.chunkCount + visPad(h.chunkCount) + h.chunkCount * 4u +
	       h.viewerCount * u32(sizeof(VisViewer)) + ((h.bitCount + 31u) / 32u) * 4u;
}

inline u32 chunkBlobSize(u32 batches, u32 vertices, u32 indices) {
	return u32(sizeof(ChunkHeader)) + batches * u32(sizeof(ChunkBatch)) + vertices * u32(sizeof(WorldVertex)) +
	       indices * u32(sizeof(u16));
}

inline u32 worldBlobSize(const WorldHeader& h) {
	return u32(sizeof(WorldHeader)) + h.materialCount * u32(sizeof(WorldMaterial)) + h.textureCount * u32(sizeof(u32)) +
	       h.portalCount * u32(sizeof(WorldPortal)) + h.ladderCount * u32(sizeof(WorldLadder)) +
	       h.chunkCount * u32(sizeof(ChunkInfo)) + ((h.flags & WORLDF_TIERS) ? h.chunkCount : 0u);
}

} // namespace g3
