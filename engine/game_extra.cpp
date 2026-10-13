// Small engine functions of the scripts, as OpenGothic's GameScript has them: the day, the height between two
// characters, a bow with arrows.
#include "game.h"

#include <math.h>

namespace g3 {

// Wld_GetDay: the days since the story began (OpenGothic: the game time's day).
void Game::xGetDay(ScriptVm& vm, void* user) { vm.pushInt(static_cast<Game*>(user)->_day); }

// Npc_GetHeightToNpc(a, b): how far a is above or below b, whole centimetres, never negative.
void Game::xGetHeightToNpc(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const int b = g.indexOf(vm.popInstance()), a = g.indexOf(vm.popInstance());
	vm.pushInt(a >= 0 && b >= 0 ? i32(fabsf(g._npcs[a].pos[1] - g._npcs[b].pos[1])) : 0);
}

// Npc_HasRangedWeaponWithAmmo(self): a bow or crossbow carried and arrows (bolts) for it.
void Game::xHasRangedWeaponWithAmmo(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const int n = g.indexOf(vm.popInstance());
	bool ok = false;
	if (n >= 0 && g._npcs[n].ranged != SCRIPT_NONE) {
		const GameItemInfo* it = g.itemInfo(g._npcs[n].ranged);
		ok = it && it->munition >= 0 && g.itemCount(u32(n), u32(it->munition)) > 0;
	}
	vm.pushInt(ok);
}

} // namespace g3
