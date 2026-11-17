// Cross-checks the JS protocol against the C++ firmware code.
// Needs build-host/xcheck (built from tests/xcheck.cpp); skipped otherwise.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { Song, demoSong, uploadMessages, decode, DumpCollector, msg, K } from '../js/protocol.js';

const bin = fileURLToPath(new URL('../../build-host/xcheck', import.meta.url));
const skip = !existsSync(bin) && 'build-host/xcheck not built';
const raw = (s) => Buffer.concat([Buffer.from(s.header), Buffer.from(s.cells)]);

function splitSysex(buf) {
  const out = [];
  let start = -1;
  for (let i = 0; i < buf.length; i++) {
    if (buf[i] === 0xf0) start = i;
    else if (buf[i] === 0xf7 && start >= 0) { out.push(buf.subarray(start, i + 1)); start = -1; }
  }
  return out;
}

function roundTrip(stream) {
  const reply = execFileSync(bin, ['apply'], { input: Buffer.concat(stream.map((m) => Buffer.from(m))), maxBuffer: 1 << 22 });
  const dc = new DumpCollector();
  let song = null;
  for (const f of splitSysex(reply)) { const r = dc.feed(decode(f)); if (r) song = r; }
  return song;
}

test('JS demo song is byte-identical to song_demo()', { skip }, () => {
  assert.deepEqual(raw(demoSong()), execFileSync(bin, ['demo']));
});

test('JS empty song is byte-identical to song_clear()', { skip }, () => {
  assert.deepEqual(raw(new Song()), execFileSync(bin, ['empty']));
});

test('upload stream -> firmware parser -> GetSong dump -> same song', { skip }, () => {
  const s = demoSong();
  s.name = 'XCHECK';
  s.tempo = 133;
  s.setCell(15, 5, 63, { note: 60, vel: 0, fx: 0x53, fxv: 30 });
  const back = roundTrip(uploadMessages(s));
  assert.ok(back);
  assert.deepEqual(raw(back), raw(s));
});

test('single edits are applied by the firmware parser', { skip }, () => {
  const s = new Song();
  const stream = [
    msg.setCell(3, 1, 10, { note: 62, vel: 90, fx: 0x52, fxv: 3 }),
    msg.setTempo(171), msg.setSwing(63), msg.setDrum(2, 0, 4), msg.setMix(4, 1, 10),
    msg.setEntry(5, 9, 3), msg.setSongLen(6), msg.setPatLen(7, 48), msg.setZeroNote(24),
  ];
  s.setCell(3, 1, 10, { note: 62, vel: 90, fx: 0x52, fxv: 3 });
  s.tempo = 171; s.swing = 63; s.setDrum(2, 0, 4); s.setMix(4, 1, 10);
  s.setEntry(5, 9, 3); s.songLen = 6; s.setPatLen(7, 48); s.cvZero = 24;
  const back = roundTrip(stream);
  assert.deepEqual(raw(back), raw(s));
  assert.equal(back.header.length, K.headerBytes);
});

test('v2: SetMacroMask + Panel report match the firmware', { skip }, async () => {
  const { effectiveDrum, effectiveFilter } = await import('../js/protocol.js');
  const s = demoSong();
  s.setMacroMask(0, 0b0101);
  s.setMacroMask(1, 0b1110);
  s.setDrum(0, 1, 110); s.setDrum(1, 1, 20); s.setDrum(2, 2, 10); s.setDrum(3, 2, 90);
  s.setMaster(2, 70);
  const stream = [msg.setHeader(new Song()), ...uploadMessages(s).slice(0, 1), msg.setMacroMask(0, 0b0101), msg.setMacroMask(1, 0b1110)];
  // Song state survives the round trip through the firmware parser.
  const back = roundTrip(stream);
  assert.equal(back.macroMask(0), 0b0101);
  assert.equal(back.macroMask(1), 0b1110);
  assert.deepEqual(Buffer.from(back.header), Buffer.from(s.header));
  // Panel report for the fixed status in tests/xcheck.cpp.
  const reply = execFileSync(bin, ['panel'], { input: Buffer.concat(stream.map((m) => Buffer.from(m))) });
  const frames = splitSysex(reply);
  assert.equal(frames.length, 1);
  const m = decode(frames[0]);
  assert.equal(m.type, 'panel');
  assert.equal(m.selectPage, true); assert.equal(m.downHeld, false);
  assert.deepEqual(m.knobs, [10, 64, 127]);
  assert.deepEqual(m.waiting, [true, false, true]);
  assert.deepEqual(m.macros, [25, -40, -64]);
  assert.deepEqual(m.masks, [0b0101, 0b1110]);
  assert.equal(m.slot, 4); assert.equal(m.mode, 2); assert.equal(m.pattern, 9); assert.equal(m.transpose, 12);
  assert.equal(m.tempo, s.tempo); assert.equal(m.swing, s.swing);
  for (let v = 0; v < 4; v++) {
    assert.equal(m.effDecay[v], effectiveDrum(s, m.macros, v, 1), `decay ${v}`);
    assert.equal(m.effA[v], effectiveDrum(s, m.macros, v, 2), `A ${v}`);
  }
  assert.equal(m.effFilter, effectiveFilter(s, m.macros));
});
