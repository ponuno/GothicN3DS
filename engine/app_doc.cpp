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
	r = DocReader();
	r.map = d.map;
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
	if (_panel.page() == InventoryPanel::PAGE_DOC) _panel.openPage(InventoryPanel::PAGE_PLAY);
	_invStamp = 0;
}

void App::docCommand(const PanelCommand& c) {
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
