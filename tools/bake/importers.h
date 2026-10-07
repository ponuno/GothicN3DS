// Scene sources for the baker.
#pragma once
#include "scene.h"

#include <string>

namespace g3 {

struct SynthOptions {
	int cells = 176;         // terrain is cells x cells quads
	float cellSize = 500.0f; // cm
	int huts = 36;
	int trees = 900;
};

void buildSyntheticScene(const SynthOptions& opt, SrcScene& scene);

struct ZenImportOptions {
	std::string gameDir;   // Gothic installation (contains Data/*.vdf)
	std::string zenName = "WORLD.ZEN";
	std::string meshFile;  // alternative: a standalone .MSH file ...
	std::string textureDir; // ... with loose *-C.TEX files in this directory
	// ZenKit hands the world mesh over with front faces already counter-clockwise
	// in our convention (confirmed on the Steam release of Gothic 1), so the
	// default is to keep it. The baker's winding check warns if that is wrong.
	bool flipWinding = false;
	bool objects = true;   // bake static VObs (trees, bridges, huts, ...) into the world
	bool lightmaps = true; // bake indoor lightmaps into the vertex light
};

// Throws std::runtime_error with a readable message on failure.
void importZenWorld(const ZenImportOptions& opt, SrcScene& scene);

} // namespace g3
