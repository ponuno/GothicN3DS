// Magic, the scripts' side: what a spell is (the arrays spellFXInstanceNames and spellFXAniLetters, the
// C_Spell instance "Spell_<Tag>", the visual effect "spellFX_<Tag>"), and the functions the scripts
// drive casting with (Spell_ProcessMana and Spell_ProcessMana_Release). How OpenGothic runs it:
// GameScript::invokeMana, spellDesc, spellVfx; Npc::beginCastSpell and tickCast.
#include "game.h"

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
	const u32 fx = _vm.find(name);
	const auto visOf = [&](const char* instName, const char* cls) -> u32 {
		const u32 s = _vm.find(instName);
		const u32 m = _vm.findMember(cls, "VISNAME_S");
		if (s == SCRIPT_NONE || m == SCRIPT_NONE || _vm.symbol(s).type != SCRIPT_INSTANCE) return 0;
		ScriptInstance* o = _vm.create(s);
		if (!o) return 0;
		const u32 h = fxHash(_vm.getString(m, 0, o));
		_vm.destroy(o);
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

} // namespace g3
