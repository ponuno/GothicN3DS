// The player's inventory: what the character carries, as the bottom screen
// shows it (panel.h), and the rules of using it.
//
// The rules are Gothic's own, and nothing stops the game for them:
//  * The world goes on while the inventory is looked at and used - it is on
//    the bottom screen all the time, the game never pauses for it.
//  * Using a potion or food takes the character's time: a weapon in hand is
//    put away first (its animation), then the item is brought to the mouth
//    as the model script has it ("T_POTIONFAST_STAND_2_S0" ...), and only
//    there does it work (its script function ON_STATE[0]). Meanwhile the
//    character stands: it can neither walk, turn, strike nor fend off. A
//    blow that makes it reel before the item is at its mouth spoils the use
//    (the item is kept). Afterwards the weapon is taken in hand again - with
//    its animation, if the menu says so.
//  * Until the item is at the mouth the use can be called off (a blow, the
//    weapon key); from then on the character is committed.
//  * Armour, rings and amulets may be changed in a fight, as in the original:
//    what it costs is the time the inventory is open (the character stands).
//  * What an item takes (C_ITEM.COND_ATR/COND_VALUE: strength ...) it takes.
//  * One thing at a time: a second request waits for the first (one waits,
//    the newest), a request for what is already being used is ignored.
#pragma once
#include "g3ds/base.h"

namespace g3 {

// What a thing is, as the inventory sorts and treats it.
enum ItemKind : u8 {
	IK_MELEE,
	IK_RANGED,
	IK_AMMO,
	IK_ARMOR,
	IK_AMULET,
	IK_RING,
	IK_RUNE,
	IK_SCROLL,
	IK_POTION,
	IK_FOOD,
	IK_DOC,
	IK_LIGHT,
	IK_KEY,
	IK_MISC,
	ITEM_KINDS
};

// The pages of the inventory (its tabs).
enum ItemTab : u8 { TAB_ALL, TAB_WEAPONS, TAB_ARMOR, TAB_MAGIC, TAB_POTIONS, TAB_FOOD, TAB_OTHER, ITEM_TABS };
// The page a kind of thing is on (besides TAB_ALL).
ItemTab tabOf(u8 kind);

// The pictures of things (panel.cpp draws them).
enum ItemIcon : u8 {
	ICON_SWORD,
	ICON_AXE,
	ICON_CLUB,
	ICON_GREATSWORD,
	ICON_BOW,
	ICON_CROSSBOW,
	ICON_ARROW,
	ICON_ARMOR,
	ICON_AMULET,
	ICON_RING,
	ICON_RUNE,
	ICON_SCROLL,
	ICON_POTION,
	ICON_APPLE,
	ICON_MEAT,
	ICON_BREAD,
	ICON_BOTTLE,
	ICON_PLANT,
	ICON_BOWL,
	ICON_LETTER,
	ICON_MAP,
	ICON_BOOK,
	ICON_TORCH,
	ICON_KEY,
	ICON_ORE,
	ICON_TROPHY,
	ICON_BAG,
	// The tabs' own.
	ICON_ALL,
	ICON_WEAPONS,
	ICON_MAGIC,
	ICON_OTHER,
	ICON_COUNT
};

enum ItemEntryFlags : u8 {
	ITEMF_EQUIPPED = 1 << 0, // worn, or the weapon on the belt
	ITEMF_IN_HAND = 1 << 1,  // the weapon in the hands right now
	ITEMF_BLOCKED = 1 << 2,  // the character lacks what it takes
	ITEMF_ACTION = 1 << 3,   // there is something to do with it (`action`)
	ITEMF_QUICK = 1 << 4,    // may go into a quick slot
	ITEMF_BUSY = 1 << 5,     // being used right now
};

constexpr int QUICK_SLOTS = 4; // R + D-Pad up, right, down, left
constexpr u32 INVENTORY_MAX = 120;

// A line of the info box, as Gothic lays it out: what, and its number.
struct ItemRow {
	char label[40]; // UTF-8
	char value[12]; // "" for none
	u8 warn;        // the character lacks it (shown red)
};
constexpr int ITEM_ROWS = 5;

// A line of the inventory: one kind of thing and how many.
struct ItemEntry {
	u32 symbol;
	i32 count;
	i32 value;
	u8 kind, icon, flags;
	u8 quick;      // bit k: in quick slot k
	u32 tint;      // the picture's colour, 0xRRGGBBAA
	char name[48]; // UTF-8
	// The info box: the scripts' own lines (C_ITEM.TEXT / COUNT) where the
	// item has them, as the original shows them; else made up from its numbers.
	ItemRow rows[ITEM_ROWS];
	u8 rowCount;
	char action[16];           // "Выпить", "Надеть" ... ("" if nothing)
};

// The equipment, as the left of the panel shows it.
enum EquipSlot : u8 { EQUIP_ARMOR, EQUIP_AMULET, EQUIP_RING1, EQUIP_RING2, EQUIP_MELEE, EQUIP_RANGED, EQUIP_SLOTS };

// Everything the bottom screen shows, made by App::inventory().
struct InventoryView {
	bool present = false; // a character with scripts: there is an inventory
	i32 hp = 0, hpMax = 0, mana = 0, manaMax = 0;
	i32 ore = 0;          // the ore nuggets carried (the money of the colony)
	bool combat = false;  // the character is in a fight (for the reports)
	bool busy = false;    // something is being used (or waits for the hands)
	float busyShare = 0;  // how far, 0..1
	char busyText[96] = "";
	u32 quick[QUICK_SLOTS] = {};   // item symbols; SCRIPT_NONE for an empty slot
	i32 quickCount[QUICK_SLOTS] = {};
	u8 quickIcon[QUICK_SLOTS] = {};
	u32 quickTint[QUICK_SLOTS] = {};
	char quickName[QUICK_SLOTS][48] = {};
	u32 equip[EQUIP_SLOTS] = {};   // item symbols; SCRIPT_NONE for nothing
	u8 equipIcon[EQUIP_SLOTS] = {};
	u32 equipTint[EQUIP_SLOTS] = {};
	char message[96] = "";  // the last refusal or news
	float messageTime = 0;  // seconds it still shows
	u32 count = 0;
	ItemEntry items[INVENTORY_MAX];
	u32 version = 0;        // changes whenever anything shown changes
};

// What the platform hands the panel each frame.
struct PanelInput {
	bool touching = false; // the stylus or a finger is down
	int x = 0, y = 0;      // where, bottom-screen pixels
	// The buttons, pressed this frame (with key repeat for the directions).
	bool up = false, down = false, left = false, right = false;
	bool confirm = false, back = false, assign = false; // A, B, X
	bool prevTab = false, nextTab = false;              // L, R
};

// What the panel asks the application to do.
enum PanelCommandType : u8 {
	PANEL_NONE,
	PANEL_USE,          // the item's action (drink, eat, put on, take off)
	PANEL_USE_SLOT,     // the quick slot `slot`
	PANEL_ASSIGN,       // put `symbol` into quick slot `slot`
	PANEL_CLEAR_SLOT,   // empty quick slot `slot`
	PANEL_REPORT,       // the numbers instead of the inventory
	PANEL_CURSOR_OFF,   // the buttons go back to the game
	PANEL_SELECT,       // (internal) nothing for the application
};
struct PanelCommand {
	u8 type = PANEL_NONE;
	u8 slot = 0;
	u32 symbol = 0;
};

} // namespace g3
