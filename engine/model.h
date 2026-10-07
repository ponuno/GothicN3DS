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

private:
	Vec3 motionAt(u32 frame) const;

	ClipHeader _h {};
	u8* _blob = nullptr;
	const u8* _tracks = nullptr;
	const ClipSample* _samples = nullptr;
	const i16* _motion = nullptr;
	float _rise = 0;
};

class Model {
public:
	Model() = default;
	~Model() { release(); }
	Model(const Model&) = delete;
	Model& operator=(const Model&) = delete;

	// Loads "model/<name>" with its textures and all of its clips.
	bool load(Pak& pak, const char* name);
	void release();
	bool loaded() const { return _mesh != nullptr; }

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
	u32 clipCount() const { return _h.clipCount; }
	// The model script's blend of this name ("T_RUNL_2_WALKL"), or null.
	const ModelBlend* blend(u32 nameHash) const;
	u32 blendCount() const { return _h.blendCount; }
	u32 textureBytes() const { return _textureBytes; }

	void restPose(Pose& pose) const;
	// Node transforms in model space for `pose` (`world`, nodeCount entries).
	void worldTransforms(const Pose& pose, Mat34* world) const;

	// Draws the model in `pose`, standing at `origin` and turned by `yaw`
	// (0 faces +Z), lit with `light`. `lean` tips it sideways about its feet
	// (radians, positive to its right). Call in PASS_WORLD.
	void draw(const Pose& pose, Vec3 origin, float yaw, const float light[3], float lean = 0.0f) const;
	u32 triangles() const { return _h.indexCount / 3; }

private:
	ModelHeader _h {};
	ModelNode* _nodes = nullptr;
	ModelPart* _parts = nullptr;
	ModelClipRef* _clipRefs = nullptr;
	ModelBlend* _blends = nullptr;
	Clip* _clips = nullptr;
	gfx::Mesh* _mesh = nullptr;
	gfx::Texture** _textures = nullptr; // per part; shared pointers are freed once
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
	// Playback speed of the current clip; 1 is as authored.
	void setRate(float rate) { _rate = rate; }
	void update(float dt);
	// Where the clips have carried the character since the last call (model
	// space, see Clip::motion), across hand-overs from a clip to its follower.
	Vec3 takeMotion();

	const Pose& pose() const { return _pose; }
	const Clip* current() const { return _clip; }
	float time() const { return _time; }
	// A clip that does not loop has reached its last frame (and has no follower).
	bool finished() const { return _finished; }

private:
	void evaluate();

	const Model* _model = nullptr;
	const Clip* _clip = nullptr;
	float _time = 0, _rate = 1.0f;
	float _fade = 1.0f, _fadeTime = 0; // 0..1 progress of the fade from _from
	bool _finished = false;
	Vec3 _moved {0, 0, 0};
	Pose _pose, _from;
};

} // namespace g3
