// The rules of a fight (fight.h): what the game's numbers say about items,
// reach and damage. Part of Game, which owns the characters and their items.
#include "fight.h"

#include "game.h"

#include <stdlib.h>
#include <string.h>

namespace g3 {

const char* const kFightSituations[FS_COUNT] = {"FA_ENEMY_PREHIT_",  "FA_ENEMY_STORMPREHIT_", "FA_MY_W_COMBO_",   "FA_MY_W_RUNTO_",
                                                "FA_MY_W_STRAFE_",   "FA_MY_W_FOCUS_",        "FA_MY_W_NOFOCUS_", "FA_MY_G_COMBO_",
                                                "FA_MY_G_RUNTO_",    "FA_MY_G_STRAFE_",       "FA_MY_G_FOCUS_",   "FA_MY_FK_FOCUS_",
                                                "FA_MY_G_FK_NOFOCUS_"};

const char* const kFightMoveNames[FM_COUNT] = {"",
                                              "MOVE_RUN",
                                              "MOVE_RUNBACK",
                                              "MOVE_JUMPBACK",
                                              "MOVE_TURN",
                                              "MOVE_STRAFE",
                                              "MOVE_ATTACK",
                                              "MOVE_SIDEATTACK",
                                              "MOVE_FRONTATTACK",
                                              "MOVE_TRIPLEATTACK",
                                              "MOVE_WHIRLATTACK",
                                              "MOVE_MASTERATTACK",
                                              "MOVE_TURNTOHIT",
                                              "MOVE_PARADE",
                                              "MOVE_STANDUP",
                                              "MOVE_WAIT",
                                              "MOVE_WAIT_LONGER"};
const i32 kFightMoveDefaults[FM_COUNT] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 15, 17, 18, 19, 23};

const char* weaponWord(u8 mode) {
	switch (mode) {
	case WEAPON_FIST: return "FIST";
	case WEAPON_1H: return "1H";
	case WEAPON_2H: return "2H";
	default: return "";
	}
}

float blowTime(const ClipFight* f, u32 index, float fps) {
	if (!f || index >= f->hits || f->hit[index] == CLIP_NO_FRAME || fps <= 0.0f) return -1.0f;
	return float(f->hit[index]) / fps;
}

float blowEnd(const ClipFight* f, u32 index, float fps) {
	if (!f || index >= f->hitEnds || f->hitEnd[index] == CLIP_NO_FRAME || fps <= 0.0f) return -1.0f;
	return float(f->hitEnd[index]) / fps;
}

bool inComboWindow(const ClipFight* f, u32 index, float fps, float time) {
	if (!f || index >= f->windows || fps <= 0.0f) return false;
	const float from = float(f->window[index * 2]) / fps, to = float(f->window[index * 2 + 1]) / fps;
	return time > from && time <= to;
}

bool inParry(const ClipFight* f, float fps, float time) {
	if (!f || f->parry[0] == CLIP_NO_FRAME || f->parry[1] == CLIP_NO_FRAME || fps <= 0.0f) return false;
	return time >= float(f->parry[0]) / fps && time < float(f->parry[1]) / fps;
}

// ---- Game: items, attributes, the rules ----

i32 Game::attributeOf(const GameNpc& n, i32 index) const {
	if (_mAttribute == SCRIPT_NONE || !n.object || index < 0 || u32(index) >= _vm.symbol(_mAttribute).count)
		return index == 0 ? 1 : 0;
	return _vm.getInt(_mAttribute, u32(index), n.object);
}

void Game::setAttribute(GameNpc& n, i32 index, i32 value) {
	if (_mAttribute == SCRIPT_NONE || !n.object || index < 0 || u32(index) >= _vm.symbol(_mAttribute).count) return;
	_vm.setInt(_mAttribute, u32(index), n.object, value);
}

i32 Game::npcInt(const GameNpc& n, u32 member, u32 index) const {
	if (member == SCRIPT_NONE || !n.object || _vm.symbol(member).type != SCRIPT_INT || index >= _vm.symbol(member).count) return 0;
	return _vm.getInt(member, index, n.object);
}

i32 Game::constant(const char* name, i32 otherwise) const {
	const u32 sym = _vm.find(name);
	if (sym == SCRIPT_NONE || _vm.symbol(sym).type != SCRIPT_INT || (_vm.symbol(sym).flags & SCRIPTF_MEMBER)) return otherwise;
	return _vm.getInt(sym);
}

u32 Game::function(const char* name) const {
	const u32 fn = _vm.find(name);
	return fn != SCRIPT_NONE && _vm.isPlainFunction(fn) ? fn : SCRIPT_NONE;
}

u32 Game::dice(u32 bound) {
	_seed ^= _seed << 13;
	_seed ^= _seed >> 17;
	_seed ^= _seed << 5;
	return bound ? _seed % bound : 0;
}

const GameItemInfo* Game::itemInfo(u32 symbol) {
	for (u32 i = 0; i < _itemInfoCount; ++i)
		if (_itemInfos[i].symbol == symbol) return &_itemInfos[i];
	if (symbol >= _vm.symbolCount() || _clsItem == SCRIPT_NONE) return nullptr;
	const ScriptSymbol& s = _vm.symbol(symbol);
	if (s.type != SCRIPT_INSTANCE || !(s.flags & SCRIPTF_CONST) || _vm.classOf(symbol) != _clsItem) return nullptr;
	if (_itemInfoCount >= _itemInfoCap) {
		const u32 cap = _itemInfoCap ? _itemInfoCap * 2 : 32;
		GameItemInfo* grown = static_cast<GameItemInfo*>(realloc(static_cast<void*>(_itemInfos), size_t(cap) * sizeof(GameItemInfo)));
		if (!grown) return nullptr;
		_itemInfos = grown;
		_itemInfoCap = cap;
	}
	ScriptInstance* item = _vm.create(symbol);
	if (!item) return nullptr;
	GameItemInfo info;
	memset(&info, 0, sizeof(info));
	info.symbol = symbol;
	const auto get = [&](u32 member, u32 index) {
		return member != SCRIPT_NONE && _vm.symbol(member).type == SCRIPT_INT && index < _vm.symbol(member).count
		           ? _vm.getInt(member, index, item)
		           : 0;
	};
	info.mainflag = get(_mMainFlag, 0);
	info.flags = get(_iFlags, 0);
	info.damageType = get(_iDamageType, 0);
	info.damageTotal = get(_iDamageTotal, 0);
	info.range = get(_iRange, 0);
	for (u32 k = 0; k < 8; ++k) {
		info.damage[k] = get(_iDamage, k);
		info.protection[k] = get(_iProtection, k);
	}
	if (_iVisual != SCRIPT_NONE && _vm.symbol(_iVisual).type == SCRIPT_STRING) info.visual = intern(_vm.getString(_iVisual, 0, item), true);
	if (_iName != SCRIPT_NONE && _vm.symbol(_iName).type == SCRIPT_STRING) info.name = intern(_vm.getString(_iName, 0, item), false);
	// What the inventory shows and does.
	const auto text = [&](u32 member, u32 index) -> GameText {
		if (member == SCRIPT_NONE || index >= _vm.symbol(member).count) return 0;
		const char* str = _vm.getString(member, index, item);
		return str && *str ? intern(str, false) : GameText(0);
	};
	const auto func = [&](u32 member, u32 index) -> u32 {
		if (member == SCRIPT_NONE || index >= _vm.symbol(member).count) return SCRIPT_NONE;
		const u32 fn = u32(_vm.getInt(member, index, item));
		return fn != 0 && _vm.isPlainFunction(fn) ? fn : SCRIPT_NONE;
	};
	info.description = text(_iDescription, 0);
	info.scheme = text(_iScheme, 0);
	for (u32 k = 0; k < 6; ++k) {
		info.text[k] = text(_iText, k);
		info.count[k] = _iCount != SCRIPT_NONE && k < _vm.symbol(_iCount).count ? _vm.getInt(_iCount, k, item) : 0;
	}
	info.onUse = func(_iOnState, 0);
	info.onEquip = func(_iOnEquip, 0);
	info.onUnequip = func(_iOnUnequip, 0);
	for (u32 k = 0; k < 3; ++k) {
		info.condAtr[k] = _iCondAtr != SCRIPT_NONE && k < _vm.symbol(_iCondAtr).count ? _vm.getInt(_iCondAtr, k, item) : 0;
		info.condValue[k] = _iCondValue != SCRIPT_NONE && k < _vm.symbol(_iCondValue).count ? _vm.getInt(_iCondValue, k, item) : 0;
	}
	info.value = get(_iValue, 0);
	info.material = get(_vm.findMember("C_ITEM", "MATERIAL"), 0);
	_vm.destroy(item);
	_itemInfos[_itemInfoCount] = info;
	return &_itemInfos[_itemInfoCount++];
}

namespace {

bool isMelee(const Game& g, const GameItemInfo& it) { return (it.mainflag & g.constant("ITEM_KAT_NF", 1 << 1)) != 0; }

i32 meleeScore(const GameItemInfo& it) {
	i32 sum = it.damageTotal;
	for (i32 d : it.damage) sum += d;
	return sum;
}

} // namespace

u32 Game::bestMelee(u32 npc) {
	u32 best = SCRIPT_NONE;
	i32 bestScore = -1;
	for (u32 i = 0; i < _itemCount; ++i) {
		if (_items[i].npc != npc || _items[i].count <= 0) continue;
		const u32 symbol = _items[i].symbol;
		const GameItemInfo* it = itemInfo(symbol);
		if (!it || !isMelee(*this, *it)) continue;
		const i32 score = meleeScore(*it);
		if (score > bestScore) {
			bestScore = score;
			best = symbol;
		}
	}
	if (npc < _npcCount && _npcs[npc].melee != SCRIPT_NONE) {
		const GameItemInfo* it = itemInfo(_npcs[npc].melee);
		if (it && meleeScore(*it) >= bestScore) best = _npcs[npc].melee;
	}
	return best;
}

u32 Game::bestMeleeOfAll() {
	u32 best = SCRIPT_NONE;
	i32 bestScore = -1;
	for (u32 s = 0; s < _vm.symbolCount(); ++s) {
		const ScriptSymbol& sym = _vm.symbol(s);
		if (sym.type != SCRIPT_INSTANCE || !(sym.flags & SCRIPTF_CONST) || _vm.classOf(s) != _clsItem) continue;
		const GameItemInfo* it = itemInfo(s);
		if (!it || !isMelee(*this, *it) || it->visual == 0) continue;
		const i32 score = meleeScore(*it);
		if (score > bestScore) {
			bestScore = score;
			best = s;
		}
	}
	return best;
}

void Game::equip(u32 npc, u32 symbol) {
	if (npc >= _npcCount) return;
	const GameItemInfo* it = itemInfo(symbol);
	if (!it) return;
	if (it->mainflag & constant("ITEM_KAT_NF", 1 << 1)) _npcs[npc].melee = symbol;
	if (it->mainflag & constant("ITEM_KAT_FF", 1 << 2)) _npcs[npc].ranged = symbol;
	if (itemCount(npc, symbol) <= 0 && _npcs[npc].object) addItems(_npcs[npc].object, i32(symbol), 1);
}

i32 Game::guildValue(const char* member, i32 guild, i32 otherwise) {
	if (!_gilTried) {
		// The engine makes the table once, as the original does at its start.
		_gilTried = true;
		const u32 inst = _vm.find("GIL_VALUES");
		if (inst != SCRIPT_NONE && _vm.symbol(inst).type == SCRIPT_INSTANCE && (_vm.symbol(inst).flags & SCRIPTF_CONST))
			_gilValues = _vm.create(inst);
	}
	if (!_gilValues || guild < 0) return otherwise;
	// The table has one column for all people ("GIL_HUMAN"), as the original
	// copies it to every human guild.
	if (guild < constant("GIL_SEPERATOR_HUM", 16)) guild = constant("GIL_HUMAN", 1);
	const u32 m = _vm.findMember("C_GILVALUES", member);
	if (m == SCRIPT_NONE || _vm.symbol(m).type != SCRIPT_INT || u32(guild) >= _vm.symbol(m).count) return otherwise;
	const i32 v = _vm.getInt(m, u32(guild), _gilValues);
	return v ? v : otherwise;
}

float Game::meleeRange(u32 a, u32 b) {
	if (a >= _npcCount || b >= _npcCount) return 0.0f;
	const GameNpc& na = _npcs[a];
	const i32 ga = guildOf(na), gb = guildOf(_npcs[b]);
	// The bodies' own reach (Bip01 to the edge of the bounding box), then the arm's or the weapon's.
	float range = float(guildValue("FIGHT_RANGE_BASE", ga, 60) + guildValue("FIGHT_RANGE_BASE", gb, 60));
	if (na.weapon == WEAPON_1H || na.weapon == WEAPON_2H) {
		const GameItemInfo* w = na.melee != SCRIPT_NONE ? itemInfo(na.melee) : nullptr;
		range += float(guildValue(na.weapon == WEAPON_2H ? "FIGHT_RANGE_2HA" : "FIGHT_RANGE_1HA", ga, 0) + (w ? w->range : 50));
	} else {
		range += float(guildValue("FIGHT_RANGE_FIST", ga, 50));
	}
	return range;
}

float Game::approachRange(u32 a, u32 b) {
	if (a >= _npcCount) return 0.0f;
	return meleeRange(a, b) + float(guildValue("FIGHT_RANGE_G", guildOf(_npcs[a]), 0)) + 300.0f;
}

i32 Game::meleeDamage(u32 a, u32 b) {
	if (a >= _npcCount || b >= _npcCount) return 0;
	const GameNpc& na = _npcs[a];
	const GameNpc& nb = _npcs[b];
	const GameItemInfo* weapon = (na.weapon == WEAPON_1H || na.weapon == WEAPON_2H) && na.melee != SCRIPT_NONE ? itemInfo(na.melee) : nullptr;
	i32 type = weapon ? weapon->damageType : npcInt(na, _mDamageType);
	// A character the scripts give no damage type (every person of Gothic 1:
	// Npc_Default has none, and neither has the player's) hits blunt, as the
	// original engine sets it (OpenGothic: "vanilla behavior ... non-zero
	// damage type"). Without this no fist did any harm.
	if (type == 0) type = constant("DAM_BLUNT", 2);
	const i32 strength = attributeOf(na, constant("ATR_STRENGTH", 4));
	const i32 critical = constant("DAM_CRITICAL_MULTIPLIER", 2);
	// Gothic 1: the talent is the chance of a critical blow. Talents are not
	// kept yet (Npc_SetTalentValue): no critical blows.
	const bool crit = false;
	i32 total = 0;
	for (u32 i = 0; i < 8; ++i) {
		if (!(type & (1 << i))) continue;
		i32 dmg = npcInt(na, _mDamage, i);
		if (weapon) dmg += weapon->damage[i] + weapon->damageTotal;
		const i32 prot = npcInt(nb, _mProtection, i);
		if (prot < 0) continue; // immune
		const i32 v = strength + (crit ? critical : 1) * dmg - prot;
		total += v > 0 ? v : 0;
	}
	return total;
}

} // namespace g3
