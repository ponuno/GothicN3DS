// Ranged combat: a bow or crossbow in the hand, A held aims, let go shoots, then it is reloaded. The arrow
// flies as a projectile (app_magic.cpp: boltUpdate) with gravity; its hit is a roll, as OpenGothic's.
#include "app.h"
#include "lang.h"
#include <math.h>

namespace g3 {

namespace {
constexpr float kArrowSpeed = 3000.0f; // cm/s: the original's 3 cm per ms
constexpr float kGravity = 980.0f;     // cm/s^2
float wrapYawR(float a) {
	if (!(fabsf(a) < 1.0e4f)) a = 0.0f; // (fix134: an angle gone to infinity would loop here for ever)
	while (a > 3.14159265f) a -= 6.2831853f;
	while (a < -3.14159265f) a += 6.2831853f;
	return a;
}
} // namespace

u8 App::rangedMode() {
	const int hi = heroIndex();
	if (hi < 0) return WEAPON_NONE;
	Game& g = _npcs.game();
	const GameNpc& h = g.npc(u32(hi));
	if (h.ranged == 0xFFFFFFFFu) return WEAPON_NONE;
	const GameItemInfo* it = g.itemInfo(h.ranged);
	if (!it) return WEAPON_NONE;
	return (it->flags & g.constant("ITEM_CROSSBOW", 1 << 20)) ? WEAPON_CBOW : WEAPON_BOW;
}

void App::rangedInput(const Input& in) {
	const int hi = heroIndex();
	if (hi < 0) return;
	Game& g = _npcs.game();
	if (in.rangedSlot && _shot.stage == SHOT_NONE) {
		if (_hero.weapon() == WEAPON_BOW || _hero.weapon() == WEAPON_CBOW) {
			heroTake(WEAPON_NONE);
		} else {
			const u8 mode = rangedMode();
			if (mode == WEAPON_NONE) toast(tr("Нет лука"));
			else if (!heroTake(mode)) toast(tr("Сейчас нельзя"));
		}
	}
	if (_hero.weapon() != WEAPON_BOW && _hero.weapon() != WEAPON_CBOW) {
		if (_shot.stage != SHOT_NONE) {
			_shot = Shot();
			_hero.castEnd();
		}
		return;
	}
	if (in.attack && _shot.stage == SHOT_NONE && !_hero.drawing() && !_hero.down() && !_player.swimming && !_player.flying) {
		const GameNpc& h = g.npc(u32(hi));
		const GameItemInfo* bow = h.ranged != 0xFFFFFFFFu ? g.itemInfo(h.ranged) : nullptr;
		if (!bow || bow->munition < 0 || g.itemCount(u32(hi), u32(bow->munition)) <= 0) {
			++_ranged.noAmmo;
			toast(tr("Нет стрел"));
			return;
		}
		_shot = Shot();
		_shot.stage = SHOT_AIM;
		_shot.length = _hero.rangedPlay(0);
	}
}

void App::rangedUpdate(float dt) {
	if (_shot.stage != SHOT_NONE && (_hero.down() || (_hero.weapon() != WEAPON_BOW && _hero.weapon() != WEAPON_CBOW))) {
		_shot = Shot();
		_hero.castEnd();
		return;
	}
	if (_shot.stage == SHOT_NONE) return;
	_shot.time += dt;
	switch (_shot.stage) {
	case SHOT_AIM: {
		// (A press shoots, a held key shoots on and on; the first aim takes longer than the ones after a reload.)
		const float aim = min(_shot.length > 0.0f ? _shot.length : 0.3f, _shot.again ? 0.18f : 0.4f);
		if (_shot.time >= aim) {
			_shot.stage = SHOT_SHOOT;
			_shot.time = 0;
			_shot.fired = false;
			_shot.length = _hero.rangedPlay(1);
			if (_shot.length <= 0.0f) _shot.length = 0.3f;
		}
		break;
	}
	case SHOT_SHOOT:
		if (!_shot.fired && _shot.time >= _shot.length * 0.4f) {
			_shot.fired = true;
			arrowShoot();
		}
		if (_shot.time >= _shot.length) {
			_shot.stage = SHOT_RELOAD;
			_shot.time = 0;
			_shot.length = _hero.rangedPlay(2);
			if (_shot.length <= 0.0f) _shot.length = 0.4f;
		}
		break;
	case SHOT_RELOAD:
		if (_shot.time >= _shot.length) {
			const int hi = heroIndex();
			Game& g = _npcs.game();
			const GameNpc* h = hi >= 0 ? &g.npc(u32(hi)) : nullptr;
			const GameItemInfo* bow = h && h->ranged != 0xFFFFFFFFu ? g.itemInfo(h->ranged) : nullptr;
			const bool ammo = bow && bow->munition >= 0 && g.itemCount(u32(hi), u32(bow->munition)) > 0;
			if (_attackHeld && ammo) {
				// Still held: the next one.
				_shot.stage = SHOT_AIM;
				_shot.time = 0;
				_shot.again = true;
				_shot.length = _hero.rangedPlay(0);
			} else {
				if (_attackHeld && !ammo) {
					++_ranged.noAmmo;
					toast(tr("Нет стрел"));
				}
				_shot = Shot();
				_hero.castEnd();
			}
		}
		break;
	}
}

void App::arrowShoot() {
	const int hi = heroIndex();
	if (hi < 0) return;
	Game& g = _npcs.game();
	GameNpc& h = g.npcMut(u32(hi));
	const GameItemInfo* bow = h.ranged != 0xFFFFFFFFu ? g.itemInfo(h.ranged) : nullptr;
	if (!bow || bow->munition < 0 || g.itemCount(u32(hi), u32(bow->munition)) <= 0) {
		++_ranged.noAmmo;
		return;
	}
	const u32 ammo = u32(bow->munition);
	const GameItemInfo* am = g.itemInfo(ammo);
	Bolt* b = nullptr;
	for (Bolt& x : _bolts)
		if (!x.alive) b = &x;
	if (!b) {
		b = &_bolts[0];
		if (b->fx) _particles.stop(b->fx);
	}
	g.giveItems(u32(hi), ammo, -1);
	const Vec3 from = handPos();
	float dir[3] = {sinf(_hero.yaw()), 0.0f, cosf(_hero.yaw())};
	float flat = 0.0f; // the horizontal distance to the target
	if (_target >= 0) {
		const GameNpc& t = g.npc(u32(_target));
		if (t.down == DOWN_NONE && t.placed) {
			const float dx = t.pos[0] - from.x, dy = t.pos[1] + 100.0f - from.y, dz = t.pos[2] - from.z;
			const float len = sqrtf(dx * dx + dy * dy + dz * dz);
			if (len > 1.0f && fabsf(wrapYawR(atan2f(dx, dz) - _hero.yaw())) < 1.4f) {
				dir[0] = dx / len;
				dir[1] = dy / len;
				dir[2] = dz / len;
				flat = sqrtf(dx * dx + dz * dz);
			}
		}
	}
	*b = Bolt();
	b->alive = true;
	b->arrow = true;
	b->ammo = ammo;
	b->pos[0] = from.x;
	b->pos[1] = from.y;
	b->pos[2] = from.z;
	for (int i = 0; i < 3; ++i) {
		b->dir[i] = dir[i];
		b->vel[i] = dir[i] * kArrowSpeed;
	}
	// The arrow falls on its way: aim above by half of what gravity takes (OpenGothic's launch).
	if (flat > 1.0f) b->vel[1] += 0.5f * kGravity * (flat / kArrowSpeed);
	b->range = 4600.0f;
	b->fx = _particles.start(Particles::arrowTrail(), from, _hero.yaw(), 8.0f);
	// Its damage: the bow's and the arrow's, per type.
	i32 type = bow->damageType ? bow->damageType : g.constant("DAM_POINT", 64);
	for (int i = 0; i < 8; ++i) {
		b->dmg[i] = bow->damage[i] + (am ? am->damage[i] : 0);
		if (type & (1 << i)) b->dmg[i] += bow->damageTotal + (am ? am->damageTotal : 0);
	}
	const bool cross = _hero.weapon() == WEAPON_CBOW;
	b->hitChance = float(g.attributeOf(h, g.constant("ATR_DEXTERITY", 5))) / 100.0f;
	b->critChance = float(h.talent[cross ? 4 : 3]) / 100.0f;
	++_ranged.shot;
}

void App::drawArrows(const float light[3]) {
	for (const Bolt& b : _bolts) {
		if (!b.alive || !b.arrow || b.ammo == 0xFFFFFFFFu) continue;
		const float vx = b.vel[0], vy = b.vel[1], vz = b.vel[2];
		const float len = sqrtf(vx * vx + vy * vy + vz * vz);
		if (len < 1.0f) continue;
		// A local +X along the flight (the arrow's model axis is assumed to be X).
		const float xs[3] = {vx / len, vy / len, vz / len};
		float up[3] = {0, 1, 0};
		if (fabsf(xs[1]) > 0.98f) { up[1] = 0; up[2] = 1; }
		float zs[3] = {xs[1] * up[2] - xs[2] * up[1], xs[2] * up[0] - xs[0] * up[2], xs[0] * up[1] - xs[1] * up[0]};
		const float zl = sqrtf(zs[0] * zs[0] + zs[1] * zs[1] + zs[2] * zs[2]);
		if (zl < 1e-4f) continue;
		for (float& z : zs) z /= zl;
		const float ys[3] = {zs[1] * xs[2] - zs[2] * xs[1], zs[2] * xs[0] - zs[0] * xs[2], zs[0] * xs[1] - zs[1] * xs[0]};
		Mat34 at;
		at.m[0][0] = xs[0]; at.m[1][0] = xs[1]; at.m[2][0] = xs[2];
		at.m[0][1] = ys[0]; at.m[1][1] = ys[1]; at.m[2][1] = ys[2];
		at.m[0][2] = zs[0]; at.m[1][2] = zs[1]; at.m[2][2] = zs[2];
		at.m[0][3] = 0; at.m[1][3] = 0; at.m[2][3] = 0;
		_loot.drawHeld(_pak, _npcs.game(), b.ammo, at, {b.pos[0], b.pos[1], b.pos[2]}, light);
	}
}

} // namespace g3
