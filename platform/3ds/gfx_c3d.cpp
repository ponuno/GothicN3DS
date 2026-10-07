// PICA200 backend (libctru + citro3d).
//
// Fixed pipeline for world geometry:
//   vertex shader  world.v.pica (dequantise, transform, per-vertex linear fog)
//   TexEnv 0       texture * baked vertex light, alpha from the texture
//   TexEnv 1       lerp towards the fog colour by the fog factor (vertex alpha)
//   alpha test     only for cut-out materials
// Vertex and index buffers live in the linear heap and are used in place.
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
int gLocProjection = -1, gLocModelView = -1, gLocParams = -1, gLocDayDelta = -1;
Pass gPass = PASS_WORLD;

FrameDesc gFrame;
Texture* gBoundTexture = nullptr;
bool gTextured = true;
bool gAlphaTest = false;
bool gInFrame = false;
MemStats gMem;

// Engine matrices are column-major; C3D_Mtx stores rows.
void toC3D(const Mat4& m, C3D_Mtx* out) {
	for (int r = 0; r < 4; ++r) {
		out->r[r].x = m.at(r, 0);
		out->r[r].y = m.at(r, 1);
		out->r[r].z = m.at(r, 2);
		out->r[r].w = m.at(r, 3);
	}
}

void setStage0(bool textured) {
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	if (textured) {
		C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
		C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	} else {
		C3D_TexEnvSrc(env, C3D_RGB, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
		C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
		C3D_TexEnvColor(env, 0xFFFFFFFFu);
	}
	gTextured = textured;
}

void setFogStage(const u8 color[3]) {
	// out = previous * fog + fogColour * (1 - fog), fog = primary alpha
	C3D_TexEnv* env = C3D_GetTexEnv(1);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
	C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
	C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
	C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
	C3D_TexEnvColor(env, u32(color[0]) | (u32(color[1]) << 8) | (u32(color[2]) << 16) | 0xFF000000u);
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
	if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) return false;

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
	if (gLocProjection < 0 || gLocModelView < 0 || gLocParams < 0 || gLocDayDelta < 0) return false;

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
	C3D_FrameSync(); // let the last frame finish before its buffers go away
	reap();
	free(gDeadTextures.items);
	free(gDeadMeshes.items);
	gDeadTextures = {};
	gDeadMeshes = {};
	if (gProgramReady) shaderProgramFree(&gProgram);
	if (gDvlb) DVLB_Free(gDvlb);
	if (gTarget) C3D_RenderTargetDelete(gTarget);
	gProgramReady = false;
	gDvlb = nullptr;
	gTarget = nullptr;
	C3D_Fini();
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
		free(t);
		return nullptr;
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
	if (gBoundTexture == t) gBoundTexture = nullptr;
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

	setStage0(true);
	setFogStage(f.fogColor);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocDayDelta, f.dayLight[0] - 1.0f, f.dayLight[1] - 1.0f, f.dayLight[2] - 1.0f, 0.0f);
	gPass = PASS_SKY; // force setPass to apply the world state
	setPass(PASS_WORLD);
	C3D_AlphaTest(false, GPU_GREATER, 127);
	gAlphaTest = false;
	gBoundTexture = nullptr;
}

void setPass(Pass pass) {
	if (!gInFrame || pass == gPass) return;
	gPass = pass;
	if (pass == PASS_SKY) {
		C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
		C3D_CullFace(GPU_CULL_NONE);
	} else {
		C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
		C3D_CullFace(GPU_CULL_BACK_CCW);
	}
}

void setChunkTransform(const float origin[3], float posScale, float uvScale) {
	if (!gInFrame) return;
	C3D_Mtx mv;
	toC3D(mul(gFrame.view, translateScale(origin, posScale)), &mv);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, gLocModelView, &mv);
	// Fog factor = clamp(fogA - viewZ * fogB); the sky is never fogged.
	const float range = gFrame.fogEnd - gFrame.fogStart;
	float fogA = 1.0f, fogB = 0.0f;
	if (gPass == PASS_WORLD && range > 0.0f) {
		fogB = 1.0f / range;
		fogA = gFrame.fogEnd * fogB;
	}
	C3D_FVUnifSet(GPU_VERTEX_SHADER, gLocParams, uvScale, 1.0f / 255.0f, fogA, fogB);
}

void setMaterial(Texture* t, u32 materialFlags) {
	if (!gInFrame) return;
	const bool textured = t != nullptr;
	if (textured != gTextured) setStage0(textured);
	if (t && t != gBoundTexture) {
		C3D_TexBind(0, &t->tex);
		gBoundTexture = t;
	}
	const bool alphaTest = (materialFlags & MATF_ALPHA_TEST) != 0;
	if (alphaTest != gAlphaTest) {
		C3D_AlphaTest(alphaTest, GPU_GREATER, 127);
		gAlphaTest = alphaTest;
	}
}

void draw(Mesh* m, u32 firstIndex, u32 indexCount) {
	if (!gInFrame || !m || firstIndex + indexCount > m->indexCount) return;
	C3D_SetBufInfo(&m->buf);
	C3D_DrawElements(GPU_TRIANGLES, int(indexCount), C3D_UNSIGNED_SHORT, m->indices + firstIndex);
}

void frameEnd() {
	// With flags = 0 citro3d flushes the whole linear heap every frame. Since
	// every buffer and texture is already flushed on creation, passing
	// GX_CMDLIST_FLUSH here should be a free win - try it once this runs on
	// hardware (docs/ROADMAP.md).
	if (gInFrame) C3D_FrameEnd(0);
	gInFrame = false;
}

MemStats memStats() { return gMem; }

} // namespace gfx
} // namespace g3
