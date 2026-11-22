// Lo-fi stereo reverb on a master send (0.4.0).
//
// Deliberately cheap and a bit grainy, in the spirit of early digital
// reverbs and Workshop System hardware:
//   send sum (mono, 48 kHz) -> 3:1 decimation (3-sample average) -> 16 kHz
//   -> 2 Schroeder allpass diffusers -> 4-line feedback delay network
//      (Hadamard mixing, one-pole damping low-pass and decay gain per line,
//      16-bit lines, optional bit reduction (truncation of the stored
//      samples) and slow pitch wobble)
//   -> stereo taps L = line0 + line2, R = line1 + line3 (different lengths,
//      so the two sides are decorrelated) -> linear or sample-and-hold
//      upsampling back to 48 kHz -> return level.
// The 16 kHz tick is split over three audio samples (diffusers + tail
// detection, FDN read + lines 0/1, lines 2/3), so no single interrupt carries
// all of it. When nothing is sent and
// the tail has decayed below the output resolution, the reverb stops
// rendering (the outputs step from < 1/2 LSB to exactly 0).
// SPDX-License-Identifier: MIT
#pragma once

#include "dsp.h"

namespace trk {

class Reverb {
public:
    static constexpr int kLines = 4;
    // Longest line lengths at 16 kHz (71 / 87 / 105 / 125 ms), coprime-ish.
    static constexpr int kMaxLen0 = 1129, kMaxLen1 = 1391, kMaxLen2 = 1687, kMaxLen3 = 1999;
    // Each line lives in a 2048-sample ring with one shared write index, so a
    // read is buf[(w - len) & mask] (no wrap branches). 4 x 4 KB.
    static constexpr int kRing = 2048, kRingMask = kRing - 1;
    static constexpr int kAp0 = 128, kAp1 = 256;   // rings for the diffusers
    static constexpr int kApLen0 = 75, kApLen1 = 225;
    static constexpr int kQuietWindow = 2048;      // 16 kHz ticks (128 ms) > longest line
    static constexpr int kRate = 16000;

    // Parameters (song values 0..127) and the tail threshold from the mixer:
    // |line0 + line2| and |line1 + line3| below thr = the return is < 1/2 LSB.
    void set(int ret, int size, int decay, int tone, int lofi, int32_t thr) {
        const Tables& T = g_tables;
        ret_ = return_gain(ret);
        if (!fade_) cur_ret_ = ret_;   // no mute fade running: apply at once (as before 0.4.2)
        const int scale = 40 + (size * 87 + 63) / 127;            // 40..127 of 127
        const int max_len[kLines] = {kMaxLen0, kMaxLen1, kMaxLen2, kMaxLen3};
        const uint32_t rate = T.rv_rate[decay];                    // log2 units per tick, Q24
        for (int i = 0; i < kLines; ++i) {
            target_[i] = max_len[i] * scale / 127;
            g_[i] = exp2neg_q15(static_cast<uint32_t>(target_[i]) * rate);
            if (target_[i] != len_[i]) glide_ = true;
        }
        damp_ = 3000 + tone * 232;                                 // one-pole LP coefficient, Q15
        const int bits = lofi >> 4;                                // 0..7 low bits dropped
        mask_ = ~((1 << bits) - 1);
        hold_ = lofi >= 64;                                        // sample-and-hold upsampling
        depth_ = static_cast<int32_t>(lofi >> 3) << 8;             // wobble depth, samples Q8 (0..15)
        thr_ = thr;
    }

    // Return law: 2 * (ret / 127)^2 in Q15, so 90 = 0 dB, 127 = +6 dB.
    static int32_t return_gain(int ret) { return (ret * ret * 65534) / 16129; }

    bool running() const { return run_; }
    // (0.4.2) Mute the return (fades over ~5 ms; the tail keeps running).
    void set_mute(bool m) { mute_ = m; fade_ = true; }
    bool muted() const { return mute_; }
    int32_t return_applied() const { return cur_ret_; }
    // The next process() runs the heaviest third of the tick (fdn_a).
    bool heavy_next() const { return run_ && phase_ == 1; }
    // Peak of the return since the last call (status meter), 0..127.
    int take_peak() {
        const int p = clampi(peak_out_ >> 8, 0, 127);
        peak_out_ = 0;
        return p;
    }

    // One 48 kHz sample: snd = mono send sum (Q15), adds the return to l / r.
    void process(int32_t snd, int32_t& l, int32_t& r) {
        if (!run_) {
            if (!snd) return;
            start();
        }
        acc_ += snd;
        if (snd) in_seen_ = true;
        int32_t ol, orr;
        switch (phase_) {
        case 0:
            phase_ = 1;
            diffuse();
            if (!run_) return;
            ol = (o_l_ += s_l_); orr = (o_r_ += s_r_);
            break;
        case 1:
            phase_ = 2;
            fdn_a();
            ol = o_l_; orr = o_r_;
            break;
        default:
            phase_ = 0;
            fdn_b();
            ol = (o_l_ += s_l_); orr = (o_r_ += s_r_);
            // meter once per tick, on the cheapest phase
            {
                const int32_t a = (ol < 0 ? -ol : ol) | (orr < 0 ? -orr : orr);
                if (a > peak_raw_) peak_raw_ = a;
            }
            if (__builtin_expect(fade_, 0)) fade_step();   // (0.4.2) mute fade (per tick), untouched when not muted
            break;
        }
        if (hold_) { ol = cur_l_; orr = cur_r_; }
        ol = (ol * cur_ret_) >> 15;
        orr = (orr * cur_ret_) >> 15;
        l += ol;
        r += orr;
    }

private:
    // 2^(-e), e in Q24, result Q15 (table + linear interpolation).
    static int32_t exp2neg_q15(uint32_t e) {
        const uint32_t n = e >> 24;
        if (n >= 15) return 0;
        const uint32_t f = e & 0xFFFFFFu;
        const uint32_t i = f >> 18, fr = (f >> 2) & 0xFFFFu;       // 64 steps, 16-bit fraction
        const int32_t a = g_tables.exp2n[i], b = g_tables.exp2n[i + 1];
        const int32_t v = a + (((b - a) * static_cast<int32_t>(fr >> 1)) >> 15);
        return v >> n;
    }

    // a * b / 2^15 rounded toward zero: with plain >> 15 (toward -inf) the
    // feedback loop would settle on a -1 limit cycle instead of decaying to 0.
    static int32_t mulz(int32_t a, int32_t b) {
        const int32_t p = a * b;
        return (p + ((p >> 31) & 32767)) >> 15;
    }
    // a * b / 2^15 rounded away from zero: the damping low-pass then always
    // moves by at least 1 and settles exactly on its input (no dead band).
    static int32_t mula(int32_t a, int32_t b) {
        const int32_t p = a * b;
        return (p + (~(p >> 31) & 32767)) >> 15;
    }
    // Store with the low bits dropped (lo-fi); one's-complement truncation so
    // small negative values go to -1 (and then die), not to -2^bits.
    static int16_t store(int32_t x, int32_t msk) {
        x = sat(x, 32767);
        const int32_t sg = x >> 31;
        return static_cast<int16_t>(((x ^ sg) & msk) ^ sg);
    }

    void start() {
        run_ = true;
        quiet_ = 0;
        peak_ = 0;
        in_seen_ = false;
        phase_ = 0;
        acc_ = 0;
    }

    // Tick part 1: decimate (average of three samples), diffuse, tail detection.
    void diffuse() {
        int32_t x = (acc_ * 21845) >> 16;   // /3 (acc_ is at most ~3 x 2^17)
        acc_ = 0;
        constexpr int32_t g = 19661;   // 0.6
        const uint32_t w = apw_;
        apw_ = w + 1;
        {
            const int32_t v = ap0_[(w - kApLen0) & (kAp0 - 1)];
            const int32_t u = sat(x + ((g * v) >> 15), 32767);
            ap0_[w & (kAp0 - 1)] = static_cast<int16_t>(u);
            x = v - ((g * u) >> 15);
        }
        {
            const int32_t v = ap1_[(w - kApLen1) & (kAp1 - 1)];
            const int32_t u = sat(x + ((g * v) >> 15), 32767);
            ap1_[w & (kAp1 - 1)] = static_cast<int16_t>(u);
            x = v - ((g * u) >> 15);
        }
        diff_ = x;
        // Stop once a whole window stayed below the threshold with nothing sent.
        const int32_t al = cur_l_ < 0 ? -cur_l_ : cur_l_, ar = cur_r_ < 0 ? -cur_r_ : cur_r_;
        const int32_t m = al | ar;
        if (m > peak_) peak_ = m;
        if (++quiet_ >= kQuietWindow) {
            if (!in_seen_ && peak_ < thr_) {
                run_ = false;
                cur_l_ = cur_r_ = o_l_ = o_r_ = s_l_ = s_r_ = 0;
            }
            quiet_ = 0;
            peak_ = 0;
            in_seen_ = false;
        }
        if (++meter_ >= 64) {   // fold the raw output peak into the meter (x return gain)
            meter_ = 0;
            const int32_t p = (peak_raw_ * cur_ret_) >> 15;
            if (p > peak_out_) peak_out_ = p;
            peak_raw_ = 0;
        }
    }

    // Tick part 2: read the four lines (line 0 with the wobble), stereo taps,
    // Hadamard mix, write lines 0 and 1.
    void fdn_a() {
        const uint32_t w = w_;
        int32_t d0;
        if (depth_) {
            // A slow triangle (~0.6 Hz) on line 0, linear interpolation.
            lfo_ += 161061u;                                       // 2^32 * 0.6 / 16000
            const int32_t t = static_cast<int32_t>(lfo_ >> 16);   // 0..65535
            const int32_t tri = t < 32768 ? t : 65535 - t;         // 0..32767
            const int32_t off = (depth_ * tri) >> 15;              // samples Q8
            const uint32_t i0 = (w - static_cast<uint32_t>(len_[0]) - static_cast<uint32_t>(off >> 8)) & kRingMask;
            const int32_t a = l0_[i0], b = l0_[(i0 - 1) & kRingMask];
            d0 = a + (((b - a) * (off & 255)) >> 8);
        } else {
            d0 = l0_[(w - static_cast<uint32_t>(len_[0])) & kRingMask];
        }
        const int32_t d1 = l1_[(w - static_cast<uint32_t>(len_[1])) & kRingMask];
        const int32_t d2 = l2_[(w - static_cast<uint32_t>(len_[2])) & kRingMask];
        const int32_t d3 = l3_[(w - static_cast<uint32_t>(len_[3])) & kRingMask];
        // Stereo taps; the output ramps from the previous tap value to this one
        // over the next three samples (linear upsampling).
        const int32_t pl = cur_l_, pr = cur_r_;
        cur_l_ = d0 + d2;
        cur_r_ = d1 + d3;
        o_l_ = pl;
        o_r_ = pr;
        s_l_ = ((cur_l_ - pl) * 21845) >> 16;
        s_r_ = ((cur_r_ - pr) * 21845) >> 16;
        // Hadamard (orthogonal with the 1/2), damping low-pass, decay gain, input.
        const int32_t a = d0 + d1, b = d0 - d1, c = d2 + d3, e = d2 - d3;
        h2_ = (a - c) >> 1;
        h3_ = (b - e) >> 1;
        const int32_t in = diff_, dmp = damp_, msk = mask_;
        int32_t lp;
        lp = lp_[0]; lp += mula(((a + c) >> 1) - lp, dmp); lp_[0] = lp;
        l0_[w & kRingMask] = store(mulz(lp, g_[0]) + in, msk);
        lp = lp_[1]; lp += mula(((b + e) >> 1) - lp, dmp); lp_[1] = lp;
        l1_[w & kRingMask] = store(mulz(lp, g_[1]) - in, msk);
    }

    // Tick part 3: write lines 2 and 3, advance, size glide.
    void fdn_b() {
        const uint32_t w = w_;
        w_ = w + 1;
        const int32_t in = diff_, dmp = damp_, msk = mask_;
        int32_t lp;
        lp = lp_[2]; lp += mula(h2_ - lp, dmp); lp_[2] = lp;
        l2_[w & kRingMask] = store(mulz(lp, g_[2]) + in, msk);
        lp = lp_[3]; lp += mula(h3_ - lp, dmp); lp_[3] = lp;
        l3_[w & kRingMask] = store(mulz(lp, g_[3]) - in, msk);
        if (glide_) glide();   // size changes glide one sample per tick (a little tape-like bend)
    }

    void glide() {
        bool moving = false;
        for (int i = 0; i < kLines; ++i) {
            if (len_[i] < target_[i]) { ++len_[i]; moving = true; }
            else if (len_[i] > target_[i]) { --len_[i]; moving = true; }
        }
        glide_ = moving;
    }

    // parameters
    __attribute__((noinline)) void fade_step() {
        const int32_t tg = mute_ ? 0 : ret_;
        if (cur_ret_ < tg) cur_ret_ = cur_ret_ + 384 > tg ? tg : cur_ret_ + 384;   // ~5 ms at 16 kHz
        else if (cur_ret_ > tg) cur_ret_ = cur_ret_ - 384 < tg ? tg : cur_ret_ - 384;
        else if (!mute_) fade_ = false;   // back at the knob value: plain path again
    }
    int32_t cur_ret_ = 0;   // (0.4.2) applied return gain (fades on mute)
    int32_t ret_ = 0, damp_ = 16000, mask_ = -1, depth_ = 0, thr_ = 0;
    int32_t g_[kLines] = {}, target_[kLines] = {kMaxLen0, kMaxLen1, kMaxLen2, kMaxLen3};
    bool hold_ = false, glide_ = false;
    bool mute_ = false, fade_ = false;   // (0.4.2) fade_: muted or still fading back
    // state
    bool run_ = false, in_seen_ = false;
    uint32_t phase_ = 0, lfo_ = 0, w_ = 0, apw_ = 0, meter_ = 0;
    int32_t acc_ = 0, diff_ = 0, h2_ = 0, h3_ = 0;
    int32_t cur_l_ = 0, cur_r_ = 0, o_l_ = 0, o_r_ = 0, s_l_ = 0, s_r_ = 0;
    int32_t peak_ = 0, quiet_ = 0, peak_out_ = 0, peak_raw_ = 0;
    int32_t len_[kLines] = {kMaxLen0, kMaxLen1, kMaxLen2, kMaxLen3}, lp_[kLines] = {};
    int16_t ap0_[kAp0] = {}, ap1_[kAp1] = {};
    int16_t l0_[kRing] = {}, l1_[kRing] = {}, l2_[kRing] = {}, l3_[kRing] = {};
};

} // namespace trk
