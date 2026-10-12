// Scene sources for the baker.
#pragma once
#include "bake.h"
#include "model_src.h"
#include "scene.h"
#include "script_src.h"

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

// The script that populates the two synthetic worlds, written in Daedalus and
// compiled by this project's own compiler (daedalus.h).
const char* syntheticScriptSource();
void buildSyntheticScript(SrcScript& script);
// Its fight AI tables, a script of their own as the game's (FIGHT.DAT).
const char* syntheticFightScriptSource();
void buildSyntheticFightScript(SrcScript& script);
// Its sounds (SFX.DAT: C_SFX) and music themes (MUSIC.DAT: C_MUSICTHEME), scripts of their own as the game's.
void buildSyntheticSfxScript(SrcScript& script);
void buildSyntheticMusicScript(SrcScript& script);

// What the people of the synthetic worlds say (the lines their dialogues name).
void buildSyntheticDialogue(SrcDialogue& dialogue);

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

// A Gothic 2 modification (Chronicles of Myrtana: Archolos, ...): an .ini of the
// System folder naming its world, its compiled scripts and its archives, and
// the .mod archives in Data/ that lie over the game's own .vdf archives.
struct ZenModOptions {
	// The mod's .ini: a path, or a name looked up in <game>/System ("Archolos" or
	// "Archolos.ini"). Empty: no .ini; then only `archives` (or, with `all`, every .mod) count.
	std::string ini;
	// Archives to mount over the game: paths, or names looked up under <game>/Data.
	std::vector<std::string> archives;
	// Mount every .mod under Data/, whatever the .ini lists (what OpenGothic does for a mod
	// whose .ini has no VDF line, as Archolos' has not).
	bool all = false;
	bool active() const { return !ini.empty() || !archives.empty() || all; }
};

// What the mod says about itself (empty fields: it does not say).
struct ZenModInfo {
	bool active = false;
	std::string title;
	std::string world;        // file name of the world the mod starts in ("ARCHOLOS.ZEN")
	std::string gameDat;      // its compiled scripts ("ARCHOLOS.DAT"); GOTHIC.DAT if the .ini does not name them
	std::string outputUnits;  // its dialogue library without extension ("OU")
	std::vector<std::string> mounted;    // the archives mounted over the game, in order
	std::vector<std::string> worlds;     // .ZEN files the mod's archives bring, which the game does not have
};

// All of these throw std::runtime_error with a readable message on failure.
std::shared_ptr<ZenGame> openZenGame(const std::string& gameDir, std::vector<std::string>& notes, const ZenModOptions& mod = {});
// The mod of an opened game (`active` is false for a game without one).
const ZenModInfo& zenGameMod(const ZenGame& game);
bool zenGameHasFile(const ZenGame& game, const std::string& name);
// Writes the files whose extensions are in `exts` (".sgt", lower case) - those of the mounted archives
// and the loose ones of _work/Data/Music - flat into `outDir`, the way tools that read a folder want
// them (dmrender). Returns how many files were written; `notes` gets what went wrong.
size_t zenExtractFiles(const ZenGame& game, const std::vector<std::string>& exts, const std::string& outDir,
                       std::vector<std::string>& notes);
// Every .ZEN file of the mounted archives with its size, the biggest first.
std::vector<std::pair<std::string, size_t>> zenGameWorlds(const ZenGame& game);
// Its size in bytes (the file the mounted archives show), 0 if there is none.
size_t zenGameFileSize(const ZenGame& game, const std::string& name);
void importZenWorld(std::shared_ptr<const ZenGame> game, const ZenImportOptions& opt, SrcScene& scene);
// Development path: a loose mesh plus a directory of compiled textures.
void importZenMesh(const ZenImportOptions& opt, SrcScene& scene);

// The player's character, as the game's start script dresses it.
struct ZenActorOptions {
	std::string name = "hero";          // asset name of the baked model
	std::string script = "HUMANS.MDS";  // model script: skeleton and animations
	std::string body = "HUM_BODY_NAKED0";
	int bodyVariant = 4; // texture variant ("_V4") and skin colour ("_C1") of the body ...
	int skinColor = 1;
	std::string head = "HUM_HEAD_PONY";
	int headVariant = 9; // ... and of the head
	int teethVariant = 0;
	std::vector<std::string> clips; // animations to bake; empty: what the hero's movement needs ...
	bool heroClips = true;          // ... unless this is false: then exactly `clips`, which may be none
	// `clips` is a wish list: what the model script does not declare is
	// passed over without a word, and what a clip leads into comes along.
	bool onlyDeclared = false;
	// Every animation the model script declares whose name begins with one
	// of these is wanted too ("T_BENCH_": all there is to using a bench).
	std::vector<std::string> clipPrefixes;
	// `body` and `head` may be empty: a head alone, a body alone, or - with
	// neither - just the skeleton and its animations.
};

// Fills `model` from the game's files. Returns false, with the reason in
// model.notes, if the game does not have them (the bake then goes on without a
// character). Throws std::runtime_error on files that exist but cannot be used.
bool importZenActor(std::shared_ptr<const ZenGame> game, const ZenActorOptions& opt, SrcModel& model);

// The game's sound files by name ("DOOR_OPEN.WAV"), for BakeScripts::soundFile.
std::function<bool(const std::string&, std::vector<u8>&)> zenSoundSource(std::shared_ptr<const ZenGame> game);

// The pieces of the inhabitants' looks (bake.h), from the game's files.
LookSource zenLookSource(std::shared_ptr<const ZenGame> game);

// The game's own camera set-ups, from its compiled scripts (CAMERA.DAT).
// Returns false and leaves this project's defaults in `set` if they cannot be
// read; either way a line for the bake report goes to `notes`.
struct CameraSet;
bool importZenCamera(std::shared_ptr<const ZenGame> game, CameraSet& set, std::vector<std::string>& notes);

// The game's compiled scripts (GOTHIC.DAT): the symbols and the code, and the
// file itself for the check against the reference interpreter. Returns false,
// with a line for the bake report in `notes`, if they cannot be read.
bool importZenScript(std::shared_ptr<const ZenGame> game, SrcScript& script, std::vector<u8>& dat,
                     std::vector<std::string>& notes);
// The fight AI tables' script (FIGHT.DAT: Gothic 1 compiles Fight.src into a
// file of its own). Returns false, with a line in `notes`, if it cannot be read.
bool importZenFightScript(std::shared_ptr<const ZenGame> game, SrcScript& script, std::vector<std::string>& notes);
// The sounds (SFX.DAT, the C_SFX instances) and the music themes (MUSIC.DAT, C_MUSICTHEME): Gothic compiles
// each into a file of its own, not into GOTHIC.DAT. False, with a line in `notes`, if one cannot be read.
// The particle effects (PARTICLEFX.DAT: C_PARTICLEFX) and the visual effects of the spells (VISUALFX.DAT: CFx_Base).
bool importZenParticleScript(std::shared_ptr<const ZenGame> game, SrcScript& script, std::vector<std::string>& notes);
bool importZenVisualFxScript(std::shared_ptr<const ZenGame> game, SrcScript& script, std::vector<std::string>& notes);
bool importZenSfxScript(std::shared_ptr<const ZenGame> game, SrcScript& script, std::vector<std::string>& notes);
bool importZenMusicScript(std::shared_ptr<const ZenGame> game, SrcScript& script, std::vector<std::string>& notes);

// The game's fonts (Fonts.vdf): the one of the text and the one of the titles.
// False, with a line in `notes`, if they are not in the archives.
struct SrcFonts;
bool importZenFonts(std::shared_ptr<const ZenGame> game, SrcFonts& fonts, std::vector<std::string>& notes);

// The words people say (OU.BIN). Returns false, with a line in `notes`, if the
// game has none.
bool importZenDialogue(std::shared_ptr<const ZenGame> game, SrcDialogue& dialogue, std::vector<std::string>& notes);

} // namespace g3
