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
#include "app.h"

#include <stdio.h>
#include <string.h>

namespace g3 {

namespace {

constexpr u32 NONE = SCRIPT_NONE;

float angleBetween(float from, float to, float t) {
	float d = to - from;
	while (d > 3.14159265f) d -= 6.2831853f;
	while (d < -3.14159265f) d += 6.2831853f;
	return from + d * t;
}

} // namespace

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
			snprintf(buf, sizeof(buf), "Открыто ключом: %s", name);
			itemMessage(buf, false);
		} else {
			const u32 pick = g.vm().findHash(hashName("ITKELOCKPICK"));
			const i32 picks = pick != NONE ? g.itemCount(u32(hi), pick) : 0;
			const char* combo = _chests.pick(index);
			if (picks > 0 && combo[0]) {
				_chest.stage = CH_LOCK;
				_chest.progress = 0;
				_chest.picks = u8(picks > 99 ? 99 : picks);
				_panel.openPage(InventoryPanel::PAGE_LOCK);
				_invStamp = 0;
				return;
			}
			itemMessage(combo[0] ? "Заперто: нужен ключ или отмычка" : "Заперто: нужен ключ", true);
			_chest = ChestJob();
			return;
		}
	}
	chestBegin();
}

// Up to the chest, turned to it, and the lid.
void App::chestBegin() {
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
	if (_chest.stage == CH_NONE) {
		if (hi < 0 || !_chests.count() || !_hero.loaded() || _hero.down() || _hero.weapon() != WEAPON_NONE || _hero.drawing() ||
		    _job.stage != JOB_NONE || _panel.open() || _player.swimming || _player.climbing || _hero.mantling() || _lootTarget >= 0 ||
		    !_player.grounded)
			return;
		_chestTarget = _chests.nearest(_npcs.mobs(), _player.feet, _hero.yaw(), 170.0f, 1.2f);
		if (_chestTarget >= 0 && (in.confirm || in.attack)) {
			in.confirm = in.attack = false;
			chestStart(u32(_chestTarget));
		}
		return;
	}
	// The hands are the chest's: no blows meanwhile.
	in.attack = in.parry = in.drawWeapon = in.meleeSlot = in.fistSlot = false;
	if (hi < 0 || _hero.down() || !_hero.loaded()) {
		chestEnd(true);
		return;
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
			const u32 mob = _chests.mob(_chest.index);
			const char* scheme = mob < mobs.count() ? mobs.scheme(mob) : "";
			if (_hero.openMob(scheme)) {
				_chest.stage = CH_OPEN;
				_chest.time = 0;
			} else {
				itemMessage("Сейчас нельзя");
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
	if (!toChest) {
		const i32 taken = _chests.remove(_chest.index, symbol, amount);
		if (taken <= 0) return;
		g.giveItems(u32(hi), symbol, taken);
		if (taken > 1) snprintf(buf, sizeof(buf), "Взято: %s ×%ld", name, static_cast<long>(taken));
		else snprintf(buf, sizeof(buf), "Взято: %s", name);
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
			snprintf(buf, sizeof(buf), worn ? "Сначала снимите: %s" : "Нельзя: %s", name);
			itemMessage(buf);
			return;
		}
		if (!_chests.add(_chest.index, symbol, n)) {
			itemMessage("Сундук полон");
			return;
		}
		g.giveItems(u32(hi), symbol, -n);
		if (n > 1) snprintf(buf, sizeof(buf), "Положено: %s ×%ld", name, static_cast<long>(n));
		else snprintf(buf, sizeof(buf), "Положено: %s", name);
		itemMessage(buf, false);
	}
	_invStamp = 0;
}

void App::chestCommand(const PanelCommand& c) {
	switch (c.type) {
	case PANEL_CHEST_MOVE: chestMove(c.symbol, c.amount, c.slot != 0); break;
	case PANEL_CHEST_ALL:
		if (_chest.stage != CH_OPEN) break;
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
		const char* combo = _chests.pick(_chest.index);
		const char want = combo[_chest.progress];
		const bool right = c.slot != 0;
		if (want && ((want == 'R') == right)) {
			if (!combo[++_chest.progress]) {
				// The last turn: it opens.
				_chests.unlock(_chest.index);
				itemMessage("Замок открыт", false);
				_panel.openPage(InventoryPanel::PAGE_PLAY);
				chestBegin();
			}
		} else {
			const u32 pick = g.vm().findHash(hashName("ITKELOCKPICK"));
			if (pick != NONE) g.giveItems(u32(hi), pick, -1);
			_chest.progress = 0;
			if (_chest.picks > 0) --_chest.picks;
			if (_chest.picks == 0) {
				itemMessage("Отмычка сломалась. Отмычек больше нет", true);
				_panel.openPage(InventoryPanel::PAGE_PLAY);
				_chest = ChestJob();
			} else {
				snprintf(_chest.news, sizeof(_chest.news), "Отмычка сломалась");
				++_itemStats.refused;
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
	return big ? "Большой сундук" : small ? "Малый сундук" : "Ящик";
}

// The chest as the bottom screen shows it.
void App::chestView(const GameNpc& hero) {
	InventoryView::Chest& v = _inv.chest;
	v.open = _chest.stage == CH_OPEN;
	v.lock = _chest.stage == CH_LOCK;
	v.count = 0;
	v.length = 0;
	v.progress = _chest.progress;
	v.picks = _chest.picks;
	snprintf(v.text, sizeof(v.text), "%s", _chest.stage == CH_LOCK && _chest.news[0] ? _chest.news : "");
	v.name[0] = 0;
	if (_chest.stage == CH_NONE || _chest.index >= _chests.count()) return;
	snprintf(v.name, sizeof(v.name), "%s", chestName(_chest.index));
	v.length = u8(strlen(_chests.pick(_chest.index)));
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
