#include "pacer.h"

namespace g3 {

void FramePacer::reset() {
	_detail = 1.0f;
	_load = 0.0f;
	_interval = 1;
	_over = _under = 0;
	_probing = false;
	_sinceProbe = _settled = _atFullRate = 0;
	_probeWait = cfg.probeEvery;
}

void FramePacer::update(float frameLoad) {
	float now = frameLoad;
	if (!(now >= 0.0f)) now = 0.0f; // also catches NaN
	now = min(now, 4.0f);
	// Rises at once, falls gradually: one heavy frame is reason enough to
	// act, one light frame is not reason enough to undo it.
	_load = now > _load ? now : _load + (now - _load) * 0.25f;
	if (_probeWait == 0) _probeWait = cfg.probeEvery;

	if (_interval == 1) {
		++_atFullRate;
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
			_probing = false;
			_sinceProbe = 0;
			// Back here soon after a "yes": the check was too optimistic for
			// this place, so the next one waits longer. After a long stay at
			// full rate the slate is clean.
			_probeWait = _atFullRate < 600 ? min(_probeWait * 2, cfg.probeEveryMax) : cfg.probeEvery;
		}
		return;
	}

	// Half rate: there is twice the time now.
	if (_probing) {
		// Take the objects away, quickly but not at once, and see what is left.
		if (_detail > cfg.minDetail + 0.001f) {
			_detail = max(_detail - 0.04f, cfg.minDetail);
			_settled = 0;
			return;
		}
		if (++_settled < 12) return; // let the smoothed load come down
		_probing = false;
		_sinceProbe = 0;
		if (_load < cfg.probeFit) {
			_interval = 1; // it fits: full rate, and the detail finds its level from here
			_over = _under = 0;
			_atFullRate = 0;
		} else {
			_probeWait = min(_probeWait * 2, cfg.probeEveryMax);
		}
		return;
	}

	if (_load > cfg.halfHigh)
		_detail -= clamp((_load - cfg.halfHigh) * 0.5f, 0.01f, 0.08f);
	else if (_load < cfg.halfLow)
		_detail += cfg.recover;
	_detail = clamp(_detail, cfg.minDetail, 1.0f);

	// A scene this light needs no second thought.
	if (_load < cfg.leaveHalf)
		++_under;
	else
		_under = 0;
	if (_under >= cfg.leaveFrames) {
		_interval = 1;
		_over = _under = 0;
		_atFullRate = 600; // not the result of a check
		_probeWait = cfg.probeEvery;
		return;
	}
	// A scene in between might fit at full rate with fewer objects.
	++_sinceProbe;
	if (_detail >= 0.999f && _load < cfg.probeBelow && _sinceProbe >= _probeWait) {
		_probing = true;
		_settled = 0;
	}
}

} // namespace g3
