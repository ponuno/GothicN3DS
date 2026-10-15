#include "grass_map.h"

#include "g3ds/world.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

namespace g3 {

namespace {

// ZenGin's material groups (zenkit::MaterialGroup).
enum : u16 { GROUP_UNDEF = 0, GROUP_METAL = 1, GROUP_STONE = 2, GROUP_WOOD = 3, GROUP_EARTH = 4, GROUP_WATER = 5, GROUP_SNOW = 6 };

const char* groupWord(u16 g) {
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
float autoWeight(const GrassTexStat& t, std::string& why) {
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
		why += std::string(", ") + groupWord(t.group);
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

// Calls f(cellX, cellZ, y) for every cell whose middle the triangle covers seen from above (at least the cell of its middle).
template <class F>
void rasterize(const GrassMap& g, const SrcTriangle& t, F&& f) {
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

float areaOf(const SrcTriangle& t) {
	const float* a = t.v[0].pos;
	const float* b = t.v[1].pos;
	const float* c = t.v[2].pos;
	const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
	const float x = e1[1] * e2[2] - e1[2] * e2[1], y = e1[2] * e2[0] - e1[0] * e2[2], z = e1[0] * e2[1] - e1[1] * e2[0];
	return 0.5f * std::sqrt(x * x + y * y + z * z);
}

} // namespace

void grassTriangleNormal(const SrcTriangle& t, float n[3]) {
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

bool GrassMap::cellOf(float x, float z, int& outX, int& outZ) const {
	outX = int(std::floor((x - x0) / cell));
	outZ = int(std::floor((z - z0) / cell));
	return outX >= 0 && outZ >= 0 && outX < w && outZ < h;
}

GrassMap buildGrassMap(const SrcScene& scene, const GrassMapOptions& opt, bool& ok, std::string& why) {
	GrassMap g;
	ok = false;
	if (scene.indoor) return why = "an indoor world (no sky)", g;
	const size_t worldTris = scene.worldTriangles ? std::min(scene.worldTriangles, scene.triangles.size()) : scene.triangles.size();
	if (worldTris == 0) return why = "no world mesh", g;
	g.worldTris = worldTris;
	const float NONE = GrassMap::NONE;

	// Which way "up" winds: the ground is most of the area that faces up or down; whichever way it faces is up (the importer
	// keeps the source's winding, and the synthetic scene's is the other one).
	{
		double sum = 0;
		for (size_t i = 0; i < worldTris; ++i) {
			float n[3];
			grassTriangleNormal(scene.triangles[i], n);
			sum += double(n[1]) * double(areaOf(scene.triangles[i]));
		}
		if (sum < 0) g.upSign = -1.0f;
	}
	// ---- the world mesh's textures ----
	std::map<std::string, size_t> texIndex;
	std::vector<GrassTexStat>& texs = g.texs;
	g.triTex.assign(worldTris, -1);
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
		g.triTex[i] = int(it->second);
		GrassTexStat& ts = texs[it->second];
		float n[3];
		grassTriangleNormal(t, n);
		n[1] *= g.upSign;
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

	// ---- the map: the topmost polygon over each cell that faces up and can be stood on ----
	const float extent = std::max(hi[0] - lo[0], hi[2] - lo[2]);
	g.cell = std::max(opt.cellCm, extent / 2048.0f);
	g.x0 = lo[0];
	g.z0 = lo[2];
	g.w = std::max(1, int(std::ceil((hi[0] - lo[0]) / g.cell)));
	g.h = std::max(1, int(std::ceil((hi[2] - lo[2]) / g.cell)));
	const size_t cells = g.cells();
	g.groundY.assign(cells, NONE);
	g.waterY.assign(cells, NONE);
	g.groundTri.assign(cells, -1);
	// What can be ground: solid (not walked through: leaves, foliage cards), opaque (not light shafts, glow), not water.
	const auto groundMaterial = [&](const SrcMaterial& m) { return !m.water && !m.noCollision && m.blend == SrcBlend::Opaque; };
	std::vector<u8> facesUp(worldTris, 0);
	for (size_t i = 0; i < worldTris; ++i) {
		const SrcTriangle& t = scene.triangles[i];
		const SrcMaterial& m = scene.materials[t.material];
		if (m.water) {
			rasterize(g, t, [&](int x, int z, float y) {
				float& w = g.waterY[g.at(x, z)];
				w = std::max(w, y);
			});
			continue;
		}
		if (!groundMaterial(m)) continue;
		float n[3];
		grassTriangleNormal(t, n);
		if (n[1] * g.upSign <= 0.2f) continue; // a wall, an overhang's underside (see the cover below)
		facesUp[i] = 1;
		rasterize(g, t, [&](int x, int z, float y) {
			const size_t k = g.at(x, z);
			if (y > g.groundY[k]) {
				g.groundY[k] = y;
				g.groundTri[k] = int(i);
			}
		});
	}
	g.groundTex.assign(cells, -1);
	g.slope.assign(cells, 0.0f);
	for (size_t k = 0; k < cells; ++k) {
		if (g.groundTri[k] < 0) continue;
		float n[3];
		grassTriangleNormal(scene.triangles[size_t(g.groundTri[k])], n);
		n[1] *= g.upSign;
		g.groundTex[k] = g.triTex[size_t(g.groundTri[k])];
		g.slope[k] = std::acos(std::clamp(n[1], -1.0f, 1.0f)) * 57.29578f;
		++g.groundCells;
	}
	const double cellM2 = g.cellM2();
	for (size_t k = 0; k < cells; ++k)
		if (g.groundTex[k] >= 0) texs[size_t(g.groundTex[k])].topArea += cellM2;

	// ---- texture weights: average colours of the textures that matter, then the automatic weight or the rules ----
	std::vector<size_t> byTop(texs.size());
	for (size_t i = 0; i < texs.size(); ++i) byTop[i] = i;
	std::sort(byTop.begin(), byTop.end(), [&](size_t a, size_t b) { return texs[a].topArea != texs[b].topArea ? texs[a].topArea > texs[b].topArea : texs[a].area > texs[b].area; });
	if (scene.loadTexture)
		for (size_t r = 0; r < byTop.size() && g.coloured < 300; ++r) {
			GrassTexStat& t = texs[byTop[r]];
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
			++g.coloured;
		}
	const std::vector<Rule> rules = readRules(opt.rulesFile, g.rulesNote);
	for (GrassTexStat& t : texs) {
		t.weight = autoWeight(t, t.why);
		if (const Rule* r = ruleFor(rules, t.key)) {
			t.weight = r->weight;
			t.ruled = true;
		}
	}

	// ---- the factors ----
	g.texW.assign(cells, 0.0f);
	g.slopeF.assign(cells, 1.0f);
	g.waterF.assign(cells, 1.0f);
	g.roomF.assign(cells, 1.0f);
	g.coverF.assign(cells, 1.0f);
	g.pathF.assign(cells, 1.0f);
	g.plantF.assign(cells, 1.0f);
	g.noiseF.assign(cells, 1.0f);
	for (size_t k = 0; k < cells; ++k) {
		if (g.groundTex[k] < 0) continue;
		g.texW[k] = texs[size_t(g.groundTex[k])].weight;
		g.slopeF[k] = 1.0f - smooth01((g.slope[k] - 25.0f) / 15.0f);
	}
	// Water: under it nothing; at the very edge half; 1..4 m from it more (the banks are green).
	g.waterDist.assign(cells, 1e9f);
	std::vector<float>& waterDist = g.waterDist;
	for (size_t k = 0; k < cells; ++k)
		if (g.waterY[k] > NONE && (g.groundY[k] == NONE || g.waterY[k] > g.groundY[k] - 10.0f)) {
			waterDist[k] = 0.0f;
			if (g.groundY[k] > NONE) ++g.underwater;
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
			g.waterF[k] = waterDist[k] == 0.0f ? 0.0f : m < 1.0f ? 0.5f : m < 4.0f ? 1.3f : 1.0f;
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
				if (g.groundY[k] > r.bbMin[1] - 50.0f && g.groundY[k] < r.bbMax[1] + 50.0f) g.roomF[k] = 0.0f;
			}
	}
	// What stands on the ground: up to 8 m above it (a roof, a rock, a platform); not plants, not small things, not what
	// one walks through. The world mesh's own overhangs (a polygon of it facing down over the ground) likewise.
	const auto coverAt = [&](int x, int z, float y) {
		const size_t k = g.at(x, z);
		if (g.groundY[k] > NONE && y > g.groundY[k] + 25.0f && y < g.groundY[k] + 800.0f) g.coverF[k] = 0.0f;
	};
	for (size_t i = 0; i < worldTris; ++i) {
		if (facesUp[i] || !groundMaterial(scene.materials[scene.triangles[i].material])) continue;
		rasterize(g, scene.triangles[i], coverAt);
	}
	for (size_t i = worldTris; i < scene.triangles.size(); ++i) {
		const SrcTriangle& t = scene.triangles[i];
		const SrcMaterial& m = scene.materials[t.material];
		if (m.noCollision || m.water || m.blend != SrcBlend::Opaque) continue;
		if ((t.detail & 3u) == DETAIL_SMALL) continue;
		if (t.v[0].wind || t.v[1].wind || t.v[2].wind) continue;
		rasterize(g, t, coverAt);
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
					if (g.groundY[k] == NONE || std::fabs(g.groundY[k] - py) > 300.0f) continue;
					const float ex = g.cx(x) - px, ez = g.cz(z) - pz;
					const float d = std::sqrt(ex * ex + ez * ez);
					const float v = d < 120.0f ? 0.25f : d < 200.0f ? 0.6f : 1.0f;
					g.pathF[k] = std::min(g.pathF[k], v);
				}
		}
	};
	for (const auto& e : scene.wayEdges)
		if (e.first < scene.wayPoints.size() && e.second < scene.wayPoints.size())
			stampSegment(scene.wayPoints[e.first].pos, scene.wayPoints[e.second].pos);
	// Trees: a ring of thicker grass about each (bare at the trunk); bushes a little.
	for (const SrcPlant& p : scene.plants) {
		if (p.kind == 1) ++g.trees;
		else if (p.kind == 2) ++g.bushes;
		else if (p.kind == 3) ++g.grasses;
		if (p.kind != 1 && p.kind != 2) continue;
		const float reachCm = p.kind == 1 ? 400.0f : 250.0f;
		int cx, cz;
		if (!g.cellOf(p.pos[0], p.pos[2], cx, cz)) continue;
		const int reach = int(std::ceil(reachCm / g.cell));
		for (int z = std::max(0, cz - reach); z <= std::min(g.h - 1, cz + reach); ++z)
			for (int x = std::max(0, cx - reach); x <= std::min(g.w - 1, cx + reach); ++x) {
				const size_t k = g.at(x, z);
				if (g.groundY[k] == NONE || std::fabs(g.groundY[k] - p.footY) > 250.0f) continue;
				const float ex = g.cx(x) - p.pos[0], ez = g.cz(z) - p.pos[2];
				const float d = std::sqrt(ex * ex + ez * ez);
				if (d > reachCm) continue;
				float v = p.kind == 2 ? 1.3f : d < 70.0f ? 0.35f : 1.6f;
				if (p.kind == 1 && d < 70.0f) g.plantF[k] = std::min(g.plantF[k], v);
				else if (g.plantF[k] >= 1.0f) g.plantF[k] = std::max(g.plantF[k], v);
			}
	}
	for (size_t k = 0; k < cells; ++k) {
		if (g.groundY[k] == NONE) continue;
		const int x = int(k % size_t(g.w)), z = int(k / size_t(g.w));
		const float n = 0.7f * valueNoise(g.cx(x), g.cz(z), 1200.0f, 11u) + 0.3f * valueNoise(g.cx(x), g.cz(z), 500.0f, 23u);
		g.noiseF[k] = 0.4f + 0.85f * n;
	}

	// ---- the density, and what each factor took away ----
	g.density.assign(cells, 0.0f);
	for (size_t k = 0; k < cells; ++k) {
		if (g.groundY[k] == NONE) continue;
		const float f[8] = {g.texW[k], g.slopeF[k], g.waterF[k], g.roomF[k], g.coverF[k], g.pathF[k], g.plantF[k], g.noiseF[k]};
		float d = 1.0f;
		for (int s = 0; s < 8; ++s) {
			d *= f[s];
			g.sums[s] += double(std::min(d, 1.0f));
		}
		g.density[k] = std::clamp(d, 0.0f, 1.0f);
		g.sums[8] += g.density[k];
	}
	for (int s = 0; s < 9; ++s) g.sums[s] *= cellM2;
	ok = true;
	return g;
}

} // namespace g3
