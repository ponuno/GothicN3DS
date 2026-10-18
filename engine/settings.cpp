#include "settings.h"

#include <stdio.h>
#include <string.h>

namespace g3 {

namespace {

constexpr u32 SETTINGS_MAGIC = fourcc('G', '3', 'S', 'E');
// 2: the classic scheme became the default; a file of version 1 (whose
// scheme was the console one unless changed) begins with the classic one again.
// 3: the bottom screen shows the inventory unless the numbers are asked for
// (the "report" switch began again at "off"); the weapon back after a potion.
// 4: language, density of small objects, range of the inhabitants, the free frame rate.
// 5: the sound volume.
// 7: the frame counter, the flow of time.
// 8: Graphics+ (ambient occlusion, wind, shadows).
// 9: the far layer.
// 10: baked visibility.
// 11: simplified far geometry.
// 12: smoothing of edges.
// 13: the screen's refresh rate follows the frame.
// 14: the old picture turned sideways when a refresh has no new one.
// 15: reading ahead.
// 20: waves on the water.
// 21: weather, wet ground.
// 22: light shafts.
// 23: the sun's shadow.
// 24: the new grass.
// 25: the light of torches and fires.
// 26: English is the base language (a file of an older version that had Russian - the old default - gets English once).
// 27: the font of the interface (the game's own or the built-in one).
constexpr u32 SETTINGS_VERSION = 27;

// On disk: magic, version, the settings' bytes (as many as this version
// has), a check over all of it. A file of an older version with fewer bytes
// keeps the defaults for what it lacks.
struct FileHeader {
	u32 magic;
	u32 version;
	u32 size;
};

u32 checkOf(const u8* bytes, u32 size) {
	u32 sum = 0x9E3779B9u;
	for (u32 i = 0; i < size; ++i) sum = (sum ^ bytes[i]) * 16777619u;
	return sum;
}

} // namespace

void Settings::resetGraphics() {
	const Settings d;
	drawMetres = d.drawMetres;
	refresh = d.refresh;
	wetGround = d.wetGround;
	readAhead = d.readAhead;
	applyGfxPreset(GFX_PLUS_OFF);
}

// fix223: the sets now hold every value of the graphics (not only the four of Graphics+): the Graphics page chooses one,
// Graphics+ changes them one by one. Not in a set: the draw distance, the screen mode (3D / wide: a matter of taste and a
// restart), the Barrier, the screen's refresh (experimental), weather, wet ground and the reading ahead.
// The quality set is the player's own measured one (settings.bin of 18.10.2026: a steady 30 fps in Gothic 1, New 3DS).
namespace {
struct GfxSet {
	u8 objects, dissolve, frameRate, density, npcDistance, lamps;
	u8 occlusion, wind, waves, shadows, sunShadows, cloudShadows, grass, farLayer, farGeometry;
	u8 reproject, antialias, farFill, lodFade, godRays, visibility;
};
const GfxSet kGfxSets[GFX_PLUS_OWN] = {
    // Speed: 60 fps first (the defaults of a fresh settings.bin).
    {OBJECTS_SMOOTH, 1, FRAMES_60, 0, 1, 2, 0, 0, 0, SHADOWS_OFF, 1, 0, 0, 0, 2, 0, 0, 0, 1, 1, 0},
    // Balanced: 60 when it fits, the cheap extras on.
    {OBJECTS_BALANCED, 1, FRAMES_60, 0, 1, 2, 60, 2, 1, SHADOWS_HERO, 1, 0, 1, 1, 2, 0, 0, 1, 1, 0, 0},
    // Quality: a steady 30 and everything the player's measurement kept.
    {OBJECTS_BALANCED, 1, FRAMES_30, 0, 1, 2, 100, 3, 3, SHADOWS_ALL, 1, 0, 2, 3, 2, 0, 2, 1, 0, 0, 0},
};
#define GFX_FIELDS(X) X(objects) X(dissolve) X(frameRate) X(density) X(npcDistance) X(lamps) X(occlusion) X(wind) X(waves) \
	X(shadows) X(sunShadows) X(cloudShadows) X(grass) X(farLayer) X(farGeometry) X(reproject) X(antialias) X(farFill) X(lodFade) \
	X(godRays) X(visibility)
} // namespace

void Settings::applyGfxPreset(u8 preset) {
	if (preset >= GFX_PLUS_OWN) return;
	const GfxSet& g = kGfxSets[preset];
#define SET(f) f = g.f;
	GFX_FIELDS(SET)
#undef SET
	gfxPlus = preset;
}

u8 Settings::gfxPresetOf() const {
	for (u8 p = 0; p < GFX_PLUS_OWN; ++p) {
		const GfxSet& g = kGfxSets[p];
		bool same = true;
#define CMP(f) same = same && f == g.f;
		GFX_FIELDS(CMP)
#undef CMP
		if (same) return p;
	}
	return GFX_PLUS_OWN;
}

void Settings::sanitize() {
	drawMetres = u8(clamp<int>(drawMetres, 40, 200) / 10 * 10);
	if (objects >= OBJECT_MODES) objects = OBJECTS_SMOOTH;
	if (frameRate >= FRAME_MODES) frameRate = FRAMES_60;
	if (scheme >= CONTROL_SCHEMES) scheme = CONTROLS_CLASSIC;
	if (language > 3) language = 0;
	if (chapter != 255 && chapter > 6) chapter = 6;
	if (density > 2) density = 0;
	if (npcDistance > 2) npcDistance = 1;
	soundVolume = u8(clamp<int>(soundVolume, 0, 10));
	musicVolume = u8(clamp<int>(musicVolume, 0, 10));
	voiceVolume = u8(clamp<int>(voiceVolume, 0, 10));
	cameraSpeed = u8(clamp<int>(cameraSpeed, 1, 10));
	if (timeFlow > 3) timeFlow = 2;
	occlusion = u8(clamp<int>(occlusion, 0, 100) / 10 * 10);
	if (wind > 3) wind = 0;
	if (waves > 3) waves = 0;
	if (weather > 5) weather = 0;
	if (shadows >= SHADOW_MODES) shadows = SHADOWS_OFF;
	if (farLayer > 3) farLayer = 0;
	if (farGeometry > 3) farGeometry = 2;
	if (refresh > 5) refresh = 0;
	if (antialias > 2) antialias = 0;
	if (lodFade > 2) lodFade = 1;
	if (sunShadows > 3) sunShadows = 1;
	if (grass > 2) grass = 0;
	if (lamps > 2) lamps = 2;
	if (barrierThunder > 4) barrierThunder = 0;
	if (cloudShadows > 2) cloudShadows = 0;
	if (barrier > 2) barrier = 1; // (fix150: 0 off, 1 as in the game, 2 always; older files: 0 / 1)
	gfxPlus = gfxPresetOf();
	u8* flags[] = {&dissolve, &invertY, &autoCamera, &autoTarget, &firstPerson, &hud, &report, &perception, &life, &fights, &immortal, &weaponBack, &showFps, &visibility, &reproject, &readAhead, &farFill, &wetGround, &godRays};
	if (stereo > 2) stereo = 0;
	for (u8* f : flags) *f = *f ? 1 : 0;
}

float Settings::lodDistanceCm() const {
	switch (farGeometry) {
	case 1: return 2500.0f;
	case 2: return 4000.0f;
	case 3: return 6000.0f;
	default: return 0.0f;
	}
}

float Settings::mediumObjectsCm() const {
	switch (objects) {
	case OBJECTS_BALANCED: return 8500.0f;
	case OBJECTS_STEADY: return 10000.0f;
	default: return 7000.0f;
	}
}

float Settings::smallObjectsCm() const {
	switch (objects) {
	case OBJECTS_BALANCED: return 4500.0f;
	case OBJECTS_STEADY: return 6000.0f;
	default: return 3500.0f;
	}
}

float Settings::objectPixels() const {
	switch (objects) {
	case OBJECTS_BALANCED: return 5.0f;
	case OBJECTS_STEADY: return 4.0f;
	default: return 6.0f;
	}
}

float Settings::minDetail() const {
	switch (objects) {
	case OBJECTS_BALANCED: return 0.7f;
	case OBJECTS_STEADY: return 1.0f;
	default: return 0.35f;
	}
}

bool Settings::save(const char* path) const {
	FILE* f = fopen(path, "wb");
	if (!f) return false;
	const FileHeader h = {SETTINGS_MAGIC, SETTINGS_VERSION, u32(sizeof(Settings))};
	u8 bytes[sizeof(Settings)];
	memcpy(bytes, this, sizeof(bytes));
	const u32 check = checkOf(bytes, sizeof(bytes));
	bool ok = fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(bytes, sizeof(bytes), 1, f) == 1 && fwrite(&check, sizeof(check), 1, f) == 1;
	ok = (fclose(f) == 0) && ok;
	return ok;
}

bool Settings::load(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	FileHeader h {};
	u8 bytes[256] = {};
	u32 check = 0;
	bool ok = fread(&h, sizeof(h), 1, f) == 1 && h.magic == SETTINGS_MAGIC && h.version >= 1 && h.size > 0 && h.size <= sizeof(bytes);
	ok = ok && fread(bytes, h.size, 1, f) == 1 && fread(&check, sizeof(check), 1, f) == 1 && check == checkOf(bytes, h.size);
	fclose(f);
	if (!ok) return false;
	Settings s;
	memcpy(&s, bytes, min<u32>(h.size, sizeof(Settings)));
	if (h.version < 2) s.scheme = CONTROLS_CLASSIC;
	if (h.version < 3) s.report = 0;
	if (h.version < 18 && s.antialias == 1) s.antialias = 2; // (the old "on" is the full smoothing)
	if (h.version < 26 && s.language == 0) s.language = 1; // (Lang: 0 Russian, 1 English)
	s.sanitize();
	s.chapter = 255; // (never kept: it follows the game)
	*this = s;
	return true;
}

} // namespace g3
