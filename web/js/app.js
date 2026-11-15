// Workshop Tracker editor UI.
// SPDX-License-Identifier: MIT
import {
  K, Song, msg, decode, DumpCollector, uploadMessages, demoSong, noteName, hex2, clamp,
  ENGINES, MATERIALS, FX, NOTE_OFF, NOTE_EMPTY, PROTOCOL_VERSION,
} from './protocol.js';
import { Device } from './midi.js';

const $ = (id) => document.getElementById(id);
const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };

const TRACK_NAMES = ['Drum 1', 'Drum 2', 'Drum 3', 'Drum 4', 'CV/Gate 1', 'CV/Gate 2'];
const CH_NAMES = ['Dr1', 'Dr2', 'Dr3', 'Dr4', 'VCO1', 'VCO2'];
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
    const label = t < K.drumTracks ? `${TRACK_NAMES[t]} · ${ENGINES[state.song.drum(t, 0)].name}` : TRACK_NAMES[t];
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
  $('fxHelp').innerHTML = `<h2>Effects (${isCv ? 'CV track' : 'drum track'})</h2>${lines.join('')}` +
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

// ---------------------------------------------------------------- voices ---
function slider(label, value, min, max, fmt, onInput) {
  const w = el('label', 'prm');
  const name = el('span', null, label);
  const r = el('input'); r.type = 'range'; r.min = min; r.max = max; r.value = value;
  const out = el('output', null, fmt(value));
  r.oninput = () => { out.textContent = fmt(+r.value); onInput(+r.value); };
  w.append(name, r, out);
  w.set = (v) => { r.value = v; out.textContent = fmt(v); };
  w.setLabel = (t) => { name.textContent = t; };
  w.setFmt = (f) => { fmt = f; out.textContent = fmt(+r.value); };
  return w;
}
const fmtDecay = (v) => { const s = 0.005 * Math.pow(800, v / 127); return s < 1 ? `${Math.round(s * 1000)}ms` : `${s.toFixed(1)}s`; };
const fmtSigned = (v) => { const d = v - 64; return (d > 0 ? '+' : '') + d; };
const fmtPan = (v) => (v === 64 ? 'C' : v < 64 ? `L${64 - v}` : `R${v - 64}`);
const fmtTone = (v) => (v < 60 ? `LP${60 - v}` : v > 68 ? `HP${v - 68}` : 'flat');
const fmtFilter = (v) => (v < 61 ? `LP${61 - v}` : v > 67 ? `HP${v - 67}` : 'off');
const fmtA = (eng) => (v) => (eng === 4 ? MATERIALS[v >> 4] : eng === 3 ? `r${v >> 3}` : String(v));

let voicePanels = [];
function buildVoices() {
  const box = $('voices');
  box.innerHTML = '';
  voicePanels = [];
  const s = state.song;
  for (let v = 0; v < K.drumTracks; v++) {
    const p = el('div', 'voice');
    const title = el('div', 'title');
    title.appendChild(el('b', null, TRACK_NAMES[v]));
    const eng = el('select');
    ENGINES.forEach((e, i) => { const o = el('option', null, e.name); o.value = i; o.title = e.info; eng.appendChild(o); });
    eng.value = s.drum(v, 0);
    title.appendChild(eng);
    p.appendChild(title);
    const info = el('div', 'dim', ENGINES[s.drum(v, 0)].info);
    p.appendChild(info);
    const set = (param) => (x) => { s.setDrum(v, param, x); send(msg.setDrum(v, param, x)); changed(); };
    const dec = slider('Decay', s.drum(v, 1), 0, 127, fmtDecay, set(1));
    const a = slider(ENGINES[s.drum(v, 0)].a, s.drum(v, 2), 0, 127, fmtA(s.drum(v, 0)), set(2));
    const b = slider(ENGINES[s.drum(v, 0)].b, s.drum(v, 3), 0, 127, String, set(3));
    const tune = slider('Tune', s.drum(v, 4), 16, 112, fmtSigned, set(4));
    const vs = slider('Vel sens', s.drum(v, 5), 0, 127, String, set(5));
    eng.onchange = () => {
      const e = +eng.value;
      set(0)(e);
      info.textContent = ENGINES[e].info;
      a.setLabel(ENGINES[e].a); a.setFmt(fmtA(e));
      b.setLabel(ENGINES[e].b);
      buildGrid();
    };
    p.append(dec, a, b, tune, vs);
    const prev = el('button', null, 'Audition');
    prev.onclick = () => send(msg.preview(v, 48, 110));
    p.appendChild(prev);
    box.appendChild(p);
    voicePanels.push(p);
  }
  for (let c = 0; c < K.cvTracks; c++) {
    const p = el('div', 'voice cv');
    const title = el('div', 'title');
    title.appendChild(el('b', null, TRACK_NAMES[K.drumTracks + c]));
    title.appendChild(el('span', 'dim', `CV out ${c + 1} · Pulse out ${c + 1}`));
    p.appendChild(title);
    const set = (param) => (x) => { s.setCv(c, param, x); send(msg.setCv(c, param, x)); changed(); };
    p.append(
      slider('Gate', s.cv(c, 0), 0, 64, (v) => (v === 0 ? 'tie' : `${(v / 16).toFixed(2)} row`), set(0)),
      slider('Glide', s.cv(c, 1), 0, 127, (v) => (v === 0 ? 'off' : `${Math.round((48 + v * v * 6) / 48)}ms`), set(1)),
      slider('Transpose', s.cv(c, 2), 16, 112, fmtSigned, set(2)),
    );
    const prev = el('button', null, 'Test note');
    prev.onclick = () => send(msg.preview(K.drumTracks + c, 48, 100));
    p.appendChild(prev);
    box.appendChild(p);
    voicePanels.push(p);
  }
  const g = el('div', 'voice cv');
  g.appendChild(el('div', 'title')).appendChild(el('b', null, 'CV range'));
  g.appendChild(slider('0 V note', s.cvZero, 0, 96, (v) => noteName(v), (x) => { s.cvZero = x; send(msg.setZeroNote(x)); changed(); }));
  g.appendChild(el('div', 'dim', 'This note gives 0 V; lower notes stay at 0 V. 1 V/oct up to about +6 V.'));
  box.appendChild(g);
}
function selectVoicePanel(t) { voicePanels.forEach((p, i) => p.classList.toggle('sel', i === t)); }

// ----------------------------------------------------------------- mixer ---
let meters = [];
function buildMixer() {
  const box = $('mixer');
  box.innerHTML = '';
  meters = [];
  const s = state.song;
  const knob = (label, value, min, max, fmt, on) => {
    const w = el('label', 'k');
    w.appendChild(el('span', null, label));
    const r = el('input'); r.type = 'range'; r.min = min; r.max = max; r.value = value;
    const o = el('output', null, fmt(value));
    r.oninput = () => { o.textContent = fmt(+r.value); on(+r.value); };
    w.append(r, o);
    return w;
  };
  for (let ch = 0; ch < K.channels; ch++) {
    const st = el('div', 'strip' + (ch >= K.drumTracks ? ' in' : ''));
    st.appendChild(el('div', 'nm', CH_NAMES[ch]));
    const m = el('div', 'meter'); const bar = el('i'); m.appendChild(bar); meters.push(bar);
    st.appendChild(m);
    const set = (param) => (x) => { s.setMix(ch, param, x); send(msg.setMix(ch, param, x)); changed(); };
    st.append(
      knob('Level', s.mix(ch, 0), 0, 127, String, set(0)),
      knob('Pan', s.mix(ch, 1), 0, 127, fmtPan, set(1)),
      knob('Drive', s.mix(ch, 2), 0, 127, String, set(2)),
      knob('Tone', s.mix(ch, 3), 0, 127, fmtTone, set(3)),
    );
    if (ch >= K.drumTracks) {
      st.append(knob('Gate VCA', s.mix(ch, 4), 0, 127, (v) => (v ? fmtDecay(v) : 'off'), set(4)));
      st.title = `Audio in ${ch - K.drumTracks + 1}. Gate VCA: follows CV/Gate track ${ch - K.drumTracks + 1} (off = always open).`;
    }
    const mute = el('button', 'mute' + (s.mix(ch, 5) >= 64 ? ' on' : ''), 'M');
    mute.onclick = () => { const v = s.mix(ch, 5) >= 64 ? 0 : 127; set(5)(v); mute.classList.toggle('on', v >= 64); };
    st.appendChild(mute);
    box.appendChild(st);
  }
  const ms = el('div', 'strip master');
  ms.appendChild(el('div', 'nm', 'Master'));
  const mL = el('div', 'meter'); const bL = el('i'); mL.appendChild(bL);
  const mR = el('div', 'meter'); const bR = el('i'); mR.appendChild(bR);
  meters.push(bL, bR);
  const setM = (param) => (x) => { s.setMaster(param, x); send(msg.setMaster(param, x)); changed(); };
  ms.append(mL, mR,
    knob('Level', s.master(0), 0, 127, String, setM(0)),
    knob('Drive', s.master(1), 0, 127, String, setM(1)),
    knob('Filter', s.master(2), 0, 127, fmtFilter, setM(2)));
  box.appendChild(ms);
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
      $('fwInfo').textContent = `fw ${m.firmware} · slot ${m.slot + 1}`;
      fillSlots(m.validSlots, m.slot);
      if (m.protocol !== PROTOCOL_VERSION) toast(`Protocol ${m.protocol} on the module, editor speaks ${PROTOCOL_VERSION}. Update firmware or editor.`, 8000);
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
    case 'ack': onAck(m); break;
    default: break;
  }
}

function onStatus(st) {
  if (st.bad) return;
  const prev = state.status;
  state.status = st;
  const cpu = $('cpu');
  cpu.textContent = `${st.cpu}% / ${st.cpuPeak}%`;
  cpu.className = st.cpuPeak > 90 ? 'hot' : st.cpuPeak > 75 ? 'warn' : '';
  const modes = ['stopped', 'song', 'pattern'];
  $('modeOut').textContent = `${modes[st.mode] || '?'}${st.mode ? ` · P${hex2(st.pattern)} row ${hex2(st.row)}` : ''}${st.extClock ? ' · ext clock' : ''}${st.nextQueued ? ' · next…' : ''}${st.selectPage ? ' · panel: SELECT' : ''}`;
  // Panel changes on the module (tempo / swing knobs) show up here.
  if (st.tempo !== state.song.tempo && document.activeElement !== $('tempo')) { state.song.tempo = st.tempo; $('tempo').value = st.tempo; changed(); }
  if (st.swing !== state.song.swing && document.activeElement !== $('swing')) { state.song.swing = st.swing; $('swing').value = st.swing; $('swingOut').textContent = st.swing + '%'; changed(); }
  if (state.follow && st.mode !== 0 && st.pattern !== state.pattern && (!prev || prev.pattern !== st.pattern)) selectPattern(st.pattern);
  paintPlayhead();
  paintArrangementPos();
  st.peaks.forEach((p, i) => { if (meters[i]) meters[i].style.width = `${Math.min(100, (p / 127) * 100)}%`; });
  headHits.forEach((d, i) => d.classList.toggle('lit', i < 4 ? !!(st.hits & (1 << i)) : st.gates[i - 4]));
  $('btnPlaySong').classList.toggle('on', st.mode === 1);
  $('btnPlayPat').classList.toggle('on', st.mode === 2);
  if (state.info && st.slot !== state.info.slot) { state.info.slot = st.slot; $('slot').value = st.slot; $('fwInfo').textContent = `fw ${state.info.firmware} · slot ${st.slot + 1}`; }
}

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
  c.textContent = text;
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
    setConn('off', 'offline');
    if (reason) toast(reason + '. The module keeps playing on its own (performance mode).', 5000);
  }
};

// ---------------------------------------------------------------- wiring ---
function wire() {
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
refreshAll();
$('grid').focus();
