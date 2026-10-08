// G3SC: the game's compiled Daedalus scripts, repacked for the console.
//
// The game ships its scripts compiled (GOTHIC.DAT): a table of symbols -
// variables, constants, classes and their members, functions, prototypes,
// instances - and one block of bytecode. The baker turns that file into this
// image, which the console maps as it is: fixed-size symbols, the names
// reduced to hashes, variables and strings gathered into flat tables. The
// bytecode is the original's, byte for byte; symbol numbers and code
// addresses are the original's too.
//
//   ScriptHeader | ScriptSymbol[symbolCount] | ScriptFunction[functionCount] |
//   i32 data[dataCount] | u32 stringOffset[stringCount] | char strings[stringBytes] |
//   u8 code[codeSize]                                  (every table padded to 4 bytes)
#pragma once
#include "g3ds/base.h"

namespace g3 {

constexpr u32 SCRIPT_MAGIC = fourcc('G', '3', 'S', 'C');
constexpr u32 SCRIPT_VERSION = 1;
constexpr u32 SCRIPT_NONE = 0xFFFFFFFFu;

enum ScriptType : u8 {
	SCRIPT_VOID = 0,
	SCRIPT_FLOAT = 1,
	SCRIPT_INT = 2,
	SCRIPT_STRING = 3,
	SCRIPT_CLASS = 4,
	SCRIPT_FUNC = 5,
	SCRIPT_PROTOTYPE = 6,
	SCRIPT_INSTANCE = 7,
};

enum ScriptFlags : u8 {
	SCRIPTF_CONST = 1 << 0,
	SCRIPTF_RETURN = 1 << 1,   // a function with a return value
	SCRIPTF_MEMBER = 1 << 2,   // a member of a class: lives in an instance
	SCRIPTF_EXTERNAL = 1 << 3, // a function the engine provides
};

struct ScriptHeader {
	u32 magic;
	u32 version;
	u32 symbolCount;
	u32 functionCount; // entries of the address table
	u32 dataCount;     // 32-bit words: every int and float outside the classes
	u32 stringCount;   // string slots outside the classes
	u32 stringBytes;
	u32 codeSize;
	u32 instanceSlots; // one per symbol that can refer to an instance
	u32 externalCount; // externals are numbered 0 .. externalCount - 1
	u32 reserved[6];
};
static_assert(sizeof(ScriptHeader) == 64, "ScriptHeader layout");

// `value` by kind of symbol:
//   member of a class               its offset in an instance, bytes
//   class                           the size of an instance, bytes
//   int, float                      the first of its `count` words in `data`
//   string                          the first of its `count` slots
//   function, prototype, instance   the address of its code (SCRIPT_NONE: none)
//   external function               its number among the externals
//   function variable               its word in `data` (it holds a symbol number)
// `extra`:
//   function                        its return type (ScriptType)
//   instance (definition or variable)   its instance slot
struct ScriptSymbol {
	u32 nameHash; // hashName() of the name; 0 for a symbol without one
	u8 type;      // ScriptType
	u8 flags;     // ScriptFlags
	u16 count;    // elements of an array; parameters of a function
	u32 value;
	i32 parent; // the class of a member, the class or prototype of an instance; -1 for none
	u32 extra;
};
static_assert(sizeof(ScriptSymbol) == 20, "ScriptSymbol layout");

// Code addresses of functions, prototypes and instances, sorted by address.
struct ScriptFunction {
	u32 address;
	u32 symbol;
};
static_assert(sizeof(ScriptFunction) == 8, "ScriptFunction layout");

// The original's opcodes.
enum ScriptOp : u8 {
	OP_ADD = 0, OP_SUB = 1, OP_MUL = 2, OP_DIV = 3, OP_MOD = 4, OP_OR = 5, OP_ANDB = 6, OP_LT = 7, OP_GT = 8,
	OP_MOVI = 9, OP_ORR = 11, OP_AND = 12, OP_LSL = 13, OP_LSR = 14, OP_LTE = 15, OP_EQ = 16, OP_NEQ = 17, OP_GTE = 18,
	OP_ADDMOVI = 19, OP_SUBMOVI = 20, OP_MULMOVI = 21, OP_DIVMOVI = 22,
	OP_PLUS = 30, OP_NEGATE = 31, OP_NOT = 32, OP_CMPL = 33, OP_NOP = 45,
	OP_RSR = 60, OP_BL = 61, OP_BE = 62, OP_PUSHI = 64, OP_PUSHV = 65, OP_PUSHVI = 67,
	OP_MOVS = 70, OP_MOVSS = 71, OP_MOVVF = 72, OP_MOVF = 73, OP_MOVVI = 74, OP_B = 75, OP_BZ = 76,
	OP_GMOVI = 80, OP_PUSHVV = 245,
};

// A string member of a class takes this much room in an instance (the
// original engine's string object); the image's offsets count with it.
constexpr u32 SCRIPT_STRING_SIZE = 20;

} // namespace g3
