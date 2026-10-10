#include "ui.h"

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

bool Ui::init() {
	release();
	u8* coverage = static_cast<u8*>(malloc(size_t(uifont::ATLAS_W) * uifont::ATLAS_H));
	if (!coverage) return false;
	for (int i = 0; i < uifont::ATLAS_W * uifont::ATLAS_H / 2; ++i) {
		coverage[i * 2] = u8((uifont::kAtlas[i] & 15) * 17);
		coverage[i * 2 + 1] = u8((uifont::kAtlas[i] >> 4) * 17);
	}
	_font = gfx::uiTextureCreate(u16(uifont::ATLAS_W), u16(uifont::ATLAS_H), coverage);
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
