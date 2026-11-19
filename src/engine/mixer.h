// Stereo mixer: 4 drum channels + 2 audio inputs (the Workshop System VCOs)
// -> drive -> tone -> VCA (inputs: open, CV gate or IN track) -> level/pan -> master
// filter (performance macro) -> master drive -> 12-bit outputs.
// SPDX-License-Identifier: MIT
#pragma once

#include "dsp.h"
#include "song.h"

namespace trk {

struct ChannelState {
    int32_t gain_l = 0, gain_r = 0;   // Q15 incl. level and pan
    int32_t drive = 0;                // 0 = bypass, else Q4 gain
    int32_t tone_a = 0;               // one-pole coefficient Q15
    int8_t tone_mode = 0;             // -1 LP, 0 bypass, 1 HP
    int32_t lp = 0;
    int32_t vca = 0;                  // input VCA release coef (0 = VCA open)
    int32_t vca_env = 32767;
    int32_t peak = 0;
};

class Mixer {
public:
    // Recompute all derived coefficients from the song's mixer section.
    void update(const Song& s, int filter_macro /* -64..63 */) {
        for (int i = 0; i <= kChannels; ++i) update_one(s, i, filter_macro);
    }

    // Panel macro offsets for the inputs (Main = VCA decay, X = drive), when routed.
    void set_macros(int m_decay, int m_colour) { m_decay_ = m_decay; m_colour_ = m_colour; }

    // Spread the work: one channel (or the master) per call, every sample.
    void update_step(const Song& s, int filter_macro) {
        update_one(s, step_, filter_macro);
        step_ = step_ >= kChannels ? 0 : step_ + 1;
    }

    void update_one(const Song& s, int ch, int filter_macro) {
        const Tables& T = g_tables;
        if (ch < kChannels) {
            const uint8_t* m = s.mix[ch];
            ChannelState& c = ch_[ch];
            const int32_t lvl = m[MpMute] >= 64 ? 0 : T.level[m[MpLevel]];   // Q12
            c.gain_l = (lvl * T.pan_l[m[MpPan]]) >> 12;
            c.gain_r = (lvl * T.pan_r[m[MpPan]]) >> 12;
            int drive = m[MpDrive];
            if (ch >= kDrumTracks && macro_on_in(s, MacroColour, ch - kDrumTracks)) drive = clampi(drive + m_colour_, 0, 127);
            c.drive = drive ? 16 + drive / 2 : 0;
            const int tone = m[MpTone];
            if (tone < 60) {
                c.tone_mode = -1;
                c.tone_a = 600 + tone * tone * 8;        // darker as it goes left
            } else if (tone > 68) {
                c.tone_mode = 1;
                c.tone_a = 40 + (tone - 68) * (tone - 68) * 5;
            } else {
                c.tone_mode = 0;
            }
            if (ch >= kDrumTracks) {
                const int i = ch - kDrumTracks;
                src_[i] = vca_source(s, i);
                decay_off_[i] = macro_on_in(s, MacroDecay, i) ? m_decay_ : 0;
                c.vca = src_[i] == VcaOpen ? 0 : T.decay_coef[clampi(m[MpVca] + decay_off_[i], 0, 127)];
            }
            return;
        }
        master_gain_ = T.level[s.master[MsLevel]];
        master_drive_ = 16 + s.master[MsDrive] / 2;
        const int f = clampi(s.master[MsFilter] - 64 + filter_macro, -64, 63);
        if (f < -3) {
            mf_mode_ = -1;
            mf_f_ = clamp32(30000 - (-f) * 460, 900, 30000);   // LP sweeps down
        } else if (f > 3) {
            mf_mode_ = 1;
            mf_f_ = clamp32(200 + f * f * 6, 200, 26000);     // HP sweeps up
        } else {
            mf_mode_ = 0;
        }
    }

    // What opens input i's VCA (from the song, refreshed by update_one).
    VcaSrc source(int input) const { return src_[input]; }
    // Input VCA control for this sample: gate, velocity (0 = full level) and a
    // per-step decay lock (kNoLock-style 0xFF = use the song's decay).
    void set_vca(int input, bool g, uint8_t vel, uint8_t decay_lock) {
        gate_[input] = g;
        level_[input] = vel ? vel * 258 : 32767;
        lock_[input] = decay_lock;
    }
    void set_gate(int input, bool g) { set_vca(input, g, 0, 0xFF); }

    // in[0..3] drum voices (Q15), in[4..5] audio inputs (Q15).
    // Writes 12-bit outputs.
    // live_mask: bit per channel with signal (silent voices / unplugged inputs are skipped).
    void process(const int32_t in[kChannels], uint32_t live_mask, int16_t& out_l, int16_t& out_r) {
        int32_t l = 0, r = 0;
        for (int ch = 0; ch < kChannels; ++ch) {
            if (!(live_mask & (1u << ch))) continue;
            ChannelState& c = ch_[ch];
            int32_t x = in[ch];
            if (c.drive) x = softclip((x * c.drive) >> 4);
            if (c.tone_mode) {
                c.lp += ((x - c.lp) * c.tone_a) >> 15;
                x = c.tone_mode < 0 ? c.lp : x - c.lp;
            }
            if (ch >= kDrumTracks && c.vca) {
                ChannelState& cs = c;
                const int i = ch - kDrumTracks;
                if (gate_[i]) {
                    cs.vca_env += (level_[i] - cs.vca_env) >> 6;      // ~1.3 ms attack (or level change)
                } else if ((vca_tick_ & 7) == 0) {
                    const int32_t coef = lock_[i] == 0xFF ? cs.vca
                                         : g_tables.decay_coef[clampi(lock_[i] + decay_off_[i], 0, 127)];
                    cs.vca_env = (cs.vca_env * coef) >> 16;
                }
                x = (x * cs.vca_env) >> 15;
            }
            const int32_t ax = x < 0 ? -x : x;
            if (ax > c.peak) c.peak = ax;
            l += (x * c.gain_l) >> 15;
            r += (x * c.gain_r) >> 15;
        }
        ++vca_tick_;
        if (mf_mode_) {
            l = svf(l, mf_low_[0], mf_band_[0]);
            r = svf(r, mf_low_[1], mf_band_[1]);
        }
        l = (l * master_gain_) >> 12;
        r = (r * master_gain_) >> 12;
        l = softclip((l * master_drive_) >> 4);
        r = softclip((r * master_drive_) >> 4);
        const int32_t al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
        if (al > peak_l_) peak_l_ = al;
        if (ar > peak_r_) peak_r_ = ar;
        out_l = static_cast<int16_t>(clamp32(l >> 4, -2048, 2047));
        out_r = static_cast<int16_t>(clamp32(r >> 4, -2048, 2047));
    }

    // Peak meters, 0..127, reset after reading (status reporting).
    int take_peak(int ch) {
        const int32_t p = ch < kChannels ? ch_[ch].peak : (ch == kChannels ? peak_l_ : peak_r_);
        if (ch < kChannels) ch_[ch].peak = 0;
        else if (ch == kChannels) peak_l_ = 0;
        else peak_r_ = 0;
        return clampi(p >> 8, 0, 127);
    }

private:
    // Chamberlin SVF, q = 0.7. Runs at half scale so the states fit 16x16 products.
    int32_t svf(int32_t x, int32_t& low, int32_t& band) {
        x = sat(x >> 1, 65535);
        low = sat(low + ((mf_f_ * band) >> 15), 65535);
        const int32_t high = sat(x - low - ((23170 * band) >> 15), 65535);
        band = sat(band + ((mf_f_ * high) >> 15), 65535);
        return (mf_mode_ < 0 ? low : high) * 2;
    }

    ChannelState ch_[kChannels];
    bool gate_[kInputs] = {false, false};
    int32_t level_[kInputs] = {32767, 32767};
    uint8_t lock_[kInputs] = {0xFF, 0xFF};
    VcaSrc src_[kInputs] = {VcaOpen, VcaOpen};
    int decay_off_[kInputs] = {0, 0};
    int m_decay_ = 0, m_colour_ = 0;
    uint32_t vca_tick_ = 0;
    int32_t master_gain_ = 4096, master_drive_ = 16;
    int8_t mf_mode_ = 0;
    int32_t mf_f_ = 30000;
    int32_t mf_low_[2] = {}, mf_band_[2] = {};
    int32_t peak_l_ = 0, peak_r_ = 0;
    int step_ = 0;
};

} // namespace trk
