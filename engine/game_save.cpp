// Saving what the scripts and the engine changed (Game::save / restore).
//
// The scripts' image is the same in the game that saves and the game that
// loads, so symbols are named by number; the section carries a fingerprint
// of the image and is refused by another. Characters are told apart by their
// instance and, among those of one instance, their order of creation.
#include "bytes.h"
#include "game.h"

#include <stdlib.h>
#include <string.h>

namespace g3 {

namespace {

constexpr u32 SAVE_VERSION = 1;
constexpr u32 MAX_STRING_MEMBERS = 96;

template <class T> bool growTo(T*& items, u32& cap, u32 need, u32 first) {
	if (need <= cap) return true;
	u32 to = cap ? cap * 2 : first;
	while (to < need) to *= 2;
	T* grown = static_cast<T*>(realloc(static_cast<void*>(items), size_t(to) * sizeof(T)));
	if (!grown) return false;
	items = grown;
	cap = to;
	return true;
}

char* duplicate(const char* s) {
	const size_t n = strlen(s) + 1;
	char* c = static_cast<char*>(malloc(n));
	if (c) memcpy(c, s, n);
	return c;
}

} // namespace

void Game::freePending() {
	for (u32 i = 0; i < _pendingCount; ++i) free(_pending[i].blob);
	free(_pending);
	_pending = nullptr;
	_pendingCount = _pendingCap = 0;
}

void Game::save(ByteSink& out) const {
	out.u32v(SAVE_VERSION);
	ByteSink globals;
	_vm.saveGlobals(globals);
	out.block(globals);
	out.u32v(_seed);

	// The guilds' table.
	out.u32v(_guildCount);
	out.put(_guildAtt, size_t(_guildCount) * _guildCount * sizeof(i32));

	// The characters.
	u32 strings[MAX_STRING_MEMBERS];
	u32 stringCount = _clsNpc != SCRIPT_NONE ? _vm.stringMembers(_clsNpc, strings, MAX_STRING_MEMBERS) : 0u;
	out.u32v(_npcCount);
	for (u32 i = 0; i < _npcCount; ++i) {
		const GameNpc& n = _npcs[i];
		ByteSink r;
		u32 ordinal = 0;
		for (u32 k = 0; k < i; ++k) ordinal += _npcs[k].symbol == n.symbol ? 1u : 0u;
		r.u32v(n.symbol);
		r.u32v(ordinal);
		r.u8v(n.knowsPlayer ? 1 : 0);
		r.u8v(u8(n.attitude));
		r.u8v(u8(n.tempAttitude));
		r.u8v(n.down);
		r.u8v(n.weapon);
		r.u8v(n.placed ? 1 : 0);
		r.u16v(n.world);
		r.u32v(n.melee);
		r.u32v(n.ranged);
		r.u32v(n.armorItem);
		r.u32v(n.state);
		r.u32v(n.moved);
		for (float v : n.pos) r.f32v(v);
		r.f32v(n.yaw);
		r.str(text(n.at));
		r.str(text(n.want));
		r.u16v(n.routineCount);
		for (u32 k = 0; k < n.routineCount; ++k) {
			const GameRoutine& rt = _routines[n.routineFirst + k];
			r.u16v(rt.start);
			r.u16v(rt.stop);
			r.u32v(rt.state);
			r.str(text(rt.point));
		}
		if (n.object) {
			r.u8v(1);
			ByteSink o;
			_vm.saveObject(n.object, strings, stringCount, o);
			r.block(o);
		} else {
			r.u8v(0);
		}
		// What it carries (the player's things are the inventory's).
		u32 stacks = 0;
		if (!n.hero)
			for (u32 k = 0; k < _itemCount; ++k) stacks += _items[k].npc == i ? 1u : 0u;
		r.u32v(stacks);
		if (!n.hero)
			for (u32 k = 0; k < _itemCount; ++k)
				if (_items[k].npc == i) {
					r.u32v(_items[k].symbol);
					r.i32v(_items[k].count);
				}
		for (i16 t : n.talent) r.u16v(u16(t));
		out.block(r);
	}

	// What has been told.
	out.u32v(_infoCount);
	for (u32 i = 0; i < _infoCount; ++i) out.u8v(_infos[i].told ? 1 : 0);

	// The diary.
	out.u32v(_logTopicCount);
	for (u32 i = 0; i < _logTopicCount; ++i) {
		out.str(_logTopics[i].name);
		out.u8v(_logTopics[i].section);
		out.u8v(_logTopics[i].status);
		out.u32v(_logTopics[i].order);
	}
	out.u32v(_logEntryCount);
	for (u32 i = 0; i < _logEntryCount; ++i) {
		out.u32v(_logEntries[i].topic);
		out.str(_logEntries[i].text);
	}
	out.u32v(_logVersion);

	// The chapter card.
	out.str(_chapter.title);
	out.str(_chapter.subtitle);
	out.str(_chapter.texture);
	out.str(_chapter.sound);
	out.i32v(_chapter.delay);
	out.u32v(_chapter.serial);
}

bool Game::restore(ByteSource& in) {
	if (!loaded() || in.u32v() != SAVE_VERSION || in.bad) return false;
	ByteSource globals = in.block();
	if (in.bad || !_vm.loadGlobals(globals)) return false;
	_seed = in.u32v();

	const u32 guilds = in.u32v();
	if (in.bad || guilds > 4096 || in.left() < size_t(guilds) * guilds * 4) return false;
	if (guilds == _guildCount && _guildAtt) in.get(_guildAtt, size_t(guilds) * guilds * 4);
	else in.get(nullptr, size_t(guilds) * guilds * 4);

	// Saved characters are matched with the ones made so far; the rest wait.
	freePending();
	const u32 saved = in.u32v();
	if (in.bad || saved > (1u << 20)) return false;
	for (u32 s = 0; s < saved && !in.bad; ++s) {
		ByteSource r = in.block();
		if (in.bad) break;
		ByteSource head = r; // (a copy: read the key, keep r whole)
		const u32 symbol = head.u32v(), ordinal = head.u32v();
		int found = -1;
		u32 seen = 0;
		for (u32 i = 0; i < _npcCount; ++i)
			if (_npcs[i].symbol == symbol) {
				if (seen++ == ordinal) {
					found = int(i);
					break;
				}
			}
		if (found >= 0) {
			r.u32v();
			r.u32v();
			restoreNpc(u32(found), r);
		} else if (growTo(_pending, _pendingCap, _pendingCount + 1, 64)) {
			u8* copy = static_cast<u8*>(malloc(r.size ? r.size : 1u));
			if (copy) {
				memcpy(copy, r.data, r.size);
				_pending[_pendingCount++] = Pending {symbol, ordinal, copy, u32(r.size)};
			}
		}
	}

	const u32 infos = in.u32v();
	for (u32 i = 0; i < infos && !in.bad; ++i) {
		const u8 told = in.u8v();
		if (i < _infoCount) _infos[i].told = told != 0;
	}
	++_talkVersion;

	// The diary: the saved one replaces this one.
	const u32 topics = in.u32v();
	if (in.bad || topics > 4096) return false;
	for (u32 i = 0; i < _logTopicCount; ++i) free(_logTopics[i].name);
	for (u32 i = 0; i < _logEntryCount; ++i) free(_logEntries[i].text);
	_logTopicCount = _logEntryCount = 0;
	char buf[1024];
	for (u32 i = 0; i < topics && !in.bad; ++i) {
		in.str(buf, sizeof(buf));
		const u8 section = in.u8v(), status = in.u8v();
		const u32 order = in.u32v();
		if (in.bad || !growTo(_logTopics, _logTopicCap, _logTopicCount + 1, 32)) break;
		if (char* name = duplicate(buf)) _logTopics[_logTopicCount++] = GameLogTopic {name, section, status, order};
	}
	const u32 entries = in.u32v();
	if (in.bad || entries > (1u << 16)) return false;
	for (u32 i = 0; i < entries && !in.bad; ++i) {
		const u32 topic = in.u32v();
		in.str(buf, sizeof(buf));
		if (in.bad || topic >= _logTopicCount || !growTo(_logEntries, _logEntryCap, _logEntryCount + 1, 64)) continue;
		if (char* text = duplicate(buf)) _logEntries[_logEntryCount++] = GameLogEntry {topic, text};
	}
	const u32 version = in.u32v();
	_noticeCount = 0;
	_logVersion = (version > _logVersion ? version : _logVersion) + 1;

	GameChapter c;
	in.str(c.title, sizeof(c.title));
	in.str(c.subtitle, sizeof(c.subtitle));
	in.str(c.texture, sizeof(c.texture));
	in.str(c.sound, sizeof(c.sound));
	c.delay = in.i32v();
	// (A card seen before the save is not put up again by loading it.)
	c.serial = in.u32v();
	if (c.serial < _chapter.serial) c.serial = _chapter.serial;
	if (!in.bad) _chapter = c;
	++_itemsVersion;
	return !in.bad;
}

// One character's record (after its key) onto the character `index`.
void Game::restoreNpc(u32 index, ByteSource& r) {
	GameNpc& n = _npcs[index];
	n.knowsPlayer = r.u8v() != 0;
	n.attitude = i8(r.u8v());
	n.tempAttitude = i8(r.u8v());
	const u8 down = r.u8v();
	const u8 weapon = r.u8v();
	const bool placed = r.u8v() != 0;
	const u16 world = r.u16v();
	const u32 melee = r.u32v(), ranged = r.u32v(), armor = r.u32v();
	const u32 state = r.u32v(), moved = r.u32v();
	float pos[3];
	for (float& v : pos) v = r.f32v();
	const float yaw = r.f32v();
	char at[96], want[96];
	r.str(at, sizeof(at));
	r.str(want, sizeof(want));
	const u16 routines = r.u16v();
	GameRoutine entries[64];
	char points[64][80];
	const u32 kept = routines < 64 ? routines : 64u;
	for (u32 k = 0; k < routines && !r.bad; ++k) {
		GameRoutine e {};
		e.start = r.u16v();
		e.stop = r.u16v();
		e.state = r.u32v();
		char point[80];
		r.str(point, sizeof(point));
		if (k < kept) {
			entries[k] = e;
			memcpy(points[k], point, sizeof(point));
		}
	}
	if (r.bad) return;

	n.down = down;
	n.weapon = weapon;
	n.melee = melee;
	n.ranged = ranged;
	n.armorItem = armor;
	n.target = -1;
	n.lastHit = -1;
	n.active = false;
	if (!n.hero) {
		n.placed = placed;
		n.world = world;
		memcpy(n.pos, pos, sizeof(pos));
		n.yaw = yaw;
		n.state = state;
		n.moved = moved + 1;
		n.at = intern(at, false);
		n.want = intern(want, false);
		// Its routine: the saved one, unless the scripts have just made the same.
		bool same = n.routineCount == kept;
		for (u32 k = 0; same && k < kept; ++k) {
			const GameRoutine& have = _routines[n.routineFirst + k];
			same = have.start == entries[k].start && have.stop == entries[k].stop && have.state == entries[k].state &&
			       strcmp(text(have.point), points[k]) == 0;
		}
		if (!same && kept && growTo(_routines, _routineCap, _routineCount + kept, 512)) {
			n.routineFirst = _routineCount;
			for (u32 k = 0; k < kept; ++k) {
				entries[k].point = intern(points[k], false);
				_routines[_routineCount++] = entries[k];
			}
			n.routineCount = u16(kept);
		}
	}

	if (r.u8v() && n.object) {
		ByteSource object = r.block();
		u32 strings[MAX_STRING_MEMBERS];
		const u32 count = _clsNpc != SCRIPT_NONE ? _vm.stringMembers(n.object->classSymbol, strings, MAX_STRING_MEMBERS) : 0u;
		_vm.loadObject(n.object, strings, count, object);
	} else if (!r.bad) {
		// (no object saved)
	}

	// What it carries is exactly what was saved.
	const u32 stacks = r.u32v();
	if (r.bad || stacks > (1u << 16)) return;
	if (!n.hero) {
		for (u32 k = 0; k < _itemCount; ++k)
			if (_items[k].npc == index) _items[k].count = 0;
		for (u32 k = 0; k < stacks && !r.bad; ++k) {
			const u32 symbol = r.u32v();
			const i32 count = r.i32v();
			if (r.bad || count <= 0) continue;
			bool set = false;
			for (u32 q = 0; q < _itemCount && !set; ++q)
				if (_items[q].npc == index && _items[q].symbol == symbol) {
					_items[q].count = count;
					set = true;
				}
			if (!set && growTo(_items, _itemCap, _itemCount + 1, 256)) _items[_itemCount++] = GameItem {index, symbol, count};
		}
		++_itemsVersion;
	}
	// (saves from before the talents end here.)
	if (!r.bad && r.left() >= sizeof(n.talent) / sizeof(n.talent[0]) * 2)
		for (i16& t : n.talent) t = i16(r.u16v());
}

// A character was just made (addNpc): if a saved one is waiting for it, it
// gets its state.
void Game::applyPending(u32 index) {
	if (!_pendingCount || index >= _npcCount) return;
	const u32 symbol = _npcs[index].symbol;
	u32 ordinal = 0;
	for (u32 k = 0; k < index; ++k) ordinal += _npcs[k].symbol == symbol ? 1u : 0u;
	for (u32 i = 0; i < _pendingCount; ++i) {
		if (_pending[i].symbol != symbol || _pending[i].ordinal != ordinal) continue;
		ByteSource r(_pending[i].blob, _pending[i].size);
		r.u32v();
		r.u32v();
		restoreNpc(index, r);
		free(_pending[i].blob);
		_pending[i] = _pending[--_pendingCount];
		return;
	}
}

} // namespace g3
