// The bottom screen: the inventory, drawn by the processor into a picture of
// its own (Canvas) that the platform copies to the screen - the touch screen
// of the 3DS, a PNG on the PC. The 3D picture and its overlay are the top
// screen's; this one costs the graphics chip nothing and is drawn again only
// when something on it changes.
//
// The look is that of Gothic Classic's console inventory: cells with thin
// grey lines over a dark ground, the chosen one blue with a white edge, a
// blue strip of pages with L and R at its ends, framed bars of health and
// mana. What the chosen thing is (the info box of "what ... number" lines),
// the quick slots round the D-Pad and the button hints are drawn on the top
// screen, where the eyes are in a fight - and only while they are wanted
// (App::drawItemHud).
//
// Layout, 320 x 240 pixels:
//   top       health (red) and mana (blue)
//   grid      the things, 4 in a row, 5 rows in sight: a picture and how many;
//             a gold corner on what is worn, a gold line under the weapon in hand
//   bottom    the pages between L and R (everything, weapons, armour, magic,
//             potions, food, the rest) and the numbers of the report
//
// As in the original, the inventory may be used in a fight, but it costs:
// while it is open the character stands still - no steps, no blows - and the
// world goes on. Closed, the screen only shows. SELECT or a touch on the grid
// or the pages opens it; SELECT, B or a tap on the bars closes it. (The quick
// slots, R + D-Pad, work without opening it, as the console's do.)
//
// Touch: a tap chooses a thing (its info box shows on the top screen), a tap
// on the chosen one uses it; drag the grid up and down to scroll it; hold a
// thing to put it into a quick slot (a cross of four arrows comes up: tap
// one). Buttons: the D-Pad chooses, L / R change the page, A uses, X and then
// a direction put the chosen thing into a slot, B closes.
#pragma once
#include "inventory.h"

namespace g3 {

// A picture of the bottom screen's size, RGB565 (as the 3DS shows it), rows
// from the top. Colours are given as 0xRRGGBBAA; alpha below 255 blends.
class Canvas {
public:
	static constexpr int W = 320, H = 240;
	Canvas() = default;
	~Canvas() { release(); }
	Canvas(const Canvas&) = delete;
	Canvas& operator=(const Canvas&) = delete;

	bool init();
	void release();
	bool ready() const { return _px != nullptr; }
	const u16* pixels() const { return _px; }
	u16 pixel(int x, int y) const { return _px && x >= 0 && y >= 0 && x < W && y < H ? _px[y * W + x] : 0; }

	void clear(u32 rgba);
	void fill(int x0, int y0, int x1, int y1, u32 rgba);
	void frame(int x0, int y0, int x1, int y1, u32 rgba, int width = 1);
	// Text (UTF-8) with its top at y; `maxWidth` > 0 cuts it short with "…".
	// Returns the width drawn.
	int text(int x, int y, const char* utf8, u32 rgba, int size = 0, int align = 0, int maxWidth = 0, bool shadow = false);
	static int measure(const char* utf8, int size = 0);
	// A picture (ItemIcon) `scale` times as large, the item's colour `tint`.
	enum IconMode : u8 {
		ICON_NORMAL,
		ICON_DIM,   // greyed: something the character cannot use
		ICON_MONO,  // all of it in `tint` (its alpha too), no outline: the tabs' white signs
	};
	void icon(int x, int y, u8 icon, u32 tint, int scale, int mode = ICON_NORMAL);
	// From `top` at y0 to `bottom` at y1.
	void gradient(int x0, int y0, int x1, int y1, u32 top, u32 bottom);
	void disc(int cx, int cy, int r, u32 rgba);
	// Drawing stays within this rectangle (x1, y1 not included) until reset.
	void clip(int x0, int y0, int x1, int y1);
	void unclip() { clip(0, 0, W, H); }

	static u16 rgb565(u32 rgba) {
		return u16(((rgba >> 16) & 0xF800) | ((rgba >> 13) & 0x07E0) | ((rgba >> 11) & 0x001F));
	}

private:
	void blend(int x, int y, u32 rgba, int alpha);
	u16* _px = nullptr;
	int _cx0 = 0, _cy0 = 0, _cx1 = W, _cy1 = H;
};

class InventoryPanel {
public:
	// Where things are (for the platform and the tests).
	static constexpr int BARS_H = 20;
	static constexpr int GRID_X = 2, GRID_Y = 22, CELL_W = 79, CELL_H = 38, COLS = 4, ROWS_SHOWN = 5;
	static constexpr int GRID_H = CELL_H * ROWS_SHOWN; // 190: down to 212
	static constexpr int TABS_Y = 215, TABS_H = 23, PILL_W = 22, TAB_W = 34;
	static constexpr int PAGES = ITEM_TABS + 1; // the last: the report
	// The cross of quick slots that comes up to put a thing into one.
	static constexpr int CROSS_CELL = 52;

	bool init() { return _canvas.init(); }
	void release() { _canvas.release(); }

	// Reads the touch screen (and the buttons, while they are the panel's)
	// and says what to do.
	PanelCommand update(const PanelInput& in, const InventoryView& view, float dt);
	// Draws the panel again if anything shown has changed (or `force`);
	// true if it did.
	bool draw(const InventoryView& view, bool force = false);
	const Canvas& canvas() const { return _canvas; }
	void invalidate() { _dirty = true; }

	// The inventory is open: the buttons are the panel's, the character
	// stands (SELECT, or a touch on the grid or the pages, opens it; SELECT,
	// B or a tap on the bars closes it). Closed it only shows.
	bool cursor() const { return _cursor; }
	bool open() const { return _cursor; }
	void setCursor(bool on);
	// Choosing a slot for the chosen thing (the cross is up).
	bool assigning() const { return _assigning; }

	ItemTab tab() const { return ItemTab(_tab); }
	void setTab(ItemTab t);
	u32 selected() const { return _selected; }
	void select(u32 symbol) {
		_selected = symbol;
		_inspect = kInspectTime;
		_dirty = true;
	}
	// Seconds the chosen thing's info box still shows on the top screen.
	float inspecting() const { return _inspect; }
	// What is in the grid, in order: indices into the view's items.
	u32 shownCount() const { return _shownCount; }
	u32 shown(u32 i) const { return _shown[i]; }
	int scroll() const { return _scroll; }
	// The middle of the n-th cell of the grid as it is scrolled now (y may be off the grid).
	void tileCentre(u32 n, int& x, int& y) const;
	// The middle of the arrow of quick slot `slot` in the cross.
	static void crossCentre(int slot, int& x, int& y);
	// The middle of page `page` (PAGES - 1: the report) in the strip.
	static void pageCentre(int page, int& x, int& y);

private:
	static constexpr float kInspectTime = 4.0f;
	enum Region : u8 { R_NONE, R_PAGE, R_PREV_TAB, R_NEXT_TAB, R_TILE, R_GRID, R_BARS, R_CROSS };
	struct Hit {
		u8 region = R_NONE;
		int index = -1; // the page, the slot, the tile (into _shown)
	};
	Hit hitTest(int x, int y) const;
	void refilter(const InventoryView& v);
	const ItemEntry* entryOf(const InventoryView& v, u32 symbol) const;
	void keepInSight(u32 index);
	int maxScroll() const;
	void drawBars(const InventoryView& v);
	void drawTabs(const InventoryView& v);
	void drawGrid(const InventoryView& v);
	void drawCross(const InventoryView& v);
	void cell(int x, int y, int w, int h, bool chosen, bool lit);

	Canvas _canvas;
	u8 _tab = TAB_ALL;
	int _scroll = 0;
	u32 _selected = 0xFFFFFFFFu;
	u32 _shown[INVENTORY_MAX] = {};
	u32 _shownCount = 0;
	u32 _filteredVersion = 0xFFFFFFFFu;
	u8 _filteredTab = 0xFF;
	u32 _drawnVersion = 0xFFFFFFFFu;
	bool _dirty = true;
	bool _cursor = false;
	bool _assigning = false; // the cross is up: the next direction (or tap) picks the slot
	float _inspect = 0;
	// The touch in progress.
	bool _touching = false;
	Hit _start;
	int _startX = 0, _startY = 0, _startScroll = 0, _x = 0, _y = 0;
	float _held = 0;
	bool _moved = false, _longDone = false;
	bool _opening = false; // this touch opened the inventory
	float _flash = 0; // a cell just used: lit for a moment
	u32 _flashSymbol = 0xFFFFFFFFu;
};

} // namespace g3
