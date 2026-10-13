#include "music.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace g3 {
namespace {

void copyStr(char* dst, u32 size, const char* src) {
	if (!size) return;
	u32 n = 0;
	if (src)
		for (; src[n] && n + 1 < size; ++n) dst[n] = src[n];
	dst[n] = 0;
}

constexpr float FADE_OUT = 1.2f, FADE_IN = 1.5f;
// Into or out of danger the change is quick (the original changes at the next bar, with a fill or a break).
constexpr float FADE_OUT_FAST = 0.4f, FADE_IN_FAST = 0.6f;
// A change of theme is looked for at most this often (OpenGothic: every five seconds).
constexpr float CHOICE_EVERY = 1.0f;
// By day: OpenGothic's gtime(4,0) <= time <= gtime(21,0).
constexpr float DAY_BEGINS = 4.0f, DAY_ENDS = 21.0f;

} // namespace

bool Music::init(const char* musicPakPath, ScriptVm* vm, AudioOut* out, bool threaded) {
	shutdown();
	if (!musicPakPath || !out) return false;
	if (!_pak.open(musicPakPath)) return false;
	_vm = vm;
	_out = out;
	_classTheme = vm ? vm->find("C_MUSICTHEME") : SCRIPT_NONE;
	if (_classTheme != SCRIPT_NONE && vm->symbol(_classTheme).type != SCRIPT_CLASS) _classTheme = SCRIPT_NONE;
	_mFile = vm ? vm->findMember("C_MUSICTHEME", "FILE") : SCRIPT_NONE;
	_mVol = vm ? vm->findMember("C_MUSICTHEME", "VOL") : SCRIPT_NONE;
	if (_mFile == SCRIPT_NONE) _classTheme = SCRIPT_NONE;
	for (Resolved& r : _resolved) r = Resolved {};
	_triedCount = 0;
	_key = 0xFFFFFFFFu;
	_wanted = _current = 0;
	_gain = 0;
	_system[0] = 0;
	_zoneIdx = _defaultIdx = -1;
	_sinceChoice = 0;
	_situation = MUSIC_CALM;
	_themeName[0] = _zoneName[0] = 0;
	_lock = sys::mutexCreate();
	_wake = sys::signalCreate();
	_quit = false;
	_threaded = false;
	if (threaded && _lock && _wake && _loaderPak.open(_pak.path())) {
		_thread = sys::threadStart(&Music::threadMain, this);
		_threaded = _thread != nullptr;
		if (!_threaded) _loaderPak.close();
	}
	_ready = true;
	return true;
}

void Music::shutdown() {
	if (_thread) {
		sys::mutexLock(_lock);
		_quit = true;
		sys::mutexUnlock(_lock);
		sys::signalWake(_wake);
		sys::threadJoin(_thread);
		_thread = nullptr;
	}
	_loaderPak.close();
	if (_out) stopTrack();
	free(_zones);
	_zones = nullptr;
	_zoneCount = 0;
	if (_lock) sys::mutexDestroy(_lock);
	if (_wake) sys::signalDestroy(_wake);
	_lock = nullptr;
	_wake = nullptr;
	_pak.close();
	_out = nullptr;
	_vm = nullptr;
	_ready = false;
	_threaded = false;
}

void Music::threadMain(void* self) { static_cast<Music*>(self)->work(); }

// ---- zones ----

void Music::enterWorld(Pak& pak, const char* worldAsset) {
	free(_zones);
	_zones = nullptr;
	_zoneCount = 0;
	_zoneIdx = _defaultIdx = -1;
	_key = 0xFFFFFFFFu;
	_zoneName[0] = 0;
	if (!_ready || !worldAsset) return;
	char name[96];
	snprintf(name, sizeof(name), "%s/music", worldAsset);
	const PakEntry* e = pak.find(name);
	if (!e || e->size < sizeof(MusicZonesHeader)) return;
	u8* blob = static_cast<u8*>(pak.readAlloc(*e));
	if (!blob) return;
	MusicZonesHeader h;
	memcpy(&h, blob, sizeof(h));
	if (h.magic != MUSIC_ZONES_MAGIC || h.version != MUSIC_ZONES_VERSION || h.count == 0 || h.count > u32(ZONES) ||
	    e->size != sizeof(MusicZonesHeader) + h.count * sizeof(MusicZone)) {
		free(blob);
		return;
	}
	_zones = static_cast<MusicZone*>(malloc(h.count * sizeof(MusicZone)));
	if (_zones) {
		memcpy(_zones, blob + sizeof(MusicZonesHeader), h.count * sizeof(MusicZone));
		_zoneCount = h.count;
		for (u32 i = 0; i < _zoneCount; ++i) {
			_zones[i].name[sizeof(_zones[i].name) - 1] = 0;
			if (_zones[i].isDefault && _defaultIdx < 0) _defaultIdx = int(i);
		}
	}
	free(blob);
}

// OpenGothic: the zone he is in stays as long as he is inside it; else the last of the zones that hold him; else
// the default one. (A zone is its box: the original's priority and ellipsoids are not looked at.)
int Music::zoneAt(Vec3 p) {
	const auto inside = [&](const MusicZone& z) {
		return !z.isDefault && z.bbMin[0] <= p.x && p.x < z.bbMax[0] && z.bbMin[1] <= p.y && p.y < z.bbMax[1] && z.bbMin[2] <= p.z &&
		       p.z < z.bbMax[2];
	};
	if (_zoneIdx >= 0 && u32(_zoneIdx) < _zoneCount && inside(_zones[_zoneIdx])) return _zoneIdx;
	int found = -1;
	for (u32 i = 0; i < _zoneCount; ++i)
		if (inside(_zones[i])) found = int(i);
	_zoneIdx = found >= 0 ? found : _defaultIdx;
	return _zoneIdx;
}

void Music::setSystemTheme(const char* name) {
	char next[sizeof(_system)];
	copyStr(next, sizeof(next), name);
	if (strcmp(next, _system) == 0) return;
	copyStr(_system, sizeof(_system), next);
	_key = 0xFFFFFFFEu; // (chosen again at once)
	_sinceChoice = CHOICE_EVERY;
}

// ---- themes ----

// The track a theme name leads to: the entry's hash, 0 for none.
u32 Music::resolve(const char* name, float& volume) {
	const u32 h = hashName(name);
	u32 at = h % THEMES;
	for (u32 probe = 0; probe < u32(THEMES); ++probe, at = (at + 1) % THEMES) {
		Resolved& r = _resolved[at];
		if (r.used) {
			if (r.name == h) {
				volume = r.volume;
				return r.entry;
			}
			continue;
		}
		r.used = true;
		r.name = h;
		r.volume = 1.0f;
		r.entry = 0;
		char file[64] = "";
		if (_classTheme != SCRIPT_NONE) {
			const u32 sym = _vm->findHash(h);
			if (sym != SCRIPT_NONE && _vm->symbol(sym).type == SCRIPT_INSTANCE && _vm->classOf(sym) == _classTheme) {
				if (ScriptInstance* inst = _vm->create(sym)) {
					copyStr(file, sizeof(file), _vm->getString(_mFile, 0, inst));
					if (_mVol != SCRIPT_NONE) {
						const float v = _vm->getFloat(_mVol, 0, inst);
						r.volume = v > 0.0f ? (v > 1.5f ? min(v / 100.0f, 1.0f) : min(v, 1.0f)) : 1.0f;
					}
					_vm->destroy(inst);
				}
			}
		}
		if (file[0] && _pak.find(musicEntryHash(file))) r.entry = musicEntryHash(file);
		else if (_pak.find(musicEntryHash(name))) r.entry = musicEntryHash(name);
		volume = r.volume;
		return r.entry;
	}
	volume = 1.0f;
	return 0;
}

u32 Music::tryTheme(const char* zone, bool day, const char* sit, char* nameOut, u32 size, float& volume) {
	char name[96];
	snprintf(name, sizeof(name), "%s_%s_%s", zone, day ? "DAY" : "NGT", sit);
	if (const u32 e = resolve(name, volume)) {
		copyStr(nameOut, size, name);
		return e;
	}
	if (_triedCount < u32(TRIED)) {
		bool seen = false;
		for (u32 i = 0; i < _triedCount; ++i) seen |= strcmp(_tried[i], name) == 0;
		if (!seen) copyStr(_tried[_triedCount++], sizeof(_tried[0]), name);
	}
	return 0;
}

// OpenGothic's search: the zone's own themes, then the default zone's; by the hour, then by day; the situation,
// then (for a threat) the fight's, then the calm one.
u32 Music::choose(const char* zone, const char* defaultZone, bool day, MusicSituation situation, char* nameOut, u32 size, float& volume) {
	static const char* const kSit[3] = {"STD", "THR", "FGT"};
	const char* sits[3] = {kSit[situation], situation == MUSIC_THREAT ? kSit[MUSIC_FIGHT] : kSit[MUSIC_CALM], kSit[MUSIC_CALM]};
	const char* zones[2] = {zone, defaultZone};
	for (const char* z : zones) {
		if (!z || !*z) continue;
		for (int d = 0; d < 2; ++d) {
			const bool dayTry = d == 0 ? day : true;
			if (d == 1 && day) break;
			for (const char* sit : sits)
				if (const u32 e = tryTheme(z, dayTry, sit, nameOut, size, volume)) return e;
		}
	}
	// What a game of its own may have instead: "OC_STD", "OC_FGT", "OC".
	for (const char* z : zones) {
		if (!z || !*z) continue;
		char name[96];
		for (const char* sit : sits) {
			snprintf(name, sizeof(name), "%s_%s", z, sit);
			if (const u32 e = resolve(name, volume)) {
				copyStr(nameOut, size, name);
				return e;
			}
		}
		if (const u32 e = resolve(z, volume)) {
			copyStr(nameOut, size, z);
			return e;
		}
	}
	nameOut[0] = 0;
	return 0;
}

// ---- the stream ----

bool Music::startTrack(u32 entryHash, float volume) {
	stopTrack();
	const PakEntry* e = _pak.find(entryHash);
	if (!e || e->size < sizeof(MusicHeader)) return false;
	MusicHeader h;
	if (!_pak.readRange(*e, 0, &h, sizeof(h)) || !musicValid(h, e->size)) return false;
	if (!_out->streamOpen(h.rate, h.channels)) return false;
	_entry = *e;
	_head = h;
	_chunkFrames = h.rate;
	_chunkBytes = _chunkFrames * h.channels * 2u;
	for (u32 i = 0; i < u32(AUDIO_STREAM_SLOTS); ++i) {
		_blocks[i] = _out->allocSamples(_chunkBytes);
		if (!_blocks[i]) {
			for (u32 k = 0; k < i; ++k) {
				_out->freeSamples(_blocks[k]);
				_blocks[k] = nullptr;
			}
			_out->streamClose();
			return false;
		}
	}
	sys::mutexLock(_lock);
	++_generation;
	for (u32 i = 0; i < u32(AUDIO_STREAM_SLOTS); ++i) _slotState[i] = SLOT_EMPTY;
	_fillNext = _queueNext = 0;
	_position = 0;
	_open = true;
	_failed = false;
	sys::mutexUnlock(_lock);
	_currentVolume = volume;
	_out->streamVolume(0.0f);
	sys::signalWake(_wake);
	return true;
}

void Music::stopTrack() {
	if (!_open) return;
	sys::mutexLock(_lock);
	_open = false;
	++_generation;
	sys::mutexUnlock(_lock);
	_out->streamClose();
	for (u32 i = 0; i < u32(AUDIO_STREAM_SLOTS); ++i) {
		if (_blocks[i]) _out->freeSamples(_blocks[i]);
		_blocks[i] = nullptr;
		_slotState[i] = SLOT_EMPTY;
	}
}

// Main thread: queues the chunks the loader has read, takes back the ones that were played.
void Music::feed() {
	if (!_open) return;
	sys::mutexLock(_lock);
	bool freed = false;
	for (u32 i = 0; i < u32(AUDIO_STREAM_SLOTS); ++i)
		if (_slotState[i] == SLOT_QUEUED && _out->streamDone(i)) {
			_slotState[i] = SLOT_EMPTY;
			freed = true;
		}
	while (_slotState[_queueNext] == SLOT_READY) {
		if (!_out->streamQueue(_queueNext, static_cast<const i16*>(_blocks[_queueNext]), _slotFrames[_queueNext])) break;
		_slotState[_queueNext] = SLOT_QUEUED;
		_queueNext = (_queueNext + 1) % AUDIO_STREAM_SLOTS;
	}
	sys::mutexUnlock(_lock);
	if (freed) {
		if (_threaded) sys::signalWake(_wake);
	}
}

// Reads the next chunk into the next empty slot. False if there was nothing to do.
static bool readChunk(Pak& pak, const PakEntry& entry, const MusicHeader& h, void* dst, u32 chunkFrames, u32& position,
                      u32& framesOut) {
	const u32 loopEnd = h.loopEnd ? h.loopEnd : h.frames;
	const u32 loopStart = h.loopEnd ? h.loopStart : 0;
	if (position >= loopEnd) position = loopStart;
	u32 n = loopEnd - position;
	if (n > chunkFrames) n = chunkFrames;
	if (n == 0) return false;
	const u32 fb = h.channels * 2u;
	if (!pak.readRange(entry, u32(sizeof(MusicHeader)) + position * fb, dst, n * fb)) return false;
	position += n;
	framesOut = n;
	return true;
}

void Music::work() {
	for (;;) {
		sys::mutexLock(_lock);
		if (_quit) {
			sys::mutexUnlock(_lock);
			return;
		}
		bool did = false;
		if (_open && !_failed && _slotState[_fillNext] == SLOT_EMPTY) {
			const u32 slot = _fillNext, gen = _generation;
			u32 pos = _position;
			const PakEntry entry = _entry;
			const MusicHeader head = _head;
			void* block = _blocks[slot];
			const u32 cf = _chunkFrames;
			sys::mutexUnlock(_lock);
			u32 frames = 0;
			const bool ok = readChunk(_loaderPak, entry, head, block, cf, pos, frames);
			if (ok) _out->publishSamples(block, frames * head.channels * 2u);
			sys::mutexLock(_lock);
			if (gen == _generation && _open) {
				if (ok) {
					_slotFrames[slot] = frames;
					_slotState[slot] = SLOT_READY;
					_fillNext = (_fillNext + 1) % AUDIO_STREAM_SLOTS;
					_position = pos;
				} else {
					_failed = true;
				}
			}
			did = true;
		}
		sys::mutexUnlock(_lock);
		if (!did) sys::signalWait(_wake);
	}
}

// ---- every frame ----

void Music::update(float dt, Vec3 listener, float hours, MusicSituation situation) {
	if (!_ready) return;
	_sinceChoice += dt;
	const bool day = hours >= DAY_BEGINS && hours <= DAY_ENDS;
	// Which theme.
	u32 key;
	int zone = -1;
	if (_system[0]) {
		key = 0x80000000u | (hashName(_system) & 0x7FFFFFFFu);
	} else {
		zone = zoneAt(listener);
		key = u32(zone + 1) | (day ? 0x10000u : 0u) | (u32(situation) << 17);
	}
	if (key != _key && _volume > 0.0f && (_sinceChoice >= CHOICE_EVERY || _key == 0xFFFFFFFFu)) {
		_key = key;
		_sinceChoice = 0;
		// Into or out of danger: quick; the rest of the changes slowly.
		const bool danger = situation != _situation && !_system[0];
		_fadeOutLen = danger ? FADE_OUT_FAST : FADE_OUT;
		_fadeInLen = danger ? FADE_IN_FAST : FADE_IN;
		_situation = situation;
		char name[48] = "";
		float vol = 1.0f;
		if (_system[0]) {
			_zoneName[0] = 0;
			_wanted = resolve(_system, vol);
			copyStr(name, sizeof(name), _system);
			_wantedVolume = vol;
		} else if (zone >= 0) {
			copyStr(_zoneName, sizeof(_zoneName), _zones[zone].name);
			const char* defName = _defaultIdx >= 0 && _defaultIdx != zone ? _zones[_defaultIdx].name : nullptr;
			_wanted = choose(_zones[zone].name, defName, day, situation, name, sizeof(name), vol);
			_wantedVolume = vol; // (the zone's own volume is not used: OpenGothic does not either)
		} else {
			_zoneName[0] = 0;
			_wanted = 0;
		}
		if (_wanted != _current) copyStr(_themeName, sizeof(_themeName), name);
	}
	if (_volume <= 0.0f) {
		_wanted = 0;
		_key = 0xFFFFFFFEu; // (when the volume comes back the theme is chosen again at once)
		_sinceChoice = CHOICE_EVERY;
	}

	// Fade.
	if (_wanted != _current) {
		if (_current && _gain > 0.0f) {
			_gain = max(0.0f, _gain - dt / _fadeOutLen);
		} else {
			stopTrack();
			_current = 0;
			_gain = 0;
			if (_wanted) {
				if (startTrack(_wanted, _wantedVolume)) _current = _wanted;
				else _wanted = 0;
			}
			if (!_wanted) _themeName[0] = 0;
		}
	} else if (_current) {
		_gain = min(1.0f, _gain + dt / _fadeInLen);
	}

	if (_open) {
		if (!_threaded) {
			// No thread: read one chunk here.
			sys::mutexLock(_lock);
			const bool want = !_failed && _slotState[_fillNext] == SLOT_EMPTY;
			const u32 slot = _fillNext;
			sys::mutexUnlock(_lock);
			if (want) {
				u32 frames = 0, pos = _position;
				if (readChunk(_pak, _entry, _head, _blocks[slot], _chunkFrames, pos, frames)) {
					_out->publishSamples(_blocks[slot], frames * _head.channels * 2u);
					_slotFrames[slot] = frames;
					_slotState[slot] = SLOT_READY;
					_fillNext = (_fillNext + 1) % AUDIO_STREAM_SLOTS;
					_position = pos;
				} else {
					_failed = true;
				}
			}
		}
		feed();
		_out->streamVolume(_gain * _volume * _currentVolume);
	}
}

void Music::statusLine(char* out, u32 size) const {
	if (!_ready) {
		copyStr(out, size, "music: -");
		return;
	}
	snprintf(out, size, "music: %s %s %d%%", _zoneName[0] ? _zoneName : "-", _themeName[0] ? _themeName : "-", int(_gain * 100));
}

void Music::writeReport(FILE* f) const {
	if (!f) return;
	if (!_ready) {
		fprintf(f, "music: off\n");
		return;
	}
	fprintf(f, "music: zones %u, theme names tried and not found:\n", unsigned(_zoneCount));
	for (u32 i = 0; i < _triedCount; ++i) fprintf(f, "  %s\n", _tried[i]);
}

} // namespace g3
