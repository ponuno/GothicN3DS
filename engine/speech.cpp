#include "speech.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace g3 {

bool Speech::init(const char* speechPakPath, AudioOut* out) {
	shutdown();
	if (!out || out->channels() <= u32(CHANNEL) || !speechPakPath) return false;
	// "…/speech.pak", then "…/speech2.pak" ...
	char path[320];
	for (u32 k = 0; k < MAX_PAKS; ++k) {
		if (k == 0) {
			snprintf(path, sizeof(path), "%s", speechPakPath);
		} else {
			snprintf(path, sizeof(path), "%s", speechPakPath);
			char* dot = strrchr(path, '.');
			if (!dot) break;
			snprintf(dot, sizeof(path) - size_t(dot - path), "%u.pak", unsigned(k + 1));
		}
		if (_paks[_pakCount].open(path)) ++_pakCount;
		else if (k == 0) break;
	}
	if (!_pakCount) {
		FILE* f = fopen(speechPakPath, "rb");
		_unusable = f != nullptr; // (there, but not to be read: damaged, or too big for the console's seek)
		if (f) fclose(f);
		return false;
	}
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
	for (Pak& p : _paks) p.close();
	_pakCount = 0;
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
	Pak* pak = nullptr;
	const PakEntry* e = nullptr;
	for (u32 k = 0; k < _pakCount && !e; ++k)
		if ((e = _paks[k].find(speechEntryHash(lineHash))) != nullptr) pak = &_paks[k];
	if (!e || e->size <= sizeof(SoundHeader) || e->codec != PAK_RAW) {
		++_missing;
		return 0.0f;
	}
	u8* blob = static_cast<u8*>(pak->readAlloc(*e));
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
		if (_unusable)
			fprintf(f, "voices: off (speech.pak is there but cannot be opened: damaged, or over 2 GiB, which the console cannot seek in - bake again, the baker splits it)\n");
		else
			fprintf(f, "voices: off (no speech.pak beside data.pak)\n");
		return;
	}
	u32 recordings = 0;
	for (u32 k = 0; k < _pakCount; ++k) recordings += _paks[k].count();
	fprintf(f, "voices: %u recordings in %u archive(s); played %u, lines without a recording %u, damaged %u\n", recordings, _pakCount,
	        _played, _missing, _broken);
}

} // namespace g3
