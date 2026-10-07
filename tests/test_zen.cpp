// ZenGin importer test. No game data is needed: ZenKit is used to *write* a
// small world in the original formats (a .ZEN with mesh + BSP + start point
// inside a .VDF archive next to compiled -C.TEX textures, plus a loose .MSH),
// and the importer has to read it back exactly.
//
// Usage: test_zen <output dir>   (the directory is reused by the e2e test)
#include "importers.h"

#include <zenkit/Archive.hh>
#include <zenkit/Mesh.hh>
#include <zenkit/MultiResolutionMesh.hh>
#include <zenkit/Stream.hh>
#include <zenkit/Texture.hh>
#include <zenkit/Vfs.hh>
#include <zenkit/World.hh>

#include <squish.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

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

enum { MAT_GRASS, MAT_FENCE, MAT_WATER, MAT_NOTEX, MAT_PORTAL, MAT_COUNT };

float heightAt(int x, int z) { return 40.0f * std::sin(float(x) * 0.7f) + 30.0f * std::cos(float(z) * 0.5f); }

zenkit::Mesh buildMesh() {
	zenkit::Mesh mesh;
	mesh.name = "FIXTURE";

	const char* names[MAT_COUNT] = {"FIX_GRASS", "FIX_FENCE", "FIX_WATER", "FIX_NOTEX", "FIX_PORTAL"};
	// Mixed case on purpose: ZenGin file names are case-insensitive.
	const char* textures[MAT_COUNT] = {"Fix_Grass.TGA", "FIX_FENCE.TGA", "fix_water.tga", "FIX_NOTEX.TGA", ""};
	for (int i = 0; i < MAT_COUNT; ++i) {
		zenkit::Material m;
		m.name = names[i];
		m.texture = textures[i];
		m.group = i == MAT_WATER ? zenkit::MaterialGroup::WATER : zenkit::MaterialGroup::EARTH;
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

// Writes a compiled .MRM the way the game's files are laid out (and the way
// ZenKit's reader, which is tested against real files, expects them). ZenKit's
// own MultiResolutionMesh::save cannot be used at the pinned revision: it
// stores byte sizes where the reader expects element counts.
void writeMrm(const zenkit::MultiResolutionMesh& mesh, zenkit::Write* out) {
	std::vector<std::byte> section;
	{
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
	}
	out->write_ushort(0xB100); // mesh chunk
	out->write_uint(uint32_t(section.size()));
	out->write(section.data(), section.size());
	out->write_ushort(0xB1FF); // end chunk
	out->write_uint(0);
}

std::shared_ptr<zenkit::VirtualObject> makeObject(const char* visual, float x, float y, float z, bool solid, bool visible = true) {
	auto vob = std::make_shared<zenkit::VirtualObject>();
	auto vis = std::make_shared<zenkit::VisualMesh>();
	vis->name = visual;
	vis->type = zenkit::VisualType::MESH;
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

void checkScene(const SrcScene& scene, bool flipped, bool withObjects) {
	const int objectTriangles = withObjects ? kBoxTriangles * kBoxInstances : 0;
	CHECK(int(scene.triangles.size()) == expectedTriangles() + objectTriangles);
	// Objects add one material per (texture, collides) pair.
	CHECK(scene.materials.size() == size_t(MAT_COUNT + (withObjects ? 2 : 0)));
	if (scene.materials.size() < MAT_COUNT || scene.triangles.empty()) return;
	CHECK(scene.materials[MAT_GRASS].texture == "Fix_Grass.TGA");
	CHECK(scene.materials[MAT_WATER].water && !scene.materials[MAT_GRASS].water);

	int upFacing = 0, terrain = 0, fence = 0, water = 0, notex = 0;
	for (const SrcTriangle& t : scene.triangles) {
		CHECK(t.material != MAT_PORTAL);
		if (t.material >= MAT_COUNT) continue; // objects: see checkObjects
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

bool hasNote(const SrcScene& scene, const char* text) {
	for (const std::string& n : scene.notes)
		if (n.find(text) != std::string::npos) return true;
	return false;
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
		const SrcMaterial& m = scene.materials[t.material];
		CHECK(m.texture == "FIX_GRASS.TGA");
		Box* box = nullptr;
		for (Box& b : boxes)
			if (std::fabs(t.v[0].pos[0] - b.x) < 150 && std::fabs(t.v[0].pos[2] - b.z) < 150) box = &b;
		CHECK(box != nullptr);
		if (!box) continue;
		++box->triangles;
		CHECK(m.noCollision == !box->collides);
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
	CHECK(hasNote(scene, "objects: 3 placed from 1 meshes"));
	CHECK(hasNote(scene, "winding flipped"));
	CHECK(hasNote(scene, "1 without collision"));
	CHECK(hasNote(scene, "1 object meshes not found: MISSING.3DS"));
	CHECK(hasNote(scene, "1 with animated models"));
	CHECK(hasNote(scene, "lightmaps: 1 triangles, 1 lit") || hasNote(scene, "lightmaps: 2 triangles, 2 lit"));
}

} // namespace

int main(int argc, char** argv) {
	const fs::path dir = argc > 1 ? argv[1] : "zen_fixture";
	fs::remove_all(dir);
	fs::create_directories(dir / "loose");
	fs::create_directories(dir / "game" / "DATA"); // upper case: the lookup must not care

	const zenkit::Mesh mesh = buildMesh();
	const auto grass = saveTexture(64, zenkit::TextureFormat::DXT1, grassPixel);
	const auto water = saveTexture(64, zenkit::TextureFormat::DXT1, waterPixel);
	const auto fence = saveTexture(32, zenkit::TextureFormat::DXT3, fencePixel);

	// ---- 1. loose .MSH + texture directory ----
	{
		std::vector<std::byte> msh;
		auto w = zenkit::Write::to(&msh);
		mesh.save(w.get(), zenkit::GameVersion::GOTHIC_1);
		writeFile(dir / "loose" / "fixture.msh", msh);
		writeFile(dir / "loose" / "FIX_GRASS-C.TEX", grass);
		writeFile(dir / "loose" / "FIX_WATER-C.TEX", water);
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
			importZenWorld(opt, scene);
			checkScene(scene, flip, false);
			CHECK(!scene.hasSpawn);
		} catch (const std::exception& ex) {
			std::printf("FAIL import threw: %s\n", ex.what());
			++gFailures;
		}
	}

	// ---- 2. a real .ZEN inside a .VDF, the way a Gothic install ships it ----
	{
		zenkit::World world;
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

		auto start = std::make_shared<zenkit::VStartPoint>();
		start->type = zenkit::VirtualObjectType::zCVobStartpoint;
		start->vob_name = "START";
		start->position = zenkit::Vec3 {4000.0f, 100.0f, 1500.0f};
		// Facing +X: the object's Z axis (third column) points along world X.
		start->rotation = zenkit::Mat3::identity();
		start->rotation.columns[0] = zenkit::Vec3 {0, 0, -1};
		start->rotation.columns[2] = zenkit::Vec3 {1, 0, 0};
		world.world_vobs.push_back(start);

		// Objects. A carries B as a child; D, E and F must be skipped (missing
		// mesh, animated model, hidden).
		auto a = makeObject("Fix_Box.3ds", 5250, 100, 5250, true);
		a->rotation.columns[0] = zenkit::Vec3 {0, 0, -1}; // quarter turn around Y
		a->rotation.columns[2] = zenkit::Vec3 {1, 0, 0};
		a->children.push_back(makeObject("FIX_BOX.3DS", 1750, 100, 1750, true));
		world.world_vobs.push_back(a);
		world.world_vobs.push_back(makeObject("FIX_BOX.3DS", 750, 100, 5250, false));
		world.world_vobs.push_back(makeObject("MISSING.3DS", 3000, 100, 3000, true));
		world.world_vobs.push_back(makeObject("HUMANS.MDS", 3000, 100, 3000, true));
		world.world_vobs.push_back(makeObject("FIX_BOX.3DS", 6000, 100, 1000, true, false));
		world.way_net = std::make_shared<zenkit::WayNet>();

		std::vector<std::byte> zen;
		auto w = zenkit::Write::to(&zen);
		auto ar = zenkit::WriteArchive::to(w.get(), zenkit::ArchiveFormat::BINARY);
		ar->write_object("%", &world, zenkit::GameVersion::GOTHIC_1);
		ar->write_header();

		zenkit::Vfs vfs;
		addToVfs(vfs.mkdir("_WORK/DATA/WORLDS"), "FIXTURE.ZEN", zen);
		zenkit::VfsNode& texDir = vfs.mkdir("_WORK/DATA/TEXTURES/_COMPILED");
		addToVfs(texDir, "FIX_GRASS-C.TEX", grass);
		addToVfs(texDir, "FIX_WATER-C.TEX", water);
		addToVfs(texDir, "FIX_FENCE-C.TEX", fence);
		std::vector<std::byte> box;
		auto bw = zenkit::Write::to(&box);
		writeMrm(buildBox(true), bw.get());
		addToVfs(vfs.mkdir("_WORK/DATA/MESHES/_COMPILED"), "FIX_BOX.MRM", box);
		auto out = zenkit::Write::to(dir / "game" / "DATA" / "fixture.VDF");
		vfs.save(out.get(), zenkit::GameVersion::GOTHIC_1);
	}
	{
		std::printf("[ RUN ] world from VDF\n");
		SrcScene scene;
		ZenImportOptions opt;
		opt.gameDir = (dir / "game").string();
		opt.zenName = "fixture.zen";
		try {
			importZenWorld(opt, scene);
			checkScene(scene, false, true);
			checkObjects(scene);
			CHECK(scene.hasSpawn);
			CHECK(std::fabs(scene.spawnPos[0] - 4000.0f) < 0.01f && std::fabs(scene.spawnPos[2] - 1500.0f) < 0.01f);
			CHECK(std::fabs(scene.spawnPos[1] - 275.0f) < 0.01f); // start point + eye height
			CHECK(std::fabs(scene.spawnYaw - 1.5708f) < 0.01f);  // looking down +X
		} catch (const std::exception& ex) {
			std::printf("FAIL import threw: %s\n", ex.what());
			++gFailures;
		}
	}

	// ---- 3. error reporting ----
	{
		SrcScene scene;
		ZenImportOptions opt;
		opt.gameDir = (dir / "loose").string(); // has no Data directory
		bool threw = false;
		try {
			importZenWorld(opt, scene);
		} catch (const std::exception&) {
			threw = true;
		}
		CHECK(threw);
		opt.gameDir = (dir / "game").string();
		opt.zenName = "MISSING.ZEN";
		threw = false;
		try {
			importZenWorld(opt, scene);
		} catch (const std::exception&) {
			threw = true;
		}
		CHECK(threw);
	}

	std::printf("%d checks, %d failed\n", gChecks, gFailures);
	return gFailures ? 1 : 0;
}
