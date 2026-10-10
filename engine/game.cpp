#include "game.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace g3 {

namespace {

template <class T> bool grow(T*& items, u32& cap, u32 need, u32 first) {
	if (need <= cap) return true;
	u32 to = cap ? cap * 2 : first;
	while (to < need) to *= 2;
	T* grown = static_cast<T*>(realloc(static_cast<void*>(items), size_t(to) * sizeof(T)));
	if (!grown) return false;
	items = grown;
	cap = to;
	return true;
}

void copyText(char* to, size_t size, const char* from) {
	size_t n = strlen(from);
	if (n >= size) n = size - 1;
	memcpy(to, from, n);
	to[n] = 0;
}

u16 minutesOf(i32 hour, i32 minute) {
	i32 m = hour * 60 + minute;
	m %= 24 * 60;
	if (m < 0) m += 24 * 60;
	return u16(m);
}

} // namespace

bool GameLook::operator==(const GameLook& o) const {
	return script == o.script && body == o.body && head == o.head && armor == o.armor && bodyTex == o.bodyTex && skin == o.skin &&
	       headTex == o.headTex && teethTex == o.teethTex;
}

// ---- set-up ----

bool Game::load(Pak& pak) {
	release();
	if (!_vm.load(pak, "script/gothic")) return false;
	return bindAll();
}

bool Game::adopt(u8* image, u32 size) {
	release();
	if (!_vm.adopt(image, size)) return false;
	return bindAll();
}

void Game::release() {
	_vm.release();
	free(_npcs);
	free(_routines);
	for (u32 i = 0; i < _textCount; ++i) free(_texts[i]);
	free(_texts);
	free(_textHash);
	free(_armors);
	free(_worlds);
	_npcs = nullptr;
	_routines = nullptr;
	_texts = nullptr;
	_textHash = nullptr;
	_armors = nullptr;
	_worlds = nullptr;
	_npcCount = _npcCap = _routineCount = _routineCap = _textCount = _textCap = 0;
	_armorCount = _armorCap = _worldCount = _worldCap = 0;
	_world = 0;
	_hero = SCRIPT_NONE;
	_seed = 1;
	_stats = GameStats {};
}

bool Game::bindAll() {
	// Text 0 is the empty one.
	if (intern("", false) != 0) {
		release();
		return false;
	}
	_clsNpc = _vm.find("C_NPC");
	_clsItem = _vm.find("C_ITEM");
	_mRoutine = _vm.findMember("C_NPC", "DAILY_ROUTINE");
	_mName = _vm.findMember("C_NPC", "NAME");
	_mVisualChange = _vm.findMember("C_ITEM", "VISUAL_CHANGE");
	_mVisualSkin = _vm.findMember("C_ITEM", "VISUAL_SKIN");
	_mWear = _vm.findMember("C_ITEM", "WEAR");
	_mMainFlag = _vm.findMember("C_ITEM", "MAINFLAG");
	_mWp = _vm.findMember("C_NPC", "WP");
	_mStartState = _vm.findMember("C_NPC", "START_AISTATE");
	_mSensesRange = _vm.findMember("C_NPC", "SENSES_RANGE");
	_mGuild = _vm.findMember("C_NPC", "GUILD");
	// (Each must be what the engine takes it for, whatever the scripts declare.)
	const auto must = [&](u32& member, u8 type) {
		if (member != SCRIPT_NONE && _vm.symbol(member).type != type) member = SCRIPT_NONE;
	};
	must(_mWp, SCRIPT_STRING);
	must(_mSensesRange, SCRIPT_INT);
	must(_mGuild, SCRIPT_INT);
	if (_mStartState != SCRIPT_NONE && _vm.symbol(_mStartState).type != SCRIPT_FUNC && _vm.symbol(_mStartState).type != SCRIPT_INT)
		_mStartState = SCRIPT_NONE;

	const struct {
		const char* name;
		ScriptVm::External fn;
	} table[] = {{"WLD_INSERTNPC", xInsertNpc},
	             {"WLD_INSERTITEM", xInsertItem},
	             {"MDL_SETVISUAL", xSetVisual},
	             {"MDL_SETVISUALBODY", xSetVisualBody},
	             {"MDL_SETMODELSCALE", xSetModelScale},
	             {"MDL_SETMODELFATNESS", xSetModelFatness},
	             {"MDL_APPLYOVERLAYMDS", xApplyOverlay},
	             {"TA_MIN", xTaMin},
	             {"TA", xTa},
	             {"EQUIPITEM", xEquipItem},
	             {"HLP_GETNPC", xGetNpc},
	             {"HLP_GETINSTANCEID", xGetInstanceId},
	             {"HLP_ISVALIDNPC", xIsValid},
	             {"HLP_ISVALIDITEM", xIsValid},
	             {"HLP_RANDOM", xRandom},
	             {"HLP_STRCMP", xStrCmp},
	             {"CONCATSTRINGS", xConcat},
	             {"INTTOSTRING", xIntToString},
	             {"INTTOFLOAT", xIntToFloat},
	             {"FLOATTOINT", xFloatToInt},
	             {"NPC_ISPLAYER", xIsPlayer},
	             {"WLD_SETTIME", xSetTime},
	             {"WLD_ISTIME", xIsTime}};
	for (const auto& e : table) _vm.bind(e.name, e.fn, this);

	// The player's character exists before any world's start-up runs.
	const u32 hero = _vm.find("PC_HERO");
	if (hero != SCRIPT_NONE && _vm.symbol(hero).type == SCRIPT_INSTANCE && (_vm.symbol(hero).flags & SCRIPTF_CONST) &&
	    _clsNpc != SCRIPT_NONE && _vm.classOf(hero) == _clsNpc) {
		_hero = addNpc(hero, true);
		if (_hero != SCRIPT_NONE && _vm.heroSymbol() != SCRIPT_NONE) _vm.setInstance(_vm.heroSymbol(), _npcs[_hero].object);
	}
	return true;
}

GameText Game::intern(const char* text, bool model) {
	char buf[80];
	copyText(buf, sizeof(buf), text ? text : "");
	if (model) {
		// "Hum_GRDM_ARMOR.asc" -> "hum_grdm_armor"
		char* dot = strrchr(buf, '.');
		if (dot) *dot = 0;
		for (char* c = buf; *c; ++c)
			if (*c >= 'A' && *c <= 'Z') *c = char(*c + 32);
	}
	const u32 h = hashName(buf);
	for (u32 i = 0; i < _textCount; ++i)
		if (_textHash[i] == h && strcmp(_texts[i], buf) == 0) return GameText(i);
	if (_textCount >= 0xFFFF) return 0;
	u32 cap = _textCap;
	if (!grow(_texts, cap, _textCount + 1, 256)) return 0;
	if (!grow(_textHash, _textCap, _textCount + 1, 256)) return 0;
	const size_t n = strlen(buf) + 1;
	char* copy = static_cast<char*>(malloc(n));
	if (!copy) return 0;
	memcpy(copy, buf, n);
	_texts[_textCount] = copy;
	_textHash[_textCount] = h;
	return GameText(_textCount++);
}

GameNpc* Game::npcOf(ScriptInstance* inst) {
	if (!inst) return nullptr;
	const size_t index = reinterpret_cast<size_t>(inst->user);
	return index >= 1 && index <= _npcCount && _npcs[index - 1].object == inst ? &_npcs[index - 1] : nullptr;
}

const char* Game::nameOf(const GameNpc& n) const {
	return _mName != SCRIPT_NONE && n.object ? _vm.getString(_mName, 0, n.object) : "";
}

u32 Game::lookKey(const GameLook& l) const {
	// The texts themselves, not their numbers: those depend on what ran before.
	u32 h = 2166136261u;
	const auto mix = [&](u32 v) { h = (h ^ v) * 16777619u; };
	mix(hashName(text(l.script)));
	mix(hashName(text(l.body)));
	mix(hashName(text(l.head)));
	mix(hashName(text(l.armor)));
	mix(u32(u16(l.bodyTex)) | (u32(u16(l.skin)) << 16));
	mix(u32(u16(l.headTex)) | (u32(u16(l.teethTex)) << 16));
	return h ? h : 1u;
}

const char* const* Game::lifeFunctions(u32& count) {
	static const char* const names[] = {
	    "AI_GOTOWP",         "AI_GOTOFP",          "AI_GOTONEXTFP",     "AI_GOTONPC",        "AI_ALIGNTOWP",     "AI_ALIGNTOFP",
	    "AI_TURNTONPC",      "AI_PLAYANI",         "AI_PLAYANIBS",      "AI_WAIT",           "AI_STANDUP",       "AI_STANDUPQUICK",
	    "AI_SETWALKMODE",    "AI_STARTSTATE",      "AI_CONTINUEROUTINE", "AI_TELEPORT",      "NPC_CLEARAIQUEUE", "NPC_GETDISTTOWP",
	    "NPC_GETDISTTONPC",  "NPC_GETDISTTOPLAYER", "NPC_GETNEARESTWP", "NPC_GETNEXTWP",     "NPC_ISONFP",       "WLD_ISFPAVAILABLE",
	    "WLD_ISNEXTFPAVAILABLE", "NPC_ISINSTATE",  "NPC_WASINSTATE",    "NPC_GETSTATETIME",  "NPC_SETSTATETIME", "NPC_GETBODYSTATE",
	    "NPC_SETPERCTIME",   "WLD_DETECTNPC",      "NPC_CANSEENPC"};
	count = u32(sizeof(names) / sizeof(names[0]));
	return names;
}

u32 Game::countLooks() const {
	u32 looks = 0;
	for (u32 i = 0; i < _npcCount; ++i) {
		bool seen = false;
		for (u32 k = 0; k < i && !seen; ++k) seen = _npcs[k].look == _npcs[i].look;
		if (!seen) ++looks;
	}
	return looks;
}

// Makes the character `symbol` defines: its object, then its code (which
// dresses it), then its daily routine.
u32 Game::addNpc(u32 symbol, bool hero) {
	if (!grow(_npcs, _npcCap, _npcCount + 1, 256)) return SCRIPT_NONE;
	ScriptInstance* inst = _vm.allocate(symbol);
	if (!inst) return SCRIPT_NONE;
	const u32 index = _npcCount++;
	_npcs[index] = GameNpc {};
	_npcs[index].object = inst;
	_npcs[index].symbol = symbol;
	_npcs[index].world = _world;
	_npcs[index].hero = hero;
	_npcs[index].routineFirst = _routineCount;
	inst->user = reinterpret_cast<void*>(size_t(index) + 1);
	++_stats.npcs;
	_vm.construct(inst);

	const u32 fn = _mRoutine != SCRIPT_NONE ? u32(_vm.getInt(_mRoutine, 0, inst)) : 0u;
	if (fn != 0 && fn < _vm.symbolCount()) {
		const ScriptSymbol& f = _vm.symbol(fn);
		if (f.type == SCRIPT_FUNC && (f.flags & SCRIPTF_CONST) && !(f.flags & SCRIPTF_EXTERNAL) && f.count == 0) {
			const u32 self = _vm.selfSymbol();
			ScriptInstance* const before = _vm.instanceOf(self);
			_vm.setInstance(self, inst);
			_vm.call(fn);
			if (f.flags & SCRIPTF_RETURN) (void)_vm.popInt();
			_vm.setInstance(self, before);
		}
	}
	// (The table of characters may have moved meanwhile.)
	GameNpc& n = _npcs[index];
	// Routine entries are the ones added since; another character's cannot
	// have got in between, as routines insert nobody.
	n.routineCount = u16(min<u32>(_routineCount - n.routineFirst, 0xFFFF));
	if (n.routineCount) ++_stats.withRoutine;
	return index;
}

// What wearing the item `symbol` does to a body. The item is made once to
// read that off it.
const Game::Armor* Game::armorOf(u32 symbol) {
	for (u32 i = 0; i < _armorCount; ++i)
		if (_armors[i].symbol == symbol) return &_armors[i];
	if (symbol >= _vm.symbolCount() || _clsItem == SCRIPT_NONE || _mVisualChange == SCRIPT_NONE) return nullptr;
	const ScriptSymbol& s = _vm.symbol(symbol);
	if (s.type != SCRIPT_INSTANCE || !(s.flags & SCRIPTF_CONST) || _vm.classOf(symbol) != _clsItem) return nullptr;
	if (!grow(_armors, _armorCap, _armorCount + 1, 32)) return nullptr;
	ScriptInstance* item = _vm.create(symbol);
	if (!item) return nullptr;
	Armor a {};
	a.symbol = symbol;
	a.mesh = intern(_vm.getString(_mVisualChange, 0, item), true);
	a.skin = _mVisualSkin != SCRIPT_NONE ? i16(_vm.getInt(_mVisualSkin, 0, item)) : i16(0);
	a.wearable = a.mesh != 0;
	_vm.destroy(item);
	_armors[_armorCount] = a;
	return &_armors[_armorCount++];
}

// ---- worlds ----

void Game::runIfPresent(const char* prefix, const char* script) {
	char name[96];
	snprintf(name, sizeof(name), "%s%s", prefix, script);
	const u32 fn = _vm.find(name);
	if (fn == SCRIPT_NONE) return;
	const ScriptSymbol& f = _vm.symbol(fn);
	if (f.type != SCRIPT_FUNC || !(f.flags & SCRIPTF_CONST) || (f.flags & SCRIPTF_EXTERNAL) || f.count != 0) return;
	_vm.call(fn);
	if (f.flags & SCRIPTF_RETURN) (void)_vm.popInt();
}

void Game::enterWorld(const char* script, float hours, PointFinder find, void* user) {
	if (!loaded() || !script) return;
	const u32 h = hashName(script);
	u32 index = _worldCount;
	for (u32 i = 0; i < _worldCount; ++i)
		if (_worlds[i].nameHash == h) index = i;
	if (index == _worldCount) {
		if (_worldCount >= 0xFFFF || !grow(_worlds, _worldCap, _worldCount + 1, 8)) return;
		_worlds[_worldCount++] = World {h, false};
	}
	_world = u16(index);
	_hours = hours;
	if (!_worlds[index].started) {
		_worlds[index].started = true;
		++_stats.worlds;
		// Chance is the same whenever and wherever a world is first entered:
		// the baker has to see what the console will.
		_seed = h | 1u;
		if (index == 0) runIfPresent("STARTUP_", "GLOBAL");
		runIfPresent("STARTUP_", script);
	}
	runIfPresent("INIT_", "GLOBAL");
	runIfPresent("INIT_", script);
	setTime(hours, find, user);
}

const GameRoutine* Game::routineAt(const GameNpc& n, float hours) const {
	if (!n.routineCount) return nullptr;
	i32 minute = i32(hours * 60.0f);
	minute %= 24 * 60;
	if (minute < 0) minute += 24 * 60;
	for (u32 i = 0; i < n.routineCount; ++i) {
		const GameRoutine& r = _routines[n.routineFirst + i];
		const bool in = r.start < r.stop ? (minute >= r.start && minute < r.stop) : (minute >= r.start || minute < r.stop);
		if (in) return &r;
	}
	return &_routines[n.routineFirst];
}

int Game::indexOf(const ScriptInstance* inst) const {
	if (!inst) return -1;
	const size_t index = reinterpret_cast<size_t>(inst->user);
	return index >= 1 && index <= _npcCount && _npcs[index - 1].object == inst ? int(index - 1) : -1;
}

u32 Game::behaviour(const GameNpc& n) const {
	if (n.routineCount) return n.state;
	if (_mStartState == SCRIPT_NONE || !n.object) return 0;
	const u32 fn = u32(_vm.getInt(_mStartState, 0, n.object));
	return _vm.isPlainFunction(fn) ? fn : 0u;
}

float Game::sensesRange(const GameNpc& n) const {
	const i32 r = _mSensesRange != SCRIPT_NONE && n.object ? _vm.getInt(_mSensesRange, 0, n.object) : 0;
	return r > 0 ? float(r) : 0.0f;
}

void Game::setHeroPlace(const float pos[3], float yaw) {
	if (_hero == SCRIPT_NONE) return;
	GameNpc& h = _npcs[_hero];
	memcpy(h.pos, pos, sizeof(h.pos));
	h.yaw = yaw;
	h.placed = true;
	h.world = _world;
}

void Game::settle(u32 index, PointFinder find, void* user) {
	if (index >= _npcCount || _npcs[index].hero) return;
	GameNpc& n = _npcs[index];
	n.active = false;
	placeNow(n, find, user);
}

void Game::place(GameNpc& n, float hours, PointFinder find, void* user) {
	const GameRoutine* r = routineAt(n, hours);
	const GameText at = r ? r->point : n.spawn;
	n.state = r ? r->state : 0u;
	if (n.moved && n.want == at) return;
	n.want = at;
	// The scripts know where the character is to be through its "wp".
	if (_mWp != SCRIPT_NONE && n.object) _vm.setString(_mWp, 0, n.object, text(at));
	if (n.active && n.placed) return; // it walks there itself
	placeNow(n, find, user);
}

void Game::placeNow(GameNpc& n, PointFinder find, void* user) {
	const GameText at = n.want;
	n.at = at;
	n.placed = find && at != 0 && find(user, text(at), n.pos, n.yaw);
	if (!n.placed && n.routineCount && n.spawn != 0 && find) {
		// A routine that names a place the world lacks: where the script put him.
		n.placed = find(user, text(n.spawn), n.pos, n.yaw);
		if (n.placed) n.at = n.spawn;
	}
	++n.moved;
	++_stats.moves;
	if (!n.placed) return;
	// Several at one place stand around it, not inside each other.
	u32 others = 0;
	for (const GameNpc* o = _npcs; o != &n; ++o)
		if (o->placed && o->world == n.world && o->at == n.at && !o->hero && !o->active) ++others;
	if (others) {
		const float angle = n.yaw + float(others) * 2.4f;
		const float radius = min(70.0f + 45.0f * float(others), 400.0f);
		n.pos[0] += sinf(angle) * radius;
		n.pos[2] += cosf(angle) * radius;
		n.yaw += float(others) * 1.1f;
	}
}

u32 Game::setTime(float hours, PointFinder find, void* user) {
	_hours = hours;
	const u32 before = _stats.moves;
	_stats.unplaced = 0;
	for (u32 i = 0; i < _npcCount; ++i) {
		GameNpc& n = _npcs[i];
		if (n.hero || n.world != _world) continue;
		place(n, hours, find, user);
		if (!n.placed) ++_stats.unplaced;
	}
	return _stats.moves - before;
}

// ---- the engine functions ----
//
// Arguments come off the stack last one first.

void Game::xInsertNpc(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	char point[80];
	copyText(point, sizeof(point), vm.popString());
	const u32 symbol = u32(vm.popInt());
	const bool isNpc = symbol < vm.symbolCount() && vm.symbol(symbol).type == SCRIPT_INSTANCE &&
	                   (vm.symbol(symbol).flags & SCRIPTF_CONST) && g._clsNpc != SCRIPT_NONE && vm.classOf(symbol) == g._clsNpc;
	if (!isNpc) {
		++g._stats.notNpc;
		return;
	}
	const u32 index = g.addNpc(symbol, false);
	if (index != SCRIPT_NONE) g._npcs[index].spawn = g.intern(point, false);
}

void Game::xInsertItem(ScriptVm& vm, void* user) {
	(void)vm.popString();
	(void)vm.popInt();
	++static_cast<Game*>(user)->_stats.items;
}

void Game::xSetVisual(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const GameText script = g.intern(vm.popString(), true);
	if (GameNpc* n = g.npcOf(vm.popInstance())) n->look.script = script;
}

void Game::xSetVisualBody(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 armor = vm.popInt();
	const i32 teethTex = vm.popInt();
	const i32 headTex = vm.popInt();
	const GameText head = g.intern(vm.popString(), true);
	const i32 skin = vm.popInt();
	const i32 bodyTex = vm.popInt();
	const GameText body = g.intern(vm.popString(), true);
	ScriptInstance* const inst = vm.popInstance();
	// (Looking at the armour runs scripts: only then touch the character.)
	const Armor* worn = armor >= 0 ? g.armorOf(u32(armor)) : nullptr;
	GameNpc* n = g.npcOf(inst);
	if (!n) return;
	n->look.body = body;
	n->look.head = head;
	n->look.bodyTex = i16(bodyTex);
	n->look.skin = i16(skin);
	n->look.headTex = i16(headTex);
	n->look.teethTex = i16(teethTex);
	n->look.armor = worn && worn->wearable ? worn->mesh : GameText(0);
}

void Game::xSetModelScale(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const float z = vm.popFloat(), y = vm.popFloat(), x = vm.popFloat();
	GameNpc* n = g.npcOf(vm.popInstance());
	if (!n) return;
	// Nonsense stays out of the transforms.
	const float v[3] = {x, y, z};
	for (int i = 0; i < 3; ++i) n->look.scale[i] = v[i] > 0.05f && v[i] < 20.0f ? v[i] : 1.0f;
}

void Game::xSetModelFatness(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const float f = vm.popFloat();
	if (GameNpc* n = g.npcOf(vm.popInstance())) n->look.fatness = f > -10.0f && f < 10.0f ? f : 0.0f;
}

void Game::xApplyOverlay(ScriptVm& vm, void*) {
	// Another way of walking: nothing a standing character shows.
	(void)vm.popString();
	(void)vm.popInstance();
}

void Game::xTaMin(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const GameText point = g.intern(vm.popString(), false);
	const u32 state = u32(vm.popInt());
	const i32 stopM = vm.popInt(), stopH = vm.popInt(), startM = vm.popInt(), startH = vm.popInt();
	GameNpc* n = g.npcOf(vm.popInstance());
	if (!n || !grow(g._routines, g._routineCap, g._routineCount + 1, 512)) return;
	// (npcOf's pointer is still good: nothing has run since.)
	g._routines[g._routineCount++] = GameRoutine {minutesOf(startH, startM), minutesOf(stopH, stopM), state, point};
}

void Game::xTa(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const GameText point = g.intern(vm.popString(), false);
	const u32 state = u32(vm.popInt());
	const i32 stopH = vm.popInt(), startH = vm.popInt();
	GameNpc* n = g.npcOf(vm.popInstance());
	if (!n || !grow(g._routines, g._routineCap, g._routineCount + 1, 512)) return;
	g._routines[g._routineCount++] = GameRoutine {minutesOf(startH, 0), minutesOf(stopH, 0), state, point};
}

void Game::xEquipItem(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 item = vm.popInt();
	ScriptInstance* const inst = vm.popInstance();
	const Armor* worn = item >= 0 ? g.armorOf(u32(item)) : nullptr;
	GameNpc* n = g.npcOf(inst);
	if (!n || !worn || !worn->wearable) return;
	n->look.armor = worn->mesh;
}

void Game::xGetNpc(ScriptVm& vm, void*) {
	const u32 symbol = u32(vm.popInt());
	vm.pushInstance(vm.instanceOf(symbol));
}

void Game::xGetInstanceId(ScriptVm& vm, void*) {
	const ScriptInstance* inst = vm.popInstance();
	vm.pushInt(inst ? i32(inst->symbol) : -1);
}

void Game::xIsValid(ScriptVm& vm, void*) { vm.pushInt(vm.popInstance() != nullptr); }

void Game::xRandom(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 bound = vm.popInt();
	g._seed ^= g._seed << 13;
	g._seed ^= g._seed >> 17;
	g._seed ^= g._seed << 5;
	vm.pushInt(bound > 0 ? i32(g._seed % u32(bound)) : 0);
}

void Game::xStrCmp(ScriptVm& vm, void*) {
	// Both stay valid: nothing runs between the two.
	const char* b = vm.popString();
	const char* a = vm.popString();
	// The same text, whatever the case of its letters.
	const auto fold = [](char c) { return c >= 'a' && c <= 'z' ? char(c - 32) : c; };
	while (*a && fold(*a) == fold(*b)) {
		++a;
		++b;
	}
	vm.pushInt(*a == 0 && *b == 0);
}

void Game::xConcat(ScriptVm& vm, void*) {
	const char* b = vm.popString();
	const char* a = vm.popString();
	const size_t na = strlen(a), nb = strlen(b);
	char* joined = static_cast<char*>(malloc(na + nb + 1));
	if (!joined) {
		vm.pushString("");
		return;
	}
	memcpy(joined, a, na);
	memcpy(joined + na, b, nb + 1);
	vm.pushString(joined);
	free(joined);
}

void Game::xIntToString(ScriptVm& vm, void*) {
	char buf[16];
	snprintf(buf, sizeof(buf), "%d", int(vm.popInt()));
	vm.pushString(buf);
}

void Game::xIntToFloat(ScriptVm& vm, void*) { vm.pushFloat(float(vm.popInt())); }

void Game::xFloatToInt(ScriptVm& vm, void*) {
	const float f = vm.popFloat();
	vm.pushInt(f > -2e9f && f < 2e9f ? i32(f) : 0);
}

void Game::xIsPlayer(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const ScriptInstance* inst = vm.popInstance();
	vm.pushInt(inst && g._hero != SCRIPT_NONE && g._npcs[g._hero].object == inst);
}

void Game::xSetTime(ScriptVm& vm, void*) {
	// The clock is the player's to set here.
	(void)vm.popInt();
	(void)vm.popInt();
}

void Game::xIsTime(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 m2 = vm.popInt(), h2 = vm.popInt(), m1 = vm.popInt(), h1 = vm.popInt();
	const i32 from = minutesOf(h1, m1), to = minutesOf(h2, m2);
	const i32 now = minutesOf(0, i32(g._hours * 60.0f));
	vm.pushInt(from < to ? (now >= from && now < to) : (now >= from || now < to));
}

} // namespace g3
