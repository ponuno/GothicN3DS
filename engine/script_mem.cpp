#include "script_mem.h"

#include "bytes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace g3 {

namespace {

constexpr u32 TAG_USED = 0x55534544u; // "DESU"
constexpr u32 TAG_FREE = 0x45455246u; // "FREE"
constexpr u32 HAS_WRAP = 1u << 8;     // in a block's first word: some view begins in it
constexpr u32 MAX_CLASS = 21;         // 16 MB
constexpr u32 FIRST_BLOCK = 8;        // offset 0 is no block (0 = "none" in the free lists)

u32 rd32(const u8* p) {
	u32 v;
	memcpy(&v, p, 4);
	return v;
}
void wr32(u8* p, u32 v) { memcpy(p, &v, 4); }

u32 classFor(u32 size) {
	u32 c = 0;
	while (c < MAX_CLASS && (8u << c) < size) ++c;
	return c;
}

} // namespace

// ---- set-up ----

bool ScriptMemory::attach(ScriptVm& vm, u32 maxHeap) {
	detach();
	_vm = &vm;
	_heapMax = maxHeap < 64 * 1024 ? 64 * 1024 : (maxHeap > (HEAP_END - HEAP_BASE) ? (HEAP_END - HEAP_BASE) : maxHeap);
	_heapCap = 64 * 1024;
	_heap = static_cast<u8*>(calloc(_heapCap, 1));
	if (!_heap) {
		_vm = nullptr;
		return false;
	}
	_heapTop = FIRST_BLOCK;
	_stats = Stats();
	_stats.heapBytes = _heapCap;
	_hooks.user = this;
	_hooks.destroyed = hookDestroyed;
	_hooks.readZString = hookReadZString;
	_hooks.writeZString = hookWriteZString;
	_hooks.syncStrings = hookSync;
	vm.setMemoryHooks(&_hooks);
	return true;
}

void ScriptMemory::detach() {
	if (_vm) {
		// Views of this memory must not outlive it.
		for (u32 i = 0; i < _wrapCap; ++i)
			if (_wraps[i].address > 1 && _wraps[i].inst) {
				ScriptInstance* w = _wraps[i].inst;
				w->flags |= SCRIPTI_DEAD;
				w->ext = nullptr;
				w->size = 0;
			}
		_vm->setMemoryHooks(nullptr);
		_vm->watchStrings(false);
		_vm->purge();
		for (u32 i = 1; i < _objectNext && _objects; ++i)
			if (_objects[i]) _objects[i]->memId = 0;
	}
	::free(_heap);
	for (View*& c : _viewChunks) {
		::free(c);
		c = nullptr;
	}
	::free(_wraps);
	if (_symBytes)
		for (u32 i = 0; i < _symBytesCount; ++i) ::free(_symBytes[i]);
	::free(_symBytes);
	_symBytes = nullptr;
	_symBytesCount = 0;
	::free(_objects);
	::free(_objectFree);
	::free(_text);
	for (char*& t : _ztext) {
		::free(t);
		t = nullptr;
	}
	_heap = nullptr;
	_wraps = nullptr;
	_objects = nullptr;
	_objectFree = nullptr;
	_text = nullptr;
	_heapCap = _heapTop = _heapMax = 0;
	memset(_free, 0, sizeof(_free));
	_viewCount = _exposed = 0;
	_wrapCap = _wrapUsed = _wrapDead = 0;
	_objectCap = 0;
	_objectNext = 1;
	_objectFreeCount = _objectFreeCap = 0;
	_textCap = 0;
	_vm = nullptr;
}

// ---- the heap ----

bool ScriptMemory::growHeap(u32 need) {
	if (need > _heapMax || need < _heapTop) return false;
	u32 cap = _heapCap;
	while (cap < need) cap = cap > _heapMax / 2 ? _heapMax : cap * 2;
	if (cap < need) return false;
	u8* grown = static_cast<u8*>(::realloc(_heap, cap));
	if (!grown) return false;
	memset(grown + _heapCap, 0, cap - _heapCap);
	_heap = grown;
	_heapCap = cap;
	_stats.heapBytes = cap;
	// The views of the heap follow it.
	for (u32 i = 0; i < _wrapCap; ++i) {
		Wrap& w = _wraps[i];
		if (w.address > 1 && w.address >= HEAP_BASE && w.address < HEAP_END && w.inst) w.inst->ext = _heap + (w.address - HEAP_BASE);
	}
	return true;
}

u32 ScriptMemory::alloc(u32 size) {
	if (!_heap || size > (8u << MAX_CLASS)) {
		++_stats.refused;
		return 0;
	}
	const u32 cls = classFor(size ? size : 1);
	const u32 bytes = blockBytes(cls);
	u32 off = _free[cls];
	if (off) {
		_free[cls] = rd32(_heap + off + 8);
	} else {
		if (u64(_heapTop) + 8 + bytes > _heapCap && !growHeap(_heapTop + 8 + bytes)) {
			++_stats.refused;
			return 0;
		}
		off = _heapTop;
		_heapTop += 8 + bytes;
	}
	wr32(_heap + off, cls);
	wr32(_heap + off + 4, TAG_USED);
	memset(_heap + off + 8, 0, bytes);
	++_stats.allocs;
	++_stats.blocks;
	_stats.heapUsed += 8 + bytes;
	if (_stats.heapUsed > _stats.heapPeak) _stats.heapPeak = _stats.heapUsed;
	return HEAP_BASE + off + 8;
}

bool ScriptMemory::isBlock(u32 address) const {
	if (!_heap || address < HEAP_BASE + FIRST_BLOCK + 8 || address >= HEAP_BASE + _heapTop) return false;
	const u32 off = address - HEAP_BASE - 8;
	return (off & 7) == 0 && rd32(_heap + off + 4) == TAG_USED && (rd32(_heap + off) & 0xFF) <= MAX_CLASS;
}

void ScriptMemory::forgetBlock(u32 address, u32 bytes) {
	// Views of the freed bytes see nothing any more (a script that keeps one reads zeros, as the original's would
	// read whatever comes there next); purge() takes them away from time to time.
	for (u32 i = 0; i < _wrapCap; ++i) {
		Wrap& w = _wraps[i];
		if (w.address <= 1 || w.address < address || w.address >= address + bytes) continue;
		if (w.inst) {
			w.inst->flags |= SCRIPTI_DEAD;
			w.inst->ext = nullptr;
			w.inst->size = 0;
			w.inst->memId = 0;
			if (_stats.views) --_stats.views;
			++_wrapDead;
		}
		w.address = 1;
		w.inst = nullptr;
		--_wrapUsed;
	}
	if (_wrapDead >= 128) {
		_vm->purge();
		_wrapDead = 0;
	}
}

void ScriptMemory::free(u32 address) {
	if (!address) return;
	if (!isBlock(address)) {
		++_stats.badFrees;
		return;
	}
	const u32 off = address - HEAP_BASE - 8;
	const u32 head = rd32(_heap + off);
	const u32 cls = head & 0xFF;
	const u32 bytes = blockBytes(cls);
	if (head & HAS_WRAP) forgetBlock(address, bytes);
	wr32(_heap + off, cls);
	wr32(_heap + off + 4, TAG_FREE);
	wr32(_heap + off + 8, _free[cls]);
	_free[cls] = off;
	++_stats.frees;
	if (_stats.blocks) --_stats.blocks;
	_stats.heapUsed -= 8 + bytes;
}

u32 ScriptMemory::realloc(u32 address, u32 oldSize, u32 newSize) {
	if (!address) return alloc(newSize);
	if (!isBlock(address)) {
		++_stats.badFrees;
		return alloc(newSize);
	}
	const u32 off = address - HEAP_BASE - 8;
	const u32 cap = blockBytes(rd32(_heap + off) & 0xFF);
	if (newSize <= cap) {
		if (newSize > oldSize && oldSize < cap) memset(_heap + off + 8 + oldSize, 0, (newSize < cap ? newSize : cap) - oldSize);
		return address;
	}
	const u32 moved = alloc(newSize);
	if (!moved) return 0;
	const u32 keep = oldSize < cap ? oldSize : cap;
	// (alloc may have moved the heap: the offsets hold.)
	memcpy(_heap + (moved - HEAP_BASE), _heap + off + 8, keep);
	free(address);
	return moved;
}

// ---- reading and writing ----

u8* ScriptMemory::resolve(u32 a, u32 bytes, bool write) {
	if (!_vm || bytes == 0) return nullptr;
	const u64 end = u64(a) + bytes;
	if (a >= HEAP_BASE && a < HEAP_END) {
		const u32 off = a - HEAP_BASE;
		if (off < FIRST_BLOCK || end - HEAP_BASE > _heapTop) return nullptr;
		return _heap + off;
	}
	if (a >= OBJ_BASE) {
		const u32 id = (a - OBJ_BASE) >> OBJ_SHIFT, k = (a - OBJ_BASE) & ((1u << OBJ_SHIFT) - 1);
		ScriptInstance* inst = id < _objectNext && _objects ? _objects[id] : nullptr;
		if (!inst || u64(k) + bytes > inst->size) return nullptr;
		return inst->data() + k;
	}
	if (a >= DATA_BASE && a < HEAP_BASE) {
		if (end - DATA_BASE > u64(_vm->dataCount()) * 4) return nullptr;
		return reinterpret_cast<u8*>(_vm->dataWords()) + (a - DATA_BASE);
	}
	if (a >= STRV_BASE && a < STRC_BASE) {
		const u32 id = (a - STRV_BASE) / 32, k = (a - STRV_BASE) % 32;
		View* v = view(id);
		if (!v || !v->used || k + bytes > 20) return nullptr;
		if (!write && !v->external) refreshView(id);
		return v->bytes + k;
	}
	if (a >= STRC_BASE && a < STRC_END) {
		if (write) return nullptr; // (writeByte / writeInt take care of a string's characters)
		const u32 id = (a - STRC_BASE) >> STRC_SHIFT, k = (a - STRC_BASE) & ((1u << STRC_SHIFT) - 1);
		View* v = view(id);
		if (!v || !v->used) return nullptr;
		const char* text = _vm->getString(v->symbol, v->index, v->ctx);
		const size_t len = strlen(text);
		if (u64(k) + bytes > len + 1) return nullptr;
		return reinterpret_cast<u8*>(const_cast<char*>(text)) + k;
	}
	if (a >= CODE_BASE && a < OBJ_BASE) {
		if (write || end - CODE_BASE > _vm->codeSize()) return nullptr;
		return const_cast<u8*>(_vm->code()) + (a - CODE_BASE);
	}
	if (a >= SYMT_BASE && a < SYMS_BASE) {
		const u32 i = (a - SYMT_BASE) / 4, k = (a - SYMT_BASE) % 4;
		if (write || i >= _vm->symbolCount() || k + bytes > 4) return nullptr;
		wr32(_symTable, symbolAddress(i));
		return _symTable + k;
	}
	if (a >= SYMS_BASE && a < CODE_BASE) {
		const u32 i = (a - SYMS_BASE) / SYMS_SIZE, k = (a - SYMS_BASE) % SYMS_SIZE;
		if (i >= _vm->symbolCount() || k + bytes > SYMS_SIZE) return nullptr;
		u8* p = symbolBytes(i);
		return p ? p + k : nullptr;
	}
	return nullptr;
}

i32 ScriptMemory::readInt(u32 a) {
	const u8* p = resolve(a, 4, false);
	if (!p) {
		++_stats.badReads;
		return 0;
	}
	return i32(rd32(p));
}

u8 ScriptMemory::readByte(u32 a) {
	const u8* p = resolve(a, 1, false);
	if (!p) {
		++_stats.badReads;
		return 0;
	}
	return *p;
}

void ScriptMemory::writeInt(u32 a, i32 v) {
	if (a >= STRC_BASE && a < STRC_END) {
		for (u32 k = 0; k < 4; ++k) writeByte(a + k, u8(u32(v) >> (8 * k)));
		return;
	}
	if (a >= SYMS_BASE && a < CODE_BASE && (a - SYMS_BASE) % SYMS_SIZE == 28) {
		// A symbol's `offset`: for an instance, the object it refers to (Ikarus' MEM_AssignInst done by hand).
		const u32 i = (a - SYMS_BASE) / SYMS_SIZE;
		if (i < _vm->symbolCount() && _vm->symbol(i).type == SCRIPT_INSTANCE && !(_vm->symbol(i).flags & SCRIPTF_MEMBER)) {
			_vm->setInstance(i, objectAt(u32(v)));
			if (u8* p = symbolBytes(i)) wr32(p + 28, u32(v));
			return;
		}
	}
	u8* p = resolve(a, 4, true);
	if (!p) {
		++_stats.badWrites;
		return;
	}
	wr32(p, u32(v));
	if (a >= STRV_BASE && a < STRC_BASE) viewWritten((a - STRV_BASE) / 32);
}

void ScriptMemory::writeByte(u32 a, u8 v) {
	if (a >= STRC_BASE && a < STRC_END) {
		// A character of a string variable: the variable gets a changed copy.
		const u32 id = (a - STRC_BASE) >> STRC_SHIFT, k = (a - STRC_BASE) & ((1u << STRC_SHIFT) - 1);
		View* vw = view(id);
		if (!vw || !vw->used) {
			++_stats.badWrites;
			return;
		}
		const char* text = _vm->getString(vw->symbol, vw->index, vw->ctx);
		const u32 len = u32(strlen(text));
		if (k >= len) {
			++_stats.badWrites;
			return;
		}
		if (_textCap < len + 1) {
			char* grown = static_cast<char*>(::realloc(_text, len + 1));
			if (!grown) return;
			_text = grown;
			_textCap = len + 1;
		}
		memcpy(_text, text, len + 1);
		_text[k] = char(v);
		_vm->setString(vw->symbol, vw->index, vw->ctx, _text);
		return;
	}
	u8* p = resolve(a, 1, true);
	if (!p) {
		++_stats.badWrites;
		return;
	}
	*p = v;
	if (a >= STRV_BASE && a < STRC_BASE) viewWritten((a - STRV_BASE) / 32);
}

void ScriptMemory::copy(u32 from, u32 to, u32 bytes) {
	if (!bytes) return;
	const u8* src = resolve(from, bytes, false);
	u8* dst = (to >= STRV_BASE && to < OBJ_BASE) ? nullptr : resolve(to, bytes, true);
	if (src && dst) {
		memmove(dst, src, bytes);
		return;
	}
	if (bytes > (1u << 24)) {
		++_stats.badWrites;
		return;
	}
	// Piece by piece (a string's characters, or ranges that leave the mapped memory).
	if (from < to && from + bytes > to) {
		for (u32 k = bytes; k > 0; --k) writeByte(to + k - 1, readByte(from + k - 1));
	} else {
		for (u32 k = 0; k < bytes; ++k) writeByte(to + k, readByte(from + k));
	}
}

void ScriptMemory::fill(u32 a, u8 v, u32 bytes) {
	if (!bytes) return;
	if (u8* p = (a >= STRV_BASE && a < OBJ_BASE) ? nullptr : resolve(a, bytes, true)) {
		memset(p, v, bytes);
		return;
	}
	if (bytes > (1u << 24)) {
		++_stats.badWrites;
		return;
	}
	for (u32 k = 0; k < bytes; ++k) writeByte(a + k, v);
}

u32 ScriptMemory::readCString(u32 a, char* out, u32 size) {
	u32 n = 0;
	while (n + 1 < size) {
		const u8* p = resolve(a + n, 1, false);
		if (!p || !*p) break;
		out[n++] = char(*p);
	}
	if (size) out[n] = 0;
	return n;
}

u32 ScriptMemory::readZString(u32 a, char* out, u32 size) {
	if (size) out[0] = 0;
	const u8* z = resolve(a, 20, false);
	if (!z) {
		++_stats.badReads;
		return 0;
	}
	const u32 ptr = rd32(z + 8), len = rd32(z + 12);
	if (!ptr || !len) return 0;
	const u32 n = len < size ? len : (size ? size - 1 : 0);
	if (const u8* p = resolve(ptr, n, false)) {
		memcpy(out, p, n);
	} else {
		for (u32 k = 0; k < n; ++k) out[k] = char(readByte(ptr + k));
	}
	if (size) out[n] = 0;
	return n;
}

void ScriptMemory::writeZString(u32 a, const char* text) {
	if (!text) text = "";
	if (a >= STRV_BASE && a < STRC_BASE) {
		// A string variable's own: the variable gets the text.
		View* v = view((a - STRV_BASE) / 32);
		if (v && v->used) {
			_vm->setString(v->symbol, v->index, v->ctx, text);
			refreshView((a - STRV_BASE) / 32);
		}
		return;
	}
	u8* z = resolve(a, 20, true);
	if (!z) {
		++_stats.badWrites;
		return;
	}
	const u32 old = rd32(z + 8);
	const u32 len = u32(strlen(text));
	const u32 block = alloc(len + 2);
	z = resolve(a, 20, true); // (the heap may have moved)
	if (!z) return;
	if (block) {
		u8* p = resolve(block + 1, len + 1, true);
		if (p) memcpy(p, text, len + 1);
	}
	wr32(z + 8, block ? block + 1 : 0);
	wr32(z + 12, block ? len : 0);
	wr32(z + 16, block ? len : 0);
	// (An engine string owns its characters, one byte into their block.)
	if (old > 1 && isBlock(old - 1)) free(old - 1);
}

// ---- the parser's symbols (zCPar_Symbol) ----

u32 ScriptMemory::symbolAddress(u32 i) const {
	if (!_vm || i >= _vm->symbolCount() || i >= (CODE_BASE - SYMS_BASE) / SYMS_SIZE) return 0;
	return SYMS_BASE + i * SYMS_SIZE;
}

// The original's zCPar_Symbol: name (zString, not known here: empty), next, content, offset, bitfield (count, type
// << 12, flags << 16), file, line, lines, position, length, parent. `content` is where the values are (a variable's
// words, a string's engine string), or the code's offset (a function, a prototype, an instance); `offset` the
// object (an instance), the size (a class) or the place in its object (a member).
u8* ScriptMemory::symbolBytes(u32 i) {
	if (!_vm || i >= _vm->symbolCount()) return nullptr;
	if (!_symBytes) {
		_symBytesCount = _vm->symbolCount();
		_symBytes = static_cast<u8**>(calloc(_symBytesCount, sizeof(u8*)));
		if (!_symBytes) return nullptr;
	}
	if (i >= _symBytesCount) return nullptr;
	u8*& p = _symBytes[i];
	if (!p) {
		p = static_cast<u8*>(calloc(SYMS_SIZE, 1));
		if (!p) return nullptr;
	}
	const ScriptSymbol& s = _vm->symbol(i);
	memset(p, 0, SYMS_SIZE);
	u32 content = 0, offset = 0;
	const bool member = s.flags & SCRIPTF_MEMBER;
	switch (s.type) {
	case SCRIPT_INT:
	case SCRIPT_FLOAT:
		if (member)
			offset = s.value;
		else
			content = DATA_BASE + s.value * 4;
		break;
	case SCRIPT_STRING:
		if (member)
			offset = s.value;
		else
			content = stringAddress(i, 0, nullptr);
		break;
	case SCRIPT_FUNC:
		if (member)
			offset = s.value;
		else if (s.flags & SCRIPTF_EXTERNAL)
			content = 0;
		else if (s.flags & SCRIPTF_CONST)
			content = s.value == SCRIPT_NONE ? 0 : s.value;
		else
			content = DATA_BASE + s.value * 4;
		break;
	case SCRIPT_CLASS: offset = s.value; break;
	case SCRIPT_PROTOTYPE: content = s.value == SCRIPT_NONE ? 0 : s.value; break;
	case SCRIPT_INSTANCE:
		content = s.value == SCRIPT_NONE ? 0 : s.value;
		if (!member) offset = objectAddress(_vm->instanceOf(i));
		break;
	default: break;
	}
	wr32(p + 24, content);
	wr32(p + 28, offset);
	wr32(p + 32, u32(s.count & 0xFFF) | (u32(s.type) << 12) | (u32(s.flags & 0x0F) << 16));
	wr32(p + 56, s.parent >= 0 ? symbolAddress(u32(s.parent)) : 0);
	return p;
}

// ---- string variables (_@s) ----

u32 ScriptMemory::stringAddress(u32 symbol, u32 index, ScriptInstance* ctx) {
	if (!_vm) return 0;
	u32 id = SCRIPT_NONE;
	for (u32 i = 0; i < _viewCount; ++i) {
		const View* v = view(i);
		if (v->used && v->symbol == symbol && v->index == index && v->ctx == ctx) {
			id = i;
			break;
		}
	}
	if (id == SCRIPT_NONE) {
		for (u32 i = 0; i < _viewCount && id == SCRIPT_NONE; ++i)
			if (!view(i)->used) id = i;
		if (id == SCRIPT_NONE) {
			if (_viewCount >= MAX_VIEWS) return 0;
			if (!_viewChunks[_viewCount >> 8]) {
				_viewChunks[_viewCount >> 8] = static_cast<View*>(calloc(256, sizeof(View)));
				if (!_viewChunks[_viewCount >> 8]) return 0;
			}
			id = _viewCount++;
		}
		View* v = view(id);
		memset(v, 0, sizeof(View));
		v->symbol = symbol;
		v->index = index;
		v->ctx = ctx;
		v->used = true;
		++_stats.strings;
	}
	View* v = view(id);
	if (v->external) {
		// Its characters were the scripts' (SB_ToString): the variable has a copy of them; their block was handed
		// to the string, which lets it go now, as the engine's string would.
		if (v->external > 1 && isBlock(v->external - 1)) free(v->external - 1);
		v->external = 0;
		if (_exposed) --_exposed;
	}
	refreshView(id);
	return STRV_BASE + id * 32;
}

void ScriptMemory::refreshView(u32 id) {
	View* v = view(id);
	if (!v || !v->used) return;
	const char* text = _vm->getString(v->symbol, v->index, v->ctx);
	const u32 len = u32(strlen(text));
	memset(v->bytes, 0, 8);
	wr32(v->bytes + 8, len ? STRC_BASE + (id << STRC_SHIFT) : 0);
	wr32(v->bytes + 12, len);
	wr32(v->bytes + 16, len);
	v->lastPtr = rd32(v->bytes + 8);
	v->lastLen = len;
}

// The scripts wrote into a string variable's engine string object: its characters elsewhere (`ptr`) or fewer of them
// (`len`). The variable takes the text so made.
void ScriptMemory::viewWritten(u32 id) {
	View* v = view(id);
	if (!v || !v->used) return;
	const u32 own = STRC_BASE + (id << STRC_SHIFT);
	const u32 ptr = rd32(v->bytes + 8), len = rd32(v->bytes + 12);
	if (len > (1u << STRC_SHIFT) * 64) return;
	if (_textCap < len + 1) {
		char* grown = static_cast<char*>(::realloc(_text, len + 1));
		if (!grown) return;
		_text = grown;
		_textCap = len + 1;
	}
	u32 n = 0;
	if (ptr && len) {
		for (; n < len; ++n) {
			const u8* p = resolve(ptr + n, 1, false);
			if (!p) break;
			_text[n] = char(*p);
		}
	}
	_text[n] = 0;
	if (ptr != own && ptr) {
		if (!v->external) ++_exposed;
		v->external = ptr;
	}
	_vm->setString(v->symbol, v->index, v->ctx, _text);
	if (!v->external) refreshView(id);
	v->lastPtr = rd32(v->bytes + 8);
	v->lastLen = rd32(v->bytes + 12);
	_vm->watchStrings(_exposed > 0);
}

void ScriptMemory::syncViews() {
	// Views a script holds as an object are written without a word to anyone: look whether they were.
	if (_syncing) return;
	_syncing = true;
	bool watch = _exposed > 0;
	for (u32 i = 0; i < _viewCount; ++i) {
		View* v = view(i);
		if (!v->used || !v->seen) continue;
		v->seen = false;
		if (rd32(v->bytes + 8) != v->lastPtr || rd32(v->bytes + 12) != v->lastLen) viewWritten(i);
	}
	_vm->watchStrings(watch || _exposed > 0);
	_syncing = false;
}

// ---- the scripts' things ----

u32 ScriptMemory::objectId(ScriptInstance* inst) {
	if (inst->memId) return inst->memId;
	u32 id = 0;
	if (_objectFreeCount) {
		id = _objectFree[--_objectFreeCount];
	} else {
		if (_objectNext > MAX_OBJECTS) return 0;
		if (_objectNext >= _objectCap) {
			const u32 cap = _objectCap ? _objectCap * 2 : 256;
			ScriptInstance** grown = static_cast<ScriptInstance**>(::realloc(_objects, size_t(cap) * sizeof(ScriptInstance*)));
			if (!grown) return 0;
			memset(grown + _objectCap, 0, size_t(cap - _objectCap) * sizeof(ScriptInstance*));
			_objects = grown;
			_objectCap = cap;
		}
		id = _objectNext++;
	}
	_objects[id] = inst;
	inst->memId = id;
	++_stats.objects;
	return id;
}

u32 ScriptMemory::objectAddress(ScriptInstance* inst) {
	if (!inst || !_vm) return 0;
	if (inst->flags & SCRIPTI_VIEW) return inst->memId; // (a view keeps the address it shows there)
	const u32 id = objectId(inst);
	return id ? OBJ_BASE + (id << OBJ_SHIFT) : 0;
}

u32 ScriptMemory::addressOf(const ScriptValue& v) {
	if (!_vm) return 0;
	if (v.kind == ScriptValue::OBJECT) return objectAddress(v.ctx);
	if (v.kind != ScriptValue::VARIABLE) return 0;
	if (v.symbol == _vm->symbolCount()) return stringAddress(v.symbol, v.index, nullptr); // (a temporary string)
	if (v.symbol >= _vm->symbolCount()) return 0;
	const ScriptSymbol& s = _vm->symbol(v.symbol);
	if (s.type == SCRIPT_STRING) return stringAddress(v.symbol, v.index, (s.flags & SCRIPTF_MEMBER) ? v.ctx : nullptr);
	if (s.flags & SCRIPTF_MEMBER) {
		if (!v.ctx) return 0;
		const u32 base = objectAddress(v.ctx);
		return base ? base + s.value + u32(v.index) * 4 : 0;
	}
	if (s.type == SCRIPT_INT || s.type == SCRIPT_FLOAT || (s.type == SCRIPT_FUNC && !(s.flags & SCRIPTF_CONST)))
		return DATA_BASE + (s.value + v.index) * 4;
	if (s.type == SCRIPT_INSTANCE) return objectAddress(_vm->instanceOf(v.symbol));
	return 0;
}

ScriptInstance* ScriptMemory::objectAt(u32 a) {
	if (!_vm || !a) return nullptr;
	if (a >= OBJ_BASE) {
		const u32 id = (a - OBJ_BASE) >> OBJ_SHIFT, k = (a - OBJ_BASE) & ((1u << OBJ_SHIFT) - 1);
		ScriptInstance* inst = id < _objectNext && _objects ? _objects[id] : nullptr;
		if (inst && k == 0) return inst;
		if (!inst || k >= inst->size) return nullptr;
	}
	if (ScriptInstance* w = wrapFind(a)) {
		if (a >= STRV_BASE && a < STRC_BASE) {
			View* v = view((a - STRV_BASE) / 32);
			if (v && !v->external) refreshView((a - STRV_BASE) / 32);
			if (v) v->seen = true;
			_vm->watchStrings(true);
		}
		return w;
	}
	u32 size = 0;
	u8* host = nullptr;
	if (a >= HEAP_BASE && a < HEAP_END) {
		host = resolve(a, 1, false);
		if (host) size = _heapTop - (a - HEAP_BASE);
		if (host && isBlock(a)) wr32(_heap + (a - HEAP_BASE) - 8, rd32(_heap + (a - HEAP_BASE) - 8) | HAS_WRAP);
		else if (host) {
			// A view that begins inside a block: the block that holds it is marked (found by walking the heap).
			for (u32 off = FIRST_BLOCK; off < _heapTop;) {
				const u32 head = rd32(_heap + off);
				const u32 bytes = blockBytes(head & 0xFF);
				if (a - HEAP_BASE >= off + 8 && a - HEAP_BASE < off + 8 + bytes) {
					wr32(_heap + off, head | HAS_WRAP);
					break;
				}
				off += 8 + bytes;
			}
		}
	} else if (a >= OBJ_BASE) {
		const u32 id = (a - OBJ_BASE) >> OBJ_SHIFT, k = (a - OBJ_BASE) & ((1u << OBJ_SHIFT) - 1);
		host = resolve(a, 1, false);
		if (host) size = _objects[id]->size - k;
	} else if (a >= DATA_BASE && a < HEAP_BASE) {
		host = resolve(a, 4, false);
		if (host) size = _vm->dataCount() * 4 - (a - DATA_BASE);
	} else if (a >= SYMS_BASE && a < CODE_BASE) {
		host = resolve(a, 1, false);
		if (host) size = SYMS_SIZE - (a - SYMS_BASE) % SYMS_SIZE;
	} else if (a >= STRV_BASE && a < STRC_BASE) {
		host = resolve(a, 1, false);
		if (host) {
			const u32 k = (a - STRV_BASE) % 32;
			size = 20 - k;
			view((a - STRV_BASE) / 32)->seen = true;
			_vm->watchStrings(true);
		}
	}
	if (!host || !size) {
		++_stats.badReads;
		return nullptr;
	}
	ScriptInstance* w = _vm->wrap(host, size);
	if (!w) return nullptr;
	w->memId = a;
	wrapInsert(a, w);
	++_stats.views;
	return w;
}

ScriptInstance* ScriptMemory::newObject(u32 size, u32* address) {
	const u32 a = alloc(size);
	if (address) *address = a;
	return a ? objectAt(a) : nullptr;
}

// ---- the table of views (open addressing; 0 empty, 1 deleted) ----

void ScriptMemory::wrapInsert(u32 address, ScriptInstance* inst) {
	if ((_wrapUsed + 1) * 2 > _wrapCap) {
		const u32 cap = _wrapCap ? _wrapCap * 2 : 256;
		Wrap* grown = static_cast<Wrap*>(calloc(cap, sizeof(Wrap)));
		if (!grown) return;
		Wrap* old = _wraps;
		const u32 oldCap = _wrapCap;
		_wraps = grown;
		_wrapCap = cap;
		_wrapUsed = 0;
		for (u32 i = 0; i < oldCap; ++i)
			if (old[i].address > 1) wrapInsert(old[i].address, old[i].inst);
		::free(old);
	}
	u32 h = (address * 2654435761u) & (_wrapCap - 1);
	while (_wraps[h].address > 1) h = (h + 1) & (_wrapCap - 1);
	_wraps[h] = {address, inst};
	++_wrapUsed;
}

ScriptInstance* ScriptMemory::wrapFind(u32 address) const {
	if (!_wrapCap) return nullptr;
	u32 h = (address * 2654435761u) & (_wrapCap - 1);
	for (u32 n = 0; n < _wrapCap && _wraps[h].address; ++n) {
		if (_wraps[h].address == address) return _wraps[h].inst;
		h = (h + 1) & (_wrapCap - 1);
	}
	return nullptr;
}

void ScriptMemory::wrapRemove(u32 address) {
	if (!_wrapCap) return;
	u32 h = (address * 2654435761u) & (_wrapCap - 1);
	for (u32 n = 0; n < _wrapCap && _wraps[h].address; ++n) {
		if (_wraps[h].address == address) {
			_wraps[h].address = 1;
			_wraps[h].inst = nullptr;
			--_wrapUsed;
			return;
		}
		h = (h + 1) & (_wrapCap - 1);
	}
}

// ---- the machine's hooks ----

void ScriptMemory::hookDestroyed(ScriptInstance* inst, void* user) {
	ScriptMemory& m = *static_cast<ScriptMemory*>(user);
	if (inst->flags & SCRIPTI_VIEW) {
		if (inst->memId && m.wrapFind(inst->memId) == inst) {
			m.wrapRemove(inst->memId);
			if (m._stats.views) --m._stats.views;
		}
		return;
	}
	if (inst->memId && inst->memId < m._objectNext && m._objects[inst->memId] == inst) {
		m._objects[inst->memId] = nullptr;
		if (m._objectFreeCount == m._objectFreeCap) {
			const u32 cap = m._objectFreeCap ? m._objectFreeCap * 2 : 64;
			u32* grown = static_cast<u32*>(::realloc(m._objectFree, cap * sizeof(u32)));
			if (grown) {
				m._objectFree = grown;
				m._objectFreeCap = cap;
			}
		}
		if (m._objectFreeCount < m._objectFreeCap) m._objectFree[m._objectFreeCount++] = inst->memId;
		if (m._stats.objects) --m._stats.objects;
		inst->memId = 0;
	}
	// String variables of its members lose their address.
	for (u32 i = 0; i < m._viewCount; ++i) {
		View* v = m.view(i);
		if (v->used && v->ctx == inst) {
			if (v->external && m._exposed) --m._exposed;
			v->used = false;
			if (m._stats.strings) --m._stats.strings;
		}
	}
}

const char* ScriptMemory::hookReadZString(const u8* at, void* user) {
	ScriptMemory& m = *static_cast<ScriptMemory*>(user);
	const u32 ptr = rd32(at + 8), len = rd32(at + 12);
	char*& slot = m._ztext[m._ztextNext];
	m._ztextNext = (m._ztextNext + 1) % 4;
	const u32 n = ptr && len < (1u << 24) ? len : 0;
	char* grown = static_cast<char*>(::realloc(slot, n + 1));
	if (!grown) return "";
	slot = grown;
	u32 k = 0;
	for (; k < n; ++k) {
		const u8* p = m.resolve(ptr + k, 1, false);
		if (!p) break;
		slot[k] = char(*p);
	}
	slot[k] = 0;
	return slot;
}

void ScriptMemory::hookWriteZString(u8* at, const char* text, void* user) {
	ScriptMemory& m = *static_cast<ScriptMemory*>(user);
	// `at` lies in the heap or another mapped piece: find its address (the views know theirs).
	if (at >= m._heap && at + 20 <= m._heap + m._heapTop) {
		m.writeZString(HEAP_BASE + u32(at - m._heap), text);
		return;
	}
	const u32 len = u32(strlen(text ? text : ""));
	const u32 block = m.alloc(len + 2);
	if (block) {
		u8* p = m.resolve(block + 1, len + 1, true);
		if (p) memcpy(p, text, len + 1);
	}
	wr32(at + 8, block ? block + 1 : 0);
	wr32(at + 12, block ? len : 0);
	wr32(at + 16, block ? len : 0);
}

void ScriptMemory::hookSync(void* user) { static_cast<ScriptMemory*>(user)->syncViews(); }

// ---- saved games ----

void ScriptMemory::save(ByteSink& out) const {
	out.u32v(0x4D454D31u); // "MEM1"
	out.u32v(_heapTop);
	for (u32 c = 0; c <= MAX_CLASS; ++c) out.u32v(_free[c]);
	out.put(_heap, _heapTop);
}

bool ScriptMemory::load(ByteSource& in) {
	if (!_vm || in.u32v() != 0x4D454D31u) return false;
	const u32 top = in.u32v();
	u32 freeLists[MAX_CLASS + 1];
	for (u32 c = 0; c <= MAX_CLASS; ++c) freeLists[c] = in.u32v();
	if (in.bad || top < FIRST_BLOCK || top > _heapMax || in.left() < top) return false;
	// Views of the old heap are gone.
	forgetBlock(HEAP_BASE, HEAP_END - HEAP_BASE);
	_vm->purge();
	_wrapDead = 0;
	if (top > _heapCap && !growHeap(top)) return false;
	in.get(_heap, top);
	memset(_heap + top, 0, _heapCap - top);
	_heapTop = top;
	memcpy(_free, freeLists, sizeof(_free));
	_stats.blocks = 0;
	_stats.heapUsed = 0;
	for (u32 off = FIRST_BLOCK; off < _heapTop;) {
		const u32 bytes = blockBytes(rd32(_heap + off) & 0xFF);
		if (rd32(_heap + off + 4) == TAG_USED) {
			wr32(_heap + off, rd32(_heap + off) & 0xFF);
			++_stats.blocks;
			_stats.heapUsed += 8 + bytes;
		}
		off += 8 + bytes;
	}
	return !in.bad;
}

} // namespace g3
