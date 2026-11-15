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

## Results (engine only, cycles per sample, 4800 samples = 100 ms)

| Scenario | avg | p99 | max | max % of 4000 |
| --- | ---: | ---: | ---: | ---: |
| Idle (stopped, inputs connected) | 1162 | 1308 | 1308 | 33 % |
| Demo song playing (4 drums, 2 CV tracks, both inputs, all FX) | 1562 | 1789 | 1894 | 47 % |
| Pathological: 4 voices alternating Snare/Modal at max decay, ratchet ×4 on every row at 300 BPM, all mixer FX, both inputs | 2843 | 3107 | 3756 | 94 % |

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

## What keeps the worst case flat

* Control-rate work (envelopes, filter coefficients, pitch sweeps) every
  **8 samples**, with linear per-sample amplitude ramps.
* Periodic jobs are **staggered** on different sample phases: panel scan
  (every 8th sample, phase 1), LEDs (every 16th, phase 3), status snapshot
  (every 64th, phase 5), one mixer channel coefficient update per sample.
* The sequencer reads **one track every other sample**, and at most **one drum
  trigger per sample** is started (a queue absorbs ratchets/simultaneous hits;
  worst case 4 samples ≈ 83 µs of skew).
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
