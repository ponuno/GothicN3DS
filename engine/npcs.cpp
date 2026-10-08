#include "npcs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace g3 {

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
	_stats.total = _game.npcCount();
	return true;
}

void Npcs::release() {
	for (Piece& p : _pieces) {
		p.model.release();
		p.hash = 0;
		p.used = 0;
		p.failed = false;
	}
	_pool.release();
	_game.release();
	free(_looks);
	free(_points);
	free(_standing);
	_looks = nullptr;
	_points = nullptr;
	_standing = nullptr;
	_lookCount = _pointCount = _standingCount = 0;
	_shownCount = 0;
	_script[0] = 0;
	_stats = NpcStats {};
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

void Npcs::leaveWorld() {
	for (Piece& p : _pieces) {
		p.model.release();
		p.hash = 0;
		p.used = 0;
		p.failed = false;
	}
	_shownCount = 0;
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

	_game.enterWorld(_script, hours, findPoint, this);
	_hours = hours;
	_stats.total = _game.npcCount();
	// Everybody may stand somewhere else now.
	if (_standingCount < _game.npcCount()) {
		Standing* grown = static_cast<Standing*>(realloc(static_cast<void*>(_standing), size_t(_game.npcCount()) * sizeof(Standing)));
		if (grown) {
			for (u32 i = _standingCount; i < _game.npcCount(); ++i) grown[i] = Standing {};
			_standing = grown;
			_standingCount = _game.npcCount();
		}
	}
	for (u32 i = 0; i < _standingCount; ++i) _standing[i].moved = 0xFFFFFFFFu;
	// Whoever is around the player is there when the world appears.
	++_frame;
	select(pak, world, eye, 0xFFFFu);
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
			p.failed = !p.model.loadEntry(pak, hash, &_pool);
			p.failedAt = _frame;
			++_stats.loads;
			return p.failed ? nullptr : &p;
		}
		// Room: an unused slot, else the one that was needed longest ago.
		if (p.used == _frame) continue;
		if (!spare || (p.hash == 0 && spare->hash != 0) || (p.hash != 0 && spare->hash != 0 && p.used < spare->used)) spare = &p;
	}
	if (!spare || loadsLeft == 0) return nullptr;
	--loadsLeft;
	spare->model.release();
	spare->hash = hash;
	spare->used = _frame;
	spare->failed = !spare->model.loadEntry(pak, hash, &_pool);
	spare->failedAt = _frame;
	++_stats.loads;
	return spare->failed ? nullptr : spare;
}

void Npcs::select(Pak& pak, const World& world, Vec3 eye, u32 loadsLeft) {
	const u32 want = min<u32>(_cfg.maxShown, SHOWN);
	const float range2 = _cfg.range * _cfg.range;
	_shownCount = 0;
	_stats.here = _stats.near = _stats.waiting = _stats.noModel = 0;
	for (u32 i = 0; i < _game.npcCount(); ++i) {
		const GameNpc& n = _game.npc(i);
		if (n.hero || n.world != _game.currentWorld() || !n.placed) continue;
		++_stats.here;
		const float dx = n.pos[0] - eye.x, dy = n.pos[1] - eye.y, dz = n.pos[2] - eye.z;
		const float d2 = dx * dx + dy * dy + dz * dz;
		if (d2 > range2) continue;
		++_stats.near;
		// Among the nearest? (A short list kept in order.)
		u32 at = _shownCount;
		while (at > 0 && _shown[at - 1].dist2 > d2) --at;
		if (at >= want) continue;
		if (_shownCount < want) ++_shownCount;
		for (u32 k = _shownCount - 1; k > at; --k) _shown[k] = _shown[k - 1];
		_shown[at] = Shown {i, d2, nullptr, nullptr, nullptr};
	}
	for (u32 k = 0; k < _shownCount; ++k) {
		Shown& s = _shown[k];
		const GameNpc& n = _game.npc(s.npc);
		const LookEntry* look = lookOf(n.look);
		if (!look) {
			++_stats.noModel;
			continue;
		}
		s.rig = piece(pak, look->rig, loadsLeft);
		s.body = piece(pak, look->body, loadsLeft);
		s.head = look->head ? piece(pak, look->head, loadsLeft) : nullptr;
		// Pieces that do not fit together are as good as missing.
		if (s.rig && s.body && !s.body->model.sameSkeleton(s.rig->model)) s.body = nullptr;
		if (s.rig && s.head && !s.head->model.sameSkeleton(s.rig->model)) s.head = nullptr;
		if (!s.rig || !s.body) ++_stats.waiting;

		// Where exactly it stands: on the ground there, in the ground's light.
		if (s.npc >= _standingCount) continue;
		Standing& st = _standing[s.npc];
		if (st.moved != n.moved) {
			st = Standing {};
			st.moved = n.moved;
			st.y = n.pos[1];
		}
		if (!st.grounded) {
			float y;
			Vec3 normal;
			u8 light[4];
			if (world.rayDown({n.pos[0], n.pos[1] + 150.0f, n.pos[2]}, 650.0f, y, normal, light)) {
				st.grounded = true;
				st.y = y;
				memcpy(st.light, light, sizeof(st.light));
			}
		}
	}
	u32 models = 0;
	for (const Piece& p : _pieces) models += p.hash != 0 && !p.failed;
	_stats.models = models;
	_stats.textures = _pool.count();
	_stats.textureBytes = _pool.bytes();
}

void Npcs::update(Pak& pak, const World& world, Vec3 eye, float hours, float dt) {
	if (!loaded()) return;
	_clock += dt;
	++_frame;
	// The clock has moved on by a minute or more: so may the routines have.
	if (fabsf(hours - _hours) >= 1.0f / 60.0f) {
		_game.setTime(hours, findPoint, this);
		_hours = hours;
	}
	select(pak, world, eye, _cfg.loadsPerFrame);
}

void Npcs::draw(const Mat4& viewProj, const float dayLight[3]) {
	_stats.shown = _stats.culled = _stats.triangles = _stats.drawCalls = 0;
	if (!loaded() || !_shownCount) return;
	const Frustum frustum = frustumFromViewProj(viewProj);
	static Pose pose; // 2.7 KB that are better not on the console's stack
	for (u32 k = 0; k < _shownCount; ++k) {
		const Shown& s = _shown[k];
		if (!s.rig || !s.body) continue;
		const GameNpc& n = _game.npc(s.npc);
		const Standing* st = s.npc < _standingCount ? &_standing[s.npc] : nullptr;
		const Vec3 feet = {n.pos[0], st ? st->y : n.pos[1], n.pos[2]};
		const Model& rig = s.rig->model;
		const Model& body = s.body->model;
		// Out of view? A box around the body where it stands, turned any way.
		const ModelHeader& bh = body.header();
		float reach = 0;
		for (int a = 0; a < 3; a += 2) reach = max(reach, max(fabsf(bh.bbMin[a]), fabsf(bh.bbMax[a])));
		const float grow = max(n.look.scale[0], max(n.look.scale[1], n.look.scale[2]));
		reach = reach * grow + 40.0f;
		const float mn[3] = {feet.x - reach, feet.y - 20.0f, feet.z - reach};
		const float mx[3] = {feet.x + reach, feet.y + (bh.bbMax[1] + 60.0f) * grow, feet.z + reach};
		if (!aabbInFrustum(frustum, mn, mx)) {
			++_stats.culled;
			continue;
		}
		// Standing about, everybody in a rhythm of his own.
		rig.restPose(pose);
		if (const Clip* idle = rig.clipAt(0)) idle->sample(_clock + float(s.npc % 97) * 0.37f, pose);
		float light[3];
		const u8* l = st ? st->light : nullptr;
		for (int i = 0; i < 3; ++i) {
			const float baked = l ? float(l[i]) * (1.0f / 255.0f) : 1.0f;
			const float sky = l ? float(l[3]) * (1.0f / 255.0f) : 1.0f;
			light[i] = max(baked * (1.0f + sky * (dayLight[i] - 1.0f)) * 1.15f, 0.22f);
		}
		const bool scaled = n.look.scale[0] != 1.0f || n.look.scale[1] != 1.0f || n.look.scale[2] != 1.0f;
		const float* scale = scaled ? n.look.scale : nullptr;
		body.draw(pose, feet, n.yaw, light, 0.0f, scale);
		_stats.triangles += body.triangles();
		_stats.drawCalls += body.partCount();
		if (s.head) {
			s.head->model.draw(pose, feet, n.yaw, light, 0.0f, scale);
			_stats.triangles += s.head->model.triangles();
			_stats.drawCalls += s.head->model.partCount();
		}
		++_stats.shown;
	}
}

} // namespace g3
