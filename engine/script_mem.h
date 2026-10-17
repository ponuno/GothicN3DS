// fix195: Ikarus' memory. Scripts written for Ikarus compute with 32-bit addresses (MEM_Alloc, _@, _^, MEM_ReadInt,
// ...) that in the original are addresses in the game's process. Here they are addresses in a space of this
// engine's own, mapped onto what it has:
//
//   0x01000000 + 4 k            the machine's global words (ScriptVm::dataWords): ints, floats, function variables
//   0x10000000 + k              the heap the scripts allocate from (MEM_Alloc); grown as needed, up to a limit
//   0x20000000 + 32 id          a string variable's engine string object (zString: 0, 0, chars at +8, length at
//                               +12, reserved at +16), made when its address is asked for (_@s)
//   0x21000000 + id << 14 + k   that string's characters
//   0x32000000 + 4 i            the parser's table of symbols: the address of symbol i (currSymbolTableAddress)
//   0x36000000 + 64 i           symbol i as the original's parser has it (zCPar_Symbol: name, content, offset,
//                               bitfield, parent ...), made up from the machine's (read only, but for an instance's
//                               object: Ikarus' MEM_AssignInst through the table)
//   0x38000000 + k              the code (read only): MEM_GetFuncPtr
//   0x40000000 + id << 13 + k   an object of the machine (a C_NPC, a C_ITEM, ...): its bytes
//
// Whatever lies outside reads as 0 and swallows writes (counted: Stats::badReads / badWrites); nothing a script
// does here can touch the engine's own memory. The heap is a single block of the engine's memory, so a script
// that allocates without end costs at most `maxHeap` bytes.
//
// The heap's allocator hands out power-of-two blocks (8 bytes to 16 MB) behind an 8-byte header and keeps a free
// list per size; blocks are not merged. Simple and fast; Ikarus and LeGo allocate small things, over and over.
#pragma once
#include "script.h"

namespace g3 {

struct ByteSink;
struct ByteSource;

class ScriptMemory {
public:
	enum : u32 {
		DATA_BASE = 0x01000000u,
		HEAP_BASE = 0x10000000u,
		HEAP_END = 0x20000000u,
		STRV_BASE = 0x20000000u,
		STRC_BASE = 0x21000000u,
		STRC_SHIFT = 14,
		MAX_VIEWS = 16384,
		STRC_END = 0x31000000u,
		SYMT_BASE = 0x32000000u,
		SYMS_BASE = 0x36000000u,
		SYMS_SIZE = 64,
		CODE_BASE = 0x38000000u,
		OBJ_BASE = 0x40000000u,
		OBJ_SHIFT = 13,
		MAX_OBJECTS = (0x40000000u >> OBJ_SHIFT) - 1,
	};
	struct Stats {
		u32 heapBytes = 0;     // the heap's size (reserved)
		u32 heapUsed = 0;      // in blocks handed out (their whole size)
		u32 heapPeak = 0;
		u32 blocks = 0;        // handed out now
		u32 allocs = 0, frees = 0, refused = 0, badFrees = 0;
		u32 badReads = 0, badWrites = 0;
		u32 views = 0;         // objects made of memory (_^), alive
		u32 strings = 0;       // string variables with an address (_@s)
		u32 objects = 0;       // objects of the machine with an address
	};

	ScriptMemory() = default;
	~ScriptMemory() { detach(); }
	ScriptMemory(const ScriptMemory&) = delete;
	ScriptMemory& operator=(const ScriptMemory&) = delete;

	// Takes the machine over (its memory hooks); `maxHeap` bytes at most for the scripts' heap.
	bool attach(ScriptVm& vm, u32 maxHeap);
	void detach();
	bool attached() const { return _vm != nullptr; }
	ScriptVm& vm() { return *_vm; }

	// ---- the heap ----
	u32 alloc(u32 size);   // zeroed; 0 if there is no room
	void free(u32 address);
	u32 realloc(u32 address, u32 oldSize, u32 newSize);
	// The first address of the block that holds `address` if that is where a block's bytes begin; else 0.
	bool isBlock(u32 address) const;

	// ---- reading and writing ----
	// The engine's bytes behind [address, address + bytes), or null where that is not one piece of mapped memory.
	u8* resolve(u32 address, u32 bytes, bool write);
	i32 readInt(u32 address);
	void writeInt(u32 address, i32 v);
	u8 readByte(u32 address);
	void writeByte(u32 address, u8 v);
	void copy(u32 from, u32 to, u32 bytes);
	void fill(u32 address, u8 v, u32 bytes);
	// An engine string object (zString) at `address`: its text (into `out`, at most `size` - 1 bytes); a new text
	// for it (its characters in the heap).
	u32 readZString(u32 address, char* out, u32 size);
	void writeZString(u32 address, const char* text);
	// A string of characters at `address`, up to its 0.
	u32 readCString(u32 address, char* out, u32 size);

	// ---- the scripts' things ----
	// The address of a variable as the scripts pushed it (_@, _@f, _@s); 0 for a value that is no variable.
	u32 addressOf(const ScriptValue& v);
	// The address of an object's bytes (MEM_InstToPtr).
	u32 objectAddress(ScriptInstance* inst);
	// The object at `address` (_^): the machine's own if that is where one begins, else a view of the memory there.
	ScriptInstance* objectAt(u32 address);
	// The address of a string variable's engine string object (_@s).
	u32 stringAddress(u32 symbol, u32 index, ScriptInstance* ctx);
	// The address of symbol `i`'s zCPar_Symbol (MEM_GetSymbolByIndex); 0 if there is none.
	u32 symbolAddress(u32 i) const;
	// A block of the heap seen as an object (for the engine's stand-ins: MEM_Game, MEM_Timer ...).
	ScriptInstance* newObject(u32 size, u32* address = nullptr);

	const Stats& stats() const { return _stats; }

	// The heap and what refers into it, for a saved game. Another script image refuses it.
	void save(ByteSink& out) const;
	bool load(ByteSource& in);

private:
	struct View {
		u32 symbol, index;
		ScriptInstance* ctx;
		u8 bytes[20]; // the zString: 0, 0, ptr, len, res
		u32 external; // the scripts set its characters elsewhere (a heap address); 0: its own
		u32 lastPtr, lastLen; // what bytes held when the engine last set or took them
		bool used;
		bool seen; // a script holds it as an object (var zString z; z = _^(_@s(s))): written without a word
	};
	struct Wrap {
		u32 address; // 0: empty, 1: deleted
		ScriptInstance* inst;
	};

	static void hookDestroyed(ScriptInstance* inst, void* user);
	static const char* hookReadZString(const u8* at, void* user);
	static void hookWriteZString(u8* at, const char* text, void* user);
	static void hookSync(void* user);

	View* view(u32 id) const { return id < _viewCount ? &_viewChunks[id >> 8][id & 255] : nullptr; }
	void refreshView(u32 id);
	void viewWritten(u32 id);
	void syncViews();
	bool growHeap(u32 need);
	u32 blockBytes(u32 cls) const { return 8u << cls; }
	void wrapInsert(u32 address, ScriptInstance* inst);
	ScriptInstance* wrapFind(u32 address) const;
	void wrapRemove(u32 address);
	void forgetBlock(u32 offset, u32 bytes);
	u32 objectId(ScriptInstance* inst);
	u8* zstringBytes(const u8* z, u32& len);
	u8* symbolBytes(u32 i);

	ScriptVm* _vm = nullptr;
	ScriptMemoryHooks _hooks;
	Stats _stats;

	u8* _heap = nullptr;
	u32 _heapCap = 0, _heapTop = 0, _heapMax = 0;
	u32 _free[22] = {}; // per size: the first free block (its offset), 0: none

	View* _viewChunks[MAX_VIEWS / 256] = {};
	u32 _viewCount = 0;
	u32 _exposed = 0; // views written to elsewhere (external)
	bool _syncing = false;

	Wrap* _wraps = nullptr;
	u32 _wrapCap = 0, _wrapUsed = 0, _wrapDead = 0;

	ScriptInstance** _objects = nullptr; // by id (1 ..)
	u32 _objectCap = 0, _objectNext = 1;
	u32* _objectFree = nullptr;
	u32 _objectFreeCount = 0, _objectFreeCap = 0;

	char* _text = nullptr; // a scratch string
	u32 _textCap = 0;
	char* _ztext[4] = {};  // texts handed out by hookReadZString (a small ring)
	u8** _symBytes = nullptr; // per symbol, made when first asked for: its zCPar_Symbol
	u32 _symBytesCount = 0;
	u8 _symTable[4] = {};     // a word of the table of symbols, made when read
	u32 _ztextNext = 0;
};

} // namespace g3
