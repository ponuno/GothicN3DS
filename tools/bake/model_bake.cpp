#include "model_bake.h"
#include "vcache.h"

#include "g3ds/math.h"
#include "g3ds/model.h"
#include "g3ds/world.h"
#include "texture_bake.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace g3 {

namespace {

std::string format(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string format(const char* fmt, ...) {
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

std::string lowerName(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
	return s;
}

template <class T> void append(std::vector<u8>& out, const T& item) {
	const u8* p = reinterpret_cast<const u8*>(&item);
	out.insert(out.end(), p, p + sizeof(T));
}

Quat quatOf(const float r[4]) { return quatNormalize({r[0], r[1], r[2], r[3]}); }

// Light from above and a little from the front: enough to read the shape of
// a body without a light per character.
u8 shadeOf(const float n[3]) {
	const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	if (len < 1e-6f) return 220;
	const float d = (n[0] * 0.25f + n[1] * 0.85f + n[2] * 0.45f) / len; // roughly unit light direction
	const float s = 0.62f + 0.38f * std::clamp(0.5f + 0.5f * d, 0.0f, 1.0f);
	return u8(std::lround(s * 255.0f));
}

struct VertexKey {
	WorldVertex v;
	bool operator<(const VertexKey& o) const { return std::memcmp(&v, &o.v, sizeof(v)) < 0; }
};

} // namespace

ModelBakeResult bakeModel(const SrcModel& model, const BakeOptions& opt, PakWriter& pak, ModelBakeShared* shared) {
	ModelBakeResult result;
	auto line = [&](const std::string& s) {
		result.report += s + "\n";
		if (!opt.quiet) std::puts(s.c_str());
	};
	if (model.name.empty()) throw std::runtime_error("a model has no name");
	const size_t n = model.nodes.size();
	if (n == 0 || n > MODEL_MAX_NODES)
		throw std::runtime_error("model '" + model.name + "': " + std::to_string(n) + " nodes (1.." +
		                         std::to_string(MODEL_MAX_NODES) + " supported)");
	line("== model '" + model.name + "' ==");
	for (const std::string& note : model.notes) line(note);

	// ---- skeleton: parents first ----
	std::vector<int> order, newIndex(n, -1);
	for (size_t pass = 0; pass < n && order.size() < n; ++pass)
		for (size_t i = 0; i < n; ++i) {
			if (newIndex[i] >= 0) continue;
			const int p = model.nodes[i].parent;
			if (p >= int(n)) throw std::runtime_error("model '" + model.name + "': node parent out of range");
			if (p >= 0 && newIndex[size_t(p)] < 0) continue;
			newIndex[i] = int(order.size());
			order.push_back(int(i));
		}
	if (order.size() != n) throw std::runtime_error("model '" + model.name + "': the node hierarchy has a cycle");

	std::vector<ModelNode> nodes(n);
	std::vector<Mat34> restWorld(n);
	for (size_t k = 0; k < n; ++k) {
		const SrcNode& src = model.nodes[size_t(order[k])];
		ModelNode& dst = nodes[k];
		dst = ModelNode {};
		dst.parent = i16(src.parent < 0 ? -1 : newIndex[size_t(src.parent)]);
		dst.nameHash = hashName(src.name.c_str());
		const Quat q = quatOf(src.rot);
		std::memcpy(dst.restPos, src.pos, sizeof(dst.restPos));
		dst.restRot[0] = q.x;
		dst.restRot[1] = q.y;
		dst.restRot[2] = q.z;
		dst.restRot[3] = q.w;
		const Mat34 local = mat34From(q, {src.pos[0], src.pos[1], src.pos[2]});
		restWorld[k] = dst.parent < 0 ? local : mul(restWorld[size_t(dst.parent)], local);
		const Mat34 inv = invertRigid(restWorld[k]);
		std::memcpy(dst.invBind, inv.m, sizeof(dst.invBind));
	}

	// ---- textures ----
	std::map<std::string, u32> textureHash; // source name -> pak hash (0: missing)
	std::map<std::string, bool> textureAlpha;
	std::vector<std::string> missing;
	for (const SrcModelPart& part : model.parts) {
		const std::string& tex = part.material.texture;
		if (tex.empty() || textureHash.count(tex)) continue;
		const std::string name = texturePakName(tex);
		// Another model of this bake may have baked it already.
		if (shared) {
			const auto known = shared->textures.find(tex);
			if (known != shared->textures.end()) {
				textureHash[tex] = known->second.hash;
				textureAlpha[tex] = known->second.alpha;
				if (!known->second.hash) missing.push_back(tex);
				continue;
			}
		}
		SrcImage img;
		if (!model.loadTexture || !model.loadTexture(tex, img)) {
			textureHash[tex] = 0;
			missing.push_back(tex);
			if (shared) shared->textures[tex] = ModelBakeShared::Texture {};
			continue;
		}
		TexBakeInfo info;
		const std::vector<u8> data = bakeTexture(img, opt.tex, &info);
		// A world may have brought the same texture along already.
		if (!pak.has(name)) {
			pak.add(name, ASSET_TEXTURE, data);
			++result.textures;
		}
		textureHash[tex] = hashName(name.c_str());
		textureAlpha[tex] = info.alpha;
		if (shared) shared->textures[tex] = ModelBakeShared::Texture {textureHash[tex], info.alpha};
	}
	result.missingTextures = missing;

	// ---- --simplify: fewer triangles (parts cut out of a texture stay) ----
	std::vector<SrcModelPart> simplifiedParts;
	const std::vector<SrcModelPart>* srcParts = &model.parts;
	if (opt.simplifyModels.enabled && !model.parts.empty()) {
		simplifiedParts = model.parts;
		std::vector<bool> skip(simplifiedParts.size(), false);
		for (size_t i = 0; i < simplifiedParts.size(); ++i) {
			const std::string& tex = simplifiedParts[i].material.texture;
			skip[i] = !tex.empty() && textureAlpha[tex];
		}
		ModelSimplifyStats ss;
		simplifyModelParts(simplifiedParts, skip, opt.simplifyModels, ss);
		if (ss.simplified) {
			line("simplified (--simplify): " + std::to_string(ss.simplified) + " of " + std::to_string(ss.parts) + " parts, " +
			     std::to_string(ss.before) + " -> " + std::to_string(ss.after) + " triangles");
			srcParts = &simplifiedParts;
		}
	}

	// ---- mesh: bounds, then parts by texture and bone palette ----
	float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
	size_t triangles = 0;
	for (const SrcModelPart& part : (*srcParts)) {
		if (part.verts.size() % 3) throw std::runtime_error("model '" + model.name + "': a part is not a triangle list");
		triangles += part.verts.size() / 3;
		for (const SrcSkinVertex& v : part.verts) {
			for (int b : v.bone)
				if (b < 0 || size_t(b) >= n) throw std::runtime_error("model '" + model.name + "': vertex bone out of range");
			for (int a = 0; a < 3; ++a) {
				mn[a] = std::min(mn[a], v.pos[a]);
				mx[a] = std::max(mx[a], v.pos[a]);
			}
		}
	}
	// No mesh at all is a skeleton with its animations; a mesh without
	// triangles is a mistake.
	if (triangles == 0 && !(*srcParts).empty()) throw std::runtime_error("model '" + model.name + "' has no triangles");
	if (triangles == 0) {
		if (model.clips.empty()) throw std::runtime_error("model '" + model.name + "' has neither mesh nor animations");
		for (int a = 0; a < 3; ++a) mn[a] = mx[a] = 0.0f;
	}
	float reach = 1.0f;
	for (int a = 0; a < 3; ++a) reach = std::max({reach, std::fabs(mn[a]), std::fabs(mx[a])});
	float posScale = 1.0f / 256.0f; // a power of two, like the world's
	while (reach / posScale > 32000.0f) posScale *= 2.0f;

	std::vector<ModelPart> parts;
	std::vector<WorldVertex> vertices;
	std::vector<u16> indices;
	u64 uvClamped = 0, blended = 0;
	// Source parts that draw the same way are one group; a group is cut
	// wherever its triangles need more bones than one draw call can address.
	std::map<std::pair<u32, u16>, std::vector<const SrcModelPart*>> groups;
	for (const SrcModelPart& part : (*srcParts)) {
		const u32 tex = part.material.texture.empty() ? 0 : textureHash[part.material.texture];
		u16 flags = 0;
		if (tex == 0) flags |= MATF_NO_TEXTURE;
		if (tex && textureAlpha[part.material.texture]) flags |= MATF_ALPHA_TEST;
		groups[{tex, flags}].push_back(&part);
	}
	for (const auto& group : groups) {
		// Triangles sorted by their lowest bone: neighbours in the skeleton end
		// up in the same palette.
		struct Tri {
			const SrcSkinVertex* v;
			int low;
		};
		std::vector<Tri> tris;
		for (const SrcModelPart* part : group.second)
			for (size_t i = 0; i + 2 < part->verts.size(); i += 3) {
				int low = 1 << 30;
				for (size_t k = 0; k < 3; ++k)
					for (int b : part->verts[i + k].bone) low = std::min(low, newIndex[size_t(b)]);
				tris.push_back({&part->verts[i], low});
			}
		std::stable_sort(tris.begin(), tris.end(), [](const Tri& a, const Tri& b) { return a.low < b.low; });

		ModelPart cur {};
		std::map<int, int> slotOf; // node -> palette slot
		std::map<VertexKey, u16> lookup;
		auto begin = [&]() {
			cur = ModelPart {};
			cur.texture = group.first.first;
			cur.flags = group.first.second;
			cur.firstIndex = u32(indices.size());
			slotOf.clear();
			lookup.clear();
		};
		auto flush = [&]() {
			if (cur.indexCount == 0) return;
			cur.boneCount = u16(slotOf.size());
			for (const auto& kv : slotOf) cur.bones[kv.second] = u8(kv.first);
			result.largestPalette = std::max(result.largestPalette, slotOf.size());
			parts.push_back(cur);
		};
		begin();
		for (const Tri& t : tris) {
			std::set<int> need;
			for (size_t k = 0; k < 3; ++k) {
				need.insert(newIndex[size_t(t.v[k].bone[0])]);
				if (t.v[k].weight1 > 0.002f) need.insert(newIndex[size_t(t.v[k].bone[1])]);
			}
			size_t fresh = 0;
			for (int b : need) fresh += !slotOf.count(b);
			if (slotOf.size() + fresh > MODEL_PALETTE || vertices.size() + 3 > 65535) {
				if (vertices.size() + 3 > 65535)
					throw std::runtime_error("model '" + model.name + "': more than 65535 vertices");
				flush();
				begin();
			}
			for (int b : need)
				if (!slotOf.count(b)) {
					const int slot = int(slotOf.size());
					slotOf[b] = slot;
				}
			for (size_t k = 0; k < 3; ++k) {
				const SrcSkinVertex& sv = t.v[k];
				WorldVertex v {};
				v.x = i16(std::lround(sv.pos[0] / posScale));
				v.y = i16(std::lround(sv.pos[1] / posScale));
				v.z = i16(std::lround(sv.pos[2] / posScale));
				v.w = 1;
				const float su = sv.uv[0] / WORLD_UV_SCALE, sw = sv.uv[1] / WORLD_UV_SCALE;
				if (std::fabs(su) > 32767.0f || std::fabs(sw) > 32767.0f) ++uvClamped;
				v.u = i16(std::lround(std::clamp(su, -32767.0f, 32767.0f)));
				v.v = i16(std::lround(std::clamp(sw, -32767.0f, 32767.0f)));
				const int b0 = newIndex[size_t(sv.bone[0])];
				const bool two = sv.weight1 > 0.002f;
				const int b1 = two ? newIndex[size_t(sv.bone[1])] : b0;
				v.r = u8(slotOf[b0] * 3);
				v.g = u8(slotOf[b1] * 3);
				v.b = two ? u8(std::lround(std::clamp(sv.weight1, 0.0f, 1.0f) * 255.0f)) : u8(0);
				v.a = shadeOf(sv.normal);
				blended += two;
				auto ins = lookup.emplace(VertexKey {v}, u16(vertices.size()));
				if (ins.second) vertices.push_back(v);
				indices.push_back(ins.first->second);
			}
			cur.indexCount += 3;
		}
		flush();
	}

	// The triangles of every part in the order the GPU's vertex cache likes (they were sorted by their bones,
	// which scatters neighbours): the same triangles, the same winding, the same range of each part.
	u64 cacheBefore = 0, cacheAfter = 0;
	for (const ModelPart& p : parts) {
		if (p.firstIndex + p.indexCount > indices.size()) continue;
		u16* idx = indices.data() + p.firstIndex;
		cacheBefore += countCacheMisses(idx, p.indexCount, 16);
		optimizeIndexRange(idx, p.indexCount, vertices.size(), 16);
		cacheAfter += countCacheMisses(idx, p.indexCount, 16);
	}

	// ---- clips ----
	std::vector<ModelClipRef> clipRefs;
	std::vector<std::string> clipNotes;
	std::set<u32> clipNames;
	size_t clipBytes = 0;
	for (const SrcClip& clip : model.clips) {
		const size_t tracks = clip.nodes.size();
		if (clip.name.empty() || tracks == 0 || clip.samples.empty() || clip.samples.size() % tracks != 0 || !(clip.fps > 0))
			throw std::runtime_error("model '" + model.name + "': clip '" + clip.name + "' is malformed");
		const size_t frames = clip.samples.size() / tracks;
		if (frames > 65535) throw std::runtime_error("model '" + model.name + "': clip '" + clip.name + "' is too long");
		if (!clipNames.insert(hashName(clip.name.c_str())).second) continue; // the first of a name wins

		std::vector<SrcClipSample> samples = clip.samples;
		const bool loop = lowerName(clip.next) == lowerName(clip.name);
		// Root motion (see g3ds/model.h): what the root does since the first
		// frame is where the clip carries the character; the root itself is
		// drawn over the character's position.
		//
		// The game flags the clips that move the character ("M"); the others
		// it draws with the root where the animation puts it, off the
		// character's own position. Of those the steps into and out of a
		// bench or a chair matter: "T_BENCH_STAND_2_S0" takes the body half a
		// metre back onto the seat, and "S_BENCH_S1" sits there. Since every
		// root is drawn over the character's position here, such a step has
		// to carry the character instead (as OpenGothic does with any clip
		// whose root travels) - else everybody sits down in front of the
		// bench, in the air.
		bool carries = clip.moves;
		if (!carries && !loop)
			for (size_t t = 0; t < tracks && !carries; ++t) {
				const int node = clip.nodes[t];
				if (node < 0 || size_t(node) >= n || model.nodes[size_t(node)].parent >= 0) continue;
				for (size_t f = 1; f < frames && !carries; ++f) {
					const float dx = clip.samples[f * tracks + t].pos[0] - clip.samples[t].pos[0];
					const float dz = clip.samples[f * tracks + t].pos[2] - clip.samples[t].pos[2];
					carries = dx * dx + dz * dz > 2.0f * 2.0f;
				}
			}
		std::vector<std::array<float, 3>> motion;
		float motionReach = 0, rise = 0;
		for (size_t t = 0; t < tracks; ++t) {
			const int node = clip.nodes[t];
			if (node < 0 || size_t(node) >= n) throw std::runtime_error("model '" + model.name + "': clip node out of range");
			if (model.nodes[size_t(node)].parent >= 0) continue;
			if (carries && motion.empty()) {
				motion.resize(frames);
				for (size_t f = 0; f < frames; ++f) {
					for (int a = 0; a < 3; ++a) motion[f][size_t(a)] = clip.samples[f * tracks + t].pos[a] - clip.samples[t].pos[a];
					for (float c : motion[f]) motionReach = std::max(motionReach, std::fabs(c));
					rise = std::max(rise, motion[f][1]);
				}
			}
			for (size_t f = 0; f < frames; ++f) {
				samples[f * tracks + t].pos[0] = 0;
				samples[f * tracks + t].pos[2] = 0;
			}
		}
		// (ZenGin flags standing animations as "moving" too; a root that
		// stays where it is carries nobody anywhere.)
		if (motionReach < 0.4f) motion.clear();
		float travelled = 0;
		if (!motion.empty()) travelled = std::sqrt(motion.back()[0] * motion.back()[0] + motion.back()[2] * motion.back()[2]);
		// A loop's last frame leads back into its first, so it lasts `frames`
		// steps; anything else lasts `frames - 1`.
		const float duration = float(loop ? frames : std::max<size_t>(frames - 1, 1)) / clip.fps;
		const float travelSteps = float(std::max<size_t>(frames - 1, 1)) / clip.fps;

		float posReach = 1.0f;
		for (const SrcClipSample& s : samples)
			for (float c : s.pos) posReach = std::max(posReach, std::fabs(c));
		ClipHeader h {};
		h.magic = CLIP_MAGIC;
		h.version = CLIP_VERSION;
		h.trackCount = u16(tracks);
		h.frameCount = u16(frames);
		h.fps = clip.fps;
		const bool fight = clip.fighting();
		// The sound tags, in the order of their frames; what does not fit the block is left out.
		std::vector<ClipSfxEvent> sfxEvents;
		for (const SrcClip::Sfx& e : clip.sfx) {
			if (e.name.empty() || sfxEvents.size() >= CLIP_SFX_MAX) continue;
			ClipSfxEvent ev {};
			ev.frame = u16(std::clamp(e.frame, 0, int(frames) - 1));
			ev.kind = e.ground ? CLIP_SFX_GROUND : CLIP_SFX_PLAIN;
			ev.flags = e.emptySlot ? 1 : 0;
			ev.range = e.range > 0.0f ? e.range : 0.0f;
			std::strncpy(ev.name, e.name.c_str(), CLIP_SFX_NAME - 1);
			sfxEvents.push_back(ev);
		}
		std::stable_sort(sfxEvents.begin(), sfxEvents.end(), [](const ClipSfxEvent& a, const ClipSfxEvent& b) { return a.frame < b.frame; });
		h.flags = u16((loop ? CLIPF_LOOP : 0) | (!motion.empty() ? CLIPF_MOVES : 0) | (clip.fly ? CLIPF_FLY : 0) | (fight ? CLIPF_FIGHT : 0) | (!sfxEvents.empty() ? CLIPF_SOUND : 0));
		h.layer = u16(std::clamp(clip.layer, 0, 65535));
		h.blendIn = std::clamp(clip.blendIn, 0.0f, 5.0f);
		h.blendOut = std::clamp(clip.blendOut, 0.0f, 5.0f);
		h.next = clip.next.empty() ? 0 : hashName(clip.next.c_str());
		h.moveSpeed = travelled > 5.0f ? travelled / travelSteps : 0.0f;
		h.posScale = posReach / 32767.0f;
		h.motionScale = std::max(motionReach, 1.0f) / 32767.0f;

		std::vector<u8> blob;
		append(blob, h);
		for (size_t t = 0; t < tracks; ++t) blob.push_back(u8(newIndex[size_t(clip.nodes[t])]));
		while ((blob.size() - sizeof(h)) % 4) blob.push_back(0);
		for (const SrcClipSample& s : samples) {
			ClipSample out;
			for (int a = 0; a < 3; ++a) out.pos[a] = i16(std::lround(s.pos[a] / h.posScale));
			const Quat q = quatOf(s.rot);
			const float r[4] = {q.x, q.y, q.z, q.w};
			for (int a = 0; a < 4; ++a) out.rot[a] = i16(std::lround(r[a] * 32767.0f));
			append(blob, out);
		}
		while (blob.size() % 4) blob.push_back(0);
		for (const auto& m : motion)
			for (float c : m) append(blob, i16(std::lround(c / h.motionScale)));
		while (blob.size() % 4) blob.push_back(0);
		if (fight) {
			ClipFight cf;
			std::memset(&cf, 0xFF, sizeof(cf));
			cf.pad = 0;
			cf.reserved = 0;
			cf.reserved2 = 0;
			const auto frame = [&](int f) { return u16(std::clamp(f, 0, int(frames) - 1)); };
			cf.hits = u8(std::min<size_t>(clip.hits.size(), CLIP_FIGHT_BLOWS));
			for (u32 i = 0; i < cf.hits; ++i) cf.hit[i] = frame(clip.hits[i]);
			cf.hitEnds = u8(std::min<size_t>(clip.hitEnds.size(), CLIP_FIGHT_BLOWS));
			for (u32 i = 0; i < cf.hitEnds; ++i) cf.hitEnd[i] = frame(clip.hitEnds[i]);
			cf.windows = u8(std::min<size_t>(clip.windows.size() / 2, CLIP_FIGHT_BLOWS));
			for (u32 i = 0; i < cf.windows * 2u; ++i) cf.window[i] = frame(clip.windows[i]);
			if (clip.parry.size() >= 2) {
				cf.parry[0] = frame(clip.parry[0]);
				cf.parry[1] = frame(clip.parry[1]);
			}
			if (clip.fightModeFrame >= 0) cf.fightMode = frame(clip.fightModeFrame);
			append(blob, cf);
		}
		if (blob.size() != clipBlobSize(h.trackCount, h.frameCount, !motion.empty(), fight)) throw std::logic_error("clip layout");
		if (!sfxEvents.empty()) {
			append(blob, u32(sfxEvents.size()));
			for (const ClipSfxEvent& e : sfxEvents) append(blob, e);
		}
		const std::string entry = "anim/" + lowerName(model.name) + "/" + lowerName(clip.name);
		pak.add(entry, ASSET_CLIP, blob);
		clipBytes += blob.size();
		clipRefs.push_back({hashName(clip.name.c_str()), hashName(entry.c_str())});
		std::string fightNote;
		if (!clip.hits.empty()) fightNote += format(" %zu blow%s", clip.hits.size(), clip.hits.size() == 1 ? "" : "s");
		if (clip.parry.size() >= 2) fightNote += " parry";
		clipNotes.push_back(format("%s %.2fs%s%s%s%s", clip.name.c_str(), double(duration), loop ? " loop" : "",
		                           h.moveSpeed > 0 ? format(" %.0fcm/s", double(h.moveSpeed)).c_str() : "",
		                           rise > 20.0f ? format(" up %.0fcm", double(rise)).c_str() : "", fightNote.c_str()));
	}

	// ---- blends ----
	std::vector<ModelBlend> blends;
	std::string blendList;
	for (const SrcAniBlend& b : model.blends) {
		if (b.name.empty() || b.next.empty()) continue;
		const u32 name = hashName(b.name.c_str());
		if (clipNames.count(name)) continue; // an animation of that name wins
		bool seen = false;
		for (const ModelBlend& e : blends) seen = seen || e.nameHash == name;
		if (seen) continue;
		ModelBlend e {};
		e.nameHash = name;
		e.nextHash = hashName(b.next.c_str());
		e.time = std::clamp(std::max(b.blendIn, b.blendOut), 0.0f, 5.0f);
		blends.push_back(e);
		blendList += (blendList.empty() ? "" : ", ") + format("%s %.2fs", b.name.c_str(), double(e.time));
	}

	// ---- the model itself ----
	ModelHeader mh {};
	mh.magic = MODEL_MAGIC;
	mh.version = MODEL_VERSION;
	mh.nodeCount = u16(n);
	mh.partCount = u16(parts.size());
	mh.clipCount = u16(clipRefs.size());
	mh.blendCount = u32(blends.size());
	mh.vertexCount = u32(vertices.size());
	mh.indexCount = u32(indices.size());
	mh.posScale = posScale;
	mh.uvScale = WORLD_UV_SCALE;
	std::memcpy(mh.bbMin, mn, sizeof(mn));
	std::memcpy(mh.bbMax, mx, sizeof(mx));
	std::vector<u8> blob;
	append(blob, mh);
	for (const ModelNode& node : nodes) append(blob, node);
	for (const ModelPart& part : parts) append(blob, part);
	for (const ModelClipRef& ref : clipRefs) append(blob, ref);
	for (const ModelBlend& e : blends) append(blob, e);
	for (const WorldVertex& v : vertices) append(blob, v);
	for (u16 i : indices) append(blob, i);
	while (blob.size() % 4) blob.push_back(0);
	if (blob.size() != modelBlobSize(mh)) throw std::logic_error("model layout");
	pak.add("model/" + lowerName(model.name), ASSET_MODEL, blob);

	result.parts = parts.size();
	result.vertices = vertices.size();
	result.triangles = indices.size() / 3;
	result.clips = clipRefs.size();
	line(format("skeleton: %zu nodes; mesh: %zu triangles, %zu vertices (%.0f%% follow two bones), %zu draw calls, "
	            "largest palette %zu of %u bones; vertex shader runs per triangle %.2f -> %.2f (16-entry FIFO)",
	            n, result.triangles, result.vertices, vertices.empty() ? 0.0 : 100.0 * double(blended) / double(triangles * 3),
	            parts.size(), result.largestPalette, MODEL_PALETTE, result.triangles ? double(cacheBefore) / double(result.triangles) : 0.0,
	            result.triangles ? double(cacheAfter) / double(result.triangles) : 0.0));
	line(format("size: %.0f x %.0f x %.0f cm, feet at y = %.1f, position step %.4f cm", double(mx[0] - mn[0]),
	            double(mx[1] - mn[1]), double(mx[2] - mn[2]), double(mn[1]), double(posScale)));
	{
		std::string list;
		for (size_t i = 0; i < clipNotes.size(); ++i) list += (i ? ", " : "") + clipNotes[i];
		line(format("clips: %zu (%.0f KB): ", clipRefs.size(), double(clipBytes) / 1024.0) + list);
	}
	if (!blends.empty()) line(format("blends: %zu: ", blends.size()) + blendList);
	if (!missing.empty()) {
		std::string list;
		for (size_t i = 0; i < missing.size(); ++i) list += (i ? ", " : "") + missing[i];
		line("WARNING: model textures not found (drawn untextured): " + list);
	}
	if (uvClamped) line(format("WARNING: %llu texture coordinates out of range", static_cast<unsigned long long>(uvClamped)));
	return result;
}

} // namespace g3
