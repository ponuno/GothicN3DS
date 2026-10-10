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
constexpr u32 PILL = 0xC4C8CCFFu, PILL_TEXT = 0x1C2024FFu, PILL_OFF = 0x5C6064FFu;
constexpr u32 TEXT = 0xD8D6D0FFu, TEXT_DIM = 0x9C9A92FFu, TITLE = 0xEAE6DAFFu, WARN = 0xE2604EFFu, GOLD = 0xE8C77AFFu;
constexpr u32 HP = 0xB0261EFFu, HP_LIGHT = 0xD8584AFFu, MANA = 0x2A4CBCFFu, MANA_LIGHT = 0x5A7CE8FFu;

const char* const kArrows[QUICK_SLOTS] = {"\xE2\x86\x91", "\xE2\x86\x92", "\xE2\x86\x93", "\xE2\x86\x90"};

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

// ---- the panel ----

void InventoryPanel::setCursor(bool on) {
	if (_cursor == on) return;
	_cursor = on;
	_assigning = false;
	_inspect = on ? kInspectTime : 0.0f;
	_dirty = true;
}

void InventoryPanel::setTab(ItemTab t) {
	if (t >= ITEM_TABS || t == _tab) return;
	_tab = u8(t);
	_scroll = 0;
	_dirty = true;
}

void InventoryPanel::refilter(const InventoryView& v) {
	if (_filteredVersion == v.version && _filteredTab == _tab) return;
	_filteredVersion = v.version;
	_filteredTab = _tab;
	_shownCount = 0;
	for (u32 i = 0; i < v.count; ++i)
		if (_tab == TAB_ALL || tabOf(v.items[i].kind) == _tab) _shown[_shownCount++] = i;
	_scroll = clamp(_scroll, 0, maxScroll());
	_dirty = true;
}

const ItemEntry* InventoryPanel::entryOf(const InventoryView& v, u32 symbol) const {
	for (u32 i = 0; i < v.count; ++i)
		if (v.items[i].symbol == symbol) return &v.items[i];
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

void InventoryPanel::crossCentre(int slot, int& x, int& y) {
	static const int kDx[QUICK_SLOTS] = {0, 1, 0, -1}, kDy[QUICK_SLOTS] = {-1, 0, 1, 0};
	x = GRID_X + 2 * CELL_W + kDx[slot] * CROSS_CELL;
	y = GRID_Y + GRID_H / 2 + kDy[slot] * CROSS_CELL;
}

void InventoryPanel::pageCentre(int page, int& x, int& y) {
	x = PILL_W + 2 + page * TAB_W + TAB_W / 2;
	y = TABS_Y + TABS_H / 2;
}

InventoryPanel::Hit InventoryPanel::hitTest(int x, int y) const {
	Hit h;
	if (_assigning) {
		h.region = R_CROSS;
		for (int s = 0; s < QUICK_SLOTS; ++s) {
			int cx, cy;
			crossCentre(s, cx, cy);
			if (abs(x - cx) <= CROSS_CELL / 2 && abs(y - cy) <= CROSS_CELL / 2) h.index = s;
		}
		return h;
	}
	if (y < BARS_H) {
		h.region = R_BARS;
	} else if (y >= TABS_Y - 2) {
		if (x < PILL_W + 2) h.region = R_PREV_TAB;
		else if (x >= PILL_W + 2 + PAGES * TAB_W) h.region = R_NEXT_TAB;
		else {
			h.region = R_PAGE;
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
	refilter(v);
	if (_flash > 0.0f && (_flash -= dt) <= 0.0f) _dirty = true;
	if (_inspect > 0.0f && !_cursor && !_assigning) _inspect -= dt;
	const ItemEntry* sel = entryOf(v, _selected);
	if (_assigning && !(sel && (sel->flags & ITEMF_QUICK))) {
		_assigning = false; // (used up meanwhile)
		_dirty = true;
	}
	const auto use = [&](const ItemEntry* e) {
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
	const auto assign = [&](int slot) {
		_assigning = false;
		_dirty = true;
		_inspect = kInspectTime;
		if (!sel || !(sel->flags & ITEMF_QUICK)) return;
		// The same thing again in its own slot: the slot is emptied.
		cmd.type = v.quick[slot] == sel->symbol ? u8(PANEL_CLEAR_SLOT) : u8(PANEL_ASSIGN);
		cmd.slot = u8(slot);
		cmd.symbol = sel->symbol;
	};

	// ---- the buttons (after SELECT) ----
	if (_cursor) {
		if (!sel && _shownCount) choose(v.items[_shown[0]].symbol);
		int at = -1;
		for (u32 i = 0; i < _shownCount; ++i)
			if (v.items[_shown[i]].symbol == _selected) at = int(i);
		const bool dirs[QUICK_SLOTS] = {in.up, in.right, in.down, in.left};
		if (_assigning) {
			for (int s = 0; s < QUICK_SLOTS; ++s)
				if (dirs[s]) {
					assign(s);
					break;
				}
		} else if (_shownCount && (in.up || in.down || in.left || in.right)) {
			int n = at < 0 ? 0 : at;
			if (in.left && n % COLS > 0) --n;
			if (in.right && n % COLS < COLS - 1 && u32(n + 1) < _shownCount) ++n;
			if (in.up && n >= COLS) n -= COLS;
			if (in.down) {
				if (u32(n + COLS) < _shownCount) n += COLS;
				else if (u32((n / COLS + 1) * COLS) < _shownCount) n = int(_shownCount) - 1; // (to the last, shorter row)
			}
			choose(v.items[_shown[n]].symbol);
			keepInSight(u32(n));
		}
		if (in.prevTab) setTab(ItemTab((_tab + ITEM_TABS - 1) % ITEM_TABS));
		if (in.nextTab) setTab(ItemTab((_tab + 1) % ITEM_TABS));
		if (in.confirm && !_assigning) use(sel);
		if (in.assign && sel && (sel->flags & ITEMF_QUICK)) {
			_assigning = !_assigning;
			_dirty = true;
		}
		if (in.back) {
			if (_assigning) _assigning = false;
			else {
				setCursor(false);
				cmd.type = PANEL_CURSOR_OFF;
			}
			_dirty = true;
		}
		if (in.up || in.down || in.left || in.right || in.prevTab || in.nextTab) _inspect = kInspectTime;
	}

	// ---- the touch screen ----
	if (in.touching && !_touching) {
		_touching = true;
		_start = hitTest(in.x, in.y);
		// Closed, the grid and the pages only show; a touch on them opens the
		// inventory (the character stands meanwhile), and this touch does no more than choose.
		_opening = !_cursor && (_start.region == R_TILE || _start.region == R_GRID || _start.region == R_PREV_TAB ||
		                        _start.region == R_NEXT_TAB || (_start.region == R_PAGE && _start.index < ITEM_TABS));
		if (_opening) setCursor(true);
		_startX = _x = in.x;
		_startY = _y = in.y;
		_startScroll = _scroll;
		_held = 0;
		_moved = _longDone = false;
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
		if (!_moved && !_longDone && _held > 0.5f && _start.region == R_TILE) {
			// Held on a thing: chosen, and the cross of the quick slots comes up.
			_longDone = true;
			choose(v.items[_shown[_start.index]].symbol);
			if (sel && (sel->flags & ITEMF_QUICK)) _assigning = true;
		}
	} else if (_touching) {
		_touching = false;
		const Hit end = hitTest(_x, _y);
		if (_start.region == R_CROSS) {
			if (end.region == R_CROSS && end.index >= 0 && end.index == _start.index) assign(end.index);
			else if (end.index < 0 && _start.index < 0) {
				_assigning = false; // (a tap beside the cross: never mind)
				_dirty = true;
			}
		} else if (_moved) {
			// A scroll ends on a whole row.
			_scroll = clamp((_scroll + CELL_H / 2) / CELL_H * CELL_H, 0, maxScroll());
			_dirty = true;
		} else if (!_longDone && end.region == _start.region && end.index == _start.index) {
			switch (end.region) {
			case R_PAGE:
				if (end.index == ITEM_TABS) cmd.type = PANEL_REPORT;
				else setTab(ItemTab(end.index));
				break;
			case R_BARS:
				// A tap on the bars closes the open inventory.
				if (_cursor) {
					setCursor(false);
					cmd.type = PANEL_CURSOR_OFF;
				}
				break;
			case R_PREV_TAB: setTab(ItemTab((_tab + ITEM_TABS - 1) % ITEM_TABS)); break;
			case R_NEXT_TAB: setTab(ItemTab((_tab + 1) % ITEM_TABS)); break;
			case R_TILE: {
				const ItemEntry& e = v.items[_shown[end.index]];
				if (e.symbol == _selected && _inspect > 0.0f && !_opening) use(&e);
				else choose(e.symbol);
				break;
			}
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

bool InventoryPanel::draw(const InventoryView& v, bool force) {
	refilter(v);
	if (!_canvas.ready() || (!force && !_dirty && v.version == _drawnVersion)) return false;
	_dirty = false;
	_drawnVersion = v.version;
	_canvas.unclip();
	_canvas.gradient(0, 0, Canvas::W, Canvas::H, BG_TOP, BG_BOTTOM);
	drawBars(v);
	drawGrid(v);
	drawTabs(v);
	if (_cursor) {
		// Open: a light edge round the grid (the character stands meanwhile).
		_canvas.frame(GRID_X - 1, GRID_Y - 1, GRID_X + COLS * CELL_W + 2, GRID_Y + GRID_H + 2, 0xE8ECF0C0u);
	}
	if (_assigning) drawCross(v);
	return true;
}

void InventoryPanel::drawBars(const InventoryView& v) {
	// Health on the left, mana on the right: framed bars, as on the console's screen.
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
	bar(9, 152, v.hp, v.hpMax, HP, HP_LIGHT);
	bar(168, 311, v.mana, v.manaMax, MANA, MANA_LIGHT);
	if (v.combat) _canvas.fill(156, 7, 164, 13, 0xC8463CFFu); // (in a fight: armour and rings stay)
}

void InventoryPanel::drawTabs(const InventoryView&) {
	const int x0 = 2, x1 = Canvas::W - 2, y0 = TABS_Y, y1 = TABS_Y + TABS_H;
	_canvas.gradient(x0, y0, x1, y1, TAB_TOP, TAB_BOTTOM);
	_canvas.fill(x0, y0, x1, y0 + 1, TAB_LIGHT);
	_canvas.fill(x0, y1 - 1, x1, y1, TAB_DARK);
	for (int t = 0; t < PAGES; ++t) {
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
		const u32 c = _cursor ? PILL : PILL_OFF;
		_canvas.fill(cx - 8, cy - 6, cx + 8, cy + 6, c);
		_canvas.fill(cx - 9, cy - 5, cx + 9, cy + 5, c);
		_canvas.text(cx, cy - 7, k ? "R" : "L", PILL_TEXT, ui::TEXT, ui::CENTER);
	}
}

void InventoryPanel::drawGrid(const InventoryView& v) {
	const int x0 = GRID_X, y0 = GRID_Y, x1 = GRID_X + COLS * CELL_W, y1 = GRID_Y + GRID_H;
	_canvas.clip(x0, y0, x1 + 1, y1 + 1);
	const int rows = max(int((_shownCount + COLS - 1) / COLS), ROWS_SHOWN);
	const int firstRow = _scroll / CELL_H;
	for (int r = firstRow; r < rows && r * CELL_H - _scroll < GRID_H; ++r)
		for (int c = 0; c < COLS; ++c) {
			const u32 n = u32(r * COLS + c);
			const int x = x0 + c * CELL_W, y = y0 + r * CELL_H - _scroll;
			const ItemEntry* e = n < _shownCount ? &v.items[_shown[n]] : nullptr;
			const bool chosen = e && e->symbol == _selected;
			cell(x, y, CELL_W, CELL_H, chosen, e && _flash > 0.0f && e->symbol == _flashSymbol);
			if (!e) continue;
			const bool dim = (e->flags & ITEMF_BLOCKED) && !(e->flags & ITEMF_EQUIPPED);
			_canvas.icon(x + (CELL_W - 32) / 2, y + (CELL_H - 32) / 2, e->icon, e->tint, 2, dim ? Canvas::ICON_DIM : Canvas::ICON_NORMAL);
			if (e->flags & ITEMF_BUSY) {
				const int h = int(float(CELL_H - 2) * v.busyShare);
				_canvas.fill(x + 1, y + CELL_H - h, x + CELL_W, y + CELL_H, 0xE8C77A50u);
			}
			if (e->flags & ITEMF_EQUIPPED) {
				// Worn: a gold corner; in the hands: a gold line along the bottom too.
				for (int k = 0; k < 7; ++k) _canvas.fill(x + 1, y + 1 + k, x + 8 - k, y + 2 + k, GOLD);
				if (e->flags & ITEMF_IN_HAND) _canvas.fill(x + 3, y + CELL_H - 3, x + CELL_W - 2, y + CELL_H - 1, GOLD);
			}
			for (int q = 0; q < QUICK_SLOTS; ++q)
				if (e->quick & (1 << q)) {
					_canvas.text(x + CELL_W - 4, y + 1, kArrows[q], 0xD8DCE0C0u, ui::TEXT, ui::RIGHT, 0, true);
					break;
				}
			if (e->count > 1 || dim) {
				char n2[16];
				snprintf(n2, sizeof(n2), "%ld", static_cast<long>(e->count));
				_canvas.text(x + CELL_W - 4, y + CELL_H - 14, n2, dim ? WARN : TEXT, ui::TEXT, ui::RIGHT, 0, true);
			}
		}
	// The thin grey lines over it all, as on the console.
	for (int c = 0; c <= COLS; ++c) _canvas.fill(x0 + c * CELL_W, y0, x0 + c * CELL_W + 1, y1 + 1, CELL_LINE);
	for (int r = firstRow; r <= rows && r * CELL_H - _scroll <= GRID_H; ++r) {
		const int y = y0 + r * CELL_H - _scroll;
		_canvas.fill(x0, y, x1 + 1, y + 1, CELL_LINE);
	}
	_canvas.fill(x0, y1, x1 + 1, y1 + 1, CELL_LINE);
	for (u32 n = 0; n < _shownCount; ++n) {
		// (The chosen cell's white edge stays on top of the lines.)
		if (v.items[_shown[n]].symbol != _selected) continue;
		const int x = x0 + int(n % COLS) * CELL_W, y = y0 + int(n / COLS) * CELL_H - _scroll;
		_canvas.frame(x, y, x + CELL_W + 1, y + CELL_H + 1, SEL_EDGE);
	}
	if (maxScroll() > 0) {
		const int h = max(GRID_H * GRID_H / (GRID_H + maxScroll()), 12);
		const int y = y0 + (GRID_H - h) * _scroll / maxScroll();
		_canvas.fill(x1 - 3, y + 1, x1 - 1, y + h, 0xE8ECF0A0u);
	}
	if (_shownCount == 0)
		_canvas.text((x0 + x1) / 2, y0 + GRID_H / 2 - 7, v.present ? kTabNames[_tab] : "Инвентаря нет", TEXT_DIM, ui::TEXT, ui::CENTER, 0,
		             true);
	_canvas.unclip();
}

void InventoryPanel::drawCross(const InventoryView& v) {
	// Over the grid, darkened: the four slots round the chosen thing.
	_canvas.fill(GRID_X, GRID_Y, GRID_X + COLS * CELL_W + 1, GRID_Y + GRID_H + 1, 0x000000B0u);
	const ItemEntry* sel = entryOf(v, _selected);
	int cx, cy;
	crossCentre(0, cx, cy);
	cy += CROSS_CELL; // the middle
	const int half = CROSS_CELL / 2 - 2;
	_canvas.disc(cx, cy, 20, 0x3C4148FFu);
	if (sel) _canvas.icon(cx - 16, cy - 16, sel->icon, sel->tint, 2);
	for (int s = 0; s < QUICK_SLOTS; ++s) {
		int x, y;
		crossCentre(s, x, y);
		const bool pressed = _touching && _start.region == R_CROSS && _start.index == s;
		_canvas.gradient(x - half, y - half, x + half, y + half, pressed ? SEL_TOP : 0x23282EFFu, pressed ? SEL_BOTTOM : 0x15181CFFu);
		_canvas.frame(x - half, y - half, x + half + 1, y + half + 1, sel && v.quick[s] == sel->symbol ? GOLD : SEL_EDGE);
		if (v.quick[s] != SCRIPT_NONE) _canvas.icon(x - 16, y - 16, v.quickIcon[s], v.quickTint[s], 2, Canvas::ICON_DIM);
		_canvas.text(x, y - 8, kArrows[s], TITLE, ui::TITLE, ui::CENTER, 0, true);
	}
	_canvas.text(GRID_X + COLS * CELL_W / 2, GRID_Y + 3, "В какой слот? (R + направление)", TEXT, ui::TEXT, ui::CENTER, 0, true);
}

} // namespace g3
