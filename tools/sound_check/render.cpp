// Renders fixed scenarios through the host engine and writes raw outputs, so
// two firmware versions can be compared sample by sample (compare.py).
// Build: g++ -std=c++17 -O2 -I <src> tools/sound_check/render.cpp -o render
// Run:   ./render <outdir>
// Files: <name>.audio (int16 L,R per sample), <name>.cv (int32 mv0,mv1, uint8 gates per sample),
//        <name>.trig (text: sample voice), <name>.start (sample where the hit's first
//        output sample appears), <name>.stem (int32 per drum voice per sample, pre-mixer).
// SPDX-License-Identifier: MIT
#define TRK_SOUND_CHECK 1
#include "engine/engine.h"
#include <cstdio>
#include <new>
#include <string>
#include <vector>

using namespace trk;

static Song song_a, song_b;
alignas(8) static unsigned char eng_mem[sizeof(TrackerEngine)];

static void render(const std::string& dir, const std::string& name, int seconds, bool inputs = false) {
    TrackerEngine& eng = *new (eng_mem) TrackerEngine();   // fresh state per scenario
    EngineIn in;
    in.audio_connected[0] = in.audio_connected[1] = inputs;
    eng.init(&song_a, &song_b, 0, in);
    eng.commands().push(Command{CmdType::PlayPattern, 0, 0, 0});
    FILE* fa = std::fopen((dir + "/" + name + ".audio").c_str(), "wb");
    FILE* fc = std::fopen((dir + "/" + name + ".cv").c_str(), "wb");
    FILE* ft = std::fopen((dir + "/" + name + ".trig").c_str(), "w");
    FILE* fs = std::fopen((dir + "/" + name + ".stem").c_str(), "wb");
    FILE* fst = std::fopen((dir + "/" + name + ".start").c_str(), "w");
    uint32_t last_start[kDrumTracks] = {};
    uint32_t last[kDrumTracks] = {};
    EngineOut out;
    const int n = seconds * kSampleRate;
    for (int i = 0; i < n; ++i) {
        if (inputs) {
            in.audio[0] = static_cast<int16_t>(((i * 37) & 2047) - 1024);
            in.audio[1] = static_cast<int16_t>(((i * 53) & 2047) - 1024);
        }
        eng.process(in, out);
        std::fwrite(out.audio, 2, 2, fa);
        int32_t cv[2] = {out.cv_mv[0], out.cv_mv[1]};
        uint8_t g = static_cast<uint8_t>((out.gate[0] ? 1 : 0) | (out.gate[1] ? 2 : 0));
        std::fwrite(cv, 4, 2, fc);
        std::fwrite(&g, 1, 1, fc);
        int32_t st[kDrumTracks];
        for (int v = 0; v < kDrumTracks; ++v) st[v] = eng.stem(v);
        std::fwrite(st, 4, kDrumTracks, fs);
        for (int v = 0; v < kDrumTracks; ++v) {
            const uint32_t c = eng.status().trig_count[v];
            if (c != last[v]) { std::fprintf(ft, "%d %d\n", i, v); last[v] = c; }
            const uint32_t st0 = eng.voice_starts(v);
            if (st0 != last_start[v]) { std::fprintf(fst, "%d %d\n", i, v); last_start[v] = st0; }
        }
    }
    std::fclose(fa); std::fclose(fc); std::fclose(ft); std::fclose(fs); std::fclose(fst);
}

// One drum hit on track 0 (pattern 0, row 0), long pattern so it rings out.
static void hit_song(int engine, int a, int b, int decay, int note, bool hot) {
    song_clear(song_a);
    song_a.set_tempo(60);
    song_a.pat_len[0] = 64;
    song_a.drum[0][DpEngine] = static_cast<uint8_t>(engine);
    song_a.drum[0][DpA] = static_cast<uint8_t>(a);
    song_a.drum[0][DpB] = static_cast<uint8_t>(b);
    song_a.drum[0][DpDecay] = static_cast<uint8_t>(decay);
    set_cell(song_a, 0, 0, 0, static_cast<uint8_t>(note), 127, FxNone, 0);
    if (hot) {
        song_a.mix[0][MpDrive] = 100;
        song_a.mix[0][MpTone] = 30;
        song_a.mix[0][MpLevel] = 127;
        song_a.master[MsDrive] = 80;
        song_a.master[MsFilter] = 30;
    }
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    song_clear(song_b);
    // demo song
    song_demo(song_a);
    render(dir, "demo", 20);
    song_demo(song_a);
    render(dir, "demo_inputs", 10, true);
    // per-engine hits
    struct V { const char* n; int e, a, b, d, note; };
    const V vs[] = {
        {"kick", EngKick, 64, 0, 70, 36}, {"kick_drive", EngKick, 90, 80, 90, 33},
        {"pluck", EngPluck, 64, 64, 90, 48}, {"pluck_low", EngPluck, 30, 20, 127, 28},
        {"noise", EngNoise, 64, 64, 70, 60}, {"noise_sweep", EngNoise, 100, 120, 100, 72},
        {"metal", EngMetal, 64, 64, 80, 60}, {"metal_hi", EngMetal, 120, 127, 100, 84},
        {"modal_membrane", EngModal, 0, 64, 90, 48}, {"modal_marimba", EngModal, 16, 64, 100, 60},
        {"modal_gong", EngModal, 112, 100, 127, 40},
        {"snare", EngSnare, 64, 30, 70, 50}, {"clap", EngSnare, 64, 100, 70, 50},
    };
    for (const V& v : vs) {
        hit_song(v.e, v.a, v.b, v.d, v.note, false);
        render(dir, std::string("hit_") + v.n, 7);
        hit_song(v.e, v.a, v.b, v.d, v.note, true);
        render(dir, std::string("hot_") + v.n, 7);
    }
    // dense: the CPU bench's pathological pattern
    song_clear(song_a);
    song_a.set_tempo(300);
    for (int v = 0; v < kDrumTracks; ++v) {
        song_a.drum[v][DpEngine] = (v & 1) ? EngModal : EngSnare;
        song_a.drum[v][DpDecay] = 127;
        song_a.drum[v][DpB] = 100;
    }
    for (int r = 0; r < 16; ++r) {
        for (int t = 0; t < kDrumTracks; ++t) set_cell(song_a, 0, t, r, static_cast<uint8_t>(40 + t * 7), 127, FxRatchet, 4);
        set_cell(song_a, 0, 4, r, static_cast<uint8_t>(40 + r), 90, FxSlide, 30);
        set_cell(song_a, 0, 5, r, static_cast<uint8_t>(50 + r), 90, FxRatchet, 3);
        set_cell(song_a, 0, 6, r, 60, 100, FxRatchet, 4);
        set_cell(song_a, 0, 7, r, 60, 80, FxRatchet, 8);
    }
    song_a.mix[4][MpVcaSrc] = VcaInTrack;
    song_a.mix[5][MpVcaSrc] = VcaCvGate;
    for (int ch = 0; ch < kChannels; ++ch) { song_a.mix[ch][MpDrive] = 60; song_a.mix[ch][MpTone] = 30; song_a.mix[ch][MpVca] = 60; }
    song_a.master[MsFilter] = 20;
    render(dir, "dense", 6, true);
    return 0;
}
