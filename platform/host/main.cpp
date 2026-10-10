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
	     "  --to-time H         let the clock run from --time to this hour over the frames (routines move on)\n"
	     "  --standing          the inhabitants only stand where their routine has them (no states, no walking)\n"
	     "  --who               list at the end what each inhabitant near the camera is doing\n"
	     "  --slope-probe X,Y,Z the character at that place (the console's \"pos\"): standing, then run and jump in\n"
	     "                      eight directions, printing what the controller does frame by frame\n"
	     "  --probe-fps N       the probe's frame rate (default: 30; the console runs at 60)\n"
	     "  --probe-grid M      probe every point within M metres of X,Z, a metre apart (default: 0)\n"
	     "  --whole-chunks      draw all of every chunk in view, not only its clusters in view (for comparing)\n"
	     "  --console           the console's controls (the stick goes where the camera looks; default: classic)\n"
	     "  --menu PAGE         open the pause menu at a page (0 pause, 1 graphics, 2 controls, 3 faults) for the shots\n"
	     "  --fight             the character is put 8 m from the nearest enemy and fights it (with the best weapon\n"
	     "                      the scripts have if --sword): what happened is printed at the end\n"
	     "  --sword             with --fight: a sword in hand rather than fists\n"
	     "  --inventory         the faults menu's set of things; the bottom screen is touched as a player would:\n"
	     "                      a potion chosen and drunk, a ring put on, food held into a quick slot and used;\n"
	     "                      the bottom screen is saved next to each shot (PREFIX_bottom_NN.png)\n"
	     "  --pickup            the character walks to the things lying about and picks them up (A)\n"
	     "  --stats FILE        write a JSON summary");
}

// --slope-probe X,Y,Z: the player's character at a place (the eye position
// the console's "pos" line gives) - standing, then running and jumping in
// eight directions - with what the controller does frame by frame, in the
// columns of the console's movement recorder (L+R+SELECT writes slope.txt).
// For finding out where somebody gets stuck on a slope, from the data alone.
// With a grid (--probe-grid M) every point within M metres, a metre apart,
// is tried, and only the runs that went wrong are printed in full.
static int slopeProbe(App& app, const char* spec, int fps, int gridMetres) {
	float x, y, z;
	if (sscanf(spec, "%f,%f,%f", &x, &y, &z) != 3 || fps < 10 || fps > 240 || gridMetres < 0 || gridMetres > 20) {
		usage();
		return 2;
	}
	const float dt = 1.0f / float(fps);
	const int perSecond = fps;
	Player& pl = app.player();
	pl.flying = false;
	printf("slope probe at %.0f %.0f %.0f, %d fps%s; hero %s\n", double(x), double(y), double(z), fps,
	       gridMetres ? "" : ", every frame printed at 10 a second", app.hero().loaded() ? "drives the body" : "none (direct control)");
	if (gridMetres) printf("grid: every metre within %d m; only the runs that hung, popped or were unstuck are printed in full\n", gridMetres);
	printf("%s\n", MoveRecorder::header());
	int stuckRuns = 0, caughtRuns = 0, runs = 0;
	const u32 home = app.world().entryHash();
	char line[256];
	for (int gz = -gridMetres; gz <= gridMetres; ++gz)
		for (int gx = -gridMetres; gx <= gridMetres; ++gx) {
			const Vec3 at = {x + float(gx) * 100.0f, y, z + float(gz) * 100.0f};
			if (gridMetres) printf("-- at %.0f %.0f %.0f\n", double(at.x), double(at.y), double(at.z));
			for (int run = -1; run < 8; ++run) {
				// Run -1: just standing there. Runs 0..7: a run-up and a jump, every 45 degrees.
				const float yaw = run < 0 ? 0.0f : float(run) * 0.785398f;
				app.camera().yaw = yaw;
				// (A run that went through a portal comes back.)
				if (app.world().entryHash() != home) app.enterWorld(home, &at, yaw);
				const u32 switches = app.worldSwitches();
				app.teleport(at);
				for (int i = 0; i < perSecond; ++i) app.update(Input {}, dt);
				if (!pl.placed()) {
					// Ground higher than the eyes there (a point of the grid up the
					// slope): from above, then. (Placement looks 2 m up from the feet.)
					app.teleport({at.x, at.y + 400.0f, at.z});
					for (int i = 0; i < perSecond; ++i) app.update(Input {}, dt);
				}
				const Vec3 start = pl.feet;
				const u32 pops = pl.popUps, unstuck = pl.unstuck;
				int air = 0, longestAir = 0;
				if (!gridMetres) {
					if (run < 0)
						printf("-- standing still\n");
					else
						printf("-- run and jump towards %d degrees\n", run * 45);
				}
				const int total = run < 0 ? 5 * perSecond : 9 * perSecond;
				const int every = max(perSecond / 10, 1);
				for (int f = 0; f < total; ++f) {
					Input in;
					if (run >= 0) {
						in.moveZ = f < 4 * perSecond ? 1.0f : 0.0f;
						in.jump = f >= perSecond * 5 / 6 && f < perSecond; // held for a sixth of a second
					}
					app.update(in, dt);
					air = pl.grounded || pl.swimming || pl.climbing ? 0 : air + 1;
					longestAir = air > longestAir ? air : longestAir;
					if (!gridMetres && (f % every == 0 || f == total - 1)) {
						MoveRecorder::format(line, sizeof(line), app.moves().sample(app.moves().count() - 1));
						printf("%s\n", line);
					}
				}
				const float moved = length(pl.feet - start);
				const bool portal = app.worldSwitches() != switches;
				const bool stuck = !portal && (!pl.grounded || longestAir > 3 * perSecond);
				const u32 popped = pl.popUps - pops, freed = pl.unstuck - unstuck;
				stuckRuns += stuck;
				caughtRuns += (popped + freed) > 0;
				++runs;
				const bool full = gridMetres && (stuck || popped || freed);
				if (!gridMetres || full) {
					static const char* const kDirs[8] = {"0", "45", "90", "135", "180", "225", "270", "315"};
					printf("== %s%s: %s at the end, longest time off the ground %.1f s, %.0f cm from the start, popped out of the rock %u, "
					       "unstuck %u\n",
					       run < 0 ? "standing" : "jump towards ", run < 0 ? "" : kDirs[run],
					       portal ? "through a portal" : stuck ? "STUCK" : "fine", double(float(longestAir) * dt), double(moved), unsigned(popped), unsigned(freed));
				}
				if (full) app.moves().write(stdout, "the last frames of that run");
			}
		}
	printf("slope probe: %d of %d runs stuck (off the ground for more than 3 s, or at the end); %d needed the safety net "
	       "(popped out of the rock or unstuck)\n",
	       stuckRuns, runs, caughtRuns);
	return 0;
}

int main(int argc, char** argv) {
	const char* pakPath = nullptr;
	const char* worldName = nullptr;
	const char* detailSpec = nullptr;
	const char* outPrefix = "frame";
	const char* statsPath = nullptr;
	const char* camSpec = nullptr;
	const char* streamMode = "sync";
	const char* probeSpec = nullptr;
	int probeFps = 30, probeGrid = 0;
	int frames = 300, shots = 4;
	bool still = false, cull = true, walk = false, firstPerson = false;
	float timeOfDay = 13.0f, toTime = -1.0f;
	bool standing = false, who = false, wholeChunks = false;
	bool console = false, fight = false, sword = false, inventory = false, pickup = false;
	int menuPage = -1;
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
		else if (arg("--to-time")) toTime = float(atof(argv[++i]));
		else if (arg("--slope-probe")) probeSpec = argv[++i];
		else if (arg("--probe-fps")) probeFps = atoi(argv[++i]);
		else if (arg("--probe-grid")) probeGrid = atoi(argv[++i]);
		else if (arg("--menu")) menuPage = atoi(argv[++i]);
		else if (strcmp(argv[i], "--console") == 0) console = true;
		else if (strcmp(argv[i], "--fight") == 0) fight = true;
		else if (strcmp(argv[i], "--sword") == 0) sword = true;
		else if (strcmp(argv[i], "--inventory") == 0) inventory = true;
		else if (strcmp(argv[i], "--pickup") == 0) pickup = walk = true;
		else if (strcmp(argv[i], "--standing") == 0) standing = true;
		else if (strcmp(argv[i], "--who") == 0) who = true;
		else if (strcmp(argv[i], "--whole-chunks") == 0) wholeChunks = true;
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
	cfg.clusterCulling = !wholeChunks;
	app.setDrawDistance(drawDistance);
	if (detailSpec && sscanf(detailSpec, "%f,%f", &cfg.detailDistance[g3::DETAIL_MEDIUM], &cfg.detailDistance[g3::DETAIL_SMALL]) != 2) {
		usage();
		return 2;
	}
	app.environment().timeOfDay = timeOfDay;
	app.npcs().config().alive = !standing;
	app.setThirdPerson(!firstPerson);
	if (console) app.setScheme(CONTROLS_CONSOLE);
	if (fight) walk = true;
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
		app.camera().yaw = yaw * 0.0174533f; // (a character takes its heading from it)
		app.teleport({x, y, z});
		viewPitch = app.camera().pitch = pitch * 0.0174533f;
	}

	const WorldHeader& wh = app.world().header();
	printf("%s: %u chunks, %u materials, %u textures, %u portals, %u triangles, chunk %.0f cm\n", app.worldName(),
	       wh.chunkCount, wh.materialCount, wh.textureCount, wh.portalCount, wh.triangleCount, wh.chunkSize);

	// What a loading screen would do.
	app.world().preload(app.camera().pos);
	if (probeSpec) return slopeProbe(app, probeSpec, probeFps, probeGrid);
	if (menuPage >= 0) {
		app.menu().open();
		MenuInput mi;
		for (int k = 0; k < menuPage && menuPage < Menu::PAGES; ++k) {
			mi = MenuInput();
			mi.down = true;
			app.menu().update(mi, app.settings());
		}
		mi = MenuInput();
		mi.confirm = menuPage > 0 && menuPage < Menu::PAGES;
		app.menu().update(mi, app.settings());
	}
	// --fight: next to the nearest enemy of the character's in this world.
	int fightWith = -1;
	int heroIndex = -1;
	if (fight && app.npcs().loaded() && app.npcs().game().hero()) {
		Game& g = app.npcs().game();
		heroIndex = int(g.hero() - &g.npc(0));
		float best = 1e30f;
		for (u32 i = 0; i < g.npcCount(); ++i) {
			const GameNpc& n = g.npc(i);
			if (n.hero || !n.placed || n.world != g.currentWorld() || g.attitude(i, u32(heroIndex)) != 0) continue;
			const float d = n.pos[0] * n.pos[0] + n.pos[2] * n.pos[2];
			if (d < best) {
				best = d;
				fightWith = int(i);
			}
		}
		if (fightWith < 0) {
			fprintf(stderr, "error: nobody in this world is hostile to the character\n");
			return 1;
		}
		const GameNpc& e = g.npc(u32(fightWith));
		app.camera().yaw = 0;
		app.teleport({e.pos[0], e.pos[1] + 400.0f, e.pos[2] - 800.0f});
		app.world().preload(app.camera().pos);
		if (sword) app.menuAction(MENU_GIVE_WEAPON, nullptr);
		printf("fight: %s (%d hit points) against %s (%d)\n", g.nameOf(g.npc(u32(heroIndex))), int(g.hitPoints(g.npc(u32(heroIndex)))),
		       g.nameOf(e), int(g.hitPoints(e)));
	}

	// --inventory: things of every kind, the character hurt (so that a potion shows).
	if (inventory) {
		if (!app.npcs().loaded() || !app.npcs().game().hero()) {
			fprintf(stderr, "error: --inventory needs the scripts' character\n");
			return 1;
		}
		Game& g = app.npcs().game();
		const u32 kinds = app.giveItemKit();
		GameNpc& h = g.npcMut(u32(g.hero() - &g.npc(0)));
		g.setAttribute(h, 0, g.attributeOf(h, 1) / 4);
		printf("inventory: %u kinds of things given; health %d of %d\n", kinds, int(g.attributeOf(h, 0)), int(g.attributeOf(h, 1)));
	}
	// The scripted touches of --inventory: frame, what, where.
	enum { T_POTION, T_PAGE_ARMOR, T_FIRST, T_PAGE_FOOD, T_PAGE_ALL };
	struct Touch {
		int from, to, what;
	};
	static const Touch kTouches[] = {{8, 9, T_POTION},       {14, 15, T_POTION},    {70, 71, T_PAGE_ARMOR}, {76, 77, T_FIRST},
	                                 {82, 83, T_FIRST},      {90, 91, T_PAGE_FOOD}, {96, 117, T_FIRST},     {124, 125, -2},
	                                 {130, 131, T_PAGE_ALL}, {136, 137, T_POTION}};
	const auto touchAt = [&](int f, PanelInput& pin) {
		InventoryPanel& panel = app.panel();
		const InventoryView& v = app.inventory();
		for (const Touch& t : kTouches) {
			if (f < t.from || f > t.to) continue;
			pin.touching = true;
			int x = 0, y = 0;
			if (t.what == T_PAGE_ARMOR || t.what == T_PAGE_FOOD || t.what == T_PAGE_ALL) {
				InventoryPanel::pageCentre(t.what == T_PAGE_ARMOR ? TAB_ARMOR : t.what == T_PAGE_FOOD ? TAB_FOOD : TAB_ALL, x, y);
			} else if (t.what == -2) {
				InventoryPanel::crossCentre(1, x, y); // the right arm of the cross
			} else {
				u32 n = 0;
				if (t.what == T_POTION)
					for (u32 k = 0; k < panel.shownCount(); ++k)
						if (v.items[panel.shown(k)].kind == IK_POTION) {
							n = k;
							break;
						}
				panel.tileCentre(n, x, y);
			}
			pin.x = x;
			pin.y = y;
		}
	};

	const float dt = 1.0f / 30.0f;
	u32 maxTris = 0, maxDraws = 0, maxBlended = 0, maxLoaded = 0, maxPending = 0, maxLoadsFrame = 0;
	u64 sumCulled = 0, sumDraws = 0;
	int swimFrames = 0, wadeFrames = 0, underwaterFrames = 0;
	u64 sumTris = 0, sumWritten = 0, sumTested = 0, sumBackface = 0, sumFacing = 0;
	u64 sumInChunks = 0, sumMerged = 0, sumClusters = 0, sumClustersCulled = 0;
	int saved = 0, groundedFrames = 0;
	double updateSum = 0, updateWorst = 0;
	u32 maxInFlight = 0, maxTexturesWaiting = 0;
	int heroFrames = 0, heroRoleFrames[Hero::ROLE_COUNT] = {};
	u64 heroPixels = 0;
	u32 npcShownMax = 0, npcNearMax = 0, npcTrianglesMax = 0, npcWaitingMax = 0, npcNoModelMax = 0;
	int npcFrames = 0;
	u32 npcActorsMax = 0, npcWalkingMax = 0;
	g3::UpdateTimes timeSum;
	u64 collQueries = 0, collItems = 0, collTests = 0;

	int fightBlows = 0, fightDrawn = -1, fightDown = -1;
	for (int f = 0; f < frames; ++f) {
		Input in;
		if (fight && heroIndex >= 0) {
			// Weapon in hand, at the enemy: turn to it, close in, strike again and again.
			Game& g = app.npcs().game();
			const GameNpc& e = g.npc(u32(fightWith));
			const Vec3 feet = app.player().feet;
			const float dx = e.pos[0] - feet.x, dz = e.pos[2] - feet.z;
			const float d = sqrtf(dx * dx + dz * dz);
			float err = atan2f(dx, dz) - app.hero().yaw();
			while (err > 3.14159265f) err -= 6.2831853f;
			while (err < -3.14159265f) err += 6.2831853f;
			if (app.hero().weapon() == WEAPON_NONE && !app.hero().drawing() && f > 10 && e.down == DOWN_NONE) {
				if (sword) in.meleeSlot = true;
				else in.fistSlot = true;
			}
			if (fightDrawn < 0 && app.hero().weapon() != WEAPON_NONE) fightDrawn = f;
			if (e.down == DOWN_NONE) {
				const bool closeIn = d > g.meleeRange(u32(heroIndex), u32(fightWith)) - 20.0f;
				if (console) {
					// The stick held towards the enemy, as the camera sees it.
					const float want = atan2f(dx, dz) - app.camera().yaw;
					in.moveX = closeIn ? sinf(want) : 0.0f;
					in.moveZ = closeIn ? cosf(want) : 0.0f;
					err = 0.0f; // (the character turns to its target by itself)
				} else {
					in.moveX = clamp(err * 2.0f, -1.0f, 1.0f);
					in.moveZ = closeIn && fabsf(err) < 0.6f ? 1.0f : 0.0f;
				}
				in.attack = d < g.meleeRange(u32(heroIndex), u32(fightWith)) + 20.0f && fabsf(err) < 0.5f && f % 6 == 0;
				fightBlows += in.attack;
			} else if (fightDown < 0) {
				fightDown = f;
			}
		} else if (walk) {
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
		if (toTime >= 0.0f) {
			// (Over midnight if the end is the earlier hour.)
			const float span = toTime >= timeOfDay ? toTime - timeOfDay : toTime + 24.0f - timeOfDay;
			app.environment().timeOfDay = fmodf(timeOfDay + span * float(f) / float(frames > 1 ? frames - 1 : 1), 24.0f);
		}
		npcActorsMax = max(npcActorsMax, app.npcs().stats().actors);
		npcWalkingMax = max(npcWalkingMax, app.npcs().stats().walking);
		if (inventory && !fight) in = Input(); // (standing, to watch)
		if (pickup) {
			// To the nearest thing still lying there; A when it is in reach.
			in = Input();
			const Loot& loot = app.loot();
			const Vec3 feet = app.player().feet;
			int best = -1;
			float bestD = 1e30f;
			for (u32 k = 0; k < loot.count(); ++k) {
				const LootItem& li = loot.item(k);
				const float d = std::hypot(li.pos.x - feet.x, li.pos.z - feet.z);
				if (!li.taken && d < bestD) {
					bestD = d;
					best = int(k);
				}
			}
			if (app.lootTarget() >= 0) {
				in.confirm = f % 8 == 0;
			} else if (best >= 0 && bestD < 3000.0f) {
				const LootItem& li = loot.item(u32(best));
				float err = atan2f(li.pos.x - feet.x, li.pos.z - feet.z) - app.hero().yaw();
				while (err > 3.14159265f) err -= 6.2831853f;
				while (err < -3.14159265f) err += 6.2831853f;
				in.moveX = clamp(err * 2.0f, -1.0f, 1.0f);
				in.moveZ = fabsf(err) < 0.5f ? 0.6f : 0.0f;
			}
		}
		if (inventory) {
			PanelInput pin;
			touchAt(f, pin);
			if (f == 160) app.quickUse(1); // (R + right: what the hold put there)
			app.panelInput(pin, dt);
		}
		const double u0 = g3::sys::timeMs();
		app.update(in, dt);
		const double updateMs = g3::sys::timeMs() - u0;
		updateSum += updateMs;
		updateWorst = max(updateWorst, updateMs);
		{
			const g3::UpdateTimes& ut = app.times();
			timeSum.stream += ut.stream;
			timeSum.npcs += ut.npcs;
			timeSum.script += ut.script;
			timeSum.player += ut.player;
			timeSum.camera += ut.camera;
			const g3::CollisionStats& cs = g3::collisionStats();
			collQueries += cs.queries;
			collItems += cs.items;
			collTests += cs.tests;
		}
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

		{
			const NpcStats& ns = app.npcs().stats();
			npcShownMax = max(npcShownMax, ns.shown);
			npcNearMax = max(npcNearMax, ns.near);
			npcTrianglesMax = max(npcTrianglesMax, ns.triangles);
			npcWaitingMax = max(npcWaitingMax, ns.waiting);
			npcNoModelMax = max(npcNoModelMax, ns.noModel);
			npcFrames += ns.shown > 0;
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
		sumInChunks += ws.trianglesInChunks;
		sumMerged += ws.batchesMerged;
		sumClusters += ws.clustersTested;
		sumClustersCulled += ws.clustersCulled;
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
			if (inventory) {
				// The bottom screen next to it.
				app.panel().draw(app.inventory(), true);
				const u16* px = app.panel().canvas().pixels();
				static u8 rgba[Canvas::W * Canvas::H * 4];
				for (int i = 0; i < Canvas::W * Canvas::H; ++i) {
					rgba[i * 4 + 0] = u8(((px[i] >> 11) & 31) * 255 / 31);
					rgba[i * 4 + 1] = u8(((px[i] >> 5) & 63) * 255 / 63);
					rgba[i * 4 + 2] = u8((px[i] & 31) * 255 / 31);
					rgba[i * 4 + 3] = 255;
				}
				snprintf(name, sizeof(name), "%s_bottom_%02d.png", outPrefix, saved);
				if (!stbi_write_png(name, Canvas::W, Canvas::H, 4, rgba, Canvas::W * 4)) {
					fprintf(stderr, "error: cannot write %s\n", name);
					return 1;
				}
			}
			++saved;
		}
	}

	const WorldStats ws = app.world().stats();
	const gfx::MemStats ms = gfx::memStats();
	const double screen = double(gfx::SCREEN_W) * gfx::SCREEN_H * frames;
	const double backfaceShare = sumFacing ? double(sumBackface) / double(sumFacing) : 0.0;

	printf("\n--- %d frames ---\n", frames);
	printf("triangles/frame   avg %llu, max %u; the chunks they are in have %llu (%.0f%% drawn)\n",
	       static_cast<unsigned long long>(sumTris / u64(frames)), maxTris, static_cast<unsigned long long>(sumInChunks / u64(frames)),
	       sumInChunks ? 100.0 * double(sumTris) / double(sumInChunks) : 0.0);
	printf("draw calls/frame  avg %llu, max %u (blended max %u); %llu batches/frame culled by view or distance, %llu drawn "
	       "along with the one before; %llu clusters/frame looked at, %llu of them out of view\n",
	       static_cast<unsigned long long>(sumDraws / u64(frames)), maxDraws, maxBlended,
	       static_cast<unsigned long long>(sumCulled / u64(frames)), static_cast<unsigned long long>(sumMerged / u64(frames)),
	       static_cast<unsigned long long>(sumClusters / u64(frames)), static_cast<unsigned long long>(sumClustersCulled / u64(frames)));
	printf("chunks resident   max %u of %u (pending max %u, loads/frame max %u)\n", maxLoaded, ws.chunksTotal,
	       maxPending, maxLoadsFrame);
	printf("streaming         %s%s: update %.3f ms avg, %.3f ms worst on this PC; in flight max %u, textures waiting max %u\n",
	       streamMode, streamThread && !app.world().workerThread() ? " (no thread: inline)" : "",
	       updateSum / frames, updateWorst, maxInFlight, maxTexturesWaiting);
	printf("update            per frame on this PC: chunks %.3f ms, inhabitants %.3f ms (%.3f in scripts), character %.3f ms, "
	       "camera %.3f ms; collision: %.1f queries looking at %.0f entries and %.0f triangles\n",
	       double(timeSum.stream) / frames, double(timeSum.npcs) / frames, double(timeSum.script) / frames,
	       double(timeSum.player) / frames, double(timeSum.camera) / frames, double(collQueries) / frames,
	       double(collItems) / frames, double(collTests) / frames);
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
		printf("hero              %u triangles, %u clips, %u blends; drawn in %d frames (%.0f pixels each): standing %d, running %d, "
		       "walking %d, turning %d, in the air %d, swimming %d; camera set-up: %s\n",
		       hero.model().triangles(), hero.model().clipCount(), hero.model().blendCount(), heroFrames,
		       heroFrames ? double(heroPixels) / heroFrames : 0.0, heroRoleFrames[Hero::IDLE], heroRoleFrames[Hero::RUN],
		       heroRoleFrames[Hero::WALK], heroRoleFrames[Hero::TURN], heroRoleFrames[Hero::JUMP] + heroRoleFrames[Hero::FALL],
		       heroRoleFrames[Hero::SWIM] + heroRoleFrames[Hero::SWIM_IDLE], app.cameraSet().fromGame ? "the game's" : "this project's");
	} else {
		printf("hero              none in the archive: first person\n");
	}
	if (app.npcs().loaded()) {
		const NpcStats& ns = app.npcs().stats();
		const ScriptStats& vs = app.npcs().game().vm().stats();
		printf("scripts           %.2f MB image, %llu instructions run, %u complaints, %u calls given up, %u uses of members of no "
		       "object, %u calls of engine functions that do nothing yet\n",
		       double(app.npcs().game().vm().imageBytes()) / 1048576.0, static_cast<unsigned long long>(vs.instructions),
		       vs.errors, vs.aborted, vs.nullAccess, vs.stubCalls);
		printf("inhabitants       %u by the scripts, %u in %s at the end; near max %u, shown max %u (%u triangles) in %d "
		       "frames; %u models read, %u in memory with %u textures (%.2f MB); waiting max %u, without models max %u\n",
		       ns.total, ns.here, app.npcs().worldScript(), npcNearMax, npcShownMax, npcTrianglesMax, npcFrames, ns.loads,
		       ns.models, ns.textures, double(ns.textureBytes) / 1048576.0, npcWaitingMax, npcNoModelMax);
		printf("life              %s: %u living at most, %u walking at once; %u state functions called, %u actions carried out "
		       "(%u dropped); ways: %u found, %u not, %u characters put at their goal; animations: %u read, %.2f MB in memory, "
		       "%u missing, %u refused; way net: %u points, %u free points; things to use: %u, got into %u times, %u in use at the end\n",
		       standing ? "off (--standing)" : "on", npcActorsMax, npcWalkingMax, ns.stateCalls, ns.actions, ns.queueFull, ns.paths,
		       ns.noPath, ns.snapped, ns.clips, double(ns.clipBytes) / 1048576.0, ns.clipsMissing, ns.clipsRefused,
		       app.npcs().wayNet().nodeCount(), app.npcs().wayNet().freeCount(), app.npcs().mobs().count(), ns.mobsUsed, ns.mobsInUse);
		if (who)
			for (u32 i = 0; i < app.npcs().actorCount(); ++i) {
				const NpcActor& a = app.npcs().actor(i);
				if (!a.used) continue;
				const GameNpc& n = app.npcs().game().npc(a.npc);
				const Npcs& npcs = app.npcs();
				printf("  %-22.22s %-8s at %7.0f %6.0f %7.0f, %4.0f m away, %3.0f s in %s; thing %s stage %u state %d; body %ld; "
				       "animation %s\n",
				       npcs.game().nameOf(n), npcs.doing(a), double(n.pos[0]), double(n.pos[1]), double(n.pos[2]),
				       double(sqrtf(a.dist2)) / 100.0, double(a.stateTime), npcs.stateName(a.state)[0] ? npcs.stateName(a.state) : "its state",
				       a.mob >= 0 ? npcs.mobs().scheme(u32(a.mob)) : "-", unsigned(a.mobStage), int(a.mobState),
				       static_cast<long>(npcs.bodyState(a)), npcs.animationName(a)[0] ? npcs.animationName(a) : "?");
			}
	} else {
		printf("scripts           none in the archive: the worlds are empty\n");
	}
	if (fight && heroIndex >= 0) {
		Game& g = app.npcs().game();
		const GameNpc& e = g.npc(u32(fightWith));
		const App::FightStats& fs = app.fightStats();
		const NpcStats& ns = app.npcs().stats();
		char downAt[40] = "";
		if (fightDown >= 0) snprintf(downAt, sizeof(downAt), " (down at frame %d)", fightDown);
		printf("fight             weapon in hand after %d frames; %d attacks pressed, %u blows landed (last %d damage), %u taken, "
		       "%u fended off by the character, %u dodged; the enemy %s with %d hit points%s; the character %s with %d of %d; "
		       "inhabitants: %u blows, %u parried, %u knocked out, %u killed, %u moves of the fight AI (tables: %s; %u without one)\n",
		       fightDrawn, fightBlows, fs.blowsLanded, int(fs.lastDamage), fs.blowsTaken, fs.parried, fs.dodged,
		       e.down == DOWN_DEAD ? "dead" : e.down == DOWN_UNCONSCIOUS ? "knocked out" : "standing", int(g.hitPoints(e)),
		       downAt,
		       app.hero().down() ? "down" : "standing", int(app.heroHitPoints()), int(app.heroHitPointsMax()), ns.blows, ns.parried,
		       ns.knockedOut, ns.killed, ns.moves,
		       ns.fightAi == 'F' ? "FIGHT.DAT" : ns.fightAi == 'S' ? "the scripts" : "none read", ns.movesUntabled);
	}
	if (pickup || inventory) {
		const LootStats& ls = app.loot().stats();
		printf("things lying about %u in this world, %u taken (%u picked up by the character), %u drawn in the last frame, %u looks in memory, %u without one\n",
		       ls.count, ls.taken, app.itemStats().pickedUp, ls.drawn, ls.models, ls.noModel);
	}
	if (inventory) {
		const InventoryView& v = app.inventory();
		const App::ItemStats& is = app.itemStats();
		printf("inventory         %u kinds; health %d of %d, mana %d of %d; used %u, refused %u, spoilt %u, put on or off %u; "
		       "quick slots:",
		       v.count, int(v.hp), int(v.hpMax), int(v.mana), int(v.manaMax), is.used, is.refused, is.interrupted, is.changed);
		for (int q = 0; q < QUICK_SLOTS; ++q) printf(" %s x%d,", v.quick[q] != SCRIPT_NONE ? v.quickName[q] : "-", int(v.quickCount[q]));
		printf("\n");
		for (u32 i = 0; i < v.count; ++i) {
			const ItemEntry& e = v.items[i];
			printf("  %-34s x%-4d %-8s%s%s%s\n", e.name, int(e.count), e.action, (e.flags & ITEMF_EQUIPPED) ? " worn" : "",
			       (e.flags & ITEMF_IN_HAND) ? " in-hand" : "", (e.flags & ITEMF_BLOCKED) ? " too-weak" : "");
		}
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
		        "\"heroSwimFrames\":%d,\"heroPixels\":%llu,\"npcsTotal\":%u,\"npcsHere\":%u,\"npcsShownMax\":%u,"
		        "\"npcFrames\":%d,\"npcLoads\":%u,\"npcTrianglesMax\":%u,\"scriptErrors\":%u,\"npcActorsMax\":%u,"
		        "\"npcWalkingMax\":%u,\"npcStateCalls\":%u,\"npcPaths\":%u,\"npcSnapped\":%u,\"npcClips\":%u,"
		        "\"collisionQueries\":%.1f,\"collisionItems\":%.1f,\"collisionTests\":%.1f,\"trianglesInChunksAvg\":%llu,"
		        "\"drawCallsAvg\":%llu,\"clustersAvg\":%llu,\"mobsUsed\":%u,\"mobsInUse\":%u}\n",
		        frames, static_cast<unsigned long long>(sumTris / u64(frames)), maxTris, maxDraws, ws.chunksTotal,
		        maxLoaded, ws.peakResidentBytes, ws.budgetRejects, ws.loadFailures, double(sumWritten) / screen,
		        backfaceShare, groundedFrames, swimFrames, maxBlended,
		        static_cast<unsigned long long>(sumCulled / u64(frames)), app.worldSwitches(),
		        static_cast<unsigned long long>(sumWritten), heroFrames, heroRoleFrames[Hero::RUN],
		        heroRoleFrames[Hero::JUMP] + heroRoleFrames[Hero::FALL],
		        heroRoleFrames[Hero::SWIM] + heroRoleFrames[Hero::SWIM_IDLE],
		        static_cast<unsigned long long>(heroFrames ? heroPixels / u64(heroFrames) : 0), app.npcs().stats().total,
		        app.npcs().stats().here, npcShownMax, npcFrames, app.npcs().stats().loads, npcTrianglesMax,
		        app.npcs().loaded() ? app.npcs().game().vm().stats().errors + app.npcs().game().vm().stats().aborted : 0u,
		        npcActorsMax, npcWalkingMax, app.npcs().stats().stateCalls, app.npcs().stats().paths, app.npcs().stats().snapped,
		        app.npcs().stats().clips, double(collQueries) / frames, double(collItems) / frames, double(collTests) / frames,
		        static_cast<unsigned long long>(sumInChunks / u64(frames)), static_cast<unsigned long long>(sumDraws / u64(frames)),
		        static_cast<unsigned long long>(sumClusters / u64(frames)), app.npcs().stats().mobsUsed, app.npcs().stats().mobsInUse);
		fclose(fp);
	}

	app.shutdown();
	gfx::shutdown();
	return (ws.loadFailures == 0) ? 0 : 3;
}
