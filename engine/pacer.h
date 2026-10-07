// Frame pacing. The console shows a frame every 16.7 ms or, if the frame was
// not done, 16.7 ms later: a scene that needs 17 ms runs at 30 fps, and one
// that hovers around 16.7 ms flips between the two, which is what a stutter
// looks like. The pacer keeps the load clear of that edge:
//  * it shortens the distance at which small and medium objects (barrels,
//    bushes, fences - most of the triangles, least of the picture) are drawn
//    as soon as a frame takes more than ~90% of the time there is, and
//    lengthens it again, slowly, once there is room;
//  * if even the shortest distance is not enough it settles on a steady
//    30 fps instead of flipping, and returns to 60 when the load allows.
// Pure arithmetic on the measured times, so it is tested on the PC.
#pragma once
#include "g3ds/base.h"

namespace g3 {

struct PacerConfig {
	float high = 0.90f;      // load above which detail is taken away
	float low = 0.80f;       // load below which it is given back
	float minDetail = 0.35f; // shortest object distance, as a share of the configured one
	float recover = 0.004f;  // detail given back per frame (the full range takes about 3 s)
	float enterHalf = 1.0f;  // load that, at minimum detail, counts towards half rate
	float leaveHalf = 0.80f; // load below which full rate is tried again
	u32 enterFrames = 20;    // consecutive overloaded frames before going to half rate
	u32 leaveFrames = 60;    // consecutive light frames before leaving it
};

class FramePacer {
public:
	PacerConfig cfg;

	void reset();
	// The time the frame that just finished needed, as a share of one 60 Hz
	// refresh (1.0 = 16.7 ms). What counts as "needed" is the platform's
	// business: on the 3DS it is CPU draw time plus GPU time (see main.cpp).
	void update(float frameLoad);

	// Factor for StreamConfig::detailScale.
	float detailScale() const { return _detail; }
	// Refreshes per frame: 1 (60 fps) or 2 (30 fps).
	int interval() const { return _interval; }
	// The smoothed load the decisions are based on.
	float load() const { return _load; }

private:
	float _detail = 1.0f;
	float _load = 0.0f;
	int _interval = 1;
	u32 _over = 0, _under = 0;
};

} // namespace g3
