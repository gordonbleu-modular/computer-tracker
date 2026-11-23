// Workshop Tracker editor UI.
// SPDX-License-Identifier: MIT
import {
  K, Song, msg, decode, DumpCollector, uploadMessages, demoSong, noteName, clamp,
  ENGINES, MATERIALS, FX, FX_BY_CODE, fxFor, fxShow, fxStore, velShow, velStore, trackKind,
  VCA_SRC, VCA_SRC_NAMES, NOTE_OFF, NOTE_EMPTY, PROTOCOL_VERSION, effectiveDrum, effectiveFilter, effectiveCv, effectiveIn,
  cvFold, cvVolts, CV_NOTE_MIN, CV_NOTE_MAX, CV_OCT_MAX, RV, RV_DEFAULTS, rvSeconds, DRUM_X,
} from './protocol.js';
import { Device } from './midi.js';
import { Knob } from './knob.js';
import { MeterBallistics } from './meter.js';
import { NOTE_KEYS, renderKeyboard, detectLayout } from './keyboard.js';

const $ = (id) => document.getElementById(id);
const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };

const TRACK_NAMES = ['Drum 1', 'Drum 2', 'Drum 3', 'Drum 4', 'CV/Gate 1', 'CV/Gate 2', 'IN 1', 'IN 2'];
const CH_NAMES = ['DR1', 'DR2', 'DR3', 'DR4', 'IN1', 'IN2'];
const STORE_KEY = 'workshopTracker.song';
const BACKUP_KEY = 'workshopTracker.backup';

const state = {
  song: loadLocal() || demoSong(),
  pattern: 0, row: 0, track: 0, sub: 0, // sub: 0 note, 1 vel, 2 fx letter, 3 fx value
  octave: 4, step: 1, edit: true, follow: true,
  digit: null, // first typed digit of a two-digit value
  proto: PROTOCOL_VERSION, // of the connected module
  status: null, info: null,
  clipTrack: null, clipPattern: null,
  dump: new DumpCollector(), dumping: false,
  panel: null, panelAt: 0, legacy: false,
  dirty: false,      // edits since the song was read from / saved to the module
  ownSwapAt: -1e9,   // when the editor itself asked the module to load / init a song
  liveMute: { reverb: false, master: false, at: -1e9 }, // v7 live mutes (not in the song)
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
function changed() { saveLocal(); if (!state.dirty) { state.dirty = true; paintSlotNow(); } }
// Changes that came from the module itself (tempo / swing knobs, routing): saved in the browser, not "edits".
function synced() { saveLocal(); }

// ------------------------------------------------------------------ grid ---
// Columns per track: NOTE (3) VEL (2) FX (letter + 2). CV/gate tracks: NOTE FX
// (a gate is only on/off: no velocity since 0.3.2; the VEL span is hidden).
// Values are decimal 00-99 (0-127 inside, see fxShow / velShow).
const pad2 = (n) => String(n).padStart(2, '0');
const SUB_LABELS = { drum: ['NOTE', 'VEL', 'FX'], cv: ['NOTE', '', 'FX'], in: ['TRIG', 'LVL', 'FX'] };
const hasVel = (t) => trackKind(t) !== 'cv';
const SRC_SHORT = ['', 'OPEN', 'CV', 'TRACK'];
let rowEls = [];
let headHits = [];
let colEls = [];   // colEls[t] = track column elements (header + rows), for the open (expanded) track
let subLabelEls = [];
function trackTitle(t) {
  const s = state.song;
  if (t < K.firstCv) return `DR${t + 1} ${ENGINES[s.drum(t, 0)].name}`;
  if (t < K.firstIn) return `CV${t - K.firstCv + 1}`;
  const i = t - K.firstIn, src = s.vcaSource(i);
  return `IN${i + 1}${src === VCA_SRC.cvGate ? '←CV' : src === VCA_SRC.inTrack ? ' TRK' : ' OPEN'}`;
}
function trackLongName(t) {
  if (t < K.firstCv) return `Drum ${t + 1}: ${ENGINES[state.song.drum(t, 0)].name}`;
  if (t < K.firstIn) return `CV/gate ${t - K.firstCv + 1}: pitch on CV out ${t - K.firstCv + 1}, gate on pulse out ${t - K.firstCv + 1}`;
  const i = t - K.firstIn;
  return `IN ${i + 1}: VCA of audio in ${i + 1}, opened by ${VCA_SRC_NAMES[state.song.vcaSource(i)]}`;
}
const fwLacks = (t) => dev.connected && state.proto < 3 && t >= K.v2Tracks;
function buildGrid() {
  const g = $('grid');
  g.innerHTML = '';
  closePicker();
  const head = el('div', 'head');
  head.appendChild(el('span', 'num', ''));
  headHits = []; colEls = []; subLabelEls = [];
  for (let t = 0; t < K.tracks; t++) {
    const kind = trackKind(t);
    const h = el('span', `trk k-${kind}`);
    const top = el('div', 'tname');
    const dot = el('i', 'hit');
    headHits.push(dot);
    top.append(dot, document.createTextNode(trackTitle(t)));
    top.title = trackLongName(t) + (fwLacks(t) ? ' (needs firmware 0.3: the connected module does not play this track)' : '');
    const sub = el('div', 'subl');
    const L = SUB_LABELS[kind];
    const labs = [el('span', 'l n'), el('span', 'l v'), el('span', 'l x')];
    labs.forEach((x, i) => { x.appendChild(el('i', null, L[i])); sub.appendChild(x); });
    labs[0].title = kind === 'in' ? 'Trigger: any note key opens the VCA, 1 = OFF closes it'
      : kind === 'cv' ? 'Note C0-B5 = 0-5.92 V (1 V/oct, C0 = 0 V); 1 = OFF closes the gate' : 'Note: keys Z…M / Q…P, 1 = OFF';
    labs[1].title = kind === 'drum' ? 'Velocity 01-99 (.. = default)' : 'Level of the VCA 01-99 (.. = full)';
    labs[2].title = 'Effect: letter + value. Enter or click = list of effects';
    subLabelEls.push(labs);
    h.append(top, sub);
    head.appendChild(h);
    colEls.push([h]);
  }
  g.appendChild(head);
  rowEls = [];
  const len = state.song.patLen(state.pattern);
  for (let r = 0; r < len; r++) {
    const re = el('div', 'r');
    re.appendChild(el('span', 'num', pad2(r + 1)));
    const cells = [];
    for (let t = 0; t < K.tracks; t++) {
      const te = el('span', `trk k-${trackKind(t)}`);
      const f = [el('span', 'f n'), el('span', 'f v'), el('span', 'f xl'), el('span', 'f xv')];
      f.forEach((s, i) => { s.dataset.r = r; s.dataset.t = t; s.dataset.s = i; te.appendChild(s); });
      re.appendChild(te);
      cells.push(f);
      colEls[t].push(te);
    }
    g.appendChild(re);
    rowEls.push({ el: re, cells });
    for (let t = 0; t < K.tracks; t++) paintCell(r, t);
  }
  if (state.row >= len) state.row = len - 1;
  g.classList.toggle('oldfw', dev.connected && state.proto < 3);
  paintSubLabels();
  openTrack = -1;
  paintCursor(true);
  paintPlayhead();
  fitGrid();
}

// The grid font grows to fill the width (8 tracks without horizontal scroll).
const GRID_PX = [16, 15, 14, 13.5, 13, 12.5, 12, 11.5, 11, 10.5, 10];
function fitGrid() {
  const g = $('grid');
  for (const px of GRID_PX) {
    g.style.fontSize = `${px}px`;
    if (g.scrollWidth <= g.clientWidth + 1) return;
  }
}

function paintSubLabels() {}

function fxText(code, v) {
  if (!code) return ['.', '..'];
  return [String.fromCharCode(code), pad2(fxShow(code, v))];
}
function paintCell(r, t) {
  const re = rowEls[r];
  if (!re) return;
  const c = state.song.getCell(state.pattern, t, r);
  const [n, v, x1, v1] = re.cells[t];
  const kind = trackKind(t);
  n.textContent = kind === 'in' && c.note && c.note !== NOTE_OFF ? 'TRG' : noteName(c.note);
  n.classList.toggle('e', c.note === NOTE_EMPTY);
  n.classList.toggle('off', c.note === NOTE_OFF);
  const vs = velShow(c.vel);
  v.textContent = vs < 0 ? '..' : pad2(vs);
  v.classList.toggle('e', !c.vel);
  const [a1, b1] = fxText(c.fx, c.fxv);
  x1.textContent = a1; v1.textContent = b1;
  x1.classList.toggle('e', !c.fx); v1.classList.toggle('e', !c.fx);
}

let lastCursor = null;
let openTrack = -1;
function paintCursor(scroll) {
  if (lastCursor) lastCursor.classList.remove('c');
  const re = rowEls[state.row];
  rowEls.forEach((x, i) => x.el.classList.toggle('cur', i === state.row));
  if (openTrack !== state.track) {
    if (colEls[openTrack]) colEls[openTrack].forEach((e) => e.classList.remove('open'));
    if (colEls[state.track]) colEls[state.track].forEach((e) => e.classList.add('open'));
    openTrack = state.track;
  }
  if (!re) return;
  const s = re.cells[state.track][state.sub];
  s.classList.add('c');
  lastCursor = s;
  if (scroll) s.scrollIntoView({ block: 'nearest', inline: 'nearest' });
  showFxHelp();
  showStatusLine();
  selectVoicePanel(state.track);
  paintOctave();
  paintKeyboard();
}

function paintPlayhead() {
  const st = state.status;
  const playing = st && st.mode !== 0 && st.pattern === state.pattern;
  rowEls.forEach((x, i) => x.el.classList.toggle('play', !!playing && i === st.row));
  if (playing && state.follow && rowEls[st.row]) rowEls[st.row].el.scrollIntoView({ block: 'nearest' });
}

// Legend of the effects for the kind of track under the cursor.
let legendKind = null;
function showFxHelp() {
  const kind = trackKind(state.track);
  if (kind === legendKind) return;
  legendKind = kind;
  const box = $('fxHelp');
  const open = box.open;
  box.innerHTML = '';
  box.appendChild(el('summary', null, `Effects · ${kind === 'drum' ? 'drum' : kind === 'cv' ? 'CV/gate' : 'IN (VCA)'} track`));
  const list = el('div', 'fxlist');
  for (const [l, f] of fxFor(state.track)) {
    const d = el('div');
    d.title = f.help + (f.val === 'pct' ? ' Value 00-99.' : ` Value ${pad2(f.min)}-${pad2(f.max)}.`);
    d.append(el('b', null, l), document.createTextNode(f.name));
    list.appendChild(d);
  }
  box.appendChild(list);
  box.appendChild(el('div', 'dim', 'Enter or click an FX cell for the list. One effect per step.'));
  box.open = open;
}

// One line under the grid that explains the field under the mouse or cursor.
let hoverField = null;
function describeField(r, t, sub) {
  const c = state.song.getCell(state.pattern, t, r);
  const kind = trackKind(t);
  const where = `${trackTitle(t)} · row ${pad2(r + 1)} · `;
  if (sub === 0) {
    if (kind === 'in') return where + (c.note === NOTE_OFF ? 'OFF: closes the VCA' : c.note ? 'TRIG: opens the VCA for the hold time (IN module)' : 'TRIG: any note key opens the VCA, 1 = OFF');
    if (kind === 'cv') return where + (c.note && c.note !== NOTE_OFF ? `NOTE ${noteName(c.note)} = ${cvVolts(c.note).toFixed(2)} V` : `NOTE ${noteName(c.note)}`) +
      ' · C0 = 0 V, 1 V/oct, range C0-B5 · gate on/off, 1 = OFF closes it';
    return where + `NOTE ${noteName(c.note)} · 1 = OFF chokes the voice`;
  }
  if (sub === 1) {
    const vs = velShow(c.vel);
    const val = vs < 0 ? '.. (default)' : pad2(vs);
    if (kind === 'drum') return where + `VEL ${val}: how hard the voice is hit (.. = 79)`;
    return where + `LVL ${val}: VCA level for this trigger (.. = full)`;
  }
  const f = FX_BY_CODE[c.fx];
  if (!f) return where + `FX empty · Enter or click for the list, or type ${fxFor(t).map(([l]) => l).join(' ')}`;
  return where + `FX ${f.letter}${pad2(fxShow(c.fx, c.fxv))} = ${f.name}: ${f.help}`;
}
function showStatusLine() {
  const f = hoverField;
  $('statusLine').textContent = f ? describeField(+f.dataset.r, +f.dataset.t, +f.dataset.s) : describeField(state.row, state.track, state.sub);
}

// ------------------------------------------------------------ FX picker ---
const picker = { el: null, items: [], idx: 0, r: 0, t: 0 };
function openPicker(r, t, anchor) {
  closePicker();
  const c = state.song.getCell(state.pattern, t, r);
  const cur = c.fx;
  const box = el('div', 'picker');
  box.setAttribute('role', 'listbox');
  box.appendChild(el('div', 'ptitle', `FX · ${trackTitle(t)} · row ${pad2(r + 1)}`));
  const entries = [...fxFor(t).map(([l, f]) => ({ l, f })), { l: null }];
  picker.items = entries.map((e, i) => {
    const b = el('button', 'pitem');
    b.setAttribute('role', 'option');
    if (e.l) {
      b.append(el('b', null, e.l), el('span', 'pname', e.f.name), el('span', 'phelp', e.f.help));
      if (e.f.code === cur) b.classList.add('cur');
    } else {
      b.append(el('b', null, '·'), el('span', 'pname', 'None'), el('span', 'phelp', 'Remove this effect'));
    }
    b.onmousedown = (ev) => { ev.preventDefault(); pickFx(e.l); };
    b.onmousemove = () => { picker.idx = i; paintPicker(); };
    box.appendChild(b);
    return b;
  });
  picker.idx = Math.max(0, entries.findIndex((e) => e.f && e.f.code === cur));
  picker.r = r; picker.t = t;
  picker.el = box;
  document.body.appendChild(box);
  const a = anchor.getBoundingClientRect();
  const w = box.offsetWidth, h = box.offsetHeight;
  box.style.left = `${Math.max(4, Math.min(innerWidth - w - 4, a.left))}px`;
  box.style.top = `${a.bottom + h + 4 < innerHeight ? a.bottom + 2 : Math.max(4, a.top - h - 2)}px`;
  paintPicker();
}
function paintPicker() { picker.items.forEach((b, i) => b.classList.toggle('sel', i === picker.idx)); }
function closePicker() { if (picker.el) { picker.el.remove(); picker.el = null; } }
function pickFx(letter) {
  const { r, t } = picker;
  closePicker();
  setFx(r, t, letter);
  $('grid').focus();
}
function setFx(r, t, letter) {
  const c = state.song.getCell(state.pattern, t, r);
  if (!letter) { setCell(r, t, { ...c, fx: 0, fxv: 0 }, false); return; }
  const f = FX[letter];
  const keep = c.fx === f.code;
  setCell(r, t, { ...c, fx: f.code, fxv: keep ? c.fxv : defaultFxValue(letter) }, false);
  if (state.row === r && state.track === t) { state.sub = 3; paintCursor(false); }
}
function onPickerKey(e) {
  const n = picker.items.length;
  switch (e.code) {
    case 'ArrowDown': picker.idx = (picker.idx + 1) % n; paintPicker(); break;
    case 'ArrowUp': picker.idx = (picker.idx + n - 1) % n; paintPicker(); break;
    case 'Enter': case 'Space': picker.items[picker.idx].dispatchEvent(new MouseEvent('mousedown')); break;
    case 'Escape': closePicker(); break;
    case 'Delete': case 'Backspace': pickFx(null); break;
    default: {
      const L = e.key.toUpperCase();
      if (FX[L] && FX[L].kinds.includes(trackKind(picker.t))) pickFx(L);
      else return;
    }
  }
  e.preventDefault();
}

// --------------------------------------------------------------- editing ---
function setCell(r, t, cell, preview = true) {
  state.song.setCell(state.pattern, t, r, cell);
  paintCell(r, t);
  if (!fwLacks(t)) send(msg.setCell(state.pattern, t, r, state.song.getCell(state.pattern, t, r), state.proto));
  changed();
  showStatusLine();
  if (trackKind(t) === 'in' && cell.note && cell.note !== NOTE_OFF) ensureInTrackSource(t - K.firstIn);
  if (preview && cell.note && cell.note !== NOTE_OFF && !fwLacks(t)) send(msg.preview(t, cell.note, cell.vel || 100));
}

// Typing a trigger on an IN track: make that track open the VCA.
function ensureInTrackSource(i) {
  const s = state.song;
  if (s.vcaSource(i) === VCA_SRC.inTrack) return;
  const was = VCA_SRC_NAMES[s.vcaSource(i)];
  setMixParam(4 + i, 6, VCA_SRC.inTrack);
  if (!s.mix(4 + i, 4)) setMixParam(4 + i, 4, 40);
  toast(`IN${i + 1} VCA now follows the IN${i + 1} track (was: ${was}). Change it in the CV / IN tab.`, 4500);
  buildCvIn(); buildMixer(); refreshTrackHeads();
}
function setMixParam(ch, p, x) { state.song.setMix(ch, p, x); send(msg.setMix(ch, p, x)); changed(); }
function refreshTrackHeads() {
  document.querySelectorAll('#grid .head .tname').forEach((n, t) => { n.lastChild.textContent = trackTitle(t); n.title = trackLongName(t); });
  paintSubLabels();
  for (let r = 0; r < rowEls.length; r++) for (let t = K.firstCv; t < K.firstIn; t++) paintCell(r, t);
}

function moveCursor(dr, dt, ds) {
  const len = state.song.patLen(state.pattern);
  state.digit = null;
  if (dr) state.row = ((state.row + dr) % len + len) % len;
  if (dt) { state.track = clamp(state.track + dt, 0, K.tracks - 1); }
  if (ds) {
    let s = state.sub + ds, t = state.track;
    if (s === 1 && !hasVel(t)) s += ds;   // CV tracks: no VEL column
    if (s > 3) { if (t < K.tracks - 1) { t++; s = 0; } else s = 3; }
    if (s < 0) { if (t > 0) { t--; s = 3; } else s = 0; }
    state.sub = s; state.track = t;
  }
  if (state.sub === 1 && !hasVel(state.track)) state.sub = 2;
  paintCursor(true);
}

// ------------------------------------------------------- keyboard sketch ---
// Which computer key plays which note (keyboard.js). Docked under the grid
// (toggle with the ⌨ button) and drawn in the Keys dialog.
const KB_KEY = 'workshopTracker.kbd';
const kb = { layout: 'qwerty', map: null, sig: '' };
const noteShort = (n) => noteName(n).replace('-', '');
function keyboardTitle(t) {
  const kind = trackKind(t), o = keyOctave(t);
  if (kind === 'in') return `${trackTitle(t)} · any note key = TRIG, 1 = OFF`;
  return `${trackTitle(t)} · OCT ${o}` + (kind === 'cv' ? ` · C${o} = ${o} V (CV: C0–B5 only)` : '') + ' · - / = octave';
}
function paintKeyboard(force) {
  const t = state.track;
  const sig = `${trackKind(t)}|${keyOctave(t)}|${kb.layout}|${t}|${trackTitle(t)}`;
  if (!force && sig === kb.sig) return;
  kb.sig = sig;
  const opts = {
    layout: kb.layout, layoutMap: kb.map,
    noteText: (code) => (trackKind(t) === 'in' ? 'TRG' : noteShort(keyNote(code, t))),
  };
  const dock = $('kbd');
  if (dock && !dock.hidden) { renderKeyboard($('kbdKeys'), opts); $('kbdTitle').textContent = keyboardTitle(t); }
  if ($('help').open) renderKeyboard($('kbdHelp'), { ...opts, title: keyboardTitle(t) });
  $('kbdLayout').textContent = kb.map ? 'auto' : kb.layout.toUpperCase();
}
function showKeyboard(on) {
  $('kbd').hidden = !on;
  $('btnKbd').classList.toggle('kbon', on);
  try { localStorage.setItem(KB_KEY, on ? '1' : '0'); } catch {}
  paintKeyboard(true);
  fitGrid();
}
function pressKey(code) {
  const k = document.querySelector(`#kbdKeys .kb-key[data-code="${code}"]`);
  if (!k) return;
  k.classList.add('press');
  setTimeout(() => k.classList.remove('press'), 160);
}

// Shown value of the field under the cursor, and how to store a new one.
function fieldValue(c, sub) {
  if (sub === 1) return { show: Math.max(0, velShow(c.vel)), store: (d) => ({ ...c, vel: velStore(d) }) };
  const code = c.fx;
  return { code, show: fxShow(code, c.fxv), store: (d) => ({ ...c, fxv: fxStore(code, d) }) };
}

function onGridKey(e) {
  if (picker.el) { onPickerKey(e); return; }
  if (e.target.closest('input,select,textarea') && e.target !== $('grid')) return;
  const k = e.code;
  const ctrl = e.ctrlKey || e.metaKey;
  if (ctrl && k === 'KeyC') { copyTrack(); e.preventDefault(); return; }
  if (ctrl && k === 'KeyV') { pasteTrack(); e.preventDefault(); return; }
  if (ctrl) return;
  const c = state.song.getCell(state.pattern, state.track, state.row);
  // Shift + arrows / PgUp / PgDn: change the value under the cursor.
  if (e.shiftKey && state.edit && ['ArrowUp', 'ArrowDown', 'PageUp', 'PageDown'].includes(k)) {
    const d = (k === 'ArrowUp' ? 1 : k === 'ArrowDown' ? -1 : k === 'PageUp' ? 10 : -10);
    e.preventDefault();
    if (state.sub === 0) {
      const [lo, hi] = trackKind(state.track) === 'cv' ? [CV_NOTE_MIN, CV_NOTE_MAX] : [1, 126];
      if (c.note && c.note !== NOTE_OFF) setCell(state.row, state.track, { ...c, note: clamp(c.note + (Math.abs(d) === 10 ? Math.sign(d) * 12 : d), lo, hi) });
      return;
    }
    if (state.sub === 2) return;
    const fv = fieldValue(c, state.sub);
    if (state.sub !== 1 && !fv.code) return;
    setCell(state.row, state.track, fv.store(clamp(fv.show + d, 0, 99)), false);
    return;
  }
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
    case 'Enter':
      if (state.sub >= 2 && state.edit) { openPicker(state.row, state.track, lastCursor); }
      else send(msg.playSong(0));
      e.preventDefault();
      return;
    case 'Minus': if (state.sub === 0) { setOctave(keyOctave(state.track) - 1); e.preventDefault(); } return;
    case 'Equal': if (state.sub === 0) { setOctave(state.octave + 1); e.preventDefault(); } return;
    default: break;
  }
  if (k === 'Delete' || k === 'Backspace') {
    if (state.sub === 0) setCell(state.row, state.track, { note: 0, vel: 0, fx: 0, fxv: 0 }, false);
    else if (state.sub === 1) setCell(state.row, state.track, { ...c, vel: 0 }, false);   // (not on CV tracks)
    else setCell(state.row, state.track, { ...c, fx: 0, fxv: 0 }, false);
    if (state.edit) moveCursor(state.step, 0, 0);
    e.preventDefault();
    return;
  }
  if (state.sub === 0) {
    let note = null;
    if (k === 'Digit1') note = NOTE_OFF;
    else if (k in NOTE_KEYS) note = keyNote(k, state.track);
    if (note == null) return;
    e.preventDefault();
    pressKey(k);
    if (e.repeat) return;
    if (!state.edit) { if (note !== NOTE_OFF && !fwLacks(state.track)) send(msg.preview(state.track, note, 100)); return; }
    setCell(state.row, state.track, { ...c, note });
    moveCursor(state.step, 0, 0);
    return;
  }
  if (!state.edit) return;
  if (state.sub === 2) {
    const L = e.key.toUpperCase();
    if (L.length === 1 && FX[L]) {
      e.preventDefault();
      if (!FX[L].kinds.includes(trackKind(state.track))) { toast(`${L} (${FX[L].name}) does not apply to this track. Enter shows the effects that do.`); return; }
      setFx(state.row, state.track, L);
    }
    return;
  }
  // Decimal digits in the velocity and effect value columns: one digit sets
  // the value, a second one makes it two digits (e.g. 7, then 5 = 75).
  if (!/^[0-9]$/.test(e.key)) return;
  e.preventDefault();
  const d = +e.key;
  const fv = fieldValue(c, state.sub);
  if (state.sub !== 1 && !fv.code) { toast('Choose the effect first: Enter or click the FX letter.'); return; }
  const f = FX_BY_CODE[fv.code];
  if (state.digit == null) {
    setCell(state.row, state.track, fv.store(d), false);
    // A second digit could not give a valid value (e.g. ratchet 2-8): done.
    if (state.sub !== 1 && f && f.val === 'raw' && d * 10 > f.max) moveCursor(state.step, 0, 0);
    else state.digit = d;
  } else {
    const v = state.digit * 10 + d;
    state.digit = null;
    setCell(state.row, state.track, fv.store(v), false);
    moveCursor(state.step, 0, 0);
  }
}

// Stored units (see FX in protocol.js).
function defaultFxValue(L) {
  return { R: 2, P: 50, G: 8, S: 25, K: 8 }[L] ?? 64;
}

function copyTrack() {
  const arr = [];
  for (let r = 0; r < K.rows; r++) arr.push(state.song.getCell(state.pattern, state.track, r));
  state.clipTrack = arr;
  toast(`Copied ${TRACK_NAMES[state.track]} of pattern ${pad2(state.pattern + 1)}`, 1500);
}
function pasteTrack() {
  if (!state.clipTrack) return;
  for (let r = 0; r < K.rows; r++) state.song.setCell(state.pattern, state.track, r, state.clipTrack[r]);
  sendTrack(state.pattern, state.track);
  buildGrid();
  changed();
}
function sendTrack(p, t) {
  if (fwLacks(t)) return;
  for (let r = 0; r < K.rows; r += K.chunkRows) send(msg.setCells(state.song, p, t, r, K.chunkRows, state.proto));
}

// ------------------------------------------------------------- transport ---
function togglePlayPattern() {
  if (state.status && state.status.mode !== 0) send(msg.stop());
  else send(msg.playPattern(state.pattern));
  if (!dev.connected) toast('Not connected: connect the module to hear it');
}

function setOctave(o) { state.octave = clamp(o, 0, 8); paintOctave(); paintKeyboard(); }
// Octave the note keys play on track t: CV tracks stop at 5 (C0 = 0 V ... B5 = 5.92 V).
const keyOctave = (t) => (trackKind(t) === 'cv' ? Math.min(state.octave, CV_OCT_MAX) : state.octave);
function keyNote(code, t) {
  const n = (keyOctave(t) + 1) * 12 + NOTE_KEYS[code];
  return trackKind(t) === 'cv' ? cvFold(n) : clamp(n, 1, 126);
}
// The OCT selector shows the octave in effect for the track under the cursor.
let octCv = null;
function paintOctave() {
  const sel = $('octave');
  const cv = trackKind(state.track) === 'cv';
  if (cv !== octCv) {
    octCv = cv;
    [...sel.options].forEach((o) => { o.disabled = cv && +o.value > CV_OCT_MAX; });
    sel.title = cv ? 'Octave for note keys. CV tracks: 0-5 (C0 = 0 V, B5 = 5.92 V; only positive voltages).' : 'Octave for note keys (- / = keys)';
  }
  sel.value = keyOctave(state.track);
}

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
    for (let p = 0; p < K.patterns; p++) { const o = el('option', null, 'P' + pad2(p + 1)); o.value = p; sel.appendChild(o); }
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
const fmtPct = (v) => `${Math.round((v * 100) / 127)}%`;
const fmtRvDecay = (v) => { const t = rvSeconds(v); return t < 1 ? `${Math.round(t * 1000)}ms` : `${t.toFixed(1)}s`; };
const fmtLofi = (v) => (v === 0 ? 'clean' : `${16 - (v >> 4)}b${v >= 64 ? ' S&H' : ''}`);
let rvLed = null;
let liveMuteBtns = {}; // 'reverb' / 'master' -> button (0.4.2)

// knobs.drum[v][param], knobs.cv[t][param], knobs.mix[ch][param], knobs.master[param]
const knobs = { drum: [], cv: [], inm: [], mix: [], master: [], send: [], rv: [] };
let voicePanels = [];
let voiceLeds = [];
let gateLeds = [];
let routeBtns = []; // routeBtns[v][macro]
let routeBtnsCv = []; // [0/1 CV, 2/3 IN][macro]
let inLeds = [];
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
function krow3(...ks) { const r = krow(...ks); r.classList.add('three'); return r; }
// 0.4.3 drum knob texts
const fmtX = (v) => (v === 0 ? 'off' : `${Math.round((v * 100) / 127)}%`);
const titleX = (e) => (e === 0
  ? 'Click: a short beater click (high-passed noise + a 1.6 kHz blip, ~1.5 ms) on top of the kick, scaled with velocity. 0 = off (the kick as before).'
  : 'Grit: lo-fi fuzz after the voice: drive x1…x16 into a hard clip, fewer bits (down to 6) and from 25 % a lower sample rate (down to 12 kHz). 0 = off (the voice as before).');
const fmtB = (e) => (e === 5 ? (v) => (v <= 44 ? `snr ${v}` : v >= 84 ? `clap ${v}` : `${Math.round(((v - 44) * 100) / 40)}%clp`) : String);
const titleB = (e) => (e === 5 ? 'Snare -> clap. Up to 44: snare, brighter to the right. 44…84: the snare morphs into the clap (noise colour, extra bursts). From 84: clap, bursts further apart to the right.' : '');

// 0.4.2: each drum module wears a faceplate after the selected engine (look
// only; knobs, layout and behaviour are the same for every engine).
const PANEL_STYLES = ['p-kick', 'p-pluck', 'p-noise', 'p-metal', 'p-modal', 'p-snare'];
const PLATE_NAMES = ['KICK', 'pluck', 'noise', 'Metal', 'MODAL', 'SNARE/CLAP'];
function setPanelStyle(m, e) {
  m.classList.remove(...PANEL_STYLES);
  m.classList.add(PANEL_STYLES[e] || PANEL_STYLES[0]);
  if (m.plate) m.plate.textContent = PLATE_NAMES[e] || '';
}

function buildVoices() {
  const box = $('voices');
  box.innerHTML = '';
  voicePanels = []; voiceLeds = []; routeBtns = []; knobs.drum = [];
  const s = state.song;
  for (let v = 0; v < K.drumTracks; v++) {
    const m = moduleEl(`DRUM ${v + 1}`, 'drum');
    m.plate = el('div', 'plate');
    setPanelStyle(m, s.drum(v, 0));
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
    const kb = new Knob({ label: ENGINES[e0].b, value: s.drum(v, 3), def: 32, fmt: fmtB(e0), onChange: set(3), title: titleB(e0) });
    const kt = new Knob({ label: 'Tune', min: 16, max: 112, value: s.drum(v, 4), def: 64, fmt: fmtSigned, size: 's', bipolar: true, onChange: set(4) });
    const kv = new Knob({ label: 'Vel', value: s.drum(v, 5), def: 100, size: 's', fmt: fmtPct, onChange: set(5),
      title: 'Velocity sensitivity: how much the VEL column of a step changes this voice\'s level. 0 % = every hit at full level (VEL ignored), 100 % = VEL sets the level fully. Default 79 %.' });
    // 0.4.3: Kick = beater click level, other engines = grit (0 = off, the old sound)
    const kx = new Knob({ label: ENGINES[e0].x, value: s.drum(v, DRUM_X), def: 0, size: 's', fmt: fmtX, onChange: set(DRUM_X), title: titleX(e0) });
    kx.el.classList.add('kx');
    kx.setDisabled(dev.connected && state.proto < 8);
    knobs.drum[v] = [null, kd, ka, kb, kt, kv, kx];
    eng.onchange = () => {
      const e = +eng.value;
      set(0)(e);
      sub.textContent = ENGINES[e].info;
      setPanelStyle(m, e);
      ka.setLabel(ENGINES[e].a); ka.setFmt(fmtA(e));
      kb.setLabel(ENGINES[e].b); kb.setFmt(fmtB(e)); kb.el.title = titleB(e);
      kx.setLabel(ENGINES[e].x); kx.el.title = titleX(e);
      buildGrid();
    };
    const aud = el('button', 'audition', 'TRIG');
    aud.title = 'Play this voice on the module';
    aud.onclick = () => send(msg.preview(v, 48, 110));
    m.append(eng, sub, routes, kd.el, krow(ka, kb), krow3(kt, kv, kx), el('div', 'spacer'), aud, m.plate);
    box.appendChild(m);
    voicePanels.push(m);
  }
  buildCvIn();
}

function paintRoutes() {
  const s = state.song;
  const paint = (b, on, dis) => {
    b.classList.toggle('on', on);
    b.querySelector('.led').classList.toggle('lit', on);
    b.disabled = dis;
    b.setAttribute('aria-pressed', String(on));
  };
  routeBtns.forEach((row, v) => row.forEach((b, mi) => paint(b, s.macroOn(mi, v), !!state.legacy)));
  const old = dev.connected && state.proto < 3;
  routeBtnsCv.forEach((row, j) => row.forEach((b, mi) => {
    paint(b, !!(s.macroCvMask(mi) & (1 << j)), old);
    if (old) b.title = 'Needs firmware 0.3';
  }));
}

// MAIN / X routing buttons for a CV (j = 0/1) or IN (j = 2/3) module.
function cvinRoutes(j, whatMain, whatX) {
  const s = state.song;
  const routes = el('div', 'routes');
  routeBtnsCv[j] = [];
  [['MAIN', `Main knob → ${whatMain}`], ['X', `X knob → ${whatX}`]].forEach(([name, what], mi) => {
    const b = el('button');
    b.append(el('i', 'led'), document.createTextNode(name));
    b.title = `${what} in PERFORM mode. Click to toggle. Stored in the song.`;
    b.onclick = () => {
      const bits = s.macroCvMask(mi) ^ (1 << j);
      s.setMacroCvMask(mi, bits);
      lastMaskEdit = performance.now();
      if (!dev.connected || state.proto >= 3) send(msg.setMacroMask(mi, s.macroMask(mi), bits));
      changed(); paintRoutes(); paintMods();
    };
    routes.appendChild(b);
    routeBtnsCv[j][mi] = b;
  });
  return routes;
}

const fmtGate = (v) => (v === 0 ? 'tie' : `${(v / 16).toFixed(2)}row`);
const fmtHold = (v) => (v === 0 ? 'to OFF' : `${(v / 16).toFixed(2)}row`);
function buildCvIn() {
  const box = $('cvpanel');
  box.innerHTML = '';
  gateLeds = []; inLeds = []; knobs.cv = []; knobs.inm = []; routeBtnsCv = [];
  voicePanels.length = K.drumTracks;
  const s = state.song;
  for (let c = 0; c < K.cvTracks; c++) {
    const m = moduleEl(`CV/GATE ${c + 1}`, 'voice-cv narrow');
    gateLeds.push(m.led);
    const set = (param) => (x) => { s.setCv(c, param, x); send(msg.setCv(c, param, x)); changed(); paintMods(); };
    const kg = new Knob({ label: 'Gate', min: 0, max: 64, value: s.cv(c, 0), def: 8, size: 'l', fmt: fmtGate, onChange: set(0),
      title: 'Gate length (1/16 row). Main knob adds to it when MAIN is lit (red arc = live).' });
    const kgl = new Knob({ label: 'Glide', value: s.cv(c, 1), def: 0, size: 's', fmt: (v) => (v === 0 ? 'off' : `${Math.round((48 + v * v * 6) / 48)}ms`), onChange: set(1),
      title: 'Portamento. X knob adds to it when X is lit.' });
    const ktr = new Knob({ label: 'Transp', min: 16, max: 112, value: s.cv(c, 2), def: 64, size: 's', fmt: fmtSigned, bipolar: true, onChange: set(2) });
    knobs.cv[c] = [kg, kgl, ktr];
    const test = el('button', 'audition', 'TEST');
    test.title = 'Play a test note on this CV/gate output';
    test.onclick = () => send(msg.preview(K.firstCv + c, 48, 100));
    m.append(el('div', 'msub', `CV ${c + 1} = pitch · pulse ${c + 1} = gate`), cvinRoutes(c, 'gate length', 'glide'), kg.el, krow(kgl, ktr), el('div', 'spacer'), test, jacks(['CV', 'GATE']));
    box.appendChild(m);
    voicePanels[K.firstCv + c] = m;
  }
  const g = moduleEl('CV RANGE', 'voice-cv narrow');
  // 0.3.2: fixed mapping, only positive voltages (the Workshop System VCOs need >= 0 V).
  const scale = el('div', 'cvscale');
  for (let o = CV_OCT_MAX; o >= 0; o--) {
    const row = el('div', 'cvrow');
    row.append(el('b', null, `C${o}`), el('span', null, `${o}.00 V`));
    scale.appendChild(row);
  }
  scale.title = 'Note names on CV tracks match the voltage: C0 = 0 V, C1 = 1 V … B5 = 5.92 V.';
  g.append(el('div', 'msub', '1 V/oct · C0 = 0 V'), scale,
    el('div', 'msub', 'Notes C0–B5. Transp and CV in 1 fold out-of-range notes by an octave, so the output never goes negative.'),
    el('div', 'spacer'), jacks(['CV1', 'CV2']));
  box.appendChild(g);
  for (let i = 0; i < K.inTracks; i++) {
    const ch = 4 + i;
    const m = moduleEl(`IN ${i + 1} VCA`, 'voice-in narrow');
    inLeds.push(m.led);
    const old = dev.connected && state.proto < 3;
    const set = (param) => (x) => { setMixParam(ch, param, x); paintMods(); syncInKnobs(); };
    const src = el('select', 'engine');
    src.title = `What opens the VCA of audio in ${i + 1}. CV GATE: the gate of CV/gate track ${i + 1} opens it (full level; the mixer sets the volume). IN TRACK: the IN${i + 1} track (TRIG + LVL).`;
    const opts = [[VCA_SRC.open, 'OPEN (always on)'], [VCA_SRC.cvGate, `CV GATE ${i + 1}`], [VCA_SRC.inTrack, `IN${i + 1} TRACK`]];
    if (s.mix(ch, 6) === VCA_SRC.auto) opts.unshift([VCA_SRC.auto, s.mix(ch, 4) ? `AUTO (= CV GATE ${i + 1})` : 'AUTO (= OPEN)']);
    for (const [v, n] of opts) { const o = el('option', null, n); o.value = v; src.appendChild(o); }
    src.value = s.mix(ch, 6);
    src.disabled = old;
    src.onchange = () => {
      setMixParam(ch, 6, +src.value);
      if (+src.value >= VCA_SRC.cvGate && !s.mix(ch, 4)) setMixParam(ch, 4, 40);
      buildCvIn(); buildMixer(); refreshTrackHeads();
    };
    const kd = new Knob({ label: 'Decay', value: s.mix(ch, 4), def: 40, size: 'l', fmt: fmtDecay, onChange: set(4),
      title: 'VCA release after the gate. Main knob adds to it when MAIN is lit (red arc = live).' });
    const kh = new Knob({ label: 'Hold', min: 0, max: 64, value: s.mix(ch, 7), def: 8, size: 's', fmt: fmtHold, onChange: set(7),
      title: 'IN TRACK: how long a TRIG keeps the VCA open (1/16 row; FX G overrides per step).' });
    const kdr = new Knob({ label: 'Drive', value: s.mix(ch, 2), def: 0, size: 's', onChange: set(2),
      title: 'Saturation of this input. X knob adds to it when X is lit.' });
    if (old) [kh].forEach((k) => k.setDisabled(true));
    knobs.inm[i] = { decay: kd, hold: kh, drive: kdr };
    const test = el('button', 'audition', 'TEST');
    test.title = `Open the IN${i + 1} VCA once (IN TRACK mode)`;
    test.disabled = old;
    test.onclick = () => send(msg.preview(K.firstIn + i, 60, 127));
    m.append(src, cvinRoutes(2 + i, 'VCA decay', 'drive'), kd.el, krow(kh, kdr), el('div', 'spacer'), test, jacks(['IN', '→MIX']));
    box.appendChild(m);
    voicePanels[K.firstIn + i] = m;
  }
  paintRoutes();
  paintMods();
}
// The mixer strip shows the same IN drive / decay values.
function syncInKnobs() {
  for (let i = 0; i < K.inTracks; i++) {
    const kk = knobs.inm[i], mk = knobs.mix[4 + i];
    if (kk) { kk.decay.set(state.song.mix(4 + i, 4)); kk.drive.set(state.song.mix(4 + i, 2)); kk.hold.set(state.song.mix(4 + i, 7)); }
    if (mk) { mk[2].set(state.song.mix(4 + i, 2)); if (mk[4]) mk[4].set(state.song.mix(4 + i, 4)); }
  }
}

function selectVoicePanel(t) { voicePanels.forEach((p, i) => p && p.classList.toggle('sel', i === t)); }

// ----------------------------------------------------------------- mixer ---
// Meters: bar + peak-hold marker per channel (6 channels, master L, R), with
// PPM/VU-like ballistics animated between the module's 50 ms reports (meter.js).
let meters = [];
const meterBal = Array.from({ length: 9 }, () => new MeterBallistics()); // 6 ch, master L/R, reverb return
function meterEl() {
  const met = el('div', 'meter');
  const bar = el('i'), pk = el('b', 'pk');
  met.append(bar, pk);
  met.title = 'Level (dB scale, -48…0 dBFS). White line = peak of the last second.';
  meters.push({ bar, pk });
  return met;
}
let meterLast = 0;
function animateMeters(now) {
  const dt = meterLast ? Math.min(100, now - meterLast) : 16;
  meterLast = now;
  meterBal.forEach((m, i) => {
    m.step(dt);
    const e = meters[i];
    if (!e) return;
    e.bar.style.clipPath = `inset(${((1 - m.value) * 100).toFixed(1)}% 0 0 0)`;
    e.pk.style.bottom = `${(m.peak * 100).toFixed(1)}%`;
    e.pk.style.opacity = m.peak > 0.01 ? '1' : '0';
  });
  requestAnimationFrame(animateMeters);
}
let muteBtns = [];
function buildMixer() {
  const box = $('mixer');
  box.innerHTML = '';
  meters = []; muteBtns = []; liveMuteBtns = {}; knobs.mix = []; knobs.send = []; knobs.rv = [];
  const s = state.song;
  const noRv = dev.connected && state.proto < 6; // reverb needs firmware 0.4 (protocol 6)
  const setSend = (ch) => (x) => { s.setSend(ch, x); send(msg.setSend(ch, x)); changed(); };
  for (let ch = 0; ch < K.channels; ch++) {
    const isIn = ch >= K.drumTracks;
    const m = moduleEl(CH_NAMES[ch], 'strip' + (isIn ? ' in' : ''));
    const met = meterEl();
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
      const i = ch - K.drumTracks;
      const vca = new Knob({ label: 'VCA dec', value: s.mix(ch, 4), def: 40, size: 's', fmt: fmtDecay, onChange: (x) => { set(4)(x); syncInKnobs(); paintMods(); },
        title: `Audio in ${i + 1} VCA release (also on the CV / IN tab). Source: ${VCA_SRC_NAMES[state.song.vcaSource(i)]}` });
      k[2].onChange = (x) => { set(2)(x); syncInKnobs(); paintMods(); };
      k.push(vca);
      m.appendChild(vca.el);
      m.title = `IN${i + 1} = audio input ${i + 1} (e.g. a VCO). Its VCA is set up on the CV / IN tab.`;
    }
    const snd = new Knob({ label: 'Send', value: s.send(ch), def: 0, size: 's', fmt: fmtPct, onChange: setSend(ch),
      title: `Reverb send of ${CH_NAMES[ch]} (post level and mute). Square law: 50 % ≈ -12 dB, 100 % = full.` });
    snd.el.classList.add('send');
    snd.setDisabled(noRv);
    knobs.send[ch] = snd;
    m.appendChild(snd.el);
    const mute = el('button', 'mute');
    mute.append(el('i', 'led'), document.createTextNode('MUTE'));
    mute.onclick = () => { const v = s.mix(ch, 5) >= 64 ? 0 : 127; set(5)(v); paintMutes(); };
    muteBtns.push(mute);
    m.append(el('div', 'spacer'), mute);
    box.appendChild(m);
  }
  // Lo-fi reverb (firmware 0.4): returns into the master bus before the filter.
  const rv = moduleEl('REVERB', 'strip reverb');
  rvLed = rv.led;
  const setR = (i) => (x) => { s.setMaster(RV.return + i, x); send(msg.setMaster(RV.return + i, x)); changed(); };
  const rvm = el('div', 'meters');
  rvm.appendChild(meterEl());
  knobs.rv = [
    new Knob({ label: 'Return', value: s.master(RV.return), def: RV_DEFAULTS[0], fmt: fmtPct, onChange: setR(0), title: 'Reverb return level into the master (90 ≈ 0 dB, 100 % = +6 dB).' }),
    new Knob({ label: 'Size', value: s.master(RV.size), def: RV_DEFAULTS[1], size: 's', fmt: fmtPct, onChange: setR(1), title: 'Room size: delay line lengths 40…100 % (changes glide like a tape bend).' }),
    new Knob({ label: 'Decay', value: s.master(RV.decay), def: RV_DEFAULTS[2], size: 's', fmt: fmtRvDecay, onChange: setR(2), title: 'Decay time to -60 dB (0.3 … 9 s).' }),
    new Knob({ label: 'Tone', value: s.master(RV.tone), def: RV_DEFAULTS[3], size: 's', fmt: fmtPct, onChange: setR(3), title: 'Damping inside the tail: low = dark, high = bright.' }),
    new Knob({ label: 'Lo-fi', value: s.master(RV.lofi), def: RV_DEFAULTS[4], size: 's', fmt: fmtLofi, onChange: setR(4),
      title: 'Grit: drops low bits in the tail (16 → 9 bit), adds a slow pitch wobble, and from 50 % on uses sample-and-hold (aliased) output.' }),
  ];
  knobs.rv.forEach((k) => k.setDisabled(noRv));
  rv.append(rvm, ...knobs.rv.map((x) => x.el), el('div', 'spacer'), liveMuteBtn('reverb'));
  rv.title = noRv ? 'The reverb needs firmware 0.4.0 (protocol 6). Update the module.' : 'Lo-fi stereo reverb, 16 kHz, 4-line FDN. LED = tail is ringing.';
  if (noRv) rv.classList.add('off');
  box.appendChild(rv);
  const ms = moduleEl('MASTER', 'strip master');
  const mm = el('div', 'meters');
  for (let i = 0; i < 2; i++) mm.appendChild(meterEl());
  const setM = (param) => (x) => { s.setMaster(param, x); send(msg.setMaster(param, x)); changed(); paintMods(); };
  knobs.master = [
    new Knob({ label: 'Level', value: s.master(0), def: 120, onChange: setM(0) }),
    new Knob({ label: 'Drive', value: s.master(1), def: 10, size: 's', onChange: setM(1) }),
    new Knob({ label: 'Filter', value: s.master(2), def: 64, fmt: fmtFilter, size: 'l', bipolar: true, onChange: setM(2),
      title: 'Master filter: left = low-pass, centre = off, right = high-pass. The module\'s Y knob and CV in 2 add to it (red).' }),
  ];
  ms.append(mm, ...knobs.master.map((x) => x.el), el('div', 'spacer'), liveMuteBtn('master'));
  box.appendChild(ms);
  // meters[] order = meterBal order: 6 channels, master L, R, then the reverb return.
  meters.push(meters.splice(K.channels, 1)[0]);
  paintMutes();
  paintMods();
}
// REVERB / MASTER mutes (firmware 0.4.2, protocol 7): live only, like a mixing
// desk's return / main mute. Not stored in the song; the module starts unmuted.
// The module's Status flags are the truth while connected.
const liveMuteOk = () => !dev.connected || state.proto >= 7;
function liveMuteBtn(which) {
  const b = el('button', 'mute');
  b.append(el('i', 'led'), document.createTextNode('MUTE'));
  b.onclick = () => {
    if (!liveMuteOk()) return;
    const on = !state.liveMute[which];
    state.liveMute[which] = on;
    state.liveMute.at = performance.now();
    send(msg.setLiveMute(which, on));
    paintMutes();
  };
  liveMuteBtns[which] = b;
  return b;
}
function paintMutes() {
  const paint = (b, on) => { b.classList.toggle('on', on); b.querySelector('.led').classList.toggle('lit', on); b.setAttribute('aria-pressed', String(on)); };
  muteBtns.forEach((b, ch) => paint(b, state.song.mix(ch, 5) >= 64));
  const ok = liveMuteOk();
  for (const w of ['reverb', 'master']) {
    const b = liveMuteBtns[w];
    if (!b) continue;
    paint(b, ok && state.liveMute[w]);
    b.disabled = !ok;
    b.title = !ok ? 'Needs firmware 0.4.2 (protocol 7). Update the module.'
      : w === 'master' ? 'Mute the whole output (~5 ms fade). Live only: not saved with the song, off after power-up.'
      : 'Mute the reverb return (~5 ms fade; the tail keeps running, so unmuting brings back what still rings). Live only: not saved.';
  }
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
  // CV / IN macros (firmware 0.3). Older firmware: no arcs.
  const old = dev.connected && state.proto < 3;
  const pv = p && p.v3 ? p : null;
  for (let c = 0; c < K.cvTracks; c++) {
    const kk = knobs.cv[c];
    if (!kk) continue;
    kk[0].setMod(old ? null : pv ? pv.effCvGate[c] : effectiveCv(state.song, mac, c, 0));
    kk[1].setMod(old ? null : pv ? pv.effCvGlide[c] : effectiveCv(state.song, mac, c, 1));
  }
  for (let i = 0; i < K.inTracks; i++) {
    const dec = old ? null : pv ? pv.effInDecay[i] : effectiveIn(state.song, mac, i, 4);
    const drv = old ? null : pv ? pv.effInDrive[i] : effectiveIn(state.song, mac, i, 2);
    const kk = knobs.inm[i], mk = knobs.mix[4 + i];
    if (kk) { kk.decay.setMod(dec); kk.drive.setMod(drv); }
    if (mk) { mk[2].setMod(drv); if (mk[4]) mk[4].setMod(dec); }
  }
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
      if (state.proto !== Math.min(m.protocol, PROTOCOL_VERSION)) {
        state.proto = Math.min(m.protocol, PROTOCOL_VERSION);
        buildVoices(); buildMixer(); buildGrid(); // buildVoices also rebuilds CV/IN
      }
      state.dump.tracksExpected = state.proto < 3 ? K.v2Tracks : K.tracks;
      if (m.protocol === 4) {
        banner(`The module runs firmware ${m.firmware}. Everything works from here (the editor sets C0 = 0 V and converts the CV notes), ` +
          `but please flash the new workshop_tracker.uf2 (0.3.2): the slot knob then splits its travel over the saved slots, CV GATE ` +
          `ignores velocity and CV in 1 can no longer transpose below 0 V.`);
      } else if (m.protocol === 3) {
        banner(`The module runs firmware ${m.firmware}, which still has a second effect column. Everything works from here (the second ` +
          `effect is cleared when you edit a step), but please flash the new workshop_tracker.uf2 (0.3.2). Its songs and slots are converted.`);
      } else if (m.protocol < 3) {
        banner(`The module runs firmware ${m.firmware} (protocol ${m.protocol}). Drums and CV/gate tracks play, but the IN tracks ` +
          `and the CV/IN macro routing need firmware 0.3.2` + (m.protocol < 2 ? ', knob feedback and MAIN/X voice routing 0.2' : '') +
          `: flash the new workshop_tracker.uf2. Songs from the module are converted on reading.`);
      } else if (m.protocol > PROTOCOL_VERSION) {
        banner(`The module speaks protocol ${m.protocol}, this editor ${PROTOCOL_VERSION}. Reload the page or update the editor; some features may not work.`);
      } else hideBanner();
      paintRoutes();
      paintComputer();
      break;
    case 'header': case 'chunk': {
      const song = state.dump.feed(m);
      if (state.dumping) setConn('busy', `reading ${Math.round(state.dump.progress * 100)}%`);
      if (song) gotSongFromModule(song);
      break;
    }
    case 'dumpEnd': {
      const song = state.dump.feed(m);
      state.dumping = false;
      setConn('on', 'connected');
      if (song) gotSongFromModule(song);
      break;
    }
    case 'status': onStatus(m); break;
    case 'panel': onPanel(m); break;
    case 'ack': onAck(m); break;
    default: break;
  }
}

// A whole song read from the module.
function gotSongFromModule(song) {
  state.dumping = false;
  setConn('on', 'connected');
  replaceSong(song, state.pullWhy || 'Loaded the song from the module.');
  state.pullWhy = null;
  state.dirty = false;
  paintSlotNow();
  // Firmware <= 0.3.1 keeps its old "0 V note" (C2): the editor converted the
  // song to C0 = 0 V, so give the module the same notes and mapping.
  if (state.dump.cvConverted && state.proto < 5 && dev.connected) {
    send(msg.setZeroNote(12));
    for (let p = 0; p < K.patterns; p++) for (let t = K.firstCv; t < K.firstIn; t++) sendTrack(p, t);
  }
}

// The module plays another song than the editor shows: its slot knob (SELECT
// page) chose another slot, or a song swap happened that the editor did not
// ask for. Keep unsaved edits as a browser backup, then read the new song.
function moduleSwitched(st) {
  const name = state.song.name.trim() || 'song';
  if (state.dirty) {
    try {
      localStorage.setItem(BACKUP_KEY, JSON.stringify(state.song.toJSON()));
      localStorage.setItem(BACKUP_KEY + '.why', `unsaved edits to "${name}" (before the module switched to slot ${st.slot + 1})`);
      $('btnRestore').hidden = false;
    } catch {}
    banner(`The module switched to slot ${st.slot + 1}. Your unsaved edits to "${name}" are kept in the browser: ` +
      'Restore brings them back (then Send to module, or Save to module to keep them in a slot).');
  }
  state.pullWhy = `Module switched to slot ${st.slot + 1}: loaded its song.`;
  pull();
}

function paintSlotNow() {
  const pill = $('slotNow');
  if (!pill) return;
  const st = dev.connected ? state.status : null;
  const lab = pill.querySelector('span');
  pill.classList.toggle('on', !!st);
  pill.classList.toggle('pending', !!st && st.swapPending);
  if (!st) { lab.textContent = 'SLOT –'; pill.title = 'Connect to see which slot the module plays'; return; }
  lab.innerHTML = '';
  lab.append(document.createTextNode('SLOT '), el('b', null, String(st.slot + 1)),
    document.createTextNode(st.swapPending ? ' → next at pattern end' : state.dirty ? ' · edited' : ''));
  pill.title = `The module plays slot ${st.slot + 1}` + (state.dirty ? '; the editor has changes that are not saved to a slot yet' : '') +
    '. Turning the slot knob on the module (SELECT page) loads the new song here too.';
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
  if (st.mode) txt += ` · P${pad2(st.pattern + 1)} · ROW ${pad2(st.row + 1)}`;
  if (st.extClock) txt += ' · EXT CLK';
  if (st.nextQueued) txt += ' · NEXT…';
  $('modeOut').textContent = txt;
  $('beatLed').classList.toggle('lit', st.mode !== 0 && st.row % 4 === 0);
  // Panel changes on the module (tempo / swing knobs) show up here.
  syncTempoSwing(st.tempo, st.swing);
  if (state.follow && st.mode !== 0 && st.pattern !== state.pattern && (!prev || prev.pattern !== st.pattern)) selectPattern(st.pattern);
  paintPlayhead();
  paintArrangementPos();
  st.peaks.forEach((p, i) => { if (meterBal[i]) meterBal[i].report(p); });
  meterBal[8].report(st.reverbPeak || 0);
  if (rvLed) rvLed.classList.toggle('lit', (st.reverbPeak || 0) > 0);
  if (state.proto >= 7 && performance.now() - state.liveMute.at > 400 &&
      (st.reverbMuted !== state.liveMute.reverb || st.masterMuted !== state.liveMute.master)) {
    state.liveMute.reverb = st.reverbMuted; state.liveMute.master = st.masterMuted;
    paintMutes();
  }
  headHits.forEach((d, i) => d.classList.toggle('lit', i < K.firstCv ? !!(st.hits & (1 << i)) : i < K.firstIn ? st.gates[i - K.firstCv] : st.inGates[i - K.firstIn]));
  for (let v = 0; v < 4; v++) if (st.hits & (1 << v)) flashLed(voiceLeds[v]);
  gateLeds.forEach((l, i) => l.classList.toggle('lit', st.gates[i]));
  inLeds.forEach((l, i) => l.classList.toggle('lit', st.inGates[i]));
  $('btnPlaySong').classList.toggle('on', st.mode === 1);
  $('btnPlayPat').classList.toggle('on', st.mode === 2);
  if (state.info && st.slot !== state.info.slot) { state.info.slot = st.slot; $('slot').value = st.slot; }
  // Song swapped on the module without the editor asking (panel slot knob).
  if (prev && st.swaps !== prev.swaps && !state.dumping && performance.now() - state.ownSwapAt > 4000) moduleSwitched(st);
  if (!prev || st.slot !== prev.slot || st.swapPending !== prev.swapPending) paintSlotNow();
  if (!state.panel) { paintMods(); paintComputer(); } // v1 firmware: macros come with the status only
}

function syncTempoSwing(tempo, swing) {
  if (tempo !== state.song.tempo && document.activeElement !== $('tempo')) { state.song.tempo = tempo; $('tempo').value = tempo; synced(); }
  if (swing !== state.song.swing && document.activeElement !== $('swing')) { state.song.swing = swing; $('swing').value = swing; $('swingOut').textContent = swing + '%'; synced(); }
}

// Knob/macro feedback from the module (protocol v2).
function onPanel(p) {
  if (p.bad) return;
  state.panel = p;
  state.panelAt = performance.now();
  // Routing edits from another editor (or a song loaded on the module).
  if (performance.now() - lastMaskEdit > 800 && (p.masks[0] !== state.song.macroMask(0) || p.masks[1] !== state.song.macroMask(1))) {
    state.song.setMacroMask(0, p.masks[0]); state.song.setMacroMask(1, p.masks[1]); synced(); paintRoutes();
  }
  if (p.v3 && performance.now() - lastMaskEdit > 800 && (p.cvinMasks[0] !== state.song.macroCvMask(0) || p.cvinMasks[1] !== state.song.macroCvMask(1))) {
    state.song.setMacroCvMask(0, p.cvinMasks[0]); state.song.setMacroCvMask(1, p.cvinMasks[1]); synced(); paintRoutes();
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
  if (m.op === 'save') {
    toast(m.status === 'ok' ? `Saved to slot ${m.slot + 1}.` : `Saving slot ${m.slot + 1} failed (${m.status}).`);
    if (m.status === 'ok') { state.dirty = false; paintSlotNow(); }
  }
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
  paintSlotNow();
}

function pull() {
  if (!dev.connected) return;
  state.dump.reset();
  state.dump.tracksExpected = state.proto < 3 ? K.v2Tracks : K.tracks;
  state.dumping = true;
  setConn('busy', 'reading…');
  send(msg.getSong());
}

async function push() {
  if (!dev.connected) { toast('Not connected'); return; }
  const all = uploadMessages(state.song, state.proto);
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
    resetProto();
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
  try {
    localStorage.setItem(BACKUP_KEY, JSON.stringify(state.song.toJSON()));
    localStorage.setItem(BACKUP_KEY + '.why', 'the song the browser had before connecting');
    $('btnRestore').hidden = false;
  } catch {}
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
    resetProto();
    paintMods(); paintComputer(); paintRoutes();
    setConn('off', 'offline');
    if (reason) toast(reason + '. The module keeps playing on its own (performance mode).', 5000);
  }
};

// ---------------------------------------------------------------- wiring ---
function resetProto() {
  const was = state.proto;
  state.proto = PROTOCOL_VERSION;
  if (was !== PROTOCOL_VERSION) { hideBanner(); buildVoices(); buildMixer(); buildGrid(); }
}

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
  for (let p = 0; p < K.patterns; p++) { const x = el('option', null, 'Pattern ' + pad2(p + 1)); x.value = p; $('pattern').appendChild(x); }
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
  $('btnLoad').onclick = () => { if (!dev.connected) return toast('Not connected'); state.ownSwapAt = performance.now(); send(msg.load(+$('slot').value)); };
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
    if (dev.connected) { state.ownSwapAt = performance.now(); send(msg.initSong(0)); }
  };
  $('btnDemo').onclick = () => {
    if (!confirm('Load the demo song (editor and module)?')) return;
    replaceSong(demoSong(), 'Demo song loaded.');
    if (dev.connected) { state.ownSwapAt = performance.now(); send(msg.initSong(1)); }
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
    try {
      const why = localStorage.getItem(BACKUP_KEY + '.why') || 'the song the browser had before connecting';
      replaceSong(Song.fromJSON(JSON.parse(localStorage.getItem(BACKUP_KEY))), `Restored ${why}. Not on the module yet: use Send to module.`);
      state.dirty = true; paintSlotNow();
    } catch { toast('No backup'); }
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
    toast(`Copied pattern ${pad2(state.pattern + 1)}`, 1500);
  };
  $('btnPastePat').onclick = () => {
    const c = state.clipPattern;
    if (!c) return;
    state.song.setPatLen(state.pattern, c.len); send(msg.setPatLen(state.pattern, c.len));
    for (let t = 0; t < K.tracks; t++) { for (let r = 0; r < K.rows; r++) state.song.setCell(state.pattern, t, r, c.tracks[t][r]); sendTrack(state.pattern, t); }
    $('patLen').value = c.len; changed(); buildGrid();
  };
  $('btnClearPat').onclick = () => {
    if (!confirm(`Clear pattern ${pad2(state.pattern + 1)}?`)) return;
    for (let t = 0; t < K.tracks; t++) { for (let r = 0; r < K.rows; r++) state.song.setCell(state.pattern, t, r, { note: 0, vel: 0, fx: 0, fxv: 0 }); sendTrack(state.pattern, t); }
    changed(); buildGrid();
  };
  $('btnHelp').onclick = () => { $('help').showModal(); paintKeyboard(true); };
  $('btnKbd').onclick = () => showKeyboard($('kbd').hidden);
  $('kbdClose').onclick = () => showKeyboard(false);
  $('kbdLayout').onclick = () => {
    kb.map = null; // manual choice overrides the detected map
    kb.layout = kb.layout === 'qwerty' ? 'qwertz' : 'qwerty';
    try { localStorage.setItem(KB_KEY + '.layout', kb.layout); } catch {}
    paintKeyboard(true);
  };
  (async () => {
    let saved = null;
    try { saved = localStorage.getItem(KB_KEY + '.layout'); } catch {}
    if (saved) kb.layout = saved;
    else { const d = await detectLayout(); kb.layout = d.layout; kb.map = d.map; }
    paintKeyboard(true);
  })();
  showKeyboard((() => { try { return localStorage.getItem(KB_KEY) !== '0'; } catch { return true; } })());
  const grid = $('grid');
  grid.addEventListener('keydown', onGridKey);
  grid.addEventListener('mousedown', (e) => {
    const f = e.target.closest('span.f');
    if (!f) return;
    state.row = +f.dataset.r; state.track = +f.dataset.t; state.sub = +f.dataset.s; state.digit = null;
    paintCursor(false);
    if (state.edit && state.sub >= 2) openPicker(state.row, state.track, f);
    setTimeout(() => grid.focus(), 0);
  });
  grid.addEventListener('mouseover', (e) => {
    const f = e.target.closest('span.f');
    if (f !== hoverField) { hoverField = f; showStatusLine(); }
  });
  grid.addEventListener('mouseleave', () => { hoverField = null; showStatusLine(); });
  document.addEventListener('mousedown', (e) => { if (picker.el && !picker.el.contains(e.target) && !e.target.closest('span.f')) closePicker(); });
  window.addEventListener('resize', () => { closePicker(); fitGrid(); });
  if (!Device.supported()) toast('WebMIDI is not available in this browser: you can edit offline, but connecting needs Chrome or Edge.', 7000);
}

wire();
buildComputer();
refreshAll();
setInterval(paintComputer, 500); // live LED timeout
requestAnimationFrame(animateMeters);
$('grid').focus();
