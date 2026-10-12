#include "readahead.h"

#include "g3ds/model.h"

#include <stdlib.h>
#include <string.h>

namespace g3 {

namespace {

// What is worth reading ahead: what the game reads in the middle of a frame.
bool worthIt(u16 type) { return type == ASSET_MODEL || type == ASSET_TEXTURE || type == ASSET_CLIP || type == ASSET_CHUNK; }

constexpr u32 kMaxClipsPerModel = 12;

} // namespace

bool ReadAhead::start(const char* pakPath, u32 budget, bool threaded) {
	stop();
	if (!pakPath || !pakPath[0] || budget < 64 * 1024 || !_pak.open(pakPath)) return false;
	_budget = budget;
	_qHead = _qCount = 0;
	_bytes = _stamp = 0;
	_clipCount = _clipNext = 0;
	_stats = ReadAheadStats();
	_worstMs = 0.0f;
	_quit = false;
	if (threaded) {
		_lock = sys::mutexCreate();
		_wake = sys::signalCreate();
		if (_lock && _wake) _thread = sys::threadStart(threadMain, this);
		if (!_thread) {
			sys::mutexDestroy(_lock);
			sys::signalDestroy(_wake);
			_lock = nullptr;
			_wake = nullptr;
		}
	}
	_running = true;
	return true;
}

void ReadAhead::stop() {
	if (!_running) return;
	if (_thread) {
		lock();
		_quit = true;
		unlock();
		sys::signalWake(_wake);
		sys::threadJoin(_thread);
		_thread = nullptr;
		sys::mutexDestroy(_lock);
		sys::signalDestroy(_wake);
		_lock = nullptr;
		_wake = nullptr;
	}
	clear();
	_pak.close();
	_running = false;
}

void ReadAhead::setEnabled(bool on) {
	if (_enabled == on) return;
	_enabled = on;
	if (!on) clear();
}

void ReadAhead::attach(Pak& pak) {
	PakHooks h;
	h.take = &ReadAhead::takeHook;
	h.clip = &ReadAhead::clipHook;
	h.user = this;
	pak.setHooks(h);
}

void ReadAhead::threadMain(void* self) { static_cast<ReadAhead*>(self)->work(); }

void ReadAhead::work() {
	for (;;) {
		lock();
		const bool quit = _quit;
		unlock();
		if (quit) return;
		if (!readOne()) sys::signalWait(_wake);
	}
}

ReadAhead::Slot* ReadAhead::findLocked(u32 hash) {
	for (Slot& s : _slots)
		if (s.blob && s.hash == hash) return &s;
	return nullptr;
}

bool ReadAhead::queuedLocked(u32 hash) const {
	for (u32 i = 0; i < _qCount; ++i)
		if (_queue[(_qHead + i) % QUEUE] == hash) return true;
	return false;
}

void ReadAhead::pushLocked(u32 hash) {
	if (!hash || _qCount == QUEUE || queuedLocked(hash)) return;
	if (Slot* s = findLocked(hash)) {
		s->stamp = ++_stamp;
		return;
	}
	_queue[(_qHead + _qCount) % QUEUE] = hash;
	++_qCount;
	++_stats.requests;
}

void ReadAhead::freeSlotLocked(Slot& s) {
	free(s.blob);
	_bytes -= s.size;
	s = Slot();
}

void ReadAhead::storeLocked(u32 hash, u8* blob, u32 size) {
	if (findLocked(hash) || size > _budget) {
		free(blob);
		return;
	}
	for (;;) {
		Slot* oldest = nullptr;
		Slot* empty = nullptr;
		for (Slot& s : _slots) {
			if (!s.blob) {
				if (!empty) empty = &s;
			} else if (!oldest || s.stamp < oldest->stamp) {
				oldest = &s;
			}
		}
		if (empty && _bytes + size <= _budget) {
			empty->hash = hash;
			empty->blob = blob;
			empty->size = size;
			empty->stamp = ++_stamp;
			_bytes += size;
			return;
		}
		if (!oldest) {
			free(blob);
			return;
		}
		freeSlotLocked(*oldest);
		++_stats.evicted;
	}
}

bool ReadAhead::wantedClipLocked(u32 nameHash) const {
	for (u32 i = 0; i < _clipCount; ++i)
		if (_clipNames[i] == nameHash) return true;
	return false;
}

// A model that has been read wishes for its textures and for the animations the game is known to ask for.
void ReadAhead::chain(const PakEntry& e, const u8* blob) {
	if (e.type != ASSET_MODEL || e.size < sizeof(ModelHeader)) return;
	ModelHeader h;
	memcpy(&h, blob, sizeof(h));
	if (h.magic != MODEL_MAGIC || h.version != MODEL_VERSION) return;
	const u64 partsAt = sizeof(ModelHeader) + u64(h.nodeCount) * sizeof(ModelNode);
	const u64 refsAt = partsAt + u64(h.partCount) * sizeof(ModelPart);
	if (refsAt + u64(h.clipCount) * sizeof(ModelClipRef) > e.size) return;
	lock();
	for (u32 i = 0; i < h.partCount; ++i) {
		ModelPart part;
		memcpy(&part, blob + partsAt + u64(i) * sizeof(ModelPart), sizeof(part));
		pushLocked(part.texture);
	}
	u32 clips = 0;
	for (u32 i = 0; i < h.clipCount && clips < kMaxClipsPerModel; ++i) {
		ModelClipRef ref;
		memcpy(&ref, blob + refsAt + u64(i) * sizeof(ModelClipRef), sizeof(ref));
		if (!wantedClipLocked(ref.nameHash)) continue;
		pushLocked(ref.entryHash);
		++clips;
	}
	unlock();
}

bool ReadAhead::readOne() {
	lock();
	if (_qCount == 0) {
		unlock();
		return false;
	}
	const u32 hash = _queue[_qHead];
	_qHead = (_qHead + 1) % QUEUE;
	--_qCount;
	const bool have = findLocked(hash) != nullptr;
	unlock();
	if (have || !_enabled) return true;
	const PakEntry* e = _pak.find(hash);
	if (!e || !worthIt(e->type) || e->size == 0 || e->size > _budget / 2) return true;
	const double t0 = sys::timeMs();
	u8* blob = static_cast<u8*>(_pak.readAlloc(*e));
	const float ms = float(sys::timeMs() - t0);
	if (!blob) return true;
	chain(*e, blob);
	lock();
	if (ms > _worstMs) _worstMs = ms;
	if (_enabled) {
		storeLocked(hash, blob, e->size);
		++_stats.loaded;
	} else {
		free(blob);
	}
	unlock();
	return true;
}

bool ReadAhead::pump() { return _running && !_thread && readOne(); }

void ReadAhead::request(u32 entryHash) {
	if (!_running || !_enabled) return;
	lock();
	const u32 before = _qCount;
	pushLocked(entryHash);
	const bool added = _qCount != before;
	unlock();
	if (added && _thread) sys::signalWake(_wake);
}

bool ReadAhead::has(u32 entryHash) {
	if (!_running) return false;
	lock();
	const bool have = findLocked(entryHash) != nullptr;
	unlock();
	return have;
}

void ReadAhead::noteClip(u32 nameHash) {
	if (!_running || !nameHash) return;
	lock();
	if (!wantedClipLocked(nameHash)) {
		_clipNames[_clipNext] = nameHash;
		_clipNext = (_clipNext + 1) % CLIP_NAMES;
		if (_clipCount < CLIP_NAMES) ++_clipCount;
	}
	unlock();
}

void ReadAhead::clear() {
	lock();
	for (Slot& s : _slots)
		if (s.blob) freeSlotLocked(s);
	_qHead = _qCount = 0;
	unlock();
}

ReadAheadStats ReadAhead::stats() {
	lock();
	ReadAheadStats s = _stats;
	s.bytes = _bytes;
	s.queued = _qCount;
	s.entries = 0;
	for (const Slot& x : _slots) s.entries += x.blob != nullptr;
	unlock();
	return s;
}

float ReadAhead::takeWorstMs() {
	lock();
	const float ms = _worstMs;
	_worstMs = 0.0f;
	unlock();
	return ms;
}

void* ReadAhead::takeHook(void* self, const PakEntry& e) {
	ReadAhead* r = static_cast<ReadAhead*>(self);
	if (!r->_running || !r->_enabled || !worthIt(e.type)) return nullptr;
	void* out = nullptr;
	r->lock();
	if (Slot* s = r->findLocked(e.hash)) {
		if (s->size == e.size) {
			out = s->blob;
			r->_bytes -= s->size;
			*s = Slot();
			++r->_stats.hits;
		}
	}
	if (!out) ++r->_stats.misses;
	r->unlock();
	return out;
}

void ReadAhead::clipHook(void* self, u32 nameHash) { static_cast<ReadAhead*>(self)->noteClip(nameHash); }

} // namespace g3
