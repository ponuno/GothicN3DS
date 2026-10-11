#include "font_bake.h"

#include "codepages.h"
#include "g3ds/font.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace g3 {

namespace {

struct Cut {
	u32 code;
	int size;
	int w, h, advance;
	std::vector<u8> cov; // w * h
	int x = 0, y = 0;
};

// How much of each texel of the sheet is ink: its alpha, or - for a sheet
// without any (all opaque) - the brightest of its colours.
std::vector<u8> inkOf(const SrcImage& img) {
	const size_t n = size_t(img.width) * size_t(img.height);
	std::vector<u8> ink(n);
	bool hasAlpha = false;
	for (size_t i = 0; i < n && !hasAlpha; ++i) hasAlpha = img.rgba[i * 4 + 3] != 255;
	for (size_t i = 0; i < n; ++i) {
		const u8* p = &img.rgba[i * 4];
		ink[i] = hasAlpha ? p[3] : std::max(p[0], std::max(p[1], p[2]));
	}
	return ink;
}

} // namespace

int detectCodePage(const std::vector<std::string>& texts) {
	size_t letters = 0, cyr = 0, polish = 0;
	for (const std::string& t : texts)
		for (unsigned char c : t) {
			if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0xC0) ++letters;
			if (c >= 0xC0) ++cyr;
			if (c == 0xB3 || c == 0xB9 || c == 0x9C || c == 0x9F || c == 0xBF || c == 0xEA || c == 0xE6 || c == 0xF1) ++polish;
		}
	if (letters >= 100 && cyr * 2 >= letters) return 1251; // (a Russian text is nearly all upper half)
	if (letters >= 100 && polish * 25 >= letters) return 1250;
	return 1252;
}

bool bakeFont(const SrcFonts& fonts, std::vector<u8>& out, std::string& report, std::string& why) {
	std::vector<Cut> cuts;
	int line[2] = {0, 0}, ascent[2] = {0, 0};
	for (int s = 0; s < 2; ++s) {
		const SrcFontFace& f = fonts.face[s];
		const SrcImage& img = f.sheet;
		if (f.cells.size() < 256 || img.width <= 0 || img.height <= 0 || img.rgba.size() != size_t(img.width) * img.height * 4 || f.height <= 0) {
			why = "the font '" + f.name + "' is incomplete";
			return false;
		}
		const std::vector<u8> ink = inkOf(img);
		line[s] = f.height;
		ascent[s] = (f.height * 4 + 2) / 5;
		for (u32 b = 0x20; b < 0x100; ++b) {
			if (b == 0x7F) continue;
			const u32 code = codePageToUnicode(u32(fonts.codepage), b);
			if (!code) continue;
			const SrcFontFace::Cell& c = f.cells[b];
			Cut cut;
			cut.code = code;
			cut.size = s;
			cut.advance = std::max(c.width, 0);
			const int x0 = int(std::floor(c.u0 * float(img.width) + 0.5f)), y0 = int(std::floor(c.v0 * float(img.height) + 0.5f));
			int x1 = int(std::floor(c.u1 * float(img.width) + 0.5f)), y1 = int(std::floor(c.v1 * float(img.height) + 0.5f));
			// (A cell whose corners lie outside the sheet is a glyph the font does not have.)
			const bool valid = x0 >= 0 && y0 >= 0 && x1 > x0 && y1 > y0 && x1 <= img.width && y1 <= img.height;
			if (valid) {
				x1 = std::min(x1, x0 + 255);
				y1 = std::min(y1, y0 + 255);
				cut.w = x1 - x0;
				cut.h = y1 - y0;
				cut.cov.resize(size_t(cut.w) * cut.h);
				bool any = false;
				for (int y = 0; y < cut.h; ++y)
					for (int x = 0; x < cut.w; ++x) {
						const u8 v = ink[size_t(y0 + y) * img.width + x0 + x];
						cut.cov[size_t(y) * cut.w + x] = v;
						any = any || v > 8;
					}
				if (!any) {
					cut.w = cut.h = 0;
					cut.cov.clear();
				}
			} else {
				cut.w = cut.h = 0;
			}
			if (!cut.advance) cut.advance = cut.w;
			if (b == 0x20 && !cut.advance) cut.advance = std::max(f.height / 3, 2);
			if (!cut.w && !cut.advance) continue; // nothing to draw, nothing to step over
			cuts.push_back(std::move(cut));
		}
	}
	if (cuts.empty()) {
		why = "the fonts have no glyphs";
		return false;
	}
	// Shelf packing: tallest first, a pixel between neighbours.
	std::vector<size_t> order;
	for (size_t i = 0; i < cuts.size(); ++i)
		if (cuts[i].w) order.push_back(i);
	std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return cuts[a].h > cuts[b].h; });
	int x = 1, y = 1, rowH = 0;
	for (size_t i : order) {
		Cut& c = cuts[i];
		if (x + c.w + 1 > int(FONT_ATLAS_W)) {
			x = 1;
			y += rowH + 1;
			rowH = 0;
		}
		c.x = x;
		c.y = y;
		x += c.w + 1;
		rowH = std::max(rowH, c.h);
	}
	const int usedH = y + rowH + 1;
	const int atlasH = (usedH + 7) & ~7;
	if (atlasH > int(FONT_ATLAS_MAX_H)) {
		why = "the glyphs of the fonts do not fit in " + std::to_string(FONT_ATLAS_W) + "x" + std::to_string(FONT_ATLAS_MAX_H) + " (need " +
		      std::to_string(atlasH) + " rows)";
		return false;
	}
	std::sort(cuts.begin(), cuts.end(), [](const Cut& a, const Cut& b) { return a.size != b.size ? a.size < b.size : a.code < b.code; });
	// (Two bytes can lead to one character; the first stays.)
	cuts.erase(std::unique(cuts.begin(), cuts.end(), [](const Cut& a, const Cut& b) { return a.size == b.size && a.code == b.code; }), cuts.end());

	FontHeader h {};
	h.magic = FONT_MAGIC;
	h.version = FONT_VERSION;
	h.atlasW = u16(FONT_ATLAS_W);
	h.atlasH = u16(atlasH);
	h.glyphCount = u16(cuts.size());
	h.codepage = u16(fonts.codepage);
	for (int s = 0; s < 2; ++s) {
		h.line[s] = u8(std::min(line[s], 255));
		h.ascent[s] = u8(std::min(ascent[s], 255));
	}
	out.assign(sizeof(h) + cuts.size() * sizeof(FontGlyphRec) + size_t(FONT_ATLAS_W) * atlasH, 0);
	std::memcpy(out.data(), &h, sizeof(h));
	u8* atlas = out.data() + sizeof(h) + cuts.size() * sizeof(FontGlyphRec);
	for (size_t i = 0; i < cuts.size(); ++i) {
		const Cut& c = cuts[i];
		FontGlyphRec r {};
		r.code = u16(c.code);
		r.x = u16(c.x);
		r.y = u16(c.y);
		r.size = u8(c.size);
		r.w = u8(c.w);
		r.h = u8(c.h);
		r.left = 0;
		r.top = 0;
		r.advance = u8(std::min(c.advance, 255));
		std::memcpy(out.data() + sizeof(h) + i * sizeof(r), &r, sizeof(r));
		for (int yy = 0; yy < c.h; ++yy)
			std::memcpy(atlas + size_t(c.y + yy) * FONT_ATLAS_W + c.x, &c.cov[size_t(yy) * c.w], size_t(c.w));
	}
	char buf[256];
	std::snprintf(buf, sizeof(buf), "font: %s (%d px) and %s (%d px), code page %d, %zu glyphs, atlas %ux%d, %.0f KB", fonts.face[0].name.c_str(),
	              line[0], fonts.face[1].name.c_str(), line[1], fonts.codepage, cuts.size(), unsigned(FONT_ATLAS_W), atlasH,
	              double(out.size()) / 1024.0);
	report = buf;
	return true;
}

} // namespace g3
