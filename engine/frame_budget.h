// What a frame has left for loading things (a chunk, a texture, a character's model) on the main thread.
//
// Every load is a bump in the frame: a chunk's mesh is handed to the GPU, a texture is converted and uploaded, a model is
// read and turned into something drawable. A frame that has no time left must not also do a load, or it is shown late, and
// on a 30 fps lock "late" means 50 ms, not 35. The platform sets `headroomMs` after every frame to what the last one left
// (the period it is aiming for, less what it took); a loader asks `allow(its cost)` before starting a job and keeps its own
// estimate of the cost. A job refused too many times in a row is let through, so nothing waits for ever in a heavy scene.
#pragma once

#include "g3ds/base.h"

namespace g3 {

struct FrameBudget {
	float headroomMs = 1.0e9f; // huge: no limit (the default, and what tests see)
	u32 maxRefusals = 6;       // frames in a row a job may be put off

	bool allow(float costMs, u32& refused) const {
		if (headroomMs >= costMs) {
			refused = 0;
			return true;
		}
		if (++refused > maxRefusals) {
			refused = 0;
			return true;
		}
		return false;
	}
};

} // namespace g3
