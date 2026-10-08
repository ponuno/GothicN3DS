// The bake pipeline: scenes in, pak entries out. Shared by the command-line
// tool and the tests.
#pragma once
#include "chunker.h"
#include "model_src.h"
#include "pak_writer.h"
#include "scene.h"
#include "script_src.h"
#include "texture_bake.h"

#include <functional>
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
	// With scripts:
	size_t characters = 0;  // put into the worlds by their start-up scripts
	size_t looks = 0;       // different looks among them
	size_t looksBaked = 0;  // of which with models
	size_t npcModels = 0;   // skeletons, bodies and heads baked for them
	bool scriptChecked = false, scriptSame = false; // against the reference interpreter
};

// Where the models of the worlds' inhabitants come from. Each call fills
// `model` (the baker names it) and returns false, with the reason in
// model.notes, if there is no such thing. Names are lower case, without
// extension, as the scripts give them ("humans", "hum_grdm_armor").
struct LookSource {
	// The skeleton of a model script with the animation a character stands
	// about in, and no mesh. `humanoid`: it has a head of its own.
	std::function<bool(const std::string& script, bool humanoid, SrcModel& model)> rig;
	// A body or an armour, in the texture variant `tex` and skin colour `skin`.
	std::function<bool(const std::string& script, const std::string& mesh, int tex, int skin, SrcModel& model)> body;
	std::function<bool(const std::string& script, const std::string& head, int tex, int skin, int teeth, SrcModel& model)> head;
};

// The game's scripts: baked into the archive, run to see who lives in the
// worlds, and their inhabitants' models baked along.
struct BakeScripts {
	const SrcScript* script = nullptr;
	// The compiled file the script was read from, if any: it is then checked
	// against the reference interpreter (script_check.h).
	const std::vector<u8>* dat = nullptr;
	LookSource looks; // without it nobody is drawn
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
// The same, plus the game's scripts and the characters they put into the worlds.
BakeResult bakeWorlds(std::vector<BakeWorld>& worlds, const std::vector<SrcModel>& models, const BakeScripts& scripts,
                      const BakeOptions& opt, PakWriter& pak);

// The name the scripts know a world by: "OldMine.ZEN" -> "OLDMINE".
std::string worldScriptName(const BakeWorld& world);

} // namespace g3
