// Workshop Tracker editor UI.
// SPDX-License-Identifier: MIT
import {
  K, Song, msg, decode, DumpCollector, uploadMessages, demoSong, noteName, hex2, clamp,
  ENGINES, MATERIALS, FX, NOTE_OFF, NOTE_EMPTY, PROTOCOL_VERSION, effectiveDrum, effectiveFilter,
} from './protocol.js';
import { Device } from './midi.js';
import { Knob } from './knob.js';

const $ = (id) => document.getElementById(id);
const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };

const TRACK_NAMES = ['Drum 1', 'Drum 2', 'Drum 3', 'Drum 4', 'CV/Gate 1', 'CV/Gate 2'];
const CH_NAMES = ['DR1', 'DR2', 'DR3', 'DR4', 'IN1', 'IN2'];
const STORE_KEY = 'workshopTracker.song';
const BACKUP_KEY = 'workshopTracker.backup';

const state = {
  song: loadLocal() || demoSong(),
  pattern: 0, row: 0, track: 0, sub: 0, // sub: 0 note, 1 vel, 2 fx, 3 fx value
  octave: 4, step: 1, edit: true, follow: true,
  hexHi: null,
  status: null, info: null,
  clipTrack: null, clipPattern: null,
  dump: new DumpCollector(), dumping: false,
  panel: null, panelAt: 0, legacy: false,
};
const dev = new Device();
let pingTimer = null;

// ------------------------------------------------------------ persistence --
function loadLocal() {
  try { const j = localStorage.getItem(STORE_KEY); return j ? Song.fromJSON(JSON.parse(j)) : null; } catch { return null; }
}
let saveTimer = null;
function saveLocal() {
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => { try { localStorage.setItem(STORE_KEY, JSON.stringify(state.song.toJSON())); } catch {} }, 400);
}

function toast(text, ms = 3500) {
  const t = $('toast');
  t.textContent = text;
  t.hidden = false;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => { t.hidden = true; }, ms);
}

// Send to the module if connected. Model edits always happen locally first.
function send(bytes) { if (dev.connected) dev.send(bytes); }
function changed() { saveLocal(); }

// ------------------------------------------------------------------ grid ---
let rowEls = [];
let headHits = [];
function buildGrid() {
  const g = $('grid');
  g.innerHTML = '';
  const head = el('div', 'head');
  head.appendChild(el('span', 'num', ''));
  headHits = [];
  for (let t = 0; t < K.tracks; t++) {
    const h = el('span', 'trk' + (t >= K.drumTracks ? ' cvh' : ''));
    const dot = el('i', 'hit');
    headHits.push(dot);
    h.appendChild(dot);
    const label = t < K.drumTracks ? `DR${t + 1} ${ENGINES[state.song.drum(t, 0)].name}` : `CV${t - K.drumTracks + 1} GATE`;
    h.appendChild(document.createTextNode(label));
    head.appendChild(h);
  }
  g.appendChild(head);
  rowEls = [];
  const len = state.song.patLen(state.pattern);
  for (let r = 0; r < len; r++) {
    const re = el('div', 'r');
    re.appendChild(el('span', 'num', hex2(r)));
    const cells = [];
    for (let t = 0; t < K.tracks; t++) {
      const te = el('span', 'trk');
      const f = [el('span', 'f n'), el('span', 'f v'), el('span', 'f x'), el('span', 'f xv')];
      f.forEach((s, i) => {
        s.dataset.r = r; s.dataset.t = t; s.dataset.s = i;
        te.appendChild(s);
        if (i < 3) te.appendChild(document.createTextNode(i === 2 ? '' : ' '));
      });
      re.appendChild(te);
      cells.push(f);
    }
    g.appendChild(re);
    rowEls.push({ el: re, cells });
    for (let t = 0; t < K.tracks; t++) paintCell(r, t);
  }
  if (state.row >= len) state.row = len - 1;
  paintCursor(true);
  paintPlayhead();
}

function paintCell(r, t) {
  const re = rowEls[r];
  if (!re) return;
  const c = state.song.getCell(state.pattern, t, r);
  const [n, v, x, xv] = re.cells[t];
  n.textContent = noteName(c.note);
  n.classList.toggle('e', c.note === NOTE_EMPTY);
  n.classList.toggle('off', c.note === NOTE_OFF);
  v.textContent = c.vel ? hex2(c.vel) : '..';
  v.classList.toggle('e', !c.vel);
  x.textContent = c.fx ? String.fromCharCode(c.fx) : '.';
  x.classList.toggle('e', !c.fx);
  xv.textContent = c.fx || c.fxv ? hex2(c.fxv) : '..';
  xv.classList.toggle('e', !c.fx && !c.fxv);
}

let lastCursor = null;
function paintCursor(scroll) {
  if (lastCursor) lastCursor.classList.remove('c');
  const re = rowEls[state.row];
  rowEls.forEach((x, i) => x.el.classList.toggle('cur', i === state.row));
  if (!re) return;
  const s = re.cells[state.track][state.sub];
  s.classList.add('c');
  lastCursor = s;
  if (scroll) s.scrollIntoView({ block: 'nearest', inline: 'nearest' });
  showFxHelp();
  selectVoicePanel(state.track);
}

function paintPlayhead() {
  const st = state.status;
  const playing = st && st.mode !== 0 && st.pattern === state.pattern;
  rowEls.forEach((x, i) => x.el.classList.toggle('play', !!playing && i === st.row));
  if (playing && state.follow && rowEls[st.row]) rowEls[st.row].el.scrollIntoView({ block: 'nearest' });
}

function showFxHelp() {
  const isCv = state.track >= K.drumTracks;
  const lines = Object.entries(FX).filter(([, f]) => (isCv ? f.cv : f.drum)).map(([k, f]) => `<div><b>${k}</b> ${f.help}</div>`);
  $('fxHelp').innerHTML = `<summary>FX column · ${isCv ? 'CV track' : 'drum track'}</summary>${lines.join('')}` +
    (isCv ? '<div class="dim">Velocity is ignored on CV tracks. Notes hold until the gate length (voice setting) or OFF.</div>'
          : '<div class="dim">Velocity .. = 100 (hex 64). OFF chokes the voice.</div>');
}

// --------------------------------------------------------------- editing ---
function setCell(r, t, cell, preview = true) {
  state.song.setCell(state.pattern, t, r, cell);
  paintCell(r, t);
  send(msg.setCell(state.pattern, t, r, state.song.getCell(state.pattern, t, r)));
  changed();
  if (preview && cell.note && cell.note !== NOTE_OFF) send(msg.preview(t, cell.note, cell.vel || 100));
}

function moveCursor(dr, dt, ds) {
  const len = state.song.patLen(state.pattern);
  state.hexHi = null;
  if (dr) state.row = ((state.row + dr) % len + len) % len;
  if (dt) { state.track = clamp(state.track + dt, 0, K.tracks - 1); }
  if (ds) {
    let s = state.sub + ds, t = state.track;
    if (s > 3) { if (t < K.tracks - 1) { t++; s = 0; } else s = 3; }
    if (s < 0) { if (t > 0) { t--; s = 3; } else s = 0; }
    state.sub = s; state.track = t;
  }
  paintCursor(true);
}

const NOTE_KEYS = {
  KeyZ: 0, KeyS: 1, KeyX: 2, KeyD: 3, KeyC: 4, KeyV: 5, KeyG: 6, KeyB: 7, KeyH: 8, KeyN: 9, KeyJ: 10, KeyM: 11,
  Comma: 12, KeyL: 13, Period: 14,
  KeyQ: 12, Digit2: 13, KeyW: 14, Digit3: 15, KeyE: 16, KeyR: 17, Digit5: 18, KeyT: 19, Digit6: 20, KeyY: 21, Digit7: 22, KeyU: 23,
  KeyI: 24, Digit9: 25, KeyO: 26, Digit0: 27, KeyP: 28,
};

function onGridKey(e) {
  if (e.target.closest('input,select,textarea') && e.target !== $('grid')) return;
  const k = e.code;
  const ctrl = e.ctrlKey || e.metaKey;
  if (ctrl && k === 'KeyC') { copyTrack(); e.preventDefault(); return; }
  if (ctrl && k === 'KeyV') { pasteTrack(); e.preventDefault(); return; }
  if (ctrl) return;
  switch (k) {
    case 'ArrowUp': moveCursor(-1, 0, 0); e.preventDefault(); return;
    case 'ArrowDown': moveCursor(1, 0, 0); e.preventDefault(); return;
    case 'ArrowLeft': moveCursor(0, 0, -1); e.preventDefault(); return;
    case 'ArrowRight': moveCursor(0, 0, 1); e.preventDefault(); return;
    case 'PageUp': moveCursor(-16, 0, 0); e.preventDefault(); return;
    case 'PageDown': moveCursor(16, 0, 0); e.preventDefault(); return;
    case 'Home': state.row = 0; moveCursor(0, 0, 0); e.preventDefault(); return;
    case 'End': state.row = state.song.patLen(state.pattern) - 1; moveCursor(0, 0, 0); e.preventDefault(); return;
    case 'Tab': state.sub = 0; moveCursor(0, e.shiftKey ? -1 : 1, 0); e.preventDefault(); return;
    case 'Space': togglePlayPattern(); e.preventDefault(); return;
    case 'Enter': send(msg.playSong(0)); e.preventDefault(); return;
    case 'Minus': setOctave(state.octave - 1); e.preventDefault(); return;
    case 'Equal': setOctave(state.octave + 1); e.preventDefault(); return;
    default: break;
  }
  const c = state.song.getCell(state.pattern, state.track, state.row);
  if (k === 'Delete' || k === 'Backspace') {
    if (state.sub === 0) setCell(state.row, state.track, { note: 0, vel: 0, fx: 0, fxv: 0 }, false);
    else if (state.sub === 1) setCell(state.row, state.track, { ...c, vel: 0 }, false);
    else setCell(state.row, state.track, { ...c, fx: 0, fxv: 0 }, false);
    if (state.edit) moveCursor(state.step, 0, 0);
    e.preventDefault();
    return;
  }
  if (state.sub === 0) {
    let note = null;
    if (k === 'Digit1') note = NOTE_OFF;
    else if (k in NOTE_KEYS) note = clamp((state.octave + 1) * 12 + NOTE_KEYS[k], 1, 126);
    if (note == null) return;
    e.preventDefault();
    if (e.repeat) return;
    if (!state.edit) { if (note !== NOTE_OFF) send(msg.preview(state.track, note, 100)); return; }
    setCell(state.row, state.track, { ...c, note });
    moveCursor(state.step, 0, 0);
    return;
  }
  if (!state.edit) return;
  if (state.sub === 2) {
    const L = e.key.toUpperCase();
    if (L.length === 1 && FX[L]) {
      const isCv = state.track >= K.drumTracks;
      if (!(isCv ? FX[L].cv : FX[L].drum)) { toast(`Effect ${L} does not apply to ${isCv ? 'CV' : 'drum'} tracks`); return; }
      setCell(state.row, state.track, { ...c, fx: FX[L].code, fxv: c.fxv || defaultFxValue(L) }, false);
      moveCursor(0, 0, 1);
      e.preventDefault();
    }
    return;
  }
  // Hex digits in velocity / fx value.
  const h = /^[0-9a-fA-F]$/.test(e.key) ? parseInt(e.key, 16) : null;
  if (h == null) return;
  e.preventDefault();
  if (state.hexHi == null) {
    if (h > 7) { toast('Values go up to 7F (127)'); return; }
    state.hexHi = h;
    const v = h << 4;
    setCell(state.row, state.track, state.sub === 1 ? { ...c, vel: v } : { ...c, fxv: v }, false);
  } else {
    const v = (state.hexHi << 4) | h;
    state.hexHi = null;
    setCell(state.row, state.track, state.sub === 1 ? { ...c, vel: v } : { ...c, fxv: v }, false);
    moveCursor(state.step, 0, 0);
  }
}

function defaultFxValue(L) {
  return { R: 2, P: 50, G: 8, S: 20, K: 8 }[L] ?? 64;
}

function copyTrack() {
  const arr = [];
  for (let r = 0; r < K.rows; r++) arr.push(state.song.getCell(state.pattern, state.track, r));
  state.clipTrack = arr;
  toast(`Copied ${TRACK_NAMES[state.track]} of pattern ${hex2(state.pattern)}`, 1500);
}
function pasteTrack() {
  if (!state.clipTrack) return;
  for (let r = 0; r < K.rows; r++) state.song.setCell(state.pattern, state.track, r, state.clipTrack[r]);
  sendTrack(state.pattern, state.track);
  buildGrid();
  changed();
}
function sendTrack(p, t) {
  for (let r = 0; r < K.rows; r += K.chunkRows) send(msg.setCells(state.song, p, t, r, K.chunkRows));
}

// ------------------------------------------------------------- transport ---
function togglePlayPattern() {
  if (state.status && state.status.mode !== 0) send(msg.stop());
  else send(msg.playPattern(state.pattern));
  if (!dev.connected) toast('Not connected: connect the module to hear it');
}

function setOctave(o) { state.octave = clamp(o, 0, 8); $('octave').value = state.octave; }

// ----------------------------------------------------------- arrangement ---
function buildArrangement() {
  const box = $('arrangement');
  box.innerHTML = '';
  for (let i = 0; i < state.song.songLen; i++) {
    const e = state.song.entry(i);
    const row = el('div', 'entry');
    row.dataset.i = i;
    row.appendChild(el('span', 'idx', String(i + 1).padStart(2, '0')));
    const sel = el('select');
    for (let p = 0; p < K.patterns; p++) { const o = el('option', null, 'P' + hex2(p)); o.value = p; sel.appendChild(o); }
    sel.value = e.pattern;
    sel.onchange = () => { state.song.setEntry(i, +sel.value, state.song.entry(i).repeats); send(msg.setEntry(i, +sel.value, state.song.entry(i).repeats)); changed(); };
    const rep = el('input'); rep.type = 'number'; rep.min = 1; rep.max = 16; rep.value = e.repeats; rep.title = 'repeats';
    rep.onchange = () => { const v = clamp(+rep.value || 1, 1, 16); rep.value = v; state.song.setEntry(i, state.song.entry(i).pattern, v); send(msg.setEntry(i, state.song.entry(i).pattern, v)); changed(); };
    const edit = el('button', null, '✎'); edit.title = 'Edit this pattern';
    edit.onclick = () => selectPattern(state.song.entry(i).pattern);
    const cue = el('button', null, '▶'); cue.title = 'Play from here (or jump here at the end of the pattern while playing)';
    cue.onclick = () => { if (state.status && state.status.mode === 1) send(msg.cue(i)); else send(msg.playSong(i)); };
    row.append(sel, el('span', 'dim', '×'), rep, edit, cue);
    box.appendChild(row);
  }
  paintArrangementPos();
}
function paintArrangementPos() {
  const st = state.status;
  document.querySelectorAll('.entry').forEach((r) => r.classList.toggle('current', !!st && st.mode === 1 && +r.dataset.i === st.entry));
}

function selectPattern(p) {
  state.pattern = clamp(p, 0, K.patterns - 1);
  $('pattern').value = state.pattern;
  $('patLen').value = state.song.patLen(state.pattern);
  buildGrid();
  if (state.status && state.status.mode === 2) send(msg.cue(state.pattern));
}

// --------------------------------------------------------------- modules ---
const fmtDecay = (v) => { const s = 0.005 * Math.pow(800, v / 127); return s < 1 ? `${Math.round(s * 1000)}ms` : `${s.toFixed(1)}s`; };
const fmtSigned = (v) => { const d = v - 64; return (d > 0 ? '+' : '') + d; };
const fmtPan = (v) => (v === 64 ? 'C' : v < 64 ? `L${64 - v}` : `R${v - 64}`);
const fmtTone = (v) => (v < 60 ? `LP${60 - v}` : v > 68 ? `HP${v - 68}` : 'flat');
const fmtFilter = (v) => (v < 61 ? `LP${61 - v}` : v > 67 ? `HP${v - 67}` : 'off');
const fmtA = (eng) => (v) => (eng === 4 ? MATERIALS[v >> 4].slice(0, 8) : eng === 3 ? `r${v >> 3}` : String(v));
const fmtMacro = (v) => (v > 0 ? '+' : '') + v;

// knobs.drum[v][param], knobs.cv[t][param], knobs.mix[ch][param], knobs.master[param]
const knobs = { drum: [], cv: [], mix: [], master: [] };
let voicePanels = [];
let voiceLeds = [];
let gateLeds = [];
let routeBtns = []; // routeBtns[v][macro]
let lastMaskEdit = 0;

function moduleEl(title, cls = '') {
  const m = el('div', 'module ' + cls);
  for (const c of ['tl', 'tr', 'bl', 'br']) m.appendChild(el('i', 'screw ' + c));
  const t = el('div', 'mtitle');
  t.append(el('b', null, title));
  const led = el('i', 'led');
  t.append(led);
  m.appendChild(t);
  m.led = led;
  return m;
}
function jacks(names) {
  const j = el('div', 'jacks');
  for (const n of names) { const x = el('span', 'jack'); x.append(el('i'), document.createTextNode(n)); j.appendChild(x); }
  return j;
}
function krow(...ks) { const r = el('div', 'krow'); ks.forEach((k) => r.appendChild(k.el)); return r; }

function buildVoices() {
  const box = $('voices');
  box.innerHTML = '';
  voicePanels = []; voiceLeds = []; routeBtns = []; knobs.drum = [];
  const s = state.song;
  for (let v = 0; v < K.drumTracks; v++) {
    const m = moduleEl(`DRUM ${v + 1}`);
    voiceLeds.push(m.led);
    const set = (param) => (x) => { s.setDrum(v, param, x); send(msg.setDrum(v, param, x)); changed(); paintMods(); };
    const eng = el('select', 'engine');
    eng.title = 'Synthesis engine';
    ENGINES.forEach((e, i) => { const o = el('option', null, e.name); o.value = i; o.title = e.info; eng.appendChild(o); });
    eng.value = s.drum(v, 0);
    const sub = el('div', 'msub', ENGINES[s.drum(v, 0)].info);
    // Which PERFORM macros act on this voice.
    const routes = el('div', 'routes');
    routeBtns[v] = [];
    [['MAIN', 'Main knob (decay macro)'], ['X', 'X knob (colour macro, param A)']].forEach(([name, what], mi) => {
      const b = el('button');
      b.append(el('i', 'led'), document.createTextNode(name));
      b.title = `${what} acts on this voice in PERFORM mode. Click to toggle. Stored in the song.`;
      b.onclick = () => {
        const bits = s.macroMask(mi) ^ (1 << v);
        s.setMacroMask(mi, bits);
        lastMaskEdit = performance.now();
        send(msg.setMacroMask(mi, bits));
        changed(); paintRoutes(); paintMods();
      };
      routes.appendChild(b);
      routeBtns[v][mi] = b;
    });
    const e0 = s.drum(v, 0);
    const kd = new Knob({ label: 'Decay', value: s.drum(v, 1), def: 64, fmt: fmtDecay, size: 'l', onChange: set(1) });
    const ka = new Knob({ label: ENGINES[e0].a, value: s.drum(v, 2), def: 64, fmt: fmtA(e0), onChange: set(2) });
    const kb = new Knob({ label: ENGINES[e0].b, value: s.drum(v, 3), def: 32, onChange: set(3) });
    const kt = new Knob({ label: 'Tune', min: 16, max: 112, value: s.drum(v, 4), def: 64, fmt: fmtSigned, size: 's', bipolar: true, onChange: set(4) });
    const kv = new Knob({ label: 'Vel', value: s.drum(v, 5), def: 100, size: 's', onChange: set(5), title: 'Velocity sensitivity' });
    knobs.drum[v] = [null, kd, ka, kb, kt, kv];
    eng.onchange = () => {
      const e = +eng.value;
      set(0)(e);
      sub.textContent = ENGINES[e].info;
      ka.setLabel(ENGINES[e].a); ka.setFmt(fmtA(e));
      kb.setLabel(ENGINES[e].b);
      buildGrid();
    };
    const aud = el('button', 'audition', 'TRIG');
    aud.title = 'Play this voice on the module';
    aud.onclick = () => send(msg.preview(v, 48, 110));
    m.append(eng, sub, routes, kd.el, krow(ka, kb), krow(kt, kv), el('div', 'spacer'), aud, jacks(['TRIG', 'OUT']));
    box.appendChild(m);
    voicePanels.push(m);
  }
  paintRoutes();
  buildCv();
  paintMods();
}

function paintRoutes() {
  const s = state.song;
  routeBtns.forEach((row, v) => row.forEach((b, mi) => {
    const on = s.macroOn(mi, v);
    b.classList.toggle('on', on);
    b.querySelector('.led').classList.toggle('lit', on);
    b.disabled = !!state.legacy;
    b.setAttribute('aria-pressed', String(on));
  }));
}

function buildCv() {
  const box = $('cvpanel');
  box.innerHTML = '';
  gateLeds = []; knobs.cv = [];
  const s = state.song;
  for (let c = 0; c < K.cvTracks; c++) {
    const m = moduleEl(`CV/GATE ${c + 1}`, 'voice-cv');
    gateLeds.push(m.led);
    const set = (param) => (x) => { s.setCv(c, param, x); send(msg.setCv(c, param, x)); changed(); };
    const kg = new Knob({ label: 'Gate', min: 0, max: 64, value: s.cv(c, 0), def: 8, size: 'l', fmt: (v) => (v === 0 ? 'tie' : `${(v / 16).toFixed(2)}row`), onChange: set(0) });
    const kgl = new Knob({ label: 'Glide', value: s.cv(c, 1), def: 0, fmt: (v) => (v === 0 ? 'off' : `${Math.round((48 + v * v * 6) / 48)}ms`), onChange: set(1) });
    const ktr = new Knob({ label: 'Transp', min: 16, max: 112, value: s.cv(c, 2), def: 64, fmt: fmtSigned, bipolar: true, onChange: set(2) });
    knobs.cv[c] = [kg, kgl, ktr];
    const test = el('button', 'audition', 'TEST');
    test.title = 'Play a test note on this CV/gate output';
    test.onclick = () => send(msg.preview(K.drumTracks + c, 48, 100));
    m.append(el('div', 'msub', `CV out ${c + 1} = pitch · pulse out ${c + 1} = gate`), kg.el, krow(kgl, ktr), el('div', 'spacer'), test, jacks(['CV', 'GATE']));
    box.appendChild(m);
    voicePanels.push(m);
  }
  const g = moduleEl('CV RANGE', 'voice-cv');
  const kz = new Knob({ label: '0 V note', min: 0, max: 96, value: s.cvZero, def: 36, size: 'l', fmt: (v) => noteName(v), onChange: (x) => { s.cvZero = x; send(msg.setZeroNote(x)); changed(); } });
  knobs.cvZero = kz;
  g.append(el('div', 'msub', '1 V/oct, 0…+6 V'), kz.el, el('div', 'msub', 'This note = 0 V. Lower notes stay at 0 V.'), el('div', 'spacer'), jacks(['CV1', 'CV2']));
  box.appendChild(g);
}

function selectVoicePanel(t) { voicePanels.forEach((p, i) => p.classList.toggle('sel', i === t)); }

// ----------------------------------------------------------------- mixer ---
let meters = [];
let muteBtns = [];
function buildMixer() {
  const box = $('mixer');
  box.innerHTML = '';
  meters = []; muteBtns = []; knobs.mix = [];
  const s = state.song;
  for (let ch = 0; ch < K.channels; ch++) {
    const isIn = ch >= K.drumTracks;
    const m = moduleEl(CH_NAMES[ch], 'strip' + (isIn ? ' in' : ''));
    const met = el('div', 'meter'); const bar = el('i'); met.appendChild(bar); meters.push(bar);
    const set = (param) => (x) => { s.setMix(ch, param, x); send(msg.setMix(ch, param, x)); changed(); };
    const k = [
      new Knob({ label: 'Level', value: s.mix(ch, 0), def: 110, onChange: set(0) }),
      new Knob({ label: 'Pan', value: s.mix(ch, 1), def: 64, fmt: fmtPan, size: 's', bipolar: true, onChange: set(1) }),
      new Knob({ label: 'Drive', value: s.mix(ch, 2), def: 0, size: 's', onChange: set(2) }),
      new Knob({ label: 'Tone', value: s.mix(ch, 3), def: 64, fmt: fmtTone, size: 's', bipolar: true, onChange: set(3) }),
    ];
    knobs.mix[ch] = k;
    m.append(met, ...k.map((x) => x.el));
    if (isIn) {
      const vca = new Knob({ label: 'Gate VCA', value: s.mix(ch, 4), def: 0, size: 's', fmt: (v) => (v ? fmtDecay(v) : 'off'), onChange: set(4),
        title: `Audio in ${ch - K.drumTracks + 1}: opens with CV/gate track ${ch - K.drumTracks + 1}; value = release time (off = always open)` });
      k.push(vca);
      m.appendChild(vca.el);
      m.title = `IN${ch - K.drumTracks + 1} = audio input ${ch - K.drumTracks + 1} (e.g. a VCO)`;
    }
    const mute = el('button', 'mute');
    mute.append(el('i', 'led'), document.createTextNode('MUTE'));
    mute.onclick = () => { const v = s.mix(ch, 5) >= 64 ? 0 : 127; set(5)(v); paintMutes(); };
    muteBtns.push(mute);
    m.append(el('div', 'spacer'), mute);
    box.appendChild(m);
  }
  const ms = moduleEl('MASTER', 'strip master');
  const mm = el('div', 'meters');
  for (let i = 0; i < 2; i++) { const met = el('div', 'meter'); const bar = el('i'); met.appendChild(bar); meters.push(bar); mm.appendChild(met); }
  const setM = (param) => (x) => { s.setMaster(param, x); send(msg.setMaster(param, x)); changed(); paintMods(); };
  knobs.master = [
    new Knob({ label: 'Level', value: s.master(0), def: 120, onChange: setM(0) }),
    new Knob({ label: 'Drive', value: s.master(1), def: 10, size: 's', onChange: setM(1) }),
    new Knob({ label: 'Filter', value: s.master(2), def: 64, fmt: fmtFilter, size: 'l', bipolar: true, onChange: setM(2),
      title: 'Master filter: left = low-pass, centre = off, right = high-pass. The module\'s Y knob and CV in 2 add to it (red).' }),
  ];
  ms.append(mm, ...knobs.master.map((x) => x.el), el('div', 'spacer'), jacks(['L', 'R']));
  box.appendChild(ms);
  paintMutes();
  paintMods();
}
function paintMutes() {
  muteBtns.forEach((b, ch) => { const on = state.song.mix(ch, 5) >= 64; b.classList.toggle('on', on); b.querySelector('.led').classList.toggle('lit', on); b.setAttribute('aria-pressed', String(on)); });
}

// Red "live" arcs: the module's PERFORM macros as offsets on the song values.
function liveMacros() {
  if (!dev.connected) return null;
  if (state.panel) return state.panel.macros;
  if (state.status) return state.status.macros;
  return null;
}
function paintMods() {
  // Old firmware (protocol 1) has no routing: its macros act on every voice.
  const s = state.legacy ? { drum: (v, p) => state.song.drum(v, p), master: (p) => state.song.master(p), macroOn: () => true } : state.song;
  const mac = liveMacros() || [0, 0, 0];
  const p = dev.connected && state.panel;
  for (let v = 0; v < knobs.drum.length; v++) {
    const kk = knobs.drum[v];
    if (!kk) continue;
    kk[1].setMod(p ? p.effDecay[v] : effectiveDrum(s, mac, v, 1));
    kk[2].setMod(p ? p.effA[v] : effectiveDrum(s, mac, v, 2));
  }
  if (knobs.master[2]) knobs.master[2].setMod(p ? p.effFilter : effectiveFilter(s, mac));
}

// ------------------------------------------------- module panel (live) ----
const comp = { knobs: [], leds: [], z: [], page: null, note: null, led: null };
function buildComputer() {
  const box = $('computer');
  box.innerHTML = '';
  const t = el('div', 'mtitle');
  t.append(el('b', null, 'COMPUTER · LIVE'));
  comp.led = el('i', 'led');
  comp.led.title = 'Lit while the module reports its panel';
  t.append(comp.led);
  box.appendChild(t);
  const row = el('div', 'krow');
  comp.knobs = [0, 1, 2].map((i) => {
    const k = new Knob({ label: ['Main', 'X', 'Y'][i], value: 64, min: 0, max: 127, size: 's', readOnly: true, fmt: (v) => compFmt(i, v) });
    row.appendChild(k.el);
    return k;
  });
  box.appendChild(row);
  const pg = el('div', 'page');
  const z = el('span', 'z');
  comp.z = [el('i'), el('i'), el('i')];
  comp.z.forEach((x) => z.appendChild(x));
  comp.page = el('b', null, '—');
  const left = el('span');
  left.append(z, comp.page);
  comp.extra = el('span', null, '');
  pg.append(left, comp.extra);
  box.appendChild(pg);
  comp.note = el('div', 'note', 'Connect to see the module\'s knobs.');
  box.appendChild(comp.note);
}
function compFmt(i, v) {
  const p = state.panel, st = state.status;
  const select = p ? p.selectPage : st ? st.selectPage : false;
  if (select) {
    if (i === 0) return `slot ${(p ? p.slot : st ? st.slot : 0) + 1}`;
    if (i === 1) return `${state.song.tempo}bpm`;
    return `${state.song.swing}%`;
  }
  const mac = liveMacros() || [0, 0, 0];
  return fmtMacro(mac[i]);
}
function paintComputer() {
  const p = dev.connected ? state.panel : null;
  const st = dev.connected ? state.status : null;
  comp.led.classList.toggle('lit', !!p && performance.now() - state.panelAt < 1500);
  if (!p && !st) {
    comp.page.textContent = '—';
    comp.extra.textContent = '';
    comp.note.textContent = dev.connected ? 'Waiting for the module…' : 'Connect to see the module\'s knobs.';
    comp.knobs.forEach((k) => { k.set(64); k.el.classList.remove('waiting'); });
    comp.z.forEach((x) => x.classList.remove('on'));
    return;
  }
  const select = p ? p.selectPage : st.selectPage;
  const down = p ? p.downHeld : false;
  const labels = select ? ['Slot', 'Tempo', 'Swing'] : ['Decay', 'Colour', 'Filter'];
  const mac = liveMacros() || [0, 0, 0];
  comp.knobs.forEach((k, i) => {
    k.setLabel(labels[i]);
    // v1 firmware has no knob positions: approximate from the macro value.
    k.set(p ? p.knobs[i] : select ? 64 : mac[i] + 64);
    const waiting = p ? p.waiting[i] : false;
    k.lab.classList.toggle('blinktext', waiting);
    k.el.title = waiting ? 'Waiting for pickup: turn the module knob past the current value' : '';
  });
  comp.z[0].classList.toggle('on', select);
  comp.z[1].classList.toggle('on', !select && !down);
  comp.z[2].classList.toggle('on', down);
  comp.page.textContent = down ? 'Z DOWN' : select ? 'SELECT' : 'PERFORM';
  comp.extra.textContent = p && p.transpose ? `CV1 ${p.transpose > 0 ? '+' : ''}${p.transpose}st` : '';
  const waitAny = p ? p.waiting.some(Boolean) : st && st.knobWaiting;
  comp.note.textContent = state.legacy ? 'Old firmware: approximate (no knob positions).'
    : waitAny ? 'Blinking label = knob waits for pickup.'
    : select ? 'Main = slot, X = tempo, Y = swing.' : 'Macros are live offsets, not saved.';
}

// ----------------------------------------------------------- whole song ---
function refreshAll() {
  const s = state.song;
  $('tempo').value = s.tempo;
  $('swing').value = s.swing; $('swingOut').textContent = s.swing + '%';
  $('songName').value = s.name;
  $('patLen').value = s.patLen(state.pattern);
  buildArrangement();
  buildVoices();
  buildMixer();
  buildGrid();
}

function replaceSong(song, why) {
  state.song = song;
  saveLocal();
  refreshAll();
  if (why) toast(why);
}

// ---------------------------------------------------------- device I/O ----
function onMessage(data) {
  const m = decode(data);
  if (!m) return;
  switch (m.type) {
    case 'info':
      state.info = m;
      $('fwInfo').textContent = `fw ${m.firmware}`;
      fillSlots(m.validSlots, m.slot);
      state.legacy = m.protocol < 2;
      if (m.protocol < PROTOCOL_VERSION) {
        banner(`The module runs firmware ${m.firmware} (protocol ${m.protocol}). Editing and playback work, but knob feedback and ` +
          `MAIN/X voice routing need firmware 0.2 or newer: flash the new workshop_tracker.uf2.`);
      } else if (m.protocol > PROTOCOL_VERSION) {
        banner(`The module speaks protocol ${m.protocol}, this editor ${PROTOCOL_VERSION}. Reload the page or update the editor; some features may not work.`);
      } else hideBanner();
      paintRoutes();
      paintComputer();
      break;
    case 'header': case 'chunk': {
      const song = state.dump.feed(m);
      if (state.dumping) setConn('busy', `reading ${Math.round(state.dump.progress * 100)}%`);
      if (song) {
        state.dumping = false;
        setConn('on', 'connected');
        replaceSong(song, 'Loaded the song from the module.');
      }
      break;
    }
    case 'dumpEnd': {
      const song = state.dump.feed(m);
      state.dumping = false;
      setConn('on', 'connected');
      if (song) replaceSong(song, 'Loaded the song from the module.');
      break;
    }
    case 'status': onStatus(m); break;
    case 'panel': onPanel(m); break;
    case 'ack': onAck(m); break;
    default: break;
  }
}

const MODES = ['STOPPED', 'SONG', 'PATTERN'];
function onStatus(st) {
  if (st.bad) return;
  const prev = state.status;
  state.status = st;
  const cpu = $('cpu');
  cpu.textContent = `${st.cpu}% / ${st.cpuPeak}%`;
  cpu.className = st.cpuPeak > 90 ? 'hot' : st.cpuPeak > 75 ? 'warn' : '';
  let txt = MODES[st.mode] || '?';
  if (st.mode === 1) txt += ` · PART ${st.entry + 1}/${state.song.songLen}`;
  if (st.mode) txt += ` · P${hex2(st.pattern)} · ROW ${hex2(st.row)}`;
  if (st.extClock) txt += ' · EXT CLK';
  if (st.nextQueued) txt += ' · NEXT…';
  $('modeOut').textContent = txt;
  $('beatLed').classList.toggle('lit', st.mode !== 0 && st.row % 4 === 0);
  // Panel changes on the module (tempo / swing knobs) show up here.
  syncTempoSwing(st.tempo, st.swing);
  if (state.follow && st.mode !== 0 && st.pattern !== state.pattern && (!prev || prev.pattern !== st.pattern)) selectPattern(st.pattern);
  paintPlayhead();
  paintArrangementPos();
  st.peaks.forEach((p, i) => { if (meters[i]) meters[i].style.height = `${Math.min(100, (p / 127) * 100)}%`; });
  headHits.forEach((d, i) => d.classList.toggle('lit', i < 4 ? !!(st.hits & (1 << i)) : st.gates[i - 4]));
  for (let v = 0; v < 4; v++) if (st.hits & (1 << v)) flashLed(voiceLeds[v]);
  gateLeds.forEach((l, i) => l.classList.toggle('lit', st.gates[i]));
  $('btnPlaySong').classList.toggle('on', st.mode === 1);
  $('btnPlayPat').classList.toggle('on', st.mode === 2);
  if (state.info && st.slot !== state.info.slot) { state.info.slot = st.slot; $('slot').value = st.slot; }
  if (!state.panel) { paintMods(); paintComputer(); } // v1 firmware: macros come with the status only
}

function syncTempoSwing(tempo, swing) {
  if (tempo !== state.song.tempo && document.activeElement !== $('tempo')) { state.song.tempo = tempo; $('tempo').value = tempo; changed(); }
  if (swing !== state.song.swing && document.activeElement !== $('swing')) { state.song.swing = swing; $('swing').value = swing; $('swingOut').textContent = swing + '%'; changed(); }
}

// Knob/macro feedback from the module (protocol v2).
function onPanel(p) {
  if (p.bad) return;
  state.panel = p;
  state.panelAt = performance.now();
  // Routing edits from another editor (or a song loaded on the module).
  if (performance.now() - lastMaskEdit > 800 && (p.masks[0] !== state.song.macroMask(0) || p.masks[1] !== state.song.macroMask(1))) {
    state.song.setMacroMask(0, p.masks[0]); state.song.setMacroMask(1, p.masks[1]); changed(); paintRoutes();
  }
  syncTempoSwing(p.tempo, p.swing);
  paintMods();
  paintComputer();
}

const ledTimers = new WeakMap();
function flashLed(l) {
  if (!l) return;
  l.classList.add('lit');
  clearTimeout(ledTimers.get(l));
  ledTimers.set(l, setTimeout(() => l.classList.remove('lit'), 110));
}

function banner(text) {
  const b = $('banner');
  b.innerHTML = '';
  const x = el('button', null, 'OK');
  x.onclick = hideBanner;
  b.append(el('span', null, text), x);
  b.hidden = false;
}
function hideBanner() { $('banner').hidden = true; }

function onAck(m) {
  if (m.op === 'save') toast(m.status === 'ok' ? `Saved to slot ${m.slot + 1}.` : `Saving slot ${m.slot + 1} failed (${m.status}).`);
  if (m.op === 'load') {
    if (m.status === 'ok') { toast(`Slot ${m.slot + 1} loaded on the module, reading it…`); pull(); }
    else toast(`Slot ${m.slot + 1} is empty.`);
  }
  if (m.op === 'init') pull();
  send(msg.hello()); // refresh the slot list
}

function fillSlots(valid, current) {
  const sel = $('slot');
  sel.innerHTML = '';
  for (let i = 0; i < K.slots; i++) {
    const o = el('option', null, `${i + 1}${valid & (1 << i) ? '' : ' (empty)'}`);
    o.value = i;
    sel.appendChild(o);
  }
  sel.value = current;
}

function setConn(kind, text) {
  const c = $('connState');
  c.className = 'pill ' + kind;
  c.querySelector('span').textContent = text;
  $('btnConnect').textContent = dev.connected ? 'Disconnect' : 'Connect';
}

function pull() {
  if (!dev.connected) return;
  state.dump.reset();
  state.dumping = true;
  setConn('busy', 'reading…');
  send(msg.getSong());
}

async function push() {
  if (!dev.connected) { toast('Not connected'); return; }
  const all = uploadMessages(state.song);
  setConn('busy', 'sending…');
  for (let i = 0; i < all.length; i++) {
    dev.send(all[i]);
    if (i % 24 === 23) await new Promise((r) => setTimeout(r, 8)); // let the card keep up
  }
  setConn('on', 'connected');
  toast('Song sent to the module (not saved yet: use Save to module).');
}

async function connect() {
  if (dev.connected) {
    dev.disconnect();
    clearInterval(pingTimer);
    state.status = null;
    state.panel = null;
    state.legacy = false;
    paintMods(); paintComputer(); paintRoutes();
    setConn('off', 'offline');
    $('fwInfo').textContent = '';
    return;
  }
  if (!Device.supported()) { toast('This browser has no WebMIDI. Use Chrome or Edge on desktop.', 6000); return; }
  if (!window.isSecureContext) { toast('WebMIDI needs https:// or http://localhost.', 6000); return; }
  try {
    await dev.init();
  } catch (e) {
    toast('MIDI access was refused. Allow MIDI + SysEx for this page in the browser settings.', 6000);
    return;
  }
  let pair = dev.findPair();
  if (!pair) {
    const { ins, outs } = dev.ports();
    const names = ins.map((p) => p.name).join(', ') || 'none';
    const pick = ins.length ? prompt(`No "Workshop Tracker" MIDI port found. Ports: ${names}\nType the port name to use:`) : null;
    if (pick) {
      const inp = ins.find((p) => p.name === pick), out = outs.find((p) => p.name === pick);
      if (inp && out) pair = { inp, out };
    }
  }
  if (!pair) { toast('Module not found. Is the Workshop Tracker card in the Computer and the USB cable plugged into your computer?', 7000); return; }
  await dev.connect(pair.inp, pair.out);
  state.panel = null; state.status = null; state.legacy = false;
  setConn('on', 'connected');
  try { localStorage.setItem(BACKUP_KEY, JSON.stringify(state.song.toJSON())); $('btnRestore').hidden = false; } catch {}
  send(msg.hello());
  send(msg.ping());
  clearInterval(pingTimer);
  pingTimer = setInterval(() => send(msg.ping()), 500);
  pull();
}

dev.onMessage = onMessage;
dev.onState = (reason) => {
  if (!dev.connected) {
    clearInterval(pingTimer);
    state.status = null;
    state.panel = null;
    state.legacy = false;
    paintMods(); paintComputer(); paintRoutes();
    setConn('off', 'offline');
    if (reason) toast(reason + '. The module keeps playing on its own (performance mode).', 5000);
  }
};

// ---------------------------------------------------------------- wiring ---
function selectTab(name) {
  document.querySelectorAll('.tabs button').forEach((b) => b.setAttribute('aria-selected', String(b.dataset.tab === name)));
  for (const n of ['voices', 'mixer', 'cv']) $('pane-' + n).hidden = n !== name;
  try { localStorage.setItem('workshopTracker.tab', name); } catch {}
}

function wire() {
  document.querySelectorAll('.tabs button').forEach((b) => { b.onclick = () => selectTab(b.dataset.tab); });
  selectTab((() => { try { return localStorage.getItem('workshopTracker.tab') || 'voices'; } catch { return 'voices'; } })());
  for (let o = 0; o <= 8; o++) { const x = el('option', null, String(o)); x.value = o; $('octave').appendChild(x); }
  for (let s = 0; s <= 8; s++) { const x = el('option', null, String(s)); x.value = s; $('step').appendChild(x); }
  for (let p = 0; p < K.patterns; p++) { const x = el('option', null, 'Pattern ' + hex2(p)); x.value = p; $('pattern').appendChild(x); }
  fillSlots(0, 0);
  $('octave').value = state.octave; $('step').value = state.step;
  $('octave').onchange = () => setOctave(+$('octave').value);
  $('step').onchange = () => { state.step = +$('step').value; };
  $('editMode').onchange = () => { state.edit = $('editMode').checked; };
  $('follow').onchange = () => { state.follow = $('follow').checked; };
  $('pattern').onchange = () => selectPattern(+$('pattern').value);
  $('patLen').onchange = () => {
    const v = clamp(+$('patLen').value || 16, 1, K.rows);
    $('patLen').value = v;
    state.song.setPatLen(state.pattern, v); send(msg.setPatLen(state.pattern, v)); changed(); buildGrid();
  };
  $('tempo').onchange = () => { state.song.tempo = +$('tempo').value; $('tempo').value = state.song.tempo; send(msg.setTempo(state.song.tempo)); changed(); };
  $('swing').oninput = () => { state.song.swing = +$('swing').value; $('swingOut').textContent = state.song.swing + '%'; send(msg.setSwing(state.song.swing)); changed(); };
  $('songName').onchange = () => { state.song.name = $('songName').value.toUpperCase(); $('songName').value = state.song.name; send(msg.setName(state.song)); changed(); };
  $('btnConnect').onclick = connect;
  $('btnPlaySong').onclick = () => send(msg.playSong(0));
  $('btnPlayPat').onclick = () => send(msg.playPattern(state.pattern));
  $('btnStop').onclick = () => send(msg.stop());
  $('btnNext').onclick = () => send(msg.nextPart());
  $('btnLoad').onclick = () => { if (!dev.connected) return toast('Not connected'); send(msg.load(+$('slot').value)); };
  $('btnSave').onclick = () => {
    if (!dev.connected) return toast('Not connected');
    const slot = +$('slot').value;
    if (state.info && state.info.validSlots & (1 << slot) && !confirm(`Overwrite slot ${slot + 1} on the module?`)) return;
    send(msg.save(slot));
    toast(`Saving slot ${slot + 1}…`);
  };
  $('btnPull').onclick = () => (dev.connected ? pull() : toast('Not connected'));
  $('btnPush').onclick = push;
  $('btnNew').onclick = () => {
    if (!confirm('Start a new empty song in the editor (and on the module if connected)?')) return;
    replaceSong(new Song(), 'New song.');
    if (dev.connected) send(msg.initSong(0));
  };
  $('btnDemo').onclick = () => {
    if (!confirm('Load the demo song (editor and module)?')) return;
    replaceSong(demoSong(), 'Demo song loaded.');
    if (dev.connected) send(msg.initSong(1));
  };
  $('btnExport').onclick = () => {
    const blob = new Blob([JSON.stringify(state.song.toJSON())], { type: 'application/json' });
    const a = el('a'); a.href = URL.createObjectURL(blob); a.download = `${(state.song.name || 'song').trim().replace(/\W+/g, '_')}.wtrk.json`; a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 2000);
  };
  $('fileImport').onchange = async (e) => {
    const f = e.target.files[0];
    if (!f) return;
    try {
      replaceSong(Song.fromJSON(JSON.parse(await f.text())), `Imported ${f.name}.${dev.connected ? ' Use "Send to module" to hear it.' : ''}`);
    } catch (err) { toast('Import failed: ' + err.message, 6000); }
    e.target.value = '';
  };
  $('btnRestore').onclick = () => {
    try { replaceSong(Song.fromJSON(JSON.parse(localStorage.getItem(BACKUP_KEY))), 'Browser song restored (not sent to the module).'); } catch { toast('No backup'); }
  };
  $('btnAddEntry').onclick = () => {
    const n = state.song.songLen;
    if (n >= K.entries) return;
    state.song.songLen = n + 1;
    send(msg.setEntry(n, state.song.entry(n).pattern, state.song.entry(n).repeats));
    send(msg.setSongLen(n + 1)); changed(); buildArrangement();
  };
  $('btnDelEntry').onclick = () => {
    const n = state.song.songLen;
    if (n <= 1) return;
    state.song.songLen = n - 1; send(msg.setSongLen(n - 1)); changed(); buildArrangement();
  };
  $('btnCopyTrack').onclick = copyTrack;
  $('btnPasteTrack').onclick = pasteTrack;
  $('btnCopyPat').onclick = () => {
    state.clipPattern = { len: state.song.patLen(state.pattern), tracks: [] };
    for (let t = 0; t < K.tracks; t++) { const a = []; for (let r = 0; r < K.rows; r++) a.push(state.song.getCell(state.pattern, t, r)); state.clipPattern.tracks.push(a); }
    toast(`Copied pattern ${hex2(state.pattern)}`, 1500);
  };
  $('btnPastePat').onclick = () => {
    const c = state.clipPattern;
    if (!c) return;
    state.song.setPatLen(state.pattern, c.len); send(msg.setPatLen(state.pattern, c.len));
    for (let t = 0; t < K.tracks; t++) { for (let r = 0; r < K.rows; r++) state.song.setCell(state.pattern, t, r, c.tracks[t][r]); sendTrack(state.pattern, t); }
    $('patLen').value = c.len; changed(); buildGrid();
  };
  $('btnClearPat').onclick = () => {
    if (!confirm(`Clear pattern ${hex2(state.pattern)}?`)) return;
    for (let t = 0; t < K.tracks; t++) { for (let r = 0; r < K.rows; r++) state.song.setCell(state.pattern, t, r, { note: 0, vel: 0, fx: 0, fxv: 0 }); sendTrack(state.pattern, t); }
    changed(); buildGrid();
  };
  $('btnHelp').onclick = () => $('help').showModal();
  const grid = $('grid');
  grid.addEventListener('keydown', onGridKey);
  grid.addEventListener('mousedown', (e) => {
    const f = e.target.closest('span.f');
    if (!f) return;
    state.row = +f.dataset.r; state.track = +f.dataset.t; state.sub = +f.dataset.s; state.hexHi = null;
    paintCursor(false);
    setTimeout(() => grid.focus(), 0);
  });
  if (!Device.supported()) toast('WebMIDI is not available in this browser: you can edit offline, but connecting needs Chrome or Edge.', 7000);
}

wire();
buildComputer();
refreshAll();
setInterval(paintComputer, 500); // live LED timeout
$('grid').focus();
