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
constexpr u32 SETTINGS_VERSION = 17;

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
	objects = d.objects;
	dissolve = d.dissolve;
	frameRate = d.frameRate;
	density = d.density;
	npcDistance = d.npcDistance;
	visibility = d.visibility;
	farGeometry = d.farGeometry;
	antialias = d.antialias;
	refresh = d.refresh;
	reproject = d.reproject;
	applyGfxPreset(GFX_PLUS_OFF);
}

void Settings::applyGfxPreset(u8 preset) {
	switch (preset) {
	case GFX_PLUS_OFF:
		occlusion = 0;
		wind = 0;
		shadows = SHADOWS_OFF;
		farLayer = 0;
		break;
	case GFX_PLUS_BALANCED:
		occlusion = 60;
		wind = 2;
		shadows = SHADOWS_HERO;
		farLayer = 1;
		break;
	case GFX_PLUS_MAX:
		occlusion = 100;
		wind = 2;
		shadows = SHADOWS_ALL;
		farLayer = 2;
		break;
	default: return;
	}
	gfxPlus = preset;
}

u8 Settings::gfxPresetOf() const {
	for (u8 p = 0; p < GFX_PLUS_OWN; ++p) {
		Settings t = *this;
		t.applyGfxPreset(p);
		if (t.occlusion == occlusion && t.wind == wind && t.shadows == shadows && t.farLayer == farLayer) return p;
	}
	return GFX_PLUS_OWN;
}

void Settings::sanitize() {
	drawMetres = u8(clamp<int>(drawMetres, 40, 200) / 10 * 10);
	if (objects >= OBJECT_MODES) objects = OBJECTS_SMOOTH;
	if (frameRate >= FRAME_MODES) frameRate = FRAMES_60;
	if (scheme >= CONTROL_SCHEMES) scheme = CONTROLS_CLASSIC;
	if (language > 3) language = 0;
	if (density > 2) density = 0;
	if (npcDistance > 2) npcDistance = 1;
	soundVolume = u8(clamp<int>(soundVolume, 0, 10));
	musicVolume = u8(clamp<int>(musicVolume, 0, 10));
	voiceVolume = u8(clamp<int>(voiceVolume, 0, 10));
	cameraSpeed = u8(clamp<int>(cameraSpeed, 1, 10));
	if (timeFlow > 3) timeFlow = 2;
	occlusion = u8(clamp<int>(occlusion, 0, 100) / 10 * 10);
	if (wind > 3) wind = 0;
	if (shadows >= SHADOW_MODES) shadows = SHADOWS_OFF;
	if (farLayer > 3) farLayer = 0;
	if (farGeometry > 3) farGeometry = 2;
	if (refresh > 5) refresh = 0;
	gfxPlus = gfxPresetOf();
	u8* flags[] = {&dissolve, &invertY, &autoCamera, &autoTarget, &firstPerson, &hud, &report, &perception, &life, &fights, &immortal, &weaponBack, &showFps, &barrier, &visibility, &antialias, &reproject, &readAhead, &stereo, &farFill};
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
	s.sanitize();
	*this = s;
	return true;
}

} // namespace g3
