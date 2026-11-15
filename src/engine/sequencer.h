// Tracker playback: clock (internal with swing, or external pulses), song
// position, per-row events, effect handling and the two CV/gate tracks.
// SPDX-License-Identifier: MIT
#pragma once

#include "dsp.h"
#include "song.h"

namespace trk {

enum class PlayMode : uint8_t { Stopped = 0, Song = 1, Pattern = 2 };

struct DrumEvent {
    uint8_t track;
    uint8_t note;   // kNoteOff = choke
    uint8_t vel;
    uint8_t fx, fxv;
};

struct ClockIn {
    bool ext_connected = false;   // a cable in Pulse In 1
    bool ext_rise = false;        // rising edge this sample
    bool reset_rise = false;      // Pulse In 2 rising edge
};

struct Position {
    uint8_t entry = 0, rep = 0, row = 0, pattern = 0;
};

class Sequencer {
public:
    static constexpr int kQueue = 16;
    static constexpr int kRetrigGap = 96;   // 2 ms gate low between notes

    void attach(const Song* s) { song_ = s; pending_ = nullptr; refresh_tempo(true); }
    const Song* song() const { return song_; }

    // Switch to another song at the next pattern boundary (or now if stopped).
    void request_song(const Song* s, bool immediate) {
        if (immediate || mode_ == PlayMode::Stopped) {
            song_ = s;
            pending_ = nullptr;
            pos_ = Position{};
            pos_.pattern = song_->entries[0].pattern;
            swapped_ = true;
            if (mode_ != PlayMode::Stopped) start_from_here();
        } else {
            pending_ = s;
        }
    }
    bool take_swapped() { const bool s = swapped_; swapped_ = false; return s; }
    bool swap_pending() const { return pending_ != nullptr; }

    void play_song(int entry) {
        mode_ = PlayMode::Song;
        pos_ = Position{};
        pos_.entry = static_cast<uint8_t>(clampi(entry, 0, song_->song_len - 1));
        pos_.pattern = song_->entries[pos_.entry].pattern;
        start_from_here();
    }
    void play_pattern(int pattern) {
        mode_ = PlayMode::Pattern;
        pos_ = Position{};
        pos_.pattern = static_cast<uint8_t>(clampi(pattern, 0, kPatterns - 1));
        start_from_here();
    }
    // While looping a pattern: change which one at the next boundary.
    void cue_pattern(int pattern) { cue_pattern_ = static_cast<int8_t>(clampi(pattern, 0, kPatterns - 1)); }
    // While playing a song: jump to this entry at the next boundary.
    void cue_entry(int entry) { cue_entry_ = static_cast<int8_t>(clampi(entry, 0, kSongEntries - 1)); }
    // Performance "next part": leave the current entry at the end of this pattern.
    void queue_next() { next_part_ = true; }
    bool next_queued() const { return next_part_ || cue_entry_ >= 0; }
    void stop() {
        mode_ = PlayMode::Stopped;
        for (int c = 0; c < kCvTracks; ++c) { cv_[c].gate = false; cv_[c].gate_off = 0; cv_[c].retrig = 0; cv_[c].rat_left = 0; }
        for (auto& p : pend_) p.countdown = 0;
        for (auto& r : rat_) r.left = 0;
        row_delay_ = 0;
        row_track_ = kTracks;
    }
    void toggle() {
        if (mode_ == PlayMode::Stopped) {
            play_song(pos_.entry);
        } else {
            stop();
        }
    }

    PlayMode mode() const { return mode_; }
    bool playing() const { return mode_ != PlayMode::Stopped; }
    const Position& pos() const { return pos_; }
    int row_len() const { return row_len_; }
    uint32_t rows_played() const { return rows_played_; }

    // Live transpose of the CV tracks in semitones (CV In 1).
    void set_transpose(int semis) { transpose_ = semis; }

    // Drum events for the engine, consumed by pop_drum().
    bool pop_drum(DrumEvent& e) {
        if (q_head_ == q_tail_) return false;
        e = q_[q_tail_];
        q_tail_ = (q_tail_ + 1) % kQueue;
        return true;
    }

    // Preview a note immediately (editor keyboard / audition).
    void preview(int track, int note, int vel) {
        Cell c{static_cast<uint8_t>(note), static_cast<uint8_t>(vel), 0, 0};
        fire(track, c, false);
        if (track >= kDrumTracks) {
            CvState& cv = cv_[track - kDrumTracks];
            if (cv.gate_off == 0 && note != kNoteOff) cv.gate_off = kSampleRate / 4;
        }
    }

    // One sample. Returns true on the sample a row starts (for LEDs).
    bool tick(const ClockIn& in) {
        refresh_tempo(false);
        bool row_now = false;

        if (in.reset_rise) {
            pos_.row = 0; pos_.rep = 0; pos_.entry = 0;
            pos_.pattern = mode_ == PlayMode::Pattern ? pos_.pattern : song_->entries[0].pattern;
            first_ = true;
            if (!in.ext_connected && playing()) { pair_phase_ = 0; row_now = true; }
        }

        if (in.ext_connected) {
            ++since_edge_;
            if (in.ext_rise) {
                if (since_edge_ > 48 && since_edge_ < kSampleRate * 4) row_len_ = static_cast<int>(since_edge_);
                since_edge_ = 0;
                if (playing()) {
                    // Swing an odd row by delaying it.
                    const bool next_odd = first_ ? false : ((pos_.row + 1) & 1);
                    if (next_odd && song_->swing > 50) row_delay_ = (row_len_ * (song_->swing - 50)) / 50;
                    else row_now = true;
                }
            }
            if (row_delay_ > 0 && --row_delay_ == 0) row_now = true;
        } else if (playing()) {
            const uint32_t prev = pair_phase_;
            pair_phase_ += pair_inc_;
            if (pair_phase_ < prev) row_now = true;                                    // even row
            else if (prev < swing_point_ && pair_phase_ >= swing_point_) row_now = true;  // odd row
        }

        // Internal clock: start plays row 0 at once. External clock: the
        // first pulse after start plays row 0.
        if (start_now_) { start_now_ = false; if (!in.ext_connected) row_now = true; }

        if (row_now && playing()) {
            if (!first_) advance();
            first_ = false;
            row_track_ = 0;
            row_sub_ = 0;
            ++rows_played_;
        }
        // The row's tracks are read one every other sample (12 samples =
        // 250 us); the engine starts the resulting drum hits on the samples
        // in between, so a busy row never piles up in one audio interrupt.
        if (row_track_ < kTracks) {
            if ((row_sub_ ^= 1) != 0) {
                if (playing()) play_track(row_track_);
                ++row_track_;
            }
        }

        // Scheduled (delayed / ratcheted) events.
        for (int t = 0; t < kTracks; ++t) {
            Pending& p = pend_[t];
            if (p.countdown > 0 && --p.countdown == 0) fire_with_ratchet(t, p.cell);
            Ratchet& r = rat_[t];
            if (r.left > 0 && --r.timer <= 0) {
                --r.left;
                r.timer = r.interval;
                fire(t, r.cell, true);
            }
        }
        update_cv();
        return row_now && playing();
    }

    // CV outputs: pitch in millivolts (>= 0) and gate.
    int32_t cv_mv(int c) const { return cv_[c].mv; }
    bool gate(int c) const { return cv_[c].gate && cv_[c].retrig == 0; }
    uint8_t cv_note(int c) const { return cv_[c].note; }

private:
    struct CvState {
        int32_t pitch = 0;        // semitones Q8 (MIDI note * 256)
        int32_t target = 0;
        int32_t step = 0;
        int32_t gate_off = 0;     // samples until gate falls (0 = tie)
        int32_t retrig = 0;       // samples of forced low before gate rises
        bool gate = false;
        int32_t mv = 0;
        uint8_t note = 0;
        int32_t rat_left = 0;
    };
    struct Pending { int32_t countdown = 0; Cell cell{}; };
    struct Ratchet { int32_t left = 0, timer = 0, interval = 0; Cell cell{}; };

    void refresh_tempo(bool force) {
        const int bpm = song_->tempo();
        if (force || bpm != cached_bpm_) {
            cached_bpm_ = bpm;
            // 2^32 / (48000 * 30) = 2982.616 per BPM: one pair of rows per cycle.
            pair_inc_ = static_cast<uint32_t>(bpm) * 2982u + ((static_cast<uint32_t>(bpm) * 631u) >> 10);
            row_len_internal_ = 720000 / bpm;
        }
        if (force || song_->swing != cached_swing_) {
            cached_swing_ = song_->swing;
            swing_point_ = static_cast<uint32_t>(cached_swing_) * 42949673u;   // swing % of 2^32
        }
        if (!clock_ext_) row_len_ = row_len_internal_;
    }

    void start_from_here() {
        first_ = true;
        start_now_ = true;
        pair_phase_ = 0;
        cue_entry_ = -1;
        cue_pattern_ = -1;
        next_part_ = false;
    }

    void advance() {
        const Song& s = *song_;
        ++pos_.row;
        if (pos_.row < s.pat_len[pos_.pattern]) return;
        pos_.row = 0;
        if (pending_) {
            song_ = pending_;
            pending_ = nullptr;
            swapped_ = true;
            pos_ = Position{};
            if (mode_ == PlayMode::Pattern) mode_ = PlayMode::Song;
            pos_.pattern = song_->entries[0].pattern;
            next_part_ = false;
            cue_entry_ = -1;
            return;
        }
        if (mode_ == PlayMode::Pattern) {
            if (cue_pattern_ >= 0) { pos_.pattern = static_cast<uint8_t>(cue_pattern_); cue_pattern_ = -1; }
            return;
        }
        if (cue_entry_ >= 0) {
            pos_.entry = static_cast<uint8_t>(cue_entry_ % s.song_len);
            pos_.rep = 0;
            cue_entry_ = -1;
            next_part_ = false;
        } else if (next_part_ || ++pos_.rep >= s.entries[pos_.entry].repeats) {
            next_part_ = false;
            pos_.rep = 0;
            pos_.entry = static_cast<uint8_t>((pos_.entry + 1) % s.song_len);
        }
        pos_.pattern = s.entries[pos_.entry].pattern;
    }

    void play_track(int t) {
        const Cell c = song_->cells[pos_.pattern][t][pos_.row];
        if (c.note == kNoteEmpty && c.fx == FxNone) return;
        if (c.fx == FxProb && c.fxv < 100) {
            if (static_cast<int>(((rng_.next() >> 16) * 100u) >> 16) >= c.fxv) return;
        }
        rat_[t].left = 0;
        if (c.fx == FxDelay && c.fxv > 0) {
            pend_[t].cell = c;
            pend_[t].countdown = (row_len_ * clampi(c.fxv, 1, 15)) >> 4;
        } else {
            pend_[t].countdown = 0;
            fire_with_ratchet(t, c);
        }
    }

    void fire_with_ratchet(int t, const Cell& c) {
        fire(t, c, false);
        if (c.fx == FxRatchet && c.fxv >= 2 && c.note != kNoteEmpty && c.note != kNoteOff) {
            const int n = clampi(c.fxv, 2, 8);
            Ratchet& r = rat_[t];
            r.left = n - 1;
            r.interval = row_len_ / n;
            r.timer = r.interval;
            r.cell = c;
        }
    }

    void fire(int t, const Cell& c, bool repeat) {
        if (c.note == kNoteEmpty) return;
        if (t < kDrumTracks) {
            const int nh = (q_head_ + 1) % kQueue;
            if (nh == q_tail_) return;   // queue full: drop
            q_[q_head_] = DrumEvent{static_cast<uint8_t>(t), c.note, c.vel ? c.vel : static_cast<uint8_t>(100), c.fx, c.fxv};
            q_head_ = nh;
            return;
        }
        CvState& cv = cv_[t - kDrumTracks];
        const uint8_t* prm = song_->cv[t - kDrumTracks];
        if (c.note == kNoteOff) {
            cv.gate = false;
            cv.gate_off = 0;
            return;
        }
        const bool slide = c.fx == FxSlide || (prm[CpGlide] > 0 && c.fx != FxSlide);
        const int glide_amt = c.fx == FxSlide ? c.fxv : prm[CpGlide];
        cv.note = c.note;
        cv.target = (c.note + (prm[CpTranspose] - 64)) * 256;
        if (slide && glide_amt > 0 && cv.gate && !repeat) {
            const int32_t samples = 48 + glide_amt * glide_amt * 6;    // up to ~2 s
            cv.step = (cv.target - cv.pitch) / samples;
            if (cv.step == 0) cv.step = cv.target > cv.pitch ? 1 : -1;
        } else {
            cv.pitch = cv.target;
            cv.step = 0;
        }
        // Gate: slides keep the gate high (legato), everything else retriggers.
        const bool legato = c.fx == FxSlide && cv.gate;
        if (cv.gate && !legato) cv.retrig = kRetrigGap;
        cv.gate = true;
        int len16 = prm[CpGate];
        if (c.fx == FxGate) len16 = c.fxv;
        if (c.fx == FxRatchet && c.fxv >= 2) {
            const int n = clampi(c.fxv, 2, 8);
            cv.gate_off = (row_len_ / n) / 2;
        } else if (len16 == 0) {
            cv.gate_off = 0;        // tie until the next note or OFF
        } else {
            cv.gate_off = (row_len_ * len16) >> 4;
            if (cv.gate_off < kRetrigGap + 48) cv.gate_off = kRetrigGap + 48;
        }
    }

    void update_cv() {
        for (int c = 0; c < kCvTracks; ++c) {
            CvState& cv = cv_[c];
            if (cv.retrig > 0) --cv.retrig;
            if (cv.gate_off > 0 && --cv.gate_off == 0) cv.gate = false;
            if (cv.step) {
                cv.pitch += cv.step;
                if ((cv.step > 0 && cv.pitch >= cv.target) || (cv.step < 0 && cv.pitch <= cv.target)) {
                    cv.pitch = cv.target;
                    cv.step = 0;
                }
            }
            const int32_t rel = cv.pitch + (transpose_ - song_->cv_zero_note) * 256;
            int32_t mv = (rel * 21333) >> 16;          // 1000 mV / (12 * 256)
            cv.mv = clamp32(mv, 0, 6000);
        }
    }

    const Song* song_ = nullptr;
    const Song* pending_ = nullptr;
    bool swapped_ = false;
    PlayMode mode_ = PlayMode::Stopped;
    Position pos_;
    bool first_ = true, start_now_ = false, next_part_ = false;
    int8_t cue_entry_ = -1, cue_pattern_ = -1;
    uint32_t pair_phase_ = 0, pair_inc_ = 0, swing_point_ = 0x80000000u;
    int cached_bpm_ = -1, cached_swing_ = -1;
    int row_len_ = 6000, row_len_internal_ = 6000;
    bool clock_ext_ = false;
    uint32_t since_edge_ = 0;
    int32_t row_delay_ = 0;
    uint32_t rows_played_ = 0;
    int row_track_ = kTracks;
    int row_sub_ = 0;
    int transpose_ = 0;
    Pending pend_[kTracks];
    Ratchet rat_[kTracks];
    CvState cv_[kCvTracks];
    DrumEvent q_[kQueue];
    int q_head_ = 0, q_tail_ = 0;
    Rng rng_;

public:
    void set_clock_ext(bool e) { clock_ext_ = e; }
};

} // namespace trk
