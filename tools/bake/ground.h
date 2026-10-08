// Answers "what is the ground like under this point?" for the baker: used to
// give world objects the static light of the floor they stand on, the way
// ZenGin lights its static VObs.
#pragma once
#include "scene.h"

#include <vector>

namespace g3 {

class GroundSampler {
public:
	// Indexes the first `count` triangles of the scene (the world mesh). With
	// `solidOnly`, only what can be stood on: no water, no foliage.
	void build(const SrcScene& scene, size_t count, bool solidOnly = false);

	// Finds the highest surface at or below `y` under (x, z) and returns its
	// interpolated vertex colour. False if there is nothing below the point.
	bool sample(float x, float y, float z, u8 rgba[4], float* height = nullptr) const;

private:
	const SrcScene* _scene = nullptr;
	float _minX = 0, _minZ = 0, _cell = 400.0f;
	int _w = 0, _h = 0;
	std::vector<u32> _start; // _w * _h + 1 offsets into _items
	std::vector<u32> _items;
};

} // namespace g3
