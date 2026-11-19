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
extern "C" void bench_init(int scenario) {
    eng = new (eng_mem) TrackerEngine();
    if (scenario == 2) {
        song_clear(song_a);
        song_a.set_tempo(300);
        for (int v = 0; v < kDrumTracks; ++v) {
            song_a.drum[v][DpEngine] = (v & 1) ? EngModal : EngSnare;
            song_a.drum[v][DpDecay] = 127;
            song_a.drum[v][DpB] = 100;
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
    } else {
        song_demo(song_a);
    }
    song_clear(song_b);
    eng->init(&song_a, &song_b, 0, in);
    in.audio_connected[0] = in.audio_connected[1] = (scenario == 2);
    if (scenario >= 1) eng->commands().push(Command{CmdType::PlayPattern, 0, 0, 0});
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

extern "C" void _start() { bench_init(0); bench_run(1); while (1) {} }
