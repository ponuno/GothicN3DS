#include "mock3ds.h"

#include <citro3d.h>

#include <map>
#include <set>
#include <stdlib.h>
#include <string.h>
#include <vector>

u32* gpuCmdBuf;
u32 gpuCmdBufSize;
u32 gpuCmdBufOffset;

namespace {

mock3ds::Counters gCount;
double gNow = 0;
size_t gLinearLimit = 0;

std::map<const void*, size_t> gBlocks; // live linear allocations

// A command list: where it is and what memory its draw calls point at.
struct List {
	const u32* base = nullptr;
	u32 words = 0;
	std::set<const void*> refs; // block starts
};
std::map<const u32*, List> gRecording; // by buffer start: lists being written
List gExecuting;                       // the list the GPU is busy with (base == null: idle)
std::set<const u32*> gBuffersSeen;

u32* gCtxBuf = nullptr;
u32 gCtxWords = 0;
bool gInFrame = false;
bool gClearedSinceSubmit = false;
C3D_RenderTarget* gLinked = nullptr;
C3D_Tex* gBoundTex = nullptr;
C3D_BufInfo gBufInfo {};
C3D_AttrInfo gAttrInfo {};
C3D_TexEnv gTexEnv[6];
DVLE_s gDvle;
DVLB_s gDvlb {&gDvle};
shaderInstance_s gShader;

const void* blockOf(const void* p) {
	auto it = gBlocks.upper_bound(p);
	if (it == gBlocks.begin()) return nullptr;
	--it;
	const char* start = static_cast<const char*>(it->first);
	return static_cast<const char*>(p) < start + it->second ? it->first : nullptr;
}

bool gpuBusy() { return gExecuting.base != nullptr; }

// Writes `n` words of commands at the current position, as a draw call would.
void emit(u32 n) {
	if (!gpuCmdBuf || gpuCmdBufOffset + n > gpuCmdBufSize) {
		++gCount.wroteOutside;
		return;
	}
	const u32* at = gpuCmdBuf + gpuCmdBufOffset;
	if (gpuBusy() && at + n > gExecuting.base && at < gExecuting.base + gExecuting.words) ++gCount.wroteBusyBuffer;
	memset(gpuCmdBuf + gpuCmdBufOffset, 0xAB, n * 4);
	gpuCmdBufOffset += n;
}

} // namespace

namespace mock3ds {
Counters& counters() {
	gCount.linearBlocks = unsigned(gBlocks.size());
	gCount.linearBytes = 0;
	for (const auto& kv : gBlocks) gCount.linearBytes += kv.second;
	gCount.distinctBuffers = unsigned(gBuffersSeen.size());
	return gCount;
}
unsigned violations() {
	return gCount.wroteBusyBuffer + gCount.wroteOutside + gCount.freedInUse + gCount.submittedFreed + gCount.submittedBusy +
	       gCount.clearedBusy + gCount.badFree + gCount.drawOutsideFrame + gCount.submittedNotStart +
	       gCount.submittedUncleared;
}
void reset() {
	gCount = Counters {};
	gBuffersSeen.clear();
}
double nowMs() { return gNow; }
void setLinearLimit(size_t bytes) { gLinearLimit = bytes; }
} // namespace mock3ds

// ---- libctru ----

void* linearAlloc(size_t size) {
	if (gLinearLimit) {
		size_t total = size;
		for (const auto& kv : gBlocks) total += kv.second;
		if (total > gLinearLimit) return nullptr;
	}
	void* p = malloc(size ? size : 1);
	if (p) gBlocks[p] = size ? size : 1;
	return p;
}

void linearFree(void* mem) {
	auto it = gBlocks.find(mem);
	if (it == gBlocks.end()) {
		++gCount.badFree;
		return;
	}
	// The GPU reads vertex, index and texture memory while it executes a
	// list; a list still being written will be executed later.
	if (gExecuting.refs.count(mem) || (gExecuting.base && gExecuting.base == mem)) ++gCount.freedInUse;
	for (const auto& kv : gRecording)
		if (kv.second.refs.count(mem)) ++gCount.freedInUse;
	memset(mem, 0xDD, it->second);
	gBlocks.erase(it);
	free(mem);
}

u32 linearSpaceFree(void) { return 16u << 20; }
int GSPGPU_FlushDataCache(const void*, u32) { return 0; }

void osTickCounterStart(TickCounter* c) {
	c->reference = gNow;
	c->elapsed = 0;
}
void osTickCounterUpdate(TickCounter* c) {
	c->elapsed = gNow - c->reference;
	c->reference = gNow;
}
double osTickCounterRead(const TickCounter* c) { return c->elapsed; }

DVLB_s* DVLB_ParseFile(u32* shbinData, u32 shbinSize) { return shbinData && shbinSize ? &gDvlb : nullptr; }
void DVLB_Free(DVLB_s*) {}
int shaderProgramInit(shaderProgram_s* sp) {
	sp->vertexShader = nullptr;
	return 0;
}
int shaderProgramFree(shaderProgram_s*) { return 0; }
int shaderProgramSetVsh(shaderProgram_s* sp, DVLE_s*) {
	sp->vertexShader = &gShader;
	return 0;
}
s32 shaderInstanceGetUniformLocation(const shaderInstance_s* si, const char* name) {
	static const char* names[] = {"projection", "modelView", "chunk", "params", "dayDelta", "texOffset"};
	if (!si) return -1;
	for (int i = 0; i < 6; ++i)
		if (strcmp(names[i], name) == 0) return i * 4;
	return -1;
}

// ---- citro3d ----

void Mtx_PerspTilt(C3D_Mtx* mtx, float, float, float, float, bool) { memset(mtx, 0, sizeof(*mtx)); }

bool C3D_TexInitWithParams(C3D_Tex* tex, void*, C3D_TexInitParams p) {
	u32 size = 0;
	for (u32 l = 0; l <= p.maxLevel; ++l) size += (u32(p.width) >> l) * (u32(p.height) >> l) * (p.format == GPU_ETC1A4 ? 8 : 4) / 8;
	tex->data = linearAlloc(size);
	tex->size = size;
	return tex->data != nullptr;
}
void C3D_TexLoadImage(C3D_Tex* tex, const void* data, GPU_TEXFACE, int level) {
	// Touch both ends: the source must be readable, the destination allocated.
	if (level == 0 && tex->data && tex->size) static_cast<u8*>(tex->data)[0] = static_cast<const u8*>(data)[0];
}
void C3D_TexFlush(C3D_Tex*) {}
void C3D_TexDelete(C3D_Tex* tex) {
	if (gBoundTex == tex) gBoundTex = nullptr;
	linearFree(tex->data);
	tex->data = nullptr;
}
void C3D_TexBind(int unitId, C3D_Tex* tex) {
	if (unitId == 0) gBoundTex = tex;
}

bool C3D_Init(size_t cmdBufSize) {
	gCtxWords = u32(cmdBufSize / 4);
	gCtxBuf = static_cast<u32*>(linearAlloc(cmdBufSize));
	gInFrame = false;
	gExecuting = List {};
	gRecording.clear();
	return gCtxBuf != nullptr;
}
void C3D_Fini(void) {
	gExecuting = List {};
	if (gCtxBuf) linearFree(gCtxBuf);
	gCtxBuf = nullptr;
	GPUCMD_SetBuffer(nullptr, 0, 0);
}
float C3D_GetCmdBufUsage(void) { return 0.1f; }
float C3D_FrameRate(float) { return 60.0f; }
void C3D_FrameSync(void) { gNow += 1000.0 / 60.0; }
float C3D_GetDrawingTime(void) { return 9.0f; }
float C3D_GetProcessingTime(void) { return 4.0f; }

bool C3D_FrameBegin(u8 flags) {
	if (gInFrame) return false;
	if (flags & C3D_FRAME_SYNCDRAW) C3D_FrameSync();
	// Waits for the GPU: whatever it was executing is finished now.
	++gCount.waits;
	gExecuting = List {};
	gInFrame = true;
	GPUCMD_SetBuffer(gCtxBuf, gCtxWords, 0);
	return true;
}

bool C3D_FrameDrawOn(C3D_RenderTarget* target) {
	if (!gInFrame) return false;
	target->used = true;
	return true;
}

void C3D_FrameEnd(u8) {
	if (!gInFrame) return;
	if (gpuCmdBufOffset) {
		if (gpuBusy()) ++gCount.submittedBusy;
		if (!gClearedSinceSubmit) ++gCount.submittedUncleared;
		emit(6); // what citro3d appends: flush the framebuffer, end the list
		List list = gRecording[gpuCmdBuf];
		list.base = gpuCmdBuf;
		list.words = gpuCmdBufOffset;
		gRecording.erase(gpuCmdBuf);
		// The list must start where a buffer starts: a known allocation.
		if (!gBlocks.count(gpuCmdBuf)) ++gCount.submittedNotStart;
		for (const void* ref : list.refs)
			if (!gBlocks.count(ref)) ++gCount.submittedFreed;
		gBuffersSeen.insert(list.base);
		gCount.ownBufferFrames += list.base != gCtxBuf;
		gExecuting = list;
		++gCount.framesSubmitted;
		gClearedSinceSubmit = false;
	}
	if (gLinked && gLinked->used) {
		gLinked->used = false;
		++gCount.framesShown;
	}
	gInFrame = false;
	GPUCMD_SetBuffer(nullptr, 0, 0);
}

C3D_RenderTarget* C3D_RenderTargetCreate(int width, int height, GPU_COLORBUF, GPU_DEPTHBUF) {
	C3D_RenderTarget* t = static_cast<C3D_RenderTarget*>(calloc(1, sizeof(C3D_RenderTarget)));
	if (!t) return nullptr;
	t->frameBuf.width = u16(width);
	t->frameBuf.height = u16(height);
	return t;
}
void C3D_RenderTargetDelete(C3D_RenderTarget* target) {
	gExecuting = List {}; // waits for the GPU
	if (gLinked == target) gLinked = nullptr;
	free(target);
}
void C3D_RenderTargetSetOutput(C3D_RenderTarget* target, gfxScreen_t, gfx3dSide_t, u32) {
	gLinked = target;
	if (target) target->linked = true;
}
void C3D_FrameBufClear(C3D_FrameBuf*, C3D_ClearBits, u32, u32) {
	if (gpuBusy()) ++gCount.clearedBusy;
	gClearedSinceSubmit = true;
}
void C3D_SetFrameBuf(C3D_FrameBuf*) {}
void C3D_SetViewport(u32, u32, u32, u32) {}

void C3D_SetBufInfo(C3D_BufInfo* info) { gBufInfo = *info; }
C3D_AttrInfo* C3D_GetAttrInfo(void) { return &gAttrInfo; }
void C3D_BindProgram(shaderProgram_s*) {}
void C3D_FVUnifSet(GPU_SHADER_TYPE, int, float, float, float, float) {}
void C3D_FVUnifMtx4x4(GPU_SHADER_TYPE, int, const C3D_Mtx*) {}
C3D_TexEnv* C3D_GetTexEnv(int id) { return &gTexEnv[id]; }

void C3D_DrawElements(GPU_Primitive_t, int count, int, const void* indices) {
	++gCount.draws;
	if (!gpuCmdBuf) {
		++gCount.drawOutsideFrame;
		return;
	}
	gCount.drawsWhileBusy += gpuBusy();
	List& list = gRecording[gpuCmdBuf];
	if (gpuCmdBufOffset == 0) list = List {}; // a new list in this buffer
	// The draw call makes the GPU read these, whenever its list is executed.
	const void* refs[3] = {blockOf(gBufInfo.base), blockOf(indices), gBoundTex ? blockOf(gBoundTex->data) : nullptr};
	if (!refs[0] || !refs[1] || (gBoundTex && !refs[2])) ++gCount.submittedFreed; // already gone when recorded
	for (const void* r : refs)
		if (r) list.refs.insert(r);
	// State changes plus the draw itself: a generous 40 words.
	emit(40 + u32(count % 7));
}
