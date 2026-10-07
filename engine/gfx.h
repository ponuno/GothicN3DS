// Rendering backend interface. Two implementations:
//   platform/3ds/gfx_c3d.cpp   - PICA200 through citro3d
//   platform/host/gfx_soft.cpp - software rasteriser that mimics the same
//                                fixed-function path (texture * vertex colour,
//                                alpha test, blending, per-vertex linear fog) for PC testing.
// The engine never touches platform APIs directly.
#pragma once
#include "g3ds/math.h"
#include "g3ds/model.h"
#include "g3ds/tex.h"
#include "g3ds/world.h"

namespace g3 {
namespace gfx {

struct Texture;
struct Mesh;

constexpr int SCREEN_W = 400; // top screen
constexpr int SCREEN_H = 240;

struct FrameDesc {
	Mat4 view;
	Vec3 eye; // the camera position `view` was built from
	float fovY;
	float zNear, zFar;
	// Linear fog on view-space depth, evaluated per vertex (like ZenGin's):
	// factor = clamp((fogEnd - z) / (fogEnd - fogStart), 0, 1), 1 = no fog.
	float fogStart, fogEnd;
	u8 fogColor[3];
	u8 clearColor[3];
	// Time-of-day tint. A vertex's light is multiplied by
	// lerp(1, dayLight, vertex.a / 255): full effect outdoors, none indoors.
	float dayLight[3];
};

enum Pass {
	PASS_WORLD, // opaque geometry: depth tested and written, back faces culled, fogged
	PASS_SKY,   // backdrop: no depth, no culling, no fog
	PASS_BLEND, // translucent geometry after the opaque world: depth tested but not
	            // written, both sides drawn, blended per material (MATF_BLEND / MATF_ADD)
};

struct MemStats {
	u32 meshBytes;    // vertex + index buffers (linear heap on 3DS)
	u32 textureBytes; // texture memory
	u32 meshCount;
	u32 textureCount;
};

// What the backend made of the last finished frame.
struct FrameStats {
	u32 drawsDropped;     // draw calls skipped because the GPU command buffer was full
	float commandBufferUse; // 0..1, share of the GPU command buffer the frame needed
	// Timing; all zero where the backend has no display to keep pace with.
	float frameMs; // from the previous frame's display sync to this one's: what the player sees
	float cpuMs;   // spent issuing the frame's draw calls
	float gpuMs;   // spent by the GPU drawing the frame before this one
	// True if the GPU draws one frame while the CPU issues the next (a frame
	// then needs the larger of the two times, not their sum).
	bool overlapped;
};

bool init();
void shutdown();

// `data` is the PICA-native mip chain that follows the TexHeader.
Texture* textureCreate(const TexHeader& h, const u8* data);
void textureDestroy(Texture* t);

Mesh* meshCreate(const WorldVertex* vertices, u32 vertexCount, const u16* indices, u32 indexCount);
void meshDestroy(Mesh* m);

void frameBegin(const FrameDesc& f);
// Starts in PASS_WORLD after frameBegin.
void setPass(Pass pass);
// Sets the dequantisation transform for the following draws:
// world = origin + position * posScale.
void setChunkTransform(const float origin[3], float posScale, float uvScale);
// materialFlags: MaterialFlags from world.h. A null texture draws white.
// opacity scales the alpha of blended materials; uvOffset is added to the
// texture coordinates (scrolling water).
void setMaterial(Texture* t, u32 materialFlags, float opacity = 1.0f, float uvOffsetU = 0.0f, float uvOffsetV = 0.0f);
void draw(Mesh* m, u32 firstIndex, u32 indexCount);

// Skinned geometry (characters). The mesh is made with meshCreate from
// vertices in the layout model.h describes: every vertex follows two of the
// `bones` of the draw call.
struct SkinDraw {
	// boneCount matrices of 12 floats (3 rows of 4): rest position in model
	// space -> position relative to `origin`.
	const float* bones;
	u32 boneCount;   // at most MODEL_PALETTE
	float origin[3]; // where the model stands in the world
	float posScale, uvScale;
	float light[3];  // colour the character is lit with, time of day included
};
// Uses the material set with setMaterial. PASS_WORLD only.
void drawSkinned(Mesh* m, u32 firstIndex, u32 indexCount, const SkinDraw& skin);
void frameEnd();

// Destroyed textures and meshes may stay allocated until the GPU has finished
// the frame that last used them. This waits for that and frees them now. Call
// it outside a frame, before allocating a lot at once (a world switch).
void releaseDestroyed();

MemStats memStats();
FrameStats frameStats();

} // namespace gfx
} // namespace g3
