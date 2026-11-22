// Computer-keyboard sketch for note entry (0.3.2): which key plays which note.
// Keys are matched by KeyboardEvent.code (the physical position), so the same
// keys work on QWERTY and QWERTZ; only the printed letters differ (Y/Z, - =).
// SPDX-License-Identifier: MIT

// Semitone offset from C of the current octave, by physical key.
export const NOTE_KEYS = {
  KeyZ: 0, KeyS: 1, KeyX: 2, KeyD: 3, KeyC: 4, KeyV: 5, KeyG: 6, KeyB: 7, KeyH: 8, KeyN: 9, KeyJ: 10, KeyM: 11,
  Comma: 12, KeyL: 13, Period: 14,
  KeyQ: 12, Digit2: 13, KeyW: 14, Digit3: 15, KeyE: 16, KeyR: 17, Digit5: 18, KeyT: 19, Digit6: 20, KeyY: 21, Digit7: 22, KeyU: 23,
  KeyI: 24, Digit9: 25, KeyO: 26, Digit0: 27, KeyP: 28,
};
export const OFF_KEY = 'Digit1';
export const OCT_DOWN = 'Minus';
export const OCT_UP = 'Equal';

// Physical rows with their stagger (in key widths), like a real keyboard.
export const KEY_ROWS = [
  { indent: 0, keys: ['Digit1', 'Digit2', 'Digit3', 'Digit4', 'Digit5', 'Digit6', 'Digit7', 'Digit8', 'Digit9', 'Digit0', 'Minus', 'Equal'] },
  { indent: 0.5, keys: ['KeyQ', 'KeyW', 'KeyE', 'KeyR', 'KeyT', 'KeyY', 'KeyU', 'KeyI', 'KeyO', 'KeyP'] },
  { indent: 0.75, keys: ['KeyA', 'KeyS', 'KeyD', 'KeyF', 'KeyG', 'KeyH', 'KeyJ', 'KeyK', 'KeyL'] },
  { indent: 1.25, keys: ['KeyZ', 'KeyX', 'KeyC', 'KeyV', 'KeyB', 'KeyN', 'KeyM', 'Comma', 'Period'] },
];

const QWERTZ = { KeyY: 'Z', KeyZ: 'Y', Minus: 'ß', Equal: '´' };
// Printed label of a key. layoutMap: optional Map from navigator.keyboard.getLayoutMap().
export function keyLabel(code, layout = 'qwerty', layoutMap = null) {
  if (layoutMap && layoutMap.get(code)) return layoutMap.get(code).toUpperCase();
  if (layout === 'qwertz' && QWERTZ[code]) return QWERTZ[code];
  if (code.startsWith('Key')) return code.slice(3);
  if (code.startsWith('Digit')) return code.slice(5);
  return { Minus: '-', Equal: '=', Comma: ',', Period: '.' }[code] || code;
}

// What a key does: { kind: 'note' | 'off' | 'octdown' | 'octup' | 'none', semi, sharp }.
export function keyRole(code) {
  if (code === OFF_KEY) return { kind: 'off' };
  if (code === OCT_DOWN) return { kind: 'octdown' };
  if (code === OCT_UP) return { kind: 'octup' };
  if (code in NOTE_KEYS) {
    const semi = NOTE_KEYS[code];
    return { kind: 'note', semi, sharp: [1, 3, 6, 8, 10].includes(semi % 12) };
  }
  return { kind: 'none' };
}

// Guess QWERTZ from the browser (keyboard map when available, else the language).
export async function detectLayout() {
  try {
    if (navigator.keyboard && navigator.keyboard.getLayoutMap) {
      const m = await navigator.keyboard.getLayoutMap();
      return { layout: m.get('KeyY') === 'z' ? 'qwertz' : 'qwerty', map: m };
    }
  } catch { /* not allowed (iframe) */ }
  const lang = (navigator.language || '').toLowerCase();
  return { layout: /^(de|cs|sk|hu|pl|sl|hr|sr|bs|sq|ro)\b/.test(lang) ? 'qwertz' : 'qwerty', map: null };
}

// Draw the sketch into box. noteText(code) gives the note shown on a note key.
export function renderKeyboard(box, { layout, layoutMap, noteText, title, highlight }) {
  box.innerHTML = '';
  const rows = document.createElement('div');
  rows.className = 'kb-rows';
  for (const row of KEY_ROWS) {
    const r = document.createElement('div');
    r.className = 'kb-row';
    r.style.paddingLeft = `calc(${row.indent} * var(--kw))`;
    for (const code of row.keys) {
      const role = keyRole(code);
      const k = document.createElement('div');
      k.className = `kb-key ${role.kind}${role.sharp ? ' sharp' : ''}${highlight === code ? ' hl' : ''}`;
      k.dataset.code = code;
      const cap = document.createElement('b');
      cap.textContent = keyLabel(code, layout, layoutMap);
      const sub = document.createElement('i');
      sub.textContent = role.kind === 'note' ? noteText(code) : role.kind === 'off' ? 'OFF' : role.kind === 'octdown' ? 'OCT−' : role.kind === 'octup' ? 'OCT+' : '';
      k.append(cap, sub);
      r.appendChild(k);
    }
    rows.appendChild(r);
  }
  if (title) { const t = document.createElement('div'); t.className = 'kb-title'; t.textContent = title; box.appendChild(t); }
  box.appendChild(rows);
}
