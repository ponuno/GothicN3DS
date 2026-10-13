#include "rooms.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace g3 {

bool Rooms::load(Pak& pak, const char* worldAsset) {
	release();
	char name[112];
	snprintf(name, sizeof(name), "%s/rooms", worldAsset);
	const PakEntry* e = pak.find(name);
	if (!e || e->size < sizeof(RoomsHeader)) return false;
	u8* blob = static_cast<u8*>(pak.readAlloc(*e));
	if (!blob) return false;
	const bool ok = adopt(blob, e->size);
	free(blob);
	return ok;
}

bool Rooms::adopt(const u8* data, u32 size) {
	release();
	if (!data || size < sizeof(RoomsHeader)) return false;
	RoomsHeader h;
	memcpy(&h, data, sizeof(h));
	if (h.magic != ROOMS_MAGIC || h.version != ROOMS_VERSION || h.roomCount == 0 || h.roomCount > 65534 || h.boxCount > (1u << 22) ||
	    h.nameBytes == 0 || h.nameBytes % 4 != 0 || h.nameBytes > (1u << 22) ||
	    u64(sizeof(RoomsHeader)) + u64(h.boxCount) * sizeof(RoomBox) + u64(h.roomCount) * 4u + h.nameBytes != size)
		return false;
	_blob = static_cast<u8*>(malloc(size));
	if (!_blob) return false;
	memcpy(_blob, data, size);
	_h = h;
	_boxes = reinterpret_cast<const RoomBox*>(_blob + sizeof(RoomsHeader));
	_offsets = reinterpret_cast<const u32*>(_boxes + h.boxCount);
	_names = reinterpret_cast<const char*>(_offsets + h.roomCount);
	bool good = _names[h.nameBytes - 1] == 0;
	for (u32 i = 0; good && i < h.roomCount; ++i) good = _offsets[i] < h.nameBytes;
	for (u32 i = 0; good && i < h.boxCount; ++i) {
		const RoomBox& b = _boxes[i];
		good = b.room >= 1 && b.room <= h.roomCount && isfinite(b.bbMin[0]) && isfinite(b.bbMin[1]) && isfinite(b.bbMin[2]) &&
		       isfinite(b.bbMax[0]) && isfinite(b.bbMax[1]) && isfinite(b.bbMax[2]);
	}
	if (good) {
		_hashes = static_cast<u32*>(malloc(size_t(h.roomCount) * sizeof(u32)));
		good = _hashes != nullptr;
		for (u32 i = 0; good && i < h.roomCount; ++i) _hashes[i] = hashName(_names + _offsets[i]);
	}
	if (!good || !build()) {
		release();
		return false;
	}
	return true;
}

// The grid: cells of 16 m (a city of several kilometres is a few tens of thousands of cells at most).
bool Rooms::build() {
	if (_h.boxCount == 0) return true;
	float x0 = _boxes[0].bbMin[0], z0 = _boxes[0].bbMin[2], x1 = _boxes[0].bbMax[0], z1 = _boxes[0].bbMax[2];
	for (u32 i = 1; i < _h.boxCount; ++i) {
		const RoomBox& b = _boxes[i];
		x0 = b.bbMin[0] < x0 ? b.bbMin[0] : x0;
		z0 = b.bbMin[2] < z0 ? b.bbMin[2] : z0;
		x1 = b.bbMax[0] > x1 ? b.bbMax[0] : x1;
		z1 = b.bbMax[2] > z1 ? b.bbMax[2] : z1;
	}
	_cell = 1600.0f;
	while ((x1 - x0) / _cell * (z1 - z0) / _cell > 65536.0f) _cell *= 2.0f;
	_x0 = x0;
	_z0 = z0;
	_gw = u32((x1 - x0) / _cell) + 1;
	_gh = u32((z1 - z0) / _cell) + 1;
	const u32 cells = _gw * _gh;
	_cellStart = static_cast<u32*>(calloc(size_t(cells) + 1, sizeof(u32)));
	if (!_cellStart) return false;
	const auto span = [&](const RoomBox& b, u32& cx0, u32& cz0, u32& cx1, u32& cz1) {
		cx0 = u32((b.bbMin[0] - _x0) / _cell);
		cz0 = u32((b.bbMin[2] - _z0) / _cell);
		cx1 = u32((b.bbMax[0] - _x0) / _cell);
		cz1 = u32((b.bbMax[2] - _z0) / _cell);
		if (cx1 >= _gw) cx1 = _gw - 1;
		if (cz1 >= _gh) cz1 = _gh - 1;
	};
	u64 items = 0;
	for (u32 i = 0; i < _h.boxCount; ++i) {
		u32 cx0, cz0, cx1, cz1;
		span(_boxes[i], cx0, cz0, cx1, cz1);
		for (u32 z = cz0; z <= cz1; ++z)
			for (u32 x = cx0; x <= cx1; ++x) ++_cellStart[z * _gw + x + 1];
		items += u64(cx1 - cx0 + 1) * (cz1 - cz0 + 1);
	}
	if (items > (1u << 22)) return false;
	for (u32 c = 0; c < cells; ++c) _cellStart[c + 1] += _cellStart[c];
	_cellItems = static_cast<u32*>(malloc(size_t(items ? items : 1) * sizeof(u32)));
	u32* fill = static_cast<u32*>(malloc(size_t(cells) * sizeof(u32)));
	if (!_cellItems || !fill) {
		free(fill);
		return false;
	}
	memcpy(fill, _cellStart, size_t(cells) * sizeof(u32));
	for (u32 i = 0; i < _h.boxCount; ++i) {
		u32 cx0, cz0, cx1, cz1;
		span(_boxes[i], cx0, cz0, cx1, cz1);
		for (u32 z = cz0; z <= cz1; ++z)
			for (u32 x = cx0; x <= cx1; ++x) _cellItems[fill[z * _gw + x]++] = i;
	}
	free(fill);
	return true;
}

void Rooms::release() {
	free(_blob);
	free(_hashes);
	free(_cellStart);
	free(_cellItems);
	_blob = nullptr;
	_hashes = nullptr;
	_cellStart = _cellItems = nullptr;
	_boxes = nullptr;
	_offsets = nullptr;
	_names = nullptr;
	_h = {};
	_gw = _gh = 0;
}

// The leaf boxes are those of the leaves' polygons: one over a bare floor is flat. So a point a little above the
// feet is looked for first; failing that, the feet with some room above a box. Of several boxes, the smallest.
u16 Rooms::roomAt(Vec3 feet) const {
	if (!_cellStart || _gw == 0) return 0;
	const float fx = (feet.x - _x0) / _cell, fz = (feet.z - _z0) / _cell;
	if (fx < 0.0f || fz < 0.0f || fx >= float(_gw) || fz >= float(_gh)) return 0;
	const u32 c = u32(fz) * _gw + u32(fx);
	for (int pass = 0; pass < 2; ++pass) {
		const float y = pass == 0 ? feet.y + 60.0f : feet.y + 10.0f;
		const float below = pass == 0 ? 0.0f : 40.0f, above = pass == 0 ? 0.0f : 200.0f;
		u16 best = 0;
		float bestVolume = 0.0f;
		for (u32 k = _cellStart[c]; k < _cellStart[c + 1]; ++k) {
			const RoomBox& b = _boxes[_cellItems[k]];
			if (feet.x < b.bbMin[0] || feet.x > b.bbMax[0] || feet.z < b.bbMin[2] || feet.z > b.bbMax[2]) continue;
			if (y < b.bbMin[1] - below || y > b.bbMax[1] + above) continue;
			const float v = (b.bbMax[0] - b.bbMin[0]) * (b.bbMax[1] - b.bbMin[1] + 1.0f) * (b.bbMax[2] - b.bbMin[2]);
			if (!best || v < bestVolume) {
				best = b.room;
				bestVolume = v;
			}
		}
		if (best) return best;
	}
	return 0;
}

const char* Rooms::name(u16 room) const {
	return room >= 1 && room <= _h.roomCount ? _names + _offsets[room - 1] : "";
}

} // namespace g3
