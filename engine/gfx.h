// Rendering backend interface. Two implementations:
//   platform/3ds/gfx_c3d.cpp   - PICA200 through citro3d
//   platform/host/gfx_soft.cpp - software rasteriser that mimics the same
//                                fixed-function path (texture * vertex colour,
//                                alpha test, per-vertex linear fog) for PC testing.
// The engine never touches platform APIs directly.
#pragma once
#include "g3ds/math.h"
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
	float fovY;
	float zNear, zFar;
	// Linear fog on view-space depth, evaluated per vertex (like ZenGin's):
	// factor = clamp((fogEnd - z) / (fogEnd - fogStart), 0, 1), 1 = no fog.
	float fogStart, fogEnd;
	u8 fogColor[3];
	u8 clearColor[3];
};

struct MemStats {
	u32 meshBytes;    // vertex + index buffers (linear heap on 3DS)
	u32 textureBytes; // texture memory
	u32 meshCount;
	u32 textureCount;
};

bool init();
void shutdown();

// `data` is the PICA-native mip chain that follows the TexHeader.
Texture* textureCreate(const TexHeader& h, const u8* data);
void textureDestroy(Texture* t);

Mesh* meshCreate(const WorldVertex* vertices, u32 vertexCount, const u16* indices, u32 indexCount);
void meshDestroy(Mesh* m);

void frameBegin(const FrameDesc& f);
// Sets the dequantisation transform for the following draws.
void setChunkTransform(const float origin[3], float posScale, float uvScale);
// materialFlags: MaterialFlags from world.h. A null texture draws white.
void setMaterial(Texture* t, u32 materialFlags);
void draw(Mesh* m, u32 firstIndex, u32 indexCount);
void frameEnd();

MemStats memStats();

} // namespace gfx
} // namespace g3
