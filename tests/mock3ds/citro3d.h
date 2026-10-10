// A stand-in for citro3d; see 3ds.h in this directory.
#pragma once
#include <3ds.h>

#define C3D_DEFAULT_CMDBUF_SIZE 0x40000
#define C3D_AspectRatioTop (400.0f / 240.0f)

struct C3D_FVec {
	float w, z, y, x; // citro3d's order
};
struct C3D_Mtx {
	C3D_FVec r[4];
};
void Mtx_PerspTilt(C3D_Mtx* mtx, float fovy, float aspect, float nearZ, float farZ, bool isLeftHanded);

struct C3D_Tex {
	void* data;
	u32 size;
};
struct C3D_TexInitParams {
	u16 width, height;
	u8 maxLevel;
	GPU_TEXCOLOR format;
	GPU_TEXTURE_MODE_PARAM type;
	bool onVram;
};
bool C3D_TexInitWithParams(C3D_Tex* tex, void* cube, C3D_TexInitParams p);
static inline bool C3D_TexInit(C3D_Tex* tex, u16 width, u16 height, GPU_TEXCOLOR format) {
	return C3D_TexInitWithParams(tex, nullptr, C3D_TexInitParams {width, height, 0, format, GPU_TEX_2D, false});
}
void C3D_TexLoadImage(C3D_Tex* tex, const void* data, GPU_TEXFACE face, int level);
void C3D_TexFlush(C3D_Tex* tex);
void C3D_TexDelete(C3D_Tex* tex);
void C3D_TexBind(int unitId, C3D_Tex* tex);
static inline void C3D_TexSetFilter(C3D_Tex*, GPU_TEXTURE_FILTER_PARAM, GPU_TEXTURE_FILTER_PARAM) {}
static inline void C3D_TexSetFilterMipmap(C3D_Tex*, GPU_TEXTURE_FILTER_PARAM) {}
static inline void C3D_TexSetWrap(C3D_Tex*, GPU_TEXTURE_WRAP_PARAM, GPU_TEXTURE_WRAP_PARAM) {}

struct C3D_FrameBuf {
	void* colorBuf;
	void* depthBuf;
	u16 width, height;
};
struct C3D_RenderTarget {
	C3D_FrameBuf frameBuf;
	bool used;
	bool linked;
};
enum { C3D_FRAME_SYNCDRAW = BIT(0), C3D_FRAME_NONBLOCK = BIT(1) };
enum C3D_ClearBits { C3D_CLEAR_COLOR = BIT(0), C3D_CLEAR_DEPTH = BIT(1), C3D_CLEAR_ALL = 3 };

bool C3D_Init(size_t cmdBufSize);
void C3D_Fini(void);
float C3D_GetCmdBufUsage(void);
float C3D_FrameRate(float fps);
void C3D_FrameSync(void);
bool C3D_FrameBegin(u8 flags);
bool C3D_FrameDrawOn(C3D_RenderTarget* target);
void C3D_FrameEnd(u8 flags);
float C3D_GetDrawingTime(void);
float C3D_GetProcessingTime(void);
C3D_RenderTarget* C3D_RenderTargetCreate(int width, int height, GPU_COLORBUF colorFmt, GPU_DEPTHBUF depthFmt);
void C3D_RenderTargetDelete(C3D_RenderTarget* target);
void C3D_RenderTargetSetOutput(C3D_RenderTarget* target, gfxScreen_t screen, gfx3dSide_t side, u32 transferFlags);
void C3D_FrameBufClear(C3D_FrameBuf* fb, C3D_ClearBits clearBits, u32 clearColor, u32 clearDepth);
static inline void C3D_RenderTargetClear(C3D_RenderTarget* target, C3D_ClearBits clearBits, u32 clearColor, u32 clearDepth) {
	C3D_FrameBufClear(&target->frameBuf, clearBits, clearColor, clearDepth);
}
void C3D_SetFrameBuf(C3D_FrameBuf* fb);
void C3D_SetViewport(u32 x, u32 y, u32 w, u32 h);

struct C3D_BufInfo {
	const void* base;
	int count;
	int stride, attribCount;
	u64 permutation;
};
static inline void BufInfo_Init(C3D_BufInfo* info) {
	info->base = nullptr;
	info->count = 0;
	info->stride = 0;
	info->attribCount = 0;
	info->permutation = 0;
}
static inline int BufInfo_Add(C3D_BufInfo* info, const void* data, ptrdiff_t stride, int attribCount, u64 permutation) {
	info->base = data;
	info->stride = int(stride);
	info->attribCount = attribCount;
	info->permutation = permutation;
	return info->count++;
}
void C3D_SetBufInfo(C3D_BufInfo* info);

struct C3D_AttrInfo {
	int count;
	struct {
		int regId, format, components;
	} loader[12];
};
C3D_AttrInfo* C3D_GetAttrInfo(void);
static inline void AttrInfo_Init(C3D_AttrInfo* info) { info->count = 0; }
static inline int AttrInfo_AddLoader(C3D_AttrInfo* info, int regId, GPU_FORMATS format, int count) {
	if (info->count >= 12) return -1;
	info->loader[info->count] = {regId, int(format), count};
	return info->count++;
}

void C3D_BindProgram(shaderProgram_s* program);
void C3D_FVUnifSet(GPU_SHADER_TYPE type, int id, float x, float y, float z, float w);
void C3D_FVUnifMtx4x4(GPU_SHADER_TYPE type, int id, const C3D_Mtx* mtx);
void C3D_BoolUnifSet(GPU_SHADER_TYPE type, int id, bool value);

struct C3D_TexEnv {
	int unused;
};
enum C3D_TexEnvMode { C3D_RGB = BIT(0), C3D_Alpha = BIT(1), C3D_Both = 3 };
C3D_TexEnv* C3D_GetTexEnv(int id);
static inline void C3D_TexEnvInit(C3D_TexEnv*) {}
static inline void C3D_TexEnvSrc(C3D_TexEnv*, C3D_TexEnvMode, GPU_TEVSRC, GPU_TEVSRC = GPU_PRIMARY_COLOR, GPU_TEVSRC = GPU_PRIMARY_COLOR) {}
static inline void C3D_TexEnvOpRgb(C3D_TexEnv*, GPU_TEVOP_RGB, GPU_TEVOP_RGB = GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB = GPU_TEVOP_RGB_SRC_COLOR) {}
static inline void C3D_TexEnvFunc(C3D_TexEnv*, C3D_TexEnvMode, GPU_COMBINEFUNC) {}
static inline void C3D_TexEnvColor(C3D_TexEnv*, u32) {}

static inline void C3D_DepthTest(bool, GPU_TESTFUNC, GPU_WRITEMASK) {}
static inline void C3D_CullFace(GPU_CULLMODE) {}
static inline void C3D_AlphaTest(bool, GPU_TESTFUNC, int) {}
static inline void C3D_AlphaBlend(GPU_BLENDEQUATION, GPU_BLENDEQUATION, GPU_BLENDFACTOR, GPU_BLENDFACTOR, GPU_BLENDFACTOR, GPU_BLENDFACTOR) {}

enum { C3D_UNSIGNED_BYTE = 0, C3D_UNSIGNED_SHORT = 1 };
void C3D_DrawElements(GPU_Primitive_t primitive, int count, int type, const void* indices);
