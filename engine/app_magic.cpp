// Magic of the player's character, the way the original runs it (OpenGothic: Npc::drawSpell,
// beginCastSpell, tickCast, endCastSpell, commitSpell; World::shootSpell):
//
//   * A rune or a scroll is chosen (used in the inventory, D-Pad left and right in the stance) and the
//     magic stance taken in hand (WEAPON_MAG, the animations of the word "MAG").
//   * A, held, charges the spell: the scripts' Spell_ProcessMana decides, for every mana put in (one
//     every C_Spell.time_per_mana ms, the mana one point less each), whether to go on, to raise the level,
//     to cast or to stop. Letting go asks Spell_ProcessMana_Release.
//   * A cast plays the release animation, then the spell goes off: a flying one is a projectile that
//     travels 1 cm/ms and hurts whoever it meets (damage per level times level, less protection),
//     the others have done their work through the scripts while charged. A scroll is used up.
//
// The spells that change the world do it through the scripts, as in the original: teleporting (AI_Teleport),
// summoning (Wld_SpawnNpcRange in Spell_ProcessMana, fix79/80). Transforming (fix151): the scripts name the creature
// (Npc_SetActiveSpellInfo) while charging; when the spell goes off the character becomes it (heroTransform), and the
// magic key turns him back. The runes' circles are the scripts' to ask (Spell_ProcessMana).
#include "app.h"
#include "lang.h"
#include "g3ds/game.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace g3 {

namespace {

constexpr u32 NONE = SCRIPT_NONE;
constexpr float kBoltSpeed = 1000.0f;  // cm/s: the original's 1 cm per ms
constexpr float kBoltRadius = 75.0f;   // cm from a character's middle that a projectile hits
constexpr float kMaxRange = 6000.0f;
constexpr int kMaxLevel = 15;

float wrapYaw(float a) {
	if (!(fabsf(a) < 1.0e4f)) a = 0.0f; // (fix134: an angle gone to infinity would loop here for ever)
	while (a > 3.14159265f) a -= 6.2831853f;
	while (a < -3.14159265f) a += 6.2831853f;
	return a;
}

} // namespace

u32 App::boltCount() const {
	u32 n = 0;
	for (const Bolt& b : _bolts) n += b.alive;
	return n;
}

Vec3 App::handPos() const {
	const float yaw = _hero.yaw();
	return {_player.feet.x + sinf(yaw) * 45.0f, _player.feet.y + 130.0f, _player.feet.z + cosf(yaw) * 45.0f};
}

// Runes and scrolls the character carries, in the order they were first given; the one after (dir > 0) or before the chosen.
u32 App::nextSpell(int dir) const {
	const int hi = heroIndex();
	if (hi < 0) return NONE;
	const Game& g = _npcs.game();
	u32 list[64];
	u32 n = 0;
	for (u32 i = 0; i < g.itemStackCount() && n < 64; ++i) {
		const GameItem& st = g.itemStack(i);
		if (st.npc != u32(hi) || st.count <= 0) continue;
		const GameItemInfo* it = const_cast<Game&>(g).itemInfo(st.symbol);
		if (it && (it->mainflag & g.constant("ITEM_KAT_RUNE", 1 << 9))) list[n++] = st.symbol;
	}
	if (!n) return NONE;
	u32 at = n;
	for (u32 k = 0; k < n; ++k)
		if (list[k] == _spellItem) at = k;
	if (at == n) return dir >= 0 ? list[0] : list[n - 1];
	return list[(at + n + u32(dir >= 0 ? 1 : n - 1)) % n];
}

bool App::selectSpell(u32 item) {
	const int hi = heroIndex();
	if (hi < 0 || _hero.down()) return false;
	Game& g = _npcs.game();
	const GameItemInfo* it = g.itemInfo(item);
	if (!it || !(it->mainflag & g.constant("ITEM_KAT_RUNE", 1 << 9)) || !g.spellInfo(it->spell)) {
		toast(tr("Это не заклинание"));
		return false;
	}
	if (g.itemCount(u32(hi), item) <= 0) {
		toast(tr("Нет такого заклинания"));
		return false;
	}
	char why[48];
	if (!meetsNeeds(*it, why, sizeof(why))) {
		toast(why);
		return false;
	}
	if (_cast.stage != CAST_NONE) return false;
	_spellItem = item;
	_invStamp = 0;
	if (_hero.weapon() != WEAPON_MAG && !_hero.drawing()) {
		if (!heroTake(WEAPON_MAG)) {
			toast(tr("Сейчас нельзя"));
			return false;
		}
	}
	char name[48];
	scriptText(g.text(it->description ? it->description : it->name), name, sizeof(name));
	toast(name);
	return true;
}

// ---- input ----

void App::magicInput(const Input& in) {
	const int hi = heroIndex();
	if (hi < 0) return;
	Game& g = _npcs.game();
	// A creature casts nothing: the magic key turns him back (OpenGothic: the action key while transformed).
	if (heroTransformed()) {
		if (in.magic) heroTransformBack();
		return;
	}
	if (in.magic && _cast.stage == CAST_NONE) {
		if (_hero.weapon() == WEAPON_MAG) {
			heroTake(WEAPON_NONE);
		} else {
			u32 item = _spellItem;
			if (item == NONE || g.itemCount(u32(hi), item) <= 0) item = nextSpell(1);
			if (item == NONE) toast(tr("Заклинаний нет"));
			else selectSpell(item);
		}
	}
	if (_hero.weapon() != WEAPON_MAG) return;
	if (in.spellStep && _cast.stage == CAST_NONE) {
		const u32 next = nextSpell(in.spellStep);
		if (next != NONE && next != _spellItem) selectSpell(next);
	}
	if (in.attack && _cast.stage == CAST_NONE && !_hero.drawing()) castBegin();
}

// ---- casting ----

void App::castBegin() {
	const int hi = heroIndex();
	if (hi < 0 || _hero.down() || _player.swimming || _player.flying) return;
	Game& g = _npcs.game();
	const GameItemInfo* it = _spellItem != NONE ? g.itemInfo(_spellItem) : nullptr;
	if (!it || g.itemCount(u32(hi), _spellItem) <= 0) {
		_spellItem = nextSpell(1);
		toast(_spellItem == NONE ? tr("Заклинаний нет") : tr("Заклинание выбрано другое"));
		return;
	}
	const GameSpell* sp = g.spellInfo(it->spell);
	if (!sp) return;
	GameNpc& h = g.npcMut(u32(hi));
	char why[48];
	if (!meetsNeeds(*it, why, sizeof(why))) {
		toast(why);
		return;
	}
	if (g.attributeOf(h, g.constant("ATR_MANA", 2)) <= 0) {
		++_magic.noMana;
		toast(tr("Нет маны"));
		return;
	}
	_cast = Cast();
	_cast.spell = i16(it->spell);
	_cast.item = _spellItem;
	// The target: the one locked on to, else the nearest in front (flying spells aim at it).
	_cast.target = _target;
	h.spell = _cast.spell;
	h.spellLevel = 0;
	h.spellInfo = 0; // (what a transformation makes him: the scripts say while charging)
	const i32 code = g.processMana(u32(hi), _cast.target, 0, false);
	const i32 sendCast = g.constant("SPL_SENDCAST", 2), sendStop = g.constant("SPL_SENDSTOP", 3), dontInvest = g.constant("SPL_DONTINVEST", 0);
	if (code == sendStop || code == dontInvest) {
		++_magic.noMana;
		toast(tr("Нет маны"));
		h.spell = -1;
		_cast = Cast();
		return;
	}
	_cast.level = 1;
	h.spellLevel = 1;
	_cast.invested = 1;
	if (code == sendCast) {
		castRelease(false);
		return;
	}
	// Spells that are lost when the key is let go early (firebolt, fire rain, ice, summons...) have nothing to
	// charge by hand: the mana the scripts ask for is put in at once and the spell goes off with the press.
	// (Charge spells - fireball, storm, wind fist - and held ones - heal, telekinesis - keep their way.)
	{
		const char* heldNames[] = {"SPL_HEAL", "SPL_TELEKINESIS", "SPL_CHAINLIGHTNING", "SPL_PYROKINESIS"};
		bool held = false;
		for (const char* nm : heldNames)
			if (g.constant(nm, -77) == _cast.spell) held = true;
		if (!held && g.processMana(u32(hi), _cast.target, _cast.invested, true) == sendStop) {
			bool ok = false;
			for (int guard = 0; guard < 60; ++guard) {
				if (g.attributeOf(h, 2) <= 0) break;
				const i32 c = g.processMana(u32(hi), _cast.target, _cast.invested, false);
				g.setAttribute(h, 2, max(g.attributeOf(h, 2) - 1, i32(0)));
				if (c == sendCast) {
					ok = true;
					break;
				}
				if (c == sendStop || c == dontInvest) break;
				if (c == g.constant("SPL_NEXTLEVEL", 4) && _cast.level < kMaxLevel) {
					++_cast.level;
					h.spellLevel = _cast.level;
				}
				++_cast.invested;
			}
			if (!ok) {
				++_magic.noMana;
				toast(tr("Нет маны"));
				castCancel();
				return;
			}
			castRelease(false);
			return;
		}
	}
	_cast.stage = CAST_INVEST;
	_cast.tick = 0;
	_hero.castPlay(sp->letters, false);
	if (sp->fxInit) _cast.fx = _particles.start(sp->fxInit, handPos(), _hero.yaw(), 30.0f);
}

// The charge ends: `letGo` the player let go of the key (the scripts are asked whether that casts), else the
// scripts said to cast.
void App::castRelease(bool letGo) {
	const int hi = heroIndex();
	if (hi < 0) return;
	Game& g = _npcs.game();
	const GameSpell* sp = g.spellInfo(_cast.spell);
	if (!sp) {
		castCancel();
		return;
	}
	bool cast = true;
	if (letGo) cast = g.processMana(u32(hi), _cast.target, _cast.invested, true) == g.constant("SPL_SENDCAST", 2);
	if (_cast.fx) _particles.stop(_cast.fx);
	_cast.fx = 0;
	if (!cast) {
		castCancel();
		return;
	}
	_cast.stage = CAST_SHOOT;
	_cast.timer = 0;
	_cast.length = _hero.castPlay(sp->letters, true);
	if (_cast.length <= 0.0f) _cast.length = 0.35f; // (no animation in the model: a moment)
}

void App::castCancel() {
	const int hi = heroIndex();
	if (_cast.fx) _particles.stop(_cast.fx);
	if (hi >= 0) {
		_npcs.game().npcMut(u32(hi)).spell = -1;
		_npcs.game().npcMut(u32(hi)).spellInfo = 0;
	}
	_cast = Cast();
	_hero.castEnd();
}

// The spell goes off.
void App::castCommit() {
	const int hi = heroIndex();
	if (hi < 0) return;
	Game& g = _npcs.game();
	const GameSpell* sp = g.spellInfo(_cast.spell);
	const i32 level = max(i32(_cast.level), i32(1));
	if (sp) {
		++_magic.casts;
		_magic.lastSpell = _cast.spell;
		if (sp->shoots) {
			boltShoot(*sp, _cast.spell, level);
		} else if (sp->area) {
			// Fire rain: it comes down on everything about the caster (10 m), the picture over him.
			const GameNpc& hn = g.npc(u32(hi));
			const float at[3] = {hn.pos[0], hn.pos[1], hn.pos[2]};
			if (sp->fxArea) _particles.play(sp->fxArea, Vec3 {at[0], at[1] + 250.0f, at[2]}, _hero.yaw(), 5.0f);
			else if (sp->fxCast) _particles.play(sp->fxCast, handPos(), _hero.yaw(), 1.5f);
			u32 hit[16];
			const u32 count = _npcs.areaTargets(at, float(sp->collectRange), 600.0f, hit, 16);
			for (u32 i = 0; i < count; ++i) {
				g.setSpellStrike(_cast.spell, level);
				const i32 dmg = _npcs.strike(u32(hi), hit[i], false);
				g.setSpellStrike(-1, 0);
				g.npcMut(hit[i]).lastHitSpell = _cast.spell;
				++_magic.hits;
				_magic.lastDamage = dmg;
				if (g.npc(hit[i]).down == DOWN_NONE) _npcs.perceiveSingle(hit[i], "PERC_ASSESSMAGIC", hi);
			}
		} else {
			if (sp->fxCast) _particles.play(sp->fxCast, handPos(), _hero.yaw(), 1.5f);
			if (_cast.target >= 0 && sp->collectAlgo != g.constant("TARGET_COLLECT_CASTER", 1)) {
				GameNpc& t = g.npcMut(u32(_cast.target));
				t.lastHitSpell = _cast.spell;
				_npcs.perceiveSingle(u32(_cast.target), "PERC_ASSESSMAGIC", hi);
			}
		}
	}
	// A scroll is used up.
	const GameItemInfo* it = g.itemInfo(_cast.item);
	if (it && (it->flags & g.constant("ITEM_MULTI", 1 << 21))) {
		g.giveItems(u32(hi), _cast.item, -1);
		_invStamp = 0;
		if (g.itemCount(u32(hi), _cast.item) <= 0) {
			const u32 next = nextSpell(1);
			_spellItem = next == _cast.item ? NONE : next;
			if (_spellItem == NONE) heroTake(WEAPON_NONE);
		}
	}
	g.npcMut(u32(hi)).spell = -1;
	// A transformation: the scripts named the creature while it was charged (OpenGothic's commitSpell, after the scroll).
	const i32 into = g.npc(u32(hi)).spellInfo;
	g.npcMut(u32(hi)).spellInfo = 0;
	_cast = Cast();
	_hero.castEnd();
	if (into > 0) heroTransform(u32(into));
}

// ---- transforming ----

bool App::heroTransformed() const {
	const int hi = heroIndex();
	return hi >= 0 && _npcs.loaded() && _npcs.game().transformedNpc() == hi;
}

bool App::heroTransform(u32 symbol) {
	const int hi = heroIndex();
	if (hi < 0 || !_npcs.loaded() || !_hero.loaded() || heroTransformed()) return false;
	Game& g = _npcs.game();
	if (symbol >= g.vm().symbolCount()) return false;
	// Its model, by the baker's table "meta/forms" (the machine knows its symbols by their names' hashes only).
	u32 entry = 0;
	if (const PakEntry* e = _pak.find("meta/forms")) {
		FormsHeader fh {};
		u8 buf[sizeof(FormsHeader) + 64 * sizeof(FormEntry)];
		if (e->size >= sizeof(fh) && e->size <= sizeof(buf) && _pak.read(*e, buf)) {
			memcpy(&fh, buf, sizeof(fh));
			if (fh.magic == FORMS_MAGIC && fh.version == FORMS_VERSION && e->size == sizeof(fh) + size_t(fh.count) * sizeof(FormEntry))
				for (u32 i = 0; i < fh.count && !entry; ++i) {
					FormEntry fe;
					memcpy(&fe, buf + sizeof(fh) + i * sizeof(FormEntry), sizeof(fe));
					if (fe.instance == g.vm().symbol(symbol).nameHash) entry = fe.model;
				}
		}
	}
	if (!entry || !_pak.find(entry)) {
		// (An archive baked before fix151 has no creatures for the player.)
		++_magic.noForm;
		toast(tr("Превращения нет в data.pak (нужен перепёк)"), 3.0f);
		return false;
	}
	const Vec3 feet = _player.feet;
	const float yaw = _hero.yaw();
	if (!g.transform(u32(hi), symbol)) return false;
	if (!_hero.loadEntry(_pak, entry)) {
		g.transformBack(u32(hi));
		_hero.load(_pak);
		_hero.reset(feet, yaw);
		++_magic.noForm;
		return false;
	}
	_hero.reset(feet, yaw);
	_spellItem = NONE;
	_lastWeapon = WEAPON_FIST;
	_invStamp = 0;
	++_magic.transforms;
	toast(tr("Превращение. Обратно — кнопка магии"), 3.0f);
	return true;
}

void App::heroTransformBack() {
	const int hi = heroIndex();
	if (hi < 0 || !heroTransformed()) return;
	if (_cast.stage != CAST_NONE) castCancel();
	const Vec3 feet = _player.feet;
	const float yaw = _hero.yaw();
	const bool down = _hero.down();
	_npcs.game().transformBack(u32(hi));
	_hero.load(_pak);
	_hero.reset(feet, yaw);
	if (down) _hero.fall(_deadTime > 0.0f, false);
	_invStamp = 0;
}

void App::magicUpdate(float dt) {
	const int hi = heroIndex();
	if (hi >= 0 && _cast.stage != CAST_NONE) {
		Game& g = _npcs.game();
		GameNpc& h = g.npcMut(u32(hi));
		const GameSpell* sp = g.spellInfo(_cast.spell);
		// Struck, put away, or down: the spell is lost.
		if (_hero.down() || _hero.weapon() != WEAPON_MAG || !sp || (_cast.stage == CAST_INVEST && !_hero.casting() && _hero.role() == Hero::HIT)) {
			castCancel();
		} else if (_cast.stage == CAST_INVEST) {
			if (_cast.fx) _particles.move(_cast.fx, handPos());
			_cast.tick += dt * 1000.0f;
			bool done = false;
			bool stop = false;
			bool cast = false;
			while (!done && _cast.tick >= sp->timePerMana) {
				_cast.tick -= sp->timePerMana;
				const i32 code = g.processMana(u32(hi), _cast.target, _cast.invested, false);
				// Gothic 1: every mana put in costs a point.
				g.setAttribute(h, 2, max(g.attributeOf(h, 2) - 1, i32(0)));
				if (code == g.constant("SPL_SENDCAST", 2)) {
					cast = done = true;
				} else if (code == g.constant("SPL_SENDSTOP", 3)) {
					stop = done = true;
				} else if (code == g.constant("SPL_DONTINVEST", 0)) {
					done = true; // (no more can be put in; the cast waits for the key to be let go)
					_cast.tick = 0;
				} else {
					if (code == g.constant("SPL_NEXTLEVEL", 4) && _cast.level < kMaxLevel) {
						++_cast.level;
						h.spellLevel = _cast.level;
					}
					++_cast.invested;
					if (g.attributeOf(h, 2) <= 0) {
						// Out of mana: what is charged goes off, as letting go would.
						castRelease(true);
						return;
					}
				}
			}
			if (stop) {
				castCancel();
			} else if (cast) {
				castRelease(false);
			} else if (!_attackHeld) {
				castRelease(true);
			}
		} else if (_cast.stage == CAST_SHOOT) {
			_cast.timer += dt;
			if (_cast.timer >= _cast.length * 0.8f) castCommit();
		}
	}
	boltUpdate(dt);
}

// ---- projectiles ----

void App::boltShoot(const GameSpell& sp, i32 spell, i32 level) {
	Bolt* b = nullptr;
	for (Bolt& x : _bolts)
		if (!x.alive) b = &x;
	if (!b) {
		b = &_bolts[0]; // (the oldest goes)
		if (b->fx) _particles.stop(b->fx);
	}
	const Vec3 from = handPos();
	float dir[3] = {sinf(_hero.yaw()), 0.0f, cosf(_hero.yaw())};
	// At the target if there is one in front, else straight ahead.
	if (_cast.target >= 0) {
		const GameNpc& t = _npcs.game().npc(u32(_cast.target));
		if (t.down == DOWN_NONE && t.placed) {
			const float dx = t.pos[0] - from.x, dy = t.pos[1] + 100.0f - from.y, dz = t.pos[2] - from.z;
			const float len = sqrtf(dx * dx + dy * dy + dz * dz);
			if (len > 1.0f && fabsf(wrapYaw(atan2f(dx, dz) - _hero.yaw())) < 1.4f) {
				dir[0] = dx / len;
				dir[1] = dy / len;
				dir[2] = dz / len;
			}
		}
	}
	*b = Bolt();
	b->alive = true;
	b->pos[0] = from.x;
	b->pos[1] = from.y;
	b->pos[2] = from.z;
	for (int i = 0; i < 3; ++i) {
		b->dir[i] = dir[i];
		b->vel[i] = dir[i] * kBoltSpeed;
	}
	b->range = sp.collectRange > 0 ? min(float(sp.collectRange), kMaxRange) : kMaxRange;
	if (b->range < 1500.0f) b->range = kMaxRange;
	b->spell = i16(spell);
	b->level = u8(level);
	b->fxCollide = sp.fxCollide;
	const u32 fly = sp.fxFly ? sp.fxFly : sp.fxCast;
	if (fly) b->fx = _particles.start(fly, from, _hero.yaw(), b->range / kBoltSpeed + 0.5f);
	if (sp.fxCast) _particles.play(sp.fxCast, from, _hero.yaw(), 0.6f);
	++_magic.bolts;
}

void App::boltImpact(Bolt& b, int victim, Vec3 at) {
	const int heroShot = heroIndex();
	// Whose it is: the player's, or an inhabitant's (its stats are not the player's).
	const int hi = b.owner >= 0 ? b.owner : heroShot;
	const bool mine = hi == heroShot;
	Game& g = _npcs.game();
	if (b.fx) _particles.stop(b.fx);
	b.fx = 0;
	if (b.arrow) {
		// An arrow: it sticks where it lands (a moment), and hits by a roll.
		b.stuck = 0.001f;
		if (victim < 0 || hi < 0) return;
		b.alive = false;
		const float dist = b.travelled;
		const float refRange = 2000.0f, maxRange = 4500.0f;
		float chance = 0.0f;
		if (dist < refRange) chance = 1.0f + (b.hitChance - 1.0f) * (dist / refRange);
		else if (dist < maxRange) chance = b.hitChance * (1.0f - (dist - refRange) / (maxRange - refRange));
		const float roll = float(g.dice(100)) / 100.0f;
		if (dist > maxRange || chance <= roll) {
			if (mine) ++_ranged.misses;
			_ranged.lastDamage = 0;
			return;
		}
		i32 dmg[8];
		for (int i = 0; i < 8; ++i) dmg[i] = b.dmg[i];
		if (long(100.0f * b.critChance + 0.5f) > i32(g.dice(100)))
			for (i32& d : dmg) d *= g.constant("DAM_CRITICAL_MULTIPLIER", 2);
		g.setArrowStrike(dmg);
		const i32 dealt = _npcs.strike(u32(hi), u32(victim), false);
		g.endArrowStrike();
		if (mine) {
			++_ranged.hits;
			_ranged.lastDamage = dealt;
		}
		return;
	}
	b.alive = false;
	if (b.fxCollide) _particles.play(b.fxCollide, at, _hero.yaw(), 1.2f);
	if (victim < 0 || hi < 0) return;
	g.setSpellStrike(b.spell, b.level);
	const i32 dmg = _npcs.strike(u32(hi), u32(victim), false);
	g.setSpellStrike(-1, 0);
	GameNpc& v = g.npcMut(u32(victim));
	v.lastHitSpell = b.spell;
	if (mine) {
		++_magic.hits;
		_magic.lastDamage = dmg;
	}
	if (v.down == DOWN_NONE && victim != heroShot) _npcs.perceiveSingle(u32(victim), "PERC_ASSESSMAGIC", hi);
}

void App::boltUpdate(float dt) {
	const int hi = heroIndex();
	for (Bolt& b : _bolts) {
		if (!b.alive) continue;
		if (b.arrow && b.stuck > 0.0f) {
			// An arrow at rest: it stays a moment.
			b.stuck += dt;
			if (b.stuck > 4.0f) b.alive = false;
			continue;
		}
		if (b.arrow) b.vel[1] -= 980.0f * dt; // (the original's 9.8 m/s^2)
		const float sx = b.vel[0] * dt, sy = b.vel[1] * dt, sz = b.vel[2] * dt;
		const float step = sqrtf(sx * sx + sy * sy + sz * sz);
		if (step < 0.001f) continue;
		for (int i = 0; i < 3; ++i) b.dir[i] = b.vel[i] * dt / step;
		const Vec3 from = {b.pos[0], b.pos[1], b.pos[2]};
		const Vec3 to = {from.x + sx, from.y + sy, from.z + sz};
		// Whom it meets: the character nearest to the line of this step.
		int hit = -1;
		float best = kBoltRadius * kBoltRadius;
		if (_npcs.loaded() && hi >= 0) {
			const Game& g = _npcs.game();
			const int owner = b.owner >= 0 ? b.owner : hi;
			for (u32 i = 0; i < g.npcCount(); ++i) {
				if (int(i) == owner) continue;
				const GameNpc& n = g.npc(i);
				// (The player's character is a target of the others' shots.)
				if (int(i) == hi ? _hero.down() : (!n.placed || n.down != DOWN_NONE || !n.object)) continue;
				const float cx = n.pos[0] - from.x, cy = n.pos[1] + 90.0f - from.y, cz = n.pos[2] - from.z;
				float t = cx * b.dir[0] + cy * b.dir[1] + cz * b.dir[2];
				t = clamp(t, 0.0f, step);
				const float px = cx - b.dir[0] * t, py = cy - b.dir[1] * t, pz = cz - b.dir[2] * t;
				const float d2 = px * px + py * py + pz * pz;
				if (d2 < best) {
					best = d2;
					hit = int(i);
				}
			}
		}
		if (hit >= 0) {
			const GameNpc& n = _npcs.game().npc(u32(hit));
			boltImpact(b, hit, {n.pos[0], n.pos[1] + 90.0f, n.pos[2]});
			continue;
		}
		if (!_world.lineFree(from, to, 6.0f)) {
			boltImpact(b, -1, from);
			continue;
		}
		b.pos[0] = to.x;
		b.pos[1] = to.y;
		b.pos[2] = to.z;
		b.travelled += step;
		if (b.fx) _particles.move(b.fx, to);
		if (b.travelled >= b.range) {
			if (b.fx) _particles.stop(b.fx);
			b.fx = 0;
			b.alive = false;
		}
	}
}

// ---- the inhabitants' shots ----

App::Bolt* App::freeBolt() {
	Bolt* b = nullptr;
	for (Bolt& x : _bolts)
		if (!x.alive) b = &x;
	if (!b) {
		b = &_bolts[0]; // (the oldest goes)
		if (b->fx) _particles.stop(b->fx);
	}
	return b;
}

bool App::npcShotThunk(void* user, u32 shooter, int target, i32 spell, i32 level) {
	return static_cast<App*>(user)->npcShot(shooter, target, spell, level);
}

// From the shooter's hand at its target's chest, as the player's own arrows and spells fly (OpenGothic: an
// inhabitant shoots through the same Bullet as the player).
bool App::npcShot(u32 shooter, int target, i32 spell, i32 level) {
	Game& g = _npcs.game();
	if (shooter >= g.npcCount()) return false;
	const GameNpc& s = g.npc(shooter);
	const Vec3 from = {s.pos[0] + sinf(s.yaw) * 45.0f, s.pos[1] + 130.0f, s.pos[2] + cosf(s.yaw) * 45.0f};
	float dir[3] = {sinf(s.yaw), 0.0f, cosf(s.yaw)};
	float flat = 0.0f;
	if (target >= 0 && u32(target) < g.npcCount()) {
		const GameNpc& t = g.npc(u32(target));
		const float dx = t.pos[0] - from.x, dy = t.pos[1] + 100.0f - from.y, dz = t.pos[2] - from.z;
		const float len = sqrtf(dx * dx + dy * dy + dz * dz);
		if (len > 1.0f) {
			dir[0] = dx / len;
			dir[1] = dy / len;
			dir[2] = dz / len;
			flat = sqrtf(dx * dx + dz * dz);
		}
	}
	const float yaw = atan2f(dir[0], dir[2]);
	if (spell >= 0) {
		const GameSpell* sp = g.spellInfo(spell);
		if (!sp) return false;
		Bolt* b = freeBolt();
		*b = Bolt();
		b->alive = true;
		b->owner = int(shooter);
		for (int i = 0; i < 3; ++i) {
			b->pos[i] = i == 0 ? from.x : i == 1 ? from.y : from.z;
			b->dir[i] = dir[i];
			b->vel[i] = dir[i] * kBoltSpeed;
		}
		b->range = sp->collectRange > 0 ? min(float(sp->collectRange), kMaxRange) : kMaxRange;
		if (b->range < 1500.0f) b->range = kMaxRange;
		b->spell = i16(spell);
		b->level = u8(level < 1 ? 1 : level > kMaxLevel ? kMaxLevel : level);
		b->fxCollide = sp->fxCollide;
		const u32 fly = sp->fxFly ? sp->fxFly : sp->fxCast;
		if (fly) b->fx = _particles.start(fly, from, yaw, b->range / kBoltSpeed + 0.5f);
		if (sp->fxCast) _particles.play(sp->fxCast, from, yaw, 0.6f);
		return true;
	}
	const GameItemInfo* bow = s.ranged != 0xFFFFFFFFu ? g.itemInfo(s.ranged) : nullptr;
	if (!bow || bow->munition < 0) return false;
	const u32 ammo = u32(bow->munition);
	const GameItemInfo* am = g.itemInfo(ammo);
	Bolt* b = freeBolt();
	*b = Bolt();
	b->alive = true;
	b->arrow = true;
	b->owner = int(shooter);
	b->ammo = ammo;
	const float speed = 3000.0f;
	for (int i = 0; i < 3; ++i) {
		b->pos[i] = i == 0 ? from.x : i == 1 ? from.y : from.z;
		b->dir[i] = dir[i];
		b->vel[i] = dir[i] * speed;
	}
	if (flat > 1.0f) b->vel[1] += 0.5f * 980.0f * (flat / speed);
	b->range = 4600.0f;
	b->fx = _particles.start(Particles::arrowTrail(), from, yaw, 8.0f);
	const i32 type = bow->damageType ? bow->damageType : g.constant("DAM_POINT", 64);
	for (int i = 0; i < 8; ++i) {
		b->dmg[i] = bow->damage[i] + (am ? am->damage[i] : 0);
		if (type & (1 << i)) b->dmg[i] += bow->damageTotal + (am ? am->damageTotal : 0);
	}
	const bool cross = (bow->flags & g.constant("ITEM_CROSSBOW", 1 << 20)) != 0;
	b->hitChance = float(g.attributeOf(s, g.constant("ATR_DEXTERITY", 5))) / 100.0f;
	b->critChance = float(s.talent[cross ? 4 : 3]) / 100.0f;
	return true;
}

} // namespace g3
