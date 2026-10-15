// The baker's own rain and thunder (tools/bake/weather_sfx/*.wav, built into g3ds-bake): see weather_sfx_bake.h.
#include "weather_sfx_bake.h"

#include "sound_bake.h"

#include <cstdio>

#ifndef G3DS_WEATHER_SFX_DIR
#error "G3DS_WEATHER_SFX_DIR: the folder of the weather sounds (CMakeLists.txt sets it)"
#endif

// The WAV files go into the program as they are (GNU as: .incbin; GCC and Clang, Linux and MinGW). The read-only data
// section has another name in a Windows program, and its assembler has no ".previous": ".text" is where the compiler
// expects to be after a top-level asm (it names its own section before each function anyway).
#if defined(_WIN32)
#define G3_RODATA ".section .rdata,\"dr\"\n"
#else
#define G3_RODATA ".section .rodata\n"
#endif
#define G3_INCBIN(sym, file)                                                                                                     \
	__asm__(G3_RODATA ".global " #sym "\n.balign 16\n" #sym ":\n.incbin \"" G3DS_WEATHER_SFX_DIR "/" file "\"\n" \
	        ".global " #sym "_end\n" #sym "_end:\n.byte 0\n.text\n");                                                         \
	extern "C" const unsigned char sym[];                                                                                     \
	extern "C" const unsigned char sym##_end[];

G3_INCBIN(g3_sfx_rain, "G3_RAIN.wav")
G3_INCBIN(g3_sfx_thunder_near, "G3_THUNDER_NEAR.wav")
G3_INCBIN(g3_sfx_thunder_roll, "G3_THUNDER_ROLL.wav")
G3_INCBIN(g3_sfx_thunder_far, "G3_THUNDER_FAR.wav")

namespace g3 {

std::string bakeWeatherSfx(PakWriter& pak, u32 maxRate, std::string& meta) {
	struct Own {
		const char* name; // the sample's name ("sfx/<lower case>")
		char kind;        // in meta/weathersfx: L the rain's loop, N near thunder, F far thunder
		const unsigned char* begin;
		const unsigned char* end;
	};
	const Own own[] = {
	    {"G3_RAIN", 'L', g3_sfx_rain, g3_sfx_rain_end},
	    {"G3_THUNDER_NEAR", 'N', g3_sfx_thunder_near, g3_sfx_thunder_near_end},
	    {"G3_THUNDER_ROLL", 'F', g3_sfx_thunder_roll, g3_sfx_thunder_roll_end},
	    {"G3_THUNDER_FAR", 'F', g3_sfx_thunder_far, g3_sfx_thunder_far_end},
	};
	std::string report;
	u64 bytes = 0;
	for (const Own& o : own) {
		const std::vector<u8> file(o.begin, o.end);
		SrcSound sound;
		std::string why;
		if (!decodeWav(file, sound, why)) {
			report += std::string(report.empty() ? "" : ", ") + o.name + " NOT USED: " + why;
			continue;
		}
		resampleSound(sound, maxRate);
		if (sound.samples.size() > SOUND_MAX_FRAMES || sound.rate < SOUND_MIN_RATE) {
			report += std::string(report.empty() ? "" : ", ") + o.name + " NOT USED: too long or too low a rate";
			continue;
		}
		std::string lower = o.name;
		for (char& c : lower) c = char(c >= 'A' && c <= 'Z' ? c + 32 : c);
		// The rain plays round and round from start to end (the recording's end is faded into its start).
		const std::vector<u8> blob = o.kind == 'L' ? encodeSound(sound, 0, u32(sound.samples.size())) : encodeSound(sound);
		if (!pak.has("sfx/" + lower)) pak.add("sfx/" + lower, ASSET_SOUND, blob, false);
		bytes += blob.size();
		meta += std::string(1, o.kind) + " " + o.name + "\n";
		char line[96];
		std::snprintf(line, sizeof(line), "%s%s %.1f s at %u Hz", report.empty() ? "" : ", ", o.name, double(sound.samples.size()) / double(sound.rate),
		              sound.rate);
		report += line;
	}
	char tail[64];
	std::snprintf(tail, sizeof(tail), " (%.2f MB)", double(bytes) / 1048576.0);
	return "weather sounds of the baker: " + report + tail;
}

} // namespace g3
