// Turning the last picture for the view's turn ("re-projection", rotation only, sideways).
//
// A refresh that has no new picture shows the old one again, and a turning view then moves in jerks: a step, a stand,
// a step. Here the picture on the screen is moved sideways by the turn there has been since it was drawn, so that the
// refreshes in between show the picture a little further round and the jerk becomes a glide. How far to move is guessed:
// the turn rate is measured from the last two pictures and fades out the longer the wait goes on (a turn the player has
// let go of must not run on). Only the turn about the vertical axis (yaw) is done: it is what the C-Stick does most, and
// on the console it costs nothing - the display is merely told to start reading the picture a few columns on (the
// framebuffer is stored column by column, so a sideways move is a multiple of a column's 720 bytes, which the display
// controller reads from any such address). Looking up and down would be a move of a few bytes inside a column and is
// left alone. A move of the camera itself (parallax) is not corrected either.
//
// The maths only, so that it is tested on the PC; platform/3ds/reproject.cpp has the thread that asks it once per
// vertical blank and tells the display.
#pragma once
#include "g3ds/base.h"

#include <math.h>

namespace g3 {

struct ReprojectConfig {
	float fovY = 1.0472f;    // vertical field of view of the picture, radians
	int height = 240;        // the picture's height in pixels (the screen's, the buffer is the same size)
	int maxColumns = 24;     // most columns to move (about 6 % of the width): past that the strip left bare is too wide
	float fade = 0.15f;      // s: how fast the guessed turn rate dies away while no picture comes
	float maxGap = 0.25f;    // s: two pictures farther apart than this are not a measure of the turn (a pause, a load)
	float cutAngle = 0.6f;   // rad: more than this between two pictures is a cut (a teleport, a new place), not a turn
	float minRate = 0.05f;   // rad/s: a slower turn is not worth the risk of a wrong guess
	float maxRate = 8.0f;    // rad/s: a faster one is taken to be a mistake
};

struct ReprojectStep {
	bool present;  // the display must be told (the picture has to be shown moved, or no longer moved)
	int columns;   // the display starts this many columns into the buffer (positive: the picture moves left)
};

class YawReprojector {
public:
	ReprojectConfig cfg;

	void reset() {
		_have = false;
		_rate = 0.0f;
		_shown = 0;
	}

	// Pixels per radian at the middle of the picture (the focal length in pixels).
	float focalPixels() const { return 0.5f * float(cfg.height) / tanf(0.5f * cfg.fovY); }

	// A new picture became visible at time `t` (seconds); the view was turned to `yaw` for it.
	void arrive(float yaw, double t) {
		if (_have) {
			float d = yaw - _yaw;
			while (d > 3.14159265f) d -= 6.2831853f;
			while (d < -3.14159265f) d += 6.2831853f;
			const double gap = t - _t;
			if (gap > 0.0005 && gap <= double(cfg.maxGap) && d < cfg.cutAngle && d > -cfg.cutAngle) {
				float rate = d / float(gap);
				if (rate > cfg.maxRate || rate < -cfg.maxRate) rate = 0.0f;
				_rate = fabsf(rate) < cfg.minRate ? 0.0f : rate;
			} else {
				_rate = 0.0f; // (no measure: nothing is guessed until two pictures close together have come)
			}
		}
		_yaw = yaw;
		_t = t;
		_have = true;
		_shown = 0;
	}

	// Columns the picture shown at time `t` should be moved by (positive: the view has turned right, so the world
	// slides left, so the display starts further to the right in the buffer).
	int columnsAt(double t) const {
		if (!_have || _rate == 0.0f) return 0;
		const float age = float(t - _t);
		if (age <= 0.0f) return 0;
		// The turn so far if the rate dies away exponentially: rate * fade * (1 - e^(-age / fade)).
		const float turned = _rate * cfg.fade * (1.0f - expf(-age / cfg.fade));
		const float px = focalPixels() * tanf(turned);
		int c = int(px < 0.0f ? px - 0.5f : px + 0.5f);
		if (c > cfg.maxColumns) c = cfg.maxColumns;
		if (c < -cfg.maxColumns) c = -cfg.maxColumns;
		return c;
	}

	// One call per vertical blank, after the new picture (if there was one) has been reported with arrive().
	// `arrived`: a new picture is on the screen since the last call. `leftRoom`, `rightRoom`: how many columns the
	// buffer has to spare to the left and to the right of the picture (bare strips of that width are all that shows).
	ReprojectStep onVBlank(bool arrived, double t, int leftRoom, int rightRoom) {
		if (arrived) {
			// The system has put the new picture on the screen as it is.
			_shown = 0;
			return {false, 0};
		}
		int c = columnsAt(t);
		if (c > rightRoom) c = rightRoom;
		if (c < -leftRoom) c = -leftRoom;
		const bool change = c != _shown;
		_shown = c;
		return {change, c};
	}

	float rate() const { return _rate; }
	bool measured() const { return _have && _rate != 0.0f; }

private:
	bool _have = false;
	float _yaw = 0.0f, _rate = 0.0f;
	double _t = 0.0;
	int _shown = 0; // what the display was last told (0 after a new picture)
};

} // namespace g3
