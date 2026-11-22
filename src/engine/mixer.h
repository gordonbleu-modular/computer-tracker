// Stereo mixer: 4 drum channels + 2 audio inputs (the Workshop System VCOs)
// -> drive -> tone -> VCA (inputs: open, CV gate or IN track) -> level/pan -> master
// filter (performance macro) -> master drive -> 12-bit outputs.
// SPDX-License-Identifier: MIT
#pragma once

#include "dsp.h"
#include "reverb.h"
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
    int32_t send = 0;                 // reverb send, Q15 incl. level (post-fader, pre-pan), as applied
};

// (0.4.1) Per-channel send bookkeeping, kept out of the hot ChannelState.
struct SendState {
    int32_t tgt = 0;           // where ChannelState::send ramps to (song send or the W lock)
    int32_t lvl = 0;           // channel level Q12 (0 when muted)
    uint8_t song = 0;          // fx_send from the song
    uint8_t lock = 0xFF;       // W effect of the current hit, 0xFF = none
};

class Mixer {
public:
    // Recompute all derived coefficients from the song's mixer section.
    void update(const Song& s, int filter_macro /* -64..63 */) {
        for (int i = 0; i <= kChannels; ++i) { signature(s, i, filter_macro, sig_[i]); update_one(s, i, filter_macro); }
    }

    // Panel macro offsets for the inputs (Main = VCA decay, X = drive), when routed.
    void set_macros(int m_decay, int m_colour) { m_decay_ = m_decay; m_colour_ = m_colour; }

    // Spread the work: one channel (or the master) per call, every quiet
    // sample. The coefficients are only recomputed when one of their inputs
    // (song bytes, routing, macros) changed: same result, ~10x cheaper.
    void update_step(const Song& s, int filter_macro) {
        const int ch = step_;
        step_ = step_ >= kChannels ? 0 : step_ + 1;
        uint32_t sg[4];
        signature(s, ch, filter_macro, sg);
        if (sg[0] == sig_[ch][0] && sg[1] == sig_[ch][1] && sg[2] == sig_[ch][2] && sg[3] == sig_[ch][3]) return;
        sig_[ch][0] = sg[0]; sig_[ch][1] = sg[1]; sig_[ch][2] = sg[2]; sig_[ch][3] = sg[3];
        update_one(s, ch, filter_macro);
    }

    void update_one(const Song& s, int ch, int filter_macro) {
        const Tables& T = g_tables;
        if (ch < kChannels) {
            const uint8_t* m = s.mix[ch];
            ChannelState& c = ch_[ch];
            const int32_t lvl = m[MpMute] >= 64 ? 0 : T.level[m[MpLevel]];   // Q12
            c.gain_l = (lvl * T.pan_l[m[MpPan]]) >> 12;
            c.gain_r = (lvl * T.pan_r[m[MpPan]]) >> 12;
            // Send law: (send / 127)^2, so 64 = -12 dB, 127 = 0 dB (post-fader).
            SendState& ss = snd_[ch];
            ss.lvl = lvl;
            ss.song = c7(s.fx_send[ch]);
            ss.tgt = send_of(ss);
            // Knob / song changes apply at once (as in 0.4.0) unless a W lock
            // is ramping this channel.
            if (!(ramp_mask_ & (1u << ch))) c.send = ss.tgt;
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
            if (ch < kDrumTracks) end_thr_[ch] = end_threshold(ch);
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
        // Reverb: its tail stops once the return would be < 1/2 LSB.
        // (32-bit maths only: this runs inside the audio interrupt.)
        const uint32_t ret_q15 = static_cast<uint32_t>(Reverb::return_gain(s.master[MsRvReturn]));
        const uint32_t den = ((ret_q15 * static_cast<uint32_t>(master_gain_)) >> 12) * static_cast<uint32_t>(master_drive_);
        const uint32_t thr = den ? (1u << 21) / den : 65535u;   // 2^33 / (ret * gain * drive)
        rv_.set(s.master[MsRvReturn], s.master[MsRvSize], s.master[MsRvDecay], s.master[MsRvTone],
                s.master[MsRvLofi], static_cast<int32_t>(thr > 65535 ? 65535 : (thr < 1 ? 1 : thr)));
        for (int v = 0; v < kDrumTracks; ++v) end_thr_[v] = end_threshold(v);
    }

    // Voice amplitude (Q15) below which drum voice v can stop early: its
    // contribution to the 12-bit outputs is then < 1/4 LSB even through the
    // largest gains of the channel and master chain (drive, tone, level/pan,
    // master level, master drive, resonant filter and reverb return margin).
    // 0 = never stop early (muted channel, or the chain is too hot).
    const int32_t* end_threshold_ptr(int v) const { return &end_thr_[v]; }

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

    // (0.4.1) Per-hit reverb send from the W effect (0..127), 0xFF = back to
    // the channel's send knob. The applied send glides to the new value in
    // at most ~5 ms (128 Q15 steps per sample), so a change never clicks in
    // the reverb input. Nothing in the dry path changes.
    void set_send_lock(int ch, uint8_t lock) {
        SendState& ss = snd_[ch];
        if (lock == ss.lock) return;
        ss.lock = lock;
        ss.tgt = send_of(ss);
        if (ss.tgt != ch_[ch].send) ramp_mask_ |= 1u << ch;
    }
    void clear_send_locks() { for (int ch = 0; ch < kChannels; ++ch) set_send_lock(ch, 0xFF); }
    int32_t applied_send(int ch) const { return ch_[ch].send; }   // tests

    // in[0..3] drum voices (Q15), in[4..5] audio inputs (Q15).
    // Writes 12-bit outputs.
    // live_mask: bit per channel with signal (silent voices / unplugged inputs are skipped).
    void process(const int32_t in[kChannels], uint32_t live_mask, int16_t& out_l, int16_t& out_r) {
        // Peak meters look at every 4th sample (they are display only).
        const bool meter = (vca_tick_ & 3) == 0;
        if (!live_mask && master_idle() && !rv_.running()) {
            // Nothing to mix and the master filter is at rest: the output is
            // exactly 0, skip the master stage.
            ++vca_tick_;
            out_l = out_r = 0;
            return;
        }
        if (ramp_mask_) ramp_sends();
        int32_t l = 0, r = 0, snd = 0;
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
            if (meter) {
                const int32_t ax = x < 0 ? -x : x;
                if (ax > c.peak) c.peak = ax;
            }
            l += (x * c.gain_l) >> 15;
            r += (x * c.gain_r) >> 15;
            if (c.send) snd += (x * c.send) >> 15;
        }
        ++vca_tick_;
        // Reverb return joins the bus before the master filter (so the filter
        // macro sweeps the tail too).
        if (snd | static_cast<int32_t>(rv_.running())) rv_.process(snd, l, r);
        if (mf_mode_) {
            l = svf(l, mf_low_[0], mf_band_[0]);
            r = svf(r, mf_low_[1], mf_band_[1]);
        }
        l = (l * master_gain_) >> 12;
        r = (r * master_gain_) >> 12;
        l = softclip((l * master_drive_) >> 4);
        r = softclip((r * master_drive_) >> 4);
        if (meter) {
            const int32_t al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
            if (al > peak_l_) peak_l_ = al;
            if (ar > peak_r_) peak_r_ = ar;
        }
        if (__builtin_expect(mfade_, 0)) {   // (0.4.2) master mute, bit-identical when off
            const int32_t g = master_fade_step();
            l = static_cast<int32_t>((static_cast<int64_t>(l) * g) >> 15);
            r = static_cast<int32_t>((static_cast<int64_t>(r) * g) >> 15);
        }
        out_l = static_cast<int16_t>(clamp32(l >> 4, -2048, 2047));
        out_r = static_cast<int16_t>(clamp32(r >> 4, -2048, 2047));
    }

    // Reverb return peak meter, 0..127, reset after reading.
    int take_reverb_peak() { return rv_.take_peak(); }
    // (0.4.2) Live mutes (not in the song): the reverb return fades out over
    // ~5 ms while the tail keeps running silently, so unmuting brings back
    // what is still ringing; the master fades the stereo output to silence.
    void set_reverb_mute(bool m) { rv_.set_mute(m); }
    void set_master_mute(bool m) { mmute_ = m; mfade_ = true; }
    bool reverb_muted() const { return rv_.muted(); }
    bool master_muted() const { return mmute_; }
    int32_t reverb_return_applied() const { return rv_.return_applied(); }
    bool reverb_heavy() const { return rv_.heavy_next(); }
    bool reverb_running() const { return rv_.running(); }

    // Peak meters, 0..127, reset after reading (status reporting).
    int take_peak(int ch) {
        const int32_t p = ch < kChannels ? ch_[ch].peak : (ch == kChannels ? peak_l_ : peak_r_);
        if (ch < kChannels) ch_[ch].peak = 0;
        else if (ch == kChannels) peak_l_ = 0;
        else peak_r_ = 0;
        return clampi(p >> 8, 0, 127);
    }

private:
    static int32_t send_of(const SendState& ss) {
        const int sd = ss.lock == 0xFF ? ss.song : (ss.lock & 127);
        return clamp32((ss.lvl * g_tables.send_law[sd]) >> 12, 0, 32767);
    }
    void ramp_sends() {
        constexpr int32_t kStep = 128;   // full scale in 256 samples (5.3 ms)
        uint32_t m = ramp_mask_;
        for (int ch = 0; ch < kChannels; ++ch) {
            if (!(m & (1u << ch))) continue;
            ChannelState& c = ch_[ch];
            const int32_t t = snd_[ch].tgt, d = t - c.send;
            if (d > kStep) c.send += kStep;
            else if (d < -kStep) c.send -= kStep;
            else { c.send = t; m &= ~(1u << ch); }
        }
        ramp_mask_ = m;
    }

    int32_t end_threshold(int v) const {
        // (32-bit maths only: this runs inside the audio interrupt.)
        const ChannelState& c = ch_[v];
        const uint32_t g = static_cast<uint32_t>(c.gain_l > c.gain_r ? c.gain_l : c.gain_r);
        const uint32_t gm = (g * static_cast<uint32_t>(master_gain_)) >> 12;   // <= 2^15
        if (gm == 0) return 0;
        const uint32_t den = gm * static_cast<uint32_t>(c.drive > 16 ? c.drive : 16) *
                             (c.tone_mode > 0 ? 2u : 1u) * static_cast<uint32_t>(master_drive_) * 4u;
        const uint32_t t = (1u << 25) / den;   // 2^37 / (...): 4 Q15 at the output = 1/4 LSB
        return static_cast<int32_t>(t > 2048 ? 2048 : t);
    }

    bool master_idle() const {
        return mf_mode_ == 0 || (mf_low_[0] | mf_band_[0] | mf_low_[1] | mf_band_[1]) == 0;
    }

    // Everything update_one(ch) reads, packed into four words.
    void signature(const Song& s, int ch, int filter_macro, uint32_t out[4]) const {
        const uint8_t* m = ch < kChannels ? s.mix[ch] : s.master;
        out[0] = m[0] | (m[1] << 8) | (m[2] << 16) | (static_cast<uint32_t>(m[3]) << 24);
        out[1] = m[4] | (m[5] << 8) | (m[6] << 16) | (static_cast<uint32_t>(m[7]) << 24);
        if (ch < kDrumTracks) out[2] = 0;
        else if (ch < kChannels) out[2] = s.macro_cvin[0] | (s.macro_cvin[1] << 8) |
                                          (static_cast<uint32_t>(m_decay_ + 64) << 16) | (static_cast<uint32_t>(m_colour_ + 64) << 24);
        else out[2] = static_cast<uint32_t>(filter_macro + 64);
        out[3] = ch < kChannels ? s.fx_send[ch] : 0;   // (master: reverb params are master[3..7])
    }

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
    uint32_t sig_[kChannels + 1][4] = {};
    Reverb rv_;
    int32_t end_thr_[kDrumTracks] = {};
    uint32_t ramp_mask_ = 0;   // (0.4.1) channels whose send is gliding
    SendState snd_[kChannels];
    bool mmute_ = false;       // (0.4.2) master mute
    bool mfade_ = false;       // muted or still fading back in
    int32_t mgate_ = 32767;    // master fade gain, Q15
    // One fade step; returns the gain to apply (Q15). Leaves the fade mode once
    // fully back at unity, after which the plain path runs again (bit-identical).
    __attribute__((noinline)) int32_t master_fade_step() {
        if (mmute_) mgate_ = mgate_ > 128 ? mgate_ - 128 : 0;
        else if (mgate_ < 32767 - 128) mgate_ += 128;
        else { mgate_ = 32767; mfade_ = false; return 32768; }
        return mgate_;
    }
};

} // namespace trk
