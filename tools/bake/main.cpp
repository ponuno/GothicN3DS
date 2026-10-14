// g3ds-bake: turns scenes (the worlds of a Gothic installation or the
// built-in synthetic test scene) into data.pak for the 3DS runtime.
#include "bake.h"
#include "sound_bake.h"
#include "g3ds/model.h"
#include "g3ds/tex.h"
#include "font_bake.h"
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
	          "  g3ds-bake extract --game DIR [--mod ...] [--ext sgt,sty,dls] -o DIR   copy music files out of the archives\n"
	          "  g3ds-bake mesh --msh FILE [--textures DIR] [options]        bake a loose .MSH (development)\n"
	          "  g3ds-bake synth [--cells N] [--clutter N] [options]         bake the built-in test scene\n"
	          "                                                              (--clutter: N crates and barrels strewn over it)\n"
	          "  g3ds-bake info FILE                                         describe a data.pak\n"
	          "\n"
	          "mod (Gothic 2 modifications, e.g. Chronicles of Myrtana: Archolos):\n"
	          "  --mod NAME|FILE    the mod's .ini (a path, or a name in <game>/System: \"Archolos\"): it names the\n"
	          "                     world, the compiled scripts and the .mod archives; those in Data/ are mounted over\n"
	          "                     the game's .vdf archives (all of them if the .ini lists none)\n"
	          "  --mod-archive X    also mount this .mod/.vdf (path, or a name under Data/); can be repeated; without\n"
	          "                     --mod the world is the one the archive brings, or named with --zen\n"
	          "  --mod-all          with --mod: mount every .mod under Data/, whatever the .ini lists\n"
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
	          "  --no-split         one chunk per cell holding everything (default: medium and small objects\n"
	          "                     go in chunks of their own, loaded only near the camera)\n"
	          "  --tier-min N       fewest triangles a cell needs for a chunk of medium or small objects (default: 400)\n"
	          "  --step CM          finest vertex position step (default: 0.125)\n"
	          "  --seal CM          close seams: weld vertices and cut edges at vertices resting on\n"
	          "                     them, within this distance (default: 0.5)\n"
	          "  --no-seal          leave seams as they are in the source\n"
	          "  --max-tex N        longest texture edge, power of two, 8..1024 (default: 128)\n"
	          "  --quality 0|1|2    ETC1 effort (default: 1)\n"
	          "  --flip             reverse the triangle winding (if the winding check complains)\n"
	          "  --no-hero          do not bake the player's character (the game stays first-person)\n"
	          "  --no-script-check  skip the comparison with the reference interpreter (minutes on a big game)\n"
	          "  --no-scripts       do not bake the game's scripts (the worlds stay empty)\n"
	          "  --sound-rate HZ    highest sample rate of the sound effects (default: 22050)\n"
	          "  --no-sound         bake no sound effects and no ambient sounds\n"
	          "  --debug            write bake_debug.txt beside the output: what the scripts did while the worlds were\n"
	          "                     populated, in order, with the engine functions and Ikarus/LeGo/Union code involved\n"
	          "  --debug-report F   the same into the file F\n"
	          "  --music-dir DIR    folder of WAV renders of the music themes; makes music.pak beside the output\n"
	          "  --music-rate HZ    highest sample rate of the music (default: 44100)\n"
	          "  --hero-script NAME world: the character's model script (default: HUMANS.MDS)\n"
	          "  --hero-body NAME   world: its body mesh (default: HUM_BODY_NAKED0)\n"
	          "  --hero-head NAME   world: its head mesh (default: HUM_HEAD_PONY; \"\" for none)\n"
	          "  --hero-skin B,C,H  world: texture variants of body, skin colour and head (default: 4,1,9)\n"
	          "  --font-cp N        world: the code page of the game's fonts (1250, 1251, 1252; default: told from the texts)\n"
	          "  --no-font          world: keep the overlay's own font, do not take the game's\n"
	          "  --no-objects       world mesh only, skip trees, bridges and other world objects\n"
	          "  --no-lightmaps     keep the vertex light on lightmapped (indoor) polygons\n"
	          "  --lightmap-cell M  lightmapped triangles whose shade differs from their corners' are cut down to this edge (default 1)\n"
	          "  --lightmap-extra N the most triangles that may add (default 0: no cutting)\n"
	          "  --ao off|low|high  Graphics+: bake ambient occlusion into the vertices - corners, the foot of walls\n"
	          "                     and the ground under eaves darker (low: 16 rays of 2.5 m, high: 40 of 4 m; default: off)\n"
	          "  --ao-rays N        rays per corner instead of the level's\n"
	          "  --probe X Y Z      list the triangles around that place (metres, as the log gives them; the game's position display is in cm) at\n"
	          "                     two stages of the bake; --probe-radius M (default 25)\n"
	          "  --ao-cell M        large triangles near something are cut to this edge before the occlusion (default 2, high 1.5);\n"
	          "  --ao-extra N       the most triangles that may add (default 0: no cutting)\n"
	          "  --ao-radius CM     how far a ray looks instead of the level's\n"
	          "  --wind             Graphics+: trees, bushes and grass sway (marked by their names; see the report)\n"
	          "  --simplify         fewer triangles in everything that is drawn: the world's objects and the level (edge\n"
	          "                     collapse, a few cm of error; trees' leaves, water and glass stay) and the characters (the\n"
	          "                     bones and the texture mapping are kept). Smaller archive, faster drawing; the look changes a little\n"
	          "  --simplify-ratio R triangles kept of what is simplified (default 0.6); --simplify-error CM: how far a collapse\n"
	          "                     may move the surface (default 3); --simplify-no-characters: the world only\n"
	          "  --simplify-ratio-large R   a second, harder pass over the large objects (the class that is always drawn, the\n"
	          "                     heaviest in the busiest scenes): R of their triangles kept (try 0.4; off by default);\n"
	          "                     --simplify-error-large CM: the error allowed in that pass (default 8)\n"
	          "  --no-vcache        keep the triangles of the batches in the order the chunker made them (by default they are\n"
	          "                     reordered so that the GPU transforms fewer vertices)\n"
	          "  --npc-lod          a second, coarse body for every character (about 30 %% of the triangles, no head): shown\n"
	          "                     from afar and swapped for the full one with a dissolve as the player comes near, so\n"
	          "                     characters can be shown further (menu: NPC distance); --npc-lod-ratio R (0.3), --npc-lod-error CM (6)\n"
	          "  --lod              Graphics+: a simplified stand-in for the world meshes of far chunks (about half the\n"
	          "                     triangles, same vertices; menu: Graphics+ -> Far geometry; adds ~10 %% to the chunks)\n"
	          "  --lod-ratio R      triangles kept of what is simplified (default 0.6); --lod-error CM: how far a\n"
	          "                     collapse may move the surface (default 30)\n"
	          "  --no-lod-objects   with --lod: no stand-ins for the chunks of medium and small objects (crates, bushes, ...;\n"
	          "                     by default they get them too: 40 %% of the triangles, drawn as far as the objects are);\n"
	          "                     --lod-objects-ratio R (0.4)\n"
	          "  --vis              Graphics+: bake which parts of the world are hidden from which place (walls, hills),\n"
	          "                     so the game need not draw them (menu: Graphics+ -> Visibility; minutes to bake)\n"
	          "  --vis-radius M     how far from a cell the table goes, metres (default 150); --vis-near M: nothing\n"
	          "                     this near is ever hidden (default 25); --vis-high: more places and rays (slower, surer)\n"
	          "  --lights           bake the light of the world's lamps (torches, fire places) into the vertices, with\n"
	          "                     shadows; the large triangles near a lamp are cut smaller (more triangles, see --light-cell)\n"
	          "  --light-strength F their light, 0.1..4 (default 1); --light-cell M: the edge they are cut to, metres (default 1.5)\n"
	          "  --light-static     also the lamps the world's own compiled light has (otherwise those are left out)\n"
	          "  --water-light N    the water's own light, 0..255 (default 200): the same blue-green tone all over it,\n"
	          "                     not the patches of the source mesh's vertex light; 0: keep the source's light\n"
	          "  --water-texture S  the water's texture: made here (default: soft caustics; S = made) or the game's (S = original)\n"
	          "  --water-alpha N    how opaque the made water is, 0..255 (default 110; lower = more see-through)\n"
	          "  --water-color R,G,B  the colour of that texture (default 92,140,156)\n"
	          "  --water-shore S    colour by depth and foam in the fine water, 0..1 (default 1; needs --water-grid)\n"
	          "  --wet-shore S      the ground at the water's edge darker (wet sand), 0..1 (default 1)\n"
	          "  --canopy S         the ground under trees and bushes darker, 0..1 (default 0.3); --canopy-reach M: how far up\n"
	          "                     a plant still shades (default 18)\n"
	          "  --water-grid M     cut the water along the shores into a grid of M metres (try 2; 0 = off, the default), fine\n"
	          "                     enough for the game's \"Waves on the water\" setting to lift it; the open water stays coarse\n"
	          "  --river-width M    water narrower than this stays calm: rivers, ponds (default 40; 0: waves everywhere)\n"
	          "  --water-reach M    how far from the shore line the fine water goes, metres (default 32; a multiple of 16 cells)\n"
	          "  --far-lod          Graphics+: a coarse copy of the world drawn beyond the draw distance and where\n"
	          "                     chunks are not loaded yet (far layer; about 1.5-2 MB on the console)\n"
	          "  --far-cell M       its finest grid, metres (default 4; coarser if needed to fit)\n"
	          "  --far-tris N       its most triangles, thousands (default 80)\n"
	          "  --plant-words A,B  more words that name a tree in object/material/texture names (a mod's own)\n"
	          "  --pack-level N     LZ4HC effort 3..12 (default 9; 12 is about 0.2 % smaller and a quarter slower to bake)\n"
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
		if (cmd != "world" && cmd != "mesh" && cmd != "synth" && cmd != "extract") {
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
		ZenModOptions modOpt;
		std::string extList = "sgt,sty,dls";
		bool scriptCheck = true, follow = true, hero = true, withScripts = true, withFont = true;
		int fontCodePage = 0;
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
			else if (a == "--no-split") opt.chunker.splitDetail = false;
			else if (a == "--tier-min") opt.chunker.minTierTriangles = u32(std::stoul(value()));
			else if (a == "--step") opt.chunker.positionStep = std::stof(value());
			else if (a == "--seal") opt.chunker.sealTolerance = std::stof(value());
			else if (a == "--no-seal") opt.chunker.seal = false;
			else if (a == "--max-tex") opt.tex.maxSize = std::stoi(value());
			else if (a == "--quality") opt.tex.quality = std::stoi(value());
			else if (a == "--pack-level") PakWriter::setLevel(std::stoi(value()));
			else if (a == "--threads") opt.threads = unsigned(std::max(1, std::stoi(value())));
			else if (a == "--game") gameDir = value();
			else if (a == "--zen") levels.push_back(levelFile(value()));
			else if (a == "--ext") extList = value();
			else if (a == "--mod") modOpt.ini = value();
			else if (a == "--mod-archive") modOpt.archives.push_back(value());
			else if (a == "--mod-all") modOpt.all = true;
			else if (a == "--no-follow") follow = false;
			else if (a == "--no-hero") hero = false;
			else if (a == "--no-scripts") withScripts = false;
			else if (a == "--no-script-check") scriptCheck = false;
			else if (a == "--no-font") withFont = false;
			else if (a == "--font-cp") fontCodePage = std::stoi(value());
			else if (a == "--sound-rate") opt.soundRate = u32(std::stoul(value()));
			else if (a == "--no-sound") opt.soundRate = 0;
			else if (a == "--music-dir") opt.musicDir = value();
			else if (a == "--music-rate") opt.musicRate = u32(std::stoul(value()));
			else if (a == "--voice-rate") opt.voiceRate = u32(std::stoul(value()));
			else if (a == "--no-voices") opt.voiceRate = 0;
			else if (a == "--debug") opt.debugFile = "?";
			else if (a == "--debug-report") opt.debugFile = value();
			else if (a == "--voice-codec") {
				const std::string v = value();
				if (v != "pcm" && v != "ima") throw std::runtime_error("--voice-codec wants pcm or ima");
				opt.voiceIma = v == "ima";
			}
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
			else if (a == "--lightmap-cell") zen.lightmapCell = std::clamp(std::stof(value()), 0.1f, 10.0f) * 100.0f;
			else if (a == "--lightmap-extra") zen.lightmapExtra = u32(std::clamp(std::stoi(value()), 0, 10000000));
			else if (a == "--flip") zen.flipWinding = true;
			else if (a == "--no-flip") zen.flipWinding = false; // the default; kept for old command lines
			else if (a == "--cells") synth.cells = std::stoi(value());
			else if (a == "--clutter") synth.clutter = std::stoi(value());
			else if (a == "--ao") {
				const std::string v = value();
				if (v == "off") opt.ao.level = AO_OFF;
				else if (v == "low") opt.ao.level = AO_LOW;
				else if (v == "high") opt.ao.level = AO_HIGH;
				else throw std::runtime_error("--ao wants off, low or high");
			}
			else if (a == "--ao-rays") opt.ao.rays = u32(std::clamp(std::stoi(value()), 1, 256));
			else if (a == "--probe") {
				opt.probe = true;
				for (float& c : opt.probeAt) c = std::stof(value());
			}
			else if (a == "--probe-radius") opt.probeRadius = std::clamp(std::stof(value()), 1.0f, 500.0f);
			else if (a == "--ao-cell") opt.ao.cell = std::clamp(std::stof(value()), 0.4f, 20.0f) * 100.0f;
			else if (a == "--ao-extra") opt.ao.maxExtra = u32(std::clamp(std::stoi(value()), 0, 5000000));
			else if (a == "--ao-radius") opt.ao.radius = std::clamp(std::stof(value()), 10.0f, 5000.0f);
			else if (a == "--wind") opt.wind = true;
			else if (a == "--simplify") opt.simplify.enabled = opt.simplifyModels.enabled = true;
			else if (a == "--simplify-ratio") {
				opt.simplify.ratio = std::clamp(std::stof(value()), 0.2f, 0.95f);
				opt.simplifyModels.ratio = std::max(opt.simplify.ratio, 0.4f);
			}
			else if (a == "--simplify-ratio-large") opt.simplify.ratioLarge = std::clamp(std::stof(value()), 0.1f, 0.95f);
			else if (a == "--simplify-error-large") opt.simplify.maxErrorLarge = std::clamp(std::stof(value()), 0.5f, 50.0f);
			else if (a == "--simplify-error") opt.simplify.maxError = std::clamp(std::stof(value()), 0.5f, 50.0f);
			else if (a == "--no-vcache") opt.vertexCache = false;
			else if (a == "--npc-lod") opt.npcLod = true;
			else if (a == "--npc-lod-ratio") opt.npcLodSimplify.ratio = std::clamp(std::stof(value()), 0.05f, 0.8f);
			else if (a == "--npc-lod-error") opt.npcLodSimplify.maxError = std::clamp(std::stof(value()), 0.5f, 30.0f);
			else if (a == "--simplify-no-characters") opt.simplifyModels.enabled = false;
			else if (a == "--lod") opt.lod.enabled = true;
			else if (a == "--no-lod-objects") opt.lod.objects = false;
			else if (a == "--lod-objects-ratio") opt.lod.ratioObjects = std::clamp(std::stof(value()), 0.1f, 0.9f);
			else if (a == "--lod-ratio") opt.lod.ratio = std::clamp(std::stof(value()), 0.1f, 0.9f);
			else if (a == "--lod-error") opt.lod.maxError = std::clamp(std::stof(value()), 1.0f, 500.0f);
			else if (a == "--lights") opt.light.enabled = true;
			else if (a == "--light-strength") opt.light.strength = std::clamp(std::stof(value()), 0.1f, 4.0f);
			else if (a == "--light-cell") opt.light.cell = std::clamp(std::stof(value()), 0.2f, 10.0f) * 100.0f;
			else if (a == "--light-static") opt.light.statics = true;
			else if (a == "--water-light") opt.waterLight = u8(std::clamp(std::stoi(value()), 0, 255));
			else if (a == "--water-texture") opt.waterTexture = value() != "original";
			else if (a == "--water-alpha") opt.waterAlpha = u8(std::clamp(std::stoi(value()), 0, 255));
			else if (a == "--water-color") {
				const std::string v = value();
				int r, g, b;
				if (std::sscanf(v.c_str(), "%d,%d,%d", &r, &g, &b) != 3) throw std::runtime_error("--water-color wants R,G,B");
				opt.waterColor[0] = u8(std::clamp(r, 0, 255));
				opt.waterColor[1] = u8(std::clamp(g, 0, 255));
				opt.waterColor[2] = u8(std::clamp(b, 0, 255));
			}
			else if (a == "--water-shore") opt.waterShore = std::clamp(std::stof(value()), 0.0f, 1.0f);
			else if (a == "--wet-shore") opt.wetShore = std::clamp(std::stof(value()), 0.0f, 1.0f);
			else if (a == "--canopy") opt.canopy = std::clamp(std::stof(value()), 0.0f, 1.0f);
			else if (a == "--canopy-reach") opt.canopyReach = std::clamp(std::stof(value()), 2.0f, 60.0f) * 100.0f;
			else if (a == "--water-grid") opt.waterGrid = std::clamp(std::stof(value()), 0.0f, 50.0f) * 100.0f;
			else if (a == "--river-width") opt.riverWidth = std::clamp(std::stof(value()), 0.0f, 500.0f) * 100.0f;
			else if (a == "--water-reach") opt.waterReach = std::clamp(std::stof(value()), 1.0f, 500.0f) * 100.0f;
			else if (a == "--vis") opt.vis.enabled = true;
			else if (a == "--vis-high") opt.vis.high = true;
			else if (a == "--vis-radius") opt.vis.radius = std::clamp(std::stof(value()), 40.0f, 500.0f) * 100.0f;
			else if (a == "--vis-near") opt.vis.nearAlways = std::clamp(std::stof(value()), 0.0f, 200.0f) * 100.0f;
			else if (a == "--far-lod") opt.far.enabled = true;
			else if (a == "--far-cell") opt.far.cell = std::clamp(std::stof(value()), 0.5f, 100.0f) * 100.0f;
			else if (a == "--far-tris") opt.far.maxTriangles = u32(std::clamp(std::stoi(value()), 5, 1000)) * 1000u;
			else if (a == "--plant-words") {
				const std::string v = value();
				for (size_t p = 0; p <= v.size();) {
					size_t e = v.find(',', p);
					if (e == std::string::npos) e = v.size();
					std::string word = v.substr(p, e - p);
					for (char& c : word) c = char(std::toupper(static_cast<unsigned char>(c)));
					if (!word.empty()) zen.plantWords.push_back(word);
					p = e + 1;
				}
			}
			else throw std::runtime_error("unknown option " + a);
		}
		opt.ao.threads = opt.threads;
		opt.vis.threads = opt.threads;
		opt.lod.threads = opt.threads;
		opt.simplify.threads = opt.threads;
		opt.tex.threads = opt.threads;
		if (opt.debugFile == "?") {
			const size_t slash = out.find_last_of("/\\");
			opt.debugFile = (slash == std::string::npos ? std::string() : out.substr(0, slash + 1)) + "bake_debug.txt";
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

		if (cmd == "extract") {
			// The game's music (or whatever --ext names) out of its archives, into the folder dmrender reads.
			if (gameDir.empty()) throw std::runtime_error("extract: --game DIR is required");
			std::vector<std::string> exts, notes;
			for (size_t a = 0; a <= extList.size();) {
				size_t b = extList.find(',', a);
				if (b == std::string::npos) b = extList.size();
				if (b > a) exts.push_back("." + extList.substr(a, b - a));
				a = b + 1;
			}
			const std::shared_ptr<ZenGame> game = openZenGame(gameDir, notes, modOpt);
			const size_t n = zenExtractFiles(*game, exts, out == "data.pak" ? "music_src" : out, notes);
			for (const std::string& note : notes) std::printf("%s\n", note.c_str());
			std::printf("extracted %zu files (%s) into %s\n", n, extList.c_str(), (out == "data.pak" ? "music_src" : out).c_str());
			return n ? 0 : 1;
		}

		const auto t0 = std::chrono::steady_clock::now();
		std::vector<BakeWorld> worlds;
		std::vector<SrcModel> models;
		CameraSet camera = defaultCameraSet();
		bool haveCamera = false;
		SrcScript script, fightScript, sfxScript, musicScript, particleScript, visualFxScript;
		SrcDialogue dialogue;
		bool haveDialogue = false;
		SrcFonts fonts;
		bool haveFonts = false;
		std::vector<u8> scriptDat;
		BakeScripts scripts;
		if (cmd == "synth") {
			if (withScripts) {
				buildSyntheticScript(script);
				buildSyntheticFightScript(fightScript);
				buildSyntheticSfxScript(sfxScript);
				buildSyntheticMusicScript(musicScript);
				scripts.sfx = &sfxScript;
				scripts.music = &musicScript;
				scripts.script = &script;
				scripts.fight = &fightScript;
				scripts.looks = LookSource {buildSyntheticRig, buildSyntheticBody, buildSyntheticHead, buildSyntheticItem, buildSyntheticParticleTexture, buildSyntheticOverlay, buildSyntheticOverlays};
				scripts.soundFile = buildSyntheticSound;
				buildSyntheticDialogue(dialogue);
				haveDialogue = true;
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
			const std::shared_ptr<ZenGame> game = openZenGame(gameDir, notes, modOpt);
			const ZenModInfo& mod = zenGameMod(*game);
			if (levels.empty() && mod.active) {
				// The world the mod starts in: its .ini says; else the one world its archives bring.
				if (!mod.world.empty() && zenGameHasFile(*game, mod.world)) {
					levels.push_back(mod.world);
				} else if (mod.world.empty() && mod.worlds.size() == 1) {
					levels.push_back(mod.worlds[0]);
					notes.push_back("mod: no World= in the .ini; the only world its archives bring is " + mod.worlds[0]);
				} else if (!mod.world.empty()) {
					std::string list;
					for (const std::string& w : mod.worlds) list += (list.empty() ? "" : ", ") + w;
					throw std::runtime_error("the mod's world " + mod.world + " is in none of the archives; name one with --zen" +
					                         (list.empty() ? std::string() : " (new in the mod's archives: " + list + ")"));
				} // else: no world named: the usual names, then a list to choose from (below)
			}
			if (levels.empty()) {
				for (const char* candidate : {"WORLD.ZEN", "NEWWORLD.ZEN"})
					if (levels.empty() && zenGameHasFile(*game, candidate)) levels.push_back(candidate);
				if (levels.empty()) {
					// A game of its own (Chronicles of Myrtana: Archolos) names its world otherwise.
					const auto found = zenGameWorlds(*game);
					if (found.size() == 1) {
						levels.push_back(found[0].first);
						notes.push_back("no WORLD.ZEN or NEWWORLD.ZEN; the only world in the archives is " + found[0].first);
					} else if (found.size() >= 2 && found[0].second >= 2 * found[1].second && found[0].second >= (1u << 20)) {
						// (Listed biggest first.) One far bigger than the rest is the main world: the window of the
						// Windows baker has no field for --zen, and Archolos has ARCHOLOS_MAINLAND.ZEN (186 MB) beside
						// ENDGAME (33 MB) and a few 0 MB files.
						levels.push_back(found[0].first);
						notes.push_back("no WORLD.ZEN or NEWWORLD.ZEN; took the biggest world, " + found[0].first + " (" +
						                std::to_string(found[0].second >> 20) + " MB, the next is " + std::to_string(found[1].second >> 20) +
						                " MB). Name another with --zen NAME.ZEN");
					} else {
						std::string list;
						for (size_t i = 0; i < found.size() && i < 40; ++i)
							list += "\n    " + found[i].first + "  (" + std::to_string(found[i].second >> 20) + " MB)";
						throw std::runtime_error(
						    std::string("no WORLD.ZEN or NEWWORLD.ZEN in the archives; name the world with --zen NAME.ZEN") +
						    (found.empty() ? " (there is no .ZEN at all in the archives: wrong folder, or the .vdf/.mod are not in Data/?)"
						                   : ". The worlds found, biggest first (the main world is usually the biggest):" + list));
					}
				}
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
				if (scriptCheck) scripts.dat = &scriptDat; // (the check against the reference interpreter takes minutes on a big game)
				scripts.looks = zenLookSource(game);
				scripts.soundFile = zenSoundSource(game);
				// Gothic 1 keeps how its people fight in a file of its own.
				if (importZenFightScript(game, fightScript, worlds[0].scene.notes)) scripts.fight = &fightScript;
				// ... and its sounds and music themes, likewise in files of their own.
				if (importZenSfxScript(game, sfxScript, worlds[0].scene.notes)) scripts.sfx = &sfxScript;
				if (importZenParticleScript(game, particleScript, worlds[0].scene.notes)) scripts.particles = &particleScript;
				if (importZenVisualFxScript(game, visualFxScript, worlds[0].scene.notes)) scripts.visualfx = &visualFxScript;
				if (!opt.musicDir.empty() && importZenMusicScript(game, musicScript, worlds[0].scene.notes)) scripts.music = &musicScript;
				// ... and what they say.
				haveDialogue = importZenDialogue(game, dialogue, worlds[0].scene.notes);
			}
			// The game's own fonts for the overlay.
			if (withFont && !worlds.empty() && importZenFonts(game, fonts, worlds[0].scene.notes)) {
				std::vector<std::string> texts;
				texts.reserve(dialogue.lines.size());
				for (const auto& l : dialogue.lines) texts.push_back(l.second);
				fonts.codepage = fontCodePage ? fontCodePage : detectCodePage(texts);
				haveFonts = true;
			}
		}
		if (!firstName.empty()) worlds[0].name = firstName;

		PakWriter pak, musicPak;
		if (!opt.musicDir.empty()) scripts.musicPak = &musicPak;
		bakeWorlds(worlds, models, scripts, opt, pak);
		if (haveDialogue) {
			std::string line;
			for (DialogueAsset& asset : bakeDialogue(dialogue, line)) pak.add(asset.name, ASSET_BLOB, asset.bytes);
			std::printf("%s\n", line.c_str());
		}
		if (haveFonts) {
			std::vector<u8> blob;
			std::string report, why;
			if (bakeFont(fonts, blob, report, why)) {
				pak.add("font/ui", ASSET_BLOB, blob);
				std::printf("%s\n", report.c_str());
			} else {
				std::printf("font: not baked (%s); the overlay keeps its own font\n", why.c_str());
			}
		}
		if (haveCamera) {
			const u8* bytes = reinterpret_cast<const u8*>(&camera);
			pak.add("meta/camera", ASSET_META, std::vector<u8>(bytes, bytes + sizeof(camera)));
		}
		pak.write(out);
		std::printf("%s written (%zu entries); the rest (voices, music) follows\n", out.c_str(), pak.items().size());
		// (The fight clips again, here at the end of the report, where the tail of it shows them.)
		for (const SrcModel& m : models)
			for (const std::string& note : m.notes)
				if (note.compare(0, 9, "fightclip") == 0) std::printf("%s: %s\n", m.name.c_str(), note.c_str());
		std::fflush(stdout);
		const size_t outSlash = out.find_last_of("/\\");
		const std::string outDir = outSlash == std::string::npos ? std::string() : out.substr(0, outSlash + 1);
		// The voices: a recording for each line of the dialogue, into speech.pak beside it.
		if (haveDialogue && opt.voiceRate > 0 && scripts.soundFile) {
			std::vector<std::string> names;
			names.reserve(dialogue.lines.size());
			for (const auto& l : dialogue.lines) names.push_back(l.first);
			// (The console cannot seek past 2 GiB of one file: the recordings go into archives of less than that.)
			std::vector<std::unique_ptr<PakWriter>> speechPaks;
			std::printf("voices: baking the recordings of %zu lines of dialogue (many minutes on a big game; --no-voices skips) ...\n", names.size());
			std::fflush(stdout);
			const SpeechBakeResult sr = bakeSpeechShards(names, scripts.soundFile, opt.voiceRate, opt.threads, speechPaks, opt.voiceIma);
			std::printf("voices: %u lines of dialogue, %u recordings baked (%.1f MB at up to %u Hz, %s)", sr.lines, sr.baked,
			            double(sr.bytes) / 1048576.0, opt.voiceRate, opt.voiceIma ? "IMA ADPCM" : "16-bit, full quality");
			if (!sr.unusable.empty()) {
				std::printf("; %zu could not be used:", sr.unusable.size());
				for (size_t i = 0; i < sr.unusable.size() && i < 4; ++i) std::printf(" [%s]", sr.unusable[i].c_str());
			}
			std::printf("\n");
			if (sr.baked > 0) {
				for (size_t k = 0; k < speechPaks.size(); ++k) {
					const std::string speechPath = outDir + (k == 0 ? std::string("speech.pak") : "speech" + std::to_string(k + 1) + ".pak");
					speechPaks[k]->write(speechPath);
					std::printf("wrote %s: %zu voices\n", speechPath.c_str(), speechPaks[k]->items().size());
				}
				// (An older bake may have left more archives than this one needs; they would be stale.)
				for (size_t k = speechPaks.size(); k < 4; ++k)
					std::remove((outDir + (k == 0 ? std::string("speech.pak") : "speech" + std::to_string(k + 1) + ".pak")).c_str());
			} else {
				std::printf("WARNING: no speech.pak written: none of the %u lines has a WAV in the game's files (Speech.vdf, or "
				            "_work/Data/Sound/Speech). The game then shows subtitles only; --no-voices hides this message.\n",
				            sr.lines);
			}
		}
		if (scripts.musicPak && !musicPak.items().empty()) {
			const size_t slash = out.find_last_of("/\\");
			const std::string musicPath = (slash == std::string::npos ? std::string() : out.substr(0, slash + 1)) + "music.pak";
			musicPak.write(musicPath);
			std::printf("wrote %s: %zu tracks\n", musicPath.c_str(), musicPak.items().size());
		} else if (!opt.musicDir.empty()) {
			std::printf("WARNING: no music.pak written: no WAV in '%s' has the name of a theme file in the game's scripts "
			            "(OC_DAY_STD.sgt wants OC_DAY_STD.wav). See the 'music:' line above.\n",
			            opt.musicDir.c_str());
		}
		const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		std::printf("wrote %s: %zu worlds, %zu entries in %.1f s\n", out.c_str(), worlds.size(), pak.items().size(), secs);
		return 0;
	} catch (const std::exception& ex) {
		std::fprintf(stderr, "error: %s\n", ex.what());
		return 1;
	}
}
