#include "lang.h"
#include "start_menu.h"
#include "g3ds/pic.h"
#include "script.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace g3 {

namespace {

constexpr float GRID = 8192.0f;   // the script's coordinates
constexpr float LAYOUT_W = 320.0f; // the original's 640 x 480, at half its size
constexpr float LAYOUT_H = 240.0f;

// The script's onSelAction values (zenkit's MenuItemSelectAction).
enum : i32 { SEL_BACK = 1, SEL_START_MENU = 2, SEL_CLOSE = 4 };

// "MENU_INGAME.TGA" -> "menu_ingame"
void shortName(const char* in, char* out, u32 size) {
	const char* base = in;
	for (const char* p = in; *p; ++p)
		if (*p == '/' || *p == '\\') base = p + 1;
	u32 n = 0;
	for (const char* p = base; *p && *p != '.' && n + 1 < size; ++p) out[n++] = char(tolower(u8(*p)));
	out[n] = 0;
}

bool startsWith(const char* s, const char* prefix) {
	for (; *prefix; ++s, ++prefix)
		if (toupper(u8(*s)) != *prefix) return false;
	return true;
}

// What an item does, from its onSelAction / onSelAction_S.
u8 actionOf(const i32* act, const char* const* what) {
	for (int k = 0; k < 5; ++k) {
		const char* s = what[k] ? what[k] : "";
		if (act[k] == SEL_CLOSE) {
			if (startsWith(s, "NEW_GAME")) return START_NEW_GAME;
			if (startsWith(s, "LEAVE_GAME")) return START_EXIT;
		} else if (act[k] == SEL_START_MENU) {
			if (startsWith(s, "MENU_SAVEGAME_LOAD")) return START_LOAD;
			if (startsWith(s, "MENU_SAVEGAME_SAVE")) return START_SAVE;
			if (startsWith(s, "MENU_OPTIONS")) return START_OPTIONS;
			if (startsWith(s, "MENU_LEAVE_GAME")) return START_EXIT;
		} else if (act[k] == SEL_BACK) {
			return START_CONTINUE;
		}
	}
	return START_NONE;
}

// fix194: does START_MENU <menu> lead (within `depth` menus) to an item that starts a new game?
bool subMenuStartsGame(ScriptVm& vm, const i32* act, const char* const* what, int depth) {
	if (depth <= 0) return false;
	const u32 mItems = vm.findMember("C_MENU", "ITEMS");
	const u32 iAct = vm.findMember("C_MENU_ITEM", "ONSELACTION"), iActS = vm.findMember("C_MENU_ITEM", "ONSELACTION_S");
	if (mItems == SCRIPT_NONE || iAct == SCRIPT_NONE || iActS == SCRIPT_NONE) return false;
	for (int k = 0; k < 5; ++k) {
		if (act[k] != SEL_START_MENU || !what[k] || !*what[k]) continue;
		char up[64];
		u32 n = 0;
		for (const char* p = what[k]; *p && n + 1 < sizeof(up); ++p) up[n++] = char(toupper(u8(*p)));
		up[n] = 0;
		const u32 sym = vm.find(up);
		if (sym == SCRIPT_NONE || vm.symbol(sym).type != SCRIPT_INSTANCE) continue;
		ScriptInstance* menu = vm.create(sym);
		if (!menu) continue;
		bool found = false;
		for (u32 j = 0; j < 150 && !found; ++j) {
			const char* itemName = vm.getString(mItems, j, menu);
			if (!itemName || !*itemName) continue;
			char iu[96];
			u32 m = 0;
			for (const char* p = itemName; *p && m + 1 < sizeof(iu); ++p) iu[m++] = char(toupper(u8(*p)));
			iu[m] = 0;
			const u32 isym = vm.find(iu);
			ScriptInstance* in = isym != SCRIPT_NONE ? vm.create(isym) : nullptr;
			if (!in) continue;
			i32 a[5];
			char buf[5][48];
			const char* w[5];
			for (u32 q = 0; q < 5; ++q) {
				a[q] = vm.getInt(iAct, q, in);
				const char* s = vm.getString(iActS, q, in);
				snprintf(buf[q], sizeof(buf[q]), "%s", s ? s : "");
				w[q] = buf[q];
			}
			vm.destroy(in);
			found = actionOf(a, w) == START_NEW_GAME || subMenuStartsGame(vm, a, w, depth - 1);
		}
		vm.destroy(menu);
		if (found) return true;
	}
	return false;
}

} // namespace

bool loadUiPicture(Pak& pak, const char* name, UiPicture& out) {
	freeUiPicture(out);
	char shortened[64], entry[80];
	shortName(name ? name : "", shortened, sizeof(shortened));
	if (!shortened[0]) return false;
	snprintf(entry, sizeof(entry), "pic/%s", shortened);
	const PakEntry* e = pak.find(entry);
	if (!e || e->size < sizeof(PicHeader)) return false;
	u8* blob = static_cast<u8*>(pak.readAlloc(*e));
	if (!blob) return false;
	PicHeader h;
	memcpy(&h, blob, sizeof(h));
	const auto pot = [](u32 v) { return v >= 8 && v <= 1024 && (v & (v - 1)) == 0; };
	if (h.magic == PIC_MAGIC && h.version == PIC_VERSION && pot(h.texW) && pot(h.texH) && h.width <= h.texW && h.height <= h.texH &&
	    e->size >= sizeof(h) + size_t(h.texW) * h.texH * 2) {
		const u16* px = reinterpret_cast<const u16*>(blob + sizeof(h));
		out.tex = h.format == PIC_RGBA4 ? gfx::uiTextureCreateRGBA4(h.texW, h.texH, px) : gfx::uiTextureCreateColor(h.texW, h.texH, px);
		out.width = h.width;
		out.height = h.height;
		out.origW = h.origW ? h.origW : h.width;
		out.origH = h.origH ? h.origH : h.height;
	}
	free(blob);
	return out.tex != nullptr;
}

void freeUiPicture(UiPicture& pic) {
	if (pic.tex) gfx::textureDestroy(pic.tex);
	pic = UiPicture();
}

void drawUiPicture(Ui& ui, const UiPicture& pic, int x0, int y0, int x1, int y1, u32 tint) {
	if (pic.tex) ui.picture(pic.tex, pic.width, pic.height, x0, y0, x1, y1, tint);
}

// ---- the menu ----

void StartMenu::release() {
	for (int i = 0; i < _count; ++i) freeUiPicture(_items[i].picture);
	freeUiPicture(_back);
	freeUiPicture(_frame);
	_count = 0;
	_loaded = _fromScript = _open = false;
	_sel = -1;
}

void StartMenu::load(Pak& pak) {
	release();
	_loaded = true;
	loadUiPicture(pak, "startscreen", _back);
	ScriptVm vm;
	const u32 main = vm.load(pak, "script/menu") ? vm.find("MENU_MAIN") : SCRIPT_NONE;
	ScriptInstance* m = main != SCRIPT_NONE ? (vm.setQuiet(true), vm.create(main)) : nullptr;
	if (!m) {
		builtIn();
		vm.release();
		return;
	}
	const auto member = [&](const char* cls, const char* name) { return vm.findMember(cls, name); };
	const auto integer = [&](u32 sym, ScriptInstance* in, i32 fallback, u32 index = 0) { return sym != SCRIPT_NONE ? vm.getInt(sym, index, in) : fallback; };
	const auto text = [&](u32 sym, ScriptInstance* in, u32 index = 0) -> const char* {
		const char* s = sym != SCRIPT_NONE ? vm.getString(sym, index, in) : nullptr;
		return s ? s : "";
	};
	const u32 mBack = member("C_MENU", "BACKPIC"), mItems = member("C_MENU", "ITEMS"), mFlags = member("C_MENU", "FLAGS");
	const u32 mPosX = member("C_MENU", "POSX"), mPosY = member("C_MENU", "POSY"), mDimX = member("C_MENU", "DIMX"), mDimY = member("C_MENU", "DIMY");
	const u32 mOut = member("C_MENU", "DEFAULTOUTGAME");
	const u32 iText = member("C_MENU_ITEM", "TEXT"), iBack = member("C_MENU_ITEM", "BACKPIC"), iFlags = member("C_MENU_ITEM", "FLAGS");
	const u32 iPosX = member("C_MENU_ITEM", "POSX"), iPosY = member("C_MENU_ITEM", "POSY"), iDimX = member("C_MENU_ITEM", "DIMX"),
	          iDimY = member("C_MENU_ITEM", "DIMY");
	const u32 iAct = member("C_MENU_ITEM", "ONSELACTION"), iActS = member("C_MENU_ITEM", "ONSELACTION_S");
	_fromScript = true;
	loadUiPicture(pak, text(mBack, m), _frame);
	_posx = integer(mPosX, m, 0);
	_posy = integer(mPosY, m, 0);
	_dimx = integer(mDimX, m, 8192);
	_dimy = integer(mDimY, m, 8192);
	_flags = u32(integer(mFlags, m, 0));
	_defaultOut = integer(mOut, m, 0);
	char name[96];
	for (u32 k = 0; mItems != SCRIPT_NONE && k < 150 && _count < MAX_ITEMS; ++k) {
		const char* itemName = text(mItems, m, k);
		if (!*itemName) continue;
		u32 n = 0;
		for (const char* p = itemName; *p && n + 1 < sizeof(name); ++p) name[n++] = char(toupper(u8(*p)));
		name[n] = 0;
		const u32 sym = vm.find(name);
		ScriptInstance* in = sym != SCRIPT_NONE ? vm.create(sym) : nullptr;
		if (!in) continue;
		Item& it = _items[_count++];
		it = Item();
		it.slot = int(k);
		scriptText(text(iText, in, 0), it.text, sizeof(it.text));
		scriptText(text(iText, in, 1), it.info, sizeof(it.info));
		it.posx = integer(iPosX, in, 0);
		it.posy = integer(iPosY, in, 0);
		it.dimx = integer(iDimX, in, -1);
		it.dimy = integer(iDimY, in, -1);
		it.flags = u32(integer(iFlags, in, 0));
		i32 act[5];
		const char* what[5];
		char whatBuf[5][48];
		for (u32 a = 0; a < 5; ++a) {
			act[a] = integer(iAct, in, 0, a);
			snprintf(whatBuf[a], sizeof(whatBuf[a]), "%s", text(iActS, in, a));
			what[a] = whatBuf[a];
		}
		it.action = actionOf(act, what);
		// fix194: a mod's "New Game" may open a menu of its own first (START_MENU MENU_XXX whose item is CLOSE NEW_GAME, as
		// OpenGothic follows any START_MENU) or run a script function: look one or two menus deep for NEW_GAME, and at last
		// go by the item's own name (MENU_ITEM_NEW_GAME). Without this Archolos' New Game came out disabled.
		if (it.action == START_NONE) {
			if (subMenuStartsGame(vm, act, what, 2)) it.action = START_NEW_GAME;
			else if (strstr(name, "NEW_GAME") || strstr(name, "NEWGAME")) it.action = START_NEW_GAME;
		}
		loadUiPicture(pak, text(iBack, in), it.picture);
		vm.destroy(in);
	}
	vm.destroy(m);
	vm.release();
	if (!_count) builtIn();
}

// The engine's own: Gothic's main menu as its script lays it out, in the language of the interface.
void StartMenu::builtIn() {
	_fromScript = false;
	_posx = _posy = 0;
	_dimx = _dimy = 8192;
	_flags = MENU_DONT_SCALE_DIM | MENU_ALIGN_CENTER | MENU_SHOW_INFO;
	_defaultOut = 0;
	struct Def {
		const char* text;
		const char* info;
		u8 action;
		u32 flags;
	};
	static const Def kDefs[] = {
	    {"Новая игра", "Начать новое приключение.", START_NEW_GAME, 0},
	    {"Загрузить игру", "Загрузить сохранённую игру.", START_LOAD, 0},
	    {"Сохранить игру", "Сохранить текущую игру.", START_SAVE, IT_ONLY_INGAME},
	    {"Продолжить", "Продолжить с быстрого сохранения.", START_CONTINUE, IT_ONLY_INGAME},
	    {"Настройки", "Графика, звук, управление.", START_OPTIONS, 0},
	    {"Выход", "Выйти из игры.", START_EXIT, 0},
	};
	_count = 0;
	for (const Def& d : kDefs) {
		Item& it = _items[_count];
		it = Item();
		it.slot = _count;
		snprintf(it.text, sizeof(it.text), "%s", tr(d.text));
		snprintf(it.info, sizeof(it.info), "%s", tr(d.info));
		it.posy = 2900 + 600 * _count;
		it.flags = IT_SELECTABLE | IT_CENTERED | d.flags;
		it.action = d.action;
		++_count;
	}
}

bool StartMenu::enabled(int i) const {
	if (i < 0 || i >= _count) return false;
	const Item& it = _items[i];
	if (!(it.flags & IT_SELECTABLE) || (it.flags & IT_DISABLED) || !it.text[0]) return false;
	if (it.action == START_NONE || it.action == START_SAVE) return false; // (nothing of the port's for it; no game to save)
	if (it.action == START_CONTINUE) return _canContinue;
	if (it.flags & IT_ONLY_INGAME) return false; // (no game is running yet)
	return true;
}

void StartMenu::select(int from, int step) {
	_sel = -1;
	for (int k = 0; k < _count; ++k) {
		const int i = ((from + step * k) % _count + _count) % _count;
		if (enabled(i)) {
			_sel = i;
			return;
		}
	}
}

void StartMenu::open(bool canContinue) {
	_open = true;
	_canContinue = canContinue;
	// The script's default counts the places of MENU_MAIN's items (the headline among them).
	int from = 0;
	for (int i = 0; i < _count; ++i)
		if (_items[i].slot == _defaultOut) from = i;
	select(from, 1);
}

StartAction StartMenu::update(const MenuInput& in, bool* moved) {
	if (moved) *moved = false;
	if (!_open || _count == 0) return START_NONE;
	const int before = _sel;
	if (in.up) select(_sel < 0 ? 0 : _sel - 1, -1);
	if (in.down) select(_sel < 0 ? 0 : _sel + 1, 1);
	if (moved) *moved = _sel != before;
	if (in.confirm && enabled(_sel)) return StartAction(_items[_sel].action);
	return START_NONE;
}

void StartMenu::window(int& x, int& y, int& w, int& h) const {
	const float sw = float(gfx::SCREEN_W), sh = float(gfx::SCREEN_H);
	if (_flags & MENU_DONT_SCALE_DIM) {
		w = int(float(_dimx) * LAYOUT_W / GRID + 0.5f);
		h = int(float(_dimy) * LAYOUT_H / GRID + 0.5f);
	} else {
		w = int(float(_dimx) * sw / GRID + 0.5f);
		h = int(float(_dimy) * sh / GRID + 0.5f);
	}
	if (_flags & MENU_ALIGN_CENTER) {
		x = (gfx::SCREEN_W - w) / 2;
		y = (gfx::SCREEN_H - h) / 2;
	} else {
		x = int(float(_posx) * LAYOUT_W / GRID);
		y = int(float(_posy) * LAYOUT_H / GRID);
	}
}

void StartMenu::itemRect(int i, int& x, int& y, int& w, int& h) const {
	int wx, wy, ww, wh;
	window(wx, wy, ww, wh);
	const Item& it = _items[i];
	const i32 dx = it.dimx != -1 ? it.dimx : 8192, dy = it.dimy != -1 ? it.dimy : 750;
	x = wx + int(float(ww) * float(it.posx) / GRID);
	y = wy + int(float(wh) * float(it.posy) / GRID);
	w = int(float(ww) * float(dx) / GRID);
	h = int(float(wh) * float(dy) / GRID);
}

void StartMenu::drawBackground(Ui& ui) const {
	if (_back.loaded()) {
		drawUiPicture(ui, _back, 0, 0, gfx::SCREEN_W, gfx::SCREEN_H);
	} else {
		// (No picture of the game's: a dark sky over a glow.)
		ui.rect(0, 0, gfx::SCREEN_W, gfx::SCREEN_H, 0x0E0B08FFu);
		ui.rect(0, gfx::SCREEN_H * 2 / 3, gfx::SCREEN_W, gfx::SCREEN_H, 0x2A140AFFu);
	}
}

void StartMenu::draw(Ui& ui, const char* version) const {
	int wx, wy, ww, wh;
	window(wx, wy, ww, wh);
	if (_frame.loaded()) {
		drawUiPicture(ui, _frame, wx, wy, wx + ww, wy + wh);
	} else {
		ui.rect(wx, wy, wx + ww, wy + wh, 0x1A1714E8u);
		ui.frame(wx, wy, wx + ww, wy + wh, 0x8C7A50FFu);
		ui.frame(wx + 3, wy + 3, wx + ww - 3, wy + wh - 3, 0x5A4E36FFu);
		if (!_fromScript) ui.text(wx + ww / 2, wy + int(float(wh) * 0.17f), "GOTHIC", 0xC8C0B0FFu, ui::TITLE, ui::CENTER);
	}
	const int lh = Ui::lineHeight(ui::TEXT);
	for (int i = 0; i < _count; ++i) {
		const Item& it = _items[i];
		int x, y, w, h;
		itemRect(i, x, y, w, h);
		if (it.picture.loaded()) drawUiPicture(ui, it.picture, x, y, x + w, y + h);
		if (!it.text[0]) continue;
		const bool on = i == _sel;
		const u32 colour = !enabled(i) ? 0x8A7F6A90u : on ? 0xFFFFFFFFu : 0xD2C5A2FFu;
		if (it.flags & IT_CENTERED) ui.text(x + w / 2, y + (h - lh) / 2, it.text, colour, ui::TEXT, ui::CENTER);
		else ui.text(x, y, it.text, colour, ui::TEXT, ui::LEFT);
	}
	if ((_flags & MENU_SHOW_INFO) && _sel >= 0 && _items[_sel].info[0])
		ui.text(wx + ww / 2, wy + wh - lh - 3, _items[_sel].info, 0xC8BEA0FFu, ui::TEXT, ui::CENTER);
	// (Where the original writes its version: over the line of help, at the right.)
	if (version && *version) ui.text(wx + ww - 22, wy + wh - 16 - lh, version, 0x9A9080FFu, ui::TEXT, ui::RIGHT);
}

} // namespace g3
