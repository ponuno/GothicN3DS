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
	unsigned fadedVertices = 0;
	double fadeChanged = 0, worstNoise = 0;
	double worstWorldPx = 0, worstSkinPx = 0, worstDepth = 0, worstColor = 0, worstTex = 0;
	double worstGpuWorldPx = 0, worstGpuSkinPx = 0;
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
	const int locChunk = 8, locParams = 9, locDayDelta = 10, locTexOffset = 11, locBones = 13; // checked in testShader
	const bool skinned = call.bools & 1;
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
			faded.bools = u16(call.bools | 2u);
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
			const double p[3] = {double(v.x) * double(chunk[3]), double(v.y) * double(chunk[3]), double(v.z) * double(chunk[3])};
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
				world[k] = double(sc.anchor[k]) + double(chunk[k]) + a + (b - a) * w;
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
			if (mock3ds::shadeVertex(plain, index, asWorld, false))
				sc.movedBySwitch += std::fabs(asWorld.position[0] - got.position[0]) + std::fabs(asWorld.position[1] - got.position[1]) > 1e-3f;
		} else {
			const double q[3] = {double(v.x), double(v.y), double(v.z)};
			const u8 rgb[3] = {v.r, v.g, v.b};
			for (int k = 0; k < 3; ++k) {
				world[k] = double(sc.anchor[k]) + double(chunk[k]) + double(chunk[3]) * q[k];
				wantColor[k] = double(rgb[k]) / 255.0 * (1.0 + double(v.a) / 255.0 * double(call.uniforms[locDayDelta][k]));
			}
			++sc.worldVertices;
		}
		// View space in double precision, straight from the camera.
		double eye[4];
		for (int r = 0; r < 4; ++r)
			eye[r] = double(sc.view.at(r, 0)) * world[0] + double(sc.view.at(r, 1)) * world[1] + double(sc.view.at(r, 2)) * world[2] +
			         double(sc.view.at(r, 3));
		const double wantFog = std::clamp(double(params[2]) - eye[2] * double(params[3]), 0.0, 1.0);
		sc.worstColor = std::max(sc.worstColor, std::fabs(double(got.color[3]) - wantFog));
		for (int k = 0; k < 3; ++k) sc.worstColor = std::max(sc.worstColor, std::fabs(double(got.color[k]) - wantColor[k]));
		const double wantU = double(v.u) * double(params[0]) + double(call.uniforms[locTexOffset][0]);
		const double wantV = double(v.v) * double(params[0]) + double(call.uniforms[locTexOffset][1]);
		sc.worstTex = std::max({sc.worstTex, std::fabs(double(got.texcoord[0]) - wantU), std::fabs(double(got.texcoord[1]) - wantV)});

		// One point of the world, one point on the GPU.
		if (!skinned && !sky) {
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
		(skinned ? sc.worstGpuSkinPx : sc.worstGpuWorldPx) = std::max(skinned ? sc.worstGpuSkinPx : sc.worstGpuWorldPx, gpuPx);
		// Depth: -1 at the near plane, 0 at the far one; w is the distance.
		const double wantDepth = double(sc.zNear) * (double(sc.zFar) - eye[2]) / ((double(sc.zNear) - double(sc.zFar)) * eye[2]);
		sc.worstDepth = std::max({sc.worstDepth, std::fabs(double(got.position[2]) / double(got.position[3]) - wantDepth),
		                          std::fabs(double(got.position[3]) - eye[2]) / eye[2]});
	}
}

static void testShader(const std::string& pak) {
	mock3ds::reset();
	gfx::c3dSetClassicFrames(false);
	gfx::c3dSetFadeShader(true); // the shader with the dissolve: world.v.pica and then some
	CHECK(gfx::init() && gfx::fadeSupported());
	// The binary is the one picasso assembled from world_fade.v.pica, and the
	// backend found every uniform in it.
	std::printf("  shader: %u instructions, %u of 96 float registers%s%s\n", mock3ds::shaderInstructions(),
	            mock3ds::shaderFloatRegisters(), *mock3ds::shaderProblem() ? "; PROBLEM: " : "", mock3ds::shaderProblem());
	CHECK(!*mock3ds::shaderProblem());
	// (projection, modelView, five vectors, the palette; two constants)
	CHECK(mock3ds::shaderFloatRegisters() == 4 + 4 + 5 + MODEL_PALETTE * 3 + 2 && mock3ds::shaderFloatRegisters() <= 96);
	shaderProgram_s probe;
	shaderProgramInit(&probe);
	shaderProgramSetVsh(&probe, nullptr);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "chunk") == 8);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "params") == 9);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "dayDelta") == 10);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "texOffset") == 11);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "bones") == 13);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "skinned") == 0x68);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "fading") == 0x69);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "nonsense") < 0);

	ShaderCheck sc;
	sc.checkFade = true;
	// 1. The game as it runs: the world around a walking, turning character,
	// in the evening (the time of day tints the light).
	{
		App app;
		CHECK(app.init(pak.c_str(), nullptr));
		app.setDrawDistance(6000.0f);
		app.environment().timeOfDay = 19.5f;
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
				sc.others = app.npcs().stats().drawCalls;
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
		CHECK(sc.worstGpuWorldPx < 0.25 && sc.worstGpuSkinPx < 0.25);
		CHECK(sc.worstDepth < 1e-3 && sc.worstColor < 1e-4 && sc.worstTex < 1e-3);
		CHECK(sc.sharedPoints > 20 && sc.splitPoints == 0);
		std::printf("  dissolve: %u world vertices shaded with it on: the same otherwise (within %.1e), the noise at the screen position "
		            "(within %.1e)\n", sc.fadedVertices, sc.fadeChanged, sc.worstNoise);
		CHECK(sc.fadedVertices > 5000 && sc.fadeChanged == 0.0 && sc.worstNoise < 1e-4);
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
		CHECK(cc.highestBoneRegister == 13 + MODEL_PALETTE * 3 - 1); // a full palette was used
		chain.release();
		p.close();
	}
	gfx::shutdown();
	gfx::c3dSetFadeShader(false);
	CHECK(mock3ds::violations() == 0 && mock3ds::counters().linearBlocks == 0);
}

// The default: the shader of the version before the dissolve (world.v.pica),
// nothing on texture unit 1 - the GPU state that is known to work on the
// console. (The first build with the dissolve always on gave a black screen.)
static void testDefaultShader(const std::string& pak) {
	mock3ds::reset();
	gfx::c3dSetClassicFrames(false);
	CHECK(gfx::init() && !gfx::fadeSupported());
	std::printf("  default shader: %u instructions, %u of 96 float registers%s%s\n", mock3ds::shaderInstructions(),
	            mock3ds::shaderFloatRegisters(), *mock3ds::shaderProblem() ? "; PROBLEM: " : "", mock3ds::shaderProblem());
	CHECK(!*mock3ds::shaderProblem() && mock3ds::shaderFloatRegisters() == 4 + 4 + 5 + MODEL_PALETTE * 3 + 1);
	shaderProgram_s probe;
	shaderProgramInit(&probe);
	shaderProgramSetVsh(&probe, nullptr);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "skinned") == 0x68);
	CHECK(shaderInstanceGetUniformLocation(probe.vertexShader, "fading") < 0);
	{
		ShaderCheck sc;
		App app;
		CHECK(app.init(pak.c_str(), nullptr));
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
				sc.others = app.npcs().stats().drawCalls;
				mock3ds::setDrawHook(shaderHook, &sc);
			}
			app.render();
			mock3ds::setDrawHook(nullptr, nullptr);
			CHECK(app.world().stats().batchesFading == 0);
		}
		CHECK(sc.worldDraws > 10 && sc.worldVertices > 1000 && sc.failed == 0 && sc.unexpected == 0);
		CHECK(sc.worstWorldPx < 0.02 && sc.worstGpuWorldPx < 0.25 && sc.worstColor < 1e-4 && sc.worstTex < 1e-3);
		app.shutdown();
	}
	CHECK(mock3ds::counters().texUnit1Binds == 0);
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
		CHECK(sc.worstWorldPx < 0.02 && sc.worstGpuWorldPx < 0.25 && sc.worstColor < 1e-4 && sc.worstTex < 1e-3 && sc.splitPoints == 0);
		app.shutdown();
	}
	gfx::shutdown();
	gfx::c3dSetPlainShader(false);
	CHECK(mock3ds::violations() == 0 && mock3ds::counters().linearBlocks == 0);
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
		inhabitantDraws += app.npcs().stats().drawCalls;
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
	std::vector<SrcModel> models(2);
	buildSyntheticActor(models[0]);
	buildChainModel(models[1], 60);
	// The built-in script's people stand about in it: more characters, other
	// models, textures shared between them - all of it through the backend.
	SrcScript script;
	buildSyntheticScript(script);
	BakeScripts scripts;
	scripts.script = &script;
	scripts.looks = LookSource {buildSyntheticRig, buildSyntheticBody, buildSyntheticHead};
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
	std::printf("[ RUN ] the fallback shader without characters\n");
	testPlainShader(pak);
	std::printf("[ RUN ] frame orders\n");
	flight(pak, true, false);
	flight(pak, false, false);
	flight(pak, false, true);
	flight(pak, true, true);
	std::printf("%d checks, %d failed\n", gChecks, gFailures);
	return gFailures ? 1 : 0;
}
