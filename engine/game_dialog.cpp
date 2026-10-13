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

bool Game::hasInfos(u32 npc) const {
	if (npc >= _npcCount) return false;
	const u32 symbol = _npcs[npc].symbol;
	for (u32 i = 0; i < _infoCount; ++i)
		if (_infos[i].npc == symbol) return true;
	return false;
}

u32 Game::infosFor(u32 npc, bool important, u32* out, u32 max) {
	if (npc >= _npcCount || !max) return 0;
	const u32 symbol = _npcs[npc].symbol;
	u32 n = 0;
	for (u32 i = 0; i < _infoCount; ++i) {
		const GameInfo& in = _infos[i];
		if (in.npc != symbol || in.important != important) continue;
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
