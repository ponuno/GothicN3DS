#include "point_grid.h"

#include <math.h>
#include <stdlib.h>

namespace g3 {

namespace {
int byKey(const void* a, const void* b) {
	const u32 ka = static_cast<const u32*>(a)[0], kb = static_cast<const u32*>(b)[0];
	if (ka != kb) return ka < kb ? -1 : 1;
	const u32 ia = static_cast<const u32*>(a)[1], ib = static_cast<const u32*>(b)[1];
	return ia < ib ? -1 : ia > ib ? 1 : 0;
}

i32 squareOf(float v, float cell) {
	const float s = floorf(v / cell);
	return s < -32000.0f ? -32000 : s > 32000.0f ? 32000 : i32(s);
}
} // namespace

bool PointGrid::build(const float* x, const float* z, u32 count, u32 stride, float cell) {
	release();
	_cell = cell > 1.0f ? cell : 1.0f;
	if (count == 0) {
		_ok = true;
		return true;
	}
	_items = static_cast<Item*>(malloc(size_t(count) * sizeof(Item)));
	if (!_items) return false;
	for (u32 i = 0; i < count; ++i) {
		_items[i].key = keyOf(squareOf(x[size_t(i) * stride], _cell), squareOf(z[size_t(i) * stride], _cell));
		_items[i].index = i;
	}
	qsort(_items, count, sizeof(Item), byKey);
	_count = count;
	_ok = true;
	return true;
}

void PointGrid::release() {
	free(_items);
	_items = nullptr;
	_count = 0;
	_ok = false;
}

u32 PointGrid::find(u32 key) const {
	u32 lo = 0, hi = _count;
	while (lo < hi) {
		const u32 mid = (lo + hi) / 2;
		if (_items[mid].key < key) lo = mid + 1;
		else hi = mid;
	}
	return lo < _count && _items[lo].key == key ? lo : _count;
}

u32 PointGrid::query(float x, float z, float radius, void (*visit)(void* ctx, u32 index), void* ctx) const {
	if (!_items) return 0;
	const i32 x0 = squareOf(x - radius, _cell), x1 = squareOf(x + radius, _cell);
	const i32 z0 = squareOf(z - radius, _cell), z1 = squareOf(z + radius, _cell);
	u32 n = 0;
	for (i32 cz = z0; cz <= z1; ++cz) {
		for (i32 cx = x0; cx <= x1; ++cx) {
			const u32 key = keyOf(cx, cz);
			for (u32 k = find(key); k < _count && _items[k].key == key; ++k) {
				visit(ctx, _items[k].index);
				++n;
			}
		}
	}
	return n;
}

u32 PointGrid::collect(float x, float z, float radius, u32* out, u32 max) const {
	struct Ctx {
		u32* out;
		u32 max, n;
	} c = {out, max, 0};
	query(x, z, radius, [](void* p, u32 i) {
		Ctx& cc = *static_cast<Ctx*>(p);
		if (cc.n < cc.max) cc.out[cc.n++] = i;
	}, &c);
	return c.n;
}

} // namespace g3
