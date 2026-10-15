#include "grass_bake.h"

#include "g3ds/world.h"
#include "ground.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>

namespace g3 {

namespace {

u32 mix(u32 h) {
	h = (h ^ (h >> 16)) * 0x7feb352du;
	h = (h ^ (h >> 15)) * 0x846ca68bu;
	return h ^ (h >> 16);
}
u32 hash3(int a, int b, int c) { return mix(u32(a) * 0x9E3779B1u ^ mix(u32(b) * 0x85EBCA77u ^ mix(u32(c) * 0xC2B2AE3Du + 0x27d4eb2fu))); }
float unit(u32 h) { return float(h & 0xFFFFFFu) * (1.0f / 16777216.0f); }

std::string upper(std::string s) {
	for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
	return s;
}

u16 rgb565(const u8 c[3]) { return u16(((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)); }

// ---- the atlas ----
constexpr int CELL = 64;

struct Painter {
	SrcImage img;
	void put(int x, int y, const float rgb[3]) {
		if (x < 0 || y < 0 || x >= img.width || y >= img.height) return;
		u8* p = &img.rgba[(size_t(y) * size_t(img.width) + size_t(x)) * 4];
		for (int c = 0; c < 3; ++c) p[c] = u8(std::clamp(rgb[c], 0.0f, 255.0f));
		p[3] = 255;
	}
};

// A blade from its foot (x, bottom row) up `height` texels, leaning `bend` texels at the tip, `width` wide at the foot,
// darker at the foot than at the tip. Kept within the cell [x0 + 2, x0 + CELL - 2).
void blade(Painter& p, int cellX0, float x, float height, float bend, float width, const float foot[3], const float tip[3]) {
	const int steps = int(height) + 1;
	for (int s = 0; s < steps; ++s) {
		const float t = float(s) / float(std::max(1, steps - 1));
		const float cx = x + bend * t * t;
		const float half = std::max(0.45f, width * 0.5f * std::pow(1.0f - t, 0.7f));
		const int y = CELL - 1 - s;
		float col[3];
		for (int c = 0; c < 3; ++c) col[c] = foot[c] + (tip[c] - foot[c]) * t;
		for (int px = int(std::floor(cx - half)); px <= int(std::ceil(cx + half)); ++px) {
			if (px < cellX0 + 2 || px >= cellX0 + CELL - 2) continue;
			const float d = std::fabs(float(px) + 0.5f - cx);
			if (d <= half) p.put(px, y, col);
		}
	}
}

void dot(Painter& p, int cellX0, float cx, float cy, float r, const float rgb[3]) {
	for (int y = int(cy - r - 1); y <= int(cy + r + 1); ++y)
		for (int x = int(cx - r - 1); x <= int(cx + r + 1); ++x) {
			if (x < cellX0 + 2 || x >= cellX0 + CELL - 2 || y < 1) continue;
			const float dx = float(x) + 0.5f - cx, dy = float(y) + 0.5f - cy;
			if (dx * dx + dy * dy <= r * r) p.put(x, y, rgb);
		}
}

} // namespace

SrcImage grassAtlasImage() {
	Painter p;
	p.img.width = CELL * int(GRASS_VARIANTS);
	p.img.height = CELL;
	p.img.rgba.assign(size_t(p.img.width) * size_t(p.img.height) * 4, 0);
	// (Transparent texels keep a green, so that the filtered edges are not dark.)
	for (size_t i = 0; i < p.img.rgba.size(); i += 4) {
		p.img.rgba[i] = 70;
		p.img.rgba[i + 1] = 110;
		p.img.rgba[i + 2] = 40;
	}
	for (u32 v = 0; v < GRASS_VARIANTS; ++v) {
		const int x0 = int(v) * CELL;
		// 0 a dense low tuft, 1 tall thin blades, 2 with seed heads, 3 with a few flowers.
		const int blades = v == 0 ? 26 : v == 1 ? 14 : v == 2 ? 16 : 14;
		for (int b = 0; b < blades; ++b) {
			const u32 h = hash3(int(v), b, 7);
			const float fx = float(x0) + 8.0f + unit(h) * float(CELL - 16);
			const float centre = (fx - float(x0) - CELL * 0.5f) / (CELL * 0.5f); // -1 .. 1: the outer blades lean out and are lower
			float height = v == 0 ? 26.0f + 26.0f * unit(h >> 3) : v == 1 ? 40.0f + 22.0f * unit(h >> 3) : 30.0f + 26.0f * unit(h >> 3);
			height *= 1.0f - 0.25f * std::fabs(centre);
			const float bend = centre * (6.0f + 10.0f * unit(h >> 5)) + (unit(h >> 9) - 0.5f) * 8.0f;
			const float width = v == 1 ? 1.6f + 0.8f * unit(h >> 11) : 2.2f + 1.4f * unit(h >> 11);
			const float shadeK = 0.85f + 0.3f * unit(h >> 13);
			const float foot[3] = {58.0f * shadeK, 100.0f * shadeK, 34.0f * shadeK};
			const float tip[3] = {(130.0f + 40.0f * unit(h >> 17)) * shadeK, (178.0f + 30.0f * unit(h >> 19)) * shadeK, 60.0f * shadeK};
			blade(p, x0, fx, height, bend, width, foot, tip);
		}
		if (v == 2) {
			for (int s = 0; s < 4; ++s) {
				const u32 h = hash3(int(v), s, 31);
				const float fx = float(x0) + 14.0f + unit(h) * float(CELL - 28);
				const float height = 46.0f + 14.0f * unit(h >> 4);
				const float bend = (unit(h >> 8) - 0.5f) * 10.0f;
				const float stem[3] = {120.0f, 140.0f, 70.0f};
				blade(p, x0, fx, height, bend, 1.2f, stem, stem);
				const float head[3] = {196.0f, 182.0f, 120.0f};
				for (int k = 0; k < 4; ++k) dot(p, x0, fx + bend - 0.3f * float(k) * (bend > 0 ? 1.0f : -1.0f), float(CELL) - height + float(k) * 2.5f, 1.6f, head);
			}
		}
		if (v == 3) {
			static const float petals[3][3] = {{235, 228, 205}, {240, 200, 60}, {150, 120, 210}};
			for (int s = 0; s < 4; ++s) {
				const u32 h = hash3(int(v), s, 53);
				const float fx = float(x0) + 12.0f + unit(h) * float(CELL - 24);
				const float height = 30.0f + 20.0f * unit(h >> 4);
				const float bend = (unit(h >> 8) - 0.5f) * 8.0f;
				const float stem[3] = {60.0f, 110.0f, 40.0f};
				blade(p, x0, fx, height, bend, 1.3f, stem, stem);
				const float* petal = petals[(h >> 12) % 3];
				const float cx = fx + bend, cy = float(CELL) - height;
				dot(p, x0, cx, cy, 2.8f, petal);
				const float heart[3] = {230.0f, 190.0f, 40.0f};
				dot(p, x0, cx, cy, 1.0f, heart);
			}
		}
	}
	return p.img;
}

std::vector<u8> grassAtlasTexture(const TexBakeOptions& base) {
	TexBakeOptions o = base;
	o.maxSize = CELL * int(GRASS_VARIANTS);
	o.mipmaps = true;
	o.keepCoverage = true;
	return bakeTexture(grassAtlasImage(), o);
}

bool placeGrass(const SrcScene& scene, const GrassOptions& opt, GrassPlacement& out, GrassBakeStats& stats, std::string& why) {
	bool ok = false;
	out.map = buildGrassMap(scene, opt.map, ok, why);
	out.seeds.clear();
	if (!ok) return false;
	const GrassMap& g = out.map;
	stats.areaFull = g.sums[8];
	GroundSampler ground;
	ground.build(scene, g.worldTris, true);

	// The atlas's average colour: a ground texture's colour tints the tuft by how far it is from it.
	float atlasAvg[3] = {0, 0, 0};
	{
		const SrcImage atlas = grassAtlasImage();
		double sum[3] = {0, 0, 0}, n = 0;
		for (size_t i = 0; i + 3 < atlas.rgba.size(); i += 4) {
			if (atlas.rgba[i + 3] < 128) continue;
			for (int c = 0; c < 3; ++c) sum[c] += atlas.rgba[i + size_t(c)];
			n += 1;
		}
		for (int c = 0; c < 3; ++c) atlasAvg[c] = n > 0 ? float(sum[c] / n) : 128.0f;
	}
	std::vector<std::array<float, 3>> tint(g.texs.size(), std::array<float, 3> {1.0f, 1.0f, 1.0f});
	for (size_t t = 0; t < g.texs.size(); ++t) {
		const GrassTexStat& ts = g.texs[t];
		if (!ts.hasColour) continue;
		// The ground's colour against the blades' own, by channel, but only so far: a brown ground makes the grass drier,
		// not brown; and in its brightness the blades stay themselves (the light carries the brightness).
		float ratio[3], lum = 0;
		for (int c = 0; c < 3; ++c) {
			ratio[c] = std::clamp((ts.colour[c] + 8.0f) / (atlasAvg[c] + 8.0f), 0.4f, 2.5f);
			lum += ratio[c] * (c == 0 ? 0.3f : c == 1 ? 0.59f : 0.11f);
		}
		for (int c = 0; c < 3; ++c) tint[t][size_t(c)] = 1.0f + 0.55f * (std::clamp(ratio[c] / std::max(lum, 0.05f), 0.6f, 1.5f) - 1.0f);
	}

	u32 variantCount[4] = {0, 0, 0, 0};
	const float cellM2 = float(g.cellM2());
	for (int cz = 0; cz < g.h; ++cz)
		for (int cx = 0; cx < g.w; ++cx) {
			const size_t k = g.at(cx, cz);
			const float d = g.density[k];
			if (d < 0.04f || g.groundTri[k] < 0) continue;
			const float expected = d * opt.density * cellM2;
			const u32 hc = hash3(cx, cz, 1);
			int count = int(expected);
			if (unit(hc) < expected - float(count)) ++count;
			if (count <= 0) continue;
			float n[3];
			grassTriangleNormal(scene.triangles[size_t(g.groundTri[k])], n);
			for (float& c : n) c *= g.upSign;
			for (int i = 0; i < count; ++i) {
				const u32 h = hash3(cx, cz, 16 + i);
				const u32 h2 = mix(h ^ 0xA511E9B3u);
				GrassSeed s;
				s.pos[0] = g.x0 + (float(cx) + unit(h)) * g.cell;
				s.pos[2] = g.z0 + (float(cz) + unit(h >> 8 ^ h2)) * g.cell;
				u8 rgba[4];
				float y = 0;
				if (!ground.sample(s.pos[0], g.groundY[k] + 120.0f, s.pos[2], rgba, &y) || std::fabs(y - g.groundY[k]) > 120.0f) {
					++stats.noGround;
					continue;
				}
				// Under water after all (the edge of a pond, between the map's cells).
				if (g.waterY[k] > GrassMap::NONE && g.waterY[k] > y - 5.0f) continue;
				s.pos[1] = y;
				// The picture: mostly plain tufts, some tall, a few with seed heads or flowers (more of those where it is denser).
				const float pick = unit(h2);
				const u8 variant = pick < 0.42f ? 0 : pick < 0.78f ? 1 : pick < 0.92f - 0.04f * d ? 2 : 3;
				// The size: a little smaller where it is sparse (the edges of a meadow).
				const float scale = (0.7f + 0.6f * unit(h2 >> 6)) * (0.85f + 0.15f * d);
				const int scaleStep = std::clamp(int(std::lround((scale - 0.7f) / 0.6f * 7.0f)), 0, 7);
				const int turn = int((h2 >> 12) & 7u);
				s.look = u8(variant | (scaleStep << 2) | (turn << 5));
				// It leans with the slope, half of it, at most 15 degrees each way.
				if (n[1] > 0.2f) {
					s.leanX = i8(std::clamp(int(std::lround(0.5f * n[0] / n[1] / 0.0875f)), -3, 3));
					s.leanZ = i8(std::clamp(int(std::lround(0.5f * n[2] / n[1] / 0.0875f)), -3, 3));
				}
				const int tex = g.groundTex[k];
				for (int c = 0; c < 3; ++c) s.rgb[c] = u8(std::clamp(float(rgba[c]) * (tex >= 0 ? tint[size_t(tex)][size_t(c)] : 1.0f), 0.0f, 255.0f));
				s.day = rgba[3];
				s.key = mix(h2 + 0x632BE5ABu);
				++variantCount[variant];
				out.seeds.push_back(s);
			}
		}
	const float total = float(std::max<size_t>(out.seeds.size(), 1));
	for (int v = 0; v < 4; ++v) stats.variantShare[v] = float(variantCount[v]) / total;
	if (out.seeds.empty()) {
		why = "nothing grows (the density map is empty)";
		return false;
	}
	return true;
}

u32 removeOldGrass(SrcScene& scene, const GrassPlacement& placement) {
	const GrassMap& g = placement.map;
	if (g.cells() == 0) return 0;
	std::vector<char> grassMaterial(scene.materials.size(), 0);
	for (size_t m = 0; m < scene.materials.size(); ++m) {
		const SrcMaterial& sm = scene.materials[m];
		grassMaterial[m] = !sm.water && (upper(sm.texture).find("GRAS") != std::string::npos || upper(sm.name).find("GRAS") != std::string::npos);
	}
	u32 removed = 0;
	std::vector<SrcTriangle> kept;
	kept.reserve(scene.triangles.size());
	for (size_t i = 0; i < scene.triangles.size(); ++i) {
		const SrcTriangle& t = scene.triangles[i];
		if (i >= g.worldTris && t.material < grassMaterial.size() && grassMaterial[t.material]) {
			const float x = (t.v[0].pos[0] + t.v[1].pos[0] + t.v[2].pos[0]) / 3.0f;
			const float z = (t.v[0].pos[2] + t.v[1].pos[2] + t.v[2].pos[2]) / 3.0f;
			if (g.densityAt(x, z) >= 0.25f) {
				++removed;
				continue;
			}
		}
		kept.push_back(t);
	}
	if (removed) scene.triangles.swap(kept);
	return removed;
}

void shadeGrass(GrassPlacement& placement, const std::function<bool(float x, float z, float& shade)>& shadeAt, GrassBakeStats& stats) {
	stats.shaded = 0;
	for (GrassSeed& s : placement.seeds) {
		float shade = 0.0f;
		if (!shadeAt(s.pos[0], s.pos[2], shade)) continue;
		s.shade = u8(std::clamp(int(std::lround(shade * 3.0f)), 0, 3));
		stats.shaded += s.shade != 0;
	}
}

void encodeGrass(const GrassPlacement& placement, const GrassOptions& opt, u32 dataEntry, u32 textureEntry, std::vector<u8>& index,
                 std::vector<u8>& data, GrassBakeStats& stats) {
	const float tile = opt.tileSize;
	const float originX = std::floor(placement.map.x0 / tile) * tile, originZ = std::floor(placement.map.z0 / tile) * tile;
	// The tufts by tile, (z, x) in order.
	std::map<std::pair<int, int>, std::vector<const GrassSeed*>> tiles;
	for (const GrassSeed& s : placement.seeds) {
		const int tx = int(std::floor((s.pos[0] - originX) / tile)), tz = int(std::floor((s.pos[2] - originZ) / tile));
		tiles[{tz, tx}].push_back(&s);
	}
	std::vector<GrassTile> table;
	std::vector<GrassTuft> list;
	// (A tile's mesh holds 65535 corners at most: 8 to a tuft.)
	const size_t cap = CHUNK_MAX_VERTICES / 8;
	for (auto& kv : tiles) {
		std::vector<const GrassSeed*>& v = kv.second;
		std::sort(v.begin(), v.end(), [](const GrassSeed* a, const GrassSeed* b) { return a->key < b->key; });
		if (v.size() > cap) {
			stats.capped += u32(v.size() - cap);
			v.resize(cap);
		}
		GrassTile t {};
		t.x = i16(kv.first.second);
		t.z = i16(kv.first.first);
		t.count = u16(v.size());
		t.first = u32(list.size());
		t.yMin = 1e30f;
		t.yMax = -1e30f;
		u32 day = 0;
		for (const GrassSeed* s : v) {
			t.yMin = std::min(t.yMin, s->pos[1]);
			t.yMax = std::max(t.yMax, s->pos[1]);
			day += s->day;
		}
		// (Heights in whole cm from the lowest, in 16 bits: a tile cannot span more than 655 m of height.)
		t.yMin = std::floor(t.yMin);
		t.yMax = std::min(t.yMax, t.yMin + 65535.0f);
		t.day = u8(std::clamp<u32>((day + u32(v.size()) / 2) / u32(std::max<size_t>(v.size(), 1)), 0, 255));
		const float x0 = originX + float(t.x) * tile, z0 = originZ + float(t.z) * tile;
		const float step = tile / 256.0f;
		for (const GrassSeed* s : v) {
			GrassTuft g {};
			g.x = u8(std::clamp(int(std::floor((s->pos[0] - x0) / step)), 0, 255));
			g.z = u8(std::clamp(int(std::floor((s->pos[2] - z0) / step)), 0, 255));
			g.y = u16(std::clamp(int(std::lround(s->pos[1] - t.yMin)), 0, 65535));
			g.look = s->look;
			g.lean = u8((u8(s->leanX) & 7u) | ((u8(s->leanZ) & 7u) << 3) | ((s->shade & 3u) << 6));
			g.colour = rgb565(s->rgb);
			list.push_back(g);
		}
		stats.maxTile = std::max(stats.maxTile, u32(v.size()));
		table.push_back(t);
	}
	GrassHeader h {};
	h.magic = GRASS_MAGIC;
	h.version = GRASS_VERSION;
	h.tileCount = u32(table.size());
	h.tuftCount = u32(list.size());
	h.tileSize = tile;
	h.originX = originX;
	h.originZ = originZ;
	h.density = opt.density;
	h.height = opt.height;
	h.width = opt.width;
	h.dataEntry = dataEntry;
	h.textureEntry = textureEntry;
	index.resize(sizeof(h) + table.size() * sizeof(GrassTile));
	std::memcpy(index.data(), &h, sizeof(h));
	if (!table.empty()) std::memcpy(index.data() + sizeof(h), table.data(), table.size() * sizeof(GrassTile));
	data.resize(list.size() * sizeof(GrassTuft));
	if (!list.empty()) std::memcpy(data.data(), list.data(), data.size());
	stats.tiles = h.tileCount;
	stats.tufts = h.tuftCount;
	stats.bytes = index.size() + data.size();
}

} // namespace g3
