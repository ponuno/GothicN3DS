// Streaming world: keeps the chunks around the camera resident, frees the
// rest, and draws what is inside the frustum - opaque batches front to back,
// then translucent ones (water, glow) back to front.
#pragma once
#include "g3ds/math.h"
#include "g3ds/pak.h"
#include "collision.h"
#include "g3ds/world.h"
#include "gfx.h"
#include "loader.h"

namespace g3 {

struct StreamConfig {
	float loadRadius = 14000.0f;   // cm: chunks closer than this get loaded
	float unloadRadius = 18000.0f; // cm: chunks farther than this get freed
	float drawDistance = 12000.0f; // cm: matches the fog; farther chunks are skipped
	// How far each DetailClass is drawn. Small and medium objects (barrels,
	// bushes, fences) are most of the world's triangles but invisible from afar.
	float detailDistance[DETAIL_COUNT] = {1.0e9f, 7000.0f, 3500.0f};
	// Scales the distances of the smaller classes; the frame pacer lowers it
	// when the GPU cannot keep up.
	float detailScale = 1.0f;
	u32 maxLoadsPerFrame = 2;      // chunks handed to the GPU per frame: bounds the upload hitch
	u32 maxTextureLoadsPerFrame = 4; // same for textures, background streaming only
	u32 memoryBudget = 24u << 20;  // bytes of mesh + texture memory for the world
	// Background streaming: reading, unpacking and collision building happen
	// off the frame, and textures arrive a few per frame after their chunk.
	// Set both before open(). Without `worker` the same pipeline runs on the
	// calling thread (no threads needed, but no smoother either).
	bool background = false;
	bool worker = true;
};

struct WorldStats {
	u32 chunksTotal;
	u32 chunksLoaded;
	u32 chunksDrawn;
	u32 chunksPending;   // wanted but not loaded yet
	u32 chunksInFlight;  // of those: being read in the background
	u32 texturesWaiting; // textures of resident chunks that have not arrived yet
	u32 texturesLoaded;
	u32 drawCalls;       // both passes
	u32 blendedDrawCalls;
	u32 batchesCulled;   // batches of visible chunks skipped: out of view or too far for their size
	u32 trianglesDrawn;
	u32 loadsThisFrame;
	u32 budgetRejects;   // loads skipped because the budget was full (cumulative)
	u32 loadFailures;    // corrupt or missing data (cumulative)
	u32 residentBytes;   // mesh + texture bytes owned by the world
	u32 peakResidentBytes;
	u32 collisionBytes;  // CPU-side collision data of the resident chunks
};

class World {
public:
	World() = default;
	~World() { close(); }
	World(const World&) = delete;
	World& operator=(const World&) = delete;

	bool open(Pak* pak, const char* name);
	// `entryHash` is the pak hash of the world asset (what portals store).
	bool open(Pak* pak, u32 entryHash);
	void close();
	bool isOpen() const { return _pak != nullptr; }

	const WorldHeader& header() const { return _hdr; }
	// Chunk table access (statistics, tests).
	const ChunkInfo& chunkInfo(u32 index) const { return _chunks[index]; }
	bool chunkResident(u32 index) const { return _loaded[index].mesh != nullptr; }
	u32 entryHash() const { return _entryHash; }
	StreamConfig& config() { return _cfg; }
	const StreamConfig& config() const { return _cfg; }
	const WorldStats& stats() const { return _stats; }

	// Loads/unloads chunks around the camera. Call once per frame.
	void update(Vec3 camera);
	// Loads everything inside loadRadius immediately (loading screen).
	void preload(Vec3 camera);
	// True if chunks are streamed in the background, and whether a worker
	// thread does it.
	bool backgroundStreaming() const { return _loader.running(); }
	bool workerThread() const { return _loader.threaded(); }
	// The longest a background job took since the last call, ms.
	float takeWorstLoadMs() { return _loader.takeWorstJobMs(); }
	// Opaque geometry; gfx::frameBegin must have been called. `timeMs` drives
	// texture animation and scrolling.
	void draw(const Mat4& viewProj, Vec3 camera, u32 timeMs);
	// Translucent geometry of the chunks draw() found visible. Call after
	// gfx::setPass(PASS_BLEND).
	void drawBlended();

	// Collision against the resident chunks (see collision.h).
	// `light`, if given, receives the baked light (r g b) and the daylight
	// weight of the surface that was hit: what a character standing there is
	// lit with.
	bool rayDown(Vec3 from, float maxDrop, float& hitY, Vec3& normal, u8* light = nullptr) const;
	bool pushSphere(Vec3& center, float radius) const;
	// Highest water surface under `from`, no lower than from.y - maxDrop.
	bool waterBelow(Vec3 from, float maxDrop, float& surfaceY) const;

	// The portal whose box contains `p`, or null.
	const WorldPortal* portalAt(Vec3 p) const;
	// The ladder whose climbing box contains `p`, or null.
	const WorldLadder* ladderAt(Vec3 p) const;
	bool indoor() const { return (_hdr.flags & WORLDF_INDOOR) != 0; }

private:
	struct Batch {
		u16 material;
		u8 detail;
		u32 firstIndex, indexCount;
		float bbMin[3], bbMax[3]; // world space
	};
	struct Loaded {
		gfx::Mesh* mesh;
		Batch* batches;
		float origin[3];
		float posScale, uvScale;
		u32 bytes;
		u32 slot; // position in _resident
		u32 retryAt; // frame before which loading is not attempted again (out of memory)
		u16 batchCount;
		bool failed;    // bad data: never retried
		bool requested; // handed to the background loader
		bool hasBlended;
		ChunkCollider collider;
		ChunkCollider water;
	};
	struct TexSlot {
		gfx::Texture* tex;
		u32 refs;
		u32 bytes;
		bool failed;    // bad data: never retried
		bool waiting;   // wanted, but not there yet
		bool requested; // handed to the background loader
	};

	bool chunkJob(u32 index, LoadJob& job) const;
	bool textureJob(u32 texture, LoadJob& job) const;
	LoadStatus loadChunkNow(u32 index);
	LoadStatus finishChunk(LoadResult& r);
	void loadTextureNow(u32 texture);
	void finishTexture(LoadResult& r);
	void setWaiting(TexSlot& s, bool waiting);
	u32 collectPending(Vec3 camera);
	void sortNearest(u32 n, u32 pending);
	void updateImmediate(u32 pending);
	void updateBackground(u32 pending, Vec3 camera);
	static u32 meshBytes(const ChunkInfo& ci) {
		return u32(ci.vertexCount) * u32(sizeof(WorldVertex)) + ci.indexCount * u32(sizeof(u16));
	}
	void unloadChunk(u32 index);
	void acquireMaterial(u32 material);
	void releaseMaterial(u32 material);
	void acquireTexture(u32 texture);
	void releaseTexture(u32 texture);
	float distanceTo(u32 index, Vec3 camera) const;
	void drawBatches(const Loaded& l, bool blended);

	Pak* _pak = nullptr;
	u32 _entryHash = 0;
	WorldHeader _hdr {};
	WorldMaterial* _materials = nullptr;
	u32* _textures = nullptr; // texture table: pak hashes
	WorldPortal* _portals = nullptr;
	WorldLadder* _ladders = nullptr;
	ChunkInfo* _chunks = nullptr;
	Loaded* _loaded = nullptr; // parallel to _chunks; mesh == nullptr when absent
	TexSlot* _tex = nullptr;   // parallel to _textures
	u32* _resident = nullptr;  // indices of the loaded chunks, unordered
	u32* _order = nullptr;     // scratch: chunk indices sorted for load/draw
	float* _orderKey = nullptr;
	u32 _visible = 0;          // entries of _order filled by the last draw()
	u32 _texturesWaiting = 0;  // texture slots with `waiting` set
	StreamLoader _loader;      // running only with StreamConfig::background
	bool _immediate = false;   // inside preload(): load on the spot even with a loader
	u32 _frame = 0;            // update() counter
	u32 _chunksInFlight = 0, _inFlightBytes = 0;
	u32 _textureRetryAt = 0;
	// Per-frame view state shared by draw() and drawBlended().
	Frustum _frustum {};
	Vec3 _camera {0, 0, 0};
	u32 _timeMs = 0;
	StreamConfig _cfg;
	WorldStats _stats {};
};

} // namespace g3
