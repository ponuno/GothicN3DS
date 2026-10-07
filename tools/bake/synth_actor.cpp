// The synthetic test character: a jointed figure of boxes with a skeleton of
// 14 nodes and procedural clips. Like the synthetic scene it has nothing to
// do with Gothic's content; it exists so that skinning, animation and the
// third-person camera can be built and tested without owning the game. The
// clips carry Gothic's names, so the runtime drives it exactly like the hero.
#include "model_bake.h"

#include "g3ds/math.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace g3 {

namespace {

enum Node {
	ROOT, SPINE, CHEST, HEAD, L_ARM, L_FOREARM, R_ARM, R_FOREARM, L_THIGH, L_CALF, L_FOOT, R_THIGH, R_CALF, R_FOOT, NODES
};

struct Rest {
	const char* name;
	int parent;
	float x, y, z;
};
// Names follow the 3ds Max biped Gothic's models were built on.
const Rest kRest[NODES] = {
    {"BIP01", -1, 0, 95, 0},
    {"BIP01 SPINE", ROOT, 0, 10, 0},
    {"BIP01 SPINE1", SPINE, 0, 25, 0},
    {"BIP01 HEAD", CHEST, 0, 30, 0},
    {"BIP01 L UPPERARM", CHEST, -24, 22, 0},
    {"BIP01 L FOREARM", L_ARM, 0, -28, 0},
    {"BIP01 R UPPERARM", CHEST, 24, 22, 0},
    {"BIP01 R FOREARM", R_ARM, 0, -28, 0},
    {"BIP01 L THIGH", ROOT, -10, -5, 0},
    {"BIP01 L CALF", L_THIGH, 0, -42, 0},
    {"BIP01 L FOOT", L_CALF, 0, -42, 0},
    {"BIP01 R THIGH", ROOT, 10, -5, 0},
    {"BIP01 R CALF", R_THIGH, 0, -42, 0},
    {"BIP01 R FOOT", R_CALF, 0, -42, 0},
};

void restWorld(float out[NODES][3]) {
	for (int i = 0; i < NODES; ++i) {
		out[i][0] = kRest[i].x;
		out[i][1] = kRest[i].y;
		out[i][2] = kRest[i].z;
		if (kRest[i].parent >= 0)
			for (int a = 0; a < 3; ++a) out[i][a] += out[kRest[i].parent][a];
	}
}

// A box between two heights. Its upper ring of corners leans on `upBone`, its
// lower ring on `downBone`, so joints bend instead of breaking.
struct Box {
	float cx, cz, y0, y1, hx, hz; // centre, top and bottom height, half widths
	int bone, upBone, downBone;
	float upWeight, downWeight;
	float u0, v0, u1, v1; // texture rectangle
	bool face;            // the +Z side gets the right half of the texture
};

void addBox(SrcModelPart& part, const Box& b) {
	// Corners: bit 0 = +x, bit 1 = bottom, bit 2 = +z.
	SrcSkinVertex c[8];
	for (int i = 0; i < 8; ++i) {
		SrcSkinVertex& v = c[i];
		v.pos[0] = b.cx + ((i & 1) ? b.hx : -b.hx);
		v.pos[1] = (i & 2) ? b.y1 : b.y0;
		v.pos[2] = b.cz + ((i & 4) ? b.hz : -b.hz);
		v.bone[0] = b.bone;
		v.bone[1] = (i & 2) ? b.downBone : b.upBone;
		v.weight1 = (i & 2) ? b.downWeight : b.upWeight;
		if (v.bone[1] == v.bone[0]) v.weight1 = 0;
	}
	// Six faces, each with its outward normal.
	static const int faces[6][4] = {{0, 1, 3, 2}, {5, 4, 6, 7}, {4, 0, 2, 6}, {1, 5, 7, 3}, {4, 5, 1, 0}, {2, 3, 7, 6}};
	static const float normals[6][3] = {{0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
	for (int f = 0; f < 6; ++f) {
		SrcSkinVertex q[4];
		for (int k = 0; k < 4; ++k) {
			q[k] = c[faces[f][k]];
			std::memcpy(q[k].normal, normals[f], sizeof(q[k].normal));
			// Corners of the face in texture order: 0 top-left, 1 top-right, 2 bottom-right, 3 bottom-left.
			float u = (k == 1 || k == 2) ? b.u1 : b.u0;
			const float v = (k >= 2) ? b.v1 : b.v0;
			if (b.face) u = (f == 1) ? 0.5f + u * 0.5f : u * 0.5f;
			q[k].uv[0] = u;
			q[k].uv[1] = v;
		}
		// Front faces are counter-clockwise on screen: in this left-handed
		// system the front normal is -cross(b - a, c - a).
		const int tris[2][3] = {{0, 1, 2}, {0, 2, 3}};
		for (const auto& t : tris) {
			SrcSkinVertex a = q[t[0]], bb = q[t[1]], cc = q[t[2]];
			const float e1[3] = {bb.pos[0] - a.pos[0], bb.pos[1] - a.pos[1], bb.pos[2] - a.pos[2]};
			const float e2[3] = {cc.pos[0] - a.pos[0], cc.pos[1] - a.pos[1], cc.pos[2] - a.pos[2]};
			const float front[3] = {-(e1[1] * e2[2] - e1[2] * e2[1]), -(e1[2] * e2[0] - e1[0] * e2[2]),
			                        -(e1[0] * e2[1] - e1[1] * e2[0])};
			const float facing = front[0] * normals[f][0] + front[1] * normals[f][1] + front[2] * normals[f][2];
			part.verts.push_back(a);
			part.verts.push_back(facing > 0 ? bb : cc);
			part.verts.push_back(facing > 0 ? cc : bb);
		}
	}
}

struct Pose {
	float pos[NODES][3];
	Quat rot[NODES];
};

Pose restPose() {
	Pose p;
	for (int i = 0; i < NODES; ++i) {
		p.pos[i][0] = kRest[i].x;
		p.pos[i][1] = kRest[i].y;
		p.pos[i][2] = kRest[i].z;
		p.rot[i] = quatIdentity();
	}
	return p;
}

Quat aboutX(float a) { return {std::sin(a * 0.5f), 0, 0, std::cos(a * 0.5f)}; }
Quat aboutZ(float a) { return {0, 0, std::sin(a * 0.5f), std::cos(a * 0.5f)}; }

// The feet are left out of every clip: nodes without a track keep their rest
// transform, and the runtime has to cope with that.
const int kTracks[] = {ROOT, SPINE, CHEST, HEAD, L_ARM, L_FOREARM, R_ARM, R_FOREARM, L_THIGH, L_CALF, R_THIGH, R_CALF};

template <class Fn> SrcClip makeClip(const char* name, const char* next, int frames, bool moves, Fn pose) {
	SrcClip clip;
	clip.name = name;
	clip.next = next;
	clip.fps = 25.0f;
	clip.moves = moves;
	clip.blendIn = clip.blendOut = 0.15f;
	clip.nodes.assign(std::begin(kTracks), std::end(kTracks));
	for (int f = 0; f < frames; ++f) {
		Pose p = restPose();
		pose(float(f) / float(frames), float(f) / clip.fps, p);
		for (int node : kTracks) {
			SrcClipSample s;
			std::memcpy(s.pos, p.pos[node], sizeof(s.pos));
			s.rot[0] = p.rot[node].x;
			s.rot[1] = p.rot[node].y;
			s.rot[2] = p.rot[node].z;
			s.rot[3] = p.rot[node].w;
			clip.samples.push_back(s);
		}
	}
	return clip;
}

const float kTau = 6.2831853f;

// A stride: `swing` radians at the hips, the body travelling `speed` cm/s.
void stride(float phase, float seconds, float swing, float speed, Pose& p) {
	const float s = std::sin(phase * kTau), c = std::cos(phase * kTau);
	// Rotating about +X swings a hanging limb backwards (towards -Z).
	p.rot[L_THIGH] = aboutX(-swing * s);
	p.rot[R_THIGH] = aboutX(swing * s);
	p.rot[L_CALF] = aboutX(swing * (0.9f + 0.8f * c));
	p.rot[R_CALF] = aboutX(swing * (0.9f - 0.8f * c));
	p.rot[L_ARM] = aboutX(swing * 0.8f * s);
	p.rot[R_ARM] = aboutX(-swing * 0.8f * s);
	p.rot[L_FOREARM] = p.rot[R_FOREARM] = aboutX(-swing * 1.2f);
	p.rot[CHEST] = aboutX(swing * 0.2f);
	p.pos[ROOT][1] += 3.0f * std::fabs(c) - 2.0f;
	p.pos[ROOT][2] = speed * seconds; // the source walks away from the origin
}

} // namespace

void buildSyntheticActor(SrcModel& model) {
	model = SrcModel {};
	model.name = "hero";
	model.nodes.resize(NODES);
	for (int i = 0; i < NODES; ++i) {
		model.nodes[size_t(i)].name = kRest[i].name;
		model.nodes[size_t(i)].parent = kRest[i].parent;
		model.nodes[size_t(i)].pos[0] = kRest[i].x;
		model.nodes[size_t(i)].pos[1] = kRest[i].y;
		model.nodes[size_t(i)].pos[2] = kRest[i].z;
	}
	float w[NODES][3];
	restWorld(w);

	SrcModelPart body, head;
	body.material.name = "actor_body";
	body.material.texture = "ACTOR_BODY";
	head.material.name = "actor_head";
	head.material.texture = "ACTOR_HEAD";
	const float hipY = w[ROOT][1], chestY = w[CHEST][1], shoulderY = w[L_ARM][1];
	// Torso: pelvis and chest.
	addBox(body, {0, 0, chestY, hipY - 10, 17, 10, SPINE, CHEST, ROOT, 0.6f, 0.7f, 0.0f, 0.5f, 0.5f, 1.0f, false});
	addBox(body, {0, 0, chestY + 30, chestY, 20, 11, CHEST, CHEST, SPINE, 0, 0.4f, 0.0f, 0.0f, 0.5f, 0.5f, false});
	// Arms and legs.
	for (int side = 0; side < 2; ++side) {
		const int arm = side ? R_ARM : L_ARM, fore = side ? R_FOREARM : L_FOREARM;
		const int thigh = side ? R_THIGH : L_THIGH, calf = side ? R_CALF : L_CALF, foot = side ? R_FOOT : L_FOOT;
		const float ax = w[arm][0], lx = w[thigh][0];
		addBox(body, {ax, 0, shoulderY + 4, shoulderY - 28, 5.5f, 5.5f, arm, CHEST, fore, 0.3f, 0.5f, 0.5f, 0.0f, 1.0f, 0.5f, false});
		addBox(body, {ax, 0, shoulderY - 28, shoulderY - 58, 4.8f, 4.8f, fore, arm, fore, 0.5f, 0, 0.5f, 0.0f, 1.0f, 0.5f, false});
		addBox(body, {lx, 0, w[thigh][1] + 5, w[calf][1], 7.5f, 7.5f, thigh, ROOT, calf, 0.35f, 0.5f, 0.5f, 0.5f, 1.0f, 1.0f, false});
		addBox(body, {lx, 0, w[calf][1], w[foot][1], 6.0f, 6.0f, calf, thigh, foot, 0.5f, 0.3f, 0.5f, 0.5f, 1.0f, 1.0f, false});
		// The foot points forward: that is how the model's front is recognised.
		addBox(body, {lx, 6, w[foot][1], 0.0f, 6.0f, 13.0f, foot, calf, foot, 0.3f, 0, 0.5f, 0.5f, 1.0f, 1.0f, false});
	}
	addBox(head, {0, 0, w[HEAD][1] + 26, w[HEAD][1] + 2, 10, 11, HEAD, HEAD, HEAD, 0, 0, 0.0f, 0.0f, 1.0f, 1.0f, true});
	model.parts.push_back(std::move(body));
	model.parts.push_back(std::move(head));

	model.loadTexture = [](const std::string& name, SrcImage& out) {
		const bool isHead = name == "ACTOR_HEAD";
		if (!isHead && name != "ACTOR_BODY") return false;
		out.width = 64;
		out.height = 32;
		out.rgba.resize(size_t(64 * 32 * 4));
		for (int y = 0; y < 32; ++y)
			for (int x = 0; x < 64; ++x) {
				u8* p = &out.rgba[size_t(y * 64 + x) * 4];
				int r, g, b;
				if (isHead) {
					// Left half: hair and skin all round. Right half: the face.
					r = 214; g = 170; b = 136;
					if (y < 9) { r = 70; g = 48; b = 30; }
					if (x >= 32) {
						const int fx = x - 32;
						if (y >= 12 && y < 16 && ((fx >= 7 && fx < 12) || (fx >= 20 && fx < 25))) { r = 30; g = 30; b = 40; }
						if (y >= 23 && y < 25 && fx >= 10 && fx < 22) { r = 150; g = 60; b = 50; }
					}
				} else if (x < 32) {
					// Tunic (top) and belt and trousers (bottom).
					r = 120; g = 44; b = 36;
					if (y >= 16) { r = 82; g = 70; b = 52; }
					if (y >= 16 && y < 19) { r = 40; g = 30; b = 20; }
					if (((x + y) & 7) == 0) { r += 18; g += 12; }
				} else {
					// Sleeves (top) and trouser legs (bottom).
					r = 132; g = 52; b = 40;
					if (y >= 16) { r = 74; g = 64; b = 48; }
					if ((y & 7) == 7) { r -= 20; g -= 16; b -= 12; }
				}
				p[0] = u8(r);
				p[1] = u8(g);
				p[2] = u8(b);
				p[3] = 255;
			}
		return true;
	};

	// ---- clips ----
	model.clips.push_back(makeClip("S_RUN", "S_RUN", 50, false, [](float phase, float, Pose& p) {
		const float s = std::sin(phase * kTau);
		p.rot[CHEST] = aboutX(0.03f * s);
		p.rot[L_ARM] = aboutZ(-0.08f - 0.03f * s);
		p.rot[R_ARM] = aboutZ(0.08f + 0.03f * s);
		p.rot[HEAD] = {0, std::sin(0.1f * s), 0, std::cos(0.1f * s)};
	}));
	model.clips.push_back(makeClip("S_RUNL", "S_RUNL", 16, true, [](float phase, float seconds, Pose& p) { stride(phase, seconds, 0.75f, 480.0f, p); }));
	model.clips.push_back(makeClip("S_WALKL", "S_WALKL", 26, true, [](float phase, float seconds, Pose& p) { stride(phase, seconds, 0.35f, 150.0f, p); }));
	model.clips.push_back(makeClip("S_JUMP", "S_JUMP", 8, false, [](float phase, float, Pose& p) {
		const float s = std::sin(phase * kTau);
		p.rot[L_THIGH] = aboutX(-0.7f);
		p.rot[R_THIGH] = aboutX(-0.3f);
		p.rot[L_CALF] = aboutX(1.1f);
		p.rot[R_CALF] = aboutX(0.6f);
		p.rot[L_ARM] = aboutX(-2.4f + 0.05f * s);
		p.rot[R_ARM] = aboutX(-2.4f - 0.05f * s);
	}));
	model.clips.push_back(makeClip("S_FALLDN", "S_FALLDN", 10, false, [](float phase, float, Pose& p) {
		const float s = std::sin(phase * kTau);
		// Rotating about +Z lifts the right arm outwards, about -Z the left one.
		p.rot[L_ARM] = aboutZ(-1.3f - 0.25f * s);
		p.rot[R_ARM] = aboutZ(1.3f + 0.25f * s);
		p.rot[L_THIGH] = aboutX(-0.25f + 0.2f * s);
		p.rot[R_THIGH] = aboutX(-0.25f - 0.2f * s);
		p.rot[L_CALF] = p.rot[R_CALF] = aboutX(0.5f);
	}));
	model.clips.push_back(makeClip("S_SWIM", "S_SWIM", 30, false, [](float phase, float, Pose& p) {
		const float s = std::sin(phase * kTau);
		p.rot[L_ARM] = aboutZ(-1.2f + 0.3f * s);
		p.rot[R_ARM] = aboutZ(1.2f - 0.3f * s);
		p.rot[L_THIGH] = aboutX(0.25f * s);
		p.rot[R_THIGH] = aboutX(-0.25f * s);
	}));
	model.clips.push_back(makeClip("S_SWIMF", "S_SWIMF", 24, true, [](float phase, float seconds, Pose& p) {
		const float s = std::sin(phase * kTau), c = std::cos(phase * kTau);
		p.rot[ROOT] = aboutX(1.25f); // lying forward in the water
		p.pos[ROOT][1] = 112.0f;
		p.pos[ROOT][2] = 260.0f * seconds;
		p.rot[HEAD] = aboutX(-0.9f);
		p.rot[L_ARM] = aboutX(-2.6f + 0.9f * s);
		p.rot[R_ARM] = aboutX(-2.6f - 0.9f * s);
		p.rot[L_THIGH] = aboutX(0.3f * c);
		p.rot[R_THIGH] = aboutX(-0.3f * c);
	}));
	// Standing in walk mode, and swimming backwards.
	model.clips.push_back(makeClip("S_WALK", "S_WALK", 40, false, [](float phase, float, Pose& p) {
		const float s = std::sin(phase * kTau);
		p.rot[CHEST] = aboutX(0.02f * s);
		p.rot[L_ARM] = aboutZ(-0.05f);
		p.rot[R_ARM] = aboutZ(0.05f);
	}));
	model.clips.push_back(makeClip("S_SWIMB", "S_SWIMB", 24, true, [](float phase, float seconds, Pose& p) {
		const float s = std::sin(phase * kTau);
		p.rot[ROOT] = aboutX(-0.9f); // leaning back
		p.pos[ROOT][2] = -120.0f * seconds;
		p.rot[L_ARM] = aboutZ(-1.0f + 0.5f * s);
		p.rot[R_ARM] = aboutZ(1.0f - 0.5f * s);
	}));

	// Transitions, as ZenGin's model scripts have them: from standing into
	// the run (gathering pace) and out of it, each ending in the pose its
	// follower starts with.
	model.clips.push_back(makeClip("T_RUN_2_RUNL", "S_RUNL", 8, true, [](float phase, float seconds, Pose& p) {
		stride(0.0f, 0.0f, 0.75f * phase, 0.0f, p);
		const float whole = 7.0f / 25.0f;                       // the clip's length
		p.pos[ROOT][2] = 0.5f * 480.0f * seconds * seconds / whole; // from rest to 480 cm/s
	}));
	model.clips.push_back(makeClip("T_RUNL_2_RUN", "S_RUN", 6, true, [](float phase, float seconds, Pose& p) {
		stride(0.0f, 0.0f, 0.75f * (1.0f - phase), 0.0f, p);
		const float whole = 5.0f / 25.0f;
		p.pos[ROOT][2] = 480.0f * seconds - 0.5f * 480.0f * seconds * seconds / whole; // from 480 cm/s to rest
	}));
	// Single actions: a hop back, a step to either side, turning on the spot.
	model.clips.push_back(makeClip("T_JUMPB", "S_RUN", 11, true, [](float phase, float, Pose& p) {
		const float arc = std::sin(phase * kTau * 0.5f);
		p.pos[ROOT][1] += 10.0f * arc;
		p.pos[ROOT][2] = -70.0f * phase;
		p.rot[L_THIGH] = p.rot[R_THIGH] = aboutX(-0.5f * arc);
		p.rot[L_CALF] = p.rot[R_CALF] = aboutX(0.9f * arc);
	}));
	for (int right = 0; right < 2; ++right) {
		const float side = right ? 1.0f : -1.0f;
		model.clips.push_back(makeClip(right ? "T_RUNSTRAFER" : "T_RUNSTRAFEL", "S_RUN", 13, true, [side](float phase, float, Pose& p) {
			const float arc = std::sin(phase * kTau * 0.5f);
			p.pos[ROOT][0] = side * 60.0f * phase;
			p.rot[L_THIGH] = aboutZ(-0.35f * arc);
			p.rot[R_THIGH] = aboutZ(0.35f * arc);
		}));
		model.clips.push_back(makeClip(right ? "T_RUNTURNR" : "T_RUNTURNL", right ? "T_RUNTURNR" : "T_RUNTURNL", 12, false,
		                               [side](float phase, float, Pose& p) {
			                               const float s = std::sin(phase * kTau);
			                               p.rot[L_THIGH] = aboutX(-0.25f * std::max(s, 0.0f));
			                               p.rot[R_THIGH] = aboutX(-0.25f * std::max(-s, 0.0f));
			                               p.rot[L_CALF] = aboutX(0.5f * std::max(s, 0.0f));
			                               p.rot[R_CALF] = aboutX(0.5f * std::max(-s, 0.0f));
			                               p.rot[CHEST] = {0, std::sin(side * 0.1f), 0, std::cos(side * 0.1f)};
		                               }));
	}
	// Jumps. As in the game's files, the leap itself is in the take-off
	// animation: its root rises and travels, and the game moves the body by
	// that. S_JUMP is only the pose held in the air afterwards.
	SrcClip standJump = makeClip("T_STAND_2_JUMP", "S_JUMP", 13, true, [](float phase, float, Pose& p) {
		const float crouch = phase < 0.35f ? std::sin(phase / 0.35f * kTau * 0.5f) : 0.0f; // down and up again
		const float flight = phase < 0.35f ? 0.0f : (phase - 0.35f) / 0.65f;                // then off the ground
		p.pos[ROOT][1] += -14.0f * crouch + 75.0f * std::sin(flight * kTau * 0.25f);
		p.pos[ROOT][2] = 130.0f * flight;
		p.rot[L_THIGH] = p.rot[R_THIGH] = aboutX(-0.9f * crouch - 0.5f * flight);
		p.rot[L_CALF] = p.rot[R_CALF] = aboutX(1.6f * crouch + 0.8f * flight);
		p.rot[L_ARM] = p.rot[R_ARM] = aboutX(-2.4f * flight);
	});
	standJump.fly = true;
	model.clips.push_back(standJump);
	SrcClip runJump = makeClip("T_RUNL_2_JUMP", "S_JUMP", 9, true, [](float phase, float seconds, Pose& p) {
		p.pos[ROOT][1] += 85.0f * std::sin(phase * kTau * 0.25f);
		p.pos[ROOT][2] = 480.0f * seconds;
		p.rot[L_THIGH] = aboutX(-0.7f * phase);
		p.rot[R_THIGH] = aboutX(-0.3f * phase);
		p.rot[L_CALF] = aboutX(1.1f * phase);
		p.rot[R_CALF] = aboutX(0.6f * phase);
		p.rot[L_ARM] = p.rot[R_ARM] = aboutX(-2.4f * phase);
	});
	runJump.fly = true;
	model.clips.push_back(runJump);
	for (const char* name : {"T_JUMP_2_STAND", "T_FALLDN_2_STAND"})
		model.clips.push_back(makeClip(name, "S_RUN", 6, false, [](float phase, float, Pose& p) {
			const float crouch = 1.0f - phase;
			p.pos[ROOT][1] -= 16.0f * crouch;
			p.rot[L_THIGH] = p.rot[R_THIGH] = aboutX(-0.9f * crouch);
			p.rot[L_CALF] = p.rot[R_CALF] = aboutX(1.6f * crouch);
		}));
	// Where the script has no animation for a change, it names a fade.
	model.blends = {{"T_RUNL_2_WALKL", "S_WALKL", 0.2f, 0.2f}, {"T_WALKL_2_RUNL", "S_RUNL", 0.2f, 0.2f},
	                {"T_SWIM_2_SWIMF", "S_SWIMF", 0.3f, 0.3f}, {"T_SWIMF_2_SWIM", "S_SWIM", 0.3f, 0.3f},
	                {"T_JUMP_2_RUNL", "S_RUNL", 0.15f, 0.15f}};
	model.notes.push_back("synthetic test character: 14 nodes, clips named as in Gothic");
}

} // namespace g3
