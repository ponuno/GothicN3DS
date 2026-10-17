// Voices. What the characters say has been recorded for the game; "speech.pak"
// (g3ds/sound.h) has the recordings of the lines the baker found, as IMA ADPCM.
// A line is read when the dialogue reaches it, decoded into the memory the
// sound hardware plays from and played on a channel of its own, whole.
// The dialogue (npc_dialog.cpp) asks how long it is and keeps the subtitle
// up that long; without a recording it goes by the time it takes to read.
//
// fix204: with a thread (init's `threaded`) the card and the decoding are no business of the frame's. The talk tells
// prefetch() each line the moment the scripts queue it (an info's AI_Outputs are queued all at once, long before
// they are said); a thread of its own reads and decodes it into ordinary memory. play() then only copies the samples
// into the sound memory. A line that has not arrived yet is measured from its header (one small read) and starts
// in update() as soon as it is there. Without a thread everything happens inside play(), as before.
#pragma once
#include "audio.h"
#include "g3ds/pak.h"
#include "g3ds/sound.h"
#include "sys.h"

#include <stdio.h>

namespace g3 {

class Speech {
public:
	// The channel of the player's talk; what the world says (a second Speech) has the one before the last.
	enum { CHANNEL = 15, WORLD_CHANNEL = 14 };

	explicit Speech(u32 channel = CHANNEL) : _channel(channel) {}
	~Speech() { shutdown(); }
	Speech(const Speech&) = delete;
	Speech& operator=(const Speech&) = delete;

	// `speechPakPath`: the archive of recordings. False (nothing will be heard) if it is not
	// there or the platform has no channel to spare.
	// The archive may be in pieces (the console cannot seek past 2 GiB): "speech2.pak", "speech3.pak" ...
	// beside it are opened too.
	bool init(const char* speechPakPath, AudioOut* out, bool threaded = false);
	void shutdown();
	bool ready() const { return _ready; }

	void setVolume(float volume) { _volume = clamp(volume, 0.0f, 1.0f); }
	float volume() const { return _volume; }

	// Plays the recording of the line named by hashName(). `gain` 0..1 (distance: the dialogue's
	// business). Returns its length in seconds, or 0 if there is none (or it is muted: then the
	// length is still returned, so that the subtitle stays up as long as the line is).
	// `pan` -1..1 (left to right). `takeOver` false: a line that is still sounding is left alone and the new one is
	// only measured (a second quarrel in a street does not cut the first).
	float play(u32 lineHash, float gain, float pan = 0.0f, bool takeOver = true);
	// Silence (the line is skipped, the talk is over).
	void stop();
	bool playing() const;
	// fix204: the line will be said soon: read and decode it now, on the thread (nothing without one).
	void prefetch(u32 lineHash);
	// Once a frame: a line play() had to wait for starts as soon as its samples are there.
	void update();
	bool threaded() const { return _thread != nullptr; }
	// Lines that were waiting when play() came (for the reports).
	u32 prefetchHits() const { return _hits; }
	u32 prefetchLate() const { return _late; }

	// For the reports.
	u32 played() const { return _played; }
	u32 missing() const { return _missing; }
	u32 broken() const { return _broken; }
	void writeReport(FILE* f) const;

private:
	u32 _channel = CHANNEL;
	AudioOut* _out = nullptr;
	bool _ready = false;
	float _volume = 0.8f;
	enum { MAX_PAKS = 4 };
	Pak _paks[MAX_PAKS];
	u32 _pakCount = 0;
	bool _unusable = false; // a speech.pak was there and could not be opened
	// Two blocks, used in turn: the one that played last is freed when the next line starts.
	void* _block[2] = {};
	u32 _blockBytes[2] = {};
	u32 _cur = 0;
	u32 _played = 0, _missing = 0, _broken = 0;

	// ---- fix204: the thread and what it has made ready ----
	enum SlotState : u8 { SL_EMPTY, SL_QUEUED, SL_READING, SL_READY, SL_FAILED };
	enum TakeResult { TAKE_NONE, TAKE_WAIT, TAKE_READY, TAKE_FAILED };
	enum { SLOTS = 12 };
	static constexpr u32 BUDGET = 1536u * 1024u; // bytes of decoded samples waiting at most (ordinary heap)
	struct Slot {
		u32 hash = 0;   // the line (hashName of its name)
		u8 state = SL_EMPTY;
		i16* pcm = nullptr; // SL_READY: malloc'ed samples
		u32 frames = 0, rate = 0, bytes = 0;
		u32 stamp = 0;
	};
	struct Pending {
		bool active = false;
		u32 hash = 0;
		float left = 0.0f, right = 0.0f;
		double since = 0.0;
	};
	static void threadMain(void* self);
	void work();
	const PakEntry* findEntry(Pak* paks, u32 lineHash, Pak** which) const;
	float playSync(u32 lineHash, float gain, float pan, bool takeOver);
	TakeResult take(u32 hash, Slot& out);
	void queueLocked(u32 hash);
	void startSamples(const i16* pcm, u32 frames, u32 rate, float left, float right);
	void lock() const {
		if (_lock) sys::mutexLock(_lock);
	}
	void unlock() const {
		if (_lock) sys::mutexUnlock(_lock);
	}

	Pak _workerPaks[MAX_PAKS]; // the thread's own handles of the same archives
	sys::Thread* _thread = nullptr;
	sys::Mutex* _lock = nullptr;
	sys::Signal* _wake = nullptr;
	bool _quit = false;
	Slot _slots[SLOTS];
	u32 _slotBytes = 0, _stamp = 0;
	Pending _pending;
	u32 _hits = 0, _late = 0;
};

} // namespace g3
