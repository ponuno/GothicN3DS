// Items as the scripts see them: objects of C_ITEM the engine hands out
// (what a character has equipped, what is in a slot of its inventory, what
// lies in the world - see npc_items.cpp for that), and the engine functions
// about what a character carries and wears that need no living actor.
//
// What each function does is taken from OpenGothic (GameScript and
// Inventory), checked against the Gothic 1 scripts that call them.
#include "fight.h"
#include "game.h"

#include <math.h>
#include <string.h>

namespace g3 {

ScriptInstance* Game::itemObject(u32 symbol, i32 loot, i32 amount) {
	if (!isItem(symbol)) return nullptr;
	ItemObject* slot = nullptr;
	for (ItemObject& o : _itemObjects)
		if (o.inst && o.symbol == symbol && o.loot == loot) {
			slot = &o;
			break;
		}
	if (!slot) {
		// A free one, else the one not asked for the longest.
		for (ItemObject& o : _itemObjects)
			if (!o.inst || !slot || (slot->inst && o.used < slot->used)) {
				slot = &o;
				if (!o.inst) break;
			}
		if (slot->inst) _vm.destroy(slot->inst);
		*slot = ItemObject {_vm.create(symbol), symbol, loot, 0};
		if (!slot->inst) return nullptr;
	}
	slot->used = ++_itemObjectClock;
	if (_iAmount != SCRIPT_NONE) _vm.setInt(_iAmount, 0, slot->inst, amount);
	return slot->inst;
}

i32 Game::itemMaterial(u32 symbol) {
	const GameItemInfo* it = itemInfo(symbol);
	return it ? it->material : -1;
}

i32 Game::itemObjectLoot(const ScriptInstance* inst) const {
	if (!inst) return -2;
	for (const ItemObject& o : _itemObjects)
		if (o.inst == inst) return o.loot;
	return -2;
}

bool Game::meetsConditions(u32 npc, const GameItemInfo& it) const {
	if (npc >= _npcCount) return false;
	for (u32 k = 0; k < 3; ++k)
		if (it.condValue[k] > 0 && attributeOf(_npcs[npc], it.condAtr[k]) < it.condValue[k]) return false;
	return true;
}

u32 Game::bestArmor(u32 npc) {
	const i32 armorFlag = constant("ITEM_KAT_ARMOR", 1 << 4);
	u32 best = SCRIPT_NONE;
	i32 value = 0;
	for (u32 i = 0; i < _itemCount; ++i) {
		const GameItem& s = _items[i];
		if (s.npc != npc || s.count <= 0) continue;
		const GameItemInfo* it = itemInfo(s.symbol);
		if (!it || !(it->mainflag & armorFlag) || !meetsConditions(npc, *it)) continue;
		if (best == SCRIPT_NONE || it->value > value) {
			best = s.symbol;
			value = it->value;
		}
	}
	return best;
}

void Game::bindItems() {
	_iAmount = _vm.findMember("C_ITEM", "AMOUNT");
	if (_iAmount != SCRIPT_NONE && _vm.symbol(_iAmount).type != SCRIPT_INT) _iAmount = SCRIPT_NONE;
	memset(_itemObjects, 0, sizeof(_itemObjects));
	_itemObjectClock = 0;
	const struct {
		const char* name;
		ScriptVm::External fn;
	} table[] = {{"HLP_ISITEM", xIsItem},
	             {"NPC_GETEQUIPPEDMELEEWEAPON", xGetEquippedMelee},
	             {"NPC_GETEQUIPPEDRANGEDWEAPON", xGetEquippedRanged},
	             {"NPC_GETEQUIPPEDARMOR", xGetEquippedArmor},
	             {"NPC_HASEQUIPPEDARMOR", xHasEquippedArmor},
	             {"NPC_GETREADIEDWEAPON", xGetReadiedWeapon},
	             {"NPC_GETINVITEM", xGetInvItem},
	             {"NPC_GETINVITEMBYSLOT", xGetInvItemBySlot},
	             {"NPC_CLEARINVENTORY", xClearInventory},
	             // What a character wears changes at once, for those far away too
	             // (as AI_EquipBestMeleeWeapon): it is only a matter of what it has on.
	             {"AI_EQUIPARMOR", xEquipArmor},
	             {"AI_EQUIPBESTARMOR", xEquipBestArmor},
	             {"AI_UNEQUIPARMOR", xUnequipArmor},
	             {"AI_UNEQUIPWEAPONS", xUnequipWeapons}};
	for (const auto& e : table) _vm.bind(e.name, e.fn, this);
}

// Hlp_IsItem(item, ItMiNugget): the object is one of that kind.
void Game::xIsItem(ScriptVm& vm, void*) {
	const i32 kind = vm.popInt();
	const ScriptInstance* inst = vm.popInstance();
	vm.pushInt(inst && kind >= 0 && inst->symbol == u32(kind));
}

void Game::xGetEquippedMelee(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const GameNpc* n = g.npcOf(vm.popInstance());
	vm.pushInstance(n && n->melee != SCRIPT_NONE ? g.itemObject(n->melee) : nullptr);
}

void Game::xGetEquippedRanged(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const GameNpc* n = g.npcOf(vm.popInstance());
	vm.pushInstance(n && n->ranged != SCRIPT_NONE ? g.itemObject(n->ranged) : nullptr);
}

void Game::xGetEquippedArmor(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const GameNpc* n = g.npcOf(vm.popInstance());
	vm.pushInstance(n && n->armorItem != SCRIPT_NONE ? g.itemObject(n->armorItem) : nullptr);
}

void Game::xHasEquippedArmor(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const GameNpc* n = g.npcOf(vm.popInstance());
	vm.pushInt(n && n->armorItem != SCRIPT_NONE);
}

// The weapon in its hands: the melee weapon with a weapon of the hands drawn
// (there are no bows to draw yet); none with fists or nothing.
void Game::xGetReadiedWeapon(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const GameNpc* n = g.npcOf(vm.popInstance());
	const bool inHand = n && (n->weapon == WEAPON_1H || n->weapon == WEAPON_2H) && n->melee != SCRIPT_NONE;
	vm.pushInstance(inHand ? g.itemObject(n->melee) : nullptr);
}

// Npc_GetInvItem(npc, ItFoApple): the item, if it carries one, in ITEM and as the result.
void Game::xGetInvItem(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 kind = vm.popInt();
	const int who = g.indexOf(vm.popInstance());
	const i32 count = who >= 0 && kind >= 0 ? g.itemCount(u32(who), u32(kind)) : 0;
	ScriptInstance* obj = count > 0 ? g.itemObject(u32(kind), -1, count) : nullptr;
	g.setItemVar(obj);
	vm.pushInstance(obj);
}

// Npc_GetInvItemBySlot(npc, INV_WEAPON, 2): the second thing of the category
// (counted from 1) in ITEM; how many of it the character carries, 0 if none.
// Gothic 1's inventory has a page per category; here the things of a
// category are taken in the order the character got them.
void Game::xGetInvItemBySlot(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 slot = vm.popInt(), category = vm.popInt();
	const int who = g.indexOf(vm.popInstance());
	i32 flags = 0;
	switch (category) {
	case 1: flags = g.constant("ITEM_KAT_NF", 1 << 1) | g.constant("ITEM_KAT_FF", 1 << 2) | g.constant("ITEM_KAT_MUN", 1 << 3); break;
	case 2: flags = g.constant("ITEM_KAT_ARMOR", 1 << 4); break;
	case 3: flags = g.constant("ITEM_KAT_RUNE", 1 << 9); break;
	case 4: flags = g.constant("ITEM_KAT_MAGIC", i32(1u << 31)); break;
	case 5: flags = g.constant("ITEM_KAT_FOOD", 1 << 5); break;
	case 6: flags = g.constant("ITEM_KAT_POTIONS", 1 << 7); break;
	case 7: flags = g.constant("ITEM_KAT_DOCS", 1 << 6); break;
	case 8: flags = g.constant("ITEM_KAT_NONE", 1 << 0) | g.constant("ITEM_KAT_LIGHT", 1 << 8); break;
	default: break;
	}
	ScriptInstance* obj = nullptr;
	i32 count = 0;
	if (who >= 0 && flags && slot > 0) {
		i32 found = 0;
		for (u32 i = 0; i < g._itemCount && !obj; ++i) {
			const GameItem& s = g._items[i];
			if (s.npc != u32(who) || s.count <= 0) continue;
			const GameItemInfo* it = g.itemInfo(s.symbol);
			if (!it || !(it->mainflag & flags) || ++found != slot) continue;
			obj = g.itemObject(s.symbol, -1, s.count);
			count = obj ? s.count : 0;
		}
	}
	g.setItemVar(obj);
	vm.pushInt(count);
}

// Everything it carries goes, except what it wears or holds and the things
// of the story (ITEM_MISSION), as the original does.
void Game::xClearInventory(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const int who = g.indexOf(vm.popInstance());
	if (who < 0) return;
	const GameNpc& n = g._npcs[who];
	const i32 mission = g.constant("ITEM_MISSION", 1 << 12);
	for (u32 i = 0; i < g._itemCount; ++i) {
		GameItem& s = g._items[i];
		if (s.npc != u32(who) || s.count <= 0) continue;
		if (s.symbol == n.melee || s.symbol == n.ranged || s.symbol == n.armorItem) continue;
		const GameItemInfo* it = g.itemInfo(s.symbol);
		if (it && (it->flags & mission)) continue;
		s.count = 0;
		++g._itemsVersion;
	}
}

// AI_EquipArmor(npc, armour): the armour it carries is put on. Gothic 1
// passes the item (an object or a definition), Gothic 2 its number.
void Game::xEquipArmor(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	u32 armor = SCRIPT_NONE;
	(void)vm.popInstanceOf(armor);
	const int who = g.indexOf(vm.popInstance());
	if (who < 0 || armor == SCRIPT_NONE || g.itemCount(u32(who), armor) <= 0) return;
	const GameItemInfo* it = g.itemInfo(armor);
	if (!it || !(it->mainflag & g.constant("ITEM_KAT_ARMOR", 1 << 4))) return;
	g.wear(g._npcs[who], armor);
}

void Game::xEquipBestArmor(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const int who = g.indexOf(vm.popInstance());
	if (who < 0) return;
	const u32 best = g.bestArmor(u32(who));
	if (best != SCRIPT_NONE) g.wear(g._npcs[who], best);
}

void Game::xUnequipArmor(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const int who = g.indexOf(vm.popInstance());
	if (who >= 0) g.wear(g._npcs[who], SCRIPT_NONE);
}

void Game::xUnequipWeapons(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const int who = g.indexOf(vm.popInstance());
	if (who < 0) return;
	GameNpc& n = g._npcs[who];
	n.melee = n.ranged = SCRIPT_NONE;
	// (A weapon in hand goes with it; bare hands stay bare.)
	if (n.weapon == WEAPON_1H || n.weapon == WEAPON_2H) n.weapon = WEAPON_NONE;
	++g._itemsVersion;
}


// ---- trade ----
// How the original prices things is taken from OpenGothic (GameScript, Item::cost/sellCost, Npc::buyItem/sellItem):
// Gothic 1 has neither the currency symbol nor the multiplier, so there the ore nugget is the money and the
// player is paid what the thing costs; Gothic 2 and its mods name both in the scripts.

void Game::tradeLookUp() {
	if (_tradeTried) return;
	_tradeTried = true;
	_tradeCurrency = SCRIPT_NONE;
	_tradeMultiplier = 1.0f;
	const u32 named = _vm.find("TRADE_CURRENCY_INSTANCE");
	if (named != SCRIPT_NONE && _vm.symbol(named).type == SCRIPT_STRING && !(_vm.symbol(named).flags & SCRIPTF_MEMBER)) {
		const char* name = _vm.getString(named);
		const u32 sym = name && *name ? _vm.find(name) : SCRIPT_NONE;
		if (isItem(sym)) _tradeCurrency = sym;
	}
	if (_tradeCurrency == SCRIPT_NONE) {
		const u32 sym = _vm.find("ITMINUGGET");
		if (isItem(sym)) _tradeCurrency = sym;
	}
	const u32 mul = _vm.find("TRADE_VALUE_MULTIPLIER");
	if (mul != SCRIPT_NONE && _vm.symbol(mul).type == SCRIPT_FLOAT && !(_vm.symbol(mul).flags & SCRIPTF_MEMBER)) {
		const float m = _vm.getFloat(mul);
		if (m >= 0.0f && m < 1000.0f) _tradeMultiplier = m;
	}
}

u32 Game::tradeCurrency() {
	tradeLookUp();
	return _tradeCurrency;
}

float Game::tradeMultiplier() {
	tradeLookUp();
	return _tradeMultiplier;
}

i32 Game::buyPrice(u32 itemSymbol) {
	const GameItemInfo* it = itemInfo(itemSymbol);
	return it && it->value > 0 ? it->value : 0;
}

i32 Game::sellPrice(u32 itemSymbol) {
	const GameItemInfo* it = itemInfo(itemSymbol);
	if (!it || it->value <= 0) return 0;
	const double p = ceil(double(tradeMultiplier()) * double(it->value));
	return p < 0.0 ? 0 : p > 1.0e9 ? 1000000000 : i32(p);
}

} // namespace g3
