// Synthetic test scene: a valley with a palisaded camp, a lake and trees.
// It has nothing to do with Gothic's content; it exists so the complete
// pipeline (ETC1, cut-out alpha, chunking, streaming, fog) can be built and
// tested without owning the game. Sizes and densities are loosely modelled
// on the Colony so the bake report says something useful about budgets.
#include "importers.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace g3 {

namespace {

u32 hash2(int x, int y, u32 seed) {
	u32 h = u32(x) * 374761393u + u32(y) * 668265263u + seed * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

float rnd(int x, int y, u32 seed) { return float(hash2(x, y, seed) & 0xFFFFFF) / float(0xFFFFFF); }

float smooth(float t) { return t * t * (3.0f - 2.0f * t); }

// Value noise; `period` > 0 makes it tile (used for textures).
float noise(float x, float y, u32 seed, int period = 0) {
	int xi = int(std::floor(x)), yi = int(std::floor(y));
	float fx = smooth(x - float(xi)), fy = smooth(y - float(yi));
	auto at = [&](int ix, int iy) {
		if (period > 0) {
			ix = ((ix % period) + period) % period;
			iy = ((iy % period) + period) % period;
		}
		return rnd(ix, iy, seed);
	};
	float a = at(xi, yi), b = at(xi + 1, yi), c = at(xi, yi + 1), d = at(xi + 1, yi + 1);
	return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

float fbm(float x, float y, u32 seed, int octaves, int period = 0) {
	float sum = 0, amp = 0.5f, norm = 0;
	for (int i = 0; i < octaves; ++i) {
		sum += noise(x, y, seed + u32(i) * 17u, period) * amp;
		norm += amp;
		x *= 2;
		y *= 2;
		period *= 2;
		amp *= 0.5f;
	}
	return sum / norm;
}

struct V3 {
	float x, y, z;
};
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 norm(V3 a) {
	float l = std::sqrt(dot(a, a));
	return l > 0 ? V3 {a.x / l, a.y / l, a.z / l} : V3 {0, 1, 0};
}

const V3 kSun = norm({0.45f, 0.8f, 0.35f});

void shade(float ndl, float occlusion, u8 out[4]) {
	// Warm sun plus cool ambient, like a baked outdoor light pass.
	float d = std::max(ndl, 0.0f);
	float r = (0.38f + 0.72f * d) * occlusion;
	float g = (0.42f + 0.66f * d) * occlusion;
	float b = (0.50f + 0.52f * d) * occlusion;
	out[0] = u8(std::min(r, 1.0f) * 255.0f);
	out[1] = u8(std::min(g, 1.0f) * 255.0f);
	out[2] = u8(std::min(b, 1.0f) * 255.0f);
	out[3] = 255;
}

struct Builder {
	SrcScene& scene;

	SrcVertex vert(V3 p, float u, float v, V3 n, float occ = 1.0f) {
		SrcVertex s {};
		s.pos[0] = p.x;
		s.pos[1] = p.y;
		s.pos[2] = p.z;
		s.uv[0] = u;
		s.uv[1] = v;
		shade(dot(n, kSun), occ, s.rgba);
		return s;
	}

	// Emits a triangle that is counter-clockwise when seen from `outward`.
	// In this left-handed system that means cross(b-a, c-a) points away from it.
	void tri(SrcVertex a, SrcVertex b, SrcVertex c, V3 outward, u32 material, bool twoSided = false) {
		V3 pa {a.pos[0], a.pos[1], a.pos[2]}, pb {b.pos[0], b.pos[1], b.pos[2]}, pc {c.pos[0], c.pos[1], c.pos[2]};
		if (dot(cross(pb - pa, pc - pa), outward) > 0) std::swap(b, c);
		SrcTriangle t;
		t.v[0] = a;
		t.v[1] = b;
		t.v[2] = c;
		t.material = material;
		scene.triangles.push_back(t);
		if (twoSided) {
			std::swap(t.v[1], t.v[2]);
			scene.triangles.push_back(t);
		}
	}

	void quad(SrcVertex a, SrcVertex b, SrcVertex c, SrcVertex d, V3 outward, u32 material, bool twoSided = false) {
		tri(a, b, c, outward, material, twoSided);
		tri(a, c, d, outward, material, twoSided);
	}
};

// ---- procedural textures -------------------------------------------------

void fill(SrcImage& img, int size, const std::function<void(int, int, u8*)>& fn) {
	img.width = img.height = size;
	img.rgba.resize(size_t(size) * size * 4);
	for (int y = 0; y < size; ++y)
		for (int x = 0; x < size; ++x) fn(x, y, &img.rgba[(size_t(y) * size + x) * 4]);
}

void rgb(u8* p, float r, float g, float b, float a = 1.0f) {
	auto c = [](float v) { return u8(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
	p[0] = c(r);
	p[1] = c(g);
	p[2] = c(b);
	p[3] = c(a);
}

bool makeTexture(const std::string& name, SrcImage& img) {
	const int S = 256; // deliberately larger than the default bake size
	const float k = 1.0f / float(S);
	if (name == "SYN_GRASS") {
		fill(img, S, [&](int x, int y, u8* p) {
			float n = fbm(x * k * 8, y * k * 8, 11, 4, 8), f = noise(x * k * 64, y * k * 64, 12, 64);
			float v = 0.55f + 0.5f * n + 0.25f * (f - 0.5f);
			rgb(p, 0.22f * v + 0.05f * f, 0.48f * v, 0.14f * v);
		});
	} else if (name == "SYN_ROCK") {
		fill(img, S, [&](int x, int y, u8* p) {
			float n = fbm(x * k * 6, y * k * 6, 21, 5, 6);
			float crack = std::fabs(fbm(x * k * 4, y * k * 4, 22, 3, 4) - 0.5f);
			float v = 0.35f + 0.6f * n - (crack < 0.03f ? 0.25f : 0.0f);
			rgb(p, v * 0.95f, v * 0.92f, v * 0.88f);
		});
	} else if (name == "SYN_DIRT") {
		fill(img, S, [&](int x, int y, u8* p) {
			float n = fbm(x * k * 8, y * k * 8, 31, 4, 8), f = noise(x * k * 48, y * k * 48, 32, 48);
			float v = 0.6f + 0.4f * n + 0.15f * (f - 0.5f);
			rgb(p, 0.55f * v, 0.42f * v, 0.27f * v);
		});
	} else if (name == "SYN_WOOD") {
		fill(img, S, [&](int x, int y, u8* p) {
			int plank = x / 32;
			float grain = fbm(x * k * 4, y * k * 40, 41 + u32(plank), 3, 4);
			float edge = (x % 32) < 2 ? 0.45f : 1.0f;
			float v = (0.55f + 0.45f * grain) * edge * (0.85f + 0.15f * rnd(plank, 0, 43));
			rgb(p, 0.50f * v, 0.33f * v, 0.18f * v);
		});
	} else if (name == "SYN_THATCH") {
		fill(img, S, [&](int x, int y, u8* p) {
			float n = fbm(x * k * 48, y * k * 6, 51, 3, 6);
			float row = 0.75f + 0.25f * std::fabs(std::sin(float(y) * 3.14159f / 32.0f));
			float v = (0.5f + 0.5f * n) * row;
			rgb(p, 0.72f * v, 0.60f * v, 0.30f * v);
		});
	} else if (name == "SYN_WATER") {
		fill(img, S, [&](int x, int y, u8* p) {
			float n = fbm(x * k * 6, y * k * 6, 61, 4, 6);
			rgb(p, 0.12f + 0.2f * n, 0.30f + 0.3f * n, 0.42f + 0.35f * n);
		});
	} else if (name == "SYN_PALISADE") {
		// Pointed stakes with gaps: exercises ETC1A4 and the alpha test.
		fill(img, S, [&](int x, int y, u8* p) {
			int stake = x / 32, lx = x % 32;
			float tip = std::fabs(float(lx) - 15.5f) * 3.0f; // rows cut away at the top
			bool solid = lx >= 3 && lx < 29 && float(y) > tip + 6.0f * rnd(stake, 1, 71);
			float grain = fbm(x * k * 6, y * k * 30, 72 + u32(stake), 3, 6);
			float v = (0.5f + 0.5f * grain) * (0.8f + 0.2f * rnd(stake, 0, 73));
			rgb(p, 0.42f * v, 0.29f * v, 0.17f * v, solid ? 1.0f : 0.0f);
		});
	} else if (name == "SYN_TREE") {
		fill(img, S, [&](int x, int y, u8* p) {
			float cx = (float(x) - 127.5f) / 128.0f, cy = (float(y) - 95.0f) / 96.0f;
			float n = fbm(x * k * 10, y * k * 10, 81, 4);
			bool leaf = cx * cx + cy * cy < 0.55f + 0.5f * (n - 0.5f) && y < 200;
			bool trunk = std::fabs(float(x) - 127.5f) < 9.0f && y >= 150;
			float f = noise(x * k * 40, y * k * 40, 82);
			if (leaf)
				rgb(p, 0.12f + 0.12f * f, 0.30f + 0.25f * n, 0.10f + 0.05f * f, 1.0f);
			else if (trunk)
				rgb(p, 0.30f + 0.1f * f, 0.21f + 0.08f * f, 0.12f, 1.0f);
			else
				rgb(p, 0, 0, 0, 0);
		});
	} else {
		return false;
	}
	return true;
}

} // namespace

void buildSyntheticScene(const SynthOptions& opt, SrcScene& scene) {
	enum { GRASS, ROCK, DIRT, WOOD, THATCH, WATER, PALISADE, TREE, MISSING, MAT_COUNT };
	static const char* names[MAT_COUNT] = {"SYN_GRASS", "SYN_ROCK",     "SYN_DIRT", "SYN_WOOD",   "SYN_THATCH",
	                                       "SYN_WATER", "SYN_PALISADE", "SYN_TREE", "SYN_MISSING"};
	for (int i = 0; i < MAT_COUNT; ++i) {
		SrcMaterial m;
		m.name = names[i];
		m.texture = std::string(names[i]) + ".TGA";
		m.water = i == WATER;
		m.noCollision = i == TREE; // foliage cards: walk through
		scene.materials.push_back(m);
	}
	scene.loadTexture = [](const std::string& name, SrcImage& img) {
		std::string base = name.substr(0, name.find('.'));
		return makeTexture(base, img);
	};

	Builder b {scene};
	const int N = opt.cells;
	const float cell = opt.cellSize;
	const float half = N * cell * 0.5f;
	const float waterLevel = -420.0f;
	const float campRadius = 2600.0f;

	auto height = [&](float x, float z) {
		float d = std::sqrt(x * x + z * z);
		float hills = (fbm(x / 9000.0f + 40, z / 9000.0f + 40, 1, 5) - 0.5f) * 5200.0f;
		float detail = (fbm(x / 900.0f, z / 900.0f, 2, 3) - 0.5f) * 160.0f;
		float rim = std::pow(std::min(d / half, 1.0f), 3.0f) * 9000.0f; // mountains at the border
		float lake = -std::exp(-((x - 6500) * (x - 6500) + (z + 5200) * (z + 5200)) / (2 * 2600.0f * 2600.0f)) * 1500.0f;
		float h = hills + detail + rim + lake;
		// Flatten a plateau for the camp in the middle of the map.
		float t = smooth(std::clamp((d - campRadius) / 2200.0f, 0.0f, 1.0f));
		return 120.0f + (h - 120.0f) * t;
	};
	auto normalAt = [&](float x, float z) {
		const float e = cell * 0.5f;
		return norm({height(x - e, z) - height(x + e, z), 2 * e, height(x, z - e) - height(x, z + e)});
	};
	auto onRoad = [&](float x, float z) {
		float c = std::sin(z / 5200.0f) * 2600.0f;
		return std::fabs(x - c) < 280.0f;
	};

	// ---- terrain ----
	const float uvTile = 1.0f / 400.0f; // one texture repeat every 4 m
	for (int iz = 0; iz < N; ++iz) {
		for (int ix = 0; ix < N; ++ix) {
			float x0 = -half + ix * cell, z0 = -half + iz * cell, x1 = x0 + cell, z1 = z0 + cell;
			V3 p[4] = {{x0, height(x0, z0), z0}, {x1, height(x1, z0), z0}, {x1, height(x1, z1), z1}, {x0, height(x0, z1), z1}};
			V3 n[4];
			for (int k = 0; k < 4; ++k) n[k] = normalAt(p[k].x, p[k].z);
			float cx = (x0 + x1) * 0.5f, cz = (z0 + z1) * 0.5f;
			V3 fn = normalAt(cx, cz);
			float d = std::sqrt(cx * cx + cz * cz);

			u32 mat = GRASS;
			if (fn.y < 0.80f) mat = ROCK;
			else if (onRoad(cx, cz) || d < campRadius * 0.9f) mat = DIRT;
			else if (height(cx, cz) < waterLevel + 60.0f) mat = DIRT;
			// One deliberately broken material to exercise the fallback path.
			if (ix == N / 2 + 9 && iz == N / 2 + 9) mat = MISSING;

			SrcVertex v[4];
			for (int k = 0; k < 4; ++k) {
				float depth = std::clamp((waterLevel - p[k].y) / 500.0f, 0.0f, 0.6f); // darker under water
				v[k] = b.vert(p[k], p[k].x * uvTile, p[k].z * uvTile, n[k], 1.0f - depth);
			}
			b.quad(v[0], v[1], v[2], v[3], {0, 1, 0}, mat);

			// ---- water surface over submerged cells ----
			if (std::min({p[0].y, p[1].y, p[2].y, p[3].y}) < waterLevel) {
				SrcVertex w[4];
				for (int k = 0; k < 4; ++k)
					w[k] = b.vert({p[k].x, waterLevel, p[k].z}, p[k].x / 900.0f, p[k].z / 900.0f, {0, 1, 0});
				b.quad(w[0], w[1], w[2], w[3], {0, 1, 0}, WATER);
			}
		}
	}

	// ---- huts ----
	auto hut = [&](float cx, float cz, float w, float dpt, float wallH, float rot) {
		float y = height(cx, cz) - 20.0f;
		float cs = std::cos(rot), sn = std::sin(rot);
		auto P = [&](float lx, float ly, float lz) { return V3 {cx + lx * cs - lz * sn, y + ly, cz + lx * sn + lz * cs}; };
		auto N3 = [&](float lx, float lz) { return V3 {lx * cs - lz * sn, 0, lx * sn + lz * cs}; };
		const float hx = w * 0.5f, hz = dpt * 0.5f, roofH = wallH + w * 0.45f, eave = 40.0f;
		struct Side {
			float ax, az, bx, bz, nx, nz;
		} sides[4] = {{-hx, -hz, hx, -hz, 0, -1}, {hx, -hz, hx, hz, 1, 0}, {hx, hz, -hx, hz, 0, 1}, {-hx, hz, -hx, -hz, -1, 0}};
		for (const Side& s : sides) {
			V3 nn = N3(s.nx, s.nz);
			float len = std::sqrt((s.bx - s.ax) * (s.bx - s.ax) + (s.bz - s.az) * (s.bz - s.az)) / 256.0f;
			b.quad(b.vert(P(s.ax, 0, s.az), 0, wallH / 256.0f, nn, 0.75f), b.vert(P(s.bx, 0, s.bz), len, wallH / 256.0f, nn, 0.75f),
			       b.vert(P(s.bx, wallH, s.bz), len, 0, nn), b.vert(P(s.ax, wallH, s.az), 0, 0, nn), nn, WOOD);
		}
		// Gables.
		for (float gz : {-hz, hz}) {
			V3 nn = N3(0, gz < 0 ? -1.0f : 1.0f);
			b.tri(b.vert(P(-hx, wallH, gz), 0, 0.6f, nn), b.vert(P(hx, wallH, gz), w / 256.0f, 0.6f, nn),
			      b.vert(P(0, roofH, gz), w / 512.0f, 0, nn), nn, WOOD);
		}
		// Two roof slopes with a small overhang.
		for (float side : {-1.0f, 1.0f}) {
			V3 ln = norm({side * (roofH - wallH), hx, 0}); // slope normal in hut space
			V3 nn {ln.x * cs, ln.y, ln.x * sn};
			float ex = side * (hx + eave), ey = wallH - eave * 0.9f;
			b.quad(b.vert(P(ex, ey, -hz - eave), 0, 1.6f, nn), b.vert(P(ex, ey, hz + eave), dpt / 200.0f, 1.6f, nn),
			       b.vert(P(0, roofH, hz + eave), dpt / 200.0f, 0, nn), b.vert(P(0, roofH, -hz - eave), 0, 0, nn), nn, THATCH,
			       true);
		}
	};
	for (int i = 0; i < opt.huts; ++i) {
		float ring = i % 3 == 0 ? 0.42f : (i % 3 == 1 ? 0.62f : 0.82f);
		float ang = float(i) * 2.399963f; // golden angle keeps them from lining up
		float r = campRadius * ring * (0.92f + 0.16f * rnd(i, 0, 91));
		hut(std::cos(ang) * r, std::sin(ang) * r, 420.0f + 220.0f * rnd(i, 1, 91), 480.0f + 260.0f * rnd(i, 2, 91),
		    230.0f + 60.0f * rnd(i, 3, 91), ang + 1.5708f + (rnd(i, 4, 91) - 0.5f) * 0.6f);
	}

	// ---- palisade ring (cut-out, two-sided), with a gate towards +Z ----
	const int segs = 96;
	for (int i = 0; i < segs; ++i) {
		float a0 = float(i) / segs * 6.28318f, a1 = float(i + 1) / segs * 6.28318f;
		if (std::fabs(a0 - 1.5708f) < 0.10f) continue; // gate
		float r = campRadius * 1.04f;
		V3 p0 {std::cos(a0) * r, 0, std::sin(a0) * r}, p1 {std::cos(a1) * r, 0, std::sin(a1) * r};
		p0.y = height(p0.x, p0.z) - 30.0f;
		p1.y = height(p1.x, p1.z) - 30.0f;
		V3 nn = norm({std::cos((a0 + a1) * 0.5f), 0, std::sin((a0 + a1) * 0.5f)});
		const float hgt = 380.0f;
		b.quad(b.vert(p0, float(i), 1, nn, 0.8f), b.vert(p1, float(i + 1), 1, nn, 0.8f),
		       b.vert({p1.x, p1.y + hgt, p1.z}, float(i + 1), 0, nn), b.vert({p0.x, p0.y + hgt, p0.z}, float(i), 0, nn), nn,
		       PALISADE, true);
	}

	// ---- trees: two crossed cut-out quads each ----
	int placed = 0;
	for (int i = 0; placed < opt.trees && i < opt.trees * 12; ++i) {
		float x = (rnd(i, 0, 101) - 0.5f) * 2 * half * 0.92f, z = (rnd(i, 1, 101) - 0.5f) * 2 * half * 0.92f;
		float y = height(x, z);
		if (std::sqrt(x * x + z * z) < campRadius * 1.25f || y < waterLevel + 80.0f || normalAt(x, z).y < 0.86f || onRoad(x, z))
			continue;
		++placed;
		float hgt = 700.0f + 500.0f * rnd(i, 2, 101), wid = hgt * 0.42f, rot = rnd(i, 3, 101) * 3.14159f;
		for (int q = 0; q < 2; ++q) {
			float a = rot + float(q) * 1.5708f;
			V3 dir {std::cos(a) * wid, 0, std::sin(a) * wid}, nn {-std::sin(a), 0.35f, std::cos(a)};
			nn = norm(nn);
			b.quad(b.vert({x - dir.x, y - 15, z - dir.z}, 0, 1, nn, 0.7f), b.vert({x + dir.x, y - 15, z + dir.z}, 1, 1, nn, 0.7f),
			       b.vert({x + dir.x, y + hgt, z + dir.z}, 1, 0, {0, 1, 0}), b.vert({x - dir.x, y + hgt, z - dir.z}, 0, 0, {0, 1, 0}),
			       nn, TREE, true);
		}
	}

	// Start just outside the gate, looking back at the camp.
	scene.spawnPos[0] = 0;
	scene.spawnPos[2] = campRadius * 1.9f;
	scene.spawnPos[1] = height(0, scene.spawnPos[2]) + 175.0f; // eye height
	scene.spawnYaw = 3.14159f;
	scene.hasSpawn = true;
	scene.notes.push_back("synthetic scene: " + std::to_string(N) + "x" + std::to_string(N) + " terrain cells, " +
	                      std::to_string(opt.huts) + " huts, " + std::to_string(placed) + " trees");
}

} // namespace g3
