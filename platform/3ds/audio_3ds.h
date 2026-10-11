// The console's sound output: the DSP (libctru's ndsp) mixes up to 24
// channels; we use the first 16, one per voice of the sound engine.
// Needs the DSP firmware dumped to sdmc:/3ds/dspfirm.cdc (DSP1 does it); when
// it is not there open() fails and the game is silent.
#pragma once
#include "audio.h"

#include <3ds.h>

namespace g3 {

class Audio3ds : public AudioOut {
public:
	bool open();
	void close();
	bool isOpen() const { return _open; }
	// What ndspInit answered (0: fine): 0xD880A7FA is "no DSP firmware at sdmc:/3ds/dspfirm.cdc".
	unsigned openResult() const { return _openResult; }

	u32 channels() const override { return _open ? CHANNELS : 0; }
	void* allocSamples(u32 bytes) override;
	void freeSamples(void* block) override;
	void publishSamples(const void* block, u32 bytes) override;
	void start(u32 channel, const i16* samples, u32 frames, u32 rate, bool loop, float left, float right, float pitch) override;
	void set(u32 channel, float left, float right, float pitch) override;
	void stop(u32 channel) override;
	bool playing(u32 channel) override;
	bool streamOpen(u32 rate, u32 channels) override;
	void streamClose() override;
	bool streamQueue(u32 slot, const i16* samples, u32 frames) override;
	bool streamDone(u32 slot) override;
	void streamVolume(float volume) override;

	static constexpr u32 CHANNELS = 16;
	static constexpr int STREAM_CHANNEL = 20; // (the DSP has 24; the music has one of its own)

private:
	unsigned _openResult = 0xFFFFFFFFu;
	bool _open = false;
	ndspWaveBuf _wave[CHANNELS];
	u32 _rate[CHANNELS] = {};
	ndspWaveBuf _streamWave[AUDIO_STREAM_SLOTS];
	bool _streaming = false;
	u32 _streamChannels = 2;
};

} // namespace g3
