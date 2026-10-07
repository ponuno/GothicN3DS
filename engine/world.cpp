#include "world.h"

#include <stdlib.h>
#include <string.h>

namespace g3 {

bool World::open(Pak* pak, const char* name) {
	close();
	char path[96];
	snprintf(path, sizeof(path), "world/%s", name);
	const PakEntry* e = pak->find(path);
	if (!e || e->type != ASSET_WORLD || e->size < sizeof(WorldHeader)) {
		logf("world: '%s' not found in pak", path);
		return false;
	}
	u8* blob = static_cast<u8*>(pak->readAlloc(*e));
	if (!blob) return false;

	memcpy(&_hdr, blob, sizeof(_hdr));
	const size_t matBytes = size_t(_hdr.materialCount) * sizeof(WorldMaterial);
	const size_t chunkBytes = size_t(_hdr.chunkCount) * sizeof(ChunkInfo);
	bool ok = _hdr.magic == WORLD_MAGIC && _hdr.version == WORLD_VERSION &&
	          e->size == sizeof(WorldHeader) + matBytes + chunkBytes;
	if (ok) {
		_materials = static_cast<WorldMaterial*>(malloc(matBytes + 1));
		_chunks = static_cast<ChunkInfo*>(malloc(chunkBytes + 1));
		_loaded = static_cast<Loaded*>(calloc(_hdr.chunkCount + 1, sizeof(Loaded)));
		_tex = static_cast<TexSlot*>(calloc(_hdr.materialCount + 1, sizeof(TexSlot)));
		_order = static_cast<u32*>(malloc((size_t(_hdr.chunkCount) + 1) * sizeof(u32)));
		_orderKey = static_cast<float*>(malloc((size_t(_hdr.chunkCount) + 1) * sizeof(float)));
		ok = _materials && _chunks && _loaded && _tex && _order && _orderKey;
	}
	if (ok) {
		memcpy(_materials, blob + sizeof(WorldHeader), matBytes);
		memcpy(_chunks, blob + sizeof(WorldHeader) + matBytes, chunkBytes);
	}
	free(blob);
	if (!ok) {
		logf("world: '%s' has a bad header", path);
		_hdr = {};
		close();
		return false;
	}
	_pak = pak;
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
	free(_chunks);
	free(_loaded);
	free(_tex);
	free(_order);
	free(_orderKey);
	_materials = nullptr;
	_chunks = nullptr;
	_loaded = nullptr;
	_tex = nullptr;
	_order = nullptr;
	_orderKey = nullptr;
	_pak = nullptr;
	_hdr = {};
}

float World::distanceTo(u32 index, Vec3 c) const {
	const ChunkInfo& ci = _chunks[index];
	float dx = max(max(ci.bbMin[0] - c.x, c.x - ci.bbMax[0]), 0.0f);
	float dy = max(max(ci.bbMin[1] - c.y, c.y - ci.bbMax[1]), 0.0f);
	float dz = max(max(ci.bbMin[2] - c.z, c.z - ci.bbMax[2]), 0.0f);
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

gfx::Texture* World::acquireTexture(u32 material) {
	TexSlot& s = _tex[material];
	++s.refs;
	if (s.tex || s.failed) return s.tex;

	const PakEntry* e = _pak->find(_materials[material].textureHash);
	u8* blob = (e && e->type == ASSET_TEXTURE && e->size >= sizeof(TexHeader)) ? static_cast<u8*>(_pak->readAlloc(*e))
	                                                                         : nullptr;
	if (blob) {
		TexHeader h;
		memcpy(&h, blob, sizeof(h));
		if (texValidate(h, e->size - u32(sizeof(TexHeader)))) {
			s.tex = gfx::textureCreate(h, blob + sizeof(TexHeader));
			s.bytes = h.dataSize;
		}
		free(blob);
	}
	if (s.tex) {
		_stats.residentBytes += s.bytes;
		++_stats.texturesLoaded;
	} else {
		s.failed = true; // do not hit the SD card again every frame
		++_stats.loadFailures;
	}
	return s.tex;
}

void World::releaseTexture(u32 material) {
	TexSlot& s = _tex[material];
	if (s.refs == 0 || --s.refs != 0) return;
	if (s.tex) {
		gfx::textureDestroy(s.tex);
		_stats.residentBytes -= s.bytes;
		--_stats.texturesLoaded;
	}
	s.tex = nullptr;
	s.bytes = 0;
}

bool World::loadChunk(u32 index) {
	const ChunkInfo& ci = _chunks[index];
	const PakEntry* e = _pak->find(ci.entryHash);
	if (!e || e->type != ASSET_CHUNK || e->size != ci.size || e->size < sizeof(ChunkHeader)) return false;

	u8* blob = static_cast<u8*>(_pak->readAlloc(*e));
	if (!blob) return false;

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
	if (ok) {
		l.batches = static_cast<ChunkBatch*>(malloc(size_t(h.batchCount) * sizeof(ChunkBatch) + 1));
		l.mesh = l.batches ? gfx::meshCreate(verts, h.vertexCount, indices, h.indexCount) : nullptr;
		ok = l.mesh != nullptr;
	}
	if (ok) {
		memcpy(l.batches, batches, size_t(h.batchCount) * sizeof(ChunkBatch));
		l.batchCount = h.batchCount;
		memcpy(l.origin, h.origin, sizeof(l.origin));
		l.posScale = h.posScale;
		l.uvScale = h.uvScale;
		l.bytes = h.vertexCount * u32(sizeof(WorldVertex)) + h.indexCount * u32(sizeof(u16));
		_loaded[index] = l;
		_stats.residentBytes += l.bytes;
		++_stats.chunksLoaded;
		for (u32 b = 0; b < l.batchCount; ++b) acquireTexture(l.batches[b].material);
		_stats.peakResidentBytes = max(_stats.peakResidentBytes, _stats.residentBytes);
	} else {
		free(l.batches);
	}
	free(blob);
	return ok;
}

void World::unloadChunk(u32 index) {
	Loaded& l = _loaded[index];
	if (!l.mesh) return;
	for (u32 b = 0; b < l.batchCount; ++b) releaseTexture(l.batches[b].material);
	gfx::meshDestroy(l.mesh);
	free(l.batches);
	_stats.residentBytes -= l.bytes;
	--_stats.chunksLoaded;
	l = {};
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

	// Nearest first; only a handful per frame so a selection scan is enough.
	u32 loads = min(pending, _cfg.maxLoadsPerFrame);
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
		if (loadChunk(index)) {
			++_stats.loadsThisFrame;
			--pending;
		} else {
			_loaded[index].failed = true;
			++_stats.loadFailures;
			--pending;
		}
	}
	_stats.chunksPending = pending;
}

void World::preload(Vec3 camera) {
	const u32 saved = _cfg.maxLoadsPerFrame;
	_cfg.maxLoadsPerFrame = 0xFFFFFFFFu;
	update(camera);
	_cfg.maxLoadsPerFrame = saved;
}

void World::draw(const Mat4& viewProj, Vec3 camera) {
	_stats.chunksDrawn = 0;
	_stats.drawCalls = 0;
	_stats.trianglesDrawn = 0;
	if (!_pak) return;

	const Frustum fr = frustumFromViewProj(viewProj);
	u32 n = 0;
	for (u32 i = 0; i < _hdr.chunkCount; ++i) {
		if (!_loaded[i].mesh) continue;
		const float d = distanceTo(i, camera);
		if (d > _cfg.drawDistance || !aabbInFrustum(fr, _chunks[i].bbMin, _chunks[i].bbMax)) continue;
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

	for (u32 k = 0; k < n; ++k) {
		const Loaded& l = _loaded[_order[k]];
		gfx::setChunkTransform(l.origin, l.posScale, l.uvScale);
		for (u32 b = 0; b < l.batchCount; ++b) {
			const ChunkBatch& batch = l.batches[b];
			gfx::setMaterial(_tex[batch.material].tex, _materials[batch.material].flags);
			gfx::draw(l.mesh, batch.firstIndex, batch.indexCount);
			++_stats.drawCalls;
			_stats.trianglesDrawn += batch.indexCount / 3;
		}
		++_stats.chunksDrawn;
	}
}

} // namespace g3
