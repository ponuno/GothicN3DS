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
#include "talk.h"

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
	ITEMF_NOSELL = 1 << 6,   // not for trade (the money itself): the trade page leaves it out
};

// The quick slots: ten, as the number keys of the PC original, on the bottom
// screen to tap. Anything with an action may go there - a potion is drunk,
// a weapon taken in hand, armour put on.
constexpr int QUICK_SLOTS = 10;
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
	i32 price;     // on the trade page: what it costs to buy / what is paid for it (else 0)
	u8 kind, icon, flags;
	u16 quick;     // bit k: in quick slot k
	i16 picture;   // the baked picture (ItemPictures), -1 for none: the drawn icon then
	u32 tint;      // the picture's colour, 0xRRGGBBAA
	char name[48]; // UTF-8
	// The info box: the scripts' own lines (C_ITEM.TEXT / COUNT) where the
	// item has them, as the original shows them; else made up from its numbers.
	ItemRow rows[ITEM_ROWS];
	u8 rowCount;
	char action[32];           // "Выпить", "Надеть" ... ("" if nothing; fix192: room for "Использовать")
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
	i16 quickPicture[QUICK_SLOTS] = {};
	u32 equip[EQUIP_SLOTS] = {};   // item symbols; SCRIPT_NONE for nothing
	u8 equipIcon[EQUIP_SLOTS] = {};
	u32 equipTint[EQUIP_SLOTS] = {};
	char message[96] = "";  // the last refusal or news
	float messageTime = 0;  // seconds it still shows
	u32 count = 0;
	ItemEntry items[INVENTORY_MAX];
	// A container the character is at (a chest): what is in it, and the lock
	// that is being picked.
	enum { CHEST_MAX = 40 };
	struct Chest {
		bool open = false;       // its contents are shown
		bool lock = false;       // its lock is being picked
		bool trade = false;      // it is a trader's wares (the trade page), not a chest
		i32 gold = 0;            // trade: the money the character has
		char currency[24] = "";   // ... and what it is called (the item's name in the scripts)
		char name[40] = "";
		u32 count = 0;
		ItemEntry items[CHEST_MAX];
		u8 length = 0, progress = 0, picks = 0; // the lock: letters of the combination, got right, picks left
		char text[64] = "";      // news of the lock
	} chest;
	// The character's page.
	struct Sheet {
		char name[48] = "";
		char guild[48] = "";
		i32 level = 0, exp = 0, expNext = 0, learn = 0;
		i32 strength = 0, dexterity = 0;
		i32 protection[4] = {}; // against weapons, arrows, fire, magic
		char melee[48] = "", ranged[48] = "", armor[48] = "";
		// The talents the scripts name (TXT_TALENTS), with the level taught (TXT_TALENTS_SKILLS) and the
		// talent's value (the chance of a critical blow, of a lock that holds ...), in per cent.
		enum { TALENTS = 16 };
		struct Talent {
			char name[32] = "";
			char level[32] = "";
			i32 skill = 0, value = 0;
		} talents[TALENTS];
		u32 talentCount = 0;
	} sheet;
	// The diary (Log_CreateTopic, Log_AddEntry): its topics, newest first,
	// each with its entries.
	enum { DIARY_TOPICS = 40, DIARY_ENTRIES = 96 };
	struct Topic {
		char name[64];
		u8 status;    // LOG_RUNNING 1, LOG_SUCCESS 2, LOG_FAILED 3, LOG_OBSOLETE 4 (0: none given)
		u8 section;   // LOG_MISSION 0, LOG_NOTE 1
		bool fresh;   // written to since the diary was last looked at
		u16 first, count; // into entries
	};
	char chapter[48] = "", chapterSub[64] = ""; // the chapter the story is in (the last card), as the scripts word it
	i32 chapterNumber = 0;                      // the scripts' Kapitel, 0 if none
	Topic topics[DIARY_TOPICS];
	u32 topicCount = 0;
	char entries[DIARY_ENTRIES][200];
	u32 entryCount = 0;
	TalkView talk;          // the talk in progress (the dialogue page)
	struct Doc {
		bool open = false;    // a book, a letter or a map is being read (the reading page)
		bool map = false;
		u32 page = 0, pages = 0;
	} doc;
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
	// fix192: Y let go (one of the chosen thing put down) or held (the whole stack).
	bool drop = false, dropAll = false;
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
	PANEL_CHEST_MOVE,   // `amount` of `symbol` from the chest to the pack (slot 0) or from the pack to the chest (slot 1)
	PANEL_CHEST_ALL,    // everything out of the chest
	PANEL_CHEST_CLOSE,  // done with the chest
	PANEL_LOCK_PICK,    // the pick turned left (slot 0) or right (slot 1)
	PANEL_LOCK_CANCEL,  // give up on the lock
	PANEL_TALK_CHOOSE,  // way `slot` of the talk's menu
	PANEL_TALK_SKIP,    // on to the next line
	PANEL_TALK_LEAVE,   // B: the way that leaves (the last one), or the line skipped
	PANEL_DOC_PAGE,     // reading: `slot` 0 the page before, 1 the next (past the last: closed)
	PANEL_DOC_CLOSE,    // reading: put it down
	PANEL_DROP,         // fix192: `amount` of `symbol` put down on the ground
	PANEL_MAP,          // fix192: a map: `slot` 0 smaller, 1 larger, 2..5 moved left, right, up, down
	// (The trade page uses PANEL_CHEST_MOVE: `slot` 0 buys, 1 sells, `amount` of `symbol`; PANEL_CHEST_CLOSE ends it.)
};
struct PanelCommand {
	u8 type = PANEL_NONE;
	u8 slot = 0;
	u32 symbol = 0;
	i32 amount = 0;
};

} // namespace g3
