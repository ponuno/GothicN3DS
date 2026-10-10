// What the inhabitants notice (npcs.h): the scripts' perceptions, and the
// head that turns to look at somebody.
//
// A state tells the engine which perceptions its character has and which
// script function reacts to each (Npc_PercEnable). The "active" ones the
// engine checks every few seconds (Npc_SetPercTime) by itself: is the player
// about (PERC_ASSESSPLAYER), is an enemy (PERC_ASSESSENEMY - somebody the
// character is hostile to)? The "passive" ones other characters send
// (Npc_SendPassivePerc: a warning growl, a fight heard ...). The reaction is
// called with OTHER being who was perceived; what it does - turn round,
// watch, say something, begin another state - is the scripts' business.
//
// How the original does it is taken from OpenGothic (Npc::perceptionProcess,
// GameScript's Npc_PercEnable and the attitude functions), and the scripts
// of Gothic 1 themselves.
#include "npcs.h"

#include <math.h>
#include <string.h>

namespace g3 {

namespace {

const float kPi = 3.14159265f;

float wrap(float a) {
	while (a > kPi) a -= 2.0f * kPi;
	while (a < -kPi) a += 2.0f * kPi;
	return a;
}

void copyText(char* to, size_t size, const char* from) {
	size_t n = from ? strlen(from) : 0;
	if (n >= size) n = size - 1;
	if (n) memcpy(to, from, n);
	to[n] = 0;
}

} // namespace

// Whether `a` sees character `other`: within the reach of its senses, and
// nothing solid between their eyes. `inFront`: and in front of it, more or
// less (Npc_CanSeeNpc); without, all round (Npc_CanSeeNpcFreeLOS).
bool Npcs::sees(const NpcActor& a, u32 other, bool inFront) const {
	if (other >= _game.npcCount() || other == a.npc) return false;
	const GameNpc& n = _game.npc(a.npc);
	const GameNpc& o = _game.npc(other);
	if (!n.placed || !o.placed || n.world != o.world) return false;
	const float dx = o.pos[0] - n.pos[0], dz = o.pos[2] - n.pos[2], dy = o.pos[1] - n.pos[1];
	const float range = _game.sensesRange(n);
	if (dx * dx + dy * dy + dz * dz > range * range) return false;
	if (inFront && dx * dx + dz * dz > 1.0f && fabsf(wrap(atan2f(dx, dz) - n.yaw)) >= 1.75f) return false;
	// (A sphere of 12 cm stepped along: nothing a person could be hidden by slips through.)
	return !_world || _world->lineFree({n.pos[0], n.pos[1] + 160.0f, n.pos[2]}, {o.pos[0], o.pos[1] + 160.0f, o.pos[2]}, 12.0f);
}

// The states of fighting: there is no fighting yet, so the engine does not
// begin them (whoever is sent into one goes on with what he was doing).
// Every state with "ATTACK" in its name ("ZS_ATTACK", "ZS_MM_ATTACK", the
// orcs' ...).
bool Npcs::refusesState(u32 fn) const {
	for (u32 k = 0; k < _fightStateCount; ++k)
		if (_fightStates[k] == fn) return true;
	return false;
}

// Calls a reaction with OTHER (and VICTIM) as the perception has them.
void Npcs::react(NpcActor& a, u32 fn, int other, int victim) {
	if (!_game.vm().isPlainFunction(fn)) return;
	const int before = a.other;
	a.other = other;
	invoke(a, fn, victim);
	a.other = before; // (a state the reaction begins took OTHER along already)
	++_stats.perceived;
}

// The active perceptions, every few seconds.
void Npcs::perceive(NpcActor& a, const World& world) {
	// (Everybody in a rhythm of his own, so that they do not all look round in one frame.)
	a.percWait = (a.percTime > 0.0f ? a.percTime : 1.0f) * (0.9f + 0.2f * float(a.npc % 7) / 6.0f);
	// (Not before its state has begun: the reactions count on what it set up.)
	if (!a.state || !a.started || !a.ready) return;
	const World* before = _world;
	_world = &world;
	const GameNpc& n = _game.npc(a.npc);
	const GameNpc* hero = _game.hero();
	const u32 heroIndex = hero ? u32(hero - &_game.npc(0)) : 0xFFFFFFFFu;
	// The player.
	if (_percPlayer >= 0 && _percPlayer < Game::PERC_TYPES && a.perc[_percPlayer] && hero && sees(a, heroIndex, false))
		react(a, a.perc[_percPlayer], int(heroIndex), -1);
	// The nearest enemy: anybody about it is hostile to, the player too.
	const u32 enemyFn = _percEnemy >= 0 && _percEnemy < Game::PERC_TYPES ? a.perc[_percEnemy] : 0;
	if (enemyFn && a.state) {
		// The hostile ones about, nearest first; the line of sight is looked
		// at for one after the other until one is seen (it is the costly part).
		u32 who[ACTORS + 1];
		float d2s[ACTORS + 1];
		u32 count = 0;
		const float range = _game.sensesRange(n);
		const auto consider = [&](u32 index) {
			if (index == a.npc || _game.attitude(a.npc, index) != 0) return; // (ATT_HOSTILE)
			const GameNpc& o = _game.npc(index);
			if (!o.placed || o.world != n.world || _game.hitPoints(o) <= 0) return;
			const float dx = o.pos[0] - n.pos[0], dy = o.pos[1] - n.pos[1], dz = o.pos[2] - n.pos[2];
			const float d2 = dx * dx + dy * dy + dz * dz;
			if (d2 > range * range) return;
			u32 at = count++;
			for (; at > 0 && d2s[at - 1] > d2; --at) {
				d2s[at] = d2s[at - 1];
				who[at] = who[at - 1];
			}
			d2s[at] = d2;
			who[at] = index;
		};
		for (const NpcActor& o : _actors)
			if (o.used && o.ready && count < ACTORS) consider(o.npc);
		if (hero) consider(heroIndex);
		for (u32 k = 0; k < count; ++k)
			if (sees(a, who[k], false)) {
				react(a, enemyFn, int(who[k]), -1);
				break;
			}
	}
	// Somebody about with a weapon in hand (PERC_ASSESSFIGHTER); a body on
	// the ground (PERC_ASSESSBODY). The nearest that is seen.
	for (int kind = 0; kind < 2 && a.state; ++kind) {
		const i32 perc = kind == 0 ? _percFighter : _percBody;
		const u32 fn = perc >= 0 && perc < Game::PERC_TYPES ? a.perc[perc] : 0;
		if (!fn) continue;
		const float range = _game.sensesRange(n);
		int best = -1;
		float bestD = range * range;
		const auto consider = [&](u32 index) {
			if (index == a.npc) return;
			const GameNpc& o = _game.npc(index);
			if (!o.placed || o.world != n.world) return;
			if (kind == 0 && (o.weapon == WEAPON_NONE || o.down != DOWN_NONE)) return;
			if (kind == 1 && o.down != DOWN_DEAD) return;
			const float dx = o.pos[0] - n.pos[0], dy = o.pos[1] - n.pos[1], dz = o.pos[2] - n.pos[2];
			const float d = dx * dx + dy * dy + dz * dz;
			if (d < bestD && sees(a, index, false)) {
				bestD = d;
				best = int(index);
			}
		};
		for (const NpcActor& o : _actors)
			if (o.used && o.ready) consider(o.npc);
		if (hero) consider(heroIndex);
		if (best >= 0) react(a, fn, best, -1);
		if (!a.state) break; // (the reaction may have ended the state)
	}
	_world = before;
}

// Turns the head towards whoever it looks at, at a natural pace, as far as a
// neck goes; and back again.
void Npcs::turnHead(NpcActor& a, float dt) {
	float wantYaw = 0, wantPitch = 0;
	if (a.lookAt >= 0 && u32(a.lookAt) < _game.npcCount() && a.rig) {
		const GameNpc& n = _game.npc(a.npc);
		const GameNpc& o = _game.npc(u32(a.lookAt));
		if (o.placed && o.world == n.world) {
			// Where the body faces: the character's way, turned as far as its
			// animation turns the root (one sitting on a bench faces back).
			float body = n.yaw;
			const Vec3 f = quatRotate(a.anim.pose().rot[0], {0, 0, 1});
			const bool upright = f.x * f.x + f.z * f.z > 0.5f;
			if (upright) body += atan2f(f.x, f.z);
			const float dx = o.pos[0] - n.pos[0], dz = o.pos[2] - n.pos[2];
			const float dy = (o.pos[1] + 160.0f) - (n.pos[1] + 160.0f);
			const float flat = sqrtf(dx * dx + dz * dz);
			if (upright && flat > 1.0f) {
				wantYaw = clamp(wrap(atan2f(dx, dz) - body), -1.2f, 1.2f);
				wantPitch = clamp(atan2f(-dy, flat), -0.5f, 0.5f);
			}
		}
	}
	const float step = 3.0f * dt;
	a.headYaw += clamp(wantYaw - a.headYaw, -step, step);
	a.headPitch += clamp(wantPitch - a.headPitch, -step, step);
}

// ---- what the scripts call ----

void Npcs::xPercEnable(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const u32 fn = u32(vm.popInt());
	const i32 perc = vm.popInt();
	NpcActor* a = self.actorFor(vm.popInstance());
	if (a && perc >= 0 && perc < Game::PERC_TYPES) a->perc[perc] = vm.isPlainFunction(fn) ? fn : 0;
}

void Npcs::xPercDisable(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const i32 perc = vm.popInt();
	NpcActor* a = self.actorFor(vm.popInstance());
	if (a && perc >= 0 && perc < Game::PERC_TYPES) a->perc[perc] = 0;
}

// "Perceive now": the next look round comes at once.
void Npcs::xPerceiveAll(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	if (NpcActor* a = self.actorFor(vm.popInstance())) a->percWait = 0;
}

// Npc_SendPassivePerc(sender, perception, victim, other): everybody about
// the sender who has that perception, within its reach, reacts - with OTHER
// the fourth argument and VICTIM the third, as OpenGothic passes them (the
// scripts' usual "(self, PERC_ASSESSWARN, self, other)" has the others look
// at the offender, not at the one who warns). Without an OTHER nothing is sent.
void Npcs::xSendPassivePerc(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	ScriptInstance* otherInst = vm.popInstance();
	ScriptInstance* victimInst = vm.popInstance();
	const i32 perc = vm.popInt();
	const int sender = self.npcFor(vm.popInstance());
	const int other = self.npcFor(otherInst);
	if (sender < 0 || other < 0) return;
	self.sendPerception(perc, u32(sender), other, self.npcFor(victimInst));
}

void Npcs::xLookAtNpc(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	NpcAction act;
	act.kind = NPC_ACT_LOOK;
	act.i = self.npcFor(vm.popInstance());
	self.push(self.actorFor(vm.popInstance()), act);
}

void Npcs::xStopLookAt(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	NpcAction act;
	act.kind = NPC_ACT_LOOK;
	act.i = -1;
	self.push(self.actorFor(vm.popInstance()), act);
}

// A glance: the look goes to the other at once, and stays until the next
// AI_StopLookAt or look elsewhere.
void Npcs::xQuickLook(ScriptVm& vm, void* user) { xLookAtNpc(vm, user); }

void Npcs::xOutputSvm(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	NpcAction act;
	act.kind = NPC_ACT_SAY;
	copyText(act.text, sizeof(act.text), vm.popString());
	act.i = self.npcFor(vm.popInstance());
	self.push(self.actorFor(vm.popInstance()), act);
}

// The same said in passing, without waiting for it to be said.
void Npcs::xOutputSvmOverlay(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	char text[24];
	copyText(text, sizeof(text), vm.popString());
	(void)vm.popInstance();
	if (NpcActor* a = self.actorFor(vm.popInstance())) copyText(a->said, sizeof(a->said), text);
}

void Npcs::xCanSeeNpcFreeLos(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const int b = self.npcFor(vm.popInstance());
	ScriptInstance* inst = vm.popInstance();
	const NpcActor* a = self.actorFor(inst);
	vm.pushInt(a && b >= 0 && self.sees(*a, u32(b), false));
}

void Npcs::bindPerception() {
	ScriptVm& vm = _game.vm();
	const auto number = [&](const char* name, i32 otherwise) {
		const u32 sym = vm.find(name);
		return sym != SCRIPT_NONE && vm.symbol(sym).type == SCRIPT_INT && !(vm.symbol(sym).flags & SCRIPTF_MEMBER) ? vm.getInt(sym) : otherwise;
	};
	_percPlayer = number("PERC_ASSESSPLAYER", 1);
	_percEnemy = number("PERC_ASSESSENEMY", 2);
	// The states of fighting, by their names (see refusesState()).
	_fightStateCount = 0;
	const auto refuse = [&](const char* name) {
		const u32 fn = vm.find(name);
		if (fn == SCRIPT_NONE || !vm.isPlainFunction(fn) || _fightStateCount >= FIGHT_STATES) return;
		for (u32 k = 0; k < _fightStateCount; ++k)
			if (_fightStates[k] == fn) return;
		_fightStates[_fightStateCount++] = fn;
	};
	for (u32 at = 0; _stateNames && at < _stateNamesSize;) {
		const char* name = _stateNames + at;
		if (strstr(name, "ATTACK")) refuse(name);
		at += u32(strlen(name)) + 1;
	}
	refuse("ZS_ATTACK");
	refuse("ZS_MM_ATTACK");
	refuse("ZS_ORC_ATTACK");
	const struct {
		const char* name;
		ScriptVm::External fn;
	} table[] = {{"NPC_PERCENABLE", xPercEnable},       {"NPC_PERCDISABLE", xPercDisable},
	             {"NPC_PERCEIVEALL", xPerceiveAll},     {"NPC_SENDPASSIVEPERC", xSendPassivePerc},
	             {"AI_LOOKATNPC", xLookAtNpc},          {"AI_STOPLOOKAT", xStopLookAt},
	             {"AI_OUTPUTSVM", xOutputSvm},          {"AI_OUTPUTSVM_OVERLAY", xOutputSvmOverlay},
	             {"NPC_CANSEENPCFREELOS", xCanSeeNpcFreeLos}, {"AI_QUICKLOOK", xQuickLook}};
	for (const auto& e : table) vm.bind(e.name, e.fn, this);
}

} // namespace g3
