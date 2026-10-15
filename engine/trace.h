// Where the main loop is (fix134): a name set at each big step of a frame (App::update, App::render, the platform's
// loop), cheap enough to leave in. A watchdog on another thread (platform/3ds/main.cpp) writes it to
// sdmc:/3ds/gothic3ds/hang.txt when the frames stop coming, so that a hang on the console says where it is.
#pragma once
#include "g3ds/base.h"

namespace g3 {

struct TraceState {
	const char* volatile stage = "start";
	volatile u32 frame = 0;   // frames the main loop has finished
	volatile u32 beats = 0;   // grows at every step (a loop that goes on calling steps is not a hang)
	const char* volatile recent[16] = {}; // the last steps, a ring
	volatile u32 recentAt = 0;
};
extern TraceState gTrace;

inline void trace(const char* stage) {
	gTrace.stage = stage;
	gTrace.beats = gTrace.beats + 1;
	const u32 at = gTrace.recentAt;
	gTrace.recent[at & 15u] = stage;
	gTrace.recentAt = at + 1;
}

} // namespace g3
