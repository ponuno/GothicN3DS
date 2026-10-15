// The new grass (g3ds-bake --grass): tufts of grass sown over a world by its density map (grass_map.h), stored compactly
// by tiles of 16 m (g3ds/world.h G3GR) for the runtime (engine/grass.h) to grow meshes of round the camera.
//
// How a tuft is made: each cell of the map (1 m) gets `density x map density` tufts on average, at places hashed from
// the cell (the same every bake); its foot is on the ground there (the world mesh, as a character would stand), it leans
// with the slope (half of it, at most 15 degrees), has one of four pictures, a size of 0.7..1.3 and a turn; its colour is
// the ground's baked light there (the world's lamps and shade in it, so that it does not glow in the dark) tinted
// towards the colour of the ground's texture. With the sun's shadow baked (--sun-shadow), how much of the sun reaches it.
#pragma once
#include "grass_map.h"
#include "scene.h"
#include "texture_bake.h"

#include <functional>
#include <string>
#include <vector>

namespace g3 {

struct GrassOptions {
	bool enabled = false;      // --grass
	float density = 2.0f;      // --grass-density N: tufts per m² where the map's density is 1
	float tileSize = 1600.0f;  // cm
	float height = 45.0f;      // cm: a tuft at scale 1
	float width = 55.0f;       // cm: each of its two cards
	bool replaceOld = false;   // --grass-replace: the world's own grass objects go where the new grass grows
	GrassMapOptions map;       // --grass-rules, --grass-cell (shared with the report)
};

// A tuft in the world, before it is packed into its tile.
struct GrassSeed {
	float pos[3];
	u8 look = 0;       // as GrassTuft::look
	i8 leanX = 0, leanZ = 0;
	u8 shade = 0;      // 0..3
	u8 rgb[3] = {0, 0, 0};
	u8 day = 255;
	u32 key = 0;       // its place in the tile's order (a hash)
};

struct GrassBakeStats {
	u32 tiles = 0;
	u32 tufts = 0;
	u32 maxTile = 0;          // tufts of the fullest tile
	u32 capped = 0;           // tufts left out of tiles that would have had more than a mesh can hold
	u32 noGround = 0;         // places where the ground was not where the map had it
	u32 oldRemoved = 0;       // triangles of the world's own grass objects taken out (--grass-replace)
	u32 shaded = 0;           // tufts in the sun's shadow (some or all of it)
	double areaFull = 0;      // m² of full-density grass on the map
	u64 bytes = 0;            // the index and the list
	u64 atlasBytes = 0;
	float variantShare[4] = {0, 0, 0, 0};
};

struct GrassPlacement {
	GrassMap map;
	std::vector<GrassSeed> seeds;
};

// Sows the tufts of one world. False (and `why`) for an indoor world, one without ground, or one where nothing grows.
bool placeGrass(const SrcScene& scene, const GrassOptions& opt, GrassPlacement& out, GrassBakeStats& stats, std::string& why);

// --grass-replace: takes out the triangles of the world's grass objects (a texture or material with GRAS in its name,
// among the objects - not the world mesh) that stand where the new grass grows (map density 0.25 and more).
u32 removeOldGrass(SrcScene& scene, const GrassPlacement& placement);

// Gives each tuft the sun's shade at its foot (0 sunlit .. 1 in full shade; `shadeAt` false: nothing known there).
void shadeGrass(GrassPlacement& placement, const std::function<bool(float x, float z, float& shade)>& shadeAt, GrassBakeStats& stats);

// The two entries of a world: the index ("world/<name>/grass", naming `dataEntry` and `textureEntry`) and the tuft list.
void encodeGrass(const GrassPlacement& placement, const GrassOptions& opt, u32 dataEntry, u32 textureEntry, std::vector<u8>& index,
                 std::vector<u8>& data, GrassBakeStats& stats);

// The atlas: GRASS_VARIANTS pictures of a tuft side by side, 64 x 64 each (drawn here: blades, seed heads, a few flowers).
SrcImage grassAtlasImage();
// ... baked as a cut-out texture whose far mip levels keep the blades' coverage.
std::vector<u8> grassAtlasTexture(const TexBakeOptions& base);

} // namespace g3
