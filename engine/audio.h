// What the sound engine (sound.h) needs from the machine: a few channels that
// each play one buffer of 16-bit mono samples at a volume for the left and the
// right ear and a pitch. The console's is platform/3ds/audio_3ds.cpp (the DSP);
// the PC has no speakers here, and platform/host/audio_host.cpp only records
// what would have sounded (for the tests and the reports).
#pragma once
#include "g3ds/base.h"

namespace g3 {

constexpr u32 AUDIO_MAX_CHANNELS = 16;
constexpr u32 AUDIO_STREAM_SLOTS = 4;

class AudioOut {
public:
	virtual ~AudioOut() {}

	// Channels the sound engine may use (at most AUDIO_MAX_CHANNELS).
	virtual u32 channels() const = 0;

	// Memory samples must live in to be played (the console's DSP reads
	// linear memory). Null if there is none to spare. May be called from
	// the loading thread and from the main one; the sound engine does not
	// call them at the same time.
	virtual void* allocSamples(u32 bytes) = 0;
	virtual void freeSamples(void* block) = 0;
	// The processor has written `bytes` of a block: make them visible to whatever plays them.
	virtual void publishSamples(const void* block, u32 bytes) = 0;

	// Starts `samples` on a channel (whatever it played stops). `rate` in Hz,
	// `pitch` a factor (1: as recorded), `left` and `right` 0..1. A looping
	// buffer goes on until stop(); the samples stay valid until the channel
	// is stopped or has finished.
	virtual void start(u32 channel, const i16* samples, u32 frames, u32 rate, bool loop, float left, float right,
	                   float pitch) = 0;
	// Volume and pitch of a channel that is playing.
	virtual void set(u32 channel, float left, float right, float pitch) = 0;
	virtual void stop(u32 channel) = 0;
	// The channel is still sounding (a one-shot that has run out is not).
	virtual bool playing(u32 channel) = 0;
	// Once a frame, from the thread that drives the channels.
	virtual void update() {}

	// ---- the music stream: one more channel, fed buffer after buffer ----
	// Opens it for samples at `rate` with 1 or 2 channels (interleaved); the
	// sound begins when the first buffer is queued. False if there is none.
	virtual bool streamOpen(u32 rate, u32 channels) {
		(void)rate;
		(void)channels;
		return false;
	}
	virtual void streamClose() {}
	// Queues `frames` frames of one of AUDIO_STREAM_SLOTS buffers; they play in the order queued. The samples
	// (memory from allocSamples) must stay as they are until streamDone(slot) is true.
	virtual bool streamQueue(u32 slot, const i16* samples, u32 frames) {
		(void)slot;
		(void)samples;
		(void)frames;
		return false;
	}
	// The slot has been played to its end (or never queued): it may be refilled.
	virtual bool streamDone(u32 slot) {
		(void)slot;
		return true;
	}
	virtual void streamVolume(float volume) { (void)volume; }
};

} // namespace g3
