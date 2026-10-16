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
	if (!(fabsf(a) < 1.0e4f)) a = 0.0f; // (fix134: an angle gone to infinity would loop here for ever)
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
//
// The player's character is noticed from every side by everybody within the
// reach of the senses (it is heard, felt, smelt: not only seen), and with no
// wall to look past when it is near (NpcConfig::hearShare of the reach).
// Only a creeping one is looked for in front.
bool Npcs::sees(const NpcActor& a, u32 other, bool inFront) const {
	if (other >= _game.npcCount() || other == a.npc) return false;
	const GameNpc& n = _game.npc(a.npc);
	const GameNpc& o = _game.npc(other);
	if (!n.placed || !o.placed || n.world != o.world) return false;
	const float dx = o.pos[0] - n.pos[0], dz = o.pos[2] - n.pos[2], dy = o.pos[1] - n.pos[1];
	const float range = _game.sensesRange(n);
	const float d2 = dx * dx + dy * dy + dz * dz;
	if (d2 > range * range) return false;
	if (inFront && !seenAllRound(n, o) && dx * dx + dz * dz > 1.0f && fabsf(wrap(atan2f(dx, dz) - n.yaw)) >= 1.75f) return false;
	// Close by it is noticed through a wall.
	if (o.hero && !_heroLink.sneaking && d2 <= range * range * _cfg.hearShare * _cfg.hearShare) return true;
	if (!_world) return true;
	// (A sphere of 12 cm stepped along: nothing a person could be hidden by slips through.
	// Two heights: a creature's eyes are not a person's, and a bush at the knee is no wall.)
	return _world->lineFree({n.pos[0], n.pos[1] + 160.0f, n.pos[2]}, {o.pos[0], o.pos[1] + 160.0f, o.pos[2]}, 12.0f) ||
	       _world->lineFree({n.pos[0], n.pos[1] + 80.0f, n.pos[2]}, {o.pos[0], o.pos[1] + 100.0f, o.pos[2]}, 12.0f);
}

// Whether `n` sees `o` whichever way it faces: everybody does the player's
// character (within the reach of the senses), unless that creeps (the stealth
// mode of the original); a creature sees any other from every side too.
bool Npcs::seenAllRound(const GameNpc& n, const GameNpc& o) const {
	if (o.hero) return !_heroLink.sneaking;
	return _game.isMonster(n);
}

// Whether the player's character is within the reach of `n`'s senses (same world, on its feet).
bool Npcs::heroNear(const GameNpc& n) const {
	const GameNpc* hero = _game.hero();
	if (!hero || !hero->placed || !n.placed || hero->world != n.world) return false;
	const float range = _game.sensesRange(n);
	const float dx = hero->pos[0] - n.pos[0], dy = hero->pos[1] - n.pos[1], dz = hero->pos[2] - n.pos[2];
	return range > 0.0f && dx * dx + dy * dy + dz * dz <= range * range;
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

// Whether a person may begin an attack on `other`. A creature goes by its scripts. A person
// does not fall on somebody it has no quarrel with: it is hostile to it (its own mood
// towards the player, or its guild's towards the other's), or was hit by it, or has it
// as the target the scripts set. Whoever is told to attack with none of these goes on
// with what he was doing (NpcConfig::calmGuard).
bool Npcs::attackJustified(const NpcActor& a, int other) const {
	if (!_cfg.calmGuard || other < 0 || u32(other) >= _game.npcCount()) return true;
	const GameNpc& n = _game.npc(a.npc);
	if (_game.isMonster(n)) return true;
	if (_game.attitude(a.npc, u32(other)) == _game.constant("ATT_HOSTILE", 0)) return true;
	if (n.lastHit == other || n.target == other) return true;
	return false;
}

void Npcs::noteAttack(const NpcActor& a, int other, u32 fn, bool allowed) {
	const GameNpc& n = _game.npc(a.npc);
	const char* state = stateName(fn);
	if (strncmp(state, "ZS_", 3) == 0) state += 3;
	char who[16], whom[16];
	snprintf(who, sizeof(who), "%.15s", _game.nameOf(n));
	snprintf(whom, sizeof(whom), "%.15s", other >= 0 && u32(other) < _game.npcCount() ? _game.nameOf(_game.npc(u32(other))) : "-");
	snprintf(_stats.lastAttack, sizeof(_stats.lastAttack), "%c %s>%s %.12s a%d", allowed ? '+' : '-', who, whom, state,
	         int(other >= 0 && u32(other) < _game.npcCount() ? _game.attitude(a.npc, u32(other)) : -1));
}

// A creature that is hit (or sees an enemy) and whose scripts do nothing about it: it goes for
// the attacker, as every creature of the game does.
bool Npcs::provoke(NpcActor& a, int attacker) {
	if (!_cfg.beastsRetaliate || !_cfg.fights || !_cfg.alive || attacker < 0 || u32(attacker) >= _game.npcCount()) return false;
	GameNpc& n = _game.npcMut(a.npc);
	if (n.down != DOWN_NONE || !a.ready || !_game.isMonster(n) || _fnAttackBeast == SCRIPT_NONE) return false;
	if (inFightState(a)) return false; // (already at it: its scripts choose whom)
	n.target = attacker;
	noteAttack(a, attacker, _fnAttackBeast, true);
	clearQueue(a);
	startState(a, _fnAttackBeast, false, "", false);
	a.other = attacker;
	a.percWait = 0.0f;
	++_stats.provoked;
	return true;
}

// A creature that had no reaction of its own to the player coming close is first sent into the
// scripts' state of threat (ZS_MM_AssessEnemy: it turns to the player, growls, waits and goes for
// him only if he stays or comes on) - the original's way - and not straight into the attack.
// False if the scripts have no such state: the caller then attacks as before.
bool Npcs::threaten(NpcActor& a, int who) {
	if (_fnAssessBeast == SCRIPT_NONE || !_game.vm().isPlainFunction(_fnAssessBeast) || who < 0 || u32(who) >= _game.npcCount()) return false;
	GameNpc& n = _game.npcMut(a.npc);
	n.target = who;
	clearQueue(a);
	startState(a, _fnAssessBeast, false, "", false);
	a.other = who;
	a.percWait = 0.0f;
	return true;
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
	const GameNpc& n = _game.npc(a.npc);
	const GameNpc* hero = _game.hero();
	const u32 heroIndex = hero ? u32(hero - &_game.npc(0)) : 0xFFFFFFFFu;
	// A creature, or anybody hostile to the player, with the player within the reach of its
	// senses looks again and again: it must not wait for its slow rhythm to notice the player.
	const bool heroInReach = heroNear(n);
	if (heroInReach && (_game.isMonster(n) || _game.attitude(a.npc, heroIndex) == _game.constant("ATT_HOSTILE", 0)))
		a.percWait = min(a.percWait, _cfg.noticeEvery);
	// (Not before its state has begun: the reactions count on what it set up.)
	// Between two states too (one has ended, the queue it left - the way back to its place - is
	// still being walked): the perceptions it had stay on, as OpenGothic has it. Only a state that
	// has not begun yet is waited for.
	if (!a.ready || (a.state && !a.started)) return;
	const u32 state0 = a.state;
	const auto still = [&]() { return a.state == state0; }; // (a reaction may have begun another)
	const World* before = _world;
	_world = &world;
	// The player.
	// (Perc_SetRange: a perception reaches as far as the scripts say - the reach of the senses, if they say nothing -
	// and no further, whatever the eyes could see; as OpenGothic's perceptionProcess has it.)
	const auto within = [&](int perc, const GameNpc& o) {
		const float r = _game.perceptionRange(perc);
		if (r < 0.0f) return true;
		const float dx = o.pos[0] - n.pos[0], dy = o.pos[1] - n.pos[1], dz = o.pos[2] - n.pos[2];
		return dx * dx + dy * dy + dz * dz <= r * r;
	};
	if (_percPlayer >= 0 && _percPlayer < Game::PERC_TYPES && a.perc[_percPlayer] && hero && within(_percPlayer, *hero) &&
	    sees(a, heroIndex, _heroLink.sneaking))
		react(a, a.perc[_percPlayer], int(heroIndex), -1);
	// The nearest enemy: anybody about it is hostile to, the player too.
	const u32 enemyFn = _percEnemy >= 0 && _percEnemy < Game::PERC_TYPES ? a.perc[_percEnemy] : 0;
	if (enemyFn && still()) {
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
			if (d2 > range * range || !within(_percEnemy, o)) return;
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
			if (sees(a, who[k], _heroLink.sneaking && who[k] == heroIndex)) {
				react(a, enemyFn, int(who[k]), -1);
				break;
			}
	}
	// A creature with the player in sight and close notices him (it turns to face him: faceHero).
	if (hero && heroInReach && _game.isMonster(n) && n.down == DOWN_NONE) {
		const float dx = hero->pos[0] - n.pos[0], dy = hero->pos[1] - n.pos[1], dz = hero->pos[2] - n.pos[2];
		const float close = _game.sensesRange(n) * 0.5f;
		const float d2 = dx * dx + dy * dy + dz * dz;
		if (d2 <= close * close) a.noticed = sees(a, heroIndex, _heroLink.sneaking);
		else if (d2 > close * close * 1.44f) a.noticed = false;
	} else {
		a.noticed = false;
	}
	// A creature that is hostile to the player, awake, with the player close and neither of its
	// perceptions answered by its scripts (nothing enabled for the player or an enemy): it
	// threatens the player first (its scripts' ZS_MM_AssessEnemy: turns, growls, waits), and goes for
	// him only if he does not back off - rather than fall on him the moment he is near, or stand about
	// until he is under its nose. Not while it is at its threat (or any other "assess" state: those
	// leave their perceptions off on purpose, and a creature there was sent to attack at once).
	if ((a.state || a.queueCount || a.phase == NpcActor::MOVING) && still() && _cfg.beastsRetaliate && hero && heroInReach && _game.isMonster(n) && n.down == DOWN_NONE && !enemyFn &&
	    !(_percPlayer >= 0 && _percPlayer < Game::PERC_TYPES && a.perc[_percPlayer]) && !inFightState(a) &&
	    _game.hitPoints(*hero) > 0 && hero->down == DOWN_NONE && _game.attitude(a.npc, heroIndex) == _game.constant("ATT_HOSTILE", 0)) {
		const float dx = hero->pos[0] - n.pos[0], dy = hero->pos[1] - n.pos[1], dz = hero->pos[2] - n.pos[2];
		const float close = _game.sensesRange(n) * 0.5f;
		const float d2 = dx * dx + dy * dy + dz * dz;
		const char* state = a.state ? stateName(a.state) : "";
		const bool busy = a.state && (strstr(state, "ASSESS") || strstr(state, "WARN") || strstr(state, "THREAT") || strstr(state, "FLEE"));
		if (d2 > close * close * 1.44f) a.warned = false; // (the player backed off: the next approach is warned again)
		if (d2 <= close * close && !busy && !strstr(state, "SLEEP") && sees(a, heroIndex, _heroLink.sneaking)) {
			if (!a.warned && threaten(a, int(heroIndex)))
				a.warned = true;
			else
				provoke(a, int(heroIndex)); // (warned already and the player is still here; or no threat state in the scripts)
		}
	} else if (!heroInReach) {
		a.warned = false;
	}
	// Somebody about with a weapon in hand (PERC_ASSESSFIGHTER); a body on
	// the ground (PERC_ASSESSBODY). The nearest that is seen.
	for (int kind = 0; kind < 2 && still(); ++kind) {
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
			if (d < bestD && sees(a, index, _heroLink.sneaking && index == heroIndex)) {
				bestD = d;
				best = int(index);
			}
		};
		for (const NpcActor& o : _actors)
			if (o.used && o.ready) consider(o.npc);
		if (hero) consider(heroIndex);
		if (best >= 0) react(a, fn, best, -1);
		if (!still()) break; // (the reaction may have ended the state)
	}
	_world = before;
}

bool Npcs::debugNearest(char* out, size_t size) const {
	const GameNpc* hero = _game.hero();
	if (!hero || !size) return false;
	const NpcActor* best = nullptr;
	float bestD = 1500.0f * 1500.0f;
	for (const NpcActor& a : _actors) {
		if (!a.used) continue;
		const GameNpc& n = _game.npc(a.npc);
		if (&n == hero || n.world != hero->world) continue;
		const float dx = n.pos[0] - hero->pos[0], dy = n.pos[1] - hero->pos[1], dz = n.pos[2] - hero->pos[2];
		const float d = dx * dx + dy * dy + dz * dz;
		if (d < bestD) {
			bestD = d;
			best = &a;
		}
	}
	if (!best) return false;
	const GameNpc& n = _game.npc(best->npc);
	const u32 heroIndex = u32(hero - &_game.npc(0));
	const bool enemy = _percEnemy >= 0 && _percEnemy < Game::PERC_TYPES && best->perc[_percEnemy];
	const char* state = best->state ? stateName(best->state) : "-";
	if (strncmp(state, "ZS_", 3) == 0) state += 3;
	const i32 hb = heroBodyState() & 0x7F;
	const char* body = hb == 3 ? "RUN" : hb == 8 ? "JMP" : hb == 5 ? "SWM" : "STD";
	snprintf(out, size, "%.10s %.14s t%d d%d a%d E%d rdy%d st%d w%d s%d %s %s", _game.nameOf(n), state, int(best->stateTime), int(sqrtf(bestD) / 100.0f),
	         int(_game.attitude(best->npc, heroIndex)), enemy ? 1 : 0, best->ready ? 1 : 0, best->started ? 1 : 0,
	         int(best->percWait * 10.0f), int(_game.sensesRange(n) / 100.0f), _cfg.perceive ? "P" : "noP", body);
	return true;
}

bool Npcs::debugLastHit(char* out, size_t size, int row) const {
	if (!size || _lastHitAge > 30.0f || !_lastHitNote[0]) return false;
	if (row == 0) {
		snprintf(out, size, "%s", _lastHitNote);
		return true;
	}
	// Live: what it is doing now.
	const NpcActor* a = _lastHitNpc >= 0 ? actorOf(u32(_lastHitNpc)) : nullptr;
	if (!a) return false;
	const char* state = a->state ? stateName(a->state) : "-";
	if (strncmp(state, "ZS_", 3) == 0) state += 3;
	snprintf(out, size, "now %.14s t%d act%d ph%d q%d w%d", state, int(a->stateTime), int(a->action.kind), int(a->phase), int(a->queueCount),
	         int(a->wait * 10.0f));
	return true;
}

// Turns the head towards whoever it looks at, at a natural pace, as far as a
// neck goes; and back again.
// A creature that has the player in sight and close turns to face him, whatever it was doing in place (a roamer between
// two spots, one that has just come back from a chase): the body turns on the spot, the way a beast turns to what it
// has noticed. Not asleep, not fighting, not walking, not in the middle of an animation of its own.
void Npcs::faceHero(NpcActor& a, float dt) {
	if (!a.noticed) return;
	const GameNpc& n = _game.npc(a.npc);
	const GameNpc* hero = _game.hero();
	if (!hero || !hero->placed || hero->world != n.world || n.down != DOWN_NONE || !_game.isMonster(n) || a.flee > 0.0f) {
		a.noticed = false;
		return;
	}
	const float dx = hero->pos[0] - n.pos[0], dz = hero->pos[2] - n.pos[2];
	const float close = _game.sensesRange(n) * 0.5f;
	if (dx * dx + dz * dz > close * close * 1.44f) {
		a.noticed = false; // (he went away)
		return;
	}
	if (a.phase != NpcActor::IDLE && a.phase != NpcActor::WAITING) return;
	if (a.action.kind != NPC_ACT_NONE && a.action.kind != NPC_ACT_WAIT) return;
	if (a.state && (inFightState(a) || strstr(stateName(a.state), "SLEEP"))) return;
	if (dx * dx + dz * dz < 1.0f) return;
	turn(a, atan2f(dx, dz), 3.6f, dt);
}

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

// Npc_SendSinglePerc(sender, receiver, perception): only the receiver reacts, with the sender as OTHER.
void Npcs::xSendSinglePerc(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const i32 perc = vm.popInt();
	const int receiver = self.npcFor(vm.popInstance());
	const int sender = self.npcFor(vm.popInstance());
	if (receiver < 0 || sender < 0 || perc < 0 || perc >= Game::PERC_TYPES || !self._cfg.perceive || !self._cfg.alive) return;
	for (NpcActor& a : self._actors)
		if (a.used && a.ready && int(a.npc) == receiver && a.perc[perc] && !(a.state && !a.started) && self._game.npc(a.npc).down == DOWN_NONE)
			self.react(a, a.perc[perc], sender, -1);
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

// "$WHATDOYOUWANT" for the voice 3 is the string SVM_3 has in WHATDOYOUWANT, "SVM_3_WHATDOYOUWANT": the name of the
// recording and of the line's text. (OpenGothic: SvmDefinitions::find.) Remembered: finding a member takes a look at
// every symbol of the scripts.
bool Npcs::svmLine(u32 npc, const char* phrase, char* out, u32 size) {
	if (!out || !size) return false;
	out[0] = 0;
	if (!phrase || !*phrase || npc >= _game.npcCount()) return false;
	if (phrase[0] != '$') {
		copyText(out, size, phrase);
		return true;
	}
	ScriptVm& vm = _game.vm();
	const i32 voice = _game.npcInt(_game.npc(npc), vm.findMember("C_NPC", "VOICE"));
	if (voice < 0) return false;
	const u32 key = hashName(phrase + 1);
	for (const SvmMemo& m : _svmMemo)
		if (m.used && m.voice == u32(voice) && m.phraseHash == key) {
			copyText(out, size, m.line);
			return out[0] != 0;
		}
	char inst[24];
	snprintf(inst, sizeof(inst), "SVM_%d", int(voice));
	const u32 sym = vm.find(inst);
	const u32 member = vm.findMember("C_SVM", phrase + 1);
	char line[sizeof(SvmMemo::line)] = "";
	if (sym != SCRIPT_NONE && member != SCRIPT_NONE && vm.symbol(sym).type == SCRIPT_INSTANCE) {
		if (ScriptInstance* svm = vm.create(sym)) {
			copyText(line, sizeof(line), vm.getString(member, 0, svm));
			vm.destroy(svm);
		}
	}
	SvmMemo& m = _svmMemo[_svmMemoNext++ % SVM_MEMO];
	m.used = true;
	m.voice = u32(voice);
	m.phraseHash = key;
	copyText(m.line, sizeof(m.line), line);
	copyText(out, size, line);
	return out[0] != 0;
}

void Npcs::xOutputSvm(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	char phrase[40];
	copyText(phrase, sizeof(phrase), vm.popString()); // (the machine runs on below: the string would not last)
	const int target = self.npcFor(vm.popInstance());
	ScriptInstance* const speakerInst = vm.popInstance();
	const int speaker = self.npcFor(speakerInst);
	if (speaker < 0) return;
	if (self.talkLineOf(speaker)) {
		// In the player's talk: shown and heard like any line of it.
		char line[44];
		if (self.svmLine(u32(speaker), phrase, line, sizeof(line))) self.queueTalkLine(speakerInst, hashName(line));
	} else if (phrase[0] == '$') {
		self.queueWorldLine(speakerInst, target, 0, phrase);
	} else if (phrase[0]) {
		self.queueWorldLine(speakerInst, target, hashName(phrase), nullptr);
	}
}

// The same said in passing, without waiting for it to be said. One at a time in the world (OpenGothic: svmBarrier).
void Npcs::xOutputSvmOverlay(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	char phrase[40];
	copyText(phrase, sizeof(phrase), vm.popString());
	(void)vm.popInstance();
	ScriptInstance* const speakerInst = vm.popInstance();
	const int speaker = self.npcFor(speakerInst);
	if (speaker < 0 || !self._cfg.alive || self._overlayBarrier > 0.0f) return;
	if (NpcActor* a = self.actorFor(speakerInst)) copyText(a->said, sizeof(a->said), phrase);
	char line[44];
	if (!self.svmLine(u32(speaker), phrase, line, sizeof(line)) || !self._speechHook.say) return;
	const float seconds = self._speechHook.say(self._speechHook.user, hashName(line), speaker, true);
	if (seconds > 0.0f) self._overlayBarrier = seconds;
	++self._stats.linesSaid;
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
	_fnAttackPerson = _game.function("ZS_ATTACK");
	_fnAttackBeast = _game.function("ZS_MM_ATTACK");
	_fnAssessBeast = _game.function("ZS_MM_ASSESSENEMY");
	refuse("ZS_ORC_ATTACK");
	const struct {
		const char* name;
		ScriptVm::External fn;
	} table[] = {{"NPC_PERCENABLE", xPercEnable},       {"NPC_PERCDISABLE", xPercDisable},
	             {"NPC_PERCEIVEALL", xPerceiveAll},     {"NPC_SENDPASSIVEPERC", xSendPassivePerc},
	             {"NPC_SENDSINGLEPERC", xSendSinglePerc},
	             {"AI_LOOKATNPC", xLookAtNpc},          {"AI_STOPLOOKAT", xStopLookAt},
	             {"AI_OUTPUTSVM", xOutputSvm},          {"AI_OUTPUTSVM_OVERLAY", xOutputSvmOverlay},
	             {"NPC_CANSEENPCFREELOS", xCanSeeNpcFreeLos}, {"AI_QUICKLOOK", xQuickLook}};
	for (const auto& e : table) vm.bind(e.name, e.fn, this);
}

} // namespace g3
