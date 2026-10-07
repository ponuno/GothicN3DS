#include "bake.h"

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

// "Foo_Bar.TGA" -> "tex/foo_bar"
std::string texturePakName(const std::string& texture) { return "tex/" + lower(baseName(texture)); }

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

BakeResult bakeWorlds(std::vector<BakeWorld>& worlds, const BakeOptions& opt, PakWriter& pak) {
	if (worlds.empty()) throw std::runtime_error("nothing to bake");
	BakeResult result;
	auto line = [&](const std::string& s) {
		result.report += s + "\n";
		if (!opt.quiet) std::puts(s.c_str());
	};

	// ---- which materials and textures are referenced, by any world ----
	std::vector<std::vector<char>> used(worlds.size());
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
		for (const SrcTriangle& t : scene.triangles) {
			if (t.material >= scene.materials.size()) throw std::runtime_error("triangle references an unknown material");
			used[w][t.material] = 1;
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
		u32 blended = 0, animated = 0, scrolling = 0, water = 0;
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
		ChunkerStats cs;
		std::vector<BakedChunk> chunks = buildChunks(scene, remap, materialOrder, opt.chunker, cs);
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
		line(format("materials: %zu world materials from %zu source materials, %zu textures; %u blended (%u water), "
		            "%u animated, %u scrolling",
		            worldMaterials.size(), size_t(std::count(used[w].begin(), used[w].end(), 1)), textureTable.size(),
		            blended, water, animated, scrolling));
		if (scene.indoor)
			line(format("indoor world: no sky, background colour %u %u %u", scene.fogColor[0], scene.fogColor[1], scene.fogColor[2]));
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
		line("WARNING: missing textures (drawn with the checker fallback): " + list);
	}

	const std::string& start = worlds[0].name;
	pak.add("meta/start", ASSET_META, std::vector<u8>(start.begin(), start.end()));
	pak.add("meta/report", ASSET_META, std::vector<u8>(result.report.begin(), result.report.end()));
	return result;
}

} // namespace g3
