#include "lang.h"
#include "menu.h"

#include <stdio.h>
#include <string.h>

namespace g3 {

namespace {

enum Kind : u8 { ACTION, PAGE, TOGGLE, CHOICE, RANGE };

// The texts here are the Russian ones (lang.h): Menu::label, note and value
// give them in the language of the moment.
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
	const char* const* notes; // CHOICE: a note for each value (instead of `note`), or null
};

const char* const kObjects[] = {"Плавность", "Баланс", "Устойчиво"};
const char* const kObjectNotes[] = {"Под нагрузкой мелочь вдали убирается",
                                    "Мелкие 45 м, средние 85 м, убирается мало",
                                    "Мелкие 60 м, средние 100 м; при нагрузке 30 к/с"};
const char* const kDensity[] = {"100 %", "75 %", "50 %"};
const char* const kDensityNotes[] = {"Все кусты, камни и мусор", "Каждый четвёртый мелкий предмет убран",
                                     "Каждый второй мелкий предмет убран"};
const char* const kNpcRange[] = {"20 м", "30 м", "45 м"};
const char* const kFrames[] = {"60 (30 под нагрузкой)", "Ровно 30", "Свободно до 60"};
const char* const kFrameNotes[] = {"60, а при перегрузке ровные 30", "Всегда 30: больше деталей, нет рывков",
                                   "Кадр выходит, как только готов (30–60)"};
const char* const kSchemes[] = {"Консольная", "Классическая"};
const char* const kSchemeNotes[] = {"Стик ведёт туда, куда указывает на экране", "Как в оригинале: камера всегда за спиной"};
const char* const kBottom[] = {"Инвентарь", "Отчёт"};
const char* const kTimeFlow[] = {"Стоит", "Сутки за 24 мин", "Сутки за 1 ч", "Сутки за 2 ч"};
const char* const kLanguages[] = {"Русский", "English", "Polski", "Deutsch"};
const char* const kGfxPresets[] = {"Выкл", "Баланс", "Максимум", "Свой"};
const char* const kGfxPresetNotes[] = {"Всё выключено, как раньше", "Затенение, листва, тень героя, даль 250 м", "Всё на полную",
                                       "Значения изменены вручную"};
const char* const kWind[] = {"Нет", "Слабое", "Обычное", "Сильное"};
const char* const kShadows[] = {"Выкл", "Герой", "Все рядом"};
const char* const kFarLayer[] = {"Выкл", "250 м", "400 м", "600 м"};
const char* const kFarLayerNotes[] = {"Дальше дальности прорисовки — туман", "Упрощённый мир до 250 м",
                                      "Упрощённый мир до 400 м", "Упрощённый мир до 600 м"};
const char* const kShadowNotes[] = {"Без теней под ногами", "Круглая тень под героем", "Под героем и жителями до 25 м"};

#define NONE nullptr, nullptr, 0, 0, 0, nullptr, 0, nullptr, nullptr
const Item kMain[] = {
    {"Продолжить", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_CLOSED, nullptr, nullptr},
    {"Графика", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_GRAPHICS, "Дальность, детали, частота кадров", nullptr},
    {"Игра", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_GAME, "Бой и предметы", nullptr},
    {"Управление", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_CONTROLS, "Схема, камера", nullptr},
    {"Интерфейс", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_INTERFACE, "Язык, подсказки, нижний экран", nullptr},
    {"Звук", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_SOUND, "Громкость", nullptr},
    {"Отладка", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_DEBUG, nullptr, nullptr},
    {"Сохранить позицию", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_SAVE, "То же: R + START в игре", nullptr},
    {"Загрузить позицию", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_LOAD, "То же: R + SELECT в игре", nullptr},
    {"Выйти в Homebrew Launcher", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_EXIT, "Позиция и настройки сохранятся", nullptr},
};
const Item kGraphics[] = {
    {"Дальность прорисовки", RANGE, &Settings::drawMetres, nullptr, 10, 40, 200, "м", 0, "Дальше — больше нагрузки на видеочип", nullptr},
    {"Мелкие и средние объекты", CHOICE, &Settings::objects, kObjects, 3, 0, 0, nullptr, 0, nullptr, kObjectNotes},
    {"Густота мелких объектов", CHOICE, &Settings::density, kDensity, 3, 0, 0, nullptr, 0, nullptr, kDensityNotes},
    {"Жители вдали", CHOICE, &Settings::npcDistance, kNpcRange, 3, 0, 0, nullptr, 0, "Как далеко видны жители и звери", nullptr},
    {"Купол Барьера", TOGGLE, &Settings::barrier, nullptr, 0, 0, 0, nullptr, 0, "Сияние неба и гул, если есть в данных", nullptr},
    {"Плавное появление", TOGGLE, &Settings::dissolve, nullptr, 0, 0, 0, nullptr, 0, "Вступит в силу после перезапуска", nullptr},
    {"Частота кадров", CHOICE, &Settings::frameRate, kFrames, 3, 0, 0, nullptr, 0, nullptr, kFrameNotes},
    {"Графика+", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_GFX_PLUS, "Затенение углов, листва, тени, даль", nullptr},
    {"Сбросить графику", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_RESET_GRAPHICS, "Вернуть значения по умолчанию", nullptr},
};
const Item kGame[] = {
    {"Автозахват цели", TOGGLE, &Settings::autoTarget, nullptr, 0, 0, 0, nullptr, 0, "Взял оружие — цель захвачена", nullptr},
    {"Течение времени", CHOICE, &Settings::timeFlow, kTimeFlow, 4, 0, 0, nullptr, 0, "Реальное время на игровые сутки", nullptr},
    {"Оружие после зелья", TOGGLE, &Settings::weaponBack, nullptr, 0, 0, 0, nullptr, 0, "Выпил — снова берёт оружие в руки", nullptr},
};
const Item kControls[] = {
    {"Схема", CHOICE, &Settings::scheme, kSchemes, 2, 0, 0, nullptr, 0, nullptr, kSchemeNotes},
    {"Скорость камеры", RANGE, &Settings::cameraSpeed, nullptr, 1, 1, 10, "", 0, nullptr, nullptr},
    {"Инверсия по вертикали", TOGGLE, &Settings::invertY, nullptr, 0, 0, 0, nullptr, 0, nullptr, nullptr},
    {"Камера за героем", TOGGLE, &Settings::autoCamera, nullptr, 0, 0, 0, nullptr, 0, "Камера заходит за спину на бегу", nullptr},
    {"Вид от первого лица", TOGGLE, &Settings::firstPerson, nullptr, 0, 0, 0, nullptr, 0, nullptr, nullptr},
};
const Item kInterface[] = {
    {"Язык / Language", CHOICE, &Settings::language, kLanguages, 4, 0, 0, nullptr, 0, "Тексты игры берутся из её данных", nullptr},
    {"Интерфейс в игре", TOGGLE, &Settings::hud, nullptr, 0, 0, 0, nullptr, 0, "Здоровье, цель, подсказки", nullptr},
    {"Показывать FPS", TOGGLE, &Settings::showFps, nullptr, 0, 0, 0, nullptr, 0, "Счётчик кадров сверху справа", nullptr},
    {"Нижний экран", CHOICE, &Settings::report, kBottom, 2, 0, 0, nullptr, 0, "Отчёт: цифры для проверки", nullptr},
};
const Item kSound[] = {
    {"Громкость", RANGE, &Settings::soundVolume, nullptr, 1, 0, 10, "", 0, "Шаги, бой, звуки мира; 0 — без звука", nullptr},
    {"Музыка", RANGE, &Settings::musicVolume, nullptr, 1, 0, 10, "", 0, "Нужен music.pak рядом с data.pak", nullptr},
    {"Голоса", RANGE, &Settings::voiceVolume, nullptr, 1, 0, 10, "", 0, "Нужен speech.pak рядом с data.pak", nullptr},
};
const Item kGfxPlus[] = {
    {"Набор", CHOICE, &Settings::gfxPlus, kGfxPresets, 4, 0, 0, nullptr, 0, nullptr, kGfxPresetNotes},
    {"Затенение углов", RANGE, &Settings::occlusion, nullptr, 10, 0, 100, "%", 0, "Нужен data.pak, запечённый с --ao", nullptr},
    {"Колыхание листвы", CHOICE, &Settings::wind, kWind, 4, 0, 0, nullptr, 0, "Нужен data.pak, запечённый с --wind", nullptr},
    {"Тени персонажей", CHOICE, &Settings::shadows, kShadows, 3, 0, 0, nullptr, 0, nullptr, kShadowNotes},
    {"Дальний план", CHOICE, &Settings::farLayer, kFarLayer, 4, 0, 0, nullptr, 0, nullptr, kFarLayerNotes},
};
const Item kDebug[] = {
    {"Жители", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_DEBUG_NPC, "Восприятие, жизнь, драки", nullptr},
    {"Герой и вещи", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_DEBUG_HERO, "Здоровье, оружие, тест-набор", nullptr},
    {"Мир и время", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_DEBUG_WORLD, "Полёт, мир, время", nullptr},
};
const Item kDebugNpc[] = {
    {"Восприятие жителей", TOGGLE, &Settings::perception, nullptr, 0, 0, 0, nullptr, 0, nullptr, nullptr},
    {"Жизнь жителей", TOGGLE, &Settings::life, nullptr, 0, 0, 0, nullptr, 0, "Выкл: стоят на местах распорядка", nullptr},
    {"Драки жителей", TOGGLE, &Settings::fights, nullptr, 0, 0, 0, nullptr, 0, "Выкл: состояния атаки не начинаются", nullptr},
};
const Item kDebugHero[] = {
    {"Бессмертие", TOGGLE, &Settings::immortal, nullptr, 0, 0, 0, nullptr, 0, nullptr, nullptr},
    {"Вылечить", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_HEAL, nullptr, nullptr},
    {"Тест-набор", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_GIVE_TESTKIT, "Магия, зелья роста, оружие, лук, броня", nullptr},
    {"Дать лучшее оружие", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_GIVE_WEAPON, "Из оружия, что есть в скриптах", nullptr},
    {"Дать набор предметов", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_GIVE_ITEMS, "Зелья, еда, оружие, броня, кольца", nullptr},
};
const Item kDebugWorld[] = {
    {"Полёт вкл/выкл", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_FLY, "Свободная камера без столкновений", nullptr},
    {"Следующий мир", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_NEXT_WORLD, nullptr, nullptr},
    {"Время +1 час", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_HOUR, nullptr, nullptr},
};
#undef NONE

struct PageDef {
	const char* title;
	const Item* items;
	int count;
};
#define PAGE_OF(title, arr) {title, arr, int(sizeof(arr) / sizeof(arr[0]))}
const PageDef kPages[Menu::PAGES] = {
    PAGE_OF("Пауза", kMain),         PAGE_OF("Графика", kGraphics),   PAGE_OF("Игра", kGame),
    PAGE_OF("Управление", kControls), PAGE_OF("Интерфейс", kInterface), PAGE_OF("Звук", kSound),
    PAGE_OF("Отладка", kDebug),      PAGE_OF("Жители", kDebugNpc),    PAGE_OF("Герой и вещи", kDebugHero),
    PAGE_OF("Мир и время", kDebugWorld), PAGE_OF("Графика+", kGfxPlus),
};
#undef PAGE_OF

} // namespace

Menu::Page Menu::parentOf(Page page) {
	if (page == PAGE_GFX_PLUS) return PAGE_GRAPHICS;
	return page == PAGE_DEBUG_NPC || page == PAGE_DEBUG_HERO || page == PAGE_DEBUG_WORLD ? PAGE_DEBUG : PAGE_MAIN;
}

void Menu::open() {
	_open = true;
	_page = PAGE_MAIN;
	_sel[PAGE_MAIN] = 0;
}

int Menu::itemCount(Page page) const { return page < PAGES ? kPages[page].count : 0; }

const char* Menu::label(Page page, int item) const {
	return page < PAGES && item >= 0 && item < kPages[page].count ? tr(kPages[page].items[item].label) : "";
}

namespace {

// The note of an item: for a choice, the one of its value (without settings: the longest).
const char* noteOf(const Item& it, const Settings* s) {
	if (it.kind == CHOICE && it.notes) {
		if (s) return tr(it.notes[s->*it.field < it.count ? s->*it.field : 0]);
		const char* best = it.notes[0];
		for (int k = 1; k < it.count; ++k)
			if (strlen(tr(it.notes[k])) > strlen(tr(best))) best = it.notes[k];
		return tr(best);
	}
	return it.note ? tr(it.note) : "";
}

} // namespace

const char* Menu::note(Page page, int item, const Settings* s) const {
	return page < PAGES && item >= 0 && item < kPages[page].count ? noteOf(kPages[page].items[item], s) : "";
}

void Menu::value(Page page, int item, const Settings& s, char* out, u32 size) const {
	if (!size) return;
	out[0] = 0;
	if (page >= PAGES || item < 0 || item >= kPages[page].count) return;
	const Item& it = kPages[page].items[item];
	switch (it.kind) {
	case TOGGLE: snprintf(out, size, "%s", s.*it.field ? tr("Вкл") : tr("Выкл")); break;
	case CHOICE: snprintf(out, size, "%s", tr(it.names[s.*it.field < it.count ? s.*it.field : 0])); break;
	case RANGE: snprintf(out, size, "%d%s%s", int(s.*it.field), it.unit[0] ? " " : "", tr(it.unit)); break;
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
		_page = parentOf(_page);
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
		if (it.field == &Settings::gfxPlus) {
			// A set: its values follow. ("Own" is only shown, never chosen.)
			if (s.gfxPlus == GFX_PLUS_OWN) s.gfxPlus = u8((s.gfxPlus + it.count + step) % it.count);
			s.applyGfxPreset(s.gfxPlus);
		} else if (_page == PAGE_GFX_PLUS) {
			s.gfxPlus = s.gfxPresetOf();
		}
		return MENU_CHANGED;
	}
	case RANGE: {
		if (!dir) return MENU_NONE;
		const int v = clamp(int(s.*it.field) + dir * int(it.count), int(it.lo), int(it.hi));
		if (v == int(s.*it.field)) return MENU_NONE;
		s.*it.field = u8(v);
		if (_page == PAGE_GFX_PLUS) s.gfxPlus = s.gfxPresetOf();
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
	ui.text(gfx::SCREEN_W / 2, top + 6, tr(page.title), ui::GOLD, ui::TITLE, ui::CENTER);
	int y = top + 6 + Ui::lineHeight(ui::TITLE);
	if (status && *status) {
		ui.text(gfx::SCREEN_W / 2, y, status, ui::GREY, ui::TEXT, ui::CENTER);
		y += Ui::lineHeight(ui::TEXT);
	}
	y += 4;
	const int sel = clamp(_sel[_page], 0, page.count - 1);
	char buf[48];
	// The rows that fit between the title and the notes at the bottom; the list scrolls to keep the selection in view.
	const int foot0 = bottom - 6 - Ui::lineHeight(ui::TEXT);
	const int listBottom = foot0 - Ui::lineHeight(ui::TEXT) - 4;
	const int visible = max((listBottom - y) / line, 3);
	int first = 0;
	if (page.count > visible) first = clamp(sel - visible / 2, 0, page.count - visible);
	const int last = min(page.count, first + visible);
	if (first > 0) ui.text(x1 - 14, y - line + 6, "\xE2\x96\xB2", ui::DARK, ui::TEXT, ui::RIGHT);
	if (last < page.count) ui.text(x1 - 14, listBottom - 2, "\xE2\x96\xBC", ui::DARK, ui::TEXT, ui::RIGHT);
	for (int i = first; i < last; ++i, y += line) {
		const Item& it = page.items[i];
		const bool on = i == sel;
		if (on) ui.rect(x0 + 6, y - 1, x1 - 6, y + line - 2, 0x6E5A2E90u);
		ui.text(x0 + 14, y, tr(it.label), on ? ui::WHITE : ui::GREY);
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
	const int foot = foot0;
	if (message && *message)
		ui.text(gfx::SCREEN_W / 2, foot - Ui::lineHeight(ui::TEXT), message, ui::GREEN, ui::TEXT, ui::CENTER);
	else if (*noteOf(cur, &s))
		ui.text(gfx::SCREEN_W / 2, foot - Ui::lineHeight(ui::TEXT), noteOf(cur, &s), ui::DARK + 0x30303000u, ui::TEXT, ui::CENTER);
	ui.text(gfx::SCREEN_W / 2, foot, _page == PAGE_MAIN ? tr("A — выбрать   B, START — вернуться в игру") : tr("A — выбрать   \xE2\x97\x80\xE2\x96\xB6 — изменить   B — назад"),
	        ui::DARK + 0x20202000u, ui::TEXT, ui::CENTER);
}

} // namespace g3
