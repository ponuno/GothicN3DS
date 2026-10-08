// The game's scripts as the importers hand them over: the symbol table and
// the bytecode of a compiled Daedalus file (GOTHIC.DAT), or the output of
// this project's own small compiler (daedalus.h).
#pragma once
#include "g3ds/script.h"

#include <string>
#include <vector>

namespace g3 {

struct SrcSymbol {
	std::string name; // upper case; empty for a symbol without one
	ScriptType type = SCRIPT_VOID;
	u32 count = 0; // elements of a variable; parameters of a function
	bool isConst = false;
	bool hasReturn = false;
	bool member = false;   // a variable of a class
	bool external = false; // a function of the engine
	ScriptType returnType = SCRIPT_VOID;
	i32 parent = -1;
	u32 address = 0;      // code of a function, prototype or instance
	u32 classSize = 0;    // as the file says; the baker lays classes out itself
	u32 memberOffset = 0; // likewise
	std::vector<i32> ints;            // an int's `count` values
	std::vector<float> floats;        // a float's
	std::vector<std::string> strings; // a string's
};

struct SrcScript {
	std::vector<SrcSymbol> symbols;
	std::vector<u8> code;
	std::string source; // where it came from, for the report
	std::vector<std::string> notes;

	// The symbol of that name (any letter case), or -1.
	int find(const std::string& name) const;
};

struct ScriptBakeResult {
	std::string report;
	u32 symbols = 0, functions = 0, externals = 0, instances = 0, classes = 0;
	u32 hashClashes = 0;
};

// Builds the G3SC image (g3ds/script.h). Throws std::runtime_error on a
// script that cannot be baked.
std::vector<u8> bakeScript(const SrcScript& script, ScriptBakeResult& result);

// The same script as a compiled Daedalus file, as the game's own compiler
// writes them. For the tests: it is what the importer and the reference
// interpreter read.
std::vector<u8> writeScriptDat(const SrcScript& script);
// Reads such a file. Throws std::runtime_error.
void readScriptDat(const std::vector<u8>& file, SrcScript& script);

} // namespace g3
