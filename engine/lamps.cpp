#include "lamps.h"

#include "particles.h"
#include "world.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace g3 {

namespace {

// What a fire place's light flickers by when it has no keys of its own: three waves, about +-15 %.
float flameFlicker(double seconds, float phase) {
	const double t = seconds + double(phase);
	return 1.0f + 0.07f * float(sin(t * 7.1)) + 0.05f * float(sin(t * 13.3 + 2.1)) + 0.03f * float(sin(t * 27.7 + 4.4));
}

// Key `k` of a cycle of `n` keys at `fps`, blended (`smooth`) or stepped: the index of the key and the share of the next.
void keyAt(double seconds, float fps, u32 n, bool smooth, u32& k, u32& next, float& share) {
	double x = seconds * double(fps);
	x -= floor(x / double(n)) * double(n);
	k = u32(x);
	if (k >= n) k = n - 1;
	next = (k + 1) % n;
	share = smooth ? float(x - double(k)) : 0.0f;
}

float boxDistanceSq(const float mn[3], const float mx[3], Vec3 p) {
	const float c[3] = {p.x, p.y, p.z};
	float d = 0.0f;
	for (int a = 0; a < 3; ++a) {
		const float e = c[a] < mn[a] ? mn[a] - c[a] : c[a] > mx[a] ? c[a] - mx[a] : 0.0f;
		d += e * e;
	}
	return d;
}

} // namespace

void Lamps::leaveWorld() {
	free(_lamps);
	free(_phase);
	free(_chunk);
	free(_gate);
	_lamps = nullptr;
	_phase = nullptr;
	_chunk = nullptr;
	_gate = nullptr;
	_count = 0;
	_activeCount = 0;
	for (Seen& s : _seen) s = Seen {};
	_stats = LampStats {};
}

void Lamps::enterWorld(Pak& pak, const char* worldName, Particles* fallback) {
	leaveWorld();
	char name[96];
	snprintf(name, sizeof(name), "%s/lamps", worldName ? worldName : "");
	u32 count = 0;
	WorldLamp* lamps = nullptr;
	bool table = false;
	if (const PakEntry* e = pak.find(name)) {
		LampsHeader h {};
		u8* blob = e->size >= sizeof(LampsHeader) ? static_cast<u8*>(pak.readAlloc(*e)) : nullptr;
		if (blob) {
			memcpy(&h, blob, sizeof(h));
			if (h.magic == LAMPS_MAGIC && h.version == LAMPS_VERSION && h.count > 0 && h.count <= MAX_LAMPS && e->size == lampsBlobSize(h.count)) {
				lamps = static_cast<WorldLamp*>(malloc(size_t(h.count) * sizeof(WorldLamp)));
				if (lamps) {
					memcpy(lamps, blob + sizeof(LampsHeader), size_t(h.count) * sizeof(WorldLamp));
					count = h.count;
					table = true;
				}
			}
			free(blob);
		}
	}
	if (!table && fallback) {
		// (Baked before fix148: the fires of the particle effects, a camp fire's light each.)
		static Vec3 pos[512];
		static float rgb[512][3];
		const u32 n = fallback->fireLights(pos, rgb, 512);
		if (n) lamps = static_cast<WorldLamp*>(calloc(n, sizeof(WorldLamp)));
		if (lamps) {
			for (u32 i = 0; i < n; ++i) {
				WorldLamp& l = lamps[i];
				l.pos[0] = pos[i].x;
				l.pos[1] = pos[i].y + 30.0f;
				l.pos[2] = pos[i].z;
				l.range = 800.0f;
				// Half the effect's own colour, half a fire's.
				static const float fire[3] = {1.0f, 0.67f, 0.35f};
				const float top = max(rgb[i][0], max(rgb[i][1], rgb[i][2]));
				for (int c = 0; c < 3; ++c) {
					const float own = top > 0.01f ? rgb[i][c] / top : fire[c];
					l.color[c] = u8(clamp((own * 0.5f + fire[c] * 0.5f) * 255.0f, 0.0f, 255.0f));
				}
				l.flags = LAMPF_FIRE | LAMPF_MADE;
			}
			count = n;
		}
	}
	if (!count) {
		free(lamps);
		return;
	}
	_phase = static_cast<float*>(malloc(count * sizeof(float)));
	_chunk = static_cast<int*>(malloc(count * sizeof(int)));
	_gate = static_cast<u8*>(calloc(count, 1));
	if (!_phase || !_chunk || !_gate) {
		free(lamps);
		leaveWorld();
		return;
	}
	_lamps = lamps;
	_count = count;
	u32 baked = 0;
	for (u32 i = 0; i < count; ++i) {
		// (Each its own start: not all the fires of a camp in step. From the place, so a reload flickers the same.)
		const u32 h = u32(i32(lamps[i].pos[0])) * 73856093u ^ u32(i32(lamps[i].pos[2])) * 19349663u ^ i * 83492791u;
		_phase[i] = float(h % 10007u) * 0.01f;
		_chunk[i] = -2;
		baked += (lamps[i].flags & LAMPF_BAKED) != 0;
	}
	_stats.lamps = count;
	_stats.table = table;
	_stats.baked = baked > 0;
}

void Lamps::sample(const WorldLamp& l, u32 index, double seconds, float& rangeScale, float rgb[3]) const {
	const double t = seconds + double(_phase[index]);
	rangeScale = 1.0f;
	if (l.rangeKeys > 1 && l.rangeFps > 0.0f) {
		u32 k, n;
		float s;
		keyAt(t, l.rangeFps, l.rangeKeys, (l.flags & LAMPF_RANGE_SMOOTH) != 0, k, n, s);
		rangeScale = (float(l.rangeScale[k]) + (float(l.rangeScale[n]) - float(l.rangeScale[k])) * s) * (1.0f / 64.0f);
	}
	if (l.colorKeys > 1 && l.colorFps > 0.0f) {
		u32 k, n;
		float s;
		keyAt(t, l.colorFps, l.colorKeys, (l.flags & LAMPF_COLOR_SMOOTH) != 0, k, n, s);
		for (int c = 0; c < 3; ++c) rgb[c] = float(l.colors[k][c]) + (float(l.colors[n][c]) - float(l.colors[k][c])) * s;
	} else {
		for (int c = 0; c < 3; ++c) rgb[c] = float(l.color[c]);
	}
	// A flame without keys of its own flickers anyway.
	if ((l.flags & LAMPF_FIRE) && l.rangeKeys <= 1 && l.colorKeys <= 1) {
		const float f = flameFlicker(seconds, _phase[index]);
		for (int c = 0; c < 3; ++c) rgb[c] *= f;
		rangeScale *= 1.0f + (f - 1.0f) * 0.3f;
	}
}

void Lamps::update(const Frame& f, const World* world) {
	++_frame;
	_activeCount = 0;
	_stats.active = _stats.shining = _stats.boxes = _stats.crowded = _stats.sightTests = 0;
	_stats.world = f.world && f.characters && gfx::lampsSupported();
	if (!_count || !f.characters) {
		_stats.world = false;
		return;
	}
	// By day the lamps count for less where the sky is seen (the baker's --lights does the same through the daylight weight).
	const float day = (f.dayLight[0] + f.dayLight[1] + f.dayLight[2]) * (1.0f / 3.0f);
	_dayKeep = 1.0f - 0.7f * clamp((day - 0.35f) / 0.6f, 0.0f, 1.0f);
	// The nearest, by how far their light is from the camera.
	float nearness[MAX_ACTIVE];
	const int step = max(1, int(f.dt * 4.0f * 255.0f + 0.5f)); // in or out in a quarter of a second
	for (u32 i = 0; i < _count; ++i) {
		const WorldLamp& l = _lamps[i];
		const Vec3 p = {l.pos[0], l.pos[1], l.pos[2]};
		const Vec3 d = p - f.eye;
		const float reach = REACH + l.range;
		const float d2 = dot(d, d);
		bool on = d2 < reach * reach;
		if (on && world) on = world->chunkLoadedAt(p, _chunk[i]);
		const int target = on ? 255 : 0;
		const int g = _gate[i];
		// (dt < 0: a world just entered, at once; 0: the same moment drawn again, as it was.)
		_gate[i] = u8(f.dt < 0.0f || f.dt > 0.5f ? target : f.dt == 0.0f ? g : g < target ? min(g + step, 255) : max(g - step, 0));
		if (!_gate[i]) continue;
		const float key = sqrtf(d2) - l.range;
		u32 k = _activeCount;
		if (k == MAX_ACTIVE) {
			if (nearness[k - 1] <= key) continue;
			--k;
		} else {
			++_activeCount;
		}
		while (k > 0 && nearness[k - 1] > key) {
			_active[k] = _active[k - 1];
			nearness[k] = nearness[k - 1];
			--k;
		}
		nearness[k] = key;
		_active[k].lamp = i;
	}
	for (u32 a = 0; a < _activeCount; ++a) {
		Active& x = _active[a];
		const WorldLamp& l = _lamps[x.lamp];
		float scale, rgb[3];
		sample(l, x.lamp, f.seconds, scale, rgb);
		const float gate = float(_gate[x.lamp]) * (1.0f / 255.0f);
		x.pos = {l.pos[0], l.pos[1], l.pos[2]};
		const bool baked = (l.flags & LAMPF_BAKED) != 0;
		// (Baked: the vertices have the light of its colour and range already; the world gets the difference the flicker makes.)
		x.range = max(l.range * (baked ? 1.0f : scale), 10.0f);
		for (int c = 0; c < 3; ++c) {
			const float now = rgb[c] * (baked ? scale : 1.0f);
			x.world[c] = (baked ? now - float(l.color[c]) : now) * (1.0f / 255.0f) * WORLD_GAIN * gate;
			x.people[c] = now * (1.0f / 255.0f) * CHARACTER_GAIN * gate * (baked ? 0.5f : 1.0f);
		}
		_stats.shining += x.people[0] + x.people[1] + x.people[2] > 0.01f;
	}
	_stats.active = _activeCount;
}

u32 Lamps::touching(const float bbMin[3], const float bbMax[3]) const {
	u32 bits = 0;
	for (u32 a = 0; a < _activeCount; ++a) {
		const Active& x = _active[a];
		if (fabsf(x.world[0]) + fabsf(x.world[1]) + fabsf(x.world[2]) < 0.004f) continue;
		if (boxDistanceSq(bbMin, bbMax, x.pos) < x.range * x.range) bits |= 1u << a;
	}
	return bits;
}

u32 Lamps::pick(const float bbMin[3], const float bbMax[3], u32 candidates) {
	u32 index[gfx::MAX_DRAW_LAMPS];
	float weight[gfx::MAX_DRAW_LAMPS];
	u32 n = 0;
	bool crowded = false;
	for (u32 a = 0; candidates && a < _activeCount; ++a) {
		if (!(candidates & (1u << a))) continue;
		candidates &= ~(1u << a);
		const Active& x = _active[a];
		const float w = gfx::lampFalloff(boxDistanceSq(bbMin, bbMax, x.pos), x.range) *
		                max(fabsf(x.world[0]), max(fabsf(x.world[1]), fabsf(x.world[2])));
		if (w <= 0.0f) continue;
		if (n == gfx::MAX_DRAW_LAMPS) {
			crowded = true;
			u32 weakest = 0;
			for (u32 k = 1; k < n; ++k)
				if (weight[k] < weight[weakest]) weakest = k;
			if (weight[weakest] >= w) continue;
			index[weakest] = a;
			weight[weakest] = w;
			continue;
		}
		index[n] = a;
		weight[n] = w;
		++n;
	}
	if (!n) return 0xFFFFFFFFu;
	++_stats.boxes;
	_stats.crowded += crowded;
	// (In the order of the indices: two boxes with the same lamps have the same key, and their draws merge.)
	for (u32 i = 1; i < n; ++i)
		for (u32 k = i; k > 0 && index[k - 1] > index[k]; --k) {
			const u32 t = index[k];
			index[k] = index[k - 1];
			index[k - 1] = t;
		}
	u32 key = 0xFFFFFFFFu;
	for (u32 k = 0; k < n; ++k) key = (key & ~(0xFFu << (k * 8))) | (index[k] << (k * 8));
	return key;
}

void Lamps::setFor(u32 key, gfx::LampSet& out) const {
	out = gfx::LampSet {};
	out.dayKeep = _dayKeep;
	for (u32 k = 0; k < gfx::MAX_DRAW_LAMPS; ++k) {
		const u32 a = (key >> (k * 8)) & 0xFFu;
		if (a >= _activeCount) break;
		const Active& x = _active[a];
		gfx::DrawLamp& d = out.lamp[out.count++];
		d.pos[0] = x.pos.x;
		d.pos[1] = x.pos.y;
		d.pos[2] = x.pos.z;
		d.range = x.range;
		for (int c = 0; c < 3; ++c) d.rgb[c] = x.world[c];
	}
}

bool Lamps::sees(u32 lamp, Vec3 lampPos, Vec3 p, const World* world) {
	if (!world) return true;
	// Cached by lamp and by the place in 64 cm steps for half a second (a person walks through a few a second).
	const u32 key = ((lamp * 2654435761u) ^ (u32(i32(floorf(p.x / 64.0f))) * 73856093u) ^ (u32(i32(floorf(p.y / 64.0f))) * 19349663u) ^
	                 (u32(i32(floorf(p.z / 64.0f))) * 83492791u)) | 1u;
	Seen& s = _seen[(key >> 3) & 255u];
	if (s.key == key && _frame - s.frame < 30) return !s.blocked;
	// From a little off the lamp towards the person (a torch hangs on a wall, a fire sits in its stones).
	Vec3 d = p - lampPos;
	const float len = sqrtf(dot(d, d));
	const Vec3 from = len > 40.0f ? lampPos + d * (25.0f / len) : lampPos;
	++_stats.sightTests;
	s.key = key;
	s.frame = _frame;
	s.blocked = len > 40.0f && world->sightBlocked(from, p);
	return !s.blocked;
}

void Lamps::lightAt(Vec3 p, float sky, float out[3], const World* world) {
	const float keep = 1.0f + clamp(sky, 0.0f, 1.0f) * (_dayKeep - 1.0f);
	for (u32 a = 0; a < _activeCount; ++a) {
		const Active& x = _active[a];
		const Vec3 d = x.pos - p;
		// (A softer falloff than the world's: a person is lit as a whole, and stands a step or two from the fire.)
		const float t = 1.0f - dot(d, d) / (x.range * x.range);
		const float f = t > 0.0f ? t * t : 0.0f;
		if (f <= 0.002f) continue;
		if (!sees(x.lamp, x.pos, p, world)) continue;
		for (int c = 0; c < 3; ++c) out[c] += x.people[c] * f * keep;
	}
}

bool Lamps::active(u32 i, Vec3& pos, float& range, float rgb[3]) const {
	if (i >= _activeCount) return false;
	pos = _active[i].pos;
	range = _active[i].range;
	for (int c = 0; c < 3; ++c) rgb[c] = _active[i].world[c];
	return true;
}

} // namespace g3
