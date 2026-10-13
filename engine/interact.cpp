// The world's things the player uses: benches, beds, the grindstone, fires, levers, barrels ...
// The world's maker names what is for the player (the focus name of the thing); a thing without
// a name is left to the inhabitants and shows no prompt, whatever the scripts do with it.
// The characters use the same things through their scripts' UseMob (npc_ai.cpp); a thing in use
// is held by its user (Mobs::hold), so nobody takes the player's bench and he takes nobody's.
#include "lang.h"
#include "app.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace g3 {

namespace {
float angleBetween(float from, float to, float t) {
	float d = to - from;
	while (d > 3.14159265f) d -= 6.2831853f;
	while (d < -3.14159265f) d += 6.2831853f;
	return from + d * t;
}
} // namespace

// The name shown: a script constant's text (MOBNAME_BENCH = "Скамья"), else the words themselves.
void App::mobName(u32 mob, char* out, size_t size) const {
	out[0] = 0;
	const Mobs& mobs = _npcs.mobs();
	if (mob >= mobs.count() || !mobs.hasInfo()) return;
	const char* key = mobs.infoText(mobs.info(mob).focus);
	const Game& g = _npcs.game();
	const ScriptVm& vm = g.vm();
	const u32 sym = vm.find(key);
	const char* text = key;
	if (sym != SCRIPT_NONE && vm.symbol(sym).type == SCRIPT_STRING) text = vm.getString(sym);
	scriptText(text ? text : "", out, size);
	if (!out[0]) snprintf(out, size, "%s", key);
}

// The thing in reach in front of the character: the nearest of those meant for him that nobody uses.
int App::mobNearest() const {
	const Mobs& mobs = _npcs.mobs();
	if (!mobs.hasInfo()) return -1;
	int best = -1;
	float bestD = 170.0f * 170.0f;
	for (u32 i = 0; i < mobs.count(); ++i) {
		if (!mobs.forPlayer(i) || mobs.userOf(i) >= 0) continue;
		const WorldMob& m = mobs.mob(i);
		const float dx = m.pos[0] - _player.feet.x, dz = m.pos[2] - _player.feet.z;
		const float d2 = dx * dx + dz * dz;
		if (d2 >= bestD || fabsf(m.pos[1] - _player.feet.y) > 140.0f) continue;
		if (d2 > 40.0f * 40.0f) {
			float a = atan2f(dx, dz) - _hero.yaw();
			while (a > 3.14159265f) a -= 6.2831853f;
			while (a < -3.14159265f) a += 6.2831853f;
			if (fabsf(a) > 1.2f) continue;
		}
		bestD = d2;
		best = int(i);
	}
	return best;
}

// A: the condition (the scripts say whether it may be used), the item it wants, then the way to it.
bool App::mobStart(u32 mob) {
	const int hi = heroIndex();
	Mobs& mobs = _npcs.mobs();
	if (hi < 0 || mob >= mobs.count() || !mobs.hasInfo() || !mobs.forPlayer(mob)) return false;
	Game& g = _npcs.game();
	const WorldMobInfo& mi = mobs.info(mob);
	if (mi.useItem) {
		const u32 item = g.vm().find(mobs.infoText(mi.useItem));
		if (item != SCRIPT_NONE && g.itemCount(u32(hi), item) <= 0) {
			char name[48], buf[96];
			const GameItemInfo* ii = g.itemInfo(item);
			scriptText(ii ? g.text(ii->description ? ii->description : ii->name) : "", name, sizeof(name));
			snprintf(buf, sizeof(buf), tr("Нужен предмет: %s"), name);
			itemMessage(buf);
			return false;
		}
	}
	if (mi.condition) {
		const u32 fn = g.function(mobs.infoText(mi.condition));
		if (fn != SCRIPT_NONE && g.callAs(fn, u32(hi), -1) == 0) {
			itemMessage(tr("Сейчас нельзя"));
			return false;
		}
	}
	if (!mobs.hold(mob, u32(hi))) return false;
	_mobJob = MobJob();
	_mobJob.mob = mob;
	_mobJob.stage = MJ_GLIDE;
	// A thing with nothing to run is a place to be at (a bed, a bench): the character stays until he lets go.
	_mobJob.stay = mi.onState == 0;
	_mobJob.from = _player.feet;
	_mobJob.to = _player.feet;
	_mobJob.yawFrom = _hero.yaw();
	_mobJob.yawTo = _hero.yaw();
	const float from[3] = {_player.feet.x, _player.feet.y, _player.feet.z};
	float pos[3], yaw;
	const WorldMob& m = mobs.mob(mob);
	const int slot = mobs.place(mob, from, pos, yaw);
	if (slot >= 0) {
		_mobJob.slotKind = mobs.slot(mob, u32(slot)).kind;
		const Vec3 to = {pos[0], _player.feet.y, pos[2]};
		const float dx = to.x - _player.feet.x, dz = to.z - _player.feet.z;
		if (dx * dx + dz * dz < 160.0f * 160.0f &&
		    _world.lineFree({_player.feet.x, _player.feet.y + 90.0f, _player.feet.z}, {to.x, to.y + 90.0f, to.z}, 12.0f))
			_mobJob.to = to;
		_mobJob.yawTo = yaw;
	} else {
		_mobJob.yawTo = atan2f(m.pos[0] - _player.feet.x, m.pos[2] - _player.feet.z);
	}
	return true;
}

void App::mobEnd(bool hurried) {
	if (_mobJob.stage == MJ_NONE) return;
	_npcs.mobs().drop(u32(heroIndex() < 0 ? 0 : heroIndex()));
	if (hurried || _mobJob.stage == MJ_GLIDE) {
		if (_mobJob.stage != MJ_GLIDE) _hero.cancelUse();
		_mobJob = MobJob();
		return;
	}
	_hero.closeMob();
	_mobJob.stage = MJ_OUT;
}

void App::mobUpdate(Input& in, float dt) {
	const int hi = heroIndex();
	_mobTarget = -1;
	if (_mobJob.stage == MJ_NONE) {
		if (hi < 0 || !_hero.loaded() || _hero.down() || _hero.weapon() != WEAPON_NONE || _hero.drawing() || _job.stage != JOB_NONE ||
		    _chest.stage != CH_NONE || _panel.open() || _player.swimming || _player.climbing || _hero.mantling() ||
		    !_player.grounded || _chestTarget >= 0 || _corpseTarget >= 0 || _lootTarget >= 0 || _menu.isOpen())
			return;
		_mobTarget = mobNearest();
		if (_mobTarget >= 0 && (in.confirm || in.attack)) {
			if (mobStart(u32(_mobTarget))) in.confirm = in.attack = false;
		}
		return;
	}
	// The hands are the thing's: no blows meanwhile.
	in.attack = in.parry = in.drawWeapon = in.meleeSlot = in.fistSlot = false;
	if (hi < 0 || _hero.down() || !_hero.loaded()) {
		mobEnd(true);
		return;
	}
	Mobs& mobs = _npcs.mobs();
	const WorldMobInfo& mi = mobs.info(_mobJob.mob);
	_mobJob.time += dt;
	switch (_mobJob.stage) {
	case MJ_GLIDE: {
		const float t = clamp(_mobJob.time / 0.35f, 0.0f, 1.0f);
		const float e = t * t * (3.0f - 2.0f * t);
		_player.feet.x = _mobJob.from.x + (_mobJob.to.x - _mobJob.from.x) * e;
		_player.feet.z = _mobJob.from.z + (_mobJob.to.z - _mobJob.from.z) * e;
		_hero.setYaw(angleBetween(_mobJob.yawFrom, _mobJob.yawTo, e));
		if (t >= 1.0f) {
			if (_hero.openMob(mobs.scheme(_mobJob.mob), mobs.mob(_mobJob.mob).states, _mobJob.slotKind)) {
				_mobJob.stage = MJ_IN;
				_mobJob.time = 0;
			} else {
				itemMessage(tr("Сейчас нельзя"));
				mobs.drop(u32(hi));
				_mobJob = MobJob();
			}
		}
		break;
	}
	case MJ_IN:
	case MJ_HELD:
		if (!_hero.atMob()) {
			// A blow, a fall: it is left.
			mobEnd(true);
			break;
		}
		if (_hero.mobOpen() && !_mobJob.ran) {
			_mobJob.ran = true;
			_mobJob.stage = MJ_HELD;
			_mobJob.time = 0;
			Game& g = _npcs.game();
			if (mi.onState) {
				// The function is the object's name for it with the state's number (OpenGothic's
				// Interactive::invokeStateFunc: "<name>_S1" on the first use); one that is named whole still runs.
				const char* name = mobs.infoText(mi.onState);
				char state[96];
				snprintf(state, sizeof(state), "%s_S1", name);
				u32 fn = g.function(state);
				if (fn == SCRIPT_NONE) fn = g.function(name);
				if (fn != SCRIPT_NONE) g.callAs(fn, u32(hi), -1);
				_invStamp = 0;
			}
		}
		if (_mobJob.stage == MJ_HELD) {
			// A thing that does its work lets go of him at once; a place to be at, when he asks.
			// (As in the original, where the character stays in the last state until he is let go - a bench, a bed,
			// the anvil's hammering, whatever has a pose to stay in; a thing with none - a lever - lets go of him.)
			const bool leave = in.confirm || fabsf(in.moveX) > 0.6f || fabsf(in.moveZ) > 0.6f || in.jump;
			const bool stay = _mobJob.stay || _hero.mobHolds();
			if (!stay && _mobJob.time > 0.4f) mobEnd(false);
			else if (stay && _mobJob.time > 0.5f && leave) {
				in.confirm = false;
				mobEnd(false);
			}
		}
		break;
	case MJ_OUT:
		if (!_hero.atMob()) _mobJob = MobJob();
		break;
	default: break;
	}
}

} // namespace g3
