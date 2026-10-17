// Importer-neutral scene description. Every importer (ZenGin world, synthetic
// test scene) fills one of these; the chunker and texture baker never see
// where the data came from.
#pragma once
#include "g3ds/base.h"

#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace g3 {

struct SrcVertex {
	float pos[3];
	float uv[2]; // v = 0 is the top of the image (Direct3D convention)
	u8 rgba[4];  // rgb: baked static light; a: daylight weight (255 outdoors, 0 indoors)
	u8 occlusion = 0; // ambient occlusion, 0 open .. 255 shut in (vertex_fx.h; baked with --ao)
	u8 wind = 0;      // how far it sways, WORLD_WIND_UNIT steps (0..127; the importer marks plants, kept with --wind)
};

struct SrcTriangle {
	SrcVertex v[3]; // counter-clockwise when seen from the front
	u32 material;
	u8 detail = 0; // DetailClass (bits 0-1): how far away the triangle is still drawn; bits 2-3: its thinning layer, 4-5: its size band (small objects only; g3ds/world.h sizeBandOf)
};

enum class SrcBlend : u8 {
	Opaque, // cut-out alpha is detected from the texture
	Alpha,  // colour * alpha + background * (1 - alpha)
	Add,    // colour * alpha + background
};

struct SrcMaterial {
	std::string name;
	// Logical texture name; resolved through SrcScene::loadTexture. A name
	// containing "_A0" is the first frame of an animation ("_A1", "_A2", ...).
	std::string texture;
	u16 group = 0;
	bool water = false;       // a water surface: blended, no collision, swimmable
	bool noCollision = false; // the player passes through (foliage, water, movers)
	SrcBlend blend = SrcBlend::Opaque;
	// The material's own colour: what it is drawn with if it has no texture, or
	// the texture cannot be found (as the game does).
	u8 color[3] = {0, 0, 0};
	bool hasColor = false;
	u8 opacity = 255;         // alpha multiplier of a blended material
	float animFps = 0;        // texture animation speed; 0 picks a default
	float scroll[2] = {0, 0}; // texture scrolling, repeats per second
};

// A box that sends the player to another world.
struct SrcPortal {
	float bbMin[3], bbMax[3];
	std::string level; // target world file name, e.g. "OLDMINE.ZEN"
	std::string point; // name of the arrival point in the target world
};

// A ladder: the box of the ladder itself, and - when its model could be read - the ladder as the model has it
// (world/<name>/ladderinfo, g3ds/world.h LadderInfo): the middle of its rungs at the foot and at the head, the way
// its face looks, the side it is climbed from (+1 / -1 along `normal`, 0: not known), half its width and depth.
struct SrcLadder {
	float bbMin[3], bbMax[3];
	bool hasInfo = false;
	float foot[3] = {0, 0, 0}, head[3] = {0, 0, 0};
	float normal[2] = {0, 1};
	float halfWidth = 0, halfDepth = 0;
	int side = 0;
};

// Fills the ladder's model part from its model: the model's own box (its space: up is +y; the rungs run across
// the wider of x and z, the narrower is the way through it), the places one stands at to use it ("ZS_POS..."
// nodes, model space; the side they are on is the side it is climbed from) - placed by the columns of the
// object's rotation (`col[k]` is where the model's axis k goes) and its position.
inline void ladderFromModel(SrcLadder& l, const float lo[3], const float hi[3], const float (*slots)[3], size_t slotCount, const float col[3][3],
                            const float pos[3]) {
	const auto place = [&](const float p[3], float out[3]) {
		for (int a = 0; a < 3; ++a) out[a] = col[0][a] * p[0] + col[1][a] * p[1] + col[2][a] * p[2] + pos[a];
	};
	const float ex = hi[0] - lo[0], ez = hi[2] - lo[2];
	if (!(hi[1] - lo[1] > 50.0f) || !(ex >= 0.0f) || !(ez >= 0.0f)) return;
	const bool acrossX = ex >= ez; // the rungs run along x: the face looks along z
	const float cx = 0.5f * (lo[0] + hi[0]), cz = 0.5f * (lo[2] + hi[2]);
	const float footL[3] = {cx, lo[1], cz}, headL[3] = {cx, hi[1], cz};
	place(footL, l.foot);
	place(headL, l.head);
	if (l.head[1] <= l.foot[1] + 50.0f) return; // (a ladder lying down, or upside down: the box will do)
	const int through = acrossX ? 2 : 0;
	float nx = col[through][0], nz = col[through][2];
	const float len = std::sqrt(nx * nx + nz * nz);
	if (len < 0.3f) return; // (the way through it points up: not a ladder one climbs)
	nx /= len;
	nz /= len;
	l.normal[0] = nx;
	l.normal[1] = nz;
	l.halfWidth = 0.5f * (acrossX ? ex : ez);
	l.halfDepth = 0.5f * (acrossX ? ez : ex);
	int votes = 0;
	for (size_t k = 0; k < slotCount; ++k) {
		const float d = slots[k][through] - (acrossX ? cz : cx);
		if (std::fabs(d) > 5.0f) votes += d > 0.0f ? 1 : -1;
	}
	l.side = votes > 0 ? 1 : votes < 0 ? -1 : 0;
	l.hasInfo = l.halfWidth > 0.0f;
}

struct SrcPoint {
	float pos[3]; // on the ground
	float yaw;
};

struct SrcWayPoint {
	std::string name; // upper case
	float pos[3];
	float yaw;
};

// A thing the world's people use: a bed, a bench (see g3ds/game.h).
struct SrcMobSlot {
	std::string name; // the model's node: "ZS_POS0", "ZS_POS1_FRONT", "ZS_POS0_DIST"
	float pos[3];
	float yaw;
};

struct SrcMob {
	std::string scheme; // upper case: "BEDHIGH"
	std::string visual; // for the report
	float pos[3];
	float yaw;
	int states = 1;
	std::vector<SrcMobSlot> slots;
	// A container (oCMobContainer).
	bool container = false;
	bool locked = false;
	std::string key;      // upper case item instance, "" for none
	std::string pick;     // "LRRL"
	std::string contents; // "ITMI_GOLD:75, ITFO_FISH:2"
	std::string vobName;  // fix199: the vob's own name (the scripts' Mob_CreateItems name it), upper case
	// What the player may do with it (WorldMobInfo).
	std::string focus, onState, condition, useItem, target; // names as the world has them (script names upper case)
	std::string owner, ownerGuild;                          // whose it is (upper case), "" for nobody's
	int kind = 0;                                          // MobKind
	bool rewind = false;
};

// A thing lying about (g3ds/game.h, WorldItem).
struct SrcWorldItem {
	std::string instance; // the item's script instance, upper case: "ITFOAPPLE"
	float pos[3];
	float rot[4] = {0, 0, 0, 1}; // quaternion: model space -> world
	int amount = 1;
};

// A sound the world has placed in itself (zCVobSound): a campfire's crackle,
// water, the wind (g3ds/sound.h, AmbientSound).
struct SrcAmbient {
	std::string sound; // the script instance's name
	float pos[3];
	float radius = 1500.0f;
	int mode = 0;      // AmbientMode
	float volume = 1.0f; // 0..1
	float hourFrom = 0, hourTo = 0; // equal: all day
	float delayMin = 0, delayMax = 0;
};

// A music zone (zCZoneMusic), g3ds/sound.h MusicZone.
struct SrcMusicZone {
	std::string name; // the theme's base name ("OC": OC_DAY_STD ...)
	float bbMin[3], bbMax[3];
	int priority = 0;
	bool ellipsoid = false;
	bool isDefault = false;
	float volume = 1.0f;
};

// A fog zone (zCZoneZFog), g3ds/fog.h FogZone.
struct SrcFogZone {
	float bbMin[3], bbMax[3];
	float range = 0;  // cm
	float inner = 0;  // 0..1
	u8 color[3] = {0, 0, 0};
	bool overrideColor = false, fadeSky = false;
};

// A particle effect the world carries (a torch, a fire, a smoke): the script
// instance's name (upper case), where, and the way its carrier faces.
struct SrcEmitter {
	std::string effect;
	float pos[3];
	float yaw = 0;
};

// A light the world places (zCVobLight: a torch, a fire place, a lamp): where, how far it reaches, its colour.
// `isStatic`: the world's own compiled light already has it (the others - the flickering ones of torches and fires - are not in it).
struct SrcLight {
	float pos[3];
	float range = 0;
	u8 color[3] = {255, 255, 255};
	bool isStatic = false;
	// fix148: what the runtime needs to make it flicker (g3ds/lamps.h). ZenGin's rangeAniScale (shares of the range) and
	// colorAniList, played at their fps, blended or stepped.
	std::vector<float> rangeKeys;
	float rangeFps = 0;
	bool rangeSmooth = true;
	std::vector<std::array<u8, 3>> colorKeys;
	float colorFps = 0;
	bool colorSmooth = true;
	bool fire = false; // of a fire place (its tree file), or made for one
	bool made = false; // made by the baker (a fire place whose tree has no light)
};

struct SrcImage {
	int width = 0, height = 0;
	std::vector<u8> rgba; // top-down
};

// A trigger of the world (zCTrigger, zCTriggerList, oCTriggerScript, zCTriggerWorldStart) or a mover (zCMover):
// g3ds/world.h, "triggers".
struct SrcTrigger {
	int kind = 0;           // TriggerKind
	std::string name;       // the object's name: what events are sent to (upper case)
	std::string target;     // what it sends on (upper case); a script trigger: the function
	std::string visual;     // a mover: its mesh, lower case without extension ("model/item.<visual>")
	float bbMin[3] = {0, 0, 0}, bbMax[3] = {0, 0, 0};
	bool startEnabled = true, sendUntrigger = false, reactTrigger = true, reactTouch = false, respondPc = false, respondNpc = false;
	bool fireOnce = false, autoRotate = false;
	int maxActivations = -1;
	float retriggerDelay = 0, fireDelay = 0;
	// A mover: its keyframes as world transforms (position, rotation as a unit quaternion x y z w rotating column
	// vectors), how it behaves and moves.
	std::vector<std::array<float, 7>> keys;
	int behavior = 0, lerp = 0, speedMode = 0;
	float speed = 0, stayOpen = 0;
	std::string sfxOpenStart, sfxOpenEnd, sfxCloseStart, sfxCloseEnd, sfxMoving;
	// A mover or a door that collides (cd_static / cd_dynamic): its triangles in its own space, 9 floats each.
	std::vector<float> collision;
	// A door (oCMobDoor): where it turns (its own space), locked, the key that opens it, the lock's combination.
	float pivot[3] = {0, 0, 0};
	bool locked = false;
	std::string key, pick;
	// A camera's flight (TRG_CAMERA): `keys` are its positions, then the last camTargets of them the points it looks at
	// ({x, y, z, time, motion, 0, 1}); speed: its seconds, stayOpen: the delay of its own end, behavior: its bits.
	int camTargets = 0;
	// A list: targets and their delays.
	std::vector<std::pair<std::string, float>> list;
	int listMode = 0;
};

// A plant object of the world (a tree, a bush, a tuft of grass), as the importer recognised it (vertex_fx.h PlantKind):
// for the grass report (grass_report.h), which grows more grass about trees and finds where the old grass stands.
struct SrcPlant {
	float pos[3];      // the object's origin
	float footY = 0, topY = 0; // the lowest and highest point of its triangles
	float radius = 0;  // the mesh's radius
	u8 kind = 0;       // PlantKind: 1 tree, 2 bush, 3 grass
	std::string name;  // its visual (upper case)
};

struct SrcScene {
	std::vector<SrcMaterial> materials;
	std::vector<SrcTriangle> triangles;
	// How many of `triangles`, from the first, are the world mesh itself (the ground, the rocks, the caves; the rest are
	// its objects). 0: not known (every triangle counts as the world's).
	size_t worldTriangles = 0;
	std::vector<SrcPlant> plants;
	// Returns false if the texture does not exist. Called from worker threads
	// under a lock, so it does not have to be thread-safe.
	std::function<bool(const std::string& name, SrcImage& out)> loadTexture;
	float spawnPos[3] = {0, 0, 0}; // eye position
	float spawnYaw = 0;
	bool hasSpawn = false;
	// A world without sky (a mine): no day and night, `fogColor` as background.
	bool indoor = false;
	u8 fogColor[3] = {10, 10, 14};
	std::vector<SrcPortal> portals;
	std::vector<SrcLadder> ladders;
	std::map<std::string, SrcPoint> points; // named places (upper case): arrival points of portals
	// The way net: its points, the connections between them (pairs of
	// indices), and the free points beside it.
	std::vector<SrcWayPoint> wayPoints;
	std::vector<std::pair<u32, u32>> wayEdges;
	std::vector<SrcWayPoint> freePoints;
	std::vector<SrcMob> mobs;
	std::vector<SrcAmbient> ambient;
	std::vector<SrcMusicZone> musicZones;
	std::vector<SrcFogZone> fogZones; // regions with their own fog and water tint (not the default zone: that is fogColor)
	std::vector<SrcEmitter> emitters;
	std::vector<SrcLight> lights;
	std::vector<SrcWorldItem> items; // the world file's things (the scripts add theirs when baked)
	// The rooms (BSP sectors): their names (upper case) and the boxes of their leaves; `room` indexes roomNames.
	struct RoomBoxSrc {
		float bbMin[3], bbMax[3];
		u32 room;
	};
	std::vector<std::string> roomNames;
	std::vector<SrcTrigger> triggers;
	u32 moverConjugated = 0, moverPlain = 0; // how the movers' key rotations read (for the report)
	u32 cameraObjectRelative = 0; // camera flights left out: keyed in an object's coordinates (fix152)
	std::vector<RoomBoxSrc> roomBoxes;
	std::vector<std::string> notes; // importer remarks for the bake report
};

} // namespace g3
