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
	return true;
}

void World::close() {
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

// Bad data is remembered and never retried. Running out of memory is not:
// update() tries again, one texture per frame, until it fits.
void World::loadTexture(u32 texture) {
	TexSlot& s = _tex[texture];
	if (s.tex || s.failed) return;

	const PakEntry* e = _pak->find(_textures[texture]);
	u8* blob = (e && e->type == ASSET_TEXTURE && e->size >= sizeof(TexHeader)) ? static_cast<u8*>(_pak->readAlloc(*e))
	                                                                         : nullptr;
	bool valid = false;
	if (blob) {
		TexHeader h;
		memcpy(&h, blob, sizeof(h));
		valid = texValidate(h, e->size - u32(sizeof(TexHeader)));
		if (valid) {
			s.tex = gfx::textureCreate(h, blob + sizeof(TexHeader));
			s.bytes = h.dataSize;
		}
		free(blob);
	}
	if (s.tex) {
		_stats.residentBytes += s.bytes;
		++_stats.texturesLoaded;
		if (s.waiting) --_texturesWaiting;
		s.waiting = false;
	} else if (!valid) {
		s.failed = true; // do not hit the SD card again every frame
		++_stats.loadFailures;
		if (s.waiting) --_texturesWaiting;
		s.waiting = false;
	} else if (!s.waiting) {
		s.waiting = true;
		s.bytes = 0;
		++_texturesWaiting;
	}
}

void World::acquireTexture(u32 texture) {
	++_tex[texture].refs;
	loadTexture(texture);
}

void World::releaseTexture(u32 texture) {
	TexSlot& s = _tex[texture];
	if (s.refs == 0 || --s.refs != 0) return;
	if (s.tex) {
		gfx::textureDestroy(s.tex);
		_stats.residentBytes -= s.bytes;
		--_stats.texturesLoaded;
	}
	if (s.waiting) --_texturesWaiting;
	s.waiting = false;
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

World::LoadResult World::loadChunk(u32 index) {
	const ChunkInfo& ci = _chunks[index];
	const PakEntry* e = _pak->find(ci.entryHash);
	if (!e || e->type != ASSET_CHUNK || e->size != ci.size || e->size < sizeof(ChunkHeader)) return LOAD_BAD_DATA;

	u8* blob = static_cast<u8*>(_pak->readAlloc(*e));
	if (!blob) return LOAD_BAD_DATA;

	ChunkHeader h;
	memcpy(&h, blob, sizeof(h));
	bool ok = h.magic == CHUNK_MAGIC && h.vertexCount == ci.vertexCount && h.indexCount == ci.indexCount &&
	          h.batchCount == ci.batchCount && h.indexCount % 3 == 0 &&
	          e->size == chunkBlobSize(h.batchCount, h.vertexCount, h.indexCount);

	// The blob is laid out so every section is naturally aligned.
	const ChunkBatch* batches = reinterpret_cast<const ChunkBatch*>(blob + sizeof(ChunkHeader));
	const WorldVertex* verts = reinterpret_cast<const WorldVertex*>(batches + h.batchCount);
	const u16* indices = reinterpret_cast<const u16*>(verts + h.vertexCount);

	for (u32 b = 0; ok && b < h.batchCount; ++b) {
		ok = batches[b].material < _hdr.materialCount && batches[b].firstIndex <= h.indexCount &&
		     batches[b].indexCount <= h.indexCount - batches[b].firstIndex;
	}
	for (u32 i = 0; ok && i < h.indexCount; ++i) ok = indices[i] < h.vertexCount;

	Loaded l {};
	LoadResult result = ok ? LOAD_OK : LOAD_BAD_DATA;
	if (ok) {
		// From here on the data is good; what can still fail is memory.
		l.batches = static_cast<Batch*>(malloc(size_t(h.batchCount) * sizeof(Batch) + 1));
		ok = l.batches && l.collider.build(h, batches, verts, indices, _materials, MATF_NO_COLLISION, 0) &&
		     l.water.build(h, batches, verts, indices, _materials, MATF_WATER, MATF_WATER);
		l.mesh = ok ? gfx::meshCreate(verts, h.vertexCount, indices, h.indexCount) : nullptr;
		ok = l.mesh != nullptr;
		if (!ok) result = LOAD_NO_MEMORY;
	}
	if (ok) {
		for (u32 b = 0; b < h.batchCount; ++b) {
			const ChunkBatch& src = batches[b];
			Batch& dst = l.batches[b];
			dst.material = src.material;
			dst.detail = src.detail < DETAIL_COUNT ? src.detail : u8(DETAIL_COUNT - 1);
			dst.firstIndex = src.firstIndex;
			dst.indexCount = src.indexCount;
			for (int a = 0; a < 3; ++a) {
				dst.bbMin[a] = h.origin[a] + float(src.bbMin[a]) * h.posScale;
				dst.bbMax[a] = h.origin[a] + float(src.bbMax[a]) * h.posScale;
			}
			l.hasBlended = l.hasBlended || (_materials[src.material].flags & MATF_TRANSLUCENT) != 0;
		}
		l.batchCount = h.batchCount;
		memcpy(l.origin, h.origin, sizeof(l.origin));
		l.posScale = h.posScale;
		l.uvScale = h.uvScale;
		l.bytes = h.vertexCount * u32(sizeof(WorldVertex)) + h.indexCount * u32(sizeof(u16));
		l.slot = _stats.chunksLoaded;
		_resident[l.slot] = index;
		_loaded[index] = l;
		_stats.residentBytes += l.bytes;
		_stats.collisionBytes += l.collider.bytes() + l.water.bytes();
		++_stats.chunksLoaded;
		for (u32 b = 0; b < l.batchCount; ++b) acquireMaterial(l.batches[b].material);
		_stats.peakResidentBytes = max(_stats.peakResidentBytes, _stats.residentBytes);
	} else {
		l.collider.release();
		l.water.release();
		free(l.batches);
	}
	free(blob);
	return result;
}

void World::unloadChunk(u32 index) {
	Loaded& l = _loaded[index];
	if (!l.mesh) return;
	for (u32 b = 0; b < l.batchCount; ++b) releaseMaterial(l.batches[b].material);
	gfx::meshDestroy(l.mesh);
	free(l.batches);
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

void World::update(Vec3 camera) {
	_stats.loadsThisFrame = 0;
	if (!_pak) return;

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

	// Nearest first; only a handful per frame so a selection scan is enough.
	u32 remaining = pending;
	const u32 loads = min(pending, _cfg.maxLoadsPerFrame);
	for (u32 n = 0; n < loads; ++n) {
		u32 best = n;
		for (u32 k = n + 1; k < pending; ++k)
			if (_orderKey[k] < _orderKey[best]) best = k;
		u32 ti = _order[n];
		_order[n] = _order[best];
		_order[best] = ti;
		float tf = _orderKey[n];
		_orderKey[n] = _orderKey[best];
		_orderKey[best] = tf;

		const u32 index = _order[n];
		const ChunkInfo& ci = _chunks[index];
		const u32 need = u32(ci.vertexCount) * u32(sizeof(WorldVertex)) + ci.indexCount * u32(sizeof(u16));
		if (_stats.residentBytes + need > _cfg.memoryBudget) {
			++_stats.budgetRejects;
			break; // everything else is farther away; try again next frame
		}
		const LoadResult result = loadChunk(index);
		if (result == LOAD_NO_MEMORY) {
			// The budget said yes, the allocator said no. Same remedy: wait
			// until something has been unloaded.
			++_stats.budgetRejects;
			break;
		}
		if (result == LOAD_OK) {
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
			loadTexture(i);
			break;
		}
	}
}

void World::preload(Vec3 camera) {
	const u32 saved = _cfg.maxLoadsPerFrame;
	_cfg.maxLoadsPerFrame = 0xFFFFFFFFu;
	update(camera);
	_cfg.maxLoadsPerFrame = saved;
}

bool World::rayDown(Vec3 from, float maxDrop, float& hitY, Vec3& normal) const {
	bool found = false;
	float best = from.y - maxDrop;
	for (u32 k = 0; k < _stats.chunksLoaded; ++k) {
		const u32 i = _resident[k];
		const ChunkInfo& ci = _chunks[i];
		if (from.x < ci.bbMin[0] || from.x > ci.bbMax[0] || from.z < ci.bbMin[2] || from.z > ci.bbMax[2]) continue;
		if (from.y < ci.bbMin[1] || best > ci.bbMax[1]) continue;
		float y;
		Vec3 n;
		// Each chunk only needs to beat the best hit so far.
		if (_loaded[i].collider.rayDown(from, from.y - best, y, n) && y >= best) {
			best = y;
			hitY = y;
			normal = n;
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

void World::drawBatches(const Loaded& l, bool blended) {
	const u32 ms = _timeMs;
	bool transformSet = false;
	for (u32 b = 0; b < l.batchCount; ++b) {
		const Batch& batch = l.batches[b];
		const WorldMaterial& m = _materials[batch.material];
		if (((m.flags & MATF_TRANSLUCENT) != 0) != blended || batch.indexCount == 0) continue;
		if ((batch.detail != DETAIL_ALWAYS &&
		     aabbDistance(batch.bbMin, batch.bbMax, _camera) > _cfg.detailDistance[batch.detail]) ||
		    !aabbInFrustum(_frustum, batch.bbMin, batch.bbMax)) {
			++_stats.batchesCulled;
			continue;
		}
		if (!transformSet) {
			gfx::setChunkTransform(l.origin, l.posScale, l.uvScale);
			transformSet = true;
		}

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
		gfx::draw(l.mesh, batch.firstIndex, batch.indexCount);
		++_stats.drawCalls;
		_stats.blendedDrawCalls += blended;
		_stats.trianglesDrawn += batch.indexCount / 3;
	}
}

void World::draw(const Mat4& viewProj, Vec3 camera, u32 timeMs) {
	_stats.chunksDrawn = 0;
	_stats.drawCalls = 0;
	_stats.blendedDrawCalls = 0;
	_stats.batchesCulled = 0;
	_stats.trianglesDrawn = 0;
	_visible = 0;
	if (!_pak) return;

	_frustum = frustumFromViewProj(viewProj);
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
		drawBatches(_loaded[_order[k]], false);
		++_stats.chunksDrawn;
	}
}

void World::drawBlended() {
	// Back to front, so nearer water blends over farther water.
	for (u32 k = _visible; k-- > 0;) {
		const Loaded& l = _loaded[_order[k]];
		if (l.mesh && l.hasBlended) drawBatches(l, true);
	}
}

} // namespace g3
