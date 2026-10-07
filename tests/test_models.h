// Test characters shared by the test programs.
#pragma once
#include "g3ds/math.h"
#include "model_src.h"

#include <cmath>

namespace g3 {

// A snake standing on its tail: `count` nodes in a row, each turned a little
// against its parent, with a strip of quads between neighbours. More bones
// than one draw call can address, so the baker has to cut it into parts, and
// every vertex at a joint follows two bones. One clip ("S_BEND", not a loop)
// curls it up; a second ("T_NOD") leads into it.
inline void chainRest(int count, int node, float pos[3], float rot[4]) {
	(void)count;
	pos[0] = 0;
	pos[1] = node == 0 ? 10.0f : 7.0f;
	pos[2] = 0;
	// Alternating twists about Z and X: no node's axes are the model's.
	const float a = (node % 2 ? 0.11f : -0.07f) * 0.5f;
	const Quat q = node % 3 == 0 ? Quat {std::sin(a), 0, 0, std::cos(a)} : Quat {0, 0, std::sin(a), std::cos(a)};
	rot[0] = q.x;
	rot[1] = q.y;
	rot[2] = q.z;
	rot[3] = q.w;
}

inline void buildChainModel(SrcModel& model, int count) {
	model = SrcModel {};
	model.name = "chain";
	std::vector<Mat34> world(static_cast<size_t>(count));
	for (int i = 0; i < count; ++i) {
		SrcNode n;
		n.name = "LINK" + std::to_string(i);
		n.parent = i - 1;
		chainRest(count, i, n.pos, n.rot);
		const Mat34 local = mat34From({n.rot[0], n.rot[1], n.rot[2], n.rot[3]}, {n.pos[0], n.pos[1], n.pos[2]});
		world[size_t(i)] = i == 0 ? local : mul(world[size_t(i - 1)], local);
		model.nodes.push_back(n);
	}
	SrcModelPart part;
	part.material.name = "CHAIN";
	part.material.texture = "CHAIN_SKIN.TGA";
	model.loadTexture = [](const std::string& name, SrcImage& out) {
		if (name != "CHAIN_SKIN.TGA") return false;
		out.width = out.height = 8;
		out.rgba.assign(8 * 8 * 4, 255);
		for (size_t i = 0; i < 64; ++i) out.rgba[i * 4] = u8(120 + (i % 8) * 10);
		return true;
	};
	auto corner = [&](int node, float side, int other, float weight) {
		SrcSkinVertex v {};
		const Vec3 p = transformPoint(world[size_t(node)], {side * 4.0f, 0, 0});
		v.pos[0] = p.x;
		v.pos[1] = p.y;
		v.pos[2] = p.z;
		v.normal[0] = 0;
		v.normal[1] = 0;
		v.normal[2] = -1;
		v.uv[0] = side > 0 ? 1.0f : 0.0f;
		v.uv[1] = float(node) * 0.25f;
		v.bone[0] = node;
		v.bone[1] = other;
		v.weight1 = weight;
		return v;
	};
	for (int i = 0; i + 1 < count; ++i) {
		// The lower corners belong to node i alone; the upper ones sit on the
		// joint and are shared between i + 1 and i.
		const SrcSkinVertex a = corner(i, -1, i, 0), b = corner(i, 1, i, 0);
		const SrcSkinVertex c = corner(i + 1, -1, i, 0.4f), d = corner(i + 1, 1, i, 0.4f);
		// Both windings: the strip is seen from either side.
		for (const SrcSkinVertex& v : {a, b, c, c, b, d, a, c, b, b, c, d}) part.verts.push_back(v);
	}
	model.parts.push_back(part);

	SrcClip bend;
	bend.name = "S_BEND";
	bend.next = "";
	bend.fps = 10.0f;
	bend.blendIn = 0.0f;
	SrcClip nod;
	nod.name = "T_NOD";
	nod.next = "S_BEND";
	nod.fps = 20.0f;
	nod.blendIn = 0.0f;
	nod.blendOut = 0.25f;
	for (int i = 0; i < count; ++i) bend.nodes.push_back(i);
	nod.nodes.push_back(count - 1);
	for (int f = 0; f < 5; ++f) {
		for (int i = 0; i < count; ++i) {
			SrcClipSample s {};
			chainRest(count, i, s.pos, s.rot);
			const float a = 0.02f * float(f) * 0.5f; // every link turns the same way: a curl
			const Quat q = quatMul({s.rot[0], s.rot[1], s.rot[2], s.rot[3]}, {std::sin(a), 0, 0, std::cos(a)});
			s.rot[0] = q.x;
			s.rot[1] = q.y;
			s.rot[2] = q.z;
			s.rot[3] = q.w;
			if (i == 0) s.pos[1] += float(f); // the root rises
			bend.samples.push_back(s);
		}
	}
	for (int f = 0; f < 3; ++f) {
		SrcClipSample s {};
		chainRest(count, count - 1, s.pos, s.rot);
		const float a = 0.3f * float(f) * 0.5f;
		s.rot[0] = 0;
		s.rot[1] = std::sin(a);
		s.rot[2] = 0;
		s.rot[3] = std::cos(a);
		nod.samples.push_back(s);
	}
	model.clips.push_back(bend);
	model.clips.push_back(nod);
}

} // namespace g3
