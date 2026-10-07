// ZenGin importer test. No game data is needed: ZenKit is used to *write* two
// small worlds in the original formats (.ZEN files with mesh, BSP, objects,
// a model, a decal and level-change triggers inside a .VDF archive next to
// compiled -C.TEX textures, plus a loose .MSH), and the importer has to read
// them back exactly.
//
// Usage: test_zen <output dir> [ZenKit sample dir]
// The output directory is reused by the e2e test. With the sample directory
// (ZenKit's own tests/samples) a few genuine game files are parsed as well.
#include "bake.h"
#include "g3ds/math.h"
#include "g3ds/model.h"
#include "g3ds/pak.h"
#include "g3ds/world.h"
#include "importers.h"
#include "model_bake.h"
#include "pak_writer.h"

#include <zenkit/Archive.hh>
#include <zenkit/Mesh.hh>
#include <zenkit/ModelAnimation.hh>
#include <zenkit/ModelHierarchy.hh>
#include <zenkit/MultiResolutionMesh.hh>
#include <zenkit/Stream.hh>
#include <zenkit/Texture.hh>
#include <zenkit/Vfs.hh>
#include <zenkit/World.hh>
#include <zenkit/vobs/MovableObject.hh>
#include <zenkit/vobs/Trigger.hh>
#include <zenkit/vobs/Zone.hh>
#include <zenkit/world/WayNet.hh>

#include <squish.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace g3;

static int gFailures = 0, gChecks = 0;

#define CHECK(cond)                                                     \
	do {                                                                \
		++gChecks;                                                      \
		if (!(cond)) {                                                  \
			++gFailures;                                                \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
		}                                                               \
	} while (0)

namespace {

const int kGrid = 16;         // quads per side
const float kCell = 500.0f;   // cm
const u32 kLight = 0xFF804020; // zCOLOR 0xAARRGGBB
const uint8_t kLmLeft[3] = {200, 40, 40}, kLmRight[3] = {40, 40, 200};
const int kBoxTriangles = 12, kBoxInstances = 3;
// A model is the box (rigid part) plus one skinned triangle; there are two
// of them (.MDH + .MDM, and the same as one .MDL). The decal is one quad.
const int kModelTriangles = kBoxTriangles + 1, kModelInstances = 2, kDecalTriangles = 2;
// Plus one more box that is a door.
const int kObjectTriangles = kBoxTriangles * (kBoxInstances + 1) + kModelTriangles * kModelInstances + kDecalTriangles;

enum { MAT_GRASS, MAT_FENCE, MAT_WATER, MAT_NOTEX, MAT_PORTAL, MAT_COUNT };

float heightAt(int x, int z) { return 40.0f * std::sin(float(x) * 0.7f) + 30.0f * std::cos(float(z) * 0.5f); }

zenkit::Mesh buildMesh() {
	zenkit::Mesh mesh;
	mesh.name = "FIXTURE";

	const char* names[MAT_COUNT] = {"FIX_GRASS", "FIX_FENCE", "FIX_WATER", "FIX_NOTEX", "FIX_PORTAL"};
	// Mixed case on purpose: ZenGin file names are case-insensitive.
	const char* textures[MAT_COUNT] = {"Fix_Grass.TGA", "FIX_FENCE.TGA", "fix_water_a0.tga", "FIX_NOTEX.TGA", ""};
	for (int i = 0; i < MAT_COUNT; ++i) {
		zenkit::Material m;
		m.name = names[i];
		m.texture = textures[i];
		m.group = i == MAT_WATER ? zenkit::MaterialGroup::WATER : zenkit::MaterialGroup::EARTH;
		if (i == MAT_WATER) {
			// Two frames at 5 fps, drifting 0.2 repeats per second along u.
			m.texture_anim_fps = 5.0f;
			m.texture_anim_map_mode = zenkit::AnimationMapping::LINEAR;
			m.texture_anim_map_dir = zenkit::Vec2 {0.0002f, 0.0f};
		}
		mesh.materials.push_back(m);
	}

	// Shared lattice vertices; one feature per vertex.
	for (int z = 0; z <= kGrid; ++z)
		for (int x = 0; x <= kGrid; ++x) {
			mesh.vertices.push_back(zenkit::Vec3 {float(x) * kCell, heightAt(x, z), float(z) * kCell});
			zenkit::VertexFeature f {};
			f.texture = zenkit::Vec2 {float(x) * 2.0f, float(z) * 2.0f};
			f.light = kLight;
			f.normal = zenkit::Vec3 {0, 1, 0};
			mesh.features.push_back(f);
		}
	auto vid = [](int x, int z) { return uint32_t(z * (kGrid + 1) + x); };

	auto addPolygon = [&mesh](std::initializer_list<uint32_t> corners, uint32_t material, bool portal = false,
	                          int32_t lightmap = -1) {
		zenkit::Polygon p {};
		p.material = material;
		p.lightmap = lightmap;
		p.flags.is_portal = portal ? 1 : 0;
		p.index_count = corners.size();
		p.index_offset = mesh.polygon_vertex_indices.size();
		for (uint32_t c : corners) {
			mesh.polygon_vertex_indices.push_back(c);
			mesh.polygon_feature_indices.push_back(c);
		}
		mesh.geometry.push_back(p);
	};

	// Terrain, wound the way real ZenGin world meshes come out of ZenKit:
	// counter-clockwise when seen from above. Alternate between one 4-corner
	// polygon and two triangles so that the fan unpacking is exercised.
	for (int z = 0; z < kGrid; ++z)
		for (int x = 0; x < kGrid; ++x) {
			const uint32_t a = vid(x, z), b = vid(x + 1, z), c = vid(x + 1, z + 1), d = vid(x, z + 1);
			const uint32_t material = (x >= 10 && z >= 10) ? MAT_WATER : ((x == 3 && z == 3) ? MAT_NOTEX : MAT_GRASS);
			if ((x + z) % 2 == 0) {
				addPolygon({a, b, c, d}, material, false, material == MAT_NOTEX ? 0 : -1);
			} else {
				addPolygon({a, b, c}, material);
				addPolygon({a, c, d}, material);
			}
		}

	// One lightmap covering the cell (3, 3): left half reddish, right half
	// bluish. Axes are stored divided by their squared length, order: up, right.
	{
		std::vector<uint8_t> px(8 * 8 * 4);
		for (int y = 0; y < 8; ++y)
			for (int x = 0; x < 8; ++x) {
				uint8_t* q = &px[size_t(y * 8 + x) * 4];
				q[0] = x < 4 ? kLmLeft[0] : kLmRight[0];
				q[1] = x < 4 ? kLmLeft[1] : kLmRight[1];
				q[2] = x < 4 ? kLmLeft[2] : kLmRight[2];
				q[3] = 255;
			}
		zenkit::TextureBuilder builder {8, 8};
		builder.add_mipmap(px, zenkit::TextureFormat::R8G8B8A8);
		zenkit::LightMap lm;
		lm.image = std::make_shared<zenkit::Texture>(builder.build(zenkit::TextureFormat::R8G8B8A8));
		lm.origin = zenkit::Vec3 {3 * kCell, 0, 3 * kCell};
		lm.normals[0] = zenkit::Vec3 {0, 0, 1.0f / kCell}; // up: +Z across the cell
		lm.normals[1] = zenkit::Vec3 {1.0f / kCell, 0, 0}; // right: +X across the cell
		mesh.lightmaps.push_back(lm);
	}

	// A portal polygon: must never reach the output.
	addPolygon({vid(0, 0), vid(0, 1), vid(1, 1)}, MAT_PORTAL, true);

	// A standing fence quad with its own vertices and UVs.
	const uint32_t base = uint32_t(mesh.vertices.size());
	const float fx[4] = {1000, 2000, 2000, 1000}, fy[4] = {300, 300, 0, 0}, fu[4] = {0, 4, 4, 0}, fv[4] = {0, 0, 1, 1};
	for (int k = 0; k < 4; ++k) {
		mesh.vertices.push_back(zenkit::Vec3 {fx[k], fy[k] + 60.0f, 2250.0f});
		zenkit::VertexFeature f {};
		f.texture = zenkit::Vec2 {fu[k], fv[k]};
		f.light = 0xFF112233;
		mesh.features.push_back(f);
	}
	addPolygon({base, base + 1, base + 2, base + 3}, MAT_FENCE);
	return mesh;
}

int expectedTriangles() { return kGrid * kGrid * 2 + 2; }

// A 100 x 200 x 200 cm box standing on its origin. `inward` winds it inside
// out, which the importer has to detect and repair.
zenkit::MultiResolutionMesh buildBox(bool inward) {
	zenkit::MultiResolutionMesh mrm;
	for (int i = 0; i < 8; ++i)
		mrm.positions.push_back(zenkit::Vec3 {(i & 1) ? 50.0f : -50.0f, (i & 2) ? 200.0f : 0.0f, (i & 4) ? 100.0f : -100.0f});
	zenkit::SubMesh sub;
	sub.mat.name = "FIX_BOX_MAT";
	sub.mat.texture = "FIX_GRASS.TGA";
	static const int faces[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
	static const float normals[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
	for (int f = 0; f < 6; ++f) {
		int c[4] = {faces[f][0], faces[f][1], faces[f][2], faces[f][3]};
		const zenkit::Vec3 &p0 = mrm.positions[size_t(c[0])], &p1 = mrm.positions[size_t(c[1])], &p2 = mrm.positions[size_t(c[2])];
		const float ax = p1.x - p0.x, ay = p1.y - p0.y, az = p1.z - p0.z, bx = p2.x - p0.x, by = p2.y - p0.y, bz = p2.z - p0.z;
		// Front normal of a counter-clockwise triangle in a left-handed system.
		const float fn[3] = {-(ay * bz - az * by), -(az * bx - ax * bz), -(ax * by - ay * bx)};
		const bool outward = fn[0] * normals[f][0] + fn[1] * normals[f][1] + fn[2] * normals[f][2] > 0;
		if (outward == inward) std::swap(c[1], c[3]);
		const uint16_t base = uint16_t(sub.wedges.size());
		for (int k = 0; k < 4; ++k) {
			zenkit::MeshWedge w {};
			w.normal = zenkit::Vec3 {normals[f][0], normals[f][1], normals[f][2]};
			w.texture = zenkit::Vec2 {float(k == 1 || k == 2), float(k >= 2)};
			w.index = uint16_t(c[k]);
			sub.wedges.push_back(w);
		}
		sub.triangles.push_back(zenkit::MeshTriangle {{base, uint16_t(base + 1), uint16_t(base + 2)}});
		sub.triangles.push_back(zenkit::MeshTriangle {{base, uint16_t(base + 2), uint16_t(base + 3)}});
	}
	mrm.materials.push_back(sub.mat);
	mrm.sub_meshes.push_back(sub);
	return mrm;
}

// The body of a compiled mesh the way the game's files are laid out (and the
// way ZenKit's reader, which is tested against real files, expects it).
// ZenKit's own MultiResolutionMesh::save cannot be used at the pinned
// revision: it stores byte sizes where the reader expects element counts.
std::vector<std::byte> mrmSection(const zenkit::MultiResolutionMesh& mesh) {
	std::vector<std::byte> section;
	auto w = zenkit::Write::to(&section);
	w->write_ushort(0x305); // Gothic 1
	const auto sizeAt = w->tell();
	w->write_uint(0);
	const auto content = w->tell();
	auto here = [&] { return uint32_t(w->tell() - content); };

	const uint32_t positionsAt = here();
	for (const zenkit::Vec3& p : mesh.positions) w->write_vec3(p);
	const uint32_t normalsAt = here();

	struct Span {
		uint32_t offset, count;
	};
	std::vector<std::vector<Span>> spans;
	for (const zenkit::SubMesh& sub : mesh.sub_meshes) {
		std::vector<Span> sp;
		sp.push_back({here(), uint32_t(sub.triangles.size())});
		for (const zenkit::MeshTriangle& t : sub.triangles)
			for (uint16_t i : t.wedges) w->write_ushort(i);
		sp.push_back({here(), uint32_t(sub.wedges.size())});
		for (const zenkit::MeshWedge& wd : sub.wedges) {
			w->write_vec3(wd.normal);
			w->write_vec2(wd.texture);
			w->write_ushort(wd.index);
			w->write_ushort(0);
		}
		// colours, plane indices, planes, wedge map, vertex updates, triangle
		// edges, edges, edge scores: progressive-mesh data we do not need.
		for (int i = 0; i < 8; ++i) sp.push_back({here(), 0});
		spans.push_back(sp);
	}
	const auto end = w->tell();
	w->seek(static_cast<ssize_t>(sizeAt), zenkit::Whence::BEG);
	w->write_uint(uint32_t(end - content));
	w->seek(static_cast<ssize_t>(end), zenkit::Whence::BEG);

	w->write_ubyte(uint8_t(mesh.sub_meshes.size()));
	w->write_uint(positionsAt);
	w->write_uint(uint32_t(mesh.positions.size()));
	w->write_uint(normalsAt);
	w->write_uint(0);
	for (const auto& sp : spans)
		for (const Span& s : sp) {
			w->write_uint(s.offset);
			w->write_uint(s.count);
		}

	auto ar = zenkit::WriteArchive::to(w.get(), zenkit::ArchiveFormat::BINARY);
	for (const zenkit::SubMesh& sub : mesh.sub_meshes) {
		ar->write_string("", sub.mat.name);
		ar->write_object("%", &sub.mat, zenkit::GameVersion::GOTHIC_1);
	}
	ar->write_header();

	mesh.bbox.save(w.get());
	mesh.obbox.save(w.get());
	const char padding[16] = {};
	w->write(padding, sizeof(padding));
	return section;
}

void writeChunk(zenkit::Write* out, uint16_t type, const std::vector<std::byte>& payload) {
	out->write_ushort(type);
	out->write_uint(uint32_t(payload.size()));
	if (!payload.empty()) out->write(payload.data(), payload.size());
}

// A compiled .MRM file: the mesh chunk and an end chunk.
void writeMrm(const zenkit::MultiResolutionMesh& mesh, zenkit::Write* out) {
	writeChunk(out, 0xB100, mrmSection(mesh));
	writeChunk(out, 0xB1FF, {});
}

// ---- a small model: a skeleton of three bones, a rigid part and a skinned one
//
//   0 "BIP01 GATE"  root; its translation (file + root translation) is dropped,
//                   because ZenGin puts a world object's root at its origin
//   1 "BIP01 DOOR"  child of 0: a quarter turn around Y, 50 cm up
//   2 "BIP01 ARM"   child of 1: 100 cm along the door's own X
// In model space: door(p) = (p.z, p.y + 50, -p.x), arm(p) = door(p) + (0, 0, -100).

const float kRootShift[3] = {15.0f, 20.0f, 30.0f}; // file translation (10, 20, 30) + root translation (5, 0, 0)

zenkit::ModelHierarchy buildHierarchy() {
	zenkit::ModelHierarchy h;
	auto node = [&](const char* name, int parent, bool turn, float x, float y, float z) {
		zenkit::ModelHierarchyNode n;
		n.name = name;
		n.parent_index = int16_t(parent);
		n.transform.columns[0] = turn ? zenkit::Vec4 {0, 0, -1, 0} : zenkit::Vec4 {1, 0, 0, 0};
		n.transform.columns[1] = zenkit::Vec4 {0, 1, 0, 0};
		n.transform.columns[2] = turn ? zenkit::Vec4 {1, 0, 0, 0} : zenkit::Vec4 {0, 0, 1, 0};
		n.transform.columns[3] = zenkit::Vec4 {x, y, z, 1};
		h.nodes.push_back(n);
	};
	// Children before their parent on purpose: files do not guarantee an order.
	node("BIP01 ARM", 1, false, 100, 0, 0);
	node("BIP01 DOOR", 2, true, 0, 50, 0);
	node("BIP01 GATE", -1, false, 10, 20, 30);
	h.root_translation = zenkit::Vec3 {5, 0, 0};
	h.checksum = 0;
	return h;
}
enum { NODE_ARM = 0, NODE_DOOR = 1, NODE_GATE = 2 };

// Where the three skinned vertices end up in model space (root at the origin).
const float kSkinExpected[3][3] = {
    {0, 50, -110},   // arm, local (10, 0, 0)
    {10, 55, -50},   // half arm local (0, 0, 20) = (20, 50, -100), half door local (0, 10, 0) = (0, 60, 0)
    {30, 300, 0},    // root, local (30, 300, 0)
};

std::vector<std::byte> buildSkinChunks() {
	// The mesh stores the vertices where they are in the file's own pose,
	// which includes the root translation the importer removes.
	zenkit::MultiResolutionMesh mrm;
	for (const auto& p : kSkinExpected)
		mrm.positions.push_back(zenkit::Vec3 {p[0] + kRootShift[0], p[1] + kRootShift[1], p[2] + kRootShift[2]});
	zenkit::SubMesh sub;
	sub.mat.name = "FIX_SKIN_MAT";
	sub.mat.texture = "FIX_FENCE.TGA";
	for (uint16_t i = 0; i < 3; ++i) {
		zenkit::MeshWedge w {};
		w.normal = zenkit::Vec3 {0, 1, 0};
		w.texture = zenkit::Vec2 {float(i == 1), float(i == 2)};
		w.index = i;
		sub.wedges.push_back(w);
	}
	sub.triangles.push_back(zenkit::MeshTriangle {{0, 1, 2}});
	mrm.sub_meshes.push_back(sub);

	struct Weight {
		float weight, x, y, z;
		uint8_t node;
	};
	const std::vector<std::vector<Weight>> weights = {
	    {{1.0f, 10, 0, 0, NODE_ARM}},
	    {{0.5f, 0, 0, 20, NODE_ARM}, {0.5f, 0, 10, 0, NODE_DOOR}},
	    {{1.0f, 30, 300, 0, NODE_GATE}},
	};
	std::vector<std::byte> nodes;
	{
		auto w = zenkit::Write::to(&nodes);
		uint32_t bytes = 0;
		for (const auto& v : weights) bytes += 4 + uint32_t(v.size()) * 17;
		w->write_uint(bytes);
		for (const auto& v : weights) {
			w->write_uint(uint32_t(v.size()));
			for (const Weight& e : v) {
				w->write_float(e.weight);
				w->write_vec3(zenkit::Vec3 {e.x, e.y, e.z});
				w->write_ubyte(e.node);
			}
		}
		w->write_uint(0);   // wedge normals
		w->write_ushort(3); // bones used
		for (int i = 0; i < 3; ++i) w->write_int(i);
		for (int i = 0; i < 3; ++i) {
			for (int f = 0; f < 15; ++f) w->write_float(0); // oriented bounding box
			w->write_ushort(0);                             // without children
		}
	}
	std::vector<std::byte> out;
	auto w = zenkit::Write::to(&out);
	std::vector<std::byte> version;
	zenkit::Write::to(&version)->write_uint(4);
	writeChunk(w.get(), 0xE100, version);
	writeChunk(w.get(), 0xB100, mrmSection(mrm));
	writeChunk(w.get(), 0xB1FF, nodes);
	writeChunk(w.get(), 0xE110, {});
	return out;
}

// A compiled .MDM: one rigid mesh attached to "BIP01 DOOR", one skinned mesh.
std::vector<std::byte> buildMdm(const zenkit::MultiResolutionMesh& rigid) {
	std::vector<std::byte> out;
	auto w = zenkit::Write::to(&out);
	std::vector<std::byte> payload;
	zenkit::Write::to(&payload)->write_uint(0x04030506);
	writeChunk(w.get(), 0xD000, payload);
	payload.clear();
	{
		auto p = zenkit::Write::to(&payload);
		p->write_ushort(1);
		p->write_line("Bip01 Door"); // names are matched without regard to case
	}
	writeChunk(w.get(), 0xD020, payload);
	writeChunk(w.get(), 0xB100, mrmSection(rigid));
	payload.clear();
	{
		auto p = zenkit::Write::to(&payload);
		p->write_uint(0);   // checksum
		p->write_ushort(1); // skinned meshes; they follow the chunk instead of sitting inside it
	}
	writeChunk(w.get(), 0xD030, payload);
	const std::vector<std::byte> skin = buildSkinChunks();
	w->write(skin.data(), skin.size());
	writeChunk(w.get(), 0xD040, {});
	return out;
}

// ---- a small character: skeleton, skinned body, animations and a script ---
//
// Five nodes with rotations like the real biped's (a quarter turn in the
// root, the spine's own X pointing up):
//   0 "BIP01"          root, hips 90 cm above the ground, a quarter turn about Y
//   1 "BIP01 SPINE"    10 cm up, turned 120 degrees about (1, 1, 1)
//   2 "BIP01 HEAD"     50 cm along the spine's X: (0, 150, 0) in model space
//   3 "BIP01 L FOOT"   (-10, 5, 0) in model space
//   4 "BIP01 L TOE0"   12 cm in front of the foot
// Everything below is written in the usual maths convention (g3ds/math.h);
// the files get ZenGin's.

enum { HN_ROOT, HN_SPINE, HN_HEAD, HN_FOOT, HN_TOE, HN_COUNT };
const char* const kHeroNodeNames[HN_COUNT] = {"Bip01", "Bip01 Spine", "Bip01 Head", "Bip01 L Foot", "Bip01 L Toe0"};
const int kHeroParents[HN_COUNT] = {-1, HN_ROOT, HN_SPINE, HN_ROOT, HN_FOOT};

Quat heroRestRot(int node) {
	if (node == HN_ROOT) return quatFromYaw(1.5707963f);
	if (node == HN_SPINE) return {0.5f, 0.5f, 0.5f, 0.5f};
	return quatIdentity();
}

Vec3 heroRestPos(int node) {
	// The root's quarter turn maps (a, b, c) to (c, b, -a); its children's
	// offsets are given in model space and turned back into the root's.
	auto intoRoot = [](Vec3 m) { return Vec3 {-m.z, m.y, m.x}; };
	switch (node) {
	case HN_ROOT: return {0, 90, 0};
	case HN_SPINE: return intoRoot({0, 10, 0});
	case HN_HEAD: return {50, 0, 0};
	case HN_FOOT: return intoRoot({-10, -85, 0});
	default: return intoRoot({0, -5, 12});
	}
}

void heroRestWorld(Mat34 world[HN_COUNT]) {
	for (int i = 0; i < HN_COUNT; ++i) {
		const Mat34 local = mat34From(heroRestRot(i), heroRestPos(i));
		world[i] = kHeroParents[i] < 0 ? local : mul(world[kHeroParents[i]], local);
	}
}

// `turned`: the same character built facing -Z (the root turned half round).
Quat halfTurn(Quat q) { return quatMul(quatFromYaw(3.14159265f), q); }

zenkit::ModelHierarchy buildHeroHierarchy(bool turned) {
	zenkit::ModelHierarchy h;
	for (int i = 0; i < HN_COUNT; ++i) {
		zenkit::ModelHierarchyNode n;
		n.name = kHeroNodeNames[i];
		n.parent_index = int16_t(kHeroParents[i]);
		Vec3 pos = heroRestPos(i);
		if (i == HN_ROOT) pos = {0, 0, 0}; // the hips' height travels in the root translation
		const Mat34 m = mat34From(turned && i == HN_ROOT ? halfTurn(heroRestRot(i)) : heroRestRot(i), pos);
		for (int c = 0; c < 4; ++c) n.transform.columns[c] = zenkit::Vec4 {m.m[0][c], m.m[1][c], m.m[2][c], c == 3 ? 1.0f : 0.0f};
		h.nodes.push_back(n);
	}
	h.root_translation = zenkit::Vec3 {0, 90, 0};
	h.checksum = 0;
	return h;
}

// Model-space rest positions of the body's four vertices: a tetrahedron, its
// faces wound so that their fronts point outwards.
const float kHeroVertex[4][3] = {{0, 150, 0}, {70, 60, -50}, {-70, 5, -20}, {10, 0, 80}};
const uint16_t kHeroTris[4][3] = {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}};

std::vector<std::byte> buildHeroMdm(bool insideOut) {
	Mat34 world[HN_COUNT];
	heroRestWorld(world);
	zenkit::MultiResolutionMesh mrm;
	for (const auto& p : kHeroVertex) mrm.positions.push_back(zenkit::Vec3 {p[0], p[1], p[2]});
	zenkit::SubMesh sub;
	sub.mat.name = "FIX_HERO_SKIN";
	sub.mat.texture = "Fix_Skin_V0_C0.TGA";
	for (uint16_t i = 0; i < 4; ++i) {
		zenkit::MeshWedge w {};
		w.normal = zenkit::Vec3 {0, 0, 1};
		w.texture = zenkit::Vec2 {float(i & 1), float(i >> 1)};
		w.index = i;
		sub.wedges.push_back(w);
	}
	for (const auto& t : kHeroTris)
		sub.triangles.push_back(insideOut ? zenkit::MeshTriangle {{t[0], t[2], t[1]}} : zenkit::MeshTriangle {{t[0], t[1], t[2]}});
	mrm.sub_meshes.push_back(sub);

	// Vertex 1 hangs on three bones; the weakest must be dropped on import.
	struct Weight {
		float weight;
		int node;
	};
	const std::vector<std::vector<Weight>> weights = {
	    {{1.0f, HN_HEAD}}, {{0.6f, HN_SPINE}, {0.3f, HN_HEAD}, {0.1f, HN_ROOT}}, {{1.0f, HN_FOOT}}, {{1.0f, HN_TOE}}};
	std::vector<std::byte> nodes;
	{
		auto w = zenkit::Write::to(&nodes);
		uint32_t bytes = 0;
		for (const auto& v : weights) bytes += 4 + uint32_t(v.size()) * 17;
		w->write_uint(bytes);
		for (size_t i = 0; i < weights.size(); ++i) {
			w->write_uint(uint32_t(weights[i].size()));
			for (const Weight& e : weights[i]) {
				// The same model-space point, as each bone sees it.
				const Vec3 local = transformPoint(invertRigid(world[e.node]), {kHeroVertex[i][0], kHeroVertex[i][1], kHeroVertex[i][2]});
				w->write_float(e.weight);
				w->write_vec3(zenkit::Vec3 {local.x, local.y, local.z});
				w->write_ubyte(uint8_t(e.node));
			}
		}
		w->write_uint(0); // wedge normals
		w->write_ushort(HN_COUNT);
		for (int i = 0; i < HN_COUNT; ++i) w->write_int(i);
		for (int i = 0; i < HN_COUNT; ++i) {
			for (int f = 0; f < 15; ++f) w->write_float(0);
			w->write_ushort(0);
		}
	}
	std::vector<std::byte> out;
	auto w = zenkit::Write::to(&out);
	std::vector<std::byte> payload;
	zenkit::Write::to(&payload)->write_uint(0x04030506);
	writeChunk(w.get(), 0xD000, payload);
	payload.clear();
	zenkit::Write::to(&payload)->write_ushort(0); // no rigid parts
	writeChunk(w.get(), 0xD020, payload);
	payload.clear();
	{
		auto p = zenkit::Write::to(&payload);
		p->write_uint(0);
		p->write_ushort(1);
	}
	writeChunk(w.get(), 0xD030, payload);
	payload.clear();
	zenkit::Write::to(&payload)->write_uint(4);
	writeChunk(w.get(), 0xE100, payload);
	writeChunk(w.get(), 0xB100, mrmSection(mrm));
	writeChunk(w.get(), 0xB1FF, nodes);
	writeChunk(w.get(), 0xE110, {});
	writeChunk(w.get(), 0xD040, {});
	return out;
}

// What the clips are meant to do, frame by frame.
struct HeroSample {
	Vec3 pos;
	Quat rot;
};
const uint32_t kHeroTracks[3] = {HN_ROOT, HN_SPINE, HN_HEAD}; // the foot and the toe are not animated

HeroSample heroSample(int clip, int frame, int track) {
	const float f = float(frame);
	if (track == 0) {
		// The root walks away from the origin (100 cm per frame along Z, 5 along X) and bobs.
		return {{5.0f * f, 90.0f + f, 100.0f * f}, quatFromYaw(1.5707963f + 0.1f * f + 0.05f * float(clip))};
	}
	if (track == 1) {
		const Quat bend = {std::sin(0.05f * f), 0, 0, std::cos(0.05f * f)};
		return {heroRestPos(HN_SPINE), quatMul(heroRestRot(HN_SPINE), bend)};
	}
	const Quat nod = {0, 0, std::sin(0.1f * f + 0.02f), std::cos(0.1f * f + 0.02f)};
	return {heroRestPos(HN_HEAD), nod};
}

// A compiled .MAN. ZenGin stores a rotation as three 16-bit components of a
// quaternion with positive w - and turning the other way round than its node
// matrices. `zenGinConvention` = false writes them unturned instead, to see
// whether the importer notices.
std::vector<std::byte> buildMan(const char* name, const char* next, float fps, int clip, int frames, bool zenGinConvention,
                                bool turned) {
	auto sampleOf = [&](int f, int t) {
		HeroSample s = heroSample(clip, f, t);
		if (turned && t == 0) {
			s.pos = {-s.pos.x, s.pos.y, -s.pos.z};
			s.rot = halfTurn(s.rot);
		}
		return s;
	};
	const float rotScale = (1.0f / 65535.0f) * 2.1f;
	float lo = 1e30f, hi = -1e30f;
	for (int f = 0; f < frames; ++f)
		for (int t = 0; t < 3; ++t) {
			const HeroSample s = sampleOf(f, t);
			for (float c : {s.pos.x, s.pos.y, s.pos.z}) {
				lo = std::min(lo, c);
				hi = std::max(hi, c);
			}
		}
	const float posScale = hi > lo ? (hi - lo) / 65535.0f : 1.0f;
	std::vector<std::byte> out;
	auto w = zenkit::Write::to(&out);
	std::vector<std::byte> payload;
	{
		auto p = zenkit::Write::to(&payload);
		p->write_ushort(0xC);
		p->write_line(name);
		p->write_uint(1); // layer
		p->write_uint(uint32_t(frames));
		p->write_uint(3);
		p->write_float(fps);
		p->write_float(25.0f);
		p->write_float(lo);
		p->write_float(posScale);
		for (int i = 0; i < 6; ++i) p->write_float(0); // bounding box
		p->write_line(next);
	}
	writeChunk(w.get(), 0xA020, payload);
	payload.clear();
	{
		auto p = zenkit::Write::to(&payload);
		p->write_uint(0); // checksum
		for (uint32_t node : kHeroTracks) p->write_uint(node);
		for (int f = 0; f < frames; ++f)
			for (int t = 0; t < 3; ++t) {
				const HeroSample s = sampleOf(f, t);
				Quat q = s.rot;
				if (zenGinConvention) q = {-q.x, -q.y, -q.z, q.w};
				if (q.w < 0) q = {-q.x, -q.y, -q.z, -q.w};
				for (float c : {q.x, q.y, q.z}) p->write_ushort(uint16_t(std::lround(c / rotScale + 32767.0f)));
				for (float c : {s.pos.x, s.pos.y, s.pos.z}) p->write_ushort(uint16_t(std::lround((c - lo) / posScale)));
			}
	}
	writeChunk(w.get(), 0xA090, payload);
	return out;
}

const char kHeroScript[] =
    "Model (\"FIX_HERO\")\n{\n"
    "\tmeshAndTree (\"Fix_Hero_Body.ASC\" DONT_USE_MESH)\n"
    "\tregisterMesh (\"Fix_Hero_Body.ASC\")\n"
    "\taniEnum\n\t{\n"
    "\t\tani (\"s_Run\" 1 \"s_Run\" 0.1 0.2 M. \"Fix_Hero_M01.asc\" F 0 2 FPS:10)\n"
    "\t\tani (\"s_WalkL\" 1 \"s_WalkL\" 0.0 0.1 M. \"Fix_Hero_M02.asc\" F 0 3 FPS:25)\n"
    "\t\taniAlias (\"t_JumpB\" 1 \"s_Run\" 0.2 0.3 M. \"s_WalkL\" R)\n"
    "\t\taniAlias (\"s_Walk\" 1 \"s_Walk\" 0.0 0.0 M. \"t_JumpB\" R)\n"
    "\t}\n}\n";

void yellowPixel(int, int, uint8_t* p) {
	p[0] = 230; p[1] = 200; p[2] = 40; p[3] = 255;
}

void bluePixel(int, int, uint8_t* p) {
	p[0] = 40; p[1] = 60; p[2] = 220; p[3] = 255;
}

float quatDistance(Quat a, const float b[4]) {
	const float d = std::fabs(a.x * b[0] + a.y * b[1] + a.z * b[2] + a.w * b[3]);
	return 2.0f * std::acos(std::min(d, 1.0f)) * 57.29578f;
}

bool modelNote(const SrcModel& model, const char* text) {
	for (const std::string& n : model.notes)
		if (n.find(text) != std::string::npos) return true;
	return false;
}

struct HeroCase {
	const char* script;
	const char* body;
	bool zenGinConvention; // rotations stored the way ZenGin stores them
	bool insideOut;        // the body's triangles wound the wrong way
	bool turned;           // the character built facing -Z
};

void checkHero(std::shared_ptr<const ZenGame> game, const HeroCase& hc, const fs::path& dir) {
	const bool zenGinConvention = hc.zenGinConvention;
	ZenActorOptions opt;
	opt.script = hc.script;
	opt.body = hc.body;
	opt.head = ""; // ZenKit cannot write morph meshes; the head is covered by the sample files
	SrcModel model;
	CHECK(importZenActor(game, opt, model));
	for (const std::string& n : model.notes) std::printf("  %s\n", n.c_str());

	// Skeleton.
	CHECK(model.nodes.size() == HN_COUNT);
	if (model.nodes.size() != HN_COUNT) return;
	for (int i = 0; i < HN_COUNT; ++i) {
		const SrcNode& n = model.nodes[size_t(i)];
		std::string upperName = kHeroNodeNames[i];
		for (char& c : upperName) c = char(std::toupper(static_cast<unsigned char>(c)));
		CHECK(n.name == upperName && n.parent == kHeroParents[i]);
		const Vec3 pos = heroRestPos(i);
		CHECK(std::fabs(n.pos[0] - pos.x) < 1e-3f && std::fabs(n.pos[1] - pos.y) < 1e-3f && std::fabs(n.pos[2] - pos.z) < 1e-3f);
		CHECK(quatDistance(heroRestRot(i), n.rot) < 0.1f);
	}
	if (hc.turned) {
		CHECK(modelNote(model, "character turned by 180 degrees to face +Z (S_WALKL carries it 300 cm along -Z)"));
	} else {
		CHECK(modelNote(model, "character faces +Z (S_WALKL carries it 300 cm along +Z)"));
		CHECK(modelNote(model, "rest pose within 0.0 cm"));
	}
	CHECK(modelNote(model, hc.insideOut ? "character mesh: 4 triangles, winding flipped (score -0.147)"
	                                    : "character mesh: 4 triangles, winding kept (score 0.147)"));
	CHECK(modelNote(model, "1 of 4 skin vertices hang on more than two bones"));

	// Body: four triangles, fronts outwards; the skin variant of the texture;
	// two bones per vertex.
	CHECK(model.parts.size() == 1);
	if (model.parts.size() != 1) return;
	const SrcModelPart& part = model.parts[0];
	CHECK(part.material.texture == "FIX_SKIN_V4_C1.TGA" && modelNote(model, "character skin: FIX_SKIN_V4_C1.TGA"));
	CHECK(part.verts.size() == 12);
	int corner[12];
	for (int t = 0; t < 4; ++t)
		for (int k = 0; k < 3; ++k) corner[t * 3 + k] = kHeroTris[t][k];
	for (size_t k = 0; k < part.verts.size() && k < 12; ++k) {
		const SrcSkinVertex& v = part.verts[k];
		const float* want = kHeroVertex[corner[k]];
		CHECK(std::fabs(v.pos[0] - want[0]) < 1e-3f && std::fabs(v.pos[1] - want[1]) < 1e-3f && std::fabs(v.pos[2] - want[2]) < 1e-3f);
		if (corner[k] == 1) {
			CHECK(v.bone[0] == HN_SPINE && v.bone[1] == HN_HEAD && std::fabs(v.weight1 - 0.3f / 0.9f) < 1e-4f);
		} else {
			const int single[4] = {HN_HEAD, -1, HN_FOOT, HN_TOE};
			CHECK(v.bone[0] == single[corner[k]] && v.weight1 == 0.0f);
		}
	}

	// Clips: the two animations and the alias; everything else is reported missing.
	CHECK(model.clips.size() == 4);
	CHECK(modelNote(model, zenGinConvention ? "rotations read conjugated" : "rotations read as stored"));
	CHECK(modelNote(model, "animations not found") && modelNote(model, "S_SWIMF"));
	CHECK(modelNote(model, "hips 90.0 cm above the ground at rest, 90.0 cm in S_RUN"));
	const SrcClip* run = nullptr;
	const SrcClip* walk = nullptr;
	const SrcClip* back = nullptr;
	const SrcClip* again = nullptr;
	for (const SrcClip& c : model.clips) {
		if (c.name == "S_WALK") again = &c;
		if (c.name == "S_RUN") run = &c;
		if (c.name == "S_WALKL") walk = &c;
		if (c.name == "T_JUMPB") back = &c;
	}
	CHECK(run && walk && back && again);
	if (!run || !walk || !back || !again) return;
	CHECK(run->next == "S_RUN" && run->moves && std::fabs(run->fps - 10.0f) < 1e-4f && run->samples.size() == 3 * 3);
	CHECK(std::fabs(run->blendIn - 0.1f) < 1e-4f && std::fabs(run->blendOut - 0.2f) < 1e-4f);
	CHECK(walk->samples.size() == 4 * 3 && std::fabs(walk->fps - 25.0f) < 1e-4f);
	CHECK(back->next == "S_RUN" && back->samples.size() == 4 * 3 && std::fabs(back->blendIn - 0.2f) < 1e-4f);
	float worstRot = 0, worstPos = 0;
	auto compare = [&](const SrcClip& c, int clip, int frames, bool reversed) {
		CHECK(c.nodes.size() == 3 && c.nodes[0] == HN_ROOT && c.nodes[1] == HN_SPINE && c.nodes[2] == HN_HEAD);
		for (int f = 0; f < frames; ++f)
			for (int t = 0; t < 3; ++t) {
				const HeroSample want = heroSample(clip, reversed ? frames - 1 - f : f, t);
				const SrcClipSample& got = c.samples[size_t(f * 3 + t)];
				worstRot = std::max(worstRot, quatDistance(want.rot, got.rot));
				worstPos = std::max({worstPos, std::fabs(got.pos[0] - want.pos.x), std::fabs(got.pos[1] - want.pos.y),
				                     std::fabs(got.pos[2] - want.pos.z)});
			}
	};
	compare(*run, 0, 3, false);
	compare(*walk, 1, 4, false);
	compare(*back, 1, 4, true); // the alias plays the walk backwards
	compare(*again, 1, 4, false); // an alias of that alias, backwards again: the walk as it was
	CHECK(again->next == "S_WALK");
	std::printf("  clips: rotations within %.3f degrees, positions within %.4f cm of what was written\n", double(worstRot),
	            double(worstPos));
	CHECK(worstRot < 0.05f && worstPos < 0.01f);

	// Through the baker: the root's travel becomes a speed and leaves the samples.
	BakeOptions bo;
	bo.tex.maxSize = 32;
	bo.tex.quality = 0;
	bo.quiet = true;
	PakWriter writer;
	const ModelBakeResult baked = bakeModel(model, bo, writer);
	CHECK(baked.clips == 4 && baked.triangles == 4 && baked.parts == 1 && baked.largestPalette == 4 && baked.textures == 1);
	const std::string pakPath = (dir / "hero.pak").string();
	writer.write(pakPath);
	Pak pak;
	CHECK(pak.open(pakPath.c_str()));
	const PakEntry* e = pak.find("anim/hero/s_run");
	CHECK(e && e->type == ASSET_CLIP);
	if (e) {
		std::vector<u8> blob(e->size);
		CHECK(pak.read(*e, blob.data()));
		ClipHeader h;
		std::memcpy(&h, blob.data(), sizeof(h));
		CHECK(h.magic == CLIP_MAGIC && h.trackCount == 3 && h.frameCount == 3 && (h.flags & CLIPF_LOOP) && (h.flags & CLIPF_MOVES));
		// 100.12 cm per frame at 10 frames per second.
		CHECK(std::fabs(h.moveSpeed - std::sqrt(100.0f * 100.0f + 5.0f * 5.0f) * 10.0f) < 1.0f);
		CHECK(h.next == hashName("S_RUN"));
		const ClipSample* samples = reinterpret_cast<const ClipSample*>(blob.data() + sizeof(ClipHeader) + clipTrackTableSize(3));
		for (int f = 0; f < 3; ++f) {
			// The root stays over the origin (to what the file's 16-bit positions allow) ...
			CHECK(std::fabs(float(samples[f * 3].pos[0]) * h.posScale) < 0.02f && std::fabs(float(samples[f * 3].pos[2]) * h.posScale) < 0.02f);
			CHECK(std::fabs(float(samples[f * 3].pos[1]) * h.posScale - (90.0f + float(f))) < 0.01f); // ... but bobs
		}
	}
	CHECK(pak.find("model/hero") && pak.find("tex/fix_skin_v4_c1") && !pak.find("tex/fix_skin_v0_c0"));
}

std::shared_ptr<zenkit::VirtualObject> makeObject(const char* visual, float x, float y, float z, bool solid, bool visible = true) {
	auto vob = std::make_shared<zenkit::VirtualObject>();
	const std::string name = visual;
	const std::string ext = name.size() > 4 ? name.substr(name.size() - 4) : "";
	std::shared_ptr<zenkit::Visual> vis;
	if (ext == ".MDS" || ext == ".mds") {
		vis = std::make_shared<zenkit::VisualModel>();
		vis->type = zenkit::VisualType::MODEL;
	} else if (ext == ".PFX") {
		vis = std::make_shared<zenkit::VisualParticleEffect>();
		vis->type = zenkit::VisualType::PARTICLE_EFFECT;
	} else if (ext == ".MMS") {
		vis = std::make_shared<zenkit::VisualMorphMesh>();
		vis->type = zenkit::VisualType::MORPH_MESH;
	} else {
		vis = std::make_shared<zenkit::VisualMesh>();
		vis->type = zenkit::VisualType::MESH;
	}
	vis->name = visual;
	vob->visual = vis;
	vob->show_visual = visible;
	vob->position = zenkit::Vec3 {x, y, z};
	vob->cd_dynamic = solid;
	return vob;
}

std::vector<std::byte> saveTexture(int size, zenkit::TextureFormat format, void (*pixel)(int, int, uint8_t*)) {
	std::vector<uint8_t> rgba(size_t(size) * size * 4);
	for (int y = 0; y < size; ++y)
		for (int x = 0; x < size; ++x) pixel(x, y, &rgba[(size_t(y) * size + x) * 4]);
	// Compress with libsquish ourselves (the library ZenKit uses to *read* DXT).
	// At the pinned ZenKit revision, TextureBuilder's own RGBA -> DXT conversion
	// writes into an unsized buffer and crashes; handing it finished DXT blocks
	// avoids that code path and still yields a genuine DXT .TEX file.
	const int flags = format == zenkit::TextureFormat::DXT1 ? squish::kDxt1 : squish::kDxt3;
	std::vector<uint8_t> dxt(size_t(squish::GetStorageRequirements(size, size, flags)));
	squish::CompressImage(rgba.data(), size, size, dxt.data(), flags);
	zenkit::TextureBuilder builder {uint32_t(size), uint32_t(size)};
	builder.add_mipmap(dxt, format);
	zenkit::Texture tex = builder.build(format);
	std::vector<std::byte> out;
	auto w = zenkit::Write::to(&out);
	tex.save(w.get());
	return out;
}

void grassPixel(int, int, uint8_t* p) {
	p[0] = 40;
	p[1] = 160;
	p[2] = 64;
	p[3] = 255;
}
void waterPixel(int, int, uint8_t* p) {
	p[0] = 32;
	p[1] = 80;
	p[2] = 200;
	p[3] = 255;
}
void water2Pixel(int, int, uint8_t* p) {
	p[0] = 48;
	p[1] = 110;
	p[2] = 220;
	p[3] = 255;
}
void fencePixel(int x, int, uint8_t* p) {
	const bool solid = (x / 8) % 2 == 0;
	p[0] = 120;
	p[1] = 80;
	p[2] = 40;
	p[3] = solid ? 255 : 0;
}

void writeFile(const fs::path& path, const std::vector<std::byte>& data) {
	FILE* f = std::fopen(path.string().c_str(), "wb");
	if (!f || std::fwrite(data.data(), 1, data.size(), f) != data.size()) {
		std::printf("cannot write %s\n", path.string().c_str());
		std::exit(2);
	}
	std::fclose(f);
}

void addToVfs(zenkit::VfsNode& dir, const char* name, const std::vector<std::byte>& data) {
	auto* mem = new std::byte[data.size()];
	std::memcpy(mem, data.data(), data.size());
	dir.create(zenkit::VfsNode::file(name, zenkit::VfsFileDescriptor {mem, data.size(), true}));
}

bool hasNote(const SrcScene& scene, const char* text) {
	for (const std::string& n : scene.notes)
		if (n.find(text) != std::string::npos) return true;
	return false;
}

void checkScene(const SrcScene& scene, bool flipped, bool withObjects) {
	const int objectTriangles = withObjects ? kObjectTriangles : 0;
	CHECK(int(scene.triangles.size()) == expectedTriangles() + objectTriangles);
	// Objects add one material per distinct look: the box texture with and
	// without collision, the skinned part's texture, and the blended decal.
	CHECK(scene.materials.size() == size_t(MAT_COUNT + (withObjects ? 4 : 0)));
	if (scene.materials.size() < MAT_COUNT || scene.triangles.empty()) return;
	CHECK(scene.materials[MAT_GRASS].texture == "Fix_Grass.TGA");
	CHECK(scene.materials[MAT_WATER].water && !scene.materials[MAT_GRASS].water);
	// Water: see-through, animated and scrolling as the material says.
	const SrcMaterial& wm = scene.materials[MAT_WATER];
	CHECK(wm.opacity > 0 && wm.opacity < 255 && std::fabs(wm.animFps - 5.0f) < 1e-4f);
	CHECK(std::fabs(wm.scroll[0] - 0.2f) < 1e-4f && wm.scroll[1] == 0.0f);
	CHECK(scene.materials[MAT_GRASS].blend == SrcBlend::Opaque && scene.materials[MAT_GRASS].scroll[0] == 0.0f);
	CHECK(hasNote(scene, "water: 1 materials"));

	int upFacing = 0, terrain = 0, fence = 0, water = 0, notex = 0;
	for (const SrcTriangle& t : scene.triangles) {
		CHECK(t.material != MAT_PORTAL);
		if (t.material >= MAT_COUNT) continue; // objects: see checkObjects
		CHECK(t.detail == DETAIL_ALWAYS);
		if (t.material == MAT_FENCE) {
			++fence;
			CHECK(t.v[0].rgba[0] == 0x11 && t.v[0].rgba[1] == 0x22 && t.v[0].rgba[2] == 0x33);
			continue;
		}
		++terrain;
		water += t.material == MAT_WATER;
		notex += t.material == MAT_NOTEX;
		if (t.material == MAT_NOTEX) {
			// The lightmapped cell: light comes from the lightmap, left to right,
			// and the daylight weight is zero.
			for (const SrcVertex& v : t.v) {
				const uint8_t* want = v.pos[0] < 3.5f * kCell ? kLmLeft : kLmRight;
				CHECK(std::abs(v.rgba[0] - want[0]) < 4 && std::abs(v.rgba[1] - want[1]) < 4 && std::abs(v.rgba[2] - want[2]) < 4);
				CHECK(v.rgba[3] == 0);
			}
		} else {
			// zCOLOR 0xAARRGGBB -> r, g, b; outdoors
			CHECK(t.v[0].rgba[0] == 0x80 && t.v[0].rgba[1] == 0x40 && t.v[0].rgba[2] == 0x20 && t.v[0].rgba[3] == 255);
		}
		const float ax = t.v[1].pos[0] - t.v[0].pos[0], az = t.v[1].pos[2] - t.v[0].pos[2];
		const float bx = t.v[2].pos[0] - t.v[0].pos[0], bz = t.v[2].pos[2] - t.v[0].pos[2];
		// y component of cross(b - a, c - a): negative = counter-clockwise from above (left-handed).
		upFacing += (az * bx - ax * bz) < 0;
		// UVs are 2 repeats per cell.
		CHECK(std::fabs(t.v[0].uv[0] - t.v[0].pos[0] / kCell * 2.0f) < 1e-3f);
	}
	CHECK(fence == 2 && terrain == kGrid * kGrid * 2);
	CHECK(water == 6 * 6 * 2 && notex == 2);
	CHECK(upFacing == (flipped ? 0 : terrain));

	SrcImage img;
	CHECK(scene.loadTexture("Fix_Grass.TGA", img) && img.width == 64 && img.height == 64);
	if (img.rgba.size() == 64 * 64 * 4) {
		const uint8_t* p = &img.rgba[(32 * 64 + 32) * 4];
		CHECK(std::abs(p[0] - 40) < 10 && std::abs(p[1] - 160) < 10 && std::abs(p[2] - 64) < 10 && p[3] == 255);
	}
	SrcImage fenceImg;
	CHECK(scene.loadTexture("FIX_FENCE.TGA", fenceImg) && fenceImg.width == 32);
	if (fenceImg.rgba.size() == 32 * 32 * 4) {
		CHECK(fenceImg.rgba[(5 * 32 + 2) * 4 + 3] > 200);  // solid stripe
		CHECK(fenceImg.rgba[(5 * 32 + 12) * 4 + 3] < 50);  // gap
		CHECK(std::abs(fenceImg.rgba[(5 * 32 + 2) * 4] - 120) < 12);
	}
	SrcImage none;
	CHECK(!scene.loadTexture("FIX_NOTEX.TGA", none));
}

// Where the objects of the fixture stand (x, z); all at y = 100 except the decal.
const float kBoxAt[3][2] = {{5250, 5250}, {1750, 1750}, {750, 5250}};
const float kModelAt[2][2] = {{2250, 5250}, {4250, 5250}};
const float kDecalAt[3] = {3250, 200, 5250};
const float kDoorAt[2] = {1250, 3250};

bool near2(const SrcTriangle& t, const float at[2]) {
	return std::fabs(t.v[0].pos[0] - at[0]) < 150 && std::fabs(t.v[0].pos[2] - at[1]) < 150;
}

// The three boxes: A rotated a quarter turn on open ground, B (a child of A)
// on the lightmapped cell, C without collision.
void checkObjects(const SrcScene& scene) {
	struct Box {
		float x, z, halfX, halfZ;
		bool collides, indoor;
		int triangles = 0;
		double flux = 0;
	} boxes[3] = {{5250, 5250, 100, 50, true, false}, {1750, 1750, 50, 100, true, true}, {750, 5250, 50, 100, false, false}};

	for (const SrcTriangle& t : scene.triangles) {
		if (t.material < MAT_COUNT) continue;
		Box* box = nullptr;
		for (int i = 0; i < 3; ++i)
			if (near2(t, kBoxAt[i])) box = &boxes[i];
		if (!box) continue; // a model or the decal
		const SrcMaterial& m = scene.materials[t.material];
		CHECK(m.texture == "FIX_GRASS.TGA");
		++box->triangles;
		CHECK(m.noCollision == !box->collides);
		// Half the diagonal of a 100 x 200 x 200 box is 150 cm: a small object.
		CHECK(t.detail == DETAIL_SMALL);
		for (const SrcVertex& v : t.v) {
			// Position: rotation and translation applied, box stands 100 cm up.
			CHECK(std::fabs(std::fabs(v.pos[0] - box->x) - box->halfX) < 0.01f);
			CHECK(std::fabs(std::fabs(v.pos[2] - box->z) - box->halfZ) < 0.01f);
			CHECK(std::fabs(v.pos[1] - 100) < 0.01f || std::fabs(v.pos[1] - 300) < 0.01f);
			CHECK(v.rgba[3] == (box->indoor ? 0 : 255));
		}
		// Light of the ground below: the terrain's vertex light outdoors, the
		// middle of the lightmap (half red, half blue) for the indoor box.
		const bool top = t.v[0].pos[1] > 299 && t.v[1].pos[1] > 299 && t.v[2].pos[1] > 299;
		if (top && !box->indoor) CHECK(t.v[0].rgba[0] == 0x80 && t.v[0].rgba[1] == 0x40 && t.v[0].rgba[2] == 0x20);
		if (top && box->indoor) CHECK(std::abs(t.v[0].rgba[0] - 120) < 8 && std::abs(t.v[0].rgba[2] - 120) < 8);
		// Outward flux of the front normals: positive for a correctly wound box.
		const float ax = t.v[1].pos[0] - t.v[0].pos[0], ay = t.v[1].pos[1] - t.v[0].pos[1], az = t.v[1].pos[2] - t.v[0].pos[2];
		const float bx = t.v[2].pos[0] - t.v[0].pos[0], by = t.v[2].pos[1] - t.v[0].pos[1], bz = t.v[2].pos[2] - t.v[0].pos[2];
		box->flux += (t.v[0].pos[0] - box->x) * -(ay * bz - az * by) + (t.v[0].pos[1] - 200) * -(az * bx - ax * bz) +
		             (t.v[0].pos[2] - box->z) * -(ax * by - ay * bx);
	}
	for (const Box& b : boxes) {
		CHECK(b.triangles == kBoxTriangles);
		CHECK(b.flux > 0); // the inside-out source mesh was repaired
	}
	// The door is a box like the others, but never solid.
	int doorTriangles = 0;
	for (const SrcTriangle& t : scene.triangles) {
		if (t.material < MAT_COUNT || !near2(t, kDoorAt)) continue;
		++doorTriangles;
		CHECK(scene.materials[t.material].noCollision);
	}
	CHECK(doorTriangles == kBoxTriangles);
	CHECK(hasNote(scene, "objects: 7 placed from 3 meshes"));
	CHECK(hasNote(scene, "winding flipped"));
	CHECK(hasNote(scene, "3 without collision")); // box C, the door and the decal
	CHECK(hasNote(scene, "2 object visuals not found: HUMANS.MDS, MISSING.3DS"));
	CHECK(hasNote(scene, "1 particle effects"));
	CHECK(hasNote(scene, "lightmaps: 1 triangles, 1 lit") || hasNote(scene, "lightmaps: 2 triangles, 2 lit"));
}

// The two models: the rigid box hangs on the turned "door" bone, the skinned
// triangle is blended from three bones. Both stand at y = 100, unrotated.
void checkModels(const SrcScene& scene) {
	for (const auto& at : kModelAt) {
		int rigid = 0, skinned = 0;
		for (const SrcTriangle& t : scene.triangles) {
			if (t.material < MAT_COUNT || !near2(t, at)) continue;
			const SrcMaterial& m = scene.materials[t.material];
			CHECK(!m.noCollision && m.blend == SrcBlend::Opaque);
			if (m.texture == "FIX_GRASS.TGA") {
				++rigid;
				// The box (x +-50, y 0..200, z +-100) through door(p) = (p.z, p.y + 50, -p.x).
				for (const SrcVertex& v : t.v) {
					CHECK(std::fabs(std::fabs(v.pos[0] - at[0]) - 100) < 0.01f);
					CHECK(std::fabs(std::fabs(v.pos[2] - at[1]) - 50) < 0.01f);
					CHECK(std::fabs(v.pos[1] - 150) < 0.01f || std::fabs(v.pos[1] - 350) < 0.01f);
				}
			} else {
				++skinned;
				CHECK(m.texture == "FIX_FENCE.TGA");
				// Every corner is one of the three expected points (the winding may be flipped).
				for (const SrcVertex& v : t.v) {
					bool found = false;
					for (const auto& e : kSkinExpected)
						found = found || (std::fabs(v.pos[0] - (at[0] + e[0])) < 0.01f && std::fabs(v.pos[1] - (100 + e[1])) < 0.01f &&
						                  std::fabs(v.pos[2] - (at[1] + e[2])) < 0.01f);
					CHECK(found);
				}
			}
			CHECK(t.detail == DETAIL_MEDIUM); // the model spans 200 x 250 x 160 cm
		}
		CHECK(rigid == kBoxTriangles && skinned == 1);
	}
	CHECK(hasNote(scene, "4 meshes, 2 models in rest pose, 0 morph meshes, 1 decals"));
	CHECK(hasNote(scene, "models: 2 skinned parts; rest pose within 0.0 cm"));
}

// The decal: a 200 x 100 cm blended rectangle in the object's XY plane.
void checkDecal(const SrcScene& scene) {
	int count = 0;
	float uvMin[2] = {9, 9}, uvMax[2] = {-9, -9};
	const float at[2] = {kDecalAt[0], kDecalAt[2]};
	for (const SrcTriangle& t : scene.triangles) {
		if (t.material < MAT_COUNT || !near2(t, at)) continue;
		const SrcMaterial& m = scene.materials[t.material];
		++count;
		CHECK(m.texture == "FIX_FENCE.TGA" && m.blend == SrcBlend::Alpha && m.noCollision && m.opacity == 255);
		CHECK(std::fabs(m.animFps - 8.0f) < 1e-4f);
		for (const SrcVertex& v : t.v) {
			CHECK(std::fabs(std::fabs(v.pos[0] - kDecalAt[0]) - 100) < 0.01f);
			CHECK(std::fabs(std::fabs(v.pos[1] - kDecalAt[1]) - 50) < 0.01f);
			CHECK(std::fabs(v.pos[2] - kDecalAt[2]) < 0.01f);
			// The top of the image (v = 0) is the top of the rectangle.
			CHECK(std::fabs(v.uv[1] - (v.pos[1] > kDecalAt[1] ? 0.0f : 1.0f)) < 1e-4f);
			for (int a = 0; a < 2; ++a) {
				uvMin[a] = std::min(uvMin[a], v.uv[a]);
				uvMax[a] = std::max(uvMax[a], v.uv[a]);
			}
		}
	}
	CHECK(count == kDecalTriangles); // one face: the blended pass draws both sides
	CHECK(uvMin[0] == 0 && uvMin[1] == 0 && uvMax[0] == 1 && uvMax[1] == 1);
}

// Level-change triggers and the places they can lead to.
void checkPortals(const SrcScene& scene) {
	CHECK(scene.portals.size() == 1);
	if (scene.portals.size() == 1) {
		const SrcPortal& p = scene.portals[0];
		CHECK(p.level == "FIXTURE2.ZEN" && p.point == "ARRIVAL");
		CHECK(std::fabs(p.bbMin[0] - 6000) < 0.01f && std::fabs(p.bbMin[1]) < 0.01f && std::fabs(p.bbMax[2] - 6400) < 0.01f);
	}
	CHECK(scene.ladders.size() == 1 && !scene.indoor);
	if (scene.ladders.size() == 1)
		CHECK(std::fabs(scene.ladders[0].bbMin[0] - 7000) < 0.01f && std::fabs(scene.ladders[0].bbMax[1] - 410) < 0.01f &&
		      std::fabs(scene.ladders[0].bbMax[2] - 7020) < 0.01f);
	// Named objects are arrival points, in upper case.
	auto start = scene.points.find("START");
	CHECK(start != scene.points.end());
	if (start != scene.points.end()) {
		CHECK(std::fabs(start->second.pos[0] - 4000) < 0.01f && std::fabs(start->second.pos[1] - 100) < 0.01f);
		CHECK(std::fabs(start->second.yaw - 1.5708f) < 0.01f);
	}
}

// Wraps a mesh into a minimal outdoor world.
void fillWorld(zenkit::World& world, const zenkit::Mesh& mesh) {
	world.world_mesh = mesh;
	zenkit::BspTree& bsp = world.world_bsp_tree;
	bsp.mode = zenkit::BspTreeType::OUTDOOR;
	for (uint32_t i = 0; i < mesh.geometry.size(); ++i) bsp.polygon_indices.push_back(i);
	zenkit::BspNode leaf {};
	leaf.polygon_index = 0;
	leaf.polygon_count = uint32_t(mesh.geometry.size());
	bsp.nodes.push_back(leaf);
	bsp.leaf_node_indices.push_back(0);
	bsp.light_points.push_back(zenkit::Vec3 {0, 0, 0});
	world.way_net = std::make_shared<zenkit::WayNet>();
}

std::vector<std::byte> saveWorld(const zenkit::World& world) {
	std::vector<std::byte> zen;
	auto w = zenkit::Write::to(&zen);
	auto ar = zenkit::WriteArchive::to(w.get(), zenkit::ArchiveFormat::BINARY);
	ar->write_object("%", &world, zenkit::GameVersion::GOTHIC_1);
	ar->write_header();
	return zen;
}

std::shared_ptr<zenkit::VirtualObject> makePortal(const char* level, const char* point, float x0, float z0) {
	auto trigger = std::make_shared<zenkit::VTriggerChangeLevel>();
	trigger->type = zenkit::VirtualObjectType::oCTriggerChangeLevel;
	trigger->bbox.min = zenkit::Vec3 {x0, 0, z0};
	trigger->bbox.max = zenkit::Vec3 {x0 + 400, 300, z0 + 400};
	trigger->position = zenkit::Vec3 {x0 + 200, 150, z0 + 200};
	trigger->level_name = level;
	trigger->start_vob = point;
	return trigger;
}

std::vector<std::byte> readFile(const fs::path& path) {
	std::vector<std::byte> data;
	FILE* f = std::fopen(path.string().c_str(), "rb");
	if (!f) return data;
	std::fseek(f, 0, SEEK_END);
	data.resize(size_t(std::ftell(f)));
	std::fseek(f, 0, SEEK_SET);
	if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
	std::fclose(f);
	return data;
}

} // namespace

int main(int argc, char** argv) {
	const fs::path dir = argc > 1 ? argv[1] : "zen_fixture";
	const fs::path samples = argc > 2 ? argv[2] : "";
	fs::remove_all(dir);
	fs::create_directories(dir / "loose");
	fs::create_directories(dir / "game" / "DATA"); // upper case: the lookup must not care

	const zenkit::Mesh mesh = buildMesh();
	const auto grass = saveTexture(64, zenkit::TextureFormat::DXT1, grassPixel);
	const auto water = saveTexture(64, zenkit::TextureFormat::DXT1, waterPixel);
	const auto water2 = saveTexture(64, zenkit::TextureFormat::DXT1, water2Pixel);
	const auto fence = saveTexture(32, zenkit::TextureFormat::DXT3, fencePixel);

	// ---- 1. loose .MSH + texture directory ----
	{
		std::vector<std::byte> msh;
		auto w = zenkit::Write::to(&msh);
		mesh.save(w.get(), zenkit::GameVersion::GOTHIC_1);
		writeFile(dir / "loose" / "fixture.msh", msh);
		writeFile(dir / "loose" / "FIX_GRASS-C.TEX", grass);
		writeFile(dir / "loose" / "FIX_WATER_A0-C.TEX", water);
		writeFile(dir / "loose" / "FIX_WATER_A1-C.TEX", water2);
		writeFile(dir / "loose" / "FIX_FENCE-C.TEX", fence);
	}
	for (bool flip : {false, true}) {
		std::printf("[ RUN ] loose mesh, flipWinding=%d\n", int(flip));
		SrcScene scene;
		ZenImportOptions opt;
		opt.meshFile = (dir / "loose" / "fixture.msh").string();
		opt.textureDir = (dir / "loose").string();
		opt.flipWinding = flip;
		try {
			importZenMesh(opt, scene);
			checkScene(scene, flip, false);
			CHECK(!scene.hasSpawn && scene.portals.empty());
			// The second animation frame is found next to the first.
			SrcImage frame;
			CHECK(scene.loadTexture("fix_water_a1.tga", frame) && frame.width == 64);
			CHECK(!scene.loadTexture("fix_water_a2.tga", frame));
		} catch (const std::exception& ex) {
			std::printf("FAIL import threw: %s\n", ex.what());
			++gFailures;
		}
	}

	// ---- 2. real .ZEN files inside a .VDF, the way a Gothic install ships them ----
	{
		zenkit::World world;
		fillWorld(world, mesh);

		auto start = std::make_shared<zenkit::VStartPoint>();
		start->type = zenkit::VirtualObjectType::zCVobStartpoint;
		start->vob_name = "Start"; // names are matched without regard to case
		start->position = zenkit::Vec3 {4000.0f, 100.0f, 1500.0f};
		// Facing +X: the object's Z axis (third column) points along world X.
		start->rotation = zenkit::Mat3::identity();
		start->rotation.columns[0] = zenkit::Vec3 {0, 0, -1};
		start->rotation.columns[2] = zenkit::Vec3 {1, 0, 0};
		world.world_vobs.push_back(start);

		// Objects. A carries B as a child; D, E, F and G must be skipped (missing
		// mesh, missing model, hidden, particle effect).
		auto a = makeObject("Fix_Box.3ds", kBoxAt[0][0], 100, kBoxAt[0][1], true);
		a->rotation.columns[0] = zenkit::Vec3 {0, 0, -1}; // quarter turn around Y
		a->rotation.columns[2] = zenkit::Vec3 {1, 0, 0};
		a->children.push_back(makeObject("FIX_BOX.3DS", kBoxAt[1][0], 100, kBoxAt[1][1], true));
		world.world_vobs.push_back(a);
		world.world_vobs.push_back(makeObject("FIX_BOX.3DS", kBoxAt[2][0], 100, kBoxAt[2][1], false));
		world.world_vobs.push_back(makeObject("MISSING.3DS", 3000, 100, 3000, true));
		world.world_vobs.push_back(makeObject("HUMANS.MDS", 3000, 100, 3000, true));
		world.world_vobs.push_back(makeObject("FIX_BOX.3DS", 6000, 100, 1000, true, false));
		world.world_vobs.push_back(makeObject("FIRE.PFX", 3000, 100, 3000, true));
		// Models: one from .MDH + .MDM, one from a combined .MDL.
		world.world_vobs.push_back(makeObject("Fix_Gate.mds", kModelAt[0][0], 100, kModelAt[0][1], true));
		world.world_vobs.push_back(makeObject("FIX_MILL.MDS", kModelAt[1][0], 100, kModelAt[1][1], true));
		// A blended decal.
		{
			auto vob = std::make_shared<zenkit::VirtualObject>();
			auto decal = std::make_shared<zenkit::VisualDecal>();
			decal->type = zenkit::VisualType::DECAL;
			decal->name = "FIX_FENCE.TGA";
			decal->dimension = zenkit::Vec2 {100, 50};
			decal->alpha_func = zenkit::AlphaFunction::BLEND;
			decal->texture_anim_fps = 8.0f;
			vob->visual = decal;
			vob->show_visual = true;
			vob->position = zenkit::Vec3 {kDecalAt[0], kDecalAt[1], kDecalAt[2]};
			world.world_vobs.push_back(vob);
		}
		world.world_vobs.push_back(makePortal("FIXTURE2.ZEN", "ARRIVAL", 6000, 6000));
		// A ladder (no visual needed: only its box matters) and a door, which
		// must not block the way while doors cannot be opened.
		{
			auto ladder = std::make_shared<zenkit::VLadder>();
			ladder->type = zenkit::VirtualObjectType::oCMobLadder;
			ladder->bbox.min = zenkit::Vec3 {7000, 10, 7000};
			ladder->bbox.max = zenkit::Vec3 {7060, 410, 7020};
			ladder->position = zenkit::Vec3 {7030, 10, 7010};
			world.world_vobs.push_back(ladder);
			auto door = std::make_shared<zenkit::VDoor>();
			door->type = zenkit::VirtualObjectType::oCMobDoor;
			auto vis = std::make_shared<zenkit::VisualMesh>();
			vis->type = zenkit::VisualType::MESH;
			vis->name = "FIX_BOX.3DS";
			door->visual = vis;
			door->show_visual = true;
			door->cd_dynamic = true;
			door->position = zenkit::Vec3 {kDoorAt[0], 100, kDoorAt[1]};
			world.world_vobs.push_back(door);
		}

		// The second world: only what a portal needs.
		zenkit::World world2;
		fillWorld(world2, mesh);
		world2.world_bsp_tree.mode = zenkit::BspTreeType::INDOOR; // a mine
		auto fog = std::make_shared<zenkit::VZoneFogDefault>();
		fog->type = zenkit::VirtualObjectType::zCZoneZFogDefault;
		fog->color = zenkit::Color {30, 20, 10, 255};
		fog->range_center = 4000;
		world2.world_vobs.push_back(fog);
		auto spot = std::make_shared<zenkit::VSpot>();
		spot->type = zenkit::VirtualObjectType::zCVobSpot;
		spot->vob_name = "Arrival";
		spot->position = zenkit::Vec3 {2000.0f, 50.0f, 2500.0f};
		world2.world_vobs.push_back(spot);
		world2.world_vobs.push_back(makePortal("fixture.zen", "START", 500, 500));
		auto waypoint = std::make_shared<zenkit::WayPoint>();
		waypoint->name = "WP_TEST";
		waypoint->position = zenkit::Vec3 {1000, 20, 1200};
		waypoint->direction = zenkit::Vec3 {1, 0, 0};
		waypoint->water_depth = 0;
		waypoint->under_water = false;
		waypoint->free_point = true; // not part of any edge
		world2.way_net->points.push_back(waypoint);

		zenkit::Vfs vfs;
		zenkit::VfsNode& worlds = vfs.mkdir("_WORK/DATA/WORLDS");
		addToVfs(worlds, "FIXTURE.ZEN", saveWorld(world));
		addToVfs(worlds, "FIXTURE2.ZEN", saveWorld(world2));
		zenkit::VfsNode& texDir = vfs.mkdir("_WORK/DATA/TEXTURES/_COMPILED");
		addToVfs(texDir, "FIX_GRASS-C.TEX", grass);
		addToVfs(texDir, "FIX_WATER_A0-C.TEX", water);
		addToVfs(texDir, "FIX_WATER_A1-C.TEX", water2);
		addToVfs(texDir, "FIX_FENCE-C.TEX", fence);
		std::vector<std::byte> box;
		auto bw = zenkit::Write::to(&box);
		writeMrm(buildBox(true), bw.get());
		addToVfs(vfs.mkdir("_WORK/DATA/MESHES/_COMPILED"), "FIX_BOX.MRM", box);

		std::vector<std::byte> mdh;
		auto hw = zenkit::Write::to(&mdh);
		buildHierarchy().save(hw.get());
		const std::vector<std::byte> mdm = buildMdm(buildBox(true));
		std::vector<std::byte> mdl = mdh;
		mdl.insert(mdl.end(), mdm.begin(), mdm.end());
		zenkit::VfsNode& anims = vfs.mkdir("_WORK/DATA/ANIMS/_COMPILED");
		addToVfs(anims, "FIX_GATE.MDH", mdh);
		addToVfs(anims, "FIX_GATE.MDM", mdm);
		addToVfs(anims, "FIX_MILL.MDL", mdl);

		// The character: script, skeleton, body, three clips' worth of files
		// and both skins. A second script has its clips stored the other way
		// round; a third is the same character built facing backwards.
		const std::byte* scriptBytes = reinterpret_cast<const std::byte*>(kHeroScript);
		const std::vector<std::byte> heroScript(scriptBytes, scriptBytes + sizeof(kHeroScript) - 1);
		for (const char* base : {"FIX_HERO", "FIX_HERO2", "FIX_HERO3"}) {
			const bool zenGin = std::string(base) != "FIX_HERO2", turned = std::string(base) == "FIX_HERO3";
			std::vector<std::byte> heroMdh;
			auto hhw = zenkit::Write::to(&heroMdh);
			buildHeroHierarchy(turned).save(hhw.get());
			addToVfs(vfs.mkdir("_WORK/DATA/ANIMS"), (std::string(base) + ".MDS").c_str(), heroScript);
			addToVfs(anims, (std::string(base) + ".MDH").c_str(), heroMdh);
			addToVfs(anims, (std::string(base) + "-S_RUN.MAN").c_str(), buildMan("S_RUN", "S_RUN", 10.0f, 0, 3, zenGin, turned));
			addToVfs(anims, (std::string(base) + "-S_WALKL.MAN").c_str(),
			         buildMan("S_WALKL", "S_WALKL", 25.0f, 1, 4, zenGin, turned));
		}
		addToVfs(anims, "FIX_HERO_BODY.MDM", buildHeroMdm(false));
		addToVfs(anims, "FIX_HERO_BODY_IO.MDM", buildHeroMdm(true));
		addToVfs(texDir, "FIX_SKIN_V0_C0-C.TEX", saveTexture(32, zenkit::TextureFormat::DXT1, yellowPixel));
		addToVfs(texDir, "FIX_SKIN_V4_C1-C.TEX", saveTexture(32, zenkit::TextureFormat::DXT1, bluePixel));

		auto out = zenkit::Write::to(dir / "game" / "DATA" / "fixture.VDF");
		vfs.save(out.get(), zenkit::GameVersion::GOTHIC_1);
	}
	std::shared_ptr<ZenGame> game;
	{
		std::printf("[ RUN ] worlds from VDF\n");
		try {
			std::vector<std::string> notes;
			game = openZenGame((dir / "game").string(), notes);
			CHECK(zenGameHasFile(*game, "FIXTURE2.ZEN") && zenGameHasFile(*game, "fixture.zen"));
			CHECK(!zenGameHasFile(*game, "WORLD.ZEN"));

			SrcScene scene;
			ZenImportOptions opt;
			opt.zenName = "fixture.zen";
			importZenWorld(game, opt, scene);
			checkScene(scene, false, true);
			checkObjects(scene);
			checkModels(scene);
			checkDecal(scene);
			checkPortals(scene);
			CHECK(scene.hasSpawn);
			CHECK(std::fabs(scene.spawnPos[0] - 4000.0f) < 0.01f && std::fabs(scene.spawnPos[2] - 1500.0f) < 0.01f);
			CHECK(std::fabs(scene.spawnPos[1] - 275.0f) < 0.01f); // start point + eye height
			CHECK(std::fabs(scene.spawnYaw - 1.5708f) < 0.01f);  // looking down +X

			// Without objects, portals and arrival points are still imported.
			SrcScene bare;
			opt.objects = false;
			importZenWorld(game, opt, bare);
			checkScene(bare, false, false);
			checkPortals(bare);
			opt.objects = true;

			SrcScene second;
			opt.zenName = "FIXTURE2.ZEN";
			importZenWorld(game, opt, second);
			CHECK(second.portals.size() == 1 && second.portals[0].level == "fixture.zen" && second.portals[0].point == "START");
			auto arrival = second.points.find("ARRIVAL");
			CHECK(arrival != second.points.end());
			if (arrival != second.points.end())
				CHECK(std::fabs(arrival->second.pos[0] - 2000) < 0.01f && std::fabs(arrival->second.pos[2] - 2500) < 0.01f &&
				      std::fabs(arrival->second.yaw) < 0.01f);
			auto wp = second.points.find("WP_TEST");
			CHECK(wp != second.points.end());
			if (wp != second.points.end())
				CHECK(std::fabs(wp->second.pos[2] - 1200) < 0.01f && std::fabs(wp->second.yaw - 1.5708f) < 0.01f);
			CHECK(!second.hasSpawn && hasNote(second, "no zCVobStartpoint"));
			CHECK(second.indoor && second.fogColor[0] == 30 && second.fogColor[1] == 20 && second.fogColor[2] == 10);
			CHECK(second.ladders.empty());
		} catch (const std::exception& ex) {
			std::printf("FAIL import threw: %s\n", ex.what());
			++gFailures;
		}
	}

	// ---- the character ----
	if (game) {
		std::printf("[ RUN ] character from VDF\n");
		try {
			checkHero(game, {"Fix_Hero.MDS", "Fix_Hero_Body", true, false, false}, dir);
			// Rotations stored unturned and a body wound inside out: both noticed.
			checkHero(game, {"FIX_HERO2.mds", "FIX_HERO_BODY_IO", false, true, false}, dir);
			// Built facing backwards: turned round on import.
			checkHero(game, {"FIX_HERO3", "FIX_HERO_BODY", true, false, true}, dir);
			// A game without the files: no character, and the bake goes on.
			SrcModel none;
			ZenActorOptions missing;
			CHECK(!importZenActor(game, missing, none));
			CHECK(modelNote(none, "HUMANS.MSB/.MDS not found"));
		} catch (const std::exception& ex) {
			std::printf("FAIL character import threw: %s\n", ex.what());
			++gFailures;
		}
	}

	// ---- 3. error reporting ----
	{
		std::vector<std::string> notes;
		bool threw = false;
		try {
			openZenGame((dir / "loose").string(), notes); // has no Data directory
		} catch (const std::exception&) {
			threw = true;
		}
		CHECK(threw);
		threw = false;
		try {
			SrcScene scene;
			ZenImportOptions opt;
			opt.zenName = "MISSING.ZEN";
			if (game) importZenWorld(game, opt, scene);
		} catch (const std::exception&) {
			threw = true;
		}
		CHECK(threw);
	}

	// ---- 4. genuine game files (ZenKit's own test samples), if available ----
	const char* sampleFiles[] = {"mesh0.mrm", "morph0.mmb", "secretdoor.mdm", "smoke_waterpipe.mdm"};
	bool haveSamples = !samples.empty();
	for (const char* f : sampleFiles) haveSamples = haveSamples && fs::exists(samples / f);
	if (haveSamples) {
		std::printf("[ RUN ] sample files from %s\n", samples.string().c_str());
		fs::create_directories(dir / "samples" / "Data");
		zenkit::World world;
		fillWorld(world, mesh);
		world.world_vobs.push_back(makeObject("MESH0.3DS", 1250, 100, 1250, true));
		world.world_vobs.push_back(makeObject("MORPH0.MMS", 2250, 100, 1250, true));
		world.world_vobs.push_back(makeObject("SECRETDOOR.MDS", 3250, 100, 1250, true));
		world.world_vobs.push_back(makeObject("SMOKE_WATERPIPE.MDS", 4250, 100, 1250, true));
		zenkit::Vfs vfs;
		addToVfs(vfs.mkdir("_WORK/DATA/WORLDS"), "SAMPLES.ZEN", saveWorld(world));
		zenkit::VfsNode& data = vfs.mkdir("_WORK/DATA/ANIMS/_COMPILED");
		for (const char* f : sampleFiles) {
			std::string name = f;
			for (char& c : name) c = char(std::toupper(static_cast<unsigned char>(c)));
			addToVfs(data, name.c_str(), readFile(samples / f));
		}
		{
			auto out = zenkit::Write::to(dir / "samples" / "Data" / "samples.vdf");
			vfs.save(out.get(), zenkit::GameVersion::GOTHIC_1);
		} // closed before it is read back
		try {
			std::vector<std::string> notes;
			auto sampleGame = openZenGame((dir / "samples").string(), notes);
			SrcScene scene;
			ZenImportOptions opt;
			opt.zenName = "SAMPLES.ZEN";
			importZenWorld(sampleGame, opt, scene);
			for (const std::string& n : scene.notes) std::printf("  %s\n", n.c_str());
			CHECK(hasNote(scene, "objects: 4 placed from 4 meshes"));
			CHECK(hasNote(scene, "1 meshes, 2 models in rest pose, 1 morph meshes, 0 decals"));
			// The pipe is skinned; without its skeleton the stored pose is used as is.
			CHECK(hasNote(scene, "models: 1 skinned parts; rest pose within 0.0 cm"));
			CHECK(!hasNote(scene, "WARNING"));
			size_t objects = 0;
			for (const SrcTriangle& t : scene.triangles) objects += t.material >= MAT_COUNT;
			CHECK(objects > 100);
		} catch (const std::exception& ex) {
			std::printf("FAIL import threw: %s\n", ex.what());
			++gFailures;
		}
	} else {
		std::printf("[ SKIP ] sample files (no ZenKit sample directory given)\n");
	}

	// A genuine animation of the game's humans: the reason for reading its
	// rotations conjugated. Nodes 1..5 are the pelvis and the chain above it,
	// each a few centimetres along its parent's X axis - so the pelvis' X must
	// point up. It does when the root's and the pelvis' rotations are read
	// conjugated, and lies flat when they are read as stored.
	if (!samples.empty() && fs::exists(samples / "G1" / "HUMANS-S_FISTRUN.MAN")) {
		std::printf("[ RUN ] sample animation\n");
		auto rd = zenkit::Read::from(samples / "G1" / "HUMANS-S_FISTRUN.MAN");
		zenkit::ModelAnimation ani;
		ani.load(rd.get());
		CHECK(ani.name == "S_FISTRUN" && ani.frame_count == 20 && ani.node_count == 25 && ani.node_indices[0] == 0 &&
		      ani.node_indices[1] == 1);
		const zenkit::AnimationSample& root = ani.samples[0];
		const zenkit::AnimationSample& pelvis = ani.samples[1];
		CHECK(root.position.y > 80.0f && root.position.y < 100.0f); // hips above the ground: model space starts at the feet
		for (int n = 2; n <= 5; ++n) CHECK(ani.samples[size_t(n)].position.x > 2.0f && std::fabs(ani.samples[size_t(n)].position.y) < 0.5f);
		auto upOfX = [&](float sign) {
			const Quat r = {root.rotation.x * sign, root.rotation.y * sign, root.rotation.z * sign, root.rotation.w};
			const Quat p = {pelvis.rotation.x * sign, pelvis.rotation.y * sign, pelvis.rotation.z * sign, pelvis.rotation.w};
			return quatRotate(quatMul(r, p), {1, 0, 0}).y;
		};
		std::printf("  spine direction: up %.2f read conjugated, %.2f read as stored\n", double(upOfX(-1.0f)), double(upOfX(1.0f)));
		CHECK(upOfX(-1.0f) > 0.95f && std::fabs(upOfX(1.0f)) < 0.3f);

		// The whole first frame, put together the way the importer and the
		// runtime do (parents as in the biped the game's humans are built on:
		// the legs are nodes 26-29 and 30-33, the spine 2-6): a man standing
		// on the ground, toes in front of the ankles, facing +Z, his left
		// side at -X. Read as stored, the same numbers put his feet above his head.
		const int parentOf[34] = {-1, 0, 1, 2, 3, 4, 5, -2, 4, 8, 9, 10, 11, -2, -2, 4, 15, 16, 17, 18, -2, -2, -2, -2, -2, -2, 1, 26, 27, 28, 1, 30, 31, 32};
		for (int reading = 0; reading < 2; ++reading) {
			const float sign = reading == 0 ? -1.0f : 1.0f;
			Mat34 world[34];
			bool have[34] = {};
			for (uint32_t t = 0; t < ani.node_count; ++t) {
				const uint32_t node = ani.node_indices[t];
				if (node >= 34 || parentOf[node] == -2) continue;
				const zenkit::AnimationSample& s0 = ani.samples[t];
				const Mat34 local = mat34From(quatNormalize({s0.rotation.x * sign, s0.rotation.y * sign, s0.rotation.z * sign, s0.rotation.w}),
				                              {s0.position.x, s0.position.y, s0.position.z});
				const int parent = parentOf[node];
				if (parent >= 0 && !have[parent]) continue; // (tracks come parents first)
				world[node] = parent < 0 ? local : mul(world[parent], local);
				have[node] = true;
			}
			CHECK(have[6] && have[28] && have[29] && have[32] && have[33]);
			auto at = [&](int node) { return Vec3 {world[node].m[0][3], world[node].m[1][3], world[node].m[2][3]}; };
			const Vec3 head = at(6), lFoot = at(28), lToe = at(29), rFoot = at(32), rToe = at(33);
			std::printf("  first frame read %s: head at %.0f %.0f %.0f, ankles at height %.0f and %.0f, toes at %.1f and %.1f, %.0f and %.0f cm "
			            "in front of them\n",
			            reading == 0 ? "conjugated" : "as stored", double(head.x), double(head.y), double(head.z), double(lFoot.y),
			            double(rFoot.y), double(lToe.y), double(rToe.y), double(lToe.z - lFoot.z), double(rToe.z - rFoot.z));
			if (reading == 0) {
				CHECK(head.y > 135.0f && head.y < 160.0f);
				CHECK(std::fabs(lToe.y) < 2.0f && std::fabs(rToe.y) < 2.0f); // on the ground: the model's origin is at its feet
				CHECK(lFoot.y > 8.0f && lFoot.y < 20.0f && rFoot.y > 8.0f && rFoot.y < 20.0f);
				CHECK(lToe.z - lFoot.z > 15.0f && rToe.z - rFoot.z > 15.0f); // facing +Z
				CHECK(lFoot.x < rFoot.x - 20.0f);                            // left is -X
			} else {
				CHECK(lFoot.y > head.y && rFoot.y > head.y);
			}
		}
	}

	std::printf("%d checks, %d failed\n", gChecks, gFailures);
	return gFailures ? 1 : 0;
}
