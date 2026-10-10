#include "populate.h"
#include "fight_ai.h"
#include "fight_clips.h"

#include "game.h"
#include "g3ds/game.h"
#include "g3ds/math.h"
#include "model_bake.h"
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

	struct Faults {
		const SrcScript* script;
		std::map<std::string, u32> kinds;
	} faults {&script, {}};
	game.vm().onError(
	    [](const char* what, u32 function, u32, void* user) {
		    Faults& f = *static_cast<Faults*>(user);
		    const std::string where = function < f.script->symbols.size() ? f.script->symbols[function].name : std::string("?");
		    if (f.kinds.size() < 4096) ++f.kinds[std::string(what) + " in " + where];
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
		const u32 npcsBefore = game.npcCount();
		const bool hasStartup = script.find("STARTUP_" + name) >= 0;
		game.enterWorld(name.c_str(), 8.0f, findPoint, &worlds[w].scene);
		gameWorld[w] = game.currentWorld();
		const GameStats& now = game.stats();
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
		line(format("life: the states call %zu engine functions, %zu of which the engine carries out; the others do nothing yet, by "
		            "the number of states that call them: ",
		            called.size(), called.size() - missing.size()) +
		     (list.empty() ? "none" : list) + (byUse.size() > 40 ? ", ..." : ""));
	} catch (const std::exception& ex) {
		line(std::string("life: the states could not be looked through: ") + ex.what());
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
			}
		}
		if (!there.empty())
			line(format("things to use: the worlds have %zu kinds; the scripts name %zu of them: ", there.size(), usedCount) +
			     (used.empty() ? "none" : used) + (unused.empty() ? "" : "; not named: " + unused));
	}
	// Fighting: the engine names those animations itself (fight_clips.h).
	std::set<std::string> rigNames = aniNames;
	size_t fightNames = 0;
	for (const std::string& name : fightClipNames(false)) fightNames += rigNames.insert(name).second;
	std::string rigNotes;
	std::vector<std::string> mobClipNotes;
	BakeOptions quiet = opt;
	quiet.quiet = true;
	ModelBakeShared shared;
	struct Rig {
		SrcModel model; // kept: bodies are checked against its skeleton
		u32 hash = 0;
	};
	std::map<std::string, Rig> rigs;
	std::map<std::string, u32> requested; // body or head as asked for -> model hash (0: cannot be had)
	std::map<std::string, u32> bySignature;
	std::map<std::string, u32> problems; // reason -> looks
	std::vector<LookEntry> entries;
	size_t bodies = 0, heads = 0, triangles = 0, textures = 0, clipCount = 0;
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
			textures += mr.textures;
			missingTextures.insert(mr.missingTextures.begin(), mr.missingTextures.end());
			out = hashName(("model/" + m.name).c_str());
			bySignature[sig] = out;
			return out;
		} catch (const std::exception& ex) {
			why = kind + " unusable: " + ex.what();
			return 0;
		}
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
		entries.push_back(LookEntry {key, rig.hash, body, head});
	}

	std::sort(entries.begin(), entries.end(), [](const LookEntry& a, const LookEntry& b) { return a.key < b.key; });
	std::vector<u8> blob(sizeof(LooksHeader) + entries.size() * sizeof(LookEntry));
	LooksHeader lh {LOOKS_MAGIC, LOOKS_VERSION, u32(entries.size()), 0};
	std::memcpy(blob.data(), &lh, sizeof(lh));
	if (!entries.empty()) std::memcpy(blob.data() + sizeof(lh), entries.data(), entries.size() * sizeof(LookEntry));
	pak.add("meta/looks", ASSET_META, blob);

	size_t rigCount = 0;
	for (const auto& r : rigs) rigCount += r.second.hash != 0;
	result.looksBaked = entries.size();
	result.npcModels = rigCount + bodies + heads;
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
		// Their models: one per visual, "model/item.<visual>".
		size_t baked = 0, itemTriangles = 0;
		std::map<std::string, std::string> missing;
		const u64 itemBytesBefore = pak.rawBytes(ASSET_MODEL);
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
				const ModelBakeResult mr = bakeModel(m, quiet, pak, &shared);
				itemTriangles += mr.triangles;
				missingTextures.insert(mr.missingTextures.begin(), mr.missingTextures.end());
				++baked;
			} catch (const std::exception& ex) {
				missing[visual] = ex.what();
			}
		}
		line(format("things lying about: %zu (%zu from the world files, %zu from the start-up scripts) of %zu kinds of look; "
		            "%zu models baked, %zu triangles, %.2f MB",
		            fromFiles + fromScripts, fromFiles, fromScripts, visuals.size(), baked, itemTriangles,
		            double(pak.rawBytes(ASSET_MODEL) - itemBytesBefore) / 1048576.0));
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
