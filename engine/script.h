// A Daedalus virtual machine: runs the game's own scripts (g3ds/script.h).
//
// It behaves like the original's interpreter as far as well-formed scripts
// can tell, and where scripts are not well-formed - a function that forgets
// its return value, a member read through no instance - it does what the
// ZenKit project's interpreter does: carry on with a zero. Nothing here
// throws or allocates behind the caller's back in the middle of a frame;
// errors are counted (stats()) and the offending call is abandoned.
#pragma once
#include "g3ds/pak.h"
#include "g3ds/script.h"

namespace g3 {
struct ByteSink;
struct ByteSource;
} // namespace g3

namespace g3 {

// An object of a script class (a character, an item, ...): `size` bytes laid
// out as the class' members say. Created and owned by the machine.
struct ScriptInstance {
	u32 classSymbol;
	u32 symbol; // the instance definition it was made from (SCRIPT_NONE: none)
	u32 size;
	u32 serial;    // unique among the machine's instances, in order of creation
	void* user;    // for the host: the game object this belongs to
	u8* data() { return reinterpret_cast<u8*>(this + 1); }
	const u8* data() const { return reinterpret_cast<const u8*>(this + 1); }
};

struct ScriptStats {
	u32 errors = 0;           // things a well-formed script does not do
	u32 nullAccess = 0;       // members read or written through no object: answered with a zero.
	                          // The game's own scripts do it; the original shrugs it off too.
	u32 aborted = 0;          // calls given up (runaway loop, broken code)
	u32 stubCalls = 0;        // calls of externals nobody has bound
	u64 instructions = 0;
	u32 instances = 0;        // alive
	u32 instanceBytes = 0;
	u32 stringBytes = 0;      // strings the scripts have made, alive
	char lastError[96] = {};
};

class ScriptVm {
public:
	typedef void (*External)(ScriptVm& vm, void* user);

	ScriptVm() = default;
	~ScriptVm() { release(); }
	ScriptVm(const ScriptVm&) = delete;
	ScriptVm& operator=(const ScriptVm&) = delete;

	// Loads the image stored as `name` ("script/gothic").
	bool load(Pak& pak, const char* name);
	// Takes over `blob` (malloc'ed) if it is a valid image; otherwise frees it.
	bool adopt(u8* blob, u32 size);
	void release();
	bool loaded() const { return _blob != nullptr; }
	u32 imageBytes() const { return _blobSize; }

	// ---- symbols ----
	u32 symbolCount() const { return _h.symbolCount; }
	const ScriptSymbol& symbol(u32 i) const { return _symbols[i]; }
	// The symbol of that name (any letter case), or SCRIPT_NONE.
	u32 find(const char* name) const;
	u32 findHash(u32 nameHash) const;
	// The class a symbol belongs to: of an instance or prototype (through its
	// parents), of a member; SCRIPT_NONE if it has none.
	u32 classOf(u32 symbol) const;
	// The member `member` of class `cls` ("C_NPC", "NAME"), or SCRIPT_NONE.
	u32 findMember(const char* cls, const char* member) const;
	// The state that begins with this function (its loop and its end), or
	// null if the function is no state's beginning.
	const ScriptState* stateOf(u32 start) const;
	// A script function with code of its own (no external, no variable).
	bool isPlainFunction(u32 symbol) const;
	// The n-th parameter of a function.
	u32 parameter(u32 function, u32 n) const { return function + 1 + n; }

	// ---- externals ----
	// Gives the external function `name` an implementation. It takes its
	// arguments from the stack, last one first, and pushes its result if it
	// has one. False if the scripts have no such external.
	bool bind(const char* name, External fn, void* user);
	bool bindSymbol(u32 symbol, External fn, void* user);
	// True if the external function `symbol` has been given an implementation.
	bool isBound(u32 symbol) const;
	// Inside an external: the symbol of the function being called.
	u32 callee() const { return _fp ? _frames[_fp - 1].function : SCRIPT_NONE; }
	// Called for an external nobody has bound, after its arguments were
	// dropped and a zero pushed as its result. For statistics.
	void onUnbound(void (*fn)(u32 symbol, void* user), void* user) {
		_unbound = fn;
		_unboundUser = user;
	}

	// ---- running ----
	// Calls a script function; its arguments must have been pushed, first one
	// first. A result, if it has one, is left on the stack. False if the call
	// was abandoned.
	bool call(u32 function);
	// Calls a script function that takes nothing and gives its result: an
	// integer, or 0 if it returns none, is no such function or was abandoned.
	i32 callInt(u32 function);
	// Makes an object of the class `instanceSymbol` belongs to and runs the
	// instance's code on it (with SELF referring to it, as the engine does).
	ScriptInstance* create(u32 instanceSymbol);
	// The two halves of create(), for a host that has to register the object
	// before its code runs.
	ScriptInstance* allocate(u32 instanceSymbol);
	bool construct(ScriptInstance* inst);
	void destroy(ScriptInstance* inst);

	void pushInt(i32 v);
	void pushFloat(float v);
	void pushString(const char* s);
	void pushInstance(ScriptInstance* inst);
	i32 popInt();
	float popFloat();
	const char* popString(); // valid until the machine runs again
	ScriptInstance* popInstance();

	// ---- variables ----
	i32 getInt(u32 symbol, u32 index = 0, const ScriptInstance* ctx = nullptr) const;
	float getFloat(u32 symbol, u32 index = 0, const ScriptInstance* ctx = nullptr) const;
	const char* getString(u32 symbol, u32 index = 0, const ScriptInstance* ctx = nullptr) const;
	void setInt(u32 symbol, u32 index, ScriptInstance* ctx, i32 v);
	void setFloat(u32 symbol, u32 index, ScriptInstance* ctx, float v);
	void setString(u32 symbol, u32 index, ScriptInstance* ctx, const char* v);
	// What an instance symbol (a definition, or a variable like SELF) refers to.
	ScriptInstance* instanceOf(u32 symbol) const;
	void setInstance(u32 symbol, ScriptInstance* inst);
	// The object whose members unqualified names mean right now.
	ScriptInstance* current() const { return _current; }

	// Well-known globals (SCRIPT_NONE if the scripts lack them).
	u32 selfSymbol() const { return _self; }
	u32 otherSymbol() const { return _other; }
	u32 heroSymbol() const { return _hero; }

	// Called for every complaint and every access through no object: what it
	// is, the function it happened in and the symbol concerned (SCRIPT_NONE
	// where there is none). For reports.
	typedef void (*ErrorHook)(const char* what, u32 function, u32 symbol, void* user);
	void onError(ErrorHook fn, void* user) {
		_onError = fn;
		_onErrorUser = user;
	}

	// ---- saving ----
	// Every global variable (the numbers and the strings the scripts have
	// changed), and the members of one object. A state saved with one script
	// image is refused by another (loadGlobals returns false).
	void saveGlobals(ByteSink& out) const;
	bool loadGlobals(ByteSource& in);
	// The offsets of the string members of a class, in bytes (at most `max`).
	u32 stringMembers(u32 cls, u32* offsets, u32 max) const;
	void saveObject(const ScriptInstance* inst, const u32* strings, u32 stringCount, ByteSink& out) const;
	bool loadObject(ScriptInstance* inst, const u32* strings, u32 stringCount, ByteSource& in);

	const ScriptStats& stats() const { return _stats; }
	// Instructions one call() may take, with everything it sets off, before
	// it is given up as a runaway (a world's start-up is a single call).
	void setBudget(u32 instructions) { _budget = instructions; }
	void setQuiet(bool quiet) { _quiet = quiet; }

private:
	enum Kind : u8 { K_INT, K_FLOAT, K_REF, K_INST };
	struct Slot {
		u8 kind;
		u8 index;
		u16 pad;
		union {
			i32 i;
			float f;
			u32 sym;
		};
		ScriptInstance* ctx;
	};
	struct Frame {
		u32 returnPc;
		u32 base;     // stack height the caller is left with (plus a result)
		u32 function; // symbol
		ScriptInstance* context;
	};
	enum { STACK = 1024, FRAMES = 96, TEMPS = 32 };

	bool run(u32 symbol, u32 address);
	bool step();
	void enter(u32 symbol, u32 address, u32 returnPc);
	void leave();
	void callExternal(u32 symbol);
	u32 functionAt(u32 address) const;
	void push(const Slot& s);
	Slot pop();
	bool popRef(Slot& out);
	void fail(const char* what);
	void complain(const char* what, u32 symbol) const;
	void abandon(const char* what);
	u8* memberPtr(const ScriptSymbol& s, u32 index, const ScriptInstance* ctx, u32 bytes) const;
	const char* stringAt(u32 symbol, u32 index, const ScriptInstance* ctx) const;
	u32 newDynamic(const char* text);
	void freeDynamic(u32 handle);

	u8* _blob = nullptr;
	u32 _blobSize = 0;
	ScriptHeader _h {};
	const ScriptSymbol* _symbols = nullptr;
	const ScriptFunction* _functions = nullptr;
	const ScriptState* _states = nullptr;
	const u8* _code = nullptr;
	i32* _data = nullptr;              // the image's, written in place
	const char** _strings = nullptr;   // per slot: into the image, or owned
	u8* _stringOwned = nullptr;        // per slot
	ScriptInstance** _slots = nullptr; // instance references
	struct Bound {
		External fn;
		void* user;
	};
	Bound* _externals = nullptr;
	void (*_unbound)(u32, void*) = nullptr;
	void* _unboundUser = nullptr;
	ErrorHook _onError = nullptr;
	void* _onErrorUser = nullptr;

	ScriptInstance** _instances = nullptr; // all alive
	u32 _instanceCount = 0, _instanceCap = 0, _serial = 0;
	char** _dynamic = nullptr; // strings held by instances; handle = index + 1
	u32 _dynamicCount = 0, _dynamicCap = 0, _dynamicFree = 0;
	char* _temps[TEMPS] = {};
	u32 _tempNext = 0;

	Slot* _stack = nullptr;
	u32 _sp = 0;
	Frame* _frames = nullptr;
	u32 _fp = 0;
	u32 _pc = 0;
	ScriptInstance* _current = nullptr;
	u32 _self = SCRIPT_NONE, _other = SCRIPT_NONE, _hero = SCRIPT_NONE;
	u32 _budget = 64000000, _spent = 0;
	u32 _depth = 0; // nested call()s (a script calls an external that calls a script)
	bool _abandon = false;
	bool _quiet = false;
	mutable ScriptStats _stats;
};

} // namespace g3
