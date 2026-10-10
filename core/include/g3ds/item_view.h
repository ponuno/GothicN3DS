// How a thing of the inventory is shown: turned so that it reads well, the
// same in the picture the baker makes of it ("icon/<visual>", the grid of the
// bottom screen) and in the 3D model the top screen turns in its info box.
//
// View space here: x to the right, y up, z away from the viewer.
#pragma once
#include "g3ds/base.h"
#include "g3ds/math.h"

namespace g3 {

// "icon/<visual>": IconHeader | RGBA8 rows from the top (w * h * 4 bytes).
constexpr u32 ITEMICON_MAGIC = fourcc('G', '3', 'I', 'C');
constexpr u32 ITEMICON_VERSION = 1;
constexpr int ITEMICON_SIZE = 48;

struct IconHeader {
	u32 magic;
	u32 version;
	u16 width, height;
	u32 reserved;
};
static_assert(sizeof(IconHeader) == 16, "IconHeader layout");

// The rotation (model space -> view space) a thing is shown in, from its
// bounding box. A long thing (a sword, a bow, an arrow) lies along the
// diagonal, rising to the right, its broad side to the viewer, as Gothic's
// inventory shows weapons; anything else keeps its up and is seen from a
// little above and to the side.
inline Mat34 itemShowcase(const float mn[3], const float mx[3]) {
	const float size[3] = {mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]};
	int order[3] = {0, 1, 2};
	for (int i = 0; i < 3; ++i)
		for (int k = i + 1; k < 3; ++k)
			if (size[order[k]] > size[order[i]]) {
				const int t = order[i];
				order[i] = order[k];
				order[k] = t;
			}
	float m[3][3] = {};
	if (size[order[0]] >= 1.8f * size[order[1]] && size[order[0]] > 0.0f) {
		const float d = 0.70710678f;
		const Vec3 along = {d, d, 0}, across = {-d, d, 0};
		// (A proper rotation: the third axis goes towards or away from the
		// viewer as the order of the first two demands.)
		const bool even = (order[1] - order[0] + 3) % 3 == 1;
		const Vec3 depth = even ? Vec3 {0, 0, 1} : Vec3 {0, 0, -1};
		const Vec3 cols[3] = {along, across, depth};
		for (int k = 0; k < 3; ++k) {
			m[0][order[k]] = cols[k].x;
			m[1][order[k]] = cols[k].y;
			m[2][order[k]] = cols[k].z;
		}
		// Tipped back a little, so that it has some depth.
		const float t = -0.35f, c = cosf(t), s = sinf(t);
		for (int k = 0; k < 3; ++k) {
			const float y = m[1][k], z = m[2][k];
			m[1][k] = c * y - s * z;
			m[2][k] = s * y + c * z;
		}
	} else {
		// Turned 35 degrees, seen 25 degrees from above.
		const float yaw = 0.6f, pitch = -0.45f; // (negative: the top towards the viewer)
		const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
		// Ry(yaw), then Rx(pitch) tipping the top towards the viewer.
		const float ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
		const float rx[3][3] = {{1, 0, 0}, {0, cp, -sp}, {0, sp, cp}};
		for (int r = 0; r < 3; ++r)
			for (int c2 = 0; c2 < 3; ++c2) m[r][c2] = rx[r][0] * ry[0][c2] + rx[r][1] * ry[1][c2] + rx[r][2] * ry[2][c2];
	}
	Mat34 o;
	for (int r = 0; r < 3; ++r) {
		for (int c2 = 0; c2 < 3; ++c2) o.m[r][c2] = m[r][c2];
		o.m[r][3] = 0.0f;
	}
	return o;
}

} // namespace g3
