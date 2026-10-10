#include "item_icon.h"

#include "g3ds/item_view.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>

namespace g3 {

namespace {

struct Tex {
	SrcImage image;
	bool ok = false;
	bool alpha = false; // some texel is not opaque: alpha-tested
};

} // namespace

std::vector<u8> renderItemIcon(const SrcModel& model, int size) {
	const int S = size * 2; // drawn twice as large, then averaged down
	// The textures.
	std::map<std::string, Tex> textures;
	for (const SrcModelPart& part : model.parts) {
		const std::string& name = part.material.texture;
		if (name.empty() || textures.count(name)) continue;
		Tex& t = textures[name];
		t.ok = model.loadTexture && model.loadTexture(name, t.image) && t.image.width > 0 && t.image.height > 0 &&
		       t.image.rgba.size() >= size_t(t.image.width) * size_t(t.image.height) * 4;
		if (t.ok)
			for (size_t i = 3; i < t.image.rgba.size(); i += 4)
				if (t.image.rgba[i] < 128) {
					t.alpha = true;
					break;
				}
	}
	// The bounding box, then the turn of the showcase.
	float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
	size_t count = 0;
	for (const SrcModelPart& part : model.parts)
		for (const SrcSkinVertex& v : part.verts) {
			for (int a = 0; a < 3; ++a) {
				mn[a] = std::min(mn[a], v.pos[a]);
				mx[a] = std::max(mx[a], v.pos[a]);
			}
			++count;
		}
	if (count < 3) return {};
	const Mat34 turn = itemShowcase(mn, mx);
	const auto view = [&](const float p[3]) {
		return Vec3 {turn.m[0][0] * p[0] + turn.m[0][1] * p[1] + turn.m[0][2] * p[2],
		             turn.m[1][0] * p[0] + turn.m[1][1] * p[1] + turn.m[1][2] * p[2],
		             turn.m[2][0] * p[0] + turn.m[2][1] * p[1] + turn.m[2][2] * p[2]};
	};
	float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
	for (const SrcModelPart& part : model.parts)
		for (const SrcSkinVertex& v : part.verts) {
			const Vec3 q = view(v.pos);
			lo[0] = std::min(lo[0], q.x);
			lo[1] = std::min(lo[1], q.y);
			hi[0] = std::max(hi[0], q.x);
			hi[1] = std::max(hi[1], q.y);
		}
	const float extent = std::max(std::max(hi[0] - lo[0], hi[1] - lo[1]), 0.01f);
	const float scale = float(S) * 0.9f / extent;
	const float cx = (lo[0] + hi[0]) * 0.5f, cy = (lo[1] + hi[1]) * 0.5f;

	std::vector<float> depth(size_t(S) * size_t(S), 1e30f);
	std::vector<u8> colour(size_t(S) * size_t(S) * 4, 0);
	const Vec3 light = normalize({-0.5f, 0.7f, -0.6f});
	for (const SrcModelPart& part : model.parts) {
		const auto found = textures.find(part.material.texture);
		const Tex* tex = found != textures.end() && found->second.ok ? &found->second : nullptr;
		for (size_t t = 0; t + 2 < part.verts.size(); t += 3) {
			Vec3 q[3];
			float sx[3], sy[3];
			for (int k = 0; k < 3; ++k) {
				q[k] = view(part.verts[t + size_t(k)].pos);
				sx[k] = float(S) * 0.5f + (q[k].x - cx) * scale;
				sy[k] = float(S) * 0.5f - (q[k].y - cy) * scale;
			}
			// Lit by the face's own normal, whichever way it was wound.
			Vec3 n = normalize(cross(q[1] - q[0], q[2] - q[0]));
			if (n.z > 0) n = n * -1.0f;
			const float shade = std::min(0.55f + 0.65f * std::max(dot(n, light), 0.0f), 1.15f);
			const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
			if (std::fabs(area) < 1e-6f) continue;
			const int x0 = std::max(int(std::floor(std::min({sx[0], sx[1], sx[2]}))), 0);
			const int x1 = std::min(int(std::ceil(std::max({sx[0], sx[1], sx[2]}))), S - 1);
			const int y0 = std::max(int(std::floor(std::min({sy[0], sy[1], sy[2]}))), 0);
			const int y1 = std::min(int(std::ceil(std::max({sy[0], sy[1], sy[2]}))), S - 1);
			for (int y = y0; y <= y1; ++y)
				for (int x = x0; x <= x1; ++x) {
					const float px = float(x) + 0.5f, py = float(y) + 0.5f;
					const float w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
					const float w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
					const float w2 = 1.0f - w0 - w1;
					if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
					const float z = w0 * q[0].z + w1 * q[1].z + w2 * q[2].z;
					float& d = depth[size_t(y) * size_t(S) + size_t(x)];
					if (z >= d) continue;
					float rgb[3] = {160, 150, 140};
					if (part.material.hasColor)
						for (int c = 0; c < 3; ++c) rgb[c] = part.material.color[c];
					if (tex) {
						const SrcSkinVertex* v = &part.verts[t];
						float u = w0 * v[0].uv[0] + w1 * v[1].uv[0] + w2 * v[2].uv[0];
						float vv = w0 * v[0].uv[1] + w1 * v[1].uv[1] + w2 * v[2].uv[1];
						u -= std::floor(u);
						vv -= std::floor(vv);
						const int tx = std::min(int(u * float(tex->image.width)), tex->image.width - 1);
						const int ty = std::min(int(vv * float(tex->image.height)), tex->image.height - 1);
						const u8* texel = &tex->image.rgba[(size_t(ty) * size_t(tex->image.width) + size_t(tx)) * 4];
						if (tex->alpha && texel[3] < 128) continue;
						for (int c = 0; c < 3; ++c) rgb[c] = texel[c];
					}
					d = z;
					u8* o = &colour[(size_t(y) * size_t(S) + size_t(x)) * 4];
					for (int c = 0; c < 3; ++c) o[c] = u8(std::clamp(rgb[c] * shade, 0.0f, 255.0f));
					o[3] = 255;
				}
		}
	}
	// Averaged down: the edges come out soft.
	std::vector<u8> out(sizeof(IconHeader) + size_t(size) * size_t(size) * 4, 0);
	const IconHeader h {ITEMICON_MAGIC, ITEMICON_VERSION, u16(size), u16(size), 0};
	std::memcpy(out.data(), &h, sizeof(h));
	u8* px = out.data() + sizeof(h);
	bool any = false;
	for (int y = 0; y < size; ++y)
		for (int x = 0; x < size; ++x) {
			int sum[3] = {0, 0, 0}, covered = 0;
			for (int k = 0; k < 4; ++k) {
				const u8* s = &colour[(size_t(y * 2 + k / 2) * size_t(S) + size_t(x * 2 + k % 2)) * 4];
				if (!s[3]) continue;
				for (int c = 0; c < 3; ++c) sum[c] += s[c];
				++covered;
			}
			u8* o = &px[(size_t(y) * size_t(size) + size_t(x)) * 4];
			if (!covered) continue;
			for (int c = 0; c < 3; ++c) o[c] = u8(sum[c] / covered);
			o[3] = u8(covered * 255 / 4);
			any = true;
		}
	if (!any) return {};
	return out;
}

} // namespace g3
