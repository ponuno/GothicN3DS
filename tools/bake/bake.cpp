#include "bake.h"
#include "sound_bake.h"

#include "ground.h"
#include "model_bake.h"
#include "populate.h"
#include "vcache.h"
#include "water_fx.h"
#include "water_texture.h"
#include "ground_fx.h"

#include "g3ds/fog.h"
#include "g3ds/game.h"

#include "g3ds/tex.h"
#include "g3ds/world.h"

#include <algorithm>
#include <functional>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace g3 {


namespace {

const float kEyeHeight = 175.0f; // cm above a named arrival point
const size_t kMaxFrames = 32;    // animation frames per texture
const float kLadderReach = LADDER_BOX_REACH; // cm around a ladder from which it can be climbed (g3ds/world.h)
const float kLadderTop = LADDER_BOX_TOP;     // cm above its top the climb continues, to get over the edge

std::string lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
	return s;
}

std::string upper(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::toupper(c)); });
	return s;
}

std::string baseName(std::string s) {
	const size_t slash = s.find_last_of("/\\");
	if (slash != std::string::npos) s = s.substr(slash + 1);
	const size_t dot = s.rfind('.');
	if (dot != std::string::npos) s.resize(dot);
	return s;
}


// ZenGin animates a texture whose name contains "_A0" by counting that number
// up. Returns the name of frame `k`, or an empty string if it is not animated.
std::string frameName(const std::string& texture, size_t k) {
	const std::string up = upper(texture);
	std::string out;
	bool animated = false;
	for (size_t i = 0; i < texture.size(); ++i) {
		if (up.compare(i, 3, "_A0") == 0) {
			out += "_A" + std::to_string(k);
			i += 2;
			animated = true;
		} else {
			out += texture[i];
		}
	}
	return animated ? out : std::string();
}

SrcImage missingTexture() {
	SrcImage img;
	img.width = img.height = 16;
	img.rgba.resize(16 * 16 * 4);
	for (int y = 0; y < 16; ++y)
		for (int x = 0; x < 16; ++x) {
			const bool on = ((x / 4) + (y / 4)) & 1;
			u8* p = &img.rgba[size_t(y * 16 + x) * 4];
			p[0] = on ? 255 : 30;
			p[1] = 0;
			p[2] = on ? 255 : 30;
			p[3] = 255;
		}
	return img;
}

struct TexJob {
	std::string pakName;
	std::string source; // texture name as the scene knows it
	size_t world = 0;   // whose loader to ask
	std::vector<u8> data;
	TexBakeInfo info;
	bool ok = false;
	u8 average[3] = {128, 128, 128}; // the texture's colour from afar (the far layer)
	float coverage = 1.0f;            // share of its texels that are solid (alpha >= 128)
};

// What a texture looks like from afar: the mean of its texels (of the solid
// ones, for a cut-out), averaged as numbers - as the mip chain the chunks use
// farther away averages them - so that the far layer is no lighter than a
// chunk seen from the same place.
void averageColour(const SrcImage& img, u8 out[3], float& coverage) {
	double sum[3] = {0, 0, 0}, all[3] = {0, 0, 0};
	u64 n = 0, nAll = 0;
	const size_t texels = size_t(img.width) * size_t(img.height);
	for (size_t i = 0; i < texels && i * 4 + 3 < img.rgba.size(); ++i) {
		const u8* t = &img.rgba[i * 4];
		for (int a = 0; a < 3; ++a) all[a] += double(t[a]);
		++nAll;
		if (t[3] < 128) continue;
		for (int a = 0; a < 3; ++a) sum[a] += double(t[a]);
		++n;
	}
	coverage = nAll ? float(double(n) / double(nAll)) : 1.0f;
	const double* s = n ? sum : all;
	const double d = double(n ? n : nAll);
	for (int a = 0; a < 3; ++a) out[a] = d > 0 ? u8(std::clamp(std::lround(s[a] / d), 0l, 255l)) : 128;
}

std::string format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string format(const char* fmt, ...) {
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

namespace {
// What --probe lists: a triangle, where it is (metres), how long its longest edge is, what it is made of and the baked
// light of its corners (r g b, a = daylight, o = occlusion).
struct ProbeTri {
	float mid[3] = {0, 0, 0}, edge = 0;
	int corner[3][5] = {};
	std::string material;
};

// Of the triangles near `at`, the darkest ones with an edge of 2 m or more first (a dark flat patch is one of those).
void probePrint(const std::vector<ProbeTri>& all, const float at[3], float radius, const char* stage, const std::function<void(const std::string&)>& say) {
	struct Hit { int bright; float edge; std::string text; };
	std::vector<Hit> hits;
	size_t inside = 0, dark = 0;
	for (const ProbeTri& t : all) {
		const float dx = t.mid[0] - at[0], dy = t.mid[1] - at[1], dz = t.mid[2] - at[2];
		const float reach = radius + t.edge * 0.3f;
		if (dx * dx + dy * dy + dz * dz > reach * reach) continue;
		++inside;
		// (The darkest corner: a patch that is black at one corner and grey at the others is seen as a dark wedge.)
		int bright = 255;
		for (const auto& c : t.corner) bright = std::min(bright, std::max({c[0], c[1], c[2]}));
		if (t.edge < 2.0f) continue;
		dark += bright < 25;
		char buf[400];
		int n = std::snprintf(buf, sizeof(buf), "  [%.0f %.0f %.0f] edge %.1f m, %s:", double(t.mid[0]), double(t.mid[1]), double(t.mid[2]), double(t.edge),
		                      t.material.c_str());
		for (int k = 0; k < 3 && n > 0 && n < int(sizeof(buf)) - 40; ++k)
			n += std::snprintf(buf + n, sizeof(buf) - size_t(n), " (%d %d %d a%d o%d)", t.corner[k][0], t.corner[k][1], t.corner[k][2], t.corner[k][3], t.corner[k][4]);
		hits.push_back({bright, t.edge, buf});
	}
	std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.bright != b.bright ? a.bright < b.bright : a.edge > b.edge; });
	say(format("probe (%s): %zu triangles within %.0f m of %.0f %.0f %.0f, %zu with an edge of 2 m or more, %zu of them dark (a corner under 25); the 30 with the darkest corner (corners: r g b, a = daylight, o = occlusion):",
	           stage, inside, double(radius), double(at[0]), double(at[1]), double(at[2]), hits.size(), dark));
	std::string last;
	size_t shown = 0;
	for (const Hit& h : hits) {
		if (h.text == last) continue; // (a two-sided triangle twice)
		last = h.text;
		say(h.text);
		if (++shown >= 30) break;
	}
}

void probeScene(const SrcScene& scene, const float at[3], float radius, const char* stage, const std::function<void(const std::string&)>& say) {
	std::vector<ProbeTri> all;
	for (const SrcTriangle& t : scene.triangles) {
		ProbeTri p;
		for (int a = 0; a < 3; ++a) p.mid[a] = (t.v[0].pos[a] + t.v[1].pos[a] + t.v[2].pos[a]) / 3.0f * 0.01f;
		const float dx = p.mid[0] - at[0], dy = p.mid[1] - at[1], dz = p.mid[2] - at[2];
		if (dx * dx + dy * dy + dz * dz > (radius + 60.0f) * (radius + 60.0f)) continue; // (far off, whatever the size)
		for (int k = 0; k < 3; ++k) {
			float d2 = 0;
			for (int a = 0; a < 3; ++a) {
				const float d = t.v[(k + 1) % 3].pos[a] - t.v[k].pos[a];
				d2 += d * d;
			}
			p.edge = std::max(p.edge, std::sqrt(d2) * 0.01f);
			p.corner[k][0] = t.v[k].rgba[0]; p.corner[k][1] = t.v[k].rgba[1]; p.corner[k][2] = t.v[k].rgba[2];
			p.corner[k][3] = t.v[k].rgba[3]; p.corner[k][4] = t.v[k].occlusion;
		}
		p.material = t.material < scene.materials.size() ? scene.materials[t.material].name : std::string("?");
		all.push_back(p);
	}
	probePrint(all, at, radius, stage, say);
}

// The chunks as they will be written (after the seams, the simplifying): what the game draws near the camera.
void probeChunks(const std::vector<BakedChunk>& chunks, const SrcScene& scene, const std::vector<int>& remap, const float at[3], float radius,
                 const char* stage, const std::function<void(const std::string&)>& say) {
	std::vector<ProbeTri> all;
	for (const BakedChunk& c : chunks) {
		// (The chunk's box, to skip the far ones.)
		bool near = true;
		for (int a = 0; a < 3; ++a)
			near = near && at[a] * 100.0f > c.bbMin[a] - (radius + 60.0f) * 100.0f && at[a] * 100.0f < c.bbMax[a] + (radius + 60.0f) * 100.0f;
		if (!near) continue;
		for (const ChunkBatch& b : c.batches) {
			std::string name = "?";
			for (size_t m = 0; m < remap.size(); ++m)
				if (remap[m] == int(b.material) && m < scene.materials.size()) {
					name = scene.materials[m].name;
					break;
				}
			for (u32 i = 0; i + 2 < b.indexCount; i += 3) {
				ProbeTri p;
				float pos[3][3];
				for (int k = 0; k < 3; ++k) {
					const WorldVertex& v = c.vertices[c.indices[b.firstIndex + i + u32(k)]];
					pos[k][0] = (c.header.origin[0] + float(v.x) * c.header.posScale) * 0.01f;
					pos[k][1] = (c.header.origin[1] + float(v.y) * c.header.posScale) * 0.01f;
					pos[k][2] = (c.header.origin[2] + float(v.z) * c.header.posScale) * 0.01f;
					p.corner[k][0] = v.r; p.corner[k][1] = v.g; p.corner[k][2] = v.b; p.corner[k][3] = v.a;
					p.corner[k][4] = v.w & 0xFF;
				}
				for (int a = 0; a < 3; ++a) p.mid[a] = (pos[0][a] + pos[1][a] + pos[2][a]) / 3.0f;
				for (int k = 0; k < 3; ++k) {
					float d2 = 0;
					for (int a = 0; a < 3; ++a) {
						const float d = pos[(k + 1) % 3][a] - pos[k][a];
						d2 += d * d;
					}
					p.edge = std::max(p.edge, std::sqrt(d2));
				}
				p.material = name;
				all.push_back(p);
			}
		}
	}
	probePrint(all, at, radius, stage, say);
}
} // namespace

template <class T> void append(std::vector<u8>& out, const std::vector<T>& items) {
	const u8* p = reinterpret_cast<const u8*>(items.data());
	out.insert(out.end(), p, p + items.size() * sizeof(T));
}

} // namespace

std::string worldNameFromLevel(const std::string& level) { return lower(baseName(level)); }

// "Foo_Bar.TGA" -> "tex/foo_bar"
std::string texturePakName(const std::string& texture) { return "tex/" + lower(baseName(texture)); }

BakeResult bakeWorlds(std::vector<BakeWorld>& worlds, const BakeOptions& opt, PakWriter& pak) {
	return bakeWorlds(worlds, std::vector<SrcModel>(), opt, pak);
}

BakeResult bakeWorlds(std::vector<BakeWorld>& worlds, const std::vector<SrcModel>& models, const BakeOptions& opt,
                      PakWriter& pak) {
	return bakeWorlds(worlds, models, BakeScripts(), opt, pak);
}

void waterTint(u8 brightness, float rgb[3], bool swamp) {
	const float k = float(brightness) / 255.0f;
	rgb[0] = k * (swamp ? 0.95f : 0.82f);
	rgb[1] = k * (swamp ? 1.0f : 0.95f);
	rgb[2] = k * (swamp ? 0.72f : 1.0f);
}

bool isSwampWaterTexture(const std::string& name) {
	std::string low = name;
	for (char& c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
	return low.find("swamp") != std::string::npos;
}

size_t applyWaterBiome(SrcScene& scene, float strength) {
	if (strength <= 0.0f || scene.fogZones.empty()) return 0;
	size_t n = 0;
	for (SrcTriangle& t : scene.triangles) {
		if (t.material >= scene.materials.size() || !scene.materials[t.material].water) continue;
		float mid[3] = {0, 0, 0};
		for (const SrcVertex& v : t.v)
			for (int a = 0; a < 3; ++a) mid[a] += v.pos[a] / 3.0f;
		// The last zone that holds it wins (as the music zones do).
		const SrcFogZone* zone = nullptr;
		float w = 0.0f;
		for (const SrcFogZone& z : scene.fogZones) {
			FogZone fz {};
			std::copy_n(z.bbMin, 3, fz.bbMin);
			std::copy_n(z.bbMax, 3, fz.bbMax);
			fz.inner = z.inner;
			const float k = fogZoneWeight(fz, mid[0], mid[1], mid[2]);
			if (k > 0.0f) zone = &z, w = k;
		}
		if (!zone) continue;
		const float top = float(std::max({zone->color[0], zone->color[1], zone->color[2], u8(1)}));
		const float k = std::clamp(strength, 0.0f, 1.0f) * w;
		for (SrcVertex& v : t.v)
			for (int c = 0; c < 3; ++c) {
				const float hue = float(zone->color[c]) / top; // 1 at the strongest channel
				v.rgba[c] = u8(std::clamp(float(v.rgba[c]) * (1.0f + (hue - 1.0f) * k) + 0.5f, 0.0f, 255.0f));
			}
		++n;
	}
	return n;
}

size_t applyWaterLight(SrcScene& scene, u8 brightness) {
	if (brightness == 0) return 0;
	float f[3], fs[3];
	waterTint(brightness, f);
	waterTint(brightness, fs, true);
	const u8 tint[3] = {u8(f[0] * 255.0f + 0.5f), u8(f[1] * 255.0f + 0.5f), u8(f[2] * 255.0f + 0.5f)};
	const u8 swampTint[3] = {u8(fs[0] * 255.0f + 0.5f), u8(fs[1] * 255.0f + 0.5f), u8(fs[2] * 255.0f + 0.5f)};
	size_t n = 0;
	for (SrcTriangle& t : scene.triangles) {
		if (t.material >= scene.materials.size() || !scene.materials[t.material].water) continue;
		const bool named = isSwampWaterTexture(scene.materials[t.material].texture);
		// The swamp camp's water has the ordinary water's texture (owodwat): it is the swamp by where it lies - inside the world's
		// one fog zone (the swamp's) - and gets the murky green by the vertex colour, which the texture is multiplied by. By
		// VERTEX, not by the triangle's middle: the colour then runs across a triangle from one water to the other instead of
		// changing at its edge (the zone's own weight falls off smoothly towards its border).
		static const float kMurk[3] = {0.62f, 0.80f, 0.40f}; // (red and blue held back: less of the sky's blue)
		for (SrcVertex& v : t.v) {
			float murk = 0.0f;
			for (const SrcFogZone& z : scene.fogZones) {
				FogZone fz {};
				std::copy_n(z.bbMin, 3, fz.bbMin);
				std::copy_n(z.bbMax, 3, fz.bbMax);
				fz.inner = z.inner;
				murk = std::max(murk, fogZoneWeight(fz, v.pos[0], v.pos[1], v.pos[2]));
			}
			murk = std::min(murk, 1.0f);
			for (int c = 0; c < 3; ++c) {
				const float plain = float(tint[c]), swampy = float(swampTint[c]);
				const float base = named ? swampy : plain + (swampy - plain) * murk;
				v.rgba[c] = u8(std::clamp(base * (named ? 1.0f : 1.0f + (kMurk[c] - 1.0f) * murk) + 0.5f, 0.0f, 255.0f));
			}
		}
		++n;
	}
	return n;
}


BakeResult bakeWorlds(std::vector<BakeWorld>& worlds, const std::vector<SrcModel>& models, const BakeScripts& scripts,
                      const BakeOptions& opt, PakWriter& pak) {
	if (worlds.empty()) throw std::runtime_error("nothing to bake");
	// Named places are where somebody stands: on the ground under them. (The
	// game's way points float about a metre above it.)
	std::vector<std::string> pointNotes(worlds.size());
	for (size_t w = 0; w < worlds.size(); ++w) {
		SrcScene& scene = worlds[w].scene;
		if (scene.points.empty() && scene.wayPoints.empty() && scene.freePoints.empty()) continue;
		GroundSampler ground;
		ground.build(scene, scene.triangles.size(), true);
		size_t settled = 0, floating = 0;
		double drop = 0;
		for (auto& [name, point] : scene.points) {
			u8 light[4];
			float h = 0;
			if (ground.sample(point.pos[0], point.pos[1] + 60.0f, point.pos[2], light, &h) && h > point.pos[1] - 500.0f) {
				drop += double(point.pos[1] - h);
				point.pos[1] = h;
				++settled;
			} else {
				++floating;
			}
		}
		// The way net's points and the free points are the same kind of place.
		for (std::vector<SrcWayPoint>* list : {&scene.wayPoints, &scene.freePoints})
			for (SrcWayPoint& point : *list) {
				u8 light[4];
				float h = 0;
				if (ground.sample(point.pos[0], point.pos[1] + 60.0f, point.pos[2], light, &h) && h > point.pos[1] - 500.0f)
					point.pos[1] = h;
			}
		if (!scene.points.empty())
			pointNotes[w] = format("places: %zu named (way points and markers), %zu set on the ground (%.0f cm down on average)%s",
		                       scene.points.size(), settled, settled ? std::max(drop / double(settled), 0.0) : 0.0,
		                       floating ? format(", %zu with no ground within 5 m below left where they are", floating).c_str() : "");
	}
	BakeResult result;
	// Where the time goes (one line in the report): a lap is the time since the last one.
	std::string lapText;
	auto lapStart = std::chrono::steady_clock::now();
	const auto lap = [&](const char* what) {
		const auto now = std::chrono::steady_clock::now();
		lapText += format("%s%s %.1f s", lapText.empty() ? "" : ", ", what, std::chrono::duration<double>(now - lapStart).count());
		lapStart = now;
	};
	auto line = [&](const std::string& s) {
		result.report += s + "\n";
		if (!opt.quiet) std::puts(s.c_str());
	};

	// ---- water that looks like water (fix140): river flow and shore foam, added to the scene before anything is counted ----
	if (opt.waterFoam > 0.0f || opt.riverFlow > 0.0f || opt.waterGlint > 0.0f) {
		for (size_t w = 0; w < worlds.size(); ++w) {
			SrcScene& scene = worlds[w].scene;
			if (scene.indoor) continue;
			WaterFxOptions fo;
			fo.foam = opt.waterFoam;
			fo.foamMinWidth = opt.foamMinWidth;
			fo.flow = opt.riverFlow;
			fo.glint = opt.waterGlint;
			WaterFxStats fs;
			bakeWaterFx(scene, fo, fs);
			if (fs.glintTriangles) line(format("water glints (%s): %llu triangles", worlds[w].name.c_str(), static_cast<unsigned long long>(fs.glintTriangles)));
			if (fs.foamTriangles || fs.flowMaterials)
				line(format("water fx (%s): %u shore tiles; foam: %u shore lines (%u narrow ones left bare), %.1f km, %llu triangles; rivers: %u tiles, %u flow materials, "
				            "%llu triangles (tiles by direction %u %u %u %u %u %u %u %u)",
				            worlds[w].name.c_str(), fs.shoreTiles, fs.shoreChains, fs.foamChainsNarrow, double(fs.shoreLength) / 100000.0,
				            static_cast<unsigned long long>(fs.foamTriangles), fs.riverTiles, fs.flowMaterials,
				            static_cast<unsigned long long>(fs.flowTriangles), fs.flowTiles[0], fs.flowTiles[1], fs.flowTiles[2],
				            fs.flowTiles[3], fs.flowTiles[4], fs.flowTiles[5], fs.flowTiles[6], fs.flowTiles[7]));
		}
	}

	// ---- which materials and textures are referenced, by any world ----
	std::vector<std::vector<char>> used(worlds.size());
	std::vector<std::vector<u32>> materialTriangles(worlds.size());
	std::vector<TexJob> jobs;
	std::map<std::string, size_t> jobIndex;                 // pak name -> job
	std::map<std::string, std::vector<std::string>> frames; // base pak name -> pak names of all frames
	std::set<std::string> waterNames; // pak names of the textures water uses
	std::set<std::string> landNames;  // ... and of those a material that is not water uses (those keep the game's picture)
	auto addJob = [&](const std::string& source, size_t world) {
		const std::string name = texturePakName(source);
		if (jobIndex.emplace(name, jobs.size()).second) {
			TexJob j;
			j.pakName = name;
			j.source = source;
			j.world = world;
			jobs.push_back(std::move(j));
		}
		return name;
	};
	// The ground that is painted with a frame of the water's own animation (the swamp camp's pools: land materials that use
	// OWODWAT_A0): it gets a copy of that picture under a name of its own, so that the water's frames can be made anew without
	// repainting it (before, a shared frame was kept the game's alone, and the water flickered between old and new).
	if (opt.waterTexture) {
		const auto stem = [](const std::string& tex) {
			std::string s = lower(baseName(tex));
			const size_t dot = s.rfind('.');
			if (dot != std::string::npos) s.erase(dot);
			if (s.size() > 3 && s[s.size() - 3] == '_' && s[s.size() - 2] == 'a' && std::isdigit(static_cast<unsigned char>(s.back()))) s.erase(s.size() - 3);
			return s;
		};
		std::set<std::string> waterStems;
		for (const BakeWorld& bw : worlds)
			for (const SrcMaterial& m : bw.scene.materials)
				if (m.water && !m.texture.empty()) waterStems.insert(stem(m.texture));
		size_t renamed = 0;
		for (BakeWorld& bw : worlds) {
			std::map<std::string, std::string> back; // new logical name -> the game's
			for (SrcMaterial& m : bw.scene.materials) {
				if (m.water || m.texture.empty() || !waterStems.count(stem(m.texture))) continue;
				const std::string orig = m.texture;
				const size_t dot = orig.rfind('.');
				const std::string copy = dot == std::string::npos ? orig + "_G3LAND" : orig.substr(0, dot) + "_G3LAND" + orig.substr(dot);
				back[copy] = orig;
				m.texture = copy;
				++renamed;
			}
			if (!back.empty()) {
				auto inner = bw.scene.loadTexture;
				bw.scene.loadTexture = [inner, back](const std::string& name, SrcImage& out) {
					const auto it = back.find(name);
					return inner && inner(it == back.end() ? name : it->second, out);
				};
			}
		}
		if (renamed) line(format("ground that shares the water's pictures: %zu materials got a copy of their own", renamed));
	}
	for (size_t w = 0; w < worlds.size(); ++w) {
		SrcScene& scene = worlds[w].scene;
		used[w].assign(scene.materials.size(), 0);
		materialTriangles[w].assign(scene.materials.size(), 0);
		for (const SrcTriangle& t : scene.triangles) {
			if (t.material >= scene.materials.size()) throw std::runtime_error("triangle references an unknown material");
			used[w][t.material] = 1;
			++materialTriangles[w][t.material];
		}
		for (size_t i = 0; i < scene.materials.size(); ++i) {
			const std::string& tex = scene.materials[i].texture;
			if (!used[w][i] || tex.empty()) continue;
			const std::string base = addJob(tex, w);
			(scene.materials[i].water ? waterNames : landNames).insert(base);
			if (frames.count(base)) continue;
			std::vector<std::string>& list = frames[base];
			list.push_back(base);
			// Probe for further animation frames; loading is cheap next to encoding.
			for (size_t k = 1; k < kMaxFrames && scene.loadTexture; ++k) {
				const std::string next = frameName(tex, k);
				SrcImage probe;
				if (next.empty() || !(makeFoamFrame(next, probe) || (scene.loadTexture && scene.loadTexture(next, probe)))) break;
				list.push_back(addJob(next, w));
				(scene.materials[i].water ? waterNames : landNames).insert(list.back());
			}
		}
	}

	// A texture that water and something else both use (a swamp's mud and its water, say) stays the game's: the made one would
	// repaint the ground too. What is replaced and what is kept is told, by name.
	if (opt.waterTexture) {
		std::vector<std::string> shared, replaced;
		const auto kept = [&](const std::string& name) {
			std::string low = name;
			for (char& c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
			for (const std::string& k : opt.waterKeep)
				if (low.rfind(k, 0) == 0 || low.rfind("tex/" + k, 0) == 0) return true;
			return false;
		};
		for (auto it = waterNames.begin(); it != waterNames.end();) {
			// (A frame of an animation is never kept alone: the water would flicker between the game's frame and the made ones.)
			const std::string& nm = *it;
			const bool frame = nm.size() > 3 && nm[nm.size() - 3] == '_' && nm[nm.size() - 2] == 'a' && std::isdigit(static_cast<unsigned char>(nm.back()));
			if ((landNames.count(nm) && !frame) || kept(nm)) {
				shared.push_back(*it);
				it = waterNames.erase(it);
			} else {
				replaced.push_back(*it);
				++it;
			}
		}
		const auto names = [](const std::vector<std::string>& v) {
			std::string out;
			for (size_t i = 0; i < v.size() && i < 24; ++i) out += (i ? ", " : "") + v[i];
			return out + (v.size() > 24 ? ", ..." : "");
		};
		line(format("water textures: %zu made here (%s); %zu kept the game's because land uses them too (%s)", replaced.size(), names(replaced).c_str(),
		            shared.size(), names(shared).c_str()));
	}
	lap("sources");
	// ---- textures (ETC1 encoding dominates the bake time, so fan it out) ----
	{
		std::atomic<size_t> next {0};
		TexBakeOptions serialTex = opt.tex; // (the images already run side by side)
		serialTex.threads = 1;
		std::mutex loadLock;
		std::string firstError;
		auto worker = [&]() {
			for (;;) {
				const size_t i = next.fetch_add(1);
				if (i >= jobs.size()) return;
				try {
					SrcImage img;
					bool found;
					if (opt.waterTexture && waterNames.count(jobs[i].pakName)) {
						const bool swamp = isSwampWaterTexture(jobs[i].pakName);
						img = makeWaterTexture(swamp ? opt.swampColor : opt.waterColor, 128, swamp ? opt.swampAlpha : opt.waterAlpha);
						found = true;
					} else if (makeFoamFrame(jobs[i].source, img)) {
						found = true;
					} else if (jobs[i].pakName == texturePakName(kGlintTexture)) {
						img = makeGlintTexture();
						found = true;
					} else {
						std::lock_guard<std::mutex> g(loadLock);
						const SrcScene& scene = worlds[jobs[i].world].scene;
						found = scene.loadTexture && scene.loadTexture(jobs[i].source, img);
					}
					if (!found) continue;
					jobs[i].data = bakeTexture(img, serialTex, &jobs[i].info);
					averageColour(img, jobs[i].average, jobs[i].coverage);
					jobs[i].ok = true;
				} catch (const std::exception& ex) {
					std::lock_guard<std::mutex> g(loadLock);
					if (firstError.empty()) firstError = ex.what();
				}
			}
		};
		std::vector<std::thread> pool;
		for (unsigned i = 0; i < std::min<size_t>(std::max(1u, opt.threads), std::max<size_t>(jobs.size(), 1)); ++i)
			pool.emplace_back(worker);
		for (std::thread& t : pool) t.join();
		if (!firstError.empty()) throw std::runtime_error("texture bake failed: " + firstError);
	}

	lap("world textures");
	std::vector<std::string> missing;
	u64 texels = 0, srcTexels = 0;
	u32 alphaTextures = 0;
	for (const TexJob& j : jobs) {
		if (!j.ok) {
			missing.push_back(j.source);
			continue;
		}
		pak.add(j.pakName, ASSET_TEXTURE, j.data);
		++result.textures;
		texels += u64(j.info.width) * j.info.height;
		srcTexels += u64(j.info.srcWidth) * j.info.srcHeight;
		// (The generated water texture carries alpha for blending; it is no cut-out.)
		alphaTextures += j.info.alpha && !(opt.waterTexture && waterNames.count(j.pakName));
	}
	const std::string fallbackName = "tex/_missing";
	pak.add(fallbackName, ASSET_TEXTURE, bakeTexture(missingTexture(), opt.tex));
	// A material without a usable texture is drawn in its own colour, as the
	// game does: one small flat texture per colour.
	std::set<std::string> flatTextures;
	auto flatTexture = [&](const u8 rgb[3]) {
		const std::string name = format("tex/_flat/%02x%02x%02x", unsigned(rgb[0]), unsigned(rgb[1]), unsigned(rgb[2]));
		if (flatTextures.insert(name).second) {
			SrcImage img;
			img.width = img.height = 8;
			img.rgba.resize(8 * 8 * 4);
			for (size_t i = 0; i < img.rgba.size(); i += 4) {
				img.rgba[i] = rgb[0];
				img.rgba[i + 1] = rgb[1];
				img.rgba[i + 2] = rgb[2];
				img.rgba[i + 3] = 255;
			}
			pak.add(name, ASSET_TEXTURE, bakeTexture(img, opt.tex));
		}
		return name;
	};

	std::map<std::string, size_t> worldByLevel;
	for (size_t w = 0; w < worlds.size(); ++w) {
		if (worlds[w].name.empty()) throw std::runtime_error("a world has no name");
		for (size_t o = 0; o < w; ++o)
			if (worlds[o].name == worlds[w].name) throw std::runtime_error("two worlds are both named '" + worlds[w].name + "'");
		worldByLevel.emplace(upper(baseName(worlds[w].level.empty() ? worlds[w].name : worlds[w].level)), w);
	}

	const double MB = 1024.0 * 1024.0;
	u64 chunkRaw = 0, chunkPacked = 0;

	for (size_t w = 0; w < worlds.size(); ++w) {
		BakeWorld& bw = worlds[w];
		const SrcScene& scene = bw.scene;
		if (worlds.size() > 1) line("== world '" + bw.name + "'" + (bw.level.empty() ? "" : " (" + bw.level + ")") + " ==");
		for (const std::string& n : scene.notes) line(n);

		// ---- texture table and world materials ----
		std::vector<u32> textureTable;
		std::map<std::string, u16> textureFirst; // base pak name -> first table index
		auto tableIndex = [&](const std::string& base, u8& frameCount, bool& alpha) -> int {
			auto fr = frames.find(base);
			if (fr == frames.end() || !jobs[jobIndex[base]].ok) return -1;
			// Frames that failed to bake end the animation early.
			size_t n = 0;
			while (n < fr->second.size() && jobs[jobIndex[fr->second[n]]].ok) ++n;
			frameCount = u8(n);
			alpha = jobs[jobIndex[base]].info.alpha;
			auto it = textureFirst.find(base);
			if (it != textureFirst.end()) return it->second;
			const size_t first = textureTable.size();
			for (size_t k = 0; k < n; ++k) textureTable.push_back(hashName(fr->second[k].c_str()));
			textureFirst.emplace(base, u16(first));
			return int(first);
		};

		std::vector<WorldMaterial> worldMaterials;
		std::vector<u8> materialOrder;
		using MaterialKey = std::tuple<u16, u16, u8, u16, i16, i16>;
		std::map<MaterialKey, int> materialIndex;
		std::vector<int> remap(scene.materials.size(), -1);
		int fallbackIndex = -1;
		std::map<std::string, int> flatIndex; // flat texture name -> table index
		std::vector<std::string> untextured;  // for the report
		u32 averagesStored = 0;               // materials with the texture's mean colour (drawn until the texture is there)
		u32 blended = 0, animated = 0, scrolling = 0, water = 0;
		// "Graphics+": which materials sway, and what each does for the occlusion.
		std::vector<bool> windy(scene.materials.size(), false);
		std::vector<char> plantMaterial(scene.materials.size(), 0); // what the importer marked as plants (kept even without --wind)
		u64 swayingCorners = 0;
		for (SrcTriangle& t : bw.scene.triangles)
			for (SrcVertex& v : t.v) {
				if (v.wind && t.material < plantMaterial.size()) plantMaterial[t.material] = 1;
				if (!opt.wind) v.wind = 0;
				if (!v.wind || t.material >= windy.size()) continue;
				windy[t.material] = true;
				++swayingCorners;
			}
		std::vector<AoMaterial> aoMaterials(scene.materials.size(), AoMaterial {0.0f, false});
		u32 windyMaterials = 0;
		// The far layer: which materials go into it, in what colour.
		std::vector<bool> farInclude(scene.materials.size(), false);
		std::vector<std::array<u8, 3>> farColor(scene.materials.size(), std::array<u8, 3> {128, 128, 128});
		std::vector<float> farShrink(scene.materials.size(), 1.0f);
		for (size_t i = 0; i < scene.materials.size(); ++i) {
			if (!used[w][i]) continue;
			const SrcMaterial& sm = scene.materials[i];
			WorldMaterial wm {};
			wm.group = u8(std::min<u16>(sm.group, 255));
			wm.frameCount = 1;
			wm.opacity = 255;
			bool alpha = false;
			const int tex = sm.texture.empty() ? -1 : tableIndex(texturePakName(sm.texture), wm.frameCount, alpha);
			if (tex >= 0) {
				wm.texture = u16(tex);
			} else if (sm.hasColor) {
				const std::string flat = flatTexture(sm.color);
				auto fi = flatIndex.find(flat);
				if (fi == flatIndex.end()) {
					fi = flatIndex.emplace(flat, int(textureTable.size())).first;
					textureTable.push_back(hashName(flat.c_str()));
				}
				wm.texture = u16(fi->second);
				wm.flags |= MATF_NO_TEXTURE;
				untextured.push_back(format("'%s' (%s, %u triangles, colour %u %u %u)", sm.name.c_str(),
				                            sm.texture.empty() ? "no texture" : ("texture " + sm.texture + " not found").c_str(),
				                            materialTriangles[w][i], unsigned(sm.color[0]), unsigned(sm.color[1]),
				                            unsigned(sm.color[2])));
			} else {
				if (fallbackIndex < 0) {
					fallbackIndex = int(textureTable.size());
					textureTable.push_back(hashName(fallbackName.c_str()));
				}
				wm.texture = u16(fallbackIndex);
				wm.flags |= MATF_NO_TEXTURE;
			}
			if (sm.water) {
				wm.flags |= MATF_WATER | MATF_BLEND | MATF_NO_COLLISION;
			} else if (sm.blend == SrcBlend::Alpha) {
				wm.flags |= MATF_BLEND;
			} else if (sm.blend == SrcBlend::Add) {
				wm.flags |= MATF_ADD;
			} else if (alpha) {
				wm.flags |= MATF_ALPHA_TEST;
			}
			if (wm.flags & MATF_TRANSLUCENT) wm.opacity = sm.opacity;
			if (sm.noCollision) wm.flags |= MATF_NO_COLLISION;
			if (windy[i]) wm.flags |= MATF_WIND;
			// Glow and glass are not seen from afar; water is, as a plain surface.
			farInclude[i] = !(wm.flags & MATF_ADD) && (!(wm.flags & MATF_BLEND) || (wm.flags & MATF_WATER));
			if (tex >= 0) {
				const TexJob& tj = jobs[jobIndex[texturePakName(sm.texture)]];
				farColor[i] = {tj.average[0], tj.average[1], tj.average[2]};
				wm.average = u16(((tj.average[0] >> 3) << 11) | ((tj.average[1] >> 2) << 5) | (tj.average[2] >> 3));
				++averagesStored;
				// A cut-out drawn solid: smaller by its holes (area goes with the square).
				if (wm.flags & MATF_ALPHA_TEST) farShrink[i] = std::clamp(std::sqrt(tj.coverage), 0.35f, 1.0f);
			} else if (sm.hasColor) {
				farColor[i] = {sm.color[0], sm.color[1], sm.color[2]};
			}
			// (The far layer's water is the colour the near water has: the texture's average in the water's own light.)
			if (sm.water && opt.waterLight > 0) {
				float tint[3];
				waterTint(opt.waterLight, tint, isSwampWaterTexture(sm.texture));
				for (int c = 0; c < 3; ++c) farColor[i][size_t(c)] = u8(std::clamp(float(farColor[i][size_t(c)]) * tint[c], 0.0f, 255.0f) + 0.5f);
			}
			// Glass, water and glow let the sky through and are not shaded; a
			// cut-out (leaves, fences) stops about half of what passes it.
			aoMaterials[i] = (wm.flags & MATF_TRANSLUCENT) ? AoMaterial {0.0f, false}
			                 : (wm.flags & MATF_ALPHA_TEST)  ? AoMaterial {0.5f, true}
			                                                 : AoMaterial {1.0f, true};
			if (wm.frameCount > 1) {
				const float ms = sm.animFps > 0 ? 1000.0f / sm.animFps : 1000.0f / float(wm.frameCount);
				wm.frameMs = u16(std::clamp(ms, 16.0f, 60000.0f));
			}
			for (int a = 0; a < 2; ++a)
				wm.scroll[a] = i16(std::clamp(std::lround(sm.scroll[a] / WORLD_SCROLL_SCALE), -32767l, 32767l));

			const MaterialKey key {wm.texture, wm.flags, wm.opacity, wm.frameMs, wm.scroll[0], wm.scroll[1]};
			auto ins = materialIndex.emplace(key, int(worldMaterials.size()));
			if (ins.second) {
				worldMaterials.push_back(wm);
				materialOrder.push_back((wm.flags & MATF_TRANSLUCENT) ? 2 : ((wm.flags & MATF_ALPHA_TEST) ? 1 : 0));
				blended += (wm.flags & MATF_TRANSLUCENT) != 0;
				water += (wm.flags & MATF_WATER) != 0;
				animated += wm.frameCount > 1;
				scrolling += wm.scroll[0] != 0 || wm.scroll[1] != 0;
				windyMaterials += (wm.flags & MATF_WIND) != 0;
			}
			remap[i] = ins.first->second;
		}
		if (worldMaterials.size() > 65535 || textureTable.size() > 65535) throw std::runtime_error("too many materials");

		// ---- winding sanity check ----
		// Worlds are mostly floor: with the right winding, far more front-facing
		// area looks up than down. Front faces are counter-clockwise, which in this
		// left-handed system means the front normal is -cross(b - a, c - a).
		double areaUp = 0, areaDown = 0;
		for (const SrcTriangle& t : scene.triangles) {
			const float ax = t.v[1].pos[0] - t.v[0].pos[0], az = t.v[1].pos[2] - t.v[0].pos[2];
			const float bx = t.v[2].pos[0] - t.v[0].pos[0], bz = t.v[2].pos[2] - t.v[0].pos[2];
			const double ny = -(double(az) * bx - double(ax) * bz); // y of the front normal, scaled by area
			(ny > 0 ? areaUp : areaDown) += std::fabs(ny);
		}
		const double upShare = areaUp + areaDown > 0 ? areaUp / (areaUp + areaDown) : 1.0;

		// ---- geometry ----
		// Under ground nothing follows the sun, lightmapped or not.
		if (scene.indoor)
			for (SrcTriangle& t : bw.scene.triangles)
				for (SrcVertex& v : t.v) v.rgba[3] = 0;
		applyWaterLight(bw.scene, opt.waterLight);
		if (const size_t tinted = applyWaterBiome(bw.scene, opt.waterBiome))
			line(format("water biome: %zu water triangles tinted by the fog zones (%zu zones in '%s')", tinted, bw.scene.fogZones.size(), bw.name.c_str()));
		// Colour of the ground: wet at the water's edge, in the shade of plants (before the lamps and the occlusion use it).
		{
			GroundFxStats groundFx;
			if (!scene.indoor) {
				bakeWetShore(bw.scene, opt.wetShore, groundFx);
				bakeCanopy(bw.scene, plantMaterial, opt.canopy, opt.canopyReach, opt.threads, groundFx);
			}
			if (groundFx.wetCorners || groundFx.shadedCorners)
				line(format("ground colour: %llu corners by the water darkened, %llu under plants (%llu rays)",
				            static_cast<unsigned long long>(groundFx.wetCorners), static_cast<unsigned long long>(groundFx.shadedCorners),
				            static_cast<unsigned long long>(groundFx.rays)));
		}
		if (opt.probe) probeScene(bw.scene, opt.probeAt, opt.probeRadius, "as imported", line);
		AoStats ao;
		if (opt.ao.level != AO_OFF) bakeOcclusion(bw.scene, aoMaterials, opt.ao, ao);
		LightStats lightStats;
		if (!opt.light.enabled && opt.lampExtra > 0) {
			std::vector<SrcLight> dynamic;
			for (const SrcLight& l : bw.scene.lights)
				if (!l.isStatic) dynamic.push_back(l);
			const u32 added = cutNearLights(bw.scene, aoMaterials, dynamic, opt.lampCell, opt.lampExtra);
			if (!dynamic.empty())
				line(format("lamps: %u triangles added by cutting the large ones near the %zu lamps to %.1f m (--lamp-cell, --lamp-extra %u)",
				            added, dynamic.size(), double(opt.lampCell) / 100.0, opt.lampExtra));
		}
		{
			LightOptions lo = opt.light;
			lo.threads = opt.threads;
			bakeLights(bw.scene, aoMaterials, lo, lightStats);
		}
		if (opt.probe) probeScene(bw.scene, opt.probeAt, opt.probeRadius, "after occlusion and lamps", line);
		// ---- the new grass (--grass): sown now, with the ground's light final; the old grass objects it replaces
		// (--grass-replace) go before the chunks are cut; packed after the sun's shadow, which it takes ----
		GrassPlacement grass;
		GrassBakeStats grassStats;
		bool grassOk = false;
		std::string grassWhy;
		if (opt.grass.enabled && !scene.indoor) {
			grassOk = placeGrass(bw.scene, opt.grass, grass, grassStats, grassWhy);
			if (grassOk && opt.grass.replaceOld) grassStats.oldRemoved = removeOldGrass(bw.scene, grass);
		}
		std::vector<SunMask> grassSunMasks;
		const bool hasWind = opt.wind && windyMaterials > 0;
		ChunkerOptions chunkerOpt = opt.chunker;
		// The water cut finely along the shores (in the chunks only: the far layer, visibility and the rest see the
		// water as it is); the waves' weight rides in the vertices' w.
		const bool waterGrid = opt.waterGrid > 0.0f && water > 0;
		if (waterGrid) {
			chunkerOpt.water.cell = opt.waterGrid;
			chunkerOpt.water.riverWidth = opt.riverWidth;
			chunkerOpt.water.shore = opt.waterShore;
			chunkerOpt.water.reachTiles = std::max(1, int(std::lround(opt.waterReach / (opt.waterGrid * float(chunkerOpt.water.cellsPerTile)))));
			chunkerOpt.water.reflect = scene.indoor ? 0.0f : opt.waterReflect;
			chunkerOpt.water.reflectReach = opt.waterReflectReach;
			chunkerOpt.water.threads = opt.threads;
		}
		chunkerOpt.vertexFx = opt.ao.level != AO_OFF || hasWind || waterGrid;
		ChunkerStats cs;
		std::vector<BakedChunk> chunks = buildChunks(scene, remap, materialOrder, chunkerOpt, cs);
		if (chunks.empty()) throw std::runtime_error("world '" + bw.name + "' has no drawable triangles");
		if (waterGrid) {
			if (cs.water.fineTiles > 0)
				line(format("water grid (--water-grid): cells of %.1f m along the shores: %u shore tiles, %u tiles cut fine (%.0f x %.0f m), "
				            "%llu water triangles -> %llu",
				            double(opt.waterGrid) / 100.0, cs.water.shoreTiles, cs.water.fineTiles,
				            double(opt.waterGrid) * chunkerOpt.water.cellsPerTile / 100.0, double(opt.waterGrid) * chunkerOpt.water.cellsPerTile / 100.0,
				            static_cast<unsigned long long>(cs.water.waterIn), static_cast<unsigned long long>(cs.water.waterOut)));
			else
				line("water grid (--water-grid): no shore found - the water is left flat");
			if (cs.water.reflectCorners > 0)
				line(format("water reflections (--water-reflect %.2f, %.0f m): %llu corners (%llu under the ground), %llu rays, %llu water vertices tinted",
				            double(chunkerOpt.water.reflect), double(opt.waterReflectReach) / 100.0,
				            static_cast<unsigned long long>(cs.water.reflectCorners), static_cast<unsigned long long>(cs.water.reflectBuried),
				            static_cast<unsigned long long>(cs.water.reflectRays), static_cast<unsigned long long>(cs.water.reflectVertices)));
		}

		// ---- portals ----
		std::vector<WorldPortal> portals;
		std::vector<std::string> portalNotes;
		for (const SrcPortal& sp : scene.portals) {
			auto target = worldByLevel.find(upper(baseName(sp.level)));
			if (target == worldByLevel.end()) {
				portalNotes.push_back(sp.level + " (not baked)");
				continue;
			}
			const BakeWorld& tw = worlds[target->second];
			WorldPortal p {};
			std::memcpy(p.bbMin, sp.bbMin, sizeof(p.bbMin));
			std::memcpy(p.bbMax, sp.bbMax, sizeof(p.bbMax));
			p.targetWorld = hashName(("world/" + tw.name).c_str());
			auto point = tw.scene.points.find(upper(sp.point));
			if (point != tw.scene.points.end()) {
				p.targetPos[0] = point->second.pos[0];
				p.targetPos[1] = point->second.pos[1] + kEyeHeight;
				p.targetPos[2] = point->second.pos[2];
				p.targetYaw = point->second.yaw;
				portalNotes.push_back(tw.name + " at " + sp.point);
			} else {
				p.flags |= PORTALF_USE_SPAWN;
				portalNotes.push_back(tw.name + " at its start point ('" + sp.point + "' not found)");
			}
			portals.push_back(p);
		}

		// ---- ladders: the climbing box is the ladder plus room to stand in
		// front of it and to step off above its top ----
		std::vector<WorldLadder> ladders;
		std::vector<LadderInfo> ladderInfos;
		size_t ladderInfoCount = 0, ladderSideCount = 0;
		for (const SrcLadder& sl : scene.ladders) {
			WorldLadder l {};
			for (int a = 0; a < 3; ++a) {
				const float grow = a == 1 ? 0.0f : kLadderReach;
				l.bbMin[a] = sl.bbMin[a] - grow;
				l.bbMax[a] = sl.bbMax[a] + (a == 1 ? kLadderTop : grow);
			}
			ladders.push_back(l);
			// What its model says of it (an entry with halfWidth 0 says nothing: the runtime climbs the box).
			LadderInfo li {};
			if (sl.hasInfo) {
				for (int a = 0; a < 3; ++a) li.foot[a] = sl.foot[a], li.head[a] = sl.head[a];
				li.normal[0] = sl.normal[0];
				li.normal[1] = sl.normal[1];
				li.halfWidth = sl.halfWidth;
				li.halfDepth = sl.halfDepth;
				li.side = sl.side;
				++ladderInfoCount;
				ladderSideCount += sl.side != 0;
			}
			ladderInfos.push_back(li);
		}
		// fix148: the lamps for the runtime (torches and fires light the people and the world round them, and flicker).
		{
			const std::vector<WorldLamp> lamps = lampTable(bw.scene.lights, opt.light.enabled);
			if (!lamps.empty()) {
				const LampsHeader lh {LAMPS_MAGIC, LAMPS_VERSION, u32(lamps.size()), 0};
				std::vector<u8> blob(lampsBlobSize(lh.count));
				std::memcpy(blob.data(), &lh, sizeof(lh));
				std::memcpy(blob.data() + sizeof(lh), lamps.data(), lamps.size() * sizeof(WorldLamp));
				pak.add("world/" + bw.name + "/lamps", ASSET_META, blob);
			}
			u32 fires = 0, made = 0, keyed = 0;
			for (const WorldLamp& lamp : lamps) {
				fires += (lamp.flags & LAMPF_FIRE) != 0;
				made += (lamp.flags & LAMPF_MADE) != 0;
				keyed += lamp.rangeKeys > 1 || lamp.colorKeys > 1;
			}
			line(format("lamps for the runtime: %zu (%u of fire places, %u made for fires without one, %u flicker by keys)%s", lamps.size(),
			            fires, made, keyed, lamps.empty() ? "" : opt.light.enabled ? "; their light is baked too (--lights): the runtime adds the flicker" : "; not baked (no --lights): the runtime lights the world round them"));
		}
		if (ladderInfoCount) {
			LadderInfoHeader lh {};
			lh.magic = LADDERINFO_MAGIC;
			lh.version = LADDERINFO_VERSION;
			lh.count = u32(ladderInfos.size());
			std::vector<u8> lblob(sizeof(lh));
			std::memcpy(lblob.data(), &lh, sizeof(lh));
			append(lblob, ladderInfos);
			pak.add("world/" + bw.name + "/ladderinfo", ASSET_META, lblob);
		}

		WorldHeader wh {};
		wh.magic = WORLD_MAGIC;
		wh.version = WORLD_VERSION;
		wh.chunkSize = opt.chunker.chunkSize;
		wh.chunkCount = u32(chunks.size());
		wh.materialCount = u32(worldMaterials.size());
		wh.textureCount = u32(textureTable.size());
		wh.portalCount = u32(portals.size());
		wh.ladderCount = u32(ladders.size());
		if (scene.indoor) wh.flags |= WORLDF_INDOOR;
		if (opt.ao.level != AO_OFF) wh.flags |= WORLDF_AO;
		if (hasWind) wh.flags |= WORLDF_WIND;
		if (waterGrid && cs.water.fineTiles > 0) wh.flags |= WORLDF_WATER_GRID;
		if (chunkerOpt.splitDetail) wh.flags |= WORLDF_TIERS;
		// ---- the far layer (G10) ----
		if (opt.far.enabled && !scene.indoor) {
			FarStats fs;
			const std::vector<u8> far = buildFarLayer(scene, farInclude, farColor, farShrink, chunks, opt.chunker.chunkSize, opt.far, fs);
			if (!far.empty()) {
				const std::string farName = "world/" + bw.name + "/far";
				pak.add(farName, ASSET_META, far);
				wh.farEntry = hashName(farName.c_str());
				line(format("far layer: %u triangles (and %u in strips along %u seam edges; %u vertices) of %llu, %u crowns, grid %.1f-%.1f m "
				            "(seams %.1f; %u tries), %u tiles, %u ranges, %.2f MB",
				            fs.triangles - fs.skirtTriangles, fs.skirtTriangles, fs.seamEdges, fs.vertices, static_cast<unsigned long long>(fs.inputTriangles), fs.crowns, double(fs.cellFinest) / 100.0, double(fs.cellCoarsest) / 100.0, double(fs.cell) / 100.0,
				            fs.passes, fs.tiles, fs.ranges, double(fs.bytes) / MB));
			} else {
				line("far layer: nothing to put in it");
			}
		}
		// ---- visibility ----
		if (opt.vis.enabled) {
			VisStats vs;
			const std::vector<u8> vis = buildVisibility(scene, aoMaterials, chunks, opt.chunker.chunkSize, opt.vis, vs);
			if (!vis.empty()) {
				const std::string visName = "world/" + bw.name + "/vis";
				pak.add(visName, ASSET_META, vis);
				wh.visEntry = hashName(visName.c_str());
				line(format("visibility (--vis%s): %u rows in %u cells (%u cells with more than one floor; %u rows of rooms, %u clusters lie wholly inside a room), "
				            "%llu places looked from, %llu clusters in the rows of which %llu (%.0f%%) hidden, "
				            "%.1f M rays in %.1f s, radius %.0f m, near %.0f m; %.2f MB",
				            opt.vis.high ? "-high" : "", vs.viewers, vs.cells, vs.multiFloorCells, vs.roomRows, vs.roomClusters,
				            static_cast<unsigned long long>(vs.samples),
				            static_cast<unsigned long long>(vs.targets), static_cast<unsigned long long>(vs.hidden),
				            vs.targets ? 100.0 * double(vs.hidden) / double(vs.targets) : 0.0, double(vs.rays) / 1e6, vs.seconds,
				            double(vs.radius) / 100.0, double(opt.vis.nearAlways) / 100.0, double(vs.bytes) / MB));
			} else {
				line("visibility: nothing to put in it");
			}
		}
		// ---- the sun's shadow (--sun-shadow): a mask per grid cell, from the scene as it is (before simplifying) ----
		if (opt.sun.enabled && !scene.indoor) {
			// The cells and how far their chunks reach on x and z (objects stand out of their cell).
			std::map<std::pair<int, int>, size_t> cellOf;
			std::vector<SunMask> cells;
			std::vector<size_t> chunkCell(chunks.size());
			for (size_t i = 0; i < chunks.size(); ++i) {
				const BakedChunk& c = chunks[i];
				auto ins = cellOf.emplace(std::make_pair(c.gridX, c.gridZ), cells.size());
				if (ins.second) {
					SunMask m;
					m.gridX = c.gridX;
					m.gridZ = c.gridZ;
					m.x0 = c.bbMin[0];
					m.z0 = c.bbMin[2];
					m.x1 = c.bbMax[0];
					m.z1 = c.bbMax[2];
					cells.push_back(m);
				}
				SunMask& m = cells[ins.first->second];
				m.x0 = std::min(m.x0, c.bbMin[0]);
				m.z0 = std::min(m.z0, c.bbMin[2]);
				m.x1 = std::max(m.x1, c.bbMax[0]);
				m.z1 = std::max(m.z1, c.bbMax[2]);
				chunkCell[i] = ins.first->second;
			}
			for (SunMask& m : cells) {
				// (A little beyond, so that the last texels are not stretched over the edge.)
				m.x0 -= 50.0f;
				m.z0 -= 50.0f;
				m.x1 += 50.0f;
				m.z1 += 50.0f;
			}
			// What each source material is to the sun: see-through (2), plants and cut-out leaves (1, half a shadow, take none).
			std::vector<u8> sunKind(scene.materials.size(), 0);
			for (size_t i = 0; i < sunKind.size(); ++i) {
				const AoMaterial& am = aoMaterials[i];
				sunKind[i] = am.blocks <= 0.0f ? u8(2) : (am.blocks < 1.0f || (i < plantMaterial.size() && plantMaterial[i])) ? u8(1) : u8(0);
			}
			SunOptions so = opt.sun;
			so.threads = opt.threads;
			SunStats ss;
			const std::vector<SunMask> masks = bakeSunMasks(bw.scene, sunKind, cells, so, ss);
			std::vector<u32> maskTexture(masks.size(), SUN_NONE);
			for (size_t m = 0; m < masks.size(); ++m) {
				const std::string name = "world/" + bw.name + "/sun/" + std::to_string(masks[m].gridX) + "_" + std::to_string(masks[m].gridZ);
				pak.add(name, ASSET_TEXTURE, masks[m].texture);
				maskTexture[m] = u32(textureTable.size());
				textureTable.push_back(hashName(name.c_str()));
			}
			if (textureTable.size() > 65535) throw std::runtime_error("too many textures with the sun's masks");
			wh.textureCount = u32(textureTable.size());
			SunHeader sh {};
			sh.magic = SUN_MAGIC;
			sh.version = SUN_VERSION;
			sh.chunkCount = u32(chunks.size());
			sh.hour = so.hour;
			sunDirectionAt(so.hour, sh.dir);
			sh.texel = so.texel;
			std::vector<u8> sunBlob(sizeof(sh));
			std::memcpy(sunBlob.data(), &sh, sizeof(sh));
			std::vector<SunChunk> sc(chunks.size());
			for (size_t i = 0; i < chunks.size(); ++i) {
				const SunMask& m = masks[chunkCell[i]];
				sc[i].texture = maskTexture[chunkCell[i]];
				sc[i].x0 = m.x0;
				sc[i].z0 = m.z0;
				sc[i].scaleX = 1.0f / std::max(m.x1 - m.x0, 1.0f);
				sc[i].scaleZ = 1.0f / std::max(m.z1 - m.z0, 1.0f);
			}
			append(sunBlob, sc);
			if (grassOk) grassSunMasks = masks;
			const std::string sunName = "world/" + bw.name + "/sun";
			pak.add(sunName, ASSET_META, sunBlob);
			wh.sunEntry = hashName(sunName.c_str());
			line(format("sun's shadow (--sun-shadow): %u cells, the sun of %.1f h (%.0f degrees up), %llu texels of %.0f cm or finer, "
			            "%.1f%% of them shaded; %llu with no ground under them; %.1f M rays in %.1f s; %.2f MB",
			            ss.cells, double(so.hour), double(std::asin(std::clamp(sh.dir[1], -1.0f, 1.0f)) * 57.29578f),
			            static_cast<unsigned long long>(ss.texels), double(so.texel),
			            ss.texels ? 100.0 * double(ss.shaded) / double(ss.texels) : 0.0, static_cast<unsigned long long>(ss.empty),
			            double(ss.rays) / 1e6, ss.seconds, double(ss.bytes) / MB));
		}
		// ---- the grass: the sun's shade at each tuft, then the index and the list ----
		if (grassOk) {
			if (!grassSunMasks.empty()) {
				size_t last = 0;
				shadeGrass(grass, [&](float x, float z, float& shade) {
					for (size_t n = 0; n < grassSunMasks.size(); ++n) {
						const SunMask& m = grassSunMasks[(last + n) % grassSunMasks.size()];
						if (x < m.x0 || x >= m.x1 || z < m.z0 || z >= m.z1 || !m.width || !m.height) continue;
						if (m.texture.size() < sizeof(TexHeader) + m.width * m.height / 2) continue;
						last = (last + n) % grassSunMasks.size();
						const u32 tx = std::min(m.width - 1, u32((x - m.x0) / (m.x1 - m.x0) * float(m.width)));
						const u32 ty = std::min(m.height - 1, u32((z - m.z0) / (m.z1 - m.z0) * float(m.height)));
						const u32 nib = texL4Nibble(m.width, m.height, tx, ty);
						const u8 value = u8((m.texture[sizeof(TexHeader) + (nib >> 1)] >> ((nib & 1u) * 4u)) & 15u);
						shade = 1.0f - float(value) / 15.0f;
						return true;
					}
					return false;
				}, grassStats);
			}
			const std::string grassName = "world/" + bw.name + "/grass", dataName = grassName + "_data", atlasName = "tex/_grass";
			if (!pak.has(atlasName)) {
				const std::vector<u8> atlas = grassAtlasTexture(opt.tex);
				grassStats.atlasBytes = atlas.size();
				pak.add(atlasName, ASSET_TEXTURE, atlas);
			}
			std::vector<u8> grassIndex, grassData;
			encodeGrass(grass, opt.grass, hashName(dataName.c_str()), hashName(atlasName.c_str()), grassIndex, grassData, grassStats);
			pak.add(grassName, ASSET_META, grassIndex);
			pak.add(dataName, ASSET_BLOB, grassData, false); // (raw: the runtime reads it a tile at a time)
			line(format("grass (--grass): %u tufts (%.1f per m2 at full density; %.0f m2 of it) in %u tiles of %.0f m, the fullest %u; "
			            "pictures %.0f/%.0f/%.0f/%.0f %%; %u in the sun's shadow; %u places without ground, %u left out of full tiles; "
			            "old grass: %u triangles taken out%s; %.2f MB",
			            grassStats.tufts, double(opt.grass.density), grassStats.areaFull, grassStats.tiles, double(opt.grass.tileSize) / 100.0,
			            grassStats.maxTile, double(grassStats.variantShare[0] * 100.0f), double(grassStats.variantShare[1] * 100.0f),
			            double(grassStats.variantShare[2] * 100.0f), double(grassStats.variantShare[3] * 100.0f), grassStats.shaded,
			            grassStats.noGround, grassStats.capped, grassStats.oldRemoved, opt.grass.replaceOld ? "" : " (kept: --grass-replace)",
			            double(grassStats.bytes) / MB));
		} else if (opt.grass.enabled && !scene.indoor) {
			line("grass (--grass): none: " + grassWhy);
		}
		// ---- --simplify: fewer triangles in the geometry itself ----
		if (opt.simplify.enabled) {
			SimplifyStats ss;
			buildSimplified(chunks, worldMaterials, opt.simplify, ss);
			if (ss.chunks) {
				const u64 removed = ss.trianglesBefore - ss.trianglesAfter;
				cs.outputTriangles = cs.outputTriangles > removed ? cs.outputTriangles - removed : cs.outputTriangles;
				line(format("simplified geometry (--simplify): %u of %u chunks, %llu -> %llu triangles of what was simplified (%.0f%%), "
				            "vertices %llu -> %llu, %.1f s",
				            ss.chunks, ss.candidates, static_cast<unsigned long long>(ss.trianglesBefore),
				            static_cast<unsigned long long>(ss.trianglesAfter),
				            100.0 * double(ss.trianglesAfter) / double(std::max<u64>(ss.trianglesBefore, 1)),
				            static_cast<unsigned long long>(ss.verticesBefore), static_cast<unsigned long long>(ss.verticesAfter), ss.seconds));
			} else {
				line("simplified geometry (--simplify): nothing to simplify");
			}
			line(format("  of %u chunks: %u have fewer than %u simplifiable triangles, %u would shrink by less than 3%%", ss.candidates,
			            ss.tooFew, opt.simplify.minTriangles, ss.tooLittle));
		}
		if (opt.probe) probeChunks(chunks, bw.scene, remap, opt.probeAt, opt.probeRadius, "as written to the chunks, after seams and simplifying", line);
		// ---- simplified stand-ins for far chunks ----
		if (opt.lod.enabled && !scene.indoor) {
			LodStats ls;
			buildLods(chunks, worldMaterials, opt.lod, ls);
			if (ls.chunks) {
				wh.flags |= WORLDF_LOD;
				line(format("far geometry (--lod): %u of %u chunks got a stand-in, %llu -> %llu triangles of the simplified batches (%.0f%%), "
				            "+%.2f MB, %.1f s",
				            ls.chunks, ls.candidates, static_cast<unsigned long long>(ls.trianglesBefore),
				            static_cast<unsigned long long>(ls.trianglesAfter),
				            100.0 * double(ls.trianglesAfter) / double(std::max<u64>(ls.trianglesBefore, 1)), double(ls.extraBytes) / MB,
				            ls.seconds));
			} else {
				line("far geometry (--lod): nothing to simplify");
			}
			line(format("  of %u chunks: %u of medium or small objects left alone (--no-lod-objects), %u have too few simplifiable triangles "
			            "(%u for the large, %u for objects), %u would shrink by too little; %llu simplifiable triangles in the rest",
			            ls.candidates, ls.notTierZero, ls.tooFew, opt.lod.minTriangles, opt.lod.minObjectTriangles, ls.tooLittle,
			            static_cast<unsigned long long>(ls.simplifiable)));
			if (opt.lod.objects)
				line(format("  chunks of medium and small objects with a stand-in (kept %.0f%% of the triangles of what they replace): %u, +%.2f MB",
				            100.0 * double(opt.lod.ratioObjects), ls.objectChunks, double(ls.objectBytes) / MB));
		}
		std::memcpy(wh.fogColor, scene.fogColor, sizeof(wh.fogColor));
		wh.triangleCount = u32(cs.outputTriangles);
		for (int a = 0; a < 3; ++a) {
			wh.bbMin[a] = 1e30f;
			wh.bbMax[a] = -1e30f;
		}

		if (opt.vertexCache) {
			VertexCacheStats vc;
			std::mutex vcLock;
			std::atomic<size_t> nextChunk {0};
			const auto work = [&] {
				VertexCacheStats mine;
				for (size_t i = nextChunk.fetch_add(1); i < chunks.size(); i = nextChunk.fetch_add(1)) optimizeVertexCache(chunks[i], &mine);
				std::lock_guard<std::mutex> g(vcLock);
				vc.triangles += mine.triangles;
				vc.missesBefore += mine.missesBefore;
				vc.missesAfter += mine.missesAfter;
			};
			std::vector<std::thread> pool;
			for (unsigned t = 1; t < std::max(1u, opt.threads); ++t) pool.emplace_back(work);
			work();
			for (std::thread& t : pool) t.join();
			if (vc.triangles)
				line(format("vertex cache order: %llu triangles; vertex shader runs per triangle %.2f -> %.2f (16-entry FIFO)",
				            static_cast<unsigned long long>(vc.triangles), vc.missesBefore / double(vc.triangles),
				            vc.missesAfter / double(vc.triangles)));
		}
		std::vector<ChunkInfo> infos;
		std::vector<u8> tiers;
		int gx0 = 1 << 30, gx1 = -(1 << 30), gz0 = 1 << 30, gz1 = -(1 << 30);
		const u64 raw0 = pak.rawBytes(ASSET_CHUNK), packed0 = pak.packedBytes(ASSET_CHUNK);
		for (size_t i = 0; i < chunks.size(); ++i) {
			const BakedChunk& c = chunks[i];
			const std::string name = "world/" + bw.name + "/" + std::to_string(i);
			const std::vector<u8> blob = c.serialize();
			pak.add(name, ASSET_CHUNK, blob);

			ChunkInfo ci {};
			std::memcpy(ci.bbMin, c.bbMin, sizeof(ci.bbMin));
			std::memcpy(ci.bbMax, c.bbMax, sizeof(ci.bbMax));
			ci.entryHash = hashName(name.c_str());
			ci.size = u32(blob.size());
			ci.vertexCount = c.header.vertexCount;
			ci.batchCount = c.header.batchCount;
			ci.indexCount = c.header.indexCount;
			infos.push_back(ci);
			tiers.push_back(c.tier);
			for (int a = 0; a < 3; ++a) {
				wh.bbMin[a] = std::min(wh.bbMin[a], c.bbMin[a]);
				wh.bbMax[a] = std::max(wh.bbMax[a], c.bbMax[a]);
			}
			gx0 = std::min(gx0, c.gridX);
			gx1 = std::max(gx1, c.gridX);
			gz0 = std::min(gz0, c.gridZ);
			gz1 = std::max(gz1, c.gridZ);
		}
		result.chunks += chunks.size();
		chunkRaw += pak.rawBytes(ASSET_CHUNK) - raw0;
		chunkPacked += pak.packedBytes(ASSET_CHUNK) - packed0;
		if (scene.hasSpawn) {
			std::memcpy(wh.spawnPos, scene.spawnPos, sizeof(wh.spawnPos));
			wh.spawnYaw = scene.spawnYaw;
		} else {
			wh.spawnPos[0] = (wh.bbMin[0] + wh.bbMax[0]) * 0.5f;
			wh.spawnPos[1] = wh.bbMax[1] + 300.0f;
			wh.spawnPos[2] = (wh.bbMin[2] + wh.bbMax[2]) * 0.5f;
		}

		std::vector<u8> index(sizeof(wh));
		std::memcpy(index.data(), &wh, sizeof(wh));
		append(index, worldMaterials);
		append(index, textureTable);
		append(index, portals);
		append(index, ladders);
		append(index, infos);
		if (chunkerOpt.splitDetail) append(index, tiers);
		if (index.size() != worldBlobSize(wh)) throw std::logic_error("world index layout");
		pak.add("world/" + bw.name, ASSET_WORLD, index);

		// ---- named places, for the scripts ----
		{
			std::vector<WorldPoint> points;
			std::set<u32> seen;
			for (const auto& [name, point] : scene.points) {
				const u32 h = hashName(name.c_str());
				if (!seen.insert(h).second) continue;
				points.push_back(WorldPoint {h, {point.pos[0], point.pos[1], point.pos[2]}, point.yaw});
			}
			std::sort(points.begin(), points.end(), [](const WorldPoint& a, const WorldPoint& b) { return a.nameHash < b.nameHash; });
			PointsHeader ph {};
			ph.magic = POINTS_MAGIC;
			ph.version = POINTS_VERSION;
			ph.count = u32(points.size());
			const std::string scriptName = worldScriptName(bw);
			std::snprintf(ph.script, sizeof(ph.script), "%s", scriptName.c_str());
			std::vector<u8> blob(sizeof(ph));
			std::memcpy(blob.data(), &ph, sizeof(ph));
			append(blob, points);
			pak.add("world/" + bw.name + "/points", ASSET_META, blob);
		}

		// ---- the way net ----
		std::string wayNote;
		if (!scene.wayPoints.empty() || !scene.freePoints.empty()) {
			if (scene.wayPoints.size() > 65534) throw std::runtime_error("world '" + bw.name + "' has too many way points");
			const u32 n = u32(scene.wayPoints.size());
			std::vector<std::vector<u16>> next(n);
			size_t connections = 0;
			for (const auto& [a, b] : scene.wayEdges) {
				if (a >= n || b >= n || a == b) continue;
				if (std::find(next[a].begin(), next[a].end(), u16(b)) != next[a].end()) continue;
				next[a].push_back(u16(b));
				next[b].push_back(u16(a));
				++connections;
			}
			std::string names;
			const auto nameAt = [&](const std::string& name) {
				const u32 at = u32(names.size());
				names += name;
				names.push_back('\0');
				return at;
			};
			std::vector<WayNode> nodes(n);
			std::vector<u16> edges;
			size_t isolated = 0;
			double length = 0;
			for (u32 i = 0; i < n; ++i) {
				const SrcWayPoint& wp = scene.wayPoints[i];
				WayNode& node = nodes[i];
				std::copy_n(wp.pos, 3, node.pos);
				node.yaw = wp.yaw;
				node.nameHash = hashName(wp.name.c_str());
				node.name = nameAt(wp.name);
				node.firstEdge = u32(edges.size());
				node.edgeCount = u16(std::min<size_t>(next[i].size(), 65535));
				node.reserved = 0;
				edges.insert(edges.end(), next[i].begin(), next[i].begin() + node.edgeCount);
				isolated += next[i].empty();
				for (u16 k : next[i]) {
					const SrcWayPoint& o = scene.wayPoints[k];
					length += 0.5 * std::sqrt(double((o.pos[0] - wp.pos[0]) * (o.pos[0] - wp.pos[0]) + (o.pos[1] - wp.pos[1]) * (o.pos[1] - wp.pos[1]) +
					                                 (o.pos[2] - wp.pos[2]) * (o.pos[2] - wp.pos[2])));
				}
			}
			std::vector<FreePoint> free(scene.freePoints.size());
			for (size_t i = 0; i < free.size(); ++i) {
				std::copy_n(scene.freePoints[i].pos, 3, free[i].pos);
				free[i].yaw = scene.freePoints[i].yaw;
				free[i].name = nameAt(scene.freePoints[i].name);
			}
			while (names.size() % 4) names.push_back('\0');
			WayNetHeader nh {};
			nh.magic = WAYNET_MAGIC;
			nh.version = WAYNET_VERSION;
			nh.nodeCount = n;
			nh.edgeCount = u32(edges.size());
			nh.freeCount = u32(free.size());
			nh.nameBytes = u32(names.size());
			std::vector<u8> blob(sizeof(nh));
			std::memcpy(blob.data(), &nh, sizeof(nh));
			append(blob, nodes);
			append(blob, edges);
			while (blob.size() % 4) blob.push_back(0);
			append(blob, free);
			blob.insert(blob.end(), names.begin(), names.end());
			pak.add("world/" + bw.name + "/waynet", ASSET_META, blob);
			wayNote = format("way net: %u points, %zu connections (%.1f km of paths), %zu points without any; %zu free points", n,
			                 connections, length / 100000.0, isolated, free.size());
		}

		// ---- the things people use ----
		std::string mobNote, chestNote;
		if (!scene.mobs.empty()) {
			std::string names;
			std::map<std::string, u32> nameAt;
			std::vector<WorldMob> mobs;
			std::vector<MobSlot> slots;
			std::vector<WorldChest> chests;
			std::vector<WorldMobInfo> mobInfos;
			std::string infoNames(1, '\0');
			std::map<std::string, u32> infoAt;
			const auto infoName = [&](const std::string& text) -> u32 {
				if (text.empty()) return 0;
				auto found = infoAt.find(text);
				if (found != infoAt.end()) return found->second;
				const u32 where = u32(infoNames.size());
				infoNames += text;
				infoNames.push_back('\0');
				infoAt[text] = where;
				return where;
			};
			size_t infoNamed = 0, infoHidden = 0, mobOwned = 0;
			size_t chestItems = 0, chestLocked = 0, chestNamed = 0;
			std::map<std::string, u32> byScheme;
			std::map<std::string, u32> byFunction; // (fix153) onStateFunc (scheme, item) -> how many
			std::map<std::string, u32> byFields; // (fix154) every field the player's things carry, per kind
			size_t withoutSlots = 0;
			for (const SrcMob& src : scene.mobs) {
				if (mobs.size() >= 65534) break;
				WorldMob m {};
				std::copy_n(src.pos, 3, m.pos);
				m.yaw = src.yaw;
				m.schemeHash = hashName(src.scheme.c_str());
				auto at = nameAt.find(src.scheme);
				if (at == nameAt.end()) {
					at = nameAt.emplace(src.scheme, u32(names.size())).first;
					names += src.scheme;
					names.push_back('\0');
				}
				m.scheme = at->second;
				m.firstSlot = u32(slots.size());
				m.states = u8(std::clamp(src.states, 0, 16));
				for (const SrcMobSlot& ss : src.slots) {
					if (m.slotCount == 255) break;
					MobSlot slot {};
					std::copy_n(ss.pos, 3, slot.pos);
					slot.yaw = ss.yaw;
					const auto ends = [&](const char* tail) {
						const size_t n = std::strlen(tail);
						return ss.name.size() >= n && ss.name.compare(ss.name.size() - n, n, tail) == 0;
					};
					slot.kind = ends("_FRONT") ? u8(MOB_SLOT_FRONT) : ends("_BACK") ? u8(MOB_SLOT_BACK) : ends("_DIST") ? u8(MOB_SLOT_DIST) : u8(MOB_SLOT_PLAIN);
					slots.push_back(slot);
					++m.slotCount;
				}
				withoutSlots += m.slotCount == 0;
				++byScheme[src.scheme];
				if (src.container) {
					WorldChest c {};
					c.mob = u32(mobs.size());
					c.flags = src.locked ? u32(CHEST_LOCKED) : 0u;
					c.key = src.key.empty() ? 0u : hashName(src.key.c_str());
					const auto intern = [&](const std::string& text) {
						auto found = nameAt.find("\x01" + text);
						if (found != nameAt.end()) return found->second;
						const u32 where = u32(names.size());
						names += text;
						names.push_back('\0');
						nameAt["\x01" + text] = where;
						return where;
					};
					c.pick = intern(src.pick);
					c.contents = intern(src.contents);
					c.name = src.vobName.empty() ? 0u : hashName(src.vobName.c_str());
					chestNamed += c.name != 0;
					chests.push_back(c);
					chestItems += std::count(src.contents.begin(), src.contents.end(), ',') + (src.contents.empty() ? 0 : 1);
					chestLocked += src.locked;
				}
				{
					WorldMobInfo info {};
					info.focus = infoName(src.focus);
					info.onState = infoName(src.onState);
					info.condition = infoName(src.condition);
					info.useItem = infoName(src.useItem);
					info.target = infoName(src.target);
					info.owner = infoName(src.owner);
					info.ownerGuild = infoName(src.ownerGuild);
					mobOwned += !src.owner.empty() || !src.ownerGuild.empty();
					info.kind = u8(src.kind);
					info.rewind = src.rewind ? 1 : 0;
					// (fix153) What the player's things run in the scripts: the crafting of a game is there, if it has any.
					if (!src.onState.empty() && !src.focus.empty()) {
						std::string what = upper(src.onState) + " (" + src.scheme;
						if (!src.useItem.empty()) what += ", needs " + upper(src.useItem);
						++byFunction[what + ")"];
					}
					if (!src.container && (!src.onState.empty() || !src.condition.empty() || !src.useItem.empty() || !src.target.empty() ||
					                       !src.focus.empty())) {
						++byFields[format("%s: focus '%s', onState '%s', condition '%s', needs '%s', target '%s'", src.scheme.c_str(),
						                  src.focus.c_str(), src.onState.c_str(), src.condition.c_str(), src.useItem.c_str(),
						                  src.target.c_str())];
					}
					infoNamed += info.focus != 0;
					infoHidden += info.focus == 0;
					mobInfos.push_back(info);
				}
				mobs.push_back(m);
			}
			while (names.size() % 4) names.push_back('\0');
			MobsHeader mh {};
			mh.magic = MOBS_MAGIC;
			mh.version = MOBS_VERSION;
			mh.mobCount = u32(mobs.size());
			mh.slotCount = u32(slots.size());
			mh.nameBytes = u32(names.size());
			std::vector<u8> blob(sizeof(mh));
			std::memcpy(blob.data(), &mh, sizeof(mh));
			append(blob, mobs);
			append(blob, slots);
			blob.insert(blob.end(), names.begin(), names.end());
			pak.add("world/" + bw.name + "/mobs", ASSET_META, blob);
			if (!chests.empty()) {
				ChestsHeader ch {};
				ch.magic = CHESTS_MAGIC;
				ch.version = CHESTS_VERSION;
				ch.count = u32(chests.size());
				ch.nameBytes = u32(names.size());
				std::vector<u8> cblob(sizeof(ch));
				std::memcpy(cblob.data(), &ch, sizeof(ch));
				append(cblob, chests);
				cblob.insert(cblob.end(), names.begin(), names.end());
				pak.add("world/" + bw.name + "/chests", ASSET_META, cblob);
				chestNote = format("containers: %zu (%zu locked, %zu named for the scripts), with %zu stacks of things inside", chests.size(), chestLocked, chestNamed, chestItems);
			}
			if (!mobInfos.empty()) {
				while (infoNames.size() % 4) infoNames.push_back('\0');
				MobInfoHeader ih {};
				ih.magic = MOBINFO_MAGIC;
				ih.version = MOBINFO_VERSION;
				ih.count = u32(mobInfos.size());
				ih.nameBytes = u32(infoNames.size());
				std::vector<u8> iblob(sizeof(ih));
				std::memcpy(iblob.data(), &ih, sizeof(ih));
				append(iblob, mobInfos);
				iblob.insert(iblob.end(), infoNames.begin(), infoNames.end());
				pak.add("world/" + bw.name + "/mobinfo", ASSET_META, iblob);
				line(format("things to use by the player: %zu have a name to show (he may use those), %zu have none (the inhabitants' only); "
				            "%zu belong to somebody (a person or a guild)",
				            infoNamed, infoHidden, mobOwned));
				std::string fns;
				size_t shown = 0;
				for (const auto& [what, count] : byFunction) {
					if (++shown > 24) {
						fns += ", ...";
						break;
					}
					fns += format("%s%s x%u", fns.empty() ? "" : ", ", what.c_str(), count);
				}
				// (fix154) What every kind of thing carries: how a game that is not run by <function>_S1 does its crafting.
				line(format("fields of the things to use (%zu combinations; the forge, the lab and the cauldron are in here):", byFields.size()));
				{
					size_t n = 0;
					for (const auto& [what, count] : byFields) {
						if (++n > 80) {
							line("  ...");
							break;
						}
						line(format("  %s x%u", what.c_str(), count));
					}
				}
				line(format("things of the player's that run the scripts (<function>_S1 when used, _S0 when left): %zu kinds: ",
				            byFunction.size()) +
				     (fns.empty() ? std::string("none") : fns));
			}
			std::vector<std::pair<u32, std::string>> order;
			for (const auto& [scheme, count] : byScheme) order.push_back({count, scheme});
			std::sort(order.rbegin(), order.rend());
			std::string list;
			for (size_t i = 0; i < order.size() && i < 24; ++i) list += format("%s%s x%u", i ? ", " : "", order[i].second.c_str(), order[i].first);
			mobNote = format("things to use: %zu of %zu kinds, with %zu places to stand at (%zu have none: nobody can use "
			                 "those): ",
			                 mobs.size(), byScheme.size(), slots.size(), withoutSlots) +
			          list + (order.size() > 24 ? ", ..." : "");
		}

		// ---- the world's own sounds ----
		if (!scene.ambient.empty() && opt.soundRate > 0) {
			const std::vector<u8> ambient = bakeAmbient(scene.ambient);
			AmbientHeader ah;
			std::memcpy(&ah, ambient.data(), sizeof(ah));
			if (ah.count > 0) {
				pak.add("world/" + bw.name + "/ambient", ASSET_META, ambient);
				line(format("ambient sounds: %u placed in world '%s'", ah.count, bw.name.c_str()));
			}
		}

		// ---- triggers and movers ----
		if (!scene.triggers.empty()) {
			std::vector<char> tnames(1, '\0'); // (offset 0: none)
			std::map<std::string, u32> named;
			const auto nameOf = [&](const std::string& n) -> u32 {
				if (n.empty()) return 0;
				auto it = named.find(n);
				if (it != named.end()) return it->second;
				const u32 at = u32(tnames.size());
				tnames.insert(tnames.end(), n.begin(), n.end());
				tnames.push_back('\0');
				named.emplace(n, at);
				return at;
			};
			std::vector<WorldTrigger> table;
			std::vector<MoverKey> keys;
			std::vector<TriggerTarget> targets;
			std::vector<float> collision;
			u32 counts[8] = {};
			u32 locked = 0, solid = 0;
			std::set<std::string> visuals;
			for (const SrcTrigger& t : scene.triggers) {
				WorldTrigger wt {};
				wt.kind = u8(t.kind);
				wt.behavior = u8(t.kind == TRG_LIST ? t.listMode : t.behavior);
				wt.flags = u16((t.startEnabled ? TRGF_START_ENABLED : 0) | (t.sendUntrigger ? TRGF_SEND_UNTRIGGER : 0) |
				              (t.reactTrigger ? TRGF_REACT_TRIGGER : 0) | (t.reactTouch ? TRGF_REACT_TOUCH : 0) |
				              (t.respondPc ? TRGF_RESPOND_PC : 0) | (t.respondNpc ? TRGF_RESPOND_NPC : 0) | (t.fireOnce ? TRGF_FIRE_ONCE : 0) |
				              (t.autoRotate ? TRGF_AUTO_ROTATE : 0));
				wt.name = nameOf(t.name);
				wt.target = nameOf(t.target);
				wt.visual = nameOf(t.visual);
				wt.maxActivations = t.maxActivations;
				wt.retriggerDelay = t.retriggerDelay;
				wt.fireDelay = t.fireDelay;
				for (int a = 0; a < 3; ++a) wt.bbMin[a] = t.bbMin[a], wt.bbMax[a] = t.bbMax[a];
				if (!t.collision.empty()) {
					wt.flags = u16(wt.flags | TRGF_COLLIDES);
					wt.collFirst = u32(collision.size() / 9);
					wt.collCount = u32(t.collision.size() / 9);
					collision.insert(collision.end(), t.collision.begin(), t.collision.end());
					++solid;
				}
				if (t.kind == TRG_DOOR) {
					wt.first = u32(keys.size());
					wt.count = u16(t.keys.size());
					for (const auto& k : t.keys) {
						MoverKey mk {};
						for (int a = 0; a < 3; ++a) mk.pos[a] = k[size_t(a)];
						for (int a = 0; a < 4; ++a) mk.rot[a] = k[size_t(3 + a)];
						keys.push_back(mk);
					}
					if (t.locked) wt.flags = u16(wt.flags | TRGF_LOCKED);
					locked += t.locked;
					wt.target = nameOf(t.key);
					wt.sfx[0] = nameOf(t.pick);
					if (!t.visual.empty()) visuals.insert(t.visual);
				} else if (t.kind == TRG_MOVER) {
					wt.first = u32(keys.size());
					wt.count = u16(std::min<size_t>(t.keys.size(), 65535));
					for (size_t k = 0; k < wt.count; ++k) {
						MoverKey mk {};
						for (int a = 0; a < 3; ++a) mk.pos[a] = t.keys[k][size_t(a)];
						for (int a = 0; a < 4; ++a) mk.rot[a] = t.keys[k][size_t(3 + a)];
						keys.push_back(mk);
					}
					wt.speedMode = u8(t.speedMode);
					wt.lerp = u8(t.lerp);
					wt.speed = t.speed;
					wt.stayOpen = t.stayOpen;
					const std::string* sfx[5] = {&t.sfxOpenStart, &t.sfxOpenEnd, &t.sfxCloseStart, &t.sfxCloseEnd, &t.sfxMoving};
					for (int k = 0; k < 5; ++k) wt.sfx[k] = nameOf(*sfx[k]);
					if (!t.visual.empty()) visuals.insert(t.visual);
				} else if (t.kind == TRG_CAMERA) {
					wt.first = u32(keys.size());
					wt.count = u16(std::min<size_t>(t.keys.size(), 255 + 255));
					wt.speedMode = u8(std::min(t.camTargets, 255));
					for (size_t k = 0; k < wt.count; ++k) {
						MoverKey mk {};
						for (int a = 0; a < 3; ++a) mk.pos[a] = t.keys[k][size_t(a)];
						for (int a = 0; a < 4; ++a) mk.rot[a] = t.keys[k][size_t(3 + a)];
						keys.push_back(mk);
					}
					wt.speed = t.speed;
					wt.stayOpen = t.stayOpen;
				} else if (t.kind == TRG_LIST) {
					wt.first = u32(targets.size());
					wt.count = u16(std::min<size_t>(t.list.size(), 65535));
					for (size_t k = 0; k < wt.count; ++k) targets.push_back(TriggerTarget {nameOf(t.list[k].first), t.list[k].second});
				}
				if (t.kind >= 1 && t.kind <= 7) ++counts[t.kind];
				table.push_back(wt);
			}
			while (tnames.size() % 4) tnames.push_back('\0');
			TriggersHeader th {};
			th.magic = TRIGGERS_MAGIC;
			th.version = TRIGGERS_VERSION;
			th.count = u32(table.size());
			th.keyCount = u32(keys.size());
			th.targetCount = u32(targets.size());
			th.nameBytes = u32(tnames.size());
			th.collisionTris = u32(collision.size() / 9);
			std::vector<u8> blob(sizeof(th));
			std::memcpy(blob.data(), &th, sizeof(th));
			append(blob, table);
			append(blob, keys);
			append(blob, targets);
			append(blob, collision);
			blob.insert(blob.end(), tnames.begin(), tnames.end());
			pak.add("world/" + bw.name + "/triggers", ASSET_META, blob);
			u32 moving = 0;
			for (const SrcTrigger& t : scene.triggers) moving += t.kind == TRG_MOVER && !t.visual.empty();
			line(format("triggers: %u triggers, %u lists, %u script triggers, %u world starts, %u movers (%u drawn moving), %u doors (%u locked), "
			            "%u camera flights; %u solid (%zu triangles), %zu meshes; key rotations read %s: %u, %u the other way",
			            counts[TRG_TRIGGER], counts[TRG_LIST], counts[TRG_SCRIPT], counts[TRG_WORLDSTART], counts[TRG_MOVER], moving, counts[TRG_DOOR],
			            locked, counts[TRG_CAMERA], solid, collision.size() / 9,
			            visuals.size(), scene.moverConjugated >= scene.moverPlain ? "conjugated" : "as stored",
			            std::max(scene.moverConjugated, scene.moverPlain), std::min(scene.moverConjugated, scene.moverPlain)));
		}

		// ---- the rooms (BSP sectors) ----
		if (!scene.roomNames.empty() && !scene.roomBoxes.empty() && scene.roomNames.size() < 65535) {
			std::vector<char> rnames;
			std::vector<u32> offsets;
			for (const std::string& n : scene.roomNames) {
				offsets.push_back(u32(rnames.size()));
				rnames.insert(rnames.end(), n.begin(), n.end());
				rnames.push_back('\0');
			}
			while (rnames.size() % 4) rnames.push_back('\0');
			std::vector<RoomBox> boxes;
			for (const auto& b : scene.roomBoxes) {
				RoomBox rb {};
				for (int k = 0; k < 3; ++k) rb.bbMin[k] = b.bbMin[k], rb.bbMax[k] = b.bbMax[k];
				rb.room = u16(b.room + 1);
				boxes.push_back(rb);
			}
			RoomsHeader rh {};
			rh.magic = ROOMS_MAGIC;
			rh.version = ROOMS_VERSION;
			rh.roomCount = u32(scene.roomNames.size());
			rh.boxCount = u32(boxes.size());
			rh.nameBytes = u32(rnames.size());
			std::vector<u8> blob(sizeof(rh));
			std::memcpy(blob.data(), &rh, sizeof(rh));
			append(blob, boxes);
			append(blob, offsets);
			blob.insert(blob.end(), rnames.begin(), rnames.end());
			pak.add("world/" + bw.name + "/rooms", ASSET_META, blob);
			std::string list;
			for (size_t i = 0; i < scene.roomNames.size() && i < 12; ++i) list += (i ? ", " : "") + scene.roomNames[i];
			line(format("rooms: %zu (BSP sectors, %zu boxes): ", scene.roomNames.size(), boxes.size()) + list +
			     (scene.roomNames.size() > 12 ? ", ..." : ""));
		} else {
			line("rooms: none in this world (no BSP sectors): nobody's house is told from the street");
		}

		// ---- the world's music zones ----
		if (!scene.musicZones.empty() && opt.soundRate > 0) {
			pak.add("world/" + bw.name + "/music", ASSET_META, bakeMusicZones(scene.musicZones));
			line(format("music zones: %zu in world '%s'", scene.musicZones.size(), bw.name.c_str()));
		}

		// ---- the world's fog zones ----
		if (opt.fogZones && !scene.fogZones.empty()) {
			FogZonesHeader fh {};
			fh.magic = FOG_ZONES_MAGIC;
			fh.version = FOG_ZONES_VERSION;
			std::vector<u8> blob(sizeof(fh));
			for (const SrcFogZone& z : scene.fogZones) {
				if (fh.count >= 256u) break;
				FogZone f {};
				std::copy_n(z.bbMin, 3, f.bbMin);
				std::copy_n(z.bbMax, 3, f.bbMax);
				f.range = z.range;
				f.inner = z.inner;
				std::copy_n(z.color, 3, f.color);
				f.flags = u8((z.overrideColor ? FOGZ_OVERRIDE_COLOR : 0) | (z.fadeSky ? FOGZ_FADE_SKY : 0));
				const size_t at = blob.size();
				blob.resize(at + sizeof(f));
				std::memcpy(blob.data() + at, &f, sizeof(f));
				++fh.count;
			}
			std::memcpy(blob.data(), &fh, sizeof(fh));
			pak.add("world/" + bw.name + "/fog", ASSET_META, blob);
			line(format("fog zones: %u in world '%s'", fh.count, bw.name.c_str()));
		} else if (opt.fogZones) {
			line(format("fog zones: none in world '%s' (no zCZoneZFog): fog and water follow no region", bw.name.c_str()));
		}

		// ---- report ----
		line(format("world '%s': %.0f x %.0f x %.0f m, grid %d x %d cells of %.0f m", bw.name.c_str(),
		            (wh.bbMax[0] - wh.bbMin[0]) / 100.0, (wh.bbMax[1] - wh.bbMin[1]) / 100.0,
		            (wh.bbMax[2] - wh.bbMin[2]) / 100.0, gx1 - gx0 + 1, gz1 - gz0 + 1, wh.chunkSize / 100.0));
		line(format("geometry: %llu triangles in, %llu out (%llu degenerate dropped), %llu vertices (%.2f per triangle)",
		            static_cast<unsigned long long>(cs.inputTriangles), static_cast<unsigned long long>(cs.outputTriangles),
		            static_cast<unsigned long long>(cs.degenerate), static_cast<unsigned long long>(cs.vertices),
		            cs.outputTriangles ? double(cs.vertices) / double(cs.outputTriangles) : 0.0));
		line(format("vertex sharing: %llu distinct positions, %llu with UVs, %llu with UVs and light",
		            static_cast<unsigned long long>(cs.uniquePositions), static_cast<unsigned long long>(cs.uniquePosUv),
		            static_cast<unsigned long long>(cs.vertices)));
		// A cave has as much ceiling as floor, so only a clear majority either
		// way means something.
		line(format("winding check: %.0f%% of the horizontal front-facing area looks up%s", upShare * 100.0,
		            upShare >= 0.6   ? ""
		            : upShare >= 0.4 ? " (says nothing here: an indoor world has as much ceiling as floor)"
		                             : " - WARNING: the world looks inside out, bake again with --flip"));
		line(format("chunks: %zu (%u cells split), largest %u triangles / %u vertices / %u batches", chunks.size(),
		            cs.splitChunks, cs.maxChunkTriangles, cs.maxChunkVertices, cs.maxChunkBatches));
		{
			// Coarser steps come from giant triangles (sea, far mountains); they
			// get chunks of their own, so they do not cost their cells precision.
			std::string steps;
			for (int k = 0; k < 8; ++k) {
				if (!cs.stepChunks[k]) continue;
				steps += format("%s%u at %s%g cm", steps.empty() ? "" : ", ", cs.stepChunks[k], k == 7 ? ">= " : "",
				                double(opt.chunker.positionStep) * double(1 << k));
			}
			line("position step: " + steps);
		}
		if (opt.chunker.seal)
			line(format("seams: %llu vertices welded to a neighbour within %.2f cm, %llu T-junctions closed within %.2f cm "
			            "(+%llu triangles)",
			            static_cast<unsigned long long>(cs.welded), double(opt.chunker.sealTolerance) * 0.5,
			            static_cast<unsigned long long>(cs.tJunctions), double(opt.chunker.sealTolerance),
			            static_cast<unsigned long long>(cs.sealTriangles)));
		else
			line("seams: not repaired (--no-seal)");
		if (chunkerOpt.splitDetail) {
			const double mb = double(1 << 20);
			auto bytes = [&](int t) { return (double(cs.tierVertices[t]) * sizeof(WorldVertex) + double(cs.tierTriangles[t]) * 3.0 * sizeof(u16)) / mb; };
			line(format("chunks by detail tier (--no-split: one chunk holds everything): %u of the world and big things (%.1f MB, always around the "
			            "camera), %u with medium objects (%.1f MB, within their reach), %u with small ones (%.1f MB, within theirs)",
			            cs.tierChunks[0], bytes(0), cs.tierChunks[1], bytes(1), cs.tierChunks[2], bytes(2)));
		}
		line(format("draw distance classes: %llu triangles always drawn, %llu medium, %llu small; %llu batches (%.1f per chunk)",
		            static_cast<unsigned long long>(cs.detailTriangles[DETAIL_ALWAYS]),
		            static_cast<unsigned long long>(cs.detailTriangles[DETAIL_MEDIUM]),
		            static_cast<unsigned long long>(cs.detailTriangles[DETAIL_SMALL]),
		            static_cast<unsigned long long>(cs.batches), double(cs.batches) / double(chunks.size())));
		if (cs.clusters)
			line(format("clusters: the chunks are cut into %llu boxes of %.0f m (%u at most in one%s): %.0f triangles and %.1f "
			            "batches in a box on average",
			            static_cast<unsigned long long>(cs.clusters), double(opt.chunker.clusterSize) / 100.0, cs.maxChunkClusters,
			            cs.coarsenedChunks ? format("; %u chunks needed bigger boxes", cs.coarsenedChunks).c_str() : "",
			            double(cs.outputTriangles) / double(cs.clusters), double(cs.batches) / double(cs.clusters)));
		if (opt.ao.level != AO_OFF)
			line(format("ambient occlusion (--ao %s): %llu corners from %llu places, %u rays of %.1f m each against %llu "
			            "triangles (%.1f M rays, %.1f s); %.0f%% shut in on average, %llu corners more than half; %u triangles added by cutting the large ones near something (--ao-extra)",
			            opt.ao.level == AO_HIGH ? "high" : "low", static_cast<unsigned long long>(ao.corners),
			            static_cast<unsigned long long>(ao.unique), ao.rayCount, double(ao.radius) / 100.0,
			            static_cast<unsigned long long>(ao.occluders), double(ao.rays) / 1e6, ao.seconds, double(ao.mean) * 100.0,
			            static_cast<unsigned long long>(ao.darkCorners), ao.added));
		else
			line("ambient occlusion: not baked (--ao low|high)");
		if (opt.light.enabled)
			line(format("lamp light (--lights): %zu lamps in the world, %u baked (%u static ones left out), %llu corners lit by %.2f M "
			            "shadow rays, %u triangles added by cutting those near a lamp (%.1f s)",
			            bw.scene.lights.size(), lightStats.lamps, lightStats.skippedStatic, static_cast<unsigned long long>(lightStats.corners),
			            double(lightStats.rays) / 1e6, lightStats.added, lightStats.seconds));
		if (opt.wind)
			line(format("wind: %llu corners sway, %u world materials marked", static_cast<unsigned long long>(swayingCorners),
			            windyMaterials));
		else
			line("wind: not baked (--wind)");
		line(format("materials: %zu world materials from %zu source materials, %zu textures; %u blended (%u water), "
		            "%u animated, %u scrolling",
		            worldMaterials.size(), size_t(std::count(used[w].begin(), used[w].end(), 1)), textureTable.size(),
		            blended, water, animated, scrolling));
		line(format("material mean colours (stand-in until the texture arrives): %u of %zu materials", averagesStored, worldMaterials.size()));
		if (!untextured.empty()) {
			std::string list;
			for (size_t i = 0; i < untextured.size() && i < 16; ++i) list += (i ? ", " : "") + untextured[i];
			if (untextured.size() > 16) list += ", ...";
			line(format("untextured: %zu materials drawn in their own colour: ", untextured.size()) + list);
		}
		if (scene.indoor)
			line(format("indoor world: no sky, background colour %u %u %u", scene.fogColor[0], scene.fogColor[1], scene.fogColor[2]));
		if (!pointNotes[w].empty()) line(pointNotes[w]);
		if (!wayNote.empty()) line(wayNote);
		if (!mobNote.empty()) line(mobNote);
		if (!chestNote.empty()) line(chestNote);
		if (!ladders.empty())
			line(format("ladders: %zu (%zu read from their models, the side to climb from known for %zu)", ladders.size(), ladderInfoCount,
			            ladderSideCount));
		if (!scene.portals.empty()) {
			std::string list;
			for (size_t i = 0; i < portalNotes.size(); ++i) list += (i ? "; " : "") + portalNotes[i];
			line(format("portals: %zu of %zu lead to a baked world: ", portals.size(), scene.portals.size()) + list);
		}
		if (cs.uvRebased)
			line(format("note: %llu triangles needed a private UV origin (wide texture spans)",
			            static_cast<unsigned long long>(cs.uvRebased)));
		if (cs.uvClamped)
			line(format("WARNING: %llu triangles span more than %.0f texture repeats and were clamped",
			            static_cast<unsigned long long>(cs.uvClamped), double(32767.0f * WORLD_UV_SCALE)));
	}

	if (worlds.size() > 1) line("== all worlds ==");
	line(format("textures: %zu baked (%u cut-out), max edge %d, %.1f%% of the source texels kept, %zu missing",
	            result.textures, alphaTextures, opt.tex.maxSize, srcTexels ? 100.0 * double(texels) / double(srcTexels) : 0.0,
	            missing.size()));
	line(format("size: chunks %.2f MB (%.2f MB packed), textures %.2f MB (%.2f MB packed)", double(chunkRaw) / MB,
	            double(chunkPacked) / MB, double(pak.rawBytes(ASSET_TEXTURE)) / MB,
	            double(pak.packedBytes(ASSET_TEXTURE)) / MB));
	if (!missing.empty()) {
		std::string list;
		for (size_t i = 0; i < missing.size() && i < 12; ++i) list += (i ? ", " : "") + missing[i];
		if (missing.size() > 12) list += ", ...";
		line("WARNING: missing textures (drawn in the material's colour, or as a purple checker if it has none): " + list);
	}

	lap("worlds");
	// ---- characters ----
	// A character that cannot be baked is left out, with the reason in the
	// report; the worlds are worth having without it.
	bool heroBaked = false;
	for (size_t i = 0; i < models.size(); ++i) {
		try {
			const ModelBakeResult mr = bakeModel(models[i], opt, pak);
			result.report += mr.report;
			++result.models;
			heroBaked = heroBaked || i == 0;
		} catch (const std::runtime_error& ex) {
			line("== model '" + models[i].name + "' ==");
			line(std::string("WARNING: not baked: ") + ex.what() + (i == 0 ? "; the game stays first-person" : ""));
		}
	}
	if (heroBaked) {
		const std::string hero = lower(models[0].name);
		pak.add("meta/hero", ASSET_META, std::vector<u8>(hero.begin(), hero.end()));
	}

	lap("hero");
	// ---- the scripts and the people they put into the worlds ----
	// (Whatever goes wrong there, the worlds are worth having without it.)
	try {
		bakePopulation(worlds, scripts, opt, pak, line, result);
	} catch (const std::exception& ex) {
		line(std::string("WARNING: the scripts' part of the bake stopped: ") + ex.what());
	}
	lap("scripts and inhabitants");
	line("time: " + lapText);

	const std::string& start = worlds[0].name;
	pak.add("meta/start", ASSET_META, std::vector<u8>(start.begin(), start.end()));
	pak.add("meta/report", ASSET_META, std::vector<u8>(result.report.begin(), result.report.end()));
	return result;
}

} // namespace g3
