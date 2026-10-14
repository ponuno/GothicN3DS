#include "npcs.h"

#include "readahead.h"
#include "loot.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace g3 {

Npcs::Npcs() {}

bool Npcs::load(Pak& pak) {
	release();
	if (!_game.load(pak)) return false;
	if (const PakEntry* e = pak.find("meta/looks")) {
		u8* blob = e->size >= sizeof(LooksHeader) ? static_cast<u8*>(pak.readAlloc(*e)) : nullptr;
		LooksHeader h {};
		if (blob) memcpy(&h, blob, sizeof(h));
		if (blob && h.magic == LOOKS_MAGIC && h.version == LOOKS_VERSION && h.count < (1u << 20) &&
		    e->size == sizeof(LooksHeader) + size_t(h.count) * sizeof(LookEntry)) {
			_looks = static_cast<LookEntry*>(malloc(size_t(h.count) * sizeof(LookEntry) + 1));
			if (_looks) {
				memcpy(_looks, blob + sizeof(h), size_t(h.count) * sizeof(LookEntry));
				_lookCount = h.count;
				// Found by bisection: the table must be in order.
				for (u32 i = 1; i < _lookCount; ++i)
					if (_looks[i - 1].key >= _looks[i].key) _lookCount = 0;
			}
		}
		free(blob);
	}
	// The states' names, for reports (an archive baked before they were kept has none).
	if (const PakEntry* e = pak.find("script/states")) {
		char* names = e->size > 0 && e->size < (1u << 20) ? static_cast<char*>(pak.readAlloc(*e)) : nullptr;
		if (names && names[e->size - 1] == 0) {
			_stateNames = names;
			_stateNamesSize = e->size;
		} else {
			free(names);
		}
	}
	bindAi();
	bindDialog();
	bindWorld();
	loadFightAi(pak);
	_texts.load(pak); // (an archive baked without words simply has none: the lines are silent)
	_stats.total = _game.npcCount();
	if (_game.heroIndex() != SCRIPT_NONE) _game.clearTalkFlag(_game.heroIndex()); // (no talk is going on yet)
	return true;
}

void Npcs::release() {
	talkAbort();
	freeNearLists();
	_texts.release();
	_speechHead = _speechCount = 0;
	_speechNext = _speechDone = 1;
	_say = Saying();
	_sayText[0] = 0;
	_talk = Talk();
	_talkView = TalkView();
	for (NpcActor& a : _actors) a = NpcActor();
	for (Piece& p : _pieces) {
		p.model.release();
		p.hash = 0;
		p.used = 0;
		p.failed = false;
	}
	for (u32 k = 0; k < _overlayCount; ++k) {
		_overlayModels[k].model.release();
		_overlayModels[k].hash = 0;
		_overlayModels[k].failed = false;
	}
	_overlayCount = 0;
	_pool.release();
	_meshes.release();
	_net.release();
	_mobs.release();
	_game.release();
	free(_looks);
	free(_points);
	free(_stateNames);
	_looks = nullptr;
	_points = nullptr;
	_stateNames = nullptr;
	_stateNamesSize = 0;
	_lookCount = _pointCount = 0;
	_script[0] = 0;
	_stats = NpcStats {};
	_clipBytes = 0;
	_pak = nullptr;
	_frame = 0;
	_clock = 0;
	_hours = -1.0f;
}

bool Npcs::findPoint(void* self, const char* name, float pos[3], float& yaw) {
	const Npcs& n = *static_cast<const Npcs*>(self);
	const u32 h = hashName(name);
	u32 lo = 0, hi = n._pointCount;
	while (lo < hi) {
		const u32 mid = (lo + hi) / 2;
		if (n._points[mid].nameHash < h)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo >= n._pointCount || n._points[lo].nameHash != h) return false;
	memcpy(pos, n._points[lo].pos, 3 * sizeof(float));
	yaw = n._points[lo].yaw;
	return true;
}

const LookEntry* Npcs::lookOf(const GameLook& look) const {
	const u32 key = _game.lookKey(look);
	u32 lo = 0, hi = _lookCount;
	while (lo < hi) {
		const u32 mid = (lo + hi) / 2;
		if (_looks[mid].key < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < _lookCount && _looks[lo].key == key ? &_looks[lo] : nullptr;
}

const NpcActor* Npcs::actorOf(u32 npcIndex) const {
	for (const NpcActor& a : _actors)
		if (a.used && a.npc == npcIndex) return &a;
	return nullptr;
}

// An actor goes back to being wherever its routine wants it.
void Npcs::retire(NpcActor& a) {
	if (!a.used) return;
	_net.drop(a.npc);
	_mobs.drop(a.npc);
	if (_game.loaded() && a.npc < _game.npcCount()) _game.settle(a.npc, findPoint, this);
	a = NpcActor();
}

void Npcs::leaveWorld() {
	talkAbort();
	if (_game.heroIndex() != SCRIPT_NONE) _game.clearTalkFlag(_game.heroIndex());
	flushSpeech();
	for (NpcActor& a : _actors) retire(a);
	_net.dropAll();
	_mobs.dropAll();
	for (Piece& p : _pieces) {
		p.model.release();
		p.hash = 0;
		p.used = 0;
		p.failed = false;
	}
	_meshes.release();
	_clipBytes = 0;
	_stats.clipBytes = 0;
}

// The meshes most of the world's people wear ("meta/hotmeshes", most worn first) are read now and kept: whoever comes
// into range afterwards wears one of them from memory, and only the textures have to be read.
void Npcs::warmMeshes(Pak& pak) {
	const PakEntry* e = pak.find("meta/hotmeshes");
	if (!e || e->size < sizeof(HotMeshHeader) || e->size > (1u << 20)) return;
	u8* blob = static_cast<u8*>(pak.readAlloc(*e));
	if (!blob) return;
	HotMeshHeader h;
	memcpy(&h, blob, sizeof(h));
	if (h.magic == HOTMESH_MAGIC && e->size == sizeof(h) + size_t(h.count) * sizeof(u32)) {
		const double t0 = sys::timeMs();
		u32 warmed = 0;
		for (u32 i = 0; i < h.count; ++i) {
			u32 hash;
			memcpy(&hash, blob + sizeof(h) + size_t(i) * sizeof(u32), sizeof(hash));
			warmed += _meshes.warm(pak, hash);
		}
		_stats.meshesWarmed = warmed;
		_stats.warmMs = float(sys::timeMs() - t0);
	}
	free(blob);
}

void Npcs::enterWorld(Pak& pak, const World& world, const char* asset, Vec3 eye, float hours) {
	if (!loaded()) return;
	leaveWorld();
	warmMeshes(pak);
	free(_points);
	_points = nullptr;
	_pointCount = 0;
	_script[0] = 0;

	char name[96];
	snprintf(name, sizeof(name), "%s/points", asset);
	const PakEntry* e = pak.find(name);
	u8* blob = e && e->size >= sizeof(PointsHeader) ? static_cast<u8*>(pak.readAlloc(*e)) : nullptr;
	PointsHeader h {};
	if (blob) memcpy(&h, blob, sizeof(h));
	if (blob && h.magic == POINTS_MAGIC && h.version == POINTS_VERSION && h.count < (1u << 20) &&
	    e->size == sizeof(PointsHeader) + size_t(h.count) * sizeof(WorldPoint)) {
		_points = static_cast<WorldPoint*>(malloc(size_t(h.count) * sizeof(WorldPoint) + 1));
		if (_points) {
			memcpy(_points, blob + sizeof(h), size_t(h.count) * sizeof(WorldPoint));
			_pointCount = h.count;
			for (u32 i = 1; i < _pointCount; ++i)
				if (_points[i - 1].nameHash > _points[i].nameHash) _pointCount = 0;
		}
		memcpy(_script, h.script, sizeof(_script));
		_script[sizeof(_script) - 1] = 0;
	}
	free(blob);
	if (!_script[0]) {
		// An archive without the table: the world's own name will have to do.
		const char* slash = strrchr(asset, '/');
		snprintf(_script, sizeof(_script), "%s", slash ? slash + 1 : asset);
	}
	_net.load(pak, asset); // a world without paths: its people stand
	_mobs.load(pak, asset); // ... and without things to use: they do without
	_rooms.load(pak, asset); // ... and without rooms: no house is anybody's
	_heroMob = -1;
	_game.clearHeroRoom();

	_pak = &pak;
	_world = &world;
	_game.enterWorld(_script, hours, findPoint, this);
	_hours = hours;
	_stats.total = _game.npcCount();
	_reach = min(_cfg.range, world.config().drawDistance);
	// Whoever is around the player is there when the world appears.
	++_frame;
	select(pak, world, eye, 0xFFFFu);
	// (The archive stays known: animations are read outside update() too - a blow
	// of the player's that kills somebody needs its fall at once. Without it the
	// fall was missing until the next load, which read S_DEAD.)
	_world = nullptr;
}

// A model by its archive entry, read now if `loadsLeft` allows. Null if it
// is not in memory (yet) or cannot be used.
Npcs::Piece* Npcs::piece(Pak& pak, u32 hash, u32& loadsLeft) {
	if (!hash) return nullptr;
	Piece* spare = nullptr;
	for (Piece& p : _pieces) {
		if (p.hash == hash) {
			p.used = _frame;
			if (!p.failed) return &p;
			// Memory may have been short for a moment: another go every few seconds.
			if (_frame - p.failedAt < 240 || loadsLeft == 0 || !loadFits()) return nullptr;
			--loadsLeft;
			const double tRetry = sys::timeMs();
			p.failed = !p.model.loadEntry(pak, hash, &_pool, true, &_meshes);
			_stats.loadMs += float(sys::timeMs() - tRetry);
			p.failedAt = _frame;
			++_stats.loads;
			return p.failed ? nullptr : &p;
		}
		// Room: an unused slot, else the one that was needed longest ago.
		if (p.used == _frame) continue;
		if (!spare || (p.hash == 0 && spare->hash != 0) || (p.hash != 0 && spare->hash != 0 && p.used < spare->used)) spare = &p;
	}
	if (!spare || loadsLeft == 0 || !loadFits()) return nullptr;
	// (Nobody is drawn with it any more - but an actor waiting for another of
	// its models may still be bound to it.)
	for (NpcActor& a : _actors)
		if (a.used && (a.rig == spare || a.body == spare || a.head == spare)) {
			if (a.rig == spare) {
				a.anim.bind(nullptr);
				a.idle = nullptr;
				a.ready = false;
			}
			a.rig = a.rig == spare ? nullptr : a.rig;
			a.body = a.body == spare ? nullptr : a.body;
			a.head = a.head == spare ? nullptr : a.head;
		}
	--loadsLeft;
	_clipBytes -= min(_clipBytes, spare->model.clipBytes());
	spare->model.release();
	spare->hash = hash;
	spare->used = _frame;
	const double tLoad = sys::timeMs();
	spare->failed = !spare->model.loadEntry(pak, hash, &_pool, true, &_meshes);
	const float loadedMs = float(sys::timeMs() - tLoad);
	_pieceCostMs = _pieceCostMs * 0.8f + loadedMs * 0.2f;
	_stats.loadMs += loadedMs;
	spare->failedAt = _frame;
	++_stats.loads;
	return spare->failed ? nullptr : spare;
}

void Npcs::ground(NpcActor& a, const World& world, float above) {
	GameNpc& n = _game.npcMut(a.npc);
	float y;
	Vec3 normal;
	u8 light[4];
	if (!world.rayDown({n.pos[0], n.pos[1] + above, n.pos[2]}, above + 500.0f, y, normal, light)) return;
	n.pos[1] = y;
	memcpy(a.light, light, sizeof(a.light));
	a.grounded = true;
	if (_rooms.count()) _game.setRoom(a.npc, _rooms.hash(roomOf(n.pos)));
}

// The candidate lists of select() for `npcs` characters; false if there is no memory for them.
bool Npcs::nearListFits(u32 npcs) {
	if (npcs <= _nearCap && _nearList) return true;
	freeNearLists();
	const u32 cap = npcs + 64;
	_nearList = static_cast<u32*>(malloc(size_t(cap) * sizeof(u32)));
	_sweepList = static_cast<u32*>(malloc(size_t(cap) * sizeof(u32)));
	_seen = static_cast<u32*>(calloc(cap, sizeof(u32)));
	if (!_nearList || !_sweepList || !_seen) {
		freeNearLists();
		return false;
	}
	_nearCap = cap;
	_nearCount = _sweepCount = _sweepPos = 0;
	_nearNpcs = 0;
	_nearWorld = 0xFFFF; // (made anew at the next select)
	return true;
}

void Npcs::freeNearLists() {
	free(_nearList);
	free(_sweepList);
	free(_seen);
	_nearList = _sweepList = _seen = nullptr;
	_nearCap = _nearCount = _sweepCount = _sweepPos = 0;
	_nearWorld = 0xFFFF;
}

void Npcs::select(Pak& pak, const World& world, Vec3 eye, u32 loadsLeft) {
	const u32 want = min<u32>(_cfg.maxShown, ACTORS);
	const float range2 = _reach * _reach, keep2 = (_reach + 300.0f) * (_reach + 300.0f);
	// The nearest: a short list kept in order. Who is an actor already stays
	// one a little longer, so that nobody flickers at the edge.
	u32 chosen[ACTORS];
	float key[ACTORS], dist2[ACTORS];
	u32 count = 0;
	_stats.here = _stats.near = _stats.waiting = _stats.noModel = 0;
	_stats.loadMs = _stats.wakeMs = _stats.groundMs = 0.0f;
	_stats.reach = _reach;
	u32 startsLeft = _cfg.startsPerFrame;
	// Those about to come into range, nearest first: their models are read ahead (see below).
	const float prefetch2 = (_reach + 300.0f + _cfg.prefetchRange) * (_reach + 300.0f + _cfg.prefetchRange);
	enum { AHEAD = 8 };
	u32 ahead[AHEAD];
	float aheadKey[AHEAD];
	u32 aheadCount = 0;
	// (fix87) Only the candidates - those who were within reach of a sweep and a margin - and the actors, not all
	// of the game's characters every frame (Archolos: 1980, 2.4 ms a frame on the console).
	const u32 total = _game.npcCount();
	const float sweepReach = _reach + 300.0f + _cfg.prefetchRange + kNearMargin, sweep2 = sweepReach * sweepReach;
	const bool fresh = nearListFits(total);
	auto qualifies = [&](u32 i, Vec3 from) {
		const GameNpc& n = _game.npc(i);
		if (n.hero || n.world != _game.currentWorld() || !n.placed) return false;
		const float dx = n.pos[0] - from.x, dy = n.pos[1] - from.y, dz = n.pos[2] - from.z;
		return n.active || dx * dx + dy * dy + dz * dz <= sweep2;
	};
	auto here = [&](u32 i) {
		const GameNpc& n = _game.npc(i);
		return !n.hero && n.world == _game.currentWorld() && n.placed;
	};
	{
		const float mx = eye.x - _nearEye.x, my = eye.y - _nearEye.y, mz = eye.z - _nearEye.z;
		const bool moved = mx * mx + my * my + mz * mz > kNearMargin * kNearMargin * 0.25f;
		if (fresh && (_nearWorld != _game.currentWorld() || moved || (_nearCount == 0 && _nearNpcs == 0))) {
			// Made anew at once: all of them.
			_nearCount = _nearHere = 0;
			for (u32 i = 0; i < total; ++i) {
				_nearHere += here(i);
				if (qualifies(i, eye)) _nearList[_nearCount++] = i;
			}
			_nearWorld = _game.currentWorld();
			_nearEye = eye;
			_nearNpcs = total;
			_sweepPos = _sweepCount = _sweepHere = 0;
			_sweepEye = eye;
		} else if (fresh) {
			// A slice of the sweep: all of them in about half a second.
			const u32 slice = total / 32 + 1;
			if (_sweepPos == 0) {
				_sweepEye = eye;
				_sweepCount = _sweepHere = 0;
			}
			for (u32 k = 0; k < slice && _sweepPos < total; ++k, ++_sweepPos) {
				_sweepHere += here(_sweepPos);
				if (qualifies(_sweepPos, _sweepEye)) _sweepList[_sweepCount++] = _sweepPos;
			}
			if (_sweepPos >= total) {
				u32* t = _nearList;
				_nearList = _sweepList;
				_sweepList = t;
				_nearCount = _sweepCount;
				_nearHere = _sweepHere;
				_nearEye = _sweepEye;
				_nearNpcs = total;
				_sweepPos = 0;
			}
		}
	}
	++_selectStamp;
	auto consider = [&](u32 i) {
		const GameNpc& n = _game.npc(i);
		if (n.hero || n.world != _game.currentWorld() || !n.placed) return;
		const float dx = n.pos[0] - eye.x, dy = n.pos[1] - eye.y, dz = n.pos[2] - eye.z;
		const float d2 = dx * dx + dy * dy + dz * dz;
		const bool isActor = n.active || actorOf(i) != nullptr;
		if ((d2 > keep2 || (d2 > range2 && !isActor)) && !isActor) {
			if (d2 <= prefetch2 && n.down != DOWN_DEAD && _cfg.prefetchRange > 0.0f) {
				u32 at = aheadCount;
				while (at > 0 && aheadKey[at - 1] > d2) --at;
				if (at < AHEAD) {
					if (aheadCount < AHEAD) ++aheadCount;
					for (u32 j = aheadCount - 1; j > at; --j) {
						ahead[j] = ahead[j - 1];
						aheadKey[j] = aheadKey[j - 1];
					}
					ahead[at] = i;
					aheadKey[at] = d2;
				}
			}
			return;
		}
		if (d2 > keep2) return;
		++_stats.near;
		// (The dead come last: the living need the places more.)
		const float k = (isActor ? d2 * 0.7f : d2) * (n.down == DOWN_DEAD ? 4.0f : 1.0f);
		u32 at = count;
		while (at > 0 && key[at - 1] > k) --at;
		if (at >= want) return;
		if (count < want) ++count;
		for (u32 j = count - 1; j > at; --j) {
			chosen[j] = chosen[j - 1];
			key[j] = key[j - 1];
			dist2[j] = dist2[j - 1];
		}
		chosen[at] = i;
		key[at] = k;
		dist2[at] = d2;
		};
	if (fresh) {
		for (u32 k = 0; k < _nearCount; ++k) {
			const u32 i = _nearList[k];
			if (i >= total || _seen[i] == _selectStamp) continue;
			_seen[i] = _selectStamp;
			consider(i);
		}
		// Those made since the list (Wld_InsertNpc, a summoning), and the actors whoever they are.
		for (u32 i = _nearNpcs; i < total; ++i)
			if (_seen[i] != _selectStamp) {
				_seen[i] = _selectStamp;
				consider(i);
			}
		for (const NpcActor& a : _actors)
			if (a.used && a.npc < total && _seen[a.npc] != _selectStamp) {
				_seen[a.npc] = _selectStamp;
				consider(a.npc);
			}
		_stats.here = _nearHere;
	} else {
		// (No memory for the lists: everybody, as before.)
		for (u32 i = 0; i < total; ++i) {
			_stats.here += here(i);
			consider(i);
		}
	}
	// (What the actors are made of stays in memory while this goes on.)
	for (NpcActor& a : _actors)
		if (a.used) {
			Piece* const parts[3] = {a.rig, a.body, a.head};
			for (Piece* p : parts)
				if (p) p->used = _frame;
		}
	// Who is no longer among them goes back to his routine's place ...
	for (NpcActor& a : _actors) {
		if (!a.used) continue;
		bool still = false;
		for (u32 k = 0; k < count && !still; ++k) still = chosen[k] == a.npc;
		a.leaving = !still && a.leaving;
		if (still) continue;
		// (One that is seen dissolves out first, and goes when it is gone.)
		if (a.ready && a.shown > 0 && _fadeStep < 255 && gfx::fadeSupported())
			a.leaving = true;
		else
			retire(a);
	}
	// ... and who is new becomes an actor.
	for (u32 k = 0; k < count; ++k) {
		NpcActor* a = nullptr;
		for (NpcActor& x : _actors)
			if (x.used && x.npc == chosen[k]) a = &x;
		if (!a) {
			for (NpcActor& x : _actors)
				if (!x.used && !a) a = &x;
			if (!a) continue;
			*a = NpcActor();
			a->used = true;
			a->npc = chosen[k];
			a->movedSeen = _game.npc(chosen[k]).moved;
		}
		a->dist2 = dist2[k];
		GameNpc& n = _game.npcMut(a->npc);
		n.active = _cfg.alive;
		const LookEntry* look = lookOf(n.look);
		if (!look) {
			++_stats.noModel;
			a->ready = false;
			continue;
		}
		Piece* rig = piece(pak, look->rig, loadsLeft);
		// A look with a coarse body: from afar only that is read; the full body and head come as the player nears,
		// and the coarse one stands in until they are there.
		Piece* low = nullptr;
		bool wantFull = true;
		if (look->bodyLow && _cfg.lodDistance > 0.0f) {
			const float d = _cfg.lodDistance;
			const float edge = a->far ? d * 0.9f : d * 1.1f;
			a->far = dist2[k] > edge * edge;
			const float load = d + 600.0f;
			low = piece(pak, look->bodyLow, loadsLeft);
			wantFull = !a->far || dist2[k] < load * load;
		} else {
			a->far = false;
		}
		Piece* body = wantFull || !low ? piece(pak, look->body, loadsLeft) : nullptr;
		Piece* head = look->head && body ? piece(pak, look->head, loadsLeft) : nullptr;
		// Pieces that do not fit together are as good as missing.
		if (rig && body && !body->model.sameSkeleton(rig->model)) body = nullptr;
		if (rig && low && !low->model.sameSkeleton(rig->model)) low = nullptr;
		if (rig && head && !head->model.sameSkeleton(rig->model)) head = nullptr;
		// One new character set going per frame: his first steps run scripts, and several in one frame were a hitch.
		if (!a->ready && rig && (body || low)) {
			if (startsLeft == 0 && _cfg.startsPerFrame > 0) {
				++_stats.waiting;
				continue;
			}
			if (startsLeft > 0) --startsLeft;
		}
		if (rig != a->rig) {
			a->rig = rig;
			a->anim.bind(rig ? &rig->model : nullptr);
			a->idle = nullptr;
			if (rig) {
				const double tWake = sys::timeMs();
				wake(*a);
				_stats.wakeMs += float(sys::timeMs() - tWake);
			}
		}
		a->body = body;
		a->head = head;
		a->low = low;
		a->ready = rig && (body || low);
		if (!a->ready) ++_stats.waiting;
		// Put somewhere else by its routine (nobody lives while `alive` is off).
		if (a->movedSeen != n.moved) {
			a->movedSeen = n.moved;
			a->grounded = false;
		}
		if (!a->grounded) {
			const double tGround = sys::timeMs();
			ground(*a, world, 150.0f);
			_stats.groundMs += float(sys::timeMs() - tGround);
		}
	}
	// A frame with a load to spare reads the models of those who are about to come: by the time they are in range
	// they are in memory, and the frame they appear in is no longer the one with the archive read in it.
	// With a read-ahead (readahead.h) the card's part is done on its own thread: the wish goes there (it reads the
	// model, then its textures and the animations the game is known to use), and the frame only turns what is waiting
	// into a model - never reads from the card itself here.
	const bool reading = _ahead && _ahead->running() && _ahead->enabled();
	for (u32 k = 0; k < aheadCount; ++k) {
		const LookEntry* look = lookOf(_game.npc(ahead[k]).look);
		if (!look) continue;
		const bool coarse = look->bodyLow && _cfg.lodDistance > 0.0f;
		const u32 hashes[4] = {look->rig, coarse ? look->bodyLow : look->body, coarse ? 0 : look->head, 0};
		for (u32 h : hashes) {
			if (!h) continue;
			if (reading) {
				bool inMemory = false;
				for (const Piece& p : _pieces) inMemory = inMemory || (p.hash == h && !p.failed);
				if (inMemory) continue;
				_ahead->request(h);
				if (loadsLeft > 0 && _ahead->has(h)) piece(pak, h, loadsLeft);
			} else if (loadsLeft > 0) {
				piece(pak, h, loadsLeft);
			}
		}
	}
	u32 models = 0;
	for (const Piece& p : _pieces) models += p.hash != 0 && !p.failed;
	_stats.models = models;
	_stats.textures = _pool.count();
	_stats.textureBytes = _pool.bytes();
	_stats.meshes = _meshes.count();
	_stats.meshBytes = _meshes.bytes();
	_stats.meshHits = _meshes.hits();
	_stats.meshReads = _meshes.reads();
	_stats.clipBytes = _clipBytes;
}

void Npcs::update(Pak& pak, const World& world, Vec3 eye, float hours, float dt) {
	// Nobody talks to the player: nobody waits for him to finish (ZS_Attack waits while the flag is set).
	if (!talking() && _game.heroIndex() != SCRIPT_NONE) _game.clearTalkFlag(_game.heroIndex());
	_lastHitAge += dt;
	if (!loaded()) return;
	_pak = &pak;
	_world = &world;
	_clock += dt;
	++_frame;
	_stats.scriptMs = 0;
	const double tStart = sys::timeMs();
	// No further than the world is drawn. The reach is deliberately not tied to the
	// pacer's detail scale: people shrinking to a dozen metres under load would
	// appear right in front of the player.
	const StreamConfig& sc = world.config();
	_reach = min(_cfg.range * clamp(sc.detailScale, clamp(_cfg.reachFloor, 0.1f, 1.0f), 1.0f), sc.drawDistance);
	// The clock has moved on by a minute or more: so may the routines have.
	float hoursPassed = 0;
	if (fabsf(hours - _hours) >= 1.0f / 60.0f) {
		hoursPassed = hours - _hours;
		if (hoursPassed < 0.0f) hoursPassed += 24.0f; // over midnight
		if (_hours < 0.0f || hoursPassed > 23.0f) hoursPassed = 0; // (set back by a minute, not on by a day)
		_game.setTime(hours, findPoint, this);
		_hours = hours;
	}
	const double tClock = sys::timeMs();
	const float feet[3] = {eye.x, eye.y - 170.0f, eye.z};
	_game.setHeroPlace(feet, _heroYaw);
	_game.tickOverlays(dt);
	trackHeroRoom({feet[0], feet[1], feet[2]});
	select(pak, world, eye, _cfg.loadsPerFrame);
	const double tSelect = sys::timeMs();
	u32 actors = 0, walking = 0, looking = 0, fighting = 0;
	for (NpcActor& a : _actors) {
		if (!a.used || !a.ready) continue;
		float stepDt = dt;
		if (_cfg.farTickEvery > 1 && hoursPassed == 0.0f && a.dist2 > _cfg.farTickDistance * _cfg.farTickDistance &&
		    _game.npc(a.npc).weapon == WEAPON_NONE && !talking()) {
			a.tickDebt += dt;
			// (Not all on the same frame.)
			if ((_frame + a.npc) % _cfg.farTickEvery != 0) {
				++actors;
				continue;
			}
			stepDt = a.tickDebt;
		}
		a.tickDebt = 0.0f;
		tick(a, world, stepDt, hoursPassed);
		++actors;
		walking += a.phase == NpcActor::MOVING;
		looking += a.lookAt >= 0;
		fighting += _game.npc(a.npc).weapon != WEAPON_NONE;
	}
	tickTalk(dt);
	_stats.clockMs = float(tClock - tStart);
	_stats.selectMs = float(tSelect - tClock);
	_stats.tickMs = float(sys::timeMs() - tSelect);
	_stats.looking = looking;
	_stats.fighting = fighting;
	u32 inUse = 0;
	for (const NpcActor& a : _actors) inUse += a.used && a.mob >= 0 && a.mobStage >= NpcActor::MOB_ENTERING;
	_stats.mobsInUse = inUse;
	_stats.actors = actors;
	_stats.walking = walking;
	_stats.clipBytes = _clipBytes;
	_world = nullptr;
}

void Npcs::draw(const Mat4& viewProj, const float dayLight[3], const World* world, Vec3 eye) {
	_stats.shown = _stats.culled = _stats.triangles = _stats.drawCalls = 0;
	_stats.hidden = _stats.sightRays = 0;
	_stats.sightMs = 0;
	_drawnCount = 0;
	++_drawFrame;
	if (!loaded()) return;
	const bool lookBehindWalls = world && _cfg.hideOccluded;
	double sightTime = 0;
	const Frustum frustum = frustumFromViewProj(viewProj);
	for (NpcActor& a : _actors) {
		if (!a.used) continue;
		// Dissolving in and out, whether or not it is in view.
		if (_fadeStep >= 255 || !gfx::fadeSupported())
			a.shown = a.leaving ? 0 : 255;
		else if (a.leaving)
			a.shown = u8(max(int(a.shown) - _fadeStep, 0));
		else
			a.shown = u8(min(int(a.shown) + _fadeStep, 255));
	}
	for (NpcActor& a : _actors) {
		if (!a.used || !a.ready || !a.rig || !(a.body || a.low) || a.shown == 0) continue;
		const GameNpc& n = _game.npc(a.npc);
		// Full body or coarse one: the full one once it is there and the player is near; one dissolves into the other.
		const int target = a.body && (!a.far || !a.low) ? 255 : 0;
		if (!a.low) a.mix = 255;
		else if (_fadeStep >= 255 || !gfx::fadeSupported()) a.mix = u8(target);
		else if (a.mix < target) a.mix = u8(min(int(a.mix) + _fadeStep, target));
		else if (a.mix > target) a.mix = u8(max(int(a.mix) - _fadeStep, target));
		if (!a.body) a.mix = 0;
		const NpcPiece* mainPiece = a.body ? a.body : a.low;
		const Vec3 feet = {n.pos[0], n.pos[1], n.pos[2]};
		const Model& body = mainPiece->model;
		// Out of view? A box around the body where it stands, turned any way.
		const ModelHeader& bh = body.header();
		float reach = 0;
		for (int k = 0; k < 3; k += 2) reach = max(reach, max(fabsf(bh.bbMin[k]), fabsf(bh.bbMax[k])));
		const float grow = max(n.look.scale[0], max(n.look.scale[1], n.look.scale[2]));
		reach = reach * grow + 40.0f;
		const float mn[3] = {feet.x - reach, feet.y - 20.0f, feet.z - reach};
		const float mx[3] = {feet.x + reach, feet.y + (bh.bbMax[1] + 60.0f) * grow, feet.z + reach};
		if (!aabbInFrustum(frustum, mn, mx)) {
			++_stats.culled;
			continue;
		}
		// Behind a wall? Thin lines from the eye to the head, the chest, its sides and the knees: hidden if every
		// one of them meets something solid on the way. One seen is looked at again only every few frames (being
		// drawn a little longer costs little); one hidden, every frame (coming out from behind a corner it shows at once).
		if (lookBehindWalls) {
			const float hx = feet.x - eye.x, hz = feet.z - eye.z;
			const float flat2 = hx * hx + hz * hz;
			if (flat2 < _cfg.hiddenNear * _cfg.hiddenNear) {
				a.occluded = false;
			} else if (a.occluded || a.occlusionFrame == 0 || _drawFrame - a.occlusionFrame >= max<u32>(_cfg.hiddenRecheck, 1)) {
				const double t0 = sys::timeMs();
				const float top = max(bh.bbMax[1], 60.0f) * grow;
				const float flat = sqrtf(flat2);
				const float side = max(a.bodyRadius, 25.0f);
				const Vec3 across = {-hz / flat * side, 0.0f, hx / flat * side};
				const Vec3 chest = {feet.x, feet.y + top * 0.6f, feet.z};
				const Vec3 points[5] = {chest, {feet.x, feet.y + top * 0.9f, feet.z}, chest + across, chest - across,
				                        {feet.x, feet.y + top * 0.25f, feet.z}};
				bool seen = false;
				for (int k = 0; k < 5 && !seen; ++k) {
					++_stats.sightRays;
					seen = !world->sightBlocked(eye, points[k]);
				}
				a.occluded = !seen;
				a.occlusionFrame = _drawFrame;
				sightTime += sys::timeMs() - t0;
			}
			if (a.occluded) {
				++_stats.hidden;
				// (The pose is still worked out, as for one drawn: what asks for it - the report, the tests - sees it move on.)
				a.anim.evaluate();
				// Still somebody standing there (the triggers ask who is where), only not drawn, nor its shadow.
				if (n.down == DOWN_NONE && a.shown > 127 && _drawnCount < ACTORS) {
					_drawnFeet[_drawnCount] = feet;
					_drawnRadius[_drawnCount] = clamp((reach - 40.0f) * 0.9f, 38.0f, 160.0f);
					_drawnHidden[_drawnCount] = true;
					++_drawnCount;
				}
				continue;
			}
		}
		a.anim.evaluate();
		float light[3];
		for (int i = 0; i < 3; ++i) {
			const float baked = float(a.light[i]) * (1.0f / 255.0f);
			const float sky = float(a.light[3]) * (1.0f / 255.0f);
			light[i] = max(baked * (1.0f + sky * (dayLight[i] - 1.0f)) * 1.15f, 0.22f);
		}
		const bool scaled = n.look.scale[0] != 1.0f || n.look.scale[1] != 1.0f || n.look.scale[2] != 1.0f;
		const float* scale = scaled ? n.look.scale : nullptr;
		// Body and head are on one skeleton: its bone matrices serve both.
		static Mat34 skin[MODEL_MAX_NODES];
		const Pose* pose = &a.anim.pose();
		static Pose turned;
		if (fabsf(a.headYaw) > 0.01f || fabsf(a.headPitch) > 0.01f) {
			const int head = a.rig->model.findNode(hashName("BIP01 HEAD"));
			if (head > 0) {
				turned = *pose;
				a.rig->model.turnNode(turned, head, a.headYaw, a.headPitch);
				pose = &turned;
			}
		}
		body.skinMatrices(*pose, n.yaw, a.noLying ? a.slump * 1.5f : 0.0f, scale, skin);
		if (a.mix > 0 && a.body) {
			gfx::setFade(float(int(a.shown) * int(a.mix) / 255) * (1.0f / 255.0f));
			a.body->model.drawSkinned(skin, feet, light);
			_stats.triangles += a.body->model.triangles();
			_stats.drawCalls += a.body->model.partCount();
		}
		if (a.mix < 255 && a.low) {
			gfx::setFade(float(int(a.shown) * (255 - int(a.mix)) / 255) * (1.0f / 255.0f));
			a.low->model.drawSkinned(skin, feet, light);
			_stats.triangles += a.low->model.triangles();
			_stats.drawCalls += a.low->model.partCount();
		}
		gfx::setFade(float(a.shown) * (1.0f / 255.0f));
		if (a.mix > 127) {
			if (_heldLoot && _heldPak && n.down == DOWN_NONE)
				_heldLoot->drawCarried(*_heldPak, _game, a.rig->model, *pose, n.yaw, 0.0f, scale, n.weapon, n.melee, feet, light);
			if (_heldLoot && _heldPak && a.heldItem != 0xFFFFFFFFu && a.itemState >= 0)
				_heldLoot->drawInHand(*_heldPak, _game, a.rig->model, *pose, n.yaw, scale, a.heldItem, feet, light);
		}
		if (a.head && a.mix > 0) {
			gfx::setFade(float(int(a.shown) * int(a.mix) / 255) * (1.0f / 255.0f));
			a.head->model.drawSkinned(skin, feet, light);
			_stats.triangles += a.head->model.triangles();
			_stats.drawCalls += a.head->model.partCount();
		}
		++_stats.shown;
		if (n.down == DOWN_NONE && a.shown > 127 && _drawnCount < ACTORS) {
			_drawnFeet[_drawnCount] = feet;
			_drawnRadius[_drawnCount] = clamp((reach - 40.0f) * 0.9f, 38.0f, 160.0f);
			_drawnHidden[_drawnCount] = false;
			// (The body's own width: the rest pose's arms take a share of the box.)
			a.bodyRadius = clamp((reach - 40.0f) * 0.55f, 22.0f, 90.0f);
			++_drawnCount;
		}
	}
	gfx::setFade(1.0f);
	_stats.sightMs = float(sightTime);
}

} // namespace g3
