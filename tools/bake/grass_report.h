// The grass report (g3ds-bake --grass-report DIR): where new grass could grow in a world, for testing the grass system
// before it is built. It changes nothing in data.pak; it only looks at the imported world and writes, per outdoor world:
//   grass_<world>.txt        the ground's textures by area (with their slope, light, average colour and the weight the
//                            report gave them), what the density map came to, and what the grass would cost
//   grass_<world>.png        the density map seen from above: dark grey relief where nothing grows, green -> yellow ->
//                            red as the grass gets denser, blue water, dark dots for trees, white for the old grass objects
//   grass_<world>_mask.png   the texture weights alone (before slope, water, paths, trees ...): to check the rules
//   grass_<world>_rules.txt  the weights as a list to edit and give back with --grass-rules FILE
//
// How a place gets its density (0..1), each a factor:
//   texture  the weight of the topmost ground polygon's texture: from its average colour (green), its material group
//            (earth +, stone -) and its name (GRAS, WIESE, MOOS ... +; ROAD, WEG, STEIN, FELS ... -), or the rules file
//   slope    1 up to 25 degrees, 0 from 40
//   water    0 under water, half at the very edge, more 1..4 m from it
//   rooms    0 inside the world's rooms (houses, caves that are sectors)
//   cover    0 under an object that stands on the ground (a house, a rock, a fence): what is up to 8 m above it
//   paths    a quarter on the way net (the paths people walk), most of it within 2 m
//   trees    more in a ring about each tree (none at the trunk), a little more about bushes
//   noise    clearings and thickets over ~12 m, so that it is not an even carpet
#pragma once
#include "scene.h"

#include <string>

namespace g3 {

struct GrassReportOptions {
	std::string dir;       // where the files go ("" or "."; the folder must exist)
	std::string rulesFile; // --grass-rules: "TEXTURE weight" per line (a trailing * matches every name that begins so)
	float cellCm = 100.0f; // the map's cell (made coarser for a big world: at most 2048 cells across)
};

// Writes the files for one world; returns a line or two for the bake's log (or why nothing was written).
std::string writeGrassReport(const std::string& worldName, const SrcScene& scene, const GrassReportOptions& opt);

} // namespace g3
