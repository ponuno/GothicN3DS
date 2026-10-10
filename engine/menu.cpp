#include "menu.h"

#include <stdio.h>
#include <string.h>

namespace g3 {

namespace {

enum Kind : u8 { ACTION, PAGE, TOGGLE, CHOICE, RANGE };

struct Item {
	const char* label;
	Kind kind;
	u8 Settings::*field;      // TOGGLE, CHOICE, RANGE
	const char* const* names; // CHOICE: one per value
	u8 count;                 // CHOICE: values; RANGE: step
	u8 lo, hi;                // RANGE
	const char* unit;         // RANGE
	u8 target;                // ACTION: a MenuAction; PAGE: a page
	const char* note;         // shown under the list while the item is selected
};

const char* const kObjects[] = {"Плавность", "Баланс", "Устойчиво"};
const char* const kFrames[] = {"60 (30 под нагрузкой)", "Ровно 30"};
const char* const kSchemes[] = {"Консольная", "Классическая"};

const Item kMain[] = {
    {"Продолжить", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_CLOSED, nullptr},
    {"Графика", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_GRAPHICS, nullptr},
    {"Управление", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_CONTROLS, nullptr},
    {"Отладка", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_DEBUG, nullptr},
    {"Сохранить позицию", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_SAVE, "То же: R + START в игре"},
    {"Загрузить позицию", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_LOAD, "То же: R + SELECT в игре"},
    {"Выйти в Homebrew Launcher", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_EXIT, "Позиция и настройки сохранятся"},
};

const Item kGraphics[] = {
    {"Дальность прорисовки", RANGE, &Settings::drawMetres, nullptr, 10, 40, 200, "м", 0, "Дальше — больше нагрузки на видеочип"},
    {"Мелкие объекты", CHOICE, &Settings::objects, kObjects, 3, 0, 0, nullptr, 0,
     "Меньше мелочи вдали — ровнее 60 к/с"},
    {"Растворение объектов", TOGGLE, &Settings::dissolve, nullptr, 0, 0, 0, nullptr, 0, "Вступит в силу после перезапуска"},
    {"Частота кадров", CHOICE, &Settings::frameRate, kFrames, 2, 0, 0, nullptr, 0, nullptr},
    {"Интерфейс в игре", TOGGLE, &Settings::hud, nullptr, 0, 0, 0, nullptr, 0, "Здоровье, цель, подсказки"},
    {"Отчёт на нижнем экране", TOGGLE, &Settings::report, nullptr, 0, 0, 0, nullptr, 0, nullptr},
};

const Item kControls[] = {
    {"Схема", CHOICE, &Settings::scheme, kSchemes, 2, 0, 0, nullptr, 0,
     "Стик ведёт героя от камеры, C-стик — камера"},
    {"Скорость камеры", RANGE, &Settings::cameraSpeed, nullptr, 1, 1, 10, "", 0, nullptr},
    {"Инверсия по вертикали", TOGGLE, &Settings::invertY, nullptr, 0, 0, 0, nullptr, 0, nullptr},
    {"Камера за героем", TOGGLE, &Settings::autoCamera, nullptr, 0, 0, 0, nullptr, 0, "Камера заходит за спину на бегу"},
    {"Автозахват цели", TOGGLE, &Settings::autoTarget, nullptr, 0, 0, 0, nullptr, 0, "Взял оружие — цель захвачена"},
    {"Вид от первого лица", TOGGLE, &Settings::firstPerson, nullptr, 0, 0, 0, nullptr, 0, nullptr},
};

const Item kDebug[] = {
    {"Восприятие жителей", TOGGLE, &Settings::perception, nullptr, 0, 0, 0, nullptr, 0, nullptr},
    {"Жизнь жителей", TOGGLE, &Settings::life, nullptr, 0, 0, 0, nullptr, 0, "Выкл: стоят на местах распорядка"},
    {"Драки жителей", TOGGLE, &Settings::fights, nullptr, 0, 0, 0, nullptr, 0, "Выкл: состояния атаки не начинаются"},
    {"Бессмертие", TOGGLE, &Settings::immortal, nullptr, 0, 0, 0, nullptr, 0, nullptr},
    {"Вылечить", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_HEAL, nullptr},
    {"Дать лучшее оружие", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_GIVE_WEAPON, "Из оружия, что есть в скриптах"},
    {"Полёт вкл/выкл", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_FLY, "Свободная камера без столкновений"},
    {"Следующий мир", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_NEXT_WORLD, nullptr},
    {"Время +1 час", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_HOUR, nullptr},
};

struct PageDef {
	const char* title;
	const Item* items;
	int count;
};

const PageDef kPages[Menu::PAGES] = {
    {"Пауза", kMain, int(sizeof(kMain) / sizeof(kMain[0]))},
    {"Графика", kGraphics, int(sizeof(kGraphics) / sizeof(kGraphics[0]))},
    {"Управление", kControls, int(sizeof(kControls) / sizeof(kControls[0]))},
    {"Отладка", kDebug, int(sizeof(kDebug) / sizeof(kDebug[0]))},
};

} // namespace

void Menu::open() {
	_open = true;
	_page = PAGE_MAIN;
	_sel[PAGE_MAIN] = 0;
}

int Menu::itemCount(Page page) const { return page < PAGES ? kPages[page].count : 0; }

const char* Menu::label(Page page, int item) const {
	return page < PAGES && item >= 0 && item < kPages[page].count ? kPages[page].items[item].label : "";
}

const char* Menu::note(Page page, int item) const {
	const char* n = page < PAGES && item >= 0 && item < kPages[page].count ? kPages[page].items[item].note : nullptr;
	return n ? n : "";
}

void Menu::value(Page page, int item, const Settings& s, char* out, u32 size) const {
	if (!size) return;
	out[0] = 0;
	if (page >= PAGES || item < 0 || item >= kPages[page].count) return;
	const Item& it = kPages[page].items[item];
	switch (it.kind) {
	case TOGGLE: snprintf(out, size, "%s", s.*it.field ? "Вкл" : "Выкл"); break;
	case CHOICE: snprintf(out, size, "%s", it.names[s.*it.field < it.count ? s.*it.field : 0]); break;
	case RANGE: snprintf(out, size, "%d%s%s", int(s.*it.field), it.unit[0] ? " " : "", it.unit); break;
	default: break;
	}
}

MenuAction Menu::update(const MenuInput& in, Settings& s) {
	if (!_open) return MENU_NONE;
	if (in.close) {
		_open = false;
		return MENU_CLOSED;
	}
	const PageDef& page = kPages[_page];
	int& sel = _sel[_page];
	sel = clamp(sel, 0, page.count - 1);
	if (in.back) {
		if (_page == PAGE_MAIN) {
			_open = false;
			return MENU_CLOSED;
		}
		_page = PAGE_MAIN;
		return MENU_NONE;
	}
	if (in.up) sel = (sel + page.count - 1) % page.count;
	if (in.down) sel = (sel + 1) % page.count;
	const Item& it = page.items[sel];
	const int dir = (in.right ? 1 : 0) - (in.left ? 1 : 0);
	switch (it.kind) {
	case ACTION:
		if (!in.confirm) return MENU_NONE;
		if (it.target == MENU_CLOSED) _open = false;
		return MenuAction(it.target);
	case PAGE:
		if (in.confirm || in.right) {
			_page = Page(it.target);
			_sel[_page] = 0;
		}
		return MENU_NONE;
	case TOGGLE:
		if (!in.confirm && !dir) return MENU_NONE;
		s.*it.field = s.*it.field ? 0 : 1;
		return MENU_CHANGED;
	case CHOICE: {
		const int step = dir ? dir : in.confirm ? 1 : 0;
		if (!step) return MENU_NONE;
		s.*it.field = u8((s.*it.field + it.count + step) % it.count);
		return MENU_CHANGED;
	}
	case RANGE: {
		if (!dir) return MENU_NONE;
		const int v = clamp(int(s.*it.field) + dir * int(it.count), int(it.lo), int(it.hi));
		if (v == int(s.*it.field)) return MENU_NONE;
		s.*it.field = u8(v);
		return MENU_CHANGED;
	}
	}
	return MENU_NONE;
}

void Menu::draw(Ui& ui, const Settings& s, const char* status, const char* message) const {
	if (!_open) return;
	const PageDef& page = kPages[_page];
	const int line = Ui::lineHeight(ui::TEXT) + 3;
	const int w = 320, x0 = (gfx::SCREEN_W - w) / 2, x1 = x0 + w;
	const int top = 16, bottom = gfx::SCREEN_H - 12;
	ui.rect(0, 0, gfx::SCREEN_W, gfx::SCREEN_H, 0x00000070u);
	ui.rect(x0, top, x1, bottom, ui::PANEL);
	ui.frame(x0, top, x1, bottom, 0x8C7A50FFu);
	ui.text(gfx::SCREEN_W / 2, top + 6, page.title, ui::GOLD, ui::TITLE, ui::CENTER);
	int y = top + 6 + Ui::lineHeight(ui::TITLE);
	if (status && *status) {
		ui.text(gfx::SCREEN_W / 2, y, status, ui::GREY, ui::TEXT, ui::CENTER);
		y += Ui::lineHeight(ui::TEXT);
	}
	y += 4;
	const int sel = clamp(_sel[_page], 0, page.count - 1);
	char buf[48];
	for (int i = 0; i < page.count; ++i, y += line) {
		const Item& it = page.items[i];
		const bool on = i == sel;
		if (on) ui.rect(x0 + 6, y - 1, x1 - 6, y + line - 2, 0x6E5A2E90u);
		ui.text(x0 + 14, y, it.label, on ? ui::WHITE : ui::GREY);
		if (it.kind == PAGE) {
			ui.text(x1 - 14, y, "\xE2\x86\x92", on ? ui::WHITE : ui::DARK, ui::TEXT, ui::RIGHT);
		} else if (it.kind != ACTION) {
			value(_page, i, s, buf, sizeof(buf));
			const u32 c = on ? ui::GOLD : ui::GREY;
			if (on && it.kind != TOGGLE) {
				// (Arrows: left and right change it.)
				const int vw = ui.measure(buf);
				ui.text(x1 - 14, y, "\xE2\x96\xB6", c, ui::TEXT, ui::RIGHT);
				ui.text(x1 - 26, y, buf, c, ui::TEXT, ui::RIGHT);
				ui.text(x1 - 30 - vw, y, "\xE2\x97\x80", c, ui::TEXT, ui::RIGHT);
			} else {
				ui.text(x1 - 14, y, buf, c, ui::TEXT, ui::RIGHT);
			}
		}
	}
	const Item& cur = page.items[sel];
	const int foot = bottom - 6 - Ui::lineHeight(ui::TEXT);
	if (message && *message)
		ui.text(gfx::SCREEN_W / 2, foot - Ui::lineHeight(ui::TEXT), message, ui::GREEN, ui::TEXT, ui::CENTER);
	else if (cur.note)
		ui.text(gfx::SCREEN_W / 2, foot - Ui::lineHeight(ui::TEXT), cur.note, ui::DARK + 0x30303000u, ui::TEXT, ui::CENTER);
	ui.text(gfx::SCREEN_W / 2, foot, _page == PAGE_MAIN ? "A — выбрать   B, START — вернуться в игру" : "A — выбрать   \xE2\x97\x80\xE2\x96\xB6 — изменить   B — назад",
	        ui::DARK + 0x20202000u, ui::TEXT, ui::CENTER);
}

} // namespace g3
