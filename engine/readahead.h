// Reading ahead: the SD card's part of loading, done before it is needed.
//
// Everything the game reads is read from the archive, on whichever thread wants it: the chunks on the loader's thread
// (one job at a time: read, then build the collision, then the next), the characters' models, their textures and their
// animations on the frame's own thread - each of those a stall of its own, a few milliseconds to tens of them, right
// when somebody comes into view. The card is slow and the processor is not: while one chunk's collision is being
// built, the next chunk could be coming off the card.
//
// This is a cache of entries' bytes (already unpacked) in memory, filled by a thread of its own from a list of wishes
// and emptied by whoever reads next: the archive (Pak::setHooks) asks it first and takes the bytes if they are there,
// so the parsers that read models, textures, clips and chunks did not have to change. Nothing is kept once taken. When
// the cache is full the entry that has been waiting longest goes.
//
// Wishes come from whoever can tell what will be wanted soon: the world (the chunks it would load next, nearest and
// ahead of the way the camera goes first), the inhabitants (the characters about to come into range). A model that is
// read also wishes for its own textures and for the animations the game has been seen to ask for (it learns the names
// as the game fetches them), so that by the time the character appears nothing of it is left on the card.
#pragma once
#include "g3ds/base.h"
#include "g3ds/pak.h"
#include "sys.h"

namespace g3 {

struct ReadAheadStats {
	u32 hits = 0;        // reads that found their bytes waiting (of models, textures, animations, chunks)
	u32 misses = 0;      // reads of such entries that had to go to the card
	u32 requests = 0;    // wishes accepted
	u32 loaded = 0;      // entries read ahead
	u32 evicted = 0;     // read ahead and thrown away unused
	u32 entries = 0;     // waiting now
	u32 bytes = 0;       // ... their size
	u32 queued = 0;      // wishes not yet read
};

class ReadAhead {
public:
	static constexpr u32 QUEUE = 96; // per queue: the urgent ones (models, meshes, textures, chunks) and the rest (animations)
	static constexpr u32 SLOTS = 64;
	static constexpr u32 CLIP_NAMES = 64;

	ReadAhead() = default;
	~ReadAhead() { stop(); }
	ReadAhead(const ReadAhead&) = delete;
	ReadAhead& operator=(const ReadAhead&) = delete;

	// Opens the archive a second time for the thread. `budget`: bytes kept at most. Without `threaded` (or if no
	// thread can be had) nothing is read until pump() is called. False if the archive cannot be opened.
	bool start(const char* pakPath, u32 budget, bool threaded);
	void stop();
	bool running() const { return _running; }
	// Off: wishes are ignored and what is waiting is dropped (the menu's switch).
	void setEnabled(bool on);
	bool enabled() const { return _enabled; }

	// This archive asks here first (see PakHooks). The reads of the thread's own archive do not.
	void attach(Pak& pak);

	// Wishes the entry. Cheap; a wish already known only counts as recent again.
	void request(u32 entryHash);
	// Is it waiting in memory now?
	bool has(u32 entryHash);
	// The name of an animation the game is fetching.
	void noteClip(u32 nameHash);

	// Unthreaded: reads one wish. Returns whether there was one.
	bool pump();
	// Drops everything waiting and every wish.
	void clear();
	ReadAheadStats stats();
	// The longest single read since the last call, ms.
	float takeWorstMs();

	// The hooks' callbacks (public for the function pointers).
	static void* takeHook(void* self, const PakEntry& e);
	static void clipHook(void* self, u32 nameHash);

private:
	struct Slot {
		u32 hash = 0;
		u8* blob = nullptr;
		u32 size = 0;
		u32 stamp = 0;
	};

	static void threadMain(void* self);
	void work();
	bool readOne();
	bool queuedLocked(u32 hash) const;
	Slot* findLocked(u32 hash);
	void pushLocked(u32 hash, bool low = false);
	void storeLocked(u32 hash, u8* blob, u32 size);
	void freeSlotLocked(Slot& s);
	void chain(const PakEntry& e, const u8* blob);
	bool wantedClipLocked(u32 nameHash) const;
	void lock() const {
		if (_lock) sys::mutexLock(_lock);
	}
	void unlock() const {
		if (_lock) sys::mutexUnlock(_lock);
	}

	Pak _pak; // the thread's own handle (no hooks)
	sys::Thread* _thread = nullptr;
	sys::Mutex* _lock = nullptr;
	sys::Signal* _wake = nullptr;
	bool _running = false, _quit = false;
	volatile bool _enabled = true;
	u32 _budget = 0;

	u32 _queue[QUEUE];
	u32 _qHead = 0, _qCount = 0;
	u32 _lowQueue[QUEUE];
	u32 _lHead = 0, _lCount = 0; // animations: read when nothing urgent waits (fix89)
	Slot _slots[SLOTS];
	u32 _bytes = 0, _stamp = 0;
	u32 _clipNames[CLIP_NAMES];
	u32 _clipCount = 0, _clipNext = 0;
	ReadAheadStats _stats;
	float _worstMs = 0.0f;
};

} // namespace g3
