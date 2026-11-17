// Workshop Tracker — song model and USB-MIDI SysEx protocol (browser + Node).
// Mirrors src/engine/song.h and src/engine/protocol.h; see docs/PROTOCOL.md.
// SPDX-License-Identifier: MIT

export const MFR = 0x7d;
export const DEVICE = 0x54; // 'T'
export const PROTOCOL_VERSION = 2; // v2: SetMacroMask, Panel report

export const K = {
  drumTracks: 4,
  cvTracks: 2,
  tracks: 6,
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

// Byte offsets inside the 212-byte song header (struct SongHeader).
export const OFF = {
  name: 0,
  tempoHi: 12,
  tempoLo: 13,
  swing: 14,
  songLen: 15,
  cvZero: 16,
  macroExcl: 17, // [0] decay macro (Main), [1] colour macro (X): bit set = voice NOT affected
  reserved: 19,
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
  { name: 'Kick', a: 'Sweep', b: 'Drive', info: 'Sine with pitch sweep and drive' },
  { name: 'Pluck', a: 'Bright', b: 'Stretch', info: 'Karplus-Strong string' },
  { name: 'Noise', a: 'Reso', b: 'Sweep', info: 'Pitched band-pass noise' },
  { name: 'Metal', a: 'Ratio', b: 'Index', info: '2-op FM, inharmonic ratios' },
  { name: 'Modal', a: 'Material', b: 'Strike', info: '3-mode resonator (bars, bells, glass...)' },
  { name: 'Snare', a: 'Noise', b: 'Snap/Clap', info: 'Body + noise; B > 64 = clap' },
];
export const MATERIALS = ['Membrane', 'Marimba', 'Glock', 'Bell', 'Tube', 'Plate', 'Glass', 'Gong'];

export const DRUM_PARAMS = ['engine', 'decay', 'a', 'b', 'tune', 'velSens'];
export const CV_PARAMS = ['gate', 'glide', 'transpose'];
export const MIX_PARAMS = ['level', 'pan', 'drive', 'tone', 'vca', 'mute'];
export const MASTER_PARAMS = ['level', 'drive', 'filter'];

export const FX = {
  A: { code: 0x41, drum: true, cv: false, help: 'Param A lock (00-7F)' },
  B: { code: 0x42, drum: true, cv: false, help: 'Param B lock (00-7F)' },
  D: { code: 0x44, drum: true, cv: false, help: 'Decay lock (00-7F)' },
  G: { code: 0x47, drum: false, cv: true, help: 'Gate length in 1/16 row (00 = tie)' },
  S: { code: 0x53, drum: false, cv: true, help: 'Slide/glide time (legato)' },
  R: { code: 0x52, drum: true, cv: true, help: 'Ratchet: 02-08 hits in the row' },
  P: { code: 0x50, drum: true, cv: true, help: 'Probability in % (00-64 hex = 0-100)' },
  K: { code: 0x4b, drum: true, cv: true, help: 'Delay by n/16 row (01-0F)' },
};

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
    this.cells = new Uint8Array(K.patterns * K.tracks * K.rows * 4);
    this.clear();
  }

  static cellIndex(p, t, r) { return ((p * K.tracks + t) * K.rows + r) * 4; }

  getCell(p, t, r) {
    const i = Song.cellIndex(p, t, r);
    const c = this.cells;
    return { note: c[i], vel: c[i + 1], fx: c[i + 2], fxv: c[i + 3] };
  }
  setCell(p, t, r, cell) {
    const i = Song.cellIndex(p, t, r);
    this.cells[i] = clamp(cell.note | 0, 0, 127);
    this.cells[i + 1] = clamp(cell.vel | 0, 0, 127);
    this.cells[i + 2] = clamp(cell.fx | 0, 0, 127);
    this.cells[i + 3] = clamp(cell.fxv | 0, 0, 127);
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
  master(p) { return this.header[OFF.master + p]; }
  setMaster(p, x) { this.header[OFF.master + p] = clamp(x | 0, 0, 127); }

  // Same defaults as song_clear() in src/engine/song.h.
  clear() {
    this.header.fill(0);
    this.cells.fill(0);
    this.name = 'NEW SONG';
    this.tempo = 120;
    this.swing = 50;
    this.songLen = 1;
    this.cvZero = 36;
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
    this.setMaster(0, 120); this.setMaster(1, 10); this.setMaster(2, 64);
  }

  toJSON() {
    return {
      format: 'workshop-tracker-song', version: 1,
      header: Array.from(this.header),
      cells: btoaBytes(this.cells),
    };
  }
  static fromJSON(o) {
    if (!o || o.format !== 'workshop-tracker-song') throw new Error('Not a Workshop Tracker song file');
    const s = new Song();
    if (o.header.length !== K.headerBytes) throw new Error('Bad header size');
    s.header.set(o.header.map((v) => v & 127));
    const cells = atobBytes(o.cells);
    if (cells.length !== s.cells.length) throw new Error('Bad pattern data size');
    s.cells.set(cells.map((v) => v & 127));
    return s;
  }
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

export const msg = {
  hello: () => frame(CMD.Hello),
  getSong: () => frame(CMD.GetSong),
  ping: () => frame(CMD.Ping),
  setCell: (p, t, r, c) => frame(CMD.SetCell, [p, t, r, c.note, c.vel, c.fx, c.fxv]),
  setCells: (song, p, t, r0, count) => {
    const pl = [p, t, r0, count];
    for (let i = 0; i < count; i++) { const c = song.getCell(p, t, r0 + i); pl.push(c.note, c.vel, c.fx, c.fxv); }
    return frame(CMD.SetCells, pl);
  },
  setPatLen: (p, len) => frame(CMD.SetPatLen, [p, len]),
  setDrum: (v, p, x) => frame(CMD.SetDrum, [v, p, x]),
  setCv: (t, p, x) => frame(CMD.SetCv, [t, p, x]),
  setMix: (ch, p, x) => frame(CMD.SetMix, [ch, p, x]),
  setMaster: (p, x) => frame(CMD.SetMaster, [p, x]),
  setEntry: (i, pat, rep) => frame(CMD.SetEntry, [i, pat, rep]),
  setSongLen: (n) => frame(CMD.SetSongLen, [n]),
  setTempo: (bpm) => frame(CMD.SetTempo, [bpm >> 7, bpm & 127]),
  setSwing: (v) => frame(CMD.SetSwing, [v]),
  setName: (song) => frame(CMD.SetName, Array.from(song.header.slice(OFF.name, OFF.name + K.nameLen))),
  setZeroNote: (n) => frame(CMD.SetZeroNote, [n]),
  setMacroMask: (m, bits) => frame(CMD.SetMacroMask, [m, bits & 0x0f]),
  setHeader: (song) => frame(CMD.SetHeader, Array.from(song.header)),
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
export function uploadMessages(song) {
  const out = [msg.setHeader(song)];
  for (let p = 0; p < K.patterns; p++)
    for (let t = 0; t < K.tracks; t++)
      for (let r = 0; r < K.rows; r += K.chunkRows) out.push(msg.setCells(song, p, t, r, K.chunkRows));
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
    case CMD.PatternChunk:
      return { type: 'chunk', pattern: p[0], track: p[1], row: p[2], count: p[3], cells: p.slice(4, 4 + p[3] * 4) };
    case CMD.DumpEnd:
      return { type: 'dumpEnd' };
    case CMD.Status: {
      if (p.length < 28) return { type: 'status', bad: true };
      return {
        type: 'status',
        mode: p[0], entry: p[1], pattern: p[2], row: p[3], rep: p[4], slot: p[5], flags: p[6],
        extClock: !!(p[6] & 1), nextQueued: !!(p[6] & 2), swapPending: !!(p[6] & 4),
        knobWaiting: !!(p[6] & 8), selectPage: !!(p[6] & 16),
        tempo: p[7] * 128 + p[8], swing: p[9],
        macros: [p[10] - 64, p[11] - 64, p[12] - 64],
        cvNotes: [p[13], p[14]], gates: [!!(p[15] & 1), !!(p[15] & 2)],
        cpu: p[16], cpuPeak: p[17],
        peaks: Array.from(p.slice(18, 26)),
        hits: p[26], swaps: p[27],
      };
    }
    case CMD.Panel: {
      if (p.length < 27) return { type: 'panel', bad: true };
      return {
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

// Assembles a full song from a GetSong dump.
export class DumpCollector {
  constructor() { this.reset(); }
  reset() { this.song = new Song(); this.gotHeader = false; this.chunks = 0; this.done = false; }
  feed(m) {
    if (m.type === 'header') { this.reset(); this.song.header.set(m.header); this.gotHeader = true; }
    else if (m.type === 'chunk' && this.gotHeader) {
      for (let i = 0; i < m.count; i++) {
        const o = i * 4;
        this.song.setCell(m.pattern, m.track, m.row + i, { note: m.cells[o], vel: m.cells[o + 1], fx: m.cells[o + 2], fxv: m.cells[o + 3] });
      }
      this.chunks++;
    } else if (m.type === 'dumpEnd' && this.gotHeader) {
      this.done = true;
      return this.song;
    }
    return null;
  }
  get progress() { return this.chunks / (K.patterns * K.tracks * (K.rows / K.chunkRows)); }
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
  const vco1 = [36, 0, 0, 0, 43, 0, 0, 0, 39, 0, 0, 0, 41, 0, 43, 0];
  const vco2 = [60, 0, 63, 0, 67, 0, 63, 0, 60, 0, 70, 0, 67, 0, 63, 0];
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
  set(2, 4, 0, 36, 0, FX.G.code, 64);
  set(2, 5, 0, NOTE_OFF);
  return s;
}
