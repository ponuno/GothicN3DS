// The top screen's refresh rate from its timing registers (PDC0, 0x1EF00400; see 3dbrew "GPU/External Registers"):
// the pixel clock is fixed, a refresh is (HTotal + 1) * (VTotal + 1) pixel clocks. A longer VTotal lowers the rate; a
// higher one comes from a shorter HTotal, taken out of the blanking before the visible part (everything horizontal
// shifts with it, so the visible part and the order of the edges stay as they were). The maths only: reading and
// writing the registers is platform/3ds/display_rate.cpp.
#pragma once

#include "g3ds/base.h"

namespace g3 {

// The registers 0x00 .. 0x64 of PDC0 as 32-bit words (word k = offset 4k); the ones that matter:
//   0 HTotal  1 HStart  2 HBR  3 HPF  4 HSync  5 HPB  6 HBL  7 H interrupt (two u16)  9 VTotal
//   23 image size  24 HDisp (start | end << 16)  25 framebuffer height
struct PdcTiming {
	enum { WORDS = 26 };
	u32 reg[WORDS] = {};
};

constexpr float PDC_PIXEL_CLOCK = 11.17e6f; // Hz (about the ARM11's minimum clock / 24); from the thread on gbatemp, and the 59.8 Hz it gives at the defaults

inline float pdcRefresh(const PdcTiming& t) {
	return PDC_PIXEL_CLOCK / float((t.reg[0] & 0xFFFu) + 1u) / float((t.reg[9] & 0xFFFu) + 1u);
}

// True if the registers look like those of the top screen as the system sets them (otherwise nothing is touched).
inline bool pdcKnown(const PdcTiming& t) {
	const u32 ht = t.reg[0] & 0xFFFu, vt = t.reg[9] & 0xFFFu;
	const u32 start = t.reg[24] & 0xFFFFu, end = t.reg[24] >> 16;
	return ht >= 250 && ht <= 600 && vt >= 380 && vt <= 0xFFF && end > start && end - start == 240 && start < ht && end <= ht + 1;
}

// `base` with the refresh rate nearest `hz` (24 .. 90). Returns the rate it makes; 0 if `base` is not known.
inline float pdcTimingFor(const PdcTiming& base, float hz, PdcTiming& out) {
	out = base;
	if (!pdcKnown(base)) return 0.0f;
	if (hz < 24.0f) hz = 24.0f;
	if (hz > 90.0f) hz = 90.0f;
	const u32 ht = base.reg[0] & 0xFFFu, vt = base.reg[9] & 0xFFFu;
	if (hz <= pdcRefresh(base)) {
		// Longer frame: more blank lines at the end.
		u32 v = u32(PDC_PIXEL_CLOCK / hz / float(ht + 1u) + 0.5f);
		v = v < 1 ? 1 : v - 1;
		if (v > 0xFFFu) v = 0xFFFu;
		out.reg[9] = (base.reg[9] & ~0xFFFu) | v;
		return pdcRefresh(out);
	}
	// Shorter line: the blank in front of the visible part gets shorter (never to less than 8 pixel clocks).
	u32 h = u32(PDC_PIXEL_CLOCK / hz / float(vt + 1u) + 0.5f);
	h = h < 1 ? 1 : h - 1;
	const u32 start = base.reg[24] & 0xFFFFu;
	u32 cut = ht > h ? ht - h : 0u;
	if (cut > 160u) cut = 160u;
	if (start < 8u + cut) cut = start > 8u ? start - 8u : 0u;
	auto shift = [cut](u32 v) { return (v & ~0xFFFu) | (((v & 0xFFFu) > cut ? (v & 0xFFFu) - cut : 0u)); };
	out.reg[0] = shift(base.reg[0]);
	static const int kSingle[] = {1, 2, 3, 5, 6};
	for (int k : kSingle) out.reg[k] = shift(base.reg[k]);
	// (two u16 each)
	static const int kPairs[] = {7, 24};
	for (int k : kPairs) {
		const u32 lo = base.reg[k] & 0xFFFFu, hi = base.reg[k] >> 16;
		out.reg[k] = ((lo > cut ? lo - cut : 0u)) | ((hi > cut ? hi - cut : 0u) << 16);
	}
	return pdcRefresh(out);
}

} // namespace g3
