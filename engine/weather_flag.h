// Whether it is raining now: set by Weather::update, read by the scripts (WLD_ISRAINING). Apart from weather.h
// so that the script library needs nothing of the renderer.
#pragma once
namespace g3 {
inline bool& weatherRainingFlag() {
	static bool raining = false;
	return raining;
}
inline bool weatherRaining() { return weatherRainingFlag(); }
} // namespace g3
