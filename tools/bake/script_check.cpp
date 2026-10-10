#include "script_check.h"

#include "script.h"

#include <zenkit/DaedalusScript.hh>
#include <zenkit/DaedalusVm.hh>
#include <zenkit/Logger.hh>
#include <zenkit/Stream.hh>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>

#if defined(__unix__) || defined(__APPLE__)
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#define G3_CAN_FORK 1
#endif

namespace g3 {

namespace {

std::string number(double v) {
	char buf[48];
	std::snprintf(buf, sizeof(buf), "%.9g", v);
	return buf;
}

// ---- the two machines behind one face ----

struct Reference {
	typedef std::shared_ptr<zenkit::DaedalusInstance> Inst;
	zenkit::DaedalusVm& vm;
	std::map<const zenkit::DaedalusInstance*, u32> serials;
	std::vector<Inst> alive; // serial numbers stay unique
	bool objected = false;

	i32 popInt() { return vm.pop_int(); }
	float popFloat() { return vm.pop_float(); }
	std::string popString() { return vm.pop_string(); }
	Inst popInstance() { return vm.pop_instance(); }
	void pushInt(i32 v) { vm.push_int(v); }
	void pushFloat(float v) { vm.push_float(v); }
	void pushString(const std::string& v) { vm.push_string(v); }
	void pushInstance(const Inst& v) { vm.push_instance(v); }

	static bool null(const Inst& i) { return i == nullptr; }
	u32 symbolOf(const Inst& i) const { return i->symbol_index(); }
	u32 serialOf(const Inst& i) const {
		const auto it = serials.find(i.get());
		return it == serials.end() ? 0xFFFFFFFFu : it->second;
	}
	Inst create(u32 symbol) {
		zenkit::DaedalusSymbol* sym = vm.find_symbol_by_index(symbol);
		// As the engine does: the object exists, and its name means it, before its code runs.
		zenkit::DaedalusSymbol* cls = sym;
		while (cls && cls->type() != zenkit::DaedalusDataType::CLASS) cls = vm.find_symbol_by_index(cls->parent());
		if (!cls) return nullptr;
		Inst inst = std::make_shared<zenkit::DaedalusOpaqueInstance>(*cls, vm.find_class_members(*cls));
		serials[inst.get()] = u32(alive.size());
		alive.push_back(inst);
		vm.init_instance(std::static_pointer_cast<zenkit::DaedalusOpaqueInstance>(inst), sym);
		return inst;
	}
	Inst of(u32 symbol) {
		zenkit::DaedalusSymbol* sym = vm.find_symbol_by_index(symbol);
		return sym && sym->type() == zenkit::DaedalusDataType::INSTANCE ? sym->get_instance() : nullptr;
	}
	void set(u32 symbol, const Inst& inst) {
		zenkit::DaedalusSymbol* sym = vm.find_symbol_by_index(symbol);
		if (sym && sym->type() == zenkit::DaedalusDataType::INSTANCE) sym->set_instance(inst);
	}
	i32 getInt(u32 symbol, u32 index, const Inst& ctx) {
		return vm.find_symbol_by_index(symbol)->get_int(u16(index), ctx.get());
	}
	float getFloat(u32 symbol, u32 index, const Inst& ctx) {
		return vm.find_symbol_by_index(symbol)->get_float(u16(index), ctx.get());
	}
	std::string getString(u32 symbol, u32 index, const Inst& ctx) {
		return vm.find_symbol_by_index(symbol)->get_string(u16(index), ctx.get());
	}
	void call(u32 symbol) { vm.unsafe_call(vm.find_symbol_by_index(symbol)); }
};

struct Ours {
	typedef ScriptInstance* Inst;
	ScriptVm& vm;

	i32 popInt() { return vm.popInt(); }
	float popFloat() { return vm.popFloat(); }
	std::string popString() { return vm.popString(); }
	Inst popInstance() { return vm.popInstance(); }
	void pushInt(i32 v) { vm.pushInt(v); }
	void pushFloat(float v) { vm.pushFloat(v); }
	void pushString(const std::string& v) { vm.pushString(v.c_str()); }
	void pushInstance(Inst v) { vm.pushInstance(v); }

	static bool null(Inst i) { return i == nullptr; }
	u32 symbolOf(Inst i) const { return i->symbol; }
	u32 serialOf(Inst i) const { return i->serial; }
	Inst create(u32 symbol) { return vm.create(symbol); }
	Inst of(u32 symbol) { return vm.instanceOf(symbol); }
	void set(u32 symbol, Inst inst) { vm.setInstance(symbol, inst); }
	i32 getInt(u32 symbol, u32 index, Inst ctx) { return vm.getInt(symbol, index, ctx); }
	float getFloat(u32 symbol, u32 index, Inst ctx) { return vm.getFloat(symbol, index, ctx); }
	std::string getString(u32 symbol, u32 index, Inst ctx) { return vm.getString(symbol, index, ctx); }
	void call(u32 symbol) { vm.call(symbol); }
};

// ---- the stand-in engine ----
//
// Just enough of an engine that the scripts' start-up does what it does in
// the game: characters are built when the scripts ask for them and their
// daily routines are read. Everything else answers zero. What matters is
// that both machines get exactly the same engine.

template <class A> struct Engine {
	A& a;
	const SrcScript& s;
	std::vector<std::string> trace;
	u32 seed = 2463534242u;
	typename A::Inst hero {};
	int self = -1, other = -1, heroSym = -1, routine = -1;

	Engine(A& adapter, const SrcScript& script) : a(adapter), s(script) {
		self = s.find("SELF");
		other = s.find("OTHER");
		heroSym = s.find("HERO");
		routine = s.find("C_NPC.DAILY_ROUTINE");
	}

	bool isSymbol(i32 v, ScriptType type) const { return v >= 0 && size_t(v) < s.symbols.size() && s.symbols[v].type == type; }
	int classOf(int symbol) const {
		for (int hops = 0; symbol >= 0 && size_t(symbol) < s.symbols.size() && hops < 16; ++hops) {
			if (s.symbols[symbol].type == SCRIPT_CLASS) return symbol;
			symbol = s.symbols[symbol].parent;
		}
		return -1;
	}
	bool isDefinition(i32 v) const { return isSymbol(v, SCRIPT_INSTANCE) && s.symbols[v].isConst && classOf(v) >= 0; }
	bool isPlainFunction(i32 v) const {
		return isSymbol(v, SCRIPT_FUNC) && s.symbols[v].isConst && !s.symbols[v].external && !s.symbols[v].member &&
		       s.symbols[v].count == 0;
	}

	std::string describe(const typename A::Inst& i) {
		if (A::null(i)) return "null";
		const u32 sym = a.symbolOf(i);
		return (sym < s.symbols.size() ? s.symbols[sym].name : std::string("?")) + "#" + std::to_string(a.serialOf(i));
	}

	// Builds an object and puts everything it holds on record.
	typename A::Inst build(u32 symbol) {
		typename A::Inst inst = a.create(symbol);
		if (A::null(inst)) {
			trace.push_back("could not build " + s.symbols[symbol].name);
			return inst;
		}
		std::string rec = "built " + describe(inst) + ":";
		const int cls = classOf(int(symbol));
		for (size_t m = 0; m < s.symbols.size(); ++m) {
			const SrcSymbol& member = s.symbols[m];
			if (!member.member || member.parent != cls) continue;
			for (u32 k = 0; k < std::max<u32>(member.count, 1); ++k) {
				std::string v;
				if (member.type == SCRIPT_STRING) {
					v = a.getString(u32(m), k, inst);
					if (v.empty()) continue;
				} else if (member.type == SCRIPT_FLOAT) {
					const float f = a.getFloat(u32(m), k, inst);
					if (f == 0.0f) continue;
					v = number(f);
				} else if (member.type == SCRIPT_INT || member.type == SCRIPT_FUNC) {
					const i32 i = a.getInt(u32(m), k, inst);
					if (i == 0) continue;
					v = std::to_string(i);
				} else {
					continue;
				}
				rec += " " + member.name.substr(member.name.find('.') + 1) + (member.count > 1 ? "[" + std::to_string(k) + "]" : "") +
				       "=" + v;
			}
		}
		trace.push_back(rec);
		return inst;
	}

	// Runs a function that takes nothing; its result goes on record.
	void run(u32 symbol) {
		const SrcSymbol& f = s.symbols[symbol];
		a.call(symbol);
		if (!f.hasReturn) return;
		switch (f.returnType) {
		case SCRIPT_FLOAT: trace.push_back(f.name + " returned " + number(a.popFloat())); break;
		case SCRIPT_STRING: trace.push_back(f.name + " returned \"" + a.popString() + "\""); break;
		case SCRIPT_INSTANCE: trace.push_back(f.name + " returned " + describe(a.popInstance())); break;
		default: trace.push_back(f.name + " returned " + std::to_string(a.popInt())); break;
		}
	}

	void external(u32 symbol) {
		const SrcSymbol& f = s.symbols[symbol];
		struct Arg {
			i32 i = 0;
			float f = 0;
			std::string text;
			typename A::Inst inst {};
		};
		std::vector<Arg> args(f.count);
		std::vector<std::string> shown(f.count);
		for (u32 k = f.count; k-- > 0;) {
			switch (s.symbols[symbol + 1 + k].type) {
			case SCRIPT_FLOAT:
				args[k].f = a.popFloat();
				shown[k] = number(args[k].f);
				break;
			case SCRIPT_INT:
			case SCRIPT_FUNC:
				args[k].i = a.popInt();
				shown[k] = std::to_string(args[k].i);
				break;
			case SCRIPT_STRING:
				args[k].text = a.popString();
				shown[k] = "\"" + args[k].text + "\"";
				break;
			default:
				args[k].inst = a.popInstance();
				shown[k] = describe(args[k].inst);
				break;
			}
		}
		std::string rec = f.name + "(";
		for (u32 k = 0; k < f.count; ++k) rec += (k ? ", " : "") + shown[k];
		rec += ")";
		const size_t at = trace.size();
		trace.push_back(rec);

		i32 ri = 0;
		float rf = 0;
		std::string rs;
		typename A::Inst rinst {};
		const std::string& n = f.name;
		if (n == "WLD_INSERTNPC" && f.count == 2) {
			if (isDefinition(args[0].i)) {
				typename A::Inst npc = build(u32(args[0].i));
				const i32 fn = routine >= 0 && !A::null(npc) && classOf(args[0].i) == s.symbols[routine].parent
				                   ? a.getInt(u32(routine), 0, npc)
				                   : 0;
				if (isPlainFunction(fn) && self >= 0) {
					typename A::Inst before = a.of(u32(self));
					a.set(u32(self), npc);
					run(u32(fn));
					a.set(u32(self), before);
				}
			}
		} else if (n == "HLP_GETNPC" && f.count == 1) {
			if (isSymbol(args[0].i, SCRIPT_INSTANCE)) rinst = a.of(u32(args[0].i));
		} else if (n == "HLP_GETINSTANCEID" && f.count == 1) {
			ri = A::null(args[0].inst) ? -1 : i32(a.symbolOf(args[0].inst));
		} else if ((n == "HLP_ISVALIDNPC" || n == "HLP_ISVALIDITEM") && f.count == 1) {
			ri = !A::null(args[0].inst);
		} else if (n == "NPC_ISPLAYER" && f.count == 1) {
			ri = !A::null(args[0].inst) && args[0].inst == hero;
		} else if (n == "HLP_RANDOM" && f.count == 1) {
			seed ^= seed << 13;
			seed ^= seed >> 17;
			seed ^= seed << 5;
			ri = args[0].i > 0 ? i32(seed % u32(args[0].i)) : 0;
		} else if (n == "HLP_STRCMP" && f.count == 2) {
			std::string x = args[0].text, y = args[1].text;
			for (char& c : x) c = char(std::toupper(static_cast<unsigned char>(c)));
			for (char& c : y) c = char(std::toupper(static_cast<unsigned char>(c)));
			ri = x == y;
		} else if (n == "CONCATSTRINGS" && f.count == 2) {
			rs = args[0].text + args[1].text;
		} else if (n == "INTTOSTRING" && f.count == 1) {
			rs = std::to_string(args[0].i);
		} else if (n == "INTTOFLOAT" && f.count == 1) {
			rf = float(args[0].i);
		} else if (n == "FLOATTOINT" && f.count == 1) {
			rf = args[0].f;
			ri = rf > -2e9f && rf < 2e9f ? i32(rf) : 0;
		}
		if (!f.hasReturn) return;
		switch (f.returnType) {
		case SCRIPT_FLOAT:
			a.pushFloat(rf);
			trace[at] += " -> " + number(rf);
			break;
		case SCRIPT_STRING:
			a.pushString(rs);
			trace[at] += " -> \"" + rs + "\"";
			break;
		case SCRIPT_INSTANCE:
			a.pushInstance(rinst);
			trace[at] += " -> " + describe(rinst);
			break;
		case SCRIPT_INT:
		case SCRIPT_FUNC:
			a.pushInt(ri);
			trace[at] += " -> " + std::to_string(ri);
			break;
		default:
			break;
		}
	}
};

struct Unit {
	enum Kind { HERO, FUNCTION, INFO, ITEM } kind;
	u32 symbol;
};

template <class A> void runUnit(Engine<A>& e, const Unit& u) {
	const SrcScript& s = e.s;
	switch (u.kind) {
	case Unit::HERO:
		e.hero = e.build(u.symbol);
		if (e.heroSym >= 0) e.a.set(u32(e.heroSym), e.hero);
		break;
	case Unit::ITEM:
		e.build(u.symbol);
		break;
	case Unit::FUNCTION:
		e.run(u.symbol);
		break;
	case Unit::INFO: {
		typename A::Inst info = e.build(u.symbol);
		if (A::null(info)) break;
		const int npc = s.find("C_INFO.NPC"), cond = s.find("C_INFO.CONDITION"), talk = s.find("C_INFO.INFORMATION");
		typename A::Inst speaker {};
		if (npc >= 0) {
			const i32 who = e.a.getInt(u32(npc), 0, info);
			if (e.isSymbol(who, SCRIPT_INSTANCE)) speaker = e.a.of(u32(who));
		}
		if (e.self >= 0) e.a.set(u32(e.self), speaker);
		if (e.other >= 0) e.a.set(u32(e.other), e.hero);
		for (int member : {cond, talk}) {
			if (member < 0) continue;
			const i32 fn = e.a.getInt(u32(member), 0, info);
			if (e.isPlainFunction(fn)) e.run(u32(fn));
		}
		break;
	}
	}
}

std::string globalsOf(const SrcScript& s, const std::function<i32(u32, u32)>& get, u32& count) {
	// One line per variable that is not zero.
	std::string all;
	count = 0;
	for (size_t i = 0; i < s.symbols.size(); ++i) {
		const SrcSymbol& sym = s.symbols[i];
		// (Parameters of the engine's functions are in the table too, without
		// any room for a value: count 0.)
		if (sym.type != SCRIPT_INT || sym.member || sym.isConst || sym.count == 0) continue;
		for (u32 k = 0; k < sym.count; ++k) {
			i32 v = 0;
			try {
				v = get(u32(i), k);
			} catch (const std::exception&) {
				all += sym.name + "[" + std::to_string(k) + "] cannot be read\n";
				++count;
				continue;
			}
			++count;
			if (v) all += sym.name + "[" + std::to_string(k) + "]=" + std::to_string(v) + "\n";
		}
	}
	return all;
}

void append(std::string& to, const std::string& line, int& budget) {
	if (budget <= 0) return;
	--budget;
	to += line + "\n";
}

ScriptCheckResult compare(const std::vector<u8>& dat, const SrcScript& script, const std::vector<u8>& image,
                          const ScriptCheckOptions& opt) {
	ScriptCheckResult r;
	zenkit::Logger::set(zenkit::LogLevel::ERROR, [](zenkit::LogLevel, const char*, const char*) {});

	// The reference.
	std::unique_ptr<zenkit::DaedalusVm> refVm;
	try {
		zenkit::DaedalusScript loaded;
		auto rd = zenkit::Read::from(reinterpret_cast<const std::byte*>(dat.data()), dat.size());
		loaded.load(rd.get());
		if (loaded.symbols().size() != script.symbols.size()) throw std::runtime_error("it reads a different number of symbols");
		refVm = std::make_unique<zenkit::DaedalusVm>(std::move(loaded), zenkit::DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS);
		for (size_t i = 0; i < script.symbols.size(); ++i)
			if (script.symbols[i].type == SCRIPT_CLASS && !script.symbols[i].member)
				refVm->register_as_opaque(refVm->find_symbol_by_index(u32(i)));
	} catch (const std::exception& ex) {
		r.report = std::string("scripts: not checked against the reference interpreter: ") + ex.what() + "\n";
		return r;
	}
	Reference ref {*refVm, {}, {}, false};
	Engine<Reference> refEngine(ref, script);
	refVm->register_default_external_custom(
	    [&](zenkit::DaedalusVm&, zenkit::DaedalusSymbol& sym) { refEngine.external(sym.index()); });
	refVm->register_exception_handler(
	    [&](zenkit::DaedalusVm& v, const zenkit::DaedalusScriptError& err, const zenkit::DaedalusInstruction& in) {
		    ref.objected = true;
		    return zenkit::lenient_vm_exception_handler(v, err, in);
	    });

	// Ours.
	ScriptVm vm;
	{
		u8* blob = static_cast<u8*>(std::malloc(image.size()));
		if (!blob) throw std::runtime_error("out of memory");
		std::memcpy(blob, image.data(), image.size());
		if (!vm.adopt(blob, u32(image.size()))) {
			r.report = "scripts: this project's interpreter refuses the image baked from the file\n";
			r.ran = true;
			r.differ = 1;
			return r;
		}
	}
	Ours ours {vm};
	Engine<Ours> ourEngine(ours, script);
	// What our machine finds fault with, by kind and by the function it happens in.
	struct Faults {
		const SrcScript* script;
		std::map<std::string, u32> kinds;
	} faults {&script, {}};
	vm.onError(
	    [](const char* what, u32 function, u32, void* user) {
		    Faults& f = *static_cast<Faults*>(user);
		    const std::string where = function < f.script->symbols.size() ? f.script->symbols[function].name : std::string("?");
		    if (f.kinds.size() < 4096) ++f.kinds[std::string(what) + " in " + where];
	    },
	    &faults);
	for (size_t i = 0; i < script.symbols.size(); ++i)
		if (script.symbols[i].type == SCRIPT_FUNC && script.symbols[i].external)
			vm.bindSymbol(u32(i), [](ScriptVm& m, void* user) { static_cast<Engine<Ours>*>(user)->external(m.callee()); },
			              &ourEngine);

	// What to run.
	std::vector<Unit> units;
	const auto named = [&](const std::string& name, ScriptType type) {
		const int i = script.find(name);
		return i >= 0 && script.symbols[i].type == type ? i : -1;
	};
	if (const int hero = named(opt.hero, SCRIPT_INSTANCE); hero >= 0 && ourEngine.isDefinition(hero))
		units.push_back({Unit::HERO, u32(hero)});
	std::vector<std::string> functions = {"STARTUP_GLOBAL", "STARTUP_" + opt.world, "INIT_GLOBAL", "INIT_" + opt.world};
	functions.insert(functions.end(), opt.functions.begin(), opt.functions.end());
	for (const std::string& name : functions)
		if (const int f = named(name, SCRIPT_FUNC); f >= 0 && ourEngine.isPlainFunction(f)) units.push_back({Unit::FUNCTION, u32(f)});
	const int infoClass = named("C_INFO", SCRIPT_CLASS), itemClass = named("C_ITEM", SCRIPT_CLASS);
	for (size_t i = 0; i < script.symbols.size(); ++i) {
		if (!ourEngine.isDefinition(i32(i))) continue;
		const int cls = ourEngine.classOf(int(i));
		if (opt.items && cls == itemClass && itemClass >= 0) units.push_back({Unit::ITEM, u32(i)});
		if (opt.dialogues && cls == infoClass && infoClass >= 0) units.push_back({Unit::INFO, u32(i)});
	}

	int lines = 12;
	u32 ourErrorsBefore = 0;
	for (const Unit& u : units) {
		const std::string& name = script.symbols[u.symbol].name;
		ref.objected = false;
		refEngine.trace.clear();
		ourEngine.trace.clear();
		bool threw = false;
		std::string why;
		try {
			runUnit(refEngine, u);
		} catch (const std::exception& ex) {
			threw = true;
			why = ex.what();
		}
		runUnit(ourEngine, u);
		++r.runs;
		const std::vector<std::string>& x = refEngine.trace;
		const std::vector<std::string>& y = ourEngine.trace;
		if (threw || ref.objected) {
			++r.objected;
			if (r.objected <= 3) append(r.detail, "  " + name + ": the reference objected" + (why.empty() ? "" : " (" + why + ")"), lines);
		} else if (x == y) {
			++r.same;
			r.calls += x.size();
		} else {
			++r.differ;
			size_t k = 0;
			while (k < x.size() && k < y.size() && x[k] == y[k]) ++k;
			if (r.differ <= 3) {
				append(r.detail, "  " + name + ": step " + std::to_string(k + 1) + " of " + std::to_string(x.size()) + "/" +
				                     std::to_string(y.size()),
				       lines);
				append(r.detail, "    reference: " + (k < x.size() ? x[k].substr(0, 300) : std::string("(nothing more)")), lines);
				append(r.detail, "    ours:      " + (k < y.size() ? y[k].substr(0, 300) : std::string("(nothing more)")), lines);
				if (vm.stats().errors != ourErrorsBefore)
					append(r.detail, std::string("    our last complaint: ") + vm.stats().lastError, lines);
			}
		}
		ourErrorsBefore = vm.stats().errors;
	}

	u32 count = 0;
	std::string theirs, mine;
	try {
		theirs = globalsOf(script, [&](u32 sym, u32 k) { return refVm->find_symbol_by_index(sym)->get_int(u16(k)); }, count);
	} catch (const std::exception& ex) {
		theirs = std::string("unreadable: ") + ex.what();
	}
	mine = globalsOf(script, [&](u32 sym, u32 k) { return vm.getInt(sym, k); }, count);
	r.stateSame = theirs == mine;
	if (!r.stateSame) {
		size_t a = 0;
		while (a < theirs.size() && a < mine.size() && theirs[a] == mine[a]) ++a;
		const auto lineAt = [](const std::string& t, size_t at) {
			const size_t b = t.rfind('\n', at ? at - 1 : 0);
			const size_t from = b == std::string::npos ? 0 : b + 1;
			return t.substr(from, t.find('\n', from) - from);
		};
		append(r.detail, "  variables: reference " + lineAt(theirs, a) + ", ours " + lineAt(mine, a), lines);
	}

	r.ran = true;
	char buf[400];
	std::snprintf(buf, sizeof(buf),
	              "scripts: checked against the reference interpreter: %u runs (start-up, dialogues, items), %u the same "
	              "with %llu engine calls and objects, %u different, %u the reference objected to; %u variables at the "
	              "end %s; ours: %llu instructions, %u complaints, %u calls given up, %u uses of members of no object\n",
	              r.runs, r.same, static_cast<unsigned long long>(r.calls), r.differ, r.objected, count,
	              r.stateSame ? "the same" : "DIFFERENT", static_cast<unsigned long long>(vm.stats().instructions),
	              vm.stats().errors, vm.stats().aborted, vm.stats().nullAccess);
	r.report = buf;
	if (!faults.kinds.empty()) {
		std::vector<std::pair<u32, std::string>> byCount;
		for (const auto& [what, n] : faults.kinds) byCount.push_back({n, what});
		std::sort(byCount.rbegin(), byCount.rend());
		std::string list;
		for (size_t i = 0; i < byCount.size() && i < 6; ++i) list += (i ? "; " : "") + byCount[i].second + " x" + std::to_string(byCount[i].first);
		r.report += "scripts: where our machine found fault during the check (" + std::to_string(byCount.size()) + " places): " + list +
		            (byCount.size() > 6 ? "; ..." : "") + "\n";
	}
	if (!r.detail.empty()) r.report += r.detail;
	return r;
}

} // namespace

ScriptCheckResult checkScript(const std::vector<u8>& dat, const SrcScript& script, const std::vector<u8>& image,
                              const ScriptCheckOptions& opt) {
#ifdef G3_CAN_FORK
	if (opt.isolate) {
		// The reference interpreter recurses on the machine stack and has no
		// limits of its own: run it where a crash or an endless loop is only
		// a line in the report.
		ScriptCheckResult r;
		int fd[2];
		if (pipe(fd) != 0) return compare(dat, script, image, opt);
		std::fflush(nullptr);
		const pid_t child = fork();
		if (child < 0) {
			close(fd[0]);
			close(fd[1]);
			return compare(dat, script, image, opt);
		}
		if (child == 0) {
			close(fd[0]);
			ScriptCheckResult c;
			try {
				c = compare(dat, script, image, opt);
			} catch (const std::exception& ex) {
				c.report = std::string("scripts: the check against the reference interpreter failed: ") + ex.what() + "\n";
			}
			u32 head[8] = {c.ran, c.runs, c.same, c.differ, c.objected, c.stateSame, u32(c.calls), u32(c.calls >> 32)};
			const std::string body = c.report + '\0' + c.detail;
			const u32 size = u32(body.size());
			bool ok = write(fd[1], head, sizeof(head)) == ssize_t(sizeof(head)) && write(fd[1], &size, 4) == 4;
			size_t sent = 0;
			while (ok && sent < body.size()) {
				const ssize_t n = write(fd[1], body.data() + sent, body.size() - sent);
				if (n <= 0) break;
				sent += size_t(n);
			}
			_exit(0);
		}
		close(fd[1]);
		std::string got;
		bool timedOut = false;
		for (;;) {
			pollfd p {fd[0], POLLIN, 0};
			const int ready = poll(&p, 1, opt.timeoutSeconds * 1000);
			if (ready <= 0) {
				timedOut = ready == 0;
				break;
			}
			char buf[4096];
			const ssize_t n = read(fd[0], buf, sizeof(buf));
			if (n <= 0) break;
			got.append(buf, size_t(n));
		}
		close(fd[0]);
		if (timedOut) kill(child, SIGKILL);
		int status = 0;
		waitpid(child, &status, 0);
		u32 head[8], size = 0;
		if (got.size() >= sizeof(head) + 4) {
			std::memcpy(head, got.data(), sizeof(head));
			std::memcpy(&size, got.data() + sizeof(head), 4);
		}
		if (got.size() < sizeof(head) + 4 || got.size() != sizeof(head) + 4 + size) {
			char buf[200];
			if (timedOut)
				std::snprintf(buf, sizeof(buf), "it did not finish in %d s", opt.timeoutSeconds);
			else if (WIFSIGNALED(status))
				std::snprintf(buf, sizeof(buf), "the reference interpreter crashed (signal %d)", WTERMSIG(status));
			else
				std::snprintf(buf, sizeof(buf), "it ended without a result");
			r.report = std::string("scripts: not checked against the reference interpreter: ") + buf + "\n";
			return r;
		}
		r.ran = head[0];
		r.runs = head[1];
		r.same = head[2];
		r.differ = head[3];
		r.objected = head[4];
		r.stateSame = head[5];
		r.calls = u64(head[6]) | (u64(head[7]) << 32);
		const std::string body = got.substr(sizeof(head) + 4);
		const size_t cut = body.find('\0');
		r.report = body.substr(0, cut);
		if (cut != std::string::npos) r.detail = body.substr(cut + 1);
		return r;
	}
#endif
	return compare(dat, script, image, opt);
}

} // namespace g3
