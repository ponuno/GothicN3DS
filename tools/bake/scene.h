// Importer-neutral scene description. Every importer (ZenGin world, synthetic
// test scene) fills one of these; the chunker and texture baker never see
// where the data came from.
#pragma once
#include "g3ds/base.h"

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
	u8 detail = 0; // DetailClass (bits 0-1): how far away the triangle is still drawn; bits 2-3: its thinning layer (small objects only)
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

// A ladder: the box of the ladder itself.
struct SrcLadder {
	float bbMin[3], bbMax[3];
};

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

// A particle effect the world carries (a torch, a fire, a smoke): the script
// instance's name (upper case), where, and the way its carrier faces.
struct SrcEmitter {
	std::string effect;
	float pos[3];
	float yaw = 0;
};

struct SrcImage {
	int width = 0, height = 0;
	std::vector<u8> rgba; // top-down
};

struct SrcScene {
	std::vector<SrcMaterial> materials;
	std::vector<SrcTriangle> triangles;
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
	std::vector<SrcEmitter> emitters;
	std::vector<SrcWorldItem> items; // the world file's things (the scripts add theirs when baked)
	std::vector<std::string> notes; // importer remarks for the bake report
};

} // namespace g3
