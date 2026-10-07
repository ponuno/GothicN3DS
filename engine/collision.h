// Collision against the static world. A chunk's collision mesh is its render
// mesh: the quantised vertex positions plus the indices of every batch whose
// material collides. A small XZ grid per chunk keeps queries to a handful of
// triangles. All queries treat triangles as two-sided.
#pragma once
#include "g3ds/math.h"
#include "g3ds/world.h"

namespace g3 {

struct ChunkCollider {
	i16* positions = nullptr; // 3 per vertex, chunk-local quantised units
	u16* triangles = nullptr; // 3 vertex indices per triangle
	u32* cellStart = nullptr; // gridW * gridH + 1 offsets into cellItems
	u32* cellItems = nullptr; // triangle numbers
	u32 vertexCount = 0, triangleCount = 0;
	float origin[3] = {0, 0, 0};
	float scale = 1.0f;    // world = origin + local * scale
	float cellSize = 1.0f; // in local units
	u32 gridW = 0, gridH = 0;

	// Copies what collision needs out of a chunk. False on allocation failure;
	// a chunk without any collidable triangle is valid and simply empty.
	bool build(const ChunkHeader& h, const ChunkBatch* batches, const WorldVertex* vertices, const u16* indices,
	           const WorldMaterial* materials);
	void release();
	u32 bytes() const;

	// Highest surface under `from`, no lower than from.y - maxDrop.
	bool rayDown(Vec3 from, float maxDrop, float& hitY, Vec3& normal) const;
	// Moves the sphere out of any triangle it overlaps. True if it was moved.
	bool pushSphere(Vec3& center, float radius) const;
};

// Closest point to p on triangle abc (exposed for tests).
Vec3 closestPointOnTriangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c);

} // namespace g3
