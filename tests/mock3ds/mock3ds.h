// What the stand-in libctru/citro3d observed (see 3ds.h in this directory).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace mock3ds {

struct Counters {
	// Rule violations; all must stay 0.
	unsigned wroteBusyBuffer;     // a command was written into the buffer the GPU is executing
	unsigned wroteOutside;        // a command was written without a buffer, or past its end
	unsigned freedInUse;          // memory freed while a submitted, unfinished frame referred to it
	unsigned submittedFreed;      // a frame was submitted that refers to memory already freed
	unsigned submittedBusy;       // a frame was submitted while the GPU was still executing one
	unsigned clearedBusy;         // the render target was cleared while the GPU was drawing into it
	unsigned badFree;             // linearFree of something that is not allocated
	unsigned drawOutsideFrame;    // a draw call with no command buffer set
	unsigned submittedNotStart;   // a command list that does not begin at the start of its buffer
	unsigned submittedUncleared;  // a frame was submitted without clearing the target first
	// Plain counts.
	unsigned framesSubmitted;     // command lists handed to the GPU
	unsigned framesShown;         // render target copied to the screen
	unsigned draws;
	unsigned waits;               // C3D_FrameBegin calls (each waits for the GPU)
	unsigned ownBufferFrames;     // lists submitted from a buffer other than citro3d's own
	unsigned distinctBuffers;     // different buffers lists were submitted from
	unsigned linearBlocks;        // live linearAlloc blocks
	size_t linearBytes;
	// How far ahead of the display the CPU got: draws recorded while the GPU
	// was busy with the previous frame.
	unsigned drawsWhileBusy;
};

Counters& counters();
unsigned violations();
void reset();
// Mock time: every wait for the display advances it by one refresh.
double nowMs();
// Makes linearAlloc fail once `bytes` are allocated (0 = never).
void setLinearLimit(size_t bytes);

} // namespace mock3ds
