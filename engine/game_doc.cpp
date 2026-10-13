// Books, letters and maps the scripts put up (Doc_Create, Doc_SetPages, Doc_PrintLine(s), Doc_Show ...).
// As OpenGothic's Gothic::doc_*: a handle names a document being built; Doc_Show gives it to the reader
// and the handle is free again. The margins and fonts the scripts name are the original's screen's (640 x 480)
// and mean nothing on this one: the reader lays the text out itself (App::docLayout).
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
