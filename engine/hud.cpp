// What is drawn over the picture (App::drawOverlay): the HUD - the
// character's health, the target, a line of news - and the pause menu.
#include "app.h"

#include <stdio.h>
#include <string.h>

namespace g3 {

void App::drawOverlay() {
	if (!_ui.ready()) return;
	const bool menuOpen = _menu.isOpen();
	const int W = gfx::SCREEN_W, H = gfx::SCREEN_H;
	const Game* game = _npcs.loaded() ? &_npcs.game() : nullptr;
	const bool character = _hero.loaded() && !_player.flying;
	// (While the inventory's info box and quick slots are up, they have the bottom of the picture.)
	const bool items = itemOverlay() || _quickHint;
	if (_settings.hud && !menuOpen && character) {
		// The character's health, bottom left.
		const i32 hp = heroHitPoints(), hpMax = heroHitPointsMax();
		if (game && hpMax > 0 && !items) {
			char buf[32];
			snprintf(buf, sizeof(buf), "%d / %d", int(hp), int(hpMax));
			_ui.bar(10, H - 14, 110, H - 8, float(hp) / float(hpMax), ui::RED);
			_ui.frame(9, H - 15, 111, H - 7, 0x00000090u);
			_ui.text(10, H - 15 - Ui::lineHeight(), buf, ui::GREY);
		}
		// What is in the hands, bottom right.
		const u8 w = _hero.weapon();
		const char* held = nullptr;
		char weaponName[48] = "";
		if (w == WEAPON_FIST) held = "Кулаки";
		else if (w == WEAPON_1H || w == WEAPON_2H) {
			const GameNpc* h = heroNpc();
			const GameItemInfo* it = h && h->melee != SCRIPT_NONE && game ? _npcs.game().itemInfo(h->melee) : nullptr;
			snprintf(weaponName, sizeof(weaponName), "%s", it && it->name ? game->text(it->name) : "Оружие");
			held = weaponName;
		}
		if (items) {
		} else if (held) _ui.text(W - 10, H - 6 - Ui::lineHeight(), held, ui::GOLD, ui::TEXT, ui::RIGHT);
		else if (_hero.sneaking()) _ui.text(W - 10, H - 6 - Ui::lineHeight(), "Крадётся", ui::GREY, ui::TEXT, ui::RIGHT);
		// The target: its name and health at the top, a mark over its head.
		if (game && _target >= 0) {
			const GameNpc& t = game->npc(u32(_target));
			const char* name = game->nameOf(t);
			const int hi = heroIndex();
			const bool hostile = hi >= 0 && game->attitude(u32(_target), u32(hi)) == game->constant("ATT_HOSTILE", 0);
			if (name && *name) _ui.text(W / 2, 4, name, _locked ? ui::GOLD : ui::WHITE, ui::TEXT, ui::CENTER);
			const i32 thp = game->hitPoints(t), tmax = game->attributeOf(t, game->constant("ATR_HITPOINTS_MAX", 1));
			if (tmax > 0 && (w != WEAPON_NONE || thp < tmax))
				_ui.bar(W / 2 - 50, 5 + Ui::lineHeight(), W / 2 + 50, 9 + Ui::lineHeight(), float(thp) / float(tmax),
				        hostile ? ui::RED : 0xA0A0A0FFu);
			if (_locked) {
				int x, y;
				// (Over a person's head; beasts are lower.)
				const bool person = game->guildOf(t) < game->constant("GIL_SEPERATOR_HUM", 16);
				if (projectToScreen({t.pos[0], t.pos[1] + (person ? 205.0f : 130.0f), t.pos[2]}, x, y))
					_ui.text(x, y - Ui::lineHeight(), "\xE2\x96\xBC", ui::GOLD, ui::TEXT, ui::CENTER);
			}
		}
		if (_hero.down()) {
			if (_deadTime > 0.0f) {
				_ui.rect(0, H / 2 - 30, W, H / 2 + 30, 0x000000A0u);
				_ui.text(W / 2, H / 2 - 22, "Вы погибли", ui::RED, ui::TITLE, ui::CENTER);
				if (_deadTime > 1.5f) _ui.text(W / 2, H / 2 + 2, "A — загрузить позицию", ui::GREY, ui::TEXT, ui::CENTER);
			} else {
				_ui.text(W / 2, H / 2 - 8, "Без сознания…", ui::GREY, ui::TITLE, ui::CENTER);
			}
		}
	}
	if (_settings.hud && !menuOpen && character) drawItemHud();
	if (!menuOpen && _toastTime > 0.0f && _toast[0]) {
		// (Above the inventory's info box while that is up.)
		const int w = _ui.measure(_toast) + 16, y = items ? 116 : 150;
		_ui.rect(W / 2 - w / 2, y - 2, W / 2 + w / 2, y + Ui::lineHeight() + 2, 0x000000A0u);
		_ui.text(W / 2, y, _toast, ui::WHITE, ui::TEXT, ui::CENTER);
	}
	if (menuOpen) {
		char status[64];
		const char* world = worldName();
		const char* slash = strrchr(world, '/');
		snprintf(status, sizeof(status), "%s, %02d:%02d", slash ? slash + 1 : world, int(_env.timeOfDay), int(_env.timeOfDay * 60.0f) % 60);
		_menu.draw(_ui, _settings, status, _menuMessage);
	}
	_ui.flush();
}

} // namespace g3
