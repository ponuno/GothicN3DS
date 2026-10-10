// What a talk looks like to the screens: the words being said (the top
// screen's subtitle) and the ways to go on (the bottom screen's page).
// Npcs fills it in (npc_dialog.cpp); App passes it on.
#pragma once
#include "g3ds/base.h"

namespace g3 {

constexpr int TALK_ITEMS = 12;

struct TalkView {
	bool active = false;   // a talk is on: the player's character stands and listens
	bool choosing = false; // the ways to go on are up (else: words are said, or a moment passes)
	bool saying = false;   // a line is up
	bool heroSays = false; // ... and it is the player's
	char partner[48] = ""; // who is talked to (UTF-8)
	char speaker[48] = ""; // who says the line
	char line[200] = "";   // what is said (UTF-8)
	u32 count = 0;
	char items[TALK_ITEMS][96] = {};
	u32 version = 0;       // changes whenever anything shown changes
};

} // namespace g3
