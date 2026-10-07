// Base types shared by the runtime (3DS + host) and the tools.
// The runtime side is deliberately STL-free: C++17, no exceptions, no RTTI.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace g3 {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;

constexpr u32 fourcc(char a, char b, char c, char d) {
	return u32(u8(a)) | (u32(u8(b)) << 8) | (u32(u8(c)) << 16) | (u32(u8(d)) << 24);
}

// FNV-1a over the ASCII-lowercased name. All asset lookups go through this.
inline u32 hashName(const char* s) {
	u32 h = 2166136261u;
	for (; *s; ++s) {
		u8 c = u8(*s);
		if (c >= 'A' && c <= 'Z') c = u8(c + 32);
		if (c == '\\') c = '/';
		h = (h ^ c) * 16777619u;
	}
	return h;
}

void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

template <class T> inline T min(T a, T b) { return a < b ? a : b; }
template <class T> inline T max(T a, T b) { return a > b ? a : b; }
template <class T> inline T clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace g3
