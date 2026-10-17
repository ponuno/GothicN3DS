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
const char* const kFrames[] = {"60 (30 под нагрузкой)", "Ровно 30", "Свободно до 60", "Ровно 40", "Плавающие 30"};
const char* const kFrameNotes[] = {"60, а при перегрузке ровные 30", "Всегда 30: больше деталей, нет рывков",
                                   "Кадр выходит, как только готов (30–60)", "Экран 40 Гц, на кадр 25 мс (опыт)",
                                   "Экран 26–30 Гц, поздний кадр не ждёт"};
const char* const kAntialias[] = {"Выкл", "Лёгкое", "Полное"};
const char* const kAntialiasNotes[] = {"Без сглаживания", "Вдвое больше пикселей; после перезапуска", "Вчетверо больше пикселей; после перезапуска"};
const char* const kSchemes[] = {"Консольная", "Классическая"};
const char* const kSchemeNotes[] = {"Стик ведёт туда, куда указывает на экране", "Как в оригинале: камера всегда за спиной"};
const char* const kBottom[] = {"Инвентарь", "Отчёт"};
const char* const kTimeFlow[] = {"Стоит", "Сутки за 24 мин", "Сутки за 1 ч", "Сутки за 2 ч"};
const char* const kLanguages[] = {"Русский", "English", "Polski", "Deutsch"};
const char* const kGfxPresets[] = {"Выкл", "Баланс", "Максимум", "Свой"};
const char* const kGfxPresetNotes[] = {"Всё выключено, как раньше", "Затенение, листва, тень героя, даль 250 м", "Всё на полную",
                                       "Значения изменены вручную"};
const char* const kScreen[] = {"Обычный", "3D", "Широкий 800x240"};
const char* const kWind[] = {"Нет", "Слабое", "Обычное", "Сильное"};
const char* const kWaves[] = {"Нет", "Слабые", "Обычные", "Сильные"};
const char* const kShadows[] = {"Выкл", "Герой", "Все рядом", "Силуэт героя"};
const char* const kRefresh[] = {"Выкл", "45–60 Гц", "45–72 Гц", "45–80 Гц", "Держать 72", "Держать 80"};
const char* const kFarGeometry[] = {"Выкл", "25 м", "40 м", "60 м"};
const char* const kWeather[] = {"Авто", "Ясно", "Облачно", "Дождь", "Гроза", "Снег"};
const char* const kLodFade[] = {"Сразу", "Быстро", "Плавно"};
const char* const kFarLayer[] = {"Выкл", "250 м", "400 м", "600 м"};
const char* const kFarLayerNotes[] = {"Дальше дальности прорисовки — туман", "Упрощённый мир до 250 м",
                                      "Упрощённый мир до 400 м", "Упрощённый мир до 600 м"};
const char* const kSunShadows[] = {"Выкл", "Обычные", "Сильные", "Максимум"};
const char* const kSunShadowNotes[] = {"Без тени от солнца (--sun-shadow)", "Тень деревьев и домов днём (--sun-shadow)", "Тень темнее и дольше днём",
                                       "Самая тёмная тень"};
const char* const kCloudShadows[] = {"Выкл", "Обычные", "Тёмные"};
const char* const kCloudShadowNotes[] = {"Без теней от облаков", "Плывут по земле днём (нужна тень солнца)", "Темнее (нужна тень от солнца)"};
const char* const kGrass[] = {"Выкл", "Мало", "Много"};
const char* const kGrassNotes[] = {"Новая трава выключена (мир с --grass)", "Половина пучков, до 20 м (мир с --grass)",
                                   "Все пучки, до 28 м (мир с --grass)"};
const char* const kBarrier[] = {"Выкл", "Как в игре", "Всегда"};
const char* const kBarrierNotes[] = {"Купола нет", "25 с примерно раз в 20 минут, с молниями", "Виден всё время, с молниями"};
const char* const kBarrierThunder[] = {"Как в игре", "Гром рядом", "Раскат", "Магия молний", "Без звука"};
const char* const kBarrierThunderNotes[] = {"Звук Барьера из игры", "Близкий удар грома", "Долгий далёкий раскат",
                                            "Звук заклинания-молнии", "Молнии без грома"};
const char* const kLamps[] = {"Выкл", "Персонажи", "Всё"};
const char* const kLampsNotes[] = {"Факелы и костры не светят", "Светят на жителей, героя и вещи рядом",
                                   "И на землю и стены вокруг, с мерцанием"};
const char* const kShadowNotes[] = {"Без теней под ногами", "Круглая тень под героем", "Под героем и жителями до 25 м", "Герой по фигуре, жители круглые"};

#define NONE nullptr, nullptr, 0, 0, 0, nullptr, 0, nullptr, nullptr
const Item kMain[] = {
    {"Продолжить", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_CLOSED, nullptr, nullptr},
    {"Графика", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_GRAPHICS, "Дальность, детали, частота кадров", nullptr},
    {"Игра", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_GAME, "Бой и предметы", nullptr},
    {"Управление", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_CONTROLS, "Схема, камера", nullptr},
    {"Интерфейс", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_INTERFACE, "Язык, подсказки, нижний экран", nullptr},
    {"Звук", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_SOUND, "Громкость", nullptr},
    {"Отладка", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_DEBUG, nullptr, nullptr},
    {"Сохранить игру", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_SAVE, "10 ячеек; быстро — R + START в игре", nullptr},
    {"Загрузить игру", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_LOAD, "Быстрая загрузка — R + SELECT в игре", nullptr},
    {"Выйти в Homebrew Launcher", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_EXIT, "Позиция и настройки сохранятся", nullptr},
};
const Item kGraphics[] = {
    {"Дальность прорисовки", RANGE, &Settings::drawMetres, nullptr, 10, 40, 200, "м", 0, "Дальше — больше нагрузки на видеочип", nullptr},
    {"Мелкие и средние объекты", CHOICE, &Settings::objects, kObjects, 3, 0, 0, nullptr, 0, nullptr, kObjectNotes},
    {"Густота мелких объектов", CHOICE, &Settings::density, kDensity, 3, 0, 0, nullptr, 0, nullptr, kDensityNotes},
    {"Жители вдали", CHOICE, &Settings::npcDistance, kNpcRange, 3, 0, 0, nullptr, 0, "Как далеко видны жители и звери", nullptr},
    {"Купол Барьера", CHOICE, &Settings::barrier, kBarrier, 3, 0, 0, nullptr, 0, nullptr, kBarrierNotes},
    {"Свет огней", CHOICE, &Settings::lamps, kLamps, 3, 0, 0, nullptr, 0, nullptr, kLampsNotes},
    {"Плавное появление", TOGGLE, &Settings::dissolve, nullptr, 0, 0, 0, nullptr, 0, "Вступит в силу после перезапуска", nullptr},
    {"Частота кадров", CHOICE, &Settings::frameRate, kFrames, 3, 0, 0, nullptr, 0, nullptr, kFrameNotes},
    {"Экран", CHOICE, &Settings::stereo, kScreen, 3, 0, 0, nullptr, 0, "Ползунок 3D или 800x240; после перезапуска", nullptr},
    {"Графика+", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_GFX_PLUS, "Затенение углов, листва, тени, даль", nullptr},
    {"Экспериментально", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_EXPERIMENTAL, "Разгон экрана: 40 Гц, 80 Гц. Риск!", nullptr},
    {"Сбросить графику", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_RESET_GRAPHICS, "Вернуть значения по умолчанию", nullptr},
};
const Item kExperimental[] = {
    {"ВНИМАНИЕ: разгон экрана", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, 0, "Он не предусмотрен Nintendo", nullptr},
    {"Это может сократить жизнь экрана", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, 0, "Возможны полосы, мерцание, перегрев", nullptr},
    {"Вся ответственность на вас", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, 0, "Автор не отвечает за последствия", nullptr},
    {"Частота кадров", CHOICE, &Settings::frameRate, kFrames, 5, 0, 0, nullptr, 0, nullptr, kFrameNotes},
    {"Частота экрана", CHOICE, &Settings::refresh, kRefresh, 6, 0, 0, nullptr, 0, "Экран подстраивается под кадр", nullptr},
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
    {"Вид от первого лица", TOGGLE, &Settings::firstPerson, nullptr, 0, 0, 0, nullptr, 0, "Выкл — камера за героем", nullptr},
};
const Item kInterface[] = {
    {"Язык / Language", CHOICE, &Settings::language, kLanguages, 4, 0, 0, nullptr, 0, "Тексты игры берутся из её данных", nullptr},
    {"Интерфейс в игре", TOGGLE, &Settings::hud, nullptr, 0, 0, 0, nullptr, 0, "Здоровье, цель, подсказки", nullptr},
    {"Показывать FPS", TOGGLE, &Settings::showFps, nullptr, 0, 0, 0, nullptr, 0, "Счётчик кадров сверху справа", nullptr},
    {"Нижний экран", CHOICE, &Settings::report, kBottom, 2, 0, 0, nullptr, 0, "Отчёт: цифры для проверки", nullptr},
};
// The start menu's "options" (the original's MENU_OPTIONS): the settings' pages.
const Item kOptions[] = {
    {"Графика", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_GRAPHICS, "Дальность, детали, частота кадров", nullptr},
    {"Игра", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_GAME, "Бой и предметы", nullptr},
    {"Управление", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_CONTROLS, "Схема, камера", nullptr},
    {"Интерфейс", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_INTERFACE, "Язык, подсказки, нижний экран", nullptr},
    {"Звук", PAGE, nullptr, nullptr, 0, 0, 0, nullptr, Menu::PAGE_SOUND, "Громкость", nullptr},
};
const Item kSound[] = {
    {"Громкость", RANGE, &Settings::soundVolume, nullptr, 1, 0, 10, "", 0, "Шаги, бой, звуки мира; 0 — без звука", nullptr},
    {"Музыка", RANGE, &Settings::musicVolume, nullptr, 1, 0, 10, "", 0, "Нужен music.pak рядом с data.pak", nullptr},
    {"Голоса", RANGE, &Settings::voiceVolume, nullptr, 1, 0, 10, "", 0, "Нужен speech.pak рядом с data.pak", nullptr},
    {"Гром Барьера", CHOICE, &Settings::barrierThunder, kBarrierThunder, 5, 0, 0, nullptr, 0, nullptr, kBarrierThunderNotes},
};
const Item kGfxPlus[] = {
    {"Набор", CHOICE, &Settings::gfxPlus, kGfxPresets, 4, 0, 0, nullptr, 0, nullptr, kGfxPresetNotes},
    {"Затенение углов", RANGE, &Settings::occlusion, nullptr, 10, 0, 100, "%", 0, "Нужен data.pak, запечённый с --ao", nullptr},
    {"Колыхание листвы", CHOICE, &Settings::wind, kWind, 4, 0, 0, nullptr, 0, "Нужен data.pak, запечённый с --wind", nullptr},
    {"Волны на воде", CHOICE, &Settings::waves, kWaves, 4, 0, 0, nullptr, 0, "Нужен data.pak, запечённый с --water-grid", nullptr},
    {"Тени персонажей", CHOICE, &Settings::shadows, kShadows, 4, 0, 0, nullptr, 0, nullptr, kShadowNotes},
    {"Тени от солнца", CHOICE, &Settings::sunShadows, kSunShadows, 4, 0, 0, nullptr, 0, nullptr, kSunShadowNotes},
    {"Тени облаков", CHOICE, &Settings::cloudShadows, kCloudShadows, 3, 0, 0, nullptr, 0, nullptr, kCloudShadowNotes},
    {"Трава", CHOICE, &Settings::grass, kGrass, 3, 0, 0, nullptr, 0, nullptr, kGrassNotes},
    {"Дальний план", CHOICE, &Settings::farLayer, kFarLayer, 4, 0, 0, nullptr, 0, nullptr, kFarLayerNotes},
    {"Дальняя геометрия", CHOICE, &Settings::farGeometry, kFarGeometry, 4, 0, 0, nullptr, 0, "Упрощённые дальние чанки (мир с --lod)", nullptr},
    {"Доворот кадра", TOGGLE, &Settings::reproject, nullptr, 0, 0, 0, nullptr, 0, "Старый кадр довёрнут, пока ждём новый", nullptr},
    {"Сглаживание", CHOICE, &Settings::antialias, kAntialias, 3, 0, 0, nullptr, 0, nullptr, kAntialiasNotes},
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
    {"Повысить уровень", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_LEVEL_UP, "Опыт по правилам скриптов", nullptr},
    {"Набор: заклинания", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_GIVE_SPELLKIT, "Руны, свитки, превращения, призыв, мана", nullptr},
    {"Набор: ремёсла", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_GIVE_CRAFTKIT, "Всё для станков, алхимии, добычи", nullptr},
    {"Изучить все навыки", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_LEARN_ALL, "Таланты, ремёсла, добыча трофеев", nullptr},
    {"Дать лучшее оружие", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_GIVE_WEAPON, "Из оружия, что есть в скриптах", nullptr},
    {"Дать набор предметов", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_GIVE_ITEMS, "Зелья, еда, оружие, броня, кольца", nullptr},
};
const Item kDebugWorld[] = {
    {"Полёт вкл/выкл", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_FLY, "Свободная камера без столкновений", nullptr},
    {"Следующий мир", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_NEXT_WORLD, nullptr, nullptr},
    {"Время +1 час", ACTION, nullptr, nullptr, 0, 0, 0, nullptr, MENU_HOUR, nullptr, nullptr},
    {"Глава", RANGE, &Settings::chapter, nullptr, 1, 0, 6, "", 0, "Меняет главу скриптами игры", nullptr},
    {"Дальний план вблизи", TOGGLE, &Settings::farFill, nullptr, 0, 0, 0, nullptr, 0, "Затычка на месте незагруженных чанков", nullptr},
    {"Смена LOD", CHOICE, &Settings::lodFade, kLodFade, 3, 0, 0, nullptr, 0, "Смена дальних чанков (рябь)", nullptr},
    {"Погода", CHOICE, &Settings::weather, kWeather, 6, 0, 0, nullptr, 0, "Авто: меняется сама; иначе держится", nullptr},
    {"Мокрая земля", TOGGLE, &Settings::wetGround, nullptr, 0, 0, 0, nullptr, 0, "В дождь свет на улице темнее", nullptr},
    {"Лучи света", TOGGLE, &Settings::godRays, nullptr, 0, 0, 0, nullptr, 0, "Столбы света в тенях (днём, в ясно)", nullptr},
    {"Видимость", TOGGLE, &Settings::visibility, nullptr, 0, 0, 0, nullptr, 0, "Не рисовать скрытое стенами (мир с --vis)", nullptr},
    {"Чтение наперёд", TOGGLE, &Settings::readAhead, nullptr, 0, 0, 0, nullptr, 0, "Модели и чанки читаются с карты заранее", nullptr},
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
    PAGE_OF("Мир и время", kDebugWorld), PAGE_OF("Графика+", kGfxPlus), PAGE_OF("Экспериментально", kExperimental),
    {"Сохранить игру", nullptr, 0}, {"Загрузить игру", nullptr, 0}, // (lists of slots: Menu::drawSaves)
    PAGE_OF("Настройки", kOptions),
};
#undef PAGE_OF

} // namespace

Menu::Page Menu::parentOf(Page page) {
	if (page == PAGE_GFX_PLUS || page == PAGE_EXPERIMENTAL) return PAGE_GRAPHICS;
	return page == PAGE_DEBUG_NPC || page == PAGE_DEBUG_HERO || page == PAGE_DEBUG_WORLD ? PAGE_DEBUG : PAGE_MAIN;
}

void Menu::open(Page root) {
	_open = true;
	_root = root < PAGES ? root : PAGE_MAIN;
	_page = _root;
	_sel[_page] = 0;
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
	if (savePage(_page)) {
		const int n = slotCount(_page);
		int& sel = _sel[_page];
		sel = clamp(sel, 0, n - 1);
		if (in.back) {
			if (_page == _root) {
				_open = false;
				return MENU_CLOSED;
			}
			_page = _root;
			return MENU_NONE;
		}
		if (in.up) sel = (sel + n - 1) % n;
		if (in.down) sel = (sel + 1) % n;
		// (Left and right: five on, as the pages of the original's list.)
		if (in.left) sel = max(sel - 5, 0);
		if (in.right) sel = min(sel + 5, n - 1);
		if (in.confirm) return _page == PAGE_SAVE ? MENU_SAVE_SLOT : MENU_LOAD_SLOT;
		return MENU_NONE;
	}
	const PageDef& page = kPages[_page];
	int& sel = _sel[_page];
	sel = clamp(sel, 0, page.count - 1);
	if (in.back) {
		if (_page == _root) {
			_open = false;
			return MENU_CLOSED;
		}
		const Page up = parentOf(_page);
		_page = up == PAGE_MAIN ? _root : up;
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

namespace {

// `text` cut to `width` pixels, with "…" where it was cut.
const char* fitText(const Ui& ui, const char* text, int width, char* buf, u32 size) {
	if (ui.measure(text) <= width) return text;
	const int dots = ui.measure("\xE2\x80\xA6");
	u32 n = 0;
	for (const char* p = text; *p;) {
		const char* at = p;
		utf8Next(p);
		const u32 len = u32(p - text);
		if (len + 4 > size) break;
		memcpy(buf, text, len);
		buf[len] = 0;
		if (ui.measure(buf) + dots > width) {
			n = u32(at - text);
			break;
		}
		n = len;
	}
	memcpy(buf, text, n);
	memcpy(buf + n, "\xE2\x80\xA6", 4);
	return buf;
}

} // namespace

void Menu::drawSaves(Ui& ui, const char* message, const SaveMenuView* v) const {
	const int W = gfx::SCREEN_W, H = gfx::SCREEN_H;
	const int x0 = 6, x1 = W - 6, top = 6, bottom = H - 6;
	const int line = Ui::lineHeight(ui::TEXT) + 2, lh = Ui::lineHeight(ui::TEXT); // (eleven rows fit)
	ui.rect(0, 0, W, H, 0x00000070u);
	ui.box(x0, top, x1, bottom, 0x14100CD0u);
	ui.text(W / 2, top + 5, tr(kPages[_page].title), ui::GOLD, ui::TITLE, ui::CENTER);
	const bool saving = _page == PAGE_SAVE;
	const int n = slotCount(_page);
	const int sel = clamp(_sel[_page], 0, n - 1);
	char buf[96], cut[96];

	// The slots, left.
	const int lx0 = x0 + 6, lx1 = 202, ly0 = top + 8 + Ui::lineHeight(ui::TITLE);
	const int foot = bottom - 5 - lh, listBottom = foot - lh - 3;
	const int visible = max((listBottom - ly0) / line, 3);
	const int first = n > visible ? clamp(sel - visible / 2, 0, n - visible) : 0;
	const int last = min(n, first + visible);
	int y = ly0;
	for (int i = first; i < last; ++i, y += line) {
		const int slot = slotOfEntry(_page, i);
		const SaveInfo* info = v && v->slots ? &v->slots[slot] : nullptr;
		const bool on = i == sel;
		if (on) ui.rect(lx0, y - 1, lx1, y + line - 2, 0x6E5A2E90u);
		int tx = lx0 + 6;
		if (slot > 0 && slot <= SAVE_SLOTS) {
			snprintf(buf, sizeof(buf), "%d.", slot);
			ui.text(lx0 + 22, y, buf, on ? ui::GOLD : ui::DARK, ui::TEXT, ui::RIGHT);
			tx = lx0 + 26;
		}
		const char* name;
		u32 colour = on ? ui::WHITE : ui::GREY;
		if (!info || !info->used) {
			name = slot == 0 ? tr("Быстрое сохранение — нет") : slot == SAVE_AUTO ? tr("Автосохранение — нет") : tr("— пусто —");
			colour = on ? ui::GREY : ui::DARK;
		} else if (!info->readable) {
			name = tr("(повреждено)");
			colour = ui::RED;
		} else if (slot == 0 || slot == SAVE_AUTO) {
			name = slot == 0 ? tr("Быстрое сохранение") : tr("Автосохранение");
			if (!on) colour = 0xC8B48CFFu;
		} else {
			name = info->title[0] ? info->title : tr("Без названия");
		}
		ui.text(tx, y, fitText(ui, name, lx1 - 6 - tx, cut, sizeof(cut)), colour);
	}
	if (first > 0) ui.text(lx1 - 4, ly0 - line + 5, "\xE2\x96\xB2", ui::DARK, ui::TEXT, ui::RIGHT);
	if (last < n) ui.text(lx1 - 4, listBottom - 3, "\xE2\x96\xBC", ui::DARK, ui::TEXT, ui::RIGHT);

	// The picture and what is known of the save, right.
	const int rx0 = 210, rx1 = x1 - 6, rw = rx1 - rx0;
	const int px0 = rx0 + (rw - THUMB_W) / 2, py0 = ly0 + 1, px1 = px0 + THUMB_W, py1 = py0 + THUMB_H;
	const SaveInfo* info = v && v->slots ? &v->slots[slotOfEntry(_page, sel)] : nullptr;
	const bool current = v && v->current;
	const SaveInfo* shown = current ? &v->now : info && info->used && info->readable ? info : nullptr;
	ui.rect(px0, py0, px1, py1, 0x000000C0u);
	if (shown && v->picture) {
		ui.picture(v->picture, THUMB_W, THUMB_H, px0, py0, px1, py1, 0xFFFFFFFFu);
	} else {
		const char* what = !info || !info->used ? (current ? "" : tr("Пусто")) : !info->readable ? tr("Файл повреждён") : tr("Нет картинки");
		ui.text((px0 + px1) / 2, (py0 + py1) / 2 - lh / 2, what, ui::DARK, ui::TEXT, ui::CENTER);
	}
	ui.frame(px0 - 1, py0 - 1, px1 + 1, py1 + 1, 0x8C7A50FFu);
	y = py1 + 6;
	if (shown) {
		const char* title = current ? tr("Новое сохранение") : slotOfEntry(_page, sel) == 0 ? tr("Быстрое сохранение") : slotOfEntry(_page, sel) == SAVE_AUTO ? tr("Автосохранение") : shown->title[0] ? shown->title : tr("Без названия");
		ui.text(rx0, y, fitText(ui, title, rw, cut, sizeof(cut)), ui::GOLD);
		y += lh;
		if (shown->world[0]) {
			ui.text(rx0, y, fitText(ui, worldTitle(shown->world), rw, cut, sizeof(cut)), ui::WHITE);
			y += lh;
		}
		char when[48];
		formatGameTime(when, sizeof(when), shown->day, shown->timeOfDay);
		if (shown->chapter > 0) {
			snprintf(buf, sizeof(buf), tr("%s, глава %d"), when, int(shown->chapter));
			ui.text(rx0, y, fitText(ui, buf, rw, cut, sizeof(cut)), ui::GREY);
		} else {
			ui.text(rx0, y, fitText(ui, when, rw, cut, sizeof(cut)), ui::GREY);
		}
		y += lh;
		if (!current && shown->savedAt > 0) {
			char date[32];
			formatSaveDate(date, sizeof(date), shown->savedAt);
			snprintf(buf, sizeof(buf), tr("Сохранено: %s"), date);
			ui.text(rx0, y, fitText(ui, buf, rw, cut, sizeof(cut)), ui::GREY);
			y += lh;
		}
		if (!shown->legacy) {
			char played[32];
			formatPlayTime(played, sizeof(played), shown->playSeconds);
			snprintf(buf, sizeof(buf), tr("Время в игре: %s"), played);
			ui.text(rx0, y, fitText(ui, buf, rw, cut, sizeof(cut)), ui::GREY);
		}
	}

	if (message && *message) ui.text(W / 2, foot - lh - 1, message, ui::GREEN, ui::TEXT, ui::CENTER);
	ui.text(W / 2, foot, saving ? tr("A — сохранить   B — назад") : tr("A — загрузить   B — назад"), ui::DARK + 0x20202000u, ui::TEXT, ui::CENTER);
}

void Menu::draw(Ui& ui, const Settings& s, const char* status, const char* message, const SaveMenuView* saves) const {
	if (!_open) return;
	if (savePage(_page)) {
		drawSaves(ui, message, saves);
		return;
	}
	const PageDef& page = kPages[_page];
	const int line = Ui::lineHeight(ui::TEXT) + 3;
	const int w = 320, x0 = (gfx::SCREEN_W - w) / 2, x1 = x0 + w;
	const int top = 16, bottom = gfx::SCREEN_H - 12;
	ui.rect(0, 0, gfx::SCREEN_W, gfx::SCREEN_H, 0x00000070u);
	ui.box(x0, top, x1, bottom, 0x14100CD0u);
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
	ui.text(gfx::SCREEN_W / 2, foot, _page == PAGE_MAIN && _root == PAGE_MAIN ? tr("A — выбрать   B, START — вернуться в игру") : tr("A — выбрать   \xE2\x97\x80\xE2\x96\xB6 — изменить   B — назад"),
	        ui::DARK + 0x20202000u, ui::TEXT, ui::CENTER);
}

} // namespace g3
