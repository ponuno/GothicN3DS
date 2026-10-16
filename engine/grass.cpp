#include "grass.h"

#include "clouds.h"

#include "g3ds/tex.h"
#include "trace.h"
#include "world.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace g3 {

namespace {

float smooth(float x) {
	x = clamp(x, 0.0f, 1.0f);
	return x * x * (3.0f - 2.0f * x);
}

// The nearest and the farthest point of the square [x0, x0 + size) x [z0, z0 + size) from (x, z), squared.
void squareRange(float x0, float z0, float size, float x, float z, float& nearSq, float& farSq) {
	const float dx = x < x0 ? x0 - x : x > x0 + size ? x - (x0 + size) : 0.0f;
	const float dz = z < z0 ? z0 - z : z > z0 + size ? z - (z0 + size) : 0.0f;
	nearSq = dx * dx + dz * dz;
	const float fx = max(fabsf(x - x0), fabsf(x - (x0 + size))), fz = max(fabsf(z - z0), fabsf(z - (z0 + size)));
	farSq = fx * fx + fz * fz;
}

i16 quant(float v) { return i16(clamp(floorf(v / Grass::POS_SCALE + 0.5f), -32767.0f, 32767.0f)); }

} // namespace

void Grass::release() {
	for (Slot& s : _slots) drop(s);
	if (_texture) gfx::textureDestroy(_texture);
	_texture = nullptr;
	_textureTried = false;
	free(_table);
	_table = nullptr;
	free(_verts);
	_verts = nullptr;
	_vertCap = 0;
	free(_indices);
	_indices = nullptr;
	_indexCap = 0;
	_hdr = GrassHeader {};
	_pak = nullptr;
	_stats = GrassStats {};
}

void Grass::drop(Slot& s) {
	if (s.mesh) gfx::meshDestroy(s.mesh); // (freed once the GPU is done with it)
	free(s.tufts);
	s = Slot {};
}

bool Grass::enterWorld(Pak& pak, const char* worldAsset) {
	release();
	char name[96];
	snprintf(name, sizeof(name), "%s/grass", worldAsset);
	const PakEntry* e = pak.find(name);
	if (!e || e->size < sizeof(GrassHeader)) return false;
	u8* blob = static_cast<u8*>(pak.readAlloc(*e));
	if (!blob) return false;
	GrassHeader h;
	memcpy(&h, blob, sizeof(h));
	const PakEntry* data = h.magic == GRASS_MAGIC && h.version == GRASS_VERSION ? pak.find(h.dataEntry) : nullptr;
	// (The list is read a tile at a time: it must be stored raw and hold what the index says.)
	const bool ok = data && grassBlobSize(h) == e->size && h.tileCount > 0 && h.tileSize >= 100.0f && h.height > 0.0f && h.width > 0.0f &&
	                data->codec == PAK_RAW && data->packedSize == data->size && u64(h.tuftCount) * sizeof(GrassTuft) == data->size;
	if (!ok) {
		free(blob);
		return false;
	}
	_table = static_cast<GrassTile*>(malloc(h.tileCount * sizeof(GrassTile)));
	if (!_table) {
		free(blob);
		return false;
	}
	memcpy(_table, blob + sizeof(GrassHeader), h.tileCount * sizeof(GrassTile));
	free(blob);
	// (A tile that points outside the list is emptied.)
	for (u32 i = 0; i < h.tileCount; ++i)
		if (u64(_table[i].first) + _table[i].count > h.tuftCount) _table[i].count = 0;
	_hdr = h;
	_dataEntry = h.dataEntry;
	_textureEntry = h.textureEntry;
	_pak = &pak;
	return true;
}

int Grass::findTile(int x, int z) const {
	int lo = 0, hi = int(_hdr.tileCount) - 1;
	while (lo <= hi) {
		const int mid = (lo + hi) / 2;
		const GrassTile& t = _table[mid];
		if (t.z == z && t.x == x) return mid;
		if (t.z < z || (t.z == z && t.x < x)) lo = mid + 1;
		else hi = mid - 1;
	}
	return -1;
}

void Grass::tuftFoot(const GrassTile& t, const GrassTuft& g, float out[3]) const {
	const float step = _hdr.tileSize / 256.0f;
	out[0] = _hdr.originX + float(t.x) * _hdr.tileSize + (float(g.x) + 0.5f) * step;
	out[1] = t.yMin + float(g.y);
	out[2] = _hdr.originZ + float(t.z) * _hdr.tileSize + (float(g.z) + 0.5f) * step;
}

bool Grass::readTufts(Slot& s) {
	const GrassTile& t = _table[s.tile];
	const PakEntry* e = _pak ? _pak->find(_dataEntry) : nullptr;
	if (!e || !t.count) return false;
	s.tufts = static_cast<GrassTuft*>(malloc(t.count * sizeof(GrassTuft)));
	if (!s.tufts) return false;
	if (!_pak->readRange(*e, t.first * u32(sizeof(GrassTuft)), s.tufts, t.count * u32(sizeof(GrassTuft)))) {
		free(s.tufts);
		s.tufts = nullptr;
		return false;
	}
	++_stats.reads;
	return true;
}

u32 Grass::pressersIn(const Slot& s, const Frame& f, u8* out) const {
	const GrassTile& t = _table[s.tile];
	const float x0 = _hdr.originX + float(t.x) * _hdr.tileSize, z0 = _hdr.originZ + float(t.z) * _hdr.tileSize;
	u32 n = 0;
	for (u32 i = 0; i < f.pressCount && i < PRESSERS; ++i) {
		const Frame::Presser& p = f.press[i];
		float nearSq, farSq;
		squareRange(x0, z0, _hdr.tileSize, p.feet.x, p.feet.z, nearSq, farSq);
		const float r = p.radius + 20.0f;
		if (nearSq < r * r && p.feet.y > t.yMin - 150.0f && p.feet.y < t.yMax + 150.0f) out[n++] = u8(i);
	}
	return n;
}

float Grass::staleness(const Slot& s, const Frame& f) const {
	if (!s.built) return 1e9f;
	if (s.share != f.share || fabsf(s.radius - f.radius) > 60.0f) return 5000.0f;
	// Somebody came, went or moved on it: each of them now must stand within 15 cm of one of then.
	u8 in[PRESSERS];
	const u32 pn = pressersIn(s, f, in);
	if (pn != s.pressN) return 3000.0f;
	for (u32 k = 0; k < pn; ++k) {
		const Frame::Presser& p = f.press[in[k]];
		float best = 1e30f;
		for (u32 j = 0; j < s.pressN; ++j) {
			const float dx = p.feet.x - s.pressX[j], dz = p.feet.z - s.pressZ[j];
			best = min(best, dx * dx + dz * dz);
		}
		if (best > 15.0f * 15.0f) return 2000.0f + (in[k] == 0 ? 500.0f : 0.0f) + sqrtf(best); // (the first - the hero - before the others)
	}
	float worth = 0.0f;
	// A tile at the edge of the radius: its tufts shrink by their distance as it was when it was made.
	const GrassTile& t = _table[s.tile];
	const float x0 = _hdr.originX + float(t.x) * _hdr.tileSize, z0 = _hdr.originZ + float(t.z) * _hdr.tileSize;
	float nearSq, farSq;
	squareRange(x0, z0, _hdr.tileSize, f.camera.x, f.camera.z, nearSq, farSq);
	const float inner = max(f.radius - FADE - 50.0f, 0.0f);
	const bool edgeNow = farSq > inner * inner && nearSq < f.radius * f.radius;
	const bool edgeThen = s.farSq > inner * inner && s.nearSq < s.radius * s.radius;
	if (edgeNow || edgeThen) {
		const float dx = f.camera.x - s.camX, dy = f.camera.y - s.camY, dz = f.camera.z - s.camZ;
		const float moved = sqrtf(dx * dx + dy * dy + dz * dz);
		if (moved > 150.0f) worth = max(worth, moved);
	}
	if (fabsf(f.darkness - s.darkness) > 0.04f) worth = max(worth, 100.0f + 400.0f * fabsf(f.darkness - s.darkness));
	// fix186: the clouds have drifted on (a few metres: their edges are soft), or come or gone.
	const float cloudDark = f.clouds ? f.clouds->dark() : 0.0f;
	if (fabsf(cloudDark - s.cloudDark) > 0.04f) worth = max(worth, 100.0f + 400.0f * fabsf(cloudDark - s.cloudDark));
	if (cloudDark > 0.003f) {
		const float dx = f.clouds->driftX() - s.cloudX, dz = f.clouds->driftZ() - s.cloudZ;
		const float drift = sqrtf(dx * dx + dz * dz);
		if (drift > 400.0f) worth = max(worth, 60.0f + drift * 0.25f);
	}
	return worth;
}

void Grass::build(Slot& s, const Frame& f) {
	const GrassTile& t = _table[s.tile];
	const u32 count = f.share >= 1.0f ? t.count : u32(float(t.count) * clamp(f.share, 0.0f, 1.0f) + 0.5f);
	if (count > _vertCap) {
		WorldVertex* v = static_cast<WorldVertex*>(realloc(_verts, size_t(count) * 8 * sizeof(WorldVertex)));
		if (!v) return;
		_verts = v;
		_vertCap = count;
	}
	if (count > _indexCap) {
		u16* ix = static_cast<u16*>(realloc(_indices, size_t(count) * 12 * sizeof(u16)));
		if (!ix) return;
		_indices = ix;
		for (u32 i = _indexCap; i < count; ++i) {
			const u16 b = u16(i * 8);
			const u16 q[12] = {b, u16(b + 1), u16(b + 3), b, u16(b + 3), u16(b + 2),
			                   u16(b + 4), u16(b + 5), u16(b + 7), u16(b + 4), u16(b + 7), u16(b + 6)};
			memcpy(_indices + i * 12, q, sizeof(q));
		}
		_indexCap = count;
	}
	const float x0 = _hdr.originX + float(t.x) * _hdr.tileSize, z0 = _hdr.originZ + float(t.z) * _hdr.tileSize;
	s.origin[0] = x0;
	s.origin[1] = t.yMin;
	s.origin[2] = z0;
	const float step = _hdr.tileSize / 256.0f;
	const float radius = f.radius, radiusSq = radius * radius;
	u8 in[PRESSERS];
	const u32 pn = pressersIn(s, f, in);
	float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
	u32 n = 0;
	for (u32 i = 0; i < count; ++i) {
		const GrassTuft& g = s.tufts[i];
		if (g.y > 7800) continue; // (out of the mesh's reach above the tile's lowest foot: 81 m at POS_SCALE)
		const float px = x0 + (float(g.x) + 0.5f) * step, py = t.yMin + float(g.y), pz = z0 + (float(g.z) + 0.5f) * step;
		const float cx = px - f.camera.x, cy = py - f.camera.y, cz = pz - f.camera.z;
		const float dSq = cx * cx + cy * cy + cz * cz;
		if (dSq >= radiusSq) continue;
		const float d = sqrtf(dSq);
		const float fade = smooth((radius - d) / FADE);
		if (fade < 0.06f) continue;
		const float scale = grassScale(g.look) * fade;
		const float height = _hdr.height * scale, half = _hdr.width * scale * 0.5f;
		// (The turn is one of eight: cos and sin from a table.)
		static const float kCos[8] = {1.0f, 0.98079f, 0.92388f, 0.83147f, 0.70711f, 0.55557f, 0.38268f, 0.19509f};
		static const float kSin[8] = {0.0f, 0.19509f, 0.38268f, 0.55557f, 0.70711f, 0.83147f, 0.92388f, 0.98079f};
		const float ax = kCos[g.look >> 5], az = kSin[g.look >> 5];
		// The top: up and along the lean; round the hero's feet pressed away from him and down.
		float topX = grassLean(grassLeanX(g.lean)) * height, topY = height, topZ = grassLean(grassLeanZ(g.lean)) * height;
		float pressed = 0.0f;
		for (u32 k = 0; k < pn; ++k) {
			const Frame::Presser& p = f.press[in[k]];
			const float hx = px - p.feet.x, hz = pz - p.feet.z, hy = py - p.feet.y;
			const float hd = sqrtf(hx * hx + hz * hz);
			if (hd >= p.radius || hy < -120.0f || hy > 120.0f) continue;
			const float kk = smooth(1.0f - hd / p.radius);
			if (kk <= pressed) continue; // (the one pressing hardest)
			pressed = kk;
			const float ux = hd > 1.0f ? hx / hd : ax, uz = hd > 1.0f ? hz / hd : az;
			topX = grassLean(grassLeanX(g.lean)) * height + ux * kk * height * 0.8f;
			topZ = grassLean(grassLeanZ(g.lean)) * height + uz * kk * height * 0.8f;
			topY = height * (1.0f - 0.6f * kk);
		}
		// The colour: the ground's light there, darker at the foot; in the sun's shadow as dark as the ground is now.
		float sunK = 1.0f - f.darkness * float(grassShade(g.lean)) * (1.0f / 3.0f);
		if (f.clouds) sunK *= f.clouds->shade(px, pz); // fix186
		const float r = float((g.colour >> 11) & 31u) * (255.0f / 31.0f) * sunK, gg = float((g.colour >> 5) & 63u) * (255.0f / 63.0f) * sunK,
		            b = float(g.colour & 31u) * (255.0f / 31.0f) * sunK;
		const u8 top[3] = {u8(min(r * 1.06f, 255.0f)), u8(min(gg * 1.06f, 255.0f)), u8(min(b * 1.06f, 255.0f))};
		const u8 foot[3] = {u8(r * 0.72f), u8(gg * 0.72f), u8(b * 0.72f)};
		const u32 variant = u32(g.look & 3u) % GRASS_VARIANTS;
		const i16 u0 = i16(variant * (1024u / GRASS_VARIANTS) + 8u), u1 = i16((variant + 1u) * (1024u / GRASS_VARIANTS) - 8u);
		// The sway of the top: a little more for a big tuft (quarters of a cm, before the world's wind gain).
		const i16 sway = packVertexFx(0, u8(6.0f + 6.0f * scale));
		WorldVertex* o = _verts + n * 8;
		for (int card = 0; card < 2; ++card) {
			const float dx = card ? -az : ax, dz = card ? ax : az;
			const float fx[4] = {px - dx * half, px + dx * half, px - dx * half + topX, px + dx * half + topX};
			const float fy[4] = {py - 3.0f, py - 3.0f, py - 3.0f + topY, py - 3.0f + topY};
			const float fz[4] = {pz - dz * half, pz + dz * half, pz - dz * half + topZ, pz + dz * half + topZ};
			for (int c = 0; c < 4; ++c) {
				WorldVertex& v = o[card * 4 + c];
				v.x = quant(fx[c] - s.origin[0]);
				v.y = quant(fy[c] - s.origin[1]);
				v.z = quant(fz[c] - s.origin[2]);
				v.w = c < 2 ? i16(0) : sway;
				v.u = (c & 1) ? u1 : u0;
				v.v = i16(c < 2 ? 1016 : 4);
				const u8* col = c < 2 ? foot : top;
				v.r = col[0];
				v.g = col[1];
				v.b = col[2];
				v.a = t.day;
				lo[0] = min(lo[0], fx[c]);
				lo[1] = min(lo[1], fy[c]);
				lo[2] = min(lo[2], fz[c]);
				hi[0] = max(hi[0], fx[c]);
				hi[1] = max(hi[1], fy[c]);
				hi[2] = max(hi[2], fz[c]);
			}
		}
		++n;
	}
	if (s.mesh) gfx::meshDestroy(s.mesh); // (the old one is freed once the GPU is done with it)
	s.mesh = nullptr;
	s.bytes = 0;
	s.drawn = 0;
	if (n) {
		s.mesh = gfx::meshCreate(_verts, n * 8, _indices, n * 12);
		if (s.mesh) {
			s.drawn = n;
			s.bytes = n * 8 * u32(sizeof(WorldVertex)) + n * 12 * u32(sizeof(u16));
			// (The wind moves the tops a little out of the box.)
			for (int a = 0; a < 3; ++a) {
				s.bbMin[a] = lo[a] - (a == 1 ? 0.0f : 30.0f);
				s.bbMax[a] = hi[a] + (a == 1 ? 0.0f : 30.0f);
			}
		}
	}
	s.built = true;
	s.camX = f.camera.x;
	s.camY = f.camera.y;
	s.camZ = f.camera.z;
	s.radius = f.radius;
	s.share = f.share;
	s.darkness = f.darkness;
	s.cloudDark = f.clouds ? f.clouds->dark() : 0.0f;
	s.cloudX = f.clouds ? f.clouds->driftX() : 0.0f;
	s.cloudZ = f.clouds ? f.clouds->driftZ() : 0.0f;
	s.pressN = u8(pn);
	for (u32 k = 0; k < pn; ++k) {
		s.pressX[k] = f.press[in[k]].feet.x;
		s.pressZ[k] = f.press[in[k]].feet.z;
	}
	squareRange(x0, z0, _hdr.tileSize, f.camera.x, f.camera.z, s.nearSq, s.farSq);
	++_stats.built;
	++_stats.builtTotal;
}

void Grass::update(const Frame& f) {
	++_frame;
	_camera = f.camera;
	_stats.built = 0;
	_stats.tiles = _stats.tufts = _stats.triangles = _stats.drawCalls = 0; // (draw() counts them, if it is called)
	_stats.radius = f.radius;
	if (!_table) return;
	trace("grass: update");
	for (Slot& s : _slots) s.wanted = false;
	if (f.radius <= 0.0f) return;
	// The tiles in reach, a little beyond the radius (they are read before they are seen).
	const float reach = f.radius + 600.0f;
	const float size = _hdr.tileSize;
	const int tx0 = int(floorf((f.camera.x - reach - _hdr.originX) / size)), tx1 = int(floorf((f.camera.x + reach - _hdr.originX) / size));
	const int tz0 = int(floorf((f.camera.z - reach - _hdr.originZ) / size)), tz1 = int(floorf((f.camera.z + reach - _hdr.originZ) / size));
	struct Want {
		int tile;
		float nearSq;
	};
	Want want[SLOTS];
	u32 wanted = 0;
	for (int z = tz0; z <= tz1; ++z)
		for (int x = tx0; x <= tx1; ++x) {
			const int t = findTile(x, z);
			if (t < 0 || !_table[t].count) continue;
			const GrassTile& tile = _table[t];
			float nearSq, farSq;
			squareRange(_hdr.originX + float(x) * size, _hdr.originZ + float(z) * size, size, f.camera.x, f.camera.z, nearSq, farSq);
			if (nearSq > reach * reach) continue;
			if (f.camera.y < tile.yMin - reach || f.camera.y > tile.yMax + _hdr.height * 1.3f + reach) continue;
			// Nearest first; what does not fit the slots is left out (the farthest).
			u32 k = wanted < SLOTS ? wanted++ : SLOTS - 1;
			if (k == SLOTS - 1 && wanted == SLOTS && want[k].nearSq <= nearSq) continue;
			while (k > 0 && want[k - 1].nearSq > nearSq) {
				want[k] = want[k - 1];
				--k;
			}
			want[k] = Want {t, nearSq};
		}
	// Their slots: the ones that have them, else free ones, else the ones not wanted for longest.
	Slot* bySlot[SLOTS] = {};
	for (u32 w = 0; w < wanted; ++w)
		for (Slot& s : _slots)
			if (s.tile == want[w].tile) {
				s.wanted = true;
				s.usedFrame = _frame;
				bySlot[w] = &s;
				break;
			}
	bool readOne = false;
	for (u32 w = 0; w < wanted; ++w) {
		if (bySlot[w]) continue;
		if (readOne) break; // (one tile a frame from the card; the nearest first)
		Slot* pick = nullptr;
		for (Slot& s : _slots) {
			if (s.wanted) continue;
			if (s.tile < 0) {
				pick = &s;
				break;
			}
			if (!pick || s.usedFrame < pick->usedFrame) pick = &s;
		}
		if (!pick) break;
		drop(*pick);
		pick->tile = want[w].tile;
		pick->wanted = true;
		pick->usedFrame = _frame;
		readOne = true;
		if (!readTufts(*pick)) {
			pick->built = true; // (nothing to make of it: kept so that it is not read again and again)
			continue;
		}
		bySlot[w] = pick;
	}
	// Making: the most wanted first - a tile not made yet (the nearest first), then where somebody walks (the hero
	// first), then the stalest.
	u32 budget = BUILD_TUFTS;
	for (int pass = 0; pass < 8 && budget > 0; ++pass) {
		Slot* best = nullptr;
		float bestWorth = 0.0f;
		for (u32 w = 0; w < wanted; ++w) {
			Slot* s = bySlot[w];
			if (!s || !s->tufts) continue;
			float worth = staleness(*s, f);
			if (worth <= 0.0f) continue;
			if (!s->built) worth -= want[w].nearSq * 1e-6f; // (nearer first among the new ones)
			if (worth > bestWorth) {
				bestWorth = worth;
				best = s;
			}
		}
		if (!best) break;
		const u32 cost = _table[best->tile].count;
		if (pass > 0 && cost > budget) break;
		build(*best, f);
		budget = cost >= budget ? 0 : budget - cost;
	}
	u32 held = 0, bytes = 0;
	for (const Slot& s : _slots) {
		held += s.tile >= 0;
		bytes += s.bytes;
	}
	_stats.slots = held;
	_stats.meshBytes = bytes;
}

void Grass::draw(const World& world, const Frustum& view) {
	if (!_table || _stats.radius <= 0.0f) return;
	if (!_texture && !_textureTried) {
		_textureTried = true;
		const PakEntry* e = _pak ? _pak->find(_textureEntry) : nullptr;
		if (e && e->size > sizeof(TexHeader)) {
			if (u8* blob = static_cast<u8*>(_pak->readAlloc(*e))) {
				TexHeader th;
				memcpy(&th, blob, sizeof(th));
				if (texValidate(th, e->size - u32(sizeof(TexHeader)))) _texture = gfx::textureCreate(th, blob + sizeof(TexHeader));
				free(blob);
			}
		}
	}
	if (!_texture) return; // (white cards would be worse than none)
	trace("grass: draw");
	// Front to back.
	Slot* order[SLOTS];
	float key[SLOTS];
	u32 n = 0;
	for (Slot& s : _slots) {
		if (!s.wanted || !s.mesh || !s.drawn) continue;
		if (!aabbInFrustum(view, s.bbMin, s.bbMax)) continue;
		const float cx = 0.5f * (s.bbMin[0] + s.bbMax[0]) - _camera.x, cz = 0.5f * (s.bbMin[2] + s.bbMax[2]) - _camera.z;
		const float d = cx * cx + cz * cz;
		u32 k = n++;
		while (k > 0 && key[k - 1] > d) {
			order[k] = order[k - 1];
			key[k] = key[k - 1];
			--k;
		}
		order[k] = &s;
		key[k] = d;
	}
	if (!n) return;
	gfx::setFade(1.0f);
	gfx::setTwoSided(true);
	gfx::setMaterial(_texture, MATF_ALPHA_TEST | MATF_WIND | MATF_NO_COLLISION);
	for (u32 i = 0; i < n; ++i) {
		Slot& s = *order[i];
		gfx::ChunkFx fx;
		world.grassFx(s.origin, fx);
		gfx::setChunkTransform(s.origin, POS_SCALE, WORLD_UV_SCALE, &fx);
		gfx::draw(s.mesh, 0, s.drawn * 12);
		++_stats.tiles;
		++_stats.drawCalls;
		_stats.tufts += s.drawn;
		_stats.triangles += s.drawn * 4;
	}
	gfx::setTwoSided(false);
}

} // namespace g3
