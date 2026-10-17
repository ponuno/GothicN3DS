// What the scripts say about talking to people: the infos (C_INFO) every
// character has, the ways on in the middle of a talk (Info_AddChoice), the
// lines printed on the screen, and the change of a daily routine. The
// parts that need a living character - the words said, the menu of the
// talk - are in npc_dialog.cpp.
#include "game.h"

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
	size_t n = from ? strlen(from) : 0;
	if (n >= size) n = size - 1;
	if (n) memcpy(to, from, n);
	to[n] = 0;
}

} // namespace

// ---- the infos ----

void Game::bindDialogues() {
	_mInfoNpc = _vm.findMember("C_INFO", "NPC");
	_mInfoNr = _vm.findMember("C_INFO", "NR");
	_mInfoCondition = _vm.findMember("C_INFO", "CONDITION");
	_mInfoInformation = _vm.findMember("C_INFO", "INFORMATION");
	_mInfoDescription = _vm.findMember("C_INFO", "DESCRIPTION");
	_mInfoImportant = _vm.findMember("C_INFO", "IMPORTANT");
	_mInfoPermanent = _vm.findMember("C_INFO", "PERMANENT");
	_mInfoTrade = _vm.findMember("C_INFO", "TRADE");
	_mId = _vm.findMember("C_NPC", "ID");
	_kapitel = _vm.find("KAPITEL");
	if (_kapitel != SCRIPT_NONE && _vm.symbol(_kapitel).type != SCRIPT_INT) _kapitel = SCRIPT_NONE;
	const auto must = [&](u32& member, u8 type) {
		if (member != SCRIPT_NONE && _vm.symbol(member).type != type) member = SCRIPT_NONE;
	};
	must(_mInfoNpc, SCRIPT_INT);
	must(_mInfoNr, SCRIPT_INT);
	must(_mInfoDescription, SCRIPT_STRING);
	must(_mInfoImportant, SCRIPT_INT);
	must(_mInfoPermanent, SCRIPT_INT);
	must(_mInfoTrade, SCRIPT_INT);
	must(_mId, SCRIPT_INT);
	// (condition and information are functions, or ints holding one, by how the scripts declare them.)
	if (_mInfoCondition != SCRIPT_NONE && _vm.symbol(_mInfoCondition).type != SCRIPT_FUNC && _vm.symbol(_mInfoCondition).type != SCRIPT_INT)
		_mInfoCondition = SCRIPT_NONE;
	if (_mInfoInformation != SCRIPT_NONE && _vm.symbol(_mInfoInformation).type != SCRIPT_FUNC &&
	    _vm.symbol(_mInfoInformation).type != SCRIPT_INT)
		_mInfoInformation = SCRIPT_NONE;

	const struct {
		const char* name;
		ScriptVm::External fn;
	} table[] = {{"NPC_KNOWSINFO", xKnowsInfo},         {"NPC_CHECKINFO", xCheckInfo},
	             {"INFO_ADDCHOICE", xAddChoice},         {"INFO_CLEARCHOICES", xClearChoices},
	             {"NPC_EXCHANGEROUTINE", xExchangeRoutine}, {"PRINT", xPrint},
	             {"PRINTSCREEN", xPrintScreen},          {"NPC_KNOWSPLAYER", xKnowsPlayer},
	             {"NPC_SETKNOWSPLAYER", xSetKnowsPlayer},
	             {"NPC_SETTALENTVALUE", xSetTalentValue}, {"NPC_GETTALENTVALUE", xGetTalentValue},
	             {"NPC_SETTALENTSKILL", xSetTalentSkill}, {"NPC_GETTALENTSKILL", xGetTalentSkill}, {"INTRODUCECHAPTER", xIntroduceChapter}};
	for (const auto& e : table) _vm.bind(e.name, e.fn, this);

	// Every C_INFO instance the scripts define, made once: its code says whom it belongs to.
	const u32 cls = _vm.find("C_INFO");
	if (cls == SCRIPT_NONE || _mInfoNpc == SCRIPT_NONE) return;
	u32 count = 0;
	for (u32 i = 0; i < _vm.symbolCount(); ++i) {
		const ScriptSymbol& s = _vm.symbol(i);
		if (s.type == SCRIPT_INSTANCE && (s.flags & SCRIPTF_CONST) && _vm.classOf(i) == cls) ++count;
	}
	if (!count) return;
	_infos = static_cast<GameInfo*>(calloc(count, sizeof(GameInfo)));
	if (!_infos) return;
	for (u32 i = 0; i < _vm.symbolCount(); ++i) {
		const ScriptSymbol& s = _vm.symbol(i);
		if (!(s.type == SCRIPT_INSTANCE && (s.flags & SCRIPTF_CONST) && _vm.classOf(i) == cls)) continue;
		ScriptInstance* obj = _vm.create(i);
		if (!obj) continue;
		GameInfo& in = _infos[_infoCount++];
		in = GameInfo();
		in.object = obj;
		in.symbol = i;
		in.npc = u32(_vm.getInt(_mInfoNpc, 0, obj));
		if (_mInfoNr != SCRIPT_NONE) in.nr = _vm.getInt(_mInfoNr, 0, obj);
		if (_mInfoCondition != SCRIPT_NONE) in.condition = u32(_vm.getInt(_mInfoCondition, 0, obj));
		if (_mInfoInformation != SCRIPT_NONE) in.information = u32(_vm.getInt(_mInfoInformation, 0, obj));
		if (_mInfoImportant != SCRIPT_NONE) in.important = _vm.getInt(_mInfoImportant, 0, obj) != 0;
		if (_mInfoPermanent != SCRIPT_NONE) in.permanent = _vm.getInt(_mInfoPermanent, 0, obj) != 0;
		if (_mInfoTrade != SCRIPT_NONE) in.trade = _vm.getInt(_mInfoTrade, 0, obj) != 0;
	}
}

int Game::infoIndex(u32 symbol) const {
	// (They are in the order of their symbols.)
	u32 lo = 0, hi = _infoCount;
	while (lo < hi) {
		const u32 mid = (lo + hi) / 2;
		if (_infos[mid].symbol < symbol)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < _infoCount && _infos[lo].symbol == symbol ? int(lo) : -1;
}

const char* Game::infoDescription(u32 i) const {
	if (i >= _infoCount || _mInfoDescription == SCRIPT_NONE) return "";
	return _vm.getString(_mInfoDescription, 0, _infos[i].object);
}

i32 Game::callAs(u32 fn, u32 self, int other) {
	if (!_vm.isPlainFunction(fn)) return 0;
	const u32 s = _vm.selfSymbol(), o = _vm.otherSymbol();
	ScriptInstance* const selfBefore = _vm.instanceOf(s);
	ScriptInstance* const otherBefore = _vm.instanceOf(o);
	_vm.setInstance(s, self < _npcCount ? _npcs[self].object : nullptr);
	_vm.setInstance(o, other >= 0 && u32(other) < _npcCount ? _npcs[other].object : nullptr);
	const i32 result = _vm.callInt(fn);
	_vm.setInstance(s, selfBefore);
	_vm.setInstance(o, otherBefore);
	return result;
}

bool Game::infoAvailable(u32 info, u32 npc) {
	if (info >= _infoCount || npc >= _npcCount) return false;
	const GameInfo& in = _infos[info];
	if (in.told && !in.permanent) return false;
	if (!_vm.isPlainFunction(in.condition)) return true;
	return callAs(in.condition, npc, _hero != SCRIPT_NONE ? int(_hero) : -1) != 0;
}

// fix208: whose it is, as the scripts have it now: Gothic 1's ambient talk is shared by many, and each takes it for
// himself as the talk begins (ZS_Talk -> B_AssignAmbientInfos: Info_Vlk_1_EXIT.npc = Hlp_GetInstanceID(self)), as
// OpenGothic reads info->npc when it lists them.
u32 Game::infoNpc(u32 i) const {
	if (i >= _infoCount) return SCRIPT_NONE;
	if (_infos[i].object && _mInfoNpc != SCRIPT_NONE) _infos[i].npc = u32(_vm.getInt(_mInfoNpc, 0, _infos[i].object));
	return _infos[i].npc;
}

bool Game::hasInfos(u32 npc) const {
	if (npc >= _npcCount) return false;
	const u32 symbol = _npcs[npc].symbol;
	for (u32 i = 0; i < _infoCount; ++i)
		if (infoNpc(i) == symbol) return true;
	return false;
}

u32 Game::infosFor(u32 npc, bool important, u32* out, u32 max) {
	if (npc >= _npcCount || !max) return 0;
	const u32 symbol = _npcs[npc].symbol;
	u32 n = 0;
	for (u32 i = 0; i < _infoCount; ++i) {
		const GameInfo& in = _infos[i];
		if (infoNpc(i) != symbol || in.important != important) continue;
		// (The menu lists what has words; an important one needs none.)
		if (!important && !*infoDescription(i)) continue;
		if (!infoAvailable(i, npc)) continue;
		// In the order of NR, then of the functions (the order they were written in).
		u32 at = n < max ? n : max - 1;
		if (n >= max) {
			const GameInfo& last = _infos[out[max - 1]];
			if (in.nr > last.nr || (in.nr == last.nr && in.information >= last.information)) continue;
		} else {
			++n;
		}
		for (; at > 0; --at) {
			const GameInfo& before = _infos[out[at - 1]];
			if (before.nr < in.nr || (before.nr == in.nr && before.information <= in.information)) break;
			out[at] = out[at - 1];
		}
		out[at] = i;
	}
	return n;
}

void Game::runInfo(u32 info, u32 npc) {
	if (info >= _infoCount) return;
	_infos[info].told = true;
	++_talkVersion;
	callAs(_infos[info].information, npc, _hero != SCRIPT_NONE ? int(_hero) : -1);
}

// ---- the ways on ----

u32 Game::choicesOf(u32 info, u32* out, u32 max) const {
	u32 n = 0;
	for (u32 i = 0; i < _choiceCount && n < max; ++i)
		if (_choices[i].info == info) out[n++] = i;
	return n;
}

void Game::clearChoices(u32 info) {
	u32 to = 0;
	for (u32 i = 0; i < _choiceCount; ++i) {
		if (_choices[i].info == info) {
			free(_choices[i].text);
			continue;
		}
		_choices[to++] = _choices[i];
	}
	if (to != _choiceCount) ++_talkVersion;
	_choiceCount = to;
}

void Game::clearAllChoices() {
	for (u32 i = 0; i < _choiceCount; ++i) free(_choices[i].text);
	if (_choiceCount) ++_talkVersion;
	_choiceCount = 0;
}

void Game::xAddChoice(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const u32 fn = u32(vm.popInt());
	const char* text = vm.popString();
	const int info = g.infoIndex(u32(vm.popInt()));
	if (info < 0 || !g._vm.isPlainFunction(fn) || !grow(g._choices, g._choiceCap, g._choiceCount + 1, 32)) return;
	const size_t n = strlen(text) + 1;
	char* copy = static_cast<char*>(malloc(n));
	if (!copy) return;
	memcpy(copy, text, n);
	g._choices[g._choiceCount++] = GameChoice {u32(info), fn, copy};
	++g._choiceAdds;
	g._lastChoiceInfo = u32(info);
	++g._talkVersion;
}

void Game::xClearChoices(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const int info = g.infoIndex(u32(vm.popInt()));
	if (info >= 0) g.clearChoices(u32(info));
}

// ---- asking the infos ----

void Game::xKnowsInfo(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const int info = g.infoIndex(u32(vm.popInt()));
	(void)vm.popInstance();
	// (As the original's: a permanent info that was told still answers no.)
	vm.pushInt(info >= 0 && g._infos[info].told && !g._infos[info].permanent);
}

void Game::xCheckInfo(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const bool important = vm.popInt() != 0;
	const int npc = g.indexOf(vm.popInstance());
	u32 one[1];
	vm.pushInt(npc >= 0 && g.infosFor(u32(npc), important, one, 1) > 0);
}

// ---- what is printed ----

void Game::pushMessage(const char* text, float seconds) {
	if (!text || !*text) return;
	const u32 cap = u32(sizeof(_messages) / sizeof(_messages[0]));
	if (_messageCount == cap) { // the oldest goes
		_messageFirst = (_messageFirst + 1) % cap;
		--_messageCount;
	}
	GameMessage& m = _messages[(_messageFirst + _messageCount) % cap];
	copyText(m.text, sizeof(m.text), text);
	m.seconds = seconds;
	++_messageCount;
}

bool Game::takeMessage(GameMessage& out) {
	if (!_messageCount) return false;
	out = _messages[_messageFirst];
	_messageFirst = (_messageFirst + 1) % u32(sizeof(_messages) / sizeof(_messages[0]));
	--_messageCount;
	return true;
}

void Game::xPrint(ScriptVm& vm, void* user) { static_cast<Game*>(user)->pushMessage(vm.popString(), 3.0f); }

void Game::xPrintScreen(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	// (The scripts declare it with a number of the dialogue first, or without.)
	const u32 callee = vm.callee();
	const u32 params = callee != SCRIPT_NONE ? vm.symbol(callee).count : 5u;
	const i32 seconds = vm.popInt();
	(void)vm.popString(); // the font
	(void)vm.popInt();    // y
	(void)vm.popInt();    // x
	const char* text = vm.popString();
	char copy[96];
	copyText(copy, sizeof(copy), text);
	if (params > 5) (void)vm.popInt();
	g.pushMessage(copy, seconds > 0 ? float(seconds) : 3.0f);
}

void Game::xKnowsPlayer(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	(void)vm.popInstance();
	const int npc = g.indexOf(vm.popInstance());
	vm.pushInt(npc >= 0 && g._npcs[npc].knowsPlayer);
}

void Game::xSetKnowsPlayer(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	(void)vm.popInstance();
	const int npc = g.indexOf(vm.popInstance());
	if (npc >= 0) g._npcs[npc].knowsPlayer = true;
}

void Game::xSetTalentValue(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 value = vm.popInt();
	const i32 talent = vm.popInt();
	const int npc = g.indexOf(vm.popInstance());
	if (npc >= 0 && talent >= 0 && talent < 16) g._npcs[npc].talent[talent] = i16(value < -32768 ? -32768 : value > 32767 ? 32767 : value);
}

void Game::xGetTalentValue(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 talent = vm.popInt();
	const int npc = g.indexOf(vm.popInstance());
	vm.pushInt(npc >= 0 && talent >= 0 && talent < 16 ? g._npcs[npc].talent[talent] : 0);
}

void Game::xSetTalentSkill(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 value = vm.popInt();
	const i32 talent = vm.popInt();
	const int npc = g.indexOf(vm.popInstance());
	if (npc >= 0 && talent >= 0 && talent < 16) {
		g._npcs[npc].skill[talent] = i16(value < -32768 ? -32768 : value > 32767 ? 32767 : value);
		++g._itemsVersion; // (the sheet of the character is made anew)
		g.talentOverlays(u32(npc)); // fix202
	}
}

// fix202: a fighting talent's level is a way of moving (OpenGothic's Npc::invalidateTalentOverlays, as ZenGin's
// oCNpc::SetTalentSkill): <SKELETON>_1HST1 / _1HST2 for the one-handed weapons at level 1 / 2, _2HST*, _BOWT*, _CBOWT*,
// and _ACROBATIC. Without them every character fought as one who never learnt.
void Game::talentOverlays(u32 npc) {
	if (npc >= _npcCount) return;
	GameNpc& n = _npcs[npc];
	const char* scheme = text(n.look.script);
	if (!scheme || !*scheme) return;
	char base[48];
	copyText(base, sizeof(base), scheme);
	if (char* dot = strrchr(base, '.')) *dot = 0;
	static const struct {
		const char* talent;
		i32 fallback;
		const char* one;
		const char* two;
	} kTalents[] = {{"NPC_TALENT_1H", 1, "_1HST1", "_1HST2"},
	                {"NPC_TALENT_2H", 2, "_2HST1", "_2HST2"},
	                {"NPC_TALENT_BOW", 3, "_BOWT1", "_BOWT2"},
	                {"NPC_TALENT_CROSSBOW", 4, "_CBOWT1", "_CBOWT2"},
	                {"NPC_TALENT_ACROBAT", 11, "_ACROBATIC", nullptr}};
	for (const auto& t : kTalents) {
		const i32 index = constant(t.talent, t.fallback);
		if (index < 0 || index >= 16) continue;
		const i32 level = n.skill[index];
		char one[64], two[64];
		snprintf(one, sizeof(one), "%s%s", base, t.one);
		if (t.two) snprintf(two, sizeof(two), "%s%s", base, t.two);
		if (!t.two) {
			if (level > 0) applyOverlay(npc, one);
			else removeOverlay(npc, one);
			continue;
		}
		if (level == 1) {
			removeOverlay(npc, two);
			applyOverlay(npc, one);
		} else if (level >= 2) {
			removeOverlay(npc, one);
			applyOverlay(npc, two);
		} else {
			removeOverlay(npc, one);
			removeOverlay(npc, two);
		}
	}
}

void Game::xGetTalentSkill(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 talent = vm.popInt();
	const int npc = g.indexOf(vm.popInstance());
	vm.pushInt(npc >= 0 && talent >= 0 && talent < 16 ? g._npcs[npc].skill[talent] : 0);
}

// ---- the daily routine ----

bool Game::exchangeRoutine(u32 npc, const char* name) {
	if (npc >= _npcCount || _npcs[npc].hero || !name || !*name) return false;
	GameNpc& n = _npcs[npc];
	const i32 id = _mId != SCRIPT_NONE && n.object ? _vm.getInt(_mId, 0, n.object) : 0;
	char fnName[128];
	snprintf(fnName, sizeof(fnName), "RTN_%s_%ld", name, static_cast<long>(id));
	const u32 fn = _vm.find(fnName);
	if (fn == SCRIPT_NONE || !_vm.isPlainFunction(fn) || _vm.symbol(fn).count != 0) return false;
	const u32 before = _routineCount;
	const u32 s = _vm.selfSymbol();
	ScriptInstance* const selfBefore = _vm.instanceOf(s);
	_vm.setInstance(s, n.object);
	_vm.call(fn);
	if (_vm.symbol(fn).flags & SCRIPTF_RETURN) (void)_vm.popInt();
	_vm.setInstance(s, selfBefore);
	if (_routineCount == before) return true; // (it said nothing: the routine stays)
	// (The table of characters has not moved: no script makes characters here.)
	_npcs[npc].routineFirst = before;
	_npcs[npc].routineCount = u16(min<u32>(_routineCount - before, 0xFFFF));
	place(_npcs[npc], _hours, _find, _findUser);
	return true;
}

void Game::xExchangeRoutine(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	char name[64];
	copyText(name, sizeof(name), vm.popString());
	const int npc = g.indexOf(vm.popInstance());
	if (npc >= 0) g.exchangeRoutine(u32(npc), name);
}

// ---- chapters ----

// IntroduceChapter(title, subtitle, texture, sound, delay): the card that
// opens a chapter. The application shows it (chapter().serial changes).
void Game::xIntroduceChapter(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	GameChapter c;
	c.delay = vm.popInt();
	copyText(c.sound, sizeof(c.sound), vm.popString());
	char texture[40];
	copyText(texture, sizeof(texture), vm.popString());
	char subtitle[64];
	copyText(subtitle, sizeof(subtitle), vm.popString());
	char title[48];
	copyText(title, sizeof(title), vm.popString());
	copyText(c.texture, sizeof(c.texture), texture);
	copyText(c.subtitle, sizeof(c.subtitle), subtitle);
	copyText(c.title, sizeof(c.title), title);
	c.serial = g._chapter.serial + 1;
	g._chapter = c;
	++g._logVersion; // (the diary names the chapter)
}

} // namespace g3

namespace g3 {

i32 Game::debugLevelUp() {
	if (_hero == SCRIPT_NONE) return 0;
	ScriptInstance* o = _npcs[_hero].object;
	if (!o) return 0;
	const u32 mExp = _vm.findMember("C_NPC", "EXP"), mNext = _vm.findMember("C_NPC", "EXP_NEXT");
	const u32 mLevel = _vm.findMember("C_NPC", "LEVEL"), mLp = _vm.findMember("C_NPC", "LP");
	if (mLevel == SCRIPT_NONE) return 0;
	const i32 before = _vm.getInt(mLevel, 0, o);
	const i32 exp = mExp != SCRIPT_NONE ? _vm.getInt(mExp, 0, o) : 0;
	const i32 next = mNext != SCRIPT_NONE ? _vm.getInt(mNext, 0, o) : 0;
	const i32 need = next > exp ? next - exp : 1;
	const u32 fn = _vm.find("B_GiveXP");
	if (fn != SCRIPT_NONE && _vm.isPlainFunction(fn)) {
		_vm.pushInt(need);
		_vm.call(fn);
	}
	i32 level = _vm.getInt(mLevel, 0, o);
	if (level <= before) { // the scripts have no such function (or it did not level): by hand
		level = before + 1;
		_vm.setInt(mLevel, 0, o, level);
		if (mExp != SCRIPT_NONE) _vm.setInt(mExp, 0, o, next > exp ? next : exp);
		if (mNext != SCRIPT_NONE) _vm.setInt(mNext, 0, o, (next > 0 ? next : 500) + 500 * (level + 1));
		if (mLp != SCRIPT_NONE) _vm.setInt(mLp, 0, o, _vm.getInt(mLp, 0, o) + 10);
	}
	++_itemsVersion;
	return level;
}

int Game::debugSetChapter(i32 chapter) {
	if (_kapitel == SCRIPT_NONE || _hero == SCRIPT_NONE) return 0;
	const u32 fn = _vm.find("B_KAPITELWECHSEL");
	if (fn != SCRIPT_NONE && _vm.isPlainFunction(fn) && _vm.symbol(fn).count <= 2) {
		_vm.pushInt(chapter);
		if (_vm.symbol(fn).count == 2) _vm.pushInt(0); // (the second: the world, in the later games)
		_vm.call(fn);
		if (_vm.getInt(_kapitel) != chapter) _vm.setInt(_kapitel, 0, nullptr, chapter);
		++_logVersion;
		return 1;
	}
	_vm.setInt(_kapitel, 0, nullptr, chapter);
	++_logVersion;
	return 2;
}

u32 Game::debugLearnAll() {
	if (_hero == SCRIPT_NONE) return 0;
	GameNpc& h = _npcs[_hero];
	u32 done = 0;
	const i32 mage = constant("NPC_TALENT_MAGE", 7);
	for (i32 t = 1; t < 16; ++t) {
		h.skill[t] = i16(t == mage ? 6 : 2);
		if (isGothic2()) {
			// (Gothic 2: the fighting talents, the lock and the pockets are percentages of success)
			h.talent[t] = h.talent[t] < 100 ? 100 : h.talent[t];
		} else if (t == constant("NPC_TALENT_PICKLOCK", 5) || t == constant("NPC_TALENT_PICKPOCKET", 6)) {
			// (Gothic 1: the value is the chance of failure, so a master never fails; the weapons' critical
			// hit is left as the scripts have it)
			h.talent[t] = 0;
		}
		++done;
	}
	static const char* const kVars[] = {
	    "PLAYER_TALENT_SMITH",          "PLAYER_TALENT_ALCHEMY",   "PLAYER_TALENT_RUNES",      "PLAYER_TALENT_FOREST",
	    "PLAYER_TALENT_TAKEANIMALTROPHY", "PLAYER_TALENT_FIREMASTER", "PLAYER_TALENT_PICKPOCKET", "PLAYER_TALENT_PICKLOCK",
	    "PLAYER_TALENT_REGENERATE",     "PLAYER_TALENT_WISPDETECTOR", "PLAYER_TALENT_ACROBAT",   "PLAYER_TALENT_SNEAK",
	    "PLAYER_TALENT_FOOD",           "KNOWS_GETBLOODFLY",        "KNOWS_GETFUR",             "KNOWS_GETTEETH",
	    "KNOWS_GETCLAWS",               "KNOWS_GETHIDE",            "KNOWS_GETMANDIBLES",       "KNOWS_GETSTING",
	    "KNOWS_GETWING",                "KNOWS_GETHEART",           "KNOWS_GETSHADOWHORN",      "KNOWS_GETBFSTING",
	    "KNOWS_GETBFWING",              "KNOWS_GETDRGSNAPPERHORN",  "KNOWS_GETFIRETONGUE",      "KNOWS_GETREPTILESKIN",
	    "KNOWS_GETWARANFIRETONGUE",     "KNOWS_GETSTONEGOLEMHEART", "KNOWS_GETFIREGOLEMHEART",  "KNOWS_GETICEGOLEMHEART",
	    "KNOWS_GETSWAMPGOLEMHEART",     "KNOWS_GETDEMONHEART",      "KNOWS_GETTROPHYFUR"};
	for (const char* name : kVars) {
		const u32 sym = _vm.find(name);
		if (sym == SCRIPT_NONE || _vm.symbol(sym).type != SCRIPT_INT) continue;
		const u32 count = _vm.symbol(sym).count ? _vm.symbol(sym).count : 1u;
		for (u32 k = 0; k < count; ++k) _vm.setInt(sym, k, nullptr, 1);
		++done;
	}
	++_itemsVersion;
	return done;
}

} // namespace g3
