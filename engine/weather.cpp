#include "weather.h"

#include <math.h>

namespace g3 {

namespace {

struct Target {
	float cloud, rain, snow;
};
// Indexed by WeatherMode (the automatic one is never a state).
const Target kTargets[WEATHER_MODES] = {
    {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.60f, 0.0f, 0.0f}, {0.88f, 0.60f, 0.0f}, {1.0f, 1.0f, 0.0f}, {0.80f, 0.0f, 0.75f},
};

float approach(float v, float to, float step) {
	if (v < to) return min(v + step, to);
	return max(v - step, to);
}

unsigned mix(unsigned h) {
	h = (h ^ (h >> 15)) * 2246822519u;
	h = (h ^ (h >> 13)) * 3266489917u;
	return h ^ (h >> 16);
}

} // namespace

RainEvent Weather::rainOfDay(i32 day) {
	// Three numbers of the day, the same whenever it is asked (a save loaded, the time slept through).
	const unsigned h = mix(unsigned(day) * 0x9E3779B9u ^ 0x5EA50Fu);
	const float a = float(h & 0xFFFFu) * (1.0f / 65535.0f), b = float(mix(h + 1u) & 0xFFFFu) * (1.0f / 65535.0f),
	            c = float(mix(h + 2u) & 0xFFFFu) * (1.0f / 65535.0f);
	RainEvent e;
	// The first rain between 16:30 and 17:30; the others at any hour, day or night.
	e.start = day <= 0 ? 16.5f + a : a * 24.0f;
	e.stop = e.start + 1.0f + 1.5f * b; // 1 .. 2.5 hours
	// Only rain on the first three days; then lightning with a chance of 40 %.
	e.lightning = day >= 3 && c < 0.4f;
	return e;
}

void Weather::scheduleAt(i32 day, float hours, float& rain, float& cloud, bool& lightning) {
	rain = cloud = 0.0f;
	lightning = false;
	auto smooth = [](float x) {
		x = clamp(x, 0.0f, 1.0f);
		return x * x * (3.0f - 2.0f * x);
	};
	// Today's rain, and yesterday's if it went on past midnight.
	for (int back = 0; back <= 1; ++back) {
		if (day - back < 0) continue;
		const RainEvent e = rainOfDay(day - back);
		const float t = hours + 24.0f * float(back);
		// The rain: a quarter of an hour to come and to go; the clouds three quarters before it and after.
		const float r = smooth((t - e.start) / 0.25f) * smooth((e.stop - t) / 0.25f);
		const float c = smooth((t - (e.start - 0.75f)) / 0.75f) * smooth((e.stop + 0.75f - t) / 0.75f);
		if (r > rain) rain = r;
		if (c > cloud) cloud = c;
		if (r > 0.0f) lightning = lightning || e.lightning;
	}
}

float Weather::rnd() {
	_seed = _seed * 1664525u + 1013904223u;
	return float((_seed >> 8) & 0xFFFFu) * (1.0f / 65535.0f);
}

void Weather::configure(u8 mode, bool wet) {
	if (mode >= WEATHER_MODES) mode = WEATHER_AUTO;
	_wetOn = wet;
	if (mode == _mode) return;
	_mode = mode;
	if (mode != WEATHER_AUTO) _kind = mode;
	else _timer = 5.0f; // automatic again: pick the next weather soon
}

void Weather::pickNext() {
	const float r = rnd();
	switch (_kind) {
	case WEATHER_CLEAR: _kind = WEATHER_CLOUDY; _timer = 90.0f + rnd() * 110.0f; break;
	case WEATHER_CLOUDY:
		if (r < 0.45f) { _kind = WEATHER_CLEAR; _timer = 240.0f + rnd() * 360.0f; }
		else { _kind = WEATHER_RAIN; _timer = 120.0f + rnd() * 180.0f; }
		break;
	case WEATHER_RAIN:
		if (r < 0.6f) { _kind = WEATHER_CLOUDY; _timer = 90.0f + rnd() * 110.0f; }
		else { _kind = WEATHER_STORM; _timer = 60.0f + rnd() * 90.0f; }
		break;
	case WEATHER_STORM: _kind = WEATHER_RAIN; _timer = 100.0f + rnd() * 120.0f; break;
	default: _kind = WEATHER_CLOUDY; _timer = 90.0f + rnd() * 110.0f; break; // snow, after the mode was automatic again
	}
}

void Weather::update(float dt) {
	if (!(dt > 0.0f)) return;
	if (dt > 0.5f) dt = 0.5f;
	Target t;
	if (_mode == WEATHER_AUTO && _clockKnown) {
		// fix187: the game's schedule.
		float r, c;
		bool lightning;
		scheduleAt(_day, _hours, r, c, lightning);
		const Target& full = kTargets[lightning ? WEATHER_STORM : WEATHER_RAIN];
		t.cloud = full.cloud * c;
		t.rain = full.rain * r;
		t.snow = 0.0f;
		_kind = r > 0.0f ? (lightning ? WEATHER_STORM : WEATHER_RAIN) : c > 0.0f ? WEATHER_CLOUDY : WEATHER_CLEAR;
		_scheduleRain = r;
		_scheduleCloud = c;
	} else {
		if (_mode == WEATHER_AUTO) {
			_timer -= dt;
			if (_timer <= 0.0f) pickNext();
		} else {
			_kind = _mode;
		}
		t = kTargets[_kind];
	}
	const float step = dt / 12.0f; // twelve seconds from nothing to full
	_cloud = approach(_cloud, t.cloud, step);
	_rain = approach(_rain, t.rain, step);
	_snow = approach(_snow, t.snow, step);
	// The ground soaks up the rain in a while and dries slowly.
	if (_rain > 0.05f) _wetAmount = min(1.0f, _wetAmount + _rain * dt / 25.0f);
	else _wetAmount = max(0.0f, _wetAmount - dt / 150.0f);
	// Thunder: while the storm is full, a flash every few seconds, with an echo of it.
	_flash = max(0.0f, _flash - dt * 3.5f);
	if (_echo > 0.0f) {
		_echo -= dt;
		if (_echo <= 0.0f) _flash = max(_flash, 0.7f);
	}
	if (_kind == WEATHER_STORM && _rain > 0.8f) {
		_thunderIn -= dt;
		if (_thunderIn <= 0.0f) {
			_flash = 1.0f;
			_echo = 0.18f;
			_thunderDelay = 0.4f + rnd() * 2.6f;
			_thunderIn = 6.0f + rnd() * 16.0f;
		}
	}
	weatherRainingFlag() = _rain > 0.3f;
}

void Weather::settle() {
	Target t = kTargets[_kind];
	if (_mode == WEATHER_AUTO && _clockKnown) {
		float r, c;
		bool lightning;
		scheduleAt(_day, _hours, r, c, lightning);
		const Target& full = kTargets[lightning ? WEATHER_STORM : WEATHER_RAIN];
		t = Target {full.cloud * c, full.rain * r, 0.0f};
		_kind = r > 0.0f ? (lightning ? WEATHER_STORM : WEATHER_RAIN) : c > 0.0f ? WEATHER_CLOUDY : WEATHER_CLEAR;
	}
	_cloud = t.cloud;
	_rain = t.rain;
	_snow = t.snow;
	_wetAmount = t.rain > 0.0f ? 1.0f : 0.0f;
	_flash = 0.0f;
	weatherRainingFlag() = _rain > 0.3f;
}

float Weather::fogScale() const { return 1.0f - 0.25f * _cloud - 0.25f * _rain - 0.1f * _snow; }

void Weather::apply(SkyColors& c) const {
	const float wet = _wetOn ? _wetAmount : 0.0f;
	if (_cloud <= 0.0f && wet <= 0.0f && _flash <= 0.0f) return;
	// Sky: towards a flat dark grey of the same brightness.
	const float k = _cloud * 0.8f;
	const float dark = 1.0f - 0.30f * _cloud - 0.12f * _rain;
	for (int i = 0; i < 2; ++i) {
		u8* col = i ? c.horizon : c.zenith;
		const float grey = (0.30f * float(col[0]) + 0.59f * float(col[1]) + 0.11f * float(col[2])) * dark;
		for (int j = 0; j < 3; ++j) {
			float v = float(col[j]) * (1.0f - k) + grey * k;
			v += _flash * (230.0f - v) * 0.55f;
			col[j] = u8(clamp(v, 0.0f, 255.0f) + 0.5f);
		}
	}
	// Light on the outdoors: less of it under cloud, less again on wet ground, and a little greyer.
	float m = 1.0f - 0.28f * _cloud - 0.10f * _rain - 0.14f * wet;
	if (m < 0.2f) m = 0.2f;
	const float avg = (c.light[0] + c.light[1] + c.light[2]) * (1.0f / 3.0f);
	for (int j = 0; j < 3; ++j) {
		float l = c.light[j] + (avg - c.light[j]) * _cloud * 0.4f;
		l = l * m + _flash * 0.6f;
		c.light[j] = l;
	}
}

int Weather::streaks(double seconds, WeatherStreak* out, int max, int w, int h) const {
	int n = 0;
	const int rain = int(_rain * 130.0f), snow = int(_snow * 90.0f);
	const double t = seconds;
	for (int i = 0; i < rain && n < max; ++i) {
		const unsigned a = mix(unsigned(i) * 2654435761u + 17u);
		const float speed = 2.2f + float(a & 7u) * 0.12f; // screens a second
		float y = float((a >> 4) & 1023u) * (1.0f / 1024.0f) + float(t) * speed;
		y -= floorf(y);
		const int len = 6 + int((a >> 14) & 7u);
		const int ylen = int(y * float(h + len)) - len;
		out[n++] = WeatherStreak {i16(((a >> 16) & 1023u) * unsigned(w) / 1024u), i16(ylen), 1, i16(len), 0xB4C3D870u};
	}
	for (int i = 0; i < snow && n < max; ++i) {
		const unsigned a = mix(unsigned(i) * 2246822519u + 99u);
		const float speed = 0.12f + float(a & 7u) * 0.012f;
		float y = float((a >> 4) & 1023u) * (1.0f / 1024.0f) + float(t) * speed;
		y -= floorf(y);
		const float sway = sinf(float(t) * (0.8f + float((a >> 14) & 3u) * 0.25f) + float(i)) * 6.0f;
		const int x = int(((a >> 16) & 1023u) * unsigned(w) / 1024u) + int(sway);
		const int s = (a >> 12) & 1u ? 2 : 1;
		out[n++] = WeatherStreak {i16(x), i16(int(y * float(h))), i16(s), i16(s), 0xF0F4FFC8u};
	}
	return n;
}

} // namespace g3
