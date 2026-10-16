// fix186: the shadows of clouds drifting over the land (Graphics+, the "Cloud shadows" setting).
//
// A field of soft patches 100-300 m across, drifting with the wind, decides where a cloud keeps the sun off the ground.
// Nothing of it is drawn on its own: the sun's masks (g3ds-bake --sun-shadow, a texture per cell of the chunk grid lying
// flat over the ground) are written anew a few a frame with the field multiplied in (World::updateClouds), so the GPU
// draws exactly what it drew before - the same shader, the same stages, the same draws. The characters and the new grass
// take the field by their place on the CPU.
//
// What does not take it (as it takes no sun's mask): what sways (the leaves of trees), cut-out plants, the water.
#pragma once
#include "g3ds/base.h"

namespace g3 {

class Clouds {
public:
	// Once a frame. `coverage`: the share of the land under a cloud, 0..1. `dark`: how much a cloud takes of the light,
	// 0..1 (0: nothing to see - night, overcast, the setting off). `seconds`: the clock the drift follows.
	void set(float coverage, float dark, double seconds);
	bool active() const { return _dark > 0.003f; }
	float dark() const { return _dark; }
	float coverage() const { return _coverage; }
	// The field at world (x, z) cm: 1 in the sun, 0 under the middle of a cloud.
	float light(float x, float z) const;
	// What a cloud leaves of the light there: 1 - dark * (1 - light(x, z)).
	float shade(float x, float z) const { return _dark > 0.003f ? 1.0f - _dark * (1.0f - light(x, z)) : 1.0f; }
	// How far the field has drifted, cm (to know when something made by it is out of date).
	float driftX() const { return _shiftX; }
	float driftZ() const { return _shiftZ; }

	// The drift: cm per second, along x and z.
	static constexpr float WIND_X = 290.0f, WIND_Z = 190.0f;
	// The patches: the lattice of the big noise and of the small one, cm.
	static constexpr float BIG = 16000.0f, SMALL = 5600.0f;

	// The weather and the sun's height made into coverage and darkness (App: Weather::cloud, rain, the sun's y).
	static void fromWeather(float cloud, float rain, float sunY, float& coverage, float& dark);

private:
	float _coverage = 0.0f, _dark = 0.0f;
	float _shiftX = 0.0f, _shiftZ = 0.0f; // the drift, cm (for "has it moved": wrapped at 100 km)
	float _threshold = 1.0f;
	// The drift of each lattice (big x, big z, small x, small z): whole cells and the rest, cm.
	i32 _cells[4] = {0, 0, 0, 0};
	float _rest[4] = {0, 0, 0, 0};
};

} // namespace g3
