// Fewer triangles for the characters' meshes (g3ds-bake --simplify; see lod.h for the world's).
//
// The same edge collapse as the world's, on the source mesh of each part before it is baked: an edge is
// collapsed onto one of its own ends, cheapest first by the quadric error, never at the border of a mesh
// and never where the surviving triangles would not look as they did - a vertex at the other end must have
// the texture coordinates and the normal the triangle itself has there, and the same bones with nearly the
// same weights, so that nothing is skinned differently from what was baked. Nothing new is made: the
// vertices of the result are vertices of the source. Parts that are cut out of a texture or blended stay.
#pragma once
#include "model_src.h"

#include <vector>

namespace g3 {

struct ModelSimplifyOptions {
	bool enabled = false;
	float ratio = 0.65f;     // triangles to keep, of what is simplified
	float maxError = 0.8f;   // cm: how far (mean, over the planes around a vertex) a collapse may move the surface
	u32 minTriangles = 160;  // a part with fewer is left alone (hands, small pieces of armour, teeth)
	bool synthesize = false; // where no vertex fits, make one (the coarse far bodies, --npc-lod); else only the source's are used
};

struct ModelSimplifyStats {
	u32 parts = 0, simplified = 0;
	u64 before = 0, after = 0;
};

// `parts` are the model's parts (SrcModelPart::verts: three vertices per triangle); they are replaced.
// `skip[i]`: part i stays as it is (its texture is cut out, say).
void simplifyModelParts(std::vector<SrcModelPart>& parts, const std::vector<bool>& skip, const ModelSimplifyOptions& opt,
                        ModelSimplifyStats& stats);

} // namespace g3
