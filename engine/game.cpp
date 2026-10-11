#include "game.h"

#include "fight.h"

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
	freePending();
	_chapter = GameChapter();
	_kapitel = SCRIPT_NONE;
	_vm.release();
	memset(_itemObjects, 0, sizeof(_itemObjects)); // (the machine's, gone with it)
	free(_npcs);
	free(_routines);
	for (u32 i = 0; i < _textCount; ++i) free(_texts[i]);
	free(_texts);
	free(_textHash);
	free(_armors);
	free(_worlds);
	free(_guildAtt);
	free(_items);
	free(_drops);
	_drops = nullptr;
	for (u32 i = 0; i < _logTopicCount; ++i) free(_logTopics[i].name);
	for (u32 i = 0; i < _logEntryCount; ++i) free(_logEntries[i].text);
	free(_logTopics);
	free(_logEntries);
	_logTopics = nullptr;
	_logEntries = nullptr;
	_logTopicCount = _logTopicCap = _logEntryCount = _logEntryCap = 0;
	++_logVersion;
	_dropCount = _dropCap = 0;
	free(_itemInfos);
	_itemInfos = nullptr;
	free(_infos);
	_infos = nullptr;
	_infoCount = 0;
	for (u32 i = 0; i < _choiceCount; ++i) free(_choices[i].text);
	free(_choices);
	_choices = nullptr;
	_choiceCount = _choiceCap = 0;
	_messageFirst = _messageCount = 0;
	++_talkVersion;
	_itemInfoCount = _itemInfoCap = 0;
	_gilValues = nullptr; // (the machine's, gone with it)
	_gilTried = false;
	_guildAtt = nullptr;
	_items = nullptr;
	_guildCount = _itemCount = _itemCap = 0;
	++_itemsVersion;
	for (float& r : _percRange) r = -1.0f;
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
	_mAttribute = _vm.findMember("C_NPC", "ATTRIBUTE");
	// (Each must be what the engine takes it for, whatever the scripts declare.)
	const auto must = [&](u32& member, u8 type) {
		if (member != SCRIPT_NONE && _vm.symbol(member).type != type) member = SCRIPT_NONE;
	};
	_mProtection = _vm.findMember("C_NPC", "PROTECTION");
	_mDamage = _vm.findMember("C_NPC", "DAMAGE");
	_mDamageType = _vm.findMember("C_NPC", "DAMAGETYPE");
	_iFlags = _vm.findMember("C_ITEM", "FLAGS");
	_iDamageType = _vm.findMember("C_ITEM", "DAMAGETYPE");
	_iDamageTotal = _vm.findMember("C_ITEM", "DAMAGETOTAL");
	_iDamage = _vm.findMember("C_ITEM", "DAMAGE");
	_iRange = _vm.findMember("C_ITEM", "RANGE");
	_iVisual = _vm.findMember("C_ITEM", "VISUAL");
	_iProtection = _vm.findMember("C_ITEM", "PROTECTION");
	_iName = _vm.findMember("C_ITEM", "NAME");
	_iDescription = _vm.findMember("C_ITEM", "DESCRIPTION");
	_iScheme = _vm.findMember("C_ITEM", "SCEMENAME");
	_iText = _vm.findMember("C_ITEM", "TEXT");
	_iCount = _vm.findMember("C_ITEM", "COUNT");
	_iOnState = _vm.findMember("C_ITEM", "ON_STATE");
	_iOnEquip = _vm.findMember("C_ITEM", "ON_EQUIP");
	_iOnUnequip = _vm.findMember("C_ITEM", "ON_UNEQUIP");
	_iCondAtr = _vm.findMember("C_ITEM", "COND_ATR");
	_iCondValue = _vm.findMember("C_ITEM", "COND_VALUE");
	_iValue = _vm.findMember("C_ITEM", "VALUE");
	_itemVar = _vm.find("ITEM");
	if (_itemVar != SCRIPT_NONE && _vm.symbol(_itemVar).type != SCRIPT_INSTANCE) _itemVar = SCRIPT_NONE;
	must(_iDescription, SCRIPT_STRING);
	must(_iScheme, SCRIPT_STRING);
	must(_iText, SCRIPT_STRING);
	must(_iCount, SCRIPT_INT);
	must(_iOnState, SCRIPT_FUNC);
	must(_iOnEquip, SCRIPT_FUNC);
	must(_iOnUnequip, SCRIPT_FUNC);
	must(_iCondAtr, SCRIPT_INT);
	must(_iCondValue, SCRIPT_INT);
	must(_iValue, SCRIPT_INT);
	must(_mProtection, SCRIPT_INT);
	must(_mDamage, SCRIPT_INT);
	must(_mDamageType, SCRIPT_INT);
	must(_mMainFlag, SCRIPT_INT);
	must(_iFlags, SCRIPT_INT);
	must(_iDamageType, SCRIPT_INT);
	must(_iDamageTotal, SCRIPT_INT);
	must(_iDamage, SCRIPT_INT);
	must(_iRange, SCRIPT_INT);
	must(_iProtection, SCRIPT_INT);
	must(_mWp, SCRIPT_STRING);
	must(_mSensesRange, SCRIPT_INT);
	must(_mGuild, SCRIPT_INT);
	must(_mAttribute, SCRIPT_INT);
	for (float& r : _percRange) r = -1.0f;
	{
		// The table of what the guilds think of one another: GIL_MAX of them,
		// all hostile until the scripts say otherwise (as in the original).
		const u32 max = _vm.find("GIL_MAX");
		const i32 count = max != SCRIPT_NONE && _vm.symbol(max).type == SCRIPT_INT ? _vm.getInt(max) : 0;
		_guildCount = count > 0 && count <= 256 ? u32(count) : 64u;
		_guildAtt = static_cast<i32*>(calloc(size_t(_guildCount) * _guildCount, sizeof(i32))); // (ATT_HOSTILE is 0)
		if (!_guildAtt) _guildCount = 0;
		// Where the people's guilds end and the creatures' begin.
		const u32 sep = _vm.find("GIL_SEPERATOR_HUM");
		_humanGuilds = sep != SCRIPT_NONE && _vm.symbol(sep).type == SCRIPT_INT ? _vm.getInt(sep) : 0;
	}
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
	             {"NPC_ISINCUTSCENE", xIsInCutscene},
	             {"WLD_SETTIME", xSetTime},
	             {"WLD_ISTIME", xIsTime},
	             {"WLD_SETGUILDATTITUDE", xSetGuildAttitude},
	             {"WLD_GETGUILDATTITUDE", xGetGuildAttitude},
	             {"WLD_EXCHANGEGUILDATTITUDES", xExchangeGuildAttitudes},
	             {"CREATEINVITEM", xCreateInvItem},
	             {"LOG_CREATETOPIC", xLogCreateTopic},
	             {"LOG_SETTOPICSTATUS", xLogSetTopicStatus},
	             {"LOG_ADDENTRY", xLogAddEntry},
	             {"CREATEINVITEMS", xCreateInvItems},
	             {"NPC_REMOVEINVITEM", xRemoveInvItem},
	             {"NPC_REMOVEINVITEMS", xRemoveInvItems},
	             {"NPC_HASITEMS", xHasItems},
	             {"NPC_GETATTITUDE", xGetAttitude},
	             {"NPC_GETPERMATTITUDE", xGetPermAttitude},
	             {"NPC_SETATTITUDE", xSetAttitude},
	             {"NPC_SETTEMPATTITUDE", xSetTempAttitude},
	             {"NPC_ISDEAD", xIsDead},
	             {"NPC_SETTARGET", xSetTarget},
	             {"NPC_GETTARGET", xGetTarget},
	             {"PERC_SETRANGE", xSetPercRange},
	             {"NPC_HASEQUIPPEDMELEEWEAPON", xHasEquippedMelee},
	             {"NPC_HASEQUIPPEDRANGEDWEAPON", xHasEquippedRanged},
	             {"NPC_HASEQUIPPEDWEAPON", xHasEquippedWeapon},
	             {"NPC_ISINFIGHTMODE", xIsInFightMode},
	             {"NPC_HASREADIEDWEAPON", xHasReadiedWeapon},
	             {"NPC_HASREADIEDMELEEWEAPON", xHasReadiedMelee},
	             {"NPC_HASREADIEDRANGEDWEAPON", xHasReadiedRanged},
	             {"NPC_CHANGEATTRIBUTE", xChangeAttribute},
	             {"NPC_SETTOFISTMODE", xSetToFistMode},
	             {"NPC_SETTOFIGHTMODE", xSetToFightMode},
	             {"NPC_GETACTIVESPELL", xReturnMinusOne},
	             {"NPC_GETTRUEGUILD", xGetTrueGuild},
	             // Lines for the developers' log: there is nobody to read them.
	             {"PRINTDEBUG", xNothing},
	             {"PRINTDEBUGINST", xNothing},
	             {"PRINTDEBUGINSTCH", xNothing},
	             {"PRINTDEBUGCH", xNothing}};
	for (const auto& e : table) _vm.bind(e.name, e.fn, this);
	bindDialogues();
	bindItems();

	// The player's character exists before any world's start-up runs.
	const u32 hero = _vm.find("PC_HERO");
	if (hero != SCRIPT_NONE && _vm.symbol(hero).type == SCRIPT_INSTANCE && (_vm.symbol(hero).flags & SCRIPTF_CONST) &&
	    _clsNpc != SCRIPT_NONE && _vm.classOf(hero) == _clsNpc) {
		_hero = addNpc(hero, true);
		if (_hero != SCRIPT_NONE && _vm.heroSymbol() != SCRIPT_NONE) _vm.setInstance(_vm.heroSymbol(), _npcs[_hero].object);
	}
	// The reach of the perceptions, as the engine has the scripts set it at the start.
	const u32 init = _vm.find("INITPERCEPTIONS");
	if (init != SCRIPT_NONE && _vm.isPlainFunction(init) && _vm.symbol(init).count == 0) _vm.callInt(init);
	return true;
}

// ---- what the characters think of one another, what they carry ----

i32 Game::guildAttitude(i32 g1, i32 g2) const {
	// A guild the table does not know says nothing: neutral, never "hostile" (that
	// made inhabitants fall on whoever was of a guild past the table's end).
	if (g1 < 0 || g2 < 0 || u32(g1) >= _guildCount || u32(g2) >= _guildCount || !_guildAtt) return constant("ATT_NEUTRAL", 2);
	return _guildAtt[u32(g1) * _guildCount + u32(g2)];
}

i32 Game::attitude(u32 a, u32 b) const {
	if (a >= _npcCount || b >= _npcCount) return constant("ATT_NEUTRAL", 2);
	const GameNpc& na = _npcs[a];
	const GameNpc& nb = _npcs[b];
	if (na.hero || nb.hero) {
		// Towards the player (or the player towards one): the character's own mood first.
		const GameNpc& n = na.hero ? nb : na;
		if (n.tempAttitude >= 0) return n.tempAttitude;
		if (n.attitude >= 0) return n.attitude;
	}
	return guildAttitude(guildOf(na), guildOf(nb));
}

i32 Game::itemCount(u32 npc, u32 symbol) const {
	for (u32 i = 0; i < _itemCount; ++i)
		if (_items[i].npc == npc && _items[i].symbol == symbol) return _items[i].count;
	return 0;
}

void Game::addItems(ScriptInstance* inst, i32 symbol, i32 count) {
	const GameNpc* n = npcOf(inst);
	if (!n || symbol < 0 || u32(symbol) >= _vm.symbolCount() || count == 0) return;
	const u32 npc = u32(n - _npcs);
	for (u32 i = 0; i < _itemCount; ++i)
		if (_items[i].npc == npc && _items[i].symbol == u32(symbol)) {
			const long long total = (long long)_items[i].count + count;
			_items[i].count = i32(total < 0 ? 0 : total > (1 << 30) ? (1 << 30) : total);
			++_itemsVersion;
			return;
		}
	if (count < 0 || !grow(_items, _itemCap, _itemCount + 1, 256)) return;
	_items[_itemCount++] = GameItem {npc, u32(symbol), count};
	++_itemsVersion;
}

// ---- the diary ----

namespace {
char* copyOf(const char* s) {
	const size_t n = strlen(s ? s : "") + 1;
	char* c = static_cast<char*>(malloc(n));
	if (c) memcpy(c, s ? s : "", n);
	return c;
}
} // namespace

int Game::logTopicOf(const char* name, bool make) {
	if (!name || !*name) return -1;
	for (u32 i = 0; i < _logTopicCount; ++i)
		if (strcmp(_logTopics[i].name, name) == 0) return int(i);
	if (!make || !grow(_logTopics, _logTopicCap, _logTopicCount + 1, 32)) return -1;
	char* copy = copyOf(name);
	if (!copy) return -1;
	_logTopics[_logTopicCount] = GameLogTopic {copy, 0, 0, _logVersion};
	return int(_logTopicCount++);
}

// A line of news for the player, unless the last one is about the same topic
// and not shown yet (a task begun with its first entry says it once).
void Game::notice(u8 kind, u8 section, u8 status, const char* topic) {
	const u32 cap = u32(sizeof(_notices) / sizeof(_notices[0]));
	if (_noticeCount) {
		const GameLogNotice& last = _notices[(_noticeFirst + _noticeCount - 1) % cap];
		if (strcmp(last.topic, topic) == 0 && (kind == LOGN_ENTRY || last.kind == kind)) return;
	}
	if (_noticeCount == cap) {
		_noticeFirst = (_noticeFirst + 1) % cap;
		--_noticeCount;
	}
	GameLogNotice& n = _notices[(_noticeFirst + _noticeCount++) % cap];
	n.kind = kind;
	n.section = section;
	n.status = status;
	copyText(n.topic, sizeof(n.topic), topic);
}

bool Game::takeLogNotice(GameLogNotice& out) {
	if (!_noticeCount) return false;
	out = _notices[_noticeFirst];
	_noticeFirst = (_noticeFirst + 1) % u32(sizeof(_notices) / sizeof(_notices[0]));
	--_noticeCount;
	return true;
}

void Game::xLogCreateTopic(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 section = vm.popInt();
	const char* name = vm.popString();
	const u32 before = g._logTopicCount;
	const int t = g.logTopicOf(name, true);
	if (t < 0) return;
	g._logTopics[t].section = u8(section);
	g._logTopics[t].order = ++g._logVersion;
	if (g._logTopicCount != before) g.notice(LOGN_NEW_TOPIC, u8(section), 0, g._logTopics[t].name);
}

void Game::xLogSetTopicStatus(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 status = vm.popInt();
	const int t = g.logTopicOf(vm.popString(), true);
	if (t < 0) return;
	const u8 was = g._logTopics[t].status;
	g._logTopics[t].status = u8(status);
	g._logTopics[t].order = ++g._logVersion;
	if (was != u8(status) && (status == 2 || status == 3)) g.notice(LOGN_STATUS, g._logTopics[t].section, u8(status), g._logTopics[t].name);
}

void Game::xLogAddEntry(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const char* text = vm.popString();
	const int t = g.logTopicOf(vm.popString(), true);
	if (t < 0 || !grow(g._logEntries, g._logEntryCap, g._logEntryCount + 1, 64)) return;
	char* copy = copyOf(text);
	if (!copy) return;
	g._logEntries[g._logEntryCount++] = GameLogEntry {u32(t), copy};
	g._logTopics[t].order = ++g._logVersion;
	g.notice(LOGN_ENTRY, g._logTopics[t].section, 0, g._logTopics[t].name);
}

void Game::giveItems(u32 npc, u32 symbol, i32 count) {
	if (npc < _npcCount && _npcs[npc].object && isItem(symbol)) addItems(_npcs[npc].object, i32(symbol), count);
}

bool Game::isItem(u32 symbol) const {
	if (symbol >= _vm.symbolCount() || _clsItem == SCRIPT_NONE) return false;
	const ScriptSymbol& s = _vm.symbol(symbol);
	return s.type == SCRIPT_INSTANCE && (s.flags & SCRIPTF_CONST) && _vm.classOf(symbol) == _clsItem;
}

bool Game::callItemFunction(u32 npc, u32 symbol, u32 fn) {
	if (npc >= _npcCount || !_npcs[npc].object || !_vm.isPlainFunction(fn) || _vm.symbol(fn).count != 0 || !isItem(symbol)) return false;
	ScriptInstance* item = _vm.create(symbol);
	if (!item) return false;
	const u32 self = _vm.selfSymbol();
	ScriptInstance* const selfBefore = self != SCRIPT_NONE ? _vm.instanceOf(self) : nullptr;
	ScriptInstance* const itemBefore = _itemVar != SCRIPT_NONE ? _vm.instanceOf(_itemVar) : nullptr;
	if (self != SCRIPT_NONE) _vm.setInstance(self, _npcs[npc].object);
	if (_itemVar != SCRIPT_NONE) _vm.setInstance(_itemVar, item);
	(void)_vm.callInt(fn);
	if (self != SCRIPT_NONE) _vm.setInstance(self, selfBefore);
	if (_itemVar != SCRIPT_NONE) _vm.setInstance(_itemVar, itemBefore);
	_vm.destroy(item);
	return true;
}

void Game::xSetGuildAttitude(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 g2 = vm.popInt(), att = vm.popInt(), g1 = vm.popInt();
	if (g1 >= 0 && g2 >= 0 && u32(g1) < g._guildCount && u32(g2) < g._guildCount) g._guildAtt[u32(g1) * g._guildCount + u32(g2)] = att;
}

void Game::xGetGuildAttitude(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 g2 = vm.popInt(), g1 = vm.popInt();
	vm.pushInt(g.guildAttitude(g1, g2));
}

// The table of the scripts' constant of that name: TAB_ANZAHL numbers, a
// square of the first guilds.
void Game::xExchangeGuildAttitudes(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const u32 table = vm.find(vm.popString());
	if (table == SCRIPT_NONE || vm.symbol(table).type != SCRIPT_INT) return;
	// The table is a square of GIL_MAX guilds a side; its own width is the stride,
	// whatever part of it fits ours.
	u32 side = 0;
	while ((side + 1) * (side + 1) <= vm.symbol(table).count) ++side;
	const u32 use = min(side, g._guildCount);
	for (u32 i = 0; i < use; ++i)
		for (u32 k = 0; k < use; ++k) g._guildAtt[i * g._guildCount + k] = vm.getInt(table, i * side + k);
}

void Game::xCreateInvItem(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 item = vm.popInt();
	g.addItems(vm.popInstance(), item, 1);
}

void Game::xCreateInvItems(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 count = vm.popInt(), item = vm.popInt();
	if (count > 0) g.addItems(vm.popInstance(), item, count);
	else (void)vm.popInstance();
}

void Game::xRemoveInvItem(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 item = vm.popInt();
	g.addItems(vm.popInstance(), item, -1);
}

void Game::xRemoveInvItems(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 count = vm.popInt(), item = vm.popInt();
	if (count > 0) g.addItems(vm.popInstance(), item, -count);
	else (void)vm.popInstance();
}

void Game::xHasItems(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 item = vm.popInt();
	const int who = g.indexOf(vm.popInstance());
	vm.pushInt(who >= 0 && item >= 0 ? g.itemCount(u32(who), u32(item)) : 0);
}

void Game::xGetAttitude(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const int b = g.indexOf(vm.popInstance()), a = g.indexOf(vm.popInstance());
	// (Nobody there - OTHER not set, say - is not an enemy.)
	vm.pushInt(a >= 0 && b >= 0 ? g.attitude(u32(a), u32(b)) : g.constant("ATT_NEUTRAL", 2));
}

void Game::xGetPermAttitude(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const int b = g.indexOf(vm.popInstance()), a = g.indexOf(vm.popInstance());
	if (a < 0 || b < 0) {
		vm.pushInt(g.constant("ATT_NEUTRAL", 2));
		return;
	}
	const GameNpc& na = g._npcs[a];
	const GameNpc& nb = g._npcs[b];
	const GameNpc& n = na.hero ? nb : na;
	vm.pushInt((na.hero || nb.hero) && n.attitude >= 0 ? n.attitude : g.guildAttitude(g.guildOf(na), g.guildOf(nb)));
}

void Game::xSetAttitude(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 att = vm.popInt();
	if (GameNpc* n = g.npcOf(vm.popInstance())) n->attitude = i8(clamp<i32>(att, -1, 3));
}

void Game::xSetTempAttitude(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 att = vm.popInt();
	if (GameNpc* n = g.npcOf(vm.popInstance())) n->tempAttitude = i8(clamp<i32>(att, -1, 3));
}

void Game::xIsDead(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const GameNpc* n = g.npcOf(vm.popInstance());
	vm.pushInt(n && (n->down == DOWN_DEAD || g.hitPoints(*n) <= 0));
}

void Game::xSetTarget(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const int target = g.indexOf(vm.popInstance());
	if (GameNpc* n = g.npcOf(vm.popInstance())) n->target = target;
}

// True if the character has a target; OTHER is it from now on.
void Game::xGetTarget(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const GameNpc* n = g.npcOf(vm.popInstance());
	const bool has = n && n->target >= 0 && u32(n->target) < g._npcCount;
	if (has) vm.setInstance(vm.otherSymbol(), g._npcs[n->target].object);
	vm.pushInt(has);
}

void Game::xHasEquippedMelee(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const GameNpc* n = g.npcOf(vm.popInstance());
	vm.pushInt(n && n->melee != SCRIPT_NONE);
}

void Game::xHasEquippedRanged(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const GameNpc* n = g.npcOf(vm.popInstance());
	vm.pushInt(n && n->ranged != SCRIPT_NONE);
}

void Game::xHasEquippedWeapon(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const GameNpc* n = g.npcOf(vm.popInstance());
	vm.pushInt(n && (n->melee != SCRIPT_NONE || n->ranged != SCRIPT_NONE));
}

// Npc_IsInFightMode(npc, FMODE_...): the scripts' numbers, read by name.
void Game::xIsInFightMode(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 mode = vm.popInt();
	const GameNpc* n = g.npcOf(vm.popInstance());
	if (!n) {
		vm.pushInt(0);
		return;
	}
	bool in = false;
	if (mode == g.constant("FMODE_NONE", 0)) in = n->weapon == WEAPON_NONE;
	else if (mode == g.constant("FMODE_FIST", 1)) in = n->weapon == WEAPON_FIST;
	else if (mode == g.constant("FMODE_MELEE", 2)) in = n->weapon == WEAPON_1H || n->weapon == WEAPON_2H;
	vm.pushInt(in); // (ranged weapons and magic: not yet)
}

void Game::xHasReadiedWeapon(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const GameNpc* n = g.npcOf(vm.popInstance());
	vm.pushInt(n && n->weapon != WEAPON_NONE && n->weapon != WEAPON_FIST);
}

void Game::xHasReadiedMelee(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const GameNpc* n = g.npcOf(vm.popInstance());
	vm.pushInt(n && (n->weapon == WEAPON_1H || n->weapon == WEAPON_2H));
}

void Game::xHasReadiedRanged(ScriptVm& vm, void* user) {
	(void)vm.popInstance();
	(void)user;
	vm.pushInt(0);
}

void Game::xChangeAttribute(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 value = vm.popInt(), index = vm.popInt();
	GameNpc* n = g.npcOf(vm.popInstance());
	if (!n) return;
	i32 v = g.attributeOf(*n, index) + value;
	if (index == 0) {
		// Hit points stay within 0 and their maximum.
		v = clamp(v, i32(0), max(g.attributeOf(*n, 1), i32(1)));
	} else if (index == 2) {
		// So does mana (ATR_MANA, ATR_MANA_MAX).
		v = clamp(v, i32(0), max(g.attributeOf(*n, 3), i32(0)));
	}
	g.setAttribute(*n, index, v);
}

void Game::xSetToFistMode(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	if (GameNpc* n = g.npcOf(vm.popInstance())) n->weapon = WEAPON_FIST;
}

// Npc_SetToFightMode(npc, weapon): the weapon in hand at once.
void Game::xSetToFightMode(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 item = vm.popInt();
	GameNpc* n = g.npcOf(vm.popInstance());
	if (!n) return;
	if (item >= 0) g.equip(u32(n - g._npcs), u32(item));
	const GameItemInfo* it = n->melee != SCRIPT_NONE ? g.itemInfo(n->melee) : nullptr;
	const i32 twoHanded = g.constant("ITEM_2HD_SWD", 1 << 16) | g.constant("ITEM_2HD_AXE", 1 << 17);
	n->weapon = it ? ((it->flags & twoHanded) ? WEAPON_2H : WEAPON_1H) : WEAPON_FIST;
}

void Game::xReturnMinusOne(ScriptVm& vm, void*) {
	(void)vm.popInstance();
	vm.pushInt(-1);
}

void Game::xGetTrueGuild(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const GameNpc* n = g.npcOf(vm.popInstance());
	vm.pushInt(n ? g.guildOf(*n) : 0);
}

void Game::xSetPercRange(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 range = vm.popInt(), perc = vm.popInt();
	if (perc >= 0 && perc < PERC_TYPES) g._percRange[perc] = float(clamp<i32>(range, 0, 100000));
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
	    "NPC_SETPERCTIME",   "WLD_DETECTNPC",      "NPC_CANSEENPC",     "AI_USEMOB",         "WLD_ISMOBAVAILABLE", "WLD_GETMOBSTATE",
	    "NPC_GETDETECTEDMOB",
	    // Perception (npc_perc.cpp).
	    "NPC_PERCENABLE",    "NPC_PERCDISABLE",    "NPC_PERCEIVEALL",   "NPC_SENDPASSIVEPERC", "AI_LOOKATNPC",   "AI_STOPLOOKAT",
	    "AI_OUTPUTSVM",      "AI_OUTPUTSVM_OVERLAY", "NPC_CANSEENPCFREELOS", "AI_QUICKLOOK",
	    // Fighting (npc_fight.cpp).
	    "AI_ATTACK",         "AI_READYMELEEWEAPON", "AI_READYRANGEDWEAPON", "AI_DRAWWEAPON",   "AI_REMOVEWEAPON",
	    "AI_EQUIPBESTMELEEWEAPON", "AI_DODGE",     "AI_FLEE",           "AI_FINISHINGMOVE",  "AI_WHIRLAROUND",   "AI_TURNAWAY",
	    "NPC_GETNEXTTARGET", "NPC_ISNEXTTARGETAVAILABLE", "AI_POINTATNPC", "AI_STOPPOINTAT", "AI_EQUIPBESTRANGEDWEAPON",
	    "AI_STOPAIM",
	    // Talks (npc_dialog.cpp).
	    "AI_OUTPUT",         "AI_PROCESSINFOS",    "AI_STOPPROCESSINFOS", "INFOMANAGER_HASFINISHED",
	    // Things in the hands and in the world (npc_items.cpp).
	    "AI_USEITEM",        "AI_USEITEMTOSTATE",  "AI_TAKEITEM",       "AI_GOTOITEM",       "WLD_DETECTITEM",   "NPC_CANSEEITEM",
	    "NPC_GETDISTTOITEM"};
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
	applyPending(index);
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
	if (find) {
		_find = find;
		_findUser = user;
	}
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
	if (n.weapon != WEAPON_FIST) n.weapon = WEAPON_NONE; // (a creature's teeth stay where they are)
	n.target = -1;
	// The dead stay where they fell; the knocked out come to (out of sight).
	if (n.down == DOWN_DEAD) return;
	if (n.down == DOWN_UNCONSCIOUS) {
		n.down = DOWN_NONE;
		if (hitPoints(n) < 1) setAttribute(n, 0, 1);
	}
	placeNow(n, find, user);
}

void Game::place(GameNpc& n, float hours, PointFinder find, void* user) {
	const GameRoutine* r = routineAt(n, hours);
	const GameText at = r ? r->point : n.spawn;
	n.state = r ? r->state : 0u;
	if (n.down == DOWN_DEAD && n.placed) return;
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
	if (find) {
		_find = find;
		_findUser = user;
	}
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
	Game& g = *static_cast<Game*>(user);
	const GameText point = g.intern(vm.popString(), false);
	const i32 item = vm.popInt();
	++g._stats.items;
	if (item < 0 || !g.isItem(u32(item)) || !grow(g._drops, g._dropCap, g._dropCount + 1, 64)) return;
	g._drops[g._dropCount++] = GameDrop {g._world, u32(item), point};
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
	// (The armour is put on, too: it protects.)
	if (worn && worn->wearable) g.wear(*n, u32(armor));
}

// Puts an armour on: its protection is added to the character's (and the
// old one's taken off), as the original's equipping does.
void Game::wear(GameNpc& n, u32 armor) {
	if (n.armorItem == armor || _mProtection == SCRIPT_NONE || !n.object) return;
	const auto apply = [&](u32 item, i32 sign) {
		const GameItemInfo* it = item != SCRIPT_NONE ? itemInfo(item) : nullptr;
		if (!it) return;
		for (u32 k = 0; k < 8 && k < _vm.symbol(_mProtection).count; ++k)
			_vm.setInt(_mProtection, k, n.object, _vm.getInt(_mProtection, k, n.object) + sign * it->protection[k]);
	};
	apply(n.armorItem, -1);
	apply(armor, 1);
	n.armorItem = armor;
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
	// A weapon: in its place on the belt or the back, taken in hand in a fight.
	const GameItemInfo* it = item >= 0 ? g.itemInfo(u32(item)) : nullptr;
	GameNpc* n = g.npcOf(inst);
	if (!n) return;
	if (it && (it->mainflag & (g.constant("ITEM_KAT_NF", 1 << 1) | g.constant("ITEM_KAT_FF", 1 << 2)))) {
		g.equip(u32(n - g._npcs), u32(item));
		return;
	}
	if (!worn || !worn->wearable) return;
	n->look.armor = worn->mesh;
	g.wear(*n, u32(item));
}

void Game::xGetNpc(ScriptVm& vm, void*) {
	const u32 symbol = u32(vm.popInt());
	vm.pushInstance(vm.instanceOf(symbol));
}

void Game::xGetInstanceId(ScriptVm& vm, void*) {
	const ScriptInstance* inst = vm.popInstance();
	vm.pushInt(inst ? i32(inst->symbol) : -1);
}

// No cutscenes here: nobody is in one.
void Game::xIsInCutscene(ScriptVm& vm, void*) {
	(void)vm.popInstance();
	vm.pushInt(0);
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

// (What it was given goes with its frame.)
void Game::xNothing(ScriptVm&, void*) {}

void Game::xIsTime(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 m2 = vm.popInt(), h2 = vm.popInt(), m1 = vm.popInt(), h1 = vm.popInt();
	const i32 from = minutesOf(h1, m1), to = minutesOf(h2, m2);
	const i32 now = minutesOf(0, i32(g._hours * 60.0f));
	vm.pushInt(from < to ? (now >= from && now < to) : (now >= from || now < to));
}

} // namespace g3
