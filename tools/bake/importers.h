// Scene sources for the baker.
#pragma once
#include "scene.h"

#include <memory>
#include <string>
#include <vector>

namespace g3 {

struct SynthOptions {
	int cells = 176;         // terrain is cells x cells quads
	float cellSize = 500.0f; // cm
	int huts = 36;
	int trees = 900;
};

void buildSyntheticScene(const SynthOptions& opt, SrcScene& scene);

// A cave with a pool, reached through the portal of the main synthetic scene.
void buildSyntheticCave(SrcScene& scene);

struct ZenImportOptions {
	std::string zenName = "WORLD.ZEN"; // world file inside the game's archives
	std::string meshFile;   // importZenMesh: a standalone .MSH file ...
	std::string textureDir; // ... with loose *-C.TEX files in this directory
	// ZenKit hands the world mesh over with front faces already counter-clockwise
	// in our convention (confirmed on the Steam release of Gothic 1), so the
	// default is to keep it. The baker's winding check warns if that is wrong.
	bool flipWinding = false;
	bool objects = true;   // bake world objects (trees, bridges, chests, decals, ...) into the world
	bool lightmaps = true; // bake indoor lightmaps into the vertex light
};

// A mounted Gothic installation (its Data/*.vdf archives).
struct ZenGame;

// All of these throw std::runtime_error with a readable message on failure.
std::shared_ptr<ZenGame> openZenGame(const std::string& gameDir, std::vector<std::string>& notes);
bool zenGameHasFile(const ZenGame& game, const std::string& name);
void importZenWorld(std::shared_ptr<const ZenGame> game, const ZenImportOptions& opt, SrcScene& scene);
// Development path: a loose mesh plus a directory of compiled textures.
void importZenMesh(const ZenImportOptions& opt, SrcScene& scene);

} // namespace g3
