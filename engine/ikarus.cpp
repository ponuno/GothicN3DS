#include "ikarus.h"

#include "bytes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace g3 {

namespace {

#define NATIVE(name) void name(ScriptVm& vm, [[maybe_unused]] void* user)
#define IK (*static_cast<Ikarus*>(user))
#define MEM (static_cast<Ikarus*>(user)->memory())

u32 crc32(const u8* p, u32 n) {
	u32 c = 0xFFFFFFFFu;
	for (u32 i = 0; i < n; ++i) {
		c ^= p[i];
		for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
	}
	return ~c;
}

// A copy of a string the machine handed out (it is valid only until the machine runs again).
struct Text {
	char* s = nullptr;
	explicit Text(const char* t) {
		const size_t n = strlen(t ? t : "") + 1;
		s = static_cast<char*>(malloc(n));
		if (s) memcpy(s, t ? t : "", n);
	}
	~Text() { free(s); }
	const char* c() const { return s ? s : ""; }
	Text(const Text&) = delete;
	Text& operator=(const Text&) = delete;
};

// The function a `var func` argument names: its number, pushed as a number or as the function's own symbol, or the
// value of a function variable.
u32 funcOf(ScriptVm& vm, const ScriptValue& v) {
	u32 sym = SCRIPT_NONE;
	if (v.kind == ScriptValue::INT) {
		sym = u32(v.i);
	} else if (v.kind == ScriptValue::VARIABLE && v.symbol < vm.symbolCount()) {
		const ScriptSymbol& s = vm.symbol(v.symbol);
		if (s.type == SCRIPT_FUNC && (s.flags & SCRIPTF_CONST))
			sym = v.symbol;
		else
			sym = u32(vm.getInt(v.symbol, v.index, v.ctx));
	}
	if (sym >= vm.symbolCount()) return SCRIPT_NONE;
	const u8 t = vm.symbol(sym).type;
	return t == SCRIPT_FUNC || t == SCRIPT_PROTOTYPE || t == SCRIPT_INSTANCE ? sym : SCRIPT_NONE;
}

u32 popFunc(ScriptVm& vm) {
	ScriptValue v;
	vm.popValue(v);
	return funcOf(vm, v);
}

// An instance argument given as its symbol's number (Ikarus' `var int inst`), or as the object.
ScriptInstance* popObject(ScriptVm& vm, u32* symbol = nullptr) {
	ScriptValue v;
	vm.popValue(v);
	if (symbol) *symbol = SCRIPT_NONE;
	if (v.kind == ScriptValue::OBJECT) return v.ctx;
	u32 sym = SCRIPT_NONE;
	if (v.kind == ScriptValue::INT) sym = u32(v.i);
	if (v.kind == ScriptValue::VARIABLE) {
		if (v.symbol < vm.symbolCount() && vm.symbol(v.symbol).type == SCRIPT_INSTANCE)
			sym = v.symbol;
		else
			sym = u32(vm.getInt(v.symbol, v.index, v.ctx));
	}
	if (sym >= vm.symbolCount() || vm.symbol(sym).type != SCRIPT_INSTANCE) return nullptr;
	if (symbol) *symbol = sym;
	return vm.instanceOf(sym);
}

void callSymbol(ScriptVm& vm, u32 sym) {
	if (sym < vm.symbolCount()) vm.call(sym);
}

// ---- memory ----

NATIVE(nAlloc) {
	const i32 n = vm.popInt();
	vm.pushInt(i32(MEM.alloc(n > 0 ? u32(n) : 0u)));
}
NATIVE(nFree) { MEM.free(u32(vm.popInt())); }
NATIVE(nRealloc) {
	const i32 n = vm.popInt(), old = vm.popInt();
	const u32 p = u32(vm.popInt());
	vm.pushInt(i32(MEM.realloc(p, old > 0 ? u32(old) : 0u, n > 0 ? u32(n) : 0u)));
}
NATIVE(nReadInt) { vm.pushInt(MEM.readInt(u32(vm.popInt()))); }
NATIVE(nWriteInt) {
	const i32 v = vm.popInt();
	MEM.writeInt(u32(vm.popInt()), v);
}
NATIVE(nReadByte) { vm.pushInt(MEM.readByte(u32(vm.popInt()))); }
NATIVE(nWriteByte) {
	const i32 v = vm.popInt();
	MEM.writeByte(u32(vm.popInt()), u8(v));
}
NATIVE(nReadIntArray) {
	const i32 k = vm.popInt();
	const u32 a = u32(vm.popInt());
	vm.pushInt(MEM.readInt(a + u32(k) * 4));
}
NATIVE(nWriteIntArray) {
	const i32 v = vm.popInt(), k = vm.popInt();
	const u32 a = u32(vm.popInt());
	MEM.writeInt(a + u32(k) * 4, v);
}
NATIVE(nReadByteArray) {
	const i32 k = vm.popInt();
	const u32 a = u32(vm.popInt());
	vm.pushInt(MEM.readByte(a + u32(k)));
}
NATIVE(nWriteByteArray) {
	const i32 v = vm.popInt(), k = vm.popInt();
	const u32 a = u32(vm.popInt());
	MEM.writeByte(a + u32(k), u8(v));
}
NATIVE(nCopyBytes) {
	const i32 n = vm.popInt();
	const u32 to = u32(vm.popInt()), from = u32(vm.popInt());
	if (n > 0) MEM.copy(from, to, u32(n));
}
NATIVE(nCopyWords) {
	const i32 n = vm.popInt();
	const u32 to = u32(vm.popInt()), from = u32(vm.popInt());
	if (n > 0) MEM.copy(from, to, u32(n) * 4);
}
void swapBytes(ScriptMemory& m, u32 a, u32 b, u32 n) {
	for (u32 k = 0; k < n; ++k) {
		const u8 x = m.readByte(a + k);
		m.writeByte(a + k, m.readByte(b + k));
		m.writeByte(b + k, x);
	}
}
NATIVE(nSwapBytes) {
	const i32 n = vm.popInt();
	const u32 b = u32(vm.popInt()), a = u32(vm.popInt());
	if (n > 0) swapBytes(MEM, a, b, u32(n));
}
NATIVE(nSwapWords) {
	const i32 n = vm.popInt();
	const u32 b = u32(vm.popInt()), a = u32(vm.popInt());
	if (n > 0) swapBytes(MEM, a, b, u32(n) * 4);
}
NATIVE(nClear) {
	const i32 n = vm.popInt();
	const u32 a = u32(vm.popInt());
	if (n > 0) MEM.fill(a, 0, u32(n));
}
bool equalBytes(ScriptMemory& m, u32 a, u32 b, u32 n) {
	for (u32 k = 0; k < n; ++k)
		if (m.readByte(a + k) != m.readByte(b + k)) return false;
	return true;
}
NATIVE(nCompareBytes) {
	const i32 n = vm.popInt();
	const u32 b = u32(vm.popInt()), a = u32(vm.popInt());
	vm.pushInt(n <= 0 ? 1 : (a && b && equalBytes(MEM, a, b, u32(n))) ? 1 : 0);
}
NATIVE(nCompareWords) {
	const i32 n = vm.popInt();
	const u32 b = u32(vm.popInt()), a = u32(vm.popInt());
	vm.pushInt(n <= 0 ? 1 : (a && b && equalBytes(MEM, a, b, u32(n) * 4)) ? 1 : 0);
}
NATIVE(nReadString) {
	char buf[1024];
	MEM.readZString(u32(vm.popInt()), buf, sizeof(buf));
	vm.pushString(buf);
}
NATIVE(nWriteString) {
	const Text s(vm.popString());
	MEM.writeZString(u32(vm.popInt()), s.c());
}
NATIVE(nReadStatArr) {
	const i32 k = vm.popInt();
	ScriptValue arr;
	vm.popValue(arr);
	vm.pushInt(arr.kind == ScriptValue::VARIABLE && k >= 0 ? vm.getInt(arr.symbol, u32(k), arr.ctx) : 0);
}
NATIVE(nWriteStatArr) {
	const i32 v = vm.popInt(), k = vm.popInt();
	ScriptValue arr;
	vm.popValue(arr);
	if (arr.kind == ScriptValue::VARIABLE && k >= 0) vm.setInt(arr.symbol, u32(k), arr.ctx, v);
}
NATIVE(nReadStatStringArr) {
	const i32 k = vm.popInt();
	ScriptValue arr;
	vm.popValue(arr);
	const Text s(arr.kind == ScriptValue::VARIABLE && k >= 0 ? vm.getString(arr.symbol, u32(k), arr.ctx) : "");
	vm.pushString(s.c());
}
NATIVE(nWriteStatStringArr) {
	const Text v(vm.popString());
	const i32 k = vm.popInt();
	ScriptValue arr;
	vm.popValue(arr);
	if (arr.kind == ScriptValue::VARIABLE && k >= 0) vm.setString(arr.symbol, u32(k), arr.ctx, v.c());
}
NATIVE(nCrc32) {
	const i32 n = vm.popInt();
	const u32 a = u32(vm.popInt());
	if (n <= 0 || n > (1 << 24)) {
		vm.pushInt(0);
		return;
	}
	u8* tmp = static_cast<u8*>(malloc(u32(n)));
	if (!tmp) {
		vm.pushInt(0);
		return;
	}
	for (i32 k = 0; k < n; ++k) tmp[k] = MEM.readByte(a + u32(k));
	vm.pushInt(i32(crc32(tmp, u32(n))));
	free(tmp);
}
NATIVE(nStringHash) {
	const Text s(vm.popString());
	vm.pushInt(i32(crc32(reinterpret_cast<const u8*>(s.c()), u32(strlen(s.c())))));
}

// ---- addresses and objects ----

NATIVE(nAddressOf) {
	ScriptValue v;
	vm.popValue(v);
	vm.pushInt(i32(MEM.addressOf(v)));
}
NATIVE(nPtrToInst) { vm.pushInstance(MEM.objectAt(u32(vm.popInt()))); }
NATIVE(nNullToInst) {
	(void)user;
	vm.pushInstance(nullptr);
}
NATIVE(nCpyInst) {
	(void)user;
	vm.pushInstance(popObject(vm));
}
NATIVE(nInstToPtr) { vm.pushInt(i32(MEM.objectAddress(popObject(vm)))); }
NATIVE(nAssignInst) {
	const u32 p = u32(vm.popInt());
	u32 sym = SCRIPT_NONE;
	popObject(vm, &sym);
	if (sym != SCRIPT_NONE) vm.setInstance(sym, MEM.objectAt(p));
}
NATIVE(nAssignInstNull) {
	(void)user;
	u32 sym = SCRIPT_NONE;
	popObject(vm, &sym);
	if (sym != SCRIPT_NONE) vm.setInstance(sym, nullptr);
}

// ---- functions ----

NATIVE(nGetFuncId) {
	(void)user;
	const u32 f = popFunc(vm);
	vm.pushInt(f == SCRIPT_NONE ? -1 : i32(f));
}
NATIVE(nGetFuncOffset) {
	(void)user;
	const u32 f = popFunc(vm);
	vm.pushInt(f == SCRIPT_NONE || vm.symbol(f).value == SCRIPT_NONE ? 0 : i32(vm.symbol(f).value));
}
NATIVE(nGetFuncPtr) {
	(void)user;
	const u32 f = popFunc(vm);
	vm.pushInt(f == SCRIPT_NONE || vm.symbol(f).value == SCRIPT_NONE ? 0 : i32(ScriptMemory::CODE_BASE + vm.symbol(f).value));
}
NATIVE(nGetFuncIdByOffset) {
	(void)user;
	const u32 f = vm.functionAtAddress(u32(vm.popInt()));
	vm.pushInt(f == SCRIPT_NONE ? -1 : i32(f));
}
NATIVE(nCallById) {
	(void)user;
	callSymbol(vm, u32(vm.popInt()));
}
NATIVE(nCallByPtr) {
	(void)user;
	const u32 p = u32(vm.popInt());
	callSymbol(vm, p >= ScriptMemory::CODE_BASE ? vm.functionAtAddress(p - ScriptMemory::CODE_BASE) : SCRIPT_NONE);
}
NATIVE(nCallByOffset) {
	(void)user;
	callSymbol(vm, vm.functionAtAddress(u32(vm.popInt())));
}
NATIVE(nCallByString) {
	(void)user;
	const Text s(vm.popString());
	char up[96];
	u32 n = 0;
	for (const char* p = s.c(); *p && n + 1 < sizeof(up); ++p) up[n++] = char(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
	up[n] = 0;
	callSymbol(vm, vm.find(up));
}
NATIVE(nCall) {
	(void)user;
	callSymbol(vm, popFunc(vm));
}
// Calls of a function replaced by another (MEM_ReplaceFunc) or hooked (LeGo's HookDaedalusFunc): the other one runs,
// with the arguments the caller pushed.
NATIVE(nReplaced) {
	const u32 by = IK.replacement(vm.callee());
	if (by != SCRIPT_NONE) vm.call(by);
}
NATIVE(nReplaceFunc) {
	const u32 by = popFunc(vm), replaced = popFunc(vm);
	if (replaced == SCRIPT_NONE || by == SCRIPT_NONE || replaced == by) return;
	IK.setReplacement(replaced, by);
	vm.overrideSymbol(replaced, nReplaced, user);
}
NATIVE(nHookDaedalus) {
	// (LeGo: the hooked function calls the hook; the hook's ContinueCall() runs what was there before.)
	const u32 hook = popFunc(vm), hooked = popFunc(vm);
	if (hooked == SCRIPT_NONE || hook == SCRIPT_NONE || hooked == hook) return;
	IK.setReplacement(hooked, hook);
	IK.setReplacement(hook | 0x80000000u, hooked); // (the way back, for ContinueCall)
	vm.overrideSymbol(hooked, nReplaced, user);
}
NATIVE(nHookDaedalusS) {
	const Text hookName(vm.popString()), hookedName(vm.popString());
	char a[96], b[96];
	u32 n = 0;
	for (const char* p = hookedName.c(); *p && n + 1 < sizeof(a); ++p) a[n++] = char(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
	a[n] = 0;
	n = 0;
	for (const char* p = hookName.c(); *p && n + 1 < sizeof(b); ++p) b[n++] = char(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
	b[n] = 0;
	vm.pushInt(i32(vm.find(a)));
	vm.pushInt(i32(vm.find(b)));
	nHookDaedalus(vm, user);
}
NATIVE(nContinueCall) {
	// The hook running now is the innermost frame's function.
	const u32 hook = vm.callee();
	const u32 hooked = hook == SCRIPT_NONE ? SCRIPT_NONE : IK.replacement(hook | 0x80000000u);
	if (hooked == SCRIPT_NONE) {
		IK.message("error", "ContinueCall outside of a hook");
		return;
	}
	vm.callOriginal(hooked);
}

// ---- the stack ----

NATIVE(nPushInt) {
	(void)user;
	vm.pushInt(vm.popInt());
}
NATIVE(nPushString) {
	(void)user;
	const Text s(vm.popString());
	vm.pushString(s.c());
}
NATIVE(nPushInst) {
	(void)user;
	ScriptValue v;
	vm.popValue(v);
	if (v.kind == ScriptValue::OBJECT) {
		vm.pushValue(v);
		return;
	}
	vm.pushValue(v);
	vm.pushInstance(popObject(vm));
}
NATIVE(nPopInstResult) {
	(void)user;
	ScriptValue v;
	if (!vm.popValue(v)) {
		vm.pushInstance(nullptr);
		return;
	}
	vm.pushValue(v);
	if (v.kind == ScriptValue::OBJECT) return;
	vm.pushInstance(popObject(vm));
}
NATIVE(nStackPushInst) {
	(void)user;
	const i32 v = vm.popInt();
	if (v >= 0 && u32(v) < vm.symbolCount() && vm.symbol(u32(v)).type == SCRIPT_INSTANCE)
		vm.pushInstance(vm.instanceOf(u32(v)));
	else
		vm.pushInt(v);
}
NATIVE(nStackPopInst) {
	(void)user;
	ScriptValue v;
	vm.popValue(v);
}
NATIVE(nStackPopInstAsInt) {
	(void)user;
	ScriptValue v;
	vm.popValue(v);
	if (v.kind == ScriptValue::VARIABLE)
		vm.pushInt(i32(v.symbol));
	else if (v.kind == ScriptValue::OBJECT)
		vm.pushInt(v.ctx && v.ctx->symbol != SCRIPT_NONE ? i32(v.ctx->symbol) : 0);
	else
		vm.pushInt(v.i);
}

// ---- symbols ----

NATIVE(nFindSymbol) {
	(void)user;
	const Text s(vm.popString());
	char up[96];
	u32 n = 0;
	for (const char* p = s.c(); *p && n + 1 < sizeof(up); ++p) up[n++] = char(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
	up[n] = 0;
	const u32 sym = vm.find(up);
	vm.pushInt(sym == SCRIPT_NONE ? -1 : i32(sym));
}

// ---- messages ----

NATIVE(nError) { IK.message("error", vm.popString()); }
NATIVE(nWarn) { IK.message("warning", vm.popString()); }
NATIVE(nInfo) { IK.message("info", vm.popString()); }
NATIVE(nToSpy) {
	const Text s(vm.popString());
	const i32 kind = vm.popInt();
	IK.message(kind >= 3 ? "error" : kind == 2 ? "warning" : "info", s.c());
}
NATIVE(nMessageBox) {
	(void)vm.popInt();
	(void)vm.popString();
	IK.message("info", vm.popString());
	vm.pushInt(1);
}
NATIVE(nNothing) {
	(void)vm;
	(void)user;
}
NATIVE(nOne) {
	(void)user;
	vm.pushInt(1);
}
NATIVE(nZero) {
	(void)user;
	vm.pushInt(0);
}
NATIVE(nEmptyString) {
	(void)user;
	vm.pushString("");
}
NATIVE(nSwitchG1G2) {
	const i32 g2 = vm.popInt(), g1 = vm.popInt();
	vm.pushInt(IK.gameVersion() == 1 ? g1 : g2);
}
NATIVE(nInitGlobals) {
	(void)vm;
	IK.initGlobals();
}
NATIVE(nTime) { vm.pushInt(i32(IK.timeMs())); }
NATIVE(nTimeMicro) { vm.pushInt(i32(IK.timeMs() * 1000u)); }

// ---- strings ----

NATIVE(nStrLen) {
	(void)user;
	vm.pushInt(i32(strlen(vm.popString())));
}
NATIVE(nStrCharAt) {
	const i32 k = vm.popInt();
	const char* s = vm.popString();
	const i32 n = i32(strlen(s));
	if (k < 0 || k >= n) {
		IK.message("warning", "STR_GetCharAt: reading out of bounds");
		vm.pushInt(0);
		return;
	}
	vm.pushInt(u8(s[k]));
}
NATIVE(nStrToChar) {
	ScriptValue v;
	vm.popValue(v);
	const u32 z = MEM.addressOf(v);
	vm.pushInt(z ? MEM.readInt(z + 8) : 0);
}
NATIVE(nStrFromChar) {
	char buf[1024];
	MEM.readCString(u32(vm.popInt()), buf, sizeof(buf));
	vm.pushString(buf);
}
NATIVE(nStrSubStr) {
	i32 count = vm.popInt();
	const i32 start = vm.popInt();
	const Text s(vm.popString());
	const i32 n = i32(strlen(s.c()));
	if (start < 0 || count < 0) {
		IK.message("error", "STR_SubStr: start and count may not be negative");
		vm.pushString("");
		return;
	}
	if (start > n) {
		IK.message("warning", "STR_SubStr: the start lies beyond the end of the string");
		vm.pushString("");
		return;
	}
	if (start + count > n) count = n - start;
	char* out = static_cast<char*>(malloc(size_t(count) + 1));
	if (!out) {
		vm.pushString("");
		return;
	}
	memcpy(out, s.c() + start, size_t(count));
	out[count] = 0;
	vm.pushString(out);
	free(out);
}
NATIVE(nStrPrefix) {
	const i32 len = vm.popInt();
	vm.pushInt(0);
	vm.pushInt(len);
	// (STR_Prefix(s, len) = STR_SubStr(s, 0, len); the string is below the two numbers)
	const i32 count = vm.popInt(), start = vm.popInt();
	const Text s(vm.popString());
	vm.pushString(s.c());
	vm.pushInt(start);
	vm.pushInt(count);
	nStrSubStr(vm, user);
}
NATIVE(nStrCompare) {
	(void)user;
	const Text b(vm.popString()), a(vm.popString());
	const size_t la = strlen(a.c()), lb = strlen(b.c());
	const int c = strncmp(a.c(), b.c(), la < lb ? la : lb);
	vm.pushInt(c < 0 ? -1 : c > 0 ? 1 : la > lb ? 1 : la < lb ? -1 : 0);
}
NATIVE(nStrToInt) {
	const Text s(vm.popString());
	i32 r = 0;
	bool minus = false;
	for (const char* p = s.c(); *p; ++p) {
		if (*p >= '0' && *p <= '9') {
			r = i32(u32(r) * 10u + u32(*p - '0'));
		} else if (p == s.c() && (*p == '+' || *p == '-')) {
			minus = *p == '-';
		} else {
			IK.message("warning", "STR_ToInt: cannot convert the string");
			vm.pushInt(0);
			return;
		}
	}
	vm.pushInt(minus ? -r : r);
}
NATIVE(nStrIndexOf) {
	(void)user;
	const Text tok(vm.popString()), s(vm.popString());
	if (!*tok.c()) {
		vm.pushInt(0);
		return;
	}
	const char* at = strstr(s.c(), tok.c());
	vm.pushInt(at ? i32(at - s.c()) : -1);
}
// The part `k` of `s` cut at the one-character separator; count: how many parts.
bool splitPart(const char* s, char sep, i32 k, const char*& from, u32& len, i32& count) {
	count = 1;
	for (const char* p = s; *p; ++p) count += *p == sep ? 1 : 0;
	if (k < 0 || k >= count) return false;
	from = s;
	for (i32 i = 0; i < k; ++i) from = strchr(from, sep) + 1;
	const char* end = strchr(from, sep);
	len = end ? u32(end - from) : u32(strlen(from));
	return true;
}
NATIVE(nStrSplit) {
	const i32 k = vm.popInt();
	const Text sep(vm.popString()), s(vm.popString());
	if (strlen(sep.c()) != 1) {
		IK.message("error", "STR_Split: the separator must be one character");
		vm.pushString("");
		return;
	}
	const char* from = nullptr;
	u32 len = 0;
	i32 count = 0;
	if (!splitPart(s.c(), sep.c()[0], k, from, len, count)) {
		IK.message("error", "STR_Split: the string does not have that many parts");
		vm.pushString("");
		return;
	}
	char* out = static_cast<char*>(malloc(len + 1));
	if (!out) {
		vm.pushString("");
		return;
	}
	memcpy(out, from, len);
	out[len] = 0;
	vm.pushString(out);
	free(out);
}
NATIVE(nStrSplitCount) {
	const Text sep(vm.popString()), s(vm.popString());
	if (strlen(sep.c()) != 1) {
		IK.message("error", "STR_SplitCount: the separator must be one character");
		vm.pushInt(0);
		return;
	}
	const char* from = nullptr;
	u32 len = 0;
	i32 count = 0;
	splitPart(s.c(), sep.c()[0], 0, from, len, count);
	vm.pushInt(count);
}
// The game's own case (zSTRING::Upper / Lower): the code page's letters, umlauts included.
void changeCase(ScriptVm& vm, bool upper) {
	const Text s(vm.popString());
	const size_t n = strlen(s.c());
	char* out = static_cast<char*>(malloc(n + 1));
	if (!out) {
		vm.pushString("");
		return;
	}
	for (size_t k = 0; k <= n; ++k) {
		u8 c = u8(s.c()[k]);
		if (upper) {
			if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) c = u8(c - 32);
		} else {
			if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) c = u8(c + 32);
		}
		out[k] = char(c);
	}
	vm.pushString(out);
	free(out);
}
NATIVE(nStrUpper) {
	(void)user;
	changeCase(vm, true);
}
NATIVE(nStrLower) {
	(void)user;
	changeCase(vm, false);
}

// ---- LeGo ----

NATIVE(nHookEngine) {
	// HookEngineF / HookEngineI (address, old instruction length, function): noted, not installed.
	const u32 f = popFunc(vm);
	(void)vm.popInt();
	IK.addHook(u32(vm.popInt()), f);
}
NATIVE(nHookEngineS) {
	const Text name(vm.popString());
	(void)vm.popInt();
	const u32 a = u32(vm.popInt());
	char up[96];
	u32 n = 0;
	for (const char* p = name.c(); *p && n + 1 < sizeof(up); ++p) up[n++] = char(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
	up[n] = 0;
	IK.addHook(a, vm.find(up));
}
NATIVE(nSbToString) {
	// LeGo's StringBuilder, read straight from its memory (class StringBuilder { ptr; cln; cal; }).
	const u32 sym = IK.sbCurrent();
	const u32 sb = sym != SCRIPT_NONE ? u32(vm.getInt(sym)) : 0u;
	const u32 ptr = sb ? u32(MEM.readInt(sb)) : 0u;
	const i32 len = sb ? MEM.readInt(sb + 4) : 0;
	if (!ptr || len <= 0) {
		vm.pushString("");
		return;
	}
	const u32 n = len > (1 << 20) ? (1u << 20) : u32(len);
	char* out = static_cast<char*>(malloc(n + 1));
	if (!out) {
		vm.pushString("");
		return;
	}
	for (u32 k = 0; k < n; ++k) out[k] = char(MEM.readByte(ptr + k));
	out[n] = 0;
	vm.pushString(out);
	free(out);
}

// ---- the parser's symbols ----

NATIVE(nSymbolByIndex) {
	const i32 i = vm.popInt();
	vm.pushInt(i >= 0 ? i32(MEM.symbolAddress(u32(i))) : 0);
}
NATIVE(nSymbolByName) {
	const Text s(vm.popString());
	char up[96];
	u32 n = 0;
	for (const char* p = s.c(); *p && n + 1 < sizeof(up); ++p) up[n++] = char(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
	up[n] = 0;
	const u32 sym = vm.find(up);
	vm.pushInt(sym == SCRIPT_NONE ? 0 : i32(MEM.symbolAddress(sym)));
}

// ---- files (LeGo's binary machines; WinAPI in the original) ----
// A handle is the file's number + 1, with the mode in bit 16 (1: writing); the reading position is the file's.

constexpr i32 GENERIC_WRITE = 0x40000000;
constexpr i32 CREATE_ALWAYS = 2;

NATIVE(nCreateFile) {
	(void)vm.popInt(); // template
	(void)vm.popInt(); // flags
	const i32 disposition = vm.popInt();
	(void)vm.popInt(); // security
	(void)vm.popInt(); // share
	const i32 access = vm.popInt();
	const Text name(vm.popString());
	const bool write = (access & GENERIC_WRITE) || disposition == CREATE_ALWAYS;
	Ikarus::File* f = IK.openFile(name.c(), write);
	if (!f) {
		vm.pushInt(-1);
		return;
	}
	if (write) f->size = 0;
	const u32 index = u32(f - &IK.file(0));
	vm.pushInt(i32((index + 1) | (write ? 0x10000u : 0u)));
}
Ikarus::File* fileOf(Ikarus& ik, i32 handle) {
	const u32 k = u32(handle & 0xFFFF);
	return k >= 1 && k <= ik.fileCount() ? const_cast<Ikarus::File*>(&ik.file(k - 1)) : nullptr;
}
NATIVE(nWriteFile) {
	(void)vm.popInt(); // overlapped
	const u32 written = u32(vm.popInt());
	const i32 n = vm.popInt();
	const u32 buf = u32(vm.popInt());
	Ikarus::File* f = fileOf(IK, vm.popInt());
	if (!f || n <= 0) return;
	if (f->size + u32(n) > f->cap) {
		const u32 cap = (f->size + u32(n)) * 2;
		u8* grown = static_cast<u8*>(realloc(f->data, cap));
		if (!grown) return;
		f->data = grown;
		f->cap = cap;
	}
	for (i32 k = 0; k < n; ++k) f->data[f->size + u32(k)] = MEM.readByte(buf + u32(k));
	f->size += u32(n);
	if (written) MEM.writeInt(written, n);
}
NATIVE(nReadFile) {
	(void)vm.popInt(); // overlapped
	const u32 read = u32(vm.popInt());
	const i32 n = vm.popInt();
	const u32 buf = u32(vm.popInt());
	Ikarus::File* f = fileOf(IK, vm.popInt());
	const u32 take = f && n > 0 ? (u32(n) < f->size ? u32(n) : f->size) : 0u;
	for (u32 k = 0; k < take; ++k) MEM.writeByte(buf + k, f->data[k]);
	if (read) MEM.writeInt(read, i32(take));
}
NATIVE(nCloseFile) {
	(void)vm.popInt();
	(void)user;
}
NATIVE(nFileSize) {
	(void)vm.popInt(); // high part
	Ikarus::File* f = fileOf(IK, vm.popInt());
	vm.pushInt(f ? i32(f->size) : 0);
}
NATIVE(nSaveFilePath) {
	const i32 slot = vm.popInt();
	char path[48];
	if (slot)
		snprintf(path, sizeof(path), "savegame%d/SCRPTSAVE.SAV", int(slot));
	else
		snprintf(path, sizeof(path), "quicksave/SCRPTSAVE.SAV");
	vm.pushString(path);
}
NATIVE(nSelectedSlot) { vm.pushInt(IK.slot()); }

} // namespace

// ---- set-up ----

bool Ikarus::bind(const char* name, ScriptVm::External fn, const char* sig, bool naked) {
	ScriptVm& vm = *_vm;
	const u32 sym = vm.find(name);
	if (sym == SCRIPT_NONE) return false;
	const ScriptSymbol& s = vm.symbol(sym);
	bool ok = s.type == SCRIPT_FUNC && (s.flags & SCRIPTF_CONST) && !(s.flags & SCRIPTF_EXTERNAL);
	if (ok && sig[0] != '*') {
		// "r:ppp" - r the result, p the parameters: v nothing, i int, s string, f float, F function, I an object.
		const auto type = [](char c) -> int {
			switch (c) {
			case 'v': return SCRIPT_VOID;
			case 'i': return SCRIPT_INT;
			case 's': return SCRIPT_STRING;
			case 'f': return SCRIPT_FLOAT;
			case 'F': return SCRIPT_FUNC;
			case 'I': return SCRIPT_INSTANCE;
			default: return -1;
			}
		};
		const bool returns = sig[0] != 'v';
		ok = returns == ((s.flags & SCRIPTF_RETURN) != 0) && (!returns || int(s.extra) == type(sig[0]) ||
		                                                       (sig[0] == 'I' && s.extra == SCRIPT_INSTANCE));
		const char* params = strchr(sig, ':');
		params = params ? params + 1 : "";
		ok = ok && s.count == strlen(params);
		for (u32 k = 0; ok && k < s.count; ++k) {
			const u8 t = vm.symbol(sym + 1 + k).type;
			const int want = type(params[k]);
			ok = int(t) == want || (params[k] == 'F' && t == SCRIPT_INT) || (params[k] == 'i' && t == SCRIPT_FUNC);
		}
	}
	if (!ok) {
		++_stats.refused;
		const size_t used = strlen(_refusedList);
		if (used + strlen(name) + 3 < sizeof(_refusedList)) snprintf(_refusedList + used, sizeof(_refusedList) - used, "%s%s", used ? ", " : "", name);
		return false;
	}
	if (!vm.overrideSymbol(sym, fn, this, naked)) return false;
	++_stats.bound;
	return true;
}

bool Ikarus::attach(ScriptVm& vm, u32 maxHeap) {
	detach();
	const bool ikarus = vm.find("IKARUS_VERSION") != SCRIPT_NONE && vm.find("MEM_INITALL") != SCRIPT_NONE &&
	                    vm.find("MEM_READINT") != SCRIPT_NONE && vm.find("MEM_WRITEINT") != SCRIPT_NONE;
	if (!ikarus) return false;
	if (!_mem.attach(vm, maxHeap)) return false;
	_vm = &vm;
	const u32 base = vm.find("GOTHIC_BASE_VERSION");
	if (base != SCRIPT_NONE && vm.symbol(base).type == SCRIPT_INT) _version = vm.getInt(base) == 1 ? 1 : 2;

	// Memory.
	bind("MEM_ALLOC", nAlloc, "i:i");
	bind("MEM_FREE", nFree, "v:i");
	bind("MEM_REALLOC", nRealloc, "i:iii");
	bind("MEM_READINT", nReadInt, "i:i");
	bind("MEM_WRITEINT", nWriteInt, "v:ii");
	bind("MEM_READBYTE", nReadByte, "i:i");
	bind("MEM_WRITEBYTE", nWriteByte, "v:ii");
	bind("MEM_READINTARRAY", nReadIntArray, "i:ii");
	bind("MEM_WRITEINTARRAY", nWriteIntArray, "v:iii");
	bind("MEM_READBYTEARRAY", nReadByteArray, "i:ii");
	bind("MEM_WRITEBYTEARRAY", nWriteByteArray, "v:iii");
	bind("MEM_COPYBYTES", nCopyBytes, "v:iii");
	bind("MEM_COPYWORDS", nCopyWords, "v:iii");
	bind("MEM_COPY", nCopyWords, "v:iii");
	bind("MEM_SWAPBYTES", nSwapBytes, "v:iii");
	bind("MEM_SWAPWORDS", nSwapWords, "v:iii");
	bind("MEM_SWAP", nSwapWords, "v:iii");
	bind("MEM_CLEAR", nClear, "v:ii");
	bind("MEM_COMPAREBYTES", nCompareBytes, "i:iii");
	bind("MEM_COMPAREWORDS", nCompareWords, "i:iii");
	bind("MEM_COMPARE", nCompareWords, "i:iii");
	bind("MEM_READSTRING", nReadString, "s:i");
	bind("MEM_WRITESTRING", nWriteString, "v:is");
	bind("MEM_READSTATARR", nReadStatArr, "i:ii");
	bind("MEM_WRITESTATARR", nWriteStatArr, "v:iii");
	bind("MEM_READSTATSTRINGARR", nReadStatStringArr, "s:si");
	bind("MEM_WRITESTATSTRINGARR", nWriteStatStringArr, "v:sis");
	bind("MEM_GETBUFFERCRC32", nCrc32, "i:ii");
	bind("MEM_GETSTRINGHASH", nStringHash, "i:s");
	// Addresses and objects.
	bind("_@", nAddressOf, "i:i");
	bind("_@F", nAddressOf, "i:f");
	bind("_@S", nAddressOf, "i:s");
	bind("MEM_GETINTADDRESS", nAddressOf, "i:i");
	bind("MEM_GETFLOATADDRESS", nAddressOf, "i:f");
	bind("MEM_GETSTRINGADDRESS", nAddressOf, "i:s");
	bind("STR_GETADDRESS", nAddressOf, "i:s");
	bind("_^", nPtrToInst, "I:i");
	bind("MEM_PTRTOINST", nPtrToInst, "I:i");
	bind("MEM_NULLTOINST", nNullToInst, "I:");
	bind("MEM_CPYINST", nCpyInst, "I:i");
	bind("MEM_INSTTOPTR", nInstToPtr, "i:i");
	bind("MEM_INSTGETOFFSET", nInstToPtr, "i:i");
	bind("MEM_ASSIGNINST", nAssignInst, "v:ii");
	bind("MEM_ASSIGNCONTENTINST", nAssignInst, "v:ii");
	bind("MEM_ASSIGNINSTNULL", nAssignInstNull, "v:i");
	bind("MEM_ASSIGNCONTENTINSTNULL", nAssignInstNull, "v:i");
	// Functions.
	bind("MEM_GETFUNCID", nGetFuncId, "i:F");
	bind("MEM_GETFUNCOFFSET", nGetFuncOffset, "i:F");
	bind("MEM_GETFUNCPTR", nGetFuncPtr, "i:F");
	bind("MEM_GETFUNCIDBYOFFSET", nGetFuncIdByOffset, "i:i");
	bind("MEM_REPLACEFUNC", nReplaceFunc, "v:FF");
	bind("MEM_CALLBYID", nCallById, "v:i", true);
	bind("MEM_CALLBYPTR", nCallByPtr, "v:i", true);
	bind("MEM_CALLBYOFFSET", nCallByOffset, "v:i", true);
	bind("MEM_CALLBYSTRING", nCallByString, "v:s", true);
	bind("MEM_CALL", nCall, "v:F", true);
	// The stack.
	bind("MEM_PUSHINTPARAM", nPushInt, "v:i", true);
	bind("MEM_PUSHSTRINGPARAM", nPushString, "v:s", true);
	bind("MEM_PUSHINSTPARAM", nPushInst, "v:i", true);
	bind("MEM_POPINTRESULT", nPushInt, "i:", true);
	bind("MEM_POPSTRINGRESULT", nPushString, "s:", true);
	bind("MEM_POPINSTRESULT", nPopInstResult, "I:", true);
	bind("MEMINT_STACKPOPINT", nPushInt, "i:", true);
	bind("MEMINT_POPINT", nPushInt, "i:", true);
	bind("MEMINT_STACKPOPSTRING", nPushString, "s:", true);
	bind("MEMINT_POPSTRING", nPushString, "s:", true);
	bind("MEMINT_STACKPUSHINST", nStackPushInst, "v:i", true);
	bind("MEMINT_STACKPOPINST", nStackPopInst, "v:", true);
	bind("MEMINT_STACKPOPINSTASINT", nStackPopInstAsInt, "i:", true);
	// Symbols.
	bind("MEM_FINDPARSERSYMBOL", nFindSymbol, "i:s");
	bind("MEM_GETSYMBOLINDEX", nFindSymbol, "i:s");
	// Messages.
	bind("MEM_ERROR", nError, "v:s");
	bind("MEM_ASSERTFAIL", nError, "v:s");
	bind("MEM_ERRORBOX", nError, "v:s");
	bind("MEM_WARN", nWarn, "v:s");
	bind("MEM_INFO", nInfo, "v:s");
	bind("MEM_DEBUG", nInfo, "v:s");
	bind("MEM_INFOBOX", nInfo, "v:s");
	bind("MEM_SENDTOSPY", nToSpy, "v:is");
	bind("MEMINT_HANDLEERROR", nToSpy, "v:is");
	bind("MEM_MESSAGEBOX", nMessageBox, "i:ssi");
	bind("MEM_PRINTSTACKTRACE", nNothing, "v:");
	// What finds the original's memory and code at the start: this engine's stand-ins instead.
	bind("MEM_CHECKVERSION", nOne, "i:iii");
	bind("MEMINT_SWITCHG1G2", nSwitchG1G2, "i:ii");
	bind("MEM_INITGLOBALINST", nInitGlobals, "v:");
	bind("MEM_REINITPARSER", nInitGlobals, "v:");
	bind("MEM_INITLABELS", nInitGlobals, "v:");
	static const char* const nothing[] = {"MEM_GETADDRESS_INIT", "MEMINT_GETADDRESS_INIT", "STR_GETADDRESSINIT", "MEM_INITSTATARRS",
	                                      "MEM_INITREPEAT", "MEMINT_SETUPEXCEPTIONHANDLER", "MEMINT_REPLACESLOWFUNCTIONS",
	                                      "MEMINT_REPLACELOGGINGFUNCTIONS", "MEMINT_REPORTVERSIONCHECK", "MEMINT_INITFASTERREADWRITE",
	                                      "MEMINT_INITFASTERPUSHINST", "ASMINT_INIT", "MEM_SETSHOWDEBUG", "MEM_APPLYGOTHOPT",
	                                      "MEM_SETGOTHOPT", "MEM_SETKEYS"};
	for (const char* n : nothing) bind(n, nNothing, "*");
	bind("MEM_GETSYSTEMTIME", nTime, "i:");
	bind("MEM_GETPERFORMANCECOUNTER", nTimeMicro, "i:");
	bind("MEM_GETCOMMANDLINE", nEmptyString, "s:");
	bind("MEM_GETGOTHOPT", nEmptyString, "*");
	bind("MEM_GETMODOPT", nEmptyString, "*");
	bind("MEM_GOTHOPTSECTIONEXISTS", nZero, "*");
	bind("MEM_GOTHOPTEXISTS", nZero, "*");
	bind("MEM_MODOPTSECTIONEXISTS", nZero, "*");
	bind("MEM_MODOPTEXISTS", nZero, "*");
	bind("MEM_KEYPRESSED", nZero, "*");
	bind("MEM_KEYSTATE", nZero, "*");
	// Strings.
	bind("STR_LEN", nStrLen, "i:s");
	bind("STR_GETCHARAT", nStrCharAt, "i:si");
	bind("STR_TOCHAR", nStrToChar, "i:s");
	bind("STRINT_TOCHAR", nStrToChar, "i:s");
	bind("STR_FROMCHAR", nStrFromChar, "s:i");
	bind("STR_SUBSTR", nStrSubStr, "s:sii");
	bind("STR_PREFIX", nStrPrefix, "s:si");
	bind("STR_COMPARE", nStrCompare, "i:ss");
	bind("STR_TOINT", nStrToInt, "i:s");
	bind("STR_INDEXOF", nStrIndexOf, "i:ss");
	bind("STR_SPLIT", nStrSplit, "s:ssi");
	bind("STR_SPLITCOUNT", nStrSplitCount, "i:ss");
	bind("STR_UPPER", nStrUpper, "s:s");
	bind("STR_LOWER", nStrLower, "s:s");
	// LeGo.
	bind("HOOKENGINEF", nHookEngine, "v:iiF");
	bind("HOOKENGINEI", nHookEngine, "v:iii");
	bind("HOOKENGINE", nHookEngineS, "v:iis");
	bind("HOOKENGINES", nHookEngineS, "v:iis");
	bind("ISHOOKED", nZero, "*");
	bind("HOOKDAEDALUSFUNC", nHookDaedalus, "v:FF");
	bind("HOOKDAEDALUSFUNCF", nHookDaedalus, "v:FF");
	bind("HOOKDAEDALUSFUNCI", nHookDaedalus, "v:ii");
	bind("HOOKDAEDALUSFUNCS", nHookDaedalusS, "v:ss");
	bind("CONTINUECALL", nContinueCall, "v:", true);
	_self = vm.selfSymbol();
	_other = vm.otherSymbol();
	_item = vm.find("ITEM");
	if (_item != SCRIPT_NONE && vm.symbol(_item).type != SCRIPT_INSTANCE) _item = SCRIPT_NONE;
	_sbCurrent = vm.find("_SB_CURRENT");
	if (_sbCurrent != SCRIPT_NONE && vm.symbol(_sbCurrent).type != SCRIPT_INT) _sbCurrent = SCRIPT_NONE;
	bind("SB_TOSTRING", nSbToString, "s:");
	// The parser's symbols.
	bind("MEM_GETSYMBOLBYINDEX", nSymbolByIndex, "i:i");
	bind("MEM_GETSYMBOL", nSymbolByName, "i:s");
	bind("MEM_GETPARSERSYMBOL", nSymbolByName, "i:s");
	// Files.
	bind("WIN_CREATEFILE", nCreateFile, "i:siiiiii");
	bind("WIN_WRITEFILE", nWriteFile, "v:iiiii");
	bind("WIN_READFILE", nReadFile, "v:iiiii");
	bind("WIN_CLOSEHANDLE", nCloseFile, "v:i");
	bind("WIN_GETFILESIZE", nFileSize, "i:ii");
	bind("WIN_GETLASTERROR", nZero, "i:");
	bind("_BIN_GETSAVEFILEPATH", nSaveFilePath, "s:i");
	bind("_BR_GETSELECTEDSLOT", nSelectedSlot, "i:");
	bind("_BR_SETSELECTEDSLOT", nNothing, "v:");
	initGlobals();
	return true;
}

void Ikarus::detach() {
	_mem.detach();
	_vm = nullptr;
	_stats = Stats();
	_noteCount = 0;
	_refusedList[0] = 0;
	_hookCount = 0;
	_replacedCount = 0;
	_timer = _game = _regs = 0;
	_renderAddress = 0;
	_esp = _ebp = _stepOff = SCRIPT_NONE;
	for (u32& t : _timerOff) t = SCRIPT_NONE;
	_time = 0;
	_slot = 0;
	for (u32 i = 0; i < _fileCount; ++i) free(_files[i].data);
	memset(_files, 0, sizeof(_files));
	_fileCount = 0;
	for (char*& c : _calls) {
		free(c);
		c = nullptr;
	}
	_callNext = 0;
	_self = _other = _item = SCRIPT_NONE;
}

void Ikarus::message(const char* kind, const char* text) {
	if (kind[0] == 'e')
		++_stats.errors;
	else if (kind[0] == 'w')
		++_stats.warnings;
	else
		++_stats.infos;
	if (kind[0] == 'i' || _noteCount >= MAX_NOTES) return;
	// (The function that complained is the innermost frame - a stand-in's own, or MEM_Error's -; its caller next.)
	u32 caller = SCRIPT_NONE;
	for (u32 d = 0; _vm && d < _vm->frameCount() && caller == SCRIPT_NONE; ++d) {
		const u32 f = _vm->frameFunction(d);
		if (f != SCRIPT_NONE && !_vm->isOverridden(f)) caller = f;
	}
	_noteCallers[_noteCount] = caller;
	snprintf(_notes[_noteCount++], sizeof(_notes[0]), "%s: %s", kind, text ? text : "");
}

void Ikarus::addHook(u32 address, u32 function) {
	++_stats.hooks;
	for (u32 i = 0; i < _hookCount; ++i)
		if (_hooks[i].address == address && _hooks[i].function == function) return;
	if (_hookCount < MAX_HOOKS) _hooks[_hookCount++] = {address, function};
}

void Ikarus::setReplacement(u32 replaced, u32 by) {
	for (u32 i = 0; i < _replacedCount; ++i)
		if (_replaced[i][0] == replaced) {
			_replaced[i][1] = by;
			return;
		}
	if (_replacedCount < 64) {
		_replaced[_replacedCount][0] = replaced;
		_replaced[_replacedCount][1] = by;
		++_replacedCount;
	}
}

u32 Ikarus::replacement(u32 replaced) const {
	for (u32 i = 0; i < _replacedCount; ++i)
		if (_replaced[i][0] == replaced) return _replaced[i][1];
	return SCRIPT_NONE;
}

// Ikarus' global objects (MEM_Game, MEM_Timer, ...): zeroed memory of their classes' size, so that the scripts read
// zeros and write harmlessly; a few fields the engine keeps up to date (frame()). MEM_StackPos is the position in the
// code (ScriptVm::setPcObject).
void Ikarus::initGlobals() {
	if (!_vm) return;
	ScriptVm& vm = *_vm;
	static const char* const globals[] = {"MEM_GAME", "MEM_WORLD", "MEM_TIMER", "MEM_WORLDTIMER", "MEM_VOBTREE", "MEM_INFOMAN",
	                                      "MEM_INFORMATIONMAN", "MEM_WAYNET", "MEM_CAMERA", "MEM_SKYCONTROLLER", "MEM_SPAWNMANAGER",
	                                      "MEM_GAMEMANANGER", "MEM_GAMEMANAGER", "MEM_PARSER"};
	for (const char* name : globals) {
		const u32 sym = vm.find(name);
		if (sym == SCRIPT_NONE || vm.symbol(sym).type != SCRIPT_INSTANCE) continue;
		ScriptInstance* have = vm.instanceOf(sym);
		if (have && (have->flags & SCRIPTI_VIEW) && have->size) continue; // (made before)
		const u32 cls = vm.classOf(sym);
		const u32 size = cls != SCRIPT_NONE ? vm.symbol(cls).value : 0u;
		u32 address = 0;
		ScriptInstance* obj = _mem.newObject(size ? size : 4u, &address);
		vm.setInstance(sym, obj);
		if (!strcmp(name, "MEM_TIMER")) _timer = address;
		if (!strcmp(name, "MEM_GAME")) _game = address;
	}
	// What frame() keeps up to date, found now.
	static const char* const timer[T_COUNT] = {"FACTORMOTION", "FRAMETIMEFLOAT", "TOTALTIMEFLOAT", "FRAMETIMEFLOATSECS",
	                                           "TOTALTIMEFLOATSECS", "FRAMETIME", "TOTALTIME"};
	for (u32 k = 0; k < T_COUNT; ++k) {
		const u32 m = _timer ? vm.findMember("ZCTIMER", timer[k]) : SCRIPT_NONE;
		_timerOff[k] = m != SCRIPT_NONE ? _timer + vm.symbol(m).value : SCRIPT_NONE;
	}
	const u32 step = _game ? vm.findMember("OCGAME", "TIMESTEP") : SCRIPT_NONE;
	_stepOff = step != SCRIPT_NONE ? _game + vm.symbol(step).value : SCRIPT_NONE;
	// A game that runs: its time step is not 0 (LeGo's Timer() stops while it is).
	if (_stepOff != SCRIPT_NONE) {
		const float minusOne = -1.0f;
		i32 bits;
		memcpy(&bits, &minusOne, 4);
		_mem.writeInt(_stepOff, bits);
	}
	_esp = vm.find("ESP");
	if (_esp != SCRIPT_NONE && (vm.symbol(_esp).type != SCRIPT_INT || (vm.symbol(_esp).flags & SCRIPTF_MEMBER))) _esp = SCRIPT_NONE;
	_ebp = vm.find("EBP");
	if (_ebp != SCRIPT_NONE && (vm.symbol(_ebp).type != SCRIPT_INT || (vm.symbol(_ebp).flags & SCRIPTF_MEMBER))) _ebp = SCRIPT_NONE;
	const u32 render = vm.find("OCGAME__RENDER");
	_renderAddress = render != SCRIPT_NONE && vm.symbol(render).type == SCRIPT_INT ? u32(vm.getInt(render)) : 0u;
	// Where Ikarus thinks the parser's tables and code are.
	const auto setGlobal = [&](const char* name, u32 v) {
		const u32 g = vm.find(name);
		if (g != SCRIPT_NONE && vm.symbol(g).type == SCRIPT_INT && !(vm.symbol(g).flags & SCRIPTF_MEMBER)) vm.setInt(g, 0, nullptr, i32(v));
	};
	setGlobal("CURRSYMBOLTABLEADDRESS", ScriptMemory::SYMT_BASE);
	setGlobal("CONTENTSYMBOLTABLEADDRESS", ScriptMemory::SYMT_BASE);
	setGlobal("CURRSYMBOLTABLELENGTH", vm.symbolCount());
	setGlobal("CURRPARSERSTACKADDRESS", ScriptMemory::CODE_BASE);
	setGlobal("CONTENTPARSERSTACKADDRESS", ScriptMemory::CODE_BASE);
	if (!_regs) _regs = _mem.alloc(256);
	const u32 pos = vm.find("MEM_STACKPOS");
	if (pos != SCRIPT_NONE && vm.symbol(pos).type == SCRIPT_INSTANCE) {
		ScriptInstance* obj = vm.instanceOf(pos);
		if (!obj || !(obj->flags & SCRIPTI_VIEW) || !obj->size) {
			obj = _mem.newObject(16);
			vm.setInstance(pos, obj);
		}
		vm.setPcObject(obj);
	}
}

void Ikarus::frame(float ms, bool paused) {
	if (!_vm) return;
	if (ms < 0) ms = 0;
	_time += ms;
	const auto setF = [&](u32 off, float v) {
		if (off == SCRIPT_NONE) return;
		i32 bits;
		memcpy(&bits, &v, 4);
		_mem.writeInt(off, bits);
	};
	if (_timer) {
		// zCTimer, by the names of the scripts' own class (found in initGlobals).
		const float frameMs = paused ? 0.0f : ms;
		setF(_timerOff[T_FACTOR], 1.0f);
		setF(_timerOff[T_FRAMEF], frameMs);
		setF(_timerOff[T_TOTALF], float(_time));
		setF(_timerOff[T_FRAMEFS], frameMs / 1000.0f);
		setF(_timerOff[T_TOTALFS], float(_time / 1000.0));
		if (_timerOff[T_FRAME] != SCRIPT_NONE) _mem.writeInt(_timerOff[T_FRAME], i32(frameMs));
		if (_timerOff[T_TOTAL] != SCRIPT_NONE) _mem.writeInt(_timerOff[T_TOTAL], i32(_time));
	}
	setF(_stepOff, paused ? 0.0f : -1.0f);
	if (!paused && _renderAddress && fireAt(_renderAddress, 0, 0, 0, 0)) ++_stats.frames;
}

u32 Ikarus::fire(const char* name, u32 espOffset, i32 espValue, u32 ebpOffset, i32 ebpValue) {
	if (!_vm || !_hookCount) return 0;
	const u32 c = _vm->find(name);
	if (c == SCRIPT_NONE || _vm->symbol(c).type != SCRIPT_INT) return 0;
	return fireAt(u32(_vm->getInt(c)), espOffset, espValue, ebpOffset, ebpValue);
}

u32 Ikarus::fireAt(u32 address, u32 espOffset, i32 espValue, u32 ebpOffset, i32 ebpValue) {
	if (!_vm || !_hookCount) return 0;
	ScriptVm& vm = *_vm;
	bool any = false;
	for (u32 i = 0; i < _hookCount && !any; ++i) any = _hooks[i].address == address;
	if (!any) return 0;
	// The registers the hooks may read: ESP and EBP point into a block of zeros with the values asked for.
	if (_regs) {
		_mem.fill(_regs, 0, 256);
		if (espOffset < 120) _mem.writeInt(_regs + espOffset, espValue);
		if (ebpOffset < 120) _mem.writeInt(_regs + 128 + ebpOffset, ebpValue);
		if (_esp != SCRIPT_NONE) vm.setInt(_esp, 0, nullptr, i32(_regs));
		if (_ebp != SCRIPT_NONE) vm.setInt(_ebp, 0, nullptr, i32(_regs + 128));
	}
	u32 ran = 0;
	for (u32 i = 0; i < _hookCount; ++i) {
		if (_hooks[i].address != address || _hooks[i].function == SCRIPT_NONE) continue;
		vm.call(_hooks[i].function);
		++ran;
	}
	return ran;
}

// ---- LeGo's AI_Function ----

namespace {

// LeGo's STR_Escape undone: "\\" a backslash, "\xHH" a byte, "\" + one of its table the character of that number.
void unescape(const char* in, char* out, u32 size) {
	static const char table[] = "0123456abtnvfr789ABCDEFGHIJKLMNO_"; // (LeGo's STR_Sequences: the characters 0 .. 32)
	u32 n = 0;
	for (const char* p = in; *p && n + 1 < size; ++p) {
		if (*p != '\\' || !p[1]) {
			out[n++] = *p;
			continue;
		}
		++p;
		if (*p == '\\') {
			out[n++] = '\\';
		} else if (*p == 'x' && p[1] && p[2]) {
			const auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0; };
			out[n++] = char(hex(p[1]) * 16 + hex(p[2]));
			p += 2;
		} else if (const char* at = strchr(table, *p)) {
			out[n++] = char(at - table);
		}
	}
	out[n] = 0;
}

} // namespace

i32 Ikarus::callLater(const char* ani) {
	const u32 k = _callNext % CALLS;
	++_callNext;
	free(_calls[k]);
	const size_t n = strlen(ani) + 1;
	_calls[k] = static_cast<char*>(malloc(n));
	if (_calls[k]) memcpy(_calls[k], ani, n);
	return i32(k);
}

bool Ikarus::callTicket(ScriptInstance* self, i32 ticket) {
	if (ticket < 0 || ticket >= CALLS || !_calls[ticket]) return false;
	char* text = _calls[ticket];
	_calls[ticket] = nullptr;
	const bool ok = callNow(self, text);
	free(text);
	return ok;
}

bool Ikarus::callNow(ScriptInstance* self, const char* ani) {
	if (!_vm || !isCall(ani)) return false;
	ScriptVm& vm = *_vm;
	// The words: the kinds of the arguments (I a number, N a character's instance, S an escaped string), the
	// arguments, the function's number last; or the function's number alone.
	char words[8][128];
	u32 count = 0;
	for (const char* p = ani + 5; *p && count < 8;) {
		while (*p == ' ') ++p;
		if (!*p) break;
		u32 n = 0;
		while (*p && *p != ' ') {
			if (n + 1 < sizeof(words[0])) words[count][n++] = *p;
			++p;
		}
		words[count++][n] = 0;
	}
	if (!count) return false;
	const char* kinds = words[0];
	const bool bare = kinds[0] >= '0' && kinds[0] <= '9';
	const u32 args = bare ? 0u : u32(strlen(kinds));
	if (args > 2 || count != (bare ? 1u : args + 2u)) return false;
	const u32 fn = u32(atoi(words[bare ? 0 : count - 1]));
	if (fn >= vm.symbolCount()) return false;
	// SELF the character; OTHER and ITEM none (as LeGo has it), given back afterwards.
	ScriptInstance* const selfBefore = _self != SCRIPT_NONE ? vm.instanceOf(_self) : nullptr;
	ScriptInstance* const otherBefore = _other != SCRIPT_NONE ? vm.instanceOf(_other) : nullptr;
	ScriptInstance* const itemBefore = _item != SCRIPT_NONE ? vm.instanceOf(_item) : nullptr;
	if (_self != SCRIPT_NONE) vm.setInstance(_self, self);
	if (_other != SCRIPT_NONE) vm.setInstance(_other, nullptr);
	if (_item != SCRIPT_NONE) vm.setInstance(_item, nullptr);
	for (u32 k = 0; k < args; ++k) {
		const char* w = words[1 + k];
		switch (kinds[k]) {
		case 'I': vm.pushInt(atoi(w)); break;
		case 'N': {
			const i32 sym = atoi(w);
			vm.pushInstance(sym >= 0 && u32(sym) < vm.symbolCount() ? vm.instanceOf(u32(sym)) : nullptr);
			break;
		}
		case 'S': {
			char text[128];
			unescape(w, text, sizeof(text));
			vm.pushString(text);
			break;
		}
		default: vm.pushInt(0); break;
		}
	}
	vm.call(fn);
	if (_self != SCRIPT_NONE) vm.setInstance(_self, selfBefore);
	if (_other != SCRIPT_NONE) vm.setInstance(_other, otherBefore);
	if (_item != SCRIPT_NONE) vm.setInstance(_item, itemBefore);
	return true;
}

Ikarus::File* Ikarus::openFile(const char* name, bool create) {
	for (u32 i = 0; i < _fileCount; ++i)
		if (!strcmp(_files[i].name, name)) return &_files[i];
	if (!create || _fileCount >= sizeof(_files) / sizeof(_files[0])) return nullptr;
	File& f = _files[_fileCount++];
	memset(&f, 0, sizeof(f));
	snprintf(f.name, sizeof(f.name), "%s", name);
	return &f;
}

void Ikarus::saveFiles(ByteSink& out) const {
	out.u32v(_fileCount);
	for (u32 i = 0; i < _fileCount; ++i) {
		out.str(_files[i].name);
		out.u32v(_files[i].size);
		out.put(_files[i].data, _files[i].size);
	}
}

bool Ikarus::loadFiles(ByteSource& in) {
	for (u32 i = 0; i < _fileCount; ++i) free(_files[i].data);
	memset(_files, 0, sizeof(_files));
	_fileCount = 0;
	const u32 n = in.u32v();
	for (u32 i = 0; i < n && !in.bad; ++i) {
		char name[96];
		const u32 len = in.u32v();
		if (in.bad || len >= sizeof(name) || len > in.left()) return false;
		in.get(name, len);
		name[len] = 0;
		const u32 size = in.u32v();
		if (in.bad || size > in.left()) return false;
		File* f = openFile(name, true);
		if (!f) return false;
		f->data = static_cast<u8*>(malloc(size ? size : 1));
		if (!f->data) return false;
		in.get(f->data, size);
		f->size = f->cap = size;
	}
	return !in.bad;
}

} // namespace g3
