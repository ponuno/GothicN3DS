#include "world.h"
#include "triggers.h"

#include "readahead.h"

#include <stdlib.h>
#include <string.h>

namespace g3 {

// Graphics+: how many waves of the wind go by a plant in a second (G2).
static const float kWindWavesPerSecond = 0.35f;

void World::setReadAhead(ReadAhead* ahead) {
	_ahead = ahead;
	_loader.setReadAhead(ahead);
}

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
	const u64 tierBytes = (_hdr.flags & WORLDF_TIERS) ? u64(_hdr.chunkCount) : 0u;
	bool ok = _hdr.magic == WORLD_MAGIC && _hdr.version == WORLD_VERSION &&
	          u64(e->size) == sizeof(WorldHeader) + matBytes + texBytes + portalBytes + ladderBytes + chunkBytes + tierBytes;
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
		_tier = tierBytes ? static_cast<u8*>(malloc(size_t(tierBytes))) : nullptr;
		ok = _materials && _textures && _portals && _ladders && _chunks && _loaded && _tex && _resident && _order &&
		     _orderKey && (_tier || !tierBytes);
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
		p += chunkBytes;
		if (_tier) {
			memcpy(_tier, p, size_t(tierBytes));
			for (u32 i = 0; i < _hdr.chunkCount; ++i)
				if (_tier[i] >= DETAIL_COUNT) _tier[i] = u8(DETAIL_COUNT - 1);
		}
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
	_candValid = false;
	_candCount = 0;
	_headStrength = 0.0f;
	_anchorFrame = 0xFFFFFFFFu;
	_lastRejected = 0xFFFFFFFFu;
	buildGrid();
	// Failing to start the loader is not fatal: update() then loads on the spot.
	if (_cfg.background) _loader.start(pak->path(), _materials, _hdr.materialCount, _cfg.worker);
	return true;
}

void World::close() {
	_loader.stop(); // before the tables it reads go away
	freeFar();
	_farTried = false;
	freeVis();
	_visTried = false;
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
	free(_tier);
	freeGrid();
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
	_tier = nullptr;
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

bool World::pointShown(Vec3 p, Vec3 eye, int& chunk, bool wasOn) const {
	if (chunk == -2) {
		chunk = -1;
		for (u32 i = 0; i < _hdr.chunkCount && chunk < 0; ++i) {
			const ChunkInfo& c = _chunks[i];
			if (p.x >= c.bbMin[0] - 50.0f && p.x <= c.bbMax[0] + 50.0f && p.y >= c.bbMin[1] - 50.0f && p.y <= c.bbMax[1] + 50.0f &&
			    p.z >= c.bbMin[2] - 50.0f && p.z <= c.bbMax[2] + 50.0f)
				chunk = int(i);
		}
	}
	if (chunk >= 0 && (u32(chunk) >= _hdr.chunkCount || !_loaded[chunk].mesh)) return false;
	const float dx = p.x - eye.x, dy = p.y - eye.y, dz = p.z - eye.z;
	const float reach = min(_cfg.drawDistance, _cfg.detailDistance[DETAIL_SMALL] * _cfg.detailScale) * (wasOn ? _cfg.detailHysteresis : 1.0f);
	return dx * dx + dy * dy + dz * dz <= reach * reach;
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
	for (u32 c = 0; c < clusterCount; ++c) {
		l.clusters[c].lod = false;
		l.clusters[c].lodMix = 0;
		l.clusters[c].mixFrame = 0;
		for (int a = 0; a < 3; ++a) {
			l.clusters[c].bbMin[a] = 1e30f;
			l.clusters[c].bbMax[a] = -1e30f; // (a number nobody uses stays inside out: never in view)
		}
	}
	for (u32 b = 0; b < h.batchCount; ++b) {
		const ChunkBatch& src = batches[b];
		Batch& dst = l.batches[b];
		dst.material = src.material;
		dst.detail = (src.detail & 3) < DETAIL_COUNT ? u8(src.detail & 3) : u8(DETAIL_COUNT - 1);
		dst.layer = u8((src.detail >> 2) & 3);
		dst.band = dst.detail == DETAIL_SMALL ? batchSizeBand(src.detail) : u8(0);
		dst.cluster = src.cluster;
		dst.lodMode = batchLod(src.detail);
		l.hasLod = l.hasLod || dst.lodMode == BATCH_LOD_FAR_ONLY;
		dst.shown = 0;
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
	if (chunkTier(index)) {
		++_stats.loadedDetail;
		_stats.detailBytes += l.bytes;
	}
	if (_lastRejected == index) _lastRejected = 0xFFFFFFFFu;
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
	if (chunkTier(index)) {
		--_stats.loadedDetail;
		_stats.detailBytes -= l.bytes;
	}
	// Keep the resident list dense: the last entry takes the freed slot.
	--_stats.chunksLoaded;
	const u32 last = _resident[_stats.chunksLoaded];
	_resident[l.slot] = last;
	_loaded[last].slot = l.slot;
	l = {};
	_visible = 0; // the sorted list of the last draw() may name this chunk
}

float World::loadReach(u32 tier) const {
	if (tier == 0 || tier >= DETAIL_COUNT) return _cfg.loadRadius;
	return min(_cfg.detailDistance[tier] * _cfg.detailHysteresis + _cfg.detailLead, _cfg.loadRadius);
}

float World::unloadReach(u32 tier) const {
	if (tier == 0 || tier >= DETAIL_COUNT) return _cfg.unloadRadius;
	return min(loadReach(tier) + _cfg.detailSlack, _cfg.unloadRadius);
}

// How much farther a chunk ahead of the way the camera goes is wanted: the world arrives before the camera does.
// Never so far that the chunk would be freed again at once (the unload distance is farther than the load distance).
float World::headLeadFor(u32 index, Vec3 camera) const {
	if (_cfg.headLead <= 0.0f || _headStrength <= 0.0f) return 0.0f;
	const ChunkInfo& ci = _chunks[index];
	const float vx = (ci.bbMin[0] + ci.bbMax[0]) * 0.5f - camera.x, vz = (ci.bbMin[2] + ci.bbMax[2]) * 0.5f - camera.z;
	const float len = sqrtf(vx * vx + vz * vz);
	if (len < 100.0f) return 0.0f;
	const float dot = (vx * _headX + vz * _headZ) / len;
	if (dot <= 0.0f) return 0.0f;
	const u32 tier = chunkTier(index);
	const float room = unloadReach(tier) - loadReach(tier) - 200.0f;
	return room > 0.0f ? min(_cfg.headLead * dot * _headStrength, room) : 0.0f;
}

bool World::wanted(const ChunkInfo& ci, const Vec3* list, u32 count) const {
	for (u32 i = 0; i < count; ++i)
		if (list[i].x > ci.bbMin[0] - 150.0f && list[i].x < ci.bbMax[0] + 150.0f && list[i].z > ci.bbMin[2] - 150.0f &&
		    list[i].z < ci.bbMax[2] + 150.0f && list[i].y > ci.bbMin[1] - 300.0f && list[i].y < ci.bbMax[1] + 300.0f)
			return true;
	return false;
}

void World::wantAt(Vec3 p) const {
	if (_wantNowCount < 16) _wantNow[_wantNowCount++] = p;
}

// False while a chunk around `p` is still to come (it was listed as missing by the last update()).
bool World::settledAt(Vec3 p) const {
	for (u32 k = 0; k < _missingCount; ++k)
		if (!_loaded[_missing[k]].mesh && wanted(_chunks[_missing[k]], &p, 1)) return false;
	return true;
}

float World::priority(u32 index, float d, Vec3 camera) const {
	const u32 tier = chunkTier(index);
	float key = d + _cfg.tierBias[tier < DETAIL_COUNT ? tier : 0u];
	if (_wantPrevCount && wanted(_chunks[index], _wantPrev, _wantPrevCount)) key -= 4000.0f; // (before the rest, buildings aside)
	if (_headStrength > 0.0f) {
		const ChunkInfo& ci = _chunks[index];
		const float vx = (ci.bbMin[0] + ci.bbMax[0]) * 0.5f - camera.x, vz = (ci.bbMin[2] + ci.bbMax[2]) * 0.5f - camera.z;
		const float len = sqrtf(vx * vx + vz * vz);
		if (len > 100.0f) {
			const float dot = (vx * _headX + vz * _headZ) / len;
			key += dot > 0.0f ? -_cfg.aheadBonus * dot * _headStrength : -_cfg.behindPenalty * dot * _headStrength;
		}
	}
	return key;
}

// Which way the camera has been going, from where it was a few frames ago. Standing still lets the
// certainty fade out over about three seconds; a jump (a teleport) forgets it.
void World::trackHeading(Vec3 camera) {
	if (_anchorFrame == 0xFFFFFFFFu) {
		_anchor = camera; // (the first look: nowhere to have come from)
		_anchorFrame = _frame;
		return;
	}
	if (_frame - _anchorFrame < 10) return;
	const float dx = camera.x - _anchor.x, dz = camera.z - _anchor.z;
	const float dist = sqrtf(dx * dx + dz * dz);
	if (dist > 3000.0f) {
		_headStrength = 0.0f;
	} else if (dist > 40.0f) {
		float nx = dx / dist, nz = dz / dist;
		if (_headStrength > 0.0f) {
			nx = _headX * 0.5f + nx * 0.5f;
			nz = _headZ * 0.5f + nz * 0.5f;
			const float l = sqrtf(nx * nx + nz * nz);
			if (l > 0.01f) {
				nx /= l;
				nz /= l;
			} else {
				nx = dx / dist;
				nz = dz / dist;
			}
		}
		_headX = nx;
		_headZ = nz;
		_headStrength = 1.0f;
	} else {
		_headStrength = max(0.0f, _headStrength - 0.1f);
	}
	_anchor = camera;
	_anchorFrame = _frame;
}

static const float kCandidateMargin = 500.0f; // cm the camera may move before the candidate list is made anew

void World::freeGrid() {
	free(_gridStart);
	free(_gridItems);
	free(_big);
	free(_seen);
	free(_cand);
	_gridStart = _gridItems = _big = _seen = _cand = nullptr;
	_gridW = _gridH = 0;
	_bigCount = _candCount = _seenStamp = 0;
	_candValid = false;
}

void World::buildGrid() {
	freeGrid();
	const u32 n = _hdr.chunkCount;
	_seen = static_cast<u32*>(calloc(size_t(n) + 1, sizeof(u32)));
	_cand = static_cast<u32*>(malloc((size_t(n) + 1) * sizeof(u32)));
	_big = static_cast<u32*>(malloc((size_t(n) + 1) * sizeof(u32)));
	if (!_seen || !_cand || !_big || n == 0) {
		freeGrid(); // (without a grid every chunk is looked at, as before)
		_cand = static_cast<u32*>(malloc((size_t(n) + 1) * sizeof(u32)));
		_seen = static_cast<u32*>(calloc(size_t(n) + 1, sizeof(u32)));
		return;
	}
	float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
	for (u32 i = 0; i < n; ++i) {
		x0 = min(x0, _chunks[i].bbMin[0]);
		z0 = min(z0, _chunks[i].bbMin[2]);
		x1 = max(x1, _chunks[i].bbMax[0]);
		z1 = max(z1, _chunks[i].bbMax[2]);
	}
	float cell = max(_hdr.chunkSize, 500.0f);
	if (!(x1 >= x0) || !(z1 >= z0) || !(x1 - x0 < 1e8f) || !(z1 - z0 < 1e8f)) return;
	while (double((x1 - x0) / cell + 1.0) * double((z1 - z0) / cell + 1.0) > double(1 << 18)) cell *= 2.0f;
	_gridW = int((x1 - x0) / cell) + 1;
	_gridH = int((z1 - z0) / cell) + 1;
	_gridX0 = x0;
	_gridZ0 = z0;
	_gridCell = cell;
	const size_t cells = size_t(_gridW) * size_t(_gridH);
	_gridStart = static_cast<u32*>(calloc(cells + 1, sizeof(u32)));
	if (!_gridStart) {
		freeGrid();
		_cand = static_cast<u32*>(malloc((size_t(n) + 1) * sizeof(u32)));
		_seen = static_cast<u32*>(calloc(size_t(n) + 1, sizeof(u32)));
		return;
	}
	auto span = [&](u32 i, int& cx0, int& cx1, int& cz0, int& cz1) {
		cx0 = clamp(int((_chunks[i].bbMin[0] - x0) / cell), 0, _gridW - 1);
		cx1 = clamp(int((_chunks[i].bbMax[0] - x0) / cell), 0, _gridW - 1);
		cz0 = clamp(int((_chunks[i].bbMin[2] - z0) / cell), 0, _gridH - 1);
		cz1 = clamp(int((_chunks[i].bbMax[2] - z0) / cell), 0, _gridH - 1);
		return (cx1 - cx0 + 1) * (cz1 - cz0 + 1);
	};
	// Two passes: count the entries of each cell, then fill them.
	u64 total = 0;
	for (u32 i = 0; i < n; ++i) {
		int a, b, c, d;
		if (span(i, a, b, c, d) > 64) {
			_big[_bigCount++] = i;
			continue;
		}
		for (int z = c; z <= d; ++z)
			for (int x = a; x <= b; ++x) {
				++_gridStart[size_t(z) * size_t(_gridW) + size_t(x) + 1];
				++total;
			}
	}
	for (size_t k = 0; k < cells; ++k) _gridStart[k + 1] += _gridStart[k];
	_gridItems = static_cast<u32*>(malloc(size_t(total) * sizeof(u32) + 1));
	u32* fill = static_cast<u32*>(malloc(cells * sizeof(u32) + 1));
	if (!_gridItems || !fill) {
		free(fill);
		freeGrid();
		_cand = static_cast<u32*>(malloc((size_t(n) + 1) * sizeof(u32)));
		_seen = static_cast<u32*>(calloc(size_t(n) + 1, sizeof(u32)));
		return;
	}
	memcpy(fill, _gridStart, cells * sizeof(u32));
	for (u32 i = 0; i < n; ++i) {
		int a, b, c, d;
		if (span(i, a, b, c, d) > 64) continue;
		for (int z = c; z <= d; ++z)
			for (int x = a; x <= b; ++x) _gridItems[fill[size_t(z) * size_t(_gridW) + size_t(x)]++] = i;
	}
	free(fill);
}

// Lists the chunks that may be wanted before the camera has moved `kCandidateMargin`: those within
// their reach of it, plus that margin. Made anew only when the camera has moved farther than that,
// or a reach has changed; in between, collectPending() looks at this short list only.
void World::gatherCandidates(Vec3 camera) {
	float reach[DETAIL_COUNT];
	bool same = _candValid;
	for (u32 t = 0; t < DETAIL_COUNT; ++t) {
		reach[t] = loadReach(t);
		same = same && reach[t] == _candReach[t];
	}
	if (same) {
		const Vec3 m = camera - _candCam;
		if (m.x * m.x + m.y * m.y + m.z * m.z <= kCandidateMargin * kCandidateMargin) return;
	}
	if (!_cand || !_seen) return;
	_candValid = true;
	_candCam = camera;
	_candCount = 0;
	float widest = 0.0f;
	for (u32 t = 0; t < DETAIL_COUNT; ++t) {
		_candReach[t] = reach[t];
		widest = max(widest, reach[t]);
	}
	if (++_seenStamp == 0) {
		memset(_seen, 0, (size_t(_hdr.chunkCount) + 1) * sizeof(u32));
		_seenStamp = 1;
	}
	auto consider = [&](u32 i) {
		if (_seen[i] == _seenStamp) return;
		_seen[i] = _seenStamp;
		if (distanceTo(i, camera) < reach[chunkTier(i)] + kCandidateMargin * 2.0f + _cfg.headLead) _cand[_candCount++] = i;
	};
	if (_gridStart) {
		const float r = widest + kCandidateMargin * 2.0f + _cfg.headLead;
		const int cx0 = clamp(int(floorf((camera.x - r - _gridX0) / _gridCell)), 0, _gridW - 1);
		const int cx1 = clamp(int(floorf((camera.x + r - _gridX0) / _gridCell)), 0, _gridW - 1);
		const int cz0 = clamp(int(floorf((camera.z - r - _gridZ0) / _gridCell)), 0, _gridH - 1);
		const int cz1 = clamp(int(floorf((camera.z + r - _gridZ0) / _gridCell)), 0, _gridH - 1);
		for (int z = cz0; z <= cz1; ++z)
			for (int x = cx0; x <= cx1; ++x) {
				const size_t cell = size_t(z) * size_t(_gridW) + size_t(x);
				for (u32 k = _gridStart[cell]; k < _gridStart[cell + 1]; ++k) consider(_gridItems[k]);
			}
		for (u32 k = 0; k < _bigCount; ++k) consider(_big[k]);
	} else {
		for (u32 i = 0; i < _hdr.chunkCount; ++i) consider(i);
	}
}

// Frees chunks that are too far away and lists the missing ones that are
// near enough in _order/_orderKey (by priority). Returns how many are missing.
u32 World::collectPending(Vec3 camera) {
	trackHeading(camera);
	// (Downwards: freeing a chunk moves the last one into its place, which has been looked at.)
	for (u32 k = _stats.chunksLoaded; k-- > 0;) {
		const u32 i = _resident[k];
		if (distanceTo(i, camera) > unloadReach(chunkTier(i))) unloadChunk(i);
	}
	gatherCandidates(camera);
	u32 pending = 0;
	for (u32 c = 0; c < _candCount; ++c) {
		const u32 i = _cand[c];
		if (_loaded[i].mesh || _loaded[i].failed) continue;
		const float d = distanceTo(i, camera);
		if (d >= loadReach(chunkTier(i)) + headLeadFor(i, camera)) continue;
		_order[pending] = i;
		_orderKey[pending] = priority(i, d, camera);
		++pending;
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
	for (u32 i = 0; i < _wantNowCount; ++i) _wantPrev[i] = _wantNow[i];
	_wantPrevCount = _wantNowCount;
	_wantNowCount = 0;
	const u32 pending = collectPending(camera);
	if (_loader.running() && !_immediate)
		updateBackground(pending, camera);
	else
		updateImmediate(pending, camera);
	{
		u32 missing = 0, missingDetail = 0;
		_missingCount = 0;
		for (u32 k = 0; k < pending; ++k) {
			const u32 i = _order[k];
			if (_loaded[i].mesh) continue;
			if (!_loaded[i].failed && _missingCount < 256) _missing[_missingCount++] = i;
			++missing;
			missingDetail += chunkTier(i) != 0;
		}
		_stats.chunksPending = missing;
		_stats.pendingDetail = missingDetail;
	}
	_stats.chunksInFlight = _chunksInFlight;
	_stats.texturesWaiting = _texturesWaiting;
	_stats.budgetBytes = budget();
	// The far layer: in memory while it is asked for.
	const bool wantFar = hasFarLayer() && _cfg.farDistance > _cfg.drawDistance;
	if (wantFar && !_farTiles && !_farTried) {
		loadFar();
	} else if (!wantFar) {
		if (_farTiles) freeFar();
		_farTried = false;
	}
	_stats.farBytes = _farBytes;
	// The visibility table: in memory while it is asked for.
	if (_cfg.visibility && hasVisibility() && !_visBlob && !_visTried) {
		loadVis();
	} else if (!_cfg.visibility) {
		if (_visBlob) freeVis();
		_visTried = false;
	}
	_stats.visBytes = _visBlob ? visBlobSize(_vis) : 0u;
}

void World::freeVis() {
	free(_visBlob);
	free(_visBase);
	free(_visAllHidden);
	free(_visViewersOwned);
	_visViewersOwned = nullptr;
	_visFirst = _visCount = 0;
	_visPick = -1;
	_visBlob = nullptr;
	_visBase = nullptr;
	_visAllHidden = nullptr;
	_visCellX = _visCellZ = nullptr;
	_visClusters = nullptr;
	_visOrder = nullptr;
	_visViewers = nullptr;
	_visBits = nullptr;
	_visRow = _visHave = false;
	_vis = {};
	_stats.visBytes = 0;
}

void World::loadVis() {
	_visTried = true;
	const PakEntry* e = _pak ? _pak->find(_hdr.visEntry) : nullptr;
	if (!e || e->size < sizeof(VisHeader)) return;
	u8* blob = static_cast<u8*>(_pak->readAlloc(*e));
	if (!blob) return;
	VisHeader h;
	memcpy(&h, blob, sizeof(h));
	bool ok = h.magic == VIS_MAGIC && (h.version == VIS_VERSION || h.version == VIS_VERSION_FLOORS || h.version == VIS_VERSION_OLD) && h.chunkCount == _hdr.chunkCount && h.viewerCount > 0 &&
	          h.viewerCount < (1u << 24) && h.chunkCount < (1u << 24) && h.bitCount < 0xFFFFFFE0u && h.cellSize > 0.0f &&
	          h.radius > 0.0f && h.radius < 1.0e7f && u64(e->size) == u64(visBlobSize(h));
	if (ok) {
		const u8* p = blob + sizeof(VisHeader);
		_visCellX = reinterpret_cast<const i16*>(p);
		p += size_t(h.chunkCount) * 2;
		_visCellZ = reinterpret_cast<const i16*>(p);
		p += size_t(h.chunkCount) * 2;
		_visClusters = p;
		p += size_t(h.chunkCount) + visPad(h.chunkCount);
		_visOrder = reinterpret_cast<const u32*>(p);
		p += size_t(h.chunkCount) * 4;
		_visRooms = h.version >= VIS_VERSION;
		if (h.version == VIS_VERSION_OLD || h.version == VIS_VERSION_FLOORS) {
			// Older tables: the same rows as of the outside (room 0); version 1 with a range that holds every height.
			const bool one = h.version == VIS_VERSION_OLD;
			const u8* rowsAt = p;
			p += size_t(h.viewerCount) * (one ? sizeof(VisViewer) : sizeof(VisViewer2));
			_visViewersOwned = static_cast<VisViewer3*>(malloc(size_t(h.viewerCount) * sizeof(VisViewer3)));
			ok = _visViewersOwned != nullptr;
			for (u32 i = 0; ok && i < h.viewerCount; ++i) {
				if (one) {
					const VisViewer o = reinterpret_cast<const VisViewer*>(rowsAt)[i];
					_visViewersOwned[i] = VisViewer3 {o.x, o.z, o.bitOffset, -1.0e30f, 1.0e30f, 0u};
				} else {
					VisViewer2 o;
					memcpy(&o, rowsAt + size_t(i) * sizeof(VisViewer2), sizeof(o));
					_visViewersOwned[i] = VisViewer3 {o.x, o.z, o.bitOffset, o.yMin, o.yMax, 0u};
				}
			}
			_visViewers = _visViewersOwned;
		} else {
			_visViewers = reinterpret_cast<const VisViewer3*>(p);
			p += size_t(h.viewerCount) * sizeof(VisViewer3);
		}
		_visBits = reinterpret_cast<const u32*>(p);
		for (u32 i = 0; ok && i < h.chunkCount; ++i) ok = _visOrder[i] < h.chunkCount;
		for (u32 i = 0; ok && i < h.viewerCount; ++i) {
			ok = _visViewers[i].bitOffset <= h.bitCount && _visViewers[i].yMin <= _visViewers[i].yMax && _visViewers[i].room < 65536u;
			if (ok && i > 0) {
				const VisViewer3& a = _visViewers[i - 1];
				const VisViewer3& b = _visViewers[i];
				// (sorted by cell, then room, and by height within it)
				ok = a.z < b.z || (a.z == b.z && (a.x < b.x || (a.x == b.x && (a.room < b.room || (a.room == b.room && a.yMin <= b.yMin)))));
			}
		}
	}
	_visBase = ok ? static_cast<u32*>(malloc(size_t(h.chunkCount) * 4 + 4)) : nullptr;
	_visAllHidden = ok ? static_cast<u8*>(calloc(size_t(h.chunkCount) + 1, 1)) : nullptr;
	if (!ok || !_visBase || !_visAllHidden) {
		logf("world: the visibility table is damaged or does not fit; drawn without it");
		free(blob);
		freeVis();
		_visTried = true;
		return;
	}
	_vis = h;
	_visBlob = blob;
	_visRow = _visHave = false;
}

// The row of the table for the cell the camera is in: where each chunk's clusters begin in it.
void World::updateVisibility(Vec3 camera) {
	_stats.visRow = false;
	if (!visibilityActive()) {
		_visRow = _visHave = false;
		return;
	}
	const int cx = int(floorf(camera.x / _vis.cellSize)), cz = int(floorf(camera.z / _vis.cellSize));
	if (!_visHave || cx != _visCellNowX || cz != _visCellNowZ) {
		_visHave = true;
		_visCellNowX = cx;
		_visCellNowZ = cz;
		_visPick = -1;
		_visRow = false;
		_visFirst = _visCount = 0;
		// The rows of the cell: the cells are sorted by z, then x (and by height).
		u32 lo = 0, hi = _vis.viewerCount;
		while (lo < hi) {
			const u32 mid = (lo + hi) / 2;
			const VisViewer3& v = _visViewers[mid];
			if (v.z < cz || (v.z == cz && v.x < cx))
				lo = mid + 1;
			else
				hi = mid;
		}
		u32 end = lo;
		while (end < _vis.viewerCount && _visViewers[end].z == cz && _visViewers[end].x == cx) ++end;
		_visFirst = lo;
		_visCount = end - lo;
	}
	if (_visCount == 0) return;
	// The row: of the room the camera is in (a table before version 3 has only rows of the outside), and of the floor: the
	// row in use while the camera's height is still in its range (floors overlap a little: no change back and forth at
	// the edge), else the first row whose range holds it. A camera in a room that has no row here: none (all is drawn).
	const u32 room = _visRooms ? u32(_camRoom) : 0u;
	auto holds = [&](u32 i) { return _visViewers[i].room == room && camera.y >= _visViewers[i].yMin && camera.y < _visViewers[i].yMax; };
	int pick = -1;
	if (_visPick >= 0 && holds(u32(_visPick))) {
		pick = _visPick;
	} else {
		for (u32 i = _visFirst; i < _visFirst + _visCount && pick < 0; ++i)
			if (holds(i)) pick = int(i);
	}
	if (pick == _visPick) {
		_stats.visRow = _visRow;
		return;
	}
	_visPick = pick;
	_visRow = false;
	if (pick < 0) return; // (no floor of the cell is near the camera: everything is drawn)
	const u32 lo = u32(pick);
	for (u32 i = 0; i < _vis.chunkCount; ++i) {
		_visBase[i] = 0xFFFFFFFFu;
		_visAllHidden[i] = 0;
	}
	u64 bit = _visViewers[lo].bitOffset;
	const int reach = int(ceilf(_vis.radius / _vis.cellSize)) + 1;
	bool ok = true;
	for (int dz = -reach; dz <= reach && ok; ++dz)
		for (int dx = -reach; dx <= reach && ok; ++dx) {
			const float gx = float(max(abs(dx) - 1, 0)) * _vis.cellSize, gz = float(max(abs(dz) - 1, 0)) * _vis.cellSize;
			if (sqrtf(gx * gx + gz * gz) > _vis.radius) continue;
			const int tz = cz + dz, tx = cx + dx;
			// The chunks of that cell: the first of them in `order` by (z, x).
			u32 a = 0, b = _vis.chunkCount;
			while (a < b) {
				const u32 mid = (a + b) / 2;
				const u32 c = _visOrder[mid];
				if (_visCellZ[c] < tz || (_visCellZ[c] == tz && _visCellX[c] < tx))
					a = mid + 1;
				else
					b = mid;
			}
			for (u32 k = a; k < _vis.chunkCount; ++k) {
				const u32 c = _visOrder[k];
				if (_visCellZ[c] != tz || _visCellX[c] != tx) break;
				const u32 n = _visClusters[c];
				if (bit + n > _vis.bitCount) {
					ok = false;
					break;
				}
				_visBase[c] = u32(bit);
				bool any = false;
				for (u32 cl = 0; cl < n && !any; ++cl) any = (_visBits[(bit + cl) >> 5] & (1u << ((bit + cl) & 31))) != 0;
				_visAllHidden[c] = n > 0 && !any;
				bit += n;
			}
		}
	if (!ok) {
		for (u32 i = 0; i < _vis.chunkCount; ++i) {
			_visBase[i] = 0xFFFFFFFFu;
			_visAllHidden[i] = 0;
		}
		return;
	}
	_visRow = true;
	_stats.visRow = true;
}

void World::loadFar() {
	_farTried = true;
	const PakEntry* e = _pak ? _pak->find(_hdr.farEntry) : nullptr;
	if (!e || e->size < sizeof(FarHeader)) return;
	u8* blob = static_cast<u8*>(_pak->readAlloc(*e));
	if (!blob) return;
	FarHeader h;
	memcpy(&h, blob, sizeof(h));
	bool ok = h.magic == FAR_MAGIC && h.version == FAR_VERSION && h.tileCount > 0 && h.tileCount < (1u << 20) &&
	          h.rangeCount < (1u << 24) && h.chunkRefCount < (1u << 24) && h.vertexCount < (1u << 26) && h.indexCount < (1u << 28) &&
	          u64(e->size) == u64(farBlobSize(h));
	const u8* p = blob + sizeof(FarHeader);
	const FarTile* tiles = reinterpret_cast<const FarTile*>(p);
	const u8* rangeBytes = p + size_t(h.tileCount) * sizeof(FarTile);
	const u8* refBytes = rangeBytes + size_t(h.rangeCount) * sizeof(FarRange);
	const u8* vertexBytes = refBytes + size_t(h.chunkRefCount) * sizeof(u32);
	const u8* indexBytes = vertexBytes + size_t(h.vertexCount) * sizeof(WorldVertex);
	if (ok) {
		_farTiles = static_cast<FarTileRt*>(calloc(h.tileCount, sizeof(FarTileRt)));
		_farRanges = static_cast<FarRange*>(malloc(size_t(h.rangeCount) * sizeof(FarRange) + 1));
		_farRefs = static_cast<u32*>(malloc(size_t(h.chunkRefCount) * sizeof(u32) + 1));
		_farOrder = static_cast<u32*>(malloc(size_t(h.tileCount) * sizeof(u32)));
		_farKey = static_cast<float*>(malloc(size_t(h.tileCount) * sizeof(float)));
		ok = _farTiles && _farRanges && _farRefs && _farOrder && _farKey;
	}
	if (ok) {
		memcpy(_farRanges, rangeBytes, size_t(h.rangeCount) * sizeof(FarRange));
		memcpy(_farRefs, refBytes, size_t(h.chunkRefCount) * sizeof(u32));
		_farTileCount = h.tileCount;
		_farRangeCount = h.rangeCount;
		_farRefCount = h.chunkRefCount;
		for (u32 i = 0; ok && i < h.chunkRefCount; ++i) ok = _farRefs[i] < _hdr.chunkCount;
		for (u32 i = 0; ok && i < h.tileCount; ++i) {
			FarTile t;
			memcpy(&t, &tiles[i], sizeof(t));
			ok = t.vertexCount > 0 && t.vertexCount <= CHUNK_MAX_VERTICES && u64(t.firstVertex) + t.vertexCount <= h.vertexCount &&
			     u64(t.firstIndex) + t.indexCount <= h.indexCount && t.indexCount % 3 == 0 &&
			     u64(t.firstRange) + t.rangeCount <= h.rangeCount && t.posScale > 0.0f;
			for (u32 r = 0; ok && r < t.rangeCount; ++r) {
				const FarRange& fr = _farRanges[t.firstRange + r];
				ok = u64(fr.firstIndex) + fr.indexCount <= t.indexCount && fr.indexCount % 3 == 0 &&
				     u64(fr.firstChunk) + fr.chunkCount + fr.otherCount <= h.chunkRefCount && fr.seam <= 1 &&
				     true;
			}
			const u16* idx = reinterpret_cast<const u16*>(indexBytes) + t.firstIndex;
			for (u32 k = 0; ok && k < t.indexCount; ++k) {
				u16 v;
				memcpy(&v, idx + k, sizeof(v));
				ok = v < t.vertexCount;
			}
			if (!ok) break;
			_farTiles[i].t = t;
		}
	}
	if (ok) {
		// One mesh per tile. One that does not fit in memory is left out (a
		// hole far away rather than no layer at all).
		for (u32 i = 0; i < h.tileCount; ++i) {
			const FarTile& t = _farTiles[i].t;
			_farTiles[i].mesh = gfx::meshCreate(reinterpret_cast<const WorldVertex*>(vertexBytes) + t.firstVertex, t.vertexCount,
			                                    reinterpret_cast<const u16*>(indexBytes) + t.firstIndex, t.indexCount);
			if (_farTiles[i].mesh) _farBytes += t.vertexCount * u32(sizeof(WorldVertex)) + t.indexCount * u32(sizeof(u16));
		}
	}
	free(blob);
	if (!ok) {
		logf("world: the far layer is damaged; drawn without it");
		freeFar();
		_farTried = true;
	}
}

void World::freeFar() {
	if (_farTiles)
		for (u32 i = 0; i < _farTileCount; ++i)
			if (_farTiles[i].mesh) gfx::meshDestroy(_farTiles[i].mesh);
	free(_farTiles);
	free(_farRanges);
	free(_farRefs);
	free(_farOrder);
	free(_farKey);
	_farTiles = nullptr;
	_farRanges = nullptr;
	_farRefs = nullptr;
	_farOrder = nullptr;
	_farKey = nullptr;
	_farTileCount = _farRangeCount = _farRefCount = 0;
	_farBytes = 0;
	_stats.farBytes = 0;
}

// Everything on the spot: read, unpack, upload. Simple and predictable, and
// each load is a hitch as long as the chunk is big.
// What is loaded is not kept for the chunk's own sake: a nearer chunk that does not fit takes the room of the
// farthest ones (before, the memory stayed with whatever came first - the place the player started in - and
// every place reached later stayed empty, though the chunks that held the room were out of sight).
u32 World::budget() const {
	u32 b = _cfg.memoryBudget;
	if (_cfg.freeMemory) {
		const u32 free = _cfg.freeMemory();
		const u32 spare = free > _cfg.memoryReserve ? free - _cfg.memoryReserve : 0u;
		b = min(b, _stats.residentBytes + spare);
	}
	return b;
}

int World::pickVictim(Vec3 camera, float forKey) const {
	int victim = -1;
	float far = forKey + 300.0f; // (a chunk about as far is not worth a swap)
	for (u32 k = 0; k < _stats.chunksLoaded; ++k) {
		const u32 i = _resident[k];
		const float key = priority(i, distanceTo(i, camera), camera);
		if (key > far) {
			far = key;
			victim = int(i);
		}
	}
	return victim;
}

bool World::makeRoom(Vec3 camera, float forKey, u32 need, u32 reserved) {
	const u32 limit = max(budget(), need);
	while (_stats.residentBytes + reserved + need > limit) {
		const int victim = pickVictim(camera, forKey);
		if (victim < 0) return false;
		unloadChunk(u32(victim));
		++_stats.evictions;
	}
	return true;
}

// The budget turned a chunk away. A chunk that goes on waiting is one refusal, not one per frame.
void World::countReject(u32 index) {
	if (_lastRejected == index) return;
	_lastRejected = index;
	++_stats.budgetRejects;
}

void World::updateImmediate(u32 pending, Vec3 camera) {
	// Nearest first; only a handful per frame so a selection scan is enough.
	u32 remaining = pending;
	const u32 loads = min(pending, _cfg.maxLoadsPerFrame);
	for (u32 n = 0; n < loads; ++n) {
		if (_cfg.budget && !_cfg.budget->allow(_chunkCostMs, _refusedChunks)) break; // no time left in the frame
		sortNearest(n, pending);
		const u32 index = _order[n];
		if (_stats.residentBytes + meshBytes(_chunks[index]) > budget() && !makeRoom(camera, _orderKey[n], meshBytes(_chunks[index]), 0)) {
			countReject(index);
			break; // everything else is farther away; try again next frame
		}
		const double tLoad = sys::timeMs();
		const LoadStatus status = loadChunkNow(index);
		_chunkCostMs = _chunkCostMs * 0.8f + float(sys::timeMs() - tLoad) * 0.2f;
		if (status == LOAD_NO_MEMORY) {
			// The budget said yes, the allocator said no (the memory is in pieces, or something else took it). Free
			// the farthest chunk there is and try again next frame.
			++_stats.allocRejects;
			const int victim = pickVictim(camera, _orderKey[n]);
			if (victim >= 0) {
				unloadChunk(u32(victim));
				++_stats.evictions;
			}
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
	(void)remaining;

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
		if (_cfg.budget && !_cfg.budget->allow(kind == LOAD_CHUNK ? _chunkCostMs : _textureCostMs,
		                                       kind == LOAD_CHUNK ? _refusedChunks : _refusedTextures))
			break; // no time left in the frame: it waits for the next
		LoadResult r;
		if (!_loader.poll(r)) break;
		if (r.kind == LOAD_TEXTURE) {
			++texturesDone;
			const double tTex = sys::timeMs();
			finishTexture(r);
			_textureCostMs = _textureCostMs * 0.8f + float(sys::timeMs() - tTex) * 0.2f;
			continue;
		}
		const double tChunk = sys::timeMs();
		++chunksDone;
		Loaded& l = _loaded[r.index];
		l.requested = false;
		--_chunksInFlight;
		_inFlightBytes -= meshBytes(_chunks[r.index]);
		if (r.status == LOAD_OK && distanceTo(r.index, camera) > unloadReach(chunkTier(r.index))) {
			r.discard(); // the camera has moved on in the meantime
			continue;
		}
		const LoadStatus status = finishChunk(r);
		_chunkCostMs = _chunkCostMs * 0.8f + float(sys::timeMs() - tChunk) * 0.2f;
		if (status == LOAD_OK) {
			++_stats.loadsThisFrame;
		} else if (status == LOAD_BAD_DATA) {
			_loaded[r.index].failed = true;
			++_stats.loadFailures;
		} else {
			// Out of memory though the budget had room: the memory is in pieces. Free the farthest chunk
			// there is (if it is farther than this one) and try again in a couple of frames; if there is
			// none, wait a while instead of reading from the card again every frame.
			++_stats.allocRejects;
			const int victim = pickVictim(camera, priority(r.index, distanceTo(r.index, camera), camera));
			if (victim >= 0) {
				unloadChunk(u32(victim));
				++_stats.evictions;
				_loaded[r.index].retryAt = _frame + 2;
			} else {
				_loaded[r.index].retryAt = _frame + 30;
			}
		}
	}

	// 2. New wishes, nearest first. Only a couple at a time, so that what is
	// being read is still what is wanted when it arrives.
	const u32 maxInFlight = _cfg.maxInFlight > 0 ? _cfg.maxInFlight : 1;
	for (u32 n = 0; n < pending && _chunksInFlight < maxInFlight; ++n) {
		sortNearest(n, pending);
		const u32 index = _order[n];
		Loaded& l = _loaded[index];
		if (l.mesh || l.requested || l.failed || _frame < l.retryAt) continue;
		const u32 need = meshBytes(_chunks[index]);
		if (_stats.residentBytes + _inFlightBytes + need > budget() && !makeRoom(camera, _orderKey[n], need, _inFlightBytes)) {
			countReject(index);
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

	// 2b. The chunks after those: their bytes come off the card now, while the loader is busy with collision grids,
	// so that their jobs find them waiting. Only what the budget has room for as it is (no point in reading what
	// would be turned away).
	if (_ahead && _ahead->running() && _ahead->enabled()) {
		u32 asked = 0, spare = _stats.residentBytes + _inFlightBytes;
		for (u32 n = 0; n < pending && asked < _cfg.readAheadChunks; ++n) {
			sortNearest(n, pending);
			const u32 index = _order[n];
			const Loaded& l = _loaded[index];
			if (l.mesh || l.requested || l.failed) continue;
			const u32 need = meshBytes(_chunks[index]);
			if (spare + need > budget()) break; // (everything farther is no nearer to fitting)
			spare += need;
			_ahead->request(_chunks[index].entryHash);
			++asked;
		}
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

bool World::rayDown(Vec3 from, float maxDrop, float& hitY, Vec3& normal, u8* light, u8* group) const {
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
		u8 g = 0;
		// Each chunk only needs to beat the best hit so far.
		if (_loaded[i].collider.rayDown(from, from.y - best, y, n, light ? l : nullptr, group ? &g : nullptr) && y >= best) {
			best = y;
			hitY = y;
			normal = n;
			if (light) memcpy(light, l, 4);
			if (group) *group = g;
			found = true;
		}
	}
	// A lift's floor, a drawbridge, a shut door.
	if (_dynamic) {
		float y;
		Vec3 n;
		if (_dynamic->rayDown(from, from.y - best, y, n) && y >= best) {
			hitY = y;
			normal = n;
			if (light) light[0] = light[1] = light[2] = 180, light[3] = 255;
			if (group) *group = 0;
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
	if (_dynamic) moved = _dynamic->pushSphere(center, radius) || moved;
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
	return !_dynamic || !_dynamic->boxTouches(mn, mx);
}

bool World::lineFree(Vec3 a, Vec3 b, float radius) const {
	const float mn[3] = {min(a.x, b.x) - radius, min(a.y, b.y) - radius, min(a.z, b.z) - radius};
	const float mx[3] = {max(a.x, b.x) + radius, max(a.y, b.y) + radius, max(a.z, b.z) + radius};
	if (boxFree(mn, mx)) return true;
	// Steps of 1.5 radii: a wall between two of them is at most 0.75 radii
	// from the nearer one, so it touches that sphere.
	const Vec3 d = b - a;
	const float len = length(d);
	const int steps = clamp(int(len / (radius * 1.5f)) + 1, 1, 400);
	for (int i = 0; i <= steps; ++i) {
		Vec3 c = a + d * (float(i) / float(steps));
		if (pushSphere(c, radius)) return false;
	}
	return true;
}

bool World::sightBlocked(Vec3 a, Vec3 b) const {
	const float mn[3] = {min(a.x, b.x), min(a.y, b.y), min(a.z, b.z)};
	const float mx[3] = {max(a.x, b.x), max(a.y, b.y), max(a.z, b.z)};
	for (u32 k = 0; k < _stats.chunksLoaded; ++k) {
		const u32 i = _resident[k];
		if (chunkTier(i) != 0) continue; // medium and small objects: they hide too little of a person
		const ChunkInfo& ci = _chunks[i];
		if (mx[0] < ci.bbMin[0] || mn[0] > ci.bbMax[0] || mx[1] < ci.bbMin[1] || mn[1] > ci.bbMax[1] || mx[2] < ci.bbMin[2] ||
		    mn[2] > ci.bbMax[2])
			continue;
		if (_loaded[i].collider.segmentBlocked(a, b)) return true;
	}
	return _dynamic && _dynamic->segmentHits(a, b);
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
void World::classify(const Loaded& l, u32 index, bool count) {
	for (u32 c = 0; c < l.clusterCount; ++c) {
		Cluster& k = l.clusters[c];
		if (k.bbMin[0] > k.bbMax[0]) {
			_clusterState[c] = CLUSTER_OUT; // nothing in it
			continue;
		}
		// Hidden from the camera's cell (the baked table) counts as out of view.
		const bool hidden = _visRow && visHidden(index, c);
		_stats.clustersHidden += count && hidden;
		const int where = hidden ? 0 : _cfg.clusterCulling ? aabbFrustumClass(_frustum, k.bbMin, k.bbMax) : 1;
		_clusterState[c] = where == 0 ? u8(CLUSTER_OUT) : where == 1 ? u8(CLUSTER_IN) : u8(CLUSTER_PARTLY);
		_stats.clustersTested += count;
		_stats.clustersCulled += count && where == 0 && !hidden;
		// (Also out of view: whether its objects are within their distance is
		// kept up to date there too, so that turning the view finds them as
		// they should be, not dissolving in.)
		_clusterNear[c] = aabbDistance(k.bbMin, k.bbMax, _camera);
		// Far enough from the camera for the stand-ins (a cluster's own distance: a chunk is 40 m wide).
		k.lod = l.hasLod && _cfg.lodDistance > 0.0f && _clusterNear[c] > (k.lod ? _cfg.lodDistance * _cfg.lodHysteresis : _cfg.lodDistance);
		_stats.lodChunks += count && k.lod;
		// The stand-ins and the full batches dissolve into each other (a chunk not drawn the frame before takes its state at once).
		if (k.mixFrame != _drawFrame) {
			k.mixFrame = _drawFrame;
			const int target = k.lod ? 255 : 0;
			if (_fadeStep >= 255 || l.drawnFrame + 1 != _drawFrame)
				k.lodMix = u8(target);
			else if (k.lodMix < target)
				k.lodMix = u8(min(int(k.lodMix) + _lodStep, 255));
			else if (k.lodMix > target)
				k.lodMix = u8(max(int(k.lodMix) - _lodStep, 0));
		}
		if (where == 0) continue;
		_clusterFar[c] = aabbFarDistance(k.bbMin, k.bbMax, _camera);
	}
}

void World::drawBatches(Loaded& l, bool blended, int phase) {
	const u32 ms = _timeMs;
	bool transformSet = false;
	// A chunk that was not drawn the frame before (it has just come into view,
	// or just been loaded) shows its objects as they are, without dissolving;
	// so does the translucent pass. (The second pass over a chunk takes the first one's answer.)
	bool snap;
	if (phase == 2) {
		snap = l.snapNow;
	} else {
		snap = blended || _fadeStep >= 255 || l.drawnFrame + 1 != _drawFrame;
		l.snapNow = snap;
		l.windBatches = 0;
		if (!blended) l.drawnFrame = _drawFrame;
	}
	// The chunk as a whole may be dissolving at the draw distance: its batches share that, and merge as they would.
	const u8 edge = blended ? u8(255) : l.edgeShare;
	u8 runShown = edge;
	// A culled stretch this short between two runs of a material is drawn
	// along with them: a draw call costs the GPU about what 300 triangles do
	// (measured on the console: ~25 us a draw, ~0.07 ms per 1000 triangles),
	// and triangles out of view are clipped, which is cheaper still.
	const u32 bridge = 256 * 3;
	u32 runFirst = 0, runCount = 0, gap = 0, runMaterial = 0;
	auto flush = [&]() {
		gap = 0;
		if (!runCount) return;
		if (!transformSet) {
			gfx::ChunkFx fx;
			fx.occlusion = _fxOcclusion;
			if (_windX != 0.0f || _windZ != 0.0f) {
				fx.windX = _windX;
				fx.windZ = _windZ;
				fx.windPhase = gfx::windPhaseAt(l.origin, _windSeconds, kWindWavesPerSecond);
			}
			gfx::setChunkTransform(l.origin, l.posScale, l.uvScale, &fx);
			transformSet = true;
		}
		const WorldMaterial& m = _materials[runMaterial];
		_stats.windyDrawCalls += (m.flags & MATF_WIND) != 0 && (_windX != 0.0f || _windZ != 0.0f);
		const u32 frame = (m.frameCount > 1 && m.frameMs) ? (ms / m.frameMs) % m.frameCount : 0;
		float offset[2] = {0, 0};
		for (int a = 0; a < 2; ++a) {
			if (!m.scroll[a]) continue;
			// Whole repeats are dropped in integers, so the scroll stays smooth
			// however long the game has been running.
			const u32 period = 4096u * 1000u; // scroll units * ms per repeat
			const u32 scrollPhase = u32((u64(ms) * u32(m.scroll[a] < 0 ? -m.scroll[a] : m.scroll[a])) % period);
			const float f = float(scrollPhase) / float(period);
			offset[a] = m.scroll[a] < 0 ? 1.0f - f : f;
		}
		gfx::setMaterial(_tex[u32(m.texture) + frame].tex, m.flags, float(m.opacity) * (1.0f / 255.0f), offset[0], offset[1]);
		if (!blended) gfx::setFade(float(runShown) * (1.0f / 255.0f));
		gfx::draw(l.mesh, runFirst, runCount);
		++_stats.drawCalls;
		_stats.blendedDrawCalls += blended;
		_stats.trianglesDrawn += runCount / 3;
		runCount = 0;
	};
	for (u32 b = 0; b < l.batchCount; ++b) {
		Batch& batch = l.batches[b];
		const WorldMaterial& m = _materials[batch.material];
		if (((m.flags & MATF_TRANSLUCENT) != 0) != blended || batch.indexCount == 0) continue;
		// What sways is drawn after everything else: on the console it is a different vertex shader, and a change
		// of shader between two draw calls costs more than the calls do.
		if (phase != 0 && ((m.flags & MATF_WIND) != 0) != (phase == 2)) {
			l.windBatches += phase == 1;
			continue;
		}
		// A chunk far enough away shows its simplified stand-ins in place of the batches they replace.
		const u32 c = batch.cluster < l.clusterCount ? batch.cluster : 0u;
		// (Not dissolving - the translucent pass, or no dissolve at all - it is one or the other at once.)
		u8 lodShare = 255;
		if (batch.lodMode != BATCH_LOD_BOTH) {
			if (blended) {
				if (batch.lodMode != (l.clusters[c].lod ? BATCH_LOD_FAR_ONLY : BATCH_LOD_FULL_ONLY)) continue;
			} else {
				const u8 mix = l.clusters[c].lodMix;
				lodShare = batch.lodMode == BATCH_LOD_FAR_ONLY ? mix : u8(255 - mix);
				if (lodShare == 0) continue;
			}
		}
		bool visible = _clusterState[c] != CLUSTER_OUT;
		u8 shown = 255;
		if (batch.detail != DETAIL_ALWAYS && _clusterState[c] != CLUSTER_OUT) {
			// Too far for its size? Mostly the box's distance says so already.
			// What is drawn goes a little farther out than it comes in.
			const float limit = detailReach(batch);
			const float reach = batch.shown ? limit * _cfg.detailHysteresis : limit;
			bool inRange = true;
			if ((batch.detail == DETAIL_SMALL && batch.layer >= _cfg.smallLayers) || _clusterNear[c] > reach)
				inRange = false;
			else if (_clusterFar[c] > reach)
				inRange = aabbDistance(batch.bbMin, batch.bbMax, _camera) <= reach;
			// (Counted where the size alone keeps it away: within its class's distance, beyond its band's.)
			if (!inRange && batch.band && batch.shown == 0 && _clusterNear[c] <= _cfg.detailDistance[batch.detail] * _cfg.detailScale &&
			    !(batch.detail == DETAIL_SMALL && batch.layer >= _cfg.smallLayers))
				++_stats.sizeCulled;
			const int target = inRange ? 255 : 0;
			if (snap)
				batch.shown = u8(target);
			else if (batch.shown < target)
				batch.shown = u8(min(int(batch.shown) + _fadeStep, 255));
			else if (batch.shown > target)
				batch.shown = u8(max(int(batch.shown) - _fadeStep, 0));
			shown = batch.shown;
			visible = shown > 0;
		} else if (batch.detail != DETAIL_ALWAYS) {
			// Out of view: in or out at once, by the nearest point of the box.
			const float limit = detailReach(batch);
			const bool thinned = batch.detail == DETAIL_SMALL && batch.layer >= _cfg.smallLayers;
			batch.shown = !thinned && _clusterNear[c] <= (batch.shown ? limit * _cfg.detailHysteresis : limit) ? 255 : 0;
		}
		if (lodShare != 255) {
			shown = u8(int(shown) * int(lodShare) / 255);
			visible = visible && shown > 0;
		}
		// (Own dissolve and the chunk's together; `own` is what differs from the chunk's share.)
		const bool own = shown != 255;
		if (edge != 255 && !blended) {
			shown = u8(int(shown) * int(edge) / 255);
			visible = visible && shown > 0;
		}
		if (visible && _clusterState[c] == CLUSTER_PARTLY) visible = aabbInFrustum(_frustum, batch.bbMin, batch.bbMax);
		if (visible && !blended) {
			const u32 t = batch.indexCount / 3;
			_stats.trisByDetail[batch.detail & 3u] += t;
			const float nd = _clusterNear[c];
			_stats.trisByDist[nd < 1200.0f ? 0 : nd < 2500.0f ? 1 : 2] += t;
			if (batch.lodMode == BATCH_LOD_FAR_ONLY) _stats.lodTriangles += t;
		}
		const bool follows = runCount && batch.material == runMaterial && batch.firstIndex == runFirst + runCount + gap;
		if (!visible) {
			++_stats.batchesCulled;
			if (follows && gap + batch.indexCount <= bridge)
				gap += batch.indexCount;
			else
				flush();
			continue;
		}
		if (own || (edge != 255 && shown != edge)) {
			// Dissolving: a draw call of its own.
			flush();
			runFirst = batch.firstIndex;
			runCount = batch.indexCount;
			runMaterial = batch.material;
			runShown = shown;
			flush();
			runShown = edge;
			++_stats.batchesFading;
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
	if (edge != 255) gfx::setFade(1.0f);
	if (!blended) gfx::setFade(1.0f);
}

void World::draw(const Mat4& viewProj, Vec3 camera, u32 timeMs) {
	_stats.chunksDrawn = 0;
	_stats.chunksEdgeFading = 0;
	_stats.drawCalls = 0;
	_stats.blendedDrawCalls = 0;
	_stats.windyDrawCalls = 0;
	_stats.batchesCulled = 0;
	_stats.sizeCulled = 0;
	_stats.batchesMerged = 0;
	_stats.batchesFading = 0;
	_stats.trianglesDrawn = 0;
	_stats.trianglesInChunks = 0;
	for (int i = 0; i < 3; ++i) _stats.trisByDetail[i] = _stats.trisByDist[i] = 0;
	_stats.lodChunks = _stats.lodTriangles = 0;
	_stats.clustersTested = 0;
	_stats.clustersCulled = 0;
	_stats.farDrawCalls = 0;
	_stats.farTriangles = 0;
	_stats.farTiles = 0;
	_stats.farSeams = 0;
	_stats.chunksHidden = 0;
	_stats.clustersHidden = 0;
	_visible = 0;
	if (!_pak) return;
	updateVisibility(camera);

	_frustum = frustumFromViewProj(viewProj);
	const Frustum whole = _frustum;
	// With the far layer the fog ends farther out than the chunks are drawn:
	// a chunk in reach is drawn to its end (the far layer leaves out all of
	// it, not a part).
	const float chunkReach = drawReach() + (farActive() ? _hdr.chunkSize * 1.5f : 0.0f);
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
			farPlane[3] = ahead[0] * camera.x + ahead[1] * camera.y + ahead[2] * camera.z + chunkReach;
		}
	}
	_camera = camera;
	// How far the objects that dissolve get this frame. (A long pause - the
	// first frame, a world switch - finishes them; so does a frame of the same
	// moment again, a still picture.)
	const u32 sinceMs = timeMs - _lastDrawMs;
	if (_cfg.fadeMs <= 0.0f || !gfx::fadeSupported() || _drawFrame == 1 || sinceMs == 0 || sinceMs > 250)
		_fadeStep = 255;
	else
		_fadeStep = clamp(int(float(sinceMs) * 255.0f / _cfg.fadeMs + 0.5f), 1, 255);
	_lodStep = _fadeStep >= 255 ? 255 : clamp(int(float(_fadeStep) * _cfg.fadeMs / max(_cfg.lodFadeMs, 1.0f) + 0.5f), 1, 255);
	_lastDrawMs = timeMs;
	++_drawFrame;
	_timeMs = timeMs;
	// Graphics+: as strong as asked, where the world has it. The wind comes
	// in gusts: two slow waves of different length make it uneven.
	_fxOcclusion = hasOcclusion() ? clamp(_cfg.occlusion, 0.0f, 1.0f) : 0.0f;
	_windX = _windZ = 0.0f;
	_windSeconds = double(timeMs) * 0.001;
	if (hasWind() && _cfg.wind > 0.0f) {
		const double t = _windSeconds;
		const float gust = 0.75f + 0.25f * float(sin(t * 0.31) * sin(t * 0.17 + 1.3));
		const float push = clamp(_cfg.wind, 0.0f, 2.0f) * WORLD_WIND_UNIT * gust;
		_windX = push * 0.8f;
		_windZ = push * 0.6f;
	}
	u32 n = 0;
	for (u32 r = 0; r < _stats.chunksLoaded; ++r) {
		const u32 i = _resident[r];
		const float d = distanceTo(i, camera);
		if (d > drawReach() || !aabbInFrustum(_frustum, _chunks[i].bbMin, _chunks[i].bbMax)) continue;
		if (_visRow && _visAllHidden[i]) {
			++_stats.chunksHidden;
			continue;
		}
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

	const float band = edgeBand();
	for (u32 k = 0; k < n; ++k) {
		Loaded& l = _loaded[_order[k]];
		l.edgeShare = 255;
		if (band > 0.0f) {
			const float left = _cfg.drawDistance - _orderKey[k];
			if (left < band) l.edgeShare = u8(clamp(left / band, 0.0f, 1.0f) * 255.0f);
		}
		_stats.chunksEdgeFading += l.edgeShare != 255;
		classify(l, _order[k], true);
		drawBatches(l, false, 1);
		++_stats.chunksDrawn;
		_stats.trianglesInChunks += l.triangles;
	}
	if (farActive()) drawFar(whole);
	for (u32 k = 0; k < n; ++k) {
		Loaded& l = _loaded[_order[k]];
		if (!l.windBatches) continue;
		// The shared cluster arrays still hold the last chunk of the first pass: classify this one again.
		classify(l, _order[k], false);
		drawBatches(l, false, 2);
	}
}

bool World::farCovered(u32 firstChunk, u32 count) const {
	if (!count) return false;
	for (u32 k = 0; k < count; ++k) {
		const u32 c = _farRefs[firstChunk + k];
		// What draw() draws: loaded, and within the draw distance. (Without farFillNear a chunk within the draw distance
		// counts as drawn whether it is loaded or not: nothing coarse stands in for it.)
		if (distanceTo(c, _camera) > _cfg.drawDistance - edgeBand()) return false;
		if (!_loaded[c].mesh && _cfg.farFillNear) return false;
	}
	return true;
}

void World::drawFar(const Frustum& view) {
	// Out to the end of the fog, which is the far layer's reach.
	Frustum f = view;
	{
		float* nearPlane = f.p[4];
		const float len = sqrtf(nearPlane[0] * nearPlane[0] + nearPlane[1] * nearPlane[1] + nearPlane[2] * nearPlane[2]);
		if (len > 0.0f) {
			const float ahead[3] = {nearPlane[0] / len, nearPlane[1] / len, nearPlane[2] / len};
			float* farPlane = f.p[5];
			farPlane[0] = -ahead[0];
			farPlane[1] = -ahead[1];
			farPlane[2] = -ahead[2];
			farPlane[3] = ahead[0] * _camera.x + ahead[1] * _camera.y + ahead[2] * _camera.z + _cfg.farDistance;
		}
	}
	// Front to back, after the chunks (which hide most of what is near).
	u32 n = 0;
	for (u32 i = 0; i < _farTileCount; ++i) {
		const FarTile& t = _farTiles[i].t;
		if (!_farTiles[i].mesh) continue;
		const float d = aabbDistance(t.bbMin, t.bbMax, _camera);
		if (d > _cfg.farDistance || !aabbInFrustum(f, t.bbMin, t.bbMax)) continue;
		u32 k = n++;
		while (k > 0 && _farKey[k - 1] > d) {
			_farOrder[k] = _farOrder[k - 1];
			_farKey[k] = _farKey[k - 1];
			--k;
		}
		_farOrder[k] = i;
		_farKey[k] = d;
	}
	if (!n) return;
	// One flat colour per corner: no texture, no cut-outs, no fade.
	gfx::setMaterial(nullptr, 0);
	gfx::setFade(1.0f);
	for (u32 o = 0; o < n; ++o) {
		const FarTileRt& tile = _farTiles[_farOrder[o]];
		const FarTile& t = tile.t;
		bool transformSet = false, any = false;
		u32 runFirst = 0, runCount = 0;
		auto flush = [&]() {
			if (!runCount) return;
			if (!transformSet) {
				// A hair farther from the eye than it is (scaled about the eye:
				// the same pixels, a deeper depth), so that where it overlaps a
				// chunk drawn in full along their seam, the chunk is in front.
				const float push = 1.01f;
				const float origin[3] = {_camera.x + (t.origin[0] - _camera.x) * push, _camera.y + (t.origin[1] - _camera.y) * push,
				                         _camera.z + (t.origin[2] - _camera.z) * push};
				gfx::ChunkFx fx;
				fx.occlusion = _fxOcclusion; // (the vertices' w carries the cell's average occlusion)
				gfx::setChunkTransform(origin, t.posScale * push, 1.0f, &fx);
				transformSet = true;
			}
			gfx::draw(tile.mesh, runFirst, runCount);
			++_stats.farDrawCalls;
			++_stats.drawCalls;
			_stats.farTriangles += runCount / 3;
			any = true;
			runCount = 0;
		};
		for (u32 r = 0; r < t.rangeCount; ++r) {
			const FarRange& fr = _farRanges[t.firstRange + r];
			if (fr.seam) {
				// A seam: drawn where one of its cells is in full and the other is not.
				const bool a = farCovered(fr.firstChunk, fr.chunkCount), b = farCovered(fr.firstChunk + fr.chunkCount, fr.otherCount);
				flush();
				if (a == b || aabbDistance(fr.bbMin, fr.bbMax, _camera) > _cfg.farDistance || !aabbInFrustum(f, fr.bbMin, fr.bbMax))
					continue;
				runFirst = fr.firstIndex;
				runCount = fr.indexCount;
				gfx::setTwoSided(true);
				flush();
				gfx::setTwoSided(false);
				++_stats.farSeams;
				continue;
			}
			const bool draw = !farCovered(fr.firstChunk, fr.chunkCount) && aabbDistance(fr.bbMin, fr.bbMax, _camera) <= _cfg.farDistance &&
			                  aabbInFrustum(f, fr.bbMin, fr.bbMax);
			if (!draw) {
				flush();
				continue;
			}
			if (runCount && fr.firstIndex == runFirst + runCount) {
				runCount += fr.indexCount;
			} else {
				flush();
				runFirst = fr.firstIndex;
				runCount = fr.indexCount;
			}
		}
		flush();
		_stats.farTiles += any;
	}
}

void World::drawBlended() {
	// Back to front, so nearer water blends over farther water.
	for (u32 k = _visible; k-- > 0;) {
		Loaded& l = _loaded[_order[k]];
		if (!l.mesh || !l.hasBlended) continue;
		classify(l, _order[k], false);
		drawBatches(l, true);
	}
}

} // namespace g3
