// g3ds-bake: turns a scene (a Gothic installation or the built-in synthetic
// test scene) into data.pak for the 3DS runtime.
#include "chunker.h"
#include "g3ds/tex.h"
#include "importers.h"
#include "pak_writer.h"
#include "texture_bake.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

using namespace g3;

namespace {

struct BakeOptions {
	std::string out = "data.pak";
	std::string worldName = "world";
	ChunkerOptions chunker;
	TexBakeOptions tex;
	unsigned threads = std::max(1u, std::thread::hardware_concurrency());
};

std::string lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
	return s;
}

// "Foo_Bar.TGA" -> "tex/foo_bar"
std::string texturePakName(const std::string& texture) {
	std::string base = lower(texture);
	const size_t slash = base.find_last_of("/\\");
	if (slash != std::string::npos) base = base.substr(slash + 1);
	const size_t dot = base.rfind('.');
	if (dot != std::string::npos) base.resize(dot);
	return "tex/" + base;
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

struct BakedTexture {
	std::string pakName;
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

void bake(SrcScene& scene, const BakeOptions& opt) {
	const auto t0 = std::chrono::steady_clock::now();
	std::string report;
	auto line = [&report](const std::string& s) {
		report += s + "\n";
		std::puts(s.c_str());
	};
	for (const std::string& n : scene.notes) line(n);

	// ---- which materials and textures are actually referenced ----
	std::vector<char> used(scene.materials.size(), 0);
	for (const SrcTriangle& t : scene.triangles) {
		if (t.material >= scene.materials.size()) throw std::runtime_error("triangle references an unknown material");
		used[t.material] = 1;
	}
	std::map<std::string, std::string> textureSource; // pak name -> source texture name
	for (size_t i = 0; i < scene.materials.size(); ++i) {
		if (used[i] && !scene.materials[i].texture.empty())
			textureSource.emplace(texturePakName(scene.materials[i].texture), scene.materials[i].texture);
	}

	// ---- textures (ETC1 encoding dominates the bake time, so fan it out) ----
	std::vector<BakedTexture> textures;
	for (const auto& kv : textureSource) {
		BakedTexture bt;
		bt.pakName = kv.first;
		textures.push_back(std::move(bt));
	}
	{
		std::atomic<size_t> next {0};
		std::mutex loadLock;
		std::string firstError;
		auto worker = [&]() {
			for (;;) {
				const size_t i = next.fetch_add(1);
				if (i >= textures.size()) return;
				try {
					SrcImage img;
					bool found;
					{
						std::lock_guard<std::mutex> g(loadLock);
						found = scene.loadTexture && scene.loadTexture(textureSource[textures[i].pakName], img);
					}
					if (!found) continue;
					textures[i].data = bakeTexture(img, opt.tex, &textures[i].info);
					textures[i].ok = true;
				} catch (const std::exception& ex) {
					std::lock_guard<std::mutex> g(loadLock);
					if (firstError.empty()) firstError = ex.what();
				}
			}
		};
		std::vector<std::thread> pool;
		for (unsigned i = 0; i < std::min<size_t>(opt.threads, std::max<size_t>(textures.size(), 1)); ++i)
			pool.emplace_back(worker);
		for (std::thread& t : pool) t.join();
		if (!firstError.empty()) throw std::runtime_error("texture bake failed: " + firstError);
	}

	PakWriter pak;
	std::map<std::string, const BakedTexture*> textureByName;
	std::vector<std::string> missing;
	u64 texels = 0, srcTexels = 0;
	u32 alphaTextures = 0;
	for (const BakedTexture& bt : textures) {
		if (!bt.ok) {
			missing.push_back(textureSource[bt.pakName]);
			continue;
		}
		textureByName[bt.pakName] = &bt;
		pak.add(bt.pakName, ASSET_TEXTURE, bt.data);
		texels += u64(bt.info.width) * bt.info.height;
		srcTexels += u64(bt.info.srcWidth) * bt.info.srcHeight;
		alphaTextures += bt.info.alpha;
	}
	const std::string fallbackName = "tex/_missing";
	pak.add(fallbackName, ASSET_TEXTURE, bakeTexture(missingTexture(), opt.tex));

	// ---- world materials: one per distinct (texture, flags) ----
	std::vector<WorldMaterial> worldMaterials;
	std::map<std::pair<u32, u16>, int> materialIndex;
	std::vector<int> remap(scene.materials.size(), -1);
	for (size_t i = 0; i < scene.materials.size(); ++i) {
		if (!used[i]) continue;
		const SrcMaterial& sm = scene.materials[i];
		WorldMaterial wm {};
		wm.group = sm.group;
		auto it = sm.texture.empty() ? textureByName.end() : textureByName.find(texturePakName(sm.texture));
		if (it != textureByName.end()) {
			wm.textureHash = hashName(it->first.c_str());
			if (it->second->info.alpha) wm.flags |= MATF_ALPHA_TEST;
		} else {
			wm.textureHash = hashName(fallbackName.c_str());
			wm.flags |= MATF_NO_TEXTURE;
		}
		if (sm.water) wm.flags |= MATF_WATER | MATF_NO_COLLISION;
		if (sm.noCollision) wm.flags |= MATF_NO_COLLISION;
		auto ins = materialIndex.emplace(std::make_pair(wm.textureHash, wm.flags), int(worldMaterials.size()));
		if (ins.second) worldMaterials.push_back(wm);
		remap[i] = ins.first->second;
	}
	if (worldMaterials.size() > 65535) throw std::runtime_error("too many materials");

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
	ChunkerStats cs;
	std::vector<BakedChunk> chunks = buildChunks(scene, remap, opt.chunker, cs);
	if (chunks.empty()) throw std::runtime_error("the scene has no drawable triangles");

	WorldHeader wh {};
	wh.magic = WORLD_MAGIC;
	wh.version = WORLD_VERSION;
	wh.chunkSize = opt.chunker.chunkSize;
	wh.chunkCount = u32(chunks.size());
	wh.materialCount = u32(worldMaterials.size());
	wh.triangleCount = u32(cs.outputTriangles);
	for (int a = 0; a < 3; ++a) {
		wh.bbMin[a] = 1e30f;
		wh.bbMax[a] = -1e30f;
	}

	std::vector<ChunkInfo> infos;
	int gx0 = 1 << 30, gx1 = -(1 << 30), gz0 = 1 << 30, gz1 = -(1 << 30);
	for (size_t i = 0; i < chunks.size(); ++i) {
		const BakedChunk& c = chunks[i];
		const std::string name = "world/" + opt.worldName + "/" + std::to_string(i);
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
	if (scene.hasSpawn) {
		std::memcpy(wh.spawnPos, scene.spawnPos, sizeof(wh.spawnPos));
		wh.spawnYaw = scene.spawnYaw;
	} else {
		wh.spawnPos[0] = (wh.bbMin[0] + wh.bbMax[0]) * 0.5f;
		wh.spawnPos[1] = wh.bbMax[1] + 300.0f;
		wh.spawnPos[2] = (wh.bbMin[2] + wh.bbMax[2]) * 0.5f;
	}

	std::vector<u8> index(sizeof(wh) + worldMaterials.size() * sizeof(WorldMaterial) + infos.size() * sizeof(ChunkInfo));
	std::memcpy(index.data(), &wh, sizeof(wh));
	std::memcpy(index.data() + sizeof(wh), worldMaterials.data(), worldMaterials.size() * sizeof(WorldMaterial));
	std::memcpy(index.data() + sizeof(wh) + worldMaterials.size() * sizeof(WorldMaterial), infos.data(),
	            infos.size() * sizeof(ChunkInfo));
	pak.add("world/" + opt.worldName, ASSET_WORLD, index);

	// ---- report ----
	const double MB = 1024.0 * 1024.0;
	line(format("world '%s': %.0f x %.0f x %.0f m, grid %d x %d cells of %.0f m", opt.worldName.c_str(),
	            (wh.bbMax[0] - wh.bbMin[0]) / 100.0, (wh.bbMax[1] - wh.bbMin[1]) / 100.0,
	            (wh.bbMax[2] - wh.bbMin[2]) / 100.0, gx1 - gx0 + 1, gz1 - gz0 + 1, wh.chunkSize / 100.0));
	line(format("geometry: %llu triangles in, %llu out (%llu degenerate dropped), %llu vertices (%.2f per triangle)",
	            static_cast<unsigned long long>(cs.inputTriangles), static_cast<unsigned long long>(cs.outputTriangles),
	            static_cast<unsigned long long>(cs.degenerate), static_cast<unsigned long long>(cs.vertices),
	            cs.outputTriangles ? double(cs.vertices) / double(cs.outputTriangles) : 0.0));
	line(format("vertex sharing: %llu distinct positions, %llu with UVs, %llu with UVs and light",
	            static_cast<unsigned long long>(cs.uniquePositions), static_cast<unsigned long long>(cs.uniquePosUv),
	            static_cast<unsigned long long>(cs.vertices)));
	line(format("winding check: %.0f%% of the horizontal front-facing area looks up%s", upShare * 100.0,
	            upShare >= 0.5 ? "" : " - WARNING: the world looks inside out, bake again with --flip"));
	line(format("chunks: %zu (%u cells split), largest %u triangles / %u vertices / %u batches, position step <= %.2f cm",
	            chunks.size(), cs.splitChunks, cs.maxChunkTriangles, cs.maxChunkVertices, cs.maxChunkBatches,
	            double(cs.maxPosError) * 2.0));
	line(format("materials: %zu world materials from %zu source materials", worldMaterials.size(),
	            size_t(std::count(used.begin(), used.end(), 1))));
	line(format("textures: %zu baked (%u cut-out), max edge %d, %.1f%% of the source texels kept, %zu missing",
	            textureByName.size(), alphaTextures, opt.tex.maxSize,
	            srcTexels ? 100.0 * double(texels) / double(srcTexels) : 0.0, missing.size()));
	line(format("size: chunks %.2f MB (%.2f MB packed), textures %.2f MB (%.2f MB packed)",
	            double(pak.rawBytes(ASSET_CHUNK)) / MB, double(pak.packedBytes(ASSET_CHUNK)) / MB,
	            double(pak.rawBytes(ASSET_TEXTURE)) / MB, double(pak.packedBytes(ASSET_TEXTURE)) / MB));
	if (cs.uvRebased)
		line(format("note: %llu triangles needed a private UV origin (wide texture spans)",
		            static_cast<unsigned long long>(cs.uvRebased)));
	if (cs.uvClamped)
		line(format("WARNING: %llu triangles span more than %.0f texture repeats and were clamped",
		            static_cast<unsigned long long>(cs.uvClamped), double(32767.0f * WORLD_UV_SCALE)));
	if (!missing.empty()) {
		std::string list;
		for (size_t i = 0; i < missing.size() && i < 12; ++i) list += (i ? ", " : "") + missing[i];
		if (missing.size() > 12) list += ", ...";
		line("WARNING: missing textures (drawn with the checker fallback): " + list);
	}

	pak.add("meta/report", ASSET_META, std::vector<u8>(report.begin(), report.end()));
	pak.write(opt.out);

	const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	std::printf("wrote %s: %zu entries in %.1f s\n", opt.out.c_str(), pak.items().size(), secs);
}

int info(const char* path) {
	Pak pak;
	if (!pak.open(path)) {
		std::fprintf(stderr, "error: cannot open '%s' as a G3PK archive\n", path);
		return 1;
	}
	static const char* typeNames[] = {"blob", "texture", "world", "chunk", "meta"};
	u64 packed[5] = {}, raw[5] = {};
	u32 counts[5] = {};
	for (u32 i = 0; i < pak.count(); ++i) {
		const PakEntry& e = pak.entry(i);
		const u32 t = e.type < 5 ? e.type : 0;
		++counts[t];
		packed[t] += e.packedSize;
		raw[t] += e.size;
	}
	std::printf("%s: %u entries\n", path, pak.count());
	for (int t = 0; t < 5; ++t) {
		if (counts[t])
			std::printf("  %-8s %5u  %8.2f MB raw  %8.2f MB packed\n", typeNames[t], counts[t], double(raw[t]) / 1048576.0,
			            double(packed[t]) / 1048576.0);
	}
	if (const PakEntry* e = pak.find("meta/report")) {
		std::vector<char> text(e->size + 1, 0);
		if (pak.read(*e, text.data())) std::printf("\n%s", text.data());
	}
	return 0;
}

void usage() {
	std::puts("g3ds-bake - asset baker for the Gothic 3DS runtime\n"
	          "\n"
	          "  g3ds-bake world --game DIR [--zen WORLD.ZEN] [options]   bake a world from a Gothic install\n"
	          "  g3ds-bake mesh --msh FILE [--textures DIR] [options]     bake a loose .MSH (development)\n"
	          "  g3ds-bake synth [--cells N] [options]                    bake the built-in test scene\n"
	          "  g3ds-bake info FILE                                      describe a data.pak\n"
	          "\n"
	          "options:\n"
	          "  -o FILE            output archive (default: data.pak)\n"
	          "  --name NAME        world asset name (default: world)\n"
	          "  --chunk CM         chunk edge length (default: 4000)\n"
	          "  --max-tex N        longest texture edge, power of two, 8..1024 (default: 128)\n"
	          "  --quality 0|1|2    ETC1 effort (default: 1)\n"
	          "  --flip             reverse the triangle winding (if the winding check complains)\n"
	          "  --no-objects       world mesh only, skip trees, bridges and other static objects\n"
	          "  --no-lightmaps     keep the vertex light on lightmapped (indoor) polygons\n"
	          "  --threads N        worker threads");
}

} // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		usage();
		return 2;
	}
	const std::string cmd = argv[1];
	try {
		if (cmd == "info") {
			if (argc != 3) {
				usage();
				return 2;
			}
			return info(argv[2]);
		}
		if (cmd != "world" && cmd != "mesh" && cmd != "synth") {
			usage();
			return 2;
		}

		BakeOptions opt;
		ZenImportOptions zen;
		SynthOptions synth;
		for (int i = 2; i < argc; ++i) {
			const std::string a = argv[i];
			auto value = [&]() -> std::string {
				if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
				return argv[++i];
			};
			if (a == "-o") opt.out = value();
			else if (a == "--name") opt.worldName = value();
			else if (a == "--chunk") opt.chunker.chunkSize = std::stof(value());
			else if (a == "--max-tex") opt.tex.maxSize = std::stoi(value());
			else if (a == "--quality") opt.tex.quality = std::stoi(value());
			else if (a == "--threads") opt.threads = unsigned(std::max(1, std::stoi(value())));
			else if (a == "--game") zen.gameDir = value();
			else if (a == "--zen") zen.zenName = value();
			else if (a == "--msh") zen.meshFile = value();
			else if (a == "--textures") zen.textureDir = value();
			else if (a == "--no-objects") zen.objects = false;
			else if (a == "--no-lightmaps") zen.lightmaps = false;
			else if (a == "--flip") zen.flipWinding = true;
			else if (a == "--no-flip") zen.flipWinding = false; // the default; kept for old command lines
			else if (a == "--cells") synth.cells = std::stoi(value());
			else throw std::runtime_error("unknown option " + a);
		}
		const int mt = opt.tex.maxSize;
		if (mt < int(TEX_MIN_SIZE) || mt > int(TEX_MAX_SIZE) || (mt & (mt - 1)))
			throw std::runtime_error("--max-tex must be a power of two between 8 and 1024");
		if (opt.chunker.chunkSize < 500.0f) throw std::runtime_error("--chunk must be at least 500");
		if (synth.cells < 8 || synth.cells > 1024) throw std::runtime_error("--cells must be between 8 and 1024");

		SrcScene scene;
		if (cmd == "synth") {
			buildSyntheticScene(synth, scene);
		} else {
			if (cmd == "world" && zen.gameDir.empty()) throw std::runtime_error("world: --game DIR is required");
			if (cmd == "mesh" && zen.meshFile.empty()) throw std::runtime_error("mesh: --msh FILE is required");
			if (cmd == "world") zen.meshFile.clear();
			importZenWorld(zen, scene);
		}
		bake(scene, opt);
		return 0;
	} catch (const std::exception& ex) {
		std::fprintf(stderr, "error: %s\n", ex.what());
		return 1;
	}
}
