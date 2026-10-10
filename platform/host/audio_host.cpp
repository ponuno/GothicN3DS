#include "audio_host.h"

#include <stdlib.h>

namespace g3 {

void* HostAudio::allocSamples(u32 bytes) {
	void* p = malloc(bytes);
	if (p) ++_blocks;
	return p;
}

void HostAudio::freeSamples(void* block) {
	if (!block) return;
	free(block);
	--_blocks;
}

void HostAudio::start(u32 channel, const i16* samples, u32 frames, u32 rate, bool loop, float left, float right, float pitch) {
	if (channel >= AUDIO_MAX_CHANNELS) return;
	Channel& c = _ch[channel];
	c.on = true;
	c.loop = loop;
	c.left = left;
	c.right = right;
	c.pitch = pitch;
	c.end = _clock + double(frames) / (double(rate) * double(pitch > 0.01f ? pitch : 1.0f));
	++_started;
	_last = {channel, frames, rate, loop, left, right, pitch, samples};
}

void HostAudio::set(u32 channel, float left, float right, float pitch) {
	if (channel >= AUDIO_MAX_CHANNELS) return;
	_ch[channel].left = left;
	_ch[channel].right = right;
	_ch[channel].pitch = pitch;
}

void HostAudio::stop(u32 channel) {
	if (channel < AUDIO_MAX_CHANNELS) _ch[channel].on = false;
}

bool HostAudio::playing(u32 channel) {
	if (channel >= AUDIO_MAX_CHANNELS) return false;
	Channel& c = _ch[channel];
	if (c.on && !c.loop && _clock >= c.end) c.on = false;
	return c.on;
}

bool HostAudio::streamOpen(u32 rate, u32 channels) {
	_streamOpen = true;
	_streamRate = rate;
	_streamChannels = channels;
	_streamTail = _clock;
	for (u32 i = 0; i < AUDIO_STREAM_SLOTS; ++i) _slotBusy[i] = false;
	return true;
}

void HostAudio::streamClose() {
	_streamOpen = false;
	for (u32 i = 0; i < AUDIO_STREAM_SLOTS; ++i) _slotBusy[i] = false;
}

bool HostAudio::streamQueue(u32 slot, const i16* samples, u32 frames) {
	if (!_streamOpen || slot >= AUDIO_STREAM_SLOTS || _slotBusy[slot]) return false;
	if (_streamTail < _clock) _streamTail = _clock;
	_streamTail += double(frames) / double(_streamRate);
	_slotEnd[slot] = _streamTail;
	_slotBusy[slot] = true;
	++_streamQueued;
	_streamFirst = frames ? u32(u16(samples[0])) : 0;
	return true;
}

bool HostAudio::streamDone(u32 slot) {
	if (slot >= AUDIO_STREAM_SLOTS) return true;
	if (_slotBusy[slot] && _clock >= _slotEnd[slot]) _slotBusy[slot] = false;
	return !_slotBusy[slot];
}

} // namespace g3
