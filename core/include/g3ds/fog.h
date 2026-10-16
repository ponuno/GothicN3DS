// Pak asset "world/<name>/fog" (ASSET_META): the world's fog zones (zCZoneZFog), for fog and water that follow the region.
//   FogZonesHeader | FogZone[count]
// A zone is a box (world cm). The fog of the camera is the blend of the zones it stands in, each by FogZone weight; the
// baker also tints the water by the zone it lies in (--water-biome).
#pragma once

#include "g3ds/base.h"

#include <algorithm>

namespace g3 {

constexpr u32 FOG_ZONES_MAGIC = fourcc('G', '3', 'F', 'Z');
constexpr u32 FOG_ZONES_VERSION = 1;

struct FogZonesHeader {
	u32 magic;
	u32 version;
	u32 count;
	u32 reserved;
};
static_assert(sizeof(FogZonesHeader) == 16, "FogZonesHeader layout");

enum : u8 { FOGZ_OVERRIDE_COLOR = 1, FOGZ_FADE_SKY = 2 };

struct FogZone {
	float bbMin[3], bbMax[3];
	float range;     // cm: the fog ends this far from the eye inside the zone (0: the world's own)
	float inner;     // 0..1: the share of the half-width (to the middle) where the zone is whole; the rest is the fade from the edge
	u8 color[3];     // fog colour
	u8 flags;        // FOGZ_*
};
static_assert(sizeof(FogZone) == 36, "FogZone layout");

// 0..1: how much of the zone is on a point (smooth from its edge in; x and z only, the height must be inside the box).
inline float fogZoneWeight(const FogZone& z, float x, float y, float zz) {
	if (y < z.bbMin[1] - 200.0f || y > z.bbMax[1] + 200.0f) return 0.0f;
	const float dx = std::min(x - z.bbMin[0], z.bbMax[0] - x), dz = std::min(zz - z.bbMin[2], z.bbMax[2] - zz);
	const float d = std::min(dx, dz);
	if (d <= 0.0f) return 0.0f;
	const float half = 0.5f * std::min(z.bbMax[0] - z.bbMin[0], z.bbMax[2] - z.bbMin[2]);
	const float ramp = std::max(half * (1.0f - std::clamp(z.inner, 0.0f, 1.0f)), 100.0f);
	const float t = std::clamp(d / ramp, 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

} // namespace g3
