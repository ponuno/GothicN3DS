// Unit and integration tests for the data path: archive, texture layout,
// chunker, streaming. Usage: test_core <scratch dir>
#include "app.h"
#include "chunker.h"
#include "g3ds/pak.h"
#include "g3ds/tex.h"
#include "gfx_soft.h"
#include "importers.h"
#include "pak_writer.h"
#include "rg_etc1.h"
#include "texture_bake.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

using namespace g3;

static int gFailures = 0, gChecks = 0;

#define CHECK(cond)                                                              \
	do {                                                                         \
		++gChecks;                                                               \
		if (!(cond)) {                                                           \
			++gFailures;                                                         \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
		}                                                                        \
	} while (0)

static std::string gDir;

static void testHash() {
	CHECK(hashName("tex/Grass") == hashName("TEX/grass"));
	CHECK(hashName("tex\\grass") == hashName("tex/grass"));
	CHECK(hashName("tex/grass") != hashName("tex/grasS2"));
}

static void testPak() {
	std::vector<u8> text(50000);
	for (size_t i = 0; i < text.size(); ++i) text[i] = u8("gothic"[i % 6]); // compresses well
	std::vector<u8> noise(4097);
	std::mt19937 rng(1);
	for (u8& b : noise) b = u8(rng()); // does not compress
	std::vector<u8> empty;

	PakWriter w;
	w.add("meta/text", ASSET_META, text);
	w.add("blob/noise", ASSET_BLOB, noise);
	w.add("blob/empty", ASSET_BLOB, empty);
	bool threw = false;
	try {
		w.add("META/TEXT", ASSET_META, text); // same name after case folding
	} catch (const std::exception&) {
		threw = true;
	}
	CHECK(threw);
	const std::string path = gDir + "/test.pak";
	w.write(path);

	Pak p;
	CHECK(p.open(path.c_str()));
	CHECK(p.count() == 3);
	const PakEntry* e = p.find("Meta/Text");
	CHECK(e && e->codec == PAK_LZ4 && e->size == text.size() && e->packedSize < text.size() / 10);
	if (e) {
		std::vector<u8> out(e->size);
		CHECK(p.read(*e, out.data()) && out == text);
		CHECK(std::string(p.name(*e)) == "meta/text");
	}
	e = p.find("blob/noise");
	CHECK(e && e->codec == PAK_RAW);
	if (e) {
		void* mem = p.readAlloc(*e);
		CHECK(mem && std::memcmp(mem, noise.data(), noise.size()) == 0);
		free(mem);
	}
	e = p.find("blob/empty");
	CHECK(e && e->size == 0);
	CHECK(p.find("blob/nothing") == nullptr);
	p.close();

	// A truncated archive must be rejected, not half-loaded.
	FILE* f = fopen(path.c_str(), "rb");
	std::vector<u8> bytes(200);
	const size_t n = fread(bytes.data(), 1, bytes.size(), f);
	fclose(f);
	const std::string bad = gDir + "/truncated.pak";
	f = fopen(bad.c_str(), "wb");
	fwrite(bytes.data(), 1, n, f);
	fclose(f);
	CHECK(!p.open(bad.c_str()));
	CHECK(!p.open((gDir + "/does-not-exist.pak").c_str()));
}

static void testEtc1BlockDecoder() {
	// Our decoder against an independent implementation (rg_etc1's unpacker).
	std::mt19937 rng(7);
	int mismatches = 0;
	for (int i = 0; i < 20000; ++i) {
		u8 block[8];
		for (u8& b : block) b = u8(rng());
		unsigned int ref[16];
		// rg_etc1 reports invalid differential blocks (base + delta out of
		// range); real encoders never emit them, so they are not compared.
		if (!rg_etc1::unpack_etc1_block(block, ref)) continue;
		u8 ours[64];
		etc1DecodeBlock(block, ours);
		for (int px = 0; px < 16; ++px) {
			u8 r[4];
			std::memcpy(r, &ref[px], 4);
			if (r[0] != ours[px * 4] || r[1] != ours[px * 4 + 1] || r[2] != ours[px * 4 + 2]) ++mismatches;
		}
	}
	CHECK(mismatches == 0);
}

static double psnr(const std::vector<u8>& a, const std::vector<u8>& b, int channels = 3) {
	double se = 0;
	size_t n = 0;
	for (size_t i = 0; i + 3 < a.size(); i += 4)
		for (int c = 0; c < channels; ++c, ++n) {
			const double d = double(a[i + size_t(c)]) - double(b[i + size_t(c)]);
			se += d * d;
		}
	return se == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / (se / double(n)));
}

static void testTexture() {
	// Smooth opaque image: ETC1 must reproduce it closely and keep orientation.
	SrcImage img;
	img.width = 64;
	img.height = 32;
	img.rgba.resize(64 * 32 * 4);
	for (int y = 0; y < 32; ++y)
		for (int x = 0; x < 64; ++x) {
			u8* p = &img.rgba[size_t(y * 64 + x) * 4];
			p[0] = u8(x * 4);
			p[1] = u8(y * 8);
			p[2] = u8(128 + 60 * std::sin(x * 0.2) * std::cos(y * 0.3));
			p[3] = 255;
		}
	TexBakeOptions opt;
	opt.maxSize = 64;
	TexBakeInfo info;
	std::vector<u8> baked = bakeTexture(img, opt, &info);
	TexHeader h;
	std::memcpy(&h, baked.data(), sizeof(h));
	CHECK(texValidate(h, u32(baked.size() - sizeof(h))));
	CHECK(h.width == 64 && h.height == 32 && h.format == TEX_ETC1 && !info.alpha);
	CHECK(h.mipCount == 3); // 64x32, 32x16, 16x8: PICA stops at 8 texels
	CHECK(h.dataSize == (64 * 32 + 32 * 16 + 16 * 8) / 2);

	std::vector<u8> decoded(64 * 32 * 4);
	texDecodeLevel(h, baked.data() + sizeof(h), 0, decoded.data());
	const double quality = psnr(img.rgba, decoded);
	std::printf("  ETC1 gradient PSNR: %.1f dB\n", quality);
	CHECK(quality > 32.0);

	// PICA stores rows bottom-up: the first stored block must be the image's
	// bottom-left corner. Decode it by hand, bypassing texDecodeLevel.
	SrcImage quad;
	quad.width = quad.height = 16;
	quad.rgba.resize(16 * 16 * 4);
	for (int y = 0; y < 16; ++y)
		for (int x = 0; x < 16; ++x) {
			static const u8 colors[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
			const u8* c = colors[(y >= 8) * 2 + (x >= 8)]; // TL red, TR green, BL blue, BR white
			u8* p = &quad.rgba[size_t(y * 16 + x) * 4];
			p[0] = c[0];
			p[1] = c[1];
			p[2] = c[2];
			p[3] = 255;
		}
	opt.maxSize = 16;
	baked = bakeTexture(quad, opt, nullptr);
	std::memcpy(&h, baked.data(), sizeof(h));
	const u8* first = baked.data() + sizeof(h);
	u8 be[8];
	for (int i = 0; i < 8; ++i) be[i] = first[7 - i];
	unsigned int px[16];
	rg_etc1::unpack_etc1_block(be, px);
	u8 c[4];
	std::memcpy(c, &px[0], 4);
	CHECK(c[0] < 40 && c[1] < 40 && c[2] > 215); // blue = bottom-left
	decoded.assign(16 * 16 * 4, 0);
	texDecodeLevel(h, first, 0, decoded.data());
	CHECK(decoded[0] > 215 && decoded[1] < 40);                         // top-left stays red
	CHECK(decoded[(15 * 16 + 15) * 4 + 0] > 215 && decoded[(15 * 16 + 15) * 4 + 2] > 215); // bottom-right white

	// Cut-out texture: becomes ETC1A4 and the mask survives.
	SrcImage cut;
	cut.width = cut.height = 32;
	cut.rgba.resize(32 * 32 * 4);
	for (int y = 0; y < 32; ++y)
		for (int x = 0; x < 32; ++x) {
			u8* p = &cut.rgba[size_t(y * 32 + x) * 4];
			const bool solid = (x / 8 + y / 8) % 2 == 0;
			p[0] = 200;
			p[1] = 120;
			p[2] = 40;
			p[3] = solid ? 255 : 0;
		}
	opt.maxSize = 32;
	baked = bakeTexture(cut, opt, &info);
	std::memcpy(&h, baked.data(), sizeof(h));
	CHECK(h.format == TEX_ETC1A4 && (h.flags & TEXF_ALPHA_TEST) && info.alpha);
	CHECK(texValidate(h, u32(baked.size() - sizeof(h))));
	decoded.assign(32 * 32 * 4, 0);
	texDecodeLevel(h, baked.data() + sizeof(h), 0, decoded.data());
	int maskErrors = 0, darkFringe = 0;
	for (int i = 0; i < 32 * 32; ++i) {
		maskErrors += (decoded[size_t(i) * 4 + 3] > 127) != (cut.rgba[size_t(i) * 4 + 3] > 127);
		// Transparent texels must carry the neighbouring colour, not black.
		darkFringe += decoded[size_t(i) * 4] < 120;
	}
	CHECK(maskErrors == 0);
	CHECK(darkFringe == 0);

	// Oversized and non-power-of-two input is brought into PICA's limits.
	SrcImage big;
	big.width = 300;
	big.height = 150;
	big.rgba.assign(300 * 150 * 4, 200);
	opt.maxSize = 128;
	baked = bakeTexture(big, opt, &info);
	CHECK(info.width == 128 && info.height == 64);
	SrcImage tiny;
	tiny.width = 4;
	tiny.height = 2;
	tiny.rgba.assign(4 * 2 * 4, 255);
	baked = bakeTexture(tiny, opt, &info);
	CHECK(info.width == 8 && info.height == 8 && info.mipCount == 1);
}

static void testChunker() {
	SrcScene scene;
	scene.materials.resize(3);
	// A 20 x 20 grid of quads over 4 x 4 chunks, UVs far from the origin.
	// Height and colour depend on the lattice corner only, so quads share vertices.
	for (int z = 0; z < 20; ++z)
		for (int x = 0; x < 20; ++x) {
			const float x0 = -1000.0f + x * 200.0f, z0 = z * 200.0f;
			SrcVertex q[4] = {};
			const float px[4] = {x0, x0 + 200, x0 + 200, x0}, pz[4] = {z0, z0, z0 + 200, z0 + 200};
			for (int k = 0; k < 4; ++k) {
				const int cx = x + (k == 1 || k == 2), cz = z + (k >= 2);
				q[k].pos[0] = px[k];
				q[k].pos[1] = float((cx * 31 + cz * 17) % 97);
				q[k].pos[2] = pz[k];
				q[k].uv[0] = px[k] / 100.0f - 250.0f;
				q[k].uv[1] = pz[k] / 100.0f + 90.0f;
				q[k].rgba[0] = u8(cx * 12);
				q[k].rgba[1] = u8(cz * 12);
				q[k].rgba[2] = 77;
				q[k].rgba[3] = 255;
			}
			SrcTriangle a, b;
			a.v[0] = q[0]; a.v[1] = q[1]; a.v[2] = q[2];
			b.v[0] = q[0]; b.v[1] = q[2]; b.v[2] = q[3];
			a.material = b.material = u32((x + z) % 2);
			scene.triangles.push_back(a);
			scene.triangles.push_back(b);
		}
	// One degenerate triangle and one triangle of a dropped material.
	SrcTriangle deg = scene.triangles[0];
	deg.v[1] = deg.v[0];
	scene.triangles.push_back(deg);
	SrcTriangle dropped = scene.triangles[0];
	dropped.material = 2;
	scene.triangles.push_back(dropped);

	const std::vector<int> remap = {0, 1, -1};
	ChunkerOptions opt;
	opt.chunkSize = 1000.0f;
	ChunkerStats st;
	std::vector<BakedChunk> chunks = buildChunks(scene, remap, opt, st);
	CHECK(chunks.size() == 16);
	CHECK(st.inputTriangles == 801 && st.outputTriangles == 800 && st.degenerate == 1);
	CHECK(st.uvClamped == 0);

	u64 tris = 0;
	float worst = 0, worstUv = 0;
	for (const BakedChunk& c : chunks) {
		CHECK(c.batches.size() == 2 && c.batches[0].material < c.batches[1].material);
		tris += c.indices.size() / 3;
		// Shared corners must have been merged: far fewer than 3 vertices per triangle.
		CHECK(c.vertices.size() < c.indices.size() * 2 / 3);
		for (u16 i : c.indices) CHECK(i < c.vertices.size());
		for (const WorldVertex& v : c.vertices) {
			const float x = c.header.origin[0] + float(v.x) * c.header.posScale;
			const float z = c.header.origin[2] + float(v.z) * c.header.posScale;
			// Every source vertex sits on the 200 cm lattice.
			worst = std::max(worst, std::fabs(x - std::round(x / 200.0f) * 200.0f));
			worst = std::max(worst, std::fabs(z - std::round(z / 200.0f) * 200.0f));
			// UVs are shifted by whole repeats only: the fraction must survive.
			const float u = float(v.u) * c.header.uvScale, expectU = x / 100.0f - 250.0f;
			worstUv = std::max(worstUv, std::fabs((u - expectU) - std::round(u - expectU)));
			CHECK(v.u >= 0 && v.v >= 0);
			CHECK(v.b == 77 && v.a == 255 && v.w == 1);
			CHECK(x >= c.bbMin[0] - 0.01f && x <= c.bbMax[0] + 0.01f);
		}
		// The serialised blob has the documented size and layout.
		const std::vector<u8> blob = c.serialize();
		CHECK(blob.size() == chunkBlobSize(u32(c.batches.size()), u32(c.vertices.size()), u32(c.indices.size())));
		ChunkHeader hd;
		std::memcpy(&hd, blob.data(), sizeof(hd));
		CHECK(hd.magic == CHUNK_MAGIC && hd.vertexCount == c.vertices.size() && hd.indexCount == c.indices.size());
	}
	CHECK(tris == 800);
	std::printf("  chunker: position error %.3f cm, uv error %.5f\n", double(worst), double(worstUv));
	CHECK(worst < 0.05f);
	CHECK(worstUv < 0.002f);

	// A tiny vertex limit forces cells to split without losing triangles.
	opt.maxVertices = 24;
	ChunkerStats st2;
	chunks = buildChunks(scene, remap, opt, st2);
	CHECK(st2.outputTriangles == 800 && st2.splitChunks == 16 && chunks.size() > 16);
	for (const BakedChunk& c : chunks) CHECK(c.vertices.size() <= 24);

	// ZenGin-style UVs: every polygon shifted by its own whole number of
	// repeats. The chunker must still share the vertices, and may only ever
	// move a triangle by whole repeats.
	SrcScene plain, shifted;
	plain.materials.resize(1);
	shifted.materials.resize(1);
	std::mt19937 rng(11);
	for (int z = 0; z < 20; ++z)
		for (int x = 0; x < 20; ++x) {
			const float x0 = x * 200.0f, z0 = z * 200.0f;
			const float px[4] = {x0, x0 + 200, x0 + 200, x0}, pz[4] = {z0, z0, z0 + 200, z0 + 200};
			SrcVertex q[4] = {};
			for (int k = 0; k < 4; ++k) {
				q[k].pos[0] = px[k];
				q[k].pos[2] = pz[k];
				q[k].uv[0] = px[k] / 300.0f; // fractions of 1/3: no ties when rounding
				q[k].uv[1] = pz[k] / 300.0f;
				q[k].rgba[3] = 255;
			}
			for (int half = 0; half < 2; ++half) {
				SrcTriangle tr;
				tr.material = 0;
				tr.v[0] = q[0];
				tr.v[1] = q[half ? 2 : 1];
				tr.v[2] = q[half ? 3 : 2];
				plain.triangles.push_back(tr);
				const float du = float(int(rng() % 7) - 3), dv = float(int(rng() % 7) - 3);
				for (SrcVertex& v : tr.v) {
					v.uv[0] += du;
					v.uv[1] += dv;
				}
				shifted.triangles.push_back(tr);
			}
		}
	ChunkerOptions one;
	one.chunkSize = 100000.0f; // a single chunk
	ChunkerStats sp, ss;
	buildChunks(plain, {0}, one, sp);
	const std::vector<BakedChunk> merged = buildChunks(shifted, {0}, one, ss);
	std::printf("  uv seams: %llu vertices plain, %llu with per-polygon offsets\n",
	            static_cast<unsigned long long>(sp.vertices), static_cast<unsigned long long>(ss.vertices));
	CHECK(sp.vertices == 21 * 21);
	CHECK(ss.vertices == sp.vertices && ss.outputTriangles == 800 && ss.uvClamped == 0);
	CHECK(merged.size() == 1);
	for (const BakedChunk& c : merged)
		for (const WorldVertex& v : c.vertices) {
			const float x = c.header.origin[0] + float(v.x) * c.header.posScale;
			const float d = float(v.u) * c.header.uvScale - x / 300.0f;
			CHECK(std::fabs(d - std::round(d)) < 0.002f); // same texel, some whole repeats away
		}

	// A triangle spanning more texture repeats than 16.16 fixed point holds.
	SrcScene wide;
	wide.materials.resize(1);
	SrcTriangle t = scene.triangles[0];
	t.v[1].uv[0] = t.v[0].uv[0] + 100.0f;
	wide.triangles.push_back(t);
	ChunkerStats st3;
	buildChunks(wide, {0}, ChunkerOptions {}, st3);
	CHECK(st3.uvClamped == 1);
}

static void testMath() {
	const Mat4 proj = perspectiveLH(1.0f, 400.0f / 240.0f, 10.0f, 1000.0f);
	Vec4 n = transform(proj, {0, 0, 10, 1}), f = transform(proj, {0, 0, 1000, 1});
	CHECK(std::fabs(n.z / n.w) < 1e-5f && std::fabs(f.z / f.w - 1.0f) < 1e-5f);

	// Camera at the origin looking down +Z; +X is to the right, +Y is up.
	const Mat4 view = viewLH({0, 0, 0}, 0, 0);
	const Vec4 right = transform(mul(proj, view), {5, 0, 100, 1}), up = transform(mul(proj, view), {0, 5, 100, 1});
	CHECK(right.x > 0 && up.y > 0 && right.w > 0);

	const Frustum fr = frustumFromViewProj(mul(proj, view));
	const float in0[3] = {-10, -10, 90}, in1[3] = {10, 10, 110};
	const float behind0[3] = {-10, -10, -110}, behind1[3] = {10, 10, -90};
	const float side0[3] = {5000, -10, 90}, side1[3] = {5020, 10, 110};
	const float far0[3] = {-10, -10, 1500}, far1[3] = {10, 10, 1600};
	const float huge0[3] = {-9000, -9000, -9000}, huge1[3] = {9000, 9000, 9000};
	CHECK(aabbInFrustum(fr, in0, in1));
	CHECK(!aabbInFrustum(fr, behind0, behind1));
	CHECK(!aabbInFrustum(fr, side0, side1));
	CHECK(!aabbInFrustum(fr, far0, far1));
	CHECK(aabbInFrustum(fr, huge0, huge1));

	// Turning right (positive yaw) brings +X into view.
	const Vec3 fwd = forwardFromYawPitch(1.5708f, 0);
	CHECK(fwd.x > 0.99f && std::fabs(fwd.z) < 0.01f);
}

// Minimal bake of a scene into a pak (the CLI does the same with more
// reporting). Textures the scene cannot provide drop their triangles.
static void bakeScenePak(SrcScene& scene, const std::string& path, const char* name, float chunkSize) {
	PakWriter pak;
	std::vector<WorldMaterial> mats;
	std::vector<int> remap;
	TexBakeOptions to;
	to.maxSize = 32;
	to.quality = 0;
	for (const SrcMaterial& m : scene.materials) {
		SrcImage img;
		if (!scene.loadTexture || !scene.loadTexture(m.texture, img)) {
			if (scene.loadTexture) {
				remap.push_back(-1);
				continue;
			}
			// Scenes without textures (collision tests): a flat grey one.
			img.width = img.height = 8;
			img.rgba.assign(8 * 8 * 4, 160);
		}
		TexBakeInfo info;
		const std::string texName = "tex/" + m.name;
		pak.add(texName, ASSET_TEXTURE, bakeTexture(img, to, &info));
		WorldMaterial wm {};
		wm.textureHash = hashName(texName.c_str());
		wm.flags = u16((info.alpha ? MATF_ALPHA_TEST : 0) | (m.noCollision ? MATF_NO_COLLISION : 0));
		remap.push_back(int(mats.size()));
		mats.push_back(wm);
	}
	ChunkerOptions co;
	co.chunkSize = chunkSize;
	ChunkerStats st;
	const std::vector<BakedChunk> chunks = buildChunks(scene, remap, co, st);
	WorldHeader wh {};
	wh.magic = WORLD_MAGIC;
	wh.version = WORLD_VERSION;
	wh.chunkSize = co.chunkSize;
	wh.chunkCount = u32(chunks.size());
	wh.materialCount = u32(mats.size());
	wh.triangleCount = u32(st.outputTriangles);
	std::memcpy(wh.spawnPos, scene.spawnPos, sizeof(wh.spawnPos));
	wh.spawnYaw = scene.spawnYaw;
	std::vector<ChunkInfo> infos;
	const std::string prefix = std::string("world/") + name;
	for (size_t i = 0; i < chunks.size(); ++i) {
		const std::string chunkName = prefix + "/" + std::to_string(i);
		const std::vector<u8> blob = chunks[i].serialize();
		pak.add(chunkName, ASSET_CHUNK, blob);
		ChunkInfo ci {};
		std::memcpy(ci.bbMin, chunks[i].bbMin, 12);
		std::memcpy(ci.bbMax, chunks[i].bbMax, 12);
		ci.entryHash = hashName(chunkName.c_str());
		ci.size = u32(blob.size());
		ci.vertexCount = chunks[i].header.vertexCount;
		ci.batchCount = chunks[i].header.batchCount;
		ci.indexCount = chunks[i].header.indexCount;
		infos.push_back(ci);
		for (int a = 0; a < 3; ++a) {
			wh.bbMin[a] = i ? std::min(wh.bbMin[a], ci.bbMin[a]) : ci.bbMin[a];
			wh.bbMax[a] = i ? std::max(wh.bbMax[a], ci.bbMax[a]) : ci.bbMax[a];
		}
	}
	std::vector<u8> index(sizeof(wh) + mats.size() * sizeof(WorldMaterial) + infos.size() * sizeof(ChunkInfo));
	std::memcpy(index.data(), &wh, sizeof(wh));
	std::memcpy(index.data() + sizeof(wh), mats.data(), mats.size() * sizeof(WorldMaterial));
	std::memcpy(index.data() + sizeof(wh) + mats.size() * sizeof(WorldMaterial), infos.data(),
	            infos.size() * sizeof(ChunkInfo));
	pak.add(prefix, ASSET_WORLD, index);
	pak.write(path);
}

// Bakes a small synthetic world with the real pipeline and drives the runtime.
static void testRuntime() {
	SrcScene scene;
	SynthOptions so;
	so.cells = 48;
	so.huts = 8;
	so.trees = 60;
	buildSyntheticScene(so, scene);
	CHECK(scene.triangles.size() > 4000 && scene.hasSpawn);
	const std::string path = gDir + "/runtime.pak";
	bakeScenePak(scene, path, "t", 3000.0f);

	gfx::init();
	{
		App app;
		CHECK(!app.init(path.c_str(), "nope"));
		app.shutdown();
	}
	{
		App app;
		CHECK(app.init(path.c_str(), "t"));
		app.setDrawDistance(6000.0f);
		app.player().flying = true; // a fixed camera at the spawn point
		World& w = app.world();

		// Streaming is rate limited ...
		w.config().maxLoadsPerFrame = 1;
		app.update(Input {}, 1.0f / 30.0f);
		CHECK(w.stats().chunksLoaded == 1 && w.stats().chunksPending > 0);
		// ... and a preload fetches everything in range at once.
		w.preload(app.camera().pos);
		const u32 near = w.stats().chunksLoaded;
		CHECK(near > 4 && near < w.stats().chunksTotal && w.stats().chunksPending == 0);
		CHECK(w.stats().loadFailures == 0);
		CHECK(w.stats().residentBytes == gfx::memStats().meshBytes + gfx::memStats().textureBytes);
		CHECK(w.stats().collisionBytes > 0);

		app.render();
		CHECK(w.stats().trianglesDrawn > 500 && w.stats().chunksDrawn > 0);
		gfx::SoftStats ss = gfx::softStats();
		CHECK(ss.pixelsWritten > u32(gfx::SCREEN_W * gfx::SCREEN_H / 4));
		// Looking at the camp from the gate: mostly front faces.
		CHECK(ss.trianglesBackface < (ss.trianglesSubmitted - ss.trianglesClipped) / 2);
		// The frame is not a flat fog-coloured rectangle.
		const u8* fb = gfx::softFramebuffer();
		int distinct = 0;
		for (int i = 0; i < gfx::SCREEN_W * gfx::SCREEN_H; i += 97)
			distinct += std::abs(int(fb[i * 4 + 1]) - int(app.skyColors().horizon[1])) > 24;
		CHECK(distinct > 200);

		// Far away from everything: all chunks are released again.
		app.teleport({1e6f, 0, 1e6f});
		app.update(Input {}, 1.0f / 30.0f);
		CHECK(w.stats().chunksLoaded == 0 && w.stats().residentBytes == 0 && w.stats().texturesLoaded == 0);
		CHECK(w.stats().collisionBytes == 0);
		CHECK(gfx::memStats().meshCount == 1 && gfx::memStats().textureCount == 0); // only the sky dome is left

		// A budget too small for the neighbourhood: loading stops, nothing breaks.
		app.teleport({scene.spawnPos[0], scene.spawnPos[1], scene.spawnPos[2]});
		w.config().memoryBudget = 12 * 1024;
		w.preload(app.camera().pos);
		CHECK(w.stats().budgetRejects > 0 && w.stats().chunksLoaded > 0 && w.stats().chunksLoaded < near);
		app.render();
		CHECK(w.stats().trianglesDrawn > 0);
		app.shutdown();
		CHECK(gfx::memStats().meshCount == 0 && gfx::memStats().textureCount == 0 && gfx::memStats().meshBytes == 0);
	}

	// Corrupt one chunk: the runtime reports a failure instead of crashing,
	// and does not retry the broken chunk every frame.
	{
		FILE* f = fopen(path.c_str(), "rb");
		fseek(f, 0, SEEK_END);
		std::vector<u8> bytes(size_t(ftell(f)));
		fseek(f, 0, SEEK_SET);
		CHECK(fread(bytes.data(), 1, bytes.size(), f) == bytes.size());
		fclose(f);
		Pak p;
		p.open(path.c_str());
		u32 corrupted = 0;
		for (u32 i = 0; i < p.count(); ++i) {
			const PakEntry& e = p.entry(i);
			if (e.type != ASSET_CHUNK) continue;
			// Damage the start of every chunk payload (the header / first LZ4 token).
			for (u32 k = 0; k < 4 && k < e.packedSize; ++k) bytes[e.offset + k] ^= 0xA5;
			++corrupted;
		}
		p.close();
		const std::string bad = gDir + "/corrupt.pak";
		f = fopen(bad.c_str(), "wb");
		fwrite(bytes.data(), 1, bytes.size(), f);
		fclose(f);

		App app;
		CHECK(app.init(bad.c_str(), "t"));
		app.world().preload(app.camera().pos);
		const u32 failures = app.world().stats().loadFailures;
		CHECK(corrupted > 0 && failures > 0);
		app.world().preload(app.camera().pos);
		CHECK(app.world().stats().loadFailures == failures); // no retry storm
		app.render();
		app.shutdown();
		CHECK(gfx::memStats().meshCount == 0 && gfx::memStats().textureCount == 0);
	}
	gfx::shutdown();
}

// A hand-made obstacle course: floor, wall, see-through wall, a low step, a
// tall block, a gentle ramp and a slope too steep to climb.
static void addQuad(SrcScene& scene, const float a[3], const float b[3], const float c[3], const float d[3], u32 material) {
	const float* corners[4] = {a, b, c, d};
	SrcVertex v[4] = {};
	for (int k = 0; k < 4; ++k) {
		std::memcpy(v[k].pos, corners[k], sizeof(float) * 3);
		v[k].uv[0] = corners[k][0] / 200.0f;
		v[k].uv[1] = corners[k][2] / 200.0f;
		v[k].rgba[0] = v[k].rgba[1] = v[k].rgba[2] = 200;
		v[k].rgba[3] = 255;
	}
	SrcTriangle t1, t2;
	t1.material = t2.material = material;
	t1.v[0] = v[0]; t1.v[1] = v[1]; t1.v[2] = v[2];
	t2.v[0] = v[0]; t2.v[1] = v[2]; t2.v[2] = v[3];
	scene.triangles.push_back(t1);
	scene.triangles.push_back(t2);
}

static void addBlock(SrcScene& scene, float x0, float x1, float z0, float z1, float top) {
	const float c[8][3] = {{x0, 0, z0}, {x1, 0, z0}, {x1, 0, z1}, {x0, 0, z1}, {x0, top, z0}, {x1, top, z0}, {x1, top, z1}, {x0, top, z1}};
	addQuad(scene, c[4], c[5], c[6], c[7], 0); // top
	addQuad(scene, c[0], c[1], c[5], c[4], 0);
	addQuad(scene, c[1], c[2], c[6], c[5], 0);
	addQuad(scene, c[2], c[3], c[7], c[6], 0);
	addQuad(scene, c[3], c[0], c[4], c[7], 0);
}

static void testCollision() {
	// Closest point: face interior, edge and corner regions.
	const Vec3 a {0, 0, 0}, b {10, 0, 0}, c {0, 0, 10};
	Vec3 q = closestPointOnTriangle({2, 5, 2}, a, b, c);
	CHECK(std::fabs(q.x - 2) < 1e-4f && std::fabs(q.y) < 1e-4f && std::fabs(q.z - 2) < 1e-4f);
	q = closestPointOnTriangle({5, 3, -4}, a, b, c);
	CHECK(std::fabs(q.x - 5) < 1e-4f && std::fabs(q.z) < 1e-4f);
	q = closestPointOnTriangle({-3, 1, -3}, a, b, c);
	CHECK(std::fabs(q.x) < 1e-4f && std::fabs(q.z) < 1e-4f);
	q = closestPointOnTriangle({20, 0, 20}, a, b, c);
	CHECK(std::fabs(q.x - 5) < 1e-3f && std::fabs(q.z - 5) < 1e-3f);

	SrcScene scene;
	scene.materials.resize(2);
	scene.materials[0].name = "solid";
	scene.materials[1].name = "ghost";
	scene.materials[1].noCollision = true;
	for (int z = -4; z < 4; ++z)
		for (int x = -4; x < 4; ++x) {
			const float x0 = x * 500.0f, z0 = z * 500.0f;
			const float p[4][3] = {{x0, 0, z0}, {x0 + 500, 0, z0}, {x0 + 500, 0, z0 + 500}, {x0, 0, z0 + 500}};
			addQuad(scene, p[0], p[1], p[2], p[3], 0);
		}
	{
		const float w[4][3] = {{600, 0, -300}, {600, 0, 300}, {600, 300, 300}, {600, 300, -300}};
		addQuad(scene, w[0], w[1], w[2], w[3], 0); // wall across +X
		const float g[4][3] = {{-600, 0, -300}, {-600, 0, 300}, {-600, 300, 300}, {-600, 300, -300}};
		addQuad(scene, g[0], g[1], g[2], g[3], 1); // same, but without collision
		addBlock(scene, -200, 200, 600, 1000, 30);    // a step
		addBlock(scene, -200, 200, -1000, -600, 120); // too tall to walk onto
		const float tan30 = 0.57735f, tan70 = 2.74748f;
		const float r[4][3] = {{800, 0, 1200}, {1200, 0, 1200}, {1200, 500 * tan30, 1700}, {800, 500 * tan30, 1700}};
		addQuad(scene, r[0], r[1], r[2], r[3], 0); // 30 degree ramp
		const float st[4][3] = {{-1200, 0, 1200}, {-800, 0, 1200}, {-800, 200 * tan70, 1400}, {-1200, 200 * tan70, 1400}};
		addQuad(scene, st[0], st[1], st[2], st[3], 0); // 70 degree slope
	}
	scene.spawnPos[1] = 220.0f;
	scene.hasSpawn = true;
	const std::string path = gDir + "/collision.pak";
	bakeScenePak(scene, path, "c", 1500.0f); // several chunks, so queries cross chunk borders

	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), "c"));
	World& w = app.world();
	w.preload(app.camera().pos);
	CHECK(w.stats().chunksLoaded > 4 && w.stats().collisionBytes > 0);

	// Raw queries.
	float y = -1;
	Vec3 n {};
	CHECK(w.rayDown({10, 100, 10}, 500, y, n) && std::fabs(y) < 0.2f && n.y > 0.99f);
	CHECK(w.rayDown({0, 100, 800}, 500, y, n) && std::fabs(y - 30) < 0.2f);  // on the step
	CHECK(w.rayDown({0, 20, 800}, 500, y, n) && std::fabs(y) < 0.2f);        // from inside it: the floor
	CHECK(!w.rayDown({10, -50, 10}, 500, y, n));                             // from below the world
	CHECK(!w.rayDown({9000, 100, 10}, 500, y, n));                           // outside it
	CHECK(!w.rayDown({10, 900, 10}, 500, y, n));                             // floor farther than the drop
	CHECK(w.rayDown({1000, 400, 1450}, 800, y, n) && std::fabs(y - 250 * 0.57735f) < 1.0f && n.y > 0.85f && n.y < 0.88f);
	Vec3 sphere {585, 150, 0};
	CHECK(w.pushSphere(sphere, 30) && std::fabs(sphere.x - 570) < 0.2f); // pushed off the wall
	sphere = {540, 150, 0};
	CHECK(!w.pushSphere(sphere, 30)); // clear of it
	sphere = {-585, 150, 0};
	CHECK(!w.pushSphere(sphere, 30)); // the ghost wall does not collide

	// The character controller. One step is 480 cm/s at 30 fps = 16 cm.
	const float dt = 1.0f / 30.0f;
	Player& pl = app.player();
	auto place = [&](float x, float z, float yawDegrees) {
		app.teleport({x, 220.0f, z});
		app.camera().yaw = yawDegrees * 0.0174533f;
		for (int i = 0; i < 20; ++i) app.update(Input {}, dt);
	};
	auto walk = [&](int frames, bool jump = false) {
		float top = -1e9f;
		for (int i = 0; i < frames; ++i) {
			Input in;
			in.moveZ = 1.0f;
			in.jump = jump && i == 0;
			app.update(in, dt);
			top = std::max(top, pl.feet.y);
		}
		return top;
	};

	place(0, 0, 0);
	CHECK(pl.placed() && pl.grounded && std::fabs(pl.feet.y) < 0.3f);
	CHECK(std::fabs(app.camera().pos.y - 170.0f) < 1.0f); // eyes settle at eye height

	place(0, 0, 90); // +X, into the wall
	walk(120);
	CHECK(pl.feet.x > 560 && pl.feet.x < 571 && pl.grounded);

	place(0, 0, -90); // -X, through the ghost wall
	walk(60);
	CHECK(pl.feet.x < -900);

	place(0, 0, 0); // +Z, over the 30 cm step and down again
	float top = walk(55);
	CHECK(std::fabs(top - 30) < 0.5f && std::fabs(pl.feet.y - 30) < 0.5f && pl.grounded);
	walk(30);
	CHECK(std::fabs(pl.feet.y) < 0.5f && pl.grounded && pl.feet.z > 1300);

	place(0, 0, 180); // -Z, against the 120 cm block
	top = walk(80);
	CHECK(pl.feet.z < -560 && pl.feet.z > -572 && top < 1.0f);

	place(1000, 900, 0); // up the 30 degree ramp
	walk(40);
	CHECK(pl.grounded && pl.feet.z > 1500 && std::fabs(pl.feet.y - (pl.feet.z - 1200) * 0.57735f) < 2.0f);

	place(-1000, 900, 0); // the 70 degree slope cannot be climbed
	top = walk(80);
	CHECK(top < 60.0f && pl.feet.z < 1230);

	place(0, 0, 0); // jump: about v^2 / 2g = 82 cm, then land
	top = -1e9f;
	for (int i = 0; i < 40; ++i) {
		Input in;
		in.jump = i == 0;
		app.update(in, dt);
		top = std::max(top, pl.feet.y);
	}
	CHECK(top > 60 && top < 95 && pl.grounded && std::fabs(pl.feet.y) < 0.3f);

	// Dropped from 10 m: falls and lands. (Placement looks 30 m down.)
	app.teleport({300, 1000.0f + 170.0f, -300});
	app.update(Input {}, dt);
	CHECK(pl.placed()); // snapped straight to the ground it found
	pl.feet.y = 1000;
	pl.grounded = false;
	pl.velocityY = 0;
	int frames = 0;
	while (!pl.grounded && frames < 200) {
		app.update(Input {}, dt);
		++frames;
	}
	CHECK(pl.grounded && std::fabs(pl.feet.y) < 0.3f && frames > 20 && frames < 45); // ~1 s for 10 m at 1900 cm/s^2

	// Outside the world there is nothing to stand on: wait, do not fall.
	app.teleport({6000, 220, 0});
	for (int i = 0; i < 30; ++i) app.update(Input {}, dt);
	CHECK(!pl.placed() && std::fabs(pl.feet.y - 50.0f) < 0.01f);

	// Fly mode ignores collision and gravity.
	place(0, 0, 90);
	Input fly;
	fly.toggleFly = true;
	app.update(fly, dt);
	CHECK(pl.flying);
	for (int i = 0; i < 60; ++i) {
		Input in;
		in.moveZ = 1.0f;
		app.update(in, dt);
	}
	CHECK(pl.feet.x > 1500 && std::fabs(pl.feet.y) < 1.0f); // straight through the wall

	app.shutdown();
	CHECK(gfx::memStats().meshCount == 0);
	gfx::shutdown();
}

static void testSky() {
	const SkyColors noon = skyColorsAt(13.0f), night = skyColorsAt(1.0f), wrap = skyColorsAt(25.0f);
	CHECK(noon.light[0] == 1.0f && noon.light[1] == 1.0f && noon.light[2] == 1.0f);
	CHECK(night.light[0] < 0.5f && night.zenith[2] < 60);
	CHECK(wrap.zenith[0] == night.zenith[0] && wrap.horizon[2] == night.horizon[2]);
	const SkyColors dawn = skyColorsAt(5.75f); // halfway between night and the dawn key
	CHECK(dawn.light[0] > night.light[0] && dawn.light[0] < 0.78f);
}

int main(int argc, char** argv) {
	gDir = argc > 1 ? argv[1] : ".";
	struct {
		const char* name;
		void (*fn)();
	} tests[] = {{"hash", testHash},     {"pak", testPak},         {"etc1 block decoder", testEtc1BlockDecoder},
	             {"texture", testTexture}, {"chunker", testChunker}, {"math", testMath},
	             {"runtime", testRuntime}, {"collision", testCollision}, {"sky", testSky}};
	for (auto& t : tests) {
		const int before = gFailures;
		std::printf("[ RUN ] %s\n", t.name);
		t.fn();
		std::printf("[ %s ] %s\n", gFailures == before ? " OK " : "FAIL", t.name);
	}
	std::printf("%d checks, %d failed\n", gChecks, gFailures);
	return gFailures ? 1 : 0;
}
