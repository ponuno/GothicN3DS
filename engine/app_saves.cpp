// The saved games (saves.h): the ten slots and the quick one, their pictures,
// the menu's pages of them and the screen with the progress bar that saving
// and loading show. The position itself is App::writeState / loadState's.
#include "lang.h"
#include "app.h"
#include "saves.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace g3 {

namespace {

void makeDir(const char* dir) {
#ifdef _WIN32
	_mkdir(dir);
#else
	mkdir(dir, 0777);
#endif
}

} // namespace

void App::setSaveDir(const char* dir) { snprintf(_saveDir, sizeof(_saveDir), "%s", dir ? dir : ""); }

void App::slotPath(int slot, char* out, u32 size) const {
	if (slot <= 0) {
		snprintf(out, size, "%s", _statePath ? _statePath : "state.bin");
		return;
	}
	if (_saveDir[0]) {
		snprintf(out, size, "%s/slot%02d.sav", _saveDir, slot);
		return;
	}
	// Beside the quick save.
	const char* slash = _statePath ? strrchr(_statePath, '/') : nullptr;
	if (slash) snprintf(out, size, "%.*s/saves/slot%02d.sav", int(slash - _statePath), _statePath, slot);
	else snprintf(out, size, "saves/slot%02d.sav", slot);
}

bool App::canSave() const { return _world.isOpen() && !heroDead() && !talking(); }

SaveInfo App::currentSaveInfo() const {
	SaveInfo s;
	s.used = s.readable = true;
	snprintf(s.world, sizeof(s.world), "%s", worldShortName(worldName()));
	s.day = _npcs.loaded() ? _npcs.game().day() : 0;
	s.timeOfDay = _env.timeOfDay;
	s.chapter = _npcs.loaded() ? _npcs.game().chapterNumber() : 0;
	s.playSeconds = u32(_playSeconds);
	s.savedAt = i64(time(nullptr));
	s.picture = _savePicOk;
	return s;
}

void App::captureSavePicture() {
	if (!_savePic) _savePic = static_cast<u16*>(malloc(size_t(THUMB_W) * THUMB_H * sizeof(u16)));
	_savePicOk = _savePic && gfx::captureFrame(_savePic, THUMB_W, THUMB_H);
}

gfx::Texture* App::thumbTexture(const u16* thumb) const {
	if (!thumb) return nullptr;
	u16* px = static_cast<u16*>(calloc(size_t(THUMB_TEX_W) * THUMB_TEX_H, sizeof(u16)));
	if (!px) return nullptr;
	for (int y = 0; y < THUMB_H; ++y) memcpy(px + size_t(y) * THUMB_TEX_W, thumb + size_t(y) * THUMB_W, size_t(THUMB_W) * 2);
	gfx::Texture* t = gfx::uiTextureCreateColor(u16(THUMB_TEX_W), u16(THUMB_TEX_H), px);
	free(px);
	return t;
}

void App::freeSlotPicture() {
	if (_slotPic) gfx::textureDestroy(_slotPic);
	_slotPic = nullptr;
	_slotPicFor = -100;
}

// The header (name, world, hour, picture), then the position, into a file beside the old one that then takes its place:
// a save that fails half way leaves the one before it whole.
bool App::writeSave(const char* path, const char* title) {
	if (!_world.isOpen() || !path) return false;
	// (fix151: a save holds the character as himself; a transformation ends with it.)
	if (heroTransformed()) heroTransformBack();
	// The picture: the one of the menu's opening, or (a quick save) the frame on show now.
	if (!_menu.isOpen()) captureSavePicture();
	SaveInfo info = currentSaveInfo();
	if (title && *title) copySaveTitle(info.title, sizeof(info.title), title);
	else defaultSaveTitle(info.title, sizeof(info.title), info.world, info.day);
	progress(0.1f);
	char temp[220];
	snprintf(temp, sizeof(temp), "%s.tmp", path);
	FILE* f = fopen(temp, "wb");
	if (!f) return false;
	bool ok = writeSaveHeader(f, info, _savePicOk ? _savePic : nullptr);
	ok = writeState(f) && ok;
	ok = fclose(f) == 0 && ok;
	if (!ok) {
		remove(temp);
		return false;
	}
	progress(0.9f);
	remove(path);
	if (rename(temp, path) != 0) return false;
	progress(1.0f);
	return true;
}

bool App::saveSlot(int slot, const char* title) {
	_menuMessage[0] = 0;
	if (slot < 1 || slot > SAVE_SLOTS) return false;
	if (!canSave()) {
		snprintf(_menuMessage, sizeof(_menuMessage), "%s", tr("Сейчас сохранить нельзя"));
		return false;
	}
	char path[200];
	slotPath(slot, path, sizeof(path));
	if (char* slash = strrchr(path, '/')) {
		*slash = 0;
		makeDir(path);
		*slash = '/';
	}
	char name[48];
	if (title && *title) copySaveTitle(name, sizeof(name), title);
	else defaultSaveTitle(name, sizeof(name), worldShortName(worldName()), _npcs.loaded() ? _npcs.game().day() : 0);
	gfx::Texture* pic = _slotPicFor == -2 && _slotPic ? _slotPic : nullptr;
	gfx::Texture* own = pic || !_savePicOk ? nullptr : thumbTexture(_savePic);
	progressBegin(tr("Сохранение…"), name, pic ? pic : own);
	const bool ok = writeSave(path, name);
	progressEnd();
	if (own) gfx::textureDestroy(own);
	_slotsPage = -1; // (read again)
	if (!ok) {
		snprintf(_menuMessage, sizeof(_menuMessage), "%s", tr("Не удалось сохранить"));
		return false;
	}
	_menu.close();
	toast(tr("Игра сохранена"));
	return true;
}

bool App::loadSlot(int slot) {
	_menuMessage[0] = 0;
	if (slot < 0 || slot > SAVE_SLOTS) return false;
	char path[200];
	slotPath(slot, path, sizeof(path));
	SaveInfo info;
	u16* thumb = static_cast<u16*>(malloc(size_t(THUMB_W) * THUMB_H * sizeof(u16)));
	const bool readable = readSaveInfo(path, info, thumb);
	if (!info.used) {
		free(thumb);
		snprintf(_menuMessage, sizeof(_menuMessage), "%s", tr("Ячейка пуста"));
		return false;
	}
	if (!readable) {
		free(thumb);
		snprintf(_menuMessage, sizeof(_menuMessage), "%s", tr("Файл сохранения повреждён"));
		return false;
	}
	gfx::Texture* pic = info.picture && thumb ? thumbTexture(thumb) : nullptr;
	free(thumb);
	progressBegin(tr("Загрузка…"), slot == 0 ? tr("Быстрое сохранение") : info.title[0] ? info.title : tr("Без названия"), pic, true);
	const bool ok = loadState(path);
	progressEnd();
	if (pic) gfx::textureDestroy(pic);
	_slotsPage = -1;
	if (!ok) {
		snprintf(_menuMessage, sizeof(_menuMessage), "%s", tr("Не удалось загрузить"));
		return false;
	}
	_menu.close();
	if (_startActive) closeStartMenu(); // (loaded from the start menu: the game begins)
	toast(tr("Игра загружена"));
	return true;
}

// The save and load pages: the slots read when the page opens, the picture of the entry selected when it changes.
void App::syncSaveMenu() {
	const Menu::Page page = _menu.page();
	if (!_menu.isOpen() || !Menu::savePage(page)) {
		_slotsPage = -1;
		return;
	}
	if (_slotsPage != int(page)) {
		refreshSlots();
		_slotsPage = int(page);
		_menuMessage[0] = 0;
		// The cursor on the newest save (as the original's list opens).
		int best = -1;
		i64 newest = 0;
		for (int k = page == Menu::PAGE_SAVE ? 1 : 0; k <= SAVE_SLOTS; ++k)
			if (_slots[k].used && _slots[k].readable && _slots[k].savedAt >= newest && (best < 0 || _slots[k].savedAt > newest)) {
				best = k;
				newest = _slots[k].savedAt;
			}
		if (best < 0) best = page == Menu::PAGE_SAVE ? 1 : 0;
		_menu.select(page == Menu::PAGE_SAVE ? best - 1 : best);
		freeSlotPicture();
	}
	const int slot = _menu.saveSlot();
	const SaveInfo& info = _slots[slot >= 0 && slot <= SAVE_SLOTS ? slot : 0];
	const bool current = page == Menu::PAGE_SAVE && !info.used;
	const int key = current ? -2 : slot;
	if (key != _slotPicFor) {
		freeSlotPicture();
		_slotPicFor = key;
		if (current) {
			if (_savePicOk) _slotPic = thumbTexture(_savePic);
		} else if (info.picture) {
			char path[200];
			slotPath(slot, path, sizeof(path));
			u16* thumb = static_cast<u16*>(malloc(size_t(THUMB_W) * THUMB_H * sizeof(u16)));
			SaveInfo again;
			if (thumb && readSaveInfo(path, again, thumb) && again.picture) _slotPic = thumbTexture(thumb);
			free(thumb);
		}
	}
	_saveView.slots = _slots;
	_saveView.picture = _slotPic;
	_saveView.current = current;
	_saveView.now = currentSaveInfo();
}

// ---- the progress screen ----

void App::progressBegin(const char* what, const char* title, gfx::Texture* picture, bool loading) {
	_progress.on = _ui.ready();
	// Loading: the original's screen (LOADING.TGA, the bar of PROGRESS.TGA and PROGRESS_BAR.TGA), if the archive has it.
	if (loading && _progress.on && loadUiPicture(_pak, "loading", _loadingPic)) {
		loadUiPicture(_pak, "progress", _progressBox);
		loadUiPicture(_pak, "progress_bar", _progressBar);
	}
	_progress.share = 0.0f;
	snprintf(_progress.what, sizeof(_progress.what), "%s", what ? what : "");
	snprintf(_progress.title, sizeof(_progress.title), "%s", title ? title : "");
	_progress.picture = picture;
	progress(0.0f);
}

void App::progress(float share) {
	if (!_progress.on) return;
	_progress.share = clamp(max(share, _progress.share), 0.0f, 1.0f);
	drawProgressFrame();
}

void App::progressEnd() {
	_progress.on = false;
	_progress.picture = nullptr;
	freeUiPicture(_loadingPic);
	freeUiPicture(_progressBox);
	freeUiPicture(_progressBar);
}

gfx::FrameDesc App::blankFrame() {
	gfx::FrameDesc fd {};
	fd.view = viewLH({0, 0, 0}, 0, 0);
	fd.fovY = 1.0472f;
	fd.zNear = 10.0f;
	fd.zFar = 1000.0f;
	fd.fogStart = 900.0f;
	fd.fogEnd = 1000.0f;
	fd.dayLight[0] = fd.dayLight[1] = fd.dayLight[2] = 1.0f;
	return fd;
}

void App::drawProgressFrame() {
	const gfx::FrameDesc fd = blankFrame();
	const int W = gfx::SCREEN_W, H = gfx::SCREEN_H;
	const auto drawScreen = [&]() {
		gfx::setPass(gfx::PASS_SKY);
		if (_loadingPic.loaded()) {
			// The original's loading screen (Gothic: the bar half the screen wide, three quarters down - as OpenGothic draws it).
			drawUiPicture(_ui, _loadingPic, 0, 0, W, H);
			const int bw = W / 2, bh = max(H / 20, 8), bx = (W - bw) / 2, by = H * 3 / 4;
			const float v = max(_progress.share, 0.1f);
			if (_progressBox.loaded()) {
				drawUiPicture(_ui, _progressBox, bx, by, bx + bw, by + bh);
				const int padL = bw * 30 / max(int(_progressBox.origW), 1), padT = bh * 5 / max(int(_progressBox.origH), 1);
				const int x1 = bx + padL + int(float(bw - 2 * padL) * v);
				if (_progressBar.loaded()) drawUiPicture(_ui, _progressBar, bx + padL, by + padT, x1, by + bh - padT);
				else _ui.rect(bx + padL, by + padT, x1, by + bh - padT, 0xB4462CFFu);
			} else {
				_ui.frame(bx - 1, by - 1, bx + bw + 1, by + bh + 1, 0x8C7A50FFu);
				_ui.bar(bx, by, bx + bw, by + bh, v, 0xB4462CFFu, 0x000000A0u);
			}
			_ui.flush();
			return;
		}
		// The picture of the save behind it all, darkened (the original's loading screen shows its own picture).
		if (_progress.picture) _ui.picture(_progress.picture, THUMB_W, THUMB_H, 0, 0, W, H, 0x6E6E6EFFu);
		const int barY = H - 34;
		_ui.rect(0, barY - 46, W, H, 0x000000A0u);
		_ui.text(W / 2, barY - 40, _progress.what, ui::GOLD, ui::TITLE, ui::CENTER);
		if (_progress.title[0]) _ui.text(W / 2, barY - 22, _progress.title, ui::WHITE, ui::TEXT, ui::CENTER);
		const int x0 = 60, x1 = W - 60;
		_ui.frame(x0 - 2, barY - 2, x1 + 2, barY + 10, 0x8C7A50FFu);
		_ui.bar(x0, barY, x1, barY + 8, _progress.share, ui::GOLD, 0x000000C0u);
		char pct[16];
		snprintf(pct, sizeof(pct), "%d %%", int(_progress.share * 100.0f + 0.5f));
		_ui.text(W / 2, barY + 13, pct, ui::GREY, ui::TEXT, ui::CENTER);
		_ui.flush();
	};
	gfx::frameBegin(fd);
	drawScreen();
	if (gfx::stereoActive()) {
		gfx::eyeBegin(fd);
		drawScreen();
	}
	gfx::frameEnd();
	++_progressFrames;
}

// ---- the start menu ----

const char* App::versionText() { return "Gothic 3DS fix196"; }

void App::openStartMenu() {
	if (!_start.loaded()) _start.load(_pak);
	char path[200];
	slotPath(0, path, sizeof(path));
	SaveInfo quick;
	const bool canContinue = readSaveInfo(path, quick, nullptr) && quick.readable;
	_start.open(canContinue);
	_startActive = true;
	_menu.close();
}

void App::closeStartMenu() {
	_startActive = false;
	_start.close();
	_start.release(); // (its pictures: some 0.5 MB of the linear heap)
	_menuShot = false;
}

void App::menuSound(const char* name) {
	if (_sound.ready() && _sound.knows(name)) _sound.play(name, nullptr, 1.0f);
}

App::StartResult App::startMenuInput(const MenuInput& in) {
	StartResult r;
	if (!_startActive) return r;
	if (_menu.isOpen()) {
		// The port's own pages over the start screen (the load page, the options); B on their first page comes back here.
		const MenuAction act = _menu.update(in, _settings);
		if (act == MENU_CHANGED) {
			r.settingsChanged = true;
		} else if (act == MENU_RESET_GRAPHICS) {
			menuAction(act, nullptr);
			r.settingsChanged = true;
		} else if (act == MENU_LOAD_SLOT) {
			loadSlot(_menu.saveSlot()); // (closes the start menu if it loads)
		}
		return r;
	}
	bool moved = false;
	const StartAction a = _start.update(in, &moved);
	if (moved || a != START_NONE) _toastTime = 0.0f; // (a line of news stays until the next step: nothing counts time here)
	if (moved) menuSound("MENU_BROWSE");
	if (a != START_NONE) menuSound("MENU_SELECT");
	switch (a) {
	case START_NEW_GAME:
		// The world, the people and the scripts are as init() made them: the story begins.
		closeStartMenu();
		break;
	case START_CONTINUE:
		if (!loadSlot(0)) toast(_menuMessage[0] ? _menuMessage : tr("Не удалось загрузить"));
		break;
	case START_LOAD: _menu.open(Menu::PAGE_LOAD); break;
	case START_OPTIONS: _menu.open(Menu::PAGE_OPTIONS); break;
	case START_EXIT: r.exit = true; break;
	default: break;
	}
	return r;
}

void App::renderStartMenu() {
	const gfx::FrameDesc fd = blankFrame();
	const auto drawScreen = [&]() {
		gfx::setPass(gfx::PASS_SKY);
		if (!_ui.ready()) return;
		_start.drawBackground(_ui);
		if (_menu.isOpen()) {
			syncSaveMenu();
			_menu.draw(_ui, _settings, nullptr, _menuMessage, &_saveView);
		} else {
			_start.draw(_ui, versionText());
			if (_toastTime > 0.0f && _toast[0]) {
				const int w = _ui.measure(_toast) + 16;
				_ui.rect(gfx::SCREEN_W / 2 - w / 2, 4, gfx::SCREEN_W / 2 + w / 2, 6 + Ui::lineHeight(), 0x000000A0u);
				_ui.text(gfx::SCREEN_W / 2, 5, _toast, ui::WHITE, ui::TEXT, ui::CENTER);
			}
		}
		_ui.flush();
	};
	gfx::frameBegin(fd);
	drawScreen();
	if (gfx::stereoActive()) {
		gfx::eyeBegin(fd);
		drawScreen();
	}
	gfx::frameEnd();
}

} // namespace g3
