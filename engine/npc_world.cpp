// Rooms and owners, and the smaller engine functions of the scripts that need the living characters, as
// OpenGothic's GameScript carries them out:
//
//   * Rooms (the BSP sectors, rooms.h): who stands in which (the player's character, the actors when they are
//     put on the ground), PERC_ASSESSENTERROOM when the player walks into another one without sneaking
//     (MoveAlgo::tick), Wld_GetPlayerPortalGuild and the rest are the game's (game_extra.cpp).
//   * Theft and things used: PERC_ASSESSTHEFT when the player picks a thing up (Npc::takeItem), PERC_ASSESSUSEMOB
//     when he begins to use a thing of the world (Interactive), and whose that thing is
//     (Npc_IsDetectedMobOwnedByNpc / ByGuild, from the world's oCMobInter owner fields).
//   * Npc_GetLookAtTarget, Npc_HasBodyFlag, Npc_PlayAni, Npc_StopAni, Npc_IsInRoutine, Wld_DetectNpcEx,
//     Npc_GetHeightToItem, AI_SetNpcsToState.
#include "npcs.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace g3 {

namespace {

void copyName(char* to, size_t size, const char* from) {
	size_t n = from ? strlen(from) : 0;
	if (n >= size) n = size - 1;
	memcpy(to, from ? from : "", n);
	to[n] = 0;
}

bool sameText(const char* a, const char* b) {
	for (;; ++a, ++b) {
		char x = *a, y = *b;
		if (x >= 'a' && x <= 'z') x = char(x - 32);
		if (y >= 'a' && y <= 'z') y = char(y - 32);
		if (x != y) return false;
		if (!x) return true;
	}
}

} // namespace

u16 Npcs::roomOf(const float pos[3]) const { return _rooms.roomAt({pos[0], pos[1], pos[2]}); }

// The player's room: a new one must hold for a few frames (in a doorway the feet go in and out).
void Npcs::trackHeroRoom(Vec3 feet) {
	const u32 hero = _game.heroIndex();
	if (hero == SCRIPT_NONE || !_rooms.count()) return;
	u32& candidate = _roomCandidate;
	u32& frames = _roomFrames;
	const u32 h = _rooms.hash(_rooms.roomAt(feet));
	if (h == _game.heroRoom()) {
		frames = 0;
		return;
	}
	if (h != candidate) {
		candidate = h;
		frames = 0;
	}
	if (++frames < 6) return;
	frames = 0;
	if (!_game.setRoom(hero, h)) return;
	++_roomsEntered;
	if (_percEnterRoom < 0) _percEnterRoom = _game.constant("PERC_ASSESSENTERROOM", -1);
	if (!_heroLink.sneaking && _percEnterRoom >= 0) sendPerception(_percEnterRoom, hero, int(hero), int(hero));
}

void Npcs::heroTook(u32 itemSymbol, i32 amount) {
	const u32 hero = _game.heroIndex();
	if (hero == SCRIPT_NONE) return;
	if (_percTheft < 0) _percTheft = _game.constant("PERC_ASSESSTHEFT", -1);
	if (_percTheft < 0) return;
	// (ITEM is the thing taken while the reactions run.)
	_game.setItemVar(_game.itemObject(itemSymbol, -1, amount > 0 ? amount : 1));
	sendPerception(_percTheft, hero, int(hero), -1);
}

void Npcs::heroUsesMob(int mob) {
	const u32 hero = _game.heroIndex();
	_heroMob = mob;
	if (hero == SCRIPT_NONE || mob < 0) return;
	if (_percUseMob < 0) _percUseMob = _game.constant("PERC_ASSESSUSEMOB", -1);
	if (_percUseMob >= 0) sendPerception(_percUseMob, hero, int(hero), -1);
}

// The thing a character uses (or the player last began to use), -1 for none.
int Npcs::mobUsedBy(int npc) const {
	if (npc < 0) return -1;
	const int held = _mobs.loaded() ? _mobs.heldBy(u32(npc)) : -1;
	if (held >= 0) return held;
	if (u32(npc) == _game.heroIndex()) return _heroMob;
	for (const NpcActor& a : _actors)
		if (a.used && a.npc == u32(npc)) return a.mob;
	return -1;
}

// Npc_IsDetectedMobOwnedByNpc(user, owner): the thing `user` uses belongs to `owner` (its OWNER is the
// owner's instance).
void Npcs::xMobOwnedByNpc(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const int owner = self.npcFor(vm.popInstance());
	const int usr = self.npcFor(vm.popInstance());
	const int mob = self.mobUsedBy(usr);
	bool owned = false;
	if (owner >= 0 && mob >= 0 && u32(mob) < self._mobs.count() && self._mobs.hasInfo()) {
		const u32 o = self._mobs.info(u32(mob)).owner;
		if (o) {
			const u32 sym = vm.find(self._mobs.infoText(o));
			owned = sym != SCRIPT_NONE && sym == self._game.npc(u32(owner)).symbol;
		}
	}
	vm.pushInt(owned);
}

// Npc_IsDetectedMobOwnedByGuild(user, guild).
void Npcs::xMobOwnedByGuild(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const i32 guild = vm.popInt();
	const int mob = self.mobUsedBy(self.npcFor(vm.popInstance()));
	bool owned = false;
	if (mob >= 0 && u32(mob) < self._mobs.count() && self._mobs.hasInfo()) {
		const u32 g = self._mobs.info(u32(mob)).ownerGuild;
		if (g) owned = self._game.constant(self._mobs.infoText(g), INT_MIN) == guild;
	}
	vm.pushInt(owned);
}

void Npcs::xLookAtTarget(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const NpcActor* a = self.actorFor(vm.popInstance());
	ScriptInstance* target = nullptr;
	if (a && a->lookAt >= 0 && u32(a->lookAt) < self._game.npcCount()) target = self._game.npc(u32(a->lookAt)).object;
	vm.pushInstance(target);
}

// Npc_HasBodyFlag(npc, flag): the flag bits (BS_FLAG_INTERRUPTABLE ...) of what its body does.
void Npcs::xHasBodyFlag(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const i32 flag = vm.popInt();
	const NpcActor* a = self.actorFor(vm.popInstance());
	vm.pushInt(a && flag != 0 && (self.bodyStateOf(*a) & flag) == flag);
}

// Npc_PlayAni(npc, name): at once, not in the queue (OpenGothic's playAnimByName).
void Npcs::xNpcPlayAni(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	char name[40];
	copyName(name, sizeof(name), vm.popString());
	NpcActor* a = self.actorFor(vm.popInstance());
	if (a && a->ready && name[0]) self.playAni(*a, name, -1);
}

// Npc_StopAni(npc, name): if that is what it plays, it stops.
void Npcs::xNpcStopAni(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	char name[40];
	copyName(name, sizeof(name), vm.popString());
	NpcActor* a = self.actorFor(vm.popInstance());
	if (!a || !a->ready || !name[0]) return;
	if (sameText(self.animationName(*a), name)) {
		if (a->phase == NpcActor::PLAYING) a->wait = 0.0f;
		self.fallBack(*a, 0.2f);
	}
}

// Npc_IsInRoutine(npc, state): in the state its daily routine has it in now.
void Npcs::xIsInRoutine(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const i32 fn = vm.popInt();
	const int n = self.npcFor(vm.popInstance());
	bool in = false;
	if (n >= 0 && fn > 0) {
		const GameNpc& g = self._game.npc(u32(n));
		const u32 routine = self._game.behaviour(g);
		if (const NpcActor* a = self.actorOf(u32(n))) in = routine == u32(fn) && a->state == u32(fn);
		else in = routine == u32(fn);
	}
	vm.pushInt(in);
}

// Wld_DetectNpcEx(npc, instance, state, guild, player): Wld_DetectNpc that can leave the player out.
void Npcs::xDetectNpcEx(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const i32 player = vm.popInt(), guild = vm.popInt(), state = vm.popInt(), instance = vm.popInt();
	const int who = self.npcFor(vm.popInstance());
	if (who < 0) {
		vm.pushInt(0);
		return;
	}
	const GameNpc& n = self._game.npc(u32(who));
	const float range = self._game.sensesRange(n);
	int found = -1;
	float best = range * range;
	const auto consider = [&](u32 index, u32 itsState) {
		const GameNpc& o = self._game.npc(index);
		if (int(index) == who || !o.placed || o.world != n.world || o.down == DOWN_DEAD) return;
		if (instance != -1 && i32(o.symbol) != instance) return;
		if (state != -1 && i32(itsState) != state) return;
		if (guild != -1 && self._game.guildOf(o) != guild) return;
		const float dx = o.pos[0] - n.pos[0], dy = o.pos[1] - n.pos[1], dz = o.pos[2] - n.pos[2];
		const float d = dx * dx + dy * dy + dz * dz;
		if (d >= best) return;
		best = d;
		found = int(index);
	};
	for (const NpcActor& a : self._actors)
		if (a.used) consider(a.npc, a.state);
	if (player != 0 && self._game.heroIndex() != SCRIPT_NONE) consider(self._game.heroIndex(), 0);
	if (found >= 0) vm.setInstance(vm.otherSymbol(), self._game.npc(u32(found)).object);
	vm.pushInt(found >= 0);
}

// Npc_GetHeightToItem(npc, item): how far above or below the thing lying in the world it is.
void Npcs::xHeightToItem(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const ScriptInstance* item = vm.popInstance();
	const int n = self.npcFor(vm.popInstance());
	float pos[3];
	if (n < 0 || !self._heldLoot || !self.lootPosition(item, pos)) {
		vm.pushInt(INT_MAX);
		return;
	}
	vm.pushInt(i32(fabsf(pos[1] - self._game.npc(u32(n)).pos[1])));
}

// AI_SetNpcsToState(npc, state, radius): everybody else about it within `radius` begins that state.
void Npcs::xSetNpcsToState(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const i32 radius = vm.popInt();
	const i32 fn = vm.popInt();
	const int who = self.npcFor(vm.popInstance());
	if (who < 0 || fn <= 0 || !vm.isPlainFunction(u32(fn))) return;
	const GameNpc& n = self._game.npc(u32(who));
	const float r = float(radius), senses = self._game.sensesRange(n);
	for (NpcActor& a : self._actors) {
		if (!a.used || !a.ready || int(a.npc) == who) continue;
		const GameNpc& o = self._game.npc(a.npc);
		if (o.down == DOWN_DEAD || o.world != n.world) continue;
		const float d = distance(o.pos, n.pos);
		if (d > r || (senses > 0.0f && d > senses)) continue;
		NpcAction act;
		act.kind = NPC_ACT_START_STATE;
		act.fn = u32(fn);
		act.i = 1;
		act.f = float(a.other);
		self.push(&a, act);
	}
}

// ---- bodies ----

namespace {
bool lying(const GameNpc& n) { return n.down != DOWN_NONE; }
} // namespace

// After its step: out of the others' bodies (they stand; it moved), out of the player's, and - off the way net -
// out of the walls (a sphere at the chest, only sideways: steps and slopes are the ground's business).
void Npcs::collide(NpcActor& a, const World& world, const float before[3]) {
	GameNpc& n = _game.npcMut(a.npc);
	if (lying(n) || a.stance[0]) return;
	const float mx = n.pos[0] - before[0], mz = n.pos[2] - before[2];
	if (mx * mx + mz * mz < 1e-4f) return; // (it did not move: what bumps into it moves itself)
	float px = n.pos[0], pz = n.pos[2];
	bool pushed = false;
	const auto away = [&](float ox, float oy, float oz, float r) {
		if (fabsf(oy - n.pos[1]) > 170.0f) return;
		const float dx = px - ox, dz = pz - oz, d2 = dx * dx + dz * dz;
		if (d2 >= r * r) return;
		const float d = sqrtf(d2);
		if (d < 0.5f) {
			// Right on top: back the way it came.
			const float bl = sqrtf(mx * mx + mz * mz);
			px = ox - mx / bl * r;
			pz = oz - mz / bl * r;
		} else {
			px = ox + dx / d * r;
			pz = oz + dz / d * r;
		}
		pushed = true;
	};
	for (const NpcActor& o : _actors) {
		if (&o == &a || !o.used || !o.ready || o.leaving) continue;
		const GameNpc& on = _game.npc(o.npc);
		if (lying(on) || on.world != n.world || (o.mob >= 0 && o.mobStage >= NpcActor::MOB_ENTERING)) continue;
		away(on.pos[0], on.pos[1], on.pos[2], a.bodyRadius + o.bodyRadius);
	}
	if (const GameNpc* hero = _game.hero())
		if (!lying(*hero)) away(hero->pos[0], hero->pos[1], hero->pos[2], a.bodyRadius + 30.0f);
	// Off the net: walls.
	if (n.weapon != WEAPON_NONE || a.flee > 0.0f || a.action.kind == NPC_ACT_GOTO_NPC) {
		Vec3 c = {px, n.pos[1] + 110.0f, pz};
		const Vec3 c0 = c;
		if (world.pushSphere(c, a.bodyRadius < 28.0f ? a.bodyRadius : 28.0f)) {
			px += c.x - c0.x;
			pz += c.z - c0.z;
			pushed = true;
		}
	}
	if (!pushed) return;
	// Never further back than where it began this step (a crowd must not shove it about).
	const float ex = px - before[0], ez = pz - before[2];
	const float e2 = ex * ex + ez * ez, m2 = mx * mx + mz * mz + 25.0f;
	if (e2 > m2) {
		const float k = sqrtf(m2 / e2);
		px = before[0] + ex * k;
		pz = before[2] + ez * k;
	}
	n.pos[0] = px;
	n.pos[2] = pz;
	++_bodyPushes;
}

bool Npcs::pushHero(Vec3& feet, float radius) const {
	if (!_cfg.bodies) return false;
	bool moved = false;
	for (const NpcActor& o : _actors) {
		if (!o.used || !o.ready || o.leaving) continue;
		const GameNpc& on = _game.npc(o.npc);
		if (lying(on) || on.world != _game.currentWorld() || on.hero) continue;
		// (One sitting, lying in bed: the thing is in the way already.)
		if (o.mob >= 0 && o.mobStage >= NpcActor::MOB_ENTERING) continue;
		if (feet.y > on.pos[1] + 170.0f || feet.y + 170.0f < on.pos[1]) continue;
		const float r = radius + o.bodyRadius;
		const float dx = feet.x - on.pos[0], dz = feet.z - on.pos[2], d2 = dx * dx + dz * dz;
		if (d2 >= r * r) continue;
		const float d = sqrtf(d2);
		if (d < 0.5f) {
			feet.x = on.pos[0] + r;
		} else {
			feet.x = on.pos[0] + dx / d * r;
			feet.z = on.pos[2] + dz / d * r;
		}
		moved = true;
	}
	return moved;
}

// ---- overlays ----

const Model* Npcs::overlayModel(GameText name) {
	if (!name) return nullptr;
	return overlayModelNamed(_game.text(name));
}

const Model* Npcs::overlayModelNamed(const char* name) {
	if (!name || !*name || !_pak) return nullptr;
	char path[96];
	snprintf(path, sizeof(path), "model/ovl.%s", name);
	const u32 hash = hashName(path);
	for (u32 k = 0; k < _overlayCount; ++k)
		if (_overlayModels[k].hash == hash) return _overlayModels[k].failed ? nullptr : &_overlayModels[k].model;
	if (_overlayCount >= OVERLAY_MODELS) return nullptr;
	OverlayModel& o = _overlayModels[_overlayCount++];
	o.hash = hash;
	o.failed = !_pak->find(hash) || !o.model.loadEntry(*_pak, hash, &_pool, false);
	if (o.failed) o.model.release();
	return o.failed ? nullptr : &o.model;
}

u32 Npcs::overlayModels(u32 npc, const Model** out, u32 max) {
	if (npc >= _game.npcCount()) return 0;
	const GameNpc& n = _game.npc(npc);
	u32 count = 0;
	for (u32 k = 0; k < n.overlayCount() && count < max; ++k)
		if (const Model* m = overlayModel(n.overlay[k])) out[count++] = m;
	return count;
}

void Npcs::bindWorld() {
	ScriptVm& vm = _game.vm();
	static const struct {
		const char* name;
		ScriptVm::External fn;
	} table[] = {{"NPC_ISDETECTEDMOBOWNEDBYNPC", xMobOwnedByNpc},
	             {"NPC_ISDETECTEDMOBOWNEDBYGUILD", xMobOwnedByGuild},
	             {"NPC_GETLOOKATTARGET", xLookAtTarget},
	             {"NPC_HASBODYFLAG", xHasBodyFlag},
	             {"NPC_PLAYANI", xNpcPlayAni},
	             {"NPC_STOPANI", xNpcStopAni},
	             {"NPC_ISINROUTINE", xIsInRoutine},
	             {"WLD_DETECTNPCEX", xDetectNpcEx},
	             {"NPC_GETHEIGHTTOITEM", xHeightToItem},
	             {"AI_SETNPCSTOSTATE", xSetNpcsToState}};
	for (const auto& e : table) vm.bind(e.name, e.fn, this);
}

} // namespace g3
