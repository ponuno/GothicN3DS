#include "triggers.h"

#include "collision.h"
#include "game.h"
#include "world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace g3 {

namespace {

constexpr u32 STATE_WORLD = 0xFFFFFFFFu; // Game::triggerState: the world has been entered before

bool finite3(const float p[3]) { return isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]); }

Quat slerp(Quat a, Quat b, float t) {
	float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
	if (d < 0.0f) {
		d = -d;
		b = {-b.x, -b.y, -b.z, -b.w};
	}
	if (d > 0.9995f) return quatNlerp(a, b, t);
	const float th = acosf(d), s = sinf(th);
	const float wa = sinf((1.0f - t) * th) / s, wb = sinf(t * th) / s;
	return quatNormalize({a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb});
}

// A rotation that turns +Z to `z` (and keeps up up): a mover turned along its way.
Quat lookRotation(Vec3 z) {
	const float len = sqrtf(dot(z, z));
	if (len < 1e-4f) return quatIdentity();
	z = z * (1.0f / len);
	const Vec3 up = fabsf(z.y) < 0.999f ? Vec3 {0, 1, 0} : Vec3 {1, 0, 0};
	Vec3 x = cross(up, z);
	x = x * (1.0f / sqrtf(dot(x, x)));
	const Vec3 y = cross(z, x);
	// From the columns x y z.
	const float m00 = x.x, m10 = x.y, m20 = x.z, m01 = y.x, m11 = y.y, m21 = y.z, m02 = z.x, m12 = z.y, m22 = z.z;
	Quat q;
	const float tr = m00 + m11 + m22;
	if (tr > 0.0f) {
		const float t = sqrtf(tr + 1.0f) * 2.0f;
		q = {(m21 - m12) / t, (m02 - m20) / t, (m10 - m01) / t, 0.25f * t};
	} else if (m00 > m11 && m00 > m22) {
		const float t = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
		q = {0.25f * t, (m01 + m10) / t, (m02 + m20) / t, (m21 - m12) / t};
	} else if (m11 > m22) {
		const float t = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
		q = {(m01 + m10) / t, 0.25f * t, (m12 + m21) / t, (m02 - m20) / t};
	} else {
		const float t = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
		q = {(m02 + m20) / t, (m12 + m21) / t, 0.25f * t, (m10 - m01) / t};
	}
	return quatNormalize(q);
}

} // namespace

void Triggers::release() {
	for (u32 i = 0; i < _modelCount; ++i) _models[i].model.release();
	_modelCount = 0;
	_pool.release();
	free(_blob);
	free(_nameHash);
	free(_state);
	free(_touchList);
	free(_moverList);
	free(_doorList);
	free(_solidList);
	_touchList = _moverList = _doorList = _solidList = nullptr;
	_touchCount = _moverCount = _doorCount = _solidCount = 0;
	_delayedCount = 0;
	_delayedOverflow = false;
	_clock = 0.0;
	_hasView = false;
	_blob = nullptr;
	_nameHash = nullptr;
	_state = nullptr;
	_list = nullptr;
	_keys = nullptr;
	_targets = nullptr;
	_names = nullptr;
	_h = {};
	_pendingCount = 0;
	_stats = TriggerStats();
	_game = nullptr;
}

void Triggers::enterWorld(Pak& pak, const char* worldAsset, Game& game) {
	release();
	_game = &game;
	_world = game.currentWorld();
	char name[112];
	snprintf(name, sizeof(name), "%s/triggers", worldAsset);
	const PakEntry* e = pak.find(name);
	if (!e || e->size < sizeof(TriggersHeader)) return;
	u8* blob = static_cast<u8*>(pak.readAlloc(*e));
	if (!blob) return;
	adopt(blob, e->size, game);
}

bool Triggers::adopt(u8* blob, u32 size, Game& game) {
	release();
	_game = &game;
	_world = game.currentWorld();
	struct {
		u32 size;
	} entry {size};
	const auto* e = &entry;
	TriggersHeader h;
	memcpy(&h, blob, sizeof(h));
	if (h.version == 1) h.collisionTris = 0; // (a field of nothing then)
	bool ok = h.magic == TRIGGERS_MAGIC && (h.version == 1 || h.version == TRIGGERS_VERSION) && h.count < (1u << 16) &&
	          h.keyCount < (1u << 20) && h.targetCount < (1u << 20) && h.collisionTris < (1u << 20) && h.nameBytes >= 1 &&
	          h.nameBytes < (1u << 22) && h.nameBytes % 4 == 0 &&
	          u64(sizeof(h)) + u64(h.count) * sizeof(WorldTrigger) + u64(h.keyCount) * sizeof(MoverKey) + u64(h.targetCount) * sizeof(TriggerTarget) +
	                  u64(h.collisionTris) * 36u + h.nameBytes ==
	              e->size;
	const WorldTrigger* list = reinterpret_cast<const WorldTrigger*>(blob + sizeof(h));
	const MoverKey* keys = reinterpret_cast<const MoverKey*>(list + (ok ? h.count : 0));
	const TriggerTarget* targets = reinterpret_cast<const TriggerTarget*>(keys + (ok ? h.keyCount : 0));
	const float* coll = reinterpret_cast<const float*>(targets + (ok ? h.targetCount : 0));
	const char* names = reinterpret_cast<const char*>(coll + (ok ? size_t(h.collisionTris) * 9 : 0));
	if (ok) ok = names[h.nameBytes - 1] == 0;
	for (u32 i = 0; ok && i < h.count; ++i) {
		const WorldTrigger& t = list[i];
		ok = t.kind >= TRG_TRIGGER && t.kind <= TRG_DOOR && t.name < h.nameBytes && t.target < h.nameBytes && t.visual < h.nameBytes &&
		     finite3(t.bbMin) && finite3(t.bbMax);
		if (ok && (t.kind == TRG_MOVER || t.kind == TRG_DOOR)) ok = u64(t.first) + t.count <= h.keyCount;
		if (ok && t.kind == TRG_DOOR) ok = t.count == 2;
		if (ok && h.version >= 2 && (t.flags & TRGF_COLLIDES)) ok = u64(t.collFirst) + t.collCount <= h.collisionTris;
		if (ok && t.kind == TRG_LIST) ok = u64(t.first) + t.count <= h.targetCount;
		for (int k = 0; ok && k < 5; ++k) ok = t.sfx[k] < h.nameBytes;
	}
	for (u32 i = 0; ok && i < h.keyCount; ++i) ok = finite3(keys[i].pos) && isfinite(keys[i].rot[3]);
	for (u32 i = 0; ok && i < h.collisionTris * 3; ++i) ok = finite3(coll + size_t(i) * 3);
	for (u32 i = 0; ok && i < h.targetCount; ++i) ok = targets[i].name < h.nameBytes;
	if (ok) {
		_nameHash = static_cast<u32*>(malloc(size_t(h.count + 1) * sizeof(u32)));
		_state = static_cast<State*>(calloc(size_t(h.count) + 1, sizeof(State)));
		_touchList = static_cast<u32*>(malloc(size_t(h.count + 1) * sizeof(u32)));
		_moverList = static_cast<u32*>(malloc(size_t(h.count + 1) * sizeof(u32)));
		_doorList = static_cast<u32*>(malloc(size_t(h.count + 1) * sizeof(u32)));
		_solidList = static_cast<u32*>(malloc(size_t(h.count + 1) * sizeof(u32)));
		ok = _nameHash && _state && _touchList && _moverList && _doorList && _solidList;
	}
	if (!ok) {
		free(blob);
		release();
		_game = &game;
		return false;
	}
	_blob = blob;
	_h = h;
	_list = list;
	_keys = keys;
	_targets = targets;
	_names = names;
	_coll = h.collisionTris ? coll : nullptr;
	for (u32 i = 0; i < h.count; ++i) {
		const WorldTrigger& t = _list[i];
		_nameHash[i] = t.name ? hashName(_names + t.name) : 0u;
		State& s = _state[i];
		s.enabled = (t.flags & TRGF_START_ENABLED) != 0;
		s.emitAt = -1.0;
		s.delay = -1.0f;
		if (t.kind == TRG_MOVER) _moverList[_moverCount++] = i;
		else if (t.kind == TRG_DOOR) _doorList[_doorCount++] = i;
		else if ((t.flags & TRGF_REACT_TOUCH) && (t.flags & TRGF_RESPOND_PC)) _touchList[_touchCount++] = i;
		s.model = (t.kind == TRG_MOVER || t.kind == TRG_DOOR) && t.visual ? NOT_TRIED : NO_MODEL;
		u32 kept = 0;
		if (game.triggerState(_world, i, kept)) {
			s.frame = kept & 0xFFFFu;
			s.emitCount = u16((kept >> 16) & 0xFFFu);
			s.enabled = ((kept >> 28) & 1u) == 0;
		}
		if (t.kind == TRG_MOVER) {
			++_stats.movers;
			if (s.frame >= t.count) s.frame = 0;
			s.target = s.frame;
			pose(i);
		} else if (t.kind == TRG_DOOR) {
			++_stats.doors;
			// (Kept: bit 0 open, the side in bit 1; unlocked in bit 29.)
			s.doorOpen = (s.frame & 1u) != 0;
			s.doorSide = (s.frame & 2u) ? -1.0f : 1.0f;
			s.doorAngle = s.doorOpen ? s.doorSide * 1.5707963f : 0.0f;
			s.unlocked = ((kept >> 29) & 1u) != 0;
			doorPose(i);
		} else {
			++_stats.triggers;
		}
		if ((t.flags & TRGF_COLLIDES) && t.collCount && _coll) {
			// A sphere about its triangles, for a quick no. (Also for a door kept open: it is solid once shut.)
			const float* c = _coll + size_t(t.collFirst) * 9;
			float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
			for (u32 k = 0; k < t.collCount * 3; ++k)
				for (int a = 0; a < 3; ++a) {
					mn[a] = fminf(mn[a], c[k * 3 + u32(a)]);
					mx[a] = fmaxf(mx[a], c[k * 3 + u32(a)]);
				}
			s.collCenter = {(mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f, (mn[2] + mx[2]) * 0.5f};
			const Vec3 half = {(mx[0] - mn[0]) * 0.5f, (mx[1] - mn[1]) * 0.5f, (mx[2] - mn[2]) * 0.5f};
			s.collRadius = sqrtf(dot(half, half));
			refreshCenter(i);
			_solidList[_solidCount++] = i;
			++_stats.solid;
		}
	}
	// The world's start: its triggers fire (those that fire only once, only the first time).
	u32 seen = 0;
	const bool first = !game.triggerState(_world, STATE_WORLD, seen);
	game.setTriggerState(_world, STATE_WORLD, 1);
	for (u32 i = 0; i < _h.count; ++i)
		if (_list[i].kind == TRG_WORLDSTART) fire(i, first ? EV_START_FIRST : EV_START);
	return true;
}

bool Triggers::moverTransform(u32 i, Quat& rot, Vec3& pos) const {
	if (i >= _h.count || (_list[i].kind != TRG_MOVER && _list[i].kind != TRG_DOOR) || _list[i].count == 0) return false;
	rot = _state[i].rot;
	pos = _state[i].pos;
	return true;
}

// ---- events ----

void Triggers::send(const char* name, bool untrigger) {
	if (!name || !name[0]) return;
	dispatch(hashName(name), untrigger ? EV_UNTRIGGER : EV_TRIGGER);
	snprintf(_stats.last, sizeof(_stats.last), "%s%s", untrigger ? "-" : "", name);
}

// To every trigger of that name (names need not be unique); a delay waits in the queue.
void Triggers::dispatch(u32 hash, u8 type, float delay) {
	if (!hash || !_blob) return;
	if (delay > 0.0f) {
		if (_pendingCount < PENDING) _pending[_pendingCount++] = Pending {hash, type, delay};
		return;
	}
	if (_depth > 8) return; // (triggers that trigger each other in a ring)
	++_depth;
	bool any = false;
	for (u32 i = 0; i < _h.count; ++i)
		if (_nameHash[i] == hash) {
			any = true;
			process(i, type);
		}
	--_depth;
	if (any) ++_stats.events;
	else ++_stats.unknown;
}

// OpenGothic's AbstractTrigger::processEvent: one delayed event at a time, not again within the retrigger delay.
void Triggers::process(u32 i, u8 type) {
	const WorldTrigger& t = _list[i];
	State& s = _state[i];
	if (s.delay >= 0.0f) return;
	if (s.emitAt >= 0.0 && _clock - s.emitAt < double(t.retriggerDelay)) return;
	if (t.fireDelay > 0.0f && t.kind != TRG_MOVER) {
		s.delay = t.fireDelay;
		s.delayedType = type;
		if (_delayedCount < DELAYED) _delayed[_delayedCount++] = i;
		else _delayedOverflow = true;
		return;
	}
	fire(i, type);
}

// ... implProcessEvent.
void Triggers::fire(u32 i, u8 type) {
	const WorldTrigger& t = _list[i];
	State& s = _state[i];
	s.emitAt = _clock;
	if (type == EV_UNTRIGGER) {
		if (!s.enabled || !(t.flags & TRGF_SEND_UNTRIGGER)) return;
		if (t.kind == TRG_MOVER) onMover(i, type);
		else if (t.kind == TRG_TRIGGER && t.target) dispatch(hashName(_names + t.target), EV_UNTRIGGER);
		return;
	}
	if (type == EV_TOUCH && !(t.flags & TRGF_REACT_TOUCH)) return;
	if (type == EV_TRIGGER && !(t.flags & TRGF_REACT_TRIGGER) && t.kind != TRG_WORLDSTART) return;
	if (!s.enabled && t.kind != TRG_WORLDSTART) return;
	if (t.maxActivations >= 0 && s.emitCount >= u32(t.maxActivations) && t.kind != TRG_WORLDSTART) return;
	if (s.emitCount < 0xFFF) ++s.emitCount;
	switch (t.kind) {
	case TRG_TRIGGER:
		if (t.target) dispatch(hashName(_names + t.target), EV_TRIGGER);
		break;
	case TRG_WORLDSTART:
		if ((t.flags & TRGF_FIRE_ONCE) && type != EV_START_FIRST) break;
		if (type != EV_START && type != EV_START_FIRST) break;
		if (t.target) dispatch(hashName(_names + t.target), EV_TRIGGER);
		break;
	case TRG_LIST: {
		if (!t.count) break;
		if (t.behavior == 0) {
			float at = 0.0f;
			for (u32 k = 0; k < t.count; ++k) {
				const TriggerTarget& x = _targets[t.first + k];
				at += x.delay;
				if (x.name) dispatch(hashName(_names + x.name), EV_TRIGGER, at);
			}
		} else {
			u32 k = s.next % t.count;
			if (t.behavior == 1) s.next = u8((s.next + 1) % t.count);
			else if (_game) k = _game->dice(t.count);
			const TriggerTarget& x = _targets[t.first + k];
			if (x.name) dispatch(hashName(_names + x.name), EV_TRIGGER, x.delay);
		}
		break;
	}
	case TRG_SCRIPT:
		if (_game && t.target) {
			const u32 fn = _game->function(_names + t.target);
			if (fn != SCRIPT_NONE) {
				const u32 hero = _game->heroIndex();
				if (hero != SCRIPT_NONE) _game->callAs(fn, hero, -1);
				++_stats.scripts;
			}
		}
		break;
	case TRG_MOVER: onMover(i, type); break;
	default: break;
	}
	keep(i);
}

// ---- movers (OpenGothic's MoveTrigger) ----

u32 Triggers::nextKey(u32 i, u32 k) const {
	const u32 n = _list[i].count;
	if (_list[i].behavior == 3) return (k + 1) % n;
	return k + 1 < n ? k + 1 : n - 1;
}

u32 Triggers::prevKey(u32 i, u32 k) const {
	const u32 n = _list[i].count;
	if (_list[i].behavior == 3) return (k + n - 1) % n;
	return k > 0 ? k - 1 : 0;
}

float Triggers::keyTicks(u32 i, u32 k) const {
	const WorldTrigger& t = _list[i];
	if (t.count < 2 || t.speed <= 0.0f) return 0.0f;
	const MoverKey& a = _keys[t.first + k % t.count];
	const MoverKey& b = _keys[t.first + (k + 1) % t.count];
	const float dx = b.pos[0] - a.pos[0], dy = b.pos[1] - a.pos[1], dz = b.pos[2] - a.pos[2];
	const float len = sqrtf(dx * dx + dy * dy + dz * dz);
	if (len > 0.01f) return fmaxf(1.0f, len / t.speed);
	// Turning only: the speed is taken as turns per second, and a very slow one as made faster (OpenGothic's measurements).
	const float d = a.rot[0] * b.rot[0] + a.rot[1] * b.rot[1] + a.rot[2] * b.rot[2] + a.rot[3] * b.rot[3];
	float theta = 2.0f * d * d - 1.0f;
	theta = theta < -1.0f ? -1.0f : theta > 1.0f ? 1.0f : theta;
	const float angle = acosf(theta) * 57.29578f;
	const float speed = t.speed < 0.006f ? t.speed * 300.0f : t.speed;
	return fmaxf(1.0f, angle / 360.0f * 1000.0f / speed);
}

void Triggers::onMover(u32 i, u8 type) {
	const WorldTrigger& t = _list[i];
	State& s = _state[i];
	if (t.count < 2 || keyTicks(i, 0) <= 0.0f) return;
	const u8 before = s.mover;
	if (type == EV_UNTRIGGER) {
		if (t.behavior != 1) return;
		if (s.frame > 0 && s.mover == MV_IDLE) {
			s.mover = MV_CLOSE;
			s.target = 0;
			startMover(i, before);
		}
		return;
	}
	switch (t.behavior) {
	case 0: // toggle
		s.mover = ((s.frame == 0 && s.mover == MV_IDLE) || s.mover == MV_CLOSE) ? MV_OPEN : MV_CLOSE;
		break;
	case 1: // trigger control
		if (s.frame == 0 && s.mover == MV_IDLE) s.mover = MV_OPEN;
		break;
	case 2: // open timed
		if (s.mover == MV_OPEN_TIMED) s.frameMs = 0.0f;
		else s.mover = MV_OPEN;
		break;
	case 3: s.mover = MV_LOOP; break;
	default: return; // single keys: moved by messages, which are not done
	}
	startMover(i, before);
}

void Triggers::startMover(u32 i, u8 before) {
	State& s = _state[i];
	const WorldTrigger& t = _list[i];
	if (before == s.mover || s.mover == MV_IDLE) return;
	if (s.mover == MV_OPEN) {
		s.target = t.count - 1u;
		sound(i, 0);
		if (before == MV_CLOSE) {
			s.frameMs = fmaxf(keyTicks(i, s.frame) - s.frameMs, 0.0f);
			s.frame = nextKey(i, s.frame);
		}
	} else if (s.mover == MV_CLOSE) {
		s.target = 0;
		sound(i, 2);
		if (before == MV_OPEN) {
			s.frame = prevKey(i, s.frame);
			s.frameMs = fmaxf(keyTicks(i, s.frame) - s.frameMs, 0.0f);
		}
	}
	sound(i, 4);
}

void Triggers::arrive(u32 i) {
	State& s = _state[i];
	const WorldTrigger& t = _list[i];
	if (t.target) dispatch(hashName(_names + t.target), EV_TRIGGER);
	if (s.mover == MV_OPEN) sound(i, 1);
	if (s.mover == MV_CLOSE) sound(i, 3);
	if (t.behavior == 2 && s.mover == MV_OPEN) {
		s.mover = MV_OPEN_TIMED;
		s.frameMs = 0.0f;
		return;
	}
	s.mover = MV_IDLE;
	s.frameMs = 0.0f;
	keep(i);
}

void Triggers::tickMover(u32 i, float dtMs) {
	State& s = _state[i];
	const WorldTrigger& t = _list[i];
	s.frameMs += dtMs;
	if (s.mover == MV_OPEN_TIMED) {
		if (s.frameMs <= t.stayOpen * 1000.0f) return;
		const u8 before = s.mover;
		s.mover = MV_CLOSE;
		s.target = 0;
		s.frameMs -= t.stayOpen * 1000.0f;
		(void)before;
		sound(i, 2);
	}
	if (s.mover != MV_LOOP && s.frame == s.target) {
		arrive(i);
		pose(i);
		return;
	}
	// The key it comes from, and how far it is (several keys in one long frame: all of them).
	for (int guard = 0; guard < 8; ++guard) {
		const u32 from = s.mover == MV_CLOSE ? prevKey(i, s.frame) : s.frame;
		const float ticks = keyTicks(i, from);
		if (ticks <= 0.0f || s.frameMs < ticks) break;
		s.frameMs -= ticks;
		s.frame = s.mover == MV_CLOSE ? prevKey(i, s.frame) : nextKey(i, s.frame);
		if (s.mover != MV_LOOP && s.frame == s.target) {
			s.frameMs = 0.0f;
			break;
		}
	}
	pose(i);
}

// Where the mesh is now: between the key it comes from and the next, as fast as its speed type says.
void Triggers::pose(u32 i) {
	poseAt(i);
	refreshCenter(i);
}

void Triggers::poseAt(u32 i) {
	State& s = _state[i];
	const WorldTrigger& t = _list[i];
	if (!t.count) return;
	const bool moving = s.mover == MV_OPEN || s.mover == MV_CLOSE || s.mover == MV_LOOP;
	if (!moving || t.count < 2) {
		const MoverKey& k = _keys[t.first + (s.frame < t.count ? s.frame : 0)];
		s.pos = {k.pos[0], k.pos[1], k.pos[2]};
		s.rot = quatNormalize({k.rot[0], k.rot[1], k.rot[2], k.rot[3]});
		return;
	}
	const u32 f1 = s.mover == MV_CLOSE ? prevKey(i, s.frame) : s.frame;
	const u32 f2 = nextKey(i, f1);
	const float ticks = keyTicks(i, f1);
	float a = ticks > 0.0f ? s.frameMs / ticks : 1.0f;
	a = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
	if (s.mover == MV_CLOSE) a = 1.0f - a;
	const bool start = f1 == 0 && s.mover != MV_LOOP;
	const bool end = f2 == u32(t.count - 1) && s.mover != MV_LOOP;
	switch (t.speedMode) {
	case 1:
	case 4:
		if ((start && end) || t.speedMode == 4) a = (3.0f - 2.0f * a) * a * a;
		else if (start) a = (2.0f - a) * a * a;
		else if (end) a = ((1.0f - a) * a + 1.0f) * a;
		break;
	case 2:
	case 5:
		if (start || t.speedMode == 5) a = (2.0f - a) * a * a;
		break;
	case 3:
	case 6:
		if (end || t.speedMode == 6) a = ((1.0f - a) * a + 1.0f) * a;
		break;
	default: break;
	}
	a = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
	const MoverKey& k1 = _keys[t.first + f1];
	const MoverKey& k2 = _keys[t.first + f2];
	const Quat q1 = {k1.rot[0], k1.rot[1], k1.rot[2], k1.rot[3]}, q2 = {k2.rot[0], k2.rot[1], k2.rot[2], k2.rot[3]};
	Vec3 forward;
	if (t.lerp == 1) {
		for (int c = 0; c < 3; ++c) (&s.pos.x)[c] = k1.pos[c] + (k2.pos[c] - k1.pos[c]) * a;
		forward = {k2.pos[0] - k1.pos[0], k2.pos[1] - k1.pos[1], k2.pos[2] - k1.pos[2]};
	} else {
		const MoverKey& k0 = _keys[t.first + prevKey(i, f1)];
		const MoverKey& k3 = _keys[t.first + nextKey(i, f2)];
		float p[3], f[3];
		for (int c = 0; c < 3; ++c) {
			const float p0 = k0.pos[c], p1 = k1.pos[c], p2 = k2.pos[c], p3 = k3.pos[c];
			const float b1 = -p0 + p1 - p2 + p3, b2 = 2.0f * p0 - 2.0f * p1 + p2 - p3, b3 = -p0 + p2;
			p[c] = ((b1 * a + b2) * a + b3) * a + p1;
			f[c] = (3.0f * b1 * a + 2.0f * b2) * a + b3;
		}
		s.pos = {p[0], p[1], p[2]};
		forward = {f[0], f[1], f[2]};
	}
	s.rot = (t.flags & TRGF_AUTO_ROTATE) ? lookRotation(forward) : slerp(q1, q2, a);
}

// What is kept in the save: the key a mover rests at, how often it fired, whether it is off.
void Triggers::keep(u32 i) {
	if (!_game) return;
	const State& s = _state[i];
	const bool resting = s.mover == MV_IDLE;
	u32 frame = _list[i].kind == TRG_MOVER ? (resting ? s.frame : s.target) : 0u;
	if (_list[i].kind == TRG_DOOR) frame = (s.doorOpen ? 1u : 0u) | (s.doorSide < 0.0f ? 2u : 0u);
	_game->setTriggerState(_world, i, (frame & 0xFFFFu) | (u32(s.emitCount & 0xFFF) << 16) | (s.enabled ? 0u : 1u << 28) |
	                                      (s.unlocked ? 1u << 29 : 0u));
}

void Triggers::sound(u32 i, int which) {
	if (!_hooks.sound || which < 0 || which > 4) return;
	const u32 n = _list[i].sfx[which];
	if (!n || !_names[n]) return;
	_hooks.sound(_hooks.user, _names + n, _state[i].pos);
}

// ---- every frame ----

void Triggers::update(float dt, Vec3 heroFeet, Game& game, const Vec3* others, u32 otherCount) {
	_game = &game;
	// The scripts' events (Wld_SendTrigger).
	char name[48];
	bool untrigger = false;
	while (game.takeTrigger(name, sizeof(name), untrigger)) send(name, untrigger);
	if (!_blob) return;
	// Delayed sends (lists), and delayed triggers.
	for (u32 k = 0; k < _pendingCount;) {
		_pending[k].delay -= dt;
		if (_pending[k].delay > 0.0f) {
			++k;
			continue;
		}
		const Pending p = _pending[k];
		_pending[k] = _pending[--_pendingCount];
		dispatch(p.hash, p.type);
	}
	_clock += double(dt);
	u32 moving = 0;
	const Vec3 body = {heroFeet.x, heroFeet.y + 90.0f, heroFeet.z};
	// Delayed events (fix87: those pending, not every trigger every frame).
	auto delayStep = [&](u32 i) {
		State& s = _state[i];
		if (s.delay < 0.0f) return true; // (done)
		s.delay -= dt;
		if (s.delay >= 0.0f) return false;
		s.delay = -1.0f;
		fire(i, s.delayedType);
		return _state[i].delay < 0.0f; // (firing may have set a new delay)
	};
	if (_delayedOverflow) {
		// More than the list holds: all of them, until the list can take what is left.
		_delayedCount = 0;
		_delayedOverflow = false;
		for (u32 i = 0; i < _h.count; ++i) {
			if (_state[i].delay < 0.0f || delayStep(i)) continue;
			if (_delayedCount < DELAYED) _delayed[_delayedCount++] = i;
			else _delayedOverflow = true;
		}
	} else {
		const u32 before = _delayedCount;
		u32 kept = 0;
		for (u32 k = 0; k < before; ++k) {
			const u32 i = _delayed[k];
			if (!delayStep(i)) _delayed[kept++] = i;
		}
		// (What firing added meanwhile went to the end: moved down behind the kept ones.)
		for (u32 k = before; k < _delayedCount; ++k) _delayed[kept++] = _delayed[k];
		_delayedCount = kept;
	}
	// The player's character walks into the box (OpenGothic's onIntersect: on the way in).
	for (u32 k = 0; k < _touchCount; ++k) {
		const u32 i = _touchList[k];
		const WorldTrigger& t = _list[i];
		State& s = _state[i];
		const bool in = body.x >= t.bbMin[0] && body.x <= t.bbMax[0] && body.y >= t.bbMin[1] - 90.0f && body.y <= t.bbMax[1] &&
		                body.z >= t.bbMin[2] && body.z <= t.bbMax[2];
		if (in && !s.inside) process(i, EV_TOUCH);
		s.inside = in;
	}
	for (u32 k = 0; k < _moverCount; ++k) {
		const u32 i = _moverList[k];
		State& s = _state[i];
		if (s.mover != MV_IDLE) {
			tickMover(i, dt * 1000.0f);
			moving += s.mover != MV_IDLE;
		}
	}
	for (u32 k = 0; k < _doorCount; ++k) {
		const u32 i = _doorList[k];
		const WorldTrigger& t = _list[i];
		State& s = _state[i];
		{
			// The inhabitants walk through doors: one opens before them (if it is not locked) and shuts a while after
			// the last of them has gone.
			if (!s.hingeSet) {
				s.hinge = toWorldClosed(i, {_keys[t.first + 1].pos[0], _keys[t.first + 1].pos[1], _keys[t.first + 1].pos[2]});
				s.hingeSet = true;
			}
			const Vec3 hinge = s.hinge;
			bool someone = false;
			Vec3 who = {0, 0, 0};
			for (u32 o = 0; o < otherCount; ++o) {
				const Vec3 d = others[o] - hinge;
				if (d.x * d.x + d.z * d.z < 230.0f * 230.0f && fabsf(d.y) < 200.0f) {
					someone = true;
					who = others[o];
				}
			}
			if (someone && !s.doorOpen && !doorLocked(i)) {
				doorToggle(i, who);
				s.byOthers = true;
			}
			if (s.byOthers && s.doorOpen) {
				const Vec3 h = heroFeet - hinge;
				const bool heroNear = h.x * h.x + h.z * h.z < 260.0f * 260.0f;
				s.doorIdle = someone || heroNear ? 0.0f : s.doorIdle + dt;
				if (s.doorIdle > 3.0f) {
					doorToggle(i, who);
					s.byOthers = false;
				}
			}
			// Turning towards where it is to be: a quarter turn in 0.8 s.
			const float want = s.doorOpen ? s.doorSide * 1.5707963f : 0.0f;
			if (s.doorAngle != want) {
				const float stepA = dt * 1.5707963f / 0.8f;
				s.doorAngle = fabsf(want - s.doorAngle) <= stepA ? want : s.doorAngle + (want > s.doorAngle ? stepA : -stepA);
				doorPose(i);
				++moving;
			}
		}
	}
	_stats.moving = moving;
}

// ---- doors ----

// A point of the door's own space where it is when shut.
Vec3 Triggers::toWorldClosed(u32 i, Vec3 p) const {
	const MoverKey& k = _keys[_list[i].first];
	const Quat q = {k.rot[0], k.rot[1], k.rot[2], k.rot[3]};
	const Vec3 r = quatRotate(q, p);
	return {k.pos[0] + r.x, k.pos[1] + r.y, k.pos[2] + r.z};
}

// Turned about its hinge (a vertical line through the pivot) by doorAngle from where it stands shut.
void Triggers::doorPose(u32 i) {
	State& s = _state[i];
	const MoverKey& k = _keys[_list[i].first];
	const Quat q0 = quatNormalize({k.rot[0], k.rot[1], k.rot[2], k.rot[3]});
	const Vec3 p0 = {k.pos[0], k.pos[1], k.pos[2]};
	const Vec3 hinge = toWorldClosed(i, {_keys[_list[i].first + 1].pos[0], _keys[_list[i].first + 1].pos[1], _keys[_list[i].first + 1].pos[2]});
	const Quat turn = quatFromYaw(s.doorAngle);
	s.rot = quatMul(turn, q0);
	s.pos = hinge + quatRotate(turn, p0 - hinge);
	refreshCenter(i);
}

int Triggers::doorNear(Vec3 feet, float yaw, float reach) const {
	int best = -1;
	float bestD = reach * reach;
	for (u32 k = 0; k < _doorCount; ++k) {
		const u32 i = _doorList[k];
		// Measured to the middle of the leaf as it stands now (first a quick no by its place: a leaf is a few metres).
		const State& s = _state[i];
		{
			const float qx = s.pos.x - feet.x, qz = s.pos.z - feet.z, far = reach + 400.0f;
			if (qx * qx + qz * qz > far * far) continue;
		}
		const Vec3 c = solid(i) ? s.worldCenter : s.pos;
		const float dx = c.x - feet.x, dz = c.z - feet.z;
		const float d2 = dx * dx + dz * dz;
		if (d2 >= bestD || fabsf(c.y - (feet.y + 100.0f)) > 220.0f) continue;
		if (d2 > 60.0f * 60.0f) {
			float a = atan2f(dx, dz) - yaw;
			while (a > 3.14159265f) a -= 6.2831853f;
			while (a < -3.14159265f) a += 6.2831853f;
			if (fabsf(a) > 1.1f) continue;
		}
		bestD = d2;
		best = int(i);
	}
	return best;
}

void Triggers::doorUnlock(u32 i) {
	if (i >= _h.count || _list[i].kind != TRG_DOOR) return;
	_state[i].unlocked = true;
	keep(i);
}

void Triggers::doorToggle(u32 i, Vec3 from) {
	if (i >= _h.count || _list[i].kind != TRG_DOOR) return;
	State& s = _state[i];
	if (!s.doorOpen) {
		if (doorLocked(i)) return;
		// Away from whoever opens it: the side of the leaf they stand on decides the way it swings.
		const MoverKey& k = _keys[_list[i].first];
		const Quat q0 = quatNormalize({k.rot[0], k.rot[1], k.rot[2], k.rot[3]});
		const Vec3 hingeL = {_keys[_list[i].first + 1].pos[0], _keys[_list[i].first + 1].pos[1], _keys[_list[i].first + 1].pos[2]};
		const Vec3 local = quatRotate({-q0.x, -q0.y, -q0.z, q0.w}, from - Vec3 {k.pos[0], k.pos[1], k.pos[2]});
		// The leaf runs from the hinge along local x (or z), towards the middle; the side is the other axis.
		const bool alongX = fabsf(hingeL.x) >= fabsf(hingeL.z);
		const float leaf = alongX ? (hingeL.x <= 0.0f ? 1.0f : -1.0f) : (hingeL.z <= 0.0f ? 1.0f : -1.0f);
		const float side = alongX ? local.z : local.x;
		// A turn by +a carries +x towards -z (and +z towards +x).
		s.doorSide = alongX ? ((side >= 0.0f ? 1.0f : -1.0f) * leaf) : ((side >= 0.0f ? -1.0f : 1.0f) * leaf);
		s.doorOpen = true;
	} else {
		s.doorOpen = false;
	}
	s.doorIdle = 0.0f;
	keep(i);
}

// ---- colliding ----

Vec3 Triggers::toLocal(u32 i, Vec3 p) const {
	const State& s = _state[i];
	return quatRotate({-s.rot.x, -s.rot.y, -s.rot.z, s.rot.w}, p - s.pos);
}

Vec3 Triggers::toWorld(u32 i, Vec3 p) const {
	const State& s = _state[i];
	return s.pos + quatRotate(s.rot, p);
}

namespace {

// Möller-Trumbore: where along the ray (o + d t, t >= 0) it meets the triangle; false if not.
bool rayTriangle(Vec3 o, Vec3 d, Vec3 a, Vec3 b, Vec3 c, float& t) {
	const Vec3 e1 = b - a, e2 = c - a;
	const Vec3 p = cross(d, e2);
	const float det = dot(e1, p);
	if (fabsf(det) < 1e-6f) return false;
	const float inv = 1.0f / det;
	const Vec3 s = o - a;
	const float u = dot(s, p) * inv;
	if (u < 0.0f || u > 1.0f) return false;
	const Vec3 q = cross(s, e1);
	const float v = dot(d, q) * inv;
	if (v < 0.0f || u + v > 1.0f) return false;
	t = dot(e2, q) * inv;
	return t >= 0.0f;
}

} // namespace

bool Triggers::rayDown(Vec3 from, float maxDrop, float& hitY, Vec3& normal, int* which) const {
	if (!_coll) return false;
	bool found = false;
	float best = from.y - maxDrop;
	for (u32 k = 0; k < _solidCount; ++k) {
		const u32 i = _solidList[k];
		if (!solid(i)) continue;
		const State& s = _state[i];
		const Vec3 c = s.worldCenter;
		const float dx = c.x - from.x, dz = c.z - from.z;
		if (dx * dx + dz * dz > s.collRadius * s.collRadius || c.y - s.collRadius > from.y || c.y + s.collRadius < best) continue;
		const Vec3 o = toLocal(i, from);
		const Vec3 down = quatRotate({-s.rot.x, -s.rot.y, -s.rot.z, s.rot.w}, {0, -1, 0});
		const float* tri = _coll + size_t(_list[i].collFirst) * 9;
		for (u32 j = 0; j < _list[i].collCount; ++j, tri += 9) {
			const Vec3 a = {tri[0], tri[1], tri[2]}, b = {tri[3], tri[4], tri[5]}, cc = {tri[6], tri[7], tri[8]};
			float t;
			if (!rayTriangle(o, down, a, b, cc, t) || t > maxDrop) continue;
			const float y = from.y - t;
			if (y < best) continue;
			best = y;
			hitY = y;
			Vec3 n = quatRotate(s.rot, cross(b - a, cc - a));
			const float len = sqrtf(dot(n, n));
			n = len > 1e-6f ? n * (1.0f / len) : Vec3 {0, 1, 0};
			if (n.y < 0.0f) n = n * -1.0f;
			normal = n;
			if (which) *which = int(i);
			found = true;
		}
	}
	return found;
}

bool Triggers::pushSphere(Vec3& center, float radius) const {
	if (!_coll) return false;
	bool moved = false;
	for (u32 k = 0; k < _solidCount; ++k) {
		const u32 i = _solidList[k];
		if (!solid(i)) continue;
		const State& st = _state[i];
		const Vec3 c = st.worldCenter;
		const Vec3 d = center - c;
		const float reach = st.collRadius + radius;
		if (dot(d, d) > reach * reach) continue;
		Vec3 p = toLocal(i, center);
		bool here = false;
		const float* tri = _coll + size_t(_list[i].collFirst) * 9;
		for (u32 j = 0; j < _list[i].collCount; ++j, tri += 9) {
			const Vec3 a = {tri[0], tri[1], tri[2]}, b = {tri[3], tri[4], tri[5]}, cc = {tri[6], tri[7], tri[8]};
			const Vec3 q = closestPointOnTriangle(p, a, b, cc);
			const Vec3 dq = p - q;
			const float d2 = dot(dq, dq);
			if (d2 >= radius * radius) continue;
			const float len = sqrtf(d2);
			Vec3 n;
			if (len > 1e-4f) n = dq * (1.0f / len);
			else {
				n = cross(b - a, cc - a);
				const float nl = sqrtf(dot(n, n));
				if (nl < 1e-6f) continue;
				n = n * (1.0f / nl);
			}
			p = q + n * radius;
			here = true;
		}
		if (here) {
			center = toWorld(i, p);
			moved = true;
		}
	}
	return moved;
}

bool Triggers::segmentHits(Vec3 a, Vec3 b) const {
	if (!_coll) return false;
	const Vec3 ab = b - a;
	const float len = sqrtf(dot(ab, ab));
	if (len < 1e-3f) return false;
	for (u32 k = 0; k < _solidCount; ++k) {
		const u32 i = _solidList[k];
		if (!solid(i)) continue;
		const State& s = _state[i];
		// (The segment's nearest approach to the sphere.)
		const Vec3 c = s.worldCenter;
		float t = dot(c - a, ab) / (len * len);
		t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
		const Vec3 near = a + ab * t - c;
		if (dot(near, near) > s.collRadius * s.collRadius) continue;
		const Vec3 o = toLocal(i, a);
		const Vec3 d = quatRotate({-s.rot.x, -s.rot.y, -s.rot.z, s.rot.w}, ab * (1.0f / len));
		const float* tri = _coll + size_t(_list[i].collFirst) * 9;
		for (u32 j = 0; j < _list[i].collCount; ++j, tri += 9) {
			float hit;
			if (rayTriangle(o, d, {tri[0], tri[1], tri[2]}, {tri[3], tri[4], tri[5]}, {tri[6], tri[7], tri[8]}, hit) && hit <= len) return true;
		}
	}
	return false;
}

bool Triggers::boxTouches(const float mn[3], const float mx[3]) const {
	if (!_coll) return false;
	for (u32 k = 0; k < _solidCount; ++k) {
		const u32 i = _solidList[k];
		if (!solid(i)) continue;
		const State& s = _state[i];
		const Vec3 c = s.worldCenter;
		const float r = s.collRadius;
		if (c.x + r < mn[0] || c.x - r > mx[0] || c.y + r < mn[1] || c.y - r > mx[1] || c.z + r < mn[2] || c.z - r > mx[2]) continue;
		return true;
	}
	return false;
}

int Triggers::floorUnder(Vec3 feet, Vec3& local) const {
	float y;
	Vec3 n;
	int which = -1;
	if (!rayDown({feet.x, feet.y + 30.0f, feet.z}, 60.0f, y, n, &which) || which < 0) return -1;
	if (_list[which].kind != TRG_MOVER || _state[which].mover == MV_IDLE) return -1; // (only what moves carries)
	local = toLocal(u32(which), feet);
	return which;
}

Vec3 Triggers::fromLocal(int which, Vec3 local) const {
	return which >= 0 && u32(which) < _h.count ? toWorld(u32(which), local) : local;
}

void Triggers::draw(Pak& pak, const World& world, Vec3 eye, const float dayLight[3], float range) {
	_stats.drawn = 0;
	if (!_blob) return;
	bool loadedOne = false;
	u32 rays = 0;
	for (u32 k = 0; k < _moverCount + _doorCount; ++k) {
		const u32 i = k < _moverCount ? _moverList[k] : _doorList[k - _moverCount];
		const WorldTrigger& t = _list[i];
		State& s = _state[i];
		if ((t.kind != TRG_MOVER && t.kind != TRG_DOOR) || s.model == NO_MODEL) continue;
		const Vec3 d = s.pos - eye;
		if (dot(d, d) > range * range) continue;
		if (s.model == NOT_TRIED) {
			if (loadedOne) continue; // (one model read per frame)
			loadedOne = true;
			s.model = NO_MODEL;
			char name[112];
			snprintf(name, sizeof(name), "model/item.%s", _names + t.visual);
			const u32 hash = hashName(name);
			for (u32 j = 0; j < _modelCount; ++j)
				if (_models[j].hash == hash) s.model = u16(_models[j].model.loaded() ? j : NO_MODEL);
			if (s.model == NO_MODEL && _modelCount < MODELS) {
				Look& look = _models[_modelCount];
				look.hash = hash;
				if (look.model.loadEntry(pak, hash, &_pool) && look.model.hasMesh()) s.model = u16(_modelCount);
				else look.model.release();
				++_modelCount;
			}
			if (s.model == NO_MODEL) continue;
		}
		if (!s.lit && rays < 4) {
			++rays;
			float y;
			Vec3 n;
			u8 l[4];
			// (Under a lift's floor or a gate's middle there is ground somewhere.)
			if (world.rayDown({s.pos.x, s.pos.y + 50.0f, s.pos.z}, 3000.0f, y, n, l)) {
				s.sky = float(l[3]) * (1.0f / 255.0f);
				for (int c = 0; c < 3; ++c) s.light[c] = fmaxf(float(l[c]) * (1.0f / 255.0f) * 1.15f, 0.25f);
			} else {
				s.sky = 1.0f;
				s.light[0] = s.light[1] = s.light[2] = 0.8f;
			}
			s.lit = true;
		}
		if (!s.lit) continue;
		const Model& m = _models[s.model].model;
		if (_hasView || _minPixels > 0.0f) {
			// Out of view, or too small from here (fix87).
			const ModelHeader& mh = m.header();
			float r = 0.0f;
			for (int a = 0; a < 3; ++a) r = fmaxf(r, fmaxf(fabsf(mh.bbMin[a]), fabsf(mh.bbMax[a])));
			r = fmaxf(r, 10.0f);
			if (_minPixels > 0.0f && 2.0f * r * _focalPx < _minPixels * sqrtf(dot(d, d))) continue;
			if (_hasView) {
				const float mn[3] = {s.pos.x - r, s.pos.y - r, s.pos.z - r}, mx[3] = {s.pos.x + r, s.pos.y + r, s.pos.z + r};
				if (!aabbInFrustum(_view, mn, mx)) continue;
			}
		}
		float light[3];
		for (int c = 0; c < 3; ++c) light[c] = fmaxf(s.light[c] * (1.0f + s.sky * (dayLight[c] - 1.0f)), 0.22f);
		const Mat34 skin = mat34From(s.rot, {0, 0, 0});
		m.drawSkinned(&skin, s.pos, light);
		++_stats.drawn;
	}
	_hasView = false; // (for this frame's draw only)
}

} // namespace g3
