// Rotary knob control (SVG). Drag vertically (Shift = fine), mouse wheel,
// double-click = default, keyboard: arrows, PgUp/PgDn, Home/End.
//
// The white pointer + gold arc show the stored value. An optional red arc and
// dot show a live "effective" value (e.g. the module's macro offset), which is
// never written into the song.
// SPDX-License-Identifier: MIT

const NS = 'http://www.w3.org/2000/svg';
const A0 = -135, A1 = 135; // degrees, 0 = up

function polar(cx, cy, r, deg) {
  const a = ((deg - 90) * Math.PI) / 180;
  return [cx + r * Math.cos(a), cy + r * Math.sin(a)];
}
function arc(r, d0, d1) {
  if (Math.abs(d1 - d0) < 0.01) return '';
  const [s, e] = d0 < d1 ? [d0, d1] : [d1, d0];
  const [x0, y0] = polar(50, 50, r, s);
  const [x1, y1] = polar(50, 50, r, e);
  return `M${x0.toFixed(2)} ${y0.toFixed(2)} A${r} ${r} 0 ${e - s > 180 ? 1 : 0} 1 ${x1.toFixed(2)} ${y1.toFixed(2)}`;
}
function svg(tag, attrs, parent) {
  const e = document.createElementNS(NS, tag);
  for (const k in attrs) e.setAttribute(k, attrs[k]);
  if (parent) parent.appendChild(e);
  return e;
}

let uid = 0;

// One shared gradient for all knob caps (silver, lit from top-left).
function ensureDefs() {
  if (document.getElementById('kcapGrad')) return;
  const holder = svg('svg', { width: 0, height: 0, style: 'position:absolute', 'aria-hidden': 'true' });
  const defs = svg('defs', {}, holder);
  const g = svg('radialGradient', { id: 'kcapGrad', cx: '35%', cy: '30%', r: '75%' }, defs);
  svg('stop', { offset: '0%', 'stop-color': '#f4f5f6' }, g);
  svg('stop', { offset: '55%', 'stop-color': '#aab0b6' }, g);
  svg('stop', { offset: '100%', 'stop-color': '#5d6168' }, g);
  // 0.4.2: cap colours for the drum faceplates (see style.css, .p-*)
  for (const [id, c0, c1, c2] of [['kcapBlack', '#5a5c60', '#1c1d1f', '#050505'], ['kcapWhite', '#ffffff', '#ecebe6', '#b9b8b2'],
    ['kcapBlue', '#8fb2ff', '#2f5fd8', '#173483'], ['kcapCream', '#fffdf6', '#e4dfcf', '#a8a292']]) {
    const x = svg('radialGradient', { id, cx: '35%', cy: '30%', r: '75%' }, defs);
    svg('stop', { offset: '0%', 'stop-color': c0 }, x);
    svg('stop', { offset: '55%', 'stop-color': c1 }, x);
    svg('stop', { offset: '100%', 'stop-color': c2 }, x);
  }
  document.body.appendChild(holder);
}

export class Knob {
  constructor({ label, min = 0, max = 127, value = 0, def = null, fmt = String, onChange = () => {}, size = 'm', bipolar = false, title = '', readOnly = false }) {
    ensureDefs();
    this.min = min; this.max = max; this.value = value; this.def = def ?? value;
    this.fmt = fmt; this.onChange = onChange; this.bipolar = bipolar; this.mod = null;
    const el = (this.el = document.createElement('div'));
    el.className = `knob k-${size}`;
    el.setAttribute('role', 'slider');
    if (readOnly) { el.classList.add('ro'); el.setAttribute('aria-readonly', 'true'); } else el.tabIndex = 0;
    if (title) el.title = title;
    this.lab = document.createElement('div');
    this.lab.className = 'klabel';
    this.lab.id = `kl${++uid}`;
    el.setAttribute('aria-labelledby', this.lab.id);
    const s = (this.svg = svg('svg', { viewBox: '0 0 100 100', class: 'kface', 'aria-hidden': 'true' }));
    svg('path', { d: arc(44, A0, A1), class: 'ktrack' }, s);
    this.valArc = svg('path', { class: 'kval-arc' }, s);
    this.modArc = svg('path', { class: 'kmod-arc' }, s);
    for (let i = 0; i <= 10; i++) { // scale ticks
      const d = A0 + ((A1 - A0) * i) / 10;
      const [x0, y0] = polar(50, 50, 36, d), [x1, y1] = polar(50, 50, 39, d);
      svg('line', { x1: x0, y1: y0, x2: x1, y2: y1, class: 'ktick' }, s);
    }
    svg('circle', { cx: 50, cy: 50, r: 31, class: 'kskirt' }, s);
    svg('circle', { cx: 50, cy: 50, r: 25, class: 'kcap' }, s);
    this.ptr = svg('line', { x1: 50, y1: 50, x2: 50, y2: 26, class: 'kptr' }, s);
    this.modDot = svg('circle', { r: 4.2, class: 'kmod-dot' }, s);
    this.out = document.createElement('div');
    this.out.className = 'kvalue';
    el.append(this.lab, s, this.out);
    this.setLabel(label);
    if (!readOnly) this._bind();
    this._draw();
  }

  setLabel(t) { this.lab.textContent = t; }
  setFmt(f) { this.fmt = f; this._draw(); }
  setRange(min, max) { this.min = min; this.max = max; this.set(this.value); }
  set(v) { this.value = Math.max(this.min, Math.min(this.max, Math.round(v))); this._draw(); }
  // Live effective value (null = none / same as stored).
  setMod(v) {
    const m = v == null || v === this.value ? null : Math.max(this.min, Math.min(this.max, v));
    if (m === this.mod) return;
    this.mod = m;
    this._draw();
  }
  setDisabled(d) { this.el.classList.toggle('disabled', d); this.el.setAttribute('aria-disabled', String(d)); }

  _deg(v) { return A0 + ((v - this.min) / (this.max - this.min || 1)) * (A1 - A0); }

  _draw() {
    const d = this._deg(this.value);
    const from = this.bipolar ? this._deg(Math.round((this.min + this.max + 1) / 2)) : A0;
    this.valArc.setAttribute('d', arc(44, from, d));
    const [px, py] = polar(50, 50, 22, d);
    this.ptr.setAttribute('x2', px.toFixed(2));
    this.ptr.setAttribute('y2', py.toFixed(2));
    const live = this.mod != null;
    this.el.classList.toggle('modulated', live);
    if (live) {
      const md = this._deg(this.mod);
      this.modArc.setAttribute('d', arc(44, d, md));
      const [mx, my] = polar(50, 50, 44, md);
      this.modDot.setAttribute('cx', mx.toFixed(2));
      this.modDot.setAttribute('cy', my.toFixed(2));
      this.out.innerHTML = '';
      const b = document.createElement('span'); b.textContent = this.fmt(this.value);
      const e = document.createElement('span'); e.className = 'eff'; e.textContent = '→' + this.fmt(this.mod);
      this.out.append(b, e);
      this.el.setAttribute('aria-valuetext', `${this.fmt(this.value)}, live ${this.fmt(this.mod)}`);
    } else {
      this.modArc.setAttribute('d', '');
      this.out.textContent = this.fmt(this.value);
      this.el.setAttribute('aria-valuetext', this.fmt(this.value));
    }
    this.el.setAttribute('aria-valuemin', this.min);
    this.el.setAttribute('aria-valuemax', this.max);
    this.el.setAttribute('aria-valuenow', this.value);
  }

  _commit(v) {
    const n = Math.max(this.min, Math.min(this.max, Math.round(v)));
    if (n === this.value) return;
    this.value = n;
    this._draw();
    this.onChange(n);
  }

  _bind() {
    const el = this.el;
    let startY = 0, startV = 0, drag = false;
    el.addEventListener('pointerdown', (e) => {
      if (e.button !== 0 || el.classList.contains('disabled')) return;
      drag = true; startY = e.clientY; startV = this.value;
      el.setPointerCapture(e.pointerId);
      el.classList.add('active');
      el.focus({ preventScroll: true });
      e.preventDefault();
    });
    el.addEventListener('pointermove', (e) => {
      if (!drag) return;
      const range = this.max - this.min;
      const px = e.shiftKey ? 600 : 160; // pixels for the full range
      this._commit(startV + ((startY - e.clientY) * range) / px);
    });
    const end = (e) => { if (!drag) return; drag = false; el.classList.remove('active'); try { el.releasePointerCapture(e.pointerId); } catch {} };
    el.addEventListener('pointerup', end);
    el.addEventListener('pointercancel', end);
    el.addEventListener('dblclick', () => { if (!el.classList.contains('disabled')) this._commit(this.def); });
    el.addEventListener('wheel', (e) => {
      if (el.classList.contains('disabled')) return;
      e.preventDefault();
      const step = e.shiftKey ? 1 : Math.max(1, Math.round((this.max - this.min) / 64));
      this._commit(this.value + (e.deltaY < 0 ? step : -step));
    }, { passive: false });
    el.addEventListener('keydown', (e) => {
      if (el.classList.contains('disabled')) return;
      const big = Math.max(2, Math.round((this.max - this.min) / 16));
      const map = { ArrowUp: 1, ArrowRight: 1, ArrowDown: -1, ArrowLeft: -1, PageUp: big, PageDown: -big };
      if (e.key in map) this._commit(this.value + map[e.key]);
      else if (e.key === 'Home') this._commit(this.min);
      else if (e.key === 'End') this._commit(this.max);
      else if (e.key === 'Delete' || e.key === 'Backspace') this._commit(this.def);
      else return;
      e.preventDefault();
      e.stopPropagation();
    });
  }
}
