#include "water_texture.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace g3 {

namespace {

const double kTau = 6.283185307179586;

// fix151: the water's surface instead of caustics. Caustics are the light on the *bottom* of clear shallow water; drawn on
// the surface they made the lakes read as a swimming pool (a bright net over a pale body). Now the picture is what a
// wind-blown surface looks like from a few metres up:
//  - a height field of wind ripples: a sum of travelling-wave components whose wave vectors are whole numbers of cycles
//    across the tile (so it tiles with no seam), mostly along one wind direction, the amplitude falling with the
//    frequency as in a sea spectrum, plus a fine octave of capillary ripples;
//  - its slope shades it: facets turned to the sky mirror it (a pale, cool grey-blue), facets turned away show the water's
//    own dark body; a few of the steepest facets catch a glint;
//  - "cat's paws": a low-frequency mask that makes some patches rougher and some calmer, so the 9 m tile does not repeat
//    as an obvious grid when seen across a lake;
//  - alpha follows the reflection: mirrored sky is opaque, the dark troughs let a little of the bottom through.

struct Wave {
	int kx, ky;     // cycles across the tile along u and v
	double amp;     // height
	double phase;   // radians
};

// A small deterministic generator: the texture is the same on every bake and every platform.
struct Lcg {
	uint32_t s;
	double next() {
		s = s * 1664525u + 1013904223u;
		return double(s >> 8) / double(1u << 24);
	}
	double gauss() {
		const double a = std::max(next(), 1e-9), b = next();
		return std::sqrt(-2.0 * std::log(a)) * std::cos(kTau * b);
	}
};

// `count` components with |k| in [kMin, kMax] cycles a tile, spread by `spread` radians about the wind direction.
void addWaves(std::vector<Wave>& out, Lcg& rng, int count, double kMin, double kMax, double wind, double spread,
              double ampScale, double falloff) {
	int tries = 0;
	while (count > 0 && tries++ < count * 50) {
		const double k = kMin * std::pow(kMax / kMin, rng.next());
		const double dir = wind + rng.gauss() * spread;
		const int kx = int(std::lround(k * std::cos(dir))), ky = int(std::lround(k * std::sin(dir)));
		// (no component along an axis: those line up from tile to tile into a visible grid)
		if (std::abs(kx) < 1 || std::abs(ky) < 1) continue;
		bool dup = false;
		for (const Wave& w : out) dup = dup || (w.kx == kx && w.ky == ky) || (w.kx == -kx && w.ky == -ky);
		if (dup) continue;
		const double len = std::sqrt(double(kx * kx + ky * ky));
		out.push_back({kx, ky, ampScale * std::pow(len, -falloff), rng.next() * kTau});
		--count;
	}
}

double smooth01(double x) {
	x = std::clamp(x, 0.0, 1.0);
	return x * x * (3.0 - 2.0 * x);
}

} // namespace

SrcImage makeWaterTexture(const unsigned char color[3], int size, int alpha) {
	SrcImage img;
	img.width = img.height = size;
	img.rgba.resize(size_t(size) * size_t(size) * 4);

	Lcg rng {0x6A7E5u};
	const double wind = 0.62; // radians in the (u, v) plane
	std::vector<Wave> waves;
	addWaves(waves, rng, 34, 3.0, 13.0, wind, 0.70, 1.0, 1.25);   // the wind ripples
	addWaves(waves, rng, 16, 14.0, 24.0, wind, 1.00, 0.30, 1.0); // capillary ripples, less ordered
	// (the highest frequency is 26 cycles over 128 texels: about 5 texels a cycle, so no aliasing at the top level)
	std::vector<Wave> paws;
	addWaves(paws, rng, 10, 1.0, 3.6, wind, 3.0, 1.0, 0.5);

	// Two passes: the slope field first (and its spread, to set the shading's scale), then the colours.
	const size_t n = size_t(size) * size_t(size);
	std::vector<double> gx(n), gy(n), pawMask(n);
	double sumSq = 0.0, pawMin = 1e9, pawMax = -1e9;
	for (int y = 0; y < size; ++y)
		for (int x = 0; x < size; ++x) {
			const double u = double(x) / double(size), v = double(y) / double(size);
			double dx = 0.0, dy = 0.0;
			for (const Wave& w : waves) {
				const double c = std::cos(kTau * (w.kx * u + w.ky * v) + w.phase) * w.amp * kTau;
				dx += c * w.kx;
				dy += c * w.ky;
			}
			double p = 0.0;
			for (const Wave& w : paws) p += w.amp * std::sin(kTau * (w.kx * u + w.ky * v) + w.phase);
			const size_t i = size_t(y) * size_t(size) + size_t(x);
			gx[i] = dx;
			gy[i] = dy;
			pawMask[i] = p;
			sumSq += dx * dx + dy * dy;
			pawMin = std::min(pawMin, p);
			pawMax = std::max(pawMax, p);
		}
	const double rms = std::sqrt(sumSq / double(n)) + 1e-9;

	// Colours: the deep body is the colour asked for; the mirrored sky a pale cool grey-blue that keeps a little of it.
	double deep[3], sky[3];
	static const double kSky[3] = {118.0, 138.0, 148.0};
	for (int k = 0; k < 3; ++k) {
		deep[k] = double(color[k]);
		sky[k] = kSky[k] * 0.7 + std::min(double(color[k]) * 1.6, 255.0) * 0.3;
	}
	// The light (sky) comes from one side of the ripples: a fixed direction in (u, v), across the wind.
	const double lx = std::cos(wind + 1.2), ly = std::sin(wind + 1.2);

	for (int y = 0; y < size; ++y)
		for (int x = 0; x < size; ++x) {
			const size_t i = size_t(y) * size_t(size) + size_t(x);
			// 0 calm .. 1 rough patch; never quite flat.
			const double paw = smooth01((pawMask[i] - pawMin) / (pawMax - pawMin + 1e-9));
			const double rough = 0.45 + 0.55 * paw;
			const double sx = gx[i] / rms * rough, sy = gy[i] / rms * rough;
			// How much the facet faces the sky: its slope towards the light, plus a little of its steepness in any direction
			// (seen at a grazing angle, any tilted facet shows more sky than a flat one).
			const double facing = sx * lx + sy * ly;
			const double steep = std::sqrt(sx * sx + sy * sy);
			const double s = facing * 0.85 + steep * 0.25;
			const double refl = 1.0 / (1.0 + std::exp(-2.8 * (s - 0.65))); // 0 trough .. 1 mirrored crest
			// The glints: only the steepest facets towards the light, sparse, and fewer in the calm patches.
			const double glint = std::pow(std::max(facing - 1.75, 0.0), 1.5) * 0.55 * (0.4 + 0.6 * paw);
			// The body darkens a little in the calm patches (less sky in them) - the slow light and dark of a lake.
			const double body = 0.80 + 0.20 * paw;
			unsigned char* p = &img.rgba[i * 4];
			for (int k = 0; k < 3; ++k) {
				double c = deep[k] * body;
				c += (sky[k] - c) * refl * 0.60;
				c += (255.0 - c) * std::min(glint, 0.85);
				p[k] = (unsigned char)std::clamp(c + 0.5, 0.0, 255.0);
			}
			p[3] = (unsigned char)std::clamp(double(alpha) + 55.0 * refl + 60.0 * std::min(glint, 1.0), 0.0, 255.0);
		}
	return img;
}

} // namespace g3
