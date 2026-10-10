// Sound in the archive. Three kinds of thing:
//  * "sfx/<name>" (ASSET_SOUND): one sample - the game's WAV file of that name
//    (lower case, no extension) turned into 16-bit mono PCM at a rate the
//    console can mix cheaply. The game's C_SFX instances (SFX.d) say which
//    sample a sound name means and how loud it is: the engine reads them from
//    the script image, so the archive has no table of its own.
//  * the event tags of the model scripts (*eventSFX, *eventSFXGrnd), kept in
//    the clips (ClipSfx, g3ds/model.h): a footstep at frame 6 of S_RUNL.
//  * "meta/ambient" (ASSET_META): the sounds the worlds have placed in them
//    (campfires, water, wind): see AmbientHeader.
#pragma once
#include "g3ds/base.h"

namespace g3 {

constexpr u32 SOUND_MAGIC = fourcc('G', '3', 'S', 'N');
constexpr u32 SOUND_VERSION = 1;

enum SoundFormat : u16 {
	SOUND_PCM16 = 0, // signed 16-bit little-endian, one channel
};

// "sfx/<name>": SoundHeader, then `frames` samples.
struct SoundHeader {
	u32 magic;
	u32 version;
	u32 rate;   // Hz
	u32 frames; // samples (one channel)
	u16 format; // SoundFormat
	u16 flags;
	u32 loopStart; // samples; a loop plays loopStart..loopEnd over and over (0, 0: the whole sample)
	u32 loopEnd;
	u32 reserved;
};
static_assert(sizeof(SoundHeader) == 32, "SoundHeader layout");

// What a sample may be: long enough to be music or a speech is not a sound
// effect, and the console has little memory to keep them in.
constexpr u32 SOUND_MAX_FRAMES = 22050u * 30u;
constexpr u32 SOUND_MAX_RATE = 48000;
constexpr u32 SOUND_MIN_RATE = 4000;

inline u32 soundBytes(const SoundHeader& h) { return u32(sizeof(SoundHeader)) + h.frames * 2u; }

inline bool soundValid(const SoundHeader& h, u32 blobSize) {
	return h.magic == SOUND_MAGIC && h.version == SOUND_VERSION && h.format == SOUND_PCM16 && h.frames > 0 &&
	       h.frames <= SOUND_MAX_FRAMES && h.rate >= SOUND_MIN_RATE && h.rate <= SOUND_MAX_RATE &&
	       blobSize == soundBytes(h) && h.loopStart <= h.loopEnd && h.loopEnd <= h.frames;
}

// The archive name of a sample: "DOOR_OPEN.WAV" -> "sfx/door_open".
inline u32 soundEntryHash(const char* fileName) {
	char name[96] = "sfx/";
	u32 n = 4;
	for (const char* p = fileName; *p && n < sizeof(name) - 1; ++p) {
		if (*p == '.') break;
		name[n++] = *p;
	}
	name[n] = 0;
	return hashName(name);
}

// "meta/ambient": the sounds placed in the worlds. For each world the baker
// writes "world/<name>/ambient": AmbientHeader, then AmbientSound[count].
constexpr u32 AMBIENT_MAGIC = fourcc('G', '3', 'A', 'M');
constexpr u32 AMBIENT_VERSION = 1;

enum AmbientMode : u8 {
	AMBIENT_LOOP = 0,   // sounds all the time while the listener is in range
	AMBIENT_ONCE = 1,   // once, when the listener comes into range
	AMBIENT_RANDOM = 2, // now and then while in range
};

struct AmbientHeader {
	u32 magic;
	u32 version;
	u32 count;
	u32 reserved;
};
static_assert(sizeof(AmbientHeader) == 16, "AmbientHeader layout");

struct AmbientSound {
	float pos[3];
	float radius;  // cm: silent beyond
	u32 sound;     // hash of the sound's name in the scripts (SFX.d), 0 for none
	u8 mode;       // AmbientMode
	u8 volume;     // 0..255
	u8 hourFrom;   // sounds only from this hour of the day ...
	u8 hourTo;     // ... until this one (equal: all day)
	float delayMin, delayMax; // seconds between plays in AMBIENT_RANDOM
};
static_assert(sizeof(AmbientSound) == 32, "AmbientSound layout");

// ---- music ----
// Music is not in data.pak: its tracks are long, and live in an archive of their
// own ("music.pak" next to it, entries stored raw so that the player can read
// them in pieces). "music/<theme file>": MusicHeader, then `frames` frames of
// `channels` interleaved 16-bit samples. The baker makes them from WAV files
// the player provides, named like the files of the scripts' C_MUSICTHEME
// instances ("OC_DAY_STD.sgt" -> OC_DAY_STD.wav).
constexpr u32 MUSIC_MAGIC = fourcc('G', '3', 'M', 'U');
constexpr u32 MUSIC_VERSION = 1;

struct MusicHeader {
	u32 magic;
	u32 version;
	u32 rate;
	u32 frames;
	u16 channels; // 1 or 2
	u16 flags;
	u32 loopStart; // frames; the track goes back to loopStart when it reaches loopEnd (0, 0: the whole track)
	u32 loopEnd;
	u32 reserved;
};
static_assert(sizeof(MusicHeader) == 32, "MusicHeader layout");

inline bool musicValid(const MusicHeader& h, u32 blobSize) {
	return h.magic == MUSIC_MAGIC && h.version == MUSIC_VERSION && (h.channels == 1 || h.channels == 2) && h.frames > 0 &&
	       h.rate >= SOUND_MIN_RATE && h.rate <= SOUND_MAX_RATE && h.loopStart <= h.loopEnd && h.loopEnd <= h.frames &&
	       u64(blobSize) == u64(sizeof(MusicHeader)) + u64(h.frames) * h.channels * 2u;
}

inline u32 musicEntryHash(const char* fileName) {
	char name[96] = "music/";
	u32 n = 6;
	for (const char* p = fileName; *p && n < sizeof(name) - 1; ++p) {
		if (*p == '.') break;
		name[n++] = *p;
	}
	name[n] = 0;
	return hashName(name);
}

// "world/<name>/music": the world's music zones (zCZoneMusic). The theme a
// zone plays is named after it: <zone>_DAY_STD and so on in the scripts.
constexpr u32 MUSIC_ZONES_MAGIC = fourcc('G', '3', 'M', 'Z');
constexpr u32 MUSIC_ZONES_VERSION = 1;

struct MusicZonesHeader {
	u32 magic;
	u32 version;
	u32 count;
	u32 reserved;
};
static_assert(sizeof(MusicZonesHeader) == 16, "MusicZonesHeader layout");

struct MusicZone {
	float bbMin[3], bbMax[3];
	i32 priority;
	u8 ellipsoid;  // the zone is the ellipsoid in its box, not the box
	u8 isDefault;  // plays outside every other zone
	u8 volume;     // 0..255
	u8 reserved;
	char name[32]; // upper case, zero-terminated
};
static_assert(sizeof(MusicZone) == 64, "MusicZone layout");

} // namespace g3
