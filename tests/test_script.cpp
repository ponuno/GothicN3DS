// The script machine: this project's Daedalus compiler, the image baker and
// the interpreter. Usage: test_script <scratch dir>
#include "bytes.h"
#include "daedalus.h"
#include "fight.h"
#include "fight_ai.h"
#include "game.h"
#include "importers.h"
#include "script_check.h"
#include "script.h"
#include "script_src.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace g3;

static int gFailures = 0, gChecks = 0;

#define CHECK(cond)                                                              \
	do {                                                                         \
		++gChecks;                                                               \
		if (!(cond)) {                                                           \
			++gFailures;                                                         \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
		}                                                                        \
	} while (0)

namespace {

// Source -> image -> a running machine.
struct Machine {
	SrcScript src;
	ScriptVm vm;
	ScriptBakeResult baked;

	explicit Machine(const std::string& source) {
		compileDaedalus(source, src);
		const std::vector<u8> image = bakeScript(src, baked);
		u8* blob = static_cast<u8*>(std::malloc(image.size()));
		std::memcpy(blob, image.data(), image.size());
		if (!vm.adopt(blob, u32(image.size()))) throw std::runtime_error("the machine refused its image");
	}
	i32 callInt(const char* name) {
		const u32 f = vm.find(name);
		if (f == SCRIPT_NONE) throw std::runtime_error(std::string("no function ") + name);
		vm.call(f);
		return vm.popInt();
	}
	i32 global(const char* name, u32 index = 0) { return vm.getInt(vm.find(name), index); }
};

bool compiles(const std::string& source, std::string* why = nullptr) {
	try {
		SrcScript s;
		compileDaedalus(source, s);
		return true;
	} catch (const std::exception& ex) {
		if (why) *why = ex.what();
		return false;
	}
}

const char* const kBasics = R"(
const int FIVE = 5;
const int MASK = 1 << 4 | 3;
const int LIST[3] = { 10, 20, FIVE * 6 };
const float HALF = 0.5;
const string HELLO = "hello";
var int counter;
var int table[4];
var string words[2];
var float ratio;

func int add(var int a, var int b) { return a + b; };
func int sub(var int a, var int b) { return a - b; };
func int precedence() { return 2 + 3 * 4 - 10 / 2 % 3; };       // 2 + 12 - (5 % 3) = 12
func int logic() { return (1 < 2) && (2 <= 2) && !(3 > 4) && (5 >= 5) && (1 != 2) && (7 == 7) || 0; };
func int bits() { return (MASK & 0) | (6 & 3) | (1 << 5) | (256 >> 4) | (~0 & 8); };
func int nested() { return add(sub(10, 4), add(LIST[1], LIST[2])); };  // 6 + 50
func int negative() { return -7 + -(-3) + (0 - 2) * -1; };          // -7 + 3 + 2
func int division() { return 7 / 2 + -7 / 2 + 7 % 3 + -7 % 3; };    // 3 - 3 + 1 - 1
func void bump(var int by) { counter += by; counter *= 2; counter -= 1; counter /= 3; };
func int branches(var int v) {
	if (v < 0) { return -1; }
	else if (v == 0) { return 0; }
	else if (v < 10) { if (v % 2) { return 1; }; return 2; }
	else { return 3; };
};
func int arrays() {
	table[0] = 4; table[1] = table[0] * 2; table[3] = table[1] + LIST[0];
	table[2] += 5;
	return table[0] + table[1] + table[2] + table[3];
};
func string greet() { return HELLO; };
func void strings() { words[0] = "one"; words[1] = greet(); words[0] = words[1]; };
func void floats() { ratio = HALF; ratio = 2; ratio = -1.25; };
func int forgets() { counter = 77; };                 // no return statement
func int twice() { return add(1, 2) + add(3, 4); };
func void discards() { add(1, 2); add(3, 4); };     // results stay on the stack
)";

} // namespace

static void testBasics() {
	Machine m(kBasics);
	ScriptVm& vm = m.vm;
	CHECK(m.global("FIVE") == 5 && m.global("MASK") == 19 && m.global("LIST", 2) == 30);
	CHECK(vm.getFloat(vm.find("half")) == 0.5f);
	CHECK(std::strcmp(vm.getString(vm.find("HELLO")), "hello") == 0);
	CHECK(m.callInt("precedence") == 12);
	CHECK(m.callInt("logic") == 1);
	CHECK(m.callInt("bits") == ((6 & 3) | 32 | 16 | 8));
	CHECK(m.callInt("nested") == 56);
	CHECK(m.callInt("negative") == -2);
	CHECK(m.callInt("division") == 0);
	CHECK(m.callInt("twice") == 10);

	const u32 add = vm.find("ADD"), sub = vm.find("SUB");
	vm.pushInt(40);
	vm.pushInt(2);
	CHECK(vm.call(add) && vm.popInt() == 42);
	vm.pushInt(40);
	vm.pushInt(2);
	CHECK(vm.call(sub) && vm.popInt() == 38);
	// Parameters are the symbols after their function.
	CHECK(vm.symbol(add).count == 2 && vm.parameter(add, 1) == vm.find("ADD.B"));
	CHECK(vm.getInt(vm.find("ADD.A")) == 40);

	vm.pushInt(7);
	CHECK(vm.call(vm.find("BUMP")));
	CHECK(m.global("COUNTER") == 4); // ((0 + 7) * 2 - 1) / 3
	const struct {
		i32 in, out;
	} cases[] = {{-5, -1}, {0, 0}, {3, 1}, {4, 2}, {12, 3}};
	for (const auto& c : cases) {
		vm.pushInt(c.in);
		CHECK(vm.call(vm.find("branches")) && vm.popInt() == c.out);
	}
	CHECK(m.callInt("arrays") == 4 + 8 + 5 + 18);
	CHECK(m.global("TABLE", 3) == 18);

	CHECK(vm.call(vm.find("STRINGS")));
	CHECK(std::strcmp(vm.getString(vm.find("WORDS"), 0), "hello") == 0);
	CHECK(std::strcmp(vm.getString(vm.find("WORDS"), 1), "hello") == 0);
	CHECK(vm.call(vm.find("GREET")) && std::strcmp(vm.popString(), "hello") == 0);
	CHECK(vm.call(vm.find("FLOATS")) && vm.getFloat(vm.find("RATIO")) == -1.25f);

	// A function that forgets its result returns a zero; results nobody takes
	// do not pile up.
	CHECK(m.callInt("forgets") == 0 && m.global("COUNTER") == 77);
	for (int i = 0; i < 5000; ++i) CHECK(vm.call(vm.find("DISCARDS")));
	CHECK(vm.popInt() == 0); // the stack is empty
	CHECK(vm.stats().errors == 0 && vm.stats().aborted == 0);

	// What the scripts cannot do is refused, not done.
	vm.setInt(vm.find("FIVE"), 0, nullptr, 6);
	CHECK(m.global("FIVE") == 5 && vm.stats().errors == 1);
	CHECK(vm.getInt(vm.find("LIST"), 3) == 0 && vm.stats().errors == 2);
	CHECK(vm.find("nothing") == SCRIPT_NONE && !vm.call(SCRIPT_NONE) && !vm.call(vm.find("COUNTER")));
}

static void testCompilerErrors() {
	std::string why;
	CHECK(!compiles("func void f() { x = 1; };", &why) && why.find("line 1") != std::string::npos);
	CHECK(!compiles("var int a;\nvar int a;", &why) && why.find("line 2") != std::string::npos);
	CHECK(!compiles("func int f(var int a) { return a; };\nfunc void g() { f(1, 2); };", &why) && why.find("takes 1") != std::string::npos);
	CHECK(!compiles("func void f() { return 1; };"));
	CHECK(!compiles("const int A = B;"));
	CHECK(!compiles("var string s; func void f() { s = 1; };"));
	CHECK(!compiles("var int i; func void f() { i = \"text\"; };"));
	CHECK(!compiles("func void f() {"));
	CHECK(!compiles("instance x(nothing) {};"));
	CHECK(!compiles("var int a[0];"));
	CHECK(!compiles("func void f() { var int t[2]; t[2] = 1; };"));
	CHECK(compiles("func void f() { var int t[2]; t[1] = 1; }; /* comment\n */ // more"));
}

// Objects, the engine's side of calls, and scripts that misbehave.
static void testObjects() {
	Machine m(R"(
class C_NPC { var int id; var string name[2]; var int stats[3]; var float size; var func routine; };
instance self, other (C_NPC);
extern func int Twice(var int v);
extern func void Trace(var C_NPC who, var string what, var float f, var int i);
extern func C_NPC Find(var int which);
extern func void CallBack(var func fn);
extern func string Nobody(var C_NPC who, var string s, var float f, var int i);
extern func int Sloppy(var int a, var int b);
var int total;
var int depth;
prototype Base (C_NPC) { stats[0] = 1; name = "base"; size = 1.5; };
instance A (Base) { id = 7; stats[1] = stats[0] + 4; name[1] = "second"; routine = Daily; };
instance B (C_NPC) { id = Twice(21); name = "b"; other = Find(A); stats[2] = other.id; };
func void Daily() { total += self.id; };
func int Sum(var C_NPC who) { return who.id + who.stats[0] + who.stats[1] + who.stats[2]; };
func void Say(var C_NPC who) { Trace(who, who.name, who.size, Sum(who)); };
func void Rename(var C_NPC who, var string to) { who.name = to; who.name[1] = who.name; };
func void Nested() { CallBack(Inner); total += 1000; };
func void Inner() { total += Twice(50); };
func int Lost() { return self.id; };
func string Quiet() { return Nobody(self, "x", 1.0, 2); };
func void Endless() { depth += 1; Endless(); };
func void Echo() { CallBack(Echo); };
func int UsesSloppy() { return Sloppy(1, 2) + 5; };
)");
	ScriptVm& vm = m.vm;
	struct Host {
		std::string trace;
		int callbacks = 0;
	} host;
	CHECK(vm.bind("TWICE", [](ScriptVm& v, void*) { v.pushInt(v.popInt() * 2); }, nullptr));
	CHECK(vm.bind("trace", [](ScriptVm& v, void* user) {
		      const i32 i = v.popInt();
		      const float f = v.popFloat();
		      const std::string what = v.popString();
		      const ScriptInstance* who = v.popInstance();
		      char buf[160];
		      std::snprintf(buf, sizeof(buf), "%u:%s:%.2f:%d;", who ? unsigned(who->symbol) : 0u, what.c_str(), double(f), int(i));
		      static_cast<Host*>(user)->trace += buf;
	      }, &host));
	CHECK(vm.bind("FIND", [](ScriptVm& v, void*) { v.pushInstance(v.instanceOf(u32(v.popInt()))); }, nullptr));
	CHECK(vm.bind("CALLBACK", [](ScriptVm& v, void* user) {
		      ++static_cast<Host*>(user)->callbacks;
		      v.call(u32(v.popInt()));
	      }, &host));
	// Takes one of its two arguments and returns two values: the machine evens it out.
	CHECK(vm.bind("SLOPPY", [](ScriptVm& v, void*) {
		      (void)v.popInt();
		      v.pushInt(1);
		      v.pushInt(37);
	      }, nullptr));
	CHECK(!vm.bind("NO_SUCH", nullptr, nullptr) && !vm.bind("SUM", nullptr, nullptr));

	const u32 symA = vm.find("A"), symB = vm.find("B"), cls = vm.find("C_NPC");
	const u32 mId = vm.findMember("C_NPC", "ID"), mName = vm.findMember("c_npc", "name"), mStats = vm.findMember("C_NPC", "STATS");
	const u32 mSize = vm.findMember("C_NPC", "SIZE"), mRoutine = vm.findMember("C_NPC", "ROUTINE");
	CHECK(mId != SCRIPT_NONE && mName != SCRIPT_NONE && mStats != SCRIPT_NONE && vm.findMember("C_NPC", "NOPE") == SCRIPT_NONE);
	CHECK(vm.classOf(symA) == cls && vm.classOf(mId) == cls && vm.classOf(vm.find("TOTAL")) == SCRIPT_NONE);
	CHECK(vm.symbol(cls).value == 4 + 2 * SCRIPT_STRING_SIZE + 12 + 4 + 4);

	ScriptInstance* a = vm.create(symA);
	CHECK(a && a->symbol == symA && a->classSymbol == cls && vm.instanceOf(symA) == a);
	CHECK(vm.getInt(mId, 0, a) == 7 && vm.getInt(mStats, 0, a) == 1 && vm.getInt(mStats, 1, a) == 5);
	CHECK(std::string(vm.getString(mName, 0, a)) == "base" && std::string(vm.getString(mName, 1, a)) == "second");
	CHECK(vm.getFloat(mSize, 0, a) == 1.5f && u32(vm.getInt(mRoutine, 0, a)) == vm.find("DAILY"));
	// SELF meant the object while its code ran, and is restored afterwards.
	CHECK(vm.instanceOf(vm.selfSymbol()) == nullptr && vm.current() == nullptr);

	ScriptInstance* b = vm.create(symB);
	CHECK(b && vm.getInt(mId, 0, b) == 42 && vm.instanceOf(vm.otherSymbol()) == a);
	// As in the original: once "other.id" has been read, the members that
	// follow in the same body are other's too.
	CHECK(vm.getInt(mStats, 2, b) == 0 && vm.getInt(mStats, 2, a) == 7);
	CHECK(vm.stats().instances == 2 && vm.stats().errors == 0);

	// A second object of the same definition: the name now means the new one.
	ScriptInstance* a2 = vm.create(symA);
	CHECK(a2 && a2 != a && a2->serial == 2 && vm.instanceOf(symA) == a2);

	vm.pushInstance(a);
	CHECK(vm.call(vm.find("SUM")) && vm.popInt() == 7 + 1 + 5 + 7);
	vm.pushInstance(b);
	CHECK(vm.call(vm.find("SAY")));
	char expected[64];
	std::snprintf(expected, sizeof(expected), "%u:b:0.00:42;", unsigned(symB));
	CHECK(host.trace == expected);

	// Strings of an object: assigned from itself, and gone with it.
	vm.pushInstance(a);
	vm.pushString("renamed");
	CHECK(vm.call(vm.find("RENAME")));
	CHECK(std::string(vm.getString(mName, 0, a)) == "renamed" && std::string(vm.getString(mName, 1, a)) == "renamed");
	const u32 bytesBefore = vm.stats().stringBytes;
	CHECK(bytesBefore > 0);
	vm.setString(mName, 0, a, vm.getString(mName, 0, a));
	CHECK(std::string(vm.getString(mName, 0, a)) == "renamed" && vm.stats().stringBytes == bytesBefore);

	// The daily routine, the way the game calls it.
	vm.setInstance(vm.selfSymbol(), a);
	CHECK(vm.call(u32(vm.getInt(mRoutine, 0, a))) && m.global("TOTAL") == 7);
	CHECK(m.callInt("LOST") == 7);

	// Engine function -> script -> engine function.
	CHECK(vm.call(vm.find("NESTED")) && m.global("TOTAL") == 7 + 100 + 1000 && host.callbacks == 1);
	// A host function that takes too little and returns too much.
	CHECK(m.callInt("USESSLOPPY") == 42);
	CHECK(vm.popInt() == 0 && vm.stats().errors == 0);

	// An external nobody implements: its arguments go, a default comes back.
	static u32 unbound;
	unbound = 0;
	vm.onUnbound([](u32 symbol, void*) { unbound = symbol; }, nullptr);
	CHECK(vm.call(vm.find("QUIET")) && std::string(vm.popString()).empty());
	CHECK(unbound == vm.find("NOBODY") && vm.stats().stubCalls == 1 && vm.stats().errors == 0);

	// Members without an object: zero, a complaint, no crash.
	vm.setInstance(vm.selfSymbol(), nullptr);
	// (Counted apart from complaints: the game's own scripts do it.)
	static std::string fault;
	fault.clear();
	vm.onError([](const char* what, u32 function, u32, void* user) {
		fault = std::string(what) + "@" + std::to_string(function == static_cast<ScriptVm*>(user)->find("LOST"));
	}, &vm);
	CHECK(m.callInt("LOST") == 0 && vm.stats().errors == 0 && vm.stats().nullAccess == 1);
	// (A returned variable is read by whoever takes the result: here the host.)
	CHECK(fault == "a member was used without an object@0");
	vm.onError(nullptr, nullptr);
	CHECK(vm.getInt(mId, 0, nullptr) == 0);
	vm.setInt(mStats, 3, a, 1); // past the array
	vm.setString(mId, 0, a, "no");
	CHECK(vm.getInt(mId, 0, a) == 7);

	// Runaway scripts are stopped, and the machine works afterwards.
	const u32 aborted = vm.stats().aborted;
	CHECK(!vm.call(vm.find("ENDLESS")) && vm.stats().aborted == aborted + 1);
	CHECK(m.global("DEPTH") > 50 && m.global("DEPTH") < 200);
	CHECK(!vm.call(vm.find("ECHO")) && vm.stats().aborted == aborted + 2);
	vm.setBudget(5);
	CHECK(!vm.call(vm.find("NESTED")) && vm.stats().aborted == aborted + 3);
	vm.setBudget(64000000);
	vm.pushInstance(a);
	CHECK(vm.call(vm.find("SUM")) && vm.popInt() == 20);
	CHECK(vm.popInt() == 0);

	// Destroying an object clears every name that meant it.
	vm.setInstance(vm.selfSymbol(), a2);
	vm.destroy(a2);
	CHECK(vm.instanceOf(symA) == nullptr && vm.instanceOf(vm.selfSymbol()) == nullptr && vm.stats().instances == 2);
	vm.destroy(a);
	vm.destroy(b);
	CHECK(vm.stats().instances == 0 && vm.stats().instanceBytes == 0 && vm.stats().stringBytes == 0);
	vm.destroy(nullptr);
}

// Whatever is in the image, the machine must neither crash nor hang.
static void testBrokenImages() {
	SrcScript src;
	buildSyntheticScript(src);
	ScriptBakeResult baked;
	const std::vector<u8> image = bakeScript(src, baked);
	ScriptHeader h;
	std::memcpy(&h, image.data(), sizeof(h));
	const auto adopt = [](ScriptVm& vm, const std::vector<u8>& bytes) {
		u8* blob = static_cast<u8*>(std::malloc(bytes.size() ? bytes.size() : 1));
		if (!bytes.empty()) std::memcpy(blob, bytes.data(), bytes.size());
		return vm.adopt(blob, u32(bytes.size()));
	};
	{
		ScriptVm vm;
		CHECK(adopt(vm, image) && vm.loaded() && vm.imageBytes() == image.size());
		CHECK(!adopt(vm, std::vector<u8>()) && !vm.loaded());
		CHECK(!adopt(vm, std::vector<u8>(image.begin(), image.end() - 4)));
		std::vector<u8> bad = image;
		bad[0] ^= 1;
		CHECK(!adopt(vm, bad));
		bad = image;
		bad[4] = 99; // version
		CHECK(!adopt(vm, bad));
		CHECK(!vm.adopt(nullptr, 100));
		CHECK(vm.find("SELF") == SCRIPT_NONE && !vm.call(0) && vm.create(0) == nullptr);
	}
	const u32 atSymbols = sizeof(ScriptHeader);
	const u32 atFunctions = atSymbols + h.symbolCount * u32(sizeof(ScriptSymbol));
	const u32 atCode = u32(image.size()) - ((h.codeSize + 3) & ~3u);
	std::mt19937 rng(7);
	int adopted = 0, refused = 0;
	u64 instructions = 0;
	for (int round = 0; round < 1500; ++round) {
		std::vector<u8> bad = image;
		const int hits = 1 + int(rng() % 6);
		for (int k = 0; k < hits; ++k) {
			// Mostly the tables the machine trusts and the code.
			u32 at;
			switch (rng() % 4) {
			case 0: at = atSymbols + rng() % (atFunctions - atSymbols); break;
			case 1: at = atCode + rng() % h.codeSize; break;
			case 2: at = atFunctions + rng() % (atCode - atFunctions); break;
			default: at = 8 + rng() % u32(image.size() - 8); break;
			}
			bad[at] = rng() % 3 ? u8(rng()) : u8(bad[at] ^ (1u << (rng() % 8)));
		}
		ScriptVm vm;
		if (!adopt(vm, bad)) {
			++refused;
			continue;
		}
		++adopted;
		vm.setBudget(20000);
		static std::mt19937 host;
		host.seed(u32(round));
		const auto external = [](ScriptVm& v, void*) {
			// An engine that takes and gives at random.
			for (u32 n = host() % 4; n > 0; --n) {
				switch (host() % 4) {
				case 0: (void)v.popInt(); break;
				case 1: (void)v.popFloat(); break;
				case 2: (void)v.popString(); break;
				default: (void)v.popInstance(); break;
				}
			}
			switch (host() % 5) {
			case 0: v.pushInt(i32(host())); break;
			case 1: v.pushString("text"); break;
			case 2: v.pushInstance(v.current()); break;
			case 3: v.pushFloat(1.0f); break;
			default: break;
			}
			if (host() % 16 == 0) v.call(host() % v.symbolCount());
		};
		for (u32 i = 0; i < vm.symbolCount(); ++i) {
			const ScriptSymbol& s = vm.symbol(i);
			if (s.type == SCRIPT_FUNC && (s.flags & SCRIPTF_EXTERNAL) && rng() % 2) vm.bindSymbol(i, external, nullptr);
		}
		std::vector<ScriptInstance*> made;
		for (u32 i = 0; i < vm.symbolCount(); ++i) {
			const ScriptSymbol& s = vm.symbol(i);
			if (s.type == SCRIPT_INSTANCE) {
				if (ScriptInstance* inst = vm.create(i)) made.push_back(inst);
				if (!made.empty() && vm.selfSymbol() != SCRIPT_NONE) vm.setInstance(vm.selfSymbol(), made[rng() % made.size()]);
			} else if (s.type == SCRIPT_FUNC) {
				vm.pushInt(3);
				vm.pushString("CAMP_SQUARE");
				vm.call(i);
				(void)vm.popInt();
			}
			(void)vm.getInt(i, rng() % 4, made.empty() ? nullptr : made.back());
			(void)vm.getFloat(i, 0, nullptr);
			(void)vm.getString(i, rng() % 4, made.empty() ? nullptr : made.back());
			vm.setInt(i, rng() % 4, made.empty() ? nullptr : made.front(), 5);
			vm.setString(i, rng() % 4, made.empty() ? nullptr : made.front(), "set");
		}
		instructions += vm.stats().instructions;
		for (size_t k = 0; k < made.size(); k += 2) vm.destroy(made[k]);
	}
	std::printf("       %d damaged images run (%llu instructions), %d refused at load\n", adopted,
	            static_cast<unsigned long long>(instructions), refused);
	CHECK(adopted > 300 && refused > 100);
}

// The script of the built-in scene, and the same through the game's own file
// format and the reference interpreter.
static void testSyntheticScript() {
	SrcScript src;
	buildSyntheticScript(src);
	ScriptBakeResult baked;
	const std::vector<u8> image = bakeScript(src, baked);
	std::printf("       %s", baked.report.c_str());
	// (Eight classes: the spells' C_Spell, their visual effects' CFx_Base and C_ParticleFXEmitKey, and the five: the particle effects' C_ParticleFX, the fight AI's C_FightAI, the sounds' C_SFX and the music's C_MUSICTHEME are in scripts of their own, as in the game.)
	CHECK(baked.hashClashes == 0 && baked.classes == 9 && baked.instances >= 12 && baked.externals >= 25);
	// The fight AI tables, that script of their own, bake into "script/fight".
	{
		SrcScript fight;
		buildSyntheticFightScript(fight);
		std::string report;
		const std::vector<u8> table = bakeFightAi(fight, report);
		std::printf("       %s\n", report.c_str());
		CHECK(table.size() > sizeof(FightAiHeader));
		FightAiHeader h {};
		std::memcpy(&h, table.data(), sizeof(h));
		CHECK(h.magic == FIGHT_AI_MAGIC && h.situations == FS_COUNT && h.tactics >= 18 &&
		      table.size() == sizeof(h) + size_t(h.tactics) * FS_COUNT * FIGHT_AI_MOVES);
		// Tactic 15 (the scavenger): at a blow's reach and in focus, the first move is an attack.
		CHECK(table[sizeof(h) + (size_t(15) * FS_COUNT + FS_MY_W_FOCUS) * FIGHT_AI_MOVES] == FM_ATTACK);
	}

	// The file format: written and read back, it is the same script.
	const std::vector<u8> dat = writeScriptDat(src);
	SrcScript back;
	readScriptDat(dat, back);
	CHECK(back.symbols.size() == src.symbols.size() && back.code == src.code);
	bool same = back.symbols.size() == src.symbols.size();
	for (size_t i = 0; same && i < src.symbols.size(); ++i) {
		const SrcSymbol& a = src.symbols[i];
		const SrcSymbol& b = back.symbols[i];
		same = a.name == b.name && a.type == b.type && a.count == b.count && a.isConst == b.isConst &&
		       a.hasReturn == b.hasReturn && a.member == b.member && a.external == b.external && a.parent == b.parent &&
		       a.address == b.address && a.ints == b.ints && a.floats == b.floats && a.strings == b.strings &&
		       a.memberOffset == b.memberOffset && a.classSize == b.classSize && a.returnType == b.returnType;
		if (!same) std::printf("       symbol %zu (%s) differs\n", i, a.name.c_str());
	}
	CHECK(same);
	ScriptBakeResult bakedBack;
	CHECK(bakeScript(back, bakedBack) == image);
	std::vector<u8> cut(dat.begin(), dat.begin() + long(dat.size() / 2));
	bool threw = false;
	try {
		readScriptDat(cut, back);
	} catch (const std::exception&) {
		threw = true;
	}
	CHECK(threw);

#ifdef G3DS_WITH_ZENKIT
	for (const bool isolate : {false, true}) {
		ScriptCheckOptions opt;
		opt.isolate = isolate;
		opt.functions = {"STARTUP_CAVE", "INIT_CAVE"};
		const ScriptCheckResult r = checkScript(dat, src, image, opt);
		std::printf("       %s", r.report.c_str());
		CHECK(r.ran && r.differ == 0 && r.objected == 0 && r.stateSame);
		// (The 18 things of the inventory's test items - and the chests' key and pick - are made once each too; the 8 are the
		// dialogues' conditions and the functions they run that the reference interpreter checks as well; the last 2 the
		// conditions of the hero's own infos - a bed's sleep.)
		CHECK(r.runs == 1 + 4 + 4 + 2 + 18 + 8 + 4 + 2 && r.same == r.runs && r.calls > 60);
	}
#endif
}

// Books and letters (Doc_*), the clock and the day, the fall, the small questions: the engine side of the scripts.
static void testDocsAndClock() {
	SrcScript src;
	compileDaedalus(std::string(syntheticScriptSource()) + R"(
extern func int  Doc_Create();
extern func int  Doc_CreateMap();
extern func void Doc_SetPages(var int handle, var int count);
extern func void Doc_PrintLine(var int handle, var int page, var string text);
extern func void Doc_Show(var int handle);
extern func int  Wld_GetDay();
extern func int  Npc_GetHeightToNpc(var C_NPC a, var C_NPC b);
var int probe_day;
var int probe_height;
func void ProbeBook() {
	var int d; d = Doc_Create(); Doc_SetPages(d, 2);
	Doc_PrintLine(d, 0, "First line"); Doc_PrintLine(d, 0, "Second line"); Doc_PrintLine(d, 1, "Over the page");
	Doc_Show(d);
};
func void ProbeMap() { var int d; d = Doc_CreateMap(); Doc_Show(d); };
func void ProbeDay() { probe_day = Wld_GetDay(); };
func void ProbeLate() { Wld_SetTime(23, 0); };
func void ProbeMorning() { Wld_SetTime(6, 30); };
func void ProbeHeight() { probe_height = Npc_GetHeightToNpc(self, self); };
)", src);
	ScriptBakeResult baked;
	const std::vector<u8> image = bakeScript(src, baked);
	Game game;
	u8* blob = static_cast<u8*>(std::malloc(image.size()));
	std::memcpy(blob, image.data(), image.size());
	CHECK(game.adopt(blob, u32(image.size())));
	const u32 hero = game.heroIndex();
	const auto call = [&](const char* name) { return game.callAs(game.function(name), hero, -1); };
	const auto number = [&](const char* name) { return game.vm().getInt(game.vm().find(name)); };

	// A book: pages of lines, put up by Doc_Show once.
	CHECK(game.documentShown() == nullptr);
	call("PROBEBOOK");
	const GameDocument* doc = game.documentShown();
	CHECK(doc && !doc->map && doc->pageCount == 2);
	if (doc) {
		CHECK(std::strcmp(doc->pages[0].text, "First line\nSecond line\n") == 0);
		CHECK(std::strcmp(doc->pages[1].text, "Over the page\n") == 0);
	}
	const u32 serial = game.documentSerial();
	game.documentDone();
	CHECK(game.documentShown() == nullptr && game.documentSerial() == serial);
	call("PROBEMAP");
	CHECK(game.documentShown() && game.documentShown()->map && game.documentSerial() == serial + 1);
	game.documentDone();

	// The clock: a time put earlier than it is makes the next day; Wld_GetDay counts them.
	float hours = 0.0f;
	CHECK(!game.takeTimeSet(hours));
	game.jumpClock(8.0f);
	call("PROBEDAY");
	CHECK(number("PROBE_DAY") == 0);
	call("PROBELATE");
	CHECK(game.takeTimeSet(hours) && fabsf(hours - 23.0f) < 0.01f && !game.takeTimeSet(hours));
	game.jumpClock(hours);
	call("PROBEMORNING");
	CHECK(game.takeTimeSet(hours) && fabsf(hours - 6.5f) < 0.01f);
	game.jumpClock(hours);
	call("PROBEDAY");
	CHECK(number("PROBE_DAY") == 1);
	call("PROBEHEIGHT");
	CHECK(number("PROBE_HEIGHT") == 0);

	// A day goes in a save.
	ByteSink sink;
	game.save(sink);
	CHECK(!sink.failed);
	Game other;
	u8* blob2 = static_cast<u8*>(std::malloc(image.size()));
	std::memcpy(blob2, image.data(), image.size());
	CHECK(other.adopt(blob2, u32(image.size())));
	ByteSource source(sink.data, sink.size);
	CHECK(other.restore(source) && other.day() == 1);
}

// Summoned and inserted later: made at a character, kept in a save and made again by it; rooms of guilds.
static void testLateAndRooms() {
	SrcScript src;
	compileDaedalus(std::string(syntheticScriptSource()), src);
	ScriptBakeResult baked;
	const std::vector<u8> image = bakeScript(src, baked);
	SynthOptions so;
	so.cells = 48;
	SrcScene world;
	buildSyntheticScene(so, world);
	const Game::PointFinder find = [](void* user, const char* name, float pos[3], float& yaw) {
		const SrcScene& scene = *static_cast<const SrcScene*>(user);
		std::string up = name;
		for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
		const auto it = scene.points.find(up);
		if (it == scene.points.end()) return false;
		std::copy_n(it->second.pos, 3, pos);
		yaw = it->second.yaw;
		return true;
	};
	const auto adopt = [&](Game& g) {
		u8* blob = static_cast<u8*>(std::malloc(image.size()));
		std::memcpy(blob, image.data(), image.size());
		return g.adopt(blob, u32(image.size()));
	};
	Game game;
	game.setQuiet(true);
	CHECK(adopt(game));
	game.enterWorld("WORLD", 8.0f, find, &world);
	const u32 before = game.npcCount();
	for (u32 i = 0; i < before; ++i) CHECK(!game.npc(i).late);
	const u32 hero = game.heroIndex();
	const float at[3] = {100.0f, 0.0f, 200.0f};
	game.setHeroPlace(at, 0.0f);
	const u32 molerat = game.vm().find("MOLERAT");
	CHECK(molerat != SCRIPT_NONE);
	const u32 first = game.spawnNear(molerat, hero, 2);
	CHECK(first == before && game.npcCount() == before + 2);
	if (first != before) return;
	const GameNpc& m = game.npc(first);
	CHECK(m.late && m.spawned && m.placed && m.world == game.currentWorld());
	CHECK(std::fabs(m.pos[2] - 300.0f) < 1.0f && std::fabs(m.pos[0] - 100.0f) < 1.0f);
	// A new minute: the summoned stays where it was put.
	game.setTime(9.0f, find, &world);
	CHECK(game.npc(first).placed && std::fabs(game.npc(first).pos[2] - 300.0f) < 1.0f);
	// Rooms: the guild given to a room, -1 outside, GIL_NONE for a room nobody was given.
	CHECK(game.roomGuild(0) == -1 && game.roomGuild(hashName("HUT")) == 0);
	CHECK(!game.setRoom(hero, 0) && game.setRoom(hero, hashName("HUT")) && game.heroRoom() == hashName("HUT") && game.heroFormerRoom() == 0);
	// A save keeps them; the loading game makes them again.
	ByteSink sink;
	game.save(sink);
	CHECK(!sink.failed);
	Game other;
	other.setQuiet(true);
	CHECK(adopt(other));
	other.enterWorld("WORLD", 8.0f, find, &world);
	CHECK(other.npcCount() == before);
	ByteSource source(sink.data, sink.size);
	CHECK(other.restore(source));
	CHECK(other.npcCount() == before + 2);
	if (other.npcCount() == before + 2) {
		const GameNpc& r = other.npc(before);
		CHECK(r.symbol == molerat && r.late && r.spawned && r.placed && std::fabs(r.pos[2] - 300.0f) < 1.0f);
	}
}

// The game layer on the built-in script: who is where, wearing what.
static void testGame() {
	// The built-in script, plus a world whose start-up names a place nobody has.
	SrcScript src;
	compileDaedalus(std::string(syntheticScriptSource()) +
	                    "func void STARTUP_LOST() { Wld_InsertNpc(Molerat, \"NO_SUCH_POINT\"); Wld_InsertNpc(ItMw_Sword, \"X\"); };",
	                src);
	ScriptBakeResult baked;
	const std::vector<u8> image = bakeScript(src, baked);
	SynthOptions so;
	so.cells = 48;
	SrcScene world, cave;
	buildSyntheticScene(so, world);
	buildSyntheticCave(cave);
	const Game::PointFinder find = [](void* user, const char* name, float pos[3], float& yaw) {
		const SrcScene& scene = *static_cast<const SrcScene*>(user);
		std::string up = name;
		for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
		const auto it = scene.points.find(up);
		if (it == scene.points.end()) return false;
		std::copy_n(it->second.pos, 3, pos);
		yaw = it->second.yaw;
		return true;
	};
	const auto adopt = [&](Game& g) {
		u8* blob = static_cast<u8*>(std::malloc(image.size()));
		std::memcpy(blob, image.data(), image.size());
		return g.adopt(blob, u32(image.size()));
	};
	Game game;
	CHECK(!game.loaded() && game.npcCount() == 0);
	CHECK(adopt(game));
	const auto byName = [&](const char* instance, u32 nth = 0) -> const GameNpc* {
		const u32 sym = game.vm().find(instance);
		for (u32 i = 0; i < game.npcCount(); ++i)
			if (game.npc(i).symbol == sym && nth-- == 0) return &game.npc(i);
		return nullptr;
	};
	const auto text = [&](GameText t) { return std::string(game.text(t)); };

	// The player's character is there before any world is.
	CHECK(game.npcCount() == 1 && game.hero() && game.hero()->hero);
	{
		const GameLook& l = game.hero()->look;
		CHECK(text(l.script) == "humans" && text(l.body) == "hum_body_naked0" && text(l.head) == "hum_head_pony");
		CHECK(l.bodyTex == 4 && l.skin == 1 && l.headTex == 9 && l.teethTex == 0 && l.armor == 0);
		CHECK(std::string(game.nameOf(*game.hero())) == "I");
		CHECK(game.vm().instanceOf(game.vm().heroSymbol()) == game.hero()->object);
	}

	game.enterWorld("WORLD", 8.0f, find, &world);
	CHECK(game.stats().npcs == 9 && game.npcCount() == 9 && game.stats().withRoutine == 4 && game.stats().items == 1);
	CHECK(game.stats().unplaced == 0 && game.stats().worlds == 1 && game.stats().notNpc == 0);
	CHECK(game.vm().getInt(game.vm().find("KAPITEL")) == 1 && game.vm().getInt(game.vm().find("CAMP_POPULATION")) == 4);
	CHECK(game.vm().stats().errors == 0 && game.vm().stats().aborted == 0);

	const GameNpc* thorus = byName("GRD_200_THORUS");
	const GameNpc* jackal = byName("GRD_201_JACKAL");
	const GameNpc* digger = byName("VLK_500_DIGGER");
	const GameNpc* walker = byName("VLK_501_DIGGER");
	CHECK(thorus && jackal && digger && walker);
	if (thorus && jackal && digger && walker) {
		CHECK(std::string(game.nameOf(*thorus)) == "Thorus" && std::string(game.nameOf(*jackal)) == "Guard");
		// Armour is read off the item; it replaces the bare body.
		CHECK(text(thorus->look.armor) == "hum_grdm_armor" && text(thorus->look.head) == "hum_head_fighter");
		CHECK(thorus->look.bodyTex == 0 && thorus->look.skin == 1 && thorus->look.headTex == 12);
		CHECK(text(digger->look.armor) == "hum_vlkl_armor" && digger->look.skin == 0 && digger->look.teethTex == 2);
		CHECK(walker->look.armor == 0 && text(walker->look.body) == "hum_body_naked0");
		// B_Scale: by strength.
		CHECK(thorus->look.scale[0] == 1.1f && thorus->look.scale[1] == 1.0f && digger->look.scale[0] == 0.9f);
		CHECK(jackal->look.fatness == 1.0f && digger->look.fatness == -1.0f);
		// 8:00 by their routines.
		CHECK(thorus->routineCount == 2 && jackal->routineCount == 3 && digger->routineCount == 3);
		CHECK(thorus->placed && text(thorus->at) == "CAMP_GATE_GUARD_L" && text(jackal->at) == "CAMP_GATE_GUARD_R");
		CHECK(text(digger->at) == "CAMP_SMITH" && text(walker->at) == "ROAD_1");
		CHECK(thorus->state == game.vm().find("ZS_GUARD") && digger->state == game.vm().find("ZS_SMITH"));
		const SrcPoint& gate = world.points["CAMP_GATE_GUARD_L"];
		CHECK(thorus->pos[0] == gate.pos[0] && thorus->pos[1] == gate.pos[1] && thorus->pos[2] == gate.pos[2] &&
		      thorus->yaw == gate.yaw);
		// Thorus and the first guard share the armour, not the face.
		CHECK(!(thorus->look == jackal->look) && game.lookKey(thorus->look) != game.lookKey(jackal->look));
	}
	// Creatures: no routine, they are where the script put them.
	const GameNpc* sc1 = byName("SCAVENGER", 0);
	const GameNpc* sc2 = byName("SCAVENGER", 1);
	const GameNpc* young = byName("YSCAVENGER");
	CHECK(sc1 && sc2 && young);
	if (sc1 && sc2 && young) {
		CHECK(text(sc1->look.script) == "scavenger" && text(sc1->look.body) == "sca_body" && sc1->look.head == 0);
		CHECK(sc1->routineCount == 0 && sc1->placed && text(sc1->at) == "FOREST_SCAVENGER_1" && sc1->state == 0);
		CHECK(sc1->look == sc2->look && sc1->look == young->look && game.lookKey(sc1->look) == game.lookKey(young->look));
		CHECK(young->look.scale[0] == 0.8f && std::string(game.nameOf(*young)) == "Young scavenger");
	}
	CHECK(game.countLooks() == 7); // hero, Thorus, Jackal, two diggers, scavengers, molerats

	// The clock moves on: the routines do.
	const u32 moved = game.setTime(21.0f, find, &world);
	CHECK(moved == 3); // Jackal and both diggers; Thorus guards the gate all day
	if (thorus && jackal && digger && walker) {
		CHECK(text(thorus->at) == "CAMP_GATE_GUARD_L" && thorus->moved == 1);
		CHECK(text(jackal->at) == "CAMP_FIRE" && text(digger->at) == "CAMP_FIRE" && text(walker->at) == "CAMP_FIRE");
		CHECK(jackal->state == game.vm().find("ZS_SITAROUND"));
		// Three at one fire: around it, not inside each other.
		const SrcPoint& fire = world.points["CAMP_FIRE"];
		const auto dist = [](const float* a, const float* b) { return std::hypot(a[0] - b[0], a[2] - b[2]); };
		CHECK(dist(jackal->pos, fire.pos) < 1.0f && dist(digger->pos, fire.pos) > 60.0f && dist(walker->pos, fire.pos) > 60.0f);
		CHECK(dist(digger->pos, walker->pos) > 60.0f && dist(walker->pos, fire.pos) < 450.0f);
	}
	CHECK(game.setTime(21.5f, find, &world) == 0);
	// Over midnight: Jackal sleeps from 23:30 to 7:30.
	game.setTime(2.0f, find, &world);
	if (jackal && digger) CHECK(text(jackal->at) == "CAMP_HUT_1" && text(digger->at) == "CAMP_HUT_2");
	game.setTime(23.75f, find, &world);
	if (jackal) CHECK(text(jackal->at) == "CAMP_HUT_1");

	// Another world: its own start-up, its own people; back again, nobody twice.
	game.enterWorld("CAVE", 8.0f, find, &cave);
	CHECK(game.npcCount() == 12 && game.stats().worlds == 2 && game.currentWorld() == 1 && game.stats().unplaced == 0);
	const GameNpc* caveGuard = byName("GRD_210_CAVEGUARD");
	CHECK(caveGuard && caveGuard->world == 1 && caveGuard->placed && text(caveGuard->at) == "CAVE_GUARD");
	if (caveGuard && thorus) CHECK(caveGuard->look == thorus->look && thorus->world == 0);
	game.enterWorld("WORLD", 8.0f, find, &world);
	CHECK(game.npcCount() == 12 && game.stats().worlds == 2 && game.currentWorld() == 0 && game.stats().unplaced == 0);
	if (thorus) CHECK(text(thorus->at) == "CAMP_GATE_GUARD_L");
	// A world the scripts do not know, and one whose start-up asks for the impossible.
	game.enterWorld("NOWHERE", 8.0f, find, &world);
	CHECK(game.npcCount() == 12 && game.stats().worlds == 3 && game.stats().unplaced == 0);
	game.enterWorld("LOST", 8.0f, find, &world);
	CHECK(game.npcCount() == 13 && game.stats().unplaced == 1 && game.stats().notNpc == 1);
	const GameNpc* lost = byName("MOLERAT", 3);
	CHECK(lost && !lost->placed && text(lost->want) == "NO_SUCH_POINT" && lost->world == 3);
	CHECK(game.vm().stats().errors == 0 && game.vm().stats().aborted == 0);

	// The same again gives the same: nothing depends on chance or on leftovers.
	Game again;
	CHECK(adopt(again));
	again.enterWorld("LOST", 8.0f, find, &world); // the other way round
	again.enterWorld("CAVE", 8.0f, find, &cave);
	again.enterWorld("WORLD", 8.0f, find, &world);
	CHECK(again.npcCount() == game.npcCount());
	u32 sameLooks = 0;
	for (u32 i = 0; i < game.npcCount(); ++i)
		for (u32 k = 0; k < again.npcCount(); ++k)
			if (again.lookKey(again.npc(k).look) == game.lookKey(game.npc(i).look)) {
				++sameLooks;
				break;
			}
	CHECK(sameLooks == game.npcCount());

	// Without scripts, and with scripts that have nothing of the kind.
	Game none;
	none.enterWorld("WORLD", 8.0f, find, &world);
	CHECK(none.npcCount() == 0 && none.setTime(3.0f, find, &world) == 0 && none.hero() == nullptr);
	{
		SrcScript bare;
		compileDaedalus("var int x; func void STARTUP_WORLD() { x = 5; };", bare);
		ScriptBakeResult br;
		const std::vector<u8> img = bakeScript(bare, br);
		u8* blob = static_cast<u8*>(std::malloc(img.size()));
		std::memcpy(blob, img.data(), img.size());
		Game g;
		CHECK(g.adopt(blob, u32(img.size())) && g.hero() == nullptr);
		g.enterWorld("WORLD", 8.0f, nullptr, nullptr);
		CHECK(g.npcCount() == 0 && g.vm().getInt(g.vm().find("X")) == 5);
	}
	game.release();
	CHECK(!game.loaded() && game.npcCount() == 0);
}

#ifdef G3DS_WITH_ZENKIT
// Programs nobody wrote: random functions over globals, arrays, objects and
// strings, run by both interpreters.
static std::string randomProgram(u32 seed, std::vector<std::string>& functions) {
	std::mt19937 rng(seed);
	const auto pick = [&](int n) { return int(rng() % u32(n)); };
	std::string src = R"(
const int LIMIT = 997;
const int SMALL[4] = { 3, 5, 7, 11 };
var int g0; var int g1; var int g2; var int g3;
var int arr[8];
var string text;
var float ratio;
class C_NPC { var int a; var int b[4]; var string s; var float f; var func fn; };
instance self, other (C_NPC);
instance hero (C_NPC);
extern func void Wld_InsertNpc(var int inst, var string point);
extern func C_NPC Hlp_GetNpc(var int inst);
extern func int Hlp_GetInstanceID(var C_NPC n);
extern func int Hlp_IsValidNpc(var C_NPC n);
extern func int Hlp_Random(var int bound);
extern func int Hlp_StrCmp(var string a, var string b);
extern func string ConcatStrings(var string a, var string b);
extern func string IntToString(var int x);
extern func float IntToFloat(var int x);
extern func int FloatToInt(var float x);
extern func void Note(var string what, var int value);
extern func void NoteFloat(var float value);
extern func int Unknown(var C_NPC n, var string s, var float f, var int i);
prototype Base (C_NPC) { a = 5; b[1] = 7; s = "base"; f = 1.5; };
instance N0 (Base) { b[0] = a * 2; s = "zero"; };
instance N1 (Base) { a = 40; b[3] = b[1] + a; fn = helper; };
instance N2 (C_NPC) { a = SMALL[2]; f = 0.25; s = ConcatStrings("n", IntToString(a)); };
instance PC_HERO (Base) { a = 1; };
func int helper() { return g0 % 13 + 1; };
func int mix(var int x, var int y) { return (x % 100) * (y % 100) - x % 7; };
func void poke(var C_NPC n, var int v) { n.a = v % LIMIT; n.b[2] += 1; };
func string label(var int v) { return ConcatStrings("v", IntToString(v % 50)); };
func void STARTUP_WORLD() {
	Wld_InsertNpc(N0, "A"); Wld_InsertNpc(N1, "B"); Wld_InsertNpc(N2, "C");
	self = Hlp_GetNpc(N1); other = Hlp_GetNpc(N2);
};
)";
	static const char* const ints[] = {"g0", "g1", "g2", "g3", "arr[0]", "arr[3]", "arr[7]", "N0.a", "N1.a", "N2.b[1]",
	                                   "self.a", "other.a", "self.b[3]", "hero.a", "N1.b[2]", "SMALL[1]", "LIMIT"};
	static const char* const targets[] = {"g0", "g1", "g2", "g3", "arr[0]", "arr[3]", "arr[7]", "N0.a", "N1.a", "N2.b[1]",
	                                      "self.a", "other.a", "self.b[3]", "hero.a", "N1.b[2]"};
	static const char* const objects[] = {"N0", "N1", "N2", "self", "other", "hero"};
	static const char* const ops[] = {"+", "-", "|", "&", "<", ">", "<=", ">=", "==", "!=", "&&", "||"};
	int made = 0;
	std::function<std::string(int)> expr = [&](int depth) -> std::string {
		const int k = depth <= 0 ? pick(2) : pick(12);
		switch (k) {
		case 0: return std::to_string(pick(101) - 50);
		case 1: return ints[pick(int(std::size(ints)))];
		case 2: return "(" + expr(depth - 1) + " " + ops[pick(int(std::size(ops)))] + " " + expr(depth - 1) + ")";
		case 3: return "((" + expr(depth - 1) + ") % 100 * ((" + expr(depth - 1) + ") % 100))";
		case 4: return "(" + expr(depth - 1) + " / ((" + expr(depth - 1) + " & 7) + 1))";
		case 5: return "(" + expr(depth - 1) + " % ((" + expr(depth - 1) + " & 15) + 2))";
		case 6: return "((" + expr(depth - 1) + " % 100) << (" + expr(depth - 1) + " & 3))";
		case 7: return "(" + expr(depth - 1) + " >> (" + expr(depth - 1) + " & 3))";
		case 8: return std::string(pick(3) == 0 ? "!" : pick(2) ? "-" : "~") + "(" + expr(depth - 1) + ")";
		case 9: return "mix(" + expr(depth - 1) + ", " + expr(depth - 1) + ")";
		case 10:
			switch (pick(6)) {
			case 0: return "Hlp_Random(" + std::to_string(pick(20) + 1) + ")";
			case 1: return std::string("Hlp_GetInstanceID(") + objects[pick(6)] + ")";
			case 2: return std::string("Hlp_StrCmp(") + objects[pick(6)] + ".s, label(" + expr(depth - 1) + "))";
			case 3: return "FloatToInt(IntToFloat(" + expr(depth - 1) + " % 1000))";
			case 4: return std::string("Unknown(") + objects[pick(6)] + ", text, 0.5, " + expr(depth - 1) + ")";
			default: return "helper()";
			}
		default:
			return made > 0 ? "f" + std::to_string(pick(made)) + "()" : std::string("helper()");
		}
	};
	std::function<std::string(int)> block = [&](int depth) -> std::string {
		std::string out;
		const int n = 1 + pick(4);
		for (int i = 0; i < n; ++i) {
			switch (pick(depth > 0 ? 11 : 9)) {
			case 0:
			case 1: out += std::string("\t") + targets[pick(int(std::size(targets)))] + " = (" + expr(3) + ") % LIMIT;\n"; break;
			case 2: {
				static const char* const assign[] = {"+=", "-=", "*=", "/="};
				const int op = pick(4);
				out += std::string("\t") + targets[pick(int(std::size(targets)))] + " " + assign[op] + " " +
				       (op < 2 ? "(" + expr(2) + ") % 50" : std::to_string(pick(3) + 1)) + ";\n";
				if (op == 2) out += "\tg3 = g3 % LIMIT; g0 = g0 % LIMIT; g1 = g1 % LIMIT; g2 = g2 % LIMIT;\n";
				break;
			}
			case 3: out += "\tNote(\"" + std::to_string(pick(100)) + "\", " + expr(3) + ");\n"; break;
			case 4: out += std::string("\tpoke(") + objects[pick(6)] + ", " + expr(2) + ");\n"; break;
			case 5: out += std::string("\t") + objects[pick(6)] + ".s = label(" + expr(2) + ");\n"; break;
			case 6: out += std::string("\ttext = ") + (pick(2) ? std::string(objects[pick(6)]) + ".s" : "ConcatStrings(label(g1), \"!\")") + ";\n"; break;
			case 7:
				out += std::string("\t") + (pick(2) ? "self" : "other") + " = Hlp_GetNpc(N" + std::to_string(pick(3)) + ");\n";
				break;
			case 8:
				out += std::string("\t") + objects[pick(6)] + ".f = " + std::to_string(pick(40) - 20) + "." + std::to_string(pick(100)) +
				       ";\n\tNoteFloat(" + objects[pick(6)] + ".f);\n\tratio = " + objects[pick(6)] + ".f;\n";
				break;
			default:
				out += "\tif (" + expr(2) + ") {\n" + block(depth - 1) + "\t}";
				if (pick(2)) out += " else if (" + expr(1) + ") {\n" + block(depth - 1) + "\t}";
				if (pick(2)) out += " else {\n" + block(depth - 1) + "\t}";
				out += ";\n";
				break;
			}
		}
		return out;
	};
	// Results that are left on the stack, forgotten or returned twice are part of it.
	for (int f = 0; f < 30; ++f) {
		const std::string name = "f" + std::to_string(f);
		src += "func int " + name + "() {\n" + block(2);
		if (pick(8) == 0) src += "\thelper(); mix(1, 2);\n";
		if (pick(10) != 0) src += "\treturn (" + expr(3) + ") % LIMIT;\n";
		src += "};\n";
		functions.push_back(name);
		++made;
	}
	return src;
}

static void testAgainstReference() {
	u64 calls = 0;
	for (u32 seed = 1; seed <= 60; ++seed) {
		std::vector<std::string> functions;
		const std::string source = randomProgram(seed, functions);
		SrcScript src;
		try {
			compileDaedalus(source, src);
		} catch (const std::exception& ex) {
			std::printf("       program %u does not compile: %s\n", seed, ex.what());
			CHECK(false);
			continue;
		}
		ScriptBakeResult baked;
		const std::vector<u8> image = bakeScript(src, baked);
		ScriptCheckOptions opt;
		opt.isolate = false;
		opt.functions = functions;
		const ScriptCheckResult r = checkScript(writeScriptDat(src), src, image, opt);
		CHECK(r.ran && r.differ == 0 && r.objected == 0 && r.stateSame && r.runs == 32);
		if (r.differ || r.objected || !r.stateSame) std::printf("       program %u: %s", seed, r.report.c_str());
		calls += r.calls;
	}
	std::printf("       60 random programs, %llu engine calls the same in both interpreters\n", static_cast<unsigned long long>(calls));
	CHECK(calls > 5000);
}
#endif

int main(int argc, char** argv) {
	const char* only = argc > 2 ? argv[2] : nullptr;
	struct {
		const char* name;
		void (*fn)();
	} tests[] = {{"basics", testBasics}, {"compiler errors", testCompilerErrors}, {"objects", testObjects}, {"broken images", testBrokenImages}, {"synthetic script", testSyntheticScript},
	             {"game", testGame}, {"docs and clock", testDocsAndClock}, {"late and rooms", testLateAndRooms},
#ifdef G3DS_WITH_ZENKIT
	             {"against the reference", testAgainstReference},
#endif
	};
	for (auto& t : tests) {
		if (only && !std::strstr(t.name, only)) continue;
		const int before = gFailures;
		std::printf("[ RUN ] %s\n", t.name);
		try {
			t.fn();
		} catch (const std::exception& ex) {
			++gFailures;
			std::printf("FAIL %s threw: %s\n", t.name, ex.what());
		}
		std::printf("[ %s ] %s\n", gFailures == before ? " OK " : "FAIL", t.name);
	}
	std::printf("%d checks, %d failed\n", gChecks, gFailures);
	return gFailures ? 1 : 0;
}
