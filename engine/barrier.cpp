#include "barrier.h"

#include "g3ds/pak.h"
#include "g3ds/tex.h"
#include "sound.h"

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

// The Barrier's texture when the archive has none: a web of soft lines (white, the colour comes from the vertices).
gfx::Texture* makeWebTexture() {
	constexpr int N = 64;
	static u8 cov[N * N];
	for (int y = 0; y < N; ++y)
		for (int x = 0; x < N; ++x) {
			float best = 1.0f;
			// Lines between hashed points of an 4x4 lattice that wraps.
			for (int c = 0; c < 16; ++c) {
				const int cx = c & 3, cy = c >> 2;
				const float px = (float(cx) + float(hashU(u32(c) * 7u + 1u) & 255u) / 255.0f) * 16.0f;
				const float py = (float(cy) + float(hashU(u32(c) * 13u + 5u) & 255u) / 255.0f) * 16.0f;
				const int n = int(((cx + 1) & 3) + cy * 4);
				const float qx = (float((cx + 1) & 3) + float(hashU(u32(n) * 7u + 1u) & 255u) / 255.0f) * 16.0f + (cx == 3 ? 64.0f : 0.0f);
				const float qy = (float(cy) + float(hashU(u32(n) * 13u + 5u) & 255u) / 255.0f) * 16.0f;
				for (int k = 0; k < 2; ++k) {
					const float ax = px, ay = py;
					const float bx = k ? px + 4.0f : qx, by = k ? py + 16.0f : qy;
					const float dx = bx - ax, dy = by - ay;
					for (int ox = -1; ox <= 1; ++ox)
						for (int oy = -1; oy <= 1; ++oy) {
							const float fx = float(x) + float(ox * N) - ax, fy = float(y) + float(oy * N) - ay;
							const float t = clamp((fx * dx + fy * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
							const float ex = fx - dx * t, ey = fy - dy * t;
							best = min(best, sqrtf(ex * ex + ey * ey) / 2.5f);
						}
				}
			}
			cov[y * N + x] = u8(clamp(1.0f - best, 0.0f, 1.0f) * 255.0f);
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
		_fade[i] = v.fade;
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
	// Where the lightning starts, sector by sector (quarters round the dome's middle): low corners that are seen.
	for (u32 s = 0; s < SECTORS; ++s) _startCount[s] = 0;
	u32 candidates[SECTORS] = {};
	for (int pass = 0; pass < 2; ++pass) {
		u32 seen[SECTORS] = {};
		for (u32 i = 0; i < n; ++i) {
			const float* p = _pos + i * 3;
			const float rel = (p[1] - _bbMin[1]) / _radii[1];
			if (rel < 0.04f || rel > 0.25f || _fade[i] < 0.25f) continue;
			const float az = atan2f(p[0] - _centre[0], p[2] - _centre[2]);
			const u32 s = u32(clamp(int((az + 3.14159265f) / (kTwoPi / float(SECTORS))), 0, int(SECTORS) - 1));
			if (pass == 0) {
				++candidates[s];
				continue;
			}
			// Every k-th, spread round the sector.
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
	if (_glow) gfx::textureDestroy(_glow);
	_texture = _streak = _glow = nullptr;
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
	u32 used = 0;
	auto keep = [&](const char* name) -> const char* {
		const u32 len = u32(strlen(name));
		if (used + len + 1 > sizeof(_names)) return nullptr;
		char* at = _names + used;
		memcpy(at, name, len + 1);
		used += len + 1;
		return at;
	};
	// The baker's list (meta/barriersfx): the scripts' own names, sorted into the hum and the rest.
	if (const PakEntry* e = pak.find("meta/barriersfx")) {
		static char text[1024];
		if (e->size > 0 && e->size < sizeof(text) && pak.read(*e, text)) {
			text[e->size] = 0;
			for (char* p = text; *p;) {
				char* end = strchr(p, '\n');
				if (end) *end = 0;
				if (p[0] && p[1] == ' ' && p[2] && sound.knows(p + 2)) {
					if (p[0] == 'A' && !_humName) _humName = keep(p + 2);
					else if (p[0] == 'T' && !_thunderName) _thunderName = keep(p + 2);
				}
				if (!end) break;
				p = end + 1;
			}
		}
	}
	static const char* const kHum[] = {"MFX_BARRIERE_AMBIENT", "MFX_BARRIER_AMBIENT", "BARRIERE_AMBIENT", "BARRIER_AMBIENT", "MFX_BARRIERE_LOOP", "BARRIERE_LOOP"};
	static const char* const kThunder[] = {"MFX_BARRIERE_SHOOT", "MFX_BARRIER_SHOOT", "MFX_BARRIERE_THUNDER", "MFX_BARRIERE_LIGHTNING", "MFX_BARRIERE_BLITZ",
	                                       "BARRIERE_SHOOT", "BARRIERE_THUNDER", "MFX_BARRIERE_SHOOT_A1",
	                                       // Last: the weather's thunder (made by the engine when the game has none: App::weatherSound).
	                                       "G3_THUNDER_MADE_A"};
	if (!_humName)
		for (const char* n : kHum)
			if (sound.knows(n)) {
				_humName = n;
				break;
			}
	if (!_thunderName)
		for (const char* n : kThunder)
			if (sound.knows(n)) {
				_thunderName = n;
				break;
			}
}

void Barrier::update(float dt, u8 mode, bool outdoor, Vec3 camera, Sound& sound, Pak& pak) {
	if (!_loaded) load(pak);
	if (!_soundsSearched && sound.ready()) findSounds(sound, pak);
	++_frame;
	dt = clamp(dt, 0.0f, 0.25f);
	_time += double(dt);
	if (_time > 100000.0) _time -= 100000.0 - fmod(100000.0, double(kTwoPi) / double(BARRIER_SCROLL)); // (never far from 0)
	// Shown or not.
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
				_wait = 900.0f + 600.0f * rnd(); // 15..25 minutes
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
		// It comes: the sectors' first strikes follow soon, one after the other.
		for (u32 s = 0; s < SECTORS; ++s) _sectorClock[s] = SECTOR_DELAY[s] * (0.55f + 0.4f * rnd());
	}
	_level = clamp(_level + (want ? dt / FADE_IN : -dt / FADE_OUT), 0.0f, 1.0f);
	_flash = max(0.0f, _flash - dt * 3.0f);
	// The hum, while it is seen.
	if (_humName && _level > 0.0f) {
		if (!_hum || !sound.playing(_hum)) _hum = sound.playLoop(_humName, 0.4f * _level);
		else sound.setHandleGain(_hum, 0.4f * _level);
	} else if (_hum) {
		sound.stop(_hum);
		_hum = 0;
	}
	// Lightning: each sector strikes when its delay has passed and its last thunder is over.
	if (want && _level > 0.5f) {
		for (u32 s = 0; s < SECTORS; ++s) {
			_sectorClock[s] += dt;
			if (_sectorClock[s] < SECTOR_DELAY[s]) continue;
			if (_sectorSound[s] && sound.playing(_sectorSound[s])) continue;
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

void Barrier::toSphere(const float p[3], float q[3]) const {
	for (int a = 0; a < 3; ++a) q[a] = (p[a] - _centre[a]) / _radii[a];
	normalize3(q);
}

void Barrier::onSphere(const float q[3], float out[3]) const {
	// (a little inside the dome)
	for (int a = 0; a < 3; ++a) out[a] = _centre[a] + q[a] * _radii[a] * 0.99f;
}

void Barrier::spawnBolt(u32 sector, Sound& sound, Vec3 camera) {
	if (!_startCount[sector]) return;
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
	const u32 start = _starts[sector][u32(rnd() * float(_startCount[sector])) % _startCount[sector]];
	float qs[3], qt[3];
	toSphere(_pos + start * 3, qs);
	toSphere(_pos + _top * 3, qt);
	if (dot3(qs, qt) > 0.995f) return;
	// The main stroke: along the great circle from the start towards the top, so far, jagged sideways.
	const float omega = acosf(clamp(dot3(qs, qt), -1.0f, 1.0f));
	const float reach = 0.5f + 0.4f * rnd();
	float offset = 0.0f;
	for (u32 i = 0; i < BOLT_POINTS; ++i) {
		const float f = float(i) / float(BOLT_POINTS - 1) * reach;
		const float a = sinf((1.0f - f) * omega), c = sinf(f * omega), sn = sinf(omega);
		float q[3] = {(qs[0] * a + qt[0] * c) / sn, (qs[1] * a + qt[1] * c) / sn, (qs[2] * a + qt[2] * c) / sn};
		// Sideways: across the way, on the sphere.
		float along[3] = {qt[0] - q[0], qt[1] - q[1], qt[2] - q[2]}, side[3];
		cross3(q, along, side);
		normalize3(side);
		if (i > 0) offset = offset * 0.55f + (rnd() - 0.5f) * 0.075f * omega;
		for (int k = 0; k < 3; ++k) q[k] += side[k] * offset;
		normalize3(q);
		onSphere(q, b->main[i].p);
	}
	// Branches: shorter strokes leaving it at an angle.
	b->branches = u8(1 + u32(rnd() * float(BRANCHES)) % BRANCHES);
	for (u32 k = 0; k < b->branches; ++k) {
		const u32 from = 3u + u32(rnd() * float(BOLT_POINTS / 2));
		b->branchFrom[k] = u8(min(from, BOLT_POINTS - 2u));
		float q[3], next[3];
		toSphere(b->main[b->branchFrom[k]].p, q);
		toSphere(b->main[b->branchFrom[k] + 1].p, next);
		float dir[3] = {next[0] - q[0], next[1] - q[1], next[2] - q[2]}, side[3];
		normalize3(dir);
		cross3(q, dir, side);
		const float turn = (0.45f + 0.5f * rnd()) * (rnd() < 0.5f ? -1.0f : 1.0f);
		for (int a = 0; a < 3; ++a) dir[a] = dir[a] * cosf(turn) + side[a] * sinf(turn);
		const float step = omega * (0.025f + 0.025f * rnd());
		for (u32 j = 0; j < BRANCH_POINTS; ++j) {
			onSphere(q, b->branch[k][j].p);
			cross3(q, dir, side);
			normalize3(side);
			const float jag = (rnd() - 0.5f) * 0.9f * step;
			for (int a = 0; a < 3; ++a) q[a] += dir[a] * step + side[a] * jag;
			normalize3(q);
			// (keep the way tangent to the sphere)
			const float d = dot3(dir, q);
			for (int a = 0; a < 3; ++a) dir[a] -= q[a] * d;
			normalize3(dir);
		}
	}
	b->alive = true;
	b->age = 0.0f;
	b->life = 0.75f + 0.45f * rnd();
	b->seed = rnd() * 1000.0f;
	_flash = 1.0f;
	++_thunders;
	// Its thunder, from its side: a place in its direction, near enough to be heard well.
	if (_thunderName && sound.ready()) {
		float d[3] = {b->main[0].p[0] - camera.x, b->main[0].p[1] - camera.y, b->main[0].p[2] - camera.z};
		const float dist = normalize3(d);
		const Vec3 at {camera.x + d[0] * 1500.0f, camera.y + d[1] * 1500.0f, camera.z + d[2] * 1500.0f};
		const float gain = clamp(1.0f - (dist - 30000.0f) / 200000.0f, 0.55f, 1.0f);
		_sectorSound[sector] = sound.play(_thunderName, &at, gain);
	}
}

void Barrier::draw(Vec3 camera, float radius) {
	if (!hasMesh() || radius <= 0.0f) return;
	// The second eye of a stereo frame draws what the first one built (the same camera: Sky::draw's too).
	const bool fresh = _drawnFrame != _frame;
	if (fresh) {
		_drawnFrame = _frame;
		_turn = (_turn + 1) % MESHES;
		_domeBuilt = _boltQuads = _streakQuads = 0;
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
		for (int l = 0; l < 2; ++l) gfx::draw(_domeMeshes[l][_turn], 0, _indexCount);
		return;
	}
	_domeBuilt = 1;
	const float R = radius * 0.95f;
	const float t = wrap(_time);
	const double sc = _time * double(BARRIER_SCROLL);
	const float scroll = float(sc - floor(sc));
	const float gain = 255.0f * _level * (1.0f + 0.3f * _flash);
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
	for (int l = 0; l < 2; ++l) {
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
		u8 disc[32 * 32];
		for (int y = 0; y < 32; ++y)
			for (int x = 0; x < 32; ++x) {
				const float dx = (float(x) - 15.5f) / 15.5f, dy = (float(y) - 15.5f) / 15.5f;
				const float r = clamp(1.0f - sqrtf(dx * dx + dy * dy), 0.0f, 1.0f);
				disc[y * 32 + x] = u8(r * r * 255.0f);
			}
		_glow = gfx::uiTextureCreate(32, 32, disc, true);
		if (!_streak || !_glow) return;
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
		float q[BOLT_POINTS][3], dist[BOLT_POINTS];
		n = min(n, BOLT_POINTS);
		for (u32 i = 0; i < n; ++i) dist[i] = project(pts[i].p, q[i]);
		float side[BOLT_POINTS][3];
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
	const float strength = clamp(_level * 1.6f, 0.0f, 1.0f);
	for (const Bolt& b : _bolt) {
		if (!b.alive) continue;
		const float grow = 0.12f;
		float shine;
		if (b.age < grow) {
			shine = 1.0f;
		} else {
			// It flickers, as lightning strikes again along its way, and dies away.
			const u32 step = u32(b.age * 22.0f) + u32(b.seed);
			const float flick = float(hashU(step) & 255u) / 255.0f;
			shine = (0.45f + 0.55f * flick) * (1.0f - smooth01((b.age - (b.life - 0.35f)) / 0.35f));
		}
		shine *= strength;
		if (shine < 0.02f) continue;
		const u32 reach = 2u + u32(min(b.age / grow, 1.0f) * float(BOLT_POINTS - 2));
		const float glowRgb[3] = {70.0f * shine, 100.0f * shine, 255.0f * shine};
		const float coreRgb[3] = {225.0f * shine, 235.0f * shine, 255.0f * shine};
		strip(b.main, reach, 900.0f, 0.010f, glowRgb, 0.5f);
		strip(b.main, reach, 220.0f, 0.0035f, coreRgb, 0.5f);
		for (u32 k = 0; k < b.branches; ++k) {
			if (b.branchFrom[k] + 1u >= reach) continue;
			const float g2[3] = {glowRgb[0] * 0.6f, glowRgb[1] * 0.6f, glowRgb[2] * 0.6f};
			const float c2[3] = {coreRgb[0] * 0.7f, coreRgb[1] * 0.7f, coreRgb[2] * 0.7f};
			const u32 n = min(BRANCH_POINTS, reach - b.branchFrom[k]);
			strip(b.branch[k], n, 600.0f, 0.007f, g2, 0.8f);
			strip(b.branch[k], n, 150.0f, 0.0028f, c2, 0.8f);
		}
	}
	const u32 streakQuads = quads;
	// Where they start: a flash on the dome.
	for (const Bolt& b : _bolt) {
		if (!b.alive || quads >= BOLT_QUADS) continue;
		const float fade = (1.0f - smooth01(b.age / b.life)) * strength;
		if (fade < 0.02f) continue;
		float q[3];
		const float dist = project(b.main[0].p, q);
		float dir[3] = {q[0], q[1], q[2]};
		normalize3(dir);
		float right[3], up[3];
		const float worldUp[3] = {0, 1, 0};
		cross3(worldUp, dir, right);
		if (normalize3(right) < 1e-4f) {
			right[0] = 1;
			right[1] = right[2] = 0;
		}
		cross3(dir, right, up);
		const float h = max(4500.0f * R / dist, R * 0.04f);
		WorldVertex* v = verts + quads * 4;
		static const float cx[4] = {-1, 1, -1, 1}, cy[4] = {1, 1, -1, -1};
		for (int k = 0; k < 4; ++k) {
			float p[3];
			for (int a = 0; a < 3; ++a) p[a] = q[a] + (right[a] * cx[k] + up[a] * cy[k]) * h;
			setPos(v[k], p, radius);
			v[k].u = i16(cx[k] < 0 ? 0 : 1024);
			v[k].v = i16(cy[k] > 0 ? 0 : 1024);
			v[k].r = byteOf(130.0f * fade);
			v[k].g = byteOf(160.0f * fade);
			v[k].b = byteOf(255.0f * fade);
			v[k].a = 0;
		}
		++quads;
	}
	if (!quads) return;
	gfx::meshUpdate(_boltMeshes[_turn], verts, quads * 4);
	_boltQuads = quads;
	_streakQuads = streakQuads;
	const float origin[3] = {camera.x - radius, camera.y - radius, camera.z - radius};
	gfx::setChunkTransform(origin, 2.0f * radius / 32766.0f, 1.0f / 1024.0f);
	drawBoltMesh();
}

void Barrier::drawBoltMesh() {
	gfx::Mesh* mesh = _boltMeshes[_turn];
	if (_streakQuads) {
		gfx::setMaterial(_streak, MATF_ADD, 1.0f);
		gfx::draw(mesh, 0, _streakQuads * 6);
	}
	if (_boltQuads > _streakQuads) {
		gfx::setMaterial(_glow, MATF_ADD, 1.0f);
		gfx::draw(mesh, _streakQuads * 6, (_boltQuads - _streakQuads) * 6);
	}
}

} // namespace g3
