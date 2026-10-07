// The console's frame order, checked on the PC.
//
// platform/3ds/gfx_c3d.cpp is compiled here, unchanged, against a stand-in
// for libctru and citro3d (tests/mock3ds). The stand-in draws nothing; it
// keeps the books a GPU keeps: which command buffer it is executing, which
// vertex, index and texture memory that buffer points at, and when it is
// done. Breaking those rules on the console means garbage on the screen or a
// crash, and no emulator on this PC would show it. Here every breach is
// counted.
//
// Both frame orders run the same flight through the test worlds, with chunks
// streaming in and out, world switches and a full video memory:
//  * citro3d's own order (wait for the GPU, then issue the draw calls), and
//  * the overlapped order (issue the next frame's draw calls into a second
//    buffer while the GPU executes the previous frame).
// usage: test_frames <scratch dir>
#include "app.h"
#include "bake.h"
#include "gfx_c3d.h"
#include "importers.h"
#include "mock3ds.h"
#include "pak_writer.h"

#include <citro3d.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace g3;

static int gFailures = 0, gChecks = 0;

#define CHECK(cond)                                                              \
	do {                                                                         \
		++gChecks;                                                               \
		if (!(cond)) {                                                           \
			++gFailures;                                                         \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
		}                                                                        \
	} while (0)

static void printViolations(const char* when) {
	const mock3ds::Counters& c = mock3ds::counters();
	std::printf("  violations %s: busy buffer written %u, written outside %u, freed in use %u, submitted freed %u,\n"
	            "    submitted while busy %u, cleared while busy %u, bad free %u, draw without buffer %u,\n"
	            "    list not at buffer start %u, submitted uncleared %u\n",
	            when, c.wroteBusyBuffer, c.wroteOutside, c.freedInUse, c.submittedFreed, c.submittedBusy, c.clearedBusy,
	            c.badFree, c.drawOutsideFrame, c.submittedNotStart, c.submittedUncleared);
}

// The checker itself: each rule must be noticed when it is broken on purpose.
static void testMockNotices() {
	mock3ds::reset();
	CHECK(C3D_Init(0x4000));
	C3D_RenderTarget* target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, 0);
	void* vertices = linearAlloc(256);
	u16* indices = static_cast<u16*>(linearAlloc(64));
	C3D_BufInfo buf;
	BufInfo_Init(&buf);
	BufInfo_Add(&buf, vertices, 16, 3, 0x210);

	// A correct frame.
	CHECK(C3D_FrameBegin(C3D_FRAME_SYNCDRAW));
	C3D_RenderTargetClear(target, C3D_CLEAR_ALL, 0, 0);
	C3D_FrameDrawOn(target);
	C3D_SetBufInfo(&buf);
	C3D_DrawElements(GPU_TRIANGLES, 3, C3D_UNSIGNED_SHORT, indices);
	u32* const list = gpuCmdBuf;
	C3D_FrameEnd(0);
	CHECK(mock3ds::violations() == 0 && mock3ds::counters().framesSubmitted == 1 && mock3ds::counters().framesShown == 1);

	// Writing the next frame into the buffer the GPU is executing.
	GPUCMD_SetBuffer(list, 0x1000, 0);
	C3D_DrawElements(GPU_TRIANGLES, 3, C3D_UNSIGNED_SHORT, indices);
	CHECK(mock3ds::counters().wroteBusyBuffer == 1);
	// Clearing the target the GPU is drawing into.
	C3D_RenderTargetClear(target, C3D_CLEAR_ALL, 0, 0);
	CHECK(mock3ds::counters().clearedBusy == 1);
	// Freeing memory the executing list reads (and the list just written
	// refers to it as well: two breaches).
	linearFree(vertices);
	CHECK(mock3ds::counters().freedInUse == 2);
	// A draw call with no buffer at all.
	GPUCMD_SetBuffer(nullptr, 0, 0);
	C3D_DrawElements(GPU_TRIANGLES, 3, C3D_UNSIGNED_SHORT, indices);
	CHECK(mock3ds::counters().drawOutsideFrame == 1);
	// A draw call that points at freed memory.
	CHECK(C3D_FrameBegin(0));
	C3D_RenderTargetClear(target, C3D_CLEAR_ALL, 0, 0);
	C3D_SetBufInfo(&buf);
	C3D_DrawElements(GPU_TRIANGLES, 3, C3D_UNSIGNED_SHORT, indices);
	CHECK(mock3ds::counters().submittedFreed == 1);
	C3D_FrameEnd(0);
	CHECK(mock3ds::counters().submittedUncleared == 0);
	// Freeing twice.
	linearFree(vertices);
	CHECK(mock3ds::counters().badFree == 1);
	// A frame nobody cleared the target for, submitted while the GPU is busy.
	void* more = linearAlloc(256);
	BufInfo_Init(&buf);
	BufInfo_Add(&buf, more, 16, 3, 0x210);
	u32* second = static_cast<u32*>(linearAlloc(0x1000));
	GPUCMD_SetBuffer(second, 0x400, 0);
	C3D_SetBufInfo(&buf);
	C3D_DrawElements(GPU_TRIANGLES, 3, C3D_UNSIGNED_SHORT, indices);
	const u32 recorded = gpuCmdBufOffset;
	CHECK(C3D_FrameBegin(0)); // (this is the wait: the GPU is idle again)
	GPUCMD_SetBuffer(second, 0x400, recorded);
	C3D_FrameEnd(0);
	CHECK(mock3ds::counters().submittedUncleared == 1 && mock3ds::counters().submittedBusy == 0);
	// A list that does not start where its buffer starts.
	CHECK(C3D_FrameBegin(0));
	C3D_RenderTargetClear(target, C3D_CLEAR_ALL, 0, 0);
	GPUCMD_SetBuffer(second + 0x200, 0x200, 0);
	C3D_DrawElements(GPU_TRIANGLES, 3, C3D_UNSIGNED_SHORT, indices);
	C3D_FrameEnd(0);
	CHECK(mock3ds::counters().submittedNotStart == 1);
	// Running past the end of the buffer.
	CHECK(C3D_FrameBegin(0));
	GPUCMD_SetBuffer(gpuCmdBuf, 8, 0);
	C3D_DrawElements(GPU_TRIANGLES, 3, C3D_UNSIGNED_SHORT, indices);
	CHECK(mock3ds::counters().wroteOutside == 1);
	C3D_FrameEnd(0);
	CHECK(C3D_FrameBegin(0)); // wait for the GPU before freeing what it read
	C3D_FrameEnd(0);
	linearFree(more);
	linearFree(second);

	C3D_RenderTargetDelete(target);
	linearFree(indices);
	C3D_Fini();
	CHECK(mock3ds::counters().linearBlocks == 0);
}

static void flight(const std::string& pak, bool classic, bool worker) {
	mock3ds::reset();
	gfx::c3dSetClassicFrames(classic);
	CHECK(gfx::init());
	App app;
	app.world().config().background = true;
	app.world().config().worker = worker;
	app.world().config().maxLoadsPerFrame = 1;
	CHECK(app.init(pak.c_str(), nullptr));
	app.setDrawDistance(6000.0f);
	app.world().preload(app.camera().pos);
	app.player().flying = true;
	const Vec3 home = app.camera().pos;

	const float dt = 1.0f / 60.0f;
	unsigned rendered = 0;
	bool clean = true;
	auto frame = [&](const Input& in) {
		const double before = mock3ds::nowMs();
		app.update(in, dt);
		app.render();
		++rendered;
		const gfx::FrameStats fs = gfx::frameStats();
		CHECK(fs.overlapped == !classic && fs.drawsDropped == 0);
		// Exactly one wait for the display per frame, and the frame time is
		// measured from display to display.
		CHECK(std::fabs(mock3ds::nowMs() - before - 1000.0 / 60.0) < 1e-6);
		if (rendered > 1) CHECK(std::fabs(double(fs.frameMs) - 1000.0 / 60.0) < 1e-3);
		if (clean && mock3ds::violations() != 0) {
			clean = false;
			char when[64];
			std::snprintf(when, sizeof(when), "at frame %u", rendered);
			printViolations(when);
		}
	};

	// 1. A flight across chunk borders: meshes and textures come and go.
	for (int f = 0; f < 240; ++f) {
		Input in;
		in.moveZ = 1.0f;
		in.fast = true;
		in.lookX = (f / 50) % 2 ? 0.3f : -0.2f;
		if (f % 12 == 5) in.timeStep = 0.5f; // the sky dome is rebuilt inside the frame
		frame(in);
	}
	CHECK(app.world().stats().loadFailures == 0);
	// 2. Everything dropped at once, and back again, several times.
	for (int round = 0; round < 6; ++round) {
		app.teleport({1e6f, 0, 1e6f});
		for (int f = 0; f < 3; ++f) frame(Input {});
		app.teleport(home);
		for (int f = 0; f < 25; ++f) frame(Input {});
	}
	// 3. World switches in the middle of it all (they free a whole world and
	// wait for the GPU outside a frame).
	for (int hop = 0; hop < 5; ++hop) {
		Input in;
		in.nextWorld = true;
		frame(in);
		Input fly;
		fly.moveZ = 1.0f;
		fly.fast = true;
		for (int f = 0; f < 30; ++f) frame(fly);
	}
	CHECK(app.worldSwitches() == 5);
	// 4. Video memory full: allocation fails, the backend frees what the GPU
	// has finished with and tries again.
	app.teleport({1e6f, 0, 1e6f});
	for (int f = 0; f < 3; ++f) frame(Input {});
	mock3ds::setLinearLimit(mock3ds::counters().linearBytes + 48 * 1024);
	app.teleport(app.world().indoor() ? Vec3 {0, 175.0f, 900.0f} : home);
	for (int f = 0; f < 80; ++f) frame(Input {});
	mock3ds::setLinearLimit(0);
	for (int f = 0; f < 200; ++f) frame(Input {});
	CHECK(app.world().stats().chunksLoaded > 0 && app.world().stats().loadFailures == 0);

	const mock3ds::Counters c = mock3ds::counters();
	std::printf("  %-10s %-9s %u frames, %u draw calls (%u issued while the GPU was busy), %u waits, %u buffer(s)\n",
	            classic ? "classic" : "overlapped", worker ? "+ thread" : "", rendered, c.draws, c.drawsWhileBusy, c.waits,
	            c.distinctBuffers);
	CHECK(mock3ds::violations() == 0);
	if (mock3ds::violations() != 0 && clean) printViolations("at the end");
	// Every frame reaches the screen; the few without a command list are the
	// ones with nothing in view (far away from an indoor world: not even sky).
	// (With a worker thread the frames of this test outrun the loader - they
	// take no time here - so more of them are empty.)
	CHECK(c.framesShown == rendered && c.framesSubmitted <= rendered);
	CHECK(c.framesSubmitted > rendered * (worker ? 5 : 9) / 10 && c.draws > rendered * (worker ? 5 : 10));
	if (classic) {
		// citro3d's order: one buffer, and the GPU is always idle while the
		// CPU issues draw calls.
		CHECK(c.distinctBuffers == 1 && c.ownBufferFrames == 0 && c.drawsWhileBusy == 0);
	} else {
		// Overlapped: two buffers taking turns, and nearly every draw call
		// issued while the GPU was still executing the frame before.
		CHECK(c.distinctBuffers == 2 && c.ownBufferFrames == c.framesSubmitted);
		CHECK(c.drawsWhileBusy > c.draws * 9 / 10);
	}

	app.shutdown();
	gfx::shutdown();
	CHECK(mock3ds::violations() == 0);
	CHECK(mock3ds::counters().linearBlocks == 0); // nothing left in the linear heap
}

int main(int argc, char** argv) {
	const std::string dir = argc > 1 ? argv[1] : ".";

	std::vector<BakeWorld> worlds(2);
	worlds[0].name = "world";
	worlds[0].level = "WORLD.ZEN";
	SynthOptions so;
	so.cells = 48;
	so.huts = 8;
	so.trees = 60;
	buildSyntheticScene(so, worlds[0].scene);
	worlds[1].name = "cave";
	worlds[1].level = "CAVE.ZEN";
	buildSyntheticCave(worlds[1].scene);
	BakeOptions bo;
	bo.chunker.chunkSize = 3000.0f;
	bo.tex.maxSize = 32;
	bo.tex.quality = 0;
	bo.threads = 2;
	bo.quiet = true;
	PakWriter writer;
	bakeWorlds(worlds, bo, writer);
	const std::string pak = dir + "/frames.pak";
	writer.write(pak);

	std::printf("[ RUN ] the checker notices broken rules\n");
	testMockNotices();
	std::printf("[ RUN ] frame orders\n");
	flight(pak, true, false);
	flight(pak, false, false);
	flight(pak, false, true);
	flight(pak, true, true);
	std::printf("%d checks, %d failed\n", gChecks, gFailures);
	return gFailures ? 1 : 0;
}
