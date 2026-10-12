// Frame pacing. The console shows a frame every 16.7 ms or, if the frame was
// not done, 16.7 ms later: a scene that needs 17 ms runs at 30 fps, and one
// that hovers around 16.7 ms flips between the two, which is what a stutter
// looks like. The pacer keeps the load clear of that edge:
//  * at 60 fps it shortens the distance at which small and medium objects
//    (barrels, bushes, fences - most of the triangles, least of the picture)
//    are drawn as soon as a frame takes more than ~90% of the time there is,
//    and lengthens it again once there has been room for a while: in steps
//    of a tenth, two seconds apart, and not while the view is turning fast
//    (the load comes and goes with the view, and the objects would with it);
//  * if even the shortest distance is not enough it settles on a steady
//    30 fps instead of flipping. There is twice the time then, so the objects
//    come back;
//  * from 30 fps it returns to 60 when the scene has become light, and now
//    and then it checks whether 60 would fit with fewer objects: it takes
//    them away for a moment, looks at the load, and decides.
// Pure arithmetic on the measured times, so it is tested on the PC.
#pragma once
#include "g3ds/base.h"

namespace g3 {

struct PacerConfig {
	// Full rate (60 fps).
	float high = 0.90f;      // load above which detail is taken away
	float low = 0.80f;       // load below which it is given back
	float minDetail = 0.35f; // shortest object distance, as a share of the configured one
	// When even that is too much (the objects are gone and the world itself is more than a frame
	// can take - an open square) the world's own draw distance, with its fog, shortens too, down to
	// this share, a little at a time; it is given back before the objects are.
	float minDistance = 0.65f;
	float distOver = 1.0f;     // load above which (objects gone) the distance shortens
	float distStep = 0.02f;    // most taken away in a frame (less when only a little over)
	float distRecover = 0.05f; // given back at a time (with the same calm as the objects)
	float recoverStep = 0.10f; // detail given back at a time ...
	u32 recoverEvery = 120;    // ... at most every this many frames (2 s) ...
	u32 calmFrames = 90;       // ... once the load has been below `low` this many frames running (1.5 s)
	                           // and the view has not been turning fast meanwhile: walking and turning
	                           // the camera the load comes and goes, and the objects with it - they are
	                           // given back only when it has settled
	float recover = 0.002f;    // at half rate: detail given back per frame (the full range takes about 5 s)
	float enterHalf = 1.0f;  // load that, at minimum detail, counts towards half rate
	u32 enterFrames = 20;    // consecutive overloaded frames before going to half rate
	// Half rate (30 fps). Loads are still shares of one 60 Hz refresh.
	float halfHigh = 1.80f;  // load above which detail is taken away even here
	float halfLow = 1.60f;   // load below which it is given back
	float leaveHalf = 0.80f; // load below which full rate returns straight away
	u32 leaveFrames = 60;    // consecutive light frames before that
	float probeBelow = 1.30f; // load at full detail below which it is worth asking whether 60 fps would fit with less
	float probeFit = 0.93f;   // load at minimum detail that answers yes
	u32 probeEvery = 240;     // frames between two such checks (8 s); doubles after each "no"
	u32 probeEveryMax = 1920;
	// A steady 30 fps whatever the load (the menu's "30 fps"): half rate
	// from the first frame, never back to full rate. The detail is still kept
	// within the time there is.
	bool halfOnly = false;
	// The other way round (the menu's "free up to 60"): never half rate. A frame that does not
	// fit goes out at the next refresh, so the rate lies anywhere between 30 and 60; the detail is
	// kept within the time there is as at 60.
	bool freeRate = false;
};

class FramePacer {
public:
	PacerConfig cfg;

	void reset();
	// The time the frame that just finished needed, as a share of one 60 Hz
	// refresh (1.0 = 16.7 ms). What counts as "needed" is the platform's
	// business (see platform/3ds/main.cpp). `turning`: the view turns fast
	// this frame - nothing is given back now, and the calm starts over.
	void update(float frameLoad, bool turning = false);

	// Factor for StreamConfig::detailScale.
	float detailScale() const { return _detail; }
	// Factor for the draw distance of the world itself (1 unless the objects are already at their minimum).
	float distanceScale() const { return _dist; }
	// Refreshes per frame: 1 (60 fps) or 2 (30 fps).
	int interval() const { return _interval; }
	// The smoothed load the decisions are based on.
	float load() const { return _load; }
	// True while it is finding out whether 60 fps would fit (statistics).
	bool probing() const { return _probing; }

private:
	float _detail = 1.0f;
	float _dist = 1.0f;
	float _load = 0.0f;
	int _interval = 1;
	u32 _over = 0, _under = 0, _calm = 0, _sinceStep = 0;
	bool _probing = false;
	u32 _sinceProbe = 0, _probeWait = 0, _settled = 0, _atFullRate = 0;
};

} // namespace g3
