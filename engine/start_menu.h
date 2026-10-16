// The start menu, as the original's: what the game's menu script (MENU.DAT, baked into "script/menu") says
// MENU_MAIN is - its items, their texts and the line of help under each, where they stand (the game's 8192 x 8192
// grid), its frame - over the game's start screen (STARTSCREEN), as OpenGothic builds it (GameMenu). The layout is
// the original's 640 x 480 at half its size on the top screen. Without a menu script in the archive the engine's own
// items stand in, laid out the same way.
//
// What an item does is what its script says it does (onSelAction / onSelAction_S): NEW_GAME, LEAVE_GAME, the menus
// of loading, saving and the options (the port's own pages stand in for those), BACK (the original's "continue" -
// here the quick save, there being no game running yet). What the port has no counterpart for (the intro video,
// the credits) is shown dimmed, as an item that cannot be chosen.
#pragma once
#include "g3ds/pak.h"
#include "gfx.h"
#include "menu.h"
#include "ui.h"

namespace g3 {

enum StartAction : u8 { START_NONE, START_NEW_GAME, START_CONTINUE, START_LOAD, START_SAVE, START_OPTIONS, START_EXIT };

// A picture of the archive's "pic/<name>" (g3ds/pic.h) as a texture of the overlay.
struct UiPicture {
	gfx::Texture* tex = nullptr;
	u16 width = 0, height = 0; // of the picture in the texture
	u16 origW = 0, origH = 0;  // of the game's texture
	bool loaded() const { return tex != nullptr; }
};
// `name` as the game names it ("STARTSCREEN.TGA") or already short ("startscreen"). False if the archive lacks it.
bool loadUiPicture(Pak& pak, const char* name, UiPicture& out);
void freeUiPicture(UiPicture& pic);
// Over x0, y0 .. x1, y1 (stretched, as the game stretches its pictures over their rectangles).
void drawUiPicture(Ui& ui, const UiPicture& pic, int x0, int y0, int x1, int y1, u32 tint = 0xFFFFFFFFu);

class StartMenu {
public:
	static constexpr int MAX_ITEMS = 32;
	// The script's flags (zenkit's MenuFlag / MenuItemFlag).
	enum : u32 { MENU_DONT_SCALE_DIM = 1u << 3, MENU_ALIGN_CENTER = 1u << 5, MENU_SHOW_INFO = 1u << 6 };
	enum : u32 { IT_SELECTABLE = 1u << 2, IT_CENTERED = 1u << 4, IT_DISABLED = 1u << 5, IT_ONLY_OUTGAME = 1u << 8, IT_ONLY_INGAME = 1u << 9 };
	struct Item {
		char text[64] = "";  // UTF-8
		char info[128] = ""; // the line of help (text[1]), shown at the bottom while it is selected
		i32 posx = 0, posy = 0, dimx = -1, dimy = -1;
		u32 flags = 0;
		u8 action = START_NONE;
		int slot = 0; // its place in MENU_MAIN's items (what defaultOutGame counts)
		UiPicture picture;
	};

	~StartMenu() { release(); }
	// From the archive's menu script; the engine's own items if it has none. The pictures are read here too.
	void load(Pak& pak);
	void release();
	bool loaded() const { return _loaded; }
	bool fromScript() const { return _fromScript; }
	int count() const { return _count; }
	const Item& item(int i) const { return _items[i]; }
	bool hasBackground() const { return _back.loaded(); }
	bool hasFrame() const { return _frame.loaded(); }

	// `canContinue`: there is a quick save to go on from (the original's "continue" needs a game running).
	void open(bool canContinue);
	void close() { _open = false; }
	bool isOpen() const { return _open; }
	int selected() const { return _sel; }
	bool enabled(int i) const;
	// Up and down go from item to item that can be chosen (round), A does what the selected one does.
	// `moved`: the selection changed (for the menu's sound).
	StartAction update(const MenuInput& in, bool* moved = nullptr);
	// The start screen (whole screen) - drawn under the port's own pages too.
	void drawBackground(Ui& ui) const;
	// The window, its items, the line of help, `version` in its corner.
	void draw(Ui& ui, const char* version) const;
	// Where the window lies on the top screen.
	void window(int& x, int& y, int& w, int& h) const;
	// Where item i lies (and its text's top), for tests.
	void itemRect(int i, int& x, int& y, int& w, int& h) const;

private:
	void builtIn();
	void select(int from, int step);

	Item _items[MAX_ITEMS];
	int _count = 0;
	i32 _posx = 0, _posy = 0, _dimx = 8192, _dimy = 8192;
	u32 _flags = 0;
	int _defaultOut = 0;
	UiPicture _back, _frame;
	bool _loaded = false, _fromScript = false, _open = false, _canContinue = false;
	int _sel = -1;
};

} // namespace g3
