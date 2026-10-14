// Weather: clear, cloudy, rain, thunderstorm, snow. A state with smooth transitions (clouds, rain, snow and
// the wetness of the ground are separate amounts that follow the wanted weather), applied to the sky colours,
// the fog and the outdoor light; rain and snow are drawn as streaks over the picture. Automatic mode walks
// through the weather by itself. (Nothing of the original game's weather data is read: it is designed here.)
#pragma once
#include "sky.h"
#include "weather_flag.h"

namespace g3 {

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
	// Outdoors the amounts follow the weather; indoors nothing is shown (they still run on).
	void update(float dt);
	// Greys and darkens the colours of a clear sky, shortens the view (see fogScale), flashes in a storm.
	void apply(SkyColors& c) const;
	float fogScale() const; // 1 clear .. about 0.5 in a storm
	// Streaks of rain or snow on a screen of w x h pixels at `seconds`; returns how many (<= max).
	int streaks(double seconds, WeatherStreak* out, int max, int w, int h) const;
	float cloud() const { return _cloud; }
	float rain() const { return _rain; }
	float snow() const { return _snow; }
	float wet() const { return _wetAmount; }
	float flash() const { return _flash; }
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
	float _flash = 0, _echo = 0, _thunderIn = 8.0f;
	u32 _seed = 12345u;
};

} // namespace g3
