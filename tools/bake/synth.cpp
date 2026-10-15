// Synthetic test scene: a valley with a palisaded camp, a lake and trees.
// It has nothing to do with Gothic's content; it exists so the complete
// pipeline (ETC1, cut-out alpha, chunking, streaming, fog) can be built and
// tested without owning the game. Sizes and densities are loosely modelled
// on the Colony so the bake report says something useful about budgets.
#include "g3ds/world.h"
#include "importers.h"
#include "vertex_fx.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

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
	u8 detail = 0; // DetailClass given to the triangles emitted from now on

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
		t.detail = detail;
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

const int kWaterFrames = 4;

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
	} else if (name.rfind("SYN_WATER_A", 0) == 0 && name.size() == 12 && name[11] >= '0' && name[11] < '0' + kWaterFrames) {
		// Four frames of ripples: the same noise sampled along a small circle.
		const float phase = float(name[11] - '0') / float(kWaterFrames) * 6.28318f;
		fill(img, S, [&](int x, int y, u8* p) {
			float n = fbm(x * k * 6 + 0.35f * std::cos(phase), y * k * 6 + 0.35f * std::sin(phase), 61, 4, 6);
			float glint = std::max(0.0f, std::sin(n * 20.0f + phase)) * 0.12f;
			rgb(p, 0.10f + 0.2f * n + glint, 0.30f + 0.3f * n + glint, 0.44f + 0.35f * n + glint);
		});
	} else if (name == "SYN_GLOW") {
		// A soft blob of light; the alpha is how much of it gets added.
		fill(img, S, [&](int x, int y, u8* p) {
			float cx = (float(x) - 127.5f) / 128.0f, cy = (float(y) - 127.5f) / 128.0f;
			float a = std::clamp(1.0f - std::sqrt(cx * cx + cy * cy), 0.0f, 1.0f);
			rgb(p, 1.0f, 0.82f, 0.45f, a * a);
		});
	} else if (name == "SYN_WEB") {
		// A cobweb: thin threads on a transparent background, drawn blended.
		fill(img, S, [&](int x, int y, u8* p) {
			float cx = float(x) - 127.5f, cy = float(y) - 127.5f;
			float r = std::sqrt(cx * cx + cy * cy), ang = std::atan2(cy, cx);
			float spoke = std::fabs(std::fmod(ang * 8.0f / 6.28318f + 16.0f, 1.0f) - 0.5f);
			float ring = std::fabs(std::fmod(r / 22.0f, 1.0f) - 0.5f);
			bool thread = r < 124.0f && (spoke < 0.03f + 2.0f / (r + 8.0f) || ring < 0.06f);
			rgb(p, 0.85f, 0.85f, 0.88f, thread ? 0.75f : 0.0f);
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

// Both synthetic worlds use the same material list.
void addMaterials(SrcScene& scene) {
	static const char* names[] = {"SYN_GRASS",    "SYN_ROCK", "SYN_DIRT",    "SYN_WOOD", "SYN_THATCH", "SYN_WATER_A0",
	                              "SYN_PALISADE", "SYN_TREE", "SYN_MISSING", "SYN_GLOW", "SYN_WEB"};
	enum { WATER = 5, TREE = 7, GLOW = 9, WEB = 10 };
	for (int i = 0; i < int(sizeof(names) / sizeof(names[0])); ++i) {
		SrcMaterial m;
		m.name = names[i];
		m.texture = std::string(names[i]) + ".TGA";
		if (i == WATER) {
			// Animated, slowly drifting, see-through.
			m.water = true;
			m.opacity = 150;
			m.animFps = 4.0f;
			m.scroll[0] = 0.03f;
			m.scroll[1] = 0.015f;
		}
		m.noCollision = i == TREE || i == GLOW || i == WEB; // foliage cards and decals: walk through
		if (i == GLOW) m.blend = SrcBlend::Add;
		if (i == WEB) m.blend = SrcBlend::Alpha;
		scene.materials.push_back(m);
	}
	scene.loadTexture = [](const std::string& name, SrcImage& img) {
		std::string base = name.substr(0, name.find('.'));
		return makeTexture(base, img);
	};
}

} // namespace

// fix150: a dome over the synthetic valley, as the game's MAGICFRONTIER_OUT is over the colony: flattened, standing below the
// ground at its rim, mapped round and up like the original, with a web of lightning for its texture (SYNTH_BARRIERE).
bool buildSyntheticBarrier(SrcBarrier& out, std::string& why) {
	out = SrcBarrier {};
	(void)why;
	const int rings = 18, segs = 48;
	const float rx = 52000.0f, ry = 26000.0f, base = -6000.0f;
	for (int r = 0; r <= rings; ++r) {
		const float el = float(r) / float(rings) * 1.5707963f;
		for (int s = 0; s <= segs; ++s) {
			const float az = float(s) / float(segs) * 6.2831853f;
			out.pos.insert(out.pos.end(), {rx * std::cos(el) * std::sin(az), base + ry * std::sin(el), rx * std::cos(el) * std::cos(az)});
			out.uv.insert(out.uv.end(), {float(s) / float(segs) * 6.0f, float(r) / float(rings) * 3.0f});
		}
	}
	for (int r = 0; r < rings; ++r)
		for (int s = 0; s < segs; ++s) {
			const u32 a = u32(r * (segs + 1) + s), b = a + 1, c = a + u32(segs + 1), d = c + 1;
			out.indices.insert(out.indices.end(), {a, b, d, a, d, c});
		}
	out.texture = "SYNTH_BARRIERE.TGA";
	out.mesh = "synthetic";
	return true;
}

namespace {
// A web of jagged blue-white lines on black, wrapping on both sides (the look of BARRIERE.TGA).
void synthBarrierTexture(SrcImage& img) {
	const int N = 128;
	img.width = img.height = N;
	std::vector<float> lum(size_t(N * N), 0.0f);
	u32 seed = 12345u;
	auto rnd = [&]() {
		seed = seed * 1664525u + 1013904223u;
		return float(seed >> 8) / 16777216.0f;
	};
	auto plot = [&](float x, float y, float w) {
		for (int oy = -2; oy <= 2; ++oy)
			for (int ox = -2; ox <= 2; ++ox) {
				const int px = int(std::floor(x)) + ox, py = int(std::floor(y)) + oy;
				const float dx = float(px) + 0.5f - x, dy = float(py) + 0.5f - y;
				const float f = std::max(0.0f, 1.0f - std::sqrt(dx * dx + dy * dy) / 1.6f) * w;
				float& l = lum[size_t(((py % N + N) % N) * N + ((px % N + N) % N))];
				l = std::max(l, f);
			}
	};
	for (int k = 0; k < 26; ++k) {
		float x = rnd() * N, y = rnd() * N, dir = rnd() * 6.2831853f;
		const float w = 0.5f + 0.5f * rnd();
		const int steps = 40 + int(rnd() * 80);
		for (int i = 0; i < steps; ++i) {
			dir += (rnd() - 0.5f) * 1.1f;
			x += std::cos(dir) * 1.5f;
			y += std::sin(dir) * 1.5f;
			plot(x, y, w * (1.0f - 0.5f * float(i) / float(steps)));
		}
	}
	img.rgba.assign(size_t(N * N * 4), 255);
	for (int i = 0; i < N * N; ++i) {
		const float l = lum[size_t(i)];
		img.rgba[size_t(i) * 4 + 0] = u8(std::min(255.0f, 40.0f * l + 160.0f * l * l));
		img.rgba[size_t(i) * 4 + 1] = u8(std::min(255.0f, 70.0f * l + 160.0f * l * l));
		img.rgba[size_t(i) * 4 + 2] = u8(std::min(255.0f, 255.0f * l));
	}
}
} // namespace

bool buildSyntheticParticleTexture(const std::string& name, SrcImage& img) {
	std::string base;
	for (char c : name.substr(0, name.find('.'))) base += char(std::tolower((unsigned char)c));
	if (base == "synth_barriere") {
		synthBarrierTexture(img);
		return true;
	}
	const bool spark = base == "synth_spark", smoke = base == "synth_smoke";
	if (!spark && !smoke) return false;
	img.width = img.height = 32;
	img.rgba.assign(32 * 32 * 4, 0);
	for (int y = 0; y < 32; ++y)
		for (int x = 0; x < 32; ++x) {
			const float dx = (x - 15.5f) / 16.0f, dy = (y - 15.5f) / 16.0f;
			const float r = std::sqrt(dx * dx + dy * dy);
			const float f = r >= 1.0f ? 0.0f : (1.0f - r) * (1.0f - r);
			u8* px = &img.rgba[size_t(y * 32 + x) * 4];
			px[0] = spark ? 255 : 150;
			px[1] = spark ? u8(120 + 100 * f) : 150;
			px[2] = spark ? u8(40 + 80 * f) : 150;
			px[3] = u8(255 * f);
		}
	return true;
}

void buildSyntheticScene(const SynthOptions& opt, SrcScene& scene) {
	enum { GRASS, ROCK, DIRT, WOOD, THATCH, WATER, PALISADE, TREE, MISSING, GLOW, WEB, MAT_COUNT };
	addMaterials(scene);

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
		if (opt.river) {
			// A flood plain a little over the water, and in it a channel that goes down towards +x.
			const float zr = 6500.0f, dz = std::fabs(z - zr);
			const float plain = smooth(std::clamp(1.0f - (dz - 500.0f) / 2500.0f, 0.0f, 1.0f));
			h = h + (-h + 90.0f) * plain;
			const float bed = waterLevel - 70.0f - (x + half) / (2.0f * half) * 250.0f;
			const float bank = smooth(std::clamp((dz - 160.0f) / 340.0f, 0.0f, 1.0f));
			h = std::min(h, bed + (h - bed) * bank);
		}
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
		const size_t firstTriangle = scene.triangles.size();
		for (int q = 0; q < 2; ++q) {
			float a = rot + float(q) * 1.5708f;
			V3 dir {std::cos(a) * wid, 0, std::sin(a) * wid}, nn {-std::sin(a), 0.35f, std::cos(a)};
			nn = norm(nn);
			b.quad(b.vert({x - dir.x, y - 15, z - dir.z}, 0, 1, nn, 0.7f), b.vert({x + dir.x, y - 15, z + dir.z}, 1, 1, nn, 0.7f),
			       b.vert({x + dir.x, y + hgt, z + dir.z}, 1, 0, {0, 1, 0}), b.vert({x - dir.x, y + hgt, z - dir.z}, 0, 0, {0, 1, 0}),
			       nn, TREE, true);
		}
		// They sway in the wind like the game's (vertex_fx.h; kept with --wind).
		const PlantKind kind = plantKindOf("SYN_TREE");
		markSway(scene.triangles.data() + firstTriangle, scene.triangles.size() - firstTriangle, kind, y - 15.0f, y + hgt,
		         plantSway(kind, hgt + 15.0f));
	}

	// ---- small and medium objects: only drawn near the camera ----
	auto box = [&](float cx, float cz, float w, float h, float rot, int sides, u32 mat) {
		// A prism with `sides` faces and a lid, standing on the ground.
		const float y = height(cx, cz) - 5.0f;
		for (int i = 0; i < sides; ++i) {
			const float a0 = rot + float(i) / float(sides) * 6.28318f, a1 = rot + float(i + 1) / float(sides) * 6.28318f;
			const V3 p0 {cx + std::cos(a0) * w, y, cz + std::sin(a0) * w}, p1 {cx + std::cos(a1) * w, y, cz + std::sin(a1) * w};
			const V3 nn = norm({std::cos((a0 + a1) * 0.5f), 0, std::sin((a0 + a1) * 0.5f)});
			b.quad(b.vert(p0, float(i) * 0.5f, 1, nn, 0.7f), b.vert(p1, float(i + 1) * 0.5f, 1, nn, 0.7f),
			       b.vert({p1.x, y + h, p1.z}, float(i + 1) * 0.5f, 0, nn), b.vert({p0.x, y + h, p0.z}, float(i) * 0.5f, 0, nn), nn, mat);
			b.tri(b.vert({cx, y + h, cz}, 0.5f, 0.5f, {0, 1, 0}), b.vert({p0.x, y + h, p0.z}, 0, 0, {0, 1, 0}),
			      b.vert({p1.x, y + h, p1.z}, 1, 0, {0, 1, 0}), {0, 1, 0}, mat);
		}
	};
	int barrels = 0, crates = 0;
	for (int i = 0; i < opt.huts * 2; ++i) {
		const float ang = float(i) * 1.618f, r = campRadius * (0.25f + 0.6f * rnd(i, 0, 131));
		b.detail = DETAIL_SMALL;
		box(std::cos(ang) * r, std::sin(ang) * r, 38.0f, 95.0f, rnd(i, 1, 131), 8, WOOD); // barrel
		++barrels;
		if (i % 3 == 0) {
			b.detail = DETAIL_MEDIUM;
			box(std::cos(ang) * r + 260.0f, std::sin(ang) * r + 140.0f, 150.0f, 210.0f, 0.785f + rnd(i, 2, 131), 4, WOOD); // crate
			++crates;
		}
	}
	for (int i = 0, placedClutter = 0; placedClutter < opt.clutter && i < opt.clutter * 4; ++i) {
		const float x = (rnd(i, 3, 977) * 2.0f - 1.0f) * (half - cell), z = (rnd(i, 4, 977) * 2.0f - 1.0f) * (half - cell);
		if (height(x, z) < waterLevel + 30.0f) continue;
		++placedClutter;
		if (i % 3 == 0) {
			b.detail = DETAIL_MEDIUM;
			box(x, z, 110.0f, 160.0f, rnd(i, 5, 977) * 3.0f, 4, WOOD);
		} else {
			b.detail = DETAIL_SMALL;
			box(x, z, 35.0f, 80.0f + 40.0f * rnd(i, 6, 977), rnd(i, 5, 977) * 3.0f, 8, WOOD);
		}
	}
	b.detail = DETAIL_ALWAYS;

	// ---- a gate that leads into the cave world, with a glow and a cobweb ----
	const float gateX = 1500.0f, gateZ = campRadius * 1.9f;
	const float gateY = height(gateX, gateZ);
	for (float side : {-1.0f, 1.0f}) box(gateX + side * 230.0f, gateZ, 30.0f, 330.0f, 0.785f, 4, WOOD);
	{
		const V3 n {0, 0, 1};
		const float x0 = gateX - 200.0f, x1 = gateX + 200.0f, y0 = gateY + 10.0f, y1 = gateY + 310.0f;
		// One face each: the blended pass draws both sides.
		b.quad(b.vert({x0, y0, gateZ}, 0, 1, n), b.vert({x1, y0, gateZ}, 1, 1, n), b.vert({x1, y1, gateZ}, 1, 0, n),
		       b.vert({x0, y1, gateZ}, 0, 0, n), n, GLOW);
		b.quad(b.vert({x0, y0, gateZ + 6.0f}, 0, 1, n), b.vert({x1, y0, gateZ + 6.0f}, 1, 1, n),
		       b.vert({x1, y1, gateZ + 6.0f}, 1, 0, n), b.vert({x0, y1, gateZ + 6.0f}, 0, 0, n), n, WEB);
		// Glow is light: full brightness, untouched by the time of day.
		for (size_t t = scene.triangles.size() - 4; t < scene.triangles.size() - 2; ++t)
			for (SrcVertex& v : scene.triangles[t].v) {
				v.rgba[0] = v.rgba[1] = v.rgba[2] = 255;
				v.rgba[3] = 0;
			}
	}
	SrcPortal portal;
	const float pmin[3] = {gateX - 200.0f, gateY - 50.0f, gateZ - 120.0f}, pmax[3] = {gateX + 200.0f, gateY + 320.0f, gateZ + 120.0f};
	std::copy_n(pmin, 3, portal.bbMin);
	std::copy_n(pmax, 3, portal.bbMax);
	portal.level = "CAVE.ZEN";
	portal.point = "CAVE_ENTRY";
	scene.portals.push_back(portal);
	// Where the cave's exit leads back to: in front of the gate, facing away.
	scene.points["CAMP_GATE"] = SrcPoint {{gateX, height(gateX, gateZ + 600.0f), gateZ + 600.0f}, 0.0f};

	// Where the script puts its characters (way points, in the game's terms).
	const struct {
		const char* name;
		float x, z, yaw;
	} places[] = {{"CAMP_GATE_GUARD_L", -200.0f, campRadius * 1.1f, 0.0f},
	              {"CAMP_GATE_GUARD_R", 200.0f, campRadius * 1.1f, 0.0f},
	              {"CAMP_SQUARE", 0.0f, 300.0f, 1.5708f},
	              {"CAMP_FIRE", 350.0f, -200.0f, 3.14159f},
	              {"CAMP_SMITH", -500.0f, 500.0f, -1.5708f},
	              {"CAMP_HUT_1", 900.0f, 900.0f, 0.0f},
	              {"CAMP_HUT_2", -900.0f, 800.0f, 0.0f},
	              {"ROAD_1", -250.0f, campRadius * 1.9f - 450.0f, 0.4f},
	              {"FOREST_SCAVENGER_1", 700.0f, campRadius * 1.9f - 650.0f, -2.0f},
	              {"FOREST_SCAVENGER_2", 1000.0f, campRadius * 1.9f - 450.0f, 2.5f},
	              {"FOREST_SCAVENGER_3", 850.0f, campRadius * 1.9f - 300.0f, 1.0f},
	              {"FOREST_MOLERAT_1", -900.0f, campRadius * 1.9f - 550.0f, 0.0f}};
	for (const auto& place : places) {
		scene.points[place.name] = SrcPoint {{place.x, height(place.x, place.z), place.z}, place.yaw};
		scene.wayPoints.push_back(SrcWayPoint {place.name, {place.x, height(place.x, place.z), place.z}, place.yaw});
	}
	// The paths between them: out of the gate from either guard post, along
	// the road and into the wood; inside from the square to everything.
	// (Nobody walks through the palisade: the gate posts connect only through
	// the gate itself.)
	scene.wayPoints.push_back(SrcWayPoint {"CAMP_GATE", {0.0f, height(0.0f, campRadius), campRadius}, 0.0f});
	const auto wayIndex = [&](const char* name) {
		for (size_t i = 0; i < scene.wayPoints.size(); ++i)
			if (scene.wayPoints[i].name == name) return u32(i);
		throw std::logic_error(std::string("no way point ") + name);
	};
	const char* const ways[][2] = {{"CAMP_GATE_GUARD_L", "CAMP_GATE"},   {"CAMP_GATE_GUARD_R", "CAMP_GATE"}, {"CAMP_GATE", "CAMP_SQUARE"},
	                               {"CAMP_SQUARE", "CAMP_FIRE"},         {"CAMP_SQUARE", "CAMP_SMITH"},      {"CAMP_FIRE", "CAMP_HUT_1"},
	                               {"CAMP_SMITH", "CAMP_HUT_2"},         {"CAMP_SQUARE", "CAMP_HUT_2"},      {"CAMP_GATE_GUARD_L", "ROAD_1"},
	                               {"CAMP_GATE_GUARD_R", "ROAD_1"},      {"ROAD_1", "FOREST_MOLERAT_1"},     {"ROAD_1", "FOREST_SCAVENGER_1"},
	                               {"FOREST_SCAVENGER_1", "FOREST_SCAVENGER_2"}, {"FOREST_SCAVENGER_2", "FOREST_SCAVENGER_3"},
	                               {"FOREST_SCAVENGER_3", "ROAD_1"}};
	for (const auto& way : ways) scene.wayEdges.push_back({wayIndex(way[0]), wayIndex(way[1])});
	// Free points: seats round the fire, a place to stand and talk, and spots
	// the creatures roam between.
	const struct {
		const char* name;
		float x, z, yaw;
	} spots[] = {{"FP_SIT_CAMPFIRE_01", 350.0f + 170.0f, -200.0f, -1.5708f},
	             {"FP_SIT_CAMPFIRE_02", 350.0f - 170.0f, -200.0f, 1.5708f},
	             {"FP_SIT_CAMPFIRE_03", 350.0f, -200.0f - 170.0f, 0.0f},
	             {"FP_SMALLTALK_SQUARE_01", -120.0f, 380.0f, 1.5708f},
	             {"FP_SMALLTALK_SQUARE_02", 120.0f, 380.0f, -1.5708f},
	             {"FP_GUARD_GATE_L", -200.0f, campRadius * 1.1f + 60.0f, 0.0f},
	             {"FP_ROAM_FOREST_01", 600.0f, campRadius * 1.9f - 500.0f, 0.5f},
	             {"FP_ROAM_FOREST_02", 1150.0f, campRadius * 1.9f - 600.0f, 2.0f},
	             {"FP_ROAM_FOREST_03", 900.0f, campRadius * 1.9f - 150.0f, -1.0f},
	             {"FP_ROAM_FOREST_04", -1000.0f, campRadius * 1.9f - 400.0f, 3.0f}};
	for (const auto& spot : spots) scene.freePoints.push_back(SrcWayPoint {spot.name, {spot.x, height(spot.x, spot.z), spot.z}, spot.yaw});

	// Things to use: a bench by the fire, a bed at each hut (two at the first), an anvil at the
	// smith's - and a chest, which no script ever names.
	{
		const auto thing = [&](const char* kind, float x, float z, float yaw, int states, float w, float h) {
			b.detail = DETAIL_MEDIUM;
			box(x, z, w, h, yaw + 0.785f, 4, WOOD);
			b.detail = DETAIL_ALWAYS;
			SrcMob mob;
			mob.scheme = kind;
			mob.visual = std::string(kind) + "_SYNTH.ASC";
			mob.pos[0] = x;
			mob.pos[1] = height(x, z);
			mob.pos[2] = z;
			mob.yaw = yaw;
			mob.states = states;
			scene.mobs.push_back(mob);
			return scene.mobs.size() - 1;
		};
		const auto slot = [&](size_t mob, const char* name, float dx, float dz, float yaw, float up = 0.0f) {
			const float x = scene.mobs[mob].pos[0] + dx, z = scene.mobs[mob].pos[2] + dz;
			scene.mobs[mob].slots.push_back(SrcMobSlot {name, {x, height(x, z) + up, z}, yaw});
		};
		// The bench, facing the fire: two places side by side in front of it,
		// backs to it - left a metre up, at the height of the nodes they come
		// from, as the game's are where the baker finds no ground of the world
		// under them (a floor that is an object): one sits on the floor all the same.
		const size_t bench = thing("BENCH", 350.0f, 110.0f, 3.14159f, 1, 90.0f, 42.0f);
		slot(bench, "ZS_POS0", -45.0f, -110.0f, 3.14159f, 100.0f);
		slot(bench, "ZS_POS1", 45.0f, -110.0f, 3.14159f, 100.0f);
		scene.mobs[bench].focus = "MOBNAME_BENCH";
		scene.mobs[bench].kind = 0; // MOBKIND_INTER: a place to be at (with no function to run); a bed has its sleep
		// Beds: got into from the front.
		// (On the near side of the hut's wall: a bed behind it is not seen,
		// and nobody walks through the wall to it.)
		const size_t bed1 = thing("BEDHIGH", 900.0f - 190.0f, 900.0f, -1.5708f, 1, 110.0f, 45.0f);
		slot(bed1, "ZS_POS0_FRONT", 125.0f, 0.0f, -1.5708f);
		slot(bed1, "ZS_POS1_BACK", -125.0f, 0.0f, 1.5708f);
		scene.mobs[bed1].focus = "MOBNAME_BED";
		scene.mobs[bed1].onState = "SLEEPABIT"; // (the player's sleep: SLEEPABIT_S1)
		scene.mobs[bed1].kind = 1;
		const size_t bed2 = thing("BEDHIGH", -900.0f + 190.0f, 800.0f, 1.5708f, 1, 110.0f, 45.0f);
		slot(bed2, "ZS_POS0_FRONT", -125.0f, 0.0f, 1.5708f);
		scene.mobs[bed2].kind = 1; // (a bed with no name: the player may sleep in it all the same)
		// A low bed by the first hut, too, behind the wall: the sleeper's
		// script asks for both kinds of bed, and is to stay in the high one
		// it got into first - and never to go through the wall to this one.
		const size_t bed3 = thing("BEDLOW", 900.0f + 190.0f, 900.0f, 1.5708f, 1, 110.0f, 30.0f);
		slot(bed3, "ZS_POS0", -125.0f, 0.0f, 1.5708f);
		// (This one the scripts never use: the player's own, with a condition.)
		scene.mobs[bed3].focus = "MOBNAME_ANVIL";
		scene.mobs[bed3].onState = "ANVIL_S1";
		scene.mobs[bed3].condition = "ANVIL_COND";
		scene.mobs[bed3].kind = 0; // (a thing that does its work: not a bed to sleep in)
		// The anvil: one stands a pace from it, on whichever side one comes from.
		const size_t anvil = thing("BSANVIL", -500.0f - 160.0f, 500.0f, 0.0f, 0, 32.0f, 75.0f);
		slot(anvil, "ZS_POS0_DIST", 0.0f, 75.0f, 3.14159f);
		scene.mobs[anvil].focus = "MOBNAME_ANVIL";
		scene.mobs[anvil].onState = "SMITHWEAPON"; // (fix153: what to make, the player's own talk; SMITHWEAPON_S1 / _S0)
		scene.mobs[anvil].kind = 0;
		// (fix154) A forge, the Gothic 1 way: no function, a thing it wants (a nugget); the animations' item tags turn it
		// into a sword while he is at it.
		const size_t forge = thing("BSFIRE", -500.0f - 160.0f, 500.0f + 260.0f, 0.0f, 1, 32.0f, 75.0f);
		slot(forge, "ZS_POS0_DIST", 0.0f, 75.0f, 3.14159f);
		scene.mobs[forge].focus = "MOBNAME_FORGE";
		scene.mobs[forge].useItem = "ITMINUGGET";
		scene.mobs[forge].kind = 0;
		// The chests: an open one with a few things in it, among them a key and
		// lock picks; a small one that the key opens; a small one that only a pick
		// opens, turning it left, right, left. Each is stood before from the north.
		const auto chest = [&](const char* kind, float x, float z, float w, float h, const char* contents, bool locked, const char* key,
		                       const char* pick) {
			const size_t c = thing(kind, x, z, 0.0f, 1, w, h);
			slot(c, "ZS_POS0", 0.0f, 90.0f, 3.14159f);
			SrcMob& m = scene.mobs[c];
			m.container = true;
			m.locked = locked;
			m.key = key;
			m.pick = pick;
			m.contents = contents;
		};
		chest("CHESTBIG", -500.0f, 500.0f - 260.0f, 55.0f, 60.0f, "ITMINUGGET:75, ITFOAPPLE:2, ITKELOCKPICK:3, ITKE_CAMPCHEST:1", false, "", "");
		chest("CHESTSMALL", -330.0f, 500.0f - 260.0f, 38.0f, 40.0f, "ITFO_POTION_HEALTH_01:2", true, "ITKE_CAMPCHEST", "");
		chest("CHESTSMALL", -680.0f, 500.0f - 260.0f, 38.0f, 40.0f, "ITMINUGGET:20", true, "", "LRL");
		// A ledge: a block of stone 150 cm high in the camp, to jump up to
		// (a wall the hands take hold of).
		b.detail = DETAIL_MEDIUM;
		box(-700.0f, -700.0f, 150.0f, 150.0f, 0.785f, 4, WOOD);
		// ... one to hang from (2.3 m: the high climb) and a wall too tall for any (4.2 m: a jump straight up).
		box(-1100.0f, -700.0f, 150.0f, 230.0f, 0.785f, 4, WOOD);
		box(-1500.0f, -700.0f, 150.0f, 420.0f, 0.785f, 4, WOOD);
		b.detail = DETAIL_ALWAYS;
	}

	// Start just outside the gate, looking back at the camp.
	scene.spawnPos[0] = 0;
	scene.spawnPos[2] = campRadius * 1.9f;
	scene.spawnPos[1] = height(0, scene.spawnPos[2]) + 175.0f; // eye height
	scene.spawnYaw = 3.14159f;
	scene.hasSpawn = true;
	{
		// A steady hum 4 m from where the player starts, heard 25 m around.
		SrcAmbient hum;
		hum.sound = "SFX_TEST_HUM";
		hum.pos[0] = scene.spawnPos[0] + 400.0f;
		hum.pos[1] = scene.spawnPos[1] - 100.0f;
		hum.pos[2] = scene.spawnPos[2];
		hum.radius = 2500.0f;
		hum.mode = 0;
		hum.volume = 0.8f;
		scene.ambient.push_back(hum);
	}
	{
		// The whole world is one music zone: TEST_DAY_STD by day, TEST_NGT_STD at night, TEST_DAY_FGT in a fight by day.
		SrcMusicZone zone;
		zone.name = "TEST";
		zone.isDefault = true;
		for (int k = 0; k < 3; ++k) {
			zone.bbMin[k] = -1.0e7f;
			zone.bbMax[k] = 1.0e7f;
		}
		scene.musicZones.push_back(zone);
	}
	// Things lying in the grass before the start (the world file's oCItem in
	// the game): to pick up. The scripts put one more at the smith's.
	{
		const struct {
			const char* instance;
			float dx, dz, yaw;
			int amount;
		} kLying[] = {{"ITFOAPPLE", 40.0f, -170.0f, 0.3f, 1},
		              {"ITFO_POTION_HEALTH_01", -60.0f, -200.0f, 0.0f, 1},
		              {"ITMW_CLUB", 120.0f, -260.0f, 1.2f, 1},
		              {"ITMINUGGET", -150.0f, -300.0f, 0.0f, 5}};
		for (const auto& l : kLying) {
			SrcWorldItem it;
			it.instance = l.instance;
			it.pos[0] = scene.spawnPos[0] + l.dx;
			it.pos[2] = scene.spawnPos[2] + l.dz;
			it.pos[1] = height(it.pos[0], it.pos[2]);
			it.rot[1] = std::sin(l.yaw * 0.5f);
			it.rot[3] = std::cos(l.yaw * 0.5f);
			it.amount = l.amount;
			scene.items.push_back(it);
		}
	}
	{
		// A fire and its smoke, a few steps from where the player starts.
		for (const char* effect : {"SYNTH_FIRE", "SYNTH_SMOKE"}) {
			SrcEmitter e;
			e.effect = effect;
			e.pos[0] = scene.spawnPos[0] + 250.0f;
			e.pos[2] = scene.spawnPos[2] - 400.0f;
			e.pos[1] = height(e.pos[0], e.pos[2]) + (effect[6] == 'S' ? 60.0f : 10.0f);
			scene.emitters.push_back(e);
		}
		// fix148: its light, flickering as a fire place's tree light does (range and colour keys).
		SrcLight l;
		l.pos[0] = scene.spawnPos[0] + 250.0f;
		l.pos[2] = scene.spawnPos[2] - 400.0f;
		l.pos[1] = height(l.pos[0], l.pos[2]) + 80.0f;
		l.range = 800.0f;
		l.color[0] = 255;
		l.color[1] = 180;
		l.color[2] = 100;
		l.rangeKeys = {1.0f, 0.9f, 1.05f, 0.95f};
		l.rangeFps = 8.0f;
		l.colorKeys = {{255, 180, 100}, {255, 160, 80}, {240, 170, 90}};
		l.colorFps = 5.0f;
		l.fire = true;
		scene.lights.push_back(l);
	}
	scene.notes.push_back("synthetic scene: " + std::to_string(N) + "x" + std::to_string(N) + " terrain cells, " +
	                      std::to_string(opt.huts) + " huts, " + std::to_string(placed) + " trees, " +
	                      std::to_string(barrels) + " barrels, " + std::to_string(crates) + " crates");
}

// A lightmapped rock chamber, 30 x 40 m, with a pool whose floor slopes from
// the entrance side down to 3 m, so it can be waded into, swum and left again.
void buildSyntheticCave(SrcScene& scene) {
	enum { ROCK = 1, DIRT = 2, WOOD = 3, WATER = 5 };
	addMaterials(scene);
	Builder b {scene};
	scene.indoor = true;
	scene.fogColor[0] = 14;
	scene.fogColor[1] = 12;
	scene.fogColor[2] = 10;

	const float x0 = -1500.0f, x1 = 1500.0f, z0 = 0.0f, z1 = 4000.0f, top = 800.0f;
	const float poolX = 500.0f, poolZ0 = 1500.0f, poolZ1 = 2750.0f, poolDepth = 300.0f, waterY = -20.0f;
	// Indoor light: dim, warm, and not tinted by the time of day (alpha 0).
	auto lit = [&](V3 p, float u, float v, float shade) {
		SrcVertex s = b.vert(p, u, v, {0, 1, 0});
		const float d = std::clamp(shade * (0.75f + 0.25f * std::sin(p.x * 0.004f) * std::cos(p.z * 0.003f)), 0.0f, 1.0f);
		s.rgba[0] = u8(150 * d);
		s.rgba[1] = u8(128 * d);
		s.rgba[2] = u8(104 * d);
		s.rgba[3] = 0;
		return s;
	};
	const float cell = 250.0f;
	for (float z = z0; z < z1; z += cell)
		for (float x = x0; x < x1; x += cell) {
			const float cx = x + cell * 0.5f, cz = z + cell * 0.5f;
			const bool pool = std::fabs(cx) < poolX && cz > poolZ0 && cz < poolZ1;
			auto P = [&](float px, float pz) {
				// Pool cells use the sloped bottom, everything else is flat.
				return V3 {px, pool ? -poolDepth * (std::clamp(pz, poolZ0, poolZ1) - poolZ0) / (poolZ1 - poolZ0) : 0.0f, pz};
			};
			b.quad(lit(P(x, z), x / 300.0f, z / 300.0f, pool ? 0.6f : 1.0f), lit(P(x + cell, z), (x + cell) / 300.0f, z / 300.0f, pool ? 0.6f : 1.0f),
			       lit(P(x + cell, z + cell), (x + cell) / 300.0f, (z + cell) / 300.0f, pool ? 0.6f : 1.0f),
			       lit(P(x, z + cell), x / 300.0f, (z + cell) / 300.0f, pool ? 0.6f : 1.0f), {0, 1, 0}, DIRT);
			if (pool)
				b.quad(lit({x, waterY, z}, x / 700.0f, z / 700.0f, 1.0f), lit({x + cell, waterY, z}, (x + cell) / 700.0f, z / 700.0f, 1.0f),
				       lit({x + cell, waterY, z + cell}, (x + cell) / 700.0f, (z + cell) / 700.0f, 1.0f),
				       lit({x, waterY, z + cell}, x / 700.0f, (z + cell) / 700.0f, 1.0f), {0, 1, 0}, WATER);
			// Ceiling.
			b.quad(lit({x, top, z}, x / 300.0f, z / 300.0f, 0.5f), lit({x + cell, top, z}, (x + cell) / 300.0f, z / 300.0f, 0.5f),
			       lit({x + cell, top, z + cell}, (x + cell) / 300.0f, (z + cell) / 300.0f, 0.5f),
			       lit({x, top, z + cell}, x / 300.0f, (z + cell) / 300.0f, 0.5f), {0, -1, 0}, ROCK);
		}
	// The pool's side and far walls (the near edge is the ramp).
	auto wall = [&](V3 a, V3 c, V3 inward, float bottomA, float bottomC, float topY) {
		b.quad(lit({a.x, bottomA, a.z}, 0, 1, 0.8f), lit({c.x, bottomC, c.z}, 2, 1, 0.8f), lit({c.x, topY, c.z}, 2, 0, 0.8f),
		       lit({a.x, topY, a.z}, 0, 0, 0.8f), inward, ROCK);
	};
	wall({-poolX, 0, poolZ0}, {-poolX, 0, poolZ1}, {1, 0, 0}, 0.0f, -poolDepth, 0.0f);
	wall({poolX, 0, poolZ0}, {poolX, 0, poolZ1}, {-1, 0, 0}, 0.0f, -poolDepth, 0.0f);
	wall({-poolX, 0, poolZ1}, {poolX, 0, poolZ1}, {0, 0, -1}, -poolDepth, -poolDepth, 0.0f);
	// The chamber's four walls.
	wall({x0, 0, z0}, {x1, 0, z0}, {0, 0, 1}, 0, 0, top);
	wall({x0, 0, z1}, {x1, 0, z1}, {0, 0, -1}, 0, 0, top);
	wall({x0, 0, z0}, {x0, 0, z1}, {1, 0, 0}, 0, 0, top);
	wall({x1, 0, z0}, {x1, 0, z1}, {-1, 0, 0}, 0, 0, top);

	// A ledge in the far left corner, 3 m up, reached by a ladder.
	const float ledgeX = -900.0f, ledgeZ = 3400.0f, ledgeY = 300.0f;
	b.quad(lit({x0, ledgeY, ledgeZ}, 0, 0, 1.0f), lit({ledgeX, ledgeY, ledgeZ}, 2, 0, 1.0f), lit({ledgeX, ledgeY, z1}, 2, 2, 1.0f),
	       lit({x0, ledgeY, z1}, 0, 2, 1.0f), {0, 1, 0}, DIRT);
	wall({x0, 0, ledgeZ}, {ledgeX, 0, ledgeZ}, {0, 0, -1}, 0, 0, ledgeY);
	wall({ledgeX, 0, ledgeZ}, {ledgeX, 0, z1}, {1, 0, 0}, 0, 0, ledgeY);
	SrcLadder ladder;
	const float lmin[3] = {-1230.0f, 0.0f, ledgeZ - 12.0f}, lmax[3] = {-1170.0f, ledgeY + 20.0f, ledgeZ};
	std::copy_n(lmin, 3, ladder.bbMin);
	std::copy_n(lmax, 3, ladder.bbMax);
	scene.ladders.push_back(ladder);
	{
		// The ladder itself: a plank leaning on the ledge's front.
		const V3 n {0, 0, -1};
		b.quad(lit({lmin[0], lmin[1], lmin[2]}, 0, 3, 1.0f), lit({lmax[0], lmin[1], lmin[2]}, 0.5f, 3, 1.0f),
		       lit({lmax[0], lmax[1], lmin[2]}, 0.5f, 0, 1.0f), lit({lmin[0], lmax[1], lmin[2]}, 0, 0, 1.0f), n, WOOD);
	}

	SrcPortal portal;
	const float pmin[3] = {-200.0f, -50.0f, 80.0f}, pmax[3] = {200.0f, 320.0f, 380.0f};
	std::copy_n(pmin, 3, portal.bbMin);
	std::copy_n(pmax, 3, portal.bbMax);
	portal.level = "WORLD.ZEN";
	portal.point = "CAMP_GATE";
	scene.portals.push_back(portal);
	scene.points["CAVE_ENTRY"] = SrcPoint {{0.0f, 0.0f, 900.0f}, 0.0f};
	scene.points["CAVE_GUARD"] = SrcPoint {{700.0f, 0.0f, 1200.0f}, 3.14159f};
	scene.points["CAVE_MOLERAT_1"] = SrcPoint {{-800.0f, 0.0f, 1300.0f}, 1.0f};
	scene.points["CAVE_MOLERAT_2"] = SrcPoint {{900.0f, 0.0f, 3200.0f}, -2.0f};
	for (const char* name : {"CAVE_ENTRY", "CAVE_GUARD", "CAVE_MOLERAT_1", "CAVE_MOLERAT_2"}) {
		const SrcPoint& p = scene.points[name];
		scene.wayPoints.push_back(SrcWayPoint {name, {p.pos[0], p.pos[1], p.pos[2]}, p.yaw});
	}
	scene.wayEdges = {{0, 1}, {0, 2}, {1, 3}};
	scene.freePoints.push_back(SrcWayPoint {"FP_ROAM_CAVE_01", {-500.0f, 0.0f, 1700.0f}, 0.0f});
	scene.freePoints.push_back(SrcWayPoint {"FP_ROAM_CAVE_02", {-900.0f, 0.0f, 1000.0f}, 1.0f});

	scene.spawnPos[0] = 0;
	scene.spawnPos[1] = 175.0f;
	scene.spawnPos[2] = 900.0f;
	scene.spawnYaw = 0;
	scene.hasSpawn = true;
	scene.notes.push_back("synthetic cave: one chamber with a pool, " + std::to_string(scene.triangles.size()) + " triangles");
}

} // namespace g3
