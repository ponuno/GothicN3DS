// pica_replay: runs a real data.pak through the console's graphics code on the PC (fix139).
//
//   pica_replay <data.pak> [frames] [x,y,z] [grass N]
//   (fix147: `grass 1` / `grass 2` turns the new grass on, few / many, as the menu would; the default is a first start's: off)
//
// platform/3ds/gfx_c3d.cpp is compiled against the stand-in for libctru and citro3d (tests/mock3ds), as in test_frames:
// the stand-in keeps the GPU's books (which memory a submitted frame refers to) and runs the assembled vertex shader.
// Here every vertex of every draw call of the first frames is shaded, in the GPU's 24-bit floats, and whatever the
// console's GPU could choke on is reported with the step of the frame it came from (engine/trace.h): a position or
// colour that is not a number or infinite, a w at or below 0 together with huge x/y (the clipper's worst case), an
// index past the vertex buffer, a draw with no vertex buffer, a broken rule of the GPU's memory.
#include "app.h"
#include "gfx_c3d.h"
#include "mock3ds.h"
#include "settings.h"
#include "trace.h"

#include <citro3d.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>

using namespace g3;

namespace {

struct Stats {
	unsigned draws = 0, vertices = 0, failed = 0;
	unsigned nonFinite = 0, hugeClip = 0, badIndex = 0, noBuffer = 0, zeroCount = 0;
	float worstAbs = 0.0f;
	float maxUv = 0, maxUv1 = 0, maxUv2 = 0, maxCol = 0, minCol = 0, minW = 1e30f;
	unsigned maxCount = 0, maxVerts = 0, maxIndexValue = 0;
	std::map<unsigned, unsigned> countHist;
	std::map<std::string, unsigned> drawsByStage;
	std::map<std::string, unsigned> problemsByStage;
	std::map<int, unsigned> drawsByEntry;
	std::set<std::string> shown;
	int frame = 0;
	unsigned printed = 0;
};

void note(Stats& s, const char* what, const mock3ds::DrawCall& c, unsigned index, const mock3ds::ShadedVertex* v) {
	const std::string stage = gTrace.stage ? gTrace.stage : "?";
	++s.problemsByStage[stage + ": " + what];
	if (s.printed >= 40) return;
	++s.printed;
	std::printf("frame %d, %s, entry %d, bools 0x%04x, %u indices: %s", s.frame, stage.c_str(), c.entry, unsigned(c.bools), c.count, what);
	if (v)
		std::printf(" (vertex %u: pos %g %g %g %g, colour %g %g %g %g)", index, double(v->position[0]), double(v->position[1]),
		            double(v->position[2]), double(v->position[3]), double(v->color[0]), double(v->color[1]), double(v->color[2]),
		            double(v->color[3]));
	std::printf("\n");
}

void hook(const mock3ds::DrawCall& c, void* user) {
	Stats& s = *static_cast<Stats*>(user);
	++s.draws;
	++s.drawsByStage[gTrace.stage ? gTrace.stage : "?"];
	++s.drawsByEntry[c.entry];
	if (!c.vertices || !c.indices) {
		++s.noBuffer;
		note(s, "no vertex or index buffer", c, 0, nullptr);
		return;
	}
	if (c.count == 0) {
		++s.zeroCount;
		return;
	}
	const size_t vbytes = mock3ds::blockBytes(c.vertices);
	const unsigned vcount = c.stride ? unsigned(vbytes / c.stride) : 0;
	s.maxCount = std::max(s.maxCount, c.count);
	s.maxVerts = std::max(s.maxVerts, vcount);
	{
		unsigned b = 1;
		while (b < c.count) b <<= 1;
		++s.countHist[b];
	}
	for (unsigned i = 0; i < c.count; ++i) s.maxIndexValue = std::max(s.maxIndexValue, unsigned(c.indices[i]));
	std::set<unsigned> done;
	for (unsigned i = 0; i < c.count; ++i) {
		const unsigned idx = c.indices[i];
		if (vbytes && idx >= vcount) {
			++s.badIndex;
			note(s, "index past the vertex buffer", c, idx, nullptr);
			return;
		}
		if (!done.insert(idx).second) continue;
		mock3ds::ShadedVertex v;
		if (!mock3ds::shadeVertex(c, idx, v, true)) {
			++s.failed;
			note(s, mock3ds::shaderProblem(), c, idx, nullptr);
			return;
		}
		++s.vertices;
		bool finite = true;
		for (float f : v.position) finite = finite && std::isfinite(f);
		for (float f : v.color) finite = finite && std::isfinite(f);
		for (float f : v.texcoord) finite = finite && std::isfinite(f);
		for (float f : v.texcoord1) finite = finite && std::isfinite(f);
		for (float f : v.texcoord2) finite = finite && std::isfinite(f);
		if (!finite) {
			++s.nonFinite;
			note(s, "a value that is not a number or infinite", c, idx, &v);
			continue;
		}
		for (int k = 0; k < 2; ++k) {
			s.maxUv = std::max(s.maxUv, std::fabs(v.texcoord[k]));
			s.maxUv1 = std::max(s.maxUv1, std::fabs(v.texcoord1[k]));
			s.maxUv2 = std::max(s.maxUv2, std::fabs(v.texcoord2[k]));
		}
		for (int k = 0; k < 4; ++k) s.maxCol = std::max(s.maxCol, v.color[k]), s.minCol = std::min(s.minCol, v.color[k]);
		s.minW = std::min(s.minW, v.position[3]);
		const float a = std::max(std::fabs(v.position[0]), std::fabs(v.position[1]));
		s.worstAbs = std::max(s.worstAbs, a);
		if (a > 1.0e7f || std::fabs(v.position[3]) > 1.0e7f || std::fabs(v.position[2]) > 1.0e7f) {
			++s.hugeClip;
			note(s, "a huge clip-space coordinate", c, idx, &v);
		}
	}
}

} // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		std::printf("usage: pica_replay <data.pak> [frames] [x,y,z] [grass N]\n");
		return 2;
	}
	const int frames = argc > 2 ? std::atoi(argv[2]) : 60;
	if (getenv("LINEAR_MB")) mock3ds::setLinearLimit(size_t(atoi(getenv("LINEAR_MB"))) << 20);
	App app;
	// As platform/3ds/main.cpp on a first start: the dissolve on (its own shader entries), no smoothing, no stereo.
	gfx::c3dSetFadeShader(Settings().dissolve != 0);
	if (!gfx::init()) {
		std::printf("gfx init failed\n");
		return 1;
	}
	{
		// The stream as platform/3ds/main.cpp sets it up.
		StreamConfig& stream = app.world().config();
		stream.background = true;
		stream.maxLoadsPerFrame = 1;
		stream.budget = &app.frameBudget();
		app.npcs().config().budget = &app.frameBudget();
		app.setReadAheadBudget(4u << 20);
		stream.headLead = 1200.0f;
	}
	if (!app.init(argv[1], nullptr)) {
		std::printf("cannot start: %s\n", app.error());
		return 1;
	}
	app.world().config().memoryBudget = 44u << 20;
	app.world().config().memoryReserve = 5u << 20;
	app.npcs().config().startsPerFrame = 1;
	app.npcs().config().reachFloor = 0.6f;
	app.npcs().config().farTickEvery = 2;
	app.npcs().config().poseHidden = false;
	app.npcs().config().prefetchRange = 3000.0f;
	// The settings of a first start (no settings.bin), as on the console.
	Settings st;
	for (int i = 3; i + 1 < argc; ++i)
		if (std::strcmp(argv[i], "grass") == 0) st.grass = u8(std::atoi(argv[i + 1]));
		else if (std::strcmp(argv[i], "lamps") == 0) st.lamps = u8(std::atoi(argv[i + 1]));
	st.sanitize();
	app.settings() = st;
	app.applySettings();
	if (st.grass) std::printf("grass: %d (%s)\n", int(st.grass), app.grass().available() ? "in data.pak" : "NOT in data.pak: bake with --grass");
	if (argc > 3 && std::strcmp(argv[3], "grass") != 0 && std::strcmp(argv[3], "lamps") != 0) {
		float x, y, z;
		if (std::sscanf(argv[3], "%f,%f,%f", &x, &y, &z) == 3) app.teleport({x, y, z});
	}
	app.world().preload(app.camera().pos);
	std::printf("camera %.0f %.0f %.0f, shader: %s\n", double(app.camera().pos.x), double(app.camera().pos.y),
	            double(app.camera().pos.z), gfx::c3dFastShader() ? "fast (four entries)" : "fallback");
	Stats s;
	const float dtArg = getenv("DT") ? float(atof(getenv("DT"))) : 1.0f / 30.0f;
	for (int f = 0; f < frames; ++f) {
		s.frame = f;
		Input in;
		const float dt = f == 0 && getenv("DT0") ? float(atof(getenv("DT0"))) : dtArg;
		app.update(in, dt);
		{
			const Player& pl = app.player();
			const Camera& cam = app.camera();
			std::printf("frame %d: feet %.1f %.1f %.1f climbing %d grounded %d, camera %.1f %.1f %.1f yaw %.3f pitch %.3f\n", f, double(pl.feet.x), double(pl.feet.y),
			            double(pl.feet.z), int(pl.climbing), int(pl.grounded), double(cam.pos.x), double(cam.pos.y), double(cam.pos.z), double(cam.yaw), double(cam.pitch));
		}
		mock3ds::setDrawHook(hook, &s);
		app.render();
		mock3ds::setDrawHook(nullptr, nullptr);
		{
			const gfx::FrameStats fs = gfx::frameStats();
			std::printf("frame %d: command buffer %.1f %%, draws dropped %u, program switches %u\n", f, double(fs.commandBufferUse) * 100.0,
			            unsigned(fs.drawsDropped), unsigned(fs.programSwitches));
		}
		if (mock3ds::violations()) {
			std::printf("frame %d: the GPU's rules broken (%u so far)\n", f, mock3ds::violations());
		}
	}
	const mock3ds::Counters& c = mock3ds::counters();
	std::printf("\n%d frames, %u draw calls, %u vertices shaded (24-bit floats), largest |x|,|y| in clip space %g\n", frames, s.draws, s.vertices,
	            double(s.worstAbs));
	std::printf("problems: not a number/infinite %u, huge %u, index past the buffer %u, no buffer %u, shader refused %u, empty draws %u\n",
	            s.nonFinite, s.hugeClip, s.badIndex, s.noBuffer, s.failed, s.zeroCount);
	std::printf("GPU rules: busy buffer written %u, written outside %u, freed in use %u, submitted freed %u, submitted busy %u, "
	            "draw outside a frame %u\n",
	            c.wroteBusyBuffer, c.wroteOutside, c.freedInUse, c.submittedFreed, c.submittedBusy, c.drawOutsideFrame);
	std::printf("largest |uv| %g, |uv1| %g, |uv2| %g; colour %g..%g; least w %g\n", double(s.maxUv), double(s.maxUv1), double(s.maxUv2), double(s.minCol), double(s.maxCol), double(s.minW));
	std::printf("largest draw %u indices, largest vertex buffer %u vertices, largest index %u\nindex counts (up to):", s.maxCount, s.maxVerts, s.maxIndexValue);
	for (const auto& kv : s.countHist) std::printf(" %u:%u", kv.first, kv.second);
	std::printf("\n");
	std::printf("draw calls by entry:");
	for (const auto& kv : s.drawsByEntry) std::printf(" %d: %u", kv.first, kv.second);
	std::printf("\ndraw calls by step:\n");
	for (const auto& kv : s.drawsByStage) std::printf("  %-28s %u\n", kv.first.c_str(), kv.second);
	if (!s.problemsByStage.empty()) {
		std::printf("problems by step:\n");
		for (const auto& kv : s.problemsByStage) std::printf("  %-60s %u\n", kv.first.c_str(), kv.second);
	}
	app.shutdown();
	gfx::shutdown();
	return 0;
}
