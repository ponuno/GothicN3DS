// The fight AI tables ("script/fight", engine/fight.h): which moves a fighter
// of each tactic picks from in each situation of a fight. Gothic 1 compiles
// them into a file of their own (FIGHT.DAT, from Fight.src: the C_FightAI
// instances "FA_ENEMY_PREHIT_3" ...), which the engine of the original reads
// beside GOTHIC.DAT; this bakes them into a table the engine reads directly.
#pragma once
#include "script_src.h"

#include <string>
#include <vector>

namespace g3 {

// The table from a compiled fight script. `report` gets a line: how many
// tactics have tables. Throws std::runtime_error on a script that has none.
std::vector<u8> bakeFightAi(const SrcScript& fight, std::string& report);

} // namespace g3
