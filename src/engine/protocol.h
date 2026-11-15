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
constexpr uint8_t kProtocolVersion = 1;
constexpr uint8_t kFwMajor = 0, kFwMinor = 1, kFwPatch = 0;
constexpr int kSlots = 6;
constexpr int kChunkRows = 16;

enum Cmd : uint8_t {
    // web -> card
    Hello = 0x01,        // -> Info
    GetSong = 0x02,      // -> SongHeader, 384 x PatternChunk (16 pat x 6 trk x 4), DumpEnd
    Ping = 0x09,         // keep-alive; Status streams while pings arrive
    SetCell = 0x10,      // pat track row note vel fx fxv
    SetCells = 0x11,     // pat track row0 count cells[count*4]
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
        case Hello: send_info(); break;
        case GetSong: send_song(s); break;
        case Ping: host_.pinged(); break;
        case SetCell:
            if (n != 7 || !cell_ok(p[0], p[1], p[2])) return reject();
            s.cells[p[0]][p[1]][p[2]] = Cell{p[3], p[4], p[5], p[6]};
            break;
        case SetCells: {
            if (n < 4 || !cell_ok(p[0], p[1], p[2])) return reject();
            const int count = p[3];
            if (count < 1 || p[2] + count > kRows || n != 4 + count * 4) return reject();
            for (int i = 0; i < count; ++i) {
                const uint8_t* c = p + 4 + i * 4;
                s.cells[p[0]][p[1]][p[2] + i] = Cell{c[0], c[1], c[2], c[3]};
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
            if (n != 3 || p[0] >= kChannels || p[1] >= kParamsPerChannel) return reject();
            s.mix[p[0]][p[1]] = p[2];
            break;
        case SetMaster:
            if (n != 2 || p[0] >= kParamsPerChannel) return reject();
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
        case SetZeroNote:
            if (n != 1 || p[0] > 126) return reject();
            s.cv_zero_note = p[0];
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
                    uint8_t c[5 + kChunkRows * 4];
                    c[0] = PatternChunk;
                    c[1] = static_cast<uint8_t>(p);
                    c[2] = static_cast<uint8_t>(t);
                    c[3] = static_cast<uint8_t>(r0);
                    c[4] = kChunkRows;
                    std::memcpy(c + 5, &s.cells[p][t][r0], kChunkRows * 4);
                    for (int i = 5; i < 5 + kChunkRows * 4; ++i) c[i] &= 0x7F;
                    host_.send(c, sizeof(c));
                }
            }
        }
        const uint8_t e[] = {DumpEnd, static_cast<uint8_t>(kPatterns), static_cast<uint8_t>(kTracks)};
        host_.send(e, sizeof(e));
    }

private:
    static bool cell_ok(uint8_t p, uint8_t t, uint8_t r) { return p < kPatterns && t < kTracks && r < kRows; }
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
    b[i++] = static_cast<uint8_t>((st.gate[0] ? 1 : 0) | (st.gate[1] ? 2 : 0));
    b[i++] = cpu_now & 0x7F;
    b[i++] = cpu_peak & 0x7F;
    for (int ch = 0; ch < kChannels + 2; ++ch) b[i++] = static_cast<uint8_t>(mx.take_peak(ch));
    b[i++] = trig_bits & 0x0F;
    b[i++] = static_cast<uint8_t>(st.swap_count & 0x7F);
    return i;   // 29 bytes
}

} // namespace proto
} // namespace trk
