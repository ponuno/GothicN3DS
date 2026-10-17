// Music. The game's own is DirectMusic (segments and DLS banks), which nothing
// here can play; what plays instead are tracks the player has rendered from
// it, one WAV file per theme, baked into "music.pak" (g3ds/sound.h).
//
// Which theme is up to the world, as in OpenGothic (WorldSound::tickSoundZone):
// each music zone of a world (zCZoneMusic) names a theme - "OC" plays OC_DAY_STD
// by day (4:00 to 21:00) and OC_NGT_STD by night; while an enemy is after the
// player the _THR ones (no weapon in hand) or the _FGT ones (weapon drawn).
// The zone is the one the listener is in and stays in as long as it is inside
// it, else the last zone of the list that holds him, else the world's default.
// What is not there is looked for in the default zone, by day instead of
// night, as _STD instead of _FGT / _THR. The scripts' C_MUSICTHEME instance
// says which file the theme is. Changing from one theme to another fades out
// and then in (the original changes at the next bar; a render cannot).
//
// The track is read from the card piece by piece by a worker thread and
// handed to the platform's stream in buffers of about a second.
#pragma once
#include "audio.h"
#include "g3ds/math.h"
#include "g3ds/pak.h"
#include "g3ds/sound.h"
#include "script.h"
#include "sys.h"

#include <stdio.h>

namespace g3 {

enum MusicSituation : u8 { MUSIC_CALM, MUSIC_THREAT, MUSIC_FIGHT };

class Music {
public:
	enum { ZONES = 1024, THEMES = 96, TRIED = 12 };

	Music() = default;
	~Music() { shutdown(); }
	Music(const Music&) = delete;
	Music& operator=(const Music&) = delete;

	// `musicPakPath`: the archive of tracks. False (nothing will play) if it
	// is not there, the platform has no stream or the scripts have no themes.
	bool init(const char* musicPakPath, ScriptVm* vm, AudioOut* out, bool threaded);
	void shutdown();
	bool ready() const { return _ready; }

	void setVolume(float volume) { _volume = clamp(volume, 0.0f, 1.0f); }
	float volume() const { return _volume; }

	// A theme that belongs to no zone (SYS_MENU while the menu is up, SYS_LOADING): played instead of the world's
	// until it is taken off (null or ""), then the world's comes back.
	void setSystemTheme(const char* name);
	// fix201: a video has the stream: the track stops and nothing plays until let go (then the theme is chosen anew).
	void suspend(bool on);
	bool suspended() const { return _suspended; }
	// The zones of a world ("world/oldmine" -> "world/oldmine/music").
	void enterWorld(Pak& pak, const char* worldAsset);
	// Every frame. `hours`: the time of day.
	void update(float dt, Vec3 listener, float hours, MusicSituation situation);

	// What is sounding (or being faded to), "" for nothing; and the zone the listener is in.
	const char* theme() const { return _themeName; }
	const char* zone() const { return _zoneName; }
	// Fade level 0..1 of the track now.
	float level() const { return _gain; }
	void statusLine(char* out, u32 size) const;
	void writeReport(FILE* f) const;

private:
	struct Resolved {
		u32 name = 0;
		u32 entry = 0;  // hash of the track's entry; 0: no such track
		float volume = 1.0f;
		bool used = false;
	};
	// One chunk of the track in flight between the loading thread and the stream.
	enum SlotState : u8 { SLOT_EMPTY, SLOT_READY, SLOT_QUEUED };

	int zoneAt(Vec3 p);
	u32 resolve(const char* name, float& volume);
	u32 choose(const char* zone, const char* defaultZone, bool day, MusicSituation situation, char* nameOut, u32 size, float& volume);
	u32 tryTheme(const char* zone, bool day, const char* sit, char* nameOut, u32 size, float& volume);
	bool startTrack(u32 entryHash, float volume);
	void stopTrack();
	void feed();
	void work();
	static void threadMain(void* self);

	ScriptVm* _vm = nullptr;
	AudioOut* _out = nullptr;
	bool _ready = false;
	float _volume = 0.6f;
	u32 _classTheme = SCRIPT_NONE, _mFile = SCRIPT_NONE, _mVol = SCRIPT_NONE;

	Pak _pak;       // the main thread's handle (headers)
	Pak _loaderPak; // the worker's
	Resolved _resolved[THEMES];
	MusicZone* _zones = nullptr;
	u32 _zoneCount = 0;
	int _zoneIdx = -1;      // the zone the listener is in (kept while he is inside it)
	int _defaultIdx = -1;   // the world's default zone
	char _system[32] = "";  // setSystemTheme
	float _sinceChoice = 0; // seconds since the theme was last chosen
	float _fadeOutLen = 1.2f, _fadeInLen = 1.5f;
	MusicSituation _situation = MUSIC_CALM;

	// What is wanted and what is playing.
	u32 _key = 0xFFFFFFFFu; // zone, day/night and situation the wanted theme was chosen for
	u32 _wanted = 0, _current = 0;
	float _wantedVolume = 1.0f, _currentVolume = 1.0f;
	float _gain = 0.0f;
	char _themeName[48] = "";
	char _zoneName[32] = "";
	char _tried[TRIED][96];
	u32 _triedCount = 0;

	// The stream.
	bool _open = false;
	MusicHeader _head {};
	PakEntry _entry {};
	u32 _chunkFrames = 0, _chunkBytes = 0;
	void* _blocks[AUDIO_STREAM_SLOTS] = {};
	u8 _slotState[AUDIO_STREAM_SLOTS] = {};
	u32 _slotFrames[AUDIO_STREAM_SLOTS] = {};
	u32 _fillNext = 0, _queueNext = 0;
	u32 _position = 0;   // frame the next chunk is read from
	u32 _generation = 0; // changes with every track: a chunk read for the old one is thrown away
	bool _failed = false;

	sys::Thread* _thread = nullptr;
	sys::Mutex* _lock = nullptr;
	sys::Signal* _wake = nullptr;
	bool _quit = false;
	bool _threaded = false;
	bool _suspended = false;
};

} // namespace g3
