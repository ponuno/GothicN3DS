// g3ds-host: runs the exact runtime the 3DS runs, on a PC, with the software
// backend. Headless: flies a scripted camera path, enforces the console's
// memory budget, writes screenshots and prints machine-readable statistics.
#include "app.h"
#include "gfx_soft.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace g3;

static void usage() {
	puts("usage: g3ds-host --pak data.pak [options]\n"
	     "  --world NAME        world asset to load (default: world)\n"
	     "  --frames N          frames to simulate at 30 fps (default: 300)\n"
	     "  --shots N           screenshots to save, evenly spaced (default: 4)\n"
	     "  --out PREFIX        screenshot prefix (default: frame)\n"
	     "  --cam X,Y,Z,YAW,PITCH  start pose instead of the baked spawn (cm, degrees)\n"
	     "  --still             keep the camera still\n"
	     "  --no-cull           draw back faces too (winding check)\n"
	     "  --budget-mb N       world memory budget (default: 24)\n"
	     "  --draw-distance CM  draw distance (default: 12000)\n"
	     "  --stats FILE        write a JSON summary");
}

int main(int argc, char** argv) {
	const char* pakPath = nullptr;
	const char* worldName = "world";
	const char* outPrefix = "frame";
	const char* statsPath = nullptr;
	const char* camSpec = nullptr;
	int frames = 300, shots = 4;
	bool still = false, cull = true;
	float budgetMb = 24.0f, drawDistance = 12000.0f;

	for (int i = 1; i < argc; ++i) {
		auto arg = [&](const char* name) { return strcmp(argv[i], name) == 0 && i + 1 < argc; };
		if (arg("--pak")) pakPath = argv[++i];
		else if (arg("--world")) worldName = argv[++i];
		else if (arg("--frames")) frames = atoi(argv[++i]);
		else if (arg("--shots")) shots = atoi(argv[++i]);
		else if (arg("--out")) outPrefix = argv[++i];
		else if (arg("--cam")) camSpec = argv[++i];
		else if (arg("--budget-mb")) budgetMb = float(atof(argv[++i]));
		else if (arg("--draw-distance")) drawDistance = float(atof(argv[++i]));
		else if (arg("--stats")) statsPath = argv[++i];
		else if (strcmp(argv[i], "--still") == 0) still = true;
		else if (strcmp(argv[i], "--no-cull") == 0) cull = false;
		else {
			usage();
			return 2;
		}
	}
	if (!pakPath || frames < 1) {
		usage();
		return 2;
	}

	gfx::init();
	gfx::softSetCulling(cull);

	App app;
	if (!app.init(pakPath, worldName)) {
		fprintf(stderr, "error: %s\n", app.error());
		return 1;
	}
	StreamConfig& cfg = app.world().config();
	cfg.memoryBudget = u32(budgetMb * 1024.0f * 1024.0f);
	app.setDrawDistance(drawDistance);
	float viewPitch = -0.22f;
	app.camera().pitch = viewPitch;
	if (!still && !camSpec) app.camera().pos.y += 1000.0f;

	if (camSpec) {
		float x, y, z, yaw, pitch;
		if (sscanf(camSpec, "%f,%f,%f,%f,%f", &x, &y, &z, &yaw, &pitch) != 5) {
			usage();
			return 2;
		}
		app.camera().pos = {x, y, z};
		app.camera().yaw = yaw * 0.0174533f;
		viewPitch = app.camera().pitch = pitch * 0.0174533f;
	}

	const WorldHeader& wh = app.world().header();
	printf("world '%s': %u chunks, %u materials, %u triangles, chunk %.0f cm\n", worldName, wh.chunkCount,
	       wh.materialCount, wh.triangleCount, wh.chunkSize);

	// What a loading screen would do.
	app.world().preload(app.camera().pos);

	const float dt = 1.0f / 30.0f;
	u32 maxTris = 0, maxDraws = 0, maxLoaded = 0, maxPending = 0, maxLoadsFrame = 0;
	u64 sumTris = 0, sumWritten = 0, sumTested = 0, sumBackface = 0, sumFacing = 0;
	int saved = 0;

	for (int f = 0; f < frames; ++f) {
		Input in;
		if (!still) {
			// A lazy S-curve flight a few metres up: enough to cross chunk
			// borders and exercise load/unload. There is no collision yet, so
			// the path stays level and only the view is tilted down.
			in.moveZ = 0.5f; // 20 m/s
			in.fast = true;
			in.lookX = (f / 90) % 2 == 0 ? 0.12f : -0.2f;
			app.camera().pitch = 0.0f;
		}
		app.update(in, dt);
		if (!still) app.camera().pitch = viewPitch;
		app.render();

		const WorldStats& ws = app.world().stats();
		const gfx::SoftStats ss = gfx::softStats();
		maxTris = max(maxTris, ws.trianglesDrawn);
		maxDraws = max(maxDraws, ws.drawCalls);
		maxLoaded = max(maxLoaded, ws.chunksLoaded);
		maxPending = max(maxPending, ws.chunksPending);
		maxLoadsFrame = max(maxLoadsFrame, ws.loadsThisFrame);
		sumTris += ws.trianglesDrawn;
		sumWritten += ss.pixelsWritten;
		sumTested += ss.pixelsTested;
		sumBackface += ss.trianglesBackface;
		sumFacing += ss.trianglesSubmitted - ss.trianglesClipped; // triangles that reached the facing test

		const bool shot = shots > 0 && (f == frames - 1 || (shots > 1 && f == (frames - 1) * saved / (shots - 1)));
		if (shot && saved < shots) {
			char name[512];
			snprintf(name, sizeof(name), "%s_%02d.png", outPrefix, saved);
			if (!stbi_write_png(name, gfx::SCREEN_W, gfx::SCREEN_H, 4, gfx::softFramebuffer(), gfx::SCREEN_W * 4)) {
				fprintf(stderr, "error: cannot write %s\n", name);
				return 1;
			}
			printf("frame %4d -> %s  (%u chunks, %u draws, %u tris)\n", f, name, ws.chunksDrawn, ws.drawCalls,
			       ws.trianglesDrawn);
			++saved;
		}
	}

	const WorldStats ws = app.world().stats();
	const gfx::MemStats ms = gfx::memStats();
	const double screen = double(gfx::SCREEN_W) * gfx::SCREEN_H * frames;
	const double backfaceShare = sumFacing ? double(sumBackface) / double(sumFacing) : 0.0;

	printf("\n--- %d frames ---\n", frames);
	printf("triangles/frame   avg %llu, max %u\n", static_cast<unsigned long long>(sumTris / u64(frames)), maxTris);
	printf("draw calls/frame  max %u\n", maxDraws);
	printf("chunks resident   max %u of %u (pending max %u, loads/frame max %u)\n", maxLoaded, ws.chunksTotal,
	       maxPending, maxLoadsFrame);
	printf("world memory      now %.2f MB, peak %.2f MB, budget %.0f MB\n", ws.residentBytes / 1048576.0,
	       ws.peakResidentBytes / 1048576.0, double(budgetMb));
	printf("  meshes %.2f MB (%u), textures %.2f MB (%u)\n", ms.meshBytes / 1048576.0, ms.meshCount,
	       ms.textureBytes / 1048576.0, ms.textureCount);
	printf("overdraw          %.2f written, %.2f depth-tested per pixel\n", double(sumWritten) / screen,
	       double(sumTested) / screen);
	// Informational only: from inside a valley most triangles in view belong to
	// slopes facing away. The baker's area-based winding check is the real test.
	printf("back faces        %.0f%% of the triangles in view\n", backfaceShare * 100.0);
	printf("budget rejects %u, load failures %u, pak I/O %.2f MB\n", ws.budgetRejects, ws.loadFailures,
	       double(app.pak().bytesRead()) / 1048576.0);

	if (statsPath) {
		FILE* fp = fopen(statsPath, "w");
		if (!fp) {
			fprintf(stderr, "error: cannot write %s\n", statsPath);
			return 1;
		}
		fprintf(fp,
		        "{\"frames\":%d,\"trianglesAvg\":%llu,\"trianglesMax\":%u,\"drawCallsMax\":%u,"
		        "\"chunksTotal\":%u,\"chunksLoadedMax\":%u,\"peakBytes\":%u,\"budgetRejects\":%u,"
		        "\"loadFailures\":%u,\"overdraw\":%.3f,\"backfaceShare\":%.3f,\"pixelsWritten\":%llu}\n",
		        frames, static_cast<unsigned long long>(sumTris / u64(frames)), maxTris, maxDraws, ws.chunksTotal,
		        maxLoaded, ws.peakResidentBytes, ws.budgetRejects, ws.loadFailures, double(sumWritten) / screen,
		        backfaceShare, static_cast<unsigned long long>(sumWritten));
		fclose(fp);
	}

	app.shutdown();
	gfx::shutdown();
	return (ws.loadFailures == 0) ? 0 : 3;
}
