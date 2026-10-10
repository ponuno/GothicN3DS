#include "waynet.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace g3 {

namespace {

char upperChar(char c) { return c >= 'a' && c <= 'z' ? char(c - 32) : c; }

float distance(const float a[3], const float b[3]) {
	const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
	return sqrtf(x * x + y * y + z * z);
}

} // namespace

bool WayNet::load(Pak& pak, const char* worldAsset) {
	release();
	char name[112];
	snprintf(name, sizeof(name), "%s/waynet", worldAsset);
	const PakEntry* e = pak.find(name);
	if (!e || e->size < sizeof(WayNetHeader)) return false;
	u8* blob = static_cast<u8*>(pak.readAlloc(*e));
	if (!blob) return false;
	WayNetHeader h;
	memcpy(&h, blob, sizeof(h));
	bool ok = h.magic == WAYNET_MAGIC && h.version == WAYNET_VERSION && h.nodeCount <= 65534 && h.edgeCount < (1u << 22) &&
	          h.freeCount < 65535 && h.nameBytes < (1u << 24) && h.nameBytes % 4 == 0;
	u32 atNodes = sizeof(WayNetHeader), atEdges = 0, atFree = 0, atNames = 0;
	if (ok) {
		atEdges = atNodes + h.nodeCount * u32(sizeof(WayNode));
		atFree = atEdges + ((h.edgeCount * 2u + 3u) & ~3u);
		atNames = atFree + h.freeCount * u32(sizeof(FreePoint));
		ok = u64(atNames) + h.nameBytes == e->size;
	}
	const WayNode* nodes = reinterpret_cast<const WayNode*>(blob + atNodes);
	const u16* edges = reinterpret_cast<const u16*>(blob + atEdges);
	const FreePoint* free_ = reinterpret_cast<const FreePoint*>(blob + atFree);
	const char* names = reinterpret_cast<const char*>(blob + atNames);
	if (ok && (h.nodeCount || h.freeCount)) ok = h.nameBytes > 0 && names[h.nameBytes - 1] == 0;
	for (u32 i = 0; ok && i < h.nodeCount; ++i)
		ok = nodes[i].name < h.nameBytes && nodes[i].firstEdge <= h.edgeCount && nodes[i].edgeCount <= h.edgeCount - nodes[i].firstEdge &&
		     isfinite(nodes[i].pos[0]) && isfinite(nodes[i].pos[1]) && isfinite(nodes[i].pos[2]) && isfinite(nodes[i].yaw);
	for (u32 i = 0; ok && i < h.edgeCount; ++i) ok = edges[i] < h.nodeCount;
	for (u32 i = 0; ok && i < h.freeCount; ++i)
		ok = free_[i].name < h.nameBytes && isfinite(free_[i].pos[0]) && isfinite(free_[i].pos[1]) && isfinite(free_[i].pos[2]) &&
		     isfinite(free_[i].yaw);
	if (ok) {
		_holder = static_cast<u16*>(calloc(h.freeCount + 1, sizeof(u16)));
		_cost = static_cast<float*>(malloc((h.nodeCount + 1) * sizeof(float)));
		_from = static_cast<u16*>(malloc((h.nodeCount + 1) * sizeof(u16)));
		_heap = static_cast<u16*>(malloc((h.nodeCount + 1) * sizeof(u16)));
		_slot = static_cast<u16*>(malloc((h.nodeCount + 1) * sizeof(u16)));
		ok = _holder && _cost && _from && _heap && _slot;
	}
	if (!ok) {
		free(blob);
		release();
		return false;
	}
	_blob = blob;
	_h = h;
	_nodes = nodes;
	_edges = edges;
	_free = free_;
	_names = names;
	return true;
}

void WayNet::release() {
	free(_blob);
	free(_holder);
	free(_cost);
	free(_from);
	free(_heap);
	free(_slot);
	_blob = nullptr;
	_holder = nullptr;
	_cost = nullptr;
	_from = nullptr;
	_heap = nullptr;
	_slot = nullptr;
	_nodes = nullptr;
	_edges = nullptr;
	_free = nullptr;
	_names = nullptr;
	_h = WayNetHeader {};
}

int WayNet::findNode(const char* name) const {
	if (!_blob || !name || !*name) return -1;
	const u32 h = hashName(name);
	for (u32 i = 0; i < _h.nodeCount; ++i)
		if (_nodes[i].nameHash == h) return int(i);
	return -1;
}

int WayNet::nearestNode(const float pos[3], int except) const {
	int best = -1;
	float bestD = 0;
	for (u32 i = 0; i < _h.nodeCount; ++i) {
		if (int(i) == except) continue;
		const float x = _nodes[i].pos[0] - pos[0], y = _nodes[i].pos[1] - pos[1], z = _nodes[i].pos[2] - pos[2];
		const float d = x * x + y * y + z * z;
		if (best < 0 || d < bestD) {
			best = int(i);
			bestD = d;
		}
	}
	return best;
}

// Dijkstra from `to` backwards, so that following _from from `from` walks
// the way forwards.
u32 WayNet::path(u32 from, u32 to, u16* out, u32 max) const {
	if (!_blob || from >= _h.nodeCount || to >= _h.nodeCount || from == to) return 0;
	const u32 n = _h.nodeCount;
	for (u32 i = 0; i < n; ++i) {
		_cost[i] = -1.0f; // not reached
		_slot[i] = 0;
	}
	u32 size = 0;
	const auto swap = [&](u32 a, u32 b) {
		const u16 t = _heap[a];
		_heap[a] = _heap[b];
		_heap[b] = t;
		_slot[_heap[a]] = u16(a + 1);
		_slot[_heap[b]] = u16(b + 1);
	};
	const auto up = [&](u32 i) {
		while (i > 0) {
			const u32 parent = (i - 1) / 2;
			if (_cost[_heap[parent]] <= _cost[_heap[i]]) break;
			swap(parent, i);
			i = parent;
		}
	};
	const auto down = [&](u32 i) {
		for (;;) {
			u32 least = i;
			const u32 l = i * 2 + 1, r = l + 1;
			if (l < size && _cost[_heap[l]] < _cost[_heap[least]]) least = l;
			if (r < size && _cost[_heap[r]] < _cost[_heap[least]]) least = r;
			if (least == i) return;
			swap(i, least);
			i = least;
		}
	};
	_cost[to] = 0.0f;
	_from[to] = u16(to);
	_heap[size] = u16(to);
	_slot[to] = u16(++size);
	bool found = false;
	while (size > 0) {
		const u32 at = _heap[0];
		if (at == from) {
			found = true;
			break;
		}
		swap(0, --size);
		_slot[at] = 0xFFFF; // done
		if (size) down(0);
		const WayNode& node = _nodes[at];
		for (u32 k = 0; k < node.edgeCount; ++k) {
			const u32 next = _edges[node.firstEdge + k];
			if (_slot[next] == 0xFFFF) continue;
			const float cost = _cost[at] + distance(node.pos, _nodes[next].pos) + 1.0f;
			if (_cost[next] >= 0.0f && _cost[next] <= cost) continue;
			_cost[next] = cost;
			_from[next] = u16(at);
			if (_slot[next] == 0) {
				_heap[size] = u16(next);
				_slot[next] = u16(++size);
			}
			up(u32(_slot[next]) - 1);
		}
	}
	if (!found) return 0;
	u32 count = 0;
	for (u32 at = from; at != to && count <= n; ++count) {
		at = _from[at];
		if (count < max) out[count] = u16(at);
	}
	return count;
}

bool WayNet::nameContains(u32 freePoint, const char* word) const {
	if (freePoint >= _h.freeCount || !word || !*word) return false;
	const char* name = _names + _free[freePoint].name;
	for (const char* at = name; *at; ++at) {
		const char* a = at;
		const char* w = word;
		while (*w && *a && *a == upperChar(*w)) {
			++a;
			++w;
		}
		if (!*w) return true;
	}
	return false;
}

int WayNet::findFree(const float pos[3], const char* word, u32 owner, int except) const {
	if (!_blob) return -1;
	int best = -1;
	float bestD = FREE_POINT_RANGE * FREE_POINT_RANGE;
	for (u32 i = 0; i < _h.freeCount; ++i) {
		if (int(i) == except) continue;
		if (_holder[i] && _holder[i] != owner + 1) continue;
		const float x = _free[i].pos[0] - pos[0], y = _free[i].pos[1] - pos[1], z = _free[i].pos[2] - pos[2];
		const float d = x * x + y * y + z * z;
		if (d >= bestD || !nameContains(i, word)) continue;
		best = int(i);
		bestD = d;
	}
	return best;
}

void WayNet::hold(int freePoint, u32 owner) {
	if (!_blob || owner >= 0xFFFF) return;
	drop(owner);
	if (freePoint >= 0 && u32(freePoint) < _h.freeCount && !_holder[freePoint]) _holder[freePoint] = u16(owner + 1);
}

void WayNet::drop(u32 owner) {
	if (!_blob) return;
	for (u32 i = 0; i < _h.freeCount; ++i)
		if (_holder[i] == owner + 1) _holder[i] = 0;
}

int WayNet::heldBy(u32 owner) const {
	if (!_blob) return -1;
	for (u32 i = 0; i < _h.freeCount; ++i)
		if (_holder[i] == owner + 1) return int(i);
	return -1;
}

void WayNet::dropAll() {
	if (_holder) memset(_holder, 0, _h.freeCount * sizeof(u16));
}

} // namespace g3
