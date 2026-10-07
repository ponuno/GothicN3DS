// Importer-neutral scene description. Every importer (ZenGin world, synthetic
// test scene) fills one of these; the chunker and texture baker never see
// where the data came from.
#pragma once
#include "g3ds/base.h"

#include <functional>
#include <string>
#include <vector>

namespace g3 {

struct SrcVertex {
	float pos[3];
	float uv[2]; // v = 0 is the top of the image (Direct3D convention)
	u8 rgba[4];  // baked static light
};

struct SrcTriangle {
	SrcVertex v[3]; // counter-clockwise when seen from the front
	u32 material;
};

struct SrcMaterial {
	std::string name;
	std::string texture; // logical texture name; resolved through SrcScene::loadTexture
	u16 group = 0;
	bool water = false;
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
	float spawnPos[3] = {0, 0, 0};
	float spawnYaw = 0;
	bool hasSpawn = false;
	std::vector<std::string> notes; // importer remarks for the bake report
};

} // namespace g3
