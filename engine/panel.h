// The bottom screen, drawn by the processor into a picture of its own
// (Canvas) that the platform copies to the screen - the touch screen of the
// 3DS, a PNG on the PC. It costs the graphics chip nothing and is drawn again
// only when something on it changes.
//
// The look follows Gothic Classic's console interface: framed bars of health
// and mana, cells with thin grey lines over a dark ground, the chosen one
// blue with a white edge, a blue strip of pages with L and R at its ends; the
// things are shown by pictures of their own models (baked: "icon/<visual>").
//
// Pages:
//   play       (always, while nothing is open) health and mana; the ten
//              quick slots, 1 to 0 as the number keys of the PC original -
//              tap one to use what is in it; three buttons: the inventory,
//              the diary, the character
//   inventory  the things, 5 in a row, 4 rows in sight; the pages of kinds
//              (everything, weapons, armour, magic, potions, food, the rest)
//              and the report's numbers
//   diary      the topics the scripts wrote (Log_CreateTopic, Log_AddEntry)
//   character  level, experience, attributes, protection, what is worn
//   chest      (opened at a chest) its things or the pack's, one side at a
//              time; a tap chooses, a second tap moves the thing over to the
//              other side; buttons: the chest, the pack, take everything
//   lock       (a locked chest) the pick: left or right, as the combination goes
//   dialogue   (a talk) the ways to go on, one under the other: the D-Pad and A,
//              or a tap; B takes the last, which by the scripts' habit leaves.
//              While a line is said it shows the line; A or a tap goes on
// While a page is open the character stands still - as in the original the
// inventory may be used in a fight, at that price. SELECT or the buttons open
// them; SELECT, B or the back arrow at the top close them.
//
// Inventory, touch: a tap chooses a thing (its info box and turning model show
// on the top screen), a tap on the chosen one uses it; drag the grid to
// scroll; hold a thing (or X) and the ten slots come up: tap one to put it
// there. Buttons: the D-Pad chooses, L / R change the page, A uses, X puts
// into a slot (then the D-Pad and A pick it), B closes.
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
	// A picture of RGB565 and coverage, drawn `w` x `h` (scaled to it, its
	// shape kept and centred); `dim` greys it.
	void picture(int x, int y, int w, int h, const u16* rgb, const u8* alpha, int pw, int ph, bool dim = false);
	// Text broken into lines of at most `width` pixels; returns the height taken.
	int paragraph(int x, int y, int width, const char* utf8, u32 rgba, int maxLines = 0);
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

// The pictures of the things (baked "icon/<visual>"), kept as the bottom
// screen draws them: RGB565 and a coverage of 0..255.
struct ItemPicture {
	u16 visual = 0; // the GameText of the visual it shows
	u16 w = 0, h = 0;
	u16* rgb = nullptr;
	u8* alpha = nullptr;
};

class ItemPictures {
public:
	enum { CAPACITY = 192 };
	ItemPictures() = default;
	~ItemPictures() { release(); }
	ItemPictures(const ItemPictures&) = delete;
	ItemPictures& operator=(const ItemPictures&) = delete;
	// The picture of `visual`, -1 if it is not here (yet).
	int find(u16 visual) const;
	// Keeps a picture from RGBA8 rows; -1 if full or out of memory.
	int add(u16 visual, int w, int h, const u8* rgba);
	const ItemPicture* get(int i) const { return i >= 0 && u32(i) < _count ? &_pics[i] : nullptr; }
	u32 count() const { return _count; }
	void release();

private:
	ItemPicture _pics[CAPACITY];
	u32 _count = 0;
};

class InventoryPanel {
public:
	enum PanelPage : u8 { PAGE_PLAY, PAGE_INVENTORY, PAGE_DIARY, PAGE_CHARACTER, PAGE_CHEST, PAGE_LOCK, PAGE_DIALOG, PAGE_DOC, PANEL_PAGES };
	// Where things are (for the platform and the tests).
	static constexpr int BARS_H = 20;
	// The play page: two rows of five quick slots, three buttons under them.
	static constexpr int SLOT_X = 4, SLOT_Y = 26, SLOT_W = 63, SLOT_H = 60;
	static constexpr int BUTTON_Y = 154, BUTTON_H = 80, BUTTON_W = 98;
	// The inventory: 5 x 4 cells in sight, the strip of kinds under them.
	static constexpr int GRID_X = 0, GRID_Y = 22, CELL_W = 64, CELL_H = 48, COLS = 5, ROWS_SHOWN = 4;
	static constexpr int GRID_H = CELL_H * ROWS_SHOWN; // 192: down to 214
	static constexpr int TABS_Y = 215, TABS_H = 23, PILL_W = 22, TAB_W = 34;
	static constexpr int TAB_PAGES = ITEM_TABS + 1; // the last: the report
	// An open page's header: the back arrow.
	static constexpr int BACK_W = 44;

	bool init() { return _canvas.init(); }
	void release() { _canvas.release(); }
	void setPictures(const ItemPictures* pictures) {
		_pictures = pictures;
		_dirty = true;
	}

	// Reads the touch screen (and the buttons, while a page is open) and says what to do.
	PanelCommand update(const PanelInput& in, const InventoryView& view, float dt);
	// Draws the panel again if anything shown has changed (or `force`); true if it did.
	bool draw(const InventoryView& view, bool force = false);
	const Canvas& canvas() const { return _canvas; }
	void invalidate() { _dirty = true; }

	PanelPage page() const { return PanelPage(_page); }
	void openPage(PanelPage p);
	// A page is open: the buttons are the panel's, the character stands.
	bool open() const { return _page != PAGE_PLAY; }
	bool cursor() const { return open(); }
	void setCursor(bool on) { openPage(on ? PAGE_INVENTORY : PAGE_PLAY); }
	// Choosing a slot for the chosen thing (the ten slots are up).
	bool assigning() const { return _assigning; }
	// (fix166) ZL held: one of the ten slots lit as the D-Pad moves (-1: none).
	void setChoice(int slot) {
		if (slot != _choiceSlot) _dirty = true;
		_choiceSlot = slot;
	}
	int choice() const { return _choiceSlot; }

	ItemTab tab() const { return ItemTab(_tab); }
	void setTab(ItemTab t);
	u32 selected() const { return _selected; }
	void select(u32 symbol) {
		_selected = symbol;
		_inspect = kInspectTime;
		_dirty = true;
	}
	// Seconds the chosen thing's info box still shows on the top screen.
	float inspecting() const { return _page == PAGE_INVENTORY || _page == PAGE_CHEST ? _inspect : 0.0f; }
	// The chest page shows the chest's things (0) or the pack's (1).
	int side() const { return _side; }
	void setSide(int side);
	// The dialogue page: row `k` of the menu (the middle of its left part's y).
	static constexpr int TALK_Y = 32, TALK_ROW = 16;
	int talkCursor() const { return _talkCursor; }
	// The middle of the chest page's button `k` (0 the chest, 1 the pack, 2 take all) and of
	// the lock page's (0 left, 1 right).
	static void stripCentre(int k, int& x, int& y);
	static void lockCentre(int k, int& x, int& y);
	// What is in the grid, in order: indices into the view's items.
	u32 shownCount() const { return _shownCount; }
	u32 shown(u32 i) const { return _shown[i]; }
	int scroll() const { return _scroll; }
	// The middle of the n-th cell of the grid as it is scrolled now (y may be off the grid).
	void tileCentre(u32 n, int& x, int& y) const;
	// The middle of quick slot `slot` (on the play page, and in the picker).
	static void slotCentre(int slot, int& x, int& y);
	// fix192: the hold menu's drop buttons (under the ten slots): `k` 0 one of the thing, 1 all of them (a stack);
	// false if that button is not there (one thing: a single wide button, k 0).
	static constexpr int DROP_Y = 196, DROP_H = 38;
	static bool dropButton(int k, u32 count, int& x0, int& x1);
	// The middle of the play page's button `k` (0 inventory, 1 diary, 2 character).
	static void buttonCentre(int k, int& x, int& y);
	// The middle of kind page `page` (TAB_PAGES - 1: the report) in the strip.
	static void pageCentre(int page, int& x, int& y);

private:
	static constexpr float kInspectTime = 4.0f;
	enum Region : u8 { R_NONE, R_SLOT, R_BUTTON, R_BACK, R_KIND, R_PREV_TAB, R_NEXT_TAB, R_TILE, R_GRID, R_PICKER, R_LIST, R_BARS, R_STRIP, R_LOCK, R_TALK, R_DOC };
	struct Hit {
		u8 region = R_NONE;
		int index = -1;
	};
	Hit hitTest(int x, int y) const;
	void refilter(const InventoryView& v);
	// The things the grid shows: the pack's, or on the chest page's first side the chest's.
	const ItemEntry* items(const InventoryView& v) const { return _page == PAGE_CHEST && _side == 0 ? v.chest.items : v.items; }
	u32 itemCount(const InventoryView& v) const { return _page == PAGE_CHEST && _side == 0 ? v.chest.count : v.count; }
	bool gridPage() const { return _page == PAGE_INVENTORY || _page == PAGE_CHEST; }
	const ItemEntry* entryOf(const InventoryView& v, u32 symbol) const;
	void keepInSight(u32 index);
	int maxScroll() const;
	void drawBars(const InventoryView& v, bool header);
	void drawPlay(const InventoryView& v);
	void drawTabs(const InventoryView& v);
	void drawGrid(const InventoryView& v);
	void drawPicker(const InventoryView& v);
	void drawDiary(const InventoryView& v);
	void drawCharacter(const InventoryView& v);
	void drawStrip(const InventoryView& v);
	void drawLock(const InventoryView& v);
	void drawDialog(const InventoryView& v);
	void drawDoc(const InventoryView& v);
	void drawThing(int x, int y, int w, int h, i16 picture, u8 icon, u32 tint, bool dim);
	void slotTile(int slot, const InventoryView& v, bool picker);
	void cell(int x, int y, int w, int h, bool chosen, bool lit);

	Canvas _canvas;
	const ItemPictures* _pictures = nullptr;
	const InventoryView* _view = nullptr; // (fix192) the view of the last update, for hit tests that need the chosen thing
	u8 _page = PAGE_PLAY;
	u8 _tab = TAB_ALL;
	u8 _side = 0;
	int _scroll = 0;
	int _listScroll = 0; // the diary's
	int _topic = -1;     // the diary's topic opened
	int _talkCursor = 0; // the dialogue page's chosen way
	int _talkTop = 0;    // fix222: the first way shown (the list scrolls)
	u32 _talkVersion = 0xFFFFFFFFu;
	int _topicY[InventoryView::DIARY_TOPICS] = {}; // where each topic was drawn (for taps)
	u32 _selected = 0xFFFFFFFFu;
	u8 _talentPage = 0;         // fix214: which page of the talents (more than a page of them: X turns the page)
	static constexpr int TALENT_ROW = 14, TALENT_TOP = BARS_H + 26;
	static int talentsPerPage() { return (Canvas::H - 22 - TALENT_TOP) / TALENT_ROW; }
	void turnTalents(u32 count);
	bool _talentsShown = false; // the character page shows the talents instead of the numbers (X, or a tap)
	u32 _shown[INVENTORY_MAX] = {};
	u32 _shownCount = 0;
	u32 _filteredVersion = 0xFFFFFFFFu;
	u8 _filteredTab = 0xFF;
	u8 _filteredPage = 0xFF, _filteredSide = 0xFF;
	u32 _drawnVersion = 0xFFFFFFFFu;
	bool _dirty = true;
	float _talkScroll = 0.0f; // fix202: seconds the chosen dialogue line has been running along
	int _talkShift = 0;       // ... and how far it has run (pixels)
	bool _assigning = false; // the ten slots are up: the next tap (or the D-Pad and A) picks one
	int _pickerSlot = 0;
	int _choiceSlot = -1; // (fix166) ZL held: the quick slot the D-Pad points at on the play page
	float _inspect = 0;
	// The touch in progress.
	bool _touching = false;
	Hit _start;
	int _startX = 0, _startY = 0, _startScroll = 0, _x = 0, _y = 0;
	float _held = 0;
	bool _moved = false, _longDone = false;
	float _flash = 0; // a slot or cell just used: lit for a moment
	int _flashSlot = -1;
	u32 _flashSymbol = 0xFFFFFFFFu;
};

} // namespace g3
