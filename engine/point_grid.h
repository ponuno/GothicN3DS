// Points of the world that do not move (things lying about, things to use, doors) sorted into squares of the
// ground, so that "what is near here" looks at the few squares around a place instead of at every point - Archolos
// has thousands of each, and the old loops went through all of them every frame (fix87).
#pragma once
#include "g3ds/base.h"

namespace g3 {

class PointGrid {
public:
	PointGrid() = default;
	~PointGrid() { release(); }
	PointGrid(const PointGrid&) = delete;
	PointGrid& operator=(const PointGrid&) = delete;

	// Point i is at (x[i * stride], z[i * stride]), cm (stride in floats: the size of the caller's record).
	// `cell`: the edge of a square, cm. False on allocation failure (then built() is false: the caller looks at
	// everything, as before).
	bool build(const float* x, const float* z, u32 count, u32 stride, float cell = 1600.0f);
	void release();
	bool built() const { return _ok; }

	// Calls visit(ctx, index) for every point whose square reaches within `radius` of (x, z) - a few more than
	// those within the radius (the caller measures); each index once. Returns how many it visited.
	u32 query(float x, float z, float radius, void (*visit)(void* ctx, u32 index), void* ctx) const;
	// The same, the indices written to `out` (at most `max`); returns how many.
	u32 collect(float x, float z, float radius, u32* out, u32 max) const;
	// The same with a callable f(index); without a grid (not built) every index 0..total-1.
	template <class F> void forNear(float x, float z, float radius, u32 total, F& f) const {
		if (!_ok) {
			for (u32 i = 0; i < total; ++i) f(i);
			return;
		}
		query(x, z, radius, [](void* c, u32 i) { (*static_cast<F*>(c))(i); }, &f);
	}

private:
	struct Item {
		u32 key;   // the square: (cz + 32768) << 16 | (cx + 32768)
		u32 index;
	};
	u32 keyOf(i32 cx, i32 cz) const { return (u32(cz + 32768) << 16) | u32(cx + 32768); }
	// The first item of square `key`, or _count if none.
	u32 find(u32 key) const;

	Item* _items = nullptr;
	u32 _count = 0;
	float _cell = 1600.0f;
	bool _ok = false;
};

} // namespace g3
