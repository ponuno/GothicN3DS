#include "loot.h"
#include "lamps.h"

#include "game.h"
#include "gfx.h"
#include "readahead.h"
#include "sys.h"
#include "world.h"

#include <stdlib.h>
#include <string.h>

namespace g3 {

void Loot::release() {
	leaveWorld();
	for (u32 i = 0; i < _lookCount; ++i) _looks[i].model.release();
	_lookCount = 0;
	_pool.release();
	free(_taken);
	_taken = nullptr;
	_takenCount = _takenCap = 0;
	free(_dropped);
	_dropped = nullptr;
	_droppedCount = _droppedCap = 0;
	_nextDrop = 1;
}

void Loot::leaveWorld() {
	free(_items);
	_items = nullptr;
	_count = _base = _cap = 0;
	_grid.release();
	_world = 0;
	_stats = LootStats();
	// (The looks stay: the next world has many of the same.)
}

bool Loot::wasTaken(u32 world, u32 index) const {
	for (u32 i = 0; i < _takenCount; ++i)
		if (_taken[i].world == world && _taken[i].index == index) return true;
	return false;
}

void Loot::markTaken(u32 world, u32 index) {
	if (wasTaken(world, index)) return;
	if (_takenCount >= _takenCap) {
		const u32 cap = _takenCap ? _takenCap * 2 : 64;
		Taken* grown = static_cast<Taken*>(realloc(static_cast<void*>(_taken), sizeof(Taken) * cap));
		if (!grown) return;
		_taken = grown;
		_takenCap = cap;
	}
	_taken[_takenCount++] = Taken {world, index};
}

bool Loot::grow(u32 count) {
	if (count <= _cap) return true;
	const u32 cap = max(count, _cap + _cap / 4 + 16);
	LootItem* grown = static_cast<LootItem*>(realloc(static_cast<void*>(_items), sizeof(LootItem) * cap));
	if (!grown) return false;
	memset(static_cast<void*>(grown + _cap), 0, sizeof(LootItem) * (cap - _cap));
	_items = grown;
	_cap = cap;
	return true;
}

void Loot::enterWorld(Pak& pak, const char* worldName, u32 worldHash, Game& game) {
	leaveWorld();
	_world = worldHash;
	_game = &game;
	char name[96];
	snprintf(name, sizeof(name), "%s/items", worldName ? worldName : "");
	const PakEntry* e = pak.find(name);
	u8* blob = e && e->size >= sizeof(ItemsHeader) ? static_cast<u8*>(malloc(e->size)) : nullptr;
	ItemsHeader h {};
	if (blob && pak.read(*e, blob)) memcpy(&h, blob, sizeof(h));
	if (!blob || h.magic != ITEMS_MAGIC || h.version != ITEMS_VERSION || e->size != sizeof(ItemsHeader) + size_t(h.count) * sizeof(WorldItem) ||
	    h.count == 0 || !grow(h.count)) {
		// (No things of its own: those the player dropped here all the same.)
		free(blob);
		placeDropped();
		return;
	}
	_count = _base = h.count;
	for (u32 i = 0; i < _count; ++i) {
		WorldItem wi;
		memcpy(&wi, blob + sizeof(ItemsHeader) + size_t(i) * sizeof(WorldItem), sizeof(wi));
		LootItem& it = _items[i];
		const u32 s = game.vm().findHash(wi.instance);
		it.symbol = s != SCRIPT_NONE && game.isItem(s) ? s : SCRIPT_NONE;
		it.pos = {wi.pos[0], wi.pos[1], wi.pos[2]};
		it.rot = quatNormalize({wi.rot[0], wi.rot[1], wi.rot[2], wi.rot[3]});
		it.amount = wi.amount > 0 ? wi.amount : 1;
		it.flags = wi.flags;
		it.model = NOT_TRIED;
		it.taken = it.symbol == SCRIPT_NONE || wasTaken(_world, i);
		it.lit = false;
		it.wait = 0;
		it.drop = 0;
		_stats.taken += it.taken;
	}
	free(blob);
	placeDropped();
}

void Loot::placeDropped() {
	_count = _base;
	_stats.taken = 0;
	for (u32 i = 0; i < _base; ++i) _stats.taken += _items[i].taken;
	u32 here = 0;
	for (u32 k = 0; k < _droppedCount; ++k) here += _dropped[k].world == _world;
	if (here && _game && grow(_base + here))
		for (u32 k = 0; k < _droppedCount; ++k) {
			const Dropped& d = _dropped[k];
			if (d.world != _world) continue;
			const u32 s = _game->vm().findHash(d.item);
			if (s == SCRIPT_NONE || !_game->isItem(s)) continue; // (a data.pak whose scripts lack it)
			LootItem& it = _items[_count++];
			memset(static_cast<void*>(&it), 0, sizeof(it));
			it.symbol = s;
			it.pos = {d.pos[0], d.pos[1], d.pos[2]};
			it.rot = quatNormalize({d.rot[0], d.rot[1], d.rot[2], d.rot[3]});
			it.amount = d.amount > 0 ? d.amount : 1;
			it.model = NOT_TRIED;
			it.drop = d.id;
		}
	_stats.count = _count;
	_grid.release();
	if (_count) _grid.build(&_items[0].pos.x, &_items[0].pos.z, _count, u32(sizeof(LootItem) / sizeof(float)));
}

int Loot::drop(Game& game, u32 symbol, i32 amount, Vec3 pos, Quat rot) {
	if (!_world || symbol == SCRIPT_NONE || symbol >= game.vm().symbolCount() || amount <= 0) return -1;
	_game = &game;
	if (_droppedCount >= _droppedCap) {
		const u32 cap = _droppedCap ? _droppedCap * 2 : 32;
		Dropped* grown = static_cast<Dropped*>(realloc(static_cast<void*>(_dropped), sizeof(Dropped) * cap));
		if (!grown) return -1;
		_dropped = grown;
		_droppedCap = cap;
	}
	if (!grow(_count + 1)) return -1;
	const Quat q = quatNormalize(rot);
	Dropped& d = _dropped[_droppedCount++];
	d.world = _world;
	d.id = _nextDrop++;
	if (!_nextDrop) _nextDrop = 1;
	d.item = game.vm().symbol(symbol).nameHash;
	d.amount = amount;
	d.pos[0] = pos.x;
	d.pos[1] = pos.y;
	d.pos[2] = pos.z;
	d.rot[0] = q.x;
	d.rot[1] = q.y;
	d.rot[2] = q.z;
	d.rot[3] = q.w;
	LootItem& it = _items[_count];
	memset(static_cast<void*>(&it), 0, sizeof(it));
	it.symbol = symbol;
	it.pos = pos;
	it.rot = q;
	it.amount = amount;
	it.model = NOT_TRIED;
	it.drop = d.id;
	++_count;
	_stats.count = _count;
	// (Where it lies: the squares again - a drop is rare, a few thousand points sort in no time.)
	_grid.release();
	_grid.build(&_items[0].pos.x, &_items[0].pos.z, _count, u32(sizeof(LootItem) / sizeof(float)));
	return int(_count - 1);
}

void Loot::update(Pak& pak, const World& world, Game& game, Vec3 eye) {
	bool loadedOne = false;
	++_updateNo;
	u32 rays = 0; // floors looked for this frame: a few, so that a crowd of things coming into reach is not met in one frame
	_stats.noModel = 0;
	_stats.rayMs = _stats.loadMs = 0.0f;
	auto each = [&](u32 i) {
		LootItem& it = _items[i];
		if (it.taken) return;
		const float dx = it.pos.x - eye.x, dz = it.pos.z - eye.z;
		if (dx * dx + dz * dz > 4000.0f * 4000.0f) return;
		if (!it.lit && rays < 6) {
			++rays;
			// Lit like the floor it lies on, once that is there.
			float y;
			Vec3 n;
			u8 l[4];
			const double tRay = sys::timeMs();
			const bool floorFound = world.rayDown({it.pos.x, it.pos.y + 60.0f, it.pos.z}, 400.0f, y, n, l);
			_stats.rayMs += float(sys::timeMs() - tRay);
			if (floorFound) {
				// (How much of the sky the floor sees: the time of day's share, applied when drawn.)
				it.sky = float(l[3]) * (1.0f / 255.0f);
				for (int c = 0; c < 3; ++c) it.light[c] = max(float(l[c]) * (1.0f / 255.0f) * 1.15f, 0.25f);
				it.lit = true;
			}
		}
		if (it.model == NOT_TRIED && !loadedOne) {
			const GameItemInfo* info = game.itemInfo(it.symbol);
			const GameText visual = info ? info->visual : GameText(0);
			it.model = NO_MODEL;
			if (!visual) return;
			for (u32 k = 0; k < _lookCount; ++k)
				if (_looks[k].visual == visual) it.model = u16(_looks[k].model.loaded() ? k : NO_MODEL);
			if (it.model != NO_MODEL) return;
			bool known = false;
			for (u32 k = 0; k < _lookCount; ++k) known = known || _looks[k].visual == visual;
			if (known || _lookCount >= LOOKS) return;
			Ask* ask = nullptr;
			for (Ask& a : _asks)
				if (a.entry && a.visual == u32(visual)) ask = &a;
			if (!ask) {
				char name[96];
				snprintf(name, sizeof(name), "model/item.%s", game.text(visual));
				ask = &_asks[_askNext++ % ASKS];
				*ask = Ask();
				ask->visual = u32(visual);
				ask->entry = hashName(name);
			}
			const u32 entry = ask->entry;
			if (_ahead && _ahead->running() && _ahead->enabled() && it.wait < 120) {
				// Wished from the read-ahead's thread; built here when it is there (the frame never waits for the card).
				if (ask->frame != _updateNo) {
					ask->frame = _updateNo;
					_ahead->request(entry);
					ask->have = _ahead->has(entry);
					ask->idle = _ahead->idle();
				}
				// (And what the model drags after it - its textures - has been read too, or has had its time.)
				if (!ask->have || (it.wait < 40 && !ask->idle)) {
					++it.wait;
					it.model = NOT_TRIED;
					return;
				}
			}
			Look& look = _looks[_lookCount];
			look.visual = visual;
			loadedOne = true;
			const double tBuild = sys::timeMs();
			const bool ok = look.model.loadEntry(pak, entry, &_pool) && look.model.hasMesh();
			_stats.loadMs += float(sys::timeMs() - tBuild);
			if (!ok) look.model.release();
			it.model = ok ? u16(_lookCount) : NO_MODEL;
			++_lookCount;
		}
		_stats.noModel += it.model == NO_MODEL;
	};
	_grid.forNear(eye.x, eye.z, 4000.0f, _count, each);
	_stats.models = 0;
	for (u32 k = 0; k < _lookCount; ++k) _stats.models += _looks[k].model.loaded();
}

void Loot::draw(Vec3 eye, const float dayLight[3], float range, const Frustum* view, float minPixels, float focalPx) {
	_stats.drawn = _stats.drawCalls = _stats.triangles = 0;
	auto each = [&](u32 i) {
		const LootItem& it = _items[i];
		if (it.taken || !it.lit || it.model >= _lookCount) return;
		const Vec3 d = it.pos - eye;
		const float d2 = dot(d, d);
		if (d2 > range * range) return;
		const Model& m = _looks[it.model].model;
		if (!m.loaded()) return;
		// Out of view, or too small to be seen from here (a ring, a coin: a pixel at a few metres).
		const ModelHeader& mh = m.header();
		float r = 0.0f;
		for (int a = 0; a < 3; ++a) r = max(r, max(fabsf(mh.bbMin[a]), fabsf(mh.bbMax[a])));
		r = max(r, 5.0f);
		if (minPixels > 0.0f && 2.0f * r * focalPx < minPixels * sqrtf(d2)) return;
		if (view) {
			const float mn[3] = {it.pos.x - r, it.pos.y - r, it.pos.z - r}, mx[3] = {it.pos.x + r, it.pos.y + r, it.pos.z + r};
			if (!aabbInFrustum(*view, mn, mx)) return;
		}
		float light[3];
		for (int c = 0; c < 3; ++c) light[c] = max(it.light[c] * (1.0f + it.sky * (dayLight[c] - 1.0f)), 0.22f);
		if (_lamps) _lamps->lightAt({it.pos.x, it.pos.y + 15.0f, it.pos.z}, it.sky, light, nullptr); // (fix148; no look past walls for a thing)
		const Mat34 skin = mat34From(it.rot, {0, 0, 0});
		m.drawSkinned(&skin, it.pos, light);
		++_stats.drawn;
		_stats.triangles += m.triangles();
		for (u32 p = 0; p < m.partCount(); ++p) _stats.drawCalls += m.part(p).indexCount != 0;
	};
	_grid.forNear(eye.x, eye.z, range, _count, each);
}

const Model* Loot::lookModel(Pak& pak, Game& game, u32 symbol) {
	const GameItemInfo* info = game.itemInfo(symbol);
	if (!info || !info->visual) return nullptr;
	int found = -1;
	for (u32 k = 0; k < _lookCount; ++k)
		if (_looks[k].visual == info->visual) found = int(k);
	if (found < 0) {
		if (_lookCount >= LOOKS) return nullptr;
		char name[96];
		snprintf(name, sizeof(name), "model/item.%s", game.text(info->visual));
		Look& look = _looks[_lookCount];
		look.visual = info->visual;
		const bool ok = look.model.loadEntry(pak, hashName(name), &_pool) && look.model.hasMesh();
		if (!ok) look.model.release();
		found = int(_lookCount++);
	}
	const Model& m = _looks[found].model;
	return m.loaded() ? &m : nullptr;
}

void Loot::drawHeld(Pak& pak, Game& game, u32 symbol, const Mat34& attach, Vec3 origin, const float light[3]) {
	if (const Model* m = lookModel(pak, game, symbol)) m->drawSkinned(&attach, origin, light);
}

void Loot::drawCarried(Pak& pak, Game& game, const Model& host, const Pose& pose, float yaw, float lean, const float* scale,
                       u8 weapon, u32 melee, Vec3 origin, const float light[3], Vec3* blade) {
	if (melee == 0xFFFFFFFFu) return;
	const GameItemInfo* info = game.itemInfo(melee);
	if (!info) return;
	const i32 twoHanded = game.constant("ITEM_2HD_SWD", 1 << 16) | game.constant("ITEM_2HD_AXE", 1 << 17);
	const bool two = (info->flags & twoHanded) != 0;
	static const u32 hand = hashName("ZS_RIGHTHAND"), belt = hashName("ZS_SWORD"), back = hashName("ZS_LONGSWORD");
	const bool inHand = weapon == 2 || weapon == 3; // WEAPON_1H, WEAPON_2H
	static const u32 rightHand = hashName("BIP01 R HAND"), pelvis = hashName("BIP01 PELVIS"), spine = hashName("BIP01 SPINE1");
	// (A skeleton without the game's attachment nodes: the hand, the hip, the back.)
	int node = host.findNode(inHand ? hand : (two ? back : belt));
	if (node < 0) node = host.findNode(inHand ? rightHand : (two ? spine : pelvis));
	Mat34 at;
	if (!host.nodeMatrix(pose, node, yaw, lean, scale, at)) return;
	drawHeld(pak, game, melee, at, origin, light);
	if (blade && inHand) { // fix216: the far end of the blade, in the world
		if (const Model* m = lookModel(pak, game, melee)) {
			const ModelHeader& mh = m->header();
			int a = 0;
			for (int k = 1; k < 3; ++k)
				if (mh.bbMax[k] - mh.bbMin[k] > mh.bbMax[a] - mh.bbMin[a]) a = k;
			float p[3];
			for (int k = 0; k < 3; ++k) p[k] = (mh.bbMin[k] + mh.bbMax[k]) * 0.5f;
			p[a] = fabsf(mh.bbMax[a]) >= fabsf(mh.bbMin[a]) ? mh.bbMax[a] : mh.bbMin[a];
			*blade = origin + transformPoint(at, {p[0], p[1], p[2]});
		}
	}
}

void Loot::drawRanged(Pak& pak, Game& game, const Model& host, const Pose& pose, float yaw, float lean, const float* scale,
                      u8 weapon, u32 ranged, Vec3 origin, const float light[3]) {
	if (ranged == 0xFFFFFFFFu || !game.itemInfo(ranged)) return;
	static const u32 hand = hashName("ZS_LEFTHAND"), back = hashName("ZS_BOW"), leftHand = hashName("BIP01 L HAND"),
	                 spine = hashName("BIP01 SPINE1");
	const bool inHand = weapon == 4 || weapon == 5; // WEAPON_BOW, WEAPON_CBOW
	int node = host.findNode(inHand ? hand : back);
	if (node < 0) node = host.findNode(inHand ? leftHand : spine);
	Mat34 at;
	if (!host.nodeMatrix(pose, node, yaw, lean, scale, at)) return;
	drawHeld(pak, game, ranged, at, origin, light);
}

bool Loot::drawInHand(Pak& pak, Game& game, const Model& host, const Pose& pose, float yaw, const float* scale, u32 symbol, Vec3 origin,
                      const float light[3], bool left, float lean, Vec3* tip) {
	if (symbol == 0xFFFFFFFFu) return false;
	static const u32 hand = hashName("ZS_RIGHTHAND"), rightHand = hashName("BIP01 R HAND");
	static const u32 handL = hashName("ZS_LEFTHAND"), leftHand = hashName("BIP01 L HAND");
	int node = host.findNode(left ? handL : hand);
	if (node < 0) node = host.findNode(left ? leftHand : rightHand);
	Mat34 at;
	if (node < 0 || !host.nodeMatrix(pose, node, yaw, lean, scale, at)) {
		// fix213: a skeleton without the hand: the flame still burns where a hand at the side would hold it, not over the head.
		if (tip) {
			const float side = left ? -1.0f : 1.0f;
			*tip = origin + Vec3 {cosf(yaw) * 30.0f * side + sinf(yaw) * 25.0f, 105.0f, -sinf(yaw) * 30.0f * side + cosf(yaw) * 25.0f};
		}
		return false;
	}
	const Model* m = lookModel(pak, game, symbol);
	if (!m) {
		if (tip) *tip = origin + Vec3 {at.m[0][3], at.m[1][3] + 25.0f, at.m[2][3]}; // (no model: above the hand)
		return false;
	}
	m->drawSkinned(&at, origin, light);
	if (tip) {
		// The far end of its longest side (the hand holds the other).
		const ModelHeader& mh = m->header();
		int a = 0;
		for (int k = 1; k < 3; ++k)
			if (mh.bbMax[k] - mh.bbMin[k] > mh.bbMax[a] - mh.bbMin[a]) a = k;
		float p[3];
		for (int k = 0; k < 3; ++k) p[k] = (mh.bbMin[k] + mh.bbMax[k]) * 0.5f;
		p[a] = fabsf(mh.bbMax[a]) >= fabsf(mh.bbMin[a]) ? mh.bbMax[a] : mh.bbMin[a];
		*tip = origin + transformPoint(at, {p[0], p[1], p[2]});
	}
	return true;
}

int Loot::nearest(Vec3 feet, float yaw, float reach, float halfAngle) const {
	int best = -1;
	float bestScore = 1e30f;
	auto each = [&](u32 i) {
		const LootItem& it = _items[i];
		if (it.taken) return;
		const float dx = it.pos.x - feet.x, dz = it.pos.z - feet.z, dy = it.pos.y - feet.y;
		const float d2 = dx * dx + dz * dz;
		if (d2 > reach * reach || dy < -80.0f || dy > 160.0f) return;
		float off = atan2f(dx, dz) - yaw;
		if (!(fabsf(off) < 1.0e4f)) off = 0.0f; // (fix134: an angle gone to infinity would loop here for ever)
		while (off > 3.14159265f) off -= 6.2831853f;
		while (off < -3.14159265f) off += 6.2831853f;
		if (d2 > 40.0f * 40.0f && fabsf(off) > halfAngle) return;
		// The nearest, and of two as near the one more ahead (of equal ones the first, as before).
		const float score = sqrtf(d2) + 60.0f * fabsf(off);
		if (score < bestScore || (score == bestScore && int(i) < best)) {
			bestScore = score;
			best = int(i);
		}
	};
	_grid.forNear(feet.x, feet.z, reach, _count, each);
	return best;
}

bool Loot::take(u32 index) {
	if (index >= _count || _items[index].taken) return false;
	_items[index].taken = true;
	++_stats.taken;
	if (const u32 id = _items[index].drop) {
		// (A dropped thing is gone from the list; its place in _items stays, taken, so the indices hold.)
		for (u32 k = 0; k < _droppedCount; ++k)
			if (_dropped[k].id == id && _dropped[k].world == _world) {
				_dropped[k] = _dropped[--_droppedCount];
				break;
			}
		return true;
	}
	markTaken(_world, index);
	return true;
}

// ---- state.bin ----

namespace {
// 'G3L2' (fix192): the taken list, then the dropped things. ('G3LT', before: the taken list alone.)
constexpr u32 TAKEN_MAGIC = fourcc('G', '3', 'L', '2'), TAKEN_MAGIC_1 = fourcc('G', '3', 'L', 'T');
}

void Loot::saveTaken(FILE* f) const {
	if (!f) return;
	const u32 head[3] = {TAKEN_MAGIC, _takenCount, _droppedCount};
	u32 check = 0x9E3779B9u;
	const auto add = [&](const void* p, size_t n) {
		const u8* b = static_cast<const u8*>(p);
		for (size_t k = 0; k < n; ++k) check = (check ^ b[k]) * 16777619u;
		fwrite(p, n, 1, f);
	};
	add(head, sizeof(head));
	if (_takenCount) add(_taken, sizeof(Taken) * _takenCount);
	if (_droppedCount) add(_dropped, sizeof(Dropped) * _droppedCount);
	fwrite(&check, sizeof(check), 1, f);
}

bool Loot::loadTaken(FILE* f) {
	u32 head[3] = {};
	if (!f || fread(head, sizeof(u32) * 2, 1, f) != 1 || (head[0] != TAKEN_MAGIC && head[0] != TAKEN_MAGIC_1) || head[1] > (1u << 20)) return false;
	const bool withDropped = head[0] == TAKEN_MAGIC;
	if (withDropped && (fread(&head[2], sizeof(u32), 1, f) != 1 || head[2] > (1u << 16))) return false;
	Taken* list = static_cast<Taken*>(malloc(sizeof(Taken) * (head[1] ? head[1] : 1u)));
	Dropped* dropped = static_cast<Dropped*>(malloc(sizeof(Dropped) * (head[2] ? head[2] : 1u)));
	if (!list || !dropped) {
		free(list);
		free(dropped);
		return false;
	}
	u32 check = 0x9E3779B9u;
	const auto sum = [&](const void* p, size_t n) {
		const u8* b = static_cast<const u8*>(p);
		for (size_t k = 0; k < n; ++k) check = (check ^ b[k]) * 16777619u;
	};
	sum(head, sizeof(u32) * (withDropped ? 3 : 2));
	u32 stored = 0;
	const bool ok = (head[1] == 0 || fread(list, sizeof(Taken) * head[1], 1, f) == 1) && (sum(list, sizeof(Taken) * head[1]), true) &&
	                (head[2] == 0 || fread(dropped, sizeof(Dropped) * head[2], 1, f) == 1) && (sum(dropped, sizeof(Dropped) * head[2]), true) &&
	                fread(&stored, sizeof(stored), 1, f) == 1 && stored == check;
	if (!ok) {
		free(list);
		free(dropped);
		return false;
	}
	free(_taken);
	_taken = list;
	_takenCount = _takenCap = head[1];
	if (!_takenCap) {
		free(_taken);
		_taken = nullptr;
	}
	free(_dropped);
	_dropped = dropped;
	_droppedCount = _droppedCap = head[2];
	_nextDrop = 1;
	for (u32 k = 0; k < _droppedCount; ++k) _nextDrop = max(_nextDrop, _dropped[k].id + 1);
	if (!_droppedCap) {
		free(_dropped);
		_dropped = nullptr;
	}
	// This world as the list says: what is taken is away, the rest is back; and what was dropped here lies here.
	for (u32 i = 0; i < _base; ++i) _items[i].taken = _items[i].symbol == SCRIPT_NONE || wasTaken(_world, i);
	placeDropped();
	return true;
}

} // namespace g3
