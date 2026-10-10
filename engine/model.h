// Animated characters: a skeleton with a skinned mesh (Model), sampled
// animation (Clip) and the thing that plays clips on a skeleton (Animator).
// Formats: g3ds/model.h.
#pragma once
#include "g3ds/math.h"
#include "g3ds/model.h"
#include "g3ds/pak.h"
#include "gfx.h"

namespace g3 {

// A skeleton's node transforms relative to their parents.
struct Pose {
	Vec3 pos[MODEL_MAX_NODES];
	Quat rot[MODEL_MAX_NODES];
};

class Clip {
public:
	Clip() = default;
	~Clip() { release(); }
	Clip(const Clip&) = delete;
	Clip& operator=(const Clip&) = delete;

	bool load(Pak& pak, u32 entryHash, u32 nodeCount);
	void release();
	bool loaded() const { return _blob != nullptr; }
	u32 bytes() const { return _bytes; }

	const ClipHeader& header() const { return _h; }
	bool loops() const { return (_h.flags & CLIPF_LOOP) != 0; }
	// Seconds one pass takes at normal speed.
	float duration() const;
	// Writes the animated nodes' transforms at `time` (seconds; wrapped for a
	// loop, clamped otherwise) into `pose`. Other nodes are left alone.
	void sample(float time, Pose& pose) const;

	// Root motion: where the clip has carried the character `time` seconds in,
	// in model space (+Z ahead), relative to where it started. A loop keeps
	// going: every pass adds its travel. Zero for a clip that stays in place.
	bool moves() const { return _motion != nullptr; }
	Vec3 motion(float time) const;
	// The highest the clip lifts the character above where it started, cm.
	float rise() const { return _rise; }
	// The frames that matter in a fight (g3ds/model.h), or null.
	const ClipFight* fight() const { return _fight; }
	// Seconds into the clip of a frame of it.
	float frameTime(u32 frame) const { return float(frame) / _h.fps; }

private:
	Vec3 motionAt(u32 frame) const;

	ClipHeader _h {};
	u8* _blob = nullptr;
	const u8* _tracks = nullptr;
	const ClipSample* _samples = nullptr;
	const i16* _motion = nullptr;
	const ClipFight* _fight = nullptr;
	float _rise = 0;
	u32 _bytes = 0;
};

// Textures several models share: each is loaded once and freed when the last
// model using it goes.
class TexturePool {
public:
	TexturePool() = default;
	~TexturePool() { release(); }
	TexturePool(const TexturePool&) = delete;
	TexturePool& operator=(const TexturePool&) = delete;

	// The texture stored under this pak entry; null if it is missing, broken
	// or the pool is full. Every acquire needs its drop.
	gfx::Texture* acquire(Pak& pak, u32 entryHash);
	void drop(gfx::Texture* texture);
	void release();
	u32 count() const { return _count; }
	u32 bytes() const { return _bytes; }

private:
	enum { CAPACITY = 96 };
	struct Slot {
		u32 hash;
		u32 users;
		u32 bytes;
		gfx::Texture* texture;
	};
	Slot _slots[CAPACITY] = {};
	u32 _count = 0, _bytes = 0;
};

class Model {
public:
	Model() = default;
	~Model() { release(); }
	Model(const Model&) = delete;
	Model& operator=(const Model&) = delete;

	// Loads "model/<name>" with its textures and all of its clips.
	bool load(Pak& pak, const char* name);
	// The same by pak entry; with a pool, textures are shared through it (and
	// the pool must outlive the model). A model may be a skeleton with clips
	// and no mesh: it draws nothing, but poses made on it fit every model with
	// the same skeleton.
	// With `lazyClips` no clip is read now: each is read when fetch() first
	// asks for it (a model of a world's people has hundreds, of which a
	// place needs a few).
	bool loadEntry(Pak& pak, u32 entryHash, TexturePool* pool = nullptr, bool lazyClips = false);
	void release();
	bool loaded() const { return _nodes != nullptr; }
	bool hasMesh() const { return _mesh != nullptr; }
	// True if a pose of `other` can be drawn with this model.
	bool sameSkeleton(const Model& other) const;

	const ModelHeader& header() const { return _h; }
	u32 nodeCount() const { return _h.nodeCount; }
	const ModelNode& node(u32 i) const { return _nodes[i]; }
	u32 partCount() const { return _h.partCount; }
	const ModelPart& part(u32 i) const { return _parts[i]; }
	// Node with this name hash, or -1.
	int findNode(u32 nameHash) const;
	// The clip with this name hash (hashName("S_RUNL")), or null.
	const Clip* clip(u32 nameHash) const;
	const Clip* clip(const char* name) const { return clip(hashName(name)); }
	// The same, reading the clip from the archive if it has not been yet -
	// and the clips that follow it, so that it can be played through. Null if
	// the model has none of that name, it cannot be read, or reading it would
	// take the clips in memory past `budget` bytes (`inMemory` is that count,
	// kept by the caller across its models).
	const Clip* fetch(Pak& pak, u32 nameHash, u32& inMemory, u32 budget);
	bool hasClip(u32 nameHash) const;
	// Bytes of the clips that are in memory.
	u32 clipBytes() const;
	u32 clipCount() const { return _h.clipCount; }
	// The i-th clip, or null if it could not be loaded.
	const Clip* clipAt(u32 i) const { return i < _h.clipCount && _clips[i].loaded() ? &_clips[i] : nullptr; }
	// The model script's blend of this name ("T_RUNL_2_WALKL"), or null.
	const ModelBlend* blend(u32 nameHash) const;
	u32 blendCount() const { return _h.blendCount; }
	u32 textureBytes() const { return _textureBytes; }

	void restPose(Pose& pose) const;
	// Turns a node of `pose` (a head) by `yaw` about the model's up axis and
	// then by `pitch` (positive: down) about the axis across where it then
	// looks - in model space, whatever way the node's own axes point. The
	// way it looks is reckoned from where the root faces.
	void turnNode(Pose& pose, int node, float yaw, float pitch) const;
	// Node transforms in model space for `pose` (`world`, nodeCount entries).
	void worldTransforms(const Pose& pose, Mat34* world) const;

	// Draws the model in `pose`, standing at `origin` and turned by `yaw`
	// (0 faces +Z), lit with `light`. `lean` tips it sideways about its feet
	// (radians, positive to its right); `scale` (x, y, z), if given, stretches
	// it about its feet. Call in PASS_WORLD.
	void draw(const Pose& pose, Vec3 origin, float yaw, const float light[3], float lean = 0.0f,
	          const float* scale = nullptr) const;
	// The two halves of draw(), for several models on one skeleton (a body
	// and its head): the bone matrices are worked out once - `skin` gets
	// nodeCount of them - and each model is drawn with them.
	void skinMatrices(const Pose& pose, float yaw, float lean, const float* scale, Mat34* skin) const;
	void drawSkinned(const Mat34* skin, Vec3 origin, const float light[3]) const;
	u32 triangles() const { return _h.indexCount / 3; }

private:
	ModelHeader _h {};
	ModelNode* _nodes = nullptr;
	ModelPart* _parts = nullptr;
	ModelClipRef* _clipRefs = nullptr;
	ModelBlend* _blends = nullptr;
	Clip* _clips = nullptr;
	u8* _clipTried = nullptr; // per clip: reading it has been tried
	gfx::Mesh* _mesh = nullptr;
	gfx::Texture** _textures = nullptr; // per part; shared pointers are freed once
	TexturePool* _pool = nullptr;       // where the textures come from, if not our own
	u32 _textureBytes = 0;
};

// Plays clips on a model's skeleton, fading from one to the next.
class Animator {
public:
	void bind(const Model* model);
	// Makes `clip` the current one. Nothing happens if it already is. The
	// pose fades over `blend` seconds (negative: what the clip itself asks).
	void play(const Clip* clip, float blend = -1.0f);
	// Starts the current clip over (a step that is taken again).
	void restart(float blend);
	// Jumps to `time` in the current clip (a combo going on with its next
	// blow from where the last one ends). What the root travels meanwhile
	// counts as moved.
	void seek(float time);
	// Playback speed of the current clip; 1 is as authored.
	void setRate(float rate) { _rate = rate; }
	void update(float dt) {
		advance(dt);
		evaluate();
	}
	// update() in two halves, for a character nobody sees: time and motion
	// move on with advance(), and the pose is only worked out - evaluate() -
	// when it is needed.
	void advance(float dt);
	void evaluate();
	// Where the clips have carried the character since the last call (model
	// space, see Clip::motion), across hand-overs from a clip to its follower.
	Vec3 takeMotion();

	const Pose& pose() const { return _pose; }
	const Clip* current() const { return _clip; }
	float time() const { return _time; }
	// A clip that does not loop has reached its last frame (and has no follower).
	bool finished() const { return _finished; }

private:
	const Model* _model = nullptr;
	const Clip* _clip = nullptr;
	float _time = 0, _rate = 1.0f;
	float _fade = 1.0f, _fadeTime = 0; // 0..1 progress of the fade from _from
	bool _finished = false;
	Vec3 _moved {0, 0, 0};
	Pose _pose, _from;
	Pose _base; // what the nodes without a track in the clip keep (see play())
};

// When blow `index` of an attack clip lands, seconds: its event frame
// (DEF_OPT_FRAME), or - a clip without fight events, from an archive baked
// before them - a single blow at 45% of it. Negative: there is no such blow.
inline float clipBlowTime(const Clip& c, u32 index) {
	const ClipFight* f = c.fight();
	if (!f) return index == 0 ? c.duration() * 0.45f : -1.0f;
	return index < f->hits && f->hit[index] != CLIP_NO_FRAME && c.header().fps > 0.0f ? float(f->hit[index]) / c.header().fps : -1.0f;
}

} // namespace g3
