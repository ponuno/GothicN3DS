#include "weather_sfx.h"

#include <math.h>
#include <string.h>

namespace g3 {

namespace {

struct Rng {
	u32 s;
	u32 next() {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return s;
	}
	float unit() { return float(next() & 0xFFFFFFu) * (1.0f / 16777216.0f); }
	float signedUnit() { return unit() * 2.0f - 1.0f; }
};

// One-pole low pass: y += k (x - y), k from the corner frequency.
float lowK(float hz, float rate) { return 1.0f - expf(-2.0f * 3.14159265f * hz / rate); }

i16 toPcm(float v) {
	v = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
	return i16(lrintf(v * 32000.0f));
}

void normalise(float* buf, u32 n, float peak) {
	float m = 1e-6f;
	for (u32 i = 0; i < n; ++i) m = fabsf(buf[i]) > m ? fabsf(buf[i]) : m;
	const float k = peak / m;
	for (u32 i = 0; i < n; ++i) buf[i] *= k;
}

} // namespace

bool weatherNameIsSpell(const char* name) {
	if (!name) return true;
	char up[8] = {};
	for (int i = 0; i < 6 && name[i]; ++i) up[i] = char(name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i]);
	return strncmp(up, "MFX_", 4) == 0 || strncmp(up, "SPELL", 5) == 0 || strncmp(up, "SPL_", 4) == 0;
}

void makeRainSound(i16* out) {
	constexpr u32 N = RAIN_SFX_FRAMES;
	constexpr u32 FADE = RAIN_SFX_RATE / 4; // the last quarter second is laid over the first
	constexpr u32 TOTAL = N + FADE;
	const float rate = float(RAIN_SFX_RATE);
	static float buf[TOTAL];
	Rng rng {0x9E3779B9u};

	// The body: pink noise (Paul Kellet's filter), its lows taken out (rain has no rumble) and its top softened.
	float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
	float lowA = 0, lowB = 0, top = 0;
	const float kLowA = lowK(320.0f, rate), kLowB = lowK(140.0f, rate), kTop = lowK(5200.0f, rate);
	for (u32 i = 0; i < TOTAL; ++i) {
		const float w = rng.signedUnit();
		b0 = 0.99886f * b0 + w * 0.0555179f;
		b1 = 0.99332f * b1 + w * 0.0750759f;
		b2 = 0.96900f * b2 + w * 0.1538520f;
		b3 = 0.86650f * b3 + w * 0.3104856f;
		b4 = 0.55000f * b4 + w * 0.5329522f;
		b5 = -0.7616f * b5 - w * 0.0168980f;
		const float pink = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362f) * 0.11f;
		b6 = w * 0.115926f;
		lowA += kLowA * (pink - lowA);
		lowB += kLowB * (pink - lowB);
		const float band = pink - lowA * 0.85f - lowB * 0.15f;
		top += kTop * (band - top);
		buf[i] = top;
	}
	// The drops: short ticks of bright noise, many quiet ones and a few close loud ones.
	const u32 drops = TOTAL / 50; // about 320 a second heard on their own
	for (u32 d = 0; d < drops; ++d) {
		const u32 at = rng.next() % TOTAL;
		const float r = rng.unit();
		const float loud = r < 0.05f ? 0.55f + 0.45f * rng.unit() : 0.08f + 0.25f * rng.unit();
		const u32 len = u32(rate * (0.002f + 0.006f * rng.unit()));
		const float kd = lowK(1800.0f + 4500.0f * rng.unit(), rate);
		float y = 0;
		for (u32 k = 0; k < len && at + k < TOTAL; ++k) {
			y += kd * (rng.signedUnit() - y);
			const float env = (1.0f - float(k) / float(len));
			buf[at + k] += y * loud * env * env * 1.6f;
		}
	}
	// A slow swell: two cycles and three cycles in the loop, so it meets itself at the seam.
	for (u32 i = 0; i < TOTAL; ++i) {
		const float t = float(i) / float(N);
		buf[i] *= 0.86f + 0.09f * sinf(t * 2.0f * 3.14159265f * 2.0f) + 0.05f * sinf(t * 2.0f * 3.14159265f * 3.0f + 1.3f);
	}
	// The seam: the tail past N fades in over the start (equal power).
	for (u32 i = 0; i < FADE; ++i) {
		const float x = float(i) / float(FADE);
		const float in = sinf(x * 1.5707963f), outK = cosf(x * 1.5707963f);
		buf[i] = buf[i] * in + buf[N + i] * outK;
	}
	// As loud as a steady rain is (by its average, not its loudest drop), the few loudest drops rounded off.
	double sum = 0.0;
	for (u32 i = 0; i < N; ++i) sum += double(buf[i]) * buf[i];
	const float k = 0.22f / float(sqrt(sum / double(N)) + 1e-9);
	for (u32 i = 0; i < N; ++i) {
		const float x = buf[i] * k;
		out[i] = toPcm(x / sqrtf(1.0f + x * x));
	}
}

void makeThunderSound(i16* out, u32 seed) {
	constexpr u32 N = THUNDER_SFX_FRAMES;
	const float rate = float(THUNDER_SFX_RATE);
	static float buf[N];
	Rng rng {seed * 2654435761u + 0x1234567u};
	if (!rng.s) rng.s = 1;
	// Rolls: a few swells of rumble one after another, each later one fainter and lower.
	float swellAt[6], swellLen[6], swellAmp[6];
	float t0 = 0.05f + 0.25f * rng.unit();
	for (int k = 0; k < 6; ++k) {
		swellAt[k] = t0;
		swellLen[k] = 0.5f + 1.1f * rng.unit();
		swellAmp[k] = (k == 0 ? 1.0f : 0.35f + 0.55f * rng.unit()) * expf(-0.35f * float(k));
		t0 += 0.3f + 0.7f * rng.unit();
	}
	const float crack = 0.6f * rng.unit(); // how much of a sharp crack there is at the start (a near strike)
	float brownA = 0, brownB = 0, lp1 = 0, lp2 = 0, lp3 = 0, hp = 0, crackLp = 0;
	// (Small speakers give back little under 150 Hz: the rumble is kept up where they can say it.)
	const float k1 = lowK(650.0f, rate), k2 = lowK(220.0f, rate), kHp = lowK(70.0f, rate), k3 = lowK(500.0f, rate), kCrack = lowK(2600.0f, rate);
	for (u32 i = 0; i < N; ++i) {
		const float t = float(i) / rate;
		const float w = rng.signedUnit();
		// The rumble: white noise between about 70 and 650 Hz (lp1 - lp2 is a band), a little of the deepest left in.
		brownB = brownB * 0.996f + w * 0.05f;
		lp1 += k1 * (w - lp1);
		lp2 += k2 * (w - lp2);
		brownA += lowK(18.0f, rate) * (fabsf(w) - brownA); // (a slow flutter: the roll is uneven)
		float env = 0.0f;
		for (int k = 0; k < 6; ++k) {
			const float x = (t - swellAt[k]) / swellLen[k];
			if (x <= 0.0f || x >= 3.0f) continue;
			// A quick rise and a long fall.
			env += swellAmp[k] * (x < 0.12f ? x / 0.12f : expf(-(x - 0.12f) * 1.6f));
		}
		lp3 += k3 * ((lp1 - lp2) * 1.6f + lp2 * 0.9f + brownB * 0.25f - lp3); // (one pole more: no hiss in a rumble)
		float v = lp3 * env * (0.6f + 0.8f * brownA);
		if (t < 0.35f && crack > 0.0f) {
			crackLp += kCrack * (w - crackLp);
			const float ce = t < 0.004f ? t / 0.004f : expf(-(t - 0.004f) * 14.0f);
			v += crackLp * ce * crack * 1.4f;
		}
		hp += kHp * (v - hp);
		buf[i] = v - hp; // (no drift below hearing)
	}
	// The very end fades out whatever is left.
	const u32 tail = u32(rate * 0.4f);
	for (u32 i = 0; i < tail; ++i) buf[N - 1 - i] *= float(i) / float(tail);
	normalise(buf, N, 0.92f);
	for (u32 i = 0; i < N; ++i) out[i] = toPcm(buf[i]);
}

} // namespace g3
