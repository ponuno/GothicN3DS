// The game's scripts as the importers hand them over: the symbol table and
// the bytecode of a compiled Daedalus file (GOTHIC.DAT), or the output of
// this project's own small compiler (daedalus.h).
#pragma once
#include "g3ds/script.h"

#include <string>
#include <utility>
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

// What people say (the game's OU.BIN): the name the scripts give a line, and
// its text, as the game's files have them (in the code page of the copy).
struct SrcDialogue {
	std::string source; // where it came from, for the report
	std::vector<std::pair<std::string, std::string>> lines;
};

// Cuts the lines into the assets of g3ds/dialog.h: "dialog/index" and
// "dialog/<k>". `report` gets a line about them.
struct DialogueAsset {
	std::string name;
	std::vector<u8> bytes;
};
std::vector<DialogueAsset> bakeDialogue(const SrcDialogue& dialogue, std::string& report);

struct ScriptBakeResult {
	std::string report;
	u32 symbols = 0, functions = 0, externals = 0, instances = 0, classes = 0;
	u32 hashClashes = 0;
	std::vector<std::string> stateNames; // the functions that begin a state ("ZS_SLEEP"), for reports
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
