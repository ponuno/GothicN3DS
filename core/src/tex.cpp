#include "g3ds/tex.h"

namespace g3 {

bool texValidate(const TexHeader& h, u32 availableBytes) {
	auto pot = [](u32 v) { return v >= TEX_MIN_SIZE && v <= TEX_MAX_SIZE && (v & (v - 1)) == 0; };
	if (h.magic != TEX_MAGIC || !pot(h.width) || !pot(h.height)) return false;
	if (h.format != TEX_ETC1 && h.format != TEX_ETC1A4) return false;
	if (h.mipCount == 0 || h.mipCount > texMaxMipCount(h.width, h.height)) return false;
	return texLevelOffset(h, h.mipCount) == h.dataSize && h.dataSize <= availableBytes;
}

static const u8 kModifiers[8][2] = {
    {2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183},
};

static inline u8 clamp8(int v) { return u8(v < 0 ? 0 : (v > 255 ? 255 : v)); }

void etc1DecodeBlock(const u8 b[8], u8 out[64]) {
	int base[2][3];
	const bool diff = (b[3] & 2) != 0;
	const bool flip = (b[3] & 1) != 0;

	if (diff) {
		for (int c = 0; c < 3; ++c) {
			int v = b[c] >> 3;
			int d = b[c] & 7;
			if (d >= 4) d -= 8;
			int v2 = v + d;
			base[0][c] = (v << 3) | (v >> 2);
			base[1][c] = ((v2 & 31) << 3) | ((v2 & 31) >> 2);
		}
	} else {
		for (int c = 0; c < 3; ++c) {
			int hi = b[c] >> 4, lo = b[c] & 15;
			base[0][c] = hi * 17;
			base[1][c] = lo * 17;
		}
	}

	const int table[2] = {(b[3] >> 5) & 7, (b[3] >> 2) & 7};
	const u32 bits = (u32(b[4]) << 24) | (u32(b[5]) << 16) | (u32(b[6]) << 8) | u32(b[7]);

	for (int x = 0; x < 4; ++x) {
		for (int y = 0; y < 4; ++y) {
			const int texel = x * 4 + y; // ETC1 numbers texels column-major
			const int sub = flip ? (y >= 2) : (x >= 2);
			const int large = (bits >> texel) & 1;
			const int negative = (bits >> (texel + 16)) & 1;
			int mod = kModifiers[table[sub]][large];
			if (negative) mod = -mod;
			u8* px = out + (y * 4 + x) * 4;
			px[0] = clamp8(base[sub][0] + mod);
			px[1] = clamp8(base[sub][1] + mod);
			px[2] = clamp8(base[sub][2] + mod);
			px[3] = 255;
		}
	}
}

void texSolid(u8 r, u8 g, u8 b, TexHeader& header, u8 data[32]) {
	header = TexHeader {};
	header.magic = TEX_MAGIC;
	header.width = header.height = 8;
	header.format = TEX_ETC1;
	header.mipCount = 1;
	header.flags = 0;
	header.dataSize = 32;
	// Individual mode, table 0, every texel "+2": the base is the colour less 2, in 4 bits (the nearest of 17 * n).
	auto nibble = [](u8 c) {
		const int v = (int(c) - 2 + 8) / 17;
		return u8(v < 0 ? 0 : (v > 15 ? 15 : v));
	};
	u8 block[8] = {u8((nibble(r) << 4) | nibble(r)), u8((nibble(g) << 4) | nibble(g)), u8((nibble(b) << 4) | nibble(b)), 0, 0, 0, 0, 0};
	for (int k = 0; k < 4; ++k)
		for (int i = 0; i < 8; ++i) data[k * 8 + i] = block[7 - i]; // (the stored block has its bytes reversed)
}

void texDecodeLevel(const TexHeader& h, const u8* data, u32 level, u8* out) {
	const u32 w = h.width >> level, hh = h.height >> level;
	const u32 blockBytes = texBytesPerBlock(h.format);
	const bool hasAlpha = h.format == TEX_ETC1A4;
	const u8* src = data + texLevelOffset(h, level);

	for (u32 ty = 0; ty < hh / 8; ++ty) {
		for (u32 tx = 0; tx < w / 8; ++tx) {
			for (u32 sub = 0; sub < 4; ++sub, src += blockBytes) {
				const u8* color = src + (hasAlpha ? 8 : 0);
				u8 be[8];
				for (int i = 0; i < 8; ++i) be[i] = color[7 - i];
				u8 px[64];
				etc1DecodeBlock(be, px);

				const u32 bx = tx * 8 + (sub & 1) * 4;
				const u32 by = ty * 8 + (sub >> 1) * 4; // in stored (bottom-up) rows
				for (u32 y = 0; y < 4; ++y) {
					for (u32 x = 0; x < 4; ++x) {
						u8* dst = out + ((hh - 1 - (by + y)) * w + bx + x) * 4;
						const u8* p = px + (y * 4 + x) * 4;
						dst[0] = p[0];
						dst[1] = p[1];
						dst[2] = p[2];
						if (hasAlpha) {
							const u32 n = x * 4 + y;
							const u8 a4 = (src[n / 2] >> ((n & 1) * 4)) & 15;
							dst[3] = u8(a4 * 17);
						} else {
							dst[3] = 255;
						}
					}
				}
			}
		}
	}
}

} // namespace g3
