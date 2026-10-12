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
	// How many of the four thinning layers of the small objects are drawn (4: all; the rest of the
	// bushes, stones and litter is left out for a sparser, lighter picture).
	u8 smallLayers = 4;
	// What is drawn goes this much farther out than it comes in (1.1: 10%
	// farther), so that an object at its distance does not come and go with
	// every step and every sway of the camera.
	float detailHysteresis = 1.1f;
	// Objects coming into their distance are dissolved in over this long,
	// and dissolved out again when they leave it (see gfx::setFade); 0: they
	// appear and vanish at once, as before. Off-screen they change at once.
	float fadeMs = 400.0f;
	u32 maxLoadsPerFrame = 2;      // chunks handed to the GPU per frame: bounds the upload hitch
	u32 maxTextureLoadsPerFrame = 4; // same for textures, background streaming only
	u32 memoryBudget = 24u << 20;  // bytes of mesh + texture memory for the world
	// Background streaming: reading, unpacking and collision building happen
	// off the frame, and textures arrive a few per frame after their chunk.
	// Set both before open(). Without `worker` the same pipeline runs on the
	// calling thread (no threads needed, but no smoother either).
	bool background = false;
	bool worker = true;
	// Of a chunk in view only the clusters in view and this side of the fog's
	// end are drawn. Off: the whole chunk, as before there were clusters (for
	// comparing).
	bool clusterCulling = true;
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
	u32 batchesMerged;   // batches that went out in the draw call of the one before them
	u32 batchesFading;   // batches drawn dissolving in or out (each a draw call of its own)
	u32 trianglesDrawn;
	u32 trianglesInChunks; // all the triangles of the chunks drawn: what drawing whole chunks would cost
	u32 clustersTested;  // boxes of visible chunks looked at ...
	u32 clustersCulled;  // ... and of them wholly out of view or past the fog
	u32 loadsThisFrame;
	u32 budgetRejects;   // loads skipped because the budget was full (cumulative)
	u32 evictions;       // far chunks freed to make room for nearer ones (cumulative)
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
	// `group`, if given, receives the material group of the surface (what a step on it sounds like).
	bool rayDown(Vec3 from, float maxDrop, float& hitY, Vec3& normal, u8* light = nullptr, u8* group = nullptr) const;
	bool pushSphere(Vec3& center, float radius) const;
	// True if nothing solid reaches into the box (a quick "all clear": false
	// only says that something may be there).
	bool boxFree(const float mn[3], const float mx[3]) const;
	// True if nothing that collides comes within `radius` of the straight
	// line from `a` to `b` (a sphere stepped along it): who stands at `a`
	// sees `b`. Only loaded chunks count.
	bool lineFree(Vec3 a, Vec3 b, float radius = 8.0f) const;
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
		u8 layer; // of the small objects: 0..3, see StreamConfig::smallLayers
		u8 cluster;
		u8 shown; // smaller classes: how much of it is drawn, 0..255 (dissolving in between; see StreamConfig::fadeMs)
		u32 firstIndex, indexCount;
		float bbMin[3], bbMax[3]; // world space
	};
	// A box of a chunk (see ChunkBatch) with what is known of everything in
	// it: one look at the box settles most of its batches.
	struct Cluster {
		float bbMin[3], bbMax[3];
	};
	struct Loaded {
		gfx::Mesh* mesh;
		Batch* batches;
		Cluster* clusters;
		u16 clusterCount;
		float origin[3];
		float posScale, uvScale;
		u32 bytes;
		u32 triangles;
		u32 slot; // position in _resident
		u32 retryAt; // frame before which loading is not attempted again (out of memory)
		u32 drawnFrame; // the last draw() that drew it: one that did not draw it the frame before shows its objects at once
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
	void updateImmediate(u32 pending, Vec3 camera);
	// Frees the farthest loaded chunks (farther than `forDistance`) until `need` more bytes fit the budget; false: they do not.
	bool makeRoom(Vec3 camera, float forDistance, u32 need, u32 reserved);
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
	void drawBatches(Loaded& l, bool blended);
	void classify(const Loaded& l, bool count);

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
	// Per cluster of the chunk being drawn: CLUSTER_OUT, CLUSTER_IN (all of
	// it is in view) or CLUSTER_PARTLY (its batches are looked at one by one).
	enum : u8 { CLUSTER_OUT, CLUSTER_IN, CLUSTER_PARTLY };
	u8 _clusterState[256] = {};
	float _clusterNear[256] = {}, _clusterFar[256] = {}; // distance of its box from the camera: nearest and farthest point
	u32 _timeMs = 0;
	u32 _drawFrame = 1;     // draw() counter
	u32 _lastDrawMs = 0;
	int _fadeStep = 255;    // how much of a dissolve one frame does (255: all of it)
	StreamConfig _cfg;
	WorldStats _stats {};
};

} // namespace g3
