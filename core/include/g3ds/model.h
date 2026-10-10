// G3MD / G3AN: animated characters.
//
// A model ("model/<name>") is a skeleton, a skinned mesh cut into parts (one
// draw call each), a table of the animation clips baked for it and a table
// of the blends its model script defines between them:
//   ModelHeader | ModelNode[nodeCount] | ModelPart[partCount] |
//   ModelClipRef[clipCount] | ModelBlend[blendCount] |
//   WorldVertex[vertexCount] | u16[indexCount]
// A clip ("anim/<model>/<clip>") is a table of sampled node transforms, and
// for a clip that carries the character somewhere, where it carries it:
//   ClipHeader | u8 trackNode[trackCount] (padded to 4) |
//   ClipSample[frameCount * trackCount]  (frame after frame) |
//   i16 motion[frameCount * 3]           (only with CLIPF_MOVES; padded to 4)
//   ClipFight                            (only with CLIPF_FIGHT)
//
// Root motion, as in ZenGin: an animation's root node travels (a run moves
// forward, a jump rises), and the game moves the character by what the root
// does from frame to frame instead of drawing the root there. So the root's
// horizontal position is taken out of the samples altogether - the body is
// always drawn over the character's position - and its travel since the
// first frame is kept as the motion table (x, y, z per frame, * motionScale).
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
constexpr u32 MODEL_VERSION = 2;
constexpr u32 CLIP_VERSION = 2;

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
	u32 blendCount;
	u32 reserved;
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

// "aniBlend" of the model script: going from one animation to another is not
// an animation of its own but a fade of this length. Named like a transition
// ("T_RUNL_2_WALKL"), which is how it is looked up.
struct ModelBlend {
	u32 nameHash;
	u32 nextHash; // the animation it leads to
	float time;   // seconds
	u32 reserved;
};
static_assert(sizeof(ModelBlend) == 16, "ModelBlend layout");

enum ClipFlags : u16 {
	CLIPF_LOOP = 1 << 0,  // the clip follows itself
	CLIPF_MOVES = 1 << 1, // the root travels: a motion table follows the samples
	CLIPF_FLY = 1 << 2,   // the script marks it as airborne ("F")
	CLIPF_FIGHT = 1 << 3, // a ClipFight block follows: the frames that matter in a fight
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
	u32 next;          // name hash of the clip that follows this one; 0 for none
	float moveSpeed;   // cm/s the character travels horizontally, on average
	float posScale;    // position = sample * posScale
	float motionScale; // motion = table entry * motionScale (cm)
	u32 reserved;
};
static_assert(sizeof(ClipHeader) == 48, "ClipHeader layout");

// A node's transform relative to its parent. A root node's x and z are 0
// (see "root motion" above); its height stays.
struct ClipSample {
	i16 pos[3];
	i16 rot[4]; // x y z w, * CLIP_ROT_SCALE
};
static_assert(sizeof(ClipSample) == 14, "ClipSample layout");

// What the model script's event tags say about a clip in a fight, in frames of
// the clip (0 = its first). Unused entries are 0xFFFF.
//  * hit: where a blow lands (DEF_OPT_FRAME) - one per blow of a combo;
//  * hitEnd: where each blow ends (DEF_HIT_END): an attack that is not
//    carried on stops there;
//  * window: pairs of frames (DEF_WINDOW), one pair per blow: pressing
//    attack again between them goes on with the next blow, which begins
//    where this one ends;
//  * parry: from and to (DEF_PAR_FRAME): blows that land meanwhile are fended off;
//  * fightMode: where the weapon goes from the belt into the hand or back
//    (SET_FIGHT_MODE), in the animations of drawing and putting away.
constexpr u32 CLIP_FIGHT_BLOWS = 6;
constexpr u16 CLIP_NO_FRAME = 0xFFFF;
struct ClipFight {
	u8 hits, hitEnds, windows, pad;
	u16 hit[CLIP_FIGHT_BLOWS];
	u16 hitEnd[CLIP_FIGHT_BLOWS];
	u16 window[CLIP_FIGHT_BLOWS * 2];
	u16 parry[2];
	u16 fightMode;
	u16 reserved;
	u32 reserved2;
};
static_assert(sizeof(ClipFight) == 64, "ClipFight layout");

inline u32 clipTrackTableSize(u32 trackCount) { return (trackCount + 3u) & ~3u; }

// Where the samples end (the motion table starts there, 4-byte aligned).
inline u32 clipSamplesEnd(u32 trackCount, u32 frameCount) {
	return (u32(sizeof(ClipHeader)) + clipTrackTableSize(trackCount) + trackCount * frameCount * u32(sizeof(ClipSample)) + 3u) & ~3u;
}

inline u32 clipBlobSize(u32 trackCount, u32 frameCount, bool motion, bool fight = false) {
	return clipSamplesEnd(trackCount, frameCount) + (motion ? (frameCount * 6u + 3u) & ~3u : 0u) + (fight ? u32(sizeof(ClipFight)) : 0u);
}

inline u32 modelBlobSize(const ModelHeader& h) {
	return u32(sizeof(ModelHeader)) + h.nodeCount * u32(sizeof(ModelNode)) + h.partCount * u32(sizeof(ModelPart)) +
	       h.clipCount * u32(sizeof(ModelClipRef)) + h.blendCount * u32(sizeof(ModelBlend)) + h.vertexCount * 16u +
	       ((h.indexCount * 2u + 3u) & ~3u);
}

// "meta/camera": the game's own camera set-ups (its CAMERA.DAT), baked so the
// view behaves as it does there. Ranges in metres, angles in degrees, as the
// scripts have them.
constexpr u32 CAMERA_MAGIC = fourcc('G', '3', 'C', 'M');
constexpr u32 CAMERA_VERSION = 1;

struct CameraMode {
	float bestRange, minRange, maxRange;
	float bestElevation, minElevation, maxElevation;
	float bestAzimuth, minAzimuth, maxAzimuth;
	float rotOffsetX, rotOffsetY;
	float targetOffsetX, targetOffsetY, targetOffsetZ;
	float veloTrans, veloRot;
};
static_assert(sizeof(CameraMode) == 64, "CameraMode layout");

enum CameraModeId : u32 { CAMERA_NORMAL, CAMERA_SWIM, CAMERA_FALL, CAMERA_MODE_COUNT };

struct CameraSet {
	u32 magic;
	u32 version;
	u32 modeCount;
	u32 fromGame; // 1: read from the game's scripts; 0: this project's defaults
	CameraMode mode[CAMERA_MODE_COUNT];
};
static_assert(sizeof(CameraSet) == 16 + 64 * CAMERA_MODE_COUNT, "CameraSet layout");

// What is used when the game's camera scripts cannot be read.
inline CameraSet defaultCameraSet() {
	CameraSet s {};
	s.magic = CAMERA_MAGIC;
	s.version = CAMERA_VERSION;
	s.modeCount = CAMERA_MODE_COUNT;
	s.fromGame = 0;
	const CameraMode normal = {2.5f, 1.4f, 10.0f, 25.0f, 0.0f, 89.0f, 0.0f, -90.0f, 90.0f, 20.0f, 0.0f, 0.0f, 0.0f, 0.0f, 40.0f, 2.0f};
	for (CameraMode& m : s.mode) m = normal;
	return s;
}

} // namespace g3
