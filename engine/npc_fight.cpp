// The inhabitants fight (npcs.h, fight.h): the scripts' states of attack
// call AI_Attack again and again, and each call has the engine choose the
// next move from the fight AI table of the character's tactic for the
// situation it is in - the enemy about to strike, the enemy within reach of
// the weapon ("W") or of a few steps ("G"), in front or not. A move is a few
// steps (a blow left, then right, then ahead; a parry; a hop back; a step
// aside; closing in), carried out one after the other, each when the one
// before has had its time. Blows land at the frame the animation says
// (ClipFight); what they do - fended off, damage, reeling, down - is the same
// for everybody and is decided in strike().
//
// As the original does it, after OpenGothic's FightAlgo and Npc::implAttack.
#include "npcs.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
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

} // namespace

// ---- what the scripts' numbers are ----

u8 Npcs::moveOf(i32 scriptMove) const {
	if (scriptMove == 0) return FM_NONE;
	for (u8 m = 1; m < FM_COUNT; ++m)
		if (_scriptMoves[m] == scriptMove) return m;
	return FM_NONE;
}

void Npcs::bindFight() {
	ScriptVm& vm = _game.vm();
	const auto number = [&](const char* name, i32 otherwise) { return _game.constant(name, otherwise); };
	_bsHit = number("BS_HIT", 26 | 32768);
	_bsParade = number("BS_PARADE", 27);
	_bsStumble = number("BS_STUMBLE", 21);
	_bsDead = number("BS_DEAD", 23);
	_bsUnconscious = number("BS_UNCONSCIOUS", 22);
	_bsRun = number("BS_RUN", 3);
	_percDamage = number("PERC_ASSESSDAMAGE", -1);
	_percOthersDamage = number("PERC_ASSESSOTHERSDAMAGE", -1);
	_percFightSound = number("PERC_ASSESSFIGHTSOUND", -1);
	_percDefeat = number("PERC_ASSESSDEFEAT", -1);
	_percMurder = number("PERC_ASSESSMURDER", -1);
	_percFighter = number("PERC_ASSESSFIGHTER", -1);
	_percBody = number("PERC_ASSESSBODY", -1);
	_percDrawWeapon = number("PERC_DRAWWEAPON", -1);
	_percRemoveWeapon = number("PERC_ASSESSREMOVEWEAPON", -1);
	_percThreat = number("PERC_ASSESSTHREAT", -1);
	_fnUnconscious = _game.function("ZS_UNCONSCIOUS");
	_fnDead = _game.function("ZS_DEAD");
	// The moves, by the scripts' names (Fight.d).
	for (u32 m = 1; m < FM_COUNT; ++m) _scriptMoves[m] = number(kFightMoveNames[m], kFightMoveDefaults[m]);
	for (FaiTable& t : _fai) t.read = false;
	const struct {
		const char* name;
		ScriptVm::External fn;
	} table[] = {{"AI_ATTACK", xAttack},
	             {"AI_READYMELEEWEAPON", xReadyMelee},
	             {"AI_READYRANGEDWEAPON", xReadyRanged},
	             {"AI_READYSPELL", xReadySpell},
	             {"AI_UNREADYSPELL", xUnreadySpell},
	             {"NPC_ISDRAWINGSPELL", xIsDrawingSpell},
	             {"NPC_HASSPELL", xHasSpell},
	             {"NPC_GETACTIVESPELLISSCROLL", xSpellIsScroll},
	             {"NPC_SETACTIVESPELLINFO", xSetSpellInfo},
	             {"AI_DRAWWEAPON", xDrawWeapon},
	             {"AI_REMOVEWEAPON", xRemoveWeapon},
	             {"AI_EQUIPBESTMELEEWEAPON", xEquipBestMelee},
	             {"AI_DODGE", xDodge},
	             {"AI_FLEE", xFlee},
	             {"AI_FINISHINGMOVE", xFinishingMove},
	             {"AI_WHIRLAROUND", xWhirlAround},
	             {"AI_TURNAWAY", xTurnAway},
	             {"NPC_GETNEXTTARGET", xGetNextTarget},
	             {"NPC_ISNEXTTARGETAVAILABLE", xIsNextTargetAvailable},
	             {"AI_POINTATNPC", xNoAction},
	             {"AI_STOPPOINTAT", xNoAction},
	             {"AI_EQUIPBESTRANGEDWEAPON", xEquipBestRanged},
	             {"AI_STOPAIM", xNoAction}};
	for (const auto& e : table) vm.bind(e.name, e.fn, this);
}

// ---- bows and spells ----

// How far a shot reaches (OpenGothic's FightAlgo::weaponRange: the reference bow range of the game, the magic
// focus range).
float Npcs::rangedReach(const GameNpc& n) const {
	if (n.weapon == WEAPON_MAG) return 3500.0f;
	return _game.isGothic2() ? 1500.0f : 2000.0f;
}

// Aim and shoot (a bow: "S_BOWSHOOT"; a spell: "T_MAGRUN_2_<letters>SHOOT"); the shot leaves part of the way in
// (tickFight). A bow is reloaded after it.
bool Npcs::startShot(NpcActor& a) {
	GameNpc& n = _game.npcMut(a.npc);
	const Clip* c = nullptr;
	float share = 0.4f;
	if (n.weapon == WEAPON_MAG) {
		const GameSpell* sp = n.spell >= 0 ? _game.spellInfo(n.spell) : nullptr;
		if (sp && sp->letters[0]) {
			char name[40];
			static const char* const kCastClips[4] = {"T_MAGRUN_2_%sSHOOT", "S_%sSHOOT", "T_MAGRUN_2_%sCAST", "S_%sCAST"};
			for (const char* f : kCastClips) {
				snprintf(name, sizeof(name), f, sp->letters);
				if ((c = clip(a, name)) != nullptr) break;
			}
		}
		share = 0.6f;
	} else {
		c = weaponClip(a, "S_%sSHOOT");
		if (!c) c = weaponClip(a, "S_%sAIM");
	}
	const float length = c ? c->duration() : 0.6f;
	if (c) {
		a.anim.play(c, 0.1f);
		a.carried = false;
		a.shotClip = c;
		a.shotAt = length * share;
	} else {
		fireShot(a); // (no animation for it: it goes off at once)
	}
	a.phase = NpcActor::PLAYING;
	// The shot, and a pause to draw the next arrow / to gather the next spell.
	float after = n.weapon == WEAPON_MAG ? 0.8f : 0.5f;
	if (n.weapon != WEAPON_MAG)
		if (const Clip* r = weaponClip(a, "T_%sRELOAD")) after = r->duration();
	a.faiWait = length + after;
	return true;
}

void Npcs::fireShot(NpcActor& a) {
	GameNpc& n = _game.npcMut(a.npc);
	const int target = n.target;
	if (n.weapon == WEAPON_MAG) {
		const GameSpell* sp = n.spell >= 0 ? _game.spellInfo(n.spell) : nullptr;
		if (!sp) return;
		// The scripts are asked as for the player (Spell_ProcessMana), quickly: the spell is charged as far as
		// they let it (and its other effects - a summoning, a healing - happen there), mana is paid per point.
		i32 invested = 0, level = 1;
		bool cast = !sp->shoots; // (a spell that does its work through the scripts needs no projectile)
		for (int k = 0; k < 16; ++k) {
			const i32 code = _game.processMana(a.npc, target, invested, false);
			const i32 mana = _game.attributeOf(n, 2);
			if (mana > 0) _game.setAttribute(n, 2, mana - 1);
			if (code == _game.constant("SPL_SENDCAST", 2)) {
				cast = true;
				break;
			}
			if (code == _game.constant("SPL_SENDSTOP", 3)) return;
			if (code == _game.constant("SPL_NEXTLEVEL", 4)) ++level;
			else if (code == _game.constant("SPL_DONTINVEST", 0)) {
				cast = true;
				break;
			}
			++invested;
			if (_game.attributeOf(n, 2) <= 0) {
				cast = true;
				break;
			}
		}
		if (!cast) return;
		n.spellLevel = u8(level > 15 ? 15 : level);
		++_spellsCast;
		if (sp->shoots && _shotHook.fire) _shotHook.fire(_shotHook.user, a.npc, target, n.spell, level);
		if (!sp->shoots && target >= 0 && u32(target) < _game.npcCount()) {
			_game.npcMut(u32(target)).lastHitSpell = n.spell;
			perceiveSingle(u32(target), "PERC_ASSESSMAGIC", int(a.npc));
		}
		// A scroll is used up.
		const u32 item = _game.spellItemOf(a.npc, n.spell);
		const GameItemInfo* it = item != SCRIPT_NONE ? _game.itemInfo(item) : nullptr;
		if (it && (it->flags & _game.constant("ITEM_MULTI", 1 << 21))) _game.giveItems(a.npc, item, -1);
		return;
	}
	const GameItemInfo* bow = n.ranged != SCRIPT_NONE ? _game.itemInfo(n.ranged) : nullptr;
	if (!bow || bow->munition < 0 || _game.itemCount(a.npc, u32(bow->munition)) <= 0) return;
	if (_shotHook.fire && _shotHook.fire(_shotHook.user, a.npc, target, -1, 0)) {
		_game.giveItems(a.npc, u32(bow->munition), -1);
		++_shotsFired;
	}
}

// AI_ReadyRangedWeapon(npc): the bow or crossbow, if it has one and arrows for it; else the melee weapon.
void Npcs::xReadyRanged(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const int who = self.npcFor(vm.popInstance());
	NpcActor* a = who >= 0 ? self.actorByNpc(u32(who)) : nullptr;
	if (!a || !self._cfg.alive) return;
	NpcAction act;
	act.kind = NPC_ACT_DRAW;
	const u8 mode = self._game.rangedModeOf(u32(who), true);
	act.i = mode != WEAPON_NONE ? mode : u8(WEAPON_NONE);
	self.push(a, act);
}

void Npcs::xEquipBestRanged(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const int who = self.npcFor(vm.popInstance());
	if (who < 0) return;
	const u32 best = self._game.bestRanged(u32(who));
	if (best != SCRIPT_NONE) self._game.equip(u32(who), best);
}

// AI_ReadySpell(npc, spell, mana): the magic stance with that spell (OpenGothic: aiReadySpell).
void Npcs::xReadySpell(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const i32 mana = vm.popInt();
	const i32 spell = vm.popInt();
	const int who = self.npcFor(vm.popInstance());
	NpcActor* a = who >= 0 ? self.actorByNpc(u32(who)) : nullptr;
	if (!a || mana <= 0 || spell < 0 || !self._game.spellInfo(spell) || !self._cfg.alive) return;
	GameNpc& n = self._game.npcMut(u32(who));
	if (n.weapon == WEAPON_MAG) {
		n.spell = i16(spell);
		return;
	}
	NpcAction act;
	act.kind = NPC_ACT_DRAW;
	act.i = WEAPON_MAG;
	act.fn = u32(spell + 1);
	self.push(a, act);
}

void Npcs::xUnreadySpell(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const int who = self.npcFor(vm.popInstance());
	NpcActor* a = who >= 0 ? self.actorByNpc(u32(who)) : nullptr;
	if (!a || self._game.npc(u32(who)).weapon != WEAPON_MAG) return;
	NpcAction act;
	act.kind = NPC_ACT_UNDRAW;
	self.push(a, act);
}

// Npc_IsDrawingSpell(npc): the rune (or scroll) in hand, its instance; 0 for none.
void Npcs::xIsDrawingSpell(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const int who = self.npcFor(vm.popInstance());
	i32 item = 0;
	if (who >= 0) {
		const GameNpc& n = self._game.npc(u32(who));
		if (n.weapon == WEAPON_MAG && n.spell >= 0) {
			const u32 it = self._game.spellItemOf(u32(who), n.spell);
			if (it != SCRIPT_NONE) item = i32(it);
		}
	}
	vm.pushInt(item);
}

void Npcs::xHasSpell(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const i32 spell = vm.popInt();
	const int who = self.npcFor(vm.popInstance());
	vm.pushInt(who >= 0 && self._game.spellItemOf(u32(who), spell) != SCRIPT_NONE);
}

void Npcs::xSpellIsScroll(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const int who = self.npcFor(vm.popInstance());
	bool scroll = false;
	if (who >= 0) {
		const GameNpc& n = self._game.npc(u32(who));
		const u32 it = n.spell >= 0 ? self._game.spellItemOf(u32(who), n.spell) : SCRIPT_NONE;
		const GameItemInfo* info = it != SCRIPT_NONE ? self._game.itemInfo(it) : nullptr;
		scroll = info && (info->flags & self._game.constant("ITEM_MULTI", 1 << 21));
	}
	vm.pushInt(scroll);
}

// Npc_SetActiveSpellInfo(npc, value): what the spell being cast makes him - a transformation's creature (an
// instance); kept until the spell goes off (App::castCommit, as OpenGothic's Npc::commitSpell) or is lost.
void Npcs::xSetSpellInfo(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const i32 value = vm.popInt();
	const int who = self._game.indexOf(vm.popInstance());
	if (who >= 0) self._game.npcMut(u32(who)).spellInfo = value;
	vm.pushInt(0);
}

bool Npcs::loadFightAi(Pak& pak) {
	const PakEntry* e = pak.find("script/fight");
	if (!e || e->size < sizeof(FightAiHeader) || e->size > (1u << 16)) return false;
	u8* blob = static_cast<u8*>(pak.readAlloc(*e));
	if (!blob) return false;
	FightAiHeader h {};
	memcpy(&h, blob, sizeof(h));
	const bool ok = h.magic == FIGHT_AI_MAGIC && h.version == FIGHT_AI_VERSION && h.situations == FS_COUNT && h.tactics > 0 &&
	                h.tactics <= 1024 && e->size == sizeof(h) + size_t(h.tactics) * FS_COUNT * FIGHT_AI_MOVES;
	if (ok) {
		static_assert(FIGHT_AI_MOVES == 6, "a table has six moves");
		const u8* moves = blob + sizeof(h);
		for (u32 t = 0; t < TACTICS; ++t) {
			FaiTable& table = _fai[t];
			table.read = true; // (a tactic the file does not have has no moves)
			memset(table.moves, 0, sizeof(table.moves));
			if (t >= h.tactics) continue;
			for (u32 sit = 0; sit < FS_COUNT; ++sit)
				for (u32 k = 0; k < FIGHT_AI_MOVES; ++k) {
					const u8 m = moves[(size_t(t) * FS_COUNT + sit) * FIGHT_AI_MOVES + k];
					table.moves[sit][k] = m < FM_COUNT ? m : u8(FM_NONE);
				}
		}
		_stats.fightAi = 'F';
	}
	free(blob);
	return ok;
}

// ---- geometry ----

float Npcs::distanceTo(const NpcActor& a, u32 target) const {
	if (target >= _game.npcCount()) return 1e30f;
	const GameNpc& n = _game.npc(a.npc);
	const GameNpc& o = _game.npc(target);
	const float dx = o.pos[0] - n.pos[0], dz = o.pos[2] - n.pos[2], dy = o.pos[1] - n.pos[1];
	// (Heights count a little: a fighter on a ledge is out of reach.)
	return sqrtf(dx * dx + dz * dz + 0.25f * dy * dy);
}

bool Npcs::inFocus(const NpcActor& a, u32 target, float halfAngle) const {
	if (target >= _game.npcCount()) return false;
	const GameNpc& n = _game.npc(a.npc);
	const GameNpc& o = _game.npc(target);
	const float dx = o.pos[0] - n.pos[0], dz = o.pos[2] - n.pos[2];
	if (dx * dx + dz * dz < 1.0f) return true;
	return fabsf(wrap(atan2f(dx, dz) - n.yaw)) <= halfAngle;
}

bool Npcs::targetValid(const NpcActor& a) const {
	const GameNpc& n = _game.npc(a.npc);
	if (n.target < 0 || u32(n.target) >= _game.npcCount() || u32(n.target) == a.npc) return false;
	const GameNpc& t = _game.npc(u32(n.target));
	return t.placed && t.world == n.world && t.down == DOWN_NONE;
}

// About to strike: in the part of an attack before its blow lands.
bool Npcs::prehit(u32 npc) const {
	const GameNpc* hero = _game.hero();
	if (hero && npc == u32(hero - &_game.npc(0))) return _heroLink.prehit;
	const NpcActor* a = actorOf(npc);
	if (!a || !a->blow.clip || a->anim.current() != a->blow.clip) return false;
	const Clip* c = a->anim.current();
	const float hit = clipBlowTime(*c, a->blow.combo);
	return hit >= 0.0f && a->anim.time() < hit;
}

// ---- animations of a weapon in hand ----

// "S_%sRUN" with the word of the weapon in hand; without it, or with the
// fists', if the model has no such animation (as OpenGothic's solveFrm).
const Clip* Npcs::weaponClip(NpcActor& a, const char* pattern) {
	const GameNpc& n = _game.npc(a.npc);
	const char* word = n.weapon != WEAPON_NONE ? weaponWord(n.weapon) : (a.fist ? "FIST" : "");
	const char* const tries[3] = {word, "", "FIST"};
	for (const char* w : tries) {
		char name[48];
		snprintf(name, sizeof(name), pattern, w, w);
		if (const Clip* c = clip(a, name)) return c;
	}
	return nullptr;
}

void Npcs::fightIdle(NpcActor& a, float blend) {
	const Clip* c = weaponClip(a, a.walkMode == NpcActor::WALK ? "S_%sWALK" : "S_%sRUN");
	if (!c) c = weaponClip(a, "S_%sRUN");
	if (!c) {
		fallBack(a, blend);
		return;
	}
	a.idle = c;
	a.anim.play(c, blend);
	a.carried = false;
}

// Takes a weapon in hand (or puts it away: WEAPON_NONE), through the model's
// animations for it; the hands change half way, where the script says.
bool Npcs::startDraw(NpcActor& a, u8 mode) {
	GameNpc& n = _game.npcMut(a.npc);
	if (mode == n.weapon) return false;
	if (a.phase == NpcActor::MOVING) stopWalking(a);
	const Clip* c = nullptr;
	bool backwards = false;
	// The clips as for the player's character (Hero::draw): the draw itself, its alias backwards.
	const auto named = [&](const char* pattern, u8 m) {
		const char* w = weaponWord(m);
		char name[40];
		snprintf(name, sizeof(name), pattern, w, w);
		return clip(a, name);
	};
	if (mode == WEAPON_NONE) {
		c = named("T_%sRUN_2_%s", n.weapon);
		if (!c) c = named("T_%sMOVE_2_MOVE", n.weapon);
		if (!c) {
			c = named("T_%s_2_%sRUN", n.weapon);
			if (!c) c = named("T_RUN_2_%s", n.weapon);
			if (!c) c = named("T_MOVE_2_%sMOVE", n.weapon);
			backwards = c != nullptr;
		}
	} else {
		c = named("T_%s_2_%sRUN", mode);
		if (!c) c = named("T_RUN_2_%s", mode);
		if (!c) c = named("T_MOVE_2_%sMOVE", mode);
	}
	if (!c || a.fist) {
		// No animation for it (creatures always "fight with fists"): at once.
		n.weapon = mode;
		if (mode == WEAPON_NONE) fallBack(a, 0.2f);
		else fightIdle(a, 0.2f);
		return false;
	}
	if (backwards) a.anim.playReversed(c, 0.12f);
	else a.anim.play(c, -1.0f);
	a.carried = !backwards && c->moves();
	const ClipFight* f = c->fight();
	a.drawTo = mode;
	a.drawBack = backwards;
	const float at = f && f->fightMode != CLIP_NO_FRAME ? c->frameTime(f->fightMode) : c->duration() * 0.5f;
	a.drawAt = backwards ? c->duration() - at : at;
	a.wait = c->duration();
	// (A draw that goes through several clips: the rest follows by itself.)
	return true;
}

// The blow of an attack move: ahead, from the left, from the right.
bool Npcs::attackClipPlay(NpcActor& a, int kind) {
	const GameNpc& n = _game.npc(a.npc);
	const bool running = a.phase == NpcActor::MOVING;
	const Clip* c = nullptr;
	if (n.weapon == WEAPON_FIST || a.fist) {
		c = running ? weaponClip(a, "T_%sATTACKMOVE") : nullptr;
		if (!c) c = weaponClip(a, "S_%sATTACK");
	} else if (kind == 1) {
		c = weaponClip(a, "T_%sATTACKL");
	} else if (kind == 2) {
		c = weaponClip(a, "T_%sATTACKR");
	} else if (running) {
		c = weaponClip(a, "T_%sATTACKMOVE");
	}
	if (!c) c = weaponClip(a, "S_%sATTACK");
	if (!c) return false;
	// Carrying on with a combo: the same attack still in its window.
	const Clip* cur = a.anim.current();
	if (cur == c && a.blow.clip == c) {
		const ClipFight* f = c->fight();
		const float fps = c->header().fps;
		if (f && a.blow.combo + 1u < f->hits && inComboWindow(f, a.blow.combo, fps, a.anim.time())) {
			const float end = blowEnd(f, a.blow.combo, fps);
			if (end > a.anim.time()) a.anim.seek(end);
			++a.blow.combo;
			const float next = blowEnd(f, a.blow.combo, fps);
			a.faiWait = next > a.anim.time() ? next - a.anim.time() : 0.2f;
			return true;
		}
	}
	if (running) stopWalking(a);
	a.anim.play(c, 0.1f);
	a.carried = c->moves();
	a.blow.reset();
	a.blow.clip = c;
	const float end = blowEnd(c->fight(), 0, c->header().fps);
	a.faiWait = end > 0.0f ? end : c->duration();
	a.phase = NpcActor::PLAYING;
	return true;
}

// ---- the fight AI ----

// The next move from the tables, as its steps.
bool Npcs::fetchMove(NpcActor& a) {
	const GameNpc& n = _game.npc(a.npc);
	const u32 tg = u32(n.target);
	const u32 tactic = u32(clamp<i32>(_game.npcInt(n, _game.vm().findMember("C_NPC", "FIGHT_TACTIC")), 0, TACTICS - 1));
	FaiTable& t = _fai[tactic];
	if (!t.read) {
		t.read = true;
		_stats.fightAi = 'S';
		memset(t.moves, 0, sizeof(t.moves));
		ScriptVm& vm = _game.vm();
		const u32 moveMember = vm.findMember("C_FIGHTAI", "MOVE");
		for (u32 s = 0; s < FS_COUNT; ++s) {
			char name[48];
			snprintf(name, sizeof(name), "%s%u", kFightSituations[s], unsigned(tactic));
			const u32 sym = vm.find(name);
			if (sym == SCRIPT_NONE || moveMember == SCRIPT_NONE || vm.symbol(sym).type != SCRIPT_INSTANCE) continue;
			ScriptInstance* inst = vm.create(sym);
			if (!inst) continue;
			for (u32 k = 0; k < 6 && k < vm.symbol(moveMember).count; ++k) t.moves[s][k] = moveOf(vm.getInt(moveMember, k, inst));
			vm.destroy(inst);
		}
	}
	const auto pick = [&](u32 situation) -> u8 {
		u32 count = 0;
		while (count < 6 && t.moves[situation][count] != FM_NONE) ++count;
		return count ? t.moves[situation][_game.dice(count)] : u8(FM_NONE);
	};
	u8 move = FM_NONE;
	if (a.hitFlag) {
		a.hitFlag = false;
		move = pick(FS_MY_W_STRAFE);
	}
	const float dist = distanceTo(a, tg);
	const float wRange = _game.meleeRange(a.npc, tg), gRange = _game.approachRange(a.npc, tg);
	const bool focus = inFocus(a, tg, 0.52f);
	const bool running = a.phase == NpcActor::MOVING;
	if (move == FM_NONE && prehit(tg)) {
		// The enemy is about to strike, and is close and turned this way.
		const NpcActor* other = actorOf(tg);
		const bool itFaces = other ? inFocus(*other, a.npc, 0.52f) : true;
		const bool itRuns = other ? other->phase == NpcActor::MOVING : _heroLink.running;
		if (dist <= _game.meleeRange(tg, a.npc) && itFaces && focus) move = pick(itRuns ? FS_ENEMY_STORMPREHIT : FS_ENEMY_PREHIT);
		if (move == FM_NONE && itRuns) move = pick(FS_ENEMY_PREHIT);
	}
	if (move == FM_NONE && rangedWeapon(n)) {
		// A bow or a spell in hand (OpenGothic's FightAlgo: the "FK" situations, the far fight): within its reach and
		// turned to the enemy, the table's moves for that; else those for out of reach. Without a table: closer, turn,
		// shoot.
		const float reach = rangedReach(n);
		if (dist <= reach && focus) move = pick(FS_MY_FK_FOCUS);
		if (move == FM_NONE) move = pick(FS_MY_G_FK_NOFOCUS);
		// (A table of melee moves for the far fight: a blow is a shot, a parry or a hop is no use at a distance.)
		if (move == FM_PARADE || move == FM_SIDEATTACK || move == FM_FRONTATTACK || move == FM_TRIPLEATTACK || move == FM_WHIRLATTACK ||
		    move == FM_MASTERATTACK)
			move = FM_ATTACK;
		if (move == FM_NONE || (move == FM_RUN && dist <= reach * 0.8f) || ((move == FM_ATTACK) && dist > reach))
			move = dist > reach ? FM_RUN : focus ? FM_ATTACK : FM_TURN;
	}
	if (move == FM_NONE && dist <= wRange) move = pick(focus ? (running ? FS_MY_W_RUNTO : FS_MY_W_FOCUS) : FS_MY_W_NOFOCUS);
	if (move == FM_NONE && dist <= gRange && focus) move = pick(running ? FS_MY_G_RUNTO : FS_MY_G_FOCUS);
	if (move == FM_NONE) move = pick(FS_MY_G_FK_NOFOCUS);
	if (move == FM_NONE) move = pick(FS_MY_W_NOFOCUS);
	if (move == FM_NONE) {
		// No table for this tactic at all: go for it - closer, turned to it,
		// and a blow once within reach (closing in for ever is no fight).
		++_stats.movesUntabled;
		move = dist > wRange ? FM_RUN : focus ? FM_ATTACK : FM_TURN;
	}
	// Very close and facing it: waiting or turning is a hop back instead
	// (as in the original, which keeps a wolf from standing in one's face).
	const float closeup = 0.75f * float(_game.guildValue("FIGHT_RANGE_BASE", _game.guildOf(n), 60) +
	                                    _game.guildValue("FIGHT_RANGE_BASE", _game.guildOf(_game.npc(tg)), 60));
	if ((move == FM_WAIT || move == FM_WAITLONGER || move == FM_TURN) && dist < closeup && focus) move = FM_JUMPBACK;
	u8* s = a.steps;
	u8 count = 0;
	const bool coin = _game.dice(2) != 0;
	switch (move) {
	case FM_RUN: s[count++] = STEP_MOVE; break;
	case FM_TURN:
	case FM_RUNBACK:
	case FM_TURNTOHIT:
	case FM_STANDUP: s[count++] = STEP_TURN; break;
	case FM_JUMPBACK: s[count++] = STEP_JUMPBACK; break;
	case FM_STRAFE: s[count++] = coin ? STEP_STRAFEL : STEP_STRAFER; break;
	case FM_ATTACK: s[count++] = STEP_ATTACK; break;
	case FM_SIDEATTACK:
		s[count++] = STEP_ATTACKL;
		s[count++] = STEP_ATTACKR;
		break;
	case FM_FRONTATTACK:
		s[count++] = coin ? STEP_ATTACKL : STEP_ATTACKR;
		s[count++] = STEP_ATTACK;
		break;
	case FM_TRIPLEATTACK:
		if (coin) {
			s[count++] = STEP_ATTACK;
			s[count++] = STEP_ATTACKR;
			s[count++] = STEP_ATTACKL;
		} else {
			s[count++] = STEP_ATTACKL;
			s[count++] = STEP_ATTACKR;
			s[count++] = STEP_ATTACK;
		}
		break;
	case FM_WHIRLATTACK:
		for (int k = 0; k < 4; ++k) s[count++] = (k & 1) ? STEP_ATTACKR : STEP_ATTACKL;
		break;
	case FM_MASTERATTACK:
		s[count++] = STEP_ATTACKL;
		s[count++] = STEP_ATTACKR;
		for (int k = 0; k < 4; ++k) s[count++] = STEP_ATTACK;
		break;
	case FM_PARADE: s[count++] = STEP_BLOCK; break;
	case FM_WAITLONGER: s[count++] = STEP_WAITLONG; break;
	default: s[count++] = STEP_WAIT; break;
	}
	a.stepCount = count;
	a.stepAt = 0;
	++_stats.moves;
	return count > 0;
}

// A step of the move in hand. True once it is done.
bool Npcs::doStep(NpcActor& a, const World& world, float dt) {
	GameNpc& n = _game.npcMut(a.npc);
	const u32 tg = u32(n.target);
	const GameNpc& t = _game.npc(tg);
	const float dx = t.pos[0] - n.pos[0], dz = t.pos[2] - n.pos[2];
	const float toward = atan2f(dx, dz);
	const float dist = distanceTo(a, tg);
	const float wRange = _game.meleeRange(a.npc, tg);
	(void)world;
	const auto playStep = [&](const char* pattern, const char* orPattern) {
		const Clip* c = weaponClip(a, pattern);
		if (!c && orPattern) c = weaponClip(a, orPattern);
		if (!c) return false;
		if (a.phase == NpcActor::MOVING) stopWalking(a);
		a.anim.play(c, 0.1f);
		a.carried = c->moves();
		a.faiWait = c->duration();
		a.phase = NpcActor::PLAYING;
		return true;
	};
	switch (a.steps[a.stepAt]) {
	case STEP_ATTACK:
	case STEP_ATTACKL:
	case STEP_ATTACKR: {
		// Turned towards it first, nearly exactly.
		if (!inFocus(a, tg, 0.09f) && dist > 1.0f) {
			turn(a, toward, 6.0f, dt);
			return false;
		}
		if (rangedWeapon(n)) {
			if (a.phase == NpcActor::MOVING) stopWalking(a);
			startShot(a);
			return true;
		}
		const int kind = a.steps[a.stepAt] == STEP_ATTACKL ? 1 : a.steps[a.stepAt] == STEP_ATTACKR ? 2 : 0;
		// A blow to the side is for one who is within reach.
		if (kind != 0 && dist > wRange) return true;
		attackClipPlay(a, kind);
		return true;
	}
	case STEP_BLOCK:
		if (!inFocus(a, tg, 0.52f)) return true;
		playStep("T_%sPARADE_O", "T_%sPARADE_0");
		return true;
	case STEP_JUMPBACK:
		if (!playStep("T_%sJUMPB", "T_%sPARADEJUMPB")) playStep("T_JUMPB", nullptr);
		return true;
	case STEP_STRAFEL:
		playStep("T_%sRUNSTRAFEL", nullptr);
		return true;
	case STEP_STRAFER:
		playStep("T_%sRUNSTRAFER", nullptr);
		return true;
	case STEP_WAIT:
	case STEP_WAITLONG:
		if (a.phase == NpcActor::MOVING) stopWalking(a);
		a.faiWait = a.steps[a.stepAt] == STEP_WAIT ? 0.2f : 0.3f;
		return true;
	case STEP_MOVE:
	case STEP_TURN: {
		// Close in until within reach of the weapon, and face it.
		if (dist <= (rangedWeapon(n) ? rangedReach(n) * 0.8f : wRange)) {
			if (a.phase == NpcActor::MOVING) stopWalking(a);
			return turn(a, toward, 6.0f, dt) || inFocus(a, tg, 0.09f);
		}
		if (a.phase != NpcActor::MOVING) {
			if (fabsf(wrap(toward - n.yaw)) > 1.2f) {
				turn(a, toward, 6.0f, dt);
				return false;
			}
			const u8 mode = a.walkMode;
			a.walkMode = NpcActor::RUN;
			startWalking(a);
			a.walkMode = mode;
			a.bestDist = dist;
			a.stuck = 0;
		}
		n.yaw = wrap(n.yaw + clamp(wrap(toward - n.yaw), -6.0f * dt, 6.0f * dt));
		const Clip* c = a.anim.current();
		if (a.anim.finished() || !c) {
			if ((c = weaponClip(a, "S_%sRUNL")) != nullptr) a.anim.play(c, 0.1f);
		}
		if (!c || !c->moves()) {
			const float step = min(420.0f * dt, dist);
			n.pos[0] += sinf(n.yaw) * step;
			n.pos[2] += cosf(n.yaw) * step;
		}
		// Not getting any closer: let the state think again.
		if (dist < a.bestDist - 1.0f) {
			a.bestDist = dist;
			a.stuck = 0;
		} else if ((a.stuck += dt) > 2.0f) {
			stopWalking(a);
			return true;
		}
		return false;
	}
	default: return true;
	}
}

// ---- the actions ----

bool Npcs::beginFight(NpcActor& a, const World& world) {
	GameNpc& n = _game.npcMut(a.npc);
	NpcAction& act = a.action;
	switch (act.kind) {
	case NPC_ACT_ATTACK:
		if (!targetValid(a) || n.down != DOWN_NONE) return true;
		// (The original's: no weapon in hand, the melee one first.)
		if (n.weapon == WEAPON_NONE) {
			const GameItemInfo* w = n.melee != SCRIPT_NONE ? _game.itemInfo(n.melee) : nullptr;
			const i32 twoHanded = _game.constant("ITEM_2HD_SWD", 1 << 16) | _game.constant("ITEM_2HD_AXE", 1 << 17);
			const u8 mode = w ? ((w->flags & twoHanded) ? WEAPON_2H : WEAPON_1H) : WEAPON_FIST;
			if (startDraw(a, mode)) {
				a.phase = NpcActor::PLAYING;
				return false; // (the attack itself on the next call)
			}
		}
		// A bow without arrows, a spell not to be had (no mana): the melee weapon instead.
		if (((n.weapon == WEAPON_BOW || n.weapon == WEAPON_CBOW) && _game.rangedModeOf(a.npc, true) == WEAPON_NONE) ||
		    (n.weapon == WEAPON_MAG && (n.spell < 0 || !_game.spellInfo(n.spell)))) {
			NpcAction again = act;
			NpcAction draw;
			draw.kind = NPC_ACT_DRAW;
			draw.i = WEAPON_NONE;
			pushFront(a, again);
			pushFront(a, draw); // (the draw first: it puts the bow away and takes the melee weapon)
			return true;
		}
		if (a.stepAt >= a.stepCount && !fetchMove(a)) return true;
		return false;
	case NPC_ACT_DRAW: {
		u8 mode = u8(act.i);
		if (mode == WEAPON_MAG && act.fn > 0) {
			n.spell = i16(i32(act.fn) - 1);
			n.spellLevel = 1;
		}
		if (mode == WEAPON_NONE) {
			const GameItemInfo* w = n.melee != SCRIPT_NONE ? _game.itemInfo(n.melee) : nullptr;
			const i32 twoHanded = _game.constant("ITEM_2HD_SWD", 1 << 16) | _game.constant("ITEM_2HD_AXE", 1 << 17);
			mode = w ? ((w->flags & twoHanded) ? WEAPON_2H : WEAPON_1H) : WEAPON_FIST;
		}
		if (n.weapon == mode) return true;
		if (n.weapon != WEAPON_NONE && n.weapon != WEAPON_FIST) {
			// Another weapon in hand: away with it first, then this one.
			NpcAction again = act;
			startDraw(a, WEAPON_NONE);
			pushFront(a, again);
			a.phase = NpcActor::PLAYING;
			return false;
		}
		if (!startDraw(a, mode)) return true;
		a.phase = NpcActor::PLAYING;
		return false;
	}
	case NPC_ACT_UNDRAW:
		if (n.weapon == WEAPON_MAG) n.spell = -1;
		if (n.weapon == WEAPON_NONE) return true;
		if (!startDraw(a, WEAPON_NONE)) return true;
		a.phase = NpcActor::PLAYING;
		return false;
	case NPC_ACT_EQUIP_BEST: {
		const u32 best = _game.bestMelee(a.npc);
		if (best != SCRIPT_NONE) _game.equip(a.npc, best);
		return true;
	}
	case NPC_ACT_DODGE: {
		const Clip* c = weaponClip(a, "T_%sJUMPB");
		if (!c) c = weaponClip(a, "T_%sPARADEJUMPB");
		if (!c) return true;
		if (a.phase == NpcActor::MOVING) stopWalking(a);
		a.anim.play(c, 0.1f);
		a.carried = c->moves();
		a.wait = c->duration();
		a.phase = NpcActor::PLAYING;
		return false;
	}
	case NPC_ACT_FLEE:
		if (n.target < 0) return true;
		a.flee = act.f > 0.0f ? act.f : 1.0f;
		return false;
	case NPC_ACT_FINISH:
		if (act.i < 0 || u32(act.i) >= _game.npcCount() || _game.npc(u32(act.i)).down != DOWN_UNCONSCIOUS) return true;
		a.bestDist = 1e30f;
		a.stuck = 0;
		return false;
	case NPC_ACT_WHIRL:
	case NPC_ACT_TURN_AWAY:
		if (act.i < 0 || u32(act.i) >= _game.npcCount() || !_game.npc(u32(act.i)).placed) return true;
		a.phase = NpcActor::TURNING;
		return false;
	default: (void)world; return true;
	}
}

bool Npcs::carryOnFight(NpcActor& a, const World& world, float dt) {
	GameNpc& n = _game.npcMut(a.npc);
	NpcAction& act = a.action;
	switch (act.kind) {
	case NPC_ACT_ATTACK: {
		if (n.down != DOWN_NONE) return true;
		if (a.drawTo != 0xFF) return false; // (taking the weapon in hand)
		if (!targetValid(a)) {
			a.stepCount = a.stepAt = 0;
			if (a.phase == NpcActor::MOVING) stopWalking(a);
			return true;
		}
		const GameNpc& t = _game.npc(u32(n.target));
		const float toward = atan2f(t.pos[0] - n.pos[0], t.pos[2] - n.pos[2]);
		if (a.stumble > 0.0f) return false;
		if (a.faiWait > 0.0f) {
			// Meanwhile it keeps turned towards the enemy (not while a blow is
			// being swung: that goes where it was aimed).
			if (!a.blow.clip) turn(a, toward, 3.0f, dt);
			return false;
		}
		if (a.stepAt >= a.stepCount) return true; // the move is done: the state's loop decides what next
		if (!doStep(a, world, dt)) return false;
		++a.stepAt;
		return a.stepAt >= a.stepCount && a.faiWait <= 0.0f && !a.blow.clip;
	}
	case NPC_ACT_DRAW:
	case NPC_ACT_UNDRAW:
		if (a.drawTo != 0xFF) return false;
		a.phase = NpcActor::IDLE;
		return true;
	case NPC_ACT_DODGE:
		a.wait -= dt;
		if (a.wait > 0.0f) return false;
		a.phase = NpcActor::IDLE;
		return true;
	case NPC_ACT_FLEE: {
		if (n.target < 0 || u32(n.target) >= _game.npcCount()) return true;
		const GameNpc& t = _game.npc(u32(n.target));
		const float away = atan2f(n.pos[0] - t.pos[0], n.pos[2] - t.pos[2]);
		if (a.phase != NpcActor::MOVING) {
			const u8 mode = a.walkMode;
			a.walkMode = NpcActor::RUN;
			startWalking(a);
			a.walkMode = mode;
		}
		n.yaw = wrap(n.yaw + clamp(wrap(away - n.yaw), -6.0f * dt, 6.0f * dt));
		const Clip* c = a.anim.current();
		if (a.anim.finished() || !c) {
			if ((c = modeClip(a, "S_%sL", true)) != nullptr) a.anim.play(c, 0.1f);
		}
		if (!c || !c->moves()) {
			n.pos[0] += sinf(n.yaw) * 420.0f * dt;
			n.pos[2] += cosf(n.yaw) * 420.0f * dt;
		}
		a.flee -= dt;
		if (a.flee > 0.0f) return false;
		stopWalking(a);
		return true;
	}
	case NPC_ACT_FINISH: {
		const u32 victim = u32(act.i);
		const GameNpc& v = _game.npc(victim);
		if (v.down != DOWN_UNCONSCIOUS) return true;
		const float dist = distanceTo(a, victim);
		if (dist > 180.0f) {
			if (!walkTo(a, v.pos, 150.0f, dt, true)) return false;
			stopWalking(a);
		}
		if (a.blow.clip) return false; // the blow is being swung
		if (a.wait <= 0.0f) {
			const Clip* c = weaponClip(a, "T_%sSFINISH");
			if (!c) c = weaponClip(a, "S_%sATTACK");
			if (c) {
				a.anim.play(c, 0.1f);
				a.wait = c->duration();
				a.phase = NpcActor::PLAYING;
			}
			goDown(victim, int(a.npc), true, false);
			a.wait = max(a.wait, 0.01f);
			return false;
		}
		a.wait -= dt;
		if (a.wait > 0.0f) return false;
		a.phase = NpcActor::IDLE;
		return true;
	}
	case NPC_ACT_WHIRL:
	case NPC_ACT_TURN_AWAY: {
		const GameNpc& o = _game.npc(u32(act.i));
		float yaw = atan2f(o.pos[0] - n.pos[0], o.pos[2] - n.pos[2]);
		if (act.kind == NPC_ACT_TURN_AWAY) yaw = wrap(yaw + kPi);
		if (!turn(a, yaw, act.kind == NPC_ACT_WHIRL ? 9.0f : 3.2f, dt)) return false;
		a.phase = NpcActor::IDLE;
		return true;
	}
	default: return true;
	}
}

// Each frame: the hands changing on a draw, a blow landing or ending, reeling.
void Npcs::tickFight(NpcActor& a, float dt) {
	GameNpc& n = _game.npcMut(a.npc);
	const Clip* c = a.anim.current();
	const float t = a.anim.time();
	const bool sameClip = c == a.animClip;
	const float stumbleBefore = a.stumble;
	if (a.stumble > -1.0f) a.stumble = max(a.stumble - dt, -1.0f);
	if (a.faiWait > 0.0f) a.faiWait -= dt;
	if (a.refuseTalk > 0.0f) a.refuseTalk -= dt;
	if (stumbleBefore > 0.0f && a.stumble <= 0.0f && n.down == DOWN_NONE && !a.blow.clip && a.drawTo == 0xFF && n.weapon != WEAPON_NONE) {
		// Done reeling: back on its feet in the fighting stance, not frozen in the last frame.
		a.faiWait = 0;
		fightIdle(a, 0.15f);
	}
	// Watchdog: a fight action that goes nowhere (nothing playing, nothing walking, no blow, no
	// reeling, no wait) for a while is dropped, so that the state's loop picks the fight up again.
	if (a.action.kind == NPC_ACT_ATTACK && a.phase != NpcActor::MOVING && !a.blow.clip && a.stumble <= 0.0f && a.faiWait <= 0.0f) {
		if ((a.fightStall += dt) > 1.5f) {
			a.fightStall = 0;
			a.stepCount = a.stepAt = 0;
			a.drawTo = 0xFF;
			a.drawBack = false;
			a.phase = NpcActor::IDLE;
			a.action = NpcAction();
			fightIdle(a, 0.1f);
		}
	} else {
		a.fightStall = 0;
	}
	if (n.weapon != a.heardWeapon) {
		if (n.down == DOWN_NONE) emitDraw(a.npc, a.heardWeapon, n.weapon, n.melee);
		a.heardWeapon = n.weapon;
	}
	if (a.drawTo != 0xFF) {
		// The hands change where the animation says (or when it is over).
		const float played = a.drawBack && c ? c->duration() - t : t;
		const float playedBefore = a.drawBack && c ? c->duration() - a.animPrev : a.animPrev;
		if ((sameClip && playedBefore < a.drawAt && played >= a.drawAt) || !sameClip || a.anim.finished() || a.drawAt <= 0.0f) n.weapon = a.drawTo;
		if (a.anim.finished() || (c && c->loops())) {
			n.weapon = a.drawTo;
			a.drawTo = 0xFF;
			a.drawBack = false;
			if (n.weapon == WEAPON_NONE) fallBack(a, 0.2f);
			else fightIdle(a, 0.2f);
		}
	}
	if (a.shotClip) {
		if (c != a.shotClip || n.down != DOWN_NONE || !rangedWeapon(n)) {
			a.shotClip = nullptr; // (struck, put away: it does not go off)
		} else if (t >= a.shotAt && (!sameClip || a.animPrev < a.shotAt || a.anim.finished())) {
			fireShot(a);
			a.shotClip = nullptr;
		}
	}
	if (a.blow.clip) {
		if (c != a.blow.clip) {
			a.blow.reset();
		} else {
			const ClipFight* f = c->fight();
			const float fps = c->header().fps;
			const float hit = clipBlowTime(*c, a.blow.combo);
			if (hit >= 0.0f && a.blow.landed <= a.blow.combo && t >= hit && (!sameClip || a.animPrev < hit || a.animPrev > t)) {
				a.blow.landed = u8(a.blow.combo + 1);
				// It lands on the target if that is within reach and in front.
				if (targetValid(a)) {
					const u32 tg = u32(n.target);
					if (distanceTo(a, tg) <= _game.meleeRange(a.npc, tg) + 30.0f && inFocus(a, tg, 0.8f)) {
						const GameNpc& v = _game.npc(tg);
						const float back = fabsf(wrap(atan2f(n.pos[0] - v.pos[0], n.pos[2] - v.pos[2]) - v.yaw));
						strike(a.npc, tg, back > kPi * 0.5f);
					}
				}
			}
			const float end = blowEnd(f, a.blow.combo, fps);
			if ((end >= 0.0f && t >= end && a.faiWait <= 0.0f) || a.anim.finished()) {
				// Not carried on: the attack stops where the blow ends.
				a.blow.reset();
				if (n.down == DOWN_NONE) fightIdle(a, 0.15f);
				if (a.phase == NpcActor::PLAYING && a.action.kind == NPC_ACT_ATTACK) a.phase = NpcActor::IDLE;
			}
		}
	}
	// Knocked out, and the scripts have no state for it: up again after a while.
	if (n.down == DOWN_UNCONSCIOUS && _fnUnconscious == SCRIPT_NONE && a.stateTime > 20.0f) {
		n.down = DOWN_NONE;
		standUp(a);
	}
}

// ---- a blow lands ----

i32 Npcs::strike(u32 attacker, u32 victim, bool fromBehind) {
	if (attacker >= _game.npcCount() || victim >= _game.npcCount() || attacker == victim) return -1;
	GameNpc& v = _game.npcMut(victim);
	if (v.down != DOWN_NONE) return -1;
	const GameNpc* hero = _game.hero();
	const u32 heroIndex = hero ? u32(hero - &_game.npc(0)) : 0xFFFFFFFFu;
	if (victim == heroIndex) {
		// The player's character: the application decides (parry, damage, down).
		if (!_heroLink.hit) return -1;
		_heroLink.hit(_heroLink.user, attacker);
		++_stats.blows;
		return 0;
	}
	NpcActor* a = nullptr;
	for (NpcActor& x : _actors)
		if (x.used && x.npc == victim) a = &x;
	const GameNpc& att = _game.npc(attacker);
	if (a && a->ready) {
		// Fended off: a parry with the attacker in front (a beast's teeth are
		// no weapon to parry), or a hop back.
		const Clip* c = a->anim.current();
		const bool beast = att.weapon == WEAPON_FIST && _game.guildOf(att) >= _game.constant("GIL_SEPERATOR_HUM", 16);
		if (c && !beast && inParry(c->fight(), c->header().fps, a->anim.time()) && inFocus(*a, attacker, 1.05f)) {
			++_stats.parried;
			emitParry(victim);
			sendPerception(_percFightSound, victim, int(attacker), int(victim));
			return -1;
		}
		if (c && a->stateTime >= 0.0f && (strstr(animationName(*a), "JUMPB") != nullptr) && a->anim.time() < c->duration() * 0.7f &&
		    !fromBehind) {
			++_stats.parried;
			return -1;
		}
	}
	v.lastHit = int(attacker);
	// The victim takes note first (and may begin a state of its own), then the others hear it.
	const u32 stateBefore = a ? a->state : 0u;
	u8 queuedBefore = a ? a->queueCount : u8(0);
	const bool listens = a && a->ready && _percDamage >= 0 && _percDamage < Game::PERC_TYPES && a->perc[_percDamage] && _cfg.alive;
	const u32 unboundBefore = _unboundCount;
	if (listens && !inFightState(*a) && !a->blow.clip) {
		// Hit out of a quiet state: what it still had in its queue (a wait, a walk, a word) is
		// forgotten, so the reaction - and the fight - begins at once, as in the original.
		clearQueue(*a);
		queuedBefore = 0;
	}
	if (listens) react(*a, a->perc[_percDamage], int(attacker), int(victim));
	if (victim != heroIndex && attacker == heroIndex) {
		// (For the debug line: did it hear the blow, what did it do about it.)
		const char* before = stateName(stateBefore);
		const char* after = a ? stateName(a->state) : "";
		if (strncmp(before, "ZS_", 3) == 0) before += 3;
		if (strncmp(after, "ZS_", 3) == 0) after += 3;
		snprintf(_lastHitNote, sizeof(_lastHitNote), "hit %.9s L%d %.10s>%.10s q%d a%d u%d", _game.nameOf(v), listens ? 1 : 0, before, after,
		         a ? int(a->queueCount) : -1, int(_game.attitude(victim, heroIndex)), int(_unboundCount - unboundBefore));
		_lastHitAge = 0.0f;
		_lastHitNpc = int(victim);
	}
	sendPerception(_percFightSound, victim, int(attacker), int(victim));
	if (a) {
		a->hitFlag = true;
		if (!a->blow.clip) {
			// (A blow of its own already on its way goes on.)
			a->stepCount = a->stepAt = 0;
			a->faiWait = 0;
		}
	}
	const i32 dmg = _game.meleeDamage(attacker, victim);
	++_stats.blows;
	emitHit(attacker, victim);
	if (a && a->ready && a->mob < 0 && dmg > 0 && a->stumble <= -0.75f) {
		// Reeling: from in front back, from behind forward. (A blow that does
		// no harm - armour - does not stagger, nor does one that comes while
		// still reeling: three beasts cannot keep a guard from striking back.)
		const Clip* c = clip(*a, fromBehind ? "T_STUMBLE" : "T_STUMBLEB");
		if (!c) c = clip(*a, "T_GOTHIT");
		if (c && !a->blow.clip) {
			if (a->phase == NpcActor::MOVING) stopWalking(*a);
			a->anim.play(c, 0.05f);
			a->carried = c->moves();
			a->stumble = c->duration();
		}
	}
	const i32 immortal = _game.constant("NPC_FLAG_IMMORTAL", 1 << 1);
	const bool cannotDie = (_game.npcInt(v, _game.vm().findMember("C_NPC", "FLAGS")) & immortal) != 0;
	i32 hp = _game.hitPoints(v);
	if (!cannotDie) hp = max(hp - dmg, i32(0));
	_game.setAttribute(v, 0, hp);
	// Down? A person beaten by somebody not hostile to it is only knocked out.
	const bool human = _game.guildOf(v) < _game.constant("GIL_SEPERATOR_HUM", 16);
	if (hp <= (human ? 1 : 0)) {
		if (human && _game.attitude(victim, attacker) != _game.constant("ATT_HOSTILE", 0)) {
			_game.setAttribute(v, 0, max(hp, i32(1)));
			goDown(victim, int(attacker), false, fromBehind);
		} else if (hp <= 0) {
			goDown(victim, int(attacker), true, fromBehind);
		}
	} else {
		sendPerception(_percOthersDamage, victim, int(attacker), int(victim));
		if (dmg > 0 && (_stats.blows & 1)) emitSvm(victim, "AARGH"); // (every other blow, as OpenGothic's rand(2))
	}
	if (a) a->hitFromBehind = fromBehind;
	// A creature that was hit and whose scripts did nothing about it (they listen for no
	// damage in the state it is in, or let it be) turns on whoever hit it.
	if (a && a->ready && v.down == DOWN_NONE && _cfg.alive) {
		const bool answered = listens && (a->state != stateBefore || a->queueCount != queuedBefore || inFightState(*a) || a->flee > 0.0f);
		if (!answered) provoke(*a, int(attacker));
	}
	return dmg;
}

void Npcs::goDown(u32 npc, int attacker, bool dead, bool fromBehind) {
	if (npc >= _game.npcCount()) return;
	GameNpc& n = _game.npcMut(npc);
	if (n.down == DOWN_DEAD || (!dead && n.down == DOWN_UNCONSCIOUS)) return;
	n.down = dead ? DOWN_DEAD : DOWN_UNCONSCIOUS;
	n.weapon = WEAPON_NONE;
	n.lastHit = attacker;
	if (dead) _game.setAttribute(n, 0, 0);
	(dead ? _stats.killed : _stats.knockedOut) += 1;
	emitSvm(npc, dead ? "DEAD" : "AARGH");
	NpcActor* a = nullptr;
	for (NpcActor& x : _actors)
		if (x.used && x.npc == npc) a = &x;
	if (a) {
		clearQueue(*a);
		releaseMob(*a);
		a->blow.reset();
		a->drawTo = 0xFF;
		a->stepCount = a->stepAt = 0;
		a->stumble = -1.0f;
		a->flee = 0;
		a->lookAt = -1;
		for (u32& p : a->perc) p = 0;
		// Down where it stands: from in front backwards, from behind forwards.
		const char* tr = dead ? (fromBehind ? "T_DEAD" : "T_DEADB") : (fromBehind ? "T_STAND_2_WOUNDED" : "T_STAND_2_WOUNDEDB");
		const char* in = dead ? (fromBehind ? "DEAD" : "DEADB") : (fromBehind ? "WOUNDED" : "WOUNDEDB");
		char held[16];
		snprintf(held, sizeof(held), "S_%s", in);
		const Clip* c = clip(*a, tr, dead);
		if (!c) {
			// (A model with only one way of falling.)
			c = clip(*a, dead ? "T_DEAD" : "T_STAND_2_WOUNDED", dead);
			in = dead ? "DEAD" : "WOUNDED";
		}
		snprintf(a->stance, sizeof(a->stance), "%s", in);
		if (!c) c = clip(*a, held, dead);
		if (c) a->anim.play(c, 0.1f);
		a->noLying = dead && !c;
		a->slump = 0.0f;
		if (!c) {
			// No way of falling in the model: the blow, the run, the bite it was in
			// ends; it stands still (and a dead one is tipped over, see npcs.cpp).
			const Clip* calm = modeClip(*a, "S_%s", true);
			if (calm) {
				a->anim.play(calm, 0.1f);
				a->idle = calm;
			}
		}
		a->carried = false;
		a->phase = NpcActor::IDLE;
		a->bodyState = -1;
		// The state of being down, as the original starts it: without ending
		// the one it was in (its end would have it walk off).
		const u32 fn = dead ? _fnDead : _fnUnconscious;
		if (fn != SCRIPT_NONE && _cfg.alive) {
			clearState(*a, false);
			startState(*a, fn, false, nullptr, false);
			a->other = attacker;
		} else {
			clearState(*a, false);
		}
	}
	sendPerception(dead ? _percMurder : _percDefeat, npc, attacker, int(npc));
}

// ---- the sounds of fighting ----

namespace {

// The scripts' MAT_ numbers as the sound names spell them.
const char* materialTag(i32 m) {
	switch (m) {
	case 0: return "WO";
	case 1: return "ST";
	case 2: return "ME";
	case 3: return "LE";
	case 4: return "CL";
	case 5: return "GL";
	default: return "UD";
	}
}

} // namespace

void Npcs::emitSound(u32 npc, const char* names, float gain) {
	if (_namedSoundHook.fn && npc < _game.npcCount()) _namedSoundHook.fn(_namedSoundHook.user, names, int(npc), gain);
}

bool Npcs::targetsHero() const {
	const u32 hero = _game.heroIndex();
	if (hero == SCRIPT_NONE) return false;
	for (const NpcActor& a : _actors) {
		if (!a.used || !a.ready || a.npc == hero) continue;
		const GameNpc& n = _game.npc(a.npc);
		if (n.target != int(hero) || n.down != DOWN_NONE || !inFightState(a)) continue;
		if (n.weapon == WEAPON_NONE && !_game.isMonster(n)) continue;
		return true;
	}
	return false;
}

void Npcs::emitSvm(u32 npc, const char* what) {
	if (npc >= _game.npcCount()) return;
	const i32 voice = _game.npcInt(_game.npc(npc), _game.vm().findMember("C_NPC", "VOICE"));
	if (voice <= 0) return;
	char name[40];
	snprintf(name, sizeof(name), "SVM_%ld_%s", static_cast<long>(voice), what);
	emitSound(npc, name);
}

void Npcs::emitHit(u32 attacker, u32 victim) {
	if (attacker >= _game.npcCount() || victim >= _game.npcCount()) return;
	const GameNpc& a = _game.npc(attacker);
	const GameNpc& v = _game.npc(victim);
	// What it is hit on: metal or wooden armour, else flesh (the only ones the originals have).
	const char* armour = "FL";
	if (v.armorItem != SCRIPT_NONE) {
		const i32 m = _game.itemMaterial(v.armorItem);
		if (m == 0 || m == 2) armour = materialTag(m);
	}
	char names[96];
	if ((a.weapon == WEAPON_1H || a.weapon == WEAPON_2H) && a.melee != SCRIPT_NONE) {
		const char* w = materialTag(_game.itemMaterial(a.melee));
		snprintf(names, sizeof(names), "CS_IAM_%s_%s|CS_IAM_%s_FL|CS_IAM_UD_FL|CS_IAM", w, armour, w);
	} else {
		// Fists, or a beast's teeth and claws ("JA": jaws).
		const bool beast = _game.isMonster(a);
		snprintf(names, sizeof(names), "CS_MAM_%s_%s|CS_MAM_%s_FL|CS_MAM", beast ? "JA" : "FI", armour, beast ? "JA" : "FI");
	}
	emitSound(victim, names);
}

void Npcs::emitParry(u32 npc) { emitSound(npc, "CS_IAI_ME_ME|CS_IAI"); }

void Npcs::emitDraw(u32 npc, u8 from, u8 to, u32 weaponItem) {
	const bool melee = to == WEAPON_1H || to == WEAPON_2H || from == WEAPON_1H || from == WEAPON_2H;
	if (!melee || weaponItem == SCRIPT_NONE) return;
	const char* m = _game.itemMaterial(weaponItem) == 2 ? "ME" : "WO";
	char names[96];
	if (to == WEAPON_NONE) // (Gothic 1 has no sound of its own for putting away: the drawing's, softer)
		snprintf(names, sizeof(names), "UNDRAWSOUND_%s|DRAWSOUND_%s", m, m);
	else
		snprintf(names, sizeof(names), "DRAWSOUND_%s|DRAWSOUND_ME", m);
	emitSound(npc, names, to == WEAPON_NONE ? 0.7f : 1.0f);
}

// ---- perceptions sent by the engine ----

void Npcs::sendPerception(const char* percName, u32 sender, int other, int victim) {
	sendPerception(_game.constant(percName, -1), sender, other, victim);
}

void Npcs::sendPerception(i32 perc, u32 sender, int other, int victim) {
	if (perc < 0 || perc >= Game::PERC_TYPES || !_cfg.perceive || !_cfg.alive || sender >= _game.npcCount() || other < 0) return;
	const GameNpc& s = _game.npc(sender);
	const float range = _game.perceptionRange(perc);
	static int depth = 0;
	if (depth > 0) return;
	++depth;
	for (NpcActor& a : _actors) {
		if (!a.used || !a.ready || a.npc == sender || !a.perc[perc] || (a.state && !a.started)) continue;
		const GameNpc& n = _game.npc(a.npc);
		if (n.world != s.world || n.down != DOWN_NONE) continue;
		const float r = range >= 0.0f ? range : _game.sensesRange(n);
		const float dx = n.pos[0] - s.pos[0], dy = n.pos[1] - s.pos[1], dz = n.pos[2] - s.pos[2];
		if (dx * dx + dy * dy + dz * dz > r * r) continue;
		react(a, a.perc[perc], other, victim);
	}
	--depth;
}

void Npcs::noticeNow(u32 source) {
	if (!_cfg.perceive || source >= _game.npcCount()) return;
	const GameNpc& s = _game.npc(source);
	for (NpcActor& a : _actors) {
		if (!a.used || !a.ready || a.npc == source) continue;
		const GameNpc& n = _game.npc(a.npc);
		if (n.world != s.world || n.down != DOWN_NONE) continue;
		const float r = _game.sensesRange(n);
		const float dx = n.pos[0] - s.pos[0], dy = n.pos[1] - s.pos[1], dz = n.pos[2] - s.pos[2];
		if (r > 0.0f && dx * dx + dy * dy + dz * dz <= r * r) a.percWait = 0.0f;
	}
}

void Npcs::perceiveSingle(u32 receiver, const char* percName, int other) {
	const i32 perc = _game.constant(percName, -1);
	if (perc < 0 || perc >= Game::PERC_TYPES || !_cfg.perceive || !_cfg.alive || other < 0) return;
	for (NpcActor& a : _actors)
		if (a.used && a.ready && a.npc == receiver && a.perc[perc] && !(a.state && !a.started) && _game.npc(a.npc).down == DOWN_NONE)
			react(a, a.perc[perc], other, -1);
}

// ---- the player's targets ----

u32 Npcs::areaTargets(const float pos[3], float range, float elev, u32* out, u32 max) const {
	const u32 hero = _game.heroIndex();
	u32 n = 0;
	for (const NpcActor& a : _actors) {
		if (!a.used || !a.ready || a.npc == hero || n >= max) continue;
		const GameNpc& g = _game.npc(a.npc);
		if (g.down != DOWN_NONE || !g.placed) continue;
		if (hero != SCRIPT_NONE && _game.attitude(a.npc, hero) == _game.constant("ATT_FRIENDLY", 3)) continue;
		const float dx = g.pos[0] - pos[0], dy = g.pos[1] - pos[1], dz = g.pos[2] - pos[2];
		if (sqrtf(dx * dx + dz * dz) > range || fabsf(dy) > elev) continue;
		out[n++] = a.npc;
	}
	return n;
}

int Npcs::pickTarget(const float pos[3], float yaw, float range, float halfAngle, int except, int side, bool hostileOnly) const {
	const GameNpc* hero = _game.hero();
	const u32 heroIndex = hero ? u32(hero - &_game.npc(0)) : 0xFFFFFFFFu;
	int best = -1;
	float bestScore = 1e30f;
	for (const NpcActor& a : _actors) {
		if (!a.used || !a.ready || int(a.npc) == except) continue;
		const GameNpc& n = _game.npc(a.npc);
		if (n.down != DOWN_NONE || !n.placed) continue;
		if (hostileOnly && heroIndex != 0xFFFFFFFFu && _game.attitude(a.npc, heroIndex) != _game.constant("ATT_HOSTILE", 0)) continue;
		const float dx = n.pos[0] - pos[0], dy = n.pos[1] - pos[1], dz = n.pos[2] - pos[2];
		const float d = sqrtf(dx * dx + dz * dz);
		if (d > range || fabsf(dy) > 400.0f) continue;
		const float off = d > 1.0f ? wrap(atan2f(dx, dz) - yaw) : 0.0f;
		if (fabsf(off) > halfAngle) continue;
		if (side > 0 && off <= 0.05f) continue;
		if (side < 0 && off >= -0.05f) continue;
		// Switching sides: the nearest by angle; else near and in the middle.
		const float score = side ? fabsf(off) : d + fabsf(off) * 300.0f;
		if (score < bestScore) {
			bestScore = score;
			best = int(a.npc);
		}
	}
	return best;
}

int Npcs::nextTarget(const NpcActor& a, bool set) {
	const GameNpc& n = _game.npc(a.npc);
	const float range = _game.sensesRange(n);
	int best = -1;
	float bestD = range * range;
	const auto consider = [&](u32 index) {
		if (index == a.npc || _game.attitude(a.npc, index) != _game.constant("ATT_HOSTILE", 0)) return;
		const GameNpc& o = _game.npc(index);
		if (!o.placed || o.world != n.world || o.down != DOWN_NONE) return;
		const float dx = o.pos[0] - n.pos[0], dy = o.pos[1] - n.pos[1], dz = o.pos[2] - n.pos[2];
		const float d = dx * dx + dy * dy + dz * dz;
		if (d >= bestD || !sees(a, index, false)) return;
		bestD = d;
		best = int(index);
	};
	for (const NpcActor& o : _actors)
		if (o.used && o.ready) consider(o.npc);
	if (const GameNpc* hero = _game.hero()) consider(u32(hero - &_game.npc(0)));
	if (set && best >= 0) _game.npcMut(a.npc).target = best;
	return best;
}

// ---- what the scripts call ----

void Npcs::xAttack(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	NpcAction act;
	act.kind = NPC_ACT_ATTACK;
	self.push(self.actorFor(vm.popInstance()), act);
}

void Npcs::xReadyMelee(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	NpcAction act;
	act.kind = NPC_ACT_DRAW;
	act.i = WEAPON_NONE; // (the melee weapon it has equipped, else the fists)
	self.push(self.actorFor(vm.popInstance()), act);
}

void Npcs::xDrawWeapon(ScriptVm& vm, void* user) { xReadyMelee(vm, user); }

void Npcs::xRemoveWeapon(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	NpcAction act;
	act.kind = NPC_ACT_UNDRAW;
	self.push(self.actorFor(vm.popInstance()), act);
}

void Npcs::xEquipBestMelee(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	ScriptInstance* inst = vm.popInstance();
	// (At once, also for those far away: it is only a matter of what they carry.)
	const int who = self.npcFor(inst);
	if (who < 0) return;
	const u32 best = self._game.bestMelee(u32(who));
	if (best != SCRIPT_NONE) self._game.equip(u32(who), best);
}

void Npcs::xDodge(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	NpcAction act;
	act.kind = NPC_ACT_DODGE;
	self.push(self.actorFor(vm.popInstance()), act);
}

void Npcs::xFlee(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	NpcAction act;
	act.kind = NPC_ACT_FLEE;
	act.f = 1.0f; // (called again and again by the state's loop)
	self.push(self.actorFor(vm.popInstance()), act);
}

void Npcs::xFinishingMove(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	NpcAction act;
	act.kind = NPC_ACT_FINISH;
	act.i = self.npcFor(vm.popInstance());
	self.push(self.actorFor(vm.popInstance()), act);
}

void Npcs::xWhirlAround(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	NpcAction act;
	act.kind = NPC_ACT_WHIRL;
	act.i = self.npcFor(vm.popInstance());
	self.push(self.actorFor(vm.popInstance()), act);
}

void Npcs::xTurnAway(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	NpcAction act;
	act.kind = NPC_ACT_TURN_AWAY;
	act.i = self.npcFor(vm.popInstance());
	self.push(self.actorFor(vm.popInstance()), act);
}

// True if an enemy was found; it becomes the target and OTHER. (Without one
// OTHER is cleared, as the original does.)
void Npcs::xGetNextTarget(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const NpcActor* a = self.actorFor(vm.popInstance());
	const int found = a ? self.nextTarget(*a, true) : -1;
	vm.setInstance(vm.otherSymbol(), found >= 0 ? self._game.npc(u32(found)).object : nullptr);
	vm.pushInt(found >= 0);
}

void Npcs::xIsNextTargetAvailable(ScriptVm& vm, void* user) {
	Npcs& self = *static_cast<Npcs*>(user);
	const NpcActor* a = self.actorFor(vm.popInstance());
	vm.pushInt(a && self.nextTarget(*a, false) >= 0);
}

// Pointing and the like: no animation for it here; the call costs nothing.
void Npcs::xNoAction(ScriptVm& vm, void*) {
	const u32 fn = vm.callee();
	const ScriptSymbol& s = vm.symbol(fn);
	for (u32 i = s.count; i > 0; --i) {
		const ScriptSymbol& p = vm.symbol(fn + i);
		if (p.type == SCRIPT_FLOAT) (void)vm.popFloat();
		else if (p.type == SCRIPT_INT || p.type == SCRIPT_FUNC) (void)vm.popInt();
		else (void)vm.popInstance();
	}
}

} // namespace g3
