// Books, letters and maps the scripts put up (Doc_Create, Doc_SetPages, Doc_PrintLine(s), Doc_Show ...).
// As OpenGothic's Gothic::doc_*: a handle names a document being built; Doc_Show gives it to the reader
// and the handle is free again. The margins and fonts the scripts name (Doc_SetMargins, Doc_SetFont) are
// kept per page since fix227: the reader scales them from the original's 640x480 onto its own screen.
#include "game.h"

#include <stdlib.h>
#include <string.h>

namespace g3 {

namespace {

void copyText(char* to, size_t size, const char* from) {
	size_t n = from ? strlen(from) : 0;
	if (n >= size) n = size - 1;
	memcpy(to, from, n);
	to[n] = 0;
}

// The name of a world as the original keeps it in a path ("..\\WORLD.ZEN" -> "world.zen"), lower case.
void levelName(char* to, size_t size, const char* from) {
	const char* base = from ? from : "";
	for (const char* p = base; *p; ++p)
		if (*p == '\\' || *p == '/') base = p + 1;
	copyText(to, size, base);
	for (char* p = to; *p; ++p)
		if (*p >= 'A' && *p <= 'Z') *p = char(*p - 'A' + 'a');
}

} // namespace

void Game::docFree(GameDocument& d) {
	for (GameDocPage& p : d.pages) free(p.text);
	d = GameDocument {};
}

int Game::docCreate(bool map) {
	for (u32 i = 0; i < GAME_DOCS; ++i) {
		if (_docs[i].used) continue;
		docFree(_docs[i]);
		_docs[i].used = true;
		_docs[i].map = map;
		_docs[i].pageCount = 1; // (Doc_SetPages says more; a script that prints without it has the one)
		for (u32 p = 0; p < GAME_DOC_PAGES; ++p) {
			_docs[i].pages[p].margins[0] = -1;
			_docs[i].pages[p].margins[1] = -1;
			_docs[i].pages[p].margins[2] = -1;
			_docs[i].pages[p].margins[3] = -1;
			_docs[i].pages[p].centered = false;
			_docs[i].pages[p].font[0] = 0;
		}
		return int(i);
	}
	return -1;
}

// Adds a line to a page. A page past the count is refused (the original ignores it too).
bool Game::docPrint(int handle, int page, const char* text) {
	GameDocument* d = docOf(handle);
	if (!d || page < 0 || u32(page) >= d->pageCount || !text) return false;
	GameDocPage& p = d->pages[page];
	const u32 add = u32(strlen(text)) + 1; // (and the line break)
	if (p.length + add + 1 > 4096) return false;
	if (p.length + add + 1 > p.capacity) {
		u32 cap = p.capacity ? p.capacity * 2 : 256;
		while (cap < p.length + add + 1) cap *= 2;
		char* grown = static_cast<char*>(realloc(p.text, cap));
		if (!grown) return false;
		p.text = grown;
		p.capacity = cap;
	}
	memcpy(p.text + p.length, text, add - 1);
	p.length += add - 1;
	p.text[p.length++] = '\n';
	p.text[p.length] = 0;
	return true;
}

bool Game::docShow(int handle) {
	GameDocument* d = docOf(handle);
	if (!d) return false;
	docFree(_docShown);
	_docShown = *d; // (the pages' text is the reader's now)
	*d = GameDocument {};
	++_docSerial;
	return true;
}

void Game::documentDone() { docFree(_docShown); }

void Game::xDocCreate(ScriptVm& vm, void* user) { vm.pushInt(static_cast<Game*>(user)->docCreate(false)); }

void Game::xDocCreateMap(ScriptVm& vm, void* user) { vm.pushInt(static_cast<Game*>(user)->docCreate(true)); }

// Doc_SetPage(handle, page, image, scale): the page's picture; a page that does not exist is the whole's.
void Game::xDocSetPage(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	(void)vm.popInt(); // (the scale: the reader draws the picture as it fits)
	const char* image = vm.popString();
	char copy[40];
	copyText(copy, sizeof(copy), image);
	const int page = vm.popInt();
	GameDocument* d = g.docOf(vm.popInt());
	if (!d) return;
	if (page >= 0 && u32(page) < d->pageCount) copyText(d->pages[page].image, sizeof(d->pages[page].image), copy);
	else copyText(d->image, sizeof(d->image), copy);
}

void Game::xDocSetPages(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const int count = vm.popInt();
	GameDocument* d = g.docOf(vm.popInt());
	if (d && count >= 0 && u32(count) <= GAME_DOC_PAGES) d->pageCount = u32(count);
}

// fix227. Doc_SetMargins(doc, page, left, top, right, bottom, center): the text area of a page, in the
// pixels of the original screen (640x480). The reader scales them onto its own screen; where not set, it
// lays the text out as before. A page that does not exist — as Doc_SetPage: for the whole document.
void Game::xDocSetMargins(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const i32 center = vm.popInt();
	const i32 bottom = vm.popInt();
	const i32 right = vm.popInt();
	const i32 top = vm.popInt();
	const i32 left = vm.popInt();
	const int page = vm.popInt();
	GameDocument* d = g.docOf(vm.popInt());
	if (!d) return;
	const bool cen = center != 0;
	// The page keeps the margins as left, right, top, bottom; the center flag — beside them.
	const i16 m[4] = {i16(clamp<i32>(left, -1, 32000)), i16(clamp<i32>(right, -1, 32000)),
	                  i16(clamp<i32>(top, -1, 32000)), i16(clamp<i32>(bottom, -1, 32000))};
	if (page >= 0 && u32(page) < d->pageCount) {
		GameDocPage& p = d->pages[page];
		for (int k = 0; k < 4; ++k) p.margins[k] = m[k];
		p.centered = cen;
	} else if (page < 0) {
		for (u32 p = 0; p < GAME_DOC_PAGES; ++p) {
			for (int k = 0; k < 4; ++k) d->pages[p].margins[k] = m[k];
			d->pages[p].centered = cen;
		}
	}
}

// Doc_SetFont(doc, page, font) — the name of a font of the original ("FONT_OLD_20_WHITE.TGA", ...).
// We have two sizes (and keep the color of the ink), so only the height in the name is used: 20 and above
// is the large, otherwise the small one. The reader parses the name again when laying out.
void Game::xDocSetFonts(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	char name[64];
	copyText(name, sizeof(name), vm.popString());
	const int page = vm.popInt();
	GameDocument* d = g.docOf(vm.popInt());
	if (!d) return;
	if (page >= 0 && u32(page) < d->pageCount) {
		copyText(d->pages[page].font, sizeof(d->pages[page].font), name);
	} else if (page < 0) {
		for (u32 p = 0; p < GAME_DOC_PAGES; ++p) copyText(d->pages[p].font, sizeof(d->pages[p].font), name);
	}
}

void Game::xDocPrintLine(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	char text[512];
	copyText(text, sizeof(text), vm.popString());
	const int page = vm.popInt();
	g.docPrint(vm.popInt(), page, text);
}

void Game::xDocSetLevel(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	const char* level = vm.popString();
	char name[40];
	levelName(name, sizeof(name), level);
	if (GameDocument* d = g.docOf(vm.popInt())) memcpy(d->level, name, sizeof(name));
}

void Game::xDocSetLevelCoords(ScriptVm& vm, void* user) {
	Game& g = *static_cast<Game*>(user);
	i32 c[4];
	for (int k = 3; k >= 0; --k) c[k] = vm.popInt();
	if (GameDocument* d = g.docOf(vm.popInt())) memcpy(d->coords, c, sizeof(c));
}

void Game::xDocShow(ScriptVm& vm, void* user) { static_cast<Game*>(user)->docShow(vm.popInt()); }

} // namespace g3
