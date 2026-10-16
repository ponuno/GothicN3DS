// The pause menu, drawn on the top screen over the stopped game: continue,
// the settings (graphics, controls, faults), saving and loading the game
// (ten slots and the quick one, as the original's), leaving.
// It changes Settings in place and tells the caller what else to do.
#pragma once
#include "saves.h"
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
	MENU_LEVEL_UP,       // one level more (the scripts' experience rules)
	MENU_GIVE_SPELLKIT,  // every rune and scroll, mana
	MENU_GIVE_CRAFTKIT,  // for the crafts: what the things ask for, ingredients
	MENU_LEARN_ALL,      // every talent and skill learnt
	MENU_GIVE_TESTKIT,   // the test kit: spells, stat potions, a weapon, a bow, an armour
	MENU_SAVE_SLOT,      // the save page: save into Menu::saveSlot() (1..SAVE_SLOTS)
	MENU_LOAD_SLOT,      // the load page: load Menu::saveSlot() (0 the quick save, 1..SAVE_SLOTS)
};

// What the save and load pages show (App fills it): the slots, and the picture of the one selected.
struct SaveMenuView {
	const SaveInfo* slots = nullptr; // SAVE_SLOTS + 1 of them, 0 the quick save
	gfx::Texture* picture = nullptr; // of the entry selected (THUMB_W x THUMB_H in the corner of its texture), or null
	bool current = false;            // the selected slot is empty on the save page: `now` and the picture are the game as it is
	SaveInfo now;                    // the game as it is (world, hour, time played)
};

class Menu {
public:
	enum Page : u8 { PAGE_MAIN, PAGE_GRAPHICS, PAGE_GAME, PAGE_CONTROLS, PAGE_INTERFACE, PAGE_SOUND, PAGE_DEBUG, PAGE_DEBUG_NPC, PAGE_DEBUG_HERO, PAGE_DEBUG_WORLD, PAGE_GFX_PLUS, PAGE_EXPERIMENTAL, PAGE_SAVE, PAGE_LOAD, PAGE_OPTIONS, PAGES };

	// `root`: the page it opens at, and the one B leaves it from (the pause menu: the main page; the start menu opens
	// the load page and the options this way, and B goes back to it).
	void open(Page root = PAGE_MAIN);
	Page root() const { return _root; }
	void close() { _open = false; }
	bool isOpen() const { return _open; }
	Page page() const { return _page; }
	int selected() const { return _sel[_page]; }
	void select(int item) { _sel[_page] = item; }
	// The save and load pages: a list of slots (not of items) and a picture.
	static bool savePage(Page page) { return page == PAGE_SAVE || page == PAGE_LOAD; }
	// Their entries: the save page the ten slots, the load page the quick save first.
	static int slotCount(Page page) { return page == PAGE_SAVE ? SAVE_SLOTS : page == PAGE_LOAD ? SAVE_SLOTS + 1 : 0; }
	// The slot of the entry selected (0 the quick save), -1 off those pages.
	int saveSlot() const { return _page == PAGE_SAVE ? _sel[_page] + 1 : _page == PAGE_LOAD ? _sel[_page] : -1; }
	static int slotOfEntry(Page page, int entry) { return page == PAGE_SAVE ? entry + 1 : entry; }
	// The page B goes back to (the main one for most; the debug pages' parent is the debug page, Graphics+'s the graphics page).
	static Page parentOf(Page page);

	MenuAction update(const MenuInput& in, Settings& s);
	// `status`: a line under the title (the world, the time), may be null.
	// `message`: what the last action did ("Позиция сохранена"), may be null.
	// `saves`: for the save and load pages (without it they show empty slots).
	void draw(Ui& ui, const Settings& s, const char* status, const char* message, const SaveMenuView* saves = nullptr) const;

	// For tests: the label and the shown value of an item of the page.
	int itemCount(Page page) const;
	const char* label(Page page, int item) const;
	// "" if none; of a choice, the note of its value in `s` (null: the longest).
	const char* note(Page page, int item, const Settings* s = nullptr) const;
	void value(Page page, int item, const Settings& s, char* out, u32 size) const;

private:
	void drawSaves(Ui& ui, const char* message, const SaveMenuView* saves) const;

	Page _page = PAGE_MAIN;
	Page _root = PAGE_MAIN;
	int _sel[PAGES] = {};
	bool _open = false;
};

} // namespace g3
