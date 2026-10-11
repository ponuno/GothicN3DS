// Voices. What the characters say has been recorded for the game; "speech.pak"
// (g3ds/sound.h) has the recordings of the lines the baker found, as IMA ADPCM.
// A line is read when the dialogue reaches it, decoded into the memory the
// sound hardware plays from and played on a channel of its own, whole.
// The dialogue (npc_dialog.cpp) asks how long it is and keeps the subtitle
// up that long; without a recording it goes by the time it takes to read.
#pragma once
#include "audio.h"
#include "g3ds/pak.h"
#include "g3ds/sound.h"

#include <stdio.h>

namespace g3 {

class Speech {
public:
	enum { CHANNEL = 15 };

	Speech() = default;
	~Speech() { shutdown(); }
	Speech(const Speech&) = delete;
	Speech& operator=(const Speech&) = delete;

	// `speechPakPath`: the archive of recordings. False (nothing will be heard) if it is not
	// there or the platform has no channel to spare.
	// The archive may be in pieces (the console cannot seek past 2 GiB): "speech2.pak", "speech3.pak" ...
	// beside it are opened too.
	bool init(const char* speechPakPath, AudioOut* out);
	void shutdown();
	bool ready() const { return _ready; }

	void setVolume(float volume) { _volume = clamp(volume, 0.0f, 1.0f); }
	float volume() const { return _volume; }

	// Plays the recording of the line named by hashName(). `gain` 0..1 (distance: the dialogue's
	// business). Returns its length in seconds, or 0 if there is none (or it is muted: then the
	// length is still returned, so that the subtitle stays up as long as the line is).
	float play(u32 lineHash, float gain);
	// Silence (the line is skipped, the talk is over).
	void stop();
	bool playing() const;

	// For the reports.
	u32 played() const { return _played; }
	u32 missing() const { return _missing; }
	u32 broken() const { return _broken; }
	void writeReport(FILE* f) const;

private:
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
};

} // namespace g3
