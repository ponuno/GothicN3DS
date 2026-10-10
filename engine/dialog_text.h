// The words people say (baked from the game's OU.BIN, core/include/g3ds/dialog.h):
// found by the name the scripts give a line, read from the archive a block
// at a time - the whole of it is too large to keep in memory.
#pragma once
#include "g3ds/dialog.h"
#include "g3ds/pak.h"

namespace g3 {

class DialogTexts {
public:
	DialogTexts() = default;
	~DialogTexts() { release(); }
	DialogTexts(const DialogTexts&) = delete;
	DialogTexts& operator=(const DialogTexts&) = delete;

	// Reads the index. False if the archive has none (baked before talks, or
	// from a game without OU.BIN): every line is then "not found".
	bool load(Pak& pak);
	void release();
	bool loaded() const { return _firsts != nullptr; }
	u32 lineCount() const { return _lines; }
	u32 blockCount() const { return _blocks; }

	// The text of the line named `name` / of that hash, in the game's code
	// page, or null if there is none. Valid until the next call.
	const char* find(const char* name) { return find(hashName(name)); }
	const char* find(u32 hash);
	// Blocks read from the archive since load (for reports).
	u32 reads() const { return _reads; }

private:
	enum { CACHED = 2 };
	struct Cached {
		u32 block = 0xFFFFFFFFu;
		u32 used = 0;
		u8* data = nullptr;
	};
	const Cached* fetch(u32 block);

	Pak* _pak = nullptr;
	u32* _firsts = nullptr;
	u32 _blocks = 0, _lines = 0;
	Cached _cache[CACHED];
	u32 _tick = 0, _reads = 0;
};

} // namespace g3
