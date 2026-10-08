#include "script.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace g3 {

namespace {

const u32 kReturnToHost = 0xFFFFFFFFu;

u32 align4(u32 v) { return (v + 3u) & ~3u; }

char* copyString(const char* s) {
	const size_t n = strlen(s) + 1;
	char* c = static_cast<char*>(malloc(n));
	if (c) memcpy(c, s, n);
	return c;
}

} // namespace

// ---- loading ----

bool ScriptVm::load(Pak& pak, const char* name) {
	release();
	const PakEntry* e = pak.find(name);
	if (!e || e->size < sizeof(ScriptHeader)) return false;
	u8* blob = static_cast<u8*>(pak.readAlloc(*e));
	if (!blob) return false;
	return adopt(blob, e->size);
}

bool ScriptVm::adopt(u8* blob, u32 size) {
	release();
	if (!blob) return false;
	ScriptHeader h;
	bool ok = size >= sizeof(h);
	if (ok) {
		memcpy(&h, blob, sizeof(h));
		ok = h.magic == SCRIPT_MAGIC && h.version == SCRIPT_VERSION && h.symbolCount > 0 && h.symbolCount < (1u << 24) &&
		     h.functionCount < (1u << 24) && h.dataCount < (1u << 26) && h.stringCount < (1u << 24) &&
		     h.stringBytes < (1u << 28) && h.codeSize < (1u << 28) && h.instanceSlots < (1u << 24) &&
		     h.externalCount < (1u << 16);
	}
	u32 atSymbols = 0, atFunctions = 0, atData = 0, atOffsets = 0, atStrings = 0, atCode = 0;
	if (ok) {
		atSymbols = sizeof(ScriptHeader);
		atFunctions = atSymbols + h.symbolCount * u32(sizeof(ScriptSymbol));
		atData = atFunctions + h.functionCount * u32(sizeof(ScriptFunction));
		atOffsets = atData + h.dataCount * 4u;
		atStrings = atOffsets + h.stringCount * 4u;
		atCode = atStrings + align4(h.stringBytes);
		ok = u64(atCode) + align4(h.codeSize) == size;
	}
	const ScriptSymbol* symbols = reinterpret_cast<const ScriptSymbol*>(blob + atSymbols);
	const ScriptFunction* functions = reinterpret_cast<const ScriptFunction*>(blob + atFunctions);
	const u32* offsets = reinterpret_cast<const u32*>(blob + atOffsets);
	const char* strings = reinterpret_cast<const char*>(blob + atStrings);
	// Nothing in the tables may point outside them: the interpreter trusts
	// symbol contents from here on.
	for (u32 i = 0; ok && i < h.symbolCount; ++i) {
		const ScriptSymbol& s = symbols[i];
		const u32 n = s.count ? s.count : 1u;
		ok = s.type <= SCRIPT_INSTANCE && s.parent >= -1 && s.parent < i32(h.symbolCount);
		if (!ok) break;
		if (s.flags & SCRIPTF_MEMBER) continue; // checked against the instance on every access
		switch (s.type) {
		case SCRIPT_INT:
		case SCRIPT_FLOAT:
			ok = s.value <= h.dataCount && n <= h.dataCount - s.value;
			break;
		case SCRIPT_STRING:
			ok = s.value <= h.stringCount && n <= h.stringCount - s.value;
			break;
		case SCRIPT_FUNC:
			if (s.flags & SCRIPTF_EXTERNAL)
				ok = s.value < h.externalCount;
			else if (s.flags & SCRIPTF_CONST)
				ok = s.value == SCRIPT_NONE || s.value < h.codeSize;
			else
				ok = s.value <= h.dataCount && n <= h.dataCount - s.value;
			// Its parameters are the symbols that follow it.
			ok = ok && s.count < h.symbolCount - i;
			break;
		case SCRIPT_PROTOTYPE:
			ok = s.value == SCRIPT_NONE || s.value < h.codeSize;
			break;
		case SCRIPT_INSTANCE:
			ok = (s.value == SCRIPT_NONE || s.value < h.codeSize) && s.extra < h.instanceSlots;
			break;
		default:
			break;
		}
	}
	for (u32 i = 0; ok && i < h.functionCount; ++i)
		ok = functions[i].symbol < h.symbolCount && functions[i].address < h.codeSize &&
		     (i == 0 || functions[i - 1].address < functions[i].address);
	for (u32 i = 0; ok && i < h.stringCount; ++i) ok = offsets[i] == SCRIPT_NONE || offsets[i] < h.stringBytes;
	if (ok && h.stringBytes) ok = strings[h.stringBytes - 1] == 0;
	if (ok) {
		_strings = static_cast<const char**>(calloc(h.stringCount + 1, sizeof(char*)));
		_stringOwned = static_cast<u8*>(calloc(h.stringCount + 1, 1));
		_slots = static_cast<ScriptInstance**>(calloc(h.instanceSlots + 1, sizeof(ScriptInstance*)));
		_externals = static_cast<Bound*>(calloc(h.externalCount + 1, sizeof(Bound)));
		_stack = static_cast<Slot*>(calloc(STACK, sizeof(Slot)));
		_frames = static_cast<Frame*>(calloc(FRAMES, sizeof(Frame)));
		ok = _strings && _stringOwned && _slots && _externals && _stack && _frames;
	}
	if (!ok) {
		free(blob);
		release();
		return false;
	}
	_blob = blob;
	_blobSize = size;
	_h = h;
	_symbols = symbols;
	_functions = functions;
	_data = reinterpret_cast<i32*>(blob + atData);
	_code = blob + atCode;
	for (u32 i = 0; i < h.stringCount; ++i) _strings[i] = offsets[i] == SCRIPT_NONE ? "" : strings + offsets[i];
	_self = find("SELF");
	_other = find("OTHER");
	_hero = find("HERO");
	u32* const globals[3] = {&_self, &_other, &_hero};
	for (u32* g : globals)
		if (*g != SCRIPT_NONE && _symbols[*g].type != SCRIPT_INSTANCE) *g = SCRIPT_NONE;
	_stats = ScriptStats {};
	return true;
}

void ScriptVm::release() {
	for (u32 i = 0; i < _instanceCount; ++i) free(_instances[i]);
	free(_instances);
	for (u32 i = 0; i < _dynamicCount; ++i) free(_dynamic[i]);
	free(_dynamic);
	if (_strings && _stringOwned)
		for (u32 i = 0; i < _h.stringCount; ++i)
			if (_stringOwned[i]) free(const_cast<char*>(_strings[i]));
	for (char*& t : _temps) {
		free(t);
		t = nullptr;
	}
	free(_strings);
	free(_stringOwned);
	free(_slots);
	free(_externals);
	free(_stack);
	free(_frames);
	free(_blob);
	_instances = nullptr;
	_dynamic = nullptr;
	_strings = nullptr;
	_stringOwned = nullptr;
	_slots = nullptr;
	_externals = nullptr;
	_stack = nullptr;
	_frames = nullptr;
	_blob = nullptr;
	_symbols = nullptr;
	_functions = nullptr;
	_data = nullptr;
	_code = nullptr;
	_blobSize = 0;
	_instanceCount = _instanceCap = _serial = 0;
	_dynamicCount = _dynamicCap = _dynamicFree = 0;
	_sp = _fp = _pc = 0;
	_depth = 0;
	_tempNext = 0;
	_current = nullptr;
	_abandon = false;
	_self = _other = _hero = SCRIPT_NONE;
	_h = ScriptHeader {};
	_stats = ScriptStats {};
}

// ---- symbols ----

u32 ScriptVm::findHash(u32 nameHash) const {
	if (!_blob || nameHash == 0) return SCRIPT_NONE;
	for (u32 i = 0; i < _h.symbolCount; ++i)
		if (_symbols[i].nameHash == nameHash) return i;
	return SCRIPT_NONE;
}

u32 ScriptVm::find(const char* name) const { return findHash(hashName(name)); }

u32 ScriptVm::classOf(u32 symbol) const {
	for (int hops = 0; hops < 16 && symbol < _h.symbolCount; ++hops) {
		const ScriptSymbol& s = _symbols[symbol];
		if (s.type == SCRIPT_CLASS) return symbol;
		if (s.parent < 0) break;
		symbol = u32(s.parent);
	}
	return SCRIPT_NONE;
}

u32 ScriptVm::findMember(const char* cls, const char* member) const {
	const u32 c = find(cls);
	if (c == SCRIPT_NONE || _symbols[c].type != SCRIPT_CLASS) return SCRIPT_NONE;
	char full[96];
	snprintf(full, sizeof(full), "%s.%s", cls, member);
	const u32 h = hashName(full);
	for (u32 i = 0; i < _h.symbolCount; ++i) {
		const ScriptSymbol& s = _symbols[i];
		if (s.nameHash == h && (s.flags & SCRIPTF_MEMBER) && s.parent == i32(c)) return i;
	}
	return SCRIPT_NONE;
}

bool ScriptVm::bind(const char* name, External fn, void* user) { return bindSymbol(find(name), fn, user); }

bool ScriptVm::bindSymbol(u32 s, External fn, void* user) {
	if (s >= _h.symbolCount || _symbols[s].type != SCRIPT_FUNC || !(_symbols[s].flags & SCRIPTF_EXTERNAL)) return false;
	_externals[_symbols[s].value] = {fn, user};
	return true;
}

u32 ScriptVm::functionAt(u32 address) const {
	u32 lo = 0, hi = _h.functionCount;
	while (lo < hi) {
		const u32 mid = (lo + hi) / 2;
		if (_functions[mid].address < address)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < _h.functionCount && _functions[lo].address == address ? _functions[lo].symbol : SCRIPT_NONE;
}

// ---- errors ----

void ScriptVm::fail(const char* what) {
	++_stats.errors;
	snprintf(_stats.lastError, sizeof(_stats.lastError), "%s (at %u)", what, unsigned(_pc));
}

void ScriptVm::abandon(const char* what) {
	if (!_abandon) {
		++_stats.aborted;
		snprintf(_stats.lastError, sizeof(_stats.lastError), "%s (at %u)", what, unsigned(_pc));
		if (_stats.aborted <= 3 && !_quiet) logf("script: call abandoned: %s", _stats.lastError);
	}
	_abandon = true;
}

// ---- the stack ----

void ScriptVm::push(const Slot& s) {
	if (_sp >= STACK) {
		abandon("stack overflow");
		return;
	}
	_stack[_sp++] = s;
}

ScriptVm::Slot ScriptVm::pop() {
	if (_sp == 0) {
		Slot s {};
		s.kind = K_INT;
		return s;
	}
	return _stack[--_sp];
}

bool ScriptVm::popRef(Slot& out) {
	if (_sp == 0) {
		fail("a variable was expected on an empty stack");
		return false;
	}
	out = _stack[--_sp];
	if (out.kind != K_REF) {
		fail("a variable was expected, a value found");
		return false;
	}
	return true;
}

void ScriptVm::pushInt(i32 v) {
	Slot s {};
	s.kind = K_INT;
	s.i = v;
	push(s);
}

void ScriptVm::pushFloat(float v) {
	Slot s {};
	s.kind = K_FLOAT;
	s.f = v;
	push(s);
}

void ScriptVm::pushInstance(ScriptInstance* inst) {
	Slot s {};
	s.kind = K_INST;
	s.ctx = inst;
	push(s);
}

void ScriptVm::pushString(const char* text) {
	// A string that is no variable's lives in one of a ring of temporaries.
	const u32 t = _tempNext;
	_tempNext = (_tempNext + 1) % TEMPS;
	char* copy = copyString(text ? text : "");
	free(_temps[t]);
	_temps[t] = copy;
	Slot s {};
	s.kind = K_REF;
	s.index = u8(t);
	s.sym = _h.symbolCount; // the pseudo-symbol of the temporaries
	push(s);
}

i32 ScriptVm::popInt() {
	if (_sp == 0) return 0;
	const Slot s = _stack[--_sp];
	if (s.kind == K_INT) return s.i;
	if (s.kind == K_REF) return getInt(s.sym, s.index, s.ctx);
	fail("an integer was expected");
	return 0;
}

float ScriptVm::popFloat() {
	if (_sp == 0) return 0.0f;
	const Slot s = _stack[--_sp];
	if (s.kind == K_FLOAT) return s.f;
	if (s.kind == K_INT) {
		// A float literal travels as the integer with its bits.
		float f;
		memcpy(&f, &s.i, 4);
		return f;
	}
	if (s.kind == K_REF) return getFloat(s.sym, s.index, s.ctx);
	fail("a float was expected");
	return 0.0f;
}

const char* ScriptVm::popString() {
	Slot s;
	if (!popRef(s)) return "";
	return stringAt(s.sym, s.index, s.ctx);
}

ScriptInstance* ScriptVm::popInstance() {
	if (_sp == 0) {
		fail("an instance was expected on an empty stack");
		return nullptr;
	}
	const Slot s = _stack[--_sp];
	if (s.kind == K_INST) return s.ctx;
	if (s.kind == K_REF) return instanceOf(s.sym);
	fail("an instance was expected");
	return nullptr;
}

// ---- variables ----

u8* ScriptVm::memberPtr(const ScriptSymbol& s, u32 index, const ScriptInstance* ctx, u32 bytes) const {
	if (!ctx) {
		++_stats.errors;
		snprintf(_stats.lastError, sizeof(_stats.lastError), "a member was used without an instance");
		return nullptr;
	}
	const u64 at = u64(s.value) + u64(index) * bytes;
	if (at + bytes > ctx->size) {
		++_stats.errors;
		snprintf(_stats.lastError, sizeof(_stats.lastError), "a member lies outside its instance");
		return nullptr;
	}
	return const_cast<u8*>(ctx->data()) + at;
}

i32 ScriptVm::getInt(u32 symbol, u32 index, const ScriptInstance* ctx) const {
	if (symbol >= _h.symbolCount) return 0;
	const ScriptSymbol& s = _symbols[symbol];
	if ((s.type != SCRIPT_INT && s.type != SCRIPT_FUNC) || index >= (s.count ? s.count : 1u)) {
		++_stats.errors;
		snprintf(_stats.lastError, sizeof(_stats.lastError), "symbol %u was read as an integer", unsigned(symbol));
		return 0;
	}
	i32 v = 0;
	if (s.flags & SCRIPTF_MEMBER) {
		if (const u8* p = memberPtr(s, index, ctx, 4)) memcpy(&v, p, 4);
		return v;
	}
	if (s.type == SCRIPT_FUNC && (s.flags & (SCRIPTF_CONST | SCRIPTF_EXTERNAL))) return 0; // a function is not a variable
	return _data[s.value + index];
}

float ScriptVm::getFloat(u32 symbol, u32 index, const ScriptInstance* ctx) const {
	if (symbol >= _h.symbolCount) return 0.0f;
	const ScriptSymbol& s = _symbols[symbol];
	if (s.type != SCRIPT_FLOAT || index >= (s.count ? s.count : 1u)) {
		++_stats.errors;
		snprintf(_stats.lastError, sizeof(_stats.lastError), "symbol %u was read as a float", unsigned(symbol));
		return 0.0f;
	}
	float v = 0;
	if (s.flags & SCRIPTF_MEMBER) {
		if (const u8* p = memberPtr(s, index, ctx, 4)) memcpy(&v, p, 4);
		return v;
	}
	memcpy(&v, &_data[s.value + index], 4);
	return v;
}

const char* ScriptVm::stringAt(u32 symbol, u32 index, const ScriptInstance* ctx) const {
	if (symbol == _h.symbolCount) return index < TEMPS && _temps[index] ? _temps[index] : "";
	if (symbol >= _h.symbolCount) return "";
	const ScriptSymbol& s = _symbols[symbol];
	if (s.type != SCRIPT_STRING || index >= (s.count ? s.count : 1u)) {
		++_stats.errors;
		snprintf(_stats.lastError, sizeof(_stats.lastError), "symbol %u was read as a string", unsigned(symbol));
		return "";
	}
	if (s.flags & SCRIPTF_MEMBER) {
		const u8* p = memberPtr(s, index, ctx, SCRIPT_STRING_SIZE);
		if (!p) return "";
		u32 handle;
		memcpy(&handle, p, 4);
		return handle && handle <= _dynamicCount && _dynamic[handle - 1] ? _dynamic[handle - 1] : "";
	}
	return _strings[s.value + index];
}

const char* ScriptVm::getString(u32 symbol, u32 index, const ScriptInstance* ctx) const {
	return stringAt(symbol, index, ctx);
}

void ScriptVm::setInt(u32 symbol, u32 index, ScriptInstance* ctx, i32 v) {
	if (symbol >= _h.symbolCount) return;
	const ScriptSymbol& s = _symbols[symbol];
	if ((s.type != SCRIPT_INT && s.type != SCRIPT_FUNC) || index >= (s.count ? s.count : 1u)) {
		fail("an integer was written to something else");
		return;
	}
	if (s.flags & (SCRIPTF_CONST | SCRIPTF_EXTERNAL)) {
		fail("a constant was written to");
		return;
	}
	if (s.flags & SCRIPTF_MEMBER) {
		if (u8* p = memberPtr(s, index, ctx, 4)) memcpy(p, &v, 4);
		return;
	}
	_data[s.value + index] = v;
}

void ScriptVm::setFloat(u32 symbol, u32 index, ScriptInstance* ctx, float v) {
	if (symbol >= _h.symbolCount) return;
	const ScriptSymbol& s = _symbols[symbol];
	if (s.type != SCRIPT_FLOAT || index >= (s.count ? s.count : 1u)) {
		fail("a float was written to something else");
		return;
	}
	if (s.flags & SCRIPTF_CONST) {
		fail("a constant was written to");
		return;
	}
	if (s.flags & SCRIPTF_MEMBER) {
		if (u8* p = memberPtr(s, index, ctx, 4)) memcpy(p, &v, 4);
		return;
	}
	memcpy(&_data[s.value + index], &v, 4);
}

u32 ScriptVm::newDynamic(const char* text) {
	char* copy = copyString(text);
	if (!copy) return 0;
	_stats.stringBytes += u32(strlen(copy)) + 1;
	// A freed handle, if there is one.
	if (_dynamicFree) {
		for (u32 i = 0; i < _dynamicCount; ++i)
			if (!_dynamic[i]) {
				_dynamic[i] = copy;
				--_dynamicFree;
				return i + 1;
			}
		_dynamicFree = 0;
	}
	if (_dynamicCount == _dynamicCap) {
		const u32 cap = _dynamicCap ? _dynamicCap * 2 : 256;
		char** grown = static_cast<char**>(realloc(_dynamic, size_t(cap) * sizeof(char*)));
		if (!grown) {
			free(copy);
			return 0;
		}
		_dynamic = grown;
		_dynamicCap = cap;
	}
	_dynamic[_dynamicCount++] = copy;
	return _dynamicCount;
}

void ScriptVm::freeDynamic(u32 handle) {
	if (!handle || handle > _dynamicCount || !_dynamic[handle - 1]) return;
	_stats.stringBytes -= u32(strlen(_dynamic[handle - 1])) + 1;
	free(_dynamic[handle - 1]);
	_dynamic[handle - 1] = nullptr;
	++_dynamicFree;
}

void ScriptVm::setString(u32 symbol, u32 index, ScriptInstance* ctx, const char* v) {
	if (symbol >= _h.symbolCount) return;
	const ScriptSymbol& s = _symbols[symbol];
	if (s.type != SCRIPT_STRING || index >= (s.count ? s.count : 1u)) {
		fail("a string was written to something else");
		return;
	}
	if (s.flags & SCRIPTF_CONST) {
		fail("a constant was written to");
		return;
	}
	if (!v) v = "";
	if (s.flags & SCRIPTF_MEMBER) {
		u8* p = memberPtr(s, index, ctx, SCRIPT_STRING_SIZE);
		if (!p) return;
		u32 old;
		memcpy(&old, p, 4);
		// (The new text may be the old one: copy before freeing.)
		const u32 handle = *v ? newDynamic(v) : 0;
		freeDynamic(old);
		memcpy(p, &handle, 4);
		return;
	}
	const u32 slot = s.value + index;
	char* copy = copyString(v);
	if (!copy) return;
	if (_stringOwned[slot]) free(const_cast<char*>(_strings[slot]));
	_strings[slot] = copy;
	_stringOwned[slot] = 1;
}

ScriptInstance* ScriptVm::instanceOf(u32 symbol) const {
	if (symbol >= _h.symbolCount || _symbols[symbol].type != SCRIPT_INSTANCE || (_symbols[symbol].flags & SCRIPTF_MEMBER))
		return nullptr;
	return _slots[_symbols[symbol].extra];
}

void ScriptVm::setInstance(u32 symbol, ScriptInstance* inst) {
	if (symbol >= _h.symbolCount || _symbols[symbol].type != SCRIPT_INSTANCE || (_symbols[symbol].flags & SCRIPTF_MEMBER))
		return;
	_slots[_symbols[symbol].extra] = inst;
}

// ---- instances ----

ScriptInstance* ScriptVm::allocate(u32 instanceSymbol) {
	if (!_blob || instanceSymbol >= _h.symbolCount) return nullptr;
	const u32 cls = classOf(instanceSymbol);
	if (cls == SCRIPT_NONE || _symbols[instanceSymbol].type != SCRIPT_INSTANCE) return nullptr;
	const u32 size = _symbols[cls].value;
	if (size > (1u << 16)) return nullptr;
	if (_instanceCount == _instanceCap) {
		const u32 cap = _instanceCap ? _instanceCap * 2 : 256;
		ScriptInstance** grown = static_cast<ScriptInstance**>(realloc(_instances, size_t(cap) * sizeof(ScriptInstance*)));
		if (!grown) return nullptr;
		_instances = grown;
		_instanceCap = cap;
	}
	ScriptInstance* inst = static_cast<ScriptInstance*>(calloc(1, sizeof(ScriptInstance) + size));
	if (!inst) return nullptr;
	inst->classSymbol = cls;
	inst->symbol = instanceSymbol;
	inst->size = size;
	inst->serial = _serial++;
	_instances[_instanceCount++] = inst;
	++_stats.instances;
	_stats.instanceBytes += u32(sizeof(ScriptInstance)) + size;
	// The definition's name refers to the newest object made from it.
	setInstance(instanceSymbol, inst);
	return inst;
}

bool ScriptVm::construct(ScriptInstance* inst) {
	if (!inst || inst->symbol >= _h.symbolCount) return false;
	const ScriptSymbol& s = _symbols[inst->symbol];
	if (s.value == SCRIPT_NONE) return true;
	ScriptInstance* const before = _current;
	ScriptInstance* const selfBefore = _self != SCRIPT_NONE ? instanceOf(_self) : nullptr;
	_current = inst;
	if (_self != SCRIPT_NONE) setInstance(_self, inst);
	const bool ok = run(inst->symbol, s.value);
	_current = before;
	if (_self != SCRIPT_NONE) setInstance(_self, selfBefore);
	return ok;
}

ScriptInstance* ScriptVm::create(u32 instanceSymbol) {
	ScriptInstance* inst = allocate(instanceSymbol);
	if (inst) construct(inst);
	return inst;
}

void ScriptVm::destroy(ScriptInstance* inst) {
	if (!inst) return;
	// Its strings.
	for (u32 i = 0; i < _h.symbolCount; ++i) {
		const ScriptSymbol& s = _symbols[i];
		if (s.type != SCRIPT_STRING || !(s.flags & SCRIPTF_MEMBER) || s.parent != i32(inst->classSymbol)) continue;
		for (u32 k = 0; k < (s.count ? s.count : 1u); ++k) {
			const u64 at = u64(s.value) + u64(k) * SCRIPT_STRING_SIZE;
			if (at + 4 > inst->size) break;
			u32 handle;
			memcpy(&handle, inst->data() + at, 4);
			freeDynamic(handle);
		}
	}
	for (u32 i = 0; i < _h.instanceSlots; ++i)
		if (_slots[i] == inst) _slots[i] = nullptr;
	if (_current == inst) _current = nullptr;
	for (u32 i = 0; i < _fp; ++i)
		if (_frames[i].context == inst) _frames[i].context = nullptr;
	for (u32 i = 0; i < _sp; ++i)
		if (_stack[i].ctx == inst) _stack[i].ctx = nullptr;
	for (u32 i = 0; i < _instanceCount; ++i)
		if (_instances[i] == inst) {
			_instances[i] = _instances[--_instanceCount];
			break;
		}
	--_stats.instances;
	_stats.instanceBytes -= u32(sizeof(ScriptInstance)) + inst->size;
	free(inst);
}

// ---- running ----

bool ScriptVm::call(u32 function) {
	if (!_blob || function >= _h.symbolCount) return false;
	const ScriptSymbol& s = _symbols[function];
	if (s.type != SCRIPT_FUNC) return false;
	if (s.flags & SCRIPTF_EXTERNAL) {
		callExternal(function);
		return true;
	}
	if (!(s.flags & SCRIPTF_CONST) || s.value == SCRIPT_NONE) return false;
	return run(function, s.value);
}

void ScriptVm::enter(u32 symbol, u32 address, u32 returnPc) {
	if (_fp >= FRAMES) {
		abandon("calls nested too deep");
		return;
	}
	const ScriptSymbol& s = _symbols[symbol];
	const u32 params = s.type == SCRIPT_FUNC ? s.count : 0u;
	Frame& f = _frames[_fp++];
	f.returnPc = returnPc;
	f.base = _sp > params ? _sp - params : 0;
	f.function = symbol;
	f.context = _current;
	_pc = address;
}

// Back to the caller. The caller is left with exactly what the function's
// declaration promises, whatever its code did: nothing, or one result.
void ScriptVm::leave() {
	const Frame f = _frames[--_fp];
	const ScriptSymbol& s = _symbols[f.function];
	if (s.type == SCRIPT_FUNC && (s.flags & SCRIPTF_RETURN)) {
		if (_sp <= f.base) {
			_sp = f.base;
			switch (s.extra) {
			case SCRIPT_FLOAT: pushFloat(0.0f); break;
			case SCRIPT_INT:
			case SCRIPT_FUNC: pushInt(0); break;
			case SCRIPT_STRING: pushString(""); break;
			case SCRIPT_INSTANCE: pushInstance(nullptr); break;
			default: break;
			}
		} else if (_sp - f.base > 1) {
			const Slot top = _stack[_sp - 1];
			_sp = f.base;
			_stack[_sp++] = top;
		}
	} else {
		_sp = f.base;
	}
	_pc = f.returnPc;
	_current = f.context;
}

void ScriptVm::callExternal(u32 symbol) {
	const ScriptSymbol& s = _symbols[symbol];
	const Bound b = _externals[s.value];
	if (b.fn) {
		// Framed like a script function, so that a host function that takes
		// too few or too many values cannot unbalance the scripts' stack.
		if (_fp >= FRAMES) {
			abandon("calls nested too deep");
			return;
		}
		const u32 pc = _pc;
		Frame& f = _frames[_fp++];
		f.returnPc = pc;
		f.base = _sp > s.count ? _sp - s.count : 0;
		f.function = symbol;
		f.context = _current;
		b.fn(*this, b.user);
		leave();
		return;
	}
	// Nobody home: drop the arguments, answer with a zero.
	for (u32 i = s.count; i > 0; --i) {
		const ScriptSymbol& p = _symbols[symbol + i];
		if (p.type == SCRIPT_FLOAT)
			(void)popFloat();
		else if (p.type == SCRIPT_INT || p.type == SCRIPT_FUNC)
			(void)popInt();
		else if (_sp)
			--_sp;
	}
	if (s.flags & SCRIPTF_RETURN) {
		switch (s.extra) {
		case SCRIPT_FLOAT: pushFloat(0.0f); break;
		case SCRIPT_INT:
		case SCRIPT_FUNC: pushInt(0); break;
		case SCRIPT_STRING: pushString(""); break;
		case SCRIPT_INSTANCE: pushInstance(nullptr); break;
		default: break;
		}
	}
	++_stats.stubCalls;
	if (_unbound) _unbound(symbol, _unboundUser);
}

bool ScriptVm::run(u32 symbol, u32 address) {
	if (!_blob || _abandon) return false;
	if (address >= _h.codeSize) return false;
	if (_depth == 0) _spent = 0;
	if (_depth >= 24) {
		abandon("scripts and host calling each other too deep");
		return false;
	}
	++_depth;
	const u32 floor = _fp, pcBefore = _pc;
	enter(symbol, address, kReturnToHost);
	while (!_abandon && _fp > floor) {
		if (!step()) break;
	}
	bool ok = !_abandon;
	if (_abandon) {
		// Unwind to where this call started.
		while (_fp > floor) {
			const Frame f = _frames[--_fp];
			_sp = min(_sp, f.base);
			_current = f.context;
		}
	}
	_pc = pcBefore;
	--_depth;
	if (_depth == 0) _abandon = false; // the outermost caller has been told
	return ok;
}

// One instruction. False when the function the host called has returned.
bool ScriptVm::step() {
	if (++_spent > _budget) {
		abandon("a script ran too long");
		return false;
	}
	++_stats.instructions;
	const u32 pc = _pc;
	if (pc >= _h.codeSize) {
		abandon("execution left the code");
		return false;
	}
	const u8 op = _code[pc];
	u32 operand = 0;
	u32 size = 1;
	switch (op) {
	case OP_BL:
	case OP_BE:
	case OP_PUSHI:
	case OP_PUSHV:
	case OP_PUSHVI:
	case OP_B:
	case OP_BZ:
	case OP_GMOVI:
		size = 5;
		break;
	case OP_PUSHVV:
		size = 6;
		break;
	default:
		break;
	}
	if (pc + size > _h.codeSize) {
		abandon("an instruction is cut off by the end of the code");
		return false;
	}
	if (size >= 5) memcpy(&operand, _code + pc + 1, 4);
	_pc = pc + size;

	i32 a, b;
	Slot ref;
	switch (op) {
	case OP_ADD:
		a = popInt();
		b = popInt();
		pushInt(i32(u32(a) + u32(b)));
		break;
	case OP_SUB:
		a = popInt();
		b = popInt();
		pushInt(i32(u32(a) - u32(b)));
		break;
	case OP_MUL:
		a = popInt();
		b = popInt();
		pushInt(i32(u32(a) * u32(b)));
		break;
	case OP_DIV:
	case OP_MOD:
		a = popInt();
		b = popInt();
		if (b == 0) {
			fail("division by zero");
			pushInt(0);
		} else if (b == -1) {
			pushInt(op == OP_DIV ? i32(0u - u32(a)) : 0);
		} else {
			pushInt(op == OP_DIV ? a / b : a % b);
		}
		break;
	case OP_OR:
		a = popInt();
		b = popInt();
		pushInt(a | b);
		break;
	case OP_ANDB:
		a = popInt();
		b = popInt();
		pushInt(a & b);
		break;
	case OP_LT:
		a = popInt();
		b = popInt();
		pushInt(a < b);
		break;
	case OP_GT:
		a = popInt();
		b = popInt();
		pushInt(a > b);
		break;
	case OP_LTE:
		a = popInt();
		b = popInt();
		pushInt(a <= b);
		break;
	case OP_GTE:
		a = popInt();
		b = popInt();
		pushInt(a >= b);
		break;
	case OP_EQ:
		a = popInt();
		b = popInt();
		pushInt(a == b);
		break;
	case OP_NEQ:
		a = popInt();
		b = popInt();
		pushInt(a != b);
		break;
	case OP_ORR:
		a = popInt();
		b = popInt();
		pushInt(a || b);
		break;
	case OP_AND:
		a = popInt();
		b = popInt();
		pushInt(a && b);
		break;
	case OP_LSL:
		a = popInt();
		b = popInt();
		pushInt(i32(u32(a) << (u32(b) & 31u)));
		break;
	case OP_LSR:
		a = popInt();
		b = popInt();
		pushInt(a >> (u32(b) & 31u));
		break;
	case OP_PLUS:
		pushInt(popInt());
		break;
	case OP_NEGATE:
		pushInt(i32(0u - u32(popInt())));
		break;
	case OP_NOT:
		pushInt(!popInt());
		break;
	case OP_CMPL:
		pushInt(~popInt());
		break;
	case OP_NOP:
		break;
	case OP_RSR:
		leave();
		break;
	case OP_BL: {
		const u32 target = functionAt(operand);
		if (target == SCRIPT_NONE) {
			fail("a call to an address where no function starts");
			break;
		}
		enter(target, operand, _pc);
		break;
	}
	case OP_BE:
		if (operand >= _h.symbolCount || _symbols[operand].type != SCRIPT_FUNC ||
		    !(_symbols[operand].flags & SCRIPTF_EXTERNAL)) {
			fail("a call to an external that is none");
			break;
		}
		callExternal(operand);
		break;
	case OP_PUSHI:
		pushInt(i32(operand));
		break;
	case OP_PUSHV:
	case OP_PUSHVI:
	case OP_PUSHVV: {
		if (operand >= _h.symbolCount) {
			fail("a symbol that does not exist was pushed");
			pushInt(0);
			break;
		}
		Slot s {};
		s.kind = K_REF;
		s.index = op == OP_PUSHVV ? _code[pc + 5] : u8(0);
		s.sym = operand;
		s.ctx = _current;
		push(s);
		break;
	}
	case OP_MOVI:
	case OP_MOVVF:
		if (popRef(ref)) {
			a = popInt();
			setInt(ref.sym, ref.index, ref.ctx, a);
		}
		break;
	case OP_MOVF:
		if (popRef(ref)) {
			const float v = popFloat();
			setFloat(ref.sym, ref.index, ref.ctx, v);
		}
		break;
	case OP_MOVS:
		if (popRef(ref)) {
			const char* v = popString();
			if (ref.sym == _h.symbolCount)
				fail("a temporary string was assigned to");
			else
				setString(ref.sym, ref.index, ref.ctx, v);
		}
		break;
	case OP_ADDMOVI:
	case OP_SUBMOVI:
	case OP_MULMOVI:
	case OP_DIVMOVI:
		if (popRef(ref)) {
			a = popInt();
			if (op == OP_DIVMOVI && a == 0) {
				fail("division by zero");
				break;
			}
			const i32 old = getInt(ref.sym, ref.index, ref.ctx);
			i32 v;
			if (op == OP_ADDMOVI)
				v = i32(u32(old) + u32(a));
			else if (op == OP_SUBMOVI)
				v = i32(u32(old) - u32(a));
			else if (op == OP_MULMOVI)
				v = i32(u32(old) * u32(a));
			else
				v = a == -1 ? i32(0u - u32(old)) : old / a;
			setInt(ref.sym, ref.index, ref.ctx, v);
		}
		break;
	case OP_MOVVI:
		if (popRef(ref)) {
			ScriptInstance* inst = popInstance();
			if (ref.sym < _h.symbolCount && _symbols[ref.sym].type == SCRIPT_INSTANCE)
				setInstance(ref.sym, inst);
			else
				fail("an instance was assigned to something else");
		}
		break;
	case OP_B:
		if (operand >= _h.codeSize) {
			abandon("a jump out of the code");
			return false;
		}
		_pc = operand;
		break;
	case OP_BZ:
		if (popInt() == 0) {
			if (operand >= _h.codeSize) {
				abandon("a jump out of the code");
				return false;
			}
			_pc = operand;
		}
		break;
	case OP_GMOVI:
		if (operand >= _h.symbolCount || _symbols[operand].type != SCRIPT_INSTANCE) {
			fail("the current instance was set from something else");
			break;
		}
		_current = instanceOf(operand);
		break;
	case OP_MOVSS:
	default:
		abandon("an instruction the machine does not know");
		return false;
	}
	return !_abandon;
}

} // namespace g3
