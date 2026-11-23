// The whole card minus the hardware: sequencer (8 tracks) + 4 drum voices + mixer +
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
    SwapSong,      // swap live <-> spare; a = 1 immediately, 0 at pattern end;
                   // b = slot + 1 the new song belongs to once swapped (0 = keep)
    NextPart,
    SetSlot,       // a = slot number the live song now belongs to
    CancelSwap,    // drop a swap still waiting for the pattern end
    SetSlotMask,   // a = occupied flash slots (bit k = slot k), for the panel's slot knob
    SetMute,       // (0.4.2) a = 0 reverb return, 1 master output; b = muted. Live only, not in the song.
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
    // One LED per sample at most (each PWM write costs ~30 cycles): write
    // led_value to LED led_index when led_index >= 0.
    int8_t led_index = -1;
    uint16_t led_value = 0;
};

// Written by core 0, read by core 1 (single words, no locking needed).
struct EngineStatus {
    volatile uint32_t mode = 0, entry = 0, pattern = 0, row = 0, rep = 0;
    volatile uint32_t slot = 0, swap_count = 0, rows_played = 0;
    volatile uint32_t slot_mask = 0;                  // occupied flash slots (from core 1)
    volatile uint32_t flags = 0;          // bit0 ext clock, bit1 next queued, bit2 swap pending,
                                          // bit3 knob waiting, bit4 select page
    volatile int32_t macro[3] = {0, 0, 0};
    volatile uint32_t cv_note[2] = {0, 0};
    volatile uint32_t gate[2] = {0, 0};
    volatile uint32_t in_gate[2] = {0, 0};            // IN1/IN2 VCA gates (v3)
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
        for (int v = 0; v < kDrumTracks; ++v) {
            voices_[v].seed(0x1234567u * static_cast<uint32_t>(v + 1));
            voices_[v].set_end_threshold(mixer_.end_threshold_ptr(v));
        }
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
#ifdef TRK_SOUND_CHECK
    int32_t stem(int v) const { return stem_[v]; }
    bool voice_active(int v) const { return voices_[v].active(); }
    uint32_t voice_starts(int v) const { return voices_[v].starts_; }
    int32_t stem_[kDrumTracks] = {};
#endif
    const Panel& panel() const { return panel_; }
    // Last computed LED levels (refreshed every 16 samples, written out one per sample).
    const uint16_t* leds() const { return led_; }

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

        // --- one heavy job per sample ---------------------------------------
        // Priority: a Modal voice's resonator setup (runs inside render), a
        // queued drum trigger, the panel scan, then the sequencer's row read
        // or scheduled event. Everything else that can wait skips such a sample.
        // (0.4.0) The reverb's heaviest third (every 3rd sample while it runs)
        // takes the token too: triggers, the panel and the sequencer then wait
        // one sample (21 us). With the reverb stopped nothing changes.
        bool token = setup_mask_ == 0 && !mixer_.reverb_heavy();   // setup: from the last render pass

        // --- panel ----------------------------------------------------------
        // The panel runs at 6 kHz (every 8th sample, or the next free one):
        // plenty for knobs and switch.
        PanelOut po;
        if ((tick_ & 7) == 1) panel_due_ = true;
        if (warmup_ > 0) {
            if (--warmup_ == 0) panel_.begin(in.knobs, in.sw, live_->tempo(), live_->swing, panel_.slot());
            panel_due_ = false;
        } else if (panel_due_ && token && !seq_.drum_queued()) {
            panel_due_ = false;
            token = false;
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

        DrumEvent e;
        const bool trig = token && seq_.pop_drum(e);
        if (trig) { trigger(e, m_decay, m_color); token = false; }
        seq_.set_macros(m_decay, m_color);

        // --- sequencer --------------------------------------------------------
        ClockIn ck;
        ck.ext_connected = in.clock_connected;
        ck.ext_rise = in.clock_rise;
        ck.reset_rise = in.reset_rise;
        seq_.set_clock_ext(in.clock_connected);
        const bool row = seq_.tick(ck, token);
        if (seq_.worked() && seq_.take_in_send_change()) {   // (0.4.1) W effect on an IN trigger (only fire() sets it)
            mixer_.set_send_lock(kDrumTracks, seq_.in_send_lock(0));
            mixer_.set_send_lock(kDrumTracks + 1, seq_.in_send_lock(1));
        }
        if (seq_.take_swapped()) on_swapped();


        // --- audio --------------------------------------------------------------
        // run_mask_: voices sounding or still owing idle() steps (kept here
        // so silent voices cost one bit test, not a call each).
        int32_t ch[kChannels];
        ch[0] = ch[1] = ch[2] = ch[3] = 0;   // (plain stores: an initialiser became a memset call)
        uint32_t live_mask = 0, setup = 0;
        if (run_mask_) {
            uint32_t run = 0;
            for (int v = 0; v < kDrumTracks; ++v) {
                const uint32_t bit = 1u << v;
                if (!(run_mask_ & bit)) continue;
                DrumVoice& dv = voices_[v];
                if (dv.active()) {
                    ch[v] = dv.render();
                    live_mask |= bit;
                    run |= bit;
                    if (dv.setup_pending()) setup |= bit;
                } else if (dv.idle()) {
                    run |= bit;
                }
            }
            run_mask_ = run;
            // (0.4.3) grit on the voices that use it (one test while none does)
            if (__builtin_expect((grit_mask_ & live_mask) != 0, 0)) {
                for (int v = 0; v < kDrumTracks; ++v)
                    if (grit_mask_ & live_mask & (1u << v)) ch[v] = grit_[v].process(ch[v]);
            }
        }
        setup_mask_ = setup;
#ifdef TRK_SOUND_CHECK
        for (int v = 0; v < kDrumTracks; ++v) stem_[v] = ch[v];   // tools/sound_check only
#endif
        ch[4] = static_cast<int32_t>(in.audio[0]) << 4;
        ch[5] = static_cast<int32_t>(in.audio[1]) << 4;
        if (in.audio_connected[0]) live_mask |= 1u << 4;
        if (in.audio_connected[1]) live_mask |= 1u << 5;
        // Deferrable work skips the samples that start a voice or a note (the
        // heaviest jobs), which keeps the worst-case sample flat.
        // Input VCA control about every 4th sample (~100 us, far below the
        // 1.3 ms attack).
        if (vca_div_ > 0) --vca_div_;
        // (never starved: after 32 busy samples in a row it runs anyway)
        bool busy = !token || seq_.worked();
        if (busy && ++busy_run_ >= 32) busy = false;
        if (!busy) busy_run_ = 0;
        if (vca_div_ == 0 && !busy) {
            vca_div_ = 4;
            for (int i = 0; i < kInputs; ++i) {
                const VcaSrc src = mixer_.source(i);
                if (src == VcaCvGate) mixer_.set_vca(i, seq_.gate(i), 0, kNoLock);   // full level (0.3.2)
                else if (src == VcaInTrack) mixer_.set_vca(i, seq_.in_gate(i), seq_.in_vel(i), seq_.in_decay_lock(i));
            }
        }
        // Mixer coefficients: one channel checked every 4th quiet sample (a
        // change is picked up within ~0.6 ms, the check costs ~120 cycles).
        const bool light = !busy;   // (reverb-heavy samples have no token, so they are busy)
        if (light && (tick_ & 3) == 2) {
            mixer_.set_macros(m_decay, m_color);
            mixer_.update_step(*live_, m_filter);
        }
        mixer_.process(ch, live_mask, out.audio[0], out.audio[1]);

        for (int i = 0; i < kCvTracks; ++i) {
            out.cv_mv[i] = seq_.cv_mv(i);
            out.gate[i] = seq_.gate(i);
        }

        // --- LEDs + status --------------------------------------------------------
        if (row) row_flash_ = seq_.pos().row % 4 == 0 ? 150 : 0;   // 50 ms in LED ticks
        // LED levels every 16 samples, status every 64, each on the next
        // sample without a heavy job; the LEDs are then written one per sample.
        out.led_index = -1;
        if ((tick_ & 15) == 3) led_due_ = true;
        if ((tick_ & 63) == 4) publish_due_ = true;
        ++tick_;
        if (led_due_ && light) {
            led_due_ = false;
            update_leds();
            led_next_ = 0;
        } else if (publish_due_ && light) {
            publish_due_ = false;
            publish(m_decay, m_color, m_filter, in);
        } else if (led_next_ < 6) {
            out.led_index = static_cast<int8_t>(led_next_);
            out.led_value = led_[led_next_];
            ++led_next_;
        }
    }

private:
    void handle(const Command& c) {
        switch (c.type) {
        case CmdType::PlaySong: seq_.play_song(c.a); break;
        case CmdType::PlayPattern: seq_.play_pattern(c.a); break;
        case CmdType::Stop: seq_.stop(); break;
        case CmdType::SetMute:
            if (c.a == 0) mixer_.set_reverb_mute(c.b != 0);
            else mixer_.set_master_mute(c.b != 0);
            break;
        case CmdType::CueEntry:
            if (seq_.mode() == PlayMode::Pattern) seq_.cue_pattern(c.a);
            else seq_.cue_entry(c.a);
            break;
        case CmdType::Preview:
            seq_.preview(c.a, c.b, c.c);
            break;
        case CmdType::SwapSong:
            swap_slot_ = c.b;
            seq_.request_song(spare_, c.a != 0);
            pending_swap_ = true;
            if (seq_.take_swapped()) on_swapped();
            break;
        case CmdType::CancelSwap:
            if (seq_.cancel_pending()) {
                pending_swap_ = false;
                status_.swap_count = status_.swap_count + 1;   // counts as done for core 1
                swap_slot_ = 0;
            }
            break;
        case CmdType::NextPart: seq_.queue_next(); break;
        case CmdType::SetSlot:
            panel_.set_slot(c.a);
            status_.slot = c.a;
            break;
        case CmdType::SetSlotMask:
            panel_.set_slot_mask(c.a);
            status_.slot_mask = c.a;
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
        mixer_.clear_send_locks();
        if (swap_slot_ > 0) {
            status_.slot = static_cast<uint32_t>(swap_slot_ - 1);
            panel_.set_slot(swap_slot_ - 1);
        }
        swap_slot_ = 0;
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
        if (e.decay != kNoLock) decay = e.decay;
        if (e.a != kNoLock) pa = e.a;
        if (e.b != kNoLock) pb = e.b;
        // Macros are offsets on top of the song value (never stored), and only
        // for the voices routed to them in the song header.
        if (macro_on(*live_, MacroDecay, e.track)) decay += m_decay;
        if (macro_on(*live_, MacroColour, e.track)) pa += m_color;
        a.decay = static_cast<uint8_t>(clampi(decay, 0, 127));
        a.a = static_cast<uint8_t>(clampi(pa, 0, 127));
        a.b = static_cast<uint8_t>(pb);
        a.x = p[DpX];   // (0.4.3) kick click / grit
        v.trigger(a);
        if (v.grit()) {
            if (grit_v_[e.track] != v.grit()) { grit_v_[e.track] = v.grit(); grit_[e.track].set(v.grit()); }
            grit_mask_ |= 1u << e.track;
        } else grit_mask_ &= ~(1u << e.track);
        mixer_.set_send_lock(e.track, e.send);   // (0.4.1) W effect, or back to the knob
        run_mask_ |= 1u << e.track;
        hit_[e.track] = 4095;
        status_.trig_count[e.track] = status_.trig_count[e.track] + 1;
    }

    void update_leds() {
        uint16_t* out_led = led_;
        const uint32_t t = tick_;
        if (busy_) {
            for (int i = 0; i < 6; ++i) out_led[i] = static_cast<uint16_t>(((t >> 10) + static_cast<uint32_t>(i)) % 6 == 0 ? 4095 : 200);
            return;
        }
        if (panel_.page_up()) {
            // One LED per slot (reading order 0 1 / 2 3 / 4 5): occupied slots
            // glow dimly, the playing slot is bright (blinks while a knob waits
            // for pickup), a chosen slot waiting for the pattern end flashes.
            const bool blink = panel_.waiting() && ((t >> 12) & 1);
            const int list = panel_.slot_list();
            const int playing = static_cast<int>(status_.slot);
            for (int i = 0; i < 6; ++i) out_led[i] = (list >> i) & 1 ? 250 : 0;
            if (playing >= 0 && playing < 6) out_led[playing] = blink ? 250 : 4095;
            if (seq_.swap_pending() && panel_.slot() != playing) out_led[panel_.slot()] = ((t >> 11) & 1) ? 4095 : 0;
            return;
        }
        if (row_flash_ > 0) row_flash_ -= 1;
        // LED0: beat (or 6 Hz blink while a knob waits for pickup).
        out_led[0] = panel_.waiting() ? (((t >> 12) & 1) ? 4095 : 0) : static_cast<uint16_t>(row_flash_ > 0 ? 4095 : 0);
        // LED1: playing; fast blink while a part change is queued.
        if (seq_.next_queued() || seq_.swap_pending()) out_led[1] = ((t >> 11) & 1) ? 4095 : 0;
        // (stopped: a dim glow while the reverb tail still rings, 0.4.0)
        else out_led[1] = seq_.playing() ? 1500 : (mixer_.reverb_running() ? 200 : 0);
        if (panel_.down_held()) out_led[1] = 4095;
        // LED2-5: drum hits.
        for (int v = 0; v < kDrumTracks; ++v) {
            out_led[2 + v] = static_cast<uint16_t>(hit_[v]);
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
                        (panel_.waiting() ? 8u : 0u) | (panel_.page_up() ? 16u : 0u) |
                        (mixer_.reverb_muted() ? 32u : 0u) | (mixer_.master_muted() ? 64u : 0u);
        status_.macro[0] = md;
        status_.macro[1] = mc;
        status_.macro[2] = mf;
        for (int i = 0; i < kCvTracks; ++i) {
            status_.cv_note[i] = seq_.cv_note(i);
            status_.gate[i] = seq_.gate(i) ? 1u : 0u;
            status_.in_gate[i] = seq_.in_gate(i) ? 1u : 0u;
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
    uint8_t swap_slot_ = 0;
    int vca_div_ = 0;
    int busy_run_ = 0;
    volatile bool busy_ = false;
    int warmup_ = 0;
    int transpose_ = 0;
    uint32_t tick_ = 0;
    uint32_t run_mask_ = 0, setup_mask_ = 0;
    uint32_t grit_mask_ = 0;   // (0.4.3) voices whose last hit has grit on
    GritState grit_[kDrumTracks];
    uint8_t grit_v_[kDrumTracks] = {};
    uint16_t led_[6] = {};
    int led_next_ = 6;
    bool led_due_ = false, publish_due_ = false, panel_due_ = false;
    int32_t row_flash_ = 0;
    int32_t hit_[kDrumTracks] = {};
};

} // namespace trk
