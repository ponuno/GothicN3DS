// Magic, the scripts' side: what a spell is (the arrays spellFXInstanceNames and spellFXAniLetters, the
// C_Spell instance "Spell_<Tag>", the visual effect "spellFX_<Tag>"), and the functions the scripts
// drive casting with (Spell_ProcessMana and Spell_ProcessMana_Release). How OpenGothic runs it:
// GameScript::invokeMana, spellDesc, spellVfx; Npc::beginCastSpell and tickCast.
#include "game.h"
#include "bytes.h"
#include "fight.h"

#include <stdlib.h>

#include <stdio.h>
#include <string.h>

namespace g3 {

namespace {

// Name hash of an instance by its text, for the particle effects (Particles::play).
u32 fxHash(const char* name) { return name && *name ? hashName(name) : 0u; }

} // namespace

const GameSpell* Game::spellInfo(i32 id) {
	if (id < 0 || id >= 64) return nullptr;
	if (_spellsRead[id]) return _spells[id].known ? &_spells[id] : nullptr;
	_spellsRead[id] = true;
	GameSpell& sp = _spells[id];
	sp = GameSpell();
	if (_spellNames == SCRIPT_NONE) {
		_spellNames = _vm.find("SPELLFXINSTANCENAMES");
		_spellLetters = _vm.find("SPELLFXANILETTERS");
		_spellProcess = _vm.find("SPELL_PROCESSMANA");
		_spellRelease = _vm.find("SPELL_PROCESSMANA_RELEASE");
		if (_spellNames == SCRIPT_NONE) _spellNames = SCRIPT_NONE - 1; // (looked for, not there)
	}
	if (_spellNames >= _vm.symbolCount() || _vm.symbol(_spellNames).type != SCRIPT_STRING || u32(id) >= _vm.symbol(_spellNames).count) return nullptr;
	sp.known = true;
	snprintf(sp.tag, sizeof(sp.tag), "%s", _vm.getString(_spellNames, u32(id)));
	if (_spellLetters < _vm.symbolCount() && _vm.symbol(_spellLetters).type == SCRIPT_STRING && u32(id) < _vm.symbol(_spellLetters).count)
		snprintf(sp.letters, sizeof(sp.letters), "%s", _vm.getString(_spellLetters, u32(id)));
	char name[64];
	// C_Spell.
	snprintf(name, sizeof(name), "SPELL_%s", sp.tag);
	const u32 inst = _vm.find(name);
	if (inst != SCRIPT_NONE && _vm.symbol(inst).type == SCRIPT_INSTANCE) {
		if (ScriptInstance* o = _vm.create(inst)) {
			const auto num = [&](const char* member, i32 otherwise) {
				const u32 m = _vm.findMember("C_SPELL", member);
				return m != SCRIPT_NONE && _vm.symbol(m).type == SCRIPT_INT ? _vm.getInt(m, 0, o) : otherwise;
			};
			const u32 tpm = _vm.findMember("C_SPELL", "TIME_PER_MANA");
			if (tpm != SCRIPT_NONE) sp.timePerMana = _vm.symbol(tpm).type == SCRIPT_FLOAT ? _vm.getFloat(tpm, 0, o) : float(_vm.getInt(tpm, 0, o));
			sp.damagePerLevel = num("DAMAGE_PER_LEVEL", 1);
			sp.damageType = num("DAMAGETYPE", 0);
			sp.spellType = num("SPELLTYPE", 1);
			sp.collectAlgo = num("TARGETCOLLECTALGO", 0);
			sp.collectType = num("TARGETCOLLECTTYPE", 0);
			sp.collectRange = num("TARGETCOLLECTRANGE", 0);
			_vm.destroy(o);
		}
	}
	if (sp.timePerMana < 50.0f) sp.timePerMana = 50.0f;
	const i32 none = constant("TARGET_COLLECT_NONE", 0), caster = constant("TARGET_COLLECT_CASTER", 1), focus = constant("TARGET_COLLECT_FOCUS", 2),
	          allNone = constant("TARGET_COLLECT_ALL_FALLBACK_NONE", 4);
	// (Whether a spell flies is the original engine's own business; OpenGothic goes by the collect algorithm, Stormfist apart.)
	sp.shoots = (sp.collectAlgo != none && sp.collectAlgo != caster && sp.collectAlgo != focus && sp.collectAlgo != allNone) || id == constant("SPL_STORMFIST", 47);
	// The visual effect: the particle effects of its keys.
	snprintf(name, sizeof(name), "SPELLFX_%s", sp.tag);
	ScriptVm& vfx = visualVm();
	const u32 fx = vfx.find(name);
	const auto visOf = [&](const char* instName, const char* cls) -> u32 {
		const u32 s = vfx.find(instName);
		const u32 m = vfx.findMember(cls, "VISNAME_S");
		if (s == SCRIPT_NONE || m == SCRIPT_NONE || vfx.symbol(s).type != SCRIPT_INSTANCE) return 0;
		ScriptInstance* o = vfx.create(s);
		if (!o) return 0;
		const u32 h = fxHash(vfx.getString(m, 0, o));
		vfx.destroy(o);
		return h;
	};
	if (fx != SCRIPT_NONE) {
		sp.fxFly = visOf(name, "CFX_BASE");
		char key[80];
		snprintf(key, sizeof(key), "SPELLFX_%s_KEY_INIT", sp.tag);
		sp.fxInit = visOf(key, "C_PARTICLEFXEMITKEY");
		snprintf(key, sizeof(key), "SPELLFX_%s_KEY_CAST", sp.tag);
		sp.fxCast = visOf(key, "C_PARTICLEFXEMITKEY");
		snprintf(key, sizeof(key), "SPELLFX_%s_KEY_COLLIDE", sp.tag);
		sp.fxCollide = visOf(key, "C_PARTICLEFXEMITKEY");
		sp.area = sp.collectAlgo == allNone && sp.collectRange > 0 && sp.damageType != 0;
		if (sp.area) {
			// The cast key creates the rain (emCreateFXID): its picture is the effect.
			snprintf(key, sizeof(key), "SPELLFX_%s_KEY_CAST", sp.tag);
			const u32 ks = vfx.find(key), km = vfx.findMember("C_PARTICLEFXEMITKEY", "EMCREATEFXID");
			if (ks != SCRIPT_NONE && km != SCRIPT_NONE && vfx.symbol(ks).type == SCRIPT_INSTANCE) {
				if (ScriptInstance* o = vfx.create(ks)) {
					char made[80];
					snprintf(made, sizeof(made), "%s", vfx.getString(km, 0, o));
					vfx.destroy(o);
					sp.fxArea = visOf(made, "CFX_BASE");
				}
			}
		}
	}
	return &sp;
}

i32 Game::processMana(u32 caster, int target, i32 invested, bool release) {
	spellInfo(0); // (finds the functions)
	const u32 fn = release ? _spellRelease : _spellProcess;
	const i32 stop = constant("SPL_SENDSTOP", 3);
	if (caster >= _npcCount || !_npcs[caster].object || fn >= _vm.symbolCount() || !_vm.isPlainFunction(fn)) return stop;
	const u32 self = _vm.selfSymbol(), other = _vm.otherSymbol();
	ScriptInstance* const selfBefore = self != SCRIPT_NONE ? _vm.instanceOf(self) : nullptr;
	ScriptInstance* const otherBefore = other != SCRIPT_NONE ? _vm.instanceOf(other) : nullptr;
	if (self != SCRIPT_NONE) _vm.setInstance(self, _npcs[caster].object);
	if (other != SCRIPT_NONE) _vm.setInstance(other, target >= 0 && u32(target) < _npcCount ? _npcs[u32(target)].object : nullptr);
	_vm.pushInt(invested);
	i32 code = stop;
	if (_vm.call(fn)) code = _vm.popInt();
	if (self != SCRIPT_NONE) _vm.setInstance(self, selfBefore);
	if (other != SCRIPT_NONE) _vm.setInstance(other, otherBefore);
	return code;
}

void Game::xActiveSpell(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const GameNpc* n = g.npcOf(vm.popInstance());
	vm.pushInt(n ? n->spell : -1);
}

void Game::xActiveSpellLevel(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const GameNpc* n = g.npcOf(vm.popInstance());
	vm.pushInt(n ? n->spellLevel : 0);
}

void Game::xActiveSpellCat(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const GameNpc* n = g.npcOf(vm.popInstance());
	const GameSpell* sp = n && n->spell >= 0 ? g.spellInfo(n->spell) : nullptr;
	vm.pushInt(sp ? sp->spellType : 1);
}

void Game::xLastHitSpellId(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const GameNpc* n = g.npcOf(vm.popInstance());
	vm.pushInt(n ? n->lastHitSpell : -1);
}

void Game::xLastHitSpellCat(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const GameNpc* n = g.npcOf(vm.popInstance());
	const GameSpell* sp = n && n->lastHitSpell >= 0 ? g.spellInfo(n->lastHitSpell) : nullptr;
	vm.pushInt(sp ? sp->spellType : 1);
}

u32 Game::spellItemOf(u32 npc, i32 spell) {
	if (npc >= _npcCount || spell < 0) return SCRIPT_NONE;
	const i32 rune = constant("ITEM_KAT_RUNE", 1 << 9);
	for (u32 i = 0; i < _itemCount; ++i) {
		const GameItem& st = _items[i];
		if (st.npc != npc || st.count <= 0) continue;
		const GameItemInfo* it = itemInfo(st.symbol);
		if (it && (it->mainflag & rune) && it->spell == spell) return st.symbol;
	}
	return SCRIPT_NONE;
}

u8 Game::rangedModeOf(u32 npc, bool withAmmo) {
	if (npc >= _npcCount || _npcs[npc].ranged == SCRIPT_NONE) return WEAPON_NONE;
	const GameItemInfo* it = itemInfo(_npcs[npc].ranged);
	if (!it) return WEAPON_NONE;
	if (withAmmo && (it->munition < 0 || itemCount(npc, u32(it->munition)) <= 0)) return WEAPON_NONE;
	return (it->flags & constant("ITEM_CROSSBOW", 1 << 20)) ? WEAPON_CBOW : WEAPON_BOW;
}

u32 Game::bestRanged(u32 npc) {
	if (npc >= _npcCount) return SCRIPT_NONE;
	const i32 far = constant("ITEM_KAT_FF", 1 << 2);
	u32 best = SCRIPT_NONE;
	i32 bestDamage = -1;
	for (u32 i = 0; i < _itemCount; ++i) {
		const GameItem& st = _items[i];
		if (st.npc != npc || st.count <= 0) continue;
		const GameItemInfo* it = itemInfo(st.symbol);
		if (!it || !(it->mainflag & far)) continue;
		if (it->damageTotal > bestDamage) {
			bestDamage = it->damageTotal;
			best = st.symbol;
		}
	}
	return best;
}

// ---- transforming (fix151) ----
// As OpenGothic's Npc::commitSpell and Npc::TransformBack: the spell's scripts name the creature
// (Npc_SetActiveSpellInfo(self, Wolf)); when it goes off the caster's object is put aside, the creature's instance
// code is run on it (its visual, attributes, guild, damage), the things he carries are put aside too and his level
// stays his. Back again, all of it returns but what he gained as the creature: experience, learning points, level and
// the AI variables of the scripts.

void Game::dropTransform() {
	free(_trf.object);
	free(_trf.items);
	_trf = Transform();
}

bool Game::transform(u32 npc, u32 symbol) {
	if (_trf.on || npc >= _npcCount || !_npcs[npc].object) return false;
	if (symbol >= _vm.symbolCount() || _vm.symbol(symbol).type != SCRIPT_INSTANCE || _clsNpc == SCRIPT_NONE ||
	    _vm.classOf(symbol) != _clsNpc)
		return false;
	GameNpc& n = _npcs[npc];
	ScriptInstance* o = n.object;
	u32 strings[64];
	const u32 stringCount = _vm.stringMembers(o->classSymbol, strings, 64);
	ByteSink sink;
	_vm.saveObject(o, strings, stringCount, sink);
	if (sink.failed || !sink.size) return false;
	u8* copy = static_cast<u8*>(malloc(sink.size));
	if (!copy) return false;
	memcpy(copy, sink.data, sink.size);
	u32 carried = 0;
	for (u32 i = 0; i < _itemCount; ++i) carried += _items[i].npc == npc && _items[i].count > 0;
	GameItem* items = carried ? static_cast<GameItem*>(malloc(carried * sizeof(GameItem))) : nullptr;
	if (carried && !items) {
		free(copy);
		return false;
	}
	u32 k = 0;
	for (u32 i = 0; i < _itemCount; ++i)
		if (_items[i].npc == npc && _items[i].count > 0) {
			items[k++] = _items[i];
			_items[i].count = 0; // (the creature carries nothing: OpenGothic gives it an empty inventory)
		}
	++_itemsVersion;
	_trf.on = true;
	_trf.npc = npc;
	_trf.into = symbol;
	_trf.object = copy;
	_trf.objectSize = u32(sink.size);
	_trf.before = n;
	_trf.items = items;
	_trf.itemCount = carried;
	// What he wore and held is not the creature's.
	n.melee = n.ranged = n.armorItem = 0xFFFFFFFFu;
	n.weapon = WEAPON_NONE;
	n.spell = -1;
	n.spellLevel = 0;
	n.spellInfo = 0;
	n.trueGuild = -1;
	for (u32 i = 0; i < GameNpc::OVERLAYS; ++i) {
		n.overlay[i] = 0;
		n.overlayLeft[i] = 0.0f;
	}
	// The creature's code on his own object (SELF is he); his instance stays his name to the scripts.
	const u32 mLevel = _vm.findMember("C_NPC", "LEVEL");
	const i32 level = mLevel != SCRIPT_NONE ? _vm.getInt(mLevel, 0, o) : 0;
	const u32 own = o->symbol;
	o->symbol = symbol;
	_vm.construct(o);
	o->symbol = own;
	if (mLevel != SCRIPT_NONE) _vm.setInt(mLevel, 0, o, level);
	return true;
}

bool Game::transformBack(u32 npc) {
	if (!_trf.on || npc != _trf.npc || npc >= _npcCount || !_npcs[npc].object) return false;
	GameNpc& n = _npcs[npc];
	ScriptInstance* o = n.object;
	// What he gained as the creature stays his (OpenGothic: exp, exp_next, lp, level, aivar).
	const char* const keep[] = {"EXP", "EXP_NEXT", "LP", "LEVEL", "AIVAR"};
	u32 member[5];
	i32* kept[5] = {};
	u32 keptCount[5] = {};
	for (u32 i = 0; i < 5; ++i) {
		member[i] = _vm.findMember("C_NPC", keep[i]);
		if (member[i] == SCRIPT_NONE || _vm.symbol(member[i]).type != SCRIPT_INT) {
			member[i] = SCRIPT_NONE;
			continue;
		}
		const u32 count = _vm.symbol(member[i]).count ? _vm.symbol(member[i]).count : 1u;
		kept[i] = static_cast<i32*>(malloc(count * sizeof(i32)));
		if (!kept[i]) continue;
		keptCount[i] = count;
		for (u32 k = 0; k < count; ++k) kept[i][k] = _vm.getInt(member[i], k, o);
	}
	u32 strings[64];
	const u32 stringCount = _vm.stringMembers(o->classSymbol, strings, 64);
	ByteSource src(_trf.object, _trf.objectSize);
	_vm.loadObject(o, strings, stringCount, src);
	for (u32 i = 0; i < 5; ++i) {
		for (u32 k = 0; k < keptCount[i]; ++k) _vm.setInt(member[i], k, o, kept[i][k]);
		free(kept[i]);
	}
	// What he carried, and nothing the creature was given.
	for (u32 i = 0; i < _itemCount; ++i)
		if (_items[i].npc == npc) _items[i].count = 0;
	for (u32 i = 0; i < _trf.itemCount; ++i) addItems(o, i32(_trf.items[i].symbol), _trf.items[i].count);
	++_itemsVersion;
	const GameNpc& b = _trf.before;
	n.look = b.look;
	memcpy(n.talent, b.talent, sizeof(n.talent));
	memcpy(n.skill, b.skill, sizeof(n.skill));
	n.melee = b.melee;
	n.ranged = b.ranged;
	n.armorItem = b.armorItem;
	n.trueGuild = b.trueGuild;
	for (u32 i = 0; i < GameNpc::OVERLAYS; ++i) {
		n.overlay[i] = b.overlay[i];
		n.overlayLeft[i] = b.overlayLeft[i];
	}
	n.weapon = WEAPON_NONE;
	n.spell = -1;
	n.spellLevel = 0;
	n.spellInfo = 0;
	dropTransform();
	return true;
}

} // namespace g3
