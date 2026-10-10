// Collision against the static world. A chunk's collision mesh is its render
// mesh: the quantised vertex positions plus the indices of every batch whose
// material collides. A small XZ grid per chunk keeps queries to a handful of
// triangles. All queries treat triangles as two-sided. Water surfaces get a
// collider of their own, used to find the water level.
#pragma once
#include "g3ds/math.h"
#include "g3ds/world.h"

namespace g3 {

struct ChunkCollider {
	i16* positions = nullptr; // 3 per vertex, chunk-local quantised units
	u16* triangles = nullptr; // 3 vertex indices per triangle
	u32* cellStart = nullptr; // gridW * gridH + 1 offsets into cellItems
	// Per cell: a triangle's number (ITEM_TRI_BITS bits) and, above it, the
	// first and the last of the LEVELS height levels it reaches into. A cell
	// is a column through the whole chunk - every storey of a tower is in it -
	// and the levels let a query skip what is above and below it without
	// looking at a single vertex.
	u32* cellItems = nullptr;
	u8* light = nullptr;      // 4 per triangle: its baked light (r g b) and daylight weight
	u8* group = nullptr;      // 1 per triangle: the material group (what a step on it sounds like)
	u32 vertexCount = 0, triangleCount = 0;
	float origin[3] = {0, 0, 0};
	float scale = 1.0f;    // world = origin + local * scale
	float cellSize = 1.0f; // in local units
	u32 gridW = 0, gridH = 0;
	i32 gridX0 = 0, gridZ0 = 0; // local coordinates of the grid's corner (they can be negative)
	i32 levelY0 = 0;            // local height the levels start at
	float levelScale = 0;       // levels per local unit

	enum : u32 { ITEM_TRI_BITS = 20, LEVEL_BITS = 6, LEVELS = 1u << LEVEL_BITS, MAX_TRIANGLES = 1u << ITEM_TRI_BITS };

	// Copies what collision needs out of a chunk: the triangles of every batch
	// whose material has (flags & flagMask) == flagValue. False on allocation
	// failure; a chunk without any such triangle is valid and simply empty.
	bool build(const ChunkHeader& h, const ChunkBatch* batches, const WorldVertex* vertices, const u16* indices,
	           const WorldMaterial* materials, u16 flagMask, u16 flagValue);
	void release();
	u32 bytes() const;

	// Highest surface under `from`, no lower than from.y - maxDrop. `lightOut`,
	// if given, receives the four light bytes of the triangle that was hit.
	bool rayDown(Vec3 from, float maxDrop, float& hitY, Vec3& normal, u8* lightOut = nullptr, u8* groupOut = nullptr) const;
	// Moves the sphere out of any triangle it overlaps. True if it was moved.
	bool pushSphere(Vec3& center, float radius) const;
	// True if no triangle's bounds reach into the box (so nothing in it can
	// be hit; the reverse does not hold).
	bool boxFree(const float mn[3], const float mx[3]) const;

private:
	// The levels local heights y0..y1 reach into; false if none.
	bool levels(float y0, float y1, u32& lo, u32& hi) const;
};

// What the queries of all colliders have cost since the last reset: for the
// reports (a frame's worth says where a slow place spends its time).
struct CollisionStats {
	u32 queries = 0; // rayDown, pushSphere and boxFree calls that reached a grid
	u32 items = 0;   // cell entries looked at
	u32 tests = 0;   // triangles whose vertices were looked at
};
const CollisionStats& collisionStats();
void collisionStatsReset();

// Closest point to p on triangle abc (exposed for tests).
Vec3 closestPointOnTriangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c);

} // namespace g3
