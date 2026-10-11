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
	SOUND_IMA4 = 1,  // speech only (see below): IMA ADPCM, 4 bits a sample
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

// ---- voices: "speech.pak" ----
// What the characters say. One entry for each line of the game's dialogue that
// has a recording, named "speech/<8 hex digits>": hashName() of the line's name
// ("DIA_Diego_Hallo_11_00"), the one dialog/index has. The blob is a
// SoundHeader (format SOUND_PCM16, the recording as it is - the default - or SOUND_IMA4, a quarter the size) and mono samples; IMA ADPCM: the first
// sample whole (4 bytes: the predictor and the step index), then the other
// frames-1 as 4-bit codes, the low nibble first (--voice-codec ima).
constexpr u32 SPEECH_MAX_FRAMES = 22050u * 90u;

inline u32 imaBytes(u32 frames) { return 4u + (frames > 0 ? (frames - 1u + 1u) / 2u : 0u); }

inline bool speechValid(const SoundHeader& h, u32 blobSize) {
	if (h.magic != SOUND_MAGIC || h.version != SOUND_VERSION || h.frames == 0 || h.frames > SPEECH_MAX_FRAMES ||
	    h.rate < SOUND_MIN_RATE || h.rate > SOUND_MAX_RATE)
		return false;
	if (h.format == SOUND_PCM16) return blobSize == u32(sizeof(SoundHeader)) + h.frames * 2u;
	return h.format == SOUND_IMA4 && blobSize == u32(sizeof(SoundHeader)) + imaBytes(h.frames);
}

inline u32 speechEntryHash(u32 lineHash) {
	static const char digits[] = "0123456789abcdef";
	char name[24] = "speech/";
	for (u32 i = 0; i < 8; ++i) name[7 + i] = digits[(lineHash >> (28 - 4 * i)) & 15u];
	name[15] = 0;
	return hashName(name);
}
// The name the baker gives that entry.
inline void speechEntryName(u32 lineHash, char (&out)[16]) {
	static const char digits[] = "0123456789abcdef";
	for (u32 i = 0; i < 7; ++i) out[i] = "speech/"[i];
	for (u32 i = 0; i < 8; ++i) out[7 + i] = digits[(lineHash >> (28 - 4 * i)) & 15u];
	out[15] = 0;
}

// The IMA ADPCM both sides share (the step tables of the format).
constexpr i32 IMA_STEPS[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,
                               28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,
                               107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,
                               408,   449,   494,   544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,
                               1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
                               5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
                               22385, 24623, 27086, 29794, 32767};
constexpr i32 IMA_INDEX[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

struct ImaState {
	i32 predictor = 0;
	i32 index = 0;
};
// One code in, one sample out (the decoder and, to track it, the encoder use it).
inline i16 imaStep(ImaState& st, u32 code) {
	const i32 step = IMA_STEPS[st.index];
	i32 diff = step >> 3;
	if (code & 1) diff += step >> 2;
	if (code & 2) diff += step >> 1;
	if (code & 4) diff += step;
	if (code & 8) diff = -diff;
	st.predictor = st.predictor + diff;
	if (st.predictor > 32767) st.predictor = 32767;
	if (st.predictor < -32768) st.predictor = -32768;
	st.index += IMA_INDEX[code & 15u];
	if (st.index < 0) st.index = 0;
	if (st.index > 88) st.index = 88;
	return i16(st.predictor);
}
// Decodes `frames` samples from `payload` (imaBytes(frames) bytes) into `out`.
inline void imaDecode(const u8* payload, u32 frames, i16* out) {
	ImaState st;
	st.predictor = i16(u16(payload[0] | (payload[1] << 8)));
	st.index = payload[2] > 88 ? 88 : payload[2];
	out[0] = i16(st.predictor);
	const u8* nibbles = payload + 4;
	for (u32 i = 1; i < frames; ++i) {
		const u32 k = i - 1;
		const u32 code = (nibbles[k >> 1] >> ((k & 1) * 4)) & 15u;
		out[i] = imaStep(st, code);
	}
}

} // namespace g3
