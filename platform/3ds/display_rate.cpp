#include "display_rate.h"

#include "display_timing.h"

#include <3ds.h>

#include <math.h>

namespace g3 {
namespace display {

namespace {

// PDC0 (0x1EF00400) as GSPGPU:WriteHWRegs counts: from 0x1EB00000.
const u32 kRegBase = 0x00400400u;

PdcTiming gBase, gNow;
bool gReady = false, gModified = false;
int gFailure = 0;
float gHz = 0.0f;
float gWanted = 0.0f; // the rate to put back after a pause (0: none)

bool write(const PdcTiming& t, const PdcTiming& from) {
	// Only the words that differ, in runs (the registers must be written 32 bits at a time).
	u32 k = 0;
	while (k < PdcTiming::WORDS) {
		if (t.reg[k] == from.reg[k]) {
			++k;
			continue;
		}
		u32 end = k + 1;
		while (end < PdcTiming::WORDS && t.reg[end] != from.reg[end]) ++end;
		if (R_FAILED(GSPGPU_WriteHWRegs(kRegBase + k * 4u, &t.reg[k], u8((end - k) * 4u)))) return false;
		k = end;
	}
	return true;
}

} // namespace

bool init() {
	gReady = false;
	gModified = false;
	gHz = 0.0f;
	gFailure = 0;
	if (R_FAILED(GSPGPU_ReadHWRegs(kRegBase, gBase.reg, PdcTiming::WORDS * 4u))) {
		gFailure = 1;
		return false;
	}
	if (!pdcKnown(gBase)) {
		gFailure = 2;
		return false;
	}
	gNow = gBase;
	gHz = pdcRefresh(gBase);
	gReady = true;
	return true;
}

float set(float hz) {
	if (!gReady) return 0.0f;
	PdcTiming next;
	const float made = pdcTimingFor(gBase, hz, next);
	if (made <= 0.0f) return 0.0f;
	if (!write(next, gNow)) {
		gFailure = 3;
		gReady = false; // (whatever was written stays; restore() still tries to undo it)
		return 0.0f;
	}
	gNow = next;
	gModified = true;
	gHz = made;
	return made;
}

void restore(bool pausing) {
	if (!gModified) {
		if (!pausing) gWanted = 0.0f;
		return;
	}
	gWanted = pausing ? gHz : 0.0f;
	// (Even after a failed write: the base is what the system had.)
	GSPGPU_WriteHWRegs(kRegBase, gBase.reg, 0x20u); // words 0..7
	GSPGPU_WriteHWRegs(kRegBase + 9u * 4u, &gBase.reg[9], 4u);
	GSPGPU_WriteHWRegs(kRegBase + 24u * 4u, &gBase.reg[24], 4u);
	gNow = gBase;
	gModified = false;
	gHz = pdcRefresh(gBase); // (what the screen runs at now)
}

void resume() {
	const float hz = gWanted;
	gWanted = 0.0f;
	if (gReady && hz > 0.0f && fabsf(hz - pdcRefresh(gBase)) > 0.01f) set(hz);
}

float current() { return gHz; }
int failure() { return gFailure; }

} // namespace display
} // namespace g3
