// Console-only extras of the citro3d backend.
#pragma once

namespace g3 {
namespace gfx {

// citro3d's own frame order: wait for the display and for the GPU, then issue
// the draw calls. The GPU sits idle while the CPU issues them, so a frame
// takes CPU time plus GPU time. The default here is the other way round
// (issue the draw calls while the GPU is still busy with the previous frame,
// then wait); `true` selects citro3d's order - one frame less input lag, and
// the known-good path should the default ever misbehave. Call before init().
void c3dSetClassicFrames(bool classic);
// Chooses the vertex shader; call before gfx::init(). `plain` = true: the
// world-only shader of the versions before characters. drawSkinned() then
// draws nothing. A fallback should the character branch misbehave on hardware.
void c3dSetPlainShader(bool plain);
// Squeezes the picture into half the screen: half the pixels, the same triangles (for finding out whether the GPU is limited by the pixels).
void c3dSetFillTest(bool on);
// The sun's shadow left out of the drawing altogether (its texture stages never set): for finding faults (C-Stick up at start).
void c3dSetNoSun(bool on);
// For finding faults (debug.txt): materials with any of `skipFlags` (MATF_*) are not drawn; `program` 1 draws the world with the fast
// entry only, 2 with the full one only, 0 as usual.
void c3dSetDebugDraw(u16 skipFlags, int program);
// fix144, the sun's mask (debug.txt words sunl4 sunkeep sunwhite sunnotex sunnearest sunrepeat). Call before init().
enum SunDebug : unsigned {
	SUN_DBG_L4 = 1u << 0,      // masks stay GPU_L4 as baked (by default they become GPU_A8, the noise's format, at load)
	SUN_DBG_KEEP = 1u << 1,    // a mask stays on unit 1 after its chunk (as fix143); by default the noise/white goes back
	SUN_DBG_WHITE = 1u << 2,   // the white 8x8 is bound in place of every mask (sampling, no mask data)
	SUN_DBG_NOTEX = 1u << 3,   // the sun's entry is used, but no stage reads unit 1 and no mask is bound
	SUN_DBG_NEAREST = 1u << 4, // masks filtered NEAREST (not LINEAR)
	SUN_DBG_REPEAT = 1u << 5,  // masks wrapped REPEAT (not CLAMP_TO_EDGE)
};
void c3dSetSunDebug(unsigned flags);
// Smooths the edges: the picture is drawn twice the size each way (four times the pixels) and shrunk by the
// display-transfer unit as it goes to the screen. Call before init(); if the bigger buffers do not fit into VRAM
// the picture stays as it was (c3dSupersample() says which).
void c3dSetSupersample(bool on);
// 0 off, 1 light (480x400, averaged in pairs: twice the pixels), 2 full (480x800, 2x2: four times). Before init() only.
void c3dSetSupersampleMode(int mode);
bool c3dSupersampleLight();
bool c3dSupersample();
// Stereo 3D: the scene is drawn twice a frame, once for each eye (gfx::stereoSet switches it on and off while the
// game runs, with the 3D slider). Call before init(): the right eye's target and bigger command buffers are made there.
// Not together with the smoothing (gfx::stereoAvailable() says whether it is there).
void c3dSetStereo(bool on);
// The wide top screen: 800x240 (the picture twice as fine across; the interface is drawn the size it was). Call before
// init(). Rules out stereo 3D; with the smoothing the picture is smoothed up and down only. c3dWide() says whether the
// screen is wide (a console without the wide mode stays as it was).
void c3dSetWide(bool on);
bool c3dWide();
// Wait only for the top screen's refresh (the bottom one stays at 60 Hz, and citro3d's own sync waits for both:
// a top screen set faster than 60 Hz would be held to 60 frames).
void c3dSyncTopOnly(bool on);
// Issues every world draw twice, with nothing changed between (for finding out what a draw call costs the GPU).
void c3dSetDoubleDraws(bool on);
bool c3dPlainShader();
// `classic` = true: the shader of the versions before the fast one (world.v.pica: projection and view apart,
// the same code for everything), for comparing. Call before init(); ignored with the plain or the fade shader.
void c3dSetClassicShader(bool classic);
// True if the two-entry shader (fast static world, full for characters and what sways) is the one in use.
bool c3dFastShader();
// `fade` = true: the shader with the dissolve (world_fade.v.pica) and the
// noise texture on unit 1, so that gfx::setFade works. Off by default: the
// first build with it showed a black screen on the console, and this keeps
// the known-good shader and GPU state until the dissolve has been seen to
// work there. Call before init(); ignored with the plain shader.
void c3dSetFadeShader(bool fade);

} // namespace gfx
} // namespace g3
