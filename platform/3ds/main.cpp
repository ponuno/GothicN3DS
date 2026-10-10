// Entry point for the 3DS build. Top screen: the world, the HUD and the
// pause menu. Bottom screen: the inventory (engine/panel.h: a picture the
// processor draws and this file copies into the screen's framebuffer when it
// changes; the touch screen works it) - or, with "Нижний экран: Отчёт" in the
// menu or its last page tapped, a text console with the numbers that matter
// for the feasibility check (frame time split into CPU and GPU, triangles,
// streaming, memory). A tap on the report brings the inventory back.
//
// Controls. By default the classic scheme of the PC original, the camera
// always behind the character: Circle Pad up: run, down: hop back,
// left/right: turn; C-Stick turns faster and tilts the view; L / R step aside;
// locked on to an enemy with a weapon in hand the character keeps facing it,
// Circle Pad left/right step round it, a C-Stick flick picks the next target;
// the buttons as below. The menu's "Управление" switches to the console
// scheme (after Gothic Classic on the consoles):
//   Circle Pad    go where it points, as the camera sees it (a slight push: walk)
//   C-Stick       turn the camera about the character; locked on: flick left or right
//                 for the next target to that side
//   B             jump            A  attack (with the Circle Pad left/right: a blow to the side);
//                                    hold for a combo: press again as a blow ends
//   Y             parry (held)    X  draw the last weapon / put it away
//   ZL            melee weapon (fists without one)     ZR  fists
//   D-Pad Up      lock on to the enemy in front / let go (drawing a weapon locks
//                 on to a hostile one in front by itself: the menu's "Автозахват цели")
//   D-Pad Down    sneak on / off
//   L             quick magic (not yet)
//   START         pause menu: continue, graphics, controls, faults, save, leave
//   SELECT        opens the inventory on the bottom screen (D-Pad choose, L / R
//                 page, A use, X: into a quick slot, B or SELECT close); the
//                 character stands while it is open
//   touch         the bottom screen: the ten quick slots (tap: use), the
//                 inventory, the diary, the character
//   R + START     save the position       R + SELECT  load it
//   L+R+SELECT    write the last 5 seconds of the character's movement to
//                 sdmc:/3ds/gothic3ds/slope.txt (added at its end): press it where the
//                 character is stuck. The "pos" line counts the files written ("sd").
//   START+SELECT  held for a second: leave at once (for testing)
// Flying, the next world and the time of day are in the menu's "Отладка".
// The position is saved on leaving and restored on the next start; hold L while
// starting to begin at the world's start point instead. The menu's settings are
// kept in sdmc:/3ds/gothic3ds/settings.bin.
//
// Switches for comparing on the console, held while the program starts:
//   B  load chunks on the frame's own thread, as before (no background loading)
//   X  no frame pacer (fixed object distances, never 30 fps)
//   Y  citro3d's own frame order: the GPU waits while the CPU issues the draws
//   A  first person from the start (sets the menu's "Вид от первого лица")
//   R  the vertex shader of the versions before characters; no character at all
//   SELECT   the inhabitants are not drawn (the scripts run all the same)
//   D-Pad Up the inhabitants only stand where their routine has them: no
//            states, no walking - as in the version before they lived
//   ZL       whole chunks are drawn, as before they were cut into clusters
//            (what the clusters save shows in "tris" and "gpu")
//   D-Pad Down  steady objects (sets the menu's "Объекты: Устойчиво")
//   D-Pad Left  nobody perceives anything (sets the menu's "Восприятие: Выкл")
//   D-Pad Right objects appear at once, without dissolving (the menu's
//            "Растворение" otherwise decides, from the next start)
#include "app.h"
#include "gfx_c3d.h"
#include "pacer.h"
#include "sys.h"

#include <3ds.h>
#include <citro3d.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

using g3::App;
using g3::clamp;
using g3::Input;
using g3::max;
using g3::WorldStats;
namespace gfx = g3::gfx;

static const char* kPakPath = "sdmc:/3ds/gothic3ds/data.pak";
// Where the player was when the program was left; the next start continues there.
static const char* kStatePath = "sdmc:/3ds/gothic3ds/state.bin";
// Where L+R+SELECT writes the movement recorder.
static const char* kSlopePath = "sdmc:/3ds/gothic3ds/slope.txt";
// The menu's settings.
static const char* kSettingsPath = "sdmc:/3ds/gothic3ds/settings.bin";

// The HOME Menu was opened: the game stands still in its pause menu meanwhile.
static volatile bool gSuspended = false;
static aptHookCookie gAptCookie;
static void onApt(APT_HookType hook, void*) {
	if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP) gSuspended = true;
}

// The application: 13 KB, with the character's poses in it. Not a local of
// main(): the stack is 32 KB.
static App gApp;

static float stick(int v) {
	const float dead = 18.0f, full = 140.0f;
	float f = float(v);
	if (f > -dead && f < dead) return 0.0f;
	f = (f > 0 ? f - dead : f + dead) / (full - dead);
	return clamp(f, -1.0f, 1.0f);
}

// The bottom-screen report: laid out once every 30 frames, written a line
// per frame (printing all of it at once takes several milliseconds).
static const int kReportLines = 28; // (row 29 is for the steps of leaving)
static char gReport[kReportLines][48];
static int gReportCount = 0;

static void reportLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void reportLine(const char* fmt, ...) {
	if (gReportCount >= kReportLines) return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(gReport[gReportCount++], sizeof(gReport[0]), fmt, ap);
	va_end(ap);
}

// A line at the bottom of the bottom screen (below the report).
static void exitStep(const char* text) { printf("\x1b[29;1H%-39.39s", text); }

// The inventory's picture into the bottom screen. Its framebuffer is the
// console's (RGB565, one buffer, shown as it is written): turned a quarter -
// the 3DS keeps its screens in columns of 240 from the bottom - and flushed
// from the cache so that the display reads what was written.
static void showPanel(const g3::Canvas& canvas) {
	u16 width = 0, height = 0;
	u16* fb = reinterpret_cast<u16*>(gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &width, &height));
	const u16* src = canvas.pixels();
	if (!fb || !src || width != g3::Canvas::H || height != g3::Canvas::W) return;
	for (int x = 0; x < g3::Canvas::W; ++x) {
		u16* column = fb + x * g3::Canvas::H;
		const u16* from = src + x;
		for (int y = g3::Canvas::H - 1; y >= 0; --y, from += g3::Canvas::W) column[y] = *from;
	}
	GSPGPU_FlushDataCache(fb, u32(g3::Canvas::W) * g3::Canvas::H * sizeof(u16));
}

static void waitForStart() {
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START) break;
		gspWaitForVBlank();
	}
}

int main(int, char**) {
	// New 3DS: 804 MHz + L2 cache. Without this the app runs at Old 3DS speed.
	osSetSpeedupEnable(true);

	gfxInitDefault();
	gfxSet3D(false);
	consoleInit(GFX_BOTTOM, nullptr);

	bool isNew = false;
	APT_CheckNew3DS(&isNew);
	printf("Gothic 3DS runtime (stage 0)\n%s\n\n", isNew ? "New 3DS" : "Old 3DS: expect a slideshow");

	// Buttons held while starting select what to compare (see the top of this
	// file). A few refreshes, so that the first reading is not an empty one.
	u32 startKeys = 0;
	for (int i = 0; i < 4; ++i) {
		gspWaitForVBlank();
		hidScanInput();
		startKeys |= hidKeysHeld();
	}
	const bool background = !(startKeys & KEY_B), pacing = !(startKeys & KEY_X), classic = (startKeys & KEY_Y) != 0;
	const bool plainShader = (startKeys & KEY_R) != 0;
	const bool nobody = (startKeys & KEY_SELECT) != 0; // the scripts run, their people are not drawn
	const bool standing = (startKeys & KEY_DUP) != 0;  // they stand: no states, no walking
	const bool wholeChunks = (startKeys & KEY_ZL) != 0; // no culling inside chunks
	const bool steadyKey = (startKeys & KEY_DDOWN) != 0; // objects drawn farther, never taken away by the pacer
	const bool blind = (startKeys & KEY_DLEFT) != 0;     // no perception
	g3::Settings settings;
	const bool hadSettings = settings.load(kSettingsPath);
	// Objects dissolve in and out (a shader of its own): as the menu says,
	// unless D-Pad Right is held.
	const bool dissolve = settings.dissolve && !(startKeys & KEY_DRIGHT);
	gfx::c3dSetClassicFrames(classic);
	gfx::c3dSetPlainShader(plainShader);
	gfx::c3dSetFadeShader(dissolve);

	App& app = gApp;
	// Chunks are read, unpacked and given their collision grids on a worker
	// thread; the frame only copies the finished mesh into video memory.
	g3::StreamConfig& stream = app.world().config();
	stream.background = background;
	stream.maxLoadsPerFrame = background ? 1 : 2;
	stream.clusterCulling = !wholeChunks;
	if (!dissolve) stream.fadeMs = 0.0f;
	bool ok = gfx::init();
	if (!ok) printf("gfx init failed\n");
	if (ok && !app.init(kPakPath, nullptr)) {
		printf("%s\nexpected at %s\n", app.error(), kPakPath);
		ok = false;
	}
	if (!ok) {
		printf("\nPress START to exit.\n");
		waitForStart();
		gfx::shutdown();
		osSetSpeedupEnable(false);
		gfxExit();
		return 1;
	}

	// The linear heap, where meshes and textures live, is 32 MB; the command
	// buffer and the frame buffers are in it too.
	app.world().config().memoryBudget = 20u << 20;
	// Continue where the last session ended, unless L is held at start.
	const bool hadState = !(startKeys & KEY_L) && app.loadState(kStatePath);
	// The settings: the draw distance of an older state.bin counts once, for
	// a settings.bin that is not there yet.
	if (!hadSettings && hadState) settings.drawMetres = g3::u8(g3::clamp(int(app.drawDistance() / 1000.0f + 0.5f) * 10, 40, 200));
	if (steadyKey) settings.objects = g3::OBJECTS_STEADY;
	if ((startKeys & KEY_A) || plainShader) settings.firstPerson = 1;
	if (blind) settings.perception = 0;
	if (standing) settings.life = 0;
	app.settings() = settings;
	app.applySettings();
	app.setStatePath(kStatePath);
	if (nobody || plainShader) app.npcs().config().maxShown = 0;
	printf("loading...\n");
	app.world().preload(app.camera().pos);
	consoleClear();
	aptHook(&gAptCookie, onApt, nullptr);
	hidSetRepeatParameters(24, 6);

	u32 frame = 0;
	float msAccum = 0.0f, msWorst = 0.0f, updateAccum = 0.0f, updateWorst = 0.0f;
	u32 lateFrames = 0; // frames of this report that missed their refresh
	u32 lastPerceived = 0;
	g3::FramePacer pacer;
	const auto pace = [&]() {
		const g3::Settings& st = app.settings();
		pacer.cfg.minDetail = st.minDetail(); // (steady: too heavy means 30 fps, the objects stay)
		pacer.cfg.halfOnly = st.frameRate == g3::FRAMES_30;
	};
	pace();
	int interval = 1;
	int reportLines = 0;
	u32 lastStateCalls = 0;
	g3::UpdateTimes partSum;               // where update() spent its time, summed over a report
	u32 collQueries = 0, collTests = 0;    // and what its collision queries looked at
	u32 slopeWrites = 0;                   // L+R+SELECT: records written (a failed write counts as none)
	float lastYaw = app.camera().yaw;      // how fast the view turns: the pacer gives no objects back meanwhile
	u32 fadingSum = 0;                     // batches drawn dissolving, summed over a report
	bool slopeFailed = false;
	float leaveHeld = 0;                   // START+SELECT held this long: leave
	bool flicked = false;                  // the C-Stick was flicked aside (locked on: the next target)
	bool reportShown = true;
	bool panelShown = false;               // the inventory's picture is on the bottom screen

	while (aptMainLoop()) {
		hidScanInput();
		const u32 down = hidKeysDown(), held = hidKeysHeld(), repeat = hidKeysDownRepeat();
		g3::Menu& menu = app.menu();
		if (gSuspended) {
			gSuspended = false;
			if (!menu.isOpen()) menu.open();
		}
		// START+SELECT held for a second: out at once (for testing).
		leaveHeld = (held & KEY_START) && (held & KEY_SELECT) ? leaveHeld + 1.0f / 60.0f : 0.0f;
		if (leaveHeld > 1.0f) break;

		// The step of the simulation is the time between the last two frames
		// reaching the display: as even as the display itself, whatever the
		// work in between took.
		const gfx::FrameStats timing = gfx::frameStats();
		const float ms = timing.frameMs > 0.0f ? timing.frameMs : 16.7f * float(interval);
		const float dt = clamp(ms * 0.001f, 0.001f, 0.1f);

		circlePosition pad, cstick;
		hidCircleRead(&pad);
		hidCstickRead(&cstick);
		touchPosition touch;
		hidTouchRead(&touch);

		// ---- the bottom screen: the inventory or the report ----
		const bool inventoryScreen = app.settings().report == 0;
		g3::InventoryPanel& panel = app.panel();
		// The buttons the inventory takes this frame (after SELECT); the game gets the rest.
		u32 taken = 0;
		if (inventoryScreen && !menu.isOpen()) {
			g3::PanelInput pin;
			pin.touching = (held & KEY_TOUCH) != 0;
			pin.x = touch.px;
			pin.y = touch.py;
			if (panel.cursor()) {
				taken = KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT | KEY_A | KEY_B | KEY_X | KEY_Y | KEY_L | KEY_R;
				pin.up = (repeat & KEY_DUP) != 0;
				pin.down = (repeat & KEY_DDOWN) != 0;
				pin.left = (repeat & KEY_DLEFT) != 0;
				pin.right = (repeat & KEY_DRIGHT) != 0;
				pin.confirm = (down & KEY_A) != 0;
				pin.back = (down & (KEY_B | KEY_SELECT)) != 0 && !(held & (KEY_R | KEY_START));
				pin.assign = (down & KEY_X) != 0;
				pin.prevTab = (down & KEY_L) != 0;
				pin.nextTab = (down & KEY_R) != 0;
			} else if ((down & KEY_SELECT) && !(held & (KEY_R | KEY_L | KEY_START)) && app.steersHero()) {
				panel.setCursor(true);
				taken = KEY_SELECT;
			}
			const g3::PanelCommand pc = app.panelInput(pin, dt);
			if (pc.type == g3::PANEL_REPORT) {
				app.settings().report = 1;
				app.settings().save(kSettingsPath);
			}
			if (pc.type == g3::PANEL_CURSOR_OFF) taken |= KEY_SELECT | KEY_B;
		} else if (!inventoryScreen && (down & KEY_TOUCH) && !menu.isOpen()) {
			// A tap on the report: the inventory again.
			app.settings().report = 0;
			app.settings().save(kSettingsPath);
		}
		if (!inventoryScreen || menu.isOpen()) panel.setCursor(false);
		const u32 gameDown = down & ~taken, gameHeld = held & ~taken;

		// ---- the pause menu: the game stands still ----
		bool record = false;
		float updateMs = 0;
		if (menu.isOpen()) {
			g3::MenuInput mi;
			mi.up = (repeat & (KEY_DUP | KEY_CPAD_UP)) != 0;
			mi.down = (repeat & (KEY_DDOWN | KEY_CPAD_DOWN)) != 0;
			mi.left = (repeat & (KEY_DLEFT | KEY_CPAD_LEFT)) != 0;
			mi.right = (repeat & (KEY_DRIGHT | KEY_CPAD_RIGHT)) != 0;
			mi.confirm = (down & KEY_A) != 0;
			mi.back = (down & KEY_B) != 0;
			mi.close = (down & KEY_START) != 0;
			const g3::MenuAction act = menu.update(mi, app.settings());
			if (act == g3::MENU_CHANGED) {
				app.applySettings();
				pace();
				app.settings().save(kSettingsPath);
			} else if (act == g3::MENU_EXIT) {
				break;
			} else if (act != g3::MENU_NONE && act != g3::MENU_CLOSED) {
				app.menuAction(act, kStatePath);
			}
			app.render();
		} else {
			Input in;
			in.moveX = stick(pad.dx);
			in.moveZ = stick(pad.dy);
			in.lookX = stick(cstick.dx);
			in.lookY = stick(cstick.dy);
			const bool console = app.scheme() == g3::CONTROLS_CONSOLE;
			// R and L+R combinations first: they are not their single buttons.
			const bool rHeld = (held & KEY_R) != 0 && !(taken & KEY_R);
			record = (down & KEY_SELECT) && (held & KEY_L) && rHeld;
			const bool quickSave = (down & KEY_START) && rHeld, quickLoad = (down & KEY_SELECT) && rHeld && !record;
			const u32 dpad = ~0u;
			if (app.steersHero()) {
				in.jump = (gameHeld & KEY_B) != 0;
				in.attack = in.confirm = (gameDown & KEY_A) != 0;
				in.parry = (gameHeld & KEY_Y) != 0;
				in.drawWeapon = (gameDown & KEY_X) != 0;
				in.meleeSlot = (gameDown & KEY_ZL) != 0;
				in.fistSlot = (gameDown & KEY_ZR) != 0;
				in.focus = (gameDown & dpad & KEY_DUP) != 0;
				in.toggleSneak = (gameDown & dpad & KEY_DDOWN) != 0;
				// Locked on: the C-Stick flicked aside picks the next target
				// (either scheme: the character faces the target by itself then).
				if (app.targetLocked()) {
					if (!flicked && (in.lookX > 0.7f || in.lookX < -0.7f)) {
						in.switchTarget = in.lookX > 0.0f ? 1.0f : -1.0f;
						flicked = true;
					}
					if (in.lookX < 0.3f && in.lookX > -0.3f) flicked = false;
					in.lookX = 0;
				}
				if (console) {
					if ((gameDown & KEY_L) && !rHeld) app.toast("Быстрой магии пока нет");
				} else {
					in.strafe = (gameHeld & KEY_R ? 1.0f : 0.0f) - (gameHeld & KEY_L ? 1.0f : 0.0f);
				}
				if ((gameDown & KEY_SELECT) && !rHeld && !(held & KEY_START) && !inventoryScreen)
					app.toast("Инвентарь — на нижнем экране (меню: Графика)");
			} else {
				// Flying (the menu's "Отладка"): a free camera.
				if (held & KEY_A) in.lookX = 1.0f;
				if (held & KEY_Y) in.lookX = -1.0f;
				if (held & KEY_X) in.lookY = 1.0f;
				if (held & KEY_B) in.lookY = -1.0f;
				in.moveY = (held & KEY_R ? 1.0f : 0.0f) - (held & KEY_L ? 1.0f : 0.0f);
				in.jump = (held & KEY_R) != 0;
				in.fast = (held & (KEY_ZL | KEY_ZR)) != 0;
			}
			if (quickSave) {
				app.toast(app.saveState(kStatePath) ? "Позиция сохранена" : "Не удалось сохранить");
			} else if (quickLoad) {
				app.toast(app.loadState(kStatePath) ? "Позиция загружена" : "Сохранённой позиции нет");
			} else if ((down & KEY_START) && !(held & KEY_SELECT)) {
				menu.open();
				panel.setCursor(false);
			}
			TickCounter updateClock;
			osTickCounterStart(&updateClock);
			app.update(in, dt);
			osTickCounterUpdate(&updateClock);
			updateMs = float(osTickCounterRead(&updateClock));
			updateAccum += updateMs;
			updateWorst = max(updateWorst, updateMs);
			{
				const g3::UpdateTimes& ut = app.times();
				partSum.stream += ut.stream;
				partSum.npcs += ut.npcs;
				partSum.script += ut.script;
				partSum.player += ut.player;
				partSum.camera += ut.camera;
				collQueries += g3::collisionStats().queries;
				collTests += g3::collisionStats().tests;
			}

			if (record) {
				// After the update, so that the frame the keys were pressed in is in it.
				const g3::Player& pl = app.player();
				const g3::Vec3 eye = pl.eye();
				char title[160];
				snprintf(title, sizeof(title), "%s at %02d:%02d, pos %.0f %.0f %.0f, popped out of the rock %lu, unstuck %lu, record %lu",
				         app.worldName(), int(app.environment().timeOfDay), int(app.environment().timeOfDay * 60.0f) % 60, double(eye.x),
				         double(eye.y), double(eye.z), static_cast<unsigned long>(pl.popUps), static_cast<unsigned long>(pl.unstuck),
				         static_cast<unsigned long>(slopeWrites + 1));
				FILE* f = fopen(kSlopePath, "a");
				const bool written = f && app.moves().write(f, title);
				if (f) fclose(f);
				if (written) ++slopeWrites;
				slopeFailed = !written;
			}

			app.render();
		} // (not in the menu)

		// What the frame cost decides how much detail the next one gets, and
		// whether the display runs at 60 or a steady 30. With overlapped
		// frames the CPU and the GPU work at the same time and the slower of
		// the two sets the pace; in citro3d's own order the GPU waits for the
		// CPU, and a frame takes the sum. (Measured in that order on a New
		// 3DS: 28% + 97% gave 20.9 ms, which is 125% of 16.7 ms.)
		const gfx::FrameStats fs = gfx::frameStats();
		const float load = (fs.overlapped ? max(fs.cpuMs + updateMs, fs.gpuMs) : fs.cpuMs + fs.gpuMs) * 0.06f;
		// The view turning faster than about 60 degrees a second: what comes
		// into view comes and goes, so no objects are given back meanwhile.
		float turn = app.camera().yaw - lastYaw;
		while (turn > 3.14159265f) turn -= 6.2831853f;
		while (turn < -3.14159265f) turn += 6.2831853f;
		lastYaw = app.camera().yaw;
		const bool turning = turn > 1.0f * dt || turn < -1.0f * dt;
		fadingSum += app.world().stats().batchesFading;
		// (A world switch or any other pause is not a frame to learn from.)
		if (pacing && updateMs < 50.0f && !record) {
			pacer.update(load, turning);
			app.world().config().detailScale = pacer.detailScale();
			if (pacer.interval() != interval) {
				interval = pacer.interval();
				C3D_FrameRate(interval == 2 ? 30.0f : 60.0f);
			}
		}

		if (fs.frameMs > 0.0f) {
			msAccum += fs.frameMs;
			msWorst = max(msWorst, fs.frameMs);
			lateFrames += fs.frameMs > 16.7f * float(interval) * 1.25f;
		}
		if (++frame % 30 == 0) {
			const WorldStats& ws = app.world().stats();
			const gfx::MemStats mem = gfx::memStats();
			const g3::Player& pl = app.player();
			const char* state = pl.flying     ? "flying (menu: faults)"
			                    : pl.swimming ? "swimming"
			                    : pl.climbing ? "on a ladder"
			                    : !pl.grounded ? "in the air"
			                    : pl.waterDepth > 0 ? "wading"
			                                        : "on the ground";
			gReportCount = 0;
			reportLine("frame %5.1f ms avg %5.1f worst", double(msAccum / 30.0f), double(msWorst));
			reportLine("cpu %3.0f%%  gpu %3.0f%%  load %3.0f%%", double(fs.cpuMs * 6.0f), double(fs.gpuMs * 6.0f),
			     double(load * 100.0f));
			reportLine("update %4.2f ms avg %5.1f worst", double(updateAccum / 30.0f), double(updateWorst));
			// update() by its parts: chunks, inhabitants (of which scripts),
			// the player's character, the camera - and the collision queries
			// of a frame with the triangles they looked at.
			reportLine("upd w%3.1f n%3.1f s%3.1f p%3.1f c%3.1f", double(partSum.stream / 30.0f), double(partSum.npcs / 30.0f),
			     double(partSum.script / 30.0f), double(partSum.player / 30.0f), double(partSum.camera / 30.0f));
			reportLine("collide %3lu queries %5lu tris", static_cast<unsigned long>(collQueries / 30),
			     static_cast<unsigned long>(collTests / 30));
			reportLine("loader %-6s job %5.1f ms  fly %lu",
			     !app.world().backgroundStreaming() ? "off" : app.world().workerThread() ? "thread" : "inline",
			     double(app.world().takeWorstLoadMs()), static_cast<unsigned long>(ws.chunksInFlight));
			// (fd: objects dissolving in or out, on average; "off": the dissolve is off.)
			if (dissolve && gfx::fadeSupported())
				reportLine("detail %3.0f%%  %2d fps  late %2lu/30 fd %lu", double(app.world().config().detailScale * 100.0f),
				     interval == 2 ? 30 : 60, static_cast<unsigned long>(lateFrames), static_cast<unsigned long>(fadingSum / 30));
			else
				reportLine("detail %3.0f%%  %2d fps  late %2lu/30 fd off", double(app.world().config().detailScale * 100.0f),
				     interval == 2 ? 30 : 60, static_cast<unsigned long>(lateFrames));
			reportLine("frames %s  pacer %s%s", fs.overlapped ? "overlapped" : "classic", pacing ? "on" : "off",
			     app.settings().objects == g3::OBJECTS_STEADY ? " STEADY" : app.settings().objects == g3::OBJECTS_BALANCED ? " BALANCE" : "");
			// Triangles drawn, of those in the chunks in view (the rest is what
			// the clusters save); draw calls; chunks.
			reportLine("tris %6lu/%6lu dr %4lu ch %2lu%s", static_cast<unsigned long>(ws.trianglesDrawn),
			     static_cast<unsigned long>(ws.trianglesInChunks), static_cast<unsigned long>(ws.drawCalls),
			     static_cast<unsigned long>(ws.chunksDrawn), wholeChunks ? " ZL" : "");
			reportLine("blend %3lu cull %4lu merged %4lu", static_cast<unsigned long>(ws.blendedDrawCalls),
			     static_cast<unsigned long>(ws.batchesCulled), static_cast<unsigned long>(ws.batchesMerged));
			reportLine("cmdbuf %3.0f%%  dropped draws %lu", double(fs.commandBufferUse * 100.0f),
			     static_cast<unsigned long>(fs.drawsDropped));
			reportLine("resident %3lu/%lu  pending %3lu", static_cast<unsigned long>(ws.chunksLoaded),
			     static_cast<unsigned long>(ws.chunksTotal), static_cast<unsigned long>(ws.chunksPending));
			// (Meshes, textures and collision of the world; free linear memory; chunks refused for the budget, failed.)
			reportLine("mesh %5.2f tex %5.2f MB col %5.0fK", double(mem.meshBytes) / 1048576.0, double(mem.textureBytes) / 1048576.0,
			     double(ws.collisionBytes) / 1024.0);
			reportLine("linear free %5.1f MB rej %lu fail %lu", double(linearSpaceFree()) / 1048576.0,
			     static_cast<unsigned long>(ws.budgetRejects), static_cast<unsigned long>(ws.loadFailures));
			reportLine("draw distance %3.0f m  time %02d:%02d", double(app.drawDistance() / 100.0f), int(app.environment().timeOfDay),
			     int(app.environment().timeOfDay * 60.0f) % 60);
			reportLine("%-22.22s %-16.16s", state, app.worldName());
			// The player's eyes, wherever the camera hangs behind them.
			const g3::Vec3 eye = pl.eye();
			// Then: times the character was popped out of the rock and was
			// unstuck (see Player::unstick), and the records written to slope.txt.
			reportLine("pos %7.0f %6.0f %7.0f u%lu/%lu sd%lu%s", double(eye.x), double(eye.y), double(eye.z),
			     static_cast<unsigned long>(pl.popUps), static_cast<unsigned long>(pl.unstuck), static_cast<unsigned long>(slopeWrites),
			     slopeFailed ? "!" : "");
			const g3::Hero& hero = app.hero();
			if (plainShader) {
				reportLine("hero off: plain shader (R at start)");
			} else if (hero.loaded()) {
				static const char* const kRoles[g3::Hero::ROLE_COUNT] = {"stand", "run",   "walk",  "back", "step",
				                                                         "turn",  "jump",  "fall",  "swim", "float",
				                                                         "attack", "parry", "hit", "down", "draw", "use", "climb"};
				reportLine("hero %-5s %3.0f cm/s %s%s cam %s", kRoles[hero.role()], double(hero.speed()),
				     hero.walking() ? "walk " : "", app.thirdPerson() ? "3rd" : "1st", app.cameraSet().fromGame ? "game" : "own");
			} else {
				reportLine("hero: none in data.pak");
			}
			// The scripts and the people they put into the world.
			const g3::Npcs& npcs = app.npcs();
			if (npcs.loaded()) {
				const g3::NpcStats& ns = npcs.stats();
				const g3::ScriptStats& vs = npcs.game().vm().stats();
				reportLine("npcs %2lu/%2lu in %2.0f m of %4lu%s", static_cast<unsigned long>(ns.shown),
				     static_cast<unsigned long>(ns.near), double(ns.reach / 100.0f), static_cast<unsigned long>(ns.here),
				     nobody ? " OFF" : "");
				// (it: things lying about - taken / all of this world, drawn, looks without a model.)
				const g3::LootStats& ls = app.loot().stats();
				reportLine("ntri %5lu m%2lu w%lu n%lu it%lu/%lu d%lu x%lu", static_cast<unsigned long>(ns.triangles),
				     static_cast<unsigned long>(ns.models), static_cast<unsigned long>(ns.waiting),
				     static_cast<unsigned long>(ns.noModel), static_cast<unsigned long>(ls.taken), static_cast<unsigned long>(ls.count),
				     static_cast<unsigned long>(ls.drawn), static_cast<unsigned long>(ls.noModel));
				// Their life: how many live and walk, animations in memory, state
				// functions called a second, ways not found + characters put at their goal.
				// ("use": beds, benches and the like in use; in "x" also the
				// times somebody had no animation for one.)
				reportLine("life %2lu wlk %lu use %lu ani %3luK st %2lu/s x%lu%s", static_cast<unsigned long>(ns.actors),
				     static_cast<unsigned long>(ns.walking), static_cast<unsigned long>(ns.mobsInUse),
				     static_cast<unsigned long>(ns.clipBytes / 1024), static_cast<unsigned long>((ns.stateCalls - lastStateCalls) * 2),
				     static_cast<unsigned long>(ns.noPath + ns.snapped + ns.clipsRefused + ns.mobsFailed), app.npcs().config().alive ? "" : " OFF");
				lastStateCalls = ns.stateCalls;
				reportLine("script %3.1fMB all %4lu err %lu/%lu nul %lu", double(npcs.game().vm().imageBytes()) / 1048576.0,
				     static_cast<unsigned long>(ns.total), static_cast<unsigned long>(vs.errors),
				     static_cast<unsigned long>(vs.aborted), static_cast<unsigned long>(vs.nullAccess));
				// The nearest of them: its state and seconds in it; the thing it
				// uses (stage: C coming, T turning, E getting in, I in it, L
				// leaving), the state of the thing it is in, what the scripts are
				// told its body does, how far it faces off the place's way; the
				// animation it is in.
				if (const g3::NpcActor* a = npcs.nearest()) {
					const g3::GameNpc& n = npcs.game().npc(a->npc);
					const char* zs = npcs.stateName(a->state);
					reportLine("%-13.13s %-18.18s%4.0fs", npcs.game().nameOf(n), zs[0] ? zs + (strncmp(zs, "ZS_", 3) == 0 ? 3 : 0) : "-",
					     double(a->stateTime));
					static const char kStages[] = "-CTEIL";
					float off = 0;
					if (a->mob >= 0) {
						off = (n.yaw - a->mobYaw) * 57.29578f;
						while (off > 180.0f) off -= 360.0f;
						while (off < -180.0f) off += 360.0f;
					}
					reportLine("mob %-9.9s %c s%-2d bs %-6ld off %4.0f", a->mob >= 0 ? npcs.mobs().scheme(u32(a->mob)) : "-",
					     kStages[a->mobStage < 6 ? a->mobStage : 0], int(a->mobState), static_cast<long>(npcs.bodyState(*a)), double(off));
					reportLine("ani %-24.24s %-8.8s", npcs.animationName(*a), npcs.doing(*a));
				}
				// What they notice: reactions a second, how many look at
				// somebody, fights the scripts wanted that were not begun (the
				// menu's "Драки жителей" off), and what the nearest said last.
				const g3::NpcActor* near = npcs.nearest();
				reportLine("perc %2lu/s look %lu nf %lu%s %-12.12s", static_cast<unsigned long>((ns.perceived - lastPerceived) * 2),
				     static_cast<unsigned long>(ns.looking), static_cast<unsigned long>(ns.fightsRefused), app.npcs().config().perceive ? "" : " OFF",
				     near && near->said[0] ? near->said : "");
				lastPerceived = ns.perceived;
				// Fighting: what the character holds, its hit points, the target;
				// blows landed and taken by it, and of everybody: blows, parried,
				// knocked out, killed, moves of the fight AI.
				{
					const g3::App::FightStats& fst = app.fightStats();
					const int t = app.target();
					// (u: potions and food used / spoilt by a blow / refused.)
					const g3::App::ItemStats& is = app.itemStats();
					reportLine("fight %s hp %ld/%ld %s%-7.7s u%lu/%lu/%lu", g3::weaponWord(app.hero().weapon())[0] ? g3::weaponWord(app.hero().weapon()) : "-",
					     static_cast<long>(app.heroHitPoints()), static_cast<long>(app.heroHitPointsMax()), app.targetLocked() ? "@" : "",
					     t >= 0 ? npcs.game().nameOf(npcs.game().npc(g3::u32(t))) : "", static_cast<unsigned long>(is.used),
					     static_cast<unsigned long>(is.interrupted), static_cast<unsigned long>(is.refused));
					// (d: the damage of the character's last blow; ai: moves of the fight
					// AI, and where its tables are from - F FIGHT.DAT, S the scripts,
					// - none read yet; "!" moves picked without a table.)
					reportLine("blows %lu/%lu d%ld all %lu par %lu ko %lu x %lu ai %lu%c%s", static_cast<unsigned long>(fst.blowsLanded),
					     static_cast<unsigned long>(fst.blowsTaken), static_cast<long>(fst.lastDamage), static_cast<unsigned long>(ns.blows),
					     static_cast<unsigned long>(ns.parried), static_cast<unsigned long>(ns.knockedOut), static_cast<unsigned long>(ns.killed),
					     static_cast<unsigned long>(ns.moves), ns.fightAi, ns.movesUntabled ? "!" : "");
				}
			} else {
				reportLine("scripts: none in data.pak");
			}
			reportLines = gReportCount;
			msAccum = 0.0f;
			msWorst = 0.0f;
			updateAccum = 0.0f;
			updateWorst = 0.0f;
			partSum = g3::UpdateTimes();
			collQueries = collTests = 0;
			lateFrames = 0;
			fadingSum = 0;
		}
		// The bottom screen: the inventory's picture when it has changed, or
		// one line of the report per frame, padded to overwrite the old one.
		const bool wantReport = app.settings().report != 0;
		if (wantReport != reportShown) {
			reportShown = wantReport;
			consoleClear();
			panelShown = false;
		}
		if (!wantReport) {
			if (app.panel().draw(app.inventory(), !panelShown)) showPanel(app.panel().canvas());
			panelShown = true;
		}
		const int row = int(frame % 30);
		if (reportShown && row < reportLines) printf("\x1b[%d;1H%-39.39s", row + 1, gReport[row]);
	}

	// Leaving. Each step says so on the bottom screen first: should the
	// console ever hang here, the last line tells where.
	exitStep("exit 1/4: saving the position");
	app.saveState(kStatePath);
	app.settings().save(kSettingsPath);
	aptUnhook(&gAptCookie);
	exitStep("exit 2/4: closing the world");
	app.shutdown();
	exitStep("exit 3/4: closing the graphics");
	gfx::shutdown();
	// The CPU mode is the whole system's, not this program's: the launcher
	// that comes back and the HOME Menu behind it must not inherit it.
	osSetSpeedupEnable(false);
	exitStep("exit 4/4: done");
	gfxExit();
	return 0;
}
