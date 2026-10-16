#include "lang.h"
#include "saves.h"

#include <stddef.h>
#include <string.h>
#include <time.h>

namespace g3 {

namespace {

void copyText(char* to, u32 size, const char* from) {
	if (!size) return;
	u32 n = 0;
	if (from)
		while (from[n] && n + 1 < size) ++n;
	// (Not in the middle of a UTF-8 letter.)
	while (n > 0 && from && n < strlen(from) && (u8(from[n]) & 0xC0u) == 0x80u) --n;
	if (n) memcpy(to, from, n);
	to[n] = 0;
}

} // namespace

void copySaveTitle(char* to, u32 size, const char* from) { copyText(to, size, from); }

u32 saveHeaderCheck(const SaveHeader& h) {
	const u8* p = reinterpret_cast<const u8*>(&h);
	u32 sum = 0x811C9DC5u;
	for (size_t i = 0; i < offsetof(SaveHeader, check); ++i) sum = (sum ^ p[i]) * 16777619u;
	return sum;
}

SaveHeader makeSaveHeader(const SaveInfo& info, bool picture) {
	SaveHeader h;
	memset(&h, 0, sizeof(h));
	h.magic = SAVE_MAGIC;
	h.version = SAVE_HEADER_VERSION;
	h.thumbW = picture ? u16(THUMB_W) : u16(0);
	h.thumbH = picture ? u16(THUMB_H) : u16(0);
	h.bytes = u32(sizeof(SaveHeader)) + u32(h.thumbW) * h.thumbH * 2u;
	copyText(h.title, sizeof(h.title), info.title);
	copyText(h.world, sizeof(h.world), info.world);
	h.day = info.day;
	h.timeOfDay = info.timeOfDay;
	h.chapter = info.chapter;
	h.playSeconds = info.playSeconds;
	h.savedAtLo = u32(u64(info.savedAt));
	h.savedAtHi = u32(u64(info.savedAt) >> 32);
	h.check = saveHeaderCheck(h);
	return h;
}

bool writeSaveHeader(FILE* f, const SaveInfo& info, const u16* thumb) {
	const SaveHeader h = makeSaveHeader(info, thumb != nullptr);
	if (fwrite(&h, sizeof(h), 1, f) != 1) return false;
	return !thumb || fwrite(thumb, size_t(THUMB_W) * THUMB_H * 2, 1, f) == 1;
}

bool readSaveHeader(FILE* f, SaveInfo& info, u16* thumb) {
	info = SaveInfo();
	info.used = true;
	const long start = ftell(f);
	u32 magic = 0;
	if (fread(&magic, 4, 1, f) != 1) return false;
	if (magic != SAVE_MAGIC) {
		// A file of before the header: the position starts here.
		fseek(f, start, SEEK_SET);
		info.readable = info.legacy = true;
		return true;
	}
	SaveHeader h;
	memcpy(&h, &magic, 4);
	if (fread(reinterpret_cast<u8*>(&h) + 4, sizeof(h) - 4, 1, f) != 1) return false;
	if (h.version != SAVE_HEADER_VERSION || h.check != saveHeaderCheck(h)) return false;
	const bool picture = h.thumbW == THUMB_W && h.thumbH == THUMB_H;
	if (h.bytes != sizeof(SaveHeader) + u32(h.thumbW) * h.thumbH * 2u || (!picture && (h.thumbW || h.thumbH))) return false;
	copyText(info.title, sizeof(info.title), h.title);
	h.world[sizeof(h.world) - 1] = 0;
	copyText(info.world, sizeof(info.world), h.world);
	info.day = h.day;
	info.timeOfDay = h.timeOfDay == h.timeOfDay && h.timeOfDay >= 0.0f && h.timeOfDay < 24.0f ? h.timeOfDay : 0.0f;
	info.chapter = h.chapter;
	info.playSeconds = h.playSeconds;
	info.savedAt = i64(u64(h.savedAtLo) | (u64(h.savedAtHi) << 32));
	info.picture = picture;
	if (picture) {
		if (thumb) {
			if (fread(thumb, size_t(THUMB_W) * THUMB_H * 2, 1, f) != 1) return false;
		} else if (fseek(f, long(THUMB_W) * THUMB_H * 2, SEEK_CUR) != 0) {
			return false;
		}
	}
	info.readable = true;
	return true;
}

bool readSaveInfo(const char* path, SaveInfo& info, u16* thumb) {
	info = SaveInfo();
	FILE* f = path ? fopen(path, "rb") : nullptr;
	if (!f) return false;
	bool ok = readSaveHeader(f, info, thumb);
	if (!ok) {
		info = SaveInfo();
		info.used = true; // (there, but damaged)
	}
	fclose(f);
	return ok;
}

const char* worldShortName(const char* asset) {
	if (!asset) return "";
	const char* slash = strrchr(asset, '/');
	return slash ? slash + 1 : asset;
}

const char* worldTitle(const char* world) {
	struct Known {
		const char* asset;
		const char* title;
	};
	static const Known kKnown[] = {
	    {"world", "Колония"},
	    {"oldmine", "Старая шахта"},
	    {"freemine", "Свободная шахта"},
	    {"orcgraveyard", "Кладбище орков"},
	    {"orctempel", "Храм Спящего"},
	};
	const char* w = worldShortName(world);
	for (const Known& k : kKnown) {
		const char* a = k.asset;
		const char* b = w;
		while (*a && *b && (*b == *a || *b == *a - 32)) ++a, ++b;
		if (!*a && (!*b || *b == '.')) return tr(k.title);
	}
	return w;
}

void formatGameTime(char* out, u32 size, i32 day, float timeOfDay) {
	const int minutes = int(timeOfDay * 60.0f + 0.5f) % (24 * 60);
	snprintf(out, size, tr("День %d, %02d:%02d"), int(day) + 1, minutes / 60, minutes % 60);
}

void formatPlayTime(char* out, u32 size, u32 seconds) {
	const u32 minutes = seconds / 60;
	if (minutes >= 60) snprintf(out, size, tr("%d ч %02d мин"), int(minutes / 60), int(minutes % 60));
	else snprintf(out, size, tr("%d мин"), int(minutes));
}

void formatSaveDate(char* out, u32 size, i64 savedAt) {
	if (!size) return;
	out[0] = 0;
	if (savedAt <= 0) return;
	// (The console's clock is local time already and its library knows no time zone: localtime is the clock as set.)
	const time_t t = time_t(savedAt);
	const struct tm* m = localtime(&t);
	if (!m) return;
	snprintf(out, size, "%02d.%02d.%04d %02d:%02d", m->tm_mday, m->tm_mon + 1, m->tm_year + 1900, m->tm_hour, m->tm_min);
}

void defaultSaveTitle(char* out, u32 size, const char* world, i32 day) {
	snprintf(out, size, tr("%s, день %d"), worldTitle(world), int(day) + 1);
}

} // namespace g3
