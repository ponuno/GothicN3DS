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

bool Pak::open(const char* path) {
	close();
	_file = fopen(path, "rb");
	if (!_file) return false;

	bool ok = fread(&_hdr, sizeof(_hdr), 1, _file) == 1 && _hdr.magic == PAK_MAGIC && _hdr.version == PAK_VERSION;
	if (ok) {
		_table = static_cast<PakEntry*>(malloc(size_t(_hdr.entryCount) * sizeof(PakEntry) + 1));
		_names = static_cast<char*>(malloc(size_t(_hdr.namesSize) + 1));
		ok = _table && _names;
	}
	if (ok) {
		ok = fseek(_file, long(_hdr.tableOffset), SEEK_SET) == 0 &&
		     fread(_table, sizeof(PakEntry), _hdr.entryCount, _file) == _hdr.entryCount;
	}
	if (ok && _hdr.namesSize) {
		ok = fseek(_file, long(_hdr.namesOffset), SEEK_SET) == 0 &&
		     fread(_names, 1, _hdr.namesSize, _file) == _hdr.namesSize;
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
	if (!_file || fseek(_file, long(e.offset), SEEK_SET) != 0) return false;

	if (e.codec == PAK_RAW) {
		if (e.packedSize != e.size) return false;
		_bytesRead += e.size;
		return fread(dst, 1, e.size, _file) == e.size;
	}
	if (e.codec != PAK_LZ4) return false;

	if (_scratchCap < e.packedSize) {
		u32 cap = max<u32>(e.packedSize, 64 * 1024);
		u8* p = static_cast<u8*>(realloc(_scratch, cap));
		if (!p) return false;
		_scratch = p;
		_scratchCap = cap;
	}
	if (fread(_scratch, 1, e.packedSize, _file) != e.packedSize) return false;
	_bytesRead += e.packedSize;
	int n = LZ4_decompress_safe(reinterpret_cast<const char*>(_scratch), static_cast<char*>(dst), int(e.packedSize),
	                            int(e.size));
	return n >= 0 && u32(n) == e.size;
}

void* Pak::readAlloc(const PakEntry& e) {
	void* p = malloc(e.size ? e.size : 1);
	if (p && !read(e, p)) {
		free(p);
		p = nullptr;
	}
	return p;
}

} // namespace g3
