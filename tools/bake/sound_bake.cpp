#include "sound_bake.h"
#include <iterator>
#include <fstream>
#include <filesystem>

#include "scene.h"
#include "script.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>

namespace g3 {

namespace {

u16 rd16(const u8* p) { return u16(p[0] | (p[1] << 8)); }
u32 rd32(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24); }
i16 clip16(int v) { return i16(std::clamp(v, -32768, 32767)); }

const int kImaStep[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
                          31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
                          130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
                          544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
                          2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
                          9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
const int kImaIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

// One IMA ADPCM block: per channel a 4-byte header (predictor, step index), then
// groups of 4 bytes (8 samples) per channel in turn. Samples are written
// interleaved into `out`.
void decodeImaBlock(const u8* block, u32 size, u32 channels, std::vector<i16>& out) {
	if (size < 4 * channels) return;
	int predictor[8], index[8];
	for (u32 c = 0; c < channels; ++c) {
		predictor[c] = i16(rd16(block + 4 * c));
		index[c] = std::clamp<int>(block[4 * c + 2], 0, 88);
	}
	const u32 groups = (size - 4 * channels) / (4 * channels);
	const size_t base = out.size();
	out.resize(base + size_t(groups) * 8 * channels + channels);
	for (u32 c = 0; c < channels; ++c) out[base + c] = i16(predictor[c]);
	size_t frame = 1;
	for (u32 g = 0; g < groups; ++g) {
		for (u32 c = 0; c < channels; ++c) {
			const u8* in = block + 4 * channels + (size_t(g) * channels + c) * 4;
			for (u32 b = 0; b < 4; ++b)
				for (u32 half = 0; half < 2; ++half) {
					const int nibble = half ? (in[b] >> 4) : (in[b] & 15);
					const int step = kImaStep[index[c]];
					int diff = step >> 3;
					if (nibble & 1) diff += step >> 2;
					if (nibble & 2) diff += step >> 1;
					if (nibble & 4) diff += step;
					predictor[c] = std::clamp(predictor[c] + ((nibble & 8) ? -diff : diff), -32768, 32767);
					index[c] = std::clamp(index[c] + kImaIndex[nibble], 0, 88);
					out[base + (frame + b * 2 + half) * channels + c] = i16(predictor[c]);
				}
		}
		frame += 8;
	}
}

const int kMsAdapt[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};

void decodeMsBlock(const u8* block, u32 size, u32 channels, const std::vector<std::pair<int, int>>& coef, std::vector<i16>& out) {
	const u32 head = 7 * channels;
	if (size < head || channels > 2) return;
	int pred[2], delta[2], s1[2], s2[2];
	for (u32 c = 0; c < channels; ++c) {
		pred[c] = std::min<int>(block[c], int(coef.size()) - 1);
		delta[c] = i16(rd16(block + channels + 2 * c));
		s1[c] = i16(rd16(block + 3 * channels + 2 * c));
		s2[c] = i16(rd16(block + 5 * channels + 2 * c));
	}
	for (u32 c = 0; c < channels; ++c) out.push_back(i16(s2[c]));
	for (u32 c = 0; c < channels; ++c) out.push_back(i16(s1[c]));
	u32 channel = 0;
	for (u32 i = head; i < size; ++i)
		for (u32 half = 0; half < 2; ++half) {
			const int nibble = half ? (block[i] & 15) : (block[i] >> 4);
			const int signedNibble = nibble >= 8 ? nibble - 16 : nibble;
			const int predicted = (s1[channel] * coef[size_t(pred[channel])].first + s2[channel] * coef[size_t(pred[channel])].second) / 256;
			const int value = std::clamp(predicted + signedNibble * delta[channel], -32768, 32767);
			s2[channel] = s1[channel];
			s1[channel] = value;
			delta[channel] = std::max(16, delta[channel] * kMsAdapt[nibble] / 256);
			out.push_back(i16(value));
			channel = (channel + 1) % channels;
		}
}

} // namespace

bool decodeWav(const std::vector<u8>& file, SrcSound& out, std::string& why, bool keepStereo) {
	out = SrcSound {};
	if (file.size() < 12 || std::memcmp(file.data(), "RIFF", 4) != 0 || std::memcmp(file.data() + 8, "WAVE", 4) != 0) {
		why = "not a WAV file";
		return false;
	}
	u32 format = 0, channels = 0, rate = 0, align = 0, bits = 0;
	std::vector<std::pair<int, int>> msCoef;
	const u8* data = nullptr;
	u32 dataSize = 0;
	for (size_t at = 12; at + 8 <= file.size();) {
		const u8* chunk = file.data() + at;
		u32 size = rd32(chunk + 4);
		const size_t body = at + 8;
		if (body + size > file.size()) size = u32(file.size() - body); // (a cut-off file: take what there is)
		if (std::memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
			format = rd16(file.data() + body);
			channels = rd16(file.data() + body + 2);
			rate = rd32(file.data() + body + 4);
			align = rd16(file.data() + body + 12);
			bits = rd16(file.data() + body + 14);
			if (format == 0xFFFE && size >= 26) format = rd16(file.data() + body + 24); // (extensible: the sub-format's first word)
			if (format == 2 && size >= 22) {
				const u32 count = rd16(file.data() + body + 20);
				for (u32 i = 0; i < count && 22 + (i + 1) * 4 <= size; ++i)
					msCoef.push_back({int(i16(rd16(file.data() + body + 22 + i * 4))), int(i16(rd16(file.data() + body + 24 + i * 4)))});
			}
		} else if (std::memcmp(chunk, "data", 4) == 0) {
			data = file.data() + body;
			dataSize = size;
			break;
		}
		at = body + size + (size & 1);
	}
	if (!data || channels == 0 || channels > 8 || rate < 1000 || rate > 192000) {
		why = "no usable fmt/data chunks";
		return false;
	}
	std::vector<i16> interleaved;
	if (format == 1 || format == 3) {
		const u32 bytes = bits / 8;
		if (bytes == 0 || bytes > 4 || (format == 3 && bytes != 4)) {
			why = "unsupported sample size";
			return false;
		}
		const size_t count = dataSize / bytes;
		interleaved.resize(count);
		for (size_t i = 0; i < count; ++i) {
			const u8* p = data + i * bytes;
			int v;
			if (format == 3) {
				float f;
				std::memcpy(&f, p, 4);
				v = int(std::lround(std::clamp(f, -1.0f, 1.0f) * 32767.0f));
			} else if (bytes == 1) {
				v = (int(p[0]) - 128) * 256;
			} else if (bytes == 2) {
				v = i16(rd16(p));
			} else if (bytes == 3) {
				v = i16(rd16(p + 1));
			} else {
				v = i16(rd16(p + 2));
			}
			interleaved[i] = clip16(v);
		}
	} else if (format == 0x11) {
		if (align < 4 * channels) {
			why = "bad IMA ADPCM block size";
			return false;
		}
		for (size_t at = 0; at + align <= dataSize; at += align) decodeImaBlock(data + at, align, channels, interleaved);
	} else if (format == 2) {
		if (msCoef.empty() || align < 7 * channels || channels > 2) {
			why = "bad Microsoft ADPCM header";
			return false;
		}
		for (size_t at = 0; at + align <= dataSize; at += align) decodeMsBlock(data + at, align, channels, msCoef, interleaved);
	} else {
		why = "unsupported WAV format " + std::to_string(format);
		return false;
	}
	const size_t frames = interleaved.size() / channels;
	if (frames == 0) {
		why = "no samples";
		return false;
	}
	out.rate = rate;
	if (keepStereo && channels >= 2) {
		out.channels = 2;
		out.samples.resize(frames * 2);
		for (size_t f = 0; f < frames; ++f) {
			out.samples[f * 2] = interleaved[f * channels];
			out.samples[f * 2 + 1] = interleaved[f * channels + 1];
		}
		return true;
	}
	out.samples.resize(frames);
	for (size_t f = 0; f < frames; ++f) {
		int sum = 0;
		for (u32 c = 0; c < channels; ++c) sum += interleaved[f * channels + c];
		out.samples[f] = i16(sum / int(channels));
	}
	return true;
}

void resampleSound(SrcSound& sound, u32 maxRate) {
	if (maxRate == 0 || sound.rate <= maxRate || sound.samples.empty()) return;
	const double ratio = double(sound.rate) / double(maxRate);
	const size_t n = size_t(double(sound.samples.size()) / ratio);
	std::vector<i16> out(std::max<size_t>(n, 1));
	for (size_t i = 0; i < out.size(); ++i) {
		// The mean over the stretch of the source this sample stands for.
		const double from = double(i) * ratio, to = std::min(from + ratio, double(sound.samples.size()));
		double sum = 0, weight = 0;
		for (size_t k = size_t(from); double(k) < to && k < sound.samples.size(); ++k) {
			const double w = std::min(double(k + 1), to) - std::max(double(k), from);
			sum += double(sound.samples[k]) * w;
			weight += w;
		}
		out[i] = clip16(int(std::lround(weight > 0 ? sum / weight : 0.0)));
	}
	sound.samples.swap(out);
	sound.rate = maxRate;
}

std::vector<u8> encodeSound(const SrcSound& sound, u32 loopStart, u32 loopEnd) {
	SoundHeader h {};
	h.magic = SOUND_MAGIC;
	h.version = SOUND_VERSION;
	h.rate = sound.rate;
	h.frames = u32(sound.samples.size());
	h.format = SOUND_PCM16;
	h.loopStart = loopStart;
	h.loopEnd = loopEnd;
	std::vector<u8> blob(sizeof(h) + sound.samples.size() * 2);
	std::memcpy(blob.data(), &h, sizeof(h));
	for (size_t i = 0; i < sound.samples.size(); ++i) {
		blob[sizeof(h) + i * 2] = u8(sound.samples[i] & 0xFF);
		blob[sizeof(h) + i * 2 + 1] = u8((sound.samples[i] >> 8) & 0xFF);
	}
	return blob;
}

std::vector<u8> makeWav(const std::vector<i16>& samples, u32 rate) {
	std::vector<u8> w(44 + samples.size() * 2);
	const auto put32 = [&](size_t at, u32 v) {
		for (int i = 0; i < 4; ++i) w[at + size_t(i)] = u8(v >> (8 * i));
	};
	const auto put16 = [&](size_t at, u16 v) {
		w[at] = u8(v);
		w[at + 1] = u8(v >> 8);
	};
	std::memcpy(w.data(), "RIFF", 4);
	put32(4, u32(36 + samples.size() * 2));
	std::memcpy(w.data() + 8, "WAVEfmt ", 8);
	put32(16, 16);
	put16(20, 1);
	put16(22, 1);
	put32(24, rate);
	put32(28, rate * 2);
	put16(32, 2);
	put16(34, 16);
	std::memcpy(w.data() + 36, "data", 4);
	put32(40, u32(samples.size() * 2));
	for (size_t i = 0; i < samples.size(); ++i) put16(44 + i * 2, u16(samples[i]));
	return w;
}

bool buildSyntheticSound(const std::string& file, std::vector<u8>& bytes) {
	std::string name;
	for (char c : file) name += char(std::toupper(static_cast<unsigned char>(c)));
	u32 rate;
	double freq, seconds;
	if (name == "STEP.WAV") {
		rate = 44100;
		freq = 600.0;
		seconds = 0.2;
	} else if (name == "STEP_STONE.WAV") {
		rate = 22050;
		freq = 300.0;
		seconds = 0.3;
	} else if (name == "HUM.WAV") {
		rate = 22050;
		freq = 110.0;
		seconds = 1.0;
	} else {
		return false;
	}
	std::vector<i16> samples(size_t(double(rate) * seconds));
	for (size_t i = 0; i < samples.size(); ++i) {
		const double t = double(i) / double(rate);
		const double fade = 1.0 - double(i) / double(samples.size());
		samples[i] = i16(std::lround(12000.0 * fade * std::sin(2.0 * 3.14159265358979 * freq * t)));
	}
	bytes = makeWav(samples, rate);
	return true;
}

SoundBakeResult bakeSounds(ScriptVm& vm, const std::function<bool(const std::string&, std::vector<u8>&)>& fetch, u32 maxRate,
                           PakWriter& pak) {
	SoundBakeResult result;
	if (maxRate == 0 || !fetch) return result;
	const u32 cls = vm.find("C_SFX");
	const u32 member = vm.findMember("C_SFX", "FILE");
	if (cls == SCRIPT_NONE || member == SCRIPT_NONE) return result;
	// The files the instances name, once each (lower case, as the archive names them).
	std::map<std::string, std::string> files; // stem -> file as the script wrote it
	for (u32 i = 0; i < vm.symbolCount(); ++i) {
		const ScriptSymbol& sym = vm.symbol(i);
		if (sym.type != SCRIPT_INSTANCE || vm.classOf(i) != cls) continue;
		++result.instances;
		ScriptInstance* inst = vm.create(i);
		if (!inst) continue;
		std::string file = vm.getString(member, 0, inst);
		vm.destroy(inst);
		if (file.empty()) continue;
		std::string stem = file.substr(0, file.find('.'));
		for (char& c : stem) c = char(std::tolower(static_cast<unsigned char>(c)));
		files.emplace(stem, file);
	}
	result.files = u32(files.size());
	for (const auto& [stem, file] : files) {
		std::vector<u8> bytes;
		if (!fetch(file, bytes)) {
			result.missing.push_back(file);
			continue;
		}
		SrcSound sound;
		std::string why;
		if (!decodeWav(bytes, sound, why)) {
			result.unusable.push_back(file + ": " + why);
			continue;
		}
		resampleSound(sound, maxRate);
		if (sound.samples.size() > SOUND_MAX_FRAMES) {
			result.unusable.push_back(file + ": too long for an effect (" + std::to_string(sound.samples.size() / sound.rate) + " s)");
			continue;
		}
		if (sound.rate < SOUND_MIN_RATE) {
			result.unusable.push_back(file + ": rate " + std::to_string(sound.rate));
			continue;
		}
		const std::vector<u8> blob = encodeSound(sound);
		pak.add("sfx/" + stem, ASSET_SOUND, blob, false);
		result.bytes += blob.size();
		++result.baked;
	}
	return result;
}

std::vector<u8> encodeMusic(const SrcSound& sound, u32 loopStart, u32 loopEnd) {
	MusicHeader h {};
	h.magic = MUSIC_MAGIC;
	h.version = MUSIC_VERSION;
	h.rate = sound.rate;
	h.channels = u16(sound.channels);
	h.frames = u32(sound.samples.size() / sound.channels);
	h.loopStart = loopStart;
	h.loopEnd = loopEnd;
	std::vector<u8> blob(sizeof(h) + sound.samples.size() * 2);
	std::memcpy(blob.data(), &h, sizeof(h));
	for (size_t i = 0; i < sound.samples.size(); ++i) {
		blob[sizeof(h) + i * 2] = u8(sound.samples[i] & 0xFF);
		blob[sizeof(h) + i * 2 + 1] = u8((sound.samples[i] >> 8) & 0xFF);
	}
	return blob;
}

MusicBakeResult bakeMusic(ScriptVm& vm, const std::string& dir, u32 maxRate, PakWriter& pak) {
	MusicBakeResult result;
	const u32 cls = vm.find("C_MUSICTHEME");
	const u32 member = vm.findMember("C_MUSICTHEME", "FILE");
	if (cls == SCRIPT_NONE || member == SCRIPT_NONE) return result;
	const auto lowerOf = [](std::string s) {
		for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
		return s;
	};
	// The WAV files of the folder by lower-case stem.
	std::map<std::string, std::filesystem::path> found;
	std::error_code ec;
	for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
		if (!e.is_regular_file(ec) || lowerOf(e.path().extension().string()) != ".wav") continue;
		found.emplace(lowerOf(e.path().stem().string()), e.path());
	}
	std::map<std::string, std::string> files; // stem -> file as the script wrote it
	for (u32 i = 0; i < vm.symbolCount(); ++i) {
		const ScriptSymbol& sym = vm.symbol(i);
		if (sym.type != SCRIPT_INSTANCE || vm.classOf(i) != cls) continue;
		++result.themes;
		ScriptInstance* inst = vm.create(i);
		if (!inst) continue;
		const std::string file = vm.getString(member, 0, inst);
		vm.destroy(inst);
		if (file.empty()) continue;
		files.emplace(lowerOf(file.substr(0, file.find('.'))), file);
	}
	result.files = u32(files.size());
	for (const auto& [stem, file] : files) {
		const auto it = found.find(stem);
		if (it == found.end()) {
			result.missing.push_back(file);
			continue;
		}
		std::vector<u8> bytes;
		{
			std::ifstream in(it->second, std::ios::binary);
			bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}
		SrcSound sound;
		std::string why;
		if (!decodeWav(bytes, sound, why, true)) {
			result.unusable.push_back(file + ": " + why);
			continue;
		}
		if (sound.channels == 2 && maxRate && sound.rate > maxRate) {
			SrcSound l, r;
			l.rate = r.rate = sound.rate;
			for (size_t f = 0; f + 1 < sound.samples.size(); f += 2) {
				l.samples.push_back(sound.samples[f]);
				r.samples.push_back(sound.samples[f + 1]);
			}
			resampleSound(l, maxRate);
			resampleSound(r, maxRate);
			sound.rate = l.rate;
			sound.samples.resize(std::min(l.samples.size(), r.samples.size()) * 2);
			for (size_t f = 0; f * 2 < sound.samples.size(); ++f) {
				sound.samples[f * 2] = l.samples[f];
				sound.samples[f * 2 + 1] = r.samples[f];
			}
		} else if (sound.channels == 1) {
			resampleSound(sound, maxRate);
		}
		if (sound.rate < SOUND_MIN_RATE || sound.rate > SOUND_MAX_RATE) {
			result.unusable.push_back(file + ": rate " + std::to_string(sound.rate));
			continue;
		}
		const std::vector<u8> blob = encodeMusic(sound);
		pak.add("music/" + stem, ASSET_SOUND, blob, false);
		result.bytes += blob.size();
		++result.baked;
	}
	return result;
}

std::vector<u8> bakeMusicZones(const std::vector<SrcMusicZone>& zones) {
	std::vector<u8> blob(sizeof(MusicZonesHeader));
	MusicZonesHeader h {};
	h.magic = MUSIC_ZONES_MAGIC;
	h.version = MUSIC_ZONES_VERSION;
	for (const SrcMusicZone& z : zones) {
		if (h.count >= 1024u) break;
		MusicZone m {};
		for (int k = 0; k < 3; ++k) {
			m.bbMin[k] = z.bbMin[k];
			m.bbMax[k] = z.bbMax[k];
		}
		m.priority = z.priority;
		m.ellipsoid = z.ellipsoid ? 1 : 0;
		m.isDefault = z.isDefault ? 1 : 0;
		m.volume = u8(std::clamp(int(std::lround(z.volume * 255.0f)), 0, 255));
		std::strncpy(m.name, z.name.c_str(), sizeof(m.name) - 1);
		const size_t at = blob.size();
		blob.resize(at + sizeof(m));
		std::memcpy(blob.data() + at, &m, sizeof(m));
		++h.count;
	}
	std::memcpy(blob.data(), &h, sizeof(h));
	return blob;
}

std::vector<u8> bakeAmbient(const std::vector<SrcAmbient>& sounds) {
	std::vector<u8> blob(sizeof(AmbientHeader));
	AmbientHeader h {};
	h.magic = AMBIENT_MAGIC;
	h.version = AMBIENT_VERSION;
	u32 count = 0;
	for (const SrcAmbient& s : sounds) {
		if (s.sound.empty() || count >= 20000u) continue;
		AmbientSound a {};
		for (int k = 0; k < 3; ++k) a.pos[k] = s.pos[k];
		a.radius = std::clamp(s.radius, 50.0f, 60000.0f);
		a.sound = hashName(s.sound.c_str());
		a.mode = u8(std::clamp(s.mode, 0, 2));
		a.volume = u8(std::lround(std::clamp(s.volume, 0.0f, 1.0f) * 255.0f));
		a.hourFrom = u8(std::clamp(int(std::floor(s.hourFrom)), 0, 24) % 24);
		a.hourTo = u8(std::clamp(int(std::ceil(s.hourTo)), 0, 24) % 24);
		a.delayMin = std::max(s.delayMin, 0.0f);
		a.delayMax = std::max(s.delayMax, a.delayMin);
		const u8* p = reinterpret_cast<const u8*>(&a);
		blob.insert(blob.end(), p, p + sizeof(a));
		++count;
	}
	h.count = count;
	std::memcpy(blob.data(), &h, sizeof(h));
	return blob;
}

} // namespace g3
