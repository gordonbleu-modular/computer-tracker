// The four drum-synth voices. Each voice can run any of six engines.
// Per-sample work is integer only; envelopes and modulated coefficients
// update every kControlRate samples; trigger-time maths uses at most one
// 32-bit (hardware) divide and one 64-bit multiply.
// SPDX-License-Identifier: MIT
#pragma once

#include "dsp.h"
#include "song.h"

namespace trk {

constexpr int kKsSize = 1024;   // Karplus-Strong delay line (lowest note ~47 Hz)

struct TriggerArgs {
    uint8_t engine;
    uint8_t note;    // MIDI 0..127 incl. tune
    uint8_t vel;     // 1..127, after velocity sensitivity
    uint8_t decay;   // 0..127
    uint8_t a;       // 0..127
    uint8_t b;       // 0..127
    uint8_t x = 0;   // (0.4.3) Kick: click level, other engines: grit. 0 = off (as before)
};

// (0.4.3) Snare/clap morph zone of param B: below kMorphLo the snare and from
// kMorphHi on the clap are computed exactly as in 0.4.2; in between the two
// are blended (noise filter, noise tap, burst level and spacing).
constexpr int kMorphLo = 44, kMorphHi = 84;
// (0.4.3) Internal engine code: a Kick whose beater click is still sounding.
// It falls back to EngKick once the click is over, so the plain kick path is
// exactly the 0.4.2 code.
constexpr uint8_t kEngKickClick = 6;

// (0.4.3) Grit (Pluck, Noise, Metal, Modal, Snare; the engine applies it after
// the voice, see GritState): drive x1..x16 into a hard clip (fuzz), then round
// to 3..10 dropped low bits (12-bit DAC drops 4 anyway) and hold each value
// for 1..4 samples (48..12 kHz). 0 = off.
inline int32_t grit_gain(int g) { return 16 + ((g * 121) >> 6); }   // Q4
inline int grit_drop(int g) { return (g >> 4) + 3; }
inline int32_t grit_half(int g) { return 1 << (grit_drop(g) - 1); }
struct GritState {
    int32_t q = 16, mask = -1, half = 0, lim = 32767, hold = 0;
    int n = 1, cnt = 0;
    void set(int g) {
        q = grit_gain(g);
        mask = ~((1 << grit_drop(g)) - 1);
        half = grit_half(g);
        lim = 32767 - half;
        n = 1 + (g >> 5);
    }
    int32_t process(int32_t y) {
        if (++cnt < n) return hold;
        cnt = 0;
        return hold = (sat((y * q) >> 4, lim) + half) & mask;
    }
};

// Inharmonic FM ratios (Q8) for the Metal engine.
constexpr uint16_t kFmRatios[16] = {256, 361, 384, 443, 512, 617, 707, 804,
                                    896, 1075, 1382, 1613, 1825, 2202, 2534, 2893};
// Modal ratios (Q8) for 8 "materials".
constexpr uint16_t kModalRatios[8][3] = {
    {256, 407, 548},    // membrane
    {256, 1021, 2560},  // marimba bar
    {256, 706, 1383},   // free bar / glockenspiel
    {256, 512, 614},    // bell-ish
    {256, 512, 768},    // tube (harmonic)
    {256, 425, 694},    // plate
    {256, 594, 1088},   // glass
    {256, 376, 535},    // gong
};

inline uint32_t isqrt32(uint32_t x) {
    uint32_t r = 0, bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit) {
        if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

// floor(sqrt(x)) for every uint32_t, identical to isqrt32() but ~3x cheaper
// on the M0+: an 8-bit table seed, one Newton step (one hardware divide) and
// an exact +-1 correction. Checked against isqrt32() in tests/test_engine.cpp.
struct SqrtSeed {
    uint16_t t[256];   // round(16 * sqrt(i))
    constexpr SqrtSeed() : t() {
        for (int i = 0; i < 256; ++i) {
            int r = 0;
            while ((r + 1) * (r + 1) <= 256 * i) ++r;   // floor(16 sqrt(i))
            t[i] = static_cast<uint16_t>(r);
        }
    }
};
inline constexpr SqrtSeed kSqrtSeed{};
inline uint32_t isqrt32_fast(uint32_t x) {
    if (x < 256) return kSqrtSeed.t[x] >> 4;
    uint32_t t = x;
    int k = 0;                                   // result shift
    if (t >= (1u << 24)) { t >>= 16; k += 8; }
    if (t >= (1u << 16)) { t >>= 8; k += 4; }
    if (t >= (1u << 12)) { t >>= 4; k += 2; }
    if (t >= (1u << 8)) { t >>= 4; k += 2; }
    uint32_t r = (static_cast<uint32_t>(kSqrtSeed.t[t]) << k) >> 4;
    if (r == 0) r = 1;
    r = (r + x / r) >> 1;
    if (r > 65535) r = 65535;
    while (r * r > x) --r;
    while (r < 65535 && (r + 1) * (r + 1) <= x) ++r;
    return r;
}

class DrumVoice {
public:
    void trigger(const TriggerArgs& t) {
        const Tables& T = g_tables;
        engine_ = t.engine < EngCount ? t.engine : static_cast<uint8_t>(EngKick);
        const int note = t.note > 127 ? 127 : t.note;
        vel_amp_ = t.vel * 258;               // Q15
        decay_ = t.decay;
        a_ = t.a;
        b_ = t.b;
        // (0.4.3) grit: drive into the clipper, bit and rate crush. 0 = off.
        grit_ = engine_ == EngKick ? 0 : t.x;
        if (grit_) { gq_ = grit_gain(grit_); ghalf_ = grit_half(grit_); }   // for the end detection
        aenv_.start(1u << 24, T.decay_coef[decay_]);
        life_ = (T.decay_samples[decay_] >> 3) + (T.decay_samples[decay_] >> 4) + 64;
        if (!active_) hold_ = 0;   // (from silence: hold 0 until the first sample)
        ctl_ = 0;
        ghost_ = 0;
#ifdef TRK_SOUND_CHECK
        first_ = true;
#endif
        wait_ = 1;   // first sample on the next interrupt (0.4.0)
        active_ = true;
        peak_ = 0;
        quiet_ = 0;
        win_ = 0;
        switch (engine_) {
        case EngKick:
            inc_base_ = T.note_inc[note];
            ph1_ = 0;
            menv_.start(1u << 24, T.decay_coef[clampi(decay_ / 3 + 12, 0, 127)]);
            // (0.4.3) beater click: level x, scaled with the velocity
            click_ = t.x ? (t.x * 258 * vel_amp_) >> 15 : 0;
            click_lp_ = 0;
            click_ph_ = 0;
            if (click_) engine_ = kEngKickClick;
            break;
        case EngPluck: {
            const uint32_t p = T.ks_period[note];
            ks_len_ = static_cast<uint16_t>(p >> 8);
            ks_frac_ = static_cast<int32_t>(p & 255);
            exc_count_ = ks_len_;
            exc_lp_ = 0;
            lp_a_ = clamp32(1500 + a_ * 250, 0, 32767);
            damp_ = (127 - b_) * 129;        // 0 = bright/stretched, 16383 = dark
            int32_t loss = (static_cast<int32_t>(T.ks_loss[decay_]) * ks_len_ * 328) >> 16;
            if (loss < 1) loss = 1;
            gain_ = 32767 - clamp32(loss, 1, 30000);
            if (life_ < 4000) life_ = 4000;
            break;
        }
        case EngNoise:
            makeup_f_ = -1;
            f_base_ = T.svf_f[note];
            q_ = clamp32(32768 - a_ * 254, 400, 32768);
            menv_.start(1u << 24, T.decay_coef[clampi(decay_ / 2 + 10, 0, 127)]);
            break;
        case EngMetal:
            inc1_ = T.note_inc[note];
            inc2_ = static_cast<uint32_t>((static_cast<uint64_t>(inc1_) * kFmRatios[a_ >> 3]) >> 8);
            idx_max_ = b_ * 258;
            menv_.start(1u << 24, T.decay_coef[clampi(decay_ - 12, 0, 127)]);
            break;
        case EngModal: {
            // Mode 0 is set up on the sample after the trigger, modes 1 and 2
            // with the first two output samples (two hardware divides each),
            // so a trigger never shares an interrupt with a setup step.
            modal_note_ = static_cast<uint8_t>(note);
            for (int i = 0; i < 3; ++i) { mlow_[i] = mband_[i] = 0; mgain_[i] = 0; }
            modal_stage_ = 0;
            wait_ = 2;          // mode 0 on the next interrupt, the sound one later
            exc_count_ = 96;
            lp_a_ = clamp32(2000 + b_ * 240, 0, 32767);
            exc_lp_ = 0;
            break;
        }
        case EngSnare:
        default:
            inc_base_ = T.note_inc[note];
            ph1_ = 0;
            menv_.start(1u << 24, T.decay_coef[clampi(decay_ / 3 + 4, 0, 127)]);
            mix_ = 0;
            nmix_ = b_ >= 64 ? 256 : 0;   // noise tap as 0.4.2: b >= 64 band-pass, else high-pass
            if (b_ >= kMorphHi) {   // clap: 3 extra bursts, 9..14 ms apart, band-pass ~1.2 kHz
                burst_left_ = 3;
                burst_gap_ = 192 + (b_ - 64) * 8;
                burst_timer_ = burst_gap_;
                f_base_ = 5000;
            } else if (b_ <= kMorphLo) {   // snare: noise colour from dark to bright
                burst_left_ = 0;
                f_base_ = 6000 + b_ * 330;
            } else {          // (0.4.3) morph: t = 0 (snare) .. 1 (clap)
                mix_ = (b_ - kMorphLo) * 819;                // Q15, 40 steps
                // Q8 blend high-pass -> band-pass, 1 - (1 - t)^3: the high-pass
                // noise dominates the brightness, so it is faded out early
                const int u = kMorphHi - b_;                  // 39 .. 1
                nmix_ = static_cast<uint16_t>(256 - u * u * u * 4 / 1000);
                burst_left_ = 3;
                burst_gap_ = 96 + (b_ - kMorphLo) * 32 / 5;  // 2 .. 7.3 ms, meets the clap at kMorphHi
                burst_timer_ = burst_gap_;
                const int32_t fs = 6000 + (b_ < 63 ? b_ : 63) * 330;   // snare colour, at most its 0.4.2 top
                f_base_ = fs + (((5000 - fs) * mix_) >> 15);
            }
            low_ = band_ = 0;
            break;
        }
    }

    // Fast fade (note-off on a drum track).
    void choke() {
        if (!active_) return;
        aenv_.coef = g_tables.decay_coef[4];
        gain_ = 24000;
        if (life_ > 600) life_ = 600;
    }

    bool active() const { return active_; }
    uint8_t engine() const { return engine_; }
    // Called instead of render() while the voice is silent.
    // Returns false once nothing is owed any more.
    bool idle() {
        if (!ghost_) return false;
        --ghost_;
        if (engine_ == EngMetal) { ph1_ += inc1_; ph2_ += inc2_; }
        else rng_.next();
        return ghost_ != 0;
    }
    // Early end: the mixer's threshold for this voice's channel (see Mixer).
    void set_end_threshold(const int32_t* t) { end_thr_ = t; }
    // A Modal voice still sets up a resonator on its next render (a divide).
    bool setup_pending() const { return active_ && engine_ == EngModal && modal_stage_ < 3; }

    int32_t render() {
        if (!active_) return 0;
        // The trigger and the voice's first sample never share an audio
        // interrupt: every hit starts one sample (21 us) after its trigger
        // (Modal: one more sample that sets up its first resonator). In
        // between the voice holds its last output, so a retriggered ringing
        // voice does not dip to zero.
        if (wait_) {
            if (--wait_ == 0 && engine_ == EngModal) setup_modal(modal_stage_++);
            return hold_;
        }
#ifdef TRK_SOUND_CHECK
        if (ctl_ == 0 && first_) { first_ = false; ++starts_; }
#endif
        return hold_ = render_sound();
    }
    uint8_t grit() const { return grit_; }

private:
    // (0.4.3) Kick beater click: high-passed noise + a 1.6 kHz blip, ~1.5 ms.
    __attribute__((noinline)) int32_t kick_click() {
        const int32_t n = click_rng_.noise();
        click_lp_ += (n - click_lp_) >> 2;
        const int32_t s = sine_lookup(click_ph_);
        click_ph_ += 143165577u;                          // 1.6 kHz at 48 kHz
        const int32_t out = ((((n - click_lp_) >> 1) + (s >> 1)) * click_) >> 15;
        click_ -= (click_ >> 6) + 1;
        if (click_ < 0) click_ = 0;
        return out;
    }

    int32_t render_sound() {
        if (ctl_ == 0) control();
        ctl_ = (ctl_ + 1) & (kControlRate - 1);
        amp_ += amp_step_;
        int32_t y;
        switch (engine_) {
        case EngKick: {
            y = sine_lookup(ph1_);
            ph1_ += inc_cur_;
            y = (y * amp_) >> 15;
            if (b_) y = softclip((y * (16 + b_)) >> 4);
            return y;
        }
        case kEngKickClick: {   // (0.4.3) kick + beater click (only while the click sounds)
            y = sine_lookup(ph1_);
            ph1_ += inc_cur_;
            y = (y * amp_) >> 15;
            y = clamp16(y + kick_click());
            if (b_) y = softclip((y * (16 + b_)) >> 4);
            return y;
        }
        case EngPluck: {
            const int32_t r = (ks_pos_ - ks_len_) & (kKsSize - 1);
            const int32_t d0 = ks_[r];
            const int32_t d1 = ks_[(r - 1) & (kKsSize - 1)];
            const int32_t d = d0 + (((d1 - d0) * ks_frac_) >> 8);
            const int32_t f = d + (((prev_ - d) * damp_) >> 15);
            prev_ = d;
            y = (f * gain_) >> 15;
            if (exc_count_) {
                --exc_count_;
                exc_lp_ += ((rng_.noise() - exc_lp_) * lp_a_) >> 15;
                y += (exc_lp_ * vel_amp_) >> 15;
            }
            y = clamp16(y);
            if (y > peak_) peak_ = y; else if (-y > peak_) peak_ = -y;
            ks_[ks_pos_] = static_cast<int16_t>(y);
            ks_pos_ = (ks_pos_ + 1) & (kKsSize - 1);
            return y;
        }
        case EngNoise: {
            const int32_t in = rng_.noise() >> 3;
            low_ = sat(low_ + ((f_cur_ * band_) >> 15), 65535);
            const int32_t high = sat(in - low_ - ((q_ * band_) >> 14), 65535);
            band_ = sat(band_ + ((f_cur_ * high) >> 15), 65535);
            y = (band_ * makeup_) >> 8;
            return (clamp16(y) * amp_) >> 15;
        }
        case EngMetal: {
            const int32_t m = sine_lookup(ph2_);
            ph2_ += inc2_;
            const uint32_t off = static_cast<uint32_t>(m * idx_cur_) << 2;
            y = sine_lookup(ph1_ + off);
            ph1_ += inc1_;
            return (y * amp_) >> 15;
        }
        case EngModal: {
            if (modal_stage_ < 3) setup_modal(modal_stage_++);
            int32_t x = 0;
            if (exc_count_) {
                --exc_count_;
                exc_lp_ += ((rng_.noise() - exc_lp_) * lp_a_) >> 15;
                x = exc_lp_ >> 2;
            }
            int32_t sum = 0;
            for (int i = 0; i < 3; ++i) {
                mlow_[i] = sat(mlow_[i] + ((mf_[i] * mband_[i]) >> 15), 65535);
                const int32_t hi = sat(((x * mgain_[i]) >> 6) - mlow_[i] - ((mq_[i] * mband_[i]) >> 14), 65535);
                mband_[i] = sat(mband_[i] + ((mf_[i] * hi) >> 15), 65535);
                sum += mband_[i];
            }
            y = clamp16(sum >> 1);
            y = (y * vel_amp_) >> 15;
            if (y > peak_) peak_ = y; else if (-y > peak_) peak_ = -y;
            return y;
        }
        case EngSnare:
        default: {
            int32_t body = sine_lookup(ph1_);
            ph1_ += inc_cur_;
            body = (body * benv_) >> 15;
            const int32_t in = rng_.noise() >> 2;
            low_ = sat(low_ + ((f_base_ * band_) >> 15), 65535);
            const int32_t high = sat(in - low_ - band_, 65535);
            band_ = sat(band_ + ((f_base_ * high) >> 15), 65535);
            // noise tap, fixed per hit (0.4.3: branch-free): nmix 0 = high-pass
            // (snare), 256 = band-pass (clap), exact; in between the morph blend
            const int32_t nz = high + (((band_ - high) * nmix_) >> 8);
            y = (body * (127 - a_) + clamp16(nz) * (a_ + 16)) >> 7;
            return (clamp16(y) * amp_) >> 15;
        }
        }
    }

    void setup_modal(int i) {
        const Tables& T = g_tables;
        const uint16_t* r = kModalRatios[a_ >> 4];
        int32_t f = (static_cast<int32_t>(T.svf_f[modal_note_]) * r[i]) >> 8;
        f = clamp32(f, 40, 26000);
        // Energy a resonator picks up from the short burst falls with
        // frequency (~f), so scale its input by 1/f.
        const int32_t g = (i == 0 ? 32767 : (8000 + b_ * 150) >> (i - 1));
        mgain_[i] = (f >= 26000) ? 0 : clamp32((g >> 6) * (25000 / f), 0, 1 << 20) >> 2;
        mf_[i] = f;
        const int d = clampi(decay_ - 10 * i, 0, 127);
        mq_[i] = clamp32(T.modal_k[d] / f, 3, 32768);
    }

    // True once the voice can no longer reach 1/4 LSB at the outputs.
    bool below_end(int32_t target) {
        int32_t thr = end_thr_ ? *end_thr_ : 0;
        if (thr <= 0) return false;
        // (0.4.3) grit: the output is (y * gq / 16) rounded to the crushed
        // step, so y below max(thr, half step) * 16 / gq can no longer be heard.
        if (__builtin_expect(grit_ != 0, 0)) thr = ((thr > ghalf_ ? thr : ghalf_) << 4) / gq_;
        switch (engine_) {
        case EngPluck:
        case EngModal:
            // Free-ringing: watch the output peak over 4096-sample windows;
            // stop after two quiet windows in a row (not during the burst).
            if (++win_ < 4096 / kControlRate) return false;
            win_ = 0;
            quiet_ = (exc_count_ == 0 && peak_ * 2 < thr) ? quiet_ + 1 : 0;
            peak_ = 0;
            return quiet_ >= 2;
        case EngKick: {
            // Output <= envelope (x drive gain); the envelope only falls.
            const int32_t a = amp_ > target ? amp_ : target;
            return (b_ ? (a * (16 + b_)) >> 4 : a) < thr;
        }
        case kEngKickClick:
            return false;   // (0.4.3) let the click finish
        case EngSnare:
            if (burst_left_) return false;   // clap bursts restart the envelope
            [[fallthrough]];
        default:
            return amp_ < thr && target < thr;
        }
    }

    void control() {
        aenv_.tick();
        int32_t target = (aenv_.q15() * vel_amp_) >> 15;
        if (engine_ == EngModal) target = 32767;   // modal decay lives in the resonators
        amp_step_ = (target - amp_) >> 3;
        if (life_ <= 0 || below_end(target)) {
            // After an early end the free-running state the next hit inherits
            // (noise generator, FM phases) keeps stepping in idle() for as
            // long as the voice would have rung, so the next hits start
            // exactly as without the early end.
            ghost_ = (engine_ == EngNoise || engine_ == EngSnare || engine_ == EngMetal) && life_ > 0 ? life_ * kControlRate : 0;
            active_ = false;
            amp_ = amp_step_ = 0;
            return;
        }
        --life_;
        switch (engine_) {
        case kEngKickClick:
            if (click_ == 0) engine_ = EngKick;   // (0.4.3) click over: plain kick path again
            [[fallthrough]];
        case EngKick: {
            menv_.tick();
            const uint32_t k = (static_cast<uint32_t>(a_) * 7u * static_cast<uint32_t>(menv_.q15())) >> 10;
            // inc_base * (4096 + k) >> 12 without a 64-bit multiply (exact, the
            // M0+ has none): split inc_base into 16-bit halves.
            const uint32_t m = 4096u + k;   // < 2^15
            inc_cur_ = ((inc_base_ >> 16) * m << 4) + (((inc_base_ & 0xFFFFu) * m) >> 12);
            break;
        }
        case EngNoise: {
            menv_.tick();
            const int32_t k = (b_ * 3 * menv_.q15()) >> 7;               // up to ~4x
            f_cur_ = clamp32(f_base_ + ((f_base_ * (k >> 4)) >> 11), 1, 28000);
            // Band-pass noise loudness ~ sqrt(f/q): compensate so every
            // pitch/resonance setting has a similar level.
            // (Only when the cutoff moved: without a sweep it is computed once.)
            if (f_cur_ != makeup_f_) {
                makeup_f_ = f_cur_;
                makeup_ = clamp32(static_cast<int32_t>(12u * isqrt32_fast((static_cast<uint32_t>(q_) << 16) / static_cast<uint32_t>(f_cur_) * 2u)), 64, 32000);
            }
            break;
        }
        case EngMetal:
            menv_.tick();
            idx_cur_ = (menv_.q15() * idx_max_) >> 15;
            break;
        case EngSnare:
        default:
            menv_.tick();
            benv_ = menv_.q15();
            inc_cur_ = inc_base_ + ((inc_base_ >> 15) * static_cast<uint32_t>(benv_ >> 1));
            if (burst_left_) {
                burst_timer_ -= kControlRate;
                if (burst_timer_ <= 0) {
                    if (mix_ == 0) aenv_.v = 1u << 24;
                    else if (aenv_.v < (static_cast<uint32_t>(mix_) << 9)) aenv_.v = static_cast<uint32_t>(mix_) << 9;   // (0.4.3) morph: bursts at level t
                    --burst_left_;
                    burst_timer_ = burst_gap_;
                }
            }
            break;
        case EngPluck:
        case EngModal:
            break;
        }
    }

    uint8_t engine_ = EngKick;
    bool active_ = false;
    uint8_t decay_ = 0, a_ = 0, b_ = 0;
    uint8_t grit_ = 0;      // (0.4.3) in former padding: no offset of a 0.4.2 member moves
    uint16_t nmix_ = 0;     // (0.4.3) snare noise tap, Q8 (0 high-pass, 256 band-pass)
    uint32_t ctl_ = 0;
    int32_t life_ = 0;
    int32_t vel_amp_ = 0, amp_ = 0, amp_step_ = 0;
    Env aenv_, menv_;
    // oscillators
    uint32_t ph1_ = 0, ph2_ = 0, inc1_ = 0, inc2_ = 0, inc_base_ = 0, inc_cur_ = 0;
    int32_t idx_max_ = 0, idx_cur_ = 0, benv_ = 0;
    // filters
    int32_t low_ = 0, band_ = 0, f_base_ = 0, f_cur_ = 0, q_ = 16384, makeup_ = 4096, makeup_f_ = -1;
    int32_t mlow_[3] = {}, mband_[3] = {}, mf_[3] = {}, mq_[3] = {}, mgain_[3] = {};
    // exciter / KS
    int32_t exc_count_ = 0, exc_lp_ = 0, lp_a_ = 0, damp_ = 0, gain_ = 0, prev_ = 0;
    int32_t ks_frac_ = 0;
    uint16_t ks_len_ = 2, ks_pos_ = 0;
    int32_t burst_left_ = 0, burst_gap_ = 0, burst_timer_ = 0;
    uint8_t modal_note_ = 60;
    int modal_stage_ = 3;
    const int32_t* end_thr_ = nullptr;
    int32_t peak_ = 0;
    int quiet_ = 0, win_ = 0;
    int32_t ghost_ = 0;
    int wait_ = 0;
    int32_t hold_ = 0;
public:
#ifdef TRK_SOUND_CHECK
    bool first_ = false;
    uint32_t starts_ = 0;   // tools/sound_check: hits whose first sample rendered
#endif
private:   // noise steps still owed after an early end
    Rng rng_;
    int16_t ks_[kKsSize] = {};
    // (0.4.3) click / grit / morph (at the end: the 0.4.2 members keep their offsets)
    int32_t click_ = 0, click_lp_ = 0;
    uint32_t click_ph_ = 0;
    Rng click_rng_;
    int32_t mix_ = 0;
    int32_t gq_ = 16, ghalf_ = 0;
public:
    void seed(uint32_t s) { rng_.s = s ? s : 1; }
};

} // namespace trk
