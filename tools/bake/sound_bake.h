// Sound for the archive: the game's WAV files as samples ("sfx/<name>", see
// g3ds/sound.h), found through the C_SFX instances of its scripts.
#pragma once
#include "g3ds/sound.h"
#include "pak_writer.h"

#include <functional>
#include <string>
#include <vector>

namespace g3 {

class ScriptVm;

struct SrcSound {
	std::vector<i16> samples; // one channel
	u32 rate = 0;             // Hz
	u32 channels = 1;         // 1, or 2 for music (interleaved)
};

// Reads a WAV file: PCM of 8, 16, 24 and 32 bits, 32-bit float, IMA and
// Microsoft ADPCM, any number of channels (mixed down to one). Returns false,
// with the reason in `why`, for anything else.
bool decodeWav(const std::vector<u8>& file, SrcSound& out, std::string& why, bool keepStereo = false);
// Brings the rate down to `maxRate` if it is higher (averaging, so that what
// the lower rate cannot hold does not fold back as noise).
void resampleSound(SrcSound& sound, u32 maxRate);
// The archive blob of a sample (g3ds/sound.h).
std::vector<u8> encodeSound(const SrcSound& sound, u32 loopStart = 0, u32 loopEnd = 0);
// A WAV file of 16-bit mono samples (for the tests and the synthetic game).
std::vector<u8> makeWav(const std::vector<i16>& samples, u32 rate);

// The sound files of the synthetic game: STEP.WAV (a click, 44.1 kHz), STEP_STONE.WAV
// (a lower one, 22.05 kHz), HUM.WAV (a steady tone). False for any other name.
bool buildSyntheticSound(const std::string& file, std::vector<u8>& bytes);

struct SoundBakeResult {
	u32 instances = 0; // C_SFX instances in the scripts
	u32 files = 0;     // different sample files they name
	u32 baked = 0;
	u64 bytes = 0;     // of the samples in the archive
	std::vector<std::string> missing; // files the game does not have
	std::vector<std::string> unusable; // files that could not be used, with the reason
};

// Runs every instance of the scripts' class C_SFX, fetches the sample file it
// names (`fetch`: the file's name -> its bytes, false if there is none) and
// adds "sfx/<name>" to `pak`. A rate of 0 bakes nothing.
SoundBakeResult bakeSounds(ScriptVm& vm, const std::function<bool(const std::string&, std::vector<u8>&)>& fetch, u32 maxRate,
                           PakWriter& pak);

struct MusicBakeResult {
	u32 themes = 0;  // C_MUSICTHEME instances in the scripts
	u32 files = 0;   // different track files they name
	u32 baked = 0;
	u64 bytes = 0;
	std::vector<std::string> missing; // tracks not in the folder
	std::vector<std::string> unusable;
};
// Runs every instance of the scripts' class C_MUSICTHEME, looks for a WAV file
// named like the one it names in `dir` and adds "music/<name>" (stereo kept, the
// rate brought down to `maxRate`) to `pak`: the archive "music.pak".
MusicBakeResult bakeMusic(ScriptVm& vm, const std::string& dir, u32 maxRate, PakWriter& pak);
// For the tests: a track of `seconds` seconds, with a loop.
std::vector<u8> encodeMusic(const SrcSound& sound, u32 loopStart = 0, u32 loopEnd = 0);

// The music zones of a world: the blob of "world/<name>/music".
struct SrcMusicZone;
std::vector<u8> bakeMusicZones(const std::vector<SrcMusicZone>& zones);

// The ambient sounds of a world: the blob of "world/<name>/ambient".
struct SrcAmbient;
std::vector<u8> bakeAmbient(const std::vector<SrcAmbient>& sounds);

} // namespace g3
