#include "populate.h"

#include <fstream>
#include "sound_bake.h"
#include "weather_sfx_bake.h"
#include "fight_ai.h"
#include "fight_clips.h"

#include "game.h"
#include "g3ds/game.h"
#include "g3ds/model.h"
#include "g3ds/math.h"
#include "g3ds/item_view.h"
#include "g3ds/card.h"
#include "item_icon.h"
#include "model_bake.h"
#include "texture_bake.h"
#include "g3ds/pfx.h"
#include "g3ds/barrier.h"
#include "script_check.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

namespace g3 {

namespace {

std::string format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string format(const char* fmt, ...) {
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

std::string upper(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::toupper(c)); });
	return s;
}

bool findPoint(void* user, const char* name, float pos[3], float& yaw) {
	const SrcScene& scene = *static_cast<const SrcScene*>(user);
	const auto it = scene.points.find(upper(name));
	if (it == scene.points.end()) return false;
	std::copy_n(it->second.pos, 3, pos);
	yaw = it->second.yaw;
	return true;
}

struct Look {
	u32 key = 0;
	std::string script, body, head, armor;
	int bodyTex = 0, skin = 0, headTex = 0, teethTex = 0;
	u32 users = 0;
	std::string example; // somebody who looks like this
};

// What a model is made of, to tell two requests for the same thing apart
// from two for different things: a body in a skin colour its textures do not
// have is the same model as in the colour they do.
std::string signature(const std::string& kind, const SrcModel& m) {
	std::string s = kind;
	std::vector<std::string> textures;
	size_t triangles = 0;
	for (const SrcModelPart& part : m.parts) {
		textures.push_back(upper(part.material.texture));
		triangles += part.verts.size() / 3;
	}
	std::sort(textures.begin(), textures.end());
	for (const std::string& t : textures) s += "|" + t;
	return s + "|" + std::to_string(triangles);
}

bool sameSkeleton(const SrcModel& a, const SrcModel& b) {
	if (a.nodes.size() != b.nodes.size()) return false;
	for (size_t i = 0; i < a.nodes.size(); ++i)
		if (upper(a.nodes[i].name) != upper(b.nodes[i].name) || a.nodes[i].parent != b.nodes[i].parent) return false;
	return true;
}


// For the report: what a skeleton has for using each kind of thing the
// scripts name - the steps into it, the states, the steps out - and how
// each moves the body: where the root ends up (cm: + is to the right, up,
// ahead), whether the game flags the clip as moving the character ("M")
// and whether it is baked to carry it; how far the root turns; for a
// state, where the root is held. And which of the usual steps are missing.
void describeMobClips(const std::string& rigName, const SrcModel& model, const std::vector<std::string>& schemes,
                      std::vector<std::string>& out) {
	std::string all;
	size_t kinds = 0;
	for (const std::string& scheme : schemes) {
		std::string have, missing;
		std::set<std::string> names;
		for (const SrcClip& clip : model.clips) {
			const std::string name = clip.name;
			const bool ours = (name.compare(0, 2, "T_") == 0 || name.compare(0, 2, "S_") == 0 || name.compare(0, 2, "R_") == 0) &&
			                  name.compare(2, scheme.size() + 1, scheme + "_") == 0;
			if (!ours) continue;
			names.insert(name);
			const size_t tracks = clip.nodes.size();
			if (!tracks || clip.samples.size() < tracks) continue;
			const size_t frames = clip.samples.size() / tracks;
			size_t root = tracks;
			for (size_t t = 0; t < tracks; ++t)
				if (clip.nodes[t] >= 0 && size_t(clip.nodes[t]) < model.nodes.size() && model.nodes[size_t(clip.nodes[t])].parent < 0) root = t;
			const bool loop = clip.next == clip.name;
			std::string what = format("%s %.2fs", name.c_str(), double(float(frames) / std::max(clip.fps, 1.0f)));
			if (!loop && !clip.next.empty()) what += " ->" + clip.next;
			if (loop) what += " loop";
			if (root < tracks) {
				const SrcClipSample& a = clip.samples[root];
				const SrcClipSample& b = clip.samples[(frames - 1) * tracks + root];
				const Vec3 fa = quatRotate(quatNormalize({a.rot[0], a.rot[1], a.rot[2], a.rot[3]}), {0, 0, 1});
				const Vec3 fb = quatRotate(quatNormalize({b.rot[0], b.rot[1], b.rot[2], b.rot[3]}), {0, 0, 1});
				// (Only where the root's forward is not pointing up or down: lying, it is.)
				float turn = 0;
				if (std::hypot(fa.x, fa.z) > 0.4f && std::hypot(fb.x, fb.z) > 0.4f) {
					turn = (std::atan2(fb.x, fb.z) - std::atan2(fa.x, fa.z)) * 57.29578f;
					while (turn > 180.0f) turn -= 360.0f;
					while (turn < -180.0f) turn += 360.0f;
				}
				float reach = 0;
				for (size_t f = 1; f < frames; ++f) {
					const SrcClipSample& c = clip.samples[f * tracks + root];
					reach = std::max(reach, std::hypot(c.pos[0] - a.pos[0], c.pos[2] - a.pos[2]));
				}
				if (loop)
					what += format(" root held at %.0f %.0f %.0f", double(a.pos[0]), double(a.pos[1]), double(a.pos[2]));
				else
					what += format(" root %+.0f %+.0f %+.0f", double(b.pos[0] - a.pos[0]), double(b.pos[1] - a.pos[1]), double(b.pos[2] - a.pos[2]));
				if (std::fabs(turn) >= 5.0f) what += format(" turns %+.0f", double(turn));
				what += clip.moves ? " M" : "";
				what += !loop && (clip.moves || reach > 2.0f) ? " (carries)" : "";
			} else {
				what += " no root (keeps the step's before)";
			}
			have += (have.empty() ? "" : ", ") + what;
		}
		if (names.empty()) continue;
		const auto has = [&](const std::string& n) {
			return names.count(n) || names.count(n.substr(0, 2) + scheme + "_FRONT" + n.substr(2 + scheme.size())) ||
			       names.count(n.substr(0, 2) + scheme + "_BACK" + n.substr(2 + scheme.size()));
		};
		for (const char* step : {"T_%s_STAND_2_S0", "S_%s_S0", "T_%s_S0_2_STAND"}) {
			const std::string n = format(step, scheme.c_str());
			if (!has(n)) missing += (missing.empty() ? "" : ", ") + n;
		}
		if (has("T_" + scheme + "_S0_2_S1") || has("S_" + scheme + "_S1"))
			for (const char* step : {"T_%s_S0_2_S1", "S_%s_S1", "T_%s_S1_2_S0"}) {
				const std::string n = format(step, scheme.c_str());
				if (!has(n)) missing += (missing.empty() ? "" : ", ") + n;
			}
		all += format("\n  %s: ", scheme.c_str()) + have + (missing.empty() ? "" : "; missing: " + missing);
		++kinds;
	}
	if (kinds) out.push_back(format("things to use: what %s has for %zu kinds of them:", rigName.c_str(), kinds) + all);
}

} // namespace

std::string worldScriptName(const BakeWorld& world) {
	std::string s = world.level.empty() ? world.name : world.level;
	const size_t slash = s.find_last_of("/\\");
	if (slash != std::string::npos) s = s.substr(slash + 1);
	const size_t dot = s.rfind('.');
	if (dot != std::string::npos) s.resize(dot);
	return upper(s);
}

namespace {

// Which library of the mods' scripts a function belongs to, by its name (the libraries add Daedalus code
// that reads and writes the game's memory: this VM has no such memory, so what they do is not the original's).
const char* libraryOf(const std::string& name) {
	static const char* const ikarus[] = {"MEM_", "MEMINT_", "ASM_", "ASMINT_", "CALL_", "CALLINT_", "IKARUS"};
	static const char* const lego[] = {"FF_", "_FF_", "HOOK", "LEGO", "BUFFER_", "LOCALS", "CURSOR_", "VIEW_", "BAR_", "PERM_", "HT_", "LIST_",
	                                    "TIMER_", "TEXT_", "GAMESTATE_", "EVENTHANDLER", "BIN_", "ANIMATION_", "TRIGGERFIX", "DIALOGGESTURES",
	                                    "CONSOLE_", "SAVEGAME_"};
	static const char* const unionLib[] = {"UNION_", "SP_", "SYSTEMPACK", "NINJA", "ZUNION"};
	for (const char* p : ikarus)
		if (name.compare(0, std::strlen(p), p) == 0) return "Ikarus";
	for (const char* p : lego)
		if (name.compare(0, std::strlen(p), p) == 0) return "LeGo";
	for (const char* p : unionLib)
		if (name.compare(0, std::strlen(p), p) == 0) return "Union/Systempack";
	return "the game / the mod";
}

} // namespace

void bakePopulation(std::vector<BakeWorld>& worlds, const BakeScripts& scripts, const BakeOptions& opt, PakWriter& pak,
                    const std::function<void(const std::string&)>& line, BakeResult& result) {
	if (!scripts.script) return;
	const SrcScript& script = *scripts.script;
	line("== scripts ==");
	const auto lines = [&](const std::string& text) {
		size_t at = 0;
		while (at < text.size()) {
			const size_t end = text.find('\n', at);
			line(text.substr(at, end == std::string::npos ? std::string::npos : end - at));
			if (end == std::string::npos) break;
			at = end + 1;
		}
	};

	// ---- the image ----
	std::vector<u8> image;
	std::vector<u8> stateNames; // "script/states": the states' names, one after the other with a 0 after each
	try {
		ScriptBakeResult sr;
		image = bakeScript(script, sr);
		for (const std::string& name : sr.stateNames) {
			const std::string up = upper(name);
			stateNames.insert(stateNames.end(), up.begin(), up.end());
			stateNames.push_back(0);
		}
		lines(sr.report);
	} catch (const std::exception& ex) {
		line(std::string("WARNING: scripts not baked: ") + ex.what() + "; the worlds stay empty");
		return;
	}
	const auto fresh = [&]() {
		u8* blob = static_cast<u8*>(std::malloc(image.size()));
		if (!blob) throw std::bad_alloc();
		std::memcpy(blob, image.data(), image.size());
		return blob;
	};

	// ---- the same in the reference interpreter ----
	if (scripts.dat) {
		ScriptCheckOptions co;
		co.world = worldScriptName(worlds[0]);
		for (size_t w = 1; w < worlds.size(); ++w) {
			co.functions.push_back("STARTUP_" + worldScriptName(worlds[w]));
			co.functions.push_back("INIT_" + worldScriptName(worlds[w]));
		}
		try {
			// (It can take minutes on a big game: say so, or it looks like a hang.)
			std::printf("scripts: checking against the reference interpreter (at most %d s; --no-script-check skips it) ...\n", co.timeoutSeconds);
			std::fflush(stdout);
			const ScriptCheckResult cr = checkScript(*scripts.dat, script, image, co);
			lines(cr.report);
			result.scriptChecked = cr.ran;
			result.scriptSame = cr.ran && cr.differ == 0 && cr.stateSame;
		} catch (const std::exception& ex) {
			line(std::string("scripts: not checked against the reference interpreter: ") + ex.what());
		}
	}

	// ---- who lives where ----
	Game game;
	game.setQuiet(true);
	if (!game.adopt(fresh(), u32(image.size()))) {
		line("WARNING: the script machine refuses the baked scripts; the worlds stay empty");
		return;
	}
	pak.add("script/gothic", ASSET_SCRIPT, image);
	if (!stateNames.empty()) pak.add("script/states", ASSET_META, stateNames);
	if (scripts.fight) {
		try {
			std::string report;
			pak.add("script/fight", ASSET_META, bakeFightAi(*scripts.fight, report));
			line(report + " (" + scripts.fight->source + ")");
		} catch (const std::exception& ex) {
			line(std::string("WARNING: fight AI not baked (") + scripts.fight->source + ": " + ex.what() +
			     "); the inhabitants fight without its tables");
		}
	} else {
		line("fight AI: no tables of their own (FIGHT.DAT); the engine looks for them in the scripts");
	}

	// A script of its own (SFX.DAT, MUSIC.DAT): baked into the archive as `entry`, and a machine of its own
	// to read its instances in here. False (with a line) if it cannot be had.
	const auto sideScript = [&](const SrcScript* src, const char* entry, ScriptVm& vm) {
		if (!src) return false;
		try {
			ScriptBakeResult sr;
			const std::vector<u8> img = bakeScript(*src, sr);
			pak.add(entry, ASSET_SCRIPT, img);
			u8* blob = static_cast<u8*>(std::malloc(img.size()));
			if (!blob) throw std::bad_alloc();
			std::memcpy(blob, img.data(), img.size());
			if (!vm.adopt(blob, u32(img.size()))) throw std::runtime_error("the machine refuses the image");
			line(format("%s: %s, %zu bytes", entry, src->source.c_str(), img.size()));
			return true;
		} catch (const std::exception& ex) {
			line(std::string("WARNING: ") + entry + " not baked (" + src->source + ": " + ex.what() + ")");
			return false;
		}
	};

	// ---- sounds: the samples the scripts' C_SFX instances name ----
	ScriptVm sfxVm;
	const bool haveSfx = sideScript(scripts.sfx, "script/sfx", sfxVm);
	if (opt.soundRate > 0 && scripts.soundFile) {
		try {
			const SoundBakeResult sb = bakeSounds(haveSfx ? sfxVm : game.vm(), scripts.soundFile, opt.soundRate, pak);
			std::string text = format("sounds: %u C_SFX instances name %u files; %u baked (%.1f MB at up to %u Hz)", sb.instances,
			                          sb.files, sb.baked, double(sb.bytes) / 1048576.0, opt.soundRate);
			if (!sb.missing.empty()) {
				text += format("; %zu are not in the game's files:", sb.missing.size());
				for (size_t i = 0; i < sb.missing.size() && i < 6; ++i) text += " " + sb.missing[i];
			}
			if (!sb.unusable.empty()) {
				text += format("; %zu could not be used:", sb.unusable.size());
				for (size_t i = 0; i < sb.unusable.size() && i < 4; ++i) text += " [" + sb.unusable[i] + "]";
			}
			line(text);
			// The baker's own rain and thunder (weather_sfx_bake.h): listed first, so the game takes them before its own.
			std::string ownWeather;
			if (opt.weatherSfx) line(bakeWeatherSfx(pak, opt.soundRate, ownWeather));
			// The Barrier's hum: which sound instances the scripts have for it (the engine looks for MFX_BARRIERE_AMBIENT and the like).
			const SrcScript* names = haveSfx ? scripts.sfx : scripts.script;
			if (!names && !ownWeather.empty()) pak.add("meta/weathersfx", ASSET_META, std::vector<u8>(ownWeather.begin(), ownWeather.end()));
			if (names) {
				std::string found;
				for (const SrcSymbol& s : names->symbols) {
					std::string up = s.name;
					for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
					if (up.find("BARRI") != std::string::npos) found += (found.empty() ? "" : ", ") + s.name;
				}
				line(found.empty() ? "barrier: no sound instance named like it in the scripts; the dome is silent"
				                   : "barrier: sound instances named like it: " + found);
				// fix150: which of them is the hum (A) and which the lightning (T): engine/barrier.cpp takes them.
				{
					std::string list;
					for (const SrcSymbol& s : names->symbols) {
						std::string up = s.name;
						for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
						if (up.find("BARRI") == std::string::npos) continue;
						// (fix184: only the magic's own sounds; Gothic 1's ROPEWAY_BARRIER is the rope way, not thunder)
						if (up.compare(0, 4, "MFX_") != 0) continue;
						const bool hum = up.find("AMBIENT") != std::string::npos || up.find("LOOP") != std::string::npos ||
						                 up.find("HUM") != std::string::npos;
						list += std::string(hum ? "A " : "T ") + s.name + "\n";
					}
					if (!list.empty()) pak.add("meta/barriersfx", ASSET_META, std::vector<u8>(list.begin(), list.end()));
				}
				// The weather's sounds: the instances named like rain and thunder (the engine takes the first it can play; a looping one for the rain).
				std::string rainList, thunderList, meta = ownWeather;
				for (const SrcSymbol& s : names->symbols) {
					std::string up = s.name;
					for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
					// (A spell's sound - MFX_FIRERAIN_RAIN, MFX_THUNDERBALL_* - is not the weather.)
					const bool spell = up.rfind("MFX_", 0) == 0 || up.rfind("SPELL", 0) == 0 || up.rfind("SPL_", 0) == 0;
					const bool rain = !spell && (up.find("RAIN") != std::string::npos || up.find("REGEN") != std::string::npos);
					const bool thunder = !spell && (up.find("THUNDER") != std::string::npos || up.find("DONNER") != std::string::npos ||
					                                up.find("GEWITTER") != std::string::npos);
					if (rain) { rainList += (rainList.empty() ? "" : ", ") + s.name; meta += "R " + s.name + "\n"; }
					if (thunder) { thunderList += (thunderList.empty() ? "" : ", ") + s.name; meta += "T " + s.name + "\n"; }
				}
				// Files no instance names but the engine of the game may play by itself (ZenGin's sky plays the rain).
				static const char* const kFiles[][2] = {{"R", "RAIN_01.WAV"}, {"R", "RAIN.WAV"}, {"R", "RAIN_LOOP.WAV"}, {"R", "AMB_RAIN.WAV"},
				                                        {"T", "THUNDER_01.WAV"}, {"T", "THUNDER.WAV"}, {"T", "THUNDER_A1.WAV"}, {"T", "DONNER.WAV"}};
				for (const auto& f : kFiles) {
					std::vector<u8> bytes;
					SrcSound snd;
					std::string why;
					if (!scripts.soundFile(f[1], bytes) || !decodeWav(bytes, snd, why)) continue;
					resampleSound(snd, opt.soundRate);
					if (snd.samples.size() > SOUND_MAX_FRAMES || snd.rate < SOUND_MIN_RATE) continue;
					std::string stem = f[1];
					stem = stem.substr(0, stem.find('.'));
					std::string lower = stem;
					for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
					if (!pak.has("sfx/" + lower)) pak.add("sfx/" + lower, ASSET_SOUND, encodeSound(snd), false);
					meta += std::string(f[0]) + " " + stem + "\n";
					(f[0][0] == 'R' ? rainList : thunderList) += (f[0][0] == 'R' ? rainList : thunderList).empty() ? stem + " (file)" : ", " + stem + " (file)";
				}
				if (!meta.empty()) pak.add("meta/weathersfx", ASSET_META, std::vector<u8>(meta.begin(), meta.end()));
				const std::string none = ownWeather.empty() ? "none (the game makes its own)" : "none (the baker's are used)";
				line("weather sounds of the game: rain: " + (rainList.empty() ? none : rainList) + "; thunder: " + (thunderList.empty() ? none : thunderList) +
				     (ownWeather.empty() ? "" : "; the baker's own come first"));
			}
		} catch (const std::exception& ex) {
			line(std::string("WARNING: sounds not baked: ") + ex.what());
		}
	} else if (opt.soundRate > 0) {
		line("sounds: no sound files to read; the game is silent");
	}

	// ---- music: the renders in --music-dir, into music.pak ----
	if (!opt.musicDir.empty() && scripts.musicPak) {
		ScriptVm musicVm;
		const bool haveMusic = sideScript(scripts.music, "script/music", musicVm);
		try {
			const MusicBakeResult mb = bakeMusic(haveMusic ? musicVm : game.vm(), opt.musicDir, opt.musicRate, *scripts.musicPak);
			std::string text = format("music: %u C_MUSICTHEME instances name %u tracks; %u baked (%.1f MB at up to %u Hz)", mb.themes,
			                          mb.files, mb.baked, double(mb.bytes) / 1048576.0, opt.musicRate);
			if (!mb.missing.empty()) {
				text += format("; %zu have no WAV in the folder:", mb.missing.size());
				for (size_t i = 0; i < mb.missing.size() && i < 6; ++i) text += " " + mb.missing[i];
			}
			if (!mb.unusable.empty()) {
				text += format("; %zu could not be used:", mb.unusable.size());
				for (size_t i = 0; i < mb.unusable.size() && i < 4; ++i) text += " [" + mb.unusable[i] + "]";
			}
			line(text);
		} catch (const std::exception& ex) {
			line(std::string("WARNING: music not baked: ") + ex.what());
		}
	}

	struct Faults {
		const SrcScript* script;
		std::map<std::string, u32> kinds;
		std::vector<std::string> order; // the kinds in the order they first happened (the first is where it began)
	} faults {&script, {}, {}};
	std::string dbg; // the detailed report (--debug)
	const bool debugging = !opt.debugFile.empty();
	const auto dbgLine = [&](const std::string& text) {
		if (debugging) dbg += text + "\n";
	};
	game.vm().onError(
	    [](const char* what, u32 function, u32, void* user) {
		    Faults& f = *static_cast<Faults*>(user);
		    const std::string where = function < f.script->symbols.size() ? f.script->symbols[function].name : std::string("?");
		    const std::string key = std::string(what) + " in " + where;
		    if (f.kinds.size() < 4096) {
			    if (++f.kinds[key] == 1 && f.order.size() < 400) f.order.push_back(key);
		    }
	    },
	    &faults);
	std::map<u32, u32> unboundCalls; // calls of engine functions nobody implements, by symbol
	game.vm().onUnbound([](u32 symbol, void* user) { ++(*static_cast<std::map<u32, u32>*>(user))[symbol]; }, &unboundCalls);

	if (const GameNpc* hero = game.hero()) {
		const GameLook& l = hero->look;
		line(format("the player's character: %s, body %s (texture %d, skin %d), head %s (texture %d)", game.text(l.script),
		            game.text(l.body), l.bodyTex, l.skin, game.text(l.head), l.headTex));
	} else {
		line("the scripts have no PC_HERO");
	}
	std::vector<u16> gameWorld(worlds.size(), 0xFFFF); // the game's index of each world
	for (size_t w = 0; w < worlds.size(); ++w) {
		const std::string name = worldScriptName(worlds[w]);
		const GameStats before = game.stats();
		const ScriptStats vmBefore = game.vm().stats();
		const u32 npcsBefore = game.npcCount();
		const bool hasStartup = script.find("STARTUP_" + name) >= 0;
		game.enterWorld(name.c_str(), 8.0f, findPoint, &worlds[w].scene);
		gameWorld[w] = game.currentWorld();
		const GameStats& now = game.stats();
		{
			const ScriptStats& vmNow = game.vm().stats();
			dbgLine(format("world '%s' (STARTUP_%s%s): %llu instructions, %u complaints, %u calls given up, %u calls of engine functions nobody carries out, "
			         "%u uses of members of no object; %u characters",
			         worlds[w].name.c_str(), name.c_str(), hasStartup ? "" : " is missing",
			         static_cast<unsigned long long>(vmNow.instructions - vmBefore.instructions), vmNow.errors - vmBefore.errors,
			         vmNow.aborted - vmBefore.aborted, vmNow.stubCalls - vmBefore.stubCalls, vmNow.nullAccess - vmBefore.nullAccess,
			         now.npcs - before.npcs));
		}
		if (!hasStartup) {
			line("world '" + worlds[w].name + "': the scripts have no STARTUP_" + name + "; nobody lives there");
			continue;
		}
		u32 creatures = 0;
		std::vector<std::string> nowhere;
		for (u32 i = npcsBefore; i < game.npcCount(); ++i) {
			const GameNpc& n = game.npc(i);
			creatures += n.routineCount == 0;
			if (!n.placed) {
				const std::string where = game.text(n.want);
				if (std::find(nowhere.begin(), nowhere.end(), where) == nowhere.end()) nowhere.push_back(where);
			}
		}
		std::string l = format("world '%s': STARTUP_%s puts %u characters into it (%u with a daily routine, %u without), %u items",
		                       worlds[w].name.c_str(), name.c_str(), now.npcs - before.npcs, now.withRoutine - before.withRoutine,
		                       creatures, now.items - before.items);
		if (now.notNpc != before.notNpc) l += format("; %u insertions of things that are no characters", now.notNpc - before.notNpc);
		line(l);
		if (now.unplaced) {
			std::string list;
			for (size_t i = 0; i < nowhere.size() && i < 8; ++i) list += (i ? ", " : "") + nowhere[i];
			if (nowhere.size() > 8) list += ", ...";
			line(format("WARNING: %u of them stand at places the world does not have (not drawn): ", now.unplaced) + list);
		}
	}
	result.characters = game.stats().npcs;
	{
		const ScriptStats& vs = game.vm().stats();
		u64 stubbed = 0;
		std::vector<std::pair<u32, u32>> byCount;
		for (const auto& [symbol, count] : unboundCalls) {
			stubbed += count;
			byCount.push_back({count, symbol});
		}
		std::sort(byCount.rbegin(), byCount.rend());
		std::string list;
		for (size_t i = 0; i < byCount.size() && i < 12; ++i)
			list += format("%s%s x%u", i ? ", " : "", script.symbols[byCount[i].second].name.c_str(), byCount[i].first);
		if (byCount.size() > 12) list += ", ...";
		line(format("start-up ran %llu instructions; %u complaints, %u calls given up, %u uses of members of no object%s%s",
		            static_cast<unsigned long long>(vs.instructions), vs.errors, vs.aborted, vs.nullAccess,
		            vs.errors || vs.aborted ? "; the last complaint: " : "", vs.errors || vs.aborted ? vs.lastError : ""));
		if (!faults.kinds.empty()) {
			std::vector<std::pair<u32, std::string>> faultsByCount;
			for (const auto& [what, n] : faults.kinds) faultsByCount.push_back({n, what});
			std::sort(faultsByCount.rbegin(), faultsByCount.rend());
			std::string where;
			for (size_t i = 0; i < faultsByCount.size() && i < 8; ++i)
				where += (i ? "; " : "") + faultsByCount[i].second + " x" + std::to_string(faultsByCount[i].first);
			line("where: " + where + (faultsByCount.size() > 8 ? "; ..." : ""));
		}
		if (debugging) {
			dbgLine("");
			dbgLine(format("== the start-up scripts, all worlds: %llu instructions, %u complaints, %u calls given up, %u uses of members of no object ==",
			         static_cast<unsigned long long>(vs.instructions), vs.errors, vs.aborted, vs.nullAccess));
			dbgLine(format("script image %.2f MB; objects of the characters and items made by the start-up: %u, %.2f MB; strings made %.2f MB",
			         double(game.vm().imageBytes()) / 1048576.0, vs.instances, double(vs.instanceBytes) / 1048576.0,
			         double(vs.stringBytes) / 1048576.0));
			dbgLine("");
			dbgLine("-- complaints in the order they FIRST happened (the first is where things began to go wrong; later ones may follow from it) --");
			for (size_t i = 0; i < faults.order.size() && i < 60; ++i)
				dbgLine(format("%3zu. %s  (x%u, library: %s)", i + 1, faults.order[i].c_str(), faults.kinds[faults.order[i]],
				         libraryOf(faults.order[i].substr(faults.order[i].rfind(" in ") == std::string::npos ? 0 : faults.order[i].rfind(" in ") + 4))));
			dbgLine("");
			dbgLine("-- complaints by how often (all, up to 400 kinds) --");
			{
				std::vector<std::pair<u32, std::string>> all;
				std::map<std::string, u32> perLibrary;
				for (const auto& [what, n] : faults.kinds) {
					all.push_back({n, what});
					const size_t at = what.rfind(" in ");
					perLibrary[libraryOf(at == std::string::npos ? what : what.substr(at + 4))] += n;
				}
				std::sort(all.rbegin(), all.rend());
				for (size_t i = 0; i < all.size() && i < 400; ++i) dbgLine(format("%8u  %s", all[i].first, all[i].second.c_str()));
				dbgLine("-- complaints by library (by the function's name) --");
				for (const auto& [lib, n] : perLibrary) dbgLine(format("%8u  %s", n, lib.c_str()));
			}
			dbgLine("");
			dbgLine("-- engine functions the start-up called that this engine does not carry out (every one: they return 0 / an empty string) --");
			for (const auto& [count, symbol] : byCount) dbgLine(format("%8u  %s", count, script.symbols[symbol].name.c_str()));
		}
		if (!byCount.empty())
			line(format("engine functions the start-up called that do nothing yet: %zu, called %llu times: ", byCount.size(),
			            static_cast<unsigned long long>(stubbed)) +
			     list);
	}

	// ---- what they do all day ----
	// States are run on the console only; what can be said here is which
	// states the routines use, and which engine functions the states call
	// that the engine does not carry out yet.
	try {
		// Where every function's code begins and ends, and who is who.
		std::map<u32, u32> byAddress; // address -> symbol
		std::map<std::string, u32> byName;
		for (u32 i = 0; i < script.symbols.size(); ++i) {
			const SrcSymbol& sym = script.symbols[i];
			if (sym.member) continue;
			const bool code = (sym.type == SCRIPT_FUNC && sym.isConst && !sym.external) || sym.type == SCRIPT_PROTOTYPE ||
			                  (sym.type == SCRIPT_INSTANCE && sym.isConst);
			if (code && sym.address < script.code.size()) byAddress.emplace(sym.address, i);
			if (sym.type == SCRIPT_FUNC && sym.isConst && !sym.external && !sym.name.empty()) byName.emplace(sym.name, i);
		}
		const auto scan = [&](u32 fn, std::set<u32>& callees, std::set<u32>& externals) {
			const SrcSymbol& sym = script.symbols[fn];
			auto next = byAddress.upper_bound(sym.address);
			const u32 end = next == byAddress.end() ? u32(script.code.size()) : next->first;
			for (u32 at = sym.address; at < end;) {
				const u8 op = script.code[at];
				const u32 size = op == OP_PUSHVV ? 6u
				                 : (op == OP_BL || op == OP_BE || op == OP_PUSHI || op == OP_PUSHV || op == OP_PUSHVI || op == OP_B ||
				                    op == OP_BZ || op == OP_GMOVI)
				                       ? 5u
				                       : 1u;
				if (at + size > end) break;
				u32 operand = 0;
				if (size >= 5) std::memcpy(&operand, &script.code[at + 1], 4);
				if (op == OP_BL) {
					const auto target = byAddress.find(operand);
					if (target != byAddress.end()) callees.insert(target->second);
				} else if (op == OP_BE && operand < script.symbols.size() && script.symbols[operand].external) {
					externals.insert(operand);
				}
				at += size;
			}
		};
		struct StateUse {
			std::set<u32> externals;
		};
		std::map<u32, StateUse> states; // by the function that begins the state
		std::map<u32, std::pair<std::set<u32>, std::set<u32>>> scanned; // function -> callees, externals
		size_t reached = 0;
		for (const auto& [name, fn] : byName) {
			std::vector<u32> entry = {fn};
			for (const char* suffix : {"_LOOP", "_END"}) {
				const auto it = byName.find(name + suffix);
				if (it != byName.end()) entry.push_back(it->second);
			}
			if (entry.size() < 2) continue;
			StateUse& use = states[fn];
			std::set<u32> seen(entry.begin(), entry.end());
			std::vector<u32> todo = entry;
			while (!todo.empty()) {
				const u32 f = todo.back();
				todo.pop_back();
				auto it = scanned.find(f);
				if (it == scanned.end()) {
					it = scanned.emplace(f, std::pair<std::set<u32>, std::set<u32>>()).first;
					scan(f, it->second.first, it->second.second);
				}
				use.externals.insert(it->second.second.begin(), it->second.second.end());
				for (u32 callee : it->second.first)
					if (seen.insert(callee).second && script.symbols[callee].type == SCRIPT_FUNC) todo.push_back(callee);
			}
			reached = std::max(reached, scanned.size());
		}
		// What the people of these worlds are in by their routines, and the creatures to start with.
		std::map<u32, u32> inUse;
		for (u32 i = 0; i < game.npcCount(); ++i) {
			const GameNpc& n = game.npc(i);
			if (n.hero) continue;
			std::set<u32> own;
			for (u32 k = 0; k < n.routineCount; ++k) own.insert(game.routine(n.routineFirst + k).state);
			if (!n.routineCount)
				if (const u32 fn = game.behaviour(n)) own.insert(fn);
			for (u32 fn : own) ++inUse[fn];
		}
		u32 lifeCount = 0;
		const char* const* lifeNames = Game::lifeFunctions(lifeCount);
		const auto done = [&](u32 external) {
			if (game.vm().isBound(external)) return true;
			for (u32 k = 0; k < lifeCount; ++k)
				if (script.symbols[external].name == lifeNames[k]) return true;
			return false;
		};
		std::map<u32, u32> missing; // external -> states that call it
		std::set<u32> called;
		for (const auto& [fn, use] : states)
			for (u32 external : use.externals) {
				called.insert(external);
				if (!done(external)) ++missing[external];
			}
		std::vector<std::pair<u32, u32>> byUse;
		for (const auto& [fn, count] : inUse) byUse.push_back({count, fn});
		std::sort(byUse.rbegin(), byUse.rend());
		std::string list;
		for (size_t i = 0; i < byUse.size() && i < 20; ++i)
			list += format("%s%s x%u", i ? ", " : "", script.symbols[byUse[i].second].name.c_str(), byUse[i].first);
		line(format("life: the scripts have %zu states; the people of these worlds are put into %zu: ", states.size(), byUse.size()) +
		     list + (byUse.size() > 20 ? ", ..." : ""));
		byUse.clear();
		for (const auto& [external, count] : missing) byUse.push_back({count, external});
		std::sort(byUse.rbegin(), byUse.rend());
		list.clear();
		for (size_t i = 0; i < byUse.size() && i < 40; ++i)
			list += format("%s%s x%u", i ? ", " : "", script.symbols[byUse[i].second].name.c_str(), byUse[i].first);
		if (debugging) {
			dbgLine("");
			dbgLine("== the states of the characters (ZS_*): what they call ==");
			dbgLine(format("%zu states; %zu functions are reached from them; %zu engine functions called, %zu carried out", states.size(), scanned.size(),
			         called.size(), called.size() - missing.size()));
			std::map<std::string, u32> reachedBy;
			for (const auto& [fn, calls] : scanned) ++reachedBy[libraryOf(script.symbols[fn].name)];
			for (const auto& [lib, n] : reachedBy) dbgLine(format("  functions reached from the states, in %s: %u", lib.c_str(), n));
			dbgLine("-- engine functions the states call that this engine does not carry out, by the number of states that call them --");
			std::vector<std::pair<u32, u32>> all;
			for (const auto& [external, count] : missing) all.push_back({count, external});
			std::sort(all.rbegin(), all.rend());
			for (const auto& [count, external] : all) dbgLine(format("%8u  %s", count, script.symbols[external].name.c_str()));
		}
		line(format("life: the states call %zu engine functions, %zu of which the engine carries out; the others do nothing yet, by "
		            "the number of states that call them: ",
		            called.size(), called.size() - missing.size()) +
		     (list.empty() ? "none" : list) + (byUse.size() > 40 ? ", ..." : ""));

		// ---- what they say ----
		// The talks: every info (C_INFO) the scripts define tells what is said
		// and done by a function of its own (and the ways on it offers, which
		// it names to Info_AddChoice). Which engine functions those call is
		// the same question as for the states.
		if (game.infoCount()) {
			std::set<u32> runs, externals;
			std::vector<u32> todo;
			size_t forNpcs = 0, important = 0, permanent = 0;
			std::set<u32> owners;
			for (u32 i = 0; i < game.infoCount(); ++i) {
				const GameInfo& in = game.info(i);
				owners.insert(in.npc);
				important += in.important;
				permanent += in.permanent;
				for (u32 fn : {in.condition, in.information})
					if (fn < script.symbols.size() && script.symbols[fn].type == SCRIPT_FUNC && runs.insert(fn).second) todo.push_back(fn);
			}
			forNpcs = owners.size();
			while (!todo.empty()) {
				const u32 f = todo.back();
				todo.pop_back();
				if (script.symbols[f].external || script.symbols[f].address >= script.code.size()) continue;
				std::set<u32> callees, calledExternals;
				scan(f, callees, calledExternals);
				externals.insert(calledExternals.begin(), calledExternals.end());
				// A way on is named by passing its function on.
				const SrcSymbol& sym = script.symbols[f];
				auto next = byAddress.upper_bound(sym.address);
				const u32 end = next == byAddress.end() ? u32(script.code.size()) : next->first;
				for (u32 at = sym.address; at < end;) {
					const u8 op = script.code[at];
					const u32 size = op == OP_PUSHVV ? 6u
					                 : (op == OP_BL || op == OP_BE || op == OP_PUSHI || op == OP_PUSHV || op == OP_PUSHVI || op == OP_B ||
					                    op == OP_BZ || op == OP_GMOVI)
					                       ? 5u
					                       : 1u;
					if (at + size > end) break;
					if (op == OP_PUSHI) {
						u32 operand = 0;
						std::memcpy(&operand, &script.code[at + 1], 4);
						if (operand >= 8 && operand < script.symbols.size() && script.symbols[operand].type == SCRIPT_FUNC &&
						    script.symbols[operand].isConst && !script.symbols[operand].external && !script.symbols[operand].member)
							callees.insert(operand);
					}
					at += size;
				}
				for (u32 callee : callees)
					if (script.symbols[callee].type == SCRIPT_FUNC && runs.insert(callee).second) todo.push_back(callee);
			}
			size_t carried = 0;
			std::vector<u32> pending;
			for (u32 external : externals) {
				const bool ok = done(external);
				carried += ok;
				if (!ok) pending.push_back(external);
			}
			if (debugging) {
				dbgLine("");
				dbgLine(format("== the talks: %u infos; the %zu functions they run call %zu engine functions, %zu carried out ==", game.infoCount(),
				         runs.size(), externals.size(), carried));
				std::map<std::string, u32> runsBy;
				for (u32 fn : runs) ++runsBy[libraryOf(script.symbols[fn].name)];
				for (const auto& [lib, n] : runsBy) dbgLine(format("  functions run by the talks, in %s: %u", lib.c_str(), n));
				dbgLine("-- engine functions the talks call that this engine does not carry out --");
				for (u32 external : pending) dbgLine("  " + script.symbols[external].name);
			}
			std::string names;
			for (size_t i = 0; i < pending.size() && i < 40; ++i)
				names += format("%s%s", i ? ", " : "", script.symbols[pending[i]].name.c_str());
			line(format("dialogue: %u infos of %zu characters (%zu important, %zu permanent); the %zu functions they run call %zu engine "
			            "functions, %zu of which the engine carries out; the others do nothing yet: ",
			            game.infoCount(), forNpcs, important, permanent, runs.size(), externals.size(), carried) +
			     (names.empty() ? "none" : names) + (pending.size() > 40 ? ", ..." : ""));
		}
	} catch (const std::exception& ex) {
		line(std::string("life: the states could not be looked through: ") + ex.what());
	}

	if (debugging) {
		dbgLine("");
		dbgLine("== the scripts by library (functions by the name's start) ==");
		std::map<std::string, u32> perLibrary;
		std::map<std::string, std::string> example;
		for (const SrcSymbol& sym : script.symbols) {
			if (sym.type != SCRIPT_FUNC || !sym.isConst || sym.external || sym.member || sym.name.empty()) continue;
			const std::string lib = libraryOf(sym.name);
			if (++perLibrary[lib] == 1) example[lib] = sym.name;
		}
		for (const auto& [lib, n] : perLibrary) dbgLine(format("%8u  %s (e.g. %s)", n, lib.c_str(), example[lib].c_str()));
		u32 external = 0;
		for (const SrcSymbol& sym : script.symbols) external += sym.external;
		dbgLine(format("%u engine functions are named by the script (the engine has no code for those it lacks: they return 0 / an empty string)", external));
		dbgLine("Where the Ikarus/LeGo/Union libraries read or write the game's memory (MEM_*, ASM_*, CALL_*), this machine has no such memory:");
		dbgLine("the complaints above are that. What depends on them (hooks, FF_* timers, LeGo's screen) does not work; the plain game script does.");
		std::ofstream file(opt.debugFile, std::ios::binary);
		if (file) {
			file << "Gothic 3DS: bake_debug.txt (--debug)\n" << "script: " << script.source << "\n\n" << dbg;
			line("debug report written to " + opt.debugFile + " (" + std::to_string(dbg.size() / 1024) + " KB)");
		} else {
			line("WARNING: the debug report could not be written to " + opt.debugFile);
		}
	}

	// ---- those the scripts put in later ----
	// A talk inserts a character (Wld_InsertNpc), a spell summons one (Wld_SpawnNpcRange): their looks are wanted
	// too, or they stand there unseen. The instance is pushed as a number right before the call; every such number
	// that names a character nobody has been made of yet is made here once, in no world, to read its look off.
	// fix151: a transforming spell names its creature the same way (Npc_SetActiveSpellInfo(self, Wolf)): those are
	// made too, and kept in `formSyms` for their whole models below.
	std::set<u32> formSyms;
	{
		std::set<u32> targets, formCalls;
		for (const char* n : {"WLD_INSERTNPC", "WLD_SPAWNNPCRANGE", "WLD_INSERTNPCANDRESPAWN", "NPC_SETACTIVESPELLINFO"})
			for (u32 i = 0; i < script.symbols.size(); ++i)
				if (script.symbols[i].external && script.symbols[i].name == n) {
					targets.insert(i);
					if (std::string(n) == "NPC_SETACTIVESPELLINFO") formCalls.insert(i);
				}
		std::set<u32> have, wanted;
		for (u32 i = 0; i < game.npcCount(); ++i) have.insert(game.npc(i).symbol);
		u32 recent[6] = {};
		u32 recentCount = 0;
		for (u32 at = 0; at < script.code.size();) {
			const u8 op = script.code[at];
			const u32 size = op == OP_PUSHVV ? 6u
			                 : (op == OP_BL || op == OP_BE || op == OP_PUSHI || op == OP_PUSHV || op == OP_PUSHVI || op == OP_B ||
			                    op == OP_BZ || op == OP_GMOVI)
			                       ? 5u
			                       : 1u;
			if (at + size > script.code.size()) break;
			u32 operand = 0;
			if (size >= 5) std::memcpy(&operand, &script.code[at + 1], 4);
			if (op == OP_PUSHI) {
				recent[recentCount % 6] = operand;
				++recentCount;
			} else if (op == OP_BE) {
				if (targets.count(operand))
					for (u32 k = 0; k < 6 && k < recentCount; ++k) {
						const u32 sym = recent[k];
						if (sym < script.symbols.size() && script.symbols[sym].type == SCRIPT_INSTANCE && script.symbols[sym].isConst) {
							if (formCalls.count(operand)) formSyms.insert(sym);
							if (!have.count(sym)) wanted.insert(sym);
						}
					}
				recentCount = 0;
			} else if (op == OP_BL || op == OP_RSR) {
				recentCount = 0;
			}
			at += size;
		}
		const u32 before = game.npcCount();
		std::string list;
		u32 made = 0;
		for (u32 sym : wanted) {
			if (game.probeNpc(sym) == SCRIPT_NONE) continue; // (not a character: an item, a number that happens to be one)
			if (made < 12) list += (made ? ", " : "") + script.symbols[sym].name;
			++made;
		}
		line(format("characters the scripts put in later (talks, summoning): %u kinds not in any world at the start, made to "
		            "read their looks (%u characters before): ",
		            made, before) +
		     (list.empty() ? std::string("none") : list) + (made > 12 ? ", ..." : ""));
	}

	// ---- what they look like ----
	std::map<u32, Look> looks;
	for (u32 i = 0; i < game.npcCount(); ++i) {
		const GameNpc& n = game.npc(i);
		const GameLook& gl = n.look;
		if (n.hero) continue; // has a model of its own
		if (gl.script == 0 || (gl.body == 0 && gl.armor == 0)) continue; // no model script, no body: nothing to draw
		const u32 key = game.lookKey(gl);
		Look& l = looks[key];
		if (l.users++ == 0) {
			l.key = key;
			l.script = game.text(gl.script);
			l.body = game.text(gl.body);
			l.head = game.text(gl.head);
			l.armor = game.text(gl.armor);
			l.bodyTex = gl.bodyTex;
			l.skin = gl.skin;
			l.headTex = gl.headTex;
			l.teethTex = gl.teethTex;
			l.example = n.symbol < script.symbols.size() ? script.symbols[n.symbol].name : std::string("?");
		}
	}
	result.looks = looks.size();
	if (looks.empty() || !scripts.looks.rig || !scripts.looks.body) {
		if (!looks.empty()) line(format("%zu looks; no models for them in this bake: nobody is drawn", looks.size()));
		return;
	}

	// Animations the scripts name: every text in them that looks like one
	// ("T_STAND_2_SIT", "S_LGUARD", "R_SCRATCHHEAD"). Which of them a model
	// has is for its source to say.
	std::set<std::string> aniNames;
	for (const SrcSymbol& sym : script.symbols) {
		if (sym.type != SCRIPT_STRING || sym.member) continue;
		for (const std::string& text : sym.strings) {
			if (text.size() < 3 || text.size() > 31 || text[1] != '_') continue;
			const std::string name = upper(text);
			if (name[0] != 'T' && name[0] != 'S' && name[0] != 'R') continue;
			if (name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos) continue;
			aniNames.insert(name);
		}
	}
	// Getting up again is never named: the engine works it out from what the
	// character is in ("T_STAND_2_SIT" or "S_SIT" needs "T_SIT_2_STAND").
	{
		std::set<std::string> back;
		for (const std::string& name : aniNames) {
			std::string in;
			if (name.compare(0, 2, "S_") == 0) in = name.substr(2);
			const size_t to = name.rfind("_2_");
			if (name.compare(0, 2, "T_") == 0 && to != std::string::npos) in = name.substr(to + 3);
			if (!in.empty() && in != "STAND") back.insert("T_" + in + "_2_STAND");
		}
		aniNames.insert(back.begin(), back.end());
	}
	std::vector<std::string> usedSchemes; // the kinds of things the scripts name that the worlds have
	// The things people use, and what using them looks like: the scripts
	// name the kind ("BENCH"), the animations are named after it
	// ("T_BENCH_STAND_2_S0", "S_BENCH_S1").
	{
		std::set<std::string> texts;
		for (const SrcSymbol& sym : script.symbols) {
			if (sym.type != SCRIPT_STRING || sym.member) continue;
			for (const std::string& text : sym.strings)
				if (!text.empty() && text.size() < 48) texts.insert(upper(text));
		}
		std::map<std::string, u32> there;
		for (const BakeWorld& bw : worlds)
			for (const SrcMob& mob : bw.scene.mobs) ++there[mob.scheme];
		std::string used, unused;
		size_t usedCount = 0;
		for (const auto& [scheme, count] : there) {
			if (texts.count(scheme)) {
				usedSchemes.push_back(scheme);
				aniNames.insert("T_" + scheme + "_*");
				aniNames.insert("S_" + scheme + "_*");
				used += format("%s%s x%u", used.empty() ? "" : ", ", scheme.c_str(), count);
				++usedCount;
			} else {
				unused += (unused.empty() ? "" : ", ") + scheme;
				// (The player's character uses things the scripts never name - a lever, the grindstone - by
				// their own scheme: its animations are needed all the same.)
				aniNames.insert("T_" + scheme + "_*");
				aniNames.insert("S_" + scheme + "_*");
			}
		}
		// A ladder is climbed with the scheme LADDER ("T_LADDER_S0_2_S1"): the worlds' ladders are not among their
		// things to use, so it is asked for by name.
		aniNames.insert("T_LADDER_*");
		aniNames.insert("S_LADDER_*");
		if (!there.empty())
			line(format("things to use: the worlds have %zu kinds; the scripts name %zu of them: ", there.size(), usedCount) +
			     (used.empty() ? "none" : used) + (unused.empty() ? "" : "; not named: " + unused));
	}
	// Fighting: the engine names those animations itself (fight_clips.h).
	std::set<std::string> rigNames = aniNames;
	size_t fightNames = 0;
	for (const std::string& name : fightClipNames(false)) fightNames += rigNames.insert(name).second;
	// Archers and mages among them: the bow and the magic stance, aiming, shooting, casting (what a skeleton does not
	// declare is passed over - creatures have none of it).
	for (const std::string& name : bowClipNames()) fightNames += rigNames.insert(name).second;
	for (const std::string& name : magicClipNames()) fightNames += rigNames.insert(name).second;
	// Things in the hands (AI_UseItem, AI_UseItemToState): the animations of
	// every scheme the scripts' items have ("T_FOOD_STAND_2_S0", "S_FOOD_S0",
	// "T_FOOD_RANDOM_1"), and picking a thing up from the ground (AI_TakeItem).
	std::set<std::string> itemSchemes;
	for (u32 sym = 0; sym < game.vm().symbolCount(); ++sym)
		if (game.isItem(sym))
			if (const GameItemInfo* info = game.itemInfo(sym)) {
				const std::string scheme = upper(game.text(info->scheme));
				if (!scheme.empty() && scheme.size() < 16 && scheme.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") == std::string::npos)
					itemSchemes.insert(scheme);
			}
	size_t itemFamilies = 0;
	for (const std::string& scheme : itemSchemes) {
		itemFamilies += rigNames.insert("T_" + scheme + "_*").second;
		rigNames.insert("S_" + scheme + "_*");
	}
	for (const char* name : {"T_STAND_2_IGET", "S_IGET", "T_IGET_2_STAND", "C_STAND_2_IGET_1", "C_IGET_1", "C_IGET_2_STAND_1"}) rigNames.insert(name);
	if (!itemSchemes.empty()) {
		std::string list;
		for (const std::string& scheme : itemSchemes) list += (list.empty() ? "" : ", ") + scheme;
		line(format("things in the hands: the scripts' items are used in %zu ways (%s); their animations are asked of every skeleton",
		            itemFamilies, list.c_str()));
	}
	std::string rigNotes;
	std::vector<std::string> mobClipNotes;
	BakeOptions quiet = opt;
	quiet.quiet = true;
	quiet.sharedMeshes = true; // (fix88) bodies and heads that differ in their textures only share one mesh entry
	ModelBakeShared shared;
	struct Rig {
		SrcModel model; // kept: bodies are checked against its skeleton
		u32 hash = 0;
	};
	std::map<std::string, Rig> rigs;
	std::map<std::string, u32> requested; // body or head as asked for -> model hash (0: cannot be had)
	std::map<std::string, u32> bySignature;
	std::map<u32, SrcModel> sources; // keyed by model hash; // bodies as baked, for their coarse versions (--npc-lod)
	std::map<std::string, u32> problems; // reason -> looks
	std::map<u32, u32> meshOfModel;      // model hash -> its shared mesh entry
	std::map<u32, size_t> meshSize;      // mesh entry -> bytes
	size_t meshModels = 0;               // models that use a shared mesh
	std::vector<LookEntry> entries;
	size_t bodies = 0, heads = 0, triangles = 0, fullBodyTriangles = 0, textures = 0, clipCount = 0;
	std::set<std::string> missingTextures;
	const u64 modelsBefore = pak.rawBytes(ASSET_MODEL), clipsBefore = pak.rawBytes(ASSET_CLIP), texBefore = pak.rawBytes(ASSET_TEXTURE);

	// One piece of a look: from the source, checked, baked once.
	const auto piece = [&](const std::string& request, const std::string& kind, const std::string& what, const Rig& rig,
	                       size_t& counter, const std::function<bool(SrcModel&)>& make, std::string& why) -> u32 {
		const auto known = requested.find(request);
		if (known != requested.end()) return known->second;
		u32& out = requested[request];
		out = 0;
		SrcModel m;
		try {
			if (!make(m)) {
				why = m.notes.empty() ? kind + " not found" : m.notes.back();
				return 0;
			}
			if (!sameSkeleton(m, rig.model)) {
				why = kind + " made for another skeleton";
				return 0;
			}
			m.clips.clear();
			m.blends.clear();
			const std::string sig = signature(kind + "|" + what, m);
			const auto same = bySignature.find(sig);
			if (same != bySignature.end()) return out = same->second;
			m.name = "npc." + kind + std::to_string(counter);
			const ModelBakeResult mr = bakeModel(m, quiet, pak, &shared);
			++counter;
			triangles += mr.triangles;
			if (mr.meshEntry) {
				++meshModels;
				meshOfModel[hashName(("model/" + m.name).c_str())] = mr.meshEntry;
				meshSize[mr.meshEntry] = mr.meshBytes;
			}
			if (kind == "body") fullBodyTriangles += mr.triangles;
			textures += mr.textures;
			missingTextures.insert(mr.missingTextures.begin(), mr.missingTextures.end());
			out = hashName(("model/" + m.name).c_str());
			if (kind == "body" && opt.npcLod) sources[out] = m;
			bySignature[sig] = out;
			return out;
		} catch (const std::exception& ex) {
			why = kind + " unusable: " + ex.what();
			return 0;
		}
	};

	// The coarse body of a look (--npc-lod): the same source, cut down hard; baked once per full body.
	std::map<u32, u32> lowOf;
	size_t lowBodies = 0, lowTriangles = 0;
	BakeOptions coarse = quiet;
	coarse.simplifyModels = opt.npcLodSimplify;
	coarse.simplifyModels.enabled = true;
	const auto lowBody = [&](u32 full) -> u32 {
		if (!full) return 0;
		const auto known = lowOf.find(full);
		if (known != lowOf.end()) return known->second;
		u32& out = lowOf[full];
		out = 0;
		const auto src = sources.find(full);
		if (src == sources.end()) return 0;
		try {
			SrcModel m = src->second;
			m.name += ".lo";
			const ModelBakeResult mr = bakeModel(m, coarse, pak, &shared);
			lowTriangles += mr.triangles;
			if (mr.meshEntry) {
				++meshModels;
				meshOfModel[hashName(("model/" + m.name).c_str())] = mr.meshEntry;
				meshSize[mr.meshEntry] = mr.meshBytes;
			}
			++lowBodies;
			out = hashName(("model/" + m.name).c_str());
		} catch (const std::exception&) {
			out = 0;
		}
		return out;
	};

	for (const auto& [key, l] : looks) {
		std::string why;
		// The skeleton and its idle animation.
		auto rigIt = rigs.find(l.script);
		if (rigIt == rigs.end()) {
			Rig rig;
			try {
				if (scripts.looks.rig(l.script, !l.head.empty(), rigNames, rig.model)) {
					rig.model.parts.clear();
					rig.model.blends.clear();
					rig.model.name = "npc.rig" + std::to_string(rigs.size());
					if (rig.model.clips.empty()) {
						why = "no animation to stand in";
					} else {
						const ModelBakeResult mr = bakeModel(rig.model, quiet, pak, &shared);
						describeMobClips(l.script, rig.model, usedSchemes, mobClipNotes);
						clipCount += mr.clips;
						rigNotes += format("%s%s %zu", rigNotes.empty() ? "" : ", ", l.script.c_str(), mr.clips);
						rig.hash = hashName(("model/" + rig.model.name).c_str());
					}
				} else {
					why = rig.model.notes.empty() ? "no skeleton" : rig.model.notes.back();
				}
			} catch (const std::exception& ex) {
				why = std::string("skeleton unusable: ") + ex.what();
				rig.hash = 0;
			}
			rig.model.clips.clear(); // only the skeleton is needed from here on
			rigIt = rigs.emplace(l.script, std::move(rig)).first;
			if (!why.empty()) rigIt->second.model.notes.push_back(why);
		}
		const Rig& rig = rigIt->second;
		u32 body = 0, head = 0;
		if (!rig.hash) {
			if (why.empty()) why = rig.model.notes.empty() ? "no skeleton" : rig.model.notes.back();
		} else {
			const std::string mesh = l.armor.empty() ? l.body : l.armor;
			body = piece(format("b|%s|%s|%d|%d", l.script.c_str(), mesh.c_str(), l.bodyTex, l.skin), "body", l.script + "/" + mesh,
			             rig, bodies,
			             [&](SrcModel& m) { return scripts.looks.body(l.script, mesh, l.bodyTex, l.skin, m); }, why);
			if (body && !l.head.empty() && scripts.looks.head) {
				std::string headWhy;
				head = piece(format("h|%s|%s|%d|%d|%d", l.script.c_str(), l.head.c_str(), l.headTex, l.skin, l.teethTex), "head",
				             l.script + "/" + l.head, rig, heads,
				             [&](SrcModel& m) { return scripts.looks.head(l.script, l.head, l.headTex, l.skin, l.teethTex, m); },
				             headWhy);
				// A character without its head is still better than none.
				if (!head) ++problems["head missing (drawn without): " + (headWhy.empty() ? l.head : headWhy)];
			}
		}
		if (!body) {
			++problems[(why.empty() ? std::string("no body") : why) + " [" + l.script + "/" + (l.armor.empty() ? l.body : l.armor) +
			           ", e.g. " + l.example + "]"];
			continue;
		}
		u32 low = 0;
		if (opt.npcLod) {
			low = lowBody(body);
		}
		entries.push_back(LookEntry {key, rig.hash, body, head, low});
	}

	std::sort(entries.begin(), entries.end(), [](const LookEntry& a, const LookEntry& b) { return a.key < b.key; });
	std::vector<u8> blob(sizeof(LooksHeader) + entries.size() * sizeof(LookEntry));
	LooksHeader lh {LOOKS_MAGIC, LOOKS_VERSION, u32(entries.size()), 0};
	std::memcpy(blob.data(), &lh, sizeof(lh));
	if (!entries.empty()) std::memcpy(blob.data() + sizeof(lh), entries.data(), entries.size() * sizeof(LookEntry));
	pak.add("meta/looks", ASSET_META, blob);

	// The meshes most of the people wear (fix88): the engine reads them when a world is entered and keeps them, so that
	// whoever comes into range wears a mesh that is already in memory. By the characters that wear them, most first, as
	// many as fit a budget.
	if (!meshSize.empty()) {
		std::map<u32, u32> uses; // mesh entry -> characters
		std::map<u32, const LookEntry*> byKey;
		for (const LookEntry& e : entries) byKey[e.key] = &e;
		for (const auto& [key, l] : looks) {
			const auto it = byKey.find(key);
			if (it == byKey.end()) continue;
			for (u32 model : {it->second->body, it->second->head, it->second->bodyLow}) {
				const auto m = meshOfModel.find(model);
				if (m != meshOfModel.end()) uses[m->second] += l.users;
			}
		}
		std::vector<std::pair<u32, u32>> ranked; // (characters, mesh entry)
		for (const auto& [mesh, n] : uses)
			if (n >= 2) ranked.push_back({n, mesh});
		std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
		const size_t budget = 3u << 20;
		size_t bytes = 0, charactersCovered = 0, charactersAll = 0;
		for (const auto& [mesh, n] : uses) charactersAll += n;
		std::vector<u32> hot;
		for (const auto& [n, mesh] : ranked) {
			if (hot.size() >= 200 || bytes + meshSize[mesh] > budget) continue;
			hot.push_back(mesh);
			bytes += meshSize[mesh];
			charactersCovered += n;
		}
		std::vector<u8> hotBlob(sizeof(HotMeshHeader) + hot.size() * sizeof(u32));
		const HotMeshHeader hh {HOTMESH_MAGIC, u32(hot.size())};
		std::memcpy(hotBlob.data(), &hh, sizeof(hh));
		if (!hot.empty()) std::memcpy(hotBlob.data() + sizeof(hh), hot.data(), hot.size() * sizeof(u32));
		pak.add("meta/hotmeshes", ASSET_META, hotBlob);
		size_t totalBytes = 0;
		for (const auto& [mesh, size] : meshSize) totalBytes += size;
		line(format("shared meshes: %zu models share %zu meshes (%.2f MB); the engine keeps the %zu most worn (%.2f MB) in memory: they dress %zu of %zu body and head places",
		            meshModels, meshSize.size(), double(totalBytes) / 1048576.0, hot.size(), double(bytes) / 1048576.0, charactersCovered, charactersAll));
	}

	// ---- overlays (Mdl_ApplyOverlayMds: the militia's walk, the mage's, a learnt fight talent's blows) ----
	// Of every skeleton the inhabitants have, and of the player's (humans): the overlays the game has that the
	// scripts name. Each is a model of its own, "model/ovl.<name>" (the skeleton and the overlay's animations), read
	// when somebody wears it.
	if (scripts.looks.overlay && scripts.looks.overlays) {
		std::set<std::string> named; // upper case stems the scripts mention ("HUMANS_MILITIA")
		for (const SrcSymbol& sym : script.symbols)
			for (const std::string& str : sym.strings) {
				const std::string u = upper(str);
				if (u.size() > 4 && u.compare(u.size() - 4, 4, ".MDS") == 0) named.insert(u.substr(0, u.size() - 4));
			}
		// (And the literal arguments of the calls that apply one: the strings of the symbols may miss them.)
		size_t fromStrings = named.size(), fromCalls = 0;
		for (const char* fn : {"MDL_APPLYOVERLAYMDS", "MDL_APPLYOVERLAYMDSTIMED"}) {
			const u32 callee = game.vm().find(fn);
			if (callee == SCRIPT_NONE) continue;
			const u32 args = std::string(fn) == "MDL_APPLYOVERLAYMDS" ? 2u : 3u;
			static u32 sites[256 * 3];
			const u32 found = std::min<u32>(game.vm().callSites(callee, args, sites, 256), 256);
			for (u32 k = 0; k < found; ++k) {
				const u32 sym = sites[k * args + 1];
				if (sym >= game.vm().symbolCount() || game.vm().symbol(sym).type != SCRIPT_STRING) continue;
				std::string u = upper(game.vm().getString(sym));
				if (u.size() > 4 && u.compare(u.size() - 4, 4, ".MDS") == 0) u.resize(u.size() - 4);
				if (named.insert(u).second) ++fromCalls;
			}
		}
		std::set<std::string> bases {"HUMANS"};
		for (const auto& r : rigs)
			if (r.second.hash) bases.insert(upper(r.first));
		size_t overlays = 0, overlayClips = 0;
		std::string list, failed;
		size_t candidates = 0, candidatesNamed = 0;
		for (const std::string& base : bases) {
			for (const std::string& ovl : scripts.looks.overlays(base)) {
				++candidates;
				if (named.count(ovl)) ++candidatesNamed;
				if (!named.count(ovl)) continue;
				SrcModel m;
				try {
					if (!scripts.looks.overlay(base, ovl, m)) {
						failed += (failed.empty() ? "" : ", ") + ovl;
						continue;
					}
					m.parts.clear();
					m.blends.clear();
					std::string lowerName = ovl;
					for (char& c : lowerName) c = char(std::tolower(static_cast<unsigned char>(c)));
					m.name = "ovl." + lowerName;
					const ModelBakeResult mr = bakeModel(m, quiet, pak, &shared);
					++overlays;
					overlayClips += mr.clips;
					if (overlays <= 16) list += format("%s%s %zu", list.empty() ? "" : ", ", ovl.c_str(), mr.clips);
				} catch (const std::exception& ex) {
					failed += (failed.empty() ? "" : ", ") + ovl + " (" + ex.what() + ")";
				}
			}
		}
		line(format("overlays: %zu baked (%zu animations): ", overlays, overlayClips) + (list.empty() ? std::string("none") : list) +
		     (overlays > 16 ? ", ..." : "") + (failed.empty() ? "" : "; not usable: " + failed));
		line(format("overlays: the scripts name %zu (%zu in strings, %zu more in calls to apply one); the game has %zu for %zu skeletons, "
		            "%zu of them named%s",
		            named.size(), fromStrings, fromCalls, candidates, bases.size(), candidatesNamed,
		            named.empty() ? " - the scripts' names could not be read" : candidates == 0 ? " - no files <SKELETON>_*.MSB/.MDS found" : ""));
	}

	// ---- transformations (fix151) ----
	// What a transforming spell makes the player's character: the creature whole, "model/form.<instance>" (lower case),
	// with the animations the hero's own movement and fists ask for, the way OpenGothic's Npc::commitSpell gives him the
	// creature's instance and its visual (Npc_SetActiveSpellInfo). Its look is read off a character of that instance.
	if (scripts.looks.form && !formSyms.empty()) {
		std::string list, failed;
		size_t forms = 0, formClips = 0;
		std::vector<FormEntry> formEntries;
		const u64 formBytes = pak.rawBytes(ASSET_MODEL) + pak.rawBytes(ASSET_CLIP) + pak.rawBytes(ASSET_TEXTURE);
		for (u32 sym : formSyms) {
			const GameNpc* who = nullptr;
			for (u32 i = 0; i < game.npcCount() && !who; ++i)
				if (game.npc(i).symbol == sym && !game.npc(i).hero) who = &game.npc(i);
			const std::string name = script.symbols[sym].name;
			if (!who) continue; // (a number that is no character: Npc_SetActiveSpellInfo is given other things too)
			const GameLook& gl = who->look;
			const std::string mesh = gl.armor ? game.text(gl.armor) : game.text(gl.body);
			if (!gl.script || mesh.empty()) {
				failed += (failed.empty() ? "" : ", ") + name + " (no look)";
				continue;
			}
			SrcModel m;
			try {
				if (!scripts.looks.form(game.text(gl.script), mesh, gl.bodyTex, gl.skin, m)) {
					failed += (failed.empty() ? "" : ", ") + name + " (" + (m.notes.empty() ? std::string("not found") : m.notes.back()) + ")";
					continue;
				}
				std::string lowerName = name;
				for (char& c : lowerName) c = char(std::tolower(static_cast<unsigned char>(c)));
				m.name = "form." + lowerName;
				BakeOptions formOpt = opt; // (as the hero is baked: a mesh of its own, not a shared one)
				formOpt.quiet = true;
				const ModelBakeResult mr = bakeModel(m, formOpt, pak);
				formEntries.push_back(FormEntry {hashName(name.c_str()), hashName(("model/" + m.name).c_str())});
				++forms;
				formClips += mr.clips;
				list += format("%s%s %zu", list.empty() ? "" : ", ", name.c_str(), mr.clips);
			} catch (const std::exception& ex) {
				failed += (failed.empty() ? "" : ", ") + name + " (" + ex.what() + ")";
			}
		}
		if (!formEntries.empty()) {
			const FormsHeader fh {FORMS_MAGIC, FORMS_VERSION, u32(formEntries.size()), 0};
			std::vector<u8> blob(sizeof(fh) + formEntries.size() * sizeof(FormEntry));
			std::memcpy(blob.data(), &fh, sizeof(fh));
			std::memcpy(blob.data() + sizeof(fh), formEntries.data(), formEntries.size() * sizeof(FormEntry));
			pak.add("meta/forms", ASSET_META, blob);
		}
		const u64 after = pak.rawBytes(ASSET_MODEL) + pak.rawBytes(ASSET_CLIP) + pak.rawBytes(ASSET_TEXTURE);
		line(format("transformations: %zu creatures baked whole for the player (%zu animations, %.2f MB): ", forms, formClips,
		            double(after - formBytes) / 1048576.0) +
		     (list.empty() ? std::string("none") : list) + (failed.empty() ? "" : "; not usable: " + failed));
	} else if (!formSyms.empty()) {
		line(format("transformations: the scripts name %zu creatures, but this bake has no source for whole models", formSyms.size()));
	}

	size_t rigCount = 0;
	for (const auto& r : rigs) rigCount += r.second.hash != 0;
	result.looksBaked = entries.size();
	result.npcModels = rigCount + bodies + heads + lowBodies;
	if (lowBodies) line(format("coarse bodies (--npc-lod): %zu, %zu triangles (the full bodies they stand in for: %zu)", lowBodies, lowTriangles, fullBodyTriangles));
	u32 drawn = 0;
	for (const auto& [key, l] : looks)
		for (const LookEntry& e : entries)
			if (e.key == key) drawn += l.users;
	line(format("inhabitants: %zu characters with %zu different looks; models for %zu of them (%u characters): %zu skeletons "
	            "with %zu animations, %zu bodies, %zu heads; %zu triangles, %zu textures, %.2f MB of models, %.2f MB of "
	            "animations and %.2f MB of textures",
	            result.characters, looks.size(), entries.size(), drawn, rigCount, clipCount, bodies, heads, triangles, textures,
	            double(pak.rawBytes(ASSET_MODEL) - modelsBefore) / 1048576.0, double(pak.rawBytes(ASSET_CLIP) - clipsBefore) / 1048576.0,
	            double(pak.rawBytes(ASSET_TEXTURE) - texBefore) / 1048576.0));
	{
		size_t named = 0, families = 0;
		for (const std::string& name : aniNames) (name.back() == '*' ? families : named) += 1;
		line(format("animations: the scripts name or imply %zu%s; fighting %zu more; baked per skeleton: ", named,
		            families ? format(", and using the things they name takes %zu families more", families).c_str() : "", fightNames) +
		     rigNotes);
	}
	for (const std::string& note : mobClipNotes) line(note);
	if (!problems.empty()) {
		line(format("WARNING: %zu kinds of trouble with the inhabitants' models:", problems.size()));
		size_t shown = 0;
		for (const auto& [what, count] : problems) {
			if (++shown > 16) {
				line("  ...");
				break;
			}
			line(format("  %u looks: ", count) + what);
		}
	}
	if (!missingTextures.empty()) {
		std::string list;
		size_t shown = 0;
		for (const std::string& t : missingTextures) {
			if (++shown > 12) break;
			list += (list.empty() ? "" : ", ") + t;
		}
		if (missingTextures.size() > 12) list += ", ...";
		line(format("WARNING: %zu textures of the inhabitants not found (drawn untextured): ", missingTextures.size()) + list);
	}


	// ---- particle effects: the pictures of every C_PARTICLEFX of the scripts, and the effects the worlds carry ----
	// (Gothic 1 keeps them in PARTICLEFX.DAT, the spells' effects in VISUALFX.DAT: scripts of their own.)
	ScriptVm particleVm, visualVm;
	const bool haveParticles = sideScript(scripts.particles, "script/particlefx", particleVm);
	sideScript(scripts.visualfx, "script/visualfx", visualVm);
	{
		ScriptVm& pvm = haveParticles ? particleVm : game.vm();
		const u32 cls = pvm.find("C_PARTICLEFX");
		const u32 member = cls == SCRIPT_NONE ? SCRIPT_NONE : pvm.findMember("C_PARTICLEFX", "VISNAME_S");
		std::set<std::string> effects;           // upper case names of the instances
		std::set<std::string> wanted;            // picture names, lower case without extension
		u32 instances = 0, noPicture = 0;
		if (cls != SCRIPT_NONE && member != SCRIPT_NONE) {
			for (u32 sym = 0; sym < pvm.symbolCount(); ++sym) {
				const ScriptSymbol& sy = pvm.symbol(sym);
				if (sy.type != SCRIPT_INSTANCE || !(sy.flags & SCRIPTF_CONST) || pvm.classOf(sym) != cls) continue;
				ScriptInstance* inst = pvm.create(sym);
				if (!inst) continue;
				++instances;
				std::string pic = pvm.getString(member, 0, inst);
				pvm.destroy(inst);
				std::transform(pic.begin(), pic.end(), pic.begin(), [](unsigned char c) { return char(std::tolower(c)); });
				const size_t dot = pic.rfind('.');
				if (dot != std::string::npos) pic.resize(dot);
				if (pic.empty()) {
					++noPicture;
					continue;
				}
				wanted.insert(pic);
			}
		}
		// Frames of an animated picture: NAME_A0, NAME_A1 ...
		size_t baked = 0;
		std::vector<std::string> missingPics;
		if (scripts.looks.texture) {
			const auto bakeOne = [&](const std::string& name) {
				const std::string pakName = "tex/" + name;
				if (pak.has(pakName)) return true;
				SrcImage img;
				if (!scripts.looks.texture(name + ".tga", img)) return false;
				pak.add(pakName, ASSET_TEXTURE, bakeTexture(img, opt.tex));
				++baked;
				return true;
			};
			for (const std::string& name : wanted) {
				if (!bakeOne(name)) {
					missingPics.push_back(name);
					continue;
				}
				const size_t a = name.find("_a0");
				if (a == std::string::npos) continue;
				for (size_t k = 1; k < 32; ++k)
					if (!bakeOne(name.substr(0, a) + "_a" + std::to_string(k) + name.substr(a + 3))) break;
			}
		}
		size_t emitters = 0, unknownEffects = 0;
		for (size_t w = 0; w < worlds.size(); ++w) {
			std::vector<WorldEmitter> table;
			for (const SrcEmitter& e : worlds[w].scene.emitters) {
				const u32 sym = pvm.findHash(hashName(e.effect.c_str()));
				if (sym == SCRIPT_NONE || pvm.classOf(sym) != cls) {
					++unknownEffects;
					continue;
				}
				table.push_back({hashName(e.effect.c_str()), {e.pos[0], e.pos[1], e.pos[2]}, e.yaw});
			}
			if (table.empty()) continue;
			std::vector<u8> emBlob(emittersBlobSize(u32(table.size())));
			const EmittersHeader h {EMITTERS_MAGIC, EMITTERS_VERSION, u32(table.size()), 0};
			std::memcpy(emBlob.data(), &h, sizeof(h));
			std::memcpy(emBlob.data() + sizeof(h), table.data(), table.size() * sizeof(WorldEmitter));
			pak.add("world/" + worlds[w].name + "/emitters", ASSET_META, emBlob);
			emitters += table.size();
		}
		if (instances || emitters || unknownEffects)
			line(format("particles: %u effects in the scripts (%zu pictures baked, %zu of the names have none), %zu emitters in the worlds%s",
			            instances, baked, missingPics.size(), emitters,
			            unknownEffects ? format(", %zu with an effect the scripts lack", unknownEffects).c_str() : ""));
		if (!missingPics.empty()) {
			std::string list;
			for (size_t i = 0; i < missingPics.size() && i < 12; ++i) list += (i ? ", " : "") + missingPics[i];
			line("  pictures not found: " + list + (missingPics.size() > 12 ? ", ..." : ""));
		}
		(void)noPicture;
	}

	// ---- fix150: the Barrier's dome (g3ds/barrier.h) ----
	if (scripts.looks.barrier) {
		SrcBarrier src;
		std::string why;
		if (!scripts.looks.barrier(src, why)) {
			line("barrier: " + why + "; the dome is not drawn");
		} else {
			const size_t nv = src.pos.size() / 3;
			if (nv > 65535) {
				line(format("barrier: %s has %zu corners (at most 65535); the dome is not drawn", src.mesh.c_str(), nv));
			} else {
				BarrierHeader h {};
				h.magic = BARRIER_MAGIC;
				h.version = BARRIER_VERSION;
				h.vertexCount = u32(nv);
				h.indexCount = u32(src.indices.size() / 3 * 3);
				for (int a = 0; a < 3; ++a) {
					h.bbMin[a] = 1e30f;
					h.bbMax[a] = -1e30f;
				}
				for (size_t i = 0; i < nv; ++i)
					for (int a = 0; a < 3; ++a) {
						h.bbMin[a] = std::min(h.bbMin[a], src.pos[i * 3 + size_t(a)]);
						h.bbMax[a] = std::max(h.bbMax[a], src.pos[i * 3 + size_t(a)]);
					}
				float uvMin[2] = {1e30f, 1e30f}, uvMax[2] = {-1e30f, -1e30f};
				std::vector<BarrierVertex> verts(nv);
				for (size_t i = 0; i < nv; ++i) {
					BarrierVertex& v = verts[i];
					for (int a = 0; a < 3; ++a) v.pos[a] = src.pos[i * 3 + size_t(a)];
					for (int a = 0; a < 2; ++a) {
						v.uv[a] = src.uv[i * 2 + size_t(a)];
						uvMin[a] = std::min(uvMin[a], v.uv[a]);
						uvMax[a] = std::max(uvMax[a], v.uv[a]);
					}
					v.fade = barrierFadeAt(v.pos[1], h.bbMin[1], h.bbMax[1]);
					if (v.pos[1] > verts[h.top].pos[1]) h.top = u32(i);
				}
				// Its texture, as the material names it (BARRIERE.TGA); always repeated, its own alpha not used.
				// fix184: oCBarrier puts BARRIERE.TGA on the dome itself; the mesh's own material names another (Gothic 1:
				// MODIALPHA02, 16x16). The material's is taken only when the game has no BARRIERE.TGA.
				std::string texNote = "no texture named";
				if (scripts.looks.texture) {
					SrcImage probe;
					if (pak.has(texturePakName("BARRIERE.TGA")) || scripts.looks.texture("BARRIERE.TGA", probe)) {
						if (!src.texture.empty() && src.texture != "BARRIERE.TGA") texNote = "material's " + src.texture + " replaced; ";
						src.texture = "BARRIERE.TGA";
					}
				}
				if (!src.texture.empty()) {
					const std::string pakName = texturePakName(src.texture);
					SrcImage img;
					const std::string before = texNote.compare(0, 11, "material's ") == 0 ? texNote : std::string();
					if (pak.has(pakName)) {
						h.texture = hashName(pakName.c_str());
						texNote = before + pakName + " (already baked)";
					} else if (scripts.looks.texture && scripts.looks.texture(src.texture, img)) {
						for (size_t p = 3; p < img.rgba.size(); p += 4) img.rgba[p] = 255;
						pak.add(pakName, ASSET_TEXTURE, bakeTexture(img, opt.tex));
						h.texture = hashName(pakName.c_str());
						texNote = before + format("%s %dx%d", pakName.c_str(), img.width, img.height);
					} else {
						texNote = src.texture + " not found";
					}
				}
				std::vector<u8> barrierBlob(barrierBlobSize(h.vertexCount, h.indexCount));
				std::memcpy(barrierBlob.data(), &h, sizeof(h));
				std::memcpy(barrierBlob.data() + sizeof(h), verts.data(), nv * sizeof(BarrierVertex));
				u8* idx = barrierBlob.data() + sizeof(h) + nv * sizeof(BarrierVertex);
				for (u32 i = 0; i < h.indexCount; ++i) {
					const u16 x = u16(src.indices[i]);
					std::memcpy(idx + i * 2u, &x, 2);
				}
				pak.add("fx/barrier", ASSET_META, barrierBlob);
				line(format("barrier: %s, %u corners, %u triangles, box (%.0f %.0f %.0f)..(%.0f %.0f %.0f) m, uv (%.2f %.2f)..(%.2f %.2f), "
				            "top corner at (%.0f %.0f %.0f) m; texture %s",
				            src.mesh.c_str(), h.vertexCount, h.indexCount / 3, h.bbMin[0] * 0.01, h.bbMin[1] * 0.01, h.bbMin[2] * 0.01,
				            h.bbMax[0] * 0.01, h.bbMax[1] * 0.01, h.bbMax[2] * 0.01, uvMin[0], uvMin[1], uvMax[0], uvMax[1],
				            verts[h.top].pos[0] * 0.01, verts[h.top].pos[1] * 0.01, verts[h.top].pos[2] * 0.01, texNote.c_str()));
			}
		}
	}

	// ---- the things lying about: the world files' and the start-up scripts' ----
	{
		std::map<std::string, u32> unknown;  // instances the scripts do not have
		std::map<std::string, u32> nowhere;  // places Wld_InsertItem names that the world lacks
		std::map<std::string, u32> visuals;  // visual -> how many lie about
		size_t fromFiles = 0, fromScripts = 0;
		for (size_t w = 0; w < worlds.size(); ++w) {
			const SrcScene& scene = worlds[w].scene;
			std::vector<WorldItem> table;
			const auto add = [&](u32 symbol, const float pos[3], const float rot[4], i32 amount, u32 flags) {
				const GameItemInfo* info = game.itemInfo(symbol);
				if (!info) return;
				WorldItem wi {};
				std::copy_n(pos, 3, wi.pos);
				std::copy_n(rot, 4, wi.rot);
				wi.instance = game.vm().symbol(symbol).nameHash;
				wi.amount = std::max(amount, i32(1));
				wi.flags = flags;
				table.push_back(wi);
				++visuals[game.text(info->visual)];
			};
			for (const SrcWorldItem& it : scene.items) {
				const u32 s = game.vm().findHash(hashName(it.instance.c_str()));
				if (s == SCRIPT_NONE || !game.isItem(s)) {
					++unknown[it.instance];
					continue;
				}
				add(s, it.pos, it.rot, it.amount, 0);
			}
			const size_t files = table.size();
			for (u32 d = 0; d < game.dropCount(); ++d) {
				const GameDrop& drop = game.drop(d);
				if (drop.world != gameWorld[w]) continue;
				float pos[3], yaw = 0;
				const std::string where = game.text(drop.point);
				bool found = findPoint(const_cast<SrcScene*>(&scene), where.c_str(), pos, yaw);
				for (const auto* list : {&scene.wayPoints, &scene.freePoints})
					for (const SrcWayPoint& p : *list)
						if (!found && p.name == upper(where)) {
							std::copy_n(p.pos, 3, pos);
							yaw = p.yaw;
							found = true;
						}
				if (!found) {
					++nowhere[where];
					continue;
				}
				const float rot[4] = {0, std::sin(yaw * 0.5f), 0, std::cos(yaw * 0.5f)};
				add(drop.symbol, pos, rot, 1, WORLD_ITEM_SCRIPT);
			}
			fromFiles += files;
			fromScripts += table.size() - files;
			std::vector<u8> itemsBlob(sizeof(ItemsHeader) + table.size() * sizeof(WorldItem));
			const ItemsHeader ih {ITEMS_MAGIC, ITEMS_VERSION, u32(table.size()), u32(table.size() - files)};
			std::memcpy(itemsBlob.data(), &ih, sizeof(ih));
			if (!table.empty()) std::memcpy(itemsBlob.data() + sizeof(ih), table.data(), table.size() * sizeof(WorldItem));
			pak.add("world/" + worlds[w].name + "/items", ASSET_META, itemsBlob);
		}
		// The cards of the chapters (IntroduceChapter's texture): the picture named in each call. Each
		// is fitted into the top screen (letterboxed on black) and kept as RGB565: "card/<name>".
		{
			std::set<std::string> names;
			ScriptVm& cvm = game.vm();
			// (The third argument of every IntroduceChapter call that names its picture as a literal.)
			const u32 intro = cvm.find("INTRODUCECHAPTER");
			if (intro != SCRIPT_NONE) {
				u32 sites[64 * 5] = {};
				const u32 found = std::min<u32>(cvm.callSites(intro, 5, sites, 64), 64);
				for (u32 k = 0; k < found; ++k) {
					const u32 sym = sites[k * 5 + 2];
					if (sym >= cvm.symbolCount() || cvm.symbol(sym).type != SCRIPT_STRING) continue;
					std::string text = cvm.getString(sym);
					std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
					const size_t dot = text.rfind('.');
					if (dot != std::string::npos) text.resize(dot);
					if (!text.empty() && text.size() < 40 && text.find_first_of("*/\\") == std::string::npos) names.insert(text);
				}
			}
			size_t cards = 0, cardBytes = 0;
			std::string absent;
			for (const std::string& name : names) {
				SrcImage img;
				if (!scripts.looks.texture || !scripts.looks.texture(name + ".tga", img) || img.width < 2 || img.height < 2) {
					absent += (absent.empty() ? "" : " ") + name;
					continue;
				}
				// Fit: the whole picture inside CARD_W x CARD_H, its proportions kept.
				const float scale = std::min(float(CARD_W) / float(img.width), float(CARD_H) / float(img.height));
				const int dw = std::max(1, int(float(img.width) * scale + 0.5f)), dh = std::max(1, int(float(img.height) * scale + 0.5f));
				const int ox = (CARD_W - dw) / 2, oy = (CARD_H - dh) / 2;
				std::vector<u8> blob(sizeof(CardHeader) + size_t(CARD_W) * CARD_H * 2, 0);
				const CardHeader ch {CARD_MAGIC, CARD_VERSION, u16(CARD_W), u16(CARD_H), 0};
				std::memcpy(blob.data(), &ch, sizeof(ch));
				u16* px = reinterpret_cast<u16*>(blob.data() + sizeof(ch));
				for (int y = 0; y < dh; ++y)
					for (int x = 0; x < dw; ++x) {
						// (A box over the source texels this pixel covers.)
						const int sx0 = x * img.width / dw, sx1 = std::max(sx0 + 1, (x + 1) * img.width / dw);
						const int sy0 = y * img.height / dh, sy1 = std::max(sy0 + 1, (y + 1) * img.height / dh);
						u32 sum[3] = {0, 0, 0}, n = 0;
						for (int sy = sy0; sy < sy1 && sy < img.height; ++sy)
							for (int sx = sx0; sx < sx1 && sx < img.width; ++sx) {
								const u8* c = &img.rgba[(size_t(sy) * img.width + size_t(sx)) * 4];
								for (int k = 0; k < 3; ++k) sum[k] += c[k];
								++n;
							}
						if (!n) continue;
						const u32 r = sum[0] / n, g = sum[1] / n, b = sum[2] / n;
						px[size_t(oy + y) * CARD_W + size_t(ox + x)] = u16(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
					}
				pak.add("card/" + name, ASSET_META, blob);
				++cards;
				cardBytes += blob.size();
			}
			line(format("chapter cards: %zu pictures named by IntroduceChapter calls, %zu baked (%.2f MB)%s%s", names.size(), cards,
			            double(cardBytes) / 1048576.0, absent.empty() ? "" : "; not in the game's files: ", absent.c_str()));
		}
		const size_t lying = visuals.size();
		// Every thing of the scripts may end up in the inventory: all their
		// looks get a model ("model/item.<visual>": the top screen turns it in
		// the info box, the world shows it lying about) and a picture
		// ("icon/<visual>": the bottom screen's grid).
		for (u32 sym = 0; sym < game.vm().symbolCount(); ++sym)
			if (game.isItem(sym))
				if (const GameItemInfo* info = game.itemInfo(sym)) visuals.emplace(game.text(info->visual), 0u);
		// The movers' meshes (gates, lifts) are drawn the same way: "model/item.<visual>", no picture.
		std::set<std::string> moverOnly;
		for (const auto& bw : worlds)
			for (const SrcTrigger& t : bw.scene.triggers)
				if ((t.kind == TRG_MOVER || t.kind == TRG_DOOR) && !t.visual.empty() && visuals.emplace(t.visual, 0u).second) moverOnly.insert(t.visual);
		size_t baked = 0, itemTriangles = 0, icons = 0;
		std::map<std::string, std::string> missing;
		const u64 itemBytesBefore = pak.rawBytes(ASSET_MODEL), iconBytesBefore = pak.rawBytes(ASSET_META);
		for (const auto& [visual, count] : visuals) {
			if (visual.empty()) continue;
			SrcModel m;
			try {
				if (!scripts.looks.item || !scripts.looks.item(visual, m)) {
					missing[visual] = m.notes.empty() ? "no mesh" : m.notes.back();
					continue;
				}
				m.clips.clear();
				m.name = "item." + visual;
				const std::vector<u8> icon = moverOnly.count(visual) ? std::vector<u8>() : renderItemIcon(m, ITEMICON_SIZE);
				if (!icon.empty()) {
					pak.add("icon/" + visual, ASSET_META, icon);
					++icons;
				}
				const ModelBakeResult mr = bakeModel(m, quiet, pak, &shared);
				itemTriangles += mr.triangles;
				missingTextures.insert(mr.missingTextures.begin(), mr.missingTextures.end());
				++baked;
			} catch (const std::exception& ex) {
				missing[visual] = ex.what();
			}
		}
		line(format("things lying about: %zu (%zu from the world files, %zu from the start-up scripts) of %zu kinds of look; "
		            "looks of all things of the scripts: %zu, %zu models baked (%zu triangles, %.2f MB), %zu pictures for the "
		            "inventory (%.2f MB)",
		            fromFiles + fromScripts, fromFiles, fromScripts, lying, visuals.size(), baked, itemTriangles,
		            double(pak.rawBytes(ASSET_MODEL) - itemBytesBefore) / 1048576.0, icons,
		            double(pak.rawBytes(ASSET_META) - iconBytesBefore) / 1048576.0));
		const auto listOf = [](const std::map<std::string, u32>& m) {
			std::string list;
			size_t shown = 0;
			for (const auto& [name, n] : m) {
				if (++shown > 10) {
					list += ", ...";
					break;
				}
				list += format("%s%s x%u", list.empty() ? "" : ", ", name.c_str(), n);
			}
			return list;
		};
		if (!unknown.empty()) line(format("WARNING: %zu kinds of things in the world files the scripts do not know (left out): ", unknown.size()) + listOf(unknown));
		if (!nowhere.empty()) line(format("WARNING: the scripts put things at %zu places the worlds lack (left out): ", nowhere.size()) + listOf(nowhere));
		if (!missing.empty()) {
			std::string list;
			size_t shown = 0;
			for (const auto& [v, why] : missing) {
				if (++shown > 8) {
					list += "; ...";
					break;
				}
				list += (list.empty() ? "" : "; ") + why;
			}
			line(format("WARNING: %zu looks of things not baked (they lie there unseen): ", missing.size()) + list);
		}
	}
}

} // namespace g3
