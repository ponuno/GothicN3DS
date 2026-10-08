// A small compiler for Daedalus, the game's script language.
//
// The game itself ships its scripts compiled, and those are what the baker
// reads (GOTHIC.DAT). This compiler is for everything that has no game files:
// the script of the built-in test scene, and the tests, which run the same
// source through this project's interpreter and the reference one.
//
// It knows the language as the game's scripts use it - constants, variables
// and arrays, classes, prototypes and instances, functions, if/else, the
// usual operators - and produces what the game's compiler produces: the same
// symbol table (parameters and locals after their function, members after
// their class, a symbol per string literal) and the same code sequences.
// One addition: engine functions are declared in the source,
//     extern func int Hlp_Random(var int bound);
#pragma once
#include "script_src.h"

#include <string>

namespace g3 {

// Throws std::runtime_error ("line 12: ...") on a source it cannot compile.
void compileDaedalus(const std::string& source, SrcScript& out);

} // namespace g3
