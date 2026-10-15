// Where grass grows in a world: the density map shared by the grass report (grass_report.h, --grass-report) and the grass
// itself (grass_bake.h, --grass). A grid of cells (1 m by default) over the world seen from above; each cell has the
// ground under it (the topmost polygon of the world mesh that faces up and can be stood on) and a density 0..1, the
// product of these factors:
//   texture  the weight of the ground polygon's texture: from its average colour (green), its material group (earth +,
//            stone -) and its name (GRAS, WIESE, MOOS ... +; ROAD, WEG, STEIN, FELS ... -), or the rules file
//   slope    1 up to 25 degrees, 0 from 40
//   water    0 under water, half at the very edge, more 1..4 m from it
//   rooms    0 inside the world's rooms (houses, caves that are sectors)
//   cover    0 under an object that stands on the ground (a house, a rock, a fence): what is up to 8 m above it; and
//            under an overhang of the world mesh itself (a polygon facing down less than 8 m above the ground)
//   paths    a quarter on the way net (the paths people walk), most of it within 2 m
//   trees    more in a ring about each tree (none at the trunk), a little more about bushes
//   noise    clearings and thickets over ~12 m, so that it is not an even carpet
// Polygons one walks through (leaves, foliage cards of the world mesh: no collision) and see-through ones are not ground
// (fix147: before, a canopy of leaves in the world mesh hid the ground under it and the report showed holes there).
#pragma once
#include "scene.h"

#include <string>
#include <vector>

namespace g3 {

struct GrassMapOptions {
	std::string rulesFile; // --grass-rules: "TEXTURE weight" per line (a trailing * matches every name that begins so)
	float cellCm = 100.0f; // the map's cell (made coarser for a big world: at most 2048 cells across)
};

// A texture of the world mesh, as the map sees it.
struct GrassTexStat {
	std::string key;   // the texture's name, upper case ("(colour) NAME" for a material without one)
	std::string texture;
	u16 group = 0;
	double area = 0;   // m², all of its world triangles
	double upArea = 0; // m², facing up (more than 11 degrees above the vertical)
	double flatArea = 0; // m², up to 25 degrees
	double lightSum = 0, lightWeight = 0;
	double topArea = 0; // m², the cells of the map where it is the topmost ground
	bool hasColour = false;
	float colour[3] = {0, 0, 0};
	float weight = 0;
	bool ruled = false;
	std::string why;
};

struct GrassMap {
	static constexpr float NONE = -1e30f;
	// The grid: cell (x, z) covers [x0 + x * cell, x0 + (x + 1) * cell) and likewise on z.
	float x0 = 0, z0 = 0, cell = 100;
	int w = 0, h = 0;
	size_t at(int x, int z) const { return size_t(z) * size_t(w) + size_t(x); }
	bool cellOf(float x, float z, int& cx, int& cz) const;
	float cx(int x) const { return x0 + (float(x) + 0.5f) * cell; }
	float cz(int z) const { return z0 + (float(z) + 0.5f) * cell; }
	size_t cells() const { return size_t(w) * size_t(h); }
	double cellM2() const { return double(cell) * double(cell) / 10000.0; }

	float upSign = 1.0f;      // which way the world mesh's "up" winds (normals times this face up)
	size_t worldTris = 0;     // how many of the scene's triangles are the world mesh
	std::vector<GrassTexStat> texs;
	std::vector<int> triTex;  // per world triangle: its entry in texs
	size_t coloured = 0;      // textures whose average colour was read
	std::string rulesNote;

	// Per cell.
	std::vector<float> groundY;   // the ground's height (NONE: no ground: a wall, nothing, under water only)
	std::vector<int> groundTri;   // the world triangle that is the ground there (-1: none)
	std::vector<int> groundTex;   // its entry in texs (-1: none)
	std::vector<float> slope;     // degrees
	std::vector<float> waterY;    // the highest water surface (NONE: none)
	std::vector<float> waterDist; // cells to the nearest water (0: under water)
	std::vector<float> texW, slopeF, waterF, roomF, coverF, pathF, plantF, noiseF; // the factors
	std::vector<float> density;   // 0..1
	size_t groundCells = 0, underwater = 0;

	// Statistics of the factors: what is left, m² of full density, after each in turn ([8]: the density itself).
	double sums[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
	u32 trees = 0, bushes = 0, grasses = 0;

	// The density at a world point (0 outside the map).
	float densityAt(float x, float z) const {
		int a, b;
		return cellOf(x, z, a, b) ? density[at(a, b)] : 0.0f;
	}
};

// Builds the map of a world (an outdoor one with a world mesh; for others `ok` is false and `why` says why).
GrassMap buildGrassMap(const SrcScene& scene, const GrassMapOptions& opt, bool& ok, std::string& why);

// The normal of a triangle (unit, as wound).
void grassTriangleNormal(const SrcTriangle& t, float n[3]);

} // namespace g3
