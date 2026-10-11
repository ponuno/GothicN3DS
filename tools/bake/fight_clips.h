// The animations of fighting. The scripts never name them - the engine
// makes their names up from the weapon in hand ("S_1HRUN", "T_FISTATTACKMOVE"),
// as ZenGin does and as OpenGothic's AnimationSolver lays out - so the baker
// has to bring them along by itself: for the player's character, and for
// every skeleton of the inhabitants (what a model script does not declare is
// passed over, see ZenActorOptions::onlyDeclared).
#pragma once
#include <string>
#include <vector>

namespace g3 {

// The weapon words of the names: fists (creatures always fight "with fists"),
// one-handed and two-handed weapons.
inline const std::vector<std::string>& fightWeapons() {
	static const std::vector<std::string> w = {"FIST", "1H", "2H"};
	return w;
}

// Standing and going with a weapon in hand: the states the transitions
// between which are wanted too ("T_1HRUN_2_1HRUNL").
inline std::vector<std::string> fightStates() {
	std::vector<std::string> out;
	for (const std::string& w : fightWeapons())
		for (const char* s : {"RUN", "RUNL", "WALK", "WALKL"}) out.push_back(w + s);
	for (const char* s : {"MAGRUN", "MAGRUNL", "MAGWALK", "MAGWALKL"}) out.push_back(s); // (the player's character in the magic stance)
	for (const char* w : {"BOW", "CBOW"})
		for (const char* s : {"RUN", "RUNL", "WALK", "WALKL"}) out.push_back(std::string(w) + s); // (and with bow and crossbow)
	return out;
}

// The letters of the spells' animations (the scripts' spellFXAniLetters): "T_MAGRUN_2_FIBCAST".
inline const std::vector<std::string>& spellAniLetters() {
	static const std::vector<std::string> l = {"SLE", "FIB", "TRF", "FEA", "HEA", "SUM", "WND", "TEL", "PYR", "CON", "FBT", "LIN", "FRZ", "NE2", "NE3", "NE4", "NE5"};
	return l;
}

// Magic in the hands: the stance (word "MAG": "S_MAGRUN"), taking it in hand and away, and the casting of
// every kind of spell. The player's character only.
inline std::vector<std::string> magicClipNames() {
	std::vector<std::string> out;
	for (const char* name : {"S_MAGRUN", "S_MAGRUNL", "S_MAGWALK", "S_MAGWALKL", "T_MAGRUN_2_MAGRUNL", "T_MAGRUNL_2_MAGRUN", "T_MAGWALK_2_MAGWALKL",
	                         "T_MAGWALKL_2_MAGWALK", "T_MAG_2_MAGRUN", "T_MAGRUN_2_MAG", "T_RUN_2_MAG", "T_MAG_2_RUN", "T_MOVE_2_MAGMOVE",
	                         "T_MAGMOVE_2_MOVE", "T_MAGRUN_2_MAGRUN", "T_CASTFAIL", "T_MAGRUNSTRAFEL", "T_MAGRUNSTRAFER", "T_MAGRUNTURNL",
	                         "T_MAGRUNTURNR", "T_MAGWALKTURNL", "T_MAGWALKTURNR", "T_MAGJUMPB"})
		out.push_back(name);
	for (const std::string& l : spellAniLetters())
		for (const char* f : {"T_MAGRUN_2_%CAST", "T_MAGRUN_2_%SHOOT", "T_MAGMOVE_2_%CAST", "T_MAGMOVE_2_%SHOOT", "S_%CAST", "S_%SHOOT",
		                      "T_%CAST_2_SHOOT", "T_%SHOOT_2_STAND", "T_STAND_2_%CAST", "T_%CAST_2_STAND"}) {
			std::string name;
			for (const char* c = f; *c; ++c) {
				if (*c == '%') name += l;
				else name += *c;
			}
			out.push_back(name);
		}
	return out;
}

// Bow and crossbow in the hands (word "BOW", "CBOW"): the stance, drawing, aiming, shooting, reloading.
inline std::vector<std::string> bowClipNames() {
	std::vector<std::string> out;
	for (const char* w : {"BOW", "CBOW"})
		for (const char* f : {"S_%RUN", "S_%RUNL", "S_%WALK", "S_%WALKL", "T_%RUN_2_%RUNL", "T_%RUNL_2_%RUN", "T_%WALK_2_%WALKL",
		                      "T_%WALKL_2_%WALK", "T_%_2_%RUN", "T_%RUN_2_%", "T_RUN_2_%", "T_%_2_RUN", "T_MOVE_2_%MOVE", "T_%MOVE_2_MOVE",
		                      "S_%AIM", "S_%SHOOT", "T_%RELOAD", "T_%RUNSTRAFEL", "T_%RUNSTRAFER", "T_%RUNTURNL", "T_%RUNTURNR",
		                      "T_%WALKTURNL", "T_%WALKTURNR", "T_%JUMPB"}) {
			std::string name;
			for (const char* c = f; *c; ++c) {
				if (*c == '%') name += w;
				else name += *c;
			}
			out.push_back(name);
		}
	return out;
}

// `hero`: the player's character gets sneaking too.
inline std::vector<std::string> fightClipNames(bool hero) {
	std::vector<std::string> out;
	for (const std::string& w : fightWeapons()) {
		for (const char* f : {"S_%RUN", "S_%RUNL", "S_%WALK", "S_%WALKL", "T_%RUNSTRAFEL", "T_%RUNSTRAFER", "T_%WALKWSTRAFEL",
		                      "T_%WALKWSTRAFER", "T_%RUNTURNL", "T_%RUNTURNR", "T_%WALKTURNL", "T_%WALKTURNR", "T_%JUMPB",
		                      "T_%PARADEJUMPB", "S_%ATTACK", "T_%ATTACKL", "T_%ATTACKR", "T_%ATTACKMOVE", "T_%PARADE_O",
		                      "T_%PARADE_0", "T_%PARADE_0_A2", "T_%PARADE_0_A3", "T_%SFINISH", "T_RUN_2_%", "T_%_2_%RUN",
		                      "T_%RUN_2_%", "T_%_2_RUN", "T_MOVE_2_%MOVE", "T_%MOVE_2_MOVE"}) {
			std::string name;
			for (const char* c = f; *c; ++c) {
				if (*c == '%') name += w;
				else name += *c;
			}
			out.push_back(name);
		}
	}
	for (const char* name : {"T_STUMBLE", "T_STUMBLEB", "T_GOTHIT", "T_DEAD", "T_DEADB", "S_DEAD", "S_DEADB", "T_STAND_2_WOUNDED",
	                         "S_WOUNDED", "T_WOUNDED_2_STAND", "T_STAND_2_WOUNDEDB", "S_WOUNDEDB", "T_WOUNDEDB_2_STAND",
	                         "T_WOUNDED_TRY", "T_WOUNDED_2_DEAD", "T_WOUNDEDB_2_DEADB", "T_SURPRISE_CW", "T_SURPRISE_CCW"})
		out.push_back(name);
	if (hero)
		for (const char* name : {"S_SNEAK", "S_SNEAKL", "S_SNEAKBL", "T_SNEAK_2_SNEAKL", "T_SNEAKL_2_SNEAK", "T_SNEAKTURNL",
		                         "T_SNEAKTURNR", "T_SNEAKSTRAFEL", "T_SNEAKSTRAFER", "T_SNEAKBL_2_SNEAK", "T_SNEAK_2_SNEAKBL"})
			out.push_back(name);
	return out;
}

// Using things (a potion, food): the player's character brings an item to
// its mouth as the model script has it for the item's scheme (C_ITEM.SCEMENAME).
// Asked for in case the model has them.
inline std::vector<std::string> itemUseClipNames() {
	std::vector<std::string> out;
	for (const char* scheme : {"POTION", "POTIONFAST", "FOOD", "FOODHUGE", "MEAT", "RICE", "JOINT"})
		for (const char* f : {"T_%_STAND_2_S0", "S_%_S0", "T_%_S0_2_STAND"}) {
			std::string name;
			for (const char* c = f; *c; ++c) {
				if (*c == '%') name += scheme;
				else name += *c;
			}
			out.push_back(name);
		}
	// Picking a thing up from the ground.
	for (const char* name : {"T_STAND_2_IGET", "S_IGET", "T_IGET_2_STAND", "C_STAND_2_IGET_1", "C_IGET_1", "C_IGET_2_STAND_1"})
		out.push_back(name);
	// Opening a chest: bending to it, the lid, and back.
	for (const char* scheme : {"CHESTBIG", "CHESTSMALL"})
		for (const char* f : {"T_%_STAND_2_S0", "S_%_S0", "T_%_S0_2_S1", "S_%_S1", "T_%_S1_2_S0", "T_%_S0_2_STAND", "T_%_S1_2_STAND"}) {
			std::string name;
			for (const char* c = f; *c; ++c) {
				if (*c == '%') name += scheme;
				else name += *c;
			}
			out.push_back(name);
		}
	// Climbing onto a ledge.
	for (const char* name : {"T_JUMPUPLOW", "T_STAND_2_JUMPUPLOW", "T_JUMPUPMID", "T_STAND_2_JUMPUPMID", "T_JUMPUP", "T_STAND_2_JUMPUP",
	                         "T_JUMP_2_HANG", "T_STAND_2_HANG", "S_HANG", "T_HANG_2_STAND", "T_RUNL_2_JUMPUP", "T_JUMPUPLOW_2_STAND",
	                         "T_JUMPUPMID_2_STAND"})
		out.push_back(name);
	return out;
}

// Names the model script may have for climbing and picking up under other
// names than those above: every animation of the player's character that has
// one of these words in it is brought along, and the report lists them.
inline bool isHeroReachFamily(const std::string& upperName) {
	for (const char* word : {"JUMPUP", "HANG", "CLIMB", "LEDGE", "IGET", "PLUNDER", "PICKUP", "TAKE", "CHEST"})
		if (upperName.find(word) != std::string::npos) return true;
	return false;
}

} // namespace g3
