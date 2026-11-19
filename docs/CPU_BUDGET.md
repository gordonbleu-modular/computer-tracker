# CPU budget

## Target

* RP2040 at **192 MHz** (core voltage raised to 1.15 V, a common, safe RP2040
  overclock; 192 MHz is a multiple of 48 MHz, which the ComputerCard docs recommend
  for clean ADC/PWM timing).
* `ProcessSample()` runs at **48 kHz** in the ADC DMA interrupt on core 0 →
  **4000 cycles per sample**. ComputerCard's own per-sample work (ADC
  de-interleaving, normalisation probe, PWM/DAC writes, knob smoothing) also has to
  fit in that window.
* Core 1 does USB, the protocol and flash writes. It has no hard deadline.

## How it is measured

1. **On the module**, every `ProcessSample()` is timed with SysTick. The card
   reports the average and the peak over each 50 ms window in the **Status**
   message, and the web app shows them (`CPU now / peak`, turns orange >75 %,
   red >90 %). This number does **not** include ComputerCard's overhead.
2. **On the host**, `tools/cpu_bench` compiles the same engine code
   for Cortex-M0+ with the firmware's flags (`-O2 -mcpu=cortex-m0plus -mthumb`)
   and runs it in the Unicorn emulator with a simple per-instruction M0+ cycle
   model (1 cycle ALU and multiply (the RP2040 has the single-cycle
   multiplier), 2 for loads/stores, 1+N for LDM/STM/PUSH/POP, 2 for taken
   branches, 3 for BL, and 26 cycles flat per divide, which stands for the SIO
   hardware divider plus call overhead). Results are approximate (no bus
   contention, all code in RAM as in the real copy_to_ram build).

```
source /workspace/workshop_computer/env.sh          # ARM toolchain
tools/cpu_bench/build.sh
python3 tools/cpu_bench/run_bench.py tools/cpu_bench/bench.elf 4800 [--profile]
```

## Results (engine only, cycles per sample, 4800 samples = 100 ms; firmware 0.3.1)

| Scenario | avg | p99 | max | max % of 4000 |
| --- | ---: | ---: | ---: | ---: |
| Idle (stopped, inputs connected) | 1208 | 1470 | 1549 | 39 % |
| Demo song playing (4 drums, 2 CV tracks, IN1 track, both inputs, effects) | 1610 | 1954 | 1982 | 50 % |
| Pathological: 4 voices alternating Snare/Modal at max decay, an effect on every row of all 8 tracks at 300 BPM (drums ratchet ×4, CV1 slide, CV2 ratchet ×3, IN1 ratchet ×4, IN2 ratchet ×8), IN2 following CV2, all mixer FX | 2919 | 3307 | 3711 | 93 % |

For comparison (avg / max): firmware 0.3.0 (two effects per step) idle 1207 / 1548,
demo 1608 / 2044, pathological 2923 / 3708; firmware 0.2.0 (6 tracks) idle
1155 / 1352, demo 1555 / 1886, pathological 2836 / 3718.

### What 0.3.x (8 tracks, IN tracks) costs, and what removing FX2 gave back

* **CPU:** 8 tracks cost about +50 cycles average (+1.3 % of the core) over 0.2
  in every scenario: two more tracks to read and the IN VCAs with their own
  envelopes. Removing the second effect column in 0.3.1 changes the CPU
  numbers only within noise: the extra effect was a few compares per row read,
  and the worst case is set by the voices and the mixer.
* **Busy-sample deferral (kept):** re-measured for 0.3.1 with the deferral
  disabled, the pathological case was 2898 avg / 3865 max (96.6 %), with it
  2919 / 3711 (92.8 %). It costs about 20 cycles on average but takes ≈150
  cycles off the worst single sample, which is the number that matters.
* **RAM:** a song is 32,980 bytes (0.3.0: 49,364; 0.2: 24,788), and the firmware
  keeps two (live + spare for glitch-free loading). RAM use is **155.6 KB of
  256 KB (59 %)**, down from 188.5 KB (72 %) in 0.3.0.
* **Flash:** firmware image 65 KB. A slot record is 32,996 bytes, written as
  9 sectors (36 KB) per save. The slots keep 0.3.0's 52 KB spacing so that songs
  saved with 0.3.0 can still be loaded; 6 slots plus the untouched 0.2 slots
  reserve 484 KB at the top of the 2 MB flash.
* **USB:** a full dump or upload is 512 messages of 73 bytes (≈37 KB; 0.3.0:
  ≈53 KB, 0.2: 384 messages, ≈28 KB).

Add **≈150 cycles** for the `ProcessSample()` wrapper (I/O mapping, SysTick,
LEDs) and an estimated **400–700 cycles** for ComputerCard (not measured; it
depends on the library version). That gives:

* normal use: **≈55–65 %** of the core;
* the pathological case is **close to or slightly over budget** for single
  samples. A rare overrun of one sample does not crash anything, but it may
  produce a click, and ComputerCard's ADC sequencing may glitch for a
  sample. This is the biggest performance risk; check the on-module CPU
  readout on real hardware.

144 MHz (the ComputerCard default suggestion) would **not** be enough
(3000 cycles); this is why the card runs at 192 MHz.

The v2 knob feedback costs the audio core almost nothing: core 0 only copies
a few words into the status block every 64 samples; the Panel report is
built and rate-limited on core 1.

## What keeps the worst case flat

* Control-rate work (envelopes, filter coefficients, pitch sweeps) every
  **8 samples**, with linear per-sample amplitude ramps.
* Periodic jobs are **staggered** on different sample phases: panel scan
  (every 8th sample, phase 1), LEDs (every 16th, phase 3), status snapshot
  (every 64th, phase 5), one mixer channel coefficient update per sample.
* The sequencer reads **one track every other sample**, and at most **one drum
  trigger per sample** is started (a queue absorbs ratchets/simultaneous hits;
  worst case 4 samples ≈ 83 µs of skew).
* (v3) Ratchets, delays and gate ends are **timestamps** (one compare per sample
  instead of per-track counters), and at most **one scheduled event fires per
  sample**; a second one due on the same sample waits one sample (21 µs).
* (v3) On a **busy sample** (a voice was triggered or the sequencer read a row)
  the mixer coefficient update and the input VCA control step are skipped; a
  guard runs them anyway after 32 busy samples in a row. The input VCA control
  runs every 4th sample with per-sample ramps.
* The Modal engine computes its three resonator coefficients **one per
  sample** after a hit.
* Silent voices and **unplugged audio inputs are skipped** entirely.
* No floats, no 64-bit multiplies in the audio path (Cortex-M0+ has neither);
  tables are built once at boot.

## Profile of the pathological case

`run_bench.py --profile` attributes cycles per function. The big items are the
Modal resonators (3 SVFs per voice), the snare's noise SVF, and the mixer
(drive soft-clip + tone + pan for 6 channels, then the master SVF and
soft-clip).

## Ideas if it is too tight on hardware

* Reduce Modal to 2 resonators when 3+ voices are Modal.
* Run the master filter at 24 kHz.
* Cap ratchets at 4.
