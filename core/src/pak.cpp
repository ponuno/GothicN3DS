#include "g3ds/pak.h"

#include "lz4.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

namespace g3 {

void logf(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fputc('\n', stdout);
}

namespace {

// stdio's seek takes a `long`: 32 bits on the console, so nothing past 2 GiB can be reached there.
bool seekTo(FILE* f, u64 offset) {
	if (sizeof(long) < 8 && offset > 0x7FFFFFFFull) return false;
	return fseek(f, long(offset), SEEK_SET) == 0;
}

// fix203: the archive is read unbuffered (see Pak::open), so each fread is one request to the card. A big entry is
// read in pieces of this size: the system's FS serves one request at a time for all threads, and a piece this big
// keeps the per-request overhead small while a read on the frame's thread never waits behind more than one piece of
// a worker's multi-megabyte read.
constexpr u32 kReadPiece = 256 * 1024;

bool readFully(FILE* f, void* dst, u32 size) {
	u8* p = static_cast<u8*>(dst);
	while (size) {
		const u32 n = size < kReadPiece ? size : kReadPiece;
		if (fread(p, 1, n, f) != n) return false;
		p += n;
		size -= n;
	}
	return true;
}

} // namespace

bool Pak::open(const char* path) {
	close();
	_file = fopen(path, "rb");
	if (!_file) return false;
	// fix203: no stdio buffer. devkitARM's newlib gives a FILE a BUFSIZ = 1024-byte buffer (libctru's fstat reports no
	// st_blksize), and a buffered fread refills that buffer over and over: every kilobyte of every entry was a request
	// of its own to the FS service (IPC to fs, then Process9). Unbuffered, newlib reads straight into the caller's
	// memory with one request per fread. Every read here is a seek plus one large block, so the buffer never helped.
	setvbuf(_file, nullptr, _IONBF, 0);

	bool ok = fread(&_hdr, sizeof(_hdr), 1, _file) == 1 && _hdr.magic == PAK_MAGIC && _hdr.version == PAK_VERSION;
	if (ok) {
		_table = static_cast<PakEntry*>(malloc(size_t(_hdr.entryCount) * sizeof(PakEntry) + 1));
		_names = static_cast<char*>(malloc(size_t(_hdr.namesSize) + 1));
		ok = _table && _names;
	}
	if (ok) {
		ok = seekTo(_file, _hdr.tableOffset) &&
		     readFully(_file, _table, _hdr.entryCount * u32(sizeof(PakEntry)));
	}
	if (ok && _hdr.namesSize) {
		ok = seekTo(_file, _hdr.namesOffset) &&
		     readFully(_file, _names, _hdr.namesSize);
	}
	if (ok) {
		_names[_hdr.namesSize] = 0;
		// The lookup is a binary search, so reject an unsorted table up front.
		for (u32 i = 1; i < _hdr.entryCount && ok; ++i) ok = _table[i - 1].hash < _table[i].hash;
	}
	if (!ok) {
		close();
		return false;
	}
	snprintf(_path, sizeof(_path), "%s", path);
	return true;
}

void Pak::close() {
	if (_file) fclose(_file);
	free(_table);
	free(_names);
	free(_scratch);
	_file = nullptr;
	_table = nullptr;
	_names = nullptr;
	_scratch = nullptr;
	_scratchCap = 0;
	_hooks = PakHooks();
	_path[0] = 0;
	_hdr = {};
}

const char* Pak::name(const PakEntry& e) const {
	return (_names && e.nameOffset < _hdr.namesSize) ? _names + e.nameOffset : "";
}

const PakEntry* Pak::find(u32 hash) const {
	u32 lo = 0, hi = _hdr.entryCount;
	while (lo < hi) {
		u32 mid = lo + (hi - lo) / 2;
		if (_table[mid].hash < hash)
			lo = mid + 1;
		else
			hi = mid;
	}
	return (lo < _hdr.entryCount && _table[lo].hash == hash) ? &_table[lo] : nullptr;
}

bool Pak::read(const PakEntry& e, void* dst) {
	if (_hooks.take && e.size) {
		if (void* ready = _hooks.take(_hooks.user, e)) {
			memcpy(dst, ready, e.size);
			free(ready);
			return true;
		}
	}
	return readCard(e, dst);
}

bool Pak::readCard(const PakEntry& e, void* dst) {
	if (!_file || !seekTo(_file, e.offset)) return false;

	if (e.codec == PAK_RAW) {
		if (e.packedSize != e.size) return false;
		_bytesRead += e.size;
		return readFully(_file, dst, e.size);
	}
	if (e.codec != PAK_LZ4) return false;

	if (_scratchCap < e.packedSize) {
		u32 cap = max<u32>(e.packedSize, 64 * 1024);
		u8* p = static_cast<u8*>(realloc(_scratch, cap));
		if (!p) return false;
		_scratch = p;
		_scratchCap = cap;
	}
	if (!readFully(_file, _scratch, e.packedSize)) return false;
	_bytesRead += e.packedSize;
	int n = LZ4_decompress_safe(reinterpret_cast<const char*>(_scratch), static_cast<char*>(dst), int(e.packedSize),
	                            int(e.size));
	return n >= 0 && u32(n) == e.size;
}

bool Pak::readRange(const PakEntry& e, u32 offset, void* dst, u32 size) {
	if (!_file || e.codec != PAK_RAW || e.packedSize != e.size || u64(offset) + size > e.size) return false;
	if (!seekTo(_file, u64(e.offset) + offset)) return false;
	_bytesRead += size;
	return readFully(_file, dst, size);
}

void* Pak::readAlloc(const PakEntry& e) {
	if (_hooks.take && e.size) {
		if (void* ready = _hooks.take(_hooks.user, e)) return ready;
	}
	void* p = malloc(e.size ? e.size : 1);
	if (p && !readCard(e, p)) {
		free(p);
		p = nullptr;
	}
	return p;
}

} // namespace g3
