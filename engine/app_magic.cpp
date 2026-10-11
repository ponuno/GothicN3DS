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
// Not done: the spells that need the world to change (teleporting, summoning, transforming) have only
// their effect; the runes' circles are not asked for; the others' spells (inhabitants casting) are not.
#include "app.h"
#include "lang.h"

#include <math.h>
#include <stdio.h>

namespace g3 {

namespace {

constexpr u32 NONE = SCRIPT_NONE;
constexpr float kBoltSpeed = 1000.0f;  // cm/s: the original's 1 cm per ms
constexpr float kBoltRadius = 75.0f;   // cm from a character's middle that a projectile hits
constexpr float kMaxRange = 6000.0f;
constexpr int kMaxLevel = 15;

float wrapYaw(float a) {
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
	if (hi >= 0) _npcs.game().npcMut(u32(hi)).spell = -1;
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
	_cast = Cast();
	_hero.castEnd();
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
	for (int i = 0; i < 3; ++i) b->dir[i] = dir[i];
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
	const int hi = heroIndex();
	Game& g = _npcs.game();
	if (b.fx) _particles.stop(b.fx);
	b.fx = 0;
	b.alive = false;
	if (b.fxCollide) _particles.play(b.fxCollide, at, _hero.yaw(), 1.2f);
	if (victim < 0 || hi < 0) return;
	g.setSpellStrike(b.spell, b.level);
	const i32 dmg = _npcs.strike(u32(hi), u32(victim), false);
	g.setSpellStrike(-1, 0);
	GameNpc& v = g.npcMut(u32(victim));
	v.lastHitSpell = b.spell;
	++_magic.hits;
	_magic.lastDamage = dmg;
	if (v.down == DOWN_NONE) _npcs.perceiveSingle(u32(victim), "PERC_ASSESSMAGIC", hi);
}

void App::boltUpdate(float dt) {
	const int hi = heroIndex();
	for (Bolt& b : _bolts) {
		if (!b.alive) continue;
		const float step = kBoltSpeed * dt;
		const Vec3 from = {b.pos[0], b.pos[1], b.pos[2]};
		const Vec3 to = {from.x + b.dir[0] * step, from.y + b.dir[1] * step, from.z + b.dir[2] * step};
		// Whom it meets: the character nearest to the line of this step.
		int hit = -1;
		float best = kBoltRadius * kBoltRadius;
		if (_npcs.loaded() && hi >= 0) {
			const Game& g = _npcs.game();
			for (u32 i = 0; i < g.npcCount(); ++i) {
				if (int(i) == hi) continue;
				const GameNpc& n = g.npc(i);
				if (!n.placed || n.down != DOWN_NONE || !n.object) continue;
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

} // namespace g3
