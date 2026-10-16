// Unit and integration tests for the data path: archive, texture layout,
// chunker, streaming. Usage: test_core <scratch dir>
#include <set>
#include "app.h"
#include "saves.h"
#include "audio_host.h"
#include "bake.h"
#include "water_grid.h"
#include "clouds.h"
#include "sound_bake.h"
#include "chunker.h"
#include "vcache.h"
#include "daedalus.h"
#include "display_timing.h"
#include "font_bake.h"
#include "g3ds/font.h"
#include "g3ds/pak.h"
#include "g3ds/tex.h"
#include "gfx_soft.h"
#include "importers.h"
#include "lang.h"
#include "loader.h"
#include "ui_font.h"
#include "waynet.h"
#include "weather.h"
#include "ground_fx.h"
#include "puddles.h"
#include "rain.h"
#include "ripples.h"
#include "sun_bake.h"
#include "weather_sfx.h"
#include "godrays.h"
#include "water_fx.h"
#include "water_texture.h"
#include "model_bake.h"
#include "pacer.h"
#include "stereo.h"
#include "readahead.h"
#include "rooms.h"
#include "bytes.h"
#include "triggers.h"
#include "game.h"
#include "reproject.h"
#include "pak_writer.h"
#include "rg_etc1.h"
#include "test_models.h"
#include "texture_bake.h"
#include "vertex_fx.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cmath>
#include <set>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

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

static std::string gDir;

// ---- the light of lamps baked into the vertices ----
static void testLampLight() {
	CHECK(lampLight(0.0f, 800.0f, 1.0f) == 1.0f && lampLight(800.0f, 800.0f, 1.0f) == 0.0f && lampLight(900.0f, 800.0f, 1.0f) == 0.0f);
	CHECK(lampLight(200.0f, 800.0f, 1.0f) > lampLight(400.0f, 800.0f, 1.0f) && lampLight(400.0f, 800.0f, 1.0f) > lampLight(700.0f, 800.0f, 1.0f));
	CHECK(lampLight(300.0f, 800.0f, 0.2f) < lampLight(300.0f, 800.0f, 1.0f) && lampLight(300.0f, 800.0f, -1.0f) == 0.0f);

	// A floor of two big triangles (front normal up), a wall across x = 1125, a torch above the middle.
	auto build = [&](bool staticLamp) {
		SrcScene scene;
		scene.materials.resize(1);
		auto tri = [&](const float a[3], const float b[3], const float c[3]) {
			SrcTriangle t {};
			const float* p[3] = {a, b, c};
			for (int k = 0; k < 3; ++k) {
				for (int i = 0; i < 3; ++i) t.v[k].pos[i] = p[k][i];
				t.v[k].rgba[0] = t.v[k].rgba[1] = t.v[k].rgba[2] = 60;
				t.v[k].rgba[3] = 255;
			}
			t.material = 0;
			scene.triangles.push_back(t);
		};
		const float f00[3] = {0, 0, 0}, f10[3] = {2000, 0, 0}, f01[3] = {0, 0, 2000}, f11[3] = {2000, 0, 2000};
		tri(f00, f10, f01);
		tri(f10, f11, f01);
		// The wall (a thin upright quad at x = 1125): two triangles with fronts to the lamp or not, it blocks either way.
		const float w0[3] = {1125, 0, -500}, w1[3] = {1125, 600, -500}, w2[3] = {1125, 0, 2500}, w3[3] = {1125, 600, 2500};
		tri(w0, w1, w2);
		tri(w1, w3, w2);
		SrcLight l;
		l.pos[0] = 1000; l.pos[1] = 150; l.pos[2] = 1000;
		l.range = 900;
		l.color[0] = 255; l.color[1] = 200; l.color[2] = 120;
		l.isStatic = staticLamp;
		scene.lights.push_back(l);
		return scene;
	};
	auto lightAt = [&](const SrcScene& s, float x, float z, int& weight) -> int {
		for (const SrcTriangle& t : s.triangles)
			for (const SrcVertex& v : t.v)
				if (std::fabs(v.pos[0] - x) < 1.0f && std::fabs(v.pos[1]) < 1.0f && std::fabs(v.pos[2] - z) < 1.0f) {
					weight = v.rgba[3];
					return v.rgba[0];
				}
		weight = -1;
		return -1;
	};
	const std::vector<AoMaterial> materials(1);
	LightOptions off;
	SrcScene a = build(false);
	LightStats st;
	bakeLights(a, materials, off, st); // (not asked for: nothing changes)
	CHECK(a.triangles.size() == 4 && st.lamps == 0);
	LightOptions on;
	on.enabled = true;
	SrcScene s = build(true);
	bakeLights(s, materials, on, st); // (the world has this one's light already)
	CHECK(s.triangles.size() == 4 && st.skippedStatic == 1);
	on.statics = true;
	s = build(true);
	bakeLights(s, materials, on, st);
	CHECK(st.lamps == 1);
	s = build(false);
	bakeLights(s, materials, on, st);
	std::printf("  lamp light: %u triangles added, %llu corners lit, %.2f M shadow rays\n", st.added, static_cast<unsigned long long>(st.corners),
	            double(st.rays) / 1e6);
	CHECK(st.lamps == 1 && st.added > 20 && st.corners > 20 && s.triangles.size() == 4 + st.added);
	int wUnder, wNear, wFar, wShadow;
	const int under = lightAt(s, 1000, 1000, wUnder), near = lightAt(s, 750, 1000, wNear), far = lightAt(s, 0, 1000, wFar); // (about 1000 away: the lamp does not reach)
	const int shadow = lightAt(s, 1250, 1000, wShadow); // (as near as 750, but the wall is between)
	CHECK(under > 0 && near > 0 && far > 0 && shadow > 0);
	CHECK(under > near && near > far && far == 60 && wFar == 255);       // out of its reach the vertex is as it was
	CHECK(wUnder < wNear && wNear < 255 && under > 60);                  // lit: more light, less of it the sun's
	CHECK(shadow == 60 && wShadow == 255);                               // (behind the wall nothing arrives)
	// At night (the day colour 0.2) the lamp's share is what is left: the lit corner is brighter than the unlit.
	auto atNight = [&](int rgb, int w) { return float(rgb) * (1.0f + float(w) / 255.0f * (0.2f - 1.0f)); };
	CHECK(atNight(under, wUnder) > atNight(far, wFar) * 2.0f);
	// And by day it is the sun's and the lamp's together, never darker than before.
	CHECK(atNight(under, wUnder) < float(under));
	CHECK(st.added <= on.maxExtra);
}

// ---- water: one light all over it ----
static void testWaterLight() {
	SrcScene scene;
	scene.materials.resize(2);
	scene.materials[0].water = false;
	scene.materials[1].water = true;
	SrcTriangle land {}, sea {};
	land.material = 0;
	sea.material = 1;
	for (int k = 0; k < 3; ++k) {
		for (int c = 0; c < 3; ++c) {
			land.v[k].rgba[c] = u8(30 + 40 * k + c);
			sea.v[k].rgba[c] = u8(10 + 90 * k + c); // (patches: a corner dark, one bright)
		}
		land.v[k].rgba[3] = 255;
		sea.v[k].rgba[3] = 0; // (indoors, say)
	}
	scene.triangles = {land, sea, sea};
	CHECK(applyWaterLight(scene, 0) == 0 && scene.triangles[1].v[0].rgba[0] == 10);
	CHECK(applyWaterLight(scene, 200) == 2);
	bool same = true;
	for (int t = 1; t < 3; ++t)
		for (int k = 0; k < 3; ++k) same = same && std::memcmp(scene.triangles[size_t(t)].v[k].rgba, scene.triangles[1].v[0].rgba, 3) == 0;
	CHECK(same && scene.triangles[1].v[0].rgba[2] == 200 && scene.triangles[1].v[0].rgba[0] < scene.triangles[1].v[0].rgba[1] &&
	      scene.triangles[1].v[0].rgba[1] < scene.triangles[1].v[0].rgba[2]);
	CHECK(scene.triangles[1].v[2].rgba[3] == 0); // (the daylight weight is left as it was)
	CHECK(scene.triangles[0].v[1].rgba[0] == 70 && scene.triangles[0].v[1].rgba[3] == 255); // (land is not touched)
	float tint[3];
	waterTint(255, tint);
	CHECK(tint[2] == 1.0f && tint[0] < tint[1] && tint[1] < tint[2]);
}

// Fog zones: the weight fades in from the zone's edge; the water in a swamp zone turns green, outside it stays.
static void testFogZones() {
	FogZone z {};
	z.bbMin[0] = -10000, z.bbMax[0] = 10000, z.bbMin[1] = -1000, z.bbMax[1] = 3000, z.bbMin[2] = -10000, z.bbMax[2] = 10000;
	z.inner = 0.5f;
	CHECK(fogZoneWeight(z, 0, 0, 0) == 1.0f && fogZoneWeight(z, 20000, 0, 0) == 0.0f && fogZoneWeight(z, 0, 9000, 0) == 0.0f);
	const float edge = fogZoneWeight(z, 9900, 0, 0), mid = fogZoneWeight(z, 7500, 0, 0);
	CHECK(edge > 0.0f && edge < mid && mid < 1.0f);
	CHECK(sizeof(FogZone) == 36);

	SrcScene scene;
	scene.materials.resize(1);
	scene.materials[0].water = true;
	SrcFogZone sz;
	std::copy_n(z.bbMin, 3, sz.bbMin);
	std::copy_n(z.bbMax, 3, sz.bbMax);
	sz.inner = 1.0f;
	sz.color[0] = 80, sz.color[1] = 120, sz.color[2] = 40; // green
	scene.fogZones.push_back(sz);
	SrcTriangle in {}, out {};
	in.material = out.material = 0;
	for (int k = 0; k < 3; ++k) {
		in.v[k].pos[0] = float(k) * 100.0f;
		out.v[k].pos[0] = 50000.0f + float(k) * 100.0f;
		for (int c = 0; c < 3; ++c) in.v[k].rgba[c] = out.v[k].rgba[c] = 200;
	}
	scene.triangles = {in, out};
	CHECK(applyWaterBiome(scene, 0.0f) == 0 && applyWaterBiome(scene, 1.0f) == 1);
	const u8* g = scene.triangles[0].v[0].rgba;
	CHECK(g[1] == 200 && g[0] < 200 && g[2] < g[0]); // (green kept, red a little less, blue the least)
	CHECK(scene.triangles[1].v[0].rgba[0] == 200 && scene.triangles[1].v[0].rgba[2] == 200); // (outside: as it was)
}

// --water-grid: the water cut finely along the shores only, with the waves' weight fading towards the coarse water.
static void testWaterGrid() {
	SrcScene scene;
	scene.materials.resize(2);
	scene.materials[1].water = true;
	auto tri = [&](u32 m, float x0, float y0, float z0, float x1, float y1, float z1, float x2, float y2, float z2) {
		SrcTriangle t {};
		t.material = m;
		const float p[3][3] = {{x0, y0, z0}, {x1, y1, z1}, {x2, y2, z2}};
		for (int k = 0; k < 3; ++k) {
			for (int a = 0; a < 3; ++a) t.v[k].pos[a] = p[k][a];
			t.v[k].uv[0] = p[k][0] / 1000.0f;
			t.v[k].uv[1] = p[k][2] / 1000.0f;
			t.v[k].rgba[0] = t.v[k].rgba[1] = t.v[k].rgba[2] = 180;
			t.v[k].rgba[3] = 255;
			t.v[k].wind = 99; // (to be replaced by the weight)
		}
		scene.triangles.push_back(t);
	};
	// A sea of 40 x 40 km at y = 0; land: a flat shore at y = 0 crossing it (a strip of ground tilted through the water's
	// level) at x 0..2000, z 0..2000, and far away a stretch of ground 10 m below the sea (a sea floor: no shore).
	tri(1, -20000, 0, -20000, 20000, 0, -20000, -20000, 0, 20000);
	tri(1, 20000, 0, -20000, 20000, 0, 20000, -20000, 0, 20000);
	tri(0, 0, -300, 0, 2000, 300, 0, 0, 300, 2000);
	tri(0, 2000, 300, 0, 2000, 300, 2000, 0, 300, 2000);
	tri(0, 15000, -1000, 15000, 15500, -1000, 15000, 15000, -1000, 15500);
	WaterGridOptions opt;
	opt.cell = 200.0f;
	WaterGrid off(scene, WaterGridOptions {});
	CHECK(!off.active());
	WaterGrid grid(scene, opt);
	CHECK(grid.active() && grid.stats().shoreTiles >= 1 && grid.stats().fineTiles >= grid.stats().shoreTiles && grid.stats().fineTiles < 40);
	std::vector<SrcTriangle> pieces;
	for (int i = 0; i < 2; ++i) grid.cut(scene.triangles[size_t(i)], pieces);
	// not a million cells of 2 m for a sea this big: the fine water is the shore's and its neighbours'
	CHECK(pieces.size() > 100 && pieces.size() < 30000);
	double area = 0;
	int fineWithWeight = 0, atFull = 0;
	bool wound = true, onlyWater = true, weightsOk = true;
	float maxEdge = 0;
	for (const SrcTriangle& t : pieces) {
		const double cr = double(t.v[1].pos[0] - t.v[0].pos[0]) * (t.v[2].pos[2] - t.v[0].pos[2]) -
		                  double(t.v[2].pos[0] - t.v[0].pos[0]) * (t.v[1].pos[2] - t.v[0].pos[2]);
		wound = wound && cr > 0 ;
		area += 0.5 * std::fabs(cr);
		onlyWater = onlyWater && t.material == 1;
		for (int k = 0; k < 3; ++k) {
			weightsOk = weightsOk && t.v[k].wind <= 127 && t.v[k].rgba[0] == 180 && t.v[k].pos[1] == 0.0f;
			fineWithWeight += t.v[k].wind > 0;
			atFull += t.v[k].wind == 127;
			const float dx = t.v[(k + 1) % 3].pos[0] - t.v[k].pos[0], dz = t.v[(k + 1) % 3].pos[2] - t.v[k].pos[2];
			if (std::fabs(t.v[k].pos[0]) < 2500 && std::fabs(t.v[k].pos[2]) < 2500 && std::fabs(dx) < 1e6f) maxEdge = std::max(maxEdge, std::sqrt(dx * dx + dz * dz));
		}
	}
	CHECK(wound && onlyWater && weightsOk);
	CHECK(std::fabs(area - 40000.0 * 40000.0) < 40000.0 * 40000.0 * 1e-4); // (the sea is all still there)
	CHECK(fineWithWeight > 100 && atFull > 20); // (the waves have a weight where the water is fine, and a place where it is 1)
	CHECK(maxEdge < 300.0f); // (nothing near the shore is a giant)
	// a piece far out at sea: coarse, no weight; one at the shore: cut, with a weight that is 0 at the border of the fine water
	bool farCoarse = true;
	for (const SrcTriangle& t : pieces)
		if (std::fabs(t.v[0].pos[0]) > 12000 || std::fabs(t.v[0].pos[2]) > 12000)
			for (int k = 0; k < 3; ++k) farCoarse = farCoarse && t.v[k].wind == 0;
	CHECK(farCoarse);
	// the weight along a line out to sea from the shore falls to 0 and stays there
	bool monotone = true;
	int last = 127;
	const float xs[] = {1000, 3000, 5000, 7000, 9000, 12000};
	for (float x : xs) {
		float best = 1e9f;
		int w = 0;
		for (const SrcTriangle& t : pieces)
			for (int k = 0; k < 3; ++k) {
				const float d = std::fabs(t.v[k].pos[0] - x) + std::fabs(t.v[k].pos[2] - 1000.0f);
				if (d < best) {
					best = d;
					w = t.v[k].wind;
				}
			}
		monotone = monotone && w <= last + 127; // (not asked more than that: coarse corners are 0)
		last = w;
	}
	CHECK(monotone && last == 0);
	// rivers: with riverWidth the sea keeps its waves, a narrow channel between two banks gets none, a wide one does
	{
		WaterGridOptions ro = opt;
		ro.riverWidth = 4000.0f;
		WaterGrid sea(scene, ro);
		std::vector<SrcTriangle> seaPieces;
		for (int i = 0; i < 2; ++i) sea.cut(scene.triangles[size_t(i)], seaPieces);
		int full = 0;
		for (const SrcTriangle& t : seaPieces)
			for (int k = 0; k < 3; ++k) full += t.v[k].wind == 127;
		CHECK(full > 20);
		auto channel = [&](float half, bool river) {
			scene.triangles.clear();
			const float len = 6000.0f, bank = 1000.0f;
			// water, and a bank each side sloping through the level at z = +-half
			tri(1, -len, 0, -half - 200, len, 0, -half - 200, -len, 0, half + 200);
			tri(1, len, 0, -half - 200, len, 0, half + 200, -len, 0, half + 200);
			for (int side = -1; side <= 1; side += 2) {
				const float in = side * (half - 100), out = side * (half + bank);
				tri(0, -len, -100, in, len, -100, in, -len, 300, out);
				tri(0, len, -100, in, len, 300, out, -len, 300, out);
			}
			WaterGridOptions o = opt;
			o.riverWidth = river ? 4000.0f : 0.0f;
			WaterGrid g(scene, o);
			std::vector<SrcTriangle> out;
			for (int i = 0; i < 2; ++i) g.cut(scene.triangles[size_t(i)], out);
			int top = 0;
			for (const SrcTriangle& t : out)
				for (int k = 0; k < 3; ++k) top = std::max<int>(top, t.v[k].wind);
			return top;
		};
		CHECK(channel(400.0f, false) > 60);  // (without the option a small channel gets waves)
		CHECK(channel(400.0f, true) == 0);   // a 8 m wide river: calm
		CHECK(channel(6000.0f, true) > 60);  // a 120 m wide water: waves
	}
}

// fix185: reflections baked into the fine water (WaterGridOptions::reflect).
static void testWaterReflect() {
	SrcScene scene;
	scene.materials.resize(2);
	scene.materials[1].water = true;
	auto tri = [&](u32 m, float x0, float y0, float z0, float x1, float y1, float z1, float x2, float y2, float z2, u8 r, u8 g, u8 b) {
		SrcTriangle t {};
		t.material = m;
		const float p[3][3] = {{x0, y0, z0}, {x1, y1, z1}, {x2, y2, z2}};
		for (int k = 0; k < 3; ++k) {
			for (int a = 0; a < 3; ++a) t.v[k].pos[a] = p[k][a];
			t.v[k].rgba[0] = r, t.v[k].rgba[1] = g, t.v[k].rgba[2] = b;
			t.v[k].rgba[3] = 255;
		}
		scene.triangles.push_back(t);
	};
	tri(1, -20000, 0, -20000, 20000, 0, -20000, -20000, 0, 20000, 180, 180, 180);
	tri(1, 20000, 0, -20000, 20000, 0, 20000, -20000, 0, 20000, 180, 180, 180);
	// a shore, and a green cliff 30 m high standing in the water along z = 0, x -1000..3000
	tri(0, 0, -300, 0, 2000, 300, 0, 0, 300, 2000, 120, 120, 120);
	tri(0, 2000, 300, 0, 2000, 300, 2000, 0, 300, 2000, 120, 120, 120);
	tri(0, -1000, -200, 0, 3000, -200, 0, -1000, 3000, 0, 60, 140, 60);
	tri(0, 3000, -200, 0, 3000, 3000, 0, -1000, 3000, 0, 60, 140, 60);
	auto run = [&](float reflect, unsigned threads, WaterGridStats* st = nullptr) {
		WaterGridOptions o;
		o.cell = 200.0f;
		o.reflect = reflect;
		o.threads = threads;
		WaterGrid g(scene, o);
		std::vector<SrcTriangle> out;
		for (int i = 0; i < 2; ++i) g.cut(scene.triangles[size_t(i)], out);
		if (st) *st = g.stats();
		return out;
	};
	const std::vector<SrcTriangle> off = run(0.0f, 1);
	WaterGridStats st;
	const std::vector<SrcTriangle> on = run(0.8f, 1, &st);
	CHECK(off.size() == on.size() && st.reflectCorners > 0 && st.reflectRays > 0 && st.reflectVertices > 0);
	// without it nothing changes; with it the water by the cliff (in front of it, z -300..-700) is darker and greener,
	// and the open sea far out is as it was
	bool offPlain = true, farPlain = true;
	int nearN = 0, nearDark = 0, nearGreen = 0;
	for (size_t i = 0; i < on.size(); ++i)
		for (int k = 0; k < 3; ++k) {
			const SrcVertex& a = off[i].v[k];
			const SrcVertex& b = on[i].v[k];
			offPlain = offPlain && a.rgba[0] == 180 && a.rgba[1] == 180;
			if (std::fabs(b.pos[0]) > 12000 || std::fabs(b.pos[2]) > 12000) farPlain = farPlain && b.rgba[0] == 180 && b.rgba[1] == 180 && b.rgba[2] == 180;
			if (b.pos[0] > 0 && b.pos[0] < 2000 && b.pos[2] < -300 && b.pos[2] > -700) {
				++nearN;
				nearDark += b.rgba[0] < 140;
				nearGreen += b.rgba[1] > b.rgba[0] && b.rgba[1] > b.rgba[2];
			}
		}
	CHECK(offPlain && farPlain);
	CHECK(nearN > 10 && nearDark == nearN && nearGreen == nearN);
	// the same with threads
	const std::vector<SrcTriangle> par = run(0.8f, 4);
	bool same = par.size() == on.size();
	for (size_t i = 0; same && i < on.size(); ++i)
		for (int k = 0; k < 3; ++k) same = same && std::equal(par[i].v[k].rgba, par[i].v[k].rgba + 4, on[i].v[k].rgba);
	CHECK(same);
}

static void testHash() {
	CHECK(hashName("tex/Grass") == hashName("TEX/grass"));
	CHECK(hashName("tex\\grass") == hashName("tex/grass"));
	CHECK(hashName("tex/grass") != hashName("tex/grasS2"));
}

static void testPak() {
	std::vector<u8> text(50000);
	for (size_t i = 0; i < text.size(); ++i) text[i] = u8("gothic"[i % 6]); // compresses well
	std::vector<u8> noise(4097);
	std::mt19937 rng(1);
	for (u8& b : noise) b = u8(rng()); // does not compress
	std::vector<u8> empty;

	PakWriter w;
	w.add("meta/text", ASSET_META, text);
	w.add("blob/noise", ASSET_BLOB, noise);
	w.add("blob/empty", ASSET_BLOB, empty);
	bool threw = false;
	try {
		w.add("META/TEXT", ASSET_META, text); // same name after case folding
	} catch (const std::exception&) {
		threw = true;
	}
	CHECK(threw);
	const std::string path = gDir + "/test.pak";
	w.write(path);

	Pak p;
	CHECK(p.open(path.c_str()));
	CHECK(p.count() == 3);
	const PakEntry* e = p.find("Meta/Text");
	CHECK(e && e->codec == PAK_LZ4 && e->size == text.size() && e->packedSize < text.size() / 10);
	if (e) {
		std::vector<u8> out(e->size);
		CHECK(p.read(*e, out.data()) && out == text);
		CHECK(std::string(p.name(*e)) == "meta/text");
	}
	e = p.find("blob/noise");
	CHECK(e && e->codec == PAK_RAW);
	if (e) {
		void* mem = p.readAlloc(*e);
		CHECK(mem && std::memcmp(mem, noise.data(), noise.size()) == 0);
		free(mem);
	}
	e = p.find("blob/empty");
	CHECK(e && e->size == 0);
	CHECK(p.find("blob/nothing") == nullptr);
	p.close();

	// A truncated archive must be rejected, not half-loaded.
	FILE* f = fopen(path.c_str(), "rb");
	std::vector<u8> bytes(200);
	const size_t n = fread(bytes.data(), 1, bytes.size(), f);
	fclose(f);
	const std::string bad = gDir + "/truncated.pak";
	f = fopen(bad.c_str(), "wb");
	fwrite(bytes.data(), 1, n, f);
	fclose(f);
	CHECK(!p.open(bad.c_str()));
	CHECK(!p.open((gDir + "/does-not-exist.pak").c_str()));
}

static void testTexSolid() {
	// The stand-in texture for a material whose own has not come: decodes to the colour asked for (within the 4-bit steps).
	const u8 colours[][3] = {{0, 0, 0}, {255, 255, 255}, {128, 100, 60}, {30, 200, 90}, {250, 10, 10}};
	for (const auto& c : colours) {
		TexHeader h;
		u8 data[32];
		texSolid(c[0], c[1], c[2], h, data);
		CHECK(texValidate(h, 32));
		std::vector<u8> rgba(8 * 8 * 4);
		texDecodeLevel(h, data, 0, rgba.data());
		for (int px = 0; px < 64; ++px)
			for (int k = 0; k < 3; ++k) CHECK(std::abs(int(rgba[size_t(px) * 4 + size_t(k)]) - int(c[k])) <= 12);
	}
}

static void testEtc1BlockDecoder() {
	// Our decoder against an independent implementation (rg_etc1's unpacker).
	std::mt19937 rng(7);
	int mismatches = 0;
	for (int i = 0; i < 20000; ++i) {
		u8 block[8];
		for (u8& b : block) b = u8(rng());
		unsigned int ref[16];
		// rg_etc1 reports invalid differential blocks (base + delta out of
		// range); real encoders never emit them, so they are not compared.
		if (!rg_etc1::unpack_etc1_block(block, ref)) continue;
		u8 ours[64];
		etc1DecodeBlock(block, ours);
		for (int px = 0; px < 16; ++px) {
			u8 r[4];
			std::memcpy(r, &ref[px], 4);
			if (r[0] != ours[px * 4] || r[1] != ours[px * 4 + 1] || r[2] != ours[px * 4 + 2]) ++mismatches;
		}
	}
	CHECK(mismatches == 0);
}

static double psnr(const std::vector<u8>& a, const std::vector<u8>& b, int channels = 3) {
	double se = 0;
	size_t n = 0;
	for (size_t i = 0; i + 3 < a.size(); i += 4)
		for (int c = 0; c < channels; ++c, ++n) {
			const double d = double(a[i + size_t(c)]) - double(b[i + size_t(c)]);
			se += d * d;
		}
	return se == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / (se / double(n)));
}

static void testTexture() {
	// Smooth opaque image: ETC1 must reproduce it closely and keep orientation.
	SrcImage img;
	img.width = 64;
	img.height = 32;
	img.rgba.resize(64 * 32 * 4);
	for (int y = 0; y < 32; ++y)
		for (int x = 0; x < 64; ++x) {
			u8* p = &img.rgba[size_t(y * 64 + x) * 4];
			p[0] = u8(x * 4);
			p[1] = u8(y * 8);
			p[2] = u8(128 + 60 * std::sin(x * 0.2) * std::cos(y * 0.3));
			p[3] = 255;
		}
	TexBakeOptions opt;
	opt.maxSize = 64;
	TexBakeInfo info;
	std::vector<u8> baked = bakeTexture(img, opt, &info);
	TexHeader h;
	std::memcpy(&h, baked.data(), sizeof(h));
	CHECK(texValidate(h, u32(baked.size() - sizeof(h))));
	CHECK(h.width == 64 && h.height == 32 && h.format == TEX_ETC1 && !info.alpha);
	CHECK(h.mipCount == 3); // 64x32, 32x16, 16x8: PICA stops at 8 texels
	CHECK(h.dataSize == (64 * 32 + 32 * 16 + 16 * 8) / 2);

	std::vector<u8> decoded(64 * 32 * 4);
	texDecodeLevel(h, baked.data() + sizeof(h), 0, decoded.data());
	const double quality = psnr(img.rgba, decoded);
	std::printf("  ETC1 gradient PSNR: %.1f dB\n", quality);
	CHECK(quality > 32.0);

	// PICA stores rows bottom-up: the first stored block must be the image's
	// bottom-left corner. Decode it by hand, bypassing texDecodeLevel.
	SrcImage quad;
	quad.width = quad.height = 16;
	quad.rgba.resize(16 * 16 * 4);
	for (int y = 0; y < 16; ++y)
		for (int x = 0; x < 16; ++x) {
			static const u8 colors[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
			const u8* c = colors[(y >= 8) * 2 + (x >= 8)]; // TL red, TR green, BL blue, BR white
			u8* p = &quad.rgba[size_t(y * 16 + x) * 4];
			p[0] = c[0];
			p[1] = c[1];
			p[2] = c[2];
			p[3] = 255;
		}
	opt.maxSize = 16;
	baked = bakeTexture(quad, opt, nullptr);
	std::memcpy(&h, baked.data(), sizeof(h));
	const u8* first = baked.data() + sizeof(h);
	u8 be[8];
	for (int i = 0; i < 8; ++i) be[i] = first[7 - i];
	unsigned int px[16];
	rg_etc1::unpack_etc1_block(be, px);
	u8 c[4];
	std::memcpy(c, &px[0], 4);
	CHECK(c[0] < 40 && c[1] < 40 && c[2] > 215); // blue = bottom-left
	decoded.assign(16 * 16 * 4, 0);
	texDecodeLevel(h, first, 0, decoded.data());
	CHECK(decoded[0] > 215 && decoded[1] < 40);                         // top-left stays red
	CHECK(decoded[(15 * 16 + 15) * 4 + 0] > 215 && decoded[(15 * 16 + 15) * 4 + 2] > 215); // bottom-right white

	// Cut-out texture: becomes ETC1A4 and the mask survives.
	SrcImage cut;
	cut.width = cut.height = 32;
	cut.rgba.resize(32 * 32 * 4);
	for (int y = 0; y < 32; ++y)
		for (int x = 0; x < 32; ++x) {
			u8* p = &cut.rgba[size_t(y * 32 + x) * 4];
			const bool solid = (x / 8 + y / 8) % 2 == 0;
			p[0] = 200;
			p[1] = 120;
			p[2] = 40;
			p[3] = solid ? 255 : 0;
		}
	opt.maxSize = 32;
	baked = bakeTexture(cut, opt, &info);
	std::memcpy(&h, baked.data(), sizeof(h));
	CHECK(h.format == TEX_ETC1A4 && (h.flags & TEXF_ALPHA_TEST) && info.alpha);
	CHECK(texValidate(h, u32(baked.size() - sizeof(h))));
	decoded.assign(32 * 32 * 4, 0);
	texDecodeLevel(h, baked.data() + sizeof(h), 0, decoded.data());
	int maskErrors = 0, darkFringe = 0;
	for (int i = 0; i < 32 * 32; ++i) {
		maskErrors += (decoded[size_t(i) * 4 + 3] > 127) != (cut.rgba[size_t(i) * 4 + 3] > 127);
		// Transparent texels must carry the neighbouring colour, not black.
		darkFringe += decoded[size_t(i) * 4] < 120;
	}
	CHECK(maskErrors == 0);
	CHECK(darkFringe == 0);

	// Oversized and non-power-of-two input is brought into PICA's limits.
	SrcImage big;
	big.width = 300;
	big.height = 150;
	big.rgba.assign(300 * 150 * 4, 200);
	opt.maxSize = 128;
	baked = bakeTexture(big, opt, &info);
	CHECK(info.width == 128 && info.height == 64);
	SrcImage tiny;
	tiny.width = 4;
	tiny.height = 2;
	tiny.rgba.assign(4 * 2 * 4, 255);
	baked = bakeTexture(tiny, opt, &info);
	CHECK(info.width == 8 && info.height == 8 && info.mipCount == 1);

	// Several threads on one image make the same bytes as one (plain and with an alpha channel).
	for (int withAlpha = 0; withAlpha < 2; ++withAlpha) {
		SrcImage noise;
		noise.width = noise.height = 128;
		noise.rgba.resize(128 * 128 * 4);
		u32 seed = 12345u;
		for (size_t i = 0; i < noise.rgba.size(); ++i) {
			seed = seed * 1664525u + 1013904223u;
			noise.rgba[i] = u8(seed >> 24);
			if (i % 4 == 3 && !withAlpha) noise.rgba[i] = 255;
		}
		TexBakeOptions one = opt, many = opt;
		one.threads = 1;
		many.threads = 4;
		CHECK(bakeTexture(noise, one, nullptr) == bakeTexture(noise, many, nullptr));
	}
}

static void testFrameBudget() {
	FrameBudget b;
	u32 refused = 0;
	// No limit by default.
	CHECK(b.allow(50.0f, refused) && refused == 0);
	// Time left: a job that fits goes, one that does not waits, but not for ever.
	b.headroomMs = 3.0f;
	CHECK(b.allow(2.0f, refused));
	int waited = 0;
	while (!b.allow(6.0f, refused) && waited < 100) ++waited;
	CHECK(waited == int(b.maxRefusals));
	CHECK(refused == 0);
	// Over the period already: nothing goes until the refusals run out.
	b.headroomMs = -5.0f;
	waited = 0;
	while (!b.allow(0.5f, refused) && waited < 100) ++waited;
	CHECK(waited == int(b.maxRefusals));
}

static void testVertexCache() {
	// A grid of 40 x 40 quads whose triangles come in a shuffled order: reordering keeps every triangle, its winding and the
	// batch's range, and the vertex shader runs far fewer times.
	const int n = 40;
	BakedChunk c;
	for (int y = 0; y <= n; ++y)
		for (int x = 0; x <= n; ++x) {
			WorldVertex v {};
			v.x = i16(x);
			v.y = i16(y);
			c.vertices.push_back(v);
		}
	std::vector<std::array<u16, 3>> tris;
	for (int y = 0; y < n; ++y)
		for (int x = 0; x < n; ++x) {
			const u16 a = u16(y * (n + 1) + x), b = u16(a + 1), d = u16(a + n + 1), e = u16(d + 1);
			tris.push_back({a, d, b});
			tris.push_back({b, d, e});
		}
	u32 seed = 7u;
	for (size_t i = tris.size(); i > 1; --i) {
		seed = seed * 1664525u + 1013904223u;
		std::swap(tris[i - 1], tris[(seed >> 8) % i]);
	}
	// Two batches: the first 1000 triangles and the rest.
	for (const auto& t : tris) c.indices.insert(c.indices.end(), t.begin(), t.end());
	ChunkBatch b1 {}, b2 {};
	b1.firstIndex = 0;
	b1.indexCount = 3000;
	b2.firstIndex = 3000;
	b2.indexCount = u32(c.indices.size()) - 3000;
	c.batches = {b1, b2};
	const std::vector<u16> before = c.indices;
	VertexCacheStats st;
	optimizeVertexCache(c, &st);
	CHECK(c.indices.size() == before.size());
	CHECK(c.batches[0].firstIndex == 0 && c.batches[0].indexCount == 3000 && c.batches[1].firstIndex == 3000);
	// The same triangles with the same corner order (a triangle may start at any corner? no: exactly as it was).
	auto triSet = [](const std::vector<u16>& v, size_t from, size_t to) {
		std::vector<std::array<u16, 3>> s;
		for (size_t i = from; i < to; i += 3) s.push_back({v[i], v[i + 1], v[i + 2]});
		std::sort(s.begin(), s.end());
		return s;
	};
	CHECK(triSet(before, 0, 3000) == triSet(c.indices, 0, 3000));
	CHECK(triSet(before, 3000, before.size()) == triSet(c.indices, 3000, c.indices.size()));
	std::printf("  vertex cache: %.2f -> %.2f shader runs per triangle\n", st.missesBefore / double(st.triangles),
	            st.missesAfter / double(st.triangles));
	CHECK(st.missesAfter < st.missesBefore * 0.5);
	CHECK(st.missesAfter / double(st.triangles) < 1.3);
}

static void testChunker() {
	SrcScene scene;
	scene.materials.resize(3);
	// A 20 x 20 grid of quads over 4 x 4 chunks, UVs far from the origin.
	// Height and colour depend on the lattice corner only, so quads share vertices.
	for (int z = 0; z < 20; ++z)
		for (int x = 0; x < 20; ++x) {
			const float x0 = -1000.0f + x * 200.0f, z0 = z * 200.0f;
			SrcVertex q[4] = {};
			const float px[4] = {x0, x0 + 200, x0 + 200, x0}, pz[4] = {z0, z0, z0 + 200, z0 + 200};
			for (int k = 0; k < 4; ++k) {
				const int cx = x + (k == 1 || k == 2), cz = z + (k >= 2);
				q[k].pos[0] = px[k];
				q[k].pos[1] = float((cx * 31 + cz * 17) % 97);
				q[k].pos[2] = pz[k];
				q[k].uv[0] = px[k] / 100.0f - 250.0f;
				q[k].uv[1] = pz[k] / 100.0f + 90.0f;
				q[k].rgba[0] = u8(cx * 12);
				q[k].rgba[1] = u8(cz * 12);
				q[k].rgba[2] = 77;
				q[k].rgba[3] = 255;
			}
			SrcTriangle a, b;
			a.v[0] = q[0]; a.v[1] = q[1]; a.v[2] = q[2];
			b.v[0] = q[0]; b.v[1] = q[2]; b.v[2] = q[3];
			a.material = b.material = u32((x + z) % 2);
			scene.triangles.push_back(a);
			scene.triangles.push_back(b);
		}
	// One degenerate triangle and one triangle of a dropped material.
	SrcTriangle deg = scene.triangles[0];
	deg.v[1] = deg.v[0];
	scene.triangles.push_back(deg);
	SrcTriangle dropped = scene.triangles[0];
	dropped.material = 2;
	scene.triangles.push_back(dropped);

	const std::vector<int> remap = {0, 1, -1};
	ChunkerOptions opt;
	opt.chunkSize = 1000.0f;
	ChunkerStats st;
	std::vector<BakedChunk> chunks = buildChunks(scene, remap, {0, 0}, opt, st);
	CHECK(chunks.size() == 16);
	CHECK(st.inputTriangles == 801 && st.outputTriangles == 800 && st.degenerate == 1);
	CHECK(st.uvClamped == 0);

	u64 tris = 0;
	float worst = 0, worstUv = 0;
	for (const BakedChunk& c : chunks) {
		CHECK(c.batches.size() == 2 && c.batches[0].material < c.batches[1].material);
		tris += c.indices.size() / 3;
		// Shared corners must have been merged: far fewer than 3 vertices per triangle.
		CHECK(c.vertices.size() < c.indices.size() * 2 / 3);
		for (u16 i : c.indices) CHECK(i < c.vertices.size());
		for (const WorldVertex& v : c.vertices) {
			const float x = c.header.origin[0] + float(v.x) * c.header.posScale;
			const float z = c.header.origin[2] + float(v.z) * c.header.posScale;
			// Every source vertex sits on the 200 cm lattice.
			worst = std::max(worst, std::fabs(x - std::round(x / 200.0f) * 200.0f));
			worst = std::max(worst, std::fabs(z - std::round(z / 200.0f) * 200.0f));
			// UVs are shifted by whole repeats only: the fraction must survive.
			const float u = float(v.u) * c.header.uvScale, expectU = x / 100.0f - 250.0f;
			worstUv = std::max(worstUv, std::fabs((u - expectU) - std::round(u - expectU)));
			CHECK(v.u >= 0 && v.v >= 0);
			CHECK(v.b == 77 && v.a == 255 && v.w == 1);
			CHECK(x >= c.bbMin[0] - 0.01f && x <= c.bbMax[0] + 0.01f);
		}
		// The serialised blob has the documented size and layout.
		const std::vector<u8> blob = c.serialize();
		CHECK(blob.size() == chunkBlobSize(u32(c.batches.size()), u32(c.vertices.size()), u32(c.indices.size())));
		ChunkHeader hd;
		std::memcpy(&hd, blob.data(), sizeof(hd));
		CHECK(hd.magic == CHUNK_MAGIC && hd.vertexCount == c.vertices.size() && hd.indexCount == c.indices.size());
	}
	CHECK(tris == 800);
	std::printf("  chunker: position error %.3f cm, uv error %.5f\n", double(worst), double(worstUv));
	CHECK(worst < 0.05f);
	CHECK(worstUv < 0.002f);

	// A tiny vertex limit forces cells to split without losing triangles.
	opt.maxVertices = 24;
	ChunkerStats st2;
	chunks = buildChunks(scene, remap, {0, 0}, opt, st2);
	CHECK(st2.outputTriangles == 800 && st2.splitChunks == 16 && chunks.size() > 16);
	for (const BakedChunk& c : chunks) CHECK(c.vertices.size() <= 24);

	// ZenGin-style UVs: every polygon shifted by its own whole number of
	// repeats. The chunker must still share the vertices, and may only ever
	// move a triangle by whole repeats.
	SrcScene plain, shifted;
	plain.materials.resize(1);
	shifted.materials.resize(1);
	std::mt19937 rng(11);
	for (int z = 0; z < 20; ++z)
		for (int x = 0; x < 20; ++x) {
			const float x0 = x * 200.0f, z0 = z * 200.0f;
			const float px[4] = {x0, x0 + 200, x0 + 200, x0}, pz[4] = {z0, z0, z0 + 200, z0 + 200};
			SrcVertex q[4] = {};
			for (int k = 0; k < 4; ++k) {
				q[k].pos[0] = px[k];
				q[k].pos[2] = pz[k];
				q[k].uv[0] = px[k] / 300.0f; // fractions of 1/3: no ties when rounding
				q[k].uv[1] = pz[k] / 300.0f;
				q[k].rgba[3] = 255;
			}
			for (int half = 0; half < 2; ++half) {
				SrcTriangle tr;
				tr.material = 0;
				tr.v[0] = q[0];
				tr.v[1] = q[half ? 2 : 1];
				tr.v[2] = q[half ? 3 : 2];
				plain.triangles.push_back(tr);
				const float du = float(int(rng() % 7) - 3), dv = float(int(rng() % 7) - 3);
				for (SrcVertex& v : tr.v) {
					v.uv[0] += du;
					v.uv[1] += dv;
				}
				shifted.triangles.push_back(tr);
			}
		}
	ChunkerOptions one;
	one.chunkSize = 100000.0f; // a single chunk
	ChunkerStats sp, ss;
	buildChunks(plain, {0}, {0}, one, sp);
	const std::vector<BakedChunk> merged = buildChunks(shifted, {0}, {0}, one, ss);
	std::printf("  uv seams: %llu vertices plain, %llu with per-polygon offsets\n",
	            static_cast<unsigned long long>(sp.vertices), static_cast<unsigned long long>(ss.vertices));
	CHECK(sp.vertices == 21 * 21);
	CHECK(ss.vertices == sp.vertices && ss.outputTriangles == 800 && ss.uvClamped == 0);
	CHECK(merged.size() == 1);
	for (const BakedChunk& c : merged)
		for (const WorldVertex& v : c.vertices) {
			const float x = c.header.origin[0] + float(v.x) * c.header.posScale;
			const float d = float(v.u) * c.header.uvScale - x / 300.0f;
			CHECK(std::fabs(d - std::round(d)) < 0.002f); // same texel, some whole repeats away
		}

	// Draw order and detail classes: batches come out sorted by order key
	// (opaque, cut-out, blended), then detail class, then material; each batch
	// knows its bounds.
	{
		SrcScene mix;
		mix.materials.resize(3);
		auto quadAt = [&](float x, u32 material, u8 detail) {
			SrcTriangle tr = plain.triangles[0], tr2 = plain.triangles[1];
			for (SrcTriangle* q : {&tr, &tr2}) {
				for (SrcVertex& v : q->v) v.pos[0] += x;
				q->material = material;
				q->detail = detail;
				mix.triangles.push_back(*q);
			}
		};
		quadAt(0, 0, DETAIL_SMALL);    // world material 2: blended
		quadAt(1000, 1, DETAIL_ALWAYS); // world material 0: opaque
		quadAt(2000, 1, DETAIL_SMALL);
		quadAt(3000, 2, DETAIL_MEDIUM); // world material 1: cut-out
		quadAt(4000, 1, DETAIL_ALWAYS);
		ChunkerStats sm;
		ChunkerOptions whole = one;
		whole.clusterSize = 0; // a batch per material, wherever its triangles are
		const std::vector<BakedChunk> mc = buildChunks(mix, {2, 0, 1}, {0, 1, 2}, whole, sm);
		CHECK(mc.size() == 1 && mc[0].batches.size() == 4);
		if (mc.size() == 1 && mc[0].batches.size() == 4) {
			const BakedChunk& c = mc[0];
			const u16 wantMaterial[4] = {0, 0, 1, 2};
			const u8 wantDetail[4] = {DETAIL_ALWAYS, DETAIL_SMALL, DETAIL_MEDIUM, DETAIL_SMALL};
			const float wantX[4][2] = {{1000, 4200}, {2000, 2200}, {3000, 3200}, {0, 200}};
			for (int b = 0; b < 4; ++b) {
				CHECK(c.batches[b].material == wantMaterial[b] && c.batches[b].detail == wantDetail[b]);
				CHECK(c.batches[b].indexCount == (b == 0 ? 12u : 6u));
				const float x0 = c.header.origin[0] + float(c.batches[b].bbMin[0]) * c.header.posScale;
				const float x1 = c.header.origin[0] + float(c.batches[b].bbMax[0]) * c.header.posScale;
				CHECK(std::fabs(x0 - wantX[b][0]) < 0.3f && std::fabs(x1 - wantX[b][1]) < 0.3f);
				CHECK(c.batches[b].bbMin[1] <= c.batches[b].bbMax[1] && c.batches[b].bbMin[2] < c.batches[b].bbMax[2]);
			}
		}
		CHECK(sm.batches == 4 && sm.detailTriangles[DETAIL_ALWAYS] == 4 && sm.detailTriangles[DETAIL_MEDIUM] == 2 &&
		      sm.detailTriangles[DETAIL_SMALL] == 4);
		CHECK(sm.clusters == 0);
		for (const ChunkBatch& b : mc[0].batches) CHECK(b.cluster == 0);

		// Clusters: the chunk is cut into boxes, and a material gets a batch
		// in each box it has triangles in - one after the other in the index
		// buffer, so that the runtime can draw neighbours with one call.
		ChunkerStats sc;
		ChunkerOptions boxes = one;
		boxes.clusterSize = 1000.0f;
		const std::vector<BakedChunk> cc = buildChunks(mix, {2, 0, 1}, {0, 1, 2}, boxes, sc);
		CHECK(cc.size() == 1 && cc[0].batches.size() == 5 && sc.clusters == 5 && sc.maxChunkClusters == 5 && sc.coarsenedChunks == 0);
		if (cc.size() == 1 && cc[0].batches.size() == 5) {
			const BakedChunk& c = cc[0];
			// The opaque material's two quads are 30 m apart: two batches, in the order of their boxes.
			CHECK(c.batches[0].material == 0 && c.batches[1].material == 0 && c.batches[0].detail == DETAIL_ALWAYS &&
			      c.batches[1].detail == DETAIL_ALWAYS);
			CHECK(c.batches[0].cluster < c.batches[1].cluster && c.batches[0].indexCount == 6 && c.batches[1].indexCount == 6);
			CHECK(c.batches[1].firstIndex == c.batches[0].firstIndex + c.batches[0].indexCount);
			const float x0 = c.header.origin[0] + float(c.batches[0].bbMin[0]) * c.header.posScale;
			const float x1 = c.header.origin[0] + float(c.batches[1].bbMin[0]) * c.header.posScale;
			CHECK(std::fabs(x0 - 1000.0f) < 0.3f && std::fabs(x1 - 4000.0f) < 0.3f);
			// Every quad is in a box of its own; the boxes are numbered along the row.
			u32 seen = 0;
			for (const ChunkBatch& b : c.batches) seen |= 1u << b.cluster;
			CHECK(seen == 31);
			// The same triangles and vertices as without boxes.
			CHECK(c.indices.size() == mc[0].indices.size() && c.vertices.size() == mc[0].vertices.size());
		}
		// A byte names a box: a chunk that would have more gets bigger ones.
		SrcScene wide;
		wide.materials.resize(1);
		for (int z = 0; z < 20; ++z)
			for (int x = 0; x < 20; ++x)
				for (SrcTriangle tr : {plain.triangles[0], plain.triangles[1]}) {
					for (SrcVertex& v : tr.v) {
						v.pos[0] += float(x) * 300.0f;
						v.pos[2] += float(z) * 300.0f;
					}
					tr.material = 0;
					wide.triangles.push_back(tr);
				}
		ChunkerStats sw;
		ChunkerOptions tiny = one;
		tiny.chunkSize = 20000.0f;
		tiny.clusterSize = 250.0f;
		const std::vector<BakedChunk> wc = buildChunks(wide, {0}, {0}, tiny, sw);
		CHECK(wc.size() == 1 && sw.coarsenedChunks == 1 && sw.clusters > 16 && sw.clusters <= 255);
		CHECK(wc.size() == 1 && wc[0].batches.size() == sw.clusters && wc[0].indices.size() == 20 * 20 * 6);
	}

	// One lattice for the whole world: a vertex on the border of two chunks
	// lands on bit-identical world coordinates in both, whatever its position,
	// so there are no hairline cracks along chunk borders.
	{
		SrcScene seam;
		seam.materials.resize(1);
		std::mt19937 jitter(5);
		auto h = [&](int ix, int iz) { return float((ix * 7919 + iz * 104729) % 1000) * 0.137f - 40.0f; };
		for (int z = 0; z < 12; ++z)
			for (int x = 0; x < 12; ++x) {
				SrcTriangle tr;
				tr.material = 0;
				const int cx[4] = {x, x + 1, x + 1, x}, cz[4] = {z, z, z + 1, z + 1};
				SrcVertex q[4] = {};
				for (int k = 0; k < 4; ++k) {
					// Awkward coordinates: nothing lines up with the lattice by itself.
					q[k].pos[0] = float(cx[k]) * 333.33f - 1777.7f;
					q[k].pos[1] = h(cx[k], cz[k]);
					q[k].pos[2] = float(cz[k]) * 333.33f + 12345.6f;
					q[k].rgba[3] = 255;
				}
				tr.v[0] = q[0]; tr.v[1] = q[1]; tr.v[2] = q[2];
				seam.triangles.push_back(tr);
				tr.v[1] = q[2]; tr.v[2] = q[3];
				seam.triangles.push_back(tr);
			}
		(void)jitter;
		ChunkerOptions small;
		small.chunkSize = 1000.0f;
		ChunkerStats sq;
		const std::vector<BakedChunk> sc = buildChunks(seam, {0}, {0}, small, sq);
		CHECK(sc.size() > 9);
		// Collect every world position each source corner was written to.
		std::map<std::pair<int, int>, std::vector<std::array<float, 3>>> seen;
		for (const BakedChunk& c : sc)
			for (const WorldVertex& v : c.vertices) {
				const float wx = c.header.origin[0] + float(v.x) * c.header.posScale;
				const float wy = c.header.origin[1] + float(v.y) * c.header.posScale;
				const float wz = c.header.origin[2] + float(v.z) * c.header.posScale;
				const int ix = int(std::lround((wx + 1777.7f) / 333.33f)), iz = int(std::lround((wz - 12345.6f) / 333.33f));
				seen[{ix, iz}].push_back({wx, wy, wz});
				CHECK(std::fabs(wx - (float(ix) * 333.33f - 1777.7f)) <= 0.07f && std::fabs(wy - h(ix, iz)) <= 0.07f);
			}
		size_t shared = 0;
		for (const auto& kv : seen) {
			shared += kv.second.size() > 1;
			for (const auto& p : kv.second) CHECK(p == kv.second[0]);
		}
		CHECK(seen.size() == 13 * 13 && shared > 20); // many corners sit in two or more chunks
		CHECK(std::fabs(sc[0].header.posScale - 0.125f) < 1e-6f);
		CHECK(sq.stepChunks[0] == sc.size() && sq.tJunctions == 0 && sq.welded == 0);

		// A triangle too large for the base step gets a coarser one - in a part
		// of its own, so the rest of its cell stays fine. The corners it shares
		// with ordinary triangles move to the coarse lattice everywhere, so the
		// two parts still agree about them to the bit.
		SrcScene giant = seam;
		SrcTriangle g = seam.triangles[0];
		g.v[1] = g.v[2];
		g.v[2] = seam.triangles[1].v[2]; // one edge of the first quad ...
		g.v[0].pos[0] -= 30000.0f;       // ... and a corner 300 m away
		g.v[0].pos[2] = g.v[1].pos[2];
		giant.triangles.push_back(g);
		ChunkerStats sg;
		const std::vector<BakedChunk> gc = buildChunks(giant, {0}, {0}, small, sg);
		CHECK(gc.size() == sc.size() + 1 && sg.outputTriangles == sq.outputTriangles + 1);
		CHECK(sg.stepChunks[0] == sc.size() && sg.stepChunks[2] == 1);
		const BakedChunk* coarse = nullptr;
		for (const BakedChunk& c : gc)
			if (c.header.posScale > 0.2f) coarse = &c;
		CHECK(coarse && std::fabs(coarse->header.posScale - 0.5f) < 1e-6f && coarse->vertices.size() == 3);
		if (coarse) {
			int matched = 0;
			for (const WorldVertex& v : coarse->vertices) {
				const std::array<float, 3> p = {coarse->header.origin[0] + float(v.x) * coarse->header.posScale,
				                                coarse->header.origin[1] + float(v.y) * coarse->header.posScale,
				                                coarse->header.origin[2] + float(v.z) * coarse->header.posScale};
				for (const BakedChunk& c : gc) {
					if (&c == coarse) continue;
					for (const WorldVertex& o : c.vertices)
						matched += p == std::array<float, 3> {c.header.origin[0] + float(o.x) * c.header.posScale,
						                                      c.header.origin[1] + float(o.y) * c.header.posScale,
						                                      c.header.origin[2] + float(o.z) * c.header.posScale};
				}
			}
			CHECK(matched >= 2); // both shared corners found, bit for bit, in a fine chunk
		}
	}

	// A triangle spanning more texture repeats than 16.16 fixed point holds.
	SrcScene wide;
	wide.materials.resize(1);
	SrcTriangle t = scene.triangles[0];
	t.v[1].uv[0] = t.v[0].uv[0] + 100.0f;
	wide.triangles.push_back(t);
	ChunkerStats st3;
	buildChunks(wide, {0}, {0}, ChunkerOptions {}, st3);
	CHECK(st3.uvClamped == 1);
}

static void testMath() {
	const Mat4 proj = perspectiveLH(1.0f, 400.0f / 240.0f, 10.0f, 1000.0f);
	Vec4 n = transform(proj, {0, 0, 10, 1}), f = transform(proj, {0, 0, 1000, 1});
	CHECK(std::fabs(n.z / n.w) < 1e-5f && std::fabs(f.z / f.w - 1.0f) < 1e-5f);

	// Camera at the origin looking down +Z; +X is to the right, +Y is up.
	const Mat4 view = viewLH({0, 0, 0}, 0, 0);
	const Vec4 right = transform(mul(proj, view), {5, 0, 100, 1}), up = transform(mul(proj, view), {0, 5, 100, 1});
	CHECK(right.x > 0 && up.y > 0 && right.w > 0);

	const Frustum fr = frustumFromViewProj(mul(proj, view));
	const float in0[3] = {-10, -10, 90}, in1[3] = {10, 10, 110};
	const float behind0[3] = {-10, -10, -110}, behind1[3] = {10, 10, -90};
	const float side0[3] = {5000, -10, 90}, side1[3] = {5020, 10, 110};
	const float far0[3] = {-10, -10, 1500}, far1[3] = {10, 10, 1600};
	const float huge0[3] = {-9000, -9000, -9000}, huge1[3] = {9000, 9000, 9000};
	CHECK(aabbInFrustum(fr, in0, in1));
	CHECK(!aabbInFrustum(fr, behind0, behind1));
	CHECK(!aabbInFrustum(fr, side0, side1));
	CHECK(!aabbInFrustum(fr, far0, far1));
	CHECK(aabbInFrustum(fr, huge0, huge1));

	// Turning right (positive yaw) brings +X into view.
	const Vec3 fwd = forwardFromYawPitch(1.5708f, 0);
	CHECK(fwd.x > 0.99f && std::fabs(fwd.z) < 0.01f);
}

// Bakes one scene into a pak with the pipeline the command-line tool uses.
static void bakeScenePak(const SrcScene& scene, const std::string& path, const char* name, const ChunkerOptions& chunker) {
	std::vector<BakeWorld> worlds(1);
	worlds[0].name = name;
	worlds[0].scene = scene;
	BakeOptions opt;
	opt.chunker = chunker;
	opt.tex.maxSize = 32;
	opt.tex.quality = 0;
	opt.threads = 2;
	opt.quiet = true;
	PakWriter pak;
	bakeWorlds(worlds, opt, pak);
	pak.write(path);
}

static void bakeScenePak(const SrcScene& scene, const std::string& path, const char* name, float chunkSize) {
	ChunkerOptions chunker;
	chunker.chunkSize = chunkSize;
	bakeScenePak(scene, path, name, chunker);
}

// A wall the way level designers build them: neighbouring strips cut into
// different numbers of pieces, so corners of one strip rest on the edges of
// the next ("T-junctions"), and one strip whose corners are a hair off.
static void buildSeamWall(SrcScene& scene, bool magentaRoom) {
	scene.materials.resize(1);
	scene.materials[0].name = "wall";
	scene.materials[0].texture = "WALL";
	scene.loadTexture = [](const std::string&, SrcImage& out) {
		out.width = out.height = 8;
		out.rgba.assign(8 * 8 * 4, 255);
		for (size_t i = 0; i < out.rgba.size(); i += 4) {
			out.rgba[i] = 40;
			out.rgba[i + 1] = 160;
			out.rgba[i + 2] = 40;
		}
		return true;
	};
	if (magentaRoom) {
		scene.indoor = true; // no sky: whatever shows through a gap is exactly this colour
		scene.fogColor[0] = 255;
		scene.fogColor[1] = 0;
		scene.fogColor[2] = 255;
	}
	scene.hasSpawn = true;
	const float angle = 0.47f, ox = 1234.567f, oy = 33.33f, oz = -4321.09f;
	auto at = [&](float along, float up, float noise) {
		SrcVertex v {};
		v.pos[0] = ox + along * std::cos(angle) + noise;
		v.pos[1] = oy + up - noise;
		v.pos[2] = oz + along * std::sin(angle) + noise;
		v.uv[0] = along / 100.0f;
		v.uv[1] = -up / 100.0f;
		v.rgba[0] = v.rgba[1] = v.rgba[2] = 255;
		v.rgba[3] = 0;
		return v;
	};
	auto quad = [&](float s0, float s1, float h0, float h1, float noise) {
		SrcTriangle a, b;
		a.material = b.material = 0;
		a.v[0] = at(s0, h0, noise); a.v[1] = at(s1, h0, noise); a.v[2] = at(s1, h1, noise);
		b.v[0] = at(s0, h0, noise); b.v[1] = at(s1, h1, noise); b.v[2] = at(s0, h1, noise);
		scene.triangles.push_back(a);
		scene.triangles.push_back(b);
	};
	// Six columns, 2 m wide and 5 m tall, each cut into its own number of rows.
	const int rows[6] = {1, 3, 2, 5, 1, 4};
	for (int c = 0; c < 6; ++c)
		for (int r = 0; r < rows[c]; ++r)
			quad(float(c) * 200.0f, float(c + 1) * 200.0f, 500.0f * float(r) / float(rows[c]),
			     500.0f * float(r + 1) / float(rows[c]), 0.0f);
	// A band along the top, cut into seven and placed a hundredth of a
	// millimetre off: its corners are not the columns' corners.
	for (int c = 0; c < 7; ++c) quad(1200.0f * float(c) / 7.0f, 1200.0f * float(c + 1) / 7.0f, 500.0f, 600.0f, 0.001f);
}

// Total length of the triangle edges that have no partner running the other
// way through the same two points: the outline of the mesh plus every crack.
static double openEdgeLength(const std::vector<BakedChunk>& chunks) {
	using Point = std::array<long long, 3>;
	std::map<std::pair<Point, Point>, int> edges;
	for (const BakedChunk& c : chunks)
		for (size_t i = 0; i + 2 < c.indices.size(); i += 3) {
			Point p[3];
			for (int k = 0; k < 3; ++k) {
				const WorldVertex& v = c.vertices[c.indices[i + size_t(k)]];
				const i16 q[3] = {v.x, v.y, v.z};
				for (int a = 0; a < 3; ++a)
					p[k][size_t(a)] = std::llround((double(c.header.origin[a]) + double(q[a]) * double(c.header.posScale)) * 64.0);
			}
			for (int k = 0; k < 3; ++k) ++edges[{p[k], p[(k + 1) % 3]}];
		}
	double open = 0;
	for (const auto& kv : edges) {
		auto back = edges.find({kv.first.second, kv.first.first});
		const int unmatched = kv.second - (back == edges.end() ? 0 : back->second);
		if (unmatched <= 0) continue;
		double d2 = 0;
		for (size_t a = 0; a < 3; ++a) {
			const double d = double(kv.first.first[a] - kv.first.second[a]) / 64.0;
			d2 += d * d;
		}
		open += std::sqrt(d2) * unmatched;
	}
	return open;
}

static void bakeScenePak(const SrcScene& scene, const std::string& path, const char* name, const ChunkerOptions& chunker);

// How many pixels show the background through the wall, seen from 40 cm.
static int seamPixels(const std::string& path) {
	App app;
	if (!app.init(path.c_str(), "seam")) return -1;
	gfx::softSetCulling(false); // either side of the wall will do
	const float angle = 0.47f, ox = 1234.567f, oy = 33.33f, oz = -4321.09f;
	int holes = 0;
	for (int i = 0; i < 24; ++i) {
		const float along = 150.0f + 39.0f * float(i), up = 180.0f + float((i * 131) % 240);
		Camera& cam = app.camera();
		cam.pos = {ox + along * std::cos(angle) - 40.0f * std::sin(angle), oy + up,
		           oz + along * std::sin(angle) + 40.0f * std::cos(angle)};
		cam.yaw = std::atan2(std::sin(angle), -std::cos(angle)) + 0.3f * float(i % 3 - 1);
		cam.pitch = 0.25f * float(i % 5 - 2);
		app.world().preload(cam.pos);
		app.render();
		const u8* fb = gfx::softFramebuffer();
		for (int p = 0; p < gfx::SCREEN_W * gfx::SCREEN_H; ++p) {
			const bool hole = fb[p * 4] > 200 && fb[p * 4 + 1] < 60 && fb[p * 4 + 2] > 200;
			holes += hole;
			if (hole && getenv("SEAM_DEBUG")) std::printf("    view %d: hole at %d,%d\n", i, p % gfx::SCREEN_W, p / gfx::SCREEN_W);
		}
	}
	gfx::softSetCulling(true);
	app.shutdown();
	return holes;
}

static void testSeams() {
	SrcScene wall;
	buildSeamWall(wall, true);
	const double outline = 2.0 * (1200.0 + 600.0);

	// The old pipeline: half-centimetre lattice, vertices rounded one by one.
	ChunkerOptions before;
	before.chunkSize = 500.0f;
	before.positionStep = 0.5f;
	before.seal = false;
	ChunkerStats sb;
	const std::vector<BakedChunk> cracked = buildChunks(wall, {0}, {0}, before, sb);
	const double openBefore = openEdgeLength(cracked);

	ChunkerOptions now;
	now.chunkSize = 500.0f;
	ChunkerStats sn;
	const std::vector<BakedChunk> sealed = buildChunks(wall, {0}, {0}, now, sn);
	const double openNow = openEdgeLength(sealed);
	std::printf("  seams: open edges %.0f cm before, %.0f cm sealed (outline %.0f cm); %llu T-junctions, %llu welded, "
	            "+%llu triangles\n",
	            openBefore, openNow, outline, static_cast<unsigned long long>(sn.tJunctions),
	            static_cast<unsigned long long>(sn.welded), static_cast<unsigned long long>(sn.sealTriangles));
	CHECK(cracked.size() >= 4 && sealed.size() >= 4); // the wall spans several chunks
	CHECK(openBefore > outline + 3000.0);           // the test wall really has cracks ...
	CHECK(std::fabs(openNow - outline) < 2.0);      // ... and sealed, only its outline is open
	CHECK(sn.tJunctions > 15 && sn.sealTriangles > 15 && sn.degenerate == 0);
	CHECK(sn.outputTriangles == sn.inputTriangles + sn.sealTriangles);
	CHECK(sb.tJunctions == 0 && sb.sealTriangles == 0 && sb.outputTriangles == sb.inputTriangles);
	// Nothing moved by more than the tolerance allows, and the wall kept its area.
	auto area = [](const std::vector<BakedChunk>& chunks) {
		double sum = 0;
		for (const BakedChunk& c : chunks)
			for (size_t i = 0; i + 2 < c.indices.size(); i += 3) {
				double p[3][3];
				for (int k = 0; k < 3; ++k) {
					const WorldVertex& v = c.vertices[c.indices[i + size_t(k)]];
					p[k][0] = double(v.x) * c.header.posScale;
					p[k][1] = double(v.y) * c.header.posScale;
					p[k][2] = double(v.z) * c.header.posScale;
				}
				const double a[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
				const double b[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
				const double n[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
				sum += 0.5 * std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			}
		return sum;
	};
	CHECK(std::fabs(area(sealed) - 1200.0 * 600.0) < 1200.0 * 600.0 * 0.001);

	// A sliver triangle that only exists to fill a T-junction closes up
	// without taking its neighbours along.
	{
		SrcScene sliver;
		sliver.materials.resize(1);
		auto v = [](float x, float y) {
			SrcVertex s {};
			s.pos[0] = x;
			s.pos[1] = y;
			return s;
		};
		auto tri = [&](SrcVertex a, SrcVertex b, SrcVertex c) {
			SrcTriangle t;
			t.material = 0;
			t.v[0] = a; t.v[1] = b; t.v[2] = c;
			sliver.triangles.push_back(t);
		};
		tri(v(0, 0), v(200, 0), v(100, 300));     // the big one, edge (0,0)-(200,0) below
		tri(v(0, 0), v(100, -300), v(100, 0.0f)); // two below it, meeting in the middle of that edge
		tri(v(100, 0.0f), v(100, -300), v(200, 0));
		tri(v(0, 0), v(100, 0.0f), v(200, 0));    // zero area: the "fix" some exporters leave behind
		ChunkerStats ssl;
		ChunkerOptions one;
		one.chunkSize = 100000.0f;
		const std::vector<BakedChunk> sc = buildChunks(sliver, {0}, {0}, one, ssl);
		CHECK(ssl.inputTriangles == 4 && ssl.degenerate == 1 && ssl.outputTriangles == 4 && ssl.sealTriangles == 1);
		CHECK(std::fabs(openEdgeLength(sc) - (2.0 * std::sqrt(100.0 * 100.0 + 300.0 * 300.0) * 2.0)) < 1.0);
	}

	// Random walls: a rectangle cut again and again along random lines, so
	// that corners end up on edges everywhere, turned to an arbitrary angle,
	// every piece with float noise of its own. Sealed, each must be watertight
	// (only its outline open) without losing area.
	{
		std::mt19937 rng(2024);
		auto rnd = [&](float lo, float hi) { return lo + (hi - lo) * float(rng() % 100000) / 100000.0f; };
		double worstOpen = 0, worstArea = 0;
		u64 junctions = 0;
		for (int round = 0; round < 40; ++round) {
			struct Rect {
				float s0, h0, s1, h1;
			};
			const float width = rnd(300, 2500), height = rnd(200, 900);
			std::vector<Rect> rects = {{0, 0, width, height}};
			const int cuts = 3 + int(rng() % 40);
			for (int c = 0; c < cuts; ++c) {
				const size_t pick = rng() % rects.size();
				const Rect r = rects[pick];
				const bool vertical = (r.s1 - r.s0) > (r.h1 - r.h0) ? rng() % 4 != 0 : rng() % 4 == 0;
				const float span = vertical ? r.s1 - r.s0 : r.h1 - r.h0;
				if (span < 12.0f) continue;
				const float at = (vertical ? r.s0 : r.h0) + span * rnd(0.2f, 0.8f);
				rects[pick] = vertical ? Rect {r.s0, r.h0, at, r.h1} : Rect {r.s0, r.h0, r.s1, at};
				rects.push_back(vertical ? Rect {at, r.h0, r.s1, r.h1} : Rect {r.s0, at, r.s1, r.h1});
			}
			const float yaw = rnd(0, 6.28f), tilt = rnd(-0.4f, 0.4f);
			const float o[3] = {rnd(-90000, 90000), rnd(-3000, 3000), rnd(-90000, 90000)};
			SrcScene sc;
			sc.materials.resize(1);
			for (const Rect& r : rects) {
				const float n[3] = {rnd(-0.004f, 0.004f), rnd(-0.004f, 0.004f), rnd(-0.004f, 0.004f)};
				auto at = [&](float along, float up) {
					SrcVertex v {};
					// A tilted plane: `up` leans out of the vertical.
					v.pos[0] = o[0] + along * std::cos(yaw) + up * std::sin(tilt) * -std::sin(yaw) + n[0];
					v.pos[1] = o[1] + up * std::cos(tilt) + n[1];
					v.pos[2] = o[2] + along * std::sin(yaw) + up * std::sin(tilt) * std::cos(yaw) + n[2];
					v.uv[0] = along / 128.0f;
					v.uv[1] = up / 128.0f;
					v.rgba[0] = u8(rng());
					v.rgba[3] = 255;
					return v;
				};
				SrcTriangle a, b;
				a.material = b.material = 0;
				a.v[0] = at(r.s0, r.h0); a.v[1] = at(r.s1, r.h0); a.v[2] = at(r.s1, r.h1);
				b.v[0] = at(r.s0, r.h0); b.v[1] = at(r.s1, r.h1); b.v[2] = at(r.s0, r.h1);
				// Either diagonal.
				if (rng() % 2) {
					a.v[2] = at(r.s0, r.h1);
					b.v[0] = at(r.s1, r.h0);
				}
				sc.triangles.push_back(a);
				sc.triangles.push_back(b);
			}
			ChunkerOptions co;
			co.chunkSize = rnd(400, 4000);
			ChunkerStats st;
			const std::vector<BakedChunk> out = buildChunks(sc, {0}, {0}, co, st);
			junctions += st.tJunctions;
			worstOpen = std::max(worstOpen, std::fabs(openEdgeLength(out) - 2.0 * (double(width) + double(height))));
			worstArea = std::max(worstArea, std::fabs(area(out) / (double(width) * double(height)) - 1.0));
			CHECK(st.outputTriangles == st.inputTriangles + st.sealTriangles - st.degenerate);
			for (const BakedChunk& c : out) CHECK(std::fabs(c.header.posScale - 0.125f) < 1e-6f);
		}
		std::printf("  random walls: %llu T-junctions closed; outline off by <= %.2f cm, area by <= %.4f%%\n",
		            static_cast<unsigned long long>(junctions), worstOpen, worstArea * 100.0);
		CHECK(junctions > 500);
		CHECK(worstOpen < 3.0);    // rounding of the outline itself
		CHECK(worstArea < 0.002);
	}

	// The same wall on screen, from as close as the player gets to a wall.
	const std::string cracksPath = gDir + "/seam-before.pak", sealedPath = gDir + "/seam-sealed.pak";
	bakeScenePak(wall, cracksPath, "seam", before);
	bakeScenePak(wall, sealedPath, "seam", now);
	gfx::init();
	const int holesBefore = seamPixels(cracksPath), holesNow = seamPixels(sealedPath);
	gfx::shutdown();
	std::printf("  seams on screen (24 close-ups): %d background pixels before, %d sealed\n", holesBefore, holesNow);
	CHECK(holesBefore > 50);
	CHECK(holesNow == 0);
}

// Bakes a small synthetic world with the real pipeline and drives the runtime.
static void testRuntime() {
	SrcScene scene;
	SynthOptions so;
	so.cells = 48;
	so.huts = 8;
	so.trees = 60;
	buildSyntheticScene(so, scene);
	CHECK(scene.triangles.size() > 4000 && scene.hasSpawn);
	const std::string path = gDir + "/runtime.pak";
	bakeScenePak(scene, path, "t", 3000.0f);

	gfx::init();
	{
		App app;
		CHECK(!app.init(path.c_str(), "nope"));
		app.shutdown();
	}
	{
		App app;
		CHECK(app.init(path.c_str(), "t"));
		app.setDrawDistance(6000.0f);
		app.player().flying = true; // a fixed camera at the spawn point
		World& w = app.world();

		// Streaming is rate limited ...
		w.config().maxLoadsPerFrame = 1;
		app.update(Input {}, 1.0f / 30.0f);
		CHECK(w.stats().chunksLoaded == 1 && w.stats().chunksPending > 0);
		// ... and always takes the nearest missing chunks first.
		{
			const Vec3 c = app.camera().pos;
			auto distance = [&](u32 i) {
				const ChunkInfo& ci = w.chunkInfo(i);
				const float dx = std::max({ci.bbMin[0] - c.x, c.x - ci.bbMax[0], 0.0f});
				const float dy = std::max({ci.bbMin[1] - c.y, c.y - ci.bbMax[1], 0.0f});
				const float dz = std::max({ci.bbMin[2] - c.z, c.z - ci.bbMax[2], 0.0f});
				return std::sqrt(dx * dx + dy * dy + dz * dz);
			};
			// All but two of the missing chunks in one go: the two left out
			// must be the farthest ones.
			const u32 pendingBefore = w.stats().chunksPending;
			CHECK(pendingBefore > 6);
			w.config().maxLoadsPerFrame = pendingBefore - 2;
			app.update(Input {}, 1.0f / 30.0f);
			CHECK(w.stats().chunksLoaded == pendingBefore - 1 && w.stats().chunksPending == 2);
			float farthestLoaded = 0, nearestMissing = 1e30f;
			for (u32 i = 0; i < w.stats().chunksTotal; ++i) {
				if (w.chunkResident(i))
					farthestLoaded = std::max(farthestLoaded, distance(i));
				else
					nearestMissing = std::min(nearestMissing, distance(i));
			}
			CHECK(farthestLoaded <= nearestMissing);
		}
		// ... and a preload fetches everything in range at once.
		w.preload(app.camera().pos);
		const u32 near = w.stats().chunksLoaded;
		CHECK(near > 4 && near < w.stats().chunksTotal && w.stats().chunksPending == 0);
		CHECK(w.stats().loadFailures == 0);
		CHECK(w.stats().residentBytes == gfx::memStats().meshBytes + gfx::memStats().textureBytes);
		CHECK(w.stats().collisionBytes > 0);

		app.render();
		CHECK(w.stats().trianglesDrawn > 500 && w.stats().chunksDrawn > 0);
		gfx::SoftStats ss = gfx::softStats();
		CHECK(ss.pixelsWritten > u32(gfx::SCREEN_W * gfx::SCREEN_H / 4));
		// Looking at the camp from the gate: mostly front faces.
		CHECK(ss.trianglesBackface < (ss.trianglesSubmitted - ss.trianglesClipped) / 2);
		// The frame is not a flat fog-coloured rectangle.
		const u8* fb = gfx::softFramebuffer();
		int distinct = 0;
		for (int i = 0; i < gfx::SCREEN_W * gfx::SCREEN_H; i += 97)
			distinct += std::abs(int(fb[i * 4 + 1]) - int(app.skyColors().horizon[1])) > 24;
		CHECK(distinct > 200);

		const u32 skyBytes = gfx::memStats().meshBytes + gfx::memStats().textureBytes - w.stats().residentBytes; // the dome

		// Far away from everything: all chunks are released again.
		app.teleport({1e6f, 0, 1e6f});
		app.update(Input {}, 1.0f / 30.0f);
		CHECK(w.stats().chunksLoaded == 0 && w.stats().residentBytes == 0 && w.stats().texturesLoaded == 0);
		CHECK(w.stats().collisionBytes == 0);
		CHECK(gfx::memStats().meshCount >= 1 && gfx::memStats().meshCount <= 8 && gfx::memStats().textureCount == 0); // only the sky (dome and its quads) is left

		// The allocator runs dry before the budget does (a full linear heap on
		// the console): chunks and textures wait instead of being written off,
		// and arrive once there is room.
		app.teleport({scene.spawnPos[0], scene.spawnPos[1], scene.spawnPos[2]});
		gfx::softSetMemoryLimit(gfx::memStats().meshBytes + 40 * 1024);
		w.preload(app.camera().pos);
		const u32 starved = w.stats().chunksLoaded;
		CHECK(starved > 0 && starved < near && w.stats().allocRejects > 0 && w.stats().loadFailures == 0);
		CHECK(w.stats().chunksPending == near - starved);
		app.render(); // whatever is there can be drawn, textured or not
		gfx::softSetMemoryLimit(0);
		w.preload(app.camera().pos);
		for (int i = 0; i < 40; ++i) app.update(Input {}, 1.0f / 30.0f); // one waiting texture per frame
		CHECK(w.stats().chunksLoaded == near && w.stats().loadFailures == 0 && w.stats().chunksPending == 0);
		CHECK(w.stats().residentBytes == gfx::memStats().meshBytes + gfx::memStats().textureBytes - skyBytes);
		app.render();
		CHECK(w.stats().trianglesDrawn > 500);
		app.teleport({1e6f, 0, 1e6f});
		app.update(Input {}, 1.0f / 30.0f);
		CHECK(w.stats().chunksLoaded == 0 && w.stats().texturesLoaded == 0 && w.stats().residentBytes == 0);
		w.config() = StreamConfig {};
		app.setDrawDistance(6000.0f);

		// A budget too small for the neighbourhood: loading stops, nothing breaks.
		app.teleport({scene.spawnPos[0], scene.spawnPos[1], scene.spawnPos[2]});
		w.config().memoryBudget = 12 * 1024;
		w.preload(app.camera().pos);
		CHECK(w.stats().budgetRejects > 0 && w.stats().chunksLoaded > 0 && w.stats().chunksLoaded < near);
		app.render();
		CHECK(w.stats().trianglesDrawn > 0);
		app.shutdown();
		CHECK(gfx::memStats().meshCount == 0 && gfx::memStats().textureCount == 0 && gfx::memStats().meshBytes == 0);
	}

	// Corrupt one chunk: the runtime reports a failure instead of crashing,
	// and does not retry the broken chunk every frame.
	{
		FILE* f = fopen(path.c_str(), "rb");
		fseek(f, 0, SEEK_END);
		std::vector<u8> bytes(size_t(ftell(f)));
		fseek(f, 0, SEEK_SET);
		CHECK(fread(bytes.data(), 1, bytes.size(), f) == bytes.size());
		fclose(f);
		Pak p;
		p.open(path.c_str());
		u32 corrupted = 0;
		for (u32 i = 0; i < p.count(); ++i) {
			const PakEntry& e = p.entry(i);
			if (e.type != ASSET_CHUNK) continue;
			// Damage the start of every chunk payload (the header / first LZ4 token).
			for (u32 k = 0; k < 4 && k < e.packedSize; ++k) bytes[e.offset + k] ^= 0xA5;
			++corrupted;
		}
		p.close();
		const std::string bad = gDir + "/corrupt.pak";
		f = fopen(bad.c_str(), "wb");
		fwrite(bytes.data(), 1, bytes.size(), f);
		fclose(f);

		App app;
		CHECK(app.init(bad.c_str(), "t"));
		app.world().preload(app.camera().pos);
		const u32 failures = app.world().stats().loadFailures;
		CHECK(corrupted > 0 && failures > 0);
		app.world().preload(app.camera().pos);
		CHECK(app.world().stats().loadFailures == failures); // no retry storm
		app.render();
		app.shutdown();
		CHECK(gfx::memStats().meshCount == 0 && gfx::memStats().textureCount == 0);
	}
	gfx::shutdown();
}

// Two worlds in one archive: portals, water (blending, animation, wading,
// swimming, the view from below), detail classes.
static void testWorlds() {
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
	{
		// The cave's ladder as a model would have it (turned half round: its own +z looks away from the climber),
		// with the place to stand at in front of it: the runtime climbs it by what the model says. (The life worlds
		// keep the bare box: "ladder steering" climbs that.)
		CHECK(worlds[1].scene.ladders.size() == 1);
		SrcLadder& l = worlds[1].scene.ladders[0];
		const float lo[3] = {-30.0f, 0.0f, -6.0f}, hi[3] = {30.0f, 320.0f, 6.0f};
		const float places[1][3] = {{0.0f, 90.0f, 40.0f}};
		const float col[3][3] = {{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}};
		const float at[3] = {-1200.0f, 0.0f, 3394.0f};
		ladderFromModel(l, lo, hi, places, 1, col, at);
		CHECK(l.hasInfo && l.side == 1 && std::fabs(l.normal[1] + 1.0f) < 1e-4f && std::fabs(l.halfWidth - 30.0f) < 1e-3f &&
		      std::fabs(l.head[1] - 320.0f) < 1e-3f && std::fabs(l.foot[2] - 3394.0f) < 1e-3f);
	}
	const float gate[3] = {worlds[0].scene.points["CAMP_GATE"].pos[0], worlds[0].scene.points["CAMP_GATE"].pos[1],
	                       worlds[0].scene.points["CAMP_GATE"].pos[2]};
	BakeOptions bo;
	bo.tex.maxSize = 32;
	bo.tex.quality = 0;
	bo.threads = 2;
	bo.quiet = true;
	bo.foamMinWidth = 0.0f; // (the synthetic lake is small: the foam's eight textures are counted below)
	PakWriter writer;
	const BakeResult result = bakeWorlds(worlds, bo, writer);
	const std::string path = gDir + "/worlds.pak";
	writer.write(path);
	// Textures are shared: both worlds use the same water, rock and dirt (and the foam's eight frames, fix140).
	CHECK(result.textures == 21 && result.report.find("== world 'cave'") != std::string::npos);
	CHECK(result.report.find("portals: 1 of 1 lead to a baked world: cave at CAVE_ENTRY") != std::string::npos);
	CHECK(result.report.find("portals: 1 of 1 lead to a baked world: world at CAMP_GATE") != std::string::npos);
	CHECK(result.report.find("1 animated, 1 scrolling") != std::string::npos);

	const float dt = 1.0f / 30.0f;
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr)); // the archive names its start world
	CHECK(std::string(app.worldName()) == "world/world");
	World& w = app.world();
	Player& pl = app.player();
	CHECK(w.header().portalCount == 1 && w.header().textureCount == 22);
	w.preload(app.camera().pos);

	// ---- detail classes: small objects are only drawn near the camera ----
	app.player().flying = true;
	app.teleport({0, 600, 0}); // above the middle of the camp, barrels all around
	app.camera().pitch = -0.6f;
	app.update(Input {}, dt);
	w.preload(app.camera().pos);
	app.render();
	const u32 trisNear = w.stats().trianglesDrawn, culledNear = w.stats().batchesCulled;
	w.config().detailDistance[DETAIL_SMALL] = 0.0f;
	w.config().detailDistance[DETAIL_MEDIUM] = 0.0f;
	app.render();
	std::printf("  detail classes: %u triangles with objects, %u without them\n", trisNear, w.stats().trianglesDrawn);
	CHECK(w.stats().trianglesDrawn < trisNear && w.stats().batchesCulled > culledNear);
	w.config() = StreamConfig {};
	app.setDrawDistance(12000.0f);

	// ---- clusters: of a chunk in view only what is in view is drawn ----
	// What is left out is out of the picture or wholly in the fog, so with
	// the fog's end far beyond everything the picture is the same, pixel for
	// pixel, as with whole chunks drawn - from wherever one looks.
	{
		app.setDrawDistance(60000.0f);
		app.environment().fogStart = 59000.0f;
		u64 trisOn = 0, trisOff = 0, drawsOn = 0, drawsOff = 0, inChunks = 0, merged = 0, clustersOut = 0;
		int views = 0, differing = 0;
		for (int k = 0; k < 24; ++k) {
			const float angle = float(k) * 1.7f;
			app.teleport({std::sin(angle) * float(200 + k * 150), 260.0f + float(k % 5) * 350.0f, std::cos(angle) * float(200 + k * 150)});
			app.camera().yaw = angle * 2.3f;
			app.camera().pitch = (k % 4 == 0) ? -1.1f : (k % 4 == 1) ? 0.3f : -0.25f;
			app.update(Input {}, dt);
			w.preload(app.camera().pos);
			w.config().clusterCulling = false;
			app.render();
			const std::vector<u8> whole(gfx::softFramebuffer(), gfx::softFramebuffer() + gfx::SCREEN_W * gfx::SCREEN_H * 4);
			trisOff += w.stats().trianglesDrawn;
			drawsOff += w.stats().drawCalls;
			CHECK(w.stats().clustersCulled == 0);
			w.config().clusterCulling = true;
			app.render();
			trisOn += w.stats().trianglesDrawn;
			drawsOn += w.stats().drawCalls;
			inChunks += w.stats().trianglesInChunks;
			merged += w.stats().batchesMerged;
			clustersOut += w.stats().clustersCulled;
			CHECK(w.stats().trianglesDrawn <= w.stats().trianglesInChunks);
			differing += std::memcmp(whole.data(), gfx::softFramebuffer(), whole.size()) != 0;
			++views;
		}
		std::printf("  clusters: %d views, %llu triangles drawn of %llu with whole chunks (%llu in the chunks), %llu draw calls "
		            "against %llu, %llu batches merged, %llu clusters out of view\n",
		            views, static_cast<unsigned long long>(trisOn), static_cast<unsigned long long>(trisOff),
		            static_cast<unsigned long long>(inChunks), static_cast<unsigned long long>(drawsOn),
		            static_cast<unsigned long long>(drawsOff), static_cast<unsigned long long>(merged),
		            static_cast<unsigned long long>(clustersOut));
		CHECK(differing == 0);
		CHECK(trisOn * 10 < trisOff * 8); // a fifth saved at the very least
		CHECK(clustersOut > 0 && merged > 0);
		// Neighbours in view go out in one call: not many more calls than with whole chunks.
		CHECK(drawsOn < drawsOff * 13 / 10);

		// The fog's end is the far plane: with a short draw distance less is
		// drawn still, and nothing that is not in the fog all over goes
		// missing - where the two pictures differ, the one with whole chunks
		// shows the fog's colour (a silhouette in front of the sky).
		app.setDrawDistance(5000.0f);
		app.teleport({0, 400.0f, 0});
		app.camera().yaw = 0.4f;
		app.camera().pitch = -0.1f;
		app.update(Input {}, dt);
		w.preload(app.camera().pos);
		w.config().clusterCulling = false;
		app.render();
		const std::vector<u8> whole(gfx::softFramebuffer(), gfx::softFramebuffer() + gfx::SCREEN_W * gfx::SCREEN_H * 4);
		const u32 trisWhole = w.stats().trianglesDrawn;
		w.config().clusterCulling = true;
		app.render();
		CHECK(w.stats().trianglesDrawn < trisWhole);
		const SkyColors sky = app.skyColors();
		int changed = 0, notFog = 0;
		for (int i = 0; i < gfx::SCREEN_W * gfx::SCREEN_H; ++i) {
			if (std::memcmp(&whole[size_t(i) * 4], gfx::softFramebuffer() + i * 4, 3) == 0) continue;
			++changed;
			for (int c = 0; c < 3; ++c) notFog += std::abs(int(whole[size_t(i) * 4 + size_t(c)]) - int(sky.horizon[c])) > 2;
		}
		std::printf("  clusters: at 50 m %u triangles instead of %u; %d pixels of fog-coloured silhouette gone\n",
		            w.stats().trianglesDrawn, trisWhole, changed);
		CHECK(notFog == 0);
		w.config() = StreamConfig {};
		app.setDrawDistance(12000.0f);
		app.environment().fogStart = 12000.0f * 0.2f;
	}

	// ---- blended pass: the glow and the cobweb in the gate ----
	app.teleport({gate[0], gate[1] + 175.0f, gate[2]});
	app.camera().yaw = 3.14159f; // CAMP_GATE looks away from the gate; turn round
	app.camera().pitch = 0;
	app.update(Input {}, dt);
	w.preload(app.camera().pos);
	app.render();
	CHECK(w.stats().blendedDrawCalls >= 2 && w.stats().drawCalls > w.stats().blendedDrawCalls);
	// Additive light in the gate: brighter and warmer than anything the lit
	// world produces on its own (the cobweb's threads cover part of it).
	{
		const u8* fb = gfx::softFramebuffer();
		int glowing = 0;
		for (int i = 0; i < gfx::SCREEN_W * gfx::SCREEN_H; ++i) glowing += fb[i * 4] > 230 && fb[i * 4] > fb[i * 4 + 2] + 40;
		std::printf("  blended pass: %d glowing pixels\n", glowing);
		CHECK(glowing > 100 && glowing < 4000);
	}

	// ---- portal: walking through the gate leads into the cave ----
	pl.flying = false;
	app.teleport({gate[0], gate[1] + 175.0f, gate[2]});
	app.camera().yaw = 3.14159f;
	for (int i = 0; i < 20; ++i) app.update(Input {}, dt); // settle on the ground
	CHECK(pl.grounded && app.worldSwitches() == 0);
	int frames = 0;
	for (; frames < 120 && app.worldSwitches() == 0; ++frames) {
		Input in;
		in.moveZ = 1.0f;
		app.update(in, dt);
	}
	CHECK(app.worldSwitches() == 1 && frames > 15);
	CHECK(std::string(app.worldName()) == "world/cave");
	CHECK(std::fabs(app.camera().pos.x) < 1.0f && std::fabs(app.camera().pos.z - 900.0f) < 1.0f);
	CHECK(std::fabs(app.camera().yaw) < 1e-4f);
	CHECK(w.stats().chunksLoaded == 4 && w.stats().chunksTotal == 4); // preloaded on arrival
	for (int i = 0; i < 20; ++i) app.update(Input {}, dt);
	CHECK(pl.grounded && std::fabs(pl.feet.y) < 0.3f && app.worldSwitches() == 1); // no bounce back

	// ---- water: wade in, swim, come back out ----
	bool waded = false;
	for (int i = 0; i < 400 && pl.feet.z < 2400.0f; ++i) {
		Input in;
		in.moveZ = 1.0f;
		app.update(in, dt);
		waded = waded || (!pl.swimming && pl.grounded && pl.waterDepth > 60.0f);
	}
	CHECK(waded && pl.swimming && !pl.grounded);
	// Afloat: the feet hang 125 cm below the surface (at -20), the eyes are above it.
	CHECK(std::fabs(pl.feet.y - (-20.0f - 125.0f)) < 2.0f && app.camera().pos.y > -20.0f);
	const float swimStart = pl.feet.z;
	for (int i = 0; i < 30; ++i) {
		Input in;
		in.moveZ = 1.0f;
		in.fast = true; // no sprinting in water
		app.update(in, dt);
	}
	CHECK(pl.feet.z - swimStart < 300.0f && pl.feet.z > swimStart + 100.0f);
	// The far wall of the pool stops the swimmer.
	for (int i = 0; i < 200; ++i) {
		Input in;
		in.moveZ = 1.0f;
		app.update(in, dt);
	}
	CHECK(pl.swimming && pl.feet.z < 2750.0f && pl.feet.z > 2650.0f);
	// Looking back over the pool: the water is drawn in the blended pass, and
	// being animated and scrolling, the same view changes over time.
	app.camera().yaw = 3.14159f;
	app.camera().pitch = -0.35f;
	app.update(Input {}, dt);
	app.render();
	CHECK(!app.underwater() && w.stats().blendedDrawCalls > 0);
	const std::vector<u8> before(gfx::softFramebuffer(), gfx::softFramebuffer() + gfx::SCREEN_W * gfx::SCREEN_H * 4);
	app.update(Input {}, 0.3f); // more than one animation frame (4 per second)
	app.render();
	int changed = 0;
	for (size_t i = 0; i < before.size(); i += 4) changed += std::abs(int(before[i + 2]) - int(gfx::softFramebuffer()[i + 2])) > 3;
	std::printf("  water animation: %d pixels changed in 0.3 s\n", changed);
	CHECK(changed > 2000);
	app.camera().pitch = 0;

	// Back up the ramp.
	for (int i = 0; i < 400 && pl.feet.z > 1300.0f; ++i) {
		Input in;
		in.moveZ = 1.0f;
		app.update(in, dt);
	}
	CHECK(!pl.swimming && pl.grounded && std::fabs(pl.feet.y) < 0.5f && pl.waterDepth == 0.0f);

	// ---- under water the view is fogged in ----
	pl.flying = true;
	app.teleport({0, -200.0f, 2400.0f});
	app.update(Input {}, dt);
	app.render();
	CHECK(app.underwater());
	app.teleport({0, 300.0f, 2400.0f});
	app.update(Input {}, dt);
	app.render();
	CHECK(!app.underwater());
	float surface = 0;
	CHECK(w.waterBelow({0, 300, 2400}, 1000, surface) && std::fabs(surface + 20.0f) < 0.3f);
	CHECK(!w.waterBelow({0, 300, 1000}, 1000, surface)); // dry floor
	CHECK(!w.waterBelow({0, -100, 2400}, 1000, surface)); // from below the surface
	pl.flying = false;

	// ---- an indoor world has no sky and no time of day ----
	CHECK(w.indoor() && w.header().fogColor[0] == 14 && w.header().ladderCount == 1);
	CHECK(w.ladderInfo(0) && w.ladderInfo(0)->side == 1 && std::fabs(w.ladderInfo(0)->normal[1] + 1.0f) < 1e-4f);
	std::printf("  ladder from its model: %s\n", w.ladderInfo(0) ? "yes" : "no");
	app.teleport({0, 175.0f, 900.0f});
	app.camera().yaw = 0.4f;
	app.environment().timeOfDay = 13.0f;
	for (int i = 0; i < 20; ++i) app.update(Input {}, dt);
	app.render();
	const std::vector<u8> noon(gfx::softFramebuffer(), gfx::softFramebuffer() + gfx::SCREEN_W * gfx::SCREEN_H * 4);
	// A whole number of water animation cycles later (1 s; the scroll period is far longer), at night.
	app.environment().timeOfDay = 1.0f;
	app.render();
	int differing = 0, bright = 0;
	for (size_t i = 0; i < noon.size(); i += 4) {
		differing += std::abs(int(noon[i]) - int(gfx::softFramebuffer()[i])) > 2;
		bright += noon[i] > 40;
	}
	CHECK(differing == 0 && bright > 10000); // lit by its own light, at any hour
	app.environment().timeOfDay = 13.0f;

	// ---- ladder: walk into it to climb, step off at the top, back down again ----
	app.teleport({-1200.0f, 175.0f, 3000.0f});
	app.camera().yaw = 0;
	for (int i = 0; i < 20; ++i) app.update(Input {}, dt);
	CHECK(pl.grounded && !pl.climbing);
	bool climbed = false;
	float highest = 0;
	for (int i = 0; i < 300 && !(pl.grounded && pl.feet.y > 290.0f && pl.feet.z > 3450.0f); ++i) {
		Input in;
		in.moveZ = 1.0f;
		app.update(in, dt);
		climbed = climbed || pl.climbing;
		highest = std::max(highest, pl.feet.y);
	}
	std::printf("  ladder: up to %.0f cm, standing at %.0f %.0f %.0f\n", double(highest), double(pl.feet.x), double(pl.feet.y),
	            double(pl.feet.z));
	CHECK(climbed && !pl.climbing && pl.grounded && std::fabs(pl.feet.y - 300.0f) < 0.5f && pl.feet.z > 3450.0f);
	CHECK(highest < 345.0f); // no leap over the edge
	// Walking about on the ledge next to the ladder's top is just walking.
	for (int i = 0; i < 10; ++i) {
		Input in;
		in.moveX = 1.0f;
		app.update(in, dt);
		CHECK(!pl.climbing && pl.grounded);
	}
	// Down: back onto the ladder, facing the ledge.
	app.teleport({-1200.0f, 475.0f, 3460.0f});
	for (int i = 0; i < 20; ++i) app.update(Input {}, dt);
	CHECK(pl.grounded && std::fabs(pl.feet.y - 300.0f) < 0.5f);
	climbed = false;
	float fastestDrop = 0, lastY = pl.feet.y;
	for (int i = 0; i < 300 && !(pl.grounded && pl.feet.y < 1.0f); ++i) {
		Input in;
		in.moveZ = -1.0f;
		app.update(in, dt);
		climbed = climbed || pl.climbing;
		fastestDrop = std::max(fastestDrop, lastY - pl.feet.y);
		lastY = pl.feet.y;
	}
	CHECK(climbed && pl.grounded && std::fabs(pl.feet.y) < 0.5f && pl.feet.z < 3400.0f && pl.feet.z > 3330.0f);
	CHECK(fastestDrop < 8.0f); // climbed down at 170 cm/s (5.7 cm a frame), not fallen
	// Jumping lets go of a ladder.
	for (int i = 0; i < 40; ++i) {
		Input in;
		in.moveZ = 1.0f;
		app.update(in, dt);
	}
	CHECK(pl.climbing && pl.feet.y > 100.0f);
	Input letGo;
	letGo.jump = true;
	app.update(letGo, dt);
	CHECK(!pl.climbing);
	for (int i = 0; i < 40; ++i) app.update(Input {}, dt);
	CHECK(pl.grounded && std::fabs(pl.feet.y) < 0.5f);

	// ---- the way back ----
	app.teleport({0, 175.0f, 900.0f});
	app.camera().yaw = 3.14159f;
	for (int i = 0; i < 20; ++i) app.update(Input {}, dt);
	for (frames = 0; frames < 120 && app.worldSwitches() == 1; ++frames) {
		Input in;
		in.moveZ = 1.0f;
		app.update(in, dt);
	}
	CHECK(app.worldSwitches() == 2 && std::string(app.worldName()) == "world/world");
	CHECK(std::fabs(app.camera().pos.x - gate[0]) < 1.0f && std::fabs(app.camera().pos.z - gate[2]) < 1.0f);

	// ---- the saved state brings the player back to the same spot, across worlds ----
	{
		const std::string statePath = gDir + "/state.bin";
		app.environment().timeOfDay = 17.25f;
		app.setDrawDistance(9000.0f);
		app.camera().pitch = -0.3f;
		const Vec3 left = app.camera().pos;
		const float leftYaw = app.camera().yaw;
		CHECK(app.saveState(statePath.c_str()));
		// Leave: another world, another place, another hour.
		CHECK(app.enterWorld(hashName("world/cave"), nullptr, 0));
		app.environment().timeOfDay = 3.0f;
		app.setDrawDistance(12000.0f);
		const u32 switches = app.worldSwitches();
		CHECK(app.loadState(statePath.c_str()));
		CHECK(std::string(app.worldName()) == "world/world" && app.worldSwitches() == switches);
		CHECK(std::fabs(app.camera().pos.x - left.x) < 0.01f && std::fabs(app.camera().pos.y - left.y) < 0.01f &&
		      std::fabs(app.camera().pos.z - left.z) < 0.01f);
		CHECK(std::fabs(app.camera().yaw - leftYaw) < 1e-5f && std::fabs(app.camera().pitch + 0.3f) < 1e-5f);
		CHECK(std::fabs(app.environment().timeOfDay - 17.25f) < 1e-4f && std::fabs(app.drawDistance() - 9000.0f) < 0.01f);
		for (int i = 0; i < 20; ++i) app.update(Input {}, dt);
		CHECK(pl.grounded && std::string(app.worldName()) == "world/world");

		// A damaged or missing file changes nothing.
		FILE* f = fopen(statePath.c_str(), "r+b");
		fseek(f, 20, SEEK_SET);
		fputc(0x5A, f);
		fclose(f);
		const Vec3 here = app.camera().pos;
		CHECK(!app.loadState(statePath.c_str()));
		CHECK(!app.loadState((gDir + "/no-such-state.bin").c_str()));
		CHECK(app.camera().pos.x == here.x && app.camera().pos.z == here.z);
		app.setDrawDistance(12000.0f);
		app.environment().timeOfDay = 13.0f;
	}

	// ---- the debug key walks through the worlds of the archive; bad targets are refused ----
	const u32 switchesBefore = app.worldSwitches();
	Input next;
	next.nextWorld = true;
	app.update(next, dt);
	CHECK(app.worldSwitches() == switchesBefore + 1 && std::string(app.worldName()) == "world/cave");
	app.update(next, dt);
	CHECK(app.worldSwitches() == switchesBefore + 2 && std::string(app.worldName()) == "world/world");
	CHECK(!app.enterWorld(hashName("world/nowhere"), nullptr, 0));
	CHECK(!app.enterWorld(hashName("tex/syn_rock"), nullptr, 0)); // exists, but is not a world
	CHECK(std::string(app.worldName()) == "world/world" && app.worldSwitches() == switchesBefore + 2);
	app.render();
	CHECK(w.stats().trianglesDrawn > 100 && w.stats().loadFailures == 0);

	app.shutdown();
	CHECK(gfx::memStats().meshCount == 0 && gfx::memStats().textureCount == 0);

	// ---- a world index that points outside its own tables is refused ----
	{
		WorldHeader wh {};
		wh.magic = WORLD_MAGIC;
		wh.version = WORLD_VERSION;
		wh.chunkSize = 4000;
		wh.materialCount = 1;
		wh.textureCount = 1;
		WorldMaterial wm {};
		wm.texture = 0;
		wm.frameCount = 2; // two frames, but the table has one texture
		const u32 tex = hashName("tex/none");
		std::vector<u8> blob(sizeof(wh) + sizeof(wm) + sizeof(tex));
		std::memcpy(blob.data(), &wh, sizeof(wh));
		std::memcpy(blob.data() + sizeof(wh), &wm, sizeof(wm));
		std::memcpy(blob.data() + sizeof(wh) + sizeof(wm), &tex, sizeof(tex));
		PakWriter bad;
		bad.add("world/bad", ASSET_WORLD, blob);
		wm.frameCount = 1;
		std::memcpy(blob.data() + sizeof(wh), &wm, sizeof(wm));
		bad.add("world/empty", ASSET_WORLD, blob);
		blob.pop_back();
		bad.add("world/short", ASSET_WORLD, blob);
		const std::string badPath = gDir + "/badworld.pak";
		bad.write(badPath);
		Pak pak;
		CHECK(pak.open(badPath.c_str()));
		World world;
		CHECK(!world.open(&pak, "bad") && !world.isOpen());
		CHECK(!world.open(&pak, "short"));
		CHECK(world.open(&pak, "empty") && world.isOpen()); // valid, just without chunks
		world.update({0, 0, 0});
		world.draw(identity(), {0, 0, 0}, 0);
		world.drawBlended();
		world.close();
	}
	gfx::shutdown();
}

// A hand-made obstacle course: floor, wall, see-through wall, a low step, a
// tall block, a gentle ramp and a slope too steep to climb.
static void addQuad(SrcScene& scene, const float a[3], const float b[3], const float c[3], const float d[3], u32 material) {
	const float* corners[4] = {a, b, c, d};
	SrcVertex v[4] = {};
	for (int k = 0; k < 4; ++k) {
		std::memcpy(v[k].pos, corners[k], sizeof(float) * 3);
		v[k].uv[0] = corners[k][0] / 200.0f;
		v[k].uv[1] = corners[k][2] / 200.0f;
		v[k].rgba[0] = v[k].rgba[1] = v[k].rgba[2] = 200;
		v[k].rgba[3] = 255;
	}
	SrcTriangle t1, t2;
	t1.material = t2.material = material;
	t1.v[0] = v[0]; t1.v[1] = v[1]; t1.v[2] = v[2];
	t2.v[0] = v[0]; t2.v[1] = v[2]; t2.v[2] = v[3];
	scene.triangles.push_back(t1);
	scene.triangles.push_back(t2);
}

static void addBlock(SrcScene& scene, float x0, float x1, float z0, float z1, float top) {
	const float c[8][3] = {{x0, 0, z0}, {x1, 0, z0}, {x1, 0, z1}, {x0, 0, z1}, {x0, top, z0}, {x1, top, z0}, {x1, top, z1}, {x0, top, z1}};
	addQuad(scene, c[4], c[5], c[6], c[7], 0); // top
	addQuad(scene, c[0], c[1], c[5], c[4], 0);
	addQuad(scene, c[1], c[2], c[6], c[5], 0);
	addQuad(scene, c[2], c[3], c[7], c[6], 0);
	addQuad(scene, c[3], c[0], c[4], c[7], 0);
}

// ---- background loading ----

// The loader against the plain function it wraps: every chunk and texture of
// a baked world, through the queue, with and without a thread.
static void testLoader() {
	SrcScene scene;
	SynthOptions so;
	so.cells = 48;
	so.huts = 8;
	so.trees = 60;
	buildSyntheticScene(so, scene);
	const std::string path = gDir + "/loader.pak";
	bakeScenePak(scene, path, "t", 3000.0f);

	Pak pak;
	CHECK(pak.open(path.c_str()));
	CHECK(std::string(pak.path()) == path);
	const PakEntry* we = pak.find("world/t");
	CHECK(we != nullptr);
	if (!we) return;
	std::vector<u8> index(we->size);
	CHECK(pak.read(*we, index.data()));
	WorldHeader wh;
	std::memcpy(&wh, index.data(), sizeof(wh));
	std::vector<WorldMaterial> materials(wh.materialCount);
	std::memcpy(materials.data(), index.data() + sizeof(wh), materials.size() * sizeof(WorldMaterial));
	std::vector<u32> textures(wh.textureCount);
	std::memcpy(textures.data(), index.data() + sizeof(wh) + materials.size() * sizeof(WorldMaterial), textures.size() * 4);
	std::vector<ChunkInfo> chunks(wh.chunkCount);
	std::memcpy(chunks.data(), index.data() + worldBlobSize(wh) - chunks.size() * sizeof(ChunkInfo) - ((wh.flags & WORLDF_TIERS) ? wh.chunkCount : 0u),
	            chunks.size() * sizeof(ChunkInfo));

	std::vector<LoadJob> jobs;
	for (u32 i = 0; i < wh.chunkCount; ++i) {
		LoadJob j {};
		j.kind = LOAD_CHUNK;
		j.index = i;
		j.entry = *pak.find(chunks[i].entryHash);
		j.info = chunks[i];
		jobs.push_back(j);
	}
	for (u32 i = 0; i < wh.textureCount; ++i) {
		LoadJob j {};
		j.kind = LOAD_TEXTURE;
		j.index = i;
		j.entry = *pak.find(textures[i]);
		jobs.push_back(j);
	}
	CHECK(jobs.size() > 40);

	StreamLoader none;
	CHECK(!none.start((gDir + "/does-not-exist.pak").c_str(), materials.data(), wh.materialCount, true));
	CHECK(!none.running() && !none.submit(jobs[0]));

	for (int threaded = 0; threaded < 2; ++threaded) {
		StreamLoader loader;
		CHECK(loader.start(path.c_str(), materials.data(), wh.materialCount, threaded != 0));
		CHECK(loader.running() && loader.threaded() == (threaded != 0));
		size_t next = 0, done = 0, mismatches = 0, colliders = 0;
		u32 lastChunk = 0;
		bool inOrder = true;
		for (int spins = 0; done < jobs.size() && spins < 2000000; ++spins) {
			while (next < jobs.size() && loader.submit(jobs[next])) ++next;
			CHECK(loader.outstanding() <= StreamLoader::CAPACITY);
			LoadKind kind;
			if (!loader.peek(kind)) {
				if (threaded) usleep(50);
				continue;
			}
			LoadResult r;
			CHECK(loader.poll(r));
			CHECK(r.kind == kind && r.status == LOAD_OK && r.blob && r.size == jobs[done].entry.size);
			// Results come back in the order the jobs went in.
			inOrder = inOrder && r.index == jobs[done].index && r.kind == jobs[done].kind;
			LoadResult ref;
			StreamLoader::process(pak, jobs[done], materials.data(), wh.materialCount, ref);
			mismatches += ref.status != r.status || ref.size != r.size || std::memcmp(ref.blob, r.blob, r.size) != 0 ||
			              ref.collider.triangleCount != r.collider.triangleCount ||
			              ref.water.triangleCount != r.water.triangleCount || ref.collider.bytes() != r.collider.bytes();
			if (r.kind == LOAD_CHUNK) {
				colliders += r.collider.triangleCount > 0;
				lastChunk = r.index;
			} else {
				CHECK(r.collider.triangleCount == 0);
			}
			ref.discard();
			r.discard();
			CHECK(r.blob == nullptr);
			++done;
		}
		CHECK(done == jobs.size() && mismatches == 0 && inOrder && loader.outstanding() == 0);
		// (A few chunks hold nothing to stand on: only tree tops or water.)
		CHECK(colliders > wh.chunkCount / 2 && colliders <= wh.chunkCount && lastChunk == wh.chunkCount - 1);
		CHECK(loader.takeWorstJobMs() > 0.0f && loader.takeWorstJobMs() == 0.0f);

		// The queue is bounded ...
		u32 accepted = 0;
		for (const LoadJob& j : jobs) accepted += loader.submit(j);
		CHECK(accepted == StreamLoader::CAPACITY && loader.outstanding() == StreamLoader::CAPACITY);
		// ... and can be emptied in one go, whatever state the jobs are in.
		loader.cancelAll();
		LoadResult r;
		LoadKind kind;
		CHECK(loader.outstanding() == 0 && !loader.peek(kind) && !loader.poll(r));

		// Data that does not match what the world index promises is refused.
		LoadJob bad = jobs[0];
		bad.info.vertexCount = u16(bad.info.vertexCount + 1);
		LoadJob wrongType = jobs[0];
		wrongType.kind = LOAD_TEXTURE;
		LoadJob truncated = jobs[1];
		truncated.entry.packedSize /= 2;
		CHECK(loader.submit(bad) && loader.submit(wrongType) && loader.submit(truncated) && loader.submit(jobs[2]));
		int got = 0;
		for (int spins = 0; got < 4 && spins < 2000000; ++spins) {
			if (!loader.poll(r)) {
				if (threaded) usleep(50);
				continue;
			}
			CHECK(r.status == (got < 3 ? LOAD_BAD_DATA : LOAD_OK) && (got < 3 ? r.blob == nullptr : r.blob != nullptr));
			r.discard();
			++got;
		}
		CHECK(got == 4);

		// Stopping with work under way leaves nothing behind (the sanitizer
		// build checks that), and stopping twice is harmless.
		for (size_t i = 0; i < 6; ++i) loader.submit(jobs[i]);
		loader.stop();
		loader.stop();
		CHECK(!loader.running() && !loader.threaded() && !loader.submit(jobs[0]));
		// A stopped loader can be started again.
		CHECK(loader.start(path.c_str(), materials.data(), wh.materialCount, threaded != 0));
		CHECK(loader.submit(jobs[3]));
	} // destroyed with a job under way
}

// The world streamed in the background must end up exactly like the world
// loaded on the spot - same chunks, same memory, same picture - without ever
// doing more per frame than it is allowed to.

// A platform's free memory for the world (World::budget): a pool, less what meshes and textures hold.
static u32 gFakePool = 0;
static u32 fakeFree() {
	const gfx::MemStats m = gfx::memStats();
	const u32 used = m.meshBytes + m.textureBytes;
	return used < gFakePool ? gFakePool - used : 0u;
}

static void testStreaming() {
	const std::string path = gDir + "/loader.pak"; // baked by testLoader
	SrcScene scene;
	SynthOptions so;
	so.cells = 48;
	so.huts = 8;
	so.trees = 60;
	buildSyntheticScene(so, scene);
	const Vec3 spawn = {scene.spawnPos[0], scene.spawnPos[1], scene.spawnPos[2]};
	const float dt = 1.0f / 30.0f;

	struct Snapshot {
		std::vector<bool> resident;
		u32 bytes = 0, textures = 0, collision = 0;
		std::vector<u8> frame;
	};
	gfx::init();
	Snapshot shots[4];
	static const char* names[4] = {"on the frame", "background, no thread", "background, worker thread", "worker + read-ahead"};
	for (int mode = 0; mode < 4; ++mode) {
		const bool background = mode > 0, threaded = mode >= 2;
		App app;
		if (mode == 3) app.setReadAheadBudget(2u << 20);
		app.world().config().background = background;
		app.world().config().worker = threaded;
		CHECK(app.init(path.c_str(), "t"));
		World& w = app.world();
		CHECK(w.backgroundStreaming() == background && w.workerThread() == threaded);
		app.setDrawDistance(6000.0f);
		app.player().flying = true;
		w.config().maxLoadsPerFrame = 1;
		w.config().maxTextureLoadsPerFrame = 2;

		auto distance = [&](u32 i) {
			const ChunkInfo& ci = w.chunkInfo(i);
			const Vec3 c = app.camera().pos;
			const float dx = std::max({ci.bbMin[0] - c.x, c.x - ci.bbMax[0], 0.0f});
			const float dy = std::max({ci.bbMin[1] - c.y, c.y - ci.bbMax[1], 0.0f});
			const float dz = std::max({ci.bbMin[2] - c.z, c.z - ci.bbMax[2], 0.0f});
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		};
		// Updates until nothing is missing any more; false if that never happens.
		auto settle = [&](int limit, bool watch) {
			std::vector<bool> had(w.stats().chunksTotal);
			for (u32 i = 0; i < w.stats().chunksTotal; ++i) had[i] = w.chunkResident(i);
			float lastArrival = 0;
			bool ordered = true;
			u32 texturesBefore = w.stats().texturesLoaded;
			for (int f = 0; f < limit; ++f) {
				app.update(Input {}, dt);
				const WorldStats& st = w.stats();
				if (watch) {
					CHECK(st.loadsThisFrame <= 1 && st.chunksInFlight <= 2);
					CHECK(st.texturesLoaded <= texturesBefore + 2 || !background);
					texturesBefore = st.texturesLoaded;
					for (u32 i = 0; i < st.chunksTotal; ++i) {
						if (had[i] || !w.chunkResident(i)) continue;
						had[i] = true;
						ordered = ordered && distance(i) >= lastArrival;
						lastArrival = distance(i);
					}
				}
				if (st.chunksPending == 0 && st.chunksInFlight == 0 && st.texturesWaiting == 0) {
					if (watch) CHECK(ordered); // nearest first, all the way
					return true;
				}
				if (threaded) usleep(100);
			}
			return false;
		};

		// 1. Standing at the start point until everything around is there.
		CHECK(w.stats().chunksLoaded == 0);
		CHECK(settle(200000, true));
		Snapshot& shot = shots[mode];
		for (u32 i = 0; i < w.stats().chunksTotal; ++i) shot.resident.push_back(w.chunkResident(i));
		shot.bytes = w.stats().residentBytes;
		shot.textures = w.stats().texturesLoaded;
		shot.collision = w.stats().collisionBytes;
		app.setClock(5.0); // the water moves: same moment in every mode
		app.render();
		shot.frame.assign(gfx::softFramebuffer(), gfx::softFramebuffer() + gfx::SCREEN_W * gfx::SCREEN_H * 4);
		if (mode == 3) {
			// The chunks after those being read came off the card ahead of time, and the loader found them waiting.
			const ReadAheadStats ra = app.readAhead().stats();
			std::printf("  read-ahead: %u hits, %u misses, %u read, %u evicted\n", ra.hits, ra.misses, ra.loaded, ra.evicted);
			CHECK(app.readAhead().running() && ra.loaded > 0 && ra.hits > 0);
		}
		const u32 near = w.stats().chunksLoaded;
		CHECK(near > 4 && near < w.stats().chunksTotal && w.stats().loadFailures == 0 && w.stats().budgetRejects == 0);
		const u32 skyBytes = gfx::memStats().meshBytes + gfx::memStats().textureBytes - w.stats().residentBytes;
		std::printf("  streaming %-26s %u chunks, %u textures, %.2f MB\n", names[mode], near, shot.textures,
		            double(shot.bytes) / 1048576.0);

		// 2. Leaving while chunks are on their way: what arrives late is thrown away.
		app.teleport({1e6f, 0, 1e6f});
		app.update(Input {}, dt);
		CHECK(w.stats().chunksLoaded == 0);
		app.teleport(spawn);
		app.update(Input {}, dt); // asks for the nearest chunks ...
		if (background) CHECK(w.stats().chunksInFlight > 0);
		app.teleport({1e6f, 0, 1e6f}); // ... and is gone before they arrive
		CHECK(settle(200000, false));
		CHECK(w.stats().chunksLoaded == 0 && w.stats().residentBytes == 0 && w.stats().texturesLoaded == 0 &&
		      w.stats().collisionBytes == 0);
		CHECK(gfx::memStats().meshCount >= 1 && gfx::memStats().meshCount <= 8 && gfx::memStats().textureCount == 0); // the sky dome

		// 3. A loading screen in the middle of streaming: everything at once,
		// and nothing twice afterwards.
		app.teleport(spawn);
		app.update(Input {}, dt);
		app.update(Input {}, dt);
		w.preload(app.camera().pos);
		CHECK(w.stats().chunksLoaded == near && w.stats().chunksPending == 0 && w.stats().texturesWaiting == 0);
		CHECK(w.stats().residentBytes == shot.bytes && w.stats().texturesLoaded == shot.textures);
		for (int f = 0; f < 30; ++f) {
			app.update(Input {}, dt);
			if (threaded) usleep(100);
		}
		CHECK(w.stats().chunksLoaded == near && w.stats().chunksInFlight == 0 && w.stats().residentBytes == shot.bytes);
		CHECK(w.stats().residentBytes == gfx::memStats().meshBytes + gfx::memStats().textureBytes - skyBytes);
		CHECK(w.stats().collisionBytes == shot.collision);

		// 4. Video memory runs out while streaming: chunks wait, nothing is
		// written off, and they arrive once there is room.
		app.teleport({1e6f, 0, 1e6f});
		CHECK(settle(200000, false) && w.stats().chunksLoaded == 0);
		app.teleport(spawn);
		gfx::softSetMemoryLimit(gfx::memStats().meshBytes + 40 * 1024);
		for (int f = 0; f < 400; ++f) {
			app.update(Input {}, dt);
			if (threaded) usleep(100);
		}
		const u32 starved = w.stats().chunksLoaded;
		CHECK(starved > 0 && starved < near && w.stats().allocRejects > 0 && w.stats().loadFailures == 0);
		app.render(); // whatever is there can be drawn
		gfx::softSetMemoryLimit(0);
		CHECK(settle(200000, false));
		CHECK(w.stats().chunksLoaded == near && w.stats().loadFailures == 0 && w.stats().residentBytes == shot.bytes &&
		      w.stats().texturesLoaded == shot.textures);

		// 5. The budget is respected with chunks on their way, too.
		app.teleport({1e6f, 0, 1e6f});
		CHECK(settle(200000, false));
		w.config().memoryBudget = shot.bytes / 2;
		app.teleport(spawn);
		u32 peak = 0;
		for (int f = 0; f < 300; ++f) {
			app.update(Input {}, dt);
			peak = std::max(peak, w.stats().residentBytes);
			if (threaded) usleep(100);
		}
		// Chunks are admitted by their mesh size; their textures come on top.
		CHECK(w.stats().chunksLoaded > 0 && w.stats().chunksLoaded < near && w.stats().chunksPending > 0);
		CHECK(peak <= shot.bytes / 2 + 256 * 1024);
		// 5b. A budget that holds only part of the neighbourhood goes with the player: walking on, the chunks left
		// behind give their room to the ones ahead (before, whatever came first kept the memory for good).
		{
			const u32 held = w.stats().chunksLoaded;
			const u32 evicted = w.stats().evictions;
			std::vector<bool> heldSet(w.stats().chunksTotal);
			for (u32 i = 0; i < w.stats().chunksTotal; ++i) heldSet[i] = w.chunkResident(i);
			app.teleport({spawn.x + 4500.0f, spawn.y, spawn.z});
			u32 peak2 = 0;
			for (int f = 0; f < 300; ++f) {
				app.update(Input {}, dt);
				peak2 = std::max(peak2, w.stats().residentBytes);
				if (threaded) usleep(100);
			}
			std::printf("  budget walk (%s): %u chunks held, %u evicted, peak %.2f of %.2f MB\n", names[mode], held,
			            w.stats().evictions - evicted, double(peak2) / 1048576.0, double(shot.bytes / 2) / 1048576.0);
			// The player has moved on and chunks ahead have arrived, inside the budget. The background loader has to make room
			// for chunks on their way, so it must evict what was left behind; loading on the frame frees what is out of reach
			// before asking for the next chunk, and may never need to evict.
			u32 fresh = 0;
			for (u32 i = 0; i < w.stats().chunksTotal; ++i)
				if (!heldSet[i] && w.chunkResident(i)) ++fresh;
			CHECK(fresh > 0 && (!background || w.stats().evictions > evicted) && peak2 <= shot.bytes / 2 + 256 * 1024 &&
			      w.stats().loadFailures == 0);
		}
		w.config().memoryBudget = 24u << 20;
		// 5c. Where the platform tells how much memory is free (the 3DS's linear heap), the world takes no more
		// than that less a reserve, however big its own budget (StreamConfig::freeMemory).
		{
			app.teleport({1e6f, 0, 1e6f});
			CHECK(settle(200000, false));
			const gfx::MemStats base = gfx::memStats(); // (the characters' models stay)
			gFakePool = base.meshBytes + base.textureBytes + shot.bytes * 3 / 5 + 64 * 1024;
			w.config().freeMemory = fakeFree;
			w.config().memoryReserve = 64 * 1024;
			app.teleport(spawn);
			u32 peak3 = 0;
			for (int f = 0; f < 300; ++f) {
				app.update(Input {}, dt);
				peak3 = std::max(peak3, w.stats().residentBytes);
				if (threaded) usleep(100);
			}
			std::printf("  free memory (%s): %u chunks, peak %.2f MB, budget %.2f MB of %.2f\n", names[mode], w.stats().chunksLoaded,
			            double(peak3) / 1048576.0, double(w.stats().budgetBytes) / 1048576.0, double(shot.bytes) / 1048576.0);
			CHECK(w.stats().chunksLoaded > 0 && w.stats().chunksLoaded < near && peak3 <= shot.bytes * 3 / 5 + 256 * 1024);
			CHECK(w.stats().budgetBytes < (24u << 20) && w.stats().loadFailures == 0);
			w.config().freeMemory = nullptr;
			w.config().memoryReserve = StreamConfig().memoryReserve;
		}

		// 6. Closed with work under way.
		app.teleport({1e6f, 0, 1e6f});
		app.update(Input {}, dt);
		app.teleport(spawn);
		app.update(Input {}, dt);
		app.shutdown();
		CHECK(gfx::memStats().meshCount == 0 && gfx::memStats().textureCount == 0);
	}
	gfx::shutdown();
	for (int mode = 1; mode < 4; ++mode) {
		CHECK(shots[mode].resident == shots[0].resident);
		CHECK(shots[mode].bytes == shots[0].bytes && shots[mode].textures == shots[0].textures &&
		      shots[mode].collision == shots[0].collision);
		CHECK(shots[mode].frame == shots[0].frame); // the very same picture
	}
}

// Walking through a portal with background streaming on: the old world's
// loader is stopped, the new world gets one of its own.
static void testStreamingWorlds() {
	const std::string path = gDir + "/worlds.pak"; // baked by testWorlds
	gfx::init();
	for (int threaded = 0; threaded < 2; ++threaded) {
		App app;
		app.world().config().background = true;
		app.world().config().worker = threaded != 0;
		CHECK(app.init(path.c_str(), nullptr));
		World& w = app.world();
		w.preload(app.camera().pos);
		const u32 worldHash = w.entryHash();
		app.player().flying = true;
		for (int hop = 0; hop < 6; ++hop) {
			Input in;
			in.nextWorld = true; // the debug key: next world of the archive
			app.update(in, 1.0f / 30.0f);
			CHECK(w.isOpen() && w.backgroundStreaming() && w.workerThread() == (threaded != 0));
			CHECK((w.entryHash() == worldHash) == (hop % 2 == 1));
			CHECK(w.stats().chunksLoaded > 0 && w.stats().chunksPending == 0); // preloaded on arrival
			// Fly off so that the next switch happens with chunks under way.
			Input fly;
			fly.moveZ = 1.0f;
			fly.fast = true;
			for (int f = 0; f < 20; ++f) app.update(fly, 1.0f / 30.0f);
			app.render();
		}
		CHECK(app.worldSwitches() == 6 && w.stats().loadFailures == 0);
		app.shutdown();
		CHECK(gfx::memStats().meshCount == 0 && gfx::memStats().textureCount == 0);
	}
	gfx::shutdown();
}

// ---- frame pacing ----

// An open square the objects cannot save: the world's own distance shortens after them and returns before them.
static void testPacerDistance() {
	FramePacer p;
	p.cfg.freeRate = true;
	p.reset();
	for (int i = 0; i < 600; ++i) p.update(1.2f);
	CHECK(p.detailScale() <= p.cfg.minDetail + 0.01f);
	CHECK(p.distanceScale() < 0.9f);
	CHECK(p.distanceScale() >= p.cfg.minDistance - 0.001f);
	CHECK(p.interval() == 1);
	// Objects go before the distance does: with a load that the objects alone cure the distance stays whole.
	FramePacer q;
	q.reset();
	for (int i = 0; i < 300; ++i) q.update(0.95f);
	CHECK(q.distanceScale() == 1.0f);
	// Light again: the distance is back before any object.
	bool objectsBackFirst = false;
	for (int i = 0; i < 3000 && (p.distanceScale() < 1.0f || p.detailScale() < 1.0f); ++i) {
		p.update(0.5f);
		if (p.distanceScale() < 1.0f && p.detailScale() > p.cfg.minDetail + 0.01f) objectsBackFirst = true;
	}
	CHECK(!objectsBackFirst);
	CHECK(p.distanceScale() == 1.0f && p.detailScale() > 0.99f);
	// Without free rate: the settled 30 fps comes only once the distance is spent too, and gives it back.
	FramePacer h;
	h.reset();
	for (int i = 0; i < 2000 && h.interval() == 1; ++i) h.update(1.5f);
	CHECK(h.interval() == 2);
	for (int i = 0; i < 200; ++i) h.update(1.1f);
	CHECK(h.distanceScale() > 0.99f);
}

// ---- stereo 3D: where the eyes stand ----

static Vec3 viewPoint(const Mat4& v, Vec3 p) {
	return {v.at(0, 0) * p.x + v.at(0, 1) * p.y + v.at(0, 2) * p.z + v.at(0, 3), v.at(1, 0) * p.x + v.at(1, 1) * p.y + v.at(1, 2) * p.z + v.at(1, 3),
	        v.at(2, 0) * p.x + v.at(2, 1) * p.y + v.at(2, 2) * p.z + v.at(2, 3)};
}

static void testStereoEyes() {
	const Vec3 pos = {100.0f, 50.0f, 200.0f};
	for (int k = 0; k < 4; ++k) {
		const float yaw = 0.7f + 1.3f * float(k);
		StereoEye l, r;
		stereoEyes(pos, yaw, 1.0f, l, r);
		const Vec3 d = r.pos - l.pos;
		const Vec3 fwd = {sinf(yaw), 0.0f, cosf(yaw)};
		CHECK(std::fabs(length(d) - STEREO_MAX_SEPARATION) < 1e-3f);
		CHECK(std::fabs(dot(d, fwd)) < 1e-3f); // (across the way the view looks)
		CHECK(length((l.pos + r.pos) * 0.5f - pos) < 1e-3f && l.yaw > yaw && r.yaw < yaw);
		// What lies at the convergence distance straight ahead falls in the middle of both views.
		const Vec3 mark = pos + fwd * STEREO_CONVERGENCE;
		for (const StereoEye* e : {&l, &r}) {
			const Mat4 v = viewLH(e->pos, e->yaw, 0.0f);
			const Vec3 p = viewPoint(v, mark);
			CHECK(std::fabs(p.x) < 0.01f && p.z > 300.0f);
		}
		// Something nearer shows to the left eye more to the right (crossed), farther more to the left.
		const Vec3 nearMark = pos + fwd * 150.0f, farMark = pos + fwd * 3000.0f;
		const Mat4 vl = viewLH(l.pos, l.yaw, 0.0f), vr = viewLH(r.pos, r.yaw, 0.0f);
		CHECK(viewPoint(vl, nearMark).x / viewPoint(vl, nearMark).z > viewPoint(vr, nearMark).x / viewPoint(vr, nearMark).z);
		CHECK(viewPoint(vl, farMark).x / viewPoint(vl, farMark).z < viewPoint(vr, farMark).x / viewPoint(vr, farMark).z);
	}
	StereoEye l, r;
	stereoEyes(pos, 1.0f, 0.0f, l, r);
	CHECK(length(l.pos - pos) == 0.0f && length(r.pos - pos) == 0.0f && l.yaw == 1.0f && r.yaw == 1.0f);
	stereoEyes(pos, 1.0f, 5.0f, l, r); // (beyond the slider's top it is held there)
	CHECK(std::fabs(length(r.pos - l.pos) - STEREO_MAX_SEPARATION) < 1e-3f);
}

// A scene far over the time there is: the distance starts to shorten with the objects, and comes back quickly.
static void testPacerHeavy() {
	FramePacer p;
	p.cfg.freeRate = true;
	p.reset();
	int firstDist = -1;
	for (int f = 0; f < 40; ++f) {
		p.update(1.6f);
		if (firstDist < 0 && p.distanceScale() < 1.0f) firstDist = f;
	}
	std::printf("  pacer heavy: distance starts at frame %d (objects %.2f)\n", firstDist, double(p.detailScale()));
	CHECK(firstDist >= 0 && firstDist < 10);
	for (int f = 0; f < 600; ++f) p.update(1.6f);
	CHECK(p.distanceScale() <= p.cfg.minDistance + 0.01f);
	// Light again: the distance is whole after about 10 steps of 0.7 s, then the objects.
	int distBack = -1;
	for (int f = 0; f < 3000 && p.detailScale() < 1.0f; ++f) {
		p.update(0.5f);
		if (distBack < 0 && p.distanceScale() >= 1.0f) distBack = f;
	}
	std::printf("  pacer heavy: distance back after %d frames\n", distBack);
	CHECK(distBack > 0 && distBack < 600 && p.detailScale() > 0.99f);
	// A load over the line but not far: the objects go first and the distance is not touched while they are going.
	FramePacer q;
	for (int f = 0; f < 5; ++f) q.update(1.25f);
	CHECK(q.detailScale() > q.cfg.minDetail + 0.01f && q.distanceScale() == 1.0f);
}

static void testPacer() {
	// A stand-in for the console: objects are 40% of a frame's cost.
	auto cost = [](float scene, float detail) { return scene * (0.6f + 0.4f * detail); };

	// Plenty of room: nothing changes.
	FramePacer p;
	for (int f = 0; f < 600; ++f) p.update(cost(0.7f, p.detailScale()));
	CHECK(p.detailScale() == 1.0f && p.interval() == 1);
	{
		// The objects are given back only once the load has settled: a load
		// that keeps coming and going (walking, turning the camera) does not
		// bring them back and take them away again and again.
		FramePacer q;
		for (int f = 0; f < 40; ++f) q.update(0.98f); // too heavy: objects go
		const float cut = q.detailScale();
		for (int f = 0; f < 300; ++f) q.update(f % 60 < 40 ? 0.5f : 0.85f); // light, but not for 1.5 s on end
		const float unsettled = q.detailScale();
		for (int f = 0; f < 80; ++f) q.update(0.5f);
		const float early = q.detailScale();
		for (int f = 0; f < 120; ++f) q.update(0.5f);
		if (getenv("G3_TRACE")) std::printf("pacer: %f %f %f %f %d\n", double(cut), double(unsettled), double(early), double(q.detailScale()), q.interval());
		CHECK(q.interval() == 1 && cut < 0.9f && std::fabs(unsettled - cut) < 1e-6f && std::fabs(early - cut) < 1e-6f && q.detailScale() > cut + 0.05f);
		// ... a tenth at a time, two seconds apart, up to everything.
		FramePacer r;
		for (int f = 0; f < 60; ++f) r.update(0.95f); // (heavy, but not so heavy that it goes to 30 fps)
		CHECK(r.detailScale() <= r.cfg.minDetail + 0.001f && r.interval() == 1);
		int steps = 0, lastStep = -1000, closest = 1000;
		float before = r.detailScale(), biggest = 0;
		for (int f = 0; f < 1800; ++f) {
			r.update(0.5f);
			if (r.detailScale() != before) {
				++steps;
				biggest = std::max(biggest, r.detailScale() - before);
				closest = std::min(closest, f - lastStep);
				lastStep = f;
				before = r.detailScale();
			}
		}
		std::printf("  pacer gives back: %d steps, at most %.2f, at least %d frames apart, to %.2f\n", steps, double(biggest), closest, double(r.detailScale()));
		CHECK(r.detailScale() == 1.0f && steps == 7 && biggest < 0.101f && closest >= 120);
		// While the view turns fast nothing comes back, and the calm starts over after it.
		FramePacer t;
		for (int f = 0; f < 40; ++f) t.update(0.98f);
		const float held = t.detailScale();
		for (int f = 0; f < 300; ++f) t.update(0.5f, true);
		CHECK(t.detailScale() == held);
		for (int f = 0; f < 89; ++f) t.update(0.5f);
		CHECK(t.detailScale() == held);
		t.update(0.5f);
		CHECK(t.detailScale() > held + 0.05f);
	}

	{
		// The steady 30: a square that does not fit even without the small objects. The distance goes (the objects first),
		// settles where the frame fits (below halfHigh), and comes back once the scene is light.
		auto cost2 = [&](float scene, const FramePacer& q) { return scene * (0.6f + 0.4f * q.detailScale()) * (0.5f + 0.5f * q.distanceScale()); };
		FramePacer h30;
		h30.cfg.halfOnly = true;
		for (int f = 0; f < 1500; ++f) h30.update(cost2(2.8f, h30));
		const float dist = h30.distanceScale();
		CHECK(h30.interval() == 2 && h30.detailScale() <= h30.cfg.minDetail + 0.001f && dist < 0.95f && dist >= h30.cfg.minDistance);
		CHECK(cost2(2.8f, h30) < h30.cfg.halfHigh + 0.1f);
		for (int f = 0; f < 1500; ++f) h30.update(cost2(1.2f, h30));
		CHECK(h30.distanceScale() == 1.0f && h30.detailScale() == 1.0f);
		// A scene that fits changes nothing.
		FramePacer fit;
		fit.cfg.halfOnly = true;
		for (int f = 0; f < 600; ++f) fit.update(cost2(1.5f, fit));
		CHECK(fit.distanceScale() == 1.0f && fit.detailScale() == 1.0f);
	}

	// A view that needs 105% at full detail. The pacer finds a detail level
	// that fits and stays at 60 fps, without hunting.
	float worst = 0, lowest = 1, highest = 0;
	int changes = 0, late = 0;
	for (int f = 0; f < 1200; ++f) {
		const float load = cost(1.05f, p.detailScale());
		const int was = p.interval();
		p.update(load);
		changes += p.interval() != was;
		if (f >= 300) {
			worst = std::max(worst, load);
			late += load > 1.0f;
			lowest = std::min(lowest, p.detailScale());
			highest = std::max(highest, p.detailScale());
		}
	}
	std::printf("  pacer at 105%%: detail settles at %.0f..%.0f%%, load <= %.0f%%\n", double(lowest * 100.0f),
	            double(highest * 100.0f), double(worst * 100.0f));
	CHECK(changes == 0 && p.interval() == 1 && late == 0);
	CHECK(worst <= 0.92f && lowest > 0.40f && highest < 0.75f && highest - lowest < 0.15f);

	// The first frames of the overload are the only late ones.
	p.reset();
	late = 0;
	for (int f = 0; f < 300; ++f) {
		const float load = cost(1.05f, p.detailScale());
		late += load > 1.0f;
		p.update(load);
	}
	CHECK(late >= 1 && late <= 4);

	// A view that does not fit even without objects: one switch to a steady
	// 30 fps, not a flicker between the two - and at 30 fps, with twice the
	// time, the objects come back.
	p.reset();
	changes = 0;
	for (int f = 0; f < 900; ++f) {
		const int was = p.interval();
		p.update(cost(1.5f, p.detailScale()));
		changes += p.interval() != was;
	}
	CHECK(p.interval() == 2 && changes == 1 && p.detailScale() > 0.99f && !p.probing());
	// It is too heavy to even ask whether 60 fps would do: the objects stay.
	lowest = 1.0f;
	for (int f = 0; f < 3000; ++f) {
		p.update(cost(1.5f, p.detailScale()));
		lowest = std::min(lowest, p.detailScale());
	}
	CHECK(p.interval() == 2 && lowest > 0.99f);

	// The view gets lighter, but not light: 60 fps would fit with fewer
	// objects. The pacer finds out by taking them away for a moment, and
	// switches.
	changes = 0;
	int probes = 0;
	bool wasProbing = false;
	for (int f = 0; f < 600 && p.interval() == 2; ++f) {
		p.update(cost(1.2f, p.detailScale())); // 120% with everything, 89% with the minimum
		probes += p.probing() && !wasProbing;
		wasProbing = p.probing();
	}
	CHECK(p.interval() == 1 && probes == 1 && p.detailScale() < 0.5f);
	late = 0;
	for (int f = 0; f < 900; ++f) {
		const float load = cost(1.2f, p.detailScale());
		late += load > 1.0f;
		const int was = p.interval();
		p.update(load);
		changes += p.interval() != was;
	}
	CHECK(p.interval() == 1 && changes == 0 && late == 0); // and stays there

	// A view where the answer is no (96% even with the minimum): the objects
	// dip for a moment each time it asks, it asks less and less often, and it
	// never leaves 30 fps.
	// (Reached from a heavier view; coming from a light one it would simply
	// stay at 60.)
	p.reset();
	for (int f = 0; f < 300; ++f) p.update(cost(1.5f, p.detailScale()));
	CHECK(p.interval() == 2);
	probes = 0;
	wasProbing = false;
	int firstHalf = 0;
	bool stayed = true;
	for (int f = 0; f < 6000; ++f) {
		p.update(cost(1.297f, p.detailScale()));
		probes += p.probing() && !wasProbing;
		wasProbing = p.probing();
		if (f == 2999) firstHalf = probes;
		stayed = stayed && p.interval() == 2;
	}
	CHECK(stayed);
	std::printf("  pacer at 30 fps, borderline view: asked %d times in 200 s (%d of them in the first 100 s)\n", probes,
	            firstHalf);
	CHECK(probes >= 3 && probes <= 6 && firstHalf >= probes - firstHalf);

	// A clearly light view needs no check: straight back to 60, objects and all.
	changes = 0;
	for (int f = 0; f < 900; ++f) {
		const int was = p.interval();
		p.update(cost(0.75f, p.detailScale()));
		changes += p.interval() != was;
	}
	CHECK(p.interval() == 1 && changes == 1 && p.detailScale() > 0.95f);

	// Nonsense input does no harm.
	p.reset();
	p.update(std::nanf(""));
	p.update(-5.0f);
	p.update(1.0e9f);
	CHECK(p.detailScale() >= p.cfg.minDetail && p.detailScale() <= 1.0f && p.load() <= 4.0f);
}

// Small objects coming into their distance dissolve in, and out again when
// they leave it; what is drawn goes a little farther out than it comes in;
// and what comes into view by turning is there at once.
static void testDetailFade() {
	SrcScene scene;
	scene.materials.resize(2);
	scene.materials[0].name = "floor";
	scene.materials[0].hasColor = true;
	scene.materials[0].color[0] = 40;
	scene.materials[0].color[1] = 90;
	scene.materials[1].name = "crate";
	scene.materials[1].hasColor = true;
	scene.materials[1].color[0] = 250;
	scene.materials[1].color[2] = 250;
	const float f[4][3] = {{-2000, 0, -2000}, {2000, 0, -2000}, {2000, 0, 2000}, {-2000, 0, 2000}};
	addQuad(scene, f[3], f[2], f[1], f[0], 0); // (facing up)
	const size_t first = scene.triangles.size();
	// A crate of 2 m, 10 m ahead of x = 0, z = 0.
	const float c[8][3] = {{-100, 0, 1000}, {100, 0, 1000}, {100, 0, 1200}, {-100, 0, 1200},
	                       {-100, 200, 1000}, {100, 200, 1000}, {100, 200, 1200}, {-100, 200, 1200}};
	addQuad(scene, c[0], c[1], c[5], c[4], 1); // the side facing -Z
	addQuad(scene, c[1], c[0], c[4], c[5], 1); // (both ways round: whichever is the front)
	for (size_t t = first; t < scene.triangles.size(); ++t) scene.triangles[t].detail = DETAIL_SMALL;
	scene.spawnPos[1] = 150.0f;
	scene.hasSpawn = true;
	const std::string path = gDir + "/fade.pak";
	bakeScenePak(scene, path, "f", 8000.0f);

	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), "f"));
	World& w = app.world();
	StreamConfig& cfg = w.config();
	cfg.detailDistance[DETAIL_SMALL] = 1500.0f; // the crate is in at 15 m, out at 16.5 m
	cfg.detailScale = 1.0f;
	app.player().flying = true;
	app.camera().pitch = -0.1f;
	const float dt = 1.0f / 30.0f;
	auto at = [&](float z, float yaw) {
		app.camera().yaw = yaw;
		app.teleport({0, 150, z});
	};
	// One frame: the crate's share of the picture (its pixels), and whether it dissolves.
	auto frame = [&](u32& fading) {
		app.update(Input {}, dt);
		app.render();
		fading = w.stats().batchesFading;
		const u8* px = gfx::softFramebuffer();
		int crate = 0; // (magenta)
		for (int i = 0; i < gfx::SCREEN_W * gfx::SCREEN_H; ++i) crate += px[i * 4] > 120 && px[i * 4 + 2] > 120 && px[i * 4 + 1] < 80;
		return crate;
	};
	u32 fading = 0;
	at(1000.0f - 1700.0f, 0);
	w.preload(app.camera().pos);
	for (int i = 0; i < 5; ++i) {
		const int g = frame(fading);
		CHECK(g == 0 && fading == 0); // 17 m: not drawn
	}

	// 14 m: in, dissolving over 0.4 s - more of it every frame.
	at(1000.0f - 1400.0f, 0);
	int last = 0, frames = 0;
	bool growing = true;
	for (int i = 0; i < 30; ++i) {
		const int g = frame(fading);
		if (fading) {
			++frames;
			growing = growing && g >= last;
		}
		last = g;
	}
	const int full = last;
	std::printf("  dissolving in: %d frames at 30 fps, %d pixels of the crate in the end\n", frames, full);
	CHECK(full > 500 && frames >= 10 && frames <= 13 && growing && fading == 0);
	{
		// Halfway: about half of its pixels.
		at(1000.0f - 1700.0f, 0);
		for (int i = 0; i < 20; ++i) frame(fading);
		at(1000.0f - 1400.0f, 0);
		int half = 0;
		for (int i = 0; i < 6; ++i) half = frame(fading);
		std::printf("  six frames in: %d of %d pixels\n", half, full);
		CHECK(fading >= 1 && half > full / 4 && half < full * 3 / 4);
		for (int i = 0; i < 20; ++i) frame(fading);
	}

	// 16 m: past its distance, but drawn - it stays (10% farther out).
	at(1000.0f - 1600.0f, 0);
	int stays = 0;
	for (int i = 0; i < 20; ++i) stays += frame(fading) > 0 && fading == 0;
	CHECK(stays == 20);
	// 17 m: it goes, dissolving out.
	at(1000.0f - 1700.0f, 0);
	frames = 0;
	for (int i = 0; i < 30; ++i) {
		last = frame(fading);
		frames += fading != 0;
	}
	CHECK(last == 0 && frames >= 10 && frames <= 13);
	// Back at 16 m it does not come back: from outside, 15 m is its distance.
	at(1000.0f - 1600.0f, 0);
	for (int i = 0; i < 10; ++i) CHECK(frame(fading) == 0 && fading == 0);

	// Walking up to it looking the other way, then turning round: it is just there.
	at(1000.0f - 1700.0f, 3.14159f);
	for (int i = 0; i < 5; ++i) frame(fading);
	at(1000.0f - 1400.0f, 3.14159f);
	for (int i = 0; i < 5; ++i) CHECK(frame(fading) == 0 && fading == 0);
	app.camera().yaw = 0;
	CHECK(frame(fading) > full * 9 / 10 && fading == 0);

	// Without dissolving (fadeMs 0): in at once, out at once.
	cfg.fadeMs = 0.0f;
	at(1000.0f - 1700.0f, 0);
	CHECK(frame(fading) == 0);
	at(1000.0f - 1400.0f, 0);
	CHECK(frame(fading) > full * 9 / 10 && fading == 0);

	app.shutdown();
	gfx::shutdown();
}

// A tower: many floors over one footprint, so that every column of the
// collision grid holds a lot that a query is nowhere near. The queries must
// answer exactly as they would looking at every entry of their cells - and
// look at far fewer.
static void testCollisionLevels() {
	const int n = 24, floors = 12;
	std::vector<WorldVertex> verts;
	std::vector<u16> idx;
	u32 seed = 4711;
	auto rnd = [&]() {
		seed = seed * 1664525u + 1013904223u;
		return float(seed >> 8) / float(1 << 24);
	};
	for (int f = 0; f < floors; ++f)
		for (int z = 0; z <= n; ++z)
			for (int x = 0; x <= n; ++x) {
				WorldVertex v {};
				v.x = i16((x - n / 2) * 800);
				// Floors that are not quite level, and one that is a slope through the others.
				v.y = i16((f - floors / 2) * 2000 + int(rnd() * 300.0f) + (f == 3 ? x * 900 : 0));
				v.z = i16((z - n / 2) * 800);
				v.w = 1;
				v.r = u8(f * 20);
				v.g = v.b = v.a = 255;
				verts.push_back(v);
			}
	for (int f = 0; f < floors; ++f)
		for (int z = 0; z < n; ++z)
			for (int x = 0; x < n; ++x) {
				const u16 a = u16(f * (n + 1) * (n + 1) + z * (n + 1) + x), b = u16(a + 1), c = u16(a + n + 1), d = u16(c + 1);
				for (u16 i : {a, c, b, b, c, d}) idx.push_back(i);
			}
	ChunkHeader h {};
	h.magic = CHUNK_MAGIC;
	h.batchCount = 1;
	h.vertexCount = u16(verts.size());
	h.indexCount = u32(idx.size());
	h.posScale = 0.125f;
	h.origin[0] = 300.0f;
	h.origin[1] = -40.0f;
	h.origin[2] = 77.0f;
	ChunkBatch batch {};
	batch.indexCount = h.indexCount;
	WorldMaterial material {};
	material.frameCount = 1;
	ChunkCollider col;
	CHECK(col.build(h, &batch, verts.data(), idx.data(), &material, MATF_NO_COLLISION, 0));
	CHECK(col.triangleCount == u32(idx.size() / 3) && col.gridW > 4 && col.gridH > 4);

	const u32 triMask = ChunkCollider::MAX_TRIANGLES - 1;
	auto vertex = [&](u32 tri, int k) {
		const i16* p = col.positions + col.triangles[tri * 3 + u32(k)] * 3;
		return Vec3 {float(p[0]), float(p[1]), float(p[2])};
	};
	// The queries as they were before the levels: every entry of the cells.
	auto pushReference = [&](Vec3& center, float radius) {
		const float inv = 1.0f / col.scale;
		Vec3 c = {(center.x - col.origin[0]) * inv, (center.y - col.origin[1]) * inv, (center.z - col.origin[2]) * inv};
		const float r = radius * inv;
		const float gx = c.x - float(col.gridX0), gz = c.z - float(col.gridZ0);
		const float fx0 = (gx - r) / col.cellSize, fx1 = (gx + r) / col.cellSize;
		const float fz0 = (gz - r) / col.cellSize, fz1 = (gz + r) / col.cellSize;
		if (fx1 < 0 || fz1 < 0 || fx0 >= float(col.gridW) || fz0 >= float(col.gridH)) return false;
		const u32 x0 = u32(std::max(fx0, 0.0f)), x1 = std::min(u32(std::max(fx1, 0.0f)), col.gridW - 1);
		const u32 z0 = u32(std::max(fz0, 0.0f)), z1 = std::min(u32(std::max(fz1, 0.0f)), col.gridH - 1);
		bool moved = false;
		for (u32 z = z0; z <= z1; ++z)
			for (u32 x = x0; x <= x1; ++x)
				for (u32 i = col.cellStart[z * col.gridW + x]; i < col.cellStart[z * col.gridW + x + 1]; ++i) {
					const u32 t = col.cellItems[i] & triMask;
					const Vec3 a = vertex(t, 0), b = vertex(t, 1), d = vertex(t, 2);
					const Vec3 away = c - closestPointOnTriangle(c, a, b, d);
					const float dist2 = dot(away, away);
					if (dist2 >= r * r) continue;
					if (dist2 > 1e-8f) {
						const float dist = std::sqrt(dist2);
						c = c + away * ((r - dist) / dist);
					} else {
						c = c + normalize(cross(b - a, d - a)) * r;
					}
					moved = true;
				}
		if (moved) center = {col.origin[0] + c.x * col.scale, col.origin[1] + c.y * col.scale, col.origin[2] + c.z * col.scale};
		return moved;
	};
	auto boxReference = [&](const float mn[3], const float mx[3]) {
		for (u32 t = 0; t < col.triangleCount; ++t) {
			bool apart = false;
			for (int a = 0; a < 3; ++a) {
				float lo = 1e30f, hi = -1e30f;
				for (int k = 0; k < 3; ++k) {
					const Vec3 v = vertex(t, k);
					const float w = col.origin[a] + (a == 0 ? v.x : a == 1 ? v.y : v.z) * col.scale;
					lo = std::min(lo, w);
					hi = std::max(hi, w);
				}
				apart = apart || lo > mx[a] || hi < mn[a];
			}
			if (!apart) return false;
		}
		return true;
	};

	collisionStatsReset();
	u32 pushed = 0, differ = 0, rays = 0, rayDiffer = 0, boxes = 0, boxDiffer = 0;
	for (int i = 0; i < 30000; ++i) {
		const Vec3 at = {col.origin[0] + (rnd() - 0.5f) * 2800.0f, col.origin[1] + (rnd() - 0.5f) * 3400.0f,
		                 col.origin[2] + (rnd() - 0.5f) * 2800.0f};
		const float radius = 4.0f + rnd() * (i % 7 == 0 ? 300.0f : 60.0f);
		Vec3 a = at, b = at;
		const bool ma = col.pushSphere(a, radius), mb = pushReference(b, radius);
		pushed += ma;
		differ += ma != mb || a.x != b.x || a.y != b.y || a.z != b.z;

		// Straight down: the highest floor below, as found by looking at them all.
		const float drop = rnd() * 700.0f;
		float y = 0;
		Vec3 nrm;
		u8 light[4] = {};
		const bool hit = col.rayDown(at, drop, y, nrm, light);
		float best = -1e30f;
		for (u32 t = 0; t < col.triangleCount; ++t) {
			const Vec3 p0 = vertex(t, 0), p1 = vertex(t, 1), p2 = vertex(t, 2);
			const float lx = (at.x - col.origin[0]) / col.scale, lz = (at.z - col.origin[2]) / col.scale;
			const float det = (p1.z - p2.z) * (p0.x - p2.x) + (p2.x - p1.x) * (p0.z - p2.z);
			if (std::fabs(det) < 1e-6f) continue;
			const float w0 = ((p1.z - p2.z) * (lx - p2.x) + (p2.x - p1.x) * (lz - p2.z)) / det;
			const float w1 = ((p2.z - p0.z) * (lx - p2.x) + (p0.x - p2.x) * (lz - p2.z)) / det;
			const float w2 = 1.0f - w0 - w1;
			if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
			const float wy = col.origin[1] + (p0.y * w0 + p1.y * w1 + p2.y * w2) * col.scale;
			if (wy <= at.y + 0.01f && wy >= at.y - drop - 0.01f) best = std::max(best, wy);
		}
		rays += hit;
		// (A floor within a hundredth of the ends of the ray may go either way.)
		if (hit ? std::fabs(y - best) > 0.05f : best > at.y - drop + 0.05f && best < at.y - 0.05f) ++rayDiffer;

		const float half[3] = {5.0f + rnd() * 200.0f, 5.0f + rnd() * 120.0f, 5.0f + rnd() * 200.0f};
		const float mn[3] = {at.x - half[0], at.y - half[1], at.z - half[2]}, mx[3] = {at.x + half[0], at.y + half[1], at.z + half[2]};
		const bool free = col.boxFree(mn, mx);
		boxes += free;
		boxDiffer += free != boxReference(mn, mx);
	}
	CHECK(pushed > 3000 && pushed < 27000 && differ == 0);
	CHECK(rays > 3000 && rayDiffer == 0);
	CHECK(boxes > 1000 && boxes < 29000 && boxDiffer == 0);
	// Of what the cells hold, a query looks at the vertices of a fraction.
	const CollisionStats& cs = collisionStats();
	CHECK(cs.queries > 30000 && cs.items > cs.queries * 20);
	CHECK(cs.tests * 4 < cs.items);
	std::printf("  collision levels: %u queries, %.0f entries and %.1f triangles each\n", cs.queries,
	            double(cs.items) / cs.queries, double(cs.tests) / cs.queries);
	col.release();
}

static float gDeepest = 0;
static void testCollision() {
	testCollisionLevels();
	// Closest point: face interior, edge and corner regions.
	const Vec3 a {0, 0, 0}, b {10, 0, 0}, c {0, 0, 10};
	Vec3 q = closestPointOnTriangle({2, 5, 2}, a, b, c);
	CHECK(std::fabs(q.x - 2) < 1e-4f && std::fabs(q.y) < 1e-4f && std::fabs(q.z - 2) < 1e-4f);
	q = closestPointOnTriangle({5, 3, -4}, a, b, c);
	CHECK(std::fabs(q.x - 5) < 1e-4f && std::fabs(q.z) < 1e-4f);
	q = closestPointOnTriangle({-3, 1, -3}, a, b, c);
	CHECK(std::fabs(q.x) < 1e-4f && std::fabs(q.z) < 1e-4f);
	q = closestPointOnTriangle({20, 0, 20}, a, b, c);
	CHECK(std::fabs(q.x - 5) < 1e-3f && std::fabs(q.z - 5) < 1e-3f);

	SrcScene scene;
	scene.materials.resize(2);
	scene.materials[0].name = "solid";
	scene.materials[1].name = "ghost";
	scene.materials[1].noCollision = true;
	for (int z = -4; z < 4; ++z)
		for (int x = -4; x < 4; ++x) {
			const float x0 = x * 500.0f, z0 = z * 500.0f;
			const float p[4][3] = {{x0, 0, z0}, {x0 + 500, 0, z0}, {x0 + 500, 0, z0 + 500}, {x0, 0, z0 + 500}};
			addQuad(scene, p[0], p[1], p[2], p[3], 0);
		}
	{
		const float w[4][3] = {{600, 0, -300}, {600, 0, 300}, {600, 300, 300}, {600, 300, -300}};
		addQuad(scene, w[0], w[1], w[2], w[3], 0); // wall across +X
		const float g[4][3] = {{-600, 0, -300}, {-600, 0, 300}, {-600, 300, 300}, {-600, 300, -300}};
		addQuad(scene, g[0], g[1], g[2], g[3], 1); // same, but without collision
		addBlock(scene, -200, 200, 600, 1000, 30);    // a step
		addBlock(scene, -200, 200, -1000, -600, 120); // too tall to walk onto
		const float tan30 = 0.57735f, tan70 = 2.74748f;
		const float r[4][3] = {{800, 0, 1200}, {1200, 0, 1200}, {1200, 500 * tan30, 1700}, {800, 500 * tan30, 1700}};
		addQuad(scene, r[0], r[1], r[2], r[3], 0); // 30 degree ramp
		const float st[4][3] = {{-1200, 0, 1200}, {-800, 0, 1200}, {-800, 200 * tan70, 1400}, {-1200, 200 * tan70, 1400}};
		addQuad(scene, st[0], st[1], st[2], st[3], 0); // 70 degree slope
		// A crease between two 65 degree slopes, its bottom on the floor.
		const float tan65 = 2.14451f;
		const float v1[4][3] = {{1300, 0, -1500}, {1900, 0, -1500}, {1900, 200 * tan65, -1300}, {1300, 200 * tan65, -1300}};
		const float v2[4][3] = {{1900, 0, -1500}, {1300, 0, -1500}, {1300, 200 * tan65, -1700}, {1900, 200 * tan65, -1700}};
		addQuad(scene, v1[0], v1[1], v1[2], v1[3], 0);
		addQuad(scene, v2[0], v2[1], v2[2], v2[3], 0);
		// The same crease, but its bottom falling 25 degrees along X (a gully
		// in the rocks): from 400 cm up at x = -1900 to 120 at x = -1300.
		const float a0 = 400, a1 = 400 - 600 * 0.46631f, up = 200 * tan65;
		const float g1[4][3] = {{-1900, a0, -1500}, {-1300, a1, -1500}, {-1300, a1 + up, -1300}, {-1900, a0 + up, -1300}};
		const float g2[4][3] = {{-1300, a1, -1500}, {-1900, a0, -1500}, {-1900, a0 + up, -1700}, {-1300, a1 + up, -1700}};
		addQuad(scene, g1[0], g1[1], g1[2], g1[3], 0);
		addQuad(scene, g2[0], g2[1], g2[2], g2[3], 0);
		// Hillsides as the game's terrain has them: one sheet - level ground,
		// a face too steep to stand on, level ground on top - with nothing
		// under it. At 60 degrees (x 2100..2950) and at 70 (x 3050..3900).
		const float faces[2][3] = {{2100, 2950, 1.73205f}, {3050, 3900, 2.74748f}};
		for (const auto& f : faces) {
			const float h = 400 * f[2];
			const float lo[4][3] = {{f[0], 0, -1200}, {f[1], 0, -1200}, {f[1], 0, 0}, {f[0], 0, 0}};
			const float face[4][3] = {{f[0], 0, 0}, {f[1], 0, 0}, {f[1], h, 400}, {f[0], h, 400}};
			const float hi[4][3] = {{f[0], h, 400}, {f[1], h, 400}, {f[1], h, 900}, {f[0], h, 900}};
			addQuad(scene, lo[0], lo[1], lo[2], lo[3], 0);
			addQuad(scene, face[0], face[1], face[2], face[3], 0);
			addQuad(scene, hi[0], hi[1], hi[2], hi[3], 0);
		}
	}
	scene.spawnPos[1] = 220.0f;
	scene.hasSpawn = true;
	const std::string path = gDir + "/collision.pak";
	bakeScenePak(scene, path, "c", 1500.0f); // several chunks, so queries cross chunk borders

	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), "c"));
	World& w = app.world();
	w.preload(app.camera().pos);
	CHECK(w.stats().chunksLoaded > 4 && w.stats().collisionBytes > 0);

	// Raw queries.
	float y = -1;
	Vec3 n {};
	CHECK(w.rayDown({10, 100, 10}, 500, y, n) && std::fabs(y) < 0.2f && n.y > 0.99f);
	CHECK(w.rayDown({0, 100, 800}, 500, y, n) && std::fabs(y - 30) < 0.2f);  // on the step
	CHECK(w.rayDown({0, 20, 800}, 500, y, n) && std::fabs(y) < 0.2f);        // from inside it: the floor
	CHECK(!w.rayDown({10, -50, 10}, 500, y, n));                             // from below the world
	CHECK(!w.rayDown({9000, 100, 10}, 500, y, n));                           // outside it
	CHECK(!w.rayDown({10, 900, 10}, 500, y, n));                             // floor farther than the drop
	CHECK(w.rayDown({1000, 400, 1450}, 800, y, n) && std::fabs(y - 250 * 0.57735f) < 1.0f && n.y > 0.85f && n.y < 0.88f);
	Vec3 sphere {585, 150, 0};
	CHECK(w.pushSphere(sphere, 30) && std::fabs(sphere.x - 570) < 0.2f); // pushed off the wall
	sphere = {540, 150, 0};
	CHECK(!w.pushSphere(sphere, 30)); // clear of it
	sphere = {-585, 150, 0};
	CHECK(!w.pushSphere(sphere, 30)); // the ghost wall does not collide
	// Lines of sight: not through the wall, through the ghost wall, over the step.
	CHECK(!w.lineFree({300, 150, 0}, {900, 150, 0}) && !w.lineFree({900, 150, 20}, {300, 100, -20}));
	CHECK(w.lineFree({300, 150, 0}, {580, 150, 0}) && w.lineFree({-300, 150, 0}, {-900, 150, 0}));
	CHECK(w.lineFree({0, 100, 400}, {0, 100, 1200}) && !w.lineFree({0, 15, 400}, {0, 15, 1200}));
	CHECK(w.lineFree({300, 150, 0}, {300, 150, 0})); // (no length at all)

	// The character controller. One step is 480 cm/s at 30 fps = 16 cm.
	const float dt = 1.0f / 30.0f;
	Player& pl = app.player();
	auto place = [&](float x, float z, float yawDegrees) {
		app.teleport({x, 220.0f, z});
		app.camera().yaw = yawDegrees * 0.0174533f;
		for (int i = 0; i < 20; ++i) app.update(Input {}, dt);
	};
	auto walk = [&](int frames, bool jump = false) {
		float top = -1e9f;
		for (int i = 0; i < frames; ++i) {
			Input in;
			in.moveZ = 1.0f;
			in.jump = jump && i == 0;
			app.update(in, dt);
			top = std::max(top, pl.feet.y);
		}
		return top;
	};

	place(0, 0, 0);
	CHECK(pl.placed() && pl.grounded && std::fabs(pl.feet.y) < 0.3f);
	CHECK(std::fabs(app.camera().pos.y - 170.0f) < 1.0f); // eyes settle at eye height

	place(0, 0, 90); // +X, into the wall
	walk(120);
	CHECK(pl.feet.x > 560 && pl.feet.x < 571 && pl.grounded);

	place(0, 0, -90); // -X, through the ghost wall
	walk(60);
	CHECK(pl.feet.x < -900);

	place(0, 0, 0); // +Z, over the 30 cm step and down again
	float top = walk(55);
	CHECK(std::fabs(top - 30) < 0.5f && std::fabs(pl.feet.y - 30) < 0.5f && pl.grounded);
	walk(30);
	CHECK(std::fabs(pl.feet.y) < 0.5f && pl.grounded && pl.feet.z > 1300);

	place(0, 0, 180); // -Z, against the 120 cm block
	top = walk(80);
	CHECK(pl.feet.z < -560 && pl.feet.z > -572 && top < 1.0f);

	place(1000, 900, 0); // up the 30 degree ramp
	walk(40);
	CHECK(pl.grounded && pl.feet.z > 1500 && std::fabs(pl.feet.y - (pl.feet.z - 1200) * 0.57735f) < 2.0f);

	place(-1000, 900, 0); // the 70 degree slope cannot be climbed
	top = walk(80);
	CHECK(top < 60.0f && pl.feet.z < 1230);

	// Jumping at the slope, the stick held forward: the body slides off it
	// and stands at its foot - it is not pinned to the slope, sliding for ever.
	place(-1000, 900, 0);
	walk(10);
	{
		int sliding = 0;
		for (int i = 0; i < 90; ++i) {
			Input in;
			in.moveZ = 1.0f;
			in.jump = i == 0;
			app.update(in, dt);
			sliding = pl.grounded ? 0 : sliding + 1;
		}
		std::printf("       jumped at the steep slope: feet at z %.0f, %.0f up, %s (%d frames off the ground)\n", double(pl.feet.z),
		            double(pl.feet.y), pl.grounded ? "standing" : "sliding", sliding);
		CHECK(pl.grounded && pl.feet.y < 50.0f && pl.feet.z < 1260.0f);
	}
	// Dropped into the crease between two slopes too steep to stand on: it
	// comes to rest there instead of sliding from one to the other.
	app.teleport({1600, 600 + 170.0f, -1460});
	app.update(Input {}, dt);
	pl.feet = {1600, 600, -1460};
	pl.grounded = false;
	pl.velocityY = 0;
	{
		int frames = 0;
		while (!pl.grounded && frames < 300) {
			app.update(Input {}, dt);
			++frames;
		}
		const Vec3 rest = pl.feet;
		bool stays = pl.grounded;
		for (int i = 0; i < 60; ++i) {
			app.update(Input {}, dt);
			stays = stays && pl.grounded;
		}
		std::printf("       dropped into the crease: at rest after %d frames, %.0f up at z %.0f; %s\n", frames, double(pl.feet.y),
		            double(pl.feet.z), stays ? "stays" : "does not stay");
		CHECK(pl.grounded && stays && frames < 90 && length(pl.feet - rest) < 2.0f && std::fabs(pl.feet.z + 1500.0f) < 80.0f);
		// ... and walks out of it again along the crease.
		app.camera().yaw = 90.0f * 0.0174533f;
		walk(30);
		CHECK(pl.feet.x > 1750.0f);
	}

	// Dropped into the gully: it slides down along it (or stands in it) - it
	// does not hang there, rocking from one side to the other.
	app.teleport({-1850, 900 + 170.0f, -1470});
	app.update(Input {}, dt);
	pl.feet = {-1850, 900, -1470};
	pl.grounded = false;
	pl.velocityY = 0;
	{
		int frames = 0;
		while (!pl.grounded && frames < 300) {
			app.update(Input {}, dt);
			++frames;
		}
		std::printf("       dropped into the gully: %s after %d frames at %.0f %.0f %.0f\n", pl.grounded ? "standing" : "still sliding", frames,
		            double(pl.feet.x), double(pl.feet.y), double(pl.feet.z));
		CHECK(pl.grounded && frames < 120);
	}

	// Running and jumping at a hillside with nothing under it, from every
	// distance: the body never ends up hanging in the air (the feet used to
	// get into the rock below the spheres, and then there was nothing under
	// them at all).
	for (const float hx : {2525.0f, 3475.0f}) {
		int hung = 0, tries = 0;
		for (int startZ = -420; startZ <= -60; startZ += 15) {
			for (const float sideYaw : {0.0f, 20.0f, -30.0f}) {
				app.teleport({hx, 220.0f, float(startZ)});
				app.camera().yaw = sideYaw * 0.0174533f;
				for (int i = 0; i < 10; ++i) app.update(Input {}, dt);
				int air = 0;
				for (int i = 0; i < 150; ++i) {
					Input in;
					in.moveZ = i < 40 ? 1.0f : 0.0f;
					in.jump = i == 2;
					app.update(in, dt);
					air = pl.grounded ? 0 : air + 1;
					const float tanA = hx < 3000 ? 1.73205f : 2.74748f;
					const float surf = std::min(std::max(pl.feet.z, 0.0f), 400.0f) * tanA;
					gDeepest = std::max(gDeepest, surf - pl.feet.y);
				}
				++tries;
				if (air > 45) {
					++hung;
					if (hung <= 3)
						std::printf("       hillside x %.0f from z %d yaw %.0f: off the ground %d frames at %.0f %.0f %.0f\n", double(hx), startZ,
						            double(sideYaw), air, double(pl.feet.x), double(pl.feet.y), double(pl.feet.z));
				}
			}
		}
		std::printf("       jumps at the hillside at x %.0f: %d of %d hang in the air; feet at most %.0f cm under it\n", double(hx), hung, tries, double(gDeepest));
		gDeepest = 0;
		CHECK(hung == 0);
	}

	// Feet in the rock with nothing under them (the hillside's top is 693 cm
	// up, and the sheet has no underside): out on top at once ...
	auto inRock = [&](float x, float feetY, float z) {
		app.teleport({x, 900.0f, z});
		for (int i = 0; i < 5; ++i) app.update(Input {}, dt);
		pl.feet = {x, feetY, z};
		pl.grounded = false;
		pl.velocityY = 0;
	};
	{
		const u32 pops = pl.popUps, unstuck = pl.unstuck;
		inRock(2525, 560, 650);
		app.update(Input {}, dt);
		CHECK(pl.grounded && std::fabs(pl.feet.y - 693.0f) < 1.0f && pl.popUps == pops + 1 && pl.debug().step == 'p');
		// The movement recorder has it, as its newest line, and writes it out.
		const MoveRecorder& rec = app.moves();
		CHECK(rec.count() == MoveRecorder::kCapacity);
		const MoveSample& last = rec.sample(rec.count() - 1);
		CHECK(last.step == 'p' && (last.flags & MOVE_GROUNDED) && std::fabs(last.feet.y - 693.0f) < 1.0f && last.body == '?');
		char line[256];
		MoveRecorder::format(line, sizeof(line), last);
		CHECK(std::strstr(line, " g-r----- p ? ") != nullptr);
		const std::string recPath = gDir + "/slope.txt";
		FILE* rf = std::fopen(recPath.c_str(), "w");
		CHECK(rec.write(rf, "test"));
		std::fclose(rf);
		std::string written;
		if (FILE* back = std::fopen(recPath.c_str(), "rb")) {
			char buf[4096];
			for (size_t got; (got = std::fread(buf, 1, sizeof(buf), back)) > 0;) written.append(buf, got);
			std::fclose(back);
		}
		CHECK(written.rfind("== test: 300 frames\ncolumns:", 0) == 0 && std::count(written.begin(), written.end(), '\n') == 307);
		// ... and from deeper than a body's height, after 1.5 s of getting nowhere.
		inRock(2525, 300, 650);
		int frames = 0;
		while (!pl.grounded && frames < 120) {
			app.update(Input {}, dt);
			++frames;
		}
		std::printf("       feet 4 m deep in the rock: on top after %d frames (%.0f up)\n", frames, double(pl.feet.y));
		CHECK(pl.grounded && std::fabs(pl.feet.y - 693.0f) < 1.0f && pl.unstuck == unstuck + 1 && frames >= 44 && frames <= 92);
		// Walks off it again.
		app.camera().yaw = 0;
		walk(8);
		CHECK(pl.grounded && pl.feet.z > 760.0f);
		// Nothing below and nothing above (between the floor and the hillside):
		// that is the edge of the loaded world - it waits there, it is not
		// stood on thin air.
		const u32 before = pl.unstuck;
		inRock(1950, 0, -500); // (placed on the floor first)
		pl.feet = {2050, 100, -500};
		for (int i = 0; i < 120; ++i) app.update(Input {}, dt);
		std::printf("       nothing below or above: %s at %.0f %.0f %.0f, step %c\n", pl.grounded ? "standing" : "waiting", double(pl.feet.x),
		            double(pl.feet.y), double(pl.feet.z), pl.debug().step);
		CHECK(!pl.grounded && std::fabs(pl.feet.y - 100.0f) < 5.0f && pl.unstuck == before && pl.debug().step == 'h');
	}

	place(0, 0, 0); // jump: about v^2 / 2g = 82 cm, then land
	top = -1e9f;
	for (int i = 0; i < 40; ++i) {
		Input in;
		in.jump = i == 0;
		app.update(in, dt);
		top = std::max(top, pl.feet.y);
	}
	CHECK(top > 60 && top < 95 && pl.grounded && std::fabs(pl.feet.y) < 0.3f);

	// Dropped from 10 m: falls and lands. (Placement looks 30 m down.)
	app.teleport({300, 1000.0f + 170.0f, -300});
	app.update(Input {}, dt);
	CHECK(pl.placed()); // snapped straight to the ground it found
	pl.feet.y = 1000;
	pl.grounded = false;
	pl.velocityY = 0;
	int frames = 0;
	while (!pl.grounded && frames < 200) {
		app.update(Input {}, dt);
		++frames;
	}
	CHECK(pl.grounded && std::fabs(pl.feet.y) < 0.3f && frames > 20 && frames < 45); // ~1 s for 10 m at 1900 cm/s^2

	// Outside the world there is nothing to stand on: wait, do not fall.
	app.teleport({6000, 220, 0});
	for (int i = 0; i < 30; ++i) app.update(Input {}, dt);
	CHECK(!pl.placed() && std::fabs(pl.feet.y - 50.0f) < 0.01f);

	// Fly mode ignores collision and gravity.
	place(0, 0, 90);
	Input fly;
	fly.toggleFly = true;
	app.update(fly, dt);
	CHECK(pl.flying);
	for (int i = 0; i < 60; ++i) {
		Input in;
		in.moveZ = 1.0f;
		app.update(in, dt);
	}
	CHECK(pl.feet.x > 1500 && std::fabs(pl.feet.y) < 1.0f); // straight through the wall

	app.shutdown();
	CHECK(gfx::memStats().meshCount == 0);
	gfx::shutdown();
}

// ---- characters ----

// The rest-pose vertices of a source model, posed by hand: the reference the
// runtime's skinning is compared with.
static Vec3 posedVertex(const SrcSkinVertex& v, const std::vector<Mat34>& skin) {
	const Vec3 p = {v.pos[0], v.pos[1], v.pos[2]};
	const Vec3 a = transformPoint(skin[size_t(v.bone[0])], p), b = transformPoint(skin[size_t(v.bone[1])], p);
	return a + (b - a) * v.weight1;
}

static void testModel() {
	SrcModel chain, actor;
	buildChainModel(chain, 60);
	buildSyntheticActor(actor);
	BakeOptions bo;
	bo.tex.maxSize = 32;
	bo.tex.quality = 0;
	bo.quiet = true;
	const std::string path = gDir + "/models.pak";
	{
		PakWriter writer;
		const ModelBakeResult r = bakeModel(chain, bo, writer);
		// 59 double-sided quads; 60 bones do not fit one palette.
		CHECK(r.triangles == 59 * 4 && r.parts >= 3 && r.largestPalette == MODEL_PALETTE && r.clips == 2 && r.textures == 1);
		CHECK(r.report.find("skeleton: 60 nodes") != std::string::npos);
		const ModelBakeResult a = bakeModel(actor, bo, writer);
		CHECK(a.parts == 2 && a.clips == 82 && a.largestPalette <= MODEL_PALETTE);
		CHECK(a.report.find("S_1HATTACK 1.88s 3 blows") != std::string::npos && a.report.find("T_FISTPARADE_O 0.52s parry") != std::string::npos);
		CHECK(a.report.find("blends: 5: T_RUNL_2_WALKL 0.20s") != std::string::npos);
		CHECK(a.report.find("T_STAND_2_JUMP 0.48s 239cm/s up 74cm") != std::string::npos);
		writer.write(path);
	}
	// What cannot be baked is refused with a reason.
	{
		PakWriter writer;
		SrcModel bad = chain;
		bad.nodes[3].parent = 7;
		bad.nodes[7].parent = 3;
		bool threw = false;
		try {
			bakeModel(bad, bo, writer);
		} catch (const std::runtime_error& e) {
			threw = std::string(e.what()).find("cycle") != std::string::npos;
		}
		CHECK(threw);
		bad = chain;
		bad.parts[0].verts[5].bone[1] = 60;
		threw = false;
		try {
			bakeModel(bad, bo, writer);
		} catch (const std::runtime_error&) {
			threw = true;
		}
		CHECK(threw);
		bad = chain;
		bad.clips[0].samples.pop_back();
		threw = false;
		try {
			bakeModel(bad, bo, writer);
		} catch (const std::runtime_error&) {
			threw = true;
		}
		CHECK(threw);
	}

	// Root motion: what the root does since the first frame is where the clip
	// carries the character; the samples keep the root over the character.
	{
		SrcModel m = chain;
		m.name = "walker";
		m.clips.clear();
		SrcClip crawl, stay, hop, still;
		crawl.name = crawl.next = "S_CRAWL";
		stay.name = stay.next = "S_STAY";
		hop.name = "T_HOP";
		still.name = still.next = "S_STILL";
		crawl.fps = stay.fps = hop.fps = still.fps = 10.0f;
		crawl.moves = stay.moves = hop.moves = still.moves = true; // (ZenGin flags standing animations, too)
		hop.fly = true;
		crawl.nodes = stay.nodes = hop.nodes = still.nodes = {0};
		for (int f = 0; f < 5; ++f) {
			SrcClipSample s {};
			float rest[3];
			chainRest(60, 0, rest, s.rot);
			s.pos[0] = 12.0f + 3.0f * float(f % 2); // a stance off centre, swaying
			s.pos[1] = 10.0f;
			s.pos[2] = 50.0f * float(f);
			crawl.samples.push_back(s);
			s.pos[2] = -1.0f;
			stay.samples.push_back(s);
			s.pos[0] = 12.0f;
			still.samples.push_back(s);
			s.pos[1] = 10.0f + 30.0f * float(f); // up and away
			s.pos[2] = 20.0f * float(f);
			hop.samples.push_back(s);
		}
		m.clips = {crawl, stay, hop, still};
		m.blends = {{"T_CRAWL_2_STAY", "S_STAY", 0.25f, 0.1f}, {"T_HOP", "S_STAY", 9.0f, 9.0f}, {"", "S_STAY", 1.0f, 1.0f}};
		PakWriter writer;
		const ModelBakeResult r = bakeModel(m, bo, writer);
		CHECK(r.report.find("S_CRAWL 0.50s loop 500cm/s") != std::string::npos);
		CHECK(r.report.find("T_HOP 0.40s 200cm/s up 120cm") != std::string::npos);
		CHECK(r.report.find("blends: 1: T_CRAWL_2_STAY 0.25s") != std::string::npos); // an animation of the name wins; no name, no blend
		const std::string walkerPath = gDir + "/walker.pak";
		writer.write(walkerPath);
		Pak wp;
		CHECK(wp.open(walkerPath.c_str()));
		Clip c, st, hp, sl;
		CHECK(c.load(wp, hashName("anim/walker/s_crawl"), 60) && st.load(wp, hashName("anim/walker/s_stay"), 60));
		CHECK(hp.load(wp, hashName("anim/walker/t_hop"), 60) && sl.load(wp, hashName("anim/walker/s_still"), 60));
		CHECK(c.moves() && (c.header().flags & CLIPF_MOVES) && std::fabs(c.header().moveSpeed - 500.0f) < 1.0f && c.rise() == 0.0f);
		// The sway of a stance is motion, too (the body sways with it), but no speed.
		CHECK(st.moves() && st.header().moveSpeed == 0.0f);
		// A root that stays put carries nobody anywhere, whatever the script's flag says.
		CHECK(!sl.moves() && !(sl.header().flags & CLIPF_MOVES) && sl.motion(0.3f).z == 0.0f);
		CHECK(hp.moves() && (hp.header().flags & CLIPF_FLY) && !hp.loops() && std::fabs(hp.rise() - 120.0f) < 0.1f);
		Pose p;
		for (int f = 0; f < 5; ++f) {
			// The root is drawn over the character: x and z are gone from the samples ...
			c.sample(float(f) * 0.1f, p);
			CHECK(p.pos[0].x == 0.0f && p.pos[0].z == 0.0f && std::fabs(p.pos[0].y - 10.0f) < 0.01f);
			// ... and are where the clip carries it.
			const Vec3 at = c.motion(float(f) * 0.1f);
			CHECK(std::fabs(at.x - 3.0f * float(f % 2)) < 0.02f && std::fabs(at.z - 50.0f * float(f)) < 0.02f && at.y == 0.0f);
		}
		// Between frames, and round the loop: the step from the last frame
		// back to the first is taken at the clip's average pace.
		CHECK(std::fabs(c.motion(0.25f).z - 125.0f) < 0.05f && std::fabs(c.motion(0.25f).x - 1.5f) < 0.02f);
		CHECK(std::fabs(c.motion(0.45f).z - 225.0f) < 0.05f);
		CHECK(std::fabs(c.motion(0.5f).z - 250.0f) < 0.05f && std::fabs(c.motion(1.7f).z - 850.0f) < 0.1f);
		CHECK(std::fabs(c.motion(1.7f).x) < 0.02f && c.motion(-1.0f).z == 0.0f);
		// A clip that ends stays where it got to; its height is part of the way.
		const Vec3 top = hp.motion(0.4f), beyond = hp.motion(5.0f), mid = hp.motion(0.2f);
		CHECK(std::fabs(top.y - 120.0f) < 0.05f && std::fabs(top.z - 80.0f) < 0.05f && beyond.y == top.y && beyond.z == top.z);
		CHECK(std::fabs(mid.y - 60.0f) < 0.05f && std::fabs(mid.z - 40.0f) < 0.05f);

		// The animator adds up what its clips carry, across the hand-over
		// from a clip to its follower.
		gfx::init();
		Model wm;
		CHECK(wm.load(wp, "walker") && wm.blendCount() == 1);
		const ModelBlend* bl = wm.blend(hashName("t_crawl_2_stay"));
		CHECK(bl && bl->nextHash == hashName("S_STAY") && std::fabs(bl->time - 0.25f) < 1e-6f && !wm.blend(hashName("T_HOP")));
		Animator an;
		an.bind(&wm);
		an.play(wm.clip("S_CRAWL"), 0.0f);
		Vec3 sum {0, 0, 0};
		for (int i = 0; i < 37; ++i) {
			an.update(0.033f);
			sum = sum + an.takeMotion();
		}
		CHECK(std::fabs(sum.z - 500.0f * 37 * 0.033f) < 0.2f && an.takeMotion().z == 0.0f);
		an.play(wm.clip("T_HOP"), 0.0f); // 0.4 s, no follower
		sum = {0, 0, 0};
		for (int i = 0; i < 20; ++i) {
			an.update(0.033f);
			sum = sum + an.takeMotion();
		}
		CHECK(an.finished() && std::fabs(sum.y - 120.0f) < 0.1f && std::fabs(sum.z - 80.0f) < 0.1f);
		an.restart(0.0f);
		CHECK(!an.finished() && an.time() == 0.0f && an.current() == wm.clip("T_HOP"));
		wm.release();
		gfx::shutdown();
	}

	gfx::init();
	Pak pak;
	CHECK(pak.open(path.c_str()));
	const PakEntry* me = pak.find("model/chain");
	const PakEntry* ce = pak.find("anim/chain/s_bend");
	CHECK(me && me->type == ASSET_MODEL && ce && ce->type == ASSET_CLIP && pak.find("tex/chain_skin"));
	{
		Model m;
		CHECK(!m.load(pak, "nobody") && !m.loaded());
		CHECK(m.load(pak, "chain") && m.loaded());
		CHECK(m.nodeCount() == 60 && m.triangles() == 59 * 4 && m.clipCount() == 2 && m.textureBytes() > 0);
		CHECK(m.findNode(hashName("LINK17")) == 17 && m.findNode(hashName("link59")) == 59 && m.findNode(hashName("NOPE")) < 0);
		u32 indexSum = 0;
		for (u32 i = 0; i < m.partCount(); ++i) {
			CHECK(m.part(i).boneCount > 0 && m.part(i).boneCount <= MODEL_PALETTE);
			indexSum += m.part(i).indexCount;
		}
		CHECK(indexSum == 59 * 4 * 3);

		// At rest every bone's skin matrix is the identity: the mesh is drawn as stored.
		Pose rest;
		m.restPose(rest);
		Mat34 world[MODEL_MAX_NODES];
		m.worldTransforms(rest, world);
		float worst = 0;
		for (u32 i = 0; i < m.nodeCount(); ++i) {
			Mat34 inv;
			std::memcpy(inv.m, m.node(i).invBind, sizeof(inv.m));
			const Mat34 skin = mul(world[i], inv), id = mat34Identity();
			for (int r = 0; r < 3; ++r)
				for (int c = 0; c < 4; ++c) worst = std::max(worst, std::fabs(skin.m[r][c] - id.m[r][c]));
		}
		std::printf("  rest pose: skin matrices within %.2g of the identity\n", double(worst));
		CHECK(worst < 1e-3f); // sixty transforms deep

		// A clip that is played once: sampled between its frames, held at its ends.
		const Clip* bend = m.clip("S_BEND");
		const Clip* nod = m.clip("t_nod");
		CHECK(bend && nod && !m.clip("S_RUN"));
		if (bend && nod) {
			CHECK(bend->header().frameCount == 5 && bend->header().trackCount == 60 && !bend->loops());
			CHECK(std::fabs(bend->duration() - 0.4f) < 1e-5f && bend->header().next == 0 && bend->header().moveSpeed == 0.0f);
			CHECK(nod->header().trackCount == 1 && nod->header().next == hashName("S_BEND") && std::fabs(nod->duration() - 0.1f) < 1e-5f);
			Pose p = rest;
			bend->sample(0.0f, p);
			CHECK(std::fabs(p.pos[0].y - 10.0f) < 0.01f);
			bend->sample(0.05f, p); // half way from frame 0 to frame 1
			CHECK(std::fabs(p.pos[0].y - 10.5f) < 0.01f);
			bend->sample(0.2f, p);
			CHECK(std::fabs(p.pos[0].y - 12.0f) < 0.01f);
			// Link 5 at frame 2: its rest rotation, then 0.04 rad about its own X.
			float restPos[3], restRot[4];
			chainRest(60, 5, restPos, restRot);
			const Quat want = quatMul({restRot[0], restRot[1], restRot[2], restRot[3]}, {std::sin(0.02f), 0, 0, std::cos(0.02f)});
			const float d = std::fabs(want.x * p.rot[5].x + want.y * p.rot[5].y + want.z * p.rot[5].z + want.w * p.rot[5].w);
			CHECK(d > 0.999999f);
			CHECK(std::fabs(p.pos[5].y - 7.0f) < 0.01f);
			bend->sample(99.0f, p);
			CHECK(std::fabs(p.pos[0].y - 14.0f) < 0.01f);
			bend->sample(-3.0f, p);
			CHECK(std::fabs(p.pos[0].y - 10.0f) < 0.01f);
			// A clip only moves the nodes it has tracks for.
			Pose q = rest;
			q.pos[3] = {1, 2, 3};
			nod->sample(0.05f, q);
			CHECK(q.pos[3].x == 1 && q.pos[3].y == 2 && q.pos[3].z == 3 && std::fabs(q.rot[59].y - std::sin(0.15f)) < 1e-3f);
		}

		// The animator: T_NOD runs out and hands over to S_BEND, which runs out
		// and stays on its last frame.
		Animator an;
		an.bind(&m);
		CHECK(an.current() == nullptr && !an.finished() && std::fabs(an.pose().pos[0].y - 10.0f) < 1e-4f);
		an.play(nod);
		an.update(0.05f);
		CHECK(an.current() == nod && std::fabs(an.time() - 0.05f) < 1e-6f);
		an.play(nod); // already playing: not restarted
		CHECK(std::fabs(an.time() - 0.05f) < 1e-6f);
		const Quat before = an.pose().rot[59];
		an.update(0.1f);
		CHECK(an.current() == bend && std::fabs(an.time() - 0.05f) < 1e-5f && !an.finished());
		// The hand-over fades (T_NOD's blend-out is 0.25 s): no jump in the pose.
		const Quat after = an.pose().rot[59];
		CHECK(std::fabs(before.x * after.x + before.y * after.y + before.z * after.z + before.w * after.w) > 0.995f);
		for (int i = 0; i < 20; ++i) an.update(0.05f);
		CHECK(an.finished() && an.current() == bend && std::fabs(an.time() - bend->duration()) < 1e-6f);
		CHECK(std::fabs(an.pose().pos[0].y - 14.0f) < 0.01f);
		an.play(nullptr, 0.0f); // back to rest
		an.update(0.01f);
		CHECK(an.current() == nullptr && std::fabs(an.pose().pos[0].y - 10.0f) < 1e-4f);
		// A clip without a track for a node leaves it where the clip before
		// left it, as in ZenGin: T_NOD has none for the root, so after S_BEND
		// it stays bent up at 14 (the game's "S_BENCH_S1" has no root track
		// either: the root stays as the step into it left it, down on the seat).
		an.play(bend, 0.0f);
		for (int i = 0; i < 20; ++i) an.update(0.05f);
		CHECK(std::fabs(an.pose().pos[0].y - 14.0f) < 0.01f);
		an.play(nod, 0.0f);
		an.update(0.02f);
		CHECK(an.current() == nod && std::fabs(an.pose().pos[0].y - 14.0f) < 0.01f);
		an.play(nullptr, 0.0f);
		an.update(0.01f);
		CHECK(std::fabs(an.pose().pos[0].y - 10.0f) < 1e-4f);

		// Drawn where the source mesh is - at rest and curled up - seen from
		// the side, standing somewhere in the world and turned.
		for (int posed = 0; posed < 2; ++posed) {
			Pose pose = rest;
			if (posed && bend) bend->sample(0.4f, pose);
			m.worldTransforms(pose, world);
			std::vector<Mat34> skin(60);
			for (u32 i = 0; i < 60; ++i) {
				Mat34 inv;
				std::memcpy(inv.m, m.node(i).invBind, sizeof(inv.m));
				skin[i] = mul(world[i], inv);
			}
			const Vec3 at = {300.0f, -50.0f, 700.0f};
			const float yaw = 0.6f, light[3] = {1.0f, 0.8f, 0.6f};
			gfx::FrameDesc fd {};
			const Vec3 eye = {300.0f, 180.0f, -100.0f};
			fd.view = viewLH(eye, 0.0f, 0.0f);
			fd.eye = eye;
			fd.fovY = 1.0f;
			fd.zNear = 20.0f;
			fd.zFar = 5000.0f;
			fd.fogStart = 4000.0f;
			fd.fogEnd = 5000.0f;
			fd.clearColor[0] = 1;
			fd.clearColor[1] = 2;
			fd.clearColor[2] = 3;
			fd.dayLight[0] = fd.dayLight[1] = fd.dayLight[2] = 1.0f;
			gfx::frameBegin(fd);
			gfx::setPass(gfx::PASS_WORLD);
			m.draw(pose, at, yaw, light);
			gfx::frameEnd();
			const gfx::SoftStats ss = gfx::softStats();
			// The strip has both sides: one of each pair faces the camera.
			CHECK(ss.skinnedTriangles == 59 * 4 && ss.skinnedBackface == 59 * 2 && ss.skinnedPixels > 100);
			// Where the pixels are ...
			int x0 = 9999, x1 = -1, y0 = 9999, y1 = -1;
			const u8* fb = gfx::softFramebuffer();
			for (int y = 0; y < gfx::SCREEN_H; ++y)
				for (int x = 0; x < gfx::SCREEN_W; ++x) {
					const u8* px = fb + (y * gfx::SCREEN_W + x) * 4;
					if (px[0] == 1 && px[1] == 2 && px[2] == 3) continue;
					x0 = std::min(x0, x);
					x1 = std::max(x1, x);
					y0 = std::min(y0, y);
					y1 = std::max(y1, y);
				}
			// ... and where the source vertices land.
			const Mat4 vp = mul(perspectiveLH(fd.fovY, float(gfx::SCREEN_W) / float(gfx::SCREEN_H), fd.zNear, fd.zFar), fd.view);
			const Mat34 turn = mat34From(quatFromYaw(yaw), at);
			float wx0 = 1e9f, wx1 = -1e9f, wy0 = 1e9f, wy1 = -1e9f;
			for (const SrcSkinVertex& v : chain.parts[0].verts) {
				const Vec3 w = transformPoint(turn, posedVertex(v, skin));
				const Vec4 c = transform(vp, {w.x, w.y, w.z, 1.0f});
				const float sx = (c.x / c.w * 0.5f + 0.5f) * gfx::SCREEN_W, sy = (0.5f - c.y / c.w * 0.5f) * gfx::SCREEN_H;
				wx0 = std::min(wx0, sx);
				wx1 = std::max(wx1, sx);
				wy0 = std::min(wy0, sy);
				wy1 = std::max(wy1, sy);
			}
			std::printf("  chain %s: %u triangles, %u back, %u pixels; drawn %d..%d x %d..%d, expected %.1f..%.1f x %.1f..%.1f\n", posed ? "curled" : "at rest",
			            ss.skinnedTriangles, ss.skinnedBackface, ss.skinnedPixels, x0, x1, y0, y1, double(wx0), double(wx1), double(wy0), double(wy1));
			CHECK(wy1 - wy0 > (posed ? 35.0f : 80.0f)); // a tall thing on the screen, shorter once curled up
			CHECK(std::fabs(float(x0) - wx0) < 1.6f && std::fabs(float(x1) - wx1) < 1.6f);
			CHECK(std::fabs(float(y0) - wy0) < 1.6f && std::fabs(float(y1) - wy1) < 1.6f);
		}
		m.release();
		CHECK(!m.loaded() && gfx::memStats().meshCount == 0 && gfx::memStats().textureCount == 0);
	}
	{
		// The test character: loops wrap, moving clips know their speed.
		Model m;
		CHECK(m.load(pak, "hero") && m.nodeCount() == 16 && m.clipCount() == 82 && m.blendCount() == 5);
		// The frames of a fight come through: three blows of a combo, each
		// with its window and its end; a parry's frames; where a draw changes hands.
		{
			const Clip* attack = m.clip("S_1HATTACK");
			const Clip* parry = m.clip("T_1HPARADE_O");
			const Clip* draw = m.clip("T_1H_2_1HRUN");
			CHECK(attack && parry && draw && m.clip("S_RUN") && !m.clip("S_RUN")->fight());
			if (attack && parry && draw) {
				const ClipFight* f = attack->fight();
				CHECK(f && f->hits == 3 && f->hit[0] == 7 && f->hit[1] == 23 && f->hit[2] == 39 && f->hitEnds == 3 && f->hitEnd[2] == 47);
				CHECK(f && f->windows == 3 && f->window[2] == 19 && f->window[3] == 31 && f->parry[0] == CLIP_NO_FRAME);
				const float fps = attack->header().fps;
				CHECK(std::fabs(blowTime(f, 1, fps) - 23.0f / 25.0f) < 1e-5f && blowTime(f, 3, fps) < 0.0f);
				CHECK(std::fabs(blowEnd(f, 2, fps) - 47.0f / 25.0f) < 1e-5f);
				CHECK(inComboWindow(f, 0, fps, 0.3f) && !inComboWindow(f, 0, fps, 0.7f) && inComboWindow(f, 1, fps, 1.0f) && !inComboWindow(f, 3, fps, 1.0f));
				const ClipFight* p = parry->fight();
				CHECK(p && p->hits == 0 && p->parry[0] == 1 && p->parry[1] == 12 && inParry(p, fps, 0.1f) && !inParry(p, fps, 0.5f));
				CHECK(draw->fight() && draw->fight()->fightMode == 6 && std::fabs(draw->frameTime(6) - 0.24f) < 1e-5f);
			}
		}
		const Clip* run = m.clip("S_RUNL");
		const Clip* walk = m.clip("S_WALKL");
		const Clip* stand = m.clip("S_RUN");
		const Clip* takeOff = m.clip("T_STAND_2_JUMP");
		CHECK(run && walk && stand && takeOff);
		if (run && walk && stand && takeOff) {
			CHECK(run->loops() && (run->header().flags & CLIPF_MOVES) && std::fabs(run->header().moveSpeed - 480.0f) < 2.0f);
			CHECK(std::fabs(walk->header().moveSpeed - 150.0f) < 2.0f);
			CHECK(stand->loops() && !stand->moves() && stand->header().moveSpeed == 0.0f);
			// A loop carries the character on and on, at its pace.
			CHECK(std::fabs(run->motion(0.3f).z - 144.0f) < 0.1f && std::fabs(run->motion(run->duration() * 2.5f).z - 480.0f * 0.64f * 2.5f) < 0.2f);
			CHECK((takeOff->header().flags & CLIPF_FLY) && takeOff->rise() > 70.0f && takeOff->rise() < 76.0f);
			CHECK(!takeOff->loops() && takeOff->header().next == hashName("S_JUMP"));
			// The root does not travel: the game moves the character.
			Pose a, b, c;
			m.restPose(a);
			b = c = a;
			const float d = run->duration();
			run->sample(0.3f * d, a);
			run->sample(1.3f * d, b);
			run->sample(-0.7f * d, c);
			float wrap = 0;
			for (u32 i = 0; i < m.nodeCount(); ++i)
				wrap = std::max({wrap, std::fabs(a.rot[i].x - b.rot[i].x), std::fabs(a.rot[i].w - b.rot[i].w), std::fabs(a.rot[i].x - c.rot[i].x),
				                 std::fabs(a.pos[i].y - b.pos[i].y)});
			CHECK(wrap < 1e-3f);
			float travel = 0;
			for (int f = 0; f < 16; ++f) {
				run->sample(d * float(f) / 16.0f, a);
				travel = std::max({travel, std::fabs(a.pos[0].x), std::fabs(a.pos[0].z)});
			}
			CHECK(travel < 0.01f && a.pos[0].y > 80.0f);
			// The end of a loop leads back into its start: no jump at the seam.
			run->sample(d - 1e-4f, a);
			run->sample(0.0f, b);
			float seam = 0;
			for (u32 i = 0; i < m.nodeCount(); ++i) seam = std::max(seam, std::fabs(a.rot[i].x - b.rot[i].x));
			CHECK(seam < 0.01f);
		}
	}
	pak.close();

	// Damaged data is turned down, never trusted.
	{
		Pak src;
		CHECK(src.open(path.c_str()));
		auto blob = [&](const char* name) {
			const PakEntry* e = src.find(name);
			std::vector<u8> out(e ? e->size : 0);
			if (e) CHECK(src.read(*e, out.data()));
			return out;
		};
		const std::vector<u8> model = blob("model/chain"), clip = blob("anim/chain/s_bend"), nod = blob("anim/chain/t_nod");
		ModelHeader mh;
		std::memcpy(&mh, model.data(), sizeof(mh));
		const size_t partsAt = sizeof(ModelHeader) + mh.nodeCount * sizeof(ModelNode);
		const size_t verticesAt = partsAt + mh.partCount * sizeof(ModelPart) + mh.clipCount * sizeof(ModelClipRef) +
		                          mh.blendCount * sizeof(ModelBlend);
		const size_t indicesAt = verticesAt + mh.vertexCount * sizeof(WorldVertex);
		struct Damage {
			const char* what;
			size_t at;
			u8 value;
			bool truncate;
		};
		const Damage damages[] = {
		    {"magic", 0, 'X', false},
		    {"version", 4, 9, false},
		    {"node count", 8, 61, false},
		    {"truncated", 0, 0, true},
		    {"parent after child", sizeof(ModelHeader) + 5 * sizeof(ModelNode), 40, false},
		    {"palette size", partsAt + 6, 27, false},
		    {"palette bone", partsAt + 16, 200, false},
		    {"index range", partsAt + 12 + 3, 0x7F, false}, // top byte of indexCount
		    {"vertex slot beyond the palette", verticesAt + 12, 78, false},
		    {"vertex slot between rows", verticesAt + 13, 4, false},
		    {"index beyond the vertices", indicesAt + 1, 0xFF, false},
		};
		PakWriter writer;
		writer.add("model/good", ASSET_MODEL, model);
		int n = 0;
		for (const Damage& d : damages) {
			std::vector<u8> bad = model;
			if (d.truncate)
				bad.resize(bad.size() - 4);
			else
				bad[d.at] = d.value;
			writer.add("model/bad" + std::to_string(n++), ASSET_MODEL, bad);
		}
		// Clips: a good one, one for a longer skeleton, one cut short.
		ModelClipRef refs[2];
		std::memcpy(refs, model.data() + partsAt + mh.partCount * sizeof(ModelPart), sizeof(refs));
		std::vector<u8> badClip = clip;
		badClip[sizeof(ClipHeader) + 7] = 60; // a track for node 60 of 60
		writer.add("anim/chain/s_bend", ASSET_CLIP, badClip);
		std::vector<u8> shortClip = nod;
		shortClip.resize(shortClip.size() - 14);
		writer.add("anim/chain/t_nod", ASSET_CLIP, shortClip);
		const std::string badPath = gDir + "/badmodels.pak";
		writer.write(badPath);
		src.close();

		Pak bad;
		CHECK(bad.open(badPath.c_str()));
		Model m;
		for (int i = 0; i < n; ++i) {
			const bool loaded = m.load(bad, ("bad" + std::to_string(i)).c_str());
			if (loaded) std::printf("  damaged model accepted: %s\n", damages[i].what);
			CHECK(!loaded && !m.loaded());
		}
		CHECK(gfx::memStats().meshCount == 0 && gfx::memStats().textureCount == 0);
		// The model itself is fine; its broken clips are simply not there, and
		// its missing texture leaves it untextured.
		CHECK(m.load(bad, "good") && m.clipCount() == 2 && !m.clip("S_BEND") && !m.clip("T_NOD") && m.textureBytes() == 0);
		Pose rest;
		m.restPose(rest);
		gfx::FrameDesc fd {};
		fd.view = viewLH({0, 200, -600}, 0, 0);
		fd.eye = {0, 200, -600};
		fd.fovY = 1.0f;
		fd.zNear = 20.0f;
		fd.zFar = 5000.0f;
		fd.fogEnd = 5000.0f;
		const float light[3] = {1, 1, 1};
		gfx::frameBegin(fd);
		gfx::setPass(gfx::PASS_WORLD);
		m.draw(rest, {0, 0, 0}, 0, light);
		gfx::frameEnd();
		CHECK(gfx::softStats().skinnedPixels > 300);
		m.release();
		bad.close();
	}
	gfx::shutdown();
}

// The player's character in the game: steered, moved and animated the way
// the original does it, with the camera behind it and out of the walls.
static void testHero() {
	std::vector<BakeWorld> worlds(2);
	worlds[0].name = "yard";
	worlds[0].level = "YARD.ZEN";
	SrcScene& scene = worlds[0].scene;
	scene.materials.resize(1);
	scene.materials[0].name = "solid";
	// A floor of 80 x 80 m: brightly lit where x < 0, dim where x > 0.
	for (int z = -8; z < 8; ++z)
		for (int x = -8; x < 8; ++x) {
			const float x0 = x * 500.0f, z0 = z * 500.0f;
			const float p[4][3] = {{x0, 0, z0}, {x0 + 500, 0, z0}, {x0 + 500, 0, z0 + 500}, {x0, 0, z0 + 500}};
			addQuad(scene, p[0], p[1], p[2], p[3], 0);
			for (size_t t = scene.triangles.size() - 2; t < scene.triangles.size(); ++t)
				for (SrcVertex& v : scene.triangles[t].v) v.rgba[0] = v.rgba[1] = v.rgba[2] = x < 0 ? 250 : 60;
		}
	// A wall along x = 1000..1040, six metres high; a 30 cm step; a platform to fall off.
	addBlock(scene, 1000, 1040, -3000, 3000, 600);
	addBlock(scene, -600, 600, 1500, 2000, 30);
	addBlock(scene, -3400, -3000, -400, 400, 180);
	{
		// A slope of 65 degrees, too steep to stand on, rising towards -Z.
		const float tan65 = 2.14451f;
		const float sl[4][3] = {{-1800, 0, -2600}, {-2800, 0, -2600}, {-2800, 300 * tan65, -2900}, {-1800, 300 * tan65, -2900}};
		addQuad(scene, sl[0], sl[1], sl[2], sl[3], 0);
		// A gully between two such faces, its bottom falling 25 degrees towards +X.
		const float a0 = 400, a1 = 400 - 600 * 0.46631f, up = 250 * tan65;
		const float g1[4][3] = {{2000, a0, -1500}, {2600, a1, -1500}, {2600, a1 + up, -1250}, {2000, a0 + up, -1250}};
		const float g2[4][3] = {{2600, a1, -1500}, {2000, a0, -1500}, {2000, a0 + up, -1750}, {2600, a1 + up, -1750}};
		addQuad(scene, g1[0], g1[1], g1[2], g1[3], 0);
		addQuad(scene, g2[0], g2[1], g2[2], g2[3], 0);
	}
	scene.spawnPos[0] = 0;
	scene.spawnPos[1] = 175;
	scene.spawnPos[2] = 0;
	scene.hasSpawn = true;
	worlds[1].name = "cave";
	worlds[1].level = "CAVE.ZEN";
	buildSyntheticCave(worlds[1].scene);

	std::vector<SrcModel> models(1);
	buildSyntheticActor(models[0]);
	// A run of 520 cm/s: the character must move as fast as its animation carries it.
	for (SrcClip& clip : models[0].clips)
		if (clip.name == "S_RUNL")
			for (size_t i = 0; i < clip.samples.size(); i += clip.nodes.size()) clip.samples[i].pos[2] *= 520.0f / 480.0f;
	BakeOptions bo;
	bo.chunker.chunkSize = 2000.0f;
	bo.tex.maxSize = 32;
	bo.tex.quality = 0;
	bo.threads = 2;
	bo.quiet = true;
	const std::string path = gDir + "/hero.pak";
	{
		PakWriter writer;
		const BakeResult r = bakeWorlds(worlds, models, bo, writer);
		CHECK(r.models == 1 && r.report.find("== model 'hero' ==") != std::string::npos);
		writer.write(path);
	}
	// A character that cannot be baked does not take the worlds down with it.
	{
		std::vector<SrcModel> broken(1);
		buildChainModel(broken[0], 97); // more nodes than the format holds
		std::vector<BakeWorld> one(1);
		one[0] = worlds[0];
		PakWriter writer;
		const BakeResult r = bakeWorlds(one, broken, bo, writer);
		CHECK(r.models == 0 && r.report.find("WARNING: not baked: model 'chain': 97 nodes") != std::string::npos);
		CHECK(r.report.find("the game stays first-person") != std::string::npos);
		const std::string brokenPath = gDir + "/brokenhero.pak";
		writer.write(brokenPath);
		Pak p;
		CHECK(p.open(brokenPath.c_str()) && !p.find("meta/hero") && !p.find("model/chain") && p.find("meta/start"));
	}

	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr));
	app.world().preload(app.camera().pos);
	const Hero& hero = app.hero();
	{
		// A head turned to look at somebody: its way ahead turns by the yaw
		// about the model's up axis, then tips down by the pitch - with the
		// chest bent forward under it, too.
		const Model& m = hero.model();
		const int head = m.findNode(hashName("BIP01 HEAD")), chest = m.findNode(hashName("BIP01 SPINE1"));
		CHECK(head > 0 && chest > 0);
		if (head > 0 && chest > 0) {
			for (int bent = 0; bent < 2; ++bent) {
				Pose p;
				m.restPose(p);
				if (bent) p.rot[chest] = {std::sin(0.2f), 0, 0, std::cos(0.2f)};
				static Mat34 w[MODEL_MAX_NODES];
				m.worldTransforms(p, w);
				const Vec3 o0 = transformPoint(w[head], {0, 0, 0}), d0 = transformPoint(w[head], {0, 0, 10}) - o0;
				m.turnNode(p, head, 0.5f, 0.3f);
				m.worldTransforms(p, w);
				const Vec3 o1 = transformPoint(w[head], {0, 0, 0}), d1 = transformPoint(w[head], {0, 0, 10}) - o1;
				const Vec3 across = quatRotate(quatFromYaw(0.5f), {1, 0, 0});
				const Quat tilt = {across.x * std::sin(0.15f), across.y * std::sin(0.15f), across.z * std::sin(0.15f), std::cos(0.15f)};
				const Vec3 want = quatRotate(quatMul(tilt, quatFromYaw(0.5f)), d0);
				CHECK(length(o1 - o0) < 0.01f && length(d1 - want) < 0.02f);
				if (!bent) CHECK(std::fabs(d1.x - 10.0f * std::sin(0.5f) * std::cos(0.3f)) < 0.02f && d1.y < -2.9f);
			}
		}
	}
	Player& pl = app.player();
	CHECK(hero.loaded() && app.thirdPerson() && app.steersHero() && !app.cameraSet().fromGame);
	const Clip* const sRun = hero.clip("S_RUN");
	const Clip* const sRunL = hero.clip("S_RUNL");
	const Clip* const sWalk = hero.clip("S_WALK");
	const Clip* const sWalkL = hero.clip("S_WALKL");
	const Clip* const sJump = hero.clip("S_JUMP");
	CHECK(sRun && sRunL && sWalk && sWalkL && sJump);

	const float dt = 1.0f / 30.0f, deg = 0.0174533f;
	auto place = [&](float x, float z, float yawDegrees) {
		app.camera().yaw = yawDegrees * deg; // the character takes its heading from it
		app.teleport({x, 220.0f, z});
		for (int i = 0; i < 60; ++i) app.update(Input {}, dt);
	};
	auto angle = [](float a, float b) {
		float d = std::fmod(a - b, 6.2831853f);
		if (d > 3.14159265f) d -= 6.2831853f;
		if (d < -3.14159265f) d += 6.2831853f;
		return std::fabs(d);
	};
	auto step = [&](float forward, float turn = 0.0f, float strafe = 0.0f, bool jump = false) {
		Input in;
		in.moveZ = forward;
		in.moveX = turn;
		in.strafe = strafe;
		in.jump = jump;
		app.update(in, dt);
	};
	auto current = [&]() { return hero.animator().current(); };

	// ---- standing, and the camera's place ----
	// The project's own set-up: 2.5 m behind the hips (95 cm up) at 25 degrees,
	// the view tipped up by 20 and down again by 5.8 for the wide picture.
	place(-1500, 0, 0);
	CHECK(pl.grounded && hero.role() == Hero::IDLE && hero.body() == Hero::GROUNDED && current() == sRun && hero.speed() < 1.0f);
	CHECK(angle(hero.yaw(), 0.0f) < 1e-4f && std::fabs(hero.hipHeight() - 95.0f) < 0.01f);
	Vec3 cam = app.camera().pos;
	const float behind = 250.0f * std::cos(25.0f * deg), above = 250.0f * std::sin(25.0f * deg);
	CHECK(std::fabs(cam.x + 1500.0f) < 0.5f && std::fabs(cam.y - (95.0f + above)) < 1.0f && std::fabs(cam.z + behind) < 1.0f);
	CHECK(angle(app.camera().yaw, 0.0f) < 0.01f && std::fabs(app.camera().pitch - (-25.0f + 20.0f - 5.82f) * deg) < 0.01f);
	CHECK(app.heroVisible());
	app.render();
	gfx::SoftStats ss = gfx::softStats();
	CHECK(ss.skinnedTriangles == hero.model().triangles() && ss.skinnedPixels > 2500);
	{
		// In the lower half of the picture, feet and all, seen from behind.
		const Mat4 vp = mul(perspectiveLH(app.camera().fovY, float(gfx::SCREEN_W) / float(gfx::SCREEN_H), app.camera().zNear, app.camera().zFar),
		                    viewLH(cam, app.camera().yaw, app.camera().pitch));
		const Vec4 feet = transform(vp, {pl.feet.x, pl.feet.y, pl.feet.z, 1.0f}), head = transform(vp, {pl.feet.x, pl.feet.y + 180.0f, pl.feet.z, 1.0f});
		// (The feet are at the very edge, where the original has them too.)
		CHECK(std::fabs(feet.x / feet.w) < 0.02f && feet.y / feet.w > -1.08f && feet.y / feet.w < -0.7f);
		CHECK(head.y / head.w > -0.1f && head.y / head.w < 0.35f);
	}

	// ---- running: through the transitions, at the animation's pace ----
	step(1.0f);
	CHECK(current() == hero.clip("T_RUN_2_RUNL") && hero.role() == Hero::RUN);
	float early = 0;
	for (int i = 0; i < 5; ++i) step(1.0f);
	early = hero.speed();
	for (int i = 0; i < 50; ++i) step(1.0f);
	// Gathering pace first (the transition's root does), then the loop's 520 cm/s.
	CHECK(early > 30.0f && early < 350.0f && current() == sRunL && std::fabs(hero.speed() - 520.0f) < 6.0f);
	CHECK(angle(hero.yaw(), 0.0f) < 1e-4f && std::fabs(pl.feet.x + 1500.0f) < 0.5f && pl.feet.z > 800.0f);
	// The view widens when the character runs: the looked-after point trails it.
	cam = app.camera().pos;
	const float trail = pl.feet.z - cam.z;
	CHECK(trail > behind + 150.0f && trail < behind + 230.0f);
	// Letting go: the run is not cut off, it is run out - T_RUNL_2_RUN carries
	// the body another 48 cm - and then the character stands.
	const float zLet = pl.feet.z;
	step(0.0f);
	CHECK(current() == hero.clip("T_RUNL_2_RUN"));
	for (int i = 0; i < 45; ++i) step(0.0f);
	CHECK(current() == sRun && hero.role() == Hero::IDLE && hero.speed() < 2.0f);
	CHECK(pl.feet.z - zLet > 40.0f && pl.feet.z - zLet < 62.0f);
	// ... and the camera closes up again.
	for (int i = 0; i < 60; ++i) step(0.0f);
	CHECK(std::fabs(pl.feet.z - app.camera().pos.z - behind) < 3.0f);
	// A tap is a commitment, as in the original: into the run, and out of it again.
	const float zTap = pl.feet.z;
	step(1.0f);
	int busyFrames = 0;
	for (int i = 0; i < 40; ++i) {
		step(0.0f);
		busyFrames += current() != sRun;
	}
	CHECK(pl.feet.z - zTap > 100.0f && busyFrames >= 13 && current() == sRun);

	// ---- turning: the character, not the camera ----
	place(-1500, 0, 0);
	const Vec3 spot = pl.feet;
	for (int i = 0; i < 30; ++i) step(0.0f, 1.0f);
	CHECK(current() == hero.clip("T_RUNTURNR") && hero.role() == Hero::TURN);
	CHECK(std::fabs(hero.yaw() - 95.0f * deg) < 0.02f); // a second at 95 degrees a second
	CHECK(length(pl.feet - spot) < 0.5f);
	// The camera swings round after it: behind for a moment, then behind again.
	CHECK(angle(app.camera().yaw, hero.yaw()) > 0.05f);
	for (int i = 0; i < 60; ++i) step(0.0f);
	CHECK(current() == sRun && angle(app.camera().yaw, hero.yaw()) < 0.01f);
	cam = app.camera().pos;
	CHECK(std::fabs(cam.x - (spot.x - std::sin(hero.yaw()) * behind)) < 2.0f && std::fabs(cam.z - (spot.z - std::cos(hero.yaw()) * behind)) < 2.0f);
	for (int i = 0; i < 15; ++i) step(0.0f, -1.0f);
	CHECK(current() == hero.clip("T_RUNTURNL") && hero.yaw() < 95.0f * deg - 0.7f);
	// The second stick turns it too, faster (the original's mouse).
	place(-1500, 0, 0);
	for (int i = 0; i < 15; ++i) {
		Input in;
		in.lookX = 1.0f;
		app.update(in, dt);
	}
	CHECK(std::fabs(hero.yaw() - 90.0f * deg) < 0.02f);
	// Running and turning: a curve, leaning into it (at most 14.5 degrees).
	place(-1500, 0, 0);
	for (int i = 0; i < 45; ++i) step(1.0f, 1.0f);
	CHECK(current() == sRunL && hero.yaw() > 2.0f && hero.lean() > 0.08f && hero.lean() <= 0.2531f);
	CHECK(pl.feet.x > spot.x + 100.0f);
	for (int i = 0; i < 45; ++i) step(1.0f, -1.0f);
	CHECK(hero.lean() < -0.08f);
	for (int i = 0; i < 60; ++i) step(0.0f);
	CHECK(std::fabs(hero.lean()) < 1e-3f);

	// ---- back: a hop back, and another while the stick is held ----
	place(-1500, 0, 0);
	step(-1.0f);
	CHECK(current() == hero.clip("T_JUMPB") && hero.role() == Hero::BACK);
	for (int i = 0; i < 13; ++i) step(0.0f);
	CHECK(current() == sRun && std::fabs(pl.feet.z + 63.6f) < 1.0f); // the whole hop, though the stick was let go
	for (int i = 0; i < 60; ++i) step(-1.0f);
	CHECK(pl.feet.z < -63.0f - 220.0f);
	// ---- a step sideways: left is -X when facing +Z ----
	place(-1500, 0, 0);
	step(0.0f, 0.0f, -1.0f);
	CHECK(current() == hero.clip("T_RUNSTRAFEL") && hero.role() == Hero::STRAFE);
	for (int i = 0; i < 16; ++i) step(0.0f);
	CHECK(current() == sRun && std::fabs(pl.feet.x + 1555.4f) < 1.0f && std::fabs(pl.feet.z) < 1.0f);
	for (int i = 0; i < 40; ++i) step(0.0f, 0.0f, 1.0f);
	CHECK(pl.feet.x > -1555.0f + 100.0f && hero.clip("T_RUNSTRAFER") != nullptr);

	// ---- walking: a slight push of the stick, or walk mode ----
	place(-1500, 0, 0);
	for (int i = 0; i < 30; ++i) step(0.5f);
	CHECK(current() == sWalkL && hero.role() == Hero::WALK && std::fabs(hero.speed() - 150.0f) < 4.0f);
	// Pushed all the way: into the run, by the fade the script names (no animation for it).
	step(1.0f);
	CHECK(current() == sRunL);
	for (int i = 0; i < 20; ++i) step(1.0f);
	CHECK(std::fabs(hero.speed() - 520.0f) < 8.0f);
	for (int i = 0; i < 30; ++i) step(0.0f);
	{
		Input in;
		in.toggleWalk = true;
		app.update(in, dt);
		CHECK(hero.walking());
		for (int i = 0; i < 20; ++i) step(0.0f);
		CHECK(current() == sWalk);
		for (int i = 0; i < 30; ++i) step(1.0f);
		CHECK(current() == sWalkL && std::fabs(hero.speed() - 150.0f) < 4.0f);
		for (int i = 0; i < 20; ++i) step(0.0f);
		app.update(in, dt);
		CHECK(!hero.walking());
		for (int i = 0; i < 20; ++i) step(0.0f);
		CHECK(current() == sRun);
	}

	// ---- jumping: the take-off animation carries the body ----
	place(-1500, 0, 0);
	step(0.0f, 0.0f, 0.0f, true);
	CHECK(hero.body() == Hero::TAKEOFF && current() == hero.clip("T_STAND_2_JUMP") && hero.role() == Hero::JUMP);
	float top = 0, lowest = 0;
	int takeoffFrames = 0, airFrames = 0, landFrames = 0;
	bool hipsKept = true;
	float deepest = 95.0f;
	for (int i = 0; i < 70; ++i) {
		step(0.0f);
		top = std::max(top, pl.feet.y);
		lowest = std::min(lowest, pl.feet.y);
		takeoffFrames += hero.body() == Hero::TAKEOFF;
		airFrames += hero.body() == Hero::AIRBORNE;
		landFrames += current() == hero.clip("T_JUMP_2_STAND");
		// While the animation's height is in the body, the hips are drawn at
		// their own; the crouch before the leap stays in the pose.
		if (hero.body() == Hero::TAKEOFF) {
			const float hips = hero.pose().pos[0].y;
			deepest = std::min(deepest, hips);

			hipsKept = hipsKept && hips < 95.5f && (pl.feet.y < 1.0f || std::fabs(hips - 95.0f) < 0.5f);
		}
		if (hero.body() == Hero::AIRBORNE) CHECK(current() == sJump || current() == hero.clip("T_STAND_2_JUMP"));
	}
	// Crouch (the body stays on the ground), 74 cm up with the animation, on a
	// little by its own momentum, down again; 130 cm forward and what the flight adds.
	CHECK(takeoffFrames >= 12 && takeoffFrames <= 15 && airFrames > 8 && landFrames >= 4 && hipsKept);
	std::printf("  jump: crouch to %.1f cm (hips at rest: 95), top %.1f cm, landed at z = %.0f\n", double(deepest), double(top), double(pl.feet.z));
	CHECK(deepest < 90.0f && deepest > 60.0f);
	CHECK(top > 75.0f && top < 130.0f && lowest > -0.5f);
	CHECK(pl.grounded && hero.body() == Hero::GROUNDED && current() == sRun && std::fabs(pl.feet.y) < 0.3f);
	CHECK(pl.feet.z > 200.0f && pl.feet.z < 420.0f);
	// Out of the run: further and higher.
	place(-1500, 0, 0);
	for (int i = 0; i < 30; ++i) step(1.0f);
	const float zRun = pl.feet.z;
	step(1.0f, 0.0f, 0.0f, true);
	CHECK(current() == hero.clip("T_RUNL_2_JUMP") && hero.body() == Hero::TAKEOFF);
	top = 0;
	int f = 0;
	for (; f < 80 && !(pl.grounded && hero.body() == Hero::GROUNDED && f > 5); ++f) {
		step(1.0f);
		top = std::max(top, pl.feet.y);
	}
	CHECK(f < 60 && top > 85.0f && top < 150.0f && pl.feet.z - zRun > 350.0f);
	// Landing in a run: straight back into it (the script's fade), no stop.
	step(1.0f);
	CHECK(current() == sRunL);
	// Holding the button is one jump, not a row of them.
	place(-1500, 0, 0);
	int jumps = 0;
	for (int i = 0; i < 90; ++i) {
		const Hero::Body before = hero.body();
		step(0.0f, 0.0f, 0.0f, true);
		jumps += before != Hero::TAKEOFF && hero.body() == Hero::TAKEOFF;
	}
	CHECK(jumps == 1);

	// ---- falling: off a platform, with the pace it had ----
	app.camera().yaw = 90.0f * deg;
	app.teleport({-3200, 180.0f + 170.0f, 0});
	for (int i = 0; i < 30; ++i) step(0.0f);
	CHECK(pl.grounded && std::fabs(pl.feet.y - 180.0f) < 0.5f && angle(hero.yaw(), 90.0f * deg) < 1e-3f);
	int fallFrames = 0, landing = 0;
	float xLeft = 0, xLanded = 0;
	for (int i = 0; i < 90; ++i) {
		const bool wasGrounded = pl.grounded;
		step(i < 22 ? 1.0f : 0.0f); // let go in the air: it lands standing
		if (wasGrounded && !pl.grounded) xLeft = pl.feet.x;
		if (!wasGrounded && pl.grounded) xLanded = pl.feet.x;
		fallFrames += hero.role() == Hero::FALL && current() == hero.clip("S_FALLDN");
		landing += current() == hero.clip("T_FALLDN_2_STAND");
	}
	CHECK(fallFrames > 5 && landing >= 4 && pl.grounded && std::fabs(pl.feet.y) < 0.3f && current() == sRun);
	CHECK(xLeft > -3010.0f && xLanded - xLeft > 200.0f); // 0.6 s of falling at 520 cm/s
	// Landing in a run, there is no stop: straight on.
	app.camera().yaw = 90.0f * deg;
	app.teleport({-3200, 180.0f + 170.0f, 0});
	for (int i = 0; i < 30; ++i) step(0.0f);
	for (int i = 0; i < 60; ++i) step(1.0f);
	CHECK(pl.grounded && std::fabs(pl.feet.y) < 0.3f && current() == sRunL && pl.feet.x > -2400.0f);
	// A 30 cm step up and down is not a jump or a fall.
	place(0, 1200, 0);
	int offGround = 0;
	for (int i = 0; i < 70; ++i) {
		step(1.0f);
		offGround += hero.role() == Hero::JUMP || hero.role() == Hero::FALL;
	}
	CHECK(pl.feet.z > 2100.0f && offGround == 0 && current() == sRunL);
	// A teleport is not a sprint.
	app.teleport({-2000, 220, -2000});
	app.update(Input {}, dt);
	CHECK(hero.speed() < 1.0f && std::fabs(hero.feet().x + 2000.0f) < 1e-3f && current() == sRun);
	// A running jump at a slope too steep to stand on: it slides back down
	// and stands at its foot - it does not hang on the slope, pressed into
	// it by the jump's pace, sliding for ever.
	place(-2300, -1700, 180);
	{
		int frame = 0, highest = 0;
		float peak = 0;
		for (; frame < 150; ++frame) {
			step(1.0f, 0.0f, 0.0f, frame == 40);
			if (pl.feet.y > peak) {
				peak = pl.feet.y;
				highest = frame;
			}
		}
		int still = 0;
		for (int i = 0; i < 45; ++i) {
			step(0.0f);
			still += pl.grounded;
		}
		std::printf("       a running jump at the steep slope: up to %.0f cm (frame %d), then at z %.0f, %.0f up, %s, body %d\n", double(peak),
		            highest, double(pl.feet.z), double(pl.feet.y), pl.grounded ? "standing" : "sliding", int(hero.body()));
		CHECK(peak > 40.0f && pl.feet.z > -2700.0f && pl.feet.y < 50.0f && pl.grounded && still >= 30 && hero.body() == Hero::GROUNDED);
	}
	// ... and at the gully, from above: in the end it stands, on its feet.
	app.camera().yaw = 180.0f * deg;
	app.teleport({2100, 1000.0f + 170.0f, -1100});
	app.update(Input {}, dt);
	pl.feet = {2100, 1000, -1100};
	pl.grounded = false;
	{
		int frames = 0;
		for (; frames < 240; ++frames) step(frames < 90 ? 1.0f : 0.0f, 0.0f, 0.0f, frames == 5);
		int still = 0;
		for (int i = 0; i < 30; ++i) {
			step(0.0f);
			still += pl.grounded;
		}
		std::printf("       into the gully: at %.0f %.0f %.0f, %s, body %d\n", double(pl.feet.x), double(pl.feet.y), double(pl.feet.z),
		            pl.grounded ? "standing" : "sliding", int(hero.body()));
		CHECK(pl.grounded && still == 30 && hero.body() == Hero::GROUNDED);
	}

	// ---- light: like the ground it stands on, tinted by the time of day ----
	place(-1500, 0, 0);
	const float bright = hero.light()[1];
	place(500, 0, 0);
	const float dim = hero.light()[1];
	CHECK(bright > 1.0f && bright < 1.2f && std::fabs(dim - 60.0f / 255.0f * 1.15f) < 0.02f);
	app.environment().timeOfDay = 1.0f; // night
	place(-1500, 0, 0);
	CHECK(hero.light()[1] < bright * 0.6f && hero.light()[1] >= 0.22f - 1e-3f);
	app.environment().timeOfDay = 13.0f;

	// ---- the camera and the wall at x = 1000 ----
	// Facing away from it with two metres of room: the camera stays this side.
	place(800, 0, -90);
	cam = app.camera().pos;
	CHECK(cam.x < 1000.0f - 23.0f && cam.x > 930.0f && app.heroVisible());
	// With the back to the wall there is no room behind: the view from above,
	// as the game does it - the character stays in the picture.
	place(960, 0, -90);
	cam = app.camera().pos;
	CHECK(app.heroVisible() && cam.x < 1000.0f - 23.0f && cam.y > 95.0f + 135.0f && app.camera().pitch < -0.8f);
	app.render();
	CHECK(gfx::softStats().skinnedPixels > 500);
	// Running away from it, and swinging round next to it: never inside the wall, and no jumps.
	bool inWall = false;
	float worstJump = 0;
	Vec3 last = app.camera().pos;
	for (int i = 0; i < 60; ++i) {
		step(1.0f);
		cam = app.camera().pos;
		inWall = inWall || cam.x > 1000.0f - 23.0f;
		if (i > 0) worstJump = std::max(worstJump, length(cam - last) - 520.0f * dt);
		last = cam;
	}
	CHECK(!inWall && app.heroVisible() && worstJump < 60.0f);
	place(900, 0, 0);
	for (int i = 0; i < 30; ++i) {
		step(0.0f, -1.0f); // turning left: the camera swings towards +X, where the wall is
		inWall = inWall || app.camera().pos.x > 1000.0f - 23.0f;
	}
	CHECK(!inWall && hero.yaw() < -1.5f);

	// ---- the view: tilting, first person, flying ----
	place(-1500, 0, 0);
	const float camY = app.camera().pos.y;
	for (int i = 0; i < 20; ++i) {
		Input in;
		in.lookY = 1.0f; // looking up: the camera sinks
		app.update(in, dt);
	}
	for (int i = 0; i < 30; ++i) step(0.0f);
	CHECK(app.camera().pos.y < camY - 60.0f && angle(hero.yaw(), 0.0f) < 1e-4f && app.camera().pitch > -0.19f + 0.3f);
	Input toggle;
	toggle.toggleCamera = true;
	app.update(toggle, dt);
	CHECK(!app.thirdPerson() && !app.heroVisible() && std::fabs(app.camera().pos.y - pl.eye().y) < 1e-3f &&
	      std::fabs(app.camera().pos.z - pl.feet.z) < 1e-3f && app.camera().pitch == 0.0f);
	app.render();
	CHECK(gfx::softStats().skinnedTriangles == 0);
	// Through its eyes the character is steered all the same.
	for (int i = 0; i < 30; ++i) step(1.0f, 1.0f);
	CHECK(pl.feet.z > 100.0f && angle(app.camera().yaw, hero.yaw()) < 1e-4f && hero.yaw() > 1.5f && app.steersHero());
	app.update(toggle, dt);
	CHECK(app.thirdPerson() && app.heroVisible());
	// Flying is a free camera again; landing puts the character where it is, facing where it looks.
	Input fly;
	fly.toggleFly = true;
	app.update(fly, dt);
	CHECK(pl.flying && !app.steersHero() && !app.heroVisible() && std::fabs(app.camera().pos.z - pl.feet.z) < 1e-3f);
	const float flyYaw = app.camera().yaw;
	for (int i = 0; i < 10; ++i) {
		Input in;
		in.lookX = 1.0f;
		in.moveX = 1.0f; // sideways, as a free camera goes
		app.update(in, dt);
	}
	CHECK(std::fabs(app.camera().yaw - flyYaw - 0.6f) < 0.01f);
	app.update(fly, dt);
	for (int i = 0; i < 40; ++i) step(0.0f);
	CHECK(!pl.flying && app.steersHero() && pl.grounded && angle(hero.yaw(), flyYaw + 0.6f) < 0.01f && app.heroVisible());
	CHECK(length(hero.feet() - pl.feet) < 1e-3f);

	// ---- where the game is left, it goes on: place and heading ----
	place(-1200, 300, 135);
	const std::string statePath = gDir + "/hero-state.bin";
	CHECK(app.saveState(statePath.c_str()));
	place(0, 0, 0);
	CHECK(app.loadState(statePath.c_str()));
	for (int i = 0; i < 30; ++i) step(0.0f);
	CHECK(std::fabs(pl.feet.x + 1200.0f) < 1.0f && std::fabs(pl.feet.z - 300.0f) < 1.0f && angle(hero.yaw(), 135.0f * deg) < 1e-3f);

	// ---- water: wading is a walk, deep water carries the body ----
	Input next;
	next.nextWorld = true;
	app.update(next, dt);
	CHECK(app.world().indoor() && hero.loaded());
	app.world().preload(app.camera().pos);
	for (int i = 0; i < 20; ++i) step(0.0f);
	int wadeWalk = 0, wadeFrames = 0, swimFrames = 0;
	for (int i = 0; i < 400 && swimFrames < 60; ++i) {
		step(1.0f);
		if (pl.swimming) {
			++swimFrames;
		} else if (pl.waterDepth > pl.config.wadeDepth + 5.0f && pl.grounded) {
			++wadeFrames;
			wadeWalk += hero.role() == Hero::WALK;
		}
	}
	CHECK(wadeFrames > 5 && wadeWalk >= wadeFrames - 3 && swimFrames == 60);
	CHECK(hero.body() == Hero::SWIMMING && hero.role() == Hero::SWIM && current() == hero.clip("S_SWIMF"));
	CHECK(std::fabs(hero.speed() - 260.0f) < 8.0f);
	// The hips at their own height above the floating body: 30 cm under the surface.
	const float surface = pl.feet.y + pl.waterDepth;
	CHECK(std::fabs(pl.waterDepth - pl.config.swimDepth) < 2.0f && std::fabs(pl.feet.y + hero.pose().pos[0].y - (surface - 30.0f)) < 2.0f);
	app.render();
	CHECK(gfx::softStats().skinnedPixels > 200);
	for (int i = 0; i < 30; ++i) step(0.0f);
	CHECK(pl.swimming && hero.role() == Hero::SWIM_IDLE && current() == hero.clip("S_SWIM") && hero.speed() < 5.0f);
	const float zSwim = pl.feet.z;
	for (int i = 0; i < 30; ++i) step(-1.0f);
	CHECK(current() == hero.clip("S_SWIMB") && pl.feet.z < zSwim - 80.0f);

	app.shutdown();
	CHECK(gfx::memStats().meshCount == 0 && gfx::memStats().textureCount == 0);

	// ---- the game's own camera set-up, carried in the archive ----
	{
		PakWriter writer;
		std::vector<BakeWorld> one(1);
		one[0] = worlds[0];
		bakeWorlds(one, models, bo, writer);
		CameraSet set = defaultCameraSet();
		set.fromGame = 1;
		set.mode[CAMERA_NORMAL] = {4.0f, 2.0f, 10.0f, 40.0f, 0.0f, 89.0f, 0.0f, -90.0f, 90.0f, 10.0f, 0.0f, 0.0f, 20.0f, 0.0f, 80.0f, 2.0f};
		const u8* bytes = reinterpret_cast<const u8*>(&set);
		writer.add("meta/camera", ASSET_META, std::vector<u8>(bytes, bytes + sizeof(set)));
		const std::string camPath = gDir + "/camera.pak";
		writer.write(camPath);
		App a;
		CHECK(a.init(camPath.c_str(), nullptr));
		a.world().preload(a.camera().pos);
		for (int i = 0; i < 60; ++i) a.update(Input {}, dt);
		CHECK(a.cameraSet().fromGame == 1);
		// Four metres from the hips (and 20 cm above them: the target offset), at 40 degrees.
		const Vec3 c = a.camera().pos, feet = a.player().feet;
		CHECK(std::fabs(c.y - (feet.y + 95.0f + 20.0f + 400.0f * std::sin(40.0f * deg))) < 1.0f);
		CHECK(std::fabs(feet.z - c.z - 400.0f * std::cos(40.0f * deg)) < 1.0f);
		CHECK(std::fabs(a.camera().pitch - (-40.0f + 10.0f - 5.82f) * deg) < 0.01f);
		a.shutdown();

		// Nonsense in its place is not believed.
		PakWriter bad;
		bakeWorlds(one, models, bo, bad);
		set.mode[CAMERA_NORMAL].bestRange = 4000.0f;
		bad.add("meta/camera", ASSET_META, std::vector<u8>(bytes, bytes + sizeof(set)));
		const std::string badPath = gDir + "/badcamera.pak";
		bad.write(badPath);
		App b;
		CHECK(b.init(badPath.c_str(), nullptr));
		CHECK(b.cameraSet().fromGame == 0 && b.cameraSet().mode[CAMERA_NORMAL].bestRange == 2.5f);
		b.shutdown();
	}

	// ---- take-off animations that do not lift the body: thrown instead ----
	// (Whether the game's own do is not known before its files are baked.)
	{
		std::vector<SrcModel> flat = models;
		for (SrcClip& clip : flat[0].clips)
			if (clip.name == "T_STAND_2_JUMP" || clip.name == "T_RUNL_2_JUMP")
				for (size_t i = 0; i < clip.samples.size(); i += clip.nodes.size()) clip.samples[i].pos[1] = clip.samples[0].pos[1];
		PakWriter writer;
		std::vector<BakeWorld> one(1);
		one[0] = worlds[0];
		const BakeResult r = bakeWorlds(one, flat, bo, writer);
		CHECK(r.report.find("T_STAND_2_JUMP 0.48s 239cm/s,") != std::string::npos); // no "up ... cm"
		const std::string flatPath = gDir + "/flatjump.pak";
		writer.write(flatPath);
		App a;
		CHECK(a.init(flatPath.c_str(), nullptr));
		a.world().preload(a.camera().pos);
		Player& p = a.player();
		auto go = [&](float forward, bool jump) {
			Input in;
			in.moveZ = forward;
			in.jump = jump;
			a.update(in, dt);
		};
		for (int variant = 0; variant < 2; ++variant) {
			a.camera().yaw = 0;
			a.teleport({-1500, 220.0f, 0});
			for (int i = 0; i < 60; ++i) go(0.0f, false);
			const float pace = variant ? 1.0f : 0.0f;
			for (int i = 0; i < 30; ++i) go(pace, false);
			const float z0 = p.feet.z;
			go(pace, true);
			CHECK(a.hero().body() == Hero::AIRBORNE && a.hero().role() == Hero::JUMP);
			float peak = 0;
			int frames = 0;
			for (; frames < 90 && !(p.grounded && frames > 3); ++frames) {
				go(pace, false);
				peak = std::max(peak, p.feet.y);
			}
			// 408 cm/s up against 981 cm/s^2: 85 cm, 0.83 s - and forward at
			// the pace of the take-off animation (239 cm/s; out of the run 480).
			std::printf("  thrown jump %s: top %.1f cm, %d frames, %.0f cm forward\n", variant ? "from a run" : "from standing", double(peak),
			            frames, double(p.feet.z - z0));
			CHECK(peak > 90.0f && peak < 125.0f && frames > 20 && frames < 36);
			CHECK(variant ? p.feet.z - z0 > 350.0f : (p.feet.z - z0 > 150.0f && p.feet.z - z0 < 230.0f));
			for (int i = 0; i < 40; ++i) go(0.0f, false);
			CHECK(a.hero().body() == Hero::GROUNDED && a.hero().animator().current() == a.hero().clip("S_RUN"));
		}
		a.shutdown();
	}

	// ---- an archive without a character: a free first-person camera ----
	{
		PakWriter writer;
		std::vector<BakeWorld> one(1);
		one[0] = worlds[0];
		bakeWorlds(one, bo, writer);
		const std::string plain = gDir + "/nohero.pak";
		writer.write(plain);
		App a;
		CHECK(a.init(plain.c_str(), nullptr));
		a.world().preload(a.camera().pos);
		a.setThirdPerson(true);
		for (int i = 0; i < 30; ++i) a.update(Input {}, dt);
		CHECK(!a.hero().loaded() && !a.thirdPerson() && !a.steersHero() && !a.heroVisible() && std::fabs(a.camera().pos.y - 170.0f) < 1.0f);
		// The stick moves sideways and the view turns with the other one, as before.
		const float x0 = a.player().feet.x;
		for (int i = 0; i < 30; ++i) {
			Input in;
			in.moveX = 1.0f;
			in.lookX = 0.5f;
			a.update(in, dt);
		}
		CHECK(a.player().feet.x > x0 + 300.0f && std::fabs(a.camera().yaw - 0.9f) < 0.01f);
		a.render();
		CHECK(gfx::softStats().skinnedTriangles == 0);
		a.shutdown();
	}
	gfx::shutdown();
}

// ---- sound ----

// A mono IMA ADPCM file of `samples`, 256-byte blocks (for the decoder's test).
static std::vector<u8> imaWav(const std::vector<i16>& samples, u32 rate) {
	static const int step[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
	                             31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
	                             130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
	                             544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
	                             2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
	                             9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
	static const int adjust[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
	const u32 block = 256, perBlock = (block - 4) * 2 + 1;
	std::vector<u8> data;
	for (size_t at = 0; at + perBlock <= samples.size(); at += perBlock) {
		int predictor = samples[at], index = 0;
		data.push_back(u8(predictor & 0xFF));
		data.push_back(u8((predictor >> 8) & 0xFF));
		data.push_back(u8(index));
		data.push_back(0);
		for (u32 n = 0; n < (block - 4) * 2; n += 2) {
			u8 byte = 0;
			for (u32 half = 0; half < 2; ++half) {
				const int target = samples[at + 1 + n + half];
				int diff = target - predictor, code = 0;
				if (diff < 0) {
					code = 8;
					diff = -diff;
				}
				int st = step[index], delta = st >> 3;
				if (diff >= st) { code |= 4; diff -= st; delta += st; }
				st >>= 1;
				if (diff >= st) { code |= 2; diff -= st; delta += st; }
				st >>= 1;
				if (diff >= st) { code |= 1; delta += st; }
				predictor = std::clamp(predictor + ((code & 8) ? -delta : delta), -32768, 32767);
				index = std::clamp(index + adjust[code & 7], 0, 88);
				byte = u8(byte | (code << (half * 4)));
			}
			data.push_back(byte);
		}
	}
	std::vector<u8> w(44 + data.size());
	const auto put32 = [&](size_t at, u32 v) { for (int i = 0; i < 4; ++i) w[at + size_t(i)] = u8(v >> (8 * i)); };
	const auto put16 = [&](size_t at, u16 v) { w[at] = u8(v); w[at + 1] = u8(v >> 8); };
	std::memcpy(w.data(), "RIFF", 4);
	put32(4, u32(36 + data.size()));
	std::memcpy(w.data() + 8, "WAVEfmt ", 8);
	put32(16, 16);
	put16(20, 0x11);
	put16(22, 1);
	put32(24, rate);
	put32(28, rate * block / perBlock);
	put16(32, u16(block));
	put16(34, 4);
	std::memcpy(w.data() + 36, "data", 4);
	put32(40, u32(data.size()));
	std::copy(data.begin(), data.end(), w.begin() + 44);
	return w;
}

// Lets the sound engine's loading thread and the host's clock run until `count` sounds have started.
static bool soundWait(Sound& sound, HostAudio& audio, u32 count) {
	// The game's clock must not run ahead of the loading thread: a sample that has waited longer than
	// WAIT_FOR_SAMPLE of game time counts as late and is dropped, and on a slow or busy machine the thread needs
	// longer than the 60 ms of real time that 10 ms steps would allow. So the steps are tiny.
	for (int i = 0; i < 3000; ++i) {
		sound.update(0.0002f, 12.0f);
		audio.advance(0.01f);
		if (audio.startedCount() >= count) return true;
		usleep(2000);
	}
	return false;
}

static void testSpeech() {
	namespace fs = std::filesystem;
	SrcDialogue dialogue;
	buildSyntheticDialogue(dialogue);
	std::vector<std::string> names;
	for (const auto& l : dialogue.lines) names.push_back(l.first);
	PakWriter pak;
	const SpeechBakeResult r = bakeSpeech(names, buildSyntheticSound, 11025, 2, pak, true);
	CHECK(r.lines == names.size() && r.baked == 2 && r.unusable.empty());
	CHECK(pak.items().size() == 2);
	// The encoder and the decoder: a tone comes back close to what went in.
	{
		SrcSound tone;
		tone.rate = 11025;
		for (int i = 0; i < 4000; ++i) tone.samples.push_back(i16(std::lround(9000.0 * std::sin(double(i) * 0.07))));
		const std::vector<u8> blob = encodeSpeech(tone, true); // (IMA: the default is 16-bit)
		SoundHeader h;
		std::memcpy(&h, blob.data(), sizeof(h));
		CHECK(speechValid(h, u32(blob.size())) && blob.size() == sizeof(SoundHeader) + 4 + 2000);
		std::vector<i16> back(tone.samples.size());
		imaDecode(blob.data() + sizeof(h), h.frames, back.data());
		double err = 0, sig = 0;
		for (size_t i = 0; i < back.size(); ++i) {
			err += double(back[i] - tone.samples[i]) * double(back[i] - tone.samples[i]);
			sig += double(tone.samples[i]) * double(tone.samples[i]);
		}
		CHECK(err < sig * 0.02);
	}
	const std::string dir = gDir + "/speech_game";
	fs::create_directories(dir);
	pak.write(dir + "/speech.pak");
	// Full quality (the default): 16-bit samples as recorded, nothing resampled.
	{
		PakWriter full;
		const SpeechBakeResult fr = bakeSpeech(names, buildSyntheticSound, 48000, 2, full);
		CHECK(fr.baked == 2 && full.items().size() == 2);
		fs::create_directories(dir + "/full");
		full.write(dir + "/full/speech.pak");
		HostAudio a2;
		Speech sp2;
		CHECK(sp2.init((dir + "/full/speech.pak").c_str(), &a2));
		const float len = sp2.play(hashName("DIA_Thorus_Job_15_00"), 1.0f);
		CHECK(len > 0.59f && len < 0.61f && a2.last().rate == 44100 && a2.last().frames == 26460);
		int pk = 0;
		for (u32 i = 0; i < a2.last().frames; ++i) pk = std::max(pk, std::abs(int(a2.last().samples[i])));
		CHECK(pk > 10000);
	}
	// In pieces (the console cannot seek past 2 GiB of a file): one recording to an archive, found in whichever it is in.
	{
		std::vector<std::unique_ptr<PakWriter>> shards;
		const SpeechBakeResult sh = bakeSpeechShards(names, buildSyntheticSound, 48000, 2, shards, false, 1000, 4);
		CHECK(sh.baked == 2 && shards.size() == 2 && shards[0]->items().size() == 1 && shards[1]->items().size() == 1);
		fs::create_directories(dir + "/shards");
		shards[0]->write(dir + "/shards/speech.pak");
		shards[1]->write(dir + "/shards/speech2.pak");
		HostAudio a3;
		Speech sp3;
		CHECK(sp3.init((dir + "/shards/speech.pak").c_str(), &a3));
		CHECK(sp3.play(hashName("DIA_Thorus_Hello_09_00"), 1.0f) > 1.0f);
		CHECK(sp3.play(hashName("DIA_Thorus_Job_15_00"), 1.0f) > 0.5f);
		CHECK(a3.startedCount() == 2 && sp3.missing() == 0);
		// No room left: the recording is reported, not dropped silently.
		std::vector<std::unique_ptr<PakWriter>> tight;
		const SpeechBakeResult no = bakeSpeechShards(names, buildSyntheticSound, 48000, 2, tight, false, 1000, 1);
		CHECK(no.baked == 1 && no.unusable.size() == 1);
		// A file that is there but is no archive: refused, and the report says why.
		{
			FILE* f = std::fopen((dir + "/shards/broken.pak").c_str(), "wb");
			if (f) {
				std::fputs("not an archive", f);
				std::fclose(f);
			}
			HostAudio a4;
			Speech sp4;
			CHECK(!sp4.init((dir + "/shards/broken.pak").c_str(), &a4));
			char report[512] = "";
			FILE* rf = fmemopen(report, sizeof(report) - 1, "w");
			if (rf) {
				sp4.writeReport(rf);
				std::fclose(rf);
			}
			CHECK(std::strstr(report, "cannot be opened") != nullptr);
		}
	}

	HostAudio audio;
	Speech speech;
	CHECK(!speech.init((dir + "/nothing.pak").c_str(), &audio));
	CHECK(speech.init((dir + "/speech.pak").c_str(), &audio) && speech.ready());
	speech.setVolume(0.5f);
	const float hello = speech.play(hashName("DIA_Thorus_Hello_09_00"), 1.0f);
	CHECK(hello > 1.95f && hello < 2.05f);
	CHECK(audio.startedCount() == 1 && audio.last().channel == u32(Speech::CHANNEL) && !audio.last().loop);
	CHECK(audio.last().rate == 11025 && audio.last().frames > 21900 && audio.last().frames < 22200);
	CHECK(audio.leftOf(Speech::CHANNEL) > 0.49f && audio.leftOf(Speech::CHANNEL) < 0.51f);
	int peak = 0;
	for (u32 i = 0; i < audio.last().frames; ++i) peak = std::max(peak, std::abs(int(audio.last().samples[i])));
	CHECK(peak > 6000);
	CHECK(speech.playing());
	audio.advance(1.0f);
	CHECK(speech.playing());
	speech.stop();
	CHECK(!speech.playing());
	// Another line replaces it; one without a recording is nothing; far away is muted but still has its length.
	const float job = speech.play(hashName("DIA_Thorus_Job_15_00"), 0.5f);
	CHECK(job > 0.55f && job < 0.65f && audio.startedCount() == 2);
	CHECK(audio.leftOf(Speech::CHANNEL) > 0.24f && audio.leftOf(Speech::CHANNEL) < 0.26f);
	CHECK(speech.play(hashName("DIA_Jackal_Hello_15_00"), 1.0f) == 0.0f && speech.missing() == 1);
	const float quiet = speech.play(hashName("DIA_Thorus_Hello_09_00"), 0.0f);
	CHECK(quiet > 1.95f && audio.startedCount() == 2 && !speech.playing());
	CHECK(audio.liveBlocks() <= 2);
	speech.shutdown();
	CHECK(audio.liveBlocks() == 0 && !speech.ready());
	// What the world says has a reader and a channel of its own: from the side it comes from, and a line that is
	// sounding is not cut by the next, which is only measured (its speaker still waits it out).
	{
		HostAudio wa;
		Speech world(Speech::WORLD_CHANNEL);
		CHECK(world.init((dir + "/speech.pak").c_str(), &wa));
		CHECK(world.play(hashName("DIA_Thorus_Hello_09_00"), 1.0f, -1.0f, false) > 1.95f);
		CHECK(wa.startedCount() == 1 && wa.last().channel == u32(Speech::WORLD_CHANNEL));
		CHECK(wa.leftOf(Speech::WORLD_CHANNEL) > 0.7f && wa.rightOf(Speech::WORLD_CHANNEL) < 0.05f);
		const float second = world.play(hashName("DIA_Thorus_Job_15_00"), 1.0f, 0.0f, false);
		CHECK(second > 0.55f && second < 0.65f && wa.startedCount() == 1 && world.playing());
		wa.advance(5.0f);
		CHECK(!world.playing());
		CHECK(world.play(hashName("DIA_Thorus_Job_15_00"), 1.0f, 1.0f, false) > 0.55f && wa.startedCount() == 2);
		CHECK(wa.rightOf(Speech::WORLD_CHANNEL) > 0.7f && wa.leftOf(Speech::WORLD_CHANNEL) < 0.05f);
		world.shutdown();
		CHECK(wa.liveBlocks() == 0);
	}
}

static void testMusic() {
	namespace fs = std::filesystem;
	const std::string dir = gDir + "/music_wavs";
	fs::create_directories(dir);
	// Three renders of 2.5 s at 22.05 kHz; the files are named as the scripts' themes name them.
	const auto writeTone = [&](const char* name, double freq) {
		std::vector<i16> tone(55125);
		for (size_t i = 0; i < tone.size(); ++i) tone[i] = i16(std::lround(8000.0 * std::sin(double(i) * freq)));
		const std::vector<u8> wav = makeWav(tone, 22050);
		std::ofstream(dir + "/" + name, std::ios::binary).write(reinterpret_cast<const char*>(wav.data()), std::streamsize(wav.size()));
	};
	writeTone("TEST_DAY_STD.wav", 0.05);
	writeTone("test_ngt_std.WAV", 0.08);
	writeTone("TEST_DAY_FGT.wav", 0.11);
	writeTone("TEST_MENU.wav", 0.2);

	SrcScript script;
	buildSyntheticScript(script);
	std::vector<BakeWorld> worlds(1);
	worlds[0].name = "world";
	worlds[0].level = "WORLD.ZEN";
	SynthOptions so;
	so.cells = 48;
	so.huts = 6;
	so.trees = 20;
	buildSyntheticScene(so, worlds[0].scene);
	std::vector<SrcModel> models(1);
	buildSyntheticActor(models[0]);
	BakeScripts scripts;
	scripts.script = &script;
	scripts.looks = LookSource {buildSyntheticRig, buildSyntheticBody, buildSyntheticHead, buildSyntheticItem, buildSyntheticParticleTexture, buildSyntheticOverlay, buildSyntheticOverlays, nullptr};
	scripts.soundFile = buildSyntheticSound;
	SrcScript sfxScript, musicScript;
	buildSyntheticSfxScript(sfxScript);
	buildSyntheticMusicScript(musicScript);
	scripts.sfx = &sfxScript;
	scripts.music = &musicScript;
	PakWriter musicPak;
	scripts.musicPak = &musicPak;
	BakeOptions bo;
	bo.chunker.chunkSize = 3000.0f;
	bo.tex.maxSize = 32;
	bo.tex.quality = 0;
	bo.threads = 2;
	bo.quiet = true;
	bo.musicDir = dir;
	PakWriter writer;
	bakeWorlds(worlds, models, scripts, bo, writer);
	CHECK(writer.has("world/world/music"));
	CHECK(musicPak.has("music/test_day_std") && musicPak.has("music/test_ngt_std") && musicPak.has("music/test_day_fgt") && musicPak.has("music/test_menu"));
	CHECK(!writer.has("music/test_day_std")); // (the tracks are not in data.pak)
	const std::string sub = gDir + "/music_game";
	fs::create_directories(sub);
	writer.write(sub + "/data.pak");
	musicPak.write(sub + "/music.pak");

	gfx::init();
	{
		HostAudio audio;
		App app;
		app.setAudio(&audio);
		CHECK(app.init((sub + "/data.pak").c_str(), nullptr));
		Music& music = app.music();
		CHECK(music.ready());
		const auto run = [&](float seconds, Vec3 at, float hours, MusicSituation sit) {
			for (float t = 0; t < seconds; t += 0.05f) {
				music.update(0.05f, at, hours, sit);
				audio.advance(0.05f);
				usleep(300);
			}
		};
		run(4.0f, {0, 0, 0}, 12.0f, MUSIC_CALM);
		CHECK(std::string(music.zone()) == "TEST" && std::string(music.theme()) == "TEST_DAY_STD");
		CHECK(audio.streamIsOpen() && audio.streamRate() == 22050 && audio.streamChannels() == 1);
		CHECK(audio.streamQueued() >= 4); // 2.5 s of track, looping: more than it holds is queued
		CHECK(music.level() > 0.99f);
		// (Day-theme volume is unset in the scripts: full; master 0.6.)
		CHECK(std::fabs(audio.streamLevel() - 0.6f) < 0.02f);
		// Night: fade out, the other track fades in.
		run(1.0f, {0, 0, 0}, 23.0f, MUSIC_CALM);
		CHECK(music.level() < 0.9f || std::string(music.theme()) == "TEST_NGT_STD");
		run(4.0f, {0, 0, 0}, 23.0f, MUSIC_CALM);
		CHECK(std::string(music.theme()) == "TEST_NGT_STD" && music.level() > 0.99f);
		// A fight has a theme of its own - by day (this world has none for the night: the calm one goes on, as in
		// OpenGothic's search, which tries the hour before the situation).
		run(5.0f, {0, 0, 0}, 23.0f, MUSIC_FIGHT);
		CHECK(std::string(music.theme()) == "TEST_NGT_STD");
		run(5.0f, {0, 0, 0}, 12.0f, MUSIC_FIGHT);
		CHECK(std::string(music.theme()) == "TEST_DAY_FGT");
		// A threat (nothing in the hand) has no theme here either: the fight's is the next.
		run(5.0f, {0, 0, 0}, 12.0f, MUSIC_CALM);
		CHECK(std::string(music.theme()) == "TEST_DAY_STD");
		run(5.0f, {0, 0, 0}, 12.0f, MUSIC_THREAT);
		CHECK(std::string(music.theme()) == "TEST_DAY_FGT");
		// Day is from 4:00 to 21:00.
		run(5.0f, {0, 0, 0}, 3.5f, MUSIC_CALM);
		CHECK(std::string(music.theme()) == "TEST_NGT_STD");
		run(5.0f, {0, 0, 0}, 4.5f, MUSIC_CALM);
		CHECK(std::string(music.theme()) == "TEST_DAY_STD");
		run(5.0f, {0, 0, 0}, 20.5f, MUSIC_CALM);
		CHECK(std::string(music.theme()) == "TEST_DAY_STD");
		run(5.0f, {0, 0, 0}, 21.5f, MUSIC_CALM);
		CHECK(std::string(music.theme()) == "TEST_NGT_STD");
		// The menu has a theme of its own, and the world's comes back after it.
		music.setSystemTheme("SYS_MENU");
		run(5.0f, {0, 0, 0}, 12.0f, MUSIC_CALM);
		CHECK(std::string(music.theme()) == "SYS_MENU" && audio.streamIsOpen());
		music.setSystemTheme(nullptr);
		run(5.0f, {0, 0, 0}, 12.0f, MUSIC_CALM);
		CHECK(std::string(music.theme()) == "TEST_DAY_STD");
		// Volume 0: silence, the stream closes.
		music.setVolume(0.0f);
		run(3.0f, {0, 0, 0}, 12.0f, MUSIC_FIGHT);
		CHECK(!audio.streamIsOpen() && std::string(music.theme()).empty());
		music.setVolume(0.6f);
		run(4.0f, {0, 0, 0}, 12.0f, MUSIC_CALM);
		CHECK(audio.streamIsOpen() && std::string(music.theme()) == "TEST_DAY_STD");
		app.shutdown();
		CHECK(audio.liveBlocks() == 0);
	}
	// Without music.pak: no music, no harm.
	{
		fs::remove(sub + "/music.pak");
		HostAudio audio;
		App app;
		app.setAudio(&audio);
		CHECK(app.init((sub + "/data.pak").c_str(), nullptr));
		CHECK(!app.music().ready());
		app.shutdown();
	}
}

static void testSound() {
	// ---- the WAV reader, the resampler, the blob ----
	std::vector<i16> tone(8820);
	for (size_t i = 0; i < tone.size(); ++i) tone[i] = i16(std::lround(10000.0 * std::sin(double(i) * 0.05)));
	{
		SrcSound s;
		std::string why;
		CHECK(decodeWav(makeWav(tone, 44100), s, why) && s.rate == 44100 && s.samples == tone);
		resampleSound(s, 22050);
		CHECK(s.rate == 22050 && s.samples.size() == tone.size() / 2);
		// (A slow tone survives the averaging: about the same shape at half the length.)
		CHECK(std::abs(int(s.samples[1000]) - int(tone[2000])) < 400);
		const std::vector<u8> blob = encodeSound(s);
		SoundHeader h;
		std::memcpy(&h, blob.data(), sizeof(h));
		CHECK(blob.size() == soundBytes(h) && soundValid(h, u32(blob.size())));
		CHECK(!soundValid(h, u32(blob.size()) - 2));
		CHECK(soundEntryHash("STEP_STONE.WAV") == hashName("sfx/step_stone"));
		SrcSound unchanged = s;
		resampleSound(unchanged, 44100);
		CHECK(unchanged.samples == s.samples);
		CHECK(!decodeWav({1, 2, 3}, s, why) && !why.empty());
		// IMA ADPCM: a lossy copy of the tone.
		std::vector<i16> longTone(2100); // (four 505-sample blocks and a remainder)
		for (size_t i = 0; i < longTone.size(); ++i) longTone[i] = i16(std::lround(9000.0 * std::sin(double(i) * 0.03)));
		SrcSound ima;
		CHECK(decodeWav(imaWav(longTone, 22050), ima, why) && ima.rate == 22050 && ima.samples.size() >= 2000);
		double err = 0;
		for (size_t i = 0; i < ima.samples.size(); ++i) err = std::max(err, std::fabs(double(ima.samples[i]) - double(longTone[i])));
		CHECK(err < 900.0);
	}

	// ---- the sound engine, on a baked game ----
	SrcScript script;
	buildSyntheticScript(script);
	std::vector<BakeWorld> worlds(2);
	worlds[0].name = "world";
	worlds[0].level = "WORLD.ZEN";
	SynthOptions so;
	so.cells = 48;
	so.huts = 6;
	so.trees = 20;
	buildSyntheticScene(so, worlds[0].scene);
	worlds[1].name = "cave";
	worlds[1].level = "CAVE.ZEN";
	buildSyntheticCave(worlds[1].scene);
	std::vector<SrcModel> models(1);
	buildSyntheticActor(models[0]);
	BakeScripts scripts;
	scripts.script = &script;
	scripts.looks = LookSource {buildSyntheticRig, buildSyntheticBody, buildSyntheticHead, buildSyntheticItem, buildSyntheticParticleTexture, buildSyntheticOverlay, buildSyntheticOverlays, nullptr};
	scripts.soundFile = buildSyntheticSound;
	SrcScript sfxScript;
	buildSyntheticSfxScript(sfxScript);
	scripts.sfx = &sfxScript;
	BakeOptions bo;
	bo.chunker.chunkSize = 3000.0f;
	bo.tex.maxSize = 32;
	bo.tex.quality = 0;
	bo.threads = 2;
	bo.quiet = true;
	PakWriter writer;
	bakeWorlds(worlds, models, scripts, bo, writer);
	CHECK(writer.has("sfx/step") && writer.has("sfx/step_stone") && writer.has("sfx/hum"));
	CHECK(writer.has("world/world/ambient") && !writer.has("world/cave/ambient"));
	const std::string path = gDir + "/sound.pak";
	writer.write(path);
	// The same with sound switched off: nothing of it in the archive.
	{
		bo.soundRate = 0;
		PakWriter silent;
		std::vector<BakeWorld> again(2);
		again[0].name = "world";
		again[0].level = "WORLD.ZEN";
		buildSyntheticScene(so, again[0].scene);
		again[1].name = "cave";
		again[1].level = "CAVE.ZEN";
		buildSyntheticCave(again[1].scene);
		bakeWorlds(again, models, scripts, bo, silent);
		CHECK(!silent.has("sfx/step") && !silent.has("world/world/ambient"));
		bo.soundRate = 22050;
	}

	gfx::init();
	{
		HostAudio audio;
		App app;
		app.setAudio(&audio);
		CHECK(app.init(path.c_str(), nullptr));
		Sound& sound = app.sound();
		CHECK(sound.ready() && sound.volume() > 0.79f && sound.volume() < 0.81f);
		CHECK(sound.knows("SFX_TEST_STEP") && sound.knows("sfx_test_hum") && !sound.knows("SFX_NO_SUCH_THING"));
		CHECK(sound.stats().ambient == 1);
		CHECK(app.hero().loaded());
		// fix184: the Barrier's thunder follows the menu's choice (the game's MFX_Barriere_Ambient, else the baker's thunders).
		if (sound.knows("G3_THUNDER_FAR") && sound.knows("G3_THUNDER_NEAR") && sound.knows("G3_THUNDER_ROLL")) {
			Barrier bar;
			const char* const want[] = {"G3_THUNDER_ROLL", "G3_THUNDER_NEAR", "G3_THUNDER_ROLL"}; // (0: no game sound here: the roll)
			for (u8 c = 0; c < 3; ++c) {
				bar.update(0.016f, Barrier::OFF, c, true, {0, 0, 0}, sound, app.pak());
				CHECK(bar.thunderName() && std::strcmp(bar.thunderName(), want[c]) == 0);
			}
			bar.update(0.016f, Barrier::OFF, Barrier::THUNDER_MAGIC, true, {0, 0, 0}, sound, app.pak());
			CHECK(bar.thunderName() != nullptr); // (no spell of lightning here: the far thunder)
			bar.update(0.016f, Barrier::OFF, Barrier::THUNDER_NONE, true, {0, 0, 0}, sound, app.pak());
			CHECK(!bar.thunderName());
			// A game that has it: "As in the game" is MFX_Barriere_Ambient (oCBarrier plays it for every strike).
			static i16 click[256] = {};
			CHECK(sound.addSynthetic("MFX_BARRIERE_AMBIENT", click, 256, 16000, false));
			CHECK(sound.playable("MFX_BARRIERE_AMBIENT") && sound.playable("G3_THUNDER_ROLL") && !sound.playable("SFX_NO_SUCH_THING"));
			Barrier own;
			own.update(0.016f, Barrier::OFF, Barrier::THUNDER_FAR, true, {0, 0, 0}, sound, app.pak());
			CHECK(own.thunderName() && std::strcmp(own.thunderName(), "MFX_BARRIERE_AMBIENT") == 0);
			CHECK(!own.humName());
			own.update(0.016f, Barrier::OFF, Barrier::THUNDER_NEAR, true, {0, 0, 0}, sound, app.pak());
			CHECK(own.thunderName() && std::strcmp(own.thunderName(), "G3_THUNDER_NEAR") == 0);
			own.update(0.016f, Barrier::OFF, Barrier::THUNDER_FAR, true, {0, 0, 0}, sound, app.pak());
			CHECK(own.thunderName() && std::strcmp(own.thunderName(), "MFX_BARRIERE_AMBIENT") == 0);
		} else {
			CHECK(!"weather thunders baked");
		}

		// A sound without a place: both ears the same, the scripts' volume and ours (100/127 * 0.8).
		sound.setListener({0, 0, 0}, 0.0f);
		const u32 before = audio.startedCount();
		sound.play("SFX_TEST_STEP", nullptr);
		CHECK(soundWait(sound, audio, before + 1));
		CHECK(audio.last().rate == 22050 && audio.last().frames == 4410 && !audio.last().loop);
		CHECK(std::fabs(audio.last().left - audio.last().right) < 0.001f);
		CHECK(std::fabs(audio.last().left - 100.0f / 127.0f * 0.8f) < 0.01f);
		const SoundStats s1 = sound.stats();
		CHECK(s1.played >= 1 && s1.loads >= 1 && s1.unknown == 0);

		// With a place: louder in the ear it is nearer to; beyond its range not at all.
		const Vec3 right = {300, 0, 100}, left = {-300, 0, 100}, far = {6000, 0, 0};
		u32 n = audio.startedCount();
		sound.play("SFX_TEST_STEP", &right);
		CHECK(audio.startedCount() == n + 1 && audio.last().right > audio.last().left * 1.5f);
		n = audio.startedCount();
		sound.play("SFX_TEST_STEP", &left);
		CHECK(audio.startedCount() == n + 1 && audio.last().left > audio.last().right * 1.5f);
		n = audio.startedCount();
		sound.play("SFX_TEST_STEP", &far);
		CHECK(audio.startedCount() == n && sound.stats().culled >= 1);
		// The listener turns to face +X: what is on +X is dead ahead.
		sound.setListener({0, 0, 0}, 1.5707963f);
		const Vec3 ahead = {300, 0, 0};
		sound.play("SFX_TEST_STEP", &ahead);
		CHECK(std::fabs(audio.last().left - audio.last().right) < 0.02f);
		sound.setListener({0, 0, 0}, 0.0f);
		// A short range carries less far.
		n = audio.startedCount();
		sound.play("SFX_TEST_STEP", &right, 1.0f, 200.0f);
		CHECK(audio.startedCount() == n);

		// What the ground is made of: the extended name when the scripts have one.
		CHECK(Sound::surfaceSuffix(2) && std::strcmp(Sound::surfaceSuffix(2), "_STONE") == 0 && !Sound::surfaceSuffix(0));
		n = audio.startedCount();
		sound.playGround("SFX_TEST_STEP", 2, nullptr);
		CHECK(soundWait(sound, audio, n + 1) && audio.last().frames == 6615);
		n = audio.startedCount();
		sound.playGround("SFX_TEST_STEP", 4, nullptr); // (no "_EARTH": the plain step)
		CHECK(audio.startedCount() == n + 1 && audio.last().frames == 4410);

		// The scripts' own calls.
		ScriptVm& vm = app.npcs().game().vm();
		n = audio.startedCount();
		vm.call(vm.find("SNDTEST"));
		CHECK(audio.startedCount() == n + 1 && vm.stats().errors == 0);
		const u32 unknownBefore = sound.stats().unknown;
		sound.play("SFX_NOT_THERE", nullptr);
		CHECK(sound.stats().unknown == unknownBefore + 1);

		// The weather: a spell's sound is never the rain; with none of its own the game makes rain and thunder.
		CHECK(weatherNameIsSpell("MFX_FIRERAIN_RAIN") && weatherNameIsSpell("mfx_thunderball_cast") && !weatherNameIsSpell("RAIN_01") &&
		      !weatherNameIsSpell("THUNDER"));
		{
			static i16 rain[RAIN_SFX_FRAMES];
			makeRainSound(rain);
			double sum = 0, step = 0;
			int peak = 0;
			for (u32 i = 0; i < RAIN_SFX_FRAMES; ++i) {
				sum += double(rain[i]) * rain[i];
				peak = std::max(peak, std::abs(int(rain[i])));
				if (i) step += std::abs(int(rain[i]) - int(rain[i - 1]));
			}
			const double rms = std::sqrt(sum / RAIN_SFX_FRAMES), meanStep = step / (RAIN_SFX_FRAMES - 1);
			const int seam = std::abs(int(rain[0]) - int(rain[RAIN_SFX_FRAMES - 1]));
			std::printf("  made rain: rms %.0f, peak %d, mean step %.0f, the seam %d\n", rms, peak, meanStep, seam);
			CHECK(rms > 2000 && peak <= 32000 && seam < meanStep * 6.0);
			static i16 thunder[THUNDER_SFX_FRAMES];
			makeThunderSound(thunder, 7);
			double tsum = 0;
			for (u32 i = 0; i < THUNDER_SFX_FRAMES; ++i) tsum += double(thunder[i]) * thunder[i];
			CHECK(std::sqrt(tsum / THUNDER_SFX_FRAMES) > 1000 && thunder[THUNDER_SFX_FRAMES - 1] == 0);
			// A made sample plays by its name, loops by playLoop, and its handle stops it.
			CHECK(sound.addSynthetic("G3_TEST_RAIN", rain, RAIN_SFX_FRAMES, RAIN_SFX_RATE, true) && sound.knows("G3_TEST_RAIN"));
			CHECK(!sound.addSynthetic("G3_TEST_RAIN", rain, RAIN_SFX_FRAMES, RAIN_SFX_RATE, true));
			n = audio.startedCount();
			const u32 h = sound.playLoop("G3_TEST_RAIN", 0.5f);
			CHECK(h && sound.playing(h) && audio.startedCount() == n + 1 && audio.last().loop && audio.last().rate == RAIN_SFX_RATE &&
			      audio.last().frames == RAIN_SFX_FRAMES);
			sound.stop(h);
			CHECK(!sound.playing(h));
			// A loop whose sample is still being read: the handle holds while it waits, and stops what starts from it.
			n = audio.startedCount();
			const u32 hum = sound.playLoop("SFX_TEST_HUM", 0.5f);
			CHECK(hum && sound.playing(hum));
			CHECK(soundWait(sound, audio, n + 1) && audio.last().loop && sound.playing(hum));
			sound.setHandleGain(hum, 0.2f);
			sound.stop(hum);
			CHECK(!sound.playing(hum));
			// The baker put its own recordings in (weather_sfx_bake.h): the app takes them and makes none of its own.
			Input none;
			app.update(none, 1.0f / 30.0f);
			CHECK(sound.knows("G3_RAIN") && sound.knows("G3_THUNDER_NEAR") && sound.knows("G3_THUNDER_FAR") && sound.knows("G3_THUNDER_ROLL"));
			CHECK(!sound.knows("G3_RAIN_MADE") && !sound.knows("G3_THUNDER_MADE_A"));
		}

		// Volume: 0 is silence, and a sound already playing follows it.
		app.settings().soundVolume = 0;
		app.applySettings();
		n = audio.startedCount();
		sound.play("SFX_TEST_STEP", nullptr);
		CHECK(audio.startedCount() == n);
		app.settings().soundVolume = 10;
		app.applySettings();
		CHECK(sound.volume() == 1.0f);
		sound.stopAll();

		// Running: the clip's footsteps sound, the hum of the world is heard.
		app.settings().soundVolume = 8;
		app.applySettings();
		const float dt = 1.0f / 30.0f;
		const u32 steps0 = audio.startedCount();
		Input run;
		run.moveZ = 1.0f;
		u32 stepSounds = 0, lastCount = audio.startedCount();
		bool hum = false;
		for (int i = 0; i < 200; ++i) {
			app.update(run, dt);
			audio.advance(dt);
			if (audio.startedCount() > lastCount) {
				lastCount = audio.startedCount();
				if (audio.last().frames == 4410 || audio.last().frames == 6615) ++stepSounds;
				if (audio.last().loop) hum = true;
			}
			usleep(1500);
		}
		CHECK(audio.startedCount() > steps0);
		CHECK(stepSounds >= 3);
		CHECK(hum && app.sound().stats().ambient == 1);

		// Leaving the world takes the world's own sounds with it.
		app.shutdown();
		CHECK(audio.liveBlocks() == 0);
	}
	gfx::shutdown();
}

static void testSky() {
	const SkyColors noon = skyColorsAt(13.0f), night = skyColorsAt(1.0f), wrap = skyColorsAt(25.0f);
	CHECK(noon.light[0] == 1.0f && noon.light[1] == 1.0f && noon.light[2] == 1.0f);
	CHECK(night.light[0] < 0.5f && night.zenith[2] < 60);
	CHECK(wrap.zenith[0] == night.zenith[0] && wrap.horizon[2] == night.horizon[2]);
	const SkyColors dawn = skyColorsAt(5.75f); // halfway between night and the dawn key
	CHECK(dawn.light[0] > night.light[0] && dawn.light[0] < 0.78f);
}

// The inhabitants: the script's people in the running game.
static void testInhabitants() {
	SrcScript script;
	buildSyntheticScript(script);
	const auto bake = [&](const std::string& path, const BakeScripts& scripts, BakeResult* result = nullptr) {
		std::vector<BakeWorld> worlds(2);
		worlds[0].name = "world";
		worlds[0].level = "WORLD.ZEN";
		SynthOptions so;
		so.cells = 48;
		so.huts = 6;
		so.trees = 20;
		buildSyntheticScene(so, worlds[0].scene);
		worlds[1].name = "cave";
		worlds[1].level = "CAVE.ZEN";
		buildSyntheticCave(worlds[1].scene);
		std::vector<SrcModel> models(1);
		buildSyntheticActor(models[0]);
		BakeOptions bo;
		bo.chunker.chunkSize = 3000.0f;
		bo.tex.maxSize = 32;
		bo.tex.quality = 0;
		bo.threads = 2;
		bo.quiet = true;
		PakWriter writer;
		const BakeResult r = bakeWorlds(worlds, models, scripts, bo, writer);
		if (result) *result = r;
		writer.write(path);
	};
	BakeScripts scripts;
	scripts.script = &script;
	scripts.looks = LookSource {buildSyntheticRig, buildSyntheticBody, buildSyntheticHead, buildSyntheticItem, buildSyntheticParticleTexture, buildSyntheticOverlay, buildSyntheticOverlays, nullptr};
	const std::string path = gDir + "/people.pak";
	BakeResult baked;
	bake(path, scripts, &baked);
	CHECK(baked.characters == 12 && baked.looks == 6 && baked.looksBaked == 6 && baked.npcModels == 3 + 6 + 4);
	CHECK(!baked.scriptChecked); // nothing to check a built-in script against

	gfx::init();
	const float dt = 1.0f / 30.0f;
	{
		App app;
		CHECK(app.init(path.c_str(), nullptr));
		app.world().preload(app.camera().pos);
		Npcs& npcs = app.npcs();
		app.world().config().fadeMs = 0.0f; // (they come and go at once here; the dissolve is checked below)
		CHECK(npcs.loaded() && std::string(npcs.worldScript()) == "WORLD");
		CHECK(npcs.stats().total == 9 && npcs.stats().here == 8 && npcs.game().vm().stats().errors == 0);
		// Here everybody just stands where his routine has him; their life is testLife's.
		npcs.config().alive = false;
		app.environment().timeOfDay = 8.0f;
		for (int i = 0; i < 5; ++i) app.update(Input {}, dt);
		// From the start point, looking at the camp: the gate's two guards, the
		// digger on the road, the creatures by the wood.
		CHECK(npcs.stats().near == 7 && npcs.stats().waiting == 0 && npcs.stats().noModel == 0);
		app.render();
		const NpcStats first = npcs.stats();
		CHECK(first.shown >= 3 && first.shown + first.culled == 7 && first.triangles > 300 && first.drawCalls >= first.shown);
		const gfx::SoftStats ss = gfx::softStats();
		// (The things lying about are drawn the same way.)
		CHECK(ss.skinnedTriangles == first.triangles + app.loot().stats().triangles + (app.heroVisible() ? app.hero().model().triangles() : 0));
		// Three skeletons, and a body and a head for most: read once each, textures shared.
		CHECK(first.models >= 8 && first.models <= 13 && first.loads == first.models && first.textures >= 5);
		for (int i = 0; i < 30; ++i) {
			app.update(Input {}, dt);
			app.render();
		}
		CHECK(npcs.stats().loads == first.loads); // standing still reads nothing more

		// Everybody stands on the ground, upright, where the script said.
		const Game& game = npcs.game();
		u32 checked = 0;
		for (u32 i = 0; i < game.npcCount(); ++i) {
			const GameNpc& n = game.npc(i);
			if (n.hero || !n.placed) continue;
			float y;
			Vec3 normal;
			if (!app.world().rayDown({n.pos[0], n.pos[1] + 150.0f, n.pos[2]}, 650.0f, y, normal)) continue;
			CHECK(std::fabs(y - n.pos[1]) < 25.0f);
			++checked;
		}
		CHECK(checked >= 5);

		// Dissolving: the ones who leave are drawn fading out and let go when nothing is left; the ones who come fade in.
		{
			const Game& dg = npcs.game();
			auto seen = [&](u32& partly, u32& whole, u32& actors) {
				partly = whole = actors = 0;
				for (u32 i = 0; i < dg.npcCount(); ++i)
					if (const NpcActor* a = npcs.actorOf(i)) {
						++actors;
						partly += a->shown > 0 && a->shown < 255;
						whole += a->shown == 255;
					}
			};
			u32 partly, whole, actors;
			app.world().config().fadeMs = 400.0f;
			for (int i = 0; i < 3; ++i) {
				app.update(Input {}, dt);
				app.render();
			}
			seen(partly, whole, actors);
			CHECK(actors >= 3 && whole == actors); // (they are all there in full: the dissolve in is long done)
			npcs.config().maxShown = 0;
			bool fading = false;
			for (int i = 0; i < 4; ++i) {
				app.update(Input {}, dt);
				app.render();
				seen(partly, whole, actors);
				fading = fading || (actors > 0 && partly > 0);
			}
			CHECK(fading);
			for (int i = 0; i < 40; ++i) {
				app.update(Input {}, dt);
				app.render();
			}
			seen(partly, whole, actors);
			CHECK(actors == 0);
			npcs.config().maxShown = 12;
			bool coming = false;
			for (int i = 0; i < 6; ++i) {
				app.update(Input {}, dt);
				app.render();
				seen(partly, whole, actors);
				coming = coming || partly > 0;
			}
			CHECK(coming);
			for (int i = 0; i < 40; ++i) {
				app.update(Input {}, dt);
				app.render();
			}
			seen(partly, whole, actors);
			CHECK(actors >= 3 && whole == actors);
			app.world().config().fadeMs = 0.0f;
			app.update(Input {}, dt);
			app.render();
		}

		// Under load the reach of characters stays (they would otherwise appear
		// right in front of the player), never beyond what the world draws.
		CHECK(npcs.stats().reach == 3000.0f);
		app.world().config().detailScale = 0.5f;
		app.update(Input {}, dt);
		CHECK(npcs.stats().reach == 3000.0f);
		app.world().config().detailScale = 0.1f;
		app.update(Input {}, dt);
		CHECK(npcs.stats().reach == 3000.0f);
		// With a floor under load the reach shrinks to it and no further.
		npcs.config().reachFloor = 0.6f;
		app.update(Input {}, dt);
		CHECK(fabsf(npcs.stats().reach - 1800.0f) < 0.01f);
		npcs.config().reachFloor = 1.0f;
		app.world().config().detailScale = 1.0f;
		const float drawDistance = app.drawDistance();
		app.setDrawDistance(2000.0f);
		app.update(Input {}, dt);
		CHECK(npcs.stats().reach == 2000.0f);
		app.setDrawDistance(drawDistance);
		app.update(Input {}, dt);
		CHECK(npcs.stats().reach == 3000.0f && npcs.stats().near == 7);

		// Fewer allowed: the nearest ones. None allowed: none.
		npcs.config().maxShown = 2;
		app.update(Input {}, dt);
		app.render();
		CHECK(npcs.stats().shown <= 2 && npcs.stats().near == 7);
		npcs.config().maxShown = 0;
		app.update(Input {}, dt);
		app.render();
		CHECK(npcs.stats().shown == 0 &&
		      gfx::softStats().skinnedTriangles == app.loot().stats().triangles + (app.heroVisible() ? app.hero().model().triangles() : 0));
		// A short range: only who stands close.
		npcs.config().maxShown = 12;
		npcs.config().range = 700.0f;
		app.update(Input {}, dt);
		CHECK(npcs.stats().near >= 1 && npcs.stats().near <= 3);
		// Everybody at once, one model read per frame: they turn up one after the other.
		app.teleport({0.0f, 600.0f, 600.0f});
		npcs.config().range = 20000.0f;
		npcs.config().maxShown = 16;
		const u32 loadsBefore = npcs.stats().loads;
		app.update(Input {}, dt);
		CHECK(npcs.stats().near == 8 && npcs.stats().loads <= loadsBefore + 1);
		for (int i = 0; i < 40; ++i) app.update(Input {}, dt);
		CHECK(npcs.stats().waiting == 0 && npcs.stats().models == 13);

		// Evening: the routines have moved on, three of them to the fire.
		const u32 movesBefore = game.stats().moves;
		app.environment().timeOfDay = 21.0f;
		app.update(Input {}, dt);
		CHECK(game.stats().moves == movesBefore + 3);
		u32 atFire = 0;
		for (u32 i = 0; i < game.npcCount(); ++i) atFire += std::string(game.text(game.npc(i).at)) == "CAMP_FIRE";
		CHECK(atFire == 3);
		app.render();
		CHECK(npcs.stats().shown >= 1);

		// The cave: its own people, the others stay where they are; and back.
		const u32 loadsOutside = npcs.stats().loads;
		CHECK(app.enterWorld(hashName("world/cave"), nullptr, 0));
		CHECK(std::string(npcs.worldScript()) == "CAVE" && npcs.stats().total == 12 && npcs.stats().here == 3);
		CHECK(npcs.stats().loads > loadsOutside && npcs.stats().models <= 6); // the outside's models have gone
		for (int i = 0; i < 5; ++i) {
			app.update(Input {}, dt);
			app.render();
		}
		CHECK(npcs.stats().near == 3 && npcs.stats().waiting == 0);
		CHECK(app.enterWorld(hashName("world/world"), nullptr, 0));
		CHECK(npcs.stats().total == 12 && npcs.stats().here == 8 && game.stats().worlds == 2);
		app.shutdown();
		CHECK(!npcs.loaded() && npcs.stats().total == 0);
	}
	const gfx::MemStats leak = gfx::memStats();
	CHECK(leak.meshCount == 0 && leak.textureCount == 0);

	// Scripts without models for their people: they are there, nobody is seen.
	{
		BakeScripts bare;
		bare.script = &script;
		const std::string barePath = gDir + "/people_bare.pak";
		BakeResult r;
		bake(barePath, bare, &r);
		CHECK(r.characters == 12 && r.looks == 6 && r.looksBaked == 0 && r.report.find("nobody is drawn") != std::string::npos);
		App app;
		CHECK(app.init(barePath.c_str(), nullptr));
		app.environment().timeOfDay = 8.0f;
		app.update(Input {}, dt);
		app.render();
		CHECK(app.npcs().loaded() && app.npcs().stats().here == 8 && app.npcs().stats().near == 7);
		CHECK(app.npcs().stats().noModel == 7 && app.npcs().stats().shown == 0 && app.npcs().stats().loads == 0);
		app.shutdown();
	}
	// A source that has only some of the pieces: heads missing, creatures on
	// the wrong skeleton, one kind of armour that cannot be read at all.
	{
		BakeScripts partial;
		partial.script = &script;
		partial.looks.rig = buildSyntheticRig;
		partial.looks.body = [](const std::string& s, const std::string& mesh, int tex, int skin, SrcModel& m) {
			if (mesh == "hum_vlkl_armor") throw std::runtime_error("the file is damaged");
			// Creatures get a man's body: it does not fit their skeleton.
			return buildSyntheticBody(s == "humans" ? s : "humans", mesh, tex, skin, m);
		};
		partial.looks.head = [](const std::string&, const std::string& head, int, int, int, SrcModel& m) {
			m.notes.push_back(head + " not found");
			return false;
		};
		const std::string partialPath = gDir + "/people_partial.pak";
		BakeResult r;
		bake(partialPath, partial, &r);
		const auto said = [&](const char* text) { return r.report.find(text) != std::string::npos; };
		CHECK(r.looks == 6 && r.looksBaked == 3 && said("3 bodies, 0 heads"));
		CHECK(said("body made for another skeleton") && said("body unusable: the file is damaged [humans/hum_vlkl_armor, e.g. VLK_500_DIGGER]"));
		CHECK(said("head missing (drawn without): hum_head_fighter not found"));
		App app;
		CHECK(app.init(partialPath.c_str(), nullptr));
		app.environment().timeOfDay = 8.0f;
		for (int i = 0; i < 3; ++i) app.update(Input {}, dt);
		app.render();
		const NpcStats& ns = app.npcs().stats();
		// The two guards and the digger on the road are seen, headless; the creatures are not.
		CHECK(ns.near == 7 && ns.noModel == 4 && ns.waiting == 0 && ns.shown >= 1 && ns.shown <= 3);
		app.shutdown();
	}
	// An archive whose table of looks or of places is damaged: no crash, nobody shown.
	{
		Pak src;
		CHECK(src.open(path.c_str()));
		for (const char* victim : {"meta/looks", "world/world/points", "script/gothic"}) {
			PakWriter w;
			for (u32 i = 0; i < src.count(); ++i) {
				const PakEntry& e = src.entry(i);
				std::vector<u8> data(e.size);
				CHECK(src.read(e, data.data()));
				if (e.hash == hashName(victim)) {
					std::mt19937 rng(e.size);
					for (size_t k = 16; k < data.size(); k += 3) data[k] = u8(rng());
				}
				w.add(src.name(e), AssetType(e.type), data);
			}
			const std::string badPath = gDir + "/people_bad.pak";
			w.write(badPath);
			App app;
			CHECK(app.init(badPath.c_str(), nullptr));
			app.environment().timeOfDay = 8.0f;
			for (int i = 0; i < 3; ++i) {
				app.update(Input {}, dt);
				app.render();
			}
			std::printf("       %s damaged: scripts %s, %u characters, %u here, %u shown\n", victim,
			            app.npcs().loaded() ? "loaded" : "refused", app.npcs().stats().total, app.npcs().stats().here,
			            app.npcs().stats().shown);
			if (std::string(victim) == "meta/looks") CHECK(app.npcs().loaded() && app.npcs().stats().here == 8);
			// (Without the table of places the world has no name the scripts know.)
			if (std::string(victim) == "world/world/points") CHECK(app.npcs().loaded() && app.npcs().stats().here == 0);
			if (std::string(victim) == "script/gothic") CHECK(!app.npcs().loaded());
			app.shutdown();
		}
	}
	gfx::shutdown();
}

// The way net on its own: shortest ways, free points, damaged data.
static void testWayNet() {
	std::mt19937 rng(11);
	const auto bake = [&](const SrcScene& src, const std::string& path) {
		std::vector<BakeWorld> worlds(1);
		worlds[0].name = "net";
		worlds[0].level = "NET.ZEN";
		SrcScene& scene = worlds[0].scene;
		scene = src;
		scene.materials.resize(1);
		scene.materials[0].name = "floor";
		const float p[4][3] = {{-9000, 0, -9000}, {9000, 0, -9000}, {9000, 0, 9000}, {-9000, 0, 9000}};
		addQuad(scene, p[0], p[1], p[2], p[3], 0);
		BakeOptions bo;
		bo.tex.maxSize = 16;
		bo.tex.quality = 0;
		bo.quiet = true;
		PakWriter writer;
		bakeWorlds(worlds, bo, writer);
		writer.write(path);
	};
	// A random net: 120 points, each tied to a few of its neighbours; some left alone.
	SrcScene scene;
	const u32 n = 120;
	for (u32 i = 0; i < n; ++i) {
		char name[16];
		std::snprintf(name, sizeof(name), "WP_%03u", i);
		scene.wayPoints.push_back(SrcWayPoint {name, {float(rng() % 16000) - 8000.0f, 50.0f, float(rng() % 16000) - 8000.0f}, 0.3f});
	}
	std::vector<std::vector<float>> dist(n, std::vector<float>(n, 1e30f));
	const auto length = [&](u32 a, u32 b) {
		const float* p = scene.wayPoints[a].pos;
		const float* q = scene.wayPoints[b].pos;
		return std::sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[2] - q[2]) * (p[2] - q[2]));
	};
	for (u32 i = 0; i < n - 10; ++i) {
		// The three nearest.
		std::vector<u32> order;
		for (u32 k = 0; k < n - 10; ++k)
			if (k != i) order.push_back(k);
		std::partial_sort(order.begin(), order.begin() + 3, order.end(), [&](u32 a, u32 b) { return length(i, a) < length(i, b); });
		for (int k = 0; k < 3; ++k) scene.wayEdges.push_back({i, order[size_t(k)]});
	}
	scene.wayEdges.push_back({5, 5});   // nonsense the baker drops
	scene.wayEdges.push_back({3, 500});
	for (int i = 0; i < 12; ++i) {
		char name[32];
		std::snprintf(name, sizeof(name), i % 3 == 0 ? "FP_SIT_%02d" : "FP_ROAM_%02d", i);
		scene.freePoints.push_back(SrcWayPoint {name, {float(i) * 100.0f, 30.0f, 0.0f}, 1.0f});
	}
	const std::string path = gDir + "/waynet.pak";
	bake(scene, path);
	Pak pak;
	CHECK(pak.open(path.c_str()));
	WayNet net;
	CHECK(!net.load(pak, "world/nowhere") && !net.loaded() && net.nodeCount() == 0);
	CHECK(net.load(pak, "world/net") && net.nodeCount() == n && net.freeCount() == 12);
	// On the ground (the floor is at 0), named, found by name in any case.
	CHECK(net.node(7).pos[1] == 0.0f && net.freePoint(3).pos[1] == 0.0f && net.node(7).yaw == 0.3f);
	CHECK(std::string(net.nodeName(7)) == "WP_007" && net.findNode("wp_007") == 7 && net.findNode("WP_999") == -1 && net.findNode("") == -1);
	CHECK(std::string(net.freeName(4)) == "FP_ROAM_04");

	// Every connection once from each end; the reference: all pairs, the slow way.
	std::vector<std::vector<bool>> tied(n, std::vector<bool>(n, false));
	for (const auto& [a, b] : scene.wayEdges)
		if (a < n && b < n && a != b) tied[a][b] = tied[b][a] = true;
	const auto edge = [&](u32 a, u32 b) {
		const float* p = net.node(a).pos;
		const float* q = net.node(b).pos;
		return std::sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2])) + 1.0f;
	};
	for (u32 a = 0; a < n; ++a) {
		dist[a][a] = 0;
		for (u32 b = 0; b < n; ++b)
			if (tied[a][b]) dist[a][b] = edge(a, b);
	}
	for (u32 k = 0; k < n; ++k)
		for (u32 a = 0; a < n; ++a)
			for (u32 b = 0; b < n; ++b) dist[a][b] = std::min(dist[a][b], dist[a][k] + dist[k][b]);
	u32 ways = 0, none = 0;
	float worst = 0;
	for (int round = 0; round < 3000; ++round) {
		const u32 a = rng() % n, b = rng() % n;
		u16 out[256];
		const u32 count = net.path(a, b, out, 256);
		if (a == b || dist[a][b] > 1e29f) {
			CHECK(count == 0);
			++none;
			continue;
		}
		CHECK(count > 0 && count <= 256 && out[count - 1] == b);
		// Every step along a connection, and the whole no longer than the shortest.
		float sum = 0;
		u32 at = a;
		bool connected = true;
		for (u32 k = 0; k < count && k < 256; ++k) {
			connected = connected && tied[at][out[k]];
			sum += edge(at, out[k]);
			at = out[k];
		}
		CHECK(connected);
		worst = std::max(worst, std::fabs(sum - dist[a][b]));
		++ways;
		// A short list gets the first steps and the true count.
		u16 few[2];
		CHECK(net.path(a, b, few, 2) == count && few[0] == out[0] && (count < 2 || few[1] == out[1]));
	}
	std::printf("       %u ways through a net of %u points checked against all-pairs search (worst difference %.3f cm), %u pairs without a way\n",
	            ways, n, double(worst), none);
	CHECK(ways > 2000 && none > 100 && worst < 0.5f);
	CHECK(net.path(0, 9999, nullptr, 0) == 0 && net.path(9999, 0, nullptr, 0) == 0);
	// The nearest node, and the nearest but one.
	for (int round = 0; round < 200; ++round) {
		const float at[3] = {float(rng() % 16000) - 8000.0f, 0.0f, float(rng() % 16000) - 8000.0f};
		u32 best = 0;
		for (u32 i = 1; i < n; ++i)
			if (length(i, i) == 0 && std::hypot(net.node(i).pos[0] - at[0], net.node(i).pos[2] - at[2]) <
			                             std::hypot(net.node(best).pos[0] - at[0], net.node(best).pos[2] - at[2]))
				best = i;
		CHECK(net.nearestNode(at) == int(best));
		const int second = net.nearestNode(at, int(best));
		CHECK(second >= 0 && second != int(best));
	}

	// Free points: by a word of the name, the nearest in reach, one holder each.
	const float origin[3] = {0, 0, 0}, far[3] = {5000, 0, 0};
	CHECK(net.findFree(origin, "SIT", 1) == 0 && net.findFree(origin, "sit", 1) == 0 && net.findFree(origin, "ROAM", 1) == 1);
	CHECK(net.findFree(origin, "FP_", 1) == 0 && net.findFree(origin, "BED", 1) == -1 && net.findFree(origin, "", 1) == -1);
	CHECK(net.findFree(far, "SIT", 1) == -1); // too far away
	CHECK(net.nameContains(3, "SIT_03") && !net.nameContains(3, "SIT_04") && !net.nameContains(99, "SIT"));
	net.hold(0, 1);
	CHECK(net.heldBy(1) == 0 && net.findFree(origin, "SIT", 1) == 0); // its own is still there for it
	CHECK(net.findFree(origin, "SIT", 2) == 3);                      // for the next one the next
	CHECK(net.findFree(origin, "SIT", 1, 0) == 3);                   // and for itself if it wants another
	net.hold(0, 2);                                                  // taken: stays the first one's
	CHECK(net.heldBy(1) == 0 && net.heldBy(2) == -1);
	net.hold(3, 1);                                                  // one each: the first is let go
	CHECK(net.heldBy(1) == 3 && net.findFree(origin, "SIT", 2) == 0);
	net.hold(77, 2);
	net.hold(-1, 2);
	CHECK(net.heldBy(2) == -1);
	net.drop(1);
	CHECK(net.heldBy(1) == -1);
	net.hold(6, 9);
	net.dropAll();
	CHECK(net.heldBy(9) == -1);

	// Damaged data: refused, or at least harmless.
	const PakEntry* e = pak.find("world/net/waynet");
	CHECK(e != nullptr);
	std::vector<u8> good(e ? e->size : 0);
	if (e) CHECK(pak.read(*e, good.data()));
	int loaded = 0, refused = 0;
	for (int round = 0; round < 400 && !good.empty(); ++round) {
		std::vector<u8> bad = good;
		const int hits = 1 + int(rng() % 4);
		for (int k = 0; k < hits; ++k) {
			const size_t at = round < 100 ? 8 + rng() % 24 : 8 + rng() % (bad.size() - 8);
			bad[at] = rng() % 2 ? u8(rng()) : u8(bad[at] ^ (1u << (rng() % 8)));
		}
		if (round % 50 == 49) bad.resize(bad.size() - 1 - rng() % 64);
		PakWriter w;
		w.add("world/net/waynet", ASSET_META, bad);
		const std::string badPath = gDir + "/waynet_bad.pak";
		w.write(badPath);
		Pak bp;
		CHECK(bp.open(badPath.c_str()));
		WayNet b;
		if (!b.load(bp, "world/net")) {
			++refused;
			continue;
		}
		++loaded;
		u16 out[64];
		for (int k = 0; k < 40 && b.nodeCount(); ++k) {
			(void)b.path(rng() % b.nodeCount(), rng() % b.nodeCount(), out, 64);
			const float at[3] = {float(rng() % 16000) - 8000.0f, 0.0f, float(rng() % 16000) - 8000.0f};
			(void)b.nearestNode(at);
			(void)b.findNode(b.nodeName(rng() % b.nodeCount()));
			const int fp = b.findFree(at, "ROAM", u32(k));
			b.hold(fp, u32(k));
		}
	}
	std::printf("       %d damaged nets used, %d refused\n", loaded, refused);
	CHECK(loaded > 50 && refused > 50);
}

// The two synthetic worlds with a script of the caller's and the people it names.
static BakeResult bakeLifeWorlds(const std::string& source, const std::string& path, std::vector<BakeWorld>& worlds,
                                 bool withFightAi = true, bool sunShadow = false, bool grass = false, int lampMode = 0, bool menu = false) {
	SrcScript script;
	compileDaedalus(source, script);
	worlds.assign(2, BakeWorld());
	worlds[0].name = "world";
	worlds[0].level = "WORLD.ZEN";
	SynthOptions so;
	so.cells = 48;
	so.huts = 6;
	so.trees = 20;
	buildSyntheticScene(so, worlds[0].scene);
	if (lampMode == 1) worlds[0].scene.lights.clear(); // (fix148: no table of lamps - as a data.pak of before)
	worlds[1].name = "cave";
	worlds[1].level = "CAVE.ZEN";
	buildSyntheticCave(worlds[1].scene);
	std::vector<SrcModel> models(1);
	buildSyntheticActor(models[0]);
	BakeOptions bo;
	bo.light.enabled = lampMode == 2;
	bo.chunker.chunkSize = 3000.0f;
	bo.tex.maxSize = 32;
	bo.tex.quality = 0;
	bo.threads = 2;
	bo.quiet = true;
	bo.sun.enabled = sunShadow;
	bo.sun.texel = 48.0f;
	bo.sun.samples = 3;
	bo.grass.enabled = grass;
	BakeScripts scripts;
	scripts.script = &script;
	SrcScript fight;
	if (withFightAi) {
		buildSyntheticFightScript(fight);
		scripts.fight = &fight;
	}
	scripts.looks = LookSource {buildSyntheticRig, buildSyntheticBody, buildSyntheticHead, buildSyntheticItem, buildSyntheticParticleTexture, buildSyntheticOverlay, buildSyntheticOverlays, nullptr, buildSyntheticForm};
	SrcScript menuScript;
	if (menu) {
		buildSyntheticMenuScript(menuScript);
		scripts.menu = &menuScript;
	}
	PakWriter writer;
	const BakeResult r = bakeWorlds(worlds, models, scripts, bo, writer);
	writer.write(path);
	return r;
}

// fix148: the light of torches and fires (g3ds/lamps.h, engine/lamps.h). The baker's table: the dynamic lamps only, the keys
// of their flicker thinned to eight, the baked ones marked. The game: the synthetic camp fire's lamp lights the hero (more red
// than blue), flickers, reaches the world's boxes round it and not those far off; the menu turns it off for the world (1) and
// for all (0); without the table the fires of the particles stand in; baked with --lights the world takes only the flicker.
static void testLamps() {
	{
		std::vector<SrcLight> src(3);
		src[0].isStatic = true;
		src[0].range = 500.0f;
		src[1].range = 700.0f;
		src[1].fire = true;
		for (int k = 0; k < 16; ++k) src[1].rangeKeys.push_back(k % 4 < 2 ? 1.2f : 0.8f); // (thinned to every second: 1.2, 0.8, ...)
		src[1].rangeFps = 16.0f;
		src[1].colorKeys = {{255, 0, 0}, {0, 255, 0}};
		src[1].colorFps = 2.0f;
		src[2].range = 0.5f; // (no reach: left out)
		const std::vector<WorldLamp> t = lampTable(src, false), tb = lampTable(src, true);
		CHECK(t.size() == 1 && tb.size() == 1);
		if (t.size() == 1 && tb.size() == 1) {
			CHECK(t[0].range == 700.0f && (t[0].flags & LAMPF_FIRE) && !(t[0].flags & LAMPF_BAKED) && (tb[0].flags & LAMPF_BAKED));
			CHECK(t[0].rangeKeys == LAMP_KEYS && std::fabs(t[0].rangeFps - 8.0f) < 1e-4f && t[0].rangeScale[0] == 77 && t[0].rangeScale[1] == 51);
			CHECK(t[0].colorKeys == 2 && t[0].colors[1][1] == 255 && t[0].colorFps == 2.0f);
		}
	}
	std::vector<BakeWorld> worlds;
	const std::string path = gDir + "/lamps.pak";
	bakeLifeWorlds(syntheticScriptSource(), path, worlds, false);
	gfx::init();
	{
		App app;
		CHECK(app.init(path.c_str(), nullptr));
		app.skipChapters();
		app.npcs().game().setQuiet(true);
		app.weather().configure(WEATHER_CLEAR, true);
		app.weather().settle();
		app.environment().timeOfDay = 23.0f;
		app.settings().timeFlow = 0;
		CHECK(app.settings().lamps == 2); // (the default)
		app.applySettings();
		app.world().preload(app.camera().pos);
		CHECK(app.lamps().available() && app.lamps().stats().table && !app.lamps().stats().baked && app.lamps().stats().lamps == 1);
		for (int i = 0; i < 30; ++i) app.update(Input {}, 1.0f / 30.0f);
		app.environment().timeOfDay = 23.0f; // (the scripts set the morning as the game starts)
		for (int i = 0; i < 10; ++i) app.update(Input {}, 1.0f / 30.0f);
		app.render();
		const LampStats& ls = app.lamps().stats();
		Vec3 at;
		float range = 0, rgb[3] = {0, 0, 0};
		CHECK(ls.active == 1 && ls.shining == 1 && ls.world && app.lamps().active(0, at, range, rgb));
		const float lit[3] = {app.hero().light()[0], app.hero().light()[1], app.hero().light()[2]};
		std::printf("  lamp at %.0f %.0f %.0f, range %.0f, colour %.2f %.2f %.2f; hero %.2f %.2f %.2f, %u boxes of the world lit, %u draws\n",
		            double(at.x), double(at.y), double(at.z), double(range), double(rgb[0]), double(rgb[1]), double(rgb[2]), double(lit[0]),
		            double(lit[1]), double(lit[2]), ls.boxes, app.world().stats().lampDrawCalls);
		CHECK(rgb[0] > rgb[2] && range > 600.0f && range < 1000.0f);
		// Its flicker: another moment, another colour or reach.
		float rgb2[3] = {0, 0, 0}, range2 = 0;
		for (int i = 0; i < 7; ++i) app.update(Input {}, 1.0f / 30.0f);
		CHECK(app.lamps().active(0, at, range2, rgb2) && (range2 != range || rgb2[0] != rgb[0] || rgb2[1] != rgb[1]));
		// The boxes: round it, yes; far off, no.
		{
			Lamps& lamps = const_cast<Lamps&>(app.lamps());
			const float near0[3] = {at.x - 100.0f, at.y - 300.0f, at.z - 100.0f}, near1[3] = {at.x + 100.0f, at.y, at.z + 100.0f};
			const float far0[3] = {at.x + 5000.0f, at.y, at.z}, far1[3] = {at.x + 6000.0f, at.y + 100.0f, at.z + 100.0f};
			const u32 bits = lamps.touching(near0, near1);
			CHECK(bits == 1u && lamps.touching(far0, far1) == 0u);
			const u32 key = lamps.pick(near0, near1, bits);
			gfx::LampSet set;
			lamps.setFor(key, set);
			std::printf("  box key %08x: %u lamps, range %.0f (%.0f), keep %.2f\n", key, set.count, double(set.lamp[0].range), double(range2), double(set.dayKeep));
			CHECK(key != 0xFFFFFFFFu && set.count == 1 && set.lamp[0].range == range2 && set.dayKeep > 0.9f);
		}
		// Off for the world (1): the people still; all off (0): the hero as the ground has him.
		app.settings().lamps = 1;
		app.update(Input {}, 1.0f / 30.0f);
		CHECK(!app.lamps().lightsWorld() && app.lamps().stats().active == 1);
		app.settings().lamps = 0;
		for (int i = 0; i < 3; ++i) app.update(Input {}, 1.0f / 30.0f);
		const float dark[3] = {app.hero().light()[0], app.hero().light()[1], app.hero().light()[2]};
		std::printf("  hero without the lamps %.2f %.2f %.2f\n", double(dark[0]), double(dark[1]), double(dark[2]));
		CHECK(app.lamps().stats().active == 0 && !app.lamps().lightsWorld());
		CHECK(lit[0] - dark[0] > 0.05f && lit[0] - dark[0] > lit[2] - dark[2]);
		// The debug word: nothing either.
		app.settings().lamps = 2;
		app.setRenderSkip(App::SKIP_LAMPS);
		app.update(Input {}, 1.0f / 30.0f);
		CHECK(app.lamps().stats().active == 0);
		app.setRenderSkip(0);
		app.shutdown();
	}
	// A data.pak without the table: the camp fire's particles stand in.
	{
		const std::string old = gDir + "/lamps_old.pak";
		bakeLifeWorlds(syntheticScriptSource(), old, worlds, false, false, false, 1);
		App app;
		CHECK(app.init(old.c_str(), nullptr));
		std::printf("  without the table: %u lamps from the particles\n", app.lamps().stats().lamps);
		CHECK(app.lamps().available() && !app.lamps().stats().table && app.lamps().stats().lamps == 1);
		app.shutdown();
	}
	// Baked with --lights: marked, and the world takes only the flicker (small, either way of 0).
	{
		const std::string baked = gDir + "/lamps_baked.pak";
		bakeLifeWorlds(syntheticScriptSource(), baked, worlds, false, false, false, 2);
		App app;
		CHECK(app.init(baked.c_str(), nullptr));
		app.skipChapters();
		app.environment().timeOfDay = 23.0f;
		app.settings().timeFlow = 0;
		app.applySettings();
		app.world().preload(app.camera().pos);
		for (int i = 0; i < 5; ++i) app.update(Input {}, 1.0f / 30.0f);
		app.environment().timeOfDay = 23.0f;
		float most = 0.0f;
		for (int i = 0; i < 20; ++i) {
			app.update(Input {}, 1.0f / 30.0f);
			Vec3 at;
			float range = 0, rgb[3];
			if (app.lamps().active(0, at, range, rgb)) most = std::max(most, std::fabs(rgb[0]));
		}
		std::printf("  baked: the world's flicker at most %.3f\n", double(most));
		// (fix149: its baked light again, BAKED_EXTRA of it, and the flicker round that.)
		// fix150: the engine side of fix149 (BAKED_EXTRA, Settings::lampPower) is not in the repository; its checks wait for it.
		CHECK(app.lamps().stats().baked && most < 1.6f);
#ifdef G3DS_HAVE_FIX149
		CHECK(most > 0.5f);
		// The menu's brightness: twice as much at 200 % as at 100 %, half at 50 %.
		float at100 = 0.0f, at200 = 0.0f, at50 = 0.0f;
		for (int pass = 0; pass < 3; ++pass) {
			app.settings().lampPower = u8(pass == 0 ? 100 : pass == 1 ? 200 : 50);
			app.update(Input {}, 0.0f);
			Vec3 at;
			float range = 0, rgb[3];
			CHECK(app.lamps().active(0, at, range, rgb));
			(pass == 0 ? at100 : pass == 1 ? at200 : at50) = rgb[0];
		}
		std::printf("  brightness 50/100/200 %%: %.2f %.2f %.2f\n", double(at50), double(at100), double(at200));
		CHECK(at200 > at100 * 1.5f && at50 < at100 * 0.75f);
#endif
		app.shutdown();
	}
	gfx::shutdown();
}

// The new grass (fix147, g3ds-bake --grass; engine/grass.h): the bake sows the same tufts every time, on the ground and
// not in the water nor on the paths, packs them by tiles; the runtime makes meshes of the tiles round the camera only,
// draws them as cut-outs, presses them aside round the hero and the inhabitants, gives way under load and frees all.
static void testGrass() {
	// The bake: the map and the tufts, twice the same.
	SrcScene scene;
	SynthOptions so;
	so.cells = 48;
	so.huts = 6;
	so.trees = 20;
	buildSyntheticScene(so, scene);
	GrassOptions go;
	go.enabled = true;
	GrassPlacement a, b;
	GrassBakeStats sa, sb;
	std::string why;
	CHECK(placeGrass(scene, go, a, sa, why) && placeGrass(scene, go, b, sb, why));
	CHECK(a.seeds.size() > 1000 && a.seeds.size() == b.seeds.size());
	bool same = a.seeds.size() == b.seeds.size();
	for (size_t i = 0; same && i < a.seeds.size(); ++i)
		same = std::memcmp(a.seeds[i].pos, b.seeds[i].pos, sizeof(a.seeds[i].pos)) == 0 && a.seeds[i].look == b.seeds[i].look &&
		       a.seeds[i].rgb[1] == b.seeds[i].rgb[1];
	CHECK(same);
	// Nothing in the water; on the ground (within a few cm of the map's ground); much less on the paths than elsewhere.
	const GrassMap& map = a.map;
	u32 wet = 0, floating = 0, onPath = 0;
	double pathArea = 0;
	for (const GrassSeed& s : a.seeds) {
		int cx, cz;
		if (!map.cellOf(s.pos[0], s.pos[2], cx, cz)) {
			++floating;
			continue;
		}
		const size_t k = map.at(cx, cz);
		wet += map.waterY[k] > GrassMap::NONE && map.waterY[k] > s.pos[1] - 5.0f;
		floating += std::fabs(s.pos[1] - map.groundY[k]) > 120.0f;
	}
	auto nearPath = [&](float x, float z) {
		for (const auto& e : scene.wayEdges) {
			const float* p = scene.wayPoints[e.first].pos;
			const float* q = scene.wayPoints[e.second].pos;
			const float dx = q[0] - p[0], dz = q[2] - p[2], len2 = dx * dx + dz * dz;
			const float t = len2 > 0 ? std::clamp(((x - p[0]) * dx + (z - p[2]) * dz) / len2, 0.0f, 1.0f) : 0.0f;
			const float ex = p[0] + dx * t - x, ez = p[2] + dz * t - z;
			if (ex * ex + ez * ez < 80.0f * 80.0f) return true;
		}
		return false;
	};
	for (const GrassSeed& s : a.seeds) onPath += nearPath(s.pos[0], s.pos[2]);
	double groundArea = 0;
	for (int z = 0; z < map.h; ++z)
		for (int x = 0; x < map.w; ++x) {
			if (map.groundY[map.at(x, z)] == GrassMap::NONE || map.texW[map.at(x, z)] < 0.5f) continue;
			groundArea += map.cellM2();
			if (nearPath(map.cx(x), map.cz(z))) pathArea += map.cellM2();
		}
	const double pathDensity = pathArea > 0 ? double(onPath) / pathArea : 0.0, allDensity = double(a.seeds.size()) / std::max(groundArea, 1.0);
	std::printf("  bake: %zu tufts (%u without ground), %u in water, %u floating; %.2f tufts/m2 on the paths, %.2f on grassy ground\n",
	            a.seeds.size(), sa.noGround, wet, floating, pathDensity, allDensity);
	CHECK(wet == 0 && floating == 0 && sa.noGround < a.seeds.size() / 50);
	CHECK(scene.wayEdges.empty() || pathArea == 0 || pathDensity < 0.5 * allDensity);
	// Packing: every tuft in its tile, the index sorted, the list raw-sized.
	{
		std::vector<u8> index, data;
		encodeGrass(a, go, 1, 2, index, data, sa);
		GrassHeader h;
		std::memcpy(&h, index.data(), sizeof(h));
		CHECK(h.magic == GRASS_MAGIC && grassBlobSize(h) == index.size() && h.tuftCount * sizeof(GrassTuft) == data.size() &&
		      h.tuftCount == a.seeds.size());
		const GrassTile* tiles = reinterpret_cast<const GrassTile*>(index.data() + sizeof(h));
		bool sorted = true, inside = true;
		u32 sum = 0;
		for (u32 t = 0; t < h.tileCount; ++t) {
			if (t) sorted = sorted && (tiles[t - 1].z < tiles[t].z || (tiles[t - 1].z == tiles[t].z && tiles[t - 1].x < tiles[t].x));
			sum += tiles[t].count;
			const GrassTuft* list = reinterpret_cast<const GrassTuft*>(data.data()) + tiles[t].first;
			for (u32 i = 0; i < tiles[t].count; ++i) inside = inside && float(list[i].y) <= tiles[t].yMax - tiles[t].yMin + 1.0f;
		}
		CHECK(sorted && inside && sum == h.tuftCount);
		// The atlas: a cut-out of four pictures whose smallest level still has blades.
		const std::vector<u8> atlas = grassAtlasTexture(TexBakeOptions {});
		TexHeader th;
		std::memcpy(&th, atlas.data(), sizeof(th));
		CHECK(texValidate(th, u32(atlas.size() - sizeof(th))) && th.width == 256 && th.height == 64 && th.format == TEX_ETC1A4 &&
		      (th.flags & TEXF_ALPHA_TEST) && th.mipCount == 4);
		u32 cover[4] = {0, 0, 0, 0};
		for (u32 l = 0; l < th.mipCount; ++l) {
			const u32 w = th.width >> l, hh = th.height >> l;
			std::vector<u8> rgba(size_t(w) * hh * 4);
			texDecodeLevel(th, atlas.data() + sizeof(th), l, rgba.data());
			u32 n = 0;
			for (size_t i = 3; i < rgba.size(); i += 4) n += rgba[i] >= 128;
			cover[l] = u32(100.0 * n / (double(w) * hh));
		}
		std::printf("  atlas: %u x %u, coverage %u/%u/%u/%u %% by level\n", th.width, th.height, cover[0], cover[1], cover[2], cover[3]);
		CHECK(cover[0] > 10 && cover[3] + 8 >= cover[0]);
	}

	// The game: a world baked with it (and the sun's shadow), on the PC's renderer.
	std::vector<BakeWorld> worlds;
	const std::string path = gDir + "/grass.pak";
	bakeLifeWorlds(syntheticScriptSource(), path, worlds, true, true, true);
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr));
	CHECK(app.grass().available());
	app.skipChapters();
	app.npcs().game().setQuiet(true);
	app.weather().configure(WEATHER_CLEAR, true);
	app.weather().settle();
	app.environment().timeOfDay = 14.5f;
	app.settings().timeFlow = 0;
	app.settings().wind = 2;
	app.applySettings();
	app.world().preload(app.camera().pos);
	for (int i = 0; i < 20; ++i) app.update(Input {}, 1.0f / 30.0f);
	// Off (the default): nothing held, nothing drawn.
	CHECK(app.settings().grass == 0 && app.grassRadius() == 0.0f);
	app.render();
	CHECK(app.grass().stats().tiles == 0 && app.grass().stats().slots == 0);
	const std::vector<u8> off(gfx::softFramebuffer(), gfx::softFramebuffer() + gfx::SCREEN_W * gfx::SCREEN_H * 4);
	// Many: the tiles round the camera arrive a little a frame, then it is drawn.
	app.settings().grass = 2;
	app.applySettings();
	CHECK(std::fabs(app.grassRadius() - 2800.0f) < 1.0f);
	u32 most = 0, frames = 0, bigBuild = 0;
	for (int i = 0; i < 40; ++i) {
		app.render();
		most = std::max(most, app.grass().stats().tufts);
		bigBuild = std::max(bigBuild, app.grass().stats().built);
		frames += app.grass().stats().tiles > 0;
	}
	const GrassStats many = app.grass().stats();
	const u8* on = gfx::softFramebuffer();
	u32 changed = 0;
	for (u32 i = 0; i < u32(gfx::SCREEN_W * gfx::SCREEN_H); ++i)
		changed += std::abs(int(on[i * 4]) - int(off[i * 4])) + std::abs(int(on[i * 4 + 1]) - int(off[i * 4 + 1])) > 12;
	std::printf("  many: %u tiles, %u tufts, %u triangles in %u draw calls; %u tiles held (%.2f MB), %u made, %u read; %u pixels changed\n",
	            many.tiles, many.tufts, many.triangles, many.drawCalls, many.slots, double(many.meshBytes) / 1048576.0, many.builtTotal,
	            many.reads, changed);
	CHECK(many.tiles > 0 && many.tufts > 100 && many.triangles == many.tufts * 4 && many.drawCalls == many.tiles && changed > 100);
	CHECK(many.slots <= Grass::SLOTS && many.reads == many.slots && bigBuild <= 8);
	// What is drawn lies within the radius of the camera: every tuft of a tile held is either within it, or left out.
	{
		const Vec3 cam = app.camera().pos;
		u32 within = 0;
		for (u32 t = 0; t < app.grass().tileCount(); ++t) {
			const GrassTile& tile = app.grass().tile(t);
			const float x0 = app.grass().header().originX + float(tile.x) * app.grass().header().tileSize;
			const float z0 = app.grass().header().originZ + float(tile.z) * app.grass().header().tileSize;
			const float dx = std::max({x0 - cam.x, 0.0f, cam.x - (x0 + app.grass().header().tileSize)});
			const float dz = std::max({z0 - cam.z, 0.0f, cam.z - (z0 + app.grass().header().tileSize)});
			within += dx * dx + dz * dz < 2800.0f * 2800.0f;
		}
		CHECK(many.slots >= std::min<u32>(within, Grass::SLOTS) && app.grass().findTile(-30000, -30000) < 0);
	}
	// Few: half the tufts, a shorter radius.
	app.settings().grass = 1;
	app.applySettings();
	for (int i = 0; i < 40; ++i) app.render();
	const GrassStats few = app.grass().stats();
	std::printf("  few: %u tiles, %u tufts\n", few.tiles, few.tufts);
	CHECK(few.tufts > 0 && few.tufts * 3 < many.tufts * 2);
	// Under load it gives way first: the pacer's detail at 75 % halves the radius, the relief takes more.
	app.settings().grass = 2;
	app.applySettings();
	app.world().config().detailScale = 0.75f;
	CHECK(std::fabs(app.grassRadius() - 1400.0f) < 1.0f);
	app.setGfxRelief(true);
	CHECK(app.grassRadius() < 1400.0f);
	app.setGfxRelief(false);
	app.world().config().detailScale = 1.0f;
	// debug.txt "grass": not drawn.
	app.setRenderSkip(App::SKIP_GRASS);
	app.render();
	CHECK(app.grass().stats().tiles == 0);
	app.setRenderSkip(0);
	// Walking through it: the tiles under the hero are made again as he goes (pressed aside round his feet).
	const u32 madeBefore = app.grass().stats().builtTotal;
	for (int i = 0; i < 60; ++i) {
		Input in;
		in.moveZ = 1.0f;
		app.update(in, 1.0f / 30.0f);
		app.render();
	}
	std::printf("  walking: %u tiles made in 60 frames\n", app.grass().stats().builtTotal - madeBefore);
	CHECK(app.grass().stats().builtTotal > madeBefore);
	// Settings: a file of before version 24 keeps it off; a wrong value is put right.
	{
		Settings st;
		st.grass = 7;
		st.sanitize();
		CHECK(st.grass == 0 && st.grassRadiusCm() == 0.0f);
		st.grass = 1;
		CHECK(st.grassShare() == 0.5f && st.grassRadiusCm() == 2000.0f);
	}
	// A world switch drops it all; the end frees every mesh.
	app.shutdown();
	CHECK(gfx::memStats().meshCount == 0);
}

// The inhabitants' life: states run by the scripts, walking the way net.
// The things people use: the table of a world (mobs.h).
static void testMobs() {
	std::mt19937 rng(23);
	SrcScene scene;
	scene.materials.resize(1);
	scene.materials[0].name = "floor";
	const float p[4][3] = {{-9000, 0, -9000}, {9000, 0, -9000}, {9000, 0, 9000}, {-9000, 0, 9000}};
	addQuad(scene, p[0], p[1], p[2], p[3], 0);
	const auto add = [&](const char* kind, float x, float y, float z, int states) -> SrcMob& {
		SrcMob m;
		m.scheme = kind;
		m.visual = std::string(kind) + "_X.ASC";
		m.pos[0] = x;
		m.pos[1] = y;
		m.pos[2] = z;
		m.yaw = 0.5f;
		m.states = states;
		scene.mobs.push_back(m);
		return scene.mobs.back();
	};
	// A bed with a front and a back, a second one upstairs, a far one; a bench
	// without a place to stand at; a cauldron one stands a pace from.
	SrcMob& bed = add("BED", 100, 0, 0, 1);
	bed.slots.push_back(SrcMobSlot {"ZS_POS0_FRONT", {100, 0, -120}, 0.0f});
	bed.slots.push_back(SrcMobSlot {"ZS_POS1_BACK", {100, 0, 120}, 3.14159f});
	add("BED", 150, 400, 0, 1).slots.push_back(SrcMobSlot {"ZS_POS0", {150, 400, -100}, 0.0f});
	add("BED", 5000, 0, 0, 1).slots.push_back(SrcMobSlot {"ZS_POS0", {5000, 0, -100}, 0.0f});
	add("BENCH", -300, 0, 0, 1);
	add("BEDHIGH", -500, 0, 0, 99).slots.push_back(SrcMobSlot {"ZS_POS0", {-500, 0, -100}, 0.0f});
	add("CAULDRON", 0, 0, 600, 3).slots.push_back(SrcMobSlot {"ZS_POS0_DIST", {0, 0, 690}, 0.0f});
	std::vector<BakeWorld> worlds(1);
	worlds[0].name = "m";
	worlds[0].level = "M.ZEN";
	worlds[0].scene = scene;
	BakeOptions bo;
	bo.tex.maxSize = 16;
	bo.tex.quality = 0;
	bo.quiet = true;
	PakWriter writer;
	const BakeResult br = bakeWorlds(worlds, bo, writer);
	CHECK(br.report.find("things to use: 6 of 4 kinds, with 6 places to stand at (1 have none: nobody can use those): BED x3") !=
	      std::string::npos);
	const std::string path = gDir + "/mobs.pak";
	writer.write(path);
	Pak pak;
	CHECK(pak.open(path.c_str()));
	Mobs mobs;
	CHECK(!mobs.load(pak, "world/nowhere") && !mobs.loaded() && mobs.count() == 0);
	const float nowhere[3] = {0, 0, 0};
	CHECK(mobs.find(nowhere, "BED", 1) == -1 && mobs.heldBy(1) == -1 && !mobs.hold(0, 1)); // (an empty table answers too)
	mobs.drop(1);
	mobs.dropAll();
	CHECK(mobs.load(pak, "world/m") && mobs.count() == 6);
	CHECK(std::string(mobs.scheme(0)) == "BED" && mobs.mob(0).slotCount == 2 && mobs.mob(0).states == 1 && mobs.mob(4).states == 16);
	CHECK(mobs.slot(0, 0).kind == MOB_SLOT_FRONT && mobs.slot(0, 1).kind == MOB_SLOT_BACK && mobs.slot(1, 0).kind == MOB_SLOT_PLAIN &&
	      mobs.slot(5, 0).kind == MOB_SLOT_DIST);
	CHECK(mobs.isScheme(0, "bed") && !mobs.isScheme(0, "BEDHIGH") && !mobs.isScheme(4, "BED") && !mobs.isScheme(99, "BED") && !mobs.isScheme(0, ""));

	// Found: the nearest of its kind within ten metres, in any letter case; a
	// kind is a whole word ("BED" is no "BEDHIGH").
	const float here[3] = {0, 0, 0}, upstairs[3] = {100, 380, 0}, away[3] = {4200, 0, 0}, far[3] = {2500, 0, 0};
	CHECK(mobs.find(here, "BED", 1) == 0 && mobs.find(here, "Bed", 1) == 0 && mobs.find(upstairs, "BED", 1) == 1);
	CHECK(mobs.find(away, "BED", 1) == 2 && mobs.find(far, "BED", 1) == -1);
	CHECK(mobs.find(here, "BEDHIGH", 1) == 4 && mobs.find(here, "BE", 1) == -1 && mobs.find(here, "", 1) == -1 && mobs.find(here, nullptr, 1) == -1);
	CHECK(mobs.find(here, "BED", 1, 0) == 1); // not that one: the next nearest
	{
		// All of them, nearest first (the one 50 m away is out of reach).
		int all[4] = {-1, -1, -1, -1};
		CHECK(mobs.candidates(here, "BED", 1, -1, all, 4) == 2 && all[0] == 0 && all[1] == 1);
		CHECK(mobs.candidates(here, "BED", 1, -1, all, 1) == 1 && all[0] == 0);
		CHECK(mobs.candidates(upstairs, "BED", 1, -1, all, 1) == 1 && all[0] == 1); // (a full list keeps the nearest)
		CHECK(mobs.candidates(here, "BED", 1, 0, all, 4) == 1 && all[0] == 1 && mobs.candidates(here, "BED", 1, -1, all, 0) == 0);
	}
	// A thing without a place to stand at is there, but not to be used.
	CHECK(mobs.isScheme(3, "BENCH") && mobs.mob(3).slotCount == 0 && mobs.find(here, "BENCH", 1) == -1);
	// One user each, one thing each.
	CHECK(mobs.hold(0, 7) && mobs.heldBy(7) == 0 && mobs.userOf(0) == 7 && mobs.userOf(1) == -1);
	CHECK(mobs.find(here, "BED", 7) == 0 && mobs.find(here, "BED", 8) == 1); // taken for everybody else
	CHECK(!mobs.hold(0, 8) && mobs.heldBy(8) == -1 && mobs.userOf(0) == 7);
	CHECK(mobs.hold(3, 7) && mobs.heldBy(7) == 3 && mobs.userOf(0) == -1); // moving on lets go of the first
	CHECK(!mobs.hold(99, 7) && !mobs.hold(1, 0xFFFF) && mobs.heldBy(7) == 3);
	mobs.drop(7);
	CHECK(mobs.heldBy(7) == -1 && mobs.userOf(3) == -1);
	CHECK(mobs.hold(1, 3) && mobs.hold(2, 4));
	mobs.dropAll();
	CHECK(mobs.userOf(1) == -1 && mobs.userOf(2) == -1);

	// Where one stands: the nearer of a bed's two sides; where the thing is
	// if it has no places; and a pace from the cauldron on the side one
	// comes from, facing it.
	float at[3], yaw = 9;
	const float south[3] = {120, 0, -900}, north[3] = {80, 0, 700};
	CHECK(mobs.place(0, south, at, yaw) == 0 && at[2] == -120.0f && yaw == 0.0f);
	CHECK(mobs.place(0, north, at, yaw) == 1 && at[2] == 120.0f && std::fabs(yaw - 3.14159f) < 1e-5f);
	CHECK(mobs.place(3, south, at, yaw) == -1 && at[0] == -300.0f && at[2] == 0.0f && yaw == 0.5f);
	for (int k = 0; k < 40; ++k) {
		const float angle = float(k) * 0.37f, range = 150.0f + float(k) * 40.0f;
		const float from[3] = {std::sin(angle) * range, 0, 600.0f + std::cos(angle) * range};
		CHECK(mobs.place(5, from, at, yaw) == 0);
		CHECK(std::fabs(std::hypot(at[0], at[2] - 600.0f) - 90.0f) < 0.01f);                         // a pace from it
		CHECK(std::hypot(at[0] - std::sin(angle) * 90.0f, at[2] - 600.0f - std::cos(angle) * 90.0f) < 0.05f); // on one's own side
		CHECK(std::fabs(std::sin(yaw) * 90.0f + at[0]) < 0.05f && std::fabs(std::cos(yaw) * 90.0f + at[2] - 600.0f) < 0.05f); // facing it
	}
	const float onIt[3] = {0, 0, 600};
	CHECK(mobs.place(5, onIt, at, yaw) == 0 && std::isfinite(at[0]) && std::isfinite(at[2]) && std::isfinite(yaw));

	// Damaged data: refused, or at least harmless.
	const PakEntry* e = pak.find("world/m/mobs");
	CHECK(e != nullptr);
	std::vector<u8> good(e ? e->size : 0);
	if (e) CHECK(pak.read(*e, good.data()));
	int loaded = 0, refused = 0;
	for (int round = 0; round < 400 && !good.empty(); ++round) {
		std::vector<u8> bad = good;
		const int hits = 1 + int(rng() % 4);
		for (int k = 0; k < hits; ++k) {
			const size_t where = round < 100 ? 8 + rng() % 12 : 8 + rng() % (bad.size() - 8);
			bad[where] = rng() % 2 ? u8(rng()) : u8(bad[where] ^ (1u << (rng() % 8)));
		}
		if (round % 50 == 49) bad.resize(bad.size() - 1 - rng() % 40);
		PakWriter w;
		w.add("world/m/mobs", ASSET_META, bad);
		const std::string badPath = gDir + "/mobs_bad.pak";
		w.write(badPath);
		Pak bp;
		CHECK(bp.open(badPath.c_str()));
		Mobs b;
		if (!b.load(bp, "world/m")) {
			++refused;
			continue;
		}
		++loaded;
		for (u32 k = 0; k < 30; ++k) {
			const float from[3] = {float(rng() % 12000) - 6000.0f, float(rng() % 600), float(rng() % 2000) - 1000.0f};
			const int found = b.find(from, k % 2 ? "BED" : "CAULDRON", k);
			if (found < 0) continue;
			float pos[3], facing;
			const int slot = b.place(u32(found), from, pos, facing);
			CHECK(slot < int(b.mob(u32(found)).slotCount) && std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(pos[2]));
			CHECK(std::strlen(b.scheme(u32(found))) < 4096);
			b.hold(u32(found), k);
		}
	}
	std::printf("       %d damaged tables used, %d refused\n", loaded, refused);
	CHECK(loaded > 50 && refused > 50);
}

static void testLife() {
	// The built-in script, and with it somebody who asks the engine questions
	// and writes the answers down.
	std::string source = syntheticScriptSource();
	const std::string hook = "func void STARTUP_SUB_CAMP() {";
	CHECK(source.find(hook) != std::string::npos);
	source.replace(source.find(hook), hook.size(), hook + "\n\tWld_InsertNpc(VLK_502_Probe, \"CAMP_SQUARE\");");
	source += R"(
var int probe_dist_wp; var int probe_dist_none; var int probe_state; var int probe_detect; var int probe_other; var int probe_dist_npc;
var int probe_see; var int probe_time; var int probe_body; var int probe_fp; var int probe_hero; var int probe_loops; var int probe_ended;
var string probe_wp; var string probe_own; var int probe_see_first;
var int probe_mob; var int probe_mob_state; var int probe_smith_body; var int probe_sitter_body; var string probe_mob_own; var int probe_sleeper_body;
extern func int Wld_DetectNpc(var C_NPC n, var int npcInstance, var func state, var int guild);
extern func int Npc_CanSeeNpc(var C_NPC a, var C_NPC b);
func void ZS_Probe() { AI_GotoWP(self, self.wp); AI_AlignToWP(self); probe_own = self.wp; };
func int ZS_Probe_Loop() {
	probe_loops += 1;
	probe_dist_wp = Npc_GetDistToWP(self, "camp_fire");
	probe_dist_none = Npc_GetDistToWP(self, "NO_SUCH_WP");
	probe_wp = Npc_GetNearestWP(self);
	probe_state = Npc_IsInState(self, ZS_Probe) + 10 * Npc_IsInState(self, ZS_Guard);
	probe_detect = Wld_DetectNpc(self, -1, ZS_Smith, GIL_VLK);
	if (probe_detect) {
		probe_other = other.id;
		probe_dist_npc = Npc_GetDistToNpc(self, other);
		if (probe_loops == 1) { probe_see_first = Npc_CanSeeNpc(self, other) + 10 * Npc_CanSeeNpc(other, self); };
		if (probe_loops == 3) { AI_TurnToNpc(self, other); };
		probe_see = Npc_CanSeeNpc(self, other) + 10 * Npc_CanSeeNpc(other, self);
	};
	// The things around: which kinds are free to use, and what state they are in.
	probe_mob = Wld_IsMobAvailable(self, "BENCH") + 10 * Wld_IsMobAvailable(self, "bsanvil") + 100 * Wld_IsMobAvailable(self, "BEDHIGH")
	          + 1000 * Wld_IsMobAvailable(self, "NOSUCH") + 10000 * Wld_IsMobAvailable(self, "");
	probe_mob_state = Wld_GetMobState(self, "Bench") + 10 * Wld_GetMobState(self, "BEDHIGH");
	probe_mob_own = Npc_GetDetectedMob(self);
	if (probe_detect) { probe_smith_body = Npc_GetBodyState(other); };
	if (Wld_DetectNpc(self, VLK_500_Digger, ZS_SitAround, -1)) { probe_sitter_body = Npc_GetBodyState(other); };
	probe_hero = Npc_GetDistToNpc(self, hero);
	probe_time = Npc_GetStateTime(self);
	probe_body = Npc_GetBodyState(self);
	probe_fp = Wld_IsFPAvailable(self, "SMALLTALK") + 10 * Wld_IsFPAvailable(self, "NOSUCH") + 100 * Npc_IsOnFP(self, "SMALLTALK");
	probe_sleeper_body = -1;
	if (Wld_DetectNpc(self, GRD_201_Jackal, ZS_SleepBed, -1)) { probe_sleeper_body = Npc_GetBodyState(other); };
	AI_Wait(self, 1);
	return LOOP_CONTINUE;
};
func void ZS_Probe_End() { probe_ended += 1; };
instance VLK_502_Probe (Npc_Default) {
	name = "Probe"; guild = GIL_VLK; id = 502; senses_range = 3000;
	Mdl_SetVisual(self, "HUMANS.MDS");
	Mdl_SetVisualBody(self, "hum_body_Naked0", 3, 1, "Hum_Head_Pony", 9, 2, -1);
	daily_routine = Rtn_start_502;
};
func void Rtn_start_502() {
	TA_Min(self, 0, 0, 22, 0, ZS_Probe, "CAMP_SQUARE");
	TA_Min(self, 22, 0, 24, 0, ZS_Stand, "CAMP_SQUARE");
};
)";
	std::vector<BakeWorld> worlds;
	const std::string path = gDir + "/life.pak";
	{
		const BakeResult r = bakeLifeWorlds(source, path, worlds);
		const auto said = [&](const char* text) { return r.report.find(text) != std::string::npos; };
		if (getenv("G3_TRACE")) std::puts(r.report.c_str());
		// Of the animations the scripts name each skeleton got the ones it has.
		CHECK(said("animations: the scripts name or imply 9, and using the things they name takes 16 families more; fighting 346 "
		           "more; baked per skeleton: humans 78, scavenger 13, molerat 13"));
		// ... and those of the things in the hands (drinking a potion; picking up: 3 + 3 of the 67).
		CHECK(said("things in the hands: the scripts' items are used in 4 ways (FOOD, MEAT, POTION, POTIONFAST)"));
		// The things of the camp, and which of them the scripts know.
		CHECK(said("things to use: 9 of 7 kinds, with 11 places to stand at (0 have none: nobody can use those): CHESTSMALL x2, BEDHIGH x2"));
		CHECK(said("things to use: the worlds have 7 kinds; the scripts name 4 of them: BEDHIGH x2, BEDLOW x1, BENCH x1, BSANVIL x1; not named: BSFIRE, CHESTBIG, CHESTSMALL"));
		CHECK(said("way net: 13 points, 15 connections") && said("10 free points") && said(" 12 states, "));
		// What the states need of the engine is on record.
		CHECK(said("life: the scripts have 12 states; the people of these worlds are put into 7: ZS_MM_ALLSCHEDULER x6, ZS_GUARD x3"));
		CHECK(said("life: the states call 45 engine functions, 45 of which the engine carries out; the others do nothing yet, by the number of states that call them: none"));
	}
	const SrcScene& scene = worlds[0].scene; // (its places are on the ground now)
	const auto wayPoint = [&](const char* name) -> const SrcWayPoint& {
		for (const SrcWayPoint& wp : scene.wayPoints)
			if (wp.name == name) return wp;
		throw std::logic_error(std::string("no way point ") + name);
	};
	// How far a place is from the nearest connection of the net.
	const auto offTheNet = [&](const float pos[3]) {
		float best = 1e30f;
		for (const auto& [a, b] : scene.wayEdges) {
			const float* p = scene.wayPoints[a].pos;
			const float* q = scene.wayPoints[b].pos;
			const float ex = q[0] - p[0], ez = q[2] - p[2];
			const float t = std::clamp(((pos[0] - p[0]) * ex + (pos[2] - p[2]) * ez) / (ex * ex + ez * ez), 0.0f, 1.0f);
			best = std::min(best, std::hypot(pos[0] - (p[0] + ex * t), pos[2] - (p[2] + ez * t)));
		}
		return best;
	};
	const i32 kBsStand = (1 << 15) | (1 << 16), kBsSit = 11 | (1 << 16);
	const auto mobOf = [&](const char* kind, int nth) -> const SrcMob& {
		for (const SrcMob& m : scene.mobs)
			if (m.scheme == kind && nth-- == 0) return m;
		throw std::logic_error(std::string("no ") + kind);
	};

	gfx::init();
	for (int variant = 0; variant < 4; ++variant) {
		// The same day at three frame rates; and once with next to no memory
		// for animations.
		const float dt = variant == 1 ? 1.0f / 20.0f : variant == 2 ? 1.0f / 60.0f : 1.0f / 30.0f;
		const bool poor = variant == 3;
		App app;
		CHECK(app.init(path.c_str(), nullptr));
		app.world().preload(app.camera().pos);
		Npcs& npcs = app.npcs();
		app.world().config().fadeMs = 0.0f; // (they come and go at once here)
		const Game& game = npcs.game();
		const WayNet& net = npcs.wayNet();
		CHECK(net.loaded() && net.nodeCount() == 13 && net.freeCount() == 10);
		const Mobs& mobs = npcs.mobs();
		CHECK(mobs.loaded() && mobs.count() == 9);
		// What a character is at: the thing's kind if it is in it, "" if not.
		const auto inThing = [&](u32 npc) -> std::string {
			const NpcActor* a = npcs.actorOf(npc);
			return a && a->mob >= 0 && a->mobStage == NpcActor::MOB_IN ? mobs.scheme(u32(a->mob)) : "";
		};
		npcs.config().range = 20000.0f;
		npcs.config().maxShown = 16;
		// (A day of the camp's life: the creatures by the wood would see the
		// guards and fight them. Fighting has a test of its own.)
		npcs.config().fights = false;
		if (poor) npcs.config().clipBudget = 9000;
		app.setDrawDistance(20000.0f);
		app.environment().timeOfDay = 8.0f;
		const auto who = [&](const char* instance) -> u32 {
			const u32 sym = game.vm().find(instance);
			for (u32 i = 0; i < game.npcCount(); ++i)
				if (game.npc(i).symbol == sym) return i;
			throw std::logic_error(std::string("nobody is ") + instance);
		};
		const u32 thorus = who("GRD_200_THORUS"), jackal = who("GRD_201_JACKAL"), smith = who("VLK_500_DIGGER"),
		          walker = who("VLK_501_DIGGER"), probe = who("VLK_502_PROBE");
		const auto global = [&](const char* name) { return game.vm().getInt(game.vm().find(name)); };
		const auto text = [&](const char* name) { return std::string(game.vm().getString(game.vm().find(name))); };
		const auto away = [&](u32 npc, const float pos[3]) {
			return std::hypot(game.npc(npc).pos[0] - pos[0], game.npc(npc).pos[2] - pos[2]);
		};
		const auto trace = [&](const char* when) {
			if (variant != 0) return;
			std::printf("       -- %s (%.2f h): %u actors, %u walking; %u state calls, %u actions, %u clips read (%u KB)\n", when,
			            double(app.environment().timeOfDay), npcs.stats().actors, npcs.stats().walking, npcs.stats().stateCalls,
			            npcs.stats().actions, npcs.stats().clips, npcs.stats().clipBytes / 1024);
			for (u32 i = 0; i < npcs.actorCount(); ++i) {
				const NpcActor& a = npcs.actor(i);
				if (!a.used) continue;
				const GameNpc& n = game.npc(a.npc);
				std::printf("          %-15s %-7s at %5.0f %4.0f %5.0f facing %5.2f; way point %2d, free point %2d\n", game.nameOf(n),
				            npcs.doing(a), double(n.pos[0]), double(n.pos[1]), double(n.pos[2]), double(n.yaw), a.atNode, a.atFree);
			}
		};
		// Runs the game on, watching one of the characters: how fast it goes,
		// how far it strays from the paths, whether it stays on the ground.
		struct Watch {
			float fastest = 0, farthestOff = 0, worstGround = 0, travelled = 0;
			bool throughGate = false;
		};
		const auto run = [&](float seconds, u32 watched, Watch* w, const float* until = nullptr) {
			for (int i = 0; i < int(seconds / dt); ++i) {
				const GameNpc before = game.npc(watched);
				app.update(Input {}, dt);
				if (i % 7 == 0) app.render();
				if (!w) continue;
				const GameNpc& n = game.npc(watched);
				const float step = std::hypot(n.pos[0] - before.pos[0], n.pos[2] - before.pos[2]);
				w->fastest = std::max(w->fastest, step / dt);
				w->travelled += step;
				if (!until || away(watched, until) > 260.0f) w->farthestOff = std::max(w->farthestOff, offTheNet(n.pos));
				w->throughGate = w->throughGate || (std::fabs(n.pos[0]) < 60.0f && std::fabs(n.pos[2] - 2600.0f) < 60.0f);
				float y;
				Vec3 normal;
				if (app.world().rayDown({n.pos[0], n.pos[1] + 100.0f, n.pos[2]}, 400.0f, y, normal))
					w->worstGround = std::max(w->worstGround, std::fabs(y - n.pos[1]));
			}
		};

		// ---- morning: everybody takes up what the routine says ----
		run(16.0f, thorus, nullptr);
		trace("morning");
		CHECK(npcs.stats().actors == 9 && npcs.stats().waiting == 0 && npcs.stats().noModel == 0);
		const u32 zsGuard = game.vm().find("ZS_GUARD"), zsSit = game.vm().find("ZS_SITAROUND"), zsSleep = game.vm().find("ZS_SLEEP");
		for (const u32 guard : {thorus, jackal}) {
			const NpcActor* a = npcs.actorOf(guard);
			CHECK(a && a->ready && a->state == zsGuard && a->started && (poor || std::string(a->stance) == "LGUARD"));
			if (!a) continue;
			const SrcWayPoint& post = wayPoint(guard == thorus ? "CAMP_GATE_GUARD_L" : "CAMP_GATE_GUARD_R");
			CHECK(away(guard, post.pos) < 60.0f && std::fabs(game.npc(guard).yaw - post.yaw) < 0.01f && a->atNode >= 0);
			CHECK(std::string(net.nodeName(u32(a->atNode))) == post.name && game.npc(guard).active);
			if (!poor) CHECK(a->anim.current() && (a->anim.current() == a->rig->model.clip("S_LGUARD") || a->phase == NpcActor::PLAYING));
		}
		CHECK(away(walker, wayPoint("ROAD_1").pos) < 60.0f);
		{
			// The smith is at his anvil: a pace from it, on the side he came
			// from, facing it - and in the one state such a thing has.
			const NpcActor* a = npcs.actorOf(smith);
			const SrcMob& anvil = mobOf("BSANVIL", 0);
			if (poor) {
				// (No memory for the animation: he stands where he was sent, and leaves the anvil be.)
				CHECK(a && a->mob == -1 && a->mobFailed >= 0 && mobs.isScheme(u32(a->mobFailed), "BSANVIL") && npcs.stats().mobsFailed == 1);
				CHECK(away(smith, anvil.pos) < 85.0f && mobs.heldBy(smith) == -1);
			} else {
				CHECK(inThing(smith) == "BSANVIL" && a->mobState == 0 && a->mobKind == MOB_SLOT_DIST && mobs.heldBy(smith) == a->mob);
				CHECK(std::fabs(away(smith, anvil.pos) - 75.0f) < 1.0f && game.npc(smith).pos[0] > anvil.pos[0]);
				const GameNpc& n = game.npc(smith);
				CHECK(std::fabs(n.yaw - std::atan2(anvil.pos[0] - n.pos[0], anvil.pos[2] - n.pos[2])) < 0.02f);
				CHECK(std::string(a->stance) == "BSANVIL_S0" && a->anim.current() &&
				      (a->anim.current() == a->rig->model.clip("S_BSANVIL_S0") || a->phase == NpcActor::PLAYING));
				CHECK(npcs.stats().mobsUsed == 1 && npcs.stats().mobsInUse == 1 && npcs.stats().mobsFailed == 0);
			}
		}
		// The creatures are in the state their scheduler chose, each at a spot of its own.
		{
			std::set<int> spots;
			u32 roaming = 0;
			for (u32 i = 0; i < npcs.actorCount(); ++i) {
				const NpcActor& a = npcs.actor(i);
				if (!a.used || game.npc(a.npc).routineCount) continue;
				CHECK(a.fist && a.state == game.vm().find("ZS_MM_RTN_ROAM") && a.prevState == game.vm().find("ZS_MM_ALLSCHEDULER"));
				++roaming;
				if (a.atFree >= 0) {
					CHECK(net.nameContains(u32(a.atFree), "FP_ROAM") && net.heldBy(a.npc) == a.atFree);
					spots.insert(a.atFree);
				}
			}
			CHECK(roaming == 4 && spots.size() >= 3);
		}
		// What the engine told the one who asked.
		CHECK(global("PROBE_LOOPS") >= 10 && global("PROBE_ENDED") == 0 && text("PROBE_OWN") == "CAMP_SQUARE");
		CHECK(std::abs(global("PROBE_DIST_WP") - 610) < 60 && global("PROBE_DIST_NONE") == INT_MAX && text("PROBE_WP") == "CAMP_SQUARE");
		CHECK(global("PROBE_STATE") == 1 && global("PROBE_DETECT") == 1 && global("PROBE_OTHER") == 500);
		CHECK(std::abs(global("PROBE_DIST_NPC") - 618) < 60);
		// The square's way point faces +X and the smithy, to its west, faces
		// west: at first neither sees the other. Then the one turns round.
		CHECK(global("PROBE_SEE_FIRST") == 0 && global("PROBE_SEE") == 1);
		{
			const GameNpc& p = game.npc(probe);
			const GameNpc& o = game.npc(smith);
			CHECK(std::fabs(p.yaw - std::atan2(o.pos[0] - p.pos[0], o.pos[2] - p.pos[2])) < 0.02f);
		}
		CHECK(global("PROBE_BODY") == kBsStand && global("PROBE_FP") == 1 && global("PROBE_TIME") >= 10);
		// Of the things within ten metres the bench is free and the anvil is
		// the smith's; so is the second hut's bed (the first is too far away);
		// a free thing is in its first state, one that is not there in none.
		// And the smith's body is busy with a thing (one that is neither sat
		// on nor lain in).
		CHECK(global("PROBE_MOB") == (poor ? 111 : 101) && global("PROBE_MOB_STATE") == 0 + 0 && text("PROBE_MOB_OWN").empty());
		CHECK(global("PROBE_SMITH_BODY") == (poor ? kBsStand : (16 | (1 << 15))));
		{
			const Vec3 eye = app.player().eye();
			const GameNpc& p = game.npc(probe);
			const float d = std::sqrt((p.pos[0] - eye.x) * (p.pos[0] - eye.x) + (p.pos[1] - eye.y + 170.0f) * (p.pos[1] - eye.y + 170.0f) +
			                          (p.pos[2] - eye.z) * (p.pos[2] - eye.z));
			CHECK(std::fabs(float(global("PROBE_HERO")) - d) < 3.0f);
		}

		// ---- evening: the routines move on, and people walk ----
		const float thorusTime = npcs.actorOf(thorus) ? npcs.actorOf(thorus)->stateTime : 0.0f;
		app.environment().timeOfDay = 20.5f;
		Watch w;
		const SrcWayPoint& fire = wayPoint("CAMP_FIRE");
		run(8.0f, jackal, &w, fire.pos);
		trace("eight seconds into the evening");
		CHECK(npcs.actorOf(jackal) && npcs.actorOf(jackal)->phase == NpcActor::MOVING && npcs.stats().walking >= 2);
		CHECK(npcs.actorOf(jackal)->state == zsSit && npcs.actorOf(jackal)->prevState == zsGuard && npcs.actorOf(jackal)->stance[0] == 0);
		run(34.0f, jackal, &w, fire.pos);
		trace("evening");
		{
			// The guard has walked from the gate to the fire: through the gate
			// and along the paths, at a walk, on the ground - and sat down.
			// (The digger from the smithy was there first and has the bench.)
			const NpcActor* a = npcs.actorOf(jackal);
			CHECK(a && a->atFree >= 0 && net.nameContains(u32(a->atFree), "SIT") && (poor || std::string(a->stance) == "SIT"));
			if (variant == 0 || getenv("G3_TRACE"))
				std::printf("       the guard's way to the fire: %.0f cm, at most %.0f cm/s, %.0f cm off the paths, %.1f cm off the ground\n",
				            double(w.travelled), double(w.fastest), double(w.farthestOff), double(w.worstGround));
			CHECK(w.throughGate && w.travelled > 3200.0f && w.travelled < 4200.0f && w.farthestOff < 100.0f && w.worstGround < 3.0f);
			CHECK(w.fastest < 260.0f); // S_WALKL goes at 150 cm/s
			if (a && a->atFree >= 0) {
				const FreePoint& seat = net.freePoint(u32(a->atFree));
				CHECK(away(jackal, seat.pos) < 80.0f && std::fabs(game.npc(jackal).yaw - seat.yaw) < 0.01f);
				if (!poor) CHECK(a->bodyState == kBsSit);
			}
			// The digger from the smithy has left his anvil and sits on the
			// bench - a step back from where he stood in front of it, his back
			// to it; the one from the road stands at the fire's way point; the
			// first guard has not moved.
			const NpcActor* b = npcs.actorOf(smith);
			CHECK(a && net.heldBy(jackal) == a->atFree && b);
			if (poor) {
				// (Without the animations: on the ground, like the guard.)
				CHECK(b && b->mob == -1 && b->atFree >= 0 && a && b->atFree != a->atFree);
			} else if (b) {
				const SrcMob& bench = mobOf("BENCH", 0);
				CHECK(inThing(smith) == "BENCH" && b->mobState == 1 && std::string(b->stance) == "BENCH_S1" && b->atFree == -1);
				CHECK(b->anim.current() == b->rig->model.clip("S_BENCH_S1"));
				{
					// Down on the seat, though "S_BENCH_S1" has no root track.
					Pose rest;
					b->rig->model.restPose(rest);
					CHECK(b->anim.pose().pos[0].y < rest.pos[0].y - 35.0f);
				}
				bool onSlot = false;
				for (const SrcMobSlot& slot : bench.slots) {
					const float backX = slot.pos[0] - std::sin(slot.yaw) * 40.0f, backZ = slot.pos[2] - std::cos(slot.yaw) * 40.0f;
					onSlot = onSlot || (std::hypot(game.npc(smith).pos[0] - backX, game.npc(smith).pos[2] - backZ) < 5.0f &&
					                    std::fabs(game.npc(smith).yaw - slot.yaw) < 0.01f);
				}
				if (!onSlot)
					std::printf("       on the bench at %.1f %.1f facing %.3f; slots at %.1f %.1f (%.3f) and %.1f %.1f\n", double(game.npc(smith).pos[0]),
					            double(game.npc(smith).pos[2]), double(game.npc(smith).yaw), double(bench.slots[0].pos[0]),
					            double(bench.slots[0].pos[2]), double(bench.slots[0].yaw), double(bench.slots[1].pos[0]), double(bench.slots[1].pos[2]));
				CHECK(onSlot && mobs.userOf(u32(b->mob)) == int(smith) && net.heldBy(smith) == -1);
				// On the ground, though the places are a metre up.
				{
					const GameNpc& n = game.npc(smith);
					float y;
					Vec3 normal;
					CHECK(app.world().rayDown({n.pos[0], n.pos[1] + 150.0f, n.pos[2]}, 400.0f, y, normal) && std::fabs(y - n.pos[1]) < 3.0f);
				}
				// What the scripts are told: the bench is taken, the anvil
				// free again, and the one on the bench is sitting.
				CHECK(global("PROBE_MOB") == 110 && global("PROBE_MOB_STATE") == -1 + 0 && global("PROBE_SITTER_BODY") == kBsSit);
				// The scripts are told what his body is doing: sitting.
				CHECK(npcs.stats().mobsUsed == 2 && npcs.stats().mobsInUse == 1);
			}
			const NpcActor* c = npcs.actorOf(walker);
			CHECK(c && c->atNode >= 0 && std::string(net.nodeName(u32(c->atNode))) == "CAMP_FIRE" && away(walker, fire.pos) < 60.0f);
			const NpcActor* t = npcs.actorOf(thorus);
			CHECK(t && t->state == zsGuard && t->stateTime > thorusTime + 40.0f && away(thorus, wayPoint("CAMP_GATE_GUARD_L").pos) < 60.0f);
		}

		// ---- night: to the huts; the creatures lie down ----
		app.environment().timeOfDay = 23.75f;
		Watch night;
		run(45.0f, jackal, &night);
		trace("night");
		{
			// To bed: the state that walks to the hut hands over to the one
			// that gets into the bed there - from its front, which is where
			// the hut's way point is nearest to.
			const NpcActor* a = npcs.actorOf(jackal);
			const u32 zsSleepBed = game.vm().find("ZS_SLEEPBED");
			if (poor) {
				// (No memory for the bed's animations: as in the original, the
				// sleeper who is not in bed three seconds on goes to sit.)
				CHECK(a && a->state == zsSit && a->prevState == zsSleepBed && a->mob == -1);
			} else {
				CHECK(a && a->state == zsSleepBed && a->prevState == zsSleep && a->atFree == -1 && net.heldBy(jackal) == -1);
				CHECK(away(jackal, wayPoint("CAMP_HUT_1").pos) < 70.0f);
			}
			if (!poor && a) {
				const SrcMob& bed = mobOf("BEDHIGH", 0);
				CHECK(inThing(jackal) == "BEDHIGH" && a->mobState == 1 && a->mobKind == MOB_SLOT_FRONT && std::string(a->stance) == "BEDHIGH_S1");
				CHECK(away(jackal, bed.slots[0].pos) < 1.0f && std::fabs(game.npc(jackal).yaw - bed.slots[0].yaw) < 0.01f);
				CHECK(a->anim.current() == a->rig->model.clip("S_BEDHIGH_S1"));
				// The other bed has the digger in it; the bench and the anvil are free again.
				CHECK(inThing(smith) == "BEDHIGH" && npcs.actorOf(smith)->mob != a->mob && npcs.stats().mobsInUse == 2);
				u32 free = 0;
				for (u32 m = 0; m < mobs.count(); ++m) free += mobs.userOf(m) < 0;
				CHECK(free == 7 && npcs.stats().mobsUsed == 4);
			}
			CHECK((poor || night.farthestOff < 190.0f) && night.travelled > 1000.0f); // (the seat is 1.7 m off the path)
			u32 asleep = 0;
			for (u32 i = 0; i < npcs.actorCount(); ++i) {
				const NpcActor& c = npcs.actor(i);
				if (c.used && c.fist) asleep += c.state == game.vm().find("ZS_MM_RTN_SLEEP") && (poor || std::string(c.stance) == "SLEEP");
			}
			CHECK(asleep == 4);
			// The one who asked has been through the end of his state, once.
			CHECK(global("PROBE_ENDED") == 1 && npcs.actorOf(probe) && npcs.actorOf(probe)->state == game.vm().find("ZS_STAND"));
		}

		// ---- deep in the night: the sleepers stay in their beds ----
		// (They used to get up at once: in a bed the body was "lying", which
		// the original's sleep state takes for "not in bed" - and the second
		// kind of bed it asks for got the sleeper out of the first.)
		if (!poor) {
			app.environment().timeOfDay = 3.0f;
			u32 outOfBed = 0, notBusy = 0;
			const u32 used = npcs.stats().mobsUsed;
			for (int second = 0; second < 40; ++second) {
				run(1.0f, jackal, nullptr);
				for (const u32 sleeper : {jackal, smith}) {
					const NpcActor* a = npcs.actorOf(sleeper);
					outOfBed += !a || inThing(sleeper) != "BEDHIGH" || std::string(a->stance) != "BEDHIGH_S1" ||
					            a->anim.current() != a->rig->model.clip("S_BEDHIGH_S1");
					// Lying: "S_BEDHIGH_S1" has no track for the root, which
					// stays as the step into the bed left it - down and tipped
					// over -, not upright at standing height (asleep standing
					// in the air, as on the console).
					if (a && a->rig) {
						Pose rest;
						a->rig->model.restPose(rest);
						const Quat r = a->anim.pose().rot[0];
						outOfBed += a->anim.pose().pos[0].y > rest.pos[0].y - 25.0f || std::fabs(r.w) > 0.9f;
					}
				}
				notBusy += second >= 3 && global("PROBE_SLEEPER_BODY") != 15; // (the one who asks has a state to begin first)
			}
			if (variant == 0) std::printf("       deep in the night: %u out of bed, %u not busy, %u more uses\n", outOfBed, notBusy, npcs.stats().mobsUsed - used);
			CHECK(outOfBed == 0 && notBusy == 0 && npcs.stats().mobsUsed == used && npcs.actorOf(jackal)->state == game.vm().find("ZS_SLEEPBED"));
			// What the report says of him.
			const NpcActor* a = npcs.actorOf(jackal);
			CHECK(a && std::string(npcs.stateName(a->state)) == "ZS_SLEEPBED" && std::string(npcs.animationName(*a)) == "S_BEDHIGH_S1" &&
			      npcs.bodyState(*a) == 15 && a->mobStage == NpcActor::MOB_IN && npcs.stateName(0)[0] == 0);
			CHECK(npcs.nearest() != nullptr);
			// The low bed is near, and free - but behind the hut's wall: not his.
			const GameNpc& j = game.npc(jackal);
			const int low = npcs.mobs().find(j.pos, "BEDLOW", jackal);
			CHECK(low >= 0 && npcs.visibleMob(app.world(), jackal, "BEDLOW") == -1);
			if (low >= 0) {
				float at[3], yaw;
				npcs.mobs().place(u32(low), j.pos, at, yaw);
				CHECK(!app.world().lineFree({j.pos[0], j.pos[1] + 130.0f, j.pos[2]}, {at[0], at[1] + 80.0f, at[2]}));
				CHECK(app.world().lineFree({j.pos[0], j.pos[1] + 130.0f, j.pos[2]}, {j.pos[0] - 300.0f, j.pos[1] + 130.0f, j.pos[2]}));
			}
		}

		// ---- and morning again: back to the posts ----
		app.environment().timeOfDay = 8.0f;
		run(50.0f, jackal, nullptr);
		trace("the next morning");
		{
			const NpcActor* a = npcs.actorOf(jackal);
			CHECK(a && a->state == zsGuard && away(jackal, wayPoint("CAMP_GATE_GUARD_R").pos) < 60.0f && (poor || std::string(a->stance) == "LGUARD"));
			// Out of bed and off: nothing of the bed sticks to him, and he
			// walked from where he had stood before getting in.
			CHECK(a && a->mob == -1 && a->mobStage == NpcActor::MOB_NONE && mobs.heldBy(jackal) == -1 && a->grounded);
			if (!poor) {
				CHECK(inThing(smith) == "BSANVIL" && npcs.stats().mobsInUse == 1 && npcs.stats().mobsUsed == 5);
				u32 used = 0;
				for (u32 m = 0; m < mobs.count(); ++m) used += mobs.userOf(m) >= 0;
				CHECK(used == 1);
			}
			u32 awake = 0;
			for (u32 i = 0; i < npcs.actorCount(); ++i) {
				const NpcActor& c = npcs.actor(i);
				if (c.used && c.fist) awake += c.state == game.vm().find("ZS_MM_RTN_ROAM") && c.stance[0] == 0;
			}
			CHECK(awake == 4);
		}
		const NpcStats ns = npcs.stats();
		if (variant == 0 || poor)
			std::printf("       %s: %u ways found, %u not, %u put at their goal, %u actions dropped, %u animations missing, %u refused; "
			            "%u animations read, %u KB; scripts: %u/%u complaints, %u through no object\n",
			            poor ? "with 9 KB for animations" : "a day", ns.paths, ns.noPath, ns.snapped, ns.queueFull, ns.clipsMissing,
			            ns.clipsRefused, ns.clips, ns.clipBytes / 1024, game.vm().stats().errors, game.vm().stats().aborted,
			            game.vm().stats().nullAccess);
		CHECK(ns.paths >= 6 && ns.noPath == 0 && ns.snapped == 0 && ns.queueFull == 0 && ns.clipsMissing == 0);
		CHECK(poor ? ns.mobsFailed >= 3 && ns.mobsUsed == ns.mobsFailed : ns.mobsFailed == 0);
		CHECK(game.vm().stats().errors == 0 && game.vm().stats().aborted == 0 && game.vm().stats().nullAccess == 0);
		// (What everybody stands in was read before the budget was cut.)
		CHECK(poor ? (ns.clipsRefused > 0 && ns.clips <= 4) : (ns.clipsRefused == 0 && ns.clips >= 15 && ns.clipBytes > 40000));

		// ---- what they notice ----
		if (variant == 0) {
			const u32 heroIndex = u32(game.hero() - &game.npc(0));
			// The guilds' table and the creatures' own lines, as the start-up set them.
			CHECK(game.attitude(thorus, heroIndex) == 2 && game.attitude(heroIndex, thorus) == 2 && game.attitude(thorus, jackal) == 3);
			CHECK(game.guildAttitude(20, 21) == 2 && game.guildAttitude(20, 0) == 0 && game.guildAttitude(99, 0) == 2);
			// The creatures saw the player at the gate, an enemy, and would
			// have gone for him: there is no fighting, they roam on.
			CHECK(global("MM_ENEMY_SEEN") > 0 && npcs.stats().fightsRefused > 0);
			u32 targeting = 0;
			for (u32 i = 0; i < npcs.actorCount(); ++i) {
				const NpcActor& c = npcs.actor(i);
				if (!c.used || !c.fist) continue;
				CHECK(c.state != game.vm().find("ZS_MM_ATTACK"));
				targeting += game.npc(c.npc).target == int(heroIndex);
			}
			CHECK(targeting >= 1);
			// The player walks up to the gate guard, to his left: he turns his
			// head to him, asks what he wants and tells the other guard, who
			// looks at him.
			const SrcWayPoint& post = wayPoint("CAMP_GATE_GUARD_L");
			const u32 perceivedBefore = npcs.stats().perceived;
			// (What he says is a line of his voice, said in the world: it is asked for at his place.)
			static u32 saidHash = 0, saidWho = 0xFFFFFFFFu;
			static int saidBy = -1;
			saidWho = thorus;
			saidHash = 0;
			Npcs::SpeechHook listen;
			listen.say = [](void*, u32 hash, int who, bool) -> float {
				if (u32(who) == saidWho) {
					saidHash = hash;
					saidBy = who;
				}
				return 0.5f;
			};
			npcs.setSpeechHook(listen);
			const u32 saidBefore = npcs.stats().linesSaid;
			app.teleport({post.pos[0] + 300.0f, post.pos[1] + 170.0f, post.pos[2] + 400.0f});
			run(3.0f, thorus, nullptr);
			const NpcActor* t = npcs.actorOf(thorus);
			const NpcActor* j = npcs.actorOf(jackal);
			CHECK(t && t->lookAt == int(heroIndex) && std::string(t->said) == "$WHATDOYOUWANT" && npcs.stats().perceived > perceivedBefore);
			if (t) std::printf("       the guard looks at the player: head %.2f rad round, %.2f down\n", double(t->headYaw), double(t->headPitch));
			CHECK(t && t->headYaw > 0.4f && t->headYaw < 0.8f && t->state == zsGuard);
			// $WHATDOYOUWANT of his voice is the line "SVM_<voice>_WHATDOYOUWANT"; it was said where he stands (the other guard, who also saw the player, said his own).
			{
				const i32 voice = game.npcInt(game.npc(thorus), game.vm().findMember("C_NPC", "VOICE"));
				char want[48];
				std::snprintf(want, sizeof(want), "SVM_%d_WHATDOYOUWANT", int(voice));
				CHECK(voice > 0 && saidHash == hashName(want) && saidBy == int(thorus));
				CHECK(npcs.stats().linesSaid > saidBefore && npcs.stats().linesHeard >= 1);
			}
			CHECK(j && j->lookAt == int(thorus) && npcs.stats().looking >= 2);
			// ... the head is turned in the pose that is drawn (the rig's head bone).
			app.render();
			// And when the player has gone, the guard looks ahead again.
			app.teleport({post.pos[0], post.pos[1] + 170.0f, post.pos[2] + 2500.0f});
			run(4.0f, thorus, nullptr);
			t = npcs.actorOf(thorus);
			CHECK(t && t->lookAt == -1 && std::fabs(t->headYaw) < 0.05f);
		}

		// ---- the player leaves: everybody is simply where his routine has him ----
		if (variant == 0) {
			app.environment().timeOfDay = 20.5f;
			run(6.0f, jackal, nullptr); // on their way to the fire
			CHECK(npcs.actorOf(jackal) && npcs.actorOf(jackal)->phase == NpcActor::MOVING);
			npcs.config().range = 400.0f;
			app.teleport({-7000.0f, 900.0f, -7000.0f});
			run(0.5f, jackal, nullptr);
			CHECK(npcs.stats().actors == 0 && npcs.stats().near == 0 && npcs.actorOf(jackal) == nullptr);
			u32 atFire = 0;
			for (const u32 npc : {jackal, smith, walker}) {
				CHECK(!game.npc(npc).active && net.heldBy(npc) == -1 && mobs.heldBy(npc) == -1);
				atFire += away(npc, fire.pos) < 200.0f && std::string(game.text(game.npc(npc).at)) == "CAMP_FIRE";
			}
			CHECK(atFire == 3);
			// Coming back, they begin where the routine has them.
			npcs.config().range = 20000.0f;
			run(20.0f, jackal, nullptr);
			// (All three begin at the fire now: whoever asks first has the bench.)
			CHECK(npcs.stats().actors == 9 && npcs.actorOf(jackal));
			u32 onBench = 0, onGround = 0;
			for (const u32 npc : {jackal, smith}) {
				onBench += inThing(npc) == "BENCH";
				onGround += npcs.actorOf(npc) && std::string(npcs.actorOf(npc)->stance) == "SIT";
			}
			CHECK(onBench == 1 && onGround == 1);
			// Into the cave and back while they live: nothing left behind.
			CHECK(app.enterWorld(hashName("world/cave"), nullptr, 0));
			run(8.0f, jackal, nullptr);
			CHECK(npcs.stats().actors == 3 && npcs.wayNet().nodeCount() == 4 && !game.npc(jackal).active);
			u32 caveGuard = 0;
			for (u32 i = 0; i < npcs.actorCount(); ++i)
				caveGuard += npcs.actor(i).used && npcs.actor(i).state == zsGuard && std::string(npcs.actor(i).stance) == "LGUARD";
			CHECK(caveGuard == 1);
			CHECK(app.enterWorld(hashName("world/world"), nullptr, 0));
			run(3.0f, jackal, nullptr);
			CHECK(npcs.stats().actors == 9);
		}
		app.shutdown();
	}
	const gfx::MemStats leak = gfx::memStats();
	CHECK(leak.meshCount == 0 && leak.textureCount == 0);

	// A world whose way net or scripts are damaged: people stand, nothing breaks.
	{
		Pak src;
		CHECK(src.open(path.c_str()));
		for (const char* victim : {"world/world/waynet", "world/world/mobs", "script/gothic", "anim/npc.rig0/s_walkl", "anim/npc.rig0/t_bench_s0_2_s1"}) {
			for (int round = 0; round < 6; ++round) {
				PakWriter w;
				bool hit = false;
				for (u32 i = 0; i < src.count(); ++i) {
					const PakEntry& e = src.entry(i);
					std::vector<u8> data(e.size);
					CHECK(src.read(e, data.data()));
					if (e.hash == hashName(victim)) {
						hit = true;
						std::mt19937 rng(u32(round) * 77u + e.size);
						if (round == 0)
							data.resize(data.size() / 2);
						else
							for (int k = 0; k < 1 + round * 3; ++k) data[16 + rng() % (data.size() - 16)] = u8(rng());
					}
					w.add(src.name(e), AssetType(e.type), data);
				}
				CHECK(hit);
				const std::string badPath = gDir + "/life_bad.pak";
				w.write(badPath);
				App app;
				CHECK(app.init(badPath.c_str(), nullptr));
				app.world().preload(app.camera().pos);
				app.npcs().game().setQuiet(true);
				app.npcs().config().range = 20000.0f;
				app.npcs().config().maxShown = 16;
				app.setDrawDistance(20000.0f);
				for (const float hour : {8.0f, 20.5f, 23.75f}) {
					app.environment().timeOfDay = hour;
					for (int i = 0; i < 240; ++i) {
						app.update(Input {}, 1.0f / 30.0f);
						if (i % 16 == 0) app.render();
					}
				}
				for (u32 i = 0; i < app.npcs().game().npcCount(); ++i) {
					const GameNpc& n = app.npcs().game().npc(i);
					CHECK(std::isfinite(n.pos[0]) && std::isfinite(n.pos[1]) && std::isfinite(n.pos[2]) && std::isfinite(n.yaw));
				}
				app.shutdown();
			}
		}
	}

	// Scripts that do everything wrong: states that start themselves, send
	// people to places that are not there, to nobody, fill the queue, empty
	// it, set clocks back. Nothing may break, and time must not stand still.
	{
		std::string chaos = syntheticScriptSource();
		std::string inserts = hook;
		for (const char* at : {"CAMP_SQUARE", "CAMP_FIRE", "CAMP_SMITH", "ROAD_1", "NO_SUCH_PLACE", "CAMP_GATE_GUARD_L"})
			inserts += std::string("\n\tWld_InsertNpc(VLK_600_Chaos, \"") + at + "\");";
		chaos.replace(chaos.find(hook), hook.size(), inserts);
		std::string flood;
		for (int i = 0; i < 40; ++i) flood += "AI_Wait(self, 0.02); ";
		chaos += R"(
var int chaos_starts; var int chaos_loops; var int chaos_ends;
extern func void AI_Teleport(var C_NPC n, var string wp);
extern func int Npc_WasInState(var C_NPC n, var func state);
extern func string Npc_GetNextWP(var C_NPC n);
extern func int Npc_GetDistToPlayer(var C_NPC n);
func void ZS_Chaos() {
	chaos_starts += 1;
	AI_GotoWP(self, "NO_SUCH"); AI_GotoWP(self, ""); AI_GotoFP(self, "NOSUCH"); AI_GotoFP(self, ""); AI_GotoNextFP(self, "");
	AI_PlayAni(self, "T_NO_SUCH_ANI"); AI_PlayAni(self, ""); AI_PlayAniBS(self, "S_SIT", -5);
	AI_PlayAni(self, "T_THIS_NAME_IS_MUCH_TOO_LONG_FOR_ANY_ANIMATION_THERE_EVER_WAS_2_STAND_2_");
	AI_Wait(self, -1); AI_Wait(self, 0);
	AI_AlignToWP(self); AI_AlignToFP(self);
	AI_TurnToNpc(self, other); AI_GotoNpc(self, other); AI_GotoNpc(self, self); AI_TurnToNpc(self, self);
	AI_GotoWP(other, "CAMP_FIRE"); AI_StandUp(other); AI_Wait(other, 1);
	AI_Teleport(self, "NOWHERE");
};
func int ZS_Chaos_Loop() {
	var int i;
	chaos_loops += 1;
	i = Hlp_Random(12);
	if (i == 0) { Npc_ClearAIQueue(self); AI_GotoWP(self, "CAMP_HUT_2"); };
	if (i == 1) {
		if (Hlp_Random(3) == 0) { AI_StartState(self, B_AssessSC, 0, "CAMP_FIRE"); AI_Wait(self, 5); }   // no state at all
		else                    { AI_StartState(self, ZS_Chaos2, 0, "CAMP_SMITH"); };
	};
	if (i == 2) { AI_Teleport(self, "CAMP_HUT_1"); AI_GotoWP(self, "FOREST_SCAVENGER_3"); };
	if (i == 3) { AI_GotoNpc(self, hero); AI_TurnToNpc(self, hero); };
	if (i == 4) { AI_SetWalkmode(self, 77); AI_GotoWP(self, "FOREST_MOLERAT_1"); AI_SetWalkmode(self, -3); };
	if (i == 5) { AI_PlayAniBS(self, "T_STAND_2_SIT", BS_SIT); AI_GotoNextFP(self, "FP_"); AI_PlayAni(self, "T_STAND_2_LGUARD"); };
	if (i == 6) { Npc_SetStateTime(self, -5); Npc_SetPercTime(self, -1); AI_Wait(self, 100000); AI_ContinueRoutine(self); };
	if (i == 7) { )" + flood + R"( };
	if (i == 8) { other = Hlp_GetNpc(GRD_200_Thorus); AI_GotoNpc(self, other); AI_GotoFP(self, "SIT"); AI_AlignToFP(self); };
	if (i == 9) { chaos_ends += Npc_WasInState(self, ZS_Chaos2) + Npc_GetDistToPlayer(self) * 0 + Hlp_StrCmp(Npc_GetNextWP(self), ""); };
	if (i == 10) { AI_StartState(self, ZS_Chaos, 1, "ROAD_1"); AI_StartState(self, ZS_Guard, 1, "CAMP_GATE"); };
	return i == 11;
};
func void ZS_Chaos_End() { chaos_ends += 1; AI_StartState(self, ZS_Chaos, 0, ""); AI_GotoWP(self, self.wp); };
func void ZS_Chaos2() { AI_StartState(self, ZS_Chaos, 1, "CAMP_SQUARE"); };
func int ZS_Chaos2_Loop() { return 1; };
instance VLK_600_Chaos (Npc_Default) {
	name = "Chaos"; guild = GIL_VLK; id = 600; senses_range = 100000;
	Mdl_SetVisual(self, "HUMANS.MDS");
	Mdl_SetVisualBody(self, "hum_body_Naked0", 3, 1, "Hum_Head_Pony", 9, 2, -1);
	daily_routine = Rtn_start_600;
};
func void Rtn_start_600() {
	TA_Min(self, 0, 0, 12, 0, ZS_Chaos, "CAMP_SQUARE");
	TA_Min(self, 12, 0, 18, 0, ZS_Chaos2, "CAMP_FIRE");
	TA_Min(self, 18, 0, 24, 0, ZS_Chaos, "NO_SUCH_PLACE_EITHER");
};
)";
		std::vector<BakeWorld> chaosWorlds;
		const std::string chaosPath = gDir + "/life_chaos.pak";
		bakeLifeWorlds(chaos, chaosPath, chaosWorlds);
		App app;
		CHECK(app.init(chaosPath.c_str(), nullptr));
		app.world().preload(app.camera().pos);
		Npcs& npcs = app.npcs();
		npcs.game().setQuiet(true);
		npcs.config().range = 20000.0f;
		npcs.config().maxShown = 16;
		npcs.config().fights = false; // (the creatures would kill the diggers: a day of life is what is tested here)
		app.setDrawDistance(20000.0f);
		std::mt19937 rng(5);
		u32 walkingMax = 0;
		for (int i = 0; i < 30 * 150; ++i) {
			if (i % 300 == 0) app.environment().timeOfDay = float(rng() % 24) + 0.5f;
			if (i % 700 == 699) app.teleport({float(rng() % 4000) - 2000.0f, 700.0f, float(rng() % 6000) - 1000.0f});
			if (i % 1100 == 1099) npcs.config().range = (rng() % 2) ? 20000.0f : 900.0f;
			Input in;
			in.moveZ = (i / 200) % 2 ? 1.0f : 0.0f;
			in.lookX = 0.3f;
			app.update(in, 1.0f / 30.0f);
			if (i % 11 == 0) app.render();
			walkingMax = std::max(walkingMax, npcs.stats().walking);
			if (i % 450 == 449 && getenv("G3_TRACE")) {
				std::printf("   frame %d: %u actors, starts %d\n", i, npcs.stats().actors, npcs.game().vm().getInt(npcs.game().vm().find("CHAOS_STARTS")));
				for (u32 k = 0; k < npcs.actorCount(); ++k) {
					const NpcActor& a = npcs.actor(k);
					if (!a.used || std::string(npcs.game().nameOf(npcs.game().npc(a.npc))) != "Chaos") continue;
					std::printf("      %s state %u started %d action %d queue %u wait %.1f phase %d stance '%s' loopWait %.2f\n", npcs.doing(a), a.state,
					            int(a.started), int(a.action.kind), unsigned(a.queueCount), double(a.wait), int(a.phase), a.stance, double(a.loopWait));
				}
			}
		}
		const Game& game = npcs.game();
		for (u32 i = 0; i < game.npcCount(); ++i) {
			const GameNpc& n = game.npc(i);
			CHECK(std::isfinite(n.pos[0]) && std::isfinite(n.pos[1]) && std::isfinite(n.pos[2]) && std::isfinite(n.yaw));
			CHECK(std::fabs(n.pos[0]) < 30000.0f && std::fabs(n.pos[2]) < 30000.0f);
		}
		const NpcStats& ns = npcs.stats();
		const auto global = [&](const char* name) { return game.vm().getInt(game.vm().find(name)); };
		std::printf("       chaos: %d starts, %d loops, %d ends; %u state calls, %u actions, %u dropped; up to %u walking; %u ways, %u "
		            "without, %u put at their goal; %u animations missing; scripts %u/%u, %u through no object\n",
		            global("CHAOS_STARTS"), global("CHAOS_LOOPS"), global("CHAOS_ENDS"), ns.stateCalls, ns.actions, ns.queueFull,
		            walkingMax, ns.paths, ns.noPath, ns.snapped, ns.clipsMissing, game.vm().stats().errors, game.vm().stats().aborted,
		            game.vm().stats().nullAccess);
		CHECK(global("CHAOS_STARTS") > 40 && global("CHAOS_LOOPS") > 50 && global("CHAOS_ENDS") > 10);
		CHECK(ns.queueFull > 0 && ns.clipsMissing > 0 && walkingMax >= 2 && ns.paths > 20);
		CHECK(game.vm().stats().aborted == 0 && game.vm().stats().errors == 0);
		// Whatever they did to each other, the others' day went on.
		u32 guards = 0;
		for (u32 i = 0; i < npcs.actorCount(); ++i) guards += npcs.actor(i).used && npcs.actor(i).state == game.vm().find("ZS_GUARD");
		app.environment().timeOfDay = 8.0f;
		npcs.config().range = 20000.0f;
		app.teleport({0.0f, 600.0f, 3300.0f});
		for (int i = 0; i < 30 * 40; ++i) app.update(Input {}, 1.0f / 30.0f);
		// (Thorus has something to tell the player and waits for him to listen: nobody does here, so the talk is dropped.)
		npcs.talkAbort();
		for (int i = 0; i < 30 * 15; ++i) app.update(Input {}, 1.0f / 30.0f);
		const NpcActor* thorus = nullptr;
		for (u32 i = 0; i < npcs.actorCount(); ++i)
			if (npcs.actor(i).used && game.npc(npcs.actor(i).npc).symbol == game.vm().find("GRD_200_THORUS")) thorus = &npcs.actor(i);
		CHECK(thorus && thorus->state == game.vm().find("ZS_GUARD") && std::string(thorus->stance) == "LGUARD");
		(void)guards;
		app.shutdown();
	}
	gfx::shutdown();
}


// Fighting (fight.h, npc_fight.cpp): the damage the rules give, going down,
// the inhabitants striking back, the player's character with a weapon in
// hand, and the console's target lock.
static void testFight() {
	std::vector<BakeWorld> worlds;
	const std::string path = gDir + "/fight.pak";
	bakeLifeWorlds(syntheticScriptSource(), path, worlds);
	gfx::init();
	const auto nearest = [](const Game& g, const char* name, Vec3 from) {
		int best = -1;
		float bestD = 1e30f;
		for (u32 i = 0; i < g.npcCount(); ++i) {
			const GameNpc& n = g.npc(i);
			if (!n.placed || n.world != g.currentWorld() || std::strcmp(g.nameOf(n), name) != 0) continue;
			const float d = std::hypot(n.pos[0] - from.x, n.pos[2] - from.z);
			if (d < bestD) {
				bestD = d;
				best = int(i);
			}
		}
		return best;
	};
	// 1. The rules, blow by blow.
	{
		App app;
		CHECK(app.init(path.c_str(), nullptr));
		app.world().preload(app.camera().pos);
		Npcs& npcs = app.npcs();
		Game& g = npcs.game();
		g.setQuiet(true);
		npcs.config().range = 20000.0f;
		const auto global = [&](const char* name) { return g.vm().getInt(g.vm().find(name)); };
		CHECK(g.hero() != nullptr);
		const u32 hero = u32(g.hero() - &g.npc(0));
		const int scav = nearest(g, "Scavenger", {0, 0, 0}), digger = nearest(g, "Digger", {0, 0, 0});
		const int guard = nearest(g, "Guard", {0, 0, 0});
		CHECK(scav >= 0 && digger >= 0 && guard >= 0);
		if (scav < 0 || digger < 0 || guard < 0) return;
		for (int i = 0; i < 30; ++i) app.update(Input {}, 1.0f / 30.0f);
		// Bare hands against the beast's hide: strength less its protection; a sword adds its damage.
		const i32 str = g.attributeOf(g.npc(hero), g.constant("ATR_STRENGTH", 4));
		g.npcMut(hero).weapon = WEAPON_FIST;
		CHECK(g.meleeDamage(hero, u32(scav)) == std::max(str - 5, i32(0)));
		CHECK(app.menuAction(MENU_GIVE_WEAPON, nullptr) && g.npc(hero).melee != SCRIPT_NONE);
		g.npcMut(hero).weapon = WEAPON_1H;
		CHECK(g.meleeDamage(hero, u32(scav)) == str + 30 - 5);
		CHECK(g.meleeRange(hero, u32(scav)) > 100.0f && g.meleeRange(hero, u32(scav)) < 400.0f);
		// Struck until dead: down for good, in ZS_Dead, never up again.
		const i32 scavHp = g.hitPoints(g.npc(u32(scav)));
		CHECK(scavHp > 0);
		const u32 deathsBefore = u32(global("DEATHS"));
		for (int blow = 0; blow < 10 && !npcs.isDown(u32(scav)); ++blow) npcs.strike(hero, u32(scav), false);
		CHECK(g.npc(u32(scav)).down == DOWN_DEAD && g.hitPoints(g.npc(u32(scav))) == 0);
		for (int i = 0; i < 60; ++i) app.update(Input {}, 1.0f / 30.0f);
		CHECK(u32(global("DEATHS")) == deathsBefore + 1 && npcs.stats().killed >= 1);
		CHECK(npcs.strike(hero, u32(scav), false) < 0); // (the dead take no more blows)
		// A person beaten by somebody not hostile to it is only knocked out:
		// one hit point left, ZS_Unconscious, and up again after a while.
		CHECK(g.attitude(u32(digger), hero) != g.constant("ATT_HOSTILE", 0));
		for (int blow = 0; blow < 40 && !npcs.isDown(u32(digger)); ++blow) npcs.strike(hero, u32(digger), false);
		CHECK(g.npc(u32(digger)).down == DOWN_UNCONSCIOUS && g.hitPoints(g.npc(u32(digger))) == 1);
		for (int i = 0; i < 30; ++i) app.update(Input {}, 1.0f / 30.0f);
		CHECK(global("KNOCKED_OUT") == 1);
		for (int i = 0; i < 30 * 30 && npcs.isDown(u32(digger)); ++i) app.update(Input {}, 1.0f / 30.0f);
		CHECK(!npcs.isDown(u32(digger)) && g.hitPoints(g.npc(u32(digger))) >= 1);
		// Struck, the guard takes note and fights back (its state's reaction to damage).
		const i32 noticed = global("DAMAGE_NOTICED"), begun = global("ATTACKS_BEGUN");
		const int scav2 = nearest(g, "Scavenger", {g.npc(u32(guard)).pos[0], 0, g.npc(u32(guard)).pos[2]});
		CHECK(scav2 >= 0 && scav2 != scav);
		// (Armour: no harm done, but a blow all the same. The state begins
		// once what the guard is doing - a second's wait - is done.)
		CHECK(npcs.strike(u32(scav2 >= 0 ? scav2 : scav), u32(guard), false) == 0);
		for (int i = 0; i < 75; ++i) app.update(Input {}, 1.0f / 30.0f);
		CHECK(global("DAMAGE_NOTICED") == noticed + 1 && global("ATTACKS_BEGUN") >= begun + 1);
		// (The beast is far off: the guard gives up the chase - its loop's
		// rule - and is back at its post.)
		CHECK(g.npc(u32(guard)).target == scav2 && npcs.actorOf(u32(guard)) && !npcs.isDown(u32(guard)));
		app.shutdown();
	}
	// 2. The player's character against a beast, sword in hand, as the
	// player would: close in, strike again and again. The console's
	// controls: the weapon drawn, the beast becomes the locked target.
	{
		App app;
		CHECK(app.init(path.c_str(), nullptr));
		app.settings().scheme = CONTROLS_CONSOLE;
		app.settings().immortal = 1;
		app.applySettings();
		CHECK(app.scheme() == CONTROLS_CONSOLE);
		Npcs& npcs = app.npcs();
		Game& g = npcs.game();
		g.setQuiet(true);
		const u32 hero = u32(g.hero() - &g.npc(0));
		const int scav = nearest(g, "Scavenger", {0, 0, 0});
		CHECK(scav >= 0);
		if (scav < 0) return;
		const GameNpc& e = g.npc(u32(scav));
		app.teleport({e.pos[0], e.pos[1] + 400.0f, e.pos[2] - 600.0f});
		app.world().preload(app.camera().pos);
		CHECK(app.menuAction(MENU_GIVE_WEAPON, nullptr));
		bool locked = false, landed = false;
		int pressed = 0, frames = 0;
		for (; frames < 30 * 40 && e.down == DOWN_NONE; ++frames) {
			Input in;
			const Vec3 feet = app.player().feet;
			const float dx = e.pos[0] - feet.x, dz = e.pos[2] - feet.z;
			const float d = std::hypot(dx, dz);
			if (frames == 5) in.meleeSlot = true;
			if (app.hero().weapon() != WEAPON_NONE) {
				// The stick held towards the beast (the camera's yaw is the stick's frame).
				const float want = std::atan2(dx, dz) - app.camera().yaw;
				const float range = g.meleeRange(hero, u32(scav));
				if (d > range - 30.0f) {
					in.steer = true;
					in.steerYaw = want;
					in.moveX = std::sin(want);
					in.moveZ = std::cos(want);
				}
				in.attack = d < range + 20.0f && frames % 8 == 0;
				pressed += in.attack;
			}
			app.update(in, 1.0f / 30.0f);
			locked |= app.target() == scav && app.targetLocked();
			landed |= app.fightStats().blowsLanded > 0;
		}
		std::printf("  the character against a scavenger: %d frames, %d attacks pressed, %u landed, %u taken; locked on: %s\n", frames,
		            pressed, app.fightStats().blowsLanded, app.fightStats().blowsTaken, locked ? "yes" : "no");
		CHECK(locked && landed && e.down == DOWN_DEAD);
		CHECK(!app.hero().down() && app.heroHitPoints() > 0); // (immortal)
		// Put away: no target held any more after a while.
		Input away;
		away.drawWeapon = true;
		app.update(away, 1.0f / 30.0f);
		for (int i = 0; i < 60; ++i) app.update(Input {}, 1.0f / 30.0f);
		CHECK(app.hero().weapon() == WEAPON_NONE && !app.targetLocked());
		app.shutdown();
	}
	gfx::shutdown();
}

// Magic: the spells as the scripts describe them, the casting loop (a point of mana at a time, the
// scripts deciding), a projectile that hurts, a scroll used up, healing while charged.
static void testRanged() {
	std::vector<BakeWorld> worlds;
	const std::string path = gDir + "/ranged.pak";
	bakeLifeWorlds(syntheticScriptSource(), path, worlds);
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr));
	if (!app.npcs().loaded()) return;
	app.settings().scheme = CONTROLS_CONSOLE;
	app.settings().immortal = 1;
	app.settings().fights = 0;
	app.settings().perception = 0;
	app.applySettings();
	Npcs& npcs = app.npcs();
	Game& g = npcs.game();
	g.setQuiet(true);
	const u32 hero = u32(g.hero() - &g.npc(0));
	const float dt = 1.0f / 30.0f;
	const u32 bow = g.vm().find("ITRW_BOW_SMALL"), arrow = g.vm().find("ITAMARROW");
	CHECK(bow != SCRIPT_NONE && arrow != SCRIPT_NONE);
	const GameItemInfo* bi = g.itemInfo(bow);
	CHECK(bi && bi->munition == i32(arrow) && bi->damageType == 64 && bi->damageTotal == 20);
	int scav = -1;
	for (u32 i = 0; i < g.npcCount(); ++i)
		if (g.npc(i).placed && g.npc(i).world == g.currentWorld() && std::strcmp(g.nameOf(g.npc(i)), "Scavenger") == 0) scav = int(i);
	CHECK(scav >= 0);
	if (scav < 0) return;
	const GameNpc& e = g.npc(u32(scav));
	app.teleport({e.pos[0], e.pos[1] + 400.0f, e.pos[2] - 600.0f});
	app.world().preload(app.camera().pos);
	for (int i = 0; i < 30; ++i) app.update(Input {}, dt);
	// 1. No bow: nothing is taken. Then the bow worn, the key takes it in hand.
	{
		Input in;
		in.rangedSlot = true;
		app.update(in, dt);
		CHECK(app.hero().weapon() == WEAPON_NONE);
	}
	g.giveItems(hero, bow, 1);
	g.giveItems(hero, arrow, 10);
	g.setAttribute(g.npcMut(hero), 5, 100);
	{
		// The inventory: the bow has an action and a quick slot; used, it is worn and in hand; again, away; once more, in hand.
		const ItemEntry* be = nullptr;
		const InventoryView& v = app.inventory();
		for (u32 i = 0; i < v.count; ++i)
			if (v.items[i].symbol == bow) be = &v.items[i];
		CHECK(be && (be->flags & ITEMF_ACTION) && (be->flags & ITEMF_QUICK));
		app.setQuickSlot(1, bow);
		CHECK(app.quickUse(1));
		for (int i = 0; i < 60; ++i) app.update(Input {}, dt);
		CHECK(g.npc(hero).ranged == bow && app.hero().weapon() == WEAPON_BOW);
		CHECK(app.quickUse(1));
		for (int i = 0; i < 60; ++i) app.update(Input {}, dt);
		CHECK(app.hero().weapon() == WEAPON_NONE && g.npc(hero).ranged == bow);
		CHECK(app.quickUse(1));
		for (int i = 0; i < 60; ++i) app.update(Input {}, dt);
		CHECK(app.hero().weapon() == WEAPON_BOW);
	}
	const i32 protPoint = g.npcInt(e, g.vm().findMember("C_NPC", "PROTECTION"), 6);
	const i32 hpBefore = g.hitPoints(e);
	for (int i = 0; i < 90 && !(app.targetLocked() && app.target() == scav); ++i) {
		Input in;
		const Vec3 feet = app.player().feet;
		const float want = std::atan2(e.pos[0] - feet.x, e.pos[2] - feet.z) - app.camera().yaw;
		in.moveX = 0.9f * std::sin(want);
		in.moveZ = 0.9f * std::cos(want);
		app.update(in, dt);
	}
	CHECK(app.target() == scav);
	// 2. A held, then let go: one arrow, which finds the beast.
	int frames = 0;
	for (; frames < 30 * 6 && app.rangedStats().hits + app.rangedStats().misses == 0; ++frames) {
		Input in;
		if (frames == 2) in.attack = true;
		in.attackHeld = frames >= 2 && frames < 2 + 12;
		app.update(in, dt);
		if (frames == 8) CHECK(app.aiming() && app.rangedStats().shot == 0);
	}
	std::printf("  an arrow: %d frames, %u shot, %u hits, %u misses, damage %d (protection %d), arrows left %d\n", frames, app.rangedStats().shot,
	            app.rangedStats().hits, app.rangedStats().misses, int(app.rangedStats().lastDamage), int(protPoint), int(g.itemCount(hero, arrow)));
	CHECK(app.rangedStats().shot == 1 && app.rangedStats().hits == 1 && app.rangedStats().misses == 0);
	CHECK(g.itemCount(hero, arrow) == 9);
	CHECK(app.rangedStats().lastDamage == std::max(20 - protPoint, i32(0)));
	CHECK(g.hitPoints(e) == std::max(hpBefore - std::max(20 - protPoint, i32(0)), i32(0)));
	for (int i = 0; i < 90; ++i) app.update(Input {}, dt);
	// 2b. The key held: it shoots on and on until the arrows are gone.
	{
		const u32 shot = app.rangedStats().shot;
		const i32 left = g.itemCount(hero, arrow);
		for (int i = 0; i < 30 * 4; ++i) {
			Input in;
			in.attack = i == 0;
			in.attackHeld = true;
			app.update(in, dt);
		}
		std::printf("  held for 4 s: %u more shots, arrows left %d -> %d\n", app.rangedStats().shot - shot, int(left), int(g.itemCount(hero, arrow)));
		CHECK(app.rangedStats().shot - shot >= 3);
		for (int i = 0; i < 90; ++i) app.update(Input {}, dt);
		CHECK(!app.aiming());
	}
	// 3. Without arrows: a toast, no shot.
	g.giveItems(hero, arrow, -g.itemCount(hero, arrow));
	{
		const u32 shot = app.rangedStats().shot;
		Input in;
		in.attack = in.attackHeld = true;
		app.update(in, dt);
		app.update(Input {}, dt);
		CHECK(app.rangedStats().noAmmo == 1 && app.rangedStats().shot == shot && !app.aiming());
	}
	// 4. The key puts the bow away.
	{
		Input in;
		in.rangedSlot = true;
		app.update(in, dt);
		for (int i = 0; i < 60; ++i) app.update(Input {}, dt);
		CHECK(app.hero().weapon() == WEAPON_NONE);
	}
	// 5. (fix166) X: the last one used (the bow) in hand; ZL: the next weapon of the quick slots, food passed by.
	{
		const u32 sword = g.vm().find("ITMW_SWORD"), apple = g.vm().find("ITFOAPPLE");
		CHECK(sword != SCRIPT_NONE && apple != SCRIPT_NONE);
		g.giveItems(hero, sword, 1);
		g.giveItems(hero, apple, 3);
		const i32 apples = g.itemCount(hero, apple);
		g.giveItems(hero, arrow, 5);
		g.setAttribute(g.npcMut(hero), 4, 100);
		for (int q = 0; q < QUICK_SLOTS; ++q) app.setQuickSlot(q, SCRIPT_NONE);
		app.setQuickSlot(0, apple);
		app.setQuickSlot(1, bow);
		app.setQuickSlot(3, sword);
		const auto press = [&](bool x, bool zl) {
			Input in;
			in.drawWeapon = x;
			in.cycleWeapon = zl;
			app.update(in, dt);
			for (int i = 0; i < 150; ++i) app.update(Input {}, dt);
		};
		press(true, false);
		CHECK(app.hero().weapon() == WEAPON_BOW);
		press(false, true);
		std::printf("  ZL from the bow: weapon %d, melee is the sword %d, apples %d\n", int(app.hero().weapon()), int(g.npc(hero).melee == sword),
		            int(g.itemCount(hero, apple)));
		CHECK((app.hero().weapon() == WEAPON_1H || app.hero().weapon() == WEAPON_2H) && g.npc(hero).melee == sword);
		CHECK(g.itemCount(hero, apple) == apples);
		press(false, true);
		CHECK(app.hero().weapon() == WEAPON_BOW && g.itemCount(hero, apple) == apples);
		press(true, false);
		CHECK(app.hero().weapon() == WEAPON_NONE);
		press(false, true); // (empty hands: the one after the last, the sword)
		CHECK(app.hero().weapon() == WEAPON_1H || app.hero().weapon() == WEAPON_2H);
		press(true, false);
		press(true, false); // (the last: the sword)
		CHECK(app.hero().weapon() == WEAPON_1H || app.hero().weapon() == WEAPON_2H);
		press(true, false);
		// ZL held: the slot chosen takes it; the apple's slot does nothing (it is not eaten).
		CHECK(app.quickSlotInHand() == 3);
		CHECK(app.quickArm(0)); // (the apple: as a tap on its slot - eaten)
		for (int i = 0; i < 200; ++i) app.update(Input {}, dt);
		CHECK(g.itemCount(hero, apple) == apples - 1);
		CHECK(app.quickArm(1));
		for (int i = 0; i < 150; ++i) app.update(Input {}, dt);
		CHECK(app.hero().weapon() == WEAPON_BOW && app.quickSlotInHand() == 1);
		CHECK(app.quickArm(3));
		for (int i = 0; i < 200; ++i) app.update(Input {}, dt);
		CHECK((app.hero().weapon() == WEAPON_1H || app.hero().weapon() == WEAPON_2H) && app.quickSlotInHand() == 3);
		press(true, false);
	}
}

static void testMagic() {
	std::vector<BakeWorld> worlds;
	const std::string path = gDir + "/magic.pak";
	bakeLifeWorlds(syntheticScriptSource(), path, worlds);
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr));
	if (!app.npcs().loaded()) return;
	app.settings().scheme = CONTROLS_CONSOLE;
	app.settings().immortal = 1;
	app.settings().fights = 0;
	app.settings().perception = 0;
	app.applySettings();
	Npcs& npcs = app.npcs();
	Game& g = npcs.game();
	g.setQuiet(true);
	const u32 hero = u32(g.hero() - &g.npc(0));
	const float dt = 1.0f / 30.0f;
	const auto find = [&](const char* name) {
		const u32 s = g.vm().find(name);
		CHECK(s != SCRIPT_NONE && g.isItem(s));
		return s;
	};
	// 1. What the scripts say a spell is.
	{
		const GameSpell* bolt = g.spellInfo(17);
		CHECK(bolt && std::strcmp(bolt->tag, "Firebolt") == 0 && std::strcmp(bolt->letters, "FBT") == 0);
		CHECK(bolt && bolt->timePerMana == 500.0f && bolt->damagePerLevel == 30 && bolt->damageType == 8 && bolt->shoots);
		CHECK(bolt && bolt->fxFly != 0 && bolt->fxCast != 0 && bolt->fxCollide != 0);
		const GameSpell* heal = g.spellInfo(4);
		CHECK(heal && std::strcmp(heal->letters, "HEA") == 0 && !heal->shoots && heal->spellType == 0);
		const GameSpell* ball = g.spellInfo(1);
		CHECK(ball && ball->timePerMana == 750.0f);
		CHECK(g.spellInfo(40) == nullptr && g.spellInfo(-1) == nullptr);
		const GameItemInfo* rune = g.itemInfo(find("ITARRUNEFIREBOLT"));
		CHECK(rune && rune->spell == 17 && rune->circle == 1);
		// Spell_ProcessMana asked the way the engine does: the spell is the caster's.
		g.npcMut(hero).spell = 17;
		CHECK(g.processMana(hero, -1, 0, false) == 1 && g.processMana(hero, -1, 1, false) == 2 && g.processMana(hero, -1, 1, true) == 3);
		g.npcMut(hero).spell = 1;
		CHECK(g.processMana(hero, -1, 0, false) == 1 && g.processMana(hero, -1, 1, false) == 4 && g.processMana(hero, -1, 5, false) == 2);
		CHECK(g.processMana(hero, -1, 2, true) == 2);
		g.npcMut(hero).spell = -1;
	}
	// 2. A firebolt at a beast: the rune chosen, the stance taken, A held until the spell goes off.
	const int scav = [&]() {
		int best = -1;
		for (u32 i = 0; i < g.npcCount(); ++i)
			if (g.npc(i).placed && g.npc(i).world == g.currentWorld() && std::strcmp(g.nameOf(g.npc(i)), "Scavenger") == 0) best = int(i);
		return best;
	}();
	CHECK(scav >= 0);
	if (scav < 0) return;
	const GameNpc& e = g.npc(u32(scav));
	app.teleport({e.pos[0], e.pos[1] + 400.0f, e.pos[2] - 600.0f});
	app.world().preload(app.camera().pos);
	for (int i = 0; i < 30; ++i) app.update(Input {}, dt);
	const u32 rune = find("ITARRUNEFIREBOLT"), scroll = find("ITARSCROLLFIREBOLT"), healRune = find("ITARRUNEHEAL");
	g.npcMut(hero).skill[g.constant("NPC_TALENT_MAGE", 7)] = 6; // (the circles are learnt: a teacher sets them)
	g.giveItems(hero, rune, 1);
	g.giveItems(hero, scroll, 1);
	g.giveItems(hero, healRune, 1);
	const i32 protFire = g.npcInt(e, g.vm().findMember("C_NPC", "PROTECTION"), 3);
	g.setAttribute(g.npcMut(hero), 3, 20);
	g.setAttribute(g.npcMut(hero), 2, 10);
	{
		// The inventory shows a rune with an action and lets it into a quick slot; the slot chooses the spell.
		const ItemEntry* re = nullptr;
		const InventoryView& v = app.inventory();
		for (u32 i = 0; i < v.count; ++i)
			if (v.items[i].symbol == rune) re = &v.items[i];
		CHECK(re && (re->flags & ITEMF_ACTION) && (re->flags & ITEMF_QUICK));
		app.setQuickSlot(2, rune);
		CHECK(app.quickUse(2) && app.spellItem() == rune);
	}
	for (int i = 0; i < 60; ++i) app.update(Input {}, dt);
	CHECK(app.hero().weapon() == WEAPON_MAG && g.npc(hero).weapon == WEAPON_MAG);
	const i32 hpBefore = g.hitPoints(e);
	// Turned to the beast first (the stick towards it, a step or two), then the key held.
	for (int i = 0; i < 90 && !(app.targetLocked() && app.target() == scav); ++i) {
		Input in;
		const Vec3 feet = app.player().feet;
		const float want = std::atan2(e.pos[0] - feet.x, e.pos[2] - feet.z) - app.camera().yaw;
		in.moveX = 0.9f * std::sin(want);
		in.moveZ = 0.9f * std::cos(want);
		app.update(in, dt);
	}
	CHECK(app.target() == scav);
	bool cast = false;
	int frames = 0;
	for (; frames < 30 * 8 && app.magicStats().hits == 0; ++frames) {
		Input in;
		if (frames == 3) in.attack = true;
		in.attackHeld = frames >= 3 && !cast && frames < 3 + 90;
		app.update(in, dt);
		cast |= app.magicStats().bolts > 0;
	}
	std::printf("  a firebolt: %d frames, %u bolts, %u hits, damage %d (the beast's fire protection %d), mana %d\n", frames, app.magicStats().bolts,
	            app.magicStats().hits, int(app.magicStats().lastDamage), int(protFire), int(g.attributeOf(g.npc(hero), 2)));
	CHECK(app.magicStats().casts == 1 && app.magicStats().bolts == 1 && app.magicStats().hits == 1);
	CHECK(app.magicStats().lastDamage == std::max(30 - protFire, i32(0)));
	CHECK(g.hitPoints(e) == std::max(hpBefore - std::max(30 - protFire, i32(0)), i32(0)));
	CHECK(g.attributeOf(g.npc(hero), 2) == 9); // (a point of mana, SPL_SENDCAST_FIREBOLT)
	CHECK(g.npc(hero).spell == -1 && !app.spellCasting() && g.npc(u32(scav)).lastHitSpell == 17);
	// 3. Firebolt goes off with the press (its mana is put in at once), whether the key is held or not.
	{
		const u32 bolts = app.magicStats().bolts;
		const i32 mana = g.attributeOf(g.npc(hero), 2);
		Input press;
		press.attack = press.attackHeld = true;
		app.update(press, dt);
		Input let;
		for (int i = 0; i < 60; ++i) app.update(let, dt);
		CHECK(!app.spellCasting() && app.magicStats().bolts == bolts + 1 && g.attributeOf(g.npc(hero), 2) == mana - 1);
	}
	// 4. A scroll: used up by the cast, and the stance goes when it was the last of the spells chosen.
	{
		CHECK(app.selectSpell(scroll));
		const u32 casts = app.magicStats().casts;
		for (int i = 0; i < 90 && app.magicStats().casts == casts; ++i) {
			Input in;
			if (i == 2) in.attack = true;
			in.attackHeld = i >= 2;
			app.update(in, dt);
		}
		CHECK(app.magicStats().casts == casts + 1 && g.itemCount(hero, scroll) == 0);
		for (int i = 0; i < 30; ++i) app.update(Input {}, dt);
		CHECK(app.spellItem() != scroll);
	}
	// 5. Healing: charged, it gives its hit points one point of mana at a time, and stops when letting go.
	{
		CHECK(app.selectSpell(healRune));
		g.setAttribute(g.npcMut(hero), 0, 5);
		g.setAttribute(g.npcMut(hero), 2, 10);
		for (int i = 0; i < 90; ++i) {
			Input in;
			if (i == 2) in.attack = true;
			in.attackHeld = i >= 2 && i < 2 + 75; // (2.5 s: five points of mana)
			app.update(in, dt);
		}
		const i32 hp = g.hitPoints(g.npc(hero)), mana = g.attributeOf(g.npc(hero), 2);
		std::printf("  healing: %d hit points of 40, mana %d\n", int(hp), int(mana));
		CHECK(hp >= 5 + 3 * 10 && hp <= 40);
		CHECK(mana <= 7 && mana >= 4 && !app.spellCasting());
	}
	// 6. The inventory uses a rune as choosing it; the stance goes away with the key.
	{
		g.setAttribute(g.npcMut(hero), 2, 10);
		CHECK(app.itemAction(rune) && app.spellItem() == rune);
		Input key;
		key.magic = true;
		for (int i = 0; i < 90; ++i) app.update(i == 0 ? key : Input {}, dt);
		CHECK(app.hero().weapon() == WEAPON_NONE);
		app.update(key, dt);
		for (int i = 0; i < 90; ++i) app.update(Input {}, dt);
		CHECK(app.hero().weapon() == WEAPON_MAG);
	}
	// 7. The faults menu's test kit: every spell as a rune, the potions that raise attributes (as far as the
	// scripts have them), mana to spend.
	{
		g.giveItems(hero, rune, -g.itemCount(hero, rune));
		CHECK(app.menuAction(MENU_GIVE_TESTKIT, nullptr));
		CHECK(g.itemCount(hero, rune) == 1 && g.itemCount(hero, find("ITARRUNEFIREBALL")) == 1 && g.itemCount(hero, find("ITARSCROLLFIREBOLT")) == 3);
		CHECK(g.attributeOf(g.npc(hero), 3) >= 100 && g.attributeOf(g.npc(hero), 2) == g.attributeOf(g.npc(hero), 3));
		CHECK(g.npc(hero).armorItem != SCRIPT_NONE && g.npc(hero).melee != SCRIPT_NONE);
	}
	// 8. The other debug items: a level up, the kits for spells and crafts, every talent learnt.
	{
		const u32 mLevel = g.vm().findMember("C_NPC", "LEVEL");
		CHECK(mLevel != SCRIPT_NONE);
		const i32 level = g.vm().getInt(mLevel, 0, g.npc(hero).object);
		CHECK(app.menuAction(MENU_LEVEL_UP, nullptr));
		CHECK(g.vm().getInt(mLevel, 0, g.npc(hero).object) == level + 1);
		CHECK(app.menuAction(MENU_GIVE_SPELLKIT, nullptr));
		CHECK(g.itemCount(hero, find("ITARRUNEFIREBALL")) >= 1);
		const i32 mage = g.constant("NPC_TALENT_MAGE", 7);
		CHECK(g.talentSkill(hero, mage) == 6);
		CHECK(app.menuAction(MENU_GIVE_CRAFTKIT, nullptr));
		CHECK(g.itemCount(hero, find("ITMINUGGET")) >= 3);
		CHECK(app.menuAction(MENU_LEARN_ALL, nullptr));
		CHECK(g.talentSkill(hero, g.constant("NPC_TALENT_SNEAK", 8)) != 0);
	}
	app.shutdown();
	gfx::shutdown();
}

// The inventory: using things takes the character's time, armour is not
// changed in a fight, requirements hold, quick slots, state.bin, the panel.

// fix151: a transformation, as OpenGothic's Npc::commitSpell / transformBack: the rune's scripts name the creature
// (Npc_SetActiveSpellInfo), the spell goes off, the character is the creature with its whole model (baked from
// "meta/forms") and nothing in his hands; the magic key turns him back with all he had.
static void testTransformHero() {
	std::vector<BakeWorld> worlds;
	const std::string path = gDir + "/transform.pak";
	bakeLifeWorlds(syntheticScriptSource(), path, worlds);
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr));
	if (!app.npcs().loaded() || !app.hero().loaded()) return;
	app.settings().scheme = CONTROLS_CONSOLE;
	app.settings().immortal = 1;
	app.settings().fights = 0;
	app.settings().perception = 0;
	app.applySettings();
	Game& g = app.npcs().game();
	g.setQuiet(true);
	const u32 hero = u32(g.hero() - &g.npc(0));
	const float dt = 1.0f / 30.0f;
	const u32 rune = g.vm().find("ITARRUNETRFMOLERAT"), sword = g.vm().find("ITMW_SWORD"), molerat = g.vm().find("MOLERAT");
	CHECK(rune != SCRIPT_NONE && sword != SCRIPT_NONE && molerat != SCRIPT_NONE);
	if (rune == SCRIPT_NONE || sword == SCRIPT_NONE || molerat == SCRIPT_NONE) return;
	for (int i = 0; i < 30; ++i) app.update(Input {}, dt);
	g.npcMut(hero).skill[g.constant("NPC_TALENT_MAGE", 7)] = 6;
	g.giveItems(hero, rune, 1);
	g.giveItems(hero, sword, 1);
	g.setAttribute(g.npcMut(hero), 2, 10);
	const GameText lookBefore = g.npc(hero).look.script;
	CHECK(app.selectSpell(rune));
	for (int i = 0; i < 60; ++i) app.update(Input {}, dt);
	CHECK(app.hero().weapon() == WEAPON_MAG);
	for (int i = 0; i < 120 && !app.heroTransformed(); ++i) {
		Input in;
		if (i == 2) in.attack = true;
		in.attackHeld = i >= 2;
		app.update(in, dt);
	}
	std::printf("  transformed: %d (%u times, %u without a model), fists only %d, look '%s'\n", int(app.heroTransformed()),
	            app.magicStats().transforms, app.magicStats().noForm, int(app.hero().fistOnly()), g.text(g.npc(hero).look.script));
	CHECK(app.heroTransformed() && app.magicStats().transforms == 1 && g.transformedInto() == molerat);
	CHECK(app.hero().fistOnly() && app.hero().clip("S_FISTRUN") && !app.hero().clip("S_RUN"));
	CHECK(g.itemCount(hero, rune) == 0 && g.itemCount(hero, sword) == 0 && g.npc(hero).look.script != lookBefore);
	CHECK(app.hero().weapon() == WEAPON_NONE && app.hero().animator().current() != nullptr);
	// As the creature: it goes about (the fists' going stands in for the bare one), and its attack key bites.
	for (int i = 0; i < 40; ++i) {
		Input in;
		in.moveZ = 1.0f;
		app.update(in, dt);
	}
	CHECK(app.hero().animator().current() == app.hero().clip("S_FISTRUNL") || app.hero().speed() > 1.0f);
	{
		Input bite;
		bite.attack = bite.attackHeld = true;
		app.update(bite, dt);
		for (int i = 0; i < 20; ++i) app.update(Input {}, dt);
		CHECK(app.hero().weapon() == WEAPON_FIST);
	}
	// The magic key casts nothing now: it turns him back.
	{
		Input back;
		back.magic = true;
		app.update(back, dt);
		for (int i = 0; i < 5; ++i) app.update(Input {}, dt);
	}
	CHECK(!app.heroTransformed() && g.transformedNpc() == -1);
	CHECK(!app.hero().fistOnly() && app.hero().clip("S_RUN") && g.npc(hero).look.script == lookBefore);
	CHECK(g.itemCount(hero, rune) == 1 && g.itemCount(hero, sword) == 1);
	// A save ends a transformation: what is saved is the character himself.
	CHECK(app.heroTransform(molerat) && app.heroTransformed());
	const std::string state = gDir + "/transform.state";
	CHECK(app.saveState(state.c_str()));
	CHECK(!app.heroTransformed() && g.itemCount(hero, sword) == 1);
	CHECK(app.heroTransform(molerat));
	CHECK(app.loadState(state.c_str()) && !app.heroTransformed() && g.itemCount(hero, rune) == 1);
	for (int i = 0; i < 10; ++i) app.update(Input {}, dt);
}

static void testInventory() {
	// Text of the scripts in their code page.
	{
		char out[64];
		scriptText("\xC7\xE5\xEB\xFC\xE5 \xEB\xE5\xF7\xE5\xED\xE8\xFF", out, sizeof(out));
		CHECK(std::strcmp(out, "Зелье лечения") == 0);
		scriptText("K\xE4se und Fl\xFCgel", out, sizeof(out));
		CHECK(std::strcmp(out, "Kaese und Fluegel") == 0);
		scriptText("Зелье", out, sizeof(out));
		CHECK(std::strcmp(out, "Зелье") == 0);
		scriptText("Зелье лечения", out, 8); // (cut short, never in the middle of a letter)
		CHECK(std::strcmp(out, "Зел") == 0);
	}
	std::vector<BakeWorld> worlds;
	const std::string path = gDir + "/inventory.pak";
	bakeLifeWorlds(syntheticScriptSource(), path, worlds);
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr));
	if (!app.npcs().loaded() || !app.hero().loaded()) return;
	app.world().preload(app.camera().pos);
	Npcs& npcs = app.npcs();
	Game& g = npcs.game();
	g.setQuiet(true);
	// Nobody fights here: the rules are the character's.
	app.settings().fights = 0;
	app.settings().perception = 0;
	app.settings().immortal = 1;
	app.applySettings();
	const u32 hero = u32(g.hero() - &g.npc(0));
	const float dt = 1.0f / 30.0f;
	const auto run = [&](float seconds) {
		for (int i = 0; i < int(seconds / dt); ++i) app.update(Input {}, dt);
	};
	run(1.0f);
	const auto find = [&](const char* name) {
		const u32 s = g.vm().find(name);
		CHECK(s != SCRIPT_NONE && g.isItem(s));
		return s;
	};
	const u32 potion = find("ITFO_POTION_HEALTH_01"), apple = find("ITFOAPPLE"), ring = find("ITMI_RING_FIRE");
	const u32 armor = find("GRD_ARMOR_M"), heavy = find("ITMW_2H_SWORD_HEAVY");
	const auto entry = [&](u32 s) -> const ItemEntry* {
		const InventoryView& v = app.inventory();
		for (u32 i = 0; i < v.count; ++i)
			if (v.items[i].symbol == s) return &v.items[i];
		return nullptr;
	};
	// The scene's hero brings three potions and two apples along.
	{
		const InventoryView& v = app.inventory();
		CHECK(v.present && v.count == 2 && v.hpMax == 40 && v.manaMax == 20);
		const ItemEntry* e = entry(potion);
		CHECK(e && e->count == 3 && e->kind == IK_POTION && e->icon == ICON_POTION && std::strcmp(e->action, "Выпить") == 0);
		CHECK(e && std::strcmp(e->name, "Essence of Healing") == 0 && e->rowCount == 2 && std::strcmp(e->rows[0].label, "Heals") == 0 &&
		      std::strcmp(e->rows[0].value, "50") == 0);
		CHECK(entry(apple) && std::strcmp(entry(apple)->action, "Съесть") == 0 && (entry(apple)->flags & ITEMF_QUICK));
	}
	CHECK(app.giveItemKit() >= 12);

	// 1. A potion with a sword in hand: away with it, drink (the animation of
	// POTIONFAST), the health back, the sword in hand again. The character
	// stands meanwhile, whatever the stick says.
	{
		CHECK(app.menuAction(MENU_GIVE_WEAPON, nullptr));
		Input draw;
		draw.meleeSlot = true;
		app.update(draw, dt);
		run(1.5f);
		CHECK(app.hero().weapon() == WEAPON_1H);
		g.setAttribute(g.npcMut(hero), 0, 10);
		const i32 before = g.itemCount(hero, potion);
		CHECK(app.itemAction(potion) && app.itemBusy());
		CHECK(app.itemAction(potion)); // (twice: still one)
		const Vec3 at = app.player().feet;
		int frames = 0, using_ = 0;
		float moved = 0;
		for (; frames < 30 * 8 && app.itemBusy(); ++frames) {
			Input in;
			in.moveZ = 1.0f;  // pushing on: it does not walk off while drinking
			in.attack = false;
			app.update(in, dt);
			if (app.hero().usingItem()) {
				++using_;
				moved = std::max(moved, length(app.player().feet - at));
			}
		}
		std::printf("  a potion with a sword in hand: %d frames busy, %d of them drinking (moved %.1f cm, weapon %d, used %u); health %d\n", frames, using_, double(moved), int(app.hero().weapon()), app.itemStats().used,
		            int(g.hitPoints(g.npc(hero))));
		CHECK(!app.itemBusy() && g.hitPoints(g.npc(hero)) == 40 && g.itemCount(hero, potion) == before - 1);
		CHECK(using_ >= 30 && moved < 5.0f); // (in 0.6 s + 0.3 s + out 0.48 s)
		run(1.0f); // (the sword is being taken in hand again)
		CHECK(app.hero().weapon() == WEAPON_1H && app.itemStats().used == 1);
	}
	// 2. Struck while drinking: spoilt, the potion kept. Then called off before
	// the drinking begins (the weapon key): nothing used.
	{
		g.setAttribute(g.npcMut(hero), 0, 10);
		const i32 before = g.itemCount(hero, potion);
		CHECK(app.itemAction(potion));
		int waited = 0;
		for (; waited < 90 && !app.hero().usingItem(); ++waited) app.update(Input {}, dt);
		std::printf("  struck while drinking: began after %d frames (job %d)\n", waited, int(app.itemBusy()));
		CHECK(app.hero().usingItem());
		int beast = -1;
		for (u32 i = 0; i < g.npcCount() && beast < 0; ++i)
			if (!g.npc(i).hero && g.npc(i).placed && g.npc(i).world == g.currentWorld()) beast = int(i);
		CHECK(beast >= 0);
		npcs.strike(u32(beast), hero, false);
		run(3.0f);
		CHECK(!app.itemBusy() && g.itemCount(hero, potion) == before && g.hitPoints(g.npc(hero)) == 10);
		CHECK(app.itemStats().interrupted == 1);
		CHECK(app.hero().weapon() != WEAPON_NONE && app.itemAction(potion));
		Input key;
		key.drawWeapon = true;
		app.update(key, dt);
		run(3.0f);
		CHECK(!app.itemBusy() && g.itemCount(hero, potion) == before);
	}
	// 3. An apple without its animation: standing two seconds, 8 more health.
	{
		Input away;
		away.drawWeapon = true;
		if (app.hero().weapon() != WEAPON_NONE) app.update(away, dt);
		run(1.5f);
		CHECK(app.hero().weapon() == WEAPON_NONE);
		g.setAttribute(g.npcMut(hero), 0, 20);
		CHECK(app.itemAction(apple));
		int frames = 0;
		for (; frames < 30 * 5 && app.itemBusy(); ++frames) app.update(Input {}, dt);
		CHECK(frames >= 55 && frames <= 75 && g.hitPoints(g.npc(hero)) == 28);
	}
	// 4. Armour: on and off, in a fight as well; it protects.
	// A ring's own function adds its protection, and takes it away again.
	{
		const u32 protEdge = g.vm().findMember("C_NPC", "PROTECTION");
		const auto prot = [&](u32 k) { return g.vm().getInt(protEdge, k, g.npc(hero).object); };
		Input draw;
		draw.fistSlot = true;
		app.update(draw, dt);
		run(1.0f);
		// (In a fight too, as in the original: the armour goes on and off.)
		CHECK(app.heroInCombat() && app.itemAction(armor) && g.npc(hero).armorItem == armor);
		CHECK(app.itemAction(armor) && g.npc(hero).armorItem != armor);
		Input away;
		away.fistSlot = true;
		app.update(away, dt);
		run(6.0f);
		CHECK(!app.heroInCombat());
		const i32 edge = prot(1), fire = prot(3);
		CHECK(app.itemAction(armor) && g.npc(hero).armorItem == armor && prot(1) == edge + 55);
		CHECK(app.itemAction(ring) && app.ring(0) == ring && prot(3) == fire + 3);
		CHECK(entry(ring) && (entry(ring)->flags & ITEMF_EQUIPPED) && std::strcmp(entry(ring)->action, "Снять") == 0);
		CHECK(app.itemAction(ring) && app.ring(0) == SCRIPT_NONE && prot(3) == fire);
		CHECK(app.itemAction(ring)); // (on again, for state.bin)
		// What it takes: strength 60 for the two-hander.
		if (g.itemCount(hero, heavy) <= 0) g.giveItems(hero, heavy, 1);
		CHECK(entry(heavy) && (entry(heavy)->flags & ITEMF_BLOCKED) && !app.itemAction(heavy) && g.npc(hero).melee != heavy);

		// 5. state.bin keeps what is carried and worn and the quick slots; the
		// protection is the same after (not counted twice).
		app.setQuickSlot(2, apple);
		const std::string statePath = gDir + "/inventory-state.bin";
		const i32 potions = g.itemCount(hero, potion), edgeWorn = prot(1), fireWorn = prot(3);
		CHECK(app.saveState(statePath.c_str()));
		g.giveItems(hero, potion, -potions);
		CHECK(app.itemAction(armor) && g.npc(hero).armorItem == SCRIPT_NONE);
		app.setQuickSlot(2, SCRIPT_NONE);
		CHECK(app.loadState(statePath.c_str()));
		CHECK(g.itemCount(hero, potion) == potions && g.npc(hero).armorItem == armor && app.ring(0) == ring);
		CHECK(prot(1) == edgeWorn && prot(3) == fireWorn && app.quickSlot(2) == apple);
	}
	// 6. The panel. On the play page: the bars, ten quick slots, three
	// buttons. The inventory button opens the grid (the character stands);
	// a hold on a thing brings up the ten slots, a tap puts it into one; the
	// slot is used from the play page with a tap. A tap chooses, a second
	// uses. The last kind page asks for the report. The diary and the
	// character's page come up from their buttons.
	{
		InventoryPanel& panel = app.panel();
		PanelInput none, touch;
		touch.touching = true;
		const auto tap = [&](int x, int y) {
			touch.x = x;
			touch.y = y;
			app.panelInput(touch, dt);
			return app.panelInput(none, dt);
		};
		app.panelInput(none, dt);
		CHECK(panel.page() == InventoryPanel::PAGE_PLAY && !panel.open());
		int x, y;
		InventoryPanel::buttonCentre(0, x, y);
		tap(x, y);
		CHECK(panel.page() == InventoryPanel::PAGE_INVENTORY && panel.open());
		app.panelInput(none, dt);
		u32 n = panel.shownCount();
		for (u32 k = 0; k < panel.shownCount(); ++k)
			if (app.inventory().items[panel.shown(k)].symbol == potion) n = k;
		CHECK(n < panel.shownCount());
		panel.tileCentre(n, x, y);
		touch.x = x;
		touch.y = y;
		for (int i = 0; i < 20; ++i) app.panelInput(touch, dt);
		CHECK(panel.assigning() && panel.selected() == potion);
		app.panelInput(none, dt);
		InventoryPanel::slotCentre(1, x, y);
		tap(x, y);
		CHECK(!panel.assigning() && app.quickSlot(1) == potion);
		// The same with the buttons: X brings up the slots, A puts the thing into one - and does not use it as well.
		{
			const i32 had = g.itemCount(hero, potion);
			panel.select(potion);
			PanelInput xBtn, aBtn, right;
			xBtn.assign = true;
			aBtn.confirm = true;
			right.right = true;
			app.panelInput(xBtn, dt);
			CHECK(panel.assigning());
			app.panelInput(right, dt);
			app.panelInput(right, dt);
			app.panelInput(aBtn, dt);
			int inSlots = 0;
			for (int k = 0; k < 10; ++k) inSlots += app.quickSlot(k) == potion;
			CHECK(!panel.assigning() && !app.itemBusy() && g.itemCount(hero, potion) == had && inSlots == 1);
			for (int k = 0; k < 10; ++k) app.setQuickSlot(k, SCRIPT_NONE);
			app.setQuickSlot(1, potion);
		}
		// While the inventory is open the character stands: the stick does not move it.
		{
			const Vec3 at = app.player().feet;
			Input push;
			push.moveZ = 1.0f;
			push.attack = true;
			for (int i = 0; i < 30; ++i) app.update(push, dt);
			CHECK(length(app.player().feet - at) < 1.0f && app.hero().role() != Hero::ATTACK);
		}
		panel.select(apple);
		panel.tileCentre(n, x, y);
		tap(x, y);
		CHECK(panel.selected() == potion && panel.inspecting() > 0.0f && !app.itemBusy());
		tap(x, y);
		CHECK(app.itemBusy());
		run(4.0f);
		// The chosen cell is blue (beside its picture).
		CHECK(panel.draw(app.inventory(), true) && panel.canvas().ready());
		{
			const u16 px = panel.canvas().pixel(x - 28, y - 14);
			const int r = (px >> 11) << 3, b = (px & 31) << 3;
			CHECK(b > r + 20);
		}
		InventoryPanel::pageCentre(InventoryPanel::TAB_PAGES - 1, x, y);
		CHECK(tap(x, y).type == PANEL_REPORT);
		// The back arrow closes it: the character goes again, and slot 2 is a tap away.
		CHECK(tap(10, 8).type == PANEL_CURSOR_OFF && !panel.open());
		{
			const Vec3 at = app.player().feet;
			Input push;
			push.moveZ = 1.0f;
			for (int i = 0; i < 30; ++i) app.update(push, dt);
			CHECK(length(app.player().feet - at) > 50.0f);
		}
		run(1.0f);
		g.setAttribute(g.npcMut(hero), 0, 10);
		InventoryPanel::slotCentre(1, x, y);
		CHECK(tap(x, y).type == PANEL_USE_SLOT && app.itemBusy());
		for (int i = 0; i < 30 * 5 && app.itemBusy(); ++i) app.update(Input {}, dt);
		CHECK(g.hitPoints(g.npc(hero)) == 40);
		// The diary (the scene's start-up writes a topic) and the character.
		InventoryPanel::buttonCentre(1, x, y);
		tap(x, y);
		const InventoryView& v = app.inventory();
		CHECK(panel.page() == InventoryPanel::PAGE_DIARY && v.topicCount == 1 && v.topics[0].count == 1 &&
		      std::strcmp(v.topics[0].name, "The camp") == 0 && v.topics[0].status == 1);
		CHECK(panel.draw(v, true));
		tap(10, 8);
		InventoryPanel::buttonCentre(2, x, y);
		tap(x, y);
		CHECK(panel.page() == InventoryPanel::PAGE_CHARACTER && v.sheet.strength == 10 && std::strcmp(v.sheet.name, "I") == 0);
		CHECK(panel.draw(v, true));
		tap(10, 8);
		CHECK(!panel.open());
	}
	// 7. Things lying about: four from the world file and the smith's sword
	// from the start-up script. Walked up to, A picks one up (the character
	// stands meanwhile); it stays taken across the worlds and in state.bin.
	{
		Loot& loot = app.loot();
		CHECK(loot.count() == 5 && loot.stats().taken == 0);
		u32 scriptItems = 0;
		int appleAt = -1;
		for (u32 k = 0; k < loot.count(); ++k) {
			scriptItems += (loot.item(k).flags & WORLD_ITEM_SCRIPT) != 0;
			if (loot.item(k).symbol == apple) appleAt = int(k);
		}
		CHECK(scriptItems == 1 && appleAt >= 0);
		if (appleAt < 0) return;
		// (Nobody hostile near: the pick-up is not spoilt.)
		run(1.0f);
		const LootItem& li = loot.item(u32(appleAt));
		const std::string statePath = gDir + "/loot-before.bin";
		CHECK(app.saveState(statePath.c_str()));
		app.camera().yaw = 0.0f;
		app.teleport({li.pos.x, li.pos.y + 170.0f, li.pos.z - 70.0f});
		run(1.0f);
		app.render();
		CHECK(app.lootTarget() == appleAt && loot.stats().drawn >= 1);
		const i32 apples = g.itemCount(hero, apple);
		Input a;
		a.confirm = true;
		app.update(a, dt);
		int frames = 0;
		int seen = 0;
		static const char* const kIget[3] = {"C_STAND_2_IGET_1", "C_IGET_1", "C_IGET_2_STAND_1"};
		for (; frames < 90 && app.itemBusy(); ++frames) {
			app.update(Input {}, dt);
			for (int k = 0; k < 3; ++k)
				if (app.hero().animator().current() == app.hero().clip(kIget[k])) seen |= 1 << k;
		}
		// (Gothic 1's own animations of picking up: down, held, up again.)
		std::printf("  picked up in %d frames, through %d of 7 of the original's animations\n", frames, seen);
		CHECK(seen == 7);
		CHECK(li.taken && g.itemCount(hero, apple) == apples + 1 && app.itemStats().pickedUp == 1 && app.lootTarget() != appleAt);
		const std::string after = gDir + "/loot-after.bin";
		CHECK(app.saveState(after.c_str()));
		CHECK(app.enterWorld(hashName("world/cave"), nullptr, 0) && app.loot().count() == 0);
		CHECK(app.loadState(after.c_str()) && app.loot().count() == 5 && app.loot().item(u32(appleAt)).taken);
		CHECK(app.loadState(statePath.c_str()) && !app.loot().item(u32(appleAt)).taken);
	}
	app.shutdown();
	gfx::shutdown();
}

// The thinning of small objects: four layers baked into the batches; the
// menu's density leaves the last ones out.
static void testThinning() {
	SrcScene scene;
	scene.materials.resize(2);
	scene.materials[0].name = "floor";
	scene.materials[0].hasColor = true;
	scene.materials[0].color[0] = 40;
	scene.materials[1].name = "bush";
	scene.materials[1].hasColor = true;
	scene.materials[1].color[1] = 200;
	const float f[4][3] = {{-2000, 0, -2000}, {2000, 0, -2000}, {2000, 0, 2000}, {-2000, 0, 2000}};
	addQuad(scene, f[3], f[2], f[1], f[0], 0);
	for (int layer = 0; layer < 4; ++layer) {
		const float x = -300.0f + 200.0f * float(layer);
		const float c[4][3] = {{x - 50, 0, 1000}, {x + 50, 0, 1000}, {x + 50, 100, 1000}, {x - 50, 100, 1000}};
		const size_t first = scene.triangles.size();
		addQuad(scene, c[0], c[1], c[2], c[3], 1);
		addQuad(scene, c[1], c[0], c[3], c[2], 1);
		for (size_t t = first; t < scene.triangles.size(); ++t) scene.triangles[t].detail = u8(DETAIL_SMALL | (layer << 2));
	}
	scene.spawnPos[1] = 150.0f;
	scene.hasSpawn = true;
	const std::string path = gDir + "/thin.pak";
	bakeScenePak(scene, path, "t", 8000.0f);
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), "t"));
	World& w = app.world();
	app.player().flying = true;
	app.camera().yaw = 0;
	app.teleport({0, 150, 0});
	w.preload(app.camera().pos);
	auto drawn = [&](u8 layers) {
		w.config().smallLayers = layers;
		w.config().fadeMs = 0.0f; // (in and out at once)
		for (int i = 0; i < 3; ++i) {
			app.update(Input {}, 1.0f / 30.0f);
			app.render();
		}
		return w.stats().trianglesDrawn;
	};
	const u32 all = drawn(4), three = drawn(3), two = drawn(2), none = drawn(0);
	std::printf("  thinning: %u triangles with 4 layers, %u with 3, %u with 2, %u with none\n", all, three, two, none);
	CHECK(all == none + 16 && three == none + 12 && two == none + 8);
}

// Small objects by size (fix84): a band is drawn only as far as its things are a few pixels across.
static void testSizeBands() {
	CHECK(sizeBandOf(160.0f) == 0 && sizeBandOf(45.0f) == 1 && sizeBandOf(20.0f) == 2 && sizeBandOf(5.0f) == 3);
	CHECK(batchSizeBand(u8(DETAIL_SMALL | (2 << 2) | (3 << 4) | (1 << 6))) == 3);
	SrcScene scene;
	scene.materials.resize(2);
	scene.materials[0].name = "floor";
	scene.materials[0].hasColor = true;
	scene.materials[1].name = "cup";
	scene.materials[1].hasColor = true;
	scene.materials[1].color[0] = 200;
	const float f[4][3] = {{-3000, 0, -3000}, {3000, 0, -3000}, {3000, 0, 3000}, {-3000, 0, 3000}};
	addQuad(scene, f[3], f[2], f[1], f[0], 0);
	// (band, distance ahead): the bands' reach at 4 pixels is about 31, 16 and 8 m.
	const int things[5][2] = {{0, 2600}, {2, 800}, {2, 2600}, {3, 200}, {3, 1600}};
	for (int i = 0; i < 5; ++i) {
		const float x = -200.0f + 100.0f * float(i), z = float(things[i][1]);
		const float c[4][3] = {{x - 20, 0, z}, {x + 20, 0, z}, {x + 20, 40, z}, {x - 20, 40, z}};
		const size_t first = scene.triangles.size();
		addQuad(scene, c[0], c[1], c[2], c[3], 1);
		addQuad(scene, c[1], c[0], c[3], c[2], 1);
		for (size_t t = first; t < scene.triangles.size(); ++t) scene.triangles[t].detail = u8(DETAIL_SMALL | (things[i][0] << 4));
	}
	scene.spawnPos[1] = 150.0f;
	scene.hasSpawn = true;
	const std::string path = gDir + "/bands.pak";
	bakeScenePak(scene, path, "t", 8000.0f);
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), "t"));
	World& w = app.world();
	app.player().flying = true;
	app.camera().yaw = 0;
	app.teleport({0, 150, 0});
	w.preload(app.camera().pos);
	auto drawn = [&](float pixels) {
		w.config().sizePixels = pixels;
		w.config().focalPx = 208.0f;
		w.config().detailDistance[DETAIL_SMALL] = 6000.0f;
		w.config().fadeMs = 0.0f;
		for (int i = 0; i < 3; ++i) {
			app.update(Input {}, 1.0f / 30.0f);
			app.render();
		}
		return w.stats().trianglesDrawn;
	};
	const u32 all = drawn(0.0f), banded = drawn(4.0f), floorOnly = all - 20;
	const u32 cut = w.stats().sizeCulled;
	std::printf("  size bands: %u triangles without, %u with (%u batches left out for their size)\n", all, banded, cut);
	// Off: all five things (4 triangles each). On: the big one far, the 15-30 cm one near, the tiny one very near.
	CHECK(banded == floorOnly + 12 && cut >= 2); // (a thing can lie in two clusters: two batches)
}

// Lines of sight (fix84): ChunkCollider::segmentBlocked against a wall, the ground and a cut-out, and against a
// brute force over every triangle for random segments.
static void testSight() {
	std::vector<WorldVertex> verts;
	std::vector<u16> idx;
	auto vert = [&](int x, int y, int z) {
		WorldVertex v {};
		v.x = i16(x);
		v.y = i16(y);
		v.z = i16(z);
		v.w = 1;
		verts.push_back(v);
		return u16(verts.size() - 1);
	};
	auto quad = [&](u16 a, u16 b, u16 c, u16 d) {
		for (u16 i : {a, b, c, a, c, d}) idx.push_back(i);
	};
	// Batch 0: the ground (x, z -4000..4000 at y 0) and a wall at z = 0, x -500..500, y 0..400. Batch 1: a fence
	// (cut-out) at z = 1000, the same size.
	quad(vert(-4000, 0, -4000), vert(-4000, 0, 4000), vert(4000, 0, 4000), vert(4000, 0, -4000));
	quad(vert(-500, 0, 0), vert(-500, 400, 0), vert(500, 400, 0), vert(500, 0, 0));
	const u32 solidCount = u32(idx.size());
	quad(vert(-500, 0, 1000), vert(-500, 400, 1000), vert(500, 400, 1000), vert(500, 0, 1000));
	ChunkHeader h {};
	h.magic = CHUNK_MAGIC;
	h.batchCount = 2;
	h.vertexCount = u16(verts.size());
	h.indexCount = u32(idx.size());
	h.posScale = 1.0f;
	ChunkBatch batches[2] {};
	batches[0].indexCount = solidCount;
	batches[1].material = 1;
	batches[1].firstIndex = solidCount;
	batches[1].indexCount = u32(idx.size()) - solidCount;
	WorldMaterial materials[2] {};
	materials[0].frameCount = materials[1].frameCount = 1;
	materials[0].group = 3;
	materials[1].flags = MATF_ALPHA_TEST;
	ChunkCollider col;
	CHECK(col.build(h, batches, verts.data(), idx.data(), materials, MATF_NO_COLLISION, 0));
	CHECK(col.segmentBlocked({0, 100, -300}, {0, 100, 300}));     // through the wall
	CHECK(col.segmentBlocked({0, 100, 300}, {0, 100, -300}));     // ... the other way
	CHECK(!col.segmentBlocked({800, 100, -300}, {800, 100, 300})); // beside it
	CHECK(!col.segmentBlocked({0, 600, -300}, {0, 600, 300}));     // over it
	CHECK(!col.segmentBlocked({0, 100, -300}, {0, 100, -10}));     // short of it
	CHECK(col.segmentBlocked({-3000, 150, -2500}, {2500, 150, 2900})); // across many cells, through it
	CHECK(col.segmentBlocked({0, 100, -300}, {0, -100, -100}));    // into the ground
	CHECK(!col.segmentBlocked({0, 100, 700}, {0, 100, 1300}));     // through the fence: one sees through it
	{
		// rayDown still tells the ground's group (bit 7 is the see-through mark, not part of it).
		float y = 0;
		Vec3 n;
		u8 g = 0;
		CHECK(col.rayDown({1000, 200, 1500}, 500, y, n, nullptr, &g) && g == 3);
	}
	// Random segments against every solid triangle.
	std::mt19937 rng(84);
	std::uniform_real_distribution<float> u(-4500.0f, 4500.0f), uy(-200.0f, 900.0f);
	int agree = 0, blocked = 0;
	for (int i = 0; i < 2000; ++i) {
		const Vec3 a = {u(rng), uy(rng), u(rng)}, b = {u(rng), uy(rng), u(rng)};
		bool hit = false;
		for (u32 t = 0; t + 2 < solidCount && !hit; t += 3) {
			const Vec3 p0 = {float(verts[idx[t]].x), float(verts[idx[t]].y), float(verts[idx[t]].z)};
			const Vec3 p1 = {float(verts[idx[t + 1]].x), float(verts[idx[t + 1]].y), float(verts[idx[t + 1]].z)};
			const Vec3 p2 = {float(verts[idx[t + 2]].x), float(verts[idx[t + 2]].y), float(verts[idx[t + 2]].z)};
			const Vec3 d = b - a, e1 = p1 - p0, e2 = p2 - p0, pv = cross(d, e2);
			const float det = dot(e1, pv);
			if (std::fabs(det) < 1e-6f) continue;
			const Vec3 tv = a - p0;
			const float uu = dot(tv, pv) / det;
			if (uu < 0 || uu > 1) continue;
			const Vec3 qv = cross(tv, e1);
			const float vv = dot(d, qv) / det, tt = dot(e2, qv) / det;
			hit = vv >= 0 && uu + vv <= 1 && tt > 1e-3f && tt < 1.0f - 1e-3f;
		}
		agree += col.segmentBlocked(a, b) == hit;
		blocked += hit;
	}
	std::printf("  sight: %d of 2000 random segments agree with the brute force (%d blocked)\n", agree, blocked);
	CHECK(agree >= 1995 && blocked > 200);
	col.release();
}

// Graphics+ (G1-G3): the baker's ambient occlusion and plant marks, what the chunker writes into the vertices,
// and the shadows under the characters with the way they give way under load.
static void testGfxPlus() {
	// 1. Plants by their names: words of the name, not letters inside other words.
	CHECK(plantKindOf("OW_LOB_TREE_V1.3DS") == PlantKind::Tree && plantKindOf("BIGTREES") == PlantKind::Tree);
	CHECK(plantKindOf("nw_nature_bush_01") == PlantKind::Bush && plantKindOf("GRASS_A0.TGA") == PlantKind::Grass);
	CHECK(plantKindOf("OW_STREET_LAMP.3DS") == PlantKind::None && plantKindOf("OW_TREESTUMP_01.3DS") == PlantKind::None);
	CHECK(plantKindOf("OW_FIREPLACE") == PlantKind::None && plantKindOf("XX_EICHENHAIN") == PlantKind::None);
	CHECK(plantKindOf("XX_EICHENHAIN", {"EICHENHAIN"}) == PlantKind::Tree);
	CHECK(plantSway(PlantKind::Tree, 1000.0f) > plantSway(PlantKind::Grass, 50.0f) && plantSway(PlantKind::None, 500.0f) == 0.0f);
	{
		// The foot stands still, the top sways the most; a tree bends near its top.
		SrcTriangle t;
		const float ys[3] = {0.0f, 500.0f, 1000.0f};
		for (int k = 0; k < 3; ++k) t.v[k].pos[0] = t.v[k].pos[2] = 0.0f, t.v[k].pos[1] = ys[k];
		markSway(&t, 1, PlantKind::Tree, 0.0f, 1000.0f, 12.0f);
		CHECK(t.v[0].wind == 0 && t.v[2].wind == 48 && t.v[1].wind == 12);
	}

	// 2. Occlusion: a floor of 6 x 6 m in metre squares with a wall 3 m high along one side.
	SrcScene scene;
	scene.materials.resize(2);
	scene.materials[0].name = "STONE";
	scene.materials[1].name = "GLASS";
	auto vert = [](float x, float y, float z) {
		SrcVertex v {};
		v.pos[0] = x;
		v.pos[1] = y;
		v.pos[2] = z;
		v.rgba[0] = v.rgba[1] = v.rgba[2] = 200;
		v.rgba[3] = 255;
		return v;
	};
	auto tri = [&](SrcVertex a, SrcVertex b, SrcVertex c, u32 material) {
		SrcTriangle t;
		t.v[0] = a;
		t.v[1] = b;
		t.v[2] = c;
		t.material = material;
		scene.triangles.push_back(t);
	};
	for (int i = 0; i < 6; ++i)
		for (int j = 0; j < 6; ++j) {
			const float x0 = float(i) * 100.0f, x1 = x0 + 100.0f, z0 = float(j) * 100.0f, z1 = z0 + 100.0f;
			// (front faces up: counter-clockwise from above, see bake.cpp's winding check)
			tri(vert(x0, 0, z0), vert(x1, 0, z0), vert(x0, 0, z1), 0);
			tri(vert(x1, 0, z0), vert(x1, 0, z1), vert(x0, 0, z1), 0);
			// The wall at x = 0, facing the floor, in squares too.
			if (i < 3) {
				const float y0 = float(i) * 100.0f, y1 = y0 + 100.0f;
				tri(vert(0, y0, z0), vert(0, y0, z1), vert(0, y1, z0), 0);
				tri(vert(0, y0, z1), vert(0, y1, z1), vert(0, y1, z0), 0);
			}
		}
	// A pane of glass in the open: it neither blocks nor is shaded.
	tri(vert(500, 50, 300), vert(500, 50, 400), vert(500, 150, 300), 1);
	std::vector<AoMaterial> mats = {AoMaterial {1.0f, true}, AoMaterial {0.0f, false}};
	AoOptions ao;
	ao.level = AO_LOW;
	ao.threads = 2;
	AoStats st;
	bakeOcclusion(scene, mats, ao, st);
	// The occlusion of a floor (normal up) or wall (normal +x) corner at a place.
	auto occlusionAt = [&](float x, float y, float z, bool wall) {
		int best = -1;
		for (const SrcTriangle& t : scene.triangles) {
			if (t.material != 0) continue;
			const bool isWall = t.v[0].pos[0] == 0.0f && t.v[1].pos[0] == 0.0f && t.v[2].pos[0] == 0.0f;
			if (isWall != wall) continue;
			for (const SrcVertex& v : t.v)
				if (v.pos[0] == x && v.pos[1] == y && v.pos[2] == z) best = std::max(best, int(v.occlusion));
		}
		return best;
	};
	const int foot = occlusionAt(0, 0, 300, false), open = occlusionAt(600, 0, 300, false), mid = occlusionAt(200, 0, 300, false);
	const int wallFoot = occlusionAt(0, 0, 300, true), wallTop = occlusionAt(0, 300, 300, true);
	std::printf("  occlusion: floor at the wall %d, 2 m off %d, 6 m off %d; wall foot %d, top %d (of 255); %llu corners from %llu places, "
	            "%.0f%% on average\n", foot, mid, open, wallFoot, wallTop, static_cast<unsigned long long>(st.corners),
	            static_cast<unsigned long long>(st.unique), double(st.mean) * 100.0);
	CHECK(foot > 60 && mid >= 0 && mid < foot && open >= 0 && open < 12 && wallFoot > wallTop && wallTop >= 0 && wallTop < 40);
	CHECK(scene.triangles.back().v[0].occlusion == 0 && st.occluders == scene.triangles.size() - 1);
	// One place and one smooth surface: one answer (so the chunker still merges the corners).
	CHECK(st.unique < st.corners);

	// A floor of two huge triangles beside a wall: the cutting gives the shade somewhere to fall off, and leaves the open middle alone.
	{
		SrcScene big;
		big.materials.resize(1);
		auto bt = [&](SrcVertex a, SrcVertex b, SrcVertex c) {
			SrcTriangle t;
			t.v[0] = a; t.v[1] = b; t.v[2] = c;
			t.material = 0;
			big.triangles.push_back(t);
		};
		bt(vert(0, 0, 0), vert(1600, 0, 0), vert(0, 0, 1600));
		bt(vert(1600, 0, 0), vert(1600, 0, 1600), vert(0, 0, 1600));
		bt(vert(0, 0, 0), vert(0, 0, 1600), vert(0, 400, 0));
		bt(vert(0, 0, 1600), vert(0, 400, 1600), vert(0, 400, 0));
		AoOptions a2;
		a2.level = AO_LOW;
		a2.maxExtra = 300000;
		a2.threads = 2;
		AoStats s2;
		const size_t before = big.triangles.size();
		bakeOcclusion(big, mats, a2, s2);
		int farMax = 0, nearMax = 0;
		for (const SrcTriangle& t : big.triangles) {
			if (t.v[0].pos[1] != 0 || t.v[1].pos[1] != 0 || t.v[2].pos[1] != 0 || t.v[0].pos[0] == 0 && t.v[1].pos[0] == 0) continue;
			for (const SrcVertex& v : t.v) (v.pos[0] > 900 ? farMax : nearMax) = std::max(v.pos[0] > 900 ? farMax : nearMax, int(v.occlusion));
		}
		std::printf("  occlusion cutting: %zu -> %zu triangles (%u added); near the wall at most %d, far side %d\n", before, big.triangles.size(),
		            s2.added, nearMax, farMax);
		CHECK(s2.added > 0 && big.triangles.size() == before + s2.added);
		CHECK(s2.added < 4000 && nearMax > 60 && farMax < 12);
		a2.maxExtra = 0;
		SrcScene whole = big;
		whole.triangles.resize(before);
		AoStats s3;
		bakeOcclusion(whole, mats, a2, s3);
		CHECK(s3.added == 0 && whole.triangles.size() == before);
	}

	// 3. The chunker writes occlusion and sway into w with vertexFx, and 1 without.
	scene.triangles[0].v[0].wind = 9;
	{
		ChunkerOptions co;
		co.vertexFx = true;
		ChunkerStats cs;
		const std::vector<BakedChunk> chunks = buildChunks(scene, {0, 0}, {0, 0}, co, cs);
		u32 shaded = 0, swaying = 0, plain = 0;
		for (const BakedChunk& c : chunks)
			for (const WorldVertex& v : c.vertices) {
				shaded += vertexOcclusion(v.w) > 0;
				swaying += vertexWind(v.w) == 9;
				plain += v.w == 1;
			}
		CHECK(shaded > 10 && swaying >= 1);
		co.vertexFx = false;
		ChunkerStats cs2;
		const std::vector<BakedChunk> old = buildChunks(scene, {0, 0}, {0, 0}, co, cs2);
		bool allOne = !old.empty();
		for (const BakedChunk& c : old)
			for (const WorldVertex& v : c.vertices) allOne = allOne && v.w == 1;
		CHECK(allOne);
		(void)plain;
	}
	CHECK(packVertexFx(200, 77) == i16(200 | (77 << 8)) && vertexOcclusion(packVertexFx(200, 77)) == 200 &&
	      vertexWind(packVertexFx(200, 200)) == 127);

	// 4. Shadows: on the ground under the characters, only darkening; under load only the hero keeps one.
	std::vector<BakeWorld> worlds;
	const std::string path = gDir + "/gfxplus.pak";
	bakeLifeWorlds(syntheticScriptSource(), path, worlds);
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr));
	if (!app.npcs().loaded() || !app.hero().loaded()) return;
	app.world().preload(app.camera().pos);
	app.npcs().game().setQuiet(true);
	app.settings().fights = 0;
	app.settings().perception = 0;
	app.settings().immortal = 1;
	app.applySettings();
	for (int i = 0; i < 45; ++i) app.update(Input {}, 1.0f / 30.0f);
	CHECK(app.heroVisible());
	app.settings().shadows = SHADOWS_OFF;
	app.applySettings();
	app.render();
	const std::vector<u8> without(gfx::softFramebuffer(), gfx::softFramebuffer() + gfx::SCREEN_W * gfx::SCREEN_H * 4);
	CHECK(app.shadows().stats().drawn == 0);
	app.settings().shadows = SHADOWS_ALL;
	app.applySettings();
	app.render();
	const u32 all = app.shadows().stats().drawn, asked = app.shadows().stats().asked;
	const u8* with = gfx::softFramebuffer();
	u32 darker = 0, lighter = 0;
	for (u32 i = 0; i < u32(gfx::SCREEN_W * gfx::SCREEN_H); ++i)
		for (int c = 0; c < 3; ++c) {
			const int d = int(with[i * 4 + u32(c)]) - int(without[i * 4 + u32(c)]);
			darker += d < -2;
			lighter += d > 1;
		}
	app.setGfxRelief(true);
	app.render();
	const u32 relieved = app.shadows().stats().drawn;
	app.setGfxRelief(false);
	std::printf("  shadows: %u characters, %u laid (%u under load); %u channel values darker, %u lighter\n", asked, all, relieved, darker,
	            lighter);
	CHECK(all >= 1 && asked >= all && darker > 30 && lighter == 0 && relieved == 1);
	// The hero's own shape thrown by the sun: drawn at the steady 30 fps too (the wind's relief), and evenly dark - each
	// pixel once, however many parts of the body lie over it there.
	{
		app.skipChapters();
		app.environment().timeOfDay = 16.0f; // (a low sun behind the camera: the shadow reaches out ahead of the hero)
		app.weather().configure(WEATHER_CLEAR, true);
		app.weather().settle();
		// The camera turned to look along +Z: the sun stands on the -Z side all day, the shadow falls away from the eye.
		for (int i = 0; i < 400; ++i) {
			float y = app.camera().yaw;
			while (y > 3.14159265f) y -= 6.2831853f;
			while (y < -3.14159265f) y += 6.2831853f;
			if (std::fabs(y) < 0.1f) break;
			Input turn;
			turn.lookX = y > 0.0f ? -0.5f : 0.5f;
			app.update(turn, 1.0f / 30.0f);
		}
		app.settings().shadows = SHADOWS_OFF;
		app.applySettings();
		app.render();
		const std::vector<u8> plain(gfx::softFramebuffer(), gfx::softFramebuffer() + gfx::SCREEN_W * gfx::SCREEN_H * 4);
		app.settings().shadows = SHADOWS_HERO_CAST;
		app.applySettings();
		app.setWindRelief(true);
		app.render();
		app.setWindRelief(false);
		const u32 cast = app.shadows().stats().cast;
		const u32 roundWithCast = app.shadows().stats().drawn - cast; // (the people's and beasts' round ones, with his shape)
		const u8* shaded = gfx::softFramebuffer();
		u32 dark = 0;
		float lo = 1.0f, hi = 0.0f;
		for (u32 i = 0; i < u32(gfx::SCREEN_W * gfx::SCREEN_H); ++i) {
			const int before = plain[i * 4] + plain[i * 4 + 1] + plain[i * 4 + 2], after = shaded[i * 4] + shaded[i * 4 + 1] + shaded[i * 4 + 2];
			if (before < 90 || after > before - 9) continue; // (a dark ground says little, and the unshaded are left out)
			++dark;
			const float k = float(after) / float(before);
			lo = std::min(lo, k);
			hi = std::max(hi, k);
		}
		std::printf("  the hero's cast shadow: %u drawn, %u pixels darkened to between %.2f and %.2f of their light\n", cast, dark,
		            double(lo), double(hi));
		std::printf("  with it, %u round shadows under the others\n", roundWithCast);
		CHECK(cast == 1 && dark > 20 && hi - lo < 0.10f);
	}
	// Light shafts: in the morning, looking towards the sun past the trees, beams stand in gaps and fade in over a few frames.
	{
		app.settings().shadows = SHADOWS_OFF;
		app.settings().godRays = 1;
		app.applySettings();
		app.environment().timeOfDay = 7.6f;
		for (int i = 0; i < 400; ++i) {
			float y = app.camera().yaw - 3.14159265f;
			while (y > 3.14159265f) y -= 6.2831853f;
			while (y < -3.14159265f) y += 6.2831853f;
			if (std::fabs(y) < 0.1f) break;
			Input turn;
			turn.lookX = y > 0.0f ? -0.5f : 0.5f;
			app.update(turn, 1.0f / 30.0f);
		}
		u32 most = 0;
		for (int i = 0; i < 60; ++i) {
			app.update(Input {}, 1.0f / 30.0f);
			app.render();
			most = std::max(most, app.godRays().drawn());
		}
		std::printf("  light shafts: up to %u beams\n", most);
		CHECK(most > 0 && most <= GodRays::MAX);
	}
	// The menu: the page is under Graphics, its set puts the values, a change by hand makes it "own".
	{
		Menu m;
		Settings s;
		m.open();
		MenuInput down, ok, right, back;
		down.down = true;
		ok.confirm = true;
		right.right = true;
		back.back = true;
		m.update(down, s);
		m.update(ok, s);
		CHECK(m.page() == Menu::PAGE_GRAPHICS);
		for (int i = 0; i < m.itemCount(Menu::PAGE_GRAPHICS) && std::strcmp(m.label(Menu::PAGE_GRAPHICS, m.selected()), "Графика+") != 0; ++i)
			m.update(down, s);
		CHECK(m.update(ok, s) == MENU_NONE && m.page() == Menu::PAGE_GFX_PLUS);
		CHECK(m.update(right, s) == MENU_CHANGED && s.gfxPlus == GFX_PLUS_BALANCED && s.occlusion == 60 && s.wind == 2 &&
		      s.shadows == SHADOWS_HERO);
		CHECK(m.update(right, s) == MENU_CHANGED && s.gfxPlus == GFX_PLUS_MAX && s.shadows == SHADOWS_ALL);
		CHECK(m.update(right, s) == MENU_CHANGED && s.gfxPlus == GFX_PLUS_OFF && s.occlusion == 0); // ("own" is skipped)
		m.update(down, s);
		CHECK(m.update(right, s) == MENU_CHANGED && s.occlusion == 10 && s.gfxPlus == GFX_PLUS_OWN);
		s.sanitize();
		CHECK(s.gfxPlus == GFX_PLUS_OWN && s.gfxPresetOf() == GFX_PLUS_OWN);
		CHECK(m.update(back, s) == MENU_NONE && m.page() == Menu::PAGE_GRAPHICS);
	}
	app.shutdown();
	CHECK(gfx::memStats().meshCount == 0);
}

// The far layer (G10): what the baker makes of a world (within its budget, every cell
// naming its chunks), and what the runtime draws with it - beyond the draw distance and
// in place of chunks not drawn, without the sky showing through where the two meet.
// ---- chunks cut by detail class (WORLDF_TIERS) and the streaming around them ----

// A flat square of `size` cm facing up at (x, y, z), of a detail class.
static void addPatch(SrcScene& scene, float x, float y, float z, float size, u32 material, u8 detail) {
	const float p[4][3] = {{x, y, z}, {x + size, y, z}, {x + size, y, z + size}, {x, y, z + size}};
	const size_t first = scene.triangles.size();
	addQuad(scene, p[3], p[2], p[1], p[0], material);
	for (size_t t = first; t < scene.triangles.size(); ++t) scene.triangles[t].detail = detail;
}

// 10 x 10 cells of 40 m: a floor, 400 medium and 400 small objects in each, and a sea under it all.
static SrcScene tieredScene() {
	SrcScene scene;
	scene.materials.resize(3);
	const char* names[3] = {"floor", "stuff", "sea"};
	for (int m = 0; m < 3; ++m) {
		scene.materials[size_t(m)].name = names[m];
		scene.materials[size_t(m)].hasColor = true;
		scene.materials[size_t(m)].color[0] = u8(60 + m * 80);
	}
	u32 seed = 12345;
	auto rnd = [&]() {
		seed = seed * 1664525u + 1013904223u;
		return float((seed >> 8) & 0xFFFF) / 65536.0f;
	};
	for (int cz = 0; cz < 10; ++cz)
		for (int cx = 0; cx < 10; ++cx) {
			const float x0 = float(cx) * 4000.0f, z0 = float(cz) * 4000.0f;
			addPatch(scene, x0 + 20.0f, 0.0f, z0 + 20.0f, 3960.0f, 0, DETAIL_ALWAYS);
			for (int n = 0; n < 200; ++n) {
				addPatch(scene, x0 + 100.0f + rnd() * 3700.0f, 5.0f + rnd(), z0 + 100.0f + rnd() * 3700.0f, 40.0f, 1, DETAIL_MEDIUM);
				addPatch(scene, x0 + 100.0f + rnd() * 3700.0f, 5.0f + rnd(), z0 + 100.0f + rnd() * 3700.0f, 30.0f, 1, DETAIL_SMALL);
			}
		}
	addPatch(scene, -60000.0f, -50.0f, -60000.0f, 160000.0f, 2, DETAIL_ALWAYS); // (reaches into every cell: a chunk for the "big" list)
	scene.spawnPos[0] = scene.spawnPos[2] = 20000.0f;
	scene.spawnPos[1] = 150.0f;
	scene.hasSpawn = true;
	return scene;
}

static float boxDistance(const ChunkInfo& ci, Vec3 c) {
	const float dx = std::max({ci.bbMin[0] - c.x, c.x - ci.bbMax[0], 0.0f});
	const float dy = std::max({ci.bbMin[1] - c.y, c.y - ci.bbMax[1], 0.0f});
	const float dz = std::max({ci.bbMin[2] - c.z, c.z - ci.bbMax[2], 0.0f});
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

static void testChunkTiers() {
	const SrcScene scene = tieredScene();
	const std::string split = gDir + "/tiers.pak", plain = gDir + "/tiers_plain.pak";
	ChunkerOptions co;
	co.chunkSize = 4000.0f;
	co.splitDetail = true;
	bakeScenePak(scene, split, "t", co);
	co.splitDetail = false;
	bakeScenePak(scene, plain, "t", co);
	gfx::init();

	// 1. The baked tables: three kinds of chunk per cell, each holding what it says.
	Pak pakSplit, pakPlain;
	CHECK(pakSplit.open(split.c_str()) && pakPlain.open(plain.c_str()));
	World a, b;
	CHECK(a.open(&pakSplit, "t") && b.open(&pakPlain, "t"));
	CHECK(a.hasTiers() && !b.hasTiers());
	u32 tierCount[3] = {0, 0, 0};
	for (u32 i = 0; i < a.header().chunkCount; ++i) ++tierCount[a.chunkTier(i)];
	CHECK(tierCount[0] >= 100 && tierCount[1] == 100 && tierCount[2] == 100);
	CHECK(b.header().chunkCount == tierCount[0]); // (the plain archive: every cell, and the sea, one chunk)
	for (u32 i = 0; i < b.header().chunkCount; ++i) CHECK(b.chunkTier(i) == 0);
	// The medium and small objects are a good half of what a cell holds.
	u64 bytes[3] = {0, 0, 0};
	for (u32 i = 0; i < a.header().chunkCount; ++i) {
		const ChunkInfo& ci = a.chunkInfo(i);
		bytes[a.chunkTier(i)] += u64(ci.vertexCount) * sizeof(WorldVertex) + u64(ci.indexCount) * 2;
	}
	CHECK(bytes[0] * 3 < bytes[0] + bytes[1] + bytes[2]);

	// 2. What is wanted around the camera, found through the grid, is what a look at every chunk finds -
	// at several places, the sea (in the list of big chunks) included.
	{
		World w;
		w.config().maxLoadsPerFrame = 0; // nothing is loaded: only what is wanted is counted
		w.config().drawDistance = 6000.0f;
		w.config().loadRadius = 8000.0f;
		w.config().unloadRadius = 12000.0f;
		w.config().detailDistance[DETAIL_MEDIUM] = 3000.0f;
		w.config().detailDistance[DETAIL_SMALL] = 1500.0f;
		CHECK(w.open(&pakSplit, "t"));
		const Vec3 places[] = {{20000, 100, 20000}, {2000, 100, 2000}, {39000, 100, 100}, {-3000, 100, 21000}, {20000, 100, 20000}, {200000, 0, 0}};
		// (The menu changes how far the objects are drawn: what is wanted follows, without a restart.)
		for (float medium : {3000.0f, 6000.0f, 1500.0f}) {
			w.config().detailDistance[DETAIL_MEDIUM] = medium;
			u32 wantedMost = 0;
			for (const Vec3& at : places) {
				w.update(at);
				u32 expect = 0;
				for (u32 i = 0; i < w.header().chunkCount; ++i) {
					const u32 t = w.chunkTier(i);
					const float reach = t == 0 ? 8000.0f : std::min(w.config().detailDistance[t] * w.config().detailHysteresis + w.config().detailLead, 8000.0f);
					expect += boxDistance(w.chunkInfo(i), at) < reach;
				}
				CHECK(w.stats().chunksPending == expect);
				wantedMost = std::max(wantedMost, expect);
			}
			CHECK(wantedMost > 10);
			// The camera far from all of it: only the sea (in the list of big chunks) is wanted.
			CHECK(w.stats().chunksPending <= 1);
		}
	}

	// 3. Standing in the middle: the world around, the objects only near.
	{
		World w;
		StreamConfig& cfg = w.config();
		cfg.drawDistance = 6000.0f;
		cfg.loadRadius = 8000.0f;
		cfg.unloadRadius = 12000.0f;
		cfg.detailDistance[DETAIL_MEDIUM] = 3000.0f;
		cfg.detailDistance[DETAIL_SMALL] = 1500.0f;
		CHECK(w.open(&pakSplit, "t"));
		const Vec3 at = {20000, 100, 20000};
		w.preload(at);
		CHECK(w.stats().chunksPending == 0 && w.stats().loadFailures == 0);
		u32 resident[3] = {0, 0, 0};
		for (u32 i = 0; i < w.header().chunkCount; ++i) {
			if (!w.chunkResident(i)) continue;
			const u32 t = w.chunkTier(i);
			++resident[t];
			CHECK(boxDistance(w.chunkInfo(i), at) < (t == 0 ? 8000.0f : t == 1 ? 3000.0f * 1.1f + 1500.0f : 1500.0f * 1.1f + 1500.0f));
		}
		CHECK(resident[0] > resident[1] && resident[1] > resident[2] && resident[2] >= 1);
		CHECK(w.stats().loadedDetail == resident[1] + resident[2]);
		// The same place in the archive that holds everything in one chunk costs far more.
		World p;
		p.config() = cfg;
		CHECK(p.open(&pakPlain, "t"));
		p.preload(at);
		CHECK(p.stats().residentBytes > w.stats().residentBytes * 3 / 2);
		std::printf("  tiers: %u+%u+%u chunks, %.2f MB resident against %.2f MB with whole chunks\n", resident[0], resident[1], resident[2],
		            double(w.stats().residentBytes) / 1048576.0, double(p.stats().residentBytes) / 1048576.0);
		// Walking away frees the objects first.
		Vec3 far = at;
		far.x += 3500.0f;
		for (int i = 0; i < 40; ++i) w.update(far);
		w.preload(far);
		CHECK(w.stats().loadFailures == 0);
		for (u32 i = 0; i < w.header().chunkCount; ++i)
			if (w.chunkResident(i)) CHECK(boxDistance(w.chunkInfo(i), far) <= (w.chunkTier(i) == 0 ? 12000.0f : 3000.0f * 1.1f + 1500.0f + 1200.0f));
		// Out of everything's reach: all freed.
		for (int i = 0; i < 4; ++i) w.update({1.0e6f, 0, 1.0e6f});
		CHECK(w.stats().chunksLoaded <= 1 && w.stats().loadedDetail == 0 && w.stats().detailBytes == 0);
	}

	// 4. A budget that is too small: the nearest world comes first, the objects of far chunks are not kept,
	// what is turned away is counted once and nothing breaks.
	{
		World w;
		StreamConfig& cfg = w.config();
		cfg.drawDistance = 6000.0f;
		cfg.loadRadius = 8000.0f;
		cfg.unloadRadius = 12000.0f;
		cfg.detailDistance[DETAIL_MEDIUM] = 3000.0f;
		cfg.detailDistance[DETAIL_SMALL] = 1500.0f;
		cfg.background = true;
		cfg.worker = false;
		cfg.maxLoadsPerFrame = 2;
		CHECK(w.open(&pakSplit, "t"));
		u64 coreBytes = 0;
		for (u32 i = 0; i < w.header().chunkCount; ++i)
			if (w.chunkTier(i) == 0 && boxDistance(w.chunkInfo(i), {20000, 100, 20000}) < 8000.0f)
				coreBytes += u64(w.chunkInfo(i).vertexCount) * sizeof(WorldVertex) + u64(w.chunkInfo(i).indexCount) * 2;
		// Room for most of the world around, with textures, and not for the objects.
		cfg.memoryBudget = u32(coreBytes * 6 / 5);
		Vec3 at = {20000, 100, 20000};
		for (int f = 0; f < 3000; ++f) {
			w.update(at);
			if (w.stats().chunksPending == 0 && w.stats().chunksInFlight == 0) break;
		}
		CHECK(w.stats().loadFailures == 0);
		CHECK(w.stats().residentBytes <= cfg.memoryBudget + 1);
		CHECK(w.stats().budgetRejects > 0);
		// Nothing resident is farther by priority than something missing (the swap rule), beyond its margin.
		auto key = [&](u32 i) { return boxDistance(w.chunkInfo(i), at) + cfg.tierBias[w.chunkTier(i)]; };
		float worstResident = -1e30f, bestMissing = 1e30f;
		for (u32 i = 0; i < w.header().chunkCount; ++i) {
			const float d = boxDistance(w.chunkInfo(i), at);
			const u32 t = w.chunkTier(i);
			const float reach = t == 0 ? 8000.0f : std::min(cfg.detailDistance[t] * cfg.detailHysteresis + cfg.detailLead, 8000.0f);
			if (w.chunkResident(i))
				worstResident = std::max(worstResident, key(i));
			else if (d < reach)
				bestMissing = std::min(bestMissing, key(i));
		}
		CHECK(worstResident <= bestMissing + 300.0f + 1.0f);
		// Waiting is not a refusal per frame.
		const u32 rejects = w.stats().budgetRejects;
		for (int f = 0; f < 120; ++f) w.update(at);
		CHECK(w.stats().budgetRejects == rejects);
		// Walking on keeps it going, the heading counts, and the budget holds.
		for (int f = 0; f < 600; ++f) {
			at.x += 20.0f;
			w.update(at);
			CHECK(w.stats().residentBytes <= cfg.memoryBudget + 1);
		}
		CHECK(w.stats().loadFailures == 0 && w.stats().evictions > 0);
	}
	pakSplit.close();
	pakPlain.close();
}

// ---- baked visibility (--vis) ----

static void testVisibility() {
	SrcScene scene;
	scene.materials.resize(4);
	const char* names[4] = {"ground", "wall", "house", "crate"};
	const u8 reds[4] = {40, 200, 230, 250};
	for (int m = 0; m < 4; ++m) {
		scene.materials[size_t(m)].name = names[m];
		scene.materials[size_t(m)].hasColor = true;
		scene.materials[size_t(m)].color[0] = reds[m];
		scene.materials[size_t(m)].color[1] = u8(30 + m * 50);
		scene.materials[size_t(m)].color[2] = u8(250 - m * 60);
	}
	// 3 x 3 cells of 40 m, a floor in each; a wall 30 m high across the whole width at z = 60 m; behind it houses and crates.
	for (int cz = 0; cz < 3; ++cz)
		for (int cx = 0; cx < 3; ++cx) {
			const float x0 = float(cx) * 4000.0f, z0 = float(cz) * 4000.0f;
			const float f[4][3] = {{x0, 0, z0}, {x0 + 4000, 0, z0}, {x0 + 4000, 0, z0 + 4000}, {x0, 0, z0 + 4000}};
			addQuad(scene, f[3], f[2], f[1], f[0], 0);
		}
	addBlock(scene, 0.0f, 12000.0f, 6000.0f, 6200.0f, 3000.0f); // (material 0 in the helper; recoloured below)
	for (size_t t = 0; t < scene.triangles.size(); ++t)
		if (scene.triangles[t].v[0].pos[1] > 1.0f || scene.triangles[t].v[1].pos[1] > 1.0f) scene.triangles[t].material = 1;
	for (int i = 0; i < 12; ++i) {
		const float x = 800.0f + float(i % 4) * 2800.0f, z = 8600.0f + float(i / 4) * 1100.0f;
		const size_t first = scene.triangles.size();
		addBlock(scene, x, x + 600.0f, z, z + 600.0f, 800.0f);
		for (size_t t = first; t < scene.triangles.size(); ++t) scene.triangles[t].material = 2;
	}
	for (int i = 0; i < 40; ++i) {
		const float x = 300.0f + float(i % 8) * 1400.0f, z = 8200.0f + float(i / 8) * 700.0f;
		const float p[4][3] = {{x, 60, z}, {x + 50, 60, z}, {x + 50, 60, z + 50}, {x, 60, z + 50}};
		const size_t first = scene.triangles.size();
		addQuad(scene, p[3], p[2], p[1], p[0], 3);
		for (size_t t = first; t < scene.triangles.size(); ++t) scene.triangles[t].detail = DETAIL_MEDIUM;
	}
	scene.spawnPos[0] = 6000.0f;
	scene.spawnPos[1] = 170.0f;
	scene.spawnPos[2] = 1500.0f;
	scene.hasSpawn = true;

	const std::string withVis = gDir + "/vis.pak", without = gDir + "/novis.pak";
	{
		std::vector<BakeWorld> worlds(1);
		worlds[0].name = "v";
		worlds[0].scene = scene;
		BakeOptions opt;
		opt.chunker.chunkSize = 4000.0f;
		opt.tex.maxSize = 32;
		opt.tex.quality = 0;
		opt.threads = 2;
		opt.quiet = true;
		opt.vis.enabled = true;
		opt.vis.threads = 2;
		PakWriter pak;
		bakeWorlds(worlds, opt, pak);
		pak.write(withVis);
		BakeOptions plain = opt;
		plain.vis.enabled = false;
		PakWriter pak2;
		bakeWorlds(worlds, plain, pak2);
		pak2.write(without);
	}
	gfx::init();
	{
		Pak p1, p2;
		CHECK(p1.open(withVis.c_str()) && p2.open(without.c_str()));
		World a, b;
		CHECK(a.open(&p1, "v") && b.open(&p2, "v"));
		CHECK(a.hasVisibility() && !b.hasVisibility());
	}

	App app;
	CHECK(app.init(withVis.c_str(), "v"));
	World& w = app.world();
	CHECK(w.hasVisibility());
	app.setDrawDistance(12000.0f);
	app.player().flying = true;
	const float dt = 1.0f / 30.0f;
	auto frame = [&](bool vis, std::vector<u8>& px, u32& tris, u32& hidden) {
		w.config().visibility = vis;
		app.setClock(100.0); // (the texture animation must not tell the two runs apart)
		app.update(Input {}, dt);
		app.render();
		px.assign(gfx::softFramebuffer(), gfx::softFramebuffer() + gfx::SCREEN_W * gfx::SCREEN_H * 4);
		tris = w.stats().trianglesDrawn;
		hidden = w.stats().clustersHidden + w.stats().chunksHidden;
	};
	w.config().detailDistance[DETAIL_MEDIUM] = 20000.0f;
	app.camera().pitch = -0.05f;
	u32 culledSomewhere = 0, fewer = 0;
	// Poses in front of the wall, looking every which way, and behind it.
	const float poses[][4] = {{2000, 1000, 0.0f, 170}, {6000, 1500, 0.3f, 170}, {10000, 800, -0.4f, 170}, {6000, 3000, 0.0f, 450},
	                          {1000, 3500, 0.7f, 170}, {6000, 2500, 3.1f, 170}, {6000, 9500, 3.14f, 170}, {3000, 7000, 0.0f, 170},
	                          {9000, 1200, 0.1f, 600}, {6000, 500, -0.2f, 170}};
	for (const auto& pose : poses) {
		app.camera().yaw = pose[2];
		app.teleport({pose[0], pose[3], pose[1]});
		w.preload(app.camera().pos);
		std::vector<u8> off, on;
		u32 trisOff = 0, trisOn = 0, hiddenOff = 0, hiddenOn = 0;
		for (int i = 0; i < 12; ++i) frame(false, off, trisOff, hiddenOff);
		for (int i = 0; i < 12; ++i) frame(true, on, trisOn, hiddenOn);
		CHECK(hiddenOff == 0);
		CHECK(off == on); // not a pixel of the picture depends on the table
		CHECK(trisOn <= trisOff);
		culledSomewhere += hiddenOn > 0;
		fewer += trisOn < trisOff;
		CHECK(w.stats().visRow); // (the poses are all in cells with a row)
	}
	CHECK(culledSomewhere >= 3 && fewer >= 3);
	std::printf("  visibility: hidden clusters in %u of %zu poses, fewer triangles in %u\n", culledSomewhere, sizeof(poses) / sizeof(poses[0]), fewer);
	// The poses facing the houses from this side of the wall: the houses are behind it, hidden.
	{
		app.camera().yaw = 0.0f;
		app.teleport({6000.0f, 170.0f, 1500.0f});
		std::vector<u8> on;
		u32 t = 0, h = 0;
		frame(true, on, t, h);
		frame(true, on, t, h);
		CHECK(h > 0);
		CHECK(w.stats().visBytes > 0);
		// Off: the table is let go, and nothing is hidden.
		frame(false, on, t, h);
		frame(false, on, t, h);
		CHECK(h == 0 && w.stats().visBytes == 0 && !w.visibilityActive());
	}
	// Outside every cell with a row: all is drawn.
	{
		app.teleport({-90000.0f, 170.0f, -90000.0f});
		std::vector<u8> on;
		u32 t = 0, h = 0;
		frame(true, on, t, h);
		frame(true, on, t, h);
		CHECK(h == 0 && !w.stats().visRow);
	}
}

// A cell with two floors has a row for each, chosen by the camera's height: what the upper deck hides from the street is
// not what it hides from the deck itself (fix88).
static void testVisibilityFloors() {
	SrcScene scene;
	scene.materials.resize(2);
	scene.materials[0].name = "deck";
	scene.materials[1].name = "crate";
	for (SrcMaterial& m : scene.materials) {
		m.hasColor = true;
		m.color[0] = 120;
	}
	// Two decks, 20 m apart, over 3 x 1 cells; crates on the upper one in the last cell.
	for (const float y : {0.0f, 2000.0f})
		for (int cx = 0; cx < 3; ++cx) {
			const float x0 = float(cx) * 4000.0f;
			const float f[4][3] = {{x0, y, 0}, {x0 + 4000, y, 0}, {x0 + 4000, y, 4000}, {x0, y, 4000}};
			addQuad(scene, f[0], f[1], f[2], f[3], 0); // (facing up: a floor to stand on)
		}
	for (int i = 0; i < 6; ++i) {
		const float x = 9000.0f + float(i % 3) * 600.0f, z = 1000.0f + float(i / 3) * 900.0f;
		const size_t first = scene.triangles.size();
		addBlock(scene, x, x + 200.0f, z, z + 200.0f, 150.0f);
		for (size_t t = first; t < scene.triangles.size(); ++t) {
			scene.triangles[t].material = 1;
			for (SrcVertex& v : scene.triangles[t].v) v.pos[1] += 2000.0f;
		}
	}
	scene.spawnPos[0] = 1000.0f;
	scene.spawnPos[1] = 170.0f;
	scene.spawnPos[2] = 1000.0f;
	scene.hasSpawn = true;
	const std::string path = gDir + "/visfloors.pak";
	{
		std::vector<BakeWorld> worlds(1);
		worlds[0].name = "f";
		worlds[0].scene = scene;
		BakeOptions opt;
		opt.chunker.chunkSize = 4000.0f;
		opt.tex.maxSize = 32;
		opt.tex.quality = 0;
		opt.threads = 2;
		opt.quiet = true;
		opt.vis.enabled = true;
		opt.vis.threads = 2;
		opt.vis.nearAlways = 100.0f;
		PakWriter pak;
		bakeWorlds(worlds, opt, pak);
		pak.write(path);
	}
	gfx::init();
	Pak pak;
	CHECK(pak.open(path.c_str()));
	World w;
	CHECK(w.open(&pak, "f"));
	CHECK(w.hasVisibility());
	w.config().visibility = true;
	for (int i = 0; i < 4; ++i) w.update({1000.0f, 170.0f, 1000.0f});
	CHECK(w.visibilityActive());
	// The chunk of the last cell.
	u32 chunk = 0xFFFFFFFFu;
	for (u32 i = 0; i < w.header().chunkCount; ++i)
		if (w.chunkInfo(i).bbMax[0] > 11000.0f && w.chunkInfo(i).bbMin[0] > 7000.0f) chunk = i;
	CHECK(chunk != 0xFFFFFFFFu);
	const int street = w.visibilityFloor({1000.0f, 170.0f, 1000.0f});
	u32 total = 0;
	const u32 hiddenFromStreet = w.hiddenClusters(chunk, total);
	const int deck = w.visibilityFloor({1000.0f, 2170.0f, 1000.0f});
	u32 total2 = 0;
	const u32 hiddenFromDeck = w.hiddenClusters(chunk, total2);
	std::printf("  visibility floors: street row %d hides %08x, deck row %d hides %08x of %u clusters\n", street, hiddenFromStreet, deck,
	            hiddenFromDeck, total);
	CHECK(street >= 0 && deck >= 0 && street != deck && total == total2 && total >= 3);
	CHECK(hiddenFromStreet != hiddenFromDeck); // (one table row for both floors could not tell them apart)
	CHECK(hiddenFromStreet != 0 && hiddenFromDeck != 0);
	// Back at the street, the same row; a camera far under the lowest floor has none (everything is drawn).
	CHECK(w.visibilityFloor({1000.0f, 300.0f, 1000.0f}) == street);
	CHECK(w.visibilityFloor({1000.0f, -9000.0f, 1000.0f}) == -1);
	// At the edge between the floors the row in use stays while it still holds the camera.
	const int a = w.visibilityFloor({1000.0f, 1900.0f, 1000.0f});
	CHECK(w.visibilityFloor({1000.0f, 1800.0f, 1000.0f}) == a && w.visibilityFloor({1000.0f, 1900.0f, 1000.0f}) == a);
}


// A house's inside is not "near" to the street (fix91): from outside its crate is hidden by the walls, in the room it is
// seen; a table of the older kind, or a camera in a room with no row, draws everything.
static void testVisibilityRooms() {
	SrcScene scene;
	scene.materials.resize(2);
	scene.materials[0].name = "ground";
	scene.materials[1].name = "house";
	for (SrcMaterial& m : scene.materials) {
		m.hasColor = true;
		m.color[0] = 120;
	}
	// (The ground in small tiles: one big ground triangle would share the crate's cluster and keep it "near".)
	for (int tx = 0; tx < 24; ++tx)
		for (int tz = 0; tz < 8; ++tz) {
			const float x0 = float(tx) * 500.0f, z0 = float(tz) * 500.0f;
			const float f[4][3] = {{x0, 0, z0}, {x0 + 500, 0, z0}, {x0 + 500, 0, z0 + 500}, {x0, 0, z0 + 500}};
			addQuad(scene, f[0], f[1], f[2], f[3], 0);
		}
	size_t first = scene.triangles.size();
	addBlock(scene, 9000.0f, 12000.0f, 500.0f, 3500.0f, 400.0f); // the house: a closed box (walls and roof)
	for (size_t t = first; t < scene.triangles.size(); ++t) scene.triangles[t].material = 1;
	first = scene.triangles.size();
	addBlock(scene, 10500.0f, 10700.0f, 1500.0f, 1700.0f, 150.0f); // the crate inside (a small object: a chunk of its own)
	for (size_t t = first; t < scene.triangles.size(); ++t) {
		scene.triangles[t].material = 1;
		scene.triangles[t].detail = DETAIL_SMALL;
	}
	scene.roomNames.push_back("HOUSE");
	SrcScene::RoomBoxSrc rb {};
	rb.bbMin[0] = 9050.0f, rb.bbMin[1] = 0.0f, rb.bbMin[2] = 550.0f;
	rb.bbMax[0] = 11950.0f, rb.bbMax[1] = 380.0f, rb.bbMax[2] = 3450.0f;
	rb.room = 0;
	scene.roomBoxes.push_back(rb);
	scene.spawnPos[0] = 1000.0f;
	scene.spawnPos[1] = 170.0f;
	scene.spawnPos[2] = 1000.0f;
	scene.hasSpawn = true;
	const std::string path = gDir + "/visrooms.pak";
	{
		std::vector<BakeWorld> worlds(1);
		worlds[0].name = "r";
		worlds[0].scene = scene;
		BakeOptions opt;
		opt.chunker.chunkSize = 4000.0f;
		opt.tex.maxSize = 32;
		opt.tex.quality = 0;
		opt.threads = 2;
		opt.quiet = true;
		opt.vis.enabled = true;
		opt.vis.threads = 2;
		opt.vis.nearAlways = 100.0f;
		PakWriter pak;
		bakeWorlds(worlds, opt, pak);
		pak.write(path);
	}
	gfx::init();
	Pak pak;
	CHECK(pak.open(path.c_str()));
	World w;
	CHECK(w.open(&pak, "r"));
	CHECK(w.hasVisibility());
	w.config().visibility = true;
	for (int i = 0; i < 4; ++i) w.update({8400.0f, 170.0f, 400.0f});
	CHECK(w.visibilityActive());
	u32 chunk = 0xFFFFFFFFu;
	for (u32 i = 0; i < w.header().chunkCount; ++i)
		if (w.chunkInfo(i).bbMin[0] < 10500.0f && w.chunkInfo(i).bbMax[0] > 10700.0f && w.chunkInfo(i).bbMax[1] > 100.0f) chunk = i; // (the cell with the house)
	CHECK(chunk != 0xFFFFFFFFu);
	w.setCameraRoom(0);
	const int outside = w.visibilityFloor({8400.0f, 170.0f, 400.0f});
	u32 total = 0;
	const u32 hiddenOutside = w.hiddenClusters(chunk, total);
	w.setCameraRoom(1);
	const int inside = w.visibilityFloor({10600.0f, 170.0f, 1000.0f});
	u32 total2 = 0;
	const u32 hiddenInside = w.hiddenClusters(chunk, total2);
	std::printf("  visibility rooms: outside row %d hides %08x, the room's row %d hides %08x of %u clusters\n", outside, hiddenOutside, inside,
	            hiddenInside, total);
	CHECK(outside >= 0 && inside >= 0 && total == total2 && total >= 2);
	CHECK((hiddenOutside & ~hiddenInside) != 0); // (something the street does not see - the crate - the room does)
	CHECK((hiddenInside & ~hiddenOutside) == 0);
	// A camera in a room the cell has no row for: everything is drawn.
	w.setCameraRoom(7);
	CHECK(w.visibilityFloor({10600.0f, 170.0f, 1000.0f}) == -1);
	w.setCameraRoom(0);
	CHECK(w.visibilityFloor({8400.0f, 170.0f, 400.0f}) == outside);
}

// ---- simplified stand-ins for far chunks (--lod) ----

static SrcScene hillScene(bool jitter = false) {
	SrcScene scene;
	scene.materials.resize(1);
	scene.materials[0].name = "ground";
	scene.materials[0].hasColor = true;
	scene.materials[0].color[0] = 90;
	scene.materials[0].color[1] = 140;
	scene.materials[0].color[2] = 60;
	// 80 x 80 quads of one metre: a gentle swell with a ridge, flat enough that most of it may be simplified.
	auto height = [](int x, int z) {
		const float fx = float(x), fz = float(z);
		float y = 150.0f * std::sin(fx * 0.012f) * std::cos(fz * 0.009f);
		if (x >= 3000 / 100 + 5 && x < 3000 / 100 + 9) y += 120.0f; // a ridge
		return y;
	};
	for (int z = 0; z < 80; ++z)
		for (int x = 0; x < 80; ++x) {
			float q[4][3] = {{float(x) * 100, height(x, z), float(z) * 100}, {float(x + 1) * 100, height(x + 1, z), float(z) * 100},
			                 {float(x + 1) * 100, height(x + 1, z + 1), float(z + 1) * 100}, {float(x) * 100, height(x, z + 1), float(z + 1) * 100}};
			addQuad(scene, q[3], q[2], q[1], q[0], 0);
			// Every quad its own light, a little apart: no two share a vertex, as polygons of a level do not.
			if (jitter)
				for (size_t t = scene.triangles.size() - 2; t < scene.triangles.size(); ++t)
					for (int k = 0; k < 3; ++k) scene.triangles[t].v[k].rgba[0] = u8(190 + (x * 7 + z * 13) % 21);
		}
	scene.spawnPos[0] = 4000.0f;
	scene.spawnPos[1] = 600.0f;
	scene.spawnPos[2] = 4000.0f;
	scene.hasSpawn = true;
	return scene;
}

static void testLodWith(bool jitter) {
	const SrcScene scene = hillScene(jitter);
	ChunkerOptions co;
	co.chunkSize = 4000.0f;
	ChunkerStats cs;
	std::vector<BakedChunk> chunks = buildChunks(scene, {0}, {0}, co, cs);
	CHECK(chunks.size() == 4);
	std::vector<std::vector<u16>> originalIndices;
	std::vector<u32> originalCollision;
	WorldMaterial mat {};
	const std::vector<WorldMaterial> materials = {mat};
	for (const BakedChunk& c : chunks) {
		ChunkCollider cc;
		CHECK(cc.build(c.header, c.batches.data(), c.vertices.data(), c.indices.data(), materials.data(), MATF_NO_COLLISION, 0));
		originalCollision.push_back(cc.triangleCount);
		originalIndices.push_back(c.indices);
		cc.release();
	}
	LodOptions lo;
	lo.enabled = true;
	lo.threads = 2;
	LodStats ls;
	buildLods(chunks, materials, lo, ls);
	CHECK(ls.chunks == 4);
	CHECK(ls.trianglesAfter * 10 < ls.trianglesBefore * 7);
	if (jitter) CHECK(cs.outputTriangles == 12800); // a good third gone from a gentle hill
	std::printf("  lod%s: %llu -> %llu triangles of %u chunks, +%.0f KB\n", jitter ? " (every polygon its own vertices)" : "", static_cast<unsigned long long>(ls.trianglesBefore),
	            static_cast<unsigned long long>(ls.trianglesAfter), ls.chunks, double(ls.extraBytes) / 1024.0);
	for (size_t ci = 0; ci < chunks.size(); ++ci) {
		const BakedChunk& c = chunks[ci];
		CHECK(c.header.batchCount == c.batches.size() && c.header.indexCount == c.indices.size());
		CHECK(std::equal(originalIndices[ci].begin(), originalIndices[ci].end(), c.indices.begin())); // (the full picture is untouched)
		// The border of what the stand-in draws is the border of what it replaces: no crack towards a neighbour.
		// (By place: polygons that meet at a place need not share a vertex.)
		std::vector<u64> place(c.vertices.size());
		for (size_t v = 0; v < place.size(); ++v)
			place[v] = (u64(u16(c.vertices[v].x)) << 32) | (u64(u16(c.vertices[v].y)) << 16) | u64(u16(c.vertices[v].z));
		std::map<std::pair<u64, u64>, int> full, far;
		auto edges = [&](std::map<std::pair<u64, u64>, int>& m, u8 lodWanted) {
			for (const ChunkBatch& b : c.batches) {
				if (batchLod(b.detail) == BATCH_LOD_BOTH || batchLod(b.detail) == lodWanted)
					for (u32 i = 0; i + 2 < b.indexCount; i += 3)
						for (int k = 0; k < 3; ++k) {
							const u32 a = c.indices[b.firstIndex + i + u32(k)], d = c.indices[b.firstIndex + i + u32((k + 1) % 3)];
							CHECK(a < c.vertices.size() && d < c.vertices.size());
							++m[{std::min(place[a], place[d]), std::max(place[a], place[d])}];
						}
			}
		};
		edges(full, BATCH_LOD_FULL_ONLY);
		edges(far, BATCH_LOD_FAR_ONLY);
		std::set<std::pair<u64, u64>> borderFull, borderFar;
		for (const auto& kv : full) if (kv.second == 1) borderFull.insert(kv.first);
		for (const auto& kv : far) if (kv.second == 1) borderFar.insert(kv.first);
		CHECK(!borderFull.empty() && borderFull == borderFar);
		// The collision mesh is the full one: the stand-in is not part of it.
		ChunkCollider cc;
		CHECK(cc.build(c.header, c.batches.data(), c.vertices.data(), c.indices.data(), materials.data(), MATF_NO_COLLISION, 0));
		CHECK(cc.triangleCount == originalCollision[ci]);
		cc.release();
		u32 farBatches = 0, fullOnly = 0;
		for (const ChunkBatch& b : c.batches) {
			farBatches += batchLod(b.detail) == BATCH_LOD_FAR_ONLY;
			fullOnly += batchLod(b.detail) == BATCH_LOD_FULL_ONLY;
		}
		CHECK(farBatches >= 1 && fullOnly >= 1);
	}

	// In the engine: a chunk farther than lodDistance draws fewer triangles; not so one that is near.
	const std::string withLod = gDir + "/lod.pak";
	{
		std::vector<BakeWorld> worlds(1);
		worlds[0].name = "h";
		worlds[0].scene = scene;
		BakeOptions opt;
		opt.chunker.chunkSize = 4000.0f;
		opt.tex.maxSize = 32;
		opt.tex.quality = 0;
		opt.threads = 2;
		opt.quiet = true;
		opt.lod.enabled = true;
		PakWriter pak;
		bakeWorlds(worlds, opt, pak);
		pak.write(withLod);
	}
	gfx::init();
	App app;
	CHECK(app.init(withLod.c_str(), "h"));
	World& w = app.world();
	CHECK((w.header().flags & WORLDF_LOD) != 0);
	w.config().fadeMs = 0.0f; // (one set at once; the dissolve between them is checked below)
	app.setDrawDistance(20000.0f);
	app.player().flying = true;
	app.camera().yaw = 0.7f;
	app.camera().pitch = -0.4f;
	auto trianglesAt = [&](float lod) {
		w.config().lodDistance = lod;
		app.teleport({1000.0f, 1500.0f, 1000.0f});
		w.preload(app.camera().pos);
		for (int i = 0; i < 6; ++i) {
			app.update(Input {}, 1.0f / 30.0f);
			app.render();
		}
		return w.stats().trianglesDrawn;
	};
	const u32 full = trianglesAt(0.0f), far = trianglesAt(1.0f), near = trianglesAt(1.0e6f);
	std::printf("  lod in the engine: %u triangles drawn, %u with stand-ins everywhere, %u with the distance far away\n", full, far, near);
	CHECK(full > 0 && far > 0 && far * 10 < full * 8);
	CHECK(near == full);
	// Dissolving: from full to stand-ins, both are drawn for a while (more than either alone), then only the stand-ins; and back.
	w.config().fadeMs = 400.0f;
	trianglesAt(1.0e6f);
	for (int i = 0; i < 20; ++i) {
		app.update(Input {}, 1.0f / 30.0f);
		app.render();
	}
	const u32 settledFull = w.stats().trianglesDrawn;
	w.config().lodDistance = 1.0f;
	u32 most = 0;
	for (int i = 0; i < 4; ++i) {
		app.update(Input {}, 1.0f / 30.0f);
		app.render();
		most = std::max(most, w.stats().trianglesDrawn);
	}
	CHECK(most > settledFull); // (both sets at once)
	for (int i = 0; i < 40; ++i) {
		app.update(Input {}, 1.0f / 30.0f);
		app.render();
	}
	CHECK(w.stats().trianglesDrawn == far);
	w.config().lodDistance = 1.0e6f;
	most = 0;
	for (int i = 0; i < 4; ++i) {
		app.update(Input {}, 1.0f / 30.0f);
		app.render();
		most = std::max(most, w.stats().trianglesDrawn);
	}
	CHECK(most > settledFull);
	for (int i = 0; i < 40; ++i) {
		app.update(Input {}, 1.0f / 30.0f);
		app.render();
	}
	CHECK(w.stats().trianglesDrawn == full);
}


// Closed balls of one detail class: crates and mushrooms, which are most of the triangles a scene has.
static SrcScene ballScene(u8 detail, int balls) {
	SrcScene scene;
	scene.materials.resize(2);
	scene.materials[0].name = "floor";
	scene.materials[1].name = "stuff";
	for (SrcMaterial& m : scene.materials) {
		m.hasColor = true;
		m.color[0] = 90;
	}
	addPatch(scene, 20.0f, 0.0f, 20.0f, 3960.0f, 0, DETAIL_ALWAYS);
	const int rings = 10, around = 16;
	for (int b = 0; b < balls; ++b) {
		const float cx = 300.0f + float(b % 10) * 300.0f, cz = 300.0f + float(b / 10) * 300.0f, r = 40.0f;
		auto point = [&](int ring, int a) {
			SrcVertex v {};
			const float pitch = 3.14159265f * float(ring) / float(rings), yaw = 6.2831853f * float(a % around) / float(around);
			v.pos[0] = cx + r * std::sin(pitch) * std::cos(yaw);
			v.pos[1] = r + r * std::cos(pitch);
			v.pos[2] = cz + r * std::sin(pitch) * std::sin(yaw);
			v.uv[0] = float(a) / float(around);
			v.uv[1] = float(ring) / float(rings);
			v.rgba[0] = v.rgba[1] = v.rgba[2] = 200;
			v.rgba[3] = 255;
			return v;
		};
		for (int ring = 0; ring < rings; ++ring)
			for (int a = 0; a < around; ++a) {
				SrcTriangle t1, t2;
				t1.material = t2.material = 1;
				t1.detail = t2.detail = detail;
				t1.v[0] = point(ring, a);
				t1.v[1] = point(ring + 1, a);
				t1.v[2] = point(ring + 1, a + 1);
				t2.v[0] = point(ring, a);
				t2.v[1] = point(ring + 1, a + 1);
				t2.v[2] = point(ring, a + 1);
				// (the poles: one triangle of each pair is a sliver)
				if (ring > 0) scene.triangles.push_back(t1);
				if (ring < rings - 1) scene.triangles.push_back(t2);
			}
	}
	scene.spawnPos[0] = scene.spawnPos[2] = 2000.0f;
	scene.spawnPos[1] = 150.0f;
	scene.hasSpawn = true;
	return scene;
}

// Chunks of medium and small objects get stand-ins too, and keep their class: drawn as far as the full batches are.
static void testLodObjects() {
	for (const u8 detail : {u8(DETAIL_MEDIUM), u8(DETAIL_SMALL)}) {
		const SrcScene scene = ballScene(detail, 40);
		ChunkerOptions co;
		co.chunkSize = 4000.0f;
		co.splitDetail = true;
		ChunkerStats cs;
		const std::vector<BakedChunk> built = buildChunks(scene, {0, 0}, {0, 0}, co, cs);
		WorldMaterial mat {};
		const std::vector<WorldMaterial> materials = {mat, mat};
		for (const bool objects : {false, true}) {
			std::vector<BakedChunk> chunks = built;
			LodOptions lo;
			lo.enabled = true;
			lo.objects = objects;
			lo.threads = 2;
			LodStats ls;
			buildLods(chunks, materials, lo, ls);
			u64 full = 0, far = 0;
			u32 objectChunks = 0;
			for (const BakedChunk& c : chunks) {
				if (c.tier == 0) continue;
				bool has = false;
				for (const ChunkBatch& b : c.batches) {
					const u8 lod = batchLod(b.detail);
					if (lod == BATCH_LOD_FULL_ONLY) full += b.indexCount / 3;
					if (lod == BATCH_LOD_FAR_ONLY) {
						far += b.indexCount / 3;
						has = true;
						CHECK((b.detail & 3) == detail); // (the class of what it stands in for)
					}
				}
				objectChunks += has;
			}
			std::printf("  lod of %s objects%s: %u chunks, %llu -> %llu triangles\n", detail == DETAIL_MEDIUM ? "medium" : "small",
			            objects ? "" : " (off)", objectChunks, static_cast<unsigned long long>(full), static_cast<unsigned long long>(far));
			if (!objects) {
				CHECK(objectChunks == 0 && ls.objectChunks == 0 && ls.notTierZero > 0);
			} else {
				CHECK(objectChunks >= 1 && ls.objectChunks == objectChunks && full > 0 && far > 0 && far * 10 < full * 7);
				for (const BakedChunk& c : chunks) {
					CHECK(c.header.batchCount == c.batches.size() && c.header.indexCount == c.indices.size());
					for (size_t i = 0; i < c.indices.size(); ++i) CHECK(c.indices[i] < c.vertices.size());
				}
			}
		}
	}
}

// ... and in the engine: far from the camera the stand-ins are drawn in place of the objects.
static void testLodObjectsInEngine() {
	const SrcScene scene = ballScene(DETAIL_MEDIUM, 40);
	const std::string path = gDir + "/lod_objects.pak";
	{
		std::vector<BakeWorld> worlds(1);
		worlds[0].name = "h";
		worlds[0].scene = scene;
		BakeOptions opt;
		opt.chunker.chunkSize = 4000.0f;
		opt.chunker.splitDetail = true;
		opt.tex.maxSize = 32;
		opt.tex.quality = 0;
		opt.threads = 2;
		opt.quiet = true;
		opt.lod.enabled = true;
		PakWriter pak;
		bakeWorlds(worlds, opt, pak);
		pak.write(path);
	}
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), "h"));
	World& w = app.world();
	CHECK((w.header().flags & WORLDF_LOD) != 0);
	w.config().fadeMs = 0.0f;
	app.setDrawDistance(20000.0f);
	app.player().flying = true;
	app.camera().yaw = 0.7f;
	app.camera().pitch = -0.4f;
	auto drawn = [&](float lod) {
		w.config().lodDistance = lod;
		app.teleport({300.0f, 900.0f, 300.0f});
		w.preload(app.camera().pos);
		for (int i = 0; i < 6; ++i) {
			app.update(Input {}, 1.0f / 30.0f);
			app.render();
		}
		return w.stats().trianglesDrawn;
	};
	const u32 full = drawn(0.0f), far = drawn(1.0f);
	std::printf("  lod of objects in the engine: %u triangles drawn, %u with stand-ins\n", full, far);
	CHECK(full > 0 && far > 0 && far < full);
}

static void testLod() {
	testLodWith(false);
	testLodWith(true);
	testLodObjects();
	testLodObjectsInEngine();
}

// ---- fewer triangles in everything (--simplify) ----

// A gentle hill whose far half is medium objects (another detail class), in the chunker's own cells.
static SrcScene simplifyScene() {
	SrcScene scene = hillScene(true);
	for (size_t t = 0; t < scene.triangles.size(); ++t) {
		float cx = 0;
		for (int k = 0; k < 3; ++k) cx += scene.triangles[t].v[k].pos[0] / 3.0f;
		if (cx >= 4000.0f) scene.triangles[t].detail = DETAIL_MEDIUM;
	}
	return scene;
}

// A steep ramp of light across the ground (the edge of a shade): the simplifier carries each triangle's light to the new
// corners by extending its field; where that runs past 0..255 it used to clamp - a black or white corner, a flat wedge.
static void testSimplifySteepLight() {
	SrcScene scene = hillScene(false);
	auto ramp = [](float x) { return std::clamp((x - 3000.0f) / 150.0f, 0.0f, 1.0f) * 255.0f; };
	for (SrcTriangle& t : scene.triangles)
		for (SrcVertex& v : t.v) v.rgba[0] = v.rgba[1] = v.rgba[2] = u8(ramp(v.pos[0]) + 0.5f);
	ChunkerOptions co;
	co.chunkSize = 4000.0f;
	ChunkerStats cs;
	std::vector<BakedChunk> chunks = buildChunks(scene, {0}, {0}, co, cs);
	WorldMaterial mat {};
	const std::vector<WorldMaterial> materials = {mat};
	SimplifyOptions so;
	so.enabled = true;
	so.ratio = 0.3f;
	so.maxError = 60.0f;
	so.threads = 2;
	SimplifyStats ss;
	buildSimplified(chunks, materials, so, ss);
	// What the surface shows at x is the light of the source's triangles: the ramp sampled at the metres, between them linear.
	auto shown = [&](float x) {
		const float x0 = std::floor(x / 100.0f) * 100.0f, f = (x - x0) / 100.0f;
		return ramp(x0) * (1.0f - f) + ramp(x0 + 100.0f) * f;
	};
	// Every simplified triangle's own light, at its middle and at the middles of its edges, against that.
	int worst = 0;
	u64 probes = 0;
	for (const BakedChunk& c : chunks)
		for (const ChunkBatch& b : c.batches)
			for (u32 i = 0; i + 2 < b.indexCount; i += 3) {
				float px[3], pr[3];
				for (int k = 0; k < 3; ++k) {
					const WorldVertex& v = c.vertices[c.indices[b.firstIndex + i + u32(k)]];
					px[k] = c.header.origin[0] + float(v.x) * c.header.posScale;
					pr[k] = float(v.r);
				}
				const float wx[4][3] = {{0.5f, 0.5f, 0.0f}, {0.0f, 0.5f, 0.5f}, {0.5f, 0.0f, 0.5f}, {1.0f / 3, 1.0f / 3, 1.0f / 3}};
				for (const auto& w : wx) {
					const float x = px[0] * w[0] + px[1] * w[1] + px[2] * w[2], r = pr[0] * w[0] + pr[1] * w[1] + pr[2] * w[2];
					worst = std::max(worst, int(std::fabs(r - shown(x))));
					++probes;
				}
			}
	std::printf("  steep light: %llu -> %llu triangles; the light of a simplified triangle is off the source's by %d at most (of 255, %llu probes)\n",
	            static_cast<unsigned long long>(ss.trianglesBefore), static_cast<unsigned long long>(ss.trianglesAfter), worst,
	            static_cast<unsigned long long>(probes));
	CHECK(ss.chunks > 0 && ss.trianglesAfter < ss.trianglesBefore && probes > 0);
	CHECK(worst <= 40);
}

static void testSimplifyWorld() {
	const SrcScene scene = simplifyScene();
	ChunkerOptions co;
	co.chunkSize = 4000.0f;
	co.minTierTriangles = 1; // (the medium half is its own chunks)
	ChunkerStats cs;
	std::vector<BakedChunk> chunks = buildChunks(scene, {0}, {0}, co, cs);
	CHECK(chunks.size() >= 4);
	WorldMaterial mat {};
	const std::vector<WorldMaterial> materials = {mat};
	const std::vector<BakedChunk> original = chunks;
	std::vector<u32> originalCollision;
	for (const BakedChunk& c : original) {
		ChunkCollider cc;
		CHECK(cc.build(c.header, c.batches.data(), c.vertices.data(), c.indices.data(), materials.data(), MATF_NO_COLLISION, 0));
		originalCollision.push_back(cc.triangleCount);
		cc.release();
	}
	SimplifyOptions so;
	so.enabled = true;
	so.ratio = 0.5f;
	so.threads = 2;
	SimplifyStats ss;
	buildSimplified(chunks, materials, so, ss);
	std::printf("  simplify: %llu -> %llu triangles in %u of %u chunks, vertices %llu -> %llu\n", static_cast<unsigned long long>(ss.trianglesBefore),
	            static_cast<unsigned long long>(ss.trianglesAfter), ss.chunks, ss.candidates, static_cast<unsigned long long>(ss.verticesBefore),
	            static_cast<unsigned long long>(ss.verticesAfter));
	CHECK(ss.chunks >= 4 && ss.trianglesAfter * 10 < ss.trianglesBefore * 7);
	CHECK(ss.verticesAfter < ss.verticesBefore); // (what nothing uses any more is dropped)
	auto placeOf = [](const BakedChunk& c, u32 v) {
		return (u64(u16(c.vertices[v].x)) << 32) | (u64(u16(c.vertices[v].y)) << 16) | u64(u16(c.vertices[v].z));
	};
	for (size_t ci = 0; ci < chunks.size(); ++ci) {
		const BakedChunk& c = chunks[ci];
		const BakedChunk& o = original[ci];
		CHECK(c.header.vertexCount == c.vertices.size() && c.header.batchCount == c.batches.size() && c.header.indexCount == c.indices.size());
		std::set<u64> before;
		for (u32 v = 0; v < o.vertices.size(); ++v) before.insert(placeOf(o, v));
		// Every batch is a range inside the indices, with its own box, and nothing new was made: every place was there before.
		u32 expect = 0;
		for (const ChunkBatch& b : c.batches) {
			CHECK(b.firstIndex == expect && b.indexCount % 3 == 0);
			expect += b.indexCount;
			for (u32 i = 0; i < b.indexCount; ++i) {
				const u16 vi = c.indices[b.firstIndex + i];
				CHECK(vi < c.vertices.size() && before.count(placeOf(c, vi)) == 1);
				const i16 p[3] = {c.vertices[vi].x, c.vertices[vi].y, c.vertices[vi].z};
				for (int d = 0; d < 3; ++d) CHECK(p[d] >= b.bbMin[d] && p[d] <= b.bbMax[d]);
			}
		}
		CHECK(expect == c.indices.size());
		// The border of each detail class's triangles is where it was: no crack towards a neighbour, and the other
		// class (drawn or not as the camera moves) fits as before.
		auto borders = [&](const BakedChunk& k) {
			std::map<u8, std::map<std::pair<u64, u64>, int>> perClass;
			for (const ChunkBatch& b : k.batches)
				for (u32 i = 0; i + 2 < b.indexCount; i += 3)
					for (int e = 0; e < 3; ++e) {
						const u64 a = placeOf(k, k.indices[b.firstIndex + i + u32(e)]), d = placeOf(k, k.indices[b.firstIndex + i + u32((e + 1) % 3)]);
						++perClass[u8(b.detail & 3)][{std::min(a, d), std::max(a, d)}];
					}
			std::map<u8, std::set<std::pair<u64, u64>>> out;
			for (const auto& cls : perClass)
				for (const auto& kv : cls.second)
					if (kv.second == 1) out[cls.first].insert(kv.first);
			return out;
		};
		CHECK(borders(c) == borders(o));
		// The collision mesh is the simplified one.
		ChunkCollider cc;
		CHECK(cc.build(c.header, c.batches.data(), c.vertices.data(), c.indices.data(), materials.data(), MATF_NO_COLLISION, 0));
		CHECK(cc.triangleCount > 0 && cc.triangleCount < originalCollision[ci]);
		cc.release();
	}
	// The harder pass over the large objects: they lose more, the medium ones lose what they lost before, the borders stay.
	{
		auto countClass = [](const std::vector<BakedChunk>& cs, u8 cls) {
			u64 n = 0;
			for (const BakedChunk& k : cs)
				for (const ChunkBatch& b : k.batches)
					if ((b.detail & 3u) == cls) n += b.indexCount / 3;
			return n;
		};
		std::vector<BakedChunk> hard = original;
		SimplifyOptions ho = so;
		ho.ratioLarge = 0.25f;
		ho.maxErrorLarge = 30.0f;
		SimplifyStats hs;
		buildSimplified(hard, materials, ho, hs);
		const u64 largePlain = countClass(chunks, DETAIL_ALWAYS), largeHard = countClass(hard, DETAIL_ALWAYS);
		const u64 mediumPlain = countClass(chunks, DETAIL_MEDIUM), mediumHard = countClass(hard, DETAIL_MEDIUM);
		std::printf("  simplify large: class A %llu -> %llu (first pass only: %llu), medium %llu -> %llu\n",
		            static_cast<unsigned long long>(countClass(original, DETAIL_ALWAYS)), static_cast<unsigned long long>(largeHard),
		            static_cast<unsigned long long>(largePlain), static_cast<unsigned long long>(mediumPlain),
		            static_cast<unsigned long long>(mediumHard));
		CHECK(largeHard < largePlain);
		CHECK(mediumHard == mediumPlain);
		for (size_t ci = 0; ci < hard.size(); ++ci) {
			const BakedChunk& c = hard[ci];
			CHECK(c.header.indexCount == c.indices.size());
			for (const ChunkBatch& b : c.batches)
				for (u32 i = 0; i < b.indexCount; ++i) CHECK(c.indices[b.firstIndex + i] < c.vertices.size());
		}
	}
	// Water, glass and cut-out leaves are left as they are.
	{
		std::vector<BakedChunk> kept = original;
		WorldMaterial leaf {};
		leaf.flags = MATF_ALPHA_TEST;
		SimplifyStats none;
		buildSimplified(kept, {leaf}, so, none);
		CHECK(none.chunks == 0);
		for (size_t ci = 0; ci < kept.size(); ++ci) CHECK(kept[ci].indices == original[ci].indices);
	}
	// Through the baker, into an archive the engine draws from.
	const std::string plain = gDir + "/simplify_off.pak", lean = gDir + "/simplify_on.pak";
	for (int on = 0; on < 2; ++on) {
		std::vector<BakeWorld> worlds(1);
		worlds[0].name = "h";
		worlds[0].scene = scene;
		BakeOptions opt;
		opt.chunker.chunkSize = 4000.0f;
		opt.tex.maxSize = 32;
		opt.tex.quality = 0;
		opt.threads = 2;
		opt.quiet = true;
		opt.simplify.enabled = on != 0;
		PakWriter pak;
		const BakeResult r = bakeWorlds(worlds, opt, pak);
		if (on) CHECK(r.report.find("simplified geometry (--simplify)") != std::string::npos);
		pak.write(on ? lean : plain);
	}
	gfx::init();
	u32 drawn[2] = {0, 0};
	for (int on = 0; on < 2; ++on) {
		App app;
		CHECK(app.init((on ? lean : plain).c_str(), "h"));
		World& w = app.world();
		app.setDrawDistance(20000.0f);
		app.player().flying = true;
		app.camera().yaw = 0.7f;
		app.camera().pitch = -0.4f;
		app.teleport({1000.0f, 1500.0f, 1000.0f});
		w.preload(app.camera().pos);
		for (int i = 0; i < 6; ++i) {
			app.update(Input {}, 1.0f / 30.0f);
			app.render();
		}
		drawn[on] = w.stats().trianglesDrawn;
	}
	std::printf("  simplify in the engine: %u triangles drawn, %u simplified\n", drawn[0], drawn[1]);
	CHECK(drawn[0] > 0 && drawn[1] > 0 && drawn[1] * 10 < drawn[0] * 7);
}

// A tube of two bones, a hundred rings of sixteen: the weights change along it, and a part of it
// is the cut-out kind.
static SrcModelPart tubePart(int rings, int around, bool openEnds) {
	SrcModelPart part;
	part.material.name = "skin";
	auto vertex = [&](int r, int a) {
		SrcSkinVertex v {};
		const float angle = float(a) * 6.2831853f / float(around);
		v.pos[0] = 12.0f * std::cos(angle);
		v.pos[1] = float(r) * 4.0f;
		v.pos[2] = 12.0f * std::sin(angle);
		v.normal[0] = std::cos(angle);
		v.normal[1] = 0;
		v.normal[2] = std::sin(angle);
		v.uv[0] = float(a) / float(around);
		v.uv[1] = float(r) / float(rings);
		v.bone[0] = 0;
		v.bone[1] = 1;
		v.weight1 = float(r) / float(rings);
		return v;
	};
	(void)openEnds;
	for (int r = 0; r < rings; ++r)
		for (int a = 0; a < around; ++a) {
			const int a1 = (a + 1) % around;
			SrcSkinVertex q[4] = {vertex(r, a), vertex(r, a1), vertex(r + 1, a1), vertex(r + 1, a)};
			// (the seam of the texture: the last column has its own coordinates)
			if (a == around - 1) {
				q[1].uv[0] = 1.0f;
				q[2].uv[0] = 1.0f;
			}
			const int idx[6] = {0, 2, 1, 0, 3, 2};
			for (int k = 0; k < 6; ++k) part.verts.push_back(q[idx[k]]);
		}
	return part;
}

static void testSimplifyModel() {
	std::vector<SrcModelPart> parts = {tubePart(100, 16, true), tubePart(2, 6, true)};
	const std::vector<SrcModelPart> original = parts;
	ModelSimplifyOptions mo;
	mo.enabled = true;
	mo.ratio = 0.5f;
	ModelSimplifyStats ms;
	simplifyModelParts(parts, std::vector<bool>(parts.size(), false), mo, ms);
	std::printf("  simplify characters: %llu -> %llu triangles in %u of %u parts\n", static_cast<unsigned long long>(ms.before),
	            static_cast<unsigned long long>(ms.after), ms.simplified, ms.parts);
	CHECK(ms.parts == 2 && ms.simplified == 1 && ms.after * 10 < ms.before * 7);
	CHECK(parts[0].verts.size() % 3 == 0 && parts[0].verts.size() / 3 == ms.after);
	CHECK(parts[1].verts.size() == original[1].verts.size()); // too few triangles: left alone
	// Nothing new is made: every vertex is one of the source's, with its bones and its weight.
	std::set<std::vector<float>> known;
	for (const SrcSkinVertex& v : original[0].verts)
		known.insert({v.pos[0], v.pos[1], v.pos[2], v.normal[0], v.normal[1], v.normal[2], v.uv[0], v.uv[1], float(v.bone[0]), float(v.bone[1]), v.weight1});
	for (const SrcSkinVertex& v : parts[0].verts)
		CHECK(known.count({v.pos[0], v.pos[1], v.pos[2], v.normal[0], v.normal[1], v.normal[2], v.uv[0], v.uv[1], float(v.bone[0]), float(v.bone[1]), v.weight1}) == 1);
	// A part told to stay stays.
	std::vector<SrcModelPart> again = original;
	ModelSimplifyStats keep;
	simplifyModelParts(again, {true, false}, mo, keep);
	CHECK(keep.simplified == 0 && again[0].verts.size() == original[0].verts.size());
	// The first and last ring (the open ends) did not move: the tube is as long as it was.
	float lo = 1e9f, hi = -1e9f;
	for (const SrcSkinVertex& v : parts[0].verts) {
		lo = std::min(lo, v.pos[1]);
		hi = std::max(hi, v.pos[1]);
	}
	CHECK(lo == 0.0f && hi == 400.0f);

	// Bones that change every ring (a body at its joints) leave no vertex of the source to collapse onto; the
	// coarse far bodies (synthesize) make one there, the ordinary --simplify does not (fix88).
	{
		SrcModelPart joints = tubePart(100, 16, true);
		for (size_t i = 0; i < joints.verts.size(); ++i) {
			SrcSkinVertex& v = joints.verts[i];
			const int block = int(v.pos[1] / 4.0f);
			v.bone[0] = block % 7;
			v.bone[1] = (block + 1) % 7;
			v.weight1 = 0.5f;
			v.uv[0] += 0.003f * float(int(v.pos[1]) % 3); // (a mapping that is not linear)
		}
		size_t counts[2];
		for (int synth = 0; synth < 2; ++synth) {
			std::vector<SrcModelPart> p = {joints};
			ModelSimplifyOptions o;
			o.enabled = true;
			o.ratio = 0.3f;
			o.maxError = 6.0f;
			o.synthesize = synth != 0;
			ModelSimplifyStats st;
			simplifyModelParts(p, {false}, o, st);
			counts[synth] = p[0].verts.size() / 3;
			CHECK(p[0].verts.size() % 3 == 0);
		}
		std::printf("  simplify far bodies: %zu triangles of %zu without new vertices, %zu with\n", counts[0], joints.verts.size() / 3, counts[1]);
		CHECK(counts[1] < counts[0] && counts[1] * 10 < (joints.verts.size() / 3) * 5);
	}

	// Through the model baker: fewer triangles in the archive, the same skeleton.
	SrcModel m;
	m.name = "tube";
	SrcNode root, child;
	root.name = "ROOT";
	child.name = "BONE";
	child.parent = 0;
	child.pos[1] = 200.0f;
	m.nodes = {root, child};
	m.parts = {tubePart(100, 16, true)};
	size_t triangles[2] = {0, 0};
	for (int on = 0; on < 2; ++on) {
		BakeOptions bo;
		bo.quiet = true;
		bo.simplifyModels.enabled = on != 0;
		bo.simplifyModels.ratio = 0.5f;
		PakWriter writer;
		const ModelBakeResult r = bakeModel(m, bo, writer);
		triangles[on] = r.triangles;
		if (on) CHECK(r.report.find("simplified (--simplify)") != std::string::npos);
	}
	std::printf("  simplify in the model baker: %zu -> %zu triangles\n", triangles[0], triangles[1]);
	CHECK(triangles[0] == 3200 && triangles[1] > 0 && triangles[1] * 10 < triangles[0] * 7);
}

// The top screen's refresh rate from its timing registers: the rate asked for is what comes out, the visible part
// and the order of the edges stay, and registers that are not the top screen's are left alone.
static void testDisplayTiming() {
	PdcTiming base;
	base.reg[0] = 0x1C2;
	base.reg[1] = 0xD1;
	base.reg[2] = 0x1C1;
	base.reg[3] = 0x1C1;
	base.reg[4] = 0;
	base.reg[5] = 0xCF;
	base.reg[6] = 0xD1;
	base.reg[7] = 0x1C501C1;
	base.reg[9] = 0x19D;
	base.reg[23] = 0x19000F0;
	base.reg[24] = 0x1C100D1;
	base.reg[25] = 0x1920002;
	CHECK(pdcKnown(base));
	std::printf("  display: defaults %.2f Hz\n", double(pdcRefresh(base)));
	CHECK(pdcRefresh(base) > 59.5f && pdcRefresh(base) < 60.1f);
	for (float hz : {30.0f, 45.0f, 50.0f, 55.0f, 59.8f, 60.0f, 65.0f, 72.0f, 80.0f, 90.0f}) {
		PdcTiming t;
		const float made = pdcTimingFor(base, hz, t);
		std::printf("  display: asked %.1f made %.2f (HTotal %u VTotal %u)\n", double(hz), double(made), unsigned(t.reg[0] & 0xFFF), unsigned(t.reg[9] & 0xFFF));
		CHECK(made > hz * 0.985f && made < hz * 1.015f);
		CHECK(pdcRefresh(t) == made && pdcKnown(t));
		// The visible part is 240 pixels wide, as it was; the edges keep their order; nothing else was touched.
		CHECK((t.reg[24] >> 16) - (t.reg[24] & 0xFFFF) == 240);
		CHECK(t.reg[1] < t.reg[0] && t.reg[6] == t.reg[1] && t.reg[5] + 2 == t.reg[1] && t.reg[2] < t.reg[0] + 1 && t.reg[3] <= t.reg[0]);
		CHECK(t.reg[4] == base.reg[4] && t.reg[23] == base.reg[23] && t.reg[25] == base.reg[25]);
		CHECK(t.reg[9] >= (base.reg[9] & 0xFFF) || hz >= pdcRefresh(base));
	}
	// Out of range is held to 30..90, and what is not the top screen's is not touched.
	PdcTiming t;
	CHECK(pdcTimingFor(base, 500.0f, t) < 91.0f && pdcTimingFor(base, 1.0f, t) > 23.0f && pdcTimingFor(base, 1.0f, t) < 25.0f);
	PdcTiming odd = base;
	odd.reg[0] = 0x20;
	CHECK(!pdcKnown(odd) && pdcTimingFor(odd, 72.0f, t) == 0.0f && t.reg[0] == 0x20);
	odd = base;
	odd.reg[24] = 0x1C100F1; // 224 wide: another screen
	CHECK(!pdcKnown(odd));
	// The menu's setting: values, range.
	Settings st;
	CHECK(st.refresh == 0 && st.refreshMaxHz() == 0.0f);
	st.refresh = 2;
	CHECK(st.refreshMaxHz() == 72.0f);
	st.refresh = 5;
	CHECK(st.refreshMaxHz() == 80.0f && st.refreshHold());
	st.refresh = 3;
	CHECK(st.refreshMaxHz() == 80.0f && !st.refreshHold());
	st.refresh = 9;
	st.sanitize();
	CHECK(st.refresh == 0);
}

// The re-projection of a turning view (engine/reproject.h): how far the old picture is moved is what the engine's own
// camera would have moved it, the guess follows the turn the last pictures made and dies away, and it stays within
// what the buffer has to spare.
static void testReproject() {
	// 1. The geometry, against the engine's own projection: the middle of the picture, seen from a camera turned by
	//    `d` more, lands focal * tan(d) pixels to the left (the world slides left when the view turns right).
	{
		YawReprojector r;
		for (float fov : {0.9f, 1.0472f, 1.3f})
			for (float yaw0 : {0.0f, 1.0f, -2.0f, 3.0f})
				for (float d : {0.03f, -0.03f, 0.12f}) {
					r.cfg.fovY = fov;
					const Vec3 eye = {120.0f, 50.0f, -300.0f};
					const Vec3 look = forwardFromYawPitch(yaw0, 0.0f);
					const Vec4 world = {eye.x + look.x * 1000.0f, eye.y + look.y * 1000.0f, eye.z + look.z * 1000.0f, 1.0f};
					const Mat4 proj = perspectiveLH(fov, 400.0f / 240.0f, 20.0f, 14000.0f);
					const Vec4 clip = transform(mul(proj, viewLH(eye, yaw0 + d, 0.0f)), world);
					const float ndcX = clip.x / clip.w;
					const float movedLeft = -ndcX * 200.0f;
					CHECK(std::fabs(movedLeft - r.focalPixels() * std::tan(d)) < 0.02f);
					CHECK((movedLeft > 0) == (d > 0));
				}
	}
	// 2. A steady turn of 1 rad/s, a picture every 16.7 ms: nothing to do while the pictures keep coming.
	{
		YawReprojector r;
		double t = 10.0;
		float yaw = 0.5f;
		int presents = 0;
		for (int i = 0; i < 30; ++i) {
			r.arrive(yaw, t);
			presents += r.onVBlank(true, t, 24, 24).present;
			t += 1.0 / 60.0;
			yaw += 1.0f / 60.0f;
		}
		CHECK(presents == 0 && std::fabs(r.rate() - 1.0f) < 0.02f && r.measured());
		// 3. Then the pictures stop coming: the old one is moved further with every refresh, never past the cap, and
		//    each change is told to the display once.
		int told = 0, last = 0;
		bool grows = true;
		for (int k = 1; k <= 5; ++k) {
			const double now = t - 1.0 / 60.0 + double(k) / 60.0;
			const ReprojectStep st = r.onVBlank(false, now, 24, 24);
			const float age = float(k) / 60.0f;
			const float want = r.focalPixels() * std::tan(1.0f * 0.15f * (1.0f - std::exp(-age / 0.15f)));
			CHECK(std::fabs(float(st.columns) - want) <= 0.51f);
			CHECK(st.columns >= last && st.columns <= 24 && st.columns > 0);
			grows = grows && st.columns >= last;
			told += st.present;
			CHECK(st.present == (st.columns != last));
			last = st.columns;
		}
		CHECK(grows && told >= 3 && last >= 10 && last <= 24);
		// 4. A new picture comes: the display is not told anything (the system has put it on the screen as it is),
		//    and the next refresh without one starts again from little.
		CHECK(!r.onVBlank(true, t + 0.1, 24, 24).present);
		r.arrive(yaw + 0.05f, t + 0.1);
		const ReprojectStep again = r.onVBlank(false, t + 0.1 + 1.0 / 60.0, 24, 24);
		CHECK(again.columns < last);
	}
	// 5. What the buffer has to spare limits the move; no room in a direction means no move that way.
	{
		YawReprojector r;
		r.arrive(0.0f, 1.0);
		r.arrive(0.1f, 1.0 + 1.0 / 30.0); // 3 rad/s to the right
		CHECK(r.onVBlank(false, 1.2, 24, 3).columns == 3);
		YawReprojector l;
		l.arrive(0.0f, 1.0);
		l.arrive(-0.1f, 1.0 + 1.0 / 30.0); // to the left
		const ReprojectStep none = l.onVBlank(false, 1.2, 0, 24);
		CHECK(none.columns == 0 && !none.present);
		const ReprojectStep some = l.onVBlank(false, 1.2, 5, 24);
		CHECK(some.columns == -5 && some.present);
		// The cap, whatever the turn and the wait.
		YawReprojector fast;
		fast.arrive(0.0f, 1.0);
		fast.arrive(0.12f, 1.0 + 1.0 / 60.0); // 7.2 rad/s
		CHECK(fast.columnsAt(3.0) == fast.cfg.maxColumns);
	}
	// 6. What is not a measure of a turn gives no guess: a cut, a long pause, a crawl, a madman; the turn through
	//    +-pi is a small turn.
	{
		YawReprojector r;
		r.arrive(0.0f, 1.0);
		r.arrive(1.0f, 1.0 + 1.0 / 60.0);
		CHECK(!r.measured() && r.columnsAt(1.1) == 0); // a cut
		r.arrive(0.0f, 5.0);
		r.arrive(0.1f, 5.6);
		CHECK(!r.measured()); // 0.6 s apart: a pause, not a turn
		r.arrive(0.0f, 8.0);
		r.arrive(0.0005f, 8.0 + 1.0 / 60.0);
		CHECK(!r.measured()); // 0.03 rad/s
		r.arrive(0.0f, 9.0);
		r.arrive(0.5f, 9.0 + 0.01);
		CHECK(!r.measured()); // 50 rad/s
		r.arrive(3.13f, 12.0);
		r.arrive(-3.13f, 12.0 + 1.0 / 60.0);
		CHECK(r.measured() && r.rate() > 1.0f && r.rate() < 2.0f);
		r.reset();
		CHECK(!r.measured() && r.columnsAt(100.0) == 0);
	}
	// 7. The setting: off at first, kept by the file, one or none, back to off with the graphics.
	{
		Settings st;
		CHECK(st.reproject == 0);
		st.reproject = 1;
		const std::string path = gDir + "/reproject_settings.bin";
		CHECK(st.save(path.c_str()));
		Settings back;
		CHECK(back.load(path.c_str()) && back.reproject == 1);
		back.reproject = 7;
		back.sanitize();
		CHECK(back.reproject == 1);
		back.resetGraphics();
		CHECK(back.reproject == 0);
		Menu m;
		bool found = false;
		for (int i = 0; i < m.itemCount(Menu::PAGE_GFX_PLUS); ++i) found = found || std::strstr(m.label(Menu::PAGE_GFX_PLUS, i), "Доворот") != nullptr;
		CHECK(found);
	}
}

// Reading ahead (engine/readahead.h): what is wished for is read and waits; the archive hands it over once and then
// reads from the card as before; a model brings its textures and the animations the game has been seen to fetch; a
// full cache drops what has waited longest; the switch drops everything.
static void testReadAhead() {
	const std::string path = gDir + "/readahead.pak";
	auto blobOf = [](size_t n, u8 seed) {
		std::vector<u8> v(n);
		for (size_t i = 0; i < n; ++i) v[i] = u8(seed + i * 7);
		return v;
	};
	const u32 texHash = hashName("tex/skin"), runName = hashName("S_RUN"), walkName = hashName("S_WALK");
	const u32 runClip = hashName("clip/run"), walkClip = hashName("clip/walk");
	std::vector<u8> model(sizeof(ModelHeader) + sizeof(ModelNode) + sizeof(ModelPart) + 2 * sizeof(ModelClipRef), 0);
	{
		ModelHeader h {};
		h.magic = MODEL_MAGIC;
		h.version = MODEL_VERSION;
		h.nodeCount = 1;
		h.partCount = 1;
		h.clipCount = 2;
		memcpy(model.data(), &h, sizeof(h));
		ModelPart part {};
		part.texture = texHash;
		memcpy(model.data() + sizeof(ModelHeader) + sizeof(ModelNode), &part, sizeof(part));
		const ModelClipRef refs[2] = {{runName, runClip}, {walkName, walkClip}};
		memcpy(model.data() + sizeof(ModelHeader) + sizeof(ModelNode) + sizeof(ModelPart), refs, sizeof(refs));
	}
	{
		PakWriter w;
		w.add("model/man", ASSET_MODEL, model);
		w.add("tex/skin", ASSET_TEXTURE, blobOf(3000, 1));
		w.add("clip/run", ASSET_CLIP, blobOf(2000, 2));
		w.add("clip/walk", ASSET_CLIP, blobOf(2000, 3));
		for (int i = 0; i < 20; ++i) w.add("chunk/" + std::to_string(i), ASSET_CHUNK, blobOf(40000, u8(10 + i)));
		w.add("sound/x", ASSET_SOUND, blobOf(500, 9));
		w.write(path);
	}
	const u32 modelHash = hashName("model/man");

	// 1. Without a thread: wishes are read by pump(); the model brings the texture and the run clip, not the walk clip
	//    (nobody has fetched it); the archive takes the bytes once.
	{
		ReadAhead ra;
		Pak pak;
		CHECK(pak.open(path.c_str()) && ra.start(path.c_str(), 1u << 20, false));
		ra.attach(pak);
		ra.noteClip(runName);
		ra.request(modelHash);
		ra.request(modelHash); // (the same wish twice is one)
		ra.request(hashName("sound/x")); // (not worth it: read and then ignored)
		while (ra.pump()) {}
		CHECK(ra.has(modelHash) && ra.has(texHash) && ra.has(runClip) && !ra.has(walkClip) && !ra.has(hashName("sound/x")));
		const PakEntry* e = pak.find(modelHash);
		CHECK(e != nullptr);
		void* got = e ? pak.readAlloc(*e) : nullptr;
		CHECK(got && std::memcmp(got, model.data(), model.size()) == 0);
		free(got);
		CHECK(ra.stats().hits == 1 && !ra.has(modelHash));
		void* again = pak.readAlloc(*e); // (from the card now)
		CHECK(again && std::memcmp(again, model.data(), model.size()) == 0 && ra.stats().misses == 1);
		free(again);
		// read() takes from the cache too, into the caller's buffer.
		const PakEntry* te = pak.find(texHash);
		std::vector<u8> buf(te->size);
		CHECK(pak.read(*te, buf.data()) && buf == blobOf(3000, 1) && ra.stats().hits == 2);
		// A clip fetched through the archive teaches the name.
		pak.noteClip(walkName);
		ra.request(modelHash);
		while (ra.pump()) {}
		CHECK(ra.has(walkClip));
		// 4. The switch.
		ra.setEnabled(false);
		CHECK(!ra.has(walkClip) && ra.stats().entries == 0 && ra.stats().bytes == 0);
		ra.request(modelHash);
		CHECK(!ra.pump() && !ra.has(modelHash));
		ra.setEnabled(true);
		ra.stop();
	}
	// 2. A cache that holds only part of the wishes: the one that has waited longest goes, the size is kept.
	{
		ReadAhead ra;
		CHECK(ra.start(path.c_str(), 130000, false));
		for (int i = 0; i < 20; ++i) ra.request(hashName(("chunk/" + std::to_string(i)).c_str()));
		// (the queue holds 48: all twenty are in)
		while (ra.pump()) {}
		const ReadAheadStats st = ra.stats();
		std::printf("  read-ahead: %u chunks in %u bytes, %u evicted\n", st.entries, st.bytes, st.evicted);
		CHECK(st.bytes <= 130000 && st.entries == 3 && st.evicted == 17 && st.loaded == 20);
		CHECK(ra.has(hashName("chunk/19")) && ra.has(hashName("chunk/17")) && !ra.has(hashName("chunk/0")));
		// A wish for what is waiting makes it the youngest: chunk 17 survives the next two.
		ra.request(hashName("chunk/17"));
		ra.request(hashName("chunk/0"));
		ra.request(hashName("chunk/1"));
		while (ra.pump()) {}
		CHECK(ra.has(hashName("chunk/17")) && ra.has(hashName("chunk/1")) && !ra.has(hashName("chunk/18")));
	}
	// 3. With the thread: the wishes are read on their own.
	{
		ReadAhead ra;
		CHECK(ra.start(path.c_str(), 1u << 20, true));
		CHECK(ra.running());
		for (int i = 0; i < 6; ++i) ra.request(hashName(("chunk/" + std::to_string(i)).c_str()));
		bool all = false;
		for (int t = 0; t < 4000 && !all; ++t) {
			all = true;
			for (int i = 0; i < 6; ++i) all = all && ra.has(hashName(("chunk/" + std::to_string(i)).c_str()));
			if (!all) usleep(500);
		}
		CHECK(all && ra.stats().loaded == 6);
		ra.stop();
		CHECK(!ra.running() && !ra.has(hashName("chunk/0")));
	}
	// 5. The menu's setting and the stream's: on at first, kept by the file.
	{
		Settings st;
		CHECK(st.readAhead == 1);
		st.readAhead = 0;
		const std::string sp = gDir + "/readahead_settings.bin";
		CHECK(st.save(sp.c_str()));
		Settings back;
		CHECK(back.load(sp.c_str()) && back.readAhead == 0);
	}
}

// Models that differ in their textures only name one mesh (fix88): made once, read and put on the GPU once.
static void testSharedMeshes() {
	SrcNode root, child;
	root.name = "ROOT";
	child.name = "BONE";
	child.parent = 0;
	child.pos[1] = 200.0f;
	const auto make = [&](const char* name, int rings, const char* texture) {
		SrcModel m;
		m.name = name;
		m.nodes = {root, child};
		m.parts = {tubePart(rings, 16, true)};
		m.parts[0].material.texture = texture; // (no picture to load: the part is baked untextured, and the same mesh)
		return m;
	};
	BakeOptions shared;
	shared.quiet = true;
	shared.sharedMeshes = true;
	BakeOptions plain;
	plain.quiet = true;
	PakWriter writer, writerPlain;
	const ModelBakeResult a = bakeModel(make("a", 20, "SKIN_A.TGA"), shared, writer);
	const ModelBakeResult b = bakeModel(make("b", 20, "SKIN_B.TGA"), shared, writer);
	const ModelBakeResult c = bakeModel(make("c", 30, "SKIN_A.TGA"), shared, writer);
	bakeModel(make("a", 20, "SKIN_A.TGA"), plain, writerPlain);
	CHECK(a.meshEntry != 0 && a.meshEntry == b.meshEntry && c.meshEntry != a.meshEntry);
	CHECK(a.meshNew && !b.meshNew && c.meshNew);
	CHECK(writer.rawBytes(ASSET_MODEL) < writerPlain.rawBytes(ASSET_MODEL) * 3); // 3 models + 2 meshes against 1 whole
	const std::string path = gDir + "/sharedmeshes.pak";
	writer.write(path);

	gfx::init();
	{
		Pak pak;
		CHECK(pak.open(path.c_str()));
		const int base = gfx::memStats().meshCount;
		MeshPool pool;
		{
			Model ma, mb, mc;
			CHECK(ma.loadEntry(pak, hashName("model/a"), nullptr, true, &pool));
			CHECK(mb.loadEntry(pak, hashName("model/b"), nullptr, true, &pool));
			CHECK(mc.loadEntry(pak, hashName("model/c"), nullptr, true, &pool));
			CHECK(ma.hasMesh() && mb.hasMesh() && mc.hasMesh() && ma.triangles() == 640 && mc.triangles() == 960);
			CHECK(pool.count() == 2 && pool.reads() == 2 && pool.hits() == 1);
			CHECK(gfx::memStats().meshCount == base + 2); // (the same GPU mesh for a and b)
		}
		// Nobody wears them: they stay (the same people come back), and are found again.
		CHECK(pool.count() == 2 && gfx::memStats().meshCount == base + 2);
		{
			Model ma;
			CHECK(ma.loadEntry(pak, hashName("model/a"), nullptr, true, &pool));
			CHECK(pool.reads() == 2 && pool.hits() == 2);
		}
		// With no room to spare an unused mesh goes, the longest unused first.
		pool.setIdleBudget(1);
		{
			Model mc;
			CHECK(mc.loadEntry(pak, hashName("model/c"), nullptr, true, &pool));
		}
		CHECK(pool.count() <= 1);
		pool.release();
		CHECK(gfx::memStats().meshCount == base);
		// Warmed meshes stay however little room there is.
		MeshPool warm;
		warm.setIdleBudget(1);
		CHECK(warm.warm(pak, a.meshEntry) && !warm.warm(pak, hashName("model/a")));
		{
			Model ma;
			CHECK(ma.loadEntry(pak, hashName("model/a"), nullptr, true, &warm));
			CHECK(warm.hits() == 1 && warm.reads() == 1);
		}
		CHECK(warm.count() == 1);
		warm.release();
		// Without a pool the model reads its mesh for itself, and gives it back.
		{
			Model ma;
			CHECK(ma.loadEntry(pak, hashName("model/a")) && ma.hasMesh());
			CHECK(gfx::memStats().meshCount == base + 1);
		}
		CHECK(gfx::memStats().meshCount == base);
	}
	// A mesh that is missing: the model is not used, and nothing is left behind.
	{
		Pak full;
		CHECK(full.open(path.c_str()));
		const PakEntry* me = full.find("model/a");
		CHECK(me != nullptr);
		PakWriter noMesh;
		if (me) {
			u8* raw = static_cast<u8*>(full.readAlloc(*me));
			CHECK(raw != nullptr);
			if (raw) noMesh.add("model/a", ASSET_MODEL, std::vector<u8>(raw, raw + me->size));
			free(raw);
		}
		const std::string npath = gDir + "/nomesh.pak";
		noMesh.write(npath);
		Pak pak;
		CHECK(pak.open(npath.c_str()));
		const int base = gfx::memStats().meshCount;
		MeshPool pool;
		Model m;
		CHECK(!m.loadEntry(pak, hashName("model/a"), nullptr, true, &pool) && !m.loaded());
		CHECK(!m.loadEntry(pak, hashName("model/a")) && !m.loaded());
		CHECK(gfx::memStats().meshCount == base && pool.count() == 0);
	}
	gfx::shutdown();
}

static void testShore() {
	SrcScene scene;
	scene.materials.resize(3);
	scene.materials[1].water = true;
	scene.materials[2].noCollision = true; // a plant
	auto tri = [&](u32 m, float x0, float y0, float z0, float x1, float y1, float z1, float x2, float y2, float z2) {
		SrcTriangle t {};
		t.material = m;
		const float p[3][3] = {{x0, y0, z0}, {x1, y1, z1}, {x2, y2, z2}};
		for (int k = 0; k < 3; ++k) {
			for (int a = 0; a < 3; ++a) t.v[k].pos[a] = p[k][a];
			t.v[k].rgba[0] = t.v[k].rgba[1] = t.v[k].rgba[2] = 180;
			t.v[k].rgba[3] = 255;
		}
		scene.triangles.push_back(t);
	};
	tri(1, -20000, 0, -20000, 20000, 0, -20000, -20000, 0, 20000);
	tri(1, 20000, 0, -20000, 20000, 0, 20000, -20000, 0, 20000);
	// ground tilting up out of the sea (front faces up: the winding of the water grid test's ground)
	tri(0, 0, -300, 0, 2000, 300, 0, 0, 300, 2000);
	tri(0, 2000, 300, 0, 2000, 300, 2000, 0, 300, 2000);
	// Depth colour and foam.
	WaterGridOptions opt;
	opt.cell = 200.0f;
	opt.shore = 1.0f;
	WaterGrid grid(scene, opt);
	CHECK(grid.active());
	std::vector<SrcTriangle> pieces;
	for (int i = 0; i < 2; ++i) grid.cut(scene.triangles[size_t(i)], pieces);
	int lo = 255, hi = 0, bright = 0;
	for (const SrcTriangle& t : pieces)
		for (int k = 0; k < 3; ++k) {
			lo = std::min<int>(lo, t.v[k].rgba[0]);
			hi = std::max<int>(hi, t.v[k].rgba[1]);
			bright += t.v[k].rgba[0] > 190;
		}
	CHECK(lo == 180 && hi > 200 && hi < 235 && bright > 0); // deep water keeps its tone; the shallows are a little lighter (not milky); the edge warms to foam
	// Wet ground: the corner at the water's level darkens, one 3 m up does not.
	GroundFxStats st;
	bakeWetShore(scene, 1.0f, st);
	const SrcTriangle& ground = scene.triangles[2];
	CHECK(st.wetCorners > 0);
	// (corner 0 is at y -300 under the sea, corner 1 at y 300 on dry land)
	CHECK(ground.v[0].rgba[0] < 130 && ground.v[1].rgba[0] == 180);
	// Canopy: a plant over one corner of a flat ground shades that corner only.
	SrcScene c;
	c.materials.resize(3);
	c.materials[2].noCollision = true;
	auto ctri = [&](u32 m, float x0, float y0, float z0, float x1, float y1, float z1, float x2, float y2, float z2) {
		SrcTriangle t {};
		t.material = m;
		const float p[3][3] = {{x0, y0, z0}, {x1, y1, z1}, {x2, y2, z2}};
		for (int k = 0; k < 3; ++k) {
			for (int a = 0; a < 3; ++a) t.v[k].pos[a] = p[k][a];
			t.v[k].rgba[0] = t.v[k].rgba[1] = t.v[k].rgba[2] = 200;
			t.v[k].rgba[3] = 255;
		}
		c.triangles.push_back(t);
	};
	ctri(0, 0, 0, 0, 3000, 0, 0, 0, 0, 3000);          // ground, facing up
	ctri(2, -200, 500, -200, -200, 500, 400, 400, 500, -200); // leaves over the corner (0, 0, 0)
	std::vector<char> plant = {0, 0, 1};
	GroundFxStats cs;
	bakeCanopy(c, plant, 1.0f, 1800.0f, 1, cs);
	CHECK(cs.shadedCorners >= 1 && c.triangles[0].v[0].rgba[0] < 185 && c.triangles[0].v[1].rgba[0] == 200 && c.triangles[0].v[2].rgba[0] == 200);
	// None of it with strength 0.
	SrcScene d = c;
	for (SrcVertex& v : d.triangles[0].v) v.rgba[0] = 200;
	bakeCanopy(d, plant, 0.0f, 1800.0f, 1, cs);
	CHECK(d.triangles[0].v[0].rgba[0] == 200);
}

// fix140: a river through a valley that runs down towards +x (or -x): the water in it flows that way, and the shores get foam.
// The player's ripples (fix141): rings spread and fade; none outside the water.
static void testRipples() {
	CHECK(Ripples::radius(0.0f, 1.0f) < Ripples::radius(0.5f, 1.0f) && Ripples::radius(0.5f, 1.0f) < Ripples::radius(1.0f, 1.0f));
	CHECK(Ripples::radius(1.0f, 1.0f) > Ripples::radius(1.0f, 0.2f));
	CHECK(Ripples::opacity(0.0f, 0.8f) > Ripples::opacity(0.5f, 0.8f) && Ripples::opacity(1.0f, 0.8f) == 0.0f);
	Ripples r;
	CHECK(r.rings() == 0);
	// fix150: the shapes grow, fade in quickly and out at the end; the atlas has something in every tile and nothing on borders.
	for (u32 k = 0; k < 4; ++k) {
		const Ripples::Kind kind = Ripples::Kind(k);
		CHECK(Ripples::extent(kind, 0.8f, 0.5f) > Ripples::extent(kind, 0.1f, 0.5f));
		CHECK(Ripples::visibility(kind, 0.0f, 0.8f) == 0.0f && Ripples::visibility(kind, 0.3f, 0.8f) > 0.0f && Ripples::visibility(kind, 1.0f, 0.8f) == 0.0f);
		CHECK(Ripples::tileOf(kind) == k);
	}
	static u8 atlas[128 * 128];
	Ripples::makeAtlas(atlas);
	for (int ty = 0; ty < 2; ++ty)
		for (int tx = 0; tx < 2; ++tx) {
			unsigned sum = 0, border = 0;
			for (int y = 0; y < 64; ++y)
				for (int x = 0; x < 64; ++x) {
					const u8 v = atlas[(ty * 64 + y) * 128 + tx * 64 + x];
					sum += v;
					if (x == 0 || y == 0 || x == 63 || y == 63) border += v;
				}
			CHECK(sum > 64u * 64u * 4u);
			CHECK(border < 64u * 4u * 4u);
		}
}

static void testWaterFx() {
	auto build = [](float bedSlope) {
		SrcScene scene;
		scene.materials.resize(2);
		scene.materials[1].water = true;
		scene.materials[1].scroll[0] = 0.01f; // (the game's own scroll: replaced in the river)
		auto tri = [&](u32 m, const float p[3][3], bool water) {
			SrcTriangle t {};
			t.material = m;
			for (int k = 0; k < 3; ++k) {
				for (int a = 0; a < 3; ++a) t.v[k].pos[a] = p[k][a];
				t.v[k].uv[0] = p[k][0] / 900.0f;
				t.v[k].uv[1] = p[k][2] / 900.0f;
				t.v[k].rgba[0] = t.v[k].rgba[1] = t.v[k].rgba[2] = water ? 200 : 120;
				t.v[k].rgba[3] = 255;
			}
			scene.triangles.push_back(t);
		};
		const float x0 = -9000.0f, x1 = 9000.0f, z0 = -3000.0f, z1 = 3000.0f, step = 200.0f;
		auto ground = [&](float x, float z) {
			// A channel 6 m wide whose bed falls by 2 m along 18 km, banks 4 m over the water.
			const float bed = -60.0f - bedSlope * (x - x0) / (x1 - x0) * 200.0f - (bedSlope < 0 ? 200.0f * -1.0f * 0.0f : 0.0f);
			const float t = std::clamp((std::fabs(z) - 200.0f) / 300.0f, 0.0f, 1.0f);
			const float s = t * t * (3.0f - 2.0f * t);
			return bed + (400.0f - bed) * s;
		};
		for (float x = x0; x < x1; x += step)
			for (float z = z0; z < z1; z += step) {
				const float a[3][3] = {{x, ground(x, z), z}, {x + step, ground(x + step, z), z}, {x, ground(x, z + step), z + step}};
				const float b[3][3] = {{x + step, ground(x + step, z), z}, {x + step, ground(x + step, z + step), z + step}, {x, ground(x, z + step), z + step}};
				tri(0, a, false);
				tri(0, b, false);
			}
		const float w1[3][3] = {{x0, 0, z0}, {x1, 0, z0}, {x0, 0, z1}}, w2[3][3] = {{x1, 0, z0}, {x1, 0, z1}, {x0, 0, z1}};
		tri(1, w1, true);
		tri(1, w2, true);
		return scene;
	};
	for (int dir = 0; dir < 2; ++dir) {
		// dir 0: the bed falls towards +x, 1: towards -x (it rises).
		SrcScene scene = build(dir == 0 ? 1.0f : -1.0f);
		if (dir == 1) {
			// (rising towards +x: the bed's height is the mirror image)
		}
		const size_t trianglesBefore = scene.triangles.size();
		WaterFxOptions opt;
		opt.foam = 1.0f;
		opt.flow = 70.0f;
		opt.glint = dir == 0 ? 0.5f : 0.0f;
		WaterFxStats st;
		bakeWaterFx(scene, opt, st);
		{
			bool glintMaterial = false;
			for (const SrcMaterial& m : scene.materials)
				if (m.name == "G3_WATER_GLINT" && m.blend == SrcBlend::Add) glintMaterial = true;
			CHECK(glintMaterial == (dir == 0) && (st.glintTriangles > 0) == (dir == 0));
		}
		if (false) std::printf("  water fx: %u shore tiles, %u chains, %llu foam triangles, %u river tiles, %u flow materials\n", st.shoreTiles, st.shoreChains, (unsigned long long)st.foamTriangles, st.riverTiles, st.flowMaterials);
		CHECK(st.shoreTiles >= 3 && st.riverTiles >= 3 && st.flowMaterials == 1 && st.foamTriangles > 200 && st.shoreChains >= 1);
		// The flow's copy of the water material scrolls the pattern with the water: -J^-1 scroll = (+-70 cm/s, 0) in x z; J = 1/900.
		int flowMaterial = -1;
		for (size_t i = 0; i < scene.materials.size(); ++i)
			if (scene.materials[i].name.find("_FLOW") != std::string::npos) flowMaterial = int(i);
		CHECK(flowMaterial > 0);
		if (flowMaterial > 0) {
			const SrcMaterial& m = scene.materials[size_t(flowMaterial)];
			const float want = (dir == 0 ? -1.0f : 1.0f) * 70.0f / 900.0f;
			CHECK(m.water && std::fabs(m.scroll[0] - want) < 0.002f && std::fabs(m.scroll[1]) < 0.002f);
		}
		// The foam: its own blended material after the water's, lying over the water's edge, within the channel.
		int foamMaterial = -1;
		for (size_t i = 0; i < scene.materials.size(); ++i)
			if (scene.materials[i].texture == kFoamTexture) foamMaterial = int(i);
		CHECK(foamMaterial > 0 && scene.materials[size_t(foamMaterial)].blend == SrcBlend::Alpha && scene.materials[size_t(foamMaterial)].noCollision);
		u64 foam = 0, moved = 0, outside = 0, below = 0;
		for (const SrcTriangle& t : scene.triangles) {
			if (int(t.material) == foamMaterial) {
				++foam;
				for (const SrcVertex& v : t.v) {
					outside += std::fabs(v.pos[2]) > 800.0f;
					below += v.pos[1] < 0.5f;
				}
			} else if (t.material < scene.materials.size() && scene.materials[t.material].name.find("_FLOW") != std::string::npos) {
				++moved;
			}
		}
		CHECK(foam == st.foamTriangles && moved == st.flowTriangles && moved > 0 && outside == 0 && below == 0);
		CHECK(scene.triangles.size() > trianglesBefore);
	}
	// Nothing asked for: nothing done. A lake (no channel): no flow.
	{
		SrcScene scene = build(1.0f);
		const size_t n = scene.triangles.size(), m = scene.materials.size();
		WaterFxOptions off;
		WaterFxStats st;
		bakeWaterFx(scene, off, st);
		CHECK(scene.triangles.size() == n && scene.materials.size() == m);
	}
	// The foam's picture tiles along the shore and fades at both edges.
	{
		const SrcImage f = makeFoamTexture(64, 32);
		CHECK(f.width == 64 && f.height == 32 && f.rgba.size() == 64u * 32u * 4u);
		int solid = 0;
		for (int x = 0; x < 64; ++x) {
			CHECK(f.rgba[size_t(31 * 64 + x) * 4 + 3] < 8); // the open water's edge: nothing
			solid += f.rgba[size_t(10 * 64 + x) * 4 + 3] > 40;
		}
		CHECK(solid > 8); // some foam at the water's edge
	}
	// It is animated: the frames differ (the lace moves, the front goes out and back), neighbours alike, and found by name.
	{
		SrcImage f0, f1, f4, bad;
		CHECK(makeFoamFrame("g3_foam_a0.tga", f0) && makeFoamFrame("tex/G3_FOAM_A1.TGA", f1) && makeFoamFrame("G3_FOAM_A4.TGA", f4));
		CHECK(!makeFoamFrame("G3_FOAM_A8.TGA", bad) && !makeFoamFrame("G3_FOAM.TGA", bad) && !makeFoamFrame("WATER_A1.TGA", bad));
		auto diff = [](const SrcImage& a, const SrcImage& b) {
			double d = 0.0;
			for (size_t i = 3; i < a.rgba.size(); i += 4) d += std::fabs(double(a.rgba[i]) - double(b.rgba[i]));
			return d / double(a.rgba.size() / 4);
		};
		CHECK(f0.width == f1.width && f0.height == f1.height);
		CHECK(diff(f0, f1) > 0.5 && diff(f0, f4) > diff(f0, f1)); // (moves, and further the further in the loop)
		CHECK(diff(f0, f1) < 40.0);                                // (but not a different picture each time)
	}
}

static void testWeather() {
	// Clear: nothing changes. Rain: after a while it is raining, the sky greys, the light dims, the ground wets.
	const SkyColors day = skyColorsAt(12.0f);
	Weather w;
	w.configure(WEATHER_CLEAR, true);
	for (int i = 0; i < 200; ++i) w.update(0.1f);
	SkyColors c = day;
	w.apply(c);
	CHECK(c.zenith[2] == day.zenith[2] && c.light[0] == day.light[0] && w.fogScale() == 1.0f && !weatherRaining());
	w.configure(WEATHER_RAIN, true);
	w.update(0.1f);
	CHECK(w.rain() > 0.0f && w.rain() < 0.1f); // not at once
	for (int i = 0; i < 400; ++i) w.update(0.1f);
	CHECK(w.rain() == 0.6f && w.raining() && weatherRaining() && w.wet() > 0.5f);
	c = day;
	w.apply(c);
	CHECK(c.light[0] < day.light[0] * 0.85f && c.zenith[2] < day.zenith[2] && w.fogScale() < 0.85f);
	// Greyer: the zenith's blue lead over red shrinks.
	CHECK(int(c.zenith[2]) - int(c.zenith[0]) < int(day.zenith[2]) - int(day.zenith[0]));
	// Wet ground off: the same sky, brighter light.
	Weather dry;
	dry.configure(WEATHER_RAIN, false);
	for (int i = 0; i < 600; ++i) dry.update(0.1f);
	SkyColors d = day;
	dry.apply(d);
	CHECK(d.light[0] > c.light[0] && dry.wet() > 0.5f);
	// It dries slowly after the rain.
	w.configure(WEATHER_CLEAR, true);
	for (int i = 0; i < 300; ++i) w.update(0.1f);
	CHECK(w.rain() == 0.0f && !weatherRaining() && w.wet() > 0.3f);
	for (int i = 0; i < 2000; ++i) w.update(0.1f);
	CHECK(w.wet() == 0.0f);
	// Storm: flashes sometimes; snow: streaks, no wet ground.
	Weather st;
	st.configure(WEATHER_STORM, true);
	float peak = 0;
	for (int i = 0; i < 3000; ++i) {
		st.update(0.1f);
		if (st.flash() > peak) peak = st.flash();
	}
	CHECK(peak > 0.9f && st.rain() == 1.0f);
	Weather sn;
	sn.configure(WEATHER_SNOW, true);
	for (int i = 0; i < 400; ++i) sn.update(0.1f);
	WeatherStreak buf[240];
	const int ns = sn.streaks(3.0, buf, 240, 400, 240);
	CHECK(ns > 50 && sn.wet() == 0.0f && sn.rain() == 0.0f);
	for (int i = 0; i < ns; ++i) CHECK(buf[i].x >= -8 && buf[i].x < 410 && buf[i].y >= -20 && buf[i].y <= 240);
	CHECK(st.streaks(1.0, buf, 240, 400, 240) > 100 && sn.streaks(1.0, buf, 10, 400, 240) == 10);
	// Automatic: over a long time it visits rain and clear.
	Weather au;
	au.configure(WEATHER_AUTO, true);
	bool sawRain = false, sawClear = false;
	for (int i = 0; i < 36000; ++i) {
		au.update(0.5f);
		sawRain |= au.kind() == WEATHER_RAIN || au.kind() == WEATHER_STORM;
		sawClear |= au.kind() == WEATHER_CLEAR;
	}
	CHECK(sawRain && sawClear);
	// fix187: the game's schedule (Inside-Gothic). Every day one rain of 1 .. 2.5 hours, at any hour; the first between 16:30
	// and 17:30; no lightning on the first three days, then about 40 % of the rains.
	{
		const RainEvent first = Weather::rainOfDay(0);
		CHECK(first.start >= 16.5f && first.start <= 17.5f && !first.lightning);
		int lightning = 0, late = 0, early = 0;
		bool lengths = true, sameEachTime = true, firstDays = true;
		for (int d = 0; d < 400; ++d) {
			const RainEvent e = Weather::rainOfDay(d), again = Weather::rainOfDay(d);
			sameEachTime = sameEachTime && e.start == again.start && e.stop == again.stop && e.lightning == again.lightning;
			lengths = lengths && e.stop - e.start >= 1.0f && e.stop - e.start <= 2.5f && e.start >= 0.0f && e.start < 24.0f;
			if (d < 3) firstDays = firstDays && !e.lightning;
			else lightning += e.lightning;
			late += e.start >= 20.0f;
			early += e.start < 6.0f;
		}
		const float share = float(lightning) / 397.0f;
		std::printf("  rain schedule: first rain %.2f-%.2f h; lightning in %.0f%% of the rains from day 4; %d start at night (before 6), %d after 20\n",
		            double(first.start), double(first.stop), double(share * 100.0f), early, late);
		CHECK(sameEachTime && lengths && firstDays && share > 0.32f && share < 0.48f && early > 50 && late > 50);
		// The amounts over the first day: nothing in the morning, full rain in the middle of it, clouds before it starts.
		float r, c;
		bool l;
		Weather::scheduleAt(0, 10.0f, r, c, l);
		CHECK(r == 0.0f && c == 0.0f);
		Weather::scheduleAt(0, (first.start + first.stop) * 0.5f, r, c, l);
		CHECK(r == 1.0f && c == 1.0f && !l);
		Weather::scheduleAt(0, first.start - 0.3f, r, c, l);
		CHECK(r == 0.0f && c > 0.2f);
		// A rain that goes on past midnight is still there the next day.
		for (int d = 3; d < 400; ++d) {
			const RainEvent e = Weather::rainOfDay(d);
			if (e.stop <= 24.5f) continue;
			Weather::scheduleAt(d + 1, (e.stop - 24.0f) * 0.5f, r, c, l);
			CHECK(r > 0.5f && l == e.lightning);
			break;
		}
		// The weather in automatic mode follows it on the game's clock: the first day clear at noon, raining in the evening.
		Weather g;
		g.configure(WEATHER_AUTO, true);
		g.setClock(0, 12.0f);
		for (int i = 0; i < 400; ++i) g.update(0.1f);
		CHECK(g.kind() == WEATHER_CLEAR && g.rain() == 0.0f);
		g.setClock(0, (first.start + first.stop) * 0.5f);
		for (int i = 0; i < 400; ++i) g.update(0.1f);
		CHECK(g.kind() == WEATHER_RAIN && g.rain() > 0.55f && g.raining());
		// A day with lightning: a storm with flashes.
		int stormDay = -1;
		for (int d = 3; d < 60 && stormDay < 0; ++d)
			if (Weather::rainOfDay(d).lightning) stormDay = d;
		CHECK(stormDay >= 3);
		const RainEvent se = Weather::rainOfDay(stormDay);
		float peak = 0.0f;
		for (int i = 0; i < 3000; ++i) {
			g.setClock((se.start + se.stop) * 0.5f >= 24.0f ? stormDay + 1 : stormDay, std::fmod((se.start + se.stop) * 0.5f, 24.0f));
			g.update(0.1f);
			peak = std::max(peak, g.flash());
		}
		CHECK(g.kind() == WEATHER_STORM && peak > 0.9f);
	}
	// Puddles: the same cell always gives the same puddle, about a third of the cells have one, small and flat.
	{
		int have = 0, total = 0;
		for (int iz = -20; iz < 20; ++iz)
			for (int ix = -20; ix < 20; ++ix) {
				float x, z, rx, rz, a, th, x2, z2;
				const bool p = Puddles::cellPuddle(ix, iz, x, z, rx, rz, a, th);
				CHECK(p == Puddles::cellPuddle(ix, iz, x2, z2, rx, rz, a, th));
				++total;
				if (!p) continue;
				++have;
				CHECK(x >= float(ix) * Puddles::CELL && x < float(ix + 1) * Puddles::CELL && z >= float(iz) * Puddles::CELL && z < float(iz + 1) * Puddles::CELL);
				CHECK(rx >= 40.0f && rx < 400.0f && rz >= 40.0f && th > 0.0f && th < 0.6f);
			}
		CHECK(have > total / 5 && have < total / 2);
	}
	// Rain: a cell's drop is the same every time, inside its cell, and fewer cells have one in light rain.
	{
		int heavy = 0, light = 0;
		for (int i = 0; i < 400; ++i) {
			float x, z, ph, sp, x2, z2;
			const int ix = i % 20, iz = i / 20;
			const bool a = Rain::cellDrop(ix, iz, 1.0f, x, z, ph, sp);
			CHECK(a == Rain::cellDrop(ix, iz, 1.0f, x2, z2, ph, sp) && !Rain::cellDrop(ix, iz, 0.0f, x2, z2, ph, sp));
			if (a) CHECK(x >= float(ix) * Rain::CELL && x < float(ix + 1) * Rain::CELL && z >= float(iz) * Rain::CELL && z < float(iz + 1) * Rain::CELL && ph >= 0 && ph < 1 && sp >= 0.9f && sp <= 1.1f);
			heavy += a;
			light += Rain::cellDrop(ix, iz, 0.3f, x2, z2, ph, sp);
		}
		CHECK(heavy > 250 && heavy < 350 && light > 50 && light < heavy / 2); // (fix187: denser - 0.75 of the cells at full rain)
	}
	// The waves under a ring: height and slope as the shader's (the slope is the height's derivative).
	{
		float l0, sx0, sz0, l1, sx1, sz1;
		const float x = 1234.0f, z = -567.0f, e = 0.5f;
		gfx::waveSurfaceAt(x, z, 10.0, 22.0f, 1.0f, l0, sx0, sz0);
		gfx::waveSurfaceAt(x + e, z, 10.0, 22.0f, 1.0f, l1, sx1, sz1);
		CHECK(std::fabs((l1 - l0) / e - sx0) < 0.02f && std::fabs(l0) <= 22.0f * 1.0f + 0.01f);
		gfx::waveSurfaceAt(x, z + e, 10.0, 22.0f, 1.0f, l1, sx1, sz1);
		CHECK(std::fabs((l1 - l0) / e - sz0) < 0.02f);
		gfx::waveSurfaceAt(x, z, 10.0, 22.0f, 0.0f, l1, sx1, sz1);
		CHECK(l1 == 0.0f && sx1 == 0.0f && sz1 == 0.0f); // (weight 0: still water)
	}
	// The made water texture tiles: the step across the seam is no bigger than a step inside.
	{
		const unsigned char col[3] = {92, 140, 156};
		const SrcImage w = makeWaterTexture(col, 64);
		CHECK(w.width == 64 && w.rgba.size() == 64u * 64u * 4u);
		int inside = 0, seam = 0;
		for (int y = 0; y < 64; ++y)
			for (int k = 0; k < 4; ++k) {
				inside = std::max(inside, std::abs(int(w.rgba[size_t(y * 64 + 31) * 4 + size_t(k)]) - int(w.rgba[size_t(y * 64 + 32) * 4 + size_t(k)])));
				seam = std::max(seam, std::abs(int(w.rgba[size_t(y * 64) * 4 + size_t(k)]) - int(w.rgba[size_t(y * 64 + 63) * 4 + size_t(k)])));
				seam = std::max(seam, std::abs(int(w.rgba[size_t(y) * 4 + size_t(k)]) - int(w.rgba[size_t(63 * 64 + y) * 4 + size_t(k)])));
			}
		CHECK(seam <= inside + 12 && w.rgba[3] >= 100 && w.rgba[3] < 200);
	}
	Settings s;
	s.weather = 9;
	s.wetGround = 7;
	s.sanitize();
	CHECK(s.weather == 0 && s.wetGround == 1);
}

static void testSimplify() {
	testSimplifyWorld();
	testSimplifySteepLight();
	testSimplifyModel();
	testSharedMeshes();
}

static void testFarLayer() {
	SrcScene scene;
	scene.materials.resize(2);
	scene.materials[0].name = "grass";
	scene.materials[0].hasColor = true;
	scene.materials[0].color[0] = 40;
	scene.materials[0].color[1] = 160;
	scene.materials[0].color[2] = 40;
	scene.materials[1].name = "tower";
	scene.materials[1].hasColor = true;
	scene.materials[1].color[0] = 220;
	scene.materials[1].color[1] = 30;
	scene.materials[1].color[2] = 30;
	auto height = [](float x, float z) { return 300.0f * std::sin(x / 3000.0f) * std::cos(z / 2500.0f); };
	const float step = 400.0f, half = 16000.0f;
	for (float x = -half; x < half; x += step)
		for (float z = -half; z < half; z += step) {
			const float a[3] = {x, height(x, z + step), z + step}, b[3] = {x + step, height(x + step, z + step), z + step};
			const float c[3] = {x + step, height(x + step, z), z}, d[3] = {x, height(x, z), z};
			addQuad(scene, d, c, b, a, 0); // (facing up: see the bake's winding note)
		}
	// A red tower 140 m away, seen from either side.
	{
		const float x0 = -500, x1 = 500, z0 = 13500, z1 = 14500, y0 = -300, y1 = 3000;
		const float k[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}, {x0, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y1, z1}};
		const int faces[5][4] = {{4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
		for (const auto& f : faces) {
			addQuad(scene, k[f[0]], k[f[1]], k[f[2]], k[f[3]], 1);
			addQuad(scene, k[f[3]], k[f[2]], k[f[1]], k[f[0]], 1);
		}
	}
	// The tower is shut in (ambient occlusion 200 of 255) as a baked world would have it.
	for (SrcTriangle& t : scene.triangles)
		if (t.material == 1)
			for (SrcVertex& v : t.v) v.occlusion = 200;
	scene.spawnPos[1] = 400.0f;
	scene.hasSpawn = true;
	const u32 input = u32(scene.triangles.size());

	// ---- the baker ----
	std::vector<BakeWorld> worlds(1);
	worlds[0].name = "far";
	worlds[0].scene = scene;
	BakeOptions opt;
	opt.chunker.chunkSize = 4000.0f;
	opt.tex.maxSize = 32;
	opt.tex.quality = 0;
	opt.threads = 2;
	opt.quiet = true;
	opt.far.enabled = true;
	opt.far.maxTriangles = 3000;
	PakWriter pakw;
	bakeWorlds(worlds, opt, pakw);
	const std::string path = gDir + "/far.pak";
	pakw.write(path);
	Pak pak;
	CHECK(pak.open(path.c_str()));
	const PakEntry* we = pak.find("world/far");
	CHECK(we != nullptr);
	if (!we) return;
	std::vector<u8> index(we->size);
	CHECK(pak.read(*we, index.data()));
	WorldHeader wh;
	std::memcpy(&wh, index.data(), sizeof(wh));
	const PakEntry* fe = pak.find(wh.farEntry);
	CHECK(wh.farEntry == hashName("world/far/far") && fe != nullptr);
	if (!fe) return;
	std::vector<u8> blob(fe->size);
	CHECK(pak.read(*fe, blob.data()));
	FarHeader fh;
	std::memcpy(&fh, blob.data(), sizeof(fh));
	CHECK(fh.magic == FAR_MAGIC && fh.version == FAR_VERSION && blob.size() == farBlobSize(fh));
	u32 stripTris = 0, cells = 0, seams = 0;
	{
		const FarRange* ranges = reinterpret_cast<const FarRange*>(blob.data() + sizeof(FarHeader) + fh.tileCount * sizeof(FarTile));
		for (u32 i = 0; i < fh.rangeCount; ++i) {
			cells += !ranges[i].seam;
			seams += ranges[i].seam;
			if (ranges[i].seam) stripTris += ranges[i].indexCount / 3;
		}
	}
	const u32 farTris = fh.indexCount / 3 - stripTris;
	std::printf("  far layer: %u triangles of %u (and %u in %u seams), grid %.1f m, %u tiles, %u cells, %u bytes\n", farTris, input,
	            stripTris, seams, double(fh.cellSize) / 100.0, fh.tileCount, cells, u32(blob.size()));
	CHECK(farTris <= 3000 && farTris > 200 && fh.cellSize > 400.0f && cells == 64 && seams >= 100 && stripTris < farTris);
	{
		const u32* refs = reinterpret_cast<const u32*>(blob.data() + sizeof(FarHeader) + fh.tileCount * sizeof(FarTile) +
		                                               fh.rangeCount * sizeof(FarRange));
		bool refsOk = true;
		for (u32 i = 0; i < fh.chunkRefCount; ++i) refsOk = refsOk && refs[i] < wh.chunkCount;
		CHECK(refsOk);
	}
	// The far layer carries the occlusion in the vertices' w, as the chunks do (and none where there is none).
	{
		const WorldVertex* verts = reinterpret_cast<const WorldVertex*>(blob.data() + sizeof(FarHeader) + fh.tileCount * sizeof(FarTile) +
		                                                              fh.rangeCount * sizeof(FarRange) + fh.chunkRefCount * sizeof(u32));
		u32 shut = 0, open = 0;
		for (u32 i = 0; i < fh.vertexCount; ++i) {
			shut += verts[i].w >= 150;
			open += verts[i].w == 0;
		}
		CHECK(shut >= 4 && open > shut);
	}
	// Without the option there is none, and the rest of the archive is as it was.
	{
		BakeOptions plain = opt;
		plain.far.enabled = false;
		std::vector<BakeWorld> w2(1);
		w2[0].name = "far";
		w2[0].scene = scene;
		PakWriter p2;
		bakeWorlds(w2, plain, p2);
		const std::string path2 = gDir + "/far_off.pak";
		p2.write(path2);
		Pak q;
		CHECK(q.open(path2.c_str()) && q.find("world/far/far") == nullptr);
	}

	// ---- the runtime ----
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), "far"));
	World& w = app.world();
	CHECK(w.hasFarLayer());
	app.player().flying = true;
	app.setThirdPerson(false);
	auto look = [&](float farM, Vec3 at, float yaw, float pitch, u32& red, u32& sky) {
		w.config().farDistance = farM * 100.0f;
		app.setDrawDistance(6000.0f);
		app.camera().yaw = yaw;
		app.camera().pitch = pitch;
		app.teleport(at);
		w.preload(app.camera().pos);
		for (int i = 0; i < 3; ++i) {
			app.update(Input {}, 1.0f / 30.0f);
			app.camera().yaw = yaw;
			app.camera().pitch = pitch;
			app.render();
		}
		const u8* fb = gfx::softFramebuffer();
		if (getenv("FAR_DEBUG")) {
			static int shot = 0;
			char name[64];
			std::snprintf(name, sizeof(name), "far_%d.ppm", shot++);
			if (FILE* f = std::fopen(name, "wb")) {
				std::fprintf(f, "P6 %d %d 255\n", gfx::SCREEN_W, gfx::SCREEN_H);
				for (int i = 0; i < gfx::SCREEN_W * gfx::SCREEN_H; ++i) std::fwrite(fb + i * 4, 1, 3, f);
				std::fclose(f);
			}
			std::printf("    camera %.0f %.0f %.0f pitch %.2f: %u chunks drawn of %u loaded, %u triangles, far %u\n", double(app.camera().pos.x),
			            double(app.camera().pos.y), double(app.camera().pos.z), double(app.camera().pitch), w.stats().chunksDrawn,
			            w.stats().chunksLoaded, w.stats().trianglesDrawn, w.stats().farTriangles);
		}
		red = sky = 0;
		for (int y = 0; y < gfx::SCREEN_H; ++y)
			for (int x = 0; x < gfx::SCREEN_W; ++x) {
				const u8* px = fb + (y * gfx::SCREEN_W + x) * 4;
				red += px[0] > 120 && px[1] < 90 && px[2] < 90;
				// Below the middle of a view down on to the ground: blue is the sky through a hole.
				sky += y > gfx::SCREEN_H / 3 && px[2] > px[1] + 4;
			}
	};
	u32 redNear, skyNear, redFar, skyFar, redDown, skyDown, redDownFar, skyDownFar;
	// Towards the tower, 140 m off: past the draw distance only the far layer has it.
	look(0.0f, {0, 400, 0}, 0.0f, 0.0f, redNear, skyNear);
	CHECK(!w.farActive() && w.stats().farBytes == 0);
	look(300.0f, {0, 400, 0}, 0.0f, 0.0f, redFar, skyFar);
	const WorldStats fs = w.stats();
	CHECK(w.farActive() && fs.farBytes > 0 && fs.farTriangles > 0 && fs.farDrawCalls > 0);
	// From high up, steeply down: the chunks drawn in full in the middle, the far layer round them.
	look(0.0f, {0, 2500, -3000}, 0.0f, -0.45f, redDown, skyDown);
	look(300.0f, {0, 2500, -3000}, 0.0f, -0.45f, redDownFar, skyDownFar);
	const u32 farTrisDown = w.stats().farTriangles;
	std::printf("  far layer: tower %u pixels without it, %u with; looking down, %u sky pixels below the horizon without it, %u with "
	            "(%u far triangles)\n",
	            redNear, redFar, skyDown, skyDownFar, farTrisDown);
	CHECK(redNear == 0 && redFar > 40);
	CHECK(skyDown > 2000 && skyDownFar < 10 && w.stats().farSeams > 0);
	// Chunks at the draw distance dissolve out over the far layer instead of ending in a seam; off, none do.
	{
		bool anyEdge = false;
		for (float z : {-3000.0f, -1500.0f, 0.0f, 1500.0f, 3000.0f}) {
			look(300.0f, {0, 400, z}, 0.0f, -0.2f, redFar, skyFar);
			anyEdge = anyEdge || w.stats().chunksEdgeFading > 0;
		}
		CHECK(anyEdge);
		w.config().edgeFadeShare = 0.0f;
		look(300.0f, {0, 400, 0}, 0.0f, -0.2f, redFar, skyFar);
		CHECK(w.stats().chunksEdgeFading == 0);
		w.config().edgeFadeShare = 0.3f;
	}
	// Farther draw distance: more of the world in full, less of the far layer.
	w.config().farDistance = 30000.0f;
	app.setDrawDistance(12000.0f);
	w.preload(app.camera().pos);
	for (int i = 0; i < 3; ++i) {
		app.update(Input {}, 1.0f / 30.0f);
		app.render();
	}
	CHECK(w.stats().farTriangles < farTrisDown);
	// Switched off: freed.
	w.config().farDistance = 0.0f;
	app.update(Input {}, 1.0f / 30.0f);
	CHECK(!w.farActive() && w.stats().farBytes == 0);
	// The menu's values: the sets switch it on, sanitize keeps it in range.
	{
		Settings s;
		s.applyGfxPreset(GFX_PLUS_BALANCED);
		CHECK(s.farLayer == 1 && s.farCm() == 25000.0f);
		s.applyGfxPreset(GFX_PLUS_MAX);
		CHECK(s.farLayer == 2 && s.gfxPresetOf() == GFX_PLUS_MAX);
		s.farLayer = 9;
		s.sanitize();
		CHECK(s.farLayer == 0);
	}
	app.shutdown();
	CHECK(gfx::memStats().meshCount == 0);
}

// Settings (settings.bin) and the pause menu.
static void testSettings() {
	const std::string path = gDir + "/settings.bin";
	{
		Settings s;
		s.drawMetres = 170;
		s.objects = OBJECTS_STEADY;
		s.frameRate = FRAMES_30;
		s.cameraSpeed = 9;
		s.invertY = 1;
		s.immortal = 1;
		CHECK(s.save(path.c_str()));
		Settings t;
		CHECK(t.load(path.c_str()) && std::memcmp(&s, &t, sizeof(s)) == 0);
		CHECK(t.drawDistanceCm() == 17000.0f && t.minDetail() > 0.99f);
		// Out of range: clamped. A broken file: the defaults stay.
		Settings u;
		u.drawMetres = 5;
		u.cameraSpeed = 200;
		u.objects = 77;
		u.sanitize();
		CHECK(u.drawMetres >= 40 && u.cameraSpeed <= 10 && u.objects < 3);
		FILE* f = std::fopen(path.c_str(), "r+b");
		CHECK(f != nullptr);
		if (f) {
			std::fseek(f, 14, SEEK_SET);
			std::fputc(0x5A, f);
			std::fclose(f);
		}
		Settings v;
		CHECK(!v.load(path.c_str()) && v.drawMetres == Settings().drawMetres && v.cameraSpeed == Settings().cameraSpeed);
		CHECK(!v.load((gDir + "/no_such_settings.bin").c_str()));
	}
	{
		Menu m;
		Settings s;
		m.open();
		CHECK(m.isOpen() && m.page() == Menu::PAGE_MAIN && m.selected() == 0);
		MenuInput down, up, right, left, ok, back, start;
		down.down = true;
		up.up = true;
		right.right = true;
		left.left = true;
		ok.confirm = true;
		back.back = true;
		start.close = true;
		m.update(down, s);
		CHECK(m.selected() == 1 && std::strcmp(m.label(Menu::PAGE_MAIN, 1), "Графика") == 0);
		CHECK(m.update(ok, s) == MENU_NONE && m.page() == Menu::PAGE_GRAPHICS);
		CHECK(m.update(right, s) == MENU_CHANGED && s.drawMetres == 130);
		CHECK(m.update(left, s) == MENU_CHANGED && m.update(left, s) == MENU_CHANGED && s.drawMetres == 110);
		char value[32];
		m.value(Menu::PAGE_GRAPHICS, 0, s, value, sizeof(value));
		CHECK(std::strcmp(value, "110 м") == 0);
		m.update(down, s);
		CHECK(m.update(right, s) == MENU_CHANGED && s.objects == OBJECTS_BALANCED);
		CHECK(m.update(back, s) == MENU_NONE && m.page() == Menu::PAGE_MAIN && m.selected() == 1);
		m.update(up, s);
		m.update(up, s); // (round to the last)
		CHECK(m.selected() == m.itemCount(Menu::PAGE_MAIN) - 1 && m.update(ok, s) == MENU_EXIT);
		CHECK(m.update(start, s) == MENU_CLOSED && !m.isOpen());
		// The debug page leads to three pages of its own; B goes back one step at a time, and the kit is on one of them.
		{
			Menu d;
			d.open();
			MenuInput none, ok2, back2, dn;
			ok2.confirm = true;
			back2.back = true;
			dn.down = true;
			for (int i = 0; i < 6; ++i) d.update(dn, s);
			CHECK(d.update(ok2, s) == MENU_NONE && d.page() == Menu::PAGE_DEBUG);
			d.update(dn, s);
			CHECK(d.update(ok2, s) == MENU_NONE && d.page() == Menu::PAGE_DEBUG_HERO);
			bool kit = false;
			for (int i = 0; i < d.itemCount(Menu::PAGE_DEBUG_HERO); ++i) {
				if (i) d.update(dn, s);
				kit = kit || d.update(ok2, s) == MENU_GIVE_TESTKIT;
				// (the page is long: it scrolls, up to 12 lines)
			}
			CHECK(kit);
			CHECK(d.update(back2, s) == MENU_NONE && d.page() == Menu::PAGE_DEBUG);
			CHECK(d.update(back2, s) == MENU_NONE && d.page() == Menu::PAGE_MAIN);
			for (int p = 0; p < Menu::PAGES; ++p) CHECK(d.itemCount(Menu::Page(p)) <= 12); // (the list scrolls)
			(void)none;
		}
		// Every line fits the panel (320 pixels, 8 of margin either side).
		Ui ui;
		int widest = 0;
		for (int p = 0; p < Menu::PAGES; ++p)
			for (int i = 0; i < m.itemCount(Menu::Page(p)); ++i) {
				m.value(Menu::Page(p), i, s, value, sizeof(value));
				widest = std::max({widest, ui.measure(m.label(Menu::Page(p), i)) + ui.measure(value) + 40, ui.measure(m.note(Menu::Page(p), i))});
			}
		CHECK(widest <= 304);
		CHECK(ui.measure("Вы погибли", ui::TITLE) > ui.measure("Вы погибли", ui::TEXT));
	}
	// The game's own font: its glyphs come before the built-in ones, which stand in for what it lacks;
	// damaged data is refused and the built-in font stays.
	{
		FontHeader h {};
		h.magic = FONT_MAGIC;
		h.version = FONT_VERSION;
		h.atlasW = u16(FONT_ATLAS_W);
		h.atlasH = 16;
		h.glyphCount = 2;
		h.line[0] = 10;
		h.line[1] = 12;
		h.ascent[0] = 8;
		h.ascent[1] = 10;
		FontGlyphRec a0 {}, a1 {};
		a0.code = a1.code = 'A';
		a0.x = 1; a0.y = 1; a0.size = 0; a0.w = 5; a0.h = 7; a0.advance = 6;
		a1.x = 8; a1.y = 1; a1.size = 1; a1.w = 5; a1.h = 9; a1.advance = 7;
		std::vector<u8> blob(sizeof(h) + 2 * sizeof(FontGlyphRec) + size_t(FONT_ATLAS_W) * h.atlasH, 0);
		std::memcpy(blob.data(), &h, sizeof(h));
		std::memcpy(blob.data() + sizeof(h), &a0, sizeof(a0));
		std::memcpy(blob.data() + sizeof(h) + sizeof(a0), &a1, sizeof(a1));
		blob[sizeof(h) + 2 * sizeof(FontGlyphRec) + 1 * FONT_ATLAS_W + 1] = 255; // a texel of the first glyph
		ui::GlyphBox before, g;
		CHECK(ui::glyph('A', ui::TEXT, before) && before.y < 256);
		CHECK(!ui::setGameFont(blob.data(), u32(blob.size() - 1)));
		CHECK(ui::glyph('A', ui::TEXT, g) && g.y == before.y && g.advance == before.advance);
		CHECK(ui::setGameFont(blob.data(), u32(blob.size())));
		CHECK(ui::glyph('A', ui::TEXT, g) && g.advance == 6 && g.w == 5 && g.y >= 512);
		CHECK(ui::glyph('A', ui::TITLE, g) && g.advance == 7 && g.h == 9);
		CHECK(ui::coverage(1, 512 + 1) == 15 && ui::coverage(2, 512 + 1) == 0);
		CHECK(ui::glyph('B', ui::TEXT, g) && g.y < 256); // (not in the game's font: the built-in glyph)
		CHECK(Ui::lineHeight(ui::TEXT) == 12 && Ui::lineHeight(ui::TITLE) == 14);
		ui::clearGameFont();
		CHECK(ui::glyph('A', ui::TEXT, g) && g.y == before.y && Ui::lineHeight() == uifont::LINE[0]);

		// The baker: two sheets of 16x16 cells (byte b at column b % 16, row b / 16), a Russian code page.
		SrcFonts fonts;
		fonts.codepage = 1251;
		for (int s = 0; s < 2; ++s) {
			SrcFontFace& f = fonts.face[s];
			f.name = s ? "BIG" : "SMALL";
			f.height = s ? 14 : 8;
			const int cell = f.height + 2;
			f.sheet.width = f.sheet.height = 256;
			f.sheet.rgba.assign(size_t(256) * 256 * 4, 0);
			for (int b = 0; b < 256; ++b) {
				SrcFontFace::Cell c;
				c.width = 6 + s * 3;
				const int cx = (b % 16) * 16, cy = (b / 16) * 16;
				c.u0 = float(cx) / 256.0f;
				c.v0 = float(cy) / 256.0f;
				c.u1 = float(cx + c.width) / 256.0f;
				c.v1 = float(cy + cell) / 256.0f;
				f.cells.push_back(c);
				if (b == 0x20) continue; // (a space: nothing in its cell)
				for (int y = 0; y < f.height; ++y)
					for (int x = 0; x < c.width - 1; ++x) f.sheet.rgba[(size_t(cy + y) * 256 + cx + x) * 4 + 3] = 255;
			}
		}
		std::vector<u8> baked;
		std::string report, why;
		CHECK(bakeFont(fonts, baked, report, why) && why.empty());
		CHECK(ui::setGameFont(baked.data(), u32(baked.size())));
		ui::GlyphBox ya, space;
		CHECK(ui::glyph(0x42F, ui::TEXT, ya) && ya.y >= 512 && ya.advance == 6 && ya.w == 6 && ya.h == 10); // byte 0xDF is Я in 1251: not 'ß'
		CHECK(ui::glyph(0x42F, ui::TITLE, g) && g.advance == 9 && g.h == 16);
		CHECK(ui::glyph('A', ui::TEXT, g) && g.y >= 512 && ui::coverage(g.x, g.y) == 15);
		CHECK(ui::glyph(' ', ui::TEXT, space) && space.advance == 6 && space.w == 0);
		CHECK(ui::glyph(0x25BC, ui::TEXT, g) && g.y < 256); // (the marker is not in the game's fonts: the built-in one)
		CHECK(std::string(report).find("code page 1251") != std::string::npos);
		ui::clearGameFont();
		// (A page where the same byte is another letter.)
		fonts.codepage = 1252;
		CHECK(bakeFont(fonts, baked, report, why) && ui::setGameFont(baked.data(), u32(baked.size())));
		CHECK(ui::glyph(0xE4, ui::TEXT, g) && g.y >= 512);
		ui::clearGameFont();
		CHECK(detectCodePage({std::string(300, '\xE0')}) == 1251 && detectCodePage({std::string(300, 'a')}) == 1252);
	}
	// Languages: every text in all four, the same %-conversions as the key's,
	// every letter in the font, every line of the menu fits the panel.
	{
		auto conversions = [](const char* t) {
			std::string out;
			for (; *t; ++t)
				if (*t == '%') {
					out += *t;
					while (t[1] && !std::strchr("sdlufx%", t[1])) ++t;
					while (t[1] == 'l' || t[1] == 's' || t[1] == 'd' || t[1] == 'u' || t[1] == 'f' || t[1] == 'x') out += *++t;
					if (t[1] == '%') ++t;
				}
			return out;
		};
		bool complete = true, same = true, letters = true;
		for (u32 i = 0; i < langEntries(); ++i)
			for (u8 l = 0; l < LANG_COUNT; ++l) {
				const char* t = langText(i, l);
				complete = complete && t && *t;
				same = same && t && conversions(t) == conversions(langKey(i));
				for (const char* p = t; t && *p;) {
					const u32 c = utf8Next(p);
					bool found = c < 128;
					for (const uifont::Glyph& g : uifont::kGlyphs) found = found || g.code == c;
					letters = letters && found;
				}
			}
		CHECK(langEntries() > 200 && complete && same && letters);
		Ui ui;
		Menu m;
		Settings s;
		char value[48];
		const u32 missesBefore = langMisses();
		for (u8 l = 0; l < LANG_COUNT; ++l) {
			setLanguage(l);
			int widest = 0;
			for (int p = 0; p < Menu::PAGES; ++p)
				for (int i = 0; i < m.itemCount(Menu::Page(p)); ++i) {
					m.value(Menu::Page(p), i, s, value, sizeof(value));
					const int line = ui.measure(m.label(Menu::Page(p), i)) + ui.measure(value) + 40;
					widest = std::max({widest, line, ui.measure(m.note(Menu::Page(p), i))});
					if (std::getenv("G3_TRACE") && (line > 304 || ui.measure(m.note(Menu::Page(p), i)) > 304))
						std::printf("  lang %d wide: %s / %s / %s\n", int(l), m.label(Menu::Page(p), i), value, m.note(Menu::Page(p), i));
				}
			CHECK(widest <= 304);
			CHECK(l == LANG_RU ? std::strcmp(tr("Сила"), "Сила") == 0 : std::strcmp(tr("Сила"), "Сила") != 0);
		}
		setLanguage(LANG_DE);
		CHECK(std::strcmp(tr("Сила"), "Stärke") == 0 && std::strcmp(tr("Нет такого текста"), "Нет такого текста") == 0);
		setLanguage(LANG_RU);
		// (The menu's own texts are all in the table; the one asked for above is the single miss.)
		CHECK(langMisses() == missesBefore + 1);
	}
}


// The saved games: ten slots and the quick one, as the original's - the header the menu reads (name, world, hour,
// chapter, when, time played, picture), the files of before the header, the menu's pages, the progress screen.
static void writePpm(const std::string& path) {
	FILE* f = fopen(path.c_str(), "wb");
	if (!f) return;
	std::fprintf(f, "P6\n%d %d\n255\n", gfx::SCREEN_W, gfx::SCREEN_H);
	const u8* fb = gfx::softFramebuffer();
	for (int i = 0; i < gfx::SCREEN_W * gfx::SCREEN_H; ++i) fwrite(fb + i * 4, 3, 1, f);
	fclose(f);
}

static void testSaves() {
	// 1. The header on its own.
	{
		const std::string path = gDir + "/header.sav";
		SaveInfo in;
		copySaveTitle(in.title, sizeof(in.title), "Перед шахтой");
		std::snprintf(in.world, sizeof(in.world), "oldmine");
		in.day = 4;
		in.timeOfDay = 21.5f;
		in.chapter = 2;
		in.playSeconds = 2 * 3600 + 5 * 60 + 7;
		in.savedAt = 1792160000;
		static u16 thumb[THUMB_W * THUMB_H], back[THUMB_W * THUMB_H];
		for (int i = 0; i < THUMB_W * THUMB_H; ++i) thumb[i] = u16(i * 37);
		FILE* f = fopen(path.c_str(), "wb");
		CHECK(f && writeSaveHeader(f, in, thumb));
		fputs("POSITION", f);
		fclose(f);
		SaveInfo out;
		f = fopen(path.c_str(), "rb");
		CHECK(readSaveHeader(f, out, back));
		char rest[9] = {};
		CHECK(fread(rest, 8, 1, f) == 1 && std::strcmp(rest, "POSITION") == 0); // (it stands where the position begins)
		fclose(f);
		CHECK(out.used && out.readable && !out.legacy && out.picture && std::strcmp(out.title, "Перед шахтой") == 0 &&
		      std::strcmp(out.world, "oldmine") == 0 && out.day == 4 && out.chapter == 2 && out.playSeconds == in.playSeconds &&
		      out.savedAt == in.savedAt && std::fabs(out.timeOfDay - 21.5f) < 1e-5f);
		CHECK(std::memcmp(thumb, back, sizeof(thumb)) == 0);
		// Damaged: refused (and the file is not loaded).
		f = fopen(path.c_str(), "r+b");
		fseek(f, 20, SEEK_SET);
		fputc('X', f);
		fclose(f);
		CHECK(!readSaveInfo(path.c_str(), out, nullptr) && out.used && !out.readable);
		CHECK(!readSaveInfo((gDir + "/no-such.sav").c_str(), out, nullptr) && !out.used);
		// A file of before the header: legacy, read from its start.
		f = fopen(path.c_str(), "wb");
		fputs("G3STxxxx", f);
		fclose(f);
		CHECK(readSaveInfo(path.c_str(), out, nullptr) && out.legacy && out.readable && !out.picture);
		// The texts.
		setLanguage(LANG_RU);
		char buf[64];
		formatGameTime(buf, sizeof(buf), 4, 21.5f);
		CHECK(std::strcmp(buf, "День 5, 21:30") == 0);
		formatPlayTime(buf, sizeof(buf), in.playSeconds);
		CHECK(std::strcmp(buf, "2 ч 05 мин") == 0);
		formatPlayTime(buf, sizeof(buf), 59 * 60 + 59);
		CHECK(std::strcmp(buf, "59 мин") == 0);
		CHECK(std::strcmp(worldTitle("world/oldmine"), "Старая шахта") == 0 && std::strcmp(worldTitle("WORLD"), "Колония") == 0 &&
		      std::strcmp(worldTitle("cave"), "cave") == 0);
		defaultSaveTitle(buf, sizeof(buf), "world", 2);
		CHECK(std::strcmp(buf, "Колония, день 3") == 0);
		formatSaveDate(buf, sizeof(buf), 0);
		CHECK(buf[0] == 0);
		formatSaveDate(buf, sizeof(buf), in.savedAt);
		CHECK(std::strlen(buf) == 16 && buf[2] == '.' && buf[5] == '.' && buf[13] == ':');
		setLanguage(LANG_DE);
		formatGameTime(buf, sizeof(buf), 0, 6.0f);
		CHECK(std::strcmp(buf, "Tag 1, 06:00") == 0 && std::strcmp(worldTitle("world"), "Die Kolonie") == 0);
		setLanguage(LANG_RU);
		// A name is cut between letters, never inside one (Cyrillic: two bytes each).
		char cut[8];
		copySaveTitle(cut, sizeof(cut), "Шахта");
		CHECK(std::strcmp(cut, "Шах") == 0);
		// The picture shrinks by the mean.
		u16 small[2 * 1];
		shrinkPicture(4, 2, [](int x, int, u8* c) { c[0] = x < 2 ? 255 : 0; c[1] = 0; c[2] = x < 2 ? 0 : 255; }, small, 2, 1);
		CHECK(small[0] == 0xF800 && small[1] == 0x001F);
	}

	// 2. The game's slots.
	gfx::init();
	App app;
	CHECK(app.init((gDir + "/worlds.pak").c_str(), nullptr));
	const std::string statePath = gDir + "/saves-quick.bin";
	app.setStatePath(statePath.c_str());
	app.setSaveDir((gDir + "/saves").c_str());
	char path[256];
	for (int k = 0; k <= SAVE_SLOTS; ++k) {
		app.slotPath(k, path, sizeof(path));
		remove(path);
	}
	CHECK(std::string(path) == gDir + "/saves/slot10.sav");
	app.world().preload(app.camera().pos);
	const float dt = 1.0f / 30.0f;
	app.player().flying = true;
	app.teleport({0, 600, 0});
	app.camera().pitch = -0.4f;
	app.environment().timeOfDay = 15.0f;
	for (int i = 0; i < 3; ++i) {
		app.update(Input {}, dt);
		app.render();
	}
	app.setPlaySeconds(3725.0);
	// The menu opens: the picture of the moment is taken (the game, not the menu over it).
	app.menu().open();
	app.render();
	CHECK(app.savePicture() != nullptr);
	{
		const u16* pic = app.savePicture();
		int lit = 0;
		for (int i = 0; i < THUMB_W * THUMB_H; ++i) lit += pic[i] != 0;
		CHECK(lit > THUMB_W * THUMB_H / 2);
	}
	// The main page leads to the save page (item 7); it lists ten empty slots, the cursor on the first.
	MenuInput down, ok, back;
	down.down = true;
	ok.confirm = true;
	back.back = true;
	for (int i = 0; i < 7; ++i) app.menu().update(down, app.settings());
	CHECK(app.menu().update(ok, app.settings()) == MENU_NONE && app.menu().page() == Menu::PAGE_SAVE);
	app.render();
	CHECK(app.menu().saveSlot() == 1 && !app.slotInfo(1).used && !app.slotInfo(0).used);
	writePpm(gDir + "/saves-empty.ppm");
	app.menu().update(down, app.settings());
	app.menu().update(down, app.settings());
	CHECK(app.menu().update(ok, app.settings()) == MENU_SAVE_SLOT && app.menu().saveSlot() == 3);
	const u32 framesBefore = app.progressFrames();
	CHECK(app.menuAction(MENU_SAVE_SLOT, nullptr));
	CHECK(!app.menu().isOpen() && std::strcmp(app.toastText(), "Игра сохранена") == 0);
	CHECK(app.progressFrames() >= framesBefore + 4); // (the screen with the bar: a frame each step)
	writePpm(gDir + "/saves-progress.ppm");
	app.slotPath(3, path, sizeof(path));
	SaveInfo info;
	CHECK(readSaveInfo(path, info, nullptr) && info.picture && std::strcmp(info.world, "world") == 0 && info.playSeconds == 3725 &&
	      std::fabs(info.timeOfDay - 15.0f) < 1e-3f && info.savedAt > 0 && std::strcmp(info.title, "Колония, день 1") == 0);
	// A named one, into slot 1 (the 3DS asks for the name with its keyboard).
	app.menu().open();
	app.render();
	CHECK(app.saveSlot(1, "У ворот лагеря"));
	app.slotPath(1, path, sizeof(path));
	CHECK(readSaveInfo(path, info, nullptr) && std::strcmp(info.title, "У ворот лагеря") == 0);
	// A quick save (the frame on show is its picture).
	CHECK(app.saveState(statePath.c_str()) && readSaveInfo(statePath.c_str(), info, nullptr) && info.picture && !info.legacy);

	// Elsewhere, later: the load page brings the game back - the world, the place, the hour, the time played.
	const Vec3 there = app.camera().pos;
	CHECK(app.enterWorld(hashName("world/cave"), nullptr, 0));
	app.environment().timeOfDay = 3.0f;
	app.setPlaySeconds(9000.0);
	app.menu().open();
	app.render();
	for (int i = 0; i < 8; ++i) app.menu().update(down, app.settings());
	CHECK(app.menu().update(ok, app.settings()) == MENU_NONE && app.menu().page() == Menu::PAGE_LOAD);
	app.render();
	// (The cursor on the newest: the quick save, entry 0.)
	CHECK(app.menu().saveSlot() == 0 && app.slotInfo(0).used && app.slotInfo(1).used && app.slotInfo(3).used && !app.slotInfo(2).used);
	app.menu().update(down, app.settings());
	app.menu().update(down, app.settings());
	app.menu().update(down, app.settings());
	app.render();
	writePpm(gDir + "/saves-load.ppm");
	CHECK(app.menu().update(ok, app.settings()) == MENU_LOAD_SLOT && app.menu().saveSlot() == 3);
	CHECK(app.menuAction(MENU_LOAD_SLOT, nullptr));
	writePpm(gDir + "/saves-loading.ppm");
	CHECK(!app.menu().isOpen() && std::string(app.worldName()) == "world/world");
	CHECK(std::fabs(app.camera().pos.x - there.x) < 0.5f && std::fabs(app.camera().pos.z - there.z) < 0.5f);
	CHECK(std::fabs(app.environment().timeOfDay - 15.0f) < 1e-3f && std::fabs(app.playSeconds() - 3725.0) < 1.0);
	// An empty slot: nothing happens, the menu says so.
	app.menu().open();
	app.render();
	CHECK(!app.loadSlot(5) && std::string(app.worldName()) == "world/world");
	// A file of before the header loads, and its slot shows its world.
	{
		app.slotPath(3, path, sizeof(path));
		FILE* f = fopen(path, "rb");
		SaveHeader h;
		CHECK(f && fread(&h, sizeof(h), 1, f) == 1);
		fseek(f, long(h.bytes), SEEK_SET);
		std::vector<u8> rest;
		int c;
		while ((c = fgetc(f)) != EOF) rest.push_back(u8(c));
		fclose(f);
		char old[256];
		app.slotPath(7, old, sizeof(old));
		f = fopen(old, "wb");
		fwrite(rest.data(), rest.size(), 1, f);
		fclose(f);
		app.refreshSlots();
		CHECK(app.slotInfo(7).used && app.slotInfo(7).legacy && std::strcmp(app.slotInfo(7).world, "world") == 0);
		CHECK(app.enterWorld(hashName("world/cave"), nullptr, 0));
		CHECK(app.loadSlot(7) && std::string(app.worldName()) == "world/world");
	}
	// Dead or talking, no save: (no hero in this archive - canSave holds; the rule is the menu's line otherwise).
	CHECK(app.canSave());
	app.menu().close();
	app.render();
	app.shutdown();
}

// The start menu (fix189): built from the menu script's MENU_MAIN over the start screen, as OpenGothic builds it - the
// items the script names with its texts, which of them can be chosen out of a game, what they do; the port's pages
// for loading and the options over it; continuing from the quick save behind the original's loading screen.
static void testStartMenu() {
	std::vector<BakeWorld> worlds;
	const std::string path = gDir + "/startmenu.pak";
	const BakeResult baked = bakeLifeWorlds(syntheticScriptSource(), path, worlds, true, false, false, 0, true);
	CHECK(baked.report.find("start menu: MENU_MAIN of the menu script; 6 pictures baked") != std::string::npos);
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr));
	app.world().preload(app.camera().pos);
	const std::string quick = gDir + "/start-quick.bin";
	remove(quick.c_str());
	app.setStatePath(quick.c_str());
	app.setSaveDir((gDir + "/start-saves").c_str());
	app.npcs().game().setQuiet(true);
	app.openStartMenu();
	const StartMenu& sm = app.startMenu();
	CHECK(app.startMenuActive() && sm.fromScript() && sm.hasBackground() && sm.hasFrame() && sm.count() == 9);
	CHECK(sm.selected() >= 0 && std::strcmp(sm.item(sm.selected()).text, "New Game") == 0);
	// Out of a game, without a quick save: new, load, options and exit; not save, continue, the intro, the credits.
	std::string can;
	for (int i = 0; i < sm.count(); ++i)
		if (sm.enabled(i)) can += std::string(sm.item(i).text) + "|";
	CHECK(can == "New Game|Load Game|Options|Exit Gothic|");
	int wx, wy, ww, wh;
	sm.window(wx, wy, ww, wh);
	CHECK(wx == 40 && wy == 0 && ww == 320 && wh == 240); // (640 x 480 at half its size, in the middle)
	int ix, iy, iw, ih;
	sm.itemRect(1, ix, iy, iw, ih);
	CHECK(ix == 40 && iw == 320 && std::abs(iy - 84) <= 1 && std::abs(ih - 21) <= 1);
	app.render();
	writePpm(gDir + "/start-menu.ppm");
	{
		const u8* fb = gfx::softFramebuffer();
		const u8* side = fb + (120 * gfx::SCREEN_W + 10) * 4;   // the start screen beside the window
		const u8* edge = fb + (120 * gfx::SCREEN_W + 44) * 4;   // the frame's stone edge
		CHECK(side[0] > side[2] && edge[0] > 100 && edge[1] > 100);
	}
	MenuInput down, up, ok, back, right;
	down.down = true;
	up.up = true;
	ok.confirm = true;
	back.back = true;
	right.right = true;
	const auto selectedText = [&]() { return std::string(sm.selected() >= 0 ? sm.item(sm.selected()).text : ""); };
	app.startMenuInput(down);
	CHECK(selectedText() == "Load Game");
	app.startMenuInput(down);
	CHECK(selectedText() == "Options"); // (save and continue passed by)
	app.startMenuInput(down);
	CHECK(selectedText() == "Exit Gothic");
	app.startMenuInput(down);
	CHECK(selectedText() == "New Game");
	app.startMenuInput(up);
	CHECK(selectedText() == "Exit Gothic");
	CHECK(app.startMenuInput(ok).exit);
	// The options: the settings' pages over the start screen; B comes back to the start menu.
	app.startMenuInput(up);
	CHECK(selectedText() == "Options");
	app.startMenuInput(ok);
	CHECK(app.menu().isOpen() && app.menu().page() == Menu::PAGE_OPTIONS && app.startMenuActive());
	app.render();
	writePpm(gDir + "/start-options.ppm");
	app.startMenuInput(ok);
	CHECK(app.menu().page() == Menu::PAGE_GRAPHICS);
	const u8 drawWas = app.settings().drawMetres;
	CHECK(app.startMenuInput(right).settingsChanged && app.settings().drawMetres != drawWas);
	app.settings().drawMetres = drawWas;
	app.startMenuInput(back);
	CHECK(app.menu().isOpen() && app.menu().page() == Menu::PAGE_OPTIONS);
	app.startMenuInput(back);
	CHECK(!app.menu().isOpen() && app.startMenuActive());
	// The load page, with nothing saved: B back.
	app.startMenuInput(up);
	app.startMenuInput(ok);
	CHECK(app.menu().isOpen() && app.menu().page() == Menu::PAGE_LOAD);
	app.startMenuInput(back);
	CHECK(!app.menu().isOpen() && app.startMenuActive());
	// A new game: the world as it was made, the menu gone (its pictures with it).
	app.startMenuInput(up);
	CHECK(selectedText() == "New Game");
	app.startMenuInput(ok);
	CHECK(!app.startMenuActive());
	const float dt = 1.0f / 30.0f;
	for (int i = 0; i < 5; ++i) {
		app.update(Input {}, dt);
		app.render();
	}
	// Continue: from the quick save, behind the original's loading screen.
	CHECK(app.saveState(quick.c_str()));
	const Vec3 there = app.player().eye();
	app.player().flying = true;
	app.teleport({there.x + 2000.0f, there.y + 500.0f, there.z});
	app.openStartMenu();
	can.clear();
	for (int i = 0; i < sm.count(); ++i)
		if (sm.enabled(i)) can += std::string(sm.item(i).text) + "|";
	CHECK(can == "New Game|Load Game|Continue|Options|Exit Gothic|");
	app.startMenuInput(down);
	app.startMenuInput(down);
	CHECK(selectedText() == "Continue");
	const u32 frames = app.progressFrames();
	app.startMenuInput(ok);
	writePpm(gDir + "/start-loading.ppm");
	CHECK(!app.startMenuActive() && app.progressFrames() > frames);
	CHECK(std::fabs(app.player().eye().x - there.x) < 1.0f && std::fabs(app.player().eye().z - there.z) < 1.0f);
	{
		// (The loading screen's bar: the picture of PROGRESS_BAR, red-orange, three quarters down.)
		const u8* fb = gfx::softFramebuffer();
		const u8* bar = fb + ((gfx::SCREEN_H * 3 / 4 + 6) * gfx::SCREEN_W + gfx::SCREEN_W / 2) * 4;
		CHECK(bar[0] > 150 && bar[2] < 80);
	}
	app.shutdown();

	// An archive without a menu script: the engine's own items, in the interface's language.
	App plain;
	CHECK(plain.init((gDir + "/worlds.pak").c_str(), nullptr));
	plain.openStartMenu();
	CHECK(!plain.startMenu().fromScript() && plain.startMenu().count() == 6 && std::strcmp(plain.startMenu().item(0).text, "Новая игра") == 0);
	plain.render();
	writePpm(gDir + "/start-builtin.ppm");
	plain.shutdown();
}

// Talks: A at Thorus, the greeting he begins with, the menu of what the scripts
// offer, a choice inside an info, a quest begun and finished, a routine changed.
static void testDialogs() {
	std::vector<BakeWorld> worlds;
	const std::string path = gDir + "/dialogs.pak";
	bakeLifeWorlds(syntheticScriptSource(), path, worlds);
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr));
	if (!app.npcs().loaded() || !app.hero().loaded()) return;
	app.world().preload(app.camera().pos);
	Npcs& npcs = app.npcs();
	Game& g = npcs.game();
	g.setQuiet(true);
	app.settings().fights = 0;
	app.settings().perception = 0;
	app.settings().immortal = 1;
	app.applySettings();
	const u32 hero = u32(g.hero() - &g.npc(0));
	const float dt = 1.0f / 30.0f;
	const auto run = [&](float seconds) {
		for (int i = 0; i < int(seconds / dt); ++i) app.update(Input {}, dt);
	};
	const u32 thorusSym = g.vm().find("GRD_200_THORUS"), apple = g.vm().find("ITFOAPPLE"), potion = g.vm().find("ITFO_POTION_HEALTH_01");
	CHECK(thorusSym != SCRIPT_NONE && apple != SCRIPT_NONE);
	u32 thorus = SCRIPT_NONE;
	for (u32 i = 0; i < g.npcCount(); ++i)
		if (g.npc(i).symbol == thorusSym) thorus = i;
	CHECK(thorus != SCRIPT_NONE);
	if (thorus == SCRIPT_NONE) return;
	CHECK(g.hasInfos(thorus));
	run(1.0f);

	// Stands the character two paces from Thorus, facing him.
	app.teleport({g.npc(thorus).pos[0], g.npc(thorus).pos[1] + 5.0f, g.npc(thorus).pos[2] + 150.0f});
	app.camera().yaw = 3.14159265f;
	run(1.5f);
	CHECK(npcs.canTalk(thorus));
	CHECK(app.talkTarget() == int(thorus));
	Input a;
	a.confirm = true;
	app.update(a, dt);
	const auto until = [&](auto&& pred, float limit) {
		for (float t = 0; t < limit && !pred(); t += dt) {
			if (npcs.talkView().saying) npcs.talkSkip();
			app.update(Input {}, dt);
		}
		return pred();
	};
	// 1. The important greeting is told first; then the menu comes up.
	CHECK(until([&] { return npcs.talkView().choosing; }, 40.0f));
	CHECK(app.panel().page() == InventoryPanel::PAGE_DIALOG);
	const int helloInfo = g.infoIndex(g.vm().find("DIA_THORUS_HELLO"));
	CHECK(helloInfo >= 0 && g.info(u32(helloInfo)).told);
	const auto itemWith = [&](const char* prefix) {
		const TalkView& v = npcs.talkView();
		for (u32 i = 0; i < v.count; ++i)
			if (strncmp(v.items[i], prefix, strlen(prefix)) == 0) return int(i);
		return -1;
	};
	CHECK(itemWith("Who are you") < 0); // (told, not permanent: off the menu)
	const int job = itemWith("What do you do"), favour = itemWith("Is there anything");
	CHECK(job >= 0 || favour >= 0);

	// 2. The favour: a choice inside the info, the quest begins.
	if (favour >= 0) {
		npcs.talkChoose(u32(favour));
		CHECK(until([&] { return npcs.talkView().choosing && itemWith("I will bring") >= 0; }, 40.0f));
		npcs.talkChoose(u32(itemWith("I will bring")));
		CHECK(until([&] { return npcs.talkView().choosing && itemWith("End") >= 0 && itemWith("I will bring") < 0; }, 40.0f));
		CHECK(itemWith("Is there anything") < 0);
		bool topic = false;
		for (u32 i = 0; i < g.logTopicCount(); ++i)
			if (g.logTopic(i).section == g.constant("LOG_MISSION", 0) && g.logTopic(i).status == g.constant("LOG_RUNNING", 1)) topic = true;
		CHECK(topic);
	}
	// 3. Leaving: the last way of the menu.
	const int leave = int(npcs.talkView().count) - 1;
	CHECK(leave >= 0);
	npcs.talkChoose(u32(leave));
	CHECK(until([&] { return !npcs.talkView().active; }, 40.0f));
	run(0.5f);
	CHECK(app.panel().page() == InventoryPanel::PAGE_PLAY);

	// 4. The apple is brought: the quest ends with a reward and a new routine.
	g.giveItems(hero, apple, 1);
	const i32 potions = g.itemCount(hero, potion), apples = g.itemCount(hero, apple);
	// (Back in his routine first: the state of talking listens for nothing, as the original's.)
	CHECK(until([&] { return app.talkTarget() == int(thorus) && npcs.canTalk(thorus); }, 8.0f));
	run(1.5f);
	app.update(a, dt);
	CHECK(until([&] { return npcs.talkView().choosing; }, 40.0f));
	const int done = itemWith("Here is your apple");
	CHECK(done >= 0);
	if (done >= 0 && favour >= 0) {
		npcs.talkChoose(u32(done));
		CHECK(until([&] { return npcs.talkView().choosing || !npcs.talkView().active; }, 40.0f));
		CHECK(g.itemCount(hero, apple) == apples - 1 && g.itemCount(hero, potion) == potions + 1);
		CHECK(g.vm().getInt(g.vm().find("MIS_DELIVERY")) == g.constant("LOG_SUCCESS", 2));
		// The chapter card waits for the talk to end (it does not come up in the middle of it).
		app.update(a, dt);
		if (npcs.talkView().active) CHECK(app.chapterSeen() != g.chapter().serial || g.chapter().serial == 0);
	}
	npcs.talkAbort();
	run(0.5f);
	CHECK(!npcs.talkView().active);

	// 5. The story has moved on: a chapter began (the card is up), the diary says so.
	if (favour >= 0) {
		CHECK(g.chapterNumber() == 2);
		CHECK(std::strcmp(g.chapter().title, "Chapter 2") == 0 && g.chapter().serial == 2);
		CHECK(app.chapterSeen() == 2);
		{
			// (fix168) The debug page's chapter slider: it follows the game, and moved, it changes the chapter.
			app.update(a, dt);
			CHECK(app.settings().chapter == 2);
			app.settings().chapter = 4;
			app.applySettings();
			CHECK(g.chapterNumber() == 4);
			app.settings().chapter = 2;
			app.applySettings();
			CHECK(g.chapterNumber() == 2);
		}
		{
			// The baker finds the pictures the scripts name for the chapters (the calls' literal arguments).
			u32 sites[16 * 5] = {};
			const u32 intro = g.vm().find("INTRODUCECHAPTER");
			const u32 found = intro == SCRIPT_NONE ? 0 : g.vm().callSites(intro, 5, sites, 16);
			CHECK(found >= 2);
			bool one = false, two = false;
			for (u32 k = 0; k < found && k < 16; ++k) {
				const char* pic = g.vm().getString(sites[k * 5 + 2]);
				one = one || std::strcmp(pic, "chapter1.tga") == 0;
				two = two || std::strcmp(pic, "chapter2.tga") == 0;
			}
			CHECK(one && two);
		}
		run(11.0f);
		CHECK(app.chapterTime() <= 0.0f);

		// What was written is "fresh" until the diary has been looked at and left.
		{
			app.panel().openPage(InventoryPanel::PAGE_DIARY);
			run(0.3f);
			bool fresh = false;
			for (u32 i = 0; i < app.inventory().topicCount; ++i) fresh = fresh || app.inventory().topics[i].fresh;
			CHECK(fresh);
			app.panel().openPage(InventoryPanel::PAGE_PLAY);
			run(0.3f);
			for (u32 i = 0; i < app.inventory().topicCount; ++i) CHECK(!app.inventory().topics[i].fresh);
		}
		// 6. Saved, then undone, then loaded: the quest, the chapter, the routine,
		// the diary and the infos are as they were.
		const std::string state = gDir + "/quest.state";
		CHECK(app.saveState(state.c_str()));
		const u32 delivery = g.vm().find("MIS_DELIVERY"), kapitel = g.vm().find("KAPITEL");
		const u32 restFn = g.routine(g.npc(thorus).routineFirst).state;
		const i8 attitude = g.npc(thorus).attitude;
		const u32 topics = g.logTopicCount(), entries = g.logEntryCount();
		const int hello = g.infoIndex(g.vm().find("DIA_THORUS_HELLO"));
		CHECK(hello >= 0 && g.info(u32(hello)).told);
		g.vm().setInt(delivery, 0, nullptr, 0);
		g.vm().setInt(kapitel, 0, nullptr, 1);
		g.setInfoTold(u32(hello), false);
		g.npcMut(thorus).attitude = 0;
		g.giveItems(thorus, apple, 3);
		g.pushMessage("noise", 1.0f);
		CHECK(app.loadState(state.c_str()));
		CHECK(g.vm().getInt(delivery) == g.constant("LOG_SUCCESS", 2));
		CHECK(g.chapterNumber() == 2);
		CHECK(g.info(u32(hello)).told);
		CHECK(g.logTopicCount() == topics && g.logEntryCount() == entries);
		bool success = false;
		for (u32 i = 0; i < g.logTopicCount(); ++i)
			if (g.logTopic(i).status == g.constant("LOG_SUCCESS", 2)) success = true;
		CHECK(success);
		CHECK(g.npc(thorus).attitude == attitude);
		CHECK(g.itemCount(thorus, apple) == 0);
		CHECK(g.routine(g.npc(thorus).routineFirst).state == restFn);
		// The diary shows the chapter and the finished task.
		{
			app.panel().openPage(InventoryPanel::PAGE_DIARY);
			run(0.3f);
			const InventoryView& dv = app.inventory();
			CHECK(std::strcmp(dv.chapter, "Chapter 2") == 0 && dv.chapterNumber == 2);
			bool finished = false;
			for (u32 i = 0; i < dv.topicCount; ++i) finished = finished || dv.topics[i].status == 2;
			CHECK(finished);
			app.panel().openPage(InventoryPanel::PAGE_PLAY);
			run(0.3f);
		}
		GameLogNotice note;
		CHECK(!g.takeLogNotice(note) || note.topic[0]);
		// (A card seen before the save does not come up again by loading it.)
		run(0.5f);
		CHECK(app.chapterTime() <= 0.0f && app.chapterSeen() == 2);
		// A damaged state is refused, not half-loaded.
		{
			FILE* f = std::fopen(state.c_str(), "r+b");
			CHECK(f != nullptr);
			if (f) {
				std::fseek(f, -8, SEEK_END);
				const unsigned char junk[4] = {1, 2, 3, 4};
				std::fwrite(junk, 1, 4, f);
				std::fclose(f);
			}
			g.vm().setInt(delivery, 0, nullptr, 0);
			app.loadState(state.c_str());
			CHECK(g.vm().getInt(delivery) == 0); // (the damaged section changed nothing)
		}
	}
}

// The chests of the test camp: an open one, a locked one with its key, a locked
// one with a combination; a block of stone to climb onto.
// Particle effects: the synthetic world has a fire and its smoke by the start; they run only near the player.
static void testParticles() {
	std::vector<BakeWorld> worlds;
	const std::string path = gDir + "/particles.pak";
	const BakeResult r = bakeLifeWorlds(syntheticScriptSource(), path, worlds);
	CHECK(r.report.find("particles: 2 effects in the scripts (2 pictures baked, 0 of the names have none), 2 emitters in the worlds") != std::string::npos);
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr));
	if (!app.npcs().loaded()) return;
	app.world().preload(app.camera().pos);
	const float dt = 1.0f / 30.0f;
	const auto run = [&](float seconds) {
		for (int i = 0; i < int(seconds / dt); ++i) app.update(Input {}, dt);
	};
	const Particles& p = app.particles();
	CHECK(p.stats().emitters == 2);
	run(0.5f);
	CHECK(p.stats().running == 2 && p.stats().effects == 2);
	run(3.0f);
	// 30 + 6 a second, living 0.9 s and 2.5 s: some tens, within the pool.
	CHECK(p.stats().particles >= 20 && p.stats().particles <= Particles::MAX_PARTICLES);
	app.render();
	CHECK(p.stats().drawCalls >= 1);
	// A one-off effect of the scripts' own.
	CHECK(app.particles().stats().effects == 2);
	CHECK(app.particles().play(hashName("SYNTH_FIRE"), {0, 0, 0}, 0.0f, 1.0f));
	CHECK(!app.particles().play(hashName("NO_SUCH_EFFECT"), {0, 0, 0}, 0.0f, 1.0f));
	// A fire burns where its torch is drawn: where the world draws nothing that near, the fires are out (and come back with it).
	{
		const float drawn = app.drawDistance();
		app.setDrawDistance(150.0f);
		run(4.0f);
		CHECK(p.stats().particles < 5);
		app.setDrawDistance(drawn);
		run(4.0f);
		CHECK(p.stats().particles >= 20);
	}
	// Far away nothing emits and what is alive dies out.
	app.teleport({app.camera().pos.x + 9000.0f, app.camera().pos.y, app.camera().pos.z});
	run(4.0f);
	CHECK(p.stats().running <= 1); // (only the one-off effect, left behind)
	CHECK(p.stats().particles < 40);
}


// Barter: Thorus's trade info opens his wares once it has been told; buying and selling at the scripts' prices,
// the money neither bought nor sold, what is worn kept, the purse short; B ends the trade and the menu is back.
// The things of the world the player uses: only those the world named show a prompt; a bench
// holds him until he lets go, a thing with a script runs it (when its condition allows).
// The console's controls on a ladder: the stick up climbs, the stick down climbs down - the character keeps
// facing the ladder and does not turn away from it (as it does when steered towards a heading).
static void testLadderSteering() {
	std::vector<BakeWorld> worlds;
	const std::string path = gDir + "/ladder_steer.pak";
	bakeLifeWorlds(syntheticScriptSource(), path, worlds);
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr));
	if (!app.hero().loaded()) return;
	app.settings().scheme = CONTROLS_CONSOLE;
	app.settings().immortal = 1;
	app.applySettings();
	app.npcs().game().setQuiet(true);
	const float dt = 1.0f / 30.0f;
	Input next;
	next.nextWorld = true;
	app.update(next, dt);
	for (int i = 0; i < 60; ++i) app.update(Input {}, dt);
	const Player& pl = app.player();
	app.camera().yaw = 0;
	app.teleport({-1200.0f, 175.0f, 3000.0f});
	for (int i = 0; i < 60; ++i) app.update(Input {}, dt);
	CHECK(pl.grounded && !pl.climbing);
	bool climbed = false;
	float highest = 0;
	for (int i = 0; i < 90 && pl.feet.y < 120.0f; ++i) {
		Input in;
		in.moveZ = 1.0f; // the stick pushed away from the camera
		app.update(in, dt);
		climbed = climbed || pl.climbing;
		highest = std::max(highest, pl.feet.y);
	}
	std::printf("  ladder (console): up to %.0f cm, climbing %d, facing %.2f\n", double(highest), int(pl.climbing), double(app.hero().yaw()));
	CHECK(climbed && highest > 100.0f);
	// Part of the way up: the stick pulled back goes down, still facing the wall.
	if (!pl.climbing) return;
	const float up = pl.feet.y;
	for (int i = 0; i < 20; ++i) {
		Input in;
		in.moveZ = -1.0f;
		app.update(in, dt);
	}
	std::printf("  ladder (console): down from %.0f to %.0f cm, facing %.2f\n", double(up), double(pl.feet.y), double(app.hero().yaw()));
	CHECK(pl.feet.y < up - 40.0f);
	CHECK(std::fabs(app.hero().yaw()) < 0.2f);
	for (int i = 0; i < 90 && !pl.grounded; ++i) {
		Input in;
		in.moveZ = -1.0f;
		app.update(in, dt);
	}
	CHECK(pl.grounded && std::fabs(pl.feet.y) < 0.5f && !pl.climbing);

	// From the top with A: on the ledge by the ladder's head the prompt is up; A puts him over the edge onto
	// the rungs, turned to face them; down to the floor, and A at the foot takes hold again.
	app.camera().yaw = 3.14159f; // (the character faces the way the camera does when put somewhere)
	app.teleport({-1200.0f, 300.0f + 175.0f, 3480.0f});
	for (int i = 0; i < 30; ++i) app.update(Input {}, dt);
	CHECK(pl.grounded && std::fabs(pl.feet.y - 300.0f) < 1.0f && !pl.climbing);
	std::printf("  ladder (top): prompt %d\n", app.ladderTarget());
	CHECK(app.ladderTarget() == 0);
	Input a;
	a.confirm = true;
	app.update(a, dt);
	CHECK(pl.climbing && (pl.ladderPhase() == Player::LADDER_MOUNT_TOP));
	bool clung = false;
	for (int i = 0; i < 60; ++i) {
		app.update(Input {}, dt);
		clung = clung || pl.ladderPhase() == Player::LADDER_CLIMB;
	}
	std::printf("  ladder (top): on it %d at %.0f cm, z %.0f, facing %.2f\n", int(clung), double(pl.feet.y), double(pl.feet.z), double(app.hero().yaw()));
	CHECK(clung && pl.climbing && pl.feet.y < 300.0f && pl.feet.y > 100.0f && pl.feet.z < 3390.0f && std::fabs(app.hero().yaw()) < 0.2f);
	for (int i = 0; i < 150 && !pl.grounded; ++i) {
		Input in;
		in.moveZ = -1.0f;
		app.update(in, dt);
	}
	for (int i = 0; i < 30; ++i) app.update(Input {}, dt);
	CHECK(pl.grounded && !pl.climbing && std::fabs(pl.feet.y) < 0.5f);
	CHECK(app.ladderTarget() == 0);
	app.update(a, dt);
	CHECK(pl.climbing && pl.ladderPhase() == Player::LADDER_MOUNT);
	// Up all the way: over the top onto the ledge - each way on, along and off the ladder with its own clip.
	const Model& body = app.hero().model();
	const Clip* const mountClip = body.clip("T_LADDER_STAND_2_S0");
	const Clip* const rungs[2] = {body.clip("T_LADDER_S0_2_S1"), body.clip("T_LADDER_S1_2_S2")};
	const Clip* const overClip = body.clip("T_LADDER_S2_2_STAND");
	bool sawMount = false, sawRungs = false, sawOver = false;
	for (int i = 0; i < 200 && !(pl.grounded && pl.feet.y > 290.0f); ++i) {
		Input in;
		in.moveZ = 1.0f;
		app.update(in, dt);
		const Clip* c = app.hero().animator().current();
		sawMount = sawMount || (mountClip && c == mountClip && pl.ladderPhase() == Player::LADDER_MOUNT);
		sawRungs = sawRungs || ((c == rungs[0] || c == rungs[1]) && c && pl.ladderPhase() == Player::LADDER_CLIMB);
		sawOver = sawOver || (overClip && c == overClip && pl.ladderPhase() == Player::LADDER_EXIT_TOP);
	}
	std::printf("  ladder (clips): on %d, rungs %d, over the top %d\n", int(sawMount), int(sawRungs), int(sawOver));
	CHECK(sawMount && sawRungs && sawOver);
	std::printf("  ladder (up again): standing at %.0f %.0f %.0f\n", double(pl.feet.x), double(pl.feet.y), double(pl.feet.z));
	CHECK(pl.grounded && !pl.climbing && std::fabs(pl.feet.y - 300.0f) < 1.0f && pl.feet.z > 3420.0f);
}

static void testMobUse() {
	std::vector<BakeWorld> worlds;
	const std::string path = gDir + "/mobuse.pak";
	bakeLifeWorlds(syntheticScriptSource(), path, worlds);
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr));
	if (!app.npcs().loaded() || !app.hero().loaded()) return;
	app.world().preload(app.camera().pos);
	Npcs& npcs = app.npcs();
	Game& g = npcs.game();
	g.setQuiet(true);
	app.settings().fights = 0;
	app.settings().perception = 0;
	app.settings().immortal = 1;
	app.applySettings();
	const u32 hero = u32(g.hero() - &g.npc(0));
	const float dt = 1.0f / 30.0f;
	const auto run = [&](float seconds) {
		for (int i = 0; i < int(seconds / dt); ++i) app.update(Input {}, dt);
	};
	run(1.0f);
	Mobs& mobs = npcs.mobs();
	CHECK(mobs.hasInfo());
	if (!mobs.hasInfo()) return;
	int bench = -1, anvil = -1, sharp = -1, hiddenBed = -1, namedBed = -1;
	u32 hidden = 0, named = 0;
	for (u32 i = 0; i < mobs.count(); ++i) {
		if (mobs.forPlayer(i)) ++named; else ++hidden;
		if (mobs.isScheme(i, "BENCH")) bench = int(i);
		else if (mobs.isScheme(i, "BSANVIL")) anvil = int(i);
		else if (mobs.isScheme(i, "BEDLOW")) sharp = int(i);
		else if (mobs.isScheme(i, "BEDHIGH")) (mobs.info(i).focus ? namedBed : hiddenBed) = int(i);
	}
	CHECK(bench >= 0 && anvil >= 0 && sharp >= 0 && hiddenBed >= 0 && namedBed >= 0);
	CHECK(named == 6);     // the bench, both beds (a bed is to sleep in, named or not), the anvil, the forge, the other
	CHECK(hidden == 3);    // the three chests: no prompt
	if (bench < 0 || anvil < 0 || sharp < 0 || hiddenBed < 0 || namedBed < 0) return;
	const auto stand = [&](int mob, float dx, float dz, float yaw) {
		const WorldMob& m = mobs.mob(u32(mob));
		app.camera().yaw = yaw;
		app.teleport({m.pos[0] + dx, m.pos[1] + 220.0f, m.pos[2] + dz});
		run(1.5f);
	};
	const auto pressA = [&]() {
		Input in;
		in.confirm = true;
		app.update(in, dt);
	};
	const auto used = [&]() { return g.vm().getInt(g.vm().find("MOBTEST_USED")); };

	// 1. A bed nobody named is a bed all the same: the prompt (the scripts' word for a bed), A lies down in it,
	//    B (the menu's last way: not now) gets up again.
	stand(hiddenBed, -150.0f, 0.0f, 1.5708f);
	CHECK(app.mobTarget() == int(hiddenBed));
	{
		char name[48];
		app.mobName(u32(hiddenBed), name, sizeof(name));
		CHECK(std::strcmp(name, "Bed") == 0);
	}
	pressA();
	run(2.5f);
	CHECK(app.mobInUse() && npcs.talkView().active);
	CHECK(npcs.talkBack());
	run(3.0f);
	CHECK(!app.mobInUse() && !npcs.talkView().active);

	// 2. The bench: the prompt, A, the place is held; A again lets go.
	stand(bench, 0.0f, -90.0f, 0.0f);
	CHECK(app.mobTarget() == bench);
	pressA();
	run(2.0f);
	CHECK(app.mobInUse());
	CHECK(mobs.userOf(u32(bench)) == int(hero));
	for (int i = 0; i < 20; ++i) app.update(Input {}, dt);
	pressA();
	run(2.0f);
	CHECK(!app.mobInUse());
	CHECK(mobs.userOf(u32(bench)) < 0);

	// 3. The grinding thing: its condition says no first (nothing run), then yes (the state function runs once, he lets go by himself).
	stand(sharp, 90.0f, 0.0f, -1.5708f);
	CHECK(app.mobTarget() == sharp);
	pressA();
	run(1.0f);
	CHECK(!app.mobInUse() && used() == 0);
	g.vm().setInt(g.vm().find("MOBTEST_ALLOW"), 0, nullptr, 1);
	pressA();
	run(3.0f);
	CHECK(used() == 1);
	CHECK(!app.mobInUse());
	CHECK(mobs.userOf(u32(sharp)) < 0);

	// 4. The named bed: it is a bed, not a door - he lies down in it and stays; its state function (SLEEPABIT_S1)
	//    puts up his own infos (AI_ProcessInfos(hero)); "until morning" moves the clock, and he gets up again.
	const auto sleepTo = [&](const char* item) -> bool {
		const TalkView& v = npcs.talkView();
		for (u32 i = 0; i < v.count; ++i)
			if (std::strstr(v.items[i], item)) return npcs.talkChoose(i);
		return false;
	};
	app.environment().timeOfDay = 13.0f;
	stand(namedBed, 150.0f, 0.0f, -1.5708f);
	CHECK(app.mobTarget() == namedBed);
	pressA();
	run(2.5f);
	std::printf("  bed: in use %d, talk %d (%s), %u ways\n", int(app.mobInUse()), int(npcs.talkView().active), npcs.talkView().partner,
	            npcs.talkView().count);
	CHECK(app.mobInUse() && mobs.userOf(u32(namedBed)) == int(hero));
	CHECK(npcs.talkView().active && npcs.selfTalk() && npcs.talkView().count == 2);
	// (The stick does not get him out of bed while the talk is up.)
	for (int i = 0; i < 10; ++i) {
		Input in;
		in.moveZ = 1.0f;
		app.update(in, dt);
	}
	CHECK(app.mobInUse() && npcs.talkView().active);
	CHECK(sleepTo("morning"));
	run(3.0f);
	std::printf("  bed: slept until %.2f h, in use %d, talk %d\n", double(app.environment().timeOfDay), int(app.mobInUse()),
	            int(npcs.talkView().active));
	CHECK(std::fabs(app.environment().timeOfDay - 8.0f) < 0.1f);
	CHECK(!npcs.talkView().active && !app.mobInUse() && mobs.userOf(u32(namedBed)) < 0);

	// 5. The same bed when its scripts put up nothing: the engine's menu of hours; sleeping heals.
	g.vm().setInt(g.vm().find("SLEEPTEST_NOSCRIPT"), 0, nullptr, 1);
	app.environment().timeOfDay = 9.0f;
	GameNpc& me = g.npcMut(hero);
	g.setAttribute(me, 0, 1);
	stand(namedBed, 150.0f, 0.0f, -1.5708f);
	pressA();
	run(2.5f);
	CHECK(app.mobInUse() && npcs.talkView().active && npcs.talkView().count == 5);
	CHECK(sleepTo(tr("Спать до вечера")));
	run(3.0f);
	std::printf("  bed (engine's menu): slept until %.2f h, hit points %d\n", double(app.environment().timeOfDay), int(g.hitPoints(g.npc(hero))));
	CHECK(std::fabs(app.environment().timeOfDay - 20.0f) < 0.1f && g.hitPoints(g.npc(hero)) > 1);
	CHECK(!npcs.talkView().active && !app.mobInUse());
	// "Not now" (B: the last way): up again, the clock as it was.
	stand(namedBed, 150.0f, 0.0f, -1.5708f);
	pressA();
	run(2.5f);
	CHECK(npcs.talkView().active && npcs.talkBack());
	run(3.0f);
	CHECK(std::fabs(app.environment().timeOfDay - 20.0f) < 0.2f && !app.mobInUse());
	g.vm().setInt(g.vm().find("SLEEPTEST_NOSCRIPT"), 0, nullptr, 0);

	// 6. (fix153) The anvil the way the games with crafting have it: its state function (SMITHWEAPON_S1) puts up his
	//    own infos of what to make; he stays at it while they are up (the stick does nothing), the choice makes the
	//    thing out of what he carries, and he leaves it, its SMITHWEAPON_S0 running.
	{
		const u32 nugget = g.vm().find("ITMINUGGET"), sword = g.vm().find("ITMW_SWORD");
		CHECK(nugget != SCRIPT_NONE && sword != SCRIPT_NONE);
		g.giveItems(hero, nugget, 2);
		const i32 swords = g.itemCount(hero, sword);
		const u32 callsBefore = app.mobUseStats().stateCalls;
		stand(anvil, 0.0f, 160.0f, 3.14159f);
		CHECK(app.mobTarget() == anvil);
		pressA();
		run(2.5f);
		std::printf("  anvil: in use %d, talk %d, %u ways, %u state calls, %u talks\n", int(app.mobInUse()), int(npcs.talkView().active),
		            npcs.talkView().count, app.mobUseStats().stateCalls - callsBefore, app.mobUseStats().talks);
		CHECK(app.mobUsed() == anvil && npcs.talkView().active && npcs.selfTalk() && npcs.talkView().count == 2);
		for (int i = 0; i < 10; ++i) {
			Input in;
			in.moveZ = 1.0f;
			app.update(in, dt);
		}
		CHECK(app.mobInUse() && npcs.talkView().active);
		CHECK(sleepTo("sword"));
		run(3.0f);
		CHECK(g.itemCount(hero, sword) == swords + 1 && g.itemCount(hero, nugget) == 0);
		CHECK(!npcs.talkView().active && !app.mobInUse() && mobs.userOf(u32(anvil)) < 0);
		CHECK(g.vm().getInt(g.vm().find("MOBTEST_LEFT")) == 1 && app.mobUseStats().stateCalls == callsBefore + 2);
	}

	// 7. (fix154) The forge the Gothic 1 way: no function at all. It wants a nugget (nothing in his things: it says so
	//    and he stays where he is); with one, the tags of its animations take it into his hand and turn it into a
	//    sword, and letting go leaves him with the sword.
	{
		int forge = -1;
		for (u32 i = 0; i < mobs.count(); ++i)
			if (mobs.isScheme(i, "BSFIRE")) forge = int(i);
		const u32 nugget = g.vm().find("ITMINUGGET"), sword = g.vm().find("ITMW_SWORD");
		CHECK(forge >= 0 && nugget != SCRIPT_NONE && sword != SCRIPT_NONE);
		if (forge < 0) return;
		const i32 swords = g.itemCount(hero, sword);
		stand(forge, 0.0f, 160.0f, 3.14159f);
		CHECK(app.mobTarget() == forge && g.itemCount(hero, nugget) == 0);
		pressA();
		run(1.0f);
		CHECK(!app.mobInUse());
		g.giveItems(hero, nugget, 1);
		pressA();
		run(3.0f);
		std::printf("  forge: in use %d, nuggets %d, swords %d (before %d)\n", int(app.mobInUse()), int(g.itemCount(hero, nugget)),
		            int(g.itemCount(hero, sword)), int(swords));
		CHECK(app.mobInUse() && g.itemCount(hero, nugget) == 0 && g.itemCount(hero, sword) == swords + 1);
		pressA();
		run(2.0f);
		CHECK(!app.mobInUse() && g.itemCount(hero, sword) == swords + 1);
	}
}

static void testTrade() {
	std::vector<BakeWorld> worlds;
	const std::string path = gDir + "/trade.pak";
	bakeLifeWorlds(syntheticScriptSource(), path, worlds);
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr));
	if (!app.npcs().loaded() || !app.hero().loaded()) return;
	app.world().preload(app.camera().pos);
	Npcs& npcs = app.npcs();
	Game& g = npcs.game();
	g.setQuiet(true);
	app.settings().fights = 0;
	app.settings().perception = 0;
	app.settings().immortal = 1;
	app.applySettings();
	const u32 hero = u32(g.hero() - &g.npc(0));
	const float dt = 1.0f / 30.0f;
	const auto run = [&](float seconds) {
		for (int i = 0; i < int(seconds / dt); ++i) app.update(Input {}, dt);
	};
	const u32 thorusSym = g.vm().find("GRD_200_THORUS"), beer = g.vm().find("ITFOBEER"), potion = g.vm().find("ITFO_POTION_HEALTH_01"),
	          nugget = g.vm().find("ITMINUGGET"), sword = g.vm().find("ITMW_SWORD");
	CHECK(thorusSym != SCRIPT_NONE && beer != SCRIPT_NONE && potion != SCRIPT_NONE && nugget != SCRIPT_NONE);
	u32 thorus = SCRIPT_NONE;
	for (u32 i = 0; i < g.npcCount(); ++i)
		if (g.npc(i).symbol == thorusSym) thorus = i;
	CHECK(thorus != SCRIPT_NONE);
	if (thorus == SCRIPT_NONE) return;
	// 0. The prices are the scripts': Gothic 1 has no multiplier, the money is the ore.
	CHECK(g.tradeCurrency() == nugget && g.tradeMultiplier() == 1.0f);
	CHECK(g.buyPrice(beer) == 10 && g.sellPrice(beer) == 10 && g.buyPrice(potion) == 25 && g.buyPrice(nugget) == 1);
	CHECK(g.info(u32(g.infoIndex(g.vm().find("DIA_THORUS_TRADE")))).trade);
	run(1.0f);
	app.teleport({g.npc(thorus).pos[0], g.npc(thorus).pos[1] + 5.0f, g.npc(thorus).pos[2] + 150.0f});
	app.camera().yaw = 3.14159265f;
	run(1.5f);
	Input a;
	a.confirm = true;
	app.update(a, dt);
	const auto until = [&](auto&& pred, float limit) {
		for (float t = 0; t < limit && !pred(); t += dt) {
			if (npcs.talkView().saying) npcs.talkSkip();
			app.update(Input {}, dt);
		}
		return pred();
	};
	CHECK(until([&] { return npcs.talkView().choosing; }, 40.0f));
	const auto itemWith = [&](const char* prefix) {
		const TalkView& v = npcs.talkView();
		for (u32 i = 0; i < v.count; ++i)
			if (strncmp(v.items[i], prefix, strlen(prefix)) == 0) return int(i);
		return -1;
	};
	const int trade = itemWith("Let us trade");
	CHECK(trade >= 0);
	if (trade < 0) return;

	// 1. The wares come up: the trade page, the stock without the sword he wears, the money out of the pack.
	const i32 purse = 100;
	g.giveItems(hero, nugget, purse - g.itemCount(hero, nugget));
	npcs.talkChoose(u32(trade));
	CHECK(until([&] { return npcs.trading(); }, 40.0f));
	run(0.3f);
	CHECK(app.panel().page() == InventoryPanel::PAGE_CHEST && npcs.talkView().trading && !npcs.talkView().choosing);
	const auto view = [&]() -> const InventoryView& { return app.inventory(); };
	{
		const InventoryView& v = view();
		CHECK(v.chest.trade && v.chest.open && v.chest.gold == purse && std::strcmp(v.chest.name, "Thorus") == 0);
		CHECK(v.chest.count == 2); // (the beer and the potions; not the sword he wears)
		for (u32 i = 0; i < v.chest.count; ++i) {
			const ItemEntry& e = v.chest.items[i];
			CHECK((e.symbol == beer && e.count == 4 && e.price == 10) || (e.symbol == potion && e.count == 2 && e.price == 25));
		}
		bool moneyOut = false;
		for (u32 i = 0; i < v.count; ++i)
			if (v.items[i].symbol == nugget) moneyOut = (v.items[i].flags & ITEMF_NOSELL) != 0;
		CHECK(moneyOut);
	}
	const auto cmd = [&](u8 slot, u32 sym, i32 amount, u8 type = PANEL_CHEST_MOVE) {
		PanelCommand c;
		c.type = type;
		c.slot = slot;
		c.symbol = sym;
		c.amount = amount;
		app.panelCommand(c);
	};
	const i32 beers = g.itemCount(hero, beer), potions = g.itemCount(hero, potion);
	// 2. Buying: two beers cost twenty; more than he has, or than the purse allows, is cut.
	cmd(0, beer, 2);
	CHECK(g.itemCount(hero, beer) == beers + 2 && g.itemCount(thorus, beer) == 2 && g.itemCount(hero, nugget) == purse - 20);
	cmd(0, beer, 10);
	CHECK(g.itemCount(hero, beer) == beers + 4 && g.itemCount(thorus, beer) == 0 && g.itemCount(hero, nugget) == purse - 40);
	CHECK(view().chest.count == 1);
	cmd(0, potion, 10); // (the purse allows two of a price of 25; he has two)
	CHECK(g.itemCount(hero, potion) == potions + 2 && g.itemCount(hero, nugget) == purse - 40 - 50);
	// Too poor: nothing moves.
	g.giveItems(hero, nugget, -(g.itemCount(hero, nugget) - 3));
	g.giveItems(thorus, potion, 1);
	const i32 heroPotions = g.itemCount(hero, potion);
	cmd(0, potion, 1);
	CHECK(g.itemCount(thorus, potion) == 1 && g.itemCount(hero, potion) == heroPotions && g.itemCount(hero, nugget) == 3);
	// 3. Selling: paid the whole value; the money itself is not sold; what is worn stays.
	cmd(1, beer, 3);
	CHECK(g.itemCount(hero, beer) == beers + 1 && g.itemCount(thorus, beer) == 3 && g.itemCount(hero, nugget) == 3 + 30);
	// fix158: the trader pays from his own purse - the money he took for the wares is there, and no more than he has.
	CHECK(g.itemCount(thorus, nugget) >= 60); // (90 paid for the wares, 30 paid out)
	g.giveItems(thorus, nugget, 15 - g.itemCount(thorus, nugget));
	{
		const i32 heroBeer = g.itemCount(hero, beer), heroOre = g.itemCount(hero, nugget);
		cmd(1, beer, 3); // (ten a piece, fifteen in his purse: one is bought)
		CHECK(g.itemCount(hero, beer) == heroBeer - 1 && g.itemCount(hero, nugget) == heroOre + 10 && g.itemCount(thorus, nugget) == 5);
		cmd(1, beer, 1); // (five left: none)
		CHECK(g.itemCount(hero, beer) == heroBeer - 1 && g.itemCount(hero, nugget) == heroOre + 10 && g.itemCount(thorus, nugget) == 5);
		g.giveItems(thorus, nugget, 95); // (as before)
	}
	const i32 ore = g.itemCount(hero, nugget), traderOre = g.itemCount(thorus, nugget);
	cmd(1, nugget, ore);
	CHECK(g.itemCount(hero, nugget) == ore && g.itemCount(thorus, nugget) == traderOre);
	if (sword != SCRIPT_NONE) {
		const i32 swords = g.itemCount(hero, sword);
		cmd(1, sword, 5);
		CHECK(g.itemCount(hero, sword) >= (g.npc(hero).melee == sword ? 1 : 0) && g.itemCount(hero, sword) <= swords);
	}

	// 4. B: the wares go, the menu is back and the talk goes on.
	cmd(0, 0, 0, PANEL_CHEST_CLOSE);
	CHECK(until([&] { return npcs.talkView().choosing && !npcs.talkView().trading; }, 10.0f));
	CHECK(!npcs.trading() && itemWith("Let us trade") >= 0);
	run(0.3f);
	CHECK(app.panel().page() == InventoryPanel::PAGE_DIALOG);
	// Again, and left by the talk's end (the hero walks off): no page is left behind.
	npcs.talkChoose(u32(itemWith("Let us trade")));
	CHECK(until([&] { return npcs.trading(); }, 40.0f));
	npcs.talkAbort();
	run(0.5f);
	CHECK(!npcs.talkView().active && !npcs.trading() && app.panel().page() == InventoryPanel::PAGE_PLAY);
}


// Teaching: Thorus sells his skills for learning points (the scripts do the work: points taken, attribute raised,
// talent skill set). Without the skill the character cannot sneak, a rune of a circle he has not learnt is
// blocked; the character's page names the talents; the skills are kept in state.bin.
static void testTeach() {
	std::vector<BakeWorld> worlds;
	const std::string path = gDir + "/teach.pak";
	bakeLifeWorlds(syntheticScriptSource(), path, worlds);
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr));
	if (!app.npcs().loaded() || !app.hero().loaded()) return;
	app.world().preload(app.camera().pos);
	Npcs& npcs = app.npcs();
	Game& g = npcs.game();
	g.setQuiet(true);
	app.settings().fights = 0;
	app.settings().perception = 0;
	app.settings().immortal = 1;
	app.applySettings();
	const u32 hero = u32(g.hero() - &g.npc(0));
	const float dt = 1.0f / 30.0f;
	const auto run = [&](float seconds) {
		for (int i = 0; i < int(seconds / dt); ++i) app.update(Input {}, dt);
	};
	const u32 thorusSym = g.vm().find("GRD_200_THORUS"), bigRune = g.vm().find("ITARRUNEFIREBALL");
	const i32 sneak = g.constant("NPC_TALENT_SNEAK", 8), mage = g.constant("NPC_TALENT_MAGE", 7);
	u32 thorus = SCRIPT_NONE;
	for (u32 i = 0; i < g.npcCount(); ++i)
		if (g.npc(i).symbol == thorusSym) thorus = i;
	CHECK(thorus != SCRIPT_NONE && bigRune != SCRIPT_NONE);
	if (thorus == SCRIPT_NONE) return;
	const u32 lpSym = g.vm().findMember("C_NPC", "LP");
	const auto lp = [&]() { return g.npcInt(g.npc(hero), lpSym); };
	CHECK(lp() == 10 && g.npc(hero).skill[sneak] == 0 && g.npc(hero).skill[mage] == 0);
	CHECK(g.vm().find("NPC_GETTALENTSKILL") != SCRIPT_NONE);
	run(1.0f);
	// 1. Not learnt: the sneak key does nothing.
	Input sn;
	sn.toggleSneak = true;
	app.update(sn, dt);
	run(0.2f);
	CHECK(!app.hero().sneaking() && !app.heroCanSneak());
	// A rune of the third circle is out of reach (blocked in the pack).
	g.giveItems(hero, bigRune, 1);
	const auto blocked = [&]() {
		const InventoryView& v = app.inventory();
		for (u32 i = 0; i < v.count; ++i)
			if (v.items[i].symbol == bigRune) return (v.items[i].flags & ITEMF_BLOCKED) != 0;
		return false;
	};
	CHECK(blocked());
	// 2. The talk.
	app.teleport({g.npc(thorus).pos[0], g.npc(thorus).pos[1] + 5.0f, g.npc(thorus).pos[2] + 150.0f});
	app.camera().yaw = 3.14159265f;
	run(1.5f);
	Input a;
	a.confirm = true;
	app.update(a, dt);
	const auto until = [&](auto&& pred, float limit) {
		for (float t = 0; t < limit && !pred(); t += dt) {
			if (npcs.talkView().saying) npcs.talkSkip();
			app.update(Input {}, dt);
		}
		return pred();
	};
	CHECK(until([&] { return npcs.talkView().choosing; }, 40.0f));
	const auto itemWith = [&](const char* prefix) {
		const TalkView& v = npcs.talkView();
		for (u32 i = 0; i < v.count; ++i)
			if (strncmp(v.items[i], prefix, strlen(prefix)) == 0) return int(i);
		return -1;
	};
	const auto choose = [&](const char* prefix) {
		const int k = itemWith(prefix);
		CHECK(k >= 0);
		if (k < 0) return;
		npcs.talkChoose(u32(k));
		app.update(Input {}, dt); // (the view follows the talk a frame later)
		CHECK(until([&] { return npcs.talkView().choosing; }, 40.0f));
	};
	CHECK(app.inventory().sheet.learn == 10);
	const i32 str = g.attributeOf(g.npc(hero), g.constant("ATR_STRENGTH", 4));
	choose("Teach me strength");
	CHECK(lp() == 9 && g.attributeOf(g.npc(hero), g.constant("ATR_STRENGTH", 4)) == str + 5);
	choose("Teach me to sneak");
	CHECK(lp() == 7 && g.npc(hero).skill[sneak] == 1 && itemWith("Teach me to sneak") < 0); // (taught: off the menu)
	choose("Teach me the first circle");
	CHECK(lp() == 4 && g.npc(hero).skill[mage] == 1 && itemWith("Teach me the first circle") < 0);
	// Too poor: the scripts refuse, nothing changes.
	g.vm().setInt(lpSym, 0, g.npc(hero).object, 0);
	choose("Teach me strength");
	CHECK(lp() == 0 && g.attributeOf(g.npc(hero), g.constant("ATR_STRENGTH", 4)) == str + 5);
	npcs.talkAbort();
	run(0.5f);
	// 3. What was learnt: sneaking works, the circle 1 rune is open, the third is still blocked.
	app.update(sn, dt);
	run(0.2f);
	CHECK(app.heroCanSneak() && app.hero().sneaking());
	CHECK(blocked());
	g.npcMut(hero).skill[mage] = 3;
	g.giveItems(hero, bigRune, 1); // (the pack is looked at anew)
	CHECK(!blocked());
	g.npcMut(hero).skill[mage] = 1;
	// 4. The page of the character names the talents by the scripts' words.
	{
		const InventoryView& v = app.inventory();
		CHECK(v.sheet.talentCount == 3);
		bool found = false;
		for (u32 i = 0; i < v.sheet.talentCount; ++i)
			if (std::strcmp(v.sheet.talents[i].name, "Sneaking") == 0) found = std::strcmp(v.sheet.talents[i].level, "Can") == 0;
		CHECK(found);
	}
	// 5. state.bin keeps the skills.
	const std::string statePath = gDir + "/teach-state.bin";
	CHECK(app.saveState(statePath.c_str()));
	g.npcMut(hero).skill[sneak] = 0;
	g.npcMut(hero).skill[mage] = 0;
	CHECK(app.loadState(statePath.c_str()));
	CHECK(g.npc(hero).skill[sneak] == 1 && g.npc(hero).skill[mage] == 1);
}

static void testChests() {
	std::vector<BakeWorld> worlds;
	const std::string path = gDir + "/chests.pak";
	bakeLifeWorlds(syntheticScriptSource(), path, worlds);
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr));
	if (!app.npcs().loaded() || !app.hero().loaded()) return;
	app.world().preload(app.camera().pos);
	Npcs& npcs = app.npcs();
	Game& g = npcs.game();
	g.setQuiet(true);
	app.settings().fights = 0;
	app.settings().perception = 0;
	app.settings().immortal = 1;
	app.applySettings();
	const u32 hero = u32(g.hero() - &g.npc(0));
	const float dt = 1.0f / 30.0f;
	const auto run = [&](float seconds) {
		for (int i = 0; i < int(seconds / dt); ++i) app.update(Input {}, dt);
	};
	const auto find = [&](const char* name) {
		const u32 sym = g.vm().find(name);
		CHECK(sym != SCRIPT_NONE && g.isItem(sym));
		return sym;
	};
	const u32 nugget = find("ITMINUGGET"), apple = find("ITFOAPPLE"), pick = find("ITKELOCKPICK"), key = find("ITKE_CAMPCHEST");
	const u32 potion = find("ITFO_POTION_HEALTH_01");
	run(1.0f);

	// 1. The table: three containers, one open, one locked with a key, one with a combination.
	Chests& chests = app.chests();
	CHECK(chests.count() == 3);
	if (chests.count() != 3) return;
	u32 bigC = 0, keyC = 0, pickC = 0;
	for (u32 i = 0; i < 3; ++i) {
		if (!chests.locked(i)) bigC = i;
		else if (chests.key(i) != SCRIPT_NONE) keyC = i;
		else pickC = i;
	}
	CHECK(!chests.locked(bigC) && chests.stackCount(bigC) == 4);
	CHECK(chests.locked(keyC) && chests.key(keyC) == key && chests.stackCount(keyC) == 1);
	CHECK(chests.locked(pickC) && std::strcmp(chests.pick(pickC), "LRL") == 0 && chests.stackCount(pickC) == 1);

	// Stands the character a pace south of the chest, facing it, and presses A.
	const auto before = [&](u32 c) {
		const Mobs& mobs = npcs.mobs();
		const WorldMob& m = mobs.mob(chests.mob(c));
		app.camera().yaw = 3.14159265f;
		app.teleport({m.pos[0], m.pos[1] + 220.0f, m.pos[2] + 90.0f});
		run(1.5f);
	};
	const auto pressA = [&]() {
		Input in;
		in.confirm = true;
		app.update(in, dt);
	};
	const auto count = [&](u32 sym) { return g.itemCount(hero, sym); };
	const auto chestHas = [&](u32 c, u32 sym) {
		for (u32 k = 0; k < chests.stackCount(c); ++k)
			if (chests.stack(c, k).symbol == sym) return chests.stack(c, k).count;
		return 0;
	};
	const auto cmd = [&](u8 type, u8 slot = 0, u32 sym = 0, i32 amount = 0) {
		PanelCommand c;
		c.type = type;
		c.slot = slot;
		c.symbol = sym;
		c.amount = amount;
		app.panelCommand(c);
	};

	// 2. The open chest: A, the chest page, things both ways, everything out.
	before(bigC);
	CHECK(app.chestTarget() == int(bigC) && app.chestStage() == App::CH_NONE);
	pressA();
	run(3.0f);
	CHECK(app.chestStage() == App::CH_OPEN && app.panel().page() == InventoryPanel::PAGE_CHEST);
	// fix157: the chest's lid is a mover named for the chest's place among the things of the world; opening sends it its event.
	char lidEvent[40];
	std::snprintf(lidEvent, sizeof(lidEvent), "G3CHEST%lu", static_cast<unsigned long>(chests.mob(bigC)));
	CHECK(std::strcmp(app.triggers().stats().last, lidEvent) == 0);
	{
		const InventoryView& v = app.inventory();
		CHECK(v.chest.open && !v.chest.lock && v.chest.count == 4 && std::strcmp(v.chest.name, "Большой сундук") == 0);
	}
	const i32 apples = count(apple);
	cmd(PANEL_CHEST_MOVE, 0, nugget, 75);
	CHECK(count(nugget) == 75 && chestHas(bigC, nugget) == 0 && chests.stackCount(bigC) == 3);
	cmd(PANEL_CHEST_MOVE, 1, apple, 1); // (one of the pack's apples into the chest)
	CHECK(count(apple) == apples - 1 && chestHas(bigC, apple) == 3);
	cmd(PANEL_CHEST_MOVE, 1, potion, 99); // (no more than there are)
	CHECK(count(potion) == 0 && chestHas(bigC, potion) == 3);
	cmd(PANEL_CHEST_MOVE, 0, potion, 3);
	CHECK(count(potion) == 3);
	cmd(PANEL_CHEST_ALL);
	CHECK(chests.stackCount(bigC) == 0 && count(pick) == 3 && count(key) == 1 && count(apple) == apples + 2);
	// Persistence: what changed is in state.bin; the key goes back into the chest, a load puts it back where it was.
	const std::string statePath = gDir + "/chests-state.bin";
	CHECK(app.saveState(statePath.c_str()));
	cmd(PANEL_CHEST_MOVE, 1, nugget, 10);
	CHECK(chestHas(bigC, nugget) == 10);
	CHECK(app.loadState(statePath.c_str()));
	run(0.5f);
	CHECK(chests.stackCount(bigC) == 0 && count(nugget) == 75);
	cmd(PANEL_CHEST_CLOSE);
	run(2.5f);
	CHECK(app.chestStage() == App::CH_NONE && app.panel().page() == InventoryPanel::PAGE_PLAY);
	CHECK(std::string(app.triggers().stats().last) == std::string("-") + lidEvent); // (shut again)

	// 3. The locked chest with a key: the key in the pack opens it at once.
	before(keyC);
	CHECK(app.chestTarget() == int(keyC));
	pressA();
	run(3.0f);
	CHECK(!chests.locked(keyC) && app.chestStage() == App::CH_OPEN);
	cmd(PANEL_CHEST_ALL);
	CHECK(count(potion) == 5 && chests.stackCount(keyC) == 0);
	cmd(PANEL_CHEST_CLOSE);
	run(2.5f);
	CHECK(app.chestStage() == App::CH_NONE);

	// 4. The combination: right first is wrong (the pick breaks, back to the start),
	// then left, right, left opens it. (Gothic 1's rule, as OpenGothic has it: a wrong turn breaks the pick unless a die of
	// 100 is below 100 less the talent's value; the talent at 100 means it always breaks.)
	g.npcMut(hero).talent[5] = 100;
	before(pickC);
	CHECK(app.chestTarget() == int(pickC));
	pressA();
	run(0.5f);
	CHECK(app.chestStage() == App::CH_LOCK && app.panel().page() == InventoryPanel::PAGE_LOCK);
	{
		const InventoryView& v = app.inventory();
		CHECK(v.chest.lock && v.chest.length == 3 && v.chest.picks == 3 && v.chest.progress == 0);
	}
	cmd(PANEL_LOCK_PICK, 0); // L
	CHECK(app.inventory().chest.progress == 1);
	cmd(PANEL_LOCK_PICK, 0); // L again: wrong, the pick breaks
	CHECK(count(pick) == 2 && app.inventory().chest.progress == 0 && app.chestStage() == App::CH_LOCK);
	cmd(PANEL_LOCK_PICK, 0);
	cmd(PANEL_LOCK_PICK, 1);
	CHECK(chests.locked(pickC));
	cmd(PANEL_LOCK_PICK, 0);
	CHECK(!chests.locked(pickC) && count(pick) == 2);
	run(3.0f);
	CHECK(app.chestStage() == App::CH_OPEN && chestHas(pickC, nugget) == 20);
	cmd(PANEL_CHEST_CLOSE);
	run(2.5f);
	CHECK(app.chestStage() == App::CH_NONE);

	// 5. Without the key and the pick (the saved state has them locked again): it stays shut.
	CHECK(app.loadState(statePath.c_str()));
	run(0.5f);
	CHECK(chests.locked(keyC) && chests.locked(pickC));
	g.npcMut(hero).talent[5] = 100; // (a wrong turn breaks the pick for sure, as before)
	g.giveItems(hero, key, -count(key));
	g.giveItems(hero, pick, -count(pick));
	before(keyC);
	pressA();
	run(1.0f);
	CHECK(app.chestStage() == App::CH_NONE && chests.locked(keyC));
	before(pickC);
	pressA();
	run(1.0f);
	CHECK(app.chestStage() == App::CH_NONE && chests.locked(pickC));
	// ... and with only a few picks, all of them breaking: the last one goes, the lock stays.
	g.giveItems(hero, pick, 2);
	pressA();
	run(0.5f);
	CHECK(app.chestStage() == App::CH_LOCK);
	cmd(PANEL_LOCK_PICK, 1);
	cmd(PANEL_LOCK_PICK, 1);
	CHECK(count(pick) == 0 && app.chestStage() == App::CH_NONE && chests.locked(pickC));
	// ... but with the talent at 0 the chance to keep the pick is 100: a wrong turn only starts the lock over.
	g.npcMut(hero).talent[5] = 0;
	g.giveItems(hero, pick, 2);
	pressA();
	run(0.5f);
	CHECK(app.chestStage() == App::CH_LOCK);
	cmd(PANEL_LOCK_PICK, 1);
	cmd(PANEL_LOCK_PICK, 1);
	CHECK(count(pick) == 2 && app.chestStage() == App::CH_LOCK && app.inventory().chest.progress == 0);
	app.panel().openPage(InventoryPanel::PAGE_PLAY); // (the panel closes itself, then says so)
	cmd(PANEL_LOCK_CANCEL);
	run(0.5f);
	CHECK(app.chestStage() == App::CH_NONE && chests.locked(pickC));

	// 6. A ledge: a jump at the block of stone (150 cm) takes hold of it and climbs onto it.
	{
		const float faceZ = -700.0f - 106.0f; // (the block's south face)
		float blockTop = 0;
		{
			Vec3 nn;
			CHECK(app.world().rayDown({-700.0f, 3000.0f, -700.0f}, 5000.0f, blockTop, nn) && blockTop > 200.0f);
		}
		static const char* const kMid[3] = {"T_STAND_2_JUMPUPMID", "S_JUMPUPMID", "T_JUMPUPMID_2_STAND"};
		static const char* const kHang[3] = {"T_JUMP_2_HANG", "S_HANG", "T_HANG_2_STAND"};
		int up = 0, tried = 0;
		for (float start : {45.0f, 110.0f, 150.0f, 190.0f}) {
			const bool standing = start < 60.0f; // (right at the wall: the jump key alone)
			app.camera().yaw = 0.0f;
			app.teleport({-700.0f, 700.0f, faceZ - start});
			app.world().preload(app.camera().pos);
			run(1.5f);
			bool mantled = false;
			int seenMid = 0, seenHang = 0;
			bool onTop = false;
			for (int i = 0; i < 120; ++i) {
				const bool was = app.hero().mantling();
				Input in;
				in.moveZ = was || mantled || standing ? 0.0f : 1.0f; // (once up, it stands)
				in.jump = i < 20 && !mantled;
				app.update(in, dt);
				mantled = mantled || app.hero().mantling();
				{
					const Vec3 pf = app.player().feet;
					onTop = onTop || (app.player().grounded && std::fabs(pf.y - blockTop) < 15.0f && pf.z > faceZ + 10.0f && pf.z < faceZ + 212.0f);
				}
				const Clip* c = app.hero().animator().current();
				for (int k = 0; k < 3; ++k)
					if (c && c == app.hero().clip(kMid[k])) seenMid |= 1 << k;
				for (int k = 0; k < 3; ++k)
					if (c && c == app.hero().clip(kHang[k])) seenHang |= 1 << k;
				if (mantled && !app.hero().mantling() && was) break;
			}
			run(0.4f);
			const Vec3 f = app.player().feet;
			std::printf("  (climbed through: middle %d of 7, hang %d of 7)\n", seenMid, seenHang);
			if (standing) CHECK(seenMid == 7); // (from the ground: the original's middle climb, all three steps)
			std::printf("  a jump from %.0f cm: mantled %d, feet %.0f %.0f %.0f (the block's top is %.0f)\n", double(start), int(mantled), double(f.x),
			            double(f.y), double(f.z), double(blockTop));
			++tried;
			up += onTop; // (from a run the jump clears it and lands on it, or the hands take hold)
		}
		CHECK(up >= 3 && tried == 4);
		// The high one: the jump key right at it hangs from the edge and climbs on.
		{
			float top2 = 0;
			Vec3 nn;
			CHECK(app.world().rayDown({-1100.0f, 3000.0f, -700.0f}, 5000.0f, top2, nn));
			app.camera().yaw = 0.0f;
			app.teleport({-1100.0f, 700.0f, faceZ - 45.0f});
			run(1.5f);
			const float y0 = app.player().feet.y;
			int seenHang = 0;
			bool mantled = false;
			// The pose is fitted to the ledge: never the soles above its surface (the body floating over the
			// edge), never the hands off it while they hold it.
			float worstSole = -1e9f, handMiss = 0.0f;
			int probed = 0;
			for (int i = 0; i < 150; ++i) {
				Input in;
				in.jump = i < 3;
				app.update(in, dt);
				mantled = mantled || app.hero().mantling();
				float hy, sy;
				if (app.hero().mantling() && app.hero().mantleProbe(hy, sy)) {
					++probed;
					worstSole = std::max(worstSole, sy - top2);
					if (app.hero().mantleHandsWeight() > 0.95f) handMiss = std::max(handMiss, std::fabs(hy - top2));
				}
				const Clip* c = app.hero().animator().current();
				for (int k = 0; k < 3; ++k)
					if (c && c == app.hero().clip(kHang[k])) seenHang |= 1 << k;
			}
			const Vec3 f = app.player().feet;
			std::printf("  the high ledge (%.0f cm): mantled %d, hang %d of 7, feet %.0f %.0f %.0f\n", double(top2 - y0), int(mantled), seenHang,
			            double(f.x), double(f.y), double(f.z));
			CHECK(mantled && seenHang == 7 && std::fabs(f.y - top2) < 15.0f && app.player().grounded);
			std::printf("  fitted pose: %d frames, soles at most %.1f cm over the surface, hands off the edge by %.1f cm\n", probed, double(worstSole),
			            double(handMiss));
			CHECK(probed > 10 && worstSole < 14.0f && handMiss < 6.0f);
		}
		// The wall too tall: straight up and down again, not forward.
		{
			app.camera().yaw = 0.0f;
			app.teleport({-1500.0f, 700.0f, faceZ - 45.0f});
			run(1.5f);
			const Vec3 f0 = app.player().feet;
			float highest = f0.y;
			bool upClip = false, mantled = false;
			for (int i = 0; i < 60; ++i) {
				Input in;
				in.jump = i < 3;
				app.update(in, dt);
				highest = std::max(highest, app.player().feet.y);
				mantled = mantled || app.hero().mantling();
				const Clip* c = app.hero().animator().current();
				upClip = upClip || (c && (c == app.hero().clip("T_STAND_2_JUMPUP") || c == app.hero().clip("S_JUMPUP")));
			}
			const Vec3 f = app.player().feet;
			const float moved = std::sqrt((f.x - f0.x) * (f.x - f0.x) + (f.z - f0.z) * (f.z - f0.z));
			std::printf("  at the tall wall: up %.0f cm, moved %.0f cm, the jump-up pose %d, mantled %d\n", double(highest - f0.y), double(moved),
			            int(upClip), int(mantled));
			CHECK(!mantled && upClip && highest - f0.y > 40.0f && moved < 20.0f && app.player().grounded);
		}
	}
}


// fix190: beasts turn to the player with the game's own threat (ZS_MM_AssessEnemy of Gothic 1, as written there:
// B_MM_AssessEnemy starts it; its loop turns the beast (AI_TurnToNpc) and growls (T_WARN) while the player is within
// its threat range, and attacks after AIV_MM_DrohTime). The player comes up behind a scavenger on its way along the net:
// within two seconds it faces him. (Before fix190 Npc_ClearAIQueue left the walk in hand going and the beast went on
// with its back to him.)
static void testBeastTurn() {
	std::string source = syntheticScriptSource();
	const std::string old = "func void B_MM_AssessEnemy() {\n\tmm_enemy_seen += 1;\n\tNpc_SetTarget(self, other);\n\tAI_StartState(self, ZS_MM_Attack, 0, \"\");\n};";
	CHECK(source.find(old) != std::string::npos);
	if (source.find(old) == std::string::npos) return;
	source.replace(source.find(old), old.size(), R"(
const int AIV_MM_Behaviour = 30;
const int AIV_MM_PercRange = 31;
const int AIV_MM_DrohRange = 32;
const int AIV_MM_AttackRange = 33;
const int AIV_MM_DrohTime = 34;
const int PASSIVE = 1;
const int HUNTER = 2;
const int PACKHUNTER = 3;
var int mm_assess_loops;
extern func int Npc_CanSeeNpc(var C_NPC a, var C_NPC b);
func void B_MM_AssessEnemy() {
	mm_enemy_seen += 1;
	if ((Npc_GetDistToNpc(self, other) < self.aivar[AIV_MM_PercRange]) && Npc_CanSeeNpcFreeLOS(self, other)) {
		if (Wld_GetGuildAttitude(self.guild, other.guild) != ATT_HOSTILE) { return; };
		Npc_ClearAIQueue(self);
		AI_StartState(self, ZS_MM_AssessEnemy, 0, "");
	};
};
func void ZS_MM_AssessEnemy() {
	Npc_SetPercTime(self, 2);
	Npc_PercEnable(self, PERC_ASSESSDAMAGE, B_MM_ReactToDamage);
	if (self.aivar[AIV_MM_Behaviour] == HUNTER) {
		AI_StandUp(self);
		AI_TurnToNpc(self, other);
		AI_PlayAni(self, "T_WARN");
		AI_SetWalkmode(self, NPC_WALK);
	};
	if ((self.aivar[AIV_MM_Behaviour] == PASSIVE) || (self.aivar[AIV_MM_Behaviour] == PACKHUNTER)) {
		AI_StandUp(self);
		AI_LookAtNpc(self, other);
	};
};
func int ZS_MM_AssessEnemy_loop() {
	mm_assess_loops += 1;
	if (Npc_GetDistToNpc(self, other) > self.aivar[AIV_MM_PercRange]) { return 1; };
	if ((Npc_GetDistToNpc(self, other) <= self.aivar[AIV_MM_PercRange]) && (Npc_GetDistToNpc(self, other) > self.aivar[AIV_MM_DrohRange])) {
		if ((self.aivar[AIV_MM_Behaviour] == PASSIVE) || (self.aivar[AIV_MM_Behaviour] == PACKHUNTER)) {
			if (!Npc_CanSeeNpc(self, other)) { AI_TurnToNpc(self, other); };
		};
		if (self.aivar[AIV_MM_Behaviour] == HUNTER) { AI_GotoNpc(self, other); };
	};
	if ((Npc_GetDistToNpc(self, other) <= self.aivar[AIV_MM_DrohRange]) && (Npc_GetDistToNpc(self, other) > self.aivar[AIV_MM_AttackRange])) {
		if ((self.aivar[AIV_MM_Behaviour] == PASSIVE) || (self.aivar[AIV_MM_Behaviour] == PACKHUNTER)) {
			if (Npc_GetStateTime(self) > self.aivar[AIV_MM_DrohTime]) {
				Npc_SetTarget(self, other);
				Npc_ClearAIQueue(self);
				AI_StartState(self, ZS_MM_Attack, 0, "");
			} else {
				AI_TurnToNpc(self, other);
				AI_PlayAni(self, "T_WARN");
				Npc_SendPassivePerc(self, PERC_ASSESSWARN, other, self);
			};
		};
		if (self.aivar[AIV_MM_Behaviour] == HUNTER) { AI_GotoNpc(self, other); };
	} else {
		Npc_SetStateTime(self, 0);
	};
	if (Npc_GetDistToNpc(self, other) <= self.aivar[AIV_MM_AttackRange]) {
		Npc_SetTarget(self, other);
		Npc_ClearAIQueue(self);
		AI_StandUp(self);
		AI_StartState(self, ZS_MM_Attack, 0, "");
	};
	return 0;
};
func void ZS_MM_AssessEnemy_end() {
	AI_StopLookAt(self);
};)");
	const std::string proto = "\tguild = GIL_SCAVENGER;\n\taivar[AIV_WASDEFEATED] = FALSE;";
	CHECK(source.find(proto) != std::string::npos);
	if (source.find(proto) == std::string::npos) return;
	source.replace(source.find(proto), proto.size(), proto + "\n\taivar[AIV_MM_Behaviour] = PASSIVE; aivar[AIV_MM_PercRange] = 1200; aivar[AIV_MM_DrohRange] = 1000;\n\taivar[AIV_MM_AttackRange] = 700; aivar[AIV_MM_DrohTime] = 5;");
	// Roaming is a long walk to and fro (the original's AI_GotoWP along the net): the beast is mostly on its way.
	{
		const std::string roam = "\t\tif (Wld_IsNextFPAvailable(self, \"FP_ROAM\")) {\n\t\t\tAI_GotoNextFP(self, \"FP_ROAM\");\n\t\t\tAI_PlayAni(self, \"T_WARN\");\n\t\t};";
		CHECK(source.find(roam) != std::string::npos);
		if (source.find(roam) != std::string::npos)
			source.replace(source.find(roam), roam.size(),
			               "\t\tif (Npc_GetDistToWP(self, \"ROAD_1\") < 300) { AI_GotoWP(self, \"FOREST_SCAVENGER_3\"); } else { AI_GotoWP(self, \"ROAD_1\"); };");
	}
	// Only the player is its enemy here (the people of the camp would be, too: the nearest is threatened).
	{
		const std::string att = "\tWld_SetGuildAttitude(GIL_SCAVENGER, ATT_HOSTILE, GIL_NONE);";
		CHECK(source.find(att) != std::string::npos);
		if (source.find(att) != std::string::npos)
			source.replace(source.find(att), att.size(), att + "\n\tWld_SetGuildAttitude(GIL_SCAVENGER, ATT_NEUTRAL, GIL_GRD);\n\tWld_SetGuildAttitude(GIL_SCAVENGER, ATT_NEUTRAL, GIL_VLK);");
	}
	std::vector<BakeWorld> worlds;
	const std::string path = gDir + "/beastturn.pak";
	bakeLifeWorlds(source, path, worlds);
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr));
	if (!app.npcs().loaded() || !app.hero().loaded()) return;
	app.world().preload(app.camera().pos);
	Npcs& npcs = app.npcs();
	Game& g = npcs.game();
	g.setQuiet(true);
	app.settings().fights = 1;
	app.settings().perception = 1;
	app.settings().immortal = 1;
	app.applySettings();
	npcs.config().range = 20000.0f;
	app.environment().timeOfDay = 12.0f;
	const float dt = 1.0f / 30.0f;
	const auto run = [&](float seconds) {
		for (int i = 0; i < int(seconds / dt); ++i) app.update(Input {}, dt);
	};
	const auto global = [&](const char* name) { return g.vm().getInt(g.vm().find(name)); };
	// What the growl asks the sound engine for (the fallback names, when the clip has no tag): never "SCAVENGER.MDS_WARN".
	static std::string asked;
	asked.clear();
	Npcs::NamedSoundHook spy;
	spy.fn = [](void*, const char* names, int, float) { asked += names; asked += ';'; };
	spy.user = nullptr;
	npcs.setNamedSoundHook(spy);
	app.teleport({-7000.0f, 900.0f, -7000.0f});
	run(3.0f);
	int b = -1;
	for (u32 i = 0; i < g.npcCount() && b < 0; ++i)
		if (g.isMonster(g.npc(i)) && std::strcmp(g.nameOf(g.npc(i)), "Scavenger") == 0 && g.npc(i).placed && g.npc(i).world == g.currentWorld()) b = int(i);
	CHECK(b >= 0);
	if (b < 0) return;
	const GameNpc& bn = g.npc(u32(b));
	const GameNpc& hn = *g.hero();
	// Behind it, 9 m off: within its threat range (10 m), out of its attack range (7 m).
	for (int round = 0; round < 2; ++round) {
		const float behind = round == 0 ? 900.0f : 1100.0f;
		// Caught on its way (the player out of the reach of its senses until then).
		app.teleport({bn.pos[0] + 3500.0f, bn.pos[1] + 200.0f, bn.pos[2]});
		for (int k = 0; k < 600; ++k) {
			const NpcActor* w = npcs.actorOf(u32(b));
			if (w && w->phase == NpcActor::MOVING) break;
			if (getenv("G3_TRACE") && k % 30 == 0 && w)
				std::printf("   [wait %d] %s act %d ph %d q %d at %.0f %.0f t %.1f\n", k, npcs.stateName(w->state), int(w->action.kind), int(w->phase), int(w->queueCount), double(bn.pos[0]), double(bn.pos[2]), double(w->stateTime));
			app.update(Input {}, dt);
		}
		{
			const NpcActor* w = npcs.actorOf(u32(b));
			CHECK(w && w->phase == NpcActor::MOVING);
		}
		app.teleport({bn.pos[0] - std::sin(bn.yaw) * behind, bn.pos[1] + 10.0f, bn.pos[2] - std::cos(bn.yaw) * behind});
		const i32 loops0 = global("MM_ASSESS_LOOPS");
		float err = 9.0f;
		float tm = 0;
		for (; tm < 4.0f; tm += dt) {
			app.update(Input {}, dt);
			const float want = std::atan2(hn.pos[0] - bn.pos[0], hn.pos[2] - bn.pos[2]);
			err = std::fabs(std::remainder(want - bn.yaw, 6.2831853f));
			const NpcActor* a = npcs.actorOf(u32(b));
			if (getenv("G3_TRACE") && int(tm / dt) % 3 == 0)
				std::printf("   [%.0f cm, %.1f s] %s act %d ph %d q %d err %.2f loops %d yaw %.2f at %.0f %.0f hero %.0f %.0f\n", double(behind), double(tm), a ? npcs.stateName(a->state) : "-",
				            a ? int(a->action.kind) : -1, a ? int(a->phase) : -1, a ? int(a->queueCount) : -1, double(err), int(global("MM_ASSESS_LOOPS")), double(bn.yaw), double(bn.pos[0]), double(bn.pos[2]), double(hn.pos[0]), double(hn.pos[2]));
			if (getenv("G3_TRACE") && int(tm / dt) % 3 == 0 && a)
				std::printf("      other %s act.i %d pt %.2f\n", a->other >= 0 ? g.nameOf(g.npc(u32(a->other))) : "-", int(a->action.i), double(a->percTime));
			if (err < 0.3f) break;
		}
		const NpcActor* a = npcs.actorOf(u32(b));
		std::printf("  beast turn, player %.0f cm behind a walking beast: faced him after %.1f s (error %.2f rad), state %s, %d loops, %u walks cut short\n", double(behind),
		            double(tm), double(err), a ? npcs.stateName(a->state) : "-", int(global("MM_ASSESS_LOOPS") - loops0), npcs.stats().queueAborts);
		CHECK(err < 0.3f && tm < 2.0f);
		app.teleport({-7000.0f, 900.0f, -7000.0f});
		run(6.0f);
	}
	std::printf("  the growl asks for: %s\n", asked.c_str());
	CHECK(asked.find("SCAVENGER_WARN") != std::string::npos && asked.find(".MDS") == std::string::npos);
}

// The real scripts of Gothic 1, compiled from their sources (G3_G1_SCRIPT: one file, see the handoff), on the synthetic
// world: a scavenger at day (ZS_MM_Rtn_EatGround) and a wolf; the player comes up behind each, 9 m off. It must turn
// to face him (ZS_MM_AssessEnemy: AI_TurnToNpc + T_WARN) within a few seconds. Skipped without the file.
static void testG1Beasts() {
	const char* file = getenv("G3_G1_SCRIPT");
	if (!file) {
		std::printf("  (no G3_G1_SCRIPT: skipped)\n");
		return;
	}
	std::ifstream in(file);
	std::stringstream ss;
	ss << in.rdbuf();
	std::string source = ss.str() + R"(
func void STARTUP_WORLD() {
	Kapitel = 1;
	Wld_InsertNpc(Scavenger, "FOREST_SCAVENGER_1");
	Wld_InsertNpc(Molerat, "FOREST_MOLERAT_1");
};
func void INIT_WORLD() {
	B_InitMonsterAttitudes();
	B_InitGuildAttitudes();
};
)";
	std::vector<BakeWorld> worlds;
	const std::string path = gDir + "/g1beasts.pak";
	try {
		bakeLifeWorlds(source, path, worlds);
	} catch (const std::exception& e) {
		std::printf("  bake failed: %s\n", e.what());
		CHECK(false);
		return;
	}
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr));
	if (!app.npcs().loaded() || !app.hero().loaded()) return;
	app.world().preload(app.camera().pos);
	Npcs& npcs = app.npcs();
	Game& g = npcs.game();
	g.setQuiet(true);
	app.settings().fights = 1;
	app.settings().perception = 1;
	app.settings().immortal = 1;
	app.applySettings();
	npcs.config().range = 20000.0f;
	app.environment().timeOfDay = 12.0f;
	const float dt = 1.0f / 30.0f;
	const auto run = [&](float seconds) {
		for (int i = 0; i < int(seconds / dt); ++i) app.update(Input {}, dt);
	};
	app.teleport({-7000.0f, 900.0f, -7000.0f});
	run(4.0f);
	const GameNpc& hn = *g.hero();
	for (const char* who : {"Scavenger", "Molerat"}) {
		int b = -1;
		for (u32 i = 0; i < g.npcCount() && b < 0; ++i)
			if (g.isMonster(g.npc(i)) && std::strcmp(g.nameOf(g.npc(i)), who) == 0 && g.npc(i).placed && g.npc(i).world == g.currentWorld()) b = int(i);
		std::printf("  %s: %s\n", who, b >= 0 ? "placed" : "missing");
		CHECK(b >= 0);
		if (b < 0) continue;
		const GameNpc& bn = g.npc(u32(b));
		{
			const NpcActor* a = npcs.actorOf(u32(b));
			std::printf("   before: actor %d ready %d rig %d, state %s, att to hero %d, guild %d\n", a ? 1 : 0, a ? int(a->ready) : -1, a ? int(a->rig != nullptr) : -1, a ? npcs.stateName(a->state) : "-", int(g.attitude(u32(b), u32(&hn - &g.npc(0)))), int(g.guildOf(bn)));
		}
		const float behind = 900.0f;
		app.teleport({bn.pos[0] - std::sin(bn.yaw) * behind, bn.pos[1] + 10.0f, bn.pos[2] - std::cos(bn.yaw) * behind});
		float err = 9.0f, tm = 0;
		for (; tm < 6.0f; tm += dt) {
			app.update(Input {}, dt);
			const float want = std::atan2(hn.pos[0] - bn.pos[0], hn.pos[2] - bn.pos[2]);
			err = std::fabs(std::remainder(want - bn.yaw, 6.2831853f));
			const NpcActor* a = npcs.actorOf(u32(b));
			if (getenv("G3_TRACE") && int(tm / dt) % 3 == 0)
				std::printf("   [%s %.1f s] %s act %d ph %d q %d err %.2f dist %.0f stance %s perc %u pw %.2f\n", who, double(tm), a ? npcs.stateName(a->state) : "-",
				            a ? int(a->action.kind) : -1, a ? int(a->phase) : -1, a ? int(a->queueCount) : -1, double(err),
				            double(std::hypot(hn.pos[0] - bn.pos[0], hn.pos[2] - bn.pos[2])), a ? a->stance : "", npcs.stats().perceived, a ? double(a->percWait) : -1.0);
			if (err < 0.3f) break;
		}
		const NpcActor* a = npcs.actorOf(u32(b));
		std::printf("   noModel %u waiting %u actors %u pos %.0f %.0f %.0f hero %.0f %.0f %.0f\n", npcs.stats().noModel, npcs.stats().waiting, npcs.stats().actors, double(bn.pos[0]), double(bn.pos[1]), double(bn.pos[2]), double(hn.pos[0]), double(hn.pos[1]), double(hn.pos[2]));
		std::printf("  G1 %s, player 9 m behind: error %.2f rad after %.1f s, state %s\n", who, double(err), double(tm), a ? npcs.stateName(a->state) : "-");
		CHECK(err < 0.3f);
		app.teleport({-7000.0f, 900.0f, -7000.0f});
		run(8.0f);
	}
}

// The inhabitants' wits: a walker stops to be talked to, creatures notice the player at once and
// from every side and answer a blow, and a person does not fall on somebody with no quarrel.
static void testNpcAi() {
	std::string source = syntheticScriptSource();
	// A talk that does not clear the queue first, as the game's own B_AssessTalk: the engine must stop the walker.
	{
		const std::string clear = "\tNpc_ClearAIQueue(self);\n\tAI_StartState(self, ZS_Talk, 1, \"\");";
		CHECK(source.find(clear) != std::string::npos);
		if (source.find(clear) != std::string::npos) source.replace(source.find(clear), clear.size(), "\tAI_StartState(self, ZS_Talk, 1, \"\");");
	}
	const std::string hook = "func void STARTUP_SUB_CAMP() {";
	CHECK(source.find(hook) != std::string::npos);
	source.replace(source.find(hook), hook.size(), hook + "\n\tWld_InsertNpc(VLK_503_Trouble, \"CAMP_SQUARE\");\n\tWld_InsertNpc(VLK_504_Wanderer, \"CAMP_FIRE\");");
	source += R"(
var int trouble_loops;
func void ZS_Troublemaker() { Npc_SetPercTime(self, 1); AI_GotoWP(self, self.wp); };
func int ZS_Troublemaker_Loop() {
	trouble_loops += 1;
	other = hero;
	AI_StartState(self, ZS_Attack, 0, "");
	AI_Wait(self, 1);
	return LOOP_CONTINUE;
};
func void ZS_Troublemaker_End() {};
func void ZS_Wanderer() {
	Npc_SetPercTime(self, 1);
	Npc_PercEnable(self, PERC_ASSESSTALK, B_AssessTalk);
	AI_SetWalkmode(self, NPC_WALK);
	AI_GotoWP(self, "ROAD_1");
};
func int ZS_Wanderer_Loop() {
	if (Npc_GetDistToWP(self, "ROAD_1") < 300) { AI_GotoWP(self, "CAMP_FIRE"); } else { AI_GotoWP(self, "ROAD_1"); };
	return LOOP_CONTINUE;
};
func void ZS_Wanderer_End() {};
instance VLK_504_Wanderer (Npc_Default) {
	name = "Wanderer"; guild = GIL_VLK; id = 504; senses_range = 3000;
	Mdl_SetVisual(self, "HUMANS.MDS");
	Mdl_SetVisualBody(self, "hum_body_Naked0", 3, 1, "Hum_Head_Pony", 9, 2, -1);
	daily_routine = Rtn_start_504;
};
func void Rtn_start_504() {
	TA_Min(self, 0, 0, 24, 0, ZS_Wanderer, "CAMP_FIRE");
};
instance VLK_503_Trouble (Npc_Default) {
	name = "Trouble"; guild = GIL_VLK; id = 503; senses_range = 3000;
	Mdl_SetVisual(self, "HUMANS.MDS");
	Mdl_SetVisualBody(self, "hum_body_Naked0", 3, 1, "Hum_Head_Pony", 9, 2, -1);
	daily_routine = Rtn_start_503;
};
func void Rtn_start_503() {
	TA_Min(self, 0, 0, 24, 0, ZS_Troublemaker, "CAMP_SQUARE");
};
)";
	std::vector<BakeWorld> worlds;
	const std::string path = gDir + "/npcai.pak";
	bakeLifeWorlds(source, path, worlds);
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr));
	if (!app.npcs().loaded() || !app.hero().loaded()) return;
	app.world().preload(app.camera().pos);
	Npcs& npcs = app.npcs();
	Game& g = npcs.game();
	g.setQuiet(true);
	npcs.config().range = 20000.0f;
	app.settings().fights = 0;
	app.settings().perception = 0;
	app.settings().immortal = 1;
	app.applySettings();
	npcs.config().range = 20000.0f;
	const u32 hero = u32(g.hero() - &g.npc(0));
	const float dt = 1.0f / 30.0f;
	const auto run = [&](float seconds) {
		for (int i = 0; i < int(seconds / dt); ++i) app.update(Input {}, dt);
	};
	const auto global = [&](const char* name) { return g.vm().getInt(g.vm().find(name)); };
	const auto byName = [&](const char* name) {
		for (u32 i = 0; i < g.npcCount(); ++i)
			if (std::strcmp(g.nameOf(g.npc(i)), name) == 0) return int(i);
		return -1;
	};
	const int thorus = byName("Wanderer"); // (whoever walks, and listens for talk)
	const int trouble = byName("Trouble");
	CHECK(thorus >= 0 && trouble >= 0);
	if (thorus < 0 || trouble < 0) return;
	g.npcMut(u32(trouble)).attitude = i8(g.constant("ATT_FRIENDLY", 3)); // (no quarrel with the player)
	const auto dbg = [&](const char* what, u32 npc) {
		if (!getenv("G3_TRACE")) return;
		const NpcActor* a = npcs.actorOf(npc);
		std::printf("   [%s] %s: %s phase %d state %s pos %.0f %.0f\n", what, g.nameOf(g.npc(npc)), a ? "actor" : "no actor", a ? int(a->phase) : -1,
		            a ? npcs.stateName(a->state) : "-", double(g.npc(npc).pos[0]), double(g.npc(npc).pos[2]));
	};
	std::vector<u32> beasts;
	for (u32 i = 0; i < g.npcCount(); ++i)
		if (g.isMonster(g.npc(i))) beasts.push_back(i);
	CHECK(beasts.size() >= 2);
	if (beasts.size() < 2) return;
	// The most solitary ones first: the others are busy with the guards.
	const auto lonely = [&](u32 b) {
		float best = 1e30f;
		for (u32 i = 0; i < g.npcCount(); ++i)
			if (!g.isMonster(g.npc(i)) && g.npc(i).placed && i != hero)
				best = std::min(best, std::hypot(g.npc(i).pos[0] - g.npc(b).pos[0], g.npc(i).pos[2] - g.npc(b).pos[2]));
		return best;
	};
	std::sort(beasts.begin(), beasts.end(), [&](u32 a, u32 b) { return lonely(a) > lonely(b); });
	if (getenv("G3_TRACE"))
		for (u32 b : beasts) std::printf("   beast %u at %.0f %.0f, %.0f cm from the nearest person\n", b, double(g.npc(b).pos[0]), double(g.npc(b).pos[2]), double(lonely(b)));
	const auto stand = [&](const GameNpc& at, float behind, float yawOfHero) {
		// The player `behind` cm behind a character (the way it faces), looking along `yawOfHero`.
		app.teleport({at.pos[0] - std::sin(at.yaw) * behind, at.pos[1] + 10.0f, at.pos[2] - std::cos(at.yaw) * behind});
		app.camera().yaw = yawOfHero;
	};

	// 1. Whoever is on his way is stopped to be talked to: the talk begins within a moment and he stands.
	app.environment().timeOfDay = 12.0f;
	run(2.0f);
	for (int k = 0; k < 20 && !(npcs.actorOf(u32(thorus)) && npcs.actorOf(u32(thorus))->phase == NpcActor::MOVING); ++k) run(1.0f);
	dbg("walker", u32(thorus));
	{
		const NpcActor* t = npcs.actorOf(u32(thorus));
		CHECK(t && t->phase == NpcActor::MOVING);
		const GameNpc& tn = g.npc(u32(thorus));
		run(1.5f); // (he has turned on to his way)
		// Ahead of him on his way (where he went in the last second), facing him, a few paces off.
		const float x0 = tn.pos[0], z0 = tn.pos[2];
		run(1.0f);
		float dx = tn.pos[0] - x0, dz = tn.pos[2] - z0;
		const float len = std::max(std::hypot(dx, dz), 1.0f);
		dx /= len;
		dz /= len;
		app.teleport({tn.pos[0] + dx * 550.0f, tn.pos[1] + 10.0f, tn.pos[2] + dz * 550.0f});
		app.camera().yaw = std::atan2(-dx, -dz);
		bool targeted = false;
		for (float tm = 0; tm < 6.0f && !targeted; tm += dt) {
			app.update(Input {}, dt);
			targeted = app.talkTarget() == thorus;
			if (getenv("G3_TRACE") && int(tm / dt) % 15 == 0)
				std::printf("   [t %.1f] hero %.0f %.0f yaw %.2f grounded %d | guard %.0f %.0f yaw %.2f can %d target %d\n", double(tm),
				            double(app.player().feet.x), double(app.player().feet.z), double(app.hero().yaw()), int(app.player().grounded),
				            double(g.npc(u32(thorus)).pos[0]), double(g.npc(u32(thorus)).pos[2]), double(g.npc(u32(thorus)).yaw),
				            int(npcs.canTalk(u32(thorus))), app.talkTarget());
		}
		CHECK(targeted);
		CHECK(npcs.actorOf(u32(thorus)) && npcs.actorOf(u32(thorus))->phase == NpcActor::MOVING);
		Input a;
		a.confirm = true;
		const u32 talksBefore = npcs.stats().talks;
		app.update(a, dt);
		bool began = false;
		for (float tm = 0; tm < 2.0f && !began; tm += dt) { // (he had a long way to go: it must not wait for that)
			if (npcs.talkView().saying) npcs.talkSkip();
			app.update(Input {}, dt);
			began = npcs.stats().talks > talksBefore;
			if (getenv("G3_TRACE") && int(tm / dt) % 6 == 0) {
				const NpcActor* q = npcs.actorOf(u32(thorus));
				std::printf("   [talk %.2f] requested %d active %d | guard phase %d action %d queue %d state %s\n", double(tm), int(npcs.talking()), int(began),
				            q ? int(q->phase) : -1, q ? int(q->action.kind) : -1, q ? int(q->queueCount) : -1, q ? npcs.stateName(q->state) : "-");
			}
		}
		CHECK(began);
		CHECK(npcs.stats().talksStopped >= 1 && npcs.stats().talks >= 1);
		npcs.talkAbort();
		run(0.5f);
	}

	// 2. Beasts: asleep, with their scripts listening for nothing, a blow still brings one at the attacker.
	app.settings().fights = 1;
	app.settings().perception = 1;
	app.applySettings();
	npcs.config().range = 20000.0f;
	app.environment().timeOfDay = 23.5f;
	app.teleport({-7000.0f, 900.0f, -7000.0f});
	run(14.0f); // (the player is far away: whatever was after him gives up, and they fall asleep)
	{
		const u32 b = beasts[0];
		// (fix182: a beast asleep within the reach of its senses wakes to the player who does not creep - so the sleeper is put
		// outside it for this check, then the blow.)
		stand(g.npc(b), 2500.0f, 0.0f);
		run(1.0f);
		dbg("sleeper", b);
		const NpcActor* a = npcs.actorOf(b);
		CHECK(a && a->state == g.vm().find("ZS_MM_RTN_SLEEP"));
		stand(g.npc(b), 250.0f, 0.0f); // (and now the one that is struck is by the player)
		const u32 before = npcs.stats().provoked;
		npcs.strike(hero, b, false);
		run(0.3f);
		a = npcs.actorOf(b);
		CHECK(npcs.stats().provoked == before + 1 || (a && a->state == g.vm().find("ZS_MM_ATTACK")));
		CHECK(a && a->state == g.vm().find("ZS_MM_ATTACK") && g.npc(b).target == int(hero));
	}

	// (The people about are out of the way, so that the creatures have only the player to see.)
	for (u32 i = 0; i < g.npcCount(); ++i)
		if (i != hero && i != u32(trouble) && !g.isMonster(g.npc(i))) {
			g.setAttribute(g.npcMut(i), 0, 0);
			g.npcMut(i).down = DOWN_DEAD;
		}
	// 3. By day the creatures roam; the player appears BEHIND one, a few paces away: it notices him within
	// a moment, though he is nowhere in front of it.
	app.environment().timeOfDay = 12.0f;
	app.teleport({-7000.0f, 900.0f, -7000.0f});
	run(1.0f);
	run(10.0f);
	{
		const u32 b = beasts[1];
		app.teleport({g.npc(b).pos[0] + 5000.0f, g.npc(b).pos[1] + 10.0f, g.npc(b).pos[2]});
		run(3.0f); // (its models are read, its state has begun)
		dbg("roamer", b);
		const NpcActor* a = npcs.actorOf(b);
		CHECK(a && a->ready && a->state == g.vm().find("ZS_MM_RTN_ROAM"));
		const i32 seen = global("MM_ENEMY_SEEN");
		stand(g.npc(b), 600.0f, 0.0f);
		for (float tm = 0; tm < 0.7f; tm += dt) app.update(Input {}, dt);
		CHECK(global("MM_ENEMY_SEEN") > seen);
		// It goes after him; he runs, and it gives the chase up in the end (its clock goes on while he runs).
		bool chasing = false, gaveUp = false;
		for (float tm = 0; tm < 40.0f && !gaveUp; tm += dt) {
			Input run1;
			run1.moveZ = 1.0f;
			// (Every second a pack-mate warns it: its state is asked for again, and the clock of the chase must not start anew.)
			if (chasing && int(tm / dt) % 30 == 15) npcs.sendPerception("PERC_ASSESSWARN", hero, int(hero), -1);
			app.update(run1, dt);
			const NpcActor* q = npcs.actorOf(b);
			const bool now = q && q->state == g.vm().find("ZS_MM_ATTACK");
			chasing = chasing || now;
			gaveUp = chasing && !now;
			if (getenv("G3_TRACE") && int(tm / dt) % 30 == 0)
				std::printf("   [chase %.0f] beast %s t%.1f | hero speed %.0f\n", double(tm), q ? npcs.stateName(q->state) : "-", q ? double(q->stateTime) : -1.0,
				            double(app.hero().speed()));
		}
		CHECK(chasing);
		CHECK(gaveUp);
	}

	// 4. A person does not fall on somebody with no quarrel with him (and does once there is one).
	app.teleport({g.npc(u32(trouble)).pos[0] + 400.0f, g.npc(u32(trouble)).pos[1] + 10.0f, g.npc(u32(trouble)).pos[2]});
	app.environment().timeOfDay = 12.0f;
	dbg("trouble", u32(trouble));
	run(6.0f);
	{
		const NpcActor* t = npcs.actorOf(u32(trouble));
		CHECK(t && global("TROUBLE_LOOPS") > 0);
		CHECK(npcs.stats().attacksDenied > 0);
		CHECK(t && t->state == g.vm().find("ZS_TROUBLEMAKER"));
		g.npcMut(u32(trouble)).tempAttitude = i8(g.constant("ATT_HOSTILE", 0));
		run(4.0f);
		t = npcs.actorOf(u32(trouble));
		CHECK(t && t->state == g.vm().find("ZS_ATTACK"));
	}
	std::printf("       last attack: %s; denied %u, provoked %u, talks stopped %u\n", npcs.stats().lastAttack, npcs.stats().attacksDenied,
	            npcs.stats().provoked, npcs.stats().talksStopped);
}


// Things in the inhabitants' hands and in the world (npc_items.cpp, game_items.cpp): holding a potion
// in the state of its scheme and putting it away, drinking one, armour and weapons as the scripts ask
// about them, the inventory by slot, and a weapon found lying about and picked up.
// The dead can be searched: A before a body that carries something opens the
// chest page with its things; they only come out; the armour it wears stays.
static void testCorpses() {
	std::vector<BakeWorld> worlds;
	const std::string path = gDir + "/corpses.pak";
	bakeLifeWorlds(syntheticScriptSource(), path, worlds);
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr));
	if (!app.npcs().loaded() || !app.hero().loaded()) return;
	app.world().preload(app.camera().pos);
	Npcs& npcs = app.npcs();
	Game& g = npcs.game();
	g.setQuiet(true);
	app.settings().fights = 0;
	app.settings().perception = 0;
	app.settings().immortal = 1;
	app.applySettings();
	const u32 hero = u32(g.hero() - &g.npc(0));
	const float dt = 1.0f / 30.0f;
	const auto run = [&](float seconds) {
		for (int i = 0; i < int(seconds / dt); ++i) app.update(Input {}, dt);
	};
	const auto cmd = [&](u8 type, u8 slot = 0, u32 sym = 0, i32 amount = 0) {
		PanelCommand c;
		c.type = type;
		c.slot = slot;
		c.symbol = sym;
		c.amount = amount;
		app.panelCommand(c);
	};
	const u32 nugget = g.vm().find("ITMINUGGET"), apple = g.vm().find("ITFOAPPLE");
	CHECK(nugget != SCRIPT_NONE && apple != SCRIPT_NONE);
	run(1.0f);

	// A character of the camp (not the hero), put down for good with things on it.
	int body = -1;
	for (u32 i = 0; i < g.npcCount() && body < 0; ++i)
		if (i != hero && g.npc(i).placed && g.npc(i).world == g.currentWorld() && g.npc(i).down == DOWN_NONE) body = int(i);
	CHECK(body >= 0);
	if (body < 0) return;
	const u32 b = u32(body);
	g.giveItems(b, nugget, 12);
	g.giveItems(b, apple, 2);
	npcs.goDown(b, int(hero), true, false);
	run(2.0f);
	CHECK(g.npc(b).down == DOWN_DEAD);

	// Stands the character a pace from it and presses A.
	const auto before = [&]() {
		app.camera().yaw = 3.14159265f;
		app.teleport({g.npc(b).pos[0], g.npc(b).pos[1] + 220.0f, g.npc(b).pos[2] + 90.0f});
		run(1.5f);
	};
	before();
	CHECK(app.corpseTarget() == body && app.chestStage() == App::CH_NONE);
	{
		Input in;
		in.confirm = true;
		app.update(in, dt);
	}
	// (The page is up while the search animation is still going, not after it.)
	{
		const Clip* plunder = app.hero().clip("T_PLUNDER");
		CHECK(plunder != nullptr);
		float t = 0;
		while (!app.hero().mobOpen() && t < 4.0f) {
			app.update(Input {}, dt);
			t += dt;
		}
		CHECK(plunder && t > 0.2f && t < plunder->duration() * 0.9f);
	}
	run(1.0f);
	CHECK(app.chestStage() == App::CH_OPEN && app.corpseOpen() == body && app.panel().page() == InventoryPanel::PAGE_CHEST);
	{
		const InventoryView& v = app.inventory();
		CHECK(v.chest.open && v.chest.count >= 2 && v.chest.name[0]);
	}
	// Bent over the body the head is ahead of the hips (not behind them, nor to the side):
	// the hero is built to face +Z in model space.
	if (app.hero().stoop() > 0.9f) {
		const Model& hm = app.hero().model();
		static Mat34 world[MODEL_MAX_NODES];
		hm.worldTransforms(app.hero().pose(), world);
		const int head = hm.findNode(hashName("BIP01 HEAD")), pelvis = hm.findNode(hashName("BIP01 SPINE"));
		CHECK(head >= 0 && pelvis >= 0);
		if (head >= 0 && pelvis >= 0) {
			const float ahead = world[head].m[2][3] - world[pelvis].m[2][3];
			const float aside = std::fabs(world[head].m[0][3] - world[pelvis].m[0][3]);
			CHECK(ahead > 5.0f && aside < ahead);
		}
	}
	const i32 hn = g.itemCount(hero, nugget), ha = g.itemCount(hero, apple);
	// Out only: nothing goes in.
	cmd(PANEL_CHEST_MOVE, 1, apple, 1);
	CHECK(g.itemCount(hero, apple) == ha && g.itemCount(b, apple) == 2);
	cmd(PANEL_CHEST_MOVE, 0, nugget, 5);
	CHECK(g.itemCount(hero, nugget) == hn + 5 && g.itemCount(b, nugget) == 7);
	cmd(PANEL_CHEST_ALL);
	CHECK(g.itemCount(hero, nugget) == hn + 12 && g.itemCount(hero, apple) == ha + 2 && g.itemCount(b, nugget) == 0);
	cmd(PANEL_CHEST_CLOSE);
	run(2.5f);
	CHECK(app.chestStage() == App::CH_NONE && app.corpseOpen() < 0);
	// Searched out: no more to ask for.
	run(0.3f);
	CHECK(app.corpseTarget() < 0);

	// The memory for animations full (as after a long game on the console): the one
	// who falls still gets its fall - what has not been played for longest makes room.
	{
		static std::vector<std::string> heard;
		heard.clear();
		Npcs::NamedSoundHook h;
		h.fn = [](void*, const char* names, int, float) { heard.push_back(names); };
		npcs.setNamedSoundHook(h);
		int other = -1;
		for (u32 i = 0; i < g.npcCount() && other < 0; ++i)
			if (i != hero && i != b && !g.isMonster(g.npc(i)) && g.npc(i).placed && g.npc(i).world == g.currentWorld() &&
			    g.npc(i).down == DOWN_NONE && npcs.actorOf(i))
				other = int(i);
		CHECK(other >= 0);
		if (other >= 0) {
			const u32 o = u32(other);
			npcs.config().clipBudget = npcs.stats().clipBytes; // (no room left at all)
			const u32 evictedBefore = npcs.stats().clipsEvicted;
			npcs.strike(hero, o, false);
			npcs.goDown(o, int(hero), true, false);
			run(3.0f);
			const NpcActor* oa = npcs.actorOf(o);
			CHECK(oa && std::strncmp(oa->stance, "DEAD", 4) == 0 && !oa->noLying);
			CHECK(oa && oa->rig &&
			      (oa->anim.current() == oa->rig->model.clip("S_DEAD") || oa->anim.current() == oa->rig->model.clip("S_DEADB") ||
			       oa->anim.current() == oa->rig->model.clip("T_DEAD") || oa->anim.current() == oa->rig->model.clip("T_DEADB")));
			CHECK(npcs.stats().clipsEvicted > evictedBefore);
			CHECK(npcs.stats().clipBytes <= npcs.config().clipBudget);
			// ... and the blow and the fall were heard.
			bool hit = false;
			for (const std::string& n : heard) hit = hit || n.find("CS_MAM") != std::string::npos || n.find("CS_IAM") != std::string::npos;
			CHECK(hit);
		}
		npcs.config().clipBudget = 1536u * 1024u;
		npcs.setNamedSoundHook(Npcs::NamedSoundHook {});
	}

	// A creature goes down the same way: it lies in its dead pose, and carries its trophies.
	int beast = -1;
	for (u32 i = 0; i < g.npcCount() && beast < 0; ++i)
		if (i != hero && g.isMonster(g.npc(i)) && g.npc(i).placed && g.npc(i).world == g.currentWorld() && g.npc(i).down == DOWN_NONE)
			beast = int(i);
	CHECK(beast >= 0);
	if (beast >= 0) {
		const u32 m = u32(beast);
		app.teleport({g.npc(m).pos[0], g.npc(m).pos[1] + 220.0f, g.npc(m).pos[2] + 300.0f});
		run(1.5f);
		g.giveItems(m, apple, 3);
		npcs.goDown(m, int(hero), true, false);
		run(4.0f);
		const NpcActor* a = npcs.actorOf(m);
		CHECK(g.npc(m).down == DOWN_DEAD && a);
		if (a) {
			CHECK(std::strncmp(a->stance, "DEAD", 4) == 0);
			CHECK(a->anim.current() != nullptr);
			// (It has a fall - read at once, though the blow came from outside the actors' update -
			// and lies in it; the synthetic creatures all have one.)
			CHECK(!a->noLying && a->rig && (a->anim.current() == a->rig->model.clip("S_DEAD") || a->anim.current() == a->rig->model.clip("T_DEAD")));
		}
		CHECK(g.itemCount(m, apple) == 3);
	}

	// Drawing the sword: the whole draw is played (the hands change in its middle,
	// not at the first frame), and it is heard.
	{
		static std::vector<std::string> heard;
		heard.clear();
		Npcs::NamedSoundHook h;
		h.fn = [](void*, const char* names, int, float) { heard.push_back(names); };
		npcs.setNamedSoundHook(h);
		before();
		CHECK(app.menuAction(MENU_GIVE_WEAPON, nullptr) && g.npc(hero).melee != SCRIPT_NONE);
		Input in;
		in.meleeSlot = true;
		app.update(in, dt);
		app.update(Input {}, dt);
		CHECK(app.hero().drawing() && app.hero().weapon() == WEAPON_NONE);
		float took = 0;
		while (app.hero().weapon() == WEAPON_NONE && took < 3.0f) {
			app.update(Input {}, dt);
			took += dt;
		}
		CHECK(app.hero().weapon() == WEAPON_1H && took > 0.15f);
		bool drew = false;
		for (const std::string& n : heard) drew = drew || n.find("DRAWSOUND") != std::string::npos;
		CHECK(drew);
		npcs.setNamedSoundHook(Npcs::NamedSoundHook {});
	}
}

static void testNpcItems() {
	std::string source = syntheticScriptSource();
	const std::string hook = "func void STARTUP_SUB_CAMP() {";
	CHECK(source.find(hook) != std::string::npos);
	source.replace(source.find(hook), hook.size(), hook + "\n\tWld_InsertNpc(VLK_505_Eater, \"CAMP_HUT_2\");");
	source += R"(
const int INV_WEAPON = 1;
const int INV_ARMOR = 2;
extern func void   AI_UseItem(var C_NPC n, var int item);
extern func void   AI_UseItemToState(var C_NPC n, var int item, var int state);
extern func void   AI_TakeItem(var C_NPC n, var C_ITEM item);
extern func void   AI_GotoItem(var C_NPC n, var C_ITEM item);
extern func int    Wld_DetectItem(var C_NPC n, var int flags);
extern func int    Npc_CanSeeItem(var C_NPC n, var C_ITEM item);
extern func int    Npc_GetDistToItem(var C_NPC n, var C_ITEM item);
extern func int    Hlp_IsItem(var C_ITEM item, var int instance);
extern func int    Hlp_IsValidItem(var C_ITEM item);
extern func C_ITEM Npc_GetEquippedArmor(var C_NPC n);
extern func C_ITEM Npc_GetEquippedMeleeWeapon(var C_NPC n);
extern func C_ITEM Npc_GetReadiedWeapon(var C_NPC n);
extern func int    Npc_HasEquippedArmor(var C_NPC n);
extern func C_ITEM Npc_GetInvItem(var C_NPC n, var int item);
extern func int    Npc_GetInvItemBySlot(var C_NPC n, var int category, var int slot);
extern func void   AI_EquipArmor(var C_NPC n, var C_ITEM armor);
extern func void   AI_EquipBestArmor(var C_NPC n);
extern func void   AI_UnequipArmor(var C_NPC n);
extern func void   AI_UnequipWeapons(var C_NPC n);
func void ZS_Idle() { AI_GotoWP(self, self.wp); };
func int ZS_Idle_Loop() { return LOOP_CONTINUE; };
func void ZS_Idle_End() {};
instance VLK_505_Eater (Npc_Default) {
	name = "Eater"; guild = GIL_VLK; id = 505; senses_range = 3000;
	Mdl_SetVisual(self, "HUMANS.MDS");
	Mdl_SetVisualBody(self, "hum_body_Naked0", 3, 1, "Hum_Head_Pony", 9, 2, -1);
	daily_routine = Rtn_start_505;
};
func void Rtn_start_505() { TA_Min(self, 0, 0, 24, 0, ZS_Idle, "CAMP_HUT_2"); };
func void T_Hold() { CreateInvItems(self, ItFo_Potion_Health_01, 2); AI_UseItemToState(self, ItFo_Potion_Health_01, 0); };
func void T_PutAway() { AI_UseItemToState(self, ItFo_Potion_Health_01, -1); };
func void T_Drink() { AI_UseItem(self, ItFo_Potion_Health_01); };
func void T_HoldAndGo() { AI_UseItemToState(self, ItFo_Potion_Health_01, 0); AI_GotoWP(self, "CAMP_SQUARE"); };
var int armor_best; var int armor_has; var int armor_none; var int armor_vlk;
func void T_Armor() {
	CreateInvItem(self, VLK_ARMOR_L);
	CreateInvItem(self, GRD_ARMOR_M);
	AI_EquipBestArmor(self);
	armor_best = Hlp_IsItem(Npc_GetEquippedArmor(self), GRD_ARMOR_M);
	armor_has = Npc_HasEquippedArmor(self);
	AI_UnequipArmor(self);
	armor_none = !Npc_HasEquippedArmor(self);
	AI_EquipArmor(self, VLK_ARMOR_L);
	var C_ITEM worn;
	worn = Npc_GetEquippedArmor(self);
	armor_vlk = Hlp_IsItem(worn, VLK_ARMOR_L);
};
var int slot_count; var int slot_sword; var int slot_third; var int inv_club; var int melee_sword; var int readied_none; var int unequipped;
func void T_Slots() {
	CreateInvItem(self, ItMw_Club);
	CreateInvItems(self, ItMw_Sword, 2);
	slot_count = Npc_GetInvItemBySlot(self, INV_WEAPON, 2);
	slot_sword = Hlp_IsItem(item, ItMw_Sword);
	slot_third = Npc_GetInvItemBySlot(self, INV_WEAPON, 3);
	var C_ITEM club;
	club = Npc_GetInvItem(self, ItMw_Club);
	inv_club = Hlp_IsItem(club, ItMw_Club) && Hlp_IsItem(item, ItMw_Club);
	EquipItem(self, ItMw_Sword);
	melee_sword = Hlp_IsItem(Npc_GetEquippedMeleeWeapon(self), ItMw_Sword);
	readied_none = !Hlp_IsValidItem(Npc_GetReadiedWeapon(self));
	AI_UnequipWeapons(self);
	unequipped = !Npc_HasEquippedMeleeWeapon(self);
};
var int fetch_found; var int fetch_id; var int fetch_dist; var int fetch_dist2; var int fetch_see;
func void T_Fetch() {
	if (Wld_DetectItem(self, ITEM_KAT_NF)) {
		fetch_found = 1;
		fetch_id = Hlp_GetInstanceID(item);
		fetch_dist = Npc_GetDistToItem(self, item);
		fetch_dist2 = Npc_GetDistToNpc(self, item);
		fetch_see = Npc_CanSeeItem(self, item);
		AI_TakeItem(self, item);
	};
};
)";
	std::vector<BakeWorld> worlds;
	const std::string path = gDir + "/npcitems.pak";
	bakeLifeWorlds(source, path, worlds);
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr));
	if (!app.npcs().loaded() || !app.hero().loaded()) return;
	app.world().preload(app.camera().pos);
	Npcs& npcs = app.npcs();
	Game& g = npcs.game();
	g.setQuiet(true);
	app.settings().fights = 0;
	app.settings().perception = 0;
	app.settings().immortal = 1;
	app.applySettings();
	npcs.config().range = 20000.0f;
	app.environment().timeOfDay = 12.0f;
	const float dt = 1.0f / 30.0f;
	const auto run = [&](float seconds) {
		for (int i = 0; i < int(seconds / dt); ++i) app.update(Input {}, dt);
	};
	const auto global = [&](const char* name) { return g.vm().getInt(g.vm().find(name)); };
	int eater = -1;
	for (u32 i = 0; i < g.npcCount(); ++i)
		if (std::strcmp(g.nameOf(g.npc(i)), "Eater") == 0) eater = int(i);
	CHECK(eater >= 0);
	if (eater < 0) return;
	const u32 e = u32(eater);
	{
		const GameNpc& n = g.npc(e);
		app.teleport({n.pos[0] + 300.0f, n.pos[1] + 10.0f, n.pos[2]});
	}
	run(3.0f);
	const NpcActor* a = npcs.actorOf(e);
	CHECK(a && a->ready && a->state == g.vm().find("ZS_IDLE"));
	if (!a) return;
	const u32 potion = g.vm().find("ITFO_POTION_HEALTH_01");
	const auto call = [&](const char* fn) { g.callAs(g.vm().find(fn), e, -1); };
	const i32 itemInteract = g.constant("BS_ITEMINTERACT", 14 | (1 << 15));

	// 1. Held: up to the mouth, then holding it (S_POTIONFAST_S0); nothing used up.
	call("T_HOLD");
	run(2.0f);
	a = npcs.actorOf(e);
	CHECK(a && a->heldItem == potion && a->itemState == 0);
	CHECK(a && std::strcmp(a->stance, "POTIONFAST_S0") == 0);
	CHECK(a && std::strcmp(npcs.animationName(*a), "S_POTIONFAST_S0") == 0);
	CHECK(a && npcs.bodyState(*a) == itemInteract);
	CHECK(g.itemCount(e, potion) == 2);
	CHECK(npcs.stats().itemsHeld == 1);
	{
		// Drawn with the potion in its hand (looking at it from where the player stands).
		app.camera().yaw = std::atan2(g.npc(e).pos[0] - app.player().feet.x, g.npc(e).pos[2] - app.player().feet.z);
		const u32 shown = npcs.stats().shown;
		app.update(Input {}, dt);
		app.render();
		CHECK(npcs.stats().shown >= 1 || shown >= 1);
	}
	// 2. ... and put away again.
	call("T_PUTAWAY");
	run(2.0f);
	a = npcs.actorOf(e);
	CHECK(a && a->heldItem == 0xFFFFFFFFu && a->itemState < 0 && a->stance[0] == 0);
	CHECK(a && npcs.bodyState(*a) != itemInteract);
	CHECK(g.itemCount(e, potion) == 2);
	// 3. Drunk: one less, and it heals.
	g.setAttribute(g.npcMut(e), 0, 10);
	call("T_DRINK");
	run(3.0f);
	a = npcs.actorOf(e);
	CHECK(g.itemCount(e, potion) == 1);
	CHECK(g.attributeOf(g.npc(e), 0) == std::min(60, std::max(g.attributeOf(g.npc(e), 1), 1))); // (up to its maximum)
	CHECK(g.attributeOf(g.npc(e), 0) > 10);
	CHECK(npcs.stats().itemsUsed == 1);
	CHECK(a && a->heldItem == 0xFFFFFFFFu && a->stance[0] == 0);
	// 4. Walking off with it in hand: it is put away first.
	{
		const float x0 = g.npc(e).pos[0], z0 = g.npc(e).pos[2];
		call("T_HOLDANDGO");
		bool held = false;
		for (float tm = 0; tm < 1.5f; tm += dt) {
			app.update(Input {}, dt);
			a = npcs.actorOf(e);
			held = held || (a && a->heldItem == potion && a->itemState == 0);
		}
		CHECK(held);
		run(5.0f);
		a = npcs.actorOf(e);
		CHECK(a && a->heldItem == 0xFFFFFFFFu && a->stance[0] == 0);
		CHECK(std::hypot(g.npc(e).pos[0] - x0, g.npc(e).pos[2] - z0) > 200.0f);
	}
	// 5. Armour: the best one put on, taken off, a given one put on; its protection counts.
	{
		const u32 prot = g.vm().findMember("C_NPC", "PROTECTION");
		const i32 before = g.npcInt(g.npc(e), prot, 1);
		call("T_ARMOR");
		CHECK(global("ARMOR_BEST") == 1 && global("ARMOR_HAS") == 1 && global("ARMOR_NONE") == 1 && global("ARMOR_VLK") == 1);
		CHECK(g.npc(e).armorItem == g.vm().find("VLK_ARMOR_L"));
		CHECK(g.npcInt(g.npc(e), prot, 1) == before + 10);
	}
	// 6. Weapons by slot, the one equipped, none in hand, all taken off.
	call("T_SLOTS");
	CHECK(global("SLOT_COUNT") == 2 && global("SLOT_SWORD") == 1 && global("SLOT_THIRD") == 0);
	CHECK(global("INV_CLUB") == 1 && global("MELEE_SWORD") == 1 && global("READIED_NONE") == 1 && global("UNEQUIPPED") == 1);
	// 7. A weapon lying about: found, seen, fetched.
	{
		const u32 takenBefore = app.loot().stats().taken;
		call("T_FETCH");
		CHECK(global("FETCH_FOUND") == 1);
		const u32 id = u32(global("FETCH_ID"));
		CHECK(id == g.vm().find("ITMW_SWORD") || id == g.vm().find("ITMW_CLUB"));
		CHECK(global("FETCH_DIST") > 0 && global("FETCH_DIST") < 3000 && std::abs(global("FETCH_DIST") - global("FETCH_DIST2")) <= 1);
		CHECK(global("FETCH_SEE") == 1);
		const i32 had = g.itemCount(e, id);
		run(10.0f);
		CHECK(app.loot().stats().taken == takenBefore + 1);
		CHECK(g.itemCount(e, id) == had + 1);
		CHECK(npcs.stats().itemsTaken == 1);
		// Gone from the ground: nothing more to find there for the next one.
		a = npcs.actorOf(e);
		CHECK(a && a->action.kind == NPC_ACT_NONE);
	}
	std::printf("       things: %u held, %u used, %u taken, %u missing\n", npcs.stats().itemsHeld, npcs.stats().itemsUsed,
	            npcs.stats().itemsTaken, npcs.stats().itemsMissing);
}

static void testRooms() {
	// Two rooms: a hut (two leaves, one flat over the floor) and a hall beside it; a box shared by nobody else.
	std::vector<RoomBox> boxes;
	const auto box = [&](float x0, float y0, float z0, float x1, float y1, float z1, u16 room) {
		RoomBox b {};
		b.bbMin[0] = x0, b.bbMin[1] = y0, b.bbMin[2] = z0, b.bbMax[0] = x1, b.bbMax[1] = y1, b.bbMax[2] = z1;
		b.room = room;
		boxes.push_back(b);
	};
	box(0, 0, 0, 500, 300, 500, 1);      // the hut
	box(100, 0, 100, 200, 2, 200, 1);    // its floor alone (flat)
	box(600, -50, 0, 3000, 600, 900, 2); // the hall
	box(5000, 0, 5000, 5400, 2, 5400, 2); // a flat leaf of the hall far away
	const char names[] = "HUT01\0OCC_HALL\0\0\0";
	const u32 offsets[2] = {0, 6};
	RoomsHeader h {};
	h.magic = ROOMS_MAGIC;
	h.version = ROOMS_VERSION;
	h.roomCount = 2;
	h.boxCount = u32(boxes.size());
	h.nameBytes = 16;
	std::vector<u8> blob(sizeof(h));
	std::memcpy(blob.data(), &h, sizeof(h));
	const u8* bp = reinterpret_cast<const u8*>(boxes.data());
	blob.insert(blob.end(), bp, bp + boxes.size() * sizeof(RoomBox));
	const u8* op = reinterpret_cast<const u8*>(offsets);
	blob.insert(blob.end(), op, op + sizeof(offsets));
	blob.insert(blob.end(), names, names + 16);
	Rooms rooms;
	CHECK(rooms.adopt(blob.data(), u32(blob.size())));
	CHECK(rooms.count() == 2);
	CHECK(rooms.roomAt({250, 0, 250}) == 1);
	CHECK(rooms.roomAt({1000, 0, 500}) == 2);
	CHECK(rooms.roomAt({5200, 0, 5200}) == 2); // (a flat leaf: found with room above it)
	CHECK(rooms.roomAt({-100, 0, 250}) == 0);
	CHECK(rooms.roomAt({250, 2000, 250}) == 0); // far above the roof: outside
	CHECK(std::strcmp(rooms.name(1), "HUT01") == 0 && std::strcmp(rooms.name(2), "OCC_HALL") == 0 && rooms.name(0)[0] == 0);
	CHECK(rooms.hash(1) == hashName("hut01") && rooms.hash(0) == 0);
	// Damaged: refused.
	blob[sizeof(h)] ^= 0xFF;
	blob.resize(blob.size() - 4);
	Rooms bad;
	CHECK(!bad.adopt(blob.data(), u32(blob.size())));
}


// fix152: a camera flight (zCCSCamera, OpenGothic's CsCamera): triggered, the camera goes through its keys in time,
// looking at its target; it ends by itself after the last key and its delay, or untriggered.
static void testCameraFlight() {
	std::vector<char> names(1, '\0');
	const auto name = [&](const char* n) {
		const u32 at = u32(names.size());
		names.insert(names.end(), n, n + std::strlen(n) + 1);
		return at;
	};
	std::vector<WorldTrigger> list;
	std::vector<MoverKey> keys;
	const auto camera = [&](const char* n, u8 behavior, bool target) {
		WorldTrigger t {};
		t.kind = TRG_CAMERA;
		t.behavior = behavior;
		t.flags = TRGF_START_ENABLED | TRGF_REACT_TRIGGER | TRGF_SEND_UNTRIGGER;
		t.name = name(n);
		t.maxActivations = -1;
		t.first = u32(keys.size());
		t.speed = 2.0f;
		t.stayOpen = 0.5f;
		keys.push_back(MoverKey {{0, 500, 0}, {0.0f, 1, 0, 1}});
		keys.push_back(MoverKey {{1000, 500, 0}, {1.0f, 1, 0, 1}});
		keys.push_back(MoverKey {{2000, 500, 0}, {2.0f, 1, 0, 1}});
		if (target) keys.push_back(MoverKey {{1000, 0, 1000}, {0.0f, 1, 0, 1}});
		t.count = u16(target ? 4 : 3);
		t.speedMode = target ? 1 : 0;
		list.push_back(t);
	};
	camera("CAM_TOUR", 1, true);   // ends by itself
	camera("CAM_FREE", 2, false);  // the player may move; ends when untriggered
	while (names.size() % 4) names.push_back('\0');
	TriggersHeader h {};
	h.magic = TRIGGERS_MAGIC;
	h.version = TRIGGERS_VERSION;
	h.count = u32(list.size());
	h.keyCount = u32(keys.size());
	h.nameBytes = u32(names.size());
	const size_t size = sizeof(h) + list.size() * sizeof(WorldTrigger) + keys.size() * sizeof(MoverKey) + names.size();
	u8* b = static_cast<u8*>(std::malloc(size));
	std::memcpy(b, &h, sizeof(h));
	std::memcpy(b + sizeof(h), list.data(), list.size() * sizeof(WorldTrigger));
	std::memcpy(b + sizeof(h) + list.size() * sizeof(WorldTrigger), keys.data(), keys.size() * sizeof(MoverKey));
	std::memcpy(b + sizeof(h) + list.size() * sizeof(WorldTrigger) + keys.size() * sizeof(MoverKey), names.data(), names.size());
	Game game;
	Triggers t;
	CHECK(t.adopt(b, u32(size), game));
	CHECK(t.count() == 2 && t.stats().cameras == 2 && !t.cutsceneActive());
	const Vec3 feet = {0, 0, 0};
	const auto step = [&](float seconds) {
		for (float x = 0; x < seconds - 1e-4f; x += 0.05f) t.update(0.05f, feet, game);
	};
	Vec3 pos, look;
	bool hasLook = false;
	CHECK(!t.cutscene(pos, look, hasLook));
	t.send("cam_tour", false);
	CHECK(t.cutsceneActive() && !t.cutsceneMovable() && t.stats().flights == 1);
	CHECK(t.cutscene(pos, look, hasLook) && std::fabs(pos.x) < 0.01f && hasLook && std::fabs(look.z - 1000.0f) < 0.01f);
	step(1.0f);
	CHECK(t.cutscene(pos, look, hasLook) && std::fabs(pos.x - 1000.0f) < 1.0f && std::fabs(pos.y - 500.0f) < 0.01f);
	step(0.5f);
	CHECK(t.cutscene(pos, look, hasLook) && pos.x > 1300.0f && pos.x < 1700.0f);
	step(0.6f); // past its last key: it holds there for its delay
	CHECK(t.cutscene(pos, look, hasLook) && std::fabs(pos.x - 2000.0f) < 0.01f);
	step(0.5f);
	CHECK(!t.cutsceneActive());
	// Without a target it looks the way it flies; untriggered, it ends.
	t.send("CAM_FREE", false);
	step(0.5f);
	CHECK(t.cutsceneActive() && t.cutsceneMovable() && t.cutscene(pos, look, hasLook) && hasLook && look.x > pos.x);
	step(3.0f);
	CHECK(t.cutsceneActive()); // (it does not end by itself)
	t.send("CAM_FREE", true);
	CHECK(!t.cutsceneActive() && t.stats().flights == 2);
	// One flight ends when another begins; a skip ends it at once.
	t.send("CAM_FREE", false);
	t.send("CAM_TOUR", false);
	CHECK(t.cutsceneActive() && !t.cutsceneMovable());
	t.cutsceneStop();
	CHECK(!t.cutsceneActive());
}

static void testTriggers() {
	// A gate (toggle) that, once open, triggers "AFTER", which opens a lift (trigger control); a trigger in the way
	// that toggles the gate once when walked into; a world start that fires the first time only.
	std::vector<char> names(1, '\0');
	const auto name = [&](const char* n) {
		const u32 at = u32(names.size());
		names.insert(names.end(), n, n + std::strlen(n) + 1);
		return at;
	};
	std::vector<WorldTrigger> list;
	std::vector<MoverKey> keys;
	std::vector<float> coll;
	// A wall of its own space: x -w..w, y 0..h, at z = 0 (two triangles).
	const auto wall = [&](WorldTrigger& t, float x0, float x1, float h) {
		t.flags = u16(t.flags | TRGF_COLLIDES);
		t.collFirst = u32(coll.size() / 9);
		t.collCount = 2;
		const float q[18] = {x0, 0, 0, x1, 0, 0, x1, h, 0, x0, 0, 0, x1, h, 0, x0, h, 0};
		coll.insert(coll.end(), q, q + 18);
	};
	const auto mover = [&](const char* n, const char* target, u8 behavior, float speed, float x1, float y1) {
		WorldTrigger t {};
		t.kind = TRG_MOVER;
		t.behavior = behavior;
		t.flags = TRGF_START_ENABLED | TRGF_REACT_TRIGGER | TRGF_SEND_UNTRIGGER;
		t.name = name(n);
		t.target = target ? name(target) : 0;
		t.visual = name("gate_mesh");
		t.maxActivations = -1;
		t.first = u32(keys.size());
		t.count = 2;
		t.lerp = 1;
		t.speed = speed;
		keys.push_back(MoverKey {{0, 0, 0}, {0, 0, 0, 1}});
		keys.push_back(MoverKey {{x1, y1, 0}, {0, 0, 0, 1}});
		if (y1 > 0.0f) wall(t, -100.0f, 100.0f, 300.0f); // (the gate is solid)
		list.push_back(t);
	};
	mover("GATE", "AFTER", 0, 1.0f, 0, 300);
	{
		WorldTrigger t {};
		t.kind = TRG_TRIGGER;
		t.flags = TRGF_START_ENABLED | TRGF_REACT_TRIGGER;
		t.name = name("AFTER");
		t.target = name("LIFT");
		t.maxActivations = -1;
		list.push_back(t);
	}
	mover("LIFT", nullptr, 1, 0.5f, 100, 0);
	{
		WorldTrigger t {};
		t.kind = TRG_TRIGGER;
		t.flags = TRGF_START_ENABLED | TRGF_REACT_TOUCH | TRGF_RESPOND_PC;
		t.target = name("GATE");
		t.maxActivations = 1;
		t.bbMin[0] = 900, t.bbMin[1] = -100, t.bbMin[2] = 900;
		t.bbMax[0] = 1100, t.bbMax[1] = 200, t.bbMax[2] = 1100;
		list.push_back(t);
	}
	{
		WorldTrigger t {};
		t.kind = TRG_WORLDSTART;
		t.flags = TRGF_FIRE_ONCE;
		t.target = name("STARTED");
		t.maxActivations = -1;
		list.push_back(t);
	}
	{
		// A locked door at x = 2000, hinged at its left end, with a key and a lock of two turns.
		WorldTrigger t {};
		t.kind = TRG_DOOR;
		t.flags = TRGF_START_ENABLED | TRGF_REACT_TRIGGER | TRGF_LOCKED;
		t.name = name("DOOR_1");
		t.visual = name("door_mesh");
		t.target = name("ITKE_TEST");
		t.sfx[0] = name("LR");
		t.maxActivations = -1;
		t.first = u32(keys.size());
		t.count = 2;
		keys.push_back(MoverKey {{2000, 0, 0}, {0, 0, 0, 1}});
		keys.push_back(MoverKey {{-50, 0, 0}, {0, 0, 0, 1}});
		wall(t, -50.0f, 50.0f, 200.0f);
		list.push_back(t);
	}
	while (names.size() % 4) names.push_back('\0');
	const auto blob = [&]() {
		TriggersHeader h {};
		h.magic = TRIGGERS_MAGIC;
		h.version = TRIGGERS_VERSION;
		h.count = u32(list.size());
		h.keyCount = u32(keys.size());
		h.nameBytes = u32(names.size());
		h.collisionTris = u32(coll.size() / 9);
		const size_t size = sizeof(h) + list.size() * sizeof(WorldTrigger) + keys.size() * sizeof(MoverKey) + coll.size() * 4 + names.size();
		u8* b = static_cast<u8*>(std::malloc(size));
		u8* p = b;
		std::memcpy(p, &h, sizeof(h));
		p += sizeof(h);
		std::memcpy(p, list.data(), list.size() * sizeof(WorldTrigger));
		p += list.size() * sizeof(WorldTrigger);
		std::memcpy(p, keys.data(), keys.size() * sizeof(MoverKey));
		p += keys.size() * sizeof(MoverKey);
		std::memcpy(p, coll.data(), coll.size() * 4);
		p += coll.size() * 4;
		std::memcpy(p, names.data(), names.size());
		return std::make_pair(b, u32(size));
	};
	Game game;
	Triggers t;
	auto b = blob();
	CHECK(t.adopt(b.first, b.second, game));
	CHECK(t.count() == 6 && t.stats().movers == 2 && t.stats().triggers == 3 && t.stats().doors == 1 && t.stats().solid == 2);
	// The shut gate is a wall: a sphere in it is pushed out, a line through it is stopped, a ray finds its top.
	{
		Vec3 c = {0, 100, 10};
		CHECK(t.pushSphere(c, 30.0f) && c.z >= 29.9f);
		CHECK(t.segmentHits({0, 100, -200}, {0, 100, 200}) && !t.segmentHits({500, 100, -200}, {500, 100, 200}));
		float y;
		Vec3 n;
		CHECK(!t.rayDown({0, 500, 0}, 1000.0f, y, n)); // (a wall standing up: nothing to stand on from above, only its edge)
	}
	CHECK(t.stats().unknown == 1); // ("STARTED": nobody of that name)
	const Vec3 away = {5000, 0, 5000}, inBox = {1000, 0, 1000};
	Quat r;
	Vec3 p;
	const auto step = [&](float seconds, Vec3 feet) {
		for (float x = 0; x < seconds - 1e-4f; x += 0.05f) t.update(0.05f, feet, game);
	};
	step(0.5f, away);
	CHECK(t.moverTransform(0, r, p) && std::fabs(p.y) < 0.01f && t.moverState(0) == 0);
	// The gate opens (300 ms), then the lift (200 ms).
	t.send("gate", false);
	step(0.15f, away);
	CHECK(t.moverTransform(0, r, p) && p.y > 100.0f && p.y < 200.0f && t.moverState(0) == 1);
	step(0.3f, away);
	CHECK(t.moverTransform(0, r, p) && std::fabs(p.y - 300.0f) < 0.01f && t.moverFrame(0) == 1);
	{
		// Up: the way under it is free.
		Vec3 c = {0, 100, 10};
		CHECK(!t.pushSphere(c, 30.0f) && !t.segmentHits({0, 100, -200}, {0, 100, 200}));
	}
	step(0.4f, away);
	CHECK(t.moverTransform(2, r, p) && std::fabs(p.x - 100.0f) < 0.01f && t.moverFrame(2) == 1 && t.moverState(2) == 0);
	// The lift closes on an untrigger; the gate does not (a toggle).
	t.send("LIFT", true);
	step(0.4f, away);
	CHECK(t.moverTransform(2, r, p) && std::fabs(p.x) < 0.01f && t.moverFrame(2) == 0);
	// Walked into: the gate toggles (closes) - once only.
	step(0.1f, inBox);
	step(0.5f, inBox);
	CHECK(t.moverTransform(0, r, p) && std::fabs(p.y) < 0.01f && t.moverFrame(0) == 0);
	step(0.1f, away);
	step(0.6f, inBox);
	CHECK(t.moverFrame(0) == 0 && t.moverState(0) == 0);
	// The scripts' Wld_SendTrigger: through the game.
	u32 kept = 0;
	CHECK(game.triggerState(0, 0, kept) && (kept & 0xFFFF) == 0);
	// (The gate, arriving closed, sent its target again: the lift went up once more - as OpenGothic's movers do.)
	CHECK(game.triggerState(0, 2, kept) && (kept & 0xFFFF) == 1 && t.moverFrame(2) == 1);
	t.send("GATE", false);
	step(0.6f, away);
	CHECK(t.moverFrame(0) == 1 && game.triggerState(0, 0, kept) && (kept & 0xFFFF) == 1);
	// Entered again: the gate is where it was left (open), the world start does not fire again.
	Triggers again;
	b = blob();
	CHECK(again.adopt(b.first, b.second, game));
	CHECK(again.moverTransform(0, r, p) && std::fabs(p.y - 300.0f) < 0.01f && again.moverFrame(0) == 1);
	CHECK(again.stats().unknown == 0);
	// The door: locked (it does not open), then unlocked; opened from in front it swings away and is no wall then.
	{
		Triggers& d = again;
		const u32 door = 5;
		CHECK(d.doorNear({2000, 0, 150}, 3.14159265f) == int(door) && d.doorLocked(door));
		CHECK(std::strcmp(d.doorKey(door), "ITKE_TEST") == 0 && std::strcmp(d.doorPick(door), "LR") == 0);
		d.doorToggle(door, {2000, 0, 150});
		CHECK(!d.doorOpen(door));
		Vec3 c = {2000, 100, 10};
		CHECK(d.pushSphere(c, 30.0f));
		d.doorUnlock(door);
		d.doorToggle(door, {2000, 0, 150});
		CHECK(d.doorOpen(door) && !d.doorLocked(door));
		for (int k = 0; k < 30; ++k) d.update(0.05f, {5000, 0, 5000}, game);
		c = {2000, 100, 10};
		CHECK(!d.pushSphere(c, 30.0f));
		// It swung away from the opener (to -z) about its hinge at x 1950.
		CHECK(d.moverTransform(door, r, p) && std::fabs(p.x - 1950.0f) < 1.0f && std::fabs(p.z + 50.0f) < 1.0f);
		// Kept: unlocked and open.
		CHECK(game.triggerState(0, door, kept) && (kept & 1u) && ((kept >> 29) & 1u));
		// An inhabitant coming by a shut door: it opens, and shuts after it has gone.
		d.doorToggle(door, {2000, 0, 150});
		for (int k = 0; k < 30; ++k) d.update(0.05f, {5000, 0, 5000}, game);
		CHECK(!d.doorOpen(door));
		const Vec3 passer = {2000, 0, -100};
		d.update(0.05f, {5000, 0, 5000}, game, &passer, 1);
		CHECK(d.doorOpen(door));
		for (int k = 0; k < 100; ++k) d.update(0.05f, {5000, 0, 5000}, game);
		CHECK(!d.doorOpen(door));
	}
	// Damaged: refused.
	b = blob();
	b.first[sizeof(TriggersHeader)] = 99; // (a kind that does not exist)
	Triggers bad;
	CHECK(!bad.adopt(b.first, b.second, game));
}

// Bodies: the player's character does not walk through an inhabitant.
static void testBodies() {
	std::vector<BakeWorld> worlds;
	const std::string path = gDir + "/bodies.pak";
	bakeLifeWorlds(syntheticScriptSource(), path, worlds);
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr));
	if (!app.npcs().loaded()) return;
	app.settings().scheme = CONTROLS_CONSOLE;
	app.settings().immortal = 1;
	app.settings().fights = 0;
	app.settings().perception = 0;
	app.applySettings();
	Npcs& npcs = app.npcs();
	Game& g = npcs.game();
	g.setQuiet(true);
	const float dt = 1.0f / 30.0f;
	int scav = -1;
	for (u32 i = 0; i < g.npcCount(); ++i)
		if (g.npc(i).placed && g.npc(i).world == g.currentWorld() && std::strcmp(g.nameOf(g.npc(i)), "Scavenger") == 0) scav = int(i);
	CHECK(scav >= 0);
	if (scav < 0) return;
	const GameNpc& e = g.npc(u32(scav));
	app.teleport({e.pos[0], e.pos[1] + 400.0f, e.pos[2] - 400.0f});
	app.world().preload(app.camera().pos);
	for (int i = 0; i < 60; ++i) app.update(Input {}, dt);
	const NpcActor* a = npcs.actorOf(u32(scav));
	CHECK(a != nullptr);
	if (!a) return;
	float closest = 1e9f;
	for (int i = 0; i < 30 * 4; ++i) {
		Input in;
		const Vec3 feet = app.player().feet;
		const float want = std::atan2(e.pos[0] - feet.x, e.pos[2] - feet.z) - app.camera().yaw;
		in.moveX = std::sin(want);
		in.moveZ = std::cos(want);
		app.update(in, dt);
		const Vec3 f = app.player().feet;
		closest = std::min(closest, std::sqrt((f.x - e.pos[0]) * (f.x - e.pos[0]) + (f.z - e.pos[2]) * (f.z - e.pos[2])));
	}
	std::printf("  walked into a scavenger: closest %.0f cm (bodies %.0f + %.0f)\n", double(closest), double(app.player().config.radius),
	            double(a->bodyRadius));
	CHECK(closest >= app.player().config.radius + a->bodyRadius - 25.0f); // (a frame's step may reach in before it is put out)
	CHECK(closest < 200.0f);
}

// Overlays (Mdl_ApplyOverlayMds): Thorus wears the militia's; its standing and going stand in for the plain ones;
// the player's character takes one on and off; a timed one runs out; a save keeps them.
static void testOverlays() {
	std::vector<BakeWorld> worlds;
	const std::string path = gDir + "/overlays.pak";
	const BakeResult r = bakeLifeWorlds(syntheticScriptSource(), path, worlds);
	(void)r;
	{
		Pak pak;
		CHECK(pak.open(path.c_str()) && pak.find("model/ovl.humans_militia") && !pak.find("model/ovl.humans_nobody_names"));
	}
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr));
	if (!app.npcs().loaded()) return;
	app.settings().immortal = 1;
	app.settings().fights = 0;
	app.settings().perception = 0;
	app.applySettings();
	Npcs& npcs = app.npcs();
	Game& g = npcs.game();
	g.setQuiet(true);
	const float dt = 1.0f / 30.0f;
	int thorus = -1;
	for (u32 i = 0; i < g.npcCount(); ++i)
		if (std::strcmp(g.nameOf(g.npc(i)), "Thorus") == 0) thorus = int(i);
	CHECK(thorus >= 0);
	if (thorus < 0) return;
	const GameNpc& t = g.npc(u32(thorus));
	CHECK(t.overlayCount() == 1 && std::strcmp(g.text(t.overlay[0]), "humans_militia") == 0);
	const Model* ovl = npcs.overlayModel(t.overlay[0]);
	CHECK(ovl && ovl->clip("S_RUN") && ovl->clip("S_WALKL"));
	if (!ovl) return;
	app.teleport({t.pos[0], t.pos[1] + 400.0f, t.pos[2] - 300.0f});
	app.world().preload(app.camera().pos);
	int plain = 0, worn = 0;
	for (int i = 0; i < 30 * 6; ++i) {
		if (i == 60) {
			// (He guards: told to play his standing, he plays the militia's.)
			ScriptVm& vm = g.vm();
			const u32 ext = vm.find("AI_PLAYANI");
			if (ext != SCRIPT_NONE) {
				vm.pushInstance(t.object);
				vm.pushString("S_RUN");
				vm.call(ext);
			}
		}
		app.update(Input {}, dt);
		const NpcActor* a = npcs.actorOf(u32(thorus));
		if (!a || !a->rig || !a->anim.current()) continue;
		const Clip* c = a->anim.current();
		for (const char* name : {"S_RUN", "S_WALKL", "S_RUNL"}) {
			if (c == a->rig->model.clip(name)) ++plain;
			if (c == ovl->clip(name)) ++worn;
		}
	}
	if (const NpcActor* a = npcs.actorOf(u32(thorus)))
		std::printf("  Thorus is in %s (%s) cur %p ready %d stance '%s' rigS_RUN %p ovlS_RUN %p\n", npcs.animationName(*a), npcs.doing(*a),
		            (const void*)a->anim.current(), int(a->ready), a->stance, a->rig ? (const void*)a->rig->model.clip("S_RUN") : nullptr,
		            (const void*)ovl->clip("S_RUN"));
	else
		std::printf("  Thorus is no actor (%.0f %.0f %.0f)\n", double(t.pos[0]), double(t.pos[1]), double(t.pos[2]));
	std::printf("  Thorus: %d frames in the militia's clips, %d in the plain ones\n", worn, plain);
	CHECK(worn > 0 && plain == 0);
	// The player's character: on, its standing is the overlay's; off again, the model's.
	const u32 hero = g.heroIndex();
	g.applyOverlay(hero, "Humans_Militia.MDS");
	for (int i = 0; i < 20; ++i) app.update(Input {}, dt);
	CHECK(app.hero().animator().current() == ovl->clip("S_RUN"));
	g.removeOverlay(hero, "HUMANS_MILITIA.mds");
	for (int i = 0; i < 20; ++i) app.update(Input {}, dt);
	CHECK(app.hero().animator().current() != ovl->clip("S_RUN"));
	// Timed: gone after its time.
	g.applyOverlay(hero, "humans_militia", 0.5f);
	CHECK(g.npc(hero).overlayCount() == 1);
	for (int i = 0; i < 30; ++i) app.update(Input {}, dt);
	CHECK(g.npc(hero).overlayCount() == 0);
	// Kept in a save, given back by it.
	g.applyOverlay(hero, "humans_militia");
	ByteSink sink;
	g.save(sink);
	g.removeOverlay(hero, "humans_militia");
	ByteSource src(sink.data, sink.size);
	CHECK(g.restore(src) && g.npc(hero).overlayCount() == 1 && g.npc(u32(thorus)).overlayCount() == 1);
}

// The sun's shadow, baked (sun_bake.h): a floor and a roof on posts' height above it; under the roof's shadow on the floor
// the mask is dark, under the roof itself (the roof takes the sun) and far from it light; the masks round-trip.
static void testSunShadow() {
	// TEX_L4 round trip: what is written is read back at the same place (the console's layout, rows from the bottom).
	{
		std::vector<u8> v(16 * 8);
		for (u32 i = 0; i < v.size(); ++i) v[i] = u8((i * 7u + i / 16u) & 15u);
		const std::vector<u8> t = encodeL4(v, 16, 8);
		TexHeader h;
		std::memcpy(&h, t.data(), sizeof(h));
		CHECK(texValidate(h, u32(t.size() - sizeof(h))) && h.format == TEX_L4 && (h.flags & TEXF_CLAMP) && h.dataSize == 64);
		std::vector<u8> rgba(16 * 8 * 4);
		texDecodeLevel(h, t.data() + sizeof(h), 0, rgba.data());
		bool same = true;
		for (u32 i = 0; i < v.size(); ++i) same = same && rgba[i * 4] == v[i] * 17 && rgba[i * 4 + 3] == 255;
		CHECK(same);
	}
	SrcScene scene;
	scene.materials.resize(2);
	auto quad = [&](float x0, float z0, float x1, float z1, float y, u32 material) {
		auto tri = [&](const float a[3], const float b[3], const float c[3]) {
			SrcTriangle t {};
			const float* p[3] = {a, b, c};
			for (int k = 0; k < 3; ++k)
				for (int i = 0; i < 3; ++i) t.v[k].pos[i] = p[k][i];
			t.material = material;
			scene.triangles.push_back(t);
		};
		const float a[3] = {x0, y, z0}, b[3] = {x1, y, z0}, c[3] = {x0, y, z1}, d[3] = {x1, y, z1};
		tri(a, b, c);
		tri(b, d, c);
	};
	quad(0, 0, 4000, 4000, 0, 0);            // the floor
	quad(1800, 1800, 2200, 2200, 1000, 0);   // a roof 10 m up
	quad(500, 3000, 900, 3400, 400, 1);      // leaves (half a shadow, take none)
	std::vector<SunMask> cells(1);
	cells[0].x0 = 0;
	cells[0].z0 = 0;
	cells[0].x1 = 4000;
	cells[0].z1 = 4000;
	SunOptions opt;
	opt.enabled = true;
	opt.texel = 40.0f;
	SunStats st;
	const std::vector<u8> kind = {0, 1};
	const std::vector<SunMask> masks = bakeSunMasks(scene, kind, cells, opt, st);
	CHECK(masks.size() == 1 && masks[0].width == 128 && masks[0].height == 128 && st.empty == 0);
	if (masks.size() != 1) return;
	TexHeader h;
	std::memcpy(&h, masks[0].texture.data(), sizeof(h));
	std::vector<u8> rgba(size_t(h.width) * h.height * 4);
	texDecodeLevel(h, masks[0].texture.data() + sizeof(h), 0, rgba.data());
	auto valueAt = [&](float x, float z) {
		const int u = int(x / 4000.0f * float(h.width)), v = int(z / 4000.0f * float(h.height));
		return rgba[(size_t(v) * h.width + size_t(u)) * 4] / 17;
	};
	float sun[3];
	sunDirectionAt(opt.hour, sun);
	// Where the roof's middle throws its shadow on the floor.
	const float sx = 2000.0f - sun[0] * 1000.0f / sun[1], sz = 2000.0f - sun[2] * 1000.0f / sun[1];
	CHECK(std::fabs(sx - 2000.0f) > 300.0f || std::fabs(sz - 2000.0f) > 300.0f); // (beside the roof, not under it)
	const float lx = 900.0f - sun[0] * 400.0f / sun[1] - 200.0f, lz = 3200.0f - sun[2] * 400.0f / sun[1];
	const int under = valueAt(2000, 2000), shade = valueAt(sx, sz), open = valueAt(3700, 300), leaves = valueAt(lx + 200.0f, lz);
	std::printf("  sun's shadow: the roof %d, its shadow on the floor at %.0f %.0f %d, the open floor %d, under the leaves' shadow %d "
	            "(of 15); %.1f M rays\n", under, double(sx), double(sz), shade, open, leaves, double(st.rays) / 1e6);
	CHECK(under == 15 && shade <= 1 && open == 15 && leaves >= 5 && leaves <= 10);

	// In the game: a world baked with it, on the PC's renderer. With the menu's "Sun shadows" the ground under the
	// huts' and trees' shade gets darker, nothing lighter; off, or at night, or indoors, nothing changes.
	std::vector<BakeWorld> worlds;
	const std::string path = gDir + "/sun.pak";
	bakeLifeWorlds(syntheticScriptSource(), path, worlds, true, true);
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr));
	CHECK(app.world().hasSunMasks() && std::fabs(app.world().sunMaskHour() - 14.5f) < 0.01f && gfx::sunShadowSupported());
	if (!app.world().hasSunMasks()) return;
	app.skipChapters();
	app.npcs().game().setQuiet(true);
	app.weather().configure(WEATHER_CLEAR, true);
	app.weather().settle();
	app.environment().timeOfDay = 14.5f;
	app.settings().timeFlow = 0; // (the time stands)
	app.applySettings();
	app.world().preload(app.camera().pos);
	for (int i = 0; i < 30; ++i) app.update(Input {}, 1.0f / 30.0f);
	app.environment().timeOfDay = 14.5f;
	CHECK(app.sunShadowDarkness() > 0.4f);
	// fix146: the menu's strength - each step darker than the one before.
	{
		const float normal = app.sunShadowDarkness();
		app.settings().sunShadows = 2;
		app.applySettings();
		const float strong = app.sunShadowDarkness();
		app.settings().sunShadows = 3;
		app.applySettings();
		const float strongest = app.sunShadowDarkness();
		CHECK(strong > normal && strongest > strong && strongest <= 0.85f + 1e-4f);
		app.settings().sunShadows = 1;
		app.applySettings();
	}
	app.settings().sunShadows = 0;
	app.applySettings();
	CHECK(app.sunShadowDarkness() == 0.0f);
	app.render();
	const std::vector<u8> off(gfx::softFramebuffer(), gfx::softFramebuffer() + gfx::SCREEN_W * gfx::SCREEN_H * 4);
	app.settings().sunShadows = 1;
	app.applySettings();
	app.render();
	app.render(); // (the masks are asked for when first drawn, and arrive at once without the background loader)
	const u8* on = gfx::softFramebuffer();
	u32 darker = 0, lighter = 0;
	for (u32 i = 0; i < u32(gfx::SCREEN_W * gfx::SCREEN_H); ++i) {
		const int d = int(on[i * 4]) + on[i * 4 + 1] + on[i * 4 + 2] - (int(off[i * 4]) + off[i * 4 + 1] + off[i * 4 + 2]);
		darker += d < -6;
		lighter += d > 3;
	}
	app.environment().timeOfDay = 23.0f;
	const float night = app.sunShadowDarkness();
	app.environment().timeOfDay = 9.0f;
	const float morning = app.sunShadowDarkness();
	std::printf("  in the game: %u pixels darker in the sun's shadow, %u lighter; darkness %.2f at 14:30, %.2f at 9:00, %.2f at night\n",
	            darker, lighter, 0.42, double(morning), double(night));
	CHECK(darker > 200 && lighter == 0 && night == 0.0f && morning < 0.05f);
}

// fix186: the clouds' shadows.
static void testClouds() {
	// The field: the share asked for is under a cloud (roughly), the edges are soft, and it drifts with the wind.
	Clouds c;
	c.set(0.3f, 0.5f, 100.0);
	u32 under = 0, total = 0;
	float worstStep = 0.0f;
	for (int j = 0; j < 200; ++j)
		for (int i = 0; i < 200; ++i) {
			const float x = float(i) * 2300.0f - 50000.0f, z = float(j) * 2100.0f - 50000.0f;
			const float l = c.light(x, z);
			under += l < 0.5f;
			++total;
			worstStep = std::max(worstStep, std::fabs(c.light(x + 100.0f, z) - l));
		}
	const float share = float(under) / float(total);
	std::printf("  clouds: %.0f%% of the land under a cloud (asked 30%%), at most %.3f of light per metre\n", double(share * 100.0f), double(worstStep));
	CHECK(share > 0.15f && share < 0.45f && worstStep < 0.12f);
	Clouds later;
	later.set(0.3f, 0.5f, 110.0);
	float moved = 0.0f;
	for (int i = 0; i < 50; ++i) {
		const float x = float(i) * 1300.0f, z = float(i) * -700.0f;
		moved = std::max(moved, std::fabs(later.light(x + Clouds::WIND_X * 10.0f, z + Clouds::WIND_Z * 10.0f) - c.light(x, z)));
	}
	CHECK(moved < 0.02f);
	// The darkness: what is left of the light; none with no darkness.
	CHECK(std::fabs(c.shade(0, 0) - (1.0f - 0.5f * (1.0f - c.light(0, 0)))) < 1e-5f);
	Clouds none;
	none.set(0.9f, 0.0f, 5.0);
	CHECK(!none.active() && none.shade(1234, 5678) == 1.0f);
	// The weather: by day in fair weather, none at night, in rain or when it is overcast.
	float cov = 0, dark = 0;
	Clouds::fromWeather(0.0f, 0.0f, 0.8f, cov, dark);
	CHECK(dark > 0.3f && cov > 0.15f && cov < 0.3f);
	Clouds::fromWeather(0.0f, 0.0f, -0.3f, cov, dark);
	CHECK(dark == 0.0f);
	Clouds::fromWeather(0.3f, 1.0f, 0.8f, cov, dark);
	CHECK(dark == 0.0f);
	Clouds::fromWeather(1.0f, 0.0f, 0.8f, cov, dark);
	CHECK(dark < 0.01f);

	// In the game (the world of the sun's test, on the PC's renderer): the masks are written with the clouds in them, the
	// ground under a cloud is darker, nothing is lighter; at night nothing is written; off, the masks are as baked again.
	const std::string path = gDir + "/sun.pak";
	{
		std::vector<BakeWorld> worlds;
		bakeLifeWorlds(syntheticScriptSource(), path, worlds, true, true);
	}
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr));
	if (!app.world().hasSunMasks()) return;
	app.skipChapters();
	app.npcs().game().setQuiet(true);
	app.weather().configure(WEATHER_CLEAR, true);
	app.weather().settle();
	app.settings().timeFlow = 0;
	app.settings().sunShadows = 1;
	app.settings().cloudShadows = 0;
	app.applySettings();
	app.environment().timeOfDay = 10.0f; // (the sun's shadow is all but gone so far from the hour baked: what darkens is the clouds)
	app.world().preload(app.camera().pos);
	for (int i = 0; i < 30; ++i) app.update(Input {}, 1.0f / 30.0f);
	app.environment().timeOfDay = 10.0f;
	app.render();
	app.render();
	const std::vector<u8> off(gfx::softFramebuffer(), gfx::softFramebuffer() + gfx::SCREEN_W * gfx::SCREEN_H * 4);
	CHECK(app.world().cloudMasksWritten() == 0);
	app.settings().cloudShadows = 1;
	app.applySettings();
	app.setCloudCoverage(1.0f); // (a cloud over all of it)
	for (int i = 0; i < 40; ++i) app.render();
	const u32 written = app.world().cloudMasksWritten(), ready = app.world().cloudMasksReady();
	const u8* on = gfx::softFramebuffer();
	u32 darker = 0, lighter = 0;
	for (u32 i = 0; i < u32(gfx::SCREEN_W * gfx::SCREEN_H); ++i) {
		const int d = int(on[i * 4]) + on[i * 4 + 1] + on[i * 4 + 2] - (int(off[i * 4]) + off[i * 4 + 1] + off[i * 4 + 2]);
		darker += d < -6;
		lighter += d > 3;
	}
	const Vec3 cam = app.camera().pos;
	std::printf("  in the game: %u masks written (%u carry the clouds), %u pixels darker, %u lighter; the light left at the camera %.2f\n",
	            written, ready, darker, lighter, double(app.world().cloudShade(cam.x, cam.z)));
	CHECK(written > 0 && ready > 0 && darker > 2000 && lighter == 0 && app.world().cloudShade(cam.x, cam.z) < 0.8f);
	// At night: nothing drawn, nothing written.
	app.environment().timeOfDay = 23.0f;
	app.render();
	const u32 atNight = app.world().cloudMasksWritten();
	for (int i = 0; i < 10; ++i) app.render();
	CHECK(app.world().cloudMasksWritten() == atNight && app.world().cloudShade(cam.x, cam.z) == 1.0f);
	// Off: the masks as baked, the picture as before.
	app.environment().timeOfDay = 10.0f;
	app.settings().cloudShadows = 0;
	app.applySettings();
	app.setCloudCoverage(-1.0f);
	app.render();
	app.render();
	const u8* back = gfx::softFramebuffer();
	u32 differ = 0;
	for (u32 i = 0; i < u32(gfx::SCREEN_W * gfx::SCREEN_H) * 4; ++i) differ += std::abs(int(back[i]) - int(off[i])) > 2;
	CHECK(app.world().cloudMasksReady() == 0 && differ < 50);
}

int main(int argc, char** argv) {
	gDir = argc > 1 ? argv[1] : ".";
	const char* only = argc > 2 ? argv[2] : nullptr; // run the tests whose name contains this
	struct {
		const char* name;
		void (*fn)();
	} tests[] = {{"hash", testHash},
	             {"pak", testPak},
	             {"etc1 block decoder", testEtc1BlockDecoder},
	             {"tex solid", testTexSolid},
	             {"texture", testTexture},
	             {"frame budget", testFrameBudget},
	             {"rooms", testRooms},
	             {"triggers", testTriggers},
             {"camera flight", testCameraFlight},
	             {"vertex cache", testVertexCache},
	             {"chunker", testChunker},
	             {"seams", testSeams},
	             {"math", testMath},
	             {"runtime", testRuntime},
	             {"worlds", testWorlds},
	             {"loader", testLoader},
	             {"streaming", testStreaming},
	             {"streaming worlds", testStreamingWorlds},
	             {"pacer", testPacer},
	             {"pacer distance", testPacerDistance},
             {"pacer heavy", testPacerHeavy},
             {"stereo", testStereoEyes},
             {"water light", testWaterLight},
             {"fog zones", testFogZones},
             {"water grid", testWaterGrid},
             {"water reflect", testWaterReflect},
             {"clouds", testClouds},
             {"lamp light", testLampLight},
	             {"detail fade", testDetailFade},
             {"thinning", testThinning},
             {"size bands", testSizeBands},
             {"sight", testSight},
	             {"collision", testCollision},
	             {"model", testModel},
	             {"hero", testHero},
	             {"inhabitants", testInhabitants},
	             {"way net", testWayNet},
	             {"mobs", testMobs},
	             {"life", testLife},
	             {"fight", testFight},
             {"magic", testMagic},
             {"transform", testTransformHero},
             {"ranged", testRanged},
             {"bodies", testBodies},
             {"overlays", testOverlays},
	             {"inventory", testInventory},
             {"chests", testChests},
             {"mob use", testMobUse},
             {"ladder steering", testLadderSteering},
             {"trade", testTrade},
             {"teach", testTeach},
             {"particles", testParticles},
             {"dialogs", testDialogs},
             {"saves", testSaves},
             {"start menu", testStartMenu},
             {"npcai", testNpcAi},
	             {"beast turn", testBeastTurn},
	             {"g1 beasts", testG1Beasts},
             {"npc items", testNpcItems},
             {"corpses", testCorpses},
	             {"settings", testSettings},
	             {"graphics+", testGfxPlus},
	             {"sun shadow", testSunShadow},
             {"grass", testGrass},
             {"lamps", testLamps},
	             {"far layer", testFarLayer},
	             {"chunk tiers", testChunkTiers},
	             {"visibility", testVisibility},
	             {"visibility floors", testVisibilityFloors},
	             {"visibility rooms", testVisibilityRooms},
	             {"lod", testLod},
             {"shore", testShore},
             {"water fx", testWaterFx},
             {"ripples", testRipples},
             {"weather", testWeather},
             {"simplify", testSimplify},
             {"displaytiming", testDisplayTiming},
             {"reproject", testReproject},
             {"readahead", testReadAhead},
	             {"music", testMusic},
	             {"speech", testSpeech},
             {"sound", testSound},
	             {"sky", testSky}};
	for (auto& t : tests) {
		// Later tests reuse the archives earlier ones baked.
		const bool bakes = std::strcmp(t.name, "worlds") == 0 || std::strcmp(t.name, "loader") == 0;
		if (only && !std::strstr(t.name, only) && !bakes) continue;
		const int before = gFailures;
		std::printf("[ RUN ] %s\n", t.name);
		t.fn();
		std::printf("[ %s ] %s\n", gFailures == before ? " OK " : "FAIL", t.name);
	}
	std::printf("%d checks, %d failed\n", gChecks, gFailures);
	return gFailures ? 1 : 0;
}
