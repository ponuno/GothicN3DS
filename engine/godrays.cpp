#include "godrays.h"

#include "world.h"

#include <math.h>
#include <string.h>

namespace g3 {

namespace {

constexpr float kPosScale = 0.25f;
constexpr float kSunReach = 6000.0f; // cm: how far towards the sun a crown is checked for what stands in the way
constexpr float kMaxLength = 2600.0f; // cm: the longest beam (from a crown on a slope; it fades out before the ground)
constexpr int kAskPerFrame = 8;      // beams asked of the world in a frame (a sight line and two rays down each)

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
int quant(float v) { return int(floorf(v / 60.0f)); }

} // namespace

void GodRays::release() {
	for (gfx::Mesh*& m : _meshes) {
		if (m) gfx::meshDestroy(m);
		m = nullptr;
	}
	if (_texture) gfx::textureDestroy(_texture);
	_texture = nullptr;
	for (Beam& b : _beams) b = Beam {};
	_drawn = _crowns = 0;
	_last = -1.0;
}

void GodRays::draw(const World& world, Vec3 camera, const float look[3], const float sun[3], float strength, const float tint[3],
                   double seconds) {
	_drawn = 0;
	++_frame;
	const float dt = _last < 0.0 ? 0.0f : clamp(float(seconds - _last), 0.0f, 0.25f);
	_last = seconds;
	if (!(strength > 0.02f) || sun[1] < 0.08f) {
		for (Beam& b : _beams) b.level = 0.0f; // (they come back fading in)
		return;
	}
	if (!_texture) {
		// Soft across (brightest in the middle); along it fades in over the first sixth at the ground (v = 0) and out
		// over the last third into the leaves (v = 1).
		constexpr int W = 16, H = 64;
		static u8 cov[W * H];
		for (int y = 0; y < H; ++y)
			for (int x = 0; x < W; ++x) {
				const float u = (float(x) + 0.5f) / float(W), v = (float(y) + 0.5f) / float(H);
				const float d = fabsf(2.0f * u - 1.0f);
				const float across = (1.0f - d * d) * (1.0f - d * d);
				const float along = smooth(v / 0.16f) * (1.0f - smooth((v - 0.62f) / 0.38f));
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
	const Vec3 toSun = {sun[0], sun[1], sun[2]};
	float lx = look[0], lz = look[2];
	const float ll = sqrtf(lx * lx + lz * lz);
	if (ll > 1e-4f) {
		lx /= ll;
		lz /= ll;
	}
	const float ease = min(dt * 2.2f, 1.0f); // about half a second to come and go

	static World::PlantBox crowns[CROWNS];
	const u32 nCrowns = world.plantBoxes(camera, FAR, MIN_CROWN, crowns, CROWNS);
	_crowns = nCrowns;

	struct Shown {
		const Beam* beam;
		float level;
	};
	Shown shown[MAX];
	u32 nShown = 0;
	int asks = kAskPerFrame;
	for (u32 c = 0; c < nCrowns; ++c) {
		const World::PlantBox& box = crowns[c];
		const float w = box.bbMax[0] - box.bbMin[0], d = box.bbMax[2] - box.bbMin[2], h = box.bbMax[1] - box.bbMin[1];
		const unsigned base = mix(unsigned(quant((box.bbMin[0] + box.bbMax[0]) * 0.5f)) * 73856093u ^
		                          unsigned(quant((box.bbMin[2] + box.bbMax[2]) * 0.5f)) * 19349663u ^ unsigned(quant(box.bbMin[1])) * 83492791u);
		const int count = w > 700.0f || d > 700.0f ? 3 : 2;
		for (int k = 0; k < count; ++k) {
			const unsigned hb = mix(base + unsigned(k) * 0x9E3779B9u);
			if (unit(hb) > 0.8f) continue; // (not every crown has a gap for each)
			const u32 id = hb | 1u;
			// Its slot: the one that has it, else a free or forgotten one near its place.
			Beam* b = nullptr;
			Beam* spare = nullptr;
			for (u32 p = 0; p < 8; ++p) {
				Beam& s = _beams[(hb + p) % SLOTS];
				if (s.id == id) {
					b = &s;
					break;
				}
				if (!spare && (s.id == 0 || _frame - s.seenFrame > 90u)) spare = &s;
			}
			if (!b) {
				if (!spare) continue;
				*spare = Beam {};
				spare->id = id;
				b = spare;
			}
			b->seenFrame = _frame;
			// Ask (again, once the sun has moved some 1.5 degrees, not more often than every 2 s).
			const float sunDot = b->sunAt[0] * sun[0] + b->sunAt[1] * sun[1] + b->sunAt[2] * sun[2];
			const bool stale = b->state != 0 && sunDot < 0.99966f && float(seconds) - b->askedAt > 2.0f;
			if ((b->state == 0 || stale) && asks > 0) {
				--asks;
				b->askedAt = float(seconds);
				memcpy(b->sunAt, sun, sizeof(b->sunAt));
				b->state = 1;
				// Low in the crown, away from its rim.
				const Vec3 s = {box.bbMin[0] + (0.2f + 0.6f * unit(hb >> 3)) * w, box.bbMin[1] + (0.22f + 0.3f * unit(hb >> 7)) * h,
				                box.bbMin[2] + (0.2f + 0.6f * unit(hb >> 11)) * d};
				float gy;
				Vec3 n;
				if (!world.sightBlocked(s + toSun * 200.0f, s + toSun * kSunReach) &&
				    world.rayDown({s.x, s.y, s.z}, 3000.0f, gy, n)) {
					// Down along the sun to the ground: once from the ground under the crown, once more from where that lands.
					float len = min((s.y - gy) / sun[1], kMaxLength);
					Vec3 g = s - toSun * len;
					float gy2;
					if (world.rayDown({g.x, s.y + 400.0f, g.z}, 3400.0f, gy2, n) && gy2 < s.y) { // (from above the crown: the ground there may rise)
						len = min(max(s.y - gy2, 0.0f) / sun[1], kMaxLength);
						g = s - toSun * len;
					}
					if (len > 150.0f) {
						b->state = 2;
						b->top[0] = s.x;
						b->top[1] = s.y;
						b->top[2] = s.z;
						b->ground[0] = g.x;
						b->ground[1] = g.y;
						b->ground[2] = g.z;
					}
				}
			}
			// What is wanted of it now.
			float want = 0.0f;
			if (b->state == 2) {
				const float mx = (b->top[0] + b->ground[0]) * 0.5f, my = (b->top[1] + b->ground[1]) * 0.5f, mz = (b->top[2] + b->ground[2]) * 0.5f;
				const float ddx = mx - camera.x, ddz = mz - camera.z;
				const float dist = sqrtf(ddx * ddx + ddz * ddz);
				want = strength;
				// A short one (a low crown, a slope right under it) is a blot, not a beam.
				want *= smooth((length(Vec3 {b->top[0] - b->ground[0], b->top[1] - b->ground[1], b->top[2] - b->ground[2]}) - 250.0f) / 350.0f);
				want *= 1.0f - smooth((dist - FAR * 0.7f) / (FAR * 0.3f));
				// Close by it fades: the nearest point of the beam to the eye, not its middle.
				const float sx = b->top[0] - b->ground[0], sy = b->top[1] - b->ground[1], sz = b->top[2] - b->ground[2];
				const float ss = sx * sx + sy * sy + sz * sz;
				float t = ss > 1.0f ? ((camera.x - b->ground[0]) * sx + (camera.y - b->ground[1]) * sy + (camera.z - b->ground[2]) * sz) / ss : 0.0f;
				t = clamp(t, 0.0f, 1.0f);
				const float px = b->ground[0] + sx * t - camera.x, py = b->ground[1] + sy * t - camera.y, pz = b->ground[2] + sz * t - camera.z;
				want *= smooth((sqrtf(px * px + py * py + pz * pz) - NEAR * 0.5f) / NEAR);
				// In front of the camera (with a margin: a beam leans, its top may be in view when its foot is not).
				const float front = dist > 1.0f ? (ddx * lx + ddz * lz) / dist : 1.0f;
				want *= smooth((front + 0.3f) / 0.5f);
				float tb[3] = {mx - camera.x, my - camera.y, mz - camera.z};
				const float tl = sqrtf(tb[0] * tb[0] + tb[1] * tb[1] + tb[2] * tb[2]);
				if (tl > 1.0f) {
					tb[0] /= tl;
					tb[1] /= tl;
					tb[2] /= tl;
					want *= scatter(tb, sun);
				}
			}
			b->level += (want - b->level) * ease;
			if (b->level < 0.015f) continue;
			// Keep the brightest MAX.
			u32 at = nShown;
			if (nShown < MAX) ++nShown;
			else if (shown[MAX - 1].level >= b->level) continue;
			else at = MAX - 1;
			while (at > 0 && shown[at - 1].level < b->level) {
				shown[at] = shown[at - 1];
				--at;
			}
			shown[at] = Shown {b, b->level};
		}
	}
	// A crown that is no longer loaded or in reach: its beams start from nothing when it comes back.
	for (Beam& b : _beams)
		if (b.seenFrame != _frame) b.level = 0.0f;
	if (nShown == 0) return;

	const float origin[3] = {floorf(camera.x), floorf(camera.y), floorf(camera.z)};
	static WorldVertex verts[MAX * 4];
	u32 n = 0;
	for (u32 i = 0; i < nShown; ++i) {
		const Beam& b = *shown[i].beam;
		const Vec3 base = {b.ground[0], b.ground[1], b.ground[2]};
		const Vec3 top = {b.top[0], b.top[1], b.top[2]};
		// Turned to the camera round its own axis (the sun's direction).
		const float vx = (base.x + top.x) * 0.5f - camera.x, vy = (base.y + top.y) * 0.5f - camera.y, vz = (base.z + top.z) * 0.5f - camera.z;
		float rx = sun[1] * vz - sun[2] * vy, ry = sun[2] * vx - sun[0] * vz, rz = sun[0] * vy - sun[1] * vx;
		const float rl = sqrtf(rx * rx + ry * ry + rz * rz);
		if (rl < 1e-3f) continue;
		rx /= rl;
		ry /= rl;
		rz /= rl;
		const float w0 = 40.0f + 50.0f * unit(b.id >> 5), w1 = w0 * 0.75f; // (narrower where it leaves the gap in the leaves)
		const float shimmer = 0.82f + 0.18f * sinf(float(seconds) * (0.45f + 0.3f * unit(b.id >> 9)) + unit(b.id >> 13) * 6.28f);
		const float k = clamp(shown[i].level * shimmer, 0.0f, 1.0f);
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
			o[q].v = i16(q < 2 ? 0 : 1024); // v 0 at the ground, 1 in the leaves
			o[q].r = col[0];
			o[q].g = col[1];
			o[q].b = col[2];
			o[q].a = 0;
		}
		++n;
	}
	if (n == 0) return;
	gfx::Mesh* mesh = _meshes[_turn];
	_turn = (_turn + 1) % 3;
	gfx::meshUpdate(mesh, verts, n * 4);
	gfx::setChunkTransform(origin, kPosScale, 1.0f / 1024.0f);
	gfx::setMaterial(_texture, MATF_ADD, 0.45f);
	gfx::draw(mesh, 0, n * 6);
	_drawn = n;
}

} // namespace g3
