#include "mobs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace g3 {

namespace {

char upperChar(char c) { return c >= 'a' && c <= 'z' ? char(c - 32) : c; }

bool finite3(const float p[3]) { return isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]); }

} // namespace

bool Mobs::load(Pak& pak, const char* worldAsset) {
	release();
	char name[112];
	snprintf(name, sizeof(name), "%s/mobs", worldAsset);
	const PakEntry* e = pak.find(name);
	if (!e || e->size < sizeof(MobsHeader)) return false;
	u8* blob = static_cast<u8*>(pak.readAlloc(*e));
	if (!blob) return false;
	MobsHeader h;
	memcpy(&h, blob, sizeof(h));
	bool ok = h.magic == MOBS_MAGIC && h.version == MOBS_VERSION && h.mobCount <= 65534 && h.slotCount < (1u << 20) &&
	          h.nameBytes < (1u << 24) && h.nameBytes % 4 == 0 &&
	          u64(sizeof(MobsHeader)) + u64(h.mobCount) * sizeof(WorldMob) + u64(h.slotCount) * sizeof(MobSlot) + h.nameBytes == e->size;
	const WorldMob* mobs = reinterpret_cast<const WorldMob*>(blob + sizeof(MobsHeader));
	const MobSlot* slots = reinterpret_cast<const MobSlot*>(mobs + (ok ? h.mobCount : 0));
	const char* names = reinterpret_cast<const char*>(slots + (ok ? h.slotCount : 0));
	if (ok && h.mobCount) ok = h.nameBytes > 0 && names[h.nameBytes - 1] == 0;
	for (u32 i = 0; ok && i < h.mobCount; ++i)
		ok = mobs[i].scheme < h.nameBytes && mobs[i].firstSlot <= h.slotCount && mobs[i].slotCount <= h.slotCount - mobs[i].firstSlot &&
		     finite3(mobs[i].pos) && isfinite(mobs[i].yaw) && mobs[i].states <= 16;
	for (u32 i = 0; ok && i < h.slotCount; ++i) ok = finite3(slots[i].pos) && isfinite(slots[i].yaw) && slots[i].kind <= MOB_SLOT_DIST;
	if (ok) {
		_user = static_cast<u16*>(calloc(h.mobCount + 1, sizeof(u16)));
		ok = _user != nullptr;
	}
	if (!ok) {
		free(blob);
		release();
		return false;
	}
	_blob = blob;
	_h = h;
	_mobs = mobs;
	_slots = slots;
	_names = names;
	loadInfo(pak, worldAsset);
	return true;
}

// The optional table of what the player may do (a pak baked before it has none: nothing is for him).
void Mobs::loadInfo(Pak& pak, const char* worldAsset) {
	char name[112];
	snprintf(name, sizeof(name), "%s/mobinfo", worldAsset);
	const PakEntry* e = pak.find(name);
	if (!e || e->size < sizeof(MobInfoHeader)) return;
	u8* blob = static_cast<u8*>(pak.readAlloc(*e));
	if (!blob) return;
	MobInfoHeader h;
	memcpy(&h, blob, sizeof(h));
	bool ok = h.magic == MOBINFO_MAGIC && h.version == MOBINFO_VERSION && h.count == _h.mobCount && h.nameBytes > 0 && h.nameBytes < (1u << 24) &&
	          h.nameBytes % 4 == 0 && u64(sizeof(MobInfoHeader)) + u64(h.count) * sizeof(WorldMobInfo) + h.nameBytes == e->size;
	const WorldMobInfo* infos = reinterpret_cast<const WorldMobInfo*>(blob + sizeof(MobInfoHeader));
	const char* names = reinterpret_cast<const char*>(infos + (ok ? h.count : 0));
	if (ok) ok = names[0] == 0 && names[h.nameBytes - 1] == 0;
	for (u32 i = 0; ok && i < h.count; ++i)
		ok = infos[i].focus < h.nameBytes && infos[i].onState < h.nameBytes && infos[i].condition < h.nameBytes && infos[i].useItem < h.nameBytes &&
		     infos[i].target < h.nameBytes && infos[i].kind <= MOBKIND_OTHER;
	if (!ok) {
		free(blob);
		return;
	}
	_infoBlob = blob;
	_infos = infos;
	_infoNames = names;
}

bool Mobs::isBed(u32 i) const {
	if (!_infos || i >= _h.mobCount) return false;
	if (_infos[i].kind == MOBKIND_BED) return true;
	if (_infos[i].kind == MOBKIND_CONTAINER || _infos[i].kind == MOBKIND_DOOR) return false;
	// (By its scheme only when it has no function of its own, or a sleeping one: a thing that runs something else is that.)
	const auto has = [](const char* s, const char* w) {
		for (; *s; ++s) {
			u32 k = 0;
			while (w[k] && (s[k] == w[k] || s[k] == char(w[k] + 32))) ++k;
			if (!w[k]) return true;
		}
		return false;
	};
	const char* on = infoText(_infos[i].onState);
	return has(scheme(i), "BED") && (!*on || has(on, "SLEEP"));
}

bool Mobs::forPlayer(u32 i) const {
	if (!_infos || i >= _h.mobCount) return false;
	const WorldMob& w = _mobs[i];
	const WorldMobInfo& m = _infos[i];
	// A bed is to sleep in whether the world's maker named it or not (OpenGothic offers every bed; Gothic's
	// own beds go without a focus name often enough) - one with a place to lie down at.
	if (isBed(i)) return w.slotCount > 0 || m.focus != 0;
	return m.focus != 0 && m.kind != MOBKIND_CONTAINER && m.kind != MOBKIND_DOOR && m.kind != MOBKIND_OTHER;
}

void Mobs::release() {
	free(_blob);
	free(_infoBlob);
	free(_user);
	_blob = nullptr;
	_infoBlob = nullptr;
	_infos = nullptr;
	_infoNames = nullptr;
	_user = nullptr;
	_mobs = nullptr;
	_slots = nullptr;
	_names = nullptr;
	_h = MobsHeader {};
}

bool Mobs::isScheme(u32 mob, const char* scheme) const {
	if (mob >= _h.mobCount || !scheme) return false;
	const char* own = _names + _mobs[mob].scheme;
	for (;; ++own, ++scheme) {
		if (upperChar(*own) != upperChar(*scheme)) return false;
		if (!*own) return true;
	}
}

int Mobs::find(const float pos[3], const char* scheme, u32 owner, int except) const {
	int best = -1;
	return candidates(pos, scheme, owner, except, &best, 1) ? best : -1;
}

u32 Mobs::candidates(const float pos[3], const char* scheme, u32 owner, int except, int* out, u32 max) const {
	if (!scheme || !*scheme || !max) return 0;
	char name[64];
	size_t n = 0;
	for (; scheme[n] && n + 1 < sizeof(name); ++n) name[n] = upperChar(scheme[n]);
	name[n] = 0;
	const u32 hash = hashName(name);
	// Kept in order, nearest first; the farthest drops out of a full list.
	float d2s[16];
	u32 count = 0;
	if (max > 16) max = 16;
	for (u32 i = 0; i < _h.mobCount; ++i) {
		const WorldMob& m = _mobs[i];
		// (A thing without a place to stand at cannot be used, as in the original.)
		if (m.schemeHash != hash || int(i) == except || m.slotCount == 0) continue;
		if (_user[i] && _user[i] != u16(owner + 1)) continue;
		const float dx = m.pos[0] - pos[0], dy = m.pos[1] - pos[1], dz = m.pos[2] - pos[2];
		const float d2 = dx * dx + dy * dy + dz * dz;
		if (d2 >= SEARCH_RANGE * SEARCH_RANGE || (count == max && d2 >= d2s[count - 1]) || !isScheme(i, name)) continue;
		u32 at = count < max ? count++ : max - 1;
		for (; at > 0 && d2s[at - 1] > d2; --at) {
			d2s[at] = d2s[at - 1];
			out[at] = out[at - 1];
		}
		d2s[at] = d2;
		out[at] = int(i);
	}
	return count;
}

int Mobs::place(u32 mob, const float from[3], float pos[3], float& yaw) const {
	const WorldMob& m = _mobs[mob];
	int best = -1;
	float bestD2 = 1e30f;
	for (u32 k = 0; k < m.slotCount; ++k) {
		const MobSlot& s = _slots[m.firstSlot + k];
		float at[3] = {s.pos[0], s.pos[1], s.pos[2]};
		float facing = s.yaw;
		if (s.kind == MOB_SLOT_DIST) {
			// As far from the mob as the slot is, on the side one comes from.
			const float sx = s.pos[0] - m.pos[0], sz = s.pos[2] - m.pos[2];
			const float reach = sqrtf(sx * sx + sz * sz);
			float dx = from[0] - m.pos[0], dz = from[2] - m.pos[2];
			const float len = sqrtf(dx * dx + dz * dz);
			if (len < 1.0f) {
				dx = sx;
				dz = sz;
			}
			const float inv = 1.0f / (len < 1.0f ? (reach > 0.0f ? reach : 1.0f) : len);
			at[0] = m.pos[0] + dx * inv * reach;
			at[2] = m.pos[2] + dz * inv * reach;
			facing = atan2f(-dx, -dz);
		}
		const float dx = at[0] - from[0], dy = at[1] - from[1], dz = at[2] - from[2];
		const float d2 = dx * dx + dy * dy + dz * dz;
		if (d2 >= bestD2) continue;
		bestD2 = d2;
		best = int(k);
		memcpy(pos, at, 3 * sizeof(float));
		yaw = facing;
	}
	if (best < 0) {
		memcpy(pos, m.pos, 3 * sizeof(float));
		yaw = m.yaw;
	}
	return best;
}

bool Mobs::hold(u32 mob, u32 owner) {
	if (mob >= _h.mobCount || owner >= 0xFFFFu) return false;
	if (_user[mob] && _user[mob] != u16(owner + 1)) return false;
	drop(owner);
	_user[mob] = u16(owner + 1);
	return true;
}

void Mobs::drop(u32 owner) {
	if (!_user || owner >= 0xFFFFu) return;
	for (u32 i = 0; i < _h.mobCount; ++i)
		if (_user[i] == u16(owner + 1)) _user[i] = 0;
}

int Mobs::heldBy(u32 owner) const {
	if (!_user || owner >= 0xFFFFu) return -1;
	for (u32 i = 0; i < _h.mobCount; ++i)
		if (_user[i] == u16(owner + 1)) return int(i);
	return -1;
}

int Mobs::userOf(u32 mob) const { return mob < _h.mobCount && _user[mob] ? int(_user[mob]) - 1 : -1; }

void Mobs::dropAll() {
	if (_user) memset(_user, 0, (size_t(_h.mobCount) + 1) * sizeof(u16));
}

} // namespace g3
