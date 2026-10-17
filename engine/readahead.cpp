#include "readahead.h"

#include "g3ds/model.h"

#include <stdlib.h>
#include <string.h>

namespace g3 {

namespace {

// What is worth reading ahead: what the game reads in the middle of a frame.
bool worthIt(u16 type) { return type == ASSET_MODEL || type == ASSET_TEXTURE || type == ASSET_CLIP || type == ASSET_CHUNK; }
// fix204: a plain blob is read only when somebody wished for it by name (the talk's blocks of words); it does not
// count in the hits and misses, which are about what the game reads in the middle of a frame.
bool readable(u16 type) { return worthIt(type) || type == ASSET_BLOB; }

constexpr u32 kMaxClipsPerModel = 24;

} // namespace

bool ReadAhead::start(const char* pakPath, u32 budget, bool threaded) {
	stop();
	if (!pakPath || !pakPath[0] || budget < 64 * 1024 || !_pak.open(pakPath)) return false;
	_budget = budget;
	_qHead = _qCount = _lHead = _lCount = 0;
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
	for (u32 i = 0; i < _lCount; ++i)
		if (_lowQueue[(_lHead + i) % QUEUE] == hash) return true;
	return false;
}

void ReadAhead::pushLocked(u32 hash, bool low) {
	if (!hash || queuedLocked(hash)) return;
	if (Slot* s = findLocked(hash)) {
		s->stamp = ++_stamp;
		return;
	}
	// A full queue lets the oldest wish go: what is wished now matters more than what was wished when the player was
	// somewhere else (before, a full queue refused the new ones and the old ones were read first).
	u32* q = low ? _lowQueue : _queue;
	u32& head = low ? _lHead : _qHead;
	u32& count = low ? _lCount : _qCount;
	if (count == QUEUE) {
		head = (head + 1) % QUEUE;
		--count;
		++_stats.dropped;
	}
	q[(head + count) % QUEUE] = hash;
	++count;
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
	if ((h.flags & MODELF_SHARED_MESH) && h.reserved) pushLocked(h.reserved);
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
		pushLocked(ref.entryHash, true);
		++_stats.clipWished;
		++clips;
	}
	unlock();
}

bool ReadAhead::readOne() {
	lock();
	if (_qCount == 0 && _lCount == 0) {
		unlock();
		return false;
	}
	u32 hash;
	if (_qCount > 0) {
		hash = _queue[_qHead];
		_qHead = (_qHead + 1) % QUEUE;
		--_qCount;
	} else {
		hash = _lowQueue[_lHead];
		_lHead = (_lHead + 1) % QUEUE;
		--_lCount;
	}
	const bool have = findLocked(hash) != nullptr;
	_reading = !have && _enabled;
	unlock();
	if (have || !_enabled || sys::timeMs() < _pauseUntil) {
		lock();
		_reading = false;
		unlock();
		return true;
	}
	const PakEntry* e = _pak.find(hash);
	if (!e || !readable(e->type) || e->size == 0 || e->size > _budget / 2) {
		lock();
		_reading = false;
		unlock();
		return true;
	}
	const double t0 = sys::timeMs();
	u8* blob = static_cast<u8*>(_pak.readAlloc(*e));
	const float ms = float(sys::timeMs() - t0);
	if (!blob) {
		lock();
		_reading = false;
		unlock();
		return true;
	}
	chain(*e, blob);
	lock();
	_reading = false;
	if (ms > _worstMs) _worstMs = ms;
	if (_enabled) {
		storeLocked(hash, blob, e->size);
		++_stats.loaded;
		if (e->type == ASSET_CLIP) ++_stats.clipRead;
	} else {
		free(blob);
	}
	unlock();
	return true;
}

bool ReadAhead::pump() { return _running && !_thread && readOne(); }

void ReadAhead::request(u32 entryHash) {
	if (!_running || !_enabled || sys::timeMs() < _pauseUntil) return;
	lock();
	const u32 before = _stats.requests;
	pushLocked(entryHash);
	const bool added = _stats.requests != before;
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

void ReadAhead::relieve(u32 ms) {
	if (!_running) return;
	const double now = sys::timeMs();
	if (now - _lastRelief < 2000.0) return; // (not again at once: the models wait for these bytes)
	_lastRelief = now;
	lock();
	_stats.dropped += _qCount + _lCount;
	for (Slot& s : _slots)
		if (s.blob) freeSlotLocked(s);
	_qHead = _qCount = _lHead = _lCount = 0;
	_pauseUntil = sys::timeMs() + double(ms);
	unlock();
}

void ReadAhead::clear() {
	lock();
	for (Slot& s : _slots)
		if (s.blob) freeSlotLocked(s);
	_qHead = _qCount = _lHead = _lCount = 0;
	unlock();
}

ReadAheadStats ReadAhead::stats() {
	lock();
	ReadAheadStats s = _stats;
	s.bytes = _bytes;
	s.queued = _qCount + _lCount;
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
	if (!r->_running || !r->_enabled || !readable(e.type)) return nullptr;
	const bool counted = worthIt(e.type);
	void* out = nullptr;
	r->lock();
	if (Slot* s = r->findLocked(e.hash)) {
		if (s->size == e.size) {
			out = s->blob;
			r->_bytes -= s->size;
			*s = Slot();
			if (counted) ++r->_stats.hits;
			if (e.type == ASSET_CLIP) ++r->_stats.clipHits;
		}
	}
	if (!out && counted) {
		++r->_stats.misses;
		if (e.type == ASSET_CLIP) ++r->_stats.clipMisses;
	}
	r->unlock();
	return out;
}

void ReadAhead::clipHook(void* self, u32 nameHash) { static_cast<ReadAhead*>(self)->noteClip(nameHash); }

} // namespace g3
