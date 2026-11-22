import test from 'node:test';
import assert from 'node:assert/strict';
import { MeterBallistics, levelToFraction, METER } from '../js/meter.js';

const run = (m, ms, report, every = 50) => {
  for (let t = 0; t < ms; t += 5) {
    if (report != null && t % every === 0) m.report(report);
    m.step(5);
  }
  return m;
};

test('meter scale: dB, -48 dB floor', () => {
  assert.equal(levelToFraction(0), 0);
  assert.equal(levelToFraction(127), 1);
  assert.ok(Math.abs(levelToFraction(127 / 2) - (1 - 6.02 / 48)) < 0.01); // -6 dB
  assert.equal(levelToFraction(127 / 1000), 0); // -60 dB: below the floor
});

test('meter: fast attack, slow release', () => {
  const m = new MeterBallistics();
  run(m, 30, 127);
  assert.ok(m.value > 0.9, `attack after 30 ms: ${m.value}`); // ~10 ms time constant
  run(m, 300, 0);
  assert.ok(m.value > 0.25 && m.value < 0.5, `release after 300 ms: ${m.value}`); // ~1/e
  run(m, 1500, 0);
  assert.ok(m.value < 0.01);
});

test('meter: steady 50 ms reports do not flicker', () => {
  const m = new MeterBallistics();
  run(m, 500, 64);
  let lo = 1, hi = 0;
  for (let t = 0; t < 500; t += 5) {
    if (t % 50 === 0) m.report(t % 100 === 0 ? 64 : 40); // alternating peaks
    m.step(5);
    lo = Math.min(lo, m.value); hi = Math.max(hi, m.value);
  }
  // Raw values differ by 0.08 of the scale; the shown bar moves within ~0.1.
  assert.ok(hi - lo < 0.1, `ripple ${hi - lo}`);
});

test('meter: peak marker holds ~1 s, then falls', () => {
  const m = new MeterBallistics();
  run(m, 50, 127);
  const p = m.peak;
  run(m, METER.holdMs - 100, 0);
  assert.equal(m.peak, p); // held
  run(m, 600, 0);
  assert.ok(m.peak < p - 0.2 && m.peak >= m.value);
});

test('meter: no reports (module gone) -> falls to 0', () => {
  const m = new MeterBallistics();
  run(m, 100, 127);
  run(m, 3000, null);
  assert.ok(m.value < 0.01);
});
