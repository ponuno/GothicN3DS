// Weather: clear, cloudy, rain, thunderstorm, snow. A state with smooth transitions (clouds, rain, snow and
// the wetness of the ground are separate amounts that follow the wanted weather), applied to the sky colours,
// the fog and the outdoor light; rain and snow are drawn as streaks over the picture.
//
// Automatic mode (fix187) follows the game's own rain schedule (Inside-Gothic, "Rain"): a rain has a start and a stop
// time on the game's clock, can come at any hour, day or night, and lasts from 1 to about 2.5 hours; the first one always
// starts between 16:30 and 17:30 (so that the player sees it); on the first three days it only rains, from the fourth
// on a rain has lightning with a chance of 40 %. Here: one rain a game day (rainOfDay), the clouds gathering 45 minutes
// before it and going 45 minutes after. Without the game's clock (setClock never called) it walks through the weather by
// a timer of its own, as before.
#pragma once
#include "sky.h"
#include "weather_flag.h"

namespace g3 {

// One rain of the schedule: start and stop in hours of its day (stop may be past 24: it goes on into the next day).
struct RainEvent {
	float start = 0.0f, stop = 0.0f;
	bool lightning = false;
};

// Settings::weather: 0 is automatic, the rest force the weather.
enum WeatherMode : u8 { WEATHER_AUTO = 0, WEATHER_CLEAR, WEATHER_CLOUDY, WEATHER_RAIN, WEATHER_STORM, WEATHER_SNOW, WEATHER_MODES };

struct WeatherStreak {
	i16 x, y, w, h;
	u32 rgba; // 0xRRGGBBAA
};

class Weather {
public:
	// `mode`: WeatherMode. `wet`: wet ground darkens the outdoor light.
	void configure(u8 mode, bool wet);
	// fix187: the game's clock - the day (Wld_GetDay: 0 the first) and the hour - that the automatic weather follows.
	// Before update(), every frame.
	void setClock(i32 day, float hours) {
		_day = day;
		_hours = hours;
		_clockKnown = true;
	}
	// The rain of a day of the schedule (the same every time for the same day).
	static RainEvent rainOfDay(i32 day);
	// How much of the rain and of its clouds the schedule has at that moment (0..1 each), and whether with lightning.
	static void scheduleAt(i32 day, float hours, float& rain, float& cloud, bool& lightning);
	// Outdoors the amounts follow the weather; indoors nothing is shown (they still run on).
	void update(float dt);
	// Greys and darkens the colours of a clear sky, shortens the view (see fogScale), flashes in a storm.
	void apply(SkyColors& c) const;
	float fogScale() const; // 1 clear .. about 0.5 in a storm
	// Streaks of rain or snow on a screen of w x h pixels at `seconds`; returns how many (<= max).
	int streaks(double seconds, WeatherStreak* out, int max, int w, int h) const;
	// How strong the wind blows against the settled setting: 0.55 clear, about 1 in rain or snow, 1.9 in a storm.
	float windScale() const {
		const float storm = _rain > 0.6f ? (_rain - 0.6f) / 0.4f : 0.0f;
		return 0.55f + 0.30f * _cloud + 0.35f * max(_rain, _snow) + 0.70f * storm;
	}
	float cloud() const { return _cloud; }
	float rain() const { return _rain; }
	float snow() const { return _snow; }
	float wet() const { return _wetAmount; }
	float flash() const { return _flash; }
	// Seconds until the thunder of the last lightning is heard (the flash comes first); 0 when none is on its way. Taken once.
	bool takeThunder(float& delay) {
		if (_thunderDelay <= 0.0f) return false;
		delay = _thunderDelay;
		_thunderDelay = 0.0f;
		return true;
	}
	bool raining() const { return _rain > 0.3f; }
	u8 kind() const { return _kind; } // the weather now wanted: WEATHER_CLEAR..WEATHER_SNOW
	// All amounts at once to what the weather wants (not for the game: for tests and a world switch).
	void settle();

private:
	void pickNext();
	float rnd(); // 0..1
	u8 _mode = WEATHER_AUTO;
	bool _wetOn = true;
	u8 _kind = WEATHER_CLEAR;
	float _timer = 150.0f; // seconds the automatic weather has left
	float _cloud = 0, _rain = 0, _snow = 0, _wetAmount = 0;
	float _flash = 0, _echo = 0, _thunderIn = 8.0f, _thunderDelay = 0;
	u32 _seed = 12345u;
	i32 _day = 0;
	float _hours = 12.0f;
	bool _clockKnown = false;
	float _scheduleRain = 0.0f, _scheduleCloud = 0.0f; // the schedule's targets now (automatic mode with the clock)
};

} // namespace g3
