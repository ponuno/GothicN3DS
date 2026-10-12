#include "particles.h"

#include "game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace g3 {

namespace {

const float kDeg = 3.14159265f / 180.0f;

// The member symbols of C_PARTICLEFX, looked up once.
struct Members {
	u32 pps, ppsKeys, ppsLoop, ppsSmooth, ppsFps;
	u32 shpType, shpFor, shpOffset, shpVolume, shpDim;
	u32 dirMode, dirFor, dirTargetPos, head, headVar, elev, elevVar, velAvg, velVar;
	u32 lifeAvg, lifeVar, gravity, visName, visOrient, aniFps, aniLoop, colStart, colEnd, sizeStart, sizeEnd, alphaFunc, alphaStart,
	    alphaEnd;
	bool ready;
};

float* parseFloats(const char* s, float* out, int max, int& count) {
	count = 0;
	if (!s) return out;
	while (*s && count < max) {
		char* end;
		const float v = strtof(s, &end);
		if (end == s) break;
		out[count++] = v;
		s = end;
	}
	return out;
}

bool equalsNoCase(const char* a, const char* b) {
	for (; *a && *b; ++a, ++b)
		if ((*a | 32) != (*b | 32)) return false;
	return *a == *b;
}

} // namespace

u32 Particles::rnd() {
	_seed ^= _seed << 13;
	_seed ^= _seed >> 17;
	_seed ^= _seed << 5;
	return _seed;
}

void Particles::freeMeshes() {
	for (gfx::Mesh*& m : _meshes) {
		if (m) gfx::meshDestroy(m);
		m = nullptr;
	}
}

void Particles::leaveWorld() {
	free(_emitters);
	free(_emitterDef);
	free(_sources);
	_emitters = nullptr;
	_emitterDef = nullptr;
	_sources = nullptr;
	_emitterCount = _runningCount = _dynamicCount = _count = 0;
	_stats.emitters = _stats.running = _stats.particles = 0;
}

void Particles::release() {
	leaveWorld();
	freeMeshes();
	_textures.release();
	_defCount = 0;
	_stats = ParticleStats();
}

void Particles::enterWorld(Pak& pak, const char* worldName, Game& game) {
	leaveWorld();
	_pak = &pak;
	_game = &game;
	char name[96];
	snprintf(name, sizeof(name), "%s/emitters", worldName ? worldName : "");
	const PakEntry* e = pak.find(name);
	if (!e || e->size < sizeof(EmittersHeader)) return;
	u8* blob = static_cast<u8*>(malloc(e->size));
	if (!blob) return;
	EmittersHeader h {};
	if (pak.read(*e, blob)) memcpy(&h, blob, sizeof(h));
	if (h.magic != EMITTERS_MAGIC || h.version != EMITTERS_VERSION || h.count == 0 || e->size != emittersBlobSize(h.count)) {
		free(blob);
		return;
	}
	_emitters = static_cast<WorldEmitter*>(malloc(size_t(h.count) * sizeof(WorldEmitter)));
	_emitterDef = static_cast<u16*>(malloc(size_t(h.count) * sizeof(u16)));
	_sources = static_cast<Source*>(calloc(h.count, sizeof(Source)));
	if (!_emitters || !_emitterDef || !_sources) {
		free(blob);
		leaveWorld();
		return;
	}
	memcpy(_emitters, blob + sizeof(EmittersHeader), size_t(h.count) * sizeof(WorldEmitter));
	free(blob);
	_emitterCount = h.count;
	// (Definitions are read when an emitter first comes near.)
	for (u32 i = 0; i < _emitterCount; ++i) {
		_emitterDef[i] = 0xFFFE;
		_sources[i].time = frand() * 1000.0f; // not all fires in step
	}
	_stats.emitters = _emitterCount;
}

int Particles::defFor(u32 effect) {
	for (u32 i = 0; i < _defCount; ++i)
		if (_defs[i].hash == effect) return _defs[i].ok ? int(i) : -1;
	if (_defCount >= MAX_DEFS || !_game || !_pak) return -1;
	Def& d = _defs[_defCount];
	memset(&d, 0, sizeof(d));
	d.hash = effect;
	loadDef(d, effect);
	++_defCount;
	_stats.effects = _defCount;
	return d.ok ? int(_defCount - 1) : -1;
}

u32 Particles::arrowTrail() {
	static const u32 h = hashName("G3_ARROW_TRAIL");
	return h;
}

void Particles::loadDef(Def& d, u32 effect) {
	if (effect == arrowTrail()) {
		d.pps = 220;
		d.ppsKeys[0] = 1;
		d.ppsKeyCount = 1;
		d.ppsLooping = true;
		d.shape = SHAPE_POINT;
		d.objectFrame = true;
		d.lifeAvg = 380;
		d.lifeVar = 60;
		d.additive = true;
		d.colorStart[0] = 1.0f;
		d.colorStart[1] = 0.93f;
		d.colorStart[2] = 0.75f;
		d.colorEnd[0] = d.colorEnd[1] = d.colorEnd[2] = 0.5f;
		d.sizeStart[0] = d.sizeStart[1] = 5;
		d.sizeEndScale = 1.5f;
		d.alphaStart = 1.0f;
		d.alphaEnd = 0.0f;
		// (A soft dot: any picture of the effects will do.)
		static const char* const pics[] = {"tex/fireflare", "tex/synth_smoke", "tex/synth_spark", "tex/firesmoke", "tex/smoke"};
		for (const char* pic : pics) {
			const u32 hash = hashName(pic);
			if (gfx::Texture* t = _textures.acquire(*_pak, hash)) {
				d.frames[0] = t;
				d.frameHash[0] = hash;
				d.frameCount = 1;
				break;
			}
		}
		d.ok = d.frameCount > 0;
		return;
	}
	ScriptVm& vm = _game->particleVm();
	const u32 sym = vm.findHash(effect);
	static Members m;
	static const ScriptVm* ownerVm = nullptr;
	if (ownerVm != &vm || !m.ready) {
		const auto M = [&](const char* n) { return vm.findMember("C_PARTICLEFX", n); };
		m = Members {M("PPSVALUE"), M("PPSSCALEKEYS_S"), M("PPSISLOOPING"), M("PPSISSMOOTH"), M("PPSFPS"), M("SHPTYPE_S"), M("SHPFOR_S"),
		             M("SHPOFFSETVEC_S"), M("SHPISVOLUME"), M("SHPDIM_S"), M("DIRMODE_S"), M("DIRFOR_S"), M("DIRMODETARGETPOS_S"),
		             M("DIRANGLEHEAD"), M("DIRANGLEHEADVAR"), M("DIRANGLEELEV"), M("DIRANGLEELEVVAR"), M("VELAVG"), M("VELVAR"),
		             M("LSPPARTAVG"), M("LSPPARTVAR"), M("FLYGRAVITY_S"), M("VISNAME_S"), M("VISORIENTATION_S"), M("VISTEXANIFPS"),
		             M("VISTEXANIISLOOPING"), M("VISTEXCOLORSTART_S"), M("VISTEXCOLOREND_S"), M("VISSIZESTART_S"), M("VISSIZEENDSCALE"),
		             M("VISALPHAFUNC_S"), M("VISALPHASTART"), M("VISALPHAEND"), true};
		ownerVm = &vm;
	}
	if (sym == SCRIPT_NONE || m.visName == SCRIPT_NONE) return;
	ScriptInstance* in = vm.create(sym);
	if (!in) return;
	const auto F = [&](u32 s, float fallback) { return s == SCRIPT_NONE ? fallback : vm.getFloat(s, 0, in); };
	const auto I = [&](u32 s, int fallback) { return s == SCRIPT_NONE ? fallback : vm.getInt(s, 0, in); };
	const auto S = [&](u32 s) -> const char* { return s == SCRIPT_NONE ? "" : vm.getString(s, 0, in); };
	int n;
	d.pps = F(m.pps, 0);
	parseFloats(S(m.ppsKeys), d.ppsKeys, 16, n);
	d.ppsKeyCount = u8(n);
	if (n == 0) {
		d.ppsKeys[0] = 1;
		d.ppsKeyCount = 1;
	}
	d.ppsLooping = I(m.ppsLoop, 1) != 0;
	d.ppsSmooth = I(m.ppsSmooth, 0) != 0;
	d.ppsFps = F(m.ppsFps, 0);
	const char* type = S(m.shpType);
	d.shape = equalsNoCase(type, "LINE") ? SHAPE_LINE : equalsNoCase(type, "BOX") ? SHAPE_BOX : equalsNoCase(type, "CIRCLE") ? SHAPE_CIRCLE :
	          equalsNoCase(type, "SPHERE") ? SHAPE_SPHERE : SHAPE_POINT;
	d.volume = I(m.shpVolume, 0) != 0;
	d.objectFrame = !equalsNoCase(S(m.shpFor), "WORLD");
	float v[3];
	parseFloats(S(m.shpDim), v, 3, n);
	d.dim[0] = n > 0 ? v[0] : 0;
	d.dim[1] = n > 1 ? v[1] : d.dim[0];
	d.dim[2] = n > 2 ? v[2] : d.dim[0];
	parseFloats(S(m.shpOffset), v, 3, n);
	for (int i = 0; i < 3; ++i) d.offset[i] = i < n ? v[i] : 0;
	d.headAngle = F(m.head, 0) * kDeg;
	d.headVar = F(m.headVar, 0) * kDeg;
	d.elevAngle = F(m.elev, 0) * kDeg;
	d.elevVar = F(m.elevVar, 0) * kDeg;
	d.velAvg = F(m.velAvg, 0);
	d.velVar = F(m.velVar, 0);
	d.lifeAvg = F(m.lifeAvg, 1000.0f);
	d.lifeVar = F(m.lifeVar, 0);
	parseFloats(S(m.gravity), v, 3, n);
	for (int i = 0; i < 3; ++i) d.gravity[i] = i < n ? v[i] : 0;
	d.velocityAligned = equalsNoCase(S(m.visOrient), "VELO");
	d.additive = equalsNoCase(S(m.alphaFunc), "ADD");
	d.aniFps = F(m.aniFps, 0);
	d.aniLooping = I(m.aniLoop, 1) != 0;
	parseFloats(S(m.colStart), v, 3, n);
	for (int i = 0; i < 3; ++i) d.colorStart[i] = (i < n ? v[i] : 255.0f) / 255.0f;
	parseFloats(S(m.colEnd), v, 3, n);
	for (int i = 0; i < 3; ++i) d.colorEnd[i] = (i < n ? v[i] : 255.0f) / 255.0f;
	parseFloats(S(m.sizeStart), v, 2, n);
	d.sizeStart[0] = n > 0 ? v[0] : 10;
	d.sizeStart[1] = n > 1 ? v[1] : d.sizeStart[0];
	d.sizeEndScale = F(m.sizeEnd, 1.0f);
	d.alphaStart = clamp(F(m.alphaStart, 255.0f) / 255.0f, 0.0f, 1.0f);
	d.alphaEnd = clamp(F(m.alphaEnd, 0.0f) / 255.0f, 0.0f, 1.0f);
	// The picture, and its animation frames NAME_A0, NAME_A1 ...
	char base[96];
	snprintf(base, sizeof(base), "%s", S(m.visName));
	vm.destroy(in);
	char* dot = strrchr(base, '.');
	if (dot) *dot = 0;
	for (char* c = base; *c; ++c) *c = char(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
	if (!base[0]) return;
	char* anim = strstr(base, "_a0");
	for (u32 k = 0; k < MAX_FRAMES; ++k) {
		char name[128];
		if (k == 0 || !anim) snprintf(name, sizeof(name), "tex/%s", base);
		else snprintf(name, sizeof(name), "tex/%.*s_a%u%s", int(anim - base), base, k, anim + 3);
		if (k > 0 && !anim) break;
		const u32 hash = hashName(name);
		gfx::Texture* t = _textures.acquire(*_pak, hash);
		if (!t) break;
		d.frames[d.frameCount] = t;
		d.frameHash[d.frameCount] = hash;
		++d.frameCount;
	}
	d.ok = d.frameCount > 0;
}

void Particles::spawn(const Def& d, u16 defIndex, const float at[3], float yaw) {
	if (_count >= MAX_PARTICLES) return;
	Particle& p = _particles[_count++];
	p.def = defIndex;
	p.seed = u8(rnd());
	p.age = 0;
	p.life = max(d.lifeAvg + d.lifeVar * srand1(), 30.0f);
	// Where: the shape, in the emitter's frame (turned by its yaw).
	float o[3] = {d.offset[0], d.offset[1], d.offset[2]};
	switch (d.shape) {
	case SHAPE_LINE: {
		const float t = srand1() * 0.5f;
		o[0] += d.dim[0] * t;
		o[1] += d.dim[1] * t;
		o[2] += d.dim[2] * t;
		break;
	}
	case SHAPE_BOX:
		o[0] += d.dim[0] * 0.5f * srand1();
		o[1] += d.dim[1] * 0.5f * srand1();
		o[2] += d.dim[2] * 0.5f * srand1();
		break;
	case SHAPE_CIRCLE: {
		const float a = frand() * 6.2831853f, r = d.volume ? sqrtf(frand()) : 1.0f;
		o[0] += cosf(a) * d.dim[0] * r;
		o[2] += sinf(a) * d.dim[0] * r;
		break;
	}
	case SHAPE_SPHERE: {
		Vec3 dir;
		do dir = {srand1(), srand1(), srand1()};
		while (dot(dir, dir) > 1.0f || dot(dir, dir) < 1e-4f);
		dir = normalize(dir);
		const float r = d.volume ? cbrtf(frand()) : 1.0f;
		o[0] += dir.x * d.dim[0] * r;
		o[1] += dir.y * d.dim[0] * r;
		o[2] += dir.z * d.dim[0] * r;
		break;
	}
	default: break;
	}
	const float cy = d.objectFrame ? cosf(yaw) : 1.0f, sy = d.objectFrame ? sinf(yaw) : 0.0f;
	// (yaw turns around +Y; 0 looks down +Z.)
	p.pos[0] = at[0] + o[0] * cy + o[2] * sy;
	p.pos[1] = at[1] + o[1];
	p.pos[2] = at[2] - o[0] * sy + o[2] * cy;
	// Which way: heading and elevation round the emitter's up; 90 degrees of elevation is straight up.
	const float head = d.headAngle + d.headVar * srand1(), elev = d.elevAngle + d.elevVar * srand1();
	const float speed = max(d.velAvg + d.velVar * srand1(), 0.0f);
	const float dx = sinf(head) * cosf(elev), dy = sinf(elev), dz = cosf(head) * cosf(elev);
	p.vel[0] = (dx * cy + dz * sy) * speed;
	p.vel[1] = dy * speed;
	p.vel[2] = (-dx * sy + dz * cy) * speed;
}

void Particles::emit(const Def& d, u16 defIndex, const float pos[3], float yaw, Source& s, float dtMs) {
	float rate = d.pps;
	if (d.ppsFps > 0.0f && d.ppsKeyCount > 0) {
		const float at = s.time * 0.001f * d.ppsFps;
		u32 i = u32(at);
		if (!d.ppsLooping && i >= d.ppsKeyCount) return;
		float key = d.ppsKeys[i % d.ppsKeyCount];
		if (d.ppsSmooth) {
			const float f = at - floorf(at);
			const float next = d.ppsKeys[(i + 1) % d.ppsKeyCount];
			key += (next - key) * f;
		}
		rate *= key;
	} else if (d.ppsKeyCount > 0) {
		rate *= d.ppsKeys[0];
	}
	s.time += dtMs;
	s.acc += rate * dtMs * 0.001f;
	// (After a long pause a burst of thousands would fill the pool in one frame.)
	if (s.acc > 8.0f) s.acc = 8.0f;
	while (s.acc >= 1.0f) {
		s.acc -= 1.0f;
		spawn(d, defIndex, pos, yaw);
	}
}

Particles::Dynamic* Particles::emplace(u32 effect, Vec3 pos, float yaw, float seconds) {
	const int di = defFor(effect);
	if (di < 0) return nullptr;
	// A new one in place of the oldest when the list is full.
	Dynamic* slot = nullptr;
	if (_dynamicCount < MAX_DYNAMIC) {
		slot = &_dynamic[_dynamicCount++];
	} else {
		slot = &_dynamic[0];
		for (u32 i = 1; i < MAX_DYNAMIC; ++i)
			if (_dynamic[i].ttl < slot->ttl) slot = &_dynamic[i];
	}
	slot->def = u16(di);
	slot->pos[0] = pos.x;
	slot->pos[1] = pos.y;
	slot->pos[2] = pos.z;
	slot->yaw = yaw;
	slot->ttl = max(seconds, 0.05f);
	slot->src = Source();
	slot->id = 0;
	return slot;
}

bool Particles::play(u32 effect, Vec3 pos, float yaw, float seconds) { return emplace(effect, pos, yaw, seconds) != nullptr; }

u32 Particles::start(u32 effect, Vec3 pos, float yaw, float seconds) {
	Dynamic* slot = emplace(effect, pos, yaw, seconds);
	if (!slot) return 0;
	slot->id = _nextId++;
	if (_nextId == 0) _nextId = 1;
	return slot->id;
}

void Particles::move(u32 handle, Vec3 pos) {
	if (!handle) return;
	for (u32 i = 0; i < _dynamicCount; ++i)
		if (_dynamic[i].id == handle) {
			_dynamic[i].pos[0] = pos.x;
			_dynamic[i].pos[1] = pos.y;
			_dynamic[i].pos[2] = pos.z;
		}
}

void Particles::stop(u32 handle) {
	if (!handle) return;
	for (u32 i = 0; i < _dynamicCount; ++i)
		if (_dynamic[i].id == handle) _dynamic[i].ttl = min(_dynamic[i].ttl, 0.02f);
}

void Particles::update(float dt, Vec3 eye) {
	if (dt <= 0.0f) return;
	dt = min(dt, 0.1f);
	const float dtMs = dt * 1000.0f;
	// The world emitters near the player: the nearest few run.
	_runningCount = 0;
	float runDist[MAX_RUNNING];
	for (u32 i = 0; i < _emitterCount; ++i) {
		const WorldEmitter& e = _emitters[i];
		const float dx = e.pos[0] - eye.x, dy = e.pos[1] - eye.y, dz = e.pos[2] - eye.z;
		const float d2 = dx * dx + dy * dy + dz * dz;
		if (d2 > RANGE * RANGE) continue;
		u32 at = _runningCount;
		if (at == MAX_RUNNING) {
			if (d2 >= runDist[MAX_RUNNING - 1]) continue;
			at = MAX_RUNNING - 1;
		} else {
			++_runningCount;
		}
		while (at > 0 && runDist[at - 1] > d2) {
			runDist[at] = runDist[at - 1];
			_running[at] = _running[at - 1];
			--at;
		}
		runDist[at] = d2;
		_running[at] = i;
	}
	for (u32 r = 0; r < _runningCount; ++r) {
		const u32 i = _running[r];
		if (_emitterDef[i] == 0xFFFE) {
			const int di = defFor(_emitters[i].effect);
			_emitterDef[i] = di < 0 ? 0xFFFF : u16(di);
		}
		if (_emitterDef[i] == 0xFFFF) continue;
		emit(_defs[_emitterDef[i]], _emitterDef[i], _emitters[i].pos, _emitters[i].yaw, _sources[i], dtMs);
	}
	for (u32 i = 0; i < _dynamicCount;) {
		Dynamic& dyn = _dynamic[i];
		emit(_defs[dyn.def], dyn.def, dyn.pos, dyn.yaw, dyn.src, dtMs);
		dyn.ttl -= dt;
		if (dyn.ttl <= 0.0f) _dynamic[i] = _dynamic[--_dynamicCount];
		else ++i;
	}
	// The particles.
	for (u32 i = 0; i < _count;) {
		Particle& p = _particles[i];
		const Def& d = _defs[p.def];
		p.age += dtMs;
		if (p.age >= p.life) {
			p = _particles[--_count];
			continue;
		}
		for (int a = 0; a < 3; ++a) {
			p.vel[a] += d.gravity[a] * dtMs;
			p.pos[a] += p.vel[a] * dtMs;
		}
		++i;
	}
	_stats.running = _runningCount;
	_stats.particles = _count;
}

void Particles::draw(const Mat4& view, Vec3 eye, const float dayLight[3]) {
	_stats.drawCalls = 0;
	if (_count == 0) return;
	if (!_meshes[0]) {
		static u16 indices[MAX_PARTICLES * 6];
		for (u32 q = 0; q < MAX_PARTICLES; ++q) {
			const u16 b = u16(q * 4);
			const u16 quad[6] = {b, u16(b + 1), u16(b + 2), u16(b + 2), u16(b + 1), u16(b + 3)};
			memcpy(indices + q * 6, quad, sizeof(quad));
		}
		static WorldVertex blank[MAX_PARTICLES * 4];
		for (gfx::Mesh*& m : _meshes) {
			m = gfx::meshCreate(blank, MAX_PARTICLES * 4, indices, MAX_PARTICLES * 6);
			if (!m) {
				freeMeshes();
				return;
			}
		}
	}
	const Vec3 camRight = {view.at(0, 0), view.at(0, 1), view.at(0, 2)};
	const Vec3 camUp = {view.at(1, 0), view.at(1, 1), view.at(1, 2)};

	struct Item {
		u32 key;
		u16 particle;
		u8 frame;
	};
	static Item items[MAX_PARTICLES];
	u32 n = 0;
	for (u32 i = 0; i < _count; ++i) {
		const Particle& p = _particles[i];
		const Def& d = _defs[p.def];
		const float t = p.age / p.life;
		const float alpha = d.alphaStart + (d.alphaEnd - d.alphaStart) * t;
		if (alpha <= 0.01f) continue;
		u32 frame = 0;
		if (d.frameCount > 1) {
			const u32 f = d.aniFps > 0.0f ? u32(p.age * 0.001f * d.aniFps) : u32(t * float(d.frameCount));
			frame = d.aniLooping ? f % d.frameCount : min<u32>(f, d.frameCount - 1u);
		}
		// Blended particles come in a few steps of opacity (it is the material's); glowing ones carry theirs in the colour.
		const u32 bucket = d.additive ? 0 : u32(clamp(int(alpha * 4.0f + 0.5f), 1, 4));
		items[n++] = Item {(d.additive ? 1u << 28 : 0u) | (bucket << 24) | (u32(p.def) << 8) | frame, u16(i), u8(frame)};
	}
	if (n == 0) return;
	// (Insertion sort: a couple of hundred at most, and the order hardly changes from frame to frame.)
	for (u32 i = 1; i < n; ++i) {
		const Item it = items[i];
		u32 j = i;
		for (; j > 0 && items[j - 1].key > it.key; --j) items[j] = items[j - 1];
		items[j] = it;
	}

	static WorldVertex verts[MAX_PARTICLES * 4];
	for (u32 q = 0; q < n; ++q) {
		const Particle& p = _particles[items[q].particle];
		const Def& d = _defs[p.def];
		const float t = p.age / p.life;
		const float alpha = d.alphaStart + (d.alphaEnd - d.alphaStart) * t;
		const float scale = 1.0f + (d.sizeEndScale - 1.0f) * t;
		const float hw = d.sizeStart[0] * scale * 0.5f, hh = d.sizeStart[1] * scale * 0.5f;
		Vec3 right = camRight, up = camUp;
		const Vec3 vel = {p.vel[0], p.vel[1], p.vel[2]};
		if (d.velocityAligned && dot(vel, vel) > 1e-10f) {
			// The quad's up follows the flight; its width is across it, facing the camera.
			const Vec3 toward = {p.pos[0] - eye.x, p.pos[1] - eye.y, p.pos[2] - eye.z};
			Vec3 r = cross(normalize(vel), toward);
			if (dot(r, r) > 1e-6f) {
				right = normalize(r);
				up = normalize(vel);
			}
		}
		float c[3];
		for (int a = 0; a < 3; ++a) {
			c[a] = d.colorStart[a] + (d.colorEnd[a] - d.colorStart[a]) * t;
			if (d.additive) c[a] *= alpha;       // glow: the alpha is brightness
			else c[a] *= min(dayLight[a], 1.0f); // smoke: dimmed at night
		}
		const u8 rgb[3] = {u8(clamp(c[0], 0.0f, 1.0f) * 255.0f), u8(clamp(c[1], 0.0f, 1.0f) * 255.0f), u8(clamp(c[2], 0.0f, 1.0f) * 255.0f)};
		const Vec3 centre = {p.pos[0] - eye.x, p.pos[1] - eye.y, p.pos[2] - eye.z};
		static const int corner[4][2] = {{-1, 1}, {1, 1}, {-1, -1}, {1, -1}}; // top left, top right, bottom left, bottom right
		for (int k = 0; k < 4; ++k) {
			const Vec3 v = centre + right * (float(corner[k][0]) * hw) + up * (float(corner[k][1]) * hh);
			WorldVertex& o = verts[q * 4 + u32(k)];
			o.x = i16(clamp(v.x, -32000.0f, 32000.0f));
			o.y = i16(clamp(v.y, -32000.0f, 32000.0f));
			o.z = i16(clamp(v.z, -32000.0f, 32000.0f));
			o.w = 1;
			o.u = i16(corner[k][0] < 0 ? 8 : 1016);
			o.v = i16(corner[k][1] > 0 ? 8 : 1016);
			o.r = rgb[0];
			o.g = rgb[1];
			o.b = rgb[2];
			o.a = 0;
		}
	}
	gfx::Mesh* mesh = _meshes[_meshTurn];
	_meshTurn = (_meshTurn + 1) % MESHES;
	gfx::meshUpdate(mesh, verts, n * 4);
	const float origin[3] = {eye.x, eye.y, eye.z};
	gfx::setChunkTransform(origin, 1.0f, 1.0f / 1024.0f);
	for (u32 first = 0; first < n;) {
		u32 last = first + 1;
		while (last < n && items[last].key == items[first].key) ++last;
		const Def& d = _defs[(items[first].key >> 8) & 0xFFFF];
		const u32 bucket = (items[first].key >> 24) & 0xF;
		gfx::setMaterial(d.frames[items[first].frame], d.additive ? MATF_ADD : MATF_BLEND, d.additive ? 1.0f : float(bucket) * 0.25f);
		gfx::draw(mesh, first * 6, (last - first) * 6);
		++_stats.drawCalls;
		first = last;
	}
}

} // namespace g3
