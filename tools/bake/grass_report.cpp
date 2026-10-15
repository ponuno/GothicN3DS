#include "grass_report.h"

#include "g3ds/world.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "../../third_party/stb/stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace g3 {

namespace {

// ZenGin's material groups (zenkit::MaterialGroup).
enum : u16 { GROUP_UNDEF = 0, GROUP_METAL = 1, GROUP_STONE = 2, GROUP_WOOD = 3, GROUP_EARTH = 4, GROUP_WATER = 5, GROUP_SNOW = 6 };

const char* groupName(u16 g) {
	switch (g) {
	case GROUP_METAL: return "metal";
	case GROUP_STONE: return "stone";
	case GROUP_WOOD: return "wood";
	case GROUP_EARTH: return "earth";
	case GROUP_WATER: return "water";
	case GROUP_SNOW: return "snow";
	default: return "-";
	}
}

std::string upper(std::string s) {
	for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
	return s;
}

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
	char buf[1024];
	va_list ap;
	va_start(ap, f);
	std::vsnprintf(buf, sizeof(buf), f, ap);
	va_end(ap);
	return buf;
}

// The words of a name (cut at everything but letters and digits).
std::vector<std::string> wordsOf(const std::string& name) {
	std::vector<std::string> out;
	std::string w;
	for (char c : name) {
		if (std::isalnum(static_cast<unsigned char>(c))) w += c;
		else if (!w.empty()) out.push_back(std::move(w)), w.clear();
	}
	if (!w.empty()) out.push_back(w);
	return out;
}

// A long word anywhere in the name; a short one (3 letters) only as the beginning of a word.
bool nameHas(const std::string& up, const std::vector<std::string>& words, const char* const* list, size_t n, std::string& which) {
	for (size_t i = 0; i < n; ++i) {
		const std::string k = list[i];
		if (k.size() >= 4) {
			if (up.find(k) != std::string::npos) return which = k, true;
		} else {
			for (const std::string& w : words)
				if (w.compare(0, k.size(), k) == 0) return which = k, true;
		}
	}
	return false;
}

struct TexStat {
	std::string key;   // the texture's name, upper case ("(colour) NAME" for a material without one)
	std::string texture;
	u16 group = 0;
	double area = 0;   // m², all of its world triangles
	double upArea = 0; // m², facing up (more than 11 degrees above the vertical)
	double flatArea = 0; // m², up to 25 degrees
	double lightSum = 0, lightWeight = 0;
	double topArea = 0; // m², the cells of the map where it is the topmost ground
	bool hasColour = false;
	float colour[3] = {0, 0, 0};
	float weight = 0;
	bool ruled = false;
	std::string why;
};

struct Rule {
	std::string name; // upper case; prefix when `prefix`
	bool prefix = false;
	float weight = 0;
};

std::vector<Rule> readRules(const std::string& file, std::string& note) {
	std::vector<Rule> rules;
	if (file.empty()) return rules;
	std::ifstream in(file);
	if (!in) {
		note = "grass rules: cannot read '" + file + "' (the automatic weights are used)";
		return rules;
	}
	std::string line;
	while (std::getline(in, line)) {
		const size_t hash = line.find('#');
		if (hash != std::string::npos) line.resize(hash);
		std::istringstream ls(line);
		std::string name;
		float w;
		if (!(ls >> name >> w)) continue;
		Rule r;
		r.name = upper(name);
		if (!r.name.empty() && r.name.back() == '*') {
			r.prefix = true;
			r.name.pop_back();
		}
		r.weight = std::clamp(w, 0.0f, 1.0f);
		rules.push_back(r);
	}
	note = "grass rules: " + std::to_string(rules.size()) + " from " + file;
	return rules;
}

const Rule* ruleFor(const std::vector<Rule>& rules, const std::string& key) {
	const Rule* best = nullptr;
	for (const Rule& r : rules) {
		if (!r.prefix && r.name == key) return &r;
		if (r.prefix && key.compare(0, r.name.size(), r.name) == 0 && (!best || r.name.size() > best->name.size())) best = &r;
	}
	return best;
}

// The weight a texture gets by itself: green in colour, earth, a name that says grass; less for stone, wood, paths.
float autoWeight(const TexStat& t, std::string& why) {
	static const char* const kGood[] = {"GRAS", "WIESE", "RASEN", "MOOS", "MOSS", "MEADOW", "LAWN", "WALDBODEN", "FOREST", "HEIDE", "HEATH"};
	static const char* const kBad[] = {"ROAD", "STREET", "STRASSE", "PATH", "PFAD", "WEG", "STONE", "STEIN", "FELS", "ROCK", "CLIFF", "WALL",
	                                   "MAUER", "BRICK", "WOOD", "HOLZ", "PLANK", "BRETT", "ROOF", "DACH", "SAND", "MUD", "SCHLAMM",
	                                   "LAVA", "CAVE", "HOEHLE", "MINE", "SNOW", "SCHNEE", "WATER", "WASSER", "FLOOR", "BODEN_STEIN",
	                                   "PFLASTER", "COBBLE", "TILE", "METAL", "ERZ", "ORE"};
	if (t.group == GROUP_WATER) return why = "water", 0.0f;
	float w;
	if (t.hasColour) {
		const float green = (t.colour[1] - 0.5f * (t.colour[0] + t.colour[2])) / 255.0f;
		w = std::clamp((green - 0.015f) / 0.07f, 0.0f, 1.0f);
		why = fmt("green %+.3f", double(green));
	} else {
		w = 0.3f;
		why = "no colour";
	}
	if (t.group == GROUP_EARTH && w < 0.3f) {
		w = 0.3f;
		why += ", earth";
	} else if (t.group == GROUP_STONE || t.group == GROUP_METAL || t.group == GROUP_WOOD || t.group == GROUP_SNOW) {
		w *= 0.3f;
		why += std::string(", ") + groupName(t.group);
	}
	const std::string up = upper(t.texture.empty() ? t.key : t.texture);
	const std::vector<std::string> words = wordsOf(up);
	std::string which;
	if (nameHas(up, words, kBad, sizeof(kBad) / sizeof(kBad[0]), which)) {
		w = std::min(w, 0.15f);
		why += ", name " + which;
	} else if (nameHas(up, words, kGood, sizeof(kGood) / sizeof(kGood[0]), which)) {
		w = std::max(w, 0.85f);
		why += ", name " + which;
	}
	return w;
}

float smooth01(float x) {
	x = std::clamp(x, 0.0f, 1.0f);
	return x * x * (3.0f - 2.0f * x);
}

u32 hash2(int x, int y) {
	u32 h = u32(x) * 374761393u + u32(y) * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

// Value noise, 0..1, cells of `size` cm.
float valueNoise(float x, float z, float size, u32 seed) {
	const float fx = x / size, fz = z / size;
	const int ix = int(std::floor(fx)), iz = int(std::floor(fz));
	const float tx = smooth01(fx - float(ix)), tz = smooth01(fz - float(iz));
	const auto at = [&](int a, int b) { return float(hash2(a + int(seed), b - int(seed) * 7) & 0xFFFFu) / 65535.0f; };
	const float top = at(ix, iz) + (at(ix + 1, iz) - at(ix, iz)) * tx;
	const float bottom = at(ix, iz + 1) + (at(ix + 1, iz + 1) - at(ix, iz + 1)) * tx;
	return top + (bottom - top) * tz;
}

struct Grid {
	float x0 = 0, z0 = 0, cell = 100;
	int w = 0, h = 0;
	size_t at(int x, int z) const { return size_t(z) * size_t(w) + size_t(x); }
	bool cellOf(float x, float z, int& cx, int& cz) const {
		cx = int(std::floor((x - x0) / cell));
		cz = int(std::floor((z - z0) / cell));
		return cx >= 0 && cz >= 0 && cx < w && cz < h;
	}
	float cx(int x) const { return x0 + (float(x) + 0.5f) * cell; }
	float cz(int z) const { return z0 + (float(z) + 0.5f) * cell; }
};

// Calls f(cellX, cellZ, y) for every cell whose middle the triangle covers seen from above (at least the cell of its middle).
template <class F>
void rasterize(const Grid& g, const SrcTriangle& t, F&& f) {
	const float* a = t.v[0].pos;
	const float* b = t.v[1].pos;
	const float* c = t.v[2].pos;
	const float area2 = (b[0] - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (b[2] - a[2]);
	if (std::fabs(area2) < 1.0f) return; // standing on its edge seen from above (a wall)
	const float lo[2] = {std::min({a[0], b[0], c[0]}), std::min({a[2], b[2], c[2]})};
	const float hi[2] = {std::max({a[0], b[0], c[0]}), std::max({a[2], b[2], c[2]})};
	const int x0 = std::max(0, int(std::ceil((lo[0] - g.x0) / g.cell - 0.5f)));
	const int x1 = std::min(g.w - 1, int(std::floor((hi[0] - g.x0) / g.cell - 0.5f)));
	const int z0 = std::max(0, int(std::ceil((lo[1] - g.z0) / g.cell - 0.5f)));
	const int z1 = std::min(g.h - 1, int(std::floor((hi[1] - g.z0) / g.cell - 0.5f)));
	bool any = false;
	const float inv = 1.0f / area2;
	for (int z = z0; z <= z1; ++z)
		for (int x = x0; x <= x1; ++x) {
			const float px = g.cx(x), pz = g.cz(z);
			const float wb = ((px - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (pz - a[2])) * inv;
			const float wc = ((b[0] - a[0]) * (pz - a[2]) - (px - a[0]) * (b[2] - a[2])) * inv;
			const float wa = 1.0f - wb - wc;
			if (wa < -1e-4f || wb < -1e-4f || wc < -1e-4f) continue;
			any = true;
			f(x, z, wa * a[1] + wb * b[1] + wc * c[1]);
		}
	if (!any) {
		int x, z;
		if (g.cellOf((a[0] + b[0] + c[0]) / 3.0f, (a[2] + b[2] + c[2]) / 3.0f, x, z)) f(x, z, (a[1] + b[1] + c[1]) / 3.0f);
	}
}

void normalOf(const SrcTriangle& t, float n[3]) {
	const float* a = t.v[0].pos;
	const float* b = t.v[1].pos;
	const float* c = t.v[2].pos;
	const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
	n[0] = e1[1] * e2[2] - e1[2] * e2[1];
	n[1] = e1[2] * e2[0] - e1[0] * e2[2];
	n[2] = e1[0] * e2[1] - e1[1] * e2[0];
	const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	if (len > 0) n[0] /= len, n[1] /= len, n[2] /= len;
}

float areaOf(const SrcTriangle& t) {
	const float* a = t.v[0].pos;
	const float* b = t.v[1].pos;
	const float* c = t.v[2].pos;
	const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
	const float x = e1[1] * e2[2] - e1[2] * e2[1], y = e1[2] * e2[0] - e1[0] * e2[2], z = e1[0] * e2[1] - e1[1] * e2[0];
	return 0.5f * std::sqrt(x * x + y * y + z * z);
}

// Green -> yellow -> red.
void ramp(float t, float rgb[3]) {
	t = std::clamp(t, 0.0f, 1.0f);
	static const float k[3][3] = {{40, 150, 40}, {235, 220, 40}, {225, 40, 30}};
	const int i = t < 0.5f ? 0 : 1;
	const float f = t < 0.5f ? t * 2.0f : (t - 0.5f) * 2.0f;
	for (int c = 0; c < 3; ++c) rgb[c] = k[i][c] + (k[i + 1][c] - k[i][c]) * f;
}

std::string joinPath(const std::string& dir, const std::string& file) {
	if (dir.empty()) return file;
	const char last = dir.back();
	return dir + (last == '/' || last == '\\' ? "" : "/") + file;
}

} // namespace

std::string writeGrassReport(const std::string& worldName, const SrcScene& scene, const GrassReportOptions& opt) {
	if (scene.indoor) return "grass report: " + worldName + " is an indoor world (no sky): skipped";
	const size_t worldTris = scene.worldTriangles ? std::min(scene.worldTriangles, scene.triangles.size()) : scene.triangles.size();
	if (worldTris == 0) return "grass report: " + worldName + " has no world mesh: skipped";

	// Which way "up" winds: the ground is most of the area that faces up or down; whichever way it faces is up (the importer
	// keeps the source's winding, and the synthetic scene's is the other one).
	float upSign = 1.0f;
	{
		double sum = 0;
		for (size_t i = 0; i < worldTris; ++i) {
			float n[3];
			normalOf(scene.triangles[i], n);
			sum += double(n[1]) * double(areaOf(scene.triangles[i]));
		}
		if (sum < 0) upSign = -1.0f;
	}
	// ---- the world mesh's textures ----
	std::map<std::string, size_t> texIndex;
	std::vector<TexStat> texs;
	std::vector<int> triTex(worldTris, -1);
	float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
	for (size_t i = 0; i < worldTris; ++i) {
		const SrcTriangle& t = scene.triangles[i];
		const SrcMaterial& m = scene.materials[t.material];
		const std::string key = m.texture.empty() ? "(colour) " + upper(m.name) : upper(m.texture);
		auto it = texIndex.find(key);
		if (it == texIndex.end()) {
			it = texIndex.emplace(key, texs.size()).first;
			texs.emplace_back();
			texs.back().key = key;
			texs.back().texture = m.texture;
			texs.back().group = m.water ? u16(GROUP_WATER) : m.group;
			if (m.texture.empty() && m.hasColor) {
				texs.back().hasColour = true;
				for (int c = 0; c < 3; ++c) texs.back().colour[c] = m.color[c];
			}
		}
		triTex[i] = int(it->second);
		TexStat& ts = texs[it->second];
		float n[3];
		normalOf(t, n);
		n[1] *= upSign;
		const double a = double(areaOf(t)) / 10000.0;
		ts.area += a;
		if (n[1] > 0.2f) ts.upArea += a;
		if (n[1] > 0.906f) ts.flatArea += a; // cos 25°
		for (const SrcVertex& v : t.v) {
			ts.lightSum += a * (double(v.rgba[0]) + v.rgba[1] + v.rgba[2]) / 3.0;
			ts.lightWeight += a;
			for (int c = 0; c < 3; ++c) {
				lo[c] = std::min(lo[c], v.pos[c]);
				hi[c] = std::max(hi[c], v.pos[c]);
			}
		}
	}

	// ---- the map: the topmost world polygon over each cell ----
	Grid g;
	const float extent = std::max(hi[0] - lo[0], hi[2] - lo[2]);
	g.cell = std::max(opt.cellCm, extent / 2048.0f);
	g.x0 = lo[0];
	g.z0 = lo[2];
	g.w = std::max(1, int(std::ceil((hi[0] - lo[0]) / g.cell)));
	g.h = std::max(1, int(std::ceil((hi[2] - lo[2]) / g.cell)));
	const size_t cells = size_t(g.w) * size_t(g.h);
	const float NONE = -1e30f;
	std::vector<float> topY(cells, NONE), waterY(cells, NONE);
	std::vector<int> topTri(cells, -1);
	for (size_t i = 0; i < worldTris; ++i) {
		const SrcTriangle& t = scene.triangles[i];
		const bool water = scene.materials[t.material].water;
		const SrcBlend blend = scene.materials[t.material].blend;
		if (!water && blend != SrcBlend::Opaque) continue; // (light shafts, glow: not ground)
		rasterize(g, t, [&](int x, int z, float y) {
			const size_t k = g.at(x, z);
			if (water) {
				waterY[k] = std::max(waterY[k], y);
			} else if (y > topY[k]) {
				topY[k] = y;
				topTri[k] = int(i);
			}
		});
	}
	// Ground: the topmost polygon faces up (else it is a wall or an overhang's underside: nothing grows there).
	std::vector<float> groundY(cells, NONE), slope(cells, 0.0f);
	std::vector<int> groundTex(cells, -1);
	size_t groundCells = 0;
	for (size_t k = 0; k < cells; ++k) {
		if (topTri[k] < 0) continue;
		float n[3];
		normalOf(scene.triangles[size_t(topTri[k])], n);
		n[1] *= upSign;
		if (n[1] <= 0.2f) continue;
		groundY[k] = topY[k];
		groundTex[k] = triTex[size_t(topTri[k])];
		slope[k] = std::acos(std::clamp(n[1], -1.0f, 1.0f)) * 57.29578f;
		++groundCells;
	}
	const double cellM2 = double(g.cell) * double(g.cell) / 10000.0;
	for (size_t k = 0; k < cells; ++k)
		if (groundTex[k] >= 0) texs[size_t(groundTex[k])].topArea += cellM2;

	// ---- texture weights: average colours of the textures that matter, then the automatic weight or the rules ----
	std::vector<size_t> byTop(texs.size());
	for (size_t i = 0; i < texs.size(); ++i) byTop[i] = i;
	std::sort(byTop.begin(), byTop.end(), [&](size_t a, size_t b) { return texs[a].topArea != texs[b].topArea ? texs[a].topArea > texs[b].topArea : texs[a].area > texs[b].area; });
	size_t coloured = 0;
	if (scene.loadTexture)
		for (size_t r = 0; r < byTop.size() && coloured < 300; ++r) {
			TexStat& t = texs[byTop[r]];
			if (t.texture.empty() || t.topArea <= 0) continue;
			SrcImage img;
			if (!scene.loadTexture(t.texture, img) || img.rgba.empty()) continue;
			double sum[3] = {0, 0, 0}, n = 0;
			for (size_t p = 0; p + 3 < img.rgba.size(); p += 4) {
				if (img.rgba[p + 3] < 128) continue;
				for (int c = 0; c < 3; ++c) sum[c] += img.rgba[p + size_t(c)];
				n += 1;
			}
			if (n <= 0) continue;
			for (int c = 0; c < 3; ++c) t.colour[c] = float(sum[c] / n);
			t.hasColour = true;
			++coloured;
		}
	std::string rulesNote;
	const std::vector<Rule> rules = readRules(opt.rulesFile, rulesNote);
	for (TexStat& t : texs) {
		t.weight = autoWeight(t, t.why);
		if (const Rule* r = ruleFor(rules, t.key)) {
			t.weight = r->weight;
			t.ruled = true;
		}
	}

	// ---- the factors ----
	std::vector<float> texW(cells, 0.0f), slopeF(cells, 1.0f), waterF(cells, 1.0f), roomF(cells, 1.0f), coverF(cells, 1.0f),
	    pathF(cells, 1.0f), plantF(cells, 1.0f), noiseF(cells, 1.0f);
	for (size_t k = 0; k < cells; ++k) {
		if (groundTex[k] < 0) continue;
		texW[k] = texs[size_t(groundTex[k])].weight;
		slopeF[k] = 1.0f - smooth01((slope[k] - 25.0f) / 15.0f);
	}
	// Water: under it nothing; at the very edge half; 1..4 m from it more (the banks are green).
	std::vector<float> waterDist(cells, 1e9f);
	size_t underwater = 0;
	for (size_t k = 0; k < cells; ++k)
		if (waterY[k] > NONE && (groundY[k] == NONE || waterY[k] > groundY[k] - 10.0f)) {
			waterDist[k] = 0.0f;
			if (groundY[k] > NONE) ++underwater;
		}
	{
		// Two passes of a chamfer distance (cells, then cm).
		const float d1 = 1.0f, d2 = 1.4142f;
		for (int z = 0; z < g.h; ++z)
			for (int x = 0; x < g.w; ++x) {
				float& d = waterDist[g.at(x, z)];
				if (x > 0) d = std::min(d, waterDist[g.at(x - 1, z)] + d1);
				if (z > 0) {
					d = std::min(d, waterDist[g.at(x, z - 1)] + d1);
					if (x > 0) d = std::min(d, waterDist[g.at(x - 1, z - 1)] + d2);
					if (x + 1 < g.w) d = std::min(d, waterDist[g.at(x + 1, z - 1)] + d2);
				}
			}
		for (int z = g.h - 1; z >= 0; --z)
			for (int x = g.w - 1; x >= 0; --x) {
				float& d = waterDist[g.at(x, z)];
				if (x + 1 < g.w) d = std::min(d, waterDist[g.at(x + 1, z)] + d1);
				if (z + 1 < g.h) {
					d = std::min(d, waterDist[g.at(x, z + 1)] + d1);
					if (x + 1 < g.w) d = std::min(d, waterDist[g.at(x + 1, z + 1)] + d2);
					if (x > 0) d = std::min(d, waterDist[g.at(x - 1, z + 1)] + d2);
				}
			}
		for (size_t k = 0; k < cells; ++k) {
			const float m = waterDist[k] * g.cell / 100.0f;
			waterF[k] = waterDist[k] == 0.0f ? 0.0f : m < 1.0f ? 0.5f : m < 4.0f ? 1.3f : 1.0f;
		}
	}
	// The world's rooms (BSP sectors: houses, some caves).
	for (const SrcScene::RoomBoxSrc& r : scene.roomBoxes) {
		int ax, az, bx, bz;
		g.cellOf(r.bbMin[0], r.bbMin[2], ax, az);
		g.cellOf(r.bbMax[0], r.bbMax[2], bx, bz);
		for (int z = std::max(0, az); z <= std::min(g.h - 1, bz); ++z)
			for (int x = std::max(0, ax); x <= std::min(g.w - 1, bx); ++x) {
				const size_t k = g.at(x, z);
				if (groundY[k] > r.bbMin[1] - 50.0f && groundY[k] < r.bbMax[1] + 50.0f) roomF[k] = 0.0f;
			}
	}
	// What objects stand on the ground: up to 8 m above it (a roof, a rock, a platform); not plants, not small things,
	// not what one walks through.
	for (size_t i = worldTris; i < scene.triangles.size(); ++i) {
		const SrcTriangle& t = scene.triangles[i];
		const SrcMaterial& m = scene.materials[t.material];
		if (m.noCollision || m.water || m.blend != SrcBlend::Opaque) continue;
		if ((t.detail & 3u) == DETAIL_SMALL) continue;
		if (t.v[0].wind || t.v[1].wind || t.v[2].wind) continue;
		rasterize(g, t, [&](int x, int z, float y) {
			const size_t k = g.at(x, z);
			if (groundY[k] > NONE && y > groundY[k] + 25.0f && y < groundY[k] + 800.0f) coverF[k] = 0.0f;
		});
	}
	// The way net: the paths people walk.
	const auto stampSegment = [&](const float* a, const float* b) {
		const float dx = b[0] - a[0], dz = b[2] - a[2];
		const float len = std::sqrt(dx * dx + dz * dz);
		const int steps = std::max(1, int(len / (g.cell * 0.5f)));
		const int reach = int(std::ceil(200.0f / g.cell));
		for (int s = 0; s <= steps; ++s) {
			const float f = float(s) / float(steps);
			const float px = a[0] + dx * f, py = a[1] + (b[1] - a[1]) * f, pz = a[2] + dz * f;
			int cx, cz;
			if (!g.cellOf(px, pz, cx, cz)) continue;
			for (int z = std::max(0, cz - reach); z <= std::min(g.h - 1, cz + reach); ++z)
				for (int x = std::max(0, cx - reach); x <= std::min(g.w - 1, cx + reach); ++x) {
					const size_t k = g.at(x, z);
					if (groundY[k] == NONE || std::fabs(groundY[k] - py) > 300.0f) continue;
					const float ex = g.cx(x) - px, ez = g.cz(z) - pz;
					const float d = std::sqrt(ex * ex + ez * ez);
					const float v = d < 120.0f ? 0.25f : d < 200.0f ? 0.6f : 1.0f;
					pathF[k] = std::min(pathF[k], v);
				}
		}
	};
	for (const auto& e : scene.wayEdges)
		if (e.first < scene.wayPoints.size() && e.second < scene.wayPoints.size())
			stampSegment(scene.wayPoints[e.first].pos, scene.wayPoints[e.second].pos);
	// Trees: a ring of thicker grass about each (bare at the trunk); bushes a little.
	u32 trees = 0, bushes = 0, grasses = 0;
	for (const SrcPlant& p : scene.plants) {
		if (p.kind == 1) ++trees;
		else if (p.kind == 2) ++bushes;
		else if (p.kind == 3) ++grasses;
		if (p.kind != 1 && p.kind != 2) continue;
		const float reachCm = p.kind == 1 ? 400.0f : 250.0f;
		int cx, cz;
		if (!g.cellOf(p.pos[0], p.pos[2], cx, cz)) continue;
		const int reach = int(std::ceil(reachCm / g.cell));
		for (int z = std::max(0, cz - reach); z <= std::min(g.h - 1, cz + reach); ++z)
			for (int x = std::max(0, cx - reach); x <= std::min(g.w - 1, cx + reach); ++x) {
				const size_t k = g.at(x, z);
				if (groundY[k] == NONE || std::fabs(groundY[k] - p.footY) > 250.0f) continue;
				const float ex = g.cx(x) - p.pos[0], ez = g.cz(z) - p.pos[2];
				const float d = std::sqrt(ex * ex + ez * ez);
				if (d > reachCm) continue;
				float v = p.kind == 2 ? 1.3f : d < 70.0f ? 0.35f : 1.6f;
				if (p.kind == 1 && d < 70.0f) plantF[k] = std::min(plantF[k], v);
				else if (plantF[k] >= 1.0f) plantF[k] = std::max(plantF[k], v);
			}
	}
	for (size_t k = 0; k < cells; ++k) {
		if (groundY[k] == NONE) continue;
		const int x = int(k % size_t(g.w)), z = int(k / size_t(g.w));
		const float n = 0.7f * valueNoise(g.cx(x), g.cz(z), 1200.0f, 11u) + 0.3f * valueNoise(g.cx(x), g.cz(z), 500.0f, 23u);
		noiseF[k] = 0.4f + 0.85f * n;
	}

	// ---- the density, and what each factor took away ----
	std::vector<float> density(cells, 0.0f);
	double sums[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
	for (size_t k = 0; k < cells; ++k) {
		if (groundY[k] == NONE) continue;
		const float f[8] = {texW[k], slopeF[k], waterF[k], roomF[k], coverF[k], pathF[k], plantF[k], noiseF[k]};
		float d = 1.0f;
		for (int s = 0; s < 8; ++s) {
			d *= f[s];
			sums[s] += double(std::min(d, 1.0f));
		}
		density[k] = std::clamp(d, 0.0f, 1.0f);
		sums[8] += density[k];
	}
	size_t growing = 0;
	for (float d : density)
		if (d >= 0.1f) ++growing;
	// The old grass objects: how many stand where the map says grass grows.
	u32 oldOnGrass = 0, oldPlaced = 0;
	for (const SrcPlant& p : scene.plants) {
		if (p.kind != 3) continue;
		int cx, cz;
		if (!g.cellOf(p.pos[0], p.pos[2], cx, cz)) continue;
		++oldPlaced;
		if (density[g.at(cx, cz)] >= 0.25f) ++oldOnGrass;
	}
	// The densest place the camera sees: a square of the area of a 25 m circle (44 m), by a summed-area table.
	double worst = 0;
	{
		std::vector<double> sat((size_t(g.w) + 1) * (size_t(g.h) + 1), 0.0);
		const auto S = [&](int x, int z) -> double& { return sat[size_t(z) * (size_t(g.w) + 1) + size_t(x)]; };
		for (int z = 0; z < g.h; ++z)
			for (int x = 0; x < g.w; ++x) S(x + 1, z + 1) = density[g.at(x, z)] + S(x, z + 1) + S(x + 1, z) - S(x, z);
		const int side = std::max(1, int(std::lround(4431.0f / g.cell)));
		for (int z = 0; z + side <= g.h; ++z)
			for (int x = 0; x + side <= g.w; ++x) worst = std::max(worst, S(x + side, z + side) - S(x, z + side) - S(x + side, z) + S(x, z));
		worst *= cellM2;
	}
	double worstTile = 0;
	{
		const int side = std::max(1, int(std::lround(1600.0f / g.cell)));
		for (int tz = 0; tz < g.h; tz += side)
			for (int tx = 0; tx < g.w; tx += side) {
				double s = 0;
				for (int z = tz; z < std::min(g.h, tz + side); ++z)
					for (int x = tx; x < std::min(g.w, tx + side); ++x) s += density[g.at(x, z)];
				worstTile = std::max(worstTile, s * cellM2);
			}
	}

	// ---- pictures ----
	std::vector<u8> shade(cells, 0);
	for (int z = 0; z < g.h; ++z)
		for (int x = 0; x < g.w; ++x) {
			const size_t k = g.at(x, z);
			if (groundY[k] == NONE) continue;
			const auto y = [&](int a, int b) {
				a = std::clamp(a, 0, g.w - 1);
				b = std::clamp(b, 0, g.h - 1);
				const float v = groundY[g.at(a, b)];
				return v == NONE ? groundY[k] : v;
			};
			const float dx = (y(x + 1, z) - y(x - 1, z)) / (2.0f * g.cell), dz = (y(x, z + 1) - y(x, z - 1)) / (2.0f * g.cell);
			const float n[3] = {-dx, 1.0f, -dz};
			const float len = std::sqrt(n[0] * n[0] + 1.0f + n[2] * n[2]);
			const float l[3] = {-0.5f, 0.7071f, 0.5f};
			const float lit = std::max(0.0f, (n[0] * l[0] + n[1] * l[1] + n[2] * l[2]) / len);
			shade[k] = u8(std::clamp(35.0f + 120.0f * lit, 0.0f, 255.0f));
		}
	// (The map's z grows downwards in the picture flipped: north - the world's +z - at the top.)
	const auto paint = [&](const std::vector<float>& value, bool marks) {
		std::vector<u8> rgb(cells * 3, 0);
		for (int z = 0; z < g.h; ++z)
			for (int x = 0; x < g.w; ++x) {
				const size_t k = g.at(x, z);
				u8* p = &rgb[(size_t(g.h - 1 - z) * size_t(g.w) + size_t(x)) * 3];
				if (waterDist[k] == 0.0f) {
					p[0] = 30, p[1] = 70, p[2] = 160;
					continue;
				}
				if (groundY[k] == NONE) continue;
				float c[3] = {float(shade[k]), float(shade[k]), float(shade[k])};
				if (value[k] > 0.02f) {
					float r[3];
					ramp(value[k], r);
					const float a = 0.45f + 0.55f * std::min(value[k], 1.0f);
					const float s = 0.55f + 0.45f * float(shade[k]) / 155.0f;
					for (int i = 0; i < 3; ++i) c[i] = c[i] * (1.0f - a) + r[i] * s * a;
				}
				for (int i = 0; i < 3; ++i) p[i] = u8(std::clamp(c[i], 0.0f, 255.0f));
			}
		if (marks)
			for (const SrcPlant& pl : scene.plants) {
				int cx, cz;
				if (!g.cellOf(pl.pos[0], pl.pos[2], cx, cz)) continue;
				u8* p = &rgb[(size_t(g.h - 1 - cz) * size_t(g.w) + size_t(cx)) * 3];
				if (pl.kind == 1) p[0] = 0, p[1] = 45, p[2] = 0;
				else if (pl.kind == 3) p[0] = 255, p[1] = 255, p[2] = 255;
			}
		return rgb;
	};
	const std::string base = joinPath(opt.dir, "grass_" + worldName);
	std::vector<float> maskValue(cells, 0.0f);
	for (size_t k = 0; k < cells; ++k) maskValue[k] = texW[k];
	const std::vector<u8> densityPic = paint(density, true), maskPic = paint(maskValue, false);
	const bool wroteA = stbi_write_png((base + ".png").c_str(), g.w, g.h, 3, densityPic.data(), g.w * 3) != 0;
	const bool wroteB = stbi_write_png((base + "_mask.png").c_str(), g.w, g.h, 3, maskPic.data(), g.w * 3) != 0;

	// ---- the text ----
	std::string out;
	out += "Grass report: " + worldName + "\n";
	out += "(g3ds-bake --grass-report; nothing of this is in data.pak. North (+z) is up in the pictures, east (+x) right.)\n\n";
	out += fmt("map: %d x %d cells of %.2f m (%.0f x %.0f m), world corner x %.0f z %.0f (cm)\n", g.w, g.h, double(g.cell) / 100.0,
	           double(g.w) * g.cell / 100.0, double(g.h) * g.cell / 100.0, double(g.x0), double(g.z0));
	out += fmt("world mesh: %zu triangles, %zu textures (%zu with their average colour read); objects: %zu triangles\n", worldTris,
	           texs.size(), coloured, scene.triangles.size() - worldTris);
	out += fmt("open ground seen from above: %.0f m2 (%zu cells); under water %.0f m2\n", double(groundCells) * cellM2, groundCells,
	           double(underwater) * cellM2);
	out += fmt("plants: %u trees, %u bushes, %u grass objects\n", trees, bushes, grasses);
	if (!rulesNote.empty()) out += rulesNote + "\n";
	out += "\nWhat is left, m2 of full-density grass, after each factor in turn:\n";
	static const char* const kSteps[] = {"texture", "x slope", "x water", "x rooms", "x cover", "x paths", "x trees", "x noise"};
	for (int s = 0; s < 8; ++s) out += fmt("  %-8s %10.0f\n", kSteps[s], sums[s] * cellM2);
	out += fmt("  = %.0f m2 of full-density grass; %.0f m2 has some (density >= 0.1)\n", sums[8] * cellM2, double(growing) * cellM2);
	if (oldPlaced)
		out += fmt("old grass objects where the map grows grass (density >= 0.25): %u of %u (%.0f %%): %s\n", oldOnGrass, oldPlaced,
		           100.0 * oldOnGrass / oldPlaced,
		           oldOnGrass * 2 >= oldPlaced ? "the rules agree with the designers" : "LOW: the rules miss where the designers put grass");

	out += "\nWhat it would cost (a tuft: 2 crossed cards, 8 vertices, 4 triangles):\n";
	out += "  tufts/m2 |   tufts | compact list (6 B) | ready meshes (152 B) | densest 25 m round the camera | densest 16 m tile\n";
	for (const float tpm : {1.0f, 2.0f, 4.0f}) {
		const double tufts = sums[8] * cellM2 * tpm;
		out += fmt("  %8.0f | %7.0fk | %15.1f MB | %17.1f MB | %9.0f tufts, %6.0fk triangles | %6.0f tufts\n", double(tpm), tufts / 1000.0,
		           tufts * 6.0 / 1048576.0, tufts * 152.0 / 1048576.0, worst * tpm, worst * tpm * 4.0 / 1000.0, worstTile * tpm);
	}
	out += "  (the camera's 25 m is the radius the grass would be drawn in; its real cost also depends on how much of it is in view)\n";

	out += "\nGround textures, by the open ground they cover (topmost polygon seen from above):\n";
	out += "  weight  src   top m2   share  all m2  flat%  light  colour          group  texture  (why)\n";
	size_t listed = 0;
	for (size_t r = 0; r < byTop.size() && listed < 80; ++r) {
		const TexStat& t = texs[byTop[r]];
		if (t.topArea <= 0 && listed >= 40) break;
		++listed;
		const double share = groundCells ? 100.0 * t.topArea / (double(groundCells) * cellM2) : 0.0;
		const double flat = t.upArea > 0 ? 100.0 * t.flatArea / t.upArea : 0.0;
		const double light = t.lightWeight > 0 ? t.lightSum / t.lightWeight : 0.0;
		const std::string colour = t.hasColour ? fmt("%3.0f %3.0f %3.0f", double(t.colour[0]), double(t.colour[1]), double(t.colour[2])) : std::string("     -     ");
		out += fmt("  %5.2f  %-4s %8.0f %6.1f%% %7.0f %5.0f%% %5.0f  %-14s  %-5s  %s  (%s)\n", double(t.weight), t.ruled ? "rule" : "auto", t.topArea,
		           share, t.area, flat, light, colour.c_str(), groupName(t.group), t.key.c_str(), t.why.c_str());
	}
	out += "\nPictures: " + base + ".png (density: grey relief where nothing grows, green -> yellow -> red denser, blue water, dark dots trees, "
	       "white dots the old grass objects), " + base + "_mask.png (the texture weights alone).\n";
	out += "To change a texture's weight: edit " + base + "_rules.txt and bake again with --grass-rules " + base + "_rules.txt\n";
	{
		std::ofstream f(base + ".txt", std::ios::binary);
		f << out;
	}
	{
		std::string rulesText = "# Grass weights of " + worldName + "'s ground textures, 0 (none) .. 1 (full), for g3ds-bake --grass-rules FILE.\n"
		                        "# One per line: TEXTURE WEIGHT; a name ending in * stands for every texture that begins so (OW_ROCK* 0).\n"
		                        "# The figures after # are the open ground it covers and why the report chose the weight.\n";
		for (size_t r = 0; r < byTop.size(); ++r) {
			const TexStat& t = texs[byTop[r]];
			if (t.topArea <= 0 || t.key.compare(0, 8, "(colour)") == 0) continue;
			rulesText += fmt("%-40s %.2f   # %.0f m2, %s%s\n", t.key.c_str(), double(t.weight), t.topArea, t.ruled ? "rule; auto: " : "", t.why.c_str());
		}
		std::ofstream f(base + "_rules.txt", std::ios::binary);
		f << rulesText;
	}
	return fmt("grass report: %s: %.0f m2 of open ground, %.0f m2 of full-density grass (%.0f %%), densest 25 m: %.0f m2; %s%s.txt", worldName.c_str(),
	           double(groundCells) * cellM2, sums[8] * cellM2, groundCells ? 100.0 * sums[8] / double(groundCells) : 0.0, worst,
	           wroteA && wroteB ? "" : "PICTURES NOT WRITTEN; ", base.c_str());
}

} // namespace g3
