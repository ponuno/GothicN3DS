// Bakes an animated character: skeleton, skinned mesh, textures and clips.
#pragma once
#include "bake.h"
#include "model_src.h"

#include <string>

namespace g3 {

struct ModelBakeResult {
	std::string report;
	size_t parts = 0, vertices = 0, triangles = 0, clips = 0, textures = 0;
	size_t largestPalette = 0;
};

// Adds "model/<name>", its clips ("anim/<name>/<clip>") and its textures to
// `pak`. Throws std::runtime_error on data that cannot be baked.
ModelBakeResult bakeModel(const SrcModel& model, const BakeOptions& opt, PakWriter& pak);

// The test character of the synthetic scene: a jointed figure with the clips
// the hero needs (stand, run, walk, jump, fall, swim), named as in Gothic.
void buildSyntheticActor(SrcModel& model);

} // namespace g3
