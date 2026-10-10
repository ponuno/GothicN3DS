#include "dialog_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace g3 {

bool DialogTexts::load(Pak& pak) {
	release();
	const PakEntry* e = pak.find("dialog/index");
	if (!e || e->size < sizeof(DialogIndexHeader)) return false;
	u8* blob = static_cast<u8*>(pak.readAlloc(*e));
	if (!blob) return false;
	DialogIndexHeader h;
	memcpy(&h, blob, sizeof(h));
	const bool ok = h.magic == DIALOG_MAGIC && h.version == DIALOG_VERSION && h.blockCount > 0 && h.blockCount < (1u << 16) &&
	                e->size == sizeof(h) + size_t(h.blockCount) * sizeof(u32);
	if (ok) {
		_firsts = static_cast<u32*>(malloc(size_t(h.blockCount) * sizeof(u32)));
		if (_firsts) {
			memcpy(_firsts, blob + sizeof(h), size_t(h.blockCount) * sizeof(u32));
			_blocks = h.blockCount;
			_lines = h.lineCount;
			_pak = &pak;
			// (Bisection needs them in order.)
			for (u32 i = 1; i < _blocks; ++i)
				if (_firsts[i - 1] > _firsts[i]) {
					free(_firsts);
					_firsts = nullptr;
					_blocks = _lines = 0;
					_pak = nullptr;
					break;
				}
		}
	}
	free(blob);
	return _firsts != nullptr;
}

void DialogTexts::release() {
	for (Cached& c : _cache) {
		free(c.data);
		c = Cached();
	}
	free(_firsts);
	_firsts = nullptr;
	_blocks = _lines = 0;
	_pak = nullptr;
	_tick = _reads = 0;
}

const DialogTexts::Cached* DialogTexts::fetch(u32 block) {
	Cached* free_ = &_cache[0];
	for (Cached& c : _cache) {
		if (c.block == block && c.data) {
			c.used = ++_tick;
			return &c;
		}
		if (!c.data || c.used < free_->used) free_ = &c;
	}
	char name[24];
	snprintf(name, sizeof(name), "dialog/%lu", static_cast<unsigned long>(block));
	const PakEntry* e = _pak ? _pak->find(name) : nullptr;
	if (!e || e->size < sizeof(DialogBlockHeader)) return nullptr;
	u8* data = static_cast<u8*>(_pak->readAlloc(*e));
	if (!data) return nullptr;
	DialogBlockHeader h;
	memcpy(&h, data, sizeof(h));
	// The block must be what its header says, and its last text end in a zero.
	if (h.count == 0 || e->size != sizeof(h) + size_t(h.count) * sizeof(DialogLine) + h.textBytes || h.textBytes == 0 ||
	    data[e->size - 1] != 0) {
		free(data);
		return nullptr;
	}
	free(free_->data);
	free_->data = data;
	free_->block = block;
	free_->used = ++_tick;
	++_reads;
	return free_;
}

const char* DialogTexts::find(u32 hash) {
	if (!_firsts) return nullptr;
	// The last block that begins at or before the hash.
	u32 lo = 0, hi = _blocks;
	while (hi - lo > 1) {
		const u32 mid = (lo + hi) / 2;
		if (_firsts[mid] <= hash)
			lo = mid;
		else
			hi = mid;
	}
	if (_firsts[lo] > hash) return nullptr;
	const Cached* c = fetch(lo);
	if (!c) return nullptr;
	DialogBlockHeader h;
	memcpy(&h, c->data, sizeof(h));
	const u8* lines = c->data + sizeof(h);
	const char* texts = reinterpret_cast<const char*>(lines + size_t(h.count) * sizeof(DialogLine));
	u32 a = 0, b = h.count;
	while (a < b) {
		const u32 mid = (a + b) / 2;
		DialogLine line;
		memcpy(&line, lines + size_t(mid) * sizeof(DialogLine), sizeof(line));
		if (line.hash == hash) return line.offset < h.textBytes ? texts + line.offset : nullptr;
		if (line.hash < hash)
			a = mid + 1;
		else
			b = mid;
	}
	return nullptr;
}

} // namespace g3
