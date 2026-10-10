// Entry point for the 3DS build. Top screen: the world. Bottom screen: a
// text console with the numbers that matter for the feasibility check
// (frame time split into CPU and GPU, triangles, streaming, memory).
//
// Controls, as in the original game - the character is steered, the view
// follows it:
//   Circle Pad    up: run (a slight push: walk), down: hop back, left/right: turn
//   C-Stick       left/right: turn (faster), up/down: tilt the view
//   L / R         step left / right
//   B             jump          X  walk <-> run          Y  third <-> first person
//   D-Pad         left/right: draw distance, down: time of day, up: next world (debug)
//   SELECT        fly mode (a free camera: Circle Pad move, C-Stick or A/B/X/Y look,
//                 L/R down/up, ZL or ZR fast)
//   START         exit
// An archive without a character is walked with the free camera, too (R jumps).
// The position is saved on exit and restored on the next start; hold L while
// starting to begin at the world's start point instead.
//
// Switches for comparing on the console, held while the program starts:
//   B  load chunks on the frame's own thread, as before (no background loading)
//   X  no frame pacer (fixed object distances, never 30 fps)
//   Y  citro3d's own frame order: the GPU waits while the CPU issues the draws
//   A  first person from the start (the character is not drawn until Y is pressed)
//   R  the vertex shader of the versions before characters; no character at all
//   SELECT   the inhabitants are not drawn (the scripts run all the same)
//   D-Pad Up the inhabitants only stand where their routine has them: no
//            states, no walking - as in the version before they lived
//   ZL       whole chunks are drawn, as before they were cut into clusters
//            (what the clusters save shows in "tris" and "gpu")
//   D-Pad Down  steady objects: small and medium objects are drawn farther
//            out (60 and 100 m instead of 35 and 70) and the pacer never
//            takes them away - it goes to 30 fps instead when the frame is
//            too heavy; nobody comes and goes with the load either
//   D-Pad Left  nobody perceives anything (the inhabitants do not notice the
//            player or each other), as before perception
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
	const bool steady = (startKeys & KEY_DDOWN) != 0;   // objects drawn farther, never taken away by the pacer
	const bool blind = (startKeys & KEY_DLEFT) != 0;    // no perception
	gfx::c3dSetClassicFrames(classic);
	gfx::c3dSetPlainShader(plainShader);

	App& app = gApp;
	// Chunks are read, unpacked and given their collision grids on a worker
	// thread; the frame only copies the finished mesh into video memory.
	g3::StreamConfig& stream = app.world().config();
	stream.background = background;
	stream.maxLoadsPerFrame = background ? 1 : 2;
	stream.clusterCulling = !wholeChunks;
	if (steady) {
		stream.detailDistance[g3::DETAIL_MEDIUM] = 10000.0f;
		stream.detailDistance[g3::DETAIL_SMALL] = 6000.0f;
	}
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
	float drawDistance = 12000.0f;
	app.setDrawDistance(drawDistance);
	// Continue where the last session ended, unless L is held at start.
	if (!(startKeys & KEY_L) && app.loadState(kStatePath)) drawDistance = app.drawDistance();
	if ((startKeys & KEY_A) || plainShader) app.setThirdPerson(false);
	if (nobody || plainShader) app.npcs().config().maxShown = 0;
	app.npcs().config().alive = !standing;
	app.npcs().config().perceive = !blind;
	printf("loading...\n");
	app.world().preload(app.camera().pos);
	consoleClear();

	u32 frame = 0;
	float msAccum = 0.0f, msWorst = 0.0f, updateAccum = 0.0f, updateWorst = 0.0f;
	u32 lateFrames = 0; // frames of this report that missed their refresh
	u32 lastPerceived = 0;
	g3::FramePacer pacer;
	if (steady) pacer.cfg.minDetail = 1.0f; // (too heavy: 30 fps, the objects stay)
	int interval = 1;
	int reportLines = 0;
	u32 lastStateCalls = 0;
	g3::UpdateTimes partSum;               // where update() spent its time, summed over a report
	u32 collQueries = 0, collTests = 0;    // and what its collision queries looked at

	while (aptMainLoop()) {
		hidScanInput();
		const u32 down = hidKeysDown(), held = hidKeysHeld();
		if (down & KEY_START) break;

		// The step of the simulation is the time between the last two frames
		// reaching the display: as even as the display itself, whatever the
		// work in between took.
		const gfx::FrameStats timing = gfx::frameStats();
		const float ms = timing.frameMs > 0.0f ? timing.frameMs : 16.7f * float(interval);
		const float dt = clamp(ms * 0.001f, 0.001f, 0.1f);

		circlePosition pad, cstick;
		hidCircleRead(&pad);
		hidCstickRead(&cstick);

		Input in;
		in.moveX = stick(pad.dx);
		in.moveZ = stick(pad.dy);
		in.lookX = stick(cstick.dx);
		in.lookY = stick(cstick.dy);
		if (app.steersHero()) {
			in.strafe = (held & KEY_R ? 1.0f : 0.0f) - (held & KEY_L ? 1.0f : 0.0f);
			in.jump = (held & KEY_B) != 0;
			in.toggleWalk = (down & KEY_X) != 0;
			in.toggleCamera = (down & KEY_Y) != 0 && !plainShader;
		} else {
			if (held & KEY_A) in.lookX = 1.0f;
			if (held & KEY_Y) in.lookX = -1.0f;
			if (held & KEY_X) in.lookY = 1.0f;
			if (held & KEY_B) in.lookY = -1.0f;
			in.moveY = (held & KEY_R ? 1.0f : 0.0f) - (held & KEY_L ? 1.0f : 0.0f);
			in.jump = (held & KEY_R) != 0;
			in.fast = (held & (KEY_ZL | KEY_ZR)) != 0;
		}
		in.toggleFly = (down & KEY_SELECT) != 0;
		in.nextWorld = (down & KEY_DUP) != 0;
		if (down & KEY_DDOWN) in.timeStep = 1.0f;

		if (down & (KEY_DLEFT | KEY_DRIGHT)) {
			drawDistance = clamp(drawDistance + ((down & KEY_DRIGHT) ? 2000.0f : -2000.0f), 4000.0f, 30000.0f);
			app.setDrawDistance(drawDistance);
		}

		TickCounter updateClock;
		osTickCounterStart(&updateClock);
		app.update(in, dt);
		osTickCounterUpdate(&updateClock);
		const float updateMs = float(osTickCounterRead(&updateClock));
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

		app.render();

		// What the frame cost decides how much detail the next one gets, and
		// whether the display runs at 60 or a steady 30. With overlapped
		// frames the CPU and the GPU work at the same time and the slower of
		// the two sets the pace; in citro3d's own order the GPU waits for the
		// CPU, and a frame takes the sum. (Measured in that order on a New
		// 3DS: 28% + 97% gave 20.9 ms, which is 125% of 16.7 ms.)
		const gfx::FrameStats fs = gfx::frameStats();
		const float load = (fs.overlapped ? max(fs.cpuMs + updateMs, fs.gpuMs) : fs.cpuMs + fs.gpuMs) * 0.06f;
		// (A world switch or any other pause is not a frame to learn from.)
		if (pacing && updateMs < 50.0f) {
			pacer.update(load);
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
			const char* state = pl.flying     ? "flying (SELECT: walk)"
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
			reportLine("detail %3.0f%%  %2d fps  late %2lu/30", double(app.world().config().detailScale * 100.0f),
			     interval == 2 ? 30 : 60, static_cast<unsigned long>(lateFrames));
			reportLine("frames %s  pacer %s%s", fs.overlapped ? "overlapped" : "classic", pacing ? "on" : "off", steady ? " STEADY" : "");
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
			reportLine("mesh %5.2f MB  tex %5.2f MB", double(mem.meshBytes) / 1048576.0, double(mem.textureBytes) / 1048576.0);
			reportLine("collision %5.0f KB", double(ws.collisionBytes) / 1024.0);
			reportLine("linear free %5.1f MB", double(linearSpaceFree()) / 1048576.0);
			reportLine("budget rejects %lu  failures %lu", static_cast<unsigned long>(ws.budgetRejects),
			     static_cast<unsigned long>(ws.loadFailures));
			reportLine("draw distance %3.0f m  time %02d:%02d", double(drawDistance / 100.0f), int(app.environment().timeOfDay),
			     int(app.environment().timeOfDay * 60.0f) % 60);
			reportLine("%-22.22s %-16.16s", state, app.worldName());
			// The player's eyes, wherever the camera hangs behind them.
			const g3::Vec3 eye = pl.eye();
			reportLine("pos %7.0f %6.0f %7.0f", double(eye.x), double(eye.y), double(eye.z));
			const g3::Hero& hero = app.hero();
			if (plainShader) {
				reportLine("hero off: plain shader (R at start)");
			} else if (hero.loaded()) {
				static const char* const kRoles[g3::Hero::ROLE_COUNT] = {"stand", "run", "walk", "back", "step",
				                                                         "turn",  "jump", "fall", "swim", "float"};
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
				reportLine("npc tris %5lu mdl %2lu wait %lu none %lu", static_cast<unsigned long>(ns.triangles),
				     static_cast<unsigned long>(ns.models), static_cast<unsigned long>(ns.waiting),
				     static_cast<unsigned long>(ns.noModel));
				// Their life: how many live and walk, animations in memory, state
				// functions called a second, ways not found + characters put at their goal.
				// ("use": beds, benches and the like in use; in "x" also the
				// times somebody had no animation for one.)
				reportLine("life %2lu wlk %lu use %lu ani %3luK st %2lu/s x%lu%s", static_cast<unsigned long>(ns.actors),
				     static_cast<unsigned long>(ns.walking), static_cast<unsigned long>(ns.mobsInUse),
				     static_cast<unsigned long>(ns.clipBytes / 1024), static_cast<unsigned long>((ns.stateCalls - lastStateCalls) * 2),
				     static_cast<unsigned long>(ns.noPath + ns.snapped + ns.clipsRefused + ns.mobsFailed), standing ? " OFF" : "");
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
				// somebody, fights the scripts wanted (there are none yet), and
				// what the nearest said last.
				const g3::NpcActor* near = npcs.nearest();
				reportLine("perc %2lu/s look %lu fight %lu%s %-12.12s", static_cast<unsigned long>((ns.perceived - lastPerceived) * 2),
				     static_cast<unsigned long>(ns.looking), static_cast<unsigned long>(ns.fightsRefused), blind ? " OFF" : "",
				     near && near->said[0] ? near->said : "");
				lastPerceived = ns.perceived;
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
		}
		// One line of the report per frame, padded to overwrite the old one.
		const int row = int(frame % 30);
		if (row < reportLines) printf("\x1b[%d;1H%-39.39s", row + 1, gReport[row]);
	}

	// Leaving. Each step says so on the bottom screen first: should the
	// console ever hang here, the last line tells where.
	exitStep("exit 1/4: saving the position");
	app.saveState(kStatePath);
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
