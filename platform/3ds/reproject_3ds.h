// The console's half of the re-projection (engine/reproject.h has the maths): a thread that wakes at every vertical
// blank of the top screen and, when no new picture has come since the last one, tells the display to start reading the
// old picture a few columns on. The picture is not touched, the GPU is not asked for anything: the framebuffer is
// stored column by column (720 bytes each), so "moved sideways" is only a different start address.
//
// What it needs of the memory around the two framebuffers (libctru allocates them with room for 800 columns, the screen
// uses 400): the right-hand 400 are free, and are made black here, so a move to the left shows a black strip at the
// right edge. For a move the other way the strip comes from the memory in front of the buffer: reserve() leaves a
// black pad there before libctru allocates (the first buffer follows it), and the second buffer follows the first, so
// the unused half of that one is in front of it. Where the memory is not found to be where this expects it, that
// direction is not used.
#pragma once
#include "g3ds/base.h"

namespace g3 {
namespace reproject {

struct Stats {
	bool running;         // the thread is going and the framebuffers were found as expected
	bool enabled;
	g3::u32 vblanks;      // refreshes looked at
	g3::u32 arrivals;     // pictures that came
	g3::u32 moved;        // refreshes that showed the old picture moved
	int columns;          // the last move
	int leftRoom, rightRoom; // columns to spare either side (of the picture on show)
};

// Before gfxInitDefault(): sets aside the pad in front of the first framebuffer.
void reserve();
// Right after gfxInitDefault(): remembers the framebuffer being drawn into (the second one, at the start).
void noteInit();
// Starts the thread (it does nothing until enable(true)). False if the framebuffers are not as expected.
bool start();
// Before gfxExit(): stops the thread and puts the display back to the picture's own address.
void stop();
void enable(bool on);
// The HOME Menu has the screen: touch nothing.
void pause(bool on);
// A picture was handed to the GPU for the view turned to `yaw` (radians), field of view `fovY`.
void submit(float yaw, float fovY);
Stats stats();

} // namespace reproject
} // namespace g3
