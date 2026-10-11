#include "audio_3ds.h"

#include <string.h>

namespace g3 {

bool Audio3ds::open() {
	if (_open) return true;
	const Result rc = ndspInit();
	_openResult = unsigned(rc);
	if (R_FAILED(rc)) return false;
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	ndspSetMasterVol(1.0f);
	memset(_wave, 0, sizeof(_wave));
	for (u32 c = 0; c < CHANNELS; ++c) {
		ndspChnReset(int(c));
		ndspChnSetInterp(int(c), NDSP_INTERP_LINEAR);
		ndspChnSetFormat(int(c), NDSP_FORMAT_MONO_PCM16);
	}
	_open = true;
	return true;
}

void Audio3ds::close() {
	if (!_open) return;
	for (u32 c = 0; c < CHANNELS; ++c) ndspChnWaveBufClear(int(c));
	ndspExit();
	_open = false;
}

void* Audio3ds::allocSamples(u32 bytes) { return _open ? linearAlloc(bytes) : nullptr; }

void Audio3ds::freeSamples(void* block) {
	if (block) linearFree(block);
}

// The DSP reads memory the processor's cache may still hold the news of.
void Audio3ds::publishSamples(const void* block, u32 bytes) { DSP_FlushDataCache(block, bytes); }

void Audio3ds::start(u32 channel, const i16* samples, u32 frames, u32 rate, bool loop, float left, float right, float pitch) {
	if (!_open || channel >= CHANNELS) return;
	const int ch = int(channel);
	ndspChnWaveBufClear(ch);
	ndspChnReset(ch);
	ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
	ndspChnSetFormat(ch, NDSP_FORMAT_MONO_PCM16);
	_rate[channel] = rate;
	ndspChnSetRate(ch, float(rate) * pitch);
	float mix[12] = {};
	mix[0] = left;
	mix[1] = right;
	ndspChnSetMix(ch, mix);
	ndspWaveBuf& w = _wave[channel];
	memset(&w, 0, sizeof(w));
	w.data_vaddr = samples;
	w.nsamples = frames;
	w.looping = loop;
	w.status = NDSP_WBUF_FREE;
	ndspChnWaveBufAdd(ch, &w);
}

void Audio3ds::set(u32 channel, float left, float right, float pitch) {
	if (!_open || channel >= CHANNELS) return;
	float mix[12] = {};
	mix[0] = left;
	mix[1] = right;
	ndspChnSetMix(int(channel), mix);
	ndspChnSetRate(int(channel), float(_rate[channel]) * pitch);
}

void Audio3ds::stop(u32 channel) {
	if (!_open || channel >= CHANNELS) return;
	ndspChnWaveBufClear(int(channel));
}

bool Audio3ds::playing(u32 channel) {
	if (!_open || channel >= CHANNELS) return false;
	const u8 s = _wave[channel].status;
	return s == NDSP_WBUF_QUEUED || s == NDSP_WBUF_PLAYING;
}

bool Audio3ds::streamOpen(u32 rate, u32 channels) {
	if (!_open || channels < 1 || channels > 2) return false;
	streamClose();
	ndspChnReset(STREAM_CHANNEL);
	ndspChnSetInterp(STREAM_CHANNEL, NDSP_INTERP_LINEAR);
	ndspChnSetFormat(STREAM_CHANNEL, channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
	ndspChnSetRate(STREAM_CHANNEL, float(rate));
	float mix[12] = {};
	mix[0] = mix[1] = 1.0f;
	ndspChnSetMix(STREAM_CHANNEL, mix);
	memset(_streamWave, 0, sizeof(_streamWave));
	_streamChannels = channels;
	_streaming = true;
	return true;
}

void Audio3ds::streamClose() {
	if (!_open || !_streaming) return;
	ndspChnWaveBufClear(STREAM_CHANNEL);
	_streaming = false;
}

bool Audio3ds::streamQueue(u32 slot, const i16* samples, u32 frames) {
	if (!_open || !_streaming || slot >= AUDIO_STREAM_SLOTS) return false;
	ndspWaveBuf& w = _streamWave[slot];
	if (w.status == NDSP_WBUF_QUEUED || w.status == NDSP_WBUF_PLAYING) return false;
	memset(&w, 0, sizeof(w));
	w.data_vaddr = samples;
	w.nsamples = frames;
	w.looping = false;
	w.status = NDSP_WBUF_FREE;
	DSP_FlushDataCache(samples, frames * _streamChannels * 2u);
	ndspChnWaveBufAdd(STREAM_CHANNEL, &w);
	return true;
}

bool Audio3ds::streamDone(u32 slot) {
	if (!_open || slot >= AUDIO_STREAM_SLOTS) return true;
	const u8 s = _streamWave[slot].status;
	return s != NDSP_WBUF_QUEUED && s != NDSP_WBUF_PLAYING;
}

void Audio3ds::streamVolume(float volume) {
	if (!_open || !_streaming) return;
	float mix[12] = {};
	mix[0] = mix[1] = volume;
	ndspChnSetMix(STREAM_CHANNEL, mix);
}

} // namespace g3
