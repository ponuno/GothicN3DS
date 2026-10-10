// ZenGin importer: reads the world mesh, its lightmaps, the world objects
// (meshes, models in their rest pose, decals), portals and all textures
// straight from a Gothic installation through ZenKit. Nothing from the game
// is redistributed; the user bakes their own copy.
#include "g3ds/model.h"
#include "importers.h"

#include "g3ds/world.h"
#include "ground.h"

#include <zenkit/DaedalusVm.hh>
#include <zenkit/Mesh.hh>
#include <zenkit/addon/daedalus.hh>
#include <zenkit/Model.hh>
#include <zenkit/ModelAnimation.hh>
#include <zenkit/ModelHierarchy.hh>
#include <zenkit/ModelMesh.hh>
#include <zenkit/ModelScript.hh>
#include <zenkit/MorphMesh.hh>
#include <zenkit/MultiResolutionMesh.hh>
#include <zenkit/Stream.hh>
#include <zenkit/Texture.hh>
#include <zenkit/Vfs.hh>
#include <zenkit/World.hh>
#include <zenkit/vobs/MovableObject.hh>
#include <zenkit/vobs/Trigger.hh>
#include <zenkit/vobs/Zone.hh>
#include <zenkit/world/WayNet.hh>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

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
	sm.color[0] = m.color.r;
	sm.color[1] = m.color.g;
	sm.color[2] = m.color.b;
	sm.hasColor = true;
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
	char buf[128];
	std::snprintf(buf, sizeof(buf), "|%d%d%d|%u|%.3f|%.4f|%.4f|%02x%02x%02x", int(m.water), int(m.noCollision),
	              int(m.blend), unsigned(m.opacity), double(m.animFps), double(m.scroll[0]), double(m.scroll[1]),
	              unsigned(m.color[0]), unsigned(m.color[1]), unsigned(m.color[2]));
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
	// For the report: the files it was read from, and (models) what it is made of.
	std::string source, parts;

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

	const auto describe = [&](const std::string& what, const zenkit::MultiResolutionMesh& mrm) {
		size_t triangles = 0;
		std::string textures;
		for (const zenkit::SubMesh& sub : mrm.sub_meshes) {
			triangles += sub.triangles.size();
			if (textures.find(upper(sub.mat.texture)) == std::string::npos) textures += (textures.empty() ? "" : " ") + upper(sub.mat.texture);
		}
		out.parts += (out.parts.empty() ? "" : "; ") + what + " " + std::to_string(triangles) + " tris " + textures;
	};
	// Rigid parts hang on the node of the same name.
	for (const auto& kv : mesh.attachments) {
		M34 xf;
		bool found = false;
		if (hierarchy)
			for (size_t i = 0; i < hierarchy->nodes.size(); ++i)
				if (upper(hierarchy->nodes[i].name) == upper(kv.first)) {
					xf = pose[i];
					found = true;
					break;
				}
		describe("node " + upper(kv.first) + (found || !hierarchy ? "" : " (not in the tree)"), kv.second);
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
		describe("skin", skin.mesh);
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
			// Where a model's files are. Its own name first; a model script
			// may name another file for the skeleton ("meshAndTree") and say
			// that the mesh in there is not this model's (DONT_USE_MESH) -
			// then the meshes it registers are, and the skeleton's own mesh
			// is only the last resort.
			struct Candidate {
				std::string mesh, tree, model; // an .MDM with an .MDH, or an .MDL
			};
			std::vector<Candidate> candidates;
			std::string scriptNote;
			// (A visual named as an .ASC is its own files first; its script, if
			// it has one, only where those are missing.)
			if (ext != "MDS") {
				candidates.push_back({base, base, ""});
				candidates.push_back({"", "", base});
			}
			if (ext == "MDS" || ext == "ASC") {
				zenkit::ModelScript script;
				bool have = false;
				try {
					have = loadFile(vfs, base + ".MSB", script) || loadFile(vfs, base + ".MDS", script);
				} catch (const std::exception&) {
					have = false; // unreadable script: fall back to the visual's own name
				}
				if (have && !script.skeleton.name.empty()) {
					const std::string skel = stripExtension(upper(script.skeleton.name));
					scriptNote = "script: tree " + skel + (script.skeleton.disable_mesh ? " without its mesh" : "");
					for (const std::string& m : script.meshes) scriptNote += ", registers " + stripExtension(upper(m));
					if (!script.skeleton.disable_mesh) {
						candidates.push_back({skel, skel, ""});
						candidates.push_back({"", "", skel});
					} else {
						// The skeleton is saved under the script's name, or under the
						// name of the file it was made from.
						const std::string tree = vfs.find(base + ".MDH") ? base : skel;
						for (const std::string& m : script.meshes) candidates.push_back({stripExtension(upper(m)), tree, ""});
						candidates.push_back({"", "", base});
						candidates.push_back({base, base, ""});
						candidates.push_back({skel, skel, ""}); // borrowed: better than nothing
						candidates.push_back({"", "", skel});
					}
				}
			}
			candidates.push_back({base, base, ""});
			candidates.push_back({"", "", base});
			bool loaded = false;
			for (const Candidate& c : candidates) {
				zenkit::ModelHierarchy hierarchy;
				zenkit::ModelMesh mesh;
				zenkit::Model model;
				if (!c.model.empty()) {
					if (!loadFile(vfs, c.model + ".MDL", model)) continue;
					appendModel(*flat, &model.hierarchy, model.mesh);
					flat->source = c.model + ".MDL";
				} else {
					if (!loadFile(vfs, c.mesh + ".MDM", mesh)) continue;
					// The skeleton on its own, or the one inside a whole model.
					bool haveHierarchy = loadFile(vfs, c.tree + ".MDH", hierarchy);
					std::string treeFile = c.tree + ".MDH";
					if (!haveHierarchy && loadFile(vfs, c.tree + ".MDL", model)) {
						hierarchy = model.hierarchy;
						haveHierarchy = true;
						treeFile = c.tree + ".MDL";
					}
					appendModel(*flat, haveHierarchy ? &hierarchy : nullptr, mesh);
					flat->source = c.mesh + ".MDM" + (haveHierarchy ? " + " + treeFile : " (no tree)");
				}
				loaded = true;
				break;
			}
			if (!loaded) return nullptr;
			if (!scriptNote.empty()) flat->source += " [" + scriptNote + "]";
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
	// Things that give light, for the report: fire places (oCMobFire) by
	// visual, and every visual whose name says lamp, torch or fire.
	struct Fire {
		u32 count = 0;
		std::string slot, tree;
	};
	std::map<std::string, Fire> fires;
	std::map<std::string, u32> lights;
};

bool namedLikeLight(const std::string& upperName) {
	for (const char* word : {"LAMP", "LANTERN", "LATERN", "TORCH", "FACKEL", "FIRE", "FEUER", "LIGHT", "CANDLE", "KERZE", "SCONCE"})
		if (upperName.find(word) != std::string::npos) return true;
	return false;
}

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
			// Spots are the free points of the people's routines.
			if (vob->type == zenkit::VirtualObjectType::zCVobSpot)
				scene.freePoints.push_back(SrcWayPoint {upper(vob->vob_name), {point.pos[0], point.pos[1], point.pos[2]}, point.yaw});
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
		if (vob->type == zenkit::VirtualObjectType::oCMobFire) {
			const auto* fire = dynamic_cast<const zenkit::VFire*>(vob.get());
			const std::string visual = vob->visual && !vob->visual->name.empty() ? upper(vob->visual->name) : std::string("(no visual)");
			ObjectStats::Fire& f = stats.fires[visual + (vob->show_visual ? "" : " (hidden)")];
			++f.count;
			if (fire) {
				f.slot = upper(fire->slot);
				f.tree = upper(fire->vob_tree);
			}
		}
		if (!vob->show_visual || !vob->visual || vob->visual->name.empty()) continue;
		const std::string name = upper(vob->visual->name);
		const std::string ext = extensionOf(name);
		if (namedLikeLight(name)) ++stats.lights[name];

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
	// Models that were not read from files of their own name, and everything
	// that gives light: what each was built from, to compare with the game.
	{
		std::string list;
		size_t n = 0;
		for (const auto& kv : cache) {
			if (!kv.second || kv.second->kind != VisualKind::Model) continue;
			const std::string base = stripExtension(kv.first);
			if (kv.second->source.compare(0, base.size() + 1, base + ".") == 0) continue;
			if (++n <= 24) list += (list.empty() ? "" : "; ") + kv.first + " <- " + kv.second->source;
		}
		if (n) scene.notes.push_back("models read from files of another name: " + std::to_string(n) + ": " + list + (n > 24 ? "; ..." : ""));
	}
	if (!stats.fires.empty()) {
		u32 fireCount = 0;
		for (const auto& kv : stats.fires) fireCount += kv.second.count;
		scene.notes.push_back("fire places: " + std::to_string(fireCount) + " objects of " + std::to_string(stats.fires.size()) +
		                      " kinds (their flames are not drawn yet):");
		size_t n = 0;
		for (const auto& kv : stats.fires) {
			if (++n > 24) {
				scene.notes.push_back("  ...");
				break;
			}
			const std::string visual = kv.first.substr(0, kv.first.find(" ("));
			const auto it = cache.find(visual);
			std::string made = it == cache.end() || !it->second ? "not drawn" : it->second->source + ": " + it->second->parts;
			if (made.size() > 420) made = made.substr(0, 420) + "...";
			scene.notes.push_back("  " + kv.first + " x" + std::to_string(kv.second.count) + ", fire " + (kv.second.tree.empty() ? "-" : kv.second.tree) +
			                      " at " + (kv.second.slot.empty() ? "-" : kv.second.slot) + " <- " + made);
		}
	}
	if (!stats.lights.empty()) {
		std::string list;
		size_t n = 0;
		for (const auto& kv : stats.lights) {
			if (stats.fires.count(kv.first)) continue; // listed above
			if (++n > 40) {
				list += "; ...";
				break;
			}
			const auto it = cache.find(kv.first);
			const bool drawn = it != cache.end() && it->second;
			list += (list.empty() ? "" : "; ") + kv.first + " x" + std::to_string(kv.second) +
			        (!drawn ? " (not drawn)" : it->second->source.empty() ? "" : " <- " + it->second->source);
		}
		if (n) scene.notes.push_back("other objects named like lights: " + list);
	}
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
	fs::path dir; // the installation: compiled scripts lie there as loose files
	// Which side of a character mesh's triangles is the front: +1 as stored,
	// -1 the other, 0 not known yet. All of a game's character meshes come out
	// of the same exporter, so what has been found for one whole body holds
	// for every armour and head (which, open as they are, say little alone).
	mutable int actorWinding = 0;
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
	game->dir = gameDir;
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

// ---- characters ----------------------------------------------------------

namespace {

// Standard quaternion (x y z w) of a rotation matrix; scale is divided out.
void quatFromRows(const M34& m, float q[4]) {
	float r[3][3];
	for (int c = 0; c < 3; ++c) {
		const float len = std::sqrt(m.m[0][c] * m.m[0][c] + m.m[1][c] * m.m[1][c] + m.m[2][c] * m.m[2][c]);
		for (int k = 0; k < 3; ++k) r[k][c] = len > 1e-9f ? m.m[k][c] / len : (k == c ? 1.0f : 0.0f);
	}
	const float tr = r[0][0] + r[1][1] + r[2][2];
	if (tr > 0) {
		const float s = std::sqrt(tr + 1.0f) * 2.0f;
		q[3] = 0.25f * s;
		q[0] = (r[2][1] - r[1][2]) / s;
		q[1] = (r[0][2] - r[2][0]) / s;
		q[2] = (r[1][0] - r[0][1]) / s;
	} else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
		const float s = std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2.0f;
		q[3] = (r[2][1] - r[1][2]) / s;
		q[0] = 0.25f * s;
		q[1] = (r[0][1] + r[1][0]) / s;
		q[2] = (r[0][2] + r[2][0]) / s;
	} else if (r[1][1] > r[2][2]) {
		const float s = std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2.0f;
		q[3] = (r[0][2] - r[2][0]) / s;
		q[0] = (r[0][1] + r[1][0]) / s;
		q[1] = 0.25f * s;
		q[2] = (r[1][2] + r[2][1]) / s;
	} else {
		const float s = std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2.0f;
		q[3] = (r[1][0] - r[0][1]) / s;
		q[0] = (r[0][2] + r[2][0]) / s;
		q[1] = (r[1][2] + r[2][1]) / s;
		q[2] = 0.25f * s;
	}
}

// Angle in degrees between two rotations given as quaternions.
float quatAngle(const float a[4], const float b[4]) {
	const float d = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
	return 2.0f * std::acos(std::min(d, 1.0f)) * 57.29578f;
}

// "HUM_BODY_NAKED_V0_C0.TGA" -> "HUM_BODY_NAKED_V4_C1.TGA": the game picks
// the skin of a character by counting up these two numbers in the name.
std::string variantTexture(const std::string& texture, int variant, int color) {
	std::string up = upper(texture), out;
	for (size_t i = 0; i < up.size(); ++i) {
		const bool digitEnds = i + 3 >= up.size() || !std::isdigit(static_cast<unsigned char>(up[i + 3]));
		if (digitEnds && up.compare(i, 3, "_V0") == 0) {
			out += "_V" + std::to_string(variant);
			i += 2;
		} else if (digitEnds && up.compare(i, 3, "_C0") == 0) {
			out += "_C" + std::to_string(color);
			i += 2;
		} else {
			out += up[i];
		}
	}
	return out;
}

// Rest transforms of every node in model space, without moving the root to
// the origin (a character's model space has the ground at y = 0).
std::vector<M34> restPoseStanding(const zenkit::ModelHierarchy& h, std::vector<M34>& local) {
	const size_t n = h.nodes.size();
	local.resize(n);
	std::vector<M34> world(n);
	std::vector<char> done(n, 0);
	for (size_t i = 0; i < n; ++i) {
		local[i] = fromZen(h.nodes[i].transform);
		const int parent = h.nodes[i].parent_index;
		if (parent >= 0 && size_t(parent) < n) continue;
		local[i].m[0][3] += h.root_translation.x;
		local[i].m[1][3] += h.root_translation.y;
		local[i].m[2][3] += h.root_translation.z;
	}
	for (size_t pass = 0; pass < n; ++pass)
		for (size_t i = 0; i < n; ++i) {
			if (done[i]) continue;
			const int parent = h.nodes[i].parent_index;
			if (parent < 0 || size_t(parent) >= n) {
				world[i] = local[i];
			} else if (done[size_t(parent)]) {
				world[i] = mul(world[size_t(parent)], local[i]);
			} else {
				continue;
			}
			done[i] = 1;
		}
	return world;
}

// Adds a ZenGin mesh to a character. `skin`, if given, makes it a soft-skin
// mesh (positions and bones per vertex index); otherwise it hangs rigidly on
// `node`, whose rest transform is `xf`.
struct SkinInfo {
	std::array<float, 3> pos;
	int bone[2];
	float weight1;
};
void appendActorMesh(SrcModel& model, const zenkit::MultiResolutionMesh& mrm, const M34& xf, int node,
                     const std::vector<SkinInfo>* skin, int variant, int color, std::set<std::string>& substituted,
                     int teeth = -1) {
	for (const zenkit::SubMesh& sub : mrm.sub_meshes) {
		SrcModelPart part;
		describeMaterial(sub.mat, part.material);
		if (!part.material.texture.empty()) {
			// The skin variant, if the game has it; the mesh's own texture otherwise.
			// (Teeth and mouth of a head have a number of their own.)
			const std::string texUp = upper(part.material.texture);
			const bool isTeeth = teeth >= 0 && (texUp.compare(0, 9, "HUM_TEETH") == 0 || texUp.compare(0, 9, "HUM_MOUTH") == 0);
			const std::string wanted = variantTexture(part.material.texture, isTeeth ? teeth : variant, color);
			SrcImage probe;
			if (wanted != upper(part.material.texture) && model.loadTexture && model.loadTexture(wanted, probe)) {
				part.material.texture = wanted;
				substituted.insert(wanted);
			}
		}
		for (const zenkit::MeshTriangle& tri : sub.triangles) {
			SrcSkinVertex v[3];
			bool ok = true;
			for (int k = 0; k < 3 && ok; ++k) {
				ok = tri.wedges[k] < sub.wedges.size();
				if (!ok) break;
				const zenkit::MeshWedge& w = sub.wedges[tri.wedges[k]];
				const float n[3] = {w.normal.x, w.normal.y, w.normal.z};
				if (skin) {
					ok = w.index < skin->size();
					if (!ok) break;
					const SkinInfo& si = (*skin)[w.index];
					std::copy_n(si.pos.data(), 3, v[k].pos);
					std::copy_n(n, 3, v[k].normal);
					v[k].bone[0] = si.bone[0];
					v[k].bone[1] = si.bone[1];
					v[k].weight1 = si.weight1;
				} else {
					ok = w.index < mrm.positions.size();
					if (!ok) break;
					const float p[3] = {mrm.positions[w.index].x, mrm.positions[w.index].y, mrm.positions[w.index].z};
					xf.point(p, v[k].pos);
					xf.direction(n, v[k].normal);
					v[k].bone[0] = v[k].bone[1] = node;
					v[k].weight1 = 0;
				}
				v[k].uv[0] = w.texture.x;
				v[k].uv[1] = w.texture.y;
			}
			if (ok) part.verts.insert(part.verts.end(), v, v + 3);
		}
		if (!part.verts.empty()) model.parts.push_back(std::move(part));
	}
}

// The animations the hero's movement uses. The states ("S_...") are what the
// body can be doing; the rest are single actions: hopping back, a step
// sideways, turning on the spot.
const char* const kHeroClips[] = {
    "S_RUN",        "S_RUNL",       "S_WALK",        "S_WALKL",       "S_WALKBL",    "S_WALKWL",    "S_JUMP",
    "S_FALLDN",     "S_FALL",       "S_SWIM",        "S_SWIMF",       "S_SWIMB",     "T_JUMPB",     "T_RUNSTRAFEL",
    "T_RUNSTRAFER", "T_WALKSTRAFEL", "T_WALKSTRAFER", "T_RUNTURNL",    "T_RUNTURNR",  "T_WALKTURNL", "T_WALKTURNR",
    "T_SWIMTURNL",  "T_SWIMTURNR",
};
// ZenGin goes from state A to state B through "T_A_2_B" if the model script
// has it (and from standing through "T_STAND_2_B", to standing through
// "T_A_2_STAND"). Every such transition between these states is brought along.
const char* const kHeroStates[] = {"STAND", "RUN", "RUNL", "WALK", "WALKL", "WALKBL", "WALKWL", "JUMP", "FALLDN", "FALL", "SWIM", "SWIMF", "SWIMB"};

bool isHeroTransition(const std::string& name) {
	if (name.compare(0, 2, "T_") != 0) return false;
	const size_t mid = name.find("_2_");
	if (mid == std::string::npos) return false;
	const std::string from = name.substr(2, mid - 2), to = name.substr(mid + 3);
	bool a = false, b = false;
	for (const char* st : kHeroStates) {
		a = a || from == st;
		b = b || to == st;
	}
	return a && b;
}

} // namespace

bool importZenActor(std::shared_ptr<const ZenGame> game, const ZenActorOptions& opt, SrcModel& model) {
	model = SrcModel {};
	model.name = opt.name;
	model.loadTexture = textureLoader(game);
	const zenkit::Vfs& vfs = game->vfs;
	const std::string scriptBase = stripExtension(upper(opt.script));

	zenkit::ModelScript script;
	if (!loadFile(vfs, scriptBase + ".MSB", script) && !loadFile(vfs, scriptBase + ".MDS", script)) {
		model.notes.push_back("character: " + scriptBase + ".MSB/.MDS not found, no character baked");
		return false;
	}
	// The skeleton is saved under the script's name; older files under the
	// name of the mesh it was made from.
	zenkit::ModelHierarchy hierarchy;
	std::string treeName = scriptBase + ".MDH";
	if (!loadFile(vfs, treeName, hierarchy)) {
		treeName = stripExtension(upper(script.skeleton.name)) + ".MDH";
		if (script.skeleton.name.empty() || !loadFile(vfs, treeName, hierarchy)) {
			model.notes.push_back("character: no skeleton (" + scriptBase + ".MDH) found, no character baked");
			return false;
		}
	}
	const size_t n = hierarchy.nodes.size();
	if (n == 0) throw std::runtime_error(treeName + " has no nodes");

	// ---- skeleton ----
	std::vector<M34> local;
	const std::vector<M34> rest = restPoseStanding(hierarchy, local);
	model.nodes.resize(n);
	int rootNode = -1, headNode = -1;
	for (size_t i = 0; i < n; ++i) {
		SrcNode& node = model.nodes[i];
		node.name = upper(hierarchy.nodes[i].name);
		const int parent = hierarchy.nodes[i].parent_index;
		node.parent = (parent >= 0 && size_t(parent) < n) ? parent : -1;
		for (int a = 0; a < 3; ++a) node.pos[a] = local[i].m[a][3];
		quatFromRows(local[i], node.rot);
		if (node.parent < 0 && rootNode < 0) rootNode = int(i);
		if (node.name == "BIP01 HEAD") headNode = int(i);
	}

	// ---- body ----
	std::set<std::string> substituted;
	const std::string bodyBase = stripExtension(upper(opt.body));
	zenkit::ModelMesh body;
	if (!bodyBase.empty() && !loadFile(vfs, bodyBase + ".MDM", body)) {
		model.notes.push_back("character: body mesh " + bodyBase + ".MDM not found, no character baked");
		return false;
	}
	float skinDeviation = 0;
	size_t manyBones = 0, skinVertices = 0;
	for (const zenkit::SoftSkinMesh& skin : body.meshes) {
		std::vector<SkinInfo> info(skin.mesh.positions.size());
		for (size_t i = 0; i < info.size(); ++i) {
			SkinInfo& si = info[i];
			si.pos = {skin.mesh.positions[i].x, skin.mesh.positions[i].y, skin.mesh.positions[i].z};
			si.bone[0] = si.bone[1] = rootNode < 0 ? 0 : rootNode;
			si.weight1 = 0;
			if (i >= skin.weights.size() || skin.weights[i].empty()) continue;
			// Rest position from all the bones; at run time the two strongest carry the vertex.
			float sum[3] = {0, 0, 0}, total = 0, best[2] = {0, 0};
			int bone[2] = {-1, -1};
			for (const zenkit::SoftSkinWeightEntry& w : skin.weights[i]) {
				if (w.node_index >= n) continue;
				const float lp[3] = {w.position.x, w.position.y, w.position.z};
				float wp[3];
				rest[w.node_index].point(lp, wp);
				for (int a = 0; a < 3; ++a) sum[a] += wp[a] * w.weight;
				total += w.weight;
				if (w.weight > best[0]) {
					best[1] = best[0];
					bone[1] = bone[0];
					best[0] = w.weight;
					bone[0] = w.node_index;
				} else if (w.weight > best[1]) {
					best[1] = w.weight;
					bone[1] = w.node_index;
				}
			}
			if (total < 1e-6f || bone[0] < 0) continue;
			float d2 = 0;
			for (int a = 0; a < 3; ++a) {
				const float p = sum[a] / total;
				d2 += (p - si.pos[size_t(a)]) * (p - si.pos[size_t(a)]);
				si.pos[size_t(a)] = p;
			}
			skinDeviation = std::max(skinDeviation, std::sqrt(d2));
			si.bone[0] = bone[0];
			si.bone[1] = bone[1] >= 0 ? bone[1] : bone[0];
			si.weight1 = bone[1] >= 0 ? best[1] / (best[0] + best[1]) : 0.0f;
			manyBones += skin.weights[i].size() > 2;
			++skinVertices;
		}
		appendActorMesh(model, skin.mesh, M34 {}, 0, &info, opt.bodyVariant, opt.skinColor, substituted);
	}
	// Rigid parts of the body file hang on the node of the same name.
	for (const auto& kv : body.attachments) {
		int node = rootNode < 0 ? 0 : rootNode;
		for (size_t i = 0; i < n; ++i)
			if (model.nodes[i].name == upper(kv.first)) node = int(i);
		appendActorMesh(model, kv.second, rest[size_t(node)], node, nullptr, opt.bodyVariant, opt.skinColor, substituted);
	}

	// ---- head ----
	std::string headNote, windingNote;
	if (!opt.head.empty()) {
		const std::string headBase = stripExtension(upper(opt.head));
		zenkit::MorphMesh head;
		if (headNode < 0) {
			headNote = "; no BIP01 HEAD node, head left out";
		} else if (!loadFile(vfs, headBase + ".MMB", head)) {
			headNote = "; head " + headBase + ".MMB not found";
		} else {
			appendActorMesh(model, head.mesh, rest[size_t(headNode)], headNode, nullptr, opt.headVariant, opt.skinColor,
			                substituted, opt.teethVariant);
			headNote = "; head " + headBase;
		}
	}
	size_t triangles = 0;
	for (const SrcModelPart& part : model.parts) triangles += part.verts.size() / 3;
	if (triangles == 0 && !bodyBase.empty()) throw std::runtime_error(bodyBase + ".MDM has no triangles");
	if (triangles == 0 && !opt.head.empty()) {
		// A head on its own that is not there.
		model.notes.push_back("character" + headNote.substr(headNote.empty() ? 0 : 1));
		return false;
	}
	// Which side of a triangle is its front? Decided as for the world's
	// objects: a body is a closed shape, its faces must point outwards.
	if (triangles) {
		double c[3] = {0, 0, 0}, count = 0, radius = 0, flux = 0, area = 0;
		for (const SrcModelPart& part : model.parts)
			for (const SrcSkinVertex& v : part.verts) {
				for (int a = 0; a < 3; ++a) c[a] += v.pos[a];
				++count;
			}
		for (double& a : c) a /= count;
		for (const SrcModelPart& part : model.parts) {
			for (const SrcSkinVertex& v : part.verts) {
				double d2 = 0;
				for (int a = 0; a < 3; ++a) d2 += (v.pos[a] - c[a]) * (v.pos[a] - c[a]);
				radius = std::max(radius, std::sqrt(d2));
			}
			for (size_t t = 0; t + 2 < part.verts.size(); t += 3) {
				const float* p0 = part.verts[t].pos;
				const float* p1 = part.verts[t + 1].pos;
				const float* p2 = part.verts[t + 2].pos;
				const double ax = p1[0] - p0[0], ay = p1[1] - p0[1], az = p1[2] - p0[2];
				const double bx = p2[0] - p0[0], by = p2[1] - p0[1], bz = p2[2] - p0[2];
				// Front normal (times twice the area) in a left-handed, CCW-front world.
				const double nx = -(ay * bz - az * by), ny = -(az * bx - ax * bz), nz = -(ax * by - ay * bx);
				flux += (p0[0] - c[0]) * nx + (p0[1] - c[1]) * ny + (p0[2] - c[2]) * nz;
				area += std::sqrt(nx * nx + ny * ny + nz * nz);
			}
		}
		const double score = area > 0 && radius > 0 ? flux / (area * 2.0 * radius) : 0;
		// A flat or open mesh says nothing either way and is left as it is.
		bool flip = score < -0.02;
		const bool wholeCharacter = opt.heroClips && !bodyBase.empty();
		const bool followed = !wholeCharacter && game->actorWinding != 0;
		if (followed)
			flip = game->actorWinding < 0;
		else if (wholeCharacter || std::fabs(score) > 0.15)
			game->actorWinding = flip ? -1 : 1;
		if (flip)
			for (SrcModelPart& part : model.parts)
				for (size_t t = 0; t + 2 < part.verts.size(); t += 3) std::swap(part.verts[t + 1], part.verts[t + 2]);
		char buf[160];
		std::snprintf(buf, sizeof(buf), "character mesh: %zu triangles, winding %s (score %.3f%s)", triangles,
		              flip ? "flipped" : "kept", score, followed ? "; as found for a whole character" : "");
		windingNote = buf;
	}
	{
		char buf[320];
		std::snprintf(buf, sizeof(buf),
		              "character: %s, skeleton %s (%zu nodes), body %s%s; rest pose within %.1f cm of the stored mesh; "
		              "%zu of %zu skin vertices hang on more than two bones (the two strongest are kept)",
		              scriptBase.c_str(), treeName.c_str(), n, bodyBase.c_str(), headNote.c_str(), double(skinDeviation),
		              manyBones, skinVertices);
		model.notes.push_back(buf);
	}
	if (!windingNote.empty()) model.notes.push_back(windingNote);
	if (!substituted.empty()) {
		std::string list;
		for (const std::string& t : substituted) list += (list.empty() ? "" : ", ") + t;
		model.notes.push_back("character skin: " + list);
	}
	// ---- animations ----
	std::vector<std::string> wanted = opt.clips;
	const bool standardSet = wanted.empty() && opt.heroClips;
	// Anything but the hero's own set leaves out the walking animations, which
	// say which way the model faces: they are read for that and then dropped,
	// so that every piece of a character - skeleton, body, head - is turned alike.
	std::set<std::string> witnesses;
	if (!standardSet)
		for (const char* w : {"S_RUNL", "S_WALKL", "S_FISTRUNL", "S_FISTWALKL"})
			if (std::find(wanted.begin(), wanted.end(), w) == wanted.end()) {
				wanted.push_back(w);
				witnesses.insert(w);
			}
	if (standardSet) {
		wanted.assign(std::begin(kHeroClips), std::end(kHeroClips));
		for (const zenkit::MdsAnimation& a : script.animations)
			if (isHeroTransition(upper(a.name))) wanted.push_back(upper(a.name));
		for (const zenkit::MdsAnimationAlias& a : script.aliases)
			if (isHeroTransition(upper(a.name))) wanted.push_back(upper(a.name));
		for (const zenkit::MdsAnimationBlend& b : script.blends) {
			if (!isHeroTransition(upper(b.name))) continue;
			SrcAniBlend blend;
			blend.name = upper(b.name);
			blend.next = upper(b.next);
			blend.blendIn = b.blend_in;
			blend.blendOut = b.blend_out;
			model.blends.push_back(blend);
		}
	}
	struct Loaded {
		std::string name, next;
		float blendIn = 0.1f, blendOut = 0.1f;
		int layer = 1;
		bool moves = false, fly = false, reversed = false;
		zenkit::ModelAnimation ani;
	};
	std::vector<Loaded> loadedClips;
	std::vector<std::string> notFound;
	for (size_t wantedAt = 0; wantedAt < wanted.size(); ++wantedAt) {
		const std::string name = upper(wanted[wantedAt]);
		Loaded lc;
		lc.name = name;
		std::string file = name;
		bool described = false;
		for (const zenkit::MdsAnimation& a : script.animations)
			if (upper(a.name) == name) {
				lc.next = upper(a.next);
				lc.blendIn = a.blend_in;
				lc.blendOut = a.blend_out;
				lc.layer = int(a.layer);
				lc.moves = a.flags & zenkit::AnimationFlags::MOVE;
				lc.fly = a.flags & zenkit::AnimationFlags::FLY;
				described = true;
				break;
			}
		if (!described)
			for (const zenkit::MdsAnimationAlias& a : script.aliases)
				if (upper(a.name) == name) {
					lc.next = upper(a.next);
					lc.blendIn = a.blend_in;
					lc.blendOut = a.blend_out;
					lc.layer = int(a.layer);
					lc.moves = a.flags & zenkit::AnimationFlags::MOVE;
					lc.fly = a.flags & zenkit::AnimationFlags::FLY;
					lc.reversed = a.direction == zenkit::AnimationDirection::BACKWARD;
					file = upper(a.alias);
					described = true;
					break;
				}
		// Asked for whatever the scripts might name: what this model does not
		// have is none of its business.
		if (!described && opt.onlyDeclared) continue;
		// What an animation leads into is wanted along with it ("T_STAND_2_SIT", then "S_SIT").
		if (described && opt.onlyDeclared && !lc.next.empty() && lc.next != name &&
		    std::find(wanted.begin(), wanted.end(), lc.next) == wanted.end())
			wanted.push_back(lc.next);
		// An alias may name another alias (the monsters' scripts do); the
		// samples are in the file of the animation at the end of the chain.
		for (int depth = 0; described && depth < 4; ++depth) {
			bool isAnimation = false, followed = false;
			for (const zenkit::MdsAnimation& a : script.animations) isAnimation = isAnimation || upper(a.name) == file;
			if (isAnimation) break;
			for (const zenkit::MdsAnimationAlias& a : script.aliases)
				if (upper(a.name) == file && upper(a.alias) != file) {
					if (a.direction == zenkit::AnimationDirection::BACKWARD) lc.reversed = !lc.reversed;
					file = upper(a.alias);
					followed = true;
					break;
				}
			if (!followed) break;
		}
		bool have = false;
		try {
			have = loadFile(vfs, scriptBase + "-" + file + ".MAN", lc.ani);
		} catch (const std::exception&) {
			have = false; // unreadable: treated like a missing one
		}
		if (!have || lc.ani.frame_count == 0 || lc.ani.node_count == 0 ||
		    lc.ani.samples.size() != size_t(lc.ani.frame_count) * lc.ani.node_count ||
		    lc.ani.node_indices.size() != lc.ani.node_count) {
			if (!witnesses.count(name)) notFound.push_back(name);
			continue;
		}
		bool nodesOk = true;
		for (uint32_t idx : lc.ani.node_indices) nodesOk = nodesOk && idx < n;
		if (!nodesOk) {
			if (!witnesses.count(name)) notFound.push_back(name + " (made for another skeleton)");
			continue;
		}
		if (!described) {
			lc.next = upper(lc.ani.next);
			lc.layer = int(lc.ani.layer);
		}
		loadedClips.push_back(std::move(lc));
	}

	// ZenGin's sample rotations turn the other way round than its node
	// matrices do. Rather than trust that, compare both readings with the
	// skeleton's rest pose over the first frame of every clip: a standing or
	// walking body is close to its rest pose, its mirror image is not.
	double asIs = 0, conjugated = 0, offset = 0;
	size_t compared = 0;
	for (const Loaded& lc : loadedClips)
		for (uint32_t t = 0; t < lc.ani.node_count; ++t) {
			const uint32_t node = lc.ani.node_indices[t];
			if (model.nodes[node].parent < 0) continue; // the root turns with the walking direction
			const zenkit::AnimationSample& s = lc.ani.samples[t];
			const float a[4] = {s.rotation.x, s.rotation.y, s.rotation.z, s.rotation.w};
			const float b[4] = {-s.rotation.x, -s.rotation.y, -s.rotation.z, s.rotation.w};
			asIs += quatAngle(a, model.nodes[node].rot);
			conjugated += quatAngle(b, model.nodes[node].rot);
			const float d[3] = {s.position.x - model.nodes[node].pos[0], s.position.y - model.nodes[node].pos[1],
			                    s.position.z - model.nodes[node].pos[2]};
			offset += std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			++compared;
		}
	const bool conjugate = conjugated <= asIs;
	if (compared) {
		char buf[256];
		std::snprintf(buf, sizeof(buf),
		              "animations: rotations read %s (mean distance from the rest pose %.0f degrees; the other way %.0f); "
		              "bone offsets within %.1f cm of the skeleton's on average",
		              conjugate ? "conjugated" : "as stored", (conjugate ? conjugated : asIs) / double(compared),
		              (conjugate ? asIs : conjugated) / double(compared), offset / double(compared));
		model.notes.push_back(buf);
	}

	for (const Loaded& lc : loadedClips) {
		SrcClip clip;
		clip.name = lc.name;
		clip.next = lc.next;
		clip.fps = lc.ani.fps > 0 ? lc.ani.fps : 25.0f;
		clip.layer = lc.layer;
		clip.blendIn = lc.blendIn;
		clip.blendOut = lc.blendOut;
		clip.moves = lc.moves;
		clip.fly = lc.fly;
		for (uint32_t idx : lc.ani.node_indices) clip.nodes.push_back(int(idx));
		const uint32_t frames = lc.ani.frame_count, tracks = lc.ani.node_count;
		clip.samples.resize(size_t(frames) * tracks);
		for (uint32_t f = 0; f < frames; ++f) {
			const uint32_t src = lc.reversed ? frames - 1 - f : f;
			for (uint32_t t = 0; t < tracks; ++t) {
				const zenkit::AnimationSample& s = lc.ani.samples[size_t(src) * tracks + t];
				SrcClipSample& out = clip.samples[size_t(f) * tracks + t];
				out.pos[0] = s.position.x;
				out.pos[1] = s.position.y;
				out.pos[2] = s.position.z;
				const float sign = conjugate ? -1.0f : 1.0f;
				out.rot[0] = s.rotation.x * sign;
				out.rot[1] = s.rotation.y * sign;
				out.rot[2] = s.rotation.z * sign;
				out.rot[3] = s.rotation.w;
			}
		}
		model.clips.push_back(std::move(clip));
	}
	// Which way does it face? The game moves a character along +Z, so that is
	// where its walking animation must carry it - and where its toes point.
	{
		double travel[2] = {0, 0};
		std::string witness;
		for (const char* name : {"S_RUNL", "S_WALKL", "S_FISTRUNL", "S_FISTWALKL", "S_RUN", "S_WALK"}) {
			for (const SrcClip& clip : model.clips) {
				if (clip.name != name || !clip.moves || !witness.empty()) continue;
				const size_t tracks = clip.nodes.size(), frames = clip.samples.size() / tracks;
				for (size_t t = 0; t < tracks; ++t) {
					if (model.nodes[size_t(clip.nodes[t])].parent >= 0) continue;
					const double dx = double(clip.samples[(frames - 1) * tracks + t].pos[0]) - double(clip.samples[t].pos[0]);
					const double dz = double(clip.samples[(frames - 1) * tracks + t].pos[2]) - double(clip.samples[t].pos[2]);
					if (dx * dx + dz * dz < 10.0 * 10.0) continue; // marching on the spot says nothing
					travel[0] = dx;
					travel[1] = dz;
					witness = clip.name;
					break;
				}
			}
		}
		float toe = 0, foot = 0;
		int toes = 0, feet = 0;
		for (size_t i = 0; i < n; ++i) {
			if (model.nodes[i].name.find("TOE") != std::string::npos) {
				toe += rest[i].m[2][3];
				++toes;
			} else if (model.nodes[i].name.find("FOOT") != std::string::npos) {
				foot += rest[i].m[2][3];
				++feet;
			}
		}
		const float toesAhead = toes && feet ? toe / float(toes) - foot / float(feet) : 0.0f;
		// Quarter turns about the vertical that bring "forward" to +Z.
		int quarters = 0;
		char why[160] = "";
		if (!witness.empty()) {
			const double angle = std::atan2(travel[0], travel[1]); // 0 = along +Z
			quarters = int(std::lround(-angle / 1.5707963267948966)) & 3;
			std::snprintf(why, sizeof(why), "%s carries it %.0f cm along %s", witness.c_str(),
			              std::sqrt(travel[0] * travel[0] + travel[1] * travel[1]),
			              quarters == 0 ? "+Z" : quarters == 2 ? "-Z" : quarters == 1 ? "-X" : "+X");
		} else if (toes && feet) {
			quarters = toesAhead >= 0 ? 0 : 2;
			std::snprintf(why, sizeof(why), "toes %.1f cm %s the heels", double(std::fabs(toesAhead)),
			              toesAhead >= 0 ? "in front of" : "behind");
		}
		if (quarters) {
			const float angle = float(quarters) * 1.5707963f;
			const float cs = std::round(std::cos(angle)), sn = std::round(std::sin(angle));
			const float half[2] = {std::sin(angle * 0.5f), std::cos(angle * 0.5f)};
			auto turnVec = [&](float v[3]) {
				const float x = v[0] * cs + v[2] * sn, z = -v[0] * sn + v[2] * cs;
				v[0] = x;
				v[2] = z;
			};
			auto turnRot = [&](float q[4]) { // the turn after q
				const float x = half[1] * q[0] + half[0] * q[2], y = half[1] * q[1] + half[0] * q[3];
				const float z = half[1] * q[2] - half[0] * q[0], w = half[1] * q[3] - half[0] * q[1];
				q[0] = x;
				q[1] = y;
				q[2] = z;
				q[3] = w;
			};
			for (SrcNode& node : model.nodes)
				if (node.parent < 0) {
					turnVec(node.pos);
					turnRot(node.rot);
				}
			for (SrcModelPart& part : model.parts)
				for (SrcSkinVertex& v : part.verts) {
					turnVec(v.pos);
					turnVec(v.normal);
				}
			for (SrcClip& clip : model.clips)
				for (size_t t = 0; t < clip.nodes.size(); ++t) {
					if (model.nodes[size_t(clip.nodes[t])].parent >= 0) continue;
					for (size_t f = t; f < clip.samples.size(); f += clip.nodes.size()) {
						turnVec(clip.samples[f].pos);
						turnRot(clip.samples[f].rot);
					}
				}
		}
		if (*why) {
			char buf[256];
			if (quarters)
				std::snprintf(buf, sizeof(buf), "character turned by %d degrees to face +Z (%s)", quarters * 90, why);
			else
				std::snprintf(buf, sizeof(buf), "character faces +Z (%s)", why);
			model.notes.push_back(buf);
		}
	}
	// Where the hips are, at rest and in the first clip: the two must agree,
	// or the animations are not in the skeleton's space.
	if (rootNode >= 0 && !loadedClips.empty()) {
		const Loaded& first = loadedClips[0];
		for (uint32_t t = 0; t < first.ani.node_count; ++t)
			if (int(first.ani.node_indices[t]) == rootNode) {
				char buf[200];
				std::snprintf(buf, sizeof(buf), "animations: hips %.1f cm above the ground at rest, %.1f cm in %s",
				              double(model.nodes[size_t(rootNode)].pos[1]), double(first.ani.samples[t].position.y),
				              first.name.c_str());
				model.notes.push_back(buf);
			}
	}
	if (!notFound.empty()) {
		std::string list;
		for (const std::string& m : notFound) list += (list.empty() ? "" : ", ") + m;
		model.notes.push_back("animations not found (the character falls back to simpler ones): " + list);
	}
	if (!witnesses.empty())
		model.clips.erase(std::remove_if(model.clips.begin(), model.clips.end(),
		                                 [&](const SrcClip& c) { return witnesses.count(c.name) != 0; }),
		                  model.clips.end());
	if (model.clips.empty() && (standardSet || !opt.clips.empty()))
		model.notes.push_back("WARNING: no animation could be read; the character stands in its rest pose");
	return true;
}

LookSource zenLookSource(std::shared_ptr<const ZenGame> game) {
	LookSource source;
	source.rig = [game](const std::string& script, bool, const std::set<std::string>& wanted, SrcModel& model) {
		ZenActorOptions o;
		o.script = script;
		o.body.clear();
		o.head.clear();
		o.heroClips = false;
		o.onlyDeclared = true;
		// Standing and going, for people and (the "FIST" ones) for creatures ...
		for (const char* mode : {"RUN", "WALK", "FISTRUN", "FISTWALK"}) {
			const std::string m = mode;
			for (const std::string& name : {"S_" + m, "S_" + m + "L", "T_" + m + "_2_" + m + "L", "T_" + m + "L_2_" + m}) o.clips.push_back(name);
		}
		// ... and whatever the scripts name that this model has.
		for (const std::string& name : wanted)
			if (std::find(o.clips.begin(), o.clips.end(), name) == o.clips.end()) o.clips.push_back(name);
		if (!importZenActor(game, o, model)) return false;
		bool stands = false;
		for (const SrcClip& c : model.clips) stands = stands || c.name == "S_RUN" || c.name == "S_WALK" || c.name == "S_FISTRUN" || c.name == "S_FISTWALK";
		if (!stands) {
			model.clips.clear();
			model.notes.push_back(upper(script) + ": no animation to stand in");
		}
		return true;
	};
	source.body = [game](const std::string& script, const std::string& mesh, int tex, int skin, SrcModel& model) {
		ZenActorOptions o;
		o.script = script;
		o.body = mesh;
		o.head.clear();
		o.bodyVariant = tex;
		o.skinColor = skin;
		o.heroClips = false;
		return importZenActor(game, o, model);
	};
	source.head = [game](const std::string& script, const std::string& head, int tex, int skin, int teeth, SrcModel& model) {
		ZenActorOptions o;
		o.script = script;
		o.body.clear();
		o.head = head;
		o.headVariant = tex;
		o.skinColor = skin;
		o.teethVariant = teeth;
		o.heroClips = false;
		return importZenActor(game, o, model);
	};
	return source;
}

// ---- the camera ----------------------------------------------------------

namespace {

// A path below `root` given in any letter case (the game is a Windows program).
fs::path findNoCase(fs::path root, std::initializer_list<const char*> parts) {
	for (const char* part : parts) {
		fs::path next;
		std::error_code ec;
		for (const auto& e : fs::directory_iterator(root, ec))
			if (lower(e.path().filename().string()) == part) next = e.path();
		if (next.empty()) return {};
		root = next;
	}
	return root;
}

} // namespace

bool importZenCamera(std::shared_ptr<const ZenGame> game, CameraSet& set, std::vector<std::string>& notes) {
	set = defaultCameraSet();
	std::unique_ptr<zenkit::Read> rd;
	// Compiled scripts are loose files of the installation; a mod may carry them in an archive.
	const fs::path loose = findNoCase(game->dir, {"_work", "data", "scripts", "_compiled", "camera.dat"});
	if (!loose.empty() && fs::is_regular_file(loose)) {
		rd = zenkit::Read::from(loose);
	} else if (const zenkit::VfsNode* node = game->vfs.find("CAMERA.DAT")) {
		rd = node->open_read();
	} else {
		notes.push_back("camera: CAMERA.DAT not found (_work/Data/Scripts/_compiled); this project's own set-up is used");
		return false;
	}
	try {
		zenkit::DaedalusScript script;
		script.load(rd.get());
		if (!script.find_symbol_by_name("CCAMSYS")) throw std::runtime_error("no class CCAMSYS in it");
		zenkit::ICamera::register_(script);
		zenkit::DaedalusVm vm {std::move(script), zenkit::DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS};
		static const char* const kNames[CAMERA_MODE_COUNT] = {"CAMMODNORMAL", "CAMMODSWIM", "CAMMODFALL"};
		std::string list;
		for (u32 i = 0; i < CAMERA_MODE_COUNT; ++i) {
			zenkit::DaedalusSymbol* sym = vm.find_symbol_by_name(kNames[i]);
			if (!sym) {
				if (i == CAMERA_NORMAL) throw std::runtime_error("no instance CAMMODNORMAL in it");
				set.mode[i] = set.mode[CAMERA_NORMAL];
				continue;
			}
			// Running a script that ends early would never return: its
			// missing code reads as "add" for ever. Walk it first.
			std::function<void(std::uint32_t, int)> walk = [&](std::uint32_t at, int depth) {
				for (int steps = 0; steps < 4096; ++steps) {
					if (at >= vm.size()) break;
					const zenkit::DaedalusInstruction in = vm.instruction_at(at);
					if (in.op == zenkit::DaedalusOpcode::RSR) return;
					if (in.op == zenkit::DaedalusOpcode::BL && depth < 4) walk(in.address, depth + 1);
					at += in.size;
				}
				throw std::runtime_error(std::string("the script of ") + kNames[i] + " has no end: the file is cut short");
			};
			walk(sym->address(), 0);
			const auto cam = vm.init_instance<zenkit::ICamera>(sym);
			CameraMode& m = set.mode[i];
			m.bestRange = cam->best_range;
			m.minRange = cam->min_range;
			m.maxRange = cam->max_range;
			m.bestElevation = cam->best_elevation;
			m.minElevation = cam->min_elevation;
			m.maxElevation = cam->max_elevation;
			m.bestAzimuth = cam->best_azimuth;
			m.minAzimuth = cam->min_azimuth;
			m.maxAzimuth = cam->max_azimuth;
			m.rotOffsetX = cam->rot_offset_x;
			m.rotOffsetY = cam->rot_offset_y;
			m.targetOffsetX = cam->target_offset_x;
			m.targetOffsetY = cam->target_offset_y;
			m.targetOffsetZ = cam->target_offset_z;
			m.veloTrans = cam->velo_trans;
			m.veloRot = cam->velo_rot;
			// Values no camera can have mean the file was not understood.
			if (!(m.bestRange >= 0.5f && m.bestRange <= 20.0f) || !(std::fabs(m.bestElevation) < 90.0f) ||
			    !(std::fabs(m.rotOffsetX) < 90.0f) || !(m.veloTrans > 0.0f && m.veloTrans < 1000.0f))
				throw std::runtime_error(std::string(kNames[i]) + " has values out of range");
			char buf[200];
			std::snprintf(buf, sizeof(buf), "%s%s range %.2f m (%.2f..%.2f), elevation %.0f, azimuth %.0f, view offset %.0f %.0f, pace %.0f",
			              list.empty() ? "" : "; ", kNames[i], double(m.bestRange), double(m.minRange), double(m.maxRange),
			              double(m.bestElevation), double(m.bestAzimuth), double(m.rotOffsetX), double(m.rotOffsetY),
			              double(m.veloTrans));
			list += buf;
		}
		set.fromGame = 1;
		notes.push_back("camera: from the game's CAMERA.DAT: " + list);
		return true;
	} catch (const std::exception& ex) {
		set = defaultCameraSet();
		notes.push_back(std::string("camera: CAMERA.DAT could not be used (") + ex.what() + "); this project's own set-up is used");
		return false;
	}
}

bool importZenScript(std::shared_ptr<const ZenGame> game, SrcScript& script, std::vector<u8>& dat,
                     std::vector<std::string>& notes) {
	script = SrcScript();
	dat.clear();
	// Compiled scripts are loose files of the installation; a mod may carry them in an archive.
	const fs::path loose = findNoCase(game->dir, {"_work", "data", "scripts", "_compiled", "gothic.dat"});
	std::unique_ptr<zenkit::Read> rd;
	if (!loose.empty() && fs::is_regular_file(loose)) {
		rd = zenkit::Read::from(loose);
		script.source = "GOTHIC.DAT";
	} else if (const zenkit::VfsNode* node = game->vfs.find("GOTHIC.DAT")) {
		rd = node->open_read();
		script.source = "GOTHIC.DAT (from an archive)";
	} else {
		notes.push_back("scripts: GOTHIC.DAT not found (_work/Data/Scripts/_compiled); the world stays empty");
		return false;
	}
	try {
		rd->seek(0, zenkit::Whence::END);
		const size_t size = rd->tell();
		rd->seek(0, zenkit::Whence::BEG);
		if (size < 16 || size > (256u << 20)) throw std::runtime_error("its size makes no sense");
		dat.resize(size);
		rd->read(dat.data(), size);
		rd->seek(0, zenkit::Whence::BEG);

		zenkit::DaedalusScript zs;
		zs.load(rd.get());
		const u32 codeSize = zs.size();
		// The code is what the file ends with, after its size.
		u32 stated = 0;
		if (size >= u64(codeSize) + 4) std::memcpy(&stated, dat.data() + size - codeSize - 4, 4);
		if (size < u64(codeSize) + 4 || stated != codeSize) {
			// Not laid out as expected: this project's own reader may still cope.
			readScriptDat(dat, script);
			notes.push_back("scripts: GOTHIC.DAT has something after its code; read with this project's own reader");
			return true;
		}
		script.code.assign(dat.end() - codeSize, dat.end());
		const auto& symbols = zs.symbols();
		script.symbols.resize(symbols.size());
		for (size_t i = 0; i < symbols.size(); ++i) {
			const zenkit::DaedalusSymbol& z = symbols[i];
			SrcSymbol& o = script.symbols[i];
			o.name = upper(z.name());
			const u32 type = u32(z.type());
			if (type > SCRIPT_INSTANCE) throw std::runtime_error("the symbol '" + o.name + "' has an unknown type");
			o.type = ScriptType(type);
			o.count = z.count();
			o.isConst = z.is_const();
			o.hasReturn = z.has_return();
			o.member = z.is_member();
			o.external = z.is_external();
			const u32 rtype = u32(z.rtype());
			o.returnType = rtype <= SCRIPT_INSTANCE ? ScriptType(rtype) : SCRIPT_VOID;
			o.parent = i32(z.parent());
			if (o.parent < -1 || o.parent >= i32(symbols.size())) o.parent = -1;
			if (o.member) {
				o.memberOffset = z.offset_as_member();
				continue;
			}
			switch (o.type) {
			case SCRIPT_INT:
				for (u32 k = 0; k < o.count; ++k) o.ints.push_back(z.get_int(u16(k)));
				break;
			case SCRIPT_FLOAT:
				for (u32 k = 0; k < o.count; ++k) o.floats.push_back(z.get_float(u16(k)));
				break;
			case SCRIPT_STRING:
				for (u32 k = 0; k < o.count; ++k) o.strings.push_back(z.get_string(u16(k)));
				break;
			case SCRIPT_CLASS:
				o.classSize = z.class_size();
				break;
			case SCRIPT_FUNC:
			case SCRIPT_PROTOTYPE:
			case SCRIPT_INSTANCE:
				o.address = z.address();
				break;
			default:
				break;
			}
		}
		return true;
	} catch (const std::exception& ex) {
		script = SrcScript();
		dat.clear();
		notes.push_back(std::string("scripts: GOTHIC.DAT could not be read (") + ex.what() + "); the world stays empty");
		return false;
	}
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
	if (world.way_net) {
		std::map<const zenkit::WayPoint*, u32> index;
		for (const auto& wp : world.way_net->points) {
			if (!wp || wp->name.empty()) continue;
			const float yaw = std::atan2(wp->direction.x, wp->direction.z);
			scene.points[upper(wp->name)] = SrcPoint {{wp->position.x, wp->position.y, wp->position.z}, yaw};
			// (ZenGin keeps the points no way leads to apart; they are points of
			// the net all the same, places one can be sent to.)
			index[wp.get()] = u32(scene.wayPoints.size());
			scene.wayPoints.push_back(SrcWayPoint {upper(wp->name), {wp->position.x, wp->position.y, wp->position.z}, yaw});
		}
		for (const auto& edge : world.way_net->edges) {
			const auto a = index.find(edge.first.get()), b = index.find(edge.second.get());
			if (a != index.end() && b != index.end() && a->second != b->second) scene.wayEdges.push_back({a->second, b->second});
		}
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
