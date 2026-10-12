// A stand-in for libctru, just enough to run platform/3ds/gfx_c3d.cpp on the
// PC. It draws nothing. It models two things:
//  * ownership over time: which command buffer the GPU is executing, which
//    memory that buffer refers to, and when the GPU is done - recording every
//    violation of those rules;
//  * the vertex shader: the assembled shader binary is parsed and its
//    bytecode can be run on the vertices and uniforms of any draw call.
// See tests/mock3ds/mock3ds.h and tests/test_frames.cpp.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;

#define BIT(n) (1U << (n))

extern u32* gpuCmdBuf;
extern u32 gpuCmdBufSize;   // words
extern u32 gpuCmdBufOffset; // words

static inline void GPUCMD_SetBuffer(u32* adr, u32 size, u32 offset) {
	gpuCmdBuf = adr;
	gpuCmdBufSize = size;
	gpuCmdBufOffset = offset;
}

void* linearAlloc(size_t size);
void linearFree(void* mem);
u32 linearSpaceFree(void);
int GSPGPU_FlushDataCache(const void* adr, u32 size);

struct TickCounter {
	double elapsed, reference;
};
void osTickCounterStart(TickCounter* c);
void osTickCounterUpdate(TickCounter* c);
double osTickCounterRead(const TickCounter* c);

enum { GX_CMDLIST_FLUSH = BIT(1) };
enum { GX_TRANSFER_FMT_RGBA8 = 0, GX_TRANSFER_FMT_RGB8 = 1, GX_TRANSFER_SCALE_NO = 0 };
#define GX_TRANSFER_FLIP_VERT(x) ((x) << 0)
#define GX_TRANSFER_OUT_TILED(x) ((x) << 1)
#define GX_TRANSFER_RAW_COPY(x) ((x) << 3)
#define GX_TRANSFER_IN_FORMAT(x) ((x) << 8)
#define GX_TRANSFER_OUT_FORMAT(x) ((x) << 12)
#define GX_TRANSFER_SCALING(x) ((x) << 24)

enum gfxScreen_t { GFX_TOP = 0, GFX_BOTTOM = 1 };
enum gfx3dSide_t { GFX_LEFT = 0, GFX_RIGHT = 1 };

enum GPU_SHADER_TYPE { GPU_VERTEX_SHADER = 0, GPU_GEOMETRY_SHADER = 1 };
enum GPU_TEXCOLOR { GPU_LA8 = 0x5, GPU_A8 = 0x8, GPU_LA4 = 0x9, GPU_ETC1 = 0xC, GPU_ETC1A4 = 0xD };
enum GPU_TEXTURE_MODE_PARAM { GPU_TEX_2D = 0 };
enum GPU_TEXFACE { GPU_TEXFACE_2D = 0 };
enum GPU_TEXTURE_FILTER_PARAM { GPU_NEAREST = 0, GPU_LINEAR = 1 };
enum GPU_TEXTURE_WRAP_PARAM { GPU_CLAMP_TO_EDGE = 0, GPU_REPEAT = 2 };
enum GPU_TESTFUNC { GPU_ALWAYS = 1, GPU_GREATER = 6 };
enum GPU_WRITEMASK { GPU_WRITE_COLOR = 0x0F, GPU_WRITE_ALL = 0x1F };
enum GPU_SCISSORMODE { GPU_SCISSOR_DISABLE = 0, GPU_SCISSOR_INVERT = 1, GPU_SCISSOR_NORMAL = 3 };
enum GPU_CULLMODE { GPU_CULL_NONE = 0, GPU_CULL_BACK_CCW = 2 };
enum GPU_BLENDEQUATION { GPU_BLEND_ADD = 0 };
enum GPU_BLENDFACTOR { GPU_ZERO = 0, GPU_ONE = 1, GPU_SRC_ALPHA = 6, GPU_ONE_MINUS_SRC_ALPHA = 7 };
enum GPU_TEVSRC { GPU_PRIMARY_COLOR = 0, GPU_TEXTURE0 = 3, GPU_TEXTURE1 = 4, GPU_CONSTANT = 14, GPU_PREVIOUS = 15 };
enum GPU_TEVOP_RGB { GPU_TEVOP_RGB_SRC_COLOR = 0, GPU_TEVOP_RGB_SRC_ALPHA = 2 };
enum GPU_COMBINEFUNC { GPU_REPLACE = 0, GPU_MODULATE = 1, GPU_ADD_SIGNED = 3, GPU_INTERPOLATE = 4, GPU_SUBTRACT = 5 };
enum GPU_FORMATS { GPU_BYTE = 0, GPU_UNSIGNED_BYTE = 1, GPU_SHORT = 2, GPU_FLOAT = 3 };
enum GPU_Primitive_t { GPU_TRIANGLES = 0 };
enum GPU_COLORBUF { GPU_RB_RGBA8 = 0 };
enum GPU_DEPTHBUF { GPU_RB_DEPTH24_STENCIL8 = 3 };

struct DVLE_s {
	int unused;
};
struct DVLB_s {
	DVLE_s* DVLE;
};
struct shaderInstance_s {
	int unused;
};
struct shaderProgram_s {
	shaderInstance_s* vertexShader;
};
DVLB_s* DVLB_ParseFile(u32* shbinData, u32 shbinSize);
void DVLB_Free(DVLB_s* dvlb);
int shaderProgramInit(shaderProgram_s* sp);
int shaderProgramFree(shaderProgram_s* sp);
int shaderProgramSetVsh(shaderProgram_s* sp, DVLE_s* dvle);
s32 shaderInstanceGetUniformLocation(const shaderInstance_s* si, const char* name);
