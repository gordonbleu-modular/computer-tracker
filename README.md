# Workshop Tracker

A **tracker sequencer and drum synth program card** for the
[Music Thing Modular Workshop System](https://www.musicthing.co.uk/workshopsystem/)
**Computer** module (RP2040), plus a **browser editor** that controls it over
USB.

* **4 drum-synth voices** run **on the module**: Kick, Karplus-Strong Pluck,
  pitched band-pass Noise, 2-op FM Metal, 3-mode Modal resonator, Snare/Clap.
* A **stereo mixer** (level, pan, drive, tone, mute + master filter/drive) to
  **audio out 1/2**.
* **2 CV/gate tracks** for the Workshop System's two analogue VCOs: calibrated
  **1 V/oct on CV out 1/2**, gates on **pulse out 1/2**, glide/slide.
* **Audio in 1/2** bring the VCOs back into the mix (with an optional VCA that
  follows the CV track's gate).
* **Tracker**: 16 patterns × 64 rows × 6 tracks, note/velocity/effect columns
  (param locks, ratchets, probability, micro-delay, gate length, slide), a
  32-step song arrangement, tempo, swing.
* **Performance mode without a computer**: 6 song slots in flash, start / next
  part / stop on the Z switch, knob macros, external clock & reset, CV
  modulation.

## Quick start

1. **Flash the card.** Download `workshop_tracker.uf2` (in `dist/` or from
   the GitHub release). With the card in the Computer and USB-C plugged into
   your computer, pull off the main knob, hold the BOOTSEL button behind it, and
   tap the load/reset button next to the card slot. Drag the UF2 onto the
   `RPI-RP2` drive. (Official guide:
   <https://www.musicthing.co.uk/workshopsystem/program-cards/install/>)
2. **Patch** (minimum): audio out 1/2 → mixer / headphones (stereo L/R).
   For the VCOs: CV out 1 → VCO 1 pitch, CV out 2 → VCO 2 pitch, VCO outputs →
   audio in 1/2. Pulse out 1/2 are gates (envelopes / VCAs) if you want them.
3. **Open the editor** in Chrome or Edge:
   *GitHub Pages:* `https://<your-user>.github.io/mtm-tracker/`
   *or locally:* `npx serve web` → <http://localhost:3000>
   (or `cd web && python3 -m http.server 8000` → <http://localhost:8000>).
   It must be `https://` or `localhost`. Opening `index.html` from disk does not work, because WebMIDI SysEx needs a secure context.
4. Click **Connect** and allow MIDI/SysEx access. The editor reads the song
   from the module. Edits go to the module immediately; **Save to module**
   writes the song to the selected flash slot.
5. Unplug USB whenever you like: the module keeps running and is played
   from the panel (see [docs/CONTROLS.md](docs/CONTROLS.md)).

The editor also works **offline** (no module). Songs are autosaved in the
browser and can be exported/imported as `.wtrk.json`.

## The editor

| Area | |
| --- | --- |
| Top bar | Connect, transport (song / pattern / stop / next part), BPM, swing, CPU load of the module |
| Toolbar | song name, flash slot load/save, read from / send to module, new, demo, export/import, octave, edit step, edit mode, follow |
| Left | song arrangement (pattern × repeats, ▶ to start or cue an entry), pattern selector & length, copy/paste/clear, effect help |
| Centre | the tracker grid |
| Right | voice editors (engine, decay, A, B, tune, velocity sensitivity), CV track settings, mixer with meters |

**Keys** (press *Keys* in the app for the full list): piano layout on
`Z S X D C V…` (lower octave) and `Q 2 W 3 E R…` (upper), `1` = note OFF,
hex digits in the velocity/effect columns, effect letters in the fx column,
`Delete` clears, arrows / Tab / PgUp / PgDn move, `-`/`=` change the octave,
`Space` plays/stops the pattern, `Enter` plays the song, `Ctrl+C / Ctrl+V`
copy/paste a track.

## Voices

All synthesis is fixed-point integer DSP at 48 kHz (no FPU on the RP2040),
with envelopes and coefficients at control rate (every 8 samples).

| Engine | Algorithm | A | B |
| --- | --- | --- | --- |
| **Kick** | sine oscillator with exponential pitch sweep (up to ~8× start pitch) and soft-clip drive | sweep depth | drive |
| **Pluck** | Karplus-Strong string: noise-burst excitation into a fractional-delay line with a damping filter; loss scaled by pitch so decay is even across the keyboard | brightness of the excitation | damping / stretch |
| **Noise** | white noise through a resonant Chamberlin state-variable band-pass tuned to the note, with an envelope-driven sweep and loudness compensation | resonance (from hiss to whistling) | sweep amount |
| **Metal** | 2-operator FM with a table of inharmonic ratios and its own index envelope (bells, cowbells, metallic hats) | ratio | FM index |
| **Modal** | short filtered noise strike into **three tuned resonators** (SVF band-passes) with ratios from 8 materials: membrane, marimba, glock, bell, tube, plate, glass, gong | material | strike hardness |
| **Snare** | sine body with pitch drop + band-passed noise; B ≥ 64 turns it into a **clap** (three noise bursts) | noise mix | snap / clap |

*Tune* (±48 semitones) and the note column set the pitch; *Vel sens* sets
how much velocity changes loudness. Fx `A`, `B`, `D` lock A/B/decay per hit.

## Panel and jacks

| Jack | Function |
| --- | --- |
| Audio out 1/2 | stereo mix L/R |
| Audio in 1/2 | VCO 1/2 into mixer channels 5/6 (gate VCA optional) |
| CV out 1/2 | 1 V/oct pitch of CV tracks 1/2 (calibrated) |
| Pulse out 1/2 | gates of CV tracks 1/2 |
| Pulse in 1 | clock in, 1 pulse = 1 row (replaces the internal clock while plugged) |
| Pulse in 2 | reset |
| CV in 1 | transpose CV tracks, 1 V/oct |
| CV in 2 | filter macro modulation |

| Z | Main | X | Y |
| --- | --- | --- | --- |
| **Middle** (PERFORM) | decay macro | colour macro (param A) | master filter LP ↔ HP |
| **Up** (SELECT) | song slot 1–6 | tempo 40–240 | swing 50–75 % |
| **Down** (momentary) | tap: start / next part · hold 0.8 s: stop | | |

Tempo, swing and macros use soft takeover. Details and LED meanings:
[docs/CONTROLS.md](docs/CONTROLS.md).

## How it works

```
 browser (WebMIDI) ──USB-MIDI SysEx──▶ core 1: TinyUSB, protocol, flash writes
                                          │  edits → live song (RAM)
                                          │  commands → lock-free queue
                                          ▼
                     core 0, 48 kHz ISR: sequencer → 4 voices → mixer → DACs
                                          panel, CV/gates, clock, LEDs
```

* **Protocol:** USB-MIDI SysEx (`F0 7D 54 …`), the same approach as the
  official ComputerCard `web_interface` example and ~30 released cards.
  No driver, automatic port detection, works next to a DAW. See
  [docs/PROTOCOL.md](docs/PROTOCOL.md) and [docs/RESEARCH.md](docs/RESEARCH.md).
* **Flash:** 6 song slots (28 KB each, CRC-checked) in the top of the card's
  flash, written by core 1 one sector at a time. The firmware runs entirely
  from RAM (`copy_to_ram`), so audio keeps running while saving.
* **CPU:** 192 MHz. ≈ 47 % of the per-sample budget for the demo song (engine
  only, estimated), ≈ 94 % in a deliberately pathological worst case. See
  [docs/CPU_BUDGET.md](docs/CPU_BUDGET.md). The module reports its measured
  load to the editor.

## Building

Firmware (Pico SDK 2.2.0, ARM GCC 14):

```sh
export PICO_SDK_PATH=/path/to/pico-sdk     # with submodules (TinyUSB)
cmake -S . -B build -G Ninja -DPICO_BOARD=pico -DCMAKE_BUILD_TYPE=Release
cmake --build build
cp build/workshop_tracker.uf2 dist/
```

Tests:

```sh
# engine, sequencer, panel, protocol and flash-format unit tests (host g++)
cmake -S . -B build-host -DBUILD_HOST_TESTS=ON && cmake --build build-host && ./build-host/test_engine
# JS protocol tests + JS↔C++ cross-check (uses build-host/xcheck from the step above)
node --test web/test/*.test.mjs
```

CPU benchmark: see [docs/CPU_BUDGET.md](docs/CPU_BUDGET.md).

CI (`.github/workflows/build.yml`) runs the tests, builds the UF2, attaches it
to tagged releases and deploys `web/` to GitHub Pages (enable Pages with
source **GitHub Actions** in the repo settings).

## Layout

```
src/main.cpp              ComputerCard glue, core-1 USB/flash host
src/engine/*.h            DSP, voices, mixer, sequencer, panel, protocol, flash format (header-only, host-testable)
src/tusb_config.h, usb_descriptors.c   TinyUSB MIDI device
vendor/ComputerCard/      ComputerCard v0.4.0 (MIT, Chris Johnson)
web/                      the editor (plain HTML/CSS/ES modules, no build step)
tests/                    host unit tests, JS↔C++ cross-check helper
tools/cpu_bench/          Cortex-M0+ cycle estimate in an emulator
docs/                     protocol, controls, CPU budget, research notes
dist/                     built UF2
```

## Implemented vs. not (yet)

Implemented: everything listed at the top. Not implemented / limitations:

* no delay/reverb send (CPU budget went to the voices);
* USB **device** mode only (no USB-host MIDI keyboards);
* external clock is fixed at **1 pulse = 1 row** (no PPQN setting);
* panel macros affect new hits, not notes already ringing;
* no per-track mute/solo in the grid (use the mixer mute), no undo;
* song changes from the panel are only possible between the 6 stored slots;
* ComputerCard's own interrupt overhead is estimated, not measured.

## Known risks

* **Not yet run on hardware.** USB enumeration, the flash writes while audio
  runs, the 192 MHz / 1.15 V clock, the knob/switch feel and the absolute
  levels all need a real test.
* The pathological CPU worst case is close to the budget; if you hear clicks,
  check the CPU readout in the editor.
* Saving a slot overwrites it. If power fails mid-save, that slot comes back
  empty (CRC), never corrupt; the other slots are untouched.
* Requires a Chromium-based browser (WebMIDI with SysEx).

## License

MIT, see [LICENSE](LICENSE). ComputerCard is MIT © Chris Johnson
(`vendor/ComputerCard/LICENSE`). TinyUSB config/descriptor files are adapted
from the ComputerCard `web_interface` example.
