// ZenGin importer: reads the world mesh and its textures straight from a
// Gothic installation through ZenKit. Nothing from the game is redistributed;
// the user bakes their own copy.
#include "importers.h"

#include <zenkit/Mesh.hh>
#include <zenkit/Stream.hh>
#include <zenkit/Texture.hh>
#include <zenkit/Vfs.hh>
#include <zenkit/World.hh>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <stdexcept>

namespace fs = std::filesystem;

namespace g3 {

namespace {

std::string lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
	return s;
}

std::string upper(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::toupper(c)); });
	return s;
}

// "Foo.tga" -> "FOO-C.TEX", the name of the compiled texture inside the VDFs.
std::string compiledTextureName(const std::string& name) {
	std::string base = upper(name);
	const size_t dot = base.rfind('.');
	if (dot != std::string::npos) base.resize(dot);
	return base + "-C.TEX";
}

bool loadZenTexture(const zenkit::Vfs& vfs, const std::string& name, SrcImage& out) {
	const zenkit::VfsNode* node = vfs.find(compiledTextureName(name));
	if (!node) return false;
	auto rd = node->open_read();
	zenkit::Texture tex;
	tex.load(rd.get());
	out.width = int(tex.width());
	out.height = int(tex.height());
	out.rgba = tex.as_rgba8(0);
	return out.width > 0 && out.height > 0 && out.rgba.size() == size_t(out.width) * out.height * 4;
}

const zenkit::VirtualObject* findStartpoint(const std::vector<std::shared_ptr<zenkit::VirtualObject>>& vobs) {
	for (const auto& v : vobs) {
		if (!v) continue;
		if (v->type == zenkit::VirtualObjectType::zCVobStartpoint) return v.get();
		if (const auto* c = findStartpoint(v->children)) return c;
	}
	return nullptr;
}

void appendMesh(const zenkit::Mesh& mesh, bool flipWinding, SrcScene& scene) {
	// World::load triangulates the BSP leaf polygons for us. A standalone .MSH
	// only has the raw polygon list, so unpack its fans with the same rules.
	std::vector<uint32_t> vi, fi, mi;
	std::vector<int32_t> li;
	if (!mesh.polygons.vertex_indices.empty()) {
		vi = mesh.polygons.vertex_indices;
		fi = mesh.polygons.feature_indices;
		mi = mesh.polygons.material_indices;
		li = mesh.polygons.lightmap_indices;
	} else {
		for (const zenkit::Polygon& p : mesh.geometry) {
			if (p.index_count < 3 || p.flags.is_portal || p.flags.is_ghost_occluder || p.flags.is_outdoor) continue;
			for (size_t k = 2; k < p.index_count; ++k) {
				for (size_t c : {size_t(0), k - 1, k}) {
					vi.push_back(mesh.polygon_vertex_indices.at(p.index_offset + c));
					fi.push_back(mesh.polygon_feature_indices.at(p.index_offset + c));
				}
				mi.push_back(p.material);
				li.push_back(p.lightmap);
			}
		}
	}

	const u32 materialBase = u32(scene.materials.size());
	for (const zenkit::Material& m : mesh.materials) {
		SrcMaterial sm;
		sm.name = m.name;
		sm.texture = m.texture;
		sm.group = u16(m.group);
		sm.water = m.group == zenkit::MaterialGroup::WATER;
		scene.materials.push_back(sm);
	}

	const size_t triCount = mi.size();
	if (vi.size() != triCount * 3 || fi.size() != triCount * 3)
		throw std::runtime_error("world mesh: inconsistent polygon lists");

	u64 lightmapped = 0, lightSum = 0;
	scene.triangles.reserve(scene.triangles.size() + triCount);
	for (size_t t = 0; t < triCount; ++t) {
		if (mi[t] >= mesh.materials.size()) throw std::runtime_error("world mesh: material index out of range");
		SrcTriangle tri;
		tri.material = materialBase + mi[t];
		for (int k = 0; k < 3; ++k) {
			const zenkit::Vec3& p = mesh.vertices.at(vi[t * 3 + size_t(k)]);
			const zenkit::VertexFeature& f = mesh.features.at(fi[t * 3 + size_t(k)]);
			SrcVertex& v = tri.v[k];
			v.pos[0] = p.x;
			v.pos[1] = p.y;
			v.pos[2] = p.z;
			v.uv[0] = f.texture.x;
			v.uv[1] = f.texture.y;
			// zCOLOR is stored as 0xAARRGGBB.
			v.rgba[0] = u8(f.light >> 16);
			v.rgba[1] = u8(f.light >> 8);
			v.rgba[2] = u8(f.light);
			v.rgba[3] = 255;
			lightSum += u32(v.rgba[0]) + v.rgba[1] + v.rgba[2];
		}
		if (flipWinding) std::swap(tri.v[1], tri.v[2]);
		if (t < li.size() && li[t] >= 0) ++lightmapped;
		scene.triangles.push_back(tri);
	}

	scene.notes.push_back("world mesh: " + std::to_string(triCount) + " triangles, " +
	                      std::to_string(mesh.materials.size()) + " materials, " + std::to_string(mesh.lightmaps.size()) +
	                      " lightmaps");
	if (triCount) {
		scene.notes.push_back("average baked vertex light: " + std::to_string(lightSum / (triCount * 9)) + " / 255");
	}
	if (lightmapped) {
		scene.notes.push_back(std::to_string(lightmapped) +
		                      " triangles use lightmaps; lightmaps are not baked yet, vertex light is used instead");
	}
}

} // namespace

void importZenWorld(const ZenImportOptions& opt, SrcScene& scene) {
	auto vfs = std::make_shared<zenkit::Vfs>();

	if (!opt.meshFile.empty()) {
		// Development path: a loose mesh plus a directory of compiled textures.
		if (!opt.textureDir.empty()) vfs->mount_host(opt.textureDir, "/");
		auto rd = zenkit::Read::from(fs::path(opt.meshFile));
		zenkit::Mesh mesh;
		mesh.load(rd.get(), false);
		appendMesh(mesh, opt.flipWinding, scene);
	} else {
		fs::path dataDir;
		if (!fs::is_directory(opt.gameDir)) throw std::runtime_error("'" + opt.gameDir + "' is not a directory");
		for (const auto& e : fs::directory_iterator(opt.gameDir)) {
			if (e.is_directory() && lower(e.path().filename().string()) == "data") dataDir = e.path();
		}
		if (dataDir.empty()) throw std::runtime_error("no Data directory in '" + opt.gameDir + "'; is this a Gothic install?");

		std::vector<fs::path> disks;
		for (const auto& e : fs::directory_iterator(dataDir)) {
			if (e.is_regular_file() && lower(e.path().extension().string()) == ".vdf") disks.push_back(e.path());
		}
		std::sort(disks.begin(), disks.end());
		if (disks.empty()) throw std::runtime_error("no .vdf archives in '" + dataDir.string() + "'");
		for (const fs::path& d : disks) {
			try {
				vfs->mount_disk(d);
			} catch (const std::exception& ex) {
				scene.notes.push_back("skipped " + d.filename().string() + ": " + ex.what());
			}
		}

		const zenkit::VfsNode* node = vfs->find(opt.zenName);
		if (!node) throw std::runtime_error("'" + opt.zenName + "' not found in the mounted archives");
		auto rd = node->open_read();
		zenkit::World world;
		const zenkit::GameVersion version = world.load(rd.get());
		scene.notes.push_back(std::string("source: ") + opt.zenName + " (" +
		                      (version == zenkit::GameVersion::GOTHIC_1 ? "Gothic 1" : "Gothic 2") + ")");
		appendMesh(world.world_mesh, opt.flipWinding, scene);

		if (const zenkit::VirtualObject* start = findStartpoint(world.world_vobs)) {
			scene.spawnPos[0] = start->position.x;
			scene.spawnPos[1] = start->position.y + 175.0f; // eye height above the start point
			scene.spawnPos[2] = start->position.z;
			const zenkit::Vec3& fwd = start->rotation.columns[2];
			scene.spawnYaw = std::atan2(fwd.x, fwd.z);
			scene.hasSpawn = true;
		} else {
			scene.notes.push_back("no zCVobStartpoint found; spawning at the centre of the map");
		}
	}

	scene.loadTexture = [vfs](const std::string& name, SrcImage& out) {
		try {
			return loadZenTexture(*vfs, name, out);
		} catch (const std::exception&) {
			return false; // unreadable texture: treated like a missing one
		}
	};
}

} // namespace g3
