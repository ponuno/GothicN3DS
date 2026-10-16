// The console's frame order, checked on the PC.
//
// platform/3ds/gfx_c3d.cpp is compiled here, unchanged, against a stand-in
// for libctru and citro3d (tests/mock3ds). The stand-in draws nothing; it
// keeps the books a GPU keeps: which command buffer it is executing, which
// vertex, index and texture memory that buffer points at, and when it is
// done. Breaking those rules on the console means garbage on the screen or a
// crash, and no emulator on this PC would show it. Here every breach is
// counted.
//
// Both frame orders run the same flight through the test worlds, with chunks
// streaming in and out, world switches and a full video memory:
//  * citro3d's own order (wait for the GPU, then issue the draw calls), and
//  * the overlapped order (issue the next frame's draw calls into a second
//    buffer while the GPU executes the previous frame).
//
// The vertex shader is checked as well. The stand-in reads the assembled
// shader binary (platform/3ds/world_shbin.h) and can run its bytecode; the
// test shades the vertices of real draw calls - with the uniforms the backend
// set for them - and compares the result with where the engine's own maths
// puts those vertices on the screen: the world's and the character's.
// usage: test_frames <scratch dir>
#include "app.h"
#include "bake.h"
#include "gfx_c3d.h"
#include "importers.h"
#include "mock3ds.h"
#include "model_bake.h"
#include "pak_writer.h"
#include "test_models.h"
#include "trace.h"

#include <citro3d.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace g3;

static int gFailures = 0, gChecks = 0;

#define CHECK(cond)                                                              \
	do {                                                                         \
		++gChecks;                                                               \
		if (!(cond)) {                                                           \
			++gFailures;                                                         \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
		}                                                                        \
	} while (0)

static void printViolations(const char* when) {
	const mock3ds::Counters& c = mock3ds::counters();
	std::printf("  violations %s: busy buffer written %u, written outside %u, freed in use %u, submitted freed %u,\n"
	            "    submitted while busy %u, cleared while busy %u, bad free %u, draw without buffer %u,\n"
	            "    list not at buffer start %u, submitted uncleared %u\n",
	            when, c.wroteBusyBuffer, c.wroteOutside, c.freedInUse, c.submittedFreed, c.submittedBusy, c.clearedBusy,
	            c.badFree, c.drawOutsideFrame, c.submittedNotStart, c.submittedUncleared);
}

// The checker itself: each rule must be noticed when it is broken on purpose.
static void testMockNotices() {
	mock3ds::reset();
	CHECK(C3D_Init(0x4000));
	C3D_RenderTarget* target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, 0);
	void* vertices = linearAlloc(256);
	u16* indices = static_cast<u16*>(linearAlloc(64));
	C3D_BufInfo buf;
	BufInfo_Init(&buf);
	BufInfo_Add(&buf, vertices, 16, 3, 0x210);

	// A correct frame.
	CHECK(C3D_FrameBegin(C3D_FRAME_SYNCDRAW));
	C3D_RenderTargetClear(target, C3D_CLEAR_ALL, 0, 0);
	C3D_FrameDrawOn(target);
	C3D_SetBufInfo(&buf);
	C3D_DrawElements(GPU_TRIANGLES, 3, C3D_UNSIGNED_SHORT, indices);
	u32* const list = gpuCmdBuf;
	C3D_FrameEnd(0);
	CHECK(mock3ds::violations() == 0 && mock3ds::counters().framesSubmitted == 1 && mock3ds::counters().framesShown == 1);

	// Writing the next frame into the buffer the GPU is executing.
	GPUCMD_SetBuffer(list, 0x1000, 0);
	C3D_DrawElements(GPU_TRIANGLES, 3, C3D_UNSIGNED_SHORT, indices);
	CHECK(mock3ds::counters().wroteBusyBuffer == 1);
	// Clearing the target the GPU is drawing into.
	C3D_RenderTargetClear(target, C3D_CLEAR_ALL, 0, 0);
	CHECK(mock3ds::counters().clearedBusy == 1);
	// Freeing memory the executing list reads (and the list just written
	// refers to it as well: two breaches).
	linearFree(vertices);
	CHECK(mock3ds::counters().freedInUse == 2);
	// A draw call with no buffer at all.
	GPUCMD_SetBuffer(nullptr, 0, 0);
	C3D_DrawElements(GPU_TRIANGLES, 3, C3D_UNSIGNED_SHORT, indices);
	CHECK(mock3ds::counters().drawOutsideFrame == 1);
	// A draw call that points at freed memory.
	CHECK(C3D_FrameBegin(0));
	C3D_RenderTargetClear(target, C3D_CLEAR_ALL, 0, 0);
	C3D_SetBufInfo(&buf);
	C3D_DrawElements(GPU_TRIANGLES, 3, C3D_UNSIGNED_SHORT, indices);
	CHECK(mock3ds::counters().submittedFreed == 1);
	C3D_FrameEnd(0);
	CHECK(mock3ds::counters().submittedUncleared == 0);
	// Freeing twice.
	linearFree(vertices);
	CHECK(mock3ds::counters().badFree == 1);
	// A frame nobody cleared the target for, submitted while the GPU is busy.
	void* more = linearAlloc(256);
	BufInfo_Init(&buf);
	BufInfo_Add(&buf, more, 16, 3, 0x210);
	u32* second = static_cast<u32*>(linearAlloc(0x1000));
	GPUCMD_SetBuffer(second, 0x400, 0);
	C3D_SetBufInfo(&buf);
	C3D_DrawElements(GPU_TRIANGLES, 3, C3D_UNSIGNED_SHORT, indices);
	const u32 recorded = gpuCmdBufOffset;
	CHECK(C3D_FrameBegin(0)); // (this is the wait: the GPU is idle again)
	GPUCMD_SetBuffer(second, 0x400, recorded);
	C3D_FrameEnd(0);
	CHECK(mock3ds::counters().submittedUncleared == 1 && mock3ds::counters().submittedBusy == 0);
	// A list that does not start where its buffer starts.
	CHECK(C3D_FrameBegin(0));
	C3D_RenderTargetClear(target, C3D_CLEAR_ALL, 0, 0);
	GPUCMD_SetBuffer(second + 0x200, 0x200, 0);
	C3D_DrawElements(GPU_TRIANGLES, 3, C3D_UNSIGNED_SHORT, indices);
	C3D_FrameEnd(0);
	CHECK(mock3ds::counters().submittedNotStart == 1);
	// Running past the end of the buffer.
	CHECK(C3D_FrameBegin(0));
	GPUCMD_SetBuffer(gpuCmdBuf, 8, 0);
	C3D_DrawElements(GPU_TRIANGLES, 3, C3D_UNSIGNED_SHORT, indices);
	CHECK(mock3ds::counters().wroteOutside == 1);
	C3D_FrameEnd(0);
	CHECK(C3D_FrameBegin(0)); // wait for the GPU before freeing what it read
	C3D_FrameEnd(0);
	linearFree(more);
	linearFree(second);

	C3D_RenderTargetDelete(target);
	linearFree(indices);
	C3D_Fini();
	CHECK(mock3ds::counters().linearBlocks == 0);
}

// ---- the vertex shader, run instruction by instruction ----

struct ShaderCheck {
	// The frame being drawn.
	Mat4 view, proj;
	float anchor[3];
	float zNear, zFar;
	// The character whose draw calls are expected, part after part.
	const Model* model = nullptr;
	Mat34 skin[MODEL_MAX_NODES];
	Vec3 origin {0, 0, 0};
	float yaw = 0, lean = 0;
	float light[3] = {1, 1, 1};
	u32 nextPart = 0;
	// Skinned draw calls that come before the expected character's: other
	// characters. Theirs are checked against the palette they were drawn with.
	unsigned others = 0, otherDraws = 0, otherVertices = 0;
	// Results.
	unsigned worldDraws = 0, skinnedDraws = 0, worldVertices = 0, skinnedVertices = 0, blendedVertices = 0, behind = 0;
	unsigned failed = 0, unexpected = 0, movedBySwitch = 0;
	// The same world vertices with the dissolve switched on (b1, see gfx::setFade):
	// nothing else may change, and texture coordinate 1 is the place on the screen.
	bool checkFade = false; // (only the dissolve's shader has it)
	// Graphics+: the bool register of `windy` in the shader in use (b2 in the
	// dissolve's, b1 in the default one), and what was seen of it.
	u16 windyBit = 4;
	// Where the shader in use keeps its uniforms (world_fade/world.v.pica: the defaults; world_full/fast.v.pica:
	// the combined matrix takes fewer registers) and which program a draw call may be drawn by.
	int locChunk = 8, locParams = 9, locDayDelta = 10, locTexOffset = 11, locBones = 13, locFx = 91;
	bool twoEntries = false; // the fast shader: entry 1 (2 with the dissolve) draws the static world, 3 characters, 0 what sways
	unsigned fadeBit = 2;    // the bool register of `fading` (b1 in world_fade.v.pica, b2 in the default shader)
	unsigned fastDraws = 0, fullDraws = 0, skinDraws = 0, wrongEntry = 0, swayOnFast = 0, lampDraws = 0, litVertices = 0;
	unsigned occludedVertices = 0, swayedVertices = 0, windyDraws = 0;
	// The water's waves: the bool register of `wavy` (b3 in the default shader, 0: not looked at), the draws that had it
	// on, and those of them that the fast entry drew (none may be).
	u16 wavyBit = 0;
	int locWave = 91; // the `wave` uniform's register in the default shader
	unsigned wavyDraws = 0, wavyOnFast = 0, wavedVertices = 0, sheenVertices = 0;
	double worstWave = 0; // cm: the highest a corner was lifted
	double worstSway = 0; // cm: the farthest a corner was moved
	unsigned fadedVertices = 0;
	double fadeChanged = 0, worstNoise = 0;
	double worstWorldPx = 0, worstSkinPx = 0, worstDepth = 0, worstColor = 0, worstTex = 0;
	double worstGpuWorldPx = 0, worstGpuSkinPx = 0, worstGpuSkyPx = 0;
	u32 highestBoneRegister = 0;
	// Vertices that are one point of the world must come out of the GPU's
	// 24-bit arithmetic as one point, whichever chunk they belong to.
	struct Key {
		i32 x, y, z;
		bool operator<(const Key& o) const { return x != o.x ? x < o.x : y != o.y ? y < o.y : z < o.z; }
	};
	struct Seen {
		float clip[4];
		const void* buffer;
		bool shared;
	};
	std::map<Key, Seen> points;
	unsigned sharedPoints = 0, splitPoints = 0;

	void beginFrame(const Mat4& v, Vec3 eye, float fovY, float zn, float zf) {
		view = v;
		proj = perspectiveLH(fovY, float(gfx::SCREEN_W) / float(gfx::SCREEN_H), zn, zf);
		const EyeFrame ef = eyeFrame(v, eye);
		std::memcpy(anchor, ef.anchor, sizeof(anchor));
		zNear = zn;
		zFar = zf;
		nextPart = 0;
		points.clear();
	}
	void expect(const Model& m, const Pose& pose, Vec3 at, float turn, const float l[3], float leaning = 0.0f) {
		lean = leaning;
		model = &m;
		Mat34 world[MODEL_MAX_NODES];
		m.worldTransforms(pose, world);
		for (u32 i = 0; i < m.nodeCount(); ++i) {
			Mat34 inv;
			std::memcpy(inv.m, m.node(i).invBind, sizeof(inv.m));
			skin[i] = mul(world[i], inv);
		}
		origin = at;
		yaw = turn;
		std::memcpy(light, l, sizeof(light));
		nextPart = 0;
	}
};

static void shaderHook(const mock3ds::DrawCall& call, void* user) {
	ShaderCheck& sc = *static_cast<ShaderCheck*>(user);
	const int locChunk = sc.locChunk, locParams = sc.locParams, locDayDelta = sc.locDayDelta, locTexOffset = sc.locTexOffset,
	          locBones = sc.locBones, locFx = sc.locFx; // checked in the tests of each shader
	const bool skinned = call.bools & 1;
	const bool windy = (call.bools & sc.windyBit) != 0;
	// (fix148: entry 5, the lamps', is the fast one with the dissolve and the lamps' light.)
	const bool fastEntry = call.entry == 1 || call.entry == 2 || call.entry == 5, skinEntry = call.entry == 3;
	const bool lampEntry = sc.twoEntries && call.entry == 5;
	sc.lampDraws += lampEntry;
	if (sc.twoEntries) {
		// Characters have their own entry; swaying materials need the full shader; the fast one has no branches for them.
		(fastEntry ? sc.fastDraws : skinEntry ? sc.skinDraws : sc.fullDraws) += 1;
		sc.wrongEntry += (fastEntry && (skinned || windy)) || (skinEntry && !skinned);
	}
	const float* fx = call.uniforms[locFx];
	if (windy) ++sc.windyDraws;
	if (sc.wavyBit && (call.bools & sc.wavyBit)) {
		++sc.wavyDraws;
		sc.wavyOnFast += fastEntry;
	}
	const float* chunk = call.uniforms[locChunk];
	const float* params = call.uniforms[locParams];
	const bool sky = params[2] == 1.0f && params[3] == 0.0f; // never fogged

	const ModelPart* part = nullptr;
	bool other = false;
	if (skinned && sc.others > 0) {
		--sc.others;
		++sc.otherDraws;
		other = true;
	} else if (skinned) {
		++sc.skinnedDraws;
		while (sc.model && sc.nextPart < sc.model->partCount() && sc.model->part(sc.nextPart).indexCount == 0) ++sc.nextPart;
		if (!sc.model || sc.nextPart >= sc.model->partCount() || sc.model->part(sc.nextPart).indexCount != call.count) {
			++sc.unexpected;
			return;
		}
		part = &sc.model->part(sc.nextPart++);
		// The palette as the engine uploaded it: three rows per bone.
		for (u32 b = 0; b < part->boneCount; ++b) sc.highestBoneRegister = std::max(sc.highestBoneRegister, u32(locBones) + b * 3 + 2);
	} else {
		++sc.worldDraws;
	}

	std::vector<bool> done(65536, false);
	// Turned to where it faces, and tipped sideways about its feet (a runner in a turn).
	const Mat34 turn = mat34From(quatMul(quatFromYaw(sc.yaw), {0, 0, std::sin(-sc.lean * 0.5f), std::cos(-sc.lean * 0.5f)}), {0, 0, 0});
	for (unsigned i = 0; i < call.count; ++i) {
		const u16 index = call.indices[i];
		if (done[index]) continue;
		done[index] = true;
		WorldVertex v;
		std::memcpy(&v, static_cast<const u8*>(call.vertices) + size_t(index) * call.stride, sizeof(v));
		mock3ds::ShadedVertex got, gpu;
		if (!mock3ds::shadeVertex(call, index, got, false) || !mock3ds::shadeVertex(call, index, gpu, true)) {
			++sc.failed;
			continue;
		}

		if (sc.checkFade && !skinned && !sky) {
			mock3ds::DrawCall faded = call;
			faded.bools = u16(call.bools | sc.fadeBit);
			mock3ds::ShadedVertex f;
			if (!mock3ds::shadeVertex(faded, index, f, false)) {
				++sc.failed;
			} else {
				++sc.fadedVertices;
				for (int k = 0; k < 4; ++k)
					sc.fadeChanged = std::max({sc.fadeChanged, std::fabs(double(f.position[k]) - double(got.position[k])),
					                           std::fabs(double(f.color[k]) - double(got.color[k]))});
				const double w = double(got.position[3]);
				if (w > 1.0) {
					// Texels of the 8x8 noise: about one a pixel (240 x 400, the shader's x along the 240).
					const double s = double(got.position[0]) / w * 15.0, t = double(got.position[1]) / w * 25.0;
					sc.worstNoise = std::max({sc.worstNoise, std::fabs(double(f.texcoord1[0]) - s) / std::max(1.0, std::fabs(s)),
					                          std::fabs(double(f.texcoord1[1]) - t) / std::max(1.0, std::fabs(t))});
				}
			}
		}

		// Where the engine's maths puts this vertex.
		double world[3], wantColor[3];
		if (other) {
			// Somebody else: the two bones of the palette this draw call came
			// with, weighted, at the place it came with.
			// (The characters' entry has the position step and the place in the rows already: world_skin.v.pica.)
			const double step = skinEntry ? 1.0 : double(chunk[3]);
			const double p[3] = {double(v.x) * step, double(v.y) * step, double(v.z) * step};
			const u32 s0 = v.r / 3u, s1 = v.g / 3u;
			if (v.r % 3 || v.g % 3 || s0 >= MODEL_PALETTE || s1 >= MODEL_PALETTE) {
				++sc.unexpected;
				continue;
			}
			const double w = double(v.b) / 255.0;
			for (int k = 0; k < 3; ++k) {
				const float* r0 = call.uniforms[locBones + int(s0) * 3 + k];
				const float* r1 = call.uniforms[locBones + int(s1) * 3 + k];
				const double a = double(r0[0]) * p[0] + double(r0[1]) * p[1] + double(r0[2]) * p[2] + double(r0[3]);
				const double b = double(r1[0]) * p[0] + double(r1[1]) * p[1] + double(r1[2]) * p[2] + double(r1[3]);
				world[k] = double(sc.anchor[k]) + (skinEntry ? 0.0 : double(chunk[k])) + a + (b - a) * w;
				wantColor[k] = double(got.color[k]);
			}
			++sc.otherVertices;
		} else if (skinned) {
			const Vec3 p = {float(v.x) * chunk[3], float(v.y) * chunk[3], float(v.z) * chunk[3]};
			const u32 s0 = v.r / 3u, s1 = v.g / 3u;
			if (v.r % 3 || v.g % 3 || s0 >= part->boneCount || s1 >= part->boneCount) {
				++sc.unexpected;
				continue;
			}
			const Vec3 p0 = transformPoint(sc.skin[part->bones[s0]], p), p1 = transformPoint(sc.skin[part->bones[s1]], p);
			const float w = float(v.b) / 255.0f;
			const Vec3 posed = transformPoint(turn, p0 + (p1 - p0) * w);
			world[0] = double(sc.origin.x) + double(posed.x);
			world[1] = double(sc.origin.y) + double(posed.y);
			world[2] = double(sc.origin.z) + double(posed.z);
			for (int k = 0; k < 3; ++k) wantColor[k] = double(sc.light[k]) * double(v.a) / 255.0;
			++sc.skinnedVertices;
			sc.blendedVertices += v.b != 0 && s0 != s1;
			// The switch is what makes the difference: shaded as world
			// geometry the same vertex lands somewhere else.
			mock3ds::DrawCall plain = call;
			plain.bools = 0;
			mock3ds::ShadedVertex asWorld;
			if (skinEntry)
				++sc.movedBySwitch; // (an entry of its own: no switch to compare with)
			else if (mock3ds::shadeVertex(plain, index, asWorld, false))
				sc.movedBySwitch += std::fabs(asWorld.position[0] - got.position[0]) + std::fabs(asWorld.position[1] - got.position[1]) > 1e-3f;
		} else {
			const double q[3] = {double(v.x), double(v.y), double(v.z)};
			const u8 rgb[3] = {v.r, v.g, v.b};
			double p[3] = {double(chunk[3]) * q[0], double(chunk[3]) * q[1], double(chunk[3]) * q[2]};
			// Graphics+ (g3ds/world.h, gfx::ChunkFx): w's high byte sways along the
			// wind by the wave x (1 - |x|), its low byte darkens the light.
			const double sway = double(vertexWind(v.w)), occlusion = double(vertexOcclusion(v.w)) / 256.0;
			sc.swayOnFast += sc.twoEntries && call.entry == 1 && sway > 0; // (the fast shader would take the sway for occlusion)
			if (windy && sway > 0) {
				double t = double(fx[2]) + (p[0] + p[2]) / double(gfx::WIND_WAVE_CM);
				t -= std::floor(t);
				const double x = 2.0 * t - 1.0, wave = x * (1.0 - std::fabs(x)) * sway;
				p[0] += double(fx[0]) * wave;
				p[2] += double(fx[1]) * wave;
				sc.worstSway = std::max(sc.worstSway, std::hypot(double(fx[0]) * wave, double(fx[1]) * wave));
				++sc.swayedVertices;
			}
			// The water's waves (world_full.v.pica): two waves of one swell 4 x (1 - |x|), lift by 0.7 A + 0.3 B, the slopes light it, the
			// texture slides; all per step of the corner's weight (w's high byte), the uniform holds them per step.
			double waveShade = 1.0, shiftU = 0.0, shiftV = 0.0;
			if (sc.wavyBit && (call.bools & sc.wavyBit) && sway > 0) {
				const float* wv = call.uniforms[sc.locWave];
				const double k[2][2] = {{0.00057143, 0.00042857}, {0.00070144, 0.00086167}};
				double ws[2], sl[2];
				for (int a = 0; a < 2; ++a) {
					const double u = double(wv[a]) + k[a][0] * p[0] + k[a][1] * p[2];
					const double x = 2.0 * (u - std::floor(u)) - 1.0, edge = 1.0 - std::fabs(x);
					ws[a] = 4.0 * x * edge;
					sl[a] = 2.0 * edge - 1.0;
				}
				const double lift = double(wv[2]) * sway * (0.7 * ws[0] + 0.3 * ws[1]);
				p[1] += lift;
				waveShade = 1.0 + double(wv[3]) * sway * (0.6 * sl[0] + 0.4 * sl[1]);
				shiftU = 0.000118110 * sway * ws[0];
				shiftV = 0.000118110 * sway * ws[1];
				sc.worstWave = std::max(sc.worstWave, std::fabs(lift));
				++sc.wavedVertices;
			}
			const double shade = (1.0 - occlusion * double(fx[3])) * waveShade;
			sc.occludedVertices += occlusion > 0 && fx[3] > 0;
			for (int k = 0; k < 3; ++k) {
				world[k] = double(sc.anchor[k]) + double(chunk[k]) + p[k];
				wantColor[k] = double(rgb[k]) / 255.0 * (1.0 + double(v.a) / 255.0 * double(call.uniforms[locDayDelta][k])) * shade;
			}
			// fix148: the lamps' entry adds their light (world_lamp.v.pica): the first eight rows of the palette are four lamps,
			// (place relative to the anchor, -1/range^2) and (colour, (keep - 1) / 255).
			if (lampEntry) {
				bool lit = false;
				for (int l = 0; l < 4; ++l) {
					const float* a = call.uniforms[locBones + l * 2];
					const float* b = call.uniforms[locBones + l * 2 + 1];
					double d2 = 0.0;
					for (int k = 0; k < 3; ++k) d2 += (double(a[k]) - (double(chunk[k]) + p[k])) * (double(a[k]) - (double(chunk[k]) + p[k]));
					double t = std::max(0.0, 1.0 + d2 * double(a[3]));
					t = (t * t) * (t * t) * (1.0 + double(v.a) * double(call.uniforms[locBones + 1][3]));
					for (int k = 0; k < 3; ++k) wantColor[k] += double(b[k]) * t;
					lit = lit || (t > 0.0 && (b[0] != 0.0f || b[1] != 0.0f || b[2] != 0.0f));
				}
				sc.litVertices += lit;
			}
			++sc.worldVertices;
		}
		// View space in double precision, straight from the camera.
		double eye[4];
		for (int r = 0; r < 4; ++r)
			eye[r] = double(sc.view.at(r, 0)) * world[0] + double(sc.view.at(r, 1)) * world[1] + double(sc.view.at(r, 2)) * world[2] +
			         double(sc.view.at(r, 3));
		double wantFog = std::clamp(double(params[2]) - eye[2] * double(params[3]), 0.0, 1.0);
		if (!skinned && sc.wavyBit && (call.bools & sc.wavyBit)) {
			// fix140, the water's sheen: f = t / (1 + t), t = view depth / 800 cm; the colour 0.7 + 0.6 f times, the fog's share cut by 0.4 f.
			const double t = eye[2] * double(gfx::WATER_SHEEN_PER_CM), f = t / (1.0 + t);
			for (int k = 0; k < 3; ++k) wantColor[k] *= double(gfx::WATER_SHEEN_BASE) + double(gfx::WATER_SHEEN_RANGE) * f;
			wantFog *= 1.0 - double(gfx::WATER_SHEEN_SKY) * f;
			++sc.sheenVertices;
		}
		sc.worstColor = std::max(sc.worstColor, std::fabs(double(got.color[3]) - wantFog));
		for (int k = 0; k < 3; ++k) sc.worstColor = std::max(sc.worstColor, std::fabs(double(got.color[k]) - wantColor[k]));
		double wantU = double(v.u) * double(params[0]) + double(call.uniforms[locTexOffset][0]);
		double wantV = double(v.v) * double(params[0]) + double(call.uniforms[locTexOffset][1]);
		if (!skinned && sc.wavyBit && (call.bools & sc.wavyBit) && vertexWind(v.w) > 0) {
			// (the wave's texture slide, as above)
			const float* wv = call.uniforms[sc.locWave];
			const double k[2][2] = {{0.00057143, 0.00042857}, {0.00070144, 0.00086167}};
			double chunkP[3] = {double(chunk[3]) * double(v.x), double(chunk[3]) * double(v.y), double(chunk[3]) * double(v.z)};
			double ws[2];
			for (int a = 0; a < 2; ++a) {
				const double u = double(wv[a]) + k[a][0] * chunkP[0] + k[a][1] * chunkP[2];
				const double x = 2.0 * (u - std::floor(u)) - 1.0;
				ws[a] = 4.0 * x * (1.0 - std::fabs(x));
			}
			wantU += 0.000118110 * double(vertexWind(v.w)) * ws[0];
			wantV += 0.000118110 * double(vertexWind(v.w)) * ws[1];
		}
		sc.worstTex = std::max({sc.worstTex, std::fabs(double(got.texcoord[0]) - wantU), std::fabs(double(got.texcoord[1]) - wantV)});

		// One point of the world, one point on the GPU. (A swaying corner is
		// moved after its chunk is known: it is left out.)
		if (!skinned && !sky && !(windy && vertexWind(v.w) > 0) && !(sc.wavyBit && (call.bools & sc.wavyBit) && vertexWind(v.w) > 0)) {
			const ShaderCheck::Key key = {i32(std::lround(world[0] * 64.0)), i32(std::lround(world[1] * 64.0)), i32(std::lround(world[2] * 64.0))};
			auto it = sc.points.find(key);
			if (it == sc.points.end()) {
				ShaderCheck::Seen seen;
				std::memcpy(seen.clip, gpu.position, sizeof(seen.clip));
				seen.buffer = call.vertices;
				seen.shared = false;
				sc.points[key] = seen;
			} else if (it->second.buffer != call.vertices) {
				if (!it->second.shared) ++sc.sharedPoints;
				it->second.shared = true;
				if (std::memcmp(it->second.clip, gpu.position, sizeof(gpu.position)) != 0) ++sc.splitPoints;
			}
		}

		if (eye[2] < double(sc.zNear)) { // behind the near plane: clipped, no place on the screen
			++sc.behind;
			continue;
		}
		// The engine's projection: an upright picture, x right, y up.
		const double wantX = double(sc.proj.at(0, 0)) * eye[0] / eye[2], wantY = double(sc.proj.at(1, 1)) * eye[1] / eye[2];
		// The console's screen lies on its side: the shader's x runs up, its y to the left.
		// A vertex far outside the picture only matters by where its triangle
		// crosses the picture's edge: its error counts scaled down to there.
		const double outside = std::max({1.0, std::fabs(wantX), std::fabs(wantY)});
		auto pixels = [&](const mock3ds::ShadedVertex& s) {
			const double x = -double(s.position[1]) / double(s.position[3]), y = double(s.position[0]) / double(s.position[3]);
			return std::max(std::fabs(x - wantX) * gfx::SCREEN_W * 0.5, std::fabs(y - wantY) * gfx::SCREEN_H * 0.5) / outside;
		};
		const double px = pixels(got), gpuPx = pixels(gpu);
		(skinned ? sc.worstSkinPx : sc.worstWorldPx) = std::max(skinned ? sc.worstSkinPx : sc.worstWorldPx, px);
		// (The sky dome's far-off points are many and out at the picture's edges: the GPU's rounding shows
		// there most, on a smooth gradient nobody sees it. They have a limit of their own.)
		double& worstGpu = skinned ? sc.worstGpuSkinPx : sky ? sc.worstGpuSkyPx : sc.worstGpuWorldPx;
		worstGpu = std::max(worstGpu, gpuPx);
		// Depth: -1 at the near plane, 0 at the far one; w is the distance.
		const double wantDepth = double(sc.zNear) * (double(sc.zFar) - eye[2]) / ((double(sc.zNear) - double(sc.zFar)) * eye[2]);
		sc.worstDepth = std::max({sc.worstDepth, std::fabs(double(got.position[2]) / double(got.position[3]) - wantDepth),
		                          std::fabs(double(got.position[3]) - eye[2]) / eye[2]});
	}
}

static void testShader(const std::string& pak) {
	mock3ds::reset();
	gfx::c3dSetClassicFrames(false);
	gfx::c3dSetFadeShader(true); // the dissolve: since fix85 the default shader's entries with it (fadefast, skinmain)
	CHECK(gfx::init() && gfx::fadeSupported() && gfx::c3dFastShader());
	// The binary is the one picasso assembled from world_full/fast/fastfade/skin.v.pica, and the
	// backend found every uniform in it.
	std::printf("  shader: %u instructions, %u of 96 float registers%s%s\n", mock3ds::shaderInstructions(),
	            mock3ds::shaderFloatRegisters(), *mock3ds::shaderProblem() ? "; PROBLEM: " : "", mock3ds::shaderProblem());
	CHECK(!*mock3ds::shaderProblem());
	// (mvp, fogRow, five vectors, the palette, fx, lightK, aoK, wave; four constants: consts, noiseScale, the waves' two)
	CHECK(mock3ds::shaderFloatRegisters() == 4 + 1 + 5 + MODEL_PALETTE * 3 + 1 + 1 + 1 + 1 + 4 && mock3ds::shaderFloatRegisters() <= 96);
	shaderProgram_s probe;
	shaderProgramInit(&probe);
	shaderProgramSetVsh(&probe, nullptr);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "chunk") == 5);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "params") == 6);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "dayDelta") == 7);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "texOffset") == 8);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "bones") == 10);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "skinned") == 0x68);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "windy") == 0x69);
	// (the water's waves: `wave` after aoK and the bool `wavy` after `fading`, in every entry's source alike)
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "wavy") == 0x6b);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "wave") == shaderInstanceGetUniformLocation(probe.vertexShader, "aoK") + 1);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "fading") == 0x6A);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "fx") == 88);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "nonsense") < 0);

	ShaderCheck sc;
	sc.checkFade = true;
	sc.windyBit = 2;
	sc.wavyBit = 8;
	sc.fadeBit = 4;
	sc.locChunk = 5;
	sc.locParams = 6;
	sc.locDayDelta = 7;
	sc.locTexOffset = 8;
	sc.locBones = 10;
	sc.locFx = 88;
	sc.twoEntries = true;
	// 1. The game as it runs: the world around a walking, turning character,
	// in the evening (the time of day tints the light).
	{
		App app;
		CHECK(app.init(pak.c_str(), nullptr));
		app.skipChapters();
		app.setDrawDistance(6000.0f);
		app.environment().timeOfDay = 19.5f;
		// Graphics+ at full: the archive is baked with occlusion and wind.
		CHECK(app.world().hasOcclusion() && app.world().hasWind());
		app.world().config().occlusion = 1.0f;
		app.world().config().wind = 2.0f;
		app.world().config().waves = 22.0f;
		app.world().preload(app.camera().pos);
		CHECK(app.hero().loaded() && app.thirdPerson() && app.npcs().loaded());
		u32 inhabitants = 0;
		const float dt = 1.0f / 30.0f;
		for (int f = 0; f < 50; ++f) {
			Input in;
			in.moveZ = 1.0f;
			in.moveX = f > 20 ? 0.6f : 0.0f;
			in.lookX = 0.4f;
			in.lookY = f < 10 ? -0.3f : 0.0f;
			app.update(in, dt);
			const bool check = f % 7 == 6;
			if (check) {
				// The world's inhabitants are drawn before the hero: how many
				// draw calls they take is known once the frame has been drawn.
				app.render();
				inhabitants = std::max(inhabitants, app.npcs().stats().shown);
				const Camera& cam = app.camera();
				sc.beginFrame(viewLH(cam.pos, cam.yaw, cam.pitch), cam.pos, cam.fovY, cam.zNear, cam.zFar);
				sc.expect(app.hero().model(), app.hero().pose(), app.hero().feet(), app.hero().yaw(), app.hero().light(), app.hero().lean());
				sc.others = app.npcs().stats().drawCalls + app.loot().stats().drawCalls; // (and the things lying about)
				mock3ds::setDrawHook(shaderHook, &sc);
			}
			app.render();
			mock3ds::setDrawHook(nullptr, nullptr);
			if (check) CHECK(app.heroVisible() && sc.nextPart == app.hero().model().partCount() && sc.others == 0);
		}
		CHECK(app.hero().role() == Hero::RUN && app.hero().speed() > 300.0f);
		std::printf("  world: %u draw calls, %u vertices: within %.4f px of the engine's projection (%.4f px in the GPU's 24-bit floats); "
		            "depth within %.1e, colour and fog within %.1e, texture coordinates within %.1e\n",
		            sc.worldDraws, sc.worldVertices, sc.worstWorldPx, sc.worstGpuWorldPx, sc.worstDepth, sc.worstColor, sc.worstTex);
		std::printf("  seams: %u points shared between chunks, %u of them split by the GPU's rounding\n", sc.sharedPoints, sc.splitPoints);
		std::printf("  hero: %u draw calls, %u vertices (%u on two bones): within %.4f px (%.4f px in 24-bit floats)\n", sc.skinnedDraws,
		            sc.skinnedVertices, sc.blendedVertices, sc.worstSkinPx, sc.worstGpuSkinPx);
		std::printf("  inhabitants: up to %u in view, %u draw calls, %u vertices, checked against the palettes they were drawn with\n",
		            inhabitants, sc.otherDraws, sc.otherVertices);
		CHECK(inhabitants >= 2 && sc.otherDraws > 10 && sc.otherVertices > 300);
		CHECK(sc.worldDraws > 50 && sc.worldVertices > 5000 && sc.skinnedDraws >= 7 && sc.skinnedVertices > 500);
		CHECK(sc.blendedVertices > 50);
		CHECK(sc.failed == 0 && sc.unexpected == 0);
		CHECK(sc.worstWorldPx < 0.02 && sc.worstSkinPx < 0.02);
		CHECK(sc.worstGpuWorldPx < 0.25 && sc.worstGpuSkinPx < 0.25 && sc.worstGpuSkyPx < 0.75);
		std::printf("  sky dome: within %.4f px in 24-bit floats\n", sc.worstGpuSkyPx);
		CHECK(sc.worstDepth < 1e-3 && sc.worstColor < 1e-4 && sc.worstTex < 1e-3);
		CHECK(sc.sharedPoints > 20 && sc.splitPoints == 0);
		std::printf("  dissolve: %u world vertices shaded with it on: the same otherwise (within %.1e), the noise at the screen position "
		            "(within %.1e)\n", sc.fadedVertices, sc.fadeChanged, sc.worstNoise);
		CHECK(sc.fadedVertices > 5000 && sc.fadeChanged == 0.0 && sc.worstNoise < 1e-4);
		std::printf("  Graphics+: %u world vertices darkened by their occlusion, %u swayed in %u draw calls (at most %.1f cm)\n",
		            sc.occludedVertices, sc.swayedVertices, sc.windyDraws, sc.worstSway);
		CHECK(sc.occludedVertices > 1000 && sc.swayedVertices > 50 && sc.windyDraws > 0 && sc.worstSway > 0.5 && sc.worstSway < 140.0); // (World kWindGain 3.5: a crown sways tens of cm)
		// The static world by the fast entry with the dissolve, the characters by theirs.
		std::printf("  entries: %u draws fast, %u characters', %u full\n", sc.fastDraws, sc.skinDraws, sc.fullDraws);
		CHECK(sc.fastDraws > 50 && sc.skinDraws >= sc.skinnedDraws && sc.wrongEntry == 0 && sc.swayOnFast == 0);
		app.shutdown();
	}

	// 2. A skeleton with more bones than a draw call can address, curled up:
	// every part brings its own palette, up to the last uniform register.
	{
		Pak p;
		CHECK(p.open(pak.c_str()));
		Model chain;
		CHECK(chain.load(p, "chain"));
		CHECK(chain.nodeCount() == 60 && chain.partCount() >= 3);
		const Clip* bend = chain.clip("S_BEND");
		CHECK(bend != nullptr);
		Pose pose;
		chain.restPose(pose);
		if (bend) bend->sample(0.33f, pose);
		const Vec3 at = {120.0f, 30.0f, -40.0f};
		const float yaw = 2.2f, light[3] = {0.9f, 0.5f, 0.25f};

		ShaderCheck cc;
		cc.locChunk = 5;
		cc.locParams = 6;
		cc.locDayDelta = 7;
		cc.locTexOffset = 8;
		cc.locBones = 10;
		cc.locFx = 88;
		cc.windyBit = 2;
		gfx::FrameDesc fd {};
		const Vec3 eye = {420.0f, 260.0f, -330.0f};
		fd.view = viewLH(eye, -0.8f, -0.2f);
		fd.eye = eye;
		fd.fovY = 1.0f;
		fd.zNear = 20.0f;
		fd.zFar = 5000.0f;
		fd.fogStart = 300.0f;
		fd.fogEnd = 900.0f;
		fd.dayLight[0] = fd.dayLight[1] = fd.dayLight[2] = 1.0f;
		cc.beginFrame(fd.view, eye, fd.fovY, fd.zNear, fd.zFar);
		cc.expect(chain, pose, at, yaw, light);
		gfx::frameBegin(fd);
		gfx::setPass(gfx::PASS_WORLD);
		mock3ds::setDrawHook(shaderHook, &cc);
		chain.draw(pose, at, yaw, light);
		mock3ds::setDrawHook(nullptr, nullptr);
		gfx::frameEnd();
		std::printf("  chain: %u nodes in %u draw calls, %u vertices (%u on two bones, %u behind the camera): within %.4f px "
		            "(%.4f px in 24-bit floats); highest bone register c%u\n",
		            chain.nodeCount(), cc.skinnedDraws, cc.skinnedVertices, cc.blendedVertices, cc.behind, cc.worstSkinPx,
		            cc.worstGpuSkinPx, cc.highestBoneRegister);
		CHECK(cc.skinnedDraws == chain.partCount() && cc.nextPart == chain.partCount() && cc.failed == 0 && cc.unexpected == 0);
		CHECK(cc.skinnedVertices > 200 && cc.blendedVertices > 100 && cc.behind == 0);
		CHECK(cc.movedBySwitch > cc.skinnedVertices * 9 / 10);
		CHECK(cc.worstSkinPx < 0.02 && cc.worstGpuSkinPx < 0.25 && cc.worstColor < 1e-4 && cc.worstDepth < 1e-3);
		CHECK(cc.highestBoneRegister == 10 + MODEL_PALETTE * 3 - 1); // a full palette was used
		chain.release();
		p.close();
	}
	gfx::shutdown();
	gfx::c3dSetFadeShader(false);
	CHECK(mock3ds::violations() == 0 && mock3ds::counters().linearBlocks == 0);
}

// The sun's mask (g3ds-bake --sun-shadow): a chunk that has one is drawn by the fast entry that lays it (fastsun), whose
// texture coordinate 2 is the world's x and z mapped onto the mask as the table says - checked by running the shader on
// the vertices; a chunk without one keeps the short entry.
struct SunCheck {
	int entry = -1;
	unsigned vertices = 0;
	double worst = 0.0;
	const WorldVertex* verts = nullptr;
	float origin[3] = {0, 0, 0}, posScale = 1.0f, x0 = 0, z0 = 0, sx = 1, sz = 1;
	const C3D_Tex* tex1 = nullptr; // what unit 1 held for the last draw
};
static void sunHook(const mock3ds::DrawCall& call, void* user) {
	SunCheck& sc = *static_cast<SunCheck*>(user);
	sc.entry = call.entry;
	sc.tex1 = call.tex1;
	for (unsigned i = 0; i < call.count; ++i) {
		mock3ds::ShadedVertex out;
		if (!mock3ds::shadeVertex(call, call.indices[i], out, true)) continue;
		const WorldVertex& v = sc.verts[call.indices[i]];
		const double x = sc.origin[0] + double(v.x) * sc.posScale, z = sc.origin[2] + double(v.z) * sc.posScale;
		const double u = (x - sc.x0) * sc.sx, w = (z - sc.z0) * sc.sz;
		sc.worst = std::max(sc.worst, std::max(std::fabs(out.texcoord1[0] - u), std::fabs(out.texcoord1[1] - w))); // (fix143: coordinate 1)
		++sc.vertices;
	}
}
static void testSunMask() {
	mock3ds::reset();
	gfx::c3dSetClassicFrames(false);
	CHECK(gfx::init() && gfx::sunShadowSupported());
	// An 8x8 mask, and three corners of a chunk far from the world's origin (the anchor is near the eye).
	TexHeader th {};
	th.magic = TEX_MAGIC;
	th.width = th.height = 8;
	th.format = TEX_L4;
	th.mipCount = 1;
	th.flags = TEXF_CLAMP;
	th.dataSize = 32;
	u8 texels[32];
	std::memset(texels, 0x5A, sizeof(texels));
	gfx::Texture* mask = gfx::textureCreate(th, texels);
	CHECK(mask != nullptr);
	SunCheck sc;
	static const WorldVertex verts[3] = {{-4000, 0, -4000, 1, 0, 0, 200, 200, 200, 255}, {4000, 80, -4000, 1, 1024, 0, 200, 200, 200, 255},
	                                     {0, 160, 4000, 1, 0, 1024, 200, 200, 200, 255}};
	static const u16 idx[3] = {0, 1, 2};
	gfx::Mesh* mesh = gfx::meshCreate(verts, 3, idx, 3);
	sc.verts = verts;
	sc.origin[0] = -93000.0f;
	sc.origin[1] = 250.0f;
	sc.origin[2] = -116500.0f;
	sc.posScale = 0.25f;
	sc.x0 = -94100.0f;
	sc.z0 = -117600.0f;
	sc.sx = 1.0f / 4100.0f;
	sc.sz = 1.0f / 4100.0f;
	gfx::FrameDesc fd {};
	const Vec3 eye = {-92900.0f, 400.0f, -116900.0f};
	fd.view = viewLH(eye, 0.3f, -0.4f);
	fd.eye = eye;
	fd.fovY = 1.0f;
	fd.zNear = 20.0f;
	fd.zFar = 20000.0f;
	fd.fogStart = 3000.0f;
	fd.fogEnd = 9000.0f;
	fd.dayLight[0] = fd.dayLight[1] = fd.dayLight[2] = 1.0f;
	gfx::frameBegin(fd);
	gfx::setSunShadow(0.4f);
	gfx::setPass(gfx::PASS_WORLD);
	gfx::ChunkFx fx;
	fx.sun = mask;
	fx.sunX0 = sc.x0;
	fx.sunZ0 = sc.z0;
	fx.sunScaleX = sc.sx;
	fx.sunScaleZ = sc.sz;
	mock3ds::setDrawHook(sunHook, &sc);
	gfx::setChunkTransform(sc.origin, sc.posScale, 1.0f / 1024.0f, &fx);
	gfx::setMaterial(nullptr, 0);
	gfx::draw(mesh, 0, 3);
	const int withEntry = sc.entry;
	const double worst = sc.worst;
	const unsigned shaded = sc.vertices;
	// fix144: the mask is on unit 1 as GPU_A8 (nibble n = texel n: 0x5A gives 10, 5, 10, 5 ... times 17).
	const C3D_Tex* maskTex = sc.tex1;
	CHECK(maskTex && maskTex->fmt == GPU_A8 && maskTex->width == 8 && maskTex->height == 8 && maskTex->data &&
	      static_cast<const u8*>(maskTex->data)[0] == 170 && static_cast<const u8*>(maskTex->data)[1] == 85 &&
	      static_cast<const u8*>(maskTex->data)[63] == 85);
	// Without a mask: the short entry, and the mask no longer on unit 1.
	gfx::setChunkTransform(sc.origin, sc.posScale, 1.0f / 1024.0f, nullptr);
	gfx::draw(mesh, 0, 3);
	const int withoutEntry = sc.entry;
	CHECK(sc.tex1 && sc.tex1 != maskTex);
	mock3ds::setDrawHook(nullptr, nullptr);
	gfx::frameEnd();
	std::printf("  sun's mask: entry %d with it (%u vertices, texture coordinate 1 within %.2g), entry %d without\n", withEntry, shaded,
	            worst, withoutEntry);
	CHECK(withEntry == 4 && shaded == 3 && worst < 2e-3 && withoutEntry == 1);
	CHECK(!*mock3ds::shaderPartialOutputs());
	gfx::meshDestroy(mesh);
	gfx::textureDestroy(mask);
	gfx::shutdown();
	CHECK(mock3ds::violations() == 0);
}

// The classic shader (ZL+ZR at start): the one of the version before the dissolve (world.v.pica), projection and
// view apart, nothing on texture unit 1 - the GPU state that is known to work on the console. (The first build
// with the dissolve always on gave a black screen.) It was the default before the fast one.
// Smoothing of edges: a 480x800 target whose transfer averages 2x2, in both frame orders; the ordinary one without.
static void testSupersample(const std::string& pak) {
	for (int mode = 0; mode < 4; ++mode) {
		const bool classic = (mode & 1) != 0, super = (mode & 2) != 0;
		mock3ds::reset();
		gfx::c3dSetClassicFrames(classic);
		gfx::c3dSetSupersample(super);
		CHECK(gfx::init() && gfx::c3dSupersample() == super);
		CHECK(mock3ds::counters().outputWidth == (super ? 480u : 240u) && mock3ds::counters().outputScaling == (super ? 2u : 0u));
		App app;
		CHECK(app.init(pak.c_str(), nullptr));
		app.skipChapters();
		app.setDrawDistance(6000.0f);
		app.world().preload(app.camera().pos);
		for (int f = 0; f < 8; ++f) {
			app.update(Input {}, 1.0f / 30.0f);
			app.render();
		}
		CHECK(mock3ds::violations() == 0);
		app.shutdown();
		gfx::shutdown();
	}
	gfx::c3dSetClassicFrames(false);
	gfx::c3dSetSupersample(false);
}

// The wide screen (800x240): a 240x800 target, or 480x800 averaged along the 480 only when smoothed; no second eye; in both
// frame orders; a console without the wide mode keeps the ordinary screen; and the screen is put back at the end.
static void testWide(const std::string& pak) {
	for (int mode = 0; mode < 4; ++mode) {
		const bool classic = (mode & 1) != 0, super = (mode & 2) != 0;
		mock3ds::reset();
		gfx::c3dSetClassicFrames(classic);
		gfx::c3dSetSupersample(super);
		gfx::c3dSetStereo(true); // (asked for as well: the wide screen wins)
		gfx::c3dSetWide(true);
		CHECK(gfx::init() && gfx::c3dWide() && gfxIsWide() && !gfx::stereoAvailable());
		const mock3ds::Counters& c = mock3ds::counters();
		CHECK(c.outputWidth == (super ? 480u : 240u) && c.outputHeight == 800u && c.outputScaling == (super ? 1u : 0u));
		gfx::stereoSet(true);
		CHECK(!gfx::stereoActive());
		App app;
		CHECK(app.init(pak.c_str(), nullptr));
		app.skipChapters();
		app.setDrawDistance(6000.0f);
		app.world().preload(app.camera().pos);
		app.setStereo(0.8f);
		for (int f = 0; f < 8; ++f) {
			app.update(Input {}, 1.0f / 30.0f);
			app.render();
		}
		CHECK(c.framesShown >= 8 && c.framesShownRight == 0 && c.rightDraws == 0);
		CHECK(mock3ds::violations() == 0);
		app.shutdown();
		gfx::shutdown();
		CHECK(!gfxIsWide() && mock3ds::violations() == 0 && mock3ds::counters().linearBlocks == 0);
	}
	// No wide mode on this console: the ordinary screen, as asked for nothing.
	mock3ds::reset();
	mock3ds::setWideSupported(false);
	gfx::c3dSetSupersample(false);
	gfx::c3dSetStereo(false);
	gfx::c3dSetWide(true);
	CHECK(gfx::init() && !gfx::c3dWide());
	CHECK(mock3ds::counters().outputWidth == 240u && mock3ds::counters().outputHeight == 400u);
	gfx::shutdown();
	gfx::c3dSetWide(false);
	gfx::c3dSetStereo(false);
	gfx::c3dSetSupersample(false);
	gfx::c3dSetClassicFrames(false);
	CHECK(mock3ds::violations() == 0 && mock3ds::counters().linearBlocks == 0);
}

// Stereo 3D by two renders: the left and the right target, both copied to the screen every frame, the right one drawn into
// with its own draw calls; one frame list, in both frame orders; and nothing of it while the slider is down.
static void testStereo(const std::string& pak) {
	for (int classic = 0; classic < 2; ++classic) {
		mock3ds::reset();
		gfx::c3dSetClassicFrames(classic != 0);
		gfx::c3dSetStereo(true);
		CHECK(gfx::init() && gfx::stereoAvailable() && !gfx::stereoActive());
		App app;
		CHECK(app.init(pak.c_str(), nullptr));
		app.skipChapters();
		app.setDrawDistance(6000.0f);
		app.world().preload(app.camera().pos);
		// Slider down: one picture, the right target untouched.
		for (int f = 0; f < 4; ++f) {
			app.update(Input {}, 1.0f / 30.0f);
			app.render();
		}
		CHECK(mock3ds::counters().framesShownRight == 0 && mock3ds::counters().rightDraws == 0 && !gfx::stereoActive());
		const unsigned monoDraws = mock3ds::counters().draws;
		const unsigned monoFrames = mock3ds::counters().framesShown;
		// Slider up.
		gfx::stereoSet(true);
		app.setStereo(0.8f);
		CHECK(gfx::stereoActive() && mock3ds::counters().switches3D == 1);
		for (int f = 0; f < 6; ++f) {
			app.update(Input {}, 1.0f / 30.0f);
			app.render();
		}
		const mock3ds::Counters& c = mock3ds::counters();
		std::printf("  stereo (%s): %u draws in %u frames mono, then %u (%u for the right eye) in 6 stereo\n", classic ? "classic" : "overlapped",
		            monoDraws, monoFrames, c.draws - monoDraws, c.rightDraws);
		CHECK(c.framesShownRight == 6 && c.framesShown == monoFrames + 6);
		CHECK(c.rightDraws > 0 && c.draws - monoDraws > c.rightDraws);
		CHECK(c.rightDraws * 10 > (c.draws - monoDraws) * 4); // (the right eye gets about half of them)
		CHECK(mock3ds::violations() == 0);
		// Slider down again.
		gfx::stereoSet(false);
		app.setStereo(0.0f);
		const unsigned rightBefore = c.framesShownRight;
		for (int f = 0; f < 3; ++f) {
			app.update(Input {}, 1.0f / 30.0f);
			app.render();
		}
		CHECK(c.framesShownRight == rightBefore && mock3ds::counters().switches3D == 2 && mock3ds::violations() == 0);
		app.shutdown();
		gfx::shutdown();
		CHECK(mock3ds::counters().linearBlocks == 0);
	}
	// The light smoothing: a 480x400 target averaged in pairs (twice the pixels, not four times).
	for (int classic = 0; classic < 2; ++classic) {
		mock3ds::reset();
		gfx::c3dSetClassicFrames(classic != 0);
		gfx::c3dSetSupersampleMode(1);
		CHECK(gfx::init() && gfx::c3dSupersample() && gfx::c3dSupersampleLight());
		CHECK(mock3ds::counters().outputWidth == 480u && mock3ds::counters().outputScaling == 1u);
		gfx::shutdown();
		mock3ds::reset();
		gfx::c3dSetSupersampleMode(2);
		CHECK(gfx::init() && gfx::c3dSupersample() && !gfx::c3dSupersampleLight() && mock3ds::counters().outputScaling == 2u);
		gfx::shutdown();
		gfx::c3dSetSupersampleMode(0);
	}
	gfx::c3dSetClassicFrames(false);
	CHECK(mock3ds::violations() == 0);
	// Not together with the smoothing: no second eye, and nothing breaks.
	mock3ds::reset();
	gfx::c3dSetStereo(true);
	gfx::c3dSetSupersample(true);
	CHECK(gfx::init() && !gfx::stereoAvailable());
	gfx::stereoSet(true);
	CHECK(!gfx::stereoActive());
	gfx::shutdown();
	gfx::c3dSetStereo(false);
	gfx::c3dSetSupersample(false);
	gfx::c3dSetClassicFrames(false);
	CHECK(mock3ds::violations() == 0 && mock3ds::counters().linearBlocks == 0);
}

static void testClassicShader(const std::string& pak) {
	mock3ds::reset();
	gfx::c3dSetClassicFrames(false);
	gfx::c3dSetClassicShader(true);
	CHECK(gfx::init() && !gfx::fadeSupported() && !gfx::c3dFastShader());
	std::printf("  classic shader: %u instructions, %u of 96 float registers%s%s\n", mock3ds::shaderInstructions(),
	            mock3ds::shaderFloatRegisters(), *mock3ds::shaderProblem() ? "; PROBLEM: " : "", mock3ds::shaderProblem());
	CHECK(!*mock3ds::shaderProblem() && mock3ds::shaderFloatRegisters() == 4 + 4 + 5 + MODEL_PALETTE * 3 + 1 + 1);
	shaderProgram_s probe;
	shaderProgramInit(&probe);
	shaderProgramSetVsh(&probe, nullptr);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "skinned") == 0x68);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "fading") < 0);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "fx") == 91);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "windy") == 0x69);
	// (the classic shader has no waves: the water is drawn flat)
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "wavy") < 0);
	{
		ShaderCheck sc;
		sc.windyBit = 2;
		App app;
		CHECK(app.init(pak.c_str(), nullptr));
		app.skipChapters();
		app.world().config().fadeMs = 0.0f; // (as the console sets it without D-Pad Right)
		app.setDrawDistance(6000.0f);
		app.world().preload(app.camera().pos);
		for (int f = 0; f < 20; ++f) {
			Input in;
			in.moveZ = 1.0f;
			in.lookX = 0.4f;
			app.update(in, 1.0f / 30.0f);
			if (f == 19) {
				const Camera& cam = app.camera();
				sc.beginFrame(viewLH(cam.pos, cam.yaw, cam.pitch), cam.pos, cam.fovY, cam.zNear, cam.zFar);
				sc.expect(app.hero().model(), app.hero().pose(), app.hero().feet(), app.hero().yaw(), app.hero().light(), app.hero().lean());
				sc.others = app.npcs().stats().drawCalls + app.loot().stats().drawCalls; // (and the things lying about)
				mock3ds::setDrawHook(shaderHook, &sc);
			}
			app.render();
			mock3ds::setDrawHook(nullptr, nullptr);
			CHECK(app.world().stats().batchesFading == 0);
		}
		CHECK(sc.worldDraws > 10 && sc.worldVertices > 1000 && sc.failed == 0 && sc.unexpected == 0);
		CHECK(sc.worstWorldPx < 0.02 && sc.worstGpuWorldPx < 0.25 && sc.worstGpuSkyPx < 0.75 && sc.worstColor < 1e-4 && sc.worstTex < 1e-3);
		app.shutdown();
	}
	CHECK(mock3ds::counters().texUnit1Binds <= 1 && mock3ds::counters().programSwitches == 0); // (fix143: the white stand-in, once)
	gfx::shutdown();
	gfx::c3dSetClassicShader(false);
	CHECK(mock3ds::violations() == 0 && mock3ds::counters().linearBlocks == 0);
}

// fix148: the lamps' entry (world_lamp.v.pica, DVLE 5). A draw with lamps set is drawn by it, and its colours are the engine's
// maths: the baked light plus every lamp's colour * gfx::lampFalloff(distance^2, range) * (1 + weight * (keep - 1)) - worked out
// here from the lamps' world places, so the packing into the palette's rows (relative to the anchor) is checked too; the
// same after a new frame moved the anchor. Without lamps, with the sun's mask, or with `nolampshader`, another entry.
struct LampCheck {
	int entry = -1;
	unsigned vertices = 0, lit = 0;
	double worst = 0.0;
	const WorldVertex* verts = nullptr;
	float origin[3] = {0, 0, 0}, posScale = 1.0f;
	gfx::LampSet set;
	bool partial = false;
};
static void lampHook(const mock3ds::DrawCall& call, void* user) {
	LampCheck& lc = *static_cast<LampCheck*>(user);
	lc.entry = call.entry;
	if (call.entry != 5) return;
	for (unsigned i = 0; i < call.count; ++i) {
		mock3ds::ShadedVertex out;
		if (!mock3ds::shadeVertex(call, call.indices[i], out, false)) {
			lc.partial = true;
			continue;
		}
		const WorldVertex& v = lc.verts[call.indices[i]];
		const double at[3] = {lc.origin[0] + double(v.x) * lc.posScale, lc.origin[1] + double(v.y) * lc.posScale, lc.origin[2] + double(v.z) * lc.posScale};
		const double keep = 1.0 + double(v.a) / 255.0 * (double(lc.set.dayKeep) - 1.0);
		const u8 rgb[3] = {v.r, v.g, v.b};
		double want[3] = {rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0};
		bool any = false;
		for (u32 k = 0; k < lc.set.count; ++k) {
			const gfx::DrawLamp& l = lc.set.lamp[k];
			double d2 = 0.0;
			for (int a = 0; a < 3; ++a) d2 += (double(l.pos[a]) - at[a]) * (double(l.pos[a]) - at[a]);
			const double f = double(gfx::lampFalloff(float(d2), l.range)) * keep;
			any = any || f > 0.0;
			for (int c = 0; c < 3; ++c) want[c] += double(l.rgb[c]) * f;
		}
		lc.lit += any;
		for (int c = 0; c < 3; ++c) lc.worst = std::max(lc.worst, std::fabs(double(out.color[c]) - want[c]));
		++lc.vertices;
	}
}
static void testLampEntry() {
	mock3ds::reset();
	gfx::c3dSetClassicFrames(false);
	gfx::c3dSetFadeShader(true); // (as the console: the dissolve is on by default)
	CHECK(gfx::init() && gfx::lampsSupported());
	static const WorldVertex verts[3] = {{-4000, 0, -4000, 0, 0, 0, 120, 90, 60, 255}, {4000, 80, -4000, 0, 1024, 0, 40, 40, 40, 128},
	                                     {0, 160, 4000, 0, 0, 1024, 200, 200, 200, 0}};
	static const u16 idx[3] = {0, 1, 2};
	gfx::Mesh* mesh = gfx::meshCreate(verts, 3, idx, 3);
	LampCheck lc;
	lc.verts = verts;
	lc.origin[0] = -93000.0f;
	lc.origin[1] = 250.0f;
	lc.origin[2] = -116500.0f;
	lc.posScale = 0.25f;
	// Two lamps near the first and the second corner, one out of reach of all three.
	lc.set.count = 3;
	lc.set.dayKeep = 0.4f;
	const float near0[3] = {-93000.0f - 1000.0f + 150.0f, 250.0f + 60.0f, -116500.0f - 1000.0f + 100.0f};
	const float near1[3] = {-93000.0f + 1000.0f - 300.0f, 270.0f, -116500.0f - 1000.0f};
	const float far[3] = {-80000.0f, 250.0f, -116500.0f};
	const float* places[3] = {near0, near1, far};
	const float ranges[3] = {800.0f, 650.0f, 900.0f};
	const float colours[3][3] = {{0.9f, 0.6f, 0.3f}, {-0.1f, -0.05f, 0.0f}, {1.0f, 1.0f, 1.0f}};
	for (int k = 0; k < 3; ++k) {
		for (int a = 0; a < 3; ++a) {
			lc.set.lamp[k].pos[a] = places[k][a];
			lc.set.lamp[k].rgb[a] = colours[k][a];
		}
		lc.set.lamp[k].range = ranges[k];
	}
	gfx::FrameDesc fd {};
	Vec3 eye = {-92900.0f, 400.0f, -116900.0f};
	fd.view = viewLH(eye, 0.3f, -0.4f);
	fd.eye = eye;
	fd.fovY = 1.0f;
	fd.zNear = 20.0f;
	fd.zFar = 20000.0f;
	fd.fogStart = 3000.0f;
	fd.fogEnd = 9000.0f;
	fd.dayLight[0] = fd.dayLight[1] = fd.dayLight[2] = 1.0f;
	mock3ds::setDrawHook(lampHook, &lc);
	int entries[6] = {-1, -1, -1, -1, -1, -1};
	for (int frame = 0; frame < 2; ++frame) {
		gfx::frameBegin(fd);
		gfx::setPass(gfx::PASS_WORLD);
		gfx::setChunkTransform(lc.origin, lc.posScale, 1.0f / 1024.0f, nullptr);
		gfx::setMaterial(nullptr, 0);
		gfx::setLamps(&lc.set);
		gfx::draw(mesh, 0, 3);
		entries[frame] = lc.entry;
		// (A dissolving draw takes the lamps too: the entry has `fadefast`'s branch.)
		gfx::setFade(0.5f);
		gfx::draw(mesh, 0, 3);
		gfx::setFade(1.0f);
		if (frame == 1) {
			gfx::setLamps(nullptr);
			gfx::draw(mesh, 0, 3);
			entries[2] = lc.entry;
			// What sways is the full entry's, lamps or not.
			gfx::setLamps(&lc.set);
			gfx::setMaterial(nullptr, MATF_WIND);
			gfx::draw(mesh, 0, 3);
			entries[3] = lc.entry;
			gfx::setMaterial(nullptr, 0);
			gfx::c3dSetNoLamps(true);
			gfx::draw(mesh, 0, 3);
			entries[4] = lc.entry;
			gfx::c3dSetNoLamps(false);
			gfx::draw(mesh, 0, 3);
			entries[5] = lc.entry;
		}
		gfx::frameEnd();
		// The next frame from elsewhere: another anchor, the rows sent again.
		eye = {-90100.0f, 900.0f, -113300.0f};
		fd.view = viewLH(eye, 1.1f, -0.2f);
		fd.eye = eye;
	}
	mock3ds::setDrawHook(nullptr, nullptr);
	std::printf("  lamps' entry: %u corners (%u lit), colours within %.2g; entries %d %d with lamps, %d without, %d swaying, %d nolampshader, %d again\n",
	            lc.vertices, lc.lit, lc.worst, entries[0], entries[1], entries[2], entries[3], entries[4], entries[5]);
	CHECK(entries[0] == 5 && entries[1] == 5 && entries[2] == 2 && entries[3] == 0 && entries[4] == 2 && entries[5] == 5);
	CHECK(lc.vertices >= 15 && lc.lit >= 10 && lc.worst < 2e-4 && !lc.partial);
	CHECK(!*mock3ds::shaderPartialOutputs());
	gfx::frameStats(); // (lampDraws: the console's own count)
	gfx::meshDestroy(mesh);
	gfx::shutdown();
	gfx::c3dSetFadeShader(false);
	CHECK(mock3ds::violations() == 0);
}

// The default: the shader with two entry points (world_full.v.pica, world_fast.v.pica), one combined matrix. The
// fast entry draws the static world and the overlay, the full one the characters and what sways in the wind; the
// vertices must come out as the engine's maths wants them from whichever entry, and the right one must be bound.
static void testDefaultShader(const std::string& pak) {
	mock3ds::reset();
	gfx::c3dSetClassicFrames(false);
	CHECK(gfx::init() && !gfx::fadeSupported() && gfx::c3dFastShader());
	std::printf("  default shader: %u entry points, %u and %u instructions of %u, %u of 96 float registers%s%s\n", mock3ds::shaderEntries(),
	            mock3ds::shaderEntryInstructions(0), mock3ds::shaderEntryInstructions(1), mock3ds::shaderInstructions(),
	            mock3ds::shaderFloatRegisters(), *mock3ds::shaderProblem() ? "; PROBLEM: " : "", mock3ds::shaderProblem());
	CHECK(!*mock3ds::shaderProblem() && mock3ds::shaderEntries() == 6);
	// (mvp, fogRow, five vectors, the palette, fx, lightK, aoK, wave; four constants: consts, noiseScale, the waves' two)
	CHECK(mock3ds::shaderFloatRegisters() == 4 + 1 + 5 + MODEL_PALETTE * 3 + 1 + 1 + 1 + 1 + 4);
	// main, fastmain, fadefast (with the dissolve: 3 more), skinmain (both ways of the dissolve counted)
	std::printf("  entries: %u, %u, %u and %u instructions\n", mock3ds::shaderEntryInstructions(0), mock3ds::shaderEntryInstructions(1),
	            mock3ds::shaderEntryInstructions(2), mock3ds::shaderEntryInstructions(3));
	CHECK(mock3ds::shaderEntryInstructions(1) <= 20 && mock3ds::shaderEntryInstructions(0) < 130); // (fix140: the sheen made it 125) // (the water waves are most of the full entry: it draws only what waves or sways)
	CHECK(mock3ds::shaderEntryInstructions(2) <= 28 && mock3ds::shaderEntryInstructions(3) <= 32);
	// The sun's mask: four instructions more, in entries of their own (chunks without a mask keep the short ones; fix145: the
	// coordinate is built in a register and written whole, zw too).
	CHECK(mock3ds::shaderEntryInstructions(4) == mock3ds::shaderEntryInstructions(1) + 4);
	// fix148: entry 5 is the lamps' (`fadefast` and four lamps, about 7 instructions each), in place of `fadefastsun`.
	std::printf("  lamps' entry: %u instructions\n", mock3ds::shaderEntryInstructions(5));
	CHECK(mock3ds::shaderEntryInstructions(5) > mock3ds::shaderEntryInstructions(2) + 24 && mock3ds::shaderEntryInstructions(5) <= 64);
	shaderProgram_s probe;
	shaderProgramInit(&probe);
	shaderProgramSetVsh(&probe, nullptr);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "mvp") == 0);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "fogRow") == 4);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "chunk") == 5);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "params") == 6);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "dayDelta") == 7);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "texOffset") == 8);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "bones") == 10);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "fx") == 88);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "lightK") == 89);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "aoK") == 90);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "skinned") == 0x68);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "windy") == 0x69);
	// (the water's waves: `wave` after aoK and the bool `wavy` after `fading`, in every entry's source alike)
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "wavy") == 0x6b);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "wave") == shaderInstanceGetUniformLocation(probe.vertexShader, "aoK") + 1);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "projection") < 0);
	{
		ShaderCheck sc;
		sc.windyBit = 2;
		sc.wavyBit = 8;
		sc.locChunk = 5;
		sc.locParams = 6;
		sc.locDayDelta = 7;
		sc.locTexOffset = 8;
		sc.locBones = 10;
		sc.locFx = 88;
		sc.twoEntries = true;
		App app;
		CHECK(app.init(pak.c_str(), nullptr));
		app.skipChapters();
		app.world().config().fadeMs = 0.0f; // (as the console sets it without D-Pad Right)
		app.setDrawDistance(6000.0f);
		app.environment().timeOfDay = 19.5f;
		CHECK(app.world().hasOcclusion() && app.world().hasWind());
		app.world().config().occlusion = 1.0f;
		app.world().config().wind = 2.0f;
		app.world().config().waves = 22.0f;
		app.world().preload(app.camera().pos);
		// Rain: the ground gets wet and puddles are laid where the ground is open and flat.
		app.weather().configure(WEATHER_RAIN, true);
		app.weather().settle();
		unsigned puddled = 0, rippled = 0, drops = 0;
		for (int f = 0; f < 6; ++f) {
			Input in;
			app.update(in, 1.0f / 30.0f);
			app.render();
			puddled = std::max(puddled, app.puddles().drawn());
			rippled = std::max(rippled, app.rain().rings());
			drops = std::max(drops, app.rain().drops());
		}
		std::printf("  rain: %s, %u puddles, %u splashes, %u drops in view\n", app.world().indoor() ? "indoors" : "outdoors", puddled, rippled, drops);
		CHECK(app.world().indoor() || (puddled > 0 && drops > 20));
		app.weather().configure(WEATHER_CLEAR, true);
		app.weather().settle();
		unsigned switches = 0, frames = 0;
		for (int f = 0; f < 50; ++f) {
			Input in;
			in.moveZ = 1.0f;
			in.moveX = f > 20 ? 0.6f : 0.0f;
			in.lookX = 0.4f;
			in.lookY = f < 10 ? -0.3f : 0.0f;
			app.update(in, 1.0f / 30.0f);
			const bool check = f % 7 == 6;
			if (check) {
				app.render();
				const Camera& cam = app.camera();
				sc.beginFrame(viewLH(cam.pos, cam.yaw, cam.pitch), cam.pos, cam.fovY, cam.zNear, cam.zFar);
				sc.expect(app.hero().model(), app.hero().pose(), app.hero().feet(), app.hero().yaw(), app.hero().light(), app.hero().lean());
				sc.others = app.npcs().stats().drawCalls + app.loot().stats().drawCalls; // (and the things lying about)
				mock3ds::setDrawHook(shaderHook, &sc);
			}
			app.render();
			mock3ds::setDrawHook(nullptr, nullptr);
			if (check) {
				CHECK(app.heroVisible() && sc.nextPart == app.hero().model().partCount() && sc.others == 0);
				switches += gfx::frameStats().programSwitches;
				++frames;
			}
		}
		std::printf("  world: %u draw calls (%u by the fast entry, %u by the full one), %u vertices: within %.4f px (%.4f px in 24-bit floats); "
		            "depth within %.1e, colour and fog within %.1e, texture coordinates within %.1e\n",
		            sc.worldDraws, sc.fastDraws, sc.fullDraws, sc.worldVertices, sc.worstWorldPx, sc.worstGpuWorldPx, sc.worstDepth,
		            sc.worstColor, sc.worstTex);
		std::printf("  hero: %u draw calls, %u vertices: within %.4f px; seams: %u shared points, %u split; %u program changes in %u frames\n",
		            sc.skinnedDraws, sc.skinnedVertices, sc.worstSkinPx, sc.sharedPoints, sc.splitPoints, switches, frames);
		std::printf("  lamps: %u draw calls by their entry, %u corners lit (checked with the colours above)\n", sc.lampDraws, sc.litVertices);
		CHECK(sc.worldDraws > 50 && sc.worldVertices > 5000 && sc.skinnedDraws >= 7 && sc.skinnedVertices > 500 && sc.blendedVertices > 50);
		CHECK(sc.failed == 0 && sc.unexpected == 0);
		CHECK(sc.fastDraws > 50 && sc.skinDraws >= sc.skinnedDraws && sc.wrongEntry == 0 && sc.swayOnFast == 0);
		CHECK(sc.worstWorldPx < 0.02 && sc.worstSkinPx < 0.02);
		CHECK(sc.worstGpuWorldPx < 0.25 && sc.worstGpuSkinPx < 0.25 && sc.worstGpuSkyPx < 0.75);
		CHECK(sc.worstDepth < 1e-3 && sc.worstColor < 1e-4 && sc.worstTex < 1e-3);
		CHECK(sc.sharedPoints > 20 && sc.splitPoints == 0);
		CHECK(sc.occludedVertices > 1000 && sc.swayedVertices > 50 && sc.windyDraws > 0 && sc.worstSway > 0.5 && sc.worstSway < 140.0); // (World kWindGain 3.5: a crown sways tens of cm)
		// The water's waves: where there is water in view, it waved with the full entry (never the fast one, which would
		// read the waves' weights as occlusion); and the report counts what waved.
		std::printf("  waves: %u wavy draw calls, %u of them on the fast entry; %u triangles in the last frame\n", sc.wavyDraws, sc.wavyOnFast,
		            app.world().stats().waveTriangles);
		std::printf("  waves: %u waved corners, the highest lifted %.1f cm; the sheen on %u corners\n", sc.wavedVertices, sc.worstWave, sc.sheenVertices);
		CHECK(app.world().hasWaterGrid() && sc.wavyOnFast == 0 && sc.wavedVertices > 20 && sc.sheenVertices >= sc.wavedVertices && sc.worstWave > 3.0 && sc.worstWave <= 22.01);
		// The program changes are not free (a few dozen words of commands each); they stay in the tens a frame.
		CHECK(switches > 0 && switches < frames * 120);
		app.shutdown();
	}
	CHECK(mock3ds::counters().texUnit1Binds <= 1); // (fix143: unit 1 holds the white stand-in without the dissolve: once)
	gfx::shutdown();
	CHECK(mock3ds::violations() == 0 && mock3ds::counters().linearBlocks == 0);
}

// The new grass (fix147) through the console's graphics code: its draw calls are cut-outs of the full entry (it sways), every
// corner comes out of the shader as numbers in the screen's reach, its meshes break none of the GPU's memory rules, and
// they are all freed at the end. Only texture unit 0 (no new unit, no third texture coordinate set: the sun's lesson).
struct GrassCheck {
	unsigned draws = 0, vertices = 0, bad = 0, partial = 0, fastEntry = -1, onFast = 0;
	unsigned tex1Changes = 0;
	const C3D_Tex* tex1 = nullptr;
	bool tex1Seen = false;
};
static void grassHook(const mock3ds::DrawCall& c, void* user) {
	GrassCheck& g = *static_cast<GrassCheck*>(user);
	if (!gTrace.stage || std::strncmp(gTrace.stage, "grass", 5) != 0) return;
	++g.draws;
	if (g.tex1Seen && c.tex1 != g.tex1) ++g.tex1Changes;
	g.tex1 = c.tex1;
	g.tex1Seen = true;
	const unsigned n = c.stride ? unsigned(mock3ds::blockBytes(c.vertices) / c.stride) : 0;
	for (unsigned i = 0; i < c.count; i += 3) {
		const unsigned index = c.indices[i];
		if (index >= n) {
			++g.bad;
			continue;
		}
		mock3ds::ShadedVertex v;
		if (!mock3ds::shadeVertex(c, index, v, true)) {
			++g.bad;
			continue;
		}
		++g.vertices;
		for (int k = 0; k < 4; ++k)
			if (!std::isfinite(v.position[k]) || !std::isfinite(v.color[k])) ++g.bad;
		if (v.position[3] <= 0.0f && (std::fabs(v.position[0]) > 1e5f || std::fabs(v.position[1]) > 1e5f)) ++g.bad;
	}
	if (*mock3ds::shaderPartialOutputs()) ++g.partial;
}

static void testGrass(const std::string& pak) {
	mock3ds::reset();
	gfx::c3dSetClassicFrames(false);
	CHECK(gfx::init());
	{
		App app;
		CHECK(app.init(pak.c_str(), nullptr));
		CHECK(app.grass().available());
		app.skipChapters();
		app.setDrawDistance(6000.0f);
		app.environment().timeOfDay = 13.0f;
		app.settings().grass = 2;
		app.settings().wind = 2;
		app.applySettings();
		app.world().preload(app.camera().pos);
		GrassCheck gc;
		unsigned most = 0;
		for (int f = 0; f < 50; ++f) {
			Input in;
			in.moveZ = f > 20 ? 1.0f : 0.0f;
			in.lookX = f > 35 ? 0.3f : 0.0f;
			app.update(in, 1.0f / 30.0f);
			mock3ds::setDrawHook(grassHook, &gc);
			app.render();
			mock3ds::setDrawHook(nullptr, nullptr);
			most = std::max(most, app.grass().stats().tufts);
		}
		std::printf("  grass: %u draw calls, %u corners shaded in 24-bit floats, %u bad, %u with outputs left unwritten; up to %u tufts a frame; "
		            "unit 1 changed %u times between them\n",
		            gc.draws, gc.vertices, gc.bad, gc.partial, most, gc.tex1Changes);
		CHECK(gc.draws > 0 && gc.vertices > 1000 && gc.bad == 0 && gc.partial == 0 && most > 100 && gc.tex1Changes == 0);
		app.shutdown();
	}
	gfx::shutdown();
	CHECK(mock3ds::violations() == 0 && mock3ds::counters().linearBlocks == 0);
}

static void countSkinned(const mock3ds::DrawCall& call, void* user) { *static_cast<unsigned*>(user) += call.bools & 1; }

// The fallback: the world-only shader of the versions before characters.
// The world comes out the same, the character is simply not drawn.
static void testPlainShader(const std::string& pak) {
	mock3ds::reset();
	gfx::c3dSetClassicFrames(false);
	gfx::c3dSetPlainShader(true);
	CHECK(gfx::init() && gfx::c3dPlainShader());
	std::printf("  plain shader: %u instructions, %u of 96 float registers%s%s\n", mock3ds::shaderInstructions(),
	            mock3ds::shaderFloatRegisters(), *mock3ds::shaderProblem() ? "; PROBLEM: " : "", mock3ds::shaderProblem());
	CHECK(!*mock3ds::shaderProblem() && mock3ds::shaderFloatRegisters() == 4 + 4 + 4 + 1);
	shaderProgram_s probe;
	shaderProgramInit(&probe);
	shaderProgramSetVsh(&probe, nullptr);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "chunk") == 8);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "texOffset") == 11);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "bones") < 0);
	{
		ShaderCheck sc;
		App app;
		CHECK(app.init(pak.c_str(), nullptr));
		app.skipChapters();
		app.setDrawDistance(6000.0f);
		app.environment().timeOfDay = 19.5f;
		app.world().preload(app.camera().pos);
		for (int f = 0; f < 20; ++f) {
			Input in;
			in.moveZ = 1.0f;
			in.lookX = 0.4f;
			app.update(in, 1.0f / 30.0f);
			if (f == 19) {
				const Camera& cam = app.camera();
				sc.beginFrame(viewLH(cam.pos, cam.yaw, cam.pitch), cam.pos, cam.fovY, cam.zNear, cam.zFar);
				mock3ds::setDrawHook(shaderHook, &sc);
			}
			app.render();
			mock3ds::setDrawHook(nullptr, nullptr);
		}
		std::printf("  world: %u draw calls, %u vertices: within %.4f px (%.4f px in 24-bit floats), %u shared points, %u split\n",
		            sc.worldDraws, sc.worldVertices, sc.worstWorldPx, sc.worstGpuWorldPx, sc.sharedPoints, sc.splitPoints);
		CHECK(sc.worldDraws > 10 && sc.worldVertices > 1000 && sc.skinnedDraws == 0 && sc.failed == 0 && sc.unexpected == 0);
		CHECK(!app.heroVisible() || app.hero().loaded()); // the character is still simulated, just not drawn
		CHECK(sc.worstWorldPx < 0.02 && sc.worstGpuWorldPx < 0.25 && sc.worstGpuSkyPx < 0.75 && sc.worstColor < 1e-4 && sc.worstTex < 1e-3 && sc.splitPoints == 0);
		app.shutdown();
	}
	gfx::shutdown();
	gfx::c3dSetPlainShader(false);
	CHECK(mock3ds::violations() == 0 && mock3ds::counters().linearBlocks == 0);
}


// The overlay (HUD, pause menu): flat quads in screen pixels through the
// world's shader with an orthographic projection turned for the 3DS's
// sideways screen. A quad's corners must land on their pixels, its colour
// must be the vertex's, its texture coordinates the texels of the font.
struct UiCheck {
	bool fast = false; // the fast shader: chunk and params are elsewhere, and the matrix is the orthographic one itself
	unsigned draws = 0, vertices = 0, failed = 0, notFastEntry = 0;
	double worstPx = 0, worstColor = 0, worstTex = 0, worstDepth = 0;
	float texW = 0, texH = 0;
};

static void uiHook(const mock3ds::DrawCall& call, void* user) {
	UiCheck& uc = *static_cast<UiCheck*>(user);
	const float* chunk = call.uniforms[uc.fast ? 5 : 8];
	const float* params = call.uniforms[uc.fast ? 6 : 9];
	// The overlay's draws: unit chunk transform, no fog, identity model view (c4..c7; with the fast shader
	// the matrix is the orthographic projection alone, and its fogRow is 0 0 0 1).
	const float* mv = call.uniforms[4];
	const float* wRow = call.uniforms[3]; // an orthographic projection's last row is 0 0 0 1
	const bool identity = uc.fast ? call.uniforms[4][0] == 0 && call.uniforms[4][1] == 0 && call.uniforms[4][2] == 0 && call.uniforms[4][3] == 1 &&
	                                    wRow[0] == 0 && wRow[1] == 0 && wRow[2] == 0 && wRow[3] == 1
	                              : mv[0] == 1 && mv[1] == 0 && call.uniforms[5][1] == 1 && call.uniforms[6][2] == 1;
	const bool ui = chunk[0] == 0 && chunk[1] == 0 && chunk[2] == 0 && chunk[3] == 1 && params[2] == 1 && params[3] == 0 && identity &&
	                (call.bools & 1) == 0;
	if (!ui) return;
	++uc.draws;
	uc.notFastEntry += uc.fast && call.entry != 1;
	for (unsigned i = 0; i < call.count; ++i) {
		const u16 index = call.indices[i];
		WorldVertex v;
		std::memcpy(&v, static_cast<const u8*>(call.vertices) + size_t(index) * call.stride, sizeof(v));
		mock3ds::ShadedVertex got;
		if (!mock3ds::shadeVertex(call, index, got, true)) {
			++uc.failed;
			continue;
		}
		++uc.vertices;
		const double w = got.position[3];
		// 400 x 240 pixels, y down; the screen is the framebuffer turned: its x along the 240.
		const double wantX = 1.0 - double(v.y) / 120.0, wantY = 1.0 - double(v.x) / 200.0;
		uc.worstPx = std::max({uc.worstPx, std::fabs(got.position[0] / w - wantX) * 120.0, std::fabs(got.position[1] / w - wantY) * 200.0});
		const double z = got.position[2] / w;
		uc.worstDepth = std::max(uc.worstDepth, z < -1.0 ? -1.0 - z : z > 0.0 ? z : 0.0);
		const u8 rgb[3] = {v.r, v.g, v.b};
		for (int k = 0; k < 3; ++k) uc.worstColor = std::max(uc.worstColor, std::fabs(double(got.color[k]) - rgb[k] / 255.0));
		if (uc.texW > 0) {
			uc.worstTex = std::max({uc.worstTex, std::fabs(double(got.texcoord[0]) * uc.texW - v.u / 16.0),
			                        std::fabs(double(got.texcoord[1]) * uc.texH - v.v / 16.0 * uc.texH / uc.texW)});
		}
	}
}

static void testOverlay(const std::string& pak) {
	mock3ds::reset();
	gfx::c3dSetClassicFrames(false);
	CHECK(gfx::init());
	// 1. Quads of known places, straight through the backend.
	{
		u8 coverage[32 * 16];
		for (int i = 0; i < 32 * 16; ++i) coverage[i] = u8(i * 7);
		gfx::Texture* t = gfx::uiTextureCreate(32, 16, coverage);
		CHECK(t != nullptr);
		const gfx::MemStats before = gfx::memStats();
		UiCheck uc;
		uc.fast = gfx::c3dFastShader();
		uc.texW = 32;
		uc.texH = 16;
		mock3ds::setDrawHook(uiHook, &uc);
		for (int f = 0; f < 4; ++f) {
			gfx::FrameDesc fd {};
			fd.view = viewLH({0, 0, 0}, 0, 0);
			fd.fovY = 1.0f;
			fd.zNear = 10.0f;
			fd.zFar = 1000.0f;
			fd.fogStart = 900.0f;
			fd.fogEnd = 1000.0f;
			fd.dayLight[0] = fd.dayLight[1] = fd.dayLight[2] = 1.0f;
			gfx::frameBegin(fd);
			gfx::UiQuad q[3] = {
			    {0, 0, 400, 240, 0, 0, 32, 16, {255, 0, 0, 255}},
			    {10, 20, 42, 36, 3, 4, 7, 9, {0, 128, 255, 128}},
			    {-5, 230, 13, 250, 31, 15, 32, 16, {1, 2, 3, 40}},
			};
			gfx::uiDraw(t, q, 3);
			gfx::frameEnd();
		}
		mock3ds::setDrawHook(nullptr, nullptr);
		std::printf("  overlay: %u draw calls, %u vertices: within %.4f px, colour %.5f, texels %.4f\n", uc.draws, uc.vertices,
		            uc.worstPx, uc.worstColor, uc.worstTex);
		// Three quads, two alphas runs and one more: three draws a frame.
		CHECK(uc.draws == 12 && uc.vertices == 4 * 18 && uc.failed == 0 && uc.notFastEntry == 0);
		CHECK(uc.worstPx < 0.02 && uc.worstColor < 1e-3 && uc.worstTex < 0.01 && uc.worstDepth == 0);
		CHECK(gfx::memStats().textureBytes == before.textureBytes); // not world memory
		gfx::textureDestroy(t);
	}
	// 1b. A picture in colour (the chapter card): made, drawn over the whole screen, taken away.
	{
		u16 px[16 * 8];
		for (int i = 0; i < 16 * 8; ++i) px[i] = u16(i * 523);
		gfx::Texture* t = gfx::uiTextureCreateColor(16, 8, px);
		CHECK(t != nullptr);
		CHECK(gfx::uiTextureCreateColor(12, 8, px) == nullptr); // (not a power of two)
		UiCheck uc;
		uc.fast = gfx::c3dFastShader();
		uc.texW = 16;
		uc.texH = 8;
		mock3ds::setDrawHook(uiHook, &uc);
		gfx::FrameDesc fd {};
		fd.view = viewLH({0, 0, 0}, 0, 0);
		fd.fovY = 1.0f;
		fd.zNear = 10.0f;
		fd.zFar = 1000.0f;
		fd.fogStart = 900.0f;
		fd.fogEnd = 1000.0f;
		fd.dayLight[0] = fd.dayLight[1] = fd.dayLight[2] = 1.0f;
		gfx::frameBegin(fd);
		gfx::UiQuad q = {0, 0, 400, 240, 0, 0, 16, 8, {255, 255, 255, 200}};
		gfx::uiDraw(t, &q, 1);
		gfx::frameEnd();
		mock3ds::setDrawHook(nullptr, nullptr);
		std::printf("  colour picture: %u draws, %u vertices, %u failed, %.4f px\n", uc.draws, uc.vertices, uc.failed, uc.worstPx);
		CHECK(uc.draws == 1 && uc.vertices == 6 && uc.failed == 0 && uc.worstPx < 0.02);
		gfx::textureDestroy(t);
	}
	// 2. The application's overlay: the HUD, a message, the menu open over a
	// running world, both frame buffers of quads in turn.
	{
		App app;
		CHECK(app.init(pak.c_str(), nullptr));
		app.skipChapters();
		app.setDrawDistance(6000.0f);
		app.world().preload(app.camera().pos);
		UiCheck uc;
		uc.fast = gfx::c3dFastShader();
		mock3ds::setDrawHook(uiHook, &uc);
		for (int f = 0; f < 60; ++f) {
			Input in;
			in.moveZ = 1.0f;
			if (f == 10) app.toast("Проверка: ёжик «ü»");
			if (f == 30) app.menu().open();
			app.update(in, 1.0f / 60.0f);
			app.render();
		}
		mock3ds::setDrawHook(nullptr, nullptr);
		std::printf("  application overlay: %u draw calls, %u vertices: within %.4f px, colour %.5f; %u quads dropped\n", uc.draws,
		            uc.vertices, uc.worstPx, uc.worstColor, app.ui().dropped());
		CHECK(app.ui().ready() && uc.draws >= 30 && uc.vertices > 30 * 40 && uc.failed == 0 && uc.notFastEntry == 0);
		CHECK(uc.worstPx < 0.02 && uc.worstColor < 1e-3 && uc.worstDepth == 0 && app.ui().dropped() == 0);
		CHECK(gfx::frameStats().drawsDropped == 0);
		app.shutdown();
	}
	gfx::shutdown();
	CHECK(mock3ds::violations() == 0 && mock3ds::counters().linearBlocks == 0);
	if (mock3ds::violations()) printViolations("in the overlay");
}

static void flight(const std::string& pak, bool classic, bool worker) {
	mock3ds::reset();
	gfx::c3dSetClassicFrames(classic);
	CHECK(gfx::init());
	App app;
	app.world().config().background = true;
	app.world().config().worker = worker;
	app.world().config().maxLoadsPerFrame = 1;
	CHECK(app.init(pak.c_str(), nullptr));
	app.skipChapters();
	app.setDrawDistance(6000.0f);
	app.world().preload(app.camera().pos);
	const Vec3 home = app.player().eye();

	const float dt = 1.0f / 60.0f;
	unsigned rendered = 0;
	bool clean = true;
	auto frame = [&](const Input& in) {
		const double before = mock3ds::nowMs();
		app.update(in, dt);
		app.render();
		++rendered;
		const gfx::FrameStats fs = gfx::frameStats();
		CHECK(fs.overlapped == !classic && fs.drawsDropped == 0);
		// Exactly one wait for the display per frame, and the frame time is
		// measured from display to display.
		CHECK(std::fabs(mock3ds::nowMs() - before - 1000.0 / 60.0) < 1e-6);
		if (rendered > 1) CHECK(std::fabs(double(fs.frameMs) - 1000.0 / 60.0) < 1e-3);
		if (clean && mock3ds::violations() != 0) {
			clean = false;
			char when[64];
			std::snprintf(when, sizeof(when), "at frame %u", rendered);
			printViolations(when);
		}
	};

	// 0. On foot: the character is drawn after the world, its bone matrices
	// travel in the same command buffer.
	unsigned skinnedDraws = 0, inhabitantDraws = 0, inhabitantFrames = 0;
	mock3ds::setDrawHook(countSkinned, &skinnedDraws);
	for (int f = 0; f < 40; ++f) {
		Input in;
		in.moveZ = 1.0f;
		in.lookX = 0.2f;
		in.jump = f == 20;
		in.toggleCamera = f == 30; // the last ten frames in first person
		frame(in);
		inhabitantDraws += app.npcs().stats().drawCalls + app.loot().stats().drawCalls; // (and things lying about)
		inhabitantFrames += app.npcs().stats().shown > 0;
	}
	mock3ds::setDrawHook(nullptr, nullptr);
	// The hero in thirty of the frames, and whoever of the world's people was in view.
	CHECK(skinnedDraws == 30 * app.hero().model().partCount() + inhabitantDraws && !app.thirdPerson());
	CHECK(inhabitantFrames > 20 && inhabitantDraws > 40);
	app.player().flying = true;
	app.teleport(home);
	// 1. A flight across chunk borders: meshes and textures come and go.
	for (int f = 0; f < 240; ++f) {
		Input in;
		in.moveZ = 1.0f;
		in.fast = true;
		in.lookX = (f / 50) % 2 ? 0.3f : -0.2f;
		if (f % 12 == 5) in.timeStep = 0.5f; // the sky dome is rebuilt inside the frame
		frame(in);
	}
	CHECK(app.world().stats().loadFailures == 0);
	// 2. Everything dropped at once, and back again, several times.
	for (int round = 0; round < 6; ++round) {
		app.teleport({1e6f, 0, 1e6f});
		for (int f = 0; f < 3; ++f) frame(Input {});
		app.teleport(home);
		for (int f = 0; f < 25; ++f) frame(Input {});
	}
	// 3. World switches in the middle of it all (they free a whole world and
	// wait for the GPU outside a frame).
	for (int hop = 0; hop < 5; ++hop) {
		Input in;
		in.nextWorld = true;
		frame(in);
		Input fly;
		fly.moveZ = 1.0f;
		fly.fast = true;
		for (int f = 0; f < 30; ++f) frame(fly);
	}
	CHECK(app.worldSwitches() == 5);
	// 4. Video memory full: allocation fails, the backend frees what the GPU
	// has finished with and tries again.
	app.teleport({1e6f, 0, 1e6f});
	for (int f = 0; f < 3; ++f) frame(Input {});
	mock3ds::setLinearLimit(mock3ds::counters().linearBytes + 48 * 1024);
	app.teleport(app.world().indoor() ? Vec3 {0, 175.0f, 900.0f} : home);
	for (int f = 0; f < 80; ++f) frame(Input {});
	mock3ds::setLinearLimit(0);
	for (int f = 0; f < 200; ++f) frame(Input {});
	// (Frames take no time here; a loader thread gets real time to catch up.)
	for (int wait = 0; worker && wait < 2000 && app.world().stats().chunksLoaded == 0; ++wait) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		frame(Input {});
	}
	CHECK(app.world().stats().chunksLoaded > 0 && app.world().stats().loadFailures == 0);

	const mock3ds::Counters c = mock3ds::counters();
	std::printf("  %-10s %-9s %u frames, %u draw calls (%u issued while the GPU was busy), %u waits, %u buffer(s)\n",
	            classic ? "classic" : "overlapped", worker ? "+ thread" : "", rendered, c.draws, c.drawsWhileBusy, c.waits,
	            c.distinctBuffers);
	CHECK(mock3ds::violations() == 0);
	if (mock3ds::violations() != 0 && clean) printViolations("at the end");
	// Every frame reaches the screen; the few without a command list are the
	// ones with nothing in view (far away from an indoor world: not even sky).
	// (With a worker thread the frames of this test outrun the loader - they
	// take no time here - so more of them are empty.)
	CHECK(c.framesShown == rendered && c.framesSubmitted <= rendered);
	CHECK(c.framesSubmitted > rendered * (worker ? 5 : 9) / 10 && c.draws > rendered * (worker ? 5 : 10));
	if (classic) {
		// citro3d's order: one buffer, and the GPU is always idle while the
		// CPU issues draw calls.
		CHECK(c.distinctBuffers == 1 && c.ownBufferFrames == 0 && c.drawsWhileBusy == 0);
	} else {
		// Overlapped: two buffers taking turns, and nearly every draw call
		// issued while the GPU was still executing the frame before.
		CHECK(c.distinctBuffers == 2 && c.ownBufferFrames == c.framesSubmitted);
		CHECK(c.drawsWhileBusy > c.draws * 9 / 10);
	}

	app.shutdown();
	gfx::shutdown();
	CHECK(mock3ds::violations() == 0);
	CHECK(mock3ds::counters().linearBlocks == 0); // nothing left in the linear heap
}

int main(int argc, char** argv) {
	const std::string dir = argc > 1 ? argv[1] : ".";

	std::vector<BakeWorld> worlds(2);
	worlds[0].name = "world";
	worlds[0].level = "WORLD.ZEN";
	SynthOptions so;
	so.cells = 48;
	so.huts = 8;
	so.trees = 60;
	buildSyntheticScene(so, worlds[0].scene);
	worlds[1].name = "cave";
	worlds[1].level = "CAVE.ZEN";
	buildSyntheticCave(worlds[1].scene);
	BakeOptions bo;
	bo.chunker.chunkSize = 3000.0f;
	bo.tex.maxSize = 32;
	bo.tex.quality = 0;
	bo.threads = 2;
	bo.quiet = true;
	bo.ao.level = AO_LOW; // Graphics+ in the data: the shaders' occlusion and wind are checked
	bo.wind = true;
	bo.waterGrid = 200.0f; // (the water cut along the shores, for the waves)
	bo.riverWidth = 0.0f; // (and everywhere: the test world's water is a narrow stream, which the default leaves calm)
	bo.grass.enabled = true; // (the new grass: off in the menu unless a test turns it on)
	std::vector<SrcModel> models(2);
	buildSyntheticActor(models[0]);
	buildChainModel(models[1], 60);
	// The built-in script's people stand about in it: more characters, other
	// models, textures shared between them - all of it through the backend.
	SrcScript script;
	buildSyntheticScript(script);
	BakeScripts scripts;
	scripts.script = &script;
	scripts.looks = LookSource {buildSyntheticRig, buildSyntheticBody, buildSyntheticHead, buildSyntheticItem, nullptr, nullptr, nullptr, nullptr};
	PakWriter writer;
	bakeWorlds(worlds, models, scripts, bo, writer);
	const std::string pak = dir + "/frames.pak";
	writer.write(pak);

	std::printf("[ RUN ] the checker notices broken rules\n");
	testMockNotices();
	std::printf("[ RUN ] the vertex shader, run on the PC\n");
	testShader(pak);
	std::printf("[ RUN ] the default shader, without the dissolve\n");
	testDefaultShader(pak);
	testSunMask();
	testLampEntry();
	testClassicShader(pak);
	testSupersample(pak);
	testStereo(pak);
	std::printf("[ RUN ] the wide screen\n");
	testWide(pak);
	std::printf("[ RUN ] the fallback shader without characters\n");
	testPlainShader(pak);
	std::printf("[ RUN ] the new grass on the console's path\n");
	testGrass(pak);
	std::printf("[ RUN ] the overlay\n");
	testOverlay(pak);
	std::printf("[ RUN ] frame orders\n");
	flight(pak, true, false);
	flight(pak, false, false);
	flight(pak, false, true);
	flight(pak, true, true);
	std::printf("%d checks, %d failed\n", gChecks, gFailures);
	return gFailures ? 1 : 0;
}
