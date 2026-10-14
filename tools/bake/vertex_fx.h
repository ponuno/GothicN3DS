// What the baker puts into WorldVertex::w (g3ds/world.h): ambient occlusion
// ("Graphics+" G1) and how far a plant's corner sways in the wind (G2).
//
// Ambient occlusion: from every corner of the scene rays go out over the
// half of the sky its surface faces; the share that hits something nearby
// (weighted by how near) is how shut in the corner is. Corners of one smooth
// surface that share a place share the answer, so the chunker still merges
// them into one vertex. Computed once at bake time: nothing in the frame.
//
// Wind: the importer marks the objects that are plants by their names and
// gives each corner of them how far it may sway - nothing at the foot,
// most at the top. The runtime moves them in the vertex shader.
#pragma once
#include "scene.h"

#include <string>
#include <vector>

namespace g3 {

// ---- ambient occlusion ----

enum AoLevel : u8 { AO_OFF, AO_LOW, AO_HIGH };

struct AoOptions {
	AoLevel level = AO_OFF;
	unsigned threads = 1;
	// 0: from the level (low: 16 rays, 2.5 m; high: 40 rays, 4 m).
	u32 rays = 0;
	float radius = 0; // cm
	// Large triangles with something solid within the ray reach are cut into four (again, as far as `cell`) before
	// the rays are shot: a corner's answer is then no longer stretched over a whole floor tile (flat triangles of
	// light and shade beside a wall). 0: from the level (low 2 m, high 1.5 m); `maxExtra` the triangles it may add.
	float cell = 0; // cm
	u32 maxExtra = 0; // (0: no cutting; --ao-extra N switches it on)
};

struct AoStats {
	u64 corners = 0;  // corners that were given an answer
	u64 unique = 0;   // places + normals actually shot from (the rest share them)
	u64 rays = 0;
	u64 occluders = 0; // triangles rays can hit
	double seconds = 0;
	float mean = 0;   // average occlusion of the corners, 0..1
	u64 darkCorners = 0; // corners more than half shut in
	u32 added = 0;       // triangles the cutting of large ones near something made
	u32 rayCount = 0;
	float radius = 0;
};

// What a material does for the occlusion: `blocks` 0 (rays pass: water,
// glass, glow), 0.5 (cut-out: leaves, fences - half the rays would pass) or 1;
// `receives`: its corners are given an answer (not the translucent ones).
struct AoMaterial {
	float blocks = 1.0f;
	bool receives = true;
};

// Fills SrcVertex::occlusion of every triangle. Small objects (DETAIL_SMALL)
// receive but do not block: the menu may thin them out, and their shade
// would stay behind on the ground.
void bakeOcclusion(SrcScene& scene, const std::vector<AoMaterial>& materials, const AoOptions& opt, AoStats& stats);

// ---- wind ----

enum class PlantKind : u8 { None, Tree, Bush, Grass };

// Which kind of plant a name says (an object's visual, a material, a
// texture: "OW_LOB_TREE_V1.3DS", "NW_NATURE_BUSH_01", "GRASS_A0.TGA"). The
// name is cut into words at everything but letters and digits; a word that
// begins or ends with one of the plant words counts ("TREES", "BIGTREE" -
// not "STREET"). Words like STUMP or ROOT say it is not a plant after all.
// `extra`: more words for trees, from the command line (upper case).
PlantKind plantKindOf(const std::string& name, const std::vector<std::string>& extra = {});

// How far the top of a plant of this kind and height sways, cm.
float plantSway(PlantKind kind, float heightCm);

// Marks the corners of `count` triangles of one placed object as swaying:
// nothing at `footY`, `swayCm` at `topY`, in between a curve (a tree bends
// more near the top than near its foot).
void markSway(SrcTriangle* tris, size_t count, PlantKind kind, float footY, float topY, float swayCm);

} // namespace g3
