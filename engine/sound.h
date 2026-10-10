// The sound engine: plays the game's sounds by the names its scripts use.
//
//  * A name ("DOOR_OPEN") is an instance of the scripts' class C_SFX: it says
//    which sample (`file`), how loud (`vol`), how much the pitch varies and
//    whether it loops. The machine runs the instance to read it, so the
//    archive carries only the samples ("sfx/<file>", g3ds/sound.h).
//  * Samples are read from the archive by a worker thread and kept in memory
//    the platform's AudioOut provides, up to a limit, the longest unused one
//    leaving first. A sound asked for before its sample is in memory waits a
//    moment for it and is dropped if it takes too long.
//  * A sound with a place is made quieter with distance and panned between the
//    ears by where it is as seen from the listener (the camera).
//  * The worlds' own sounds (campfires, water, wind: "world/<name>/ambient")
//    play while the listener is within their range.
//
// Nothing here is needed to play: without an AudioOut (the PC), with a
// failed DSP or an archive without samples, every call does nothing.
#pragma once
#include "audio.h"
#include "g3ds/math.h"
#include "g3ds/pak.h"
#include "g3ds/sound.h"
#include "script.h"
#include "sys.h"

#include <stdio.h>

namespace g3 {

struct SoundStats {
	u32 played = 0;   // sounds started
	u32 unknown = 0;  // asked for by a name the scripts and the archive do not know
	u32 absent = 0;   // known to the scripts, but the archive has no sample for it
	u32 late = 0;     // the sample came too late to be of use
	u32 culled = 0;   // too far away to hear, or no voice to spare
	u32 stolen = 0;   // a quieter sound cut off for a louder one
	u32 loads = 0;    // samples read from the archive
	u32 evicted = 0;  // samples dropped from memory for others
	u32 voices = 0;   // sounding now
	u32 cacheBytes = 0, cacheLimit = 0;
	u32 ambient = 0;  // sounds placed in this world / of which sounding
	u32 ambientOn = 0;
};

class Sound {
public:
	enum { VOICES = 14, SAMPLES = 160, PENDING = 12, QUEUE = 16, NAMES = 256, AMBIENT_VOICES = 6, LOG = 24 };

	Sound() = default;
	~Sound() { shutdown(); }
	Sound(const Sound&) = delete;
	Sound& operator=(const Sound&) = delete;

	// `out` may be null (nothing to play on). `cacheBytes`: memory for samples.
	// The archive must stay open; the worker opens it a second time.
	bool init(Pak& pak, ScriptVm* vm, AudioOut* out, u32 cacheBytes, bool threaded);
	void shutdown();
	bool ready() const { return _out != nullptr && _vm != nullptr; }

	// 0..1. Changing it takes effect at once, for sounds already playing too.
	void setVolume(float master) { _master = clamp(master, 0.0f, 1.0f); }
	float volume() const { return _master; }
	// Where the ears are: the camera, and which way it looks (radians; 0 looks along +Z).
	void setListener(Vec3 pos, float yaw) {
		_listener = pos;
		_yaw = yaw;
	}

	// The world's own sounds. `worldAsset` is the name of the world in the
	// archive ("world/oldmine"). Without them (an older archive) there are none.
	void enterWorld(Pak& pak, const char* worldAsset);
	// `hours`: the time of day, for sounds that are only heard at some hours.
	void update(float dt, float hours);

	// Plays the sound the scripts call `name`. `at` null: no place (the
	// character's own, the interface's). `gain` 0..1 on top of the sound's
	// own; `range` cm the sound carries (0: the default). Returns a handle
	// for stop(), 0 if nothing was started (it may still start a moment
	// later, when its sample is in memory).
	u32 play(const char* name, const Vec3* at, float gain = 1.0f, float range = 0.0f);
	u32 playHash(u32 nameHash, const Vec3* at, float gain = 1.0f, float range = 0.0f);
	void stop(u32 handle);
	void stopAll();
	// The handle's sound is sounding.
	bool playing(u32 handle) const;
	// Does a sound of this name exist (an instance of C_SFX, or a sample of that name)?
	bool knows(const char* name);

	// Makes the scripts' Snd_Play and Snd_Play3D do something. `where` gives
	// the place of the character a script hands over (false: unknown).
	typedef bool (*Locator)(void* user, ScriptInstance* who, Vec3& pos);
	void bindScript(Locator where, void* user);

	// What walking on a surface sounds like: the name a ground event
	// (*eventSFXGrnd) is extended with, "_EARTH" for the material group 4 ...
	// Null for a group of no sound of its own.
	static const char* surfaceSuffix(u8 group);
	// Plays a ground event: `base` + the surface, or `base` alone when the
	// scripts know no sound of the extended name.
	u32 playGround(const char* base, u8 group, const Vec3* at, float gain = 1.0f, float range = 0.0f);

	SoundStats stats() const;
	// One line for the bottom screen.
	void statusLine(char* out, u32 size) const;
	// What was asked for and not there, for finding what the scripts name and the archive lacks.
	void writeReport(FILE* f) const;

private:
	enum SampleState : u8 { S_EMPTY, S_QUEUED, S_READY, S_MISSING, S_NOMEM };
	struct Sample {
		u32 hash = 0;
		u8 state = S_EMPTY;
		u8 pins = 0; // voices playing it
		u16 pad = 0;
		void* block = nullptr;
		const i16* data = nullptr;
		u32 frames = 0, rate = 0, bytes = 0;
		u32 used = 0; // tick of the last use
	};
	// What a name stands for, found once.
	struct Def {
		u32 name = 0;
		u32 sample = 0; // hash of the archive entry
		float gain = 1.0f;
		float pitchOff = 0, pitchVar = 0; // semitones
		bool loop = false;
		u8 state = 0; // 0 unused, 1 good, 2 unknown
	};
	struct Voice {
		bool active = false;
		bool located = false;
		bool loop = false;
		u16 serial = 0;
		int sample = -1;
		Vec3 pos {0, 0, 0};
		float gain = 1.0f, range = 0, pitch = 1.0f;
		float lastL = -1, lastR = -1;
		int ambient = -1;
		u32 started = 0;
	};
	struct Pending {
		bool used = false;
		u32 def = 0;
		bool located = false;
		Vec3 pos {0, 0, 0};
		float gain = 1.0f, range = 0, pitch = 1.0f;
		bool loop = false;
		int ambient = -1;
		float age = 0;
	};
	struct Emitter {
		u32 handle = 0;
		float timer = 0;
		bool inside = false;
	};
	struct Missing {
		char name[40];
		u32 count;
		bool absent; // known, but no sample
	};

	int defFor(u32 nameHash, const char* name);
	int findSample(u32 hash, bool queue);
	void queueLoad(int sample);
	bool makeRoom(u32 bytes); // lock held
	u32 startNow(int sample, bool located, Vec3 pos, float gain, float range, float pitch, bool loop, int ambient);
	u32 request(int defIndex, bool located, Vec3 pos, float gain, float range, int forceLoop, int ambient);
	int pickVoice(float loudness);
	void release(Voice& v);
	void mixOf(const Voice& v, float& left, float& right) const;
	float loudnessOf(bool located, Vec3 pos, float gain, float range) const;
	void note(const char* name, bool absent);
	void updateAmbient(float dt, float hours);
	void work();
	static void threadMain(void* self);
	float random01();

	Pak* _pak = nullptr;
	ScriptVm* _vm = nullptr;
	AudioOut* _out = nullptr;
	u32 _cacheLimit = 0, _cacheBytes = 0;
	u32 _classSfx = SCRIPT_NONE, _mFile = SCRIPT_NONE, _mVol = SCRIPT_NONE, _mLoop = SCRIPT_NONE, _mPitchOff = SCRIPT_NONE,
	    _mPitchVar = SCRIPT_NONE;
	float _master = 1.0f;
	Vec3 _listener {0, 0, 0};
	float _yaw = 0;
	u32 _tick = 0;
	u32 _rng = 0x2545F491u;

	Def _defs[NAMES];
	Sample _samples[SAMPLES];
	Voice _voices[VOICES];
	Pending _pending[PENDING];
	SoundStats _stats;
	Missing _log[LOG];
	u32 _logCount = 0;

	// The world's sounds.
	AmbientSound* _ambient = nullptr;
	Emitter* _emitters = nullptr;
	u32 _ambientCount = 0;

	// The loading thread.
	Pak _loaderPak;
	sys::Thread* _thread = nullptr;
	sys::Mutex* _lock = nullptr;
	sys::Signal* _wake = nullptr;
	int _queue[QUEUE];
	u32 _queueCount = 0;
	bool _quit = false;
	bool _threaded = false;
};

} // namespace g3
