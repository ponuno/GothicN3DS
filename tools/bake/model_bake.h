// Bakes an animated character: skeleton, skinned mesh, textures and clips.
#pragma once
#include "bake.h"
#include "model_src.h"

#include <map>
#include <set>
#include <string>

namespace g3 {

struct ModelBakeResult {
	std::string report;
	size_t parts = 0, vertices = 0, triangles = 0, clips = 0, textures = 0;
	size_t largestPalette = 0;
	std::vector<std::string> missingTextures;
};

// What several models baked into one archive share.
struct ModelBakeShared {
	struct Texture {
		u32 hash = 0; // pak entry, 0 if the texture is missing
		bool alpha = false;
	};
	std::map<std::string, Texture> textures; // by source name
};

// Adds "model/<name>", its clips ("anim/<name>/<clip>") and its textures to
// `pak`. A model may be a skeleton with clips and no mesh. Throws
// std::runtime_error on data that cannot be baked.
ModelBakeResult bakeModel(const SrcModel& model, const BakeOptions& opt, PakWriter& pak, ModelBakeShared* shared = nullptr);

// The inhabitants of the synthetic scene (see LookSource): figures like the
// test character in other clothes, and a four-legged creature.
bool buildSyntheticRig(const std::string& script, bool humanoid, const std::set<std::string>& wanted, SrcModel& model);
bool buildSyntheticBody(const std::string& script, const std::string& mesh, int tex, int skin, SrcModel& model);
bool buildSyntheticHead(const std::string& script, const std::string& head, int tex, int skin, int teeth, SrcModel& model);

// The test character of the synthetic scene: a jointed figure with the clips
// the hero needs (stand, run, walk, jump, fall, swim), named as in Gothic.
void buildSyntheticActor(SrcModel& model);

} // namespace g3
