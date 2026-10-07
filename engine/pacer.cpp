#include "pacer.h"

namespace g3 {

void FramePacer::reset() {
	_detail = 1.0f;
	_load = 0.0f;
	_interval = 1;
	_over = _under = 0;
}

void FramePacer::update(float frameLoad) {
	float now = frameLoad;
	if (!(now >= 0.0f)) now = 0.0f; // also catches NaN
	now = min(now, 4.0f);
	// Rises at once, falls gradually: one heavy frame is reason enough to
	// act, one light frame is not reason enough to undo it.
	_load = now > _load ? now : _load + (now - _load) * 0.25f;

	if (_interval == 1) {
		if (_load > cfg.high) {
			// The further over, the bigger the step; never so big that it is
			// seen as one.
			_detail -= clamp((_load - cfg.high) * 0.5f, 0.01f, 0.08f);
		} else if (_load < cfg.low) {
			_detail += cfg.recover;
		}
		_detail = clamp(_detail, cfg.minDetail, 1.0f);

		// Everything that can go has gone and it still does not fit.
		if (_detail <= cfg.minDetail + 0.001f && _load > cfg.enterHalf)
			++_over;
		else
			_over = 0;
		if (_over >= cfg.enterFrames) {
			_interval = 2;
			_over = _under = 0;
		}
	} else {
		// Half rate: there is twice the time now, but the detail stays where
		// it is - giving it back would only make the way to 60 fps longer.
		if (_load < cfg.leaveHalf)
			++_under;
		else
			_under = 0;
		if (_under >= cfg.leaveFrames) {
			_interval = 1;
			_over = _under = 0;
		}
	}
}

} // namespace g3
