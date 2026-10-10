// Text and rectangles over the picture (menus, the HUD), drawn through the
// backend's 2D overlay (gfx::uiDraw) with the font of engine/ui_font.h.
//
// Quads are collected during the frame and drawn by flush(), in the order
// they were added (a panel first, then the text on it).
#pragma once
#include "gfx.h"

namespace g3 {

// Colours are 0xRRGGBBAA.
namespace ui {
constexpr u32 WHITE = 0xFFFFFFFFu, GREY = 0xB4B4B4FFu, DARK = 0x707070FFu, GOLD = 0xE8C77AFFu, RED = 0xC8463CFFu,
              GREEN = 0x78C850FFu, BLACK = 0x000000FFu, PANEL = 0x141210C8u, SHADE = 0x00000080u;
enum Size { TEXT = 0, TITLE = 1 };
enum Align { LEFT, CENTER, RIGHT };
} // namespace ui

class Ui {
public:
	Ui() = default;
	~Ui() { release(); }
	Ui(const Ui&) = delete;
	Ui& operator=(const Ui&) = delete;

	// Makes the font texture. False if the backend could not (then nothing is drawn).
	bool init();
	void release();
	bool ready() const { return _font != nullptr; }

	// Text (UTF-8; Latin, Cyrillic and a few signs - others show as '?')
	// with its top at `y`. Returns the width it took. A one-pixel shadow
	// under it keeps it readable over a bright sky.
	int text(int x, int y, const char* utf8, u32 rgba = ui::WHITE, int size = ui::TEXT, int align = ui::LEFT, bool shadow = true);
	int measure(const char* utf8, int size = ui::TEXT) const;
	static int lineHeight(int size = ui::TEXT);
	void rect(int x0, int y0, int x1, int y1, u32 rgba);
	// An outline one pixel wide.
	void frame(int x0, int y0, int x1, int y1, u32 rgba);
	// A bar filled to `share` (0..1) of its width.
	void bar(int x0, int y0, int x1, int y1, float share, u32 fill, u32 back = 0x000000A0u);

	// Draws what was added and starts over. Call between the 3D passes and
	// gfx::frameEnd.
	void flush();
	u32 pending() const { return _count; }
	u32 dropped() const { return _dropped; }

private:
	void quad(int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1, u32 rgba);

	gfx::Texture* _font = nullptr;
	gfx::UiQuad _quads[gfx::UI_MAX_QUADS];
	u32 _count = 0, _dropped = 0;
};

// The next character of a UTF-8 string (advancing `s`); 0 at its end.
u32 utf8Next(const char*& s);

} // namespace g3
