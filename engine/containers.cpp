// The chests: the character goes to one, opens it (its animation), and the
// bottom screen shows its things beside the pack's; a locked one takes a
// key or the pick. The rules:
//  * A chest opens with A when the character stands before it with empty
//    hands (as a thing on the ground is picked up).
//  * Locked: a key in the pack (the one the chest names) opens it at once;
//    else, with a lock pick (ITKELOCKPICK) and a combination to be had, the
//    pick - left or right as the combination goes, a wrong turn breaks the
//    pick; else it stays shut.
//  * Things go both ways: a tap chooses, a second tap moves the stack over.
//    What is worn does not go into a chest.
//  * The world goes on while a chest is open, the character stands.
//  * The dead are searched the same way (A before a body with something on
//    it): the bottom screen shows what the body carries - the people's own
//    things and the trophies the scripts' ZS_Dead gave the animals
//    (B_GiveDeathInv) - and things go one way, out. The armour a body wears
//    stays on it.
#include "lang.h"
#include "app.h"

#include <stdio.h>
#include <string.h>

namespace g3 {

namespace {

constexpr u32 NONE = SCRIPT_NONE;

// A lock's combination as OpenGothic's PlayerControl::processPickLock reads it: only the L and R count (upper or lower
// case), whatever else the object names is passed over.
bool isTurn(char c) { return c == 'l' || c == 'L' || c == 'r' || c == 'R'; }

// The n-th turn of the combination ('L' or 'R'), 0 past the last.
char lockTurn(const char* combo, u32 n) {
	for (; *combo; ++combo) {
		if (!isTurn(*combo)) continue;
		if (n-- == 0) return (*combo == 'l' || *combo == 'L') ? 'L' : 'R';
	}
	return 0;
}

u32 lockLength(const char* combo) {
	u32 n = 0;
	for (; *combo; ++combo) n += isTurn(*combo);
	return n;
}

float angleBetween(float from, float to, float t) {
	float d = to - from;
	if (!(fabsf(d) < 1.0e4f)) d = 0.0f; // (fix134: an angle gone to infinity would loop here for ever)
	while (d > 3.14159265f) d -= 6.2831853f;
	while (d < -3.14159265f) d += 6.2831853f;
	return from + d * t;
}

} // namespace

// ---- the dead ----

// The things a body carries that can be taken, by place in the list: not the
// armour it wears (its last one), which is on it.
u32 App::corpseStackCount(u32 npc) const {
	const Game& g = _npcs.game();
	if (npc >= g.npcCount()) return 0;
	const u32 armor = g.npc(npc).armorItem;
	u32 n = 0;
	for (u32 i = 0; i < g.itemStackCount(); ++i) {
		const GameItem& s = g.itemStack(i);
		if (s.npc != npc || s.count <= 0) continue;
		if (s.count <= 1 && (s.symbol == armor || s.symbol == g.npc(npc).melee || s.symbol == g.npc(npc).ranged)) continue;
		++n;
	}
	return n;
}

bool App::corpseStack(u32 npc, u32 k, u32& symbol, i32& count) const {
	const Game& g = _npcs.game();
	if (npc >= g.npcCount()) return false;
	const u32 armor = g.npc(npc).armorItem;
	u32 n = 0;
	for (u32 i = 0; i < g.itemStackCount(); ++i) {
		const GameItem& s = g.itemStack(i);
		if (s.npc != npc || s.count <= 0) continue;
		i32 c = s.count;
		// (What it wears or holds stays on it, one of each.)
		if (s.symbol == armor || s.symbol == g.npc(npc).melee || s.symbol == g.npc(npc).ranged) c -= 1;
		if (c <= 0) continue;
		if (n++ == k) {
			symbol = s.symbol;
			count = c;
			return true;
		}
	}
	return false;
}

// Takes `amount` of a thing off a body; how many came (they are the hero's to give).
i32 App::corpseTake(u32 npc, u32 symbol, i32 amount) {
	Game& g = _npcs.game();
	i32 avail = 0;
	u32 sym;
	i32 c;
	for (u32 k = 0; corpseStack(npc, k, sym, c); ++k)
		if (sym == symbol) avail = c;
	i32 n = amount < avail ? amount : avail;
	if (n <= 0) return 0;
	g.giveItems(npc, symbol, -n);
	// A weapon taken off it is no longer in its hand or on its belt.
	if (g.itemCount(npc, symbol) <= 0) {
		GameNpc& b = g.npcMut(npc);
		if (b.melee == symbol) b.melee = SCRIPT_NONE;
		if (b.ranged == symbol) b.ranged = SCRIPT_NONE;
	}
	return n;
}

// The body in reach in front of the character that has something to take.
int App::corpseNearest(bool any) const {
	const Game& g = _npcs.game();
	const int hi = heroIndex();
	int best = -1;
	float bestD = 230.0f * 230.0f;
	for (u32 i = 0; i < g.npcCount(); ++i) {
		const GameNpc& n = g.npc(i);
		if (int(i) == hi || n.down == DOWN_NONE || !n.placed || n.world != g.currentWorld()) continue;
		const float dx = n.pos[0] - _player.feet.x, dz = n.pos[2] - _player.feet.z;
		const float d = dx * dx + dz * dz;
		if (d >= bestD || fabsf(n.pos[1] - _player.feet.y) > 160.0f) continue;
		float diff = atan2f(dx, dz) - _hero.yaw();
		if (!(fabsf(diff) < 1.0e4f)) diff = 0.0f; // (fix134: an angle gone to infinity would loop here for ever)
		while (diff > 3.14159265f) diff -= 6.2831853f;
		while (diff < -3.14159265f) diff += 6.2831853f;
		// (Right at the feet it is in front whichever way one faces.)
		if (d > 60.0f * 60.0f && fabsf(diff) > 1.2f) continue;
		if (!any && !corpseStackCount(i)) continue;
		best = int(i);
		bestD = d;
	}
	return best;
}

const char* App::corpseName(u32 npc, char* out, size_t size) const {
	const Game& g = _npcs.game();
	out[0] = 0;
	if (npc < g.npcCount()) scriptText(g.nameOf(g.npc(npc)), out, size);
	if (!out[0]) snprintf(out, size, "%s", tr("Тело"));
	return out;
}

void App::corpseStart(u32 npc) {
	if (npc >= _npcs.game().npcCount()) return;
	_chest = ChestJob();
	_chest.corpse = int(npc);
	chestBegin();
}

void App::chestEnd(bool hurried) {
	if (_chest.stage == CH_NONE) return;
	if (_panel.page() == InventoryPanel::PAGE_CHEST || _panel.page() == InventoryPanel::PAGE_LOCK) _panel.openPage(InventoryPanel::PAGE_PLAY);
	if (hurried) {
		_hero.cancelUse();
		_chest = ChestJob();
	} else if (_chest.stage == CH_OPEN) {
		_hero.closeMob();
		_chest.stage = CH_CLOSING;
	} else {
		_chest = ChestJob();
	}
	_invStamp = 0;
}

// A: the door in front. Open: it shuts; locked: the key, else the pick (the chests' lock page), as
// OpenGothic's Interactive for oCMobDoor.
void App::doorUse(u32 door) {
	const int hi = heroIndex();
	if (hi < 0 || door >= _triggers.count()) return;
	if (!_triggers.doorLocked(door)) {
		_triggers.doorToggle(door, _player.feet);
		return;
	}
	Game& g = _npcs.game();
	char name[48], buf[96];
	const char* keyName = _triggers.doorKey(door);
	const u32 key = keyName[0] ? g.vm().find(keyName) : NONE;
	if (key != NONE && g.itemCount(u32(hi), key) > 0) {
		const GameItemInfo* ki = g.itemInfo(key);
		scriptText(ki ? g.text(ki->description ? ki->description : ki->name) : "", name, sizeof(name));
		_triggers.doorUnlock(door);
		_triggers.doorToggle(door, _player.feet);
		snprintf(buf, sizeof(buf), tr("Открыто ключом: %s"), name);
		itemMessage(buf, false);
		return;
	}
	const u32 pick = g.vm().findHash(hashName("ITKELOCKPICK"));
	const i32 picks = pick != NONE ? g.itemCount(u32(hi), pick) : 0;
	const char* combo = _triggers.doorPick(door);
	const bool skilled = !g.isGothic2() || g.talentSkill(u32(hi), g.constant("NPC_TALENT_PICKLOCK", 5)) != 0;
	if (picks > 0 && lockLength(combo) > 0 && !skilled) {
		itemMessage(tr("Заперто: взламывать замки вы не умеете"), true);
		return;
	}
	if (picks > 0 && lockLength(combo) > 0) {
		_chest = ChestJob();
		_chest.door = int(door);
		_chest.stage = CH_LOCK;
		_chest.progress = 0;
		_chest.picks = u8(picks > 99 ? 99 : picks);
		_panel.openPage(InventoryPanel::PAGE_LOCK);
		_invStamp = 0;
		return;
	}
	itemMessage(lockLength(combo) > 0 ? tr("Заперто: нужен ключ или отмычка") : tr("Заперто: нужен ключ"), true);
}

// A: the chest in front. A lock first, then the way to it.
void App::chestStart(u32 index) {
	const int hi = heroIndex();
	if (hi < 0 || index >= _chests.count()) return;
	Game& g = _npcs.game();
	_chest = ChestJob();
	_chest.index = index;
	if (_chests.locked(index)) {
		const u32 key = _chests.key(index);
		char name[48], buf[96];
		if (key != NONE && g.itemCount(u32(hi), key) > 0) {
			const GameItemInfo* ki = g.itemInfo(key);
			scriptText(ki ? g.text(ki->description ? ki->description : ki->name) : "", name, sizeof(name));
			_chests.unlock(index);
			snprintf(buf, sizeof(buf), tr("Открыто ключом: %s"), name);
			itemMessage(buf, false);
		} else {
			const u32 pick = g.vm().findHash(hashName("ITKELOCKPICK"));
			const i32 picks = pick != NONE ? g.itemCount(u32(hi), pick) : 0;
			const char* combo = _chests.pick(index);
			// OpenGothic's Interactive::checkUseConditions: in Gothic 1 anybody with a pick may try; in Gothic 2 only one who
			// has learnt the talent.
			const bool skilled = !g.isGothic2() || g.talentSkill(u32(hi), g.constant("NPC_TALENT_PICKLOCK", 5)) != 0;
			if (picks > 0 && lockLength(combo) > 0 && !skilled) {
				itemMessage(tr("Заперто: взламывать замки вы не умеете"), true);
				_chest = ChestJob();
				return;
			}
			if (picks > 0 && lockLength(combo) > 0) {
				_chest.stage = CH_LOCK;
				_chest.progress = 0;
				_npcs.heroUsesMob(int(_chests.mob(index))); // (seen picking a lock: OpenGothic's Interactive sends it too)
				_chest.picks = u8(picks > 99 ? 99 : picks);
				_panel.openPage(InventoryPanel::PAGE_LOCK);
				_invStamp = 0;
				return;
			}
			itemMessage(lockLength(combo) > 0 ? tr("Заперто: нужен ключ или отмычка") : tr("Заперто: нужен ключ"), true);
			_chest = ChestJob();
			return;
		}
	}
	chestBegin();
}

// Up to the chest, turned to it, and the lid.
void App::chestBegin() {
	if (_chest.corpse >= 0) {
		// Turned to the body; the hero stays where he stands.
		const GameNpc& b = _npcs.game().npc(u32(_chest.corpse));
		_chest.stage = CH_GLIDE;
		_chest.time = 0;
		_chest.from = _chest.to = _player.feet;
		_chest.yawFrom = _hero.yaw();
		_chest.yawTo = atan2f(b.pos[0] - _player.feet.x, b.pos[2] - _player.feet.z);
		return;
	}
	if (_chest.index >= _chests.count()) {
		_chest = ChestJob();
		return;
	}
	const Mobs& mobs = _npcs.mobs();
	const u32 mob = _chests.mob(_chest.index);
	_chest.stage = CH_GLIDE;
	_chest.time = 0;
	_chest.from = _player.feet;
	_chest.to = _player.feet;
	_chest.yawFrom = _hero.yaw();
	_chest.yawTo = _hero.yaw();
	if (mob < mobs.count()) {
		const float from[3] = {_player.feet.x, _player.feet.y, _player.feet.z};
		float pos[3], yaw;
		const WorldMob& m = mobs.mob(mob);
		if (mobs.place(mob, from, pos, yaw) >= 0) {
			// To the place before it, if that is a short and clear way.
			const Vec3 to = {pos[0], _player.feet.y, pos[2]};
			const float dx = to.x - _player.feet.x, dz = to.z - _player.feet.z;
			if (dx * dx + dz * dz < 160.0f * 160.0f &&
			    _world.lineFree({_player.feet.x, _player.feet.y + 90.0f, _player.feet.z}, {to.x, to.y + 90.0f, to.z}, 12.0f))
				_chest.to = to;
			_chest.yawTo = yaw;
		} else {
			_chest.yawTo = atan2f(m.pos[0] - _player.feet.x, m.pos[2] - _player.feet.z);
		}
	}
}

// Every frame: the chest in reach, and the stages of using one.
void App::chestUpdate(Input& in, float dt) {
	const int hi = heroIndex();
	_chestTarget = -1;
	_corpseTarget = -1;
	if (_chest.stage == CH_NONE) {
		if (hi < 0 || !_hero.loaded() || _hero.down() || _hero.weapon() != WEAPON_NONE || _hero.drawing() ||
		    _job.stage != JOB_NONE || _panel.open() || _player.swimming || _player.climbing || _hero.mantling() || _lootTarget >= 0 ||
		    !_player.grounded)
			return;
		if (_chests.count()) _chestTarget = _chests.nearest(_npcs.mobs(), _player.feet, _hero.yaw(), 170.0f, 1.2f);
		// (A chest first; the body only when there is none in reach.)
		if (_chestTarget < 0) _corpseTarget = corpseNearest(false);
		if ((_chestTarget >= 0 || _corpseTarget >= 0) && (in.confirm || in.attack)) {
			in.confirm = in.attack = false;
			if (_chestTarget >= 0) chestStart(u32(_chestTarget));
			else corpseStart(u32(_corpseTarget));
		} else if (_chestTarget < 0 && _corpseTarget < 0 && in.confirm && corpseNearest(true) >= 0) {
			// A body with nothing on it.
			in.confirm = false;
			itemMessage(tr("Нечего взять"));
		}
		return;
	}
	// The hands are the chest's: no blows meanwhile.
	in.attack = in.parry = in.drawWeapon = in.meleeSlot = in.fistSlot = false;
	if (hi < 0 || _hero.down() || !_hero.loaded()) {
		chestEnd(true);
		return;
	}
	if (_chest.corpse >= 0 && _chest.stage != CH_CLOSING) {
		// (It went, or has nothing left: the search is over.)
		const Game& cg = _npcs.game();
		if (u32(_chest.corpse) >= cg.npcCount() || cg.npc(u32(_chest.corpse)).down == DOWN_NONE ||
		    (_chest.stage == CH_GLIDE && !corpseStackCount(u32(_chest.corpse)))) {
			chestEnd(true);
			return;
		}
	}
	_chest.time += dt;
	switch (_chest.stage) {
	case CH_LOCK:
		// (The panel's lock page is up; a SELECT or a back arrow that closed it ends this.)
		if (_panel.page() != InventoryPanel::PAGE_LOCK) {
			_chest = ChestJob();
			_invStamp = 0;
		}
		break;
	case CH_GLIDE: {
		const float t = clamp(_chest.time / 0.35f, 0.0f, 1.0f);
		const float e = t * t * (3.0f - 2.0f * t);
		_player.feet.x = _chest.from.x + (_chest.to.x - _chest.from.x) * e;
		_player.feet.z = _chest.from.z + (_chest.to.z - _chest.from.z) * e;
		_hero.setYaw(angleBetween(_chest.yawFrom, _chest.yawTo, e));
		if (t >= 1.0f) {
			const Mobs& mobs = _npcs.mobs();
			const u32 mob = _chest.corpse >= 0 ? 0xFFFFFFFFu : _chests.mob(_chest.index);
			const char* scheme = _chest.corpse >= 0 ? "CORPSE" : mob < mobs.count() ? mobs.scheme(mob) : "";
			if (_hero.openMob(scheme)) {
				_chest.stage = CH_OPEN;
				_chest.time = 0;
				if (_chest.corpse < 0 && mob < mobs.count()) _npcs.heroUsesMob(int(mob)); // (whose chest it is: PERC_ASSESSUSEMOB)
			} else {
				itemMessage(tr("Сейчас нельзя"));
				_chest = ChestJob();
			}
		}
		break;
	}
	case CH_OPEN:
		if (!_hero.atMob()) {
			// A blow, a fall: the chest is left.
			chestEnd(true);
		} else if (_hero.mobOpen() && _panel.page() != InventoryPanel::PAGE_CHEST) {
			if (_chest.time > 0.5f && _panel.page() != InventoryPanel::PAGE_PLAY) {
				chestEnd(false); // (another page was opened over it)
			} else if (_panel.page() == InventoryPanel::PAGE_PLAY && !_chest.shown) {
				_panel.openPage(InventoryPanel::PAGE_CHEST);
				_chest.shown = true; // (shown once; closing it ends the use)
				_invStamp = 0;
			} else if (_chest.shown) {
				chestEnd(false); // (the page was closed: SELECT, B ...)
			}
		}
		break;
	case CH_CLOSING:
		if (!_hero.atMob()) {
			_chest = ChestJob();
			_invStamp = 0;
		}
		break;
	default: break;
	}
}

void App::chestMove(u32 symbol, i32 amount, bool toChest) {
	const int hi = heroIndex();
	if (hi < 0 || _chest.stage != CH_OPEN || amount <= 0) return;
	Game& g = _npcs.game();
	const GameItemInfo* it = g.itemInfo(symbol);
	if (!it) return;
	char name[48], buf[96];
	scriptText(g.text(it->description ? it->description : it->name), name, sizeof(name));
	const GameNpc& h = g.npc(u32(hi));
	if (_chest.corpse >= 0 && toChest) {
		itemMessage(tr("Тело ничего не примет"));
		return;
	}
	if (!toChest) {
		const i32 taken = _chest.corpse >= 0 ? corpseTake(u32(_chest.corpse), symbol, amount) : _chests.remove(_chest.index, symbol, amount);
		if (taken <= 0) return;
		g.giveItems(u32(hi), symbol, taken);
		if (taken > 1) snprintf(buf, sizeof(buf), tr("Взято: %s ×%ld"), name, static_cast<long>(taken));
		else snprintf(buf, sizeof(buf), tr("Взято: %s"), name);
		itemMessage(buf, false);
		++_itemStats.pickedUp;
	} else {
		const i32 have = g.itemCount(u32(hi), symbol);
		const bool worn = symbol == h.melee || symbol == h.ranged || symbol == h.armorItem || symbol == _amulet || symbol == _rings[0] ||
		                  symbol == _rings[1];
		i32 n = amount < have ? amount : have;
		if (worn) n = have > 1 ? (n < have - 1 ? n : have - 1) : 0;
		if (_job.stage != JOB_NONE && _job.symbol == symbol) n = 0;
		if (n <= 0) {
			snprintf(buf, sizeof(buf), worn ? tr("Сначала снимите: %s") : tr("Нельзя: %s"), name);
			itemMessage(buf);
			return;
		}
		if (!_chests.add(_chest.index, symbol, n)) {
			itemMessage(tr("Сундук полон"));
			return;
		}
		g.giveItems(u32(hi), symbol, -n);
		if (n > 1) snprintf(buf, sizeof(buf), tr("Положено: %s ×%ld"), name, static_cast<long>(n));
		else snprintf(buf, sizeof(buf), tr("Положено: %s"), name);
		itemMessage(buf, false);
	}
	_invStamp = 0;
}

void App::chestCommand(const PanelCommand& c) {
	if (_npcs.trading()) {
		// The wares of a trader, not a chest (trade.cpp).
		if (c.type == PANEL_CHEST_MOVE) tradeMove(c.symbol, c.amount, c.slot != 0);
		else if (c.type == PANEL_CHEST_CLOSE) _npcs.tradeClose();
		return;
	}
	switch (c.type) {
	case PANEL_CHEST_MOVE: chestMove(c.symbol, c.amount, c.slot != 0); break;
	case PANEL_CHEST_ALL:
		if (_chest.stage != CH_OPEN) break;
		if (_chest.corpse >= 0) {
			u32 sym;
			i32 cnt;
			while (corpseStack(u32(_chest.corpse), 0, sym, cnt)) {
				chestMove(sym, cnt, false);
				u32 s2;
				i32 c2;
				if (corpseStack(u32(_chest.corpse), 0, s2, c2) && s2 == sym) break;
			}
			break;
		}
		// (Each stack in turn: what is taken leaves the list.)
		while (_chests.stackCount(_chest.index) > 0) {
			const Chests::Stack st = _chests.stack(_chest.index, 0);
			chestMove(st.symbol, st.count, false);
			if (_chests.stackCount(_chest.index) > 0 && _chests.stack(_chest.index, 0).symbol == st.symbol) break;
		}
		break;
	case PANEL_CHEST_CLOSE:
		if (_chest.stage == CH_OPEN) chestEnd(false);
		break;
	case PANEL_LOCK_CANCEL:
		if (_chest.stage == CH_LOCK) {
			_chest = ChestJob();
			_invStamp = 0;
		}
		break;
	case PANEL_LOCK_PICK: {
		const int hi = heroIndex();
		if (_chest.stage != CH_LOCK || hi < 0) break;
		Game& g = _npcs.game();
		const char* combo = _chest.door >= 0 ? _triggers.doorPick(u32(_chest.door)) : _chests.pick(_chest.index);
		const char want = lockTurn(combo, _chest.progress);
		const bool right = c.slot != 0;
		if (want && ((want == 'R') == right)) {
			++_chest.progress;
			if (!lockTurn(combo, _chest.progress)) {
				// The last turn: it opens.
				g.pickLockSays(u32(hi), 1, 1);
				itemMessage(tr("Замок открыт"), false);
				_panel.openPage(InventoryPanel::PAGE_PLAY);
				if (_chest.door >= 0) {
					// A door: unlocked for good, and it swings open.
					_triggers.doorUnlock(u32(_chest.door));
					_triggers.doorToggle(u32(_chest.door), _player.feet);
					_chest = ChestJob();
				} else {
					_chests.unlock(_chest.index);
					chestBegin();
				}
			} else {
				g.pickLockSays(u32(hi), 1, 0);
				_chest.news[0] = 0;
			}
		} else {
			// A wrong turn starts the lock over, and the pick may break: OpenGothic keeps it when
			// "chance to keep" > a die of 100; the chance is the dexterity (Gothic 2) or 100 less the talent's value (Gothic 1).
			_chest.progress = 0;
			const i32 keep = g.isGothic2() ? g.attributeOf(g.npc(u32(hi)), g.constant("ATR_DEXTERITY", 5))
			                               : 100 - g.talentValue(u32(hi), g.constant("NPC_TALENT_PICKLOCK", 5));
			if (keep <= i32(g.dice(100))) {
				g.pickLockSays(u32(hi), 0, 1);
				const u32 pick = g.vm().findHash(hashName("ITKELOCKPICK"));
				if (pick != NONE) g.giveItems(u32(hi), pick, -1);
				if (_chest.picks > 0) --_chest.picks;
				if (_chest.picks == 0) {
					itemMessage(tr("Отмычка сломалась. Отмычек больше нет"), true);
					_panel.openPage(InventoryPanel::PAGE_PLAY);
					_chest = ChestJob();
				} else {
					snprintf(_chest.news, sizeof(_chest.news), "%s", tr("Отмычка сломалась"));
					++_itemStats.refused;
				}
			} else {
				g.pickLockSays(u32(hi), 0, 0);
				snprintf(_chest.news, sizeof(_chest.news), "%s", tr("Мимо — отмычка цела, начните сначала"));
			}
		}
		_invStamp = 0;
		break;
	}
	default: break;
	}
}

const char* App::chestName(u32 index) const {
	const Mobs& mobs = _npcs.mobs();
	const u32 mob = index < _chests.count() ? _chests.mob(index) : 0xFFFFFFFFu;
	const char* scheme = mob < mobs.count() ? mobs.scheme(mob) : "";
	const bool big = strstr(scheme, "BIG") != nullptr || strstr(scheme, "big") != nullptr;
	const bool small = strstr(scheme, "SMALL") != nullptr || strstr(scheme, "small") != nullptr;
	return big ? tr("Большой сундук") : small ? tr("Малый сундук") : tr("Ящик");
}

// The chest as the bottom screen shows it.
void App::chestView(const GameNpc& hero) {
	InventoryView::Chest& v = _inv.chest;
	if (_npcs.trading()) {
		tradeView(hero);
		return;
	}
	v.trade = false;
	v.open = _chest.stage == CH_OPEN;
	v.lock = _chest.stage == CH_LOCK;
	v.count = 0;
	v.length = 0;
	v.progress = _chest.progress;
	v.picks = _chest.picks;
	snprintf(v.text, sizeof(v.text), "%s", _chest.stage == CH_LOCK && _chest.news[0] ? _chest.news : "");
	v.name[0] = 0;
	if (_chest.corpse >= 0 && _chest.stage != CH_NONE) {
		const u32 body = u32(_chest.corpse);
		corpseName(body, v.name, sizeof(v.name));
		if (_chest.stage != CH_OPEN) return;
		u32 sym;
		i32 cnt;
		for (u32 k = 0; k < InventoryView::CHEST_MAX && corpseStack(body, k, sym, cnt); ++k) {
			fillEntry(v.items[v.count], sym, cnt, hero, false);
			if (v.items[v.count].symbol == sym) ++v.count;
		}
		for (u32 i = 1; i < v.count; ++i) {
			u32 k = i;
			while (k > 0 && (v.items[i].kind < v.items[k - 1].kind ||
			                 (v.items[i].kind == v.items[k - 1].kind && v.items[i].value > v.items[k - 1].value)))
				--k;
			if (k == i) continue;
			ItemEntry tmp;
			memcpy(&tmp, &v.items[i], sizeof(tmp));
			memmove(&v.items[k + 1], &v.items[k], sizeof(ItemEntry) * (i - k));
			memcpy(&v.items[k], &tmp, sizeof(tmp));
		}
		return;
	}
	if (_chest.door >= 0) {
		snprintf(v.name, sizeof(v.name), "%s", tr("Дверь"));
		v.length = u8(lockLength(_triggers.doorPick(u32(_chest.door))));
		return;
	}
	if (_chest.stage == CH_NONE || _chest.index >= _chests.count()) return;
	snprintf(v.name, sizeof(v.name), "%s", chestName(_chest.index));
	v.length = u8(lockLength(_chests.pick(_chest.index)));
	if (_chest.stage != CH_OPEN) return;
	const u32 stacks = _chests.stackCount(_chest.index);
	for (u32 k = 0; k < stacks && v.count < InventoryView::CHEST_MAX; ++k) {
		const Chests::Stack& st = _chests.stack(_chest.index, k);
		fillEntry(v.items[v.count], st.symbol, st.count, hero, false);
		if (v.items[v.count].symbol == st.symbol) ++v.count;
	}
	// By kind, like the pack (the same order).
	for (u32 i = 1; i < v.count; ++i) {
		u32 k = i;
		while (k > 0 && (v.items[i].kind < v.items[k - 1].kind ||
		                 (v.items[i].kind == v.items[k - 1].kind && v.items[i].value > v.items[k - 1].value)))
			--k;
		if (k == i) continue;
		ItemEntry tmp;
		memcpy(&tmp, &v.items[i], sizeof(tmp));
		memmove(&v.items[k + 1], &v.items[k], sizeof(ItemEntry) * (i - k));
		memcpy(&v.items[k], &tmp, sizeof(tmp));
	}
}

} // namespace g3
