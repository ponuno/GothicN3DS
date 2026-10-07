// ZenGin importer: reads the world mesh, its lightmaps, the world objects
// (meshes, models in their rest pose, decals), portals and all textures
// straight from a Gothic installation through ZenKit. Nothing from the game
// is redistributed; the user bakes their own copy.
#include "importers.h"

#include "g3ds/world.h"
#include "ground.h"

#include <zenkit/Mesh.hh>
#include <zenkit/Model.hh>
#include <zenkit/ModelHierarchy.hh>
#include <zenkit/ModelMesh.hh>
#include <zenkit/ModelScript.hh>
#include <zenkit/MorphMesh.hh>
#include <zenkit/MultiResolutionMesh.hh>
#include <zenkit/Stream.hh>
#include <zenkit/Texture.hh>
#include <zenkit/Vfs.hh>
#include <zenkit/World.hh>
#include <zenkit/vobs/Trigger.hh>
#include <zenkit/vobs/Zone.hh>
#include <zenkit/world/WayNet.hh>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
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

// ---- materials -----------------------------------------------------------

// Gothic 1 has no per-material transparency setting for water; this is what
// its water roughly looks like. A material colour with its own alpha wins.
const u8 kWaterOpacity = 150;

void describeMaterial(const zenkit::Material& m, SrcMaterial& sm) {
	sm.name = m.name;
	sm.texture = m.texture;
	sm.group = u16(m.group);
	sm.noCollision = m.disable_collision;
	sm.water = m.group == zenkit::MaterialGroup::WATER;
	const bool ownAlpha = m.color.a > 0 && m.color.a < 255;
	if (sm.water) {
		sm.opacity = ownAlpha ? m.color.a : kWaterOpacity;
	} else if (m.alpha_func == zenkit::AlphaFunction::BLEND) {
		sm.blend = SrcBlend::Alpha;
		sm.opacity = ownAlpha ? m.color.a : 255;
	} else if (m.alpha_func == zenkit::AlphaFunction::ADD) {
		sm.blend = SrcBlend::Add;
	}
	sm.animFps = m.texture_anim_fps;
	if (m.texture_anim_map_mode != zenkit::AnimationMapping::NONE) {
		// ZenGin stores the scroll speed in texture repeats per millisecond.
		sm.scroll[0] = m.texture_anim_map_dir.x * 1000.0f;
		sm.scroll[1] = m.texture_anim_map_dir.y * 1000.0f;
	}
}

// Everything that makes two materials draw differently.
std::string materialKey(const SrcMaterial& m) {
	char buf[96];
	std::snprintf(buf, sizeof(buf), "|%d%d%d|%u|%.3f|%.4f|%.4f", int(m.water), int(m.noCollision), int(m.blend),
	              unsigned(m.opacity), double(m.animFps), double(m.scroll[0]), double(m.scroll[1]));
	return upper(m.texture) + buf;
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
		describeMaterial(m, sm);
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

// ---- object visuals ------------------------------------------------------
//
// Every kind of visual a world object can have is flattened to triangles in
// object space: plain meshes (.3DS -> .MRM), models in their rest pose
// (.MDS/.ASC -> .MDH + .MDM, or .MDL), morph meshes (.MMS -> .MMB) and decals.

struct M34 { // rotation/scale in the 3x3 part, translation in column 3
	float m[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};

	void point(const float in[3], float out[3]) const {
		for (int r = 0; r < 3; ++r) out[r] = m[r][0] * in[0] + m[r][1] * in[1] + m[r][2] * in[2] + m[r][3];
	}
	void direction(const float in[3], float out[3]) const {
		for (int r = 0; r < 3; ++r) out[r] = m[r][0] * in[0] + m[r][1] * in[1] + m[r][2] * in[2];
	}
};

M34 mul(const M34& a, const M34& b) {
	M34 o;
	for (int r = 0; r < 3; ++r) {
		for (int c = 0; c < 4; ++c) o.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] + a.m[r][2] * b.m[2][c];
		o.m[r][3] += a.m[r][3];
	}
	return o;
}

M34 fromZen(const zenkit::Mat4& z) {
	M34 o;
	for (int c = 0; c < 4; ++c) {
		o.m[0][c] = z.columns[c].x;
		o.m[1][c] = z.columns[c].y;
		o.m[2][c] = z.columns[c].z;
	}
	return o;
}

struct FlatVertex {
	float pos[3];
	float normal[3];
	float uv[2];
};

struct FlatSub {
	SrcMaterial material;
	std::vector<FlatVertex> verts; // three per triangle
};

enum class VisualKind { Mesh, Model, Morph, Decal, COUNT };

struct FlatMesh {
	VisualKind kind = VisualKind::Mesh;
	std::vector<FlatSub> subs;
	float radius = 0; // half the diagonal of the bounding box
	size_t triangles = 0;
	// Models only: skinned parts, and how far their rest pose is from the
	// vertex positions stored with the mesh (a self-check of the pose maths).
	u32 skinnedParts = 0;
	float skinDeviation = 0;

	void finish() {
		float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
		triangles = 0;
		for (const FlatSub& s : subs) {
			triangles += s.verts.size() / 3;
			for (const FlatVertex& v : s.verts)
				for (int a = 0; a < 3; ++a) {
					mn[a] = std::min(mn[a], v.pos[a]);
					mx[a] = std::max(mx[a], v.pos[a]);
				}
		}
		radius = triangles ? 0.5f * std::sqrt((mx[0] - mn[0]) * (mx[0] - mn[0]) + (mx[1] - mn[1]) * (mx[1] - mn[1]) +
		                                      (mx[2] - mn[2]) * (mx[2] - mn[2]))
		                   : 0.0f;
	}
};

using FlatPtr = std::shared_ptr<FlatMesh>;

// Appends a ZenGin mesh. `skinned`, if given, replaces the mesh's own vertex
// positions (soft-skin meshes store theirs relative to the bones).
void appendMrm(FlatMesh& out, const zenkit::MultiResolutionMesh& mrm, const M34& xf,
               const std::vector<std::array<float, 3>>* skinned = nullptr) {
	for (const zenkit::SubMesh& sub : mrm.sub_meshes) {
		FlatSub fs;
		describeMaterial(sub.mat, fs.material);
		fs.verts.reserve(sub.triangles.size() * 3);
		for (const zenkit::MeshTriangle& tri : sub.triangles) {
			FlatVertex v[3];
			bool ok = true;
			for (int k = 0; k < 3 && ok; ++k) {
				ok = tri.wedges[k] < sub.wedges.size();
				if (!ok) break;
				const zenkit::MeshWedge& w = sub.wedges[tri.wedges[k]];
				float p[3];
				if (skinned) {
					ok = w.index < skinned->size();
					if (ok) std::copy_n((*skinned)[w.index].data(), 3, p);
				} else {
					ok = w.index < mrm.positions.size();
					if (ok) {
						p[0] = mrm.positions[w.index].x;
						p[1] = mrm.positions[w.index].y;
						p[2] = mrm.positions[w.index].z;
					}
				}
				if (!ok) break;
				const float n[3] = {w.normal.x, w.normal.y, w.normal.z};
				xf.point(p, v[k].pos);
				xf.direction(n, v[k].normal);
				v[k].uv[0] = w.texture.x;
				v[k].uv[1] = w.texture.y;
			}
			if (ok) fs.verts.insert(fs.verts.end(), v, v + 3);
		}
		if (!fs.verts.empty()) out.subs.push_back(std::move(fs));
	}
}

// Rest pose of a model: every node's transform in model space. Like ZenGin
// does for world objects, the root node is moved to the object's origin.
// `base` receives the shift that puts the root there.
std::vector<M34> restPose(const zenkit::ModelHierarchy& h, float base[3]) {
	const size_t n = h.nodes.size();
	std::vector<M34> local(n), world(n);
	std::vector<char> done(n, 0);
	base[0] = base[1] = base[2] = 0;
	bool haveRoot = false;
	for (size_t i = 0; i < n; ++i) {
		local[i] = fromZen(h.nodes[i].transform);
		if (h.nodes[i].parent_index >= 0 && size_t(h.nodes[i].parent_index) < n) continue;
		local[i].m[0][3] += h.root_translation.x;
		local[i].m[1][3] += h.root_translation.y;
		local[i].m[2][3] += h.root_translation.z;
		if (!haveRoot) {
			for (int a = 0; a < 3; ++a) base[a] = -local[i].m[a][3];
			haveRoot = true;
		}
	}
	// Parents are not guaranteed to come first.
	for (size_t pass = 0; pass < n; ++pass) {
		bool progress = false;
		for (size_t i = 0; i < n; ++i) {
			if (done[i]) continue;
			const int parent = h.nodes[i].parent_index;
			if (parent < 0 || size_t(parent) >= n) {
				world[i] = local[i];
				for (int a = 0; a < 3; ++a) world[i].m[a][3] += base[a];
			} else if (done[size_t(parent)]) {
				world[i] = mul(world[size_t(parent)], local[i]);
			} else {
				continue;
			}
			done[i] = 1;
			progress = true;
		}
		if (!progress) break; // a cycle in the file: the remaining nodes keep the identity
	}
	return world;
}

void appendModel(FlatMesh& out, const zenkit::ModelHierarchy* hierarchy, const zenkit::ModelMesh& mesh) {
	std::vector<M34> pose;
	float base[3] = {0, 0, 0};
	if (hierarchy) pose = restPose(*hierarchy, base);

	// Rigid parts hang on the node of the same name.
	for (const auto& kv : mesh.attachments) {
		M34 xf;
		if (hierarchy)
			for (size_t i = 0; i < hierarchy->nodes.size(); ++i)
				if (upper(hierarchy->nodes[i].name) == upper(kv.first)) {
					xf = pose[i];
					break;
				}
		appendMrm(out, kv.second, xf);
	}
	// Skinned parts: each vertex is a weighted sum of bone-local positions.
	for (const zenkit::SoftSkinMesh& skin : mesh.meshes) {
		std::vector<std::array<float, 3>> positions(skin.mesh.positions.size());
		for (size_t i = 0; i < positions.size(); ++i) {
			float sum[3] = {0, 0, 0}, total = 0;
			if (i < skin.weights.size())
				for (const zenkit::SoftSkinWeightEntry& w : skin.weights[i]) {
					if (w.node_index >= pose.size()) continue;
					const float lp[3] = {w.position.x, w.position.y, w.position.z};
					float wp[3];
					pose[w.node_index].point(lp, wp);
					for (int a = 0; a < 3; ++a) sum[a] += wp[a] * w.weight;
					total += w.weight;
				}
			const float stored[3] = {skin.mesh.positions[i].x + base[0], skin.mesh.positions[i].y + base[1],
			                         skin.mesh.positions[i].z + base[2]};
			if (total > 1e-6f) {
				float d2 = 0;
				for (int a = 0; a < 3; ++a) {
					positions[i][size_t(a)] = sum[a] / total;
					d2 += (positions[i][size_t(a)] - stored[a]) * (positions[i][size_t(a)] - stored[a]);
				}
				out.skinDeviation = std::max(out.skinDeviation, std::sqrt(d2));
			} else {
				positions[i] = {stored[0], stored[1], stored[2]};
			}
		}
		++out.skinnedParts;
		appendMrm(out, skin.mesh, M34 {}, &positions);
	}
}

std::string stripExtension(std::string name) {
	const size_t dot = name.rfind('.');
	if (dot != std::string::npos) name.resize(dot);
	return name;
}

std::string extensionOf(const std::string& name) {
	const size_t dot = name.rfind('.');
	return dot == std::string::npos ? std::string() : upper(name.substr(dot + 1));
}

template <class T> bool loadFile(const zenkit::Vfs& vfs, const std::string& name, T& out) {
	const zenkit::VfsNode* node = vfs.find(name);
	if (!node) return false;
	auto rd = node->open_read();
	out.load(rd.get());
	return true;
}

// Returns null with `why` set if the visual cannot be loaded; an empty `why`
// means its files do not exist.
FlatPtr loadVisual(const zenkit::Vfs& vfs, const std::string& name, std::string& why) {
	const std::string ext = extensionOf(name);
	const std::string base = stripExtension(upper(name));
	auto flat = std::make_shared<FlatMesh>();
	try {
		if (ext == "3DS") {
			zenkit::MultiResolutionMesh mrm;
			if (!loadFile(vfs, base + ".MRM", mrm)) return nullptr;
			flat->kind = VisualKind::Mesh;
			appendMrm(*flat, mrm, M34 {});
		} else if (ext == "MMS" || ext == "MMB") {
			zenkit::MorphMesh morph;
			if (!loadFile(vfs, base + ".MMB", morph)) return nullptr;
			flat->kind = VisualKind::Morph;
			appendMrm(*flat, morph.mesh, M34 {});
		} else if (ext == "MDS" || ext == "ASC" || ext == "MDL" || ext == "MDM") {
			flat->kind = VisualKind::Model;
			// A model script names its skeleton and, mostly, takes the mesh from
			// the same file; some register their meshes separately instead.
			std::string treeBase = base, meshBase = base;
			if (ext == "MDS") {
				zenkit::ModelScript script;
				bool have = false;
				try {
					have = loadFile(vfs, base + ".MSB", script) || loadFile(vfs, base + ".MDS", script);
				} catch (const std::exception&) {
					have = false; // unreadable script: fall back to the visual's own name
				}
				if (have && !script.skeleton.name.empty()) {
					treeBase = meshBase = stripExtension(upper(script.skeleton.name));
					if (script.skeleton.disable_mesh && !script.meshes.empty()) meshBase = stripExtension(upper(script.meshes[0]));
				}
			}
			zenkit::ModelHierarchy hierarchy;
			zenkit::ModelMesh mesh;
			zenkit::Model model;
			if (loadFile(vfs, meshBase + ".MDM", mesh)) {
				const bool haveHierarchy = loadFile(vfs, treeBase + ".MDH", hierarchy);
				appendModel(*flat, haveHierarchy ? &hierarchy : nullptr, mesh);
			} else if (loadFile(vfs, treeBase + ".MDL", model)) {
				appendModel(*flat, &model.hierarchy, model.mesh);
			} else {
				return nullptr;
			}
		} else {
			return nullptr;
		}
	} catch (const std::exception& ex) {
		why = ex.what();
		return nullptr;
	}
	flat->finish();
	if (flat->triangles == 0) {
		why = "no triangles";
		return nullptr;
	}
	return flat;
}

// A decal is a flat textured rectangle in the object's XY plane.
FlatPtr makeDecal(const zenkit::VisualDecal& decal, zenkit::GameVersion version) {
	auto flat = std::make_shared<FlatMesh>();
	flat->kind = VisualKind::Decal;
	FlatSub sub;
	sub.material.name = decal.name;
	sub.material.texture = decal.name;
	sub.material.noCollision = true;
	sub.material.animFps = decal.texture_anim_fps;
	if (decal.alpha_func == zenkit::AlphaFunction::BLEND) {
		sub.material.blend = SrcBlend::Alpha;
		if (version == zenkit::GameVersion::GOTHIC_2 && decal.alpha_weight > 0) sub.material.opacity = decal.alpha_weight;
	} else if (decal.alpha_func == zenkit::AlphaFunction::ADD) {
		sub.material.blend = SrcBlend::Add;
	}
	const float sx = decal.dimension.x, sy = decal.dimension.y;
	const float corner[4][5] = {{-sx, -sy, 0, 0, 1}, {sx, -sy, 0, 1, 1}, {sx, sy, 0, 1, 0}, {-sx, sy, 0, 0, 0}};
	auto put = [&](int c, float nz) {
		FlatVertex v {};
		std::copy_n(corner[c], 3, v.pos);
		v.normal[2] = nz;
		v.uv[0] = corner[c][3];
		v.uv[1] = corner[c][4];
		sub.verts.push_back(v);
	};
	// The front looks down -Z. The blended pass draws both sides of every
	// triangle, so a blended decal needs one face only (two would blend twice);
	// an opaque one gets both faces, whatever its two_sided flag says, so that a
	// wrong guess about its front cannot make it disappear.
	for (int c : {0, 1, 2, 0, 2, 3}) put(c, -1.0f);
	if (sub.material.blend == SrcBlend::Opaque)
		for (int c : {0, 2, 1, 0, 3, 2}) put(c, 1.0f);
	flat->subs.push_back(std::move(sub));
	flat->finish();
	return flat;
}

// > 0 if the mesh's front faces (counter-clockwise, see scene.h) point outwards,
// < 0 if it is inside out, near 0 if the mesh is not a closed shape.
double outwardScore(const FlatMesh& mesh) {
	double c[3] = {0, 0, 0}, count = 0;
	for (const FlatSub& s : mesh.subs)
		for (const FlatVertex& v : s.verts) {
			for (int a = 0; a < 3; ++a) c[a] += v.pos[a];
			++count;
		}
	if (count == 0 || mesh.radius <= 0) return 0;
	for (double& a : c) a /= count;

	double flux = 0, area = 0;
	for (const FlatSub& s : mesh.subs) {
		for (size_t t = 0; t + 2 < s.verts.size(); t += 3) {
			const float* p0 = s.verts[t].pos;
			const float* p1 = s.verts[t + 1].pos;
			const float* p2 = s.verts[t + 2].pos;
			const double ax = p1[0] - p0[0], ay = p1[1] - p0[1], az = p1[2] - p0[2];
			const double bx = p2[0] - p0[0], by = p2[1] - p0[1], bz = p2[2] - p0[2];
			// Front normal (times twice the area) in a left-handed, CCW-front world.
			const double nx = -(ay * bz - az * by), ny = -(az * bx - ax * bz), nz = -(ax * by - ay * bx);
			flux += (p0[0] - c[0]) * nx + (p0[1] - c[1]) * ny + (p0[2] - c[2]) * nz;
			area += std::sqrt(nx * nx + ny * ny + nz * nz);
		}
	}
	return area > 0 ? flux / (area * 2.0 * double(mesh.radius)) : 0;
}

// ---- world objects (VObs) ------------------------------------------------

struct ObjectInstance {
	const zenkit::VirtualObject* vob;
	FlatPtr mesh;
};

struct ObjectStats {
	u32 placed[size_t(VisualKind::COUNT)] = {};
	u32 noCollision = 0, particles = 0, sprites = 0, unsupported = 0;
	u64 triangles = 0;
	std::set<std::string> missing;
	std::map<std::string, std::string> unreadable; // name -> reason
};

// Objects smaller than this (bounding radius, cm) are only drawn near the camera.
const float kSmallRadius = 160.0f, kMediumRadius = 450.0f;

void collectObjects(const std::vector<std::shared_ptr<zenkit::VirtualObject>>& vobs, const zenkit::Vfs& vfs,
                    zenkit::GameVersion version, std::map<std::string, FlatPtr>& cache, std::vector<ObjectInstance>& out,
                    ObjectStats& stats, SrcScene& scene) {
	for (const auto& vob : vobs) {
		if (!vob) continue;
		collectObjects(vob->children, vfs, version, cache, out, stats, scene);

		// Named objects double as arrival points for portals; way points win.
		if (!vob->vob_name.empty()) {
			const zenkit::Vec3& fwd = vob->rotation.columns[2];
			const SrcPoint point {{vob->position.x, vob->position.y, vob->position.z}, std::atan2(fwd.x, fwd.z)};
			const bool marker = vob->type == zenkit::VirtualObjectType::zCVobStartpoint ||
			                    vob->type == zenkit::VirtualObjectType::zCVobSpot;
			if (marker)
				scene.points[upper(vob->vob_name)] = point;
			else
				scene.points.emplace(upper(vob->vob_name), point);
		}
		if (vob->type == zenkit::VirtualObjectType::oCMobLadder) {
			SrcLadder ladder;
			const zenkit::Vec3 mn = vob->bbox.min, mx = vob->bbox.max;
			const float lo[3] = {mn.x, mn.y, mn.z}, hi[3] = {mx.x, mx.y, mx.z};
			for (int a = 0; a < 3; ++a) {
				ladder.bbMin[a] = std::min(lo[a], hi[a]);
				ladder.bbMax[a] = std::max(lo[a], hi[a]);
			}
			// A box without height is a file without bounds, not a ladder.
			if (ladder.bbMax[1] - ladder.bbMin[1] > 50.0f) scene.ladders.push_back(ladder);
		}
		if (vob->type == zenkit::VirtualObjectType::zCZoneZFogDefault) {
			if (const auto* fog = dynamic_cast<const zenkit::VZoneFog*>(vob.get())) {
				scene.fogColor[0] = fog->color.r;
				scene.fogColor[1] = fog->color.g;
				scene.fogColor[2] = fog->color.b;
			}
		}
		if (vob->type == zenkit::VirtualObjectType::oCTriggerChangeLevel) {
			if (const auto* trigger = dynamic_cast<const zenkit::VTriggerChangeLevel*>(vob.get())) {
				SrcPortal portal;
				const zenkit::Vec3 mn = vob->bbox.min, mx = vob->bbox.max;
				const float lo[3] = {mn.x, mn.y, mn.z}, hi[3] = {mx.x, mx.y, mx.z};
				for (int a = 0; a < 3; ++a) {
					portal.bbMin[a] = std::min(lo[a], hi[a]);
					portal.bbMax[a] = std::max(lo[a], hi[a]);
				}
				portal.level = trigger->level_name;
				portal.point = trigger->start_vob;
				if (!portal.level.empty()) scene.portals.push_back(portal);
			}
		}

		// The level component's "visual" is the world mesh itself; items and
		// NPCs get their looks from the scripts.
		if (vob->type == zenkit::VirtualObjectType::zCVobLevelCompo || vob->type == zenkit::VirtualObjectType::oCItem ||
		    vob->type == zenkit::VirtualObjectType::oCNpc)
			continue;
		if (!vob->show_visual || !vob->visual || vob->visual->name.empty()) continue;
		const std::string name = upper(vob->visual->name);
		const std::string ext = extensionOf(name);

		if (ext == "PFX") {
			++stats.particles;
			continue;
		}
		if (ext == "TGA") {
			const auto* decal = dynamic_cast<const zenkit::VisualDecal*>(vob->visual.get());
			if (!decal || vob->sprite_camera_facing_mode != zenkit::SpriteAlignment::NONE) {
				++stats.sprites; // always turned towards the camera: needs the runtime
				continue;
			}
			if (decal->alpha_func == zenkit::AlphaFunction::MULTIPLY ||
			    decal->alpha_func == zenkit::AlphaFunction::MULTIPLY_ALT ||
			    decal->alpha_func == zenkit::AlphaFunction::SUBTRACT || decal->dimension.x <= 0 || decal->dimension.y <= 0) {
				++stats.unsupported;
				continue;
			}
			out.push_back({vob.get(), makeDecal(*decal, version)});
			continue;
		}

		auto it = cache.find(name);
		if (it == cache.end()) {
			std::string why;
			FlatPtr mesh = loadVisual(vfs, name, why);
			if (!mesh) {
				if (ext != "3DS" && ext != "MDS" && ext != "ASC" && ext != "MDL" && ext != "MDM" && ext != "MMS" &&
				    ext != "MMB")
					++stats.unsupported;
				else if (why.empty())
					stats.missing.insert(name);
				else
					stats.unreadable[name] = why;
			}
			it = cache.emplace(name, mesh).first;
		} else if (!it->second && stats.missing.count(name) == 0 && stats.unreadable.count(name) == 0) {
			++stats.unsupported;
		}
		if (!it->second) continue;
		out.push_back({vob.get(), it->second});
	}
}

void appendObjects(const zenkit::World& world, const zenkit::Vfs& vfs, zenkit::GameVersion version, SrcScene& scene) {
	std::map<std::string, FlatPtr> cache;
	std::vector<ObjectInstance> instances;
	ObjectStats stats;
	collectObjects(world.world_vobs, vfs, version, cache, instances, stats, scene);

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

	std::map<std::string, u32> materialIndex; // everything that affects drawing -> scene material
	u32 unlit = 0;
	u64 detailCount[DETAIL_COUNT] = {0, 0, 0};
	for (const ObjectInstance& inst : instances) {
		const zenkit::VirtualObject& vob = *inst.vob;
		const zenkit::Vec3* col = vob.rotation.columns; // object -> world: p' = R * p + position
		const bool decal = inst.mesh->kind == VisualKind::Decal;

		// Like ZenGin: a static object takes the light of the floor under it.
		u8 light[4];
		if (!ground.sample(vob.position.x, vob.position.y + 100.0f, vob.position.z, light) &&
		    !ground.sample(vob.position.x, vob.position.y + 1500.0f, vob.position.z, light)) {
			std::copy_n(fallback, 4, light);
			++unlit;
		}

		// Doors and movers will open one day; until then nothing may block the way.
		const bool passable = vob.type == zenkit::VirtualObjectType::zCMover ||
		                      vob.type == zenkit::VirtualObjectType::oCMobDoor;
		const bool solid = vob.cd_dynamic && !passable && !decal;
		if (!solid) ++stats.noCollision;

		const u8 detail = inst.mesh->radius < kSmallRadius    ? u8(DETAIL_SMALL)
		                  : inst.mesh->radius < kMediumRadius ? u8(DETAIL_MEDIUM)
		                                                      : u8(DETAIL_ALWAYS);
		++detailCount[detail];

		for (const FlatSub& sub : inst.mesh->subs) {
			SrcMaterial sm = sub.material;
			sm.noCollision = sm.noCollision || !solid;
			const std::string key = materialKey(sm);
			auto it = materialIndex.find(key);
			if (it == materialIndex.end()) {
				it = materialIndex.emplace(key, u32(scene.materials.size())).first;
				scene.materials.push_back(sm);
			}
			// Glow and light shafts are light themselves; everything else is lit.
			const bool selfLit = sm.blend == SrcBlend::Add;

			for (size_t t = 0; t + 2 < sub.verts.size(); t += 3) {
				SrcTriangle tri;
				tri.material = it->second;
				tri.detail = detail;
				for (int k = 0; k < 3; ++k) {
					const FlatVertex& src = sub.verts[t + size_t(k)];
					const float* p = src.pos;
					SrcVertex& v = tri.v[k];
					v.pos[0] = col[0].x * p[0] + col[1].x * p[1] + col[2].x * p[2] + vob.position.x;
					v.pos[1] = col[0].y * p[0] + col[1].y * p[1] + col[2].y * p[2] + vob.position.y;
					v.pos[2] = col[0].z * p[0] + col[1].z * p[1] + col[2].z * p[2] + vob.position.z;
					v.uv[0] = src.uv[0];
					v.uv[1] = src.uv[1];
					if (selfLit) {
						v.rgba[0] = v.rgba[1] = v.rgba[2] = 255;
						v.rgba[3] = 0;
						continue;
					}
					// A little top-down shading so shapes read; ZenGin lights objects flat.
					const float ny = col[0].y * src.normal[0] + col[1].y * src.normal[1] + col[2].y * src.normal[2];
					const float shade = decal ? 1.0f : 0.82f + 0.18f * std::clamp(ny, -1.0f, 1.0f);
					for (int c = 0; c < 3; ++c) v.rgba[c] = u8(std::clamp(float(light[c]) * shade, 0.0f, 255.0f));
					v.rgba[3] = light[3];
				}
				if (flip && !decal) std::swap(tri.v[1], tri.v[2]);
				scene.triangles.push_back(tri);
				++stats.triangles;
			}
		}
		++stats.placed[size_t(inst.mesh->kind)];
	}

	size_t loaded = 0;
	u32 skinnedParts = 0;
	float skinDeviation = 0;
	std::string skinWorst;
	for (const auto& kv : cache) {
		if (!kv.second) continue;
		++loaded;
		skinnedParts += kv.second->skinnedParts;
		if (kv.second->skinDeviation > skinDeviation) {
			skinDeviation = kv.second->skinDeviation;
			skinWorst = kv.first;
		}
	}
	const u32 total = stats.placed[0] + stats.placed[1] + stats.placed[2] + stats.placed[3];
	scene.notes.push_back("objects: " + std::to_string(total) + " placed from " + std::to_string(loaded) + " meshes, " +
	                      std::to_string(stats.triangles) + " triangles, " + std::to_string(stats.noCollision) +
	                      " without collision; winding " + (flip ? "flipped" : "kept") + " (score " +
	                      std::to_string(vote) + ")");
	scene.notes.push_back("objects by kind: " + std::to_string(stats.placed[size_t(VisualKind::Mesh)]) + " meshes, " +
	                      std::to_string(stats.placed[size_t(VisualKind::Model)]) + " models in rest pose, " +
	                      std::to_string(stats.placed[size_t(VisualKind::Morph)]) + " morph meshes, " +
	                      std::to_string(stats.placed[size_t(VisualKind::Decal)]) + " decals");
	if (skinnedParts) {
		char buf[160];
		std::snprintf(buf, sizeof(buf), "models: %u skinned parts; rest pose within %.1f cm of the stored mesh%s%s", skinnedParts,
		              double(skinDeviation), skinWorst.empty() ? "" : ", worst ", skinWorst.c_str());
		scene.notes.push_back(buf);
	}
	scene.notes.push_back("objects by size: " + std::to_string(detailCount[DETAIL_ALWAYS]) + " large, " +
	                      std::to_string(detailCount[DETAIL_MEDIUM]) + " medium, " + std::to_string(detailCount[DETAIL_SMALL]) +
	                      " small");
	if (stats.particles || stats.sprites || stats.unsupported)
		scene.notes.push_back("objects skipped (not supported yet): " + std::to_string(stats.particles) +
		                      " particle effects, " + std::to_string(stats.sprites) + " camera-facing sprites, " +
		                      std::to_string(stats.unsupported) + " other");
	if (unlit) scene.notes.push_back("objects: " + std::to_string(unlit) + " have no ground below them, lit with the world average");
	for (const auto& kv : stats.unreadable)
		scene.notes.push_back("WARNING: object visual " + kv.first + " could not be read: " + kv.second);
	if (!stats.missing.empty()) {
		std::string list;
		size_t n = 0;
		for (const std::string& m : stats.missing) {
			if (n++ == 8) {
				list += ", ...";
				break;
			}
			list += (n > 1 ? ", " : "") + m;
		}
		scene.notes.push_back("WARNING: " + std::to_string(stats.missing.size()) + " object visuals not found: " + list);
	}
}

void describeWater(SrcScene& scene) {
	u32 count = 0, lo = 255, hi = 0;
	for (const SrcMaterial& m : scene.materials) {
		if (!m.water) continue;
		++count;
		lo = std::min<u32>(lo, m.opacity);
		hi = std::max<u32>(hi, m.opacity);
	}
	if (count)
		scene.notes.push_back("water: " + std::to_string(count) + " materials, opacity " + std::to_string(lo) + ".." +
		                      std::to_string(hi) + " of 255");
}

} // namespace

struct ZenGame {
	zenkit::Vfs vfs;
};

std::shared_ptr<ZenGame> openZenGame(const std::string& gameDir, std::vector<std::string>& notes) {
	fs::path dataDir;
	if (!fs::is_directory(gameDir)) throw std::runtime_error("'" + gameDir + "' is not a directory");
	for (const auto& e : fs::directory_iterator(gameDir)) {
		if (e.is_directory() && lower(e.path().filename().string()) == "data") dataDir = e.path();
	}
	if (dataDir.empty()) throw std::runtime_error("no Data directory in '" + gameDir + "'; is this a Gothic install?");

	std::vector<fs::path> disks;
	for (const auto& e : fs::directory_iterator(dataDir)) {
		if (e.is_regular_file() && lower(e.path().extension().string()) == ".vdf") disks.push_back(e.path());
	}
	std::sort(disks.begin(), disks.end());
	if (disks.empty()) throw std::runtime_error("no .vdf archives in '" + dataDir.string() + "'");
	auto game = std::make_shared<ZenGame>();
	for (const fs::path& d : disks) {
		try {
			game->vfs.mount_disk(d);
		} catch (const std::exception& ex) {
			notes.push_back("skipped " + d.filename().string() + ": " + ex.what());
		}
	}
	return game;
}

bool zenGameHasFile(const ZenGame& game, const std::string& name) { return game.vfs.find(name) != nullptr; }

std::function<bool(const std::string&, SrcImage&)> textureLoader(std::shared_ptr<const ZenGame> game) {
	return [game](const std::string& name, SrcImage& out) {
		try {
			return loadZenTexture(game->vfs, name, out);
		} catch (const std::exception&) {
			return false; // unreadable texture: treated like a missing one
		}
	};
}

void importZenWorld(std::shared_ptr<const ZenGame> game, const ZenImportOptions& opt, SrcScene& scene) {
	const zenkit::VfsNode* node = game->vfs.find(opt.zenName);
	if (!node) throw std::runtime_error("'" + opt.zenName + "' not found in the mounted archives");
	auto rd = node->open_read();
	zenkit::World world;
	const zenkit::GameVersion version = world.load(rd.get());
	scene.notes.push_back(std::string("source: ") + opt.zenName + " (" +
	                      (version == zenkit::GameVersion::GOTHIC_1 ? "Gothic 1" : "Gothic 2") + ")");
	appendMesh(world.world_mesh, opt, scene);
	scene.indoor = world.world_bsp_tree.mode == zenkit::BspTreeType::INDOOR;

	// Way points first: named objects of the same name take precedence.
	if (world.way_net)
		for (const auto& wp : world.way_net->points) {
			if (!wp || wp->name.empty()) continue;
			scene.points[upper(wp->name)] =
			    SrcPoint {{wp->position.x, wp->position.y, wp->position.z}, std::atan2(wp->direction.x, wp->direction.z)};
		}
	if (opt.objects) {
		appendObjects(world, game->vfs, version, scene);
	} else {
		// Portals and arrival points are still wanted.
		std::map<std::string, FlatPtr> cache;
		std::vector<ObjectInstance> instances;
		ObjectStats stats;
		zenkit::Vfs empty;
		collectObjects(world.world_vobs, empty, version, cache, instances, stats, scene);
	}
	describeWater(scene);

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
	scene.loadTexture = textureLoader(game);
}

void importZenMesh(const ZenImportOptions& opt, SrcScene& scene) {
	// Development path: a loose mesh plus a directory of compiled textures.
	auto game = std::make_shared<ZenGame>();
	if (!opt.textureDir.empty()) game->vfs.mount_host(opt.textureDir, "/");
	auto rd = zenkit::Read::from(fs::path(opt.meshFile));
	zenkit::Mesh mesh;
	mesh.load(rd.get(), false);
	appendMesh(mesh, opt, scene);
	describeWater(scene);
	scene.loadTexture = textureLoader(game);
}

} // namespace g3
