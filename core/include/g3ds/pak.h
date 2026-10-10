// G3PK: the single asset archive the runtime reads from the SD card.
// Layout: PakHeader | blobs... | PakEntry[entryCount] (sorted by hash) | names.
// Everything is little-endian; each blob is stored raw or as one LZ4 block.
#pragma once
#include "g3ds/base.h"
#include <stdio.h>

namespace g3 {

constexpr u32 PAK_MAGIC = fourcc('G', '3', 'P', 'K');
constexpr u32 PAK_VERSION = 1;

enum PakCodec : u16 { PAK_RAW = 0, PAK_LZ4 = 1 };

enum AssetType : u16 {
	ASSET_BLOB = 0,
	ASSET_TEXTURE = 1, // TexHeader + PICA-native mip chain
	ASSET_WORLD = 2,   // WorldHeader + material table + chunk table
	ASSET_CHUNK = 3,   // ChunkHeader + batches + vertices + indices
	ASSET_META = 4,    // UTF-8 text (bake report)
	ASSET_MODEL = 5,   // ModelHeader + skeleton + skinned mesh (model.h)
	ASSET_CLIP = 6,    // ClipHeader + sampled node transforms (model.h)
	ASSET_SCRIPT = 7,  // ScriptHeader + the game's compiled scripts (script.h)
	ASSET_SOUND = 8,   // SoundHeader + 16-bit mono samples (sound.h)
};

struct PakHeader {
	u32 magic;
	u32 version;
	u32 entryCount;
	u32 tableOffset;
	u32 namesOffset;
	u32 namesSize;
	u32 reserved[2];
};
static_assert(sizeof(PakHeader) == 32, "PakHeader layout");

struct PakEntry {
	u32 hash;       // hashName(name)
	u32 offset;     // from file start
	u32 packedSize; // bytes on disk
	u32 size;       // bytes after decompression
	u16 type;       // AssetType
	u16 codec;      // PakCodec
	u32 nameOffset; // into the name table (debugging / tools only)
};
static_assert(sizeof(PakEntry) == 24, "PakEntry layout");

class Pak {
public:
	Pak() = default;
	~Pak() { close(); }
	Pak(const Pak&) = delete;
	Pak& operator=(const Pak&) = delete;

	bool open(const char* path);
	void close();
	bool isOpen() const { return _file != nullptr; }
	// The file this archive was opened from (to open it a second time).
	const char* path() const { return _path; }

	u32 count() const { return _hdr.entryCount; }
	const PakEntry& entry(u32 i) const { return _table[i]; }
	const char* name(const PakEntry& e) const;

	const PakEntry* find(u32 hash) const;
	const PakEntry* find(const char* name) const { return find(hashName(name)); }

	// dst must hold e.size bytes.
	bool read(const PakEntry& e, void* dst);
	// `size` bytes from `offset` into an entry stored raw (music: read in pieces).
	bool readRange(const PakEntry& e, u32 offset, void* dst, u32 size);
	// malloc'ed buffer of e.size bytes, or nullptr. Caller frees with free().
	void* readAlloc(const PakEntry& e);

	// Bytes read from storage so far (I/O statistics).
	u64 bytesRead() const { return _bytesRead; }

private:
	FILE* _file = nullptr;
	PakHeader _hdr {};
	PakEntry* _table = nullptr;
	char* _names = nullptr;
	u8* _scratch = nullptr;
	u32 _scratchCap = 0;
	u64 _bytesRead = 0;
	char _path[256] = {0};
};

} // namespace g3
