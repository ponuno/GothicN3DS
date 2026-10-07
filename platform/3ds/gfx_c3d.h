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
bool c3dPlainShader();

} // namespace gfx
} // namespace g3
