// The synthetic test character: a jointed figure of boxes with a skeleton of
// 14 nodes and procedural clips. Like the synthetic scene it has nothing to
// do with Gothic's content; it exists so that skinning, animation and the
// third-person camera can be built and tested without owning the game. The
// clips carry Gothic's names, so the runtime drives it exactly like the hero.
#include "model_bake.h"

#include "g3ds/math.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <functional>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace g3 {

namespace {

enum Node {
	ROOT, SPINE, CHEST, HEAD, L_ARM, L_FOREARM, R_ARM, R_FOREARM, L_THIGH, L_CALF, L_FOOT, R_THIGH, R_CALF, R_FOOT, L_HAND, R_HAND, NODES
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
    {"BIP01 L HAND", L_FOREARM, 0, -26, 0},
    {"BIP01 R HAND", R_FOREARM, 0, -26, 0},
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

// `withRoot` false: no track for the root - as the game's "S_BENCH_S1" and
// "S_BED_FRONT_S1", which leave it where the step into them put it.
template <class Fn> SrcClip makeClip(const char* name, const char* next, int frames, bool moves, Fn pose, bool withRoot = true) {
	SrcClip clip;
	clip.name = name;
	clip.next = next;
	clip.fps = 25.0f;
	clip.moves = moves;
	clip.blendIn = clip.blendOut = 0.15f;
	for (int node : kTracks)
		if (withRoot || node != ROOT) clip.nodes.push_back(node);
	for (int f = 0; f < frames; ++f) {
		Pose p = restPose();
		pose(float(f) / float(frames), float(f) / clip.fps, p);
		for (int node : clip.nodes) {
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

// A clip with a footstep (*eventSFXGrnd) at each of the frames.
SrcClip withSteps(SrcClip clip, std::vector<int> frames) {
	for (int f : frames) {
		SrcClip::Sfx e;
		e.frame = f;
		e.ground = true;
		e.name = "SFX_TEST_STEP";
		e.range = 1000.0f;
		clip.sfx.push_back(e);
	}
	return clip;
}

// A clip with the event tags of a fight (frames of the clip).
SrcClip withFight(SrcClip clip, std::vector<int> hits, std::vector<int> hitEnds, std::vector<int> windows,
                  std::vector<int> parry = {}, int fightMode = -1) {
	clip.hits = std::move(hits);
	clip.hitEnds = std::move(hitEnds);
	clip.windows = std::move(windows);
	clip.parry = std::move(parry);
	clip.fightModeFrame = fightMode;
	return clip;
}

// Fists up, or a sword held out in front with the right hand.
void guardPose(bool sword, float amount, Pose& p) {
	if (sword) {
		p.rot[R_ARM] = aboutX(-0.9f * amount);
		p.rot[R_FOREARM] = aboutX(-0.6f * amount);
		p.rot[L_ARM] = aboutZ(-0.25f * amount);
	} else {
		p.rot[L_ARM] = aboutX(-1.1f * amount);
		p.rot[R_ARM] = aboutX(-1.1f * amount);
		p.rot[L_FOREARM] = p.rot[R_FOREARM] = aboutX(-1.6f * amount);
	}
	p.rot[L_THIGH] = aboutX(-0.15f * amount);
	p.rot[R_THIGH] = aboutX(0.15f * amount);
	p.rot[L_CALF] = p.rot[R_CALF] = aboutX(0.2f * amount);
}

// Blows: an arm goes out and back over `phase` 0..1 of the blow; the body leans in.
void blow(bool sword, int arm, float phase, Pose& p) {
	const float out = std::sin(std::min(phase, 1.0f) * kTau * 0.5f);
	guardPose(sword, 1.0f, p);
	if (arm == 0) {
		p.rot[R_ARM] = aboutX(-1.1f - 0.5f * out);
		p.rot[R_FOREARM] = aboutX(-1.6f * (1.0f - out));
	} else if (arm == 1) {
		p.rot[L_ARM] = aboutX(-1.1f - 0.5f * out);
		p.rot[L_FOREARM] = aboutX(-1.6f * (1.0f - out));
	} else {
		// A swing across from one side (arm 2: from the left, 3: from the right).
		const float side = arm == 2 ? 1.0f : -1.0f;
		p.rot[R_ARM] = quatMul(Quat {0, std::sin(side * (0.9f - 1.8f * out) * 0.5f), 0, std::cos(side * (0.9f - 1.8f * out) * 0.5f)}, aboutX(-1.4f));
	}
	p.rot[CHEST] = aboutX(0.25f * out);
}

void addFightClips(SrcModel& model) {
	// With fists and with a one-handed weapon: standing ready, going, drawing
	// and putting away (the weapon changes hands half way), attacking - a combo
	// of blows - fending off, hopping back, stepping aside.
	for (int sword = 0; sword < 2; ++sword) {
		const std::string w = sword ? "1H" : "FIST";
		const bool sw = sword != 0;
		model.clips.push_back(makeClip(("S_" + w + "RUN").c_str(), ("S_" + w + "RUN").c_str(), 40, false, [sw](float phase, float, Pose& p) {
			guardPose(sw, 1.0f, p);
			p.pos[ROOT][1] += 1.5f * std::sin(phase * kTau);
		}));
		model.clips.push_back(makeClip(("S_" + w + "RUNL").c_str(), ("S_" + w + "RUNL").c_str(), 16, true, [sw](float phase, float seconds, Pose& p) {
			stride(phase, seconds, 0.6f, 400.0f, p);
			Pose g = restPose();
			guardPose(sw, 1.0f, g);
			p.rot[L_ARM] = g.rot[L_ARM];
			p.rot[R_ARM] = g.rot[R_ARM];
			p.rot[L_FOREARM] = g.rot[L_FOREARM];
			p.rot[R_FOREARM] = g.rot[R_FOREARM];
		}));
		model.clips.push_back(withFight(makeClip(("T_" + w + "_2_" + w + "RUN").c_str(), ("S_" + w + "RUN").c_str(), 12, false,
		                                         [sw](float phase, float, Pose& p) { guardPose(sw, phase, p); }),
		                                {}, {}, {}, {}, 6));
		model.clips.push_back(withFight(makeClip(("T_" + w + "RUN_2_" + w).c_str(), "S_RUN", 12, false,
		                                         [sw](float phase, float, Pose& p) { guardPose(sw, 1.0f - phase, p); }),
		                                {}, {}, {}, {}, 6));
		if (sw) {
			// Three blows, each ending where the next begins; pressing again in a
			// blow's window goes on with the next one.
			model.clips.push_back(withFight(makeClip("S_1HATTACK", "S_1HRUN", 48, false,
			                                         [](float phase, float, Pose& p) {
				                                         const float t = phase * 3.0f;
				                                         const int k = std::min(int(t), 2);
				                                         blow(true, k == 1 ? 2 : k == 2 ? 3 : 0, t - float(k), p);
			                                         }),
			                                {7, 23, 39}, {15, 31, 47}, {3, 15, 19, 31, 35, 47}));
			model.clips.push_back(withFight(makeClip("T_1HATTACKL", "S_1HRUN", 16, false, [](float phase, float, Pose& p) { blow(true, 2, phase, p); }),
			                                {7}, {15}, {}));
			model.clips.push_back(withFight(makeClip("T_1HATTACKR", "S_1HRUN", 16, false, [](float phase, float, Pose& p) { blow(true, 3, phase, p); }),
			                                {7}, {15}, {}));
		} else {
			model.clips.push_back(withFight(makeClip("S_FISTATTACK", "S_FISTRUN", 30, false,
			                                         [](float phase, float, Pose& p) {
				                                         const float t = phase * 2.0f;
				                                         const int k = std::min(int(t), 1);
				                                         blow(false, k, t - float(k), p);
			                                         }),
			                                {6, 21}, {14, 29}, {2, 14, 17, 29}));
		}
		model.clips.push_back(withFight(makeClip(("T_" + w + "ATTACKMOVE").c_str(), ("S_" + w + "RUN").c_str(), 16, true,
		                                         [sw](float phase, float seconds, Pose& p) {
			                                         blow(sw, 0, phase, p);
			                                         p.pos[ROOT][2] = 250.0f * seconds;
		                                         }),
		                                {7}, {15}, {}));
		model.clips.push_back(withFight(makeClip(("T_" + w + "PARADE_O").c_str(), ("S_" + w + "RUN").c_str(), 14, false,
		                                         [sw](float phase, float, Pose& p) {
			                                         guardPose(sw, 1.0f, p);
			                                         const float up = std::sin(phase * kTau * 0.5f);
			                                         p.rot[L_ARM] = p.rot[R_ARM] = aboutX(-1.1f - 0.9f * up);
		                                         }),
		                                {}, {}, {}, {1, 12}));
		model.clips.push_back(makeClip(sw ? "T_1HJUMPB" : "T_FISTPARADEJUMPB", ("S_" + w + "RUN").c_str(), 11, true, [sw](float phase, float, Pose& p) {
			guardPose(sw, 1.0f, p);
			const float arc = std::sin(phase * kTau * 0.5f);
			p.pos[ROOT][1] += 10.0f * arc;
			p.pos[ROOT][2] = -90.0f * phase;
		}));
		for (int right = 0; right < 2; ++right) {
			const float side = right ? 1.0f : -1.0f;
			model.clips.push_back(makeClip(("T_" + w + (right ? "RUNSTRAFER" : "RUNSTRAFEL")).c_str(), ("S_" + w + "RUN").c_str(), 13, true,
			                               [side, sw](float phase, float, Pose& p) {
				                               guardPose(sw, 1.0f, p);
				                               p.pos[ROOT][0] = side * 60.0f * phase;
			                               }));
		}
	}
	// Hit, knocked out, killed.
	for (int back = 0; back < 2; ++back) {
		const float dir = back ? 1.0f : -1.0f;
		model.clips.push_back(makeClip(back ? "T_STUMBLEB" : "T_STUMBLE", "S_RUN", 10, true, [dir](float phase, float, Pose& p) {
			const float a = std::sin(phase * kTau * 0.5f);
			p.rot[CHEST] = aboutX(0.4f * dir * a);
			p.pos[ROOT][2] = -25.0f * dir * phase * -1.0f;
		}));
		const auto lying = [dir](float down, Pose& p) {
			p.pos[ROOT][1] -= 78.0f * down;
			p.rot[ROOT] = aboutX(1.5f * dir * down);
			p.rot[L_ARM] = aboutZ(-1.2f * down);
			p.rot[R_ARM] = aboutZ(1.2f * down);
		};
		model.clips.push_back(makeClip(back ? "T_DEADB" : "T_DEAD", back ? "S_DEADB" : "S_DEAD", 20, false,
		                               [lying](float phase, float, Pose& p) { lying(phase, p); }));
		model.clips.push_back(makeClip(back ? "S_DEADB" : "S_DEAD", back ? "S_DEADB" : "S_DEAD", 10, false,
		                               [lying](float, float, Pose& p) { lying(1.0f, p); }));
	}
	const auto wounded = [](float down, Pose& p) {
		p.pos[ROOT][1] -= 55.0f * down;
		p.rot[L_THIGH] = p.rot[R_THIGH] = aboutX(-1.6f * down);
		p.rot[L_CALF] = p.rot[R_CALF] = aboutX(2.2f * down);
		p.rot[CHEST] = aboutX(0.7f * down);
		p.rot[HEAD] = aboutX(0.4f * down);
	};
	model.clips.push_back(makeClip("T_STAND_2_WOUNDED", "S_WOUNDED", 15, false, [wounded](float phase, float, Pose& p) { wounded(phase, p); }));
	model.clips.push_back(makeClip("S_WOUNDED", "S_WOUNDED", 40, false, [wounded](float phase, float, Pose& p) {
		wounded(1.0f, p);
		p.rot[CHEST] = aboutX(0.7f + 0.05f * std::sin(phase * kTau));
	}));
	model.clips.push_back(makeClip("T_WOUNDED_2_STAND", "S_RUN", 20, false, [wounded](float phase, float, Pose& p) { wounded(1.0f - phase, p); }));
	// Sneaking: crouched, slowly.
	const auto crouch = [](Pose& p) {
		p.pos[ROOT][1] -= 18.0f;
		p.rot[L_THIGH] = p.rot[R_THIGH] = aboutX(-0.5f);
		p.rot[L_CALF] = p.rot[R_CALF] = aboutX(0.9f);
		p.rot[CHEST] = aboutX(0.35f);
	};
	model.clips.push_back(makeClip("S_SNEAK", "S_SNEAK", 40, false, [crouch](float, float, Pose& p) { crouch(p); }));
	model.clips.push_back(makeClip("S_SNEAKL", "S_SNEAKL", 30, true, [crouch](float phase, float seconds, Pose& p) {
		stride(phase, seconds, 0.3f, 120.0f, p);
		p.pos[ROOT][1] -= 18.0f;
		p.rot[CHEST] = aboutX(0.35f);
	}));
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
	model.clips.push_back(withSteps(makeClip("S_RUNL", "S_RUNL", 16, true, [](float phase, float seconds, Pose& p) { stride(phase, seconds, 0.75f, 480.0f, p); }), {3, 11}));
	model.clips.push_back(withSteps(makeClip("S_WALKL", "S_WALKL", 26, true, [](float phase, float seconds, Pose& p) { stride(phase, seconds, 0.35f, 150.0f, p); }), {4, 17}));
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
	// Climbing onto a ledge, under Gothic 1's names: low, middle, and high with a
	// hang from the edge. The rise is in the root, as in the game's files.
	{
		const auto reach = [](float up, Pose& p) { // arms up by `up` 0..1
			p.rot[L_ARM] = p.rot[R_ARM] = aboutX(-2.8f * up);
		};
		const auto crouch = [](float c, Pose& p) {
			p.pos[ROOT][1] -= 18.0f * c;
			p.rot[L_THIGH] = p.rot[R_THIGH] = aboutX(-0.9f * c);
			p.rot[L_CALF] = p.rot[R_CALF] = aboutX(1.6f * c);
		};
		const auto climb = [](const char* name, const char* next, int frames, float rise, float forward, float armsFrom, float armsTo) {
			return makeClip(name, next, frames, true, [rise, forward, armsFrom, armsTo](float phase, float, Pose& p) {
				const float t = std::min(phase * float(1) / 1.0f, 1.0f);
				p.pos[ROOT][1] += rise * t;
				p.pos[ROOT][2] = forward * t;
				p.rot[L_ARM] = p.rot[R_ARM] = aboutX(-2.8f * (armsFrom + (armsTo - armsFrom) * t));
				p.rot[L_THIGH] = p.rot[R_THIGH] = aboutX(-0.6f * std::sin(t * 3.14159f));
			});
		};
		const auto held = [reach, crouch](const char* name, float c, float up) {
			return makeClip(name, name, 2, false, [reach, crouch, c, up](float, float, Pose& p) {
				crouch(c, p);
				reach(up, p);
			});
		};
		const auto bend = [reach, crouch](const char* name, const char* next, int frames, float from, float to) {
			return makeClip(name, next, frames, false, [reach, crouch, from, to](float phase, float, Pose& p) {
				const float c = from + (to - from) * phase;
				crouch(c, p);
				reach(c, p);
			});
		};
		model.clips.push_back(bend("T_STAND_2_JUMPUPLOW", "S_JUMPUPLOW", 3, 0.0f, 0.5f));
		model.clips.push_back(climb("S_JUMPUPLOW", "T_JUMPUPLOW_2_STAND", 5, 56.0f, 0.0f, 0.5f, 1.0f));
		model.clips.push_back(climb("T_JUMPUPLOW_2_STAND", "S_RUN", 8, 26.0f, 40.0f, 1.0f, 0.0f));
		model.clips.push_back(bend("T_STAND_2_JUMPUPMID", "S_JUMPUPMID", 8, 0.0f, 0.7f));
		model.clips.push_back(climb("S_JUMPUPMID", "T_JUMPUPMID_2_STAND", 3, 37.0f, 0.0f, 0.7f, 1.0f));
		model.clips.push_back(climb("T_JUMPUPMID_2_STAND", "S_RUN", 20, 91.0f, 45.0f, 1.0f, 0.0f));
		model.clips.push_back(bend("T_STAND_2_JUMPUP", "S_JUMPUP", 9, 0.0f, 1.0f));
		model.clips.push_back(held("S_JUMPUP", 0.0f, 1.0f));
		model.clips.push_back(climb("T_JUMP_2_HANG", "S_HANG", 17, 112.0f, 0.0f, 1.0f, 1.0f));
		model.clips.push_back(held("S_HANG", 0.0f, 1.0f));
		model.clips.push_back(climb("T_HANG_2_STAND", "S_RUN", 25, 168.0f, 50.0f, 1.0f, 0.0f));
		// Picking a thing up: Gothic 1's "C_..._1", the lowest of three.
		model.clips.push_back(bend("C_STAND_2_IGET_1", "C_IGET_1", 8, 0.0f, 1.0f));
		model.clips.push_back(makeClip("C_IGET_1", "C_IGET_2_STAND_1", 2, false, [reach, crouch](float, float, Pose& p) {
			crouch(1.0f, p);
			reach(0.3f, p);
		}));
		model.clips.push_back(bend("C_IGET_2_STAND_1", "S_RUN", 8, 1.0f, 0.0f));
		// Searching a body ("T_PLUNDER"): down to it and up again, in one clip.
		model.clips.push_back(makeClip("T_PLUNDER", "S_RUN", 50, false, [reach, crouch](float phase, float, Pose& p) {
			const float c = std::sin(phase * 3.14159265f);
			crouch(c, p);
			reach(0.5f * c, p);
		}));
	}
	addFightClips(model);
	// Drinking a potion, as "POTIONFAST" (food has no animation here: the
	// engine's stand-still stands in for it).
	const auto drink = [](float a, Pose& p) {
		p.rot[R_ARM] = aboutX(-1.9f * a);
		p.rot[R_FOREARM] = aboutX(-1.5f * a);
		p.rot[HEAD] = aboutX(-0.35f * a);
	};
	model.clips.push_back(makeClip("T_POTIONFAST_STAND_2_S0", "S_POTIONFAST_S0", 15, false, [drink](float phase, float, Pose& p) { drink(phase, p); }));
	model.clips.push_back(makeClip("S_POTIONFAST_S0", "S_POTIONFAST_S0", 10, false, [drink](float, float, Pose& p) { drink(1.0f, p); }));
	model.clips.push_back(makeClip("T_POTIONFAST_S0_2_STAND", "S_RUN", 12, false, [drink](float phase, float, Pose& p) { drink(1.0f - phase, p); }));
	// Where the script has no animation for a change, it names a fade.
	model.blends = {{"T_RUNL_2_WALKL", "S_WALKL", 0.2f, 0.2f}, {"T_WALKL_2_RUNL", "S_RUNL", 0.2f, 0.2f},
	                {"T_SWIM_2_SWIMF", "S_SWIMF", 0.3f, 0.3f}, {"T_SWIMF_2_SWIM", "S_SWIM", 0.3f, 0.3f},
	                {"T_JUMP_2_RUNL", "S_RUNL", 0.15f, 0.15f}};
	model.notes.push_back("synthetic test character: 14 nodes, clips named as in Gothic");
}

// ---- the inhabitants of the synthetic scene ----

namespace {

std::string upperName(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::toupper(c)); });
	return s;
}

// The number after `tag` in a texture name ("..._V3_C1", "_C" -> 1).
int variantOf(const std::string& name, const char* tag) {
	const size_t at = name.rfind(tag);
	return at == std::string::npos ? 0 : std::atoi(name.c_str() + at + std::strlen(tag));
}

// Clothes and faces by name: what the name says decides the colours, so two
// looks that differ show it.
bool paintLook(const std::string& name, SrcImage& out) {
	const bool isHead = name.find("HEAD") != std::string::npos;
	const bool isArmor = name.find("ARMOR") != std::string::npos;
	const bool isHuman = name.compare(0, 4, "HUM_") == 0;
	u32 h = hashName(name.substr(0, name.find("_V")).c_str());
	const int v = variantOf(name, "_V"), c = variantOf(name, "_C");
	static const int skins[4][3] = {{232, 190, 160}, {214, 170, 136}, {170, 122, 90}, {110, 78, 58}};
	const int* skin = skins[c & 3];
	out.width = 64;
	out.height = 32;
	out.rgba.resize(size_t(64 * 32 * 4));
	for (int y = 0; y < 32; ++y)
		for (int x = 0; x < 64; ++x) {
			u8* p = &out.rgba[size_t(y * 64 + x) * 4];
			int r, g, b;
			if (!isHuman) {
				// A creature's hide: mottled, in a colour of its own.
				r = 90 + int(h & 63);
				g = 80 + int((h >> 6) & 63);
				b = 50 + int((h >> 12) & 31);
				if (((x * 7 + y * 13) % 11) < 3) { r -= 30; g -= 26; b -= 14; }
			} else if (isHead) {
				// Left half: hair and skin all round. Right half: the face.
				r = skin[0]; g = skin[1]; b = skin[2];
				if (y < 9) { r = 40 + (v * 37) % 120; g = 30 + (v * 23) % 80; b = 20 + (v * 11) % 40; }
				if (x >= 32) {
					const int fx = x - 32;
					if (y >= 12 && y < 16 && ((fx >= 7 && fx < 12) || (fx >= 20 && fx < 25))) { r = 30; g = 30; b = 40; }
					if (y >= 23 && y < 25 && fx >= 10 && fx < 22) { r = 150; g = 60; b = 50; }
				}
			} else if (isArmor) {
				// Armour: plates in the colour of its kind, the bare arms in the wearer's.
				const int base[3] = {70 + int(h & 127), 60 + int((h >> 7) & 63), 50 + int((h >> 13) & 63)};
				r = base[0]; g = base[1]; b = base[2];
				if (x >= 32 && y < 16) { r = skin[0]; g = skin[1]; b = skin[2]; }
				if ((x & 7) == 0 || (y & 7) == 0) { r -= 24; g -= 24; b -= 24; }
			} else {
				// The bare body: skin, and trousers by the body texture's number.
				r = skin[0]; g = skin[1]; b = skin[2];
				if (y >= 16) { r = 60 + (v * 29) % 70; g = 56 + (v * 17) % 50; b = 44 + (v * 7) % 40; }
				if (y >= 16 && y < 19) { r = 40; g = 30; b = 20; }
			}
			p[0] = u8(std::clamp(r, 0, 255));
			p[1] = u8(std::clamp(g, 0, 255));
			p[2] = u8(std::clamp(b, 0, 255));
			p[3] = 255;
		}
	return true;
}

// A four-legged creature: a body on four legs with a head in front.
enum CreatureNode { C_ROOT, C_HEAD, C_FL, C_FR, C_BL, C_BR, C_NODES };

void buildCreature(const std::string& script, SrcModel& model) {
	model = SrcModel {};
	// Different kinds, different sizes.
	const float size = 0.6f + float(hashName(script.c_str()) % 5) * 0.12f;
	const struct {
		const char* name;
		int parent;
		float x, y, z;
	} rest[C_NODES] = {{"BIP01", -1, 0, 52, 0},           {"BIP01 HEAD", C_ROOT, 0, 10, 46},
	                   {"BIP01 L FOREARM", C_ROOT, -11, -10, 28}, {"BIP01 R FOREARM", C_ROOT, 11, -10, 28},
	                   {"BIP01 L CALF", C_ROOT, -11, -10, -28},   {"BIP01 R CALF", C_ROOT, 11, -10, -28}};
	model.nodes.resize(C_NODES);
	float w[C_NODES][3];
	for (int i = 0; i < C_NODES; ++i) {
		SrcNode& n = model.nodes[size_t(i)];
		n.name = rest[i].name;
		n.parent = rest[i].parent;
		n.pos[0] = rest[i].x * size;
		n.pos[1] = rest[i].y * size;
		n.pos[2] = rest[i].z * size;
		for (int a = 0; a < 3; ++a) w[i][a] = n.pos[a] + (n.parent >= 0 ? w[n.parent][a] : 0.0f);
	}
	SrcModelPart body;
	body.material.name = "creature";
	addBox(body, {0, 0, w[C_ROOT][1] + 12 * size, w[C_ROOT][1] - 12 * size, 15 * size, 38 * size, C_ROOT, C_ROOT, C_ROOT, 0, 0,
	              0.0f, 0.0f, 1.0f, 0.5f, false});
	addBox(body, {0, w[C_HEAD][2], w[C_HEAD][1] + 10 * size, w[C_HEAD][1] - 10 * size, 9 * size, 13 * size, C_HEAD, C_ROOT, C_ROOT,
	              0.3f, 0.3f, 0.0f, 0.5f, 0.5f, 1.0f, false});
	for (int leg = C_FL; leg <= C_BR; ++leg)
		addBox(body, {w[leg][0], w[leg][2], w[leg][1] + 4 * size, 0.0f, 4.5f * size, 4.5f * size, leg, C_ROOT, leg, 0.4f, 0,
		              0.5f, 0.5f, 1.0f, 1.0f, false});
	model.parts.push_back(std::move(body));
	model.loadTexture = paintLook;

	// Its animations, named as the game's creatures have them (they are
	// always "in fist mode"): standing, going at a walk and at a run, lying
	// down to sleep and getting up again.
	struct Step {
		float rootY = 0, rootZ = 0, pitch = 0, headYaw = 0, headPitch = 0, legs = 0, roll = 0;
	};
	const auto make = [&](const char* name, const char* next, int frames, bool moves, const std::function<void(float, float, Step&)>& fn) {
		SrcClip clip;
		clip.name = name;
		clip.next = next;
		clip.fps = 25.0f;
		clip.moves = moves;
		clip.blendIn = clip.blendOut = 0.2f;
		clip.nodes = {C_ROOT, C_HEAD, C_FL, C_FR, C_BL, C_BR};
		for (int f = 0; f < frames; ++f) {
			Step st;
			fn(float(f) / float(frames), float(f) / clip.fps, st);
			for (int node = 0; node < C_NODES; ++node) {
				SrcClipSample sample {};
				std::copy_n(model.nodes[size_t(node)].pos, 3, sample.pos);
				sample.rot[3] = 1.0f;
				if (node == C_ROOT) {
					sample.pos[1] += st.rootY * size;
					sample.pos[2] += st.rootZ * size;
					// (Pitch, then roll onto its side.)
					const float cp = std::cos(st.pitch * 0.5f), sp = std::sin(st.pitch * 0.5f);
					const float cr = std::cos(st.roll * 0.5f), sr = std::sin(st.roll * 0.5f);
					sample.rot[0] = cr * sp;
					sample.rot[1] = -sr * sp;
					sample.rot[2] = sr * cp;
					sample.rot[3] = cr * cp;
				} else if (node == C_HEAD) {
					// (Yaw, then pitch.)
					const float cy = std::cos(st.headYaw * 0.5f), sy = std::sin(st.headYaw * 0.5f);
					const float cp = std::cos(st.headPitch * 0.5f), sp = std::sin(st.headPitch * 0.5f);
					sample.rot[0] = cy * sp;
					sample.rot[1] = sy * cp;
					sample.rot[2] = -sy * sp;
					sample.rot[3] = cy * cp;
				} else {
					// Diagonal pairs swing together.
					const float swing = st.legs * ((node == C_FL || node == C_BR) ? 1.0f : -1.0f);
					sample.rot[0] = std::sin(swing * 0.5f);
					sample.rot[3] = std::cos(swing * 0.5f);
				}
				clip.samples.push_back(sample);
			}
		}
		model.clips.push_back(std::move(clip));
	};
	for (const char* stand : {"S_FISTRUN", "S_FISTWALK"})
		make(stand, stand, 40, false, [](float phase, float, Step& st) {
			const float sw = std::sin(phase * kTau);
			st.rootY = 1.5f * sw;
			st.headYaw = 0.2f * sw;
		});
	make("S_FISTWALKL", "S_FISTWALKL", 20, true, [](float phase, float seconds, Step& st) {
		st.rootZ = 110.0f * seconds;
		st.rootY = 1.0f * std::sin(phase * 2.0f * kTau);
		st.legs = 0.45f * std::sin(phase * kTau);
	});
	make("S_FISTRUNL", "S_FISTRUNL", 12, true, [](float phase, float seconds, Step& st) {
		st.rootZ = 420.0f * seconds;
		st.rootY = 3.0f * std::sin(phase * 2.0f * kTau);
		st.legs = 0.8f * std::sin(phase * kTau);
	});
	make("T_STAND_2_SLEEP", "S_SLEEP", 15, false, [](float phase, float, Step& st) {
		st.rootY = -30.0f * phase;
		st.legs = 0.0f;
		st.headPitch = 0.5f * phase;
	});
	make("S_SLEEP", "S_SLEEP", 50, false, [](float phase, float, Step& st) {
		st.rootY = -30.0f + 1.0f * std::sin(phase * kTau);
		st.headPitch = 0.5f;
	});
	make("T_SLEEP_2_STAND", "S_FISTRUN", 12, false, [](float phase, float, Step& st) {
		st.rootY = -30.0f * (1.0f - phase);
		st.headPitch = 0.5f * (1.0f - phase);
	});
	make("T_WARN", "S_FISTRUN", 20, false, [](float phase, float, Step& st) {
		st.pitch = -0.25f * std::sin(phase * kTau * 0.5f);
		st.headPitch = -0.4f * std::sin(phase * kTau * 0.5f);
	});
	// Fighting: a bite (the blow lands at frame 7), a hop back, being hit,
	// falling over dead.
	make("S_FISTATTACK", "S_FISTRUN", 18, false, [](float phase, float, Step& st) {
		const float lunge = std::sin(std::min(phase * 1.3f, 1.0f) * kTau * 0.5f);
		st.pitch = 0.2f * lunge;
		st.rootZ = 0.0f;
		st.headPitch = 0.5f * lunge;
	});
	model.clips.back().hits = {7};
	model.clips.back().hitEnds = {15};
	make("T_FISTPARADEJUMPB", "S_FISTRUN", 10, true, [](float phase, float, Step& st) {
		st.rootZ = -80.0f * phase;
		st.rootY = 8.0f * std::sin(phase * kTau * 0.5f);
	});
	make("T_STUMBLE", "S_FISTRUN", 9, false, [](float phase, float, Step& st) { st.pitch = -0.3f * std::sin(phase * kTau * 0.5f); });
	make("T_DEAD", "S_DEAD", 15, false, [](float phase, float, Step& st) {
		st.roll = 1.5f * phase;
		st.rootY = -18.0f * phase;
	});
	make("S_DEAD", "S_DEAD", 10, false, [](float, float, Step& st) {
		st.roll = 1.5f;
		st.rootY = -18.0f;
	});
}

// What the people of the camp do beside standing and going: sitting on the
// ground, standing guard, a look around.
void addInhabitantClips(SrcModel& model) {
	const auto sit = [](float down, Pose& p) {
		p.pos[ROOT][1] -= 52.0f * down;
		p.rot[L_THIGH] = p.rot[R_THIGH] = aboutX(-1.45f * down);
		p.rot[L_CALF] = p.rot[R_CALF] = aboutX(1.2f * down);
		p.rot[CHEST] = aboutX(0.15f * down);
	};
	model.clips.push_back(makeClip("T_STAND_2_SIT", "S_SIT", 20, false, [sit](float phase, float, Pose& p) { sit(phase, p); }));
	model.clips.push_back(makeClip("S_SIT", "S_SIT", 50, false, [sit](float phase, float, Pose& p) {
		sit(1.0f, p);
		p.rot[HEAD] = aboutX(0.06f * std::sin(phase * kTau));
	}));
	model.clips.push_back(makeClip("T_SIT_2_STAND", "S_RUN", 18, false, [sit](float phase, float, Pose& p) { sit(1.0f - phase, p); }));
	const auto guard = [](float in, Pose& p) {
		p.rot[L_ARM] = aboutX(-0.9f * in);
		p.rot[R_ARM] = aboutX(-0.9f * in);
		p.rot[L_FOREARM] = aboutX(-1.3f * in);
		p.rot[R_FOREARM] = aboutX(-1.3f * in);
	};
	model.clips.push_back(makeClip("T_STAND_2_LGUARD", "S_LGUARD", 12, false, [guard](float phase, float, Pose& p) { guard(phase, p); }));
	model.clips.push_back(makeClip("S_LGUARD", "S_LGUARD", 60, false, [guard](float phase, float, Pose& p) {
		guard(1.0f, p);
		p.rot[CHEST] = aboutX(0.02f * std::sin(phase * kTau));
	}));
	model.clips.push_back(makeClip("T_LGUARD_2_STAND", "S_RUN", 10, false, [guard](float phase, float, Pose& p) { guard(1.0f - phase, p); }));
	model.clips.push_back(makeClip("T_LGUARD_SCRATCH", "S_LGUARD", 25, false, [guard](float phase, float, Pose& p) {
		guard(1.0f, p);
		p.rot[R_ARM] = aboutX(-0.9f - 1.4f * std::sin(phase * kTau * 0.5f));
	}));
	model.clips.push_back(makeClip("T_SEARCH", "S_RUN", 40, false, [](float phase, float, Pose& p) {
		const float turn = 0.7f * std::sin(phase * kTau);
		p.rot[HEAD] = {0, std::sin(turn * 0.5f), 0, std::cos(turn * 0.5f)};
	}));
	// Using the things of the camp. A bench: back onto it and down, and up
	// again - made as the game's are, without the flag that says the clip
	// moves the character: the step back is in the root, which stays half
	// a metre back for as long as one sits (in model space, as the game's
	// animations keep it).
	const auto lie = [](float down, Pose& p) {
		p.pos[ROOT][1] -= 30.0f * down;
		p.rot[ROOT] = aboutX(-1.45f * down);
	};
	const float seat = -40.0f;
	model.clips.push_back(makeClip("T_BENCH_STAND_2_S0", "S_BENCH_S0", 10, false, [seat](float phase, float, Pose& p) {
		p.pos[ROOT][2] = seat * phase;
	}));
	model.clips.push_back(makeClip("S_BENCH_S0", "S_BENCH_S0", 20, false, [seat](float, float, Pose& p) { p.pos[ROOT][2] = seat; }));
	model.clips.push_back(makeClip("T_BENCH_S0_2_S1", "S_BENCH_S1", 12, false, [sit, seat](float phase, float, Pose& p) {
		sit(0.8f * phase, p);
		p.pos[ROOT][2] = seat;
	}));
	model.clips.push_back(makeClip(
	    "S_BENCH_S1", "S_BENCH_S1", 50, false,
	    [sit, seat](float phase, float, Pose& p) {
		    sit(0.8f, p);
		    p.pos[ROOT][2] = seat;
		    p.rot[HEAD] = aboutX(0.05f * std::sin(phase * kTau));
	    },
	    false));
	model.clips.push_back(makeClip("T_BENCH_S1_2_S0", "S_BENCH_S0", 12, false, [sit, seat](float phase, float, Pose& p) {
		sit(0.8f * (1.0f - phase), p);
		p.pos[ROOT][2] = seat;
	}));
	model.clips.push_back(makeClip("T_BENCH_S0_2_STAND", "S_RUN", 10, false, [seat](float phase, float, Pose& p) {
		p.pos[ROOT][2] = seat * (1.0f - phase);
	}));
	// A bed, got into from its front: those steps have the side in their
	// names, lying in it has not.
	model.clips.push_back(makeClip("T_BEDHIGH_FRONT_STAND_2_S0", "S_BEDHIGH_S0", 10, false, [sit](float phase, float, Pose& p) { sit(0.5f * phase, p); }));
	model.clips.push_back(makeClip("S_BEDHIGH_S0", "S_BEDHIGH_S0", 20, false, [sit](float, float, Pose& p) { sit(0.5f, p); }));
	model.clips.push_back(makeClip("T_BEDHIGH_FRONT_S0_2_S1", "S_BEDHIGH_S1", 14, false, [sit, lie](float phase, float, Pose& p) {
		sit(0.5f * (1.0f - phase), p);
		lie(phase, p);
	}));
	model.clips.push_back(makeClip(
	    "S_BEDHIGH_S1", "S_BEDHIGH_S1", 60, false,
	    [lie](float phase, float, Pose& p) {
		    lie(1.0f, p);
		    p.rot[CHEST] = aboutX(0.03f * std::sin(phase * kTau));
	    },
	    false));
	model.clips.push_back(makeClip("T_BEDHIGH_S1_2_S0", "S_BEDHIGH_S0", 14, false, [sit, lie](float phase, float, Pose& p) {
		sit(0.5f * phase, p);
		lie(1.0f - phase, p);
	}));
	model.clips.push_back(makeClip("T_BEDHIGH_FRONT_S0_2_STAND", "S_RUN", 10, false, [sit](float phase, float, Pose& p) { sit(0.5f * (1.0f - phase), p); }));
	// An anvil: a thing with one state, worked at standing.
	model.clips.push_back(makeClip("T_BSANVIL_STAND_2_S0", "S_BSANVIL_S0", 8, false, [](float phase, float, Pose& p) {
		p.rot[R_ARM] = aboutX(-1.2f * phase);
	}));
	model.clips.push_back(makeClip("S_BSANVIL_S0", "S_BSANVIL_S0", 16, false, [](float phase, float, Pose& p) {
		p.rot[R_ARM] = aboutX(-1.2f);
		p.rot[R_FOREARM] = aboutX(-0.9f - 0.8f * std::sin(phase * kTau));
	}));
	model.clips.push_back(makeClip("T_BSANVIL_S0_2_STAND", "S_RUN", 8, false, [](float phase, float, Pose& p) {
		p.rot[R_ARM] = aboutX(-1.2f * (1.0f - phase));
	}));
	// Going at a walk begins and ends with a step of its own.
	model.clips.push_back(makeClip("T_WALKL_2_WALK", "S_WALK", 6, true, [](float phase, float seconds, Pose& p) {
		stride(0.0f, 0.0f, 0.4f * (1.0f - phase), 0.0f, p);
		const float whole = 5.0f / 25.0f;
		p.pos[ROOT][2] = 150.0f * seconds - 0.5f * 150.0f * seconds * seconds / whole; // from 150 cm/s to rest
	}));
}

bool isSyntheticCreature(const std::string& script) { return script == "scavenger" || script == "molerat"; }

} // namespace

bool buildSyntheticRig(const std::string& script, bool, const std::set<std::string>& wanted, SrcModel& model) {
	// Of all it can do: standing and going, and what the scripts name.
	std::set<std::string> keep = {"S_RUN", "S_RUNL", "S_WALK", "S_WALKL", "T_RUN_2_RUNL", "T_RUNL_2_RUN", "T_WALK_2_WALKL",
	                              "T_WALKL_2_WALK", "S_FISTRUN", "S_FISTRUNL", "S_FISTWALK", "S_FISTWALKL"};
	std::vector<std::string> families; // names ending in a star: all that begin like them
	for (const std::string& name : wanted) {
		if (!name.empty() && name.back() == '*')
			families.push_back(name.substr(0, name.size() - 1));
		else
			keep.insert(name);
	}
	if (script == "humans") {
		buildSyntheticActor(model);
		addInhabitantClips(model);
	} else if (isSyntheticCreature(script)) {
		buildCreature(script, model);
	} else {
		model.notes.push_back("the synthetic scene has no '" + script + "'");
		return false;
	}
	for (const SrcClip& clip : model.clips)
		for (const std::string& prefix : families)
			if (clip.name.compare(0, prefix.size(), prefix) == 0) keep.insert(clip.name);
	// (What a kept animation leads into is kept with it.)
	for (int pass = 0; pass < 3; ++pass)
		for (const SrcClip& clip : model.clips)
			if (keep.count(clip.name) && !clip.next.empty()) keep.insert(clip.next);
	model.clips.erase(std::remove_if(model.clips.begin(), model.clips.end(), [&](const SrcClip& c) { return keep.count(c.name) == 0; }),
	                  model.clips.end());
	model.parts.clear();
	model.blends.clear();
	model.notes.clear();
	return true;
}

bool buildSyntheticBody(const std::string& script, const std::string& mesh, int tex, int skin, SrcModel& model) {
	if (script == "humans") {
		buildSyntheticActor(model);
		model.parts.resize(1); // the body; the head is a model of its own
		model.parts[0].material.texture = upperName(mesh) + "_V" + std::to_string(tex) + "_C" + std::to_string(skin);
	} else if (isSyntheticCreature(script)) {
		buildCreature(script, model);
		model.parts[0].material.texture = upperName(mesh);
	} else {
		model.notes.push_back("the synthetic scene has no '" + script + "'");
		return false;
	}
	model.parts[0].material.name = mesh;
	model.loadTexture = paintLook;
	model.clips.clear();
	model.blends.clear();
	model.notes.clear();
	return true;
}

// A thing lying about: boxes in the thing's shape (a blade with a guard and
// a grip, a flask with a neck, a block for the rest), in a colour of its own.
bool buildSyntheticItem(const std::string& visual, SrcModel& model) {
	struct Piece {
		float c[3], h[3];
	};
	std::vector<Piece> pieces;
	if (visual.find("itmw") == 0) {
		const float len = visual.find("2h") != std::string::npos ? 60.0f : 40.0f;
		pieces = {{{0, 1, 8}, {3, 1, len}}, {{0, 1.5f, 8 - len}, {9, 2, 2}}, {{0, 1.5f, 8 - len - 10}, {1.5f, 1.5f, 8}}};
	} else if (visual.find("potion") != std::string::npos || visual.find("beer") != std::string::npos) {
		pieces = {{{0, 6, 0}, {5, 6, 5}}, {{0, 15, 0}, {2, 3, 2}}};
	} else if (visual.find("itrw") == 0) {
		pieces = {{{0, 2, 0}, {1.5f, 1.5f, 50}}, {{0, 2, 0}, {0.4f, 0.4f, 45}}};
	} else {
		pieces = {{{0, 6, 0}, {6, 6, 6}}};
	}
	model.nodes.assign(1, SrcNode {});
	model.nodes[0].name = "ITEM";
	SrcModelPart part;
	part.material.name = visual;
	part.material.texture = "ITEM_" + upperName(visual);
	const int faces[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 4, 7, 3}, {1, 2, 6, 5}, {3, 7, 6, 2}, {0, 1, 5, 4}};
	const float normals[6][3] = {{0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
	const float uv[4][2] = {{0, 1}, {0, 0}, {1, 0}, {1, 1}};
	for (const Piece& p : pieces) {
		const float x0 = p.c[0] - p.h[0], x1 = p.c[0] + p.h[0], y0 = p.c[1] - p.h[1], y1 = p.c[1] + p.h[1], z0 = p.c[2] - p.h[2],
		            z1 = p.c[2] + p.h[2];
		const float c[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0}, {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
		for (int f = 0; f < 6; ++f)
			for (int k : {0, 1, 2, 0, 2, 3}) {
				SrcSkinVertex v;
				std::memcpy(v.pos, c[faces[f][k]], sizeof(v.pos));
				std::memcpy(v.normal, normals[f], sizeof(v.normal));
				v.uv[0] = uv[k][0];
				v.uv[1] = uv[k][1];
				v.bone[0] = v.bone[1] = 0;
				part.verts.push_back(v);
			}
	}
	model.parts.assign(1, std::move(part));
	model.loadTexture = [](const std::string& name, SrcImage& out) {
		if (name.compare(0, 5, "ITEM_") != 0) return false;
		u32 h = 2166136261u;
		for (char ch : name) h = (h ^ u8(ch)) * 16777619u;
		out.width = out.height = 8;
		out.rgba.assign(8 * 8 * 4, 255);
		for (int i = 0; i < 64; ++i) {
			const int shade = (i % 8 == 0 || i / 8 == 0) ? 60 : 0; // (an edge, to see it turn)
			out.rgba[size_t(i) * 4 + 0] = u8(std::max(int(80 + (h & 127)) - shade, 0));
			out.rgba[size_t(i) * 4 + 1] = u8(std::max(int(80 + ((h >> 8) & 127)) - shade, 0));
			out.rgba[size_t(i) * 4 + 2] = u8(std::max(int(80 + ((h >> 16) & 127)) - shade, 0));
		}
		return true;
	};
	return true;
}

bool buildSyntheticHead(const std::string& script, const std::string& head, int tex, int skin, int, SrcModel& model) {
	if (script != "humans") {
		model.notes.push_back("only people have heads of their own in the synthetic scene");
		return false;
	}
	buildSyntheticActor(model);
	model.parts.erase(model.parts.begin());
	model.parts[0].material.name = head;
	model.parts[0].material.texture = upperName(head) + "_V" + std::to_string(tex) + "_C" + std::to_string(skin);
	model.loadTexture = paintLook;
	model.clips.clear();
	model.blends.clear();
	model.notes.clear();
	return true;
}

} // namespace g3
