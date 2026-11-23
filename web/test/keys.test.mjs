// Keyboard routing and shortcuts (web/js/keys.js, keyboard.js).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { shortcutFor, routeKey, targetKind } from '../js/keys.js';
import { keyRole, keyLabel, KEY_ROWS, NOTE_KEYS } from '../js/keyboard.js';

const ev = (code, mods = {}) => ({ code, ctrlKey: false, metaKey: false, shiftKey: false, altKey: false, ...mods });

test('octave and step-jump shortcuts', () => {
  assert.deepEqual(shortcutFor(ev('Minus')), { type: 'octave', d: -1 });
  assert.deepEqual(shortcutFor(ev('Equal')), { type: 'octave', d: 1 });
  assert.deepEqual(shortcutFor(ev('ArrowUp', { ctrlKey: true, shiftKey: true })), { type: 'octave', d: 1 });
  assert.deepEqual(shortcutFor(ev('ArrowDown', { metaKey: true, shiftKey: true })), { type: 'octave', d: -1 });
  assert.deepEqual(shortcutFor(ev('BracketLeft')), { type: 'step', d: -1 });
  assert.deepEqual(shortcutFor(ev('BracketRight')), { type: 'step', d: 1 });
  assert.deepEqual(shortcutFor(ev('ArrowRight', { ctrlKey: true, shiftKey: true })), { type: 'step', d: 1 });
  assert.deepEqual(shortcutFor(ev('ArrowLeft', { ctrlKey: true, shiftKey: true })), { type: 'step', d: -1 });
  // no clash with existing bindings or the browser / OS
  assert.equal(shortcutFor(ev('ArrowUp', { shiftKey: true })), null);   // value +1
  assert.equal(shortcutFor(ev('ArrowUp', { ctrlKey: true })), null);    // macOS Mission Control
  assert.equal(shortcutFor(ev('ArrowLeft', { altKey: true })), null);   // browser Back
  assert.equal(shortcutFor(ev('ArrowLeft', { metaKey: true })), null);  // Chrome/macOS Back
  assert.equal(shortcutFor(ev('KeyC', { ctrlKey: true })), null);       // copy track
  assert.equal(shortcutFor(ev('ArrowUp')), null);
  // the shortcut keys are not note keys
  for (const c of ['Minus', 'Equal', 'BracketLeft', 'BracketRight']) assert.ok(!(c in NOTE_KEYS), c);
});

test('keys go back to the grid after a mouse click on a control', () => {
  for (const kind of ['knob', 'select', 'button', 'checkbox']) {
    for (const code of ['ArrowUp', 'ArrowLeft', 'PageDown', 'KeyZ', 'Digit5', 'Delete', 'Space']) {
      assert.equal(routeKey(ev(code), kind, 'pointer'), 'grid', `${kind} ${code}`);
    }
    assert.equal(routeKey(ev('Tab'), kind, 'pointer'), 'native');
    assert.equal(routeKey(ev('Escape'), kind, 'pointer'), 'refocus');
  }
  assert.equal(routeKey(ev('ArrowDown'), 'body', 'keyboard'), 'grid');
});

test('controls reached with Tab keep their own keys', () => {
  assert.equal(routeKey(ev('ArrowUp'), 'knob', 'keyboard'), 'native');
  assert.equal(routeKey(ev('Home'), 'knob', 'keyboard'), 'native');
  assert.equal(routeKey(ev('KeyQ'), 'knob', 'keyboard'), 'grid');
  assert.equal(routeKey(ev('ArrowDown'), 'select', 'keyboard'), 'native');
  assert.equal(routeKey(ev('Space'), 'button', 'keyboard'), 'native');
  assert.equal(routeKey(ev('Enter'), 'button', 'keyboard'), 'native');
  assert.equal(routeKey(ev('ArrowUp'), 'button', 'keyboard'), 'grid');
  assert.equal(routeKey(ev('Space'), 'checkbox', 'keyboard'), 'native');
});

test('text fields, dialogs and the grid keep their keys', () => {
  for (const code of ['ArrowUp', 'ArrowLeft', 'KeyA', 'Digit1', 'Minus', 'BracketLeft', 'Space', 'Backspace']) {
    assert.equal(routeKey(ev(code), 'text', 'pointer'), 'native', code);
  }
  assert.equal(routeKey(ev('ArrowUp', { ctrlKey: true, shiftKey: true }), 'text', 'pointer'), 'native');
  assert.equal(routeKey(ev('Escape'), 'text', 'pointer'), 'refocus');
  assert.equal(routeKey(ev('ArrowUp'), 'dialog', 'pointer'), 'native');
  assert.equal(routeKey(ev('ArrowUp'), 'grid', 'keyboard'), 'native');
  // browser shortcuts and modifier keys alone stay with the browser
  assert.equal(routeKey(ev('KeyR', { ctrlKey: true }), 'select', 'pointer'), 'native');
  assert.equal(routeKey(ev('ShiftLeft', { shiftKey: true }), 'knob', 'pointer'), 'native');
  assert.equal(routeKey(ev('F5'), 'button', 'pointer'), 'native');
  // shortcuts work from any non-text control
  assert.equal(routeKey(ev('ArrowRight', { ctrlKey: true, shiftKey: true }), 'knob', 'keyboard'), 'grid');
});

test('targetKind', () => {
  const grid = { contains: (x) => x === cell };
  const cell = { tagName: 'SPAN', classList: { contains: () => false }, closest: () => null };
  const mk = (tagName, extra = {}) => ({ tagName, classList: { contains: (c) => (extra.cls || []).includes(c) }, closest: () => null, ...extra });
  assert.equal(targetKind(null, grid), 'body');
  assert.equal(targetKind(cell, grid), 'grid');
  assert.equal(targetKind(mk('INPUT', { type: 'text' }), grid), 'text');
  assert.equal(targetKind(mk('INPUT', { type: 'number' }), grid), 'text');
  assert.equal(targetKind(mk('INPUT', { type: 'checkbox' }), grid), 'checkbox');
  assert.equal(targetKind(mk('SELECT'), grid), 'select');
  assert.equal(targetKind(mk('BUTTON'), grid), 'button');
  assert.equal(targetKind(mk('DIV', { cls: ['knob'] }), grid), 'knob');
  assert.equal(targetKind(mk('BUTTON', { closest: () => ({}) }), grid), 'dialog');
});

test('keyboard sketch shows the step-jump keys', () => {
  assert.ok(KEY_ROWS[1].keys.includes('BracketLeft') && KEY_ROWS[1].keys.includes('BracketRight'));
  assert.equal(keyRole('BracketLeft').kind, 'jmpdown');
  assert.equal(keyRole('BracketRight').kind, 'jmpup');
  assert.equal(keyLabel('BracketLeft', 'qwertz'), 'Ü');
  assert.equal(keyLabel('BracketRight', 'qwerty'), ']');
});
