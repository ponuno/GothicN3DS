#include "texture_bake.h"

#include "g3ds/tex.h"
#include "rg_etc1.h"

#include <atomic>
#include <thread>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace g3 {

namespace {

struct Image {
	int w = 0, h = 0;
	std::vector<u8> px; // RGBA, top-down
};

int nearestPot(int v) {
	int p = 1;
	while (p * 2 <= v) p *= 2;
	// Round up only when clearly closer to the next power.
	return (v - p > p * 2 - v) ? p * 2 : p;
}

// Area-average resample. Colour is weighted by alpha so transparent texels do
// not darken the edges of cut-out textures.
Image resample(const Image& src, int dw, int dh) {
	Image dst;
	dst.w = dw;
	dst.h = dh;
	dst.px.resize(size_t(dw) * dh * 4);
	for (int y = 0; y < dh; ++y) {
		int sy0 = y * src.h / dh;
		int sy1 = std::max(sy0 + 1, (y + 1) * src.h / dh);
		for (int x = 0; x < dw; ++x) {
			int sx0 = x * src.w / dw;
			int sx1 = std::max(sx0 + 1, (x + 1) * src.w / dw);
			u64 r = 0, g = 0, b = 0, a = 0, n = 0;
			for (int sy = sy0; sy < sy1; ++sy) {
				for (int sx = sx0; sx < sx1; ++sx) {
					const u8* p = &src.px[(size_t(sy) * src.w + sx) * 4];
					r += u64(p[0]) * p[3];
					g += u64(p[1]) * p[3];
					b += u64(p[2]) * p[3];
					a += p[3];
					++n;
				}
			}
			u8* o = &dst.px[(size_t(y) * dw + x) * 4];
			if (a) {
				o[0] = u8((r + a / 2) / a);
				o[1] = u8((g + a / 2) / a);
				o[2] = u8((b + a / 2) / a);
			} else {
				o[0] = o[1] = o[2] = 0;
			}
			o[3] = u8((a + n / 2) / n);
		}
	}
	return dst;
}

// Spreads opaque colour into fully transparent texels, so bilinear filtering
// across a cut-out edge does not pull in black.
void bleedColor(Image& img) {
	std::vector<u8> known(size_t(img.w) * img.h);
	for (size_t i = 0; i < known.size(); ++i) known[i] = img.px[i * 4 + 3] > 0;
	for (int pass = 0; pass < 8; ++pass) {
		std::vector<u8> next = known;
		bool any = false;
		for (int y = 0; y < img.h; ++y) {
			for (int x = 0; x < img.w; ++x) {
				size_t i = size_t(y) * img.w + x;
				if (known[i]) continue;
				int r = 0, g = 0, b = 0, n = 0;
				static const int off[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
				for (auto& o : off) {
					// Textures tile, so wrap around the borders.
					int nx = (x + o[0] + img.w) % img.w, ny = (y + o[1] + img.h) % img.h;
					size_t j = size_t(ny) * img.w + nx;
					if (!known[j]) continue;
					r += img.px[j * 4 + 0];
					g += img.px[j * 4 + 1];
					b += img.px[j * 4 + 2];
					++n;
				}
				if (!n) continue;
				img.px[i * 4 + 0] = u8(r / n);
				img.px[i * 4 + 1] = u8(g / n);
				img.px[i * 4 + 2] = u8(b / n);
				next[i] = 1;
				any = true;
			}
		}
		known.swap(next);
		if (!any) break;
	}
}

// Appends one mip level in PICA layout (see g3ds/tex.h). The tile rows are independent, so with `threads` > 1 they are
// shared out; the bytes are the same either way.
void encodeLevel(const Image& img, bool alpha, rg_etc1::etc1_pack_params& params, std::vector<u8>& out, unsigned threads) {
	const int rows = img.h / 8, cols = img.w / 8;
	const size_t rowBytes = size_t(cols) * 4 * (alpha ? 16 : 8);
	const size_t start = out.size();
	out.resize(start + rowBytes * size_t(rows));
	const auto doRow = [&](int ty) {
		u8* o = out.data() + start + rowBytes * size_t(ty);
		for (int tx = 0; tx < cols; ++tx) {
			for (int sub = 0; sub < 4; ++sub) {
				const int bx = tx * 8 + (sub & 1) * 4;
				const int by = ty * 8 + (sub >> 1) * 4; // stored (bottom-up) rows

				unsigned int block[16];
				u8 alpha4[16];
				for (int y = 0; y < 4; ++y) {
					const int srcY = img.h - 1 - (by + y);
					for (int x = 0; x < 4; ++x) {
						const u8* p = &img.px[(size_t(srcY) * img.w + bx + x) * 4];
						// rg_etc1 wants R,G,B,A byte order in memory with A = 255.
						u8 q[4] = {p[0], p[1], p[2], 255};
						std::memcpy(&block[y * 4 + x], q, 4);
						alpha4[x * 4 + y] = u8((p[3] * 15 + 127) / 255);
					}
				}

				if (alpha) {
					for (int i = 0; i < 8; ++i) *o++ = u8(alpha4[i * 2] | (alpha4[i * 2 + 1] << 4));
				}
				u8 etc[8];
				rg_etc1::pack_etc1_block(etc, block, params);
				for (int i = 7; i >= 0; --i) *o++ = etc[i];
			}
		}
	};
	// Small levels are not worth the threads.
	if (threads <= 1 || rows < 4 || img.w * img.h < 16384) {
		for (int ty = 0; ty < rows; ++ty) doRow(ty);
		return;
	}
	const unsigned n = std::min<unsigned>(threads, unsigned(rows));
	std::atomic<int> next(0);
	const auto work = [&] {
		for (int ty = next.fetch_add(1); ty < rows; ty = next.fetch_add(1)) doRow(ty);
	};
	std::vector<std::thread> pool;
	for (unsigned i = 1; i < n; ++i) pool.emplace_back(work);
	work();
	for (std::thread& t : pool) t.join();
}

} // namespace

std::vector<u8> bakeTexture(const SrcImage& src, const TexBakeOptions& opt, TexBakeInfo* info) {
	static std::once_flag once;
	std::call_once(once, [] { rg_etc1::pack_etc1_block_init(); });

	Image img;
	img.w = src.width;
	img.h = src.height;
	img.px = src.rgba;

	int w = nearestPot(src.width), h = nearestPot(src.height);
	while (std::max(w, h) > opt.maxSize) {
		w /= 2;
		h /= 2;
	}
	w = std::clamp(w, int(TEX_MIN_SIZE), int(TEX_MAX_SIZE));
	h = std::clamp(h, int(TEX_MIN_SIZE), int(TEX_MAX_SIZE));
	if (w != img.w || h != img.h) img = resample(img, w, h);

	// Cut-out detection: a meaningful share of clearly transparent texels.
	size_t transparent = 0;
	for (size_t i = 3; i < img.px.size(); i += 4) transparent += img.px[i] < 128;
	const bool alpha = transparent * 500 > img.px.size() / 4; // > 0.2 %

	rg_etc1::etc1_pack_params params;
	params.m_quality = opt.quality <= 0   ? rg_etc1::cLowQuality
	                   : opt.quality == 1 ? rg_etc1::cMediumQuality
	                                      : rg_etc1::cHighQuality;
	params.m_dithering = false;

	TexHeader hdr {};
	hdr.magic = TEX_MAGIC;
	hdr.width = u16(w);
	hdr.height = u16(h);
	hdr.format = alpha ? TEX_ETC1A4 : TEX_ETC1;
	hdr.mipCount = u8(opt.mipmaps ? texMaxMipCount(u32(w), u32(h)) : 1);
	hdr.flags = alpha ? u16(TEXF_ALPHA_TEST) : u16(0);

	std::vector<u8> data;
	Image level = img;
	for (u32 l = 0; l < hdr.mipCount; ++l) {
		if (l) level = resample(level, level.w / 2, level.h / 2);
		Image enc = level;
		if (alpha) bleedColor(enc);
		encodeLevel(enc, alpha, params, data, opt.threads);
	}
	hdr.dataSize = u32(data.size());

	if (info) {
		info->width = w;
		info->height = h;
		info->srcWidth = src.width;
		info->srcHeight = src.height;
		info->alpha = alpha;
		info->mipCount = hdr.mipCount;
	}

	std::vector<u8> out(sizeof(TexHeader) + data.size());
	std::memcpy(out.data(), &hdr, sizeof(hdr));
	std::memcpy(out.data() + sizeof(hdr), data.data(), data.size());
	return out;
}

} // namespace g3
