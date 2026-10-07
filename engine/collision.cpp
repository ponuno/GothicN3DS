#include "collision.h"

#include <stdlib.h>
#include <string.h>

namespace g3 {

// Ericson, "Real-Time Collision Detection", 5.1.5.
Vec3 closestPointOnTriangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c) {
	const Vec3 ab = b - a, ac = c - a, ap = p - a;
	const float d1 = dot(ab, ap), d2 = dot(ac, ap);
	if (d1 <= 0 && d2 <= 0) return a;

	const Vec3 bp = p - b;
	const float d3 = dot(ab, bp), d4 = dot(ac, bp);
	if (d3 >= 0 && d4 <= d3) return b;

	const float vc = d1 * d4 - d3 * d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));

	const Vec3 cp = p - c;
	const float d5 = dot(ab, cp), d6 = dot(ac, cp);
	if (d6 >= 0 && d5 <= d6) return c;

	const float vb = d5 * d2 - d1 * d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));

	const float va = d3 * d6 - d5 * d4;
	if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));

	const float denom = 1.0f / (va + vb + vc);
	return a + ab * (vb * denom) + ac * (vc * denom);
}

void ChunkCollider::release() {
	free(positions);
	free(triangles);
	free(cellStart);
	free(cellItems);
	free(light);
	*this = ChunkCollider {};
}

u32 ChunkCollider::bytes() const {
	if (!positions) return 0;
	return vertexCount * 6 + triangleCount * (6 + (light ? 4u : 0u)) + (gridW * gridH + 1) * 4 +
	       (cellStart ? cellStart[gridW * gridH] : 0) * 4;
}

bool ChunkCollider::build(const ChunkHeader& h, const ChunkBatch* batches, const WorldVertex* vertices,
                          const u16* indices, const WorldMaterial* materials, u16 flagMask, u16 flagValue) {
	release();
	memcpy(origin, h.origin, sizeof(origin));
	scale = h.posScale > 0 ? h.posScale : 1.0f;

	u32 tris = 0;
	for (u32 b = 0; b < h.batchCount; ++b)
		if ((materials[batches[b].material].flags & flagMask) == flagValue) tris += batches[b].indexCount / 3;
	if (tris == 0) return true;

	// Only the vertices these triangles use are kept: a chunk full of trees
	// with one puddle must not pay for every leaf in its water collider.
	const u16 unused = 0xFFFF;
	u16* remap = static_cast<u16*>(malloc(size_t(h.vertexCount) * sizeof(u16) + 2));
	triangles = static_cast<u16*>(malloc(size_t(tris) * 3 * sizeof(u16)));
	light = static_cast<u8*>(malloc(size_t(tris) * 4));
	if (!remap || !triangles || !light) {
		free(remap);
		release();
		return false;
	}
	memset(remap, 0xFF, size_t(h.vertexCount) * sizeof(u16));
	triangleCount = tris;
	u32 n = 0, used = 0;
	for (u32 b = 0; b < h.batchCount; ++b) {
		if ((materials[batches[b].material].flags & flagMask) != flagValue) continue;
		const u32 count = batches[b].indexCount / 3 * 3;
		for (u32 i = 0; i < count; ++i) {
			const u16 v = indices[batches[b].firstIndex + i];
			if (remap[v] == unused) remap[v] = u16(used++);
			if (i % 3 == 0) {
				// What stands on this triangle is lit like it: the mean of its corners.
				const WorldVertex& c0 = vertices[v];
				const WorldVertex& c1 = vertices[indices[batches[b].firstIndex + i + 1]];
				const WorldVertex& c2 = vertices[indices[batches[b].firstIndex + i + 2]];
				u8* l = light + size_t(n / 3) * 4;
				l[0] = u8((u32(c0.r) + c1.r + c2.r) / 3);
				l[1] = u8((u32(c0.g) + c1.g + c2.g) / 3);
				l[2] = u8((u32(c0.b) + c1.b + c2.b) / 3);
				l[3] = u8((u32(c0.a) + c1.a + c2.a) / 3);
			}
			triangles[n++] = remap[v];
		}
	}
	positions = static_cast<i16*>(malloc(size_t(used) * 3 * sizeof(i16) + 2));
	if (!positions) {
		free(remap);
		release();
		return false;
	}
	vertexCount = used;
	for (u32 i = 0; i < h.vertexCount; ++i) {
		if (remap[i] == unused) continue;
		i16* p = positions + size_t(remap[i]) * 3;
		p[0] = vertices[i].x;
		p[1] = vertices[i].y;
		p[2] = vertices[i].z;
	}
	free(remap);

	// Grid: cells of about 2.5 m, at most 48 x 48, over the used part of the
	// chunk (local coordinates are signed: the origin is in the middle).
	i32 minX = 32767, minZ = 32767, maxX = -32768, maxZ = -32768;
	for (u32 i = 0; i < triangleCount * 3; ++i) {
		const i16* p = positions + triangles[i] * 3;
		minX = min<i32>(minX, p[0]);
		maxX = max<i32>(maxX, p[0]);
		minZ = min<i32>(minZ, p[2]);
		maxZ = max<i32>(maxZ, p[2]);
	}
	gridX0 = minX;
	gridZ0 = minZ;
	const i32 spanX = maxX - minX, spanZ = maxZ - minZ;
	cellSize = max(250.0f / scale, 1.0f);
	gridW = clamp<u32>(u32(float(spanX) / cellSize) + 1, 1, 48);
	gridH = clamp<u32>(u32(float(spanZ) / cellSize) + 1, 1, 48);
	cellSize = max(max(float(spanX + 1) / float(gridW), float(spanZ + 1) / float(gridH)), cellSize);

	const u32 cells = gridW * gridH;
	cellStart = static_cast<u32*>(calloc(cells + 1, sizeof(u32)));
	if (!cellStart) {
		release();
		return false;
	}
	auto range = [this](u32 t, u32& x0, u32& x1, u32& z0, u32& z1) {
		i32 mnx = 32767, mxx = -32768, mnz = 32767, mxz = -32768;
		for (int k = 0; k < 3; ++k) {
			const i16* p = positions + triangles[t * 3 + u32(k)] * 3;
			mnx = min<i32>(mnx, p[0]);
			mxx = max<i32>(mxx, p[0]);
			mnz = min<i32>(mnz, p[2]);
			mxz = max<i32>(mxz, p[2]);
		}
		x0 = clamp<i32>(i32(float(mnx - gridX0) / cellSize), 0, i32(gridW) - 1);
		x1 = clamp<i32>(i32(float(mxx - gridX0) / cellSize), 0, i32(gridW) - 1);
		z0 = clamp<i32>(i32(float(mnz - gridZ0) / cellSize), 0, i32(gridH) - 1);
		z1 = clamp<i32>(i32(float(mxz - gridZ0) / cellSize), 0, i32(gridH) - 1);
	};
	// Counting sort of triangles into cells.
	for (u32 t = 0; t < triangleCount; ++t) {
		u32 x0, x1, z0, z1;
		range(t, x0, x1, z0, z1);
		for (u32 z = z0; z <= z1; ++z)
			for (u32 x = x0; x <= x1; ++x) ++cellStart[z * gridW + x + 1];
	}
	for (u32 i = 1; i <= cells; ++i) cellStart[i] += cellStart[i - 1];
	cellItems = static_cast<u32*>(malloc(size_t(cellStart[cells]) * sizeof(u32) + 4));
	u32* fill = static_cast<u32*>(malloc(size_t(cells) * sizeof(u32) + 4));
	if (!cellItems || !fill) {
		free(fill);
		release();
		return false;
	}
	memcpy(fill, cellStart, size_t(cells) * sizeof(u32));
	for (u32 t = 0; t < triangleCount; ++t) {
		u32 x0, x1, z0, z1;
		range(t, x0, x1, z0, z1);
		for (u32 z = z0; z <= z1; ++z)
			for (u32 x = x0; x <= x1; ++x) cellItems[fill[z * gridW + x]++] = t;
	}
	free(fill);
	return true;
}

static inline Vec3 localVertex(const i16* positions, u16 index) {
	const i16* p = positions + index * 3;
	return {float(p[0]), float(p[1]), float(p[2])};
}

bool ChunkCollider::rayDown(Vec3 from, float maxDrop, float& hitY, Vec3& normal, u8* lightOut) const {
	if (!triangleCount) return false;
	// Work in the chunk's quantised space; the scale is uniform, so shapes keep.
	const float inv = 1.0f / scale;
	const float lx = (from.x - origin[0]) * inv, ly = (from.y - origin[1]) * inv, lz = (from.z - origin[2]) * inv;
	const float drop = maxDrop * inv;
	const float gx = lx - float(gridX0), gz = lz - float(gridZ0);
	if (gx < 0 || gz < 0) return false;
	const u32 cx = u32(gx / cellSize), cz = u32(gz / cellSize);
	if (cx >= gridW || cz >= gridH) return false;

	bool found = false;
	float best = ly - drop;
	const u32 cell = cz * gridW + cx;
	for (u32 i = cellStart[cell]; i < cellStart[cell + 1]; ++i) {
		const u16* t = triangles + cellItems[i] * 3;
		const Vec3 a = localVertex(positions, t[0]), b = localVertex(positions, t[1]), c = localVertex(positions, t[2]);
		const float det = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
		if (det > -1e-6f && det < 1e-6f) continue; // vertical: no floor to stand on
		const float w0 = ((b.z - c.z) * (lx - c.x) + (c.x - b.x) * (lz - c.z)) / det;
		const float w1 = ((c.z - a.z) * (lx - c.x) + (a.x - c.x) * (lz - c.z)) / det;
		const float w2 = 1.0f - w0 - w1;
		const float eps = -1e-4f;
		if (w0 < eps || w1 < eps || w2 < eps) continue;
		const float y = a.y * w0 + b.y * w1 + c.y * w2;
		if (y > ly || y < best) continue;
		best = y;
		found = true;
		Vec3 n = normalize(cross(b - a, c - a));
		if (n.y < 0) n = n * -1.0f;
		normal = n;
		if (lightOut && light) memcpy(lightOut, light + size_t(cellItems[i]) * 4, 4);
	}
	if (found) hitY = origin[1] + best * scale;
	return found;
}

bool ChunkCollider::pushSphere(Vec3& center, float radius) const {
	if (!triangleCount) return false;
	const float inv = 1.0f / scale;
	Vec3 c = {(center.x - origin[0]) * inv, (center.y - origin[1]) * inv, (center.z - origin[2]) * inv};
	const float r = radius * inv;

	const float gx = c.x - float(gridX0), gz = c.z - float(gridZ0);
	const float fx0 = (gx - r) / cellSize, fx1 = (gx + r) / cellSize;
	const float fz0 = (gz - r) / cellSize, fz1 = (gz + r) / cellSize;
	if (fx1 < 0 || fz1 < 0 || fx0 >= float(gridW) || fz0 >= float(gridH)) return false;
	const u32 x0 = u32(max(fx0, 0.0f)), x1 = min(u32(max(fx1, 0.0f)), gridW - 1);
	const u32 z0 = u32(max(fz0, 0.0f)), z1 = min(u32(max(fz1, 0.0f)), gridH - 1);

	bool moved = false;
	for (u32 z = z0; z <= z1; ++z) {
		for (u32 x = x0; x <= x1; ++x) {
			const u32 cell = z * gridW + x;
			for (u32 i = cellStart[cell]; i < cellStart[cell + 1]; ++i) {
				const u16* t = triangles + cellItems[i] * 3;
				const Vec3 a = localVertex(positions, t[0]), b = localVertex(positions, t[1]),
				           d = localVertex(positions, t[2]);
				const Vec3 q = closestPointOnTriangle(c, a, b, d);
				const Vec3 away = c - q;
				const float dist2 = dot(away, away);
				if (dist2 >= r * r) continue;
				if (dist2 > 1e-8f) {
					const float dist = sqrtf(dist2);
					c = c + away * ((r - dist) / dist);
				} else {
					// Centre exactly on the surface: leave along the face normal.
					Vec3 n = normalize(cross(b - a, d - a));
					c = c + n * r;
				}
				moved = true;
			}
		}
	}
	if (moved) center = {origin[0] + c.x * scale, origin[1] + c.y * scale, origin[2] + c.z * scale};
	return moved;
}

} // namespace g3
