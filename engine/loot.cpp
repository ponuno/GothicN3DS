#include "loot.h"

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
}

void Loot::leaveWorld() {
	free(_items);
	_items = nullptr;
	_count = 0;
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

void Loot::enterWorld(Pak& pak, const char* worldName, u32 worldHash, Game& game) {
	leaveWorld();
	_world = worldHash;
	char name[96];
	snprintf(name, sizeof(name), "%s/items", worldName ? worldName : "");
	const PakEntry* e = pak.find(name);
	if (!e || e->size < sizeof(ItemsHeader)) return;
	u8* blob = static_cast<u8*>(malloc(e->size));
	if (!blob) return;
	ItemsHeader h {};
	if (pak.read(*e, blob)) memcpy(&h, blob, sizeof(h));
	if (h.magic != ITEMS_MAGIC || h.version != ITEMS_VERSION || e->size != sizeof(ItemsHeader) + size_t(h.count) * sizeof(WorldItem) ||
	    h.count == 0) {
		free(blob);
		return;
	}
	_items = static_cast<LootItem*>(calloc(h.count, sizeof(LootItem)));
	if (!_items) {
		free(blob);
		return;
	}
	_count = h.count;
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
		_stats.taken += it.taken;
	}
	_stats.count = _count;
	free(blob);
	_grid.build(&_items[0].pos.x, &_items[0].pos.z, _count, u32(sizeof(LootItem) / sizeof(float)));
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
		const Mat34 skin = mat34From(it.rot, {0, 0, 0});
		m.drawSkinned(&skin, it.pos, light);
		++_stats.drawn;
		_stats.triangles += m.triangles();
		for (u32 p = 0; p < m.partCount(); ++p) _stats.drawCalls += m.part(p).indexCount != 0;
	};
	_grid.forNear(eye.x, eye.z, range, _count, each);
}

void Loot::drawHeld(Pak& pak, Game& game, u32 symbol, const Mat34& attach, Vec3 origin, const float light[3]) {
	const GameItemInfo* info = game.itemInfo(symbol);
	if (!info || !info->visual) return;
	int found = -1;
	for (u32 k = 0; k < _lookCount; ++k)
		if (_looks[k].visual == info->visual) found = int(k);
	if (found < 0) {
		if (_lookCount >= LOOKS) return;
		char name[96];
		snprintf(name, sizeof(name), "model/item.%s", game.text(info->visual));
		Look& look = _looks[_lookCount];
		look.visual = info->visual;
		const bool ok = look.model.loadEntry(pak, hashName(name), &_pool) && look.model.hasMesh();
		if (!ok) look.model.release();
		found = int(_lookCount++);
	}
	const Model& m = _looks[found].model;
	if (!m.loaded()) return;
	m.drawSkinned(&attach, origin, light);
}

void Loot::drawCarried(Pak& pak, Game& game, const Model& host, const Pose& pose, float yaw, float lean, const float* scale,
                       u8 weapon, u32 melee, Vec3 origin, const float light[3]) {
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

void Loot::drawInHand(Pak& pak, Game& game, const Model& host, const Pose& pose, float yaw, const float* scale, u32 symbol, Vec3 origin,
                      const float light[3]) {
	if (symbol == 0xFFFFFFFFu) return;
	static const u32 hand = hashName("ZS_RIGHTHAND"), rightHand = hashName("BIP01 R HAND");
	int node = host.findNode(hand);
	if (node < 0) node = host.findNode(rightHand);
	Mat34 at;
	if (node < 0 || !host.nodeMatrix(pose, node, yaw, 0.0f, scale, at)) return;
	drawHeld(pak, game, symbol, at, origin, light);
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
	markTaken(_world, index);
	return true;
}

// ---- state.bin ----

namespace {
constexpr u32 TAKEN_MAGIC = fourcc('G', '3', 'L', 'T');
}

void Loot::saveTaken(FILE* f) const {
	if (!f) return;
	const u32 head[2] = {TAKEN_MAGIC, _takenCount};
	u32 check = 0x9E3779B9u;
	const auto add = [&](const void* p, size_t n) {
		const u8* b = static_cast<const u8*>(p);
		for (size_t k = 0; k < n; ++k) check = (check ^ b[k]) * 16777619u;
		fwrite(p, n, 1, f);
	};
	add(head, sizeof(head));
	if (_takenCount) add(_taken, sizeof(Taken) * _takenCount);
	fwrite(&check, sizeof(check), 1, f);
}

bool Loot::loadTaken(FILE* f) {
	u32 head[2] = {};
	if (!f || fread(head, sizeof(head), 1, f) != 1 || head[0] != TAKEN_MAGIC || head[1] > (1u << 20)) return false;
	Taken* list = static_cast<Taken*>(malloc(sizeof(Taken) * (head[1] ? head[1] : 1u)));
	if (!list) return false;
	u32 check = 0x9E3779B9u;
	const auto sum = [&](const void* p, size_t n) {
		const u8* b = static_cast<const u8*>(p);
		for (size_t k = 0; k < n; ++k) check = (check ^ b[k]) * 16777619u;
	};
	sum(head, sizeof(head));
	u32 stored = 0;
	const bool ok = (head[1] == 0 || fread(list, sizeof(Taken) * head[1], 1, f) == 1) && (sum(list, sizeof(Taken) * head[1]), true) &&
	                fread(&stored, sizeof(stored), 1, f) == 1 && stored == check;
	if (!ok) {
		free(list);
		return false;
	}
	free(_taken);
	_taken = list;
	_takenCount = _takenCap = head[1];
	if (!_takenCap) {
		free(_taken);
		_taken = nullptr;
	}
	// This world as the list says: what is taken is away, the rest is back.
	_stats.taken = 0;
	for (u32 i = 0; i < _count; ++i) {
		_items[i].taken = _items[i].symbol == SCRIPT_NONE || wasTaken(_world, i);
		_stats.taken += _items[i].taken;
	}
	return true;
}

} // namespace g3
