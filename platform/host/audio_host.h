// The PC has no sound output here: this stands in for it. It keeps the samples
// in ordinary memory and notes what was started, with a clock of its own that
// the caller moves (advance), so that one-shots end when their time is up.
// For the tests and for the reports of the host program.
#pragma once
#include "audio.h"

namespace g3 {

class HostAudio : public AudioOut {
public:
	struct Started {
		u32 channel = 0, frames = 0, rate = 0;
		bool loop = false;
		float left = 0, right = 0, pitch = 1;
		const i16* samples = nullptr;
	};

	u32 channels() const override { return 16; }
	void* allocSamples(u32 bytes) override;
	void freeSamples(void* block) override;
	void publishSamples(const void*, u32) override {}
	void start(u32 channel, const i16* samples, u32 frames, u32 rate, bool loop, float left, float right, float pitch) override;
	void set(u32 channel, float left, float right, float pitch) override;
	void stop(u32 channel) override;
	bool playing(u32 channel) override;
	bool streamOpen(u32 rate, u32 channels) override;
	void streamClose() override;
	bool streamQueue(u32 slot, const i16* samples, u32 frames) override;
	bool streamDone(u32 slot) override;
	void streamVolume(float volume) override { _streamVolume = volume; }

	// Moves the clock on; one-shots that have run out stop sounding.
	void advance(float seconds) { _clock += double(seconds); }
	// What has been started so far (the last LOG), and the channel states.
	u32 startedCount() const { return _started; }
	const Started& last() const { return _last; }
	u32 liveBlocks() const { return _blocks; }
	float leftOf(u32 channel) const { return channel < AUDIO_MAX_CHANNELS ? _ch[channel].left : 0.0f; }
	// The stream: how many buffers were queued, whether it is open, and its state.
	bool streamIsOpen() const { return _streamOpen; }
	u32 streamQueued() const { return _streamQueued; }
	u32 streamRate() const { return _streamRate; }
	u32 streamChannels() const { return _streamChannels; }
	float streamLevel() const { return _streamVolume; }
	u32 streamFirstSample() const { return _streamFirst; }
	float rightOf(u32 channel) const { return channel < AUDIO_MAX_CHANNELS ? _ch[channel].right : 0.0f; }

private:
	struct Channel {
		bool on = false, loop = false;
		double end = 0;
		float left = 0, right = 0, pitch = 1;
	};
	Channel _ch[AUDIO_MAX_CHANNELS];
	bool _streamOpen = false;
	u32 _streamRate = 0, _streamChannels = 0, _streamQueued = 0, _streamFirst = 0;
	float _streamVolume = 1.0f;
	double _slotEnd[AUDIO_STREAM_SLOTS] = {};
	bool _slotBusy[AUDIO_STREAM_SLOTS] = {};
	double _streamTail = 0; // when everything queued so far has been played
	double _clock = 0;
	u32 _started = 0;
	u32 _blocks = 0;
	Started _last;
};

} // namespace g3
