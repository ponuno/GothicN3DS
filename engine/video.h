// fix201: the game's videos (PlayVideo("INTRO.BIK")) from "video.pak" (g3ds/video.h): the picture into an ETC1 texture
// of 512 x 256 as it goes, the sound through the platform's stream (the music's, which stops meanwhile).
//
// The pictures are read from the card one at a time as their time comes (a few kilobytes each) and a frame that is late
// is passed over (only its changed blocks are put in, not drawn), so the picture keeps up with the sound. The sound is
// read a second at a time into the stream's buffers.
#pragma once
#include "audio.h"
#include "gfx.h"
#include "g3ds/pak.h"
#include "g3ds/video.h"

namespace g3 {

class VideoPlayer {
public:
	VideoPlayer() = default;
	~VideoPlayer() { shutdown(); }
	VideoPlayer(const VideoPlayer&) = delete;
	VideoPlayer& operator=(const VideoPlayer&) = delete;

	// `pakPath`: video.pak. False if it is not there (then nothing plays).
	bool init(const char* pakPath, AudioOut* out);
	void shutdown();
	bool ready() const { return _ready; }
	void setVolume(float v) { _volume = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

	// "INTRO.BIK" (any letter case, with or without the folder and extension): whether video.pak has it.
	bool has(const char* name) const;
	// Starts it (whatever played stops). False if there is no such video or it is damaged.
	bool start(const char* name);
	void stop();
	bool active() const { return _active; }
	// Every frame while active; it ends by itself (active() false) after its last picture and sound.
	void update(float dt);

	// What to draw: the texture (VIDEO_TEX_W x VIDEO_TEX_H) with the picture at its top left, width() x height().
	gfx::Texture* texture() const { return _shown >= 0 ? _tex[_shown] : nullptr; }
	u32 width() const { return _h.width; }
	u32 height() const { return _h.height; }
	float seconds() const { return float(_time); }
	float length() const { return _h.fpsMilli ? float(_h.frameCount) * 1000.0f / float(_h.fpsMilli) : 0.0f; }

	struct Stats {
		u32 played = 0;     // videos started
		u32 frames = 0;     // pictures shown
		u32 skipped = 0;    // pictures put in late, not shown
		u32 broken = 0;     // pictures or sound that could not be read
		u32 audioChunks = 0;
	};
	const Stats& stats() const { return _stats; }

private:
	static void entryName(const char* name, char* out, u32 size);
	bool decodeFrame(u32 index);
	void feedAudio();
	void closeAudio();

	AudioOut* _out = nullptr;
	bool _ready = false;
	Pak _pak;
	float _volume = 0.8f;
	Stats _stats;

	bool _active = false;
	VideoHeader _h {};
	PakEntry _entry {};
	VideoChunk* _table = nullptr; // frames, then the sound's chunks
	u8* _packed = nullptr;
	u8* _raw = nullptr;
	u8* _image = nullptr;          // the whole texture's ETC1 data (PICA order)
	u32* _blockAt = nullptr;       // per block of the picture: its byte in _image
	gfx::Texture* _tex[2] = {};
	int _shown = -1;
	u32 _next = 0;                 // the next picture to put in
	double _time = 0;

	// The sound.
	bool _streamOpen = false;
	void* _slot[AUDIO_STREAM_SLOTS] = {};
	bool _queued[AUDIO_STREAM_SLOTS] = {};
	u32 _slotNext = 0;
	u32 _audioNext = 0;            // the next chunk to read
	u8* _ima = nullptr;
	u32 _imaCap = 0;
};

} // namespace g3
