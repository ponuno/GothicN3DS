// fix195: Ikarus (and the parts of LeGo built on its memory) on this engine.
//
// Ikarus gives Daedalus scripts the game process' memory: MEM_Alloc, MEM_ReadInt, _@ (the address of a variable), _^
// (an object at an address), calls of script functions by number, loops written as calls (while / repeat / end).
// None of that exists here, so its primitives are carried out by the engine instead of their script code
// (ScriptVm::override), on the memory of script_mem.h; the loops were already rewritten by the baker. What is built
// on the primitives - Ikarus' arrays and strings, LeGo's StringBuilder, hash tables, lists, PermMem - runs as the
// scripts have it. Calls of the original's machine code (ASM_*, CALL_*) are skipped (SCRIPTF_NULLED), and so are
// LeGo's hooks of the engine's functions: those are only listed (hooks()), for the engine to take up one by one.
#pragma once
#include "script_mem.h"

#include <string.h>

namespace g3 {

class Ikarus {
public:
	enum { MAX_NOTES = 8, MAX_HOOKS = 256 };
	struct Stats {
		u32 errors = 0, warnings = 0, infos = 0; // MEM_Error / MEM_Warn / MEM_Info and the like
		u32 bound = 0;                          // functions carried out by the engine
		u32 refused = 0;                        // of those wanted, not taken: the script's differs
		u32 hooks = 0;                          // LeGo's hooks of engine functions asked for (HookEngine*)
		u32 frames = 0;                         // frame functions run (LeGo's _FF_Hook)
	};
	struct Hook {
		u32 address;  // in the original's code
		u32 function; // the script function to run there
	};

	// True if the scripts have Ikarus; the machine then has its stand-ins. `maxHeap`: bytes for the scripts' heap.
	bool attach(ScriptVm& vm, u32 maxHeap);
	void detach();
	bool active() const { return _vm != nullptr; }
	ScriptMemory& memory() { return _mem; }

	// Each frame of the game: the time (MEM_Timer, LeGo's Timer) and LeGo's frame functions (its hook of
	// oCGame::Render).
	void frame(float ms, bool paused);
	// One of the original's functions the scripts hooked (HookEngineF) is "called" by this engine: the hooks the scripts
	// put on it run. `name`: the constant the scripts hold its address in (LeGo's "OCGAME__CHANGELEVEL"). `stackSlot`
	// / `frameSlot`: values the hooks read from the original's registers (ESP + 4, EBP + 60 ...): (offset, value)
	// pairs, up to two each. Returns how many hooks ran.
	u32 fire(const char* name, u32 espOffset = 0, i32 espValue = 0, u32 ebpOffset = 0, i32 ebpValue = 0);
	// The game changes worlds (LeGo: oCGame::changeLevel ... its end).
	u32 fireAt(u32 address, u32 espOffset, i32 espValue, u32 ebpOffset, i32 ebpValue);
	void levelChangeBegin() { fire("OCGAME__CHANGELEVEL"); }
	void levelChangeEnd() { fire("OCGAME__CHANGELEVELEND"); }
	// LeGo's AI_Function: an animation "CALL <kind> <arguments> <function>" in a character's queue is a call of a
	// script function when the queue gets there, with SELF the character (the original runs it from LeGo's hook of
	// oCNpc::EV_PlayAni). True if `ani` is one. callLater() keeps the text (an action's own is too short) and
	// returns its ticket; callNow() runs one.
	static bool isCall(const char* ani) { return ani && !strncmp(ani, "CALL ", 5); }
	bool callNow(ScriptInstance* self, const char* ani);
	i32 callLater(const char* ani);
	bool callTicket(ScriptInstance* self, i32 ticket);
	// The save slot being read or written (LeGo's _BR_GetSelectedSlot: its PermMem file goes with it).
	void setSlot(i32 slot) { _slot = slot; }
	i32 slot() const { return _slot; }

	// Files the scripts write and read (LeGo's binary machines: WIN_CreateFile ...), kept in memory; a saved game
	// carries them (save / load).
	struct File {
		char name[96];
		u8* data;
		u32 size, cap;
	};
	u32 fileCount() const { return _fileCount; }
	const File& file(u32 i) const { return _files[i]; }
	File* openFile(const char* name, bool create);
	void saveFiles(ByteSink& out) const;
	bool loadFiles(ByteSource& in);

	const Stats& stats() const { return _stats; }
	// The first messages of each kind ("error: ...").
	u32 noteCount() const { return _noteCount; }
	const char* note(u32 i) const { return i < _noteCount ? _notes[i] : ""; }
	// The script function that called the function that complained (SCRIPT_NONE: unknown).
	u32 noteCaller(u32 i) const { return i < _noteCount ? _noteCallers[i] : SCRIPT_NONE; }
	// The script's functions it wanted carried out but whose declaration differs from the expected one.
	const char* refusedList() const { return _refusedList; }
	u32 hookCount() const { return _hookCount; }
	const Hook& hook(u32 i) const { return _hooks[i]; }

	// For the natives (ikarus.cpp).
	void message(const char* kind, const char* text);
	void addHook(u32 address, u32 function);
	void initGlobals();
	u32 gameVersion() const { return _version; }
	u32 timeMs() const { return u32(_time); }
	u32 sbCurrent() const { return _sbCurrent; } // LeGo's _SB_Current (found once)
	void setReplacement(u32 replaced, u32 by);
	u32 replacement(u32 replaced) const;

private:
	bool bind(const char* name, ScriptVm::External fn, const char* signature, bool naked = false);

	ScriptVm* _vm = nullptr;
	ScriptMemory _mem;
	Stats _stats;
	u32 _version = 2; // the game the scripts' Ikarus is for (GOTHIC_BASE_VERSION)
	double _time = 0;
	u32 _timer = 0;        // MEM_Timer's object (an address), and the members it keeps
	u32 _game = 0;         // MEM_Game's
	// Found once (a search by name walks the whole table): the timer's members' offsets, the game's time step, the
	// registers, the address of oCGame::Render.
	enum { T_FACTOR, T_FRAMEF, T_TOTALF, T_FRAMEFS, T_TOTALFS, T_FRAME, T_TOTAL, T_COUNT };
	u32 _timerOff[T_COUNT] = {};
	u32 _stepOff = SCRIPT_NONE;
	u32 _esp = SCRIPT_NONE, _ebp = SCRIPT_NONE;
	u32 _renderAddress = 0; // (0: none)
	u32 _sbCurrent = SCRIPT_NONE;
	char _notes[MAX_NOTES][120] = {};
	u32 _noteCount = 0;
	u32 _noteCallers[MAX_NOTES] = {};
	char _refusedList[200] = {};
	i32 _slot = 0;
	File _files[16] = {};
	u32 _fileCount = 0;
	u32 _regs = 0; // a block standing in for the original's stack and frame (ESP, EBP) when a hook runs
	Hook _hooks[MAX_HOOKS] = {};
	u32 _hookCount = 0;
	u32 _replaced[64][2] = {};
	enum { CALLS = 64 };
	char* _calls[CALLS] = {};
	u32 _callNext = 0;
	u32 _self = SCRIPT_NONE, _other = SCRIPT_NONE, _item = SCRIPT_NONE;
	u32 _replacedCount = 0;
};

} // namespace g3
