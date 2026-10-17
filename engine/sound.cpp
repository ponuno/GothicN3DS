#include "sound.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace g3 {

namespace {

constexpr float DEFAULT_RANGE = 3000.0f; // cm a sound carries when nobody says
constexpr float MIN_AUDIBLE = 0.01f;     // quieter than this is not started
constexpr float WAIT_FOR_SAMPLE = 0.35f; // seconds a one-shot waits for its sample
constexpr float PI_F = 3.14159265f;

// Quieter with distance: full close by, nothing at `range`, in between a curve
// that falls fast at first and slowly at the end.
float attenuation(float distance, float range) {
	if (range <= 1.0f) return 1.0f;
	const float x = distance / range;
	if (x >= 1.0f) return 0.0f;
	const float inv = 1.0f - x;
	return inv * sqrtf(inv);
}

void copyName(char* dst, u32 size, const char* src) {
	u32 i = 0;
	for (; src && src[i] && i + 1 < size; ++i) dst[i] = src[i];
	dst[i] = 0;
}

} // namespace

float Sound::random01() {
	_rng ^= _rng << 13;
	_rng ^= _rng >> 17;
	_rng ^= _rng << 5;
	return float(_rng & 0xFFFFFFu) / float(0x1000000u);
}

// ---- start and stop ----

bool Sound::init(Pak& pak, ScriptVm* vm, AudioOut* out, u32 cacheBytes, bool threaded) {
	shutdown();
	if (!vm || !out || out->channels() == 0) return false;
	_pak = &pak;
	_vm = vm;
	_out = out;
	_cacheLimit = cacheBytes;
	_cacheBytes = 0;
	_stats = SoundStats {};
	_stats.cacheLimit = cacheBytes;
	_tick = 0;
	_logCount = 0;
	for (Def& d : _defs) d = Def {};
	for (Sample& s : _samples) s = Sample {};
	for (Voice& v : _voices) v = Voice {};
	for (Pending& p : _pending) p = Pending {};
	for (Ticket& t : _tickets) t = Ticket {};
	_classSfx = vm->find("C_SFX");
	if (_classSfx != SCRIPT_NONE && vm->symbol(_classSfx).type != SCRIPT_CLASS) _classSfx = SCRIPT_NONE;
	_mFile = vm->findMember("C_SFX", "FILE");
	_mVol = vm->findMember("C_SFX", "VOL");
	_mLoop = vm->findMember("C_SFX", "LOOP");
	_mPitchOff = vm->findMember("C_SFX", "PITCHOFF");
	_mPitchVar = vm->findMember("C_SFX", "PITCHVAR");
	if (_mFile == SCRIPT_NONE) _classSfx = SCRIPT_NONE;
	_lock = sys::mutexCreate();
	_wake = sys::signalCreate();
	_quit = false;
	_queueCount = 0;
	_threaded = false;
	if (threaded && _lock && _wake && _loaderPak.open(pak.path())) {
		// fix204: set before the thread starts. work() reads it at once (on the New 3DS on the other core): seeing
		// false it either quit at once (no sample was ever loaded again) or read through the main thread's archive.
		_threaded = true;
		_thread = sys::threadStart(&Sound::threadMain, this);
		if (!_thread) {
			_threaded = false;
			_loaderPak.close();
		}
	}
	return true;
}

void Sound::shutdown() {
	if (_thread) {
		sys::mutexLock(_lock);
		_quit = true;
		sys::mutexUnlock(_lock);
		sys::signalWake(_wake);
		sys::threadJoin(_thread);
		_thread = nullptr;
	}
	_loaderPak.close();
	if (_out) {
		for (u32 i = 0; i < u32(VOICES); ++i)
			if (_voices[i].active) _out->stop(i);
		for (Sample& s : _samples)
			if (s.block) _out->freeSamples(s.block);
	}
	for (Sample& s : _samples) s = Sample {};
	for (Voice& v : _voices) v = Voice {};
	free(_ambient);
	free(_emitters);
	_ambient = nullptr;
	_emitters = nullptr;
	_ambientCount = 0;
	if (_lock) sys::mutexDestroy(_lock);
	if (_wake) sys::signalDestroy(_wake);
	_lock = nullptr;
	_wake = nullptr;
	_out = nullptr;
	_vm = nullptr;
	_pak = nullptr;
	_cacheBytes = 0;
}

void Sound::threadMain(void* self) { static_cast<Sound*>(self)->work(); }

// ---- samples ----

// Frees the oldest unused samples until `bytes` more fit. The lock is held.
bool Sound::makeRoom(u32 bytes) {
	if (bytes > _cacheLimit) return false;
	while (_cacheBytes + bytes > _cacheLimit) {
		int oldest = -1;
		for (int i = 0; i < SAMPLES; ++i) {
			const Sample& s = _samples[i];
			if (s.state != S_READY || s.pins > 0) continue;
			if (oldest < 0 || s.used < _samples[oldest].used) oldest = i;
		}
		if (oldest < 0) return false;
		Sample& s = _samples[oldest];
		_out->freeSamples(s.block);
		_cacheBytes -= s.bytes;
		s = Sample {};
		++_stats.evicted;
	}
	return true;
}

// The slot of the sample with this hash; makes one (and, if `queue`, sets its
// loading going) if there is none. -1 if every slot is in use.
int Sound::findSample(u32 hash, bool queue) {
	sys::mutexLock(_lock);
	int found = -1, spare = -1;
	for (int i = 0; i < SAMPLES; ++i) {
		const Sample& s = _samples[i];
		if (s.state != S_EMPTY && s.hash == hash) {
			found = i;
			break;
		}
		if (s.state == S_EMPTY) {
			if (spare < 0) spare = i;
		} else if (s.state == S_MISSING || (s.state == S_READY && s.pins == 0)) {
			if (spare < 0 || (_samples[spare].state != S_EMPTY && s.used < _samples[spare].used)) spare = i;
		}
	}
	if (found >= 0) {
		_samples[found].used = _tick;
		sys::mutexUnlock(_lock);
		return found;
	}
	if (spare < 0 || !queue) {
		sys::mutexUnlock(_lock);
		return -1;
	}
	Sample& s = _samples[spare];
	if (s.state == S_READY) {
		_out->freeSamples(s.block);
		_cacheBytes -= s.bytes;
		++_stats.evicted;
	}
	s = Sample {};
	s.hash = hash;
	s.used = _tick;
	const PakEntry* e = _pak->find(hash);
	if (!e || e->type != ASSET_SOUND || e->size < sizeof(SoundHeader)) {
		s.state = S_MISSING;
		sys::mutexUnlock(_lock);
		return spare;
	}
	s.state = S_QUEUED;
	sys::mutexUnlock(_lock);
	queueLoad(spare);
	return spare;
}

void Sound::queueLoad(int sample) {
	if (!_threaded) {
		// No thread: read it here and now.
		work();
		(void)sample;
		return;
	}
	sys::mutexLock(_lock);
	if (_queueCount < QUEUE) _queue[_queueCount++] = sample;
	sys::mutexUnlock(_lock);
	sys::signalWake(_wake);
}

// The loading thread: takes the queued samples one by one. Without a thread
// the main one calls this to read whatever is queued, and it comes back at once.
void Sound::work() {
	for (;;) {
		sys::mutexLock(_lock);
		if (_quit) {
			sys::mutexUnlock(_lock);
			return;
		}
		int idx = -1;
		if (_threaded) {
			if (_queueCount > 0) {
				idx = _queue[0];
				for (u32 i = 1; i < _queueCount; ++i) _queue[i - 1] = _queue[i];
				--_queueCount;
			}
		} else {
			for (int i = 0; i < SAMPLES; ++i)
				if (_samples[i].state == S_QUEUED) {
					idx = i;
					break;
				}
		}
		if (idx < 0) {
			sys::mutexUnlock(_lock);
			if (!_threaded) return;
			sys::signalWait(_wake);
			continue;
		}
		const u32 hash = _samples[idx].hash;
		sys::mutexUnlock(_lock);

		Pak& pak = _threaded ? _loaderPak : *_pak;
		const PakEntry* e = pak.find(hash);
		void* block = nullptr;
		bool ok = e && e->type == ASSET_SOUND && e->size >= sizeof(SoundHeader);
		if (ok) {
			sys::mutexLock(_lock);
			ok = makeRoom(e->size);
			if (ok) {
				block = _out->allocSamples(e->size);
				if (block) _cacheBytes += e->size;
				ok = block != nullptr;
			}
			sys::mutexUnlock(_lock);
		}
		const bool fits = ok;
		if (ok) ok = pak.read(*e, block);
		SoundHeader h {};
		if (ok) {
			memcpy(&h, block, sizeof(h));
			ok = soundValid(h, e->size);
		}
		sys::mutexLock(_lock);
		Sample& s = _samples[idx];
		if (ok) {
			_out->publishSamples(block, e->size);
			s.block = block;
			s.data = reinterpret_cast<const i16*>(static_cast<const u8*>(block) + sizeof(SoundHeader));
			s.frames = h.frames;
			s.rate = h.rate;
			s.bytes = e->size;
			s.state = S_READY;
			++_stats.loads;
		} else {
			if (block) {
				_out->freeSamples(block);
				_cacheBytes -= e->size;
			}
			// No room is for another time; a damaged or missing sample is for good.
			s.state = fits || !e || e->type != ASSET_SOUND ? S_MISSING : S_EMPTY;
			if (s.state == S_EMPTY) s = Sample {};
		}
		sys::mutexUnlock(_lock);
		if (!_threaded) continue;
	}
}

// ---- names ----

int Sound::defFor(u32 nameHash, const char* name) {
	u32 at = nameHash % NAMES;
	for (u32 probe = 0; probe < NAMES; ++probe, at = (at + 1) % NAMES) {
		Def& d = _defs[at];
		if (d.state == 0) {
			d = Def {};
			d.name = nameHash;
			d.state = 2;
			if (_classSfx != SCRIPT_NONE) {
				const u32 sym = _vm->findHash(nameHash);
				if (sym != SCRIPT_NONE && _vm->symbol(sym).type == SCRIPT_INSTANCE && _vm->classOf(sym) == _classSfx) {
					if (ScriptInstance* inst = _vm->create(sym)) {
						char file[64];
						copyName(file, sizeof(file), _vm->getString(_mFile, 0, inst));
						const i32 vol = _mVol != SCRIPT_NONE ? _vm->getInt(_mVol, 0, inst) : 0;
						d.loop = _mLoop != SCRIPT_NONE && _vm->getInt(_mLoop, 0, inst) != 0;
						d.pitchOff = _mPitchOff != SCRIPT_NONE ? float(_vm->getInt(_mPitchOff, 0, inst)) : 0.0f;
						d.pitchVar = _mPitchVar != SCRIPT_NONE ? float(_vm->getInt(_mPitchVar, 0, inst)) : 0.0f;
						_vm->destroy(inst);
						if (file[0]) {
							d.sample = soundEntryHash(file);
							// (An unset volume is full; the scripts count to 127.)
							d.gain = vol <= 0 ? 1.0f : min(float(vol) / 127.0f, 1.0f);
							d.state = 1;
						}
					}
				}
			}
			// A name that is a sample's own ("sfx/<name>").
			if (d.state != 1 && name && _pak->find(soundEntryHash(name))) {
				d.sample = soundEntryHash(name);
				d.state = 1;
			}
			return int(at);
		}
		if (d.name == nameHash) return int(at);
	}
	return -1;
}

bool Sound::knows(const char* name) {
	if (!ready() || !name) return false;
	const int d = defFor(hashName(name), name);
	return d >= 0 && _defs[d].state == 1;
}

bool Sound::playable(const char* name) {
	if (!ready() || !name || !_pak) return false;
	const int d = defFor(hashName(name), name);
	if (d < 0 || _defs[d].state != 1) return false;
	const u32 hash = _defs[d].sample;
	const PakEntry* e = _pak->find(hash);
	if (e && e->type == ASSET_SOUND && e->size >= sizeof(SoundHeader)) return true;
	// A sample made here (addSynthetic): kept in memory, no archive entry.
	bool made = false;
	sys::mutexLock(_lock);
	for (int i = 0; i < SAMPLES && !made; ++i) made = _samples[i].state == S_READY && _samples[i].hash == hash && _samples[i].pins > 0;
	sys::mutexUnlock(_lock);
	return made;
}

bool Sound::isLoop(const char* name) {
	if (!ready() || !name) return false;
	const int d = defFor(hashName(name), name);
	return d >= 0 && _defs[d].state == 1 && _defs[d].loop;
}

void Sound::note(const char* name, bool absent) {
	for (u32 i = 0; i < _logCount; ++i)
		if (strncmp(_log[i].name, name, sizeof(_log[i].name) - 1) == 0) {
			++_log[i].count;
			return;
		}
	if (_logCount >= u32(LOG)) return;
	Missing& m = _log[_logCount++];
	copyName(m.name, sizeof(m.name), name);
	m.count = 1;
	m.absent = absent;
}

// ---- voices ----

float Sound::loudnessOf(bool located, Vec3 pos, float gain, float range) const {
	if (!located) return gain;
	const Vec3 d = pos - _listener;
	return gain * attenuation(length(d), range > 0.0f ? range : DEFAULT_RANGE);
}

void Sound::mixOf(const Voice& v, float& left, float& right) const {
	float g = v.gain * _master;
	if (!v.located) {
		left = right = clamp(g, 0.0f, 1.0f);
		return;
	}
	const Vec3 d = v.pos - _listener;
	const float dist = length(d);
	g *= attenuation(dist, v.range > 0.0f ? v.range : DEFAULT_RANGE);
	// Left to right across the listener, by the heading: the right hand is (cos, 0, -sin).
	float pan = 0.0f;
	const float flat = sqrtf(d.x * d.x + d.z * d.z);
	if (flat > 1.0f) pan = (d.x * cosf(_yaw) - d.z * sinf(_yaw)) / flat;
	// (Close by there is no side to speak of.)
	pan *= clamp(flat / 250.0f, 0.0f, 1.0f);
	const float angle = (pan + 1.0f) * (PI_F * 0.25f);
	left = clamp(g * cosf(angle) * 1.4142f, 0.0f, 1.0f);
	right = clamp(g * sinf(angle) * 1.4142f, 0.0f, 1.0f);
}

void Sound::release(Voice& v) {
	if (v.sample >= 0) {
		sys::mutexLock(_lock);
		if (_samples[v.sample].pins > 0) --_samples[v.sample].pins;
		sys::mutexUnlock(_lock);
	}
	if (v.ambient >= 0 && u32(v.ambient) < _ambientCount) _emitters[v.ambient].handle = 0;
	const u16 serial = v.serial;
	v = Voice {};
	v.serial = serial;
}

int Sound::pickVoice(float loudness) {
	const u32 usable = min<u32>(u32(VOICES), _out->channels());
	int quietest = -1;
	float quietestLoud = 1e9f;
	for (u32 i = 0; i < usable; ++i) {
		if (!_voices[i].active) return int(i);
		const Voice& v = _voices[i];
		const float l = loudnessOf(v.located, v.pos, v.gain, v.range);
		if (l < quietestLoud) {
			quietestLoud = l;
			quietest = int(i);
		}
	}
	if (quietest >= 0 && quietestLoud < loudness * 0.9f) {
		_out->stop(u32(quietest));
		release(_voices[quietest]);
		++_stats.stolen;
		return quietest;
	}
	return -1;
}

u32 Sound::startNow(int sample, bool located, Vec3 pos, float gain, float range, float pitch, bool loop, int ambient) {
	const int vi = pickVoice(loudnessOf(located, pos, gain, range));
	if (vi < 0) {
		++_stats.culled;
		return 0;
	}
	Voice& v = _voices[vi];
	const u16 serial = u16(v.serial + 1);
	v = Voice {};
	v.serial = serial ? serial : u16(1);
	v.active = true;
	v.located = located;
	v.loop = loop;
	v.sample = sample;
	v.pos = pos;
	v.gain = gain;
	v.range = range;
	v.pitch = pitch;
	v.ambient = ambient;
	v.started = _tick;
	sys::mutexLock(_lock);
	Sample& s = _samples[sample];
	++s.pins;
	s.used = _tick;
	const i16* data = s.data;
	const u32 frames = s.frames, rate = s.rate;
	sys::mutexUnlock(_lock);
	float l, r;
	mixOf(v, l, r);
	v.lastL = l;
	v.lastR = r;
	_out->start(u32(vi), data, frames, rate, loop, l, r, pitch);
	++_stats.played;
	const u32 handle = (u32(v.serial) << 8) | u32(vi + 1);
	if (ambient >= 0 && u32(ambient) < _ambientCount) _emitters[ambient].handle = handle;
	return handle;
}

u32 Sound::request(int defIndex, bool located, Vec3 pos, float gain, float range, int forceLoop, int ambient, u32 ticket) {
	if (defIndex < 0 || !ready()) return 0;
	const Def& d = _defs[defIndex];
	if (d.state != 1) return 0;
	const float g = gain * d.gain;
	const float r = range > 0.0f ? range : DEFAULT_RANGE;
	if (loudnessOf(located, pos, g, r) * _master < MIN_AUDIBLE) {
		++_stats.culled;
		return 0;
	}
	float pitch = 1.0f;
	if (d.pitchOff != 0.0f || d.pitchVar != 0.0f) {
		const float semitones = clamp(d.pitchOff + (random01() * 2.0f - 1.0f) * d.pitchVar, -12.0f, 12.0f);
		pitch = exp2f(semitones / 12.0f);
	}
	const bool loop = forceLoop >= 0 ? forceLoop != 0 : d.loop;
	const int s = findSample(d.sample, true);
	if (s < 0) {
		++_stats.culled;
		return 0;
	}
	u8 state;
	sys::mutexLock(_lock);
	state = _samples[s].state;
	sys::mutexUnlock(_lock);
	if (state == S_READY) {
		const u32 h = startNow(s, located, pos, g, r, pitch, loop, ambient);
		setOwn(h, d.gain);
		return h;
	}
	if (state == S_MISSING) {
		++_stats.absent;
		return 0;
	}
	for (Pending& p : _pending)
		if (!p.used) {
			p = Pending {};
			p.used = true;
			p.def = u32(defIndex);
			p.located = located;
			p.pos = pos;
			p.gain = g;
			p.range = r;
			p.pitch = pitch;
			p.loop = loop;
			p.ambient = ambient;
			p.ticket = ticket;
			return 0;
		}
	++_stats.culled;
	return 0;
}

u32 Sound::play(const char* name, const Vec3* at, float gain, float range, bool once) {
	if (!ready() || !name || !name[0]) return 0;
	int d = defFor(hashName(name), name);
	if (d < 0 || _defs[d].state != 1) {
		++_stats.unknown;
		note(name, false);
		return 0;
	}
	// One of its variants as ZenGin and OpenGothic do: "CS_IAM_ME_FL", "CS_IAM_ME_FL_A1" ...
	if (_defs[d].variants < 0) {
		int n = 0;
		char v[64];
		for (int k = 1; k <= 9; ++k) {
			snprintf(v, sizeof(v), "%s_A%d", name, k);
			const int vd = defFor(hashName(v), v);
			if (vd < 0 || _defs[vd].state != 1) break;
			n = k;
		}
		_defs[d].variants = i8(n);
	}
	if (_defs[d].variants > 0) {
		const int pick = int(random01() * float(_defs[d].variants + 1));
		if (pick > 0 && pick <= _defs[d].variants) {
			char v[64];
			snprintf(v, sizeof(v), "%s_A%d", name, pick);
			const int vd = defFor(hashName(v), v);
			if (vd >= 0 && _defs[vd].state == 1) d = vd;
		}
	}
	return request(d, at != nullptr, at ? *at : Vec3 {0, 0, 0}, gain, range, once ? 0 : -1, -1);
}

u32 Sound::playHash(u32 nameHash, const Vec3* at, float gain, float range) {
	if (!ready()) return 0;
	const int d = defFor(nameHash, nullptr);
	if (d < 0 || _defs[d].state != 1) {
		++_stats.unknown;
		return 0;
	}
	return request(d, at != nullptr, at ? *at : Vec3 {0, 0, 0}, gain, range, -1, -1);
}

const char* Sound::surfaceSuffix(u8 group) {
	switch (group) {
	case 1: return "_METAL";
	case 2: return "_STONE";
	case 3: return "_WOOD";
	case 4: return "_EARTH";
	case 5: return "_WATER";
	case 6: return "_SNOW";
	default: return nullptr;
	}
}

u32 Sound::playGround(const char* base, u8 group, const Vec3* at, float gain, float range) {
	if (!ready() || !base || !base[0]) return 0;
	const char* suffix = surfaceSuffix(group);
	if (suffix) {
		char full[64];
		snprintf(full, sizeof(full), "%s%s", base, suffix);
		if (knows(full)) return play(full, at, gain, range);
	}
	return play(base, at, gain, range);
}

u32 Sound::playLoop(const char* name, float gain) {
	if (!ready() || !name || !name[0]) return 0;
	const int d = defFor(hashName(name), name);
	if (d < 0 || _defs[d].state != 1) {
		++_stats.unknown;
		note(name, false);
		return 0;
	}
	const u32 ticket = 0x80000000u | (_nextTicket++ & 0x7FFFFFFFu);
	const u32 h = request(d, false, Vec3 {0, 0, 0}, gain, 0.0f, 1, -1, ticket);
	if (h) return h;
	for (const Pending& p : _pending)
		if (p.used && p.ticket == ticket) return ticket;
	return 0;
}

bool Sound::addSynthetic(const char* name, const i16* samples, u32 frames, u32 rate, bool loop) {
	if (!ready() || !name || !name[0] || !samples || frames == 0 || rate < SOUND_MIN_RATE) return false;
	const int d = defFor(hashName(name), name);
	if (d < 0 || _defs[d].state == 1) return false;
	const u32 hash = hashName(name) ^ 0x5A17C0DEu; // (no archive entry has it)
	const u32 bytes = frames * u32(sizeof(i16));
	sys::mutexLock(_lock);
	int slot = -1;
	for (int i = 0; i < SAMPLES && slot < 0; ++i)
		if (_samples[i].state == S_EMPTY) slot = i;
	void* block = slot >= 0 && makeRoom(bytes) ? _out->allocSamples(bytes) : nullptr;
	if (!block) {
		sys::mutexUnlock(_lock);
		return false;
	}
	memcpy(block, samples, bytes);
	_out->publishSamples(block, bytes);
	Sample& s = _samples[slot];
	s = Sample {};
	s.hash = hash;
	s.state = S_READY;
	s.pins = 1; // never given up for another
	s.block = block;
	s.data = static_cast<const i16*>(block);
	s.frames = frames;
	s.rate = rate;
	s.bytes = bytes;
	s.used = _tick;
	_cacheBytes += bytes;
	sys::mutexUnlock(_lock);
	Def& def = _defs[d];
	def.sample = hash;
	def.gain = 1.0f;
	def.loop = loop;
	def.pitchOff = def.pitchVar = 0.0f;
	def.variants = 0;
	def.state = 1;
	return true;
}

u32 Sound::resolve(u32 handle) const {
	if (!(handle & 0x80000000u)) return handle;
	for (const Ticket& t : _tickets)
		if (t.ticket == handle) return t.handle;
	return 0;
}

void Sound::setOwn(u32 handle, float own) {
	const u32 vi = (handle & 0xFFu);
	if (vi == 0 || vi > u32(VOICES)) return;
	Voice& v = _voices[vi - 1];
	if (v.active && v.serial == u16(handle >> 8)) v.own = own;
}

void Sound::stop(u32 handle) {
	if (handle & 0x80000000u) {
		for (Pending& p : _pending)
			if (p.used && p.ticket == handle) p.used = false;
		for (Ticket& t : _tickets)
			if (t.ticket == handle) {
				const u32 h = t.handle;
				t = Ticket {};
				stop(h);
			}
		return;
	}
	const u32 vi = (handle & 0xFFu);
	if (!ready() || vi == 0 || vi > u32(VOICES)) return;
	Voice& v = _voices[vi - 1];
	if (!v.active || v.serial != u16(handle >> 8)) return;
	_out->stop(vi - 1);
	release(v);
}

void Sound::setHandleGain(u32 handle, float gain) {
	if (handle & 0x80000000u) {
		for (Pending& p : _pending)
			if (p.used && p.ticket == handle) p.gain = clamp(gain, 0.0f, 1.0f) * _defs[p.def].gain;
		handle = resolve(handle);
	}
	const u32 vi = (handle & 0xFFu);
	if (!ready() || vi == 0 || vi > u32(VOICES)) return;
	Voice& v = _voices[vi - 1];
	if (!v.active || v.serial != u16(handle >> 8)) return;
	v.gain = clamp(gain, 0.0f, 1.0f) * v.own;
}

void Sound::stopAll() {
	if (!ready()) return;
	for (u32 i = 0; i < u32(VOICES); ++i)
		if (_voices[i].active) {
			_out->stop(i);
			release(_voices[i]);
		}
	for (Pending& p : _pending) p.used = false;
	for (Ticket& t : _tickets) t = Ticket {};
}

bool Sound::playing(u32 handle) const {
	if (handle & 0x80000000u) {
		for (const Pending& p : _pending)
			if (p.used && p.ticket == handle) return true; // (still waiting for its sample: on its way)
		handle = resolve(handle);
	}
	const u32 vi = (handle & 0xFFu);
	if (!ready() || vi == 0 || vi > u32(VOICES)) return false;
	const Voice& v = _voices[vi - 1];
	return v.active && v.serial == u16(handle >> 8);
}

// ---- every frame ----

void Sound::update(float dt, float hours) {
	if (!ready()) return;
	++_tick;
	u32 active = 0;
	for (u32 i = 0; i < u32(VOICES); ++i) {
		Voice& v = _voices[i];
		if (!v.active) continue;
		if (!_out->playing(i)) {
			release(v);
			continue;
		}
		++active;
		float l, r;
		mixOf(v, l, r);
		if (fabsf(l - v.lastL) > 0.01f || fabsf(r - v.lastR) > 0.01f) {
			v.lastL = l;
			v.lastR = r;
			_out->set(i, l, r, v.pitch);
		}
	}
	_stats.voices = active;
	// Sounds that waited for their sample.
	for (Pending& p : _pending) {
		if (!p.used) continue;
		p.age += dt;
		const Def& d = _defs[p.def];
		const int s = findSample(d.sample, false);
		u8 state = S_QUEUED;
		if (s >= 0) {
			sys::mutexLock(_lock);
			state = _samples[s].state;
			sys::mutexUnlock(_lock);
		} else {
			state = S_EMPTY; // evicted before it was used
		}
		if (state == S_READY) {
			p.used = false;
			if (p.age <= WAIT_FOR_SAMPLE || p.loop) {
				const u32 h = startNow(s, p.located, p.pos, p.gain, p.range, p.pitch, p.loop, p.ambient);
				setOwn(h, d.gain);
				if (p.ticket && h) {
					_tickets[_ticketTurn] = Ticket {p.ticket, h};
					_ticketTurn = (_ticketTurn + 1) % u32(sizeof(_tickets) / sizeof(_tickets[0]));
				}
			} else
				++_stats.late;
		} else if (state == S_MISSING) {
			p.used = false;
			++_stats.absent;
		} else if (p.age > 2.0f) {
			p.used = false;
			++_stats.late;
		}
	}
	updateAmbient(dt, hours);
	_out->update();
	sys::mutexLock(_lock);
	_stats.cacheBytes = _cacheBytes;
	sys::mutexUnlock(_lock);
}

// ---- the world's own sounds ----

void Sound::enterWorld(Pak& pak, const char* worldAsset) {
	if (!ready()) return;
	for (u32 i = 0; i < _ambientCount; ++i)
		if (_emitters[i].handle) stop(_emitters[i].handle);
	for (Pending& p : _pending)
		if (p.used && p.ambient >= 0) p.used = false;
	free(_ambient);
	free(_emitters);
	_ambient = nullptr;
	_emitters = nullptr;
	_ambientCount = 0;
	_stats.ambient = 0;
	if (!worldAsset) return;
	char name[96];
	snprintf(name, sizeof(name), "%s/ambient", worldAsset);
	const PakEntry* e = pak.find(name);
	if (!e || e->size < sizeof(AmbientHeader)) return;
	u8* blob = static_cast<u8*>(pak.readAlloc(*e));
	if (!blob) return;
	AmbientHeader h;
	memcpy(&h, blob, sizeof(h));
	if (h.magic != AMBIENT_MAGIC || h.version != AMBIENT_VERSION || h.count == 0 || h.count > 20000u ||
	    e->size != sizeof(AmbientHeader) + h.count * sizeof(AmbientSound)) {
		free(blob);
		return;
	}
	_ambient = static_cast<AmbientSound*>(malloc(h.count * sizeof(AmbientSound)));
	_emitters = static_cast<Emitter*>(calloc(h.count, sizeof(Emitter)));
	if (!_ambient || !_emitters) {
		free(_ambient);
		free(_emitters);
		_ambient = nullptr;
		_emitters = nullptr;
		free(blob);
		return;
	}
	memcpy(_ambient, blob + sizeof(AmbientHeader), h.count * sizeof(AmbientSound));
	free(blob);
	_ambientCount = h.count;
	_stats.ambient = h.count;
}

void Sound::updateAmbient(float dt, float hours) {
	if (_ambientCount == 0) return;
	// The nearest few that are in range and in season.
	u32 chosen[AMBIENT_VOICES];
	float chosenDist[AMBIENT_VOICES];
	u32 n = 0;
	for (u32 i = 0; i < _ambientCount; ++i) {
		const AmbientSound& a = _ambient[i];
		const float dx = a.pos[0] - _listener.x, dz = a.pos[2] - _listener.z;
		const float d2 = dx * dx + dz * dz;
		if (d2 >= a.radius * a.radius) continue;
		if (a.hourFrom != a.hourTo) {
			const float h = hours;
			const bool in = a.hourFrom < a.hourTo ? (h >= a.hourFrom && h < a.hourTo) : (h >= a.hourFrom || h < a.hourTo);
			if (!in) continue;
		}
		u32 at = n;
		if (n == u32(AMBIENT_VOICES)) {
			if (d2 >= chosenDist[n - 1]) continue;
			at = n - 1;
		} else {
			++n;
		}
		while (at > 0 && chosenDist[at - 1] > d2) {
			chosen[at] = chosen[at - 1];
			chosenDist[at] = chosenDist[at - 1];
			--at;
		}
		chosen[at] = i;
		chosenDist[at] = d2;
	}
	// Whoever fell out of range or out of the nearest few: a loop stops, the others may come round again.
	for (u32 i = 0; i < _ambientCount; ++i) {
		Emitter& em = _emitters[i];
		if (!em.inside && !em.handle) continue;
		bool keep = false;
		for (u32 k = 0; k < n; ++k) keep = keep || chosen[k] == i;
		if (keep) continue;
		em.inside = false;
		if (em.handle && _ambient[i].mode == AMBIENT_LOOP) stop(em.handle);
	}
	u32 on = 0;
	for (u32 k = 0; k < n; ++k) {
		const u32 i = chosen[k];
		const AmbientSound& a = _ambient[i];
		Emitter& em = _emitters[i];
		const Vec3 pos = {a.pos[0], a.pos[1], a.pos[2]};
		const float gain = float(a.volume) / 255.0f;
		const bool entering = !em.inside;
		em.inside = true;
		if (em.handle && !playing(em.handle)) em.handle = 0;
		if (em.handle) ++on;
		em.timer -= dt;
		switch (a.mode) {
		case AMBIENT_LOOP:
			if (!em.handle && em.timer <= 0.0f) {
				em.timer = 0.5f; // (a sample on its way: do not ask again every frame)
				const int d = defFor(a.sound, nullptr);
				if (d >= 0 && _defs[d].state == 1) request(d, true, pos, gain, a.radius, 1, int(i));
			}
			break;
		case AMBIENT_ONCE:
			if (entering) {
				const int d = defFor(a.sound, nullptr);
				if (d >= 0 && _defs[d].state == 1) request(d, true, pos, gain, a.radius, 0, int(i));
			}
			break;
		default: // AMBIENT_RANDOM
			if (entering) em.timer = a.delayMin + random01() * max(a.delayMax - a.delayMin, 0.0f);
			if (em.timer <= 0.0f) {
				em.timer = max(a.delayMin, 1.0f) + random01() * max(a.delayMax - a.delayMin, 0.0f);
				const int d = defFor(a.sound, nullptr);
				if (d >= 0 && _defs[d].state == 1) request(d, true, pos, gain, a.radius, 0, int(i));
			}
			break;
		}
	}
	_stats.ambientOn = on;
}

// ---- the scripts' part ----

namespace {
struct ScriptSide {
	Sound* sound;
	Sound::Locator where;
	void* user;
};
ScriptSide gScript; // (one machine, one engine)
} // namespace

void Sound::bindScript(ScriptVm& gameVm, Locator where, void* user) {
	if (!_vm) return;
	gScript = {this, where, user};
	gameVm.bind("SND_PLAY", [](ScriptVm& vm, void*) {
		const char* name = vm.popString();
		if (gScript.sound) gScript.sound->play(name, nullptr);
	}, nullptr);
	gameVm.bind("SND_PLAY3D", [](ScriptVm& vm, void*) {
		const char* name = vm.popString();
		ScriptInstance* who = vm.popInstance();
		Vec3 pos;
		if (gScript.sound && gScript.where && gScript.where(gScript.user, who, pos))
			gScript.sound->play(name, &pos);
		else if (gScript.sound)
			gScript.sound->play(name, nullptr);
	}, nullptr);
}

// ---- reports ----

SoundStats Sound::stats() const {
	SoundStats s = _stats;
	return s;
}

void Sound::statusLine(char* out, u32 size) const {
	if (!ready()) {
		snprintf(out, size, "snd off");
		return;
	}
	snprintf(out, size, "snd %u/%u c%.1f/%.0fM a%u/%u u%u m%u", unsigned(_stats.voices), unsigned(VOICES),
	         double(_stats.cacheBytes) / 1048576.0, double(_stats.cacheLimit) / 1048576.0, unsigned(_stats.ambientOn), unsigned(_stats.ambient),
	         unsigned(_stats.unknown), unsigned(_stats.absent));
}

void Sound::writeReport(FILE* f) const {
	fprintf(f, "sound: played %u, unknown names %u, samples missing %u, late %u, culled %u, stolen %u, loads %u, evicted %u\n",
	        unsigned(_stats.played), unsigned(_stats.unknown), unsigned(_stats.absent), unsigned(_stats.late), unsigned(_stats.culled),
	        unsigned(_stats.stolen), unsigned(_stats.loads), unsigned(_stats.evicted));
	for (u32 i = 0; i < _logCount; ++i)
		fprintf(f, "  %s x%u %s\n", _log[i].name, unsigned(_log[i].count), _log[i].absent ? "(no sample)" : "(no such sound)");
}

} // namespace g3
