#include "pacer.h"

namespace g3 {

void FramePacer::reset() {
	_detail = 1.0f;
	_dist = 1.0f;
	_far = 1.0f;
	_farOver = _sinceFar = _farRoom = 0;
	_farWait = cfg.farRecoverEvery;
	_sinceFarCut = 100000;
	_load = 0.0f;
	_interval = 1;
	_over = _under = _calm = 0;
	_sinceStep = cfg.recoverEvery;
	_sinceDist = cfg.distRecoverEvery;
	_probing = false;
	_sinceProbe = _settled = _atFullRate = 0;
	_probeWait = cfg.probeEvery;
}

void FramePacer::update(float frameLoad, bool turning, float farShare) {
	float now = frameLoad;
	if (!(now >= 0.0f)) now = 0.0f; // also catches NaN
	now = min(now, 4.0f);
	// Rises at once, falls gradually: one heavy frame is reason enough to
	// act, one light frame is not reason enough to undo it.
	_load = now > _load ? now : _load + (now - _load) * 0.25f;
	if (_probeWait == 0) _probeWait = cfg.probeEvery;
	if (cfg.halfOnly && _interval == 1) {
		_interval = 2;
		_over = _under = 0;
		_probing = false;
	}

	if (_farWait == 0) _farWait = cfg.farRecoverEvery;
	if (_sinceFarCut < 1000000) ++_sinceFarCut;
	if (_interval == 1) {
		_far = min(_far + 0.01f, 1.0f); // (the far layer is shortened at half rate only)
		++_atFullRate;
		++_sinceStep;
		++_sinceDist;
		const bool objectsGone = _detail <= cfg.minDetail + 0.001f;
		if (_load > cfg.high) {
			// The further over, the bigger the step; never so big that it is
			// seen as one. Objects first; when they are gone, the distance.
			if (!objectsGone) {
				_detail -= clamp((_load - cfg.high) * 0.5f, 0.01f, 0.08f);
				// Far over the time there is: the objects alone will not cure it, the distance goes along.
				if (_load > cfg.distParallel) _dist -= clamp((_load - cfg.distParallel) * 0.05f, 0.001f, 0.01f);
			} else if (_load > cfg.distOver)
				_dist -= clamp((_load - cfg.distOver) * 0.1f, 0.002f, cfg.distStep);
			_calm = 0;
		} else if (_load < cfg.low && !turning) {
			// (Only once the load has settled: see PacerConfig::calmFrames.
			// And a step at a time, not a little every frame: a new step of
			// objects comes in together, then nothing for a while.)
			if (++_calm >= cfg.calmFrames) {
				if (_dist < 1.0f) {
					if (_sinceDist >= cfg.distRecoverEvery) {
						_dist += cfg.distRecover;
						_sinceDist = 0;
						_sinceStep = 0; // (the objects wait their turn after the last of it)
					}
				} else if (_detail < 1.0f && _sinceStep >= cfg.recoverEvery) {
					_detail += cfg.recoverStep;
					_sinceStep = 0;
				}
			}
		} else {
			_calm = 0;
		}
		_detail = clamp(_detail, cfg.minDetail, 1.0f);
		_dist = clamp(_dist, cfg.minDistance, 1.0f);

		// Everything that can go has gone and it still does not fit.
		if (_detail <= cfg.minDetail + 0.001f && _dist <= cfg.minDistance + 0.001f && _load > cfg.enterHalf)
			++_over;
		else
			_over = 0;
		if (_over >= cfg.enterFrames && !cfg.freeRate) {
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

	// Half rate: there is twice the time now. While it is finding out whether 60 fps would fit, the whole distance is back.
	if (_probing) {
		_dist = min(_dist + 0.01f, 1.0f);
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

	++_sinceFar;
	const bool farCan = cfg.farMin < 0.999f && _far > cfg.farMin + 0.001f && farShare >= cfg.farShareMin;
	_farOver = _load > cfg.halfHigh ? _farOver + 1 : 0;
	const auto cutFar = [&]() {
		if (_sinceFar < cfg.farEvery) return;
		_far -= clamp((_load - cfg.halfHigh) * 0.05f, 0.004f, cfg.farStep);
		_sinceFar = 0;
		// Cut again soon after it was given back: the next step back waits longer.
		if (_sinceFarCut < 900) _farWait = min(_farWait * 2, cfg.farRecoverMax);
		_sinceFarCut = 0;
		_farRoom = 0;
	};
	if (_load > cfg.halfHigh && farCan && _load <= cfg.halfDistParallel) {
		// A load that lasts: the far layer gives, the objects wait. A dip that passes costs nothing.
		if (_farOver >= cfg.farHold) cutFar();
	} else if (_load > cfg.halfHigh) {
		if (farCan) cutFar(); // (far over: the far layer goes along with the rest)
		if (_detail > cfg.minDetail + 0.001f) {
			_detail -= clamp((_load - cfg.halfHigh) * 0.5f, 0.01f, 0.08f);
			if (_load > cfg.halfDistParallel) _dist -= clamp((_load - cfg.halfDistParallel) * 0.05f, 0.001f, 0.01f);
		} else if (_load > cfg.halfDistOver) {
			_dist -= clamp((_load - cfg.halfDistOver) * 0.1f, 0.002f, cfg.halfDistStep);
		}
	} else if (_load < cfg.halfLow) {
		if (_dist < 1.0f) {
			_dist += cfg.halfDistRecover;
		} else if (_detail < 1.0f) {
			_detail += cfg.recover;
		} else {
			_detail = 1.0f;
			// The far layer comes back last, a step at a time, with room to spare.
			if (_far < 1.0f && _load < cfg.farRecoverLow) {
				if (++_farRoom >= _farWait) {
					_far = min(_far + cfg.farRecoverStep, 1.0f);
					_farRoom = 0;
				}
			} else {
				_farRoom = 0;
			}
			if (_far >= 1.0f && _sinceFarCut > 1800) _farWait = cfg.farRecoverEvery;
		}
	}
	_far = clamp(_far, min(cfg.farMin, 1.0f), 1.0f);
	_detail = clamp(_detail, cfg.minDetail, 1.0f);
	_dist = clamp(_dist, cfg.minDistance, 1.0f);

	if (cfg.halfOnly) return; // (no way back to full rate, and nothing to find out)
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
