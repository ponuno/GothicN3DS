#include "sun_bake.h"

#include "bvh.h"
#include "g3ds/tex.h"
#include "g3ds/world.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

namespace g3 {

using namespace geo;

namespace {

inline V3 at(const SrcVertex& v) { return {v.pos[0], v.pos[1], v.pos[2]}; }

// The power of two (lo..hi) for n texels: a quarter coarser is still taken (a cell of 40 m and a margin is 128, not 256).
u32 pow2Above(float n, u32 lo, u32 hi) {
	u32 p = lo;
	while (p < hi && float(p) * 1.25f < n) p <<= 1;
	return p;
}

// A small hash, for the jitter of a texel's rays (the same each bake).
inline float hash01(u32 a, u32 b) {
	u32 h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
	h ^= h >> 15;
	h *= 0x2C1B3C6Du;
	h ^= h >> 12;
	return float(h & 0xFFFFFFu) / 16777216.0f;
}

} // namespace

std::vector<u8> encodeL4(const std::vector<u8>& values, u32 w, u32 h) {
	TexHeader th {};
	th.magic = TEX_MAGIC;
	th.width = u16(w);
	th.height = u16(h);
	th.format = TEX_L4;
	th.mipCount = 1;
	th.flags = TEXF_CLAMP;
	th.dataSize = w * h / 2;
	std::vector<u8> out(sizeof(TexHeader) + th.dataSize, 0);
	std::memcpy(out.data(), &th, sizeof(th));
	u8* data = out.data() + sizeof(TexHeader);
	for (u32 y = 0; y < h; ++y)
		for (u32 x = 0; x < w; ++x) {
			const u32 n = texL4Nibble(w, h, x, y);
			data[n >> 1] |= u8((values[y * w + x] & 15u) << ((n & 1u) * 4u));
		}
	return out;
}

std::vector<SunMask> bakeSunMasks(const SrcScene& scene, const std::vector<u8>& kind, const std::vector<SunMask>& cells,
                                  const SunOptions& opt, SunStats& stats) {
	stats = SunStats {};
	std::vector<SunMask> out;
	if (!opt.enabled || cells.empty()) return out;
	const auto start = std::chrono::steady_clock::now();
	auto kindOf = [&](u32 m) -> u8 { return m < kind.size() ? kind[m] : u8(0); };

	// What takes a shadow (looked for straight down) and what casts one (towards the sun). Small objects cast none:
	// the menu may thin them out (as for the occlusion).
	std::vector<Tri> receivers, blockers;
	float topY = -1e30f;
	for (const SrcTriangle& t : scene.triangles) {
		for (const SrcVertex& v : t.v) topY = std::max(topY, v.pos[1]);
		const u8 k = kindOf(t.material);
		if (k == 2) continue;
		const V3 a = at(t.v[0]);
		const V3 e1 = at(t.v[1]) - a, e2 = at(t.v[2]) - a;
		const V3 n = cross(e1, e2);
		const float len = length(n);
		if (len < 1e-4f) continue;
		if ((t.detail & 3) != DETAIL_SMALL) blockers.push_back(Tri {a, e1, e2, k == 1 ? std::clamp(opt.leaves, 0.0f, 1.0f) : 1.0f});
		// (Facing up enough to be stood on or to be a roof; either side, as the source's winding may be either.)
		if (k == 0 && std::fabs(n.y) / len > 0.35f) receivers.push_back(Tri {a, e1, e2, 1.0f});
	}
	Bvh down, toSun;
	down.build(std::move(receivers));
	toSun.build(std::move(blockers));

	float sun[3];
	sunDirectionAt(opt.hour, sun);
	const V3 s = {sun[0], sun[1], sun[2]};
	// Two directions across the sun's, to spread the rays over its disc.
	V3 a1 = std::fabs(s.y) < 0.9f ? cross(s, V3 {0, 1, 0}) : cross(s, V3 {1, 0, 0});
	a1 = a1 * (1.0f / length(a1));
	const V3 a2 = cross(s, a1);
	const float disc = std::tan(opt.softness * 0.0174533f);

	out.resize(cells.size());
	std::atomic<size_t> next {0};
	std::atomic<u64> rays {0}, texels {0}, shaded {0}, empty {0};
	auto work = [&]() {
		u64 myRays = 0, myTexels = 0, myShaded = 0, myEmpty = 0;
		for (size_t ci = next.fetch_add(1); ci < cells.size(); ci = next.fetch_add(1)) {
			const SunMask& c = cells[ci];
			SunMask& m = out[ci];
			m.gridX = c.gridX;
			m.gridZ = c.gridZ;
			m.x0 = c.x0;
			m.z0 = c.z0;
			m.x1 = c.x1;
			m.z1 = c.z1;
			const float tex = std::max(opt.texel, 4.0f);
			m.width = pow2Above((c.x1 - c.x0) / tex, 8, std::max<u32>(opt.maxSize, 8));
			m.height = pow2Above((c.z1 - c.z0) / tex, 8, std::max<u32>(opt.maxSize, 8));
			const float dx = (c.x1 - c.x0) / float(m.width), dz = (c.z1 - c.z0) / float(m.height);
			std::vector<float> vis(size_t(m.width) * m.height, -1.0f);
			for (u32 y = 0; y < m.height; ++y)
				for (u32 x = 0; x < m.width; ++x) {
					const float px = c.x0 + (float(x) + 0.5f) * dx, pz = c.z0 + (float(y) + 0.5f) * dz;
					// The highest surface there that takes a shadow.
					float t = 0;
					const Tri* hit = nullptr;
					const float top = topY + 100.0f;
					++myRays;
					if (down.nearest({px, top, pz}, {0, -1, 0}, 0.0f, 1e7f, t, &hit) <= 0.0f || !hit) {
						++myEmpty;
						continue;
					}
					V3 n = cross(hit->e1, hit->e2);
					n = n * (1.0f / length(n));
					if (n.y < 0) n = n * -1.0f;
					const float facing = dot(n, s);
					float sum = 0.0f;
					if (facing > 0.0f) {
						const V3 p = {px, top - t, pz};
						for (int k = 0; k < opt.samples; ++k) {
							// Spread over the texel (in its plane) and over the sun's disc.
							const float jx = (hash01(x * 7919u + y, u32(k) * 2u + ci * 131u) - 0.5f) * dx;
							const float jz = (hash01(y * 7907u + x, u32(k) * 2u + 1u + ci * 131u) - 0.5f) * dz;
							const float jy = -(n.x * jx + n.z * jz) / std::max(n.y, 0.2f);
							const float ang = hash01(u32(k), x ^ (y << 16)) * 6.2831853f, rad = std::sqrt(hash01(u32(k) + 99u, y ^ (x << 16))) * disc;
							V3 d = s + a1 * (std::cos(ang) * rad) + a2 * (std::sin(ang) * rad);
							d = d * (1.0f / length(d));
							const V3 o = V3 {p.x + jx, p.y + jy, p.z + jz} + n * 6.0f + d * 4.0f;
							float th = 0;
							const float blocks = toSun.nearest(o, d, 0.5f, opt.reach, th, nullptr, true);
							++myRays;
							sum += 1.0f - blocks;
						}
						sum /= float(std::max(opt.samples, 1));
						// (A surface turned almost edge-on to the sun gets little of it.)
						sum *= std::clamp(facing * 4.0f, 0.0f, 1.0f);
					}
					vis[size_t(y) * m.width + x] = sum;
				}
			// Texels with no surface under them: their neighbours' value (so that filtering at a roof's edge or beyond the
			// last triangle does not bring in a wrong one).
			for (int pass = 0; pass < 8; ++pass) {
				bool any = false;
				std::vector<float> next2 = vis;
				for (u32 y = 0; y < m.height; ++y)
					for (u32 x = 0; x < m.width; ++x) {
						if (vis[size_t(y) * m.width + x] >= 0.0f) continue;
						float sumN = 0;
						int cnt = 0;
						for (int oy = -1; oy <= 1; ++oy)
							for (int ox = -1; ox <= 1; ++ox) {
								const int nx = int(x) + ox, ny = int(y) + oy;
								if (nx < 0 || ny < 0 || nx >= int(m.width) || ny >= int(m.height)) continue;
								const float v = vis[size_t(ny) * m.width + size_t(nx)];
								if (v >= 0.0f) {
									sumN += v;
									++cnt;
								}
							}
						if (cnt) {
							next2[size_t(y) * m.width + x] = sumN / float(cnt);
							any = true;
						}
					}
				vis.swap(next2);
				if (!any) break;
			}
			std::vector<u8> values(vis.size());
			u32 lit = 0;
			for (size_t i = 0; i < vis.size(); ++i) {
				const float v = vis[i] < 0.0f ? 1.0f : vis[i];
				values[i] = u8(std::lround(std::clamp(v, 0.0f, 1.0f) * 15.0f));
				lit += values[i] == 15;
				myShaded += values[i] < 8;
			}
			myTexels += vis.size();
			m.lit = float(lit) / float(std::max<size_t>(vis.size(), 1));
			m.texture = encodeL4(values, m.width, m.height);
		}
		rays += myRays;
		texels += myTexels;
		shaded += myShaded;
		empty += myEmpty;
	};
	std::vector<std::thread> pool;
	for (unsigned t = 1; t < std::max(1u, opt.threads); ++t) pool.emplace_back(work);
	work();
	for (std::thread& t : pool) t.join();
	stats.cells = u32(out.size());
	stats.rays = rays;
	stats.texels = texels;
	stats.shaded = shaded;
	stats.empty = empty;
	for (const SunMask& m : out) stats.bytes += m.texture.size();
	stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	return out;
}

} // namespace g3
