// The life of the inhabitants (npcs.h): states, the queue of actions the
// scripts fill, and carrying those actions out - walking the way net,
// turning, playing animations.
//
// How the original does all this is taken from OpenGothic (Npc::tickRoutine,
// Npc::nextAiAction, the AI_ and Npc_ externals of its GameScript).
#include "npcs.h"

#include "fight.h"
#include "ui.h"
#include "sys.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace g3 {

namespace {

const float kPi = 3.14159265f;

// The scripts' body states (BS_...): what the body is doing, and two flags.
const i32 kBsInterruptable = 1 << 15, kBsFreeHands = 1 << 16;
const i32 kBsStand = kBsInterruptable | kBsFreeHands, kBsWalk = 1 | kBsInterruptable, kBsRun = 3;
const i32 kBsSit = 11 | kBsFreeHands, kBsLie = 12;

// How close "at a point" is, cm (the original's MAX_AI_USE_DISTANCE).
const float kAtPoint = 165.0f;

float wrapAngle(float a) {
	while (a > kPi) a -= 2.0f * kPi;
	while (a < -kPi) a += 2.0f * kPi;
	return a;
}

void upperCopy(char* to, size_t size, const char* from) {
	size_t n = 0;
	for (; from && from[n] && n + 1 < size; ++n) to[n] = from[n] >= 'a' && from[n] <= 'z' ? char(from[n] - 32) : from[n];
	to[n] = 0;
}

bool startsWith(const char* s, const char* prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

void copyName(char* to, size_t size, const char* from) {
	size_t n = 0;
	for (; from && from[n] && n + 1 < size; ++n) to[n] = from[n];
	to[n] = 0;
}

} // namespace

float Npcs::distance(const float a[3], const float b[3]) {
	const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
	return sqrtf(x * x + y * y + z * z);
}

NpcActor* Npcs::actorFor(ScriptInstance* inst) {
	const int index = _game.indexOf(inst);
	if (index < 0 || !_cfg.alive) return nullptr;
	for (NpcActor& a : _actors)
		if (a.used && a.npc == u32(index)) return &a;
	return nullptr;
}

const char* Npcs::doing(const NpcActor& a) const {
	if (!a.ready) return "loading";
	if (a.mob >= 0 && a.mobStage == NpcActor::MOB_IN) return _mobs.scheme(u32(a.mob));
	switch (a.phase) {
	case NpcActor::MOVING: return a.walkMode == NpcActor::WALK ? "walks" : "runs";
	case NpcActor::TURNING: return "turns";
	case NpcActor::PLAYING: return "acts";
	case NpcActor::WAITING: return "waits";
	default: break;
	}
	return a.stance[0] ? a.stance : "stands";
}

// ---- animations ----

// The clip of that name, read from the archive if need be.
const Clip* Npcs::clip(NpcActor& a, const char* name, bool force) {
	if (!a.rig) return nullptr;
	Model& m = a.rig->model;
	const u32 hash = hashName(name);
	if (!m.hasClip(hash)) return nullptr;
	if (!_pak) return m.clip(hash);
	const u32 before = _clipBytes;
	const Clip* c = m.fetch(*_pak, hash, _clipBytes, _cfg.clipBudget, _frame + 1);
	// No room: what has not been played for longest goes (never what somebody is in
	// the middle of), until the clip and those that follow it are in. Without this the
	// memory for animations filled up in the course of a game and stayed full: whoever
	// came to fall then had no T_DEAD, and lay down only after a reload.
	for (int tries = 0; tries < 24 && m.chainWaiting(hash); ++tries) {
		if (!evictClip()) break;
		const Clip* again = m.fetch(*_pak, hash, _clipBytes, _cfg.clipBudget, _frame + 1);
		if (!c) c = again;
	}
	if (force && m.chainWaiting(hash)) {
		const Clip* again = m.fetch(*_pak, hash, _clipBytes, 0xFFFFFFFFu, _frame + 1);
		if (!c) c = again;
	}
	if (_clipBytes != before) ++_stats.clips;
	if (!c) ++_stats.clipsRefused;
	if (c) {
		bool known = false;
		for (const Clip* k : a.named) known = known || k == c;
		if (!known) {
			a.named[a.nextName] = c;
			upperCopy(a.names[a.nextName], sizeof(a.names[0]), name);
			a.nextName = u8((a.nextName + 1) % NpcActor::NAMES);
		}
	}
	return c;
}

bool Npcs::evictClip() {
	// Pinned: what each one is playing, falls back into, strikes with.
	const Clip* pinned[3 * ACTORS];
	u32 n = 0;
	for (const NpcActor& a : _actors) {
		if (!a.used || n + 3 > 3 * ACTORS) continue;
		if (a.anim.current()) pinned[n++] = a.anim.current();
		if (a.idle) pinned[n++] = a.idle;
		if (a.blow.clip) pinned[n++] = static_cast<const Clip*>(a.blow.clip);
	}
	Piece* where = nullptr;
	int which = -1;
	u32 oldest = 0;
	for (Piece& p : _pieces) {
		if (!p.hash || p.failed) continue;
		u32 stamp = 0;
		const int i = p.model.oldestClip(pinned, n, _frame + 1, stamp);
		if (i >= 0 && (which < 0 || stamp < oldest)) {
			where = &p;
			which = i;
			oldest = stamp;
		}
	}
	if (!where) return false;
	_clipBytes -= min(_clipBytes, where->model.dropClip(which));
	++_stats.clipsEvicted;
	return true;
}

const char* Npcs::animationName(const NpcActor& a) const {
	const Clip* c = a.anim.current();
	for (int k = 0; c && k < NpcActor::NAMES; ++k)
		if (a.named[k] == c) return a.names[k];
	return "";
}

const char* Npcs::stateName(u32 fn) const {
	if (!fn || !_stateNames || fn >= _game.vm().symbolCount()) return "";
	const u32 hash = _game.vm().symbol(fn).nameHash;
	for (u32 at = 0; at < _stateNamesSize;) {
		const char* name = _stateNames + at;
		if (hashName(name) == hash) return name;
		at += u32(strlen(name)) + 1;
	}
	return "";
}

const NpcActor* Npcs::nearest() const {
	const NpcActor* best = nullptr;
	for (const NpcActor& a : _actors)
		if (a.used && a.ready && (!best || a.dist2 < best->dist2)) best = &a;
	return best;
}

// A clip of the way the character moves: `pattern` with the mode's word put
// in ("S_%sL" is "S_WALKL", "S_RUNL", "S_FISTWALKL" ...). With `orOtherMode`
// the other mode's will do if the model has none of its own (a creature
// that cannot walk runs; a step from standing into a walk is no way to
// begin a run).
const Clip* Npcs::modeClip(NpcActor& a, const char* pattern, bool orOtherMode) {
	const char* const words[2] = {a.walkMode == NpcActor::WALK ? "WALK" : "RUN", a.walkMode == NpcActor::WALK ? "RUN" : "WALK"};
	// With a weapon in hand its word goes in front ("S_1HRUNL"); a model
	// without such an animation goes as it would without.
	const u8 weapon = a.npc < _game.npcCount() ? _game.npc(a.npc).weapon : u8(WEAPON_NONE);
	const char* const prefixes[2] = {weapon != WEAPON_NONE && !a.fist ? weaponWord(weapon) : a.fist ? "FIST" : "", a.fist ? "FIST" : ""};
	for (int p = 0; p < (prefixes[0] != prefixes[1] ? 2 : 1); ++p)
		for (int k = 0; k < (orOtherMode ? 2 : 1); ++k) {
			char mode[16], name[64];
			snprintf(mode, sizeof(mode), "%s%s", prefixes[p], words[k]);
			snprintf(name, sizeof(name), pattern, mode, mode);
			if (const Clip* c = clip(a, name)) return c;
		}
	return nullptr;
}

// Its models have just come into memory: it stands there.
void Npcs::wake(NpcActor& a) {
	if (!a.rig) return;
	const Model& m = a.rig->model;
	a.fist = !m.hasClip(hashName("S_RUN")) && !m.hasClip(hashName("S_WALK")) &&
	         (m.hasClip(hashName("S_FISTRUN")) || m.hasClip(hashName("S_FISTWALK")));
	for (const Clip*& k : a.named) k = nullptr; // (of the model it had before, if any)
	// What it perceives and looks at comes with the state it begins anew.
	for (u32& p : a.perc) p = 0;
	a.lookAt = -1;
	a.headYaw = a.headPitch = 0;
	a.idle = modeClip(a, "S_%s", true);
	a.anim.play(a.idle, 0.0f);
	a.blow.reset();
	a.drawTo = 0xFF;
	a.stepCount = a.stepAt = 0;
	a.flee = a.faiWait = 0;
	a.stumble = -1.0f;
	a.noLying = false;
	a.slump = 0.0f;
	a.heardWeapon = a.npc < _game.npcCount() ? _game.npc(a.npc).weapon : u8(WEAPON_NONE);
	if (a.npc < _game.npcCount() && _game.npc(a.npc).down == DOWN_DEAD) {
		// Where it fell, as it fell.
		const Clip* lying = clip(a, "S_DEADB", true);
		snprintf(a.stance, sizeof(a.stance), "DEADB");
		if (!lying) {
			lying = clip(a, "S_DEAD", true);
			snprintf(a.stance, sizeof(a.stance), "DEAD");
		}
		if (lying) {
			a.idle = lying;
			a.anim.play(lying, 0.0f);
		}
		a.noLying = !lying;
		a.slump = lying ? 0.0f : 1.0f;
	}
	// Everybody in a rhythm of his own.
	a.anim.advance(float(a.npc % 97) * 0.37f);
	(void)a.anim.takeMotion();
	a.anim.takeEvents(nullptr, 0); // (the steps of the time before it was seen make no sound)
	// (What it was doing is forgotten with the model: it starts its state over.)
	a.phase = NpcActor::IDLE;
	a.action = NpcAction();
	a.carried = false;
	a.settle = 0;
	releaseMob(a);
	// (Nor does it hold anything.)
	if (a.heldScheme[0] && strncmp(a.stance, a.heldScheme, strlen(a.heldScheme)) == 0) a.stance[0] = 0;
	a.heldItem = 0xFFFFFFFFu;
	a.itemState = -1;
	a.heldScheme[0] = 0;
}

// Back into what it stands (or sits) in.
void Npcs::fallBack(NpcActor& a, float blend) {
	// The dead stay as they fell: nothing gets them to their feet.
	if (a.npc < _game.npcCount() && _game.npc(a.npc).down == DOWN_DEAD) return;
	const Clip* c = nullptr;
	if (a.stance[0]) {
		char name[40];
		snprintf(name, sizeof(name), "S_%s", a.stance);
		c = clip(a, name);
	}
	if (!c) c = modeClip(a, "S_%s", true);
	if (!c) c = a.idle;
	if (c) a.idle = c;
	a.anim.play(c, blend);
}

bool Npcs::playAni(NpcActor& a, const char* rawName, i32 bodyState) {
	char name[40];
	upperCopy(name, sizeof(name), rawName);
	const Clip* c = clip(a, name);
	if (!c) {
		if (a.rig && !a.rig->model.hasClip(hashName(name))) ++_stats.clipsMissing;
		return false;
	}
	if (a.anim.current() == c && !c->loops())
		a.anim.restart(0.1f);
	else
		a.anim.play(c, -1.0f);
	if (bodyState >= 0) a.bodyState = bodyState;
	// A creature's threat: the growl is the model script's event on the clip. If the clip has none
	// (or the archive lacks the sound it names), the creature's own sound of that name is tried.
	if (strcmp(name, "T_WARN") == 0 && c->sfxCount() == 0) {
		const GameNpc& n = _game.npc(a.npc);
		if (_game.isMonster(n) && n.look.script) {
			char base[24], names[192];
			upperCopy(base, sizeof(base), _game.text(n.look.script));
			snprintf(names, sizeof(names), "%s_WARN|%s_GROWL|%s_THREAT|%s_AGGRO|%s_ANGRY|%s_ATTACK", base, base, base, base, base, base);
			emitSound(a.npc, names, 1.0f);
		}
	}
	// What it is in from now on, as the animation's name says: "T_STAND_2_SIT"
	// leads into sitting, "S_SIT" is sitting.
	const char* to = nullptr;
	if (startsWith(name, "T_")) {
		for (const char* at = name; (at = strstr(at, "_2_")) != nullptr; at += 3) to = at + 3;
	} else if (startsWith(name, "S_")) {
		to = name + 2;
	}
	if (to) {
		const char* const standing[] = {"STAND", "RUN", "WALK", "SNEAK", "FISTRUN", "FISTWALK"};
		bool stands = false;
		for (const char* s : standing) stands = stands || strcmp(to, s) == 0;
		if (stands) {
			a.stance[0] = 0;
			if (bodyState < 0) a.bodyState = -1;
		} else {
			upperCopy(a.stance, sizeof(a.stance), to);
		}
	}
	a.carried = c->moves();
	if (c->loops()) return false;
	a.wait = c->duration();
	return true;
}

bool Npcs::standUp(NpcActor& a) {
	a.bodyState = -1;
	if (!a.stance[0]) {
		// Out of whatever gesture it is in the middle of.
		if (a.anim.current() != a.idle || a.anim.finished()) fallBack(a, 0.2f);
		return false;
	}
	char name[48];
	snprintf(name, sizeof(name), "T_%s_2_STAND", a.stance);
	a.stance[0] = 0;
	const Clip* c = clip(a, name);
	if (!c) {
		fallBack(a, 0.3f);
		return false;
	}
	a.anim.play(c, -1.0f);
	a.wait = c->duration();
	return true;
}

void Npcs::startWalking(NpcActor& a) {
	const Clip* c = modeClip(a, "T_%s_2_%sL", false);
	if (c)
		a.anim.play(c, -1.0f);
	else if ((c = modeClip(a, "S_%sL", true)) != nullptr)
		a.anim.play(c, 0.2f);
	a.carried = true;
	a.settle = 0;
	a.phase = NpcActor::MOVING;
}

void Npcs::stopWalking(NpcActor& a) {
	if (a.phase != NpcActor::MOVING) {
		a.phase = NpcActor::IDLE;
		return;
	}
	a.phase = NpcActor::IDLE;
	if (const Clip* c = modeClip(a, "T_%sL_2_%s", false)) {
		a.anim.play(c, -1.0f);
		a.settle = min(c->duration(), 1.0f); // its last steps still carry it
	} else {
		fallBack(a, 0.25f);
		a.carried = false;
	}
}

i32 Npcs::bodyStateOf(const NpcActor& a) const {
	const u8 down = _game.npc(a.npc).down;
	if (down == DOWN_DEAD) return _bsDead;
	if (down == DOWN_UNCONSCIOUS) return _bsUnconscious;
	if (a.blow.clip && a.anim.current() == a.blow.clip) return _bsHit;
	if (a.stumble > 0.0f) return _bsStumble;
	if (const Clip* c = a.anim.current())
		if (c->fight() && c->fight()->parry[0] != CLIP_NO_FRAME) return _bsParade;
	if (a.bodyState >= 0) return a.bodyState;
	// A thing in hand, or being picked up.
	if (a.action.kind == NPC_ACT_TAKE_ITEM && a.itemPhase >= 1) return _bsTakeItem;
	if (a.heldItem != 0xFFFFFFFFu && a.itemState >= 0) return _bsItem;
	if (a.mob >= 0 && a.mobStage >= NpcActor::MOB_ENTERING) {
		// By the kind of thing, as the scripts' lists sort them.
		const char* kind = _mobs.scheme(u32(a.mob));
		const auto listed = [kind](const char* list) {
			const size_t n = strlen(kind);
			for (const char* at = list; *at;) {
				const char* end = strchr(at, ',');
				const size_t len = end ? size_t(end - at) : strlen(at);
				if (len == n && strncmp(at, kind, n) == 0) return true;
				if (!end) break;
				at = end + 1;
			}
			return false;
		};
		// (In a bed it is "busy with a thing", not "lying": that is what the
		// original's scripts wait for - ZS_SleepBed_Loop gets up and goes to
		// sit somewhere unless its body is BS_MOBINTERACT, the orcs' ZS_Orc_TA
		// waits for that very state "while he lies in bed". BS_LIE is for
		// those the scripts lay down themselves, with AI_PlayAniBS.)
		return listed(_mobSit) ? _bsSit : listed(_mobLie) ? _bsMob : listed(_mobClimb) ? _bsClimb : listed(_mobBusy) ? _bsMob : _bsMobInterrupt;
	}
	if (a.phase == NpcActor::MOVING) return a.walkMode == NpcActor::WALK ? kBsWalk : kBsRun;
	if (strcmp(a.stance, "SIT") == 0) return kBsSit;
	if (startsWith(a.stance, "SLEEP")) return _bsLie;
	return kBsStand;
}

i32 Npcs::heroBodyState() const {
	const GameNpc* hero = _game.hero();
	if (hero && hero->down == DOWN_DEAD) return _bsDead;
	if (hero && hero->down == DOWN_UNCONSCIOUS) return _bsUnconscious;
	if (_heroLink.swimming) return _game.constant("BS_SWIM", 5);
	if (_heroLink.jumping) return _game.constant("BS_JUMP", 8);
	if (_heroLink.running) return kBsRun;
	return kBsStand;
}

// ---- things to use ----

// The animation of using the thing the actor is at that leads from one state
// to another (-1: standing by it), or - `from` == `to` - is staying in one:
// "T_BENCH_STAND_2_S0", "T_BENCH_S0_2_S1", "S_BENCH_S1". From a place in
// front of or behind the thing the names have that in them
// ("T_BEDHIGH_FRONT_STAND_2_S0"), if the model has such.
const Clip* Npcs::mobClip(NpcActor& a, int from, int to, char* nameOut) {
	if (a.mob < 0) return nullptr;
	const char* kind = _mobs.scheme(u32(a.mob));
	const char* const tags[2] = {a.mobKind == MOB_SLOT_FRONT ? "_FRONT" : a.mobKind == MOB_SLOT_BACK ? "_BACK" : "", ""};
	char state[2][12];
	const int which[2] = {from, to};
	for (int k = 0; k < 2; ++k) {
		if (which[k] < 0)
			snprintf(state[k], sizeof(state[k]), "STAND");
		else
			snprintf(state[k], sizeof(state[k]), "S%u", unsigned(which[k]) % 100u);
	}
	for (int k = 0; k < 2; ++k) {
		if (k == 1 && !tags[0][0]) break;
		char name[96];
		if (from == to)
			snprintf(name, sizeof(name), "S_%.40s%s_%s", kind, tags[k], state[0]);
		else
			snprintf(name, sizeof(name), "T_%.40s%s_%s_2_%s", kind, tags[k], state[0], state[1]);
		if (const Clip* c = clip(a, name)) {
			if (nameOut) upperCopy(nameOut, 40, name);
			return c;
		}
	}
	return nullptr;
}

// The nearest free thing of a kind that the character can see - the
// original uses a thing only if "the character sees it and it is free":
// a bench behind the wall of the hut is not his. Seeing is from the eyes to
// the middle of whoever would stand at the thing's place (a little lower than
// the eyes, so that a low beam overhead hides nothing).
int Npcs::findMob(u32 npc, const char* kind, int except) const {
	const GameNpc& n = _game.npc(npc);
	int found[8];
	const u32 count = _mobs.candidates(n.pos, kind, npc, except, found, 8);
	if (!_world) return count ? found[0] : -1;
	for (u32 k = 0; k < count; ++k) {
		float at[3], yaw;
		_mobs.place(u32(found[k]), n.pos, at, yaw);
		if (_world->lineFree({n.pos[0], n.pos[1] + 130.0f, n.pos[2]}, {at[0], at[1] + 80.0f, at[2]})) return found[k];
	}
	return -1;
}

// The actor has nothing to do with its thing any more.
void Npcs::releaseMob(NpcActor& a) {
	if (a.mob < 0 && a.mobStage == NpcActor::MOB_NONE) return;
	const bool wasIn = a.mobStage >= NpcActor::MOB_ENTERING;
	_mobs.drop(a.npc);
	a.mob = -1;
	a.mobStage = NpcActor::MOB_NONE;
	a.mobState = -1;
	if (wasIn) {
		// Back on its own feet, where it stood before it got in.
		GameNpc& n = _game.npcMut(a.npc);
		memcpy(n.pos, a.mobPos, sizeof(a.mobPos));
		a.stance[0] = 0;
		a.carried = false;
		a.grounded = false;
		a.bodyState = -1;
	}
}

// A step of using a thing (NPC_ACT_USE_MOB). True once the action is done.
bool Npcs::useMob(NpcActor& a, const World& world, float dt) {
	GameNpc& n = _game.npcMut(a.npc);
	if (a.mob < 0 || u32(a.mob) >= _mobs.count()) {
		releaseMob(a);
		return true;
	}
	const int goal = a.action.i < 0 ? -1 : min<int>(a.action.i, _mobs.mob(u32(a.mob)).states);
	switch (a.mobStage) {
	case NpcActor::MOB_COMING:
		if (!walkTo(a, a.mobPos, 20.0f, dt, true)) return false;
		stopWalking(a);
		a.mobStage = NpcActor::MOB_TURNING;
		return false;
	case NpcActor::MOB_TURNING: {
		// The last steps first; then round to face the way the thing is used,
		// shuffling onto the very spot the animations begin at.
		const bool faces = turn(a, a.mobYaw, 3.6f, dt);
		if (a.settle > 0.0f) return false;
		const float dx = a.mobPos[0] - n.pos[0], dz = a.mobPos[2] - n.pos[2];
		const float off = sqrtf(dx * dx + dz * dz), step = 90.0f * dt;
		if (off > step) {
			n.pos[0] += dx / off * step;
			n.pos[2] += dz / off * step;
			return false;
		}
		if (!faces) return false;
		{
			// On the floor it has walked over to the place, wherever the
			// place's own height is: that was found on the world's ground
			// only (or not at all, and is then the node's, a metre up), and
			// a chair upstairs stands on a floor that is an object.
			float y;
			Vec3 normal;
			if (world.rayDown({a.mobPos[0], n.pos[1] + 60.0f, a.mobPos[2]}, 160.0f, y, normal)) a.mobPos[1] = y;
			else a.mobPos[1] = n.pos[1];
		}
		memcpy(n.pos, a.mobPos, sizeof(a.mobPos));
		n.yaw = a.mobYaw;
		a.carried = false;
		a.mobStage = NpcActor::MOB_ENTERING;
		a.mobState = -1;
		a.wait = 0;
		a.phase = NpcActor::PLAYING;
		++_stats.mobsUsed;
		return false;
	}
	case NpcActor::MOB_ENTERING:
	case NpcActor::MOB_IN:
		a.wait -= dt;
		if (a.wait > 0.0f) return false;
		// (Asked to go back a state or more: the same steps the other way.)
		for (int steps = 0; steps < 8 && a.mobState != goal; ++steps) {
			const int next = a.mobState < goal ? a.mobState + 1 : a.mobState - 1;
			if (next < 0) break; // (leaving is not this action's business)
			const Clip* c = mobClip(a, a.mobState, next, nullptr);
			if (!c && a.mobState < 0) {
				// This body has no way of using such a thing (or there is no
				// memory for the animation): it leaves it be.
				++_stats.mobsFailed;
				a.mobFailed = a.mob;
				releaseMob(a);
				fallBack(a, 0.2f);
				a.phase = NpcActor::IDLE;
				return true;
			}
			a.mobState = i8(next);
			if (!c) continue; // no step of its own between the two: it simply is there
			a.anim.play(c, -1.0f);
			a.carried = c->moves();
			a.wait = c->duration();
			a.mobStage = NpcActor::MOB_ENTERING;
			return false;
		}
		// There: it stays in that state.
		a.mobStage = NpcActor::MOB_IN;
		a.carried = false;
		{
			char name[40] = "";
			if (const Clip* c = mobClip(a, a.mobState, a.mobState, name)) {
				upperCopy(a.stance, sizeof(a.stance), name + 2); // ("S_BENCH_S1" is being in "BENCH_S1")
				a.idle = c;
				if (a.anim.current() != c) a.anim.play(c, 0.2f);
			}
		}
		a.phase = NpcActor::IDLE;
		return true;
	case NpcActor::MOB_LEAVING:
		a.wait -= dt;
		if (a.wait > 0.0f) return false;
		for (int steps = 0; steps < 8 && a.mobState >= 0; ++steps) {
			const int next = a.mobState - 1;
			const Clip* c = mobClip(a, a.mobState, next, nullptr);
			a.mobState = i8(next);
			if (!c) continue;
			a.anim.play(c, -1.0f);
			a.carried = c->moves();
			a.wait = c->duration();
			return false;
		}
		releaseMob(a);
		fallBack(a, 0.2f);
		a.phase = NpcActor::IDLE;
		return true;
	default:
		releaseMob(a);
		return true;
	}
}

// ---- the queue ----

void Npcs::clearQueue(NpcActor& a) {
	a.queueHead = a.queueCount = 0;
	if (a.phase == NpcActor::MOVING) stopWalking(a);
	if (a.mob >= 0 && a.mobStage < NpcActor::MOB_ENTERING) releaseMob(a); // (only on its way there)
	// In the middle of getting in or out: it stays where the step in hand
	// leads; what comes next takes it from there.
	if (a.mob >= 0 && a.mobStage != NpcActor::MOB_IN) a.mobStage = NpcActor::MOB_IN;
	a.action = NpcAction();
	a.phase = NpcActor::IDLE;
	a.wait = 0;
}

bool Npcs::push(NpcActor* a, const NpcAction& act) {
	if (!a) return false;
	if (a->queueCount >= NpcActor::QUEUE) {
		++_stats.queueFull;
		return false;
	}
	a->queue[(a->queueHead + a->queueCount) % NpcActor::QUEUE] = act;
	++a->queueCount;
	return true;
}

void Npcs::pushFront(NpcActor& a, const NpcAction& act) {
	if (a.queueCount >= NpcActor::QUEUE) {
		++_stats.queueFull;
		return;
	}
	a.queueHead = u8((a.queueHead + NpcActor::QUEUE - 1) % NpcActor::QUEUE);
	a.queue[a.queueHead] = act;
	++a.queueCount;
}

// ---- states ----

// Calls a function of a state with SELF and OTHER meaning what they mean to it.
i32 Npcs::invoke(NpcActor& a, u32 fn, int victim) {
	ScriptVm& vm = _game.vm();
	if (!vm.isPlainFunction(fn)) return 0;
	const u32 self = vm.selfSymbol(), other = vm.otherSymbol();
	ScriptInstance* const selfBefore = vm.instanceOf(self);
	ScriptInstance* const otherBefore = vm.instanceOf(other);
	ScriptInstance* const victimBefore = vm.instanceOf(_victim);
	vm.setInstance(self, _game.npc(a.npc).object);
	vm.setInstance(other, a.other >= 0 && u32(a.other) < _game.npcCount() ? _game.npc(u32(a.other)).object : nullptr);
	vm.setInstance(_victim, victim >= 0 && u32(victim) < _game.npcCount() ? _game.npc(u32(victim)).object : nullptr);
	const double before = sys::timeMs();
	const i32 result = vm.callInt(fn);
	_stats.scriptMs += float(sys::timeMs() - before);
	vm.setInstance(self, selfBefore);
	vm.setInstance(other, otherBefore);
	vm.setInstance(_victim, victimBefore);
	++_stats.stateCalls;
	return result;
}

void Npcs::clearState(NpcActor& a, bool finalize) {
	// Out of being knocked out: up again (the state's end has it stand up).
	if (a.state && a.state == _fnUnconscious && _game.npc(a.npc).down == DOWN_UNCONSCIOUS) _game.npcMut(a.npc).down = DOWN_NONE;
	if (a.state && a.started) {
		if (finalize && a.endFn != SCRIPT_NONE) invoke(a, a.endFn);
		a.prevState = a.state;
	}
	a.state = 0;
	a.started = false;
	a.loopFn = a.endFn = SCRIPT_NONE;
}

void Npcs::abortActions(NpcActor& a) {
	// (clearQueue also ends a walk with the steps that stop it, drops the action in hand and
	// lets go of a thing it was only on its way to.)
	clearQueue(a);
	a.wait = 0.0f;
	a.stuck = 0.0f;
	if (a.anim.finished() || (a.anim.current() && a.anim.current()->loops() && a.phase == NpcActor::PLAYING)) fallBack(a, 0.2f);
}

void Npcs::startState(NpcActor& a, u32 fn, bool finalizeOld, const char* wp, bool byRoutine) {
	ScriptVm& vm = _game.vm();
	if (!vm.isPlainFunction(fn) || vm.symbol(fn).count != 0) return;
	if (!byRoutine && !_cfg.fights && refusesState(fn)) {
		// Fighting is switched off: whoever the scripts send into a fight
		// goes on with what he was doing.
		++_stats.fightsRefused;
		return;
	}
	GameNpc& n = _game.npcMut(a.npc);
	if (n.down == DOWN_DEAD && fn != _fnDead) return; // (the dead do nothing more)
	if (a.state == fn) {
		// Asked for again: it begins anew.
		if (finalizeOld) a.started = false;
		if (wp && *wp) _game.setWayPoint(n, wp);
		return;
	}
	clearQueue(a);
	clearState(a, finalizeOld);
	// A new state begins with a clean slate of perceptions: its beginning switches on the ones
	// it wants (as the original and OpenGothic have it). Otherwise those of the state before
	// stay on - a guard in ZS_Attack still had his routine's PERC_ASSESSFIGHTER, which took him
	// out of the fight into "put that weapon away" again and again.
	for (u32& p : a.perc) p = 0;
	if (wp && *wp) _game.setWayPoint(n, wp);
	a.state = fn;
	a.mobFailed = -1;
	const ScriptState* st = vm.stateOf(fn);
	a.loopFn = st ? st->loop : SCRIPT_NONE;
	a.endFn = st ? st->end : SCRIPT_NONE;
	a.started = false;
	if (byRoutine) {
		// How long it lasts: until the routine wants something else; a
		// creature's own state four hours. A state begun from within a state
		// (go to bed, then sleep) lasts as long as the one it came from.
		a.routineFn = _game.behaviour(n);
		a.routineAt = n.want;
		a.stateHours = 0;
	}
	a.routineState = byRoutine;
}

void Npcs::tickState(NpcActor& a) {
	const GameNpc& n = _game.npc(a.npc);
	if (n.down == DOWN_DEAD) {
		// Dead: its state of being dead begins once, and lasts.
		if (a.state && !a.started) {
			a.started = true;
			a.stateTime = 0;
			invoke(a, a.state);
		}
		return;
	}
	if (!a.state) {
		const u32 fn = _game.behaviour(n);
		if (!fn) return;
		startState(a, fn, false, nullptr, true);
		if (!a.state) return;
	}
	if (!a.started) {
		a.started = true;
		a.stateTime = 0;
		a.loopWait = 0;
		invoke(a, a.state);
		return;
	}
	if (a.loopWait > 0.0f) return;
	// (In a fight the loop decides the next move as soon as one is done, as
	// the original's does every frame; otherwise it need not be asked as often.)
	a.loopWait = n.weapon != WEAPON_NONE ? 0.0f : max(a.percTime, 0.25f);
	// (A state without a loop of its own simply lasts.)
	i32 loop = a.loopFn != SCRIPT_NONE ? invoke(a, a.loopFn) : 0;
	// Its hours are over?
	if (n.routineCount ? (_game.behaviour(n) != a.routineFn || n.want != a.routineAt) : a.stateHours >= 4.0f) loop = 1;
	if (loop != 0) {
		clearState(a, true);
		a.other = -1;
	}
}

// ---- carrying actions out ----

void Npcs::attachTo(NpcActor& a, int node, int freePoint) {
	a.atNode = node;
	a.atFree = freePoint;
	a.attached = false;
	if (freePoint >= 0 && u32(freePoint) < _net.freeCount()) {
		a.attachYaw = _net.freePoint(u32(freePoint)).yaw;
		a.attached = true;
	} else if (node >= 0 && u32(node) < _net.nodeCount()) {
		a.attachYaw = _net.node(u32(node)).yaw;
		a.attached = true;
	}
}

// Lays out the way to a node of the net: to the net first if the character
// is off it, then along the connections.
void Npcs::planWay(NpcActor& a, int node) {
	const GameNpc& n = _game.npc(a.npc);
	a.goalNode = node;
	a.pathCount = a.pathAt = 0;
	a.bestDist = 1e30f;
	a.stuck = 0;
	if (node < 0 || u32(node) >= _net.nodeCount()) return;
	int start = a.atNode;
	if (start < 0 || u32(start) >= _net.nodeCount() || distance(n.pos, _net.node(u32(start)).pos) > 150.0f)
		start = _net.nearestNode(n.pos);
	if (start < 0 || start == node) {
		a.path[0] = u16(node);
		a.pathCount = 1;
		return;
	}
	const u32 total = _net.path(u32(start), u32(node), a.path + 1, NpcActor::PATH - 1);
	if (!total) {
		// No way through the net: straight there.
		++_stats.noPath;
		a.path[0] = u16(node);
		a.pathCount = 1;
		return;
	}
	++_stats.paths;
	const u32 have = min<u32>(total, NpcActor::PATH - 1);
	// Standing on the first node already, or past it on the way to the
	// second: no need to go back to it.
	bool skipStart = distance(n.pos, _net.node(u32(start)).pos) < 150.0f;
	if (!skipStart) {
		const float* s = _net.node(u32(start)).pos;
		const float* next = _net.node(a.path[1]).pos;
		const float ex = next[0] - s[0], ez = next[2] - s[2];
		const float along = (n.pos[0] - s[0]) * ex + (n.pos[2] - s[2]) * ez;
		skipStart = along > 0.0f && along < ex * ex + ez * ez;
	}
	if (skipStart) {
		memmove(a.path, a.path + 1, have * sizeof(u16));
		a.pathCount = u8(have);
	} else {
		a.path[0] = u16(start);
		a.pathCount = u8(have + 1);
	}
}

// Somebody else stands on that spot.
bool Npcs::taken(const NpcActor& a, const float pos[3]) const {
	for (const NpcActor& o : _actors) {
		if (!o.used || &o == &a || o.phase == NpcActor::MOVING) continue;
		const GameNpc& n = _game.npc(o.npc);
		const float dx = n.pos[0] - pos[0], dz = n.pos[2] - pos[2];
		if (dx * dx + dz * dz < 50.0f * 50.0f) return true;
	}
	return false;
}

bool Npcs::turn(NpcActor& a, float yaw, float rate, float dt) {
	GameNpc& n = _game.npcMut(a.npc);
	const float err = wrapAngle(yaw - n.yaw), step = rate * dt;
	if (fabsf(err) <= max(step, 0.03f)) {
		n.yaw = wrapAngle(yaw);
		return true;
	}
	n.yaw = wrapAngle(n.yaw + (err > 0.0f ? step : -step));
	return false;
}

// A step towards `target`. True once it is within `radius` of it.
bool Npcs::walkTo(NpcActor& a, const float target[3], float radius, float dt, bool last) {
	GameNpc& n = _game.npcMut(a.npc);
	const float dx = target[0] - n.pos[0], dz = target[2] - n.pos[2];
	const float d = sqrtf(dx * dx + dz * dz);
	if (d < radius) return true;
	const float want = atan2f(dx, dz), err = wrapAngle(want - n.yaw);
	const bool run = a.walkMode != NpcActor::WALK;
	if (a.phase != NpcActor::MOVING) {
		// Facing away from where it is to go: turn on the spot first.
		if (fabsf(err) > 1.3f) {
			a.phase = NpcActor::TURNING;
			turn(a, want, 3.6f, dt);
			return false;
		}
		startWalking(a);
	}
	const float maxTurn = (run ? 6.3f : 5.2f) * dt;
	n.yaw = wrapAngle(n.yaw + clamp(err, -maxTurn, maxTurn));
	// The walk's own animation carries it (tick()); without one it glides.
	const Clip* c = a.anim.current();
	if (a.anim.finished() || !c) {
		if ((c = modeClip(a, "S_%sL", true)) != nullptr) a.anim.play(c, 0.1f);
	}
	if (!c || !c->moves()) {
		const float step = min((run ? 420.0f : 130.0f) * dt, d);
		n.pos[0] += sinf(n.yaw) * step;
		n.pos[2] += cosf(n.yaw) * step;
	}
	// Not getting any closer (a turn too wide, a clip that goes nowhere):
	// after a while it is simply there.
	if (d < a.bestDist - 1.0f) {
		a.bestDist = d;
		a.stuck = 0;
	} else {
		a.stuck += dt;
		if (a.stuck > (last ? 3.0f : 5.0f)) {
			++_stats.snapped;
			n.pos[0] = target[0];
			n.pos[1] = target[1];
			n.pos[2] = target[2];
			return true;
		}
	}
	return false;
}

// Starts the action that has just come off the queue. True if that is all
// there is to it.
bool Npcs::begin(NpcActor& a, const World& world) {
	if (a.action.kind >= NPC_ACT_USE_ITEM) return beginItem(a, world);
	if (a.action.kind >= NPC_ACT_ATTACK) return beginFight(a, world);
	GameNpc& n = _game.npcMut(a.npc);
	NpcAction& act = a.action;
	switch (act.kind) {
	case NPC_ACT_GOTO_NODE:
		if (act.i < 0 || u32(act.i) >= _net.nodeCount()) return true;
		_net.drop(a.npc);
		a.atFree = -1;
		// There already - or as near as one gets with somebody else standing on it.
		if (distance(n.pos, _net.node(u32(act.i)).pos) < (taken(a, _net.node(u32(act.i)).pos) ? 450.0f : 60.0f)) {
			attachTo(a, act.i, -1);
			return true;
		}
		a.attached = false;
		planWay(a, act.i);
		return a.pathCount == 0;
	case NPC_ACT_GOTO_NEXT_FP: {
		const int fp = _net.findFree(n.pos, act.text, a.npc, a.atFree);
		if (fp < 0) return true;
		const FreePoint& p = _net.freePoint(u32(fp));
		act.kind = NPC_ACT_GOTO_POS;
		act.i = fp;
		memcpy(act.pos, p.pos, 3 * sizeof(float));
		act.pos[3] = p.yaw;
	}
		// fall through
	case NPC_ACT_GOTO_POS:
		if (act.i >= 0) {
			// Somebody else got there first: the state will look for another.
			_net.hold(act.i, a.npc);
			if (_net.heldBy(a.npc) != act.i) return true;
		} else {
			_net.drop(a.npc);
		}
		a.atFree = -1;
		a.atNode = -1;
		a.attached = false;
		a.bestDist = 1e30f;
		a.stuck = 0;
		if (distance(n.pos, act.pos) < 50.0f) {
			attachTo(a, -1, act.i);
			if (act.i < 0) {
				a.attachYaw = act.pos[3];
				a.attached = true;
			}
			return true;
		}
		return false;
	case NPC_ACT_GOTO_NPC:
		if (act.i < 0 || u32(act.i) >= _game.npcCount() || !_game.npc(u32(act.i)).placed) return true;
		_net.drop(a.npc);
		a.atFree = a.atNode = -1;
		a.attached = false;
		a.bestDist = 1e30f;
		a.stuck = 0;
		return false;
	case NPC_ACT_ALIGN:
		if (!a.attached) return true;
		a.turnTo = a.attachYaw;
		a.phase = NpcActor::TURNING;
		return false;
	case NPC_ACT_TURN_NPC:
		if (act.i < 0 || u32(act.i) >= _game.npcCount() || !_game.npc(u32(act.i)).placed) return true;
		a.phase = NpcActor::TURNING;
		return false;
	case NPC_ACT_PLAY_ANI:
		if (!playAni(a, act.text, act.i)) return true;
		a.phase = NpcActor::PLAYING;
		return false;
	case NPC_ACT_WAIT:
		a.wait = act.f;
		if (a.wait <= 0.0f) return true;
		a.phase = NpcActor::WAITING;
		return false;
	case NPC_ACT_STAND_UP:
		if (a.mob >= 0) {
			// Up from a bench is out of the bench.
			act.kind = NPC_ACT_USE_MOB;
			act.i = -1;
			return begin(a, world);
		}
		if (!standUp(a)) return true;
		a.phase = NpcActor::PLAYING;
		return false;
	case NPC_ACT_USE_MOB: {
		if (act.i < 0) {
			if (a.mob < 0) return true;
			if (a.mobStage < NpcActor::MOB_ENTERING) {
				// (It never got there.)
				if (a.phase == NpcActor::MOVING) stopWalking(a);
				releaseMob(a);
				return true;
			}
			a.mobStage = NpcActor::MOB_LEAVING;
			a.wait = 0;
			a.phase = NpcActor::PLAYING;
			return false;
		}
		if (a.mob >= 0 && !_mobs.isScheme(u32(a.mob), act.text)) {
			// At a thing of another kind already, or on the way to one: this
			// is passed over. The original's scripts count on it -
			// ZS_SleepBed asks for "BEDHIGH" and then "BEDLOW" in the same
			// breath ("further UseMobs are ignored while one is going on").
			return true;
		}
		const int mob = a.mob >= 0 ? a.mob : findMob(a.npc, act.text, a.mobFailed);
		if (mob < 0) return true; // none around: the state goes on without
		if (a.mob >= 0 && a.mobStage >= NpcActor::MOB_ENTERING) {
			a.wait = 0; // at it already: on to the state asked for
			a.phase = NpcActor::PLAYING;
			return false;
		}
		if (!_mobs.hold(u32(mob), a.npc)) return true;
		a.mob = mob;
		a.mobState = -1;
		const int slot = _mobs.place(u32(mob), n.pos, a.mobPos, a.mobYaw);
		a.mobKind = slot >= 0 ? _mobs.slot(u32(mob), u32(slot)).kind : u8(MOB_SLOT_PLAIN);
		_net.drop(a.npc);
		a.atFree = a.atNode = -1;
		a.attached = false;
		a.bestDist = 1e30f;
		a.stuck = 0;
		a.mobStage = NpcActor::MOB_COMING;
		return false;
	}
	case NPC_ACT_WALK_MODE: {
		const u8 mode = (act.i & 0x7F) == 0 ? u8(NpcActor::RUN) : u8(NpcActor::WALK);
		if (mode != a.walkMode) {
			a.walkMode = mode;
			// Standing is not the same in both.
			if (!a.stance[0] && a.anim.current() == a.idle) fallBack(a, 0.3f);
		}
		return true;
	}
	case NPC_ACT_START_STATE: {
		const NpcAction copy = act; // (starting a state empties the queue and this action with it)
		const int other = int(copy.f);
		if (refusesState(copy.fn)) {
			// An attack: not on somebody it has no quarrel with (NpcConfig::calmGuard).
			const bool allowed = attackJustified(a, other);
			noteAttack(a, other, copy.fn, allowed);
			if (!allowed) {
				++_stats.attacksDenied;
				return true;
			}
		}
		startState(a, copy.fn, copy.i != 0, copy.text, false);
		a.other = other;
		return true;
	}
	case NPC_ACT_CONTINUE_ROUTINE:
		clearState(a, true);
		return true;
	case NPC_ACT_LOOK:
		a.lookAt = act.i;
		return true;
	case NPC_ACT_SAY:
		// Said when the other's earlier lines are (carryOn); until then the wait is how long it may be held up.
		if (act.text[0]) copyName(a.said, sizeof(a.said), act.text);
		a.wait = 6.0f;
		act.started = false;
		a.phase = NpcActor::WAITING;
		return false;
	case NPC_ACT_OUTPUT:
		// Said when the talk's queue of words gets to it (tickSpeech).
		if (u32(act.i) < _speechDone) return true;
		a.phase = NpcActor::WAITING;
		return false;
	case NPC_ACT_PROCESS_INFOS:
		openTalk(a);
		return true;
	case NPC_ACT_STOP_INFOS:
		if (_talk.processing && _talk.npc == int(a.npc)) _talk.stop = true;
		return true;
	case NPC_ACT_TELEPORT:
		_net.drop(a.npc);
		memcpy(n.pos, act.pos, 3 * sizeof(float));
		n.yaw = act.pos[3];
		a.atNode = a.atFree = -1;
		a.attached = false;
		a.grounded = false;
		return true;
	default:
		return true;
	}
}

// Whether the one a line is said to has no line of its own that was given earlier and is still to be said.
bool Npcs::sayMayStart(NpcActor& a, const NpcAction& act) const {
	if (act.i < 0 || u32(act.i) == a.npc) return true;
	for (const NpcActor& o : _actors) {
		if (!o.used || o.npc != u32(act.i)) continue;
		if (o.action.kind == NPC_ACT_SAY && o.action.order < act.order) return false;
		for (u32 k = 0; k < o.queueCount; ++k) {
			const NpcAction& q = o.queue[(o.queueHead + k) % NpcActor::QUEUE];
			if (q.kind == NPC_ACT_SAY && q.order < act.order) return false;
		}
	}
	return true;
}

// The line begins: it sounds from where the speaker stands. Returns how long the speaker is busy with it.
float Npcs::saySpeak(NpcActor& a, NpcAction& act) {
	++_stats.linesSaid;
	u32 hash = act.hash;
	if (!hash) {
		char line[44];
		if (!svmLine(a.npc, act.text, line, sizeof(line))) return 0.0f;
		hash = hashName(line);
	}
	float seconds = 0.0f;
	if (_speechHook.say) seconds = _speechHook.say(_speechHook.user, hash, int(a.npc), false);
	if (seconds < 0.0f) return 0.0f; // out of earshot: not worth waiting for
	if (seconds > 0.0f) {
		++_stats.linesHeard;
		return seconds + 0.2f;
	}
	// No recording: as long as it takes to read, or a moment if the words are not known either.
	const char* raw = _texts.find(hash);
	if (raw && *raw) {
		char text[200];
		scriptText(raw, text, sizeof(text));
		u32 glyphs = 0;
		for (const char* p = text; utf8Next(p);) ++glyphs;
		return clamp(1.2f + 0.055f * float(glyphs), 1.8f, 9.0f);
	}
	return 1.4f;
}

// Goes on with the action in hand. True once it is done.
bool Npcs::carryOn(NpcActor& a, const World& world, float dt) {
	if (a.action.kind >= NPC_ACT_USE_ITEM) return carryOnItem(a, world, dt);
	if (a.action.kind >= NPC_ACT_ATTACK) return carryOnFight(a, world, dt);
	GameNpc& n = _game.npcMut(a.npc);
	NpcAction& act = a.action;
	switch (act.kind) {
	case NPC_ACT_GOTO_NODE: {
		if (a.pathAt >= a.pathCount) return true;
		const u32 node = a.path[a.pathAt];
		const bool last = a.pathAt + 1 == a.pathCount && int(node) == a.goalNode;
		// (Nobody walks into somebody who stands where he is going.)
		const float near = !last ? 90.0f : taken(a, _net.node(node).pos) ? 120.0f : 40.0f;
		if (!walkTo(a, _net.node(node).pos, near, dt, last)) return false;
		a.atNode = int(node);
		a.bestDist = 1e30f;
		a.stuck = 0;
		if (++a.pathAt < a.pathCount) return false;
		if (a.atNode != a.goalNode) {
			// The way was longer than the list holds: on with the next stretch.
			const bool moving = a.phase == NpcActor::MOVING;
			planWay(a, a.goalNode);
			if (a.pathCount) return false;
			if (!moving) a.phase = NpcActor::IDLE;
		}
		stopWalking(a);
		attachTo(a, a.goalNode, -1);
		return true;
	}
	case NPC_ACT_GOTO_POS:
		if (!walkTo(a, act.pos, 35.0f, dt, true)) return false;
		stopWalking(a);
		attachTo(a, -1, act.i);
		if (act.i < 0) {
			a.attachYaw = act.pos[3];
			a.attached = true;
		}
		return true;
	case NPC_ACT_GOTO_NPC: {
		const GameNpc& o = _game.npc(u32(act.i));
		if (!o.placed || o.world != n.world) {
			stopWalking(a);
			return true;
		}
		if (!walkTo(a, o.pos, 250.0f, dt, true)) return false;
		stopWalking(a);
		return true;
	}
	case NPC_ACT_ALIGN:
		if (!turn(a, a.turnTo, 3.2f, dt)) return false;
		a.phase = NpcActor::IDLE;
		return true;
	case NPC_ACT_TURN_NPC: {
		const GameNpc& o = _game.npc(u32(act.i));
		const float dx = o.pos[0] - n.pos[0], dz = o.pos[2] - n.pos[2];
		if (dx * dx + dz * dz > 1.0f && !turn(a, atan2f(dx, dz), 3.2f, dt)) return false;
		a.phase = NpcActor::IDLE;
		return true;
	}
	case NPC_ACT_USE_MOB:
		return useMob(a, world, dt);
	case NPC_ACT_OUTPUT:
		// (Words that were flushed - the talk was cut short - are not waited for.)
		if (u32(act.i) >= _speechDone && speechPending(u32(act.i))) return false;
		a.phase = NpcActor::IDLE;
		return true;
	case NPC_ACT_SAY:
		// Lines go one after the other, whoever speaks (OpenGothic: the order id): not before the one it is said to
		// has said what was given earlier. Held up for 6 s at most.
		if (!act.started) {
			if (!sayMayStart(a, act) && (a.wait -= dt) > 0.0f) return false;
			act.started = true;
			a.wait = saySpeak(a, act);
			if (a.wait <= 0.0f) {
				a.phase = NpcActor::IDLE;
				return true;
			}
			return false;
		}
		a.wait -= dt;
		if (a.wait > 0.0f) return false;
		a.phase = NpcActor::IDLE;
		return true;
	case NPC_ACT_PLAY_ANI:
	case NPC_ACT_STAND_UP:
	case NPC_ACT_WAIT:
		a.wait -= dt;
		if (a.wait > 0.0f) return false;
		a.phase = NpcActor::IDLE;
		return true;
	default:
		return true;
	}
}

// A frame of an actor's life.
void Npcs::tick(NpcActor& a, const World& world, float dt, float hoursPassed) {
	GameNpc& n = _game.npcMut(a.npc);
	if (a.noLying && n.down == DOWN_DEAD && a.slump < 1.0f) a.slump = a.slump + dt * 2.5f < 1.0f ? a.slump + dt * 2.5f : 1.0f;
	a.animPrev = a.anim.time();
	a.animClip = a.anim.current();
	a.anim.advance(dt);
	{
		const ClipSfxEvent* events[8];
		const u32 count = a.anim.takeEvents(events, 8);
		if (_soundHook.fn)
			for (u32 i = 0; i < count; ++i) _soundHook.fn(_soundHook.user, *events[i], int(a.npc));
	}
	const Vec3 moved = a.anim.takeMotion();
	bool walked = false;
	if (a.carried && (moved.x != 0.0f || moved.z != 0.0f)) {
		// The animation's own travel, turned the way the character faces.
		const float s = sinf(n.yaw), c = cosf(n.yaw);
		n.pos[0] += c * moved.x + s * moved.z;
		n.pos[2] += -s * moved.x + c * moved.z;
		walked = true;
	}
	if (a.settle > 0.0f) {
		a.settle -= dt;
		if (a.settle <= 0.0f && a.phase != NpcActor::MOVING) a.carried = false;
	}
	if (_cfg.alive) {
		a.stateTime += dt;
		a.stateHours += hoursPassed;
		a.loopWait -= dt;
		float step = dt;
		bool asked = false;
		// Several actions a frame at most, so that those which take no time
		// do not take a frame each - but the state is asked only once (a
		// state that keeps starting itself must not eat the frame).
		for (int turnCount = 0; turnCount < 8; ++turnCount) {
			if (a.action.kind != NPC_ACT_NONE) {
				if (!carryOn(a, world, step)) break;
				a.action = NpcAction();
				++_stats.actions;
				step = 0;
				continue;
			}
			if (a.queueCount) {
				NpcAction next = a.queue[a.queueHead];
				// Nobody walks off sitting.
				const bool leaves = next.kind == NPC_ACT_GOTO_NODE || next.kind == NPC_ACT_GOTO_POS ||
				                    next.kind == NPC_ACT_GOTO_NEXT_FP || next.kind == NPC_ACT_GOTO_NPC;
				if (leaves && a.mob >= 0) {
					next = NpcAction(); // ... or with a bench still under him
					next.kind = NPC_ACT_USE_MOB;
					next.i = -1;
				} else if (leaves && a.stance[0]) {
					next = NpcAction();
					next.kind = NPC_ACT_STAND_UP;
				} else {
					a.queueHead = u8((a.queueHead + 1) % NpcActor::QUEUE);
					--a.queueCount;
				}
				a.action = next;
				if (begin(a, world)) {
					a.action = NpcAction();
					++_stats.actions;
				}
				continue;
			}
			if (a.phase != NpcActor::IDLE) a.phase = NpcActor::IDLE;
			if (asked) break;
			asked = true;
			tickState(a);
			if (a.action.kind == NPC_ACT_NONE && !a.queueCount) break;
		}
		a.percWait -= dt;
		// The player has just come within the reach of its senses: it looks at once.
		const bool near = heroNear(n);
		if (near && !a.heroWasNear) a.percWait = 0.0f;
		a.heroWasNear = near;
		if (_cfg.perceive && a.percWait <= 0.0f && n.down == DOWN_NONE) perceive(a, world);
		turnHead(a, dt);
	}
	tickFight(a, dt);
	// A thing in hand goes when the weapon comes out or the character goes down, or when
	// something else has had it stand up.
	if (a.heldItem != 0xFFFFFFFFu && (n.weapon != WEAPON_NONE || n.down != DOWN_NONE)) dropHeldItem(a);
	checkHeldItem(a);
	// A gesture that has run out leaves it standing (or sitting) as before.
	// (Not one that is down: the fall ends where it lies. A model whose T_DEAD has no S_DEAD to
	// lead into used to get up again from here.)
	if (n.down == DOWN_NONE && a.anim.finished() && a.phase != NpcActor::MOVING && a.action.kind != NPC_ACT_PLAY_ANI && a.action.kind != NPC_ACT_USE_MOB &&
	    a.action.kind != NPC_ACT_USE_ITEM && a.action.kind != NPC_ACT_TAKE_ITEM)
		fallBack(a, 0.2f);
	// ... and on a bench so does one that has led on into standing: a look
	// around from a bench ends on the bench.
	else if (a.mob >= 0 && a.mobStage == NpcActor::MOB_IN && (a.action.kind == NPC_ACT_NONE || a.action.kind == NPC_ACT_WAIT) &&
	         (a.phase == NpcActor::IDLE || a.phase == NpcActor::WAITING) && a.idle && a.anim.current() && a.anim.current() != a.idle &&
	         a.anim.current()->loops())
		fallBack(a, 0.25f);
	// (In a bed the ground is no concern: the animation knows where the body is.)
	const bool inMob = a.mob >= 0 && a.mobStage >= NpcActor::MOB_ENTERING;
	if ((walked || a.phase == NpcActor::MOVING || !a.grounded) && !inMob) ground(a, world, 120.0f);
}

// ---- what the scripts call ----

namespace {

void copyText(char* to, size_t size, const char* from) {
	size_t n = from ? strlen(from) : 0;
	if (n >= size) n = size - 1;
	if (n) memcpy(to, from, n);
	to[n] = 0;
}

} // namespace

void Npcs::xGotoWp(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	char name[64];
	copyText(name, sizeof(name), vm.popString());
	NpcActor* a = self.actorFor(vm.popInstance());
	if (!a) return;
	NpcAction act;
	const int node = self._net.findNode(name);
	if (node >= 0) {
		act.kind = NPC_ACT_GOTO_NODE;
		act.i = node;
	} else if (findPoint(&self, name, act.pos, act.pos[3])) {
		// (The scripts name places that are no way points now and then.)
		act.kind = NPC_ACT_GOTO_POS;
		act.i = -1;
	} else {
		return;
	}
	self.push(a, act);
}

void Npcs::xGotoFp(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	char word[32];
	copyText(word, sizeof(word), vm.popString());
	NpcActor* a = self.actorFor(vm.popInstance());
	if (!a) return;
	const GameNpc& n = self._game.npc(a->npc);
	// The one it is at, if that will do; else the nearest free one.
	int fp = a->atFree >= 0 && self._net.nameContains(u32(a->atFree), word) ? a->atFree : self._net.findFree(n.pos, word, a->npc);
	if (fp < 0) return;
	const FreePoint& p = self._net.freePoint(u32(fp));
	NpcAction act;
	act.kind = NPC_ACT_GOTO_POS;
	act.i = fp;
	memcpy(act.pos, p.pos, 3 * sizeof(float));
	act.pos[3] = p.yaw;
	self.push(a, act);
}

void Npcs::xGotoNextFp(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	NpcAction act;
	act.kind = NPC_ACT_GOTO_NEXT_FP;
	copyText(act.text, sizeof(act.text), vm.popString());
	self.push(self.actorFor(vm.popInstance()), act);
}

void Npcs::xGotoNpc(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	NpcAction act;
	act.kind = NPC_ACT_GOTO_NPC;
	act.i = self.npcFor(vm.popInstance());
	self.push(self.actorFor(vm.popInstance()), act);
}

void Npcs::xAlign(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	NpcAction act;
	act.kind = NPC_ACT_ALIGN;
	self.push(self.actorFor(vm.popInstance()), act);
}

void Npcs::xTurnToNpc(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	NpcAction act;
	act.kind = NPC_ACT_TURN_NPC;
	act.i = self.npcFor(vm.popInstance());
	self.push(self.actorFor(vm.popInstance()), act);
}

void Npcs::xPlayAni(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	NpcAction act;
	act.kind = NPC_ACT_PLAY_ANI;
	act.i = -1;
	copyText(act.text, sizeof(act.text), vm.popString());
	self.push(self.actorFor(vm.popInstance()), act);
}

void Npcs::xPlayAniBs(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	NpcAction act;
	act.kind = NPC_ACT_PLAY_ANI;
	act.i = vm.popInt();
	if (act.i < 0) act.i = -1;
	copyText(act.text, sizeof(act.text), vm.popString());
	self.push(self.actorFor(vm.popInstance()), act);
}

void Npcs::xWait(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	NpcAction act;
	act.kind = NPC_ACT_WAIT;
	act.f = vm.popFloat();
	NpcActor* a = self.actorFor(vm.popInstance());
	if (act.f > 0.0f && act.f < 3600.0f) self.push(a, act);
}

// AI_WaitMS(self, ms): AI_Wait in milliseconds.
void Npcs::xWaitMs(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	NpcAction act;
	act.kind = NPC_ACT_WAIT;
	act.f = float(vm.popInt()) / 1000.0f;
	NpcActor* a = self.actorFor(vm.popInstance());
	if (act.f > 0.0f && act.f < 3600.0f) self.push(a, act);
}

// Npc_SetRefuseTalk(self, seconds): it will not talk for a while; Npc_RefuseTalk(self) asks.
void Npcs::xSetRefuseTalk(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const i32 s = vm.popInt();
	if (NpcActor* a = self.actorFor(vm.popInstance())) a->refuseTalk = s > 0 ? float(s) : 0.0f;
}

void Npcs::xRefuseTalk(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const NpcActor* a = self.actorFor(vm.popInstance());
	vm.pushInt(a && a->refuseTalk > 0.0f);
}

void Npcs::xStandUp(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	NpcAction act;
	act.kind = NPC_ACT_STAND_UP;
	self.push(self.actorFor(vm.popInstance()), act);
}

void Npcs::xSetWalkMode(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	NpcAction act;
	act.kind = NPC_ACT_WALK_MODE;
	act.i = vm.popInt();
	self.push(self.actorFor(vm.popInstance()), act);
}

void Npcs::xStartState(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	NpcAction act;
	act.kind = NPC_ACT_START_STATE;
	copyText(act.text, sizeof(act.text), vm.popString());
	act.i = vm.popInt();
	act.fn = u32(vm.popInt());
	NpcActor* a = self.actorFor(vm.popInstance());
	// Whoever is "other" now is the new state's.
	act.f = float(self.npcFor(vm.instanceOf(vm.otherSymbol())));
	if (vm.isPlainFunction(act.fn)) self.push(a, act);
}

void Npcs::xContinueRoutine(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	NpcAction act;
	act.kind = NPC_ACT_CONTINUE_ROUTINE;
	self.push(self.actorFor(vm.popInstance()), act);
}

void Npcs::xTeleport(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	char name[64];
	copyText(name, sizeof(name), vm.popString());
	NpcActor* a = self.actorFor(vm.popInstance());
	NpcAction act;
	act.kind = NPC_ACT_TELEPORT;
	if (a && findPoint(&self, name, act.pos, act.pos[3])) self.push(a, act);
}

void Npcs::xClearQueue(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	if (NpcActor* a = self.actorFor(vm.popInstance())) {
		// (The action that called this, if any, goes with the rest.)
		a->queueHead = a->queueCount = 0;
	}
}

void Npcs::xDistToWp(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	char name[64];
	copyText(name, sizeof(name), vm.popString());
	const int who = self.npcFor(vm.popInstance());
	const int node = self._net.findNode(name);
	float at[3], yaw;
	if (node >= 0)
		memcpy(at, self._net.node(u32(node)).pos, sizeof(at));
	// (A place that is no way point, as the routines name now and then.)
	if (who < 0 || !self._game.npc(u32(who)).placed || (node < 0 && !findPoint(&self, name, at, yaw))) {
		vm.pushInt(INT_MAX);
		return;
	}
	vm.pushInt(i32(distance(self._game.npc(u32(who)).pos, at)));
}

void Npcs::xDistToNpc(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	ScriptInstance* const second = vm.popInstance();
	const int b = self.npcFor(second), a = self.npcFor(vm.popInstance());
	// (The scripts ask it of things lying about too: Npc_GetDistToNpc(self, item) in B_FetchWeapon.)
	float itemPos[3];
	if (b < 0 && a >= 0 && self.lootPosition(second, itemPos)) {
		vm.pushInt(i32(distance(self._game.npc(u32(a)).pos, itemPos)));
		return;
	}
	if (a < 0 || b < 0 || !self._game.npc(u32(a)).placed || !self._game.npc(u32(b)).placed ||
	    self._game.npc(u32(a)).world != self._game.npc(u32(b)).world) {
		vm.pushInt(INT_MAX);
		return;
	}
	vm.pushInt(i32(distance(self._game.npc(u32(a)).pos, self._game.npc(u32(b)).pos)));
}

void Npcs::xDistToPlayer(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const int a = self.npcFor(vm.popInstance());
	const GameNpc* hero = self._game.hero();
	if (a < 0 || !hero || !hero->placed || !self._game.npc(u32(a)).placed) {
		vm.pushInt(INT_MAX);
		return;
	}
	vm.pushInt(i32(distance(self._game.npc(u32(a)).pos, hero->pos)));
}

void Npcs::xNearestWp(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const int who = self.npcFor(vm.popInstance());
	const int node = who >= 0 ? self._net.nearestNode(self._game.npc(u32(who)).pos) : -1;
	vm.pushString(node >= 0 ? self._net.nodeName(u32(node)) : "");
}

void Npcs::xNextWp(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const int who = self.npcFor(vm.popInstance());
	int node = -1;
	if (who >= 0) {
		const int nearest = self._net.nearestNode(self._game.npc(u32(who)).pos);
		node = nearest >= 0 ? self._net.nearestNode(self._game.npc(u32(who)).pos, nearest) : -1;
	}
	vm.pushString(node >= 0 ? self._net.nodeName(u32(node)) : "");
}

void Npcs::xIsOnFp(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	char word[32];
	copyText(word, sizeof(word), vm.popString());
	const NpcActor* a = self.actorFor(vm.popInstance());
	bool on = false;
	if (a && a->atFree >= 0 && u32(a->atFree) < self._net.freeCount())
		on = self._net.nameContains(u32(a->atFree), word) &&
		     distance(self._game.npc(a->npc).pos, self._net.freePoint(u32(a->atFree)).pos) <= kAtPoint;
	vm.pushInt(on);
}

void Npcs::xIsFpAvailable(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	char word[32];
	copyText(word, sizeof(word), vm.popString());
	ScriptInstance* inst = vm.popInstance();
	const int who = self.npcFor(inst);
	if (who < 0) {
		vm.pushInt(0);
		return;
	}
	const NpcActor* a = self.actorFor(inst);
	if (a && a->atFree >= 0 && self._net.nameContains(u32(a->atFree), word)) {
		vm.pushInt(1);
		return;
	}
	vm.pushInt(self._net.findFree(self._game.npc(u32(who)).pos, word, u32(who)) >= 0);
}

void Npcs::xIsNextFpAvailable(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	char word[32];
	copyText(word, sizeof(word), vm.popString());
	ScriptInstance* inst = vm.popInstance();
	const int who = self.npcFor(inst);
	if (who < 0) {
		vm.pushInt(0);
		return;
	}
	const NpcActor* a = self.actorFor(inst);
	vm.pushInt(self._net.findFree(self._game.npc(u32(who)).pos, word, u32(who), a ? a->atFree : -1) >= 0);
}

void Npcs::xIsInState(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const u32 fn = u32(vm.popInt());
	ScriptInstance* inst = vm.popInstance();
	const int who = self.npcFor(inst);
	// Down is down, actor or not (the player's character too).
	if (who >= 0 && fn != 0) {
		const u8 down = self._game.npc(u32(who)).down;
		if ((down == DOWN_UNCONSCIOUS && fn == self._fnUnconscious) || (down == DOWN_DEAD && fn == self._fnDead)) {
			vm.pushInt(1);
			return;
		}
	}
	if (const NpcActor* a = self.actorFor(inst))
		vm.pushInt(a->state != 0 && a->state == fn);
	else // far away it is doing what its routine says
		vm.pushInt(who >= 0 && fn != 0 && self._game.behaviour(self._game.npc(u32(who))) == fn);
}

void Npcs::xWasInState(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const u32 fn = u32(vm.popInt());
	const NpcActor* a = self.actorFor(vm.popInstance());
	vm.pushInt(a && a->prevState != 0 && a->prevState == fn);
}

void Npcs::xGetStateTime(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const NpcActor* a = self.actorFor(vm.popInstance());
	vm.pushInt(a ? i32(a->stateTime) : 0);
}

void Npcs::xSetStateTime(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const i32 seconds = vm.popInt();
	if (NpcActor* a = self.actorFor(vm.popInstance())) a->stateTime = float(clamp<i32>(seconds, 0, 1 << 24));
}

void Npcs::xGetBodyState(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	ScriptInstance* inst = vm.popInstance();
	const NpcActor* a = self.actorFor(inst);
	if (!a) {
		const GameNpc* hero = self._game.hero();
		if (hero && hero->object == inst) {
			vm.pushInt(self.heroBodyState());
			return;
		}
	}
	vm.pushInt(a ? self.bodyStateOf(*a) : kBsStand);
}

void Npcs::xSetPercTime(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const float seconds = vm.popFloat();
	if (NpcActor* a = self.actorFor(vm.popInstance())) a->percTime = clamp(seconds, 0.0f, 60.0f);
}

// Looks for the nearest character around `self` that is of an instance, in a
// state and of a guild (-1: any). Who is found becomes OTHER.
void Npcs::xDetectNpc(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const i32 guild = vm.popInt(), state = vm.popInt(), instance = vm.popInt();
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
		if (int(index) == who || !o.placed || o.world != n.world) return;
		if (instance != -1 && i32(o.symbol) != instance) return;
		if (state != -1 && i32(itsState) != state) return;
		if (guild != -1 && self._game.guildOf(o) != guild) return;
		const float dx = o.pos[0] - n.pos[0], dy = o.pos[1] - n.pos[1], dz = o.pos[2] - n.pos[2];
		const float d = dx * dx + dy * dy + dz * dz;
		if (d >= best) return;
		best = d;
		found = int(index);
	};
	// Whoever is near enough to be found is an actor - or the player.
	for (const NpcActor& a : self._actors)
		if (a.used) consider(a.npc, a.state);
	if (const GameNpc* hero = self._game.hero()) consider(u32(hero - &self._game.npc(0)), 0);
	if (found >= 0) vm.setInstance(vm.otherSymbol(), self._game.npc(u32(found)).object);
	vm.pushInt(found >= 0);
}

void Npcs::xCanSeeNpc(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const int b = self.npcFor(vm.popInstance()), a = self.npcFor(vm.popInstance());
	bool sees = false;
	if (a >= 0 && b >= 0) {
		const GameNpc& n = self._game.npc(u32(a));
		const GameNpc& o = self._game.npc(u32(b));
		const float range = self._game.sensesRange(n);
		if (n.placed && o.placed && n.world == o.world && distance(n.pos, o.pos) <= range) {
			// In front of it, within +-100 degrees, for everybody (OpenGothic's canSeeNpc without free
			// line of sight): a creature that "cannot see" the player turns its body to him
			// (ZS_MM_AssessEnemy_loop), which is what makes it face him and warn. (Noticing the player
			// at all is not this: that goes all round, see Npcs::sees.)
			const float dx = o.pos[0] - n.pos[0], dz = o.pos[2] - n.pos[2];
			sees = dx * dx + dz * dz < 1.0f || fabsf(wrapAngle(atan2f(dx, dz) - n.yaw)) < 1.75f;
		}
	}
	vm.pushInt(sees);
}

void Npcs::xUseMob(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	NpcAction act;
	act.kind = NPC_ACT_USE_MOB;
	act.i = vm.popInt();
	if (act.i < 0) act.i = -1;
	upperCopy(act.text, sizeof(act.text), vm.popString());
	self.push(self.actorFor(vm.popInstance()), act);
	if (vm.symbol(vm.callee()).flags & SCRIPTF_RETURN) vm.pushInt(0);
}

void Npcs::xIsMobAvailable(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	char kind[48];
	copyText(kind, sizeof(kind), vm.popString());
	ScriptInstance* inst = vm.popInstance();
	const int who = self.npcFor(inst);
	if (who < 0 || !self._cfg.alive) {
		vm.pushInt(0);
		return;
	}
	const NpcActor* a = self.actorFor(inst);
	if (a && a->mob >= 0 && self._mobs.isScheme(u32(a->mob), kind)) {
		vm.pushInt(1); // the one it is at
		return;
	}
	vm.pushInt(self.findMob(u32(who), kind, a ? a->mobFailed : -1) >= 0);
}

void Npcs::xGetMobState(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	char kind[48];
	copyText(kind, sizeof(kind), vm.popString());
	ScriptInstance* inst = vm.popInstance();
	const int who = self.npcFor(inst);
	const NpcActor* a = self.actorFor(inst);
	if (a && a->mob >= 0 && self._mobs.isScheme(u32(a->mob), kind))
		vm.pushInt(max<i32>(a->mobState, 0));
	else // (a thing nobody is at rests in its first state)
		vm.pushInt(who >= 0 && self.findMob(u32(who), kind, -1) >= 0 ? 0 : -1);
}

void Npcs::xGetDetectedMob(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const NpcActor* a = self.actorFor(vm.popInstance());
	vm.pushString(a && a->mob >= 0 ? self._mobs.scheme(u32(a->mob)) : "");
}

void Npcs::bindAi() {
	ScriptVm& vm = _game.vm();
	// How the scripts count what a body is doing while it uses a thing, and
	// which kinds of things mean what (their own lists; the game's, should
	// they have none).
	const auto number = [&](const char* name, i32 otherwise) {
		const u32 sym = vm.find(name);
		return sym != SCRIPT_NONE && vm.symbol(sym).type == SCRIPT_INT && !(vm.symbol(sym).flags & SCRIPTF_MEMBER) ? vm.getInt(sym) : otherwise;
	};
	const auto list = [&](const char* name, const char* otherwise, char* to, size_t size) {
		const u32 sym = vm.find(name);
		const bool have = sym != SCRIPT_NONE && vm.symbol(sym).type == SCRIPT_STRING && !(vm.symbol(sym).flags & SCRIPTF_MEMBER);
		upperCopy(to, size, have ? vm.getString(sym) : otherwise);
	};
	_bsSit = number("BS_SIT", kBsSit);
	_bsLie = number("BS_LIE", kBsLie);
	_bsClimb = number("BS_CLIMB", 9 | kBsInterruptable);
	_bsMob = number("BS_MOBINTERACT", 15);
	_bsMobInterrupt = number("BS_MOBINTERACT_INTERRUPT", 16 | kBsInterruptable);
	list("MOB_SIT", "BENCH,CHAIR,GROUND,THRONE", _mobSit, sizeof(_mobSit));
	list("MOB_LIE", "BED,BEDHIGH,BEDLOW", _mobLie, sizeof(_mobLie));
	list("MOB_CLIMB", "CLIMB,LADDER,RANKE", _mobClimb, sizeof(_mobClimb));
	list("MOB_NOTINTERRUPTABLE", "DOOR,LEVER,TOUCHPLATE,TURNSWITCH,VWHEEL,CHESTBIG,CHESTSMALL,HERB,IDOL,PAN,SMOKE,INNOS", _mobBusy,
	     sizeof(_mobBusy));
	_victim = vm.find("VICTIM");
	if (_victim != SCRIPT_NONE && vm.symbol(_victim).type != SCRIPT_INSTANCE) _victim = SCRIPT_NONE;
	// (In the order of Game::lifeFunctions, which is the list of their names.)
	static const ScriptVm::External table[] = {
	    xGotoWp,      xGotoFp,       xGotoNextFp,      xGotoNpc,      xAlign,        xAlign,           xTurnToNpc,
	    xPlayAni,     xPlayAniBs,    xWait,            xStandUp,      xStandUp,      xSetWalkMode,     xStartState,
	    xContinueRoutine, xTeleport, xClearQueue,      xDistToWp,     xDistToNpc,    xDistToPlayer,    xNearestWp,
	    xNextWp,      xIsOnFp,       xIsFpAvailable,   xIsNextFpAvailable, xIsInState, xWasInState,    xGetStateTime,
	    xSetStateTime, xGetBodyState, xSetPercTime,    xDetectNpc,    xCanSeeNpc,    xUseMob,          xIsMobAvailable,
	    xGetMobState, xGetDetectedMob};
	u32 count = 0;
	const char* const* names = Game::lifeFunctions(count);
	static_assert(sizeof(table) / sizeof(table[0]) == 37, "one function per name");
	for (u32 i = 0; i < count && i < sizeof(table) / sizeof(table[0]); ++i) vm.bind(names[i], table[i], this);
	vm.bind("AI_WAITMS", xWaitMs, this);
	vm.bind("NPC_SETREFUSETALK", xSetRefuseTalk, this);
	vm.bind("NPC_REFUSETALK", xRefuseTalk, this);
	bindPerception();
	bindFight();
	bindItems();
	vm.onUnbound(&Npcs::unboundThunk, this);
}

void Npcs::unboundThunk(u32 symbol, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	self._lastUnbound = self._game.vm().symbol(symbol).nameHash;
	++self._unboundCount;
	for (u32 i = 0; i < self._unboundSeenCount; ++i)
		if (self._unboundSeen[i] == self._lastUnbound) return;
	if (self._unboundSeenCount < 6) self._unboundSeen[self._unboundSeenCount++] = self._lastUnbound;
}

} // namespace g3
