// Workshop Tracker — song model and USB-MIDI SysEx protocol (browser + Node).
// Mirrors src/engine/song.h and src/engine/protocol.h; see docs/PROTOCOL.md.
// SPDX-License-Identifier: MIT

export const MFR = 0x7d;
export const DEVICE = 0x54; // 'T'
// v2: SetMacroMask, Panel report. v3 (fw 0.3.0): 8 tracks, 6-byte cells (2 FX). v4 (fw 0.3.1): 8 tracks, 4-byte cells.
// v5 (fw 0.3.2): fixed CV mapping C0 = 0 V, no CV velocity, slot knob over the occupied slots (wire format = v4).
// v6 (fw 0.4.0): lo-fi reverb: SetMix param 8 = send, SetMaster 3..7 = reverb, header 21..26 sends,
//                27 fx version, Status +1 byte (reverb return meter).
// v7 (fw 0.4.2): SetMaster 8 = reverb return mute, 9 = master mute (>= 64 = muted), live only
//                (not in the song); Status flags bit 32 / 64 report them.
// v8 (fw 0.4.3): drum param 6 = Kick click / grit (other engines), song fx_ver 2 (0.4.0-0.4.2
//                songs: param 6/7 cleared on load). Snare/clap morph: same param B, no wire change.
export const PROTOCOL_VERSION = 8;

export const K = {
  drumTracks: 4,
  cvTracks: 2,
  inTracks: 2,
  tracks: 8, // 0-3 drums, 4-5 CV/gate, 6-7 IN1/IN2 (audio-in VCAs)
  firstCv: 4,
  firstIn: 6,
  cellBytes: 4, // note vel fx val
  v2Tracks: 6, // firmware 0.1/0.2
  v2CellBytes: 4,
  channels: 6,
  patterns: 16,
  rows: 64,
  entries: 32,
  nameLen: 12,
  headerBytes: 212,
  slots: 6,
  chunkRows: 16,
  tempoMin: 30,
  tempoMax: 300,
  swingMin: 50,
  swingMax: 75,
};

export const NOTE_EMPTY = 0;
export const NOTE_OFF = 127;
// CV tracks (0.3.2): only positive voltages, C0 = 0 V, 1 V/oct; C0..B5 = 0..5.92 V (song.h kCvNoteMin/Max).
export const CV_ZERO_NOTE = 12;
export const CV_NOTE_MIN = 12;
export const CV_NOTE_MAX = 83;
export const CV_OCT_MAX = 5; // octave selector range on CV tracks: 0..5
export function cvFold(n) {
  while (n < CV_NOTE_MIN) n += 12;
  while (n > CV_NOTE_MAX) n -= 12;
  return n;
}
export const cvVolts = (n) => (n - CV_ZERO_NOTE) / 12;

// Byte offsets inside the 212-byte song header (struct SongHeader).
export const OFF = {
  name: 0,
  tempoHi: 12,
  tempoLo: 13,
  swing: 14,
  songLen: 15,
  cvZero: 16,
  macroExcl: 17, // [0] decay macro (Main), [1] colour macro (X): bit set = voice NOT affected
  macroCvin: 19, // v3: [0] Main, [1] X: bit set = routed; bits 0/1 CV1/CV2, 2/3 IN1/IN2
  fxSend: 21, // v6: reverb send per mixer channel (6)
  fxVer: 27, // v6: 1 = reverb fields valid; 0 = older song (sends 0, reverb defaults)
  patLen: 28,
  entries: 44, // 32 x {pattern, repeats}
  drum: 108, // 4 x 8
  cv: 140, // 2 x 8
  mix: 156, // 6 x 8
  master: 204, // 8
};

export const CMD = {
  Hello: 0x01, GetSong: 0x02, Ping: 0x09,
  SetCell: 0x10, SetCells: 0x11, SetPatLen: 0x12, SetDrum: 0x13, SetCv: 0x14,
  SetMix: 0x15, SetMaster: 0x16, SetEntry: 0x17, SetSongLen: 0x18, SetTempo: 0x19,
  SetSwing: 0x1a, SetName: 0x1b, SetZeroNote: 0x1c, SetMacroMask: 0x1d, SetHeader: 0x1f,
  Transport: 0x20, Cue: 0x21, Preview: 0x22, NextPart: 0x23,
  Save: 0x30, Load: 0x31, InitSong: 0x32,
  Info: 0x41, SongHeader: 0x42, PatternChunk: 0x43, DumpEnd: 0x44, Status: 0x45, Panel: 0x46, Ack: 0x50,
};

export const ENGINES = [
  { name: 'Kick', a: 'Sweep', b: 'Drive', x: 'Click', info: 'Sine with pitch sweep and drive' },
  { name: 'Pluck', a: 'Bright', b: 'Stretch', x: 'Grit', info: 'Karplus-Strong string' },
  { name: 'Noise', a: 'Reso', b: 'Sweep', x: 'Grit', info: 'Pitched band-pass noise' },
  { name: 'Metal', a: 'Ratio', b: 'Index', x: 'Grit', info: '2-op FM, inharmonic ratios' },
  { name: 'Modal', a: 'Material', b: 'Strike', x: 'Grit', info: '3-mode resonator (bars, bells, glass...)' },
  { name: 'Snare', a: 'Noise', b: 'Snr>Clap', x: 'Grit', info: 'Body + noise; B morphs snare -> clap' },
];
export const MATERIALS = ['Membrane', 'Marimba', 'Glock', 'Bell', 'Tube', 'Plate', 'Glass', 'Gong'];

export const DRUM_PARAMS = ['engine', 'decay', 'a', 'b', 'tune', 'velSens', 'x'];
export const DRUM_X = 6; // v8: Kick click level / grit (Pluck, Noise, Metal, Modal, Snare); 0 = off
export const CV_PARAMS = ['gate', 'glide', 'transpose'];
export const MIX_PARAMS = ['level', 'pan', 'drive', 'tone', 'vca', 'mute', 'vcaSrc', 'inGate'];
export const MASTER_PARAMS = ['level', 'drive', 'filter', 'rvReturn', 'rvSize', 'rvDecay', 'rvTone', 'rvLofi'];
// Reverb (v6): master params 3..7, defaults as kRvDefaults in song.h.
export const RV = { return: 3, size: 4, decay: 5, tone: 6, lofi: 7 };
export const RV_DEFAULTS = [96, 64, 64, 56, 40];
export const FX_VERSION = 2; // 1 = 0.4.0 (reverb fields), 2 = 0.4.3 (drum param 6 valid)
export const MASTER_MUTE = { reverb: 8, master: 9 }; // v7 SetMaster live-mute params (not stored)
export const MIX_SEND = 8; // SetMix pseudo-parameter for the send
export const rvSeconds = (d) => 0.3 * Math.pow(30, d / 127); // decay param -> -60 dB time (dsp.h)
// mix[IN][6]: what opens the input VCA (v3). 0 = v2 behaviour (CV gate if decay > 0, else open).
export const VCA_SRC = { auto: 0, open: 1, cvGate: 2, inTrack: 3 };
export const VCA_SRC_NAMES = ['AUTO', 'OPEN', 'CV GATE', 'IN TRACK'];

// Track kinds: which effects make sense where.
export const trackKind = (t) => (t < K.firstCv ? 'drum' : t < K.firstIn ? 'cv' : 'in');

// Per-step effects. Values are shown and typed as two decimal digits:
//  pct: 00-99 on screen = 0-127 stored;  raw: the number itself (min..max).
export const FX = {
  A: { code: 0x41, name: 'Param A lock', kinds: ['drum'], val: 'pct', help: "Sets the voice's A parameter (colour) for this hit only." },
  B: { code: 0x42, name: 'Param B lock', kinds: ['drum'], val: 'pct', help: "Sets the voice's B parameter for this hit only." },
  D: { code: 0x44, name: 'Decay lock', kinds: ['drum', 'in'], val: 'pct', help: 'Decay for this hit only (IN: VCA release).' },
  G: { code: 0x47, name: 'Gate length', kinds: ['cv', 'in'], val: 'raw', min: 0, max: 64, help: 'Gate length in 1/16 row: 08 = half a row, 16 = one row, 00 = hold until the next note/OFF.' },
  S: { code: 0x53, name: 'Slide', kinds: ['cv'], val: 'pct', help: 'Glide to this note (legato: the gate stays high). Value = glide time.' },
  R: { code: 0x52, name: 'Ratchet', kinds: ['drum', 'cv', 'in'], val: 'raw', min: 2, max: 8, help: 'Repeats the note 2-8 times inside the row.' },
  P: { code: 0x50, name: 'Probability', kinds: ['drum', 'cv', 'in'], val: 'raw', min: 0, max: 99, help: 'Chance in % that the step plays (no P = always).' },
  K: { code: 0x4b, name: 'Delay', kinds: ['drum', 'cv', 'in'], val: 'raw', min: 1, max: 15, help: 'Plays the step n/16 of a row late (08 = half a row, off-beat feel).' },
  W: { code: 0x57, name: 'Reverb send', kinds: ['drum', 'in'], val: 'pct', help: 'Reverb send for this hit only, instead of the Send knob. Without W the knob applies again. (fw 0.4.1+)' },
};
export const FX_BY_CODE = Object.fromEntries(Object.entries(FX).map(([l, f]) => [f.code, { letter: l, ...f }]));
export const fxFor = (t) => Object.entries(FX).filter(([, f]) => f.kinds.includes(trackKind(t)));

// 0-99 display <-> 0-127 storage. Every display value maps to one stored
// value and back (round trip is exact for 0..99).
export const pctOf = (v) => Math.round((clampN(v, 0, 127) * 99) / 127);
export const pctTo = (d) => Math.round((clampN(d, 0, 99) * 127) / 99);
function clampN(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }

// FX value as shown (number 0..99) and as stored.
export function fxShow(code, v) {
  const f = FX_BY_CODE[code];
  if (!f) return clampN(v, 0, 99);
  return f.val === 'pct' ? pctOf(v) : clampN(v, 0, 99);
}
export function fxStore(code, d) {
  const f = FX_BY_CODE[code];
  if (!f) return clampN(d, 0, 127);
  return f.val === 'pct' ? pctTo(d) : clampN(d, f.min, f.max);
}
// Velocity: 0 = default (shown ".."); 1-127 shown 01-99.
export const velShow = (v) => (v ? Math.max(1, pctOf(v)) : -1);
export const velStore = (d) => Math.max(1, pctTo(d));

const NOTE_NAMES = ['C-', 'C#', 'D-', 'D#', 'E-', 'F-', 'F#', 'G-', 'G#', 'A-', 'A#', 'B-'];
export function noteName(n) {
  if (n === NOTE_EMPTY) return '---';
  if (n === NOTE_OFF) return 'OFF';
  const oct = Math.floor(n / 12) - 1;
  return NOTE_NAMES[n % 12] + (oct < 0 ? 'm' : String(oct));
}
export const hex2 = (v) => v.toString(16).toUpperCase().padStart(2, '0');
export const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));

// ---------------------------------------------------------------- model ----
export class Song {
  constructor() {
    this.header = new Uint8Array(K.headerBytes);
    this.cells = new Uint8Array(K.patterns * K.tracks * K.rows * K.cellBytes);
    this.clear();
  }

  static cellIndex(p, t, r) { return ((p * K.tracks + t) * K.rows + r) * K.cellBytes; }

  getCell(p, t, r) {
    const i = Song.cellIndex(p, t, r);
    const c = this.cells;
    return { note: c[i], vel: c[i + 1], fx: c[i + 2], fxv: c[i + 3] };
  }
  // Editor edits: CV tracks have no velocity and stay in C0..B5.
  setCell(p, t, r, cell) {
    if (trackKind(t) === 'cv') {
      const n = cell.note | 0;
      cell = { ...cell, vel: 0, note: n && n !== NOTE_OFF && n < 128 ? cvFold(n) : n };
    }
    this.setCellRaw(p, t, r, cell);
  }
  // As stored (imports and dumps; normalizeCv() converts those afterwards).
  setCellRaw(p, t, r, cell) {
    const i = Song.cellIndex(p, t, r);
    this.cells[i] = clamp(cell.note | 0, 0, 127);
    this.cells[i + 1] = clamp(cell.vel | 0, 0, 127);
    this.cells[i + 2] = clamp(cell.fx | 0, 0, 127);
    this.cells[i + 3] = clamp(cell.fxv | 0, 0, 127);
  }
  // Cells from an editor 0.3.0 song file (8 tracks x 6 bytes): FX2 is dropped,
  // or moved into FX1 when that is empty (same as song_from_v030 in song.h).
  setCellsV030(bytes) {
    for (let p = 0; p < K.patterns; p++)
      for (let t = 0; t < K.tracks; t++)
        for (let r = 0; r < K.rows; r++) {
          const o = ((p * K.tracks + t) * K.rows + r) * 6;
          this.setCellRaw(p, t, r, fxFrom6(bytes, o));
        }
  }
  // Cells from a firmware 0.1/0.2 dump or song file (6 tracks x 4 bytes).
  setCellsV2(bytes) {
    for (let p = 0; p < K.patterns; p++)
      for (let t = 0; t < K.v2Tracks; t++)
        for (let r = 0; r < K.rows; r++) {
          const o = ((p * K.v2Tracks + t) * K.rows + r) * 4;
          this.setCellRaw(p, t, r, { note: bytes[o], vel: bytes[o + 1], fx: bytes[o + 2], fxv: bytes[o + 3] });
        }
  }
  // Songs from firmware/editor <= 0.3.1 had a free "0 V note" (default C2) and CV
  // velocity. Convert to C0 = 0 V like song_migrate_cv() in song.h: in-range notes keep
  // their voltage, the rest move by octaves into C0..B5, CV velocity is cleared.
  // Idempotent; returns true when something changed.
  normalizeCv() {
    const zero = this.cvZero <= 126 ? this.cvZero : 36;
    const shift = zero - CV_ZERO_NOTE;
    let changed = zero !== CV_ZERO_NOTE;
    for (let p = 0; p < K.patterns; p++)
      for (let t = K.firstCv; t < K.firstIn; t++)
        for (let r = 0; r < K.rows; r++) {
          const i = Song.cellIndex(p, t, r), c = this.cells;
          let n = c[i];
          if (n && n !== NOTE_OFF) n = cvFold(n - shift);
          if (n !== c[i] || c[i + 1]) { c[i] = n; c[i + 1] = 0; changed = true; }
        }
    this.header[OFF.cvZero] = CV_ZERO_NOTE;
    return changed;
  }
  // Header fields a v2 song lacks get the values that keep it sounding the same.
  migrateHeaderV2() {
    this.header[OFF.macroCvin] = 0; this.header[OFF.macroCvin + 1] = 0;
    for (let i = 0; i < K.inTracks; i++) { this.setMix(4 + i, 6, VCA_SRC.auto); this.setMix(4 + i, 7, 8); }
  }

  get name() {
    let s = '';
    for (let i = 0; i < K.nameLen; i++) { const c = this.header[OFF.name + i]; if (!c) break; s += String.fromCharCode(c); }
    return s;
  }
  set name(v) {
    for (let i = 0; i < K.nameLen; i++) {
      const c = i < v.length ? v.charCodeAt(i) : 0;
      this.header[OFF.name + i] = c >= 32 && c <= 126 ? c : (c ? 32 : 0);
    }
  }
  get tempo() { return this.header[OFF.tempoHi] * 128 + this.header[OFF.tempoLo]; }
  set tempo(bpm) {
    bpm = clamp(Math.round(bpm), K.tempoMin, K.tempoMax);
    this.header[OFF.tempoHi] = bpm >> 7; this.header[OFF.tempoLo] = bpm & 127;
  }
  get swing() { return this.header[OFF.swing]; }
  set swing(v) { this.header[OFF.swing] = clamp(v | 0, K.swingMin, K.swingMax); }
  get songLen() { return this.header[OFF.songLen]; }
  set songLen(v) { this.header[OFF.songLen] = clamp(v | 0, 1, K.entries); }
  get cvZero() { return this.header[OFF.cvZero]; }
  set cvZero(v) { this.header[OFF.cvZero] = clamp(v | 0, 0, 126); }
  patLen(p) { return this.header[OFF.patLen + p]; }
  setPatLen(p, v) { this.header[OFF.patLen + p] = clamp(v | 0, 1, K.rows); }
  entry(i) { return { pattern: this.header[OFF.entries + i * 2], repeats: this.header[OFF.entries + i * 2 + 1] }; }
  setEntry(i, pattern, repeats) {
    this.header[OFF.entries + i * 2] = clamp(pattern | 0, 0, K.patterns - 1);
    this.header[OFF.entries + i * 2 + 1] = clamp(repeats | 0, 1, 16);
  }
  drum(v, p) { return this.header[OFF.drum + v * 8 + p]; }
  setDrum(v, p, x) { this.header[OFF.drum + v * 8 + p] = clamp(x | 0, 0, p === 0 ? ENGINES.length - 1 : 127); }
  cv(t, p) { return this.header[OFF.cv + t * 8 + p]; }
  setCv(t, p, x) { this.header[OFF.cv + t * 8 + p] = clamp(x | 0, 0, 127); }
  mix(ch, p) { return this.header[OFF.mix + ch * 8 + p]; }
  setMix(ch, p, x) { this.header[OFF.mix + ch * 8 + p] = clamp(x | 0, 0, 127); }
  // Macro routing: bits 0..3 = drum voices the macro acts on (m 0 = decay/Main, 1 = colour/X).
  macroMask(m) { return ~this.header[OFF.macroExcl + m] & 0x0f; }
  setMacroMask(m, bits) { this.header[OFF.macroExcl + m] = ~bits & 0x0f; }
  macroOn(m, v) { return !!(this.macroMask(m) & (1 << v)); }
  // v3: routing for the CV/IN modules, bits 0/1 = CV1/CV2, 2/3 = IN1/IN2 (bit set = routed).
  macroCvMask(m) { return this.header[OFF.macroCvin + m] & 0x0f; }
  setMacroCvMask(m, bits) { this.header[OFF.macroCvin + m] = bits & 0x0f; }
  macroOnCv(m, c) { return !!(this.macroCvMask(m) & (1 << c)); }
  macroOnIn(m, i) { return !!(this.macroCvMask(m) & (4 << i)); }
  // What opens input i's VCA (same rule as vca_source() in song.h).
  vcaSource(i) {
    const src = this.mix(4 + i, 6);
    if (src === VCA_SRC.auto || src > 3) return this.mix(4 + i, 4) ? VCA_SRC.cvGate : VCA_SRC.open;
    return src;
  }
  master(p) { return this.header[OFF.master + p]; }
  setMaster(p, x) { this.header[OFF.master + p] = clamp(x | 0, 0, 127); }
  // Reverb send per channel (v6).
  send(ch) { return this.header[OFF.fxSend + ch]; }
  setSend(ch, x) { this.header[OFF.fxSend + ch] = clamp(x | 0, 0, 127); }
  get fxVer() { return this.header[OFF.fxVer]; }
  // Songs from before 0.4.0: sends 0 and reverb defaults (same as header_sanitize()
  // in song.h), so they sound exactly as before. Returns true when it changed something.
  // Songs from 0.4.0-0.4.2 (fx_ver 1) keep the reverb and get click/grit 0.
  migrateFx() {
    const v = this.header[OFF.fxVer];
    if (v === FX_VERSION) return false;
    if (v !== 1) {
      for (let c = 0; c < K.channels; c++) this.setSend(c, 0);
      RV_DEFAULTS.forEach((x, i) => this.setMaster(RV.return + i, x));
    }
    for (let d = 0; d < K.drumTracks; d++) { this.header[OFF.drum + d * 8 + 6] = 0; this.header[OFF.drum + d * 8 + 7] = 0; }
    this.header[OFF.fxVer] = FX_VERSION;
    return true;
  }

  // Same defaults as song_clear() in src/engine/song.h.
  clear() {
    this.header.fill(0);
    this.cells.fill(0);
    this.name = 'NEW SONG';
    this.tempo = 120;
    this.swing = 50;
    this.songLen = 1;
    this.cvZero = CV_ZERO_NOTE;
    for (let p = 0; p < K.patterns; p++) this.setPatLen(p, 16);
    for (let i = 0; i < K.entries; i++) this.setEntry(i, i < K.patterns ? i : 0, 1);
    const engines = [0, 5, 3, 1], decays = [62, 48, 40, 80];
    for (let v = 0; v < K.drumTracks; v++) {
      this.setDrum(v, 0, engines[v]); this.setDrum(v, 1, decays[v]);
      this.setDrum(v, 2, 64); this.setDrum(v, 3, 32); this.setDrum(v, 4, 64); this.setDrum(v, 5, 100);
    }
    for (let c = 0; c < K.cvTracks; c++) { this.setCv(c, 0, 8); this.setCv(c, 1, 0); this.setCv(c, 2, 64); }
    const pans = [64, 52, 80, 40, 44, 84];
    for (let ch = 0; ch < K.channels; ch++) {
      this.setMix(ch, 0, 110); this.setMix(ch, 1, pans[ch]); this.setMix(ch, 2, 0);
      this.setMix(ch, 3, 64); this.setMix(ch, 4, 0); this.setMix(ch, 5, 0);
    }
    for (let i = 0; i < K.inTracks; i++) { this.setMix(4 + i, 6, VCA_SRC.auto); this.setMix(4 + i, 7, 8); }
    this.setMacroCvMask(0, 0x0f); this.setMacroCvMask(1, 0x0f);
    this.setMaster(0, 120); this.setMaster(1, 10); this.setMaster(2, 64);
    RV_DEFAULTS.forEach((v, i) => this.setMaster(RV.return + i, v));
    this.header[OFF.fxVer] = FX_VERSION;
  }

  toJSON() {
    return {
      format: 'workshop-tracker-song', version: 3, // v3 file = 8 tracks x 4-byte cells (v2 = 6-byte cells, v1 = 6 tracks)
      header: Array.from(this.header),
      cells: btoaBytes(this.cells),
    };
  }
  static fromJSON(o) {
    if (!o || o.format !== 'workshop-tracker-song') throw new Error('Not a Workshop Tracker song file');
    const s = new Song();
    if (o.header.length !== K.headerBytes) throw new Error('Bad header size');
    s.header.set(o.header.map((v) => v & 127));
    const cells = atobBytes(o.cells).map((v) => v & 127);
    if (cells.length === s.cells.length) {
      s.cells.set(cells);
    } else if (cells.length === K.patterns * K.tracks * K.rows * 6) {
      s.setCellsV030(cells); // file saved by editor 0.3.0 (two effects)
    } else if (cells.length === K.patterns * K.v2Tracks * K.rows * 4) {
      s.setCellsV2(cells); // file saved by editor 0.1/0.2
      s.migrateHeaderV2();
    } else {
      throw new Error('Bad pattern data size');
    }
    s.normalizeCv(); // files from editor <= 0.3.1 (C2 = 0 V, CV velocity)
    s.migrateFx(); // files from editor <= 0.3.2: no reverb fields
    return s;
  }
}

// One cell from 6 bytes (note vel fx fxv fx2 fxv2) of firmware/editor 0.3.0.
function fxFrom6(b, o) {
  const use2 = !b[o + 2] && b[o + 4];
  return { note: b[o], vel: b[o + 1], fx: use2 ? b[o + 4] : b[o + 2], fxv: use2 ? b[o + 5] : b[o + 3] };
}

function btoaBytes(u8) {
  let s = '';
  for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
  return typeof btoa === 'function' ? btoa(s) : Buffer.from(u8).toString('base64');
}
function atobBytes(b64) {
  if (typeof atob === 'function') {
    const s = atob(b64);
    const u = new Uint8Array(s.length);
    for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i);
    return u;
  }
  return new Uint8Array(Buffer.from(b64, 'base64'));
}

// --------------------------------------------------------------- encode ----
export function frame(cmd, payload = []) {
  const out = new Uint8Array(payload.length + 5);
  out[0] = 0xf0; out[1] = MFR; out[2] = DEVICE; out[3] = cmd;
  for (let i = 0; i < payload.length; i++) {
    const b = payload[i];
    if (b < 0 || b > 127 || !Number.isInteger(b)) throw new Error(`payload byte ${i} out of range: ${b}`);
    out[4 + i] = b;
  }
  out[out.length - 1] = 0xf7;
  return out;
}

// `proto` = the firmware's protocol version. Cells go out in the 4-byte form,
// except to firmware 0.3.0 (protocol 3), which gets its 6-byte form with an
// empty second effect so no stale FX2 stays on the module. Below 3, tracks
// 6-7 do not exist.
export const msg = {
  hello: () => frame(CMD.Hello),
  getSong: () => frame(CMD.GetSong),
  ping: () => frame(CMD.Ping),
  setCell: (p, t, r, c, proto = PROTOCOL_VERSION) =>
    frame(CMD.SetCell, proto === 3 ? [p, t, r, c.note, c.vel, c.fx, c.fxv, 0, 0] : [p, t, r, c.note, c.vel, c.fx, c.fxv]),
  setCells: (song, p, t, r0, count, proto = PROTOCOL_VERSION) => {
    const pl = [p, t, r0, count];
    for (let i = 0; i < count; i++) {
      const c = song.getCell(p, t, r0 + i);
      pl.push(c.note, c.vel, c.fx, c.fxv);
      if (proto === 3) pl.push(0, 0);
    }
    return frame(CMD.SetCells, pl);
  },
  setPatLen: (p, len) => frame(CMD.SetPatLen, [p, len]),
  setDrum: (v, p, x) => frame(CMD.SetDrum, [v, p, x]),
  setCv: (t, p, x) => frame(CMD.SetCv, [t, p, x]),
  setMix: (ch, p, x) => frame(CMD.SetMix, [ch, p, x]),
  setSend: (ch, x) => frame(CMD.SetMix, [ch, MIX_SEND, x]), // v6
  setMaster: (p, x) => frame(CMD.SetMaster, [p, x]),
  setLiveMute: (which, on) => frame(CMD.SetMaster, [which === 'master' ? 9 : 8, on ? 127 : 0]), // v7
  setEntry: (i, pat, rep) => frame(CMD.SetEntry, [i, pat, rep]),
  setSongLen: (n) => frame(CMD.SetSongLen, [n]),
  setTempo: (bpm) => frame(CMD.SetTempo, [bpm >> 7, bpm & 127]),
  setSwing: (v) => frame(CMD.SetSwing, [v]),
  setName: (song) => frame(CMD.SetName, Array.from(song.header.slice(OFF.name, OFF.name + K.nameLen))),
  setZeroNote: (n) => frame(CMD.SetZeroNote, [n]),
  setMacroMask: (m, bits, cvin) => frame(CMD.SetMacroMask, cvin === undefined ? [m, bits & 0x0f] : [m, bits & 0x0f, cvin & 0x0f]),
  // Firmware 0.4.0-0.4.2 (protocol 6/7) knows fx_ver 1 only and would reset the
  // reverb of anything else: send it fx_ver 1 (it ignores drum param 6 anyway).
  setHeader: (song, proto = PROTOCOL_VERSION) => {
    const h = Array.from(song.header);
    if (proto < 8 && h[OFF.fxVer] === FX_VERSION) h[OFF.fxVer] = 1;
    return frame(CMD.SetHeader, h);
  },
  stop: () => frame(CMD.Transport, [0, 0]),
  playSong: (entry = 0) => frame(CMD.Transport, [1, entry]),
  playPattern: (p) => frame(CMD.Transport, [2, p]),
  cue: (x) => frame(CMD.Cue, [x]),
  preview: (t, note, vel) => frame(CMD.Preview, [t, note, vel]),
  nextPart: () => frame(CMD.NextPart),
  save: (slot) => frame(CMD.Save, [slot]),
  load: (slot) => frame(CMD.Load, [slot]),
  initSong: (kind) => frame(CMD.InitSong, [kind]),
};

// All messages needed to replace the module's song with `song`.
export function uploadMessages(song, proto = PROTOCOL_VERSION) {
  const out = [msg.setHeader(song, proto)];
  const tracks = proto >= 3 ? K.tracks : K.v2Tracks;
  for (let p = 0; p < K.patterns; p++)
    for (let t = 0; t < tracks; t++)
      for (let r = 0; r < K.rows; r += K.chunkRows) out.push(msg.setCells(song, p, t, r, K.chunkRows, proto));
  return out;
}

// --------------------------------------------------------------- decode ----
// Returns null for messages that are not ours.
export function decode(data) {
  const d = data instanceof Uint8Array ? data : new Uint8Array(data);
  if (d.length < 5 || d[0] !== 0xf0 || d[d.length - 1] !== 0xf7 || d[1] !== MFR || d[2] !== DEVICE) return null;
  const cmd = d[3];
  const p = d.subarray(4, d.length - 1);
  switch (cmd) {
    case CMD.Info:
      return {
        type: 'info', protocol: p[0], firmware: `${p[1]}.${p[2]}.${p[3]}`,
        tracks: p[4], drumTracks: p[5], patterns: p[6], rows: p[7], slots: p[8], entries: p[9],
        slot: p[10], validSlots: p[11],
      };
    case CMD.SongHeader:
      return { type: 'header', header: p.slice(0, K.headerBytes), ok: p.length === K.headerBytes };
    case CMD.PatternChunk: {
      // 4 bytes per cell (0.1/0.2: tracks 0-5 only; 0.3.1), 6 from firmware 0.3.0.
      const width = p[3] && p.length - 4 === p[3] * 6 ? 6 : 4;
      return { type: 'chunk', pattern: p[0], track: p[1], row: p[2], count: p[3], width, cells: p.slice(4, 4 + p[3] * width) };
    }
    case CMD.DumpEnd:
      return { type: 'dumpEnd' };
    case CMD.Status: {
      if (p.length < 28) return { type: 'status', bad: true };
      return {
        type: 'status',
        mode: p[0], entry: p[1], pattern: p[2], row: p[3], rep: p[4], slot: p[5], flags: p[6],
        extClock: !!(p[6] & 1), nextQueued: !!(p[6] & 2), swapPending: !!(p[6] & 4),
        knobWaiting: !!(p[6] & 8), selectPage: !!(p[6] & 16),
        reverbMuted: !!(p[6] & 32), masterMuted: !!(p[6] & 64), // v7
        tempo: p[7] * 128 + p[8], swing: p[9],
        macros: [p[10] - 64, p[11] - 64, p[12] - 64],
        cvNotes: [p[13], p[14]], gates: [!!(p[15] & 1), !!(p[15] & 2)], inGates: [!!(p[15] & 4), !!(p[15] & 8)],
        cpu: p[16], cpuPeak: p[17],
        peaks: Array.from(p.slice(18, 26)),
        hits: p[26], swaps: p[27],
        reverbPeak: p.length >= 29 ? p[28] : 0, // v6
      };
    }
    case CMD.Panel: {
      if (p.length < 27) return { type: 'panel', bad: true };
      const v3 = p.length >= 37;
      return {
        v3,
        cvinMasks: v3 ? [p[27], p[28]] : [0, 0],
        effCvGate: v3 ? [p[29], p[30]] : null, effCvGlide: v3 ? [p[31], p[32]] : null,
        effInDecay: v3 ? [p[33], p[34]] : null, effInDrive: v3 ? [p[35], p[36]] : null,
        type: 'panel',
        selectPage: !!(p[0] & 1), downHeld: !!(p[0] & 2),
        knobs: [p[1], p[2], p[3]], waiting: [!!(p[4] & 1), !!(p[4] & 2), !!(p[4] & 4)],
        macros: [p[5] - 64, p[6] - 64, p[7] - 64],
        masks: [p[8], p[9]],
        effDecay: Array.from(p.slice(10, 14)), effA: Array.from(p.slice(14, 18)), effFilter: p[18],
        tempo: p[19] * 128 + p[20], swing: p[21], slot: p[22], mode: p[23], entry: p[24], pattern: p[25],
        transpose: p[26] - 64,
      };
    }
    case CMD.Ack: {
      const ops = { [CMD.Save]: 'save', [CMD.Load]: 'load', [CMD.InitSong]: 'init' };
      return { type: 'ack', op: ops[p[0]] || p[0], status: ['ok', 'empty', 'busy', 'error'][p[1]] || p[1], slot: p[2] };
    }
    default:
      return { type: 'unknown', cmd };
  }
}

// Effective parameter values while a PERFORM macro is applied (same maths as
// effective_drum / effective_filter in protocol.h). macros = [decay, colour, filter], -64..63.
export function effectiveDrum(song, macros, v, param) {
  const base = song.drum(v, param);
  if (param === 1 && song.macroOn(0, v)) return clamp(base + macros[0], 0, 127);
  if (param === 2 && song.macroOn(1, v)) return clamp(base + macros[1], 0, 127);
  return base;
}
export function effectiveFilter(song, macros) { return clamp(song.master(2) - 64 + macros[2], -64, 63) + 64; }
// v3 (effective_cv / effective_in): CV gate (param 0) and glide (1); IN decay (mix 4) and drive (mix 2).
export function effectiveCv(song, macros, c, param) {
  const base = song.cv(c, param);
  if (param === 0 && base > 0 && song.macroOnCv(0, c)) return clamp(base + Math.trunc(macros[0] / 2), 1, 64);
  if (param === 1 && song.macroOnCv(1, c)) return clamp(base + macros[1], 0, 127);
  return base;
}
export function effectiveIn(song, macros, i, param) {
  const base = song.mix(4 + i, param);
  if (param === 4 && song.macroOnIn(0, i)) return clamp(base + macros[0], 0, 127);
  if (param === 2 && song.macroOnIn(1, i)) return clamp(base + macros[1], 0, 127);
  return base;
}

// Assembles a full song from a GetSong dump.
export class DumpCollector {
  constructor() { this.reset(); }
  reset() { this.song = new Song(); this.gotHeader = false; this.chunks = 0; this.done = false; this.maxTrack = -1; }
  // Firmware 0.1/0.2 dumps have only tracks 0-5.
  get legacy() { return this.chunks > 0 && this.maxTrack < K.firstIn; }
  feed(m) {
    if (m.type === 'header') { this.reset(); this.song.header.set(m.header); this.gotHeader = true; }
    else if (m.type === 'chunk' && this.gotHeader) {
      if (m.track > this.maxTrack) this.maxTrack = m.track;
      for (let i = 0; i < m.count; i++) {
        const o = i * m.width, c = m.cells;
        this.song.setCellRaw(m.pattern, m.track, m.row + i, m.width === 6 ? fxFrom6(c, o) : { note: c[o], vel: c[o + 1], fx: c[o + 2], fxv: c[o + 3] });
      }
      this.chunks++;
    } else if (m.type === 'dumpEnd' && this.gotHeader) {
      this.done = true;
      if (this.legacy) this.song.migrateHeaderV2(); // firmware 0.1/0.2: IN tracks empty, VCAs follow the CV gates
      this.cvConverted = this.song.normalizeCv();  // firmware <= 0.3.1: C2 = 0 V -> C0 = 0 V
      this.song.migrateFx();                       // firmware <= 0.3.2: no reverb fields
      return this.song;
    }
    return null;
  }
  // tracksExpected: set to K.v2Tracks for firmware 0.1/0.2 (only used for the progress readout).
  get progress() { return Math.min(1, this.chunks / (K.patterns * (this.tracksExpected || K.tracks) * (K.rows / K.chunkRows))); }
}

// Same demo as song_demo() in src/engine/song.h (used offline).
export function demoSong() {
  const s = new Song();
  s.name = 'DEMO'; s.tempo = 118; s.swing = 56; s.songLen = 4;
  s.setEntry(0, 0, 2); s.setEntry(1, 1, 2); s.setEntry(2, 0, 1); s.setEntry(3, 2, 1);
  const set = (p, t, r, note, vel = 0, fx = 0, fxv = 0) => s.setCell(p, t, r, { note, vel, fx, fxv });
  for (let p = 0; p < 3; p++) for (let r = 0; r < 16; r++) {
    if (r % 4 === 0) set(p, 0, r, 36, 110);
    if (r % 8 === 4) set(p, 1, r, 50, 100);
    if (r % 2 === 0) set(p, 2, r, 80, r % 4 === 2 ? 100 : 50);
  }
  const pluck = [48, 0, 0, 55, 0, 0, 51, 0, 48, 0, 58, 0, 0, 55, 0, 0];
  const vco1 = [12, 0, 0, 0, 19, 0, 0, 0, 15, 0, 0, 0, 17, 0, 19, 0]; // C0 = 0 V
  const vco2 = [36, 0, 39, 0, 43, 0, 39, 0, 36, 0, 46, 0, 43, 0, 39, 0];
  for (let r = 0; r < 16; r++) {
    if (pluck[r]) set(0, 3, r, pluck[r], 96);
    if (vco1[r]) set(0, 4, r, vco1[r]);
    if (vco2[r]) set(0, 5, r, vco2[r]);
    if (vco1[r]) set(1, 4, r, vco1[r] + 5, 0, FX.S.code, 20);
    if (vco2[r]) set(1, 5, r, vco2[r] + 5, 0, r === 4 ? FX.R.code : 0, r === 4 ? 3 : 0);
  }
  set(1, 1, 14, 50, 60, FX.R.code, 3);
  for (let r = 1; r < 16; r += 2) set(1, 2, r, 84, 40, FX.P.code, 50);
  set(1, 3, 0, 43, 110, FX.D.code, 100);
  set(1, 3, 8, 46, 90);
  for (let r = 8; r < 16; r++) set(2, 1, r, 45 + r, 40 + r * 5);
  set(2, 4, 0, 12, 0, FX.G.code, 64);
  set(2, 5, 0, NOTE_OFF);
  set(2, 2, 15, 84, 70, FX.R.code, 4);
  s.setMix(4, 6, VCA_SRC.inTrack); s.setMix(4, 4, 40); // IN1 on its own track
  for (let p = 0; p < 3; p++) for (let r = 0; r < 16; r += 2) set(p, K.firstIn, r, 60, r % 4 === 0 ? 127 : 70);
  return s;
}
