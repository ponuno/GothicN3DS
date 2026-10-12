// The pause menu, drawn on the top screen over the stopped game: continue,
// the settings (graphics, controls, faults), saving the position, leaving.
// It changes Settings in place and tells the caller what else to do.
#pragma once
#include "settings.h"
#include "ui.h"

namespace g3 {

// Button presses for the menu (key repeat is the platform's business).
struct MenuInput {
	bool up = false, down = false, left = false, right = false;
	bool confirm = false; // A
	bool back = false;    // B (and START closes it from anywhere)
	bool close = false;   // START
};

enum MenuAction : u8 {
	MENU_NONE,
	MENU_CLOSED,       // back to the game
	MENU_CHANGED,      // a setting changed: apply it (and save settings.bin)
	MENU_SAVE,         // save the position (state.bin)
	MENU_LOAD,         // go back to the saved position
	MENU_EXIT,         // leave the program
	MENU_NEXT_WORLD,   // the faults page's
	MENU_HOUR,
	MENU_FLY,
	MENU_HEAL,
	MENU_GIVE_WEAPON,
	MENU_GIVE_ITEMS,
	MENU_RESET_GRAPHICS, // the graphics page's values back to the defaults
	MENU_GIVE_TESTKIT,   // the test kit: spells, stat potions, a weapon, a bow, an armour
};

class Menu {
public:
	enum Page : u8 { PAGE_MAIN, PAGE_GRAPHICS, PAGE_GAME, PAGE_CONTROLS, PAGE_INTERFACE, PAGE_SOUND, PAGE_DEBUG, PAGE_DEBUG_NPC, PAGE_DEBUG_HERO, PAGE_DEBUG_WORLD, PAGE_GFX_PLUS, PAGES };

	void open();
	void close() { _open = false; }
	bool isOpen() const { return _open; }
	Page page() const { return _page; }
	int selected() const { return _sel[_page]; }
	// The page B goes back to (the main one for most; the debug pages' parent is the debug page, Graphics+'s the graphics page).
	static Page parentOf(Page page);

	MenuAction update(const MenuInput& in, Settings& s);
	// `status`: a line under the title (the world, the time), may be null.
	// `message`: what the last action did ("Позиция сохранена"), may be null.
	void draw(Ui& ui, const Settings& s, const char* status, const char* message) const;

	// For tests: the label and the shown value of an item of the page.
	int itemCount(Page page) const;
	const char* label(Page page, int item) const;
	// "" if none; of a choice, the note of its value in `s` (null: the longest).
	const char* note(Page page, int item, const Settings* s = nullptr) const;
	void value(Page page, int item, const Settings& s, char* out, u32 size) const;

private:
	Page _page = PAGE_MAIN;
	int _sel[PAGES] = {};
	bool _open = false;
};

} // namespace g3
