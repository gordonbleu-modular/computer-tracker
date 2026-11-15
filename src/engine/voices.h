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
        aenv_.start(1u << 24, T.decay_coef[decay_]);
        life_ = (T.decay_samples[decay_] >> 3) + (T.decay_samples[decay_] >> 4) + 64;
        ctl_ = 0;
        active_ = true;
        switch (engine_) {
        case EngKick:
            inc_base_ = T.note_inc[note];
            ph1_ = 0;
            menv_.start(1u << 24, T.decay_coef[clampi(decay_ / 3 + 12, 0, 127)]);
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
            // Mode 0 is set up now; modes 1 and 2 on the next two samples
            // (one hardware divide each) so a trigger never spikes the ISR.
            modal_note_ = static_cast<uint8_t>(note);
            for (int i = 0; i < 3; ++i) { mlow_[i] = mband_[i] = 0; mgain_[i] = 0; }
            setup_modal(0);
            modal_stage_ = 1;
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
            if (b_ >= 64) {   // clap: 3 extra bursts, 4..14 ms apart, band-pass ~1.2 kHz
                burst_left_ = 3;
                burst_gap_ = 192 + (b_ - 64) * 8;
                burst_timer_ = burst_gap_;
                f_base_ = 5000;
            } else {          // snare: noise colour from dark to bright
                burst_left_ = 0;
                f_base_ = 6000 + b_ * 330;
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

    int32_t render() {
        if (!active_) return 0;
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
            return (y * vel_amp_) >> 15;
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
            const int32_t nz = burst_left_ || b_ >= 64 ? band_ : high;
            y = (body * (127 - a_) + clamp16(nz) * (a_ + 16)) >> 7;
            return (clamp16(y) * amp_) >> 15;
        }
        }
    }

private:
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

    void control() {
        aenv_.tick();
        int32_t target = (aenv_.q15() * vel_amp_) >> 15;
        if (engine_ == EngModal) target = 32767;   // modal decay lives in the resonators
        amp_step_ = (target - amp_) >> 3;
        if (life_ <= 0) {
            active_ = false;
            amp_ = amp_step_ = 0;
            return;
        }
        --life_;
        switch (engine_) {
        case EngKick: {
            menv_.tick();
            const uint32_t k = (static_cast<uint32_t>(a_) * 7u * static_cast<uint32_t>(menv_.q15())) >> 10;
            inc_cur_ = static_cast<uint32_t>((static_cast<uint64_t>(inc_base_) * (4096u + k)) >> 12);
            break;
        }
        case EngNoise: {
            menv_.tick();
            const int32_t k = (b_ * 3 * menv_.q15()) >> 7;               // up to ~4x
            f_cur_ = clamp32(f_base_ + ((f_base_ * (k >> 4)) >> 11), 1, 28000);
            // Band-pass noise loudness ~ sqrt(f/q): compensate so every
            // pitch/resonance setting has a similar level.
            makeup_ = clamp32(static_cast<int32_t>(12u * isqrt32((static_cast<uint32_t>(q_) << 16) / static_cast<uint32_t>(f_cur_) * 2u)), 64, 32000);
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
                    aenv_.v = 1u << 24;
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
    uint32_t ctl_ = 0;
    int32_t life_ = 0;
    int32_t vel_amp_ = 0, amp_ = 0, amp_step_ = 0;
    Env aenv_, menv_;
    // oscillators
    uint32_t ph1_ = 0, ph2_ = 0, inc1_ = 0, inc2_ = 0, inc_base_ = 0, inc_cur_ = 0;
    int32_t idx_max_ = 0, idx_cur_ = 0, benv_ = 0;
    // filters
    int32_t low_ = 0, band_ = 0, f_base_ = 0, f_cur_ = 0, q_ = 16384, makeup_ = 4096;
    int32_t mlow_[3] = {}, mband_[3] = {}, mf_[3] = {}, mq_[3] = {}, mgain_[3] = {};
    // exciter / KS
    int32_t exc_count_ = 0, exc_lp_ = 0, lp_a_ = 0, damp_ = 0, gain_ = 0, prev_ = 0;
    int32_t ks_frac_ = 0;
    uint16_t ks_len_ = 2, ks_pos_ = 0;
    int32_t burst_left_ = 0, burst_gap_ = 0, burst_timer_ = 0;
    uint8_t modal_note_ = 60;
    int modal_stage_ = 3;
    Rng rng_;
    int16_t ks_[kKsSize] = {};
public:
    void seed(uint32_t s) { rng_.s = s ? s : 1; }
};

} // namespace trk
