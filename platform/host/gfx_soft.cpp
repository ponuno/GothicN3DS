// Software backend for the PC build. It is not meant to be fast or pretty: it
// reproduces the pipeline the 3DS backend sets up (texture * vertex colour,
// alpha test, blending, per-vertex linear fog, CCW front faces, 16-bit
// indexed meshes) so the whole data path can be tested without a 3DS.
// Textures are decoded from the very same PICA-native bytes the console gets.
// Like the hardware it snaps vertices to a sub-pixel grid and fills with
// exact integer edge tests and a top-left rule, so two triangles sharing an
// edge never leave a pixel between them - and a pixel of background showing
// through is a real gap in the geometry, not a rounding accident.
#include "gfx_soft.h"
#include "saves.h"

#include <stdlib.h>
#include <string.h>

namespace g3 {
namespace gfx {

struct Texture {
	u16 width, height;
	u8 mipCount;
	u8* level[12];
	u32 bytes;
};

struct Mesh {
	WorldVertex* vertices;
	u16* indices;
	u32 vertexCount, indexCount;
};

namespace {

u8 gColor[SCREEN_W * SCREEN_H * 4];
float gDepth[SCREEN_W * SCREEN_H];
// setDrawOnce: the shape that last drew each pixel (as the console's stencil does it).
u8 gOnce[SCREEN_W * SCREEN_H];
u8 gOnceRef = 0;
bool gOnceOn = false;
FrameDesc gFrame;
Mat4 gProj, gViewProj;  // gViewProj takes positions relative to gAnchor
float gAnchor[3] = {0, 0, 0};
float gChunk[4] = {0, 0, 0, 1}; // chunk origin relative to the anchor, position scale
float gUvScale = 1.0f;
float gFogA = 1.0f, gFogB = 0.0f; // fog factor = clamp(A - viewZ * B)
Pass gPass = PASS_WORLD;
Texture* gTexture = nullptr;
u32 gMaterialFlags = 0;
float gOpacity = 1.0f;
float gFade = 1.0f; // setFade: share of the pixels drawn
// The noise the 3DS backend dissolves with: an ordered 8x8 pattern, 0..1.
float bayer(int x, int y) {
	const int a = (x ^ y) & 7, b = y & 7;
	const int rank = ((a & 1) << 5) | ((b & 1) << 4) | ((a & 2) << 2) | ((b & 2) << 1) | ((a & 4) >> 1) | ((b & 4) >> 2);
	return (float(rank) * 4.0f + 2.0f) * (1.0f / 255.0f);
}
float gUvOffset[2] = {0, 0};
ChunkFx gFx; // Graphics+ of the chunk being drawn (setChunkTransform)
float gSunDark = 0.0f; // setSunShadow
float gChunkSunDark = 0.0f; // fix186: the strength the chunk's mask is drawn with (ChunkFx::sunDark, else gSunDark)
bool gSunOn = false;   // the draw going on takes the chunk's sun mask
LampSet gLamps;        // fix148: setLamps (count 0: none)
bool gCull = true;
bool gTwoSided = false; // setTwoSided: this draw keeps its back faces
u32 gMemoryLimit = 0;
MemStats gMem;
SoftStats gStats;

bool fits(u32 bytes) { return gMemoryLimit == 0 || gMem.meshBytes + gMem.textureBytes + bytes <= gMemoryLimit; }

struct ClipVert {
	float x, y, z, w;
	float u, v;
	float r, g, b, a;
	float fog; // 1 = clear, 0 = fully fogged; computed per vertex like the shader does
	float u2, v2; // the sun's mask (texture coordinate 2)
};

inline ClipVert lerpVert(const ClipVert& a, const ClipVert& b, float t) {
	ClipVert o;
	const float* pa = &a.x;
	const float* pb = &b.x;
	float* po = &o.x;
	for (int i = 0; i < 13; ++i) po[i] = pa[i] + (pb[i] - pa[i]) * t;
	return o;
}

struct ScreenVert {
	float sx, sy, z, invW;
	float uw, vw; // u/w, v/w
	float r, g, b, a; // already divided by w
	float fog;        // already divided by w
	float u2w, v2w;   // the sun's mask coordinate / w
	i32 fx, fy;       // screen position in 1/16 pixel
};

const int kSubpixel = 16;

using i64 = long long;

// Twice the signed area of (a, b, p); positive when p is to the right of
// a -> b on a screen whose y points down.
inline i64 edge(const ScreenVert& a, const ScreenVert& b, i64 px, i64 py) {
	return i64(b.fx - a.fx) * (py - a.fy) - i64(b.fy - a.fy) * (px - a.fx);
}

// Pixels exactly on an edge belong to the triangle only if the edge is a top
// or a left one; the neighbour across the edge sees it the other way round.
inline bool topLeft(const ScreenVert& a, const ScreenVert& b) {
	const i32 dx = b.fx - a.fx, dy = b.fy - a.fy;
	return dy < 0 || (dy == 0 && dx > 0);
}

void rasterize(const ScreenVert& a, const ScreenVert& bIn, const ScreenVert& cIn, float lod) {
	const i64 area0 = edge(a, bIn, cIn.fx, cIn.fy);
	if (area0 == 0) return;
	// Same orientation for both windings: the inside is where all three edge
	// functions are positive.
	const ScreenVert& b = area0 > 0 ? bIn : cIn;
	const ScreenVert& c = area0 > 0 ? cIn : bIn;
	const i64 area = area0 > 0 ? area0 : -area0;
	const float invArea = 1.0f / float(area);

	int x0 = min(min(a.fx, b.fx), c.fx) / kSubpixel - 1;
	int x1 = max(max(a.fx, b.fx), c.fx) / kSubpixel + 1;
	int y0 = min(min(a.fy, b.fy), c.fy) / kSubpixel - 1;
	int y1 = max(max(a.fy, b.fy), c.fy) / kSubpixel + 1;
	x0 = max(x0, 0);
	y0 = max(y0, 0);
	x1 = min(x1, SCREEN_W - 1);
	y1 = min(y1, SCREEN_H - 1);
	if (x0 > x1 || y0 > y1) return;

	const u8* texels = nullptr;
	int tw = 1, th = 1;
	if (gTexture) {
		int level = clamp(int(lod + 0.5f), 0, int(gTexture->mipCount) - 1);
		texels = gTexture->level[level];
		tw = gTexture->width >> level;
		th = gTexture->height >> level;
	}
	// Blended materials are never cut out: their alpha is the blend factor.
	const bool blend = gPass == PASS_BLEND || (gPass == PASS_SKY && (gMaterialFlags & MATF_TRANSLUCENT) != 0); // (fix150: the Barrier)
	const bool additive = blend && (gMaterialFlags & MATF_ADD) != 0;
	const bool alphaTest = !blend && (gMaterialFlags & MATF_ALPHA_TEST) != 0;
	const bool fading = gFade < 1.0f && gPass == PASS_WORLD;
	const bool depthTest = gPass != PASS_SKY, depthWrite = gPass == PASS_WORLD;
	const bool count = gPass == PASS_WORLD; // statistics describe the opaque world
	if (count) ++gStats.trianglesRasterised;
	const i64 biasBC = topLeft(b, c) ? 0 : 1, biasCA = topLeft(c, a) ? 0 : 1, biasAB = topLeft(a, b) ? 0 : 1;

	for (int y = y0; y <= y1; ++y) {
		for (int x = x0; x <= x1; ++x) {
			const i64 px = i64(x) * kSubpixel + kSubpixel / 2, py = i64(y) * kSubpixel + kSubpixel / 2;
			const i64 e0 = edge(b, c, px, py), e1 = edge(c, a, px, py), e2 = edge(a, b, px, py);
			if (e0 < biasBC || e1 < biasCA || e2 < biasAB) continue;
			const float w0 = float(e0) * invArea, w1 = float(e1) * invArea, w2 = float(e2) * invArea;

			const int idx = y * SCREEN_W + x;
			const float z = a.z * w0 + b.z * w1 + c.z * w2;
			gStats.pixelsTested += count;
			if (depthTest && z >= gDepth[idx]) continue;
			if (gOnceOn && blend) {
				if (gOnce[idx] == gOnceRef) continue;
				gOnce[idx] = gOnceRef;
			}

			const float invW = a.invW * w0 + b.invW * w1 + c.invW * w2;
			const float w = 1.0f / invW;
			float r = (a.r * w0 + b.r * w1 + c.r * w2) * w;
			float g = (a.g * w0 + b.g * w1 + c.g * w2) * w;
			float bl = (a.b * w0 + b.b * w1 + c.b * w2) * w;
			float al = (a.a * w0 + b.a * w1 + c.a * w2) * w;
			if (texels) {
				const float u = (a.uw * w0 + b.uw * w1 + c.uw * w2) * w;
				const float v = (a.vw * w0 + b.vw * w1 + c.vw * w2) * w;
				int tx = int(floorf(u * float(tw))) & (tw - 1);
				int ty = int(floorf(v * float(th))) & (th - 1);
				const u8* t = texels + (ty * tw + tx) * 4;
				r *= t[0] * (1.0f / 255.0f);
				g *= t[1] * (1.0f / 255.0f);
				bl *= t[2] * (1.0f / 255.0f);
				al *= t[3] * (1.0f / 255.0f);
			}
			if (gSunOn) {
				// As the console's stages 1 and 2: colour * (1 - strength * (1 - mask)), the mask filtered, clamped at its edges.
				const Texture* m = gFx.sun;
				const float mu = (a.u2w * w0 + b.u2w * w1 + c.u2w * w2) * w * float(m->width) - 0.5f;
				const float mv = (a.v2w * w0 + b.v2w * w1 + c.v2w * w2) * w * float(m->height) - 0.5f;
				const float fu = floorf(mu), fv = floorf(mv);
				const int ix = int(fu), iy = int(fv);
				const float tx = mu - fu, ty2 = mv - fv;
				auto at = [&](int x, int y) {
					x = x < 0 ? 0 : (x >= int(m->width) ? int(m->width) - 1 : x);
					y = y < 0 ? 0 : (y >= int(m->height) ? int(m->height) - 1 : y);
					return float(m->level[0][(y * int(m->width) + x) * 4]) * (1.0f / 255.0f);
				};
				const float mask = (at(ix, iy) * (1.0f - tx) + at(ix + 1, iy) * tx) * (1.0f - ty2) +
				                   (at(ix, iy + 1) * (1.0f - tx) + at(ix + 1, iy + 1) * tx) * ty2;
				const float k = 1.0f - gChunkSunDark * (1.0f - mask);
				r *= k;
				g *= k;
				bl *= k;
			}
			if (fading) {
				// As the PICA's texture stages do it: alpha - noise (not below 0) + share - 0.5, tested > 0.5.
				const float faded = max(al - bayer(x, y), 0.0f) + gFade - 0.5f;
				if (faded <= 0.5f) continue;
			} else if (alphaTest && al <= 0.5f) {
				continue;
			}

			const float fog = clamp((a.fog * w0 + b.fog * w1 + c.fog * w2) * w, 0.0f, 1.0f);
			// Added light fades to nothing in the fog; everything else fades to the fog colour.
			const float fogTo[3] = {additive ? 0.0f : float(gFrame.fogColor[0]), additive ? 0.0f : float(gFrame.fogColor[1]),
			                        additive ? 0.0f : float(gFrame.fogColor[2])};
			float src[3] = {fogTo[0] + (r * 255.0f - fogTo[0]) * fog, fogTo[1] + (g * 255.0f - fogTo[1]) * fog,
			                fogTo[2] + (bl * 255.0f - fogTo[2]) * fog};
			u8* out = gColor + idx * 4;
			if (blend) {
				const float alpha = clamp(al * gOpacity, 0.0f, 1.0f);
				const float keep = additive ? 1.0f : 1.0f - alpha;
				for (int ch = 0; ch < 3; ++ch) src[ch] = src[ch] * alpha + float(out[ch]) * keep;
			}
			out[0] = u8(clamp(src[0], 0.0f, 255.0f));
			out[1] = u8(clamp(src[1], 0.0f, 255.0f));
			out[2] = u8(clamp(src[2], 0.0f, 255.0f));
			out[3] = 255;
			if (depthWrite) gDepth[idx] = z;
			gStats.pixelsWritten += count;
		}
	}
}

// Clips a convex polygon against one plane; `dist` >= 0 is inside.
template <typename Dist> int clipPolygon(const ClipVert* in, int n, ClipVert* out, Dist dist) {
	int m = 0;
	for (int i = 0; i < n; ++i) {
		const ClipVert& cur = in[i];
		const ClipVert& nxt = in[(i + 1) % n];
		const float dc = dist(cur), dn = dist(nxt);
		if (dc >= 0) out[m++] = cur;
		if ((dc >= 0) != (dn >= 0)) {
			// Always from the inside vertex, so both triangles sharing this
			// edge cut it at the very same point.
			out[m++] = dc >= 0 ? lerpVert(cur, nxt, dc / (dc - dn)) : lerpVert(nxt, cur, dn / (dn - dc));
		}
	}
	return m;
}

void drawTriangle(const ClipVert in[3]) {
	// Trivial rejection against the side planes.
	int outL = 0, outR = 0, outB = 0, outT = 0, outN = 0, outF = 0;
	for (int i = 0; i < 3; ++i) {
		outL += in[i].x < -in[i].w;
		outR += in[i].x > in[i].w;
		outB += in[i].y < -in[i].w;
		outT += in[i].y > in[i].w;
		outN += in[i].z < 0;
		outF += in[i].z > in[i].w;
	}
	if (outL == 3 || outR == 3 || outB == 3 || outT == 3 || outN == 3 || outF == 3) {
		if (gPass == PASS_WORLD) ++gStats.trianglesClipped;
		return;
	}

	// Clip against the near plane and a guard band far outside the screen (so
	// the sub-pixel coordinates stay small); the scissor does the rest.
	const float kGuard = 16.0f;
	ClipVert bufA[12], bufB[12];
	int n = clipPolygon(in, 3, bufA, [](const ClipVert& v) { return v.z; });
	n = clipPolygon(bufA, n, bufB, [&](const ClipVert& v) { return v.x + kGuard * v.w; });
	n = clipPolygon(bufB, n, bufA, [&](const ClipVert& v) { return kGuard * v.w - v.x; });
	n = clipPolygon(bufA, n, bufB, [&](const ClipVert& v) { return v.y + kGuard * v.w; });
	n = clipPolygon(bufB, n, bufA, [&](const ClipVert& v) { return kGuard * v.w - v.y; });
	if (n < 3) return;
	const ClipVert* poly = bufA;

	ScreenVert sv[12];
	for (int i = 0; i < n; ++i) {
		const ClipVert& p = poly[i];
		const float invW = 1.0f / max(p.w, 1e-6f);
		sv[i].sx = (p.x * invW * 0.5f + 0.5f) * float(SCREEN_W);
		sv[i].sy = (0.5f - p.y * invW * 0.5f) * float(SCREEN_H);
		sv[i].fx = i32(lroundf(sv[i].sx * float(kSubpixel)));
		sv[i].fy = i32(lroundf(sv[i].sy * float(kSubpixel)));
		sv[i].z = p.z * invW;
		sv[i].invW = invW;
		sv[i].uw = p.u * invW;
		sv[i].vw = p.v * invW;
		sv[i].r = p.r * invW;
		sv[i].g = p.g * invW;
		sv[i].b = p.b * invW;
		sv[i].a = p.a * invW;
		sv[i].fog = p.fog * invW;
		sv[i].u2w = p.u2 * invW;
		sv[i].v2w = p.v2 * invW;
	}

	// Screen y points down, so a counter-clockwise (front) triangle has a
	// negative area here. PICA default: GPU_CULL_BACK_CCW. Summed over the
	// whole clipped polygon: its first three corners may be a sliver.
	float area = 0.0f, uvArea = 0.0f;
	for (int i = 1; i + 1 < n; ++i) {
		area += (sv[i].sx - sv[0].sx) * (sv[i + 1].sy - sv[0].sy) - (sv[i + 1].sx - sv[0].sx) * (sv[i].sy - sv[0].sy);
		uvArea += (poly[i].u - poly[0].u) * (poly[i + 1].v - poly[0].v) - (poly[i + 1].u - poly[0].u) * (poly[i].v - poly[0].v);
	}
	if (area >= 0.0f && gPass == PASS_WORLD) {
		++gStats.trianglesBackface;
		if (gCull && !gTwoSided) return;
	}

	// One mip level per triangle from the texel/pixel area ratio.
	float lod = 0.0f;
	if (gTexture) {
		uvArea = fabsf(uvArea) * float(gTexture->width) * float(gTexture->height);
		const float pxArea = fabsf(area);
		if (pxArea > 1e-3f && uvArea > pxArea) lod = 0.5f * log2f(uvArea / pxArea);
	}

	for (int i = 1; i + 1 < n; ++i) rasterize(sv[0], sv[i], sv[i + 1], lod);
}

} // namespace

bool init() {
	gMem = {};
	gStats = {};
	return true;
}

void shutdown() {}

Texture* textureCreate(const TexHeader& h, const u8* data) {
	if (!fits(h.dataSize)) return nullptr;
	Texture* t = static_cast<Texture*>(calloc(1, sizeof(Texture)));
	if (!t) return nullptr;
	t->width = h.width;
	t->height = h.height;
	t->mipCount = h.mipCount;
	for (u32 l = 0; l < h.mipCount; ++l) {
		t->level[l] = static_cast<u8*>(malloc(size_t(h.width >> l) * (h.height >> l) * 4));
		if (!t->level[l]) {
			textureDestroy(t);
			return nullptr;
		}
		texDecodeLevel(h, data, l, t->level[l]);
	}
	// Account for what the console would hold, not for the decoded copy.
	t->bytes = h.dataSize;
	gMem.textureBytes += t->bytes;
	++gMem.textureCount;
	return t;
}

void textureUpdateMask(Texture* t, const u8* texels) {
	if (!t || !texels || !t->level[0]) return;
	for (u32 y = 0; y < t->height; ++y)
		for (u32 x = 0; x < t->width; ++x) {
			u8* p = t->level[0] + (size_t(y) * t->width + x) * 4;
			p[0] = p[1] = p[2] = texels[texL4Nibble(t->width, t->height, x, y)];
		}
}

void textureDestroy(Texture* t) {
	if (!t) return;
	if (t->bytes) {
		gMem.textureBytes -= t->bytes;
		--gMem.textureCount;
	}
	for (u8* l : t->level) free(l);
	if (gTexture == t) gTexture = nullptr;
	free(t);
}

Mesh* meshCreate(const WorldVertex* vertices, u32 vertexCount, const u16* indices, u32 indexCount) {
	if (!fits(vertexCount * u32(sizeof(WorldVertex)) + indexCount * u32(sizeof(u16)))) return nullptr;
	Mesh* m = static_cast<Mesh*>(calloc(1, sizeof(Mesh)));
	if (!m) return nullptr;
	m->vertices = static_cast<WorldVertex*>(malloc(size_t(vertexCount) * sizeof(WorldVertex) + 1));
	m->indices = static_cast<u16*>(malloc(size_t(indexCount) * sizeof(u16) + 1));
	if (!m->vertices || !m->indices) {
		free(m->vertices);
		free(m->indices);
		free(m);
		return nullptr;
	}
	memcpy(m->vertices, vertices, size_t(vertexCount) * sizeof(WorldVertex));
	memcpy(m->indices, indices, size_t(indexCount) * sizeof(u16));
	m->vertexCount = vertexCount;
	m->indexCount = indexCount;
	gMem.meshBytes += vertexCount * u32(sizeof(WorldVertex)) + indexCount * u32(sizeof(u16));
	++gMem.meshCount;
	return m;
}

void meshUpdate(Mesh* m, const WorldVertex* vertices, u32 vertexCount) {
	if (!m || !vertices) return;
	if (vertexCount > m->vertexCount) vertexCount = m->vertexCount;
	memcpy(m->vertices, vertices, size_t(vertexCount) * sizeof(WorldVertex));
}

void meshDestroy(Mesh* m) {
	if (!m) return;
	gMem.meshBytes -= m->vertexCount * u32(sizeof(WorldVertex)) + m->indexCount * u32(sizeof(u16));
	--gMem.meshCount;
	free(m->vertices);
	free(m->indices);
	free(m);
}

static Mat4 gEyeView[2]; // (stereo test aid: the views of the left and the right eye)

void frameBegin(const FrameDesc& f) {
	gEyeView[0] = f.view;
	gFrame = f;
	gTwoSided = false;
	gProj = perspectiveLH(f.fovY, float(SCREEN_W) / float(SCREEN_H), f.zNear, f.zFar);
	const EyeFrame ef = eyeFrame(f.view, f.eye);
	gViewProj = mul(gProj, ef.view);
	memcpy(gAnchor, ef.anchor, sizeof(gAnchor));
	gChunk[0] = gChunk[1] = gChunk[2] = 0.0f;
	gChunk[3] = 1.0f;
	gPass = PASS_SKY; // force setPass to set the fog terms up
	setPass(PASS_WORLD);
	for (int i = 0; i < SCREEN_W * SCREEN_H; ++i) {
		gColor[i * 4 + 0] = f.clearColor[0];
		gColor[i * 4 + 1] = f.clearColor[1];
		gColor[i * 4 + 2] = f.clearColor[2];
		gColor[i * 4 + 3] = 255;
		gDepth[i] = 1.0f;
	}
	memset(gOnce, 0, sizeof(gOnce));
	gOnceRef = 0;
	gOnceOn = false;
	gStats = {};
	gTexture = nullptr;
	gMaterialFlags = 0;
	gOpacity = 1.0f;
	gFade = 1.0f;
	gUvOffset[0] = gUvOffset[1] = 0.0f;
	gFx = ChunkFx {};
	gLamps = LampSet {};
}

bool fadeSupported() { return true; }

void setFade(float visible) { gFade = clamp(visible, 0.0f, 1.0f); }

void setDrawOnce(bool on) {
	gOnceOn = on && gPass == PASS_BLEND;
	if (gOnceOn) gOnceRef = u8(gOnceRef >= 255 ? 1 : gOnceRef + 1);
}

void setTwoSided(bool both) { gTwoSided = both && gPass == PASS_WORLD; }

void setPass(Pass pass) {
	gTwoSided = false;
	gOnceOn = false;
	if (pass == gPass) return;
	gPass = pass;
	gFade = 1.0f; // (dissolving is for the opaque world only)
	if (pass == PASS_SKY) {
		gFogA = 1.0f;
		gFogB = 0.0f;
	} else {
		// No usable fog range means no fog, as in the 3DS backend.
		const float range = gFrame.fogEnd - gFrame.fogStart;
		gFogB = range > 0.0f ? 1.0f / range : 0.0f;
		gFogA = range > 0.0f ? gFrame.fogEnd * gFogB : 1.0f;
	}
}

void setChunkTransform(const float origin[3], float posScale, float uvScale, const ChunkFx* fx) {
	for (int a = 0; a < 3; ++a) gChunk[a] = origin[a] - gAnchor[a];
	gChunk[3] = posScale;
	gUvScale = uvScale;
	gFx = fx ? *fx : ChunkFx {};
}

void setMaterial(Texture* t, u32 materialFlags, float opacity, float uvOffsetU, float uvOffsetV) {
	gTexture = t;
	gMaterialFlags = materialFlags;
	gOpacity = opacity;
	gUvOffset[0] = uvOffsetU;
	gUvOffset[1] = uvOffsetV;
}

void draw(Mesh* m, u32 firstIndex, u32 indexCount) {
	if (!m || indexCount == 0 || firstIndex + indexCount > m->indexCount) return;
	const bool windy = (gMaterialFlags & MATF_WIND) != 0 && (gFx.windX != 0.0f || gFx.windZ != 0.0f);
	const bool water = (gMaterialFlags & MATF_WATER) != 0;
	const bool wavy = water && gFx.waveHeight != 0.0f;
	const bool sheen = water && gFx.waterGrid; // (fix140: the water cut in the grid, as the full shader's `wavy` block)
	gChunkSunDark = gFx.sunDark >= 0.0f ? clamp(gFx.sunDark, 0.0f, 1.0f) : gSunDark;
	gSunOn = gFx.sun && gChunkSunDark > 0.0f && gPass != PASS_SKY && (gMaterialFlags & (MATF_ALPHA_TEST | MATF_WIND | MATF_ADD | MATF_WATER)) == 0;
	// fix148: the lamps, where the console binds their entry: what its fast entry draws in the world pass (not what sways,
	// not water cut for waves), and not a chunk drawn with its sun's mask.
	const bool lamps = gLamps.count && gPass == PASS_WORLD && (gMaterialFlags & MATF_WIND) == 0 && !wavy && !(water && gFx.waterGrid) &&
	                   !(gFx.sun && gChunkSunDark > 0.0f);
	gStats.lampDraws += lamps;
	for (u32 i = 0; i + 2 < indexCount; i += 3) {
		ClipVert cv[3];
		for (int k = 0; k < 3; ++k) {
			const WorldVertex& v = m->vertices[m->indices[firstIndex + i + u32(k)]];
			float p[3] = {float(v.x) * gChunk[3], float(v.y) * gChunk[3], float(v.z) * gChunk[3]};
			// Graphics+ as the shader does it: the high byte of w is the sway,
			// moved along the wind by a sine-like wave x (1 - |x|) (peak 1/4).
			const float steps = float(v.w) * (1.0f / 256.0f);
			const float sway = floorf(steps);
			if (windy) {
				float t = gFx.windPhase + (p[0] + p[2]) * (1.0f / WIND_WAVE_CM);
				t -= floorf(t);
				const float x = t + t - 1.0f;
				const float wave = x * (1.0f - fabsf(x)) * sway;
				p[0] += gFx.windX * 4.0f * wave;
				p[2] += gFx.windZ * 4.0f * wave;
			}
			float shade = 1.0f - (steps - sway) * gFx.occlusion; // the low byte: ambient occlusion
			float uvShift[2] = {0.0f, 0.0f};
			if (wavy) {
				// As world_full.v.pica: two waves x (1 - |x|) * 4, the height from both, the slopes light the surface.
				float xs[2], ws[2], slope[2];
				for (int k2 = 0; k2 < 2; ++k2) {
					const float* d = k2 ? WAVE_B_DIR : WAVE_A_DIR;
					const float cm = k2 ? WAVE_B_CM : WAVE_A_CM;
					float t = (k2 ? gFx.waveB : gFx.waveA) + (p[0] * d[0] + p[2] * d[1]) / cm;
					t -= floorf(t);
					xs[k2] = t + t - 1.0f;
					const float edge = 1.0f - fabsf(xs[k2]);
					ws[k2] = 4.0f * xs[k2] * edge;
					slope[k2] = 2.0f * edge - 1.0f;
				}
				const float weight = sway * (1.0f / 127.0f); // (the corner's wind byte: how much of the waves it takes)
				p[1] += weight * gFx.waveHeight * (0.7f * ws[0] + 0.3f * ws[1]);
				shade *= 1.0f + weight * gFx.waveShade * (0.6f * slope[0] + 0.4f * slope[1]);
				uvShift[0] = weight * 0.015f * ws[0];
				uvShift[1] = weight * 0.015f * ws[1];
			}
			// The shader's arithmetic: position relative to the anchor first
			// (exact for lattice positions), then one matrix for every chunk.
			const Vec4 c = transform(gViewProj, {p[0] + gChunk[0], p[1] + gChunk[1], p[2] + gChunk[2], 1.0f});
			// clip.w is the view-space depth for a perspective projection.
			float fog = clamp(gFogA - c.w * gFogB, 0.0f, 1.0f);
			// The sheen as world_full.v.pica: f = t / (1 + t), t = depth / 800 cm; the colour 0.7 + 0.6 f times, the fog's share
			// cut by 0.4 f (the horizon's colour is the sky in the water).
			float sheenK = 1.0f;
			if (sheen) {
				const float t = c.w * WATER_SHEEN_PER_CM;
				const float f = t / (1.0f + t);
				sheenK = WATER_SHEEN_BASE + WATER_SHEEN_RANGE * f;
				fog *= 1.0f - WATER_SHEEN_SKY * f;
			}
			// Same arithmetic as the vertex shader: the vertex alpha says how
			// strongly the time of day tints the baked light.
			const float weight = v.a * (1.0f / 255.0f);
			const float tint[3] = {1.0f + weight * (gFrame.dayLight[0] - 1.0f), 1.0f + weight * (gFrame.dayLight[1] - 1.0f),
			                       1.0f + weight * (gFrame.dayLight[2] - 1.0f)};
			// The lamps as world_lamp.v.pica: after the occlusion, before the fog.
			float lamp[3] = {0.0f, 0.0f, 0.0f};
			if (lamps) {
				const float at[3] = {p[0] + gChunk[0] + gAnchor[0], p[1] + gChunk[1] + gAnchor[1], p[2] + gChunk[2] + gAnchor[2]};
				const float keep = 1.0f + weight * (clamp(gLamps.dayKeep, 0.0f, 1.0f) - 1.0f);
				for (u32 j = 0; j < gLamps.count; ++j) {
					const DrawLamp& l = gLamps.lamp[j];
					if (!(l.range > 1.0f)) continue;
					const float dx = l.pos[0] - at[0], dy = l.pos[1] - at[1], dz = l.pos[2] - at[2];
					const float f = lampFalloff(dx * dx + dy * dy + dz * dz, l.range) * keep;
					for (int ch = 0; ch < 3; ++ch) lamp[ch] += l.rgb[ch] * f;
				}
			}
			cv[k] = {c.x, c.y, c.z, c.w, float(v.u) * gUvScale + gUvOffset[0] + uvShift[0],
			         float(v.v) * gUvScale + gUvOffset[1] + uvShift[1],
			         v.r * (1.0f / 255.0f) * tint[0] * shade * sheenK + lamp[0], v.g * (1.0f / 255.0f) * tint[1] * shade * sheenK + lamp[1],
			         v.b * (1.0f / 255.0f) * tint[2] * shade * sheenK + lamp[2], 1.0f, fog,
			         (p[0] + gChunk[0] + gAnchor[0] - gFx.sunX0) * gFx.sunScaleX, (p[2] + gChunk[2] + gAnchor[2] - gFx.sunZ0) * gFx.sunScaleZ};
		}
		if (gPass == PASS_WORLD) ++gStats.trianglesSubmitted;
		drawTriangle(cv);
	}
	gSunOn = false;
}

void setSunShadow(float darkness) { gSunDark = clamp(darkness, 0.0f, 1.0f); }
void setLamps(const LampSet* lamps) {
	gLamps = LampSet {};
	if (!lamps || !lamps->count) return;
	gLamps.count = min(lamps->count, MAX_DRAW_LAMPS);
	gLamps.dayKeep = lamps->dayKeep;
	for (u32 k = 0; k < gLamps.count; ++k) gLamps.lamp[k] = lamps->lamp[k];
}
bool lampsSupported() { return true; }
bool sunShadowSupported() { return true; }

void drawSkinned(Mesh* m, u32 firstIndex, u32 indexCount, const SkinDraw& skin) {
	if (!m || indexCount == 0 || firstIndex + indexCount > m->indexCount || !skin.bones) return;
	const float rel[3] = {skin.origin[0] - gAnchor[0], skin.origin[1] - gAnchor[1], skin.origin[2] - gAnchor[2]};
	const u32 writtenBefore = gStats.pixelsWritten, backBefore = gStats.trianglesBackface;
	for (u32 i = 0; i + 2 < indexCount; i += 3) {
		ClipVert cv[3];
		for (int k = 0; k < 3; ++k) {
			const WorldVertex& v = m->vertices[m->indices[firstIndex + i + u32(k)]];
			// The shader's arithmetic: the colour bytes are two rows-of-three
			// offsets into the bone palette, the second bone's weight and a shade.
			const u32 b0 = min(u32(v.r) / 3u, skin.boneCount - 1), b1 = min(u32(v.g) / 3u, skin.boneCount - 1);
			const float* m0 = skin.bones + b0 * 12;
			const float* m1 = skin.bones + b1 * 12;
			const float p[3] = {float(v.x) * skin.posScale, float(v.y) * skin.posScale, float(v.z) * skin.posScale};
			const float w1 = float(v.b) * (1.0f / 255.0f);
			float out[3];
			for (int r = 0; r < 3; ++r) {
				const float a = m0[r * 4] * p[0] + m0[r * 4 + 1] * p[1] + m0[r * 4 + 2] * p[2] + m0[r * 4 + 3];
				const float b = m1[r * 4] * p[0] + m1[r * 4 + 1] * p[1] + m1[r * 4 + 2] * p[2] + m1[r * 4 + 3];
				out[r] = a + (b - a) * w1 + rel[r];
			}
			const Vec4 c = transform(gViewProj, {out[0], out[1], out[2], 1.0f});
			const float fog = clamp(gFogA - c.w * gFogB, 0.0f, 1.0f);
			const float shade = float(v.a) * (1.0f / 255.0f);
			cv[k] = {c.x, c.y, c.z, c.w, float(v.u) * skin.uvScale, float(v.v) * skin.uvScale, skin.light[0] * shade,
			         skin.light[1] * shade, skin.light[2] * shade, 1.0f, fog};
		}
		if (gPass == PASS_WORLD) ++gStats.trianglesSubmitted;
		drawTriangle(cv);
	}
	gStats.skinnedTriangles += indexCount / 3;
	gStats.skinnedBackface += gStats.trianglesBackface - backBefore;
	gStats.skinnedPixels += gStats.pixelsWritten - writtenBefore;
}

Texture* uiTextureCreate(u16 w, u16 h, const u8* coverage, bool) {
	const auto pot = [](u32 v) { return v >= 8 && v <= 1024 && (v & (v - 1)) == 0; };
	if (!coverage || !pot(w) || !pot(h) || !fits(u32(w) * h)) return nullptr;
	Texture* t = static_cast<Texture*>(calloc(1, sizeof(Texture)));
	if (!t) return nullptr;
	t->width = w;
	t->height = h;
	t->mipCount = 1;
	t->level[0] = static_cast<u8*>(malloc(size_t(w) * h * 4));
	if (!t->level[0]) {
		free(t);
		return nullptr;
	}
	// As the console holds it: 4 bits of coverage, as brightness and as alpha.
	for (u32 i = 0; i < u32(w) * h; ++i) {
		const u8 c = u8((coverage[i] >> 4) * 17);
		for (int k = 0; k < 4; ++k) t->level[0][i * 4 + u32(k)] = c;
	}
	t->bytes = 0; // (MemStats are the world's and the characters')
	return t;
}

Texture* uiTextureCreateColor(u16 w, u16 h, const u16* rgb565) {
	const auto pot = [](u32 v) { return v >= 8 && v <= 1024 && (v & (v - 1)) == 0; };
	if (!rgb565 || !pot(w) || !pot(h) || !fits(u32(w) * h)) return nullptr;
	Texture* t = static_cast<Texture*>(calloc(1, sizeof(Texture)));
	if (!t) return nullptr;
	t->width = w;
	t->height = h;
	t->mipCount = 1;
	t->level[0] = static_cast<u8*>(malloc(size_t(w) * h * 4));
	if (!t->level[0]) {
		free(t);
		return nullptr;
	}
	for (u32 i = 0; i < u32(w) * h; ++i) {
		const u16 c = rgb565[i];
		u8* o = t->level[0] + i * 4;
		o[0] = u8(((c >> 11) & 31) * 255 / 31);
		o[1] = u8(((c >> 5) & 63) * 255 / 63);
		o[2] = u8((c & 31) * 255 / 31);
		o[3] = 255;
	}
	t->bytes = 0;
	return t;
}

Texture* uiTextureCreateRGBA4(u16 w, u16 h, const u16* rgba4) {
	const auto pot = [](u32 v) { return v >= 8 && v <= 1024 && (v & (v - 1)) == 0; };
	if (!rgba4 || !pot(w) || !pot(h) || !fits(u32(w) * h)) return nullptr;
	Texture* t = static_cast<Texture*>(calloc(1, sizeof(Texture)));
	if (!t) return nullptr;
	t->width = w;
	t->height = h;
	t->mipCount = 1;
	t->level[0] = static_cast<u8*>(malloc(size_t(w) * h * 4));
	if (!t->level[0]) {
		free(t);
		return nullptr;
	}
	for (u32 i = 0; i < u32(w) * h; ++i) {
		const u16 c = rgba4[i];
		u8* o = t->level[0] + i * 4;
		for (int k = 0; k < 4; ++k) o[k] = u8(((c >> (12 - 4 * k)) & 15) * 17);
	}
	t->bytes = 0;
	return t;
}

void uiDraw(Texture* t, const UiQuad* quads, u32 count) {
	if (!t || !quads || !t->level[0]) return;
	for (u32 q = 0; q < count && q < UI_MAX_QUADS; ++q) {
		const UiQuad& in = quads[q];
		const int x0 = max<int>(in.x0, 0), y0 = max<int>(in.y0, 0), x1 = min<int>(in.x1, SCREEN_W), y1 = min<int>(in.y1, SCREEN_H);
		const float du = float(in.u1 - in.u0) / float(max(in.x1 - in.x0, 1)), dv = float(in.v1 - in.v0) / float(max(in.y1 - in.y0, 1));
		const float alpha = float(in.rgba[3]) * (1.0f / 255.0f);
		for (int y = y0; y < y1; ++y) {
			const int ty = clamp(int(float(in.v0) + (float(y - in.y0) + 0.5f) * dv), 0, int(t->height) - 1);
			for (int x = x0; x < x1; ++x) {
				const int tx = clamp(int(float(in.u0) + (float(x - in.x0) + 0.5f) * du), 0, int(t->width) - 1);
				const u8* tex = t->level[0] + (ty * t->width + tx) * 4;
				const float a = float(tex[3]) * (1.0f / 255.0f) * alpha;
				if (a <= 0.0f) continue;
				u8* out = gColor + (y * SCREEN_W + x) * 4;
				for (int ch = 0; ch < 3; ++ch) {
					const float src = float(tex[ch]) * float(in.rgba[ch]) * (1.0f / 255.0f);
					out[ch] = u8(clamp(src * a + float(out[ch]) * (1.0f - a), 0.0f, 255.0f));
				}
			}
		}
	}
}

static bool gFrameDone = false;
void frameEnd() { gFrameDone = true; }

bool captureFrame(u16* rgb565, int w, int h) {
	if (!gFrameDone || !rgb565 || w <= 0 || h <= 0) return false;
	shrinkPicture(SCREEN_W, SCREEN_H, [](int x, int y, u8* c) { memcpy(c, gColor + (y * SCREEN_W + x) * 4, 3); }, rgb565, w, h);
	return true;
}

// Stereo: no second picture here, but the calls are counted so the engine's two-eye drawing can be tested.
static bool gStereo = false;
static u32 gEyes = 0;
bool stereoAvailable() { return true; }
bool stereoActive() { return gStereo; }
void stereoSet(bool on) { gStereo = on; }
void eyeBegin(const FrameDesc& f) {
	++gEyes;
	gEyeView[1] = f.view;
	gOnceOn = false;
}
SoftEyes softEyes() { return SoftEyes {gEyes, gEyeView[0], gEyeView[1]}; }

void releaseDestroyed() {} // nothing is deferred here

MemStats memStats() { return gMem; }
FrameStats frameStats() { return FrameStats {}; }

const u8* softFramebuffer() { return gColor; }
SoftStats softStats() { return gStats; }
void softSetCulling(bool enabled) { gCull = enabled; }
void softSetMemoryLimit(u32 bytes) { gMemoryLimit = bytes; }

} // namespace gfx
} // namespace g3
