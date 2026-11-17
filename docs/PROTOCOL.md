# Workshop Tracker USB protocol (v2)

The card appears as a **USB-MIDI device** called **"Workshop Tracker"**
(VID `0x2E8A`, PID `0x10C1`). All editor traffic is **SysEx**; no notes or CCs
are used, so the port can sit next to other MIDI gear without side effects.

```
F0 7D 54 <cmd> <payload…> F7
   │  │
   │  └─ 0x54 'T' = Workshop Tracker
   └──── 0x7D = MIDI "non-commercial / educational" manufacturer ID
```

Every payload byte is 7-bit (0–127). All song values are designed to fit in
7 bits, so no nibble packing is needed. The only value that is bigger than 127
(tempo, 30–300 BPM) is split into `hi = bpm >> 7`, `lo = bpm & 127`.

Reference implementations: `src/engine/protocol.h` (firmware, unit-tested in
`tests/test_engine.cpp`) and `web/js/protocol.js` (browser). The two are
cross-checked byte for byte by `web/test/xcheck.test.mjs`. That test feeds the JS
upload stream through the real C++ parser and reads the song back. It also
decodes a Panel report built by the C++ code and compares the effective values
with the JS implementation.

## Song data model

| Item | Size | Notes |
| --- | --- | --- |
| Patterns | 16 | each 1–64 rows (`pat_len`) |
| Tracks | 6 | 0–3 drum voices, 4–5 CV/gate |
| Cell | 4 bytes | `note, vel, fx, fxv` |
| Song (arrangement) | 32 entries | `{pattern, repeats 1–16}`, `song_len` 1–32 |
| Header | 212 bytes | see the table below |
| Whole song | 24,788 bytes | header + 16×6×64×4 cells |

**Cell**

* `note`: 0 = empty, 1–126 = MIDI note, 127 = OFF (choke drum / gate off)
* `vel`: 0 = default (100), 1–127
* `fx`: 0 = none or an ASCII letter (below); `fxv` 0–127

| fx | Tracks | Meaning |
| --- | --- | --- |
| `A` | drum | lock param A for this hit |
| `B` | drum | lock param B |
| `D` | drum | lock decay |
| `G` | CV | gate length in 1/16 row (0 = tie to the next note) |
| `S` | CV | slide: glide amount, gate stays high (legato) |
| `R` | both | ratchet: 2–8 evenly spaced hits in the row |
| `P` | both | probability in % (0–100) |
| `K` | both | delay by n/16 row (1–15) |

**Header layout** (offsets in bytes; mirrors `struct SongHeader` in `song.h`)

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 12 | name (ASCII, NUL-padded) |
| 12 | 2 | tempo hi, lo |
| 14 | 1 | swing 50–75 (% of the row pair given to the first row) |
| 15 | 1 | song_len |
| 16 | 1 | cv_zero_note (MIDI note that outputs 0 V, default 36 = C2) |
| 17 | 2 | macro_excl[2] (v2): PERFORM macro routing, [0] = decay macro (Main knob), [1] = colour macro (X knob). **Bit v set = drum voice v is *not* affected.** Stored inverted so that 0 (old songs, v1 flash slots) means "all voices". |
| 19 | 9 | reserved (0) |
| 28 | 16 | pat_len[16] |
| 44 | 64 | entries[32] {pattern, repeats} |
| 108 | 32 | drum[4][8]: engine, decay, A, B, tune (64 = 0), vel-sens, –, – |
| 140 | 16 | cv[2][8]: gate (1/16 row, 0 = tie), glide, transpose (64 = 0), … |
| 156 | 48 | mix[6][8]: level, pan (64 = C), drive, tone (64 = flat), gate-VCA, mute (≥64), –, – |
| 204 | 8 | master: level, drive, filter (64 = off), … |

The firmware sanitises every incoming value (`header_sanitize`, `set_cell`),
so a buggy or malicious host cannot put the engine in a bad state.

## Commands: web → card

| Cmd | Name | Payload |
| --- | --- | --- |
| 01 | Hello | – → card answers **Info** |
| 02 | GetSong | – → **SongHeader**, 384 × **PatternChunk**, **DumpEnd** |
| 09 | Ping | – (keep-alive; while pings arrive at least every 3 s the card streams **Status** every 50 ms) |
| 10 | SetCell | pat, track, row, note, vel, fx, fxv |
| 11 | SetCells | pat, track, row0, count (≤16), count × 4 cell bytes |
| 12 | SetPatLen | pat, len |
| 13 | SetDrum | voice, param, value |
| 14 | SetCv | track (0–1), param, value |
| 15 | SetMix | channel (0–5), param, value |
| 16 | SetMaster | param, value |
| 17 | SetEntry | index, pattern, repeats |
| 18 | SetSongLen | len |
| 19 | SetTempo | hi, lo |
| 1A | SetSwing | percent |
| 1B | SetName | 12 ASCII bytes |
| 1C | SetZeroNote | note |
| 1D | SetMacroMask | macro (0 = decay/Main, 1 = colour/X), **enabled**-voice bits 0–3 (v2) |
| 1F | SetHeader | 212 raw header bytes |
| 20 | Transport | mode (0 stop, 1 song, 2 pattern), arg (entry or pattern) |
| 21 | Cue | entry (song mode) or pattern (pattern mode); applied at the end of the current pattern |
| 22 | Preview | track, note, vel (plays one note now) |
| 23 | NextPart | – (same as a tap on Z Down) |
| 30 | Save | slot 0–5 → **Ack** when the flash write has finished |
| 31 | Load | slot → **Ack**; the editor then sends GetSong |
| 32 | InitSong | kind (0 empty, 1 demo) → **Ack** |

All edits are applied to the **live song in RAM** right away (a running
pattern picks them up on the next row). Nothing is written to flash until
**Save**.

## Messages: card → web

| Cmd | Name | Payload |
| --- | --- | --- |
| 41 | Info | protocol, fw major, minor, patch, tracks, drum tracks, patterns, rows, slots, entries, current slot, valid-slot bitmask |
| 42 | SongHeader | 212 raw header bytes |
| 43 | PatternChunk | pat, track, row0, count, count × 4 cell bytes |
| 44 | DumpEnd | – |
| 45 | Status | 28 bytes, see below |
| 46 | Panel | 27 bytes, knob/macro feedback, see below (v2) |
| 50 | Ack | op (30/31/32), status (0 ok, 1 empty slot, 2 busy, 3 error), slot |

**Status payload**

| # | Field |
| --- | --- |
| 0 | mode (0 stopped, 1 song, 2 pattern) |
| 1–4 | entry, pattern, row, repeat |
| 5 | slot |
| 6 | flags: 1 ext clock, 2 next part queued, 4 song swap pending, 8 knob waiting for pickup, 16 panel on SELECT page |
| 7–8 | tempo hi, lo (live, includes panel changes) |
| 9 | swing |
| 10–12 | macros decay, colour, filter (+64) |
| 13–14 | current CV notes |
| 15 | gate bits |
| 16–17 | CPU % now / peak since last status (ProcessSample only) |
| 18–25 | peak meters 0–127: channels 1–6, master L, R |
| 26 | drum trigger bits since last status |
| 27 | song swap counter (7-bit) |

**Panel payload (v2)**

Sent by core 1 only while the editor pings, **only when something changed**,
at most every 33 ms (~30/s), plus a keep-alive once a second and right after
Hello. Knob positions are reported with hysteresis so ADC jitter does not
cause a stream of messages. Building it never touches the audio core: it
reads the status words core 0 publishes every 64 samples.

| # | Field |
| --- | --- |
| 0 | flags: 1 = Z on SELECT page (Up), 2 = Z held down |
| 1–3 | knob positions Main, X, Y (0–127) |
| 4 | bit k = knob k is waiting for soft takeover (pickup) |
| 5–7 | macros decay, colour, filter (+64). The filter macro includes CV in 2. |
| 8–9 | macro routing: enabled-voice bits for decay (Main) and colour (X) |
| 10–13 | **effective** decay of drum voices 1–4 (song value + decay macro if routed, clamped 0–127) |
| 14–17 | **effective** param A of drum voices 1–4 (song value + colour macro if routed) |
| 18 | **effective** master filter (64 = off) |
| 19–20 | tempo hi, lo |
| 21 | swing |
| 22 | slot |
| 23–25 | mode, song entry (part), pattern |
| 26 | CV in 1 transpose in semitones (+64) |

### Macros are offsets

The PERFORM macros (Main = decay, X = colour/param A, Y = master filter) are
**live offsets** added when a drum is hit (filter: continuously). They are
**never written into the song**, and saving a slot saves the song values without them.
The editor draws the song value as the knob position (gold arc) and the
effective value from the Panel report as a red arc + dot. Tempo, swing and the
slot selected on the panel's SELECT page *are* song/session changes and are
synced into the editor's song.

### Compatibility

* The protocol version is the first byte of **Info**. The editor shows a banner
  for a v1 module ("knob feedback and voice routing need firmware 0.2") and keeps
  working: everything else in v2 is a superset of v1, and Status is unchanged.
  On v1 firmware, macros affect all voices, and the editor draws the red arcs
  that way, using the macros in Status.
* A v1 module rejects SetMacroMask (unknown command, ignored). A v2 module
  loads v1 flash slots unchanged (routing bytes are 0 = all voices).
* Unknown commands are counted and ignored; messages for other devices
  (`F0 7D` with a different device byte, or other manufacturers) are ignored.

## Flow

```
editor                         card
  Hello  ───────────────────▶
         ◀─────────────────── Info
  GetSong ──────────────────▶
         ◀─────────────────── SongHeader, PatternChunk × 384, DumpEnd
  Ping (every 500 ms) ──────▶
         ◀─────────────────── Status (every 50 ms)
         ◀─────────────────── Panel (on change, ≤ 30/s)
  SetCell / SetDrum / … ────▶  (applied to live RAM)
  Save n  ──────────────────▶  core 1 writes flash
         ◀─────────────────── Ack(save, ok, n)
```

A full upload ("Send to module") is SetHeader + 384 SetCells messages
(≈28 KB). The editor pauses 8 ms after every 24 messages so the card's 256-byte
MIDI RX FIFO can drain; the card handles USB on core 1 and never blocks audio.

## Firmware side

* TinyUSB runs on **core 1** (`tud_task()` in a loop); the audio engine on core 0
  never touches USB.
* Edits are written straight into the live `Song` (single-byte stores, so a
  torn read is never worse than one old/new cell for one row).
* Load/Init build the new song in a **spare buffer** and hand it to core 0
  through a lock-free SPSC queue. Core 0 swaps the pointer between samples.
* Save snapshots the live song into the spare buffer, then erases and programs a
  28 KB slot one 4 KB sector at a time with interrupts disabled **only on
  core 1**. The binary is built `copy_to_ram`, so core 0 never reads XIP flash
  and the audio keeps running during the write. Each record starts with a 16-byte
  header (`'WTRK'`, version, size, CRC-32, ~CRC). The CRC covers the whole song,
  so if power is cut mid-write that slot simply reads as empty/invalid. It is
  never loaded half-written (the old contents of *that* slot are lost, though).
  A small journal in the last
  flash sector remembers the last-used slot for boot.
