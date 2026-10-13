// The top screen's refresh rate (experimental): the timing registers of the screen's controller are written through
// GSPGPU. Off until asked for; nothing is written to anything that does not look like the top screen's registers, and
// whatever was there is put back when the game is left or suspended.
#pragma once

namespace g3 {
namespace display {

// Reads the registers. False if that fails or they are not what the top screen's are (then nothing else here does anything).
bool init();
// Sets the refresh rate (Hz, about 30..90); the rate the registers make is returned (0 if nothing was written).
float set(float hz);
// The system's own timing again. With `pausing` (HOME, sleep) the rate in force is remembered and resume() puts it back;
// without (the menu, 30 fps) nothing is remembered, so a later resume() leaves the system's timing alone.
void restore(bool pausing = false);
void resume();
float current();
// 0 ok, otherwise the failure (for the screen of numbers): 1 read, 2 not the top screen's registers, 3 write.
int failure();

} // namespace display
} // namespace g3
