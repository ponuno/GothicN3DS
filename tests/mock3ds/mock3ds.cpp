#include <cstdio>
#include <cstdlib>
#include "mock3ds.h"

#include <citro3d.h>

#include <map>
#include <math.h>
#include <set>
#include <stdlib.h>
#include <string>
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
const int kMaxEntries = 4; // DVLEs of one shader binary
DVLE_s gDvle[kMaxEntries];
DVLB_s gDvlb {gDvle};
shaderInstance_s gShader[kMaxEntries];
int gBoundEntry = 0; // the DVLE of the program C3D_BindProgram bound last

// ---- the shader binary, as picasso wrote it ----

struct ShaderUniform {
	std::string name;
	int start, end; // registers as in the file: 0x10.. float, 0x70.. int, 0x78.. bool
};
struct ShaderOutput {
	int type, reg, mask;
};
// One DVLE: an entry point of the shared code, with its own tables.
struct ShaderEntry {
	u32 main = 0, endMain = 0;
	std::vector<ShaderUniform> uniforms;
	std::vector<ShaderOutput> outputs;
	float constants[96][4] = {};
	bool isConstant[96] = {};
	u16 boolConstants = 0;
};
struct Shader {
	bool loaded = false;
	std::vector<u32> code, opdesc;
	ShaderEntry entry[kMaxEntries];
	int entries = 0;
	std::string problem;
};
Shader gSh;
float gFloatUniforms[96][4];
u16 gBoolUniforms = 0;
mock3ds::DrawHook gDrawHook = nullptr;
void* gDrawHookUser = nullptr;

float fromFloat24(u32 v) {
	const u32 mantissa = v & 0xFFFF, exponent = (v >> 16) & 0x7F;
	if (exponent == 0 && mantissa == 0) return 0.0f;
	const float value = ldexpf(1.0f + float(mantissa) / 65536.0f, int(exponent) - 63);
	return (v >> 23) & 1 ? -value : value;
}

// Seventeen significant bits, cut off (not rounded): the pessimistic reading.
float toGpuFloat(float v) {
	if (v == 0.0f || v != v) return v;
	int e;
	const float m = frexpf(v, &e); // 0.5 <= |m| < 1
	return ldexpf(truncf(m * 131072.0f) / 131072.0f, e);
}

void parseShader(const u32* file, u32 bytes) {
	gSh = Shader {};
	const u32 words = bytes / 4;
	auto bad = [&](const char* what) { gSh.problem = what; };
	if (words < 16 || file[0] != 0x424C5644u) return bad("not a DVLB file");
	if (file[1] < 1 || file[1] > u32(kMaxEntries)) return bad("expected one to four DVLEs (vertex shaders)");
	const u32 entries = file[1];
	const u32* dvlp = file + 2 + entries;
	if (dvlp[0] != 0x504C5644u) return bad("no DVLP block");
	if (dvlp[2] / 4 + dvlp[3] > words || dvlp[4] / 4 + dvlp[5] * 2 > words) return bad("DVLP tables outside the file");
	gSh.code.assign(dvlp + dvlp[2] / 4, dvlp + dvlp[2] / 4 + dvlp[3]);
	for (u32 i = 0; i < dvlp[5]; ++i) gSh.opdesc.push_back(dvlp[dvlp[4] / 4 + i * 2]);
	for (u32 e = 0; e < entries; ++e) {
		ShaderEntry& en = gSh.entry[e];
		if (file[2 + e] / 4 + 16 > words) return bad("DVLE outside the file");
		const u32* dvle = file + file[2 + e] / 4;
		const u32 left = words - file[2 + e] / 4;
		if (dvle[0] != 0x454C5644u) return bad("no DVLE block");
		if (((dvle[1] >> 16) & 0xFF) != 0) return bad("not a vertex shader");
		en.main = dvle[2];
		en.endMain = dvle[3];
		if (en.main >= gSh.code.size() || en.endMain > gSh.code.size()) return bad("entry point outside the code");
		if (dvle[6] / 4 + dvle[7] * 5 > left || dvle[10] / 4 + dvle[11] * 2 > left || dvle[12] / 4 + dvle[13] * 2 > left ||
		    dvle[14] / 4 > left)
			return bad("DVLE tables outside the file");
		// Constants: { u16 type, u16 register, u32 data[4] }.
		for (u32 i = 0; i < dvle[7]; ++i) {
			const u32* c = dvle + dvle[6] / 4 + i * 5;
			const u32 type = c[0] & 0xFFFF, id = c[0] >> 16;
			if (type == 0) {
				if (id < 16 && (c[1] & 1)) en.boolConstants = u16(en.boolConstants | (1u << id));
			} else if (type == 2) {
				if (id >= 96) return bad("float constant outside the registers");
				en.isConstant[id] = true;
				for (int k = 0; k < 4; ++k) en.constants[id][k] = fromFloat24(c[1 + k]);
			}
		}
		// Outputs: { u16 type, u16 register, u8 mask, u8 pad[3] }.
		for (u32 i = 0; i < dvle[11]; ++i) {
			const u32* o = dvle + dvle[10] / 4 + i * 2;
			en.outputs.push_back({int(o[0] & 0xFFFF), int(o[0] >> 16), int(o[1] & 0xF)});
		}
		// Uniforms: { u32 symbol offset, u16 first register, u16 last register }.
		const char* symbols = reinterpret_cast<const char*>(dvle + dvle[14] / 4);
		const size_t symbolBytes = size_t(left - dvle[14] / 4) * 4;
		for (u32 i = 0; i < dvle[13]; ++i) {
			const u32* u = dvle + dvle[12] / 4 + i * 2;
			if (u[0] >= symbolBytes || !memchr(symbols + u[0], 0, symbolBytes - u[0])) return bad("uniform name outside the file");
			const ShaderUniform uniform = {symbols + u[0], int(u[1] & 0xFFFF), int(u[1] >> 16)};
			if (uniform.start >= 0x10 && uniform.start < 0x70) {
				if (uniform.end > 0x6F || uniform.end < uniform.start) return bad("float uniform outside the registers");
				for (int r = uniform.start; r <= uniform.end; ++r)
					if (en.isConstant[r - 0x10]) return bad("a uniform and a constant share a register");
			}
			en.uniforms.push_back(uniform);
		}
	}
	gSh.entries = int(entries);
	gSh.loaded = true;
}

// ---- the vertex processor ----

struct Machine {
	int entry; // which DVLE runs
	float in[16][4], tmp[16][4], out[16][4];
	float uniform[96][4];
	u16 bools;
	int address[3]; // a0.x, a0.y, aL
	bool cmp[2];
	bool gpuFloats;
	const char* fault;
};

// A source operand: register, optional index register, swizzle, negation.
bool readSource(Machine& m, u32 reg, u32 index, u32 selector, bool negate, float v[4]) {
	const float* r;
	if (reg < 0x10) {
		r = m.in[reg];
	} else if (reg < 0x20) {
		r = m.tmp[reg - 0x10];
	} else {
		int at = int(reg) - 0x20;
		if (index) at += m.address[index - 1]; // only uniforms can be indexed
		if (at < 0 || at >= 96) {
			m.fault = "a uniform was read outside c0..c95";
			return false;
		}
		r = m.uniform[at];
	}
	for (int i = 0; i < 4; ++i) {
		const float c = r[(selector >> (6 - 2 * i)) & 3];
		v[i] = negate ? -c : c;
	}
	return true;
}

void writeDest(Machine& m, u32 reg, u32 mask, const float v[4]) {
	float* r = reg < 0x10 ? m.out[reg] : m.tmp[reg - 0x10];
	for (int i = 0; i < 4; ++i)
		if (mask & (8u >> i)) r[i] = m.gpuFloats ? toGpuFloat(v[i]) : v[i];
}

bool compare(u32 op, float a, float b) {
	switch (op) {
	case 0: return a == b;
	case 1: return a != b;
	case 2: return a < b;
	case 3: return a <= b;
	case 4: return a > b;
	case 5: return a >= b;
	default: return true;
	}
}

bool run(Machine& m) {
	struct Block {
		u32 end, next;
	};
	std::vector<Block> stack;
	u32 pc = gSh.entry[m.entry].main;
	for (unsigned steps = 0; steps < 4096; ++steps) {
		while (!stack.empty() && pc == stack.back().end) {
			pc = stack.back().next;
			stack.pop_back();
		}
		if (pc >= gSh.code.size()) {
			m.fault = "ran past the end of the code";
			return false;
		}
		const u32 instr = gSh.code[pc];
		const u32 op = instr >> 26;
		if (op == 0x22) return true; // END
		if (op == 0x21) { // NOP
			++pc;
			continue;
		}
		// Flow control: bits 25..22 hold the bool register or the condition,
		// 21..10 the target, 7..0 a count.
		if (op >= 0x24 && op <= 0x2D && op != 0x29 && op != 0x2A && op != 0x2B) {
			const u32 target = (instr >> 10) & 0xFFF, count = instr & 0xFF;
			const bool flag = (m.bools >> ((instr >> 22) & 0xF)) & 1;
			const bool refX = (instr >> 25) & 1, refY = (instr >> 24) & 1;
			bool cond;
			switch ((instr >> 22) & 3) {
			case 0: cond = m.cmp[0] == refX || m.cmp[1] == refY; break;
			case 1: cond = m.cmp[0] == refX && m.cmp[1] == refY; break;
			case 2: cond = m.cmp[0] == refX; break;
			default: cond = m.cmp[1] == refY; break;
			}
			if (stack.size() > 16) {
				m.fault = "calls nested too deep";
				return false;
			}
			switch (op) {
			case 0x24: // CALL
			case 0x25: // CALLC
			case 0x26: // CALLU
				if (op == 0x24 || (op == 0x25 ? cond : flag)) {
					stack.push_back({target + count, pc + 1});
					pc = target;
				} else {
					++pc;
				}
				break;
			case 0x27: // IFU: the block up to `target`, or the `count` instructions from there
			case 0x28: // IFC
				if (op == 0x27 ? flag : cond) {
					stack.push_back({target, target + count});
					++pc;
				} else {
					stack.push_back({target + count, target + count});
					pc = target;
				}
				break;
			case 0x2C: // JMPC
				pc = cond ? target : pc + 1;
				break;
			default: // JMPU: bit 0 of the count inverts the test
				pc = flag == !(count & 1) ? target : pc + 1;
				break;
			}
			continue;
		}
		float a[4] = {0, 0, 0, 0}, b[4] = {0, 0, 0, 0}, c[4], r[4];
		if (op >= 0x38) { // MAD: dst = src1 * src2 + src3
			const u32 d = gSh.opdesc.size() > (instr & 0x1F) ? gSh.opdesc[instr & 0x1F] : 0;
			if (!readSource(m, (instr >> 17) & 0x1F, 0, (d >> 5) & 0xFF, (d >> 4) & 1, a) ||
			    !readSource(m, (instr >> 10) & 0x7F, (instr >> 22) & 3, (d >> 14) & 0xFF, (d >> 13) & 1, b) ||
			    !readSource(m, (instr >> 5) & 0x1F, 0, (d >> 23) & 0xFF, (d >> 22) & 1, c))
				return false;
			for (int i = 0; i < 4; ++i) r[i] = a[i] * b[i] + c[i];
			writeDest(m, (instr >> 24) & 0x1F, d & 0xF, r);
			++pc;
			continue;
		}
		if ((instr & 0x7F) >= gSh.opdesc.size()) {
			m.fault = "operand descriptor outside the table";
			return false;
		}
		const u32 d = gSh.opdesc[instr & 0x7F];
		const u32 index = (instr >> 19) & 3;
		if (op == 0x2E || op == 0x2F) { // CMP
			if (!readSource(m, (instr >> 12) & 0x7F, index, (d >> 5) & 0xFF, (d >> 4) & 1, a) ||
			    !readSource(m, (instr >> 7) & 0x1F, 0, (d >> 14) & 0xFF, (d >> 13) & 1, b))
				return false;
			m.cmp[0] = compare((instr >> 24) & 7, a[0], b[0]);
			m.cmp[1] = compare((instr >> 21) & 7, a[1], b[1]);
			++pc;
			continue;
		}
		const bool inverted = op >= 0x18 && op <= 0x1B; // the wide register is the second operand
		const bool unary = op == 0x05 || op == 0x06 || op == 0x0B || op == 0x0E || op == 0x0F || op == 0x12 || op == 0x13;
		if (inverted) {
			if (!readSource(m, (instr >> 14) & 0x1F, 0, (d >> 5) & 0xFF, (d >> 4) & 1, a) ||
			    !readSource(m, (instr >> 7) & 0x7F, index, (d >> 14) & 0xFF, (d >> 13) & 1, b))
				return false;
		} else {
			if (!readSource(m, (instr >> 12) & 0x7F, index, (d >> 5) & 0xFF, (d >> 4) & 1, a)) return false;
			if (!unary && !readSource(m, (instr >> 7) & 0x1F, 0, (d >> 14) & 0xFF, (d >> 13) & 1, b)) return false;
		}
		const u32 dest = (instr >> 21) & 0x1F, mask = d & 0xF;
		switch (op) {
		case 0x00: // ADD
			for (int i = 0; i < 4; ++i) r[i] = a[i] + b[i];
			break;
		case 0x01: // DP3
			r[0] = r[1] = r[2] = r[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
			break;
		case 0x02: // DP4
			r[0] = r[1] = r[2] = r[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
			break;
		case 0x03: // DPH
		case 0x18:
			r[0] = r[1] = r[2] = r[3] = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + b[3];
			break;
		case 0x08: // MUL
			for (int i = 0; i < 4; ++i) r[i] = a[i] * b[i];
			break;
		case 0x09: // SGE
		case 0x1A:
			for (int i = 0; i < 4; ++i) r[i] = a[i] >= b[i] ? 1.0f : 0.0f;
			break;
		case 0x0A: // SLT
		case 0x1B:
			for (int i = 0; i < 4; ++i) r[i] = a[i] < b[i] ? 1.0f : 0.0f;
			break;
		case 0x0B: // FLR
			for (int i = 0; i < 4; ++i) r[i] = floorf(a[i]);
			break;
		case 0x0C: // MAX
			for (int i = 0; i < 4; ++i) r[i] = a[i] > b[i] ? a[i] : b[i];
			break;
		case 0x0D: // MIN
			for (int i = 0; i < 4; ++i) r[i] = a[i] < b[i] ? a[i] : b[i];
			break;
		case 0x0E: // RCP
			r[0] = r[1] = r[2] = r[3] = 1.0f / a[0];
			break;
		case 0x0F: // RSQ
			r[0] = r[1] = r[2] = r[3] = 1.0f / sqrtf(a[0]);
			break;
		case 0x12: // MOVA: the address registers take whole numbers
			if (mask & 8) m.address[0] = int(a[0]);
			if (mask & 4) m.address[1] = int(a[1]);
			for (int i = 0; i < 2; ++i)
				if (m.address[i] < -128 || m.address[i] > 127) {
					m.fault = "an address register left its 8 bits";
					return false;
				}
			++pc;
			continue;
		case 0x13: // MOV
			for (int i = 0; i < 4; ++i) r[i] = a[i];
			break;
		default:
			m.fault = "an instruction the interpreter does not know";
			return false;
		}
		writeDest(m, dest, mask, r);
		++pc;
	}
	m.fault = "the shader did not come to an end";
	return false;
}

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
	gBoundEntry = 0;
	gCount = Counters {};
	gBuffersSeen.clear();
}
double nowMs() { return gNow; }
void setLinearLimit(size_t bytes) { gLinearLimit = bytes; }

void setDrawHook(DrawHook hook, void* user) {
	gDrawHook = hook;
	gDrawHookUser = user;
}

const char* shaderProblem() { return gSh.loaded ? "" : (gSh.problem.empty() ? "no shader loaded" : gSh.problem.c_str()); }

unsigned shaderFloatRegisters() {
	bool used[96] = {};
	for (const ShaderUniform& u : gSh.entry[0].uniforms)
		for (int r = u.start; r <= u.end && r < 0x70; ++r)
			if (r >= 0x10) used[r - 0x10] = true;
	unsigned n = 0;
	for (int i = 0; i < 96; ++i) n += used[i] || gSh.entry[0].isConstant[i];
	return n;
}

unsigned shaderInstructions() { return unsigned(gSh.code.size()); }
unsigned shaderEntries() { return unsigned(gSh.entries); }
unsigned shaderEntryInstructions(unsigned entry) {
	return entry < unsigned(gSh.entries) ? gSh.entry[entry].endMain - gSh.entry[entry].main : 0;
}

bool shadeVertex(const DrawCall& call, unsigned index, ShadedVertex& out, bool gpuFloats) {
	out = ShadedVertex {};
	if (!gSh.loaded || !call.vertices || call.stride == 0) return false;
	if (call.entry < 0 || call.entry >= gSh.entries) return false;
	const ShaderEntry& en = gSh.entry[call.entry];
	Machine m {};
	m.entry = call.entry;
	m.gpuFloats = gpuFloats;
	m.bools = u16(call.bools | en.boolConstants);
	for (int i = 0; i < 96; ++i)
		for (int k = 0; k < 4; ++k) {
			const float v = en.isConstant[i] ? en.constants[i][k] : call.uniforms[i][k];
			m.uniform[i][k] = gpuFloats ? toGpuFloat(v) : v;
		}
	// Attributes: the buffer holds them in the order of its permutation; a
	// component the buffer does not have reads as 0, the fourth as 1.
	for (auto& v : m.in) v[3] = 1.0f;
	const u8* p = static_cast<const u8*>(call.vertices) + size_t(index) * call.stride;
	unsigned offset = 0;
	for (int k = 0; k < gBufInfo.attribCount; ++k) {
		const int which = int((gBufInfo.permutation >> (4 * k)) & 0xF);
		if (which >= gAttrInfo.count) return false;
		const auto& loader = gAttrInfo.loader[which];
		static const unsigned kSize[4] = {1, 1, 2, 4};
		const unsigned size = kSize[loader.format & 3];
		offset = (offset + size - 1) & ~(size - 1); // the GPU aligns each attribute to its component size
		if (offset + size * unsigned(loader.components) > call.stride || loader.regId < 0 || loader.regId > 15) return false;
		for (int c = 0; c < loader.components; ++c) {
			float value;
			const u8* at = p + offset + size * unsigned(c);
			switch (loader.format) {
			case GPU_BYTE: value = float(static_cast<int8_t>(*at)); break;
			case GPU_UNSIGNED_BYTE: value = float(*at); break;
			case GPU_SHORT: {
				int16_t v;
				memcpy(&v, at, 2);
				value = float(v);
				break;
			}
			default: memcpy(&value, at, 4); break;
			}
			m.in[loader.regId][c] = value;
		}
		offset += size * unsigned(loader.components);
	}
	if (!run(m)) {
		gSh.problem = m.fault ? m.fault : "the shader failed";
		return false;
	}
	// Outputs by meaning: the set bits of an entry's mask are the components
	// of its semantic, in order.
	for (const ShaderOutput& o : en.outputs) {
		float* target = o.type == 0 ? out.position : o.type == 2 ? out.color : o.type == 3 ? out.texcoord : o.type == 5 ? out.texcoord1 : nullptr;
		const int limit = o.type == 3 || o.type == 5 ? 2 : 4;
		if (!target || o.reg < 0 || o.reg > 15) continue;
		int k = 0;
		for (int j = 0; j < 4 && k < limit; ++j)
			if (o.mask & (1 << j)) target[k++] = m.out[o.reg][j];
	}
	return true;
}
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

DVLB_s* DVLB_ParseFile(u32* shbinData, u32 shbinSize) {
	if (!shbinData || !shbinSize) return nullptr;
	parseShader(shbinData, shbinSize);
	return gSh.loaded ? &gDvlb : nullptr;
}
void DVLB_Free(DVLB_s*) {}
int shaderProgramInit(shaderProgram_s* sp) {
	sp->vertexShader = nullptr;
	return 0;
}
int shaderProgramFree(shaderProgram_s*) { return 0; }
int shaderProgramSetVsh(shaderProgram_s* sp, DVLE_s* dvle) {
	const int e = int(dvle - gDvle);
	sp->vertexShader = &gShader[e >= 0 && e < kMaxEntries ? e : 0];
	return 0;
}
s32 shaderInstanceGetUniformLocation(const shaderInstance_s* si, const char* name) {
	// As libctru does: the first register from the binary's uniform table,
	// less 0x10 - and squeezed through a signed byte.
	if (!si) return -1;
	for (const ShaderUniform& u : gSh.entry[si - gShader].uniforms)
		if (u.name == name) return s32(static_cast<int8_t>(u.start - 0x10));
	return -1;
}

// ---- citro3d ----

// As in citro3d (source/maths/mtx_persptilt.c): the picture is turned a
// quarter (the console's screens are mounted sideways) and depth runs -1..0.
void Mtx_PerspTilt(C3D_Mtx* mtx, float fovx, float invaspect, float nearZ, float farZ, bool isLeftHanded) {
	const float fovxTan = tanf(fovx / 2.0f);
	memset(mtx, 0, sizeof(*mtx));
	mtx->r[0].y = 1.0f / fovxTan;
	mtx->r[1].x = -1.0f / (fovxTan * invaspect);
	mtx->r[2].w = farZ * nearZ / (nearZ - farZ);
	mtx->r[3].z = isLeftHanded ? 1.0f : -1.0f;
	mtx->r[2].z = -mtx->r[3].z * nearZ / (nearZ - farZ);
}

// As in citro3d (source/maths/mtx_orthotilt.c).
void Mtx_OrthoTilt(C3D_Mtx* mtx, float left, float right, float bottom, float top, float nearZ, float farZ, bool isLeftHanded) {
	memset(mtx, 0, sizeof(*mtx));
	mtx->r[0].y = 2.0f / (top - bottom);
	mtx->r[0].w = (bottom + top) / (bottom - top);
	mtx->r[1].x = 2.0f / (left - right);
	mtx->r[1].w = (left + right) / (right - left);
	mtx->r[2].z = isLeftHanded ? 1.0f / (farZ - nearZ) : 1.0f / (nearZ - farZ);
	mtx->r[2].w = 0.5f * (nearZ + farZ) / (nearZ - farZ) - 0.5f;
	mtx->r[3].w = 1.0f;
}

bool C3D_TexInitWithParams(C3D_Tex* tex, void*, C3D_TexInitParams p) {
	tex->width = p.width;
	tex->height = p.height;
	tex->fmt = p.format;
	u32 size = 0;
	for (u32 l = 0; l <= p.maxLevel; ++l) size += (u32(p.width) >> l) * (u32(p.height) >> l) * (p.format == GPU_ETC1 ? 4 : 8) / 8; // (ETC1A4 and A8: a byte a texel)
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
	// As citro3d does: for units 1 and 2 it reads the texture's type first,
	// so a null texture there is a data abort on the console.
	if (unitId > 0 && !tex) {
		fprintf(stderr, "mock3ds: C3D_TexBind(%d, NULL) reads through a null pointer on the console\n", unitId);
		abort();
	}
	if (unitId == 0) gBoundTex = tex;
	if (unitId == 1) ++gCount.texUnit1Binds;
}

bool C3D_Init(size_t cmdBufSize) {
	gCtxWords = u32(cmdBufSize / 4);
	gCtxBuf = static_cast<u32*>(linearAlloc(cmdBufSize));
	gInFrame = false;
	gExecuting = List {};
	gRecording.clear();
	memset(gFloatUniforms, 0, sizeof(gFloatUniforms));
	gBoolUniforms = 0;
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
void C3D_SetScissor(GPU_SCISSORMODE, u32, u32, u32, u32) {}

void C3D_SetBufInfo(C3D_BufInfo* info) { gBufInfo = *info; }
C3D_AttrInfo* C3D_GetAttrInfo(void) { return &gAttrInfo; }
void C3D_BindProgram(shaderProgram_s* program) {
	const int e = program && program->vertexShader ? int(program->vertexShader - gShader) : 0;
	if (e != gBoundEntry) ++gCount.programSwitches;
	gBoundEntry = e;
}
void C3D_FVUnifSet(GPU_SHADER_TYPE, int id, float x, float y, float z, float w) {
	if (id < 0 || id >= 96) {
		gSh.problem = "a float uniform was set outside c0..c95";
		return;
	}
	gFloatUniforms[id][0] = x;
	gFloatUniforms[id][1] = y;
	gFloatUniforms[id][2] = z;
	gFloatUniforms[id][3] = w;
}
void C3D_FVUnifMtx4x4(GPU_SHADER_TYPE type, int id, const C3D_Mtx* mtx) {
	for (int r = 0; r < 4; ++r) C3D_FVUnifSet(type, id + r, mtx->r[r].x, mtx->r[r].y, mtx->r[r].z, mtx->r[r].w);
}
void C3D_BoolUnifSet(GPU_SHADER_TYPE, int id, bool value) {
	id -= 0x68; // as citro3d does
	if (id < 0 || id > 15) {
		gSh.problem = "a bool uniform was set outside b0..b15";
		return;
	}
	gBoolUniforms = value ? u16(gBoolUniforms | (1u << id)) : u16(gBoolUniforms & ~(1u << id));
}
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
	if (gDrawHook) {
		mock3ds::DrawCall call;
		call.vertices = gBufInfo.base;
		call.stride = unsigned(gBufInfo.stride);
		call.indices = static_cast<const u16*>(indices);
		call.count = unsigned(count);
		memcpy(call.uniforms, gFloatUniforms, sizeof(call.uniforms));
		call.bools = gBoolUniforms;
		call.entry = gBoundEntry;
		gDrawHook(call, gDrawHookUser);
	}
}
