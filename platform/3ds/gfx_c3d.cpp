// PICA200 backend (libctru + citro3d).
//
// Fixed pipeline for world geometry:
//   vertex shader  world.v.pica (dequantise, transform, per-vertex linear fog;
//                  Graphics+: baked ambient occlusion and swaying plants, see ChunkFx)
//   TexEnv 0       texture * baked vertex light, alpha = texture alpha * opacity
//   TexEnv 1       lerp towards the fog colour by the fog factor (vertex alpha)
//   TexEnv 2, 3    only while dissolving (setFade): alpha - noise + share - 0.5,
//                  the noise from texture unit 1 at the screen position
//   alpha test     for cut-out materials, and while dissolving
//   blending       off for the opaque passes, per material in PASS_BLEND
// Vertex and index buffers live in the linear heap and are used in place.
//
// Every state setter of citro3d marks a whole register group dirty and the
// next draw call re-sends it, so all state here is cached and only touched
// when it really changes: the GPU command buffer is finite, and overflowing
// it stops the program (libctru calls svcBreak).
#include "gfx.h"
#include "gfx_c3d.h"
#include "saves.h"
#include "trace.h"

#include "world_fade_shbin.h"
#include "world_fast_shbin.h"
#include "world_plain_shbin.h"
#include "world_shbin.h"

#include <3ds.h>
#include <citro3d.h>

#include <stdlib.h>
#include <string.h>

#define DISPLAY_TRANSFER_FLAGS                                                                       \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |                 \
	 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |   \
	 GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

namespace g3 {
namespace gfx {

struct Texture {
	C3D_Tex tex;
	u32 bytes; // counted in MemStats (0 for the overlay's own: it is not the world's)
};

struct Mesh {
	C3D_BufInfo buf;
	WorldVertex* vertices;
	u16* indices;
	u32 vertexCount, indexCount;
};

namespace {

C3D_RenderTarget* gTarget = nullptr;
// Stereo 3D (two renders): the right eye's own target, made at start when asked for (c3dSetStereo), and whether the
// frames are being drawn twice now (gfx::stereoSet).
C3D_RenderTarget* gTargetR = nullptr;
bool gStereoWanted = false, gStereoOn = false, gStereoFrame = false;
u32 gUiEyeBase = 0; // the quads of the eyes before this one in the frame
bool gSuper = false;       // supersampling in use: the target is 480x800 and the display transfer averages 2x2
bool gSuperWanted = false; // asked for before init()
bool gSuperLightWanted = false, gSuperLight = false; // ... as the light kind (480x400, averaged in pairs along the 480 only)
bool gWideWanted = false, gWide = false; // wide screen (800x240): asked for before init(), and whether the screen took it
DVLB_s* gDvlb = nullptr;
shaderProgram_s gProgram;     // the full shader (entry `main`); with the classic, plain and fade ones the only program
shaderProgram_s gProgramFast; // the fast shader (entry `fastmain` of the same binary, `fadefast` with the dissolve), when gMvpShader
shaderProgram_s gProgramSkin; // the characters' shader (entry `skinmain`), when gMvpShader
shaderProgram_s gProgramFastSun; // the fast one with the sun's mask (entry `fastsun`), for chunks that have one
shaderProgram_s gProgramLamp;    // fix148: the fast one with the dissolve and four lamps (entry `lampmain`, DVLE 5)
bool gProgramReady = false, gProgramFastReady = false, gProgramSkinReady = false, gProgramFastSunReady = false, gProgramLampReady = false;
bool gReady = false; // C3D_Init succeeded
int gLocProjection = -1, gLocModelView = -1, gLocChunk = -1, gLocParams = -1, gLocDayDelta = -1, gLocTexOffset = -1;
int gLocSkinLight = -1, gLocBones = -1, gLocSkinned = -1, gLocFading = -1;
int gLocFx = -1, gLocWindy = -1; // Graphics+ (not in the plain shader)
int gLocWave = -1, gLocWavy = -1; // the water's waves (only the full entry of the four-entry shader has them)
// The shader with the combined matrix (world_full / world_fast.v.pica): mvp and fogRow stand for projection and
// modelView, lightK and aoK are the fast entry's own forms of dayDelta and the occlusion strength.
int gLocMvp = -1, gLocFogRow = -1, gLocLightK = -1, gLocAoK = -1;
bool gMvpShader = false;     // in use (not with the plain or classic shader; with the dissolve since fix85)
bool gClassicShader = false; // asked for: the shader of earlier versions (world.v.pica), projection and view apart
enum BoundProgram { PROGRAM_FULL, PROGRAM_FAST, PROGRAM_SKIN, PROGRAM_FAST_SUN, PROGRAM_LAMP };
int gBound = PROGRAM_FULL;   // which entry is the bound program
float gViewDepthRow[4] = {0, 0, 1, 0}; // modelView's third row: the fog is worked out from it
u32 gProgramSwitches = 0;    // this frame's changes of the program
bool gSkinned = false; // last value of the shader's `skinned` switch
// Graphics+: the last `fx` uniform sent, and the `windy` switch - on for a
// material with MATF_WIND while the chunk's wind is not zero.
float gFx[4] = {0, 0, 0, 0};
bool gWindy = false, gMaterialWind = false, gChunkWind = false;
// The water's waves: the last `wave` uniform, and the `wavy` switch - on for a water material while the chunk's waves
// are not zero.
float gWave[4] = {0, 0, 0, 0};
bool gWavy = false, gMaterialWater = false, gChunkWave = false, gChunkWaterGrid = false;
bool gFillTest = false;    // the picture squeezed into half the screen (hold ZR at start): does the GPU time follow the pixels?
bool gDoubleDraws = false; // every world draw is issued twice (hold START at start): what does one more draw call cost with no state change?
bool gPlainShader = false; // the world-only shader of earlier versions: no characters
bool gFadeShader = false;  // the shader with the dissolve, and the noise on unit 1
Pass gPass = PASS_WORLD;

// 1 MB of command buffer: about 5000 draw calls with a texture change each.
const size_t kCommandBufferBytes = 0x100000;
// Overlapped frames: the CPU writes one command buffer while the GPU reads
// the other. A frame of the real game needs 80-140 KB.
const u32 kOwnBufferWordsMono = 0x80000 / 4;
u32 kOwnBufferWords = kOwnBufferWordsMono; // twice that when the frame is drawn twice
u32 gUiCapacity = UI_MAX_QUADS;
u32* gOwnBuffer[2] = {nullptr, nullptr};
int gOwnIndex = 0;
bool gClassic = false;
u32 gClearColor = 0;
TickCounter gCpuClock, gSyncClock;
bool gSyncStarted = false;
// Words kept free for one draw call with every state group dirty, plus the
// few commands C3D_FrameEnd appends.
const u32 kDrawReserveWords = 0x400;

enum BlendMode { BLEND_OFF, BLEND_ALPHA, BLEND_ADD, BLEND_UNSET };

FrameDesc gFrame;
Texture* gBoundTexture = nullptr;
Mesh* gBoundMesh = nullptr;
bool gTextured = true;
bool gAlphaTest = false; // the material's own (cut-out)
// Dissolving (setFade): the share of pixels drawn, 255 = all (off), and the
// noise texture it is tested against.
u8 gFade = 255;
C3D_Tex gNoise;
bool gNoiseReady = false;
bool gInFrame = false;
u8 gOpacity = 255;
bool gFogToBlack = false;
BlendMode gBlend = BLEND_UNSET;
float gTexOffset[2] = {0, 0};
float gAnchor[3] = {0, 0, 0};       // the lattice point next to the eye that positions are relative to
float gParams[4] = {0, 0, 0, 0};    // last value of the `params` uniform
MemStats gMem;
FrameStats gFrameStats = {};
u32 gDropped = 0;
// The overlay's quads (uiDraw): written into one of two vertex buffers while
// the GPU may still read the other (the frame before), and drawn with one
// index buffer of quads that never changes.
WorldVertex* gUiVertices[2] = {nullptr, nullptr};
u16* gUiIndices = nullptr;
int gUiBuffer = 0;
u32 gUiUsed = 0; // quads written this frame
bool gUiPass = false;
C3D_BufInfo gUiBufInfo;

// Engine matrices are column-major; C3D_Mtx stores rows.
void toC3D(const Mat4& m, C3D_Mtx* out) {
	for (int r = 0; r < 4; ++r) {
		out->r[r].x = m.at(r, 0);
		out->r[r].y = m.at(r, 1);
		out->r[r].z = m.at(r, 2);
		out->r[r].w = m.at(r, 3);
	}
}

// Stage 0: colour = texture * vertex light; alpha = texture alpha * opacity.
// The opacity travels in the stage's constant colour.
void setStage0(bool textured, u8 opacity) {
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	if (textured) {
		C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
		C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
	} else {
		C3D_TexEnvSrc(env, C3D_RGB, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
		C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
		C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	}
	C3D_TexEnvColor(env, 0x00FFFFFFu | (u32(opacity) << 24));
	gTextured = textured;
	gOpacity = opacity;
}

// Stage 3: out = previous * fog + fogColour * (1 - fog), fog = primary alpha.
// Added light fades to black instead, so that it adds nothing in the distance.
// (Stages 1 and 2 are the sun's shadow: setSunStages.)
void setFogStage(bool toBlack) {
	C3D_TexEnv* env = C3D_GetTexEnv(3);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
	C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	const u8* c = gFrame.fogColor;
	C3D_TexEnvColor(env, toBlack ? 0xFF000000u : (u32(c[0]) | (u32(c[1]) << 8) | (u32(c[2]) << 16) | 0xFF000000u));
	gFogToBlack = toBlack;
}

// The alpha test is on for cut-out materials and while dissolving.
bool gAlphaTestOn = false;
void applyAlphaTest() {
	const bool on = gAlphaTest || gFade != 255;
	if (on == gAlphaTestOn) return;
	C3D_AlphaTest(on, GPU_GREATER, 127);
	gAlphaTestOn = on;
}

// An ordered 8x8 pattern: the share of its texels below a threshold is the
// threshold, and they are spread evenly at every share.
void makeNoise() {
	if (!C3D_TexInit(&gNoise, 8, 8, GPU_A8)) return;
	u8* texels = static_cast<u8*>(gNoise.data);
	for (int y = 0; y < 8; ++y)
		for (int x = 0; x < 8; ++x) {
			// Bayer rank of (x, y): the bits of x ^ y and y interleaved, reversed.
			const int a = x ^ y, b = y;
			const int rank = ((a & 1) << 5) | ((b & 1) << 4) | ((a & 2) << 2) | ((b & 2) << 1) | ((a & 4) >> 1) | ((b & 4) >> 2);
			// An 8x8 texture is one tile: texels in Morton order, rows from the bottom.
			const int ty = 7 - y;
			const int at = (x & 1) | ((ty & 1) << 1) | ((x & 2) << 1) | ((ty & 2) << 2) | ((x & 4) << 2) | ((ty & 4) << 3);
			texels[at] = u8(rank * 4 + 2);
		}
	C3D_TexFlush(&gNoise);
	C3D_TexSetFilter(&gNoise, GPU_NEAREST, GPU_NEAREST);
	C3D_TexSetWrap(&gNoise, GPU_REPEAT, GPU_REPEAT);
	gNoiseReady = true;
}

// ---- Graphics+: the sun's shadow (ChunkFx::sun) ----
// fix143: the chunk's mask is on texture unit 1, laid over the ground by the vertex shader's `fastsun` entry (texture
// coordinate 1 = (x, z) * aoK.yz + texOffset.zw). It was unit 2 and texture coordinate 2: the console's GPU hung on that
// (fix133..142; with `nosun` it did not). Unit 1 is the dissolve's noise otherwise: a chunk that is dissolving is drawn
// without its mask (useProgram, updateSun), so the two never meet in one draw. Stage 1: previous * mask. Stage 2: that and stage 0's colour (kept in the combiner
// buffer) mixed by the strength in its constant: colour * (1 - strength * (1 - mask)). Strength 0 gives stage 0's
// colour back whatever unit 2 holds, so the stages stay set and only the constant changes.
// (citro3d cannot unbind units 1 and 2 - see the noise - so unit 2 keeps a texture once it has had one: the mask
// last drawn, or a white 8x8 one.)
Texture* gSunChunk = nullptr;   // the mask of the chunk being drawn (null: none)
bool gSunMaterialOk = true;     // the material takes the shadow
float gSunDark = 0.0f;          // setSunShadow
float gChunkSunDark = 0.0f;     // fix186: the strength the chunk being drawn takes (ChunkFx::sunDark, else gSunDark)
u8 gSunConst = 0;               // what stage 2's constant holds
const C3D_Tex* gSunBound = nullptr;
C3D_Tex gSunWhite;
bool gSunWhiteReady = false;
float gSunUv[4] = {0, 0, 0, 0}; // scale x, scale z, offset u, offset v (relative to the anchor)
bool gSunUvSent = false;

// ---- fix148: the lamps (setLamps) ----
// The set for the draws that follow, and whether the first eight rows of `bones` hold it (the characters' entry
// overwrites them for every draw; a new frame or eye has a new anchor the positions are relative to).
LampSet gLamps;      // wanted for the draws that follow (count 0: none); zeros past its count
LampSet gLampsRows;  // what the rows hold while gLampsSent
bool gLampsSent = false;
bool gNoLampProgram = false; // c3dSetNoLamps (debug.txt nolamps): the lamps' entry is never bound
u32 gLampDraws = 0;          // this frame's draws by it

u16 gDebugSkipFlags = 0; // c3dSetDebugDraw: materials with any of these flags are not drawn
int gDebugProgram = 0;   // 1: the fast entry for every world draw, 2: the full entry for every one
u16 gMaterialFlags = 0;
bool gNoSun = false;
bool gNoSunProgram = false; // c3dSetNoSun (C-Stick up held at start, debug.txt nosun): no sun's stages, no sun's program
// fix144 (c3dSetSunDebug): by default the masks (TEX_L4 in data.pak) are turned into GPU_A8 as they are loaded - the format of
// the dissolve's noise, the one texture known to be sampled on unit 1 without trouble; GPU_L4 was used by nothing else and the
// GPU hung whenever a mask was on unit 1 (fix133..143). Stage 1 then takes the mask's alpha. And a mask is on unit 1 only while
// the sun's entry is bound: any other entry finds the noise (or the white 8x8) there again.
unsigned gSunDebug = 0;
inline bool sunA8() { return (gSunDebug & SUN_DBG_L4) == 0; }

// The white 8x8 stand-in for unit 1 when the noise is not there (no dissolve) and a mask that was on it is freed.
bool makeSunWhite() {
	if (!gSunWhiteReady && C3D_TexInit(&gSunWhite, 8, 8, sunA8() ? GPU_A8 : GPU_L4)) {
		memset(gSunWhite.data, 0xFF, sunA8() ? 64 : 32);
		C3D_TexFlush(&gSunWhite);
		C3D_TexSetFilter(&gSunWhite, GPU_NEAREST, GPU_NEAREST);
		C3D_TexSetWrap(&gSunWhite, GPU_REPEAT, GPU_REPEAT);
		gSunWhiteReady = true;
	}
	return gSunWhiteReady;
}

void setSunStages() {
	if (gNoSun) {
		// Stages 1 and 2 pass the colour on and read no texture.
		C3D_TexEnvInit(C3D_GetTexEnv(1));
		C3D_TexEnvInit(C3D_GetTexEnv(2));
		C3D_TexEnvBufUpdate(C3D_RGB, 0);
		gSunConst = 0;
		return;
	}
	C3D_TexEnv* s1 = C3D_GetTexEnv(1);
	C3D_TexEnvInit(s1);
	if (gSunDebug & SUN_DBG_NOTEX) {
		// (sunnotex: stage 1 passes the colour on; stage 2 then mixes it with itself - no shadow, no texture read.)
		C3D_TexEnvSrc(s1, C3D_RGB, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(s1, C3D_RGB, GPU_REPLACE);
	} else {
		C3D_TexEnvSrc(s1, C3D_RGB, GPU_PREVIOUS, GPU_TEXTURE1, GPU_PRIMARY_COLOR);
		// An A8 mask has its value in the alpha (its rgb reads 0): the second operand is the texel's alpha then.
		C3D_TexEnvOpRgb(s1, GPU_TEVOP_RGB_SRC_COLOR, sunA8() ? GPU_TEVOP_RGB_SRC_ALPHA : GPU_TEVOP_RGB_SRC_COLOR,
		                GPU_TEVOP_RGB_SRC_COLOR);
		C3D_TexEnvFunc(s1, C3D_RGB, GPU_MODULATE);
	}
	C3D_TexEnvSrc(s1, C3D_Alpha, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(s1, C3D_Alpha, GPU_REPLACE);
	C3D_TexEnv* s2 = C3D_GetTexEnv(2);
	C3D_TexEnvInit(s2);
	C3D_TexEnvSrc(s2, C3D_RGB, GPU_PREVIOUS, GPU_PREVIOUS_BUFFER, GPU_CONSTANT);
	C3D_TexEnvFunc(s2, C3D_RGB, GPU_INTERPOLATE);
	C3D_TexEnvSrc(s2, C3D_Alpha, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(s2, C3D_Alpha, GPU_REPLACE);
	C3D_TexEnvColor(s2, 0xFF000000u);
	gSunConst = 0;
	// Stage 0's output into the combiner buffer (stage 2 reads it as PREVIOUS_BUFFER); stage 1's does not go in.
	C3D_TexEnvBufUpdate(C3D_RGB, BIT(0));
}

void sendSunUniforms() {
	if (!gMvpShader) return;
	C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocTexOffset, gTexOffset[0], gTexOffset[1], gSunUv[2], gSunUv[3]);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocAoK, gFx[3] / 256.0f, gSunUv[0], gSunUv[1], 0.0f);
}

// Unit 1 without a mask: the dissolve's noise, or (no dissolve) the white 8x8 - never nothing, a stage reads it.
const C3D_Tex* gUnit1 = nullptr; // what unit 1 holds (bound once, not again every frame)
void bindUnit1Default() {
	C3D_Tex* t = gNoiseReady ? &gNoise : makeSunWhite() ? &gSunWhite : nullptr;
	if (t && t != gUnit1) {
		C3D_TexBind(1, t);
		gUnit1 = t;
	}
	gSunBound = nullptr;
}

void bindSunTexture(const C3D_Tex* t) {
	if (t == gSunBound || !t) return;
	C3D_TexBind(1, const_cast<C3D_Tex*>(t));
	gSunBound = t;
	gUnit1 = t;
}

// What stage 2's constant must hold now, and unit 1 with it.
void updateSun() {
	if (gNoSun) return;
	float s = 0.0f;
	if (gSunChunk && gSunMaterialOk && !gSkinned && !gUiPass && gPass != PASS_SKY && gFade == 255 && !gNoSunProgram) s = gChunkSunDark;
	const u8 c = u8(clamp(s, 0.0f, 1.0f) * 255.0f + 0.5f);
	if (c && !(gSunDebug & SUN_DBG_NOTEX)) {
		if (gSunDebug & SUN_DBG_WHITE) {
			if (makeSunWhite()) bindSunTexture(&gSunWhite);
		} else {
			bindSunTexture(&gSunChunk->tex);
		}
	}
	if (c == gSunConst) return;
	gSunConst = c;
	C3D_TexEnvColor(C3D_GetTexEnv(2), 0xFF000000u | (u32(c) << 16) | (u32(c) << 8) | u32(c));
}

// Stages 4 and 5: pass the colour on; alpha = max(alpha - noise, 0) + share - 0.5.
// The alpha test (> 0.5) then keeps an opaque pixel where noise < share, and
// a cut-out texel (alpha 0) never.
void setFadeStages(bool on, u8 share) {
	C3D_TexEnv* s2 = C3D_GetTexEnv(4);
	C3D_TexEnv* s3 = C3D_GetTexEnv(5);
	C3D_TexEnvInit(s2);
	C3D_TexEnvInit(s3);
	if (!on) return;
	C3D_TexEnvSrc(s2, C3D_Alpha, GPU_PREVIOUS, GPU_TEXTURE1, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(s2, C3D_Alpha, GPU_SUBTRACT);
	C3D_TexEnvSrc(s3, C3D_Alpha, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(s3, C3D_Alpha, GPU_ADD_SIGNED);
	C3D_TexEnvColor(s3, u32(share) << 24);
}

void setBlend(BlendMode mode) {
	if (mode == gBlend) return;
	gBlend = mode;
	if (mode == BLEND_ALPHA)
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
	else if (mode == BLEND_ADD)
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_ZERO, GPU_ONE);
	else
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
}

void setTexOffset(float u, float v) {
	if (u == gTexOffset[0] && v == gTexOffset[1]) return;
	gTexOffset[0] = u;
	gTexOffset[1] = v;
	C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocTexOffset, u, v, gMvpShader ? gSunUv[2] : 0.0f, gMvpShader ? gSunUv[3] : 0.0f);
}

// Graphics+: the `fx` uniform, and the `windy` switch that follows from it,
// the material and whether a character is drawn.
void setFxUniform(float x, float y, float z, float w) {
	if (gLocFx < 0 || (x == gFx[0] && y == gFx[1] && z == gFx[2] && w == gFx[3])) return;
	gFx[0] = x;
	gFx[1] = y;
	gFx[2] = z;
	gFx[3] = w;
	C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocFx, x, y, z, w);
	if (gMvpShader) C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocAoK, w / 256.0f, gSunUv[0], gSunUv[1], 0.0f);
}

void updateWindy() {
	const bool on = gLocWindy >= 0 && gMaterialWind && gChunkWind && !gSkinned;
	if (on != gWindy) {
		gWindy = on;
		C3D_BoolUnifSet(GPU_VERTEX_SHADER, gLocWindy, on);
	}
	// (fix140: the `wavy` block also gives the water its sheen by distance, so it is on for all water cut in the grid; with
	// the waves off their height and shading are 0 and the water is as still as before.)
	const bool wavy = gLocWavy >= 0 && gMaterialWater && gChunkWaterGrid && !gSkinned;
	if (wavy != gWavy) {
		gWavy = wavy;
		C3D_BoolUnifSet(GPU_VERTEX_SHADER, gLocWavy, wavy);
	}
}

void setWaveUniform(float a, float b, float height, float shade) {
	if (gLocWave < 0 || (a == gWave[0] && b == gWave[1] && height == gWave[2] && shade == gWave[3])) return;
	gWave[0] = a;
	gWave[1] = b;
	gWave[2] = height;
	gWave[3] = shade;
	// (The shader multiplies by the corner's weight, 0..127: the height and the shading come in per step of it.)
	C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocWave, a, b, height * (1.0f / 127.0f), shade * (1.0f / 127.0f));
}

// The shader with the combined matrix. A C3D_Mtx holds rows; its components are named, whatever order citro3d keeps them in.
float rowComponent(const C3D_FVec& v, int j) { return j == 0 ? v.x : j == 1 ? v.y : j == 2 ? v.z : v.w; }

// projection and modelView: to the two uniforms of the earlier shaders, or multiplied, to mvp. `view` = true:
// the view's depth row is kept for the fog (the overlay's identity is not a view).
void setMatrices(const C3D_Mtx& proj, const C3D_Mtx& mv, bool view) {
	if (!gMvpShader) {
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, gLocProjection, &proj);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, gLocModelView, &mv);
		return;
	}
	C3D_Mtx mvp;
	for (int i = 0; i < 4; ++i) {
		float row[4];
		for (int j = 0; j < 4; ++j) {
			float sum = 0.0f;
			for (int k = 0; k < 4; ++k) sum += rowComponent(proj.r[i], k) * rowComponent(mv.r[k], j);
			row[j] = sum;
		}
		mvp.r[i].x = row[0];
		mvp.r[i].y = row[1];
		mvp.r[i].z = row[2];
		mvp.r[i].w = row[3];
	}
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, gLocMvp, &mvp);
	if (view)
		for (int j = 0; j < 4; ++j) gViewDepthRow[j] = rowComponent(mv.r[2], j);
}

// The fog factor, before the clamp, as one dot product with the position: fogA - fogB * viewDepth.
void setFogRow(float fogA, float fogB) {
	if (!gMvpShader) return;
	C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocFogRow, -fogB * gViewDepthRow[0], -fogB * gViewDepthRow[1], -fogB * gViewDepthRow[2],
	              fogA - fogB * gViewDepthRow[3]);
}

// dayLight - 1; the fast entry wants it divided by 255 twice (see world_fast.v.pica).
void setDayDelta(float x, float y, float z) {
	C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocDayDelta, x, y, z, 0.0f);
	if (gMvpShader) C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocLightK, x / 65025.0f, y / 65025.0f, z / 65025.0f, 0.0f);
}

// fix144: a mask is on unit 1 only for the sun's entry. Any other entry (and the overlay, the sky, the characters) finds the
// noise or the white 8x8 there: texture unit 1 samples for every pixel of every draw while it is on, whatever the stages read,
// and those entries' texture coordinate 1 is not the mask's (sunkeep: as fix143, the mask stays).
void guardUnit1() {
	if (gBound != PROGRAM_FAST_SUN && gSunBound && !(gSunDebug & SUN_DBG_KEEP)) bindUnit1Default();
}

// The fast entry draws the static world, the skin entry the characters, the full one what sways.
void bindProgram(int which) {
	if (!gMvpShader || which == gBound) return;
	if (which == PROGRAM_SKIN && !gProgramSkinReady) which = PROGRAM_FULL;
	if (which == PROGRAM_FAST_SUN && !gProgramFastSunReady) which = PROGRAM_FAST;
	if (which == PROGRAM_LAMP && !gProgramLampReady) which = PROGRAM_FAST;
	if (which == gBound) return;
	gBound = which;
	C3D_BindProgram(which == PROGRAM_FAST       ? &gProgramFast
	                : which == PROGRAM_SKIN     ? &gProgramSkin
	                : which == PROGRAM_FAST_SUN ? &gProgramFastSun
	                : which == PROGRAM_LAMP     ? &gProgramLamp
	                                            : &gProgram);
	++gProgramSwitches;
}

// fix148: the lamps into the first eight rows of `bones` (world_lamp.v.pica): (place relative to the anchor, -1/range^2),
// (colour, (dayKeep - 1) / 255). A lamp that is not there: all 0 (the falloff is then 1, times colour 0).
void sendLamps() {
	if (gLampsSent && memcmp(&gLampsRows, &gLamps, sizeof(gLamps)) == 0) return;
	const float dayK = (clamp(gLamps.dayKeep, 0.0f, 1.0f) - 1.0f) / 255.0f;
	for (u32 k = 0; k < MAX_DRAW_LAMPS; ++k) {
		const DrawLamp& l = gLamps.lamp[k];
		const bool on = k < gLamps.count && l.range > 1.0f;
		C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocBones + int(k) * 2, on ? l.pos[0] - gAnchor[0] : 0.0f, on ? l.pos[1] - gAnchor[1] : 0.0f,
		              on ? l.pos[2] - gAnchor[2] : 0.0f, on ? -1.0f / (l.range * l.range) : 0.0f);
		C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocBones + int(k) * 2 + 1, on ? l.rgb[0] : 0.0f, on ? l.rgb[1] : 0.0f, on ? l.rgb[2] : 0.0f, dayK);
	}
	gLampsRows = gLamps;
	gLampsSent = true;
}
// (A chunk with the sun's mask: the fast entry that lays it, for all its batches - the mask's strength, not the entry,
// leaves out the materials that take no shadow.)
void useProgram(bool fast) {
	// (Dissolving needs texture coordinate 1 for the noise: `fadefast`, and the chunk goes without its mask until it is whole.)
	const bool sun = gSunChunk && !gNoSunProgram && gFade == 255;
	// fix148: lamps reaching the draw - the lamps' entry (`fadefast` and the lamps; not with the sun's mask, which wins by day).
	const bool lamps = fast && !sun && gLamps.count && gProgramLampReady && !gNoLampProgram && !gSkinned && !gUiPass && gPass == PASS_WORLD;
	if (lamps) sendLamps();
	bindProgram(fast ? (sun ? PROGRAM_FAST_SUN : lamps ? PROGRAM_LAMP : PROGRAM_FAST) : PROGRAM_FULL);
	gLampDraws += lamps;
	guardUnit1();
}

// The GPU executes a frame's command list after C3D_FrameEnd returns, so
// memory a draw call referenced must outlive that frame. Destroyed resources
// are parked here and released once C3D_FrameBegin has waited for the GPU.
template <class T> struct Graveyard {
	T** items = nullptr;
	u32 count = 0, capacity = 0;
};
Graveyard<Texture> gDeadTextures;
Graveyard<Mesh> gDeadMeshes;

template <class T> bool bury(Graveyard<T>& g, T* item) {
	if (g.count == g.capacity) {
		const u32 cap = g.capacity ? g.capacity * 2 : 64;
		T** p = static_cast<T**>(realloc(g.items, cap * sizeof(T*)));
		if (!p) return false;
		g.items = p;
		g.capacity = cap;
	}
	g.items[g.count++] = item;
	return true;
}

void freeTexture(Texture* t) {
	C3D_TexDelete(&t->tex);
	free(t);
}

void freeMesh(Mesh* m) {
	linearFree(m->vertices);
	linearFree(m->indices);
	free(m);
}

void reap() {
	for (u32 i = 0; i < gDeadTextures.count; ++i) freeTexture(gDeadTextures.items[i]);
	for (u32 i = 0; i < gDeadMeshes.count; ++i) freeMesh(gDeadMeshes.items[i]);
	gDeadTextures.count = 0;
	gDeadMeshes.count = 0;
}

} // namespace

// The render target: 240x400, the screen's own size, or, supersampled, 480x800 (four times the pixels, about 3 MB
// of VRAM with the depth buffer) which the display transfer shrinks by averaging 2x2 as it copies it to the screen
// (libctru: with scaling the dimensions given are the bigger ones, which is what citro3d passes). If the big one does
// not fit into VRAM the ordinary one is made.
static bool makeTarget(bool super) {
	gSuper = false;
	gTarget = nullptr;
	// Wide: the screen is 800 across (the target's height; it is turned), so the target is 240x800, and the smoothing
	// 480x800 averaged in pairs along the 480 only (the same memory as the ordinary smoothing, the picture twice as fine
	// across to begin with).
	const int longSide = gWide ? 800 : 400;
	gSuperLight = false;
	if (super && gSuperLightWanted && !gWide) {
		// The light kind: twice the pixels (not four times) - the picture twice as fine along the 480 (the screen's height) only.
		gTarget = C3D_RenderTargetCreate(480, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
		gSuper = gTarget != nullptr;
		gSuperLight = gSuper;
	}
	if (super && !gTarget) {
		gTarget = C3D_RenderTargetCreate(480, 800, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
		gSuper = gTarget != nullptr;
	}
	if (!gTarget) gTarget = C3D_RenderTargetCreate(240, longSide, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	if (!gTarget) return false;
	C3D_RenderTargetSetOutput(gTarget, GFX_TOP, GFX_LEFT,
	                          gSuper ? (DISPLAY_TRANSFER_FLAGS | GX_TRANSFER_SCALING(gWide || gSuperLight ? GX_TRANSFER_SCALE_X : GX_TRANSFER_SCALE_XY)) : DISPLAY_TRANSFER_FLAGS);
	// The right eye: the screen's own size too (not together with the smoothing: that does not fit into VRAM twice).
	gTargetR = nullptr;
	if (gStereoWanted && !gSuper && !gWide) {
		gTargetR = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
		if (gTargetR) C3D_RenderTargetSetOutput(gTargetR, GFX_TOP, GFX_RIGHT, DISPLAY_TRANSFER_FLAGS);
	}
	return true;
}

bool init() {
	gMem = {};
	gFrameStats = {};
	gSyncStarted = false;
	gOwnIndex = 0;
	// With overlapped frames citro3d's own buffer only ever holds empty frames.
	// (Stereo: the frame is written down twice, so the buffers and the overlay's quads are twice as big. Made even if
	// the right-eye target is not, which only costs the memory; not with the smoothing, which rules stereo out.)
	// (The wide screen takes the top screen's place; the two do not go together.)
	gWide = false;
	if (gWideWanted) {
		// (The old 2DS has no wide screen, and libctru would not say so: it stays as it was.)
		u8 model = 0;
		if (R_SUCCEEDED(cfguInit())) {
			CFGU_GetSystemModel(&model);
			cfguExit();
		}
		if (model != CFG_MODEL_2DS) {
			gfxSetWide(true);
			gWide = gfxIsWide();
		}
	}
	const bool twice = gStereoWanted && !gSuperWanted && !gWide;
	kOwnBufferWords = kOwnBufferWordsMono * (twice ? 2u : 1u);
	gUiCapacity = UI_MAX_QUADS * (twice ? 2u : 1u);
	gStereoOn = gStereoFrame = false;
	if (!C3D_Init(gClassic ? kCommandBufferBytes * (twice ? 2u : 1u) : size_t(C3D_DEFAULT_CMDBUF_SIZE))) return false;
	gReady = true;
	if (!gClassic) {
		for (u32*& b : gOwnBuffer) b = static_cast<u32*>(linearAlloc(kOwnBufferWords * 4));
		if (!gOwnBuffer[0] || !gOwnBuffer[1]) return false;
	}

	if (!makeTarget(gSuperWanted)) return false;

	gMvpShader = false;
	gBound = PROGRAM_FULL;
	// The four-entry shader first (full, fast, fast with the dissolve, characters); if it cannot be had (or its
	// entries disagree about where the uniforms are) the shader of earlier versions takes over. With the dissolve
	// the fast entry is `fadefast`, which also gives the noise its place on the screen (before fix85 the dissolve
	// meant world_fade.v.pica for everything: about 36 instructions a world vertex instead of 22).
	if (!gPlainShader && !gClassicShader) {
		gDvlb = DVLB_ParseFile(reinterpret_cast<u32*>(world_fast_shbin), world_fast_shbin_size);
		if (gDvlb && gDvlb->numDVLE >= 4) {
			shaderProgramInit(&gProgram);
			shaderProgramSetVsh(&gProgram, &gDvlb->DVLE[0]);
			shaderProgramInit(&gProgramFast);
			shaderProgramSetVsh(&gProgramFast, &gDvlb->DVLE[gFadeShader ? 2 : 1]);
			shaderProgramInit(&gProgramSkin);
			shaderProgramSetVsh(&gProgramSkin, &gDvlb->DVLE[3]);
			const bool sunEntries = gDvlb->numDVLE >= 6;
			// fix148: DVLE 5 is the lamps' entry (it took the place of `fadefastsun`, which was never bound since fix142).
			if (sunEntries) {
				shaderProgramInit(&gProgramLamp);
				shaderProgramSetVsh(&gProgramLamp, &gDvlb->DVLE[5]);
			}
			if (sunEntries) {
				shaderProgramInit(&gProgramFastSun);
				// fix142: always `fastsun` (DVLE 4: position, texcoord0, colour, texcoord2), never `fadefastsun` (DVLE 5: five
				// outputs, texcoord1 and texcoord2 both). The two entries with five outputs - `fadefastsun`, and `main` until fix138 -
				// are what the console's GPU hung on: the world drawn with them hung (fix133..141), without them it did not. A chunk
				// that is dissolving (gFade != 255) is drawn by `fadefast` without its sun mask meanwhile (see useProgram).
				shaderProgramSetVsh(&gProgramFastSun, &gDvlb->DVLE[4]);
			}
			static const char* const kShared[] = {"mvp", "fogRow", "chunk", "params", "dayDelta", "texOffset", "lightK", "aoK", "fx"};
			static const char* const kSkin[] = {"mvp", "fogRow", "params", "texOffset", "skinLight", "bones", "fading"};
			gMvpShader = true;
			for (const char* name : kShared) {
				const int a = shaderInstanceGetUniformLocation(gProgram.vertexShader, name);
				gMvpShader = gMvpShader && a >= 0 && a == shaderInstanceGetUniformLocation(gProgramFast.vertexShader, name);
			}
			if (gFadeShader) {
				const int a = shaderInstanceGetUniformLocation(gProgram.vertexShader, "fading");
				gMvpShader = gMvpShader && a >= 0 && a == shaderInstanceGetUniformLocation(gProgramFast.vertexShader, "fading");
			}
			bool skinOk = gMvpShader;
			for (const char* name : kSkin) {
				const int a = shaderInstanceGetUniformLocation(gProgram.vertexShader, name);
				skinOk = skinOk && a >= 0 && a == shaderInstanceGetUniformLocation(gProgramSkin.vertexShader, name);
			}
			bool sunOk = gMvpShader && sunEntries;
			for (const char* name : kShared) {
				if (!sunOk) break;
				const int a = shaderInstanceGetUniformLocation(gProgram.vertexShader, name);
				sunOk = a >= 0 && a == shaderInstanceGetUniformLocation(gProgramFastSun.vertexShader, name);
			}
			// (The lamps' entry: the shared uniforms and `bones` where the full one has them, and `fading`.)
			bool lampOk = gMvpShader && sunEntries;
			static const char* const kLamp[] = {"mvp", "fogRow", "chunk", "params", "texOffset", "lightK", "aoK", "bones", "fading"};
			for (const char* name : kLamp) {
				if (!lampOk) break;
				const int a = shaderInstanceGetUniformLocation(gProgram.vertexShader, name);
				lampOk = a >= 0 && a == shaderInstanceGetUniformLocation(gProgramLamp.vertexShader, name);
			}
			if (gMvpShader) {
				gProgramFastReady = true;
				gProgramSkinReady = skinOk;
				gProgramFastSunReady = sunOk;
				gProgramLampReady = lampOk;
				if (!skinOk) shaderProgramFree(&gProgramSkin);
				if (sunEntries && !sunOk) shaderProgramFree(&gProgramFastSun);
				if (sunEntries && !lampOk) shaderProgramFree(&gProgramLamp);
			} else {
				shaderProgramFree(&gProgram);
				shaderProgramFree(&gProgramFast);
				shaderProgramFree(&gProgramSkin);
				if (sunEntries) shaderProgramFree(&gProgramFastSun);
				if (sunEntries) shaderProgramFree(&gProgramLamp);
				DVLB_Free(gDvlb);
				gDvlb = nullptr;
			}
		} else if (gDvlb) {
			DVLB_Free(gDvlb);
			gDvlb = nullptr;
		}
	}
	if (!gMvpShader) {
		gDvlb = gPlainShader  ? DVLB_ParseFile(reinterpret_cast<u32*>(world_plain_shbin), world_plain_shbin_size)
		        : gFadeShader ? DVLB_ParseFile(reinterpret_cast<u32*>(world_fade_shbin), world_fade_shbin_size)
		                      : DVLB_ParseFile(reinterpret_cast<u32*>(world_shbin), world_shbin_size);
		if (!gDvlb) return false;
		shaderProgramInit(&gProgram);
		shaderProgramSetVsh(&gProgram, &gDvlb->DVLE[0]);
	}
	gProgramReady = true;
	C3D_BindProgram(&gProgram);

	gLocProjection = gLocModelView = gLocMvp = gLocFogRow = gLocLightK = gLocAoK = -1;
	if (gMvpShader) {
		gLocMvp = shaderInstanceGetUniformLocation(gProgram.vertexShader, "mvp");
		gLocFogRow = shaderInstanceGetUniformLocation(gProgram.vertexShader, "fogRow");
		gLocLightK = shaderInstanceGetUniformLocation(gProgram.vertexShader, "lightK");
		gLocAoK = shaderInstanceGetUniformLocation(gProgram.vertexShader, "aoK");
		if (gLocMvp < 0 || gLocFogRow < 0 || gLocLightK < 0 || gLocAoK < 0) return false;
		gLocProjection = gLocModelView = 0; // (stand-ins for the check below; the matrices go to mvp)
	} else {
		gLocProjection = shaderInstanceGetUniformLocation(gProgram.vertexShader, "projection");
		gLocModelView = shaderInstanceGetUniformLocation(gProgram.vertexShader, "modelView");
	}
	gLocChunk = shaderInstanceGetUniformLocation(gProgram.vertexShader, "chunk");
	gLocParams = shaderInstanceGetUniformLocation(gProgram.vertexShader, "params");
	gLocDayDelta = shaderInstanceGetUniformLocation(gProgram.vertexShader, "dayDelta");
	gLocTexOffset = shaderInstanceGetUniformLocation(gProgram.vertexShader, "texOffset");
	gLocSkinLight = gLocBones = gLocSkinned = -1;
	if (!gPlainShader) {
		gLocSkinLight = shaderInstanceGetUniformLocation(gProgram.vertexShader, "skinLight");
		gLocBones = shaderInstanceGetUniformLocation(gProgram.vertexShader, "bones");
		gLocSkinned = shaderInstanceGetUniformLocation(gProgram.vertexShader, "skinned");
		if (gLocSkinLight < 0 || gLocBones < 0 || gLocSkinned < 0) return false;
		gLocFx = shaderInstanceGetUniformLocation(gProgram.vertexShader, "fx");
		gLocWindy = shaderInstanceGetUniformLocation(gProgram.vertexShader, "windy");
		if (gLocFx < 0 || gLocWindy < 0) return false;
		// (Optional: a shader without them draws the water flat.)
		gLocWave = shaderInstanceGetUniformLocation(gProgram.vertexShader, "wave");
		gLocWavy = shaderInstanceGetUniformLocation(gProgram.vertexShader, "wavy");
		if (gLocWave < 0 || gLocWavy < 0 || !gMvpShader) gLocWave = gLocWavy = -1;
		if (gFadeShader) {
			gLocFading = shaderInstanceGetUniformLocation(gProgram.vertexShader, "fading");
			if (gLocFading < 0) return false;
			makeNoise(); // (without it nothing is dissolved: see fadeSupported)
		}
	}
	if (gLocProjection < 0 || gLocModelView < 0 || gLocChunk < 0 || gLocParams < 0 || gLocDayDelta < 0 || gLocTexOffset < 0)
		return false;

	// Matches WorldVertex: short4 position, short2 uv, ubyte4 colour.
	C3D_AttrInfo* attr = C3D_GetAttrInfo();
	AttrInfo_Init(attr);
	AttrInfo_AddLoader(attr, 0, GPU_SHORT, 4);
	AttrInfo_AddLoader(attr, 1, GPU_SHORT, 2);
	AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4);

	C3D_CullFace(GPU_CULL_BACK_CCW);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	C3D_AlphaTest(false, GPU_GREATER, 127);
	gAlphaTestOn = false;

	for (WorldVertex*& v : gUiVertices) v = static_cast<WorldVertex*>(linearAlloc(gUiCapacity * 4 * sizeof(WorldVertex)));
	gUiIndices = static_cast<u16*>(linearAlloc(gUiCapacity * 6 * sizeof(u16)));
	if (!gUiVertices[0] || !gUiVertices[1] || !gUiIndices) return false;
	for (u32 q = 0; q < gUiCapacity; ++q) {
		static const u16 corners[6] = {0, 1, 2, 2, 1, 3};
		for (int k = 0; k < 6; ++k) gUiIndices[q * 6 + k] = u16(q * 4 + corners[k]);
	}
	GSPGPU_FlushDataCache(gUiIndices, gUiCapacity * 6 * sizeof(u16));
	gUiBuffer = 0;
	return true;
}

void shutdown() {
	// Without a working citro3d there is nothing to wait for or to tear down
	// (and C3D_FrameSync would wait for a VBlank callback that was never installed).
	if (gReady) {
		// Deleting the target waits for the GPU's queue: the last frame is
		// through, and only then are its buffers safe to free. (No waiting
		// for the display here - leaving must not depend on anything that
		// might not come.)
		if (gTargetR) C3D_RenderTargetDelete(gTargetR);
		if (gTarget) C3D_RenderTargetDelete(gTarget);
		reap();
		// The command writer still points into one of the buffers freed next.
		GPUCMD_SetBuffer(nullptr, 0, 0);
		for (u32*& b : gOwnBuffer) {
			if (b) linearFree(b);
			b = nullptr;
		}
		// As every citro3d program ends: the library first, then what it used.
		C3D_Fini();
		if (gNoiseReady) C3D_TexDelete(&gNoise);
		gNoiseReady = false;
		if (gSunWhiteReady) C3D_TexDelete(&gSunWhite);
		gSunWhiteReady = false;
		gSunBound = nullptr;
		gUnit1 = nullptr;
		gSunChunk = nullptr;
		gLocFading = gLocFx = gLocWindy = gLocWave = gLocWavy = -1;
		if (gProgramReady) shaderProgramFree(&gProgram);
		if (gProgramFastReady) shaderProgramFree(&gProgramFast);
		if (gProgramSkinReady) shaderProgramFree(&gProgramSkin);
		if (gProgramFastSunReady) shaderProgramFree(&gProgramFastSun);
		if (gProgramLampReady) shaderProgramFree(&gProgramLamp);
		if (gDvlb) DVLB_Free(gDvlb);
	}
	// (Only ever allocated after citro3d was up, and freed after it is down:
	// the GPU no longer reads them.)
	for (WorldVertex*& v : gUiVertices) {
		if (v) linearFree(v);
		v = nullptr;
	}
	if (gUiIndices) linearFree(gUiIndices);
	gUiIndices = nullptr;
	free(gDeadTextures.items);
	free(gDeadMeshes.items);
	gDeadTextures = {};
	gDeadMeshes = {};
	gProgramReady = gProgramFastReady = gProgramSkinReady = gProgramFastSunReady = gProgramLampReady = false;
	gLamps = LampSet {};
	gLampsSent = false;
	gMvpShader = false;
	gBound = PROGRAM_FULL;
	gDvlb = nullptr;
	gTarget = nullptr;
	gTargetR = nullptr;
	if (gStereoOn) gfxSet3D(false);
	if (gWide) gfxSetWide(false);
	gWide = false;
	gStereoOn = gStereoFrame = false;
	gSuper = false;
	gSuperLight = false;
	gReady = false;
}

void releaseDestroyed() {
	if (!gReady || gInFrame || (gDeadTextures.count == 0 && gDeadMeshes.count == 0)) return;
	// An empty frame: C3D_FrameBegin returns once the GPU has finished
	// everything submitted before, and ending a frame without draw calls
	// submits nothing.
	if (C3D_FrameBegin(0)) C3D_FrameEnd(GX_CMDLIST_FLUSH);
	reap();
}

Texture* textureCreate(const TexHeader& h, const u8* data) {
	Texture* t = static_cast<Texture*>(calloc(1, sizeof(Texture)));
	if (!t) return nullptr;

	C3D_TexInitParams p;
	p.width = h.width;
	p.height = h.height;
	p.maxLevel = u8(h.mipCount - 1);
	// fix144: a sun's mask (TEX_L4) becomes GPU_A8 here unless sunl4 (see sunA8): the same texel order, a byte a texel.
	const bool mask = h.format == TEX_L4;
	const bool toA8 = mask && sunA8();
	p.format = h.format == TEX_ETC1A4 ? GPU_ETC1A4 : toA8 ? GPU_A8 : mask ? GPU_L4 : GPU_ETC1;
	p.type = GPU_TEX_2D;
	p.onVram = false; // VRAM is only 6 MB; see docs/ROADMAP.md
	if (!C3D_TexInitWithParams(&t->tex, nullptr, p)) {
		// Memory may still be held by resources destroyed a moment ago.
		releaseDestroyed();
		if (!C3D_TexInitWithParams(&t->tex, nullptr, p)) {
			free(t);
			return nullptr;
		}
	}
	// The baker already produced PICA's tile order, so each level is a memcpy.
	u8* wide = nullptr;
	if (toA8) {
		wide = static_cast<u8*>(malloc(u32(h.width) * h.height));
		if (!wide) {
			C3D_TexDelete(&t->tex);
			free(t);
			return nullptr;
		}
	}
	for (u32 level = 0; level < h.mipCount; ++level) {
		const u8* src = data + texLevelOffset(h, level);
		if (toA8) {
			// Nibble n (the low one first) is texel n in the PICA's order (g3ds/tex.h texL4Nibble): byte n of the A8 level.
			const u32 texels = (u32(h.width) >> level) * (u32(h.height) >> level);
			for (u32 n = 0; n < texels; ++n) wide[n] = u8(((src[n >> 1] >> ((n & 1u) * 4u)) & 15u) * 17u);
			src = wide;
		}
		C3D_TexLoadImage(&t->tex, src, GPU_TEXFACE_2D, int(level));
	}
	free(wide);
	C3D_TexFlush(&t->tex);
	const bool nearest = mask && (gSunDebug & SUN_DBG_NEAREST);
	C3D_TexSetFilter(&t->tex, nearest ? GPU_NEAREST : GPU_LINEAR, nearest ? GPU_NEAREST : GPU_LINEAR);
	C3D_TexSetFilterMipmap(&t->tex, GPU_NEAREST);
	if ((h.flags & TEXF_CLAMP) && !(mask && (gSunDebug & SUN_DBG_REPEAT)))
		C3D_TexSetWrap(&t->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	else
		C3D_TexSetWrap(&t->tex, GPU_REPEAT, GPU_REPEAT);

	t->bytes = toA8 ? h.dataSize * 2u : h.dataSize;
	gMem.textureBytes += t->bytes;
	++gMem.textureCount;
	return t;
}

void textureUpdateMask(Texture* t, const u8* texels) {
	if (!t || !texels || !t->tex.data) return;
	const u32 texels0 = u32(t->tex.width) * t->tex.height;
	u8* dst = static_cast<u8*>(t->tex.data); // (level 0 first; a mask has no other)
	if (t->tex.fmt == GPU_A8) {
		memcpy(dst, texels, texels0);
	} else if (t->tex.fmt == GPU_L4) {
		for (u32 n = 0; n + 1 < texels0; n += 2) dst[n >> 1] = u8((texels[n] >> 4) | (texels[n + 1] & 0xF0u));
	} else {
		return;
	}
	C3D_TexFlush(&t->tex);
}

void textureDestroy(Texture* t) {
	if (!t) return;
	// citro3d keeps the pointer of the bound texture and reads it again
	// whenever it re-sends the texture unit (after the HOME menu, for one).
	if (gBoundTexture == t) {
		C3D_TexBind(0, nullptr);
		gBoundTexture = nullptr;
	}
	// A mask on unit 1: the noise (or the white one) takes its place.
	if (gSunBound == &t->tex) bindUnit1Default();
	if (gSunChunk == t) gSunChunk = nullptr;
	if (t->bytes) {
		gMem.textureBytes -= t->bytes;
		--gMem.textureCount;
	}
	if (!bury(gDeadTextures, t)) freeTexture(t);
}

// fix141: the GPU finds a vertex buffer and an index buffer by their offset from the buffers' base address, in 28 bits
// (GPUREG_ATTRIBBUFFERi_OFFSET, GPUREG_INDEXBUFFER_CONFIG; citro3d does not check). citro3d counts from 0x18000000 (VRAM):
// so only up to 0x27FFFFFF, and a buffer of the linear heap above that (FCRAM is 0x20000000-0x2FFFFFFF on a New 3DS)
// would be found 256 MB lower. Every vertex and index buffer here is in the linear heap (FCRAM), never in VRAM, so they are
// counted from FCRAM's start: all of it can be reached. (Not what hung fix133 - the console's linear heap is at
// 0x20000000..0x22A00000 - but a heap placed higher would hit it; kept.)
constexpr u32 kBufferBase = 0x20000000u;
static void initBufInfo(C3D_BufInfo* info) {
	BufInfo_Init(info);
	info->base_paddr = kBufferBase;
}

Mesh* meshCreate(const WorldVertex* vertices, u32 vertexCount, const u16* indices, u32 indexCount) {
	Mesh* m = static_cast<Mesh*>(calloc(1, sizeof(Mesh)));
	if (!m) return nullptr;
	const u32 vbytes = vertexCount * u32(sizeof(WorldVertex));
	const u32 ibytes = indexCount * u32(sizeof(u16));
	m->vertices = static_cast<WorldVertex*>(linearAlloc(vbytes));
	m->indices = static_cast<u16*>(linearAlloc(ibytes));
	if (!m->vertices || !m->indices) {
		// Memory may still be held by resources destroyed a moment ago.
		releaseDestroyed();
		if (!m->vertices) m->vertices = static_cast<WorldVertex*>(linearAlloc(vbytes));
		if (!m->indices) m->indices = static_cast<u16*>(linearAlloc(ibytes));
	}
	if (!m->vertices || !m->indices) {
		if (m->vertices) linearFree(m->vertices);
		if (m->indices) linearFree(m->indices);
		free(m);
		return nullptr;
	}
	memcpy(m->vertices, vertices, vbytes);
	memcpy(m->indices, indices, ibytes);
	// The GPU reads these through physical memory; push them out of the CPU cache.
	GSPGPU_FlushDataCache(m->vertices, vbytes);
	GSPGPU_FlushDataCache(m->indices, ibytes);

	initBufInfo(&m->buf);
	BufInfo_Add(&m->buf, m->vertices, sizeof(WorldVertex), 3, 0x210);
	m->vertexCount = vertexCount;
	m->indexCount = indexCount;
	gMem.meshBytes += vbytes + ibytes;
	++gMem.meshCount;
	return m;
}

void meshUpdate(Mesh* m, const WorldVertex* vertices, u32 vertexCount) {
	if (!m || !vertices) return;
	if (vertexCount > m->vertexCount) vertexCount = m->vertexCount;
	const u32 bytes = vertexCount * u32(sizeof(WorldVertex));
	memcpy(m->vertices, vertices, bytes);
	GSPGPU_FlushDataCache(m->vertices, bytes);
}

void meshDestroy(Mesh* m) {
	if (!m) return;
	if (gBoundMesh == m) gBoundMesh = nullptr;
	gMem.meshBytes -= m->vertexCount * u32(sizeof(WorldVertex)) + m->indexCount * u32(sizeof(u16));
	--gMem.meshCount;
	if (!bury(gDeadMeshes, m)) freeMesh(m);
}

// The display sync of a frame: everything the player sees is paced by the
// time between two of these.
static bool gTopOnly = false;
void c3dSyncTopOnly(bool on) { gTopOnly = on; }

// C3D_FrameBegin with the wait for the display: for the top screen alone when it runs faster than the bottom one.
static bool frameBeginSynced() {
	if (!gTopOnly) return C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	const u32 start = C3D_FrameCounter(0);
	do {
		gspWaitForAnyEvent();
	} while (C3D_FrameCounter(0) == start);
	return C3D_FrameBegin(0);
}

static void markSync() {
	if (gSyncStarted) {
		osTickCounterUpdate(&gSyncClock);
		gFrameStats.frameMs = float(osTickCounterRead(&gSyncClock));
	} else {
		osTickCounterStart(&gSyncClock);
		gSyncStarted = true;
		gFrameStats.frameMs = 0.0f;
	}
}

static void beginPassState(const FrameDesc& f, C3D_RenderTarget* target);

// The top screen's two framebuffers, as libctru turns them: gfxGetFramebuffer gives the one the next picture goes into,
// so the other is the one on show (as reproject_3ds.cpp finds them, from what it has seen).
static u8* gTopSeen[2] = {nullptr, nullptr};
static void noteTopBuffer() {
	u8* b = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
	if (!b || b == gTopSeen[0] || b == gTopSeen[1]) return;
	if (!gTopSeen[0]) {
		gTopSeen[0] = b;
	} else if (!gTopSeen[1]) {
		gTopSeen[1] = b;
	} else {
		gTopSeen[0] = gTopSeen[1];
		gTopSeen[1] = b;
	}
}

bool captureFrame(u16* rgb565, int w, int h) {
	if (!gReady || gInFrame || !rgb565 || w <= 0 || h <= 0) return false;
	noteTopBuffer();
	u16 fbw = 0, fbh = 0;
	const u8* back = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbw, &fbh);
	const u8* shown = back == gTopSeen[0] ? gTopSeen[1] : back == gTopSeen[1] ? gTopSeen[0] : nullptr;
	if (!shown || fbw != 240 || (fbh != 400 && fbh != 800)) return false;
	u32 bpp = 0;
	switch (gfxGetScreenFormat(GFX_TOP)) {
	case GSP_RGBA8_OES: bpp = 4; break;
	case GSP_BGR8_OES: bpp = 3; break;
	case GSP_RGB565_OES: bpp = 2; break;
	default: return false;
	}
	// (The display-transfer unit wrote it behind the processor's cache.)
	GSPGPU_InvalidateDataCache(shown, u32(fbw) * fbh * bpp);
	// The screen lies on its side in memory: a column of the picture after the other, each from the bottom up.
	shrinkPicture(int(fbh), int(fbw), [&](int x, int y, u8* c) {
		const u8* p = shown + (size_t(x) * fbw + size_t(fbw - 1 - y)) * bpp;
		if (bpp == 4) { // A B G R
			c[0] = p[3];
			c[1] = p[2];
			c[2] = p[1];
		} else if (bpp == 3) { // B G R
			c[0] = p[2];
			c[1] = p[1];
			c[2] = p[0];
		} else {
			const u32 v = u32(p[0]) | (u32(p[1]) << 8);
			c[0] = u8(((v >> 11) & 31u) << 3);
			c[1] = u8(((v >> 5) & 63u) << 2);
			c[2] = u8((v & 31u) << 3);
		}
	}, rgb565, w, h);
	return true;
}

void frameBegin(const FrameDesc& f) {
	noteTopBuffer();
	gFrame = f;
	gClearColor = (u32(f.clearColor[0]) << 24) | (u32(f.clearColor[1]) << 16) | (u32(f.clearColor[2]) << 8) | 0xFFu;
	if (gClassic) {
		g3::trace("gfx: waiting for the GPU and the display (classic)");
		gInFrame = frameBeginSynced();
		if (!gInFrame) return;
		markSync();
		reap(); // the previous frame is off the GPU now
		C3D_RenderTargetClear(gTarget, C3D_CLEAR_ALL, gClearColor, 0);
		C3D_FrameDrawOn(gTarget);
	} else {
		// Overlapped: the GPU may still be drawing the previous frame from
		// the other buffer. Nothing here touches the GPU or waits for it; the
		// commands are only written down, and frameEnd() submits them.
		GPUCMD_SetBuffer(gOwnBuffer[gOwnIndex], kOwnBufferWords, 0);
		gInFrame = true;
		// What C3D_FrameDrawOn does, minus its "inside a citro3d frame" check.
		C3D_SetFrameBuf(&gTarget->frameBuf);
		C3D_SetViewport(0, 0, gTarget->frameBuf.width, gTarget->frameBuf.height);
	}
	osTickCounterStart(&gCpuClock);
	gStereoFrame = gStereoOn && gTargetR != nullptr;
	gUiEyeBase = 0;
	gProgramSwitches = 0;
	gLampDraws = 0;
	gDropped = 0;
	gUiUsed = 0;
	beginPassState(f, gTarget);
}

// Everything a pass (one eye of a frame) starts from: the state is set down in the command list afresh, as the
// first draw of the frame would find it.
// setDrawOnce: the stencil, cleared to 0 with the depth at the start of each eye's picture. Each shape takes the next
// value: a pixel is drawn where the stencil is not yet that value, and the drawing sets it.
u8 gOnceRef = 0;
bool gOnceOn = false;

static void endDrawOnce() {
	if (!gOnceOn) return;
	gOnceOn = false;
	C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0x00);
	C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP);
}

void setDrawOnce(bool on) {
	if (!gInFrame || gUiPass) return;
	if (!on || gPass != PASS_BLEND) {
		endDrawOnce();
		return;
	}
	gOnceRef = u8(gOnceRef >= 255 ? 1 : gOnceRef + 1);
	gOnceOn = true;
	C3D_StencilTest(true, GPU_NOTEQUAL, gOnceRef, 0xFF, 0xFF);
	C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);
}

static void beginPassState(const FrameDesc& f, C3D_RenderTarget* target) {
	gOnceOn = true; // (whatever the last frame left: off)
	endDrawOnce();
	gOnceRef = 0;
	// The picture squeezed into half the screen: the same triangles and draws, half the pixels. (A scissor did not do
	// this: the GPU still worked on what it cut away.)
	if (gFillTest) C3D_SetViewport(0, 0, target->frameBuf.width, target->frameBuf.height / 2);
	C3D_BindProgram(&gProgram);
	gBound = PROGRAM_FULL;

	// Mtx_PerspTilt handles the sideways screen and PICA's -1..0 depth range.
	C3D_Mtx proj;
	Mtx_PerspTilt(&proj, f.fovY, C3D_AspectRatioTop, f.zNear, f.zFar, true);
	// One view matrix for the whole frame; chunks only differ in `chunk`.
	const EyeFrame ef = eyeFrame(f.view, f.eye);
	memcpy(gAnchor, ef.anchor, sizeof(gAnchor));
	gLampsSent = false; // (a new anchor; and no lamps until the world asks for them)
	gLamps = LampSet {};
	C3D_Mtx mv;
	toC3D(ef.view, &mv);
	setMatrices(proj, mv, true);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocChunk, 0.0f, 0.0f, 0.0f, 1.0f);
	gParams[0] = gParams[1] = gParams[2] = gParams[3] = -1.0f; // unknown: the first chunk sends them
	if (!gPlainShader) C3D_BoolUnifSet(GPU_VERTEX_SHADER, gLocSkinned, false);
	gSkinned = false;
	// No Graphics+ until a chunk asks for it.
	if (gLocFx >= 0) {
		gFx[0] = gFx[1] = gFx[2] = gFx[3] = 0.0f;
		C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocFx, 0.0f, 0.0f, 0.0f, 0.0f);
		if (gMvpShader) C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocAoK, 0.0f, 0.0f, 0.0f, 0.0f);
		C3D_BoolUnifSet(GPU_VERTEX_SHADER, gLocWindy, false);
	}
	gWindy = gMaterialWind = gChunkWind = false;
	if (gLocWave >= 0) {
		gWave[0] = gWave[1] = gWave[2] = gWave[3] = 0.0f;
		C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocWave, 0.0f, 0.0f, 0.0f, 0.0f);
		C3D_BoolUnifSet(GPU_VERTEX_SHADER, gLocWavy, false);
	}
	gWavy = gMaterialWater = gChunkWave = gChunkWaterGrid = false;

	// Start every frame from a known state; from here on only changes are sent.
	setStage0(true, 255);
	setSunStages();
	gSunChunk = nullptr;
	gSunUvSent = false;
	setFogStage(false);
	setDayDelta(f.dayLight[0] - 1.0f, f.dayLight[1] - 1.0f, f.dayLight[2] - 1.0f);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocTexOffset, 0.0f, 0.0f, 0.0f, 0.0f);
	gTexOffset[0] = gTexOffset[1] = 0.0f;
	gPass = PASS_SKY; // force setPass to apply the world state
	setPass(PASS_WORLD);
	C3D_AlphaTest(false, GPU_GREATER, 127);
	gAlphaTest = gAlphaTestOn = false;
	// Not dissolving.
	if (gNoiseReady) {
		C3D_BoolUnifSet(GPU_VERTEX_SHADER, gLocFading, false);
		setFadeStages(false, 255);
	}
	// The noise stays bound to unit 1: citro3d cannot unbind units 1 and 2
	// (C3D_TexBind reads the type of the texture it is given, null or not -
	// that crashed the console). No texture stage reads it unless dissolving.
	bindUnit1Default();
	gFade = 255;
	gBlend = BLEND_UNSET;
	setBlend(BLEND_OFF);
	C3D_TexBind(0, nullptr); // nothing bound until the first textured material
	gBoundTexture = nullptr;
	gBoundMesh = nullptr;
	gUiPass = false;
}

// The second eye of a stereo frame: the same again, drawn from the other side into the right target.
void eyeBegin(const FrameDesc& f) {
	if (!gInFrame || !gStereoFrame || !gTargetR) return;
	gFrame = f;
	gUiEyeBase = gUiUsed;
	if (gClassic) {
		C3D_RenderTargetClear(gTargetR, C3D_CLEAR_ALL, gClearColor, 0);
		C3D_FrameDrawOn(gTargetR);
	} else {
		C3D_SetFrameBuf(&gTargetR->frameBuf);
		C3D_SetViewport(0, 0, gTargetR->frameBuf.width, gTargetR->frameBuf.height);
	}
	beginPassState(f, gTargetR);
}

bool stereoAvailable() { return gReady && gTargetR != nullptr; }
bool stereoActive() { return gStereoOn && gTargetR != nullptr; }

// The top screen to 3D mode and back; between frames only.
void stereoSet(bool on) {
	if (!gReady || gInFrame || on == gStereoOn) return;
	if (on && !gTargetR) return;
	gStereoOn = on;
	gfxSet3D(on);
}

void setPass(Pass pass) {
	if (!gInFrame || gUiPass || pass == gPass) return;
	setFade(1.0f); // (dissolving is for the opaque world only)
	endDrawOnce();
	gPass = pass;
	gSunChunk = nullptr; // (each chunk says again)
	updateSun();
	if (pass == PASS_SKY) {
		C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
		C3D_CullFace(GPU_CULL_NONE);
	} else if (pass == PASS_BLEND) {
		// Tested against the opaque world, but not written: translucent
		// surfaces must not hide each other.
		C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
		C3D_CullFace(GPU_CULL_NONE);
	} else {
		C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
		C3D_CullFace(GPU_CULL_BACK_CCW);
	}
	if (pass != PASS_BLEND) setBlend(BLEND_OFF);
}

// uv scale and fog terms: the same for nearly every draw call of a pass, so
// only sent when they change.
static void setParams(float uvScale) {
	// Fog factor = clamp(fogA - viewZ * fogB); the sky is never fogged.
	const float range = gFrame.fogEnd - gFrame.fogStart;
	float fogA = 1.0f, fogB = 0.0f;
	if (gPass != PASS_SKY && range > 0.0f) {
		fogB = 1.0f / range;
		fogA = gFrame.fogEnd * fogB;
	}
	const float params[4] = {uvScale, 1.0f / 255.0f, fogA, fogB};
	if (memcmp(params, gParams, sizeof(params)) != 0) {
		memcpy(gParams, params, sizeof(params));
		C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocParams, params[0], params[1], params[2], params[3]);
		setFogRow(fogA, fogB);
	}
}

static void setSkinned(bool on) {
	if (on == gSkinned || gPlainShader) return;
	gSkinned = on;
	C3D_BoolUnifSet(GPU_VERTEX_SHADER, gLocSkinned, on);
	updateWindy(); // (a character never sways)
	updateSun();   // (nor takes the ground's shadow)
}

void setChunkTransform(const float origin[3], float posScale, float uvScale, const ChunkFx* fx) {
	if (!gInFrame) return;
	// Lattice positions minus a lattice point: exact, here and in the shader.
	C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocChunk, origin[0] - gAnchor[0], origin[1] - gAnchor[1], origin[2] - gAnchor[2],
	              posScale);
	setParams(uvScale);
	// The shader's wave peaks at 1/4: four times the sway reaches it.
	const ChunkFx none;
	const ChunkFx& f = fx ? *fx : none;
	setFxUniform(f.windX * 4.0f, f.windZ * 4.0f, f.windPhase, f.occlusion);
	gChunkWind = f.windX != 0.0f || f.windZ != 0.0f;
	setWaveUniform(f.waveA, f.waveB, f.waveHeight, f.waveShade);
	gChunkWave = f.waveHeight != 0.0f;
	gChunkWaterGrid = f.waterGrid && gLocWavy >= 0;
	updateWindy();
	// The sun's mask: its place relative to the anchor (u = (anchor + p - x0) * scale).
	gChunkSunDark = f.sunDark >= 0.0f ? clamp(f.sunDark, 0.0f, 1.0f) : gSunDark;
	gSunChunk = f.sun && gProgramFastSunReady && gChunkSunDark > 0.0f ? f.sun : nullptr;
	if (gSunChunk) {
		const float uv[4] = {f.sunScaleX, f.sunScaleZ, (gAnchor[0] - f.sunX0) * f.sunScaleX, (gAnchor[2] - f.sunZ0) * f.sunScaleZ};
		if (!gSunUvSent || memcmp(uv, gSunUv, sizeof(uv)) != 0) {
			memcpy(gSunUv, uv, sizeof(uv));
			gSunUvSent = true;
			sendSunUniforms();
		}
	}
	updateSun();
}

void setSunShadow(float darkness) { gSunDark = clamp(darkness, 0.0f, 1.0f); }

void setLamps(const LampSet* lamps) {
	LampSet next {};
	next.count = lamps ? min(lamps->count, MAX_DRAW_LAMPS) : 0u;
	if (next.count) {
		next.dayKeep = lamps->dayKeep;
		for (u32 k = 0; k < next.count; ++k) next.lamp[k] = lamps->lamp[k];
	}
	gLamps = next; // (sent with the first draw that takes them, if the rows do not hold them already)
}
bool lampsSupported() { return gProgramLampReady && !gNoLampProgram; }
bool sunShadowSupported() { return gProgramFastSunReady; }

void setMaterial(Texture* t, u32 materialFlags, float opacity, float uvOffsetU, float uvOffsetV) {
	if (!gInFrame) return;
	// (fix150: the sky's own layers - the Barrier - blend too, by their material)
	const bool blended = gPass == PASS_BLEND || (gPass == PASS_SKY && (materialFlags & MATF_TRANSLUCENT) != 0);
	const bool textured = t != nullptr;
	const u8 alpha = blended ? u8(clamp(opacity, 0.0f, 1.0f) * 255.0f + 0.5f) : u8(255);
	if (textured != gTextured || alpha != gOpacity) setStage0(textured, alpha);
	if (t && t != gBoundTexture) {
		C3D_TexBind(0, &t->tex);
		gBoundTexture = t;
	}
	// Blended materials are never cut out: their alpha is the blend factor.
	gAlphaTest = !blended && (materialFlags & MATF_ALPHA_TEST) != 0;
	applyAlphaTest();
	const bool additive = blended && (materialFlags & MATF_ADD) != 0;
	if (blended) setBlend(additive ? BLEND_ADD : BLEND_ALPHA);
	else if (gPass == PASS_SKY) setBlend(BLEND_OFF);
	if (additive != gFogToBlack) setFogStage(additive);
	setTexOffset(uvOffsetU, uvOffsetV);
	gMaterialFlags = u16(materialFlags);
	gMaterialWind = (materialFlags & MATF_WIND) != 0;
	gMaterialWater = (materialFlags & MATF_WATER) != 0;
	updateWindy();
	gSunMaterialOk = (materialFlags & (MATF_ALPHA_TEST | MATF_WIND | MATF_ADD | MATF_WATER)) == 0;
	updateSun();
}

bool fadeSupported() { return gNoiseReady; }

void setTwoSided(bool both) {
	if (!gInFrame || gPass != PASS_WORLD) return;
	C3D_CullFace(both ? GPU_CULL_NONE : GPU_CULL_BACK_CCW);
}

void setFade(float visible) {
	if (!gInFrame) return;
	u8 share = u8(clamp(visible, 0.0f, 1.0f) * 255.0f + 0.5f);
	if (!fadeSupported() || gPass != PASS_WORLD) share = 255;
	if (share == gFade) return;
	const bool was = gFade != 255, on = share != 255;
	gFade = share;
	updateSun(); // (dissolving: no mask, see useProgram)
	if (on && gSunBound) bindUnit1Default(); // (the noise back on unit 1 for the dissolve)
	if (on != was) {
		C3D_BoolUnifSet(GPU_VERTEX_SHADER, gLocFading, on);
		setFadeStages(on, share);
	} else {
		C3D_TexEnvColor(C3D_GetTexEnv(5), u32(share) << 24);
	}
	applyAlphaTest();
}

void draw(Mesh* m, u32 firstIndex, u32 indexCount) {
	if (!gInFrame || !m || indexCount == 0 || firstIndex + indexCount > m->indexCount) return;
	// A full command buffer makes libctru stop the program. Dropping the rest
	// of the frame's geometry is ugly but survivable, and it is reported.
	if (gpuCmdBufOffset + kDrawReserveWords > gpuCmdBufSize) {
		++gDropped;
		return;
	}
	if (gMaterialFlags & gDebugSkipFlags) return;
	setSkinned(false);
	// What sways is the full shader's (its vertices carry the sway in w, which the fast one does not know).
	// (So does the water that is waving: the height comes from the shader.)
	// (And water cut for waves, still or not: its vertices' w carries the weight of the waves.)
	useProgram(gDebugProgram == 1 ? true : gDebugProgram == 2 ? false : !gMaterialWind && !gWavy && !(gMaterialWater && gChunkWaterGrid));
	if (m != gBoundMesh) {
		C3D_SetBufInfo(&m->buf);
		gBoundMesh = m;
	}
	C3D_DrawElements(GPU_TRIANGLES, int(indexCount), C3D_UNSIGNED_SHORT, m->indices + firstIndex);
	if (gDoubleDraws && gpuCmdBufOffset + kDrawReserveWords <= gpuCmdBufSize)
		C3D_DrawElements(GPU_TRIANGLES, int(indexCount), C3D_UNSIGNED_SHORT, m->indices + firstIndex);
}

void drawSkinned(Mesh* m, u32 firstIndex, u32 indexCount, const SkinDraw& skin) {
	if (!gInFrame || gPlainShader || !m || indexCount == 0 || firstIndex + indexCount > m->indexCount || !skin.bones ||
	    skin.boneCount == 0 || skin.boneCount > MODEL_PALETTE)
		return;
	// The palette alone is 26 * 3 uniform vectors: more than an ordinary draw call.
	if (gpuCmdBufOffset + kDrawReserveWords + MODEL_PALETTE * 3 * 5 > gpuCmdBufSize) {
		++gDropped;
		return;
	}
	const float off[3] = {skin.origin[0] - gAnchor[0], skin.origin[1] - gAnchor[1], skin.origin[2] - gAnchor[2]};
	setParams(skin.uvScale);
	gLampsSent = false; // (the palette goes where the lamps were)
	C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocSkinLight, skin.light[0], skin.light[1], skin.light[2], 0.0f);
	if (gProgramSkinReady) {
		// The characters' entry: the position step and the origin go into the rows, so that one dph per row
		// gives the place relative to the lattice point (world_skin.v.pica).
		const float s = skin.posScale;
		for (u32 b = 0; b < skin.boneCount; ++b) {
			const float* row = skin.bones + b * 12;
			for (int r = 0; r < 3; ++r)
				C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocBones + int(b) * 3 + r, row[r * 4] * s, row[r * 4 + 1] * s, row[r * 4 + 2] * s,
				              row[r * 4 + 3] + off[r]);
		}
		// (chunk and the `skinned` switch are not read by this entry; they are kept as the full entry has them, for
		// whoever looks at the uniforms - the PC tests - and so that a later full-entry draw finds them in order.)
		C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocChunk, off[0], off[1], off[2], skin.posScale);
		setSkinned(true);
		bindProgram(PROGRAM_SKIN);
		guardUnit1();
	} else {
		C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocChunk, off[0], off[1], off[2], skin.posScale);
		for (u32 b = 0; b < skin.boneCount; ++b) {
			const float* row = skin.bones + b * 12;
			for (int r = 0; r < 3; ++r)
				C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocBones + int(b) * 3 + r, row[r * 4], row[r * 4 + 1], row[r * 4 + 2], row[r * 4 + 3]);
		}
		setSkinned(true);
		useProgram(false);
	}
	if (m != gBoundMesh) {
		C3D_SetBufInfo(&m->buf);
		gBoundMesh = m;
	}
	C3D_DrawElements(GPU_TRIANGLES, int(indexCount), C3D_UNSIGNED_SHORT, m->indices + firstIndex);
}

Texture* uiTextureCreate(u16 w, u16 h, const u8* coverage, bool smooth) {
	const auto pot = [](u32 v) { return v >= 8 && v <= 1024 && (v & (v - 1)) == 0; };
	if (!coverage || !pot(w) || !pot(h)) return nullptr;
	Texture* t = static_cast<Texture*>(calloc(1, sizeof(Texture)));
	if (!t) return nullptr;
	// 4-bit luminance and 4-bit alpha, both the coverage: the same whichever
	// nibble the GPU takes for which.
	if (!C3D_TexInit(&t->tex, w, h, GPU_LA4)) {
		releaseDestroyed();
		if (!C3D_TexInit(&t->tex, w, h, GPU_LA4)) {
			free(t);
			return nullptr;
		}
	}
	u8* texels = static_cast<u8*>(t->tex.data);
	for (u32 y = 0; y < h; ++y) {
		// Rows from the bottom, 8x8 tiles in raster order, Morton order in a tile
		// (as the world's textures are stored, see g3ds/tex.h).
		const u32 sy = h - 1u - y, ty = sy & 7u;
		for (u32 x = 0; x < w; ++x) {
			const u8 c = coverage[y * w + x] >> 4;
			const u32 at = (x & 1u) | ((ty & 1u) << 1) | ((x & 2u) << 1) | ((ty & 2u) << 2) | ((x & 4u) << 2) | ((ty & 4u) << 3);
			texels[((sy >> 3) * (w >> 3) + (x >> 3)) * 64u + at] = u8(c | (c << 4));
		}
	}
	C3D_TexFlush(&t->tex);
	if (smooth) {
		C3D_TexSetFilter(&t->tex, GPU_LINEAR, GPU_LINEAR);
		C3D_TexSetWrap(&t->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	} else {
		C3D_TexSetFilter(&t->tex, GPU_NEAREST, GPU_NEAREST);
		C3D_TexSetWrap(&t->tex, GPU_REPEAT, GPU_REPEAT);
	}
	t->bytes = 0; // (MemStats are the world's and the characters')
	return t;
}

Texture* uiTextureCreateColor(u16 w, u16 h, const u16* rgb565) {
	const auto pot = [](u32 v) { return v >= 8 && v <= 1024 && (v & (v - 1)) == 0; };
	if (!rgb565 || !pot(w) || !pot(h)) return nullptr;
	Texture* t = static_cast<Texture*>(calloc(1, sizeof(Texture)));
	if (!t) return nullptr;
	if (!C3D_TexInit(&t->tex, w, h, GPU_RGB565)) {
		releaseDestroyed();
		if (!C3D_TexInit(&t->tex, w, h, GPU_RGB565)) {
			free(t);
			return nullptr;
		}
	}
	u16* texels = static_cast<u16*>(t->tex.data);
	for (u32 y = 0; y < h; ++y) {
		// (The same order as uiTextureCreate: rows from the bottom, 8x8 tiles, Morton inside.)
		const u32 sy = h - 1u - y, ty = sy & 7u;
		for (u32 x = 0; x < w; ++x) {
			const u32 at = (x & 1u) | ((ty & 1u) << 1) | ((x & 2u) << 1) | ((ty & 2u) << 2) | ((x & 4u) << 2) | ((ty & 4u) << 3);
			texels[((sy >> 3) * (w >> 3) + (x >> 3)) * 64u + at] = rgb565[y * w + x];
		}
	}
	C3D_TexFlush(&t->tex);
	C3D_TexSetFilter(&t->tex, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetWrap(&t->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	t->bytes = 0;
	return t;
}

// (As uiTextureCreateColor, in the GPU's RGBA4: the same 16-bit texels, the same order.)
Texture* uiTextureCreateRGBA4(u16 w, u16 h, const u16* rgba4) {
	const auto pot = [](u32 v) { return v >= 8 && v <= 1024 && (v & (v - 1)) == 0; };
	if (!rgba4 || !pot(w) || !pot(h)) return nullptr;
	Texture* t = static_cast<Texture*>(calloc(1, sizeof(Texture)));
	if (!t) return nullptr;
	if (!C3D_TexInit(&t->tex, w, h, GPU_RGBA4)) {
		releaseDestroyed();
		if (!C3D_TexInit(&t->tex, w, h, GPU_RGBA4)) {
			free(t);
			return nullptr;
		}
	}
	u16* texels = static_cast<u16*>(t->tex.data);
	for (u32 y = 0; y < h; ++y) {
		// (The same order as uiTextureCreate: rows from the bottom, 8x8 tiles, Morton inside.)
		const u32 sy = h - 1u - y, ty = sy & 7u;
		for (u32 x = 0; x < w; ++x) {
			const u32 at = (x & 1u) | ((ty & 1u) << 1) | ((x & 2u) << 1) | ((ty & 2u) << 2) | ((x & 4u) << 2) | ((ty & 4u) << 3);
			texels[((sy >> 3) * (w >> 3) + (x >> 3)) * 64u + at] = rgba4[y * w + x];
		}
	}
	C3D_TexFlush(&t->tex);
	C3D_TexSetFilter(&t->tex, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetWrap(&t->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	t->bytes = 0;
	return t;
}

// The overlay goes through the world's own shader: an orthographic projection
// of the screen in pixels, no view, no chunk offset, no daylight, no fog. The
// vertex alpha is the shader's daylight weight, so a quad's alpha comes from
// the texture stage's constant instead - one draw call per run of equal alpha.
static void beginUi(Texture* t) {
	setFade(1.0f);
	endDrawOnce();
	gUiPass = true;
	gPass = PASS_SKY; // (no fog terms; setPass is shut until the next frame)
	gSunChunk = nullptr;
	updateSun();
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	C3D_CullFace(GPU_CULL_NONE);
	gAlphaTest = false;
	applyAlphaTest();
	setBlend(BLEND_ALPHA);
	if (gFogToBlack) setFogStage(false);
	setSkinned(false);
	useProgram(true);
	setTexOffset(0.0f, 0.0f);
	setFxUniform(0.0f, 0.0f, 0.0f, 0.0f);
	gMaterialWind = gChunkWind = false;
	gMaterialWater = gChunkWave = gChunkWaterGrid = false;
	updateWindy();
	C3D_Mtx proj, identity;
	Mtx_OrthoTilt(&proj, 0.0f, float(SCREEN_W), float(SCREEN_H), 0.0f, -1.0f, 1.0f, true);
	memset(&identity, 0, sizeof(identity));
	identity.r[0].x = identity.r[1].y = identity.r[2].z = identity.r[3].w = 1.0f;
	setMatrices(proj, identity, false);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocChunk, 0.0f, 0.0f, 0.0f, 1.0f);
	setDayDelta(0.0f, 0.0f, 0.0f);
	const float uvScale = t ? 1.0f / (16.0f * float(t->tex.width)) : 0.0f;
	gParams[0] = uvScale;
	gParams[1] = 1.0f / 255.0f;
	gParams[2] = 1.0f;
	gParams[3] = 0.0f;
	C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocParams, gParams[0], gParams[1], gParams[2], gParams[3]);
	setFogRow(1.0f, 0.0f); // (never fogged)
}

void uiDraw(Texture* t, const UiQuad* quads, u32 count) {
	if (!gInFrame || !t || !quads || count == 0) return;
	if (gpuCmdBufOffset + kDrawReserveWords * 2 > gpuCmdBufSize) {
		++gDropped;
		return;
	}
	count = min(count, UI_MAX_QUADS - (gUiUsed - gUiEyeBase));
	if (count == 0) return;
	beginUi(t);
	if (gBoundTexture != t) {
		C3D_TexBind(0, &t->tex);
		gBoundTexture = t;
	}
	const u32 first = gUiUsed;
	WorldVertex* v = gUiVertices[gUiBuffer] + first * 4;
	const float vScale = float(t->tex.width) / float(t->tex.height);
	for (u32 q = 0; q < count; ++q) {
		const UiQuad& in = quads[q];
		const i16 xs[2] = {in.x0, in.x1}, ys[2] = {in.y0, in.y1};
		const u16 us[2] = {in.u0, in.u1}, vs[2] = {in.v0, in.v1};
		for (int c = 0; c < 4; ++c) {
			WorldVertex& o = v[q * 4 + c];
			o.x = xs[c & 1];
			o.y = ys[c >> 1];
			o.z = 0;
			o.w = 0; // (no occlusion, no sway)
			o.u = i16(us[c & 1] * 16);
			o.v = i16(float(vs[c >> 1]) * 16.0f * vScale);
			o.r = in.rgba[0];
			o.g = in.rgba[1];
			o.b = in.rgba[2];
			o.a = 0;
		}
	}
	GSPGPU_FlushDataCache(v, count * 4 * sizeof(WorldVertex));
	gUiUsed += count;
	initBufInfo(&gUiBufInfo);
	BufInfo_Add(&gUiBufInfo, gUiVertices[gUiBuffer], sizeof(WorldVertex), 3, 0x210);
	C3D_SetBufInfo(&gUiBufInfo);
	gBoundMesh = nullptr;
	// Runs of equal alpha.
	for (u32 q = 0; q < count;) {
		u32 end = q + 1;
		while (end < count && quads[end].rgba[3] == quads[q].rgba[3]) ++end;
		if (gpuCmdBufOffset + kDrawReserveWords > gpuCmdBufSize) {
			++gDropped;
			break;
		}
		const u8 alpha = quads[q].rgba[3];
		if (!gTextured || alpha != gOpacity) setStage0(true, alpha);
		C3D_DrawElements(GPU_TRIANGLES, int((end - q) * 6), C3D_UNSIGNED_SHORT, gUiIndices + (first + q) * 6);
		q = end;
	}
}

void frameEnd() {
	if (!gInFrame) return;
	gInFrame = false;
	gUiBuffer ^= 1; // the GPU reads this frame's quads; the next frame writes the other buffer
	osTickCounterUpdate(&gCpuClock);
	gFrameStats.cpuMs = float(osTickCounterRead(&gCpuClock));
	gFrameStats.drawsDropped = gDropped;
	gFrameStats.programSwitches = gProgramSwitches;
	gFrameStats.lampDraws = gLampDraws;
	gFrameStats.overlapped = !gClassic;
	if (gClassic) {
		// With flags = 0 citro3d flushes the whole linear heap out of the CPU
		// cache here. Measured on a New 3DS that costs next to nothing (the
		// frame time equalled CPU + GPU time to a quarter of a millisecond),
		// so the safe default stays.
		gStereoFrame = false;
		C3D_FrameEnd(0);
		gFrameStats.commandBufferUse = C3D_GetCmdBufUsage();
		gFrameStats.gpuMs = C3D_GetDrawingTime(); // of the frame before
		return;
	}

	// Overlapped: the frame is written down; now wait for the display and for
	// the GPU to finish the previous one, then hand this one over.
	const u32 recorded = gpuCmdBufOffset;
	gFrameStats.commandBufferUse = float(recorded) / float(kOwnBufferWords);
	GPUCMD_SetBuffer(nullptr, 0, 0);
	g3::trace("gfx: waiting for the GPU (the frame before) and the display");
	if (!frameBeginSynced()) return; // cannot happen without C3D_FRAME_NONBLOCK; the frame is dropped
	g3::trace("gfx: submitting the frame");
	markSync();
	gFrameStats.gpuMs = C3D_GetDrawingTime(); // of the frame the GPU has just finished
	// The GPU is idle: whatever was destroyed while it drew can go. The frame
	// about to be submitted was recorded after those were destroyed.
	reap();
	C3D_RenderTargetClear(gTarget, C3D_CLEAR_ALL, gClearColor, 0);
	gTarget->used = true; // C3D_FrameEnd copies used targets to the screen
	if (gStereoFrame && gTargetR) {
		C3D_RenderTargetClear(gTargetR, C3D_CLEAR_ALL, gClearColor, 0);
		gTargetR->used = true;
	}
	gStereoFrame = false;
	// C3D_FrameBegin pointed the command writer at citro3d's own (empty)
	// buffer; point it back at the end of what was recorded.
	GPUCMD_SetBuffer(gOwnBuffer[gOwnIndex], kOwnBufferWords, recorded);
	C3D_FrameEnd(0);
	gOwnIndex ^= 1; // the GPU reads this buffer now; the next frame is written into the other
}

void c3dSetStereo(bool on) {
	if (!gReady) gStereoWanted = on; // (only before init(): the target and the buffers are made there)
}

void c3dSetClassicFrames(bool classic) {
	if (!gReady) gClassic = classic;
}

void c3dSetFillTest(bool on) { gFillTest = on; }
void c3dSetNoLamps(bool on) { gNoLampProgram = on; }
void c3dSetNoSun(bool on) {
	gNoSun = on;
	gNoSunProgram = on;
}
void c3dSetSunDebug(unsigned flags) {
	if (!gReady) gSunDebug = flags; // (the masks' format and the stages are chosen at init and at load)
}
void c3dSetDebugDraw(u16 skipFlags, int program) {
	gDebugSkipFlags = skipFlags;
	gDebugProgram = program;
}

void c3dSetWide(bool on) {
	if (!gReady) gWideWanted = on; // (only before init(): the targets are made there)
}

bool c3dWide() { return gWide; }

void c3dSetSupersample(bool on) {
	if (!gReady) {
		gSuperWanted = on; // (only before init(): the target is not rebuilt while frames are in flight)
		gSuperLightWanted = false;
	}
}

void c3dSetSupersampleMode(int mode) {
	if (!gReady) {
		gSuperWanted = mode != 0;
		gSuperLightWanted = mode == 1;
	}
}

bool c3dSupersampleLight() { return gSuperLight; }

bool c3dSupersample() { return gSuper; }
void c3dSetDoubleDraws(bool on) { gDoubleDraws = on; }

void c3dSetPlainShader(bool plain) {
	if (!gReady) gPlainShader = plain;
}

bool c3dPlainShader() { return gPlainShader; }

void c3dSetClassicShader(bool classic) {
	if (!gReady) gClassicShader = classic;
}

bool c3dFastShader() { return gMvpShader; }

void c3dSetFadeShader(bool fade) {
	if (!gReady) gFadeShader = fade;
}

MemStats memStats() { return gMem; }
FrameStats frameStats() { return gFrameStats; }

} // namespace gfx
} // namespace g3
