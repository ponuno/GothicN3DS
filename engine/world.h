// Streaming world: keeps the chunks around the camera resident, frees the
// rest, and draws what is inside the frustum - opaque batches front to back,
// then translucent ones (water, glow) back to front.
#pragma once
#include "g3ds/math.h"
#include "g3ds/pak.h"
#include "collision.h"
#include "g3ds/world.h"
#include "frame_budget.h"
#include "gfx.h"
#include "loader.h"

namespace g3 {

struct StreamConfig {
	float loadRadius = 14000.0f;   // cm: chunks closer than this get loaded
	float unloadRadius = 18000.0f; // cm: chunks farther than this get freed
	float drawDistance = 12000.0f; // cm: matches the fog; farther chunks are skipped
	float lodDistance = 0.0f;      // cm: chunks wholly farther than this are drawn with their simplified stand-ins (0: never)
	float distanceScale = 1.0f;    // the pacer's share of it actually drawn (the fog follows: App::setDistanceScale)
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
	// The simplified stand-ins and the full chunk dissolve into each other slower than objects do (a swap that near is seen),
	// and the way back to the full chunk waits until it is this share of the distance (hysteresis: no flicker at the edge).
	float lodFadeMs = 900.0f;
	float lodHysteresis = 0.8f;
	// With the far layer: a chunk within this share of the draw distance from its edge (at most edgeFadeMax cm) dissolves
	// out as it nears it, the far layer showing through; 0: chunks and far layer meet at the draw distance, as before.
	float edgeFadeShare = 0.3f;
	float edgeFadeMax = 1500.0f;
	// What the last frame left (platform-set): a chunk or texture is handed over only if it fits, by the cost measured of
	// the ones before. Null: no limit.
	const FrameBudget* budget = nullptr;
	u32 maxLoadsPerFrame = 2;      // chunks handed to the GPU per frame: bounds the upload hitch
	u32 readAheadChunks = 4;       // chunks after those being read whose bytes are read ahead from the card (with a ReadAhead)
	u32 maxInFlight = 2;           // chunks being read at once in the background (nearer wishes wait: what is read is still wanted when it arrives)
	// Worlds baked with WORLDF_TIERS: chunks of medium and small objects (tier 1, 2) are wanted only
	// where those are drawn: out to the class's detailDistance, a tenth farther (detailHysteresis) and
	// `detailLead` beyond that, so that they are there when the camera gets to the distance. They are
	// freed `detailSlack` farther out still. Never farther than loadRadius / unloadRadius.
	float detailLead = 1500.0f;
	float detailSlack = 1200.0f;
	// When memory is short a chunk is as good as a plain chunk this much farther away: the world around
	// the camera is worth more than the objects in it (a hole shows, a missing barrel does not).
	float tierBias[DETAIL_COUNT] = {0.0f, 1500.0f, 2500.0f};
	// Chunks ahead of the way the camera moves count this much nearer, those behind it this much farther
	// (cm), for what is loaded first and what is freed first. 0: only distance counts.
	// Chunks ahead of the way the camera goes are wanted this much farther out (cm, at most; the unload distance is the
	// limit), so that they are there before the camera is. 0: the same distance all round.
	float headLead = 0.0f;
	float aheadBonus = 800.0f;
	float behindPenalty = 800.0f;
	u32 maxTextureLoadsPerFrame = 4; // same for textures, background streaming only
	u32 memoryBudget = 24u << 20;  // bytes of mesh + texture memory for the world (at most)
	// Where the platform can say how much memory for meshes and textures is
	// free right now (the 3DS: the linear heap), the world takes no more than
	// what it holds plus that, less `memoryReserve` for everything else that
	// lives there (characters, particles, sound, the overlay). Null: only
	// memoryBudget counts. See World::budget().
	u32 (*freeMemory)() = nullptr;
	u32 memoryReserve = 6u << 20;
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
	// Graphics+ (gfx::ChunkFx): how much the baked ambient occlusion darkens
	// the light (0..1) and how strongly the plants sway (0 still, 1 as baked,
	// up to 2). Nothing of either in a world baked without it (WORLDF_AO,
	// WORLDF_WIND): hasOcclusion(), hasWind().
	float occlusion = 0.0f;
	float wind = 0.0f;
	// The far layer (G10; a world baked with --far-lod): drawn out to here, cm,
	// where the chunks are not - beyond drawDistance, and in place of chunks
	// not loaded yet. 0 (or no farther than drawDistance): not drawn, and not
	// held in memory either.
	float farDistance = 0.0f;
	// In place of chunks not loaded yet within drawDistance the far layer fills in (true), or the gap is left as it is
	// and the far layer is only beyond drawDistance (false): its flat, untextured colours are not seen close by.
	bool farFillNear = true;
	// Baked visibility (a world baked with --vis; hasVisibility()): what the table says cannot be seen from the
	// camera's cell is not drawn. Held in memory (about a megabyte) only while this is on.
	bool visibility = false;
};

struct WorldStats {
	u32 chunksTotal;
	u32 chunksLoaded;
	u32 chunksDrawn;
	u32 chunksPending;   // wanted but not loaded yet
	u32 pendingDetail;   // of them: chunks of medium and small objects (tier 1, 2)
	u32 loadedDetail;    // of chunksLoaded: tier 1, 2
	u32 detailBytes;     // of residentBytes: their meshes
	u32 chunksInFlight;  // of those: being read in the background
	u32 texturesWaiting; // textures of resident chunks that have not arrived yet
	u32 texturesLoaded;
	u32 drawCalls;       // both passes
	u32 blendedDrawCalls;
	u32 windyDrawCalls;  // of them: plants swaying (the vertex shader does more there)
	u32 batchesCulled;   // batches of visible chunks skipped: out of view or too far for their size
	u32 batchesMerged;   // batches that went out in the draw call of the one before them
	u32 chunksEdgeFading; // chunks drawn dissolving at the draw distance (over the far layer)
	u32 batchesFading;   // batches drawn dissolving in or out (each a draw call of its own)
	u32 trianglesDrawn;
	u32 trisByDetail[3]; // drawn: level geometry and large objects / medium / small (DetailClass)
	u32 trisByDist[3];   // drawn, by the batch's cluster box: nearer than 12 m / 12-25 m / farther
	u32 lodChunks;       // clusters drawn this frame with their simplified stand-ins
	u32 lodTriangles;    // of the drawn triangles: those of the stand-ins
	u32 trianglesInChunks; // all the triangles of the chunks drawn: what drawing whole chunks would cost
	u32 clustersTested;  // boxes of visible chunks looked at ...
	u32 clustersCulled;  // ... and of them wholly out of view or past the fog
	u32 loadsThisFrame;
	u32 budgetRejects;   // chunks the budget had no room for (cumulative; a chunk that keeps waiting counts once)
	u32 allocRejects;    // loads the allocator refused though the budget had room (memory too fragmented; cumulative)
	u32 evictions;       // far chunks freed to make room for nearer ones (cumulative)
	u32 loadFailures;    // corrupt or missing data (cumulative)
	u32 residentBytes;   // mesh + texture bytes owned by the world
	u32 peakResidentBytes;
	u32 budgetBytes;     // the budget of the last update (World::budget())
	u32 collisionBytes;  // CPU-side collision data of the resident chunks
	// The far layer (StreamConfig::farDistance).
	u32 farDrawCalls;    // (also counted in drawCalls)
	u32 farTriangles;    // drawn this frame (not in trianglesDrawn)
	u32 farTiles;        // tiles of it drawn this frame
	u32 farSeams;        // strips along its seams to chunks drawn in full
	u32 farBytes;        // its meshes (0: not loaded)
	// Visibility (StreamConfig::visibility).
	u32 chunksHidden;    // chunks in reach not drawn at all this frame: the table says all of them is hidden
	u32 clustersHidden;  // clusters of the others not drawn for the same reason (counted in the draw pass)
	u32 visBytes;        // the table in memory (0: not loaded)
	bool visRow;         // the camera's cell has a row in the table
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
	// 0: the world and big things (or everything, in a world without WORLDF_TIERS); 1, 2: medium, small objects only.
	u32 chunkTier(u32 index) const { return _tier ? _tier[index] : 0u; }
	bool hasTiers() const { return (_hdr.flags & WORLDF_TIERS) != 0; }
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
	// Chunks about to be wanted are read from the card ahead of time (readahead.h); the loader reads through it too.
	void setReadAhead(class ReadAhead* ahead);
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
	// What moves and is solid (gates, lifts, shut doors: triggers.h) - asked by these queries too; null: nothing.
	void setDynamic(const class Triggers* dynamic) { _dynamic = dynamic; }
	// Highest water surface under `from`, no lower than from.y - maxDrop.
	bool waterBelow(Vec3 from, float maxDrop, float& surfaceY) const;

	// The portal whose box contains `p`, or null.
	const WorldPortal* portalAt(Vec3 p) const;
	// The ladder whose climbing box contains `p`, or null.
	const WorldLadder* ladderAt(Vec3 p) const;
	bool indoor() const { return (_hdr.flags & WORLDF_INDOOR) != 0; }
	// Whether the world was baked with Graphics+ data (g3ds-bake --ao, --wind).
	bool hasOcclusion() const { return (_hdr.flags & WORLDF_AO) != 0; }
	bool hasWind() const { return (_hdr.flags & WORLDF_WIND) != 0; }
	// Whether the world has a far layer (g3ds-bake --far-lod), and whether it
	// is drawn now: loaded and asked to reach beyond the draw distance.
	bool hasFarLayer() const { return _hdr.farEntry != 0 && !indoor(); }
	// How far chunks are drawn now: the draw distance, shortened by the pacer (not with the far layer).
	float drawReach() const { return _farTiles != nullptr && _cfg.farDistance > _cfg.drawDistance ? _cfg.drawDistance : _cfg.drawDistance * _cfg.distanceScale; }
	bool farActive() const { return _farTiles != nullptr && _cfg.farDistance > _cfg.drawDistance; }
	// Whether the world has a visibility table (g3ds-bake --vis), and whether it is used now.
	bool hasVisibility() const { return _hdr.visEntry != 0; }
	bool visibilityActive() const { return _visBlob != nullptr && _cfg.visibility; }

private:
	struct Batch {
		u16 material;
		u8 detail;
		u8 layer; // of the small objects: 0..3, see StreamConfig::smallLayers
		u8 cluster;
		u8 lodMode; // BatchLod: drawn near, far or either way (a chunk with stand-ins; see StreamConfig::lodDistance)
		u8 shown; // smaller classes: how much of it is drawn, 0..255 (dissolving in between; see StreamConfig::fadeMs)
		u32 firstIndex, indexCount;
		float bbMin[3], bbMax[3]; // world space
	};
	// A box of a chunk (see ChunkBatch) with what is known of everything in
	// it: one look at the box settles most of its batches.
	struct Cluster {
		float bbMin[3], bbMax[3];
		bool lod; // drawn with its simplified stand-ins just now (with a little hysteresis)
		u8 lodMix = 0;     // how much of the stand-ins and how little of the full batches is drawn, 0..255: dissolves between the two
		u32 mixFrame = 0;  // the draw() lodMix was last stepped in (classify() runs more than once a frame)
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
		u8 edgeShare = 255;   // 255: in full; less near the draw distance, with the far layer showing through
		bool snapNow = false; // this frame's "shows its objects at once", kept for the second (swaying) pass over the chunk
		u32 windBatches = 0;  // batches of swaying materials the first pass left for the second
		u16 batchCount;
		bool failed;    // bad data: never retried
		bool requested; // handed to the background loader
		bool hasBlended;
		bool hasLod;    // has batches of BATCH_LOD_FAR_ONLY
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
	// How far from the camera a chunk of a tier is wanted / kept (cm).
	float loadReach(u32 tier) const;
	float unloadReach(u32 tier) const;
	// What a chunk at distance `d` counts as for the order of loading and of freeing: distance, plus
	// what its tier is worth less, minus (plus) a little for the way the camera is going.
	float headLeadFor(u32 index, Vec3 camera) const;
	float priority(u32 index, float d, Vec3 camera) const;
	void trackHeading(Vec3 camera);
	void buildGrid();
	void freeGrid();
	void gatherCandidates(Vec3 camera);
	// The resident chunk to free for a wish of priority `forKey` (the one farthest by priority, if beyond it), or -1.
	int pickVictim(Vec3 camera, float forKey) const;
	void countReject(u32 index);
	void sortNearest(u32 n, u32 pending);
	void updateImmediate(u32 pending, Vec3 camera);
	// Frees the farthest loaded chunks (farther than `forDistance`) until `need` more bytes fit the budget; false: they do not.
	bool makeRoom(Vec3 camera, float forDistance, u32 need, u32 reserved);
	// Bytes of mesh and texture memory the world may hold now: memoryBudget,
	// or less where the platform's free memory says so (StreamConfig::freeMemory).
	u32 budget() const;
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
	// phase 0: all batches; 1: the ones that do not sway in the wind; 2: the ones that do (see draw()).
	void drawBatches(Loaded& l, bool blended, int phase = 0);
	// The far layer: read once it is asked for, freed when it is not.
	void loadFar();
	void freeFar();
	void drawFar(const Frustum& view);
	// Whether all of these chunks (of the far layer's chunk list) are drawn in full this frame.
	bool farCovered(u32 firstChunk, u32 count) const;
	float _chunkCostMs = 2.0f, _textureCostMs = 0.8f; // estimates, kept as the loads are timed
	u32 _refusedChunks = 0, _refusedTextures = 0;
	float edgeBand() const { return _cfg.farDistance > _cfg.drawDistance && _cfg.edgeFadeShare > 0.0f && gfx::fadeSupported() ? min(_cfg.drawDistance * _cfg.edgeFadeShare, _cfg.edgeFadeMax) : 0.0f; }
	void classify(const Loaded& l, u32 index, bool count);
public:
	// How far a dissolve gets this frame, 1..255 (255: all of it); worked out by draw(). Characters dissolve by it too.
	int fadeStep() const { return _fadeStep; }
	// Whether what stands at `p` (a torch, a fire place) is drawn just now: its chunk is in memory, within the draw distance,
	// and a small object that near is within its distance. `chunk` is the caller's cache (start it at -2).
	bool pointShown(Vec3 p, Vec3 eye, int& chunk, bool wasOn) const;
private:
	const class Triggers* _dynamic = nullptr;
	void loadVis();
	void freeVis();
	// Looks up the camera's row of the table when the camera has moved to another cell.
	void updateVisibility(Vec3 camera);
	bool visHidden(u32 chunk, u32 cluster) const {
		return _visRow && _visBase[chunk] != 0xFFFFFFFFu && cluster < _visClusters[chunk] &&
		       (_visBits[(_visBase[chunk] + cluster) >> 5] & (1u << ((_visBase[chunk] + cluster) & 31))) == 0;
	}

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
	u8* _tier = nullptr;       // parallel to _chunks (null: all 0)
	u32* _order = nullptr;     // scratch: chunk indices sorted for load/draw
	float* _orderKey = nullptr;
	// A grid over the ground plane, cells of a chunk's size, each holding the chunks whose box reaches
	// into it; chunks that reach into very many cells (sea, mountains) are in `_big` instead. Finding
	// the chunks near the camera looks at a handful of cells, not at all of them.
	u32* _gridStart = nullptr; // per cell (+1): first entry in _gridItems
	u32* _gridItems = nullptr;
	u32* _big = nullptr;
	u32 _bigCount = 0;
	u32* _seen = nullptr;      // per chunk: the gather that last listed it
	u32 _seenStamp = 0;
	int _gridW = 0, _gridH = 0;
	float _gridX0 = 0.0f, _gridZ0 = 0.0f, _gridCell = 1.0f;
	// The chunks that may be wanted soon: within reach of where the camera was when this was made, plus
	// `kCandidateMargin`. Rebuilt when the camera has gone farther than that margin or a reach changed.
	u32* _cand = nullptr;
	u32 _candCount = 0;
	Vec3 _candCam {0, 0, 0};
	float _candReach[DETAIL_COUNT] = {};
	bool _candValid = false;
	// Which way the camera has been going (unit vector on the ground) and how sure that is, 0..1.
	Vec3 _anchor {0, 0, 0};
	u32 _anchorFrame = 0;
	float _headX = 0.0f, _headZ = 0.0f, _headStrength = 0.0f;
	u32 _lastRejected = 0xFFFFFFFFu; // the chunk the budget last turned away: counted once while it waits
	u32 _visible = 0;          // entries of _order filled by the last draw()
	u32 _texturesWaiting = 0;  // texture slots with `waiting` set
	class ReadAhead* _ahead = nullptr;
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
	int _fadeStep = 255;
	int _lodStep = 255;     // the same for the stand-ins' dissolve (lodFadeMs)    // how much of a dissolve one frame does (255: all of it)
	// Graphics+ of this frame (draw()): the occlusion's strength, the wind's
	// push along x and z (gusts included), and the time the wave goes by.
	float _fxOcclusion = 0.0f, _windX = 0.0f, _windZ = 0.0f;
	double _windSeconds = 0.0;
	// The far layer (FarHeader in g3ds/world.h).
	struct FarTileRt {
		FarTile t;
		gfx::Mesh* mesh;
	};
	FarTileRt* _farTiles = nullptr;
	u32 _farTileCount = 0;
	FarRange* _farRanges = nullptr;
	u32 _farRangeCount = 0;
	u32* _farRefs = nullptr;
	u32 _farRefCount = 0;
	u32* _farOrder = nullptr;
	float* _farKey = nullptr;
	u32 _farBytes = 0;
	bool _farTried = false; // read once (and failed, or freed again): not read again until asked anew
	// The visibility table (VisHeader in g3ds/world.h), one blob; the pointers point into it.
	u8* _visBlob = nullptr;
	VisHeader _vis {};
	const i16* _visCellX = nullptr;
	const i16* _visCellZ = nullptr;
	const u8* _visClusters = nullptr;
	const u32* _visOrder = nullptr;
	const VisViewer* _visViewers = nullptr;
	const u32* _visBits = nullptr;
	u32* _visBase = nullptr;   // per chunk: the first bit of its clusters in the camera's row, or 0xFFFFFFFF (not in it)
	u8* _visAllHidden = nullptr; // per chunk: every cluster of it is hidden from the camera's cell
	bool _visTried = false;
	bool _visRow = false;      // the camera's cell has a row and _visBase is filled from it
	bool _visHave = false;     // _visCellNow is the cell the row was looked up for
	int _visCellNowX = 0, _visCellNowZ = 0;
	StreamConfig _cfg;
	WorldStats _stats {};
};

} // namespace g3
