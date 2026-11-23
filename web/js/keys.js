// Keyboard routing between the tracker grid and the other controls, and the
// octave / step-jump shortcuts. Pure functions (no DOM) so they can be tested
// in node; app.js wires them up.
//
// Rules (also in the Keys dialog and docs/CONTROLS.md):
// * Text fields (song name, number fields) keep every key. Escape goes back
//   to the grid.
// * A control you clicked with the mouse (knob, dropdown, button, checkbox)
//   keeps no keys: the next key goes to the grid and acts there (an arrow
//   moves the cursor, a note key enters a note, Space plays ...).
// * A control you reached with Tab keeps its own keys (knob: arrows, PgUp /
//   PgDn, Home / End, Delete; dropdown: arrows, Home / End, Space, Enter;
//   button / checkbox: Space, Enter). Everything else goes to the grid.
// * The octave / step-jump shortcuts work everywhere except in text fields.
// * An open dialog or an open dropdown list keeps its keys (the browser
//   does not pass them to the page anyway).

// Octave: - / = (any column) or Ctrl+Shift+Up / Down.
// Step jump: [ / ] (QWERTZ: Ü / +) or Ctrl+Shift+Left / Right.
// Ctrl+Shift instead of Ctrl or Alt: Ctrl+arrows is taken by macOS (Mission
// Control, Spaces), Alt+Left/Right is browser Back/Forward, Cmd+Left/Right
// is Back/Forward in Chrome on macOS, Shift+arrows changes values.
export function shortcutFor(e) {
  const ctrl = e.ctrlKey || e.metaKey;
  if (e.altKey) return null;
  if (ctrl && e.shiftKey) {
    switch (e.code) {
      case 'ArrowUp': return { type: 'octave', d: 1 };
      case 'ArrowDown': return { type: 'octave', d: -1 };
      case 'ArrowRight': return { type: 'step', d: 1 };
      case 'ArrowLeft': return { type: 'step', d: -1 };
      default: return null;
    }
  }
  if (ctrl) return null;
  switch (e.code) {
    case 'Minus': return { type: 'octave', d: -1 };
    case 'Equal': return { type: 'octave', d: 1 };
    case 'BracketLeft': return { type: 'step', d: -1 };
    case 'BracketRight': return { type: 'step', d: 1 };
    default: return null;
  }
}

const OWN_KEYS = {
  knob: ['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'PageUp', 'PageDown', 'Home', 'End', 'Delete', 'Backspace'],
  select: ['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'PageUp', 'PageDown', 'Home', 'End', 'Space', 'Enter'],
  button: ['Space', 'Enter', 'NumpadEnter'],
  checkbox: ['Space'],
};
const NEVER = new Set(['Tab', 'ShiftLeft', 'ShiftRight', 'ControlLeft', 'ControlRight', 'AltLeft', 'AltRight',
  'MetaLeft', 'MetaRight', 'CapsLock', 'ContextMenu', 'OSLeft', 'OSRight']);

// What to do with a keydown that did not happen in the grid.
// target: 'grid' | 'text' | 'knob' | 'select' | 'button' | 'checkbox' | 'body' | 'dialog'
// via: 'pointer' | 'keyboard' (how the control got its focus)
// Returns 'native' (leave it), 'refocus' (only focus the grid) or 'grid'
// (focus the grid and handle the key there).
export function routeKey(e, target, via) {
  if (target === 'grid' || target === 'dialog') return 'native';
  if (NEVER.has(e.code) || /^F\d+$/.test(e.code)) return 'native';
  if (e.code === 'Escape') return target === 'body' ? 'native' : 'refocus';
  if (target === 'text') return 'native';
  if (shortcutFor(e)) return 'grid';
  const ctrl = e.ctrlKey || e.metaKey;
  if (ctrl || e.altKey) return 'native';   // browser / OS shortcuts, copy & paste in fields
  if (target === 'body') return 'grid';
  if (via === 'keyboard' && (OWN_KEYS[target] || []).includes(e.code)) return 'native';
  return 'grid';
}

// Kind of element for routeKey.
export function targetKind(elm, grid) {
  const doc = typeof document !== 'undefined' ? document : null;
  if (!elm || (doc && (elm === doc.body || elm === doc.documentElement))) return 'body';
  if (grid && (elm === grid || grid.contains(elm))) return 'grid';
  if (elm.closest && elm.closest('dialog[open]')) return 'dialog';
  const tag = elm.tagName;
  if (tag === 'TEXTAREA' || elm.isContentEditable) return 'text';
  if (tag === 'INPUT') {
    const t = (elm.type || 'text').toLowerCase();
    if (t === 'checkbox' || t === 'radio') return 'checkbox';
    if (t === 'button' || t === 'submit' || t === 'file' || t === 'reset') return 'button';
    if (t === 'range') return 'knob';
    return 'text';
  }
  if (tag === 'SELECT') return 'select';
  if (tag === 'BUTTON' || tag === 'LABEL' || tag === 'A') return 'button';
  if (elm.classList && elm.classList.contains('knob')) return 'knob';
  return 'body';
}
