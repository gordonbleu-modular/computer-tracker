// Fixed-point DSP helpers and lookup tables shared by the voices and mixer.
// Host-testable: no Pico SDK dependencies. All per-sample maths is 32-bit
// integer (the RP2040's Cortex-M0+ has a single-cycle 32x32->32 multiply but
// no 64-bit multiply and no FPU), tables are built once at start-up.
// SPDX-License-Identifier: MIT
#pragma once

#include <cmath>
#include <cstdint>

namespace trk {

constexpr int kSampleRate = 48000;
constexpr int kControlRate = 8;          // envelopes/coefficients every 8 samples
constexpr int kSineBits = 11;            // 2048-entry sine table
constexpr int kSineSize = 1 << kSineBits;

inline int32_t clamp32(int32_t x, int32_t lo, int32_t hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
// Symmetric saturation with one compare in the common (in-range) case.
inline int32_t sat(int32_t x, int32_t lim) {
    // (0.4.0) Marked unlikely, so the in-range case falls through without a
    // taken branch (1 cycle instead of 2 on the M0+, ~40-60 sat()s per sample).
    if (__builtin_expect(static_cast<uint32_t>(x + lim) > static_cast<uint32_t>(2 * lim), 0)) x = x < 0 ? -lim : lim;
    return x;
}
inline int32_t clamp16(int32_t x) { return sat(x, 32767); }
inline int clampi(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }

// Cubic soft clipper, Q15 in and out. Input is limited to +-1.5 (49152);
// the curve x - 4/27 x^3 reaches exactly 1.0 there with zero slope.
inline int32_t softclip(int32_t x) {
    x = sat(x, 49152);
    const int32_t xh = x >> 1;                  // Q14
    const int32_t x2 = (xh * xh) >> 14;         // Q14
    const int32_t x3 = (x2 * xh) >> 14;         // Q14
    return x - ((x3 * 9709) >> 15);
}

struct Rng {
    uint32_t s = 0x9E3779B9u;
    uint32_t next() {
        uint32_t x = s;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        s = x;
        return x;
    }
    int32_t noise() { return static_cast<int32_t>(next()) >> 16; } // +-32768
};

struct Tables {
    int16_t sine[kSineSize];
    uint32_t note_inc[128];     // phase increment per sample (2^32 = one cycle)
    uint16_t svf_f[128];        // Chamberlin f = 2 sin(pi fc / fs), Q15, capped
    uint32_t ks_period[128];    // Karplus-Strong delay, samples in Q8
    uint16_t decay_coef[128];   // per-control-tick multiplier, Q16, -60 dB in T
    uint32_t decay_samples[128];
    int32_t modal_k[128];       // 13.8 * 2^29 / T  (see Modal voice)
    uint16_t ks_loss[128];      // KS loop gain reduction (Q15) at a 200-sample period
    int16_t pan_l[128], pan_r[128];
    uint16_t level[128];        // fader law, Q12 (127 -> 4096 = 0 dB)
    uint16_t rv_rate[128];      // reverb decay: log2 gain per 16 kHz reverb tick, Q24 (0.4.0)
    uint16_t exp2n[65];         // 2^(-i/64), Q15
    uint16_t send_law[128];     // reverb send law (s/127)^2, Q15 (0.4.1: shared by the mixer knob and the W effect)
    bool ready = false;
};

inline Tables g_tables;

inline float note_hz(int n) { return 440.0f * std::pow(2.0f, (static_cast<float>(n) - 69.0f) / 12.0f); }

// Decay param 0..127 -> -60 dB time, 5 ms .. 4 s (exponential).
inline float decay_seconds(int p) {
    return 0.005f * std::pow(800.0f, static_cast<float>(p) / 127.0f);
}

inline void init_tables() {
    Tables& t = g_tables;
    if (t.ready) return;
    const float two_pi = 6.28318530718f;
    for (int i = 0; i < kSineSize; ++i) {
        t.sine[i] = static_cast<int16_t>(std::lround(32767.0f * std::sin(two_pi * static_cast<float>(i) / kSineSize)));
    }
    for (int n = 0; n < 128; ++n) {
        const float hz = note_hz(n);
        t.note_inc[n] = static_cast<uint32_t>(static_cast<double>(hz) * 4294967296.0 / kSampleRate);
        float fc = hz;
        if (fc > 7000.0f) fc = 7000.0f;
        t.svf_f[n] = static_cast<uint16_t>(std::lround(32768.0f * 2.0f * std::sin(3.14159265f * fc / kSampleRate)));
        // The 2-tap averaging loop filter adds ~0.5 sample of delay.
        float period = static_cast<float>(kSampleRate) / hz - 0.5f;
        if (period > 1020.0f) period = 1020.0f;
        if (period < 2.0f) period = 2.0f;
        t.ks_period[n] = static_cast<uint32_t>(period * 256.0f);
    }
    for (int p = 0; p < 128; ++p) {
        const float secs = decay_seconds(p);
        const float samples = secs * kSampleRate;
        t.decay_samples[p] = static_cast<uint32_t>(samples);
        const float per_tick = std::exp(-6.9078f * kControlRate / samples);
        t.decay_coef[p] = static_cast<uint16_t>(std::lround(per_tick * 65535.0f));
        t.modal_k[p] = static_cast<int32_t>(13.8f * 536870912.0f / samples);
        // Loop gain so a 200-sample period string reaches -60 dB in T.
        const float g = std::exp(-6.9078f * 200.0f / samples);
        int32_t loss = static_cast<int32_t>(std::lround((1.0f - g) * 32768.0f));
        if (loss < 2) loss = 2;
        if (loss > 30000) loss = 30000;
        t.ks_loss[p] = static_cast<uint16_t>(loss);
        // Constant-power pan.
        const float a = (static_cast<float>(p) / 127.0f) * 1.5707963f;
        t.pan_l[p] = static_cast<int16_t>(std::lround(32767.0f * std::cos(a)));
        t.pan_r[p] = static_cast<int16_t>(std::lround(32767.0f * std::sin(a)));
        // Fader: 0 = silent, 127 = 0 dB, ~-40 dB at 20.
        float g_lin = 0.0f;
        if (p > 0) g_lin = std::pow(10.0f, (static_cast<float>(p) - 127.0f) * 0.375f / 20.0f);
        t.level[p] = static_cast<uint16_t>(std::lround(g_lin * 4096.0f));
    }
    t.pan_l[64] = t.pan_r[64] = 23170; // exact centre (-3 dB)
    for (int p = 0; p < 128; ++p) {
        // Reverb decay param 0..127 -> -60 dB time 0.3 .. 9 s (exponential).
        const float t60 = 0.3f * std::pow(30.0f, static_cast<float>(p) / 127.0f);
        t.rv_rate[p] = static_cast<uint16_t>(std::lround(9.9658f / (t60 * 16000.0f) * 16777216.0f));
    }
    for (int sd = 0; sd < 128; ++sd) t.send_law[sd] = static_cast<uint16_t>((sd * sd * 32767) / 16129);
    for (int i = 0; i <= 64; ++i) t.exp2n[i] = static_cast<uint16_t>(std::lround(32767.0f * std::pow(2.0f, -static_cast<float>(i) / 64.0f)));
    t.ready = true;
}

inline int32_t sine_lookup(uint32_t phase) {
    return g_tables.sine[phase >> (32 - kSineBits)];
}

// Exponential decay envelope stepped at the control rate. Value Q24.
struct Env {
    uint32_t v = 0;
    uint16_t coef = 0;
    void start(uint32_t level_q24, uint16_t c) { v = level_q24; coef = c; }
    void tick() { v = ((v >> 8) * coef) >> 8; }
    int32_t q15() const { return static_cast<int32_t>(v >> 9); }
};

} // namespace trk
