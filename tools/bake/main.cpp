// g3ds-bake: turns scenes (the worlds of a Gothic installation or the
// built-in synthetic test scene) into data.pak for the 3DS runtime.
#include "bake.h"
#include "g3ds/model.h"
#include "g3ds/tex.h"
#include "importers.h"
#include "model_bake.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace g3;

namespace {

// Upper-case file name without any directory: how the archives index files.
std::string levelFile(std::string s) {
	const size_t slash = s.find_last_of("/\\");
	if (slash != std::string::npos) s = s.substr(slash + 1);
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::toupper(c)); });
	return s;
}

int info(const char* path) {
	Pak pak;
	if (!pak.open(path)) {
		std::fprintf(stderr, "error: cannot open '%s' as a G3PK archive\n", path);
		return 1;
	}
	static const char* typeNames[] = {"blob", "texture", "world", "chunk", "meta", "model", "clip", "script"};
	u64 packed[8] = {}, raw[8] = {};
	u32 counts[8] = {};
	for (u32 i = 0; i < pak.count(); ++i) {
		const PakEntry& e = pak.entry(i);
		const u32 t = e.type < 8 ? e.type : 0;
		++counts[t];
		packed[t] += e.packedSize;
		raw[t] += e.size;
	}
	std::printf("%s: %u entries\n", path, pak.count());
	for (int t = 0; t < 8; ++t) {
		if (counts[t])
			std::printf("  %-8s %5u  %8.2f MB raw  %8.2f MB packed\n", typeNames[t], counts[t], double(raw[t]) / 1048576.0,
			            double(packed[t]) / 1048576.0);
	}
	for (const char* meta : {"meta/start", "meta/report"}) {
		const PakEntry* e = pak.find(meta);
		if (!e) continue;
		std::vector<char> text(e->size + 1, 0);
		if (!pak.read(*e, text.data())) continue;
		if (std::string(meta) == "meta/start")
			std::printf("start world: %s\n", text.data());
		else
			std::printf("\n%s", text.data());
	}
	return 0;
}

void usage() {
	std::puts("g3ds-bake - asset baker for the Gothic 3DS runtime\n"
	          "\n"
	          "  g3ds-bake world --game DIR [--zen NAME.ZEN ...] [options]   bake the worlds of a Gothic install\n"
	          "  g3ds-bake mesh --msh FILE [--textures DIR] [options]        bake a loose .MSH (development)\n"
	          "  g3ds-bake synth [--cells N] [options]                       bake the built-in test scene\n"
	          "  g3ds-bake info FILE                                         describe a data.pak\n"
	          "\n"
	          "world: the first --zen is the world the game starts in (default: WORLD.ZEN, or\n"
	          "NEWWORLD.ZEN for Gothic 2). Worlds reached through its level-change triggers\n"
	          "(the mines, the temple, ...) are baked too. So are the game's scripts (GOTHIC.DAT):\n"
	          "they are checked against the reference interpreter and run to see who lives in\n"
	          "the worlds, and those characters' models are baked along.\n"
	          "\n"
	          "options:\n"
	          "  -o FILE            output archive (default: data.pak)\n"
	          "  --name NAME        asset name of the first world (default: from the file name)\n"
	          "  --no-follow        world: bake only the worlds named with --zen\n"
	          "  --chunk CM         chunk edge length (default: 4000)\n"
	          "  --cluster CM       edge of the boxes a chunk is cut into for culling (default: 1000; 0: off)\n"
	          "  --step CM          finest vertex position step (default: 0.125)\n"
	          "  --seal CM          close seams: weld vertices and cut edges at vertices resting on\n"
	          "                     them, within this distance (default: 0.5)\n"
	          "  --no-seal          leave seams as they are in the source\n"
	          "  --max-tex N        longest texture edge, power of two, 8..1024 (default: 128)\n"
	          "  --quality 0|1|2    ETC1 effort (default: 1)\n"
	          "  --flip             reverse the triangle winding (if the winding check complains)\n"
	          "  --no-hero          do not bake the player's character (the game stays first-person)\n"
	          "  --no-scripts       do not bake the game's scripts (the worlds stay empty)\n"
	          "  --hero-script NAME world: the character's model script (default: HUMANS.MDS)\n"
	          "  --hero-body NAME   world: its body mesh (default: HUM_BODY_NAKED0)\n"
	          "  --hero-head NAME   world: its head mesh (default: HUM_HEAD_PONY; \"\" for none)\n"
	          "  --hero-skin B,C,H  world: texture variants of body, skin colour and head (default: 4,1,9)\n"
	          "  --no-objects       world mesh only, skip trees, bridges and other world objects\n"
	          "  --no-lightmaps     keep the vertex light on lightmapped (indoor) polygons\n"
	          "  --threads N        worker threads");
}

} // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		usage();
		return 2;
	}
	const std::string cmd = argv[1];
	try {
		if (cmd == "info") {
			if (argc != 3) {
				usage();
				return 2;
			}
			return info(argv[2]);
		}
		if (cmd != "world" && cmd != "mesh" && cmd != "synth") {
			usage();
			return 2;
		}

		BakeOptions opt;
		opt.threads = std::max(1u, std::thread::hardware_concurrency());
		ZenImportOptions zen;
		ZenActorOptions actor;
		SynthOptions synth;
		std::string out = "data.pak", firstName, gameDir;
		std::vector<std::string> levels;
		bool follow = true, hero = true, withScripts = true;
		for (int i = 2; i < argc; ++i) {
			const std::string a = argv[i];
			auto value = [&]() -> std::string {
				if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
				return argv[++i];
			};
			if (a == "-o") out = value();
			else if (a == "--name") firstName = value();
			else if (a == "--chunk") opt.chunker.chunkSize = std::stof(value());
			else if (a == "--cluster") opt.chunker.clusterSize = std::stof(value());
			else if (a == "--step") opt.chunker.positionStep = std::stof(value());
			else if (a == "--seal") opt.chunker.sealTolerance = std::stof(value());
			else if (a == "--no-seal") opt.chunker.seal = false;
			else if (a == "--max-tex") opt.tex.maxSize = std::stoi(value());
			else if (a == "--quality") opt.tex.quality = std::stoi(value());
			else if (a == "--threads") opt.threads = unsigned(std::max(1, std::stoi(value())));
			else if (a == "--game") gameDir = value();
			else if (a == "--zen") levels.push_back(levelFile(value()));
			else if (a == "--no-follow") follow = false;
			else if (a == "--no-hero") hero = false;
			else if (a == "--no-scripts") withScripts = false;
			else if (a == "--hero-script") actor.script = value();
			else if (a == "--hero-body") actor.body = value();
			else if (a == "--hero-head") actor.head = value();
			else if (a == "--hero-skin") {
				const std::string v = value();
				if (std::sscanf(v.c_str(), "%d,%d,%d", &actor.bodyVariant, &actor.skinColor, &actor.headVariant) != 3)
					throw std::runtime_error("--hero-skin wants BODY,COLOUR,HEAD, e.g. 4,1,9");
			}
			else if (a == "--msh") zen.meshFile = value();
			else if (a == "--textures") zen.textureDir = value();
			else if (a == "--no-objects") zen.objects = false;
			else if (a == "--no-lightmaps") zen.lightmaps = false;
			else if (a == "--flip") zen.flipWinding = true;
			else if (a == "--no-flip") zen.flipWinding = false; // the default; kept for old command lines
			else if (a == "--cells") synth.cells = std::stoi(value());
			else throw std::runtime_error("unknown option " + a);
		}
		const int mt = opt.tex.maxSize;
		if (mt < int(TEX_MIN_SIZE) || mt > int(TEX_MAX_SIZE) || (mt & (mt - 1)))
			throw std::runtime_error("--max-tex must be a power of two between 8 and 1024");
		if (opt.chunker.chunkSize < 500.0f) throw std::runtime_error("--chunk must be at least 500");
		if (opt.chunker.clusterSize != 0.0f && !(opt.chunker.clusterSize >= 200.0f && opt.chunker.clusterSize <= 100000.0f))
			throw std::runtime_error("--cluster must be 0 (off) or at least 200");
		if (!(opt.chunker.positionStep >= 0.01f && opt.chunker.positionStep <= 8.0f))
			throw std::runtime_error("--step must be between 0.01 and 8 (cm)");
		if (!(opt.chunker.sealTolerance >= 0.0f && opt.chunker.sealTolerance <= 5.0f))
			throw std::runtime_error("--seal must be between 0 and 5 (cm)");
		if (synth.cells < 8 || synth.cells > 1024) throw std::runtime_error("--cells must be between 8 and 1024");

		const auto t0 = std::chrono::steady_clock::now();
		std::vector<BakeWorld> worlds;
		std::vector<SrcModel> models;
		CameraSet camera = defaultCameraSet();
		bool haveCamera = false;
		SrcScript script, fightScript;
		std::vector<u8> scriptDat;
		BakeScripts scripts;
		if (cmd == "synth") {
			if (withScripts) {
				buildSyntheticScript(script);
				buildSyntheticFightScript(fightScript);
				scripts.script = &script;
				scripts.fight = &fightScript;
				scripts.looks = LookSource {buildSyntheticRig, buildSyntheticBody, buildSyntheticHead, buildSyntheticItem};
			}
			if (hero) {
				models.emplace_back();
				buildSyntheticActor(models.back());
			}
			worlds.resize(2);
			worlds[0].name = "world";
			worlds[0].level = "WORLD.ZEN";
			buildSyntheticScene(synth, worlds[0].scene);
			worlds[1].name = "cave";
			worlds[1].level = "CAVE.ZEN";
			buildSyntheticCave(worlds[1].scene);
		} else if (cmd == "mesh") {
			if (zen.meshFile.empty()) throw std::runtime_error("mesh: --msh FILE is required");
			worlds.resize(1);
			worlds[0].name = "world";
			importZenMesh(zen, worlds[0].scene);
		} else {
			if (gameDir.empty()) throw std::runtime_error("world: --game DIR is required");
			std::vector<std::string> notes;
			const std::shared_ptr<ZenGame> game = openZenGame(gameDir, notes);
			if (levels.empty()) {
				for (const char* candidate : {"WORLD.ZEN", "NEWWORLD.ZEN"})
					if (levels.empty() && zenGameHasFile(*game, candidate)) levels.push_back(candidate);
				if (levels.empty()) throw std::runtime_error("no WORLD.ZEN or NEWWORLD.ZEN in the archives; name one with --zen");
			}
			// Bake the named worlds, then whatever their portals lead to.
			for (size_t i = 0; i < levels.size() && i < 32; ++i) {
				BakeWorld bw;
				bw.level = levels[i];
				bw.name = worldNameFromLevel(levels[i]);
				zen.zenName = levels[i];
				try {
					importZenWorld(game, zen, bw.scene);
				} catch (const std::exception& ex) {
					if (i == 0) throw;
					std::printf("WARNING: %s skipped: %s\n", levels[i].c_str(), ex.what());
					continue;
				}
				if (i == 0) bw.scene.notes.insert(bw.scene.notes.begin(), notes.begin(), notes.end());
				if (follow)
					for (const SrcPortal& p : bw.scene.portals) {
						const std::string level = levelFile(p.level);
						if (std::find(levels.begin(), levels.end(), level) == levels.end() && zenGameHasFile(*game, level))
							levels.push_back(level);
					}
				worlds.push_back(std::move(bw));
			}
			// The player's character, if the game's files have it.
			if (hero && !worlds.empty()) {
				// Whatever is wrong with it must not cost the worlds their bake.
				SrcModel model;
				bool have = false;
				try {
					have = importZenActor(game, actor, model);
				} catch (const std::exception& ex) {
					model.notes.push_back(std::string("WARNING: the character could not be read (") + ex.what() +
					                      "); none baked, the game stays first-person");
				}
				if (have)
					models.push_back(std::move(model));
				else
					worlds[0].scene.notes.insert(worlds[0].scene.notes.end(), model.notes.begin(), model.notes.end());
				// ... and the game's own way of looking at it.
				importZenCamera(game, camera, worlds[0].scene.notes);
				haveCamera = true;
			}
			// The game's scripts, and through them the people of its worlds.
			if (withScripts && !worlds.empty() && importZenScript(game, script, scriptDat, worlds[0].scene.notes)) {
				scripts.script = &script;
				scripts.dat = &scriptDat;
				scripts.looks = zenLookSource(game);
				// Gothic 1 keeps how its people fight in a file of its own.
				if (importZenFightScript(game, fightScript, worlds[0].scene.notes)) scripts.fight = &fightScript;
			}
		}
		if (!firstName.empty()) worlds[0].name = firstName;

		PakWriter pak;
		bakeWorlds(worlds, models, scripts, opt, pak);
		if (haveCamera) {
			const u8* bytes = reinterpret_cast<const u8*>(&camera);
			pak.add("meta/camera", ASSET_META, std::vector<u8>(bytes, bytes + sizeof(camera)));
		}
		pak.write(out);
		const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		std::printf("wrote %s: %zu worlds, %zu entries in %.1f s\n", out.c_str(), worlds.size(), pak.items().size(), secs);
		return 0;
	} catch (const std::exception& ex) {
		std::fprintf(stderr, "error: %s\n", ex.what());
		return 1;
	}
}
