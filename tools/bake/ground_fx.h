// Colour of the ground put in the vertices at bake time (no shader, no runtime cost):
//  - wet shore: ground at the water's edge, up to a little above the level, is darker (wet sand);
//  - canopy: ground under trees and bushes is darker, from rays up into the plants' triangles.
// Both multiply the baked static light (rgb) of the ground corners; they are as fine as the ground's mesh is.
#pragma once
#include "scene.h"

#include <vector>

namespace g3 {

struct GroundFxStats {
	u64 wetCorners = 0;
	u64 shadedCorners = 0; // under plants
	u64 rays = 0;
};

// `strength` 0..1: 0 nothing, 1 the ground at the waterline loses ~45 % of its light.
void bakeWetShore(SrcScene& scene, float strength, GroundFxStats& stats);
// `plant[m]`: material m belongs to plants. `strength` 0..1 (1: full shade loses 45 %); `reach` cm.
void bakeCanopy(SrcScene& scene, const std::vector<char>& plant, float strength, float reach, unsigned threads, GroundFxStats& stats);

} // namespace g3
