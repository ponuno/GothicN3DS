#include "world.h"

#include <stdlib.h>
#include <string.h>

namespace g3 {

bool World::open(Pak* pak, const char* name) {
	char path[96];
	snprintf(path, sizeof(path), "world/%s", name);
	if (!open(pak, hashName(path))) {
		logf("world: '%s' not found in pak (or baked by another version)", path);
		return false;
	}
	return true;
}

bool World::open(Pak* pak, u32 entryHash) {
	close();
	const PakEntry* e = pak->find(entryHash);
	if (!e || e->type != ASSET_WORLD || e->size < sizeof(WorldHeader)) return false;
	u8* blob = static_cast<u8*>(pak->readAlloc(*e));
	if (!blob) return false;

	memcpy(&_hdr, blob, sizeof(_hdr));
	const u64 matBytes = u64(_hdr.materialCount) * sizeof(WorldMaterial);
	const u64 texBytes = u64(_hdr.textureCount) * sizeof(u32);
	const u64 portalBytes = u64(_hdr.portalCount) * sizeof(WorldPortal);
	const u64 ladderBytes = u64(_hdr.ladderCount) * sizeof(WorldLadder);
	const u64 chunkBytes = u64(_hdr.chunkCount) * sizeof(ChunkInfo);
	bool ok = _hdr.magic == WORLD_MAGIC && _hdr.version == WORLD_VERSION &&
	          u64(e->size) == sizeof(WorldHeader) + matBytes + texBytes + portalBytes + ladderBytes + chunkBytes;
	if (ok) {
		_materials = static_cast<WorldMaterial*>(malloc(size_t(matBytes) + 1));
		_textures = static_cast<u32*>(malloc(size_t(texBytes) + 1));
		_portals = static_cast<WorldPortal*>(malloc(size_t(portalBytes) + 1));
		_ladders = static_cast<WorldLadder*>(malloc(size_t(ladderBytes) + 1));
		_chunks = static_cast<ChunkInfo*>(malloc(size_t(chunkBytes) + 1));
		_loaded = static_cast<Loaded*>(calloc(size_t(_hdr.chunkCount) + 1, sizeof(Loaded)));
		_tex = static_cast<TexSlot*>(calloc(size_t(_hdr.textureCount) + 1, sizeof(TexSlot)));
		_resident = static_cast<u32*>(malloc((size_t(_hdr.chunkCount) + 1) * sizeof(u32)));
		_order = static_cast<u32*>(malloc((size_t(_hdr.chunkCount) + 1) * sizeof(u32)));
		_orderKey = static_cast<float*>(malloc((size_t(_hdr.chunkCount) + 1) * sizeof(float)));
		ok = _materials && _textures && _portals && _ladders && _chunks && _loaded && _tex && _resident && _order &&
		     _orderKey;
	}
	if (ok) {
		const u8* p = blob + sizeof(WorldHeader);
		memcpy(_materials, p, size_t(matBytes));
		p += matBytes;
		memcpy(_textures, p, size_t(texBytes));
		p += texBytes;
		memcpy(_portals, p, size_t(portalBytes));
		p += portalBytes;
		memcpy(_ladders, p, size_t(ladderBytes));
		p += ladderBytes;
		memcpy(_chunks, p, size_t(chunkBytes));
		// Every material must stay inside the texture table with all its frames.
		for (u32 i = 0; ok && i < _hdr.materialCount; ++i)
			ok = _materials[i].frameCount >= 1 && u32(_materials[i].texture) + _materials[i].frameCount <= _hdr.textureCount;
	}
	free(blob);
	if (!ok) {
		_hdr = {};
		close();
		return false;
	}
	_pak = pak;
	_entryHash = entryHash;
	_stats = {};
	_stats.chunksTotal = _hdr.chunkCount;
	_frame = 0;
	_textureRetryAt = 0;
	_chunksInFlight = _inFlightBytes = 0;
	// Failing to start the loader is not fatal: update() then loads on the spot.
	if (_cfg.background) _loader.start(pak->path(), _materials, _hdr.materialCount, _cfg.worker);
	return true;
}

void World::close() {
	_loader.stop(); // before the tables it reads go away
	if (_loaded) {
		for (u32 i = 0; i < _hdr.chunkCount; ++i)
			if (_loaded[i].mesh) unloadChunk(i);
	}
	free(_materials);
	free(_textures);
	free(_portals);
	free(_ladders);
	free(_chunks);
	free(_loaded);
	free(_tex);
	free(_resident);
	free(_order);
	free(_orderKey);
	_materials = nullptr;
	_textures = nullptr;
	_portals = nullptr;
	_ladders = nullptr;
	_chunks = nullptr;
	_loaded = nullptr;
	_tex = nullptr;
	_resident = nullptr;
	_order = nullptr;
	_orderKey = nullptr;
	_pak = nullptr;
	_entryHash = 0;
	_visible = 0;
	_texturesWaiting = 0;
	_hdr = {};
	_stats = {}; // counters describe the world that was open
}

static float aabbDistance(const float mn[3], const float mx[3], Vec3 c) {
	const float dx = max(max(mn[0] - c.x, c.x - mx[0]), 0.0f);
	const float dy = max(max(mn[1] - c.y, c.y - mx[1]), 0.0f);
	const float dz = max(max(mn[2] - c.z, c.z - mx[2]), 0.0f);
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

float World::distanceTo(u32 index, Vec3 c) const { return aabbDistance(_chunks[index].bbMin, _chunks[index].bbMax, c); }

void World::setWaiting(TexSlot& s, bool waiting) {
	if (s.waiting == waiting) return;
	s.waiting = waiting;
	if (waiting)
		++_texturesWaiting;
	else
		--_texturesWaiting;
}

bool World::textureJob(u32 texture, LoadJob& job) const {
	const PakEntry* e = _pak->find(_textures[texture]);
	if (!e) return false;
	job = LoadJob {};
	job.kind = LOAD_TEXTURE;
	job.index = texture;
	job.entry = *e;
	return true;
}

bool World::chunkJob(u32 index, LoadJob& job) const {
	const PakEntry* e = _pak->find(_chunks[index].entryHash);
	if (!e) return false;
	job = LoadJob {};
	job.kind = LOAD_CHUNK;
	job.index = index;
	job.entry = *e;
	job.info = _chunks[index];
	return true;
}

// Takes a texture the loader has read and checked. Bad data is remembered
// and never retried. Running out of memory is not: the texture stays
// `waiting` and update() tries again later.
void World::finishTexture(LoadResult& r) {
	TexSlot& s = _tex[r.index];
	s.requested = false;
	if (s.refs == 0 || s.tex || s.failed) {
		r.discard(); // nobody wants it any more
		return;
	}
	if (r.status == LOAD_BAD_DATA) {
		s.failed = true; // do not hit the SD card again every frame
		++_stats.loadFailures;
		setWaiting(s, false);
	} else if (r.status == LOAD_OK) {
		TexHeader h;
		memcpy(&h, r.blob, sizeof(h));
		s.tex = gfx::textureCreate(h, r.blob + sizeof(TexHeader));
		if (s.tex) {
			s.bytes = h.dataSize;
			_stats.residentBytes += s.bytes;
			_stats.peakResidentBytes = max(_stats.peakResidentBytes, _stats.residentBytes);
			++_stats.texturesLoaded;
			setWaiting(s, false);
		}
	}
	if (!s.tex && !s.failed) {
		setWaiting(s, true);
		_textureRetryAt = _frame + 30;
	}
	r.discard();
}

void World::loadTextureNow(u32 texture) {
	TexSlot& s = _tex[texture];
	if (s.tex || s.failed) return;
	LoadJob job;
	LoadResult r {};
	r.kind = LOAD_TEXTURE;
	r.index = texture;
	r.status = LOAD_BAD_DATA;
	if (textureJob(texture, job)) StreamLoader::process(*_pak, job, _materials, _hdr.materialCount, r);
	finishTexture(r);
}

void World::acquireTexture(u32 texture) {
	TexSlot& s = _tex[texture];
	++s.refs;
	if (s.tex || s.failed) return;
	if (_loader.running() && !_immediate)
		setWaiting(s, true); // update() sends for it
	else
		loadTextureNow(texture);
}

void World::releaseTexture(u32 texture) {
	TexSlot& s = _tex[texture];
	if (s.refs == 0 || --s.refs != 0) return;
	if (s.tex) {
		gfx::textureDestroy(s.tex);
		_stats.residentBytes -= s.bytes;
		--_stats.texturesLoaded;
	}
	setWaiting(s, false);
	s.tex = nullptr;
	s.bytes = 0;
}

void World::acquireMaterial(u32 material) {
	const WorldMaterial& m = _materials[material];
	for (u32 f = 0; f < m.frameCount; ++f) acquireTexture(u32(m.texture) + f);
}

void World::releaseMaterial(u32 material) {
	const WorldMaterial& m = _materials[material];
	for (u32 f = 0; f < m.frameCount; ++f) releaseTexture(u32(m.texture) + f);
}

// Turns a chunk the loader has read, checked and built colliders for into a
// resident chunk: the only steps left are the ones that need the GPU.
LoadStatus World::finishChunk(LoadResult& r) {
	const u32 index = r.index;
	if (r.status != LOAD_OK) {
		const LoadStatus status = r.status;
		r.discard();
		return status;
	}
	ChunkHeader h;
	memcpy(&h, r.blob, sizeof(h));
	const ChunkBatch* batches = reinterpret_cast<const ChunkBatch*>(r.blob + sizeof(ChunkHeader));
	const WorldVertex* verts = reinterpret_cast<const WorldVertex*>(batches + h.batchCount);
	const u16* indices = reinterpret_cast<const u16*>(verts + h.vertexCount);

	Loaded l {};
	u32 clusterCount = 1;
	for (u32 b = 0; b < h.batchCount; ++b) clusterCount = max<u32>(clusterCount, u32(batches[b].cluster) + 1);
	l.batches = static_cast<Batch*>(malloc(size_t(h.batchCount) * sizeof(Batch) + 1));
	l.clusters = static_cast<Cluster*>(malloc(size_t(clusterCount) * sizeof(Cluster)));
	l.mesh = l.batches && l.clusters ? gfx::meshCreate(verts, h.vertexCount, indices, h.indexCount) : nullptr;
	if (!l.mesh) {
		free(l.batches);
		free(l.clusters);
		r.discard();
		return LOAD_NO_MEMORY;
	}
	for (u32 c = 0; c < clusterCount; ++c)
		for (int a = 0; a < 3; ++a) {
			l.clusters[c].bbMin[a] = 1e30f;
			l.clusters[c].bbMax[a] = -1e30f; // (a number nobody uses stays inside out: never in view)
		}
	for (u32 b = 0; b < h.batchCount; ++b) {
		const ChunkBatch& src = batches[b];
		Batch& dst = l.batches[b];
		dst.material = src.material;
		dst.detail = src.detail < DETAIL_COUNT ? src.detail : u8(DETAIL_COUNT - 1);
		dst.cluster = src.cluster;
		dst.firstIndex = src.firstIndex;
		dst.indexCount = src.indexCount;
		Cluster& cluster = l.clusters[src.cluster];
		for (int a = 0; a < 3; ++a) {
			dst.bbMin[a] = h.origin[a] + float(src.bbMin[a]) * h.posScale;
			dst.bbMax[a] = h.origin[a] + float(src.bbMax[a]) * h.posScale;
			if (!src.indexCount) continue;
			cluster.bbMin[a] = min(cluster.bbMin[a], dst.bbMin[a]);
			cluster.bbMax[a] = max(cluster.bbMax[a], dst.bbMax[a]);
		}
		l.hasBlended = l.hasBlended || (_materials[src.material].flags & MATF_TRANSLUCENT) != 0;
	}
	l.batchCount = h.batchCount;
	l.clusterCount = u16(clusterCount);
	l.triangles = h.indexCount / 3;
	memcpy(l.origin, h.origin, sizeof(l.origin));
	l.posScale = h.posScale;
	l.uvScale = h.uvScale;
	l.bytes = h.vertexCount * u32(sizeof(WorldVertex)) + h.indexCount * u32(sizeof(u16));
	l.collider = r.collider; // ownership moves to the chunk
	l.water = r.water;
	r.collider = ChunkCollider {};
	r.water = ChunkCollider {};
	l.slot = _stats.chunksLoaded;
	_resident[l.slot] = index;
	_loaded[index] = l;
	_stats.residentBytes += l.bytes;
	_stats.collisionBytes += l.collider.bytes() + l.water.bytes();
	++_stats.chunksLoaded;
	for (u32 b = 0; b < l.batchCount; ++b) acquireMaterial(l.batches[b].material);
	_stats.peakResidentBytes = max(_stats.peakResidentBytes, _stats.residentBytes);
	r.discard(); // the blob: its contents now live in the mesh
	return LOAD_OK;
}

LoadStatus World::loadChunkNow(u32 index) {
	LoadJob job;
	LoadResult r {};
	r.kind = LOAD_CHUNK;
	r.index = index;
	r.status = LOAD_BAD_DATA;
	if (chunkJob(index, job)) StreamLoader::process(*_pak, job, _materials, _hdr.materialCount, r);
	return finishChunk(r);
}

void World::unloadChunk(u32 index) {
	Loaded& l = _loaded[index];
	if (!l.mesh) return;
	for (u32 b = 0; b < l.batchCount; ++b) releaseMaterial(l.batches[b].material);
	gfx::meshDestroy(l.mesh);
	free(l.batches);
	free(l.clusters);
	_stats.collisionBytes -= l.collider.bytes() + l.water.bytes();
	l.collider.release();
	l.water.release();
	_stats.residentBytes -= l.bytes;
	// Keep the resident list dense: the last entry takes the freed slot.
	--_stats.chunksLoaded;
	const u32 last = _resident[_stats.chunksLoaded];
	_resident[l.slot] = last;
	_loaded[last].slot = l.slot;
	l = {};
	_visible = 0; // the sorted list of the last draw() may name this chunk
}

// Frees chunks that are too far away and lists the missing ones that are
// near enough in _order/_orderKey. Returns how many are missing.
u32 World::collectPending(Vec3 camera) {
	u32 pending = 0;
	for (u32 i = 0; i < _hdr.chunkCount; ++i) {
		const float d = distanceTo(i, camera);
		if (_loaded[i].mesh) {
			if (d > _cfg.unloadRadius) unloadChunk(i);
		} else if (d < _cfg.loadRadius && !_loaded[i].failed) {
			_order[pending] = i;
			_orderKey[pending] = d;
			++pending;
		}
	}
	_visible = 0; // _order is shared scratch space
	return pending;
}

// Moves the nearest of entries [n, pending) to position n.
void World::sortNearest(u32 n, u32 pending) {
	u32 best = n;
	for (u32 k = n + 1; k < pending; ++k)
		if (_orderKey[k] < _orderKey[best]) best = k;
	const u32 ti = _order[n];
	_order[n] = _order[best];
	_order[best] = ti;
	const float tf = _orderKey[n];
	_orderKey[n] = _orderKey[best];
	_orderKey[best] = tf;
}

void World::update(Vec3 camera) {
	_stats.loadsThisFrame = 0;
	if (!_pak) return;
	++_frame;
	const u32 pending = collectPending(camera);
	if (_loader.running() && !_immediate)
		updateBackground(pending, camera);
	else
		updateImmediate(pending);
	_stats.chunksInFlight = _chunksInFlight;
	_stats.texturesWaiting = _texturesWaiting;
}

// Everything on the spot: read, unpack, upload. Simple and predictable, and
// each load is a hitch as long as the chunk is big.
void World::updateImmediate(u32 pending) {
	// Nearest first; only a handful per frame so a selection scan is enough.
	u32 remaining = pending;
	const u32 loads = min(pending, _cfg.maxLoadsPerFrame);
	for (u32 n = 0; n < loads; ++n) {
		sortNearest(n, pending);
		const u32 index = _order[n];
		if (_stats.residentBytes + meshBytes(_chunks[index]) > _cfg.memoryBudget) {
			++_stats.budgetRejects;
			break; // everything else is farther away; try again next frame
		}
		const LoadStatus status = loadChunkNow(index);
		if (status == LOAD_NO_MEMORY) {
			// The budget said yes, the allocator said no. Same remedy: wait
			// until something has been unloaded.
			++_stats.budgetRejects;
			break;
		}
		if (status == LOAD_OK) {
			++_stats.loadsThisFrame;
		} else {
			_loaded[index].failed = true;
			++_stats.loadFailures;
		}
		--remaining;
	}
	_stats.chunksPending = remaining;

	// Textures that did not fit when their chunk arrived.
	if (_texturesWaiting) {
		for (u32 i = 0; i < _hdr.textureCount; ++i) {
			if (!_tex[i].waiting) continue;
			loadTextureNow(i);
			break;
		}
	}
}

// The loader reads and prepares; this only hands finished work to the GPU, a
// little each frame, and keeps the loader supplied with the nearest wishes.
void World::updateBackground(u32 pending, Vec3 camera) {
	// 1. Finished work, within this frame's allowance.
	u32 chunksDone = 0, texturesDone = 0;
	LoadKind kind;
	while (_loader.peek(kind)) {
		if (kind == LOAD_CHUNK ? chunksDone >= _cfg.maxLoadsPerFrame : texturesDone >= _cfg.maxTextureLoadsPerFrame) break;
		LoadResult r;
		if (!_loader.poll(r)) break;
		if (r.kind == LOAD_TEXTURE) {
			++texturesDone;
			finishTexture(r);
			continue;
		}
		++chunksDone;
		Loaded& l = _loaded[r.index];
		l.requested = false;
		--_chunksInFlight;
		_inFlightBytes -= meshBytes(_chunks[r.index]);
		if (r.status == LOAD_OK && distanceTo(r.index, camera) > _cfg.unloadRadius) {
			r.discard(); // the camera has moved on in the meantime
			continue;
		}
		const LoadStatus status = finishChunk(r);
		if (status == LOAD_OK) {
			++_stats.loadsThisFrame;
		} else if (status == LOAD_BAD_DATA) {
			_loaded[r.index].failed = true;
			++_stats.loadFailures;
		} else {
			// Out of memory: leave it alone for a second instead of reading it
			// from the card again every frame.
			++_stats.budgetRejects;
			_loaded[r.index].retryAt = _frame + 60;
		}
	}

	// 2. New wishes, nearest first. Only a couple at a time, so that what is
	// being read is still what is wanted when it arrives.
	u32 missing = 0;
	for (u32 k = 0; k < pending; ++k) missing += !_loaded[_order[k]].mesh;
	_stats.chunksPending = missing;
	const u32 maxInFlight = 2;
	for (u32 n = 0; n < pending && _chunksInFlight < maxInFlight; ++n) {
		sortNearest(n, pending);
		const u32 index = _order[n];
		Loaded& l = _loaded[index];
		if (l.mesh || l.requested || l.failed || _frame < l.retryAt) continue;
		const u32 need = meshBytes(_chunks[index]);
		if (_stats.residentBytes + _inFlightBytes + need > _cfg.memoryBudget) {
			++_stats.budgetRejects;
			break; // everything else is farther away
		}
		LoadJob job;
		if (!chunkJob(index, job)) {
			l.failed = true;
			++_stats.loadFailures;
			continue;
		}
		if (!_loader.submit(job)) break;
		l.requested = true;
		++_chunksInFlight;
		_inFlightBytes += need;
	}

	// 3. Textures of the chunks that have arrived.
	if (_texturesWaiting && _frame >= _textureRetryAt) {
		for (u32 i = 0; i < _hdr.textureCount && _loader.outstanding() < StreamLoader::CAPACITY; ++i) {
			TexSlot& s = _tex[i];
			if (!s.waiting || s.requested) continue;
			LoadJob job;
			if (!textureJob(i, job)) {
				s.failed = true;
				++_stats.loadFailures;
				setWaiting(s, false);
				continue;
			}
			if (!_loader.submit(job)) break;
			s.requested = true;
		}
	}
}

void World::preload(Vec3 camera) {
	if (!_pak) return;
	// Whatever the loader is busy with is about to be loaded right here.
	if (_loader.running()) {
		_loader.cancelAll();
		for (u32 i = 0; i < _hdr.chunkCount; ++i) _loaded[i].requested = false;
		for (u32 i = 0; i < _hdr.textureCount; ++i) _tex[i].requested = false;
		_chunksInFlight = 0;
		_inFlightBytes = 0;
	}
	const u32 saved = _cfg.maxLoadsPerFrame;
	_cfg.maxLoadsPerFrame = 0xFFFFFFFFu;
	_immediate = true;
	update(camera);
	for (u32 i = 0; i < _hdr.textureCount && _texturesWaiting; ++i)
		if (_tex[i].waiting) loadTextureNow(i);
	_immediate = false;
	_cfg.maxLoadsPerFrame = saved;
	_stats.texturesWaiting = _texturesWaiting;
}

bool World::rayDown(Vec3 from, float maxDrop, float& hitY, Vec3& normal, u8* light) const {
	bool found = false;
	float best = from.y - maxDrop;
	for (u32 k = 0; k < _stats.chunksLoaded; ++k) {
		const u32 i = _resident[k];
		const ChunkInfo& ci = _chunks[i];
		if (from.x < ci.bbMin[0] || from.x > ci.bbMax[0] || from.z < ci.bbMin[2] || from.z > ci.bbMax[2]) continue;
		if (from.y < ci.bbMin[1] || best > ci.bbMax[1]) continue;
		float y;
		Vec3 n;
		u8 l[4] = {255, 255, 255, 255};
		// Each chunk only needs to beat the best hit so far.
		if (_loaded[i].collider.rayDown(from, from.y - best, y, n, light ? l : nullptr) && y >= best) {
			best = y;
			hitY = y;
			normal = n;
			if (light) memcpy(light, l, 4);
			found = true;
		}
	}
	return found;
}

bool World::waterBelow(Vec3 from, float maxDrop, float& surfaceY) const {
	bool found = false;
	float best = from.y - maxDrop;
	for (u32 k = 0; k < _stats.chunksLoaded; ++k) {
		const u32 i = _resident[k];
		if (!_loaded[i].water.triangleCount) continue;
		const ChunkInfo& ci = _chunks[i];
		if (from.x < ci.bbMin[0] || from.x > ci.bbMax[0] || from.z < ci.bbMin[2] || from.z > ci.bbMax[2]) continue;
		float y;
		Vec3 n;
		if (_loaded[i].water.rayDown(from, from.y - best, y, n) && y >= best) {
			best = y;
			surfaceY = y;
			found = true;
		}
	}
	return found;
}

bool World::pushSphere(Vec3& center, float radius) const {
	bool moved = false;
	for (u32 k = 0; k < _stats.chunksLoaded; ++k) {
		const u32 i = _resident[k];
		const ChunkInfo& ci = _chunks[i];
		if (center.x + radius < ci.bbMin[0] || center.x - radius > ci.bbMax[0] || center.y + radius < ci.bbMin[1] ||
		    center.y - radius > ci.bbMax[1] || center.z + radius < ci.bbMin[2] || center.z - radius > ci.bbMax[2])
			continue;
		moved = _loaded[i].collider.pushSphere(center, radius) || moved;
	}
	return moved;
}

bool World::boxFree(const float mn[3], const float mx[3]) const {
	for (u32 k = 0; k < _stats.chunksLoaded; ++k) {
		const u32 i = _resident[k];
		const ChunkInfo& ci = _chunks[i];
		if (mx[0] < ci.bbMin[0] || mn[0] > ci.bbMax[0] || mx[1] < ci.bbMin[1] || mn[1] > ci.bbMax[1] || mx[2] < ci.bbMin[2] ||
		    mn[2] > ci.bbMax[2])
			continue;
		if (!_loaded[i].collider.boxFree(mn, mx)) return false;
	}
	return true;
}

const WorldPortal* World::portalAt(Vec3 p) const {
	for (u32 i = 0; i < _hdr.portalCount; ++i) {
		const WorldPortal& q = _portals[i];
		if (p.x >= q.bbMin[0] && p.x <= q.bbMax[0] && p.y >= q.bbMin[1] && p.y <= q.bbMax[1] && p.z >= q.bbMin[2] &&
		    p.z <= q.bbMax[2])
			return &q;
	}
	return nullptr;
}

const WorldLadder* World::ladderAt(Vec3 p) const {
	for (u32 i = 0; i < _hdr.ladderCount; ++i) {
		const WorldLadder& q = _ladders[i];
		if (p.x >= q.bbMin[0] && p.x <= q.bbMax[0] && p.y >= q.bbMin[1] && p.y <= q.bbMax[1] && p.z >= q.bbMin[2] &&
		    p.z <= q.bbMax[2])
			return &q;
	}
	return nullptr;
}

// How far the farthest point of a box is from `c`.
static float aabbFarDistance(const float mn[3], const float mx[3], Vec3 c) {
	const float dx = max(c.x - mn[0], mx[0] - c.x), dy = max(c.y - mn[1], mx[1] - c.y), dz = max(c.z - mn[2], mx[2] - c.z);
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

// Looks at the boxes of a chunk once; drawBatches() then knows of most
// batches what to do without looking at them.
void World::classify(const Loaded& l, bool count) {
	for (u32 c = 0; c < l.clusterCount; ++c) {
		const Cluster& k = l.clusters[c];
		if (k.bbMin[0] > k.bbMax[0]) {
			_clusterState[c] = CLUSTER_OUT; // nothing in it
			continue;
		}
		const int where = _cfg.clusterCulling ? aabbFrustumClass(_frustum, k.bbMin, k.bbMax) : 1;
		_clusterState[c] = where == 0 ? u8(CLUSTER_OUT) : where == 1 ? u8(CLUSTER_IN) : u8(CLUSTER_PARTLY);
		_stats.clustersTested += count;
		_stats.clustersCulled += count && where == 0;
		if (where == 0) continue;
		_clusterNear[c] = aabbDistance(k.bbMin, k.bbMax, _camera);
		_clusterFar[c] = aabbFarDistance(k.bbMin, k.bbMax, _camera);
	}
}

void World::drawBatches(const Loaded& l, bool blended) {
	const u32 ms = _timeMs;
	bool transformSet = false;
	// A culled stretch this short between two runs of a material is drawn
	// along with them: a draw call costs more than its few triangles.
	const u32 bridge = 24 * 3;
	u32 runFirst = 0, runCount = 0, gap = 0, runMaterial = 0;
	auto flush = [&]() {
		gap = 0;
		if (!runCount) return;
		if (!transformSet) {
			gfx::setChunkTransform(l.origin, l.posScale, l.uvScale);
			transformSet = true;
		}
		const WorldMaterial& m = _materials[runMaterial];
		const u32 frame = (m.frameCount > 1 && m.frameMs) ? (ms / m.frameMs) % m.frameCount : 0;
		float offset[2] = {0, 0};
		for (int a = 0; a < 2; ++a) {
			if (!m.scroll[a]) continue;
			// Whole repeats are dropped in integers, so the scroll stays smooth
			// however long the game has been running.
			const u32 period = 4096u * 1000u; // scroll units * ms per repeat
			const u32 phase = u32((u64(ms) * u32(m.scroll[a] < 0 ? -m.scroll[a] : m.scroll[a])) % period);
			const float f = float(phase) / float(period);
			offset[a] = m.scroll[a] < 0 ? 1.0f - f : f;
		}
		gfx::setMaterial(_tex[u32(m.texture) + frame].tex, m.flags, float(m.opacity) * (1.0f / 255.0f), offset[0], offset[1]);
		gfx::draw(l.mesh, runFirst, runCount);
		++_stats.drawCalls;
		_stats.blendedDrawCalls += blended;
		_stats.trianglesDrawn += runCount / 3;
		runCount = 0;
	};
	for (u32 b = 0; b < l.batchCount; ++b) {
		const Batch& batch = l.batches[b];
		const WorldMaterial& m = _materials[batch.material];
		if (((m.flags & MATF_TRANSLUCENT) != 0) != blended || batch.indexCount == 0) continue;
		const u32 c = batch.cluster < l.clusterCount ? batch.cluster : 0u;
		bool visible = _clusterState[c] != CLUSTER_OUT;
		if (visible && batch.detail != DETAIL_ALWAYS) {
			// Too far for its size? Mostly the box's distance says so already.
			const float limit = _cfg.detailDistance[batch.detail] * _cfg.detailScale;
			if (_clusterNear[c] > limit)
				visible = false;
			else if (_clusterFar[c] > limit)
				visible = aabbDistance(batch.bbMin, batch.bbMax, _camera) <= limit;
		}
		if (visible && _clusterState[c] == CLUSTER_PARTLY) visible = aabbInFrustum(_frustum, batch.bbMin, batch.bbMax);
		const bool follows = runCount && batch.material == runMaterial && batch.firstIndex == runFirst + runCount + gap;
		if (!visible) {
			++_stats.batchesCulled;
			if (follows && gap + batch.indexCount <= bridge)
				gap += batch.indexCount;
			else
				flush();
			continue;
		}
		if (follows) {
			runCount += gap + batch.indexCount;
			gap = 0;
			++_stats.batchesMerged;
			continue;
		}
		flush();
		runFirst = batch.firstIndex;
		runCount = batch.indexCount;
		runMaterial = batch.material;
	}
	flush();
}

void World::draw(const Mat4& viewProj, Vec3 camera, u32 timeMs) {
	_stats.chunksDrawn = 0;
	_stats.drawCalls = 0;
	_stats.blendedDrawCalls = 0;
	_stats.batchesCulled = 0;
	_stats.batchesMerged = 0;
	_stats.trianglesDrawn = 0;
	_stats.trianglesInChunks = 0;
	_stats.clustersTested = 0;
	_stats.clustersCulled = 0;
	_visible = 0;
	if (!_pak) return;

	_frustum = frustumFromViewProj(viewProj);
	// The far plane is where the fog ends (the draw distance): what lies
	// wholly behind it has the fog's colour all over and need not be drawn.
	// (The projection's own far plane is further out, to leave room for the
	// boxes that straddle this one.)
	{
		float* nearPlane = _frustum.p[4];
		const float len = sqrtf(nearPlane[0] * nearPlane[0] + nearPlane[1] * nearPlane[1] + nearPlane[2] * nearPlane[2]);
		if (len > 0.0f && _cfg.clusterCulling) {
			const float ahead[3] = {nearPlane[0] / len, nearPlane[1] / len, nearPlane[2] / len};
			float* farPlane = _frustum.p[5];
			farPlane[0] = -ahead[0];
			farPlane[1] = -ahead[1];
			farPlane[2] = -ahead[2];
			farPlane[3] = ahead[0] * camera.x + ahead[1] * camera.y + ahead[2] * camera.z + _cfg.drawDistance;
		}
	}
	_camera = camera;
	_timeMs = timeMs;
	u32 n = 0;
	for (u32 r = 0; r < _stats.chunksLoaded; ++r) {
		const u32 i = _resident[r];
		const float d = distanceTo(i, camera);
		if (d > _cfg.drawDistance || !aabbInFrustum(_frustum, _chunks[i].bbMin, _chunks[i].bbMax)) continue;
		// Insertion sort, front to back, so the depth test rejects hidden pixels early.
		u32 k = n++;
		while (k > 0 && _orderKey[k - 1] > d) {
			_order[k] = _order[k - 1];
			_orderKey[k] = _orderKey[k - 1];
			--k;
		}
		_order[k] = i;
		_orderKey[k] = d;
	}
	_visible = n;

	for (u32 k = 0; k < n; ++k) {
		const Loaded& l = _loaded[_order[k]];
		classify(l, true);
		drawBatches(l, false);
		++_stats.chunksDrawn;
		_stats.trianglesInChunks += l.triangles;
	}
}

void World::drawBlended() {
	// Back to front, so nearer water blends over farther water.
	for (u32 k = _visible; k-- > 0;) {
		const Loaded& l = _loaded[_order[k]];
		if (!l.mesh || !l.hasBlended) continue;
		classify(l, false);
		drawBatches(l, true);
	}
}

} // namespace g3
