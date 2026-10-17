// Reading: a book, a letter or a map the scripts put up (Doc_Create ... Doc_Show, game_doc.cpp). The words come
// up on the top screen on a sheet, laid out to its size; the bottom screen is the reading page (panel.cpp), where
// the pages turn. OpenGothic (DocumentMenu) shows all the pages of a document at once on its big screen and closes
// on a key; here the text is cut into screens instead, and the same keys that turn them close the last.
#include "lang.h"
#include "app.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace g3 {

namespace {

constexpr int kSheetMargin = 26; // of the sheet from the screen's edges
constexpr int kTextMargin = 12;  // of the words from the sheet's

} // namespace

int App::docWidth(const char* text) const { return _ui.ready() ? _ui.measure(text) : int(strlen(text)) * 6; }

// Cuts the document into screens: a page of the document begins a new screen, a long line wraps at the sheet's
// width, a full screen goes on to the next.
void App::docLayout(const GameDocument& d) {
	DocReader& r = _doc;
	freeUiPicture(r.pic);
	const u32 held = r.held;
	r = DocReader();
	r.held = held;
	r.map = d.map;
	if (d.map) {
		// fix192: the map's picture (the whole document's background, else its first page's).
		const char* image = d.image[0] ? d.image : d.pages[0].image;
		if (image && image[0] && loadUiPicture(_pak, image, r.pic)) {
			r.zoom = 1.0f;
			r.cx = float(r.pic.width) * 0.5f;
			r.cy = float(r.pic.height) * 0.5f;
		}
	}
	memcpy(r.level, d.level, sizeof(r.level));
	memcpy(r.coords, d.coords, sizeof(r.coords));
	r.serial = _npcs.game().documentSerial();
	const int lh = Ui::lineHeight();
	const int maxLines = clamp((gfx::SCREEN_H - 2 * kSheetMargin - 2 * kTextMargin - lh) / lh, 1, int(DocReader::LINES));
	const int maxWidth = gfx::SCREEN_W - 2 * (kSheetMargin + kTextMargin);
	u32 screen = 0;
	int line = 0;
	bool any = false; // the screen being filled has something on it
	const auto next = [&]() {
		if (!any) return true;
		if (screen + 1 >= DocReader::MAX_SCREENS) return false;
		r.lineCount[screen] = u8(line);
		++screen;
		line = 0;
		any = false;
		return true;
	};
	const auto put = [&](const char* text) {
		if (line >= maxLines && !next()) return false;
		snprintf(r.lines[screen][line], DocReader::LINE, "%s", text);
		++line;
		if (text[0]) any = true;
		return true;
	};
	bool full = false;
	for (u32 p = 0; p < d.pageCount && !full; ++p) {
		const GameDocPage& page = d.pages[p];
		if (!page.text || !page.length) continue;
		if (any && !next()) break;
		const char* cursor = page.text;
		while (*cursor && !full) {
			const char* end = strchr(cursor, '\n');
			const size_t len = end ? size_t(end - cursor) : strlen(cursor);
			char raw[512], text[512];
			snprintf(raw, sizeof(raw), "%.*s", int(len < sizeof(raw) - 1 ? len : sizeof(raw) - 1), cursor);
			scriptText(raw, text, sizeof(text));
			cursor = end ? end + 1 : cursor + len;
			if (!text[0]) {
				if (line > 0 && !put("")) full = true; // (a blank line, but not at the top of a screen)
				continue;
			}
			// A greedy wrap by words.
			char cur[DocReader::LINE] = "";
			const char* w = text;
			while (*w && !full) {
				const char* e = w;
				while (*e && *e != ' ') ++e;
				char word[DocReader::LINE];
				snprintf(word, sizeof(word), "%.*s", int(e - w < int(sizeof(word)) - 1 ? e - w : int(sizeof(word)) - 1), w);
				char trial[2 * DocReader::LINE];
				snprintf(trial, sizeof(trial), cur[0] ? "%s %s" : "%s%s", cur, word);
				if (cur[0] && (docWidth(trial) > maxWidth || strlen(trial) >= DocReader::LINE)) {
					if (!put(cur)) full = true;
					snprintf(cur, sizeof(cur), "%s", word);
				} else {
					snprintf(cur, sizeof(cur), "%s", trial);
				}
				w = *e ? e + 1 : e;
			}
			if (!full && cur[0] && !put(cur)) full = true;
		}
	}
	r.lineCount[screen] = u8(line);
	r.pages = screen + 1;
	// (A trailing blank line is nothing to read.)
	while (r.lineCount[r.pages - 1] > 0 && !r.lines[r.pages - 1][r.lineCount[r.pages - 1] - 1][0]) --r.lineCount[r.pages - 1];
	r.active = true;
}

void App::docUpdate() {
	Game& g = _npcs.game();
	if (g.documentShown() && g.documentSerial() != _doc.serial) {
		// Put up by the scripts: the hero stands over it, the bottom screen has the pages.
		docLayout(*g.documentShown());
		g.documentDone();
		_panel.openPage(InventoryPanel::PAGE_DOC);
		_invStamp = 0;
	}
	// The reading page left by other means (the hero went down, swam): the book is closed.
	if (_doc.active && _panel.page() != InventoryPanel::PAGE_DOC) docClose();
}

void App::docClose() {
	_doc.active = false;
	_doc.page = 0;
	freeUiPicture(_doc.pic); // (fix192)
	if (_doc.held != SCRIPT_NONE) {
		// (fix192) The thing read is put away (the hands come down with it).
		if (_hero.atMob() && _mobJob.stage == MJ_NONE && _job.stage == JOB_NONE) _hero.closeMob();
		_doc.held = SCRIPT_NONE;
	}
	if (_panel.page() == InventoryPanel::PAGE_DOC) _panel.openPage(InventoryPanel::PAGE_PLAY);
	_invStamp = 0;
}

float App::docMapFit() const {
	if (!_doc.pic.loaded()) return 1.0f;
	return min(float(gfx::SCREEN_W) / float(_doc.pic.width), float(gfx::SCREEN_H) / float(_doc.pic.height));
}

void App::docMapPan(float dx, float dy) {
	if (!_doc.pic.loaded()) return;
	const float scale = docMapFit() * _doc.zoom;
	_doc.cx += dx / scale;
	_doc.cy += dy / scale;
	// (The edge of the map does not leave the edge of the screen; a map smaller than the screen stays in its middle.)
	const float halfW = float(gfx::SCREEN_W) * 0.5f / scale, halfH = float(gfx::SCREEN_H) * 0.5f / scale;
	const float w = float(_doc.pic.width), h = float(_doc.pic.height);
	_doc.cx = halfW * 2.0f >= w ? w * 0.5f : clamp(_doc.cx, halfW, w - halfW);
	_doc.cy = halfH * 2.0f >= h ? h * 0.5f : clamp(_doc.cy, halfH, h - halfH);
}

void App::docCommand(const PanelCommand& c) {
	if (c.type == PANEL_MAP) {
		// fix192: the map: larger or smaller (1, 2, 4 times the whole), moved a step.
		if (!_doc.active || !_doc.map) return;
		const float step = 48.0f;
		switch (c.slot) {
		case 0: _doc.zoom = max(_doc.zoom * 0.5f, 1.0f); break;
		case 1: _doc.zoom = min(_doc.zoom * 2.0f, 4.0f); break;
		case 2: docMapPan(-step, 0); break;
		case 3: docMapPan(step, 0); break;
		case 4: docMapPan(0, -step); break;
		case 5: docMapPan(0, step); break;
		case 6: _doc.zoom = _doc.zoom >= 4.0f ? 1.0f : _doc.zoom * 2.0f; break;
		default: break;
		}
		docMapPan(0, 0);
		_invStamp = 0;
		return;
	}
	if (c.type == PANEL_DOC_CLOSE) {
		docClose();
	} else if (c.type == PANEL_DOC_PAGE && _doc.active) {
		if (c.slot == 0) {
			if (_doc.page > 0) --_doc.page;
		} else if (_doc.page + 1 < _doc.pages) {
			++_doc.page;
		} else {
			docClose(); // (past the last page: put down)
		}
		_invStamp = 0;
	}
}

// A document item is read by its use function (ON_STATE[0]): the scripts build the pages and Doc_Show them.
bool App::readItem(u32 symbol) {
	const int hi = heroIndex();
	Game& g = _npcs.game();
	const GameItemInfo* it = hi >= 0 ? g.itemInfo(symbol) : nullptr;
	if (!it || it->onUse == SCRIPT_NONE) {
		itemMessage(tr("Читать нечего"));
		return false;
	}
	if (_player.swimming || _player.climbing || _player.flying) {
		itemMessage(_player.swimming ? tr("Не в воде") : tr("Сейчас нельзя"));
		return false;
	}
	const u32 before = g.documentSerial();
	g.callItemFunction(u32(hi), symbol, it->onUse);
	if (g.documentSerial() == before) {
		itemMessage(tr("Читать нечего"));
		return false;
	}
	docUpdate(); // (at once: the inventory is still up, its page gives way)
	// fix192: the hero holds it while it is read, as the model script has it for its scheme ("T_MAP_STAND_2_S0", "S_MAP_S0").
	if (_doc.active && _doc.held == SCRIPT_NONE && canUseScheme(symbol) && _job.stage == JOB_NONE && _mobJob.stage == MJ_NONE &&
	    _chest.stage == CH_NONE && _hero.weapon() == WEAPON_NONE && !_hero.drawing()) {
		char scheme[32];
		size_t n = 0;
		for (const char* p = g.text(it->scheme); p && *p && n + 1 < sizeof(scheme); ++p) scheme[n++] = *p >= 'a' && *p <= 'z' ? char(*p - 32) : *p;
		scheme[n] = 0;
		if (_hero.openMob(scheme, 0)) _doc.held = symbol;
	}
	return true;
}

void App::drawDocument() {
	if (!_doc.active) return;
	const int W = gfx::SCREEN_W, H = gfx::SCREEN_H;
	const int lh = Ui::lineHeight();
	const int x0 = kSheetMargin, y0 = kSheetMargin - 8, x1 = W - kSheetMargin, y1 = H - kSheetMargin + 8;
	_ui.rect(0, 0, W, H, 0x000000A0u);
	_ui.rect(x0, y0, x1, y1, 0xD9C9A0F2u);
	_ui.frame(x0, y0, x1, y1, 0x5A4630FFu);
	_ui.frame(x0 + 2, y0 + 2, x1 - 2, y1 - 2, 0x8A7050A0u);
	const u32 ink = 0x2A2014FFu;
	if (_doc.map && _doc.pic.loaded()) {
		// fix192: the map's own picture over the whole top screen (larger with A / L / R, moved with the D-Pad and the
		// stick), and where the hero is on it when the scripts say what part of the world it covers.
		_ui.rect(0, 0, W, H, 0x000000FFu);
		const float scale = docMapFit() * _doc.zoom;
		const float pw = float(_doc.pic.width), ph = float(_doc.pic.height);
		// The texels on show, and where they are on the screen.
		const float u0 = max(_doc.cx - float(W) * 0.5f / scale, 0.0f), u1 = min(_doc.cx + float(W) * 0.5f / scale, pw);
		const float v0 = max(_doc.cy - float(H) * 0.5f / scale, 0.0f), v1 = min(_doc.cy + float(H) * 0.5f / scale, ph);
		const auto sx = [&](float u) { return float(W) * 0.5f + (u - _doc.cx) * scale; };
		const auto sy = [&](float v) { return float(H) * 0.5f + (v - _doc.cy) * scale; };
		const int iu0 = int(u0), iv0 = int(v0), iu1 = int(ceilf(u1)), iv1 = int(ceilf(v1));
		_ui.pictureRegion(_doc.pic.tex, iu0, iv0, iu1, iv1, int(sx(float(iu0)) + 0.5f), int(sy(float(iv0)) + 0.5f), int(sx(float(iu1)) + 0.5f),
		                  int(sy(float(iv1)) + 0.5f), 0xFFFFFFFFu);
		const i32 left = _doc.coords[0], top = _doc.coords[1];
		const i32 width = _doc.coords[2] - left, height = _doc.coords[3] - top;
		const char* world = worldName();
		const char* slash = strrchr(world, '/');
		char here[48];
		snprintf(here, sizeof(here), "%s", slash ? slash + 1 : world);
		for (char* c = here; *c; ++c)
			if (*c >= 'A' && *c <= 'Z') *c = char(*c - 'A' + 'a');
		const bool sameWorld = !_doc.level[0] || strncmp(here, _doc.level, strlen(here)) == 0;
		if (sameWorld && width != 0 && height != 0) {
			// (OpenGothic's cursor: x and z over the level's coordinates; the original's maps count z down the picture.)
			const float wx = (_player.feet.x - float(left)) / float(width);
			const float wy = (_player.feet.z - float(top)) / float(height);
			if (wx >= 0.0f && wx <= 1.0f && wy >= 0.0f && wy <= 1.0f) {
				const int px = int(sx(wx * pw)), py = int(sy(wy * ph));
				const bool blink = fmod(_clock, 1.0) < 0.7;
				_ui.rect(px - 4, py - 4, px + 4, py + 4, 0x000000C0u);
				if (blink) _ui.rect(px - 3, py - 3, px + 3, py + 3, 0xE04838FFu);
			}
		}
		// The keys, small along the bottom.
		const char* hint = _doc.zoom > 1.0f ? tr("B — закрыть; L, R — масштаб; крестовина, стик — двигать") : tr("B — закрыть; A, R — крупнее");
		const int hw = _ui.measure(hint) + 12;
		_ui.rect(W / 2 - hw / 2, H - lh - 4, W / 2 + hw / 2, H, 0x000000A0u);
		_ui.text(W / 2, H - lh - 3, hint, 0xD8D6D0FFu, ui::TEXT, ui::CENTER);
		return;
	}
	if (_doc.map) {
		// The map: its picture is not in the archive, so the sheet shows where the hero stands within the part of the
		// world the map covers (OpenGothic's cursor: the hero's x and z over the level's coordinates).
		char title[64];
		snprintf(title, sizeof(title), "%s%s%s", tr("Карта"), _doc.level[0] ? ": " : "", _doc.level);
		_ui.text(W / 2, y0 + 6, title, ink, ui::TEXT, ui::CENTER, false);
		const int mx0 = x0 + 14, my0 = y0 + 8 + lh, mx1 = x1 - 14, my1 = y1 - 12;
		_ui.frame(mx0, my0, mx1, my1, 0x5A4630FFu);
		const i32 left = _doc.coords[0], top = _doc.coords[1];
		const i32 width = _doc.coords[2] - left, height = _doc.coords[3] - top;
		const char* world = worldName();
		const char* slash = strrchr(world, '/');
		char here[48];
		snprintf(here, sizeof(here), "%s", slash ? slash + 1 : world);
		for (char* c = here; *c; ++c)
			if (*c >= 'A' && *c <= 'Z') *c = char(*c - 'A' + 'a');
		// (The world's asset name has no extension; the level's is "world.zen".)
		const bool sameWorld = !_doc.level[0] || strncmp(here, _doc.level, strlen(here)) == 0;
		if (sameWorld && width > 0 && height > 0) {
			const float wx = (_player.feet.x - float(left)) / float(width);
			const float wy = (_player.feet.z - float(top)) / float(height);
			const int px = mx0 + int(clamp(wx, 0.0f, 1.0f) * float(mx1 - mx0));
			const int py = my0 + int(clamp(wy, 0.0f, 1.0f) * float(my1 - my0));
			_ui.rect(px - 3, py - 3, px + 3, py + 3, 0xC8463CFFu);
			_ui.frame(px - 4, py - 4, px + 4, py + 4, 0x000000C0u);
		}
	} else {
		const DocReader& r = _doc;
		const u32 page = min(r.page, r.pages - 1);
		for (int i = 0; i < int(r.lineCount[page]); ++i) _ui.text(x0 + kTextMargin, y0 + kTextMargin + i * lh, r.lines[page][i], ink, ui::TEXT, ui::LEFT, false);
	}
	if (_doc.pages > 1) {
		char num[24];
		snprintf(num, sizeof(num), "%u / %u", unsigned(_doc.page + 1), unsigned(_doc.pages));
		_ui.text(x1 - 8, y1 - 4 - lh, num, 0x5A4630FFu, ui::TEXT, ui::RIGHT, false);
	}
}

} // namespace g3
