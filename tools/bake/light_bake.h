// The light of the lamps the world places (torches, fire places, lamps: zCVobLight) baked into the vertices'
// light, so that a torch lights the wall and the ground around it - with shadows: a ray from the corner to the lamp.
//
// The vertex light is rgb (what the corner gets all day) and a daylight weight w: the picture is rgb * (1 + w (day - 1)),
// day being the time of day's colour. A lamp's light must not go dark with the evening, so it goes into rgb and takes
// from w what it is worth: rgb' = rgb + lamp, w' = w * |rgb| / |rgb + lamp|; at night only the lamp is left (and
// what the vertex had of its own beyond the sun). (That is exact for the grey part; a coloured lamp is near.)
//
// Polygons of the source are large and light is per corner, so the large triangles within a lamp's reach are cut
// into smaller ones first (`cell`), up to `maxExtra` more triangles in all.
#pragma once
#include "vertex_fx.h"

namespace g3 {

struct LightOptions {
	bool enabled = false;
	float strength = 1.0f;     // the lamps' light as it is; more or less
	float cell = 150.0f;       // cm: the edge the triangles near a lamp are cut down to
	u32 maxExtra = 60000;      // triangles it may add, in all
	bool statics = false;      // the lamps whose light the world already has too
	unsigned threads = 1;
};

struct LightStats {
	u32 lamps = 0;          // that were baked
	u32 skippedStatic = 0;
	u32 added = 0;          // triangles the cutting made
	u64 corners = 0;        // corners that got light
	u64 rays = 0;
	double seconds = 0;
};

// What of a corner's light a lamp gives: 0..1 by distance (smooth to nothing at `range`) and the angle of the surface to it
// (`facing` = cosine, a little of it wraps round). Exposed for the test.
float lampLight(float distance, float range, float facing);

void bakeLights(SrcScene& scene, const std::vector<AoMaterial>& materials, const LightOptions& opt, LightStats& stats);

} // namespace g3
