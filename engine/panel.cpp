#include "lang.h"
#include "panel.h"

#include "g3ds/script.h"
#include "icons.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace g3 {

namespace {

// The colours of Gothic Classic's console inventory.
constexpr u32 BG_TOP = 0x181B1EFFu, BG_BOTTOM = 0x0E1012FFu;
constexpr u32 CELL_FILL = 0x0B0D1090u;     // a cell, darker than the ground
constexpr u32 CELL_LINE = 0x7C8086B0u;     // its thin grey edge
constexpr u32 SEL_TOP = 0x426E9EFFu, SEL_BOTTOM = 0x1A3354FFu, SEL_EDGE = 0xE8ECF0FFu;
constexpr u32 TAB_TOP = 0x22385AFFu, TAB_BOTTOM = 0x14223AFFu, TAB_LIGHT = 0x5A7CA6FFu, TAB_DARK = 0x080E18FFu;
constexpr u32 TAB_ON = 0x37587EFFu;
constexpr u32 PILL = 0xC4C8CCFFu, PILL_TEXT = 0x1C2024FFu;
constexpr u32 TEXT = 0xD8D6D0FFu, TEXT_DIM = 0x9C9A92FFu, TITLE = 0xEAE6DAFFu, WARN = 0xE2604EFFu, GOLD = 0xE8C77AFFu;
constexpr u32 HP = 0xB0261EFFu, HP_LIGHT = 0xD8584AFFu, MANA = 0x2A4CBCFFu, MANA_LIGHT = 0x5A7CE8FFu;

// The quick slots' keys, as on the PC's keyboard.
const char* const kKeys[QUICK_SLOTS] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
const char* const kPageTitles[] = {"", "Инвентарь", "Дневник", "Персонаж", "Сундук", "Замок", "Разговор"};

// The signs of the tabs, in the order of ItemTab.
const u8 kTabIcons[ITEM_TABS] = {ICON_ALL, ICON_WEAPONS, ICON_ARMOR, ICON_MAGIC, ICON_POTION, ICON_APPLE, ICON_OTHER};
const char* const kTabNames[ITEM_TABS] = {"Всё", "Оружие", "Снаряжение", "Магия", "Зелья", "Еда", "Прочее"};
u8 channel(u32 rgba, int shift) { return u8(rgba >> shift); }

u32 lerp(u32 a, u32 b, int t /*0..256*/) {
	u32 out = 0;
	for (int s = 0; s < 32; s += 8) {
		const int ca = channel(a, s), cb = channel(b, s);
		out |= u32(ca + ((cb - ca) * t >> 8)) << s;
	}
	return out;
}

// The roles of icons.h as colours (`tint` for the item's own).
u32 roleColour(char role, u32 tint) {
	switch (role) {
	case '#': return 0x1A1410FFu;
	case 'x': return lerp(tint, 0x000000FFu, 110);
	case 'o': return tint;
	case '+': return lerp(tint, 0xFFFFFFFFu, 115);
	case 'w': return 0xF5F0E1FFu;
	case 'm': return 0xC4C8D0FFu;
	case 'M': return 0x707480FFu;
	case 'b': return 0x96623AFFu;
	case 'B': return 0x5C3A1CFFu;
	case 'g': return 0xE8C77AFFu;
	case 'G': return 0x60A848FFu;
	case 'r': return 0xC83C32FFu;
	case 'p': return 0xE8DCBEFFu;
	case 'P': return 0xB0A080FFu;
	case 'k': return 0x28201AFFu;
	default: return 0;
	}
}

} // namespace

// ---- Canvas ----

bool Canvas::init() {
	if (_px) return true;
	_px = static_cast<u16*>(malloc(sizeof(u16) * W * H));
	if (_px) memset(_px, 0, sizeof(u16) * W * H);
	unclip();
	return _px != nullptr;
}

void Canvas::release() {
	free(_px);
	_px = nullptr;
}

void Canvas::clip(int x0, int y0, int x1, int y1) {
	_cx0 = clamp(x0, 0, W);
	_cy0 = clamp(y0, 0, H);
	_cx1 = clamp(x1, _cx0, W);
	_cy1 = clamp(y1, _cy0, H);
}

void Canvas::blend(int x, int y, u32 rgba, int alpha) {
	if (!_px || x < _cx0 || y < _cy0 || x >= _cx1 || y >= _cy1 || alpha <= 0) return;
	u16& d = _px[y * W + x];
	if (alpha >= 255) {
		d = rgb565(rgba);
		return;
	}
	const int r0 = (d >> 11) << 3, g0 = ((d >> 5) & 63) << 2, b0 = (d & 31) << 3;
	const int r = r0 + ((int(channel(rgba, 24)) - r0) * alpha >> 8);
	const int g = g0 + ((int(channel(rgba, 16)) - g0) * alpha >> 8);
	const int b = b0 + ((int(channel(rgba, 8)) - b0) * alpha >> 8);
	d = u16(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void Canvas::clear(u32 rgba) {
	if (!_px) return;
	const u16 c = rgb565(rgba);
	for (int i = 0; i < W * H; ++i) _px[i] = c;
}

void Canvas::fill(int x0, int y0, int x1, int y1, u32 rgba) {
	const int a = channel(rgba, 0);
	x0 = max(x0, _cx0);
	y0 = max(y0, _cy0);
	x1 = min(x1, _cx1);
	y1 = min(y1, _cy1);
	if (!_px || x1 <= x0 || y1 <= y0 || a == 0) return;
	if (a == 255) {
		const u16 c = rgb565(rgba);
		for (int y = y0; y < y1; ++y)
			for (int x = x0; x < x1; ++x) _px[y * W + x] = c;
		return;
	}
	for (int y = y0; y < y1; ++y)
		for (int x = x0; x < x1; ++x) blend(x, y, rgba, a);
}

void Canvas::gradient(int x0, int y0, int x1, int y1, u32 top, u32 bottom) {
	const int h = max(y1 - y0 - 1, 1);
	for (int y = y0; y < y1; ++y) fill(x0, y, x1, y + 1, lerp(top, bottom, (y - y0) * 256 / h));
}

void Canvas::frame(int x0, int y0, int x1, int y1, u32 rgba, int width) {
	fill(x0, y0, x1, y0 + width, rgba);
	fill(x0, y1 - width, x1, y1, rgba);
	fill(x0, y0 + width, x0 + width, y1 - width, rgba);
	fill(x1 - width, y0 + width, x1, y1 - width, rgba);
}

void Canvas::disc(int cx, int cy, int r, u32 rgba) {
	const int a = channel(rgba, 0);
	for (int y = -r; y <= r; ++y)
		for (int x = -r; x <= r; ++x) {
			// A soft edge: the share of the pixel inside, roughly.
			const int d2 = x * x + y * y, in = r * r, out = (r + 1) * (r + 1);
			if (d2 <= in) blend(cx + x, cy + y, rgba, a);
			else if (d2 < out) blend(cx + x, cy + y, rgba, a * (out - d2) / (out - in));
		}
}

int Canvas::measure(const char* s, int size) {
	int w = 0;
	ui::GlyphBox g;
	for (u32 c; s && (c = utf8Next(s)) != 0;)
		if (ui::glyph(c, size, g)) w += g.advance;
	return w;
}

int Canvas::text(int x, int y, const char* s, u32 rgba, int size, int align, int maxWidth, bool shadow) {
	if (!s) return 0;
	ui::GlyphBox dots;
	const bool haveDots = ui::glyph(0x2026, size, dots);
	int width = measure(s, size);
	const bool cut = maxWidth > 0 && width > maxWidth;
	if (cut) width = maxWidth;
	if (align == ui::CENTER) x -= width / 2;
	else if (align == ui::RIGHT) x -= width;
	const int a = channel(rgba, 0);
	for (int pass = shadow ? 0 : 1; pass < 2; ++pass) {
		const u32 colour = pass == 0 ? 0x000000FFu : rgba;
		const int alpha = pass == 0 ? a * 3 / 4 : a;
		int pen = x + (pass == 0 ? 1 : 0);
		const int top = y + (pass == 0 ? 1 : 0);
		const auto glyphAt = [&](const ui::GlyphBox& g) {
			for (int gy = 0; gy < g.h; ++gy)
				for (int gx = 0; gx < g.w; ++gx) {
					const int cov = ui::coverage(g.x + gx, g.y + gy);
					if (cov) blend(pen + g.left + gx, top + g.top + gy, colour, alpha * cov / 15);
				}
		};
		for (const char* p = s; const u32 c = utf8Next(p);) {
			ui::GlyphBox g;
			if (!ui::glyph(c, size, g)) continue;
			if (cut && pen - x + g.advance + (haveDots ? dots.advance : 0) > maxWidth) {
				if (haveDots) glyphAt(dots);
				break;
			}
			glyphAt(g);
			pen += g.advance;
		}
	}
	return width;
}

void Canvas::icon(int x, int y, u8 which, u32 tint, int scale, int mode) {
	if (which >= icons::COUNT) return;
	if (!tint) tint = 0xB4B4B4FFu;
	const int alpha = mode == ICON_MONO ? channel(tint, 0) : 255;
	for (int py = 0; py < icons::SIZE; ++py)
		for (int px = 0; px < icons::SIZE; ++px) {
			const char role = icons::kPixels[which][py][px];
			if (role == '.') continue;
			u32 c;
			if (mode == ICON_MONO) {
				if (role == '#') continue;
				c = tint;
			} else {
				c = roleColour(role, tint | 0xFF);
				if (mode == ICON_DIM) {
					const int l = (channel(c, 24) * 3 + channel(c, 16) * 5 + channel(c, 8) * 2) / 20;
					c = (u32(l) << 24) | (u32(l) << 16) | (u32(l) << 8) | 0xFF;
				}
			}
			for (int sy = 0; sy < scale; ++sy)
				for (int sx = 0; sx < scale; ++sx) blend(x + px * scale + sx, y + py * scale + sy, c, alpha);
		}
}

// ---- pictures ----

void Canvas::picture(int x, int y, int w, int h, const u16* rgb, const u8* alpha, int pw, int ph, bool dim) {
	if (!rgb || !alpha || pw <= 0 || ph <= 0) return;
	// Scaled to fit, its shape kept, in the middle.
	const int s256 = min(w * 256 / pw, h * 256 / ph);
	const int dw = pw * s256 / 256, dh = ph * s256 / 256;
	const int ox = x + (w - dw) / 2, oy = y + (h - dh) / 2;
	for (int dy = 0; dy < dh; ++dy) {
		const int sy = dy * ph / dh;
		for (int dx = 0; dx < dw; ++dx) {
			const int sx = dx * pw / dw;
			const int a = alpha[sy * pw + sx];
			if (!a) continue;
			const u16 c = rgb[sy * pw + sx];
			u32 r = u32((c >> 11) << 3), g = u32(((c >> 5) & 63) << 2), b = u32((c & 31) << 3);
			if (dim) r = g = b = (r * 3 + g * 5 + b * 2) / 20;
			blend(ox + dx, oy + dy, (r << 24) | (g << 16) | (b << 8) | 0xFF, a);
		}
	}
}

int Canvas::paragraph(int x, int y, int width, const char* s, u32 rgba, int maxLines) {
	if (!s) return 0;
	const int line = Ui::lineHeight(ui::TEXT);
	int lines = 0;
	char buf[256];
	while (*s && (maxLines <= 0 || lines < maxLines)) {
		while (*s == ' ') ++s;
		// As many words as fit.
		size_t n = 0, lastBreak = 0;
		const char* p = s;
		while (*p && *p != '\n') {
			const char* q = p;
			utf8Next(q);
			const size_t len = size_t(q - s);
			if (len >= sizeof(buf)) break;
			memcpy(buf, s, len);
			buf[len] = 0;
			if (measure(buf) > width) break;
			n = len;
			if (*q == ' ' || *q == 0 || *q == '\n') lastBreak = len;
			p = q;
		}
		if (*p && *p != '\n' && lastBreak) n = lastBreak; // (a word that does not fit goes to the next line)
		if (n == 0) n = size_t(p - s) ? size_t(p - s) : 1;
		memcpy(buf, s, n);
		buf[n] = 0;
		text(x, y + lines * line, buf, rgba, ui::TEXT, ui::LEFT, 0, true);
		++lines;
		s += n;
		if (*s == '\n') ++s;
	}
	return lines * line;
}

int ItemPictures::find(u16 visual) const {
	for (u32 i = 0; i < _count; ++i)
		if (_pics[i].visual == visual) return int(i);
	return -1;
}

int ItemPictures::add(u16 visual, int w, int h, const u8* rgba) {
	if (_count >= CAPACITY || w <= 0 || h <= 0 || w > 256 || h > 256 || !rgba) return -1;
	ItemPicture& p = _pics[_count];
	p.rgb = static_cast<u16*>(malloc(sizeof(u16) * size_t(w * h)));
	p.alpha = static_cast<u8*>(malloc(size_t(w * h)));
	if (!p.rgb || !p.alpha) {
		free(p.rgb);
		free(p.alpha);
		p = ItemPicture();
		return -1;
	}
	for (int i = 0; i < w * h; ++i) {
		const u8* c = rgba + size_t(i) * 4;
		p.rgb[i] = u16(((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3));
		p.alpha[i] = c[3];
	}
	p.visual = visual;
	p.w = u16(w);
	p.h = u16(h);
	return int(_count++);
}

void ItemPictures::release() {
	for (u32 i = 0; i < _count; ++i) {
		free(_pics[i].rgb);
		free(_pics[i].alpha);
		_pics[i] = ItemPicture();
	}
	_count = 0;
}

// ---- the panel ----

void InventoryPanel::openPage(PanelPage p) {
	if (p >= PANEL_PAGES || p == _page) return;
	_page = p;
	_assigning = false;
	_inspect = p == PAGE_INVENTORY || p == PAGE_CHEST ? kInspectTime : 0.0f;
	if (p == PAGE_CHEST) {
		_side = 0;
		_scroll = 0;
		_selected = 0xFFFFFFFFu;
		_filteredVersion = 0xFFFFFFFFu;
	}
	_topic = -1;
	_listScroll = 0;
	if (p == PAGE_DIALOG) {
		_talkCursor = 0;
		_talkVersion = 0xFFFFFFFFu;
	}
	_dirty = true;
}

void InventoryPanel::setSide(int side) {
	side = side ? 1 : 0;
	if (side == _side) return;
	_side = u8(side);
	_scroll = 0;
	_selected = 0xFFFFFFFFu;
	_inspect = kInspectTime;
	_filteredVersion = 0xFFFFFFFFu;
	_dirty = true;
}

void InventoryPanel::stripCentre(int k, int& x, int& y) {
	x = 2 + k * 107 + 52;
	y = TABS_Y + TABS_H / 2;
}

void InventoryPanel::lockCentre(int k, int& x, int& y) {
	x = k ? 240 : 80;
	y = 168;
}

void InventoryPanel::setTab(ItemTab t) {
	if (t >= ITEM_TABS || t == _tab) return;
	_tab = u8(t);
	_scroll = 0;
	_dirty = true;
}

void InventoryPanel::refilter(const InventoryView& v) {
	if (_filteredVersion == v.version && _filteredTab == _tab && _filteredPage == _page && _filteredSide == _side) return;
	_filteredVersion = v.version;
	_filteredTab = _tab;
	_filteredPage = _page;
	_filteredSide = _side;
	_shownCount = 0;
	const ItemEntry* list = items(v);
	const u32 total = itemCount(v);
	for (u32 i = 0; i < total && _shownCount < INVENTORY_MAX; ++i)
		if ((_page == PAGE_CHEST && !(v.chest.trade && (list[i].flags & ITEMF_NOSELL))) || (_page != PAGE_CHEST && (_tab == TAB_ALL || tabOf(list[i].kind) == _tab)))
			_shown[_shownCount++] = i;
	_scroll = clamp(_scroll, 0, maxScroll());
	_dirty = true;
}

const ItemEntry* InventoryPanel::entryOf(const InventoryView& v, u32 symbol) const {
	const ItemEntry* list = items(v);
	const u32 total = itemCount(v);
	for (u32 i = 0; i < total; ++i)
		if (list[i].symbol == symbol) return &list[i];
	return nullptr;
}

int InventoryPanel::maxScroll() const {
	const int rows = int((_shownCount + COLS - 1) / COLS);
	return max(rows * CELL_H - GRID_H, 0);
}

void InventoryPanel::keepInSight(u32 index) {
	const int top = int(index / COLS) * CELL_H;
	if (top < _scroll) _scroll = top;
	if (top + CELL_H > _scroll + GRID_H) _scroll = top + CELL_H - GRID_H;
	_scroll = clamp(_scroll, 0, maxScroll());
}

void InventoryPanel::tileCentre(u32 n, int& x, int& y) const {
	x = GRID_X + int(n % COLS) * CELL_W + CELL_W / 2;
	y = GRID_Y + int(n / COLS) * CELL_H - _scroll + CELL_H / 2;
}

void InventoryPanel::slotCentre(int slot, int& x, int& y) {
	x = SLOT_X + (slot % 5) * SLOT_W + SLOT_W / 2;
	y = SLOT_Y + (slot / 5) * (SLOT_H + 2) + SLOT_H / 2;
}

bool InventoryPanel::dropButton(int k, u32 count, int& x0, int& x1) {
	if (count > 1) {
		x0 = k ? 162 : 8;
		x1 = k ? Canvas::W - 8 : 158;
		return k == 0 || k == 1;
	}
	x0 = 8;
	x1 = Canvas::W - 8;
	return k == 0;
}

void InventoryPanel::buttonCentre(int k, int& x, int& y) {
	x = 6 + k * (BUTTON_W + 7) + BUTTON_W / 2;
	y = BUTTON_Y + BUTTON_H / 2;
}

void InventoryPanel::pageCentre(int page, int& x, int& y) {
	x = PILL_W + 2 + page * TAB_W + TAB_W / 2;
	y = TABS_Y + TABS_H / 2;
}

InventoryPanel::Hit InventoryPanel::hitTest(int x, int y) const {
	Hit h;
	const auto inSlot = [&](int& slot) {
		for (int k = 0; k < QUICK_SLOTS; ++k) {
			int cx, cy;
			slotCentre(k, cx, cy);
			if (abs(x - cx) <= SLOT_W / 2 && abs(y - cy) <= SLOT_H / 2) {
				slot = k;
				return true;
			}
		}
		return false;
	};
	if (_page == PAGE_PLAY) {
		if (inSlot(h.index)) h.region = R_SLOT;
		else if (y >= BUTTON_Y && y < BUTTON_Y + BUTTON_H)
			for (int k = 0; k < 3; ++k) {
				int cx, cy;
				buttonCentre(k, cx, cy);
				if (abs(x - cx) <= BUTTON_W / 2) {
					h.region = R_BUTTON;
					h.index = k;
				}
			}
		return h;
	}
	if (_page == PAGE_DOC) {
		// The left half turns back, the right half on; the arrow at the top puts the book down.
		// (fix192: a map's thirds: smaller, larger, put away.)
		if (y < BARS_H) h.region = x < BACK_W ? R_BACK : R_BARS;
		else {
			h.region = R_DOC;
			h.index = _view && _view->doc.map ? min(x * 3 / Canvas::W, 2) : x < Canvas::W / 3 ? 0 : 1;
		}
		return h;
	}
	if (_page == PAGE_DIALOG) {
		// (The talk is left by the talk's own ways, not by the back arrow.)
		if (y >= TALK_Y) {
			h.region = R_TALK;
			h.index = (y - TALK_Y) / TALK_ROW;
		}
		return h;
	}
	if (y < BARS_H) {
		h.region = x < BACK_W ? R_BACK : R_BARS;
		return h;
	}
	if (_assigning) {
		h.region = R_PICKER;
		if (!inSlot(h.index) && y >= DROP_Y && y < DROP_Y + DROP_H) {
			// (fix192) The drop buttons: 100 one, 101 all.
			const ItemEntry* sel = _view ? entryOf(*_view, _selected) : nullptr;
			const u32 count = sel ? u32(max(sel->count, i32(1))) : 1u;
			for (int k = 0; k < 2; ++k) {
				int x0, x1;
				if (dropButton(k, count, x0, x1) && x >= x0 && x < x1) h.index = 100 + k;
			}
		}
		return h;
	}
	if (_page == PAGE_LOCK) {
		for (int k = 0; k < 2; ++k) {
			int cx, cy;
			lockCentre(k, cx, cy);
			if (abs(x - cx) <= 68 && abs(y - cy) <= 40) {
				h.region = R_LOCK;
				h.index = k;
			}
		}
		return h;
	}
	if (!gridPage()) {
		h.region = R_LIST;
		return h;
	}
	if (_page == PAGE_CHEST && y >= TABS_Y - 2) {
		h.region = R_STRIP;
		h.index = clamp((x - 2) / 107, 0, 2);
	} else if (y >= TABS_Y - 2) {
		if (x < PILL_W + 2) h.region = R_PREV_TAB;
		else if (x >= PILL_W + 2 + TAB_PAGES * TAB_W) h.region = R_NEXT_TAB;
		else {
			h.region = R_KIND;
			h.index = (x - PILL_W - 2) / TAB_W;
		}
	} else if (y >= GRID_Y && y < GRID_Y + GRID_H) {
		h.region = R_GRID;
		const int col = (x - GRID_X) / CELL_W, row = (y - GRID_Y + _scroll) / CELL_H;
		const u32 n = u32(row * COLS + col);
		if (x >= GRID_X && col < COLS && n < _shownCount) {
			h.region = R_TILE;
			h.index = int(n);
		}
	}
	return h;
}

PanelCommand InventoryPanel::update(const PanelInput& in, const InventoryView& v, float dt) {
	PanelCommand cmd;
	_view = &v;
	refilter(v);
	if (_flash > 0.0f && (_flash -= dt) <= 0.0f) _dirty = true;
	if (_inspect > 0.0f && !_assigning && !gridPage()) _inspect = 0.0f;
	const ItemEntry* sel = entryOf(v, _selected);
	if (_assigning && !sel) {
		_assigning = false; // (used up meanwhile)
		_dirty = true;
	}
	const auto use = [&](const ItemEntry* e) {
		if (e && _page == PAGE_CHEST) {
			// Over to the other side: all of the stack (a trader's wares one at a time: the strip's
			// third button and X do the whole stack).
			cmd.type = PANEL_CHEST_MOVE;
			cmd.symbol = e->symbol;
			cmd.amount = v.chest.trade ? 1 : e->count;
			cmd.slot = _side == 0 ? 0 : 1;
			_inspect = kInspectTime;
			_dirty = true;
			return;
		}
		if (!e || !(e->flags & ITEMF_ACTION)) return;
		cmd.type = PANEL_USE;
		cmd.symbol = e->symbol;
		_flash = 0.25f;
		_flashSymbol = e->symbol;
		_inspect = kInspectTime;
		_dirty = true;
	};
	const auto choose = [&](u32 symbol) {
		_selected = symbol;
		sel = entryOf(v, symbol);
		_inspect = kInspectTime;
		_dirty = true;
	};
	const auto drop = [&](bool all) {
		// (fix192) Down on the ground: one of the chosen thing, or all of it.
		_assigning = false;
		_dirty = true;
		if (!sel || _page != PAGE_INVENTORY) return;
		cmd.type = PANEL_DROP;
		cmd.symbol = sel->symbol;
		cmd.amount = all ? max(sel->count, i32(1)) : 1;
		_flash = 0.25f;
		_flashSymbol = sel->symbol;
		_inspect = kInspectTime;
	};
	const auto assign = [&](int slot) {
		_assigning = false;
		_dirty = true;
		_inspect = kInspectTime;
		if (!sel || !(sel->flags & ITEMF_QUICK) || slot < 0) return;
		// The same thing again in its own slot: the slot is emptied.
		cmd.type = v.quick[slot] == sel->symbol ? u8(PANEL_CLEAR_SLOT) : u8(PANEL_ASSIGN);
		cmd.slot = u8(slot);
		cmd.symbol = sel->symbol;
		_flash = 0.3f;
		_flashSlot = slot;
	};
	const auto close = [&]() {
		const u8 was = _page;
		openPage(PAGE_PLAY);
		cmd.type = was == PAGE_CHEST  ? u8(PANEL_CHEST_CLOSE)
		           : was == PAGE_LOCK ? u8(PANEL_LOCK_CANCEL)
		           : was == PAGE_DOC  ? u8(PANEL_DOC_CLOSE)
		                              : u8(PANEL_CURSOR_OFF);
	};
	const auto nextPage = [&](int step) {
		// L / R on the diary and the character: the pages round.
		const int p = (int(_page) - 1 + step + 3) % 3 + 1;
		openPage(PanelPage(p));
	};

	// ---- the buttons (a page open) ----
	if (_page == PAGE_DIALOG) {
		const TalkView& t = v.talk;
		if (_talkVersion != t.version) {
			_talkVersion = t.version;
			_talkCursor = 0;
			_dirty = true;
		}
		if (t.choosing && t.count) {
			const int before = _talkCursor;
			if (in.down) _talkCursor = min(_talkCursor + 1, int(t.count) - 1);
			if (in.up) _talkCursor = max(_talkCursor - 1, 0);
			if (before != _talkCursor) _dirty = true;
			if (in.confirm) {
				cmd.type = PANEL_TALK_CHOOSE;
				cmd.slot = u8(_talkCursor);
			}
		} else if (in.confirm) {
			cmd.type = PANEL_TALK_SKIP;
		}
		if (in.back) cmd.type = PANEL_TALK_LEAVE;
	} else if (_page == PAGE_DOC && v.doc.map) {
		// fix192: a map: A larger (round to the whole again), L / R smaller and larger, the D-Pad moves it; B puts it away.
		cmd.type = PANEL_MAP;
		cmd.slot = in.confirm ? 6 : in.prevTab ? 0 : in.nextTab ? 1 : in.left ? 2 : in.right ? 3 : in.up ? 4 : in.down ? 5 : 255;
		if (cmd.slot == 255) cmd = PanelCommand();
		if (in.back) close();
	} else if (_page == PAGE_DOC) {
		// A book: the pages turn with L / R, the D-Pad and A; B puts it down.
		if (in.left || in.prevTab || in.up) {
			cmd.type = PANEL_DOC_PAGE;
			cmd.slot = 0;
		} else if (in.right || in.nextTab || in.confirm || in.down) {
			cmd.type = PANEL_DOC_PAGE;
			cmd.slot = 1;
		}
		if (in.back) close();
	} else if (open()) {
		const ItemEntry* list = items(v);
		if (_page == PAGE_LOCK) {
			// The pick: left or right.
			if (in.left || in.prevTab) {
				cmd.type = PANEL_LOCK_PICK;
				cmd.slot = 0;
			} else if (in.right || in.nextTab) {
				cmd.type = PANEL_LOCK_PICK;
				cmd.slot = 1;
			}
		} else if (gridPage()) {
			if (!sel && _shownCount) choose(list[_shown[0]].symbol);
			int at = -1;
			for (u32 i = 0; i < _shownCount; ++i)
				if (list[_shown[i]].symbol == _selected) at = int(i);
			const bool wasAssigning = _assigning; // (A that picks a slot must not also use the thing)
			if (_assigning) {
				// The ten slots: the D-Pad walks them, A picks.
				const int before = _pickerSlot;
				if (in.left && _pickerSlot % 5 > 0) --_pickerSlot;
				if (in.right && _pickerSlot % 5 < 4) ++_pickerSlot;
				if (in.up && _pickerSlot >= 5) _pickerSlot -= 5;
				if (in.down && _pickerSlot < 5) _pickerSlot += 5;
				if (before != _pickerSlot) _dirty = true;
				if (in.confirm) assign(_pickerSlot);
			} else if (_shownCount && (in.up || in.down || in.left || in.right)) {
				int n = at < 0 ? 0 : at;
				if (in.left && n % COLS > 0) --n;
				if (in.right && n % COLS < COLS - 1 && u32(n + 1) < _shownCount) ++n;
				if (in.up && n >= COLS) n -= COLS;
				if (in.down) {
					if (u32(n + COLS) < _shownCount) n += COLS;
					else if (u32((n / COLS + 1) * COLS) < _shownCount) n = int(_shownCount) - 1; // (to the last, shorter row)
				}
				choose(list[_shown[n]].symbol);
				keepInSight(u32(n));
			}
			if (_page == PAGE_CHEST) {
				// L and R: the chest's things or the pack's. X: everything out of the chest.
				if (in.prevTab || in.nextTab) setSide(_side ? 0 : 1);
				if (in.assign && v.chest.trade) {
					if (const ItemEntry* e = entryOf(v, _selected)) {
						cmd.type = PANEL_CHEST_MOVE;
						cmd.symbol = e->symbol;
						cmd.amount = e->count;
						cmd.slot = _side == 0 ? 0 : 1;
						_dirty = true;
					}
				} else if (in.assign && _side == 0 && v.chest.count) {
					cmd.type = PANEL_CHEST_ALL;
				}
				if (in.confirm) use(entryOf(v, _selected));
			} else {
				if (in.prevTab && !wasAssigning) setTab(ItemTab((_tab + ITEM_TABS - 1) % ITEM_TABS));
				if (in.nextTab && !wasAssigning) setTab(ItemTab((_tab + 1) % ITEM_TABS));
				if (in.confirm && !wasAssigning) use(sel);
				if (in.assign && sel && (sel->flags & ITEMF_QUICK)) {
					_assigning = !_assigning;
					_dirty = true;
				}
				if ((in.drop || in.dropAll) && sel) drop(in.dropAll); // (fix192: Y)
			}
		} else {
			if (in.prevTab) nextPage(-1);
			if (in.nextTab) nextPage(1);
			if (_page == PAGE_CHARACTER && in.assign) {
				_talentsShown = !_talentsShown;
				_dirty = true;
			}
			if (_page == PAGE_DIARY && v.topicCount) {
				// The D-Pad walks the topics.
				if (in.down) _topic = min(_topic + 1, int(v.topicCount) - 1);
				if (in.up) _topic = max(_topic - 1, 0);
				if (in.up || in.down) _dirty = true;
			}
		}
		if (in.back) {
			if (_assigning) _assigning = false;
			else close();
			_dirty = true;
		}
		if (in.up || in.down || in.left || in.right || in.prevTab || in.nextTab) _inspect = kInspectTime;
	}

	// ---- the touch screen ----
	if (in.touching && !_touching) {
		_touching = true;
		_start = hitTest(in.x, in.y);
		_startX = _x = in.x;
		_startY = _y = in.y;
		_startScroll = gridPage() ? _scroll : _listScroll;
		_held = 0;
		_moved = _longDone = false;
		if (_start.region == R_SLOT || _start.region == R_BUTTON || _start.region == R_PICKER || _start.region == R_STRIP ||
		    _start.region == R_LOCK || _start.region == R_TALK || _start.region == R_DOC)
			_dirty = true; // (pressed)
		if (_start.region == R_TALK && v.talk.choosing && _start.index >= 0 && u32(_start.index) < v.talk.count) _talkCursor = _start.index;
	} else if (in.touching) {
		_held += dt;
		_x = in.x;
		_y = in.y;
		const int dx = _x - _startX, dy = _y - _startY;
		if (!_moved && dx * dx + dy * dy > 49) _moved = true;
		if (_moved && (_start.region == R_TILE || _start.region == R_GRID)) {
			const int to = clamp(_startScroll - dy, 0, maxScroll());
			if (to != _scroll) {
				_scroll = to;
				_dirty = true;
			}
		}
		if (_moved && _start.region == R_LIST) {
			const int to = max(_startScroll - dy, 0);
			if (to != _listScroll) {
				_listScroll = to;
				_dirty = true;
			}
		}
		if (!_moved && !_longDone && _held > 0.5f && _start.region == R_TILE && _page == PAGE_INVENTORY) {
			// Held on a thing: chosen, and the ten slots come up.
			_longDone = true;
			choose(items(v)[_shown[_start.index]].symbol);
			if (sel) _assigning = true; // (fix192: any thing - the slots for those that may go there, the drop buttons for all)
		}
	} else if (_touching) {
		_touching = false;
		const Hit end = hitTest(_x, _y);
		if (_start.region == R_PICKER) {
			if (end.region == R_PICKER && end.index >= 100 && end.index == _start.index) drop(end.index == 101);
			else if (end.region == R_PICKER && end.index >= 0 && end.index < 100 && end.index == _start.index) assign(end.index);
			else if (end.index < 0 && _start.index < 0) _assigning = false; // (a tap beside the slots: never mind)
		} else if (_moved) {
			if (gridPage()) _scroll = clamp((_scroll + CELL_H / 2) / CELL_H * CELL_H, 0, maxScroll()); // (on a whole row)
		} else if (!_longDone && end.region == _start.region && end.index == _start.index) {
			switch (end.region) {
			case R_SLOT:
				cmd.type = PANEL_USE_SLOT;
				cmd.slot = u8(end.index);
				_flash = 0.25f;
				_flashSlot = end.index;
				break;
			case R_BUTTON: openPage(PanelPage(PAGE_INVENTORY + end.index)); break;
			case R_STRIP:
				if (end.index < 2) setSide(end.index);
				else if (v.chest.trade) {
					if (const ItemEntry* e = entryOf(v, _selected)) {
						cmd.type = PANEL_CHEST_MOVE;
						cmd.symbol = e->symbol;
						cmd.amount = e->count;
						cmd.slot = _side == 0 ? 0 : 1;
						_inspect = kInspectTime;
						_dirty = true;
					}
				} else if (_side == 0 && v.chest.count) cmd.type = PANEL_CHEST_ALL;
				break;
			case R_TALK:
				if (v.talk.choosing && end.index >= 0 && u32(end.index) < v.talk.count) {
					cmd.type = PANEL_TALK_CHOOSE;
					cmd.slot = u8(end.index);
				} else if (!v.talk.choosing) {
					cmd.type = PANEL_TALK_SKIP;
				}
				break;
			case R_DOC:
				if (v.doc.map) {
					// (fix192) A map: smaller, larger, put away.
					if (end.index == 2) close();
					else {
						cmd.type = PANEL_MAP;
						cmd.slot = u8(end.index);
					}
					break;
				}
				cmd.type = PANEL_DOC_PAGE;
				cmd.slot = u8(end.index);
				break;
			case R_LOCK:
				cmd.type = PANEL_LOCK_PICK;
				cmd.slot = u8(end.index);
				_flash = 0.15f;
				_flashSlot = 100 + end.index;
				break;
			case R_BACK:
			case R_BARS: close(); break;
			case R_KIND:
				if (end.index == ITEM_TABS) cmd.type = PANEL_REPORT;
				else setTab(ItemTab(end.index));
				break;
			case R_PREV_TAB: setTab(ItemTab((_tab + ITEM_TABS - 1) % ITEM_TABS)); break;
			case R_NEXT_TAB: setTab(ItemTab((_tab + 1) % ITEM_TABS)); break;
			case R_TILE: {
				const ItemEntry& e = items(v)[_shown[end.index]];
				if (e.symbol == _selected && _inspect > 0.0f) use(&e);
				else choose(e.symbol);
				break;
			}
			case R_LIST:
				if (_page == PAGE_CHARACTER) {
					_talentsShown = !_talentsShown;
					_dirty = true;
				}
				if (_page == PAGE_DIARY)
					// A tap on a topic opens it (on the open one: closes it).
					for (u32 t = 0; t < v.topicCount && t < InventoryView::DIARY_TOPICS; ++t)
						if (_y >= _topicY[t] && _y < _topicY[t] + 16) _topic = _topic == int(t) ? -1 : int(t);
				break;
			default: break;
			}
		}
		_dirty = true;
	}
	return cmd;
}

// ---- drawing ----

void InventoryPanel::cell(int x, int y, int w, int h, bool chosen, bool lit) {
	if (chosen) _canvas.gradient(x + 1, y + 1, x + w, y + h, SEL_TOP, SEL_BOTTOM);
	else _canvas.fill(x + 1, y + 1, x + w, y + h, lit ? 0x3A5E8870u : CELL_FILL);
}

void InventoryPanel::drawThing(int x, int y, int w, int h, i16 pic, u8 icon, u32 tint, bool dim) {
	const ItemPicture* p = _pictures ? _pictures->get(pic) : nullptr;
	if (p) {
		_canvas.picture(x, y, w, h, p->rgb, p->alpha, p->w, p->h, dim);
		return;
	}
	// No picture baked (an older data.pak): the drawn sign.
	const int scale = min(w, h) >= 32 ? 2 : 1;
	_canvas.icon(x + (w - 16 * scale) / 2, y + (h - 16 * scale) / 2, icon, tint, scale, dim ? Canvas::ICON_DIM : Canvas::ICON_NORMAL);
}

bool InventoryPanel::draw(const InventoryView& v, bool force) {
	refilter(v);
	if (!_canvas.ready() || (!force && !_dirty && v.version == _drawnVersion)) return false;
	_dirty = false;
	_drawnVersion = v.version;
	_canvas.unclip();
	_canvas.gradient(0, 0, Canvas::W, Canvas::H, BG_TOP, BG_BOTTOM);
	drawBars(v, open());
	switch (_page) {
	case PAGE_INVENTORY:
		drawGrid(v);
		drawTabs(v);
		break;
	case PAGE_DIARY: drawDiary(v); break;
	case PAGE_CHARACTER: drawCharacter(v); break;
	case PAGE_CHEST:
		drawGrid(v);
		drawStrip(v);
		break;
	case PAGE_LOCK: drawLock(v); break;
	case PAGE_DIALOG: drawDialog(v); break;
	case PAGE_DOC: drawDoc(v); break;
	default: drawPlay(v); break;
	}
	if (_assigning) drawPicker(v);
	return true;
}

void InventoryPanel::drawBars(const InventoryView& v, bool header) {
	// Health on the left, mana on the right: framed bars, as on the console's
	// screen. An open page has its back arrow and name before them.
	const auto bar = [&](int x0, int x1, i32 value, i32 most, u32 fill, u32 light) {
		const int y0 = 5, y1 = 15;
		_canvas.fill(x0 - 3, y0 - 3, x1 + 3, y1 + 3, 0x2A2C30FFu);
		_canvas.frame(x0 - 3, y0 - 3, x1 + 3, y1 + 3, 0x8A8E94FFu);
		_canvas.fill(x0 - 5, y0 - 1, x0 - 3, y1 + 1, 0x8A8E94FFu); // (the end caps of the frame)
		_canvas.fill(x1 + 3, y0 - 1, x1 + 5, y1 + 1, 0x8A8E94FFu);
		_canvas.fill(x0, y0, x1, y1, 0x101010FFu);
		if (most > 0) {
			const int w = int((long long)(x1 - x0) * clamp(value, i32(0), most) / most);
			_canvas.gradient(x0, y0, x0 + w, y1, light, fill);
			_canvas.fill(x0, y0 + 1, x0 + w, y0 + 2, lerp(light, 0xFFFFFFFFu, 90));
		}
	};
	if (!header) {
		bar(9, 152, v.hp, v.hpMax, HP, HP_LIGHT);
		bar(168, 311, v.mana, v.manaMax, MANA, MANA_LIGHT);
		return;
	}
	if (_page != PAGE_DIALOG) {
		_canvas.fill(2, 2, BACK_W - 4, BARS_H - 1, 0x2A2E34FFu);
		_canvas.frame(2, 2, BACK_W - 4, BARS_H - 1, CELL_LINE);
		_canvas.text((BACK_W - 2) / 2, 3, "\xE2\x97\x80", TITLE, ui::TEXT, ui::CENTER);
	}
	_canvas.text(_page == PAGE_DIALOG ? 8 : BACK_W + 2, 3, _page == PAGE_CHEST && v.chest.name[0] ? v.chest.name : tr(kPageTitles[_page]), TITLE, ui::TEXT, ui::LEFT, 84,
	             true);
	bar(140, 222, v.hp, v.hpMax, HP, HP_LIGHT);
	bar(238, 311, v.mana, v.manaMax, MANA, MANA_LIGHT);
}

void InventoryPanel::slotTile(int slot, const InventoryView& v, bool picker) {
	int cx, cy;
	slotCentre(slot, cx, cy);
	const int x = cx - SLOT_W / 2 + 1, y = cy - SLOT_H / 2, w = SLOT_W - 3, h = SLOT_H;
	const bool pressed = _touching && !_moved && (_start.region == R_SLOT || _start.region == R_PICKER) && _start.index == slot;
	const bool lit = (_flash > 0.0f && _flashSlot == slot) || pressed || (picker && _pickerSlot == slot) || (!picker && _choiceSlot == slot);
	// A key of a keyboard, in the colours of the console's cells.
	if (lit) _canvas.gradient(x, y, x + w, y + h, SEL_TOP, SEL_BOTTOM);
	else _canvas.gradient(x, y, x + w, y + h, 0x262A30F0u, 0x14171AF0u);
	_canvas.frame(x, y, x + w, y + h, lit ? SEL_EDGE : CELL_LINE);
	_canvas.fill(x + 1, y + 1, x + w - 1, y + 2, 0xFFFFFF18u);
	_canvas.text(x + 4, y + 1, kKeys[slot], lit ? TITLE : TEXT_DIM, ui::TEXT, ui::LEFT, 0, true);
	if (v.quick[slot] == SCRIPT_NONE) return;
	const bool none = v.quickCount[slot] <= 0;
	drawThing(x + 8, y + 8, w - 16, h - 14, v.quickPicture[slot], v.quickIcon[slot], v.quickTint[slot], none);
	char n[16];
	snprintf(n, sizeof(n), "%ld", static_cast<long>(v.quickCount[slot]));
	if (v.quickCount[slot] != 1) _canvas.text(x + w - 4, y + h - 15, n, none ? WARN : TEXT, ui::TEXT, ui::RIGHT, 0, true);
}

void InventoryPanel::drawPlay(const InventoryView& v) {
	for (int k = 0; k < QUICK_SLOTS; ++k) slotTile(k, v, false);
	static const u8 kIcons[3] = {ICON_BAG, ICON_BOOK, ICON_ARMOR};
	static const char* const kNames[3] = {"Инвентарь", "Дневник", "Персонаж"};
	for (int k = 0; k < 3; ++k) {
		int cx, cy;
		buttonCentre(k, cx, cy);
		const int x0 = cx - BUTTON_W / 2, y0 = BUTTON_Y, x1 = x0 + BUTTON_W, y1 = y0 + BUTTON_H;
		const bool pressed = _touching && !_moved && _start.region == R_BUTTON && _start.index == k;
		_canvas.gradient(x0, y0, x1, y1, pressed ? SEL_TOP : TAB_TOP, pressed ? SEL_BOTTOM : TAB_BOTTOM);
		_canvas.frame(x0, y0, x1, y1, pressed ? SEL_EDGE : TAB_LIGHT);
		_canvas.fill(x0 + 1, y1 - 2, x1 - 1, y1 - 1, TAB_DARK);
		_canvas.icon(cx - 16, y0 + 12, kIcons[k], 0xE8ECF0FFu, 2, Canvas::ICON_MONO);
		_canvas.text(cx, y0 + 52, tr(kNames[k]), TITLE, ui::TEXT, ui::CENTER, 0, true);
	}
	if (v.messageTime > 0.0f && v.message[0])
		_canvas.text(Canvas::W / 2, BUTTON_Y - 17, v.message, GOLD, ui::TEXT, ui::CENTER, Canvas::W - 8, true);
}

void InventoryPanel::drawTabs(const InventoryView&) {
	const int x0 = 2, x1 = Canvas::W - 2, y0 = TABS_Y, y1 = TABS_Y + TABS_H;
	_canvas.gradient(x0, y0, x1, y1, TAB_TOP, TAB_BOTTOM);
	_canvas.fill(x0, y0, x1, y0 + 1, TAB_LIGHT);
	_canvas.fill(x0, y1 - 1, x1, y1, TAB_DARK);
	for (int t = 0; t < TAB_PAGES; ++t) {
		int cx, cy;
		pageCentre(t, cx, cy);
		const bool on = t == _tab;
		if (on) {
			_canvas.fill(cx - TAB_W / 2 + 1, y0 + 1, cx + TAB_W / 2 - 1, y1 - 1, TAB_ON);
			_canvas.fill(cx - TAB_W / 2 + 4, y1 - 3, cx + TAB_W / 2 - 4, y1 - 2, 0xE8ECF0FFu);
		}
		if (t < ITEM_TABS) _canvas.icon(cx - 8, cy - 9, kTabIcons[t], on ? 0xF4F4F4FFu : 0xA8B4C4C0u, 1, Canvas::ICON_MONO);
		else {
			// The report: a little chart.
			for (int k = 0; k < 4; ++k) {
				static const int kH[4] = {5, 10, 7, 13};
				_canvas.fill(cx - 7 + k * 4, cy + 6 - kH[k], cx - 4 + k * 4, cy + 6, 0xA8B4C4C0u);
			}
		}
	}
	// L and R at the ends, as the shoulder buttons change the page.
	for (int k = 0; k < 2; ++k) {
		const int cx = k ? x1 - PILL_W / 2 : x0 + PILL_W / 2, cy = (y0 + y1) / 2;
		_canvas.fill(cx - 8, cy - 6, cx + 8, cy + 6, PILL);
		_canvas.fill(cx - 9, cy - 5, cx + 9, cy + 5, PILL);
		_canvas.text(cx, cy - 7, k ? "R" : "L", PILL_TEXT, ui::TEXT, ui::CENTER);
	}
}

void InventoryPanel::drawGrid(const InventoryView& v) {
	const int x0 = GRID_X, y0 = GRID_Y, x1 = GRID_X + COLS * CELL_W - 1, y1 = GRID_Y + GRID_H;
	_canvas.clip(x0, y0, x1 + 1, y1 + 1);
	const int rows = max(int((_shownCount + COLS - 1) / COLS), ROWS_SHOWN);
	const int firstRow = _scroll / CELL_H;
	for (int r = firstRow; r < rows && r * CELL_H - _scroll < GRID_H; ++r)
		for (int c = 0; c < COLS; ++c) {
			const u32 n = u32(r * COLS + c);
			const int x = x0 + c * CELL_W, y = y0 + r * CELL_H - _scroll;
			const ItemEntry* e = n < _shownCount ? &items(v)[_shown[n]] : nullptr;
			const bool chosen = e && e->symbol == _selected;
			cell(x, y, CELL_W, CELL_H, chosen, e && _flash > 0.0f && e->symbol == _flashSymbol);
			if (!e) continue;
			const bool dim = (e->flags & ITEMF_BLOCKED) && !(e->flags & ITEMF_EQUIPPED);
			drawThing(x + 4, y + 2, CELL_W - 8, CELL_H - 4, e->picture, e->icon, e->tint, dim);
			if (e->flags & ITEMF_EQUIPPED) {
				// Worn: a gold corner; in the hands: a gold line along the bottom too.
				for (int k = 0; k < 7; ++k) _canvas.fill(x + 1, y + 1 + k, x + 8 - k, y + 2 + k, GOLD);
				if (e->flags & ITEMF_IN_HAND) _canvas.fill(x + 3, y + CELL_H - 3, x + CELL_W - 2, y + CELL_H - 1, GOLD);
			}
			for (int q = 0; q < QUICK_SLOTS; ++q)
				if (e->quick & (1u << q)) {
					_canvas.text(x + CELL_W - 4, y + 1, kKeys[q], 0xD8DCE0C0u, ui::TEXT, ui::RIGHT, 0, true);
					break;
				}
			if (_page == PAGE_CHEST && v.chest.trade) {
				// What it costs (the trader's wares) or fetches (the pack's): red where the purse is short.
				char pr[16];
				snprintf(pr, sizeof(pr), "%ld", static_cast<long>(e->price));
				_canvas.text(x + 4, y + CELL_H - 14, pr, _side == 0 && e->price > v.chest.gold ? WARN : GOLD, ui::TEXT, ui::LEFT, 0, true);
			}
			if (e->count > 1 || dim) {
				char n2[16];
				snprintf(n2, sizeof(n2), "%ld", static_cast<long>(e->count));
				_canvas.text(x + CELL_W - 4, y + CELL_H - 14, n2, dim ? WARN : TEXT, ui::TEXT, ui::RIGHT, 0, true);
			}
		}
	// The thin grey lines over it all, as on the console.
	for (int c = 0; c <= COLS; ++c) _canvas.fill(min(x0 + c * CELL_W, x1), y0, min(x0 + c * CELL_W, x1) + 1, y1 + 1, CELL_LINE);
	for (int r = firstRow; r <= rows && r * CELL_H - _scroll <= GRID_H; ++r) {
		const int y = y0 + r * CELL_H - _scroll;
		_canvas.fill(x0, y, x1 + 1, y + 1, CELL_LINE);
	}
	_canvas.fill(x0, y1, x1 + 1, y1 + 1, CELL_LINE);
	for (u32 n = 0; n < _shownCount; ++n) {
		// (The chosen cell's white edge stays on top of the lines.)
		if (items(v)[_shown[n]].symbol != _selected) continue;
		const int x = x0 + int(n % COLS) * CELL_W, y = y0 + int(n / COLS) * CELL_H - _scroll;
		_canvas.frame(x, y, min(x + CELL_W + 1, x1 + 1), y + CELL_H + 1, SEL_EDGE);
	}
	if (maxScroll() > 0) {
		const int h = max(GRID_H * GRID_H / (GRID_H + maxScroll()), 12);
		const int y = y0 + (GRID_H - h) * _scroll / maxScroll();
		_canvas.fill(x1 - 3, y + 1, x1 - 1, y + h, 0xE8ECF0A0u);
	}
	if (_shownCount == 0)
		_canvas.text((x0 + x1) / 2, y0 + GRID_H / 2 - 7,
		             _page == PAGE_CHEST ? (_side == 0 ? (v.chest.trade ? tr("Товара нет") : tr("Сундук пуст")) : tr("В рюкзаке пусто"))
		                                 : v.present ? tr(kTabNames[_tab]) : tr("Инвентаря нет"),
		             TEXT_DIM, ui::TEXT, ui::CENTER, 0, true);
	_canvas.unclip();
	if (_page == PAGE_CHEST && v.chest.trade && !(v.messageTime > 0.0f && v.message[0])) {
		// The purse, where the last news would be.
		char purse[48];
		snprintf(purse, sizeof(purse), "%s: %ld", v.chest.currency[0] ? v.chest.currency : tr("Руда"), static_cast<long>(v.chest.gold));
		const int w = Canvas::measure(purse) + 12;
		_canvas.fill(Canvas::W / 2 - w / 2, y1 - 17, Canvas::W / 2 + w / 2, y1 - 1, 0x000000C0u);
		_canvas.text(Canvas::W / 2, y1 - 16, purse, GOLD, ui::TEXT, ui::CENTER, Canvas::W - 8);
	}
	if (v.messageTime > 0.0f && v.message[0]) {
		// The last news over the bottom of the grid.
		const int w = Canvas::measure(v.message) + 12;
		_canvas.fill(Canvas::W / 2 - w / 2, y1 - 17, Canvas::W / 2 + w / 2, y1 - 1, 0x000000C0u);
		_canvas.text(Canvas::W / 2, y1 - 16, v.message, GOLD, ui::TEXT, ui::CENTER, Canvas::W - 8);
	}
}

// The chest page's strip: the chest's things, the pack's, everything out of the chest.
void InventoryPanel::drawStrip(const InventoryView& v) {
	const int x0 = 2, x1 = Canvas::W - 2, y0 = TABS_Y, y1 = TABS_Y + TABS_H;
	_canvas.gradient(x0, y0, x1, y1, TAB_TOP, TAB_BOTTOM);
	_canvas.fill(x0, y0, x1, y0 + 1, TAB_LIGHT);
	_canvas.fill(x0, y1 - 1, x1, y1, TAB_DARK);
	static const char* const kNames[3] = {"Сундук", "Рюкзак", "Взять всё"};
	const bool trade = v.chest.trade;
	for (int k = 0; k < 3; ++k) {
		int cx, cy;
		stripCentre(k, cx, cy);
		const bool on = k < 2 && k == _side;
		const bool pressed = _touching && !_moved && _start.region == R_STRIP && _start.index == k;
		const bool off = k == 2 && (trade ? entryOf(v, _selected) == nullptr : (_side != 0 || v.chest.count == 0));
		if (on || pressed) {
			_canvas.fill(cx - 51, y0 + 1, cx + 51, y1 - 1, pressed ? SEL_BOTTOM : TAB_ON);
			_canvas.fill(cx - 46, y1 - 3, cx + 46, y1 - 2, 0xE8ECF0FFu);
		}
		if (k > 0) _canvas.fill(cx - 53, y0 + 3, cx - 52, y1 - 3, TAB_LIGHT);
		char label[40];
		if (k == 0) snprintf(label, sizeof(label), trade ? tr("Товар %lu") : tr("Сундук %lu"), static_cast<unsigned long>(v.chest.count));
		else if (k == 2 && trade) snprintf(label, sizeof(label), "%s", tr("Всю стопку"));
		else snprintf(label, sizeof(label), "%s", tr(kNames[k]));
		_canvas.text(cx, y0 + 4, label, off ? TEXT_DIM : on ? TITLE : TEXT, ui::TEXT, ui::CENTER, 100, true);
	}
}

// The lock: the squares of the combination, and the pick, left or right.
void InventoryPanel::drawLock(const InventoryView& v) {
	const InventoryView::Chest& c = v.chest;
	char buf[64];
	snprintf(buf, sizeof(buf), tr("Отмычек: %d"), int(c.picks));
	_canvas.text(Canvas::W / 2, BARS_H + 12, buf, c.picks ? TEXT : WARN, ui::TEXT, ui::CENTER, 0, true);
	// The combination: a square for each turn.
	const int n = clamp(int(c.length), 1, 16), w = 16, gap = 4;
	const int total = n * w + (n - 1) * gap, x0 = (Canvas::W - total) / 2, y0 = 70;
	for (int k = 0; k < n; ++k) {
		const int x = x0 + k * (w + gap);
		const bool done = k < c.progress;
		_canvas.fill(x, y0, x + w, y0 + w, done ? GOLD : 0x0B0D10E0u);
		_canvas.frame(x, y0, x + w, y0 + w, done ? 0xFFFFFFFFu : CELL_LINE);
	}
	_canvas.text(Canvas::W / 2, y0 + 28, c.text[0] ? c.text : tr("Верно — дальше, неверно — отмычка сломается"), c.text[0] ? GOLD : TEXT_DIM,
	             ui::TEXT, ui::CENTER, Canvas::W - 10, true);
	for (int k = 0; k < 2; ++k) {
		int cx, cy;
		lockCentre(k, cx, cy);
		const int bx0 = cx - 68, by0 = cy - 40, bx1 = cx + 68, by1 = cy + 40;
		const bool pressed = (_touching && !_moved && _start.region == R_LOCK && _start.index == k) || (_flash > 0.0f && _flashSlot == 100 + k);
		_canvas.gradient(bx0, by0, bx1, by1, pressed ? SEL_TOP : TAB_TOP, pressed ? SEL_BOTTOM : TAB_BOTTOM);
		_canvas.frame(bx0, by0, bx1, by1, pressed ? SEL_EDGE : TAB_LIGHT);
		_canvas.fill(bx0 + 1, by1 - 2, bx1 - 1, by1 - 1, TAB_DARK);
		_canvas.text(cx, cy - 8, k ? tr("Вправо \xE2\x96\xB6") : tr("\xE2\x97\x80 Влево"), TITLE, ui::TITLE, ui::CENTER, 0, true);
	}
	_canvas.text(Canvas::W / 2, 214, tr("Крестовина или L / R — тоже; B — бросить"), TEXT_DIM, ui::TEXT, ui::CENTER, Canvas::W - 8, true);
}

void InventoryPanel::drawDoc(const InventoryView& v) {
	// The words are on the top screen; here the pages turn.
	const InventoryView::Doc& d = v.doc;
	if (d.map) {
		// fix192: the map is on the top screen; here it is made smaller or larger, or put away.
		_canvas.text(Canvas::W / 2, BARS_H + 12, tr("Карта"), TITLE, ui::TITLE, ui::CENTER, 0, true);
		static const char* const kLabels[3] = {"-", "+", "Закрыть"};
		for (int k = 0; k < 3; ++k) {
			const int bx0 = 6 + k * (Canvas::W / 3), bx1 = (k + 1) * (Canvas::W / 3) - 6, by0 = 70, by1 = 190;
			const bool pressed = _touching && !_moved && _start.region == R_DOC && _start.index == k;
			_canvas.gradient(bx0, by0, bx1, by1, pressed ? SEL_TOP : TAB_TOP, pressed ? SEL_BOTTOM : TAB_BOTTOM);
			_canvas.frame(bx0, by0, bx1, by1, pressed ? SEL_EDGE : TAB_LIGHT);
			_canvas.text((bx0 + bx1) / 2, (by0 + by1) / 2 - 8, k == 2 ? tr(kLabels[k]) : kLabels[k], TITLE, ui::TITLE, ui::CENTER, bx1 - bx0 - 8, true);
		}
		_canvas.text(Canvas::W / 2, 206, tr("A, L, R — масштаб; крестовина, стик — двигать"), TEXT_DIM, ui::TEXT, ui::CENTER, Canvas::W - 8, true);
		_canvas.text(Canvas::W / 2, 222, tr("B — закрыть"), TEXT_DIM, ui::TEXT, ui::CENTER, Canvas::W - 8, true);
		return;
	}
	char buf[48];
	snprintf(buf, sizeof(buf), tr("Страница %u из %u"), unsigned(d.page + 1), unsigned(d.pages ? d.pages : 1));
	_canvas.text(Canvas::W / 2, BARS_H + 12, buf, TITLE, ui::TITLE, ui::CENTER, 0, true);
	for (int k = 0; k < 2; ++k) {
		const bool left = k == 0;
		const bool ok = left ? d.page > 0 : true;
		const int bx0 = left ? 6 : Canvas::W / 3 + 6, bx1 = left ? Canvas::W / 3 - 6 : Canvas::W - 6, by0 = 70, by1 = 190;
		const bool pressed = _touching && !_moved && _start.region == R_DOC && _start.index == k;
		_canvas.gradient(bx0, by0, bx1, by1, pressed ? SEL_TOP : TAB_TOP, pressed ? SEL_BOTTOM : TAB_BOTTOM);
		_canvas.frame(bx0, by0, bx1, by1, pressed ? SEL_EDGE : TAB_LIGHT);
		const char* label = left ? tr("\xE2\x97\x80 Назад") : d.page + 1 >= d.pages ? tr("Закрыть") : tr("Дальше \xE2\x96\xB6");
		_canvas.text((bx0 + bx1) / 2, (by0 + by1) / 2 - 8, label, ok ? TITLE : TEXT_DIM, ui::TITLE, ui::CENTER, bx1 - bx0 - 8, true);
	}
	_canvas.text(Canvas::W / 2, 214, tr("A, R, крестовина — дальше; L — назад; B — закрыть"), TEXT_DIM, ui::TEXT, ui::CENTER, Canvas::W - 8, true);
}

void InventoryPanel::drawDialog(const InventoryView& v) {
	// Only the ways to go on: the words (and who says them) are in the dialogue box
	// on the top screen.
	const TalkView& t = v.talk;
	const int x0 = 6, x1 = Canvas::W - 6;
	if (t.choosing && v.sheet.learn > 0) {
		// What a teacher is paid in.
		char lp[40];
		snprintf(lp, sizeof(lp), tr("Очки обучения: %ld"), static_cast<long>(v.sheet.learn));
		_canvas.text(Canvas::W - 6, Canvas::H - 16, lp, GOLD, ui::TEXT, ui::RIGHT, 0, true);
	}
	if (t.choosing && t.count) {
		for (u32 k = 0; k < t.count && k < u32(TALK_ITEMS); ++k) {
			const int y = TALK_Y + int(k) * TALK_ROW;
			const bool chosen = int(k) == _talkCursor;
			if (chosen) {
				_canvas.gradient(x0, y, x1, y + TALK_ROW - 2, SEL_TOP, SEL_BOTTOM);
				_canvas.frame(x0, y, x1, y + TALK_ROW - 2, SEL_EDGE);
			} else {
				_canvas.fill(x0, y, x1, y + TALK_ROW - 2, CELL_FILL);
				_canvas.frame(x0, y, x1, y + TALK_ROW - 2, CELL_LINE);
			}
			_canvas.text(x0 + 6, y + 1, t.items[k], chosen ? TITLE : TEXT, ui::TEXT, ui::LEFT, x1 - x0 - 12, true);
		}
		return;
	}
	if (t.saying) _canvas.text(Canvas::W / 2, Canvas::H - 20, tr("A — дальше"), TEXT_DIM, ui::TEXT, ui::CENTER, 0, true);
}

void InventoryPanel::drawPicker(const InventoryView& v) {
	// Over the page, darkened: the ten slots to choose from, the thing to put there; under them (fix192) the buttons
	// that put it down on the ground - one, or the whole stack.
	_canvas.fill(0, BARS_H, Canvas::W, Canvas::H, 0x000000C0u);
	const ItemEntry* sel = entryOf(v, _selected);
	const bool quick = sel && (sel->flags & ITEMF_QUICK);
	for (int k = 0; k < QUICK_SLOTS; ++k) slotTile(k, v, quick);
	if (!quick) _canvas.fill(0, SLOT_Y - 2, Canvas::W, SLOT_Y + 2 * SLOT_H + 4, 0x000000A0u); // (no slot for this one)
	const int y0 = SLOT_Y + 2 * SLOT_H + 6;
	if (sel) {
		drawThing(8, y0, 34, 34, sel->picture, sel->icon, sel->tint, false);
		_canvas.text(48, y0 + 1, sel->name, TITLE, ui::TEXT, ui::LEFT, Canvas::W - 56, true);
	}
	_canvas.text(48, y0 + 19, quick ? tr("В какой слот? Та же вещь в своём слоте — убрать") : tr("Эту вещь в слот не положить"), TEXT_DIM, ui::TEXT,
	             ui::LEFT, Canvas::W - 56, true);
	const u32 count = sel ? u32(max(sel->count, i32(1))) : 1u;
	for (int k = 0; k < 2; ++k) {
		int x0, x1;
		if (!sel || !dropButton(k, count, x0, x1)) continue;
		const bool pressed = _touching && !_moved && _start.region == R_PICKER && _start.index == 100 + k;
		const int y1 = DROP_Y + DROP_H;
		_canvas.gradient(x0, DROP_Y, x1, y1, pressed ? SEL_TOP : TAB_TOP, pressed ? SEL_BOTTOM : TAB_BOTTOM);
		_canvas.frame(x0, DROP_Y, x1, y1, pressed ? SEL_EDGE : TAB_LIGHT);
		char label[48];
		if (k == 1) snprintf(label, sizeof(label), tr("Выбросить все (%lu)"), static_cast<unsigned long>(count));
		else snprintf(label, sizeof(label), "%s", tr("Выбросить"));
		_canvas.text((x0 + x1) / 2, DROP_Y + DROP_H / 2 - 7, label, TITLE, ui::TEXT, ui::CENTER, x1 - x0 - 6, true);
	}
}

void InventoryPanel::drawDiary(const InventoryView& v) {
	const int x0 = 6, x1 = Canvas::W - 6;
	_canvas.clip(0, BARS_H + 2, Canvas::W, Canvas::H);
	int y = BARS_H + 4 - _listScroll;
	if (!v.topicCount) {
		_canvas.paragraph(x0, y + 8, x1 - x0, tr("Записей пока нет. Их делают скрипты игры (Log_AddEntry) в разговорах и заданиях."), TEXT_DIM);
	}
	// The chapter the story is in.
	if (v.chapter[0] || v.chapterNumber > 0) {
		char head[64];
		if (v.chapter[0]) snprintf(head, sizeof(head), "%s", v.chapter);
		else snprintf(head, sizeof(head), tr("Глава %d"), int(v.chapterNumber));
		_canvas.fill(x0 - 2, y, x1 + 2, y + 15, 0xFFFFFF18u);
		_canvas.text(x0 + 2, y + 1, head, GOLD, ui::TEXT, ui::LEFT, x1 - x0 - 4, true);
		if (v.chapterSub[0]) _canvas.text(x1 - 2, y + 1, v.chapterSub, TEXT_DIM, ui::TEXT, ui::RIGHT, (x1 - x0) / 2, true);
		y += 20;
	}
	static const char* const kStatus[] = {"", "идёт", "выполнено", "провалено", "устарело"};
	// The tasks first (the current ones, the done, the failed), then the notes.
	static const char* const kGroup[] = {"Текущие", "Завершённые", "Проваленные"};
	for (int pass = 0; pass < 4; ++pass) {
		const bool notes = pass == 3;
		bool headed = false;
		for (u32 t = 0; t < v.topicCount; ++t) {
			const InventoryView::Topic& topic = v.topics[t];
			if ((topic.section == 1) != notes) continue;
			const int group = topic.status == 2 ? 1 : (topic.status == 3 || topic.status == 4) ? 2 : 0;
			if (!notes && group != pass) continue;
			if (!headed) {
				headed = true;
				if (notes) {
					_canvas.text(x0, y + 1, tr("Заметки"), TITLE, ui::TEXT, ui::LEFT, 0, true);
					y += 16;
				} else {
					// (The first group carries the heading of all the tasks.)
					_canvas.text(x0, y + 1, tr(kGroup[group]), group == 0 ? TITLE : TEXT_DIM, ui::TEXT, ui::LEFT, 0, true);
					y += 16;
				}
			}
			const bool openTopic = _topic == int(t);
			_topicY[t] = y;
			_canvas.fill(x0 - 2, y, x1 + 2, y + 15, openTopic ? TAB_ON : 0xFFFFFF10u);
			if (topic.fresh) _canvas.text(x0 + 2, y + 1, "\xE2\x80\xA2", GOLD, ui::TEXT, ui::LEFT);
			_canvas.text(x0 + (topic.fresh ? 10 : 2), y + 1, topic.name, openTopic ? TITLE : group == 0 ? TEXT : TEXT_DIM, ui::TEXT, ui::LEFT,
			             x1 - x0 - 80, true);
			_canvas.text(x1 - 2, y + 1, topic.status < 5 ? tr(kStatus[topic.status]) : "", topic.status == 2 ? GOLD : topic.status == 3 ? WARN : TEXT_DIM,
			             ui::TEXT, ui::RIGHT);
			y += 16;
			if (!openTopic) continue;
			for (u32 k = 0; k < topic.count; ++k) {
				y += 2;
				y += _canvas.paragraph(x0 + 8, y, x1 - x0 - 10, v.entries[topic.first + k], TEXT_DIM);
			}
			y += 4;
		}
		if (headed) y += 4;
	}
	_canvas.unclip();
}

void InventoryPanel::drawCharacter(const InventoryView& v) {
	const InventoryView::Sheet& s = v.sheet;
	if (_talentsShown) {
		// The talents the scripts name, with the level taught; the numbers again with X or a tap.
		_canvas.text(Canvas::W / 2, BARS_H + 6, tr("Умения"), TITLE, ui::TITLE, ui::CENTER, 0, true);
		int ty = BARS_H + 28;
		bool any = false;
		for (u32 k = 0; k < s.talentCount; ++k) {
			const InventoryView::Sheet::Talent& t = s.talents[k];
			char buf[48];
			if (t.value > 0) snprintf(buf, sizeof(buf), "%s  %ld%%", t.level, static_cast<long>(t.value));
			else snprintf(buf, sizeof(buf), "%s", t.level);
			const bool learnt = t.skill != 0 || t.value > 0;
			_canvas.text(12, ty, t.name, learnt ? TEXT : TEXT_DIM, ui::TEXT, ui::LEFT, 150, true);
			_canvas.text(Canvas::W - 12, ty, buf, learnt ? GOLD : TEXT_DIM, ui::TEXT, ui::RIGHT, 150, true);
			_canvas.fill(12, ty + 13, Canvas::W - 12, ty + 14, 0xFFFFFF14u);
			ty += 15;
			any = true;
		}
		if (!any) _canvas.text(Canvas::W / 2, ty + 10, tr("Умений пока нет"), TEXT_DIM, ui::TEXT, ui::CENTER, 0, true);
		_canvas.text(Canvas::W / 2, Canvas::H - 16, tr("X или касание — характеристики"), TEXT_DIM, ui::TEXT, ui::CENTER, Canvas::W - 8, true);
		return;
	}
	int y = BARS_H + 6;
	_canvas.text(Canvas::W / 2, y, s.name[0] ? s.name : "—", TITLE, ui::TITLE, ui::CENTER, 0, true);
	y += 20;
	if (s.guild[0]) _canvas.text(Canvas::W / 2, y, s.guild, TEXT_DIM, ui::TEXT, ui::CENTER, 0, true);
	y += 18;
	const auto row = [&](int x, int w, int& yy, const char* label, const char* value, u32 colour) {
		_canvas.text(x, yy, label, TEXT_DIM, ui::TEXT, ui::LEFT, w - 40, true);
		_canvas.text(x + w, yy, value, colour, ui::TEXT, ui::RIGHT, 0, true);
		_canvas.fill(x, yy + 13, x + w, yy + 14, 0xFFFFFF14u);
		yy += 15;
	};
	char buf[48];
	int yl = y, yr = y;
	const int lx = 8, lw = 146, rx = 166, rw = 146;
	snprintf(buf, sizeof(buf), "%ld", static_cast<long>(s.level));
	row(lx, lw, yl, tr("Уровень"), buf, TITLE);
	snprintf(buf, sizeof(buf), "%ld / %ld", static_cast<long>(s.exp), static_cast<long>(s.expNext));
	row(lx, lw, yl, tr("Опыт"), buf, TEXT);
	snprintf(buf, sizeof(buf), "%ld", static_cast<long>(s.learn));
	row(lx, lw, yl, tr("Очки обучения"), buf, s.learn > 0 ? GOLD : TEXT);
	snprintf(buf, sizeof(buf), "%ld", static_cast<long>(s.strength));
	row(lx, lw, yl, tr("Сила"), buf, TEXT);
	snprintf(buf, sizeof(buf), "%ld", static_cast<long>(s.dexterity));
	row(lx, lw, yl, tr("Ловкость"), buf, TEXT);
	snprintf(buf, sizeof(buf), "%ld / %ld", static_cast<long>(v.hp), static_cast<long>(v.hpMax));
	row(lx, lw, yl, tr("Здоровье"), buf, TEXT);
	snprintf(buf, sizeof(buf), "%ld / %ld", static_cast<long>(v.mana), static_cast<long>(v.manaMax));
	row(lx, lw, yl, tr("Мана"), buf, TEXT);
	_canvas.text(rx, yr, tr("Защита"), TITLE, ui::TEXT, ui::LEFT, 0, true);
	yr += 15;
	static const char* const kProt[4] = {"от оружия", "от стрел", "от огня", "от магии"};
	for (int k = 0; k < 4; ++k) {
		snprintf(buf, sizeof(buf), "%ld", static_cast<long>(s.protection[k]));
		row(rx, rw, yr, tr(kProt[k]), buf, TEXT);
	}
	yr += 4;
	_canvas.text(rx, yr, tr("Снаряжение"), TITLE, ui::TEXT, ui::LEFT, 0, true);
	yr += 15;
	const char* worn[3] = {s.melee, s.ranged, s.armor};
	static const char* const kWorn[3] = {"Оружие:", "Дальнее:", "Броня:"};
	for (int k = 0; k < 3; ++k) {
		_canvas.text(rx, yr, tr(kWorn[k]), TEXT_DIM, ui::TEXT, ui::LEFT, 0, true);
		_canvas.text(rx + 56, yr, worn[k][0] ? worn[k] : "—", TEXT, ui::TEXT, ui::LEFT, rw - 56, true);
		yr += 15;
	}
	if (s.talentCount) _canvas.text(Canvas::W / 2, Canvas::H - 16, tr("X или касание — умения"), TEXT_DIM, ui::TEXT, ui::CENTER, Canvas::W - 8, true);
}

} // namespace g3
