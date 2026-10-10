#include "npcs.h"

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
	loadFightAi(pak);
	_texts.load(pak); // (an archive baked without words simply has none: the lines are silent)
	_stats.total = _game.npcCount();
	return true;
}

void Npcs::release() {
	talkAbort();
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
	_pool.release();
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
	_clipBytes = 0;
	_stats.clipBytes = 0;
}

void Npcs::enterWorld(Pak& pak, const World& world, const char* asset, Vec3 eye, float hours) {
	if (!loaded()) return;
	leaveWorld();
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

	_pak = &pak;
	_world = &world;
	_game.enterWorld(_script, hours, findPoint, this);
	_hours = hours;
	_stats.total = _game.npcCount();
	_reach = min(_cfg.range, world.config().drawDistance);
	// Whoever is around the player is there when the world appears.
	++_frame;
	select(pak, world, eye, 0xFFFFu);
	_pak = nullptr;
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
			if (_frame - p.failedAt < 240 || loadsLeft == 0) return nullptr;
			--loadsLeft;
			p.failed = !p.model.loadEntry(pak, hash, &_pool, true);
			p.failedAt = _frame;
			++_stats.loads;
			return p.failed ? nullptr : &p;
		}
		// Room: an unused slot, else the one that was needed longest ago.
		if (p.used == _frame) continue;
		if (!spare || (p.hash == 0 && spare->hash != 0) || (p.hash != 0 && spare->hash != 0 && p.used < spare->used)) spare = &p;
	}
	if (!spare || loadsLeft == 0) return nullptr;
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
	spare->failed = !spare->model.loadEntry(pak, hash, &_pool, true);
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
	_stats.reach = _reach;
	for (u32 i = 0; i < _game.npcCount(); ++i) {
		const GameNpc& n = _game.npc(i);
		if (n.hero || n.world != _game.currentWorld() || !n.placed) continue;
		++_stats.here;
		const float dx = n.pos[0] - eye.x, dy = n.pos[1] - eye.y, dz = n.pos[2] - eye.z;
		const float d2 = dx * dx + dy * dy + dz * dz;
		if (d2 > keep2) continue;
		const bool isActor = n.active || actorOf(i) != nullptr;
		if (d2 > range2 && !isActor) continue;
		++_stats.near;
		// (The dead come last: the living need the places more.)
		const float k = (isActor ? d2 * 0.7f : d2) * (n.down == DOWN_DEAD ? 4.0f : 1.0f);
		u32 at = count;
		while (at > 0 && key[at - 1] > k) --at;
		if (at >= want) continue;
		if (count < want) ++count;
		for (u32 j = count - 1; j > at; --j) {
			chosen[j] = chosen[j - 1];
			key[j] = key[j - 1];
			dist2[j] = dist2[j - 1];
		}
		chosen[at] = i;
		key[at] = k;
		dist2[at] = d2;
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
		if (!still) retire(a);
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
		Piece* body = piece(pak, look->body, loadsLeft);
		Piece* head = look->head ? piece(pak, look->head, loadsLeft) : nullptr;
		// Pieces that do not fit together are as good as missing.
		if (rig && body && !body->model.sameSkeleton(rig->model)) body = nullptr;
		if (rig && head && !head->model.sameSkeleton(rig->model)) head = nullptr;
		if (rig != a->rig) {
			a->rig = rig;
			a->anim.bind(rig ? &rig->model : nullptr);
			a->idle = nullptr;
			if (rig) wake(*a);
		}
		a->body = body;
		a->head = head;
		a->ready = rig && body;
		if (!a->ready) ++_stats.waiting;
		// Put somewhere else by its routine (nobody lives while `alive` is off).
		if (a->movedSeen != n.moved) {
			a->movedSeen = n.moved;
			a->grounded = false;
		}
		if (!a->grounded) ground(*a, world, 150.0f);
	}
	u32 models = 0;
	for (const Piece& p : _pieces) models += p.hash != 0 && !p.failed;
	_stats.models = models;
	_stats.textures = _pool.count();
	_stats.textureBytes = _pool.bytes();
	_stats.clipBytes = _clipBytes;
}

void Npcs::update(Pak& pak, const World& world, Vec3 eye, float hours, float dt) {
	if (!loaded()) return;
	_pak = &pak;
	_world = &world;
	_clock += dt;
	++_frame;
	_stats.scriptMs = 0;
	// No further than the world is drawn, and under load no further than the
	// small things of the world: they go first, then come back.
	const StreamConfig& sc = world.config();
	_reach = min(_cfg.range * clamp(sc.detailScale, 0.4f, 1.0f), sc.drawDistance);
	// The clock has moved on by a minute or more: so may the routines have.
	float hoursPassed = 0;
	if (fabsf(hours - _hours) >= 1.0f / 60.0f) {
		hoursPassed = hours - _hours;
		if (hoursPassed < 0.0f) hoursPassed += 24.0f; // over midnight
		if (_hours < 0.0f || hoursPassed > 23.0f) hoursPassed = 0; // (set back by a minute, not on by a day)
		_game.setTime(hours, findPoint, this);
		_hours = hours;
	}
	const float feet[3] = {eye.x, eye.y - 170.0f, eye.z};
	_game.setHeroPlace(feet, _heroYaw);
	select(pak, world, eye, _cfg.loadsPerFrame);
	u32 actors = 0, walking = 0, looking = 0, fighting = 0;
	for (NpcActor& a : _actors) {
		if (!a.used || !a.ready) continue;
		tick(a, world, dt, hoursPassed);
		++actors;
		walking += a.phase == NpcActor::MOVING;
		looking += a.lookAt >= 0;
		fighting += _game.npc(a.npc).weapon != WEAPON_NONE;
	}
	tickTalk(dt);
	_stats.looking = looking;
	_stats.fighting = fighting;
	u32 inUse = 0;
	for (const NpcActor& a : _actors) inUse += a.used && a.mob >= 0 && a.mobStage >= NpcActor::MOB_ENTERING;
	_stats.mobsInUse = inUse;
	_stats.actors = actors;
	_stats.walking = walking;
	_stats.clipBytes = _clipBytes;
	_pak = nullptr;
	_world = nullptr;
}

void Npcs::draw(const Mat4& viewProj, const float dayLight[3]) {
	_stats.shown = _stats.culled = _stats.triangles = _stats.drawCalls = 0;
	if (!loaded()) return;
	const Frustum frustum = frustumFromViewProj(viewProj);
	for (NpcActor& a : _actors) {
		if (!a.used || !a.ready || !a.rig || !a.body) continue;
		const GameNpc& n = _game.npc(a.npc);
		const Vec3 feet = {n.pos[0], n.pos[1], n.pos[2]};
		const Model& body = a.body->model;
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
		body.skinMatrices(*pose, n.yaw, 0.0f, scale, skin);
		body.drawSkinned(skin, feet, light);
		_stats.triangles += body.triangles();
		_stats.drawCalls += body.partCount();
		if (a.head) {
			a.head->model.drawSkinned(skin, feet, light);
			_stats.triangles += a.head->model.triangles();
			_stats.drawCalls += a.head->model.partCount();
		}
		++_stats.shown;
	}
}

} // namespace g3
