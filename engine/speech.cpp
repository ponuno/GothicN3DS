#include "speech.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace g3 {

bool Speech::init(const char* speechPakPath, AudioOut* out, bool threaded) {
	shutdown();
	if (!out || out->channels() <= _channel || !speechPakPath) return false;
	// "…/speech.pak", then "…/speech2.pak" ...
	char path[320];
	for (u32 k = 0; k < MAX_PAKS; ++k) {
		if (k == 0) {
			snprintf(path, sizeof(path), "%s", speechPakPath);
		} else {
			snprintf(path, sizeof(path), "%s", speechPakPath);
			char* dot = strrchr(path, '.');
			if (!dot) break;
			snprintf(dot, sizeof(path) - size_t(dot - path), "%u.pak", unsigned(k + 1));
		}
		if (_paks[_pakCount].open(path)) ++_pakCount;
		else if (k == 0) break;
	}
	if (!_pakCount) {
		FILE* f = fopen(speechPakPath, "rb");
		_unusable = f != nullptr; // (there, but not to be read: damaged, or too big for the console's seek)
		if (f) fclose(f);
		return false;
	}
	_out = out;
	_ready = true;
	// fix204: the thread reads through handles of its own (a FILE is not to be shared between threads).
	if (threaded) {
		bool all = true;
		for (u32 k = 0; k < _pakCount && all; ++k) all = _workerPaks[k].open(_paks[k].path());
		_lock = all ? sys::mutexCreate() : nullptr;
		_wake = all ? sys::signalCreate() : nullptr;
		_quit = false;
		if (_lock && _wake) _thread = sys::threadStart(&Speech::threadMain, this);
		if (!_thread) {
			for (Pak& p : _workerPaks) p.close();
			if (_lock) sys::mutexDestroy(_lock);
			if (_wake) sys::signalDestroy(_wake);
			_lock = nullptr;
			_wake = nullptr;
		}
	}
	return true;
}

void Speech::shutdown() {
	if (_thread) {
		lock();
		_quit = true;
		unlock();
		sys::signalWake(_wake);
		sys::threadJoin(_thread);
		_thread = nullptr;
	}
	for (Slot& s : _slots) {
		free(s.pcm);
		s = Slot();
	}
	_slotBytes = 0;
	_pending = Pending();
	for (Pak& p : _workerPaks) p.close();
	if (_lock) sys::mutexDestroy(_lock);
	if (_wake) sys::signalDestroy(_wake);
	_lock = nullptr;
	_wake = nullptr;
	if (_out && _ready) _out->stop(_channel);
	for (u32 i = 0; i < 2; ++i) {
		if (_block[i] && _out) _out->freeSamples(_block[i]);
		_block[i] = nullptr;
		_blockBytes[i] = 0;
	}
	for (Pak& p : _paks) p.close();
	_pakCount = 0;
	_ready = false;
	_out = nullptr;
}

void Speech::stop() {
	lock();
	_pending.active = false;
	unlock();
	if (_ready) _out->stop(_channel);
}

bool Speech::playing() const { return _ready && _out->playing(_channel); }

float Speech::playSync(u32 lineHash, float gain, float pan, bool takeOver) {
	if (!_ready) return 0.0f;
	const bool busy = !takeOver && playing();
	if (!busy) _out->stop(_channel);
	Pak* pak = nullptr;
	const PakEntry* e = nullptr;
	for (u32 k = 0; k < _pakCount && !e; ++k)
		if ((e = _paks[k].find(speechEntryHash(lineHash))) != nullptr) pak = &_paks[k];
	if (!e || e->size <= sizeof(SoundHeader) || e->codec != PAK_RAW) {
		++_missing;
		return 0.0f;
	}
	u8* blob = static_cast<u8*>(pak->readAlloc(*e));
	if (!blob) {
		++_broken;
		return 0.0f;
	}
	SoundHeader h;
	memcpy(&h, blob, sizeof(h));
	if (!speechValid(h, e->size)) {
		free(blob);
		++_broken;
		return 0.0f;
	}
	const float seconds = float(h.frames) / float(h.rate);
	const float level = clamp(gain, 0.0f, 1.0f) * _volume;
	if (busy) { // (the one sounding keeps its memory)
		free(blob);
		return seconds;
	}
	// The block used before the last one is surely silent by now.
	_cur ^= 1u;
	if (_block[_cur]) {
		_out->freeSamples(_block[_cur]);
		_block[_cur] = nullptr;
	}
	if (level < 0.005f) { // muted: only its length matters
		free(blob);
		return seconds;
	}
	const u32 bytes = h.frames * 2u;
	i16* pcm = static_cast<i16*>(_out->allocSamples(bytes));
	if (!pcm) {
		free(blob);
		++_broken;
		return seconds;
	}
	if (h.format == SOUND_IMA4)
		imaDecode(blob + sizeof(SoundHeader), h.frames, pcm);
	else
		memcpy(pcm, blob + sizeof(SoundHeader), bytes);
	free(blob);
	_out->publishSamples(pcm, bytes);
	_block[_cur] = pcm;
	_blockBytes[_cur] = bytes;
	// Equal power across the ears (the sound engine's way).
	const float angle = (clamp(pan, -1.0f, 1.0f) + 1.0f) * (3.14159265f * 0.25f);
	_out->start(_channel, pcm, h.frames, h.rate, false, clamp(level * cosf(angle) * 1.4142135f, 0.0f, 1.0f),
	            clamp(level * sinf(angle) * 1.4142135f, 0.0f, 1.0f), 1.0f);
	++_played;
	return seconds;
}

// ---- fix204: lines read and decoded ahead, on a thread of their own ----

const PakEntry* Speech::findEntry(Pak* paks, u32 lineHash, Pak** which) const {
	const u32 entry = speechEntryHash(lineHash);
	for (u32 k = 0; k < _pakCount; ++k)
		if (const PakEntry* e = paks[k].find(entry)) {
			*which = &paks[k];
			return e;
		}
	return nullptr;
}

void Speech::threadMain(void* self) { static_cast<Speech*>(self)->work(); }

void Speech::work() {
	for (;;) {
		lock();
		Slot* job = nullptr;
		while (!_quit) {
			// The line being waited for first, then the one queued first (they are said in that order).
			for (Slot& s : _slots) {
				if (s.state != SL_QUEUED) continue;
				if (_pending.active && s.hash == _pending.hash) {
					job = &s;
					break;
				}
				if (!job || s.stamp < job->stamp) job = &s;
			}
			if (job) break;
			unlock();
			sys::signalWait(_wake);
			lock();
		}
		if (_quit) {
			unlock();
			return;
		}
		job->state = SL_READING;
		const u32 hash = job->hash;
		unlock();

		// The card and the decoding: nothing here touches the sound memory (its allocator is the frame's).
		Pak* pak = nullptr;
		const PakEntry* e = findEntry(_workerPaks, hash, &pak);
		i16* pcm = nullptr;
		SoundHeader h {};
		u32 bytes = 0;
		bool noRoom = false;
		bool ok = e && e->size > sizeof(SoundHeader) && e->codec == PAK_RAW;
		u8* blob = ok ? static_cast<u8*>(pak->readAlloc(*e)) : nullptr;
		if (blob) {
			memcpy(&h, blob, sizeof(h));
			ok = speechValid(h, e->size);
		} else {
			ok = false;
		}
		if (ok) {
			bytes = h.frames * 2u;
			// Room among what waits: the oldest line nobody has asked for yet goes.
			lock();
			while (_slotBytes + bytes > BUDGET) {
				Slot* oldest = nullptr;
				for (Slot& s : _slots)
					if (s.state == SL_READY && s.hash != _pending.hash && (!oldest || s.stamp < oldest->stamp)) oldest = &s;
				if (!oldest) break;
				free(oldest->pcm);
				_slotBytes -= oldest->bytes;
				*oldest = Slot();
			}
			// The line being said now is read whatever the budget says.
			const bool urgent = _pending.active && _pending.hash == hash;
			ok = urgent || _slotBytes + bytes <= BUDGET;
			if (ok) _slotBytes += bytes; // (reserved before the allocation, so the budget holds whatever the frame does)
			else noRoom = true;
			unlock();
		}
		if (ok) {
			pcm = static_cast<i16*>(malloc(bytes));
			if (pcm) {
				if (h.format == SOUND_IMA4)
					imaDecode(blob + sizeof(SoundHeader), h.frames, pcm);
				else
					memcpy(pcm, blob + sizeof(SoundHeader), bytes);
			}
		}
		free(blob);

		lock();
		if (ok && !pcm) _slotBytes -= bytes;
		if (pcm) {
			job->pcm = pcm;
			job->frames = h.frames;
			job->rate = h.rate;
			job->bytes = bytes;
			job->state = SL_READY;
		} else if (noRoom || (ok && !pcm)) {
			*job = Slot(); // (no room now: read again when it is said)
		} else {
			job->state = SL_FAILED;
		}
		unlock();
	}
}

// Lock held. A slot for the line (an old one is reused), queued for the thread.
void Speech::queueLocked(u32 hash) {
	for (Slot& s : _slots)
		if (s.state != SL_EMPTY && s.hash == hash) return; // (keeps its place in the order)
	Slot* free_ = nullptr;
	for (Slot& s : _slots)
		if (s.state == SL_EMPTY) {
			free_ = &s;
			break;
		}
	if (!free_) {
		// Every slot taken: a failed one, else the oldest finished one, gives way; lines being read or waiting to be
		// read keep theirs (then this one is read when it is said).
		for (Slot& s : _slots) {
			if (s.state != SL_FAILED && s.state != SL_READY) continue;
			if (s.hash == _pending.hash && _pending.active) continue;
			if (!free_ || s.state == SL_FAILED || (free_->state != SL_FAILED && s.stamp < free_->stamp)) free_ = &s;
		}
		// The line being waited for takes the place of the one queued last.
		if (!free_ && _pending.active && _pending.hash == hash)
			for (Slot& s : _slots)
				if (s.state == SL_QUEUED && (!free_ || s.stamp > free_->stamp)) free_ = &s;
		if (!free_) return;
		free(free_->pcm);
		_slotBytes -= free_->bytes;
		*free_ = Slot();
	}
	free_->hash = hash;
	free_->state = SL_QUEUED;
	free_->stamp = ++_stamp;
}

void Speech::prefetch(u32 lineHash) {
	if (!_ready || !_thread || !lineHash) return;
	lock();
	queueLocked(lineHash);
	unlock();
	sys::signalWake(_wake);
}

// Takes the line's samples if they are there (the caller frees out.pcm).
Speech::TakeResult Speech::take(u32 hash, Slot& out) {
	TakeResult r = TAKE_NONE;
	lock();
	for (Slot& s : _slots) {
		if (s.state == SL_EMPTY || s.hash != hash) continue;
		if (s.state == SL_READY) {
			out = s;
			_slotBytes -= s.bytes;
			s = Slot();
			r = TAKE_READY;
		} else if (s.state == SL_FAILED) {
			s = Slot();
			r = TAKE_FAILED;
		} else {
			r = TAKE_WAIT;
		}
		break;
	}
	unlock();
	return r;
}

// Into the sound memory and onto the channel (the frame's thread: the sound memory's allocator is not shared).
void Speech::startSamples(const i16* pcm, u32 frames, u32 rate, float left, float right) {
	_cur ^= 1u;
	if (_block[_cur]) {
		_out->freeSamples(_block[_cur]);
		_block[_cur] = nullptr;
	}
	const u32 bytes = frames * 2u;
	i16* dst = static_cast<i16*>(_out->allocSamples(bytes));
	if (!dst) {
		++_broken;
		return;
	}
	memcpy(dst, pcm, bytes);
	_out->publishSamples(dst, bytes);
	_block[_cur] = dst;
	_blockBytes[_cur] = bytes;
	_out->start(_channel, dst, frames, rate, false, left, right, 1.0f);
	++_played;
}

float Speech::play(u32 lineHash, float gain, float pan, bool takeOver) {
	if (!_ready) return 0.0f;
	if (!_thread) return playSync(lineHash, gain, pan, takeOver);
	const bool busy = !takeOver && playing();
	if (!busy) {
		_out->stop(_channel);
		lock();
		_pending.active = false;
		unlock();
	}
	// Its length now (the subtitle stays up that long): only the header, one small read.
	Pak* pak = nullptr;
	const PakEntry* e = findEntry(_paks, lineHash, &pak);
	if (!e || e->size <= sizeof(SoundHeader) || e->codec != PAK_RAW) {
		++_missing;
		return 0.0f;
	}
	SoundHeader h;
	if (!pak->readRange(*e, 0, &h, sizeof(h)) || !speechValid(h, e->size)) {
		++_broken;
		return 0.0f;
	}
	const float seconds = float(h.frames) / float(h.rate);
	if (busy) return seconds; // (only measured; a recording read ahead for it stays for later)
	const float level = clamp(gain, 0.0f, 1.0f) * _volume;
	if (level < 0.005f) { // muted: only its length matters
		Slot drop;
		if (take(lineHash, drop) == TAKE_READY) free(drop.pcm);
		return seconds;
	}
	// Equal power across the ears (the sound engine's way).
	const float angle = (clamp(pan, -1.0f, 1.0f) + 1.0f) * (3.14159265f * 0.25f);
	const float left = clamp(level * cosf(angle) * 1.4142135f, 0.0f, 1.0f);
	const float right = clamp(level * sinf(angle) * 1.4142135f, 0.0f, 1.0f);
	Slot got;
	switch (take(lineHash, got)) {
	case TAKE_READY:
		++_hits;
		startSamples(got.pcm, got.frames, got.rate, left, right);
		free(got.pcm);
		return seconds;
	case TAKE_FAILED:
		++_broken;
		return seconds;
	case TAKE_NONE:
	case TAKE_WAIT:
		break;
	}
	// Not there yet: it starts in update() the moment it is (marked first, so the thread reads it whatever its
	// budget says).
	++_late;
	lock();
	_pending.active = true;
	_pending.hash = lineHash;
	_pending.left = left;
	_pending.right = right;
	_pending.since = sys::timeMs();
	unlock();
	prefetch(lineHash);
	return seconds;
}

void Speech::update() {
	if (!_thread) return;
	lock();
	const Pending p = _pending;
	unlock();
	if (!p.active) return;
	bool done = false;
	Slot got;
	switch (take(p.hash, got)) {
	case TAKE_READY:
		done = true;
		// (Much later than its subtitle it would only talk over the next line.)
		if (sys::timeMs() - p.since < 1500.0) startSamples(got.pcm, got.frames, got.rate, p.left, p.right);
		free(got.pcm);
		break;
	case TAKE_FAILED:
		done = true;
		++_broken;
		break;
	case TAKE_NONE: // (pushed out meanwhile: ask again)
		prefetch(p.hash);
		break;
	case TAKE_WAIT:
		break;
	}
	if (done || sys::timeMs() - p.since > 3000.0) {
		lock();
		if (_pending.hash == p.hash) _pending.active = false;
		unlock();
	}
}

void Speech::writeReport(FILE* f) const {
	if (!_ready) {
		if (_unusable)
			fprintf(f, "voices: off (speech.pak is there but cannot be opened: damaged, or over 2 GiB, which the console cannot seek in - bake again, the baker splits it)\n");
		else
			fprintf(f, "voices: off (no speech.pak beside data.pak)\n");
		return;
	}
	u32 recordings = 0;
	for (u32 k = 0; k < _pakCount; ++k) recordings += _paks[k].count();
	fprintf(f, "voices: %u recordings in %u archive(s); played %u, lines without a recording %u, damaged %u\n", unsigned(recordings),
	        unsigned(_pakCount), unsigned(_played), unsigned(_missing), unsigned(_broken));
	// fix204: read ahead (ready when said) / had to wait for the card.
	fprintf(f, "voices read ahead: %s, ready %u, waited %u\n", _thread ? "thread" : "off", unsigned(_hits), unsigned(_late));
}

} // namespace g3
