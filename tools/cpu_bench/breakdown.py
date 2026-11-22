#!/usr/bin/env python3
"""Per-engine / per-feature cycle breakdown (same emulator and cycle model as run_bench.py).

Usage: breakdown.py bench.elf [--lines] [--worst [--worst-lines]]
  voices : each drum engine rendered on its own (DrumVoice::render incl. its
           control-rate update every 8 samples), while sounding and after it ended
  mixer  : Mixer::process with different channel / FX settings
  --lines: engine scenarios profiled per source line (innermost inlined function)
Numbers include ~10 cycles of call overhead per sample (see "empty call").
"""
import os
import sys, subprocess, collections
ELF = sys.argv[1] if len(sys.argv) > 1 else "bench.elf"
sys.argv = [sys.argv[0], ELF, "1"]
import run_bench as rb

def measure(fn, n):
    per = []
    for _ in range(n):
        rb.state["cycles"] = 0
        rb.call(fn, 0, True)
        per.append(rb.state["cycles"])
    return per

def stats(per):
    s = sorted(per)
    return sum(s) / len(s), s[-1]

def code(e, a=64, b=64, d=127):
    return e | a << 8 | b << 16 | d << 24

ENG = ["Kick", "Pluck", "Noise", "Metal", "Modal", "Snare"]
print("=== drum voices (render() per sample, one voice) ===")
print(f"{'variant':34s} {'0-10ms avg':>10s} {'max':>5s} {'10-100ms avg':>12s} {'max':>5s} {'ended':>6s}")
variants = [
    ("Kick, drive off (B=0)", 0, 64, 0, 48), ("Kick, drive on (B=64)", 0, 64, 64, 48),
    ("Pluck (KS) C3", 1, 64, 64, 48), ("Pluck (KS) C1", 1, 64, 64, 24),
    ("Noise (SVF band-pass)", 2, 64, 64, 60), ("Metal (2-op FM)", 3, 64, 64, 60),
    ("Modal membrane", 4, 0, 64, 60), ("Modal marimba", 4, 16, 64, 60), ("Modal gong", 4, 112, 64, 60),
    ("Snare (B=30)", 5, 64, 30, 48), ("Clap (B=100)", 5, 64, 100, 48),
]
for name, e, a, b, note in variants:
    rb.call("voice_note", note)
    rb.call("voice_init", code(e, a, b, 127))
    first = measure("voice_one", 480)
    rest = measure("voice_one", 4320)
    # ended: short decay, run until the voice is done
    rb.call("voice_init", code(e, a, b, 20))
    for _ in range(40):
        rb.call("voice_run", 4800)
    ended = measure("voice_one", 200)
    f, r, en = stats(first), stats(rest), stats(ended)
    print(f"{name:34s} {f[0]:10.0f} {f[1]:5d} {r[0]:12.0f} {r[1]:5d} {en[0]:6.0f}")

print("\n=== mixer (Mixer::process per sample) ===")
mvars = [
    ("no channel live (master only)", 0), ("6 ch, all FX off", 63), ("6 ch, drive", 63 | 64),
    ("6 ch, tone LP", 63 | 128), ("6 ch, tone HP", 63 | 256), ("6 ch, drive + tone LP", 63 | 64 | 128),
    ("6 ch, master filter on", 63 | 512), ("6 ch, input VCAs (CV gate)", 63 | 1024),
    ("6 ch, everything", 63 | 64 | 128 | 512 | 1024), ("4 drum ch, everything", 15 | 64 | 128 | 512),
]
for name, c in mvars:
    rb.call("mix_init", c)
    a, m = stats(measure("mix_one", 960))
    print(f"{name:34s} avg {a:6.0f}  max {m:5d}")
a, m = stats(measure("mix_gen", 200))
print(f"{'(harness: input generation only)':34s} avg {a:6.0f}  max {m:5d}   <- subtract from the rows above")
rb.call("mix_init", 63 | 64 | 128 | 512 | 1024)
a, m = stats(measure("mix_update_one", 70))
print(f"{'update_step (1 channel coefs)':34s} avg {a:6.0f}  max {m:5d}")
a, m = stats(measure("softclip_one", 200))
print(f"{'softclip() incl. call + LCG':34s} avg {a:6.0f}  max {m:5d}")
rb.call("voice_init", code(0, 64, 64, 1))
for _ in range(10): rb.call("voice_run", 4800)
a, m = stats(measure("voice_one", 100))
print(f"{'empty call (ended voice)':34s} avg {a:6.0f}")

if "--lines" in sys.argv[0:] or "--lines" in sys.orig_argv:
    import bisect
    for sc in [int(x) for x in os.environ.get("LINE_SCENARIOS", "0,1,2").split(",")]:
        rb.call("bench_init", sc)
        rb.call("bench_run", 4800)
        rb.state["prof"] = collections.Counter()
        rb.call("bench_run", 960, True)
        prof = rb.state["prof"]; rb.state["prof"] = None
        addrs = sorted(prof)
        out = subprocess.run(["arm-none-eabi-addr2line", "-f", "-C", "-i", "-e", ELF] + [hex(a) for a in addrs],
                             capture_output=True, text=True).stdout.splitlines()
        # with -i, each address yields 1+ (func, file:line) pairs; split by re-running per address is slow,
        # so use -a to mark boundaries instead
        out = subprocess.run(["arm-none-eabi-addr2line", "-a", "-f", "-C", "-i", "-e", ELF] + [hex(a) for a in addrs],
                             capture_output=True, text=True).stdout.splitlines()
        by_line = collections.Counter(); by_func = collections.Counter()
        i = 0; cur = None
        while i < len(out):
            if out[i].startswith("0x"):
                cur = int(out[i], 16); fn = out[i + 1]; loc = out[i + 2].split("/")[-1].split(" ")[0]
                by_line[(fn.split("(")[0][-40:], loc)] += prof[cur]
                by_func[fn.split("(")[0][-50:]] += prof[cur]
                i += 3
                while i < len(out) and not out[i].startswith("0x"): i += 1
            else:
                i += 1
        print(f"\n--- scenario {sc}: innermost function (cycles/sample)")
        for k, c in by_func.most_common(18): print(f"{c/960:8.1f}  {k}")
        print(f"--- scenario {sc}: top source lines")
        for (f, l), c in by_line.most_common(25): print(f"{c/960:8.1f}  {l:22s} {f}")

if "--worst" in sys.orig_argv:
    # What the most expensive samples of the pathological scenario are made of.
    import bisect
    funcs = []
    for line in subprocess.check_output([rb.NM, "-S", "-C", ELF]).decode().splitlines():
        p = line.split(None, 3)
        if len(p) == 4 and p[2] in "tTwW":
            funcs.append((int(p[0], 16), int(p[1], 16), p[3]))
    funcs.sort(); starts = [f[0] for f in funcs]
    WSC = int(os.environ.get("WORST_SCENARIO", "2"))
    rb.call("bench_init", WSC); rb.call("bench_run", 4800)
    samples = []
    for k in range(4800):
        rb.state["prof"] = collections.Counter(); rb.state["cycles"] = 0
        rb.call("bench_one", 0, True)
        samples.append((rb.state["cycles"], rb.state["prof"]))
    rb.state["prof"] = None
    samples.sort(key=lambda x: -x[0])
    addrs = sorted({a for c, p in samples[:10] for a in p})
    out = subprocess.run(["arm-none-eabi-addr2line", "-a", "-f", "-C", "-i", "-e", ELF] + [hex(a) for a in addrs],
                         capture_output=True, text=True).stdout.splitlines()
    inner = {}
    locs = {}
    i = 0
    while i < len(out):
        if out[i].startswith("0x"):
            inner[int(out[i], 16)] = out[i + 1].split("(")[0][-45:]
            locs[int(out[i], 16)] = out[i + 2].split("/")[-1].split(" ")[0]; i += 3
            while i < len(out) and not out[i].startswith("0x"): i += 1
        else: i += 1
    for rank in range(5):
        cyc, prof = samples[rank]
        byf = collections.Counter()
        for a, c in prof.items(): byf[inner.get(a, hex(a))] += c
        print(f"\n--- worst sample #{rank+1}: {cyc} cycles")
        print("   " + ", ".join(f"{n.replace('trk::','')} {c}" for n, c in byf.most_common(12)))
        if "--worst-lines" in sys.orig_argv and rank == 0:
            byl = collections.Counter()
            for a2, c in prof.items(): byl[(locs.get(a2, "?"), inner.get(a2, "?"))] += c
            for (l, f), c in byl.most_common(int(os.environ.get("TOPN", "30"))): print(f"      {c:5d}  {l:22s} {f.replace('trk::','')}")
    import statistics
    print("\ncycles distribution (pathological): p50 %d p90 %d p99 %d p99.9 %d max %d" % tuple(
        sorted(c for c, _ in samples)[int(4800 * q)] if q < 1 else samples[0][0] for q in (0.5, 0.9, 0.99, 0.999, 1)))
