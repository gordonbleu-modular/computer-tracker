// node --test web/test
import test from 'node:test';
import assert from 'node:assert/strict';
import { Song, msg, decode, frame, CMD, K, OFF, DumpCollector, uploadMessages, demoSong, noteName,
  FX, FX_BY_CODE, fxFor, pctOf, pctTo, fxShow, fxStore, velShow, velStore, VCA_SRC, effectiveCv, effectiveIn, NOTE_OFF } from '../js/protocol.js';

test('header layout matches the firmware (212 bytes)', () => {
  assert.equal(OFF.master + 8, K.headerBytes);
  assert.equal(OFF.entries + K.entries * 2, OFF.drum);
  assert.equal(OFF.mix + 6 * 8, OFF.master);
});

test('frame is 7-bit and framed', () => {
  const m = msg.setCell(3, 1, 10, { note: 62, vel: 90, fx: 0x52, fxv: 3 });
  assert.deepEqual(Array.from(m), [0xf0, 0x7d, 0x54, CMD.SetCell, 3, 1, 10, 62, 90, 0x52, 3, 0xf7]);
  // Firmware 0.2 (protocol 2): same 7-byte form.
  assert.deepEqual(Array.from(msg.setCell(3, 1, 10, { note: 62, vel: 90, fx: 0x52, fxv: 3 }, 2)), Array.from(m));
  // Firmware 0.3.0 (protocol 3): its 9-byte form with the second effect cleared.
  const o = msg.setCell(3, 1, 10, { note: 62, vel: 90, fx: 0x52, fxv: 3 }, 3);
  assert.deepEqual(Array.from(o), [0xf0, 0x7d, 0x54, CMD.SetCell, 3, 1, 10, 62, 90, 0x52, 3, 0, 0, 0xf7]);
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
  out = c.feed(decode(frame(CMD.DumpEnd, [16, 8])));
  assert.ok(out);
  assert.deepEqual(out.cells, s.cells);
  assert.equal(c.progress, 1);
  assert.equal(c.legacy, false);
});

test('dump from firmware 0.3.0 (6-byte cells, two effects) drops FX2', () => {
  const s = demoSong();
  const c = new DumpCollector();
  c.feed(decode(frame(CMD.SongHeader, Array.from(s.header))));
  for (let p = 0; p < K.patterns; p++) for (let t = 0; t < K.tracks; t++) for (let r = 0; r < K.rows; r += 16) {
    const pl = [p, t, r, 16];
    for (let i = 0; i < 16; i++) {
      const x = s.getCell(p, t, r + i);
      const two = p === 2 && t === 2 && r + i === 15, only2 = p === 0 && t === 7 && r + i === 3;
      if (only2) pl.push(60, 0, 0, 0, FX.K.code, 8);
      else pl.push(x.note, x.vel, x.fx, x.fxv, two ? FX.P.code : 0, two ? 60 : 0);
    }
    c.feed(decode(frame(CMD.PatternChunk, pl)));
  }
  const out = c.feed(decode(frame(CMD.DumpEnd, [16, 8])));
  assert.ok(out && !c.legacy);
  assert.deepEqual(out.getCell(2, 2, 15), { note: 84, vel: 70, fx: FX.R.code, fxv: 4 });
  assert.deepEqual(out.getCell(0, 7, 3), { note: 60, vel: 0, fx: FX.K.code, fxv: 8 }); // FX2 moves into the empty FX1
  assert.equal(out.macroCvMask(0), s.macroCvMask(0)); // header unchanged
});

test('dump from firmware 0.2 (6 tracks, 4-byte cells) is migrated', () => {
  const s = demoSong();
  const c = new DumpCollector();
  c.tracksExpected = 6;
  const v2 = new Song(); v2.header.set(s.header);
  v2.header[OFF.macroCvin] = 0; v2.header[OFF.macroCvin + 1] = 0; v2.setMix(4, 6, 0); v2.setMix(4, 7, 0);
  c.feed(decode(frame(CMD.SongHeader, Array.from(v2.header))));
  for (let p = 0; p < K.patterns; p++) for (let t = 0; t < 6; t++) for (let r = 0; r < K.rows; r += 16) {
    const pl = [p, t, r, 16];
    for (let i = 0; i < 16; i++) { const x = s.getCell(p, t, r + i); pl.push(x.note, x.vel, x.fx, x.fxv); }
    c.feed(decode(frame(CMD.PatternChunk, pl)));
  }
  assert.equal(c.progress, 1);
  const out = c.feed(decode(frame(CMD.DumpEnd, [16, 6])));
  assert.ok(out && c.legacy);
  assert.equal(out.getCell(0, 0, 0).note, 36);
  assert.equal(out.getCell(0, 6, 0).note, 0); // IN tracks empty
  assert.equal(out.macroCvMask(0), 0);
  assert.equal(out.vcaSource(0), VCA_SRC.cvGate); // decay 40 > 0: follows the CV gate, as in 0.2
  assert.equal(out.mix(4, 7), 8);
});

test('upload: header + 512 four-byte chunks, 384 for firmware 0.2, 6-byte for 0.3.0', () => {
  const m = uploadMessages(demoSong());
  assert.equal(m.length, 1 + 16 * 8 * 4);
  assert.equal(m[0].length, 5 + 212);
  assert.equal(m[1].length, 5 + 4 + 16 * 4);
  for (const x of m) for (let i = 1; i < x.length - 1; i++) assert.ok(x[i] < 128);
  const o = uploadMessages(demoSong(), 2);
  assert.equal(o.length, 1 + 16 * 6 * 4);
  assert.equal(o[1].length, 5 + 4 + 16 * 4);
  const t = uploadMessages(demoSong(), 3);
  assert.equal(t.length, 1 + 16 * 8 * 4);
  assert.equal(t[1].length, 5 + 4 + 16 * 6);
  assert.equal(t[1][4 + 4 + 4], 0); // FX2 of the first cell cleared
});

test('0-99 display values map to 0-127 and back exactly', () => {
  for (let d = 0; d <= 99; d++) assert.equal(pctOf(pctTo(d)), d);
  assert.equal(pctTo(99), 127); assert.equal(pctTo(0), 0); assert.equal(pctOf(64), 50);
  assert.equal(fxShow(FX.D.code, 127), 99); assert.equal(fxStore(FX.D.code, 50), 64);
  assert.equal(fxStore(FX.R.code, 12), 8); assert.equal(fxStore(FX.R.code, 0), 2); // ratchet 2-8
  assert.equal(fxShow(FX.G.code, 16), 16); assert.equal(fxStore(FX.G.code, 80), 64);
  assert.equal(fxStore(FX.P.code, 50), 50); assert.equal(fxStore(FX.K.code, 99), 15);
  assert.equal(velShow(0), -1); assert.equal(velShow(127), 99); assert.equal(velShow(1), 1);
  assert.equal(velStore(0), 1); assert.equal(velStore(99), 127);
});

test('effects per track kind', () => {
  const letters = (t) => fxFor(t).map(([l]) => l).join('');
  assert.equal(letters(0), 'ABDRPKW');
  assert.equal(letters(4), 'GSRPK');   // CV tracks are not in the mix: no W
  assert.equal(letters(6), 'DGRPKW');
  // W (0.4.1): ASCII 'W' like FxSend in song.h, value 00-99 <-> 0-127
  assert.equal(FX.W.code, 'W'.charCodeAt(0));
  assert.equal(fxStore(FX.W.code, 99), 127); assert.equal(fxShow(FX.W.code, 127), 99); assert.equal(fxStore(FX.W.code, 50), 64);
  for (const f of Object.values(FX)) assert.ok(f.name && f.help && FX_BY_CODE[f.code]);
});

test('song file from editor 0.2 (6-track cells) loads as 8 tracks', () => {
  const s = demoSong();
  const v1cells = new Uint8Array(16 * 6 * 64 * 4);
  for (let p = 0; p < 16; p++) for (let t = 0; t < 6; t++) for (let r = 0; r < 64; r++) {
    const c = s.getCell(p, t, r); const o = ((p * 6 + t) * 64 + r) * 4;
    v1cells[o] = c.note; v1cells[o + 1] = c.vel; v1cells[o + 2] = c.fx; v1cells[o + 3] = c.fxv;
  }
  const json = { format: 'workshop-tracker-song', version: 1, header: Array.from(s.header), cells: Buffer.from(v1cells).toString('base64') };
  const back = Song.fromJSON(json);
  assert.equal(back.getCell(1, 3, 0).fx, FX.D.code);
  assert.equal(back.getCell(0, 6, 0).note, 0);
  assert.equal(back.macroCvMask(0), 0);
});

test('song file from editor 0.3.0 (6-byte cells) drops FX2', () => {
  const s = demoSong();
  const c6 = new Uint8Array(16 * 8 * 64 * 6);
  for (let p = 0; p < 16; p++) for (let t = 0; t < 8; t++) for (let r = 0; r < 64; r++) {
    const c = s.getCell(p, t, r); const o = ((p * 8 + t) * 64 + r) * 6;
    c6[o] = c.note; c6[o + 1] = c.vel; c6[o + 2] = c.fx; c6[o + 3] = c.fxv;
  }
  const o = ((2 * 8 + 2) * 64 + 15) * 6; c6[o + 4] = FX.P.code; c6[o + 5] = 60;
  const back = Song.fromJSON({ format: 'workshop-tracker-song', version: 2, header: Array.from(s.header), cells: Buffer.from(c6).toString('base64') });
  assert.deepEqual(back.cells, s.cells);
  assert.equal(s.toJSON().version, 3);
});

test('v3 effective CV/IN values (same maths as protocol.h)', () => {
  const s = new Song();
  s.setCv(0, 0, 8); s.setCv(1, 0, 0); s.setCv(0, 1, 30); s.setMix(4, 4, 50); s.setMix(5, 2, 10);
  s.setMacroCvMask(1, 0x0b);
  const m = [40, -20, 0];
  assert.equal(effectiveCv(s, m, 0, 0), 28);
  assert.equal(effectiveCv(s, m, 1, 0), 0);
  assert.equal(effectiveCv(s, m, 0, 1), 10);
  assert.equal(effectiveIn(s, m, 0, 4), 90);
  assert.equal(effectiveIn(s, m, 0, 2), 0);
  assert.equal(effectiveIn(s, m, 1, 2), 0);
  assert.equal(effectiveCv(s, [-41, 0, 0], 0, 0), 1); // trunc toward zero like C++
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
  assert.deepEqual(st.inGates, [false, false]);
  assert.equal(st.cpu, 40);
  assert.deepEqual(st.peaks, [1, 2, 3, 4, 5, 6, 7, 8]);
  assert.equal(st.reverbPeak, 0); // 0.3.2 firmware: no reverb meter byte
  assert.equal(decode(frame(CMD.Status, [...pl, 77])).reverbPeak, 77); // v6
});

test('v6: reverb fields, defaults and migration of older songs', async () => {
  const { RV, RV_DEFAULTS, OFF, msg, Song } = await import('../js/protocol.js');
  const s = new Song();
  assert.equal(s.fxVer, 2);
  RV_DEFAULTS.forEach((v, i) => assert.equal(s.master(RV.return + i), v));
  for (let c = 0; c < 6; c++) assert.equal(s.send(c), 0); // new songs start dry
  s.setSend(2, 200);
  assert.equal(s.send(2), 127);
  assert.deepEqual(Array.from(msg.setSend(5, 33)), [0xf0, 0x7d, 0x54, 0x15, 5, 8, 33, 0xf7]);
  // A song file from editor 0.3.2 (reserved bytes 0) gets sends 0 + defaults.
  const j = s.toJSON();
  j.header[OFF.fxVer] = 0; j.header[OFF.fxSend + 2] = 50; j.header[OFF.master + RV.decay] = 0;
  const old = Song.fromJSON(j);
  assert.equal(old.send(2), 0);
  assert.equal(old.master(RV.decay), RV_DEFAULTS[2]);
  assert.equal(old.fxVer, 2);
  // A 0.4.0 file keeps its values.
  const keep = Song.fromJSON(s.toJSON());
  assert.equal(keep.send(2), 127);
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

// ------------------------------------------------------------- 0.3.2 CV ----
test('CV tracks: C0 = 0 V, no velocity, notes folded into C0..B5', async () => {
  const { CV_ZERO_NOTE, CV_NOTE_MIN, CV_NOTE_MAX, cvFold, cvVolts } = await import('../js/protocol.js');
  assert.equal(CV_ZERO_NOTE, 12);
  assert.equal(cvVolts(12), 0);
  assert.equal(cvVolts(24), 1);
  assert.ok(cvVolts(CV_NOTE_MAX) < 6);
  assert.equal(cvFold(6), 18);
  assert.equal(cvFold(100), 76);
  const s = new Song();
  assert.equal(s.cvZero, 12);
  s.setCell(0, 4, 0, { note: 100, vel: 90, fx: 0, fxv: 0 });
  assert.deepEqual(s.getCell(0, 4, 0), { note: 76, vel: 0, fx: 0, fxv: 0 });
  s.setCell(0, 4, 1, { note: NOTE_OFF, vel: 90, fx: 0, fxv: 0 });
  assert.equal(s.getCell(0, 4, 1).note, NOTE_OFF);
  s.setCell(0, 0, 0, { note: 100, vel: 90, fx: 0, fxv: 0 }); // drums keep their range and velocity
  assert.deepEqual(s.getCell(0, 0, 0), { note: 100, vel: 90, fx: 0, fxv: 0 });
  // Demo: every CV note in range.
  const d = demoSong();
  for (let p = 0; p < K.patterns; p++) for (let t = 4; t < 6; t++) for (let r = 0; r < K.rows; r++) {
    const c = d.getCell(p, t, r);
    assert.equal(c.vel, 0);
    if (c.note && c.note !== NOTE_OFF) assert.ok(c.note >= CV_NOTE_MIN && c.note <= CV_NOTE_MAX);
  }
});

test('songs from <= 0.3.1 (C2 = 0 V) keep their CV voltages', () => {
  const s = new Song();
  s.cvZero = 36;
  s.setCellRaw(0, 4, 0, { note: 36, vel: 80, fx: 0, fxv: 0 }); // 0 V
  s.setCellRaw(0, 4, 1, { note: 50, vel: 0, fx: 0, fxv: 0 });  // D3 old = 14 st above 0 V
  s.setCellRaw(0, 5, 2, { note: 30, vel: 0, fx: 0, fxv: 0 });  // below the old 0 V note
  s.setCellRaw(0, 5, 3, { note: 120, vel: 0, fx: 0, fxv: 0 }); // above 6 V
  s.setCellRaw(0, 6, 0, { note: 30, vel: 70, fx: 0, fxv: 0 }); // IN track: untouched
  assert.equal(s.normalizeCv(), true);
  assert.equal(s.cvZero, 12);
  assert.deepEqual(s.getCell(0, 4, 0), { note: 12, vel: 0, fx: 0, fxv: 0 });
  assert.equal(s.getCell(0, 4, 1).note, 26);
  assert.equal(s.getCell(0, 5, 2).note, 18);
  assert.equal(s.getCell(0, 5, 3).note, 72);
  assert.deepEqual(s.getCell(0, 6, 0), { note: 30, vel: 70, fx: 0, fxv: 0 });
  assert.equal(s.normalizeCv(), false); // idempotent
  // An old song file is converted on import.
  const old = new Song();
  old.header[16] = 36;
  old.setCellRaw(1, 5, 0, { note: 48, vel: 0, fx: 0, fxv: 0 });
  const back = Song.fromJSON(JSON.parse(JSON.stringify(old.toJSON())));
  assert.equal(back.getCell(1, 5, 0).note, 24);
  assert.equal(back.cvZero, 12);
});

test('v7: live mutes (SetMaster 8/9, Status flags 32/64), not stored in the song', async () => {
  const { MASTER_MUTE, PROTOCOL_VERSION } = await import('../js/protocol.js');
  assert.ok(PROTOCOL_VERSION >= 7);
  assert.deepEqual(MASTER_MUTE, { reverb: 8, master: 9 });
  assert.deepEqual([...msg.setLiveMute('reverb', true)], [...frame(CMD.SetMaster, [8, 127])]);
  assert.deepEqual([...msg.setLiveMute('master', false)], [...frame(CMD.SetMaster, [9, 0])]);
  const pl = [1, 2, 3, 4, 0, 5, 32 | 64 | 1, 0, 120, 56, 64, 70, 60, 36, 48, 3, 40, 55, 1, 2, 3, 4, 5, 6, 7, 8, 5, 9, 70];
  const st = decode(frame(CMD.Status, pl));
  assert.equal(st.reverbMuted, true);
  assert.equal(st.masterMuted, true);
  assert.equal(st.extClock, true);
  pl[6] = 32;
  const s2 = decode(frame(CMD.Status, pl));
  assert.equal(s2.reverbMuted, true);
  assert.equal(s2.masterMuted, false);
  pl[6] = 0;
  const s3 = decode(frame(CMD.Status, pl)); // older firmware never sets these bits
  assert.equal(s3.reverbMuted || s3.masterMuted, false);
});

test('v8: click/grit param, fx_ver 2 migration, header for older firmware', async () => {
  const { OFF, Song, DRUM_X, ENGINES, PROTOCOL_VERSION, FX_VERSION } = await import('../js/protocol.js');
  assert.equal(PROTOCOL_VERSION, 8);
  assert.equal(FX_VERSION, 2);
  assert.equal(DRUM_X, 6);
  assert.deepEqual(ENGINES.map((e) => e.x), ['Click', 'Grit', 'Grit', 'Grit', 'Grit', 'Grit']);
  const s = demoSong();
  for (let v = 0; v < 4; v++) assert.equal(s.drum(v, DRUM_X), 0); // neutral by default
  // a 0.4.0-0.4.2 song (fx_ver 1) keeps its reverb, junk in bytes 6/7 is cleared
  s.header[OFF.fxVer] = 1;
  s.setSend(1, 90);
  s.header[OFF.drum + 6] = 77; s.header[OFF.drum + 3 * 8 + 7] = 5;
  assert.equal(s.migrateFx(), true);
  assert.equal(s.send(1), 90);
  assert.equal(s.drum(0, DRUM_X), 0);
  assert.equal(s.header[OFF.drum + 3 * 8 + 7], 0);
  assert.equal(s.fxVer, 2);
  // firmware 0.4.0-0.4.2 gets fx_ver 1, 0.4.3 gets 2
  s.setDrum(2, DRUM_X, 50);
  const h7 = msg.setHeader(s, 7), h8 = msg.setHeader(s, 8); // payload from byte 4
  assert.equal(h7[4 + OFF.fxVer], 1);
  assert.equal(h8[4 + OFF.fxVer], 2);
  assert.equal(h8[4 + OFF.drum + 2 * 8 + DRUM_X], 50);
  assert.equal(uploadMessages(s, 7)[0][4 + OFF.fxVer], 1);
});
