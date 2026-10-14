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
//   B             jump            A  attack (the D-Pad held left/right: a blow from that side);
//                                    hold for a combo: press again as a blow ends
//   Y             parry (held)    X  draw the last weapon / put it away
//   ZL            melee weapon (fists without one)
//   ZR            lock on to the enemy in front / let go (drawing a weapon locks
//                 on to a hostile one in front by itself: the menu's "Автозахват цели")
//   D-Pad Left/Right  with a weapon in hand: the direction of a blow (held as A is pressed)
//   D-Pad Up      with empty hands: the fists on / off
//   D-Pad Down    sneak on / off
//   L             quick magic (not yet)
//   START         pause menu: continue, graphics, controls, faults, save, leave
//   (console: SELECT takes the bow; classic: L+SELECT the bow, SELECT the spell)
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
//   R  the vertex shader of the versions before characters; no character at all (the view is first person
//      for this run only: the menu's setting is not changed)
//   SELECT   the inhabitants are not drawn (the scripts run all the same)
//   D-Pad Up the inhabitants only stand where their routine has them: no
//            states, no walking - as in the version before they lived
//   ZL       whole chunks are drawn, as before they were cut into clusters
//            (what the clusters save shows in "tris" and "gpu")
//   ZR       the picture is squeezed into half the screen: half the pixels, the same triangles
//   ZL+ZR    (both: neither of the two above) the vertex shader of the version before the fast one
//            (world.v.pica: projection and view apart, one code for everything), to compare
//   D-Pad Down  steady objects (sets the menu's "Объекты: Устойчиво")
//   D-Pad Left  nobody perceives anything (sets the menu's "Восприятие: Выкл")
//   D-Pad Right objects appear at once, without dissolving (the menu's
//            "Растворение" otherwise decides, from the next start)
#include "lang.h"
#include "app.h"
#include "audio_3ds.h"
#include "display_rate.h"
#include "reproject_3ds.h"
#include "gfx_c3d.h"
#include "pacer.h"
#include "sys.h"

#include <3ds.h>
#include <citro3d.h>

#include <malloc.h>
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
// The memory split. libctru's own gives the linear heap (where meshes and textures live, what the GPU reads) at most
// 32 MB and the rest to the ordinary heap: on a New 3DS with 124 MB that is 32 + 90 (HUD `sys`). The world's cache
// of chunks lives in the linear heap, and `rej`/`ev` of a long walk in the market showed it too small, so it takes
// 48 MB here, if the system gave enough to leave the ordinary heap at least 56 MB (else the library's split).
static const u32 kLinearWanted = 48u << 20, kHeapLeast = 56u << 20;
extern "C" {
extern char* fake_heap_start;
extern char* fake_heap_end;
extern u32 __ctru_heap, __ctru_linear_heap, __ctru_heap_size, __ctru_linear_heap_size;
void __system_allocateHeaps(void) {
	Handle reslimit = 0;
	if (R_FAILED(svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE))) svcBreak(USERBREAK_PANIC);
	s64 maxCommit = 0, currentCommit = 0;
	ResourceLimitType type = RESLIMIT_COMMIT;
	svcGetResourceLimitLimitValues(&maxCommit, reslimit, &type, 1);
	svcGetResourceLimitCurrentValues(&currentCommit, reslimit, &type, 1);
	svcCloseHandle(reslimit);
	const u32 remaining = u32(maxCommit - currentCommit) & ~0xFFFu;
	if (remaining >= kLinearWanted + kHeapLeast) {
		__ctru_linear_heap_size = kLinearWanted;
	} else {
		// As libctru: half and half, the ordinary heap at most 24 MB, the linear one at most 32.
		__ctru_linear_heap_size = (remaining / 2) & ~0xFFFu;
		if (remaining - __ctru_linear_heap_size > (24u << 20)) {
			__ctru_linear_heap_size = remaining - (24u << 20);
			if (__ctru_linear_heap_size > (32u << 20)) __ctru_linear_heap_size = 32u << 20;
		}
	}
	__ctru_heap_size = remaining - __ctru_linear_heap_size;
	if (R_FAILED(svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0x0, __ctru_heap_size, MEMOP_ALLOC, MemPerm(MEMPERM_READ | MEMPERM_WRITE)))) svcBreak(USERBREAK_PANIC);
	if (R_FAILED(svcControlMemory(&__ctru_linear_heap, 0x0, 0x0, __ctru_linear_heap_size, MEMOP_ALLOC_LINEAR, MemPerm(MEMPERM_READ | MEMPERM_WRITE)))) svcBreak(USERBREAK_PANIC);
	mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
	fake_heap_start = reinterpret_cast<char*>(__ctru_heap);
	fake_heap_end = fake_heap_start + __ctru_heap_size;
}
}

// The menu's settings.
static const char* kSettingsPath = "sdmc:/3ds/gothic3ds/settings.bin";
static const char* kHitchPath = "sdmc:/3ds/gothic3ds/hitches.txt";

// The log of dropped frames: a line for every frame that took well over a refresh, with where the time went. Kept
// in memory and written to the card in batches (and at the end), so that the writing itself does not make the
// hitches it is there to find.
// (fix88: the file is made new at every start, with a line saying which run it is; a batch is 160 lines and goes
// to the card only when it is full, when the game ends, or in the pause menu - the old 48 lines at 200-300 ms each
// were hitches of their own, and the file grew from run to run.)
static const int kHitchBatch = 160;
static char gHitchLines[kHitchBatch][300];
static int gHitchCount = 0;
static unsigned long gHitchTotal = 0;

static void flushHitches() {
	if (gHitchCount == 0) return;
	if (FILE* f = fopen(kHitchPath, "a")) {
		for (int i = 0; i < gHitchCount; ++i) fputs(gHitchLines[i], f);
		fclose(f);
	}
	gHitchCount = 0;
}

static void startHitchLog(const char* what) {
	if (FILE* f = fopen(kHitchPath, "w")) {
		fprintf(f, "# gothic3ds fix90 hitches: %s\n", what);
		fclose(f);
	}
	gHitchCount = 0;
	gHitchTotal = 0;
}


// The HOME Menu was opened: the game stands still in its pause menu meanwhile.
static volatile bool gSuspended = false;
static aptHookCookie gAptCookie;
static void onApt(APT_HookType hook, void*) {
	if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP) {
		gSuspended = true;
		g3::reproject::pause(true);
		g3::display::restore(true); // (the system's own timing while it has the screen; put back on return)
	} else if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP) {
		g3::reproject::pause(false);
		g3::display::resume();
	}
}

// The application: 13 KB, with the character's poses in it. Not a local of
// main(): the stack is 32 KB.
static App gApp;
// The DSP: sound plays through it (without the DSP firmware on the card it stays shut and the game is silent).
static g3::Audio3ds gAudio;
// What the sound engine met that the scripts name and the archive lacks.
static const char* kSoundReportPath = "sdmc:/3ds/gothic3ds/sound.txt";

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

	// (The black pad in front of the first framebuffer for the re-projection: it has to be allocated before libctru's.)
	g3::reproject::reserve();
	gfxInitDefault();
	gfxSet3D(false);
	g3::reproject::noteInit();
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
	const bool zl = (startKeys & KEY_ZL) != 0, zr = (startKeys & KEY_ZR) != 0;
	const bool classicShader = zl && zr;           // both: the earlier shader, to compare with the fast one
	const bool wholeChunks = zl && !zr;            // no culling inside chunks
	const bool halfPicture = zr && !zl;            // half the pixels, the same triangles
	const bool steadyKey = (startKeys & KEY_DDOWN) != 0; // objects drawn farther, never taken away by the pacer
	const bool blind = (startKeys & KEY_DLEFT) != 0;     // no perception
	g3::Settings settings;
	const bool hadSettings = settings.load(kSettingsPath);
	// Objects dissolve in and out (a shader of its own): as the menu says,
	// unless D-Pad Right is held.
	const bool dissolve = settings.dissolve && !(startKeys & KEY_DRIGHT);
	gfx::c3dSetClassicFrames(classic);
	gfx::c3dSetPlainShader(plainShader);
	gfx::c3dSetClassicShader(classicShader);
	gfx::c3dSetDoubleDraws((startKeys & KEY_START) != 0); // hold START at start: every draw twice
	gfx::c3dSetFillTest(halfPicture); // hold ZR at start: half the pixels, the same triangles
	gfx::c3dSetFadeShader(dissolve);
	gfx::c3dSetSupersampleMode(settings.antialias); // (a change in the menu counts after a restart)
	// The top screen: wide (800x240) or stereo 3D (the same, after a restart; the 3D not together with the smoothing). The wide
	// one wins if asked for; with the smoothing it is smoothed up and down only (across, it is twice as fine as it was).
	gfx::c3dSetWide(settings.wideScreen());
	gfx::c3dSetStereo(settings.stereo3D() && settings.antialias == 0);

	App& app = gApp;
	// Chunks are read, unpacked and given their collision grids on a worker
	// thread; the frame only copies the finished mesh into video memory.
	g3::StreamConfig& stream = app.world().config();
	stream.background = background;
	stream.maxLoadsPerFrame = background ? 1 : 2;
	stream.budget = &app.frameBudget();
	app.npcs().config().budget = &app.frameBudget();
	stream.clusterCulling = !wholeChunks;
	if (!dissolve) stream.fadeMs = 0.0f;
	// The card is read ahead on a thread of its own: 4 MB of models, textures, animations and chunks waiting
	// (the menu's "Чтение наперёд"). The chunks ahead of the way the camera goes are wanted 12 m sooner.
	app.setReadAheadBudget(4u << 20);
	stream.headLead = 1200.0f;
	// Sound before the application, which hands it to the sound engine.
	if (gAudio.open()) app.setAudio(&gAudio);
	else printf("no sound: the DSP firmware is missing\n(dump it with DSP1 to sdmc:/3ds/dspfirm.cdc)\n");
	bool ok = gfx::init();
	if (!ok) printf("gfx init failed\n");
	if (ok && !app.init(kPakPath, nullptr)) {
		printf("%s\nexpected at %s\n", app.error(), kPakPath);
		ok = false;
	}
	// The screen's refresh rate (only read here; written when the menu asks for it).
	// The wide screen (800x240) does not take the changed timing at all (stripes at 80 Hz, and the 40 Hz and floating 30 do not work either):
	// there the screen stays as the system has it, and the modes that need the timing run as without it.
	const bool displayOk = ok && !gfx::c3dWide() && g3::display::init();
	if (!ok) {
		printf("\nPress START to exit.\n");
		waitForStart();
		gAudio.close();
		gfx::shutdown();
		osSetSpeedupEnable(false);
		gfxExit();
		return 1;
	}

	// The linear heap, where meshes and textures live, is 32 MB; the command
	// buffers, the characters, the particles and the sound are in it too. The
	// world takes what is free there, less a reserve for those (World::budget),
	// up to 28 MB - before, a fixed 20 MB, whatever was free. The reserve is 5 MB (it was 6): on a video
	// from Archolos at night `lin` stayed at 5.5-7.5 MB with 6 reserved, so what the rest of the game takes
	// beyond what it has already is well under 1 MB; and the world's refusals (`rej`) were that reserve, not
	// the allocator (`nm`).
	app.world().config().memoryBudget = (__ctru_linear_heap_size > (40u << 20) ? 44u : 28u) << 20;
	app.world().config().memoryReserve = 5u << 20;
	app.world().config().freeMemory = []() -> g3::u32 { return g3::u32(linearSpaceFree()); };
	// Continue where the last session ended, unless L is held at start.
	const bool hadState = !(startKeys & KEY_L) && app.loadState(kStatePath);
	// The settings: the draw distance of an older state.bin counts once, for
	// a settings.bin that is not there yet.
	if (!hadSettings && hadState) settings.drawMetres = g3::u8(g3::clamp(int(app.drawDistance() / 1000.0f + 0.5f) * 10, 40, 200));
	if (steadyKey) settings.objects = g3::OBJECTS_STEADY;
	if (startKeys & KEY_A) settings.firstPerson = 1;
	if (blind) settings.perception = 0;
	if (standing) settings.life = 0;
	app.settings() = settings;
	app.applySettings();
	{
		char what[160];
		snprintf(what, sizeof(what), "preset %d, draw %d m, objects %d, life %d, shader %s", int(settings.gfxPresetOf()),
		         int(settings.drawMetres), int(settings.objects), int(settings.life), plainShader ? "plain" : "normal");
		startHitchLog(what);
	}
	if (plainShader) app.setThirdPerson(false); // (no character is drawn; for this run only, the menu keeps its setting)
	app.setStatePath(kStatePath);
	if (nobody || plainShader) app.npcs().config().maxShown = 0;
	// Characters coming into range: one set going per frame, the models of those on their way read ahead.
	app.npcs().config().startsPerFrame = 1;
	app.npcs().config().reachFloor = 0.6f;  // in a heavy scene the crowd is shown no nearer than 60 % of its range
	app.npcs().config().farTickEvery = 2;   // and those beyond 15 m are stepped every other frame
	app.npcs().config().prefetchRange = 3000.0f; // (their models are read on the read-ahead's thread; the frame only builds them)
	printf("loading...\n");
	app.world().preload(app.camera().pos);
	consoleClear();
	aptHook(&gAptCookie, onApt, nullptr);
	// When a refresh has no new picture, the old one is shown turned by the turn of the view (the menu's "Доворот кадра").
	const bool reprojectOk = g3::reproject::start();
	hidSetRepeatParameters(24, 6);

	u32 frame = 0;
	float msAccum = 0.0f, msWorst = 0.0f, updateAccum = 0.0f, updateWorst = 0.0f;
	u32 lateFrames = 0; // frames of this report that missed their refresh
	float refreshHz = 60.0f, refreshNeed = 16.7f; // the screen's rate as set, and how long the frames need (smoothed)
	bool displayWritten = false;
	u32 lastPerceived = 0;
	g3::FramePacer pacer;
	int pacedMode = app.settings().frameRate;
	int interval = 1;
	const auto pace = [&]() {
		const g3::Settings& st = app.settings();
		pacer.cfg.minDetail = st.minDetail(); // (steady: too heavy means 30 fps, the objects stay)
		pacer.cfg.halfOnly = st.frameRate == g3::FRAMES_30 || (st.frameRate == g3::FRAMES_30F && !displayOk);
		pacer.cfg.freeRate = st.frameRate == g3::FRAMES_FREE || (st.frameRate == g3::FRAMES_40 && displayOk) || (st.frameRate == g3::FRAMES_30F && displayOk) || (st.refreshHold() && displayOk);
		// Leaving a mode (the steady 30 above all) starts the pacer afresh: its half rate must not outlive the setting.
		if (st.frameRate != pacedMode) {
			pacedMode = st.frameRate;
			pacer.reset();
			app.world().config().detailScale = pacer.detailScale();
			app.setDistanceScale(pacer.distanceScale());
			if (interval != 1) {
				interval = 1;
				C3D_FrameRate(60.0f);
			}
		}
	};
	pace();
	int reportLines = 0;
	u32 lastStateCalls = 0;
	g3::UpdateTimes partSum;               // where update() spent its time, summed over a report
	g3::RenderTimes drawSum;               // where render() spent the CPU's time, summed over a report (fix86)
	g3::UpdateTimes partWorst;             // ... and the worst frame of each part
	float lootWorst = 0.0f;
	float npcClockWorst = 0.0f, npcSelectWorst = 0.0f, npcTickWorst = 0.0f; // of the inhabitants' part: clock / choosing / steps
	u32 collQueries = 0, collTests = 0;    // and what its collision queries looked at
	u32 slopeWrites = 0;                   // L+R+SELECT: records written (a failed write counts as none)
	float lastYaw = app.camera().yaw;      // how fast the view turns: the pacer gives no objects back meanwhile
	u32 fadingSum = 0;                     // batches drawn dissolving, summed over a report
	bool slopeFailed = false;
	float leaveHeld = 0;                   // START+SELECT held this long: leave
	bool flicked = false;                  // the C-Stick was flicked aside (locked on: the next target)
	bool reportShown = true;
	bool panelShown = false;               // the inventory's picture is on the bottom screen
	u32 swallowed = 0;                     // buttons a page or the menu used: the game ignores them until let go

	while (aptMainLoop()) {
		hidScanInput();
		// Stereo 3D: the 3D slider decides, between frames (0: one picture as usual).
		{
			const float slider = app.settings().stereo3D() ? osGet3DSliderState() : 0.0f;
			gfx::stereoSet(slider > 0.02f && gfx::stereoAvailable());
			app.setStereo(gfx::stereoActive() ? slider : 0.0f);
		}
		const u32 down = hidKeysDown(), held = hidKeysHeld(), repeat = hidKeysDownRepeat();
		g3::Menu& menu = app.menu();
		if (gSuspended) {
			gSuspended = false;
			if (!menu.isOpen()) menu.open();
		}
		{
			// The menu stands still anyway: the hitch log goes to the card now, not in the middle of play.
			static bool menuWas = false;
			if (menu.isOpen() && !menuWas) flushHitches();
			menuWas = menu.isOpen();
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
		const bool inventoryScreen = app.settings().report == 0 || app.talking();
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
		// A button the inventory (or the menu) took stays taken while it is held:
		// B closes the inventory and must not then also be a jump.
		swallowed = (swallowed | taken | (menu.isOpen() ? held : 0u)) & held;
		const u32 gameDown = down & ~taken & ~swallowed, gameHeld = held & ~taken & ~swallowed;

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
				if (act == g3::MENU_RESET_GRAPHICS) {
					pace();
					app.settings().save(kSettingsPath);
				}
			}
			app.soundIdle(dt);
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
				// ZR locks on to a target / lets go; the D-Pad gives a blow its direction.
				in.focus = (gameDown & KEY_ZR) != 0;
				in.blowDir = (gameHeld & KEY_DRIGHT) ? 1 : (gameHeld & KEY_DLEFT) ? -1 : 0;
				// As the original's: with A held, a press of the D-Pad is the blow - up ahead,
				// left and right to the sides (a press of A with the D-Pad already held works too).
				if ((gameHeld & KEY_A) && !(gameDown & KEY_A)) {
					if (gameDown & KEY_DLEFT) {
						in.attack = true;
						in.blowDir = -1;
					} else if (gameDown & KEY_DRIGHT) {
						in.attack = true;
						in.blowDir = 1;
					} else if (gameDown & KEY_DUP) {
						in.attack = true;
						in.blowDir = 0;
					}
				}
				in.dpadUp = (gameDown & dpad & KEY_DUP) != 0 && !(gameHeld & KEY_A); // (with A held it is a blow, not the fists)
				in.toggleSneak = (gameDown & dpad & KEY_DDOWN) != 0;
				in.dodge = in.toggleSneak; // (the application takes it as a dodge while a weapon is in hand)
				// Magic: A held charges a spell; in the stance the D-Pad's left and right choose the spell.
				in.attackHeld = (gameHeld & KEY_A) != 0;
				if (app.hero().weapon() == g3::WEAPON_MAG && !(gameHeld & KEY_A) && !rHeld) {
					if (gameDown & KEY_DLEFT) in.spellStep = -1;
					else if (gameDown & KEY_DRIGHT) in.spellStep = 1;
				}
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
					if ((gameDown & KEY_L) && !rHeld) in.magic = true;
				} else {
					in.strafe = (gameHeld & KEY_R ? 1.0f : 0.0f) - (gameHeld & KEY_L ? 1.0f : 0.0f);
				}
				// The classic scheme has L for the step aside: the magic stance is SELECT's.
				if ((gameDown & KEY_SELECT) && !rHeld && !(held & KEY_START) && !inventoryScreen) {
					if (console) in.rangedSlot = true; // (the inventory is on the bottom screen)
					else if (gameHeld & KEY_L) in.rangedSlot = true;
					else in.magic = true;
				}
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
				app.toast(app.saveState(kStatePath) ? g3::tr("Позиция сохранена") : g3::tr("Не удалось сохранить"));
			} else if (quickLoad) {
				app.toast(app.loadState(kStatePath) ? g3::tr("Позиция загружена") : g3::tr("Сохранённой позиции нет"));
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
				partSum.loot += ut.loot;
				partSum.npcUpdate += ut.npcUpdate;
				partSum.triggers += ut.triggers;
				partSum.particles += ut.particles;
				partSum.heroControl += ut.heroControl;
				partSum.heroMove += ut.heroMove;
				partSum.heroBodies += ut.heroBodies;
				partSum.heroFollow += ut.heroFollow;
				partSum.heroRest += ut.heroRest;
				lootWorst = max(lootWorst, ut.loot);
				npcClockWorst = max(npcClockWorst, app.npcs().stats().clockMs);
				npcSelectWorst = max(npcSelectWorst, app.npcs().stats().selectMs);
				npcTickWorst = max(npcTickWorst, app.npcs().stats().tickMs);
				partWorst.stream = max(partWorst.stream, ut.stream);
				partWorst.npcs = max(partWorst.npcs, ut.npcs);
				partWorst.script = max(partWorst.script, ut.script);
				partWorst.player = max(partWorst.player, ut.player);
				partWorst.camera = max(partWorst.camera, ut.camera);
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
			{
				const g3::RenderTimes& rt = app.renderTimes();
				drawSum.world += rt.world;
				drawSum.npcs += rt.npcs;
				drawSum.things += rt.things;
				drawSum.hero += rt.hero;
				drawSum.rest += rt.rest;
			}
		} // (not in the menu)

		// The re-projection's side: the picture just handed to the GPU was drawn for this view.
		g3::reproject::enable(reprojectOk && app.settings().reproject != 0 && !gfx::stereoActive()); // (not in 3D: the display is told its own way there)
		g3::reproject::submit(app.camera().yaw, app.camera().fovY);

		// What the frame cost decides how much detail the next one gets, and
		// whether the display runs at 60 or a steady 30. With overlapped
		// frames the CPU and the GPU work at the same time and the slower of
		// the two sets the pace; in citro3d's own order the GPU waits for the
		// CPU, and a frame takes the sum. (Measured in that order on a New
		// 3DS: 28% + 97% gave 20.9 ms, which is 125% of 16.7 ms.)
		const bool lock40 = displayOk && app.settings().frameRate == g3::FRAMES_40;
		// "Hold 72/80": the detail is kept to that rate's frame time (12.5 ms at 80), the screen follows the frame.
		// "Exactly 30, floating": every frame has 33 ms, and the screen runs at 26..30 Hz as the frame needs.
		const bool lock30 = displayOk && app.settings().frameRate == g3::FRAMES_30F;
		const bool hold = !lock40 && !lock30 && displayOk && app.settings().refreshHold() && interval == 1 && !menu.isOpen();
		const gfx::FrameStats fs = gfx::frameStats();
		const float load = (fs.overlapped ? max(fs.cpuMs + updateMs, fs.gpuMs) : fs.cpuMs + fs.gpuMs) * (lock40 ? 0.04f : lock30 ? 0.03f : hold ? app.settings().refreshMaxHz() * 0.001f : 0.06f);
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
			app.setDistanceScale(pacer.distanceScale());
			// Graphics+ gives way first: once the pacer takes objects away, the plants
			// stand still and only the hero keeps a shadow, until all objects are back.
			const bool heavy = app.gfxRelief() ? pacer.detailScale() < 0.999f || pacer.interval() == 2 : pacer.detailScale() < 0.95f;
			if (heavy != app.gfxRelief()) app.setGfxRelief(heavy);
			if (pacer.interval() != interval) {
				interval = pacer.interval();
				C3D_FrameRate(interval == 2 ? 30.0f : 60.0f);
			}
		}

		// The screen's refresh rate follows the frame: as long as it takes (CPU and GPU in parallel), a little over, between
		// the menu's limits - a frame that is a little late is shown when it is ready, not a whole refresh later. At
		// half rate (30 fps) and in the menu the screen stays as the system has it.
		if (lock40) {
			// The steady 40: the screen itself at 40 Hz, every frame has 25 ms; frames the pacer cannot fit still come at 40.
			if (!displayWritten || fabsf(refreshHz - 40.0f) > 0.7f) {
				const float made = g3::display::set(40.0f);
				if (made > 0.0f) {
					refreshHz = made;
					displayWritten = true;
				}
			}
		} else if (lock30) {
			// The screen follows the frame, between 26 and 30 Hz: quick to slow down, slow to speed up.
			const float need = fs.overlapped ? max(fs.cpuMs + updateMs, fs.gpuMs) : fs.cpuMs + fs.gpuMs;
			if (updateMs < 50.0f) refreshNeed = need > refreshNeed ? need : refreshNeed * 0.92f + need * 0.08f;
			const float wantHz = clamp(1000.0f / (refreshNeed * 1.06f + 0.5f), 26.0f, 30.0f);
			if (!displayWritten || fabsf(wantHz - refreshHz) > 0.4f) {
				const float made = g3::display::set(wantHz);
				if (made > 0.0f) {
					refreshHz = made;
					displayWritten = true;
				}
			}
		} else if (displayOk) {
			const float maxHz = app.settings().refreshMaxHz();
			if (maxHz <= 0.0f || interval != 1 || menu.isOpen() || updateMs >= 50.0f) {
				if (displayWritten && (maxHz <= 0.0f || interval != 1 || menu.isOpen())) {
					g3::display::restore();
					displayWritten = false;
				}
			} else {
				const float need = fs.overlapped ? max(fs.cpuMs + updateMs, fs.gpuMs) : fs.cpuMs + fs.gpuMs;
				// Quick to slow down, slow to speed up: a late frame must not be followed by another one.
				refreshNeed = need > refreshNeed ? need : refreshNeed * 0.92f + need * 0.08f;
				const float wantHz = clamp(1000.0f / (refreshNeed * 1.06f + 0.5f), 45.0f, maxHz);
				if (!displayWritten || fabsf(wantHz - refreshHz) > 1.5f) {
					const float made = g3::display::set(wantHz);
					if (made > 0.0f) {
						refreshHz = made;
						displayWritten = true;
					}
				}
			}
		}
		gfx::c3dSyncTopOnly(displayWritten && fabsf(refreshHz - 60.0f) > 1.5f);
		const float periodMs = displayWritten ? 1000.0f / refreshHz : 16.7f;
		// What this frame left of its period: loads (chunks, textures, models) only go ahead if they fit in what the last one left.
		{
			const float took = fs.overlapped ? max(fs.cpuMs + updateMs, fs.gpuMs) : fs.cpuMs + fs.gpuMs;
			app.frameBudget().headroomMs = updateMs < 50.0f ? periodMs * float(interval) - took : 1.0e9f;
		}
		{
			// Loads since the last frame (the counters of the inhabitants and of the world only grow).
			static g3::u32 lastModels = 0, lastClips = 0, lastChunks = 0, lastEvict = 0;
			const g3::NpcStats& nst = app.npcs().stats();
			const g3::ReadAheadStats raStats = app.readAhead().stats();
			const WorldStats& wst = app.world().stats();
			const g3::u32 dModels = nst.loads - lastModels, dClips = nst.clips - lastClips;
			const g3::u32 dChunks = wst.chunksLoaded - lastChunks, dEvict = wst.evictions - lastEvict;
			lastModels = nst.loads;
			lastClips = nst.clips;
			lastChunks = wst.chunksLoaded;
			lastEvict = wst.evictions;
			if (fs.frameMs > max(periodMs * float(interval) * 1.4f, 40.0f) && frame > 60) {
				const g3::UpdateTimes& ut = app.times();
				const g3::Vec3 e = app.player().eye();
				++gHitchTotal;
				snprintf(gHitchLines[gHitchCount++], sizeof(gHitchLines[0]),
				         "f%lu %.1fms cpu%.1f gpu%.1f upd%.1f | w%.1f n%.1f s%.1f p%.1f c%.1f l%.1f | clk%.1f sel%.1f tick%.1f | mdl%lu clip%lu chunk%+ld ev%lu pend%lu | tr%lu %.0f %.0f %.0f | nu%.1f t%.1f x%.1f hc%.1f m%.1f b%.1f f%.1f o%.1f | msh%lu rd%lu hit%lu | ld%.0f wk%.0f gr%.0f ra%lu/%lu iv%d hz%.0f k2:%d\n",
				         static_cast<unsigned long>(frame), double(fs.frameMs), double(fs.cpuMs), double(fs.gpuMs), double(updateMs), double(ut.stream),
				         double(ut.npcs), double(ut.script), double(ut.player), double(ut.camera), double(ut.loot), double(nst.clockMs),
				         double(nst.selectMs), double(nst.tickMs), static_cast<unsigned long>(dModels), static_cast<unsigned long>(dClips),
				         static_cast<long>(dChunks), static_cast<unsigned long>(dEvict), static_cast<unsigned long>(wst.chunksPending),
				         static_cast<unsigned long>(wst.trianglesDrawn), double(e.x), double(e.y), double(e.z), double(ut.npcUpdate),
				         double(ut.triggers), double(ut.particles), double(ut.heroControl), double(ut.heroMove), double(ut.heroBodies),
				         double(ut.heroFollow), double(ut.heroRest), static_cast<unsigned long>(nst.meshes),
				         static_cast<unsigned long>(nst.meshReads), static_cast<unsigned long>(nst.meshHits), double(nst.loadMs), double(nst.wakeMs),
				         double(nst.groundMs), static_cast<unsigned long>(raStats.hits), static_cast<unsigned long>(raStats.misses), interval,
				         double(displayWritten ? refreshHz : 60.0f), g3::sys::workersOnSecondCore() ? 1 : 0);
				if (gHitchCount >= kHitchBatch) flushHitches();
			}
		}
		if (fs.frameMs > 0.0f) {
			msAccum += fs.frameMs;
			msWorst = max(msWorst, fs.frameMs);
			lateFrames += fs.frameMs > periodMs * float(interval) * 1.25f;
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
			reportLine("frame %5.1f ms avg %5.1f worst %2.0fHz%s", double(msAccum / 30.0f), double(msWorst), double(displayWritten ? refreshHz : 60.0f),
			     !displayOk && app.settings().refresh ? "!" : g3::display::failure() == 3 ? "!w" : "");
			reportLine("cpu %3.0f%%  gpu %3.0f%%  load %3.0f%%", double(fs.cpuMs * 6.0f), double(fs.gpuMs * 6.0f),
			     double(load * 100.0f));
			reportLine("update %4.2f ms avg %5.1f worst", double(updateAccum / 30.0f), double(updateWorst));
			// update() by its parts: chunks, inhabitants (of which scripts),
			// the player's character, the camera - and the collision queries
			// of a frame with the triangles they looked at.
			// (average/worst frame of each: a stall shows in the worst, not in the average)
			reportLine("upd w%.1f/%.0f n%.1f/%.0f s%.1f/%.0f p%.1f/%.0f c%.1f/%.0f",
			     double(partSum.stream / 30.0f), double(partWorst.stream), double(partSum.npcs / 30.0f),
			     double(partWorst.npcs), double(partSum.script / 30.0f), double(partWorst.script),
			     double(partSum.player / 30.0f), double(partWorst.player), double(partSum.camera / 30.0f),
			     double(partWorst.camera));
			// (fix86, averages, ms) n: the inhabitants' update, loot, triggers, particles; p: steering, move, bodies, pose, rest;
			// then the CPU's draw-call time: world, inhabitants, things and movers, hero, the rest. The collision line went.
			reportLine("n u%.1f l%.1f t%.1f x%.1f p c%.1f m%.1f b%.1f f%.1f o%.1f", double(partSum.npcUpdate / 30.0f),
			     double(partSum.loot / 30.0f), double(partSum.triggers / 30.0f), double(partSum.particles / 30.0f),
			     double(partSum.heroControl / 30.0f), double(partSum.heroMove / 30.0f), double(partSum.heroBodies / 30.0f),
			     double(partSum.heroFollow / 30.0f), double(partSum.heroRest / 30.0f));
			reportLine("drw w%.1f n%.1f t%.1f h%.1f o%.1f col%lu/%lu", double(drawSum.world / 30.0f), double(drawSum.npcs / 30.0f),
			     double(drawSum.things / 30.0f), double(drawSum.hero / 30.0f), double(drawSum.rest / 30.0f),
			     static_cast<unsigned long>(collQueries / 30), static_cast<unsigned long>(collTests / 30));
			reportLine("loader %-6s job %5.1f ms  fly %lu",
			     !app.world().backgroundStreaming() ? "off" : app.world().workerThread() ? "thread" : "inline",
			     double(app.world().takeWorstLoadMs()), static_cast<unsigned long>(ws.chunksInFlight));
			{
				// Read-ahead: reads that found their bytes waiting / went to the card; entries and KB waiting.
				const g3::ReadAheadStats ra = app.readAhead().stats();
				if (app.readAhead().running())
					reportLine("ahead %s %lu+/%lu- w%lu %luK", app.readAhead().enabled() ? "on" : "off",
					     static_cast<unsigned long>(ra.hits), static_cast<unsigned long>(ra.misses),
					     static_cast<unsigned long>(ra.entries), static_cast<unsigned long>(ra.bytes >> 10));
			}
			// (fd: objects dissolving in or out, on average; "off": the dissolve is off.)
			if (dissolve && gfx::fadeSupported())
				reportLine("detail %3.0f%%  %2d fps  late %2lu/30 fd %lu", double(app.world().config().detailScale * 100.0f),
				     interval == 2 ? 30 : 60, static_cast<unsigned long>(lateFrames), static_cast<unsigned long>(fadingSum / 30));
			else
				reportLine("detail %3.0f%%  %2d fps  late %2lu/30 fd off", double(app.world().config().detailScale * 100.0f),
				     interval == 2 ? 30 : 60, static_cast<unsigned long>(lateFrames));
			// Triangles drawn, of those in the chunks in view (the rest is what the clusters save); draw calls; chunks.
			reportLine("tris %6lu/%6lu dr %4lu ch %2lu%s", static_cast<unsigned long>(ws.trianglesDrawn),
			     static_cast<unsigned long>(ws.trianglesInChunks), static_cast<unsigned long>(ws.drawCalls),
			     static_cast<unsigned long>(ws.chunksDrawn), wholeChunks ? " ZL" : "");
			// Where the triangles are: level and large objects / medium / small; then by distance from the camera
			// (under 12 m / 12-25 m / farther); chunks drawn as stand-ins and their triangles.
			reportLine("A%5lu M%5lu S%5lu n%5lu m%5lu f%5lu", static_cast<unsigned long>(ws.trisByDetail[0]),
			     static_cast<unsigned long>(ws.trisByDetail[1]), static_cast<unsigned long>(ws.trisByDetail[2]),
			     static_cast<unsigned long>(ws.trisByDist[0]), static_cast<unsigned long>(ws.trisByDist[1]),
			     static_cast<unsigned long>(ws.trisByDist[2]));
			// (sz: batches of small objects left out for their size: within their class's distance, too small there.)
			reportLine("lod %2lu cl %5lu sz%4lu pacer %s%s%s%s", static_cast<unsigned long>(ws.lodChunks),
			     static_cast<unsigned long>(ws.lodTriangles), static_cast<unsigned long>(ws.sizeCulled), pacing ? "on" : "off", fs.overlapped ? " ovl" : "",
			     app.settings().objects == g3::OBJECTS_STEADY ? " ST" : app.settings().objects == g3::OBJECTS_BALANCED ? " BAL" : "",
			     halfPicture ? " HALF" : (startKeys & KEY_START) ? " DBL" : "");
			// ... and the far layer: its triangles and draw calls ('-': not in data.pak, bake with --far-lod;
			// 'o': in it, but off in the menu).
			// ... and the baked visibility: clusters (and chunks) not drawn because the table says they are hidden
			// ('-': not in data.pak, bake with --vis; 'o': in it, but off in the menu; '?': on, but the camera's cell has no row).
			reportLine("bl %3lu cul %4lu mg %4lu far %5lu/%2lu%c vis %4lu%c", static_cast<unsigned long>(ws.blendedDrawCalls),
			     static_cast<unsigned long>(ws.batchesCulled), static_cast<unsigned long>(ws.batchesMerged),
			     static_cast<unsigned long>(ws.farTriangles), static_cast<unsigned long>(ws.farDrawCalls),
			     !app.world().hasFarLayer() ? '-' : app.world().farActive() ? ' ' : 'o',
			     static_cast<unsigned long>(ws.clustersHidden + ws.chunksHidden),
			     !app.world().hasVisibility() ? '-' : !app.world().visibilityActive() ? 'o' : ws.visRow ? ' ' : '?');
			{
				// The command buffer's use and the draws dropped for want of room; then Graphics+: occlusion
				// strength, wind setting and its draw calls, shadow setting and shadows laid. '-' after a value:
				// not in data.pak (bake with --ao / --wind); R: given way to the frame rate (the pacer).
				const g3::Settings& st = app.settings();
				reportLine("cmd%3.0f%% x%lu g+ao%3d%c w%d%c%2lu sh%d/%lu%s%s%s", double(fs.commandBufferUse * 100.0f),
				     static_cast<unsigned long>(fs.drawsDropped), int(st.occlusion), app.world().hasOcclusion() ? ' ' : '-', int(st.wind),
				     app.world().hasWind() ? ' ' : '-', static_cast<unsigned long>(ws.windyDrawCalls), int(st.shadows),
				     static_cast<unsigned long>(app.shadows().stats().drawn), app.gfxRelief() ? " R" : "", gfx::c3dSupersampleLight() ? " AA1" : gfx::c3dSupersample() ? " AA" : "", gfx::stereoActive() ? " 3D" : gfx::c3dWide() ? " W" : "");
			}
			// (Chunks in memory: the world's own + those of medium and small objects, of all; wanted and not there: the same.)
			// (RP: the re-projection, when asked for: columns of the last move and refreshes that showed the old picture
			// moved; "RP!" - asked for, but the framebuffers were not found as expected, so it is not running.)
			char reprojectTag[20] = "";
			if (app.settings().reproject) {
				const g3::reproject::Stats rs = g3::reproject::stats();
				if (rs.running) snprintf(reprojectTag, sizeof(reprojectTag), " RP%+d %lu", rs.columns, static_cast<unsigned long>(rs.moved));
				else snprintf(reprojectTag, sizeof(reprojectTag), " RP!");
			}
			reportLine("res %3lu+%3lu/%lu pend %3lu+%lu%s", static_cast<unsigned long>(ws.chunksLoaded - ws.loadedDetail),
			     static_cast<unsigned long>(ws.loadedDetail), static_cast<unsigned long>(ws.chunksTotal),
			     static_cast<unsigned long>(ws.chunksPending - ws.pendingDetail), static_cast<unsigned long>(ws.pendingDetail), reprojectTag);
			// (Meshes - of them the objects' share -, textures and collision of the world.)
			reportLine("mesh %5.2f d%4.1f tex %5.2f MB col %4.0fK", double(mem.meshBytes) / 1048576.0, double(ws.detailBytes) / 1048576.0,
			     double(mem.textureBytes) / 1048576.0, double(ws.collisionBytes) / 1024.0);
			// (Free linear memory and the world's budget from it; chunks the budget had no room for, evicted, failed,
			// refused by the allocator though the budget had room.)
			reportLine("lin %4.1f bud %4.1fMB rej %lu ev %lu f%lu nm%lu", double(linearSpaceFree()) / 1048576.0,
			     double(ws.budgetBytes) / 1048576.0, static_cast<unsigned long>(ws.budgetRejects), static_cast<unsigned long>(ws.evictions),
			     static_cast<unsigned long>(ws.loadFailures), static_cast<unsigned long>(ws.allocRejects));
			// What the system gives the application (MB): its whole region, what is free of it, the ordinary heap (the
			// linear heap is `lin` above plus what is in use).
			reportLine("sys app%4.0f free%4.1f heap%3.0f u%4.1f lin%3.0f", double(osGetMemRegionSize(MEMREGION_APPLICATION)) / 1048576.0,
			     double(osGetMemRegionFree(MEMREGION_APPLICATION)) / 1048576.0, double(__ctru_heap_size) / 1048576.0,
			     double(mallinfo().uordblks) / 1048576.0, double(__ctru_linear_heap_size) / 1048576.0);

			reportLine("draw distance %3.0f/%3.0f m  time %02d:%02d", double(app.drawDistance() / 100.0f * app.world().config().distanceScale), double(app.drawDistance() / 100.0f), int(app.environment().timeOfDay),
			     int(app.environment().timeOfDay * 60.0f) % 60);
			reportLine("%-22.22s %-16.16s", state, app.worldName());
			// The player's eyes, wherever the camera hangs behind them.
			const g3::Vec3 eye = pl.eye();
			// Then: times the character was popped out of the rock and was
			// unstuck (see Player::unstick), and the records written to slope.txt.
			// ... and the room (BSP sector) the feet are in, with its guild (Wld_AssignRoomToGuild; -1 outside).
			char roomNote[28] = "";
			if (app.npcs().rooms().count()) {
				const g3::u16 room = app.npcs().rooms().roomAt(pl.feet);
				snprintf(roomNote, sizeof(roomNote), " r:%.10s %ld", room ? app.npcs().rooms().name(room) : "-",
				         static_cast<long>(app.npcs().game().roomGuild(app.npcs().rooms().hash(room))));
			}
			reportLine("pos %7.0f %6.0f %7.0f u%lu/%lu sd%lu%s%s", double(eye.x), double(eye.y), double(eye.z),
			     static_cast<unsigned long>(pl.popUps), static_cast<unsigned long>(pl.unstuck), static_cast<unsigned long>(slopeWrites),
			     slopeFailed ? "!" : "", roomNote);
			// The inhabitants' part, worst frame: clock (routines) / choosing and making actors / their steps.
			// ... and the world's triggers: events that reached one, events to nobody, movers on their way and drawn.
			const g3::TriggerStats& trs = app.triggers().stats();
			reportLine("npc worst clk%.0f sel%.0f tick%.0f loot%.0f ms trg e%lu u%lu mv%lu/%lu", double(npcClockWorst), double(npcSelectWorst),
			     double(npcTickWorst), double(lootWorst), static_cast<unsigned long>(trs.events), static_cast<unsigned long>(trs.unknown),
			     static_cast<unsigned long>(trs.moving), static_cast<unsigned long>(trs.drawn));
			const g3::Hero& hero = app.hero();
			if (plainShader) {
				reportLine("hero off: plain shader (R at start)");
			} else if (hero.loaded()) {
				static const char* const kRoles[g3::Hero::ROLE_COUNT] = {"stand", "run",   "walk",  "back", "step",
				                                                         "turn",  "jump",  "fall",  "swim", "float",
				                                                         "attack", "parry", "hit", "down", "draw", "use", "climb"};
				// F: the fast shader; E: the fast shader with the dissolve (fix85); C: the classic one (ZL+ZR at start); D: the old
				// dissolve shader; sw: changes of program in the frame.
				reportLine("hero %-5s %3.0f cm/s %s%s cam %s %c sw%lu", kRoles[hero.role()], double(hero.speed()),
				     hero.walking() ? "walk " : "", app.thirdPerson() ? "3rd" : "1st", app.cameraSet().fromGame ? "game" : "own",
				     gfx::c3dFastShader() ? (dissolve ? 'E' : 'F') : classicShader ? 'C' : 'D', static_cast<unsigned long>(fs.programSwitches));
			} else {
				reportLine("hero: none in data.pak");
			}
			// The scripts and the people they put into the world.
			const g3::Npcs& npcs = app.npcs();
			if (npcs.loaded()) {
				const g3::NpcStats& ns = npcs.stats();
				const g3::ScriptStats& vs = npcs.game().vm().stats();
				// (oc: in view but hidden behind walls, not drawn; the time the lines of sight took.)
				reportLine("npcs %2lu/%2lu in %2.0f m of %4lu%s oc%lu %.1fms", static_cast<unsigned long>(ns.shown),
				     static_cast<unsigned long>(ns.near), double(ns.reach / 100.0f), static_cast<unsigned long>(ns.here),
				     nobody ? " OFF" : "", static_cast<unsigned long>(ns.hidden), double(ns.sightMs));
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
				// Who last began an attack on whom ("+" begun, "-" refused: no quarrel), and: refused,
				// beasts set on by the engine, talks that stopped a walker.
				reportLine("%-28.28s d%lu p%lu s%lu", ns.lastAttack[0] ? ns.lastAttack : "no attack yet", static_cast<unsigned long>(ns.attacksDenied),
				     static_cast<unsigned long>(ns.provoked), static_cast<unsigned long>(ns.talksStopped));
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
			drawSum = g3::RenderTimes();
			partWorst = g3::UpdateTimes();
			npcClockWorst = npcSelectWorst = npcTickWorst = lootWorst = 0.0f;
			collQueries = collTests = 0;
			lateFrames = 0;
			fadingSum = 0;
		}
		// The bottom screen: the inventory's picture when it has changed, or
		// one line of the report per frame, padded to overwrite the old one.
		const bool wantReport = app.settings().report != 0 && !app.talking();
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
	g3::display::restore(); // (the screen as the system had it)
	app.settings().save(kSettingsPath);
	flushHitches();
	g3::reproject::stop();
	aptUnhook(&gAptCookie);
	exitStep("exit 2/4: closing the world");
	if (FILE* sf = fopen(kSoundReportPath, "w")) {
		fprintf(sf, "dsp: %s (ndspInit 0x%08X; 0xD880A7FA means sdmc:/3ds/dspfirm.cdc is missing)\n", gAudio.isOpen() ? "open" : "NOT open",
		        gAudio.openResult());
		fprintf(sf, "sound engine: %s; music: %s\n", app.sound().ready() ? "ready" : "OFF", app.music().ready() ? "ready" : "OFF (no music.pak or no DSP)");
		app.sound().writeReport(sf);
		app.music().writeReport(sf);
		app.speech().writeReport(sf);
		if (app.chatter().ready())
			fprintf(sf, "world voices: played %u, lines without a recording %u (quarrels, greetings, talk between characters)\n",
			        unsigned(app.chatter().played()), unsigned(app.chatter().missing()));
		fclose(sf);
	}
	app.shutdown();
	gAudio.close();
	exitStep("exit 3/4: closing the graphics");
	gfx::shutdown();
	// The CPU mode is the whole system's, not this program's: the launcher
	// that comes back and the HOME Menu behind it must not inherit it.
	osSetSpeedupEnable(false);
	exitStep("exit 4/4: done");
	gfxExit();
	return 0;
}
