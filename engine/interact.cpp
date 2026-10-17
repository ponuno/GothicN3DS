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
	if (!(fabsf(d) < 1.0e4f)) d = 0.0f; // (fix134: an angle gone to infinity would loop here for ever)
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
	if (!*key && mobs.isBed(mob)) {
		// A bed nobody named: the scripts' word for a bed, else ours.
		key = "MOBNAME_BED";
		if (_npcs.game().vm().find(key) == SCRIPT_NONE) {
			snprintf(out, size, "%s", tr("Кровать"));
			return;
		}
	}
	const Game& g = _npcs.game();
	const ScriptVm& vm = g.vm();
	const u32 sym = vm.find(key);
	const char* text = key;
	if (sym != SCRIPT_NONE && vm.symbol(sym).type == SCRIPT_STRING) text = vm.getString(sym);
	scriptText(text ? text : "", out, size);
	if (!out[0]) snprintf(out, size, "%s", key);
}

// The thing in reach in front of the character: the nearest of those meant for him that nobody uses.
// Near is near the thing or near a place to stand at it (a bed's origin may be at its head, a metre and
// more from where one lies down at its side).
int App::mobNearest() const {
	const Mobs& mobs = _npcs.mobs();
	if (!mobs.hasInfo()) return -1;
	int best = -1;
	float bestD = 170.0f * 170.0f;
	const auto inFront = [&](float dx, float dz) {
		if (dx * dx + dz * dz <= 40.0f * 40.0f) return true;
		float a = atan2f(dx, dz) - _hero.yaw();
		if (!(fabsf(a) < 1.0e4f)) a = 0.0f; // (fix134: an angle gone to infinity would loop here for ever)
		while (a > 3.14159265f) a -= 6.2831853f;
		while (a < -3.14159265f) a += 6.2831853f;
		return fabsf(a) <= 1.2f;
	};
	for (u32 i = 0; i < mobs.count(); ++i) {
		if (!mobs.forPlayer(i) || mobs.userOf(i) >= 0) continue;
		const WorldMob& m = mobs.mob(i);
		if (fabsf(m.pos[1] - _player.feet.y) > 140.0f) continue;
		const float dx = m.pos[0] - _player.feet.x, dz = m.pos[2] - _player.feet.z;
		float d2 = 1e30f;
		if (inFront(dx, dz)) d2 = dx * dx + dz * dz;
		for (u32 k = 0; k < m.slotCount; ++k) {
			const MobSlot& sl = mobs.slot(i, k);
			if (fabsf(sl.pos[1] - _player.feet.y) > 140.0f) continue;
			const float sx = sl.pos[0] - _player.feet.x, sz = sl.pos[2] - _player.feet.z;
			const float s2 = sx * sx + sz * sz;
			// (At the place: whichever way he faces, as long as the thing is not behind him.)
			if (s2 < d2 && (s2 <= 60.0f * 60.0f ? inFront(dx, dz) || dx * dx + dz * dz < 120.0f * 120.0f : inFront(sx, sz))) d2 = s2;
		}
		if (d2 >= bestD) continue;
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
	// A bed always is: he lies in it while its sleep talk is up (SLEEPABIT_S1 -> AI_ProcessInfos(hero)).
	_mobJob.bed = mobs.isBed(mob);
	_mobJob.stay = mi.onState == 0 || _mobJob.bed;
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
		_mobJob.floorY = _player.feet.y;
		if (dx * dx + dz * dz < 160.0f * 160.0f &&
		    _world.lineFree({_player.feet.x, _player.feet.y + 90.0f, _player.feet.z}, {to.x, to.y + 90.0f, to.z}, 12.0f)) {
			_mobJob.to = to;
			// The floor there, found as the inhabitants find it (Npcs: the ray down from a little above); he stands on it
			// and not on whatever the bench's own shape offers his feet: that sat him up to a step higher.
			float y;
			Vec3 normal;
			if (_world.rayDown({to.x, _player.feet.y + 60.0f, to.z}, 160.0f, y, normal)) _mobJob.floorY = min(y, _player.feet.y + 15.0f);
			_mobJob.to.y = _mobJob.floorY;
		}
		_mobJob.yawTo = yaw;
	} else {
		_mobJob.floorY = _player.feet.y;
		_mobJob.yawTo = atan2f(m.pos[0] - _player.feet.x, m.pos[2] - _player.feet.z);
	}
	return true;
}

// fix154: what the animations of a thing do to the item in the hand, as OpenGothic's Npc::tickTimedEvt: the forge's
// glowing blade, the lab's flask, the pan's meat. The item stays in his things all the while (the hand holds a place
// for it), so the thing's work is a change of what he carries.
void App::itemEvent(const ClipSfxEvent& event) {
	const int hi = heroIndex();
	if (hi < 0 || _mobJob.stage == MJ_NONE) return;
	Game& g = _npcs.game();
	const auto make = [&](const char* name) {
		const u32 item = g.vm().find(name);
		if (item == SCRIPT_NONE || !g.isItem(item)) return SCRIPT_NONE;
		g.giveItems(u32(hi), item, 1);
		return item;
	};
	const auto destroy = [&]() {
		if (_mobJob.hand != SCRIPT_NONE) g.giveItems(u32(hi), _mobJob.hand, -1);
		_mobJob.hand = SCRIPT_NONE;
	};
	switch (event.kind) {
	case CLIP_ITEM_INSERT: {
		const WorldMobInfo& mi = _npcs.mobs().info(_mobJob.mob);
		if (mi.useItem) {
			const u32 item = g.vm().find(_npcs.mobs().infoText(mi.useItem));
			if (item != SCRIPT_NONE && g.itemCount(u32(hi), item) > 0) _mobJob.hand = item;
		}
		break;
	}
	case CLIP_ITEM_CREATE:
		_mobJob.hand = make(event.name);
		break;
	case CLIP_ITEM_EXCHANGE:
		destroy();
		_mobJob.hand = make(event.name);
		break;
	case CLIP_ITEM_DESTROY:
	case CLIP_ITEM_PLACE:
		destroy();
		break;
	case CLIP_ITEM_REMOVE:
		_mobJob.hand = SCRIPT_NONE;
		break;
	default: break;
	}
	_invStamp = 0;
}

bool App::mobStateFunc(u32 mob, int state, bool whole) {
	const int hi = heroIndex();
	Mobs& mobs = _npcs.mobs();
	if (hi < 0 || mob >= mobs.count() || !mobs.hasInfo()) return false;
	const WorldMobInfo& mi = mobs.info(mob);
	if (!mi.onState) return false;
	Game& g = _npcs.game();
	const char* name = mobs.infoText(mi.onState);
	char fnName[96];
	snprintf(fnName, sizeof(fnName), "%s_S%d", name, state);
	u32 fn = g.function(fnName);
	if (fn == SCRIPT_NONE && whole) fn = g.function(name);
	if (fn == SCRIPT_NONE) return false;
	const bool talkBefore = _npcs.talking();
	g.callAs(fn, u32(hi), -1);
	++_mobUse.stateCalls;
	if (!talkBefore && _npcs.talking()) ++_mobUse.talks;
	_invStamp = 0;
	return true;
}

void App::mobEnd(bool hurried) {
	if (_mobJob.stage == MJ_NONE) return;
	// (fix153) Leaving it: its state function of S0, as OpenGothic invokes it when the state is back at 0.
	if (_mobJob.ran) mobStateFunc(_mobJob.mob, 0);
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
	_doorTarget = -1;
	_ladderTarget = -1;
	if (_mobJob.stage == MJ_NONE) {
		if (hi < 0 || !_hero.loaded() || _hero.down() || _hero.weapon() != WEAPON_NONE || _hero.drawing() || _job.stage != JOB_NONE ||
		    _chest.stage != CH_NONE || _panel.open() || _player.swimming || _player.climbing || _hero.mantling() ||
		    !_player.grounded || _chestTarget >= 0 || _corpseTarget >= 0 || _lootTarget >= 0 || _menu.isOpen() ||
		    heroTransformed()) // (a creature uses no benches, doors or ladders)
			return;
		_mobTarget = mobNearest();
		if (_mobTarget >= 0 && (in.confirm || in.attack)) {
			if (mobStart(u32(_mobTarget))) in.confirm = in.attack = false;
		}
		// A door in front (and nobody to talk to there): A opens or shuts it.
		_doorTarget = _mobTarget < 0 && _talkTarget < 0 ? _triggers.doorNear(_player.feet, _hero.yaw()) : -1;
		if (_doorTarget >= 0 && (in.confirm || in.attack)) {
			doorUse(u32(_doorTarget));
			in.confirm = in.attack = false;
		}
		// A ladder at hand (nothing else to use there): A takes hold of it - at its foot to climb up, at its
		// top to climb down. (Walking into it at its foot, or off its top, takes hold of it too.)
		if (_mobTarget < 0 && _doorTarget < 0 && _talkTarget < 0) {
			bool top = false;
			_ladderTarget = _player.ladderInReach(_world, _hero.yaw(), top);
			_ladderTop = top;
			if (_ladderTarget >= 0 && (in.confirm || in.attack)) {
				if (_player.grabLadder(_world, _ladderTarget, top, _hero.yaw())) _ladderTarget = -1;
				in.confirm = in.attack = false;
			}
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
		_player.feet.y = _mobJob.from.y + (_mobJob.floorY - _mobJob.from.y) * e;
		_hero.setYaw(angleBetween(_mobJob.yawFrom, _mobJob.yawTo, e));
		if (t >= 1.0f) {
			if (_hero.openMob(mobs.scheme(_mobJob.mob), mobs.mob(_mobJob.mob).states, _mobJob.slotKind)) {
				_mobJob.stage = MJ_IN;
				_mobJob.time = 0;
				_npcs.heroUsesMob(int(_mobJob.mob)); // (PERC_ASSESSUSEMOB: whose bench, whose anvil)
				// What it sets going (a lever's gate, a wheel's drawbridge): OpenGothic's Interactive::emitTriggerEvent.
				if (mi.target) _triggers.send(mobs.infoText(mi.target), false);
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
			if (mi.onState) {
				// The function is the object's name for it with the state's number (OpenGothic's
				// Interactive::invokeStateFunc: "<name>_S1", then "_S2" ... for a thing of more states, as each is
				// reached); one that is named whole still runs. A workbench's, a forge's, an alchemist's table's
				// function puts up the player's own talk (AI_ProcessInfos(hero): what to make), held below.
				const int states = max(int(mobs.mob(_mobJob.mob).states), 1);
				for (int k = 1; k <= states; ++k) mobStateFunc(_mobJob.mob, k, k == 1);
			}
		}
		// (fix153) A talk the thing put up (a bed's sleep, a workbench's what-to-make): he stays at it while it lasts and
		// the keys are the talk's; when it is over he leaves it, as the original lets go of him.
		if (_mobJob.stage == MJ_HELD && !_mobJob.bed) {
			if (_npcs.talking()) {
				_mobJob.talked = true;
				in.confirm = in.jump = false;
				in.moveX = in.moveZ = 0;
				break;
			}
			if (_mobJob.talked) {
				mobEnd(false);
				break;
			}
		}
		if (_mobJob.stage == MJ_HELD && _mobJob.bed) {
			// In bed: its sleep talk (the scripts' own, or - with none come up - the engine's menu of hours).
			// The keys are the talk's while it is up; when it is over he gets up.
			const bool talk = _npcs.talking();
			if (talk) {
				_mobJob.talked = true;
				in.confirm = in.jump = false;
				in.moveX = in.moveZ = 0;
				break;
			}
			if (_mobJob.talked) {
				mobEnd(false);
				break;
			}
			if (!_mobJob.offered && _mobJob.time > 0.6f) {
				_mobJob.offered = true;
				// fix208: a bed the world gives no function (Gothic 1's are so) gets the scripts' own sleep talk.
				Game& g = _npcs.game();
				const u32 fn = mobs.info(_mobJob.mob).onState == 0 ? g.function("SLEEPABIT_S1") : SCRIPT_NONE;
				if (fn != SCRIPT_NONE && heroIndex() >= 0) {
					g.callAs(fn, u32(heroIndex()), -1);
					_mobJob.sleepScript = true;
					break;
				}
				if (_npcs.openSleepMenu()) {
					_mobJob.talked = true;
					in.confirm = in.jump = false;
					break;
				}
			}
			if (_mobJob.sleepScript && _mobJob.time > 2.0f) {
				// (Its talk never came up: the engine's menu of hours.)
				_mobJob.sleepScript = false;
				if (_npcs.openSleepMenu()) {
					_mobJob.talked = true;
					in.confirm = in.jump = false;
					break;
				}
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
