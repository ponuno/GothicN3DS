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
};

struct SrcTriangle {
	SrcVertex v[3]; // counter-clockwise when seen from the front
	u32 material;
	u8 detail = 0; // DetailClass: how far away the triangle is still drawn
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
	std::vector<std::string> notes; // importer remarks for the bake report
};

} // namespace g3
