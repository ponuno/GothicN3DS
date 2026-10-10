#include "ui.h"

#include "icons.h"
#include "ui_font.h"

#include <stdlib.h>
#include <string.h>

namespace g3 {

u32 utf8Next(const char*& s) {
	const u8* p = reinterpret_cast<const u8*>(s);
	if (!*p) return 0;
	u32 c = *p++;
	int more = 0;
	if (c >= 0xF0) {
		c &= 0x07;
		more = 3;
	} else if (c >= 0xE0) {
		c &= 0x0F;
		more = 2;
	} else if (c >= 0xC0) {
		c &= 0x1F;
		more = 1;
	} else if (c >= 0x80) {
		c = '?'; // a stray continuation byte
	}
	for (; more > 0; --more) {
		if ((*p & 0xC0) != 0x80) {
			c = '?';
			break;
		}
		c = (c << 6) | (*p++ & 0x3F);
	}
	s = reinterpret_cast<const char*>(p);
	return c;
}

namespace {

const uifont::Glyph* glyphOf(u32 code, int size) {
	int lo = 0, hi = uifont::GLYPHS;
	while (lo < hi) {
		const int mid = (lo + hi) / 2;
		const uifont::Glyph& g = uifont::kGlyphs[mid];
		if (g.size < size || (g.size == size && g.code < code))
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo < uifont::GLYPHS && uifont::kGlyphs[lo].size == size && uifont::kGlyphs[lo].code == code) return &uifont::kGlyphs[lo];
	return nullptr;
}

const uifont::Glyph* glyphOrMark(u32 code, int size) {
	if (code == 0x401) code = 0x415; // (should the atlas lack Ё, Е will do)
	const uifont::Glyph* g = glyphOf(code, size);
	return g ? g : glyphOf('?', size);
}

} // namespace

namespace ui {

bool glyph(u32 code, int size, GlyphBox& out) {
	const uifont::Glyph* g = glyphOrMark(code, size == TITLE ? 1 : 0);
	if (!g) return false;
	out.x = g->x;
	out.y = g->y;
	out.w = g->w;
	out.h = g->h;
	out.left = g->left;
	out.top = g->top;
	out.advance = g->advance;
	return true;
}

int coverage(int x, int y) {
	if (x < 0 || y < 0 || x >= uifont::ATLAS_W || y >= uifont::ATLAS_H) return 0;
	const int i = y * uifont::ATLAS_W + x;
	const u8 b = uifont::kAtlas[i / 2];
	return (i & 1) ? (b >> 4) : (b & 15);
}

} // namespace ui

namespace {

// True if `s` is UTF-8 through and through (and has something beyond ASCII).
bool validUtf8(const char* s) {
	const u8* p = reinterpret_cast<const u8*>(s);
	while (*p) {
		if (*p < 0x80) {
			++p;
			continue;
		}
		const int more = (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : -1;
		if (more < 0) return false;
		for (int k = 1; k <= more; ++k)
			if ((p[k] & 0xC0) != 0x80) return false;
		p += more + 1;
	}
	return true;
}

// Writes code point `c` as UTF-8 if it fits.
void put(u32 c, char* out, u32 size, u32& n) {
	char buf[4];
	u32 len = 0;
	if (c < 0x80) buf[len++] = char(c);
	else if (c < 0x800) {
		buf[len++] = char(0xC0 | (c >> 6));
		buf[len++] = char(0x80 | (c & 0x3F));
	} else {
		buf[len++] = char(0xE0 | (c >> 12));
		buf[len++] = char(0x80 | ((c >> 6) & 0x3F));
		buf[len++] = char(0x80 | (c & 0x3F));
	}
	if (n + len >= size) return;
	memcpy(out + n, buf, len);
	n += len;
}

} // namespace

void scriptText(const char* in, char* out, u32 size) {
	if (!out || size == 0) return;
	out[0] = 0;
	if (!in) return;
	u32 n = 0;
	if (validUtf8(in)) {
		for (; in[n] && n + 1 < size; ++n) out[n] = in[n];
		// (Not in the middle of a character.)
		while (n > 0 && (u8(out[n]) & 0xC0) == 0x80 && in[n]) --n;
		out[n] = 0;
		return;
	}
	// Russian: nearly every letter in the upper half; German: a few of them.
	u32 letters = 0, upper = 0;
	for (const u8* p = reinterpret_cast<const u8*>(in); *p; ++p) {
		const bool latin = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z');
		letters += latin || *p >= 0xC0;
		upper += *p >= 0xC0;
	}
	const bool cyrillic = upper >= 2 && upper * 2 >= letters;
	// 1252 above 0x80 where it differs from Latin-1, and what the font has of it.
	static const char* const kFolded = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
	for (const u8* p = reinterpret_cast<const u8*>(in); *p; ++p) {
		const u8 c = *p;
		if (c < 0x80) put(c, out, size, n);
		else if (cyrillic && c >= 0xC0) put(0x410 + (c - 0xC0), out, size, n);
		else if (cyrillic && c == 0xA8) put(0x401, out, size, n);
		else if (cyrillic && c == 0xB8) put(0x451, out, size, n);
		else if (c == 0x96 || c == 0x97) put(c == 0x96 ? 0x2013 : 0x2014, out, size, n);
		else if (c == 0x85) put(0x2026, out, size, n);
		else if (c == 0xAB || c == 0xBB) put(c, out, size, n); // « »
		else if (c == 0x93 || c == 0x94 || c == 0x84) put('"', out, size, n);
		else if (c == 0x91 || c == 0x92) put('\'', out, size, n);
		else if (c == 0xDF && !cyrillic) {
			put('s', out, size, n);
			put('s', out, size, n);
		} else if (c >= 0xC0 && !cyrillic) {
			put(u8(kFolded[c - 0xC0]), out, size, n);
			if (c == 0xC4 || c == 0xD6 || c == 0xDC || c == 0xE4 || c == 0xF6 || c == 0xFC) put('e', out, size, n); // ä -> ae
		} else put('?', out, size, n);
	}
	out[n] = 0;
}

namespace {

// Under the font, the inventory's pictures (icons.h) twice: their shapes
// (drawn dark, as an outline) and their shading (drawn in the item's colour).
constexpr int ICONS_Y = uifont::ATLAS_H, ICONS_PER_ROW = uifont::ATLAS_W / icons::SIZE;
constexpr int ICON_ROWS = (icons::COUNT + ICONS_PER_ROW - 1) / ICONS_PER_ROW;
constexpr int SHADING_Y = ICONS_Y + ICON_ROWS * icons::SIZE;
constexpr int TEXTURE_H = uifont::ATLAS_H * 2;
static_assert(SHADING_Y + ICON_ROWS * icons::SIZE <= TEXTURE_H, "the icons fit under the font");

u8 shadeOf(char role) {
	switch (role) {
	case 'x': return 150;
	case 'o': return 215;
	case '+': case 'w': case 'p': case 'g': return 255;
	case 'm': return 235;
	case 'M': return 150;
	case 'b': return 175;
	case 'B': return 120;
	case 'G': case 'r': case 'P': return 190;
	case 'k': return 70;
	default: return 0;
	}
}

} // namespace

bool Ui::init() {
	release();
	const int W = uifont::ATLAS_W;
	u8* coverage = static_cast<u8*>(calloc(size_t(W) * TEXTURE_H, 1));
	if (!coverage) return false;
	for (int i = 0; i < uifont::ATLAS_W * uifont::ATLAS_H / 2; ++i) {
		coverage[i * 2] = u8((uifont::kAtlas[i] & 15) * 17);
		coverage[i * 2 + 1] = u8((uifont::kAtlas[i] >> 4) * 17);
	}
	for (int k = 0; k < icons::COUNT; ++k) {
		const int x0 = (k % ICONS_PER_ROW) * icons::SIZE, y0 = (k / ICONS_PER_ROW) * icons::SIZE;
		for (int y = 0; y < icons::SIZE; ++y)
			for (int x = 0; x < icons::SIZE; ++x) {
				const char role = icons::kPixels[k][y][x];
				coverage[(ICONS_Y + y0 + y) * W + x0 + x] = role == '.' ? 0 : 255;
				coverage[(SHADING_Y + y0 + y) * W + x0 + x] = shadeOf(role);
			}
	}
	_font = gfx::uiTextureCreate(u16(W), u16(TEXTURE_H), coverage);
	free(coverage);
	_count = 0;
	return _font != nullptr;
}

void Ui::release() {
	if (_font) gfx::textureDestroy(_font);
	_font = nullptr;
	_count = 0;
}

int Ui::lineHeight(int size) { return uifont::LINE[size == ui::TITLE ? 1 : 0]; }

void Ui::quad(int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1, u32 rgba) {
	if (x1 <= x0 || y1 <= y0 || x1 <= 0 || y1 <= 0 || x0 >= gfx::SCREEN_W || y0 >= gfx::SCREEN_H) return;
	if (_count >= gfx::UI_MAX_QUADS) {
		++_dropped;
		return;
	}
	gfx::UiQuad& q = _quads[_count++];
	q.x0 = i16(x0);
	q.y0 = i16(y0);
	q.x1 = i16(x1);
	q.y1 = i16(y1);
	q.u0 = u16(u0);
	q.v0 = u16(v0);
	q.u1 = u16(u1);
	q.v1 = u16(v1);
	q.rgba[0] = u8(rgba >> 24);
	q.rgba[1] = u8(rgba >> 16);
	q.rgba[2] = u8(rgba >> 8);
	q.rgba[3] = u8(rgba);
}

int Ui::measure(const char* s, int size) const {
	size = size == ui::TITLE ? 1 : 0;
	int w = 0;
	for (u32 c; s && (c = utf8Next(s)) != 0;)
		if (const uifont::Glyph* g = glyphOrMark(c, size)) w += g->advance;
	return w;
}

int Ui::text(int x, int y, const char* s, u32 rgba, int size, int align, bool shadow) {
	if (!s) return 0;
	size = size == ui::TITLE ? 1 : 0;
	const int width = measure(s, size);
	if (align == ui::CENTER) x -= width / 2;
	else if (align == ui::RIGHT) x -= width;
	const u32 shade = 0x00000000u | (u32(u8(rgba)) * 3 / 4);
	for (int pass = shadow ? 0 : 1; pass < 2; ++pass) {
		int pen = x + (pass == 0 ? 1 : 0);
		const int top = y + (pass == 0 ? 1 : 0);
		for (const char* p = s; const u32 c = utf8Next(p);) {
			const uifont::Glyph* g = glyphOrMark(c, size);
			if (!g) continue;
			if (g->w)
				quad(pen + g->left, top + g->top, pen + g->left + g->w, top + g->top + g->h, g->x, g->y, g->x + g->w, g->y + g->h,
				     pass == 0 ? shade : rgba);
			pen += g->advance;
		}
	}
	return width;
}

void Ui::icon(int x, int y, u8 which, u32 tint, int scale, u8 alpha) {
	if (which >= icons::COUNT) return;
	if (!tint) tint = 0xB4B4B4FFu;
	const int u = (which % ICONS_PER_ROW) * icons::SIZE, v = (which / ICONS_PER_ROW) * icons::SIZE, n = icons::SIZE * scale;
	quad(x, y, x + n, y + n, u, ICONS_Y + v, u + icons::SIZE, ICONS_Y + v + icons::SIZE, 0x14100C00u | u32(alpha * 7 / 8));
	quad(x, y, x + n, y + n, u, SHADING_Y + v, u + icons::SIZE, SHADING_Y + v + icons::SIZE, (tint & ~0xFFu) | alpha);
}

void Ui::rect(int x0, int y0, int x1, int y1, u32 rgba) {
	quad(x0, y0, x1, y1, uifont::SOLID_X - 2, uifont::SOLID_Y - 2, uifont::SOLID_X + 2, uifont::SOLID_Y + 2, rgba);
}

void Ui::frame(int x0, int y0, int x1, int y1, u32 rgba) {
	rect(x0, y0, x1, y0 + 1, rgba);
	rect(x0, y1 - 1, x1, y1, rgba);
	rect(x0, y0 + 1, x0 + 1, y1 - 1, rgba);
	rect(x1 - 1, y0 + 1, x1, y1 - 1, rgba);
}

void Ui::bar(int x0, int y0, int x1, int y1, float share, u32 fill, u32 back) {
	share = clamp(share, 0.0f, 1.0f);
	const int split = x0 + int(float(x1 - x0) * share + 0.5f);
	rect(x0, y0, x1, y1, back);
	rect(x0, y0, split, y1, fill);
}

void Ui::flush() {
	if (_font && _count) gfx::uiDraw(_font, _quads, _count);
	_count = 0;
}

} // namespace g3
