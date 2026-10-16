#include "lang.h"

#include <string.h>

namespace g3 {

namespace {

struct Entry {
	const char* text[LANG_COUNT]; // Russian (the key), English, Polish, German
};

#include "lang_table.inc"

constexpr u32 ENTRIES = sizeof(kEntries) / sizeof(kEntries[0]);
constexpr u32 SLOTS = 2048; // (a power of two, at least twice ENTRIES)
static_assert(ENTRIES * 2 <= SLOTS, "lang slots");

u8 gLang = LANG_RU;
u16 gSlot[SLOTS]; // entry + 1, 0 for an empty slot
bool gBuilt = false;
u32 gMisses = 0;

u32 hashOf(const char* s) {
	u32 h = 2166136261u;
	for (; *s; ++s) h = (h ^ u8(*s)) * 16777619u;
	return h;
}

void build() {
	gBuilt = true;
	for (u32 i = 0; i < ENTRIES; ++i) {
		u32 k = hashOf(kEntries[i].text[LANG_RU]) & (SLOTS - 1);
		while (gSlot[k]) k = (k + 1) & (SLOTS - 1);
		gSlot[k] = u16(i + 1);
	}
}

} // namespace

void setLanguage(u8 lang) { gLang = lang < LANG_COUNT ? lang : u8(LANG_RU); }

u8 language() { return gLang; }

const char* tr(const char* ru) {
	if (!ru || gLang == LANG_RU || !*ru) return ru;
	if (!gBuilt) build();
	for (u32 k = hashOf(ru) & (SLOTS - 1); gSlot[k]; k = (k + 1) & (SLOTS - 1)) {
		const Entry& e = kEntries[gSlot[k] - 1];
		if (strcmp(e.text[LANG_RU], ru) == 0) return e.text[gLang] ? e.text[gLang] : ru;
	}
	++gMisses;
	return ru;
}

u32 langEntries() { return ENTRIES; }
const char* langKey(u32 index) { return index < ENTRIES ? kEntries[index].text[LANG_RU] : ""; }
const char* langText(u32 index, u8 lang) { return index < ENTRIES && lang < LANG_COUNT ? kEntries[index].text[lang] : ""; }
u32 langMisses() { return gMisses; }

} // namespace g3
