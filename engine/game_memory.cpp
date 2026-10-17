// fix199: the original's functions the scripts of both games call that this engine lacked.
//
// - The characters' memory of what happened (ZenGin's oCNewsMemory): Npc_MemoryEntry / Npc_MemoryEntryGuild write a
//   piece of news (a murder, a theft, a defeat ...) into a character's memory; Npc_HasNews finds one again, the other
//   functions read and forget it. Gothic 1's people are built on it (B_AssessAndMemorize, B_ReactToMemory,
//   ZS_ProclaimAndPunish: "you killed ...", "you stole ...", "you beat me"); without it they forgot everything at once.
//   The scripts write all of it themselves; the engine only keeps it (and in the saved game). Spreading news by gossip
//   (the original does it among people standing together) is not done.
// - The containers by the world's names for them: Mob_CreateItems / Mob_HasItems (Gothic 2's quests fill chests so).
//   The containers belong to the world the App shows; the start-up scripts run before it has them, so what they put in
//   waits here until the App takes it (takeContainerItems).
// - Small ones: Npc_GiveItem, Npc_GetGuildAttitude, Npc_IsNear, Wld_DetectPlayer, Wld_StopEffect.
#include "game.h"
#include "bytes.h"

#include <math.h>
#include <string.h>

namespace g3 {

namespace {

// The news' numbers the scripts get from Npc_HasNews (> 0); unique, so a number of a forgotten piece finds nothing.
void copyText(char* to, size_t size, const char* from) {
	size_t n = from ? strlen(from) : 0;
	if (n >= size) n = size - 1;
	memcpy(to, from ? from : "", n);
	to[n] = 0;
}

u32 nextNewsNumber(u32& next) {
	if (++next == 0) next = 1;
	return next;
}

} // namespace

int Game::newsAt(int holder, i32 number) const {
	if (holder < 0 || number <= 0) return -1;
	for (u32 i = 0; i < _newsCount; ++i)
		if (_news[i].holder == u32(holder) && _news[i].number == u32(number)) return int(i);
	return -1;
}

void Game::addNews(u32 holder, bool gossip, int offender, i32 newsId, int victim, i32 victimGuild) {
	// The same news about the same people is only kept once (the scripts write it each time it is seen).
	for (u32 i = 0; i < _newsCount; ++i) {
		const News& n = _news[i];
		if (n.holder == holder && n.id == newsId && n.offender == offender && n.victim == victim && n.victimGuild == victimGuild) {
			if (!gossip) _news[i].gossip = false;
			return;
		}
	}
	if (_newsCount == MAX_NEWS) {
		// Full: the oldest piece is forgotten.
		memmove(&_news[0], &_news[1], sizeof(News) * (MAX_NEWS - 1));
		--_newsCount;
	}
	News& n = _news[_newsCount++];
	n.number = nextNewsNumber(_newsNext);
	n.holder = holder;
	n.offender = offender;
	n.victim = victim;
	n.witness = gossip ? -1 : int(holder);
	n.id = newsId;
	n.victimGuild = victimGuild;
	n.gossip = gossip;
}

// Npc_MemoryEntry(self, source, offender, newsid, victim): source > 0 is gossip, else self saw it.
void Game::xMemoryEntry(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const int victim = g.indexOf(vm.popInstance());
	const i32 newsId = vm.popInt();
	const int offender = g.indexOf(vm.popInstance());
	const i32 source = vm.popInt();
	const int holder = g.indexOf(vm.popInstance());
	if (holder < 0) return;
	g.addNews(u32(holder), source > 0, offender, newsId, victim, victim >= 0 ? g.guildOf(g._npcs[victim]) : -1);
}

// Npc_MemoryEntryGuild(self, source, offender, newsid, victimguild): the victim is a whole guild.
void Game::xMemoryEntryGuild(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	// (Declared as a character, given a guild's number.)
	const i32 guild = vm.popInt();
	const i32 newsId = vm.popInt();
	const int offender = g.indexOf(vm.popInstance());
	const i32 source = vm.popInt();
	const int holder = g.indexOf(vm.popInstance());
	if (holder < 0) return;
	g.addNews(u32(holder), source > 0, offender, newsId, -1, guild);
}

// Npc_HasNews(self, newsid, offender, victim): the number of such a piece of news, 0 if none; NULL matches anybody.
// The newest is found first (the original's memory is a list with the latest at its head).
void Game::xHasNews(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	ScriptInstance* victimInst = vm.popInstance();
	ScriptInstance* offenderInst = vm.popInstance();
	const i32 newsId = vm.popInt();
	const int holder = g.indexOf(vm.popInstance());
	const int victim = g.indexOf(victimInst), offender = g.indexOf(offenderInst);
	i32 found = 0;
	if (holder >= 0) {
		for (u32 k = g._newsCount; k-- > 0;) {
			const News& n = g._news[k];
			if (n.holder != u32(holder) || n.id != newsId) continue;
			if (offenderInst && n.offender != offender) continue;
			if (victimInst) {
				// A guild's news is about each of its people.
				const bool same = n.victim >= 0 ? n.victim == victim
				                                : victim >= 0 && n.victimGuild >= 0 && g.guildOf(g._npcs[victim]) == n.victimGuild;
				if (!same) continue;
			}
			found = i32(n.number);
			break;
		}
	}
	vm.pushInt(found);
}

void Game::xIsNewsGossip(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 number = vm.popInt();
	const int at = g.newsAt(g.indexOf(vm.popInstance()), number);
	vm.pushInt(at >= 0 && g._news[at].gossip);
}

void Game::pushNewsPerson(ScriptVm& vm, int who) {
	vm.pushInstance(who >= 0 && u32(who) < _npcCount ? _npcs[who].object : nullptr);
}

void Game::xGetNewsWitness(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 number = vm.popInt();
	const int at = g.newsAt(g.indexOf(vm.popInstance()), number);
	g.pushNewsPerson(vm, at >= 0 ? g._news[at].witness : -1);
}

void Game::xGetNewsVictim(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 number = vm.popInt();
	const int at = g.newsAt(g.indexOf(vm.popInstance()), number);
	g.pushNewsPerson(vm, at >= 0 ? g._news[at].victim : -1);
}

void Game::xGetNewsOffender(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 number = vm.popInt();
	const int at = g.newsAt(g.indexOf(vm.popInstance()), number);
	g.pushNewsPerson(vm, at >= 0 ? g._news[at].offender : -1);
}

void Game::xDeleteNews(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 number = vm.popInt();
	const int at = g.newsAt(g.indexOf(vm.popInstance()), number);
	if (at < 0) return;
	memmove(&g._news[at], &g._news[at + 1], sizeof(News) * (g._newsCount - u32(at) - 1));
	--g._newsCount;
}

u32 Game::newsCount(u32 holder) const {
	u32 n = 0;
	for (u32 i = 0; i < _newsCount; ++i) n += _news[i].holder == holder;
	return n;
}

// ---- the containers ----

void Game::xMobCreateItems(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 amount = vm.popInt();
	const i32 item = vm.popInt();
	const u32 name = hashName(vm.popString());
	if (amount <= 0 || item < 0 || !g.isItem(u32(item))) return;
	// A container of the world shown takes it at once; else it waits for the App (the start-up runs first).
	if (g._containers.add && g._containers.add(g._containers.user, name, u32(item), amount)) return;
	if (g._chestGiftCount == MAX_CHEST_GIFTS) {
		++g._stats.chestGiftsLost;
		return;
	}
	g._chestGifts[g._chestGiftCount++] = ChestGift {name, u32(item), amount};
}

void Game::xMobHasItems(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 item = vm.popInt();
	const u32 name = hashName(vm.popString());
	i32 n = 0;
	if (item >= 0 && g._containers.count) n = g._containers.count(g._containers.user, name, u32(item));
	// What waits to be put in counts as in.
	for (u32 i = 0; i < g._chestGiftCount; ++i)
		if (g._chestGifts[i].name == name && g._chestGifts[i].item == u32(item)) n += g._chestGifts[i].count;
	vm.pushInt(n);
}

bool Game::takeContainerItem(ChestGift& out) {
	if (!_chestGiftCount) return false;
	out = _chestGifts[0];
	memmove(&_chestGifts[0], &_chestGifts[1], sizeof(ChestGift) * (_chestGiftCount - 1));
	--_chestGiftCount;
	return true;
}

// ---- small ones ----

// Npc_GiveItem(from, item, to): the thing goes over at once.
void Game::xGiveItem(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const int to = g.indexOf(vm.popInstance());
	const ScriptInstance* item = vm.popInstance();
	const int from = g.indexOf(vm.popInstance());
	if (from < 0 || to < 0 || !item || item->symbol == SCRIPT_NONE || !g.isItem(item->symbol)) return;
	if (g.itemCount(u32(from), item->symbol) <= 0) return;
	g.giveItems(u32(from), item->symbol, -1);
	g.giveItems(u32(to), item->symbol, 1);
}

// Npc_GetGuildAttitude(npc1, npc2): the attitude of their guilds.
void Game::xGetGuildAttitudeNpc(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const int b = g.indexOf(vm.popInstance());
	const int a = g.indexOf(vm.popInstance());
	vm.pushInt(a >= 0 && b >= 0 ? g.guildAttitude(g.guildOf(g._npcs[a]), g.guildOf(g._npcs[b])) : 0);
}

float Game::distance(u32 a, u32 b) const {
	const GameNpc& p = _npcs[a];
	const GameNpc& q = _npcs[b];
	if (!p.placed || !q.placed || p.world != q.world) return 1e9f;
	const float dx = p.pos[0] - q.pos[0], dy = p.pos[1] - q.pos[1], dz = p.pos[2] - q.pos[2];
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

// Npc_IsNear(self, other): within three metres.
void Game::xIsNear(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const int b = g.indexOf(vm.popInstance());
	const int a = g.indexOf(vm.popInstance());
	vm.pushInt(a >= 0 && b >= 0 && g.distance(u32(a), u32(b)) <= 300.0f);
}

// Wld_DetectPlayer(self): the player is within its senses.
void Game::xDetectPlayer(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const int a = g.indexOf(vm.popInstance());
	bool seen = false;
	if (a >= 0 && g._hero != SCRIPT_NONE && u32(a) != g._hero) {
		const float range = g.sensesRange(g._npcs[a]);
		seen = g.distance(u32(a), g._hero) <= (range > 0.0f ? range : 2000.0f);
	}
	vm.pushInt(seen);
}

// fix201: PlayVideo(name) / PlayVideoEx(name, screenBlend, exitSession): the App plays it (video.pak) when it can;
// the script goes on at once (the original stands still until the video is over: nothing it does is seen meanwhile).
void Game::queueVideo(const char* name) {
	if (!name || !*name || _videoCount == MAX_VIDEOS) return;
	copyText(_videos[_videoCount++], sizeof(_videos[0]), name);
	++_stats.videosAsked;
}

bool Game::takeVideo(char* out, u32 size) {
	if (!_videoCount || !size) return false;
	copyText(out, size, _videos[0]);
	memmove(&_videos[0], &_videos[1], sizeof(_videos[0]) * (_videoCount - 1));
	--_videoCount;
	return true;
}

void Game::xPlayVideo(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	g.queueVideo(vm.popString());
	vm.pushInt(1);
}

void Game::xPlayVideoEx(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 exitSession = vm.popInt();
	(void)vm.popInt(); // screenBlend
	g.queueVideo(vm.popString());
	(void)exitSession; // (the end of the game after the last video: the scripts' own ExitSession follows anyway)
	vm.pushInt(1);
}

// Wld_StopEffect(name): the effects shown here end by themselves after a moment (Wld_PlayEffect).
void Game::xStopEffect(ScriptVm& vm, void*) { (void)vm.popString(); }

void Game::bindMemory() {
	static const struct {
		const char* name;
		ScriptVm::External fn;
	} kBind[] = {
	    {"NPC_MEMORYENTRY", xMemoryEntry},       {"NPC_MEMORYENTRYGUILD", xMemoryEntryGuild}, {"NPC_HASNEWS", xHasNews},
	    {"NPC_ISNEWSGOSSIP", xIsNewsGossip},     {"NPC_GETNEWSWITNESS", xGetNewsWitness},     {"NPC_GETNEWSVICTIM", xGetNewsVictim},
	    {"NPC_GETNEWSOFFENDER", xGetNewsOffender}, {"NPC_DELETENEWS", xDeleteNews},           {"MOB_CREATEITEMS", xMobCreateItems},
	    {"MOB_HASITEMS", xMobHasItems},          {"NPC_GIVEITEM", xGiveItem},                 {"NPC_GETGUILDATTITUDE", xGetGuildAttitudeNpc},
	    {"NPC_ISNEAR", xIsNear},                 {"WLD_DETECTPLAYER", xDetectPlayer},         {"WLD_STOPEFFECT", xStopEffect},
	    {"WLD_SETOBJECTROUTINE", xSetObjectRoutine}, {"WLD_SETMOBROUTINE", xSetMobRoutine},
	    {"PLAYVIDEO", xPlayVideo},               {"PLAYVIDEOEX", xPlayVideoEx},
	};
	for (const auto& b : kBind) _vm.bind(b.name, b.fn, this);
}

// ---- the object routines ----
//
// Wld_SetObjectRoutine(hour, min, vobName, state): from that time of day the object of that name is triggered (state
// 1) or untriggered (0): Gothic 1's camps light their crystal lamps and fires at night, close the arena's gate at 20:00,
// the Free Mine's gate after it is taken. Wld_SetMobRoutine is the same for all mobs of a scheme (FIREPLACE); those are
// kept and counted but change nothing seen here (the mobs have no lit and unlit look of their own yet).
// The table is the scripts' (the start-up writes it once per world) and goes into the saved game with the state last
// sent, so that a load does not send it again.

void Game::addObjectRoutine(i32 hour, i32 minute, const char* name, i32 state, bool mob) {
	if (!name || !*name) return;
	const u32 h = hashName(name);
	u32 at = _objNameCount;
	for (u32 i = 0; i < _objNameCount; ++i)
		if (_objNames[i].hash == h && _objNames[i].world == _world && _objNames[i].mob == mob) at = i;
	if (at == _objNameCount) {
		if (_objNameCount == MAX_OBJ_NAMES) {
			++_stats.routinesLost;
			return;
		}
		ObjName& n = _objNames[_objNameCount++];
		copyText(n.name, sizeof(n.name), name);
		n.hash = h;
		n.world = _world;
		n.mob = mob;
		n.applied = -1;
	}
	const u16 when = u16(((hour % 24 + 24) % 24) * 60 + ((minute % 60 + 60) % 60));
	for (u32 i = 0; i < _objRoutineCount; ++i)
		if (_objRoutines[i].name == at && _objRoutines[i].minute == when) {
			_objRoutines[i].state = u8(state != 0);
			return;
		}
	if (_objRoutineCount == MAX_OBJ_ROUTINES) {
		++_stats.routinesLost;
		return;
	}
	_objRoutines[_objRoutineCount++] = ObjRoutine {u16(at), when, u8(state != 0)};
}

void Game::xSetObjectRoutine(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 state = vm.popInt();
	char name[48];
	copyText(name, sizeof(name), vm.popString());
	const i32 minute = vm.popInt(), hour = vm.popInt();
	g.addObjectRoutine(hour, minute, name, state, false);
}

void Game::xSetMobRoutine(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 state = vm.popInt();
	char name[48];
	copyText(name, sizeof(name), vm.popString());
	const i32 minute = vm.popInt(), hour = vm.popInt();
	g.addObjectRoutine(hour, minute, name, state, true);
}

// What each object of this world should be at this time of day; what changed is sent.
void Game::applyObjectRoutines() {
	const u16 now = u16(u32(_hours * 60.0f) % 1440u);
	for (u32 n = 0; n < _objNameCount; ++n) {
		ObjName& on = _objNames[n];
		if (on.world != _world) continue;
		// The last switch at or before now; none today: the last of the day before.
		int best = -1, latest = -1;
		for (u32 i = 0; i < _objRoutineCount; ++i) {
			const ObjRoutine& r = _objRoutines[i];
			if (r.name != n) continue;
			if (r.minute <= now && (best < 0 || r.minute > _objRoutines[best].minute)) best = int(i);
			if (latest < 0 || r.minute > _objRoutines[latest].minute) latest = int(i);
		}
		if (best < 0) best = latest;
		if (best < 0) continue;
		const i8 state = i8(_objRoutines[best].state);
		if (state == on.applied) continue;
		on.applied = state;
		if (on.mob) {
			++_stats.mobRoutineSwitches;
		} else {
			queueTrigger(on.name, state == 0);
			++_stats.objectRoutineSwitches;
		}
	}
}

void Game::saveObjectRoutines(ByteSink& out) const {
	out.u32v(_objNameCount);
	for (u32 i = 0; i < _objNameCount; ++i) {
		const ObjName& n = _objNames[i];
		out.str(n.name);
		out.u16v(n.world);
		out.u8v(n.mob ? 1 : 0);
		out.u8v(u8(n.applied));
	}
	out.u32v(_objRoutineCount);
	for (u32 i = 0; i < _objRoutineCount; ++i) {
		out.u16v(_objRoutines[i].name);
		out.u16v(_objRoutines[i].minute);
		out.u8v(_objRoutines[i].state);
	}
}

bool Game::loadObjectRoutines(ByteSource& in) {
	_objNameCount = _objRoutineCount = 0;
	const u32 names = in.u32v();
	if (in.bad || names > MAX_OBJ_NAMES) return false;
	for (u32 i = 0; i < names && !in.bad; ++i) {
		ObjName& n = _objNames[_objNameCount++];
		if (!in.str(n.name, sizeof(n.name))) return false;
		n.hash = hashName(n.name);
		n.world = in.u16v();
		n.mob = in.u8v() != 0;
		n.applied = i8(in.u8v());
	}
	const u32 routines = in.u32v();
	if (in.bad || routines > MAX_OBJ_ROUTINES) return false;
	for (u32 i = 0; i < routines && !in.bad; ++i) {
		ObjRoutine r;
		r.name = in.u16v();
		r.minute = in.u16v();
		r.state = in.u8v();
		if (r.name < _objNameCount) _objRoutines[_objRoutineCount++] = r;
	}
	return !in.bad;
}

// ---- in the saved game ----

void Game::saveNews(ByteSink& out) const {
	// The people by their keys (instance, how many of it came before), as the characters are saved.
	const auto key = [&](ByteSink& o, int who) {
		if (who < 0 || u32(who) >= _npcCount) {
			o.u32v(SCRIPT_NONE);
			o.u32v(0);
			return;
		}
		u32 ordinal = 0;
		for (int k = 0; k < who; ++k) ordinal += _npcs[k].symbol == _npcs[who].symbol ? 1u : 0u;
		o.u32v(_npcs[who].symbol);
		o.u32v(ordinal);
	};
	out.u32v(_newsNext);
	out.u32v(_newsCount);
	for (u32 i = 0; i < _newsCount; ++i) {
		const News& n = _news[i];
		out.u32v(n.number);
		key(out, int(n.holder));
		key(out, n.offender);
		key(out, n.victim);
		key(out, n.witness);
		out.i32v(n.id);
		out.i32v(n.victimGuild);
		out.u8v(n.gossip ? 1 : 0);
	}
}

bool Game::loadNews(ByteSource& in) {
	_newsCount = 0;
	const auto person = [&](ByteSource& r) -> int {
		const u32 symbol = r.u32v(), ordinal = r.u32v();
		if (symbol == SCRIPT_NONE) return -1;
		u32 seen = 0;
		for (u32 i = 0; i < _npcCount; ++i)
			if (_npcs[i].symbol == symbol && seen++ == ordinal) return int(i);
		return -2; // (not here: the piece is dropped)
	};
	_newsNext = in.u32v();
	const u32 count = in.u32v();
	if (in.bad || count > (1u << 16)) return false;
	for (u32 i = 0; i < count && !in.bad; ++i) {
		News n;
		n.number = in.u32v();
		const int holder = person(in);
		n.offender = person(in);
		n.victim = person(in);
		n.witness = person(in);
		n.id = in.i32v();
		n.victimGuild = in.i32v();
		n.gossip = in.u8v() != 0;
		if (holder < 0 || n.offender == -2 || n.victim == -2 || _newsCount == MAX_NEWS) continue;
		if (n.witness == -2) n.witness = -1;
		n.holder = u32(holder);
		_news[_newsCount++] = n;
	}
	return !in.bad;
}

} // namespace g3
