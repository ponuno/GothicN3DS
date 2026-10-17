// fix201: see video.h.
#include "video.h"

#include "g3ds/sound.h"
#include "g3ds/tex.h"
#include "lz4.h"

#include <stdlib.h>
#include <string.h>

namespace g3 {

bool VideoPlayer::init(const char* pakPath, AudioOut* out) {
	shutdown();
	_out = out;
	if (!pakPath || !_pak.open(pakPath)) return false;
	_ready = true;
	return true;
}

void VideoPlayer::shutdown() {
	stop();
	for (gfx::Texture*& t : _tex) {
		if (t) gfx::textureDestroy(t);
		t = nullptr;
	}
	free(_image);
	_image = nullptr;
	if (_ready) _pak.close();
	_ready = false;
}

// "C:\\...\\INTRO.BIK" -> "video/intro".
void VideoPlayer::entryName(const char* name, char* out, u32 size) {
	const char* base = name ? name : "";
	for (const char* c = base; *c; ++c)
		if (*c == '/' || *c == '\\') base = c + 1;
	u32 n = 0;
	const char* prefix = "video/";
	for (const char* c = prefix; *c && n + 1 < size; ++c) out[n++] = *c;
	for (const char* c = base; *c && *c != '.' && n + 1 < size; ++c) out[n++] = char(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
	out[n] = 0;
}

bool VideoPlayer::has(const char* name) const {
	if (!_ready) return false;
	char e[64];
	entryName(name, e, sizeof(e));
	return _pak.find(e) != nullptr;
}

bool VideoPlayer::start(const char* name) {
	stop();
	if (!_ready) return false;
	char e[64];
	entryName(name, e, sizeof(e));
	const PakEntry* entry = _pak.find(e);
	if (!entry || entry->size < sizeof(VideoHeader)) return false;
	_entry = *entry;
	if (!_pak.readRange(_entry, 0, &_h, sizeof(_h)) || _h.magic != VIDEO_MAGIC || _h.version != VIDEO_VERSION || _h.width == 0 ||
	    _h.height == 0 || _h.width % 8 || _h.height % 8 || _h.width > VIDEO_MAX_W || _h.height > VIDEO_MAX_H || _h.frameCount == 0 ||
	    _h.fpsMilli == 0 || _h.maxFrameRaw > (1u << 20) || _h.maxFramePacked > (1u << 21)) {
		++_stats.broken;
		return false;
	}
	const u32 chunks = _h.frameCount + _h.audioChunks;
	const u32 tableBytes = chunks * u32(sizeof(VideoChunk));
	if (u64(sizeof(VideoHeader)) + tableBytes > _entry.size) {
		++_stats.broken;
		return false;
	}
	const u32 blocks = videoBlocks(_h);
	_table = static_cast<VideoChunk*>(malloc(tableBytes));
	_packed = static_cast<u8*>(malloc(_h.maxFramePacked ? _h.maxFramePacked : 1u));
	_raw = static_cast<u8*>(malloc(_h.maxFrameRaw ? _h.maxFrameRaw : 1u));
	_blockAt = static_cast<u32*>(malloc(blocks * sizeof(u32)));
	if (!_image) _image = static_cast<u8*>(malloc(VIDEO_TEX_W * VIDEO_TEX_H / 2));
	if (!_table || !_packed || !_raw || !_blockAt || !_image || !_pak.readRange(_entry, sizeof(VideoHeader), _table, tableBytes)) {
		++_stats.broken;
		stop();
		return false;
	}
	memset(_image, 0, VIDEO_TEX_W * VIDEO_TEX_H / 2); // (black: an ETC1 block of zeros is a dark grey; covered by the first frame)
	const u32 bw = _h.width / 4u;
	for (u32 b = 0; b < blocks; ++b) _blockAt[b] = videoBlockOffset(b % bw, b / bw);
	if (!_tex[0]) {
		TexHeader th {};
		th.magic = TEX_MAGIC;
		th.width = VIDEO_TEX_W;
		th.height = VIDEO_TEX_H;
		th.format = TEX_ETC1;
		th.mipCount = 1;
		th.flags = TEXF_CLAMP;
		th.dataSize = VIDEO_TEX_W * VIDEO_TEX_H / 2;
		for (gfx::Texture*& t : _tex) t = gfx::textureCreate(th, _image);
		if (!_tex[0] || !_tex[1]) {
			++_stats.broken;
			stop();
			return false;
		}
	}
	_shown = -1;
	_next = 0;
	_time = 0;
	_active = true;
	++_stats.played;
	// The sound: the stream (the music has let it go).
	_audioNext = 0;
	_slotNext = 0;
	if (_h.audioRate && _h.audioChunks && _out && _out->streamOpen(_h.audioRate, 1)) {
		_streamOpen = true;
		for (u32 i = 0; i < u32(AUDIO_STREAM_SLOTS); ++i) {
			_slot[i] = _out->allocSamples(_h.audioChunkFrames * 2u);
			_queued[i] = false;
			if (!_slot[i]) {
				closeAudio();
				break;
			}
		}
		if (_streamOpen) feedAudio();
	}
	// The first picture at once.
	if (decodeFrame(0)) {
		_next = 1;
		_shown = 0;
		gfx::textureUpdateEtc1(_tex[0], _image);
	}
	return true;
}

void VideoPlayer::closeAudio() {
	if (!_out) return;
	if (_streamOpen) _out->streamClose();
	_streamOpen = false;
	for (u32 i = 0; i < u32(AUDIO_STREAM_SLOTS); ++i) {
		if (_slot[i]) _out->freeSamples(_slot[i]);
		_slot[i] = nullptr;
		_queued[i] = false;
	}
	free(_ima);
	_ima = nullptr;
	_imaCap = 0;
}

void VideoPlayer::stop() {
	closeAudio();
	free(_table);
	free(_packed);
	free(_raw);
	free(_blockAt);
	_table = nullptr;
	_packed = _raw = nullptr;
	_blockAt = nullptr;
	_active = false;
	_shown = -1;
}

// Puts picture `index`'s changed blocks into _image.
bool VideoPlayer::decodeFrame(u32 index) {
	if (index >= _h.frameCount) return false;
	const VideoChunk& c = _table[index];
	const u32 blocks = videoBlocks(_h), mapBytes = (blocks + 7u) / 8u;
	if (c.size > _h.maxFramePacked || c.raw > _h.maxFrameRaw || c.raw < mapBytes || u64(c.offset) + c.size > _entry.size ||
	    !_pak.readRange(_entry, c.offset, _packed, c.size) ||
	    LZ4_decompress_safe(reinterpret_cast<const char*>(_packed), reinterpret_cast<char*>(_raw), int(c.size), int(c.raw)) != int(c.raw)) {
		++_stats.broken;
		return false;
	}
	const u8* map = _raw;
	const u8* data = _raw + mapBytes;
	const u8* end = _raw + c.raw;
	for (u32 b = 0; b < blocks; ++b) {
		if (!(map[b >> 3] & (1u << (b & 7u)))) continue;
		if (data + 8 > end) {
			++_stats.broken;
			return false;
		}
		memcpy(_image + _blockAt[b], data, 8);
		data += 8;
	}
	return true;
}

// The sound: a free buffer of the stream gets the next second.
void VideoPlayer::feedAudio() {
	if (!_streamOpen) return;
	for (u32 k = 0; k < u32(AUDIO_STREAM_SLOTS); ++k) {
		const u32 s = _slotNext;
		if (_queued[s] && !_out->streamDone(s)) break;
		_queued[s] = false;
		if (_audioNext >= _h.audioChunks) break;
		const VideoChunk& c = _table[_h.frameCount + _audioNext];
		if (c.raw == 0 || c.raw > _h.audioChunkFrames || c.size != imaBytes(c.raw) || u64(c.offset) + c.size > _entry.size) {
			++_stats.broken;
			closeAudio();
			return;
		}
		if (c.size > _imaCap) {
			free(_ima);
			_ima = static_cast<u8*>(malloc(c.size));
			_imaCap = _ima ? c.size : 0;
		}
		if (!_ima || !_pak.readRange(_entry, c.offset, _ima, c.size)) {
			++_stats.broken;
			closeAudio();
			return;
		}
		imaDecode(_ima, c.raw, static_cast<i16*>(_slot[s]));
		_out->publishSamples(_slot[s], c.raw * 2u);
		if (!_out->streamQueue(s, static_cast<const i16*>(_slot[s]), c.raw)) break;
		_queued[s] = true;
		++_audioNext;
		++_stats.audioChunks;
		_slotNext = (s + 1) % AUDIO_STREAM_SLOTS;
	}
	_out->streamVolume(_volume);
}

void VideoPlayer::update(float dt) {
	if (!_active) return;
	_time += double(dt);
	feedAudio();
	// The picture due now; the ones before it are only put in.
	const u32 due = u32(_time * double(_h.fpsMilli) / 1000.0);
	bool changed = false;
	while (_next <= due && _next < _h.frameCount) {
		if (!decodeFrame(_next)) {
			stop();
			return;
		}
		if (_next < due) ++_stats.skipped;
		++_next;
		changed = true;
	}
	if (changed) {
		// (Two textures in turn: the one the GPU may still be drawing is not written.)
		const int t = _shown == 0 ? 1 : 0;
		gfx::textureUpdateEtc1(_tex[t], _image);
		_shown = t;
		++_stats.frames;
	}
	// The end: the last picture has had its time and the sound has been played.
	const bool soundOver = !_streamOpen || (_audioNext >= _h.audioChunks && [&] {
		for (u32 i = 0; i < u32(AUDIO_STREAM_SLOTS); ++i)
			if (_queued[i] && !_out->streamDone(i)) return false;
		return true;
	}());
	if (_next >= _h.frameCount && double(_time) * double(_h.fpsMilli) / 1000.0 >= double(_h.frameCount) && soundOver) stop();
}

} // namespace g3
