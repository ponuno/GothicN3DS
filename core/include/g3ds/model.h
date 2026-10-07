// G3MD / G3AN: animated characters.
//
// A model ("model/<name>") is a skeleton, a skinned mesh cut into parts (one
// draw call each) and a table of the animation clips baked for it:
//   ModelHeader | ModelNode[nodeCount] | ModelPart[partCount] |
//   ModelClipRef[clipCount] | WorldVertex[vertexCount] | u16[indexCount]
// A clip ("anim/<model>/<clip>") is a table of sampled node transforms:
//   ClipHeader | u8 trackNode[trackCount] (padded to 4) |
//   ClipSample[frameCount * trackCount]  (frame after frame)
//
// Skinning: every vertex follows at most two nodes. The vertex has the layout
// of WorldVertex, so it goes through the same vertex buffers and attribute
// setup as the world; the colour bytes carry the skinning instead of light:
//   x, y, z   rest position in model space (* posScale)
//   u, v      texture coordinate (* uvScale)
//   r, g      first and second bone: slot in the part's palette, times 3
//             (the vertex shader indexes rows of 3x4 matrices with them)
//   b         weight of the second bone, 0..255
//   a         shade baked from the rest-pose normal, 0..255
// A part's palette lists the nodes its vertices use (at most MODEL_PALETTE:
// what fits into the PICA200's uniform registers next to everything else).
//
// Model space: Y up, the character stands on y = 0 and faces +Z.
#pragma once
#include "g3ds/base.h"

namespace g3 {

constexpr u32 MODEL_MAGIC = fourcc('G', '3', 'M', 'D');
constexpr u32 CLIP_MAGIC = fourcc('G', '3', 'A', 'N');
constexpr u32 MODEL_VERSION = 1;
constexpr u32 CLIP_VERSION = 1;

constexpr u32 MODEL_MAX_NODES = 96;
constexpr u32 MODEL_PALETTE = 26;
constexpr float CLIP_ROT_SCALE = 1.0f / 32767.0f;

struct ModelHeader {
	u32 magic;
	u32 version;
	u16 nodeCount;
	u16 partCount;
	u16 clipCount;
	u16 flags;
	u32 vertexCount;
	u32 indexCount;
	float posScale;
	float uvScale;
	float bbMin[3]; // rest pose
	float bbMax[3];
	u32 reserved[2];
};
static_assert(sizeof(ModelHeader) == 64, "ModelHeader layout");

struct ModelNode {
	i16 parent; // -1 for a root; parents come before their children
	u16 flags;
	u32 nameHash;      // hashName of the node's name ("BIP01 HEAD"): attachment points
	float restPos[3];  // local transform at rest
	float restRot[4];  // x y z w
	float invBind[12]; // model space -> node space at rest: 3 rows of 4
};
static_assert(sizeof(ModelNode) == 84, "ModelNode layout");

struct ModelPart {
	u32 texture; // pak hash of the texture, 0 for none
	u16 flags;   // MaterialFlags (world.h): alpha test
	u16 boneCount;
	u32 firstIndex;
	u32 indexCount;
	u8 bones[MODEL_PALETTE]; // palette slot -> node
	u8 pad[2];
};
static_assert(sizeof(ModelPart) == 44, "ModelPart layout");

struct ModelClipRef {
	u32 nameHash;  // hashName of the clip's name ("S_RUNL")
	u32 entryHash; // pak entry of the clip
};
static_assert(sizeof(ModelClipRef) == 8, "ModelClipRef layout");

enum ClipFlags : u16 {
	CLIPF_LOOP = 1 << 0,  // the clip follows itself
	CLIPF_MOVES = 1 << 1, // the source moved the root forward; moveSpeed says how fast
};

struct ClipHeader {
	u32 magic;
	u32 version;
	u16 trackCount; // animated nodes
	u16 frameCount;
	float fps;
	u16 flags; // ClipFlags
	u16 layer;
	float blendIn;  // seconds to fade in / out, as the model script says
	float blendOut;
	u32 next;        // name hash of the clip that follows this one; 0 for none
	float moveSpeed; // cm/s the character travels while this plays at normal speed
	float posScale;  // position = sample * posScale
	u32 reserved[2];
};
static_assert(sizeof(ClipHeader) == 48, "ClipHeader layout");

// A node's transform relative to its parent. The root's net horizontal travel
// has been taken out (the game moves the character instead); its height and
// its sway on the way stay.
struct ClipSample {
	i16 pos[3];
	i16 rot[4]; // x y z w, * CLIP_ROT_SCALE
};
static_assert(sizeof(ClipSample) == 14, "ClipSample layout");

inline u32 clipTrackTableSize(u32 trackCount) { return (trackCount + 3u) & ~3u; }

inline u32 clipBlobSize(u32 trackCount, u32 frameCount) {
	return u32(sizeof(ClipHeader)) + clipTrackTableSize(trackCount) + trackCount * frameCount * u32(sizeof(ClipSample));
}

inline u32 modelBlobSize(const ModelHeader& h) {
	return u32(sizeof(ModelHeader)) + h.nodeCount * u32(sizeof(ModelNode)) + h.partCount * u32(sizeof(ModelPart)) +
	       h.clipCount * u32(sizeof(ModelClipRef)) + h.vertexCount * 16u + ((h.indexCount * 2u + 3u) & ~3u);
}

} // namespace g3
