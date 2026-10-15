// Rendering backend interface. Two implementations:
//   platform/3ds/gfx_c3d.cpp   - PICA200 through citro3d
//   platform/host/gfx_soft.cpp - software rasteriser that mimics the same
//                                fixed-function path (texture * vertex colour,
//                                alpha test, blending, per-vertex linear fog) for PC testing.
// The engine never touches platform APIs directly.
#pragma once
#include "g3ds/math.h"
#include "g3ds/model.h"
#include "g3ds/tex.h"
#include "g3ds/world.h"

namespace g3 {
namespace gfx {

struct Texture;
struct Mesh;

constexpr int SCREEN_W = 400; // top screen
constexpr int SCREEN_H = 240;

struct FrameDesc {
	Mat4 view;
	Vec3 eye; // the camera position `view` was built from
	float fovY;
	float zNear, zFar;
	// Linear fog on view-space depth, evaluated per vertex (like ZenGin's):
	// factor = clamp((fogEnd - z) / (fogEnd - fogStart), 0, 1), 1 = no fog.
	float fogStart, fogEnd;
	u8 fogColor[3];
	u8 clearColor[3];
	// Time-of-day tint. A vertex's light is multiplied by
	// lerp(1, dayLight, vertex.a / 255): full effect outdoors, none indoors.
	float dayLight[3];
};

enum Pass {
	PASS_WORLD, // opaque geometry: depth tested and written, back faces culled, fogged
	PASS_SKY,   // backdrop: no depth, no culling, no fog
	PASS_BLEND, // translucent geometry after the opaque world: depth tested but not
	            // written, both sides drawn, blended per material (MATF_BLEND / MATF_ADD)
};

struct MemStats {
	u32 meshBytes;    // vertex + index buffers (linear heap on 3DS)
	u32 textureBytes; // texture memory
	u32 meshCount;
	u32 textureCount;
};

// What the backend made of the last finished frame.
struct FrameStats {
	u32 drawsDropped;     // draw calls skipped because the GPU command buffer was full
	u32 programSwitches;  // changes between the fast and the full vertex shader in the frame (console only)
	float commandBufferUse; // 0..1, share of the GPU command buffer the frame needed
	// Timing; all zero where the backend has no display to keep pace with.
	float frameMs; // from the previous frame's display sync to this one's: what the player sees
	float cpuMs;   // spent issuing the frame's draw calls
	float gpuMs;   // spent by the GPU drawing the frame before this one
	// True if the GPU draws one frame while the CPU issues the next (a frame
	// then needs the larger of the two times, not their sum).
	bool overlapped;
};

bool init();
void shutdown();

// `data` is the PICA-native mip chain that follows the TexHeader.
Texture* textureCreate(const TexHeader& h, const u8* data);
void textureDestroy(Texture* t);

Mesh* meshCreate(const WorldVertex* vertices, u32 vertexCount, const u16* indices, u32 indexCount);
void meshDestroy(Mesh* m);
// Overwrites the first `vertexCount` vertices of a mesh (at most as many as it
// was made with); the indices stay. For geometry that changes every frame
// (particles): make several meshes and use them in turn, so that the one the
// GPU is still drawing is not the one written.
void meshUpdate(Mesh* m, const WorldVertex* vertices, u32 vertexCount);

void frameBegin(const FrameDesc& f);
// Stereo 3D, two renders: with stereoActive() the frame is drawn twice. frameBegin starts the left eye; when its
// draw calls (the scene and the overlay) are done, eyeBegin starts the right one, which is drawn in the same way and
// ended by the one frameEnd. Where a backend has no second eye, stereoAvailable() is false and the rest does nothing.
bool stereoAvailable();
bool stereoActive();
// From the next frame on; between frames.
void stereoSet(bool on);
void eyeBegin(const FrameDesc& f);
// Starts in PASS_WORLD after frameBegin.
void setPass(Pass pass);
// "Graphics+" of world geometry, from what the vertices' w carries
// (g3ds/world.h: WORLDF_AO, WORLDF_WIND). All zero: nothing of it, as for
// everything that is not the world (the sky, particles, the overlay).
struct ChunkFx {
	float occlusion = 0.0f; // 0..1: how much of the baked ambient occlusion darkens the light
	// Sideways movement, cm, of a corner of sway 1 (one step, WORLD_WIND_UNIT)
	// at the crest of the wave. Only materials with MATF_WIND move.
	float windX = 0.0f, windZ = 0.0f;
	float windPhase = 0.0f; // 0..1: the wave's phase at the chunk's origin (see windPhaseAt)
	// Water waves (MATF_WATER materials only; needs water baked in a fine grid, g3ds-bake --water-grid): the phases
	// of the two waves at the chunk's origin (0..1, see wavePhaseAt), how high they lift the surface at the crest, cm
	// (0 = flat water, the fast shader), and how much their slopes light and darken it.
	float waveA = 0.0f, waveB = 0.0f, waveHeight = 0.0f, waveShade = 0.0f;
	// The chunk's water carries the waves' weight in its vertices' w (WORLDF_WATER_GRID): the corner's wind byte, 0..127,
	// of how much of the height and the shading it takes. Such water is drawn with the full shader even when still,
	// because the fast one reads the whole of w as occlusion.
	bool waterGrid = false;
	// The sun's shadow (g3ds-bake --sun-shadow): a mask (a TEX_L4 texture, 15 sunlit .. 0 shaded) lying flat over the
	// chunk, u = (x - sunX0) * sunScaleX, v = (z - sunZ0) * sunScaleZ in world cm. Null: none. What is drawn under its
	// dark texels is darkened by setSunShadow's strength - except cut-out, swaying, glowing and water materials, and characters.
	Texture* sun = nullptr;
	float sunX0 = 0.0f, sunZ0 = 0.0f, sunScaleX = 0.0f, sunScaleZ = 0.0f;
};
// How dark a fully shaded texel of a chunk's sun mask makes what lies under it: 0 (off) .. 1 (black). For the frame.
void setSunShadow(float darkness);
// Whether this backend draws the sun's masks at all (the console's older shaders do not).
bool sunShadowSupported();
// The water's two waves (world_full.v.pica has the same numbers): directions (x, z) and wavelengths, cm; speed in
// waves a second.
constexpr float WAVE_A_DIR[2] = {0.8f, 0.6f};
constexpr float WAVE_A_CM = 1400.0f;
constexpr float WAVE_A_SPEED = 0.07f;
constexpr float WAVE_B_DIR[2] = {0.6313f, 0.7755f}; // 14 degrees off A: one swell
constexpr float WAVE_B_CM = 900.0f;
constexpr float WAVE_B_SPEED = 0.10f;
// The phase (0..1) of wave A (which = 0) or B (1) at `origin` at time `seconds`; in double, as windPhaseAt.
inline float wavePhaseAt(const float origin[3], double seconds, int which) {
	const float* d = which ? WAVE_B_DIR : WAVE_A_DIR;
	const double cm = which ? double(WAVE_B_CM) : double(WAVE_A_CM);
	const double speed = which ? double(WAVE_B_SPEED) : double(WAVE_A_SPEED);
	double t = seconds * speed + (double(origin[0]) * double(d[0]) + double(origin[2]) * double(d[1])) / cm;
	t -= floor(t);
	return float(t);
}
// How the waves lift the water at (x, z) at time `seconds`, as the vertex shader does: the height (cm) at full weight and
// 1 for the crest `height`; and the slope there (cm of height per cm along x and z) - `weight` 0..1 of the corner's share.
inline void waveSurfaceAt(float x, float z, double seconds, float height, float weight, float& lift, float& slopeX, float& slopeZ) {
	lift = slopeX = slopeZ = 0.0f;
	const float mix[2] = {0.7f, 0.3f};
	for (int k = 0; k < 2; ++k) {
		const float* d = k ? WAVE_B_DIR : WAVE_A_DIR;
		const double cm = k ? double(WAVE_B_CM) : double(WAVE_A_CM);
		const double speed = k ? double(WAVE_B_SPEED) : double(WAVE_A_SPEED);
		double t = seconds * speed + (double(x) * double(d[0]) + double(z) * double(d[1])) / cm;
		t -= floor(t);
		const float xs = float(t + t - 1.0);
		const float edge = 1.0f - (xs < 0 ? -xs : xs);
		lift += mix[k] * 4.0f * xs * edge;
		// d(4 x (1 - |x|))/dx = 4 - 8 |x|; dx/dt = 2; dt/dposition = direction / wavelength
		const float dw = mix[k] * (4.0f - 8.0f * (xs < 0 ? -xs : xs)) * 2.0f / float(cm);
		slopeX += dw * d[0];
		slopeZ += dw * d[1];
	}
	const float s = height * weight;
	lift *= s;
	slopeX *= s;
	slopeZ *= s;
}
// The wave travels along x + z: one wave per this many cm.
constexpr float WIND_WAVE_CM = 800.0f;
// The wave's phase at `origin` at time `seconds` with `wavesPerSecond`
// passing: worked out in double, so it stays exact far from the world's
// middle and long into the game, and the wave runs on unbroken from chunk
// to chunk (the shader adds the corner's offset from the origin).
inline float windPhaseAt(const float origin[3], double seconds, float wavesPerSecond) {
	double t = seconds * double(wavesPerSecond) + (double(origin[0]) + double(origin[2])) / double(WIND_WAVE_CM);
	t -= floor(t);
	return float(t);
}

// Sets the dequantisation transform for the following draws:
// world = origin + position * posScale. `fx` (may be null: none) applies
// until the next call.
void setChunkTransform(const float origin[3], float posScale, float uvScale, const ChunkFx* fx = nullptr);
// materialFlags: MaterialFlags from world.h. A null texture draws white.
// opacity scales the alpha of blended materials; uvOffset is added to the
// texture coordinates (scrolling water).
void setMaterial(Texture* t, u32 materialFlags, float opacity = 1.0f, float uvOffsetU = 0.0f, float uvOffsetV = 0.0f);
void draw(Mesh* m, u32 firstIndex, u32 indexCount);
// Dissolve: of the draws that follow only the share `visible` (0..1) of
// their pixels is drawn, picked by a fixed 8x8 pattern on the screen (alpha
// test against a noise texture; PICA has no blending that keeps the depth
// buffer right). 1 = everything, as normal. PASS_WORLD only; frameBegin
// resets it to 1. Objects coming into their draw distance are dissolved in
// with it rather than appearing at once.
void setFade(float visible);
// False where the backend cannot dissolve (setFade then draws everything).
bool fadeSupported();
// Both faces of the following draws, not only the front (PASS_WORLD; the
// other passes draw both anyway). setPass and frameBegin end it.
void setTwoSided(bool both);
// Draw-once (PASS_BLEND): while on, a pixel is drawn at most once by the following draws - the first triangle there
// wins, the ones over it are not drawn. A flat shape made of overlapping parts (the hero's body thrown onto the
// ground) then darkens the ground evenly, not twice and thrice where an arm lies over the body. Each `true` starts
// a new shape (what an earlier one drew does not count). setPass, frameBegin and eyeBegin end it.
void setDrawOnce(bool on);

// Skinned geometry (characters). The mesh is made with meshCreate from
// vertices in the layout model.h describes: every vertex follows two of the
// `bones` of the draw call.
struct SkinDraw {
	// boneCount matrices of 12 floats (3 rows of 4): rest position in model
	// space -> position relative to `origin`.
	const float* bones;
	u32 boneCount;   // at most MODEL_PALETTE
	float origin[3]; // where the model stands in the world
	float posScale, uvScale;
	float light[3];  // colour the character is lit with, time of day included
};
// Uses the material set with setMaterial. PASS_WORLD only.
void drawSkinned(Mesh* m, u32 firstIndex, u32 indexCount, const SkinDraw& skin);
// ---- the 2D overlay: menus and the HUD ----
// Drawn last, over the picture: no depth, no fog, no culling, blended by
// alpha. A quad is an axis-aligned rectangle of the screen (pixels, origin
// top left, x1 and y1 not included) showing a rectangle of the texture
// (texels), the texel multiplied by the colour. The texture is white with
// the given coverage as alpha (and as brightness: a half-covered texel is
// half as bright, the same on both backends).
struct UiQuad {
	i16 x0, y0, x1, y1;
	u16 u0, v0, u1, v1;
	u8 rgba[4];
};
// `coverage`: w * h bytes, rows from the top, 0..255. Freed with textureDestroy.
// `smooth`: filtered and clamped at the edges (a soft shape drawn in the
// world, like the shadows under the characters) instead of texel-exact.
Texture* uiTextureCreate(u16 w, u16 h, const u8* coverage, bool smooth = false);
// After the frame's 3D passes, before frameEnd. Quads of one alpha are one
// draw call: keep them together.
void uiDraw(Texture* t, const UiQuad* quads, u32 count);
// Most quads a frame can draw (the rest is dropped).
constexpr u32 UI_MAX_QUADS = 1500;

void frameEnd();

// Destroyed textures and meshes may stay allocated until the GPU has finished
// the frame that last used them. This waits for that and frees them now. Call
// it outside a frame, before allocating a lot at once (a world switch).
void releaseDestroyed();

MemStats memStats();
FrameStats frameStats();

} // namespace gfx
} // namespace g3
