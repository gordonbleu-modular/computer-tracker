// Host tests for the tracker engine, protocol and flash format.
//   g++ -std=c++17 -O2 -Wall -Wextra -Werror -I src tests/test_engine.cpp -o build-host/test_engine
// SPDX-License-Identifier: MIT
#include "engine/engine.h"
#include "engine/flashstore.h"
#include "engine/protocol.h"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace trk;

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::unique_ptr<Song> make_song(bool demo) {
    auto s = std::make_unique<Song>();
    if (demo) song_demo(*s); else song_clear(*s);
    return s;
}

// ---------------------------------------------------------------- DSP ------
static void test_dsp() {
    init_tables();
    CHECK(softclip(0) == 0);
    CHECK(softclip(49152) >= 32766 && softclip(49152) <= 32769);
    CHECK(softclip(-49152) <= -32767);
    CHECK(softclip(100000) == softclip(49152));
    CHECK(g_tables.level[127] == 4096);
    CHECK(g_tables.level[0] == 0);
    CHECK(g_tables.note_inc[69] > 0);
    // 440 Hz: inc = 440/48000 * 2^32
    const double want = 440.0 / 48000.0 * 4294967296.0;
    CHECK(std::abs(static_cast<double>(g_tables.note_inc[69]) - want) < want * 0.001);
    CHECK(g_tables.ks_period[33] < (1020u << 8) + 1);
}

struct VoiceStats { int32_t peak = 0; double e_first = 0, e_last = 0; };

static VoiceStats run_voice(uint8_t engine, uint8_t note, uint8_t decay, uint8_t a, uint8_t b) {
    DrumVoice v;
    TriggerArgs t{engine, note, 127, decay, a, b};
    v.trigger(t);
    VoiceStats s;
    const int n = kSampleRate;   // one second
    for (int i = 0; i < n; ++i) {
        const int32_t y = v.render();
        const int32_t ay = y < 0 ? -y : y;
        if (ay > s.peak) s.peak = ay;
        if (i < 4800) s.e_first += static_cast<double>(y) * y;
        if (i >= n - 4800) s.e_last += static_cast<double>(y) * y;
    }
    return s;
}

static void test_voices() {
    const char* names[] = {"kick", "pluck", "noise", "metal", "modal", "snare"};
    for (int e = 0; e < EngCount; ++e) {
        for (int a : {0, 64, 127}) {
            for (int b : {0, 64, 127}) {
                for (int note : {24, 36, 60, 96}) {
                    const VoiceStats s = run_voice(static_cast<uint8_t>(e), static_cast<uint8_t>(note), 50,
                                                   static_cast<uint8_t>(a), static_cast<uint8_t>(b));
                    CHECK(s.peak <= 49152);
                    CHECK(s.e_first > 0);
                    if (!(s.e_last < s.e_first)) std::printf("  no decay: %s a=%d b=%d n=%d\n", names[e], a, b, note);
                    CHECK(s.e_last < s.e_first);
                }
            }
        }
        const VoiceStats mid = run_voice(static_cast<uint8_t>(e), 48, 60, 64, 32);
        const double rms = std::sqrt(mid.e_first / 4800.0);
        std::printf("  %-6s peak %6d  rms(0-100ms) %7.0f\n", names[e], mid.peak, rms);
        CHECK(mid.peak > 3000);
    }
    // A long decay must still be ringing after 1 s, a short one must be silent.
    CHECK(run_voice(EngModal, 60, 120, 32, 64).e_last > 0);
    CHECK(run_voice(EngKick, 36, 5, 0, 0).e_last == 0);
}

// Fundamental by autocorrelation over 0.1..0.3 s, in Hz.
static double pitch_of(uint8_t engine, uint8_t note, uint8_t a, uint8_t b) {
    DrumVoice v;
    v.trigger(TriggerArgs{engine, note, 127, 100, a, b});
    std::vector<double> x;
    for (int i = 0; i < 14400; ++i) { const double y = v.render(); if (i >= 4800) x.push_back(y); }
    int best = 0;
    double bestv = -1e300;
    for (int lag = 40; lag < 1200; ++lag) {
        double acc = 0;
        for (size_t i = 0; i + static_cast<size_t>(lag) < x.size(); ++i) acc += x[i] * x[i + static_cast<size_t>(lag)];
        acc /= static_cast<double>(x.size() - static_cast<size_t>(lag));
        if (acc > bestv) { bestv = acc; best = lag; }
    }
    return 48000.0 / best;
}

static void test_pitch() {
    // A3 = 220 Hz on the pitched engines.
    const double kick = pitch_of(EngKick, 57, 0, 0);
    const double pluck = pitch_of(EngPluck, 57, 64, 64);
    const double modal = pitch_of(EngModal, 57, 64, 32);   // marimba-ish: fundamental dominates
    std::printf("  pitch @A3: kick %.1f pluck %.1f modal %.1f Hz\n", kick, pluck, modal);
    CHECK(std::abs(kick - 220) < 4);
    CHECK(std::abs(pluck - 220) < 4);
    CHECK(std::abs(modal - 220) < 6);
}

// ----------------------------------------------------------- sequencer -----
static void test_sequencer_timing() {
    init_tables();
    auto s = make_song(false);
    s->set_tempo(120);
    s->swing = 50;
    set_cell(*s, 0, 0, 0, 36);
    Sequencer q;
    q.attach(s.get());
    q.play_pattern(0);
    ClockIn ck;
    std::vector<int> rows;
    for (int i = 0; i < 48000; ++i) if (q.tick(ck)) rows.push_back(i);
    // 120 BPM, 4 rows per beat: 8 rows per second, 6000 samples apart.
    CHECK(rows.size() == 8);
    if (rows.size() >= 3) {
        CHECK(rows[0] == 0);
        CHECK(std::abs(rows[1] - 6000) <= 1);
        CHECK(std::abs(rows[2] - 12000) <= 1);
    }
    // Swing 75 %: odd rows start 3/4 into the pair.
    s->swing = 75;
    q.play_pattern(0);
    rows.clear();
    for (int i = 0; i < 24000; ++i) if (q.tick(ck)) rows.push_back(i);
    CHECK(rows.size() == 4);
    if (rows.size() >= 3) {
        CHECK(std::abs(rows[1] - 9000) <= 1);
        CHECK(std::abs(rows[2] - 12000) <= 1);
    }
}

static void test_sequencer_song() {
    auto s = make_song(false);
    s->song_len = 3;
    s->entries[0] = {2, 2};
    s->entries[1] = {5, 1};
    s->entries[2] = {7, 1};
    s->pat_len[2] = 4; s->pat_len[5] = 2; s->pat_len[7] = 3;
    Sequencer q;
    q.attach(s.get());
    q.play_song(0);
    ClockIn ck;
    std::vector<int> pats;
    for (int i = 0; i < 6000 * 40; ++i) if (q.tick(ck)) pats.push_back(q.pos().pattern);
    // 2,2,2,2 2,2,2,2 5,5 7,7,7 then loop
    const int want[] = {2, 2, 2, 2, 2, 2, 2, 2, 5, 5, 7, 7, 7, 2, 2};
    bool ok = pats.size() >= 15;
    for (int i = 0; ok && i < 15; ++i) ok = pats[static_cast<size_t>(i)] == want[i];
    CHECK(ok);

    // Next part from the panel: leave entry 0 after the current pattern.
    q.play_song(0);
    pats.clear();
    int rowsSeen = 0;
    for (int i = 0; i < 6000 * 8; ++i) {
        if (q.tick(ck)) {
            pats.push_back(q.pos().pattern);
            if (++rowsSeen == 1) q.queue_next();
        }
    }
    CHECK(pats.size() >= 6 && pats[4] == 5);

    // External clock + reset.
    q.stop();
    q.play_song(0);
    ClockIn ext;
    ext.ext_connected = true;
    q.set_clock_ext(true);
    int fired = 0;
    for (int i = 0; i < 10000; ++i) {
        ext.ext_rise = (i % 1000) == 10;
        ext.reset_rise = false;
        if (q.tick(ext)) ++fired;
    }
    CHECK(fired == 10);
    CHECK(q.row_len() == 1000);
    ext.ext_rise = false; ext.reset_rise = true;
    q.tick(ext);
    ext.reset_rise = false; ext.ext_rise = true;
    q.tick(ext);
    CHECK(q.pos().row == 0 && q.pos().entry == 0);
}

static void test_cv_tracks() {
    auto s = make_song(false);
    s->set_tempo(120);
    s->cv[0][CpGate] = 8;
    set_cell(*s, 0, 4, 0, 12);          // C0 -> 0 V
    set_cell(*s, 0, 4, 1, 24);          // C1 -> 1 V
    set_cell(*s, 0, 4, 2, 36, 0, FxSlide, 30);
    set_cell(*s, 0, 4, 4, 6);           // below C0 (raw) -> folded up an octave: F#0 = 0.5 V
    set_cell(*s, 0, 5, 0, 24, 0, FxGate, 0);   // tie
    set_cell(*s, 0, 5, 3, kNoteOff);
    Sequencer q;
    q.attach(s.get());
    q.play_pattern(0);
    ClockIn ck;
    // Tracks of a row are read every other sample: track 4 (CV 1) at +8.
    for (int i = 0; i < 10; ++i) q.tick(ck);
    CHECK(q.cv_mv(0) == 0);
    CHECK(q.gate(0));
    for (int i = 10; i < 3100; ++i) q.tick(ck);
    CHECK(!q.gate(0));                  // half-row gate fell at 3000
    CHECK(q.gate(1));                   // tied
    for (int i = 3100; i < 6010; ++i) q.tick(ck);
    CHECK(std::abs(q.cv_mv(0) - 1000) <= 2);
    // Row 1 retriggered: gate low for the 2 ms gap, then high.
    int gate_high_after = -1;
    // advance to row 2 (slide): gate stays high (legato) during the glide.
    for (int i = 6010; i < 12000 + 100; ++i) { q.tick(ck); if (i == 12050 && q.gate(0)) gate_high_after = i; }
    CHECK(gate_high_after == 12050);    // row 1 gate closed before row 2: the slide jumps and opens the gate
    CHECK(std::abs(q.cv_mv(0) - 2000) <= 2);
    for (int i = 0; i < 6000 * 2; ++i) q.tick(ck);
    CHECK(std::abs(q.cv_mv(0) - 500) <= 2);   // never negative: folded by an octave
    CHECK(!q.gate(1));                  // OFF at row 3
    // Transposition (CV In 1) folds by octaves too: never below 0 V, never above 6 V.
    q.set_transpose(-17);               // F#0 - 17 = C#-1 -> C#0
    for (int i = 0; i < 8; ++i) q.tick(ck);
    CHECK(std::abs(q.cv_mv(0) - 83) <= 2);
    q.set_transpose(66);                // F#0 + 66 = C6 -> C5 (5 V)
    for (int i = 0; i < 8; ++i) q.tick(ck);
    CHECK(std::abs(q.cv_mv(0) - 5000) <= 2);
    q.set_transpose(0);

    // Slide while the gate is held: glides, gate stays high.
    auto s2 = make_song(false);
    s2->cv[0][CpGate] = 0;              // tie
    set_cell(*s2, 0, 4, 0, 24);
    set_cell(*s2, 0, 4, 1, 36, 0, FxSlide, 40);
    q.attach(s2.get());
    q.play_pattern(0);
    for (int i = 0; i < 6000 + 200; ++i) q.tick(ck);
    CHECK(q.gate(0));
    CHECK(q.cv_mv(0) > 1000 && q.cv_mv(0) < 2000);
    for (int i = 0; i < 20000; ++i) q.tick(ck);
    CHECK(std::abs(q.cv_mv(0) - 2000) <= 2);

    // Retrigger gap between two tied notes.
    auto s3 = make_song(false);
    s3->cv[0][CpGate] = 0;
    set_cell(*s3, 0, 4, 0, 48);
    set_cell(*s3, 0, 4, 1, 50);
    q.attach(s3.get());
    q.play_pattern(0);
    for (int i = 0; i < 6000; ++i) q.tick(ck);
    for (int i = 0; i < 10; ++i) q.tick(ck);  // row 1 starts, track 4 read
    CHECK(!q.gate(0));
    for (int i = 0; i < 100; ++i) q.tick(ck);
    CHECK(q.gate(0));
}

static void test_drum_fx() {
    auto s = make_song(false);
    set_cell(*s, 0, 0, 0, 36, 0, FxRatchet, 4);
    set_cell(*s, 0, 1, 1, 50, 0, FxProb, 0);     // never
    set_cell(*s, 0, 2, 2, 60, 0, FxDelay, 8);    // half a row late
    Sequencer q;
    q.attach(s.get());
    q.play_pattern(0);
    ClockIn ck;
    int hits[kDrumTracks] = {};
    int delay_at = -1;
    for (int i = 0; i < 6000 * 3; ++i) {
        q.tick(ck);
        DrumEvent e;
        while (q.pop_drum(e)) { ++hits[e.track]; if (e.track == 2) delay_at = i; }
    }
    CHECK(hits[0] == 4);
    CHECK(hits[1] == 0);
    CHECK(hits[2] == 1);
    CHECK(std::abs(delay_at - (12000 + 2 + 3000)) <= 2);   // track 2 is read 2 samples into the row (0.4.0: one track per free sample)
}

// --------------------------------------------------------------- panel -----
static void test_panel() {
    Panel p;
    int k[3] = {2048, 2048, 2048};
    p.begin(k, Sw::Middle, 120, 50, 0);
    p.set_slot_mask(0x3F);   // all six slots hold songs
    PanelOut o;
    // Tap down (> debounce, < hold).
    bool tap = false, stop = false;
    for (int i = 0; i < 600; ++i) { p.process(k, Sw::Down, 120, 50, o); stop |= o.stop; }
    for (int i = 0; i < 200; ++i) { p.process(k, Sw::Middle, 120, 50, o); tap |= o.start_or_next; }
    CHECK(tap && !stop);
    // Hold.
    tap = stop = false;
    for (int i = 0; i < 6000; ++i) { p.process(k, Sw::Down, 120, 50, o); stop |= o.stop; }
    for (int i = 0; i < 200; ++i) { p.process(k, Sw::Middle, 120, 50, o); tap |= o.start_or_next; }
    CHECK(stop && !tap);
    // Macros follow the knobs at start.
    CHECK(p.macro(0) == 0);
    k[0] = 4095;
    p.process(k, Sw::Middle, 120, 50, o);
    CHECK(p.macro(0) > 50);
    // SELECT page: tempo knob must pick up before it changes the tempo.
    k[1] = 0;   // far from 120 BPM
    int tempo = 120;
    for (int i = 0; i < 200; ++i) { p.process(k, Sw::Up, tempo, 50, o); if (o.tempo > 0) tempo = o.tempo; }
    CHECK(p.page_up());
    CHECK(tempo == 120);
    CHECK(p.waiting());
    k[1] = Panel::tempo_to_knob(120);   // reaches the stored value
    p.process(k, Sw::Up, tempo, 50, o);
    k[1] = Panel::tempo_to_knob(150);
    for (int i = 0; i < 10; ++i) { p.process(k, Sw::Up, tempo, 50, o); if (o.tempo > 0) tempo = o.tempo; }
    CHECK(std::abs(tempo - 150) <= 1);
    // Slot select: knob from zone 5 to zone 2, settles, loads once.
    int selected = -1, count = 0;
    k[0] = 1700;
    for (int i = 0; i < 4000; ++i) { p.process(k, Sw::Up, tempo, 50, o); if (o.select_slot >= 0) { selected = o.select_slot; ++count; } }
    CHECK(selected == 2 && count == 1);
    // Back to PERFORM: macro 0 was 4095 -> must pick up again.
    k[0] = 0;
    for (int i = 0; i < 200; ++i) p.process(k, Sw::Middle, tempo, 50, o);
    CHECK(p.macro(0) > 50);
    CHECK(p.waiting());
}

// 0.3.2: the slot knob's travel is split evenly over the occupied slots.
static void test_slot_zones() {
    CHECK(Panel::raw_zone(2047, 2) == 0 && Panel::raw_zone(2048, 2) == 1);
    CHECK(Panel::raw_zone(1365, 3) == 0 && Panel::raw_zone(1366, 3) == 1 && Panel::raw_zone(2731, 3) == 2);
    CHECK(Panel::raw_zone(4095, 1) == 0);
    // Hysteresis: from zone 0 the knob must pass 50 % + 64 counts.
    CHECK(Panel::hyst_zone(2100, 2, 0) == 0 && Panel::hyst_zone(2120, 2, 0) == 1);
    CHECK(Panel::hyst_zone(2000, 2, 1) == 1 && Panel::hyst_zone(1980, 2, 1) == 0);
    auto run = [](Panel& p, int knob, int n, int& sel) {
        int k[3] = {knob, 2048, 2048};
        PanelOut o;
        for (int i = 0; i < n; ++i) { p.process(k, Sw::Up, 120, 50, o); if (o.select_slot >= 0) sel = o.select_slot; }
    };
    {   // Two songs in slots 1 and 3 (index 0, 2), playing slot 1.
        Panel p;
        int k[3] = {0, 2048, 2048};
        p.begin(k, Sw::Middle, 120, 50, 0);
        p.set_slot_mask(0x05);
        int sel = -1;
        run(p, 0, 400, sel);             // SELECT page, knob in the first half
        CHECK(sel == -1);
        run(p, 2100, 4000, sel);         // just past 50 %, inside the hysteresis
        CHECK(sel == -1);
        run(p, 2200, 4000, sel);         // second half -> slot index 2 (empty slots skipped)
        CHECK(sel == 2 && p.slot() == 2);
        sel = -1;
        run(p, 2000, 4000, sel);         // wobble around the middle: nothing
        CHECK(sel == -1);
        run(p, 1000, 4000, sel);
        CHECK(sel == 0);
    }
    {   // Three songs (slots 2, 4, 6 = index 1, 3, 5): edges at 33 % and 66 %.
        Panel p;
        int k[3] = {3500, 2048, 2048};
        p.begin(k, Sw::Middle, 120, 50, 1);
        p.set_slot_mask(0x2A);
        int sel = -1;
        run(p, 3500, 4000, sel);         // opening the page on slot 6's zone loads nothing
        CHECK(sel == -1);
        run(p, 2000, 4000, sel);         // middle third
        CHECK(sel == 3);
        run(p, 3500, 4000, sel);
        CHECK(sel == 5);
        sel = -1;
        run(p, 500, 4000, sel);          // first third = the playing slot 2 again
        CHECK(sel == 1);
    }
    {   // Only one song: the knob does nothing.
        Panel p;
        int k[3] = {0, 2048, 2048};
        p.begin(k, Sw::Up, 120, 50, 0);
        p.set_slot_mask(0x01);
        int sel = -1;
        run(p, 4000, 4000, sel);
        run(p, 0, 4000, sel);
        CHECK(sel == -1);
    }
}

// ----------------------------------------------------------- protocol ------
struct TestHost : proto::Host {
    Song* song;
    std::vector<std::vector<uint8_t>> sent;
    std::vector<Command> cmds;
    int saved = -1, loaded = -1, inited = -1, pings = 0;
    explicit TestHost(Song* s) : song(s) {}
    Song* live() override { return song; }
    void send(const uint8_t* b, int n) override { sent.emplace_back(b, b + n); }
    bool command(const Command& c) override { cmds.push_back(c); return true; }
    void request_save(int s) override { saved = s; }
    void request_load(int s) override { loaded = s; }
    void request_init(int k) override { inited = k; }
    void info_fields(uint8_t& slot, uint8_t& mask) override { slot = 2; mask = 5; }
    void pinged() override { ++pings; }
};

static std::vector<uint8_t> sysex(std::initializer_list<uint8_t> body) {
    std::vector<uint8_t> v{0xF0, proto::kMfr, proto::kDevice};
    v.insert(v.end(), body);
    v.push_back(0xF7);
    return v;
}

static void test_protocol() {
    auto s = make_song(true);
    TestHost h(s.get());
    proto::Protocol pr(h);
    auto feed = [&](const std::vector<uint8_t>& v) { pr.feed(v.data(), static_cast<int>(v.size())); };

    feed(sysex({proto::Hello}));
    CHECK(h.sent.size() == 1 && h.sent[0][0] == proto::Info && h.sent[0][11] == 2);

    feed(sysex({proto::SetCell, 3, 1, 10, 62, 90, 'R', 3}));
    CHECK(s->cells[3][1][10].note == 62 && s->cells[3][1][10].fx == 'R' && s->cells[3][1][10].fxv == 3);
    // Out of range pattern: rejected, nothing written.
    const uint32_t rej = pr.rejected();
    feed(sysex({proto::SetCell, 16, 1, 10, 62, 90, 0, 0}));
    CHECK(pr.rejected() == rej + 1);
    // Other manufacturer: ignored completely.
    std::vector<uint8_t> other{0xF0, 0x7D, 0x47, proto::SetTempo, 1, 0, 0xF7};
    feed(other);
    CHECK(s->tempo() == 118);
    // Split across reads + interleaved clock byte.
    std::vector<uint8_t> t = sysex({proto::SetTempo, 1, 12});   // 140
    pr.feed(t.data(), 3);
    const uint8_t clk = 0xF8;
    pr.feed(&clk, 1);
    pr.feed(t.data() + 3, static_cast<int>(t.size()) - 3);
    CHECK(s->tempo() == 140);

    feed(sysex({proto::SetCells, 0, 5, 60, 2, 40, 0, 0, 0, 41, 100, 'G', 4}));
    CHECK(s->cells[0][5][61].note == 41 && s->cells[0][5][61].fx == 'G');
    feed(sysex({proto::SetCells, 0, 5, 62, 4, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}));  // past row 63
    CHECK(s->cells[0][5][62].note == 0);

    feed(sysex({proto::SetDrum, 2, DpEngine, 9}));            // engine out of range
    CHECK(s->drum[2][DpEngine] != 9);
    feed(sysex({proto::SetDrum, 2, DpEngine, EngModal}));
    CHECK(s->drum[2][DpEngine] == EngModal);
    feed(sysex({proto::SetMix, 4, MpPan, 10}));
    CHECK(s->mix[4][MpPan] == 10);
    feed(sysex({proto::SetEntry, 5, 9, 3}));
    CHECK(s->entries[5].pattern == 9 && s->entries[5].repeats == 3);

    feed(sysex({proto::Transport, 2, 7}));
    CHECK(!h.cmds.empty() && h.cmds.back().type == CmdType::PlayPattern && h.cmds.back().a == 7);
    feed(sysex({proto::Preview, 4, 60, 100}));
    CHECK(h.cmds.back().type == CmdType::Preview && h.cmds.back().b == 60);
    feed(sysex({proto::Save, 3}));
    CHECK(h.saved == 3);
    feed(sysex({proto::Save, 6}));
    CHECK(h.saved == 3);
    feed(sysex({proto::Load, 1}));
    CHECK(h.loaded == 1);

    // Full dump: header + 16*8*4 chunks + end, all 7-bit, re-assembles to the same song.
    h.sent.clear();
    feed(sysex({proto::GetSong}));
    CHECK(h.sent.size() == 1 + 16 * 8 * 4 + 1);
    auto copy = make_song(false);
    bool seven = true;
    for (auto& m : h.sent) {
        for (size_t i = 0; i < m.size(); ++i) if (m[i] & 0x80) seven = false;
        if (m[0] == proto::SongHeader) {
            CHECK(m.size() == 1u + kSongHeaderBytes);
            std::memcpy(static_cast<SongHeader*>(copy.get()), m.data() + 1, kSongHeaderBytes);
        } else if (m[0] == proto::PatternChunk) {
            CHECK(m.size() == 5u + static_cast<size_t>(m[4]) * kCellBytes);
            std::memcpy(&copy->cells[m[1]][m[2]][m[3]], m.data() + 5, static_cast<size_t>(m[4]) * kCellBytes);
        }
    }
    CHECK(seven);
    CHECK(std::memcmp(copy.get(), s.get(), sizeof(Song)) == 0);

    // Header upload is sanitised.
    std::vector<uint8_t> hdr{proto::SetHeader};
    SongHeader bad = *s;
    bad.swing = 99;
    bad.pat_len[0] = 0;
    bad.entries[0].pattern = 40;
    const uint8_t* bp = reinterpret_cast<const uint8_t*>(&bad);
    hdr.insert(hdr.end(), bp, bp + kSongHeaderBytes);
    std::vector<uint8_t> framed{0xF0, proto::kMfr, proto::kDevice};
    framed.insert(framed.end(), hdr.begin(), hdr.end());
    framed.push_back(0xF7);
    feed(framed);
    CHECK(s->swing == kSwingMax && s->pat_len[0] == 1 && s->entries[0].pattern == 0);

    // Status message is 7-bit and has a fixed length.
    EngineStatus st;
    Mixer mx;
    uint8_t buf[64];
    const int n = proto::build_status(buf, st, *s, mx, 40, 50, 3);
    CHECK(n == 30);
    bool ok = true;
    for (int i = 0; i < n; ++i) if (buf[i] & 0x80) ok = false;
    CHECK(ok);
}

// --------------------------------------------------------- flash store -----
static void test_store() {
    auto s = make_song(true);
    uint8_t header[store::kHeaderSize];
    store::make_header(*s, header);
    std::vector<uint8_t> img(store::kSlotSize, 0xFF);
    uint8_t page[store::kPageSize];
    for (uint32_t k = 0; k < store::kRecordPages; ++k) {
        store::record_page(*s, header, k, page);
        std::memcpy(&img[k * store::kPageSize], page, store::kPageSize);
    }
    CHECK(store::slot_valid(img.data()));
    auto back = make_song(false);
    CHECK(store::slot_load(img.data(), *back));
    CHECK(std::memcmp(back.get(), s.get(), sizeof(Song)) == 0);
    img[5000] ^= 1;
    CHECK(!store::slot_valid(img.data()));
    std::vector<uint8_t> erased(store::kSlotSize, 0xFF);
    CHECK(!store::slot_valid(erased.data()));

    // Layout: system sector at the top of 2 MB, the 6 old v2 slots below it
    // (unchanged addresses), the 6 v3 slots below those; all sector aligned.
    const uint32_t fs = 2u * 1024 * 1024;
    CHECK(store::sys_offset(fs) == fs - 4096);
    CHECK(store::v2_slot_offset(fs, 5) + store::kV2SlotSize == store::sys_offset(fs));
    CHECK(store::v2_slot_offset(fs, 0) == fs - 4096 - 6 * 28672);   // same as firmware 0.1/0.2
    CHECK(store::slot_offset(fs, 5) + store::kSlotSize == store::v2_slot_offset(fs, 0));
    CHECK(store::slot_offset(fs, 0) % 4096 == 0);
    CHECK(store::reserved_bytes() == 4096 + 6 * 28672 + 6 * 53248);
    CHECK(store::kRecordPages * store::kPageSize <= store::kSlotSize);
    CHECK(store::kSlotSectors == 9 && store::kRecordSize <= store::kSlotSectors * store::kSectorSize);
    CHECK(sizeof(Song) == 32980);

    std::vector<uint8_t> sys(store::kSectorSize, 0xFF);
    CHECK(store::sys_last_slot(sys.data()) == -1);
    CHECK(store::sys_next_page(sys.data()) == 0);
    store::sys_page(4, page);
    std::memcpy(&sys[0], page, store::kPageSize);
    store::sys_page(1, page);
    std::memcpy(&sys[256], page, store::kPageSize);
    CHECK(store::sys_last_slot(sys.data()) == 1);
    CHECK(store::sys_next_page(sys.data()) == 2);
}

// ------------------------------------------------------------- engine ------
static void test_engine() {
    auto a = make_song(true);
    auto b = make_song(false);
    auto eng = std::make_unique<TrackerEngine>();
    EngineIn in;
    eng->init(a.get(), b.get(), 0, in);
    EngineOut out;
    // Start with a tap on the switch.
    for (int i = 0; i < 4800; ++i) { in.sw = Sw::Down; eng->process(in, out); }
    in.sw = Sw::Middle;
    int32_t peak = 0;
    int gate_edges = 0;
    bool last_gate = false;
    int32_t max_mv = 0;
    for (int i = 0; i < kSampleRate * 4; ++i) {
        eng->process(in, out);
        const int32_t ax = std::abs(out.audio[0]) > std::abs(out.audio[1]) ? std::abs(out.audio[0]) : std::abs(out.audio[1]);
        if (ax > peak) peak = ax;
        CHECK(out.audio[0] >= -2048 && out.audio[0] <= 2047);
        if (out.gate[0] && !last_gate) ++gate_edges;
        last_gate = out.gate[0];
        if (out.cv_mv[0] > max_mv) max_mv = out.cv_mv[0];
    }
    CHECK(eng->seq().playing());
    CHECK(peak > 300);
    CHECK(gate_edges >= 4);
    CHECK(max_mv > 0 && max_mv <= 6000);
    std::printf("  engine: peak %d / 2047, gate edges %d\n", peak, gate_edges);

    // Swap to the empty song at the next pattern boundary.
    const uint32_t swaps = eng->status().swap_count;
    eng->commands().push(Command{CmdType::SwapSong, 0, 0, 0});
    for (int i = 0; i < kSampleRate * 3; ++i) eng->process(in, out);
    CHECK(eng->status().swap_count == swaps + 1);
    CHECK(eng->live() == b.get());
    CHECK(eng->spare() == a.get());

    // Audio inputs reach the outputs through the mixer.
    eng->commands().push(Command{CmdType::Stop, 0, 0, 0});
    int32_t pk = 0;
    in.audio_connected[0] = true;
    for (int i = 0; i < 48000; ++i) {
        in.audio[0] = static_cast<int16_t>((i % 100) < 50 ? 1000 : -1000);
        eng->process(in, out);
        if (i > 24000 && std::abs(out.audio[0]) > pk) pk = std::abs(out.audio[0]);
    }
    CHECK(pk > 300);
}


// ------------------------------------------------------ v2: macros + panel --
static int64_t voice0_energy(int knob_main, bool route_voice0) {
    auto a = make_song(false);
    auto b = make_song(false);
    a->drum[0][DpEngine] = EngKick;
    a->drum[0][DpDecay] = 40;
    if (!route_voice0) a->macro_excl[MacroDecay] = 1;
    auto eng = std::make_unique<TrackerEngine>();
    EngineIn in;
    in.knobs[0] = knob_main;
    eng->init(a.get(), b.get(), 0, in);
    EngineOut out;
    for (int i = 0; i < 64; ++i) eng->process(in, out);
    eng->commands().push(Command{CmdType::Preview, 0, 48, 100});
    int64_t e = 0;
    for (int i = 0; i < kSampleRate * 2; ++i) { eng->process(in, out); e += std::abs(out.audio[0]); }
    return e;
}

static void test_macros_v2() {
    // Decay macro (Main knob) is an offset, applied only to routed voices.
    const int64_t centre = voice0_energy(2048, true);
    const int64_t up_routed = voice0_energy(4095, true);
    const int64_t up_excluded = voice0_energy(4095, false);
    const int64_t down_routed = voice0_energy(0, true);
    CHECK(up_excluded == centre);
    CHECK(up_routed > centre * 3 / 2);
    CHECK(down_routed < centre);
    std::printf("  decay macro energy: down %lld centre %lld up %lld (excluded %lld)\n",
                static_cast<long long>(down_routed), static_cast<long long>(centre),
                static_cast<long long>(up_routed), static_cast<long long>(up_excluded));

    // Header: old songs (0) route everything; sanitising keeps 4 bits.
    auto s = make_song(true);
    for (int v = 0; v < kDrumTracks; ++v) CHECK(macro_on(*s, MacroDecay, v) && macro_on(*s, MacroColour, v));
    s->macro_excl[0] = 0x7A;
    header_sanitize(*s);
    CHECK(s->macro_excl[0] == 0x0A);
    CHECK(macro_on(*s, MacroDecay, 0) && !macro_on(*s, MacroDecay, 1));

    // SetMacroMask over the protocol (enabled bits on the wire).
    TestHost h(s.get());
    proto::Protocol pr(h);
    auto feed = [&](const std::vector<uint8_t>& v) { pr.feed(v.data(), static_cast<int>(v.size())); };
    feed(sysex({proto::SetMacroMask, 0, 0x05}));
    CHECK(s->macro_excl[MacroDecay] == 0x0A);
    feed(sysex({proto::SetMacroMask, 1, 0x0E}));
    CHECK(s->macro_excl[MacroColour] == 0x01);
    const uint32_t rej = pr.rejected();
    feed(sysex({proto::SetMacroMask, 2, 0x01}));
    feed(sysex({proto::SetMacroMask, 0, 0x10}));
    feed(sysex({proto::SetMacroMask, 0}));
    CHECK(pr.rejected() == rej + 3);
    feed(sysex({proto::Hello}));
    CHECK(!h.sent.empty() && h.sent.back()[0] == proto::Info && h.sent.back()[1] == 8 && h.sent.back()[3] == 4 &&
          h.sent.back()[4] == 3 && h.sent.back()[5] == 8);

    // Panel report contents.
    EngineStatus st;
    st.flags = 16; st.down_held = 1;
    st.knob[0] = 4095; st.knob[1] = 0; st.knob[2] = 2048;
    st.knob_waiting = 2;
    st.macro[0] = 60; st.macro[1] = -30; st.macro[2] = 20;
    st.slot = 3; st.mode = 1; st.entry = 2; st.pattern = 5; st.transpose = -7;
    s->drum[0][DpDecay] = 100; s->drum[1][DpDecay] = 100;
    s->drum[0][DpA] = 10; s->drum[1][DpA] = 10;
    s->master[MsFilter] = 64;
    proto::PanelView pv{{127, 0, 64}};
    uint8_t b[64];
    const int n = proto::build_panel(b, st, *s, pv);
    CHECK(n == 1 + proto::kPanelBytes);
    CHECK(b[0] == proto::Panel && b[1] == 3);
    CHECK(b[2] == 127 && b[3] == 0 && b[4] == 64 && b[5] == 2);
    CHECK(b[6] == 124 && b[7] == 34 && b[8] == 84);
    CHECK(b[9] == 0x05 && b[10] == 0x0E);
    CHECK(b[11] == 127 && b[12] == 100);          // voice 0 routed (clamped), voice 1 not
    CHECK(b[15] == 10 && b[16] == 0);              // colour: voice 0 not routed, voice 1 routed (clamped)
    CHECK(b[19] == 84);                            // filter 64 + 20
    CHECK(b[23] == 3 && b[24] == 1 && b[25] == 2 && b[26] == 5 && b[27] == 57);
    for (int i = 1; i < n; ++i) CHECK(b[i] < 128);

    // Reporter: on change only, rate-limited, jitter-proof, keep-alive.
    proto::PanelReporter rep;
    EngineStatus q;
    uint32_t t = 1000;
    CHECK(rep.poll(t, q, *s, b) > 0);              // first report is forced
    CHECK(rep.poll(t + 1000, q, *s, b) == 0);
    t += 40000;
    CHECK(rep.poll(t, q, *s, b) == 0);             // nothing changed
    q.knob[0] = 2048 + 20;                          // ADC jitter
    t += 40000;
    CHECK(rep.poll(t, q, *s, b) == 0);
    q.knob[0] = 3000;
    CHECK(rep.poll(t + 1000, q, *s, b) > 0 && b[2] == 3000 / 32);
    t += 1000;
    q.knob[0] = 1000;
    CHECK(rep.poll(t + 1000, q, *s, b) == 0);      // changed but too soon after the last report
    t += 40000;
    CHECK(rep.poll(t, q, *s, b) > 0 && b[2] == 1000 / 32);
    int sent = 0;
    for (int i = 0; i < 100; ++i) {                 // fast sweep: at most ~30/s
        q.knob[1] = i * 40;
        t += 2000;
        if (rep.poll(t, q, *s, b) > 0) ++sent;
    }
    CHECK(sent >= 5 && sent <= 7);                  // 200 ms of moving
    t += 2000000;
    CHECK(rep.poll(t, q, *s, b) > 0);              // keep-alive
    rep.force();
    CHECK(rep.poll(t + 10, q, *s, b) > 0);
}

// ------------------------------------------------------------------ v3 -----
// Mirrors core 1's slot handling in main.cpp (load_into_spare_and_swap,
// free_spare, the panel request loop) against the real engine.
// (not inlined: GCC 14 reports a bogus stringop-overflow inside the queue otherwise)
__attribute__((noinline)) static void push_cmd(TrackerEngine& e, const Command& c) { e.commands().push(c); }

struct Core1Sim {
    TrackerEngine& eng;
    std::vector<std::unique_ptr<Song>> slots;   // nullptr = empty slot
    EngineIn* in;
    uint32_t swaps_requested = 0;
    bool hung = false;
    void run(int n) { EngineOut o; for (int i = 0; i < n; ++i) eng.process(*in, o); }
    // wait_swap_idle(): core 1 spins (servicing USB) while core 0 plays.
    void wait_idle(int max_samples) {
        int n = 0;
        while (eng.status().swap_count != swaps_requested || !eng.swap_idle()) {
            run(1);
            if (++n > max_samples) { hung = true; return; }
        }
    }
    void free_spare() {
        if (!eng.swap_idle()) push_cmd(eng, Command{CmdType::CancelSwap, 0, 0, 0});
        wait_idle(kSampleRate * 2);
    }
    bool serve() {
        const int s = eng.take_slot_request();
        if (s < 0) return false;
        free_spare();
        if (hung) return true;
        if (!slots[s]) {
            push_cmd(eng, Command{CmdType::SetSlot, static_cast<uint8_t>(eng.status().slot), 0, 0});
            return true;
        }
        *eng.spare() = *slots[s];
        ++swaps_requested;
        push_cmd(eng, Command{CmdType::SwapSong, 0, static_cast<uint8_t>(s + 1), 0});
        return true;
    }
};

// 0.3.2: fixed CV mapping C0 = 0 V; older songs keep their voltages.
static void test_cv_migration() {
    auto s = make_song(false);
    s->cv_zero_note = 36;                              // 0.3.1 default: C2 = 0 V
    set_cell(*s, 0, 4, 0, 36, 90);                     // 0 V -> C0, velocity dropped
    set_cell(*s, 0, 4, 1, 48);                         // 1 V -> C1
    set_cell(*s, 0, 4, 2, 30);                         // below the old 0 V note (was stuck at 0 V) -> F#0
    set_cell(*s, 3, 5, 9, 120);                        // above 6 V -> folded down to C5
    set_cell(*s, 0, 5, 3, kNoteOff, 50);
    set_cell(*s, 0, 6, 0, 30, 70);                     // IN track: untouched
    set_cell(*s, 0, 0, 0, 30, 70);                     // drum: untouched
    song_sanitize(*s);
    CHECK(s->cv_zero_note == kCvZeroNote);
    CHECK(s->cells[0][4][0].note == 12 && s->cells[0][4][0].vel == 0);
    CHECK(s->cells[0][4][1].note == 24);
    CHECK(s->cells[0][4][2].note == 18);
    CHECK(s->cells[3][5][9].note == 72);
    CHECK(s->cells[0][5][3].note == kNoteOff && s->cells[0][5][3].vel == 0);
    CHECK(s->cells[0][6][0].note == 30 && s->cells[0][6][0].vel == 70);
    CHECK(s->cells[0][0][0].note == 30 && s->cells[0][0][0].vel == 70);
    const Song before = *s;
    song_sanitize(*s);                                 // idempotent
    CHECK(std::memcmp(&before, s.get(), sizeof(Song)) == 0);
    // A custom 0 V note (C3 = 48) keeps its voltages too.
    auto c = make_song(false);
    c->cv_zero_note = 48;
    set_cell(*c, 0, 4, 0, 60);                         // 1 V
    song_sanitize(*c);
    CHECK(c->cells[0][4][0].note == 24);
    // Demo and new songs: every CV note in C0..B5, no CV velocity.
    auto d = make_song(true);
    bool ok = d->cv_zero_note == kCvZeroNote;
    for (auto& p : d->cells)
        for (int t = kFirstCvTrack; t < kFirstInTrack; ++t)
            for (const Cell& x : p[t]) {
                if (x.vel) ok = false;
                if (x.note && x.note != kNoteOff && (x.note < kCvNoteMin || x.note > kCvNoteMax)) ok = false;
            }
    CHECK(ok);
    // Protocol: SetZeroNote is ignored, CV cells lose their velocity and fold into range.
    TestHost h(s.get());
    proto::Protocol pr(h);
    auto feed = [&](const std::vector<uint8_t>& v) { pr.feed(v.data(), static_cast<int>(v.size())); };
    feed(sysex({proto::SetZeroNote, 36}));
    CHECK(s->cv_zero_note == kCvZeroNote);
    feed(sysex({proto::SetCell, 1, 4, 0, 100, 80, 0, 0}));   // E7 -> E5, vel 0
    CHECK(s->cells[1][4][0].note == 76 && s->cells[1][4][0].vel == 0);
    feed(sysex({proto::SetCell, 1, 6, 0, 100, 80, 0, 0}));   // IN track keeps its LVL
    CHECK(s->cells[1][6][0].note == 100 && s->cells[1][6][0].vel == 80);
    SongHeader hd = *s;
    hd.cv_zero_note = 40;
    header_sanitize(hd);
    CHECK(hd.cv_zero_note == kCvZeroNote);
}

static int zone_knob(int zone, int n = 2) { return zone * 4096 / n + 2048 / n; }   // middle of a slot zone (n occupied slots)

static void test_slot_select_bug() {
    // Two saved slots (1 and 2 = index 0 and 1), started from slot 2, playing.
    auto a = make_song(false), b = make_song(false);
    auto eng = std::make_unique<TrackerEngine>();
    EngineIn in;
    Core1Sim c1{*eng, {}, &in};
    for (int i = 0; i < 6; ++i) c1.slots.emplace_back(nullptr);
    c1.slots[0] = make_song(false); std::memcpy(c1.slots[0]->name, "SLOT ONE    ", 12);
    c1.slots[1] = make_song(false); std::memcpy(c1.slots[1]->name, "SLOT TWO    ", 12);
    *a = *c1.slots[1];
    in.knobs[0] = zone_knob(1);
    eng->init(a.get(), b.get(), 1, in);
    push_cmd(*eng, Command{CmdType::SetSlotMask, 0x03, 0, 0});   // core 1 at boot: slots 1 and 2 hold songs
    eng->commands().push(Command{CmdType::PlaySong, 0, 0, 0});
    c1.run(4800);
    in.sw = Sw::Up;                      // SELECT page
    c1.run(4800);
    // Slot 2 -> slot 1 while playing: waits for the pattern end (16 rows = 96000 samples).
    in.knobs[0] = zone_knob(0);
    c1.run(kSampleRate / 2);
    CHECK(c1.serve());
    c1.run(4800);
    CHECK(eng->seq().swap_pending());
    // The user stops the sequencer before the pattern ends (Stop from the app or panel).
    eng->commands().push(Command{CmdType::Stop, 0, 0, 0});
    c1.run(100);
    // Firmware 0.2: the swap stayed pending forever once stopped and core 1
    // hung in wait_swap_idle() on the next load, so 1 -> 2 never happened.
    CHECK(eng->swap_idle());
    CHECK(eng->status().slot == 0);
    CHECK(std::memcmp(eng->live()->name, "SLOT ONE", 8) == 0);
    // ... and back to slot 2.
    in.knobs[0] = zone_knob(1);
    c1.run(kSampleRate / 2);
    CHECK(c1.serve());
    c1.run(100);
    CHECK(!c1.hung);
    CHECK(eng->status().slot == 1);
    CHECK(std::memcmp(eng->live()->name, "SLOT TWO", 8) == 0);

    // While playing: 2 -> 1 -> 2 round trip through pattern ends.
    eng->commands().push(Command{CmdType::PlaySong, 0, 0, 0});
    in.knobs[0] = zone_knob(0);
    c1.run(kSampleRate / 2);
    CHECK(c1.serve());
    c1.run(16 * 6000);
    CHECK(eng->status().slot == 0 && std::memcmp(eng->live()->name, "SLOT ONE", 8) == 0);
    in.knobs[0] = zone_knob(1);
    c1.run(kSampleRate / 2);
    CHECK(c1.serve());
    c1.run(16 * 6000);
    CHECK(eng->status().slot == 1 && std::memcmp(eng->live()->name, "SLOT TWO", 8) == 0);

    // A newer choice replaces one still waiting (no hang, last one wins).
    in.knobs[0] = zone_knob(0);
    c1.run(kSampleRate / 2);
    CHECK(c1.serve());
    in.knobs[0] = zone_knob(1);
    c1.run(kSampleRate / 2);
    CHECK(c1.serve());
    CHECK(!c1.hung);
    c1.run(16 * 6000);
    CHECK(eng->status().slot == 1 && std::memcmp(eng->live()->name, "SLOT TWO", 8) == 0);

    // External clock that stops (cable in, no pulses): the waiting swap no
    // longer blocks the next choice.
    in.clock_connected = true;
    in.knobs[0] = zone_knob(0);
    c1.run(kSampleRate / 2);
    CHECK(c1.serve());
    c1.run(kSampleRate);
    CHECK(eng->seq().swap_pending());
    in.knobs[0] = zone_knob(1);
    c1.run(kSampleRate / 2);
    CHECK(c1.serve());
    CHECK(!c1.hung);
    in.clock_connected = false;

    // Empty slot: nothing loads and the panel goes back to the playing slot,
    // so turning back to it is a change again.
    eng->commands().push(Command{CmdType::Stop, 0, 0, 0});
    c1.run(100);
    const uint32_t playing = eng->status().slot;
    CHECK(playing == 1);
    // (Can only happen if the flash record breaks after boot: slot 5 listed but empty.)
    push_cmd(*eng, Command{CmdType::SetSlotMask, 0x13, 0, 0});
    in.knobs[0] = zone_knob(1, 3);
    c1.run(kSampleRate / 2);
    in.knobs[0] = zone_knob(2, 3);
    c1.run(kSampleRate / 2);
    CHECK(c1.serve());
    c1.run(100);
    CHECK(eng->status().slot == playing && eng->panel().slot() == static_cast<int>(playing));
    in.knobs[0] = zone_knob(0, 3);
    c1.run(kSampleRate / 2);
    CHECK(c1.serve());
    c1.run(100);
    CHECK(eng->status().slot == 0);
}

static void test_fx() {
    auto s = make_song(false);
    set_cell(*s, 0, 0, 0, 36, 0, FxRatchet, 4);   // 4 hits
    set_cell(*s, 0, 1, 0, 50, 0, FxProb, 0);      // never
    set_cell(*s, 0, 2, 0, 60, 0, FxParamA, 10);   // lock
    set_cell(*s, 0, 3, 0, 48, 0, FxDelay, 8);     // half a row late
    Cell c{1, 0, FxGate, 5};
    CHECK(fx_get(c, FxGate) == 5);
    CHECK(fx_get(c, FxSlide) == -1);
    CHECK(sizeof(Cell) == 4);
    Sequencer q;
    q.attach(s.get());
    q.play_pattern(0);
    ClockIn ck;
    int hits[kDrumTracks] = {};
    int first3 = -1;
    DrumEvent lock{};
    for (int i = 0; i < 9500; ++i) {
        q.tick(ck);
        DrumEvent e;
        while (q.pop_drum(e)) {
            ++hits[e.track];
            if (e.track == 2) lock = e;
            if (e.track == 3 && first3 < 0) first3 = i;
        }
    }
    CHECK(hits[0] == 4);
    CHECK(hits[1] == 0);
    CHECK(hits[2] == 1 && lock.a == 10 && lock.decay == kNoLock && lock.b == kNoLock);
    CHECK(hits[3] == 1 && std::abs(first3 - (3 + 3000)) <= 2);
}

static void test_in_tracks() {
    // IN1 driven by its own track: gate length, velocity = level, OFF, D lock.
    auto s = make_song(false);
    s->set_tempo(120);
    s->mix[4][MpVcaSrc] = VcaInTrack;
    s->mix[4][MpVca] = 20;
    s->mix[4][MpInGate] = 4;                                  // quarter row
    set_cell(*s, 0, kFirstInTrack, 0, 60, 64);
    set_cell(*s, 0, kFirstInTrack, 1, 60, 0, FxDecay, 90);   // decay lock
    set_cell(*s, 0, kFirstInTrack, 2, 60, 0, FxGate, 0);     // tie
    set_cell(*s, 0, kFirstInTrack, 3, kNoteOff);
    Sequencer q;
    q.attach(s.get());
    q.play_pattern(0);
    ClockIn ck;
    for (int i = 0; i < 20; ++i) q.tick(ck);
    CHECK(q.in_gate(0) && q.in_vel(0) == 64 && q.in_decay_lock(0) == kNoLock);
    for (int i = 20; i < 1600; ++i) q.tick(ck);
    CHECK(!q.in_gate(0));                                     // fell at ~1500
    for (int i = 1600; i < 6000 + 300; ++i) q.tick(ck);
    CHECK(q.in_gate(0) && q.in_vel(0) == 0 && q.in_decay_lock(0) == 90);
    for (int i = 6300; i < 7700; ++i) q.tick(ck);
    CHECK(!q.in_gate(0));                                     // quarter row again
    for (int i = 7700; i < 12300; ++i) q.tick(ck);
    CHECK(q.in_gate(0) && q.in_decay_lock(0) == kNoLock);
    for (int i = 12300; i < 17800; ++i) q.tick(ck);
    CHECK(q.in_gate(0));                                      // tied
    for (int i = 17800; i < 18300; ++i) q.tick(ck);
    CHECK(!q.in_gate(0));                                     // OFF at row 3
    CHECK(vca_source(*s, 0) == VcaInTrack && vca_source(*s, 1) == VcaOpen);

    // Engine: a steady input through IN1's VCA, louder with more velocity.
    auto level_for = [](uint8_t vel, VcaSrc src) {
        auto a = make_song(false), b = make_song(false);
        a->mix[4][MpVcaSrc] = src;
        a->mix[4][MpVca] = 60;
        a->mix[4][MpPan] = 64;
        a->mix[5][MpMute] = 127;
        a->master[MsDrive] = 0;
        for (int d = 0; d < kDrumTracks; ++d) a->mix[d][MpMute] = 127;
        a->cv[0][CpGate] = 0;
        set_cell(*a, 0, kFirstInTrack, 0, 60, vel);
        set_cell(*a, 0, kFirstCvTrack, 0, 48, vel);
        a->mix[4][MpInGate] = 0;
        auto e = std::make_unique<TrackerEngine>();
        EngineIn in;
        in.audio_connected[0] = true;
        e->init(a.get(), b.get(), 0, in);
        e->commands().push(Command{CmdType::PlayPattern, 0, 0, 0});
        EngineOut o;
        int32_t peak = 0;
        for (int i = 0; i < 4000; ++i) {
            in.audio[0] = (i & 32) ? 600 : -600;
            e->process(in, o);
            if (i > 2000) peak = std::max<int32_t>(peak, std::abs(o.audio[0]));
        }
        return peak;
    };
    const int32_t full = level_for(0, VcaInTrack), half = level_for(64, VcaInTrack), loud = level_for(127, VcaInTrack);
    std::printf("  IN VCA level: vel default %d, 64 %d, 127 %d, open %d\n", full, half, loud, level_for(64, VcaOpen));
    CHECK(half > 0 && half < loud * 3 / 4 && std::abs(full - loud) <= 2);
    // Follow-CV mode (0.3.2): the CV gate opens the VCA at full level; a CV velocity has no effect.
    const int32_t cv_half = level_for(64, VcaCvGate), cv_full = level_for(0, VcaCvGate);
    CHECK(cv_full > 0 && cv_half == cv_full && std::abs(cv_full - full) <= 2);
    CHECK(level_for(64, VcaOpen) == level_for(0, VcaOpen));
}

static void test_cvin_macros() {
    auto s = make_song(false);
    s->set_tempo(120);
    s->cv[0][CpGate] = 8;
    s->macro_cvin[MacroDecay] = 0x01;                          // CV1 only
    set_cell(*s, 0, 4, 0, 48);
    set_cell(*s, 0, 5, 0, 48);
    Sequencer q;
    q.attach(s.get());
    auto gate_len = [&](int track_cv, int md) {
        q.stop();
        q.set_macros(md, 0);
        q.play_pattern(0);
        ClockIn ck;
        int n = 0;
        for (int i = 0; i < 6000; ++i) { q.tick(ck); if (q.gate(track_cv)) ++n; }
        return n;
    };
    s->cv[1][CpGate] = 8;
    CHECK(std::abs(gate_len(0, 0) - 3000) < 20);
    CHECK(std::abs(gate_len(0, 8) - 4500) < 20);               // +4/16 -> 12/16 row
    CHECK(gate_len(0, 63) > 5900);                             // longer than the row
    CHECK(std::abs(gate_len(0, -64) - 375) < 20);              // clamps at 1/16 row
    CHECK(std::abs(gate_len(1, 8) - 3000) < 20);              // CV2 not routed
    s->cv[0][CpGate] = 0;                                      // ties stay ties
    CHECK(gate_len(0, -64) > 5900);

    // Effective values and Panel v3 bytes.
    auto p = make_song(false);
    EngineStatus st;
    st.macro[0] = 40; st.macro[1] = -20;
    p->cv[0][CpGate] = 8; p->cv[1][CpGate] = 0; p->cv[0][CpGlide] = 30;
    p->mix[4][MpVca] = 50; p->mix[5][MpDrive] = 10;
    p->macro_cvin[MacroDecay] = 0x0F; p->macro_cvin[MacroColour] = 0x0B;  // X: CV1, CV2, IN2
    CHECK(proto::effective_cv(*p, st, 0, CpGate) == 28);
    CHECK(proto::effective_cv(*p, st, 1, CpGate) == 0);
    CHECK(proto::effective_cv(*p, st, 0, CpGlide) == 10);
    CHECK(proto::effective_in(*p, st, 0, MpVca) == 90);
    CHECK(proto::effective_in(*p, st, 0, MpDrive) == 0);       // IN1 not on X
    CHECK(proto::effective_in(*p, st, 1, MpDrive) == 0);       // 10 - 20 clamps
    proto::PanelView pv{{1, 2, 3}};
    uint8_t b[64];
    CHECK(proto::build_panel(b, st, *p, pv) == 38);
    CHECK(b[28] == 0x0F && b[29] == 0x0B && b[30] == 28 && b[31] == 0 && b[32] == 10);
    CHECK(b[34] == 90 && b[35] == 40 && b[36] == 0 && b[37] == 0);

    // Mixer: Main lengthens the IN VCA decay when routed.
    p->mix[4][MpVcaSrc] = VcaInTrack;
    Mixer mx;
    mx.set_macros(0, 0); mx.update(*p, 0);
    CHECK(mx.source(0) == VcaInTrack && mx.source(1) == VcaOpen);
}

static void test_migration() {
    // A v2 flash record (version 1, 6 tracks x 4-byte cells) loads as v3.
    auto v3 = make_song(true);
    v3->mix[4][MpVca] = 30;                                    // old song: VCA follows CV gate
    std::vector<uint8_t> body(kV2SongBytes, 0);
    SongHeader h2 = *v3;
    h2.macro_cvin[0] = h2.macro_cvin[1] = 0;
    for (int i = 0; i < kInputs; ++i) { h2.mix[4 + i][MpVcaSrc] = 0; h2.mix[4 + i][MpInGate] = 0; }
    std::memcpy(body.data(), &h2, kSongHeaderBytes);
    for (int p = 0; p < kPatterns; ++p)
        for (int t = 0; t < kV2Tracks; ++t)
            for (int r = 0; r < kRows; ++r) {
                const Cell& c = v3->cells[p][t][r];
                uint8_t* d = body.data() + kSongHeaderBytes + ((p * kV2Tracks + t) * kRows + r) * 4;
                d[0] = c.note; d[1] = c.vel; d[2] = c.fx; d[3] = c.fxv;
            }
    std::vector<uint8_t> rec(store::kV2SlotSize, 0xFF);
    const uint32_t crc = store::crc32(body.data(), kV2SongBytes) ^ 0xFFFFFFFFu;
    store::put32(rec.data(), store::kSongMagic);
    rec[4] = 1; rec[5] = 0; rec[6] = kV2SongBytes & 0xFF; rec[7] = kV2SongBytes >> 8;
    store::put32(rec.data() + 8, crc);
    store::put32(rec.data() + 12, ~crc);
    std::memcpy(rec.data() + 16, body.data(), kV2SongBytes);
    std::vector<uint8_t> empty(store::kSlotSize, 0xFF);
    CHECK(store::slot_valid_v2(rec.data()));
    CHECK(!store::slot_valid(rec.data()));
    auto out = make_song(false);
    CHECK(store::slot_load_any(empty.data(), rec.data(), *out));
    CHECK(std::memcmp(out->name, v3->name, kNameLen) == 0 && out->tempo() == v3->tempo());
    bool cells_ok = true;
    for (int p = 0; p < kPatterns; ++p)
        for (int t = 0; t < kTracks; ++t)
            for (int r = 0; r < kRows; ++r) {
                const Cell& a = out->cells[p][t][r];
                const Cell& e = v3->cells[p][t][r];
                if (t < kV2Tracks) {
                    if (a.note != e.note || a.vel != e.vel || a.fx != e.fx || a.fxv != e.fxv) cells_ok = false;
                } else if (a.note || a.vel || a.fx || a.fxv) {
                    cells_ok = false;
                }
            }
    CHECK(cells_ok);
    CHECK(out->macro_cvin[0] == 0 && out->macro_cvin[1] == 0);  // macros act on drums only, as before
    CHECK(out->mix[4][MpVcaSrc] == VcaAuto && vca_source(*out, 0) == VcaCvGate);
    CHECK(vca_source(*out, 1) == VcaOpen);
    CHECK(out->mix[4][MpInGate] == 8);
    // A v3 record wins over the old one; a broken old one is ignored.
    auto mine = make_song(false);
    uint8_t header[store::kHeaderSize];
    store::make_header(*mine, header);
    std::vector<uint8_t> img(store::kSlotSize, 0xFF);
    uint8_t page[store::kPageSize];
    for (uint32_t k = 0; k < store::kRecordPages; ++k) {
        store::record_page(*mine, header, k, page);
        std::memcpy(&img[k * store::kPageSize], page, store::kPageSize);
    }
    CHECK(store::slot_load_any(img.data(), rec.data(), *out) && std::memcmp(out->name, "NEW SONG", 8) == 0);
    rec[100] ^= 1;
    CHECK(!store::slot_load_any(empty.data(), rec.data(), *out));

    // A firmware 0.3.0 record (version 2, 8 tracks x 6-byte cells) at the
    // current slot address: header kept, FX2 dropped (or moved into an empty FX1).
    auto src = make_song(true);
    std::vector<uint8_t> b3(kV030SongBytes, 0);
    std::memcpy(b3.data(), static_cast<SongHeader*>(src.get()), kSongHeaderBytes);
    auto cell3 = [&](int p, int t, int r) { return b3.data() + kSongHeaderBytes + ((p * kTracks + t) * kRows + r) * 6; };
    for (int p = 0; p < kPatterns; ++p)
        for (int t = 0; t < kTracks; ++t)
            for (int r = 0; r < kRows; ++r) {
                const Cell& c = src->cells[p][t][r];
                uint8_t* d = cell3(p, t, r);
                d[0] = c.note; d[1] = c.vel; d[2] = c.fx; d[3] = c.fxv;
            }
    uint8_t* x = cell3(2, 2, 15); x[0] = 84; x[2] = 'R'; x[3] = 4; x[4] = 'P'; x[5] = 60;   // two effects
    uint8_t* y = cell3(0, 7, 3); y[0] = 60; y[2] = 0; y[3] = 0; y[4] = 'K'; y[5] = 8;        // FX2 only
    std::vector<uint8_t> r3(store::kSlotSize, 0xFF);
    const uint32_t crc3 = store::crc32(b3.data(), kV030SongBytes) ^ 0xFFFFFFFFu;
    store::put32(r3.data(), store::kSongMagic);
    r3[4] = 2; r3[5] = 0; r3[6] = kV030SongBytes & 0xFF; r3[7] = (kV030SongBytes >> 8) & 0xFF;
    store::put32(r3.data() + 8, crc3);
    store::put32(r3.data() + 12, ~crc3);
    std::memcpy(r3.data() + 16, b3.data(), kV030SongBytes);
    CHECK(store::slot_valid_v030(r3.data()) && !store::slot_valid(r3.data()));
    CHECK(store::slot_load_any(r3.data(), rec.data(), *out));   // wins over a (broken) old record
    CHECK(std::memcmp(out->name, src->name, kNameLen) == 0 && out->macro_cvin[0] == src->macro_cvin[0]);
    CHECK(out->mix[4][MpVcaSrc] == src->mix[4][MpVcaSrc]);
    CHECK(out->cells[2][2][15].fx == 'R' && out->cells[2][2][15].fxv == 4);
    CHECK(out->cells[0][7][3].note == 60 && out->cells[0][7][3].fx == 'K' && out->cells[0][7][3].fxv == 8);
    CHECK(out->cells[0][0][0].note == src->cells[0][0][0].note && out->cells[1][3][0].fx == src->cells[1][3][0].fx);
    // Saving writes the new format over it (9 sectors).
    for (uint32_t k = 0; k < store::kRecordPages; ++k) {
        store::record_page(*mine, header, k, page);
        std::memcpy(&r3[k * store::kPageSize], page, store::kPageSize);
    }
    CHECK(store::slot_valid(r3.data()) && !store::slot_valid_v030(r3.data()));
}

static void test_protocol_v4() {
    auto s = make_song(false);
    TestHost h(s.get());
    proto::Protocol pr(h);
    auto feed = [&](const std::vector<uint8_t>& v) { pr.feed(v.data(), static_cast<int>(v.size())); };
    feed(sysex({proto::SetCell, 2, 7, 3, 61, 90, 'G', 4}));
    const Cell& c = s->cells[2][7][3];
    CHECK(c.note == 61 && c.vel == 90 && c.fx == 'G' && c.fxv == 4);
    feed(sysex({proto::SetCell, 2, 7, 3, 60, 91, 'R', 3, 'P', 50}));   // 0.3.0 editor: FX2 dropped
    CHECK(c.note == 60 && c.vel == 91 && c.fx == 'R' && c.fxv == 3);
    feed(sysex({proto::SetCell, 2, 7, 3, 60, 91, 0, 0, 'P', 50}));       // 0.3.0 editor, only FX2: moved into FX
    CHECK(c.fx == 'P' && c.fxv == 50);
    feed(sysex({proto::SetCell, 2, 7, 3, 60, 91, 0, 0}));                 // 4-byte form clears it
    CHECK(c.fx == 0 && c.fxv == 0);
    feed(sysex({proto::SetCells, 1, 6, 0, 2, 63, 1, 'G', 2, 62, 2, 0, 0}));
    CHECK(s->cells[1][6][0].note == 63 && s->cells[1][6][0].fx == 'G' && s->cells[1][6][1].note == 62);
    feed(sysex({proto::SetCells, 1, 6, 0, 2, 60, 1, 'D', 9, 'K', 4, 64, 2, 0, 0, 0, 0}));   // 0.3.0 6-byte cells
    CHECK(s->cells[1][6][0].note == 60 && s->cells[1][6][0].fx == 'D' && s->cells[1][6][1].note == 64);
    const uint32_t rej = pr.rejected();
    feed(sysex({proto::SetCell, 0, 8, 0, 60, 0, 0, 0}));          // track 8
    feed(sysex({proto::SetCells, 0, 0, 0, 2, 1, 1, 1, 1, 1}));    // wrong length
    feed(sysex({proto::Preview, 8, 60, 100}));
    CHECK(pr.rejected() == rej + 3);
    feed(sysex({proto::Preview, 7, 60, 100}));
    CHECK(h.cmds.back().type == CmdType::Preview && h.cmds.back().a == 7);
    feed(sysex({proto::SetMacroMask, 0, 0x0F, 0x05}));
    CHECK(s->macro_excl[0] == 0 && s->macro_cvin[0] == 0x05);
    feed(sysex({proto::SetMacroMask, 1, 0x03}));                  // v2 form keeps CV/IN bits
    CHECK(s->macro_excl[1] == 0x0C && s->macro_cvin[1] == 0x0F);
    feed(sysex({proto::SetMix, 5, MpVcaSrc, VcaInTrack}));
    CHECK(vca_source(*s, 1) == VcaInTrack);
    // Status: IN gates in bits 2/3 of the gate byte.
    EngineStatus st;
    st.in_gate[1] = 1; st.gate[0] = 1;
    Mixer mx;
    uint8_t b[64];
    proto::build_status(b, st, *s, mx, 0, 0, 0);
    CHECK(b[16] == (1 | 8));
}

// ----------------------------------------------- 0.4.0: optimisations + reverb --
static void test_v040_exact() {
    init_tables();
    // Fast integer square root == reference everywhere we probe (the full
    // 2^32 range was checked once offline, see docs/CPU_BUDGET.md).
    uint32_t x = 1;
    int bad = 0;
    for (uint32_t i = 0; i < 70000; ++i) if (isqrt32_fast(i) != isqrt32(i)) ++bad;
    for (uint32_t r = 1; r < 65536; r += 7) {
        const uint32_t sq = r * r;
        if (isqrt32_fast(sq) != r || isqrt32_fast(sq - 1) != r - 1) ++bad;
        if (r < 65535 && isqrt32_fast(sq + 2 * r) != r) ++bad;
    }
    for (int i = 0; i < 2000000; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        if (isqrt32_fast(x) != isqrt32(x)) ++bad;
    }
    CHECK(isqrt32_fast(0xFFFFFFFFu) == 65535);
    CHECK(bad == 0);
    // Kick pitch sweep without the 64-bit multiply: same value.
    bad = 0;
    for (int i = 0; i < 1000000; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        const uint32_t inc = x, k = (x >> 7) % 57000u, m = 4096u + k;
        const uint32_t ref = static_cast<uint32_t>((static_cast<uint64_t>(inc) * m) >> 12);
        const uint32_t fast = ((inc >> 16) * m << 4) + (((inc & 0xFFFFu) * m) >> 12);
        if (ref != fast) ++bad;
    }
    CHECK(bad == 0);
}

static void test_v040_early_end() {
    // A voice with an end threshold stops earlier; until then it is sample-identical,
    // afterwards the reference output stays below the threshold.
    init_tables();
    for (int eng : {EngKick, EngNoise, EngMetal, EngSnare, EngPluck, EngModal}) {
        DrumVoice ref, fast;
        ref.seed(77); fast.seed(77);
        const int32_t thr = 6, none = 0;
        ref.set_end_threshold(&none);
        fast.set_end_threshold(&thr);
        TriggerArgs t;
        t.engine = static_cast<uint8_t>(eng); t.note = 48; t.vel = 120; t.decay = 50; t.a = 40; t.b = 20;
        ref.trigger(t); fast.trigger(t);
        int n_ref = 0, n_fast = 0, mism = 0;
        int32_t after = 0;
        for (int i = 0; i < kSampleRate * 8; ++i) {
            const int32_t a = ref.render(), b = fast.render();
            if (ref.active()) ++n_ref;
            if (fast.active()) { ++n_fast; if (a != b) ++mism; }
            else if (std::abs(a - b) > after) after = std::abs(a - b);
            if (!fast.active()) fast.idle();
        }
        CHECK(mism == 0);
        CHECK(n_fast <= n_ref);
        CHECK(after <= thr);
        std::printf("  early end, engine %d: %d -> %d active samples, tail after end <= %d\n", eng, n_ref, n_fast, after);
    }
}

static void test_v040_song() {
    // Old songs (fx_ver 0): reverb defaults, sends 0. 0.4.0 songs keep their values.
    Song s;
    song_clear(s);
    CHECK(s.fx_ver == kFxVersion);
    for (int c = 0; c < kChannels; ++c) CHECK(s.fx_send[c] == 0);
    s.fx_ver = 0;
    s.fx_send[2] = 90;
    s.master[MsRvDecay] = 3;
    header_sanitize(s);
    CHECK(s.fx_send[2] == 0 && s.master[MsRvDecay] == kRvDefaults[2] && s.fx_ver == kFxVersion);
    s.fx_send[2] = 90;
    s.master[MsRvDecay] = 3;
    header_sanitize(s);
    CHECK(s.fx_send[2] == 90 && s.master[MsRvDecay] == 3);
    // Protocol: SetMix param 8 = send, 9 is rejected; SetMaster 3..7 = reverb.
    struct H : proto::Host {
        Song* s;
        Song* live() override { return s; }
        void send(const uint8_t*, int) override {}
        bool command(const Command&) override { return true; }
        void request_save(int) override {}
        void request_load(int) override {}
        void request_init(int) override {}
        void info_fields(uint8_t& a, uint8_t& b) override { a = b = 0; }
        void pinged() override {}
    } h;
    h.s = &s;
    proto::Protocol pr(h);
    const uint8_t m1[] = {0xF0, 0x7D, 0x54, 0x15, 4, 8, 55, 0xF7, 0xF0, 0x7D, 0x54, 0x16, 6, 101, 0xF7};
    pr.feed(m1, sizeof m1);
    CHECK(s.fx_send[4] == 55 && s.master[MsRvTone] == 101);
    const uint32_t rej = pr.rejected();
    const uint8_t m2[] = {0xF0, 0x7D, 0x54, 0x15, 4, 9, 55, 0xF7};
    pr.feed(m2, sizeof m2);
    CHECK(pr.rejected() == rej + 1);
}

struct RvRun { bool ran = false; int stop_ms = -1; int32_t peak_wet = 0; double corr = 0; bool zero_after = true; };
static RvRun reverb_run(int send, int size, int decay, int tone, int lofi, int seconds) {
    auto a = make_song(false), b = make_song(false);
    a->drum[1][DpEngine] = EngSnare;
    a->fx_send[1] = static_cast<uint8_t>(send);
    a->master[MsRvSize] = static_cast<uint8_t>(size);
    a->master[MsRvDecay] = static_cast<uint8_t>(decay);
    a->master[MsRvTone] = static_cast<uint8_t>(tone);
    a->master[MsRvLofi] = static_cast<uint8_t>(lofi);
    a->mix[1][MpPan] = 64;
    auto dry = std::make_unique<TrackerEngine>(), wet = std::make_unique<TrackerEngine>();
    auto ad = make_song(false); *ad = *a; ad->fx_send[1] = 0;
    EngineIn in;
    dry->init(ad.get(), b.get(), 0, in);
    wet->init(a.get(), b.get(), 0, in);
    EngineOut od, ow;
    for (int i = 0; i < 64; ++i) { dry->process(in, od); wet->process(in, ow); }
    dry->commands().push(Command{CmdType::Preview, 1, 50, 127});
    wet->commands().push(Command{CmdType::Preview, 1, 50, 127});
    RvRun r;
    double sl = 0, sr = 0, slr = 0;
    for (int i = 0; i < kSampleRate * seconds; ++i) {
        dry->process(in, od); wet->process(in, ow);
        if (wet->mixer().reverb_running()) r.ran = true;
        const int32_t dl = ow.audio[0] - od.audio[0], dr = ow.audio[1] - od.audio[1];
        sl += double(dl) * dl; sr += double(dr) * dr; slr += double(dl) * dr;
        if (std::abs(dl) > r.peak_wet) r.peak_wet = std::abs(dl);
        if (r.ran && r.stop_ms < 0 && !wet->mixer().reverb_running()) r.stop_ms = i * 1000 / kSampleRate;
        if (r.stop_ms >= 0 && (ow.audio[0] != od.audio[0] || ow.audio[1] != od.audio[1])) r.zero_after = false;
    }
    r.corr = slr / std::sqrt(sl * sr + 1e-9);
    return r;
}

static void test_v040_reverb() {
    // Sends at 0: the reverb never runs, the demo plays as before.
    {
        auto a = make_song(true), b = make_song(false);
        auto eng = std::make_unique<TrackerEngine>();
        EngineIn in;
        eng->init(a.get(), b.get(), 0, in);
        eng->commands().push(Command{CmdType::PlaySong, 0, 0, 0});
        EngineOut out;
        bool ran = false;
        for (int i = 0; i < kSampleRate * 3; ++i) { eng->process(in, out); ran |= eng->mixer().reverb_running(); }
        CHECK(!ran);
    }
    const RvRun def = reverb_run(100, 64, 64, 56, 40, 8);
    CHECK(def.ran);
    CHECK(def.stop_ms > 300 && def.stop_ms < 6000);
    CHECK(def.peak_wet > 20);
    CHECK(def.zero_after);
    CHECK(std::abs(def.corr) < 0.6);   // decorrelated stereo return
    std::printf("  reverb default: wet peak %d LSB, L/R corr %.2f, tail stops after %d ms\n", def.peak_wet, def.corr, def.stop_ms);
    const RvRun big = reverb_run(127, 127, 127, 127, 0, 40);
    CHECK(big.ran && big.stop_ms > 2000 && big.stop_ms < 40000 && big.zero_after);
    std::printf("  reverb max size/decay/bright: wet peak %d LSB, corr %.2f, stops after %d ms\n", big.peak_wet, big.corr, big.stop_ms);
    // No limit cycles: with threshold 1 the tail must reach exactly 0 (for every
    // tone / lo-fi corner) and the reverb then stops by itself.
    {
        static Reverb rv;   // 33 KB, keep it off the stack
        for (int tone : {0, 56, 127})
            for (int lofi : {0, 40, 127})
                for (int decay : {0, 127}) {
                    rv = Reverb();
                    rv.set(127, 127, decay, tone, lofi, 1);
                    int32_t l = 0, r = 0;
                    int i = 0;
                    for (; i < kSampleRate * 120; ++i) {
                        l = r = 0;
                        rv.process(i < 2000 ? ((i * 7919) % 60000) - 30000 : 0, l, r);
                        if (i > 2000 && !rv.running()) break;
                    }
                    CHECK(!rv.running());
                    if (rv.running()) std::printf("  reverb never settled: tone %d lofi %d decay %d\n", tone, lofi, decay);
                }
    }
    const RvRun lofi = reverb_run(127, 10, 20, 0, 127, 8);
    CHECK(lofi.ran && lofi.stop_ms > 0 && lofi.zero_after);
    std::printf("  reverb small/dark/full lo-fi: wet peak %d LSB, stops after %d ms\n", lofi.peak_wet, lofi.stop_ms);
}

static void test_v040_leds() {
    // One LED write per sample at most; all six are refreshed every 16 samples.
    auto a = make_song(true), b = make_song(false);
    auto eng = std::make_unique<TrackerEngine>();
    EngineIn in;
    eng->init(a.get(), b.get(), 0, in);
    eng->commands().push(Command{CmdType::PlaySong, 0, 0, 0});
    EngineOut out;
    int seen[6] = {};
    int writes = 0;
    for (int i = 0; i < 16000; ++i) {
        eng->process(in, out);
        if (out.led_index >= 0) {
            CHECK(out.led_index < 6);
            ++seen[out.led_index];
            ++writes;
            CHECK(out.led_value == eng->leds()[out.led_index]);
        }
    }
    for (int k = 0; k < 6; ++k) CHECK(seen[k] >= 900);   // ~1000 refreshes each in 16000 samples
    CHECK(writes <= 16000 * 6 / 16 + 6);
}


// 0.4.1: W effect = reverb send for one hit.
static void test_v041_send_fx() {
    // (a) W on DR2 row 0, a plain hit on row 8: the applied send glides up to
    // the W value (no jump > 128 Q15 per sample), the reverb runs, and the plain
    // hit glides back to the knob (0).
    {
        auto a = make_song(false), b = make_song(false);
        a->drum[1][DpEngine] = EngSnare;
        a->fx_send[1] = 0;
        a->pat_len[0] = 16;
        set_cell(*a, 0, 1, 0, 50, 0, FxSend, 127);
        set_cell(*a, 0, 1, 8, 50);
        auto eng = std::make_unique<TrackerEngine>();
        EngineIn in;
        eng->init(a.get(), b.get(), 0, in);
        EngineOut out;
        for (int i = 0; i < 64; ++i) eng->process(in, out);
        eng->commands().push(Command{CmdType::PlayPattern, 0, 0, 0});
        const int32_t lvl = g_tables.level[a->mix[1][MpLevel]];
        const int32_t want = (lvl * g_tables.send_law[127]) >> 12;
        int32_t prev = 0, max_step = 0, peak = 0;
        bool ran = false, back = false;
        const int row = 60 * kSampleRate / (a->tempo() * 4);   // samples per row (approx.)
        for (int i = 0; i < row * 12; ++i) {
            eng->process(in, out);
            const int32_t sv = eng->mixer().applied_send(1);
            max_step = std::max(max_step, std::abs(sv - prev));
            prev = sv;
            if (i < row * 7) peak = std::max(peak, sv);
            ran |= eng->mixer().reverb_running();
            if (i > row * 9 && sv == 0) back = true;
        }
        CHECK(peak == want);
        CHECK(max_step <= 128);
        CHECK(ran);
        CHECK(back);
        std::printf("  W fx: send %d (Q15) on the W hit, max step %d/sample, back to the knob after the next hit\n", peak, max_step);
    }
    // (b) W00 with the knob at 0 changes nothing at all (bit-identical output,
    // the reverb never starts): the dry path is untouched by the effect.
    {
        auto a = make_song(true), a2 = make_song(true), b = make_song(false);
        for (int r = 0; r < 64; ++r)
            if (a2->cells[0][1][r].note && !a2->cells[0][1][r].fx) a2->cells[0][1][r].fx = FxSend, a2->cells[0][1][r].fxv = 0;
        auto e1 = std::make_unique<TrackerEngine>(), e2 = std::make_unique<TrackerEngine>();
        EngineIn in;
        e1->init(a.get(), b.get(), 0, in);
        e2->init(a2.get(), b.get(), 0, in);
        e1->commands().push(Command{CmdType::PlaySong, 0, 0, 0});
        e2->commands().push(Command{CmdType::PlaySong, 0, 0, 0});
        EngineOut o1, o2;
        bool same = true, ran = false;
        for (int i = 0; i < kSampleRate * 4; ++i) {
            e1->process(in, o1); e2->process(in, o2);
            same &= o1.audio[0] == o2.audio[0] && o1.audio[1] == o2.audio[1];
            ran |= e2->mixer().reverb_running();
        }
        CHECK(same);
        CHECK(!ran);
    }
    // (c) W on an IN track sets the input channel's send; CV tracks ignore W.
    {
        auto a = make_song(false), b = make_song(false);
        a->pat_len[0] = 16;
        a->mix[4][MpVcaSrc] = VcaInTrack;
        set_cell(*a, 0, kFirstInTrack, 0, 60, 0, FxSend, 64);
        set_cell(*a, 0, kDrumTracks, 0, 36, 0, FxSend, 127);   // CV1: no-op
        auto eng = std::make_unique<TrackerEngine>();
        EngineIn in;
        in.audio_connected[0] = true;
        in.audio[0] = 800;
        eng->init(a.get(), b.get(), 0, in);
        EngineOut out;
        for (int i = 0; i < 64; ++i) eng->process(in, out);
        eng->commands().push(Command{CmdType::PlayPattern, 0, 0, 0});
        for (int i = 0; i < 4000; ++i) eng->process(in, out);
        const int32_t lvl = g_tables.level[a->mix[4][MpLevel]];
        CHECK(eng->mixer().applied_send(4) == ((lvl * g_tables.send_law[64]) >> 12));
        CHECK(eng->mixer().applied_send(5) == 0);
        CHECK(eng->mixer().reverb_running());
    }
}

// 0.4.2: live mutes for the reverb return and the master output.
static void test_v042_mutes() {
    auto mk = [](std::unique_ptr<TrackerEngine>& e, std::unique_ptr<Song>& a, std::unique_ptr<Song>& b) {
        a = make_song(true); b = make_song(false);
        for (int c = 0; c < kDrumTracks; ++c) a->fx_send[c] = 90;
        e = std::make_unique<TrackerEngine>();
        EngineIn in;
        e->init(a.get(), b.get(), 0, in);
        e->commands().push(Command{CmdType::PlaySong, 0, 0, 0});
    };
    std::unique_ptr<TrackerEngine> e1, e2;
    std::unique_ptr<Song> a1, b1, a2, b2;
    EngineIn in;
    EngineOut o1, o2;
    // (a) Unmute commands with nothing muted change nothing (bit-identical).
    {
        mk(e1, a1, b1); mk(e2, a2, b2);
        e2->commands().push(Command{CmdType::SetMute, 0, 0, 0});
        e2->commands().push(Command{CmdType::SetMute, 1, 0, 0});
        bool same = true;
        for (int i = 0; i < kSampleRate * 3; ++i) {
            e1->process(in, o1); e2->process(in, o2);
            same &= o1.audio[0] == o2.audio[0] && o1.audio[1] == o2.audio[1];
        }
        CHECK(same);
    }
    // (b) Reverb mute: the wet part fades out in <= 6 ms while the reverb keeps
    // running; after unmuting (and its fade) the output is bit-identical again.
    {
        mk(e1, a1, b1); mk(e2, a2, b2);
        for (int i = 0; i < kSampleRate; ++i) { e1->process(in, o1); e2->process(in, o2); }
        // reference without reverb: same song, sends 0 is not the same signal, so
        // compare against e1 only after unmute; while muted check the return gain.
        e2->commands().push(Command{CmdType::SetMute, 0, 1, 0});
        bool ran = false, flag = false;
        for (int i = 0; i < kSampleRate / 2; ++i) {
            e1->process(in, o1); e2->process(in, o2);
            ran |= e2->mixer().reverb_running();
        }
        flag = (e2->status().flags & 32) != 0;
        CHECK(e2->mixer().reverb_muted() && flag);
        CHECK(e2->mixer().reverb_return_applied() == 0);
        CHECK(ran);
        e2->commands().push(Command{CmdType::SetMute, 0, 0, 0});
        for (int i = 0; i < 400; ++i) { e1->process(in, o1); e2->process(in, o2); }   // fade back (8 ms)
        bool same = true;
        for (int i = 0; i < kSampleRate * 2; ++i) {
            e1->process(in, o1); e2->process(in, o2);
            same &= o1.audio[0] == o2.audio[0] && o1.audio[1] == o2.audio[1];
        }
        CHECK(same);
        CHECK((e2->status().flags & 32) == 0);
    }
    // (c) Master mute: the output fades to exactly 0 within ~6 ms (gain steps of
    // 128/32767 per sample, no hard cut), stays 0, and comes back on unmute.
    {
        mk(e1, a1, b1); mk(e2, a2, b2);
        for (int i = 0; i < kSampleRate; ++i) { e1->process(in, o1); e2->process(in, o2); }
        e2->commands().push(Command{CmdType::SetMute, 1, 1, 0});
        int32_t worst_ratio_jump = 0;
        bool silent = true;
        for (int i = 0; i < kSampleRate; ++i) {
            e1->process(in, o1); e2->process(in, o2);
            if (i > 300) silent &= o2.audio[0] == 0 && o2.audio[1] == 0;
            else worst_ratio_jump = std::max<int32_t>(worst_ratio_jump, std::abs(o2.audio[0]) - std::abs(o1.audio[0]));
        }
        CHECK(silent);
        CHECK(worst_ratio_jump <= 1);   // never louder than unmuted while fading
        CHECK((e2->status().flags & 64) != 0 && e2->mixer().master_muted());
        e2->commands().push(Command{CmdType::SetMute, 1, 0, 0});
        for (int i = 0; i < 400; ++i) { e1->process(in, o1); e2->process(in, o2); }
        bool same = true;
        for (int i = 0; i < kSampleRate; ++i) {
            e1->process(in, o1); e2->process(in, o2);
            same &= o1.audio[0] == o2.audio[0] && o1.audio[1] == o2.audio[1];
        }
        CHECK(same);
        CHECK((e2->status().flags & 64) == 0);
    }
    // (d) Protocol v7: SetMaster 8/9 become SetMute commands and never touch
    // the song (not saved); 10 is rejected.
    {
        struct H : proto::Host {
            Song* s; std::vector<Command> cmds;
            Song* live() override { return s; }
            void send(const uint8_t*, int) override {}
            bool command(const Command& c) override { cmds.push_back(c); return true; }
            void request_save(int) override {}
            void request_load(int) override {}
            void request_init(int) override {}
            void info_fields(uint8_t& x, uint8_t& y) override { x = y = 0; }
            void pinged() override {}
        } h;
        auto s = make_song(false);
        const Song before = *s;
        h.s = s.get();
        proto::Protocol pr(h);
        const uint8_t m[] = {0xF0, 0x7D, 0x54, 0x16, 8, 127, 0xF7, 0xF0, 0x7D, 0x54, 0x16, 9, 64, 0xF7,
                             0xF0, 0x7D, 0x54, 0x16, 9, 0, 0xF7};
        pr.feed(m, sizeof m);
        CHECK(h.cmds.size() == 3);
        if (h.cmds.size() == 3) {
            CHECK(h.cmds[0].type == CmdType::SetMute && h.cmds[0].a == 0 && h.cmds[0].b == 1);
            CHECK(h.cmds[1].type == CmdType::SetMute && h.cmds[1].a == 1 && h.cmds[1].b == 1);
            CHECK(h.cmds[2].a == 1 && h.cmds[2].b == 0);
        }
        CHECK(std::memcmp(&before, s.get(), sizeof(Song)) == 0);
        const uint32_t rej = pr.rejected();
        const uint8_t m2[] = {0xF0, 0x7D, 0x54, 0x16, 10, 127, 0xF7};
        pr.feed(m2, sizeof m2);
        CHECK(pr.rejected() == rej + 1);
    }
    std::printf("  mutes: unmuted bit-identical, reverb/master fade in <= 6 ms, not stored in the song\n");
}

// 0.4.3: kick click, snare/clap morph, grit, song migration.
static std::vector<int32_t> voice_out(uint8_t eng, uint8_t note, uint8_t dec, uint8_t a, uint8_t b, uint8_t x, int n) {
    DrumVoice v;
    TriggerArgs t{eng, note, 127, dec, a, b};
    t.x = x;
    v.trigger(t);
    std::vector<int32_t> o;
    o.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) o.push_back(v.render());
    return o;
}
// Energy envelope in 2 ms windows, normalised.
static std::vector<double> env_of(const std::vector<int32_t>& y) {
    std::vector<double> e;
    for (size_t i = 0; i + 96 <= y.size(); i += 96) {
        double a = 0;
        for (size_t k = i; k < i + 96; ++k) a += double(y[k]) * y[k];
        e.push_back(std::sqrt(a / 96));
    }
    double m = 1e-9;
    for (double v : e) m = std::max(m, v);
    for (double& v : e) v /= m;
    return e;
}
static double env_dist(const std::vector<double>& a, const std::vector<double>& b) {
    double d = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) d += std::fabs(a[i] - b[i]);
    return d / static_cast<double>(std::min(a.size(), b.size()));
}
// Zero-crossing rate of the first 100 ms (brightness proxy).
static double zcr(const std::vector<int32_t>& y) {
    int z = 0;
    for (size_t i = 1; i < 4800 && i < y.size(); ++i) z += (y[i - 1] < 0) != (y[i] < 0);
    return z / 4800.0;
}

static void test_v043() {
    // (a) Kick click: x = 0 is the plain kick; with x > 0 the first ms get a
    // click and, once it is over, the kick continues sample for sample.
    {
        const auto k0 = voice_out(EngKick, 36, 70, 64, 0, 0, 9600);
        const auto k1 = voice_out(EngKick, 36, 70, 64, 0, 100, 9600);
        // high-frequency energy (first difference) in the first 2 ms
        double e0 = 0, e1 = 0;
        for (int i = 1; i < 96; ++i) {
            const double d0 = k0[i] - k0[i - 1], d1 = k1[i] - k1[i - 1];
            e0 += d0 * d0; e1 += d1 * d1;
        }
        CHECK(e1 > e0 * 4);
        bool same_after = true;
        for (int i = 2400; i < 9600; ++i) same_after &= k0[i] == k1[i];
        CHECK(same_after);
        int differ_until = 0;
        for (int i = 0; i < 9600; ++i) if (k0[i] != k1[i]) differ_until = i;
        std::printf("  kick click 100: +%.1f dB high-frequency energy in the first 2 ms, kick identical again after %.1f ms\n",
                    10 * std::log10(e1 / e0), differ_until / 48.0);
        // with drive on as well
        const auto d0 = voice_out(EngKick, 36, 70, 64, 80, 0, 9600);
        const auto d1 = voice_out(EngKick, 36, 70, 64, 80, 127, 9600);
        bool same = true;
        for (int i = 2400; i < 9600; ++i) same &= d0[i] == d1[i];
        CHECK(same);
    }
    // (b) Snare/clap morph: outside 44..84 the old maths; inside, every step of
    // B changes the sound a little. The largest change between neighbouring B
    // values must be small against the whole snare -> clap distance (0.4.2
    // jumped the whole way between 63 and 64).
    {
        std::vector<std::vector<double>> env;
        std::vector<double> z;
        for (int b = 36; b <= 92; ++b) {
            const auto y = voice_out(EngSnare, 50, 70, 64, static_cast<uint8_t>(b), 0, 14400);
            env.push_back(env_of(y));
            z.push_back(zcr(y));
        }
        const double whole = env_dist(env[kMorphLo - 36], env[kMorphHi - 36]);
        const double zwhole = std::fabs(z[kMorphLo - 36] - z[kMorphHi - 36]);
        double worst = 0, zworst = 0;
        int worst_b = 0;
        for (size_t i = 1; i < env.size(); ++i) {
            const double d = env_dist(env[i - 1], env[i]);
            const double dz = std::fabs(z[i] - z[i - 1]);
            if (dz > zworst) { zworst = dz; worst_b = 36 + static_cast<int>(i); }
            worst = std::max(worst, d);
        }
        std::printf("  snare->clap morph: brightness (zero crossings) %.3f -> %.3f, largest step between neighbouring B %.3f (B %d, %.0f %% of the way); envelope steps <= %.3f of %.3f\n",
                    z[kMorphLo - 36], z[kMorphHi - 36], zworst, worst_b, 100 * zworst / zwhole, worst, whole);
        CHECK(zwhole > 0.2);
        CHECK(zworst < zwhole * 0.06);   // 40 steps: an even morph moves 2.5 % per step
        CHECK(worst < std::max(whole * 0.25, 0.01));
    }
    // (c) Grit: 0 = off; on, the voice output is crushed to the grit step and
    // held; the string itself is untouched (pluck: same pitch).
    {
        GritState g;
        g.set(127);
        CHECK(g.n == 4 && g.half == 1 << 9);
        int32_t prev = 0, changes = 0;
        bool quant = true;
        for (int i = 0; i < 400; ++i) {
            const int32_t y = g.process((i * 997) % 20000 - 10000);
            quant &= (y & ((1 << 10) - 1)) == 0;
            quant &= y <= 32767 && y >= -32768;
            if (y != prev) ++changes;
            prev = y;
        }
        CHECK(quant);
        CHECK(changes <= 100);
        GritState g1;
        g1.set(1);
        CHECK(g1.n == 1 && g1.process(1000) == (((1000 * g1.q) >> 4) + 4) / 8 * 8);
        // engine: grit 0 on every voice changes nothing; grit > 0 only the drum it is set on
        auto a = make_song(true), a2 = make_song(true), a3 = make_song(true), b = make_song(false);
        a3->drum[2][DpX] = 90;
        auto e1 = std::make_unique<TrackerEngine>(), e3 = std::make_unique<TrackerEngine>();
        EngineIn in;
        e1->init(a.get(), b.get(), 0, in);
        e3->init(a3.get(), b.get(), 0, in);
        e1->commands().push(Command{CmdType::PlaySong, 0, 0, 0});
        e3->commands().push(Command{CmdType::PlaySong, 0, 0, 0});
        EngineOut o1, o3;
        bool differs = false;
        for (int i = 0; i < kSampleRate * 2; ++i) {
            e1->process(in, o1); e3->process(in, o3);
            differs |= o1.audio[0] != o3.audio[0];
        }
        CHECK(differs);
        (void)a2;
    }
    // (d) Song format: fx_ver 2. A 0.4.0-0.4.2 song (fx_ver 1) keeps its reverb
    // and gets click/grit 0 whatever the reserved bytes held; older songs get
    // both; a 0.4.3 song keeps its values.
    {
        Song s;
        song_clear(s);
        CHECK(s.fx_ver == 2);
        s.fx_ver = 1;
        s.fx_send[1] = 77;
        s.master[MsRvDecay] = 9;
        s.drum[0][DpX] = 55; s.drum[3][DpR7] = 99;
        header_sanitize(s);
        CHECK(s.fx_ver == 2 && s.fx_send[1] == 77 && s.master[MsRvDecay] == 9 && s.drum[0][DpX] == 0 && s.drum[3][DpR7] == 0);
        s.drum[0][DpX] = 55;
        header_sanitize(s);
        CHECK(s.drum[0][DpX] == 55);
        s.fx_ver = 0;
        header_sanitize(s);
        CHECK(s.fx_send[1] == 0 && s.drum[0][DpX] == 0 && s.master[MsRvDecay] == kRvDefaults[2]);
    }
}

int main() {
    test_dsp();
    test_voices();
    test_pitch();
    test_sequencer_timing();
    test_sequencer_song();
    test_cv_tracks();
    test_drum_fx();
    test_panel();
    test_slot_zones();
    test_cv_migration();
    test_protocol();
    test_store();
    test_engine();
    test_macros_v2();
    test_slot_select_bug();
    test_fx();
    test_in_tracks();
    test_cvin_macros();
    test_migration();
    test_protocol_v4();
    test_v040_exact();
    test_v040_early_end();
    test_v040_song();
    test_v040_reverb();
    test_v040_leds();
    test_v041_send_fx();
    test_v042_mutes();
    test_v043();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
