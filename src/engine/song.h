// Song data model: patterns of 8 tracks x up to 64 rows, an arrangement,
// voice parameters and the mixer. Everything a song needs to play standalone
// lives in one flat, padding-free struct so it can be copied into flash as is.
// Every user-facing value is 7-bit (0..127) so it travels in MIDI SysEx
// without re-encoding; only the tempo needs two bytes.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace trk {

constexpr int kDrumTracks = 4;
constexpr int kCvTracks = 2;
constexpr int kInputs = 2;
constexpr int kInTracks = kInputs;                 // audio-in VCA tracks (v3)
constexpr int kTracks = kDrumTracks + kCvTracks + kInTracks;   // 0-3 drums, 4-5 CV/gate, 6-7 IN1/IN2
constexpr int kFirstCvTrack = kDrumTracks;                     // 4
constexpr int kFirstInTrack = kDrumTracks + kCvTracks;         // 6
constexpr int kChannels = kDrumTracks + kInputs;   // mixer: 4 drums + 2 audio ins
constexpr int kPatterns = 16;
constexpr int kRows = 64;
constexpr int kSongEntries = 32;
constexpr int kNameLen = 12;
constexpr int kParamsPerVoice = 8;
constexpr int kParamsPerChannel = 8;
constexpr int kTempoMin = 30;
constexpr int kTempoMax = 300;
constexpr int kSwingMin = 50;
constexpr int kSwingMax = 75;

// Note column.
constexpr uint8_t kNoteEmpty = 0;
constexpr uint8_t kNoteOff = 127;   // CV tracks: gate off. Drums: choke. IN tracks: VCA off.
// 1..126 = MIDI note number (IN tracks: any note = trigger the VCA).

// Effect column commands (ASCII letters, shown as-is in the editor).
enum Fx : uint8_t {
    FxNone = 0,
    FxParamA = 'A',      // drum: lock param A for this hit
    FxParamB = 'B',      // drum: lock param B for this hit
    FxDecay = 'D',       // drum: lock decay for this hit; IN: VCA decay for this hit
    FxGate = 'G',        // CV/IN: gate (hold) length in 1/16 row (0 = tie)
    FxSlide = 'S',       // CV: glide time for this note
    FxRatchet = 'R',     // both: 2..8 hits inside the row
    FxProb = 'P',        // both: chance 0..100 %
    FxDelay = 'K',       // both: delay the row's event by n/16 row ("kick" late)
};

// One step: note, velocity and one effect (letter + value).
// (Firmware 0.3.0 had a second effect column; its songs are converted on load.)
struct Cell {
    uint8_t note;
    uint8_t vel;   // 0 = default (drums 100, CV/IN: full VCA level)
    uint8_t fx;
    uint8_t fxv;
};

// Value of effect f in this step, or -1 if absent.
inline int fx_get(const Cell& c, uint8_t f) { return c.fx == f ? c.fxv : -1; }
inline bool cell_has_fx(const Cell& c) { return c.fx != FxNone; }

enum DrumEngine : uint8_t {
    EngKick = 0,     // sine + pitch sweep + drive
    EngPluck = 1,    // Karplus-Strong string
    EngNoise = 2,    // resonant band-pass noise with sweep
    EngMetal = 3,    // 2-op FM, inharmonic ratios
    EngModal = 4,    // 3-mode resonator bank (bars, bells, membranes...)
    EngSnare = 5,    // body sine + noise, clap bursts
    EngCount = 6,
};

// Drum voice parameters.
enum DrumParam { DpEngine = 0, DpDecay, DpA, DpB, DpTune, DpLevelVel, DpR6, DpR7 };
// CV track parameters.
enum CvParam { CpGate = 0, CpGlide, CpTranspose, CpR3, CpR4, CpR5, CpR6, CpR7 };
// Mixer channel parameters (channels 0-3 drums, 4-5 audio inputs).
// Inputs only: MpVca = VCA decay (v2: 0 = VCA off), MpVcaSrc = what opens the
// VCA (v3), MpInGate = IN-track hold length in 1/16 row (0 = until next/OFF).
enum MixParam { MpLevel = 0, MpPan, MpDrive, MpTone, MpVca, MpMute, MpVcaSrc, MpInGate };
// MpVcaSrc values. 0 is what every v2 song has, and keeps the v2 behaviour.
enum VcaSrc : uint8_t {
    VcaAuto = 0,      // v2: follow the CV gate if decay > 0, else open
    VcaOpen = 1,      // always open (input passes through)
    VcaCvGate = 2,    // follow the CV track's gate (CV velocity = level)
    VcaInTrack = 3,   // triggered by the IN track (velocity = level)
    VcaSrcCount = 4,
};
// Master parameters.
enum MasterParam { MsLevel = 0, MsDrive, MsFilter, MsR3, MsR4, MsR5, MsR6, MsR7 };

enum Macro { MacroDecay = 0, MacroColour = 1, MacroFilter = 2 };

struct SongEntry {
    uint8_t pattern;
    uint8_t repeats;   // 1..16
};

// Everything except the pattern cells (212 bytes, sent in one SysEx message).
struct SongHeader {
    char name[kNameLen];
    uint8_t tempo_hi;      // tempo = hi*128 + lo, BPM 30..300
    uint8_t tempo_lo;
    uint8_t swing;         // 50..75 %
    uint8_t song_len;      // 1..32 entries
    uint8_t cv_zero_note;  // MIDI note that gives 0 V on the CV outs (default 36 = C2)
    // Panel macro routing (protocol v2). Bit v set = drum voice v is NOT
    // affected by that macro, so 0 (old songs, v1 flash slots) = all voices.
    uint8_t macro_excl[2];  // [MacroDecay] = Main knob, [MacroColour] = X knob
    // Macro routing for the CV and IN modules (v3). Bit set = routed (enabled),
    // bit 0/1 = CV1/CV2, bit 2/3 = IN1/IN2. 0 in v2 songs = not routed, as before.
    // Main: CV gate length / IN VCA decay. X: CV glide / IN drive.
    uint8_t macro_cvin[2];
    uint8_t reserved[7];
    uint8_t pat_len[kPatterns];             // 1..64
    SongEntry entries[kSongEntries];
    uint8_t drum[kDrumTracks][kParamsPerVoice];
    uint8_t cv[kCvTracks][kParamsPerVoice];
    uint8_t mix[kChannels][kParamsPerChannel];
    uint8_t master[kParamsPerChannel];

    int tempo() const { return tempo_hi * 128 + tempo_lo; }
    void set_tempo(int bpm) {
        if (bpm < kTempoMin) bpm = kTempoMin;
        if (bpm > kTempoMax) bpm = kTempoMax;
        tempo_hi = static_cast<uint8_t>(bpm / 128);
        tempo_lo = static_cast<uint8_t>(bpm % 128);
    }
};

// True when the PERFORM macro m (MacroDecay / MacroColour) acts on drum voice v.
inline bool macro_on(const SongHeader& s, int m, int v) { return ((s.macro_excl[m] >> v) & 1) == 0; }
// True when macro m acts on CV track c (0/1).
inline bool macro_on_cv(const SongHeader& s, int m, int c) { return ((s.macro_cvin[m] >> c) & 1) != 0; }
// True when macro m acts on input i (0/1).
inline bool macro_on_in(const SongHeader& s, int m, int i) { return ((s.macro_cvin[m] >> (2 + i)) & 1) != 0; }

// What opens input i's VCA.
inline VcaSrc vca_source(const SongHeader& s, int i) {
    const uint8_t* m = s.mix[kDrumTracks + i];
    if (m[MpVcaSrc] == VcaAuto || m[MpVcaSrc] >= VcaSrcCount) return m[MpVca] ? VcaCvGate : VcaOpen;
    return static_cast<VcaSrc>(m[MpVcaSrc]);
}

struct Song : SongHeader {
    Cell cells[kPatterns][kTracks][kRows];
};

constexpr int kCellBytes = 4;
static_assert(sizeof(Cell) == kCellBytes, "Cell must be 4 bytes");
static_assert(sizeof(Song) % 4 == 0, "Song should be word-sized");
constexpr int kSongHeaderBytes = static_cast<int>(sizeof(SongHeader));
static_assert(sizeof(SongHeader) == 212, "header layout is part of the protocol");
static_assert(sizeof(Song) == sizeof(SongHeader) + kPatterns * kTracks * kRows * kCellBytes, "no padding");

// v2 layout (6 tracks, 4-byte cells), only for migrating old flash slots.
constexpr int kV2Tracks = 6;
constexpr int kV2CellBytes = 4;
constexpr int kV2SongBytes = kSongHeaderBytes + kPatterns * kV2Tracks * kRows * kV2CellBytes;   // 24788
static_assert(sizeof(Song) == 32980, "8 tracks x 4-byte cells");
// Firmware 0.3.0 layout (8 tracks, 6-byte cells with a second effect), only for migrating.
constexpr int kV030CellBytes = 6;
constexpr int kV030SongBytes = kSongHeaderBytes + kPatterns * kTracks * kRows * kV030CellBytes;   // 49364

inline void header_sanitize(SongHeader& s);

inline uint8_t c7(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 127 ? 127 : v)); }

// Clamp every field into its legal range (used after loading from flash or USB).
inline void header_sanitize(SongHeader& s) {
    for (char& c : s.name) {
        if (c != 0 && (c < 32 || c > 126)) c = ' ';
    }
    s.set_tempo(s.tempo());
    if (s.swing < kSwingMin) s.swing = kSwingMin;
    if (s.swing > kSwingMax) s.swing = kSwingMax;
    if (s.song_len < 1) s.song_len = 1;
    if (s.song_len > kSongEntries) s.song_len = kSongEntries;
    if (s.cv_zero_note > 126) s.cv_zero_note = 36;
    for (uint8_t& v : s.macro_excl) v &= 0x0F;
    for (uint8_t& v : s.macro_cvin) v &= 0x0F;
    for (uint8_t& v : s.reserved) v = 0;
    for (uint8_t& l : s.pat_len) {
        if (l < 1) l = 1;
        if (l > kRows) l = kRows;
    }
    for (SongEntry& e : s.entries) {
        if (e.pattern >= kPatterns) e.pattern = 0;
        if (e.repeats < 1) e.repeats = 1;
        if (e.repeats > 16) e.repeats = 16;
    }
    for (auto& d : s.drum) {
        for (uint8_t& v : d) v = c7(v);
        if (d[DpEngine] >= EngCount) d[DpEngine] = EngKick;
    }
    for (auto& c : s.cv) for (uint8_t& v : c) v = c7(v);
    for (auto& m : s.mix) for (uint8_t& v : m) v = c7(v);
    for (int i = 0; i < kInputs; ++i) {
        uint8_t* m = s.mix[kDrumTracks + i];
        if (m[MpVcaSrc] >= VcaSrcCount) m[MpVcaSrc] = VcaAuto;
        if (m[MpInGate] > 64) m[MpInGate] = 64;
    }
    for (uint8_t& v : s.master) v = c7(v);
}

inline void song_sanitize(Song& s) {
    header_sanitize(s);
    for (auto& p : s.cells) for (auto& t : p) for (Cell& c : t) {
        c.note = c7(c.note); c.vel = c7(c.vel); c.fx = c7(c.fx); c.fxv = c7(c.fxv);
    }
}

inline void song_clear(Song& s) {
    std::memset(&s, 0, sizeof(Song));
    std::memcpy(s.name, "NEW SONG", 8);
    s.set_tempo(120);
    s.swing = 50;
    s.song_len = 1;
    s.cv_zero_note = 36;
    for (uint8_t& l : s.pat_len) l = 16;
    for (int i = 0; i < kSongEntries; ++i) {
        s.entries[i].pattern = static_cast<uint8_t>(i < kPatterns ? i : 0);
        s.entries[i].repeats = 1;
    }
    const uint8_t engines[kDrumTracks] = {EngKick, EngSnare, EngMetal, EngPluck};
    const uint8_t decays[kDrumTracks] = {62, 48, 40, 80};
    for (int v = 0; v < kDrumTracks; ++v) {
        s.drum[v][DpEngine] = engines[v];
        s.drum[v][DpDecay] = decays[v];
        s.drum[v][DpA] = 64;
        s.drum[v][DpB] = 32;
        s.drum[v][DpTune] = 64;
        s.drum[v][DpLevelVel] = 100;  // velocity sensitivity
    }
    for (int c = 0; c < kCvTracks; ++c) {
        s.cv[c][CpGate] = 8;        // half a row
        s.cv[c][CpGlide] = 0;
        s.cv[c][CpTranspose] = 64;  // 64 = 0 semitones
    }
    const uint8_t pans[kChannels] = {64, 52, 80, 40, 44, 84};
    for (int ch = 0; ch < kChannels; ++ch) {
        s.mix[ch][MpLevel] = 110;
        s.mix[ch][MpPan] = pans[ch];
        s.mix[ch][MpDrive] = 0;
        s.mix[ch][MpTone] = 64;
        s.mix[ch][MpVca] = 0;
        s.mix[ch][MpMute] = 0;
    }
    for (int i = 0; i < kInputs; ++i) {
        s.mix[kDrumTracks + i][MpVcaSrc] = VcaAuto;   // open until the IN track or a decay is set
        s.mix[kDrumTracks + i][MpInGate] = 8;         // half a row
    }
    // New songs: Main/X also act on the CV and IN modules.
    s.macro_cvin[MacroDecay] = 0x0F;
    s.macro_cvin[MacroColour] = 0x0F;
    s.master[MsLevel] = 120;
    s.master[MsDrive] = 10;
    s.master[MsFilter] = 64;
}

inline void set_cell(Song& s, int p, int t, int r, uint8_t note, uint8_t vel = 0,
                     uint8_t fx = 0, uint8_t fxv = 0) {
    s.cells[p][t][r] = Cell{note, vel, fx, fxv};
}

// Convert a firmware 0.3.0 song image (kV030SongBytes, 8 tracks x 6-byte cells).
// The header is unchanged; the second effect column is dropped, except where
// the first one is empty (then the second effect moves into it).
inline void song_from_v030(const uint8_t* v3, Song& out) {
    std::memset(&out, 0, sizeof(Song));
    std::memcpy(static_cast<SongHeader*>(&out), v3, kSongHeaderBytes);
    const uint8_t* cells = v3 + kSongHeaderBytes;
    for (int p = 0; p < kPatterns; ++p)
        for (int t = 0; t < kTracks; ++t)
            for (int r = 0; r < kRows; ++r) {
                const uint8_t* c = cells + ((p * kTracks + t) * kRows + r) * kV030CellBytes;
                const bool use2 = c[2] == FxNone && c[4] != FxNone;
                out.cells[p][t][r] = Cell{c[0], c[1], use2 ? c[4] : c[2], use2 ? c[5] : c[3]};
            }
    song_sanitize(out);
}

// Convert a v2 song image (kV2SongBytes, 6 tracks x 4-byte cells).
// The IN VCAs keep following the CV gates (VcaAuto) and the new macro
// routing is off, so an old song plays exactly as before.
inline void song_from_v2(const uint8_t* v2, Song& out) {
    std::memset(&out, 0, sizeof(Song));
    std::memcpy(static_cast<SongHeader*>(&out), v2, kSongHeaderBytes);
    out.macro_cvin[0] = out.macro_cvin[1] = 0;
    for (int i = 0; i < kInputs; ++i) {
        out.mix[kDrumTracks + i][MpVcaSrc] = VcaAuto;
        out.mix[kDrumTracks + i][MpInGate] = 8;
    }
    const uint8_t* cells = v2 + kSongHeaderBytes;
    for (int p = 0; p < kPatterns; ++p)
        for (int t = 0; t < kV2Tracks; ++t)
            for (int r = 0; r < kRows; ++r) {
                const uint8_t* c = cells + ((p * kV2Tracks + t) * kRows + r) * kV2CellBytes;
                out.cells[p][t][r] = Cell{c[0], c[1], c[2], c[3]};
            }
    song_sanitize(out);
}

// Factory demo so a fresh card makes sound straight away.
inline void song_demo(Song& s) {
    song_clear(s);
    std::memset(s.name, 0, sizeof s.name);
    std::memcpy(s.name, "DEMO", 4);
    s.set_tempo(118);
    s.swing = 56;
    s.song_len = 4;
    s.entries[0] = {0, 2};
    s.entries[1] = {1, 2};
    s.entries[2] = {0, 1};
    s.entries[3] = {2, 1};
    for (int p = 0; p < 3; ++p) {
        for (int r = 0; r < 16; ++r) {
            if (r % 4 == 0) set_cell(s, p, 0, r, 36, 110);
            if (r % 8 == 4) set_cell(s, p, 1, r, 50, 100);
            if (r % 2 == 0) set_cell(s, p, 2, r, 80, r % 4 == 2 ? 100 : 50);
        }
    }
    // Pattern 0: plucked bass line on track 4, VCO lines.
    const uint8_t pluck[16] = {48, 0, 0, 55, 0, 0, 51, 0, 48, 0, 58, 0, 0, 55, 0, 0};
    const uint8_t vco1[16] = {36, 0, 0, 0, 43, 0, 0, 0, 39, 0, 0, 0, 41, 0, 43, 0};
    const uint8_t vco2[16] = {60, 0, 63, 0, 67, 0, 63, 0, 60, 0, 70, 0, 67, 0, 63, 0};
    for (int r = 0; r < 16; ++r) {
        if (pluck[r]) set_cell(s, 0, 3, r, pluck[r], 96);
        if (vco1[r]) set_cell(s, 0, 4, r, vco1[r]);
        if (vco2[r]) set_cell(s, 0, 5, r, vco2[r]);
        if (vco1[r]) set_cell(s, 1, 4, r, static_cast<uint8_t>(vco1[r] + 5), 0, FxSlide, 20);
        if (vco2[r]) set_cell(s, 1, 5, r, static_cast<uint8_t>(vco2[r] + 5), 0, r == 4 ? FxRatchet : 0, r == 4 ? 3 : 0);
    }
    // Pattern 1: snare ghost notes, ratchet, probability hats.
    set_cell(s, 1, 1, 14, 50, 60, FxRatchet, 3);
    for (int r = 1; r < 16; r += 2) set_cell(s, 1, 2, r, 84, 40, FxProb, 50);
    set_cell(s, 1, 3, 0, 43, 110, FxDecay, 100);
    set_cell(s, 1, 3, 8, 46, 90);
    // Pattern 2: fill.
    for (int r = 8; r < 16; ++r) set_cell(s, 2, 1, r, static_cast<uint8_t>(45 + r), static_cast<uint8_t>(40 + r * 5));
    set_cell(s, 2, 4, 0, 36, 0, FxGate, 64);
    set_cell(s, 2, 5, 0, kNoteOff);
    set_cell(s, 2, 2, 15, 84, 70, FxRatchet, 4);
    // IN1 follows its own track (accents on the VCO), IN2 stays open.
    s.mix[kDrumTracks][MpVcaSrc] = VcaInTrack;
    s.mix[kDrumTracks][MpVca] = 40;
    for (int p = 0; p < 3; ++p)
        for (int r = 0; r < 16; r += 2)
            set_cell(s, p, kFirstInTrack, r, 60, r % 4 == 0 ? 127 : 70);
}

} // namespace trk
