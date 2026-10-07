// Streaming world: keeps the chunks around the camera resident, frees the
// rest, and draws what is inside the frustum front to back.
#pragma once
#include "g3ds/math.h"
#include "g3ds/pak.h"
#include "g3ds/world.h"
#include "gfx.h"

namespace g3 {

struct StreamConfig {
	float loadRadius = 14000.0f;   // cm: chunks closer than this get loaded
	float unloadRadius = 18000.0f; // cm: chunks farther than this get freed
	float drawDistance = 12000.0f; // cm: matches the fog; farther chunks are skipped
	u32 maxLoadsPerFrame = 2;      // bounds the per-frame I/O + upload hitch
	u32 memoryBudget = 24u << 20;  // bytes of mesh + texture memory for the world
};

struct WorldStats {
	u32 chunksTotal;
	u32 chunksLoaded;
	u32 chunksDrawn;
	u32 chunksPending;   // wanted but not loaded yet
	u32 texturesLoaded;
	u32 drawCalls;
	u32 trianglesDrawn;
	u32 loadsThisFrame;
	u32 budgetRejects;   // loads skipped because the budget was full (cumulative)
	u32 loadFailures;    // corrupt or missing data (cumulative)
	u32 residentBytes;   // mesh + texture bytes owned by the world
	u32 peakResidentBytes;
};

class World {
public:
	World() = default;
	~World() { close(); }
	World(const World&) = delete;
	World& operator=(const World&) = delete;

	bool open(Pak* pak, const char* name);
	void close();

	const WorldHeader& header() const { return _hdr; }
	StreamConfig& config() { return _cfg; }
	const WorldStats& stats() const { return _stats; }

	// Loads/unloads chunks around the camera. Call once per frame.
	void update(Vec3 camera);
	// Loads everything inside loadRadius immediately (loading screen).
	void preload(Vec3 camera);
	// Issues draw calls; gfx::frameBegin must have been called.
	void draw(const Mat4& viewProj, Vec3 camera);

private:
	struct Loaded {
		gfx::Mesh* mesh;
		ChunkBatch* batches;
		float origin[3];
		float posScale, uvScale;
		u32 bytes;
		u16 batchCount;
		bool failed; // load failed once; never retried
	};
	struct TexSlot {
		gfx::Texture* tex;
		u32 refs;
		u32 bytes;
		bool failed;
	};

	bool loadChunk(u32 index);
	void unloadChunk(u32 index);
	gfx::Texture* acquireTexture(u32 material);
	void releaseTexture(u32 material);
	float distanceTo(u32 index, Vec3 camera) const;

	Pak* _pak = nullptr;
	WorldHeader _hdr {};
	WorldMaterial* _materials = nullptr;
	ChunkInfo* _chunks = nullptr;
	Loaded* _loaded = nullptr; // parallel to _chunks; mesh == nullptr when absent
	TexSlot* _tex = nullptr;   // parallel to _materials
	u32* _order = nullptr;     // scratch: chunk indices sorted for load/draw
	float* _orderKey = nullptr;
	StreamConfig _cfg;
	WorldStats _stats {};
};

} // namespace g3
