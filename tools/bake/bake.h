// The bake pipeline: scenes in, pak entries out. Shared by the command-line
// tool and the tests.
#pragma once
#include "chunker.h"
#include "far_lod.h"
#include "model_src.h"
#include "pak_writer.h"
#include "scene.h"
#include "script_src.h"
#include "texture_bake.h"
#include "lod.h"
#include "model_simplify.h"
#include "visibility.h"
#include "vertex_fx.h"
#include "light_bake.h"

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace g3 {

struct BakeOptions {
	ChunkerOptions chunker;
	TexBakeOptions tex;
	unsigned threads = 1;
	bool quiet = false; // do not print the report while baking
	std::string musicDir;  // folder of the WAV renders of the music themes (empty: no music)
	u32 musicRate = 44100; // highest rate of the music (Hz)
	u32 voiceRate = 48000; // highest rate of the spoken lines (Hz): the recordings' own, unless lowered; 0: no speech.pak
	bool voiceIma = false; // the voices as IMA ADPCM (a quarter the size) instead of 16-bit samples
	u32 soundRate = 22050; // highest sample rate of the sounds (Hz); 0: no sound is baked
	std::string debugFile; // where the detailed diagnostic report goes (empty: none; "?": bake_debug.txt beside the output)
	// "Graphics+": ambient occlusion in the vertices (--ao) and the plants'
	// sway (--wind; the importer marks them, without it the marks are dropped).
	AoOptions ao;
	bool wind = false;
	// --probe X Y Z [--probe-radius M]: lists the triangles around that place (metres) in the report, at two stages of the
	// bake (as imported; after the light and occlusion) - to find where a flat patch seen in the game comes from.
	bool probe = false;
	float probeAt[3] = {0, 0, 0};
	float probeRadius = 25.0f;
	// Water is lit the same everywhere (--water-light): the vertex light of its polygons is a fixed blue-green tone of this
	// brightness (0..255) instead of what the source mesh has (big polygons, lightmapped per corner: patches of very
	// different brightness). 0 keeps the source's light.
	u8 waterLight = 200;
	// The water cut into a grid of this many cm along the shores (--water-grid; 0: not cut): fine enough for the runtime's
	// waves to lift it corner by corner (WORLDF_WATER_GRID; see water_grid.h). `waterReach`: how far from the shore line
	// (cm, in whole tiles of 16 cells, at least one) the fine water goes (--water-reach).
	float waterGrid = 0.0f;
	float waterReach = 3200.0f;
	// The light of the world's lamps (torches, fire places) baked into the vertices (--lights, light_bake.h).
	LightOptions light;
	// The far layer (G10, --far-lod): a coarse copy of the world drawn beyond the draw distance.
	FarOptions far;
	// Visibility (--vis): which parts of the world are hidden from which cell (see visibility.h).
	VisOptions vis;
	LodOptions lod;
	// Fewer triangles in everything (--simplify): the world's meshes in place (lod.h) and the characters' (model_simplify.h).
	SimplifyOptions simplify;
	ModelSimplifyOptions simplifyModels;
	// The inhabitants' bodies and heads keep their mesh in an entry of its own, shared by those that differ in their textures only.
	bool sharedMeshes = false;
	// --npc-lod: a second, coarse body for each character, shown beyond npcLodDistance and swapped for the full one when near.
	bool npcLod = false;
	// The triangles of every batch reordered for the vertex cache (--no-vcache: left as the chunker made them).
	bool vertexCache = true;
	ModelSimplifyOptions npcLodSimplify = {true, 0.3f, 6.0f, 60, true};
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
	// The skeleton of a model script with its animations, and no mesh: those
	// a character stands and goes about in, and of the names in `wanted`
	// (animations the scripts mention, upper case) the ones this model has.
	// A name ending in a star stands for every animation beginning like it
	// ("T_BENCH_*": all there is to using a bench).
	// `humanoid`: it has a head of its own.
	std::function<bool(const std::string& script, bool humanoid, const std::set<std::string>& wanted, SrcModel& model)> rig;
	// A body or an armour, in the texture variant `tex` and skin colour `skin`.
	std::function<bool(const std::string& script, const std::string& mesh, int tex, int skin, SrcModel& model)> body;
	std::function<bool(const std::string& script, const std::string& head, int tex, int skin, int teeth, SrcModel& model)> head;
	// A thing lying about in the worlds: the mesh of the visual its script
	// gives (lower case, without extension), one node. May be empty: the
	// things are placed, and not drawn.
	std::function<bool(const std::string& visual, SrcModel& model)> item;
	// A picture by the name the scripts give it ("FIRE_A0.TGA"); false if there is none.
	std::function<bool(const std::string& name, SrcImage& image)> texture;
	// An overlay of a model script ("humans", "HUMANS_MILITIA"): the skeleton and the overlay's own animations.
	std::function<bool(const std::string& base, const std::string& overlay, SrcModel& model)> overlay;
	// The overlays the game has for a model script.
	std::function<std::vector<std::string>(const std::string& base)> overlays;
};

// The game's scripts: baked into the archive, run to see who lives in the
// worlds, and their inhabitants' models baked along.
struct BakeScripts {
	const SrcScript* script = nullptr;
	// The compiled file the script was read from, if any: it is then checked
	// against the reference interpreter (script_check.h).
	const std::vector<u8>* dat = nullptr;
	// The fight AI tables' script (FIGHT.DAT), baked into "script/fight".
	// Without it the engine looks for the tables in `script`.
	const SrcScript* fight = nullptr;
	// The sounds (SFX.DAT: C_SFX) and the music themes (MUSIC.DAT: C_MUSICTHEME) are scripts of their own in the
	// game; baked into "script/sfx" and "script/music". Without them the engine looks in `script`.
	const SrcScript* sfx = nullptr;
	const SrcScript* music = nullptr;
	// The particle effects (PARTICLEFX.DAT) and the spells' effects (VISUALFX.DAT), likewise: "script/particlefx", "script/visualfx".
	const SrcScript* particles = nullptr;
	const SrcScript* visualfx = nullptr;
	// The bytes of a sound file of the game by name ("DOOR_OPEN.WAV"); without it no sound is baked.
	std::function<bool(const std::string& file, std::vector<u8>& bytes)> soundFile;
	PakWriter* musicPak = nullptr; // where the music goes (bakeMusic); null: none
	LookSource looks; // without it nobody is drawn
};

// "OldMine.ZEN" -> "oldmine"
std::string worldNameFromLevel(const std::string& level);
// The archive name of a texture: "Foo_Bar.TGA" -> "tex/foo_bar".
std::string texturePakName(const std::string& texture);

// Bakes every world into `pak`. Textures are shared between the worlds, and
// portals are resolved against the worlds of this call. The first world is
// the one the game starts in. Throws std::runtime_error on failure.
// Gives the water's triangles the one light all over (BakeOptions::waterLight); what is not water, and the daylight weight, stay.
// Returns how many triangles it set. (0 as the brightness: nothing is changed.)
size_t applyWaterLight(SrcScene& scene, u8 brightness);
// The colour that light has, as a share of the white (the far layer's water is the texture's average in it).
void waterTint(u8 brightness, float rgb[3]);

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
