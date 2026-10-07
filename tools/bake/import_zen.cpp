// ZenGin importer: reads the world mesh, its lightmaps, the static world
// objects and all textures straight from a Gothic installation through
// ZenKit. Nothing from the game is redistributed; the user bakes their own copy.
#include "importers.h"

#include "ground.h"

#include <zenkit/Mesh.hh>
#include <zenkit/MultiResolutionMesh.hh>
#include <zenkit/Stream.hh>
#include <zenkit/Texture.hh>
#include <zenkit/Vfs.hh>
#include <zenkit/World.hh>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
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

// "Foo.tga" + "-C.TEX" -> "FOO-C.TEX"; "Tree.3ds" + ".MRM" -> "TREE.MRM".
std::string compiledName(const std::string& name, const char* suffix) {
	std::string base = upper(name);
	const size_t dot = base.rfind('.');
	if (dot != std::string::npos) base.resize(dot);
	return base + suffix;
}

bool loadZenTexture(const zenkit::Vfs& vfs, const std::string& name, SrcImage& out) {
	const zenkit::VfsNode* node = vfs.find(compiledName(name, "-C.TEX"));
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

// ---- lightmaps -----------------------------------------------------------

// ZenGin's zCLightMap stores an origin and two axes pre-divided by their
// squared length, so a world position maps to the lightmap with two dot
// products: u = (p - origin) . right, v = (p - origin) . up. In the file (and
// in ZenKit's LightMap::normals) the order is up, right.
struct LightmapSampler {
	struct Image {
		int w = 0, h = 0;
		std::vector<u8> rgba;
	};
	std::map<const zenkit::Texture*, Image> images;

	const Image& image(const zenkit::Texture* tex) {
		auto it = images.find(tex);
		if (it != images.end()) return it->second;
		Image img;
		if (tex) {
			try {
				img.rgba = tex->as_rgba8(0);
				img.w = int(tex->width());
				img.h = int(tex->height());
				if (img.rgba.size() != size_t(img.w) * img.h * 4) img = Image {};
			} catch (const std::exception&) {
				img = Image {};
			}
		}
		return images.emplace(tex, std::move(img)).first->second;
	}

	// Returns false if the lightmap has no usable image. `inside` tells
	// whether the position fell into the lightmap's rectangle.
	bool sample(const zenkit::LightMap& lm, const zenkit::Vec3& p, u8 rgb[3], bool& inside) {
		const Image& img = image(lm.image.get());
		if (img.w == 0) return false;
		const float qx = p.x - lm.origin.x, qy = p.y - lm.origin.y, qz = p.z - lm.origin.z;
		float u = qx * lm.normals[1].x + qy * lm.normals[1].y + qz * lm.normals[1].z;
		float v = qx * lm.normals[0].x + qy * lm.normals[0].y + qz * lm.normals[0].z;
		inside = u > -0.05f && u < 1.05f && v > -0.05f && v < 1.05f;
		u = std::clamp(u, 0.0f, 1.0f) * float(img.w) - 0.5f;
		v = std::clamp(v, 0.0f, 1.0f) * float(img.h) - 0.5f;
		const int x0 = std::clamp(int(std::floor(u)), 0, img.w - 1), x1 = std::min(x0 + 1, img.w - 1);
		const int y0 = std::clamp(int(std::floor(v)), 0, img.h - 1), y1 = std::min(y0 + 1, img.h - 1);
		const float fx = std::clamp(u - float(x0), 0.0f, 1.0f), fy = std::clamp(v - float(y0), 0.0f, 1.0f);
		for (int c = 0; c < 3; ++c) {
			auto at = [&](int x, int y) { return float(img.rgba[(size_t(y) * img.w + x) * 4 + size_t(c)]); };
			const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
			const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
			rgb[c] = u8(std::clamp(top + (bottom - top) * fy + 0.5f, 0.0f, 255.0f));
		}
		return true;
	}
};

// ---- world mesh ----------------------------------------------------------

void appendMesh(const zenkit::Mesh& mesh, const ZenImportOptions& opt, SrcScene& scene) {
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
		sm.noCollision = m.disable_collision;
		scene.materials.push_back(sm);
	}

	const size_t triCount = mi.size();
	if (vi.size() != triCount * 3 || fi.size() != triCount * 3)
		throw std::runtime_error("world mesh: inconsistent polygon lists");

	const size_t firstTriangle = scene.triangles.size();
	u64 lightSum = 0;
	scene.triangles.reserve(firstTriangle + triCount);
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
			v.rgba[3] = 255; // outdoors until a lightmap says otherwise
			lightSum += u32(v.rgba[0]) + v.rgba[1] + v.rgba[2];
		}
		scene.triangles.push_back(tri);
	}

	// Lightmapped polygons: sample the lightmap at each corner and use that as
	// the vertex light. Detail inside large polygons is lost, but rooms and
	// caves get their real brightness and colour.
	u64 lightmapped = 0, corners = 0, inside = 0, applied = 0;
	std::vector<std::array<u8, 3>> sampled(triCount * 3);
	std::vector<char> hasSample(triCount, 0);
	LightmapSampler sampler;
	for (size_t t = 0; t < triCount; ++t) {
		if (t >= li.size() || li[t] < 0) continue;
		++lightmapped;
		if (!opt.lightmaps || size_t(li[t]) >= mesh.lightmaps.size()) continue;
		bool ok = true;
		for (int k = 0; k < 3 && ok; ++k) {
			bool in = false;
			ok = sampler.sample(mesh.lightmaps[size_t(li[t])], mesh.vertices.at(vi[t * 3 + size_t(k)]),
			                    sampled[t * 3 + size_t(k)].data(), in);
			if (ok) {
				++corners;
				inside += in;
			}
		}
		hasSample[t] = ok;
	}
	// If the mapping formula were wrong, corners would land all over the place.
	const bool trusted = corners > 0 && inside * 100 >= corners * 90;
	if (trusted) {
		for (size_t t = 0; t < triCount; ++t) {
			if (!hasSample[t]) continue;
			++applied;
			for (int k = 0; k < 3; ++k) {
				SrcVertex& v = scene.triangles[firstTriangle + t].v[k];
				std::copy_n(sampled[t * 3 + size_t(k)].data(), 3, v.rgba);
				v.rgba[3] = 0; // lit by its lightmap, not by the sky
			}
		}
	}
	if (opt.flipWinding) {
		for (size_t t = 0; t < triCount; ++t) std::swap(scene.triangles[firstTriangle + t].v[1], scene.triangles[firstTriangle + t].v[2]);
	}

	scene.notes.push_back("world mesh: " + std::to_string(triCount) + " triangles, " +
	                      std::to_string(mesh.materials.size()) + " materials, " + std::to_string(mesh.lightmaps.size()) +
	                      " lightmaps");
	if (triCount) {
		scene.notes.push_back("average baked vertex light: " + std::to_string(lightSum / (triCount * 9)) + " / 255");
	}
	if (lightmapped) {
		std::string note = "lightmaps: " + std::to_string(lightmapped) + " triangles, ";
		if (!opt.lightmaps)
			note += "disabled, vertex light used";
		else if (trusted)
			note += std::to_string(applied) + " lit from their lightmap (" + std::to_string(inside * 100 / corners) +
			        "% of the corners inside the lightmap rectangle)";
		else
			note += "NOT applied: only " + std::to_string(corners ? inside * 100 / corners : 0) +
			        "% of the corners map into their lightmap, vertex light used instead";
		scene.notes.push_back(note);
	}
}

// ---- world objects (VObs) ------------------------------------------------

using MrmPtr = std::shared_ptr<zenkit::MultiResolutionMesh>;

struct ObjectInstance {
	const zenkit::VirtualObject* vob;
	MrmPtr mesh;
};

struct ObjectStats {
	u32 placed = 0, noCollision = 0, otherVisuals = 0;
	u64 triangles = 0;
	std::set<std::string> missingMeshes;
	std::map<std::string, std::string> unreadableMeshes; // name -> reason
};

void collectObjects(const std::vector<std::shared_ptr<zenkit::VirtualObject>>& vobs, const zenkit::Vfs& vfs,
                    std::map<std::string, MrmPtr>& cache, std::vector<ObjectInstance>& out, ObjectStats& stats) {
	for (const auto& vob : vobs) {
		if (!vob) continue;
		collectObjects(vob->children, vfs, cache, out, stats);

		// The level component's "visual" is the world mesh itself; items and
		// NPCs get their looks from the scripts.
		if (vob->type == zenkit::VirtualObjectType::zCVobLevelCompo || vob->type == zenkit::VirtualObjectType::oCItem ||
		    vob->type == zenkit::VirtualObjectType::oCNpc)
			continue;
		if (!vob->show_visual || !vob->visual || vob->visual->name.empty()) continue;
		const std::string name = upper(vob->visual->name);
		if (name.size() < 4 || name.compare(name.size() - 4, 4, ".3DS") != 0) {
			++stats.otherVisuals; // animated models, particle effects, decals: later stages
			continue;
		}

		auto it = cache.find(name);
		if (it == cache.end()) {
			MrmPtr mesh;
			if (const zenkit::VfsNode* node = vfs.find(compiledName(name, ".MRM"))) {
				try {
					auto rd = node->open_read();
					mesh = std::make_shared<zenkit::MultiResolutionMesh>();
					mesh->load(rd.get());
				} catch (const std::exception& ex) {
					mesh.reset();
					stats.unreadableMeshes[name] = ex.what();
				}
			} else {
				stats.missingMeshes.insert(name);
			}
			it = cache.emplace(name, mesh).first;
		}
		if (!it->second) continue;
		out.push_back({vob.get(), it->second});
	}
}

// > 0 if the mesh's front faces (counter-clockwise, see scene.h) point outwards,
// < 0 if it is inside out, near 0 if the mesh is not a closed shape.
double outwardScore(const zenkit::MultiResolutionMesh& mesh) {
	if (mesh.positions.empty()) return 0;
	double cx = 0, cy = 0, cz = 0;
	float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
	for (const zenkit::Vec3& p : mesh.positions) {
		cx += p.x;
		cy += p.y;
		cz += p.z;
		const float c[3] = {p.x, p.y, p.z};
		for (int a = 0; a < 3; ++a) {
			mn[a] = std::min(mn[a], c[a]);
			mx[a] = std::max(mx[a], c[a]);
		}
	}
	const double n = double(mesh.positions.size());
	cx /= n;
	cy /= n;
	cz /= n;
	const double extent = std::sqrt(double(mx[0] - mn[0]) * (mx[0] - mn[0]) + double(mx[1] - mn[1]) * (mx[1] - mn[1]) +
	                                double(mx[2] - mn[2]) * (mx[2] - mn[2]));

	double flux = 0, area = 0;
	for (const zenkit::SubMesh& sub : mesh.sub_meshes) {
		for (const zenkit::MeshTriangle& tri : sub.triangles) {
			const zenkit::Vec3* p[3];
			bool ok = true;
			for (int k = 0; k < 3 && ok; ++k) {
				ok = tri.wedges[k] < sub.wedges.size() && sub.wedges[tri.wedges[k]].index < mesh.positions.size();
				if (ok) p[k] = &mesh.positions[sub.wedges[tri.wedges[k]].index];
			}
			if (!ok) continue;
			const double ax = p[1]->x - p[0]->x, ay = p[1]->y - p[0]->y, az = p[1]->z - p[0]->z;
			const double bx = p[2]->x - p[0]->x, by = p[2]->y - p[0]->y, bz = p[2]->z - p[0]->z;
			// Front normal (times twice the area) in a left-handed, CCW-front world.
			const double nx = -(ay * bz - az * by), ny = -(az * bx - ax * bz), nz = -(ax * by - ay * bx);
			flux += (p[0]->x - cx) * nx + (p[0]->y - cy) * ny + (p[0]->z - cz) * nz;
			area += std::sqrt(nx * nx + ny * ny + nz * nz);
		}
	}
	return area > 0 && extent > 0 ? flux / (area * extent) : 0;
}

void appendObjects(const zenkit::World& world, const zenkit::Vfs& vfs, SrcScene& scene) {
	std::map<std::string, MrmPtr> cache;
	std::vector<ObjectInstance> instances;
	ObjectStats stats;
	collectObjects(world.world_vobs, vfs, cache, instances, stats);

	// All object meshes come from the same exporter, so decide the winding once
	// for the whole set: closed shapes must have their faces pointing outwards.
	double vote = 0;
	for (const auto& kv : cache)
		if (kv.second) vote += outwardScore(*kv.second);
	const bool flip = vote < 0;

	GroundSampler ground;
	ground.build(scene, scene.triangles.size());
	u64 lightSum[3] = {0, 0, 0}, lightCount = 0;
	for (const SrcTriangle& t : scene.triangles)
		for (const SrcVertex& v : t.v) {
			for (int c = 0; c < 3; ++c) lightSum[c] += v.rgba[c];
			++lightCount;
		}
	u8 fallback[4] = {128, 128, 128, 255};
	if (lightCount)
		for (int c = 0; c < 3; ++c) fallback[c] = u8(lightSum[c] / lightCount);

	std::map<std::string, u32> materialIndex; // texture + collision flag -> scene material
	u32 unlit = 0;
	for (const ObjectInstance& inst : instances) {
		const zenkit::VirtualObject& vob = *inst.vob;
		const zenkit::Vec3* col = vob.rotation.columns; // object -> world: p' = R * p + position

		// Like ZenGin: a static object takes the light of the floor under it.
		u8 light[4];
		if (!ground.sample(vob.position.x, vob.position.y + 100.0f, vob.position.z, light) &&
		    !ground.sample(vob.position.x, vob.position.y + 1500.0f, vob.position.z, light)) {
			std::copy_n(fallback, 4, light);
			++unlit;
		}

		const bool mover = vob.type == zenkit::VirtualObjectType::zCMover;
		const bool solid = vob.cd_dynamic && !mover;
		if (!solid) ++stats.noCollision;

		for (const zenkit::SubMesh& sub : inst.mesh->sub_meshes) {
			const bool noCollision = !solid || sub.mat.disable_collision;
			const std::string key = upper(sub.mat.texture) + (noCollision ? "|N" : "|C");
			auto it = materialIndex.find(key);
			if (it == materialIndex.end()) {
				SrcMaterial sm;
				sm.name = sub.mat.name;
				sm.texture = sub.mat.texture;
				sm.group = u16(sub.mat.group);
				sm.noCollision = noCollision;
				it = materialIndex.emplace(key, u32(scene.materials.size())).first;
				scene.materials.push_back(sm);
			}

			for (const zenkit::MeshTriangle& src : sub.triangles) {
				SrcTriangle tri;
				tri.material = it->second;
				bool ok = true;
				for (int k = 0; k < 3 && ok; ++k) {
					ok = src.wedges[k] < sub.wedges.size() &&
					     sub.wedges[src.wedges[k]].index < inst.mesh->positions.size();
					if (!ok) break;
					const zenkit::MeshWedge& w = sub.wedges[src.wedges[k]];
					const zenkit::Vec3& p = inst.mesh->positions[w.index];
					SrcVertex& v = tri.v[k];
					v.pos[0] = col[0].x * p.x + col[1].x * p.y + col[2].x * p.z + vob.position.x;
					v.pos[1] = col[0].y * p.x + col[1].y * p.y + col[2].y * p.z + vob.position.y;
					v.pos[2] = col[0].z * p.x + col[1].z * p.y + col[2].z * p.z + vob.position.z;
					v.uv[0] = w.texture.x;
					v.uv[1] = w.texture.y;
					// A little top-down shading so shapes read; ZenGin lights objects flat.
					const float ny = col[0].y * w.normal.x + col[1].y * w.normal.y + col[2].y * w.normal.z;
					const float shade = 0.82f + 0.18f * std::clamp(ny, -1.0f, 1.0f);
					for (int c = 0; c < 3; ++c) v.rgba[c] = u8(std::clamp(float(light[c]) * shade, 0.0f, 255.0f));
					v.rgba[3] = light[3];
				}
				if (!ok) continue;
				if (flip) std::swap(tri.v[1], tri.v[2]);
				scene.triangles.push_back(tri);
				++stats.triangles;
			}
		}
		++stats.placed;
	}

	size_t loaded = 0;
	for (const auto& kv : cache) loaded += kv.second != nullptr;
	scene.notes.push_back("objects: " + std::to_string(stats.placed) + " placed from " + std::to_string(loaded) +
	                      " meshes, " + std::to_string(stats.triangles) + " triangles, " +
	                      std::to_string(stats.noCollision) + " without collision; winding " +
	                      (flip ? "flipped" : "kept") + " (score " + std::to_string(vote) + ")");
	if (stats.otherVisuals)
		scene.notes.push_back("objects: " + std::to_string(stats.otherVisuals) +
		                      " with animated models, particles or decals skipped (not supported yet)");
	if (unlit) scene.notes.push_back("objects: " + std::to_string(unlit) + " have no ground below them, lit with the world average");
	for (const auto& kv : stats.unreadableMeshes)
		scene.notes.push_back("WARNING: object mesh " + kv.first + " could not be read: " + kv.second);
	if (!stats.missingMeshes.empty()) {
		std::string list;
		size_t n = 0;
		for (const std::string& m : stats.missingMeshes) {
			if (n++ == 8) {
				list += ", ...";
				break;
			}
			list += (n > 1 ? ", " : "") + m;
		}
		scene.notes.push_back("WARNING: " + std::to_string(stats.missingMeshes.size()) + " object meshes not found: " + list);
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
		appendMesh(mesh, opt, scene);
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
		appendMesh(world.world_mesh, opt, scene);
		if (opt.objects) appendObjects(world, *vfs, scene);

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
