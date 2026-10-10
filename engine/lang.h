// The language of the interface (menus, panel, hints, messages): Russian,
// English, Polish or German. The texts in the code are the Russian ones and
// stand as the keys: tr("Взято: %s") gives the text in the language set (or
// the key itself in Russian, and for a text the table lacks). A translation
// keeps the key's %-conversions in their order. What the game's own scripts
// say (names of things, dialogues) comes from its data and is not touched.
#pragma once
#include "g3ds/base.h"

namespace g3 {

enum Lang : u8 { LANG_RU, LANG_EN, LANG_PL, LANG_DE, LANG_COUNT };

void setLanguage(u8 lang);
u8 language();
const char* tr(const char* ru);

// For tests: how many texts the table has, the key and the text of one in a
// language, and how many asked-for texts it lacked.
u32 langEntries();
const char* langKey(u32 index);
const char* langText(u32 index, u8 lang);
u32 langMisses();

} // namespace g3
