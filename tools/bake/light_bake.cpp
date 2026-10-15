#include "light_bake.h"

#include "bvh.h"
#include "g3ds/world.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

namespace g3 {

using namespace geo;

namespace {

inline V3 at(const SrcVertex& v) { return {v.pos[0], v.pos[1], v.pos[2]}; }

// The middle of two corners.
SrcVertex middle(const SrcVertex& a, const SrcVertex& b) {
	SrcVertex m = a;
	for (int i = 0; i < 3; ++i) m.pos[i] = (a.pos[i] + b.pos[i]) * 0.5f;
	for (int i = 0; i < 2; ++i) m.uv[i] = (a.uv[i] + b.uv[i]) * 0.5f;
	for (int i = 0; i < 4; ++i) m.rgba[i] = u8((int(a.rgba[i]) + int(b.rgba[i]) + 1) / 2);
	m.occlusion = u8((int(a.occlusion) + int(b.occlusion) + 1) / 2);
	m.wind = u8((int(a.wind) + int(b.wind) + 1) / 2);
	return m;
}

float longestEdge(const SrcTriangle& t) {
	float e = 0;
	for (int k = 0; k < 3; ++k) e = std::max(e, length(at(t.v[(k + 1) % 3]) - at(t.v[k])));
	return e;
}

bool near(const SrcTriangle& t, const SrcLight& l) {
	for (int a = 0; a < 3; ++a) {
		const float lo = std::min({t.v[0].pos[a], t.v[1].pos[a], t.v[2].pos[a]});
		const float hi = std::max({t.v[0].pos[a], t.v[1].pos[a], t.v[2].pos[a]});
		if (l.pos[a] < lo - l.range || l.pos[a] > hi + l.range) return false;
	}
	return true;
}

} // namespace

float lampLight(float distance, float range, float facing) {
	if (range <= 0.0f || distance >= range) return 0.0f;
	const float fall = 1.0f - distance / range;
	const float wrap = std::clamp((facing + 0.25f) / 1.25f, 0.0f, 1.0f);
	return fall * fall * wrap;
}

u32 cutNearLights(SrcScene& scene, const std::vector<AoMaterial>& materials, const std::vector<SrcLight>& lamps, float cellCm, u32 maxExtra) {
	if (lamps.empty() || maxExtra == 0) return 0;
	auto mat = [&](u32 m) { return m < materials.size() ? materials[m] : AoMaterial {}; };
	LightStats stats;
	// ---- cut the large triangles near a lamp ----
	// (Plants - what sways - are left whole, and so is what the lamps do not reach.)
	const float cell = std::max(cellCm, 20.0f);
	const size_t original = scene.triangles.size();
	u32 budget = maxExtra;
	std::vector<SrcTriangle> out;
	out.reserve(original);
	std::vector<SrcTriangle> work;
	for (size_t i = 0; i < original; ++i) {
		const SrcTriangle& t = scene.triangles[i];
		bool reached = false;
		if (mat(t.material).receives && budget > 0 && !t.v[0].wind && !t.v[1].wind && !t.v[2].wind && longestEdge(t) > cell)
			for (const SrcLight& l : lamps) reached = reached || near(t, l);
		if (!reached) {
			out.push_back(t);
			continue;
		}
		work.assign(1, t);
		for (int depth = 0; depth < 5 && !work.empty(); ++depth) {
			std::vector<SrcTriangle> next;
			for (const SrcTriangle& w : work) {
				bool still = false;
				for (const SrcLight& l : lamps) still = still || near(w, l);
				if (!still || longestEdge(w) <= cell || budget < 3) {
					out.push_back(w);
					continue;
				}
				const SrcVertex m01 = middle(w.v[0], w.v[1]), m12 = middle(w.v[1], w.v[2]), m20 = middle(w.v[2], w.v[0]);
				SrcTriangle a = w, b = w, c = w, d = w;
				a.v[0] = w.v[0]; a.v[1] = m01; a.v[2] = m20;
				b.v[0] = m01; b.v[1] = w.v[1]; b.v[2] = m12;
				c.v[0] = m20; c.v[1] = m12; c.v[2] = w.v[2];
				d.v[0] = m01; d.v[1] = m12; d.v[2] = m20;
				budget -= 3;
				stats.added += 3;
				next.push_back(a); next.push_back(b); next.push_back(c); next.push_back(d);
			}
			work.swap(next);
		}
		for (const SrcTriangle& w : work) out.push_back(w); // (cut as far as the depth went)
	}
	scene.triangles.swap(out);

	return stats.added;
}

void bakeLights(SrcScene& scene, const std::vector<AoMaterial>& materials, const LightOptions& opt, LightStats& stats) {
	stats = LightStats {};
	if (!opt.enabled || scene.lights.empty()) return;
	const auto start = std::chrono::steady_clock::now();
	std::vector<SrcLight> lamps;
	for (const SrcLight& l : scene.lights) {
		if (l.isStatic && !opt.statics) {
			++stats.skippedStatic;
			continue;
		}
		lamps.push_back(l);
	}
	stats.lamps = u32(lamps.size());
	if (lamps.empty()) return;
	auto mat = [&](u32 m) { return m < materials.size() ? materials[m] : AoMaterial {}; };

	stats.added = cutNearLights(scene, materials, lamps, opt.cell, opt.maxExtra);

	// ---- what blocks (as for the occlusion: the small objects neither block nor cast) ----
	std::vector<Tri> blockers;
	for (const SrcTriangle& t : scene.triangles) {
		const float b = mat(t.material).blocks;
		if (b <= 0.0f || (t.detail & 3) == DETAIL_SMALL) continue;
		const V3 a = at(t.v[0]);
		const V3 e1 = at(t.v[1]) - a, e2 = at(t.v[2]) - a;
		if (length(cross(e1, e2)) < 1e-4f) continue;
		blockers.push_back(Tri {a, e1, e2, b});
	}
	Bvh bvh;
	bvh.build(std::move(blockers));

	// ---- the light at every corner ----
	std::atomic<size_t> next {0};
	std::atomic<u64> corners {0}, rays {0};
	auto work2 = [&]() {
		u64 gotLight = 0, cast = 0;
		for (;;) {
			const size_t begin = next.fetch_add(256);
			if (begin >= scene.triangles.size()) break;
			const size_t end = std::min(scene.triangles.size(), begin + 256);
			for (size_t t = begin; t < end; ++t) {
				SrcTriangle& tri = scene.triangles[t];
				if (!mat(tri.material).receives) continue;
				V3 n = cross(at(tri.v[1]) - at(tri.v[0]), at(tri.v[2]) - at(tri.v[0])) * -1.0f;
				const float nl = length(n);
				if (nl < 1e-6f) continue;
				n = n * (1.0f / nl);
				for (int k = 0; k < 3; ++k) {
					SrcVertex& v = tri.v[k];
					const V3 p = at(v);
					float sum[3] = {0, 0, 0};
					for (const SrcLight& l : lamps) {
						const V3 to = V3 {l.pos[0], l.pos[1], l.pos[2]} - p;
						const float d = length(to);
						if (d >= l.range || d < 1.0f) continue;
						const V3 dir = to * (1.0f / d);
						const float f = lampLight(d, l.range, dot(n, dir));
						if (f <= 0.0f) continue;
						// Shadow: from a little off the surface towards the lamp; something in between puts it out.
						++cast;
						float hit = 0;
						const float blocked = bvh.nearest(p + n * 3.0f, dir, 0.5f, std::max(d - 8.0f, 1.0f), hit, nullptr, true);
						if (blocked >= 0.5f) continue;
						for (int c = 0; c < 3; ++c) sum[c] += float(l.color[c]) * f * opt.strength * (1.0f - blocked);
					}
					if (sum[0] + sum[1] + sum[2] <= 0.5f) continue;
					++gotLight;
					const float before = float(v.rgba[0]) + float(v.rgba[1]) + float(v.rgba[2]);
					float after = 0;
					for (int c = 0; c < 3; ++c) {
						const float value = std::min(255.0f, float(v.rgba[c]) + sum[c]);
						v.rgba[c] = u8(value + 0.5f);
						after += value;
					}
					// The lamp is not the sun's: the daylight weight shrinks by what the light grew by.
					if (after > 0.5f) v.rgba[3] = u8(std::clamp(float(v.rgba[3]) * before / after, 0.0f, 255.0f) + 0.5f);
				}
			}
		}
		corners += gotLight;
		rays += cast;
	};
	const unsigned threads = std::max(1u, opt.threads);
	std::vector<std::thread> pool;
	for (unsigned i = 1; i < threads; ++i) pool.emplace_back(work2);
	work2();
	for (std::thread& th : pool) th.join();
	stats.corners = corners.load();
	stats.rays = rays.load();
	stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

std::vector<WorldLamp> lampTable(const std::vector<SrcLight>& lights, bool baked) {
	std::vector<WorldLamp> out;
	for (const SrcLight& l : lights) {
		if (l.isStatic || !(l.range > 1.0f)) continue;
		WorldLamp w {};
		for (int a = 0; a < 3; ++a) w.pos[a] = l.pos[a];
		w.range = std::min(l.range, 20000.0f);
		for (int c = 0; c < 3; ++c) w.color[c] = l.color[c];
		w.flags = u8((l.fire ? LAMPF_FIRE : 0) | (baked ? LAMPF_BAKED : 0) | (l.rangeSmooth ? LAMPF_RANGE_SMOOTH : 0) |
		             (l.colorSmooth ? LAMPF_COLOR_SMOOTH : 0) | (l.made ? LAMPF_MADE : 0));
		const size_t nr = l.rangeKeys.size(), nc = l.colorKeys.size();
		if (nr > 1 && l.rangeFps > 0.0f) {
			const size_t keep = std::min<size_t>(nr, LAMP_KEYS);
			for (size_t k = 0; k < keep; ++k)
				w.rangeScale[k] = u8(std::clamp(l.rangeKeys[k * nr / keep] * 64.0f + 0.5f, 0.0f, 255.0f));
			w.rangeKeys = u8(keep);
			w.rangeFps = l.rangeFps * float(keep) / float(nr);
		}
		if (nc > 1 && l.colorFps > 0.0f) {
			const size_t keep = std::min<size_t>(nc, LAMP_KEYS);
			for (size_t k = 0; k < keep; ++k)
				for (int c = 0; c < 3; ++c) w.colors[k][c] = l.colorKeys[k * nc / keep][c];
			w.colorKeys = u8(keep);
			w.colorFps = l.colorFps * float(keep) / float(nc);
		}
		out.push_back(w);
	}
	return out;
}

} // namespace g3
