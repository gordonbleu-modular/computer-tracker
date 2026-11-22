// Level meter ballistics for the mixer (0.3.2). The module reports the peak of
// each ~50 ms window (Status bytes 18-25, 0..127 linear). The editor turns that
// into a calm, PPM/VU-like display:
//   * dB scale: -48 dB .. 0 dB full scale -> 0 .. 1
//   * fast attack (time constant 10 ms), slow release (300 ms)
//   * peak-hold marker: holds 1 s, then falls at 0.6 of the scale per second
// Animated with requestAnimationFrame between reports, so it does not jump
// in 50 ms steps. Pure logic (no DOM) so it is unit-tested in Node.
// SPDX-License-Identifier: MIT

export const METER = { floorDb: -48, attackMs: 10, releaseMs: 300, holdMs: 1000, fallPerS: 0.6, staleMs: 250 };

// 0..127 linear peak -> 0..1 on the dB scale.
export function levelToFraction(v) {
  if (!(v > 0)) return 0;
  const db = 20 * Math.log10(Math.min(v, 127) / 127);
  return Math.max(0, Math.min(1, 1 - db / METER.floorDb));
}

export class MeterBallistics {
  constructor() { this.value = 0; this.peak = 0; this.target = 0; this.holdLeft = 0; this.sinceReport = Infinity; }
  // A new report from the module (0..127).
  report(v) {
    this.target = levelToFraction(v);
    this.sinceReport = 0;
  }
  // Advance by dt milliseconds; returns { value, peak } in 0..1.
  step(dt) {
    if (!(dt > 0)) return this;
    this.sinceReport += dt;
    if (this.sinceReport > METER.staleMs) this.target = 0; // module stopped reporting
    const tau = this.target > this.value ? METER.attackMs : METER.releaseMs;
    this.value += (this.target - this.value) * (1 - Math.exp(-dt / tau));
    if (this.value < 1e-4) this.value = 0;
    if (this.value >= this.peak) { this.peak = this.value; this.holdLeft = METER.holdMs; }
    else if (this.holdLeft > 0) this.holdLeft -= dt;
    else this.peak = Math.max(this.value, this.peak - (METER.fallPerS * dt) / 1000);
    return this;
  }
}
