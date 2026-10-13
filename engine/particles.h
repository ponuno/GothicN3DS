// Particle effects: fires, smoke, sparks, the glow of magic. An effect is a
// C_PARTICLEFX instance of the game's scripts (g3ds/pfx.h); the world has
// emitters of its own ("world/<name>/emitters") and the game can start
// one-off effects (play). Only the emitters near the player run; all
// particles share one small pool, and they are drawn as camera-facing quads
// in the blended pass.
#pragma once
#include "g3ds/math.h"
#include "g3ds/pak.h"
#include "g3ds/pfx.h"
#include "gfx.h"
#include "model.h"

namespace g3 {

class Game;

struct ParticleStats {
	u32 emitters = 0;  // in the world
	u32 running = 0;   // near enough to emit
	u32 particles = 0; // alive
	u32 effects = 0;   // definitions read from the scripts
	u32 drawCalls = 0; // in the last frame
};

class Particles {
public:
	static constexpr u32 MAX_PARTICLES = 192;
	static constexpr u32 MAX_RUNNING = 12;      // emitters running at once
	static constexpr u32 MAX_DYNAMIC = 16;      // one-off effects at once
	static constexpr u32 MAX_DEFS = 64;
	static constexpr u32 MAX_FRAMES = 16;
	static constexpr float RANGE = 2500.0f;     // cm from the player in which world emitters run

	Particles() = default;
	~Particles() { release(); }
	Particles(const Particles&) = delete;
	Particles& operator=(const Particles&) = delete;

	// The emitters of the world asset `worldName` ("world/world"); without
	// the archive's table there are none.
	void enterWorld(Pak& pak, const char* worldName, Game& game);
	void leaveWorld();
	void release();

	void update(float dt, Vec3 eye);
	// Fire belongs to the torch that carries it: an emitter only runs while `shown` says its place is drawn
	// (the chunk is in memory and the small objects are within their distance), and it comes in and goes out
	// with the same dissolve (`fadeStep`, World::fadeStep(): 255 = at once). `chunk` is the emitter's own cache of
	// where it stands, `wasOn` what it answered the frame before (for a little hysteresis). Without it every emitter in range runs.
	typedef bool (*ShownFn)(void* user, Vec3 pos, Vec3 eye, int& chunk, bool wasOn);
	void setGate(ShownFn shown, void* user) {
		_shown = shown;
		_shownUser = user;
	}
	void setFadeStep(int step) { _fadeStep = step < 1 ? 1 : step > 255 ? 255 : step; }
	// In PASS_BLEND. `dayLight`: the time of day's colour (smoke is dimmed by it, glow is not).
	void draw(const Mat4& view, Vec3 eye, const float dayLight[3]);

	// Starts the effect (hash of its instance name) at `pos` for `seconds`
	// (0: one puff, a short burst). False if the scripts lack the effect.
	bool play(u32 effect, Vec3 pos, float yaw, float seconds);
	// The same, but the effect can be followed: the handle (never 0) moves it (a spell in flight) or ends it;
	// 0 if the scripts lack the effect. A handle of an effect that has run its time does nothing.
	u32 start(u32 effect, Vec3 pos, float yaw, float seconds);
	void move(u32 handle, Vec3 pos);
	void stop(u32 handle);

	// The engine's own effect for the tail of an arrow (no script has one): a thin glowing streak. Start it with start().
	static u32 arrowTrail();

	const ParticleStats& stats() const { return _stats; }

private:
	enum Shape : u8 { SHAPE_POINT, SHAPE_LINE, SHAPE_BOX, SHAPE_CIRCLE, SHAPE_SPHERE };
	struct Def {
		u32 hash;
		bool ok;
		float pps;
		float ppsKeys[16];
		u8 ppsKeyCount;
		bool ppsLooping, ppsSmooth;
		float ppsFps;
		Shape shape;
		bool volume, objectFrame;
		float dim[3], offset[3];
		float headAngle, headVar, elevAngle, elevVar; // radians
		float velAvg, velVar;     // cm / ms
		float lifeAvg, lifeVar;   // ms
		float gravity[3];         // cm / ms^2
		bool velocityAligned;
		bool additive;
		float aniFps;
		bool aniLooping;
		float colorStart[3], colorEnd[3]; // 0..1
		float sizeStart[2], sizeEndScale;
		float alphaStart, alphaEnd;       // 0..1
		gfx::Texture* frames[MAX_FRAMES];
		u32 frameHash[MAX_FRAMES];
		u8 frameCount;
	};
	struct Particle {
		float pos[3], vel[3];
		float age, life; // ms
		u16 def;
		u8 seed;
	};
	struct Source { // an emitter's running state
		float time = 0, acc = 0;
		int chunk = -2;  // the world chunk it stands in (-2: not looked for yet, -1: in none)
		u8 gate = 0;     // how much it emits, 0..255: it comes in with the object that carries it (see setGate)
	};
	struct Dynamic {
		u16 def;
		float pos[3], yaw;
		float ttl; // s left
		u32 id;    // handle of start() (0: none)
		Source src;
	};
	Dynamic* emplace(u32 effect, Vec3 pos, float yaw, float seconds);

	int defFor(u32 effect);
	void loadDef(Def& d, u32 effect);
	void emit(const Def& d, u16 defIndex, const float pos[3], float yaw, Source& s, float dtMs);
	void spawn(const Def& d, u16 defIndex, const float pos[3], float yaw);
	u32 rnd();
	float frand() { return float(rnd() & 0xFFFFFF) * (1.0f / 16777216.0f); }
	float srand1() { return frand() * 2.0f - 1.0f; }
	void freeMeshes();

	Pak* _pak = nullptr;
	Game* _game = nullptr;
	TexturePool _textures;
	Def _defs[MAX_DEFS];
	u32 _defCount = 0;
	WorldEmitter* _emitters = nullptr;
	u16* _emitterDef = nullptr; // into _defs, 0xFFFF: none
	Source* _sources = nullptr;
	u32 _emitterCount = 0;
	u32 _running[MAX_RUNNING];
	u32 _runningCount = 0;
	Dynamic _dynamic[MAX_DYNAMIC];
	u32 _dynamicCount = 0;
	Particle _particles[MAX_PARTICLES];
	u32 _count = 0;
	u32 _seed = 12345;
	u32 _nextId = 1;
	ParticleStats _stats;
	ShownFn _shown = nullptr;
	void* _shownUser = nullptr;
	int _fadeStep = 255;

	static constexpr u32 MESHES = 3;
	gfx::Mesh* _meshes[MESHES] = {};
	u32 _meshTurn = 0;
};

} // namespace g3
