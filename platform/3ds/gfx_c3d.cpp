// PICA200 backend (libctru + citro3d).
//
// Fixed pipeline for world geometry:
//   vertex shader  world.v.pica (dequantise, transform, per-vertex linear fog)
//   TexEnv 0       texture * baked vertex light, alpha = texture alpha * opacity
//   TexEnv 1       lerp towards the fog colour by the fog factor (vertex alpha)
//   alpha test     only for cut-out materials
//   blending       off for the opaque passes, per material in PASS_BLEND
// Vertex and index buffers live in the linear heap and are used in place.
//
// Every state setter of citro3d marks a whole register group dirty and the
// next draw call re-sends it, so all state here is cached and only touched
// when it really changes: the GPU command buffer is finite, and overflowing
// it stops the program (libctru calls svcBreak).
#include "gfx.h"

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
	u32 bytes;
};

struct Mesh {
	C3D_BufInfo buf;
	WorldVertex* vertices;
	u16* indices;
	u32 vertexCount, indexCount;
};

namespace {

C3D_RenderTarget* gTarget = nullptr;
DVLB_s* gDvlb = nullptr;
shaderProgram_s gProgram;
bool gProgramReady = false;
bool gReady = false; // C3D_Init succeeded
int gLocProjection = -1, gLocModelView = -1, gLocParams = -1, gLocDayDelta = -1, gLocTexOffset = -1;
Pass gPass = PASS_WORLD;

// 1 MB of command buffer: about 5000 draw calls with a texture change each.
const size_t kCommandBufferBytes = 0x100000;
// Words kept free for one draw call with every state group dirty, plus the
// few commands C3D_FrameEnd appends.
const u32 kDrawReserveWords = 0x400;

enum BlendMode { BLEND_OFF, BLEND_ALPHA, BLEND_ADD, BLEND_UNSET };

FrameDesc gFrame;
Texture* gBoundTexture = nullptr;
Mesh* gBoundMesh = nullptr;
bool gTextured = true;
bool gAlphaTest = false;
bool gInFrame = false;
u8 gOpacity = 255;
bool gFogToBlack = false;
BlendMode gBlend = BLEND_UNSET;
float gTexOffset[2] = {0, 0};
MemStats gMem;
FrameStats gFrameStats = {0, 0.0f};
u32 gDropped = 0;

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

// Stage 1: out = previous * fog + fogColour * (1 - fog), fog = primary alpha.
// Added light fades to black instead, so that it adds nothing in the distance.
void setFogStage(bool toBlack) {
	C3D_TexEnv* env = C3D_GetTexEnv(1);
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
	C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocTexOffset, u, v, 0.0f, 0.0f);
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

bool init() {
	gMem = {};
	gFrameStats = {0, 0.0f};
	if (!C3D_Init(kCommandBufferBytes)) return false;
	gReady = true;

	gTarget = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	if (!gTarget) return false;
	C3D_RenderTargetSetOutput(gTarget, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

	gDvlb = DVLB_ParseFile(reinterpret_cast<u32*>(world_shbin), world_shbin_size);
	if (!gDvlb) return false;
	shaderProgramInit(&gProgram);
	shaderProgramSetVsh(&gProgram, &gDvlb->DVLE[0]);
	gProgramReady = true;
	C3D_BindProgram(&gProgram);

	gLocProjection = shaderInstanceGetUniformLocation(gProgram.vertexShader, "projection");
	gLocModelView = shaderInstanceGetUniformLocation(gProgram.vertexShader, "modelView");
	gLocParams = shaderInstanceGetUniformLocation(gProgram.vertexShader, "params");
	gLocDayDelta = shaderInstanceGetUniformLocation(gProgram.vertexShader, "dayDelta");
	gLocTexOffset = shaderInstanceGetUniformLocation(gProgram.vertexShader, "texOffset");
	if (gLocProjection < 0 || gLocModelView < 0 || gLocParams < 0 || gLocDayDelta < 0 || gLocTexOffset < 0) return false;

	// Matches WorldVertex: short4 position, short2 uv, ubyte4 colour.
	C3D_AttrInfo* attr = C3D_GetAttrInfo();
	AttrInfo_Init(attr);
	AttrInfo_AddLoader(attr, 0, GPU_SHORT, 4);
	AttrInfo_AddLoader(attr, 1, GPU_SHORT, 2);
	AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4);

	C3D_CullFace(GPU_CULL_BACK_CCW);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	C3D_AlphaTest(false, GPU_GREATER, 127);
	return true;
}

void shutdown() {
	// Without a working citro3d there is nothing to wait for or to tear down
	// (and C3D_FrameSync would wait for a VBlank callback that was never installed).
	if (gReady) {
		C3D_FrameSync(); // let the last frame reach the screen
		if (gProgramReady) shaderProgramFree(&gProgram);
		if (gDvlb) DVLB_Free(gDvlb);
		// Deleting the target waits for the GPU's queue; only then are the
		// buffers of the last frame safe to free.
		if (gTarget) C3D_RenderTargetDelete(gTarget);
		reap();
		C3D_Fini();
	}
	free(gDeadTextures.items);
	free(gDeadMeshes.items);
	gDeadTextures = {};
	gDeadMeshes = {};
	gProgramReady = false;
	gDvlb = nullptr;
	gTarget = nullptr;
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
	p.format = h.format == TEX_ETC1A4 ? GPU_ETC1A4 : GPU_ETC1;
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
	for (u32 level = 0; level < h.mipCount; ++level)
		C3D_TexLoadImage(&t->tex, data + texLevelOffset(h, level), GPU_TEXFACE_2D, int(level));
	C3D_TexFlush(&t->tex);
	C3D_TexSetFilter(&t->tex, GPU_LINEAR, GPU_LINEAR);
	C3D_TexSetFilterMipmap(&t->tex, GPU_NEAREST);
	C3D_TexSetWrap(&t->tex, GPU_REPEAT, GPU_REPEAT);

	t->bytes = h.dataSize;
	gMem.textureBytes += t->bytes;
	++gMem.textureCount;
	return t;
}

void textureDestroy(Texture* t) {
	if (!t) return;
	// citro3d keeps the pointer of the bound texture and reads it again
	// whenever it re-sends the texture unit (after the HOME menu, for one).
	if (gBoundTexture == t) {
		C3D_TexBind(0, nullptr);
		gBoundTexture = nullptr;
	}
	gMem.textureBytes -= t->bytes;
	--gMem.textureCount;
	if (!bury(gDeadTextures, t)) freeTexture(t);
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

	BufInfo_Init(&m->buf);
	BufInfo_Add(&m->buf, m->vertices, sizeof(WorldVertex), 3, 0x210);
	m->vertexCount = vertexCount;
	m->indexCount = indexCount;
	gMem.meshBytes += vbytes + ibytes;
	++gMem.meshCount;
	return m;
}

void meshDestroy(Mesh* m) {
	if (!m) return;
	if (gBoundMesh == m) gBoundMesh = nullptr;
	gMem.meshBytes -= m->vertexCount * u32(sizeof(WorldVertex)) + m->indexCount * u32(sizeof(u16));
	--gMem.meshCount;
	if (!bury(gDeadMeshes, m)) freeMesh(m);
}

void frameBegin(const FrameDesc& f) {
	gFrame = f;
	gInFrame = C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	if (!gInFrame) return;
	reap(); // the previous frame is off the GPU now

	const u32 clear = (u32(f.clearColor[0]) << 24) | (u32(f.clearColor[1]) << 16) | (u32(f.clearColor[2]) << 8) | 0xFFu;
	C3D_RenderTargetClear(gTarget, C3D_CLEAR_ALL, clear, 0);
	C3D_FrameDrawOn(gTarget);
	C3D_BindProgram(&gProgram);

	// Mtx_PerspTilt handles the sideways screen and PICA's -1..0 depth range.
	C3D_Mtx proj;
	Mtx_PerspTilt(&proj, f.fovY, C3D_AspectRatioTop, f.zNear, f.zFar, true);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, gLocProjection, &proj);

	// Start every frame from a known state; from here on only changes are sent.
	setStage0(true, 255);
	setFogStage(false);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocDayDelta, f.dayLight[0] - 1.0f, f.dayLight[1] - 1.0f, f.dayLight[2] - 1.0f, 0.0f);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocTexOffset, 0.0f, 0.0f, 0.0f, 0.0f);
	gTexOffset[0] = gTexOffset[1] = 0.0f;
	gPass = PASS_SKY; // force setPass to apply the world state
	setPass(PASS_WORLD);
	C3D_AlphaTest(false, GPU_GREATER, 127);
	gAlphaTest = false;
	gBlend = BLEND_UNSET;
	setBlend(BLEND_OFF);
	C3D_TexBind(0, nullptr); // nothing bound until the first textured material
	gBoundTexture = nullptr;
	gBoundMesh = nullptr;
	gDropped = 0;
}

void setPass(Pass pass) {
	if (!gInFrame || pass == gPass) return;
	gPass = pass;
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

void setChunkTransform(const float origin[3], float posScale, float uvScale) {
	if (!gInFrame) return;
	C3D_Mtx mv;
	toC3D(mul(gFrame.view, translateScale(origin, posScale)), &mv);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, gLocModelView, &mv);
	// Fog factor = clamp(fogA - viewZ * fogB); the sky is never fogged.
	const float range = gFrame.fogEnd - gFrame.fogStart;
	float fogA = 1.0f, fogB = 0.0f;
	if (gPass != PASS_SKY && range > 0.0f) {
		fogB = 1.0f / range;
		fogA = gFrame.fogEnd * fogB;
	}
	C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocParams, uvScale, 1.0f / 255.0f, fogA, fogB);
}

void setMaterial(Texture* t, u32 materialFlags, float opacity, float uvOffsetU, float uvOffsetV) {
	if (!gInFrame) return;
	const bool blended = gPass == PASS_BLEND;
	const bool textured = t != nullptr;
	const u8 alpha = blended ? u8(clamp(opacity, 0.0f, 1.0f) * 255.0f + 0.5f) : u8(255);
	if (textured != gTextured || alpha != gOpacity) setStage0(textured, alpha);
	if (t && t != gBoundTexture) {
		C3D_TexBind(0, &t->tex);
		gBoundTexture = t;
	}
	// Blended materials are never cut out: their alpha is the blend factor.
	const bool alphaTest = !blended && (materialFlags & MATF_ALPHA_TEST) != 0;
	if (alphaTest != gAlphaTest) {
		C3D_AlphaTest(alphaTest, GPU_GREATER, 127);
		gAlphaTest = alphaTest;
	}
	const bool additive = blended && (materialFlags & MATF_ADD) != 0;
	if (blended) setBlend(additive ? BLEND_ADD : BLEND_ALPHA);
	if (additive != gFogToBlack) setFogStage(additive);
	setTexOffset(uvOffsetU, uvOffsetV);
}

void draw(Mesh* m, u32 firstIndex, u32 indexCount) {
	if (!gInFrame || !m || indexCount == 0 || firstIndex + indexCount > m->indexCount) return;
	// A full command buffer makes libctru stop the program. Dropping the rest
	// of the frame's geometry is ugly but survivable, and it is reported.
	if (gpuCmdBufOffset + kDrawReserveWords > gpuCmdBufSize) {
		++gDropped;
		return;
	}
	if (m != gBoundMesh) {
		C3D_SetBufInfo(&m->buf);
		gBoundMesh = m;
	}
	C3D_DrawElements(GPU_TRIANGLES, int(indexCount), C3D_UNSIGNED_SHORT, m->indices + firstIndex);
}

void frameEnd() {
	// With flags = 0 citro3d flushes the whole linear heap every frame. Since
	// every buffer and texture is already flushed on creation, passing
	// GX_CMDLIST_FLUSH here should be a free win - try it once this runs on
	// hardware (docs/ROADMAP.md).
	if (gInFrame) {
		C3D_FrameEnd(0);
		gFrameStats.drawsDropped = gDropped;
		gFrameStats.commandBufferUse = C3D_GetCmdBufUsage();
	}
	gInFrame = false;
}

MemStats memStats() { return gMem; }
FrameStats frameStats() { return gFrameStats; }

} // namespace gfx
} // namespace g3
