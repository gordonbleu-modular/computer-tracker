// Bare-metal Cortex-M0+ build of the engine for cycle estimates under an
// emulator (tools/cpu_bench/run_bench.py). Not part of the firmware.
// SPDX-License-Identifier: MIT
#include "engine/engine.h"
#include <new>

using namespace trk;

alignas(8) static unsigned char eng_mem[sizeof(TrackerEngine)];
static Song song_a, song_b;
static TrackerEngine* eng;
static EngineIn in;
static EngineOut out;
volatile int32_t sink;

// scenario 0: idle (stopped, no inputs)
// scenario 1: typical demo song playing
// scenario 2: worst case: 4 x modal voices retriggered every row with
//             ratchets, both CV tracks sliding/ratcheting,
//             both IN tracks ratcheting (IN1 own track, IN2
//             following CV2), inputs with drive/tone/VCA, macros routed,
//             master filter on
// scenario 3: demo song with the reverb (sends as docs/audio/demo_reverb.wav)
// scenario 4: worst case + reverb sent from all six channels, maximum size and
//             decay, lo-fi 64 (wobble on)
// scenario 5: stopped, only the reverb tail ringing (one short hit, then silence)
// scenario 6: scenario 4 + grit 31 on all four drums (0.4.3; 1..31 is the
//             dearest setting: no sample-rate hold)
// scenario 7: scenario 4 with the two snares in the morph zone (B = 64) and
//             grit 31 on the two modal voices
// scenario 8: demo song + reverb with every 0.4.3 control on: kick click 100,
//             snare morphing (B = 64), grit 31 on drums 2-4
static void reverb_on(Song& s, const uint8_t* sends, int size, int decay, int lofi) {
    for (int c = 0; c < kChannels; ++c) s.fx_send[c] = sends[c];
    s.master[MsRvReturn] = 100;
    s.master[MsRvSize] = static_cast<uint8_t>(size);
    s.master[MsRvDecay] = static_cast<uint8_t>(decay);
    s.master[MsRvTone] = 52;
    s.master[MsRvLofi] = static_cast<uint8_t>(lofi);
}

extern "C" void bench_init(int scenario) {
    eng = new (eng_mem) TrackerEngine();
    const int sc = scenario;
    if (scenario == 6 || scenario == 7) scenario = 4;
    if (scenario == 8) scenario = 3;
    if (scenario == 2 || scenario == 4) {
        song_clear(song_a);
        song_a.set_tempo(300);
        for (int v = 0; v < kDrumTracks; ++v) {
            song_a.drum[v][DpEngine] = (v & 1) ? EngModal : EngSnare;
            song_a.drum[v][DpDecay] = 127;
            song_a.drum[v][DpB] = 100;
            if (sc == 6) song_a.drum[v][DpX] = 31;
            if (sc == 7) { if (v & 1) song_a.drum[v][DpX] = 31; else song_a.drum[v][DpB] = 64; }
        }
        for (int r = 0; r < 16; ++r) {
            for (int t = 0; t < kDrumTracks; ++t)
                set_cell(song_a, 0, t, r, static_cast<uint8_t>(40 + t * 7), 127, FxRatchet, 4);
            set_cell(song_a, 0, 4, r, static_cast<uint8_t>(40 + r), 90, FxSlide, 30);
            set_cell(song_a, 0, 5, r, static_cast<uint8_t>(50 + r), 90, FxRatchet, 3);
            set_cell(song_a, 0, 6, r, 60, 100, FxRatchet, 4);
            set_cell(song_a, 0, 7, r, 60, 80, FxRatchet, 8);
        }
        song_a.mix[4][MpVcaSrc] = VcaInTrack;
        song_a.mix[5][MpVcaSrc] = VcaCvGate;
        for (int ch = 0; ch < kChannels; ++ch) {
            song_a.mix[ch][MpDrive] = 60;
            song_a.mix[ch][MpTone] = 30;
            song_a.mix[ch][MpVca] = 60;
        }
        song_a.master[MsFilter] = 20;
        if (scenario == 4) {
            const uint8_t all[kChannels] = {100, 100, 100, 100, 100, 100};
            reverb_on(song_a, all, 127, 127, 64);
        }
    } else {
        song_demo(song_a);
        if (scenario == 3 || scenario == 5) {
            const uint8_t demo[kChannels] = {12, 80, 64, 72, 0, 0};
            reverb_on(song_a, demo, 84, 72, 48);
        }
        if (scenario == 5) { song_a.drum[0][DpDecay] = 0; song_a.fx_send[0] = 127; }
        if (sc == 8) {
            song_a.drum[0][DpX] = 100;
            song_a.drum[1][DpB] = 64;
            for (int v = 1; v < kDrumTracks; ++v) song_a.drum[v][DpX] = 31;
        }
    }
    song_clear(song_b);
    eng->init(&song_a, &song_b, 0, in);
    in.audio_connected[0] = in.audio_connected[1] = (scenario == 2 || scenario == 4);
    if (scenario >= 1 && scenario <= 4) eng->commands().push(Command{CmdType::PlayPattern, 0, 0, 0});
    if (scenario == 5) eng->commands().push(Command{CmdType::Preview, 0, 36, 127});
}

extern "C" void bench_run(int n) {
    for (int i = 0; i < n; ++i) {
        in.audio[0] = static_cast<int16_t>((i * 37) & 2047) - 1024;
        in.audio[1] = static_cast<int16_t>((i * 53) & 2047) - 1024;
        eng->process(in, out);
        sink = out.audio[0] + out.audio[1];
    }
}

extern "C" void bench_one() { bench_run(1); }

// ---- isolated pieces for tools/cpu_bench/breakdown.py ----------------------
// One drum voice on its own: code = engine | a << 8 | b << 16 | decay << 24.
static DrumVoice vb;
static uint8_t vb_note = 48;
extern "C" void voice_init(int code) {
    init_tables();
    vb = DrumVoice();
    vb.seed(12345);
    TriggerArgs t{};
    t.engine = static_cast<uint8_t>(code & 7);
    t.a = static_cast<uint8_t>((code >> 8) & 127);
    t.b = static_cast<uint8_t>((code >> 16) & 127);
    t.decay = static_cast<uint8_t>((code >> 24) & 127);
    t.note = vb_note;
    t.vel = 127;
    vb.trigger(t);
}
extern "C" void voice_note(int n) { vb_note = static_cast<uint8_t>(n); }
extern "C" void voice_run(int n) { for (int i = 0; i < n; ++i) sink = vb.render(); }
extern "C" void voice_one() { sink = vb.render(); }
extern "C" int voice_active() { return vb.active(); }

// The mixer on its own: code bits 0-5 live channels, 6 drive on, 7 tone LP,
// 8 tone HP, 9 master filter on, 10 input VCAs (CV gate, gate low = decaying).
static Mixer mx;
static Song mx_song;
static uint32_t mx_mask;
extern "C" void mix_init(int code) {
    init_tables();
    mx = Mixer();
    song_clear(mx_song);
    for (int ch = 0; ch < kChannels; ++ch) {
        mx_song.mix[ch][MpDrive] = (code & 64) ? 60 : 0;
        mx_song.mix[ch][MpTone] = (code & 128) ? 30 : (code & 256) ? 100 : 64;
        mx_song.mix[ch][MpVca] = 60;
        if (ch >= kDrumTracks) mx_song.mix[ch][MpVcaSrc] = (code & 1024) ? VcaCvGate : VcaOpen;
    }
    mx_song.master[MsFilter] = (code & 512) ? 20 : 64;
    mx.update(mx_song, 0);
    mx_mask = static_cast<uint32_t>(code & 63);
}
extern "C" void mix_one() {
    static int i = 0;
    int32_t ch[kChannels];
    for (int c = 0; c < kChannels; ++c) ch[c] = static_cast<int32_t>(((i * (37 + 16 * c)) & 32767) - 16384);
    ++i;
    int16_t l, r;
    mx.process(ch, mx_mask, l, r);
    sink = l + r;
}
extern "C" void mix_gen() {   // harness cost of mix_one without the mixer
    static int i = 0;
    int32_t ch[kChannels];
    for (int c = 0; c < kChannels; ++c) ch[c] = static_cast<int32_t>(((i * (37 + 16 * c)) & 32767) - 16384);
    ++i;
    sink = ch[0] + ch[5];
}
extern "C" void mix_update_one() { static int k = 0; mx.update_step(mx_song, 0); ++k; }
extern "C" void softclip_one() { static int32_t x = 1; x = x * 1103515245 + 12345; sink = softclip(x >> 16); }

extern "C" void _start() { bench_init(0); bench_run(1); while (1) {} }
