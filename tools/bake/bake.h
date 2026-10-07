// The bake pipeline: scenes in, pak entries out. Shared by the command-line
// tool and the tests.
#pragma once
#include "chunker.h"
#include "model_src.h"
#include "pak_writer.h"
#include "scene.h"
#include "texture_bake.h"

#include <string>
#include <vector>

namespace g3 {

struct BakeOptions {
	ChunkerOptions chunker;
	TexBakeOptions tex;
	unsigned threads = 1;
	bool quiet = false; // do not print the report while baking
};

struct BakeWorld {
	std::string name;  // asset name: the world is stored as "world/<name>"
	std::string level; // source file name ("OLDMINE.ZEN"): what portals refer to
	SrcScene scene;
};

struct BakeResult {
	std::string report;
	size_t textures = 0;
	size_t chunks = 0;
	size_t models = 0;
};

// "OldMine.ZEN" -> "oldmine"
std::string worldNameFromLevel(const std::string& level);
// The archive name of a texture: "Foo_Bar.TGA" -> "tex/foo_bar".
std::string texturePakName(const std::string& texture);

// Bakes every world into `pak`. Textures are shared between the worlds, and
// portals are resolved against the worlds of this call. The first world is
// the one the game starts in. Throws std::runtime_error on failure.
BakeResult bakeWorlds(std::vector<BakeWorld>& worlds, const BakeOptions& opt, PakWriter& pak);
// The same, plus animated characters. The first model is the player's
// ("meta/hero" names it); its presence switches the game to third person.
BakeResult bakeWorlds(std::vector<BakeWorld>& worlds, const std::vector<SrcModel>& models, const BakeOptions& opt,
                      PakWriter& pak);

} // namespace g3
