// node --test web/test
import test from 'node:test';
import assert from 'node:assert/strict';
import { Song, msg, decode, frame, CMD, K, OFF, DumpCollector, uploadMessages, demoSong, noteName } from '../js/protocol.js';

test('header layout matches the firmware (212 bytes)', () => {
  assert.equal(OFF.master + 8, K.headerBytes);
  assert.equal(OFF.entries + K.entries * 2, OFF.drum);
  assert.equal(OFF.mix + 6 * 8, OFF.master);
});

test('frame is 7-bit and framed', () => {
  const m = msg.setCell(3, 1, 10, { note: 62, vel: 90, fx: 0x52, fxv: 3 });
  assert.deepEqual(Array.from(m), [0xf0, 0x7d, 0x54, CMD.SetCell, 3, 1, 10, 62, 90, 0x52, 3, 0xf7]);
  assert.throws(() => frame(CMD.SetTempo, [200]));
});

test('tempo is split into two 7-bit bytes', () => {
  assert.deepEqual(Array.from(msg.setTempo(140)).slice(4, 6), [1, 12]);
  const s = new Song(); s.tempo = 999; assert.equal(s.tempo, K.tempoMax);
});

test('demo song equals firmware demo basics and round-trips via JSON', () => {
  const s = demoSong();
  assert.equal(s.tempo, 118);
  assert.equal(s.getCell(0, 0, 0).note, 36);
  const back = Song.fromJSON(JSON.parse(JSON.stringify(s.toJSON())));
  assert.deepEqual(back.header, s.header);
  assert.deepEqual(back.cells, s.cells);
});

test('dump collector rebuilds a song from header + chunks', () => {
  const s = demoSong();
  const c = new DumpCollector();
  const hdr = frame(CMD.SongHeader, Array.from(s.header));
  c.feed(decode(hdr));
  let out = null;
  for (let p = 0; p < K.patterns; p++) for (let t = 0; t < K.tracks; t++) for (let r = 0; r < K.rows; r += 16) {
    const pl = [p, t, r, 16];
    for (let i = 0; i < 16; i++) { const x = s.getCell(p, t, r + i); pl.push(x.note, x.vel, x.fx, x.fxv); }
    c.feed(decode(frame(CMD.PatternChunk, pl)));
  }
  out = c.feed(decode(frame(CMD.DumpEnd, [16, 6])));
  assert.ok(out);
  assert.deepEqual(out.cells, s.cells);
  assert.equal(c.progress, 1);
});

test('upload produces header + 384 chunk messages, all valid', () => {
  const m = uploadMessages(demoSong());
  assert.equal(m.length, 1 + 16 * 6 * 4);
  assert.equal(m[0].length, 5 + 212);
  for (const x of m) for (let i = 1; i < x.length - 1; i++) assert.ok(x[i] < 128);
});

test('status decode', () => {
  const pl = [1, 2, 3, 4, 0, 5, 1 | 2, 0, 120, 56, 64, 70, 60, 36, 48, 3, 40, 55, 1, 2, 3, 4, 5, 6, 7, 8, 5, 9];
  const st = decode(frame(CMD.Status, pl));
  assert.equal(st.type, 'status');
  assert.equal(st.row, 4);
  assert.equal(st.tempo, 120);
  assert.deepEqual(st.macros, [0, 6, -4]);
  assert.equal(st.extClock, true);
  assert.deepEqual(st.gates, [true, true]);
  assert.equal(st.cpu, 40);
  assert.deepEqual(st.peaks, [1, 2, 3, 4, 5, 6, 7, 8]);
});

test('foreign sysex is ignored', () => {
  assert.equal(decode(new Uint8Array([0xf0, 0x7d, 0x47, 1, 0xf7])), null);
});

test('note names', () => {
  assert.equal(noteName(60), 'C-4');
  assert.equal(noteName(61), 'C#4');
  assert.equal(noteName(0), '---');
  assert.equal(noteName(127), 'OFF');
});
