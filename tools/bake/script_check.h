// Checks this project's script interpreter against the reference one
// (ZenKit's, which OpenGothic plays the game with) on a given script: both
// run the same things - the game's start-up, every dialogue, every item -
// with the same minimal stand-in for the engine, and everything they ask of
// that engine and every object they build is compared.
#pragma once
#include "script_src.h"

#include <string>
#include <vector>

namespace g3 {

struct ScriptCheckOptions {
	std::string world = "WORLD";        // STARTUP_<world>, INIT_<world>
	std::string hero = "PC_HERO";
	std::vector<std::string> functions; // further functions without parameters to run
	bool dialogues = true;              // every C_INFO: build it, ask its condition, run it
	bool items = true;                  // every C_ITEM: build it
	bool isolate = true;                // run in a process of its own (a crash there is reported, not suffered)
	int timeoutSeconds = 180;
};

struct ScriptCheckResult {
	bool ran = false;       // false: no reference interpreter in this build, or it could not load the file
	u32 runs = 0;           // things run in both
	u32 same = 0;
	u32 differ = 0;
	u32 objected = 0;       // the reference interpreter raised an error: not compared
	u64 calls = 0;          // engine calls and object checks compared
	bool stateSame = false; // the global variables at the end
	std::string detail;     // the first differences, readable
	std::string report;     // lines for the bake report ("scripts: ...")
};

// `dat` is the compiled script file, `script` what was read from it and
// `image` what was baked from that.
ScriptCheckResult checkScript(const std::vector<u8>& dat, const SrcScript& script, const std::vector<u8>& image,
                              const ScriptCheckOptions& opt);

} // namespace g3
