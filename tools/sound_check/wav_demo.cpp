// Renders the factory demo song through the host engine to WAV files:
// dry (as stored) and with the lo-fi reverb sends turned up. Since 0.4.3
// also single-voice demos of the kick click, the snare/clap morph and grit.
// Build: g++ -std=c++17 -O2 -I src tools/sound_check/wav_demo.cpp -o wav_demo
// Run:   ./wav_demo docs/audio
// The 12-bit DAC codes are written as 16-bit PCM (code << 4), 48 kHz stereo.
// SPDX-License-Identifier: MIT
#include "engine/engine.h"
#include <cmath>
#include <cstdio>
#include <new>
#include <string>
#include <vector>

using namespace trk;

static Song song_a, song_b;
alignas(8) static unsigned char eng_mem[sizeof(TrackerEngine)];

static void put32(FILE* f, uint32_t v) { std::fwrite(&v, 4, 1, f); }
static void put16(FILE* f, uint16_t v) { std::fwrite(&v, 2, 1, f); }

static void write_wav(const std::string& path, const std::vector<int16_t>& pcm) {
    FILE* f = std::fopen(path.c_str(), "wb");
    const uint32_t bytes = static_cast<uint32_t>(pcm.size() * 2);
    std::fwrite("RIFF", 1, 4, f); put32(f, 36 + bytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, 1); put16(f, 2); put32(f, kSampleRate);
    put32(f, kSampleRate * 4); put16(f, 4); put16(f, 16);
    std::fwrite("data", 1, 4, f); put32(f, bytes);
    std::fwrite(pcm.data(), 2, pcm.size(), f);
    std::fclose(f);
}

// Plays the song for `play` seconds, stops, then renders `tail` more seconds.
static std::vector<int16_t> render(double play, double tail, double* corr = nullptr, int* stop_ms = nullptr) {
    TrackerEngine& eng = *new (eng_mem) TrackerEngine();
    EngineIn in;
    eng.init(&song_a, &song_b, 0, in);
    eng.commands().push(Command{CmdType::PlaySong, 0, 0, 0});
    std::vector<int16_t> pcm;
    const int n_play = static_cast<int>(play * kSampleRate), n = n_play + static_cast<int>(tail * kSampleRate);
    double sl = 0, sr = 0, slr = 0;
    int stopped_at = -1;
    EngineOut out;
    for (int i = 0; i < n; ++i) {
        if (i == n_play) eng.commands().push(Command{CmdType::Stop, 0, 0, 0});
        eng.process(in, out);
        pcm.push_back(static_cast<int16_t>(out.audio[0] * 16));
        pcm.push_back(static_cast<int16_t>(out.audio[1] * 16));
        sl += double(out.audio[0]) * out.audio[0]; sr += double(out.audio[1]) * out.audio[1];
        slr += double(out.audio[0]) * out.audio[1];
        if (i > n_play && stopped_at < 0 && !eng.mixer().reverb_running()) stopped_at = i - n_play;
    }
    if (corr) *corr = slr / std::sqrt(sl * sr + 1e-9);
    if (stop_ms) *stop_ms = stopped_at < 0 ? -1 : stopped_at * 1000 / kSampleRate;
    return pcm;
}

// One voice, hit after hit (mono into both channels). Grit runs after the
// voice exactly as in the engine (GritState on the voice output).
struct Hit { uint8_t engine, note, decay, a, b, x; double len; };
static std::vector<int16_t> render_hits(const std::vector<Hit>& hits) {
    std::vector<int16_t> pcm;
    for (const Hit& h : hits) {
        DrumVoice v;
        v.trigger(TriggerArgs{h.engine, h.note, 110, h.decay, h.a, h.b, h.x});
        GritState g;
        const bool grit = h.engine != EngKick && h.x != 0;
        if (grit) g.set(h.x);
        const int n = static_cast<int>(h.len * kSampleRate);
        for (int i = 0; i < n; ++i) {
            int32_t y = v.render();
            if (grit) y = g.process(y);
            y = y > 32767 ? 32767 : y < -32768 ? -32768 : y;
            const int16_t o = static_cast<int16_t>(y * 3 / 4);
            pcm.push_back(o); pcm.push_back(o);
        }
    }
    return pcm;
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    song_clear(song_b);
    song_demo(song_a);
    write_wav(dir + "/demo_dry.wav", render(16.0, 2.0));
    // Same song with the sends up: a little on the kick, more on snare,
    // hats and the plucked bass, the VCO inputs dry (the demo leaves them
    // unplugged anyway).
    song_demo(song_a);
    const uint8_t sends[kChannels] = {12, 80, 64, 72, 0, 0};
    for (int c = 0; c < kChannels; ++c) song_a.fx_send[c] = sends[c];
    song_a.master[MsRvReturn] = 100;
    song_a.master[MsRvSize] = 84;
    song_a.master[MsRvDecay] = 72;
    song_a.master[MsRvTone] = 52;
    song_a.master[MsRvLofi] = 48;
    double corr = 0; int stop_ms = 0;
    write_wav(dir + "/demo_reverb.wav", render(16.0, 4.0, &corr, &stop_ms));
    std::printf("reverb version: L/R correlation %.3f, tail stopped %d ms after the song stopped\n", corr, stop_ms);
    // Kick: click 0 (the old kick), 30, 60, 90, 127, twice each.
    std::vector<Hit> k;
    for (uint8_t x : {0, 0, 30, 30, 60, 60, 90, 90, 127, 127}) k.push_back(Hit{EngKick, 36, 56, 64, 40, x, 0.5});
    write_wav(dir + "/kick_click.wav", render_hits(k));
    // Snare -> clap: B 30..100 in steps of 5 (morph zone 44..84).
    std::vector<Hit> sn;
    for (int b = 30; b <= 100; b += 5) sn.push_back(Hit{EngSnare, 50, 56, 64, static_cast<uint8_t>(b), 0, 0.42});
    write_wav(dir + "/snare_clap_morph.wav", render_hits(sn));
    // Grit: Metal, Pluck, Modal, each with grit 0 (as before), 32, 64, 96, 127.
    std::vector<Hit> gr;
    const Hit base[3] = {{EngMetal, 62, 70, 40, 64, 0, 0.6}, {EngPluck, 45, 76, 70, 40, 0, 0.6}, {EngModal, 60, 72, 64, 64, 0, 0.6}};
    for (const Hit& b : base)
        for (uint8_t x : {0, 32, 64, 96, 127}) { Hit h = b; h.x = x; gr.push_back(h); }
    write_wav(dir + "/grit_demo.wav", render_hits(gr));
    std::printf("wrote kick_click.wav, snare_clap_morph.wav, grit_demo.wav\n");
    return 0;
}
