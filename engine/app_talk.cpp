// Talking to the inhabitants: A at a character in front of the hero begins a
// talk (the scripts run it, npcs.h); while it lasts the hero stands facing the
// partner, the bottom screen is the dialogue page and the words come up as
// the dialogue box at the top of the top one.
#include "lang.h"
#include "app.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gfx.h"
#include "ui.h"

namespace g3 {

namespace {

float turnToward(float from, float to, float maxStep) {
	float d = to - from;
	while (d > 3.14159265f) d -= 6.2831853f;
	while (d < -3.14159265f) d += 6.2831853f;
	if (d > maxStep) d = maxStep;
	if (d < -maxStep) d = -maxStep;
	return from + d;
}

} // namespace

void App::talkUpdate(Input& in, float dt) {
	const int hi = heroIndex();
	const bool on = _npcs.talkView().active;
	if (on) {
		_talkTarget = -1;
		// The hands are the talk's: no blows meanwhile.
		in.attack = in.parry = in.drawWeapon = in.meleeSlot = in.fistSlot = false;
		in.confirm = false;
		const int partner = _npcs.talkPartner();
		if (hi < 0 || _hero.down() || !_hero.loaded()) {
			_npcs.talkAbort();
		} else if (partner >= 0) {
			const GameNpc& n = _npcs.game().npc(u32(partner));
			const float want = atan2f(n.pos[0] - _player.feet.x, n.pos[2] - _player.feet.z);
			_hero.setYaw(turnToward(_hero.yaw(), want, 6.0f * dt));
		}
		// The talk's page is the menu of its ways - or, while the trader's wares are up, the trade page.
		const bool trading = _npcs.trading();
		const InventoryPanel::PanelPage want = trading ? InventoryPanel::PAGE_CHEST : InventoryPanel::PAGE_DIALOG;
		const InventoryPanel::PanelPage other = trading ? InventoryPanel::PAGE_DIALOG : InventoryPanel::PAGE_CHEST;
		if (_panel.page() == InventoryPanel::PAGE_PLAY || (_panel.page() == other && _chest.stage == CH_NONE)) {
			_panel.openPage(want);
			if (trading) _panel.setSide(0);
			_invStamp = 0;
		}
	} else {
		// (The wares may still be up if the talk was cut short.)
		if (_panel.page() == InventoryPanel::PAGE_DIALOG || (_panel.page() == InventoryPanel::PAGE_CHEST && _chest.stage == CH_NONE)) {
			_panel.openPage(InventoryPanel::PAGE_PLAY);
			_invStamp = 0;
		}
	}
	// A chapter begins (IntroduceChapter): its card comes up.
	{
		const GameChapter& ch = _npcs.game().chapter();
		if (ch.serial != _chapterSeen) {
			_chapterSeen = ch.serial;
			_chapterCard = ch;
			_chapterLength = _chapterTime = clamp(float(ch.delay) / 1000.0f, 3.0f, 10.0f);
			if (ch.sound[0] && _sound.ready()) {
				char name[48];
				snprintf(name, sizeof(name), "%s", ch.sound);
				char* dot = strrchr(name, '.');
				if (dot) *dot = 0;
				_sound.play(name, nullptr);
			}
		}
		if (_chapterTime > 0.0f) _chapterTime -= dt;
	}
	// The diary's entries are "seen" once the diary has been looked at and left.
	{
		const bool diary = _panel.page() == InventoryPanel::PAGE_DIARY;
		if (_diaryOpen && !diary) _diarySeen = _npcs.game().logVersion();
		_diaryOpen = diary;
	}
	// What the scripts print (Print, PrintScreen), and what the diary has done.
	if (_toastTime <= 0.0f) {
		GameMessage m;
		GameLogNotice note;
		if (_npcs.game().takeMessage(m)) {
			toast(m.text, m.seconds > 0.0f ? m.seconds : 2.5f);
		} else if (_npcs.game().takeLogNotice(note)) {
			char topic[64];
			scriptText(note.topic, topic, sizeof(topic));
			// (A long name is cut so that the line fits the screen.)
			if (_ui.ready()) {
				for (size_t n = strlen(topic); n > 4 && _ui.measure(topic) > 200; topic[--n] = 0) {
				}
			}
			const bool task = note.section == 0;
			const char* format = note.kind == LOGN_NEW_TOPIC ? (task ? tr("Новое задание: %s") : tr("Новая заметка: %s"))
			                     : note.kind == LOGN_STATUS ? (note.status == 2 ? tr("Задание выполнено: %s") : tr("Задание провалено: %s"))
			                                                : tr("Дневник обновлён: %s");
			char line[160];
			snprintf(line, sizeof(line), format, topic);
			toast(line, 3.0f);
		}
	}
	if (on) return;
	_talkTarget = -1;
	if (hi < 0 || !_hero.loaded() || _hero.down() || _hero.weapon() != WEAPON_NONE || _hero.drawing() || _job.stage != JOB_NONE ||
	    _panel.open() || _player.swimming || _player.climbing || _hero.mantling() || !_player.grounded || _chest.stage != CH_NONE ||
	    _lootTarget >= 0 || _chestTarget >= 0 || _corpseTarget >= 0 || _mobTarget >= 0 || _mobJob.stage != MJ_NONE || _menu.isOpen()) {
		return;
	}
	// (A scan over the few characters about is as cheap as the chests' one.)
	const float feet[3] = {_player.feet.x, _player.feet.y, _player.feet.z};
	const int t = _npcs.pickTarget(feet, _hero.yaw(), 320.0f, 0.8f);
	if (t >= 0 && _npcs.canTalk(u32(t))) _talkTarget = t;
	if (_talkTarget >= 0 && (in.confirm || in.attack)) {
		in.confirm = in.attack = false;
		if (!_npcs.talkTo(u32(_talkTarget))) toast(tr("Сейчас не до разговоров"));
	}
}

void App::drawSubtitle() {
	const TalkView& v = _npcs.talkView();
	if (!v.active || !v.saying || !v.line[0]) return;
	const int W = gfx::SCREEN_W;
	// A greedy wrap by words into at most four lines.
	char lines[4][320];
	int count = 0;
	const int maxW = W - 40;
	char cur[320] = "";
	const char* p = v.line;
	while (*p && count < 4) {
		const char* e = p;
		while (*e && *e != ' ') ++e;
		char word[120];
		const size_t len = size_t(e - p) < sizeof(word) - 1 ? size_t(e - p) : sizeof(word) - 1;
		memcpy(word, p, len);
		word[len] = 0;
		char trial[320];
		snprintf(trial, sizeof(trial), cur[0] ? "%s %s" : "%s%s", cur, word);
		if (cur[0] && _ui.measure(trial) > maxW) {
			snprintf(lines[count++], sizeof(lines[0]), "%s", cur);
			snprintf(cur, sizeof(cur), "%s", word);
		} else {
			snprintf(cur, sizeof(cur), "%s", trial);
		}
		p = *e ? e + 1 : e;
	}
	if (cur[0] && count < 4) snprintf(lines[count++], sizeof(lines[0]), "%s", cur);
	if (!count) return;
	const int lh = Ui::lineHeight();
	// At the top of the picture, as the game's dialogue box: the speaker's name and the words.
	const int top = 10;
	_ui.rect(10, top - 5, W - 10, top + (count + 1) * lh + 3, 0x000000B8u);
	_ui.text(W / 2, top, v.speaker, v.heroSays ? ui::WHITE : ui::GOLD, ui::TEXT, ui::CENTER);
	for (int i = 0; i < count; ++i) _ui.text(W / 2, top + (i + 1) * lh, lines[i], ui::WHITE, ui::TEXT, ui::CENTER);
}

} // namespace g3
