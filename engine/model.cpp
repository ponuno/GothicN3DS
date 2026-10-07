#include "model.h"

#include "g3ds/tex.h"
#include "g3ds/world.h"

#include <stdlib.h>
#include <string.h>

namespace g3 {

// ---- Clip ----

bool Clip::load(Pak& pak, u32 entryHash, u32 nodeCount) {
	release();
	const PakEntry* e = pak.find(entryHash);
	if (!e || e->type != ASSET_CLIP || e->size < sizeof(ClipHeader)) return false;
	u8* blob = static_cast<u8*>(pak.readAlloc(*e));
	if (!blob) return false;
	memcpy(&_h, blob, sizeof(_h));
	bool ok = _h.magic == CLIP_MAGIC && _h.version == CLIP_VERSION && _h.trackCount > 0 && _h.frameCount > 0 &&
	          _h.fps > 0.0f && _h.trackCount <= MODEL_MAX_NODES && e->size == clipBlobSize(_h.trackCount, _h.frameCount);
	const u8* tracks = blob + sizeof(ClipHeader);
	for (u32 t = 0; ok && t < _h.trackCount; ++t) ok = tracks[t] < nodeCount;
	if (!ok) {
		free(blob);
		_h = {};
		return false;
	}
	_blob = blob;
	_tracks = tracks;
	// 2-byte aligned: the header and the padded track table are multiples of 4.
	_samples = reinterpret_cast<const ClipSample*>(tracks + clipTrackTableSize(_h.trackCount));
	return true;
}

void Clip::release() {
	free(_blob);
	_blob = nullptr;
	_tracks = nullptr;
	_samples = nullptr;
	_h = {};
}

float Clip::duration() const {
	if (!_blob) return 0.0f;
	// A loop's last frame leads back into its first.
	const u32 steps = loops() ? _h.frameCount : max<u32>(_h.frameCount, 2) - 1;
	return float(steps) / _h.fps;
}

void Clip::sample(float time, Pose& pose) const {
	if (!_blob) return;
	const u32 frames = _h.frameCount;
	float f = time * _h.fps;
	u32 i0, i1;
	float t;
	if (loops()) {
		f = fmodf(f, float(frames));
		if (f < 0) f += float(frames);
		i0 = min(u32(f), frames - 1);
		i1 = (i0 + 1) % frames;
		t = f - float(i0);
	} else {
		f = clamp(f, 0.0f, float(frames - 1));
		i0 = min(u32(f), frames - 1);
		i1 = min(i0 + 1, frames - 1);
		t = f - float(i0);
	}
	const ClipSample* a = _samples + size_t(i0) * _h.trackCount;
	const ClipSample* b = _samples + size_t(i1) * _h.trackCount;
	const float ps = _h.posScale;
	for (u32 k = 0; k < _h.trackCount; ++k) {
		const u32 node = _tracks[k];
		const Vec3 pa = {float(a[k].pos[0]) * ps, float(a[k].pos[1]) * ps, float(a[k].pos[2]) * ps};
		const Vec3 pb = {float(b[k].pos[0]) * ps, float(b[k].pos[1]) * ps, float(b[k].pos[2]) * ps};
		pose.pos[node] = pa + (pb - pa) * t;
		const Quat qa = {float(a[k].rot[0]), float(a[k].rot[1]), float(a[k].rot[2]), float(a[k].rot[3])};
		const Quat qb = {float(b[k].rot[0]), float(b[k].rot[1]), float(b[k].rot[2]), float(b[k].rot[3])};
		pose.rot[node] = quatNlerp(qa, qb, t); // normalises: the 1/32767 scale drops out
	}
}

// ---- Model ----

bool Model::load(Pak& pak, const char* name) {
	release();
	char path[96];
	snprintf(path, sizeof(path), "model/%s", name);
	const PakEntry* e = pak.find(path);
	if (!e || e->type != ASSET_MODEL || e->size < sizeof(ModelHeader)) return false;
	u8* blob = static_cast<u8*>(pak.readAlloc(*e));
	if (!blob) return false;

	memcpy(&_h, blob, sizeof(_h));
	bool ok = _h.magic == MODEL_MAGIC && _h.version == MODEL_VERSION && _h.nodeCount > 0 &&
	          _h.nodeCount <= MODEL_MAX_NODES && _h.vertexCount > 0 && _h.vertexCount <= 65535 && _h.indexCount % 3 == 0 &&
	          e->size == modelBlobSize(_h);
	const u8* p = blob + sizeof(ModelHeader);
	if (ok) {
		_nodes = static_cast<ModelNode*>(malloc(size_t(_h.nodeCount) * sizeof(ModelNode)));
		_parts = static_cast<ModelPart*>(malloc(size_t(_h.partCount) * sizeof(ModelPart) + 1));
		_clipRefs = static_cast<ModelClipRef*>(malloc(size_t(_h.clipCount) * sizeof(ModelClipRef) + 1));
		// Zeroed memory is a valid, empty Clip (and nothing here needs operator new).
		_clips = static_cast<Clip*>(calloc(size_t(_h.clipCount) + 1, sizeof(Clip)));
		_textures = static_cast<gfx::Texture**>(calloc(size_t(_h.partCount) + 1, sizeof(gfx::Texture*)));
		ok = _nodes && _parts && _clipRefs && _clips && _textures;
	}
	const WorldVertex* vertices = nullptr;
	const u16* indices = nullptr;
	if (ok) {
		memcpy(_nodes, p, size_t(_h.nodeCount) * sizeof(ModelNode));
		p += size_t(_h.nodeCount) * sizeof(ModelNode);
		memcpy(_parts, p, size_t(_h.partCount) * sizeof(ModelPart));
		p += size_t(_h.partCount) * sizeof(ModelPart);
		memcpy(_clipRefs, p, size_t(_h.clipCount) * sizeof(ModelClipRef));
		p += size_t(_h.clipCount) * sizeof(ModelClipRef);
		vertices = reinterpret_cast<const WorldVertex*>(p); // 4-byte aligned: every table before is
		indices = reinterpret_cast<const u16*>(vertices + _h.vertexCount);
		// Parents come first, parts stay inside the buffers, palettes inside the skeleton.
		for (u32 i = 0; ok && i < _h.nodeCount; ++i) ok = _nodes[i].parent < i16(i) && _nodes[i].parent >= -1;
		for (u32 i = 0; ok && i < _h.partCount; ++i) {
			const ModelPart& part = _parts[i];
			ok = part.boneCount > 0 && part.boneCount <= MODEL_PALETTE && part.firstIndex <= _h.indexCount &&
			     part.indexCount <= _h.indexCount - part.firstIndex;
			for (u32 b = 0; ok && b < part.boneCount; ++b) ok = part.bones[b] < _h.nodeCount;
		}
		for (u32 i = 0; ok && i < _h.indexCount; ++i) ok = indices[i] < _h.vertexCount;
		// A vertex must not point past its part's palette.
		for (u32 i = 0; ok && i < _h.partCount; ++i)
			for (u32 k = 0; ok && k < _parts[i].indexCount; ++k) {
				const WorldVertex& v = vertices[indices[_parts[i].firstIndex + k]];
				ok = v.r % 3 == 0 && v.g % 3 == 0 && u32(v.r) / 3 < _parts[i].boneCount && u32(v.g) / 3 < _parts[i].boneCount;
			}
	}
	if (ok) {
		_mesh = gfx::meshCreate(vertices, _h.vertexCount, indices, _h.indexCount);
		ok = _mesh != nullptr;
	}
	free(blob);
	if (!ok) {
		release();
		return false;
	}

	// Textures: one per distinct hash. A missing or broken one leaves its
	// parts untextured rather than the model unusable.
	for (u32 i = 0; i < _h.partCount; ++i) {
		if (!_parts[i].texture) continue;
		for (u32 k = 0; k < i && !_textures[i]; ++k)
			if (_parts[k].texture == _parts[i].texture) _textures[i] = _textures[k];
		if (_textures[i]) continue;
		const PakEntry* te = pak.find(_parts[i].texture);
		if (!te || te->type != ASSET_TEXTURE || te->size < sizeof(TexHeader)) continue;
		u8* tb = static_cast<u8*>(pak.readAlloc(*te));
		if (!tb) continue;
		TexHeader th;
		memcpy(&th, tb, sizeof(th));
		if (texValidate(th, te->size - u32(sizeof(TexHeader)))) {
			_textures[i] = gfx::textureCreate(th, tb + sizeof(TexHeader));
			if (_textures[i]) _textureBytes += th.dataSize;
		}
		free(tb);
	}
	// Clips: a broken one is simply not there.
	for (u32 i = 0; i < _h.clipCount; ++i) _clips[i].load(pak, _clipRefs[i].entryHash, _h.nodeCount);
	return true;
}

void Model::release() {
	if (_textures) {
		for (u32 i = 0; i < _h.partCount; ++i) {
			if (!_textures[i]) continue;
			gfx::Texture* t = _textures[i];
			for (u32 k = i; k < _h.partCount; ++k)
				if (_textures[k] == t) _textures[k] = nullptr;
			gfx::textureDestroy(t);
		}
	}
	if (_mesh) gfx::meshDestroy(_mesh);
	if (_clips)
		for (u32 i = 0; i < _h.clipCount; ++i) _clips[i].release();
	free(_clips);
	free(_textures);
	free(_nodes);
	free(_parts);
	free(_clipRefs);
	_clips = nullptr;
	_textures = nullptr;
	_nodes = nullptr;
	_parts = nullptr;
	_clipRefs = nullptr;
	_mesh = nullptr;
	_textureBytes = 0;
	_h = {};
}

int Model::findNode(u32 nameHash) const {
	for (u32 i = 0; i < _h.nodeCount; ++i)
		if (_nodes[i].nameHash == nameHash) return int(i);
	return -1;
}

const Clip* Model::clip(u32 nameHash) const {
	for (u32 i = 0; i < _h.clipCount; ++i)
		if (_clipRefs[i].nameHash == nameHash) return _clips[i].loaded() ? &_clips[i] : nullptr;
	return nullptr;
}

void Model::restPose(Pose& pose) const {
	for (u32 i = 0; i < _h.nodeCount; ++i) {
		const ModelNode& n = _nodes[i];
		pose.pos[i] = {n.restPos[0], n.restPos[1], n.restPos[2]};
		pose.rot[i] = {n.restRot[0], n.restRot[1], n.restRot[2], n.restRot[3]};
	}
}

void Model::worldTransforms(const Pose& pose, Mat34* world) const {
	for (u32 i = 0; i < _h.nodeCount; ++i) {
		const Mat34 local = mat34From(pose.rot[i], pose.pos[i]);
		const int parent = _nodes[i].parent;
		world[i] = parent < 0 ? local : mul(world[parent], local);
	}
}

// Scratch space of Model::draw: 9 KB that are better not on the console's
// 32 KB stack. Drawing happens on one thread.
static Mat34 gDrawWorld[MODEL_MAX_NODES], gDrawSkin[MODEL_MAX_NODES];

void Model::draw(const Pose& pose, Vec3 origin, float yaw, const float light[3]) const {
	if (!_mesh) return;
	Mat34* const world = gDrawWorld;
	Mat34* const skin = gDrawSkin;
	worldTransforms(pose, world);
	// Skin matrices: rest position -> posed position, then turned to face `yaw`.
	const Mat34 turn = mat34From(quatFromYaw(yaw), {0, 0, 0});
	for (u32 i = 0; i < _h.nodeCount; ++i) {
		Mat34 inv;
		memcpy(inv.m, _nodes[i].invBind, sizeof(inv.m));
		skin[i] = mul(turn, mul(world[i], inv));
	}
	float palette[MODEL_PALETTE * 12];
	gfx::SkinDraw sd {};
	sd.bones = palette;
	sd.origin[0] = origin.x;
	sd.origin[1] = origin.y;
	sd.origin[2] = origin.z;
	sd.posScale = _h.posScale;
	sd.uvScale = _h.uvScale;
	memcpy(sd.light, light, sizeof(sd.light));
	for (u32 i = 0; i < _h.partCount; ++i) {
		const ModelPart& part = _parts[i];
		if (!part.indexCount) continue;
		for (u32 b = 0; b < part.boneCount; ++b) memcpy(palette + b * 12, skin[part.bones[b]].m, 12 * sizeof(float));
		sd.boneCount = part.boneCount;
		gfx::setMaterial(_textures[i], part.flags);
		gfx::drawSkinned(_mesh, part.firstIndex, part.indexCount, sd);
	}
}

// ---- Animator ----

void Animator::bind(const Model* model) {
	_model = model;
	_clip = nullptr;
	_time = 0;
	_rate = 1.0f;
	_fade = 1.0f;
	_fadeTime = 0;
	_finished = false;
	if (model && model->loaded()) {
		model->restPose(_pose);
		_from = _pose;
	}
}

void Animator::play(const Clip* clip, float blend) {
	if (!_model || clip == _clip) return;
	_from = _pose; // what is on screen now is where the fade starts
	_clip = clip;
	_time = 0;
	_rate = 1.0f;
	_finished = false;
	_fadeTime = blend >= 0.0f ? blend : (clip ? clip->header().blendIn : 0.15f);
	_fade = _fadeTime > 0.0f ? 0.0f : 1.0f;
	evaluate();
}

void Animator::evaluate() {
	if (!_model || !_model->loaded()) return;
	Pose target;
	_model->restPose(target); // nodes the clip does not animate keep their rest transform
	if (_clip) _clip->sample(_time, target);
	if (_fade >= 1.0f) {
		_pose = target;
		return;
	}
	const float t = _fade * _fade * (3.0f - 2.0f * _fade);
	for (u32 i = 0; i < _model->nodeCount(); ++i) {
		_pose.pos[i] = _from.pos[i] + (target.pos[i] - _from.pos[i]) * t;
		_pose.rot[i] = quatNlerp(_from.rot[i], target.rot[i], t);
	}
}

void Animator::update(float dt) {
	if (!_model) return;
	if (_fade < 1.0f) _fade = _fadeTime > 0.0f ? min(_fade + dt / _fadeTime, 1.0f) : 1.0f;
	if (_clip) {
		_time += dt * _rate;
		if (!_clip->loops() && _time >= _clip->duration()) {
			// Over: on to the clip the model script names as the next one.
			const Clip* next = _clip->header().next ? _model->clip(_clip->header().next) : nullptr;
			if (next && next != _clip) {
				const float left = _time - _clip->duration();
				play(next, _clip->header().blendOut);
				_time = left;
			} else {
				_time = _clip->duration();
				_finished = true;
			}
		} else if (_clip->loops()) {
			const float d = _clip->duration();
			if (d > 0.0f && _time >= d * 64.0f) _time = fmodf(_time, d); // keep the float precise on long stays
		}
	}
	evaluate();
}

} // namespace g3
