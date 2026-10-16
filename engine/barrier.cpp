#include "barrier.h"

#include "g3ds/pak.h"
#include "g3ds/tex.h"
#include "sound.h"

#include <initializer_list>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace g3 {

namespace {

const float kTwoPi = 6.2831853f;

// sin over a table of 4096 steps a turn: the dome asks for a few thousand a frame.
constexpr u32 kSinSteps = 4096;
float gSin[kSinSteps];
bool gSinReady = false;
void makeSin() {
	if (gSinReady) return;
	for (u32 i = 0; i < kSinSteps; ++i) gSin[i] = sinf(float(i) * kTwoPi / float(kSinSteps));
	gSinReady = true;
}
// x >= 0 (radians, any size the float holds well).
inline float fastSin(float x) { return gSin[u32(i32(x * (float(kSinSteps) / kTwoPi))) & (kSinSteps - 1u)]; }

float wrap(double x) {
	x = fmod(x, double(kTwoPi));
	if (x < 0) x += double(kTwoPi);
	return float(x);
}

inline float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline void cross3(const float a[3], const float b[3], float out[3]) {
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}
inline float normalize3(float v[3]) {
	const float l = sqrtf(dot3(v, v));
	if (l > 1e-12f) {
		const float k = 1.0f / l;
		v[0] *= k;
		v[1] *= k;
		v[2] *= k;
	}
	return l;
}
inline float smooth01(float x) {
	x = clamp(x, 0.0f, 1.0f);
	return x * x * (3.0f - 2.0f * x);
}

inline u32 hashU(u32 x) {
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

// A corner on the sphere of `radius` round the camera (the sky's lattice: origin camera - radius, 2 radius / 32766 a step).
inline void setPos(WorldVertex& v, const float q[3], float radius) {
	const float k = 0.5f / radius;
	v.x = i16(clamp((q[0] * k + 0.5f) * 32766.0f, -32000.0f, 32766.0f));
	v.y = i16(clamp((q[1] * k + 0.5f) * 32766.0f, -32000.0f, 32766.0f));
	v.z = i16(clamp((q[2] * k + 0.5f) * 32766.0f, -32000.0f, 32766.0f));
	v.w = 1;
}
inline u8 byteOf(float x) { return u8(clamp(x, 0.0f, 255.0f)); }

// The Barrier's texture when the archive has none: a few soft wavy lines (white, the colour comes from the vertices).
// Plain on purpose (fix184): a handful of long strands that wrap, not a dense web.
gfx::Texture* makeWebTexture() {
	constexpr int N = 64;
	static u8 cov[N * N];
	memset(cov, 0, sizeof(cov));
	// Three strands across (along u) and two along v: sine waves of whole periods, so the texture wraps.
	struct Wave {
		bool across;
		float at, amp, phase;
		int periods;
	};
	static const Wave kWaves[] = {{true, 10.0f, 4.0f, 0.3f, 1}, {true, 33.0f, 5.0f, 2.1f, 2}, {true, 52.0f, 3.5f, 4.0f, 1},
	                              {false, 20.0f, 5.0f, 1.2f, 1}, {false, 47.0f, 4.0f, 3.3f, 2}};
	for (int y = 0; y < N; ++y)
		for (int x = 0; x < N; ++x) {
			float best = 0.0f;
			for (const Wave& w : kWaves) {
				const float along = float(w.across ? x : y), across = float(w.across ? y : x);
				const float c = w.at + w.amp * sinf(along * kTwoPi * float(w.periods) / float(N) + w.phase);
				float d = fabsf(across - c);
				d = min(d, float(N) - d);
				best = max(best, clamp(1.0f - d / 1.8f, 0.0f, 1.0f));
			}
			cov[y * N + x] = u8(best * 255.0f);
		}
	return gfx::uiTextureCreate(N, N, cov, false);
}

} // namespace

constexpr float Barrier::SECTOR_DELAY[Barrier::SECTORS];

float Barrier::rnd() {
	_seed = _seed * 1664525u + 1013904223u;
	return float(_seed >> 8) * (1.0f / 16777216.0f);
}

bool Barrier::load(Pak& pak) {
	if (_loaded) return hasMesh();
	_loaded = true;
	makeSin();
	const PakEntry* e = pak.find("fx/barrier");
	if (!e || e->size < sizeof(BarrierHeader)) return false;
	u8* blob = static_cast<u8*>(pak.readAlloc(*e));
	if (!blob) return false;
	BarrierHeader h;
	memcpy(&h, blob, sizeof(h));
	if (h.magic != BARRIER_MAGIC || h.version != BARRIER_VERSION || h.vertexCount == 0 || h.vertexCount > 65535 || h.indexCount < 3 ||
	    e->size < barrierBlobSize(h.vertexCount, h.indexCount)) {
		free(blob);
		return false;
	}
	const u32 n = h.vertexCount;
	_pos = static_cast<float*>(malloc(n * 3 * sizeof(float)));
	_uv = static_cast<float*>(malloc(n * 2 * sizeof(float)));
	_fade = static_cast<float*>(malloc(n * sizeof(float)));
	_flick = static_cast<float*>(malloc(n * sizeof(float)));
	_layers[0] = static_cast<Layer*>(malloc(n * sizeof(Layer)));
	_layers[1] = static_cast<Layer*>(malloc(n * sizeof(Layer)));
	_indices = static_cast<u16*>(malloc(h.indexCount * sizeof(u16)));
	_verts = static_cast<WorldVertex*>(malloc(n * sizeof(WorldVertex)));
	if (!_pos || !_uv || !_fade || !_flick || !_layers[0] || !_layers[1] || !_indices || !_verts) {
		free(blob);
		release();
		_loaded = true;
		return false;
	}
	const BarrierVertex* bv = reinterpret_cast<const BarrierVertex*>(blob + sizeof(BarrierHeader));
	float uvMax = 0.0f;
	for (u32 i = 0; i < n; ++i) {
		BarrierVertex v;
		memcpy(&v, bv + i, sizeof(v));
		for (int a = 0; a < 3; ++a) _pos[i * 3 + u32(a)] = v.pos[a];
		_uv[i * 2] = v.uv[0];
		_uv[i * 2 + 1] = v.uv[1];
		_fade[i] = gameFade(v.pos[1], h.bbMax[1]);
		uvMax = max(uvMax, max(fabsf(v.uv[0]), fabsf(v.uv[1])));
		// The phases of RenderLayer's waves at this corner (s = 1 and 1.5), and of its flicker.
		const double xz = double(v.pos[0]) + double(v.pos[2]), y = double(v.pos[1]);
		for (int l = 0; l < 2; ++l) {
			const double s = l ? 1.5 : 1.0;
			_layers[l][i].pu = wrap(double(BARRIER_WAVE_FREQ) * s * xz + double(BARRIER_WAVE_PHASE) * s);
			_layers[l][i].pv = wrap(double(BARRIER_WAVE_FREQ) * s * y + double(BARRIER_WAVE_PHASE) * s);
		}
		_flick[i] = wrap(xz);
		WorldVertex& w = _verts[i];
		memset(&w, 0, sizeof(w));
		w.w = 1;
	}
	memcpy(_indices, blob + sizeof(BarrierHeader) + n * sizeof(BarrierVertex), h.indexCount * sizeof(u16));
	free(blob);
	for (u32 i = 0; i < h.indexCount; ++i)
		if (_indices[i] >= n) _indices[i] = 0;
	_vertexCount = n;
	_indexCount = h.indexCount / 3 * 3;
	// Texture coordinates in fixed point: as fine as the largest one (plus the layers' shift, 2.5, and the wave) allows.
	const float reach = uvMax + 3.0f;
	_uvScale = reach < 31.0f ? 1.0f / 1024.0f : reach < 127.0f ? 1.0f / 256.0f : reach < 511.0f ? 1.0f / 64.0f : 1.0f / 16.0f;
	for (int a = 0; a < 3; ++a) {
		_bbMin[a] = h.bbMin[a];
		_bbMax[a] = h.bbMax[a];
	}
	_top = h.top < n ? h.top : 0;
	_textureHash = h.texture;
	_pak = &pak;
	// The dome as an ellipsoid standing on the bottom of its box (where the lightning crawls).
	_centre[0] = (h.bbMin[0] + h.bbMax[0]) * 0.5f;
	_centre[1] = h.bbMin[1];
	_centre[2] = (h.bbMin[2] + h.bbMax[2]) * 0.5f;
	_radii[0] = max((h.bbMax[0] - h.bbMin[0]) * 0.5f, 100.0f);
	_radii[1] = max(h.bbMax[1] - h.bbMin[1], 100.0f);
	_radii[2] = max((h.bbMax[2] - h.bbMin[2]) * 0.5f, 100.0f);
	// The quadrants are told apart round the game's thunder centre, when it is over the dome (the colony); a dome elsewhere
	// (the synthetic world, a mod) has them round its own middle.
	const bool colony = THUNDER_CENTRE_X > h.bbMin[0] && THUNDER_CENTRE_X < h.bbMax[0] && THUNDER_CENTRE_Z > h.bbMin[2] &&
	                    THUNDER_CENTRE_Z < h.bbMax[2];
	_thunderCentre[0] = colony ? THUNDER_CENTRE_X : _centre[0];
	_thunderCentre[1] = colony ? THUNDER_CENTRE_Y : _centre[1];
	_thunderCentre[2] = colony ? THUNDER_CENTRE_Z : _centre[2];
	// oCBarrier::Init: the strikes start at the corners higher than 350 m (round the top), sorted into the quadrants. A dome
	// that is not that high (the synthetic one) takes its highest seventh instead.
	const float high = h.bbMax[1] > START_HEIGHT * 1.05f ? START_HEIGHT : h.bbMax[1] - (h.bbMax[1] - h.bbMin[1]) / 7.0f;
	for (u32 s = 0; s < SECTORS; ++s) _startCount[s] = 0;
	u32 candidates[SECTORS] = {};
	for (int pass = 0; pass < 2; ++pass) {
		u32 seen[SECTORS] = {};
		for (u32 i = 0; i < n; ++i) {
			const float* p = _pos + i * 3;
			if (p[1] <= high || i == _top) continue;
			const u32 s = quadrantOf(p[0], p[2]);
			if (pass == 0) {
				++candidates[s];
				continue;
			}
			const u32 step = candidates[s] / STARTS + 1u;
			if (seen[s]++ % step == 0 && _startCount[s] < STARTS) _starts[s][_startCount[s]++] = i;
		}
	}
	return true;
}

void Barrier::release() {
	for (auto& layer : _domeMeshes)
		for (gfx::Mesh*& m : layer) {
			if (m) gfx::meshDestroy(m);
			m = nullptr;
		}
	for (gfx::Mesh*& m : _boltMeshes) {
		if (m) gfx::meshDestroy(m);
		m = nullptr;
	}
	if (_texture) gfx::textureDestroy(_texture);
	if (_streak) gfx::textureDestroy(_streak);
	_texture = _streak = nullptr;
	free(_pos);
	free(_uv);
	free(_fade);
	free(_flick);
	free(_layers[0]);
	free(_layers[1]);
	free(_indices);
	free(_verts);
	_pos = _uv = _fade = _flick = nullptr;
	_layers[0] = _layers[1] = nullptr;
	_indices = nullptr;
	_verts = nullptr;
	_vertexCount = _indexCount = 0;
	_loaded = false;
	_textureHash = 0;
	_textureTried = _madeTexture = false;
	_pak = nullptr;
}

gfx::Texture* Barrier::loadTexture() {
	if (!_pak || !_textureHash) return nullptr;
	const PakEntry* te = _pak->find(_textureHash);
	if (!te || te->type != ASSET_TEXTURE || te->size < sizeof(TexHeader)) return nullptr;
	u8* tb = static_cast<u8*>(_pak->readAlloc(*te));
	if (!tb) return nullptr;
	TexHeader th;
	memcpy(&th, tb, sizeof(th));
	gfx::Texture* t = texValidate(th, te->size - u32(sizeof(TexHeader))) ? gfx::textureCreate(th, tb + sizeof(TexHeader)) : nullptr;
	free(tb);
	return t;
}

u32 Barrier::bolts() const {
	u32 n = 0;
	for (const Bolt& b : _bolt) n += b.alive;
	return n;
}

void Barrier::findSounds(Sound& sound, Pak& pak) {
	_soundsSearched = true;
	// oCBarrier::Render plays "MFX_Barriere_Ambient" for every strike (one sound per quadrant, at the camera); the Barrier
	// has no hum of its own (G12Dll's G1Barrier, a reconstruction of it). Only if a game lacks it: the baker's list
	// (meta/barriersfx, the scripts' sounds named like the Barrier), then similar names, then the weather's thunder.
	static const char* const kThunder[] = {"MFX_BARRIERE_AMBIENT", "MFX_BARRIER_AMBIENT", "BARRIERE_AMBIENT"};
	for (const char* n : kThunder)
		if (sound.knows(n)) {
			_thunderName = n;
			return;
		}
	if (const PakEntry* e = pak.find("meta/barriersfx")) {
		static char text[1024];
		if (e->size > 0 && e->size < sizeof(text) && pak.read(*e, text)) {
			text[e->size] = 0;
			for (char* p = text; *p && !_thunderName;) {
				char* end = strchr(p, '\n');
				if (end) *end = 0;
				if (p[0] && p[1] == ' ' && p[2] && sound.knows(p + 2)) {
					const u32 len = u32(strlen(p + 2));
					if (len < sizeof(_names)) {
						memcpy(_names, p + 2, len + 1);
						_thunderName = _names;
					}
				}
				if (!end) break;
				p = end + 1;
			}
		}
	}
	if (_thunderName) return;
	static const char* const kOther[] = {"MFX_BARRIERE_SHOOT", "MFX_BARRIERE_THUNDER"};
	for (const char* n : kOther)
		if (sound.knows(n)) {
			_thunderName = n;
			return;
		}
}

void Barrier::chooseThunder(u8 choice, Sound& sound) {
	_thunderChoice = choice;
	// The game's own (findSounds), kept for when the menu comes back to it.
	if (!_gameThunder) _gameThunder = _thunderName ? _thunderName : "";
	auto first = [&](std::initializer_list<const char*> names) -> const char* {
		for (const char* n : names)
			if (n && *n && sound.knows(n)) return n;
		return nullptr;
	};
	const char* own = *_gameThunder ? _gameThunder : nullptr;
	switch (choice) {
	case THUNDER_NEAR: _thunderName = first({"G3_THUNDER_NEAR", "G3_THUNDER_MADE_A", own, "G3_THUNDER_FAR"}); break;
	case THUNDER_ROLL: _thunderName = first({"G3_THUNDER_ROLL", "G3_THUNDER_FAR", own}); break;
	case THUNDER_MAGIC:
		_thunderName = first({"MFX_LIGHTNING_ORIGIN", "MFX_LIGHTNING_TARGET", "MFX_THUNDERBOLT_CAST", "MFX_THUNDERBOLT_COLLIDE",
		                      "MFX_THUNDERBALL_COLLIDE", own, "G3_THUNDER_FAR"});
		break;
	case THUNDER_NONE: _thunderName = nullptr; break;
	default: // as in the game: MFX_Barriere_Ambient; a game without it gets the far thunder
		_thunderName = first({own, "G3_THUNDER_FAR", "G3_THUNDER_MADE_A"});
		break;
	}
}

void Barrier::update(float dt, u8 mode, u8 thunder, bool outdoor, Vec3 camera, Sound& sound, Pak& pak) {
	if (!_loaded) load(pak);
	if (!_soundsSearched && sound.ready()) findSounds(sound, pak);
	if (_soundsSearched && thunder != _thunderChoice) chooseThunder(thunder, sound);
	++_frame;
	dt = clamp(dt, 0.0f, 0.25f);
	_time += double(dt);
	if (_time > 100000.0) _time -= 100000.0 - fmod(100000.0, double(kTwoPi) / double(BARRIER_SCROLL)); // (never far from 0)
	// Shown or not: fade in 0.25 s, 25 s seen, fade out 0.25 s; then 15..25 minutes until the next time.
	bool want = false;
	if (mode == ALWAYS) {
		want = true;
		_shown = 0.0f;
	} else if (mode == GAME) {
		if (_shown >= 0.0f) {
			_shown += dt;
			want = _shown < SHOW_SECONDS;
			if (!want && _level <= 0.0f) {
				_shown = -1.0f;
				_wait = PERIOD + PERIOD_SPREAD * (2.0f * rnd() - 1.0f);
			}
		} else {
			_wait -= dt;
			if (_wait <= 0.0f) {
				_shown = 0.0f;
				want = true;
			}
		}
	} else {
		_shown = -1.0f;
	}
	want = want && outdoor && hasMesh();
	if (want && _level <= 0.0f) {
		// It comes: every quadrant's timer starts now (Q4's first strike after 2 s, Q2's after 6, Q1's after 8, Q3's after 14).
		for (u32 s = 0; s < SECTORS; ++s) {
			_sectorClock[s] = 0.0f;
			_sectorBusy[s] = 0.0f;
			_sectorSound[s] = 0;
		}
	}
	_level = clamp(_level + (want ? dt / FADE_IN : -dt / FADE_OUT), 0.0f, 1.0f);
	// Thunder: a quadrant strikes when its activation delay is over and so is the thunder of its last strike; then its
	// timer starts again.
	if (want) {
		for (u32 s = 0; s < SECTORS; ++s) {
			_sectorClock[s] += dt;
			_sectorBusy[s] = max(0.0f, _sectorBusy[s] - dt);
			if (_level < 0.5f) continue;
			if (_sectorClock[s] < SECTOR_DELAY[s]) continue;
			if (_sectorBusy[s] > 0.0f) continue;
			if (sectorBoltAlive(s)) continue; // (its last strike still on the dome)
			_sectorClock[s] = 0.0f;
			spawnBolt(s, sound, camera);
		}
	}
	for (Bolt& b : _bolt) {
		if (!b.alive) continue;
		b.age += dt;
		if (b.age >= b.life) b.alive = false;
	}
}

u32 Barrier::quadrantOf(float x, float z) const {
	// oCBarrier::Init's: 1 (-x -z), 2 (+x -z), 3 (+x +z), 4 (-x +z).
	const bool px = x > _thunderCentre[0], pz = z > _thunderCentre[2];
	return pz ? (px ? 2u : 3u) : (px ? 1u : 0u);
}

bool Barrier::sectorBoltAlive(u32 sector) const {
	for (const Bolt& b : _bolt)
		if (b.alive && b.sector == sector) return true;
	return false;
}

void Barrier::toSphere(const float p[3], float q[3]) const {
	for (int a = 0; a < 3; ++a) q[a] = (p[a] - _centre[a]) / _radii[a];
	normalize3(q);
}

void Barrier::onSphere(const float q[3], float out[3]) const {
	// (a little inside the dome)
	for (int a = 0; a < 3; ++a) out[a] = _centre[a] + q[a] * _radii[a] * 0.99f;
}

void Barrier::along(const float qs[3], const float qt[3], float omega, float f, float q[3]) const {
	const float sn = sinf(omega);
	const float a = sinf((1.0f - f) * omega) / sn, c = sinf(f * omega) / sn;
	for (int k = 0; k < 3; ++k) q[k] = qs[k] * a + qt[k] * c;
}

void Barrier::spawnBolt(u32 sector, Sound& sound, Vec3 camera) {
	(void)camera;
	_sectorBusy[sector] = 0.0f;
	_sectorSound[sector] = 0;
	++_sectorStrikes[sector];
	++_thunders;
	// Its thunder: MFX_Barriere_Ambient, played at the camera (oCBarrier::Render: PlaySound3D on the camera's vob), as loud
	// wherever one stands under the dome. The quadrant waits for it to end.
	if (_thunderName && sound.ready()) _sectorSound[sector] = sound.play(_thunderName, nullptr, 1.0f);
	// No handle to ask (no sound, or its sample still on the way): the thunder is taken to last as the game's does.
	if (!_sectorSound[sector]) _sectorBusy[sector] = THUNDER_SECONDS;
	if (!_startCount[sector]) return;

	const u32 start = _starts[sector][u32(rnd() * float(_startCount[sector])) % _startCount[sector]];
	float qs[3], qtop[3];
	toSphere(_pos + start * 3, qs);
	toSphere(_pos + _top * 3, qtop);
	// The way: from the start, on round the dome away from its top (on the great circle through both), downwards.
	float omega0 = acosf(clamp(dot3(qs, qtop), -1.0f, 1.0f));
	if (omega0 < 0.02f) return;
	const float length = 0.3f + 0.25f * rnd(); // radians of the dome
	if (omega0 + length > 3.0f) return;
	auto pathAt = [&](float f, float q[3]) { along(qtop, qs, omega0, 1.0f + f * length / omega0, q); };
	auto sideAt = [&](float f, float side[3]) {
		float q[3], q2[3];
		pathAt(f, q);
		pathAt(f + 0.02f, q2);
		const float dir[3] = {q2[0] - q[0], q2[1] - q[1], q2[2] - q[2]};
		cross3(q, dir, side);
		normalize3(side);
	};
	Bolt* b = nullptr;
	for (Bolt& x : _bolt)
		if (!x.alive) {
			b = &x;
			break;
		}
	if (!b) {
		b = &_bolt[0];
		for (Bolt& x : _bolt)
			if (x.age / x.life > b->age / b->life) b = &x;
	}
	b->sector = u8(sector);
	// The bolt: straight (its points only follow the curve), leaning a little to one side.
	const float lean = (rnd() - 0.5f) * 0.04f * length;
	for (u32 i = 0; i < ROOT_POINTS; ++i) {
		const float f = float(i) / float(ROOT_POINTS - 1);
		float q[3], side[3];
		pathAt(f, q);
		sideAt(f, side);
		const float off = lean * f * (1.0f - f) * 4.0f;
		for (int k = 0; k < 3; ++k) q[k] += side[k] * off;
		normalize3(q);
		onSphere(q, b->root[i].p);
	}
	// The strands it breaks into: zigzags along stretches of the same way, a little apart from each other.
	b->strands = u8(2u + u32(rnd() * 3.0f) % 3u);
	float last = 0.0f;
	for (u32 k = 0; k < b->strands; ++k) {
		Strand& st = b->strand[k];
		const float f0 = 0.25f * rnd(), f1 = 0.7f + 0.3f * rnd();
		const float apart = (float(k) - 0.5f * float(b->strands - 1) + (rnd() - 0.5f) * 0.6f) * 0.06f * length;
		const float amp = (0.03f + 0.035f * rnd()) * length;
		float sign = rnd() < 0.5f ? -1.0f : 1.0f;
		for (u32 j = 0; j < STRAND_POINTS; ++j) {
			const float f = f0 + (f1 - f0) * float(j) / float(STRAND_POINTS - 1);
			const float jitter = (rnd() - 0.5f) * 0.6f * (f1 - f0) / float(STRAND_POINTS - 1);
			float q[3], side[3];
			pathAt(clamp(f + jitter, 0.0f, 1.0f), q);
			sideAt(f, side);
			const float edge = (j == 0 || j + 1 == STRAND_POINTS) ? 0.3f : 1.0f;
			const float off = apart * sinf(f * 3.14159265f) + sign * amp * edge * (0.25f + 0.75f * rnd());
			if (rnd() < 0.8f) sign = -sign; // (mostly back and forth, now and then twice the same way)
			for (int a = 0; a < 3; ++a) q[a] += side[a] * off;
			normalize3(q);
			onSphere(q, st.p[j].p);
		}
		st.start = 0.04f + 0.045f * float(k) + 0.03f * rnd();
		st.life = 0.13f + 0.1f * rnd();
		st.seed = rnd() * 1000.0f;
		last = max(last, st.start + st.life);
	}
	b->alive = true;
	b->age = 0.0f;
	b->life = max(last, ROOT_FADE_END);
}

void Barrier::draw(Vec3 camera, float radius) {
	if (!hasMesh() || radius <= 0.0f) return;
	// The second eye of a stereo frame draws what the first one built (the same camera: Sky::draw's too).
	const bool fresh = _drawnFrame != _frame;
	if (fresh) {
		_drawnFrame = _frame;
		_turn = (_turn + 1) % MESHES;
		_domeBuilt = _boltQuads = 0;
	}
	if (_level > 0.003f) drawDome(camera, radius, fresh);
	drawBolts(camera, radius, fresh);
}

void Barrier::drawDome(Vec3 camera, float radius, bool fresh) {
	if (!_domeMeshes[0][0]) {
		for (auto& layer : _domeMeshes)
			for (gfx::Mesh*& m : layer) {
				m = gfx::meshCreate(_verts, _vertexCount, _indices, _indexCount);
				if (!m) return;
			}
	}
	if (!_texture && !_textureTried) {
		_textureTried = true;
		_texture = loadTexture();
		_madeTexture = !_texture;
		if (!_texture) _texture = makeWebTexture();
	}
	const float origin[3] = {camera.x - radius, camera.y - radius, camera.z - radius};
	if (!fresh) {
		if (!_domeBuilt) return;
		gfx::setChunkTransform(origin, 2.0f * radius / 32766.0f, _uvScale);
		gfx::setMaterial(_texture, MATF_ADD, 1.0f);
		for (u32 l = 0; l < DOME_LAYERS; ++l) gfx::draw(_domeMeshes[l][_turn], 0, _indexCount);
		return;
	}
	_domeBuilt = 1;
	const float R = radius * 0.95f;
	const float t = wrap(_time);
	const double sc = _time * double(BARRIER_SCROLL);
	const float scroll = float(sc - floor(sc));
	// The game's material alpha: 120 of 255 a layer (oCBarrier::Render's fade goes up to barrierMaxOpacity). One layer
	// instead of the two (plainer, less branchy) is made as strong as both.
	const float gain = 255.0f * _level * DOME_OPACITY * (DOME_LAYERS == 1 ? 2.0f : 1.0f);
	const bool own = !_madeTexture;
	// Shared by both layers: where each corner is seen, and how bright it is.
	for (u32 i = 0; i < _vertexCount; ++i) {
		const float* p = _pos + i * 3;
		float d[3] = {p[0] - camera.x, p[1] - camera.y, p[2] - camera.z};
		const float l2 = dot3(d, d);
		const float k = l2 > 1.0f ? R / sqrtf(l2) : 0.0f;
		d[0] *= k;
		d[1] *= k;
		d[2] *= k;
		WorldVertex& v = _verts[i];
		setPos(v, d, radius);
		const float a = _fade[i] * (fastSin(_flick[i] + t) + 1.0f) * 0.5f * gain;
		if (own) {
			v.r = v.g = v.b = byteOf(a);
		} else { // (the made web is white: blue-violet as the original's)
			v.r = byteOf(a * 0.55f);
			v.g = byteOf(a * 0.65f);
			v.b = byteOf(a);
		}
		v.a = 0;
	}
	gfx::setChunkTransform(origin, 2.0f * radius / 32766.0f, _uvScale);
	gfx::setMaterial(_texture, MATF_ADD, 1.0f);
	const float inv = 1.0f / _uvScale;
	for (u32 l = 0; l < DOME_LAYERS; ++l) {
		const float s = l ? 1.5f : 1.0f, amp = BARRIER_WAVE * s;
		const Layer* ph = _layers[l];
		for (u32 i = 0; i < _vertexCount; ++i) {
			const float u = _uv[i * 2] + amp * fastSin(ph[i].pu + t) + scroll + s;
			const float v = _uv[i * 2 + 1] + amp * fastSin(ph[i].pv + t) + s;
			_verts[i].u = i16(clamp(u * inv, -32767.0f, 32767.0f));
			_verts[i].v = i16(clamp(v * inv, -32767.0f, 32767.0f));
		}
		gfx::Mesh* m = _domeMeshes[l][_turn];
		gfx::meshUpdate(m, _verts, _vertexCount);
		gfx::draw(m, 0, _indexCount);
	}
}

void Barrier::drawBolts(Vec3 camera, float radius, bool fresh) {
	if (!fresh) {
		if (!_boltQuads) return;
		const float origin[3] = {camera.x - radius, camera.y - radius, camera.z - radius};
		gfx::setChunkTransform(origin, 2.0f * radius / 32766.0f, 1.0f / 1024.0f);
		drawBoltMesh();
		return;
	}
	if (!bolts()) return;
	if (!_streak) {
		u8 cov[16 * 8];
		for (int y = 0; y < 8; ++y)
			for (int x = 0; x < 16; ++x) {
				const float d = fabsf((float(x) - 7.5f) / 7.5f);
				cov[y * 16 + x] = u8(clamp(powf(1.0f - d, 1.6f), 0.0f, 1.0f) * 255.0f);
			}
		_streak = gfx::uiTextureCreate(16, 8, cov, true);
		if (!_streak) return;
	}
	if (!_boltMeshes[0]) {
		static u16 indices[BOLT_QUADS * 6];
		for (u32 q = 0; q < BOLT_QUADS; ++q) {
			const u16 b = u16(q * 4);
			const u16 quad[6] = {b, u16(b + 1), u16(b + 2), u16(b + 2), u16(b + 1), u16(b + 3)};
			for (int k = 0; k < 6; ++k) indices[q * 6 + u32(k)] = quad[k];
		}
		static WorldVertex blank[BOLT_QUADS * 4];
		for (gfx::Mesh*& m : _boltMeshes) {
			m = gfx::meshCreate(blank, BOLT_QUADS * 4, indices, BOLT_QUADS * 6);
			if (!m) return;
		}
	}
	static WorldVertex verts[BOLT_QUADS * 4];
	u32 quads = 0;
	const float R = radius * 0.9f;
	auto project = [&](const float p[3], float q[3]) {
		float d[3] = {p[0] - camera.x, p[1] - camera.y, p[2] - camera.z};
		const float dist = normalize3(d);
		for (int a = 0; a < 3; ++a) q[a] = d[a] * R;
		return max(dist, 1.0f);
	};
	// A strip along `n` points: `half` cm across in the world (not thinner than `minAngle` of a radian on the screen), `rgb`.
	auto strip = [&](const Point* pts, u32 n, float half, float minAngle, const float rgb[3], float taper) {
		if (n < 2) return;
		n = min(n, MAX_POINTS);
		float q[MAX_POINTS][3], dist[MAX_POINTS], side[MAX_POINTS][3];
		for (u32 i = 0; i < n; ++i) dist[i] = project(pts[i].p, q[i]);
		for (u32 i = 0; i < n; ++i) {
			const u32 a = i ? i - 1 : 0, b = i + 1 < n ? i + 1 : n - 1;
			const float tng[3] = {q[b][0] - q[a][0], q[b][1] - q[a][1], q[b][2] - q[a][2]};
			cross3(tng, q[i], side[i]);
			normalize3(side[i]);
			const float f = float(i) / float(n - 1);
			const float h = max(half * R / dist[i], R * minAngle) * (1.0f - taper * f);
			for (int k = 0; k < 3; ++k) side[i][k] *= h;
		}
		for (u32 i = 0; i + 1 < n && quads < BOLT_QUADS; ++i) {
			WorldVertex* v = verts + quads * 4;
			const u32 at[4] = {i, i, i + 1, i + 1};
			const float sgn[4] = {-1, 1, -1, 1};
			for (int k = 0; k < 4; ++k) {
				float p[3];
				for (int a = 0; a < 3; ++a) p[a] = q[at[k]][a] + side[at[k]][a] * sgn[k];
				setPos(v[k], p, radius);
				v[k].u = i16(sgn[k] < 0 ? 0 : 1024);
				v[k].v = 512;
				v[k].r = byteOf(rgb[0]);
				v[k].g = byteOf(rgb[1]);
				v[k].b = byteOf(rgb[2]);
				v[k].a = 0;
			}
			++quads;
		}
	};
	// White with a pale blue halo, as the game's.
	auto colours = [](float shine, float core[3], float glow[3]) {
		core[0] = 228.0f * shine;
		core[1] = 236.0f * shine;
		core[2] = 255.0f * shine;
		glow[0] = 50.0f * shine;
		glow[1] = 70.0f * shine;
		glow[2] = 190.0f * shine;
	};
	const float strength = clamp(_level * 1.6f, 0.0f, 1.0f);
	for (const Bolt& b : _bolt) {
		if (!b.alive) continue;
		float core[3], glow[3];
		// The straight bolt: shoots up at once, shines, and is gone as its strands take over.
		if (b.age < ROOT_FADE_END) {
			const float grow = clamp(b.age / ROOT_GROW, 0.0f, 1.0f);
			const float shine = strength * (1.0f - smooth01((b.age - ROOT_SHINE) / (ROOT_FADE_END - ROOT_SHINE)));
			if (shine > 0.02f) {
				const u32 n = 2u + u32(grow * float(ROOT_POINTS - 2) + 0.5f);
				colours(shine, core, glow);
				strip(b.root, n, 1400.0f, 0.016f, glow, 0.3f);
				strip(b.root, n, 400.0f, 0.0055f, core, 0.3f);
			}
		}
		// The strands: each flickers for its moment and dies back from where it started.
		for (u32 k = 0; k < b.strands; ++k) {
			const Strand& st = b.strand[k];
			const float local = b.age - st.start;
			if (local < 0.0f || local >= st.life) continue;
			const float f = local / st.life;
			const u32 step = u32(local * 30.0f) + u32(st.seed);
			const float flick = 0.6f + 0.4f * float(hashU(step) & 255u) / 255.0f;
			const float shine = strength * flick * (1.0f - smooth01((f - 0.55f) / 0.45f));
			if (shine < 0.02f) continue;
			const u32 gone = min(u32(f * f * float(STRAND_POINTS - 2)), STRAND_POINTS - 2u);
			colours(shine, core, glow);
			strip(st.p + gone, STRAND_POINTS - gone, 900.0f, 0.011f, glow, 0.2f);
			strip(st.p + gone, STRAND_POINTS - gone, 260.0f, 0.0038f, core, 0.2f);
		}
	}
	if (!quads) return;
	gfx::meshUpdate(_boltMeshes[_turn], verts, quads * 4);
	_boltQuads = quads;
	const float origin[3] = {camera.x - radius, camera.y - radius, camera.z - radius};
	gfx::setChunkTransform(origin, 2.0f * radius / 32766.0f, 1.0f / 1024.0f);
	drawBoltMesh();
}

void Barrier::drawBoltMesh() {
	if (!_boltQuads) return;
	gfx::setMaterial(_streak, MATF_ADD, 1.0f);
	gfx::draw(_boltMeshes[_turn], 0, _boltQuads * 6);
}

} // namespace g3
