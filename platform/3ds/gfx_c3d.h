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
// Issues every world draw twice, with nothing changed between (for finding out what a draw call costs the GPU).
void c3dSetDoubleDraws(bool on);
bool c3dPlainShader();
// `fade` = true: the shader with the dissolve (world_fade.v.pica) and the
// noise texture on unit 1, so that gfx::setFade works. Off by default: the
// first build with it showed a black screen on the console, and this keeps
// the known-good shader and GPU state until the dissolve has been seen to
// work there. Call before init(); ignored with the plain shader.
void c3dSetFadeShader(bool fade);

} // namespace gfx
} // namespace g3
