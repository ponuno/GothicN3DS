#include "chests.h"

#include "game.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace g3 {

namespace {

constexpr u32 CHANGED_MAGIC = fourcc('G', '3', 'C', 'S');

char upperChar(char c) { return c >= 'a' && c <= 'z' ? char(c - 32) : c; }

} // namespace

void Chests::release() {
	leaveWorld();
	free(_records);
	_records = nullptr;
	_recordCount = _recordCap = 0;
}

void Chests::leaveWorld() {
	if (_chests) flush();
	free(_chests);
	_chests = nullptr;
	_count = 0;
	_world = 0;
}

Chests::Record* Chests::recordFor(u32 world, u32 index, bool create) const {
	for (u32 i = 0; i < _recordCount; ++i)
		if (_records[i].world == world && _records[i].index == index) return &_records[i];
	if (!create) return nullptr;
	if (_recordCount >= _recordCap) {
		const u32 cap = _recordCap ? _recordCap * 2 : 16;
		Record* grown = static_cast<Record*>(realloc(static_cast<void*>(_records), sizeof(Record) * cap));
		if (!grown) return nullptr;
		_records = grown;
		_recordCap = cap;
	}
	Record* r = &_records[_recordCount++];
	memset(r, 0, sizeof(*r));
	r->world = world;
	r->index = index;
	return r;
}

void Chests::flush() const {
	for (u32 i = 0; i < _count; ++i) {
		const Chest& c = _chests[i];
		if (!c.changed) continue;
		Record* r = recordFor(_world, i, true);
		if (!r) continue;
		r->flags = c.locked ? 0u : 1u;
		r->n = c.n;
		memcpy(r->stacks, c.stacks, sizeof(r->stacks));
	}
}

void Chests::apply() {
	for (u32 k = 0; k < _recordCount; ++k) {
		const Record& r = _records[k];
		if (r.world != _world || r.index >= _count || r.n > MAX_STACKS) continue;
		Chest& c = _chests[r.index];
		c.changed = true;
		if (r.flags & 1u) c.locked = false;
		c.n = u8(r.n);
		memcpy(c.stacks, r.stacks, sizeof(c.stacks));
	}
}

void Chests::enterWorld(Pak& pak, const char* worldName, u32 worldHash, Game& game) {
	leaveWorld();
	_world = worldHash;
	char name[96];
	snprintf(name, sizeof(name), "%s/chests", worldName ? worldName : "");
	const PakEntry* e = pak.find(name);
	if (!e || e->size < sizeof(ChestsHeader)) return;
	u8* blob = static_cast<u8*>(malloc(e->size));
	if (!blob) return;
	ChestsHeader h {};
	if (pak.read(*e, blob)) memcpy(&h, blob, sizeof(h));
	const size_t tableBytes = sizeof(ChestsHeader) + size_t(h.count) * sizeof(WorldChest);
	if (h.magic != CHESTS_MAGIC || h.version != CHESTS_VERSION || h.count == 0 || h.count > 65535 ||
	    e->size != tableBytes + h.nameBytes || h.nameBytes == 0 || blob[e->size - 1] != 0) {
		free(blob);
		return;
	}
	_chests = static_cast<Chest*>(calloc(h.count, sizeof(Chest)));
	if (!_chests) {
		free(blob);
		return;
	}
	_count = h.count;
	const char* names = reinterpret_cast<const char*>(blob + tableBytes);
	for (u32 i = 0; i < _count; ++i) {
		WorldChest wc;
		memcpy(&wc, blob + sizeof(ChestsHeader) + size_t(i) * sizeof(WorldChest), sizeof(wc));
		Chest& c = _chests[i];
		c.mob = wc.mob;
		c.locked = (wc.flags & CHEST_LOCKED) != 0;
		c.key = SCRIPT_NONE;
		if (wc.key) {
			const u32 s = game.vm().findHash(wc.key);
			if (s != SCRIPT_NONE && game.isItem(s)) c.key = s;
		}
		if (wc.pick < h.nameBytes) {
			// Only the letters of a combination.
			size_t n = 0;
			for (const char* p = names + wc.pick; *p && n + 1 < sizeof(c.pick); ++p)
				if (upperChar(*p) == 'L' || upperChar(*p) == 'R') c.pick[n++] = upperChar(*p);
			c.pick[n] = 0;
		}
		// "ITMI_GOLD:75, ITFO_FISH:2, ITMI_QUARTZ"
		if (wc.contents < h.nameBytes) {
			const char* p = names + wc.contents;
			while (*p && c.n < MAX_STACKS) {
				while (*p == ' ' || *p == ',') ++p;
				char word[64];
				size_t n = 0;
				while (*p && *p != ',' && *p != ':' && n + 1 < sizeof(word)) {
					if (*p != ' ') word[n++] = upperChar(*p);
					++p;
				}
				word[n] = 0;
				i32 amount = 1;
				if (*p == ':') {
					++p;
					amount = i32(atol(p));
					while (*p && *p != ',') ++p;
				}
				if (!n) continue;
				const u32 s = game.vm().findHash(hashName(word));
				if (s == SCRIPT_NONE || !game.isItem(s) || amount <= 0) continue;
				bool joined = false;
				for (u32 k = 0; k < c.n; ++k)
					if (c.stacks[k].symbol == s) {
						c.stacks[k].count += amount;
						joined = true;
					}
				if (!joined) c.stacks[c.n++] = Stack {s, amount};
			}
		}
	}
	free(blob);
	apply();
	++_version;
}

void Chests::resetChanges(Pak& pak, const char* worldName, u32 worldHash, Game& game) {
	for (u32 i = 0; i < _count; ++i) _chests[i].changed = false; // (so that leaving the world writes nothing down)
	free(_records);
	_records = nullptr;
	_recordCount = _recordCap = 0;
	enterWorld(pak, worldName, worldHash, game);
}

void Chests::unlock(u32 i) {
	if (i >= _count) return;
	_chests[i].locked = false;
	_chests[i].changed = true;
	++_version;
}

bool Chests::add(u32 i, u32 symbol, i32 count) {
	if (i >= _count || count <= 0) return false;
	Chest& c = _chests[i];
	for (u32 k = 0; k < c.n; ++k)
		if (c.stacks[k].symbol == symbol) {
			c.stacks[k].count += count;
			c.changed = true;
			++_version;
			return true;
		}
	if (c.n >= MAX_STACKS) return false;
	c.stacks[c.n++] = Stack {symbol, count};
	c.changed = true;
	++_version;
	return true;
}

i32 Chests::remove(u32 i, u32 symbol, i32 count) {
	if (i >= _count || count <= 0) return 0;
	Chest& c = _chests[i];
	for (u32 k = 0; k < c.n; ++k) {
		if (c.stacks[k].symbol != symbol) continue;
		const i32 taken = count < c.stacks[k].count ? count : c.stacks[k].count;
		c.stacks[k].count -= taken;
		if (c.stacks[k].count <= 0) {
			memmove(&c.stacks[k], &c.stacks[k + 1], sizeof(Stack) * (c.n - k - 1));
			--c.n;
		}
		c.changed = true;
		++_version;
		return taken;
	}
	return 0;
}

int Chests::nearest(const Mobs& mobs, Vec3 feet, float yaw, float reach, float halfAngle) const {
	int best = -1;
	float bestD = reach * reach;
	for (u32 i = 0; i < _count; ++i) {
		if (_chests[i].mob >= mobs.count()) continue;
		const WorldMob& m = mobs.mob(_chests[i].mob);
		const float dx = m.pos[0] - feet.x, dz = m.pos[2] - feet.z;
		const float d2 = dx * dx + dz * dz;
		if (d2 >= bestD || fabsf(m.pos[1] - feet.y) > 140.0f) continue;
		if (d2 > 40.0f * 40.0f) {
			float a = atan2f(dx, dz) - yaw;
			if (!(fabsf(a) < 1.0e4f)) a = 0.0f; // (fix134: an angle gone to infinity would loop here for ever)
			while (a > 3.14159265f) a -= 6.2831853f;
			while (a < -3.14159265f) a += 6.2831853f;
			if (fabsf(a) > halfAngle) continue;
		}
		bestD = d2;
		best = int(i);
	}
	return best;
}

void Chests::saveChanged(FILE* f) const {
	if (!f) return;
	flush();
	const u32 head[2] = {CHANGED_MAGIC, _recordCount};
	u32 check = 0x9E3779B9u;
	const auto add = [&](const void* p, size_t n) {
		const u8* b = static_cast<const u8*>(p);
		for (size_t k = 0; k < n; ++k) check = (check ^ b[k]) * 16777619u;
		fwrite(p, n, 1, f);
	};
	add(head, sizeof(head));
	if (_recordCount) add(_records, sizeof(Record) * _recordCount);
	fwrite(&check, sizeof(check), 1, f);
}

bool Chests::loadChanged(FILE* f) {
	u32 head[2] = {};
	if (!f || fread(head, sizeof(head), 1, f) != 1 || head[0] != CHANGED_MAGIC || head[1] > (1u << 16)) return false;
	Record* list = static_cast<Record*>(malloc(sizeof(Record) * (head[1] ? head[1] : 1u)));
	if (!list) return false;
	u32 check = 0x9E3779B9u;
	const auto sum = [&](const void* p, size_t n) {
		const u8* b = static_cast<const u8*>(p);
		for (size_t k = 0; k < n; ++k) check = (check ^ b[k]) * 16777619u;
	};
	sum(head, sizeof(head));
	u32 stored = 0;
	bool ok = (head[1] == 0 || fread(list, sizeof(Record) * head[1], 1, f) == 1) && (sum(list, sizeof(Record) * head[1]), true) &&
	          fread(&stored, sizeof(stored), 1, f) == 1 && stored == check;
	for (u32 i = 0; ok && i < head[1]; ++i) ok = list[i].n <= MAX_STACKS;
	if (!ok) {
		free(list);
		return false;
	}
	free(_records);
	_records = list;
	_recordCount = _recordCap = head[1];
	if (!_recordCap) {
		free(_records);
		_records = nullptr;
	}
	// This world as the records say: the containers back as they were made, then the changes.
	// (The table is read again from the archive by the caller if it wants the start; the
	// changes simply replace the contents of the containers they name.)
	apply();
	++_version;
	return true;
}

} // namespace g3
