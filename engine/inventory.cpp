// The player's inventory: what it is made of (App::inventory), what using
// a thing does (App::itemAction and the jobs of App::itemsUpdate), the quick
// slots, and keeping it in state.bin. The rules are in inventory.h.
#include "lang.h"
#include "app.h"

#include "g3ds/item_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace g3 {

namespace {

constexpr u32 NONE = SCRIPT_NONE;

// Case-blind: does `hay` contain `needle` (lower case)?
bool has(const char* hay, const char* needle) {
	if (!hay || !needle) return false;
	const size_t n = strlen(needle);
	for (const char* p = hay; *p; ++p) {
		size_t k = 0;
		for (; k < n && p[k]; ++k) {
			char c = p[k];
			if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
			if (c != needle[k]) break;
		}
		if (k == n) return true;
	}
	return false;
}

// What a thing is, from its category (ITEM_KAT_*) and flags as the scripts define them.
u8 kindOf(const Game& g, const GameItemInfo& it) {
	const i32 mf = it.mainflag, fl = it.flags;
	const auto kat = [&](const char* name, u32 def) { return (mf & g.constant(name, i32(def))) != 0; };
	if (kat("ITEM_KAT_NF", 1u << 1)) return IK_MELEE;
	if (kat("ITEM_KAT_FF", 1u << 2)) return IK_RANGED;
	if (kat("ITEM_KAT_MUN", 1u << 3)) return IK_AMMO;
	if (kat("ITEM_KAT_ARMOR", 1u << 4)) return IK_ARMOR;
	if (kat("ITEM_KAT_FOOD", 1u << 5)) return IK_FOOD;
	if (kat("ITEM_KAT_DOCS", 1u << 6)) return IK_DOC;
	if (kat("ITEM_KAT_POTIONS", 1u << 7)) return IK_POTION;
	if (kat("ITEM_KAT_LIGHT", 1u << 8)) return IK_LIGHT;
	if (kat("ITEM_KAT_RUNE", 1u << 9)) return (fl & g.constant("ITEM_MULTI", 1 << 21)) ? IK_SCROLL : IK_RUNE;
	if (kat("ITEM_KAT_MAGIC", 0x80000000u)) return (fl & g.constant("ITEM_AMULET", 1 << 22)) ? IK_AMULET : IK_RING;
	return IK_MISC;
}

const char* visualOf(const Game& g, const GameItemInfo& it) { return g.text(it.visual); }

// The colour of a magic thing by what its name says it is about.
u32 elementTint(const char* v) {
	if (has(v, "fire") || has(v, "feuer") || has(v, "flame")) return 0xD8502CFFu;
	if (has(v, "ice") || has(v, "eis") || has(v, "water") || has(v, "frost")) return 0x5AA8E8FFu;
	if (has(v, "light") || has(v, "licht") || has(v, "sun")) return 0xE8D050FFu;
	if (has(v, "heal") || has(v, "heil")) return 0x58C060FFu;
	return 0x9A6AD8FFu;
}

// Its picture and the picture's colour.
void pictureOf(const Game& g, const GameItemInfo& it, u8 kind, u8& icon, u32& tint) {
	const char* v = visualOf(g, it);
	tint = 0xB4B4B4FFu;
	switch (kind) {
	case IK_MELEE: {
		const i32 two = g.constant("ITEM_2HD_SWD", 1 << 16) | g.constant("ITEM_2HD_AXE", 1 << 17);
		icon = (it.flags & g.constant("ITEM_AXE", 1 << 15)) || (it.flags & g.constant("ITEM_2HD_AXE", 1 << 17)) || has(v, "axe")
		           ? u8(ICON_AXE)
		       : has(v, "club") || has(v, "mace") || has(v, "hammer") || has(v, "keule") || has(v, "stab") || has(v, "staff")
		           ? u8(ICON_CLUB)
		       : (it.flags & two) ? u8(ICON_GREATSWORD)
		                          : u8(ICON_SWORD);
		break;
	}
	case IK_RANGED: icon = (it.flags & g.constant("ITEM_CROSSBOW", 1 << 20)) || has(v, "crossbow") ? ICON_CROSSBOW : ICON_BOW; break;
	case IK_AMMO: icon = ICON_ARROW; tint = 0x8A5A30FFu; break;
	case IK_ARMOR:
		icon = ICON_ARMOR;
		tint = has(v, "grd") ? 0x8A3A30FFu
		       : has(v, "ore") || has(v, "kdw") ? 0x4A6A98FFu
		       : has(v, "kdf") ? 0xA02A20FFu
		       : has(v, "nov") || has(v, "tpl") ? 0x7A7A58FFu
		       : has(v, "dmb") ? 0x50405AFFu
		                       : 0x8A5A34FFu;
		break;
	case IK_AMULET: icon = ICON_AMULET; tint = elementTint(v); break;
	case IK_RING: icon = ICON_RING; tint = elementTint(v); break;
	case IK_RUNE: icon = ICON_RUNE; tint = elementTint(v); break;
	case IK_SCROLL: icon = ICON_SCROLL; tint = elementTint(v); break;
	case IK_POTION:
		icon = ICON_POTION;
		tint = has(v, "health") || has(v, "heal") ? 0xC83C34FFu
		       : has(v, "mana") ? 0x3C6CD8FFu
		       : has(v, "strength") || has(v, "str") ? 0xE08A2CFFu
		       : has(v, "dex") ? 0x58B048FFu
		       : has(v, "haste") || has(v, "speed") ? 0xE8D040FFu
		                                             : 0x9A5AD0FFu;
		break;
	case IK_FOOD:
		if (has(v, "plant") || has(v, "herb") || has(v, "kraut") || has(v, "moos") || has(v, "berry") || has(v, "beere")) {
			icon = ICON_PLANT;
			tint = has(v, "mana") || has(v, "blue") ? 0x5A7AE0FFu : 0xD84A6AFFu;
		} else if (has(v, "beer") || has(v, "bier") || has(v, "wine") || has(v, "wein") || has(v, "booze") || has(v, "water") ||
		           has(v, "wasser") || has(v, "schnaps") || has(v, "bottle") || has(v, "flasche")) {
			icon = ICON_BOTTLE;
			tint = has(v, "wine") || has(v, "wein") ? 0x6A2040FFu : has(v, "water") || has(v, "wasser") ? 0x3A6AA8FFu : 0x7A5020FFu;
		} else if (has(v, "rice") || has(v, "reis") || has(v, "soup") || has(v, "suppe") || has(v, "stew") || has(v, "ragout")) {
			icon = ICON_BOWL;
			tint = has(v, "rice") || has(v, "reis") ? 0xE8DCB8FFu : 0xA0602CFFu;
		} else if (has(v, "meat") || has(v, "mutton") || has(v, "ham") || has(v, "fleisch") || has(v, "schinken") || has(v, "bug") ||
		           has(v, "leg")) {
			icon = ICON_MEAT;
			tint = 0xB85A48FFu;
		} else if (has(v, "bread") || has(v, "brot") || has(v, "loaf") || has(v, "cheese") || has(v, "kaese")) {
			icon = ICON_BREAD;
		} else {
			icon = ICON_APPLE;
			tint = has(v, "grape") || has(v, "traube") ? 0x6A3A9AFFu : 0xC8342CFFu;
		}
		break;
	case IK_DOC: icon = has(v, "map") || has(v, "karte") ? ICON_MAP : has(v, "book") || has(v, "buch") ? ICON_BOOK : ICON_LETTER;
		tint = 0x8A2A24FFu;
		break;
	case IK_LIGHT: icon = ICON_TORCH; break;
	default:
		if (has(v, "key") || has(v, "schl")) icon = ICON_KEY;
		else if (has(v, "nugget") || has(v, "ore") || has(v, "erz")) {
			icon = ICON_ORE;
			tint = 0x6A8EC0FFu;
		} else if (has(v, "itat") || has(v, "teeth") || has(v, "claw") || has(v, "horn") || has(v, "fur") || has(v, "fell"))
			icon = ICON_TROPHY;
		else icon = ICON_BAG;
		break;
	}
}

// The scripts' attributes by number (ATR_ ...), as the info box names them.
const char* attributeWord(i32 a) {
	switch (a) {
	case 0: return tr("Здоровье");
	case 1: return tr("Макс. здоровье");
	case 2: return tr("Мана");
	case 3: return tr("Макс. мана");
	case 4: return tr("Сила");
	case 5: return tr("Ловкость");
	default: return tr("Свойство");
	}
}

void row(ItemEntry& e, const char* label, i32 value, bool hasValue = true, bool warn = false) {
	if (e.rowCount >= ITEM_ROWS) return;
	ItemRow& r = e.rows[e.rowCount++];
	snprintf(r.label, sizeof(r.label), "%s", label);
	if (hasValue) snprintf(r.value, sizeof(r.value), "%ld", static_cast<long>(value));
	else r.value[0] = 0;
	r.warn = warn ? 1 : 0;
}

// Sorting: the pages' order, then what is worn first, then the dearer first.
int kindOrder(u8 kind) {
	static const u8 kOrder[ITEM_KINDS] = {0, 1, 2, 3, 4, 5, 7, 8, 6, 9, 10, 12, 11, 13};
	return kind < ITEM_KINDS ? kOrder[kind] : 99;
}

bool before(const ItemEntry& a, const ItemEntry& b) {
	if (kindOrder(a.kind) != kindOrder(b.kind)) return kindOrder(a.kind) < kindOrder(b.kind);
	const bool wa = (a.flags & ITEMF_EQUIPPED) != 0, wb = (b.flags & ITEMF_EQUIPPED) != 0;
	if (wa != wb) return wa;
	if (a.value != b.value) return a.value > b.value;
	return a.symbol < b.symbol;
}

u32 mix(u32 h, u32 v) { return (h ^ v) * 16777619u; }

} // namespace

ItemTab tabOf(u8 kind) {
	switch (kind) {
	case IK_MELEE:
	case IK_RANGED:
	case IK_AMMO: return TAB_WEAPONS;
	case IK_ARMOR:
	case IK_AMULET:
	case IK_RING: return TAB_ARMOR;
	case IK_RUNE:
	case IK_SCROLL: return TAB_MAGIC;
	case IK_POTION: return TAB_POTIONS;
	case IK_FOOD: return TAB_FOOD;
	default: return TAB_OTHER;
	}
}

// ---- rules ----

bool App::heroInCombat() const { return _hero.weapon() != WEAPON_NONE || _calm < 5.0f; }

// Whether the character has what the item takes; if not, `why` says what is missing.
bool App::meetsNeeds(const GameItemInfo& it, char* why, u32 size) const {
	const GameNpc* h = heroNpc();
	if (!h) return true;
	const Game& g = _npcs.game();
	for (int k = 0; k < 3; ++k) {
		if (it.condValue[k] <= 0) continue;
		if (g.attributeOf(*h, it.condAtr[k]) < it.condValue[k]) {
			if (why) snprintf(why, size, tr("Нужно: %s %ld"), attributeWord(it.condAtr[k]), static_cast<long>(it.condValue[k]));
			return false;
		}
	}
	// A rune takes the circle of magic it is of (OpenGothic: Item::checkCondRune).
	if (it.circle > 0) {
		const i32 talent = g.constant("NPC_TALENT_MAGE", 7);
		if (talent >= 0 && talent < 16 && h->skill[talent] < it.circle) {
			if (why) snprintf(why, size, tr("Нужен круг магии: %ld"), static_cast<long>(it.circle));
			return false;
		}
	}
	return true;
}

bool App::heroCanSneak() const {
	const GameNpc* h = heroNpc();
	if (!h) return false;
	const i32 talent = _npcs.game().constant("NPC_TALENT_SNEAK", 8);
	return talent >= 0 && talent < 16 && h->skill[talent] != 0;
}

void App::itemMessage(const char* text, bool refusal) {
	snprintf(_inv.message, sizeof(_inv.message), "%s", text);
	_inv.messageTime = 3.0f;
	toast(text, 2.0f);
	if (refusal) ++_itemStats.refused;
	_invStamp = 0; // (shown at once)
}

// What a thing is for. Consumables start a job (itemsUpdate carries it
// out); equipment changes at once, or through a job when a weapon has to
// change hands first.
bool App::itemAction(u32 symbol) {
	const int hi = heroIndex();
	if (hi < 0) return false;
	Game& g = _npcs.game();
	const GameItemInfo* it = g.itemInfo(symbol);
	if (!it) return false;
	char name[48];
	scriptText(g.text(it->description ? it->description : it->name), name, sizeof(name));
	char buf[96];
	if (g.itemCount(u32(hi), symbol) <= 0) {
		snprintf(buf, sizeof(buf), tr("Нет: %s"), name);
		itemMessage(buf);
		return false;
	}
	if (_hero.down()) {
		itemMessage(tr("Сейчас нельзя"));
		return false;
	}
	const u8 kind = kindOf(g, *it);
	switch (kind) {
	case IK_POTION:
	case IK_FOOD:
		if (it->onUse == NONE) {
			snprintf(buf, sizeof(buf), tr("%s: этим не воспользоваться"), name);
			itemMessage(buf);
			return false;
		}
		return startUse(symbol);
	case IK_MELEE:
	case IK_RANGED:
	case IK_ARMOR:
	case IK_AMULET:
	case IK_RING: return equipItem(symbol, *it, kind);
	case IK_RUNE:
	case IK_SCROLL:
		if (_hero.weapon() == WEAPON_MAG && _spellItem == symbol && !_hero.drawing()) return heroTake(WEAPON_NONE);
		return selectSpell(symbol);
	case IK_DOC: return readItem(symbol);
	case IK_LIGHT: itemMessage(tr("Факелов пока нет")); return false;
	case IK_AMMO: itemMessage(tr("Стрелы берутся с луком")); return false;
	default:
		snprintf(buf, sizeof(buf), "%s", name);
		itemMessage(buf, false);
		return false;
	}
}

// A potion or food: queued behind what is being used, or begun.
bool App::startUse(u32 symbol) {
	Game& g = _npcs.game();
	const int hi = heroIndex();
	if (_job.stage != JOB_NONE) {
		if (_job.symbol == symbol) return true; // (pressed twice: once is enough)
		_queued = symbol;
		const GameItemInfo* it = g.itemInfo(symbol);
		char name[48], buf[80];
		scriptText(it ? g.text(it->description ? it->description : it->name) : "", name, sizeof(name));
		snprintf(buf, sizeof(buf), tr("Следом: %s"), name);
		itemMessage(buf, false);
		return true;
	}
	if (_player.swimming || _player.climbing || _player.flying) {
		itemMessage(_player.swimming ? tr("Не в воде") : tr("Сейчас нельзя"));
		return false;
	}
	_job = ItemJob();
	_job.symbol = symbol;
	_job.stage = _hero.weapon() != WEAPON_NONE ? JOB_PUTAWAY : JOB_START;
	_job.redraw = _settings.weaponBack ? _hero.weapon() : u8(WEAPON_NONE);
	if (!_hero.loaded()) {
		// No character to animate (an archive without one): it works at once.
		const GameItemInfo* it = g.itemInfo(symbol);
		if (it && hi >= 0) {
			g.giveItems(u32(hi), symbol, -1);
			g.callItemFunction(u32(hi), symbol, it->onUse);
			++_itemStats.used;
		}
		_job = ItemJob();
	}
	_invStamp = 0;
	return true;
}

void App::equipRing(u32 symbol, bool on) {
	const int hi = heroIndex();
	const GameItemInfo* it = hi >= 0 ? _npcs.game().itemInfo(symbol) : nullptr;
	if (!it) return;
	const u32 fn = on ? it->onEquip : it->onUnequip;
	if (fn != NONE) _npcs.game().callItemFunction(u32(hi), symbol, fn);
}

bool App::equipItem(u32 symbol, const GameItemInfo& it, u8 kind) {
	const int hi = heroIndex();
	Game& g = _npcs.game();
	GameNpc& h = g.npcMut(u32(hi));
	char name[48], buf[96], why[48];
	scriptText(g.text(it.description ? it.description : it.name), name, sizeof(name));
	const bool worn = kind == IK_MELEE    ? h.melee == symbol
	                  : kind == IK_RANGED ? h.ranged == symbol
	                  : kind == IK_ARMOR  ? h.armorItem == symbol
	                  : kind == IK_AMULET ? _amulet == symbol
	                                      : (_rings[0] == symbol || _rings[1] == symbol);
	if (!worn && !meetsNeeds(it, why, sizeof(why))) {
		itemMessage(why);
		return false;
	}
	if (_job.stage != JOB_NONE && kind == IK_MELEE) {
		itemMessage(tr("Руки заняты"));
		return false;
	}
	++_equipVersion;
	++_itemStats.changed;
	switch (kind) {
	case IK_MELEE:
		if (worn) {
			if (_hero.weapon() == WEAPON_1H || _hero.weapon() == WEAPON_2H) {
				// In the hands: away with it (it stays on the belt).
				if (!heroTake(WEAPON_NONE)) {
					itemMessage(tr("Сейчас нельзя"));
					return false;
				}
				return true;
			}
			h.melee = NONE;
			snprintf(buf, sizeof(buf), tr("Снято: %s"), name);
			itemMessage(buf, false);
			return true;
		}
		if (_hero.weapon() == WEAPON_1H || _hero.weapon() == WEAPON_2H || _hero.drawing()) {
			// Another one in hand: away with it, then this one in hand (job).
			_job = ItemJob();
			_job.symbol = symbol;
			_job.stage = JOB_SWAP_AWAY;
			_invStamp = 0;
			return true;
		}
		h.melee = symbol;
		snprintf(buf, sizeof(buf), tr("Оружие: %s"), name);
		itemMessage(buf, false);
		return true;
	case IK_RANGED: {
		const bool inHand = _hero.weapon() == WEAPON_BOW || _hero.weapon() == WEAPON_CBOW;
		if (worn) {
			// Already the bow: in hand or away.
			if (inHand) return heroTake(WEAPON_NONE);
			if (!heroTake(rangedMode())) {
				itemMessage(tr("Сейчас нельзя"));
				return false;
			}
		} else {
			h.ranged = symbol;
			if (inHand) heroTake(WEAPON_NONE);
			else if (_hero.weapon() == WEAPON_NONE && !_hero.drawing()) heroTake(rangedMode());
		}
		snprintf(buf, sizeof(buf), tr("Лук: %s"), name);
		itemMessage(buf, false);
		return true;
	}
	case IK_ARMOR:
		g.setArmor(u32(hi), worn ? NONE : symbol);
		snprintf(buf, sizeof(buf), worn ? tr("Снято: %s") : tr("Надето: %s"), name);
		itemMessage(buf, false);
		return true;
	case IK_AMULET:
		if (worn) {
			equipRing(symbol, false);
			_amulet = NONE;
		} else {
			if (_amulet != NONE) equipRing(_amulet, false);
			_amulet = symbol;
			equipRing(symbol, true);
		}
		snprintf(buf, sizeof(buf), worn ? tr("Снято: %s") : tr("Надето: %s"), name);
		itemMessage(buf, false);
		return true;
	default: // a ring
		if (worn) {
			equipRing(symbol, false);
			(_rings[0] == symbol ? _rings[0] : _rings[1]) = NONE;
		} else {
			// (Two of the same ring may be worn if two are carried.)
			const int free = _rings[0] == NONE ? 0 : _rings[1] == NONE ? 1 : -1;
			if (free < 0) {
				--_itemStats.changed;
				itemMessage(tr("Два кольца уже надеты"));
				return false;
			}
			_rings[free] = symbol;
			equipRing(symbol, true);
		}
		snprintf(buf, sizeof(buf), worn ? tr("Снято: %s") : tr("Надето: %s"), name);
		itemMessage(buf, false);
		return true;
	}
}

// Takes everything off (before a saved inventory is put back).
void App::unequipAll() {
	const int hi = heroIndex();
	if (hi < 0) return;
	for (u32& r : _rings)
		if (r != NONE) {
			equipRing(r, false);
			r = NONE;
		}
	if (_amulet != NONE) equipRing(_amulet, false);
	_amulet = NONE;
	Game& g = _npcs.game();
	g.setArmor(u32(hi), NONE);
	GameNpc& h = g.npcMut(u32(hi));
	h.melee = h.ranged = NONE;
	++_equipVersion;
}

bool App::quickUse(int slot) {
	if (slot < 0 || slot >= QUICK_SLOTS) return false;
	const u32 s = _quick[slot];
	if (s == NONE) {
		itemMessage(tr("Слот пуст"));
		return false;
	}
	_panel.invalidate();
	const int hi = heroIndex();
	Game& g = _npcs.game();
	const GameItemInfo* it = hi >= 0 ? g.itemInfo(s) : nullptr;
	if (!it) return false;
	if (kindOf(g, *it) == IK_MELEE) {
		// A weapon of a slot: to the belt if it is not, and in hand (or away if it is).
		const GameNpc& h = g.npc(u32(hi));
		if (g.itemCount(u32(hi), s) <= 0) return itemAction(s); // (says what is missing)
		if (h.melee != s) return itemAction(s);                 // (put on - swapped in hand if one is held)
		const u8 mode = meleeMode();
		if (_hero.weapon() == mode) return heroTake(WEAPON_NONE);
		return heroTake(mode) || (itemMessage(tr("Сейчас нельзя")), false);
	}
	return itemAction(s);
}

void App::setQuickSlot(int slot, u32 symbol) {
	if (slot < 0 || slot >= QUICK_SLOTS) return;
	// One thing in one slot.
	for (u32& q : _quick)
		if (q == symbol) q = NONE;
	_quick[slot] = symbol;
	_invStamp = 0;
	if (symbol != NONE && _npcs.loaded()) {
		Game& g = _npcs.game();
		const GameItemInfo* it = g.itemInfo(symbol);
		char name[48], buf[80];
		scriptText(it ? g.text(it->description ? it->description : it->name) : "", name, sizeof(name));
		snprintf(buf, sizeof(buf), tr("Слот %d: %s"), (slot + 1) % 10, name);
		itemMessage(buf, false);
	}
}

void App::panelCommand(const PanelCommand& c) {
	switch (c.type) {
	case PANEL_USE: itemAction(c.symbol); break;
	case PANEL_USE_SLOT: quickUse(c.slot); break;
	case PANEL_ASSIGN: setQuickSlot(c.slot, c.symbol); break;
	case PANEL_CLEAR_SLOT: setQuickSlot(c.slot, NONE); break;
	case PANEL_CHEST_MOVE: case PANEL_CHEST_ALL: case PANEL_CHEST_CLOSE: case PANEL_LOCK_PICK: case PANEL_LOCK_CANCEL: chestCommand(c); break;
	case PANEL_TALK_CHOOSE: _npcs.talkChoose(c.slot); break;
	case PANEL_TALK_SKIP: _npcs.talkSkip(); break;
	case PANEL_TALK_LEAVE: _npcs.talkBack(); break;
	case PANEL_DOC_PAGE:
	case PANEL_DOC_CLOSE: docCommand(c); break;
	default: break; // (the report and the cursor are the platform's)
	}
}

PanelCommand App::panelInput(const PanelInput& in, float dt) {
	const PanelCommand c = _panel.update(in, inventory(), dt);
	panelCommand(c);
	return c;
}

// ---- the jobs: a frame of using things ----

void App::itemsUpdate(Input& in, float dt) {
	const int hi = heroIndex();
	if (_inv.messageTime > 0.0f) {
		_inv.messageTime -= dt;
		if (_inv.messageTime <= 0.0f) {
			_inv.message[0] = 0;
			_invStamp = 0;
		}
	}
	// In a fight: blows given and taken, an armed enemy close by.
	{
		const u32 blows = _fight.blowsLanded + _fight.blowsTaken + _fight.parried + _fight.dodged;
		_calm = blows != _lastBlows ? 0.0f : _calm + dt;
		_lastBlows = blows;
		_hostileScan -= dt;
		if (_hostileScan <= 0.0f && hi >= 0) {
			_hostileScan = 0.5f;
			const float feet[3] = {_player.feet.x, _player.feet.y, _player.feet.z};
			const int t = _npcs.pickTarget(feet, _hero.yaw(), 800.0f, 3.2f, -1, 0, true);
			if (t >= 0) {
				const GameNpc& n = _npcs.game().npc(u32(t));
				if (n.target == hi && n.down == DOWN_NONE) _calm = 0.0f; // (an enemy after the character)
			}
		}
	}
	// A thing on the ground in reach, hands empty: A picks it up.
	_lootTarget = -1;
	if (hi >= 0 && _job.stage == JOB_NONE && _hero.loaded() && !_hero.down() && _hero.weapon() == WEAPON_NONE && !_hero.drawing() &&
	    !_panel.open() && !_player.swimming && !_player.climbing && !heroTransformed()) {
		const Vec3 feet = _player.feet;
		_lootTarget = _loot.nearest(feet, _hero.yaw(), 150.0f, 1.0f);
		if (_lootTarget >= 0 && (in.confirm || in.attack)) {
			_job = ItemJob();
			_job.stage = JOB_START;
			_job.pickup = true;
			_job.loot = u32(_lootTarget);
			_job.symbol = _loot.item(u32(_lootTarget)).symbol;
			in.confirm = in.attack = false;
			_invStamp = 0;
		}
	}
	if (_job.stage == JOB_NONE) {
		if (_queued != NONE && !_hero.down()) {
			const u32 q = _queued;
			_queued = NONE;
			startUse(q);
		}
		if (_job.stage == JOB_NONE) return;
	}
	if (hi < 0 || _hero.down() || !_hero.loaded()) {
		_job = ItemJob();
		_queued = NONE;
		_invStamp = 0;
		return;
	}
	Game& g = _npcs.game();
	const GameItemInfo* it = g.itemInfo(_job.symbol);
	char name[48], buf[96];
	scriptText(it ? g.text(it->description ? it->description : it->name) : "", name, sizeof(name));
	// Until the item is at the mouth the player may call it off.
	const bool wantsFight = in.attack || in.drawWeapon || in.meleeSlot || in.fistSlot;
	if (wantsFight && (_job.stage == JOB_PUTAWAY || _job.stage == JOB_START)) {
		_job = ItemJob();
		_queued = NONE;
		itemMessage(tr("Отменено"), false);
		return;
	}
	// The hands are the job's: no blows, no parries meanwhile.
	in.attack = in.parry = in.drawWeapon = in.meleeSlot = in.fistSlot = false;
	_job.wait += dt;
	const auto done = [&]() {
		_job = ItemJob();
		_invStamp = 0;
	};
	switch (_job.stage) {
	case JOB_PUTAWAY:
		if (_hero.weapon() == WEAPON_NONE && !_hero.drawing()) {
			_job.stage = JOB_START;
			_job.wait = 0;
		} else if (!_hero.drawing()) {
			heroTake(WEAPON_NONE); // (refused while a blow or a stumble lasts: tried again)
		}
		if (_job.wait > 3.0f) {
			itemMessage(tr("Не получилось убрать оружие"));
			done();
		}
		break;
	case JOB_START: {
		const bool drink = it && kindOf(g, *it) == IK_POTION;
		const char* scheme = it ? g.text(it->scheme) : "";
		// Without the animations: a potion takes a second, food two (the item works 60% in);
		// picking up, a second (in hand at 0.45: the bend shows).
		const float seconds = _job.pickup ? 1.0f : drink ? 1.2f : 2.0f;
		if (_job.pickup ? _hero.pickUp(seconds, 0.45f) : _hero.useItem(scheme, seconds, seconds * 0.6f)) {
			_job.stage = JOB_USING;
			_job.wait = 0;
			const GameNpc& h = g.npc(u32(hi));
			for (int a = 0; a < 6; ++a) _job.before[a] = g.attributeOf(h, a);
		} else if (_job.wait > 3.0f) {
			itemMessage(tr("Сейчас нельзя"));
			done();
		}
		break;
	}
	case JOB_USING:
		if (_hero.useMoment() && !_job.applied && _job.pickup) {
			_job.applied = true;
			if (it && _loot.take(_job.loot)) {
				const i32 n = _loot.item(_job.loot).amount;
				g.giveItems(u32(hi), _job.symbol, n);
				++_itemStats.pickedUp;
				_npcs.heroTook(_job.symbol, n); // (those about may see it: a theft)
				if (n > 1) snprintf(buf, sizeof(buf), tr("Взято: %s ×%ld"), name, static_cast<long>(n));
				else snprintf(buf, sizeof(buf), tr("Взято: %s"), name);
				itemMessage(buf, false);
			}
		} else if (_hero.useMoment() && !_job.applied) {
			_job.applied = true;
			if (!it || g.itemCount(u32(hi), _job.symbol) <= 0) {
				snprintf(buf, sizeof(buf), tr("Нет: %s"), name);
				itemMessage(buf);
			} else {
				g.giveItems(u32(hi), _job.symbol, -1);
				g.callItemFunction(u32(hi), _job.symbol, it->onUse);
				++_itemStats.used;
				// What it did, as the attributes say.
				const GameNpc& h = g.npc(u32(hi));
				buf[0] = 0;
				size_t n = 0;
				for (int a = 0; a < 6 && n + 1 < sizeof(buf); ++a) {
					const i32 d = g.attributeOf(h, a) - _job.before[a];
					if (d == 0) continue;
					n += size_t(snprintf(buf + n, sizeof(buf) - n, "%s%s %+ld", n ? " · " : "", attributeWord(a), static_cast<long>(d)));
				}
				if (!buf[0]) snprintf(buf, sizeof(buf), "%s", name);
				itemMessage(buf, false);
			}
		}
		if (!_hero.usingItem()) {
			if (!_job.applied) {
				snprintf(buf, sizeof(buf), tr("Прервано: %s"), name);
				++_itemStats.interrupted;
				itemMessage(buf, false);
			}
			if (_job.redraw != WEAPON_NONE) {
				_job.stage = JOB_REDRAW;
				_job.wait = 0;
			} else {
				done();
			}
		}
		break;
	case JOB_REDRAW:
		if (heroTake(_job.redraw == WEAPON_FIST ? u8(WEAPON_FIST) : meleeMode()) || _job.wait > 2.0f) done();
		break;
	case JOB_SWAP_AWAY:
		if (_hero.weapon() == WEAPON_NONE && !_hero.drawing()) {
			GameNpc& h = g.npcMut(u32(hi));
			h.melee = _job.symbol;
			++_equipVersion;
			snprintf(buf, sizeof(buf), tr("Оружие: %s"), name);
			itemMessage(buf, false);
			_job.stage = JOB_SWAP_DRAW;
			_job.wait = 0;
		} else if (!_hero.drawing()) {
			heroTake(WEAPON_NONE);
		}
		if (_job.stage == JOB_SWAP_AWAY && _job.wait > 3.0f) done();
		break;
	case JOB_SWAP_DRAW:
		if (heroTake(meleeMode()) || _job.wait > 2.0f) done();
		break;
	default: done(); break;
	}
}

// ---- what the bottom screen shows ----

void App::fillEntry(ItemEntry& e, u32 symbol, i32 count, const GameNpc& h, bool carried) {
	Game& g = _npcs.game();
	const GameItemInfo* it = g.itemInfo(symbol);
	memset(&e, 0, sizeof(e));
	if (!it) return;
	{
		e.symbol = symbol;
		e.count = count;
		e.value = it->value;
		e.kind = kindOf(g, *it);
		pictureOf(g, *it, e.kind, e.icon, e.tint);
		scriptText(g.text(it->description ? it->description : it->name), e.name, sizeof(e.name));
		const u32 inHand = (_hero.weapon() == WEAPON_1H || _hero.weapon() == WEAPON_2H) ? h.melee : NONE;
		const bool worn = carried && (symbol == h.melee || symbol == h.ranged || symbol == h.armorItem || symbol == _amulet ||
		                              symbol == _rings[0] || symbol == _rings[1]);
		if (worn) e.flags |= ITEMF_EQUIPPED;
		if (carried && symbol == inHand) e.flags |= ITEMF_IN_HAND;
		char why[48];
		if (carried && !worn && !meetsNeeds(*it, why, sizeof(why))) e.flags |= ITEMF_BLOCKED;
		for (int q = 0; q < QUICK_SLOTS && carried; ++q)
			if (_quick[q] == symbol) e.quick |= u16(1u << q);
		// The action.
		const char* action = "";
		switch (e.kind) {
		case IK_POTION: action = it->onUse != NONE ? tr("Выпить") : ""; break;
		case IK_FOOD:
			action = it->onUse == NONE ? "" : e.icon == ICON_BOTTLE ? tr("Выпить") : tr("Съесть");
			break;
		case IK_MELEE: action = (e.flags & ITEMF_IN_HAND) ? tr("Убрать") : worn ? tr("Снять") : tr("Взять"); break;
		case IK_RANGED:
			action = (_hero.weapon() == WEAPON_BOW || _hero.weapon() == WEAPON_CBOW) && worn ? tr("Убрать") : worn ? tr("В руки") : tr("Взять");
			if (worn && action == tr("Убрать")) e.flags |= ITEMF_IN_HAND;
			break;
		case IK_RUNE:
		case IK_SCROLL:
			action = _hero.weapon() == WEAPON_MAG && _spellItem == symbol ? tr("Убрать") : tr("Выбрать");
			if (_hero.weapon() == WEAPON_MAG && _spellItem == symbol) e.flags |= ITEMF_IN_HAND;
			break;
		case IK_ARMOR:
		case IK_AMULET:
		case IK_RING: action = worn ? tr("Снять") : tr("Надеть"); break;
		default: break;
		}
		snprintf(e.action, sizeof(e.action), "%s", action);
		if (action[0]) e.flags |= ITEMF_ACTION;
		// Anything with an action may go into a quick slot (a potion, a weapon, armour).
		if (e.flags & ITEMF_ACTION) e.flags |= ITEMF_QUICK;
		if (!carried) e.flags &= u8(~ITEMF_QUICK);
		e.picture = thumbOf(it->visual);
		// The info box: the scripts' own lines first, as the original shows them.
		for (int k = 0; k < 6; ++k) {
			if (!it->text[k]) continue;
			char label[40];
			scriptText(g.text(it->text[k]), label, sizeof(label));
			if (!label[0]) continue;
			row(e, label, it->count[k], it->count[k] != 0);
		}
		if (e.rowCount == 0) {
			// Made up from the numbers (the built-in scene, mods without texts).
			if (e.kind == IK_MELEE || e.kind == IK_RANGED) {
				i32 dmg = it->damageTotal;
				for (i32 d : it->damage) dmg += d;
				row(e, tr("Урон"), dmg);
			} else if (e.kind == IK_ARMOR) {
				row(e, tr("Защита от оружия"), it->protection[g.constant("PROT_EDGE", 2)]);
				row(e, tr("Защита от стрел"), it->protection[g.constant("PROT_POINT", 6)]);
				row(e, tr("Защита от огня"), it->protection[g.constant("PROT_FIRE", 3)]);
				row(e, tr("Защита от магии"), it->protection[g.constant("PROT_MAGIC", 5)]);
			}
			for (int k = 0; k < 3; ++k)
				if (it->condValue[k] > 0) {
					char label[40];
					snprintf(label, sizeof(label), tr("Нужно: %s"), attributeWord(it->condAtr[k]));
					row(e, label, it->condValue[k], true, g.attributeOf(h, it->condAtr[k]) < it->condValue[k]);
				}
			if (e.kind == IK_MELEE) {
				const i32 two = g.constant("ITEM_2HD_SWD", 1 << 16) | g.constant("ITEM_2HD_AXE", 1 << 17);
				row(e, (it->flags & two) ? tr("Двуручное оружие") : tr("Одноручное оружие"), 0, false);
			}
			if (it->value > 0) row(e, tr("Цена"), it->value);
		} else {
			// (The scripts' lines name what it takes; the warning is ours.)
			for (int k = 0; k < 3; ++k)
				if (it->condValue[k] > 0 && g.attributeOf(h, it->condAtr[k]) < it->condValue[k])
					for (int r = 0; r < e.rowCount; ++r)
						if (e.rows[r].value[0] && atol(e.rows[r].value) == it->condValue[k]) e.rows[r].warn = 1;
		}
	}
}

const InventoryView& App::inventory() {
	InventoryView& v = _inv;
	const int hi = heroIndex();
	if (hi < 0) {
		if (v.present) {
			v.present = false;
			v.count = 0;
			++v.version;
		}
		return v;
	}
	Game& g = _npcs.game();
	const GameNpc& h = g.npc(u32(hi));
	// A stamp of all that is shown: made anew only when it changes.
	u32 stamp = 0x9E3779B9u;
	stamp = mix(stamp, g.itemsVersion());
	stamp = mix(stamp, _equipVersion);
	stamp = mix(stamp, h.melee);
	stamp = mix(stamp, h.ranged);
	stamp = mix(stamp, h.armorItem);
	stamp = mix(stamp, _hero.weapon());
	stamp = mix(stamp, _spellItem);
	for (u32 q : _quick) stamp = mix(stamp, q);
	for (int a = 0; a < 6; ++a) stamp = mix(stamp, u32(g.attributeOf(h, a)));
	const bool combat = heroInCombat();
	stamp = mix(stamp, combat);
	stamp = mix(stamp, _job.stage);
	stamp = mix(stamp, _job.symbol);
	stamp = mix(stamp, _queued);
	stamp = mix(stamp, g.logVersion());
	stamp = mix(stamp, _diarySeen);
	stamp = mix(stamp, u32(i32(_inv.messageTime > 0.0f)));
	stamp = mix(stamp, _npcs.talkView().version);
	stamp = mix(stamp, _chests.version());
	stamp = mix(stamp, u32(_npcs.trading()));
	stamp = mix(stamp, u32(_chest.stage));
	stamp = mix(stamp, _doc.active ? 1u + _doc.page * 64u + _doc.pages : 0u);
	stamp = mix(stamp, u32(_chest.corpse + 1));
	stamp = mix(stamp, u32(_corpseTarget + 1));
	if (_chest.corpse >= 0) {
		u32 sym;
		i32 cnt;
		for (u32 k = 0; corpseStack(u32(_chest.corpse), k, sym, cnt); ++k) stamp = mix(mix(stamp, sym), u32(cnt));
	}
	stamp = mix(stamp, u32(_chest.progress));
	stamp = mix(stamp, u32(_chest.picks));
	stamp = mix(stamp, u32(_chest.news[0]));
	if (stamp == _invStamp && v.present) return v;
	_invStamp = stamp;

	v.present = true;
	v.doc.open = _doc.active;
	v.doc.map = _doc.map;
	v.doc.page = _doc.page;
	v.doc.pages = _doc.pages;
	v.hp = g.attributeOf(h, 0);
	v.hpMax = g.attributeOf(h, 1);
	v.mana = g.attributeOf(h, 2);
	v.manaMax = g.attributeOf(h, 3);
	v.combat = combat;
	v.busy = _job.stage != JOB_NONE;
	v.busyShare = 0.0f; // (no bar: as in the original, the animation says how far it is)
	v.busyText[0] = 0;
	v.ore = 0;
	char name[48];
	if (v.busy) {
		const GameItemInfo* it = g.itemInfo(_job.symbol);
		scriptText(it ? g.text(it->description ? it->description : it->name) : "", name, sizeof(name));
		const bool drink = it && kindOf(g, *it) == IK_POTION;
		switch (_job.stage) {
		case JOB_PUTAWAY: snprintf(v.busyText, sizeof(v.busyText), "%s", tr("Убирает оружие…")); break;
		case JOB_START:
		case JOB_USING:
			snprintf(v.busyText, sizeof(v.busyText), "%s %s", _job.pickup ? tr("Поднимает:") : drink ? tr("Пьёт:") : tr("Ест:"), name);
			break;
		case JOB_REDRAW: snprintf(v.busyText, sizeof(v.busyText), "%s", tr("Снова берёт оружие…")); break;
		default: snprintf(v.busyText, sizeof(v.busyText), tr("Меняет оружие: %s"), name); break;
		}
	}

	// The things carried.
	v.count = 0;
	for (u32 i = 0; i < g.itemStackCount() && v.count < INVENTORY_MAX; ++i) {
		const GameItem& st = g.itemStack(i);
		if (st.npc != u32(hi) || st.count <= 0) continue;
		if (!g.itemInfo(st.symbol)) continue;
		ItemEntry& e = v.items[v.count];
		fillEntry(e, st.symbol, st.count, h, true);
		if (e.icon == ICON_ORE) v.ore += st.count;
		if (v.busy && _job.symbol == st.symbol) e.flags |= ITEMF_BUSY;
		++v.count;
	}
	// In the pages' order (insertion sort: a few dozen at most).
	for (u32 i = 1; i < v.count; ++i) {
		u32 k = i;
		while (k > 0 && before(v.items[i], v.items[k - 1])) --k;
		if (k == i) continue;
		ItemEntry tmp;
		memcpy(&tmp, &v.items[i], sizeof(tmp));
		memmove(&v.items[k + 1], &v.items[k], sizeof(ItemEntry) * (i - k));
		memcpy(&v.items[k], &tmp, sizeof(tmp));
	}
	if (_npcs.trading()) tradeDecoratePack(v);
	// The quick slots and the equipment.
	const auto picture = [&](u32 s, u8& icon, u32& tint, char* label, u32 size) {
		const GameItemInfo* it = s != NONE ? g.itemInfo(s) : nullptr;
		if (!it) {
			icon = ICON_BAG;
			tint = 0;
			if (label) label[0] = 0;
			return;
		}
		pictureOf(g, *it, kindOf(g, *it), icon, tint);
		if (label) scriptText(g.text(it->description ? it->description : it->name), label, size);
	};
	for (int q = 0; q < QUICK_SLOTS; ++q) {
		v.quick[q] = _quick[q];
		v.quickCount[q] = _quick[q] != NONE ? g.itemCount(u32(hi), _quick[q]) : 0;
		picture(_quick[q], v.quickIcon[q], v.quickTint[q], v.quickName[q], sizeof(v.quickName[q]));
		const GameItemInfo* qi = _quick[q] != NONE ? g.itemInfo(_quick[q]) : nullptr;
		v.quickPicture[q] = qi ? thumbOf(qi->visual) : i16(-1);
	}
	const u32 worn[EQUIP_SLOTS] = {h.armorItem, _amulet, _rings[0], _rings[1], h.melee, h.ranged};
	for (int k = 0; k < EQUIP_SLOTS; ++k) {
		v.equip[k] = worn[k];
		picture(worn[k], v.equipIcon[k], v.equipTint[k], nullptr, 0);
	}
	// The character's page.
	{
		InventoryView::Sheet& sh = v.sheet;
		scriptText(g.nameOf(h), sh.name, sizeof(sh.name));
		const auto member = [&](const char* m) {
			const u32 sym = g.vm().findMember("C_NPC", m);
			return sym != NONE && g.vm().symbol(sym).type == SCRIPT_INT ? g.npcInt(h, sym) : 0;
		};
		sh.level = member("LEVEL");
		sh.exp = member("EXP");
		sh.expNext = member("EXP_NEXT");
		sh.learn = member("LP");
		sh.strength = g.attributeOf(h, g.constant("ATR_STRENGTH", 4));
		sh.dexterity = g.attributeOf(h, g.constant("ATR_DEXTERITY", 5));
		const u32 prot = g.vm().findMember("C_NPC", "PROTECTION");
		const i32 kinds[4] = {g.constant("PROT_EDGE", 2), g.constant("PROT_POINT", 6), g.constant("PROT_FIRE", 3), g.constant("PROT_MAGIC", 5)};
		for (int k = 0; k < 4; ++k)
			sh.protection[k] = prot != NONE && u32(kinds[k]) < g.vm().symbol(prot).count ? g.npcInt(h, prot, u32(kinds[k])) : 0;
		// The guild by the scripts' own names (TXT_GUILDS), if they have them.
		const i32 guild = g.guildOf(h);
		const u32 names = g.vm().find("TXT_GUILDS");
		sh.guild[0] = 0;
		if (names != NONE && g.vm().symbol(names).type == SCRIPT_STRING && guild >= 0 && u32(guild) < g.vm().symbol(names).count)
			scriptText(g.vm().getString(names, u32(guild)), sh.guild, sizeof(sh.guild));
		const auto nameOfItem = [&](u32 sym, char* out, u32 size) {
			const GameItemInfo* it = sym != NONE ? g.itemInfo(sym) : nullptr;
			out[0] = 0;
			if (it) scriptText(g.text(it->description ? it->description : it->name), out, size);
		};
		// The talents, by the scripts' own words: "Name#level 0|level 1|level 2".
		sh.talentCount = 0;
		const u32 tNames = g.vm().find("TXT_TALENTS"), tLevels = g.vm().find("TXT_TALENTS_SKILLS");
		if (tNames != NONE && g.vm().symbol(tNames).type == SCRIPT_STRING) {
			const u32 total = g.vm().symbol(tNames).count < 16 ? g.vm().symbol(tNames).count : 16u;
			for (u32 t = 0; t < total && sh.talentCount < InventoryView::Sheet::TALENTS; ++t) {
				const char* title = g.vm().getString(tNames, t);
				if (!title || !*title) continue;
				InventoryView::Sheet::Talent& tl = sh.talents[sh.talentCount++];
				tl = InventoryView::Sheet::Talent();
				tl.skill = h.skill[t];
				tl.value = h.talent[t];
				scriptText(title, tl.name, sizeof(tl.name));
				if (tLevels != NONE && g.vm().symbol(tLevels).type == SCRIPT_STRING && t < g.vm().symbol(tLevels).count) {
					// (The level's word: the text after '#', the tl.skill-th of those split by '|'.)
					const char* en = g.vm().getString(tLevels, t);
					const char* p = en ? strchr(en, '#') : nullptr;
					p = p ? p + 1 : en;
					for (i32 k = 0; p && k < tl.skill; ++k) {
						p = strchr(p, '|');
						if (p) ++p;
					}
					if (p) {
						const char* e = strchr(p, '|');
						char word[32];
						size_t n = e ? size_t(e - p) : strlen(p);
						if (n >= sizeof(word)) n = sizeof(word) - 1;
						memcpy(word, p, n);
						word[n] = 0;
						scriptText(word, tl.level, sizeof(tl.level));
					}
				}
				if (!tl.level[0]) snprintf(tl.level, sizeof(tl.level), "%ld", static_cast<long>(tl.skill));
			}
		}
		nameOfItem(h.melee, sh.melee, sizeof(sh.melee));
		nameOfItem(h.ranged, sh.ranged, sizeof(sh.ranged));
		nameOfItem(h.armorItem, sh.armor, sizeof(sh.armor));
	}
	// The diary: the topics most recently written to first, each with its entries.
	const u32 diaryStamp = mix(mix(0x9E3779B9u, g.logVersion()), _diarySeen);
	if (diaryStamp != _logStamp) {
		_logStamp = diaryStamp;
		scriptText(g.chapter().title, v.chapter, sizeof(v.chapter));
		scriptText(g.chapter().subtitle, v.chapterSub, sizeof(v.chapterSub));
		v.chapterNumber = g.chapterNumber();
		u32 order[InventoryView::DIARY_TOPICS];
		u32 n = 0;
		for (u32 t = 0; t < g.logTopicCount(); ++t) {
			u32 k = n < InventoryView::DIARY_TOPICS ? n++ : InventoryView::DIARY_TOPICS - 1;
			if (n == InventoryView::DIARY_TOPICS && k == n - 1 && g.logTopic(order[k]).order > g.logTopic(t).order) continue;
			order[k] = t;
			while (k > 0 && g.logTopic(order[k - 1]).order < g.logTopic(order[k]).order) {
				const u32 tmp = order[k - 1];
				order[k - 1] = order[k];
				order[k] = tmp;
				--k;
			}
		}
		// (The tasks before the notes, each newest first: the panel lists them so.)
		{
			u32 sorted[InventoryView::DIARY_TOPICS];
			u32 m = 0;
			for (int pass = 0; pass < 2; ++pass)
				for (u32 i = 0; i < n; ++i)
					if ((g.logTopic(order[i]).section == 1) == (pass == 1)) sorted[m++] = order[i];
			memcpy(order, sorted, sizeof(u32) * n);
		}
		v.topicCount = 0;
		v.entryCount = 0;
		for (u32 i = 0; i < n; ++i) {
			const GameLogTopic& lt = g.logTopic(order[i]);
			InventoryView::Topic& t = v.topics[v.topicCount++];
			scriptText(lt.name, t.name, sizeof(t.name));
			t.status = lt.status;
			t.section = lt.section;
			t.fresh = lt.order > _diarySeen;
			t.first = u16(v.entryCount);
			t.count = 0;
			// (Newest entries first, as many as there is room for.)
			for (u32 e = g.logEntryCount(); e-- > 0 && v.entryCount < InventoryView::DIARY_ENTRIES;)
				if (g.logEntry(e).topic == order[i]) {
					scriptText(g.logEntry(e).text, v.entries[v.entryCount++], sizeof(v.entries[0]));
					++t.count;
				}
		}
	}
	chestView(h);
	if (v.talk.version != _npcs.talkView().version) v.talk = _npcs.talkView();
	++v.version;
	return v;
}

// The picture of a look, read from the archive the first time it is wanted.
i16 App::thumbOf(u16 visual) {
	if (!visual) return -1;
	const int have = _pictures.find(visual);
	if (have >= 0) return i16(have);
	for (u32 i = 0; i < _noPictureCount; ++i)
		if (_noPicture[i] == visual) return -1;
	const auto missing = [&]() {
		if (_noPictureCount < sizeof(_noPicture) / sizeof(_noPicture[0])) _noPicture[_noPictureCount++] = visual;
		return i16(-1);
	};
	char name[96];
	snprintf(name, sizeof(name), "icon/%s", _npcs.game().text(visual));
	const PakEntry* e = _pak.find(name);
	if (!e || e->size < sizeof(IconHeader)) return missing();
	u8* blob = static_cast<u8*>(malloc(e->size));
	if (!blob) return missing();
	IconHeader h {};
	int at = -1;
	if (_pak.read(*e, blob)) {
		memcpy(&h, blob, sizeof(h));
		if (h.magic == ITEMICON_MAGIC && h.version == ITEMICON_VERSION && e->size == sizeof(h) + size_t(h.width) * h.height * 4)
			at = _pictures.add(visual, h.width, h.height, blob + sizeof(h));
	}
	free(blob);
	return at >= 0 ? i16(at) : missing();
}

// ---- the faults menu's set of things ----

u32 App::giveItemKit() {
	const int hi = heroIndex();
	if (hi < 0) return 0;
	Game& g = _npcs.game();
	struct Quota {
		u8 kind;
		u8 kinds; // how many different ones
		i32 each; // of each
	};
	static const Quota kQuota[] = {{IK_POTION, 4, 5}, {IK_FOOD, 4, 5}, {IK_MELEE, 3, 1}, {IK_RANGED, 1, 1}, {IK_AMMO, 1, 30},
	                               {IK_ARMOR, 2, 1},  {IK_RING, 2, 1},  {IK_AMULET, 1, 1}, {IK_RUNE, 1, 1},  {IK_SCROLL, 2, 3},
	                               {IK_DOC, 1, 1},    {IK_LIGHT, 1, 3}, {IK_MISC, 3, 1}};
	u8 given[ITEM_KINDS] = {};
	u32 kinds = 0;
	u32 firstOf[ITEM_KINDS][2];
	for (auto& f : firstOf) f[0] = f[1] = NONE;
	const u32 best = g.bestMeleeOfAll();
	for (u32 s = 0; s < g.vm().symbolCount(); ++s) {
		if (!g.isItem(s)) continue;
		const GameItemInfo* it = g.itemInfo(s);
		if (!it || (!it->name && !it->description) || !it->visual) continue;
		const u8 kind = kindOf(g, *it);
		if ((kind == IK_POTION || kind == IK_FOOD) && it->onUse == NONE) continue;
		for (const Quota& q : kQuota) {
			if (q.kind != kind || given[kind] >= q.kinds) continue;
			// The best weapon of all among the melee ones; the ore by the hundred.
			if (kind == IK_MELEE && given[kind] == q.kinds - 1 && best != NONE && s != best) break;
			u8 icon;
			u32 tint;
			pictureOf(g, *it, kind, icon, tint);
			const i32 n = icon == ICON_ORE ? 100 : q.each;
			if (given[kind] < 2) firstOf[kind][given[kind]] = s;
			++given[kind];
			g.giveItems(u32(hi), s, n);
			++kinds;
			break;
		}
	}
	if (best != NONE && g.itemCount(u32(hi), best) <= 0) {
		g.giveItems(u32(hi), best, 1);
		++kinds;
	}
	// Empty quick slots get the first potions, food and the best weapon.
	bool empty = true;
	for (u32 q : _quick) empty = empty && q == NONE;
	if (empty) {
		_quick[0] = firstOf[IK_POTION][0];
		_quick[1] = firstOf[IK_POTION][1];
		_quick[2] = firstOf[IK_FOOD][0];
		_quick[3] = firstOf[IK_FOOD][1];
		_quick[4] = best != NONE ? best : firstOf[IK_MELEE][0];
	}
	_invStamp = 0;
	return kinds;
}

// The test kit: every spell as a rune and some as scrolls, the potions that raise the
// attributes, one weapon, one bow with arrows, one armour. Names are the ones of the
// original scripts; whatever a mod's scripts lack is skipped, and a bow or an armour falls
// back to the first of its kind.
u32 App::giveTestKit() {
	const int hi = heroIndex();
	if (hi < 0) return 0;
	Game& g = _npcs.game();
	u32 kinds = 0;
	const auto give = [&](const char* name, i32 n) -> u32 {
		const u32 s = g.vm().find(name);
		if (s == NONE || !g.isItem(s)) return NONE;
		g.giveItems(u32(hi), s, n);
		++kinds;
		return s;
	};
	static const char* const kRunes[] = {
	    "ItArRuneLight",     "ItArRuneFirebolt",    "ItArRuneFireball",  "ItArRuneFirestorm", "ItArRuneFireRain",   "ItArRuneThunderbolt",
	    "ItArRuneThunderball", "ItArRuneChainLightning", "ItArRuneIceCube", "ItArRuneIceWave",  "ItArRuneWindfist",   "ItArRuneStormfist",
	    "ItArRuneHeal",      "ItArRuneSleep",       "ItArRunePyrokinesis", "ItArRuneBreathOfDeath", "ItArRuneDestroyUndead",
	    "ItArRuneTelekinesis", "ItArRuneCharm",     "ItArRuneControl",   "ItArRuneTeleport1"};
	static const char* const kScrolls[] = {"ItArScrollLight",      "ItArScrollFirebolt", "ItArScrollFireball", "ItArScrollThunderbolt",
	                                       "ItArScrollIceCube",    "ItArScrollWindfist", "ItArScrollHeal",     "ItArScrollSleep"};
	static const char* const kPotions[] = {
	    "ItFo_Potion_Strength_01", "ItFo_Potion_Strength_02", "ItFo_Potion_Strength_03", "ItFo_Potion_Dex_01",  "ItFo_Potion_Dex_02",
	    "ItFo_Potion_Dex_03",      "ItFo_Potion_Health_Perma_01", "ItFo_Potion_Health_Perma_02", "ItFo_Potion_Health_Perma_03",
	    "ItFo_Potion_Mana_Perma_01", "ItFo_Potion_Mana_Perma_02", "ItFo_Potion_Mana_Perma_03", "ItFo_Potion_Master_01",
	    "ItFo_Potion_Master_02",   "ItFo_Potion_Health_03",   "ItFo_Potion_Mana_03"};
	for (const char* n : kRunes) give(n, 1);
	for (const char* n : kScrolls) give(n, 3);
	for (const char* n : kPotions) give(n, 2);
	GameNpc& h = g.npcMut(u32(hi));
	// One weapon (put on the belt), the best of the scripts if the usual one is not there.
	u32 sword = give("ItMw_1H_Sword_Long_01", 1);
	if (sword == NONE) sword = g.bestMeleeOfAll();
	if (sword != NONE) {
		if (g.itemCount(u32(hi), sword) <= 0) g.giveItems(u32(hi), sword, 1);
		if (h.melee == NONE) h.melee = sword;
	}
	// One bow, arrows for it.
	u32 bow = give("ItRw_Bow_Long_01", 1);
	if (bow == NONE) bow = give("ItRw_Bow_Small_01", 1);
	if (bow != NONE && h.ranged == NONE) h.ranged = bow;
	give("ItAmArrow", 50);
	// One armour, put on.
	u32 armor = give("GRD_ARMOR_M", 1);
	if (armor == NONE) armor = give("VLK_ARMOR_M", 1);
	if (armor != NONE) g.setArmor(u32(hi), armor);
	// Mana to spend on the spells (ATR_MANA_MAX, ATR_MANA).
	GameNpc& hero = g.npcMut(u32(hi));
	const i32 manaMax = max(g.attributeOf(hero, 3), i32(100));
	g.setAttribute(hero, 3, manaMax);
	g.setAttribute(hero, 2, manaMax);
	++_equipVersion;
	_invStamp = 0;
	return kinds;
}

// The spells' kit for the debug menu: every rune, scrolls (the transformations and the summonings among them), mana to cast.
u32 App::giveSpellKit() {
	const int hi = heroIndex();
	if (hi < 0) return 0;
	Game& g = _npcs.game();
	u32 kinds = 0;
	const auto give = [&](const char* name, i32 n) {
		const u32 s = g.vm().find(name);
		if (s == NONE || !g.isItem(s)) return;
		g.giveItems(u32(hi), s, n);
		++kinds;
	};
	static const char* const kRunes[] = {
	    "ItArRuneLight", "ItArRuneFirebolt", "ItArRuneFireball", "ItArRuneFirestorm", "ItArRuneFireRain", "ItArRuneThunderbolt",
	    "ItArRuneThunderball", "ItArRuneChainLightning", "ItArRuneIceCube", "ItArRuneIceWave", "ItArRuneWindfist", "ItArRuneStormfist",
	    "ItArRuneHeal", "ItArRuneSleep", "ItArRunePyrokinesis", "ItArRuneBreathOfDeath", "ItArRuneDestroyUndead", "ItArRuneTelekinesis",
	    "ItArRuneCharm", "ItArRuneControl", "ItArRuneTeleport1", "ItArRuneTeleport2", "ItArRuneTeleport3", "ItArRuneTeleport4",
	    "ItArRuneTeleport5", "ItArRuneTeleport6", "ItArRuneFear", "ItArRuneBerzerk", "ItArRuneShrink", "ItArRuneMassDeath",
	    "ItArRuneSuckEnergy", "ItArRuneDeathBolt", "ItArRuneDeathBall", "ItArRuneArmyOfDarkness", "ItArRuneWave", "ItArRuneFireStorm",
	    "ItArRuneTrfBloodfly", "ItArRuneTrfCrawler", "ItArRuneTrfLurker", "ItArRuneTrfMeatbug", "ItArRuneTrfMolerat", "ItArRuneTrfOrcDog",
	    "ItArRuneTrfScavenger", "ItArRuneTrfShadowbeast", "ItArRuneTrfSnapper", "ItArRuneTrfWaran", "ItArRuneTrfWolf", "ItArRuneTrfSheep",
	    "ItArRuneSumGol", "ItArRuneSumDemon", "ItArRuneSumSkel", "ItArRuneSumWolf", "ItArRuneSumGobSkel", "ItArRuneSumIceGol",
	    "ItArRuneSumFireGol", "ItArRuneSumSwampGol", "ItArRuneSumZombie", "ItArRuneSummonGolem", "ItArRuneSummonDemon", "ItArRuneSummonSkeleton",
	    "ItArRuneSummonWolf", "ItArRuneSummonGoblinSkeleton", "ItArRuneSummonIceGolem", "ItArRuneSummonFireGolem", "ItArRuneSummonSwampGolem"};
	static const char* const kScrolls[] = {
	    "ItArScrollLight", "ItArScrollFirebolt", "ItArScrollFireball", "ItArScrollThunderbolt", "ItArScrollIceCube", "ItArScrollWindfist",
	    "ItArScrollHeal", "ItArScrollSleep", "ItArScrollSumGol", "ItArScrollSumDemon", "ItArScrollSumSkel", "ItArScrollSumWolf",
	    "ItArScrollTrfBloodfly", "ItArScrollTrfCrawler", "ItArScrollTrfMeatbug", "ItArScrollTrfMolerat", "ItArScrollTrfOrcDog",
	    "ItArScrollTrfScavenger", "ItArScrollTrfWaran", "ItArScrollTrfWolf", "ItArScrollTrfSheep", "ItArScrollTrfShadowbeast",
	    "ItArScrollTrfSnapper", "ItArScrollTrfLurker", "ItArScrollTeleport1", "ItArScrollTeleport2", "ItArScrollTeleport3",
	    "ItArScrollFear", "ItArScrollShrink", "ItArScrollCharm", "ItArScrollTelekinesis", "ItArScrollPyrokinesis",
	    "ItArScrollSummonGolem", "ItArScrollSummonDemon", "ItArScrollSummonSkeleton", "ItArScrollSummonWolf"};
	for (const char* n : kRunes) give(n, 1);
	for (const char* n : kScrolls) give(n, 5);
	// Mana to spend, and the circles known (the scripts' magic skill).
	GameNpc& hero = g.npcMut(u32(hi));
	const i32 manaMax = max(g.attributeOf(hero, 3), i32(1000));
	g.setAttribute(hero, 3, manaMax);
	g.setAttribute(hero, 2, manaMax);
	const i32 mage = g.constant("NPC_TALENT_MAGE", 7);
	if (mage > 0 && mage < 16) hero.skill[mage] = 6;
	++_equipVersion;
	_invStamp = 0;
	return kinds;
}

// The crafts' kit: what the things of the worlds ask the user to carry, and the usual stuff to work on.
u32 App::giveCraftKit() {
	const int hi = heroIndex();
	if (hi < 0) return 0;
	Game& g = _npcs.game();
	u32 kinds = 0;
	const auto give = [&](const char* name, i32 n) {
		const u32 s = g.vm().find(name);
		if (s == NONE || !g.isItem(s)) return;
		g.giveItems(u32(hi), s, n);
		++kinds;
	};
	// Every item the things ask for (their useItem), whichever the game.
	const Mobs& mobs = _npcs.mobs();
	if (mobs.hasInfo()) {
		const char* seen[16] = {};
		u32 seenCount = 0;
		for (u32 i = 0; i < mobs.count(); ++i) {
			const WorldMobInfo& mi = mobs.info(i);
			if (!mi.useItem) continue;
			const char* name = mobs.infoText(mi.useItem);
			bool dup = false;
			for (u32 k = 0; k < seenCount; ++k) dup = dup || strcmp(seen[k], name) == 0;
			if (dup) continue;
			if (seenCount < 16) seen[seenCount++] = name;
			give(name, 3);
		}
	}
	static const char* const kStuff[] = {
	    "ItMiNugget", "ItMiSwordRaw", "ItMiSwordRawHot", "ItMiSwordBladeHot", "ItMiSwordBlade", "ItMiHammer", "ItMiFlask", "ItMiJoint",
	    "ItFoMuttonRaw", "ItFoMutton", "ItFoCheese", "ItFoBread", "ItFoApple", "ItFoBooze", "ItFoBeer", "ItFoWine", "ItMiScoop",
	    "ItMiStomper", "ItMiAlchemy_Alcohol_01", "ItMiAlchemy_Salt_01", "ItMiAlchemy_Sulphur_01", "ItMiAlchemy_Moleratlubric_01",
	    "ItMiAlchemy_Quicksilver_01", "ItMiAlchemy_Syrianoil_01", "ItMiNugget", "ItMiCoal", "ItMiSteel", "ItMiPan",
	    "ItFo_Plants_Herb_01", "ItFo_Plants_Herb_02", "ItFo_Plants_Herb_03", "ItFo_Plants_Berrys_01", "ItFo_Plants_Mushroom_01",
	    "ItFo_Plants_Mushroom_02", "ItFo_Plants_Mana_01", "ItFo_Plants_Mana_02", "ItFo_Plants_Mana_03", "ItFo_Plants_Health_01",
	    "ItFo_Plants_Health_02", "ItFo_Plants_Health_03", "ItFo_Plants_Strength_01", "ItFo_Plants_Dex_01", "ItFo_Plants_Speed_01",
	    "ItFo_Plants_Bloodwood_01", "ItFo_Plants_Trollberrys_01", "ItFo_Plants_Seraphis_01", "ItFo_Plants_Velais_01",
	    "ItPl_Health_Herb_01", "ItPl_Health_Herb_02", "ItPl_Health_Herb_03", "ItPl_Mana_Herb_01", "ItPl_Mana_Herb_02",
	    "ItPl_Mana_Herb_03", "ItPl_Speed_Herb_01", "ItPl_Strength_Herb_01", "ItPl_Dex_Herb_01", "ItPl_Perm_Herb", "ItPl_Beet",
	    "ItPl_Mushroom_01", "ItPl_Mushroom_02", "ItPl_Forestberry", "ItPl_Planeberry", "ItPl_Temp_Herb", "ItPl_Swampherb",
	    "ItAt_Claws_01", "ItAt_Teeth_01", "ItAt_Wolf_01", "ItAt_Sting_01", "ItAt_Wing_01", "ItAt_Mandibles_01"};
	for (const char* n : kStuff) give(n, 3);
	++_equipVersion;
	_invStamp = 0;
	return kinds;
}

// ---- state.bin: the inventory after the position ----

namespace {

// 'G3I2': ten quick slots. ('G3IV', the first, had four: it is still read.)
constexpr u32 INVENTORY_MAGIC = fourcc('G', '3', 'I', '2'), INVENTORY_MAGIC_1 = fourcc('G', '3', 'I', 'V');

struct SavedInventory {
	u32 magic;
	u32 stacks;     // followed by this many SavedStack
	i32 attribute[8];
	u32 equip[EQUIP_SLOTS]; // name hashes of the items, 0 for none
	u32 quick[QUICK_SLOTS];
};
struct SavedInventory1 {
	u32 magic;
	u32 stacks;
	i32 attribute[8];
	u32 equip[EQUIP_SLOTS];
	u32 quick[4];
};
struct SavedStack {
	u32 hash;
	i32 count;
};

} // namespace

void App::saveInventory(FILE* f) const {
	const int hi = heroIndex();
	if (hi < 0 || !f) return;
	const Game& g = _npcs.game();
	const ScriptVm& vm = g.vm();
	const GameNpc& h = g.npc(u32(hi));
	const auto hashOf = [&](u32 s) { return s != NONE && s < vm.symbolCount() ? vm.symbol(s).nameHash : 0u; };
	SavedInventory si {};
	si.magic = INVENTORY_MAGIC;
	for (u32 i = 0; i < g.itemStackCount(); ++i) si.stacks += g.itemStack(i).npc == u32(hi) && g.itemStack(i).count > 0;
	for (int a = 0; a < 8; ++a) si.attribute[a] = g.attributeOf(h, a);
	const u32 worn[EQUIP_SLOTS] = {h.armorItem, _amulet, _rings[0], _rings[1], h.melee, h.ranged};
	for (int k = 0; k < EQUIP_SLOTS; ++k) si.equip[k] = hashOf(worn[k]);
	for (int q = 0; q < QUICK_SLOTS; ++q) si.quick[q] = hashOf(_quick[q]);
	u32 check = 0x9E3779B9u;
	const auto add = [&](const void* p, size_t n) {
		const u8* b = static_cast<const u8*>(p);
		for (size_t k = 0; k < n; ++k) check = (check ^ b[k]) * 16777619u;
		fwrite(p, n, 1, f);
	};
	add(&si, sizeof(si));
	for (u32 i = 0; i < g.itemStackCount(); ++i) {
		const GameItem& st = g.itemStack(i);
		if (st.npc != u32(hi) || st.count <= 0) continue;
		const SavedStack ss = {hashOf(st.symbol), st.count};
		add(&ss, sizeof(ss));
	}
	fwrite(&check, sizeof(check), 1, f);
}

bool App::loadInventory(FILE* f) {
	const int hi = heroIndex();
	if (hi < 0 || !f) return false;
	SavedInventory si {};
	u32 check = 0x9E3779B9u;
	const auto sum = [&](const void* p, size_t n) {
		const u8* b = static_cast<const u8*>(p);
		for (size_t k = 0; k < n; ++k) check = (check ^ b[k]) * 16777619u;
	};
	u32 magic = 0;
	if (fread(&magic, sizeof(magic), 1, f) != 1) return false;
	if (magic == INVENTORY_MAGIC) {
		si.magic = magic;
		if (fread(reinterpret_cast<u8*>(&si) + 4, sizeof(si) - 4, 1, f) != 1) return false;
		sum(&si, sizeof(si));
	} else if (magic == INVENTORY_MAGIC_1) {
		SavedInventory1 old {};
		old.magic = magic;
		if (fread(reinterpret_cast<u8*>(&old) + 4, sizeof(old) - 4, 1, f) != 1) return false;
		sum(&old, sizeof(old));
		si.stacks = old.stacks;
		memcpy(si.attribute, old.attribute, sizeof(si.attribute));
		memcpy(si.equip, old.equip, sizeof(si.equip));
		for (int q = 0; q < 4; ++q) si.quick[q] = old.quick[q];
	} else {
		return false;
	}
	if (si.stacks > 4096) return false;
	const u32 n = si.stacks;
	SavedStack* stacks = static_cast<SavedStack*>(malloc(sizeof(SavedStack) * (n ? n : 1u)));
	if (!stacks) return false;
	bool ok = true;
	for (u32 i = 0; i < n && ok; ++i) {
		ok = fread(&stacks[i], sizeof(SavedStack), 1, f) == 1;
		if (ok) sum(&stacks[i], sizeof(SavedStack));
	}
	u32 stored = 0;
	if (!ok || fread(&stored, sizeof(stored), 1, f) != 1 || stored != check) {
		free(stacks);
		return false;
	}
	Game& g = _npcs.game();
	const ScriptVm& vm = g.vm();
	const auto symbolOf = [&](u32 hash) -> u32 {
		if (!hash) return NONE;
		const u32 s = vm.findHash(hash);
		return s != NONE && g.isItem(s) ? s : NONE;
	};
	// Everything off, everything out; then what was carried, worn, kept in slots.
	_job = ItemJob();
	_queued = NONE;
	unequipAll();
	for (u32 i = 0; i < g.itemStackCount(); ++i) {
		const GameItem& st = g.itemStack(i);
		if (st.npc == u32(hi) && st.count > 0) g.giveItems(u32(hi), st.symbol, -st.count);
	}
	for (u32 i = 0; i < n; ++i) {
		const u32 s = symbolOf(stacks[i].hash);
		if (s != NONE && stacks[i].count > 0) g.giveItems(u32(hi), s, stacks[i].count);
	}
	free(stacks);
	GameNpc& h = g.npcMut(u32(hi));
	const u32 armor = symbolOf(si.equip[EQUIP_ARMOR]);
	if (armor != NONE) g.setArmor(u32(hi), armor);
	h.melee = symbolOf(si.equip[EQUIP_MELEE]);
	h.ranged = symbolOf(si.equip[EQUIP_RANGED]);
	_amulet = symbolOf(si.equip[EQUIP_AMULET]);
	if (_amulet != NONE) equipRing(_amulet, true);
	for (int r = 0; r < 2; ++r) {
		_rings[r] = symbolOf(si.equip[EQUIP_RING1 + r]);
		if (_rings[r] != NONE) equipRing(_rings[r], true);
	}
	for (int q = 0; q < QUICK_SLOTS; ++q) _quick[q] = symbolOf(si.quick[q]);
	// The attributes as they were (the rings' share included); alive.
	for (int a = 0; a < 8; ++a) g.setAttribute(h, a, si.attribute[a]);
	if (g.attributeOf(h, 0) < 1) g.setAttribute(h, 0, 1);
	++_equipVersion;
	_invStamp = 0;
	return true;
}

// ---- the top screen: the info box with the thing turning in it, the hints ----

bool App::itemOverlay() const {
	// (While a thing is being used nothing comes up: the animation says it all.)
	return _inv.present && (_panel.inspecting() > 0.0f || _panel.assigning());
}

namespace {

// The info box on the top screen, and the part of it the thing turns in.
constexpr int INFO_X0 = 112, INFO_X1 = 392, INFO_Y0 = 148, INFO_Y1 = 222, MODEL_W = 92;

} // namespace

// The chosen thing's model, turning slowly in the right of the info box: it
// is put into the world half a metre before the camera, where that part of
// the box is on the screen (and the box leaves that part clear).
void App::drawShowcase() {
	if (!itemOverlay() || !_npcs.loaded()) return;
	Game& g = _npcs.game();
	const u32 symbol = _panel.selected();
	const GameItemInfo* it = symbol != NONE ? g.itemInfo(symbol) : nullptr;
	if (!it || !it->visual) return;
	if (symbol != _showSymbol || it->visual != _showVisual) {
		_showSymbol = symbol;
		if (it->visual != _showVisual) {
			_showModel.release();
			_showVisual = it->visual;
			char name[96];
			snprintf(name, sizeof(name), "model/item.%s", g.text(it->visual));
			_showTried = _showModel.loadEntry(_pak, hashName(name)) && _showModel.hasMesh();
		}
	}
	if (!_showTried || !_showModel.loaded()) return;
	const ModelHeader& mh = _showModel.header();
	// (Its diagonal: however it turns, it stays in the box.)
	const float size = sqrtf((mh.bbMax[0] - mh.bbMin[0]) * (mh.bbMax[0] - mh.bbMin[0]) + (mh.bbMax[1] - mh.bbMin[1]) * (mh.bbMax[1] - mh.bbMin[1]) +
	                         (mh.bbMax[2] - mh.bbMin[2]) * (mh.bbMax[2] - mh.bbMin[2]));
	if (size <= 0.0f) return;
	// Where the model's part of the box is, as a direction from the eye.
	const float sx = float(INFO_X1 - MODEL_W / 2), sy = float(INFO_Y0 + INFO_Y1) * 0.5f;
	const float ndcX = sx / float(gfx::SCREEN_W) * 2.0f - 1.0f, ndcY = 1.0f - sy / float(gfx::SCREEN_H) * 2.0f;
	const float dist = 50.0f, halfH = dist * tanf(_cam.fovY * 0.5f), halfW = halfH * float(gfx::SCREEN_W) / float(gfx::SCREEN_H);
	const Vec3 f = forwardFromYawPitch(_cam.yaw, _cam.pitch);
	const Vec3 r = normalize(cross({0, 1, 0}, f)), u = cross(f, r);
	const Vec3 anchor = _cam.pos + f * dist + r * (ndcX * halfW) + u * (ndcY * halfH);
	// As big as the box is high, less a margin.
	const float boxH = float(INFO_Y1 - INFO_Y0 - 10) / float(gfx::SCREEN_H) * 2.0f * halfH;
	const float s = boxH / size;
	// Shown as the picture shows it, turning about its length (a long thing)
	// or its up.
	const Mat34 show = itemShowcase(mh.bbMin, mh.bbMax);
	const float sizes[3] = {mh.bbMax[0] - mh.bbMin[0], mh.bbMax[1] - mh.bbMin[1], mh.bbMax[2] - mh.bbMin[2]};
	float longest = 0, second = 0;
	for (float d : sizes) {
		if (d > longest) {
			second = longest;
			longest = d;
		} else if (d > second) {
			second = d;
		}
	}
	const bool lying = longest >= 1.8f * second;
	const float angle = float(_clock) * 0.9f;
	const Vec3 axis = lying ? Vec3 {0.70710678f, 0.70710678f, 0.0f} : Vec3 {0.0f, 1.0f, 0.0f};
	const float sa = sinf(angle * 0.5f);
	const Mat34 spin = mat34From({axis.x * sa, axis.y * sa, axis.z * sa, cosf(angle * 0.5f)}, {0, 0, 0});
	// View space to the world: the columns are the camera's right, up and forward.
	Mat34 toWorld;
	const Vec3 cols[3] = {r, u, f};
	for (int k = 0; k < 3; ++k) {
		toWorld.m[0][k] = cols[k].x;
		toWorld.m[1][k] = cols[k].y;
		toWorld.m[2][k] = cols[k].z;
		toWorld.m[k][3] = 0.0f;
	}
	Mat34 skin = mul(toWorld, mul(spin, show));
	const float c[3] = {(mh.bbMin[0] + mh.bbMax[0]) * 0.5f, (mh.bbMin[1] + mh.bbMax[1]) * 0.5f, (mh.bbMin[2] + mh.bbMax[2]) * 0.5f};
	for (int row = 0; row < 3; ++row) {
		for (int k = 0; k < 3; ++k) skin.m[row][k] *= s;
		skin.m[row][3] = -(skin.m[row][0] * c[0] + skin.m[row][1] * c[1] + skin.m[row][2] * c[2]);
	}
	const float light[3] = {1.1f, 1.08f, 1.0f};
	_showModel.drawSkinned(&skin, anchor, light);
}

void App::drawItemHud() {
	const int W = gfx::SCREEN_W, H = gfx::SCREEN_H;
	const InventoryView& v = inventory();
	if (!v.present) return;
	(void)W;
	// A name over a thing in reach, with the button that takes it: as the
	// original shows what is in focus, and what A does.
	const auto tag = [&](Vec3 at, const char* text) {
		int x, y;
		if (!projectToScreen(at, x, y)) return;
		const int w = _ui.measure(text) + 30;
		y = clamp(y - 30, 20, H - 60);
		_ui.rect(x - w / 2, y - 2, x + w / 2, y + Ui::lineHeight() + 1, 0x0A0C10A0u);
		_ui.rect(x - w / 2 + 3, y + 1, x - w / 2 + 16, y + Ui::lineHeight() - 1, 0xC4C8CCFFu);
		_ui.text(x - w / 2 + 10, y - 1, "A", 0x1C2024FFu, ui::TEXT, ui::CENTER, false);
		_ui.text(x - w / 2 + 21, y, text, ui::WHITE);
	};
	if (_lootTarget >= 0 && u32(_lootTarget) < _loot.count()) {
		const LootItem& li = _loot.item(u32(_lootTarget));
		Game& g = _npcs.game();
		const GameItemInfo* it = g.itemInfo(li.symbol);
		char name[48], line[80];
		scriptText(it ? g.text(it->description ? it->description : it->name) : "", name, sizeof(name));
		if (li.amount > 1) snprintf(line, sizeof(line), "%s ×%ld", name, static_cast<long>(li.amount));
		else snprintf(line, sizeof(line), "%s", name);
		tag({li.pos.x, li.pos.y + 40.0f, li.pos.z}, line);
	} else if (_chestTarget >= 0 && u32(_chestTarget) < _chests.count() && _chest.stage == CH_NONE) {
		const Mobs& mobs = _npcs.mobs();
		const u32 mob = _chests.mob(u32(_chestTarget));
		if (mob < mobs.count()) {
			const WorldMob& m = mobs.mob(mob);
			char line[80];
			snprintf(line, sizeof(line), _chests.locked(u32(_chestTarget)) ? tr("%s (заперт)") : "%s", chestName(u32(_chestTarget)));
			tag({m.pos[0], m.pos[1] + 75.0f, m.pos[2]}, line);
		}
	}
	if (_mobTarget >= 0 && _lootTarget < 0 && _chestTarget < 0 && _corpseTarget < 0 && _mobJob.stage == MJ_NONE && _chest.stage == CH_NONE) {
		const Mobs& mobs = _npcs.mobs();
		if (u32(_mobTarget) < mobs.count()) {
			const WorldMob& m = mobs.mob(u32(_mobTarget));
			char line[80];
			mobName(u32(_mobTarget), line, sizeof(line));
			tag({m.pos[0], m.pos[1] + 90.0f, m.pos[2]}, line);
		}
	}
	if (_doorTarget >= 0 && _lootTarget < 0 && _chestTarget < 0 && _chest.stage == CH_NONE) {
		Quat r;
		Vec3 p;
		if (_triggers.moverTransform(u32(_doorTarget), r, p)) {
			// Over the door itself (not in front of the character: by a bed next to a hut's door that read as the bed's).
			const bool open = _triggers.doorOpen(u32(_doorTarget));
			const Vec3 c = _triggers.doorCenter(u32(_doorTarget));
			tag({c.x, max(c.y, _player.feet.y + 120.0f), c.z},
			    open ? tr("Закрыть дверь") : _triggers.doorLocked(u32(_doorTarget)) ? tr("Дверь (заперта)") : tr("Открыть дверь"));
		}
	}
	if (_ladderTarget >= 0 && _mobTarget < 0 && _doorTarget < 0 && _lootTarget < 0 && _chestTarget < 0 && _chest.stage == CH_NONE) {
		Vec3 p;
		if (_player.ladderPoint(_world, _ladderTarget, _ladderTop ? _player.feet.y : _player.feet.y + 150.0f, p))
			tag({p.x, _player.feet.y + 150.0f, p.z}, _ladderTop ? tr("Спуститься по лестнице") : tr("Подняться по лестнице"));
	}
	if (_corpseTarget >= 0 && _lootTarget < 0 && _chestTarget < 0 && _chest.stage == CH_NONE) {
		const GameNpc& b = _npcs.game().npc(u32(_corpseTarget));
		char name[40], line[96];
		corpseName(u32(_corpseTarget), name, sizeof(name));
		snprintf(line, sizeof(line), tr("Обыскать: %s"), name);
		tag({b.pos[0], b.pos[1] + 60.0f, b.pos[2]}, line);
	}
	if (_lootTarget < 0 && _chestTarget < 0 && _mobTarget < 0 && _talkTarget >= 0 && !_npcs.talkView().active) {
		const GameNpc& t = _npcs.game().npc(u32(_talkTarget));
		char name[48], line[96];
		scriptText(_npcs.game().nameOf(t), name, sizeof(name));
		snprintf(line, sizeof(line), tr("Поговорить: %s"), name);
		tag({t.pos[0], t.pos[1] + 215.0f, t.pos[2]}, line);
	}
	const bool lockPage = _panel.page() == InventoryPanel::PAGE_LOCK && _chest.stage == CH_LOCK;
	if (!itemOverlay() && !lockPage) return;
	const int line = Ui::lineHeight();
	// The info box of the chosen thing: its name, the "what ... number" lines
	// on the left, the thing itself turning on the right (drawShowcase: the
	// box leaves that part clear).
	const u32 shown = _panel.selected();
	// (On the chest page's first side the things are the chest's.)
	const bool chestPage = _panel.page() == InventoryPanel::PAGE_CHEST;
	const bool chestSide = chestPage && _panel.side() == 0;
	const ItemEntry* list = chestSide ? v.chest.items : v.items;
	const u32 listCount = chestSide ? v.chest.count : v.count;
	const ItemEntry* e = nullptr;
	for (u32 i = 0; i < listCount && !lockPage; ++i)
		if (list[i].symbol == shown) e = &list[i];
	const int x0 = INFO_X0, x1 = INFO_X1, y0 = INFO_Y0, y1 = INFO_Y1, xm = INFO_X1 - MODEL_W;
	if (e) {
		_ui.rect(x0, y0, xm, y1, 0x0A0C10B4u);
		_ui.rect(xm, y0, x1, y1, 0x0A0C1040u);
		_ui.frame(x0, y0, x1, y1, 0x40444AC0u);
		int y = y0 + 3;
		_ui.text((x0 + xm) / 2, y, e->name, 0xEAE6DAFFu, ui::TEXT, ui::CENTER);
		y += line + 6;
		for (int r = 0; r < e->rowCount && y + line <= y1 - 2; ++r) {
			const ItemRow& row = e->rows[r];
			const u32 c = row.warn ? ui::RED : 0xD8D6D0FFu;
			_ui.text(x0 + 10, y, row.label, c);
			if (row.value[0]) _ui.text(xm - 8, y, row.value, c, ui::TEXT, ui::RIGHT);
			y += line;
		}
		if (!_showTried && e->icon < ICON_COUNT) _ui.icon(xm + (MODEL_W - 64) / 2, y0 + 5, e->icon, e->tint, 4, 200); // (no model: its sign)
	}
	// The hints along the bottom: the buttons and what they do now.
	int x = x0 + 4;
	const int y = H - 15;
	const auto hint = [&](const char* key, const char* what, bool on) {
		const int kw = max(_ui.measure(key) + 6, 13);
		_ui.rect(x, y + 1, x + kw, y + line, on ? 0xC4C8CCFFu : 0x5C6064FFu);
		_ui.text(x + kw / 2, y, key, 0x1C2024FFu, ui::TEXT, ui::CENTER, false);
		x += kw + 4;
		x += _ui.text(x, y, what, on ? 0xD8D6D0FFu : ui::DARK) + 10;
	};
	if (lockPage) {
		hint("L", tr("Влево"), true);
		hint("R", tr("Вправо"), true);
		hint("B", tr("Отмена"), true);
		return;
	}
	if (chestPage && v.chest.trade) {
		// The wares: what one does (buy / sell), what it comes to, the purse.
		hint("A", chestSide ? tr("Купить") : tr("Продать"), e != nullptr);
		hint("X", tr("Всю стопку"), e != nullptr);
		hint("L R", chestSide ? tr("Рюкзак") : tr("Товар"), true);
		hint("B", tr("Закрыть"), true);
		char purse[64];
		snprintf(purse, sizeof(purse), "%s: %ld", v.chest.currency[0] ? v.chest.currency : tr("Руда"), static_cast<long>(v.chest.gold));
		if (e && e->price > 0) {
			// (Under the thing that turns.)
			char price[24];
			snprintf(price, sizeof(price), tr("Цена: %ld"), static_cast<long>(e->price));
			_ui.text((xm + x1) / 2, y1 - line - 3, price, chestSide && e->price > v.chest.gold ? ui::RED : 0xE8D890FFu, ui::TEXT, ui::CENTER);
		}
		const int w = _ui.measure(purse);
		if (x + w + 12 < x1 - 4) _ui.text(x1 - 4, y, purse, 0xE8D890FFu, ui::TEXT, ui::RIGHT);
		return;
	}
	if (chestPage) {
		hint("A", tr("Переложить"), e != nullptr);
		hint("L R", _panel.side() == 0 ? tr("Рюкзак") : tr("Сундук"), true);
		if (v.chest.count > 0) hint("X", tr("Взять всё"), true);
		hint("B", tr("Закрыть"), true);
		return;
	}
	const bool can = e && (e->flags & ITEMF_ACTION) && !((e->flags & ITEMF_BLOCKED) && !(e->flags & ITEMF_EQUIPPED));
	const char* action = !e ? "—" : e->action[0] ? e->action : "—";
	if (_panel.assigning()) {
		hint("A", tr("В слот"), true);
		hint("B", tr("Отмена"), true);
	} else {
		hint("A", action, can);
		if (e && (e->flags & ITEMF_QUICK)) hint("X", tr("В слот"), true);
		hint("B", tr("Закрыть"), true);
	}
}


// ---- (fix166) ZL: the next weapon of the quick slots; X: the last one used ----

namespace {
bool armsKind(u8 k) { return k == IK_MELEE || k == IK_RANGED || k == IK_RUNE || k == IK_SCROLL; }
} // namespace

// Puts `symbol` (a melee weapon, a bow or a spell the character carries) in hand. With
// another in hand it goes away first and this one follows (heroSwitchTick).
bool App::heroArm(u32 symbol) {
	const int hi = heroIndex();
	if (hi < 0 || symbol == NONE) return false;
	Game& g = _npcs.game();
	const GameItemInfo* it = g.itemInfo(symbol);
	if (!it || g.itemCount(u32(hi), symbol) <= 0) return false;
	const u8 kind = kindOf(g, *it);
	if (!armsKind(kind)) return false;
	if (_job.stage != JOB_NONE || _cast.stage != CAST_NONE || _hero.drawing()) return false;
	if (kind == IK_RUNE || kind == IK_SCROLL) {
		if (_hero.weapon() == WEAPON_MAG) {
			// Already the stance: only another spell, as the D-Pad's choice does.
			return _spellItem == symbol || selectSpell(symbol);
		}
		if (_hero.weapon() != WEAPON_NONE) {
			_switchTo = symbol;
			_switchWait = 0;
			return heroTake(WEAPON_NONE);
		}
		return selectSpell(symbol);
	}
	GameNpc& h = g.npcMut(u32(hi));
	char why[48];
	if (!meetsNeeds(*it, why, sizeof(why))) {
		itemMessage(why);
		return false;
	}
	const bool melee = kind == IK_MELEE;
	const u32 belt = melee ? h.melee : h.ranged;
	const u8 held = _hero.weapon();
	const bool sameInHand = belt == symbol && (melee ? (held == WEAPON_1H || held == WEAPON_2H) : (held == WEAPON_BOW || held == WEAPON_CBOW));
	if (sameInHand) return true;
	if (held != WEAPON_NONE) {
		// The hands first empty (the belt changes only then: the model's weapon follows it).
		_switchTo = symbol;
		_switchWait = 0;
		return heroTake(WEAPON_NONE);
	}
	if (belt != symbol) {
		(melee ? h.melee : h.ranged) = symbol;
		++_equipVersion;
		_invStamp = 0;
	}
	char name[48];
	scriptText(g.text(it->description ? it->description : it->name), name, sizeof(name));
	const u8 mode = melee ? meleeMode() : rangedMode();
	if (!heroTake(mode)) {
		itemMessage(tr("Сейчас нельзя"));
		return false;
	}
	toast(name);
	return true;
}

void App::heroSwitchTick() {
	if (_switchTo == NONE) return;
	if (++_switchWait > 240.0f || _hero.down()) {
		_switchTo = NONE;
		return;
	}
	if (_hero.drawing()) return;
	if (_hero.weapon() != WEAPON_NONE) {
		heroTake(WEAPON_NONE);
		return;
	}
	const u32 s = _switchTo;
	_switchTo = NONE;
	heroArm(s);
}

void App::heroCycleWeapon() {
	const int hi = heroIndex();
	if (hi < 0) return;
	Game& g = _npcs.game();
	// The quick slots that hold something to fight with (food, potions and the rest are passed by).
	int list[QUICK_SLOTS];
	int n = 0;
	for (int q = 0; q < QUICK_SLOTS; ++q) {
		const u32 s = _quick[q];
		if (s == NONE || g.itemCount(u32(hi), s) <= 0) continue;
		const GameItemInfo* it = g.itemInfo(s);
		if (it && armsKind(kindOf(g, *it))) list[n++] = q;
	}
	if (!n) {
		itemMessage(tr("В быстрых слотах нет оружия"));
		return;
	}
	// Where the cycle stands: what is in hand, else what was last.
	const GameNpc& h = g.npc(u32(hi));
	u8 w = _switchTo != NONE ? u8(WEAPON_MODES) : _hero.weapon();
	u32 cur = _switchTo;
	if (w == WEAPON_NONE || w == WEAPON_FIST) w = _lastWeapon;
	if (cur == NONE) cur = w == WEAPON_MAG ? _spellItem : (w == WEAPON_BOW || w == WEAPON_CBOW) ? h.ranged : (w == WEAPON_1H || w == WEAPON_2H) ? h.melee : NONE;
	int at = -1;
	for (int k = 0; k < n; ++k)
		if (_quick[list[k]] == cur) at = k;
	const bool empty = _hero.weapon() == WEAPON_NONE || _hero.weapon() == WEAPON_FIST;
	for (int step = 1; step <= n; ++step) {
		const int k = (at + step) % n;
		const u32 s = _quick[list[k]];
		if (s == cur && !empty) continue; // (the one in hand)
		if (_switchTo != NONE && s != _switchTo) {
			_switchTo = s; // (pressed again while the hands empty: the next one waits instead)
			return;
		}
		if (heroArm(s)) return;
	}
	if (!empty) itemMessage(tr("Другого оружия в быстрых слотах нет"));
}

// X with empty hands: the last one used - the weapon, the bow or the spell.
bool App::heroTakeLast() {
	const int hi = heroIndex();
	if (hi < 0) return heroTake(meleeMode());
	Game& g = _npcs.game();
	if (_lastWeapon == WEAPON_MAG) {
		u32 item = _spellItem;
		if (item == NONE || g.itemCount(u32(hi), item) <= 0) item = nextSpell(1);
		if (item != NONE && selectSpell(item)) return true;
	} else if (_lastWeapon == WEAPON_BOW || _lastWeapon == WEAPON_CBOW) {
		const u8 r = rangedMode();
		if (r != WEAPON_NONE && heroTake(r)) return true;
	}
	return heroTake(_lastWeapon == WEAPON_1H || _lastWeapon == WEAPON_2H ? meleeMode() : _lastWeapon == WEAPON_FIST ? u8(WEAPON_FIST) : meleeMode());
}


bool App::quickArm(int slot) {
	if (slot < 0 || slot >= QUICK_SLOTS) return false;
	const int hi = heroIndex();
	const u32 s = _quick[slot];
	if (hi < 0 || s == NONE) {
		itemMessage(tr("Слот пуст"));
		return false;
	}
	Game& g = _npcs.game();
	const GameItemInfo* it = g.itemInfo(s);
	// Anything of the slots, whenever: food and potions as the touch screen's tap does (fix166).
	if (!it || !armsKind(kindOf(g, *it))) return quickUse(slot);
	if (g.itemCount(u32(hi), s) <= 0) return itemAction(s); // (says what is missing)
	_panel.invalidate();
	if (_switchTo != NONE) {
		_switchTo = s;
		return true;
	}
	return heroArm(s) || (itemMessage(tr("Сейчас нельзя")), false);
}

int App::quickSlotInHand() const {
	const int hi = heroIndex();
	if (hi < 0) return -1;
	const GameNpc& h = _npcs.game().npc(u32(hi));
	u8 w = _hero.weapon();
	if (w == WEAPON_NONE || w == WEAPON_FIST) w = _lastWeapon;
	const u32 cur = _switchTo != NONE ? _switchTo
	                : w == WEAPON_MAG ? _spellItem
	                : (w == WEAPON_BOW || w == WEAPON_CBOW) ? h.ranged
	                : (w == WEAPON_1H || w == WEAPON_2H) ? h.melee : NONE;
	for (int q = 0; q < QUICK_SLOTS; ++q)
		if (cur != NONE && _quick[q] == cur) return q;
	return -1;
}

} // namespace g3
