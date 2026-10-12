// Background loading. Reading a chunk from the SD card, unpacking it, checking
// it and building its collision grids takes several frames' worth of time on
// the console; done on the frame's own thread that is a visible hitch every
// time the player crosses a chunk border. The loader does all of it on a
// worker thread and hands back finished results; the main thread is left with
// the one step that needs the GPU - copying the mesh or the texture into
// video memory.
//
// The loader never touches the GPU and owns its own handle to the archive,
// so the two threads share nothing but the two small queues in here.
#pragma once
#include "collision.h"
#include "g3ds/pak.h"
#include "g3ds/world.h"
#include "sys.h"

namespace g3 {

enum LoadKind : u8 { LOAD_CHUNK, LOAD_TEXTURE };

enum LoadStatus : u8 {
	LOAD_OK,
	LOAD_BAD_DATA,  // missing or corrupt: do not try again
	LOAD_NO_MEMORY, // try again later
};

struct LoadJob {
	LoadKind kind;
	u32 index;      // chunk or texture index: handed back with the result
	PakEntry entry; // where the data is
	ChunkInfo info; // chunks: what the world index says the blob contains
};

struct LoadResult {
	LoadKind kind;
	LoadStatus status;
	u32 index;
	// LOAD_OK only. Chunk: the checked blob (ChunkHeader ...). Texture: the
	// checked blob (TexHeader + mip data). malloc'ed.
	u8* blob;
	u32 size;
	// Chunks: the finished collision grids (solid world, water surfaces).
	ChunkCollider collider;
	ChunkCollider water;

	// Frees whatever the result still owns.
	void discard();
};

class StreamLoader {
public:
	// Jobs that may be under way at once, finished ones not yet collected
	// included.
	static constexpr u32 CAPACITY = 8;

	StreamLoader() = default;
	~StreamLoader() { stop(); }
	StreamLoader(const StreamLoader&) = delete;
	StreamLoader& operator=(const StreamLoader&) = delete;

	// `materials` must stay valid until stop(). Without `threaded`, or if no
	// thread can be started, the same queue is worked off inside peek() and
	// poll() on the caller's thread. False if the archive cannot be opened.
	// The archive the worker reads through (null: straight from the card). Before start().
	void setReadAhead(class ReadAhead* ahead) { _ahead = ahead; }
	bool start(const char* pakPath, const WorldMaterial* materials, u32 materialCount, bool threaded);
	// Finishes the job in progress, drops everything else. Safe to call twice.
	void stop();
	bool running() const { return _running; }
	bool threaded() const { return _thread != nullptr; }

	// False if CAPACITY jobs are already under way.
	bool submit(const LoadJob& job);
	// Is a finished result waiting, and of which kind? (So the caller can
	// leave it there if it has done enough uploads for this frame.)
	bool peek(LoadKind& kind);
	// Takes the oldest finished result. The caller owns it: use it, then
	// discard() it.
	bool poll(LoadResult& out);
	// Submitted and not yet collected with poll().
	u32 outstanding() const { return _outstanding; }
	// Forgets every job: queued ones are dropped, the one in progress is
	// waited for, finished results are freed.
	void cancelAll();
	// The longest a single job took since the last call, ms.
	float takeWorstJobMs();

	// The work itself, usable without a loader (loading screens): reads,
	// unpacks and checks one asset and, for a chunk, builds its colliders.
	static void process(Pak& pak, const LoadJob& job, const WorldMaterial* materials, u32 materialCount,
	                    LoadResult& out);

private:
	static void threadMain(void* self);
	void work();
	bool runOne(); // unthreaded: one queued job, on this thread

	class ReadAhead* _ahead = nullptr;
	Pak _pak; // the worker's own file handle and unpack buffer
	const WorldMaterial* _materials = nullptr;
	u32 _materialCount = 0;

	LoadJob _jobs[CAPACITY];
	u32 _jobHead = 0, _jobCount = 0;
	LoadResult _results[CAPACITY];
	u32 _resultHead = 0, _resultCount = 0;

	sys::Thread* _thread = nullptr;
	sys::Mutex* _lock = nullptr;   // guards the queues, _busy, _quit, _worstMs
	sys::Signal* _work = nullptr;  // main -> worker: there is a job (or it is time to quit)
	sys::Signal* _idle = nullptr;  // worker -> main: a job is finished
	bool _busy = false;            // the worker is inside process()
	bool _quit = false;
	bool _running = false;
	u32 _outstanding = 0; // main thread only
	float _worstMs = 0;
};

} // namespace g3
