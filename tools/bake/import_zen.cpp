// ZenGin importer: reads the world mesh, its lightmaps, the world objects
// (meshes, models in their rest pose, decals), portals and all textures
// straight from a Gothic installation through ZenKit. Nothing from the game
// is redistributed; the user bakes their own copy.
#include "g3ds/game.h"
#include "g3ds/model.h"
#include "fight_clips.h"
#include "font_bake.h"
#include "importers.h"

#include "g3ds/world.h"
#include "ground.h"
#include "vertex_fx.h"

#include <zenkit/CutsceneLibrary.hh>
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
#include <zenkit/Font.hh>
#include <zenkit/Vfs.hh>
#include <zenkit/World.hh>
#include <zenkit/world/VobTree.hh>
#include <zenkit/vobs/Camera.hh>
#include <zenkit/vobs/Light.hh>
#include <zenkit/vobs/MovableObject.hh>
#include <zenkit/vobs/Sound.hh>
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
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
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

	// What is dark and large in the source (before any cutting): a flat black patch seen in the game is one of these
	// when it comes from the source's light. The biggest are listed with their place (metres), to match against the
	// game's position display.
	{
		struct Dark { float area, edge, at[3]; bool lightmap; u8 rgb[3][3]; };
		std::vector<Dark> dark;
		u64 darkLightmap = 0, darkVertex = 0;
		for (size_t t = 0; t < triCount; ++t) {
			const SrcTriangle& w = scene.triangles[firstTriangle + t];
			float edge = 0;
			int bright = 0;
			for (int k = 0; k < 3; ++k) {
				float d2 = 0;
				for (int a = 0; a < 3; ++a) {
					const float d = w.v[(k + 1) % 3].pos[a] - w.v[k].pos[a];
					d2 += d * d;
				}
				edge = std::max(edge, std::sqrt(d2));
				bright = std::max({bright, int(w.v[k].rgba[0]), int(w.v[k].rgba[1]), int(w.v[k].rgba[2])});
			}
			if (edge < 500.0f || bright >= 30) continue;
			const bool lm = t < hasSample.size() && hasSample[t] && trusted;
			(lm ? darkLightmap : darkVertex)++;
			Dark d {};
			d.edge = edge;
			d.area = edge * edge;
			d.lightmap = lm;
			for (int a = 0; a < 3; ++a) d.at[a] = (w.v[0].pos[a] + w.v[1].pos[a] + w.v[2].pos[a]) / 3.0f * 0.01f;
			for (int k = 0; k < 3; ++k) std::copy_n(w.v[k].rgba, 3, d.rgb[k]);
			dark.push_back(d);
		}
		if (!dark.empty()) {
			std::sort(dark.begin(), dark.end(), [](const Dark& a, const Dark& b) { return a.area > b.area; });
			std::string note = "dark large triangles in the source (edge 5 m or more, no corner brighter than 30/255): " +
			                   std::to_string(darkLightmap) + " lit from a lightmap, " + std::to_string(darkVertex) +
			                   " by the vertex light; the largest (x y z in m, edge, source):";
			char buf[160];
			for (size_t i = 0; i < dark.size() && i < 12; ++i) {
				std::snprintf(buf, sizeof(buf), " [%.0f %.0f %.0f, %.0f m, %s %d/%d/%d]", double(dark[i].at[0]), double(dark[i].at[1]),
				              double(dark[i].at[2]), double(dark[i].edge * 0.01f), dark[i].lightmap ? "lightmap" : "vertex",
				              dark[i].rgb[0][0], dark[i].rgb[1][0], dark[i].rgb[2][0]);
				note += buf;
			}
			scene.notes.push_back(note);
		}
	}

	// A lightmap holds more than its polygon's corners: where a shade's edge crosses a large triangle, the corners'
	// light stretched over it is a flat dark (or bright) wedge. Cut such triangles into four - the new corners
	// sample the lightmap at their own place - until the corners agree with the lightmap or the cut is small.
	u64 lmCut = 0, lmTriangles = 0;
	bool lmExhausted = false;
	if (trusted && opt.lightmaps && opt.lightmapExtra > 0) {
		const float cell = std::max(opt.lightmapCell, 10.0f);
		u32 budget = opt.lightmapExtra;
		std::vector<SrcTriangle> extra;
		auto sampleAt = [&](const zenkit::LightMap& lm, const float pos[3], u8 rgb[3]) {
			zenkit::Vec3 q;
			q.x = pos[0]; q.y = pos[1]; q.z = pos[2];
			bool in = false;
			return sampler.sample(lm, q, rgb, in);
		};
		auto edgeOf = [](const SrcTriangle& w) {
			float e = 0;
			for (int k = 0; k < 3; ++k) {
				float d2 = 0;
				for (int a = 0; a < 3; ++a) {
					const float d = w.v[(k + 1) % 3].pos[a] - w.v[k].pos[a];
					d2 += d * d;
				}
				e = std::max(e, std::sqrt(d2));
			}
			return e;
		};
		// How far the lightmap, at the middles of the edges and of the triangle, is from what the corners give there.
		auto error = [&](const zenkit::LightMap& lm, const SrcTriangle& w) {
			float worst = 0;
			for (int probe = 0; probe < 4; ++probe) {
				float pos[3], expect[3];
				for (int a = 0; a < 3; ++a) {
					if (probe < 3) pos[a] = (w.v[probe].pos[a] + w.v[(probe + 1) % 3].pos[a]) * 0.5f;
					else pos[a] = (w.v[0].pos[a] + w.v[1].pos[a] + w.v[2].pos[a]) / 3.0f;
				}
				for (int c = 0; c < 3; ++c)
					expect[c] = probe < 3 ? (float(w.v[probe].rgba[c]) + float(w.v[(probe + 1) % 3].rgba[c])) * 0.5f
					                      : (float(w.v[0].rgba[c]) + float(w.v[1].rgba[c]) + float(w.v[2].rgba[c])) / 3.0f;
				u8 got[3];
				if (!sampleAt(lm, pos, got)) return 0.0f;
				for (int c = 0; c < 3; ++c) worst = std::max(worst, std::fabs(float(got[c]) - expect[c]));
			}
			return worst;
		};
		std::vector<SrcTriangle> work, next, done;
		for (size_t t = 0; t < triCount && budget >= 3; ++t) {
			if (!hasSample[t]) continue;
			SrcTriangle& root = scene.triangles[firstTriangle + t];
			if (edgeOf(root) <= cell) continue;
			const zenkit::LightMap& lm = mesh.lightmaps[size_t(li[t])];
			work.assign(1, root);
			done.clear();
			for (int depth = 0; depth < 6 && !work.empty(); ++depth) {
				next.clear();
				for (const SrcTriangle& w : work) {
					if (budget < 3 || edgeOf(w) <= cell || error(lm, w) < 16.0f) {
						done.push_back(w);
						continue;
					}
					SrcVertex m[3];
					for (int k = 0; k < 3; ++k) {
						const SrcVertex &p = w.v[k], &q = w.v[(k + 1) % 3];
						m[k] = p;
						for (int a = 0; a < 3; ++a) m[k].pos[a] = (p.pos[a] + q.pos[a]) * 0.5f;
						for (int a = 0; a < 2; ++a) m[k].uv[a] = (p.uv[a] + q.uv[a]) * 0.5f;
						if (!sampleAt(lm, m[k].pos, m[k].rgba)) std::copy_n(p.rgba, 3, m[k].rgba);
						m[k].rgba[3] = 0;
					}
					SrcTriangle c[4] = {w, w, w, w};
					c[0].v[0] = w.v[0]; c[0].v[1] = m[0]; c[0].v[2] = m[2];
					c[1].v[0] = m[0]; c[1].v[1] = w.v[1]; c[1].v[2] = m[1];
					c[2].v[0] = m[2]; c[2].v[1] = m[1]; c[2].v[2] = w.v[2];
					c[3].v[0] = m[0]; c[3].v[1] = m[1]; c[3].v[2] = m[2];
					for (const SrcTriangle& n : c) next.push_back(n);
					budget -= 3;
				}
				work.swap(next);
			}
			for (const SrcTriangle& w : work) done.push_back(w);
			if (done.size() > 1) {
				++lmCut;
				root = done[0];
				for (size_t k = 1; k < done.size(); ++k) extra.push_back(done[k]);
			}
		}
		lmTriangles = extra.size();
		lmExhausted = budget < 3;
		scene.triangles.insert(scene.triangles.end(), extra.begin(), extra.end());
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
			        "% of the corners inside the lightmap rectangle)" +
			        (opt.lightmapExtra > 0
			             ? "; " + std::to_string(lmCut) + " large ones cut where the lightmap differs from their corners, +" +
			                   std::to_string(lmTriangles) + " triangles (--lightmap-extra " + std::to_string(opt.lightmapExtra) + ")" +
			                   (lmExhausted ? " - USED UP: later ones were left whole" : "")
			             : std::string());
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
	std::string node; // models: the skeleton node a rigid part hangs on (upper case), "" for others
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
	// Models only: the nodes that say where somebody using the thing stands
	// ("ZS_POS0", "ZS_POS1_FRONT"), in the model's rest pose.
	std::vector<std::pair<std::string, M34>> slots;
	// Models only: every node of the rest pose by name (a fire place's flames hang on one: BIP01 FIRE...).
	std::map<std::string, M34> nodes;

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
	if (hierarchy)
		for (size_t i = 0; i < hierarchy->nodes.size(); ++i) {
			const std::string name = upper(hierarchy->nodes[i].name);
			if (name.compare(0, 6, "ZS_POS") == 0) out.slots.emplace_back(name, pose[i]);
			out.nodes[name] = pose[i];
		}
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
		const size_t firstSub = out.subs.size();
		appendMrm(out, kv.second, xf);
		for (size_t k = firstSub; k < out.subs.size(); ++k) out.subs[k].node = upper(kv.first);
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
			// A visual named as an .ASC is that file as the game compiled it -
			// one .MDL, mesh and skeleton together - and only then anything
			// else of its name. The order matters: a model script of the same
			// name compiles to an .MDM and an .MDH of that name too, and what
			// a script calls its mesh need not be this visual at all (the
			// game's FIREPLACE_HIGH2.MDS names the mesh of the bowl that
			// stands on the ground). The script itself comes last.
			if (ext == "ASC" || ext == "MDL") {
				candidates.push_back({"", "", base});
				candidates.push_back({base, base, ""});
			} else if (ext != "MDS") {
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

// The rotation whose matrix has these columns (model space -> world) as a
// quaternion (x y z w).
void quatFromColumns(float xx, float xy, float xz, float yx, float yy, float yz, float zx, float zy, float zz, float q[4]) {
	// m[row][column]: column 0 is (xx, xy, xz).
	const float m00 = xx, m10 = xy, m20 = xz, m01 = yx, m11 = yy, m21 = yz, m02 = zx, m12 = zy, m22 = zz;
	const float trace = m00 + m11 + m22;
	if (trace > 0.0f) {
		const float t = std::sqrt(trace + 1.0f) * 2.0f;
		q[3] = 0.25f * t;
		q[0] = (m21 - m12) / t;
		q[1] = (m02 - m20) / t;
		q[2] = (m10 - m01) / t;
	} else if (m00 > m11 && m00 > m22) {
		const float t = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
		q[3] = (m21 - m12) / t;
		q[0] = 0.25f * t;
		q[1] = (m01 + m10) / t;
		q[2] = (m02 + m20) / t;
	} else if (m11 > m22) {
		const float t = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
		q[3] = (m02 - m20) / t;
		q[0] = (m01 + m10) / t;
		q[1] = 0.25f * t;
		q[2] = (m12 + m21) / t;
	} else {
		const float t = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
		q[3] = (m10 - m01) / t;
		q[0] = (m02 + m20) / t;
		q[1] = (m12 + m21) / t;
		q[2] = 0.25f * t;
	}
	const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	for (int k = 0; k < 4; ++k) q[k] = len > 0.0f ? q[k] / len : (k == 3 ? 1.0f : 0.0f);
}

// ---- world objects (VObs) ------------------------------------------------

struct ObjectInstance {
	const zenkit::VirtualObject* vob;
	FlatPtr mesh;
};

// ---- fix157: a chest's lid ----
//
// A chest is a model whose lid is a node of its own; the model script has the lid's way up ("T_S0_2_S1"). The lid is
// cut out of the chest's mesh and made a mover (drawn by the runtime, turned between two keys when the chest is opened
// and shut), the rest stays where it was. Anything that does not fit - no such animation, no one moving node, no part
// on it - leaves the chest whole, as before, and says why.
struct ChestLid {
	FlatPtr base, lid; // the chest without the lid; the lid in its node's own space
	M34 closed, open;  // the lid's node in the model's space, shut and open
	size_t lidTriangles = 0, baseTriangles = 0; // for the report
	std::string lidNode;
	float seconds = 0; // how long the opening takes
};

static bool chestLidOf(const zenkit::Vfs& vfs, const std::string& visual, const FlatMesh& full, ChestLid& out, std::string& why) {
	const std::string base = stripExtension(upper(visual));
	zenkit::ModelScript script;
	bool have = false;
	try {
		have = loadFile(vfs, base + ".MSB", script) || loadFile(vfs, base + ".MDS", script);
	} catch (const std::exception&) {
		have = false;
	}
	if (!have) {
		why = "no model script";
		return false;
	}
	std::string file;
	for (const zenkit::MdsAnimation& a : script.animations)
		if (upper(a.name) == "T_S0_2_S1") file = "T_S0_2_S1";
	if (file.empty())
		for (const zenkit::MdsAnimationAlias& a : script.aliases)
			if (upper(a.name) == "T_S0_2_S1") file = upper(a.alias);
	if (file.empty()) {
		why = "no T_S0_2_S1 in the script";
		return false;
	}
	zenkit::ModelAnimation ani;
	bool loaded = false;
	try {
		loaded = loadFile(vfs, base + "-" + file + ".MAN", ani);
	} catch (const std::exception&) {
		loaded = false;
	}
	if (!loaded || ani.frame_count < 2 || ani.node_count == 0 || ani.samples.size() != size_t(ani.frame_count) * ani.node_count ||
	    ani.node_indices.size() != ani.node_count) {
		why = "its animation file is missing or odd";
		return false;
	}
	zenkit::ModelHierarchy h;
	bool haveTree = false;
	const std::string skeleton = script.skeleton.name.empty() ? std::string() : stripExtension(upper(script.skeleton.name));
	for (const std::string& name : {base, skeleton}) {
		if (name.empty() || haveTree) continue;
		try {
			if (loadFile(vfs, name + ".MDH", h)) {
				haveTree = true;
			} else {
				zenkit::Model model;
				if (loadFile(vfs, name + ".MDL", model)) {
					h = model.hierarchy;
					haveTree = true;
				}
			}
		} catch (const std::exception&) {
			haveTree = false;
		}
	}
	const size_t n = h.nodes.size();
	if (!haveTree || n == 0) {
		why = "no skeleton file";
		return false;
	}
	for (uint32_t idx : ani.node_indices)
		if (idx >= n) {
			why = "the animation is made for another skeleton";
			return false;
		}
	float shift[3];
	const std::vector<M34> rest = restPose(h, shift);
	const auto quatOf = [](const M34& m, float q[4]) {
		quatFromColumns(m.m[0][0], m.m[1][0], m.m[2][0], m.m[0][1], m.m[1][1], m.m[2][1], m.m[0][2], m.m[1][2], m.m[2][2], q);
	};
	const auto angleOf = [](const float a[4], const float b[4]) {
		const float d = std::min(std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]), 1.0f);
		return 2.0f * std::acos(d);
	};
	// Which way the stored rotations turn: the reading that puts the first frame nearer the skeleton's rest pose.
	double asIs = 0, conjugated = 0;
	for (uint32_t t = 0; t < ani.node_count; ++t) {
		const uint32_t i = ani.node_indices[t];
		if (h.nodes[i].parent_index < 0) continue;
		float rq[4];
		quatOf(fromZen(h.nodes[i].transform), rq);
		const zenkit::AnimationSample& sm = ani.samples[t];
		const float a[4] = {sm.rotation.x, sm.rotation.y, sm.rotation.z, sm.rotation.w};
		const float c[4] = {-a[0], -a[1], -a[2], a[3]};
		asIs += angleOf(a, rq);
		conjugated += angleOf(c, rq);
	}
	const float sign = conjugated <= asIs ? -1.0f : 1.0f;
	// What moves: the tracks whose last frame is not their first. The lid is the node that TURNS most (a hinge);
	// a parent that only shifts a little with it (the chest's own node) is not the lid, or the whole chest would be.
	const size_t last = size_t(ani.frame_count - 1) * ani.node_count;
	int lid = -1;
	float lidAngle = 0.0f;
	uint32_t moving = 0;
	for (uint32_t t = 0; t < ani.node_count; ++t) {
		const zenkit::AnimationSample &a = ani.samples[t], &b = ani.samples[last + t];
		const float qa[4] = {a.rotation.x, a.rotation.y, a.rotation.z, a.rotation.w}, qb[4] = {b.rotation.x, b.rotation.y, b.rotation.z, b.rotation.w};
		const float dx = b.position.x - a.position.x, dy = b.position.y - a.position.y, dz = b.position.z - a.position.z;
		const float turn = angleOf(qa, qb);
		if (turn > 0.05f || dx * dx + dy * dy + dz * dz > 2.0f * 2.0f) {
			++moving;
			const int node = int(ani.node_indices[t]);
			if (node >= 0 && size_t(node) < n && h.nodes[size_t(node)].parent_index < 0) {
				why = "the root moves";
				return false;
			}
			// (The turn first; a move without a turn only if nothing turns.)
			const float score = turn > 0.05f ? turn : 0.001f;
			if (node >= 0 && size_t(node) < n && score > lidAngle) {
				lidAngle = score;
				lid = node;
			}
		}
	}
	if (lid < 0) {
		why = "nothing moves in the animation";
		return false;
	}
	// The node and what hangs below it.
	std::set<std::string> below;
	for (size_t i = 0; i < n; ++i) {
		int at = int(i);
		for (int guard = 0; at >= 0 && guard < 64; ++guard, at = h.nodes[size_t(at)].parent_index)
			if (at == lid) {
				below.insert(upper(h.nodes[i].name));
				break;
			}
	}
	auto lidMesh = std::make_shared<FlatMesh>();
	auto baseMesh = std::make_shared<FlatMesh>(full);
	baseMesh->subs.clear();
	lidMesh->kind = baseMesh->kind = VisualKind::Model;
	// (A rigid inverse: the node carries no scale.)
	const M34 closed = rest[size_t(lid)];
	M34 inverse;
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c) inverse.m[r][c] = closed.m[c][r];
	for (int r = 0; r < 3; ++r) inverse.m[r][3] = -(inverse.m[r][0] * closed.m[0][3] + inverse.m[r][1] * closed.m[1][3] + inverse.m[r][2] * closed.m[2][3]);
	for (const FlatSub& sub : full.subs) {
		if (sub.node.empty() || !below.count(sub.node)) {
			baseMesh->subs.push_back(sub);
			continue;
		}
		FlatSub moved = sub;
		for (FlatVertex& v : moved.verts) {
			float p[3], d[3];
			inverse.point(v.pos, p);
			inverse.direction(v.normal, d);
			std::copy_n(p, 3, v.pos);
			std::copy_n(d, 3, v.normal);
		}
		lidMesh->subs.push_back(std::move(moved));
	}
	if (lidMesh->subs.empty() || baseMesh->subs.empty()) {
		why = lidMesh->subs.empty() ? "no part of the mesh hangs on the moving node" : "the lid is the whole chest";
		return false;
	}
	{
		// A lid is the smaller part: when it holds most of the chest, the node found is not the lid.
		size_t lidTris = 0, baseTris = 0;
		for (const FlatSub& sub : lidMesh->subs) lidTris += sub.verts.size() / 3;
		for (const FlatSub& sub : baseMesh->subs) baseTris += sub.verts.size() / 3;
		if (baseTris * 3 < lidTris) {
			why = "the moving node ('" + h.nodes[size_t(lid)].name + "') holds " + std::to_string(lidTris) + " of " + std::to_string(lidTris + baseTris) +
			      " triangles: not a lid";
			return false;
		}
		out.lidTriangles = lidTris;
		out.baseTriangles = baseTris;
		out.lidNode = h.nodes[size_t(lid)].name;
	}
	// The lid's last pose: its own place in the parent, from the animation's last frame.
	int track = -1;
	for (uint32_t t = 0; t < ani.node_count; ++t)
		if (int(ani.node_indices[t]) == lid) track = int(t);
	if (track < 0) {
		why = "the moving node has no track";
		return false;
	}
	const zenkit::AnimationSample& end = ani.samples[last + size_t(track)];
	const float qx = end.rotation.x * sign, qy = end.rotation.y * sign, qz = end.rotation.z * sign, qw = end.rotation.w;
	M34 local;
	local.m[0][0] = 1 - 2 * (qy * qy + qz * qz), local.m[0][1] = 2 * (qx * qy - qz * qw), local.m[0][2] = 2 * (qx * qz + qy * qw);
	local.m[1][0] = 2 * (qx * qy + qz * qw), local.m[1][1] = 1 - 2 * (qx * qx + qz * qz), local.m[1][2] = 2 * (qy * qz - qx * qw);
	local.m[2][0] = 2 * (qx * qz - qy * qw), local.m[2][1] = 2 * (qy * qz + qx * qw), local.m[2][2] = 1 - 2 * (qx * qx + qy * qy);
	local.m[0][3] = end.position.x, local.m[1][3] = end.position.y, local.m[2][3] = end.position.z;
	out.open = mul(rest[size_t(h.nodes[size_t(lid)].parent_index)], local);
	out.closed = closed;
	{
		// (It must really open: a lid that ends where it began is no lid.)
		float qc[4], qo[4];
		quatOf(out.closed, qc);
		quatOf(out.open, qo);
		const float dx = out.open.m[0][3] - closed.m[0][3], dy = out.open.m[1][3] - closed.m[1][3], dz = out.open.m[2][3] - closed.m[2][3];
		if (angleOf(qc, qo) < 0.2f && dx * dx + dy * dy + dz * dz < 5.0f * 5.0f) {
			why = "the lid ends where it began";
			return false;
		}
	}
	out.seconds = (float(ani.frame_count) - 1.0f) / (ani.fps > 0 ? ani.fps : 25.0f);
	lidMesh->finish();
	baseMesh->finish();
	out.lid = lidMesh;
	out.base = baseMesh;
	return true;
}

struct ObjectStats {
	u32 placed[size_t(VisualKind::COUNT)] = {};
	u32 noCollision = 0, particles = 0, sprites = 0, unsupported = 0;
	u64 triangles = 0;
	std::set<std::string> missing;
	std::map<std::string, std::string> unreadable; // name -> reason
	// fix157: the chests whose lids move (by visual), and why the others' do not.
	std::map<std::string, ChestLid> lids;
	std::map<std::string, std::string> lidWhy;
	// Things that give light, for the report: fire places (oCMobFire) by
	// visual, and every visual whose name says lamp, torch or fire.
	struct Fire {
		u32 count = 0;
		std::string slot, tree;
	};
	std::map<std::string, Fire> fires;
	// The effects a fire place's tree file holds (FIRETREE_*.ZEN): name and place relative to the fire.
	struct TreeEmitter {
		std::string effect;
		float pos[3];
	};
	std::map<std::string, std::vector<TreeEmitter>> trees;
	// fix148: the lights of those trees (zCVobLight: the fire's glow and flicker), places relative to the fire likewise.
	std::map<std::string, std::vector<SrcLight>> treeLights;
	// Flames to be moved to their fire place's slot once the models are read.
	struct PendingFire {
		size_t emitter; // in scene.emitters, or (light) in scene.lights
		bool light;
		float local[3];
		const zenkit::VirtualObject* vob;
		std::string visual, slot;
	};
	std::vector<PendingFire> pendingFires;
	u32 flamesLeftOut = 0; // fire places (crystal lights) whose flames are not made
	u32 emitters = 0, fireEmitters = 0, fireWithoutTree = 0;
	// fix148: lamps (zCVobLight) - of the world: static (in its compiled light), dynamic, turned off; of the fire places' trees;
	// made for fire places whose tree has none.
	u32 lampsStatic = 0, lampsDynamic = 0, lampsOff = 0, lampsOfFires = 0, lampsMade = 0, lampsAnimated = 0;
	std::map<std::string, u32> lights;
	// The things people use, with the mesh each is drawn with (null: none).
	struct Mob {
		const zenkit::VirtualObject* vob;
		int states;
		FlatPtr mesh;
	};
	std::vector<Mob> mobs;
};

bool namedLikeLight(const std::string& upperName) {
	for (const char* word : {"LAMP", "LANTERN", "LATERN", "TORCH", "FACKEL", "FIRE", "FEUER", "LIGHT", "CANDLE", "KERZE", "SCONCE"})
		if (upperName.find(word) != std::string::npos) return true;
	return false;
}


// "FIRE_SMALL.PFX" -> "FIRE_SMALL"
std::string effectName(const zenkit::VirtualObject& vob) {
	if (!vob.visual || vob.visual->name.empty()) return std::string();
	std::string n = stripExtension(upper(vob.visual->name));
	return extensionOf(upper(vob.visual->name)) == "PFX" ? n : std::string();
}

// fix148: a zCVobLight as the baker keeps it (position, range, colour, its flicker: SrcLight).
SrcLight lampOf(const zenkit::VLight& lamp, const zenkit::Vec3& at) {
	SrcLight l;
	l.pos[0] = at.x;
	l.pos[1] = at.y;
	l.pos[2] = at.z;
	l.range = lamp.range;
	l.color[0] = lamp.color.r;
	l.color[1] = lamp.color.g;
	l.color[2] = lamp.color.b;
	l.isStatic = lamp.is_static;
	if (!lamp.is_static) {
		for (float k : lamp.range_animation_scale)
			if (std::isfinite(k)) l.rangeKeys.push_back(std::clamp(k, 0.0f, 3.9f));
		l.rangeFps = std::isfinite(lamp.range_animation_fps) ? std::clamp(lamp.range_animation_fps, 0.0f, 120.0f) : 0.0f;
		l.rangeSmooth = lamp.range_animation_smooth;
		for (const zenkit::Color& c : lamp.color_animation_list) l.colorKeys.push_back({c.r, c.g, c.b});
		l.colorFps = std::isfinite(lamp.color_animation_fps) ? std::clamp(lamp.color_animation_fps, 0.0f, 120.0f) : 0.0f;
		l.colorSmooth = lamp.color_animation_smooth;
	}
	return l;
}

// The effects hanging on `vob`, their places relative to the root they belong to (the game puts the tree's
// roots at the fire's slot and keeps the others where they are to their root). fix148: and the lights (`lights`).
void treeEmitters(const zenkit::VirtualObject& vob, const zenkit::Vec3& origin, std::vector<ObjectStats::TreeEmitter>& out,
                  std::vector<SrcLight>* lights, int depth) {
	if (depth > 6) return;
	const std::string effect = effectName(vob);
	if (!effect.empty()) out.push_back({effect, {vob.position.x - origin.x, vob.position.y - origin.y, vob.position.z - origin.z}});
	if (lights && vob.type == zenkit::VirtualObjectType::zCVobLight) {
		const auto* lamp = dynamic_cast<const zenkit::VLight*>(&vob);
		// (A tree's light is the fire's own: the game loads the tree when the fire burns, so it is never compiled in.)
		if (lamp && lamp->range > 1.0f && (lamp->is_static || lamp->on)) {
			SrcLight l = lampOf(*lamp, {vob.position.x - origin.x, vob.position.y - origin.y, vob.position.z - origin.z});
			l.isStatic = false;
			l.fire = true;
			lights->push_back(l);
		}
	}
	for (const auto& child : vob.children)
		if (child) treeEmitters(*child, origin, out, lights, depth + 1);
}

// The effects of a fire place's tree file (a small world file: its top level objects are the roots), empty if it
// has none or cannot be read.
const std::vector<ObjectStats::TreeEmitter>& fireTree(const zenkit::Vfs& vfs, zenkit::GameVersion version, ObjectStats& stats,
                                                      const std::string& name) {
	auto it = stats.trees.find(name);
	if (it != stats.trees.end()) return it->second;
	std::vector<ObjectStats::TreeEmitter>& list = stats.trees[name];
	std::vector<SrcLight>& lights = stats.treeLights[name];
	if (name.empty()) return list;
	try {
		if (const zenkit::VfsNode* node = vfs.find(name)) {
			auto rd = node->open_read();
			zenkit::World tree;
			tree.load(rd.get(), version);
			for (const auto& root : tree.world_vobs)
				if (root) treeEmitters(*root, root->position, list, &lights, 0);
		}
	} catch (const std::exception&) {
		list.clear();
		lights.clear();
	}
	return list;
}

// Objects smaller than this (bounding radius, cm) are only drawn near the camera.
const float kSmallRadius = 160.0f, kMediumRadius = 450.0f;

// ---- triggers and movers ----

namespace {

void quatMul(const float a[4], const float b[4], float o[4]) {
	o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

void quatRotate(const float q[4], const float v[3], float o[3]) {
	const float p[4] = {v[0], v[1], v[2], 0.0f};
	const float c[4] = {-q[0], -q[1], -q[2], q[3]};
	float t[4], r[4];
	quatMul(q, p, t);
	quatMul(t, c, r);
	o[0] = r[0], o[1] = r[1], o[2] = r[2];
}

} // namespace

// What a trigger or mover of the world is (OpenGothic's AbstractTrigger and its kinds). True if it is one; a mover
// with a way to go (two keyframes or more) and a mesh is `dynamic`: it is drawn by the runtime, not baked in.
// A mesh's triangles for colliding with (its own space), at most `max`.
static void collisionOf(const FlatMesh& mesh, std::vector<float>& out, size_t max = 3000) {
	for (const FlatSub& sub : mesh.subs) {
		if (sub.material.noCollision || sub.material.water || sub.material.blend != SrcBlend::Opaque) continue;
		for (size_t t = 0; t + 2 < sub.verts.size() && out.size() / 9 < max; t += 3)
			for (int k = 0; k < 3; ++k)
				for (int a = 0; a < 3; ++a) out.push_back(sub.verts[t + size_t(k)].pos[a]);
	}
}

bool collectTrigger(const zenkit::VirtualObject& vob, SrcScene& scene, bool& dynamic, const std::function<FlatPtr(const std::string&)>& meshOf) {
	using T = zenkit::VirtualObjectType;
	dynamic = false;
	SrcTrigger t;
	t.name = upper(vob.vob_name);
	const zenkit::Vec3 mn = vob.bbox.min, mx = vob.bbox.max;
	const float lo[3] = {mn.x, mn.y, mn.z}, hi[3] = {mx.x, mx.y, mx.z};
	for (int a = 0; a < 3; ++a) {
		t.bbMin[a] = std::min(lo[a], hi[a]);
		t.bbMax[a] = std::max(lo[a], hi[a]);
	}
	if (vob.type == T::zCTriggerWorldStart) {
		const auto* w = dynamic_cast<const zenkit::VTriggerWorldStart*>(&vob);
		if (!w || w->target.empty()) return false;
		t.kind = TRG_WORLDSTART;
		t.target = upper(w->target);
		t.fireOnce = w->fire_once;
		scene.triggers.push_back(t);
		return true;
	}
	if (vob.type == T::oCMobDoor) {
		// A door: drawn by the runtime, turned open about the end of its leaf nearest to its origin (the hinge),
		// shut and solid at the start.
		const auto* d = dynamic_cast<const zenkit::VDoor*>(&vob);
		if (!d || !vob.show_visual || !vob.visual || vob.visual->name.empty()) return false;
		const FlatPtr mesh = meshOf(upper(vob.visual->name));
		if (!mesh || mesh->subs.empty()) return false;
		t.kind = TRG_DOOR;
		t.visual = stripExtension(vob.visual->name);
		for (char& ch : t.visual) ch = char(std::tolower(static_cast<unsigned char>(ch)));
		t.locked = d->locked;
		t.key = upper(d->key);
		t.pick = upper(d->pick_string);
		const zenkit::Vec3* c = vob.rotation.columns;
		float rot[4];
		quatFromColumns(c[0].x, c[0].y, c[0].z, c[1].x, c[1].y, c[1].z, c[2].x, c[2].y, c[2].z, rot);
		t.keys.push_back({vob.position.x, vob.position.y, vob.position.z, rot[0], rot[1], rot[2], rot[3]});
		float lo3[3] = {1e30f, 1e30f, 1e30f}, hi3[3] = {-1e30f, -1e30f, -1e30f};
		for (const FlatSub& sub : mesh->subs)
			for (const FlatVertex& v : sub.verts)
				for (int a = 0; a < 3; ++a) lo3[a] = std::min(lo3[a], v.pos[a]), hi3[a] = std::max(hi3[a], v.pos[a]);
		// The leaf lies along its longer horizontal side; the hinge is the end of it nearer the origin.
		const int axis = (hi3[0] - lo3[0]) >= (hi3[2] - lo3[2]) ? 0 : 2;
		t.pivot[0] = t.pivot[2] = 0.0f;
		t.pivot[axis] = std::fabs(lo3[axis]) < std::fabs(hi3[axis]) ? lo3[axis] : hi3[axis];
		t.pivot[2 - axis] = 0.5f * (lo3[2 - axis] + hi3[2 - axis]);
		t.pivot[1] = lo3[1];
		t.keys.push_back({t.pivot[0], t.pivot[1], t.pivot[2], 0, 0, 0, 1});
		collisionOf(*mesh, t.collision);
		t.reactTrigger = true;
		t.startEnabled = true;
		t.maxActivations = -1;
		dynamic = true;
		scene.triggers.push_back(t);
		return true;
	}
	if (vob.type == T::zCCSCamera) {
		// fix152: a camera's flight (OpenGothic's CsCamera): its keys in the world's coordinates; one flying in an
		// object's (trajectory_for OBJECT) is left out, as OpenGothic does.
		const auto* c = dynamic_cast<const zenkit::VCutsceneCamera*>(&vob);
		if (!c || c->trajectory_frames.empty() || !(c->total_duration > 0.0f) || t.name.empty()) return false;
		if (c->trajectory_for == zenkit::CameraCoordinateReference::OBJECT ||
		    c->target_trajectory_for == zenkit::CameraCoordinateReference::OBJECT) {
			++scene.cameraObjectRelative;
			return false;
		}
		t.kind = TRG_CAMERA;
		t.speed = c->total_duration;
		t.stayOpen = c->auto_untrigger_last_delay;
		t.behavior = (c->auto_untrigger_last ? 1 : 0) | (c->auto_player_movable ? 2 : 0);
		t.reactTrigger = true;
		t.startEnabled = true;
		t.sendUntrigger = true;
		t.maxActivations = -1;
		const auto add = [&](const std::vector<std::shared_ptr<zenkit::VCameraTrajectoryFrame>>& frames) {
			int n = 0;
			for (const auto& f : frames) {
				if (!f || n >= 255) continue;
				const zenkit::Mat4& m = f->original_pose;
				t.keys.push_back({m[3][0], m[3][1], m[3][2], std::max(f->time, 0.0f), float(int(f->motion_type)), 0.0f, 1.0f});
				++n;
			}
			return n;
		};
		if (add(c->trajectory_frames) == 0) return false;
		t.camTargets = add(c->target_frames);
		scene.triggers.push_back(t);
		return true;
	}
	if (vob.type != T::zCTrigger && vob.type != T::zCTriggerList && vob.type != T::oCTriggerScript && vob.type != T::zCMover)
		return false;
	const auto* tr = dynamic_cast<const zenkit::VTrigger*>(&vob);
	if (!tr) return false;
	t.target = upper(tr->target);
	t.startEnabled = tr->start_enabled;
	t.sendUntrigger = tr->send_untrigger;
	t.reactTrigger = tr->react_to_on_trigger;
	t.reactTouch = tr->react_to_on_touch;
	t.respondPc = tr->respond_to_pc;
	t.respondNpc = tr->respond_to_npc;
	t.maxActivations = tr->max_activation_count;
	t.retriggerDelay = tr->retrigger_delay_sec;
	t.fireDelay = tr->fire_delay_sec;
	if (vob.type == T::zCTrigger) {
		t.kind = TRG_TRIGGER;
	} else if (vob.type == T::zCTriggerList) {
		const auto* l = dynamic_cast<const zenkit::VTriggerList*>(&vob);
		if (!l) return false;
		t.kind = TRG_LIST;
		t.listMode = int(l->mode);
		for (const auto& x : l->targets)
			if (!x.name.empty()) t.list.emplace_back(upper(x.name), x.delay);
	} else if (vob.type == T::oCTriggerScript) {
		const auto* sc = dynamic_cast<const zenkit::VTriggerScript*>(&vob);
		if (!sc || sc->function.empty()) return false;
		t.kind = TRG_SCRIPT;
		t.target = upper(sc->function);
	} else {
		const auto* m = dynamic_cast<const zenkit::VMover*>(&vob);
		if (!m) return false;
		t.kind = TRG_MOVER;
		// (OpenGothic: a mover counts its activations only to say "never" or "always".)
		t.maxActivations = m->max_activation_count == 0 ? 0 : -1;
		t.sendUntrigger = true; // (OpenGothic leaves a mover's at its default)
		t.behavior = int(m->behavior);
		t.lerp = m->lerp_mode == zenkit::MoverLerpType::LINEAR ? 1 : 0;
		t.speedMode = int(m->speed_mode);
		t.speed = m->speed;
		t.stayOpen = m->stay_open_time_sec;
		t.autoRotate = m->auto_rotate;
		t.sfxOpenStart = upper(m->sfx_open_start);
		t.sfxOpenEnd = upper(m->sfx_open_end);
		t.sfxCloseStart = upper(m->sfx_close_start);
		t.sfxCloseEnd = upper(m->sfx_close_end);
		t.sfxMoving = upper(m->sfx_transitioning);
		const bool hasMesh = vob.show_visual && vob.visual && !vob.visual->name.empty() && extensionOf(upper(vob.visual->name)) != "PFX" &&
		                     extensionOf(upper(vob.visual->name)) != "TGA";
		if (m->keyframes.size() >= 2) {
			// The keyframes are the object's own transform at each key; the transform it is placed with may differ
			// from the first key (OpenGothic's pos0 = placed * key0^-1, applied to every key). The files'
			// rotations are read either way and the reading that puts key 0 nearer the placed rotation is taken.
			const zenkit::Vec3* c = vob.rotation.columns;
			float placedRot[4];
			quatFromColumns(c[0].x, c[0].y, c[0].z, c[1].x, c[1].y, c[1].z, c[2].x, c[2].y, c[2].z, placedRot);
			const zenkit::Quat& q0 = m->keyframes[0].rotation;
			const float conj0[4] = {-q0.x, -q0.y, -q0.z, q0.w}, plain0[4] = {q0.x, q0.y, q0.z, q0.w};
			const auto closeness = [&](const float q[4]) {
				const float d = q[0] * placedRot[0] + q[1] * placedRot[1] + q[2] * placedRot[2] + q[3] * placedRot[3];
				return std::fabs(d);
			};
			const bool conj = closeness(conj0) >= closeness(plain0);
			++(conj ? scene.moverConjugated : scene.moverPlain);
			const auto keyRot = [&](const zenkit::Quat& q, float out[4]) {
				out[0] = conj ? -q.x : q.x;
				out[1] = conj ? -q.y : q.y;
				out[2] = conj ? -q.z : q.z;
				out[3] = q.w;
			};
			float k0[4];
			keyRot(q0, k0);
			// pos0 = placed * key0^-1: its rotation and translation.
			const float k0inv[4] = {-k0[0], -k0[1], -k0[2], k0[3]};
			float r0[4];
			quatMul(placedRot, k0inv, r0);
			const float kp0[3] = {m->keyframes[0].position.x, m->keyframes[0].position.y, m->keyframes[0].position.z};
			float rk[3];
			quatRotate(r0, kp0, rk);
			const float t0[3] = {vob.position.x - rk[0], vob.position.y - rk[1], vob.position.z - rk[2]};
			for (const auto& k : m->keyframes) {
				float kr[4], wr[4], wp[3];
				keyRot(k.rotation, kr);
				quatMul(r0, kr, wr);
				const float kp[3] = {k.position.x, k.position.y, k.position.z};
				quatRotate(r0, kp, wp);
				t.keys.push_back({wp[0] + t0[0], wp[1] + t0[1], wp[2] + t0[2], wr[0], wr[1], wr[2], wr[3]});
			}
			if (hasMesh) {
				t.visual = stripExtension(vob.visual->name);
				for (char& ch : t.visual) ch = char(std::tolower(static_cast<unsigned char>(ch)));
				dynamic = true;
				// Solid, as OpenGothic's MoveTrigger: when the world file says it collides.
				if (vob.cd_static || vob.cd_dynamic)
					if (const FlatPtr mesh = meshOf(upper(vob.visual->name))) collisionOf(*mesh, t.collision);
			}
		}
	}
	// (Nothing can be sent to a trigger without a name; one that is touched still works.)
	if (t.name.empty() && t.kind != TRG_MOVER && !(t.reactTouch && (t.respondPc || t.respondNpc))) return false;
	scene.triggers.push_back(t);
	return true;
}

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
		// Triggers and movers: a mover that moves is drawn by the runtime, not baked into the chunks.
		{
			bool dynamic = false;
			const auto meshOf = [&](const std::string& visual) -> FlatPtr {
				auto it = cache.find(visual);
				if (it != cache.end()) return it->second;
				std::string why;
				FlatPtr mesh = loadVisual(vfs, visual, why);
				cache.emplace(visual, mesh);
				return mesh;
			};
			if (collectTrigger(*vob, scene, dynamic, meshOf) && dynamic) continue;
		}
		if (vob->type == zenkit::VirtualObjectType::oCMobLadder) {
			SrcLadder ladder;
			const zenkit::Vec3 mn = vob->bbox.min, mx = vob->bbox.max;
			const float lo[3] = {mn.x, mn.y, mn.z}, hi[3] = {mx.x, mx.y, mx.z};
			for (int a = 0; a < 3; ++a) {
				ladder.bbMin[a] = std::min(lo[a], hi[a]);
				ladder.bbMax[a] = std::max(lo[a], hi[a]);
			}
			// The ladder as its model has it (its line of rungs, the way it faces): with a model that can be read.
			if (vob->visual && !vob->visual->name.empty()) {
				const std::string visual = upper(vob->visual->name);
				auto it = cache.find(visual);
				if (it == cache.end()) {
					std::string why;
					it = cache.emplace(visual, loadVisual(vfs, visual, why)).first;
				}
				const FlatPtr& mesh = it->second;
				if (mesh && !mesh->subs.empty()) {
					float mlo[3] = {1e30f, 1e30f, 1e30f}, mhi[3] = {-1e30f, -1e30f, -1e30f};
					for (const FlatSub& sub : mesh->subs)
						for (const FlatVertex& v : sub.verts)
							for (int a = 0; a < 3; ++a) mlo[a] = std::min(mlo[a], v.pos[a]), mhi[a] = std::max(mhi[a], v.pos[a]);
					std::vector<std::array<float, 3>> places;
					for (const auto& [slotName, node] : mesh->slots) places.push_back({node.m[0][3], node.m[1][3], node.m[2][3]});
					const zenkit::Vec3* c = vob->rotation.columns;
					const float col[3][3] = {{c[0].x, c[0].y, c[0].z}, {c[1].x, c[1].y, c[1].z}, {c[2].x, c[2].y, c[2].z}};
					const float at[3] = {vob->position.x, vob->position.y, vob->position.z};
					ladderFromModel(ladder, mlo, mhi, reinterpret_cast<const float(*)[3]>(places.data()), places.size(), col, at);
				}
			}
			// A box without height is a file without bounds, not a ladder.
			if (ladder.bbMax[1] - ladder.bbMin[1] > 50.0f) scene.ladders.push_back(ladder);
		}
		if (vob->type == zenkit::VirtualObjectType::zCZoneZFog) {
			if (const auto* fog = dynamic_cast<const zenkit::VZoneFog*>(vob.get())) {
				SrcFogZone z;
				const zenkit::Vec3 mn = vob->bbox.min, mx = vob->bbox.max;
				const float lo[3] = {mn.x, mn.y, mn.z}, hi[3] = {mx.x, mx.y, mx.z};
				for (int a = 0; a < 3; ++a) {
					z.bbMin[a] = std::min(lo[a], hi[a]);
					z.bbMax[a] = std::max(lo[a], hi[a]);
				}
				z.range = std::max(fog->range_center, 0.0f);
				z.inner = std::clamp(fog->inner_range_percentage > 1.5f ? fog->inner_range_percentage / 100.0f : fog->inner_range_percentage, 0.0f, 1.0f);
				z.color[0] = fog->color.r;
				z.color[1] = fog->color.g;
				z.color[2] = fog->color.b;
				z.overrideColor = fog->override_color;
				z.fadeSky = fog->fade_out_sky;
				if (z.bbMax[0] > z.bbMin[0] && z.bbMax[2] > z.bbMin[2]) scene.fogZones.push_back(z);
			}
		}
		if (vob->type == zenkit::VirtualObjectType::zCZoneZFogDefault) {
			if (const auto* fog = dynamic_cast<const zenkit::VZoneFog*>(vob.get())) {
				scene.fogColor[0] = fog->color.r;
				scene.fogColor[1] = fog->color.g;
				scene.fogColor[2] = fog->color.b;
			}
		}
		if (vob->type == zenkit::VirtualObjectType::zCVobSound || vob->type == zenkit::VirtualObjectType::zCVobSoundDaytime) {
			if (const auto* snd = dynamic_cast<const zenkit::VSound*>(vob.get())) {
				SrcAmbient a;
				a.pos[0] = vob->position.x;
				a.pos[1] = vob->position.y;
				a.pos[2] = vob->position.z;
				a.radius = snd->radius > 0.0f ? snd->radius : 1500.0f;
				a.volume = snd->volume > 1.5f ? snd->volume / 100.0f : snd->volume;
				if (snd->volume <= 0.0f) a.volume = 1.0f;
				a.mode = snd->mode == zenkit::SoundMode::LOOP ? 0 : snd->mode == zenkit::SoundMode::ONCE ? 1 : 2;
				a.delayMin = std::max(snd->random_delay - snd->random_delay_var, 0.0f);
				a.delayMax = snd->random_delay + snd->random_delay_var;
				a.sound = upper(snd->sound_name);
				if (const auto* day = dynamic_cast<const zenkit::VSoundDaytime*>(vob.get())) {
					a.hourFrom = day->start_time;
					a.hourTo = day->end_time;
					scene.ambient.push_back(a);
					// The other sound plays in the hours left over.
					if (!day->sound_name2.empty()) {
						SrcAmbient b = a;
						b.sound = upper(day->sound_name2);
						b.hourFrom = day->end_time;
						b.hourTo = day->start_time;
						scene.ambient.push_back(b);
					}
				} else {
					scene.ambient.push_back(a);
				}
			}
		}
		if (vob->type == zenkit::VirtualObjectType::oCZoneMusic || vob->type == zenkit::VirtualObjectType::oCZoneMusicDefault) {
			if (const auto* zone = dynamic_cast<const zenkit::VZoneMusic*>(vob.get())) {
				SrcMusicZone z;
				const zenkit::Vec3 mn = vob->bbox.min, mx = vob->bbox.max;
				const float lo[3] = {mn.x, mn.y, mn.z}, hi[3] = {mx.x, mx.y, mx.z};
				for (int a = 0; a < 3; ++a) {
					z.bbMin[a] = std::min(lo[a], hi[a]);
					z.bbMax[a] = std::max(lo[a], hi[a]);
				}
				// The zone's name leads to its themes; what is added to it for the zone itself is taken off.
				std::string name = upper(vob->vob_name);
				for (const char* prefix : {"MUSICZONE_", "MUSIC_", "ZONE_"})
					if (name.rfind(prefix, 0) == 0 && name.size() > std::strlen(prefix)) name = name.substr(std::strlen(prefix));
				for (const char* suffix : {"_MUSICZONE", "_MUSIC", "_ZONE"})
					if (name.size() > std::strlen(suffix) && name.compare(name.size() - std::strlen(suffix), std::string::npos, suffix) == 0)
						name.resize(name.size() - std::strlen(suffix));
				z.name = name;
				z.priority = zone->priority;
				z.ellipsoid = zone->ellipsoid;
				z.isDefault = vob->type == zenkit::VirtualObjectType::oCZoneMusicDefault;
				z.volume = zone->volume > 1.5f ? std::min(zone->volume / 100.0f, 1.0f) : (zone->volume > 0.0f ? zone->volume : 1.0f);
				if (!z.name.empty() || z.isDefault) scene.musicZones.push_back(z);
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

		// A light: kept for the baker's light (light_bake.h).
		if (vob->type == zenkit::VirtualObjectType::zCVobLight) {
			if (const auto* lamp = dynamic_cast<const zenkit::VLight*>(vob.get()); lamp && lamp->range > 1.0f) {
				// (fix148: a dynamic light that is turned off gives none; its flicker is kept for the runtime's lamps.)
				if (!lamp->is_static && !lamp->on) {
					++stats.lampsOff;
				} else {
					SrcLight l = lampOf(*lamp, vob->position);
					++(l.isStatic ? stats.lampsStatic : stats.lampsDynamic);
					stats.lampsAnimated += l.rangeKeys.size() > 1 || l.colorKeys.size() > 1;
					scene.lights.push_back(l);
				}
			}
			continue;
		}
		// A thing lying about: which (the script instance), where, how turned.
		if (vob->type == zenkit::VirtualObjectType::oCItem) {
			if (const auto* item = dynamic_cast<const zenkit::VItem*>(vob.get()); item && !item->instance.empty()) {
				SrcWorldItem wi;
				wi.instance = upper(item->instance);
				wi.pos[0] = vob->position.x;
				wi.pos[1] = vob->position.y;
				wi.pos[2] = vob->position.z;
				const zenkit::Vec3* c = vob->rotation.columns;
				quatFromColumns(c[0].x, c[0].y, c[0].z, c[1].x, c[1].y, c[1].z, c[2].x, c[2].y, c[2].z, wi.rot);
				scene.items.push_back(wi);
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
			// The flames and smoke of the fire place: its tree file's effects, set where the place stands.
			{
				const zenkit::Vec3* c = vob->rotation.columns;
				const float yaw = std::atan2(c[2].x, c[2].z);
				const std::string treeName = fire ? upper(fire->vob_tree) : std::string();
				const std::vector<ObjectStats::TreeEmitter>& tree = fireTree(vfs, version, stats, treeName);
				// The crystal lights of the camps (the ore lamps on their poles: PC_CRYSTALLIGHT) glow, they do not burn: their
				// tree has flames the game hides under the crystal, and here they would stand above it. The light stays.
				const bool glowsOnly = visual.find("CRYSTALLIGHT") != std::string::npos;
				if (glowsOnly) ++stats.flamesLeftOut;
				for (const ObjectStats::TreeEmitter& te : tree) {
					if (glowsOnly) break;
					SrcEmitter e;
					e.effect = te.effect;
					e.yaw = yaw;
					e.pos[0] = vob->position.x + c[0].x * te.pos[0] + c[1].x * te.pos[1] + c[2].x * te.pos[2];
					e.pos[1] = vob->position.y + c[0].y * te.pos[0] + c[1].y * te.pos[1] + c[2].y * te.pos[2];
					e.pos[2] = vob->position.z + c[0].z * te.pos[0] + c[1].z * te.pos[1] + c[2].z * te.pos[2];
					if (!fire || fire->slot.empty() || !vob->visual || vob->visual->name.empty()) {
						// (no slot to hang on: at the object's origin)
					} else {
						stats.pendingFires.push_back({scene.emitters.size(), false, {te.pos[0], te.pos[1], te.pos[2]}, vob.get(),
						                              upper(vob->visual->name), upper(fire->slot)});
					}
					scene.emitters.push_back(e);
					++stats.fireEmitters;
				}
				// fix148: the tree's lights, placed as its effects are (on the slot's node once the models are read).
				const std::vector<SrcLight>& treeLights = stats.treeLights[treeName];
				for (const SrcLight& tl : treeLights) {
					SrcLight l = tl;
					for (int a = 0; a < 3; ++a) {
						const float* col[3] = {&c[0].x, &c[1].x, &c[2].x};
						l.pos[a] = (&vob->position.x)[a] + col[0][a] * tl.pos[0] + col[1][a] * tl.pos[1] + col[2][a] * tl.pos[2];
					}
					if (fire && !fire->slot.empty() && vob->visual && !vob->visual->name.empty())
						stats.pendingFires.push_back({scene.lights.size(), true, {tl.pos[0], tl.pos[1], tl.pos[2]}, vob.get(),
						                              upper(vob->visual->name), upper(fire->slot)});
					stats.lampsAnimated += l.rangeKeys.size() > 1 || l.colorKeys.size() > 1;
					scene.lights.push_back(l);
					++stats.lampsOfFires;
				}
				// A fire whose tree burns (it has flames) but has no light: one of a camp fire's warmth, flickering, a little
				// over the flames. (ZenGin would give it none; the game's fire places all have one where we know them.)
				if (treeLights.empty() && !tree.empty()) {
					float sum[3] = {0, 0, 0};
					for (const ObjectStats::TreeEmitter& te : tree)
						for (int a = 0; a < 3; ++a) sum[a] += te.pos[a] / float(tree.size());
					SrcLight l;
					for (int a = 0; a < 3; ++a) {
						const float* col[3] = {&c[0].x, &c[1].x, &c[2].x};
						l.pos[a] = (&vob->position.x)[a] + col[0][a] * sum[0] + col[1][a] * sum[1] + col[2][a] * sum[2] + (a == 1 ? 40.0f : 0.0f);
					}
					l.range = 900.0f;
					l.color[0] = 255;
					l.color[1] = 170;
					l.color[2] = 90;
					l.fire = l.made = true;
					if (fire && !fire->slot.empty() && vob->visual && !vob->visual->name.empty())
						stats.pendingFires.push_back({scene.lights.size(), true, {sum[0], sum[1] + 40.0f, sum[2]}, vob.get(),
						                              upper(vob->visual->name), upper(fire->slot)});
					scene.lights.push_back(l);
					++stats.lampsMade;
				}
				if (tree.empty()) {
					// No tree file to be read: a plain fire over the place.
					SrcEmitter e;
					e.effect = "FIRE";
					e.yaw = yaw;
					e.pos[0] = vob->position.x;
					e.pos[1] = vob->position.y + 30.0f;
					e.pos[2] = vob->position.z;
					scene.emitters.push_back(e);
					++stats.fireEmitters;
					++stats.fireWithoutTree;
				}
			}
		}
		// Something to use: a bed, a bench, an anvil. (Ladders are climbed, which is the player's business.)
		const auto* usable = vob->type == zenkit::VirtualObjectType::oCMobLadder ? nullptr
		                                                                          : dynamic_cast<const zenkit::VInteractiveObject*>(vob.get());
		const bool hasName = vob->visual && !vob->visual->name.empty();
		if (usable && hasName && !vob->show_visual) stats.mobs.push_back({vob.get(), usable->state_count, nullptr});
		if (!vob->show_visual || !hasName) continue;
		const std::string name = upper(vob->visual->name);
		const std::string ext = extensionOf(name);
		if (namedLikeLight(name)) ++stats.lights[name];

		if (ext == "PFX") {
			++stats.particles;
			SrcEmitter e;
			e.effect = stripExtension(name);
			e.pos[0] = vob->position.x;
			e.pos[1] = vob->position.y;
			e.pos[2] = vob->position.z;
			e.yaw = std::atan2(vob->rotation.columns[2].x, vob->rotation.columns[2].z);
			scene.emitters.push_back(e);
			++stats.emitters;
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
		if (usable) stats.mobs.push_back({vob.get(), usable->state_count, it->second});
		if (!it->second) continue;
		FlatPtr drawn = it->second;
		if (vob->type == zenkit::VirtualObjectType::oCMobContainer && it->second->kind == VisualKind::Model) {
			// (fix157) A chest whose lid can move is drawn without it: the lid is a mover (appendObjects).
			auto lid = stats.lids.find(name);
			if (lid == stats.lids.end() && !stats.lidWhy.count(name)) {
				ChestLid cl;
				std::string why;
				if (chestLidOf(vfs, name, *it->second, cl, why)) lid = stats.lids.emplace(name, std::move(cl)).first;
				else stats.lidWhy[name] = why;
			}
			if (lid != stats.lids.end()) drawn = lid->second.base;
		}
		out.push_back({vob.get(), drawn});
	}
}

void appendObjects(const zenkit::World& world, const zenkit::Vfs& vfs, zenkit::GameVersion version, const ZenImportOptions& opt,
                   SrcScene& scene) {
	std::map<std::string, FlatPtr> cache;
	std::vector<ObjectInstance> instances;
	ObjectStats stats;
	collectObjects(world.world_vobs, vfs, version, cache, instances, stats, scene);

	// Flames hang on the slot node of the fire place's model (the game: `mapBone(slot)` for the fire tree), not
	// on the object's origin: a torch burns at its tip.
	u32 onNode = 0, offNode = 0;
	std::set<std::string> offKinds;
	for (const ObjectStats::PendingFire& pf : stats.pendingFires) {
		const auto mesh = cache.find(pf.visual);
		const auto node = mesh == cache.end() || !mesh->second ? decltype(mesh->second->nodes.end())() : mesh->second->nodes.find(pf.slot);
		if (mesh == cache.end() || !mesh->second || node == mesh->second->nodes.end()) {
			++offNode;
			if (offKinds.size() < 6) offKinds.insert(pf.visual + " at " + pf.slot);
			continue;
		}
		++onNode;
		float inNode[3];
		node->second.point(pf.local, inNode);
		const zenkit::Vec3* c = pf.vob->rotation.columns;
		float* pos = pf.light ? scene.lights[pf.emitter].pos : scene.emitters[pf.emitter].pos;
		pos[0] = pf.vob->position.x + c[0].x * inNode[0] + c[1].x * inNode[1] + c[2].x * inNode[2];
		pos[1] = pf.vob->position.y + c[0].y * inNode[0] + c[1].y * inNode[1] + c[2].y * inNode[2];
		pos[2] = pf.vob->position.z + c[0].z * inNode[0] + c[1].z * inNode[1] + c[2].z * inNode[2];
	}
	if (stats.flamesLeftOut) scene.notes.push_back("crystal lights (ore lamps): " + std::to_string(stats.flamesLeftOut) + " without a flame above them");
	if (onNode || offNode) {
		std::string list;
		for (const std::string& k : offKinds) list += (list.empty() ? "" : "; ") + k;
		scene.notes.push_back("fire flames and lights: " + std::to_string(onNode) + " hung on their place's node, " + std::to_string(offNode) +
		                      " on the object's origin (no such node in its model)" + (list.empty() ? "" : ": " + list));
	}

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

	// The things people use: what kind each is, and where one stands to use it.
	std::map<std::string, std::pair<std::string, int>> slotNotes;
	u32 lidsMade = 0;
	for (const ObjectStats::Mob& mob : stats.mobs) {
		const zenkit::VirtualObject& vob = *mob.vob;
		const zenkit::Vec3* col = vob.rotation.columns;
		SrcMob out;
		out.visual = upper(vob.visual->name);
		// The kind is the first word of the model's name: "BEDHIGH_PSI.ASC" is a "BEDHIGH".
		const size_t cut = out.visual.find('_') != std::string::npos ? out.visual.find('_') : out.visual.rfind('.');
		out.scheme = out.visual.substr(0, cut);
		if (out.scheme.empty() || out.scheme.size() > 47) continue;
		out.pos[0] = vob.position.x;
		out.pos[1] = vob.position.y;
		out.pos[2] = vob.position.z;
		out.yaw = std::atan2(col[2].x, col[2].z);
		out.states = std::clamp(mob.states, 0, 16);
		if (const auto* inter = dynamic_cast<const zenkit::VInteractiveObject*>(&vob)) {
			out.focus = upper(inter->name);
			out.onState = upper(inter->on_state_change_function);
			out.condition = upper(inter->condition_function);
			out.useItem = upper(inter->item);
			out.target = inter->target;
			out.rewind = inter->rewind;
			out.owner = upper(inter->owner);
			out.ownerGuild = upper(inter->owner_guild);
			using T = zenkit::VirtualObjectType;
			out.kind = vob.type == T::oCMobBed ? MOBKIND_BED
			           : vob.type == T::oCMobFire ? MOBKIND_FIRE
			           : vob.type == T::oCMobSwitch ? MOBKIND_SWITCH
			           : vob.type == T::oCMobWheel ? MOBKIND_WHEEL
			           : vob.type == T::oCMobContainer ? MOBKIND_CONTAINER
			           : vob.type == T::oCMobDoor ? MOBKIND_DOOR
			           : vob.type == T::oCMobInter ? MOBKIND_INTER
			                                       : MOBKIND_OTHER;
		}
		if (const auto* box = dynamic_cast<const zenkit::VContainer*>(&vob)) {
			out.container = true;
			out.locked = box->locked;
			out.key = upper(box->key);
			out.pick = upper(box->pick_string);
			out.contents = upper(box->contents);
			out.vobName = upper(vob.vob_name);
		}
		if (mob.mesh)
			for (const auto& [name, node] : mob.mesh->slots) {
				SrcMobSlot slot;
				slot.name = name;
				const float origin[3] = {0, 0, 0}, ahead[3] = {0, 0, 1};
				float p[3], d[3];
				node.point(origin, p);
				node.direction(ahead, d);
				slot.pos[0] = col[0].x * p[0] + col[1].x * p[1] + col[2].x * p[2] + vob.position.x;
				slot.pos[1] = col[0].y * p[0] + col[1].y * p[1] + col[2].y * p[2] + vob.position.y;
				slot.pos[2] = col[0].z * p[0] + col[1].z * p[1] + col[2].z * p[2] + vob.position.z;
				const float wx = col[0].x * d[0] + col[1].x * d[1] + col[2].x * d[2];
				const float wz = col[0].z * d[0] + col[1].z * d[1] + col[2].z * d[2];
				slot.yaw = std::atan2(wx, wz);
				// One stands on the floor there (the world's, not on the thing
				// itself). The node is where the middle of the body is: about a
				// metre up, and on things with a deep footing two (the game's
				// stone mill: 149 cm above the object's origin).
				u8 light[4];
				float h = 0;
				const float nodeY = slot.pos[1];
				const bool found = ground.sample(slot.pos[0], slot.pos[1] + 60.0f, slot.pos[2], light, &h) && h > slot.pos[1] - 350.0f;
				if (found) slot.pos[1] = h;
				// For the report, once per kind: where the places are on the
				// thing (its model space: + is right, up, ahead), which way one
				// faces there compared with the way to the thing's middle (0:
				// facing it, 180: the back to it), and how far down the ground is.
				if (!slotNotes.count(out.scheme) || slotNotes[out.scheme].second < 3) {
					auto& note = slotNotes[out.scheme];
					if (note.first.empty()) note.first = out.visual + ":";
					float rel = (std::atan2(d[0], d[2]) - std::atan2(-p[0], -p[2])) * 57.29578f;
					while (rel > 180.0f) rel -= 360.0f;
					while (rel < -180.0f) rel += 360.0f;
					char text[192];
					std::snprintf(text, sizeof(text), " %s at %.0f %.0f %.0f facing %+.0f, %s", name.c_str(), double(p[0]), double(p[1]),
					              double(p[2]), double(rel),
					              found ? (std::to_string(int(std::lround(nodeY - h))) + " cm above the ground").c_str()
					                    : "no ground of the world below");
					note.first += (note.second ? ";" : "") + std::string(text);
					++note.second;
				}
				out.slots.push_back(slot);
			}
		if (out.container && vob.visual) {
			const auto lid = stats.lids.find(upper(vob.visual->name));
			if (lid != stats.lids.end()) {
				// (fix157) The lid: a mover named for the chest's place in the things of the world, turned between
				// the model's shut and open poses, placed as the chest is.
				SrcTrigger t;
				t.kind = TRG_MOVER;
				t.name = "G3CHEST" + std::to_string(scene.mobs.size());
				t.visual = stripExtension(vob.visual->name);
				for (char& ch : t.visual) ch = char(std::tolower(static_cast<unsigned char>(ch)));
				t.visual += "_g3lid";
				const float R[3][3] = {{col[0].x, col[1].x, col[2].x}, {col[0].y, col[1].y, col[2].y}, {col[0].z, col[1].z, col[2].z}};
				const float at[3] = {vob.position.x, vob.position.y, vob.position.z};
				const auto place = [&](const M34& m, std::array<float, 7>& key) {
					float W[3][3];
					for (int r = 0; r < 3; ++r)
						for (int c = 0; c < 3; ++c) W[r][c] = R[r][0] * m.m[0][c] + R[r][1] * m.m[1][c] + R[r][2] * m.m[2][c];
					for (int r = 0; r < 3; ++r) key[size_t(r)] = R[r][0] * m.m[0][3] + R[r][1] * m.m[1][3] + R[r][2] * m.m[2][3] + at[r];
					float q[4];
					quatFromColumns(W[0][0], W[1][0], W[2][0], W[0][1], W[1][1], W[2][1], W[0][2], W[1][2], W[2][2], q);
					for (int k = 0; k < 4; ++k) key[size_t(3 + k)] = q[k];
				};
				std::array<float, 7> shut, open;
				place(lid->second.closed, shut);
				place(lid->second.open, open);
				float d = 0;
				for (int k = 0; k < 4; ++k) d += shut[size_t(3 + k)] * open[size_t(3 + k)];
				if (d < 0)
					for (int k = 0; k < 4; ++k) open[size_t(3 + k)] = -open[size_t(3 + k)];
				// (It turns about its own node: the same place both times, unless the animation moves the node.)
				const float move = std::sqrt((open[0] - shut[0]) * (open[0] - shut[0]) + (open[1] - shut[1]) * (open[1] - shut[1]) +
				                             (open[2] - shut[2]) * (open[2] - shut[2]));
				if (move < 2.0f)
					for (int k = 0; k < 3; ++k) open[size_t(k)] = shut[size_t(k)];
				const float turn = 2.0f * std::acos(std::min(std::fabs(d), 1.0f)) * 57.29578f; // degrees
				t.keys.push_back(shut);
				t.keys.push_back(open);
				t.behavior = 1; // trigger control: a trigger opens it, an untrigger shuts it
				t.lerp = 1;
				t.speed = move >= 2.0f ? move / std::max(lid->second.seconds, 0.3f) : std::max(turn, 1.0f) / (360.0f * std::max(lid->second.seconds, 0.3f));
				t.reactTrigger = true;
				t.startEnabled = true;
				t.sendUntrigger = true;
				t.maxActivations = -1;
				const float lo[3] = {vob.bbox.min.x, vob.bbox.min.y, vob.bbox.min.z}, hi[3] = {vob.bbox.max.x, vob.bbox.max.y, vob.bbox.max.z};
				for (int a = 0; a < 3; ++a) {
					t.bbMin[a] = std::min(lo[a], hi[a]);
					t.bbMax[a] = std::max(lo[a], hi[a]);
				}
				scene.triggers.push_back(t);
				++lidsMade;
			}
		}
		scene.mobs.push_back(std::move(out));
	}
	if (lidsMade || !stats.lidWhy.empty()) {
		std::string why;
		std::map<std::string, u32> reasons;
		for (const auto& kv : stats.lidWhy) ++reasons[kv.second];
		for (const auto& kv : reasons) why += (why.empty() ? "" : "; ") + kv.first + " x" + std::to_string(kv.second);
		scene.notes.push_back("chest lids: " + std::to_string(lidsMade) + " chests have a lid that opens (" + std::to_string(stats.lids.size()) +
		                      " kinds); " + std::to_string(stats.lidWhy.size()) + " kinds of chest stay whole" + (why.empty() ? "" : " (" + why + ")"));
		std::string kinds;
		for (const auto& kv : stats.lids)
			kinds += "\n  " + kv.first + ": lid '" + kv.second.lidNode + "' " + std::to_string(kv.second.lidTriangles) + " triangles, the rest " +
			         std::to_string(kv.second.baseTriangles) + ", " + std::to_string(int(kv.second.seconds * 100.0f) / 100.0) + " s";
		if (!kinds.empty()) scene.notes.push_back("chest lids by kind:" + kinds);
	}
	if (!slotNotes.empty()) {
		std::string all;
		for (const auto& [scheme, note] : slotNotes) all += "\n  " + scheme + " " + note.first;
		scene.notes.push_back("places to stand at, the first thing of each kind:" + all);
	}

	std::map<std::string, u32> materialIndex; // everything that affects drawing -> scene material
	u32 unlit = 0;
	u64 detailCount[DETAIL_COUNT] = {0, 0, 0};
	// For the report: what the objects cost, by size band (small ones) and mesh by mesh.
	u64 bandObjects[SIZE_BANDS] = {0, 0, 0, 0}, bandTriangles[SIZE_BANDS] = {0, 0, 0, 0}, classTriangles[DETAIL_COUNT] = {0, 0, 0};
	struct MeshCost {
		u64 copies = 0;
		size_t triangles = 0;
		float radius = 0;
		u8 detail = 0;
	};
	std::map<std::string, MeshCost> meshCost;
	struct PlaceCheck {
		u32 copies = 0, off = 0;
		float worst = 0.0f, at[3] = {0, 0, 0};
	};
	std::map<std::string, PlaceCheck> placeChecks;
	std::map<std::string, u32> mirroredCopies;
	// Plants (vertex_fx.h): by the object's name, else by a part's material or texture.
	std::map<std::string, u32> plantsByName, plantsByPart;
	u32 plantKinds[4] = {0, 0, 0, 0};
	for (const ObjectInstance& inst : instances) {
		const zenkit::VirtualObject& vob = *inst.vob;
		const size_t firstTriangle = scene.triangles.size();
		const std::string visualName = vob.visual ? upper(vob.visual->name) : std::string();
		const PlantKind objectKind = inst.mesh->kind == VisualKind::Decal ? PlantKind::None : plantKindOf(visualName, opt.plantWords);
		std::vector<std::pair<size_t, PlantKind>> partKinds; // (first triangle, kind) of each part
		const zenkit::Vec3* col = vob.rotation.columns; // object -> world: p' = R * p + position
		const bool decal = inst.mesh->kind == VisualKind::Decal;
		// A mirrored placement (negative determinant) turns every triangle inside out: its winding is swapped back.
		const float det = col[0].x * (col[1].y * col[2].z - col[2].y * col[1].z) - col[1].x * (col[0].y * col[2].z - col[2].y * col[0].z) +
		                  col[2].x * (col[0].y * col[1].z - col[1].y * col[0].z);
		const bool mirrored = det < 0.0f;
		if (mirrored && !decal) ++mirroredCopies[visualName];

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
		// Small things are told apart in four thinning layers (by where they stand, so that every
		// baking gives the same picture): the menu may leave the last ones out (World::StreamConfig::smallLayers).
		const u32 spot = u32(int(vob.position.x)) * 73856093u ^ u32(int(vob.position.y)) * 19349663u ^ u32(int(vob.position.z)) * 83492791u;
		// ... and by how big they are (size bands: the smaller, the nearer they are drawn; g3ds/world.h sizeBandOf).
		const u8 band = detail == DETAIL_SMALL ? sizeBandOf(inst.mesh->radius) : u8(0);
		// (fix162: a chest whose lid is a mover is never thinned out: the lid is always drawn, so the body must be too.)
		const bool lidded = vob.type == zenkit::VirtualObjectType::oCMobContainer && stats.lids.count(visualName) != 0;
		const u8 layered = detail == DETAIL_SMALL ? u8(detail | ((lidded ? 0u : ((spot >> 9) & 3u)) << 2) | (band << 4)) : detail;
		{
			size_t tris = 0;
			for (const FlatSub& sub : inst.mesh->subs) tris += sub.verts.size() / 3;
			classTriangles[detail] += tris;
			if (detail == DETAIL_SMALL) {
				++bandObjects[band];
				bandTriangles[band] += tris;
			}
			MeshCost& mc = meshCost[visualName.empty() ? inst.mesh->source : visualName];
			++mc.copies;
			mc.triangles = tris;
			mc.radius = inst.mesh->radius;
			mc.detail = detail;
		}

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
			PlantKind partKind = objectKind;
			if (partKind == PlantKind::None && !decal && !sm.water && sm.blend == SrcBlend::Opaque) {
				partKind = plantKindOf(sm.name, opt.plantWords);
				if (partKind == PlantKind::None) partKind = plantKindOf(sm.texture, opt.plantWords);
			}
			partKinds.emplace_back(scene.triangles.size(), partKind);

			for (size_t t = 0; t + 2 < sub.verts.size(); t += 3) {
				SrcTriangle tri;
				tri.material = it->second;
				tri.detail = layered;
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
				if ((flip != mirrored) && !decal) std::swap(tri.v[1], tri.v[2]);
				scene.triangles.push_back(tri);
				++stats.triangles;
			}
		}
		++stats.placed[size_t(inst.mesh->kind)];

		// Check against the world's own bounding box of the object: where the baked triangles ended up and where the
		// file says the object is (a bed that is placed outside its house shows up here, with its name).
		if (!decal && scene.triangles.size() > firstTriangle) {
			float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
			for (size_t t = firstTriangle; t < scene.triangles.size(); ++t)
				for (const SrcVertex& v : scene.triangles[t].v)
					for (int c = 0; c < 3; ++c) {
						lo[c] = std::min(lo[c], v.pos[c]);
						hi[c] = std::max(hi[c], v.pos[c]);
					}
			const float want[6] = {vob.bbox.min.x, vob.bbox.min.y, vob.bbox.min.z, vob.bbox.max.x, vob.bbox.max.y, vob.bbox.max.z};
			const float size = std::max({want[3] - want[0], want[4] - want[1], want[5] - want[2]});
			float off = 0.0f;
			for (int c = 0; c < 3; ++c) off = std::max(off, std::max(std::fabs(0.5f * (lo[c] + hi[c]) - 0.5f * (want[c] + want[c + 3])), 0.0f));
			if (size > 1.0f && size < 100000.0f) {
				PlaceCheck& pc = placeChecks[visualName];
				++pc.copies;
				if (off > std::max(80.0f, 0.4f * size)) {
					++pc.off;
					if (off > pc.worst) {
						pc.worst = off;
						pc.at[0] = vob.position.x;
						pc.at[1] = vob.position.y;
						pc.at[2] = vob.position.z;
					}
				}
			}
		}

		// Swaying: measured over the whole object, so that a tree's leaves near
		// the top move most even if only they are named as leaves.
		bool anyPlant = false;
		for (const auto& pk : partKinds) anyPlant = anyPlant || pk.second != PlantKind::None;
		if (anyPlant && scene.triangles.size() > firstTriangle) {
			float footY = 1e30f, topY = -1e30f;
			for (size_t t = firstTriangle; t < scene.triangles.size(); ++t)
				for (const SrcVertex& v : scene.triangles[t].v) {
					footY = std::min(footY, v.pos[1]);
					topY = std::max(topY, v.pos[1]);
				}
			PlantKind shown = objectKind;
			for (size_t p = 0; p < partKinds.size(); ++p) {
				if (partKinds[p].second == PlantKind::None) continue;
				const size_t end = p + 1 < partKinds.size() ? partKinds[p + 1].first : scene.triangles.size();
				markSway(scene.triangles.data() + partKinds[p].first, end - partKinds[p].first, partKinds[p].second, footY, topY,
				         plantSway(partKinds[p].second, topY - footY));
				if (shown == PlantKind::None) shown = partKinds[p].second;
			}
			++plantKinds[size_t(shown)];
			SrcPlant plant;
			plant.pos[0] = vob.position.x;
			plant.pos[1] = vob.position.y;
			plant.pos[2] = vob.position.z;
			plant.footY = footY;
			plant.topY = topY;
			plant.radius = inst.mesh->radius;
			plant.kind = u8(shown);
			plant.name = visualName;
			scene.plants.push_back(std::move(plant));
			++(objectKind != PlantKind::None ? plantsByName : plantsByPart)[visualName.empty() ? std::string("?") : visualName];
		}
	}
	if (!plantsByName.empty() || !plantsByPart.empty()) {
		auto top = [](const std::map<std::string, u32>& m) {
			std::vector<std::pair<u32, std::string>> v;
			for (const auto& kv : m) v.emplace_back(kv.second, kv.first);
			std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
			std::string out;
			for (size_t i = 0; i < v.size() && i < 12; ++i) out += (i ? ", " : "") + v[i].second + " x" + std::to_string(v[i].first);
			if (v.size() > 12) out += ", ... (" + std::to_string(v.size()) + " kinds)";
			return out;
		};
		scene.notes.push_back("plants that sway (kept with --wind): " + std::to_string(plantKinds[1]) + " trees, " +
		                      std::to_string(plantKinds[2]) + " bushes, " + std::to_string(plantKinds[3]) + " grasses" +
		                      (plantsByName.empty() ? "" : "\n  by the object's name: " + top(plantsByName)) +
		                      (plantsByPart.empty() ? "" : "\n  by a part's material or texture: " + top(plantsByPart)));
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
	{
		u32 total = 0;
		std::string list;
		for (const auto& kv : mirroredCopies) {
			total += kv.second;
			if (list.size() < 400) list += (list.empty() ? "" : ", ") + kv.first + " x" + std::to_string(kv.second);
		}
		scene.notes.push_back("mirrored placements (negative determinant, winding corrected): " + std::to_string(total) + " copies of " +
		                      std::to_string(mirroredCopies.size()) + " visuals" + (list.empty() ? "" : ": " + list));
	}
	{
		std::vector<std::pair<std::string, PlaceCheck>> bad;
		u32 badCopies = 0;
		for (const auto& kv : placeChecks)
			if (kv.second.off) {
				bad.push_back(kv);
				badCopies += kv.second.off;
			}
		std::sort(bad.begin(), bad.end(), [](const auto& a, const auto& b) { return a.second.off > b.second.off; });
		scene.notes.push_back("placement check: " + std::to_string(badCopies) + " object copies lie away from the bounding box the world file gives them, " +
		                      std::to_string(bad.size()) + " visuals");
		for (size_t i = 0; i < bad.size() && i < 25; ++i)
			scene.notes.push_back("  off " + bad[i].first + ": " + std::to_string(bad[i].second.off) + " of " + std::to_string(bad[i].second.copies) +
			                      " copies, worst " + std::to_string(int(bad[i].second.worst)) + " cm at " + std::to_string(int(bad[i].second.at[0])) + " " +
			                      std::to_string(int(bad[i].second.at[1])) + " " + std::to_string(int(bad[i].second.at[2])));
		for (const auto& kv : placeChecks)
			if (kv.first.find("BED") != std::string::npos)
				scene.notes.push_back("  bed " + kv.first + ": " + std::to_string(kv.second.copies) + " copies, " + std::to_string(kv.second.off) + " away from its box");
	}
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
	{
		auto num = [](u64 v) { return std::to_string(v); };
		scene.notes.push_back("object triangles by class (before --simplify): " + num(classTriangles[DETAIL_ALWAYS]) + " large, " +
		                      num(classTriangles[DETAIL_MEDIUM]) + " medium, " + num(classTriangles[DETAIL_SMALL]) + " small");
		static const char* kBand[SIZE_BANDS] = {"60 cm and more", "30-60 cm", "15-30 cm", "under 15 cm"};
		std::string bands = "small objects by size (radius; band: objects, triangles): ";
		for (u32 b = 0; b < SIZE_BANDS; ++b)
			bands += std::string(b ? "; " : "") + kBand[b] + ": " + num(bandObjects[b]) + ", " + num(bandTriangles[b]);
		scene.notes.push_back(bands + " (bands 1-3 are drawn only as far as they are a few pixels across)");
		// The meshes that cost the most, triangles times copies: what is worth simplifying or swapping first.
		static const char* kClass[DETAIL_COUNT] = {"large", "medium", "small"};
		for (u8 cls : {u8(DETAIL_SMALL), u8(DETAIL_MEDIUM), u8(DETAIL_ALWAYS)}) {
			std::vector<std::pair<u64, std::string>> list;
			for (const auto& [name, mc] : meshCost)
				if (mc.detail == cls) list.emplace_back(u64(mc.triangles) * mc.copies, name);
			if (list.empty()) continue;
			std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
			std::string line = std::string("heaviest ") + kClass[cls] + " objects (triangles x copies = all, radius):";
			for (size_t i = 0; i < list.size() && i < 20; ++i) {
				const MeshCost& mc = meshCost[list[i].second];
				char buf[200];
				std::snprintf(buf, sizeof(buf), "\n  %s %zu x %llu = %llu, %.0f cm", list[i].second.c_str(), mc.triangles,
				              static_cast<unsigned long long>(mc.copies), static_cast<unsigned long long>(list[i].first), double(mc.radius));
				line += buf;
			}
			scene.notes.push_back(line);
		}
	}
	if (stats.emitters || stats.fireEmitters)
		scene.notes.push_back("particle effects: " + std::to_string(stats.emitters) + " placed by the world, " +
		                      std::to_string(stats.fireEmitters) + " from fire places' trees" +
		                      (stats.fireWithoutTree ? " (" + std::to_string(stats.fireWithoutTree) + " fire places without a readable tree: a plain FIRE)" : std::string()));
	if (stats.lampsStatic || stats.lampsDynamic || stats.lampsOff || stats.lampsOfFires || stats.lampsMade)
		scene.notes.push_back("lamps (zCVobLight): " + std::to_string(stats.lampsStatic) + " static (in the world's compiled light), " +
		                      std::to_string(stats.lampsDynamic) + " dynamic, " + std::to_string(stats.lampsOff) + " dynamic turned off (left out); " +
		                      std::to_string(stats.lampsOfFires) + " from fire places' trees, " + std::to_string(stats.lampsMade) +
		                      " made for fire places whose tree has none; " + std::to_string(stats.lampsAnimated) + " flicker by keys");
	if (stats.sprites || stats.unsupported)
		scene.notes.push_back("objects skipped (not supported yet): " + std::to_string(stats.sprites) + " camera-facing sprites, " +
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
	ZenModInfo mod; // the modification mounted over the game, if any
	// Which side of a character mesh's triangles is the front: +1 as stored,
	// -1 the other, 0 not known yet. All of a game's character meshes come out
	// of the same exporter, so what has been found for one whole body holds
	// for every armour and head (which, open as they are, say little alone).
	mutable int actorWinding = 0;
};

namespace {

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

// A file of the game's scripts: loose under _work/Data/<dirs>/ in the installation, or in an archive. A mod's
// files are meant to lie over the game's, so with a mod mounted the archives are asked first.
struct GameFile {
	std::unique_ptr<zenkit::Read> rd;
	fs::path loose; // set if it came from a loose file
};

GameFile openGameFile(const ZenGame& game, const std::vector<std::string>& dirs, const std::string& file) {
	GameFile out;
	auto fromArchive = [&] {
		if (const zenkit::VfsNode* node = game.vfs.find(file)) out.rd = node->open_read();
	};
	auto fromDisk = [&] {
		fs::path p = game.dir;
		for (const std::string& part : dirs) {
			p = findNoCase(p, {part.c_str()});
			if (p.empty()) return;
		}
		const std::string lowerFile = lower(file);
		p = findNoCase(p, {lowerFile.c_str()});
		if (!p.empty() && fs::is_regular_file(p)) {
			out.rd = zenkit::Read::from(p);
			out.loose = p;
		}
	};
	if (game.mod.active) {
		fromArchive();
		if (!out.rd) fromDisk();
	} else {
		fromDisk();
		if (!out.rd) fromArchive();
	}
	return out;
}

std::string trimmed(const std::string& s) {
	size_t a = 0, b = s.size();
	while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
	while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
	return s.substr(a, b - a);
}

// The .ini of a mod: [SECTION] KEY=value, names without regard to case.
struct IniFile {
	std::map<std::string, std::string> values; // "SECTION/KEY"
	bool load(const fs::path& path) {
		std::ifstream in(path, std::ios::binary);
		if (!in) return false;
		std::string line, section;
		bool first = true;
		while (std::getline(in, line)) {
			if (first && line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
			first = false;
			line = trimmed(line);
			if (line.empty() || line[0] == ';' || line.compare(0, 2, "//") == 0) continue;
			if (line[0] == '[') {
				const size_t end = line.find(']');
				if (end != std::string::npos) section = upper(trimmed(line.substr(1, end - 1)));
				continue;
			}
			const size_t eq = line.find('=');
			if (eq == std::string::npos) continue;
			values[section + "/" + upper(trimmed(line.substr(0, eq)))] = trimmed(line.substr(eq + 1));
		}
		return true;
	}
	bool has(const char* section, const char* key) const { return values.count(std::string(section) + "/" + key) != 0; }
	std::string get(const char* section, const char* key) const {
		const auto it = values.find(std::string(section) + "/" + key);
		return it == values.end() ? std::string() : it->second;
	}
};

// "NewWorld\\Archolos.zen" -> "ARCHOLOS.ZEN"
std::string baseNameUpper(std::string s) {
	const size_t slash = s.find_last_of("/\\");
	if (slash != std::string::npos) s = s.substr(slash + 1);
	return upper(s);
}

bool endsWithNoCase(const std::string& s, const std::string& suffix) {
	return s.size() >= suffix.size() && lower(s.substr(s.size() - suffix.size())) == lower(suffix);
}

// Every regular file with the extension `ext` (".vdf", ".mod") under `root`, sorted by path.
std::vector<fs::path> filesWithExtension(const fs::path& root, const std::string& ext) {
	std::vector<fs::path> found;
	std::error_code ec;
	for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
		std::error_code fe;
		if (it->is_regular_file(fe) && lower(it->path().extension().string()) == ext && fs::file_size(it->path(), fe) > 0)
			found.push_back(it->path());
	}
	std::sort(found.begin(), found.end());
	return found;
}

void collectWorlds(const zenkit::VfsNode& node, std::set<std::string>& out) {
	if (node.type() == zenkit::VfsNodeType::FILE) {
		if (endsWithNoCase(node.name(), ".zen")) out.insert(upper(node.name()));
		return;
	}
	for (const zenkit::VfsNode& child : node.children()) collectWorlds(child, out);
}

} // namespace

std::shared_ptr<ZenGame> openZenGame(const std::string& gameDir, std::vector<std::string>& notes, const ZenModOptions& mod) {
	fs::path dataDir;
	if (!fs::is_directory(gameDir)) throw std::runtime_error("'" + gameDir + "' is not a directory");
	for (const auto& e : fs::directory_iterator(gameDir)) {
		if (e.is_directory() && lower(e.path().filename().string()) == "data") dataDir = e.path();
	}
	if (dataDir.empty()) throw std::runtime_error("no Data directory in '" + gameDir + "'; is this a Gothic install?");

	// The game's own archives: a mod's lie in the same folders, but go over them (below).
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
	// A game of its own (Chronicles of Myrtana: Archolos, on Steam) ships its worlds, scripts and music as .mod
	// archives in Data/ and has no .ini to say so: those are mounted over the .vdf ones all the same.
	ZenModOptions opts = mod;
	const bool automatic = !opts.active();
	if (automatic) opts.all = true;

	// ---- a modification ----------------------------------------------------
	ZenModInfo& info = game->mod;
	info.active = true;
	info.gameDat = "GOTHIC.DAT";
	bool listed = false; // the .ini names the archives it wants
	std::vector<std::string> listedNames;
	if (!opts.ini.empty()) {
		fs::path iniPath = opts.ini;
		std::error_code ec;
		if (!fs::is_regular_file(iniPath, ec)) {
			iniPath.clear();
			const fs::path systemDir = findNoCase(gameDir, {"system"});
			if (!systemDir.empty()) {
				for (const std::string& name : {opts.ini, opts.ini + ".ini"})
					for (const auto& e : fs::directory_iterator(systemDir, ec))
						if (e.is_regular_file() && lower(e.path().filename().string()) == lower(name)) iniPath = e.path();
			}
		}
		if (iniPath.empty()) {
			std::string have;
			const fs::path systemDir = findNoCase(gameDir, {"system"});
			std::error_code ec2;
			if (!systemDir.empty())
				for (const auto& e : fs::directory_iterator(systemDir, ec2))
					if (lower(e.path().extension().string()) == ".ini") have += (have.empty() ? "" : ", ") + e.path().filename().string();
			throw std::runtime_error("mod .ini '" + opts.ini + "' not found (not a file, and not in the game's System folder" +
			                         (have.empty() ? ")" : "; there: " + have + ")"));
		}
		IniFile ini;
		if (!ini.load(iniPath)) throw std::runtime_error("cannot read '" + iniPath.string() + "'");
		info.title = ini.get("INFO", "TITLE");
		const std::string world = ini.get("SETTINGS", "WORLD");
		if (!world.empty()) info.world = baseNameUpper(world);
		const std::string dat = ini.get("FILES", "GAME");
		if (!dat.empty()) info.gameDat = endsWithNoCase(dat, ".dat") ? baseNameUpper(dat) : baseNameUpper(dat) + ".DAT";
		const std::string ou = ini.get("FILES", "OUTPUTUNITS");
		if (!ou.empty()) {
			info.outputUnits = baseNameUpper(ou);
			for (const char* ext : {".BIN", ".DAT"})
				if (endsWithNoCase(info.outputUnits, ext)) info.outputUnits.resize(info.outputUnits.size() - 4);
		}
		listed = ini.has("FILES", "VDF") && !trimmed(ini.get("FILES", "VDF")).empty();
		if (listed) {
			std::istringstream words(ini.get("FILES", "VDF"));
			for (std::string w; words >> w;) listedNames.push_back(baseNameUpper(w));
		}
		notes.push_back("mod: " + iniPath.filename().string() + (info.title.empty() ? "" : " (" + info.title + ")"));
	}

	// Which archives: those the .ini lists (all the .mod ones if it lists none, or on request), then the named ones.
	std::vector<fs::path> mounts;
	auto add = [&](const fs::path& p) {
		for (const fs::path& m : mounts)
			if (lower(m.string()) == lower(p.string())) return;
		mounts.push_back(p);
	};
	if (!opts.ini.empty() || opts.all) {
		for (const fs::path& m : filesWithExtension(dataDir, ".mod")) {
			bool wanted = opts.all || !listed;
			if (!wanted)
				for (const std::string& n : listedNames)
					if (upper(m.filename().string()) == n) wanted = true;
			if (wanted) add(m);
		}
		if (listed)
			for (const std::string& n : listedNames) {
				bool found = false;
				for (const fs::path& m : mounts)
					if (upper(m.filename().string()) == n) found = true;
				if (!found) {
					// A .vdf of the game itself, listed too, is mounted already; anything else is missing.
					bool ours = false;
					for (const fs::path& d : disks)
						if (upper(d.filename().string()) == n) ours = true;
					if (!ours) notes.push_back("mod: the .ini lists " + n + ", which is not in Data/; skipped");
				}
			}
	}
	for (const std::string& a : opts.archives) {
		fs::path p = a;
		std::error_code ec;
		if (!fs::is_regular_file(p, ec)) {
			p.clear();
			for (const char* ext : {".mod", ".vdf"})
				for (const fs::path& m : filesWithExtension(dataDir, ext))
					if (p.empty() && (lower(m.filename().string()) == lower(a) || lower(m.filename().string()) == lower(a) + ext)) p = m;
		}
		if (p.empty()) throw std::runtime_error("mod archive '" + a + "' not found (not a file, and not under " + dataDir.string() + ")");
		add(p);
	}
	if (automatic && mounts.empty()) {
		game->mod = ZenModInfo();
		return game;
	}
	if (mounts.empty())
		notes.push_back("WARNING: mod: no .mod archives found under " + dataDir.string() + "; only the .ini and loose files count");

	// What the game has before the mod comes, to tell what the mod brings.
	std::set<std::string> before, after;
	collectWorlds(game->vfs.root(), before);
	for (const fs::path& m : mounts) {
		try {
			game->vfs.mount_disk(m, zenkit::VfsOverwriteBehavior::ALL); // the mod wins, whatever the dates say
			info.mounted.push_back(m.filename().string());
		} catch (const std::exception& ex) {
			notes.push_back("mod: skipped " + m.filename().string() + ": " + ex.what());
		}
	}
	collectWorlds(game->vfs.root(), after);
	for (const std::string& w : after)
		if (!before.count(w)) info.worlds.push_back(w);
	{
		std::string list;
		for (const std::string& m : info.mounted) list += (list.empty() ? "" : ", ") + m;
		notes.push_back("mod: archives over the game: " + (list.empty() ? std::string("none") : list));
	}
	return game;
}

const ZenModInfo& zenGameMod(const ZenGame& game) { return game.mod; }

namespace {
void extractNode(const zenkit::VfsNode& node, const std::vector<std::string>& exts, const fs::path& outDir,
                 std::set<std::string>& done, std::vector<std::string>& notes) {
	if (node.type() == zenkit::VfsNodeType::DIRECTORY) {
		for (const zenkit::VfsNode& child : node.children()) extractNode(child, exts, outDir, done, notes);
		return;
	}
	const std::string name = node.name();
	bool wanted = false;
	for (const std::string& e : exts) wanted = wanted || endsWithNoCase(name, e);
	if (!wanted || !done.insert(lower(name)).second) return;
	try {
		auto rd = node.open_read();
		rd->seek(0, zenkit::Whence::END);
		std::vector<char> bytes(size_t(rd->tell()));
		rd->seek(0, zenkit::Whence::BEG);
		if (!bytes.empty()) rd->read(bytes.data(), bytes.size());
		std::ofstream out(outDir / name, std::ios::binary);
		out.write(bytes.data(), std::streamsize(bytes.size()));
		if (!out) throw std::runtime_error("cannot write");
	} catch (const std::exception& ex) {
		done.erase(lower(name));
		notes.push_back("extract: " + name + ": " + ex.what());
	}
}
} // namespace

size_t zenExtractFiles(const ZenGame& game, const std::vector<std::string>& exts, const std::string& outDir,
                       std::vector<std::string>& notes) {
	std::error_code ec;
	fs::create_directories(outDir, ec);
	if (ec) throw std::runtime_error("cannot create '" + outDir + "': " + ec.message());
	std::set<std::string> done;
	extractNode(game.vfs.root(), exts, outDir, done, notes);
	// Loose files of the installation fill in what the archives do not have (the archives rank first, as in the game).
	const fs::path music = findNoCase(game.dir, {"_work", "data", "music"});
	if (!music.empty()) {
		for (fs::recursive_directory_iterator it(music, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
			std::error_code fe;
			if (!it->is_regular_file(fe)) continue;
			const std::string name = it->path().filename().string();
			bool wanted = false;
			for (const std::string& e : exts) wanted = wanted || endsWithNoCase(name, e);
			if (!wanted || !done.insert(lower(name)).second) continue;
			fs::copy_file(it->path(), fs::path(outDir) / name, fs::copy_options::overwrite_existing, fe);
			if (fe) {
				done.erase(lower(name));
				notes.push_back("extract: " + name + ": " + fe.message());
			}
		}
	}
	return done.size();
}

bool zenGameHasFile(const ZenGame& game, const std::string& name) { return game.vfs.find(name) != nullptr; }

std::vector<std::pair<std::string, size_t>> zenGameWorlds(const ZenGame& game) {
	std::set<std::string> names;
	collectWorlds(game.vfs.root(), names);
	std::vector<std::pair<std::string, size_t>> out;
	for (const std::string& n : names) out.emplace_back(n, zenGameFileSize(game, n));
	std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
	return out;
}

size_t zenGameFileSize(const ZenGame& game, const std::string& name) {
	const zenkit::VfsNode* node = game.vfs.find(name);
	if (!node || node->type() != zenkit::VfsNodeType::FILE) return 0;
	auto rd = node->open_read();
	rd->seek(0, zenkit::Whence::END);
	return size_t(rd->tell());
}


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
const char* const kHeroStates[] = {"STAND", "RUN", "RUNL", "WALK", "WALKL", "WALKBL", "WALKWL", "JUMP", "FALLDN", "FALL", "SWIM", "SWIMF", "SWIMB",
                                   "SNEAK", "SNEAKL", "SNEAKBL"};

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
	// (And between the states of a weapon in hand: "T_1HRUN_2_1HRUNL".)
	for (const std::string& st : fightStates()) {
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
	if (!loadFile(vfs, treeName, hierarchy) && !(!opt.tree.empty() && loadFile(vfs, (treeName = stripExtension(upper(opt.tree)) + ".MDH"), hierarchy))) {
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
	if (!standardSet && !opt.overlay)
		for (const char* w : {"S_RUNL", "S_WALKL", "S_FISTRUNL", "S_FISTWALKL"})
			if (std::find(wanted.begin(), wanted.end(), w) == wanted.end()) {
				wanted.push_back(w);
				witnesses.insert(w);
			}
	if (!opt.clipPrefixes.empty()) {
		const auto family = [&](const std::string& rawName) {
			const std::string name = upper(rawName);
			for (const std::string& prefix : opt.clipPrefixes)
				if (name.compare(0, prefix.size(), prefix) == 0 && std::find(wanted.begin(), wanted.end(), name) == wanted.end()) {
					wanted.push_back(name);
					return;
				}
		};
		for (const zenkit::MdsAnimation& a : script.animations) family(a.name);
		for (const zenkit::MdsAnimationAlias& a : script.aliases) family(a.name);
	}
	// Asked for in case the model has them; not missed if it has not.
	std::set<std::string> optional;
	if (standardSet) {
		wanted.assign(std::begin(kHeroClips), std::end(kHeroClips));
		for (const std::string& name : fightClipNames(true))
			if (std::find(wanted.begin(), wanted.end(), name) == wanted.end()) {
				wanted.push_back(name);
				optional.insert(name);
			}
		for (const std::string& name : bowClipNames())
			if (std::find(wanted.begin(), wanted.end(), name) == wanted.end()) {
				wanted.push_back(name);
				optional.insert(name);
			}
		for (const std::string& name : magicClipNames())
			if (std::find(wanted.begin(), wanted.end(), name) == wanted.end()) {
				wanted.push_back(name);
				optional.insert(name);
			}
		for (const std::string& name : itemUseClipNames())
			if (std::find(wanted.begin(), wanted.end(), name) == wanted.end()) {
				wanted.push_back(name);
				optional.insert(name);
			}
		// What the script has for reaching: climbing, hanging, picking up, chests.
		std::string reachFound;
		const auto reachFamily = [&](const std::string& rawName) {
			const std::string name = upper(rawName);
			if (!isHeroReachFamily(name)) return;
			if (std::find(wanted.begin(), wanted.end(), name) == wanted.end()) {
				wanted.push_back(name);
				optional.insert(name);
			}
			if (reachFound.find(" " + name + ",") == std::string::npos) reachFound += " " + name + ",";
		};
		for (const zenkit::MdsAnimation& a : script.animations) reachFamily(a.name);
		for (const zenkit::MdsAnimationAlias& a : script.aliases) reachFamily(a.name);
		// (fix155) ... and at the things of the worlds (ladders, benches, forges): the names the baker was given.
		size_t byThing = 0;
		if (!opt.clipPrefixes.empty()) {
			const auto family = [&](const std::string& rawName) {
				const std::string name = upper(rawName);
				for (const std::string& prefix : opt.clipPrefixes)
					if (name.compare(0, prefix.size(), prefix) == 0 && std::find(wanted.begin(), wanted.end(), name) == wanted.end()) {
						wanted.push_back(name);
						optional.insert(name);
						++byThing;
						return;
					}
			};
			for (const zenkit::MdsAnimation& a : script.animations) family(a.name);
			for (const zenkit::MdsAnimationAlias& a : script.aliases) family(a.name);
		}
		model.notes.push_back("character: " + std::to_string(byThing) + " animations at the things of the worlds (ladders, benches, forges, levers)");
		// (fix192) ... and with the things in his hands: every scheme of the scripts' words the model script has a way
		// into ("T_BROOM_STAND_2_S0"), with all its animations.
		if (!opt.stateSchemes.empty()) {
			std::set<std::string> names;
			for (const zenkit::MdsAnimation& a : script.animations) names.insert(upper(a.name));
			for (const zenkit::MdsAnimationAlias& a : script.aliases) names.insert(upper(a.name));
			size_t byHand = 0;
			std::string found;
			for (const std::string& w : opt.stateSchemes) {
				if (!names.count("T_" + w + "_STAND_2_S0")) continue;
				found += " " + w;
				const std::string t = "T_" + w + "_", st = "S_" + w + "_";
				for (const std::string& n : names)
					if ((n.compare(0, t.size(), t) == 0 || n.compare(0, st.size(), st) == 0) && std::find(wanted.begin(), wanted.end(), n) == wanted.end()) {
						wanted.push_back(n);
						optional.insert(n);
						++byHand;
					}
			}
			model.notes.push_back("character: " + std::to_string(byHand) + " more animations of things used in the hands (schemes:" +
			                      (found.empty() ? std::string(" none") : found) + ")");
		}
		model.notes.push_back("character: animations of reaching in the model script (ledges, picking up, chests):" +
		                      (reachFound.empty() ? std::string(" none") : reachFound));
		for (const zenkit::MdsAnimation& a : script.animations)
			if (isHeroTransition(upper(a.name)) && std::find(wanted.begin(), wanted.end(), upper(a.name)) == wanted.end())
				wanted.push_back(upper(a.name));
		for (const zenkit::MdsAnimationAlias& a : script.aliases)
			if (isHeroTransition(upper(a.name)) && std::find(wanted.begin(), wanted.end(), upper(a.name)) == wanted.end())
				wanted.push_back(upper(a.name));
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
		// The event tags of a fight (ClipFight), in frames of the clip.
		std::vector<int> hits, hitEnds, windows, parry;
		int fightMode = -1;
		int mdsFirst = -1, mdsLast = -1; // the frames of the file the animation is cut from
		std::vector<SrcClip::Sfx> sfx; // the sound tags (*eventSFX, *eventSFXGrnd)
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
				// An animation of its own may be played backwards too ("R"): G1's putting a
				// sword away is the drawing's file read the other way (as OpenGothic has it).
				lc.reversed = a.direction == zenkit::AnimationDirection::BACKWARD;
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
		if (!described && (opt.onlyDeclared || optional.count(name))) continue;
		// What an animation leads into is wanted along with it ("T_STAND_2_SIT", then "S_SIT";
		// drawing a sword: "T_RUN_2_1H", then "T_1H_2_1HRUN", then "S_1HRUN").
		if (described && (opt.onlyDeclared || optional.count(name)) && !lc.next.empty() && lc.next != name &&
		    std::find(wanted.begin(), wanted.end(), lc.next) == wanted.end()) {
			wanted.push_back(lc.next);
			if (optional.count(name)) optional.insert(lc.next);
		}
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
		// The frames of a fight are the animation's own (an alias has those
		// of the one it names); they count from its first frame.
		for (const zenkit::MdsAnimation& a : script.animations) {
			if (upper(a.name) != file) continue;
			lc.mdsFirst = int(a.first_frame);
			lc.mdsLast = int(a.last_frame);
			const int firstFrame = std::min(int(a.first_frame), int(a.last_frame));
			const auto ownFrame = [&](int frame) { return std::max(frame - firstFrame, 0); };
			// The tags of a fight (the blow's frame, the end of the blow, the combo window, the
			// parry) count in frames of the animation itself, as OpenGothic reads them - not in
			// frames of the file it is cut from: "T_2HATTACKR" is frames 37..72 of its file, its
			// tags are those of "T_2HATTACKL" (frames 1..36), and taking 37 off them left every
			// tag at 0 - a blow that never landed. (A blow's frame past the first frame is the
			// file's own count; before it, the animation's.)
			const auto tagFrame = [&](int frame) { return frame < firstFrame ? std::max(frame, 0) : std::max(frame - firstFrame, 0); };
			const auto relFrame = [&](int frame) { return std::max(frame, 0); };
			for (const zenkit::MdsEventTag& ev : a.events) {
				switch (ev.type) {
				case zenkit::MdsEventType::OPTIMAL_FRAME:
					// (Several tags: the first one counts, as in the original.)
					if (lc.hits.empty() && !ev.frames.empty())
						for (int f : ev.frames) lc.hits.push_back(tagFrame(f));
					else if (lc.hits.empty())
						lc.hits.push_back(tagFrame(ev.frame));
					break;
				case zenkit::MdsEventType::HIT_END:
					for (int f : ev.frames) lc.hitEnds.push_back(relFrame(f));
					break;
				case zenkit::MdsEventType::COMBO_WINDOW:
					for (int f : ev.frames) lc.windows.push_back(relFrame(f));
					break;
				case zenkit::MdsEventType::PARRY_FRAME:
					for (int f : ev.frames) lc.parry.push_back(relFrame(f));
					break;
				case zenkit::MdsEventType::SET_FIGHT_MODE:
					if (lc.fightMode < 0) lc.fightMode = ownFrame(ev.frame);
					break;
				case zenkit::MdsEventType::ITEM_CREATE:
				case zenkit::MdsEventType::ITEM_INSERT:
				case zenkit::MdsEventType::ITEM_REMOVE:
				case zenkit::MdsEventType::ITEM_DESTROY:
				case zenkit::MdsEventType::ITEM_PLACE:
				case zenkit::MdsEventType::ITEM_EXCHANGE: {
					// (fix154) What using a forge, a lab, a pan does to the item in the hand.
					SrcClip::Sfx t;
					t.frame = ownFrame(ev.frame);
					t.item = ev.type == zenkit::MdsEventType::ITEM_CREATE     ? CLIP_ITEM_CREATE
					         : ev.type == zenkit::MdsEventType::ITEM_INSERT   ? CLIP_ITEM_INSERT
					         : ev.type == zenkit::MdsEventType::ITEM_REMOVE   ? CLIP_ITEM_REMOVE
					         : ev.type == zenkit::MdsEventType::ITEM_DESTROY  ? CLIP_ITEM_DESTROY
					         : ev.type == zenkit::MdsEventType::ITEM_PLACE    ? CLIP_ITEM_PLACE
					                                                          : CLIP_ITEM_EXCHANGE;
					t.name = ev.item;
					lc.sfx.push_back(t);
					break;
				}
				default: break;
				}
			}
			// The sound tags: a footstep, a swish. (Played backwards a clip would
			// sound its events in the wrong order; those keep quiet.)
			for (const zenkit::MdsSoundEffect& e : a.sfx) {
				SrcClip::Sfx t;
				t.frame = ownFrame(e.frame);
				t.name = e.name;
				t.range = e.range;
				t.emptySlot = e.empty_slot;
				lc.sfx.push_back(t);
			}
			for (const zenkit::MdsSoundEffectGround& e : a.sfx_ground) {
				SrcClip::Sfx t;
				t.frame = ownFrame(e.frame);
				t.ground = true;
				t.name = e.name;
				t.range = e.range;
				t.emptySlot = e.empty_slot;
				lc.sfx.push_back(t);
			}
			// The lists rise frame after frame; a stray value at the end (ZenKit
			// reads one more when the text ends in a space) goes.
			for (std::vector<int>* list : {&lc.hits, &lc.hitEnds, &lc.windows, &lc.parry})
				for (size_t i = 1; i < list->size(); ++i)
					if ((*list)[i] < (*list)[i - 1]) {
						list->resize(i);
						break;
					}
			if (lc.windows.size() % 2) lc.windows.pop_back();
			if (lc.parry.size() > 2) lc.parry.resize(2);
			break;
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
			if (!witnesses.count(name) && !optional.count(name)) notFound.push_back(name);
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
		if (lc.reversed) {
			// Played backwards: the samples are turned round below, and so are the frames
			// of its events (they were written for the animation played forwards).
			const int last = int(lc.ani.frame_count) - 1;
			const auto flip = [&](int f) { return std::max(last - f, 0); };
			for (std::vector<int>* list : {&lc.hits, &lc.hitEnds, &lc.windows, &lc.parry}) {
				for (int& f : *list) f = flip(f);
				std::reverse(list->begin(), list->end());
			}
			// (A combo window or a parry is a pair "from to": still from before to.)
			if (lc.fightMode >= 0) lc.fightMode = flip(lc.fightMode);
			for (SrcClip::Sfx& e : lc.sfx) e.frame = flip(e.frame);
			std::stable_sort(lc.sfx.begin(), lc.sfx.end(), [](const SrcClip::Sfx& x, const SrcClip::Sfx& y) { return x.frame < y.frame; });
		}
		// (For the report: every clip of a fight or of a weapon taken in hand, as it came out.)
		if (name.find("ATTACK") != std::string::npos || name.find("PARADE") != std::string::npos || name.find("_2_1H") != std::string::npos ||
		    name.find("_2_2H") != std::string::npos || name.find("1H_2_") != std::string::npos || name.find("2H_2_") != std::string::npos ||
		    name.find("JUMPB") != std::string::npos) {
			const auto list = [](const std::vector<int>& v) {
				std::string t;
				for (size_t i = 0; i < v.size(); ++i) t += (i ? "," : "") + std::to_string(v[i]);
				return t.empty() ? std::string("-") : t;
			};
			char buf[400];
			std::snprintf(buf, sizeof(buf),
			              "fightclip %s: file %s%s, mds frames %d..%d, %u frames @ %.0f fps, %s; hits %s; hitEnds %s; windows %s; parry %s; fightMode %d; next %s",
			              name.c_str(), file.c_str(), lc.reversed ? " BACKWARDS" : "", lc.mdsFirst, lc.mdsLast, unsigned(lc.ani.frame_count),
			              double(lc.ani.fps), lc.moves ? "moves" : "in place", list(lc.hits).c_str(), list(lc.hitEnds).c_str(),
			              list(lc.windows).c_str(), list(lc.parry).c_str(), lc.fightMode, lc.next.empty() ? "-" : lc.next.c_str());
			model.notes.push_back(buf);
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
	const bool conjugate = opt.conjugated >= 0 ? opt.conjugated != 0 : conjugated <= asIs;
	model.conjugated = conjugate ? 1 : 0;
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
		clip.hits = lc.hits;
		clip.hitEnds = lc.hitEnds;
		clip.windows = lc.windows;
		clip.parry = lc.parry;
		clip.fightModeFrame = lc.fightMode;
		clip.sfx = lc.sfx;
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
		if (opt.quarters >= 0) {
			quarters = opt.quarters & 3;
			std::snprintf(why, sizeof(why), "as its base model");
		} else if (!witness.empty()) {
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
		model.quarters = quarters;
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

std::vector<std::string> zenOverlaysOf(const ZenGame& game, const std::string& rawBase) {
	const std::string base = stripExtension(upper(rawBase)) + "_";
	std::set<std::string> found;
	std::vector<const zenkit::VfsNode*> todo {&game.vfs.root()};
	while (!todo.empty()) {
		const zenkit::VfsNode* node = todo.back();
		todo.pop_back();
		if (node->type() == zenkit::VfsNodeType::DIRECTORY) {
			for (const zenkit::VfsNode& c : node->children()) todo.push_back(&c);
			continue;
		}
		const std::string name = upper(std::string(node->name()));
		const std::string ext = extensionOf(name);
		if ((ext != "MSB" && ext != "MDS") || name.compare(0, base.size(), base) != 0) continue;
		const std::string stem = stripExtension(name);
		// (A script with a skeleton of its own is a model, not an overlay: "HUMANS_SPEAR".)
		if (game.vfs.find(stem + ".MDH") || game.vfs.find(stem + ".MDL")) continue;
		found.insert(stem);
	}
	return std::vector<std::string>(found.begin(), found.end());
}

LookSource zenLookSource(std::shared_ptr<const ZenGame> game) {
	LookSource source;
	source.texture = textureLoader(game);
	// fix150: the Barrier (oCBarrier::Init loads "MAGICFRONTIER_OUT.3DS": the static mesh compiled to .MSH; a
	// multi-resolution .MRM is taken too). In world coordinates, as the game draws it.
	source.barrier = [game](SrcBarrier& out, std::string& why) {
		out = SrcBarrier {};
		const std::string base = "MAGICFRONTIER_OUT";
		try {
			if (const zenkit::VfsNode* node = game->vfs.find(base + ".MSH")) {
				zenkit::Mesh mesh;
				auto rd = node->open_read();
				mesh.load(rd.get(), false);
				out.mesh = base + ".MSH";
				// (Corners: a vertex with the texture coordinates of the polygon's corner; shared where both are.)
				std::map<std::pair<u32, u32>, u32> corner;
				auto cornerOf = [&](u32 vi, u32 fi) -> u32 {
					auto it = corner.find({vi, fi});
					if (it != corner.end()) return it->second;
					const zenkit::Vec3& p = mesh.vertices.at(vi);
					const zenkit::VertexFeature& f = mesh.features.at(fi);
					const u32 n = u32(out.pos.size() / 3);
					out.pos.insert(out.pos.end(), {p.x, p.y, p.z});
					out.uv.insert(out.uv.end(), {f.texture.x, f.texture.y});
					corner.emplace(std::make_pair(vi, fi), n);
					return n;
				};
				std::map<u32, size_t> perMaterial;
				if (!mesh.polygons.vertex_indices.empty()) {
					const auto& vi = mesh.polygons.vertex_indices;
					const auto& fi = mesh.polygons.feature_indices;
					for (size_t t = 0; t + 2 < vi.size() && t + 2 < fi.size(); t += 3) {
						for (int k = 0; k < 3; ++k) out.indices.push_back(cornerOf(vi[t + size_t(k)], fi[t + size_t(k)]));
						if (t / 3 < mesh.polygons.material_indices.size()) ++perMaterial[mesh.polygons.material_indices[t / 3]];
					}
				} else {
					for (const zenkit::Polygon& p : mesh.geometry) {
						if (p.index_count < 3 || p.flags.is_portal || p.flags.is_ghost_occluder) continue;
						for (size_t k = 2; k < p.index_count; ++k) {
							for (size_t c : {size_t(0), k - 1, k})
								out.indices.push_back(cornerOf(mesh.polygon_vertex_indices.at(p.index_offset + c),
								                               mesh.polygon_feature_indices.at(p.index_offset + c)));
							++perMaterial[p.material];
						}
					}
				}
				// The texture: the material most of it wears.
				size_t best = 0;
				for (const auto& [m, n] : perMaterial)
					if (n > best && m < mesh.materials.size() && !mesh.materials[m].texture.empty()) {
						best = n;
						out.texture = mesh.materials[m].texture;
					}
			} else {
				std::string w;
				FlatPtr flat = loadVisual(game->vfs, base + ".3DS", w);
				if (!flat || flat->subs.empty()) {
					why = w.empty() ? base + ".MSH / .MRM: not in the game's files" : base + ": " + w;
					return false;
				}
				out.mesh = base + ".MRM";
				size_t best = 0;
				for (const FlatSub& sub : flat->subs) {
					for (const FlatVertex& v : sub.verts) {
						out.indices.push_back(u32(out.pos.size() / 3));
						out.pos.insert(out.pos.end(), {v.pos[0], v.pos[1], v.pos[2]});
						out.uv.insert(out.uv.end(), {v.uv[0], v.uv[1]});
					}
					if (sub.verts.size() > best && !sub.material.texture.empty()) {
						best = sub.verts.size();
						out.texture = sub.material.texture;
					}
				}
			}
		} catch (const std::exception& e) {
			why = base + ": " + e.what();
			return false;
		}
		if (out.indices.size() < 3) {
			why = out.mesh + ": no triangles";
			return false;
		}
		return true;
	};
	// A thing lying about: its mesh (".3DS", compiled to ".MRM"; a few are
	// morph meshes or models), one node, turned as the world file turns it.
	source.item = [game](const std::string& visual, SrcModel& model) {
		FlatPtr flat;
		std::string why;
		// (fix157) "<chest>_g3lid": the lid of a chest, cut from its model, in its node's own space.
		const bool lidOnly = visual.size() > 6 && visual.compare(visual.size() - 6, 6, "_g3lid") == 0;
		const std::string look = lidOnly ? visual.substr(0, visual.size() - 6) : visual;
		std::string found;
		for (const char* ext : {".3DS", ".MMS", ".ASC", ".MDS"}) {
			std::string w;
			flat = loadVisual(game->vfs, upper(look) + ext, w);
			if (flat && !flat->subs.empty()) {
				found = upper(look) + ext;
				break;
			}
			if (!w.empty()) why = w;
			flat = nullptr;
		}
		bool lidFlip = false;
		if (lidOnly && flat) {
			ChestLid cl;
			std::string w;
			flat->finish();
			lidFlip = outwardScore(*flat) < -0.05; // (the whole chest says which way its faces point)
			if (!chestLidOf(game->vfs, found, *flat, cl, w)) {
				model.notes.push_back(upper(look) + ": no lid (" + w + ")");
				return false;
			}
			flat = cl.lid;
		}
		if (!flat) {
			model.notes.push_back(why.empty() ? upper(visual) + ": no mesh" : upper(visual) + ": " + why);
			return false;
		}
		flat->finish();
		const bool flip = lidOnly ? lidFlip : outwardScore(*flat) < -0.05;
		model.nodes.assign(1, SrcNode {});
		model.nodes[0].name = "ITEM";
		for (const FlatSub& sub : flat->subs) {
			SrcModelPart part;
			part.material = sub.material;
			for (size_t t = 0; t + 2 < sub.verts.size(); t += 3)
				for (int k = 0; k < 3; ++k) {
					const FlatVertex& v = sub.verts[t + size_t(flip ? (k == 0 ? 0 : 3 - k) : k)];
					SrcSkinVertex sv;
					std::copy_n(v.pos, 3, sv.pos);
					std::copy_n(v.normal, 3, sv.normal);
					std::copy_n(v.uv, 2, sv.uv);
					sv.bone[0] = sv.bone[1] = 0;
					part.verts.push_back(sv);
				}
			if (!part.verts.empty()) model.parts.push_back(std::move(part));
		}
		model.loadTexture = textureLoader(game);
		return !model.parts.empty();
	};
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
		// ... and whatever the scripts name that this model has (a name
		// ending in a star stands for all that begin like it).
		for (const std::string& name : wanted) {
			if (!name.empty() && name.back() == '*')
				o.clipPrefixes.push_back(name.substr(0, name.size() - 1));
			else if (std::find(o.clips.begin(), o.clips.end(), name) == o.clips.end())
				o.clips.push_back(name);
		}
		if (!importZenActor(game, o, model)) return false;
		bool stands = false;
		for (const SrcClip& c : model.clips) stands = stands || c.name == "S_RUN" || c.name == "S_WALK" || c.name == "S_FISTRUN" || c.name == "S_FISTWALK";
		if (!stands) {
			model.clips.clear();
			model.notes.push_back(upper(script) + ": no animation to stand in");
		}
		return true;
	};
	// An overlay's animations, on its base's skeleton, read as the base's are (no mesh).
	source.overlay = [game](const std::string& base, const std::string& overlay, SrcModel& model) {
		SrcModel probe;
		ZenActorOptions b;
		b.script = base;
		b.body.clear();
		b.head.clear();
		b.heroClips = false;
		b.onlyDeclared = true;
		if (!importZenActor(game, b, probe)) return false;
		ZenActorOptions o;
		o.script = overlay;
		o.tree = base;
		o.body.clear();
		o.head.clear();
		o.heroClips = false;
		o.onlyDeclared = true;
		o.overlay = true;
		o.clipPrefixes = {"T_", "S_", "C_", "R_"};
		o.conjugated = probe.conjugated;
		o.quarters = probe.quarters;
		if (!importZenActor(game, o, model)) return false;
		if (model.nodes.size() != probe.nodes.size()) {
			model.notes.push_back(upper(overlay) + ": not the skeleton of " + upper(base));
			return false;
		}
		return !model.clips.empty();
	};
	source.overlays = [game](const std::string& base) { return zenOverlaysOf(*game, base); };
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
	// fix151: a transformation's creature, whole, with the hero's set of animations (what its script lacks is passed over).
	source.form = [game](const std::string& script, const std::string& mesh, int tex, int skin, SrcModel& model) {
		ZenActorOptions o;
		o.script = script;
		o.body = mesh;
		o.head.clear();
		o.bodyVariant = tex;
		o.skinColor = skin;
		o.heroClips = true;
		return importZenActor(game, o, model);
	};
	return source;
}

// ---- the camera ----------------------------------------------------------

namespace {

// A path below `root` given in any letter case (the game is a Windows program).


} // namespace

// The game's sound files by name: in an archive (Sounds.vdf), or loose in the
// installation's sound folder.
std::function<bool(const std::string&, std::vector<u8>&)> zenSoundSource(std::shared_ptr<const ZenGame> game) {
	return [game](const std::string& file, std::vector<u8>& bytes) {
		try {
			std::unique_ptr<zenkit::Read> rd;
			if (const zenkit::VfsNode* node = game->vfs.find(upper(file))) {
				rd = node->open_read();
			} else {
				const std::string name = lower(file);
				fs::path loose = findNoCase(game->dir, {"_work", "data", "sound", "speech", name.c_str()});
				if (loose.empty()) loose = findNoCase(game->dir, {"_work", "data", "sound", "sfx", name.c_str()});
				if (loose.empty()) loose = findNoCase(game->dir, {"_work", "data", "sound", name.c_str()});
				if (loose.empty() || !fs::is_regular_file(loose)) return false;
				rd = zenkit::Read::from(loose);
			}
			rd->seek(0, zenkit::Whence::END);
			const size_t size = rd->tell();
			rd->seek(0, zenkit::Whence::BEG);
			if (size == 0 || size > (64u << 20)) return false;
			bytes.resize(size);
			rd->read(bytes.data(), size);
			return true;
		} catch (const std::exception&) {
			return false; // unreadable: treated like a missing one
		}
	};
}

bool importZenCamera(std::shared_ptr<const ZenGame> game, CameraSet& set, std::vector<std::string>& notes) {
	set = defaultCameraSet();
	// Compiled scripts are loose files of the installation; a mod may carry them in an archive.
	GameFile found = openGameFile(*game, {"_work", "data", "scripts", "_compiled"}, "CAMERA.DAT");
	std::unique_ptr<zenkit::Read> rd = std::move(found.rd);
	if (!rd) {
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

namespace {

// A compiled script of the game (`file` as "GOTHIC.DAT"): its symbols and
// code, and the file itself. `topic` begins the report's lines, `without`
// says what happens if it cannot be had.
bool importZenCompiled(std::shared_ptr<const ZenGame> game, const std::string& file, const char* topic, const char* without,
                       SrcScript& script, std::vector<u8>& dat, std::vector<std::string>& notes) {
	script = SrcScript();
	dat.clear();
	// Compiled scripts are loose files of the installation; a mod may carry them in an archive.
	GameFile found = openGameFile(*game, {"_work", "data", "scripts", "_compiled"}, file);
	std::unique_ptr<zenkit::Read> rd = std::move(found.rd);
	if (rd) {
		script.source = found.loose.empty() ? file + " (from an archive)" : file;
	} else {
		notes.push_back(std::string(topic) + ": " + file + " not found (_work/Data/Scripts/_compiled); " + without);
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
		// (A file cut short reads as zeros from where it ends: symbols of no
		// kind, and no code at all.)
		if (codeSize == 0) throw std::runtime_error("it has no code: the file is cut short or damaged");
		// The code is what the file ends with, after its size.
		u32 stated = 0;
		if (size >= u64(codeSize) + 4) std::memcpy(&stated, dat.data() + size - codeSize - 4, 4);
		if (size < u64(codeSize) + 4 || stated != codeSize) {
			// Not laid out as expected: this project's own reader may still cope.
			readScriptDat(dat, script);
			notes.push_back(std::string(topic) + ": " + file + " has something after its code; read with this project's own reader");
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
				// A file cut short reads as zeros from where it ends - no code
				// at all, then - and must not pass for a script: what has code
				// begins inside the code.
				if (!o.external && o.isConst && o.address >= codeSize)
					throw std::runtime_error("the code of '" + o.name + "' is not in the file: it is cut short or damaged");
				break;
			default:
				break;
			}
		}
		return true;
	} catch (const std::exception& ex) {
		script = SrcScript();
		dat.clear();
		notes.push_back(std::string(topic) + ": " + file + " could not be read (" + ex.what() + "); " + without);
		return false;
	}
}

} // namespace

bool importZenScript(std::shared_ptr<const ZenGame> game, SrcScript& script, std::vector<u8>& dat,
                     std::vector<std::string>& notes) {
	return importZenCompiled(game, game->mod.gameDat.empty() ? "GOTHIC.DAT" : game->mod.gameDat, "scripts", "the world stays empty", script, dat, notes);
}

bool importZenFightScript(std::shared_ptr<const ZenGame> game, SrcScript& script, std::vector<std::string>& notes) {
	std::vector<u8> dat;
	return importZenCompiled(game, "FIGHT.DAT", "fight AI", "the inhabitants fight without its tables", script, dat, notes);
}

bool importZenSfxScript(std::shared_ptr<const ZenGame> game, SrcScript& script, std::vector<std::string>& notes) {
	std::vector<u8> dat;
	return importZenCompiled(game, "SFX.DAT", "sounds", "the game is silent", script, dat, notes);
}

bool importZenParticleScript(std::shared_ptr<const ZenGame> game, SrcScript& script, std::vector<std::string>& notes) {
	std::vector<u8> dat;
	return importZenCompiled(game, "PARTICLEFX.DAT", "particle effects", "fires, smoke and magic show nothing", script, dat, notes);
}

bool importZenVisualFxScript(std::shared_ptr<const ZenGame> game, SrcScript& script, std::vector<std::string>& notes) {
	std::vector<u8> dat;
	return importZenCompiled(game, "VISUALFX.DAT", "visual effects", "spells show nothing", script, dat, notes);
}

bool importZenMusicScript(std::shared_ptr<const ZenGame> game, SrcScript& script, std::vector<std::string>& notes) {
	std::vector<u8> dat;
	return importZenCompiled(game, "MUSIC.DAT", "music", "there is no music", script, dat, notes);
}

bool importZenMenuScript(std::shared_ptr<const ZenGame> game, SrcScript& script, std::vector<std::string>& notes) {
	std::vector<u8> dat;
	return importZenCompiled(game, "MENU.DAT", "menus", "the start menu is the engine's own", script, dat, notes);
}

// The words people say: the game's output units, a cutscene library of its
// The two faces of the overlay: the first name found in each list.
bool importZenFonts(std::shared_ptr<const ZenGame> game, SrcFonts& fonts, std::vector<std::string>& notes) {
	static const char* const kText[] = {"FONT_OLD_10_WHITE.FNT", "FONT_DEFAULT.FNT", "FONT_OLD_10_WHITE_HI.FNT"};
	static const char* const kTitle[] = {"FONT_OLD_20_WHITE.FNT", "FONT_OLD_20_WHITE_HI.FNT", "FONT_OLD_10_WHITE.FNT"};
	const char* const* lists[2] = {kText, kTitle};
	std::string found;
	for (int s = 0; s < 2; ++s) {
		bool done = false;
		for (int k = 0; k < 3 && !done; ++k) {
			const zenkit::VfsNode* node = game->vfs.find(lists[s][k]);
			if (!node) continue;
			try {
				zenkit::Font font;
				auto rd = node->open_read();
				font.load(rd.get());
				SrcFontFace& face = fonts.face[s];
				face = SrcFontFace();
				face.name = lists[s][k];
				face.name.resize(face.name.size() - 4);
				face.height = int(font.height);
				if (!loadZenTexture(game->vfs, font.name, face.sheet)) throw std::runtime_error("its texture '" + font.name + "' is not in the archives");
				for (const zenkit::FontGlyph& g : font.glyphs) {
					SrcFontFace::Cell c;
					c.width = g.width;
					c.u0 = g.uv[0].x;
					c.v0 = g.uv[0].y;
					c.u1 = g.uv[1].x;
					c.v1 = g.uv[1].y;
					face.cells.push_back(c);
				}
				done = true;
			} catch (const std::exception& ex) {
				notes.push_back(std::string("fonts: ") + lists[s][k] + " could not be read (" + ex.what() + ")");
			}
		}
		if (!done) {
			// List what there is, for the next try.
			std::function<void(const zenkit::VfsNode&)> walk = [&](const zenkit::VfsNode& n) {
				for (const auto& c : n.children()) {
					const std::string& name = c.name();
					if (name.size() > 4 && upper(name.substr(name.size() - 4)) == ".FNT") found += (found.empty() ? "" : ", ") + name;
					walk(c);
				}
			};
			walk(game->vfs.root());
			notes.push_back("fonts: none of the expected fonts found" + (found.empty() ? std::string(" (no .FNT in the archives)") : "; the archives have: " + found) +
			                "; the overlay keeps its own font");
			return false;
		}
	}
	return true;
}

// own beside the compiled scripts (OU.BIN in Gothic 1; a translation may name
// it otherwise or keep it in text form, which the same reader takes).
bool importZenDialogue(std::shared_ptr<const ZenGame> game, SrcDialogue& dialogue, std::vector<std::string>& notes) {
	dialogue = SrcDialogue();
	// A mod may name its own library (OUTPUTUNITS in its .ini); the usual names follow.
	std::vector<std::string> files = {"ou.bin", "ou.dat", "ou.csl", "ou.lsc"};
	if (!game->mod.outputUnits.empty())
		files.insert(files.begin(), {lower(game->mod.outputUnits) + ".bin", lower(game->mod.outputUnits) + ".dat"});
	for (const std::string& file : files) {
		GameFile found = openGameFile(*game, {"_work", "data", "scripts", "content", "cutscene"}, file);
		std::unique_ptr<zenkit::Read> rd = std::move(found.rd);
		if (!rd) continue;
		dialogue.source = found.loose.empty() ? lower(file) + " (from an archive)" : found.loose.filename().string();
		for (zenkit::GameVersion version : {zenkit::GameVersion::GOTHIC_1, zenkit::GameVersion::GOTHIC_2}) {
			try {
				rd->seek(0, zenkit::Whence::BEG);
				auto archive = zenkit::ReadArchive::from(rd.get());
				if (!archive) throw std::runtime_error("not a ZenGin archive");
				auto lib = archive->read_object<zenkit::CutsceneLibrary>(version);
				if (!lib) throw std::runtime_error("no cutscene library in it");
				dialogue.lines.reserve(lib->blocks.size());
				for (const auto& block : lib->blocks) {
					if (!block) continue;
					const auto message = block->get_message();
					if (message) dialogue.lines.emplace_back(block->name, message->text);
				}
				return !dialogue.lines.empty();
			} catch (const std::exception& ex) {
				dialogue.lines.clear();
				if (version == zenkit::GameVersion::GOTHIC_2)
					notes.push_back(std::string("dialogue: ") + file + " could not be read (" + ex.what() + "); the people stay silent");
			}
		}
	}
	if (dialogue.source.empty())
		notes.push_back("dialogue: OU.BIN not found (_work/Data/Scripts/Content/Cutscene); the people are heard as text-less lines");
	return false;
}

// The rooms: the BSP tree's sectors, each the boxes of its leaves. A leaf that two sectors claim is
// nobody's (OpenGothic's World::roomAt asks for exactly one).
static void collectRooms(const zenkit::BspTree& bsp, const zenkit::Mesh& mesh, SrcScene& scene) {
	std::map<u64, int> claims; // leaf node -> number of sectors, or -1 once two claim it
	std::map<u64, u32> owner;
	std::map<std::string, u32> byName;
	for (const zenkit::BspSector& s : bsp.sectors) {
		if (s.name.empty()) continue;
		const std::string name = upper(s.name);
		auto ins = byName.emplace(name, u32(scene.roomNames.size()));
		if (ins.second) scene.roomNames.push_back(name);
		for (u32 r : s.node_indices) {
			if (r >= bsp.leaf_node_indices.size()) continue;
			const u64 node = bsp.leaf_node_indices[r];
			if (node >= bsp.nodes.size()) continue;
			auto it = owner.find(node);
			if (it == owner.end()) owner.emplace(node, ins.first->second);
			else if (it->second != ins.first->second) claims[node] = -1;
		}
	}
	// (fix167) The floor under the feet says the room, as OpenGothic reads it (MoveAlgo::portalName: the name of the
	// ground's material; World::guildOfRoom takes the part between ':' and '_'): ZenGin names the materials of a
	// sector's polygons after it ("S:<sector>_<material>"). A room is then its own floors and nothing else: each floor
	// polygon's footprint, cut into cells of a metre, only the cells whose middle lies on the polygon (a slanting
	// triangle's box would reach into the street). Standing on it: from a little under the floor to well above a head.
	// Without such names (fix163): the polygons' sector flags; without those, the BSP leaves.
	{
		const auto roomOfMaterial = [&](u32 mi) -> int {
			if (mi >= mesh.materials.size()) return -1;
			const std::string& n = mesh.materials[mi].name;
			const size_t colon = n.find(':');
			if (colon == std::string::npos) return -1;
			std::string rest = upper(n.substr(colon + 1));
			const size_t us = rest.find('_');
			auto it = byName.find(us == std::string::npos ? rest : rest.substr(0, us));
			if (it == byName.end()) it = byName.find(rest); // (a sector named with an underscore of its own)
			if (it == byName.end() && us != std::string::npos) {
				// The longest sector name the material's name starts with.
				size_t best = 0;
				for (const auto& [name, idx] : byName)
					if (name.size() > best && rest.compare(0, name.size(), name) == 0 && (rest.size() == name.size() || rest[name.size()] == '_')) {
						best = name.size();
						it = byName.find(name);
					}
			}
			return it == byName.end() ? -1 : int(it->second);
		};
		const auto floorCells = [&](const zenkit::Polygon& poly, u32 room, std::vector<SrcScene::RoomBoxSrc>& out) -> bool {
			if (poly.index_count < 3 || poly.plane_normal.y < 0.35f) return false; // (walls and ceilings: no one stands on them)
			std::vector<std::array<float, 3>> pts;
			for (size_t c = 0; c < poly.index_count; ++c) {
				const size_t k = size_t(poly.index_offset) + c;
				if (k >= mesh.polygon_vertex_indices.size() || mesh.polygon_vertex_indices[k] >= mesh.vertices.size()) return false;
				const zenkit::Vec3& v = mesh.vertices[mesh.polygon_vertex_indices[k]];
				pts.push_back({v.x, v.y, v.z});
			}
			float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
			for (const auto& p : pts)
				for (int a2 = 0; a2 < 3; ++a2) {
					lo[a2] = std::min(lo[a2], p[size_t(a2)]);
					hi[a2] = std::max(hi[a2], p[size_t(a2)]);
				}
			const auto inside = [&](float x, float z) {
				bool in = false;
				for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
					const float xi = pts[i][0], zi = pts[i][2], xj = pts[j][0], zj = pts[j][2];
					if (((zi > z) != (zj > z)) && x < (xj - xi) * (z - zi) / (zj - zi) + xi) in = !in;
				}
				return in;
			};
			const float cell = 100.0f;
			const int nx = std::max(1, int(std::ceil((hi[0] - lo[0]) / cell))), nz = std::max(1, int(std::ceil((hi[2] - lo[2]) / cell)));
			const float cx = (hi[0] - lo[0]) / float(nx), cz = (hi[2] - lo[2]) / float(nz);
			for (int iz = 0; iz < nz; ++iz) {
				int runStart = -1;
				for (int ix = 0; ix <= nx; ++ix) {
					const bool on = ix < nx && (nx * nz == 1 || inside(lo[0] + (float(ix) + 0.5f) * cx, lo[2] + (float(iz) + 0.5f) * cz));
					if (on && runStart < 0) runStart = ix;
					if (!on && runStart >= 0) {
						SrcScene::RoomBoxSrc bx;
						bx.bbMin[0] = lo[0] + float(runStart) * cx, bx.bbMax[0] = lo[0] + float(ix) * cx;
						bx.bbMin[2] = lo[2] + float(iz) * cz, bx.bbMax[2] = lo[2] + float(iz + 1) * cz;
						bx.bbMin[1] = lo[1] - 60.0f, bx.bbMax[1] = hi[1] + 250.0f;
						bx.room = room;
						out.push_back(bx);
						runStart = -1;
					}
				}
			}
			return true;
		};
		// What the names say against what the flags say (for the log: which of the two the world has).
		size_t named = 0, namedFloors = 0, flagged = 0, flaggedFloors = 0, agree = 0, unknownNames = 0;
		std::set<std::string> unknownSample;
		std::vector<SrcScene::RoomBoxSrc> byMaterial, byFlag;
		std::set<u32> roomsByMaterial, roomsByFlag;
		for (size_t pi = 0; pi < mesh.geometry.size(); ++pi) {
			const zenkit::Polygon& poly = mesh.geometry[pi];
			if (poly.flags.is_portal) continue;
			const int rm = roomOfMaterial(poly.material);
			if (rm < 0 && poly.material < mesh.materials.size() && mesh.materials[poly.material].name.find(':') != std::string::npos) {
				++unknownNames;
				if (unknownSample.size() < 6) unknownSample.insert(mesh.materials[poly.material].name);
			}
			int rf = -1;
			if (poly.flags.is_sector) {
				const int si = poly.flags.sector_index;
				if (si >= 0 && size_t(si) < bsp.sectors.size() && !bsp.sectors[size_t(si)].name.empty()) {
					const auto room = byName.find(upper(bsp.sectors[size_t(si)].name));
					if (room != byName.end()) rf = int(room->second);
				}
			}
			if (rm >= 0) {
				++named;
				if (floorCells(poly, u32(rm), byMaterial)) {
					++namedFloors;
					roomsByMaterial.insert(u32(rm));
				}
			}
			if (rf >= 0) {
				++flagged;
				if (floorCells(poly, u32(rf), byFlag)) {
					++flaggedFloors;
					roomsByFlag.insert(u32(rf));
				}
			}
			if (rm >= 0 && rf >= 0 && rm == rf) ++agree;
		}
		std::string sample;
		for (const auto& n : unknownSample) sample += (sample.empty() ? "" : ", ") + n;
		scene.notes.push_back("rooms: polygons named after a sector " + std::to_string(named) + " (" + std::to_string(namedFloors) + " floors, " +
		                      std::to_string(roomsByMaterial.size()) + " rooms), flagged with a sector " + std::to_string(flagged) + " (" +
		                      std::to_string(flaggedFloors) + " floors, " + std::to_string(roomsByFlag.size()) + " rooms), the two agree on " +
		                      std::to_string(agree) + "; named with ':' but no such sector " + std::to_string(unknownNames) +
		                      (sample.empty() ? std::string() : " (" + sample + ")"));
		for (int pass = 0; pass < 2; ++pass) {
			const auto& boxes = pass == 0 ? byMaterial : byFlag;
			if (boxes.empty() || boxes.size() > 600000) continue;
			scene.notes.push_back(std::string("rooms: ") + std::to_string(boxes.size()) + " boxes from the floors " +
			                      (pass == 0 ? "named after their sector (as OpenGothic)" : "flagged with their sector") + ", " +
			                      std::to_string((pass == 0 ? roomsByMaterial : roomsByFlag).size()) + " of " + std::to_string(scene.roomNames.size()) +
			                      " rooms have floors");
			for (const auto& bx : boxes) scene.roomBoxes.push_back(bx);
			return;
		}
		scene.notes.push_back("rooms: no floor is named or flagged with its sector: the BSP leaves are used");
	}
	// (fix156) A leaf's box is a box round all its polygons: a hut that stands crooked, a slope, a wall running
	// out of the leaf - and the street beside the hut was "in the room", the guards drove the player out of
	// the open. So the room is the leaf's floors and ceilings, each polygon's own footprint (walls add nothing:
	// they stand on the edge of a floor); the height is the leaf's. A leaf with no polygons to read keeps its box.
	size_t leafBoxes = 0, polyBoxes = 0;
	std::vector<SrcScene::RoomBoxSrc> boxes;
	std::set<std::array<float, 7>> seen; // (the same footprint twice - floor and ceiling of a flat room - is one box)
	for (const auto& [node, room] : owner) {
		if (claims.count(node)) continue;
		const zenkit::BspNode& n = bsp.nodes[size_t(node)];
		const size_t before = boxes.size();
		for (int pass = 0; pass < 2 && boxes.size() == before; ++pass) {
			// (Pass 0: the floors and ceilings; pass 1, a leaf that has only walls: whatever polygons it has.)
			for (u32 i = 0; i < n.polygon_count; ++i) {
				const size_t at = size_t(n.polygon_index) + i;
				if (at >= bsp.polygon_indices.size()) break;
				const u32 pi = bsp.polygon_indices[at];
				if (pi >= mesh.geometry.size()) continue;
				const zenkit::Polygon& poly = mesh.geometry[pi];
				if (poly.index_count < 3 || poly.flags.is_portal) continue;
				if (pass == 0 && fabsf(poly.plane_normal.y) < 0.35f) continue;
				float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
				bool ok = true;
				for (size_t c = 0; c < poly.index_count && ok; ++c) {
					const size_t k = size_t(poly.index_offset) + c;
					if (k >= mesh.polygon_vertex_indices.size() || mesh.polygon_vertex_indices[k] >= mesh.vertices.size()) {
						ok = false;
						break;
					}
					const zenkit::Vec3& v = mesh.vertices[mesh.polygon_vertex_indices[k]];
					const float p[3] = {v.x, v.y, v.z};
					for (int a = 0; a < 3; ++a) {
						lo[a] = std::min(lo[a], p[a]);
						hi[a] = std::max(hi[a], p[a]);
					}
				}
				if (!ok || !(hi[0] > lo[0] || hi[2] > lo[2])) continue;
				SrcScene::RoomBoxSrc b;
				b.bbMin[0] = lo[0], b.bbMax[0] = hi[0];
				b.bbMin[2] = lo[2], b.bbMax[2] = hi[2];
				// The height: the leaf's (its floor to its roof), at least the polygon's own.
				b.bbMin[1] = std::min(n.bbox.min.y, lo[1]);
				b.bbMax[1] = std::max(n.bbox.max.y, hi[1]);
				b.room = room;
				if (!seen.insert({std::floor(b.bbMin[0]), std::floor(b.bbMax[0]), std::floor(b.bbMin[2]), std::floor(b.bbMax[2]),
				                  std::floor(b.bbMin[1]), std::floor(b.bbMax[1]), float(room)}).second)
					continue;
				boxes.push_back(b);
			}
		}
		if (boxes.size() > before) {
			++polyBoxes;
		} else {
			SrcScene::RoomBoxSrc b;
			b.bbMin[0] = n.bbox.min.x, b.bbMin[1] = n.bbox.min.y, b.bbMin[2] = n.bbox.min.z;
			b.bbMax[0] = n.bbox.max.x, b.bbMax[1] = n.bbox.max.y, b.bbMax[2] = n.bbox.max.z;
			if (!(b.bbMax[0] > b.bbMin[0] && b.bbMax[2] > b.bbMin[2])) continue;
			b.room = room;
			boxes.push_back(b);
			++leafBoxes;
		}
	}
	// (A world with very many: the leaves' own boxes, as before - the grid of the engine has a limit.)
	if (boxes.size() > 600000) {
		boxes.clear();
		for (const auto& [node, room] : owner) {
			if (claims.count(node)) continue;
			const zenkit::BspNode& n = bsp.nodes[size_t(node)];
			SrcScene::RoomBoxSrc b;
			b.bbMin[0] = n.bbox.min.x, b.bbMin[1] = n.bbox.min.y, b.bbMin[2] = n.bbox.min.z;
			b.bbMax[0] = n.bbox.max.x, b.bbMax[1] = n.bbox.max.y, b.bbMax[2] = n.bbox.max.z;
			if (!(b.bbMax[0] > b.bbMin[0] && b.bbMax[2] > b.bbMin[2])) continue;
			b.room = room;
			boxes.push_back(b);
		}
		scene.notes.push_back("rooms: too many polygons, the leaves' boxes are used");
	} else if (polyBoxes + leafBoxes) {
		scene.notes.push_back("rooms: " + std::to_string(boxes.size()) + " boxes from the polygons of " + std::to_string(polyBoxes) + " leaves" +
		                      (leafBoxes ? ", " + std::to_string(leafBoxes) + " leaves with their own box" : std::string()));
	}
	for (const auto& b : boxes) scene.roomBoxes.push_back(b);
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
	scene.worldTriangles = scene.triangles.size();
	scene.indoor = world.world_bsp_tree.mode == zenkit::BspTreeType::INDOOR;
	collectRooms(world.world_bsp_tree, world.world_mesh, scene);

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
		appendObjects(world, game->vfs, version, opt, scene);
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
