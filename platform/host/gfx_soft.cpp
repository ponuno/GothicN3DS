// Software backend for the PC build. It is not meant to be fast or pretty: it
// reproduces the pipeline the 3DS backend sets up (texture * vertex colour,
// alpha test, blending, per-vertex linear fog, CCW front faces, 16-bit
// indexed meshes) so the whole data path can be tested without a 3DS.
// Textures are decoded from the very same PICA-native bytes the console gets.
#include "gfx_soft.h"

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
FrameDesc gFrame;
Mat4 gProj, gMvp;
float gUvScale = 1.0f;
float gFogA = 1.0f, gFogB = 0.0f; // fog factor = clamp(A - viewZ * B)
Pass gPass = PASS_WORLD;
Texture* gTexture = nullptr;
u32 gMaterialFlags = 0;
float gOpacity = 1.0f;
float gUvOffset[2] = {0, 0};
bool gCull = true;
u32 gMemoryLimit = 0;
MemStats gMem;
SoftStats gStats;

bool fits(u32 bytes) { return gMemoryLimit == 0 || gMem.meshBytes + gMem.textureBytes + bytes <= gMemoryLimit; }

struct ClipVert {
	float x, y, z, w;
	float u, v;
	float r, g, b, a;
	float fog; // 1 = clear, 0 = fully fogged; computed per vertex like the shader does
};

inline ClipVert lerpVert(const ClipVert& a, const ClipVert& b, float t) {
	ClipVert o;
	const float* pa = &a.x;
	const float* pb = &b.x;
	float* po = &o.x;
	for (int i = 0; i < 11; ++i) po[i] = pa[i] + (pb[i] - pa[i]) * t;
	return o;
}

struct ScreenVert {
	float sx, sy, z, invW;
	float uw, vw; // u/w, v/w
	float r, g, b, a; // already divided by w
	float fog;        // already divided by w
};

void rasterize(const ScreenVert& a, const ScreenVert& b, const ScreenVert& c, float lod) {
	const float area = (b.sx - a.sx) * (c.sy - a.sy) - (c.sx - a.sx) * (b.sy - a.sy);
	if (area == 0.0f) return;
	const float invArea = 1.0f / area;

	int x0 = int(floorf(min(min(a.sx, b.sx), c.sx)));
	int x1 = int(ceilf(max(max(a.sx, b.sx), c.sx)));
	int y0 = int(floorf(min(min(a.sy, b.sy), c.sy)));
	int y1 = int(ceilf(max(max(a.sy, b.sy), c.sy)));
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
	const bool blend = gPass == PASS_BLEND;
	const bool additive = blend && (gMaterialFlags & MATF_ADD) != 0;
	const bool alphaTest = !blend && (gMaterialFlags & MATF_ALPHA_TEST) != 0;
	const bool depthTest = gPass != PASS_SKY, depthWrite = gPass == PASS_WORLD;
	const bool count = gPass == PASS_WORLD; // statistics describe the opaque world
	if (count) ++gStats.trianglesRasterised;

	for (int y = y0; y <= y1; ++y) {
		for (int x = x0; x <= x1; ++x) {
			const float px = float(x) + 0.5f, py = float(y) + 0.5f;
			float w0 = ((b.sx - px) * (c.sy - py) - (c.sx - px) * (b.sy - py)) * invArea;
			float w1 = ((c.sx - px) * (a.sy - py) - (a.sx - px) * (c.sy - py)) * invArea;
			float w2 = 1.0f - w0 - w1;
			if (w0 < 0 || w1 < 0 || w2 < 0) continue;

			const int idx = y * SCREEN_W + x;
			const float z = a.z * w0 + b.z * w1 + c.z * w2;
			gStats.pixelsTested += count;
			if (depthTest && z >= gDepth[idx]) continue;

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
			if (alphaTest && al <= 0.5f) continue;

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

	// Clip against the near plane (z >= 0); the rest is handled by the scissor.
	ClipVert poly[4];
	int n = 0;
	for (int i = 0; i < 3; ++i) {
		const ClipVert& cur = in[i];
		const ClipVert& nxt = in[(i + 1) % 3];
		const bool curIn = cur.z >= 0, nxtIn = nxt.z >= 0;
		if (curIn) poly[n++] = cur;
		if (curIn != nxtIn) poly[n++] = lerpVert(cur, nxt, cur.z / (cur.z - nxt.z));
	}
	if (n < 3) return;

	ScreenVert sv[4];
	for (int i = 0; i < n; ++i) {
		const ClipVert& p = poly[i];
		const float invW = 1.0f / max(p.w, 1e-6f);
		sv[i].sx = (p.x * invW * 0.5f + 0.5f) * float(SCREEN_W);
		sv[i].sy = (0.5f - p.y * invW * 0.5f) * float(SCREEN_H);
		sv[i].z = p.z * invW;
		sv[i].invW = invW;
		sv[i].uw = p.u * invW;
		sv[i].vw = p.v * invW;
		sv[i].r = p.r * invW;
		sv[i].g = p.g * invW;
		sv[i].b = p.b * invW;
		sv[i].a = p.a * invW;
		sv[i].fog = p.fog * invW;
	}

	// Screen y points down, so a counter-clockwise (front) triangle has a
	// negative area here. PICA default: GPU_CULL_BACK_CCW.
	const float area =
	    (sv[1].sx - sv[0].sx) * (sv[2].sy - sv[0].sy) - (sv[2].sx - sv[0].sx) * (sv[1].sy - sv[0].sy);
	if (area >= 0.0f && gPass == PASS_WORLD) {
		++gStats.trianglesBackface;
		if (gCull) return;
	}

	// One mip level per triangle from the texel/pixel area ratio.
	float lod = 0.0f;
	if (gTexture) {
		const float uvArea = fabsf((poly[1].u - poly[0].u) * (poly[2].v - poly[0].v) -
		                           (poly[2].u - poly[0].u) * (poly[1].v - poly[0].v)) *
		                     float(gTexture->width) * float(gTexture->height);
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

void meshDestroy(Mesh* m) {
	if (!m) return;
	gMem.meshBytes -= m->vertexCount * u32(sizeof(WorldVertex)) + m->indexCount * u32(sizeof(u16));
	--gMem.meshCount;
	free(m->vertices);
	free(m->indices);
	free(m);
}

void frameBegin(const FrameDesc& f) {
	gFrame = f;
	gProj = perspectiveLH(f.fovY, float(SCREEN_W) / float(SCREEN_H), f.zNear, f.zFar);
	gMvp = mul(gProj, f.view);
	gPass = PASS_SKY; // force setPass to set the fog terms up
	setPass(PASS_WORLD);
	for (int i = 0; i < SCREEN_W * SCREEN_H; ++i) {
		gColor[i * 4 + 0] = f.clearColor[0];
		gColor[i * 4 + 1] = f.clearColor[1];
		gColor[i * 4 + 2] = f.clearColor[2];
		gColor[i * 4 + 3] = 255;
		gDepth[i] = 1.0f;
	}
	gStats = {};
	gTexture = nullptr;
	gMaterialFlags = 0;
	gOpacity = 1.0f;
	gUvOffset[0] = gUvOffset[1] = 0.0f;
}

void setPass(Pass pass) {
	if (pass == gPass) return;
	gPass = pass;
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

void setChunkTransform(const float origin[3], float posScale, float uvScale) {
	gMvp = mul(gProj, mul(gFrame.view, translateScale(origin, posScale)));
	gUvScale = uvScale;
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
	for (u32 i = 0; i + 2 < indexCount; i += 3) {
		ClipVert cv[3];
		for (int k = 0; k < 3; ++k) {
			const WorldVertex& v = m->vertices[m->indices[firstIndex + i + u32(k)]];
			const Vec4 c = transform(gMvp, {float(v.x), float(v.y), float(v.z), 1.0f});
			// clip.w is the view-space depth for a perspective projection.
			const float fog = clamp(gFogA - c.w * gFogB, 0.0f, 1.0f);
			// Same arithmetic as the vertex shader: the vertex alpha says how
			// strongly the time of day tints the baked light.
			const float weight = v.a * (1.0f / 255.0f);
			const float tint[3] = {1.0f + weight * (gFrame.dayLight[0] - 1.0f), 1.0f + weight * (gFrame.dayLight[1] - 1.0f),
			                       1.0f + weight * (gFrame.dayLight[2] - 1.0f)};
			cv[k] = {c.x, c.y, c.z, c.w, float(v.u) * gUvScale + gUvOffset[0], float(v.v) * gUvScale + gUvOffset[1],
			         v.r * (1.0f / 255.0f) * tint[0], v.g * (1.0f / 255.0f) * tint[1], v.b * (1.0f / 255.0f) * tint[2],
			         1.0f, fog};
		}
		if (gPass == PASS_WORLD) ++gStats.trianglesSubmitted;
		drawTriangle(cv);
	}
}

void frameEnd() {}

void releaseDestroyed() {} // nothing is deferred here

MemStats memStats() { return gMem; }
FrameStats frameStats() { return {0, 0.0f}; }

const u8* softFramebuffer() { return gColor; }
SoftStats softStats() { return gStats; }
void softSetCulling(bool enabled) { gCull = enabled; }
void softSetMemoryLimit(u32 bytes) { gMemoryLimit = bytes; }

} // namespace gfx
} // namespace g3
