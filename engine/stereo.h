// Stereo 3D by two renders: where each eye stands and where it looks.
//
// The eyes sit half the separation either side of the camera, along the way the view is turned, and each is turned a
// little towards the other, so that what lies `convergence` away in front falls on the same place of the screen for both
// (that distance is at the screen's depth; nearer things stand out of it, farther ones behind). The separation grows
// with the console's 3D slider. Units are the game's (centimetres): the camera of a third-person view hangs 300-400 away
// from the character, which is where the screen should be.
#pragma once
#include "g3ds/math.h"

namespace g3 {

constexpr float STEREO_MAX_SEPARATION = 12.0f; // at the slider's top, between the eyes
constexpr float STEREO_CONVERGENCE = 400.0f;

struct StereoEye {
	Vec3 pos;
	float yaw;
};

// `strength` 0..1 (the slider). The eyes of strength 0 are the camera itself.
inline void stereoEyes(Vec3 pos, float yaw, float strength, StereoEye& left, StereoEye& right) {
	const float half = 0.5f * STEREO_MAX_SEPARATION * clamp(strength, 0.0f, 1.0f);
	const Vec3 rightward = {cosf(yaw), 0.0f, -sinf(yaw)}; // (forward is (sin yaw, ., cos yaw), up is y: the left-handed way)
	const float toe = atanf(half / STEREO_CONVERGENCE);
	left.pos = pos - rightward * half;
	left.yaw = yaw + toe;
	right.pos = pos + rightward * half;
	right.yaw = yaw - toe;
}

} // namespace g3
