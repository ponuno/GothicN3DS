#include "loader.h"

#include "g3ds/tex.h"

#include <stdlib.h>
#include <string.h>

namespace g3 {

void LoadResult::discard() {
	free(blob);
	blob = nullptr;
	size = 0;
	collider.release();
	water.release();
}

void StreamLoader::process(Pak& pak, const LoadJob& job, const WorldMaterial* materials, u32 materialCount,
                           LoadResult& out) {
	out = LoadResult {};
	out.kind = job.kind;
	out.index = job.index;
	out.status = LOAD_BAD_DATA;
	const PakEntry& e = job.entry;

	if (job.kind == LOAD_TEXTURE) {
		if (e.type != ASSET_TEXTURE || e.size < sizeof(TexHeader)) return;
		u8* blob = static_cast<u8*>(malloc(e.size));
		if (!blob) {
			out.status = LOAD_NO_MEMORY;
			return;
		}
		TexHeader h;
		bool ok = pak.read(e, blob);
		if (ok) {
			memcpy(&h, blob, sizeof(h));
			ok = texValidate(h, e.size - u32(sizeof(TexHeader)));
		}
		if (!ok) {
			free(blob);
			return;
		}
		out.blob = blob;
		out.size = e.size;
		out.status = LOAD_OK;
		return;
	}

	const ChunkInfo& ci = job.info;
	if (e.type != ASSET_CHUNK || e.size != ci.size || e.size < sizeof(ChunkHeader)) return;
	u8* blob = static_cast<u8*>(malloc(e.size));
	if (!blob) {
		out.status = LOAD_NO_MEMORY;
		return;
	}
	bool ok = pak.read(e, blob);
	ChunkHeader h {};
	if (ok) {
		memcpy(&h, blob, sizeof(h));
		ok = h.magic == CHUNK_MAGIC && h.vertexCount == ci.vertexCount && h.indexCount == ci.indexCount &&
		     h.batchCount == ci.batchCount && h.indexCount % 3 == 0 &&
		     e.size == chunkBlobSize(h.batchCount, h.vertexCount, h.indexCount);
	}
	// The blob is laid out so every section is naturally aligned.
	const ChunkBatch* batches = reinterpret_cast<const ChunkBatch*>(blob + sizeof(ChunkHeader));
	const WorldVertex* verts = reinterpret_cast<const WorldVertex*>(batches + h.batchCount);
	const u16* indices = reinterpret_cast<const u16*>(verts + h.vertexCount);
	for (u32 b = 0; ok && b < h.batchCount; ++b) {
		ok = batches[b].material < materialCount && batches[b].firstIndex <= h.indexCount &&
		     batches[b].indexCount <= h.indexCount - batches[b].firstIndex;
	}
	for (u32 i = 0; ok && i < h.indexCount; ++i) ok = indices[i] < h.vertexCount;
	if (!ok) {
		free(blob);
		return;
	}
	// From here on the data is good; what can still fail is memory.
	if (!out.collider.build(h, batches, verts, indices, materials, MATF_NO_COLLISION, 0) ||
	    !out.water.build(h, batches, verts, indices, materials, MATF_WATER, MATF_WATER)) {
		out.collider.release();
		out.water.release();
		free(blob);
		out.status = LOAD_NO_MEMORY;
		return;
	}
	out.blob = blob;
	out.size = e.size;
	out.status = LOAD_OK;
}

bool StreamLoader::start(const char* pakPath, const WorldMaterial* materials, u32 materialCount, bool threaded) {
	stop();
	if (!pakPath || !pakPath[0] || !_pak.open(pakPath)) return false;
	_materials = materials;
	_materialCount = materialCount;
	_jobHead = _jobCount = _resultHead = _resultCount = _outstanding = 0;
	_busy = _quit = false;
	_worstMs = 0;
	if (threaded) {
		_lock = sys::mutexCreate();
		_work = sys::signalCreate();
		_idle = sys::signalCreate();
		if (_lock && _work && _idle) _thread = sys::threadStart(threadMain, this);
		if (!_thread) {
			// No thread to be had: carry on without one.
			sys::mutexDestroy(_lock);
			sys::signalDestroy(_work);
			sys::signalDestroy(_idle);
			_lock = nullptr;
			_work = _idle = nullptr;
		}
	}
	_running = true;
	return true;
}

void StreamLoader::stop() {
	if (!_running) return;
	cancelAll();
	if (_thread) {
		sys::mutexLock(_lock);
		_quit = true;
		sys::mutexUnlock(_lock);
		sys::signalWake(_work);
		sys::threadJoin(_thread);
		_thread = nullptr;
		sys::mutexDestroy(_lock);
		sys::signalDestroy(_work);
		sys::signalDestroy(_idle);
		_lock = nullptr;
		_work = _idle = nullptr;
	}
	_pak.close();
	_materials = nullptr;
	_materialCount = 0;
	_running = false;
}

void StreamLoader::threadMain(void* self) { static_cast<StreamLoader*>(self)->work(); }

void StreamLoader::work() {
	for (;;) {
		sys::mutexLock(_lock);
		while (!_quit && _jobCount == 0) {
			sys::mutexUnlock(_lock);
			sys::signalWait(_work);
			sys::mutexLock(_lock);
		}
		if (_quit) {
			sys::mutexUnlock(_lock);
			return;
		}
		const LoadJob job = _jobs[_jobHead];
		_jobHead = (_jobHead + 1) % CAPACITY;
		--_jobCount;
		_busy = true;
		sys::mutexUnlock(_lock);

		const double t0 = sys::timeMs();
		LoadResult r;
		process(_pak, job, _materials, _materialCount, r);
		const float ms = float(sys::timeMs() - t0);

		sys::mutexLock(_lock);
		// There is always room: submit() admits at most CAPACITY jobs between
		// here and poll().
		_results[(_resultHead + _resultCount) % CAPACITY] = r;
		++_resultCount;
		_busy = false;
		if (ms > _worstMs) _worstMs = ms;
		sys::mutexUnlock(_lock);
		sys::signalWake(_idle);
	}
}

bool StreamLoader::runOne() {
	if (_jobCount == 0) return false;
	const LoadJob job = _jobs[_jobHead];
	_jobHead = (_jobHead + 1) % CAPACITY;
	--_jobCount;
	const double t0 = sys::timeMs();
	LoadResult r;
	process(_pak, job, _materials, _materialCount, r);
	const float ms = float(sys::timeMs() - t0);
	if (ms > _worstMs) _worstMs = ms;
	_results[(_resultHead + _resultCount) % CAPACITY] = r;
	++_resultCount;
	return true;
}

bool StreamLoader::submit(const LoadJob& job) {
	if (!_running || _outstanding >= CAPACITY) return false;
	if (_thread) sys::mutexLock(_lock);
	_jobs[(_jobHead + _jobCount) % CAPACITY] = job;
	++_jobCount;
	if (_thread) {
		sys::mutexUnlock(_lock);
		sys::signalWake(_work);
	}
	++_outstanding;
	return true;
}

bool StreamLoader::peek(LoadKind& kind) {
	if (!_running) return false;
	bool have;
	if (_thread) {
		sys::mutexLock(_lock);
		have = _resultCount > 0;
		if (have) kind = _results[_resultHead].kind;
		sys::mutexUnlock(_lock);
	} else {
		have = _resultCount > 0 || runOne();
		if (have) kind = _results[_resultHead].kind;
	}
	return have;
}

bool StreamLoader::poll(LoadResult& out) {
	if (!_running) return false;
	if (_thread) sys::mutexLock(_lock);
	else if (_resultCount == 0) runOne();
	const bool have = _resultCount > 0;
	if (have) {
		out = _results[_resultHead];
		_resultHead = (_resultHead + 1) % CAPACITY;
		--_resultCount;
	}
	if (_thread) sys::mutexUnlock(_lock);
	if (have) --_outstanding;
	return have;
}

void StreamLoader::cancelAll() {
	if (!_running) return;
	if (_thread) {
		sys::mutexLock(_lock);
		_jobCount = 0;
		_jobHead = 0;
		// The job in progress cannot be interrupted (it is in the middle of a
		// file read); it is a few milliseconds.
		while (_busy) {
			sys::mutexUnlock(_lock);
			sys::signalWait(_idle);
			sys::mutexLock(_lock);
		}
	} else {
		_jobCount = 0;
		_jobHead = 0;
	}
	while (_resultCount > 0) {
		_results[_resultHead].discard();
		_resultHead = (_resultHead + 1) % CAPACITY;
		--_resultCount;
	}
	_resultHead = 0;
	if (_thread) sys::mutexUnlock(_lock);
	_outstanding = 0;
}

float StreamLoader::takeWorstJobMs() {
	if (!_running) return 0.0f;
	if (_thread) sys::mutexLock(_lock);
	const float ms = _worstMs;
	_worstMs = 0;
	if (_thread) sys::mutexUnlock(_lock);
	return ms;
}

} // namespace g3
