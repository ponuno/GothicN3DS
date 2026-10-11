// Things in the inhabitants' hands and in the world (npcs.h): an apple, a
// bottle, a joint held while they stand about (AI_UseItemToState), a potion
// drunk (AI_UseItem), a weapon picked up from the ground (Wld_DetectItem,
// AI_TakeItem).
//
// Using a thing is the animations of its scheme (C_ITEM.SCEMENAME), as for
// the things of the world: "T_FOOD_STAND_2_S0" brings an apple to the mouth,
// "S_FOOD_S0" holds it there, "T_FOOD_S0_2_STAND" puts it away. While it is
// held the character stands in "FOOD_S0" (NpcActor::stance), so that what
// the scripts play meanwhile ("T_FOOD_RANDOM_1") falls back into holding it,
// and getting up to walk off puts it away. How the original behaves is taken
// from OpenGothic (AiQueue, Npc::nextAiAction, Inventory::putState).
#include "loot.h"
#include "npcs.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace g3 {

namespace {

const u32 NONE = 0xFFFFFFFFu;

void upperCopy(char* to, size_t size, const char* from) {
	size_t n = 0;
	for (; from && from[n] && n + 1 < size; ++n) to[n] = from[n] >= 'a' && from[n] <= 'z' ? char(from[n] - 32) : from[n];
	to[n] = 0;
}

} // namespace

// ---- where the things lying about are ----

int Npcs::lootOf(const ScriptInstance* inst) const {
	const i32 index = _game.itemObjectLoot(inst);
	if (index < 0 || !_heldLoot || u32(index) >= _heldLoot->count()) return -1;
	const LootItem& it = _heldLoot->item(u32(index));
	return it.taken || it.symbol != inst->symbol ? -1 : index;
}

bool Npcs::lootPosition(const ScriptInstance* inst, float pos[3]) const {
	const int index = lootOf(inst);
	if (index < 0) return false;
	const LootItem& it = _heldLoot->item(u32(index));
	pos[0] = it.pos.x;
	pos[1] = it.pos.y;
	pos[2] = it.pos.z;
	return true;
}

// ---- holding and using ----

// The animation of the held item's scheme from one state to another (-1:
// standing), or of staying in one (from == to).
const Clip* Npcs::itemClip(NpcActor& a, int from, int to) {
	if (!a.heldScheme[0]) return nullptr;
	char name[64];
	if (from == to) {
		snprintf(name, sizeof(name), "S_%s_S%d", a.heldScheme, from);
	} else {
		char f[8], t[8];
		if (from < 0) snprintf(f, sizeof(f), "STAND");
		else snprintf(f, sizeof(f), "S%d", from % 100);
		if (to < 0) snprintf(t, sizeof(t), "STAND");
		else snprintf(t, sizeof(t), "S%d", to % 100);
		snprintf(name, sizeof(name), "T_%s_%s_2_%s", a.heldScheme, f, t);
	}
	return clip(a, name);
}

// The thing is out of its hand at once (no animation): a fight, a fall, its models gone.
void Npcs::dropHeldItem(NpcActor& a) {
	if (a.heldItem == NONE && a.itemState < 0) return;
	const bool holding = a.itemState >= 0;
	a.heldItem = NONE;
	a.itemState = -1;
	a.heldScheme[0] = 0;
	if (holding) {
		a.stance[0] = 0;
		fallBack(a, 0.2f);
	}
}

// Whatever else has had the character stand up (AI_StandUp, an animation
// into standing, a bench) has had it put the thing away.
void Npcs::checkHeldItem(NpcActor& a) {
	if (a.heldItem == NONE || a.action.kind == NPC_ACT_USE_ITEM) return;
	const size_t n = strlen(a.heldScheme);
	const bool holds = a.itemState >= 0 && n && strncmp(a.stance, a.heldScheme, n) == 0 && a.stance[n] == '_';
	if (!holds) {
		a.heldItem = NONE;
		a.itemState = -1;
		a.heldScheme[0] = 0;
	}
}

// A step of NPC_ACT_USE_ITEM once the animation before it has run out. True
// once the action is done.
bool Npcs::itemStep(NpcActor& a) {
	NpcAction& act = a.action;
	const u32 symbol = u32(act.i);
	const int goal = int(act.f);
	const auto enter = [&](int state) {
		// Holding it in `state` from now on (or, -1, not at all).
		a.itemState = i8(state);
		if (state >= 0) {
			a.heldItem = symbol;
			snprintf(a.stance, sizeof(a.stance), "%s_S%d", a.heldScheme, state);
		} else {
			a.heldItem = NONE;
			a.stance[0] = 0;
		}
	};
	const auto play = [&](const Clip* c) {
		a.anim.play(c, -1.0f);
		a.carried = c->moves();
		a.wait = c->duration();
		a.phase = NpcActor::PLAYING;
	};
	if (goal == -2) {
		// AI_UseItem: up to the mouth, used, put away.
		switch (a.itemPhase) {
		case 0:
			a.itemPhase = 1;
			if (a.itemState < 0) {
				const Clip* c = itemClip(a, -1, 0);
				enter(0);
				if (c) {
					play(c);
					return false;
				}
			}
			// fall through
		case 1: {
			a.itemPhase = 2;
			// Used: one less, and what using it does (a potion heals).
			if (_game.itemCount(a.npc, symbol) > 0) {
				_game.giveItems(a.npc, symbol, -1);
				const GameItemInfo* it = _game.itemInfo(symbol);
				if (it && it->onUse != SCRIPT_NONE) _game.callItemFunction(a.npc, symbol, it->onUse);
				++_stats.itemsUsed;
			}
			if (const Clip* c = itemClip(a, 0, 0)) {
				a.idle = c;
				a.anim.play(c, 0.15f);
				a.wait = c->loops() ? 0.3f : c->duration();
				a.phase = NpcActor::PLAYING;
				return false;
			}
		}
			// fall through
		case 2: {
			a.itemPhase = 3;
			const Clip* c = itemClip(a, max<int>(a.itemState, 0), -1);
			enter(-1);
			a.heldScheme[0] = 0;
			if (c) {
				play(c);
				return false;
			}
		}
			// fall through
		default:
			fallBack(a, 0.2f);
			a.phase = NpcActor::IDLE;
			return true;
		}
	}
	// AI_UseItemToState: from state to state, a step at a time.
	for (int steps = 0; steps < 8 && a.itemState != goal; ++steps) {
		const int next = a.itemState < goal ? a.itemState + 1 : a.itemState - 1;
		const Clip* c = itemClip(a, a.itemState, next);
		if (!c && a.itemState < 0 && next == 0 && !itemClip(a, 0, 0)) {
			// This body has no way of holding such a thing: it holds it all the same,
			// unseen (the scripts' randoms then find no animation and leave it be).
			enter(0);
			continue;
		}
		enter(next);
		if (c) {
			play(c);
			return false;
		}
	}
	if (goal >= 0) {
		if (const Clip* c = itemClip(a, goal, goal)) {
			a.idle = c;
			if (a.anim.current() != c) a.anim.play(c, 0.2f);
		}
	} else {
		a.heldScheme[0] = 0;
		fallBack(a, 0.2f);
	}
	a.carried = false;
	a.phase = NpcActor::IDLE;
	return true;
}

bool Npcs::beginItem(NpcActor& a, const World& world) {
	NpcAction& act = a.action;
	if (act.kind == NPC_ACT_TAKE_ITEM) {
		if (!_heldLoot || act.i < 0 || u32(act.i) >= _heldLoot->count()) return true;
		const LootItem& it = _heldLoot->item(u32(act.i));
		if (it.taken || it.symbol != act.fn) return true;
		if (a.mob >= 0 || a.stance[0]) {
			// On its feet first.
			const NpcAction copy = act;
			pushFront(a, copy);
			act = NpcAction();
			act.kind = a.mob >= 0 ? NPC_ACT_USE_MOB : NPC_ACT_STAND_UP;
			act.i = -1;
			return begin(a, world);
		}
		_net.drop(a.npc);
		a.atFree = a.atNode = -1;
		a.attached = false;
		a.bestDist = 1e30f;
		a.stuck = 0;
		a.itemPhase = 0;
		a.wait = 0;
		return false;
	}
	// NPC_ACT_USE_ITEM
	const u32 symbol = act.i >= 0 ? u32(act.i) : NONE;
	const int goal = int(act.f);
	const GameItemInfo* it = symbol != NONE ? _game.itemInfo(symbol) : nullptr;
	if (!it) return true;
	if (goal == -1 && a.heldItem == NONE && a.itemState < 0) return true; // (nothing in hand)
	if (a.heldItem != NONE && a.heldItem != symbol) {
		// Something else in hand: that goes first.
		const NpcAction copy = act;
		if (goal != -1) pushFront(a, copy);
		act = NpcAction();
		act.kind = NPC_ACT_USE_ITEM;
		act.i = i32(a.heldItem);
		act.f = -1.0f;
		return beginItem(a, world);
	}
	if (goal != -1 && a.heldItem != symbol && _game.itemCount(a.npc, symbol) <= 0) {
		++_stats.itemsMissing;
		return true;
	}
	if (a.mob >= 0 || (a.stance[0] && a.heldItem == NONE)) {
		// Up from the bench, out of the sitting, first (the original waits for standing too).
		const NpcAction copy = act;
		pushFront(a, copy);
		act = NpcAction();
		act.kind = a.mob >= 0 ? NPC_ACT_USE_MOB : NPC_ACT_STAND_UP;
		act.i = -1;
		return begin(a, world);
	}
	if (a.phase == NpcActor::MOVING) stopWalking(a);
	if (a.heldItem == NONE) {
		upperCopy(a.heldScheme, sizeof(a.heldScheme), _game.text(it->scheme));
		a.itemState = -1;
		if (goal >= 0) ++_stats.itemsHeld;
	}
	a.itemPhase = 0;
	a.wait = 0;
	return itemStep(a);
}

// AI_TakeItem: over to it, down to it, into the inventory.
bool Npcs::takeStep(NpcActor& a, const World& world, float dt) {
	(void)world;
	NpcAction& act = a.action;
	const bool there = _heldLoot && u32(act.i) < _heldLoot->count() && !_heldLoot->item(u32(act.i)).taken;
	const auto play = [&](const char* const* names, u32 count) {
		for (u32 k = 0; k < count; ++k)
			if (const Clip* c = clip(a, names[k])) {
				a.anim.play(c, -1.0f);
				a.carried = c->moves();
				a.wait = c->duration();
				a.phase = NpcActor::PLAYING;
				return true;
			}
		return false;
	};
	switch (a.itemPhase) {
	case 0: {
		if (!there) {
			stopWalking(a);
			return true;
		}
		const LootItem& it = _heldLoot->item(u32(act.i));
		const float at[3] = {it.pos.x, it.pos.y, it.pos.z};
		if (!walkTo(a, at, 60.0f, dt, true)) return false;
		stopWalking(a);
		{
			GameNpc& n = _game.npcMut(a.npc);
			const float dx = at[0] - n.pos[0], dz = at[2] - n.pos[2];
			if (dx * dx + dz * dz > 1.0f) n.yaw = atan2f(dx, dz);
		}
		a.itemPhase = 1;
		static const char* const down[] = {"C_STAND_2_IGET_1", "T_STAND_2_IGET"};
		if (play(down, 2)) return false;
	}
		// fall through
	case 1: {
		a.itemPhase = 2;
		if (there && _heldLoot->take(u32(act.i))) {
			const LootItem& it = _heldLoot->item(u32(act.i));
			_game.giveItems(a.npc, it.symbol, it.amount > 0 ? it.amount : 1);
			++_stats.itemsTaken;
		}
		static const char* const up[] = {"C_IGET_2_STAND_1", "T_IGET_2_STAND"};
		if (play(up, 2)) return false;
	}
		// fall through
	default:
		fallBack(a, 0.2f);
		a.phase = NpcActor::IDLE;
		return true;
	}
}

bool Npcs::carryOnItem(NpcActor& a, const World& world, float dt) {
	if (a.action.kind == NPC_ACT_TAKE_ITEM) {
		if (a.itemPhase == 0) return takeStep(a, world, dt);
		a.wait -= dt;
		if (a.wait > 0.0f) return false;
		return takeStep(a, world, dt);
	}
	if (a.action.kind != NPC_ACT_USE_ITEM) return true;
	a.wait -= dt;
	if (a.wait > 0.0f) return false;
	return itemStep(a);
}

// ---- what the scripts call ----

void Npcs::bindItems() {
	ScriptVm& vm = _game.vm();
	const auto number = [&](const char* name, i32 otherwise) {
		const u32 s = vm.find(name);
		return s != SCRIPT_NONE && vm.symbol(s).type == SCRIPT_INT && (vm.symbol(s).flags & SCRIPTF_CONST) ? vm.getInt(s) : otherwise;
	};
	_bsItem = number("BS_ITEMINTERACT", 14 | (1 << 15));
	_bsTakeItem = number("BS_TAKEITEM", 17);
	const struct {
		const char* name;
		ScriptVm::External fn;
	} table[] = {{"AI_USEITEM", xUseItem},           {"AI_USEITEMTOSTATE", xUseItemToState}, {"AI_TAKEITEM", xTakeItem},
	             {"AI_GOTOITEM", xGotoItem},         {"WLD_DETECTITEM", xDetectItem},        {"NPC_CANSEEITEM", xCanSeeItem},
	             {"NPC_GETDISTTOITEM", xDistToItem}};
	for (const auto& e : table) vm.bind(e.name, e.fn, this);
}

void Npcs::xUseItem(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const i32 item = vm.popInt();
	NpcActor* a = self.actorFor(vm.popInstance());
	if (!a || item < 0) return;
	NpcAction act;
	act.kind = NPC_ACT_USE_ITEM;
	act.i = item;
	act.f = -2.0f;
	self.push(a, act);
}

void Npcs::xUseItemToState(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const i32 state = vm.popInt(), item = vm.popInt();
	NpcActor* a = self.actorFor(vm.popInstance());
	if (!a || item < 0) return;
	NpcAction act;
	act.kind = NPC_ACT_USE_ITEM;
	act.i = item;
	act.f = float(clamp<i32>(state, -1, 9));
	self.push(a, act);
}

void Npcs::xTakeItem(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const ScriptInstance* item = vm.popInstance();
	NpcActor* a = self.actorFor(vm.popInstance());
	const int index = self.lootOf(item);
	if (!a || index < 0) return;
	NpcAction act;
	act.kind = NPC_ACT_TAKE_ITEM;
	act.i = index;
	act.fn = item->symbol;
	self.push(a, act);
}

void Npcs::xGotoItem(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const ScriptInstance* item = vm.popInstance();
	NpcActor* a = self.actorFor(vm.popInstance());
	NpcAction act;
	if (!a || !self.lootPosition(item, act.pos)) return;
	act.kind = NPC_ACT_GOTO_POS;
	act.i = -1;
	const GameNpc& n = self._game.npc(a->npc);
	act.pos[3] = atan2f(act.pos[0] - n.pos[0], act.pos[2] - n.pos[2]); // (facing it once there)
	self.push(a, act);
}

// Wld_DetectItem(self, ITEM_KAT_NF): the nearest thing of that kind lying
// within the reach of its senses goes into ITEM; 1 if there is one.
void Npcs::xDetectItem(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const i32 flags = vm.popInt();
	const int who = self.npcFor(vm.popInstance());
	if (who < 0 || !self._heldLoot || !flags) {
		vm.pushInt(0);
		return;
	}
	const GameNpc& n = self._game.npc(u32(who));
	const float range = self._game.sensesRange(n) > 0.0f ? self._game.sensesRange(n) : 2000.0f;
	int best = -1;
	float bestD = range * range;
	for (u32 i = 0; i < self._heldLoot->count(); ++i) {
		const LootItem& it = self._heldLoot->item(i);
		if (it.taken || it.symbol == SCRIPT_NONE) continue;
		const float dx = it.pos.x - n.pos[0], dy = it.pos.y - n.pos[1], dz = it.pos.z - n.pos[2];
		const float d = dx * dx + dy * dy + dz * dz;
		if (d >= bestD) continue;
		const GameItemInfo* info = self._game.itemInfo(it.symbol);
		if (!info || !(info->mainflag & flags)) continue;
		best = int(i);
		bestD = d;
	}
	if (best < 0) {
		vm.pushInt(0);
		return;
	}
	const LootItem& it = self._heldLoot->item(u32(best));
	ScriptInstance* obj = self._game.itemObject(it.symbol, best, it.amount > 0 ? it.amount : 1);
	if (obj) self._game.setItemVar(obj);
	vm.pushInt(obj != nullptr);
}

void Npcs::xCanSeeItem(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const ScriptInstance* item = vm.popInstance();
	const int who = self.npcFor(vm.popInstance());
	float at[3];
	if (who < 0 || !self.lootPosition(item, at)) {
		vm.pushInt(0);
		return;
	}
	const GameNpc& n = self._game.npc(u32(who));
	const float range = self._game.sensesRange(n) > 0.0f ? self._game.sensesRange(n) : 2000.0f;
	if (distance(n.pos, at) > range) {
		vm.pushInt(0);
		return;
	}
	const bool free = !self._world || self._world->lineFree({n.pos[0], n.pos[1] + 130.0f, n.pos[2]}, {at[0], at[1] + 20.0f, at[2]});
	vm.pushInt(free);
}

void Npcs::xDistToItem(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const ScriptInstance* item = vm.popInstance();
	const int who = self.npcFor(vm.popInstance());
	float at[3];
	if (who < 0 || !self.lootPosition(item, at)) {
		vm.pushInt(INT_MAX);
		return;
	}
	vm.pushInt(i32(distance(self._game.npc(u32(who)).pos, at)));
}

} // namespace g3
