// The Barrier over the colony (Gothic 1's oCBarrier), fix150.
//
// The original draws one mesh of the game, MAGICFRONTIER_OUT (a dome over the whole valley, in world coordinates), with
// its texture (BARRIERE.TGA) twice, additively, without writing depth. Every frame it rewrites each vertex
// (oCBarrier::RenderLayer, as GD3D11's recreation for GOUC reads it out of Gothic2.exe, VS_GoucBarrier.hlsl):
//
//   u = u0 + 0.105 s sin(0.098 s (x + z) + 0.42 s + t) + frac(0.15 t) + s
//   v = v0 + 0.105 s sin(0.098 s y       + 0.42 s + t)                 + s
//   alpha = fade * (sin(x + z + t) + 1) / 2
//
// s = 1 for the first layer, 1.5 for the second; t seconds; x y z the vertex in cm. `fade` is what oCBarrier::Init
// gives each vertex: rising over the lowest 80 m of the mesh, falling to 0 above 92.5 % of its height (the dark
// eye at the top). Lightning crawls over it from four sectors, each with a sound of its own (engine/barrier.h).
//
// Pak asset "fx/barrier" (ASSET_META):
//   BarrierHeader | BarrierVertex[vertexCount] | u16 indices[indexCount] (triangles)
// and the texture under BarrierHeader::texture (a pak name hash, "tex/<name>"; 0: none baked).
// Pak asset "meta/barriersfx" (ASSET_META, text): the scripts' sound instances named like the Barrier, one a line:
//   "A NAME" its hum (AMBIENT, LOOP), "T NAME" the rest (the lightning: SHOOT, THUNDER ...).
#pragma once
#include "g3ds/base.h"

namespace g3 {

constexpr u32 BARRIER_MAGIC = fourcc('G', '3', 'B', 'R');
constexpr u32 BARRIER_VERSION = 1;

// oCBarrier's numbers.
constexpr float BARRIER_GROUND_FADE = 8000.0f; // cm over the bottom of the mesh in which it fades in
constexpr float BARRIER_TOP_FADE = 0.925f;     // share of the height above which it fades out
constexpr float BARRIER_WAVE = 0.105f;         // amplitude of the texture wobble
constexpr float BARRIER_WAVE_FREQ = 0.098f;    // per cm
constexpr float BARRIER_WAVE_PHASE = 0.42f;
constexpr float BARRIER_SCROLL = 0.15f;        // texture repeats a second along u

struct BarrierHeader {
	u32 magic;
	u32 version;
	u32 vertexCount;
	u32 indexCount;  // a multiple of 3
	u32 texture;     // hashName("tex/...") of its texture, 0: none
	u32 top;         // the highest vertex (where the lightning goes)
	float bbMin[3];  // world cm
	float bbMax[3];
	u32 reserved[2];
};
static_assert(sizeof(BarrierHeader) == 56, "BarrierHeader layout");

struct BarrierVertex {
	float pos[3]; // world cm
	float uv[2];
	float fade;   // 0..1 (oCBarrier::Init)
};
static_assert(sizeof(BarrierVertex) == 24, "BarrierVertex layout");

inline u32 barrierBlobSize(u32 vertexCount, u32 indexCount) {
	return u32(sizeof(BarrierHeader)) + vertexCount * u32(sizeof(BarrierVertex)) + indexCount * 2u;
}

// oCBarrier::Init's fade of a vertex at height y of a mesh from y0 to y1.
inline float barrierFadeAt(float y, float y0, float y1) {
	const float h = y1 - y0;
	if (h <= 0.0f) return 1.0f;
	const float rel = y - y0;
	const float topStart = h * BARRIER_TOP_FADE;
	float a;
	if (rel > topStart) a = (h - rel) / (h - topStart > 1.0f ? h - topStart : 1.0f);
	else a = rel / BARRIER_GROUND_FADE;
	return a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
}

} // namespace g3
