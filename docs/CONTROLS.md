# Control map

```
         ┌─────────────── Computer ───────────────┐
         │  (Main)            Z  Up    = SELECT    │
         │                       Mid   = PERFORM   │
         │  (X)   (Y)            Down  = tap/hold  │
         │                                        │
         │  LED0 LED1      Audio in 1/2  ◀ VCO 1/2 │
         │  LED2 LED3      Audio out 1/2 ▶ L / R   │
         │  LED4 LED5      CV in 1/2     ◀ mod     │
         │                 CV out 1/2    ▶ VCO 1V/oct
         │                 Pulse in 1/2  ◀ clock / reset
         │                 Pulse out 1/2 ▶ gates   │
         └────────────────────────────────────────┘
```

## Jacks

| Jack | Function |
| --- | --- |
| **Audio out 1 / 2** | Stereo mix **L / R**: 4 drum voices + both audio inputs, through the mixer and master section |
| **Audio in 1 / 2** | Mixer channels 5/6 ("VCO1", "VCO2"). Patch the two analogue VCOs here. Each has drive, tone, level, pan and an optional **gate VCA** that opens with CV/gate track 1/2, so the VCOs are "played" by the tracker. Unplugged inputs are skipped (saves CPU, no noise). |
| **CV out 1 / 2** | Calibrated **1 V/oct** pitch of CV/gate track 1 / 2 (uses the card's EEPROM calibration). The song's "0 V note" (default C2) is 0 V; range 0…+6 V. Glide and slide happen here. |
| **Pulse out 1 / 2** | **Gate** of CV/gate track 1 / 2 (high while the note sounds; brief 2 ms gap between retriggered notes, held through slides). |
| **Pulse in 1** | **Clock in**: one rising edge = one tracker row (e.g. 4 PPQN = 16th notes). When a cable is plugged in, the internal clock is off; swing still applies (the odd rows are delayed by a fraction of the measured row length). The first pulse after start plays row 0. |
| **Pulse in 2** | **Reset**: song mode → back to the first song entry, row 0; pattern mode → row 0 of the pattern. With the internal clock row 0 plays immediately; with an external clock it plays on the next clock pulse. |
| **CV in 1** | **Transpose** both CV tracks: 1 V = 12 semitones (−6…+6 V, quantised to semitones with hysteresis). Unplugged = 0. |
| **CV in 2** | Added to the **filter macro** (−6…+6 V sweeps the master filter from full low-pass to full high-pass). |

## Switch Z and knobs

The panel works with or without USB. Web edits and panel moves both act on the
same live song.

### Z Middle: PERFORM

| Knob | Macro | Effect |
| --- | --- | --- |
| **Main** | Decay | Offsets the decay of every drum hit (−64…+63; centre = as programmed) |
| **X** | Colour | Offsets param A of every drum hit (sweep / brightness / resonance / FM ratio / material / noise mix) |
| **Y** | Filter | Master filter: left = low-pass, centre = off, right = high-pass |

Decay and colour are applied **when a drum is hit** (not to notes already
ringing). Filter acts continuously. All three have a small dead zone at noon.

### Z Up: SELECT

| Knob | Function |
| --- | --- |
| **Main** | **Song slot 1–6** (six zones). A new slot is loaded 0.3 s after the knob stops in a zone. While playing, the change happens at the end of the current pattern (LED blinks). Slot 1 falls back to the demo song when empty; other empty slots are ignored. The slot is only changed once the knob has **left the zone it was in when you switched to Up**, so flicking Z never jumps to another song. |
| **X** | **Tempo** 40–240 BPM (soft takeover) |
| **Y** | **Swing** 50–75 % (soft takeover) |

Soft takeover: the knob does nothing until it passes the current value (the
LED0 / slot LED blinks while a knob is waiting). Macros work the same way when
you come back from SELECT to PERFORM.

### Z Down (momentary)

| Action | Function |
| --- | --- |
| **Tap** (release before 0.8 s) | Stopped → **start** the song from the current entry. Playing → **next part**: jump to the next song entry at the end of the current pattern (LED1 blinks while queued). |
| **Hold 0.8 s** | **Stop** (all gates off). |

The switch is debounced for 10 ms; the tap fires on release.

## LEDs

```
0 1
2 3
4 5
```

| Page | LEDs |
| --- | --- |
| PERFORM | **0** beat (flashes every 4 rows; blinks while a macro knob waits for pickup) · **1** playing (dim), blinking = next part / song change queued, bright while Z is held · **2–5** drum voice 1–4 hits |
| SELECT | the LED of the current **slot** (1 = LED0 … 6 = LED5); blinking = waiting for tempo/swing pickup or a song change at the end of the pattern |
| Saving | running light while the flash is being written (audio keeps playing) |

## Boot

On power-up the card loads the last used / saved slot (remembered in flash),
or the built-in demo song if that slot is empty. It starts **stopped**; tap Z
Down to play. With no USB cable the card is fully usable from the panel.
