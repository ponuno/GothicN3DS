#include "grass_report.h"
#include "grass_map.h"

#include "g3ds/world.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "../../third_party/stb/stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace g3 {

namespace {

const char* groupName(u16 g) {
	switch (g) {
	case 1: return "metal";
	case 2: return "stone";
	case 3: return "wood";
	case 4: return "earth";
	case 5: return "water";
	case 6: return "snow";
	default: return "-";
	}
}

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
	char buf[1024];
	va_list ap;
	va_start(ap, f);
	std::vsnprintf(buf, sizeof(buf), f, ap);
	va_end(ap);
	return buf;
}

// Green -> yellow -> red.
void ramp(float t, float rgb[3]) {
	t = std::clamp(t, 0.0f, 1.0f);
	static const float k[3][3] = {{40, 150, 40}, {235, 220, 40}, {225, 40, 30}};
	const int i = t < 0.5f ? 0 : 1;
	const float f = t < 0.5f ? t * 2.0f : (t - 0.5f) * 2.0f;
	for (int c = 0; c < 3; ++c) rgb[c] = k[i][c] + (k[i + 1][c] - k[i][c]) * f;
}

std::string joinPath(const std::string& dir, const std::string& file) {
	if (dir.empty()) return file;
	const char last = dir.back();
	return dir + (last == '/' || last == '\\' ? "" : "/") + file;
}

} // namespace

std::string writeGrassReport(const std::string& worldName, const SrcScene& scene, const GrassReportOptions& opt) {
	GrassMapOptions mo;
	mo.rulesFile = opt.rulesFile;
	mo.cellCm = opt.cellCm;
	bool ok = false;
	std::string why;
	const GrassMap g = buildGrassMap(scene, mo, ok, why);
	if (!ok) return "grass report: " + worldName + ": " + why + ": skipped";
	const float NONE = GrassMap::NONE;
	const size_t cells = g.cells();
	const double cellM2 = g.cellM2();
	const std::vector<GrassTexStat>& texs = g.texs;
	const std::vector<float>& density = g.density;
	const std::vector<float>& groundY = g.groundY;
	const std::vector<float>& waterDist = g.waterDist;
	const size_t worldTris = g.worldTris, groundCells = g.groundCells, underwater = g.underwater, coloured = g.coloured;
	const u32 trees = g.trees, bushes = g.bushes, grasses = g.grasses;
	const std::string& rulesNote = g.rulesNote;
	std::vector<size_t> byTop(texs.size());
	for (size_t i = 0; i < texs.size(); ++i) byTop[i] = i;
	std::sort(byTop.begin(), byTop.end(), [&](size_t a, size_t b) { return texs[a].topArea != texs[b].topArea ? texs[a].topArea > texs[b].topArea : texs[a].area > texs[b].area; });
	size_t growing = 0;
	for (float d : density)
		if (d >= 0.1f) ++growing;
	// The old grass objects: how many stand where the map says grass grows.
	u32 oldOnGrass = 0, oldPlaced = 0;
	for (const SrcPlant& p : scene.plants) {
		if (p.kind != 3) continue;
		int cx, cz;
		if (!g.cellOf(p.pos[0], p.pos[2], cx, cz)) continue;
		++oldPlaced;
		if (density[g.at(cx, cz)] >= 0.25f) ++oldOnGrass;
	}
	// The densest place the camera sees: a square of the area of a 25 m circle (44 m), by a summed-area table.
	double worst = 0;
	{
		std::vector<double> sat((size_t(g.w) + 1) * (size_t(g.h) + 1), 0.0);
		const auto S = [&](int x, int z) -> double& { return sat[size_t(z) * (size_t(g.w) + 1) + size_t(x)]; };
		for (int z = 0; z < g.h; ++z)
			for (int x = 0; x < g.w; ++x) S(x + 1, z + 1) = density[g.at(x, z)] + S(x, z + 1) + S(x + 1, z) - S(x, z);
		const int side = std::max(1, int(std::lround(4431.0f / g.cell)));
		for (int z = 0; z + side <= g.h; ++z)
			for (int x = 0; x + side <= g.w; ++x) worst = std::max(worst, S(x + side, z + side) - S(x, z + side) - S(x + side, z) + S(x, z));
		worst *= cellM2;
	}
	double worstTile = 0;
	{
		const int side = std::max(1, int(std::lround(1600.0f / g.cell)));
		for (int tz = 0; tz < g.h; tz += side)
			for (int tx = 0; tx < g.w; tx += side) {
				double s = 0;
				for (int z = tz; z < std::min(g.h, tz + side); ++z)
					for (int x = tx; x < std::min(g.w, tx + side); ++x) s += density[g.at(x, z)];
				worstTile = std::max(worstTile, s * cellM2);
			}
	}

	// ---- pictures ----
	std::vector<u8> shade(cells, 0);
	for (int z = 0; z < g.h; ++z)
		for (int x = 0; x < g.w; ++x) {
			const size_t k = g.at(x, z);
			if (groundY[k] == NONE) continue;
			const auto y = [&](int a, int b) {
				a = std::clamp(a, 0, g.w - 1);
				b = std::clamp(b, 0, g.h - 1);
				const float v = groundY[g.at(a, b)];
				return v == NONE ? groundY[k] : v;
			};
			const float dx = (y(x + 1, z) - y(x - 1, z)) / (2.0f * g.cell), dz = (y(x, z + 1) - y(x, z - 1)) / (2.0f * g.cell);
			const float n[3] = {-dx, 1.0f, -dz};
			const float len = std::sqrt(n[0] * n[0] + 1.0f + n[2] * n[2]);
			const float l[3] = {-0.5f, 0.7071f, 0.5f};
			const float lit = std::max(0.0f, (n[0] * l[0] + n[1] * l[1] + n[2] * l[2]) / len);
			shade[k] = u8(std::clamp(35.0f + 120.0f * lit, 0.0f, 255.0f));
		}
	// (The map's z grows downwards in the picture flipped: north - the world's +z - at the top.)
	const auto paint = [&](const std::vector<float>& value, bool marks) {
		std::vector<u8> rgb(cells * 3, 0);
		for (int z = 0; z < g.h; ++z)
			for (int x = 0; x < g.w; ++x) {
				const size_t k = g.at(x, z);
				u8* p = &rgb[(size_t(g.h - 1 - z) * size_t(g.w) + size_t(x)) * 3];
				if (waterDist[k] == 0.0f) {
					p[0] = 30, p[1] = 70, p[2] = 160;
					continue;
				}
				if (groundY[k] == NONE) continue;
				float c[3] = {float(shade[k]), float(shade[k]), float(shade[k])};
				if (value[k] > 0.02f) {
					float r[3];
					ramp(value[k], r);
					const float a = 0.45f + 0.55f * std::min(value[k], 1.0f);
					const float s = 0.55f + 0.45f * float(shade[k]) / 155.0f;
					for (int i = 0; i < 3; ++i) c[i] = c[i] * (1.0f - a) + r[i] * s * a;
				}
				for (int i = 0; i < 3; ++i) p[i] = u8(std::clamp(c[i], 0.0f, 255.0f));
			}
		if (marks)
			for (const SrcPlant& pl : scene.plants) {
				int cx, cz;
				if (!g.cellOf(pl.pos[0], pl.pos[2], cx, cz)) continue;
				u8* p = &rgb[(size_t(g.h - 1 - cz) * size_t(g.w) + size_t(cx)) * 3];
				if (pl.kind == 1) p[0] = 0, p[1] = 45, p[2] = 0;
				else if (pl.kind == 3) p[0] = 255, p[1] = 255, p[2] = 255;
			}
		return rgb;
	};
	const std::string base = joinPath(opt.dir, "grass_" + worldName);
	std::vector<float> maskValue(cells, 0.0f);
	for (size_t k = 0; k < cells; ++k) maskValue[k] = g.texW[k];
	const std::vector<u8> densityPic = paint(density, true), maskPic = paint(maskValue, false);
	const bool wroteA = stbi_write_png((base + ".png").c_str(), g.w, g.h, 3, densityPic.data(), g.w * 3) != 0;
	const bool wroteB = stbi_write_png((base + "_mask.png").c_str(), g.w, g.h, 3, maskPic.data(), g.w * 3) != 0;

	// ---- the text ----
	std::string out;
	out += "Grass report: " + worldName + "\n";
	out += "(g3ds-bake --grass-report; nothing of this is in data.pak. North (+z) is up in the pictures, east (+x) right.)\n\n";
	out += fmt("map: %d x %d cells of %.2f m (%.0f x %.0f m), world corner x %.0f z %.0f (cm)\n", g.w, g.h, double(g.cell) / 100.0,
	           double(g.w) * g.cell / 100.0, double(g.h) * g.cell / 100.0, double(g.x0), double(g.z0));
	out += fmt("world mesh: %zu triangles, %zu textures (%zu with their average colour read); objects: %zu triangles\n", worldTris,
	           texs.size(), coloured, scene.triangles.size() - worldTris);
	out += fmt("open ground seen from above: %.0f m2 (%zu cells); under water %.0f m2\n", double(groundCells) * cellM2, groundCells,
	           double(underwater) * cellM2);
	out += fmt("plants: %u trees, %u bushes, %u grass objects\n", trees, bushes, grasses);
	if (!rulesNote.empty()) out += rulesNote + "\n";
	out += "\nWhat is left, m2 of full-density grass, after each factor in turn:\n";
	static const char* const kSteps[] = {"texture", "x slope", "x water", "x rooms", "x cover", "x paths", "x trees", "x noise"};
	for (int s = 0; s < 8; ++s) out += fmt("  %-8s %10.0f\n", kSteps[s], g.sums[s]);
	out += fmt("  = %.0f m2 of full-density grass; %.0f m2 has some (density >= 0.1)\n", g.sums[8], double(growing) * cellM2);
	if (oldPlaced)
		out += fmt("old grass objects where the map grows grass (density >= 0.25): %u of %u (%.0f %%): %s\n", oldOnGrass, oldPlaced,
		           100.0 * oldOnGrass / oldPlaced,
		           oldOnGrass * 2 >= oldPlaced ? "the rules agree with the designers" : "LOW: the rules miss where the designers put grass");

	out += "\nWhat it would cost (a tuft: 2 crossed cards, 8 vertices, 4 triangles):\n";
	out += "  tufts/m2 |   tufts | compact list (6 B) | ready meshes (152 B) | densest 25 m round the camera | densest 16 m tile\n";
	for (const float tpm : {1.0f, 2.0f, 4.0f}) {
		const double tufts = g.sums[8] * tpm;
		out += fmt("  %8.0f | %7.0fk | %15.1f MB | %17.1f MB | %9.0f tufts, %6.0fk triangles | %6.0f tufts\n", double(tpm), tufts / 1000.0,
		           tufts * 6.0 / 1048576.0, tufts * 152.0 / 1048576.0, worst * tpm, worst * tpm * 4.0 / 1000.0, worstTile * tpm);
	}
	out += "  (the camera's 25 m is the radius the grass would be drawn in; its real cost also depends on how much of it is in view)\n";

	out += "\nGround textures, by the open ground they cover (topmost polygon seen from above):\n";
	out += "  weight  src   top m2   share  all m2  flat%  light  colour          group  texture  (why)\n";
	size_t listed = 0;
	for (size_t r = 0; r < byTop.size() && listed < 80; ++r) {
		const GrassTexStat& t = texs[byTop[r]];
		if (t.topArea <= 0 && listed >= 40) break;
		++listed;
		const double share = groundCells ? 100.0 * t.topArea / (double(groundCells) * cellM2) : 0.0;
		const double flat = t.upArea > 0 ? 100.0 * t.flatArea / t.upArea : 0.0;
		const double light = t.lightWeight > 0 ? t.lightSum / t.lightWeight : 0.0;
		const std::string colour = t.hasColour ? fmt("%3.0f %3.0f %3.0f", double(t.colour[0]), double(t.colour[1]), double(t.colour[2])) : std::string("     -     ");
		out += fmt("  %5.2f  %-4s %8.0f %6.1f%% %7.0f %5.0f%% %5.0f  %-14s  %-5s  %s  (%s)\n", double(t.weight), t.ruled ? "rule" : "auto", t.topArea,
		           share, t.area, flat, light, colour.c_str(), groupName(t.group), t.key.c_str(), t.why.c_str());
	}
	out += "\nPictures: " + base + ".png (density: grey relief where nothing grows, green -> yellow -> red denser, blue water, dark dots trees, "
	       "white dots the old grass objects), " + base + "_mask.png (the texture weights alone).\n";
	out += "To change a texture's weight: edit " + base + "_rules.txt and bake again with --grass-rules " + base + "_rules.txt\n";
	{
		std::ofstream f(base + ".txt", std::ios::binary);
		f << out;
	}
	{
		std::string rulesText = "# Grass weights of " + worldName + "'s ground textures, 0 (none) .. 1 (full), for g3ds-bake --grass-rules FILE.\n"
		                        "# One per line: TEXTURE WEIGHT; a name ending in * stands for every texture that begins so (OW_ROCK* 0).\n"
		                        "# The figures after # are the open ground it covers and why the report chose the weight.\n";
		for (size_t r = 0; r < byTop.size(); ++r) {
			const GrassTexStat& t = texs[byTop[r]];
			if (t.topArea <= 0 || t.key.compare(0, 8, "(colour)") == 0) continue;
			rulesText += fmt("%-40s %.2f   # %.0f m2, %s%s\n", t.key.c_str(), double(t.weight), t.topArea, t.ruled ? "rule; auto: " : "", t.why.c_str());
		}
		std::ofstream f(base + "_rules.txt", std::ios::binary);
		f << rulesText;
	}
	return fmt("grass report: %s: %.0f m2 of open ground, %.0f m2 of full-density grass (%.0f %%), densest 25 m: %.0f m2; %s%s.txt", worldName.c_str(),
	           double(groundCells) * cellM2, g.sums[8], groundCells ? 100.0 * g.sums[8] / (double(groundCells) * cellM2) : 0.0, worst,
	           wroteA && wroteB ? "" : "PICTURES NOT WRITTEN; ", base.c_str());
}

} // namespace g3
