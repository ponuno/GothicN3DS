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
constexpr u32 SETTINGS_VERSION = 7;

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
	u8* flags[] = {&dissolve, &invertY, &autoCamera, &autoTarget, &firstPerson, &hud, &report, &perception, &life, &fights, &immortal, &weaponBack, &showFps, &barrier};
	for (u8* f : flags) *f = *f ? 1 : 0;
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
