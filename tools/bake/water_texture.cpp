#include "water_texture.h"

#include <algorithm>
#include <cmath>

namespace g3 {

namespace {

const double kTau = 6.283185307179586;

// The well-known tileable caustic: a few rounds of a warped sine lattice, periodic in both axes.
double caustic(double u, double v) {
	const double inten = 0.005;
	const double px = std::fmod(u * kTau, kTau) - 250.0, py = std::fmod(v * kTau, kTau) - 250.0;
	double ix = px, iy = py, c = 1.0;
	for (int n = 0; n < 5; ++n) {
		const double t = 1.7 * (1.0 - 3.5 / double(n + 1));
		ix = px + std::cos(t - ix) + std::sin(t + iy);
		iy = py + std::sin(t - iy) + std::cos(t + ix);
		const double a = px / (std::sin(ix + t) / inten), b = py / (std::cos(iy + t) / inten);
		c += 1.0 / std::sqrt(a * a + b * b);
	}
	c /= 5.0;
	c = 1.17 - std::pow(c, 1.4);
	return std::pow(std::fabs(c), 8.0);
}

} // namespace

SrcImage makeWaterTexture(const unsigned char color[3], int size, int alpha) {
	SrcImage img;
	img.width = img.height = size;
	img.rgba.resize(size_t(size) * size_t(size) * 4);
	for (int y = 0; y < size; ++y)
		for (int x = 0; x < size; ++x) {
			const double u = double(x) / double(size), v = double(y) / double(size);
			double c = caustic(u, v);
			c = std::min(c * 0.85, 1.0);
			// A slow swell: darker and lighter bands, periodic (whole waves across the tile).
			const double swell = 0.5 + 0.5 * std::sin(kTau * (u * 2.0 + v) + 1.3 * std::sin(kTau * v * 3.0));
			// (fix140: a deeper body, 0.62..1.0, and only a light touch of white on the caustic lines: 0.65 made the water read as frost.)
				const double body = 0.62 + 0.38 * swell;
			unsigned char* p = &img.rgba[(size_t(y) * size_t(size) + size_t(x)) * 4];
			for (int k = 0; k < 3; ++k) {
				const double base = double(color[k]) * body;
				p[k] = (unsigned char)std::clamp(base + (255.0 - base) * c * 0.32, 0.0, 255.0);
			}
			p[3] = (unsigned char)std::clamp(double(alpha) + 45.0 * c + 15.0 * swell, 0.0, 255.0);
		}
	return img;
}

} // namespace g3
