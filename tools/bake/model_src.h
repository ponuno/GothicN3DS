// Importer-neutral description of an animated character. Importers (ZenGin
// model files, the synthetic test character) fill one of these; the model
// baker never sees where the data came from.
//
// Conventions: Y up, the character stands on y = 0 and faces +Z; rotations
// are unit quaternions (x y z w) that rotate column vectors.
#pragma once
#include "scene.h"

#include <functional>
#include <string>
#include <vector>

namespace g3 {

struct SrcNode {
	std::string name;
	int parent = -1;
	float pos[3] = {0, 0, 0}; // transform relative to the parent, at rest
	float rot[4] = {0, 0, 0, 1};
};

struct SrcSkinVertex {
	float pos[3]; // model space, rest pose
	float normal[3];
	float uv[2];
	int bone[2];       // node indices; the same twice for a rigid vertex
	float weight1 = 0; // share of bone[1], 0..1
};

struct SrcModelPart {
	SrcMaterial material;
	std::vector<SrcSkinVertex> verts; // three per triangle, counter-clockwise seen from the front
};

struct SrcClipSample {
	float pos[3];
	float rot[4];
};

struct SrcClip {
	std::string name;
	std::string next; // the clip that follows; the clip's own name for a loop
	float fps = 25.0f;
	int layer = 1;
	float blendIn = 0.1f, blendOut = 0.1f; // seconds
	bool moves = false;     // the root node travels: the game moves the character by that much instead
	bool fly = false;       // marked as airborne in the model script
	std::vector<int> nodes; // animated nodes
	std::vector<SrcClipSample> samples; // frame after frame, one sample per animated node
	// The model script's event tags that matter in a fight (g3ds/model.h,
	// ClipFight), in frames of this clip.
	std::vector<int> hits, hitEnds, windows, parry;
	int fightModeFrame = -1;
	// The model script's sound tags (*eventSFX, *eventSFXGrnd), in frames of this clip.
	struct Sfx {
		int frame = 0;
		bool ground = false;
		bool emptySlot = false;
		int item = 0; // fix154: 0 a sound; else ClipSfxKind (CLIP_ITEM_*): an item tag, `name` is the item
		float range = 0;
		std::string name;
	};
	std::vector<Sfx> sfx;
	bool fighting() const { return !hits.empty() || !hitEnds.empty() || !windows.empty() || !parry.empty() || fightModeFrame >= 0; }
};

// "aniBlend": a fade standing in for a transition animation.
struct SrcAniBlend {
	std::string name; // "T_RUNL_2_WALKL"
	std::string next; // "S_WALKL"
	float blendIn = 0.1f, blendOut = 0.1f;
};

struct SrcModel {
	std::string name; // asset name: stored as "model/<name>"
	std::vector<SrcNode> nodes;
	std::vector<SrcModelPart> parts;
	std::vector<SrcClip> clips;
	std::vector<SrcAniBlend> blends;
	std::function<bool(const std::string& name, SrcImage& out)> loadTexture;
	std::vector<std::string> notes; // importer remarks for the bake report
	// How the importer read the animations (ZenGin: conjugated rotations, quarter turns to face +Z); -1: not decided.
	// An overlay of the same skeleton is read the same way.
	int conjugated = -1, quarters = -1;
};

} // namespace g3
