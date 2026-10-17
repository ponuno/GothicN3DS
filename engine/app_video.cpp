// fix201: the game's videos (video.h): asked for by the scripts (PlayVideo), played over everything while the game
// stands still, skipped with A, B or START.
#include "app.h"

#include <stdio.h>
#include <string.h>

namespace g3 {

// video.pak beside data.pak, if the player has made one.
void App::startVideo() {
	char path[320];
	snprintf(path, sizeof(path), "%s", _pak.path());
	char* slash = strrchr(path, '/');
	char* bslash = strrchr(path, '\\');
	if (bslash > slash) slash = bslash;
	snprintf(slash ? slash + 1 : path, sizeof(path) - (slash ? u32(slash + 1 - path) : 0), "video.pak");
	if (_video.init(path, _audio)) _video.setVolume(float(_settings.musicVolume > _settings.voiceVolume ? _settings.musicVolume : _settings.voiceVolume) / 10.0f);
}

bool App::playVideo(const char* name) {
	if (!_video.ready() || !_video.has(name)) return false;
	// The world falls silent; the music lets the stream go.
	_music.suspend(true);
	_sound.stopAll();
	_speech.stop();
	_chatter.stop();
	if (!_video.start(name)) {
		_music.suspend(false);
		return false;
	}
	_videoHold = 0.5f;
	return true;
}

bool App::videoUpdate(const Input& in, float dt) {
	// (Under the start menu the world's start-up has run already: its intro waits for "New game", as the original's.
	// A save loaded instead forgets it: Game::restore.)
	if (_startActive && !_video.active()) return false;
	if (!_video.active()) {
		// What the scripts asked for (one after the other: Extro, PlayerOut, Credits).
		if (_npcs.loaded()) {
			char name[48];
			while (_npcs.game().takeVideo(name, sizeof(name)))
				if (playVideo(name)) break;
		}
		if (!_video.active()) {
			if (_music.suspended()) _music.suspend(false);
			return false;
		}
	}
	if (_videoHold > 0.0f) _videoHold -= dt;
	else if (in.confirm) _video.stop(); // (the console gives A, B and START as confirm here)
	_video.update(dt);
	if (!_video.active()) {
		// The next one at once, or the game again.
		if (_npcs.loaded()) {
			char name[48];
			while (_npcs.game().takeVideo(name, sizeof(name)))
				if (playVideo(name)) return true;
		}
		_music.suspend(false);
		return true; // (this frame still belongs to it: the world goes on with the next)
	}
	return true;
}

void App::renderVideo() {
	const gfx::FrameDesc fd = blankFrame();
	const int W = gfx::SCREEN_W, H = gfx::SCREEN_H;
	// fix214: depth for the films. The picture is a window into the screen: behind the glass in the middle (the
	// left eye's copy a little to the left, the right eye's to the right), at the glass at its edges, which a dark
	// frame holds at the screen's own depth. The slider sets how deep (0: flat, as before).
	const auto drawScreen = [&](int eye) {
		gfx::setPass(gfx::PASS_SKY);
		if (!_ui.ready()) return;
		_ui.rect(0, 0, W, H, 0x000000FFu);
		if (gfx::Texture* t = _video.texture()) {
			// As large as the screen takes it, its shape kept, in the middle.
			const int vw = int(_video.width()), vh = int(_video.height());
			const float s = (float(W) / float(vw) < float(H) / float(vh)) ? float(W) / float(vw) : float(H) / float(vh);
			const int dw = int(float(vw) * s), dh = int(float(vh) * s);
			const int x0 = (W - dw) / 2, y0 = (H - dh) / 2;
			const float depth = eye == 0 ? 0.0f : float(eye) * clamp(_stereo, 0.0f, 1.0f) * 6.0f; // pixels, at the middle
			if (depth == 0.0f || vw < 16) {
				_ui.picture(t, vw, vh, x0, y0, x0 + dw, y0 + dh, 0xFFFFFFFFu);
			} else {
				const int strips = 16;
				for (int k = 0; k < strips; ++k) {
					const int u0 = vw * k / strips, u1 = vw * (k + 1) / strips;
					const float mid = (float(k) + 0.5f) / float(strips) * 2.0f - 1.0f; // -1 .. 1 across
					const float shift = depth * (1.0f - mid * mid);
					const int sx0 = x0 + dw * k / strips + int(shift), sx1 = x0 + dw * (k + 1) / strips + int(shift) + 1;
					_ui.pictureRegion(t, u0, 0, u1, vh, sx0, y0, sx1, y0 + dh, 0xFFFFFFFFu);
				}
				// The frame at the glass: the picture's shifted edges are covered.
				const int m = int(fabsf(depth)) + 2;
				_ui.rect(0, 0, x0 + m, H, 0x000000FFu);
				_ui.rect(x0 + dw - m, 0, W, H, 0x000000FFu);
			}
		}
		_ui.flush();
	};
	gfx::frameBegin(fd);
	drawScreen(gfx::stereoActive() ? -1 : 0);
	if (gfx::stereoActive()) {
		gfx::eyeBegin(fd);
		drawScreen(1);
	}
	gfx::frameEnd();
}

} // namespace g3
