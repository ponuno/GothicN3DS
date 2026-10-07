// Unit and integration tests for the data path: archive, texture layout,
// chunker, streaming. Usage: test_core <scratch dir>
#include "app.h"
#include "bake.h"
#include "chunker.h"
#include "g3ds/pak.h"
#include "g3ds/tex.h"
#include "gfx_soft.h"
#include "importers.h"
#include "loader.h"
#include "model_bake.h"
#include "pacer.h"
#include "pak_writer.h"
#include "rg_etc1.h"
#include "test_models.h"
#include "texture_bake.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

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
	std::vector<BakedChunk> chunks = buildChunks(scene, remap, {0, 0}, opt, st);
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
	chunks = buildChunks(scene, remap, {0, 0}, opt, st2);
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
	buildChunks(plain, {0}, {0}, one, sp);
	const std::vector<BakedChunk> merged = buildChunks(shifted, {0}, {0}, one, ss);
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

	// Draw order and detail classes: batches come out sorted by order key
	// (opaque, cut-out, blended), then detail class, then material; each batch
	// knows its bounds.
	{
		SrcScene mix;
		mix.materials.resize(3);
		auto quadAt = [&](float x, u32 material, u8 detail) {
			SrcTriangle tr = plain.triangles[0], tr2 = plain.triangles[1];
			for (SrcTriangle* q : {&tr, &tr2}) {
				for (SrcVertex& v : q->v) v.pos[0] += x;
				q->material = material;
				q->detail = detail;
				mix.triangles.push_back(*q);
			}
		};
		quadAt(0, 0, DETAIL_SMALL);    // world material 2: blended
		quadAt(1000, 1, DETAIL_ALWAYS); // world material 0: opaque
		quadAt(2000, 1, DETAIL_SMALL);
		quadAt(3000, 2, DETAIL_MEDIUM); // world material 1: cut-out
		quadAt(4000, 1, DETAIL_ALWAYS);
		ChunkerStats sm;
		const std::vector<BakedChunk> mc = buildChunks(mix, {2, 0, 1}, {0, 1, 2}, one, sm);
		CHECK(mc.size() == 1 && mc[0].batches.size() == 4);
		if (mc.size() == 1 && mc[0].batches.size() == 4) {
			const BakedChunk& c = mc[0];
			const u16 wantMaterial[4] = {0, 0, 1, 2};
			const u8 wantDetail[4] = {DETAIL_ALWAYS, DETAIL_SMALL, DETAIL_MEDIUM, DETAIL_SMALL};
			const float wantX[4][2] = {{1000, 4200}, {2000, 2200}, {3000, 3200}, {0, 200}};
			for (int b = 0; b < 4; ++b) {
				CHECK(c.batches[b].material == wantMaterial[b] && c.batches[b].detail == wantDetail[b]);
				CHECK(c.batches[b].indexCount == (b == 0 ? 12u : 6u));
				const float x0 = c.header.origin[0] + float(c.batches[b].bbMin[0]) * c.header.posScale;
				const float x1 = c.header.origin[0] + float(c.batches[b].bbMax[0]) * c.header.posScale;
				CHECK(std::fabs(x0 - wantX[b][0]) < 0.3f && std::fabs(x1 - wantX[b][1]) < 0.3f);
				CHECK(c.batches[b].bbMin[1] <= c.batches[b].bbMax[1] && c.batches[b].bbMin[2] < c.batches[b].bbMax[2]);
			}
		}
		CHECK(sm.batches == 4 && sm.detailTriangles[DETAIL_ALWAYS] == 4 && sm.detailTriangles[DETAIL_MEDIUM] == 2 &&
		      sm.detailTriangles[DETAIL_SMALL] == 4);
	}

	// One lattice for the whole world: a vertex on the border of two chunks
	// lands on bit-identical world coordinates in both, whatever its position,
	// so there are no hairline cracks along chunk borders.
	{
		SrcScene seam;
		seam.materials.resize(1);
		std::mt19937 jitter(5);
		auto h = [&](int ix, int iz) { return float((ix * 7919 + iz * 104729) % 1000) * 0.137f - 40.0f; };
		for (int z = 0; z < 12; ++z)
			for (int x = 0; x < 12; ++x) {
				SrcTriangle tr;
				tr.material = 0;
				const int cx[4] = {x, x + 1, x + 1, x}, cz[4] = {z, z, z + 1, z + 1};
				SrcVertex q[4] = {};
				for (int k = 0; k < 4; ++k) {
					// Awkward coordinates: nothing lines up with the lattice by itself.
					q[k].pos[0] = float(cx[k]) * 333.33f - 1777.7f;
					q[k].pos[1] = h(cx[k], cz[k]);
					q[k].pos[2] = float(cz[k]) * 333.33f + 12345.6f;
					q[k].rgba[3] = 255;
				}
				tr.v[0] = q[0]; tr.v[1] = q[1]; tr.v[2] = q[2];
				seam.triangles.push_back(tr);
				tr.v[1] = q[2]; tr.v[2] = q[3];
				seam.triangles.push_back(tr);
			}
		(void)jitter;
		ChunkerOptions small;
		small.chunkSize = 1000.0f;
		ChunkerStats sq;
		const std::vector<BakedChunk> sc = buildChunks(seam, {0}, {0}, small, sq);
		CHECK(sc.size() > 9);
		// Collect every world position each source corner was written to.
		std::map<std::pair<int, int>, std::vector<std::array<float, 3>>> seen;
		for (const BakedChunk& c : sc)
			for (const WorldVertex& v : c.vertices) {
				const float wx = c.header.origin[0] + float(v.x) * c.header.posScale;
				const float wy = c.header.origin[1] + float(v.y) * c.header.posScale;
				const float wz = c.header.origin[2] + float(v.z) * c.header.posScale;
				const int ix = int(std::lround((wx + 1777.7f) / 333.33f)), iz = int(std::lround((wz - 12345.6f) / 333.33f));
				seen[{ix, iz}].push_back({wx, wy, wz});
				CHECK(std::fabs(wx - (float(ix) * 333.33f - 1777.7f)) <= 0.07f && std::fabs(wy - h(ix, iz)) <= 0.07f);
			}
		size_t shared = 0;
		for (const auto& kv : seen) {
			shared += kv.second.size() > 1;
			for (const auto& p : kv.second) CHECK(p == kv.second[0]);
		}
		CHECK(seen.size() == 13 * 13 && shared > 20); // many corners sit in two or more chunks
		CHECK(std::fabs(sc[0].header.posScale - 0.125f) < 1e-6f);
		CHECK(sq.stepChunks[0] == sc.size() && sq.tJunctions == 0 && sq.welded == 0);

		// A triangle too large for the base step gets a coarser one - in a part
		// of its own, so the rest of its cell stays fine. The corners it shares
		// with ordinary triangles move to the coarse lattice everywhere, so the
		// two parts still agree about them to the bit.
		SrcScene giant = seam;
		SrcTriangle g = seam.triangles[0];
		g.v[1] = g.v[2];
		g.v[2] = seam.triangles[1].v[2]; // one edge of the first quad ...
		g.v[0].pos[0] -= 30000.0f;       // ... and a corner 300 m away
		g.v[0].pos[2] = g.v[1].pos[2];
		giant.triangles.push_back(g);
		ChunkerStats sg;
		const std::vector<BakedChunk> gc = buildChunks(giant, {0}, {0}, small, sg);
		CHECK(gc.size() == sc.size() + 1 && sg.outputTriangles == sq.outputTriangles + 1);
		CHECK(sg.stepChunks[0] == sc.size() && sg.stepChunks[2] == 1);
		const BakedChunk* coarse = nullptr;
		for (const BakedChunk& c : gc)
			if (c.header.posScale > 0.2f) coarse = &c;
		CHECK(coarse && std::fabs(coarse->header.posScale - 0.5f) < 1e-6f && coarse->vertices.size() == 3);
		if (coarse) {
			int matched = 0;
			for (const WorldVertex& v : coarse->vertices) {
				const std::array<float, 3> p = {coarse->header.origin[0] + float(v.x) * coarse->header.posScale,
				                                coarse->header.origin[1] + float(v.y) * coarse->header.posScale,
				                                coarse->header.origin[2] + float(v.z) * coarse->header.posScale};
				for (const BakedChunk& c : gc) {
					if (&c == coarse) continue;
					for (const WorldVertex& o : c.vertices)
						matched += p == std::array<float, 3> {c.header.origin[0] + float(o.x) * c.header.posScale,
						                                      c.header.origin[1] + float(o.y) * c.header.posScale,
						                                      c.header.origin[2] + float(o.z) * c.header.posScale};
				}
			}
			CHECK(matched >= 2); // both shared corners found, bit for bit, in a fine chunk
		}
	}

	// A triangle spanning more texture repeats than 16.16 fixed point holds.
	SrcScene wide;
	wide.materials.resize(1);
	SrcTriangle t = scene.triangles[0];
	t.v[1].uv[0] = t.v[0].uv[0] + 100.0f;
	wide.triangles.push_back(t);
	ChunkerStats st3;
	buildChunks(wide, {0}, {0}, ChunkerOptions {}, st3);
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

// Bakes one scene into a pak with the pipeline the command-line tool uses.
static void bakeScenePak(const SrcScene& scene, const std::string& path, const char* name, const ChunkerOptions& chunker) {
	std::vector<BakeWorld> worlds(1);
	worlds[0].name = name;
	worlds[0].scene = scene;
	BakeOptions opt;
	opt.chunker = chunker;
	opt.tex.maxSize = 32;
	opt.tex.quality = 0;
	opt.threads = 2;
	opt.quiet = true;
	PakWriter pak;
	bakeWorlds(worlds, opt, pak);
	pak.write(path);
}

static void bakeScenePak(const SrcScene& scene, const std::string& path, const char* name, float chunkSize) {
	ChunkerOptions chunker;
	chunker.chunkSize = chunkSize;
	bakeScenePak(scene, path, name, chunker);
}

// A wall the way level designers build them: neighbouring strips cut into
// different numbers of pieces, so corners of one strip rest on the edges of
// the next ("T-junctions"), and one strip whose corners are a hair off.
static void buildSeamWall(SrcScene& scene, bool magentaRoom) {
	scene.materials.resize(1);
	scene.materials[0].name = "wall";
	scene.materials[0].texture = "WALL";
	scene.loadTexture = [](const std::string&, SrcImage& out) {
		out.width = out.height = 8;
		out.rgba.assign(8 * 8 * 4, 255);
		for (size_t i = 0; i < out.rgba.size(); i += 4) {
			out.rgba[i] = 40;
			out.rgba[i + 1] = 160;
			out.rgba[i + 2] = 40;
		}
		return true;
	};
	if (magentaRoom) {
		scene.indoor = true; // no sky: whatever shows through a gap is exactly this colour
		scene.fogColor[0] = 255;
		scene.fogColor[1] = 0;
		scene.fogColor[2] = 255;
	}
	scene.hasSpawn = true;
	const float angle = 0.47f, ox = 1234.567f, oy = 33.33f, oz = -4321.09f;
	auto at = [&](float along, float up, float noise) {
		SrcVertex v {};
		v.pos[0] = ox + along * std::cos(angle) + noise;
		v.pos[1] = oy + up - noise;
		v.pos[2] = oz + along * std::sin(angle) + noise;
		v.uv[0] = along / 100.0f;
		v.uv[1] = -up / 100.0f;
		v.rgba[0] = v.rgba[1] = v.rgba[2] = 255;
		v.rgba[3] = 0;
		return v;
	};
	auto quad = [&](float s0, float s1, float h0, float h1, float noise) {
		SrcTriangle a, b;
		a.material = b.material = 0;
		a.v[0] = at(s0, h0, noise); a.v[1] = at(s1, h0, noise); a.v[2] = at(s1, h1, noise);
		b.v[0] = at(s0, h0, noise); b.v[1] = at(s1, h1, noise); b.v[2] = at(s0, h1, noise);
		scene.triangles.push_back(a);
		scene.triangles.push_back(b);
	};
	// Six columns, 2 m wide and 5 m tall, each cut into its own number of rows.
	const int rows[6] = {1, 3, 2, 5, 1, 4};
	for (int c = 0; c < 6; ++c)
		for (int r = 0; r < rows[c]; ++r)
			quad(float(c) * 200.0f, float(c + 1) * 200.0f, 500.0f * float(r) / float(rows[c]),
			     500.0f * float(r + 1) / float(rows[c]), 0.0f);
	// A band along the top, cut into seven and placed a hundredth of a
	// millimetre off: its corners are not the columns' corners.
	for (int c = 0; c < 7; ++c) quad(1200.0f * float(c) / 7.0f, 1200.0f * float(c + 1) / 7.0f, 500.0f, 600.0f, 0.001f);
}

// Total length of the triangle edges that have no partner running the other
// way through the same two points: the outline of the mesh plus every crack.
static double openEdgeLength(const std::vector<BakedChunk>& chunks) {
	using Point = std::array<long long, 3>;
	std::map<std::pair<Point, Point>, int> edges;
	for (const BakedChunk& c : chunks)
		for (size_t i = 0; i + 2 < c.indices.size(); i += 3) {
			Point p[3];
			for (int k = 0; k < 3; ++k) {
				const WorldVertex& v = c.vertices[c.indices[i + size_t(k)]];
				const i16 q[3] = {v.x, v.y, v.z};
				for (int a = 0; a < 3; ++a)
					p[k][size_t(a)] = std::llround((double(c.header.origin[a]) + double(q[a]) * double(c.header.posScale)) * 64.0);
			}
			for (int k = 0; k < 3; ++k) ++edges[{p[k], p[(k + 1) % 3]}];
		}
	double open = 0;
	for (const auto& kv : edges) {
		auto back = edges.find({kv.first.second, kv.first.first});
		const int unmatched = kv.second - (back == edges.end() ? 0 : back->second);
		if (unmatched <= 0) continue;
		double d2 = 0;
		for (size_t a = 0; a < 3; ++a) {
			const double d = double(kv.first.first[a] - kv.first.second[a]) / 64.0;
			d2 += d * d;
		}
		open += std::sqrt(d2) * unmatched;
	}
	return open;
}

static void bakeScenePak(const SrcScene& scene, const std::string& path, const char* name, const ChunkerOptions& chunker);

// How many pixels show the background through the wall, seen from 40 cm.
static int seamPixels(const std::string& path) {
	App app;
	if (!app.init(path.c_str(), "seam")) return -1;
	gfx::softSetCulling(false); // either side of the wall will do
	const float angle = 0.47f, ox = 1234.567f, oy = 33.33f, oz = -4321.09f;
	int holes = 0;
	for (int i = 0; i < 24; ++i) {
		const float along = 150.0f + 39.0f * float(i), up = 180.0f + float((i * 131) % 240);
		Camera& cam = app.camera();
		cam.pos = {ox + along * std::cos(angle) - 40.0f * std::sin(angle), oy + up,
		           oz + along * std::sin(angle) + 40.0f * std::cos(angle)};
		cam.yaw = std::atan2(std::sin(angle), -std::cos(angle)) + 0.3f * float(i % 3 - 1);
		cam.pitch = 0.25f * float(i % 5 - 2);
		app.world().preload(cam.pos);
		app.render();
		const u8* fb = gfx::softFramebuffer();
		for (int p = 0; p < gfx::SCREEN_W * gfx::SCREEN_H; ++p) {
			const bool hole = fb[p * 4] > 200 && fb[p * 4 + 1] < 60 && fb[p * 4 + 2] > 200;
			holes += hole;
			if (hole && getenv("SEAM_DEBUG")) std::printf("    view %d: hole at %d,%d\n", i, p % gfx::SCREEN_W, p / gfx::SCREEN_W);
		}
	}
	gfx::softSetCulling(true);
	app.shutdown();
	return holes;
}

static void testSeams() {
	SrcScene wall;
	buildSeamWall(wall, true);
	const double outline = 2.0 * (1200.0 + 600.0);

	// The old pipeline: half-centimetre lattice, vertices rounded one by one.
	ChunkerOptions before;
	before.chunkSize = 500.0f;
	before.positionStep = 0.5f;
	before.seal = false;
	ChunkerStats sb;
	const std::vector<BakedChunk> cracked = buildChunks(wall, {0}, {0}, before, sb);
	const double openBefore = openEdgeLength(cracked);

	ChunkerOptions now;
	now.chunkSize = 500.0f;
	ChunkerStats sn;
	const std::vector<BakedChunk> sealed = buildChunks(wall, {0}, {0}, now, sn);
	const double openNow = openEdgeLength(sealed);
	std::printf("  seams: open edges %.0f cm before, %.0f cm sealed (outline %.0f cm); %llu T-junctions, %llu welded, "
	            "+%llu triangles\n",
	            openBefore, openNow, outline, static_cast<unsigned long long>(sn.tJunctions),
	            static_cast<unsigned long long>(sn.welded), static_cast<unsigned long long>(sn.sealTriangles));
	CHECK(cracked.size() >= 4 && sealed.size() >= 4); // the wall spans several chunks
	CHECK(openBefore > outline + 3000.0);           // the test wall really has cracks ...
	CHECK(std::fabs(openNow - outline) < 2.0);      // ... and sealed, only its outline is open
	CHECK(sn.tJunctions > 15 && sn.sealTriangles > 15 && sn.degenerate == 0);
	CHECK(sn.outputTriangles == sn.inputTriangles + sn.sealTriangles);
	CHECK(sb.tJunctions == 0 && sb.sealTriangles == 0 && sb.outputTriangles == sb.inputTriangles);
	// Nothing moved by more than the tolerance allows, and the wall kept its area.
	auto area = [](const std::vector<BakedChunk>& chunks) {
		double sum = 0;
		for (const BakedChunk& c : chunks)
			for (size_t i = 0; i + 2 < c.indices.size(); i += 3) {
				double p[3][3];
				for (int k = 0; k < 3; ++k) {
					const WorldVertex& v = c.vertices[c.indices[i + size_t(k)]];
					p[k][0] = double(v.x) * c.header.posScale;
					p[k][1] = double(v.y) * c.header.posScale;
					p[k][2] = double(v.z) * c.header.posScale;
				}
				const double a[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
				const double b[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
				const double n[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
				sum += 0.5 * std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			}
		return sum;
	};
	CHECK(std::fabs(area(sealed) - 1200.0 * 600.0) < 1200.0 * 600.0 * 0.001);

	// A sliver triangle that only exists to fill a T-junction closes up
	// without taking its neighbours along.
	{
		SrcScene sliver;
		sliver.materials.resize(1);
		auto v = [](float x, float y) {
			SrcVertex s {};
			s.pos[0] = x;
			s.pos[1] = y;
			return s;
		};
		auto tri = [&](SrcVertex a, SrcVertex b, SrcVertex c) {
			SrcTriangle t;
			t.material = 0;
			t.v[0] = a; t.v[1] = b; t.v[2] = c;
			sliver.triangles.push_back(t);
		};
		tri(v(0, 0), v(200, 0), v(100, 300));     // the big one, edge (0,0)-(200,0) below
		tri(v(0, 0), v(100, -300), v(100, 0.0f)); // two below it, meeting in the middle of that edge
		tri(v(100, 0.0f), v(100, -300), v(200, 0));
		tri(v(0, 0), v(100, 0.0f), v(200, 0));    // zero area: the "fix" some exporters leave behind
		ChunkerStats ssl;
		ChunkerOptions one;
		one.chunkSize = 100000.0f;
		const std::vector<BakedChunk> sc = buildChunks(sliver, {0}, {0}, one, ssl);
		CHECK(ssl.inputTriangles == 4 && ssl.degenerate == 1 && ssl.outputTriangles == 4 && ssl.sealTriangles == 1);
		CHECK(std::fabs(openEdgeLength(sc) - (2.0 * std::sqrt(100.0 * 100.0 + 300.0 * 300.0) * 2.0)) < 1.0);
	}

	// Random walls: a rectangle cut again and again along random lines, so
	// that corners end up on edges everywhere, turned to an arbitrary angle,
	// every piece with float noise of its own. Sealed, each must be watertight
	// (only its outline open) without losing area.
	{
		std::mt19937 rng(2024);
		auto rnd = [&](float lo, float hi) { return lo + (hi - lo) * float(rng() % 100000) / 100000.0f; };
		double worstOpen = 0, worstArea = 0;
		u64 junctions = 0;
		for (int round = 0; round < 40; ++round) {
			struct Rect {
				float s0, h0, s1, h1;
			};
			const float width = rnd(300, 2500), height = rnd(200, 900);
			std::vector<Rect> rects = {{0, 0, width, height}};
			const int cuts = 3 + int(rng() % 40);
			for (int c = 0; c < cuts; ++c) {
				const size_t pick = rng() % rects.size();
				const Rect r = rects[pick];
				const bool vertical = (r.s1 - r.s0) > (r.h1 - r.h0) ? rng() % 4 != 0 : rng() % 4 == 0;
				const float span = vertical ? r.s1 - r.s0 : r.h1 - r.h0;
				if (span < 12.0f) continue;
				const float at = (vertical ? r.s0 : r.h0) + span * rnd(0.2f, 0.8f);
				rects[pick] = vertical ? Rect {r.s0, r.h0, at, r.h1} : Rect {r.s0, r.h0, r.s1, at};
				rects.push_back(vertical ? Rect {at, r.h0, r.s1, r.h1} : Rect {r.s0, at, r.s1, r.h1});
			}
			const float yaw = rnd(0, 6.28f), tilt = rnd(-0.4f, 0.4f);
			const float o[3] = {rnd(-90000, 90000), rnd(-3000, 3000), rnd(-90000, 90000)};
			SrcScene sc;
			sc.materials.resize(1);
			for (const Rect& r : rects) {
				const float n[3] = {rnd(-0.004f, 0.004f), rnd(-0.004f, 0.004f), rnd(-0.004f, 0.004f)};
				auto at = [&](float along, float up) {
					SrcVertex v {};
					// A tilted plane: `up` leans out of the vertical.
					v.pos[0] = o[0] + along * std::cos(yaw) + up * std::sin(tilt) * -std::sin(yaw) + n[0];
					v.pos[1] = o[1] + up * std::cos(tilt) + n[1];
					v.pos[2] = o[2] + along * std::sin(yaw) + up * std::sin(tilt) * std::cos(yaw) + n[2];
					v.uv[0] = along / 128.0f;
					v.uv[1] = up / 128.0f;
					v.rgba[0] = u8(rng());
					v.rgba[3] = 255;
					return v;
				};
				SrcTriangle a, b;
				a.material = b.material = 0;
				a.v[0] = at(r.s0, r.h0); a.v[1] = at(r.s1, r.h0); a.v[2] = at(r.s1, r.h1);
				b.v[0] = at(r.s0, r.h0); b.v[1] = at(r.s1, r.h1); b.v[2] = at(r.s0, r.h1);
				// Either diagonal.
				if (rng() % 2) {
					a.v[2] = at(r.s0, r.h1);
					b.v[0] = at(r.s1, r.h0);
				}
				sc.triangles.push_back(a);
				sc.triangles.push_back(b);
			}
			ChunkerOptions co;
			co.chunkSize = rnd(400, 4000);
			ChunkerStats st;
			const std::vector<BakedChunk> out = buildChunks(sc, {0}, {0}, co, st);
			junctions += st.tJunctions;
			worstOpen = std::max(worstOpen, std::fabs(openEdgeLength(out) - 2.0 * (double(width) + double(height))));
			worstArea = std::max(worstArea, std::fabs(area(out) / (double(width) * double(height)) - 1.0));
			CHECK(st.outputTriangles == st.inputTriangles + st.sealTriangles - st.degenerate);
			for (const BakedChunk& c : out) CHECK(std::fabs(c.header.posScale - 0.125f) < 1e-6f);
		}
		std::printf("  random walls: %llu T-junctions closed; outline off by <= %.2f cm, area by <= %.4f%%\n",
		            static_cast<unsigned long long>(junctions), worstOpen, worstArea * 100.0);
		CHECK(junctions > 500);
		CHECK(worstOpen < 3.0);    // rounding of the outline itself
		CHECK(worstArea < 0.002);
	}

	// The same wall on screen, from as close as the player gets to a wall.
	const std::string cracksPath = gDir + "/seam-before.pak", sealedPath = gDir + "/seam-sealed.pak";
	bakeScenePak(wall, cracksPath, "seam", before);
	bakeScenePak(wall, sealedPath, "seam", now);
	gfx::init();
	const int holesBefore = seamPixels(cracksPath), holesNow = seamPixels(sealedPath);
	gfx::shutdown();
	std::printf("  seams on screen (24 close-ups): %d background pixels before, %d sealed\n", holesBefore, holesNow);
	CHECK(holesBefore > 50);
	CHECK(holesNow == 0);
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
		// ... and always takes the nearest missing chunks first.
		{
			const Vec3 c = app.camera().pos;
			auto distance = [&](u32 i) {
				const ChunkInfo& ci = w.chunkInfo(i);
				const float dx = std::max({ci.bbMin[0] - c.x, c.x - ci.bbMax[0], 0.0f});
				const float dy = std::max({ci.bbMin[1] - c.y, c.y - ci.bbMax[1], 0.0f});
				const float dz = std::max({ci.bbMin[2] - c.z, c.z - ci.bbMax[2], 0.0f});
				return std::sqrt(dx * dx + dy * dy + dz * dz);
			};
			// All but two of the missing chunks in one go: the two left out
			// must be the farthest ones.
			const u32 pendingBefore = w.stats().chunksPending;
			CHECK(pendingBefore > 6);
			w.config().maxLoadsPerFrame = pendingBefore - 2;
			app.update(Input {}, 1.0f / 30.0f);
			CHECK(w.stats().chunksLoaded == pendingBefore - 1 && w.stats().chunksPending == 2);
			float farthestLoaded = 0, nearestMissing = 1e30f;
			for (u32 i = 0; i < w.stats().chunksTotal; ++i) {
				if (w.chunkResident(i))
					farthestLoaded = std::max(farthestLoaded, distance(i));
				else
					nearestMissing = std::min(nearestMissing, distance(i));
			}
			CHECK(farthestLoaded <= nearestMissing);
		}
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

		const u32 skyBytes = gfx::memStats().meshBytes + gfx::memStats().textureBytes - w.stats().residentBytes; // the dome

		// Far away from everything: all chunks are released again.
		app.teleport({1e6f, 0, 1e6f});
		app.update(Input {}, 1.0f / 30.0f);
		CHECK(w.stats().chunksLoaded == 0 && w.stats().residentBytes == 0 && w.stats().texturesLoaded == 0);
		CHECK(w.stats().collisionBytes == 0);
		CHECK(gfx::memStats().meshCount == 1 && gfx::memStats().textureCount == 0); // only the sky dome is left

		// The allocator runs dry before the budget does (a full linear heap on
		// the console): chunks and textures wait instead of being written off,
		// and arrive once there is room.
		app.teleport({scene.spawnPos[0], scene.spawnPos[1], scene.spawnPos[2]});
		gfx::softSetMemoryLimit(gfx::memStats().meshBytes + 40 * 1024);
		w.preload(app.camera().pos);
		const u32 starved = w.stats().chunksLoaded;
		CHECK(starved > 0 && starved < near && w.stats().budgetRejects > 0 && w.stats().loadFailures == 0);
		CHECK(w.stats().chunksPending == near - starved);
		app.render(); // whatever is there can be drawn, textured or not
		gfx::softSetMemoryLimit(0);
		w.preload(app.camera().pos);
		for (int i = 0; i < 40; ++i) app.update(Input {}, 1.0f / 30.0f); // one waiting texture per frame
		CHECK(w.stats().chunksLoaded == near && w.stats().loadFailures == 0 && w.stats().chunksPending == 0);
		CHECK(w.stats().residentBytes == gfx::memStats().meshBytes + gfx::memStats().textureBytes - skyBytes);
		app.render();
		CHECK(w.stats().trianglesDrawn > 500);
		app.teleport({1e6f, 0, 1e6f});
		app.update(Input {}, 1.0f / 30.0f);
		CHECK(w.stats().chunksLoaded == 0 && w.stats().texturesLoaded == 0 && w.stats().residentBytes == 0);
		w.config() = StreamConfig {};
		app.setDrawDistance(6000.0f);

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

// Two worlds in one archive: portals, water (blending, animation, wading,
// swimming, the view from below), detail classes.
static void testWorlds() {
	std::vector<BakeWorld> worlds(2);
	worlds[0].name = "world";
	worlds[0].level = "WORLD.ZEN";
	SynthOptions so;
	so.cells = 48;
	so.huts = 8;
	so.trees = 60;
	buildSyntheticScene(so, worlds[0].scene);
	worlds[1].name = "cave";
	worlds[1].level = "CAVE.ZEN";
	buildSyntheticCave(worlds[1].scene);
	const float gate[3] = {worlds[0].scene.points["CAMP_GATE"].pos[0], worlds[0].scene.points["CAMP_GATE"].pos[1],
	                       worlds[0].scene.points["CAMP_GATE"].pos[2]};
	BakeOptions bo;
	bo.tex.maxSize = 32;
	bo.tex.quality = 0;
	bo.threads = 2;
	bo.quiet = true;
	PakWriter writer;
	const BakeResult result = bakeWorlds(worlds, bo, writer);
	const std::string path = gDir + "/worlds.pak";
	writer.write(path);
	// Textures are shared: both worlds use the same water, rock and dirt.
	CHECK(result.textures == 13 && result.report.find("== world 'cave'") != std::string::npos);
	CHECK(result.report.find("portals: 1 of 1 lead to a baked world: cave at CAVE_ENTRY") != std::string::npos);
	CHECK(result.report.find("portals: 1 of 1 lead to a baked world: world at CAMP_GATE") != std::string::npos);
	CHECK(result.report.find("1 animated, 1 scrolling") != std::string::npos);

	const float dt = 1.0f / 30.0f;
	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr)); // the archive names its start world
	CHECK(std::string(app.worldName()) == "world/world");
	World& w = app.world();
	Player& pl = app.player();
	CHECK(w.header().portalCount == 1 && w.header().textureCount == 14);
	w.preload(app.camera().pos);

	// ---- detail classes: small objects are only drawn near the camera ----
	app.player().flying = true;
	app.teleport({0, 600, 0}); // above the middle of the camp, barrels all around
	app.camera().pitch = -0.6f;
	app.update(Input {}, dt);
	w.preload(app.camera().pos);
	app.render();
	const u32 trisNear = w.stats().trianglesDrawn, culledNear = w.stats().batchesCulled;
	w.config().detailDistance[DETAIL_SMALL] = 0.0f;
	w.config().detailDistance[DETAIL_MEDIUM] = 0.0f;
	app.render();
	std::printf("  detail classes: %u triangles with objects, %u without them\n", trisNear, w.stats().trianglesDrawn);
	CHECK(w.stats().trianglesDrawn < trisNear && w.stats().batchesCulled > culledNear);
	w.config() = StreamConfig {};
	app.setDrawDistance(12000.0f);

	// ---- blended pass: the glow and the cobweb in the gate ----
	app.teleport({gate[0], gate[1] + 175.0f, gate[2]});
	app.camera().yaw = 3.14159f; // CAMP_GATE looks away from the gate; turn round
	app.camera().pitch = 0;
	app.update(Input {}, dt);
	w.preload(app.camera().pos);
	app.render();
	CHECK(w.stats().blendedDrawCalls >= 2 && w.stats().drawCalls > w.stats().blendedDrawCalls);
	// Additive light in the gate: brighter and warmer than anything the lit
	// world produces on its own (the cobweb's threads cover part of it).
	{
		const u8* fb = gfx::softFramebuffer();
		int glowing = 0;
		for (int i = 0; i < gfx::SCREEN_W * gfx::SCREEN_H; ++i) glowing += fb[i * 4] > 230 && fb[i * 4] > fb[i * 4 + 2] + 40;
		std::printf("  blended pass: %d glowing pixels\n", glowing);
		CHECK(glowing > 100 && glowing < 4000);
	}

	// ---- portal: walking through the gate leads into the cave ----
	pl.flying = false;
	app.teleport({gate[0], gate[1] + 175.0f, gate[2]});
	app.camera().yaw = 3.14159f;
	for (int i = 0; i < 20; ++i) app.update(Input {}, dt); // settle on the ground
	CHECK(pl.grounded && app.worldSwitches() == 0);
	int frames = 0;
	for (; frames < 120 && app.worldSwitches() == 0; ++frames) {
		Input in;
		in.moveZ = 1.0f;
		app.update(in, dt);
	}
	CHECK(app.worldSwitches() == 1 && frames > 15);
	CHECK(std::string(app.worldName()) == "world/cave");
	CHECK(std::fabs(app.camera().pos.x) < 1.0f && std::fabs(app.camera().pos.z - 900.0f) < 1.0f);
	CHECK(std::fabs(app.camera().yaw) < 1e-4f);
	CHECK(w.stats().chunksLoaded == 4 && w.stats().chunksTotal == 4); // preloaded on arrival
	for (int i = 0; i < 20; ++i) app.update(Input {}, dt);
	CHECK(pl.grounded && std::fabs(pl.feet.y) < 0.3f && app.worldSwitches() == 1); // no bounce back

	// ---- water: wade in, swim, come back out ----
	bool waded = false;
	for (int i = 0; i < 400 && pl.feet.z < 2400.0f; ++i) {
		Input in;
		in.moveZ = 1.0f;
		app.update(in, dt);
		waded = waded || (!pl.swimming && pl.grounded && pl.waterDepth > 60.0f);
	}
	CHECK(waded && pl.swimming && !pl.grounded);
	// Afloat: the feet hang 125 cm below the surface (at -20), the eyes are above it.
	CHECK(std::fabs(pl.feet.y - (-20.0f - 125.0f)) < 2.0f && app.camera().pos.y > -20.0f);
	const float swimStart = pl.feet.z;
	for (int i = 0; i < 30; ++i) {
		Input in;
		in.moveZ = 1.0f;
		in.fast = true; // no sprinting in water
		app.update(in, dt);
	}
	CHECK(pl.feet.z - swimStart < 300.0f && pl.feet.z > swimStart + 100.0f);
	// The far wall of the pool stops the swimmer.
	for (int i = 0; i < 200; ++i) {
		Input in;
		in.moveZ = 1.0f;
		app.update(in, dt);
	}
	CHECK(pl.swimming && pl.feet.z < 2750.0f && pl.feet.z > 2650.0f);
	// Looking back over the pool: the water is drawn in the blended pass, and
	// being animated and scrolling, the same view changes over time.
	app.camera().yaw = 3.14159f;
	app.camera().pitch = -0.35f;
	app.update(Input {}, dt);
	app.render();
	CHECK(!app.underwater() && w.stats().blendedDrawCalls > 0);
	const std::vector<u8> before(gfx::softFramebuffer(), gfx::softFramebuffer() + gfx::SCREEN_W * gfx::SCREEN_H * 4);
	app.update(Input {}, 0.3f); // more than one animation frame (4 per second)
	app.render();
	int changed = 0;
	for (size_t i = 0; i < before.size(); i += 4) changed += std::abs(int(before[i + 2]) - int(gfx::softFramebuffer()[i + 2])) > 3;
	std::printf("  water animation: %d pixels changed in 0.3 s\n", changed);
	CHECK(changed > 2000);
	app.camera().pitch = 0;

	// Back up the ramp.
	for (int i = 0; i < 400 && pl.feet.z > 1300.0f; ++i) {
		Input in;
		in.moveZ = 1.0f;
		app.update(in, dt);
	}
	CHECK(!pl.swimming && pl.grounded && std::fabs(pl.feet.y) < 0.5f && pl.waterDepth == 0.0f);

	// ---- under water the view is fogged in ----
	pl.flying = true;
	app.teleport({0, -200.0f, 2400.0f});
	app.update(Input {}, dt);
	app.render();
	CHECK(app.underwater());
	app.teleport({0, 300.0f, 2400.0f});
	app.update(Input {}, dt);
	app.render();
	CHECK(!app.underwater());
	float surface = 0;
	CHECK(w.waterBelow({0, 300, 2400}, 1000, surface) && std::fabs(surface + 20.0f) < 0.3f);
	CHECK(!w.waterBelow({0, 300, 1000}, 1000, surface)); // dry floor
	CHECK(!w.waterBelow({0, -100, 2400}, 1000, surface)); // from below the surface
	pl.flying = false;

	// ---- an indoor world has no sky and no time of day ----
	CHECK(w.indoor() && w.header().fogColor[0] == 14 && w.header().ladderCount == 1);
	app.teleport({0, 175.0f, 900.0f});
	app.camera().yaw = 0.4f;
	app.environment().timeOfDay = 13.0f;
	for (int i = 0; i < 20; ++i) app.update(Input {}, dt);
	app.render();
	const std::vector<u8> noon(gfx::softFramebuffer(), gfx::softFramebuffer() + gfx::SCREEN_W * gfx::SCREEN_H * 4);
	// A whole number of water animation cycles later (1 s; the scroll period is far longer), at night.
	app.environment().timeOfDay = 1.0f;
	app.render();
	int differing = 0, bright = 0;
	for (size_t i = 0; i < noon.size(); i += 4) {
		differing += std::abs(int(noon[i]) - int(gfx::softFramebuffer()[i])) > 2;
		bright += noon[i] > 40;
	}
	CHECK(differing == 0 && bright > 10000); // lit by its own light, at any hour
	app.environment().timeOfDay = 13.0f;

	// ---- ladder: walk into it to climb, step off at the top, back down again ----
	app.teleport({-1200.0f, 175.0f, 3000.0f});
	app.camera().yaw = 0;
	for (int i = 0; i < 20; ++i) app.update(Input {}, dt);
	CHECK(pl.grounded && !pl.climbing);
	bool climbed = false;
	float highest = 0;
	for (int i = 0; i < 300 && !(pl.grounded && pl.feet.y > 290.0f && pl.feet.z > 3450.0f); ++i) {
		Input in;
		in.moveZ = 1.0f;
		app.update(in, dt);
		climbed = climbed || pl.climbing;
		highest = std::max(highest, pl.feet.y);
	}
	std::printf("  ladder: up to %.0f cm, standing at %.0f %.0f %.0f\n", double(highest), double(pl.feet.x), double(pl.feet.y),
	            double(pl.feet.z));
	CHECK(climbed && !pl.climbing && pl.grounded && std::fabs(pl.feet.y - 300.0f) < 0.5f && pl.feet.z > 3450.0f);
	CHECK(highest < 345.0f); // no leap over the edge
	// Walking about on the ledge next to the ladder's top is just walking.
	for (int i = 0; i < 10; ++i) {
		Input in;
		in.moveX = 1.0f;
		app.update(in, dt);
		CHECK(!pl.climbing && pl.grounded);
	}
	// Down: back onto the ladder, facing the ledge.
	app.teleport({-1200.0f, 475.0f, 3460.0f});
	for (int i = 0; i < 20; ++i) app.update(Input {}, dt);
	CHECK(pl.grounded && std::fabs(pl.feet.y - 300.0f) < 0.5f);
	climbed = false;
	float fastestDrop = 0, lastY = pl.feet.y;
	for (int i = 0; i < 300 && !(pl.grounded && pl.feet.y < 1.0f); ++i) {
		Input in;
		in.moveZ = -1.0f;
		app.update(in, dt);
		climbed = climbed || pl.climbing;
		fastestDrop = std::max(fastestDrop, lastY - pl.feet.y);
		lastY = pl.feet.y;
	}
	CHECK(climbed && pl.grounded && std::fabs(pl.feet.y) < 0.5f && pl.feet.z < 3400.0f && pl.feet.z > 3330.0f);
	CHECK(fastestDrop < 8.0f); // climbed down at 170 cm/s (5.7 cm a frame), not fallen
	// Jumping lets go of a ladder.
	for (int i = 0; i < 40; ++i) {
		Input in;
		in.moveZ = 1.0f;
		app.update(in, dt);
	}
	CHECK(pl.climbing && pl.feet.y > 100.0f);
	Input letGo;
	letGo.jump = true;
	app.update(letGo, dt);
	CHECK(!pl.climbing);
	for (int i = 0; i < 40; ++i) app.update(Input {}, dt);
	CHECK(pl.grounded && std::fabs(pl.feet.y) < 0.5f);

	// ---- the way back ----
	app.teleport({0, 175.0f, 900.0f});
	app.camera().yaw = 3.14159f;
	for (int i = 0; i < 20; ++i) app.update(Input {}, dt);
	for (frames = 0; frames < 120 && app.worldSwitches() == 1; ++frames) {
		Input in;
		in.moveZ = 1.0f;
		app.update(in, dt);
	}
	CHECK(app.worldSwitches() == 2 && std::string(app.worldName()) == "world/world");
	CHECK(std::fabs(app.camera().pos.x - gate[0]) < 1.0f && std::fabs(app.camera().pos.z - gate[2]) < 1.0f);

	// ---- the saved state brings the player back to the same spot, across worlds ----
	{
		const std::string statePath = gDir + "/state.bin";
		app.environment().timeOfDay = 17.25f;
		app.setDrawDistance(9000.0f);
		app.camera().pitch = -0.3f;
		const Vec3 left = app.camera().pos;
		const float leftYaw = app.camera().yaw;
		CHECK(app.saveState(statePath.c_str()));
		// Leave: another world, another place, another hour.
		CHECK(app.enterWorld(hashName("world/cave"), nullptr, 0));
		app.environment().timeOfDay = 3.0f;
		app.setDrawDistance(12000.0f);
		const u32 switches = app.worldSwitches();
		CHECK(app.loadState(statePath.c_str()));
		CHECK(std::string(app.worldName()) == "world/world" && app.worldSwitches() == switches);
		CHECK(std::fabs(app.camera().pos.x - left.x) < 0.01f && std::fabs(app.camera().pos.y - left.y) < 0.01f &&
		      std::fabs(app.camera().pos.z - left.z) < 0.01f);
		CHECK(std::fabs(app.camera().yaw - leftYaw) < 1e-5f && std::fabs(app.camera().pitch + 0.3f) < 1e-5f);
		CHECK(std::fabs(app.environment().timeOfDay - 17.25f) < 1e-4f && std::fabs(app.drawDistance() - 9000.0f) < 0.01f);
		for (int i = 0; i < 20; ++i) app.update(Input {}, dt);
		CHECK(pl.grounded && std::string(app.worldName()) == "world/world");

		// A damaged or missing file changes nothing.
		FILE* f = fopen(statePath.c_str(), "r+b");
		fseek(f, 20, SEEK_SET);
		fputc(0x5A, f);
		fclose(f);
		const Vec3 here = app.camera().pos;
		CHECK(!app.loadState(statePath.c_str()));
		CHECK(!app.loadState((gDir + "/no-such-state.bin").c_str()));
		CHECK(app.camera().pos.x == here.x && app.camera().pos.z == here.z);
		app.setDrawDistance(12000.0f);
		app.environment().timeOfDay = 13.0f;
	}

	// ---- the debug key walks through the worlds of the archive; bad targets are refused ----
	const u32 switchesBefore = app.worldSwitches();
	Input next;
	next.nextWorld = true;
	app.update(next, dt);
	CHECK(app.worldSwitches() == switchesBefore + 1 && std::string(app.worldName()) == "world/cave");
	app.update(next, dt);
	CHECK(app.worldSwitches() == switchesBefore + 2 && std::string(app.worldName()) == "world/world");
	CHECK(!app.enterWorld(hashName("world/nowhere"), nullptr, 0));
	CHECK(!app.enterWorld(hashName("tex/syn_rock"), nullptr, 0)); // exists, but is not a world
	CHECK(std::string(app.worldName()) == "world/world" && app.worldSwitches() == switchesBefore + 2);
	app.render();
	CHECK(w.stats().trianglesDrawn > 100 && w.stats().loadFailures == 0);

	app.shutdown();
	CHECK(gfx::memStats().meshCount == 0 && gfx::memStats().textureCount == 0);

	// ---- a world index that points outside its own tables is refused ----
	{
		WorldHeader wh {};
		wh.magic = WORLD_MAGIC;
		wh.version = WORLD_VERSION;
		wh.chunkSize = 4000;
		wh.materialCount = 1;
		wh.textureCount = 1;
		WorldMaterial wm {};
		wm.texture = 0;
		wm.frameCount = 2; // two frames, but the table has one texture
		const u32 tex = hashName("tex/none");
		std::vector<u8> blob(sizeof(wh) + sizeof(wm) + sizeof(tex));
		std::memcpy(blob.data(), &wh, sizeof(wh));
		std::memcpy(blob.data() + sizeof(wh), &wm, sizeof(wm));
		std::memcpy(blob.data() + sizeof(wh) + sizeof(wm), &tex, sizeof(tex));
		PakWriter bad;
		bad.add("world/bad", ASSET_WORLD, blob);
		wm.frameCount = 1;
		std::memcpy(blob.data() + sizeof(wh), &wm, sizeof(wm));
		bad.add("world/empty", ASSET_WORLD, blob);
		blob.pop_back();
		bad.add("world/short", ASSET_WORLD, blob);
		const std::string badPath = gDir + "/badworld.pak";
		bad.write(badPath);
		Pak pak;
		CHECK(pak.open(badPath.c_str()));
		World world;
		CHECK(!world.open(&pak, "bad") && !world.isOpen());
		CHECK(!world.open(&pak, "short"));
		CHECK(world.open(&pak, "empty") && world.isOpen()); // valid, just without chunks
		world.update({0, 0, 0});
		world.draw(identity(), {0, 0, 0}, 0);
		world.drawBlended();
		world.close();
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

// ---- background loading ----

// The loader against the plain function it wraps: every chunk and texture of
// a baked world, through the queue, with and without a thread.
static void testLoader() {
	SrcScene scene;
	SynthOptions so;
	so.cells = 48;
	so.huts = 8;
	so.trees = 60;
	buildSyntheticScene(so, scene);
	const std::string path = gDir + "/loader.pak";
	bakeScenePak(scene, path, "t", 3000.0f);

	Pak pak;
	CHECK(pak.open(path.c_str()));
	CHECK(std::string(pak.path()) == path);
	const PakEntry* we = pak.find("world/t");
	CHECK(we != nullptr);
	if (!we) return;
	std::vector<u8> index(we->size);
	CHECK(pak.read(*we, index.data()));
	WorldHeader wh;
	std::memcpy(&wh, index.data(), sizeof(wh));
	std::vector<WorldMaterial> materials(wh.materialCount);
	std::memcpy(materials.data(), index.data() + sizeof(wh), materials.size() * sizeof(WorldMaterial));
	std::vector<u32> textures(wh.textureCount);
	std::memcpy(textures.data(), index.data() + sizeof(wh) + materials.size() * sizeof(WorldMaterial), textures.size() * 4);
	std::vector<ChunkInfo> chunks(wh.chunkCount);
	std::memcpy(chunks.data(), index.data() + worldBlobSize(wh) - chunks.size() * sizeof(ChunkInfo),
	            chunks.size() * sizeof(ChunkInfo));

	std::vector<LoadJob> jobs;
	for (u32 i = 0; i < wh.chunkCount; ++i) {
		LoadJob j {};
		j.kind = LOAD_CHUNK;
		j.index = i;
		j.entry = *pak.find(chunks[i].entryHash);
		j.info = chunks[i];
		jobs.push_back(j);
	}
	for (u32 i = 0; i < wh.textureCount; ++i) {
		LoadJob j {};
		j.kind = LOAD_TEXTURE;
		j.index = i;
		j.entry = *pak.find(textures[i]);
		jobs.push_back(j);
	}
	CHECK(jobs.size() > 40);

	StreamLoader none;
	CHECK(!none.start((gDir + "/does-not-exist.pak").c_str(), materials.data(), wh.materialCount, true));
	CHECK(!none.running() && !none.submit(jobs[0]));

	for (int threaded = 0; threaded < 2; ++threaded) {
		StreamLoader loader;
		CHECK(loader.start(path.c_str(), materials.data(), wh.materialCount, threaded != 0));
		CHECK(loader.running() && loader.threaded() == (threaded != 0));
		size_t next = 0, done = 0, mismatches = 0, colliders = 0;
		u32 lastChunk = 0;
		bool inOrder = true;
		for (int spins = 0; done < jobs.size() && spins < 2000000; ++spins) {
			while (next < jobs.size() && loader.submit(jobs[next])) ++next;
			CHECK(loader.outstanding() <= StreamLoader::CAPACITY);
			LoadKind kind;
			if (!loader.peek(kind)) {
				if (threaded) usleep(50);
				continue;
			}
			LoadResult r;
			CHECK(loader.poll(r));
			CHECK(r.kind == kind && r.status == LOAD_OK && r.blob && r.size == jobs[done].entry.size);
			// Results come back in the order the jobs went in.
			inOrder = inOrder && r.index == jobs[done].index && r.kind == jobs[done].kind;
			LoadResult ref;
			StreamLoader::process(pak, jobs[done], materials.data(), wh.materialCount, ref);
			mismatches += ref.status != r.status || ref.size != r.size || std::memcmp(ref.blob, r.blob, r.size) != 0 ||
			              ref.collider.triangleCount != r.collider.triangleCount ||
			              ref.water.triangleCount != r.water.triangleCount || ref.collider.bytes() != r.collider.bytes();
			if (r.kind == LOAD_CHUNK) {
				colliders += r.collider.triangleCount > 0;
				lastChunk = r.index;
			} else {
				CHECK(r.collider.triangleCount == 0);
			}
			ref.discard();
			r.discard();
			CHECK(r.blob == nullptr);
			++done;
		}
		CHECK(done == jobs.size() && mismatches == 0 && inOrder && loader.outstanding() == 0);
		// (A few chunks hold nothing to stand on: only tree tops or water.)
		CHECK(colliders > wh.chunkCount / 2 && colliders <= wh.chunkCount && lastChunk == wh.chunkCount - 1);
		CHECK(loader.takeWorstJobMs() > 0.0f && loader.takeWorstJobMs() == 0.0f);

		// The queue is bounded ...
		u32 accepted = 0;
		for (const LoadJob& j : jobs) accepted += loader.submit(j);
		CHECK(accepted == StreamLoader::CAPACITY && loader.outstanding() == StreamLoader::CAPACITY);
		// ... and can be emptied in one go, whatever state the jobs are in.
		loader.cancelAll();
		LoadResult r;
		LoadKind kind;
		CHECK(loader.outstanding() == 0 && !loader.peek(kind) && !loader.poll(r));

		// Data that does not match what the world index promises is refused.
		LoadJob bad = jobs[0];
		bad.info.vertexCount = u16(bad.info.vertexCount + 1);
		LoadJob wrongType = jobs[0];
		wrongType.kind = LOAD_TEXTURE;
		LoadJob truncated = jobs[1];
		truncated.entry.packedSize /= 2;
		CHECK(loader.submit(bad) && loader.submit(wrongType) && loader.submit(truncated) && loader.submit(jobs[2]));
		int got = 0;
		for (int spins = 0; got < 4 && spins < 2000000; ++spins) {
			if (!loader.poll(r)) {
				if (threaded) usleep(50);
				continue;
			}
			CHECK(r.status == (got < 3 ? LOAD_BAD_DATA : LOAD_OK) && (got < 3 ? r.blob == nullptr : r.blob != nullptr));
			r.discard();
			++got;
		}
		CHECK(got == 4);

		// Stopping with work under way leaves nothing behind (the sanitizer
		// build checks that), and stopping twice is harmless.
		for (size_t i = 0; i < 6; ++i) loader.submit(jobs[i]);
		loader.stop();
		loader.stop();
		CHECK(!loader.running() && !loader.threaded() && !loader.submit(jobs[0]));
		// A stopped loader can be started again.
		CHECK(loader.start(path.c_str(), materials.data(), wh.materialCount, threaded != 0));
		CHECK(loader.submit(jobs[3]));
	} // destroyed with a job under way
}

// The world streamed in the background must end up exactly like the world
// loaded on the spot - same chunks, same memory, same picture - without ever
// doing more per frame than it is allowed to.
static void testStreaming() {
	const std::string path = gDir + "/loader.pak"; // baked by testLoader
	SrcScene scene;
	SynthOptions so;
	so.cells = 48;
	so.huts = 8;
	so.trees = 60;
	buildSyntheticScene(so, scene);
	const Vec3 spawn = {scene.spawnPos[0], scene.spawnPos[1], scene.spawnPos[2]};
	const float dt = 1.0f / 30.0f;

	struct Snapshot {
		std::vector<bool> resident;
		u32 bytes = 0, textures = 0, collision = 0;
		std::vector<u8> frame;
	};
	gfx::init();
	Snapshot shots[3];
	static const char* names[3] = {"on the frame", "background, no thread", "background, worker thread"};
	for (int mode = 0; mode < 3; ++mode) {
		const bool background = mode > 0, threaded = mode == 2;
		App app;
		app.world().config().background = background;
		app.world().config().worker = threaded;
		CHECK(app.init(path.c_str(), "t"));
		World& w = app.world();
		CHECK(w.backgroundStreaming() == background && w.workerThread() == threaded);
		app.setDrawDistance(6000.0f);
		app.player().flying = true;
		w.config().maxLoadsPerFrame = 1;
		w.config().maxTextureLoadsPerFrame = 2;

		auto distance = [&](u32 i) {
			const ChunkInfo& ci = w.chunkInfo(i);
			const Vec3 c = app.camera().pos;
			const float dx = std::max({ci.bbMin[0] - c.x, c.x - ci.bbMax[0], 0.0f});
			const float dy = std::max({ci.bbMin[1] - c.y, c.y - ci.bbMax[1], 0.0f});
			const float dz = std::max({ci.bbMin[2] - c.z, c.z - ci.bbMax[2], 0.0f});
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		};
		// Updates until nothing is missing any more; false if that never happens.
		auto settle = [&](int limit, bool watch) {
			std::vector<bool> had(w.stats().chunksTotal);
			for (u32 i = 0; i < w.stats().chunksTotal; ++i) had[i] = w.chunkResident(i);
			float lastArrival = 0;
			bool ordered = true;
			u32 texturesBefore = w.stats().texturesLoaded;
			for (int f = 0; f < limit; ++f) {
				app.update(Input {}, dt);
				const WorldStats& st = w.stats();
				if (watch) {
					CHECK(st.loadsThisFrame <= 1 && st.chunksInFlight <= 2);
					CHECK(st.texturesLoaded <= texturesBefore + 2 || !background);
					texturesBefore = st.texturesLoaded;
					for (u32 i = 0; i < st.chunksTotal; ++i) {
						if (had[i] || !w.chunkResident(i)) continue;
						had[i] = true;
						ordered = ordered && distance(i) >= lastArrival;
						lastArrival = distance(i);
					}
				}
				if (st.chunksPending == 0 && st.chunksInFlight == 0 && st.texturesWaiting == 0) {
					if (watch) CHECK(ordered); // nearest first, all the way
					return true;
				}
				if (threaded) usleep(100);
			}
			return false;
		};

		// 1. Standing at the start point until everything around is there.
		CHECK(w.stats().chunksLoaded == 0);
		CHECK(settle(200000, true));
		Snapshot& shot = shots[mode];
		for (u32 i = 0; i < w.stats().chunksTotal; ++i) shot.resident.push_back(w.chunkResident(i));
		shot.bytes = w.stats().residentBytes;
		shot.textures = w.stats().texturesLoaded;
		shot.collision = w.stats().collisionBytes;
		app.setClock(5.0); // the water moves: same moment in every mode
		app.render();
		shot.frame.assign(gfx::softFramebuffer(), gfx::softFramebuffer() + gfx::SCREEN_W * gfx::SCREEN_H * 4);
		const u32 near = w.stats().chunksLoaded;
		CHECK(near > 4 && near < w.stats().chunksTotal && w.stats().loadFailures == 0 && w.stats().budgetRejects == 0);
		const u32 skyBytes = gfx::memStats().meshBytes + gfx::memStats().textureBytes - w.stats().residentBytes;
		std::printf("  streaming %-26s %u chunks, %u textures, %.2f MB\n", names[mode], near, shot.textures,
		            double(shot.bytes) / 1048576.0);

		// 2. Leaving while chunks are on their way: what arrives late is thrown away.
		app.teleport({1e6f, 0, 1e6f});
		app.update(Input {}, dt);
		CHECK(w.stats().chunksLoaded == 0);
		app.teleport(spawn);
		app.update(Input {}, dt); // asks for the nearest chunks ...
		if (background) CHECK(w.stats().chunksInFlight > 0);
		app.teleport({1e6f, 0, 1e6f}); // ... and is gone before they arrive
		CHECK(settle(200000, false));
		CHECK(w.stats().chunksLoaded == 0 && w.stats().residentBytes == 0 && w.stats().texturesLoaded == 0 &&
		      w.stats().collisionBytes == 0);
		CHECK(gfx::memStats().meshCount == 1 && gfx::memStats().textureCount == 0); // the sky dome

		// 3. A loading screen in the middle of streaming: everything at once,
		// and nothing twice afterwards.
		app.teleport(spawn);
		app.update(Input {}, dt);
		app.update(Input {}, dt);
		w.preload(app.camera().pos);
		CHECK(w.stats().chunksLoaded == near && w.stats().chunksPending == 0 && w.stats().texturesWaiting == 0);
		CHECK(w.stats().residentBytes == shot.bytes && w.stats().texturesLoaded == shot.textures);
		for (int f = 0; f < 30; ++f) {
			app.update(Input {}, dt);
			if (threaded) usleep(100);
		}
		CHECK(w.stats().chunksLoaded == near && w.stats().chunksInFlight == 0 && w.stats().residentBytes == shot.bytes);
		CHECK(w.stats().residentBytes == gfx::memStats().meshBytes + gfx::memStats().textureBytes - skyBytes);
		CHECK(w.stats().collisionBytes == shot.collision);

		// 4. Video memory runs out while streaming: chunks wait, nothing is
		// written off, and they arrive once there is room.
		app.teleport({1e6f, 0, 1e6f});
		CHECK(settle(200000, false) && w.stats().chunksLoaded == 0);
		app.teleport(spawn);
		gfx::softSetMemoryLimit(gfx::memStats().meshBytes + 40 * 1024);
		for (int f = 0; f < 400; ++f) {
			app.update(Input {}, dt);
			if (threaded) usleep(100);
		}
		const u32 starved = w.stats().chunksLoaded;
		CHECK(starved > 0 && starved < near && w.stats().budgetRejects > 0 && w.stats().loadFailures == 0);
		app.render(); // whatever is there can be drawn
		gfx::softSetMemoryLimit(0);
		CHECK(settle(200000, false));
		CHECK(w.stats().chunksLoaded == near && w.stats().loadFailures == 0 && w.stats().residentBytes == shot.bytes &&
		      w.stats().texturesLoaded == shot.textures);

		// 5. The budget is respected with chunks on their way, too.
		app.teleport({1e6f, 0, 1e6f});
		CHECK(settle(200000, false));
		w.config().memoryBudget = shot.bytes / 2;
		app.teleport(spawn);
		u32 peak = 0;
		for (int f = 0; f < 300; ++f) {
			app.update(Input {}, dt);
			peak = std::max(peak, w.stats().residentBytes);
			if (threaded) usleep(100);
		}
		// Chunks are admitted by their mesh size; their textures come on top.
		CHECK(w.stats().chunksLoaded > 0 && w.stats().chunksLoaded < near && w.stats().chunksPending > 0);
		CHECK(peak <= shot.bytes / 2 + 256 * 1024);
		w.config().memoryBudget = 24u << 20;

		// 6. Closed with work under way.
		app.teleport({1e6f, 0, 1e6f});
		app.update(Input {}, dt);
		app.teleport(spawn);
		app.update(Input {}, dt);
		app.shutdown();
		CHECK(gfx::memStats().meshCount == 0 && gfx::memStats().textureCount == 0);
	}
	gfx::shutdown();
	for (int mode = 1; mode < 3; ++mode) {
		CHECK(shots[mode].resident == shots[0].resident);
		CHECK(shots[mode].bytes == shots[0].bytes && shots[mode].textures == shots[0].textures &&
		      shots[mode].collision == shots[0].collision);
		CHECK(shots[mode].frame == shots[0].frame); // the very same picture
	}
}

// Walking through a portal with background streaming on: the old world's
// loader is stopped, the new world gets one of its own.
static void testStreamingWorlds() {
	const std::string path = gDir + "/worlds.pak"; // baked by testWorlds
	gfx::init();
	for (int threaded = 0; threaded < 2; ++threaded) {
		App app;
		app.world().config().background = true;
		app.world().config().worker = threaded != 0;
		CHECK(app.init(path.c_str(), nullptr));
		World& w = app.world();
		w.preload(app.camera().pos);
		const u32 worldHash = w.entryHash();
		app.player().flying = true;
		for (int hop = 0; hop < 6; ++hop) {
			Input in;
			in.nextWorld = true; // the debug key: next world of the archive
			app.update(in, 1.0f / 30.0f);
			CHECK(w.isOpen() && w.backgroundStreaming() && w.workerThread() == (threaded != 0));
			CHECK((w.entryHash() == worldHash) == (hop % 2 == 1));
			CHECK(w.stats().chunksLoaded > 0 && w.stats().chunksPending == 0); // preloaded on arrival
			// Fly off so that the next switch happens with chunks under way.
			Input fly;
			fly.moveZ = 1.0f;
			fly.fast = true;
			for (int f = 0; f < 20; ++f) app.update(fly, 1.0f / 30.0f);
			app.render();
		}
		CHECK(app.worldSwitches() == 6 && w.stats().loadFailures == 0);
		app.shutdown();
		CHECK(gfx::memStats().meshCount == 0 && gfx::memStats().textureCount == 0);
	}
	gfx::shutdown();
}

// ---- frame pacing ----

static void testPacer() {
	// A stand-in for the console: objects are 40% of a frame's cost.
	auto cost = [](float scene, float detail) { return scene * (0.6f + 0.4f * detail); };

	// Plenty of room: nothing changes.
	FramePacer p;
	for (int f = 0; f < 600; ++f) p.update(cost(0.7f, p.detailScale()));
	CHECK(p.detailScale() == 1.0f && p.interval() == 1);

	// A view that needs 105% at full detail. The pacer finds a detail level
	// that fits and stays at 60 fps, without hunting.
	float worst = 0, lowest = 1, highest = 0;
	int changes = 0, late = 0;
	for (int f = 0; f < 1200; ++f) {
		const float load = cost(1.05f, p.detailScale());
		const int was = p.interval();
		p.update(load);
		changes += p.interval() != was;
		if (f >= 300) {
			worst = std::max(worst, load);
			late += load > 1.0f;
			lowest = std::min(lowest, p.detailScale());
			highest = std::max(highest, p.detailScale());
		}
	}
	std::printf("  pacer at 105%%: detail settles at %.0f..%.0f%%, load <= %.0f%%\n", double(lowest * 100.0f),
	            double(highest * 100.0f), double(worst * 100.0f));
	CHECK(changes == 0 && p.interval() == 1 && late == 0);
	CHECK(worst <= 0.92f && lowest > 0.40f && highest < 0.75f && highest - lowest < 0.15f);

	// The first frames of the overload are the only late ones.
	p.reset();
	late = 0;
	for (int f = 0; f < 300; ++f) {
		const float load = cost(1.05f, p.detailScale());
		late += load > 1.0f;
		p.update(load);
	}
	CHECK(late >= 1 && late <= 4);

	// A view that does not fit even without objects: one switch to a steady
	// 30 fps, not a flicker between the two - and at 30 fps, with twice the
	// time, the objects come back.
	p.reset();
	changes = 0;
	for (int f = 0; f < 900; ++f) {
		const int was = p.interval();
		p.update(cost(1.5f, p.detailScale()));
		changes += p.interval() != was;
	}
	CHECK(p.interval() == 2 && changes == 1 && p.detailScale() > 0.99f && !p.probing());
	// It is too heavy to even ask whether 60 fps would do: the objects stay.
	lowest = 1.0f;
	for (int f = 0; f < 3000; ++f) {
		p.update(cost(1.5f, p.detailScale()));
		lowest = std::min(lowest, p.detailScale());
	}
	CHECK(p.interval() == 2 && lowest > 0.99f);

	// The view gets lighter, but not light: 60 fps would fit with fewer
	// objects. The pacer finds out by taking them away for a moment, and
	// switches.
	changes = 0;
	int probes = 0;
	bool wasProbing = false;
	for (int f = 0; f < 600 && p.interval() == 2; ++f) {
		p.update(cost(1.2f, p.detailScale())); // 120% with everything, 89% with the minimum
		probes += p.probing() && !wasProbing;
		wasProbing = p.probing();
	}
	CHECK(p.interval() == 1 && probes == 1 && p.detailScale() < 0.5f);
	late = 0;
	for (int f = 0; f < 900; ++f) {
		const float load = cost(1.2f, p.detailScale());
		late += load > 1.0f;
		const int was = p.interval();
		p.update(load);
		changes += p.interval() != was;
	}
	CHECK(p.interval() == 1 && changes == 0 && late == 0); // and stays there

	// A view where the answer is no (96% even with the minimum): the objects
	// dip for a moment each time it asks, it asks less and less often, and it
	// never leaves 30 fps.
	// (Reached from a heavier view; coming from a light one it would simply
	// stay at 60.)
	p.reset();
	for (int f = 0; f < 300; ++f) p.update(cost(1.5f, p.detailScale()));
	CHECK(p.interval() == 2);
	probes = 0;
	wasProbing = false;
	int firstHalf = 0;
	bool stayed = true;
	for (int f = 0; f < 6000; ++f) {
		p.update(cost(1.297f, p.detailScale()));
		probes += p.probing() && !wasProbing;
		wasProbing = p.probing();
		if (f == 2999) firstHalf = probes;
		stayed = stayed && p.interval() == 2;
	}
	CHECK(stayed);
	std::printf("  pacer at 30 fps, borderline view: asked %d times in 200 s (%d of them in the first 100 s)\n", probes,
	            firstHalf);
	CHECK(probes >= 3 && probes <= 6 && firstHalf >= probes - firstHalf);

	// A clearly light view needs no check: straight back to 60, objects and all.
	changes = 0;
	for (int f = 0; f < 900; ++f) {
		const int was = p.interval();
		p.update(cost(0.75f, p.detailScale()));
		changes += p.interval() != was;
	}
	CHECK(p.interval() == 1 && changes == 1 && p.detailScale() > 0.95f);

	// Nonsense input does no harm.
	p.reset();
	p.update(std::nanf(""));
	p.update(-5.0f);
	p.update(1.0e9f);
	CHECK(p.detailScale() >= p.cfg.minDetail && p.detailScale() <= 1.0f && p.load() <= 4.0f);
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

// ---- characters ----

// The rest-pose vertices of a source model, posed by hand: the reference the
// runtime's skinning is compared with.
static Vec3 posedVertex(const SrcSkinVertex& v, const std::vector<Mat34>& skin) {
	const Vec3 p = {v.pos[0], v.pos[1], v.pos[2]};
	const Vec3 a = transformPoint(skin[size_t(v.bone[0])], p), b = transformPoint(skin[size_t(v.bone[1])], p);
	return a + (b - a) * v.weight1;
}

static void testModel() {
	SrcModel chain, actor;
	buildChainModel(chain, 60);
	buildSyntheticActor(actor);
	BakeOptions bo;
	bo.tex.maxSize = 32;
	bo.tex.quality = 0;
	bo.quiet = true;
	const std::string path = gDir + "/models.pak";
	{
		PakWriter writer;
		const ModelBakeResult r = bakeModel(chain, bo, writer);
		// 59 double-sided quads; 60 bones do not fit one palette.
		CHECK(r.triangles == 59 * 4 && r.parts >= 3 && r.largestPalette == MODEL_PALETTE && r.clips == 2 && r.textures == 1);
		CHECK(r.report.find("skeleton: 60 nodes") != std::string::npos);
		const ModelBakeResult a = bakeModel(actor, bo, writer);
		CHECK(a.parts == 2 && a.clips == 8 && a.largestPalette <= MODEL_PALETTE);
		writer.write(path);
	}
	// What cannot be baked is refused with a reason.
	{
		PakWriter writer;
		SrcModel bad = chain;
		bad.nodes[3].parent = 7;
		bad.nodes[7].parent = 3;
		bool threw = false;
		try {
			bakeModel(bad, bo, writer);
		} catch (const std::runtime_error& e) {
			threw = std::string(e.what()).find("cycle") != std::string::npos;
		}
		CHECK(threw);
		bad = chain;
		bad.parts[0].verts[5].bone[1] = 60;
		threw = false;
		try {
			bakeModel(bad, bo, writer);
		} catch (const std::runtime_error&) {
			threw = true;
		}
		CHECK(threw);
		bad = chain;
		bad.clips[0].samples.pop_back();
		threw = false;
		try {
			bakeModel(bad, bo, writer);
		} catch (const std::runtime_error&) {
			threw = true;
		}
		CHECK(threw);
	}

	// Travel and stance. ZenGin flags standing animations as "moving" too;
	// only where the root ends up elsewhere is there travel to take out.
	{
		SrcModel m = chain;
		m.name = "walker";
		m.clips.clear();
		SrcClip crawl, stay;
		crawl.name = crawl.next = "S_CRAWL";
		stay.name = stay.next = "S_STAY";
		crawl.fps = stay.fps = 10.0f;
		crawl.moves = stay.moves = true;
		crawl.nodes = stay.nodes = {0};
		for (int f = 0; f < 5; ++f) {
			SrcClipSample s {};
			float rest[3];
			chainRest(60, 0, rest, s.rot);
			s.pos[0] = 12.0f + 3.0f * float(f % 2); // a stance off centre, swaying
			s.pos[1] = 10.0f;
			s.pos[2] = 50.0f * float(f);
			crawl.samples.push_back(s);
			s.pos[2] = -1.0f + 0.4f * float(f % 2);
			stay.samples.push_back(s);
		}
		m.clips = {crawl, stay};
		PakWriter writer;
		const ModelBakeResult r = bakeModel(m, bo, writer);
		CHECK(r.report.find("S_CRAWL 0.50s loop 500cm/s") != std::string::npos);
		CHECK(r.report.find("S_STAY 0.50s loop,") != std::string::npos || r.report.find("S_STAY 0.50s loop\n") != std::string::npos);
		const std::string walkerPath = gDir + "/walker.pak";
		writer.write(walkerPath);
		Pak wp;
		CHECK(wp.open(walkerPath.c_str()));
		Clip c, st;
		CHECK(c.load(wp, hashName("anim/walker/s_crawl"), 60) && st.load(wp, hashName("anim/walker/s_stay"), 60));
		CHECK((c.header().flags & CLIPF_MOVES) && std::fabs(c.header().moveSpeed - 500.0f) < 1.0f);
		CHECK(!(st.header().flags & CLIPF_MOVES) && st.header().moveSpeed == 0.0f);
		Pose p;
		for (int f = 0; f < 5; ++f) {
			c.sample(float(f) * 0.1f, p);
			CHECK(std::fabs(p.pos[0].x - (12.0f + 3.0f * float(f % 2))) < 0.01f && std::fabs(p.pos[0].z) < 0.01f);
			st.sample(float(f) * 0.1f, p);
			CHECK(std::fabs(p.pos[0].x - (12.0f + 3.0f * float(f % 2))) < 0.01f && std::fabs(p.pos[0].z - (-1.0f + 0.4f * float(f % 2))) < 0.01f);
		}
	}

	gfx::init();
	Pak pak;
	CHECK(pak.open(path.c_str()));
	const PakEntry* me = pak.find("model/chain");
	const PakEntry* ce = pak.find("anim/chain/s_bend");
	CHECK(me && me->type == ASSET_MODEL && ce && ce->type == ASSET_CLIP && pak.find("tex/chain_skin"));
	{
		Model m;
		CHECK(!m.load(pak, "nobody") && !m.loaded());
		CHECK(m.load(pak, "chain") && m.loaded());
		CHECK(m.nodeCount() == 60 && m.triangles() == 59 * 4 && m.clipCount() == 2 && m.textureBytes() > 0);
		CHECK(m.findNode(hashName("LINK17")) == 17 && m.findNode(hashName("link59")) == 59 && m.findNode(hashName("NOPE")) < 0);
		u32 indexSum = 0;
		for (u32 i = 0; i < m.partCount(); ++i) {
			CHECK(m.part(i).boneCount > 0 && m.part(i).boneCount <= MODEL_PALETTE);
			indexSum += m.part(i).indexCount;
		}
		CHECK(indexSum == 59 * 4 * 3);

		// At rest every bone's skin matrix is the identity: the mesh is drawn as stored.
		Pose rest;
		m.restPose(rest);
		Mat34 world[MODEL_MAX_NODES];
		m.worldTransforms(rest, world);
		float worst = 0;
		for (u32 i = 0; i < m.nodeCount(); ++i) {
			Mat34 inv;
			std::memcpy(inv.m, m.node(i).invBind, sizeof(inv.m));
			const Mat34 skin = mul(world[i], inv), id = mat34Identity();
			for (int r = 0; r < 3; ++r)
				for (int c = 0; c < 4; ++c) worst = std::max(worst, std::fabs(skin.m[r][c] - id.m[r][c]));
		}
		std::printf("  rest pose: skin matrices within %.2g of the identity\n", double(worst));
		CHECK(worst < 1e-3f); // sixty transforms deep

		// A clip that is played once: sampled between its frames, held at its ends.
		const Clip* bend = m.clip("S_BEND");
		const Clip* nod = m.clip("t_nod");
		CHECK(bend && nod && !m.clip("S_RUN"));
		if (bend && nod) {
			CHECK(bend->header().frameCount == 5 && bend->header().trackCount == 60 && !bend->loops());
			CHECK(std::fabs(bend->duration() - 0.4f) < 1e-5f && bend->header().next == 0 && bend->header().moveSpeed == 0.0f);
			CHECK(nod->header().trackCount == 1 && nod->header().next == hashName("S_BEND") && std::fabs(nod->duration() - 0.1f) < 1e-5f);
			Pose p = rest;
			bend->sample(0.0f, p);
			CHECK(std::fabs(p.pos[0].y - 10.0f) < 0.01f);
			bend->sample(0.05f, p); // half way from frame 0 to frame 1
			CHECK(std::fabs(p.pos[0].y - 10.5f) < 0.01f);
			bend->sample(0.2f, p);
			CHECK(std::fabs(p.pos[0].y - 12.0f) < 0.01f);
			// Link 5 at frame 2: its rest rotation, then 0.04 rad about its own X.
			float restPos[3], restRot[4];
			chainRest(60, 5, restPos, restRot);
			const Quat want = quatMul({restRot[0], restRot[1], restRot[2], restRot[3]}, {std::sin(0.02f), 0, 0, std::cos(0.02f)});
			const float d = std::fabs(want.x * p.rot[5].x + want.y * p.rot[5].y + want.z * p.rot[5].z + want.w * p.rot[5].w);
			CHECK(d > 0.999999f);
			CHECK(std::fabs(p.pos[5].y - 7.0f) < 0.01f);
			bend->sample(99.0f, p);
			CHECK(std::fabs(p.pos[0].y - 14.0f) < 0.01f);
			bend->sample(-3.0f, p);
			CHECK(std::fabs(p.pos[0].y - 10.0f) < 0.01f);
			// A clip only moves the nodes it has tracks for.
			Pose q = rest;
			q.pos[3] = {1, 2, 3};
			nod->sample(0.05f, q);
			CHECK(q.pos[3].x == 1 && q.pos[3].y == 2 && q.pos[3].z == 3 && std::fabs(q.rot[59].y - std::sin(0.15f)) < 1e-3f);
		}

		// The animator: T_NOD runs out and hands over to S_BEND, which runs out
		// and stays on its last frame.
		Animator an;
		an.bind(&m);
		CHECK(an.current() == nullptr && !an.finished() && std::fabs(an.pose().pos[0].y - 10.0f) < 1e-4f);
		an.play(nod);
		an.update(0.05f);
		CHECK(an.current() == nod && std::fabs(an.time() - 0.05f) < 1e-6f);
		an.play(nod); // already playing: not restarted
		CHECK(std::fabs(an.time() - 0.05f) < 1e-6f);
		const Quat before = an.pose().rot[59];
		an.update(0.1f);
		CHECK(an.current() == bend && std::fabs(an.time() - 0.05f) < 1e-5f && !an.finished());
		// The hand-over fades (T_NOD's blend-out is 0.25 s): no jump in the pose.
		const Quat after = an.pose().rot[59];
		CHECK(std::fabs(before.x * after.x + before.y * after.y + before.z * after.z + before.w * after.w) > 0.995f);
		for (int i = 0; i < 20; ++i) an.update(0.05f);
		CHECK(an.finished() && an.current() == bend && std::fabs(an.time() - bend->duration()) < 1e-6f);
		CHECK(std::fabs(an.pose().pos[0].y - 14.0f) < 0.01f);
		an.play(nullptr, 0.0f); // back to rest
		an.update(0.01f);
		CHECK(an.current() == nullptr && std::fabs(an.pose().pos[0].y - 10.0f) < 1e-4f);

		// Drawn where the source mesh is - at rest and curled up - seen from
		// the side, standing somewhere in the world and turned.
		for (int posed = 0; posed < 2; ++posed) {
			Pose pose = rest;
			if (posed && bend) bend->sample(0.4f, pose);
			m.worldTransforms(pose, world);
			std::vector<Mat34> skin(60);
			for (u32 i = 0; i < 60; ++i) {
				Mat34 inv;
				std::memcpy(inv.m, m.node(i).invBind, sizeof(inv.m));
				skin[i] = mul(world[i], inv);
			}
			const Vec3 at = {300.0f, -50.0f, 700.0f};
			const float yaw = 0.6f, light[3] = {1.0f, 0.8f, 0.6f};
			gfx::FrameDesc fd {};
			const Vec3 eye = {300.0f, 180.0f, -100.0f};
			fd.view = viewLH(eye, 0.0f, 0.0f);
			fd.eye = eye;
			fd.fovY = 1.0f;
			fd.zNear = 20.0f;
			fd.zFar = 5000.0f;
			fd.fogStart = 4000.0f;
			fd.fogEnd = 5000.0f;
			fd.clearColor[0] = 1;
			fd.clearColor[1] = 2;
			fd.clearColor[2] = 3;
			fd.dayLight[0] = fd.dayLight[1] = fd.dayLight[2] = 1.0f;
			gfx::frameBegin(fd);
			gfx::setPass(gfx::PASS_WORLD);
			m.draw(pose, at, yaw, light);
			gfx::frameEnd();
			const gfx::SoftStats ss = gfx::softStats();
			// The strip has both sides: one of each pair faces the camera.
			CHECK(ss.skinnedTriangles == 59 * 4 && ss.skinnedBackface == 59 * 2 && ss.skinnedPixels > 100);
			// Where the pixels are ...
			int x0 = 9999, x1 = -1, y0 = 9999, y1 = -1;
			const u8* fb = gfx::softFramebuffer();
			for (int y = 0; y < gfx::SCREEN_H; ++y)
				for (int x = 0; x < gfx::SCREEN_W; ++x) {
					const u8* px = fb + (y * gfx::SCREEN_W + x) * 4;
					if (px[0] == 1 && px[1] == 2 && px[2] == 3) continue;
					x0 = std::min(x0, x);
					x1 = std::max(x1, x);
					y0 = std::min(y0, y);
					y1 = std::max(y1, y);
				}
			// ... and where the source vertices land.
			const Mat4 vp = mul(perspectiveLH(fd.fovY, float(gfx::SCREEN_W) / float(gfx::SCREEN_H), fd.zNear, fd.zFar), fd.view);
			const Mat34 turn = mat34From(quatFromYaw(yaw), at);
			float wx0 = 1e9f, wx1 = -1e9f, wy0 = 1e9f, wy1 = -1e9f;
			for (const SrcSkinVertex& v : chain.parts[0].verts) {
				const Vec3 w = transformPoint(turn, posedVertex(v, skin));
				const Vec4 c = transform(vp, {w.x, w.y, w.z, 1.0f});
				const float sx = (c.x / c.w * 0.5f + 0.5f) * gfx::SCREEN_W, sy = (0.5f - c.y / c.w * 0.5f) * gfx::SCREEN_H;
				wx0 = std::min(wx0, sx);
				wx1 = std::max(wx1, sx);
				wy0 = std::min(wy0, sy);
				wy1 = std::max(wy1, sy);
			}
			std::printf("  chain %s: %u triangles, %u back, %u pixels; drawn %d..%d x %d..%d, expected %.1f..%.1f x %.1f..%.1f\n", posed ? "curled" : "at rest",
			            ss.skinnedTriangles, ss.skinnedBackface, ss.skinnedPixels, x0, x1, y0, y1, double(wx0), double(wx1), double(wy0), double(wy1));
			CHECK(wy1 - wy0 > (posed ? 35.0f : 80.0f)); // a tall thing on the screen, shorter once curled up
			CHECK(std::fabs(float(x0) - wx0) < 1.6f && std::fabs(float(x1) - wx1) < 1.6f);
			CHECK(std::fabs(float(y0) - wy0) < 1.6f && std::fabs(float(y1) - wy1) < 1.6f);
		}
		m.release();
		CHECK(!m.loaded() && gfx::memStats().meshCount == 0 && gfx::memStats().textureCount == 0);
	}
	{
		// The test character: loops wrap, moving clips know their speed.
		Model m;
		CHECK(m.load(pak, "hero") && m.nodeCount() == 14 && m.clipCount() == 8);
		const Clip* run = m.clip("S_RUNL");
		const Clip* walk = m.clip("S_WALKL");
		const Clip* stand = m.clip("S_RUN");
		const Clip* takeOff = m.clip("T_STAND_2_JUMP");
		CHECK(run && walk && stand && takeOff);
		if (run && walk && stand && takeOff) {
			CHECK(run->loops() && (run->header().flags & CLIPF_MOVES) && std::fabs(run->header().moveSpeed - 480.0f) < 2.0f);
			CHECK(std::fabs(walk->header().moveSpeed - 150.0f) < 2.0f);
			CHECK(stand->loops() && !(stand->header().flags & CLIPF_MOVES) && stand->header().moveSpeed == 0.0f);
			CHECK(!takeOff->loops() && takeOff->header().next == hashName("S_JUMP"));
			// The root does not travel: the game moves the character.
			Pose a, b, c;
			m.restPose(a);
			b = c = a;
			const float d = run->duration();
			run->sample(0.3f * d, a);
			run->sample(1.3f * d, b);
			run->sample(-0.7f * d, c);
			float wrap = 0;
			for (u32 i = 0; i < m.nodeCount(); ++i)
				wrap = std::max({wrap, std::fabs(a.rot[i].x - b.rot[i].x), std::fabs(a.rot[i].w - b.rot[i].w), std::fabs(a.rot[i].x - c.rot[i].x),
				                 std::fabs(a.pos[i].y - b.pos[i].y)});
			CHECK(wrap < 1e-3f);
			float travel = 0;
			for (int f = 0; f < 16; ++f) {
				run->sample(d * float(f) / 16.0f, a);
				travel = std::max({travel, std::fabs(a.pos[0].x), std::fabs(a.pos[0].z)});
			}
			CHECK(travel < 0.01f && a.pos[0].y > 80.0f);
			// The end of a loop leads back into its start: no jump at the seam.
			run->sample(d - 1e-4f, a);
			run->sample(0.0f, b);
			float seam = 0;
			for (u32 i = 0; i < m.nodeCount(); ++i) seam = std::max(seam, std::fabs(a.rot[i].x - b.rot[i].x));
			CHECK(seam < 0.01f);
		}
	}
	pak.close();

	// Damaged data is turned down, never trusted.
	{
		Pak src;
		CHECK(src.open(path.c_str()));
		auto blob = [&](const char* name) {
			const PakEntry* e = src.find(name);
			std::vector<u8> out(e ? e->size : 0);
			if (e) CHECK(src.read(*e, out.data()));
			return out;
		};
		const std::vector<u8> model = blob("model/chain"), clip = blob("anim/chain/s_bend"), nod = blob("anim/chain/t_nod");
		ModelHeader mh;
		std::memcpy(&mh, model.data(), sizeof(mh));
		const size_t partsAt = sizeof(ModelHeader) + mh.nodeCount * sizeof(ModelNode);
		const size_t verticesAt = partsAt + mh.partCount * sizeof(ModelPart) + mh.clipCount * sizeof(ModelClipRef);
		const size_t indicesAt = verticesAt + mh.vertexCount * sizeof(WorldVertex);
		struct Damage {
			const char* what;
			size_t at;
			u8 value;
			bool truncate;
		};
		const Damage damages[] = {
		    {"magic", 0, 'X', false},
		    {"version", 4, 9, false},
		    {"node count", 8, 61, false},
		    {"truncated", 0, 0, true},
		    {"parent after child", sizeof(ModelHeader) + 5 * sizeof(ModelNode), 40, false},
		    {"palette size", partsAt + 6, 27, false},
		    {"palette bone", partsAt + 16, 200, false},
		    {"index range", partsAt + 12 + 3, 0x7F, false}, // top byte of indexCount
		    {"vertex slot beyond the palette", verticesAt + 12, 78, false},
		    {"vertex slot between rows", verticesAt + 13, 4, false},
		    {"index beyond the vertices", indicesAt + 1, 0xFF, false},
		};
		PakWriter writer;
		writer.add("model/good", ASSET_MODEL, model);
		int n = 0;
		for (const Damage& d : damages) {
			std::vector<u8> bad = model;
			if (d.truncate)
				bad.resize(bad.size() - 4);
			else
				bad[d.at] = d.value;
			writer.add("model/bad" + std::to_string(n++), ASSET_MODEL, bad);
		}
		// Clips: a good one, one for a longer skeleton, one cut short.
		ModelClipRef refs[2];
		std::memcpy(refs, model.data() + partsAt + mh.partCount * sizeof(ModelPart), sizeof(refs));
		std::vector<u8> badClip = clip;
		badClip[sizeof(ClipHeader) + 7] = 60; // a track for node 60 of 60
		writer.add("anim/chain/s_bend", ASSET_CLIP, badClip);
		std::vector<u8> shortClip = nod;
		shortClip.resize(shortClip.size() - 14);
		writer.add("anim/chain/t_nod", ASSET_CLIP, shortClip);
		const std::string badPath = gDir + "/badmodels.pak";
		writer.write(badPath);
		src.close();

		Pak bad;
		CHECK(bad.open(badPath.c_str()));
		Model m;
		for (int i = 0; i < n; ++i) {
			const bool loaded = m.load(bad, ("bad" + std::to_string(i)).c_str());
			if (loaded) std::printf("  damaged model accepted: %s\n", damages[i].what);
			CHECK(!loaded && !m.loaded());
		}
		CHECK(gfx::memStats().meshCount == 0 && gfx::memStats().textureCount == 0);
		// The model itself is fine; its broken clips are simply not there, and
		// its missing texture leaves it untextured.
		CHECK(m.load(bad, "good") && m.clipCount() == 2 && !m.clip("S_BEND") && !m.clip("T_NOD") && m.textureBytes() == 0);
		Pose rest;
		m.restPose(rest);
		gfx::FrameDesc fd {};
		fd.view = viewLH({0, 200, -600}, 0, 0);
		fd.eye = {0, 200, -600};
		fd.fovY = 1.0f;
		fd.zNear = 20.0f;
		fd.zFar = 5000.0f;
		fd.fogEnd = 5000.0f;
		const float light[3] = {1, 1, 1};
		gfx::frameBegin(fd);
		gfx::setPass(gfx::PASS_WORLD);
		m.draw(rest, {0, 0, 0}, 0, light);
		gfx::frameEnd();
		CHECK(gfx::softStats().skinnedPixels > 300);
		m.release();
		bad.close();
	}
	gfx::shutdown();
}

// The player's character in the game: what it does follows what the player
// does, and the camera stays behind it and out of the walls.
static void testHero() {
	std::vector<BakeWorld> worlds(2);
	worlds[0].name = "yard";
	worlds[0].level = "YARD.ZEN";
	SrcScene& scene = worlds[0].scene;
	scene.materials.resize(1);
	scene.materials[0].name = "solid";
	// A floor of 80 x 80 m: brightly lit where x < 0, dim where x > 0.
	for (int z = -8; z < 8; ++z)
		for (int x = -8; x < 8; ++x) {
			const float x0 = x * 500.0f, z0 = z * 500.0f;
			const float p[4][3] = {{x0, 0, z0}, {x0 + 500, 0, z0}, {x0 + 500, 0, z0 + 500}, {x0, 0, z0 + 500}};
			addQuad(scene, p[0], p[1], p[2], p[3], 0);
			for (size_t t = scene.triangles.size() - 2; t < scene.triangles.size(); ++t)
				for (SrcVertex& v : scene.triangles[t].v) v.rgba[0] = v.rgba[1] = v.rgba[2] = x < 0 ? 250 : 60;
		}
	// A wall along x = 1000..1040, six metres high, and a 30 cm step.
	addBlock(scene, 1000, 1040, -3000, 3000, 600);
	addBlock(scene, -600, 600, 1500, 2000, 30);
	scene.spawnPos[0] = 0;
	scene.spawnPos[1] = 175;
	scene.spawnPos[2] = 0;
	scene.hasSpawn = true;
	worlds[1].name = "cave";
	worlds[1].level = "CAVE.ZEN";
	buildSyntheticCave(worlds[1].scene);

	std::vector<SrcModel> models(1);
	buildSyntheticActor(models[0]);
	// A run of 520 cm/s instead of the controller's own 480: the player must take it over.
	for (SrcClip& clip : models[0].clips)
		if (clip.name == "S_RUNL")
			for (size_t i = 0; i < clip.samples.size(); i += clip.nodes.size()) clip.samples[i].pos[2] *= 520.0f / 480.0f;
	BakeOptions bo;
	bo.chunker.chunkSize = 2000.0f;
	bo.tex.maxSize = 32;
	bo.tex.quality = 0;
	bo.threads = 2;
	bo.quiet = true;
	const std::string path = gDir + "/hero.pak";
	{
		PakWriter writer;
		const BakeResult r = bakeWorlds(worlds, models, bo, writer);
		CHECK(r.models == 1 && r.report.find("== model 'hero' ==") != std::string::npos);
		writer.write(path);
	}

	// A character that cannot be baked does not take the worlds down with it.
	{
		std::vector<SrcModel> broken(1);
		buildChainModel(broken[0], 97); // more nodes than the format holds
		std::vector<BakeWorld> one(1);
		one[0] = worlds[0];
		PakWriter writer;
		const BakeResult r = bakeWorlds(one, broken, bo, writer);
		CHECK(r.models == 0 && r.report.find("WARNING: not baked: model 'chain': 97 nodes") != std::string::npos);
		CHECK(r.report.find("the game stays first-person") != std::string::npos);
		const std::string brokenPath = gDir + "/brokenhero.pak";
		writer.write(brokenPath);
		Pak p;
		CHECK(p.open(brokenPath.c_str()) && !p.find("meta/hero") && !p.find("model/chain") && p.find("meta/start"));
	}

	gfx::init();
	App app;
	CHECK(app.init(path.c_str(), nullptr));
	app.world().preload(app.camera().pos);
	const Hero& hero = app.hero();
	Player& pl = app.player();
	CHECK(hero.loaded() && app.thirdPerson());
	for (int r = 0; r < Hero::ROLE_COUNT; ++r) CHECK(hero.hasRole(Hero::Role(r)));
	CHECK(std::fabs(pl.config.walkSpeed - 520.0f) < 3.0f && std::fabs(hero.roleSpeed(Hero::RUN) - pl.config.walkSpeed) < 1e-3f);
	CHECK(std::fabs(pl.config.swimSpeed - 260.0f) < 3.0f);

	const float dt = 1.0f / 30.0f;
	auto place = [&](float x, float z, float yawDegrees) {
		app.teleport({x, 220.0f, z});
		app.camera().yaw = yawDegrees * 0.0174533f;
		app.camera().pitch = 0;
		for (int i = 0; i < 60; ++i) app.update(Input {}, dt);
	};
	auto angle = [](float a, float b) {
		float d = std::fmod(a - b, 6.2831853f);
		if (d > 3.14159265f) d -= 6.2831853f;
		if (d < -3.14159265f) d += 6.2831853f;
		return std::fabs(d);
	};

	// Standing: the idle clip, the camera 3.3 m behind and above the feet.
	place(-1500, 0, 0);
	CHECK(pl.grounded && hero.role() == Hero::IDLE && hero.speed() < 1.0f && hero.animator().current() == hero.model().clip("S_RUN"));
	CHECK(app.heroVisible());
	Vec3 cam = app.camera().pos;
	CHECK(std::fabs(cam.x + 1500.0f) < 1.0f && std::fabs(cam.y - 155.0f) < 1.0f && std::fabs(cam.z + 330.0f) < 2.0f);
	app.render();
	gfx::SoftStats ss = gfx::softStats();
	CHECK(ss.skinnedTriangles == hero.model().triangles() && ss.skinnedPixels > 1500);
	// In the middle of the picture, seen from behind, feet below the horizon.
	{
		const Mat4 vp = mul(perspectiveLH(app.camera().fovY, float(gfx::SCREEN_W) / float(gfx::SCREEN_H), app.camera().zNear, app.camera().zFar),
		                    viewLH(cam, app.camera().yaw, app.camera().pitch));
		const Vec4 feet = transform(vp, {pl.feet.x, pl.feet.y, pl.feet.z, 1.0f}), head = transform(vp, {pl.feet.x, pl.feet.y + 180.0f, pl.feet.z, 1.0f});
		CHECK(std::fabs(feet.x / feet.w) < 0.02f && feet.y / feet.w < -0.5f && head.y / head.w > 0.0f && head.y / head.w < 0.3f);
	}

	// Running: the run clip at the speed of the feet, the body turned the way it goes.
	int runFrames = 0;
	for (int i = 0; i < 45; ++i) {
		Input in;
		in.moveZ = 1.0f;
		app.update(in, dt);
		runFrames += hero.role() == Hero::RUN;
	}
	CHECK(runFrames > 38 && hero.role() == Hero::RUN && std::fabs(hero.speed() - 520.0f) < 8.0f);
	CHECK(hero.animator().current() == hero.model().clip("S_RUNL") && angle(hero.yaw(), 0.0f) < 0.02f);
	CHECK(pl.feet.z > 700.0f && std::fabs(hero.feet().z - pl.feet.z) < 1e-3f);
	// The camera follows at its distance.
	cam = app.camera().pos;
	CHECK(std::fabs(pl.feet.z - cam.z - 330.0f) < 3.0f);
	// Sideways: the body faces where it goes, not where the camera looks.
	for (int i = 0; i < 20; ++i) {
		Input in;
		in.moveX = -1.0f;
		app.update(in, dt);
	}
	CHECK(angle(hero.yaw(), -1.5707963f) < 0.05f && hero.role() == Hero::RUN);
	// Half a push of the stick: a walk, played faster than authored (192 over 150 cm/s).
	for (int i = 0; i < 30; ++i) {
		Input in;
		in.moveZ = 0.37f;
		app.update(in, dt);
	}
	CHECK(hero.role() == Hero::WALK && hero.animator().current() == hero.model().clip("S_WALKL"));
	CHECK(std::fabs(hero.speed() - 0.37f * 520.0f) < 6.0f);
	// Letting go: back to standing.
	for (int i = 0; i < 20; ++i) app.update(Input {}, dt);
	CHECK(hero.role() == Hero::IDLE && hero.speed() < 5.0f);

	// A jump: up, down, and on the ground again.
	place(-1500, 0, 0);
	int jumpFrames = 0, fallFrames = 0;
	for (int i = 0; i < 40; ++i) {
		Input in;
		in.jump = i == 0;
		app.update(in, dt);
		jumpFrames += hero.role() == Hero::JUMP;
		fallFrames += hero.role() == Hero::FALL;
	}
	CHECK(jumpFrames > 4 && fallFrames > 4 && hero.role() == Hero::IDLE && pl.grounded);
	// A 30 cm step up and down is not a jump or a fall.
	place(0, 1200, 0);
	int airFrames = 0;
	for (int i = 0; i < 60; ++i) {
		Input in;
		in.moveZ = 1.0f;
		app.update(in, dt);
		airFrames += hero.role() == Hero::JUMP || hero.role() == Hero::FALL;
	}
	CHECK(pl.feet.z > 2100.0f && airFrames == 0);
	// A teleport is not a sprint.
	app.teleport({-2000, 220, -2000});
	app.update(Input {}, dt);
	CHECK(hero.speed() < 1.0f && std::fabs(hero.feet().x + 2000.0f) < 1e-3f);

	// Lit like the ground it stands on, tinted by the time of day.
	place(-1500, 0, 0);
	const float bright = hero.light()[1];
	place(500, 0, 0);
	const float dim = hero.light()[1];
	CHECK(bright > 1.0f && bright < 1.2f && std::fabs(dim - 60.0f / 255.0f * 1.15f) < 0.02f);
	app.environment().timeOfDay = 1.0f; // night
	place(-1500, 0, 0);
	CHECK(hero.light()[1] < bright * 0.6f && hero.light()[1] >= 0.22f - 1e-3f);
	app.environment().timeOfDay = 13.0f;

	// The camera and the wall at x = 1000. Looking along -X with the wall
	// behind: 2 m of room, so the boom is cut short and the camera stays this
	// side of the wall ...
	place(800, 0, -90);
	cam = app.camera().pos;
	CHECK(cam.x < 1000.0f - 23.0f && cam.x > 950.0f && app.heroVisible());
	// ... and with the back to the wall there is no room at all: the view is
	// (nearly) the character's own, and the character is not drawn.
	place(960, 0, -90);
	cam = app.camera().pos;
	CHECK(!app.heroVisible() && cam.x - pl.feet.x < 25.0f && cam.x - pl.feet.x >= 0.0f && cam.x < 1000.0f - 23.0f &&
	      std::fabs(cam.y - 155.0f) < 1e-3f);
	app.render();
	CHECK(gfx::softStats().skinnedTriangles == 0);
	// Walking away from it: the camera never enters the wall and comes back
	// out gently (no jump of more than a step and a bit per frame).
	float worstJump = 0, boomBefore = pl.feet.x - cam.x;
	bool inWall = false;
	for (int i = 0; i < 60; ++i) {
		Input in;
		in.moveZ = 1.0f;
		app.update(in, dt);
		cam = app.camera().pos;
		inWall = inWall || cam.x > 1000.0f - 23.0f;
		const float boom = cam.x - pl.feet.x;
		worstJump = std::max(worstJump, std::fabs(boom - boomBefore));
		boomBefore = boom;
	}
	CHECK(!inWall && app.heroVisible() && worstJump < 75.0f);
	CHECK(std::fabs(cam.x - pl.feet.x - 330.0f) < 12.0f);
	// Swinging the camera round so that the wall comes between: in at once.
	place(900, 0, 0);
	for (int i = 0; i < 40; ++i) {
		Input in;
		in.lookX = -1.0f; // turn left: the camera swings towards +X
		app.update(in, dt);
		inWall = inWall || app.camera().pos.x > 1000.0f - 23.0f;
	}
	CHECK(!inWall);

	// First person on request, and always while flying.
	place(-1500, 0, 0);
	Input toggle;
	toggle.toggleCamera = true;
	app.update(toggle, dt);
	CHECK(!app.thirdPerson() && !app.heroVisible() && std::fabs(app.camera().pos.y - pl.eye().y) < 1e-3f &&
	      std::fabs(app.camera().pos.z - pl.feet.z) < 1e-3f);
	app.render();
	CHECK(gfx::softStats().skinnedTriangles == 0);
	app.update(toggle, dt);
	CHECK(app.thirdPerson());
	for (int i = 0; i < 10; ++i) app.update(Input {}, dt); // the camera pulls back out
	CHECK(app.heroVisible() && pl.feet.z - app.camera().pos.z > 200.0f);
	Input fly;
	fly.toggleFly = true;
	app.update(fly, dt);
	CHECK(pl.flying && !app.heroVisible() && std::fabs(app.camera().pos.z - pl.feet.z) < 1e-3f);
	app.update(fly, dt);
	CHECK(!pl.flying);

	// Into the cave and its pool: wading is a walk, then the body lies in the
	// surface with its hips a hand below it, and swims as fast as it moves.
	Input next;
	next.nextWorld = true;
	app.update(next, dt);
	CHECK(app.world().indoor() && hero.loaded());
	app.world().preload(app.camera().pos);
	int wadeWalk = 0, wadeFrames = 0, swimFrames = 0;
	for (int i = 0; i < 220 && swimFrames < 40; ++i) {
		Input in;
		in.moveZ = 1.0f;
		app.update(in, dt);
		if (pl.swimming) {
			++swimFrames;
		} else if (pl.waterDepth > pl.config.wadeDepth && pl.grounded) {
			++wadeFrames;
			wadeWalk += hero.role() == Hero::WALK;
		}
	}
	CHECK(wadeFrames > 5 && wadeWalk >= wadeFrames - 3 && swimFrames == 40);
	CHECK(hero.role() == Hero::SWIM && hero.animator().current() == hero.model().clip("S_SWIMF"));
	const float surface = pl.feet.y + pl.waterDepth;
	CHECK(std::fabs(hero.origin().y + hero.animator().pose().pos[0].y - (surface - 22.0f)) < 3.0f);
	app.render();
	CHECK(gfx::softStats().skinnedPixels > 200);
	for (int i = 0; i < 30; ++i) app.update(Input {}, dt);
	CHECK(pl.swimming && hero.role() == Hero::SWIM_IDLE && hero.animator().current() == hero.model().clip("S_SWIM"));

	app.shutdown();
	CHECK(gfx::memStats().meshCount == 0 && gfx::memStats().textureCount == 0);

	// An archive without a character: first person, whatever was asked for.
	{
		PakWriter writer;
		std::vector<BakeWorld> one(1);
		one[0] = worlds[0];
		bakeWorlds(one, bo, writer);
		const std::string plain = gDir + "/nohero.pak";
		writer.write(plain);
		App a;
		CHECK(a.init(plain.c_str(), nullptr));
		a.world().preload(a.camera().pos);
		a.setThirdPerson(true);
		for (int i = 0; i < 30; ++i) a.update(Input {}, dt);
		CHECK(!a.hero().loaded() && !a.thirdPerson() && !a.heroVisible() && std::fabs(a.camera().pos.y - 170.0f) < 1.0f);
		CHECK(a.player().config.walkSpeed == 480.0f);
		a.render();
		CHECK(gfx::softStats().skinnedTriangles == 0);
		a.shutdown();
	}
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
	const char* only = argc > 2 ? argv[2] : nullptr; // run the tests whose name contains this
	struct {
		const char* name;
		void (*fn)();
	} tests[] = {{"hash", testHash},
	             {"pak", testPak},
	             {"etc1 block decoder", testEtc1BlockDecoder},
	             {"texture", testTexture},
	             {"chunker", testChunker},
	             {"seams", testSeams},
	             {"math", testMath},
	             {"runtime", testRuntime},
	             {"worlds", testWorlds},
	             {"loader", testLoader},
	             {"streaming", testStreaming},
	             {"streaming worlds", testStreamingWorlds},
	             {"pacer", testPacer},
	             {"collision", testCollision},
	             {"model", testModel},
	             {"hero", testHero},
	             {"sky", testSky}};
	for (auto& t : tests) {
		// Later tests reuse the archives earlier ones baked.
		const bool bakes = std::strcmp(t.name, "worlds") == 0 || std::strcmp(t.name, "loader") == 0;
		if (only && !std::strstr(t.name, only) && !bakes) continue;
		const int before = gFailures;
		std::printf("[ RUN ] %s\n", t.name);
		t.fn();
		std::printf("[ %s ] %s\n", gFailures == before ? " OK " : "FAIL", t.name);
	}
	std::printf("%d checks, %d failed\n", gChecks, gFailures);
	return gFailures ? 1 : 0;
}
