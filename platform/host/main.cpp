// g3ds-host: runs the exact runtime the 3DS runs, on a PC, with the software
// backend. Headless: flies a scripted camera path, enforces the console's
// memory budget, writes screenshots and prints machine-readable statistics.
#include "app.h"
#include "gfx_soft.h"
#include "sys.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace g3;

static void usage() {
	puts("usage: g3ds-host --pak data.pak [options]\n"
	     "  --world NAME        world asset to load (default: the archive's start world)\n"
	     "  --frames N          frames to simulate at 30 fps (default: 300)\n"
	     "  --shots N           screenshots to save, evenly spaced (default: 4)\n"
	     "  --out PREFIX        screenshot prefix (default: frame)\n"
	     "  --cam X,Y,Z,YAW,PITCH  start pose instead of the baked spawn (cm, degrees)\n"
	     "  --still             keep the camera still\n"
	     "  --walk              walk on the ground with collision instead of flying\n"
	     "  --first-person      look through the character's eyes (default with a baked character: third person)\n"
	     "  --time H            time of day in hours (default: 13)\n"
	     "  --no-cull           draw back faces too (winding check)\n"
	     "  --budget-mb N       world memory budget (default: 24)\n"
	     "  --draw-distance CM  draw distance (default: 12000)\n"
	     "  --detail M,S        draw distance of medium and small objects, cm (default: 7000,3500)\n"
	     "  --stream MODE       how chunks are loaded while moving: sync (default; on the frame),\n"
	     "                      inline (the background pipeline without a thread), thread (as on the console)\n"
	     "  --stats FILE        write a JSON summary");
}

int main(int argc, char** argv) {
	const char* pakPath = nullptr;
	const char* worldName = nullptr;
	const char* detailSpec = nullptr;
	const char* outPrefix = "frame";
	const char* statsPath = nullptr;
	const char* camSpec = nullptr;
	const char* streamMode = "sync";
	int frames = 300, shots = 4;
	bool still = false, cull = true, walk = false, firstPerson = false;
	float timeOfDay = 13.0f;
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
		else if (arg("--stream")) streamMode = argv[++i];
		else if (arg("--detail")) detailSpec = argv[++i];
		else if (arg("--time")) timeOfDay = float(atof(argv[++i]));
		else if (strcmp(argv[i], "--still") == 0) still = true;
		else if (strcmp(argv[i], "--walk") == 0) walk = true;
		else if (strcmp(argv[i], "--first-person") == 0) firstPerson = true;
		else if (strcmp(argv[i], "--no-cull") == 0) cull = false;
		else {
			usage();
			return 2;
		}
	}
	const bool streamSync = strcmp(streamMode, "sync") == 0, streamInline = strcmp(streamMode, "inline") == 0,
	           streamThread = strcmp(streamMode, "thread") == 0;
	if (!pakPath || frames < 1 || !(streamSync || streamInline || streamThread)) {
		usage();
		return 2;
	}

	gfx::init();
	gfx::softSetCulling(cull);

	App app;
	app.world().config().background = !streamSync;
	app.world().config().worker = streamThread;
	if (!app.init(pakPath, worldName)) {
		fprintf(stderr, "error: %s\n", app.error());
		return 1;
	}
	StreamConfig& cfg = app.world().config();
	cfg.memoryBudget = u32(budgetMb * 1024.0f * 1024.0f);
	app.setDrawDistance(drawDistance);
	if (detailSpec && sscanf(detailSpec, "%f,%f", &cfg.detailDistance[g3::DETAIL_MEDIUM], &cfg.detailDistance[g3::DETAIL_SMALL]) != 2) {
		usage();
		return 2;
	}
	app.environment().timeOfDay = timeOfDay;
	app.setThirdPerson(!firstPerson);
	// Three modes: a level flight a few metres up (default), a walk on the
	// ground with collision (--walk), or a fixed view from the spawn (--still).
	float viewPitch = walk ? -0.06f : -0.22f;
	app.camera().pitch = viewPitch;
	app.player().flying = !walk;
	if (!still && !walk && !camSpec) {
		Vec3 eye = app.camera().pos;
		eye.y += 1000.0f;
		app.teleport(eye);
	}

	if (camSpec) {
		float x, y, z, yaw, pitch;
		if (sscanf(camSpec, "%f,%f,%f,%f,%f", &x, &y, &z, &yaw, &pitch) != 5) {
			usage();
			return 2;
		}
		app.teleport({x, y, z});
		app.camera().yaw = yaw * 0.0174533f;
		viewPitch = app.camera().pitch = pitch * 0.0174533f;
	}

	const WorldHeader& wh = app.world().header();
	printf("%s: %u chunks, %u materials, %u textures, %u portals, %u triangles, chunk %.0f cm\n", app.worldName(),
	       wh.chunkCount, wh.materialCount, wh.textureCount, wh.portalCount, wh.triangleCount, wh.chunkSize);

	// What a loading screen would do.
	app.world().preload(app.camera().pos);

	const float dt = 1.0f / 30.0f;
	u32 maxTris = 0, maxDraws = 0, maxBlended = 0, maxLoaded = 0, maxPending = 0, maxLoadsFrame = 0;
	u64 sumCulled = 0, sumDraws = 0;
	int swimFrames = 0, wadeFrames = 0, underwaterFrames = 0;
	u64 sumTris = 0, sumWritten = 0, sumTested = 0, sumBackface = 0, sumFacing = 0;
	int saved = 0, groundedFrames = 0;
	double updateSum = 0, updateWorst = 0;
	u32 maxInFlight = 0, maxTexturesWaiting = 0;
	int heroFrames = 0, heroRoleFrames[Hero::ROLE_COUNT] = {};
	u64 heroPixels = 0;

	for (int f = 0; f < frames; ++f) {
		Input in;
		if (walk) {
			// Walk ahead, weaving a little, and jump now and then.
			in.moveZ = 1.0f;
			in.lookX = (f / 90) % 2 == 0 ? 0.12f : -0.2f;
			in.jump = f % 75 == 40;
		} else if (!still) {
			// A lazy S-curve flight: enough to cross chunk borders and exercise
			// load/unload. The path stays level; only the view is tilted down.
			in.moveZ = 0.5f; // 20 m/s
			in.fast = true;
			in.lookX = (f / 90) % 2 == 0 ? 0.12f : -0.2f;
			app.camera().pitch = 0.0f;
		}
		const double u0 = g3::sys::timeMs();
		app.update(in, dt);
		const double updateMs = g3::sys::timeMs() - u0;
		updateSum += updateMs;
		updateWorst = max(updateWorst, updateMs);
		if (!still && !walk) app.camera().pitch = viewPitch;
		groundedFrames += app.player().grounded;
		swimFrames += app.player().swimming;
		wadeFrames += !app.player().swimming && app.player().waterDepth > 0;
		app.render();
		underwaterFrames += app.underwater();
		if (app.heroVisible()) {
			++heroFrames;
			++heroRoleFrames[app.hero().role()];
		}

		const WorldStats& ws = app.world().stats();
		const gfx::SoftStats ss = gfx::softStats();
		maxTris = max(maxTris, ws.trianglesDrawn);
		maxDraws = max(maxDraws, ws.drawCalls);
		maxBlended = max(maxBlended, ws.blendedDrawCalls);
		sumCulled += ws.batchesCulled;
		sumDraws += ws.drawCalls;
		maxLoaded = max(maxLoaded, ws.chunksLoaded);
		maxPending = max(maxPending, ws.chunksPending);
		maxLoadsFrame = max(maxLoadsFrame, ws.loadsThisFrame);
		maxInFlight = max(maxInFlight, ws.chunksInFlight);
		maxTexturesWaiting = max(maxTexturesWaiting, ws.texturesWaiting);
		sumTris += ws.trianglesDrawn;
		sumWritten += ss.pixelsWritten;
		sumTested += ss.pixelsTested;
		sumBackface += ss.trianglesBackface;
		heroPixels += ss.skinnedPixels;
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
	printf("draw calls/frame  avg %llu, max %u (blended max %u); %llu batches/frame culled by view or distance\n",
	       static_cast<unsigned long long>(sumDraws / u64(frames)), maxDraws, maxBlended,
	       static_cast<unsigned long long>(sumCulled / u64(frames)));
	printf("chunks resident   max %u of %u (pending max %u, loads/frame max %u)\n", maxLoaded, ws.chunksTotal,
	       maxPending, maxLoadsFrame);
	printf("streaming         %s%s: update %.3f ms avg, %.3f ms worst on this PC; in flight max %u, textures waiting max %u\n",
	       streamMode, streamThread && !app.world().workerThread() ? " (no thread: inline)" : "",
	       updateSum / frames, updateWorst, maxInFlight, maxTexturesWaiting);
	printf("world memory      now %.2f MB, peak %.2f MB, budget %.0f MB\n", ws.residentBytes / 1048576.0,
	       ws.peakResidentBytes / 1048576.0, double(budgetMb));
	printf("  meshes %.2f MB (%u), textures %.2f MB (%u)\n", ms.meshBytes / 1048576.0, ms.meshCount,
	       ms.textureBytes / 1048576.0, ms.textureCount);
	printf("overdraw          %.2f written, %.2f depth-tested per pixel\n", double(sumWritten) / screen,
	       double(sumTested) / screen);
	// Informational only: from inside a valley most triangles in view belong to
	// slopes facing away. The baker's area-based winding check is the real test.
	printf("back faces        %.0f%% of the triangles in view\n", backfaceShare * 100.0);
	if (walk) {
		const Vec3 p = app.player().feet;
		printf("walk              %d frames: %d on the ground (%d of them wading), %d swimming, %d in the air\n", frames,
		       groundedFrames, wadeFrames, swimFrames, frames - groundedFrames - swimFrames);
		printf("                  ended at %.0f %.0f %.0f in %s, collision data %.0f KB\n", double(p.x), double(p.y),
		       double(p.z), app.worldName(), double(ws.collisionBytes) / 1024.0);
	}
	if (app.hero().loaded()) {
		const Hero& hero = app.hero();
		printf("hero              %u triangles, %u clips; drawn in %d frames (%.0f pixels each): standing %d, running %d, walking %d, "
		       "in the air %d, swimming %d\n",
		       hero.model().triangles(), hero.model().clipCount(), heroFrames, heroFrames ? double(heroPixels) / heroFrames : 0.0,
		       heroRoleFrames[Hero::IDLE], heroRoleFrames[Hero::RUN], heroRoleFrames[Hero::WALK],
		       heroRoleFrames[Hero::JUMP] + heroRoleFrames[Hero::FALL], heroRoleFrames[Hero::SWIM] + heroRoleFrames[Hero::SWIM_IDLE]);
	} else {
		printf("hero              none in the archive: first person\n");
	}
	if (underwaterFrames) printf("camera under water %d frames\n", underwaterFrames);
	if (app.worldSwitches()) printf("world switches    %u\n", app.worldSwitches());
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
		        "\"loadFailures\":%u,\"overdraw\":%.3f,\"backfaceShare\":%.3f,\"groundedFrames\":%d,"
		        "\"swimFrames\":%d,\"blendedDrawsMax\":%u,\"batchesCulledAvg\":%llu,\"worldSwitches\":%u,"
		        "\"pixelsWritten\":%llu,\"heroFrames\":%d,\"heroRunFrames\":%d,\"heroAirFrames\":%d,"
		        "\"heroSwimFrames\":%d,\"heroPixels\":%llu}\n",
		        frames, static_cast<unsigned long long>(sumTris / u64(frames)), maxTris, maxDraws, ws.chunksTotal,
		        maxLoaded, ws.peakResidentBytes, ws.budgetRejects, ws.loadFailures, double(sumWritten) / screen,
		        backfaceShare, groundedFrames, swimFrames, maxBlended,
		        static_cast<unsigned long long>(sumCulled / u64(frames)), app.worldSwitches(),
		        static_cast<unsigned long long>(sumWritten), heroFrames, heroRoleFrames[Hero::RUN],
		        heroRoleFrames[Hero::JUMP] + heroRoleFrames[Hero::FALL],
		        heroRoleFrames[Hero::SWIM] + heroRoleFrames[Hero::SWIM_IDLE],
		        static_cast<unsigned long long>(heroFrames ? heroPixels / u64(heroFrames) : 0));
		fclose(fp);
	}

	app.shutdown();
	gfx::shutdown();
	return (ws.loadFailures == 0) ? 0 : 3;
}
