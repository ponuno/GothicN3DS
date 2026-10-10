// The way net of a world (g3ds/game.h): the points people walk between, the
// connections they walk along, and the free points beside them.
#pragma once
#include "g3ds/game.h"
#include "g3ds/pak.h"

namespace g3 {

class WayNet {
public:
	// How far away a free point may be to be found, cm (as in the original).
	static constexpr float FREE_POINT_RANGE = 800.0f;

	WayNet() = default;
	~WayNet() { release(); }
	WayNet(const WayNet&) = delete;
	WayNet& operator=(const WayNet&) = delete;

	// Loads "<worldAsset>/waynet". False if the world has none or it is damaged.
	bool load(Pak& pak, const char* worldAsset);
	void release();
	bool loaded() const { return _blob != nullptr; }

	u32 nodeCount() const { return _h.nodeCount; }
	const WayNode& node(u32 i) const { return _nodes[i]; }
	const char* nodeName(u32 i) const { return _names + _nodes[i].name; }
	u32 freeCount() const { return _h.freeCount; }
	const FreePoint& freePoint(u32 i) const { return _free[i]; }
	const char* freeName(u32 i) const { return _names + _free[i].name; }

	// The node of that name (any letter case), or -1.
	int findNode(const char* name) const;
	// The node nearest to a position, `except` aside; -1 in an empty net.
	int nearestNode(const float pos[3], int except = -1) const;
	// The shortest way from one node to another along the connections: the
	// nodes to pass, `from` not among them, `to` the last. Writes at most
	// `max` of them and returns how many the whole way has; 0 if there is no
	// way (or `from` is `to`).
	u32 path(u32 from, u32 to, u16* out, u32 max) const;

	// Free points. One is found by a word its name contains ("SIT" finds
	// "FP_SIT_CAMPFIRE_01"): the nearest within FREE_POINT_RANGE that nobody
	// else holds. `owner` is whoever asks (any number but 0xFFFF); the point
	// it holds itself counts as available, `except` (an index, or -1) does not.
	int findFree(const float pos[3], const char* word, u32 owner, int except = -1) const;
	bool nameContains(u32 freePoint, const char* word) const;
	// One point per owner: holding another lets go of the first.
	void hold(int freePoint, u32 owner);
	void drop(u32 owner);
	int heldBy(u32 owner) const;
	void dropAll();

private:
	u8* _blob = nullptr;
	WayNetHeader _h {};
	const WayNode* _nodes = nullptr;
	const u16* _edges = nullptr;
	const FreePoint* _free = nullptr;
	const char* _names = nullptr;
	u16* _holder = nullptr; // per free point: owner + 1, 0 for nobody
	// Scratch of path().
	mutable float* _cost = nullptr;
	mutable u16* _from = nullptr;
	mutable u16* _heap = nullptr;
	mutable u16* _slot = nullptr; // per node: where in the heap + 1, 0 if not in it
};

} // namespace g3
