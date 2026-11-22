#!/usr/bin/env python3
"""Compare two render.cpp output directories (before / after a change).

Usage: compare.py <before_dir> <after_dir>

Columns
  out max|d|   largest difference of the 12-bit DAC codes (1 = 1 LSB), sample by sample,
               after the best constant lag of 0..4 samples (column 'lag'; a uniform
               start delay is an intended event shift)
  samples≠     number of output samples that differ
  stem max|d|  per drum voice, each hit compared sample by sample *aligned at the sample
               its first output appears* (<name>.start) (so an intended event shift does not count), in 12-bit LSB
               (Q15 / 16, i.e. at unity channel gain)
  start shift  sound start time after - before in samples, max / mean |shift|
               (trigger log: <name>.trig, same matching)
  cv           CV changes compared as (time, value) events: values must match exactly;
               the column shows max time shift in samples (or 'VALUE' on a mismatch)
  gate         same for the gate edges
  handover     largest per-voice difference in the 2 samples before a retrigger of a
               still-sounding voice (0.4.0 holds the last value there, 0.3.2 rendered
               one or two more samples of the old tail), in LSB
"""
import sys, os
import numpy as np

a_dir, b_dir = sys.argv[1], sys.argv[2]
HANDOVER = 2
names = sorted(f[:-6] for f in os.listdir(a_dir) if f.endswith(".audio"))
CVT = np.dtype([("mv", "<i4", 2), ("g", "u1")])


def events(x):
    idx = np.nonzero(np.diff(x))[0] + 1
    return idx, x[idx]


def ev_shift(xa, xb):
    ia, va = events(xa); ib, vb = events(xb)
    if len(va) != len(vb) or not np.array_equal(va, vb):
        return "VALUE"
    return int(np.max(np.abs(ib - ia))) if len(ia) else 0


worst_out = worst_stem = 0
print(f"{'scenario':20s} {'out max|d|':>10s} {'lag':>3s} {'samples≠':>9s} {'stem max|d| LSB (v0..v3)':>26s} {'start shift':>11s} {'cv shift':>9s} {'gate':>6s} {'handover':>9s}")
for n in names:
    A = np.fromfile(f"{a_dir}/{n}.audio", "<i2").astype(np.int32)
    B = np.fromfile(f"{b_dir}/{n}.audio", "<i2").astype(np.int32)
    best = None
    for lag in range(0, 5):   # interleaved L,R: lag in frames
        m = min(len(A), len(B) - 2 * lag)
        d = np.abs(A[:m] - B[2 * lag:2 * lag + m])
        r = (int(d.max()) if m else 0, int(np.count_nonzero(d)) // 2, lag)
        if best is None or r[:2] < best[:2]: best = r
    dmax, cnt, lag = best
    SA = np.fromfile(f"{a_dir}/{n}.stem", "<i4").reshape(-1, 4)
    SB = np.fromfile(f"{b_dir}/{n}.stem", "<i4").reshape(-1, 4)
    ta = [tuple(map(int, l.split())) for l in open(f"{a_dir}/{n}.start")]
    tb = [tuple(map(int, l.split())) for l in open(f"{b_dir}/{n}.start")]
    shifts, stem = [], []
    hand = 0
    for v in range(4):
        xa = [t for t, vv in ta if vv == v]; xb = [t for t, vv in tb if vv == v]
        if len(xa) != len(xb):
            shifts.append(10 ** 6)
        shifts += [b - a for a, b in zip(xa, xb)]
        smax = 0
        # before the first hit: both silent
        pre = min(xa[0] if xa else len(SA), xb[0] if xb else len(SB))
        if pre: smax = int(np.abs(SA[:pre, v] - SB[:pre, v]).max())
        for k in range(len(xa)):
            if k >= len(xb): break
            ea = xa[k + 1] if k + 1 < len(xa) else len(SA)
            eb = xb[k + 1] if k + 1 < len(xb) else len(SB)
            L = min(ea - xa[k], eb - xb[k])
            # The last samples before a retrigger are the hand-over: 0.4.0 holds
            # the old voice's last value while the new hit is set up (1-2
            # samples) where 0.3.2 rendered the old tail. Reported separately.
            H = HANDOVER if k + 1 < len(xa) else 0
            if L - H > 0:
                smax = max(smax, int(np.abs(SA[xa[k]:xa[k] + L - H, v] - SB[xb[k]:xb[k] + L - H, v]).max()))
            if H and L > 0:
                hand = max(hand, int(np.abs(SA[xa[k] + L - H:xa[k] + L, v] - SB[xb[k] + L - H:xb[k] + L, v]).max()))
        stem.append(smax)
    ca = np.fromfile(f"{a_dir}/{n}.cv", CVT); cb = np.fromfile(f"{b_dir}/{n}.cv", CVT)
    cvs = [ev_shift(ca["mv"][:, c], cb["mv"][:, c]) for c in range(2)]
    gs = [ev_shift((ca["g"] >> c) & 1, (cb["g"] >> c) & 1) for c in range(2)]
    cv = "VALUE" if "VALUE" in cvs else str(max(cvs))
    gt = "VALUE" if "VALUE" in gs else str(max(gs))
    sh = f"{max(map(abs, shifts)) if shifts else 0}/{(sum(map(abs, shifts)) / len(shifts)) if shifts else 0:.2f}"
    st = " ".join(f"{x / 16:5.2f}" for x in stem)
    print(f"{n:20s} {dmax:10d} {lag:3d} {cnt:9d} {st:>26s} {sh:>11s} {cv:>9s} {gt:>6s} {hand / 16:9.1f}")
    worst_out = max(worst_out, dmax); worst_stem = max(worst_stem, max(stem) / 16)
print(f"max output difference: {worst_out} LSB; max aligned per-voice difference: {worst_stem:.2f} LSB")
