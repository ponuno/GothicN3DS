// Entry point for the 3DS build. Top screen: the world. Bottom screen: a
// text console with the numbers that matter for the feasibility check
// (frame time split into CPU and GPU, triangles, streaming, memory).
//
// Controls: Circle Pad move, C-Stick or A/B/X/Y look, L/R down/up,
//           ZL or ZR (or D-Pad Up) fast, D-Pad Left/Right draw distance,
//           START exit.
#include "app.h"

#include <3ds.h>
#include <citro3d.h>

#include <stdio.h>

using g3::App;
using g3::clamp;
using g3::Input;
using g3::max;
using g3::WorldStats;
namespace gfx = g3::gfx;

static const char* kPakPath = "sdmc:/3ds/gothic3ds/data.pak";

static float stick(int v) {
	const float dead = 18.0f, full = 140.0f;
	float f = float(v);
	if (f > -dead && f < dead) return 0.0f;
	f = (f > 0 ? f - dead : f + dead) / (full - dead);
	return clamp(f, -1.0f, 1.0f);
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

	App app;
	bool ok = gfx::init();
	if (!ok) printf("gfx init failed\n");
	if (ok && !app.init(kPakPath, "world")) {
		printf("%s\nexpected at %s\n", app.error(), kPakPath);
		ok = false;
	}
	if (!ok) {
		printf("\nPress START to exit.\n");
		waitForStart();
		gfx::shutdown();
		gfxExit();
		return 1;
	}

	float drawDistance = 12000.0f;
	app.setDrawDistance(drawDistance);
	printf("loading...\n");
	app.world().preload(app.camera().pos);
	consoleClear();

	TickCounter clock;
	osTickCounterStart(&clock);
	u32 frame = 0;
	float msAccum = 0.0f, msWorst = 0.0f, updateAccum = 0.0f;

	while (aptMainLoop()) {
		hidScanInput();
		const u32 down = hidKeysDown(), held = hidKeysHeld();
		if (down & KEY_START) break;

		osTickCounterUpdate(&clock);
		const float ms = float(osTickCounterRead(&clock));
		const float dt = clamp(ms * 0.001f, 0.001f, 0.1f);

		circlePosition pad, cstick;
		hidCircleRead(&pad);
		hidCstickRead(&cstick);

		Input in;
		in.moveX = stick(pad.dx);
		in.moveZ = stick(pad.dy);
		in.lookX = stick(cstick.dx);
		in.lookY = stick(cstick.dy);
		if (held & KEY_A) in.lookX = 1.0f;
		if (held & KEY_Y) in.lookX = -1.0f;
		if (held & KEY_X) in.lookY = 1.0f;
		if (held & KEY_B) in.lookY = -1.0f;
		in.moveY = (held & KEY_R ? 1.0f : 0.0f) - (held & KEY_L ? 1.0f : 0.0f);
		in.fast = (held & (KEY_ZL | KEY_ZR | KEY_DUP)) != 0;

		if (down & (KEY_DLEFT | KEY_DRIGHT)) {
			drawDistance = clamp(drawDistance + ((down & KEY_DRIGHT) ? 2000.0f : -2000.0f), 4000.0f, 30000.0f);
			app.setDrawDistance(drawDistance);
		}

		TickCounter updateClock;
		osTickCounterStart(&updateClock);
		app.update(in, dt);
		osTickCounterUpdate(&updateClock);
		updateAccum += float(osTickCounterRead(&updateClock));

		app.render();

		msAccum += ms;
		msWorst = max(msWorst, ms);
		if (++frame % 30 == 0) {
			const WorldStats& ws = app.world().stats();
			const gfx::MemStats mem = gfx::memStats();
			// citro3d reports both as a percentage of a 60 fps frame.
			printf("\x1b[1;1H");
			printf("frame  %5.1f ms avg  %5.1f worst   \n", double(msAccum / 30.0f), double(msWorst));
			printf("cpu %3.0f%%  gpu %3.0f%%  (of 16.7 ms)  \n", double(C3D_GetProcessingTime() * 6.0f),
			       double(C3D_GetDrawingTime() * 6.0f));
			printf("update %5.2f ms (streaming)      \n", double(updateAccum / 30.0f));
			printf("tris %6lu  draws %4lu  chunks %3lu  \n", static_cast<unsigned long>(ws.trianglesDrawn),
			       static_cast<unsigned long>(ws.drawCalls), static_cast<unsigned long>(ws.chunksDrawn));
			printf("resident %3lu/%lu  pending %3lu     \n", static_cast<unsigned long>(ws.chunksLoaded),
			       static_cast<unsigned long>(ws.chunksTotal), static_cast<unsigned long>(ws.chunksPending));
			printf("mesh %5.2f MB  tex %5.2f MB       \n", double(mem.meshBytes) / 1048576.0,
			       double(mem.textureBytes) / 1048576.0);
			printf("linear free %5.1f MB              \n", double(linearSpaceFree()) / 1048576.0);
			printf("budget rejects %lu  failures %lu   \n", static_cast<unsigned long>(ws.budgetRejects),
			       static_cast<unsigned long>(ws.loadFailures));
			printf("draw distance %3.0f m               \n", double(drawDistance / 100.0f));
			printf("pos %7.0f %6.0f %7.0f         \n", double(app.camera().pos.x), double(app.camera().pos.y),
			       double(app.camera().pos.z));
			msAccum = 0.0f;
			msWorst = 0.0f;
			updateAccum = 0.0f;
		}
	}

	app.shutdown();
	gfx::shutdown();
	gfxExit();
	return 0;
}
