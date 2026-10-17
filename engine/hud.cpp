// What is drawn over the picture (App::drawOverlay): the HUD - the
// character's health, the target, a line of news - and the pause menu.
#include "lang.h"
#include "app.h"
#include "g3ds/card.h"
#include "gfx.h"

#include <stdio.h>
#include <string.h>

namespace g3 {

void App::drawOverlay() {
	if (!_ui.ready()) return;
	const bool menuOpen = _menu.isOpen();
	const int W = gfx::SCREEN_W, H = gfx::SCREEN_H;
	const Game* game = _npcs.loaded() ? &_npcs.game() : nullptr;
	const bool character = _hero.loaded() && !_player.flying;
	// (While the inventory's info box is up, it has the bottom of the picture.)
	const bool items = itemOverlay();
	if (!menuOpen && character && _npcs.loaded()) drawLowHealth();
	if (_settings.hud && !menuOpen && character) {
		// The character's health, bottom left - only while the bottom screen
		// shows the report: otherwise its bars are there, as on the console.
		const i32 hp = heroHitPoints(), hpMax = heroHitPointsMax();
		if (game && hpMax > 0 && !items && _settings.report) {
			char buf[32];
			snprintf(buf, sizeof(buf), "%d / %d", int(hp), int(hpMax));
			_ui.bar(10, H - 14, 110, H - 8, float(hp) / float(hpMax), ui::RED);
			_ui.frame(9, H - 15, 111, H - 7, 0x86683BE0u);
			_ui.text(10, H - 15 - Ui::lineHeight(), buf, ui::GREY);
		}
		// fix210: the breath, over the health, while there is less than all of it.
		if (_player.swimming && _player.breath < _player.config.airTime - 0.5f && !items) {
			_ui.bar(10, H - 24, 110, H - 18, _player.breath / _player.config.airTime, 0x7DC8E6FFu);
			_ui.frame(9, H - 25, 111, H - 17, 0x86683BE0u);
		}
		// What is in the hands, bottom right.
		const u8 w = _hero.weapon();
		const char* held = nullptr;
		char weaponName[48] = "";
		if (w == WEAPON_FIST) held = tr("Кулаки");
		else if (w == WEAPON_1H || w == WEAPON_2H) {
			const GameNpc* h = heroNpc();
			const GameItemInfo* it = h && h->melee != SCRIPT_NONE && game ? _npcs.game().itemInfo(h->melee) : nullptr;
			snprintf(weaponName, sizeof(weaponName), "%s", it && it->name ? game->text(it->name) : tr("Оружие"));
			held = weaponName;
		}
		if (w == WEAPON_MAG && game) {
			// The spell in hand, and the mana (a bar as the hit points have, charged part brighter).
			const GameItemInfo* it = _spellItem != SCRIPT_NONE ? _npcs.game().itemInfo(_spellItem) : nullptr;
			snprintf(weaponName, sizeof(weaponName), "%s", it ? game->text(it->description ? it->description : it->name) : tr("Заклинание"));
			held = weaponName;
			const GameNpc* h = heroNpc();
			const i32 mana = h ? game->attributeOf(*h, game->constant("ATR_MANA", 2)) : 0;
			const i32 manaMax = h ? game->attributeOf(*h, game->constant("ATR_MANA_MAX", 3)) : 0;
			if (manaMax > 0 && !items) {
				_ui.bar(W - 110, H - 14, W - 10, H - 8, float(mana) / float(manaMax), 0x4678E0FFu);
				_ui.frame(W - 111, H - 15, W - 9, H - 7, 0x86683BE0u);
				if (_cast.stage == CAST_INVEST) _ui.text(W - 10, H - 15 - 2 * Ui::lineHeight(), tr("Заряд"), ui::GREY, ui::TEXT, ui::RIGHT);
			}
		}
		if ((w == WEAPON_BOW || w == WEAPON_CBOW) && game) {
			// The bow in hand, and how many arrows are left.
			const GameNpc* h = heroNpc();
			const GameItemInfo* it = h && h->ranged != SCRIPT_NONE ? _npcs.game().itemInfo(h->ranged) : nullptr;
			const i32 n = it && it->munition >= 0 ? _npcs.game().itemCount(u32(heroIndex()), u32(it->munition)) : 0;
			snprintf(weaponName, sizeof(weaponName), "%s  x%d", it && it->name ? game->text(it->name) : tr("Лук"), int(n));
			held = weaponName;
		}
		if (items) {
		} else if (held) _ui.text(W - 10, H - 6 - Ui::lineHeight(), held, ui::GOLD, ui::TEXT, ui::RIGHT);
		else if (_hero.sneaking()) _ui.text(W - 10, H - 6 - Ui::lineHeight(), tr("Крадётся"), ui::GREY, ui::TEXT, ui::RIGHT);
		// The target: its name and health at the top, a mark over its head.
		// (Not during a talk: the partner's name is the dialogue box's, the target's
		// white name above it was a stray second copy.)
		if (game && _target >= 0 && !_npcs.talkView().active) {
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
				_ui.text(W / 2, H / 2 - 22, tr("Вы погибли"), ui::RED, ui::TITLE, ui::CENTER);
				if (_deadTime > 1.5f) _ui.text(W / 2, H / 2 + 2, tr("A — загрузить позицию"), ui::GREY, ui::TEXT, ui::CENTER);
			} else {
				_ui.text(W / 2, H / 2 - 8, tr("Без сознания…"), ui::GREY, ui::TITLE, ui::CENTER);
			}
		}
	}
	if (_settings.hud && !menuOpen && character) drawItemHud();
	if (_settings.showFps) {
		char fps[16];
		snprintf(fps, sizeof(fps), "%d FPS", int(_fps + 0.5f));
		_ui.text(W - 6, 4, fps, ui::GOLD, ui::TEXT, ui::RIGHT);
	}
	if (!menuOpen && _chapterTime > 0.0f) {
		// The card of a chapter: it fades in, stays, fades out.
		const float t = _chapterLength - _chapterTime;
		const float a = min(min(t / 0.8f, _chapterTime / 1.0f), 1.0f);
		const u32 alpha = u32(clamp(a, 0.0f, 1.0f) * 255.0f);
		if (_chapterPic) {
			// The game's picture of the chapter fills the screen; over it, as OpenGothic's ChapterScreen puts them,
			// the title near the top and the subtitle near the bottom (fix168: the words were left off).
			_ui.rect(0, 0, W, H, (0x000000u << 8) | alpha);
			_ui.picture(_chapterPic, CARD_W, CARD_H, u8(alpha));
			_ui.text(W / 2, 22, _chapterCard.title, (ui::WHITE & 0xFFFFFF00u) | alpha, ui::TITLE, ui::CENTER);
			_ui.text(W / 2, H - 22 - Ui::lineHeight(), _chapterCard.subtitle, (ui::WHITE & 0xFFFFFF00u) | alpha, ui::TEXT, ui::CENTER);
		} else {
			_ui.rect(0, 0, W, H, (0x000000u << 8) | (alpha * 200u / 255u));
			_ui.text(W / 2, H / 2 - 26, _chapterCard.title, (ui::GOLD & 0xFFFFFF00u) | alpha, ui::TITLE, ui::CENTER);
			_ui.text(W / 2, H / 2 + 6, _chapterCard.subtitle, (ui::WHITE & 0xFFFFFF00u) | alpha, ui::TEXT, ui::CENTER);
		}
	}
	if (_timeFade > 0.0f) {
		// The clock was set: black in, black out, the time changes at the dark.
		const float t = 1.0f - _timeFade / kTimeFadeLength;
		const float a = t < 0.5f ? t * 2.0f : (1.0f - t) * 2.0f;
		_ui.rect(0, 0, W, H, (0x000000u << 8) | u32(clamp(a, 0.0f, 1.0f) * 255.0f));
	}
	if (!menuOpen) drawDocument();
	if (!menuOpen) drawSubtitle();
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
		syncSaveMenu();
		_menu.draw(_ui, _settings, status, _menuMessage, &_saveView);
	}
	_ui.flush();
}

} // namespace g3
