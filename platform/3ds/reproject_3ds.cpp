#include "reproject_3ds.h"

#include "reproject.h"

#include <3ds.h>

#include <string.h>

namespace g3 {
namespace reproject {

namespace {

constexpr u32 kColumnBytes = 240u * 3u;           // the top screen is 240 pixels tall, 3 bytes each (BGR8), stored by column
constexpr u32 kImageColumns = 400u;
constexpr u32 kBufferColumns = 800u;              // libctru allocates room for the "wide" mode
constexpr u32 kBufferBytes = kColumnBytes * kBufferColumns;
constexpr u32 kPadColumns = 32u;                  // black in front of the first buffer; more than the longest move
constexpr u32 kPadBytes = kColumnBytes * kPadColumns;
constexpr u32 kRing = 8u;

u8* gPad = nullptr;
u8* gBuf[2] = {nullptr, nullptr};                 // libctru's top framebuffers by their index; [1] is known at the start, [0] is found
u8* gLastBack = nullptr;
int gLeftRoom[2] = {0, 0}, gRightRoom[2] = {0, 0};
bool gPrepared = false;

volatile bool gEnabled = false, gPaused = false, gQuit = false, gRunning = false;
u32 gSubmitted = 0;                               // pictures handed to the GPU (main thread writes, ours reads)
u32 gRing[kRing];                                 // their yaw (float bits)
u32 gFovBits = 0;
u32 gArrivals = 0;                                // pictures seen on the screen, counted in step with gSubmitted

YawReprojector gR;
bool gShifted = false;                            // the display is told to start away from the picture's own address
Stats gStats = {};
Thread gThread = nullptr;

float bitsToFloat(u32 b) {
	float f;
	memcpy(&f, &b, sizeof(f));
	return f;
}

u32 floatToBits(float f) {
	u32 b;
	memcpy(&b, &f, sizeof(b));
	return b;
}

// What libctru's own present of a top framebuffer in 2D mode from linear memory says (gfx.c: gfxPresentFramebuffer);
// the same buffer, the same index, only another start address.
void present(unsigned index, const u8* start) {
	const u32 mode = u32(gfxGetScreenFormat(GFX_TOP)) | BIT(6) | (1u << 8);
	gspPresentBuffer(GFX_TOP, index, start, start, kColumnBytes, mode);
}

// Both framebuffers are known: the unused halves are made black, the room either side is worked out.
void prepare() {
	for (int b = 0; b < 2; ++b) {
		u8* tail = gBuf[b] + kImageColumns * kColumnBytes;
		memset(tail, 0, kBufferBytes - kImageColumns * kColumnBytes);
		GSPGPU_FlushDataCache(tail, kBufferBytes - kImageColumns * kColumnBytes);
	}
	if (gPad) {
		memset(gPad, 0, kPadBytes);
		GSPGPU_FlushDataCache(gPad, kPadBytes);
	}
	for (int b = 0; b < 2; ++b) {
		const u8* other = gBuf[1 - b];
		gRightRoom[b] = int(kBufferColumns - kImageColumns);
		if (other + kBufferBytes == gBuf[b]) gLeftRoom[b] = int(kBufferColumns - kImageColumns); // the black half of the other buffer
		else if (gPad && gPad + kPadBytes == gBuf[b]) gLeftRoom[b] = int(kPadColumns);
		else gLeftRoom[b] = 0;
	}
	gPrepared = true;
}

void onVBlank() {
	const double t = double(svcGetSystemTick()) / double(SYSCLOCK_ARM11);
	++gStats.vblanks;
	u8* back = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
	if (!gBuf[1] || !back) return;
	if (!gBuf[0] && back != gBuf[1]) {
		gBuf[0] = back;
		prepare();
	}

	// A new picture was put on the screen since the last refresh: libctru turned the buffer it draws into.
	const bool arrived = back != gLastBack;
	if (arrived) {
		gLastBack = back;
		++gStats.arrivals;
		// Pictures come in the order they were handed over: the n-th to arrive is the n-th handed over. (One that
		// arrives before submit() has noted it takes the latest there is, and the count catches up at the next. A swap
		// that none of ours made is dropped from the count when it gets ahead.)
		const u32 submitted = __atomic_load_n(&gSubmitted, __ATOMIC_ACQUIRE);
		++gArrivals;
		if (gArrivals > submitted + 1u) gArrivals = submitted;
		const u32 n = gArrivals < submitted ? gArrivals : submitted;
		if (n >= 1) {
			gR.cfg.fovY = bitsToFloat(__atomic_load_n(&gFovBits, __ATOMIC_RELAXED));
			gR.arrive(bitsToFloat(__atomic_load_n(&gRing[(n - 1u) % kRing], __ATOMIC_RELAXED)), t);
		}
		gShifted = false;
	}
	if (!gPrepared || gPaused) {
		gShifted = false;
		gStats.columns = 0;
		return;
	}

	// The picture on show is the buffer that is not being drawn into.
	const unsigned shownIndex = back == gBuf[0] ? 1u : 0u;
	const u8* shown = gBuf[shownIndex];
	if (!gEnabled) {
		if (gShifted && !gspIsPresentPending(GFX_TOP)) {
			present(shownIndex, shown);
			gShifted = false;
			gStats.columns = 0;
		}
		return;
	}
	// A picture waiting to go on the screen at the next refresh is the real one: it takes the place of whatever is shown.
	const bool real = arrived || gspIsPresentPending(GFX_TOP);
	const ReprojectStep step = gR.onVBlank(real, t, gLeftRoom[shownIndex], gRightRoom[shownIndex]);
	if (!step.present || real) return;
	present(shownIndex, shown + int(kColumnBytes) * step.columns);
	gShifted = step.columns != 0;
	gStats.columns = step.columns;
	if (step.columns != 0) ++gStats.moved;
}

void threadMain(void*) {
	while (!gQuit) {
		gspWaitForEvent(GSPGPU_EVENT_VBlank0, true);
		if (gQuit) break;
		onVBlank();
	}
}

} // namespace

void reserve() {
	if (!gPad) gPad = static_cast<u8*>(linearAlloc(kPadBytes));
}

void noteInit() {
	gBuf[1] = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr); // (libctru starts with buffer 0 on show, 1 to draw into)
	gBuf[0] = nullptr;
	gLastBack = gBuf[1];
	gPrepared = false;
}

bool start() {
	if (gRunning) return true;
	if (!gBuf[1] || gfxIs3D() || gfxIsWide() || gfxGetScreenFormat(GFX_TOP) != GSP_BGR8_OES) return false;
	gQuit = false;
	gSubmitted = 0;
	gArrivals = 0;
	gR.reset();
	gShifted = false;
	gStats = {};
	s32 priority = 0x30;
	svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
	// Above the game's own thread: it wakes at the refresh while the game is in the middle of a long step, which is
	// what it is for.
	if (priority > 0x19) --priority;
	gThread = threadCreate(threadMain, nullptr, 16 * 1024, priority, -2, false);
	gRunning = gThread != nullptr;
	return gRunning;
}

void stop() {
	if (!gRunning) return;
	gQuit = true;
	gEnabled = false;
	// (It wakes at the next refresh; if the screen is not delivering any, it is left to the end of the program.)
	if (R_SUCCEEDED(threadJoin(gThread, 500000000ULL))) threadFree(gThread);
	gThread = nullptr;
	gRunning = false;
	if (gPrepared && gBuf[1]) {
		const u8* back = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
		if (gShifted && back) present(back == gBuf[0] ? 1u : 0u, back == gBuf[0] ? gBuf[1] : gBuf[0]);
	}
	gShifted = false;
}

void enable(bool on) { gEnabled = on; }

void pause(bool on) { gPaused = on; }

void submit(float yaw, float fovY) {
	const u32 n = __atomic_load_n(&gSubmitted, __ATOMIC_RELAXED);
	__atomic_store_n(&gRing[n % kRing], floatToBits(yaw), __ATOMIC_RELAXED);
	__atomic_store_n(&gFovBits, floatToBits(fovY), __ATOMIC_RELAXED);
	__atomic_store_n(&gSubmitted, n + 1u, __ATOMIC_RELEASE);
}

Stats stats() {
	Stats s = gStats;
	s.running = gRunning && gPrepared;
	s.enabled = gEnabled;
	s.leftRoom = gLeftRoom[0] < gLeftRoom[1] ? gLeftRoom[0] : gLeftRoom[1];
	s.rightRoom = gRightRoom[0] < gRightRoom[1] ? gRightRoom[0] : gRightRoom[1];
	return s;
}

} // namespace reproject
} // namespace g3
