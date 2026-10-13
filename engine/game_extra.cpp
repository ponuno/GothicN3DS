// Small engine functions of the scripts, as OpenGothic's GameScript has them: the day, the height between two
// characters, a bow with arrows.
#include "game.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace g3 {

namespace {
void copyText(char* to, size_t size, const char* from) {
	size_t n = from ? strlen(from) : 0;
	if (n >= size) n = size - 1;
	memcpy(to, from ? from : "", n);
	to[n] = 0;
}
} // namespace

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

// ---- rooms and owners (as OpenGothic's GameScript: wld_assignroomtoguild, wld_getplayerportalguild,
// npc_getportalguild, npc_isinplayersroom, npc_ownedbynpc) ----

bool Game::setRoom(u32 npc, u32 roomHash) {
	if (npc >= _npcCount) return false;
	_npcs[npc].room = roomHash;
	if (npc != _hero || roomHash == _heroRoom) return false;
	_heroFormerRoom = _heroRoom;
	_heroRoom = roomHash;
	return true;
}

i32 Game::roomGuild(u32 roomHash) const {
	if (!roomHash) return -1;
	for (u32 i = 0; i < _roomGuildCount; ++i)
		if (_roomGuilds[i].world == _world && _roomGuilds[i].hash == roomHash) return _roomGuilds[i].guild;
	return constant("GIL_NONE", 0);
}

// Wld_AssignRoomToGuild(room, guild): the room (a sector's name, any case) of the world whose start-up runs.
void Game::xAssignRoomToGuild(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 guild = vm.popInt();
	const char* name = vm.popString();
	if (!name || !name[0]) return;
	const u32 h = hashName(name);
	for (u32 i = 0; i < g._roomGuildCount; ++i)
		if (g._roomGuilds[i].world == g._world && g._roomGuilds[i].hash == h) {
			g._roomGuilds[i].guild = guild;
			return;
		}
	if (g._roomGuildCount >= g._roomGuildCap) {
		const u32 cap = g._roomGuildCap ? g._roomGuildCap * 2 : 64;
		RoomGuild* grown = static_cast<RoomGuild*>(realloc(static_cast<void*>(g._roomGuilds), size_t(cap) * sizeof(RoomGuild)));
		if (!grown) return;
		g._roomGuilds = grown;
		g._roomGuildCap = cap;
	}
	g._roomGuilds[g._roomGuildCount++] = RoomGuild {g._world, h, guild};
}

void Game::xPlayerPortalGuild(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	vm.pushInt(g.roomGuild(g._heroRoom));
}

void Game::xFormerPlayerPortalGuild(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	vm.pushInt(g.roomGuild(g._heroFormerRoom));
}

void Game::xNpcPortalGuild(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const int n = g.indexOf(vm.popInstance());
	vm.pushInt(n >= 0 ? g.roomGuild(g._npcs[n].room) : -1);
}

// Npc_IsInPlayersRoom(npc): both in the same room (outside is no room).
void Game::xIsInPlayersRoom(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const int n = g.indexOf(vm.popInstance());
	vm.pushInt(n >= 0 && g._heroRoom != 0 && g._npcs[n].room == g._heroRoom);
}

// Npc_OwnedByNpc(item, npc): the item's OWNER is the character's instance.
void Game::xOwnedByNpc(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const int n = g.indexOf(vm.popInstance());
	ScriptInstance* item = vm.popInstance();
	bool owned = false;
	if (n >= 0 && item && g._iOwner != SCRIPT_NONE && item->classSymbol == g._clsItem) {
		const i32 owner = g._vm.getInt(g._iOwner, 0, item);
		owned = owner > 0 && u32(owner) == g._npcs[n].symbol;
	}
	vm.pushInt(owned);
}

// AI_PrintScreen(text, x, y, font, seconds) (Gothic 2): as PrintScreen, at once.
void Game::xAiPrintScreen(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 seconds = vm.popInt();
	(void)vm.popString();
	(void)vm.popInt();
	(void)vm.popInt();
	char copy[96];
	copyText(copy, sizeof(copy), vm.popString());
	if (copy[0]) g.pushMessage(copy, seconds > 0 ? float(seconds) : 3.0f);
	vm.pushInt(0);
}

void Game::xSetTrueGuild(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 guild = vm.popInt();
	const int n = g.indexOf(vm.popInstance());
	if (n >= 0) g._npcs[n].trueGuild = guild;
	vm.pushInt(0);
}

void Game::xReturnFalse(ScriptVm& vm, void*) { vm.pushInt(0); }
void Game::xReturnTrue(ScriptVm& vm, void*) { vm.pushInt(1); }

// ---- summoning ----

u32 Game::spawnNear(u32 symbol, u32 nearNpc, u32 count) {
	if (nearNpc >= _npcCount || count == 0) return SCRIPT_NONE;
	const bool isNpc = symbol < _vm.symbolCount() && _vm.symbol(symbol).type == SCRIPT_INSTANCE && _clsNpc != SCRIPT_NONE &&
	                   _vm.classOf(symbol) == _clsNpc;
	if (!isNpc) return SCRIPT_NONE;
	if (count > 16) count = 16;
	u32 first = SCRIPT_NONE;
	for (u32 i = 0; i < count; ++i) {
		const u32 index = addNpc(symbol, false);
		if (index == SCRIPT_NONE) break;
		if (first == SCRIPT_NONE) first = index;
		// OpenGothic's wld_spawnnpcrange: a metre from the caster, round him.
		const GameNpc& at = _npcs[nearNpc];
		GameNpc& n = _npcs[index];
		const float a = at.yaw + 6.2831853f * float(i) / float(count);
		n.pos[0] = at.pos[0] + sinf(a) * 100.0f;
		n.pos[1] = at.pos[1];
		n.pos[2] = at.pos[2] + cosf(a) * 100.0f;
		n.yaw = at.yaw;
		n.world = at.world;
		n.placed = true;
		n.spawned = true;
		n.moved = 1;
		n.want = n.at = 0;
		++_stats.moves;
	}
	return first;
}

u32 Game::probeNpc(u32 symbol) {
	const bool isNpc = symbol < _vm.symbolCount() && _vm.symbol(symbol).type == SCRIPT_INSTANCE &&
	                   (_vm.symbol(symbol).flags & SCRIPTF_CONST) && _clsNpc != SCRIPT_NONE && _vm.classOf(symbol) == _clsNpc;
	if (!isNpc) return SCRIPT_NONE;
	const u32 index = addNpc(symbol, false);
	if (index != SCRIPT_NONE) {
		_npcs[index].world = 0xFFFF;
		_npcs[index].placed = false;
	}
	return index;
}

// Wld_SpawnNpcRange(near, instance, count, lifetime): the lifetime is not kept (OpenGothic neither).
void Game::xSpawnNpcRange(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	(void)vm.popFloat();
	const i32 count = vm.popInt();
	const i32 symbol = vm.popInt();
	const int at = g.indexOf(vm.popInstance());
	if (at < 0 || symbol <= 0 || count <= 0) return;
	g.spawnNear(u32(symbol), u32(at), u32(count));
}

// Wld_RemoveNpc(instance): the first of that instance in the world is taken out of it.
void Game::xRemoveNpc(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 symbol = vm.popInt();
	for (u32 i = 0; i < g._npcCount; ++i) {
		GameNpc& n = g._npcs[i];
		if (n.hero || i32(n.symbol) != symbol || n.world != g._world) continue;
		n.world = 0xFFFF;
		n.placed = false;
		n.active = false;
		++n.moved;
		return;
	}
}

// Wld_PlayEffect(visual, source, target, level, damage, damageType, projectile): as OpenGothic, only the effect
// without damage or flight; it is shown at the source (a character). The visual is a VisualFX instance whose
// particle effect (VISNAME_S) is played, or a particle effect itself.
void Game::xPlayEffect(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 projectile = vm.popInt(), damageType = vm.popInt(), damage = vm.popInt(), level = vm.popInt();
	(void)vm.popInstance();
	ScriptInstance* source = vm.popInstance();
	char visual[64];
	copyText(visual, sizeof(visual), vm.popString());
	if (projectile || damageType || damage || level || !visual[0]) return;
	u32 pfx = 0;
	ScriptVm& vfx = g.visualVm();
	const u32 s = vfx.find(visual), m = vfx.findMember("CFX_BASE", "VISNAME_S");
	if (s != SCRIPT_NONE && m != SCRIPT_NONE && vfx.symbol(s).type == SCRIPT_INSTANCE) {
		if (ScriptInstance* o = vfx.create(s)) {
			const char* vis = vfx.getString(m, 0, o);
			if (vis && vis[0]) pfx = hashName(vis);
			vfx.destroy(o);
		}
	}
	if (!pfx && g.particleVm().find(visual) != SCRIPT_NONE) pfx = hashName(visual);
	if (!pfx) return;
	if (g._effectCount == 8) {
		g._effectFirst = (g._effectFirst + 1) % 8;
		--g._effectCount;
	}
	g._effects[(g._effectFirst + g._effectCount++) % 8] = Effect {pfx, g.indexOf(source)};
}

bool Game::takeEffect(Effect& out) {
	if (!_effectCount) return false;
	out = _effects[_effectFirst];
	_effectFirst = (_effectFirst + 1) % 8;
	--_effectCount;
	return true;
}

// Wld_SendTrigger(name) / Wld_SendUntrigger(name): to the world's triggers of that name (a gate, a lift).
void Game::xSendTrigger(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	char name[48];
	copyText(name, sizeof(name), vm.popString());
	if (!name[0]) return;
	if (g._triggerCount == 16) {
		g._triggerFirst = (g._triggerFirst + 1) % 16;
		--g._triggerCount;
	}
	TriggerEvent& e = g._triggerEvents[(g._triggerFirst + g._triggerCount++) % 16];
	memcpy(e.name, name, sizeof(name));
	e.untrigger = false;
}

void Game::xSendUntrigger(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	xSendTrigger(vm, user);
	if (g._triggerCount) g._triggerEvents[(g._triggerFirst + g._triggerCount - 1) % 16].untrigger = true;
}

bool Game::takeTrigger(char* name, u32 size, bool& untrigger) {
	if (!_triggerCount || size == 0) return false;
	const TriggerEvent& e = _triggerEvents[_triggerFirst];
	copyText(name, size, e.name);
	for (char* c = name; *c; ++c)
		if (*c >= 'a' && *c <= 'z') *c = char(*c - 32);
	untrigger = e.untrigger;
	_triggerFirst = (_triggerFirst + 1) % 16;
	--_triggerCount;
	return true;
}

bool Game::triggerState(u16 world, u32 index, u32& value) const {
	for (u32 i = 0; i < _triggerKeptCount; ++i)
		if (_triggerKept[i].world == world && _triggerKept[i].index == index) {
			value = _triggerKept[i].value;
			return true;
		}
	return false;
}

void Game::setTriggerState(u16 world, u32 index, u32 value) {
	for (u32 i = 0; i < _triggerKeptCount; ++i)
		if (_triggerKept[i].world == world && _triggerKept[i].index == index) {
			_triggerKept[i].value = value;
			return;
		}
	if (_triggerKeptCount >= _triggerKeptCap) {
		const u32 cap = _triggerKeptCap ? _triggerKeptCap * 2 : 64;
		TriggerKept* grown = static_cast<TriggerKept*>(realloc(static_cast<void*>(_triggerKept), size_t(cap) * sizeof(TriggerKept)));
		if (!grown) return;
		_triggerKept = grown;
		_triggerKeptCap = cap;
	}
	_triggerKept[_triggerKeptCount++] = TriggerKept {world, index, value};
}

void Game::bindExtra() {
	_iOwner = _vm.findMember("C_ITEM", "OWNER");
	static const struct {
		const char* name;
		ScriptVm::External fn;
	} table[] = {{"WLD_ASSIGNROOMTOGUILD", xAssignRoomToGuild},
	             {"WLD_GETPLAYERPORTALGUILD", xPlayerPortalGuild},
	             {"WLD_GETFORMERPLAYERPORTALGUILD", xFormerPlayerPortalGuild},
	             {"NPC_GETPORTALGUILD", xNpcPortalGuild},
	             {"NPC_ISINPLAYERSROOM", xIsInPlayersRoom},
	             {"NPC_OWNEDBYNPC", xOwnedByNpc},
	             {"AI_PRINTSCREEN", xAiPrintScreen},
	             {"NPC_SETTRUEGUILD", xSetTrueGuild},
	             {"WLD_SPAWNNPCRANGE", xSpawnNpcRange},
	             {"WLD_REMOVENPC", xRemoveNpc},
	             {"WLD_PLAYEFFECT", xPlayEffect},
	             {"WLD_SENDTRIGGER", xSendTrigger},
	             {"WLD_SENDUNTRIGGER", xSendUntrigger},
	             // No weather here (OpenGothic: always dry); the language functions only answer yes.
	             {"WLD_ISRAINING", xReturnFalse},
	             {"GAME_INITENGLISH", xReturnTrue},
	             {"GAME_INITGERMAN", xReturnTrue}};
	for (const auto& e : table) _vm.bind(e.name, e.fn, this);
}

} // namespace g3
