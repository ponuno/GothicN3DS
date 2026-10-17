// fix201: see video_bake.h and g3ds/video.h.
#include "video_bake.h"

#include "g3ds/sound.h"
#include "g3ds/video.h"
#include "lz4hc.h"
#include "rg_etc1.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

#ifdef _WIN32
#define g3_popen _popen
#define g3_pclose _pclose
#define G3_PIPE_READ "rb"
#else
#define G3_PIPE_READ "r" // (glibc's popen takes no "b")
#define g3_popen popen
#define g3_pclose pclose
#endif

namespace g3 {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
	for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

// A command line for the shell: the program and the file quoted (cmd.exe wants the whole line quoted once more).
std::string commandLine(const std::string& ffmpeg, const std::string& args) {
	std::string line = "\"" + ffmpeg + "\" " + args;
#ifdef _WIN32
	line = "\"" + line + "\"";
#endif
	return line;
}

bool runsFfmpeg(const std::string& program) {
#ifdef _WIN32
	const std::string line = commandLine(program, "-version 2>NUL");
#else
	const std::string line = commandLine(program, "-version 2>/dev/null");
#endif
	FILE* p = g3_popen(line.c_str(), "r");
	if (!p) return false;
	char buf[256];
	bool seen = false;
	while (std::fgets(buf, sizeof(buf), p))
		if (std::strstr(buf, "ffmpeg version")) seen = true;
	return g3_pclose(p) == 0 && seen;
}

// Reads one binary PPM ("P6 w h 255\n" + w*h*3 bytes) from a pipe. False at the end.
bool readPpm(FILE* p, u32& w, u32& h, std::vector<u8>& rgb) {
	const auto token = [&](u32& out) {
		int c = std::fgetc(p);
		while (c == ' ' || c == '\n' || c == '\r' || c == '\t') c = std::fgetc(p);
		if (c == EOF) return false;
		u32 v = 0;
		bool any = false;
		while (c >= '0' && c <= '9') {
			v = v * 10u + u32(c - '0');
			any = true;
			c = std::fgetc(p);
		}
		out = v;
		return any; // (the one white space after the number is eaten)
	};
	int c = std::fgetc(p);
	if (c == EOF) return false;
	if (c != 'P' || std::fgetc(p) != '6') return false;
	u32 maxv = 0;
	if (!token(w) || !token(h) || !token(maxv) || maxv != 255 || w == 0 || h == 0 || w > 4096 || h > 4096) return false;
	rgb.resize(size_t(w) * h * 3);
	return std::fread(rgb.data(), 1, rgb.size(), p) == rgb.size();
}

// The IMA ADPCM chunk of g3ds/sound.h (its own predictor at the start): the codes whose decoded samples land nearest.
std::vector<u8> imaChunk(const i16* s, u32 frames) {
	std::vector<u8> out(imaBytes(frames), 0);
	if (!frames) return out;
	ImaState st;
	st.predictor = s[0];
	if (frames > 1) {
		const int d = std::abs(int(s[1]) - int(s[0]));
		while (st.index < 88 && IMA_STEPS[st.index] < d / 2) ++st.index;
	}
	out[0] = u8(st.predictor & 0xFF);
	out[1] = u8((st.predictor >> 8) & 0xFF);
	out[2] = u8(st.index);
	u8* nibbles = out.data() + 4;
	for (u32 i = 1; i < frames; ++i) {
		u32 best = 0;
		int bestErr = 1 << 30;
		for (u32 code = 0; code < 16; ++code) {
			ImaState t = st;
			const int err = std::abs(int(imaStep(t, code)) - int(s[i]));
			if (err < bestErr) {
				bestErr = err;
				best = code;
			}
		}
		imaStep(st, best);
		const u32 k = i - 1;
		nibbles[k >> 1] = u8(nibbles[k >> 1] | (best << ((k & 1) * 4)));
	}
	return out;
}

void put(std::vector<u8>& v, const void* p, size_t n) {
	const u8* b = static_cast<const u8*>(p);
	v.insert(v.end(), b, b + n);
}

} // namespace

std::string findFfmpeg(const std::string& given, const std::string& exeDir) {
	if (!given.empty()) return runsFfmpeg(given) ? given : std::string();
	std::error_code ec;
	for (const char* name : {"ffmpeg.exe", "ffmpeg"}) {
		const fs::path beside = fs::path(exeDir.empty() ? "." : exeDir) / name;
		if (fs::exists(beside, ec) && runsFfmpeg(beside.string())) return beside.string();
	}
	return runsFfmpeg("ffmpeg") ? std::string("ffmpeg") : std::string();
}

std::vector<std::string> findGameVideos(const std::string& gameDir) {
	std::vector<std::string> out;
	std::error_code ec;
	if (gameDir.empty() || !fs::is_directory(gameDir, ec)) return out;
	// _work/Data/Video, whatever the letter case of each part.
	fs::path at = gameDir;
	for (const char* part : {"_work", "data", "video"}) {
		bool found = false;
		for (const auto& e : fs::directory_iterator(at, ec)) {
			if (e.is_directory(ec) && lower(e.path().filename().string()) == part) {
				at = e.path();
				found = true;
				break;
			}
		}
		if (!found) return out;
	}
	for (const auto& e : fs::directory_iterator(at, ec))
		if (e.is_regular_file(ec) && lower(e.path().extension().string()) == ".bik") out.push_back(e.path().string());
	std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
	return out;
}

std::string videoEntryName(const std::string& file) {
	// (Either kind of slash, whatever the system: names from a Windows game may be read anywhere.)
	const size_t slash = file.find_last_of("/\\");
	std::string base = slash == std::string::npos ? file : file.substr(slash + 1);
	const size_t dot = base.find('.');
	if (dot != std::string::npos) base.resize(dot);
	return "video/" + lower(base);
}

VideoBakeResult bakeVideo(const std::string& file, const VideoBakeOptions& opt, std::vector<u8>& out) {
	VideoBakeResult r;
	out.clear();
	if (opt.ffmpeg.empty()) {
		r.why = "no ffmpeg";
		return r;
	}
	static std::once_flag once;
	std::call_once(once, [] { rg_etc1::pack_etc1_block_init(); });
	const u32 maxW = std::min<u32>(opt.maxW, VIDEO_MAX_W) & ~7u, maxH = std::min<u32>(opt.maxH, VIDEO_MAX_H) & ~7u;
	const u32 fps = std::clamp<u32>(opt.fps, 1, 30);

	// ---- the sound: mono 16-bit at audioRate ----
	std::vector<i16> sound;
	if (opt.audioRate > 0) {
		char args[512];
		std::snprintf(args, sizeof(args), "-v quiet -nostdin -i \"%s\" -vn -ac 1 -ar %u -f s16le -", file.c_str(), opt.audioRate);
		if (FILE* p = g3_popen(commandLine(opt.ffmpeg, args).c_str(), G3_PIPE_READ)) {
			i16 buf[4096];
			size_t n;
			while ((n = std::fread(buf, 2, 4096, p)) > 0) sound.insert(sound.end(), buf, buf + n);
			g3_pclose(p);
		}
	}

	// ---- the pictures: fitted into maxW x maxH, multiples of 8, `fps` a second, as PPM one after the other ----
	char args[1024];
	std::snprintf(args, sizeof(args),
	              "-v quiet -nostdin -i \"%s\" -an -vf \"fps=%u,scale=w=%u:h=%u:force_original_aspect_ratio=decrease:flags=area,"
	              "scale=w='trunc(iw/8)*8':h='trunc(ih/8)*8'\" -f image2pipe -vcodec ppm -",
	              file.c_str(), fps, maxW, maxH);
	FILE* p = g3_popen(commandLine(opt.ffmpeg, args).c_str(), G3_PIPE_READ);
	if (!p) {
		r.why = "ffmpeg did not start";
		return r;
	}
	u32 w = 0, h = 0;
	std::vector<u8> rgb, ref;               // this frame; what the blocks shown were made from
	std::vector<std::vector<u8>> frames;    // packed
	std::vector<u32> rawSizes;
	u32 maxRaw = 0, maxPacked = 0;
	std::vector<u8> changed, blocks, raw, packed;
	std::vector<u8> send;
	while (true) {
		u32 fw = 0, fh = 0;
		if (!readPpm(p, fw, fh, rgb)) break;
		if (frames.empty()) {
			w = fw;
			h = fh;
			if (w % 8 || h % 8 || w > VIDEO_MAX_W || h > VIDEO_MAX_H) {
				r.why = "ffmpeg gave a picture of " + std::to_string(w) + "x" + std::to_string(h);
				break;
			}
			ref.assign(rgb.size(), 0);
		} else if (fw != w || fh != h) {
			r.why = "the picture changed its size";
			break;
		}
		const u32 bw = w / 4, bh = h / 4, count = bw * bh;
		changed.assign((count + 7) / 8, 0);
		send.assign(count, 0);
		const bool first = frames.empty();
		for (u32 by = 0; by < bh; ++by)
			for (u32 bx = 0; bx < bw; ++bx) {
				bool differs = first;
				if (!first) {
					u32 sum = 0;
					for (u32 y = 0; y < 4 && !differs; ++y) {
						const size_t row = (size_t(by * 4 + y) * w + bx * 4) * 3;
						for (u32 k = 0; k < 12; ++k) {
							const int d = int(rgb[row + k]) - int(ref[row + k]);
							sum += u32(d * d);
							if (d > 40 || d < -40) differs = true;
						}
					}
					if (float(sum) / 48.0f > opt.sameBlock) differs = true;
				}
				if (differs) send[by * bw + bx] = 1;
			}
		// The blocks sent, encoded (rows of blocks shared out among the threads).
		std::vector<u32> order;
		for (u32 b = 0; b < count; ++b)
			if (send[b]) order.push_back(b);
		blocks.assign(order.size() * 8, 0);
		rg_etc1::etc1_pack_params params;
		params.m_quality = rg_etc1::cLowQuality;
		const auto encode = [&](size_t from, size_t to) {
			for (size_t k = from; k < to; ++k) {
				const u32 b = order[k], bx = b % bw, by = b / bw;
				unsigned int block[16];
				for (u32 y = 0; y < 4; ++y) {
					// (Block row y is the picture's row 4*by + 3 - y: the PICA stores rows from the bottom.)
					const size_t row = (size_t(by * 4 + 3 - y) * w + bx * 4) * 3;
					for (u32 x = 0; x < 4; ++x) {
						const u8 q[4] = {rgb[row + x * 3], rgb[row + x * 3 + 1], rgb[row + x * 3 + 2], 255};
						std::memcpy(&block[y * 4 + x], q, 4);
					}
				}
				u8 etc[8];
				rg_etc1::pack_etc1_block(etc, block, params);
				for (int i = 0; i < 8; ++i) blocks[k * 8 + size_t(i)] = etc[7 - i];
			}
		};
		const unsigned threads = std::max(1u, std::min(opt.threads, 16u));
		if (threads <= 1 || order.size() < 256) {
			encode(0, order.size());
		} else {
			std::vector<std::thread> pool;
			const size_t per = (order.size() + threads - 1) / threads;
			for (unsigned t = 0; t < threads; ++t) {
				const size_t from = t * per, to = std::min(order.size(), from + per);
				if (from < to) pool.emplace_back(encode, from, to);
			}
			for (auto& th : pool) th.join();
		}
		for (const u32 b : order) {
			changed[b >> 3] = u8(changed[b >> 3] | (1u << (b & 7)));
			const u32 bx = b % bw, by = b / bw;
			for (u32 y = 0; y < 4; ++y) {
				const size_t row = (size_t(by * 4 + y) * w + bx * 4) * 3;
				std::memcpy(&ref[row], &rgb[row], 12);
			}
		}
		r.blocksSent += order.size();
		r.blocksAll += count;
		raw.clear();
		put(raw, changed.data(), changed.size());
		put(raw, blocks.data(), blocks.size());
		packed.resize(size_t(LZ4_compressBound(int(raw.size()))));
		const int n = LZ4_compress_HC(reinterpret_cast<const char*>(raw.data()), reinterpret_cast<char*>(packed.data()), int(raw.size()),
		                              int(packed.size()), 9);
		if (n <= 0) {
			r.why = "LZ4 failed";
			break;
		}
		frames.emplace_back(packed.begin(), packed.begin() + n);
		rawSizes.push_back(u32(raw.size()));
		maxRaw = std::max(maxRaw, u32(raw.size()));
		maxPacked = std::max(maxPacked, u32(n));
	}
	g3_pclose(p);
	if (!r.why.empty()) return r;
	if (frames.empty()) {
		r.why = "ffmpeg gave no pictures (not a video it reads?)";
		return r;
	}

	// ---- the entry ----
	const u32 chunkFrames = opt.audioRate > 0 ? opt.audioRate : 1;
	const u32 audioChunks = sound.empty() ? 0u : u32((sound.size() + chunkFrames - 1) / chunkFrames);
	VideoHeader vh {};
	vh.magic = VIDEO_MAGIC;
	vh.version = VIDEO_VERSION;
	vh.width = u16(w);
	vh.height = u16(h);
	vh.frameCount = u32(frames.size());
	vh.fpsMilli = fps * 1000u;
	vh.audioRate = sound.empty() ? 0u : opt.audioRate;
	vh.audioFrames = u32(sound.size());
	vh.audioChunks = audioChunks;
	vh.audioChunkFrames = sound.empty() ? 0u : chunkFrames;
	vh.maxFrameRaw = maxRaw;
	vh.maxFramePacked = maxPacked;
	std::vector<std::vector<u8>> audio;
	for (u32 c = 0; c < audioChunks; ++c) {
		const size_t from = size_t(c) * chunkFrames, n = std::min<size_t>(chunkFrames, sound.size() - from);
		audio.push_back(imaChunk(sound.data() + from, u32(n)));
	}
	size_t at = sizeof(VideoHeader) + sizeof(VideoChunk) * (frames.size() + audio.size());
	std::vector<VideoChunk> table;
	for (size_t i = 0; i < frames.size(); ++i) {
		table.push_back(VideoChunk {u32(at), u32(frames[i].size()), rawSizes[i]});
		at += frames[i].size();
	}
	for (u32 c = 0; c < audioChunks; ++c) {
		const u32 n = u32(std::min<size_t>(chunkFrames, sound.size() - size_t(c) * chunkFrames));
		table.push_back(VideoChunk {u32(at), u32(audio[c].size()), n});
		at += audio[c].size();
	}
	if (at >= (u64(1) << 31)) {
		r.why = "too big for one entry";
		return r;
	}
	out.reserve(at);
	put(out, &vh, sizeof(vh));
	put(out, table.data(), table.size() * sizeof(VideoChunk));
	for (const auto& f : frames) put(out, f.data(), f.size());
	for (const auto& a : audio) put(out, a.data(), a.size());
	r.ok = true;
	r.width = w;
	r.height = h;
	r.frames = u32(frames.size());
	r.audioFrames = u32(sound.size());
	return r;
}

} // namespace g3
