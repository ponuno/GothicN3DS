#include "bake.h"
#include "sound_bake.h"

#include "ground.h"
#include "model_bake.h"
#include "populate.h"

#include "g3ds/game.h"

#include "g3ds/tex.h"
#include "g3ds/world.h"

#include <algorithm>
#include <atomic>
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
const float kLadderReach = 45.0f; // cm around a ladder from which it can be climbed
const float kLadderTop = 100.0f;  // cm above its top the climb continues, to get over the edge

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
};

std::string format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string format(const char* fmt, ...) {
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

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
	auto line = [&](const std::string& s) {
		result.report += s + "\n";
		if (!opt.quiet) std::puts(s.c_str());
	};

	// ---- which materials and textures are referenced, by any world ----
	std::vector<std::vector<char>> used(worlds.size());
	std::vector<std::vector<u32>> materialTriangles(worlds.size());
	std::vector<TexJob> jobs;
	std::map<std::string, size_t> jobIndex;                 // pak name -> job
	std::map<std::string, std::vector<std::string>> frames; // base pak name -> pak names of all frames
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
			if (frames.count(base)) continue;
			std::vector<std::string>& list = frames[base];
			list.push_back(base);
			// Probe for further animation frames; loading is cheap next to encoding.
			for (size_t k = 1; k < kMaxFrames && scene.loadTexture; ++k) {
				const std::string next = frameName(tex, k);
				SrcImage probe;
				if (next.empty() || !scene.loadTexture(next, probe)) break;
				list.push_back(addJob(next, w));
			}
		}
	}

	// ---- textures (ETC1 encoding dominates the bake time, so fan it out) ----
	{
		std::atomic<size_t> next {0};
		std::mutex loadLock;
		std::string firstError;
		auto worker = [&]() {
			for (;;) {
				const size_t i = next.fetch_add(1);
				if (i >= jobs.size()) return;
				try {
					SrcImage img;
					bool found;
					{
						std::lock_guard<std::mutex> g(loadLock);
						const SrcScene& scene = worlds[jobs[i].world].scene;
						found = scene.loadTexture && scene.loadTexture(jobs[i].source, img);
					}
					if (!found) continue;
					jobs[i].data = bakeTexture(img, opt.tex, &jobs[i].info);
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
		alphaTextures += j.info.alpha;
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
		u32 blended = 0, animated = 0, scrolling = 0, water = 0;
		// "Graphics+": which materials sway, and what each does for the occlusion.
		std::vector<bool> windy(scene.materials.size(), false);
		u64 swayingCorners = 0;
		for (SrcTriangle& t : bw.scene.triangles)
			for (SrcVertex& v : t.v) {
				if (!opt.wind) v.wind = 0;
				if (!v.wind || t.material >= windy.size()) continue;
				windy[t.material] = true;
				++swayingCorners;
			}
		std::vector<AoMaterial> aoMaterials(scene.materials.size(), AoMaterial {0.0f, false});
		u32 windyMaterials = 0;
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
		AoStats ao;
		if (opt.ao.level != AO_OFF) bakeOcclusion(bw.scene, aoMaterials, opt.ao, ao);
		const bool hasWind = opt.wind && windyMaterials > 0;
		ChunkerOptions chunkerOpt = opt.chunker;
		chunkerOpt.vertexFx = opt.ao.level != AO_OFF || hasWind;
		ChunkerStats cs;
		std::vector<BakedChunk> chunks = buildChunks(scene, remap, materialOrder, chunkerOpt, cs);
		if (chunks.empty()) throw std::runtime_error("world '" + bw.name + "' has no drawable triangles");

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
		for (const SrcLadder& sl : scene.ladders) {
			WorldLadder l {};
			for (int a = 0; a < 3; ++a) {
				const float grow = a == 1 ? 0.0f : kLadderReach;
				l.bbMin[a] = sl.bbMin[a] - grow;
				l.bbMax[a] = sl.bbMax[a] + (a == 1 ? kLadderTop : grow);
			}
			ladders.push_back(l);
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
		std::memcpy(wh.fogColor, scene.fogColor, sizeof(wh.fogColor));
		wh.triangleCount = u32(cs.outputTriangles);
		for (int a = 0; a < 3; ++a) {
			wh.bbMin[a] = 1e30f;
			wh.bbMax[a] = -1e30f;
		}

		std::vector<ChunkInfo> infos;
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
			size_t chestItems = 0, chestLocked = 0;
			std::map<std::string, u32> byScheme;
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
					chests.push_back(c);
					chestItems += std::count(src.contents.begin(), src.contents.end(), ',') + (src.contents.empty() ? 0 : 1);
					chestLocked += src.locked;
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
				chestNote = format("containers: %zu (%zu locked), with %zu stacks of things inside", chests.size(), chestLocked, chestItems);
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

		// ---- the world's music zones ----
		if (!scene.musicZones.empty() && opt.soundRate > 0) {
			pak.add("world/" + bw.name + "/music", ASSET_META, bakeMusicZones(scene.musicZones));
			line(format("music zones: %zu in world '%s'", scene.musicZones.size(), bw.name.c_str()));
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
			            "triangles (%.1f M rays, %.1f s); %.0f%% shut in on average, %llu corners more than half",
			            opt.ao.level == AO_HIGH ? "high" : "low", static_cast<unsigned long long>(ao.corners),
			            static_cast<unsigned long long>(ao.unique), ao.rayCount, double(ao.radius) / 100.0,
			            static_cast<unsigned long long>(ao.occluders), double(ao.rays) / 1e6, ao.seconds, double(ao.mean) * 100.0,
			            static_cast<unsigned long long>(ao.darkCorners)));
		else
			line("ambient occlusion: not baked (--ao low|high)");
		if (opt.wind)
			line(format("wind: %llu corners sway, %u world materials marked", static_cast<unsigned long long>(swayingCorners),
			            windyMaterials));
		else
			line("wind: not baked (--wind)");
		line(format("materials: %zu world materials from %zu source materials, %zu textures; %u blended (%u water), "
		            "%u animated, %u scrolling",
		            worldMaterials.size(), size_t(std::count(used[w].begin(), used[w].end(), 1)), textureTable.size(),
		            blended, water, animated, scrolling));
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
		if (!ladders.empty()) line(format("ladders: %zu", ladders.size()));
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

	// ---- the scripts and the people they put into the worlds ----
	// (Whatever goes wrong there, the worlds are worth having without it.)
	try {
		bakePopulation(worlds, scripts, opt, pak, line, result);
	} catch (const std::exception& ex) {
		line(std::string("WARNING: the scripts' part of the bake stopped: ") + ex.what());
	}

	const std::string& start = worlds[0].name;
	pak.add("meta/start", ASSET_META, std::vector<u8>(start.begin(), start.end()));
	pak.add("meta/report", ASSET_META, std::vector<u8>(result.report.begin(), result.report.end()));
	return result;
}

} // namespace g3
