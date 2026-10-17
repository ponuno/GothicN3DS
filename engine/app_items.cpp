// fix192: the things of the inventory that leave the pack or go into the hands - as the original has them.
//  * Dropping: a thing (or a stack) is put down on the ground in front of the character with the animation of putting
//    a thing down ("T_STAND_2_IDROP" ..., the picking up's where the model has none). It lies there as the things of
//    the world do (Loot::drop): whoever passes may pick it up - the player, or an inhabitant whose scripts look for
//    things (Wld_DetectItem) - and it stays in its world, across the worlds and in the saves. What is dropped and
//    worn is taken off; a weapon in the hands is not let go of (put it away first).
//  * The torch (ITEM_KAT_LIGHT, OpenGothic's Npc::setTorch): lit, it is in the left hand ("ZS_LEFTHAND") with a flame
//    at its end and a light that flickers on the hero, the people about and the world round him (Lamps::setCarried).
//    It stays with the fists and a one-handed weapon; a two-hander, a bow, a spell, a ladder or a thing used in the
//    hands hide it for as long as they have the hands; water puts it out. Put out (its action again) it is burnt: one less.
//  * Things used in the hands by their scheme (C_ITEM.SCEMENAME - a broom, a lute, a hammer ...): the character
//    takes the thing as the model script has it ("T_BROOM_STAND_2_S0", "T_BROOM_S0_2_S1") and goes on with it
//    ("S_BROOM_S1") until he lets go (A, B, the stick, the thing's action again), then puts it away the same way back
//    - the way the inhabitants do it (AI_UseItemToState, npc_items.cpp). ON_STATE[0], if the thing has one, runs when
//    the state is reached, and the thing is used up then (a joint); one without it is kept (a broom). A scheme with
//    no state to stay in (the joint's) is used the way a potion is (startUse).
#include "lang.h"
#include "app.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace g3 {

namespace {

constexpr u32 NONE = SCRIPT_NONE;

void schemeOf(const Game& g, const GameItemInfo& it, char* out, size_t size) {
	const char* s = it.scheme ? g.text(it.scheme) : "";
	size_t n = 0;
	for (; s && s[n] && n + 1 < size; ++n) out[n] = s[n] >= 'a' && s[n] <= 'z' ? char(s[n] - 32) : s[n];
	out[n] = 0;
}

} // namespace

// ---- using by the scheme ----

bool App::canUseScheme(u32 symbol, bool* keeps) {
	if (keeps) *keeps = false;
	if (!_hero.loaded() || !_npcs.loaded() || heroTransformed()) return false;
	Game& g = _npcs.game();
	const GameItemInfo* it = g.itemInfo(symbol);
	if (!it || !it->scheme) return false;
	char scheme[32], name[64];
	schemeOf(g, *it, scheme, sizeof(scheme));
	if (!scheme[0]) return false;
	snprintf(name, sizeof(name), "T_%s_STAND_2_S0", scheme);
	if (!_hero.clip(name)) return false;
	if (keeps) {
		snprintf(name, sizeof(name), "T_%s_S0_2_S1", scheme);
		*keeps = _hero.clip(name) != nullptr;
		if (!*keeps) {
			snprintf(name, sizeof(name), "S_%s_S1", scheme);
			*keeps = _hero.clip(name) != nullptr;
		}
	}
	return true;
}

bool App::startStateItem(u32 symbol) {
	Game& g = _npcs.game();
	const GameItemInfo* it = g.itemInfo(symbol);
	if (!it) return false;
	if (_job.stage != JOB_NONE) {
		if (_job.state && _job.symbol == symbol) {
			// Its action again: let go.
			if (!_job.leaving && _hero.atMob()) {
				_job.leaving = true;
				_hero.closeMob();
			}
			return true;
		}
		itemMessage(tr("Руки заняты"));
		return false;
	}
	if (_player.swimming || _player.climbing || _player.flying || _mobJob.stage != MJ_NONE || _chest.stage != CH_NONE) {
		itemMessage(_player.swimming ? tr("Не в воде") : tr("Сейчас нельзя"));
		return false;
	}
	_job = ItemJob();
	_job.symbol = symbol;
	_job.state = true;
	_job.keep = it->onUse == NONE;
	_job.stage = _hero.weapon() != WEAPON_NONE ? JOB_PUTAWAY : JOB_START;
	_job.redraw = _settings.weaponBack ? _hero.weapon() : u8(WEAPON_NONE);
	_invStamp = 0;
	return true;
}

// A frame of a thing used in the hands: the weapon away, into the states, held until let go, out again.
void App::stateItemFrame(Input& in, float dt) {
	const int hi = heroIndex();
	Game& g = _npcs.game();
	const GameItemInfo* it = g.itemInfo(_job.symbol);
	char name[48];
	scriptText(it ? g.text(it->description ? it->description : it->name) : "", name, sizeof(name));
	// Letting go: A, B, the weapon key, a blow; the stick (not while the inventory is open: the hands are in it).
	const bool letGo = in.confirm || in.jump || in.attack || in.drawWeapon || in.meleeSlot || in.fistSlot || in.magic || in.rangedSlot ||
	                   (!_panel.open() && (fabsf(in.moveX) > 0.6f || fabsf(in.moveZ) > 0.6f));
	// The hands are the thing's.
	in.attack = in.parry = in.drawWeapon = in.meleeSlot = in.fistSlot = in.confirm = in.jump = false;
	in.magic = in.rangedSlot = false;
	_job.wait += dt;
	const auto done = [&]() {
		_job = ItemJob();
		_invStamp = 0;
	};
	const auto finish = [&]() {
		if (_job.redraw != WEAPON_NONE) {
			_job.stage = JOB_REDRAW;
			_job.wait = 0;
			_job.state = false; // (the redraw is the common one)
		} else {
			done();
		}
	};
	switch (_job.stage) {
	case JOB_PUTAWAY:
		if (letGo && !_job.applied) {
			done();
			itemMessage(tr("Отменено"), false);
			break;
		}
		if (_hero.weapon() == WEAPON_NONE && !_hero.drawing()) {
			_job.stage = JOB_START;
			_job.wait = 0;
		} else if (!_hero.drawing()) {
			heroTake(WEAPON_NONE);
		}
		if (_job.wait > 3.0f) {
			itemMessage(tr("Не получилось убрать оружие"));
			done();
		}
		break;
	case JOB_START: {
		char scheme[32] = "";
		if (it) schemeOf(g, *it, scheme, sizeof(scheme));
		if (it && scheme[0] && _hero.openMob(scheme, 3)) {
			_job.stage = JOB_USING;
			_job.wait = 0;
		} else if (_job.wait > 3.0f || !it) {
			itemMessage(tr("Сейчас нельзя"));
			done();
		}
		break;
	}
	case JOB_USING:
		if (!_hero.atMob()) {
			// Out of it: let go and back on his feet, or knocked out of it (a blow, a fall).
			if (!_job.applied) {
				char buf[96];
				snprintf(buf, sizeof(buf), tr("Прервано: %s"), name);
				++_itemStats.interrupted;
				itemMessage(buf, false);
			}
			finish();
			break;
		}
		if (_hero.mobOpen() && !_job.applied) {
			_job.applied = true;
			_job.wait = 0;
			++_itemStats.stateUses;
			if (it && it->onUse != NONE && hi >= 0) {
				if (!_job.keep) g.giveItems(u32(hi), _job.symbol, -1);
				g.callItemFunction(u32(hi), _job.symbol, it->onUse);
			}
			itemMessage(name, false);
			_invStamp = 0;
		}
		if (!_job.leaving && (letGo || (_job.keep && hi >= 0 && g.itemCount(u32(hi), _job.symbol) <= 0))) {
			_job.leaving = true;
			_hero.closeMob();
		}
		// A scheme with no pose to stay in lets go of him by itself.
		if (!_job.leaving && _job.applied && !_hero.mobHolds() && _job.wait > 0.4f) {
			_job.leaving = true;
			_hero.closeMob();
		}
		break;
	default: done(); break;
	}
}

// ---- dropping ----

bool App::dropItem(u32 symbol, i32 amount) {
	const int hi = heroIndex();
	if (hi < 0) return false;
	Game& g = _npcs.game();
	const GameItemInfo* it = g.itemInfo(symbol);
	if (!it) return false;
	char name[48], buf[96];
	scriptText(g.text(it->description ? it->description : it->name), name, sizeof(name));
	const i32 have = g.itemCount(u32(hi), symbol);
	if (have <= 0) {
		snprintf(buf, sizeof(buf), tr("Нет: %s"), name);
		itemMessage(buf);
		return false;
	}
	amount = clamp(amount, i32(1), have);
	if (_hero.down() || heroTransformed() || _mobJob.stage != MJ_NONE || _chest.stage != CH_NONE || talking()) {
		itemMessage(tr("Сейчас нельзя"));
		return false;
	}
	if (_player.swimming || _player.climbing || _player.flying) {
		itemMessage(_player.swimming ? tr("Не в воде") : tr("Сейчас нельзя"));
		return false;
	}
	if (_job.stage != JOB_NONE) {
		itemMessage(tr("Руки заняты"));
		return false;
	}
	// The weapon in the hands is not let go of.
	if (amount >= have) {
		const GameNpc& h = g.npc(u32(hi));
		const u8 w = _hero.weapon();
		const bool held = (h.melee == symbol && (w == WEAPON_1H || w == WEAPON_2H)) || (h.ranged == symbol && (w == WEAPON_BOW || w == WEAPON_CBOW)) ||
		                  (_spellItem == symbol && w == WEAPON_MAG);
		if (held || _hero.drawing()) {
			itemMessage(tr("Сначала уберите оружие"));
			return false;
		}
	}
	_job = ItemJob();
	_job.symbol = symbol;
	_job.drop = true;
	_job.amount = amount;
	_job.stage = _hero.weapon() != WEAPON_NONE ? JOB_PUTAWAY : JOB_START;
	_job.redraw = _settings.weaponBack ? _hero.weapon() : u8(WEAPON_NONE);
	_invStamp = 0;
	if (!_hero.loaded()) {
		// No character to animate (an archive without one): down at once.
		dropFrame();
		_job = ItemJob();
	}
	return true;
}

// Takes off what is dropped and worn, as many as no longer are carried.
void App::dropRelease(u32 symbol, i32 left) {
	const int hi = heroIndex();
	if (hi < 0) return;
	Game& g = _npcs.game();
	GameNpc& h = g.npcMut(u32(hi));
	bool changed = false;
	if (left <= 0) {
		if (h.armorItem == symbol) {
			g.setArmor(u32(hi), NONE);
			changed = true;
		}
		if (_amulet == symbol) {
			equipRing(symbol, false);
			_amulet = NONE;
			changed = true;
		}
		if (h.melee == symbol) {
			h.melee = NONE;
			changed = true;
		}
		if (h.ranged == symbol) {
			h.ranged = NONE;
			changed = true;
		}
		if (_spellItem == symbol && _hero.weapon() != WEAPON_MAG) _spellItem = NONE;
		if (_torch.lit && _torch.symbol == symbol) torchOut(false, nullptr);
	}
	// (Two of the same ring worn, one dropped: the other stays on.)
	i32 rings = (_rings[0] == symbol) + (_rings[1] == symbol);
	for (int r = 1; r >= 0 && rings > max(left, i32(0)); --r)
		if (_rings[r] == symbol) {
			equipRing(symbol, false);
			_rings[r] = NONE;
			--rings;
			changed = true;
		}
	if (changed) {
		++_equipVersion;
		++_itemStats.changed;
	}
}

// The moment the hand lets go: where the thing lands (in front of the feet, on the floor, never through a wall),
// and the thing out of the pack into the world.
bool App::dropFrame() {
	const int hi = heroIndex();
	if (hi < 0) return false;
	Game& g = _npcs.game();
	const u32 symbol = _job.symbol;
	const GameItemInfo* it = g.itemInfo(symbol);
	char name[48], buf[96];
	scriptText(it ? g.text(it->description ? it->description : it->name) : "", name, sizeof(name));
	const i32 have = g.itemCount(u32(hi), symbol);
	const i32 n = min(_job.amount, have);
	if (!it || n <= 0) {
		snprintf(buf, sizeof(buf), tr("Нет: %s"), name);
		itemMessage(buf);
		return false;
	}
	const float yaw = _hero.loaded() ? _hero.yaw() : _cam.yaw;
	const Vec3 feet = _player.feet;
	const Vec3 dir = {sinf(yaw), 0.0f, cosf(yaw)};
	const Vec3 chest = {feet.x, feet.y + 90.0f, feet.z};
	Vec3 at = feet;
	for (float reach : {60.0f, 35.0f}) {
		const Vec3 to = feet + dir * reach;
		if (_world.lineFree(chest, {to.x, chest.y, to.z}, 10.0f)) {
			at = to;
			break;
		}
	}
	float floorY = feet.y;
	{
		float y;
		Vec3 normal;
		if (_world.rayDown({at.x, feet.y + 80.0f, at.z}, 260.0f, y, normal) && y > feet.y - 150.0f) floorY = y;
	}
	// Lying, not standing on its end: a long thing (a sword whose model stands up) is laid down; then turned any way
	// round and set on the floor by its lowest point.
	Quat rot = quatFromYaw(yaw + 1.3f + float((symbol * 2654435761u) >> 24) * 0.01f);
	float lift = 2.0f;
	if (const Model* m = _loot.lookModel(_pak, g, symbol)) {
		const ModelHeader& mh = m->header();
		const float size[3] = {mh.bbMax[0] - mh.bbMin[0], mh.bbMax[1] - mh.bbMin[1], mh.bbMax[2] - mh.bbMin[2]};
		if (size[1] > 1.5f * max(size[0], size[2]) && size[1] > 25.0f) rot = quatMul(rot, quatNormalize({0.70710678f, 0.0f, 0.0f, 0.70710678f}));
		const Mat34 r = mat34From(rot, {0, 0, 0});
		float low = 1e30f;
		for (int k = 0; k < 8; ++k) {
			const Vec3 c = {(k & 1) ? mh.bbMax[0] : mh.bbMin[0], (k & 2) ? mh.bbMax[1] : mh.bbMin[1], (k & 4) ? mh.bbMax[2] : mh.bbMin[2]};
			low = min(low, transformPoint(r, c).y);
		}
		if (low < 1e29f) lift = 0.5f - low;
	}
	at.y = floorY + lift;
	dropRelease(symbol, have - n);
	if (_loot.drop(g, symbol, n, at, rot) < 0) {
		itemMessage(tr("Сейчас нельзя"));
		return false;
	}
	g.giveItems(u32(hi), symbol, -n);
	++_itemStats.dropped;
	if (n > 1) snprintf(buf, sizeof(buf), tr("Выброшено: %s ×%ld"), name, static_cast<long>(n));
	else snprintf(buf, sizeof(buf), tr("Выброшено: %s"), name);
	itemMessage(buf, false);
	_invStamp = 0;
	return true;
}

// ---- the torch ----

bool App::toggleTorch(u32 symbol) {
	const int hi = heroIndex();
	if (hi < 0) return false;
	Game& g = _npcs.game();
	if (_torch.lit) {
		torchOut(true, tr("Факел потушен"));
		return true;
	}
	if (g.itemCount(u32(hi), symbol) <= 0) return false;
	if (_player.swimming) {
		itemMessage(tr("Не в воде"));
		return false;
	}
	if (_hero.down() || heroTransformed()) {
		itemMessage(tr("Сейчас нельзя"));
		return false;
	}
	_torch = Torch();
	_torch.lit = true;
	_torch.symbol = symbol;
	_torch.look = symbol;
	if (!_loot.lookModel(_pak, g, symbol)) {
		// (ItLsTorchBurning's look is "ITLSTORCHBURNING.ZEN", no model: the plain torch's, and the flame is ours.)
		const u32 plain = g.vm().find("ITLSTORCH");
		if (plain != NONE && g.isItem(plain)) _torch.look = plain;
	}
	++_itemStats.torches;
	itemMessage(tr("Факел зажжён"), false);
	_invStamp = 0;
	return true;
}

void App::torchOut(bool burnt, const char* why) {
	if (!_torch.lit) return;
	const int hi = heroIndex();
	if (burnt && hi >= 0 && _npcs.loaded() && _npcs.game().itemCount(u32(hi), _torch.symbol) > 0) _npcs.game().giveItems(u32(hi), _torch.symbol, -1);
	_particles.stop(_torch.flame);
	_torch = Torch();
	if (why) itemMessage(why, false);
	_invStamp = 0;
}

void App::torchUpdate(float) {
	if (!_torch.lit) return;
	const int hi = heroIndex();
	if (hi < 0 || !_npcs.loaded() || _npcs.game().itemCount(u32(hi), _torch.symbol) <= 0) {
		torchOut(false, nullptr); // (sold, given away, taken by the scripts)
		return;
	}
	if (_player.swimming || _underwater) {
		torchOut(true, tr("Факел погас в воде"));
		return;
	}
	if (_hero.down() && _deadTime > 0.0f) {
		torchOut(false, nullptr);
		return;
	}
	// (What is in the hands, or being taken into them: a two-hander, a bow, a spell take both.)
	const u8 w = _hero.weapon(), to = _hero.weaponIntent();
	_torch.shown = _hero.loaded() && !heroTransformed() && !_hero.down() && (w == WEAPON_NONE || w == WEAPON_FIST || w == WEAPON_1H) &&
	               (to == WEAPON_NONE || to == WEAPON_FIST || to == WEAPON_1H) && !_player.climbing &&
	               !(_job.stage != JOB_NONE && _job.state) && _mobJob.stage == MJ_NONE && !_player.flying;
	if (_torch.shown && _torch.tipKnown) {
		if (!_particles.alive(_torch.flame) || _torch.flameWorld != _world.entryHash()) {
			_torch.flame = _particles.start(Particles::torchFlame(), _torch.tip, 0.0f, 3600.0f);
			_torch.flameWorld = _world.entryHash();
		} else {
			_particles.move(_torch.flame, _torch.tip);
		}
	} else if (_torch.flame) {
		_particles.stop(_torch.flame);
		_torch.flame = 0;
	}
}

// After the hero: the torch in his left hand (where its flame is, for the next frame), and the thing he is using in
// his right (a potion, an apple, a broom: as the inhabitants hold theirs).
void App::drawHeldItems() {
	if (!_hero.loaded() || !_npcs.loaded()) return;
	Game& g = _npcs.game();
	if (_torch.lit && _torch.shown) {
		Vec3 tip = _hero.feet() + Vec3 {0.0f, 160.0f, 0.0f}; // (a skeleton without the hand: over his head)
		_loot.drawInHand(_pak, g, _hero.model(), _hero.pose(), _hero.yaw(), nullptr, _torch.look, _hero.feet(), _hero.light(), true, _hero.lean(), &tip);
		_torch.tip = tip;
		_torch.tipKnown = true;
	} else {
		_torch.tipKnown = false;
	}
	// The thing of a job in the right hand: while it is used (not one being picked up - it is on the ground until the
	// hand is there - nor one put down after the hand let go of it).
	if (_job.stage == JOB_USING && _job.symbol != NONE && !_job.pickup && !(_job.drop && _job.applied) && _hero.usingItem())
		_loot.drawInHand(_pak, g, _hero.model(), _hero.pose(), _hero.yaw(), nullptr, _job.symbol, _hero.feet(), _hero.light(), false, _hero.lean());
	else if (_doc.held != NONE && _hero.atMob() && _mobJob.stage == MJ_NONE)
		_loot.drawInHand(_pak, g, _hero.model(), _hero.pose(), _hero.yaw(), nullptr, _doc.held, _hero.feet(), _hero.light(), false, _hero.lean());
}

} // namespace g3

namespace g3 {

// fix202: what the player's character wears is what is drawn (the baked look "the hero in <armour>"); the naked hero
// when there is none, or that look was not baked (an older data.pak).
void App::updateHeroArmor() {
	const GameNpc* hn = heroNpc();
	const u32 armor = hn && !heroTransformed() ? hn->armorItem : 0xFFFFFFFFu;
	if (armor == _armorShown) return;
	_armorShown = armor;
	_armorReady = false;
	_armorBody.release();
	_armorHead.release();
	if (armor == 0xFFFFFFFFu || !_npcs.loaded() || !_hero.loaded()) return;
	Game& g = _npcs.game();
	GameLook look;
	if (!g.lookInArmor(g.heroIndex(), armor, look)) return;
	if (!_npcs.loadLook(_pak, look, _armorBody, _armorHead)) return;
	// (Only on the very skeleton the hero has: its matrices are given to them as they are.)
	_armorReady = _armorBody.nodeCount() == _hero.model().nodeCount() &&
	              (!_armorHead.loaded() || _armorHead.nodeCount() == _hero.model().nodeCount());
	if (!_armorReady) {
		_armorBody.release();
		_armorHead.release();
	}
}

void App::drawHero() {
	updateHeroArmor();
	if (!_armorReady) {
		_hero.draw();
		return;
	}
	static Mat34 skin[MODEL_MAX_NODES];
	_hero.model().skinMatrices(_hero.pose(), _hero.yaw(), _hero.lean(), nullptr, skin);
	_armorBody.drawSkinned(skin, _hero.feet(), _hero.light());
	if (_armorHead.loaded()) _armorHead.drawSkinned(skin, _hero.feet(), _hero.light());
}

} // namespace g3
