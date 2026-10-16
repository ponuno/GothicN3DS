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
	FRAMES_FREE, // each frame goes out at the first refresh after it is done: 30..60, never held to a steady 30
	FRAMES_40,   // the screen itself runs at 40 Hz and every frame has 25 ms (needs the display timing; otherwise as 60)
	FRAMES_30F,  // a steady 30 whose screen follows the frame: 26..30 Hz, so a frame a little over 33 ms is shown when it is ready (needs the display timing; otherwise as the steady 30)
	FRAME_MODES
};

// "Graphics+": the shadows under the characters (G3).
enum ShadowMode : u8 {
	SHADOWS_OFF,
	SHADOWS_HERO, // under the player's character only
	SHADOWS_ALL,  // and under everybody near (as far as the inhabitants are drawn, up to 25 m)
	SHADOWS_HERO_CAST, // the hero's own body thrown on the ground by the sun (his outline), nobody else's
	SHADOW_MODES
};

// Sets of the Graphics+ page's values (Settings::gfxPlus).
enum GfxPreset : u8 { GFX_PLUS_OFF, GFX_PLUS_BALANCED, GFX_PLUS_MAX, GFX_PLUS_OWN, GFX_PRESETS };

enum ControlScheme : u8 {
	CONTROLS_CONSOLE, // the stick moves the character the way it points as the camera sees it; the C-stick turns the camera
	CONTROLS_CLASSIC, // as the PC original: forward/back along the character, left/right turn it; the camera behind it
	CONTROL_SCHEMES
};

struct Settings {
	// Graphics.
	u8 drawMetres = 120;     // 40..200
	u8 objects = OBJECTS_SMOOTH;
	u8 dissolve = 1;         // objects dissolve in and out (a shader of its own: from the next start)
	u8 frameRate = FRAMES_60;
	// Controls.
	u8 scheme = CONTROLS_CLASSIC; // (the default since settings.bin version 2)
	u8 cameraSpeed = 5;      // 1..10
	u8 invertY = 0;
	u8 autoCamera = 1;       // the camera swings round behind a running character
	u8 autoTarget = 1;       // drawing a weapon locks on to the nearest enemy in front
	u8 firstPerson = 0;
	// Interface.
	u8 hud = 1;
	u8 report = 0;           // the bottom screen: 0 the inventory, 1 the numbers
	// For finding faults.
	u8 perception = 1;       // the inhabitants notice the player and each other
	u8 life = 1;             // the inhabitants live their day (off: they stand)
	u8 fights = 1;           // the inhabitants fight (off: states of attack are not begun)
	u8 immortal = 0;         // the player's character takes no damage
	// Version 3: the inventory.
	u8 weaponBack = 1;       // after a potion the weapon is taken in hand again (with its animation)
	// Version 4: language, the thinning of small objects, the range of the inhabitants.
	u8 language = 0;         // Lang (lang.h): 0 Russian, 1 English, 2 Polish, 3 German
	u8 density = 0;          // small objects drawn: 0 all, 1 three layers of four, 2 two of four (needs data baked with the layers)
	u8 npcDistance = 1;      // how far the inhabitants are drawn: 0 20 m, 1 30 m, 2 45 m
	// Version 5: sound.
	u8 soundVolume = 8;      // 0 (mute) .. 10
	u8 musicVolume = 6;      // 0 (mute) .. 10
	// Version 6: voices.
	u8 voiceVolume = 8;      // 0 (mute: the subtitles stay up as long as the line is) .. 10
	// Version 7: the frame counter (for testing) and the flow of time.
	u8 showFps = 0;          // the frames per second in the corner of the top screen
	u8 barrier = 1;          // the Barrier (barrier.h): 0 off, 1 as in the game (25 s about every 20 min), 2 always
	u8 timeFlow = 2;         // how fast the game's clock runs: 0 stopped, 1 a day in 24 minutes, 2 in an hour, 3 in two hours
	// Version 8: "Graphics+" (all off at first; what the data lacks does nothing).
	u8 gfxPlus = GFX_PLUS_OFF; // the set the values below came from (GFX_PLUS_OWN: changed one by one)
	u8 occlusion = 0;        // 0..100 %: the baked ambient occlusion's strength (data baked with --ao)
	u8 wind = 0;             // the plants sway: 0 still, 1 gently, 2 normal, 3 strongly (data baked with --wind)
	u8 shadows = SHADOWS_OFF; // round shadows under the characters
	// Version 9: the far layer (G10; data baked with --far-lod): 0 off, 1 250 m, 2 400 m, 3 600 m.
	u8 farLayer = 0;
	// Version 10: baked visibility (data baked with --vis): 0 off, 1 what the table says is hidden is not drawn.
	u8 visibility = 0;
	// Version 11: simplified far geometry (data baked with --lod): 0 off, then from 25 / 40 / 60 m a chunk lies in view.
	u8 farGeometry = 2;
	// Version 12: smoothing of edges (supersampling: the picture drawn at twice the size each way and shrunk): 0 off, 1 on.
	u8 antialias = 0; // 0 off, 1 light (twice the pixels, along one axis), 2 full (four times; before version 18 this was 1)
	// Version 13: the top screen's refresh rate follows the frame (experimental, writes the screen controller's timing):
	// 0 off (60 Hz), 1 between 45 and 60 Hz, 2 up to 72, 3 up to 80.
	u8 refresh = 0;
	// 1..3: the screen follows the frame up to 60 / 72 / 80 Hz (detail as at 60 fps); 4, 5: the same up to 72 / 80, and the
	// detail is kept to that rate's frame time too ("hold").
	float refreshMaxHz() const { return refresh == 1 ? 60.0f : (refresh == 2 || refresh == 4) ? 72.0f : (refresh == 3 || refresh == 5) ? 80.0f : 0.0f; }
	bool refreshHold() const { return refresh >= 4; }
	// Version 14: when a refresh has no new picture the old one is shown turned sideways by the turn of the view
	// (experimental, see reproject.h): 0 off, 1 on.
	u8 reproject = 0;
	// Version 15: the cache that reads models, textures, animations and chunks ahead (readahead.h): 0 off, 1 on.
	u8 readAhead = 1;
	// Version 16: the top screen's mode (after a restart): 0 the ordinary 400x240, 1 stereo 3D (the scene drawn for each
	// eye, by the console's 3D slider), 2 wide (800x240: twice the pixels across, and the 3D slider does nothing).
	// (Called stereo for the file's sake: it began as the 3D switch, and 0 and 1 mean what they did.)
	u8 stereo = 0;
	bool stereo3D() const { return stereo == 1; }
	bool wideScreen() const { return stereo == 2; }
	// Version 17: the far layer fills in, within the draw distance, for chunks not loaded yet (0: it is only drawn beyond it).
	u8 farFill = 0;
	// Version 19: how the simplified far chunks and the full ones change places: 0 at once, 1 quickly (0.35 s), 2 slowly (0.9 s).
	u8 lodFade = 1;
	// Version 20: waves on the water: 0 still, 1 gentle, 2 normal, 3 strong (data baked with --water-grid).
	u8 waves = 0;
	// Version 21: weather (WeatherMode, weather.h: 0 automatic) and the wet look of the ground in rain (0 off, 1 on).
	u8 weather = 0;
	u8 wetGround = 1;
	// Version 22: light shafts through gaps in the shade (godrays.h): 0 off, 1 on.
	u8 godRays = 1;
	// Version 23: the sun's shadow baked into the world (g3ds-bake --sun-shadow): 0 off, 1 on.
	u8 sunShadows = 1; // fix146: 0 off, 1 normal, 2 strong, 3 strongest (App::sunShadowDarkness; was on/off, 1 = on)
	// Version 24: the new grass (data baked with --grass): 0 off, 1 few (half the tufts, out to 20 m), 2 many (all, 28 m).
	u8 grass = 0;
	float grassRadiusCm() const { return grass == 1 ? 2000.0f : grass == 2 ? 2800.0f : 0.0f; }
	float grassShare() const { return grass == 1 ? 0.5f : 1.0f; }
	// Version 25 (fix148): the light of torches and fires (engine/lamps.h): 0 off, 1 the people and things, 2 the world round them too.
	u8 lamps = 2;
	// (fix168) The debug page's chapter slider: the game's chapter, kept level with it while the game runs; moved in the menu,
	// the chapter is changed. 255: not yet known (a freshly read file) - nothing is done then.
	u8 chapter = 255;
	// (fix184) The Barrier's thunder (Barrier::Thunder): 0 as in the game (MFX_Barriere_Ambient; else the far thunder), 1 near, 2 the long roll, 3 the game's
	// lightning spell, 4 none. (At the end: an older file, shorter, keeps the default.)
	u8 barrierThunder = 0;
	float waveHeightCm() const { return waves == 1 ? 10.0f : waves == 2 ? 22.0f : waves == 3 ? 40.0f : 0.0f; }
	float lodFadeMs() const { return lodFade == 0 ? 1.0f : lodFade == 2 ? 900.0f : 350.0f; }
	float lodDistanceCm() const;

	// Clamps every value into its range (after reading a file of another version).
	void sanitize();
	float drawDistanceCm() const { return float(drawMetres) * 100.0f; }
	// Draw distances of medium and small objects, cm (StreamConfig::detailDistance).
	float mediumObjectsCm() const;
	float smallObjectsCm() const;
	// How many pixels across the smallest objects of a size band still are where it stops (StreamConfig::sizePixels).
	float objectPixels() const;
	// The least the pacer may take the objects down to (FramePacer minDetail).
	float minDetail() const;
	// Of the four thinning layers of the small objects, how many are drawn.
	u8 smallLayers() const { return u8(4 - density); }
	// Real seconds a game day takes (0: the clock stands).
	float dayLengthSeconds() const { return timeFlow == 1 ? 1440.0f : timeFlow == 2 ? 3600.0f : timeFlow == 3 ? 7200.0f : 0.0f; }
	float npcRangeCm() const { return npcDistance == 0 ? 2000.0f : npcDistance == 2 ? 4500.0f : 3000.0f; }
	// The graphics page's values as they were at first.
	void resetGraphics();
	// Graphics+: the values of a set (GFX_PLUS_OWN changes nothing), and which
	// set the present values are (GFX_PLUS_OWN if none).
	void applyGfxPreset(u8 preset);
	u8 gfxPresetOf() const;
	float occlusionStrength() const { return float(occlusion) * 0.01f; }
	// How far the far layer reaches, cm (0: off).
	float farCm() const { return farLayer == 1 ? 25000.0f : farLayer == 2 ? 40000.0f : farLayer == 3 ? 60000.0f : 0.0f; }
	float windStrength() const { return wind == 1 ? 0.5f : wind == 2 ? 1.0f : wind == 3 ? 1.7f : 0.0f; }

	// settings.bin. load() leaves the defaults and returns false if the file
	// is missing or damaged.
	bool save(const char* path) const;
	bool load(const char* path);
};

} // namespace g3
