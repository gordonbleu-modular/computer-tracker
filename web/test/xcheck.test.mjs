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
    // CV cell from an old editor: velocity dropped, note folded into C0..B5 (both sides).
    [0xf0, 0x7d, 0x54, 0x10, 2, 4, 5, 100, 80, 0, 0, 0xf7],
  ];
  s.setCell(3, 1, 10, { note: 62, vel: 90, fx: 0x52, fxv: 3 });
  s.tempo = 171; s.swing = 63; s.setDrum(2, 0, 4); s.setMix(4, 1, 10);
  s.setEntry(5, 9, 3); s.songLen = 6; s.setPatLen(7, 48); // SetZeroNote is ignored since 0.3.2 (C0 = 0 V)
  s.setCell(2, 4, 5, { note: 100, vel: 80, fx: 0, fxv: 0 });
  assert.deepEqual(s.getCell(2, 4, 5), { note: 76, vel: 0, fx: 0, fxv: 0 });
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

test('v3/v4: IN tracks, CV/IN macro routing and Panel v3 match the firmware', { skip }, async () => {
  const { effectiveCv, effectiveIn } = await import('../js/protocol.js');
  const s = new Song();
  s.setCell(4, 7, 63, { note: 60, vel: 90, fx: 0x44, fxv: 20 });
  s.setCell(0, 6, 0, { note: 60, vel: 127, fx: 0x50, fxv: 40 });
  s.setCv(0, 0, 8); s.setCv(1, 0, 0); s.setCv(0, 1, 30); s.setCv(1, 1, 5);
  s.setMix(4, 4, 50); s.setMix(5, 4, 100); s.setMix(4, 2, 30); s.setMix(5, 2, 10); s.setMix(4, 6, 3);
  s.setMacroMask(0, 0b0011); s.setMacroCvMask(0, 0b0101);
  s.setMacroMask(1, 0b1100); s.setMacroCvMask(1, 0b1010);
  const stream = [
    msg.setHeader(new Song()),
    msg.setCell(4, 7, 63, s.getCell(4, 7, 63)),
    msg.setCells(s, 0, 6, 0, 1),
    msg.setCv(0, 0, 8), msg.setCv(1, 0, 0), msg.setCv(0, 1, 30), msg.setCv(1, 1, 5),
    msg.setMix(4, 4, 50), msg.setMix(5, 4, 100), msg.setMix(4, 2, 30), msg.setMix(5, 2, 10), msg.setMix(4, 6, 3),
    msg.setMacroMask(0, 0b0011, 0b0101), msg.setMacroMask(1, 0b1100, 0b1010),
  ];
  const back = roundTrip(stream);
  assert.deepEqual(raw(back), raw(s));
  // The 0.3.0 forms (9-byte SetCell, 6-byte cells) are accepted; FX2 is dropped.
  const old = roundTrip([msg.setHeader(new Song()), msg.setCell(4, 7, 63, s.getCell(4, 7, 63), 3), msg.setCells(s, 0, 6, 0, 1, 3),
    msg.setCv(0, 0, 8), msg.setCv(1, 0, 0), msg.setCv(0, 1, 30), msg.setCv(1, 1, 5),
    msg.setMix(4, 4, 50), msg.setMix(5, 4, 100), msg.setMix(4, 2, 30), msg.setMix(5, 2, 10), msg.setMix(4, 6, 3),
    msg.setMacroMask(0, 0b0011, 0b0101), msg.setMacroMask(1, 0b1100, 0b1010)]);
  assert.deepEqual(raw(old), raw(s));
  const reply = execFileSync(bin, ['panel'], { input: Buffer.concat(stream.map((m) => Buffer.from(m))) });
  const m = decode(splitSysex(reply)[0]);
  assert.equal(m.v3, true);
  assert.deepEqual(m.cvinMasks, [0b0101, 0b1010]);
  for (let c = 0; c < 2; c++) {
    assert.equal(m.effCvGate[c], effectiveCv(s, m.macros, c, 0), `gate ${c}`);
    assert.equal(m.effCvGlide[c], effectiveCv(s, m.macros, c, 1), `glide ${c}`);
    assert.equal(m.effInDecay[c], effectiveIn(s, m.macros, c, 4), `in decay ${c}`);
    assert.equal(m.effInDrive[c], effectiveIn(s, m.macros, c, 2), `in drive ${c}`);
  }
});

test('v6: reverb sends and master reverb params round-trip through the firmware parser', { skip }, async () => {
  const { RV } = await import('../js/protocol.js');
  const s = new Song();
  const stream = [msg.setSend(0, 20), msg.setSend(3, 127), msg.setSend(5, 64),
    msg.setMaster(RV.return, 110), msg.setMaster(RV.size, 5), msg.setMaster(RV.decay, 127),
    msg.setMaster(RV.tone, 0), msg.setMaster(RV.lofi, 99)];
  s.setSend(0, 20); s.setSend(3, 127); s.setSend(5, 64);
  s.setMaster(RV.return, 110); s.setMaster(RV.size, 5); s.setMaster(RV.decay, 127);
  s.setMaster(RV.tone, 0); s.setMaster(RV.lofi, 99);
  const back = roundTrip(stream);
  assert.deepEqual(raw(back), raw(s));
  assert.equal(back.send(3), 127);
});

test('v6: an old header (fx version 0) is migrated the same way on both sides', { skip }, async () => {
  const { OFF, RV_DEFAULTS, RV } = await import('../js/protocol.js');
  const old = demoSong();
  // What a 0.3.2 editor/firmware would send: reserved bytes 0, master 3..7 junk.
  old.header.fill(0, OFF.fxSend, OFF.fxVer + 1);
  old.header[OFF.fxSend + 2] = 99;              // stray byte must not become a send
  for (let i = 0; i < 5; i++) old.header[OFF.master + 3 + i] = 7;
  const back = roundTrip([msg.setHeader(old)]);
  const js = Song.fromJSON(old.toJSON());
  for (const s of [back, js]) {
    for (let c = 0; c < K.channels; c++) assert.equal(s.send(c), 0);
    RV_DEFAULTS.forEach((v, i) => assert.equal(s.master(RV.return + i), v));
    assert.equal(s.fxVer, 2);
  }
  assert.deepEqual(Buffer.from(back.header), Buffer.from(js.header));
});

test('v7: live mutes go through the firmware parser without touching the song', { skip }, () => {
  const s = demoSong();
  const back = roundTrip([...uploadMessages(s), msg.setLiveMute('reverb', true), msg.setLiveMute('master', true), msg.setLiveMute('master', false)]);
  assert.deepEqual(raw(back), raw(s));
});

test('v8: click/grit (drum param 6) and the fx_ver 1 -> 2 migration match the firmware', { skip }, async () => {
  const { OFF, DRUM_X } = await import('../js/protocol.js');
  const s = demoSong();
  s.setDrum(0, DRUM_X, 90); s.setDrum(3, DRUM_X, 31);
  assert.deepEqual(raw(roundTrip(uploadMessages(s))), raw(s));
  const back = roundTrip([msg.setHeader(new Song()), msg.setDrum(1, DRUM_X, 64)]);
  assert.equal(back.drum(1, DRUM_X), 64);
  // a 0.4.2 header (fx_ver 1) with junk in the reserved bytes: same result on both sides
  const old = demoSong();
  old.header[OFF.fxVer] = 1;
  old.header[OFF.drum + 6] = 99; old.header[OFF.drum + 15] = 12;
  const b2 = roundTrip([msg.setHeader(old)]);
  const js = Song.fromJSON(old.toJSON());
  assert.deepEqual(Buffer.from(b2.header), Buffer.from(js.header));
  assert.equal(js.drum(0, DRUM_X), 0);
});
