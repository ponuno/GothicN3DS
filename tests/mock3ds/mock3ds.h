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
	unsigned outputWidth, outputScaling; // the linked target's width and the scaling bits of its transfer
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
	unsigned texUnit1Binds;       // C3D_TexBind on unit 1 (only the dissolve uses it)
	unsigned programSwitches;     // C3D_BindProgram that changed the entry point
	unsigned framesShownRight;    // right-eye target copied to the screen
	unsigned switches3D;          // gfxSet3D calls that changed the mode
	unsigned rightDraws;          // draw calls recorded while the right target was the one drawn into
};

Counters& counters();
unsigned violations();
void reset();
// Mock time: every wait for the display advances it by one refresh.
double nowMs();
// Makes linearAlloc fail once `bytes` are allocated (0 = never).
void setLinearLimit(size_t bytes);

// ---- the vertex shader ----
//
// DVLB_ParseFile reads the shader binary for real, uniform locations come
// from its tables, and the uniform setters keep their values. A draw call
// can then be shaded on the PC: its vertices run through the bytecode, one
// instruction after the other, as the PICA200's vertex processor would.

// A draw call as the GPU would see it.
struct DrawCall {
	const void* vertices; // the bound vertex buffer
	unsigned stride;
	const uint16_t* indices;
	unsigned count;
	float uniforms[96][4]; // float registers c0..c95: x y z w
	uint16_t bools;        // bool registers b0..b15
	int entry;             // the DVLE (entry point of the shader binary) of the program bound for the draw
};
typedef void (*DrawHook)(const DrawCall& call, void* user);
// Called for every C3D_DrawElements issued inside a frame.
void setDrawHook(DrawHook hook, void* user);

struct ShadedVertex {
	float position[4]; // clip space, as the shader leaves it
	float color[4];
	float texcoord[2];
	float texcoord1[2];
};
// Runs the bound shader on vertex `index` of the call's buffer. False if the
// bytecode does something the interpreter refuses: an unknown instruction, a
// register outside the uniform space, an endless loop. `gpuFloats` rounds
// every uniform and every intermediate result to the 24-bit floats the
// PICA200 computes with (1 sign, 7 exponent, 16 mantissa bits).
bool shadeVertex(const DrawCall& call, unsigned index, ShadedVertex& out, bool gpuFloats = false);
// What is wrong with the shader binary or its use, or "" if nothing.
const char* shaderProblem();
// Float registers the shader's uniforms and constants occupy (of 96).
unsigned shaderFloatRegisters();
unsigned shaderInstructions();
// The DVLEs of the binary (entry points of the one code blob), and the
// instructions of one of them.
unsigned shaderEntries();
unsigned shaderEntryInstructions(unsigned entry);

} // namespace mock3ds
