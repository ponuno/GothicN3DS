// What the player sets in the menu (menu.h), kept in settings.bin next to
// state.bin: graphics, controls, the interface and a few switches for
// finding faults. The application applies what concerns it (App::apply); the
// platform reads the rest (frame pacing, the bottom screen).
#pragma once
#include "g3ds/base.h"

namespace g3 {

enum ObjectMode : u8 {
	OBJECTS_SMOOTH,   // the pacer takes small and medium objects away under load (60 fps first)
	OBJECTS_BALANCED, // small 45 m, medium 85 m; the pacer never below 70 %
	OBJECTS_STEADY,   // small 60 m, medium 100 m, never taken away: 30 fps when too heavy
	OBJECT_MODES
};

enum FrameRateMode : u8 {
	FRAMES_60,   // 60, a steady 30 while the frame does not fit
	FRAMES_30,   // always a steady 30 (more detail, smoother under load)
	FRAME_MODES
};

enum ControlScheme : u8 {
	CONTROLS_CONSOLE, // the stick moves the character the way it points as the camera sees it; the C-stick turns the camera
	CONTROLS_CLASSIC, // as the PC original: forward/back along the character, left/right turn it
	CONTROL_SCHEMES
};

struct Settings {
	// Graphics.
	u8 drawMetres = 120;     // 40..200
	u8 objects = OBJECTS_SMOOTH;
	u8 dissolve = 1;         // objects dissolve in and out (a shader of its own: from the next start)
	u8 frameRate = FRAMES_60;
	// Controls.
	u8 scheme = CONTROLS_CONSOLE;
	u8 cameraSpeed = 5;      // 1..10
	u8 invertY = 0;
	u8 autoCamera = 1;       // the camera swings round behind a running character
	u8 autoTarget = 1;       // drawing a weapon locks on to the nearest enemy in front
	u8 firstPerson = 0;
	// Interface.
	u8 hud = 1;
	u8 report = 1;           // the numbers on the bottom screen
	// For finding faults.
	u8 perception = 1;       // the inhabitants notice the player and each other
	u8 life = 1;             // the inhabitants live their day (off: they stand)
	u8 fights = 1;           // the inhabitants fight (off: states of attack are not begun)
	u8 immortal = 0;         // the player's character takes no damage

	// Clamps every value into its range (after reading a file of another version).
	void sanitize();
	float drawDistanceCm() const { return float(drawMetres) * 100.0f; }
	// Draw distances of medium and small objects, cm (StreamConfig::detailDistance).
	float mediumObjectsCm() const;
	float smallObjectsCm() const;
	// The least the pacer may take the objects down to (FramePacer minDetail).
	float minDetail() const;

	// settings.bin. load() leaves the defaults and returns false if the file
	// is missing or damaged.
	bool save(const char* path) const;
	bool load(const char* path);
};

} // namespace g3
