// USB-MIDI SysEx protocol between the card and the web editor.
// See docs/PROTOCOL.md. Every message is  F0 7D 54 <cmd> <payload> F7
// (7D = MIDI "non-commercial" manufacturer ID, 54 = 'T' for Tracker).
// All payload bytes are 7-bit; the song format is designed so no
// re-encoding is ever needed. Host-testable: no Pico or TinyUSB includes.
// SPDX-License-Identifier: MIT
#pragma once

#include "engine.h"
#include "song.h"

#include <cstdint>
#include <cstring>

namespace trk {
namespace proto {

constexpr uint8_t kMfr = 0x7D;
constexpr uint8_t kDevice = 0x54;
// v2: SetMacroMask, Panel report. v3 (fw 0.3.0): 8 tracks, 6-byte cells (2 FX).
// v4 (fw 0.3.1): 8 tracks, 4-byte cells (one effect again); v3's 6-byte forms are still accepted.
// v5 (fw 0.3.2): fixed CV mapping C0 = 0 V (SetZeroNote is ignored), no CV velocity,
//                panel slot select over the occupied slots. Wire format = v4.
// v6 (fw 0.4.0): lo-fi reverb. SetMix param 8 = reverb send, SetMaster params
//                3..7 = reverb return/size/decay/tone/lo-fi, header bytes 21..26 =
//                sends, byte 27 = fx_ver (1), Status +1 byte (reverb return meter).
//                fw 0.4.1: per-step effect W (reverb send for one hit); same protocol,
//                older firmware simply ignores the letter.
//    v7 (fw 0.4.2): SetMaster param 8 = reverb return mute, 9 = master mute
//                (value >= 64 = muted). Live only: not stored in the song, off
//                at power-up. Status flags bit 32 / 64 report them.
//    v8 (fw 0.4.3): drum param 6 = Kick click / grit (other engines), song fx_ver 2;
//                the snare/clap morph needs no protocol change.
constexpr uint8_t kProtocolVersion = 8;
constexpr uint8_t kMasterRvMute = 8, kMasterMute = 9;
constexpr uint8_t kFwMajor = 0, kFwMinor = 4, kFwPatch = 3;
constexpr uint8_t kMixSend = 8;    // SetMix pseudo-parameter: reverb send (song header fx_send)
constexpr int kV030CellWire = 6;   // cell size in SetCell/SetCells from a 0.3.0 editor (second effect folded, see cell_from_wire)
constexpr int kSlots = 6;
constexpr int kChunkRows = 16;

enum Cmd : uint8_t {
    // web -> card
    Hello = 0x01,        // -> Info
    GetSong = 0x02,      // -> SongHeader, 512 x PatternChunk (16 pat x 8 trk x 4), DumpEnd
    Ping = 0x09,         // keep-alive; Status streams while pings arrive
    SetCell = 0x10,      // pat track row note vel fx fxv  (9 bytes from a 0.3.0 editor: see cell_from_wire)
    SetCells = 0x11,     // pat track row0 count cells[count*4]  (count*6 from a 0.3.0 editor: see cell_from_wire)
    SetPatLen = 0x12,    // pat len
    SetDrum = 0x13,      // voice param value
    SetCv = 0x14,        // track param value
    SetMix = 0x15,       // channel param value
    SetMaster = 0x16,    // param value
    SetEntry = 0x17,     // index pattern repeats
    SetSongLen = 0x18,   // len
    SetTempo = 0x19,     // hi lo
    SetSwing = 0x1A,     // percent
    SetName = 0x1B,      // 12 ASCII bytes
    SetZeroNote = 0x1C,  // note for 0 V
    SetMacroMask = 0x1D, // macro (0 Main, 1 X), drum bits 0..3 [, CV/IN bits 0..3 (v3)]
    SetHeader = 0x1F,    // raw song header (kSongHeaderBytes)
    Transport = 0x20,    // mode(0 stop, 1 song, 2 pattern) arg
    Cue = 0x21,          // entry (song mode) or pattern (pattern mode), at pattern end
    Preview = 0x22,      // track note vel
    NextPart = 0x23,
    Save = 0x30,         // slot -> Ack
    Load = 0x31,         // slot -> Ack (then GetSong to read it)
    InitSong = 0x32,     // kind (0 empty, 1 demo) -> Ack
    // card -> web
    Info = 0x41,
    SongHeader = 0x42,
    PatternChunk = 0x43,
    DumpEnd = 0x44,
    Status = 0x45,
    Panel = 0x46,        // knob/macro feedback, on change, max ~30/s (v2; 10 more bytes in v3)
    Ack = 0x50,          // op status slot
};

enum AckStatus : uint8_t { AckOk = 0, AckEmpty = 1, AckBusy = 2, AckError = 3 };

// Collects SysEx messages addressed to this card from a MIDI byte stream.
struct Parser {
    static constexpr int kMax = 300;
    uint8_t buf[kMax];
    int n = 0;
    bool in = false;
    bool overflow = false;

    // Returns true when buf[0..len) holds <cmd><payload> of a complete message.
    bool feed(uint8_t b, const uint8_t*& msg, int& len) {
        if (b == 0xF0) { in = true; n = 0; overflow = false; return false; }
        if (!in) return false;
        if (b == 0xF7) {
            in = false;
            if (overflow || n < 3 || buf[0] != kMfr || buf[1] != kDevice) return false;
            msg = buf + 2;
            len = n - 2;
            return true;
        }
        if (b & 0x80) {
            // Real-time bytes may interleave; any other status aborts the SysEx.
            if (b >= 0xF8) return false;
            in = false;
            return false;
        }
        if (n < kMax) buf[n++] = b; else overflow = true;
        return false;
    }
};

// What the protocol needs from the firmware (or from a test double).
class Host {
public:
    virtual ~Host() = default;
    virtual Song* live() = 0;
    virtual void send(const uint8_t* body, int len) = 0;   // body = <cmd><payload>, framing added by Sender
    virtual bool command(const Command& c) = 0;             // to the audio engine
    virtual void request_save(int slot) = 0;                // async; host replies with Ack
    virtual void request_load(int slot) = 0;
    virtual void request_init(int kind) = 0;
    virtual void info_fields(uint8_t& slot, uint8_t& valid_mask) = 0;
    virtual void pinged() = 0;
    virtual void hello() {}                                 // editor (re)connected
};

class Protocol {
public:
    explicit Protocol(Host& h) : host_(h) {}

    void feed(const uint8_t* data, int len) {
        for (int i = 0; i < len; ++i) {
            const uint8_t* m;
            int l;
            if (parser_.feed(data[i], m, l)) handle(m, l);
        }
    }

    uint32_t messages() const { return messages_; }
    uint32_t rejected() const { return rejected_; }

    void handle(const uint8_t* m, int len) {
        if (len < 1) return;
        ++messages_;
        const uint8_t cmd = m[0];
        const uint8_t* p = m + 1;
        const int n = len - 1;
        Song& s = *host_.live();
        switch (cmd) {
        case Hello: send_info(); host_.hello(); break;
        case GetSong: send_song(s); break;
        case Ping: host_.pinged(); break;
        case SetCell: {
            if ((n != 7 && n != 9) || !cell_ok(p[0], p[1], p[2])) return reject();
            s.cells[p[0]][p[1]][p[2]] = cell_from_wire(p + 3, n == 9, p[1]);
            break;
        }
        case SetCells: {
            if (n < 4 || !cell_ok(p[0], p[1], p[2])) return reject();
            const int count = p[3];
            if (count < 1 || p[2] + count > kRows) return reject();
            const int w = n == 4 + count * kCellBytes ? kCellBytes : (n == 4 + count * kV030CellWire ? kV030CellWire : 0);
            if (!w) return reject();
            for (int i = 0; i < count; ++i) {
                s.cells[p[0]][p[1]][p[2] + i] = cell_from_wire(p + 4 + i * w, w == kV030CellWire, p[1]);
            }
            break;
        }
        case SetPatLen:
            if (n != 2 || p[0] >= kPatterns || p[1] < 1 || p[1] > kRows) return reject();
            s.pat_len[p[0]] = p[1];
            break;
        case SetDrum:
            if (n != 3 || p[0] >= kDrumTracks || p[1] >= kParamsPerVoice) return reject();
            if (p[1] == DpEngine && p[2] >= EngCount) return reject();
            s.drum[p[0]][p[1]] = p[2];
            break;
        case SetCv:
            if (n != 3 || p[0] >= kCvTracks || p[1] >= kParamsPerVoice) return reject();
            s.cv[p[0]][p[1]] = p[2];
            break;
        case SetMix:
            if (n != 3 || p[0] >= kChannels || p[1] > kMixSend) return reject();
            if (p[1] == kMixSend) s.fx_send[p[0]] = p[2];
            else s.mix[p[0]][p[1]] = p[2];
            break;
        case SetMaster:
            if (n != 2 || p[0] > kMasterMute) return reject();
            if (p[0] >= kMasterRvMute) {   // v7: live mutes, sent to the audio core
                host_.command(Command{CmdType::SetMute, static_cast<uint8_t>(p[0] - kMasterRvMute), static_cast<uint8_t>(p[1] >= 64), 0});
                break;
            }
            s.master[p[0]] = p[1];
            break;
        case SetEntry:
            if (n != 3 || p[0] >= kSongEntries || p[1] >= kPatterns || p[2] < 1 || p[2] > 16) return reject();
            s.entries[p[0]] = SongEntry{p[1], p[2]};
            break;
        case SetSongLen:
            if (n != 1 || p[0] < 1 || p[0] > kSongEntries) return reject();
            s.song_len = p[0];
            break;
        case SetTempo:
            if (n != 2) return reject();
            s.set_tempo(p[0] * 128 + p[1]);
            break;
        case SetSwing:
            if (n != 1 || p[0] < kSwingMin || p[0] > kSwingMax) return reject();
            s.swing = p[0];
            break;
        case SetName:
            if (n != kNameLen) return reject();
            for (int i = 0; i < kNameLen; ++i) s.name[i] = (p[i] >= 32 && p[i] <= 126) ? static_cast<char>(p[i]) : (p[i] ? ' ' : 0);
            break;
        case SetZeroNote:   // 0.3.2: the CV mapping is fixed (C0 = 0 V); accepted and ignored
            if (n != 1 || p[0] > 126) return reject();
            s.cv_zero_note = kCvZeroNote;
            break;
        case SetMacroMask:
            if ((n != 2 && n != 3) || p[0] > MacroColour || p[1] > 0x0F || (n == 3 && p[2] > 0x0F)) return reject();
            s.macro_excl[p[0]] = static_cast<uint8_t>(~p[1] & 0x0F);
            if (n == 3) s.macro_cvin[p[0]] = p[2];
            break;
        case SetHeader: {
            if (n != kSongHeaderBytes) return reject();
            // Sanitise a copy so the audio core never sees an out-of-range value.
            trk::SongHeader h;
            std::memcpy(&h, p, kSongHeaderBytes);
            header_sanitize(h);
            static_cast<trk::SongHeader&>(s) = h;
            break;
        }
        case Transport:
            if (n != 2) return reject();
            if (p[0] == 0) host_.command(Command{CmdType::Stop, 0, 0, 0});
            else if (p[0] == 1) host_.command(Command{CmdType::PlaySong, p[1], 0, 0});
            else if (p[0] == 2) host_.command(Command{CmdType::PlayPattern, p[1], 0, 0});
            else return reject();
            break;
        case Cue:
            if (n != 1) return reject();
            host_.command(Command{CmdType::CueEntry, p[0], 0, 0});
            break;
        case Preview:
            if (n != 3 || p[0] >= kTracks) return reject();
            host_.command(Command{CmdType::Preview, p[0], p[1], p[2]});
            break;
        case NextPart:
            host_.command(Command{CmdType::NextPart, 0, 0, 0});
            break;
        case Save:
            if (n != 1 || p[0] >= kSlots) return reject();
            host_.request_save(p[0]);
            break;
        case Load:
            if (n != 1 || p[0] >= kSlots) return reject();
            host_.request_load(p[0]);
            break;
        case InitSong:
            if (n != 1) return reject();
            host_.request_init(p[0]);
            break;
        default:
            return reject();
        }
    }

    // --- card -> web ---------------------------------------------------------
    void send_info() {
        uint8_t slot = 0, mask = 0;
        host_.info_fields(slot, mask);
        const uint8_t b[] = {Info, kProtocolVersion, kFwMajor, kFwMinor, kFwPatch,
                             kTracks, kDrumTracks, kPatterns, kRows, kSlots, kSongEntries,
                             slot, mask};
        host_.send(b, sizeof(b));
    }

    void send_ack(uint8_t op, uint8_t status, uint8_t slot) {
        const uint8_t b[] = {Ack, op, status, slot};
        host_.send(b, sizeof(b));
    }

    void send_song(const Song& s) {
        uint8_t b[2 + kSongHeaderBytes];
        b[0] = SongHeader;
        std::memcpy(b + 1, static_cast<const trk::SongHeader*>(&s), kSongHeaderBytes);
        host_.send(b, 1 + kSongHeaderBytes);
        for (int p = 0; p < kPatterns; ++p) {
            for (int t = 0; t < kTracks; ++t) {
                for (int r0 = 0; r0 < kRows; r0 += kChunkRows) {
                    uint8_t c[5 + kChunkRows * kCellBytes];
                    c[0] = PatternChunk;
                    c[1] = static_cast<uint8_t>(p);
                    c[2] = static_cast<uint8_t>(t);
                    c[3] = static_cast<uint8_t>(r0);
                    c[4] = kChunkRows;
                    std::memcpy(c + 5, &s.cells[p][t][r0], kChunkRows * kCellBytes);
                    for (int i = 5; i < 5 + kChunkRows * kCellBytes; ++i) c[i] &= 0x7F;
                    host_.send(c, sizeof(c));
                }
            }
        }
        const uint8_t e[] = {DumpEnd, static_cast<uint8_t>(kPatterns), static_cast<uint8_t>(kTracks)};
        host_.send(e, sizeof(e));
    }

private:
    static bool cell_ok(uint8_t p, uint8_t t, uint8_t r) { return p < kPatterns && t < kTracks && r < kRows; }
    // A 6-byte cell from a 0.3.0 editor carries a second effect: it is dropped, or moved into
    // the effect column if that one is empty (same rule as song_from_v030).
    // CV tracks have no velocity, and their notes stay in C0..B5 (0.3.2).
    static Cell cell_from_wire(const uint8_t* c, bool v030, int track) {
        const bool use2 = v030 && c[2] == FxNone && c[4] != FxNone;
        Cell v{c[0], c[1], use2 ? c[4] : c[2], use2 ? c[5] : c[3]};
        if (track >= kFirstCvTrack && track < kFirstInTrack) {
            v.vel = 0;
            if (v.note != kNoteEmpty && v.note < kNoteOff) v.note = static_cast<uint8_t>(cv_fold_note(v.note));
        }
        return v;
    }
    void reject() { ++rejected_; }

    Host& host_;
    Parser parser_;
    uint32_t messages_ = 0, rejected_ = 0;
};

// Build the status message (card -> web), ~20 times a second while pinged.
inline int build_status(uint8_t* b, const EngineStatus& st, const Song& s, Mixer& mx,
                        uint8_t cpu_now, uint8_t cpu_peak, uint8_t trig_bits) {
    int i = 0;
    b[i++] = Status;
    b[i++] = static_cast<uint8_t>(st.mode & 0x7F);
    b[i++] = static_cast<uint8_t>(st.entry & 0x7F);
    b[i++] = static_cast<uint8_t>(st.pattern & 0x7F);
    b[i++] = static_cast<uint8_t>(st.row & 0x7F);
    b[i++] = static_cast<uint8_t>(st.rep & 0x7F);
    b[i++] = static_cast<uint8_t>(st.slot & 0x7F);
    b[i++] = static_cast<uint8_t>(st.flags & 0x7F);
    b[i++] = s.tempo_hi & 0x7F;
    b[i++] = s.tempo_lo & 0x7F;
    b[i++] = s.swing & 0x7F;
    for (int k = 0; k < 3; ++k) b[i++] = static_cast<uint8_t>(clampi(st.macro[k] + 64, 0, 127));
    b[i++] = static_cast<uint8_t>(st.cv_note[0] & 0x7F);
    b[i++] = static_cast<uint8_t>(st.cv_note[1] & 0x7F);
    b[i++] = static_cast<uint8_t>((st.gate[0] ? 1 : 0) | (st.gate[1] ? 2 : 0) |
                                  (st.in_gate[0] ? 4 : 0) | (st.in_gate[1] ? 8 : 0));   // bits 2/3: IN VCA gates (v3)
    b[i++] = cpu_now & 0x7F;
    b[i++] = cpu_peak & 0x7F;
    for (int ch = 0; ch < kChannels + 2; ++ch) b[i++] = static_cast<uint8_t>(mx.take_peak(ch));
    b[i++] = trig_bits & 0x0F;
    b[i++] = static_cast<uint8_t>(st.swap_count & 0x7F);
    b[i++] = static_cast<uint8_t>(mx.take_reverb_peak());   // v6
    return i;   // 30 bytes
}

// --- Panel report (v2) -------------------------------------------------------
// What the module's knobs are doing right now, plus the effective (song value
// + macro offset) parameter values, so the editor can draw them.
constexpr int kPanelBytesV2 = 27;
constexpr int kPanelBytes = 37;   // payload bytes after the command byte (v3)

struct PanelView {
    uint8_t knob7[3];   // reported knob positions 0..127 (with hysteresis)
};

// Effective value of a drum parameter after the PERFORM macro offset.
inline int effective_drum(const Song& s, const EngineStatus& st, int v, int param) {
    const int base = s.drum[v][param];
    if (param == DpDecay && macro_on(s, MacroDecay, v)) return clampi(base + st.macro[0], 0, 127);
    if (param == DpA && macro_on(s, MacroColour, v)) return clampi(base + st.macro[1], 0, 127);
    return base;
}
inline int effective_filter(const Song& s, const EngineStatus& st) {
    return clampi(s.master[MsFilter] - 64 + st.macro[2], -64, 63) + 64;
}
// CV track c: gate length in 1/16 row (0 = tie) and glide after the macros.
inline int effective_cv(const Song& s, const EngineStatus& st, int c, int param) {
    const int base = s.cv[c][param];
    if (param == CpGate && base > 0 && macro_on_cv(s, MacroDecay, c)) return clampi(base + st.macro[0] / 2, 1, 64);
    if (param == CpGlide && macro_on_cv(s, MacroColour, c)) return clampi(base + st.macro[1], 0, 127);
    return base;
}
// Input i: VCA decay and drive after the macros.
inline int effective_in(const Song& s, const EngineStatus& st, int i, int param) {
    const int base = s.mix[kDrumTracks + i][param];
    if (param == MpVca && macro_on_in(s, MacroDecay, i)) return clampi(base + st.macro[0], 0, 127);
    if (param == MpDrive && macro_on_in(s, MacroColour, i)) return clampi(base + st.macro[1], 0, 127);
    return base;
}

inline int build_panel(uint8_t* b, const EngineStatus& st, const Song& s, const PanelView& v) {
    int i = 0;
    b[i++] = Panel;
    b[i++] = static_cast<uint8_t>(((st.flags & 16) ? 1 : 0) | (st.down_held ? 2 : 0));
    for (int k = 0; k < 3; ++k) b[i++] = v.knob7[k] & 0x7F;
    b[i++] = static_cast<uint8_t>(st.knob_waiting & 7);
    for (int k = 0; k < 3; ++k) b[i++] = static_cast<uint8_t>(clampi(st.macro[k] + 64, 0, 127));
    b[i++] = static_cast<uint8_t>(~s.macro_excl[MacroDecay] & 0x0F);
    b[i++] = static_cast<uint8_t>(~s.macro_excl[MacroColour] & 0x0F);
    for (int d = 0; d < kDrumTracks; ++d) b[i++] = static_cast<uint8_t>(effective_drum(s, st, d, DpDecay));
    for (int d = 0; d < kDrumTracks; ++d) b[i++] = static_cast<uint8_t>(effective_drum(s, st, d, DpA));
    b[i++] = static_cast<uint8_t>(effective_filter(s, st));
    b[i++] = s.tempo_hi & 0x7F;
    b[i++] = s.tempo_lo & 0x7F;
    b[i++] = s.swing & 0x7F;
    b[i++] = static_cast<uint8_t>(st.slot & 0x7F);
    b[i++] = static_cast<uint8_t>(st.mode & 0x7F);
    b[i++] = static_cast<uint8_t>(st.entry & 0x7F);
    b[i++] = static_cast<uint8_t>(st.pattern & 0x7F);
    b[i++] = static_cast<uint8_t>(clampi(st.transpose + 64, 0, 127));
    // v3: CV/IN macro routing and effective values.
    b[i++] = s.macro_cvin[MacroDecay] & 0x0F;
    b[i++] = s.macro_cvin[MacroColour] & 0x0F;
    for (int c = 0; c < kCvTracks; ++c) b[i++] = static_cast<uint8_t>(effective_cv(s, st, c, CpGate));
    for (int c = 0; c < kCvTracks; ++c) b[i++] = static_cast<uint8_t>(effective_cv(s, st, c, CpGlide));
    for (int k = 0; k < kInputs; ++k) b[i++] = static_cast<uint8_t>(effective_in(s, st, k, MpVca));
    for (int k = 0; k < kInputs; ++k) b[i++] = static_cast<uint8_t>(effective_in(s, st, k, MpDrive));
    return i;   // 1 + kPanelBytes
}

// Decides when to send a Panel report (runs on core 1, never on the audio core):
// only when something changed, at most every min_gap_us, plus a keep-alive.
// Knob positions use hysteresis so ADC jitter does not cause a stream.
class PanelReporter {
public:
    static constexpr uint32_t kMinGapUs = 33000;      // ~30 reports/s max
    static constexpr uint32_t kKeepAliveUs = 1000000; // resend once a second
    static constexpr int kHyst = 16;                  // of 4095, ~0.4 %

    void force() { force_ = true; }

    // Returns the message length to send (0 = nothing to send now).
    int poll(uint32_t now_us, const EngineStatus& st, const Song& s, uint8_t* out) {
        for (int k = 0; k < 3; ++k) {
            const int raw = st.knob[k];
            const int centre = view_.knob7[k] * 32 + 16;
            if (raw > centre + 16 + kHyst || raw < centre - 16 - kHyst || first_)
                view_.knob7[k] = static_cast<uint8_t>(clampi(raw >> 5, 0, 127));
        }
        first_ = false;
        if (!force_ && now_us - last_us_ < kMinGapUs) return 0;
        const int len = build_panel(out, st, s, view_);
        const bool changed = std::memcmp(out, last_, static_cast<size_t>(len)) != 0;
        if (!changed && !force_ && now_us - last_us_ < kKeepAliveUs) return 0;
        std::memcpy(last_, out, static_cast<size_t>(len));
        last_us_ = now_us;
        force_ = false;
        return len;
    }

private:
    PanelView view_{{64, 64, 64}};
    uint8_t last_[1 + kPanelBytes] = {};
    uint32_t last_us_ = 0;
    bool force_ = true;
    bool first_ = true;
};

} // namespace proto
} // namespace trk
