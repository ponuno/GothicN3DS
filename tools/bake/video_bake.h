// fix201: the game's videos (Bink) into video.pak (g3ds/video.h). Bink is decoded by ffmpeg, which the baker runs (it
// is not linked in): ffmpeg.exe beside g3ds-bake.exe, or on the PATH, or --ffmpeg PATH. Without it there are no videos.
#pragma once
#include "g3ds/base.h"

#include <string>
#include <vector>

namespace g3 {

struct VideoBakeOptions {
	std::string ffmpeg;      // the program; "" - none: nothing is baked
	u32 fps = 15;            // frames a second on the console
	u32 maxW = 400, maxH = 240; // the picture fits into this, keeping its shape (multiples of 8)
	u32 audioRate = 22050;   // Hz, mono; 0: no sound
	// How different a 4x4 block must be from what is shown to be sent again (mean squared error of its 48 values).
	// Lower: sharper motion, bigger files.
	float sameBlock = 14.0f;
	unsigned threads = 1;
};

struct VideoBakeResult {
	bool ok = false;
	u32 width = 0, height = 0, frames = 0, audioFrames = 0;
	u64 blocksSent = 0, blocksAll = 0;
	std::string why; // when not ok
};

// The ffmpeg to use: `given` if set, else ffmpeg(.exe) beside `exeDir`, else whatever the PATH has ("ffmpeg"), else "".
std::string findFfmpeg(const std::string& given, const std::string& exeDir);
// The video files of a game (<game>/_work/Data/Video/*.bik, any letter case), sorted.
std::vector<std::string> findGameVideos(const std::string& gameDir);
// One video into a "video/<name>" entry's bytes.
VideoBakeResult bakeVideo(const std::string& file, const VideoBakeOptions& opt, std::vector<u8>& out);
// "video/intro" for ".../INTRO.BIK".
std::string videoEntryName(const std::string& file);

} // namespace g3
