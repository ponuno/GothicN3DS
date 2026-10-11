#include "speech.h"

#include <stdlib.h>
#include <string.h>

namespace g3 {

bool Speech::init(const char* speechPakPath, AudioOut* out) {
	shutdown();
	if (!out || out->channels() <= u32(CHANNEL) || !speechPakPath) return false;
	if (!_pak.open(speechPakPath)) return false;
	_out = out;
	_ready = true;
	return true;
}

void Speech::shutdown() {
	if (_out && _ready) _out->stop(CHANNEL);
	for (u32 i = 0; i < 2; ++i) {
		if (_block[i] && _out) _out->freeSamples(_block[i]);
		_block[i] = nullptr;
		_blockBytes[i] = 0;
	}
	_pak.close();
	_ready = false;
	_out = nullptr;
}

void Speech::stop() {
	if (_ready) _out->stop(CHANNEL);
}

bool Speech::playing() const { return _ready && _out->playing(CHANNEL); }

float Speech::play(u32 lineHash, float gain) {
	if (!_ready) return 0.0f;
	_out->stop(CHANNEL);
	const PakEntry* e = _pak.find(speechEntryHash(lineHash));
	if (!e || e->size <= sizeof(SoundHeader) || e->codec != PAK_RAW) {
		++_missing;
		return 0.0f;
	}
	u8* blob = static_cast<u8*>(_pak.readAlloc(*e));
	if (!blob) {
		++_broken;
		return 0.0f;
	}
	SoundHeader h;
	memcpy(&h, blob, sizeof(h));
	if (!speechValid(h, e->size)) {
		free(blob);
		++_broken;
		return 0.0f;
	}
	const float seconds = float(h.frames) / float(h.rate);
	const float level = clamp(gain, 0.0f, 1.0f) * _volume;
	// The block used before the last one is surely silent by now.
	_cur ^= 1u;
	if (_block[_cur]) {
		_out->freeSamples(_block[_cur]);
		_block[_cur] = nullptr;
	}
	if (level < 0.005f) { // muted: only its length matters
		free(blob);
		return seconds;
	}
	const u32 bytes = h.frames * 2u;
	i16* pcm = static_cast<i16*>(_out->allocSamples(bytes));
	if (!pcm) {
		free(blob);
		++_broken;
		return seconds;
	}
	if (h.format == SOUND_IMA4)
		imaDecode(blob + sizeof(SoundHeader), h.frames, pcm);
	else
		memcpy(pcm, blob + sizeof(SoundHeader), bytes);
	free(blob);
	_out->publishSamples(pcm, bytes);
	_block[_cur] = pcm;
	_blockBytes[_cur] = bytes;
	_out->start(CHANNEL, pcm, h.frames, h.rate, false, level, level, 1.0f);
	++_played;
	return seconds;
}

void Speech::writeReport(FILE* f) const {
	if (!_ready) {
		fprintf(f, "voices: off (no speech.pak beside data.pak)\n");
		return;
	}
	fprintf(f, "voices: %u recordings in speech.pak; played %u, lines without a recording %u, damaged %u\n", _pak.count(), _played,
	        _missing, _broken);
}

} // namespace g3
