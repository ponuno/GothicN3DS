#include "godrays.h"

#include "world.h"

#include <math.h>
#include <string.h>

namespace g3 {

namespace {

constexpr float kPosScale = 0.25f;
constexpr float kSunReach = 5000.0f;  // cm: how far towards the sun a place is checked for shade
constexpr float kSide = 320.0f;       // cm: the places round a beam's that are checked for shade
constexpr int kAskPerFrame = 8;       // places asked of the world in a frame (5 rays each at most)
constexpr float kDensity = 0.62f;     // share of the cells that have a place for a beam

unsigned mix(unsigned h) {
	h = (h ^ (h >> 15)) * 2246822519u;
	h = (h ^ (h >> 13)) * 3266489917u;
	return h ^ (h >> 16);
}
float unit(unsigned h) { return float(h & 0xFFFFu) * (1.0f / 65535.0f); }
float smooth(float x) {
	x = clamp(x, 0.0f, 1.0f);
	return x * x * (3.0f - 2.0f * x);
}

} // namespace

void GodRays::release() {
	for (gfx::Mesh*& m : _meshes) {
		if (m) gfx::meshDestroy(m);
		m = nullptr;
	}
	if (_texture) gfx::textureDestroy(_texture);
	_texture = nullptr;
	for (Cell& c : _cells) c = Cell {};
	_drawn = 0;
	_last = -1.0;
}

void GodRays::draw(const World& world, Vec3 camera, const float look[3], const float sun[3], float strength, const float tint[3],
                   double seconds) {
	_drawn = 0;
	const float dt = _last < 0.0 ? 0.0f : clamp(float(seconds - _last), 0.0f, 0.25f);
	_last = seconds;
	if (!(strength > 0.02f) || sun[1] < 0.03f) {
		for (Cell& c : _cells) c.level = 0.0f; // (they come back fading in)
		return;
	}
	if (!_texture) {
		// Soft across (a little brighter in the middle), fading in over the first tenth at the ground (v = 0) and out
		// slowly towards the sun (v = 1).
		constexpr int W = 16, H = 64;
		static u8 cov[W * H];
		for (int y = 0; y < H; ++y)
			for (int x = 0; x < W; ++x) {
				const float u = (float(x) + 0.5f) / float(W), v = (float(y) + 0.5f) / float(H);
				const float d = fabsf(2.0f * u - 1.0f);
				const float across = (1.0f - d * d) * (1.0f - d * d);
				const float along = smooth(v / 0.12f) * (1.0f - smooth((v - 0.35f) / 0.65f));
				cov[y * W + x] = u8(clamp(across * along, 0.0f, 1.0f) * 255.0f + 0.5f);
			}
		_texture = gfx::uiTextureCreate(W, H, cov, true);
		if (!_texture) return;
	}
	if (!_meshes[0]) {
		static u16 indices[MAX * 6];
		for (u32 q = 0; q < MAX; ++q) {
			const u16 b = u16(q * 4);
			const u16 quad[6] = {b, u16(b + 1), u16(b + 2), u16(b + 2), u16(b + 1), u16(b + 3)};
			memcpy(indices + q * 6, quad, sizeof(quad));
		}
		static WorldVertex blank[MAX * 4];
		for (gfx::Mesh*& m : _meshes) {
			m = gfx::meshCreate(blank, MAX * 4, indices, MAX * 6);
			if (!m) {
				release();
				return;
			}
		}
	}
	// The sun's way along the ground, and across it.
	float hx = sun[0], hz = sun[2];
	const float hl = sqrtf(hx * hx + hz * hz);
	if (hl < 0.05f) return; // (the sun overhead: the beams would stand straight up and be seen edge on)
	hx /= hl;
	hz /= hl;
	const float px = -hz, pz = hx;
	const Vec3 toSun = {sun[0], sun[1], sun[2]};
	float lx = look[0], lz = look[2];
	const float ll = sqrtf(lx * lx + lz * lz);
	if (ll > 1e-4f) {
		lx /= ll;
		lz /= ll;
	}
	const float ease = min(dt * 2.2f, 1.0f); // about half a second to come and go

	struct Beam {
		const Cell* cell;
		float level;
		unsigned hash;
	};
	Beam beams[MAX];
	u32 nBeams = 0;
	int asks = kAskPerFrame;
	const int cx = int(floorf(camera.x / CELL)), cz = int(floorf(camera.z / CELL));
	const int reach = int(FAR / CELL) + 1;
	for (int dz = -reach; dz <= reach; ++dz)
		for (int dx = -reach; dx <= reach; ++dx) {
			const int ix = cx + dx, iz = cz + dz;
			const unsigned h = mix(unsigned(ix) * 73856093u ^ unsigned(iz) * 19349663u ^ 0x51ED27u);
			if (unit(h) >= kDensity) continue;
			const float x = (float(ix) + 0.15f + 0.7f * unit(mix(h + 1u))) * CELL;
			const float z = (float(iz) + 0.15f + 0.7f * unit(mix(h + 2u))) * CELL;
			const float ddx = x - camera.x, ddz = z - camera.z;
			const float dist = sqrtf(ddx * ddx + ddz * ddz);
			Cell& c = _cells[(unsigned(iz) & (GRID - 1)) * GRID + (unsigned(ix) & (GRID - 1))];
			if (c.ix != ix || c.iz != iz || c.state == 0) {
				if (c.ix != ix || c.iz != iz) {
					c = Cell {};
					c.ix = ix;
					c.iz = iz;
				}
			}
			if (dist > FAR) {
				c.level = 0.0f;
				continue;
			}
			// Ask (again, once the sun has moved some 1.5 degrees, not more often than every 2 s).
			const float sunDot = c.sunAt[0] * sun[0] + c.sunAt[1] * sun[1] + c.sunAt[2] * sun[2];
			const bool stale = c.state != 0 && sunDot < 0.99966f && float(seconds) - c.askedAt > 2.0f;
			if ((c.state == 0 || stale) && asks > 0) {
				--asks;
				c.askedAt = float(seconds);
				memcpy(c.sunAt, sun, sizeof(c.sunAt));
				c.x = x;
				c.z = z;
				c.state = 1;
				c.shaded = 0;
				float ground;
				Vec3 n;
				if (world.rayDown({x, camera.y + 2000.0f, z}, 6000.0f, ground, n) && n.y > 0.5f) {
					c.ground = ground;
					const Vec3 base = {x, ground + 40.0f, z};
					if (!world.sightBlocked(base, base + toSun * kSunReach)) {
						// Round it: across the sun's way and along it.
						const float off[4][2] = {{px, pz}, {-px, -pz}, {hx, hz}, {-hx, -hz}};
						int shaded = 0;
						for (const auto& o : off) {
							const Vec3 at = {x + o[0] * kSide, ground + 160.0f, z + o[1] * kSide};
							if (world.sightBlocked(at, at + toSun * kSunReach)) ++shaded;
						}
						c.shaded = u8(shaded);
						if (shaded > 0) c.state = 2;
					}
				}
			}
			// What is wanted of it now.
			float want = 0.0f;
			if (c.state == 2) {
				want = beamLevel(true, c.shaded) * strength;
				// Far off it fades out; close by too (and so does one whose beam would pass right by the eye).
				want *= 1.0f - smooth((dist - FAR * 0.7f) / (FAR * 0.3f));
				want *= smooth((dist - NEAR) / NEAR);
				// In front of the camera only (with a margin: a beam leans, its top may be in view when its foot is not).
				const float front = dist > 1.0f ? (ddx * lx + ddz * lz) / dist : 1.0f;
				want *= smooth((front + 0.25f) / 0.5f);
				// Forward scattering: towards the sun from the eye. The beam's middle is what is looked at.
				const float midY = c.ground + 0.5f * 1200.0f * toSun.y;
				float tb[3] = {x + toSun.x * 600.0f - camera.x, midY - camera.y, z + toSun.z * 600.0f - camera.z};
				const float tl = sqrtf(tb[0] * tb[0] + tb[1] * tb[1] + tb[2] * tb[2]);
				if (tl > 1.0f) {
					tb[0] /= tl;
					tb[1] /= tl;
					tb[2] /= tl;
					want *= scatter(tb, sun);
				}
			}
			c.level += (want - c.level) * ease;
			if (c.level < 0.015f) continue;
			// Keep the brightest MAX.
			u32 at = nBeams;
			if (nBeams < MAX) ++nBeams;
			else if (beams[MAX - 1].level >= c.level) continue;
			else at = MAX - 1;
			while (at > 0 && beams[at - 1].level < c.level) {
				beams[at] = beams[at - 1];
				--at;
			}
			beams[at] = Beam {&c, c.level, h};
		}
	if (nBeams == 0) return;

	const float origin[3] = {floorf(camera.x), floorf(camera.y), floorf(camera.z)};
	static WorldVertex verts[MAX * 4];
	u32 n = 0;
	for (u32 i = 0; i < nBeams; ++i) {
		const Cell& c = *beams[i].cell;
		const unsigned h = beams[i].hash;
		const float len = 1300.0f + 1300.0f * unit(h >> 3);
		const Vec3 base = {c.x, c.ground, c.z};
		const Vec3 top = base + toSun * len;
		// Turned to the camera round its own axis (the sun's direction).
		const float vx = (base.x + top.x) * 0.5f - camera.x, vy = (base.y + top.y) * 0.5f - camera.y, vz = (base.z + top.z) * 0.5f - camera.z;
		float rx = sun[1] * vz - sun[2] * vy, ry = sun[2] * vx - sun[0] * vz, rz = sun[0] * vy - sun[1] * vx;
		const float rl = sqrtf(rx * rx + ry * ry + rz * rz);
		if (rl < 1e-3f) continue;
		rx /= rl;
		ry /= rl;
		rz /= rl;
		const float w0 = 70.0f + 70.0f * unit(h >> 11), w1 = w0 * 1.7f;
		const float shimmer = 0.82f + 0.18f * sinf(float(seconds) * (0.45f + 0.3f * unit(h >> 5)) + unit(h >> 13) * 6.28f);
		const float k = clamp(beams[i].level * shimmer * 1.6f, 0.0f, 1.0f);
		const u8 col[3] = {u8(clamp(tint[0] * k, 0.0f, 255.0f)), u8(clamp(tint[1] * k, 0.0f, 255.0f)), u8(clamp(tint[2] * k, 0.0f, 255.0f))};
		const Vec3 pts[4] = {{base.x - rx * w0, base.y - ry * w0, base.z - rz * w0}, {base.x + rx * w0, base.y + ry * w0, base.z + rz * w0},
		                     {top.x - rx * w1, top.y - ry * w1, top.z - rz * w1}, {top.x + rx * w1, top.y + ry * w1, top.z + rz * w1}};
		WorldVertex* o = verts + n * 4;
		for (int q = 0; q < 4; ++q) {
			o[q].x = i16(clamp((pts[q].x - origin[0]) / kPosScale, -32000.0f, 32000.0f));
			o[q].y = i16(clamp((pts[q].y - origin[1]) / kPosScale, -32000.0f, 32000.0f));
			o[q].z = i16(clamp((pts[q].z - origin[2]) / kPosScale, -32000.0f, 32000.0f));
			o[q].w = 0;
			o[q].u = i16((q & 1) ? 1024 : 0);
			o[q].v = i16(q < 2 ? 0 : 1024); // v 0 at the ground, 1 towards the sun
			o[q].r = col[0];
			o[q].g = col[1];
			o[q].b = col[2];
			o[q].a = 0;
		}
		++n;
	}
	if (n == 0) return;
	gfx::Mesh* mesh = _meshes[_turn];
	_turn = (_turn + 1) % MESHES;
	gfx::meshUpdate(mesh, verts, n * 4);
	gfx::setChunkTransform(origin, kPosScale, 1.0f / 1024.0f);
	gfx::setMaterial(_texture, MATF_ADD, 0.7f);
	gfx::draw(mesh, 0, n * 6);
	_drawn = n;
}

} // namespace g3
