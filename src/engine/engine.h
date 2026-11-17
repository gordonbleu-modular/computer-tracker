// The whole card minus the hardware: sequencer + 4 drum voices + mixer +
// panel. Runs one sample per process() call on core 0. Core 1 (USB / flash)
// talks to it through a lock-free command queue, the live/spare song
// pointers and a status block. Host-testable.
// SPDX-License-Identifier: MIT
#pragma once

#include "dsp.h"
#include "mixer.h"
#include "panel.h"
#include "sequencer.h"
#include "song.h"
#include "voices.h"

#include <atomic>

namespace trk {

// Single-producer / single-consumer ring (core 1 -> core 0).
template <typename T, int N>
class SpscQueue {
public:
    bool push(const T& v) {
        const uint32_t h = head_.load(std::memory_order_relaxed);
        const uint32_t n = (h + 1) % N;
        if (n == tail_.load(std::memory_order_acquire)) return false;
        buf_[h] = v;
        head_.store(n, std::memory_order_release);
        return true;
    }
    bool pop(T& v) {
        const uint32_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        v = buf_[t];
        tail_.store((t + 1) % N, std::memory_order_release);
        return true;
    }
private:
    std::atomic<uint32_t> head_{0}, tail_{0};
    T buf_[N];
};

enum class CmdType : uint8_t {
    PlaySong,      // a = entry
    PlayPattern,   // a = pattern
    Stop,
    CueEntry,      // a = entry (song mode) / cue pattern (pattern mode)
    Preview,       // a = track, b = note, c = vel
    SwapSong,      // swap live <-> spare; a = 1 immediately, 0 at pattern end
    NextPart,
    SetSlot,       // a = slot number the live song now belongs to
};

struct Command {
    CmdType type;
    uint8_t a, b, c;
};

struct EngineIn {
    int16_t audio[2] = {0, 0};
    int16_t cv[2] = {0, 0};
    bool cv_connected[2] = {false, false};
    bool audio_connected[2] = {false, false};
    bool clock_connected = false;
    bool clock_rise = false;
    bool reset_rise = false;
    int knobs[3] = {2048, 2048, 2048};
    Sw sw = Sw::Middle;
};

struct EngineOut {
    int16_t audio[2] = {0, 0};
    int32_t cv_mv[2] = {0, 0};
    bool gate[2] = {false, false};
    uint16_t led[6] = {};
    bool leds_valid = false;   // LEDs are refreshed every 16th sample
};

// Written by core 0, read by core 1 (single words, no locking needed).
struct EngineStatus {
    volatile uint32_t mode = 0, entry = 0, pattern = 0, row = 0, rep = 0;
    volatile uint32_t slot = 0, swap_count = 0, rows_played = 0;
    volatile uint32_t flags = 0;          // bit0 ext clock, bit1 next queued, bit2 swap pending,
                                          // bit3 knob waiting, bit4 select page
    volatile int32_t macro[3] = {0, 0, 0};
    volatile uint32_t cv_note[2] = {0, 0};
    volatile uint32_t gate[2] = {0, 0};
    volatile uint32_t trig_count[kDrumTracks] = {};
    // Panel state for the editor's knob feedback (protocol v2).
    volatile int32_t knob[3] = {2048, 2048, 2048};   // raw 0..4095
    volatile uint32_t knob_waiting = 0;               // bit per knob: waiting for pickup
    volatile uint32_t down_held = 0;
    volatile int32_t transpose = 0;                   // CV In 1, semitones
};

class TrackerEngine {
public:
    void init(Song* a, Song* b, int slot, const EngineIn& in) {
        init_tables();
        live_ = a;
        spare_ = b;
        seq_.attach(live_);
        mixer_.update(*live_, 0);
        for (int v = 0; v < kDrumTracks; ++v) voices_[v].seed(0x1234567u * static_cast<uint32_t>(v + 1));
        panel_.begin(in.knobs, in.sw, live_->tempo(), live_->swing, slot);
        status_.slot = static_cast<uint32_t>(slot);
        warmup_ = 0;
    }

    // On hardware the knob filters need ~100 ms to settle after power-up;
    // the panel starts (and reads its initial knob positions) after that.
    void set_warmup(int samples) { warmup_ = samples; }
    // Core 1 sets this while it writes flash (all LEDs glow).
    void set_busy(bool b) { busy_ = b; }

    Song* live() const { return live_; }
    Song* spare() const { return spare_; }
    bool swap_idle() const { return !pending_swap_; }
    EngineStatus& status() { return status_; }
    SpscQueue<Command, 32>& commands() { return cmds_; }
    Sequencer& seq() { return seq_; }
    Mixer& mixer() { return mixer_; }
    const Panel& panel() const { return panel_; }

    // Slot the panel asked for (core 1 loads it into spare, then sends SwapSong).
    int take_slot_request() {
        const int s = slot_request_;
        slot_request_ = -1;
        return s;
    }

    void process(const EngineIn& in, EngineOut& out) {
        // --- commands from core 1 -------------------------------------------
        Command c;
        if (cmds_.pop(c)) handle(c);

        // --- panel ----------------------------------------------------------
        // The panel runs at 6 kHz (every 8th sample): plenty for knobs and switch.
        PanelOut po;
        if (warmup_ > 0) {
            if (--warmup_ == 0) panel_.begin(in.knobs, in.sw, live_->tempo(), live_->swing, panel_.slot());
        } else if ((tick_ & 7) == 1) {
            panel_.process(in.knobs, in.sw, live_->tempo(), live_->swing, po);
        }
        if (po.tempo > 0) live_->set_tempo(po.tempo);
        if (po.swing > 0) live_->swing = static_cast<uint8_t>(po.swing);
        if (po.select_slot >= 0) slot_request_ = po.select_slot;
        if (po.stop) seq_.stop();
        if (po.start_or_next) {
            if (seq_.playing()) seq_.queue_next();
            else seq_.play_song(seq_.pos().entry);
        }
        const int m_decay = panel_.macro(0);
        const int m_color = panel_.macro(1);
        int m_filter = panel_.macro(2);
        if (in.cv_connected[1]) m_filter = clampi(m_filter + (in.cv[1] >> 5), -64, 63);

        // CV In 1: transpose the CV tracks in semitones (with hysteresis).
        if (in.cv_connected[0]) {
            const int32_t q = in.cv[0] * 9;                 // semitones * 256 (2048 = 6 V = 72 st)
            const int32_t d = q - transpose_ * 256;
            if (d > 160 || d < -160) transpose_ = (q + (q >= 0 ? 128 : -128)) / 256;
        } else {
            transpose_ = 0;
        }
        seq_.set_transpose(transpose_);

        // --- drum triggers queued on the previous sample --------------------
        // At most one per sample keeps the worst case flat.
        DrumEvent e;
        if (seq_.pop_drum(e)) trigger(e, m_decay, m_color);

        // --- sequencer --------------------------------------------------------
        ClockIn ck;
        ck.ext_connected = in.clock_connected;
        ck.ext_rise = in.clock_rise;
        ck.reset_rise = in.reset_rise;
        seq_.set_clock_ext(in.clock_connected);
        const bool row = seq_.tick(ck);
        if (seq_.take_swapped()) on_swapped();


        // --- audio --------------------------------------------------------------
        int32_t ch[kChannels];
        uint32_t live_mask = 0;
        for (int v = 0; v < kDrumTracks; ++v) {
            if (voices_[v].active()) { ch[v] = voices_[v].render(); live_mask |= 1u << v; }
            else ch[v] = 0;
        }
        ch[4] = static_cast<int32_t>(in.audio[0]) << 4;
        ch[5] = static_cast<int32_t>(in.audio[1]) << 4;
        if (in.audio_connected[0]) live_mask |= 1u << 4;
        if (in.audio_connected[1]) live_mask |= 1u << 5;
        mixer_.set_gate(0, seq_.gate(0));
        mixer_.set_gate(1, seq_.gate(1));
        mixer_.update_step(*live_, m_filter);
        mixer_.process(ch, live_mask, out.audio[0], out.audio[1]);

        for (int i = 0; i < kCvTracks; ++i) {
            out.cv_mv[i] = seq_.cv_mv(i);
            out.gate[i] = seq_.gate(i);
        }

        // --- LEDs + status --------------------------------------------------------
        if (row) row_flash_ = seq_.pos().row % 4 == 0 ? 150 : 0;   // 50 ms in LED ticks
        out.leds_valid = (tick_ & 15) == 3;   // staggered against panel and status work
        if (out.leds_valid) update_leds(out);
        if ((++tick_ & 63) == 5) publish(m_decay, m_color, m_filter, in);
    }

private:
    void handle(const Command& c) {
        switch (c.type) {
        case CmdType::PlaySong: seq_.play_song(c.a); break;
        case CmdType::PlayPattern: seq_.play_pattern(c.a); break;
        case CmdType::Stop: seq_.stop(); break;
        case CmdType::CueEntry:
            if (seq_.mode() == PlayMode::Pattern) seq_.cue_pattern(c.a);
            else seq_.cue_entry(c.a);
            break;
        case CmdType::Preview:
            seq_.preview(c.a, c.b, c.c);
            break;
        case CmdType::SwapSong:
            seq_.request_song(spare_, c.a != 0);
            pending_swap_ = true;
            if (seq_.take_swapped()) on_swapped();
            break;
        case CmdType::NextPart: seq_.queue_next(); break;
        case CmdType::SetSlot:
            panel_.set_slot(c.a);
            status_.slot = c.a;
            break;
        }
    }

    void on_swapped() {
        if (!pending_swap_) return;
        pending_swap_ = false;
        Song* old = live_;
        live_ = spare_;
        spare_ = old;
        mixer_.update(*live_, 0);
        status_.swap_count = status_.swap_count + 1;
    }

    void trigger(const DrumEvent& e, int m_decay, int m_color) {
        DrumVoice& v = voices_[e.track];
        if (e.note == kNoteOff) { v.choke(); return; }
        const uint8_t* p = live_->drum[e.track];
        TriggerArgs a;
        a.engine = p[DpEngine];
        a.note = static_cast<uint8_t>(clampi(e.note + p[DpTune] - 64, 0, 127));
        const int sens = p[DpLevelVel];
        a.vel = static_cast<uint8_t>(clampi(127 - (((127 - e.vel) * sens) >> 7), 1, 127));
        int decay = p[DpDecay], pa = p[DpA], pb = p[DpB];
        if (e.fx == FxDecay) decay = e.fxv;
        if (e.fx == FxParamA) pa = e.fxv;
        if (e.fx == FxParamB) pb = e.fxv;
        // Macros are offsets on top of the song value (never stored), and only
        // for the voices routed to them in the song header.
        if (macro_on(*live_, MacroDecay, e.track)) decay += m_decay;
        if (macro_on(*live_, MacroColour, e.track)) pa += m_color;
        a.decay = static_cast<uint8_t>(clampi(decay, 0, 127));
        a.a = static_cast<uint8_t>(clampi(pa, 0, 127));
        a.b = static_cast<uint8_t>(pb);
        v.trigger(a);
        hit_[e.track] = 4095;
        status_.trig_count[e.track] = status_.trig_count[e.track] + 1;
    }

    void update_leds(EngineOut& out) {
        const uint32_t t = tick_;
        if (busy_) {
            for (int i = 0; i < 6; ++i) out.led[i] = static_cast<uint16_t>(((t >> 10) + static_cast<uint32_t>(i)) % 6 == 0 ? 4095 : 200);
            return;
        }
        if (panel_.page_up()) {
            // Slot as one LED (reading order 0 1 / 2 3 / 4 5); blink if a knob waits.
            const bool blink = panel_.waiting() && ((t >> 12) & 1);
            for (int i = 0; i < 6; ++i) out.led[i] = (i == panel_.slot() && !blink) ? 4095 : 0;
            if (seq_.swap_pending()) out.led[panel_.slot()] = ((t >> 11) & 1) ? 4095 : 300;
            return;
        }
        if (row_flash_ > 0) row_flash_ -= 1;
        // LED0: beat (or 6 Hz blink while a knob waits for pickup).
        out.led[0] = panel_.waiting() ? (((t >> 12) & 1) ? 4095 : 0) : static_cast<uint16_t>(row_flash_ > 0 ? 4095 : 0);
        // LED1: playing; fast blink while a part change is queued.
        if (seq_.next_queued() || seq_.swap_pending()) out.led[1] = ((t >> 11) & 1) ? 4095 : 0;
        else out.led[1] = seq_.playing() ? 1500 : 0;
        if (panel_.down_held()) out.led[1] = 4095;
        // LED2-5: drum hits.
        for (int v = 0; v < kDrumTracks; ++v) {
            out.led[2 + v] = static_cast<uint16_t>(hit_[v]);
            if (hit_[v] > 0) hit_[v] = hit_[v] > 64 ? hit_[v] - 16 - (hit_[v] >> 6) : 0;
        }
    }

    void publish(int md, int mc, int mf, const EngineIn& in) {
        const bool ext = in.clock_connected;
        for (int k = 0; k < 3; ++k) status_.knob[k] = in.knobs[k];
        status_.knob_waiting = static_cast<uint32_t>(panel_.waiting_mask());
        status_.down_held = panel_.down_held() ? 1u : 0u;
        status_.transpose = transpose_;
        const Position& p = seq_.pos();
        status_.mode = static_cast<uint32_t>(seq_.mode());
        status_.entry = p.entry;
        status_.pattern = p.pattern;
        status_.row = p.row;
        status_.rep = p.rep;
        status_.rows_played = seq_.rows_played();
        status_.flags = (ext ? 1u : 0u) | (seq_.next_queued() ? 2u : 0u) | (seq_.swap_pending() ? 4u : 0u) |
                        (panel_.waiting() ? 8u : 0u) | (panel_.page_up() ? 16u : 0u);
        status_.macro[0] = md;
        status_.macro[1] = mc;
        status_.macro[2] = mf;
        for (int i = 0; i < kCvTracks; ++i) {
            status_.cv_note[i] = seq_.cv_note(i);
            status_.gate[i] = seq_.gate(i) ? 1u : 0u;
        }
    }

    Song* volatile live_ = nullptr;     // read by core 1 between samples
    Song* volatile spare_ = nullptr;
    Sequencer seq_;
    DrumVoice voices_[kDrumTracks];
    Mixer mixer_;
    Panel panel_;
    SpscQueue<Command, 32> cmds_;
    EngineStatus status_;
    volatile int slot_request_ = -1;
    volatile bool pending_swap_ = false;
    volatile bool busy_ = false;
    int warmup_ = 0;
    int transpose_ = 0;
    uint32_t tick_ = 0;
    int32_t row_flash_ = 0;
    int32_t hit_[kDrumTracks] = {};
};

} // namespace trk
