// Front-panel performance controls (works with or without USB).
//
//   Switch Middle  PERFORM  Main = decay macro, X = colour macro, Y = filter macro
//   Switch Up      SELECT   Main = song slot 1-6, X = tempo, Y = swing
//   Switch Down    (momentary) tap = start / next part, hold 0.8 s = stop
//
// Values that are stored in the song (tempo, swing) use soft takeover: the
// knob only takes over once it passes the stored value. Macros keep their
// value while you are on the SELECT page and pick up again on return.
// SPDX-License-Identifier: MIT
#pragma once

#include "dsp.h"

#include <cstdlib>

namespace trk {

enum class Sw : uint8_t { Up = 0, Middle = 1, Down = 2 };

// Soft takeover for one knob/value pair.
struct Pickup {
    int value = 0;         // knob-space target (0..4095)
    bool caught = true;
    int last = -1;
    static constexpr int kWindow = 48;

    void arm(int target_knob) { value = target_knob; caught = false; last = -1; }
    // Returns true when the knob controls the value.
    bool update(int knob) {
        if (!caught) {
            const int d = knob - value;
            const bool near = d > -kWindow && d < kWindow;
            const bool crossed = last >= 0 && ((last - value) ^ (knob - value)) < 0;
            if (near || crossed) caught = true;
            last = knob;
        }
        if (caught) value = knob;
        return caught;
    }
};

struct PanelOut {
    bool start_or_next = false;   // tap
    bool stop = false;            // hold
    int select_slot = -1;         // 0..5 when the slot knob settles on a new slot
    int tempo = -1;               // new BPM if the knob changed it
    int swing = -1;               // new swing % if changed
};

class Panel {
public:
    static constexpr int kSlots = 6;
    // Times in panel ticks: process() runs at 6 kHz (every 8th sample).
    static constexpr int kTickHz = 6000;
    static constexpr int kDebounce = 60;        // 10 ms
    static constexpr int kHold = 4800;          // 0.8 s
    static constexpr int kSettle = 1800;        // 0.3 s before a slot loads

    // Call once at start with the current knobs.
    void begin(const int knobs[3], Sw sw, int tempo, int swing, int slot) {
        for (int i = 0; i < 3; ++i) macro_[i].value = knobs[i];
        page_up_ = sw == Sw::Up;
        stable_sw_ = sw == Sw::Down ? Sw::Middle : sw;
        raw_sw_ = stable_sw_;
        current_slot_ = slot;
        if (page_up_) enter_up(knobs, tempo, swing);
    }

    void process(const int knobs[3], Sw sw_in, int tempo, int swing, PanelOut& out) {
        out = PanelOut{};
        ++t_;
        // Debounce the switch.
        if (sw_in != raw_sw_) { raw_sw_ = sw_in; deb_ = kDebounce; }
        else if (deb_ > 0 && --deb_ == 0) set_switch(raw_sw_, knobs, tempo, swing, out);

        if (stable_sw_ == Sw::Down) {
            ++held_;
            if (held_ == kHold) out.stop = true;
        }

        if (page_up_) {
            // Slot select: only after the knob leaves the zone it was in.
            const int zone = slot_zone(knobs[0]);
            if (zone != entry_zone_) {
                entry_zone_ = -1;
                if (zone != candidate_) { candidate_ = zone; settle_ = kSettle; }
                else if (settle_ > 0 && --settle_ == 0 && zone != current_slot_) {
                    out.select_slot = zone;
                    current_slot_ = zone;
                }
            }
            // Knob jitter must not wobble the tempo: move >10 counts first.
            if (tempo_.update(knobs[1]) && std::abs(tempo_.value - bpm_knob_) > 10) {
                bpm_knob_ = tempo_.value;
                const int bpm = kTempoMinK + (tempo_.value * (kTempoMaxK - kTempoMinK) + 2047) / 4095;
                if (bpm != tempo) out.tempo = bpm;
            }
            if (swing_.update(knobs[2]) && std::abs(swing_.value - swing_knob_) > 10) {
                swing_knob_ = swing_.value;
                const int sw = 50 + (swing_.value * 25 + 2047) / 4095;
                if (sw != swing) out.swing = sw;
            }
        } else {
            for (int i = 0; i < 3; ++i) macro_[i].update(knobs[i]);
        }
    }

    // Macro values, -64..63 with a centre dead zone.
    int macro(int i) const {
        const int v = (macro_[i].value >> 5) - 64;
        if (v > -4 && v < 4) return 0;
        return v > 0 ? v - 3 : v + 3;
    }
    bool page_up() const { return page_up_; }
    bool down_held() const { return stable_sw_ == Sw::Down; }
    bool waiting() const {
        if (page_up_) return !tempo_.caught || !swing_.caught;
        return !macro_[0].caught || !macro_[1].caught || !macro_[2].caught;
    }
    // Bit k set = knob k (0 Main, 1 X, 2 Y) waits for soft takeover.
    int waiting_mask() const {
        if (page_up_) return (tempo_.caught ? 0 : 2) | (swing_.caught ? 0 : 4);
        return (macro_[0].caught ? 0 : 1) | (macro_[1].caught ? 0 : 2) | (macro_[2].caught ? 0 : 4);
    }
    int slot() const { return current_slot_; }
    void set_slot(int s) { current_slot_ = s; }

    static int slot_zone(int knob) { return clampi(knob * kSlots / 4096, 0, kSlots - 1); }
    static int tempo_to_knob(int bpm) { return clampi((bpm - kTempoMinK) * 4095 / (kTempoMaxK - kTempoMinK), 0, 4095); }
    static int swing_to_knob(int sw) { return clampi((sw - 50) * 4095 / 25, 0, 4095); }
    static constexpr int kTempoMinK = 40, kTempoMaxK = 240;

private:
    void enter_up(const int knobs[3], int tempo, int swing) {
        page_up_ = true;
        entry_zone_ = slot_zone(knobs[0]);
        candidate_ = entry_zone_;
        tempo_.arm(tempo_to_knob(tempo));
        swing_.arm(swing_to_knob(swing));
        bpm_knob_ = tempo_.value;
        swing_knob_ = swing_.value;
    }

    void set_switch(Sw s, const int knobs[3], int tempo, int swing, PanelOut& out) {
        if (s == stable_sw_) return;
        const Sw prev = stable_sw_;
        stable_sw_ = s;
        if (s == Sw::Down) {
            held_ = 0;
        } else if (prev == Sw::Down) {
            if (held_ < kHold) out.start_or_next = true;
        }
        if (s == Sw::Up && !page_up_) {
            enter_up(knobs, tempo, swing);
        } else if (s == Sw::Middle && page_up_) {
            page_up_ = false;
            for (int i = 0; i < 3; ++i) macro_[i].arm(macro_[i].value);
        }
    }

    Pickup macro_[3];
    Pickup tempo_, swing_;
    bool page_up_ = false;
    Sw raw_sw_ = Sw::Middle, stable_sw_ = Sw::Middle;
    int deb_ = 0;
    int held_ = 0;
    uint32_t t_ = 0;
    int entry_zone_ = -1, candidate_ = -1, settle_ = 0, current_slot_ = 0;
    int bpm_knob_ = 0, swing_knob_ = 0;
};

} // namespace trk
