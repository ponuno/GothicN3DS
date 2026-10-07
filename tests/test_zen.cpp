// ZenGin importer test. No game data is needed: ZenKit is used to *write* a
// small world in the original formats (a .ZEN with mesh + BSP + start point
// inside a .VDF archive next to compiled -C.TEX textures, plus a loose .MSH),
// and the importer has to read it back exactly.
//
// Usage: test_zen <output dir>   (the directory is reused by the e2e test)
#include "importers.h"

#include <zenkit/Archive.hh>
#include <zenkit/Mesh.hh>
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

	auto addPolygon = [&mesh](std::initializer_list<uint32_t> corners, uint32_t material, bool portal = false) {
		zenkit::Polygon p {};
		p.material = material;
		p.lightmap = -1;
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
				addPolygon({a, b, c, d}, material);
			} else {
				addPolygon({a, b, c}, material);
				addPolygon({a, c, d}, material);
			}
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

void checkScene(const SrcScene& scene, bool flipped) {
	CHECK(int(scene.triangles.size()) == expectedTriangles());
	CHECK(scene.materials.size() == MAT_COUNT);
	if (scene.materials.size() != MAT_COUNT || scene.triangles.empty()) return;
	CHECK(scene.materials[MAT_GRASS].texture == "Fix_Grass.TGA");
	CHECK(scene.materials[MAT_WATER].water && !scene.materials[MAT_GRASS].water);

	int upFacing = 0, terrain = 0, fence = 0, water = 0, notex = 0;
	for (const SrcTriangle& t : scene.triangles) {
		CHECK(t.material != MAT_PORTAL);
		if (t.material == MAT_FENCE) {
			++fence;
			CHECK(t.v[0].rgba[0] == 0x11 && t.v[0].rgba[1] == 0x22 && t.v[0].rgba[2] == 0x33);
			continue;
		}
		++terrain;
		water += t.material == MAT_WATER;
		notex += t.material == MAT_NOTEX;
		// zCOLOR 0xAARRGGBB -> r, g, b
		CHECK(t.v[0].rgba[0] == 0x80 && t.v[0].rgba[1] == 0x40 && t.v[0].rgba[2] == 0x20 && t.v[0].rgba[3] == 255);
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
			checkScene(scene, flip);
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
			checkScene(scene, false);
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
