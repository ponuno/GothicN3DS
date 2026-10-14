// Water cut finely where waves are seen (g3ds-bake --water-grid): along the shores, in cells of a few metres, with a
// weight per corner that fades the waves out towards the coarse water beyond. See WaterGrid.
#pragma once
#include "g3ds/base.h"
#include "ground.h"
#include "scene.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace g3 {

struct WaterGridOptions {
	float cell = 0.0f;      // cm, the grid's cell (lines through every multiple of it, x and z); 0: nothing is cut
	int cellsPerTile = 16;  // a tile is this many cells on an edge: the unit of "near a shore"
	int reachTiles = 1;     // water this many tiles from a shore tile is cut as well (at least 1)
	float fade = 2400.0f;   // cm: the waves' weight rises from 0 at the edge of the cut water to 1 this far in (<= a tile)
	// cm: water narrower than this (rivers, ponds) is left calm. Its "openness" - the greatest distance from a bank
	// within 0.8 x this of a point - decides: below a quarter of it no waves, above 0.6 of it the full ones. 0: off.
	float riverWidth = 0.0f;
	// 0..1: the water's colour by depth and the foam at the shore, in the fine water's vertices (0: off). Shallow water is
	// lighter and greener than the deep (which is the colour the coarse water has, so the two meet); where the water is
	// less than 50 cm deep, or the ground over it is within 3 m of the level, it fades towards white.
	float shore = 0.0f;
};

struct WaterGridStats {
	u64 waterIn = 0;       // water triangles given
	u64 waterOut = 0;      // and what they became
	u32 shoreTiles = 0;    // tiles where the ground meets the water
	u32 fineTiles = 0;     // tiles cut into cells (the shore tiles and their neighbours)
};

// The water (every triangle of a material with `water`) is cut in two scales. Everything lies on one lattice of tiles; a
// tile near a shore (where ground or an object crosses the water's level, plus the tiles `reachTiles` around those) is cut
// along the cell lines; the water of the other tiles is left in big pieces. Every corner of a fine piece carries in
// SrcVertex::wind how strongly the waves lift it (0..127): 0 at the border of the fine water and in all the coarse water,
// up to 127 from `fade` cm inside. The runtime lifts a corner by its weight, so the fine water meets the coarse one flat.
// (Where the lattice's tiles hold no water at all nothing is made.) The mesh is not sealed here: the chunker seals the
// T-junctions between big and small pieces, and the pieces on both sides of such an edge are flat there.
class WaterGrid {
public:
	WaterGrid(const SrcScene& scene, const WaterGridOptions& opt);
	bool active() const { return _active; }
	// Appends the pieces of one water triangle to `out` (a triangle in one cell and one tile stays as it is, but for its
	// weights).
	void cut(const SrcTriangle& t, std::vector<SrcTriangle>& out);
	const WaterGridStats& stats() const { return _stats; }

private:
	using Poly = std::vector<SrcVertex>;
	static uint64_t key(int ix, int iz) { return (uint64_t(uint32_t(ix + (1 << 30))) << 32) | uint32_t(iz + (1 << 30)); }
	int tileOf(float v) const;
	int clampTile(int axis, int t) const { return t < _lo[axis] ? _lo[axis] : t > _hi[axis] ? _hi[axis] : t; }
	bool fine(int ix, int iz) const { return _fine.count(key(ix, iz)) != 0; }
	float weightAt(float x, float z) const;
	float openness(float x, float z) const;
	void buildOpenness(const SrcScene& scene, const std::unordered_map<uint64_t, std::pair<float, float>>& level,
	                   const std::unordered_set<uint64_t>& shore);
	void emit(const Poly& poly, bool dense, std::vector<SrcTriangle>& out, const SrcTriangle& proto);
	void cutTile(const Poly& poly, int ix, int iz, std::vector<SrcTriangle>& out, const SrcTriangle& proto);

	void shoreColour(SrcVertex& v) const;
	GroundSampler _ground;
	WaterGridOptions _opt;
	bool _active = false;
	float _tile = 0.0f;
	int _lo[2] = {0, 0}, _hi[2] = {0, 0}; // the tiles the lattice covers (the outermost reach out for ever)
	std::unordered_set<uint64_t> _fine;
	std::unordered_set<uint64_t> _fineSuper; // 8 x 8 tiles
	std::unordered_set<uint64_t> _water;     // the tiles with water in them
	std::unordered_map<uint64_t, float> _open; // per cell of the fine water: its openness (cm), see riverWidth
	WaterGridStats _stats;
};

} // namespace g3
