// A growing buffer to write a saved state into, and a bounds-checked reader
// of one. Nothing here reads past its end: a damaged file makes `bad` true
// and every further read give zeros.
#pragma once
#include "g3ds/base.h"

#include <stdlib.h>
#include <string.h>

namespace g3 {

struct ByteSink {
	u8* data = nullptr;
	size_t size = 0, cap = 0;
	bool failed = false;

	ByteSink() = default;
	ByteSink(const ByteSink&) = delete;
	ByteSink& operator=(const ByteSink&) = delete;
	~ByteSink() { free(data); }

	void put(const void* p, size_t n) {
		if (failed || n == 0) return;
		if (size + n > cap) {
			size_t to = cap ? cap * 2 : 4096;
			while (to < size + n) to *= 2;
			u8* grown = static_cast<u8*>(realloc(data, to));
			if (!grown) {
				failed = true;
				return;
			}
			data = grown;
			cap = to;
		}
		memcpy(data + size, p, n);
		size += n;
	}
	void u8v(u8 v) { put(&v, 1); }
	void u16v(u16 v) { put(&v, 2); }
	void u32v(u32 v) { put(&v, 4); }
	void i32v(i32 v) { put(&v, 4); }
	void f32v(float v) { put(&v, 4); }
	// A text: its length, then its bytes.
	void str(const char* s) {
		const u32 n = s ? u32(strlen(s)) : 0u;
		u32v(n);
		put(s, n);
	}
	// A block of bytes with its length in front.
	void block(const ByteSink& b) {
		u32v(u32(b.size));
		put(b.data, b.size);
	}
};

struct ByteSource {
	const u8* data;
	size_t size, pos = 0;
	bool bad = false;

	ByteSource(const u8* d, size_t n) : data(d), size(n) {}

	bool get(void* out, size_t n) {
		if (bad || n > size - pos) {
			bad = true;
			if (out) memset(out, 0, n);
			return false;
		}
		if (out) memcpy(out, data + pos, n);
		pos += n;
		return true;
	}
	u8 u8v() {
		u8 v = 0;
		get(&v, 1);
		return v;
	}
	u16 u16v() {
		u16 v = 0;
		get(&v, 2);
		return v;
	}
	u32 u32v() {
		u32 v = 0;
		get(&v, 4);
		return v;
	}
	i32 i32v() {
		i32 v = 0;
		get(&v, 4);
		return v;
	}
	float f32v() {
		float v = 0;
		get(&v, 4);
		return v;
	}
	// Reads a text into `out` (cut to fit); false if the file is damaged.
	bool str(char* out, size_t cap) {
		const u32 n = u32v();
		if (bad || n > size - pos) {
			bad = true;
			if (cap) out[0] = 0;
			return false;
		}
		const size_t keep = n < cap - 1 ? n : cap - 1;
		memcpy(out, data + pos, keep);
		out[keep] = 0;
		pos += n;
		return true;
	}
	// A block written by ByteSink::block: a reader over it.
	ByteSource block() {
		const u32 n = u32v();
		if (bad || n > size - pos) {
			bad = true;
			return ByteSource(data, 0);
		}
		ByteSource inner(data + pos, n);
		pos += n;
		return inner;
	}
	size_t left() const { return size - pos; }
};

// A sum over bytes to tell a damaged section from a whole one.
inline u32 byteCheck(const u8* p, size_t n) {
	u32 sum = 0x9E3779B9u;
	for (size_t i = 0; i < n; ++i) sum = (sum ^ p[i]) * 16777619u;
	return sum;
}

} // namespace g3
