#!/usr/bin/env python3
"""Estimate Cortex-M0+ cycles per sample of TrackerEngine::process().

Runs bench.elf (a bare-metal build of the engine) in the Unicorn emulator and
applies a simple RP2040 Cortex-M0+ timing model per executed instruction:
1 cycle ALU/MUL, 2 for loads/stores, 1+N for LDM/STM/PUSH/POP, 2 for taken
branches, 3 for BL. Zero-wait-state SRAM is assumed (the firmware runs from
RAM).

Division: the Pico SDK routes 32-bit / and % to the RP2040's SIO hardware
divider (8 cycles + call overhead). libgcc's software routines are therefore
charged a flat 26 cycles per call instead of their emulated cost.

Usage: run_bench.py bench.elf [samples]
"""
import os, subprocess, sys, struct
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_PC
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_MCLASS

ELF = sys.argv[1] if len(sys.argv) > 1 else "bench.elf"
N = int(sys.argv[2]) if len(sys.argv) > 2 else 4800
OBJCOPY = "arm-none-eabi-objcopy"
NM = "arm-none-eabi-nm"
BASE = 0x20000000
STOP = 0x30000000

subprocess.check_call([OBJCOPY, "-O", "binary", ELF, "/tmp/bench.bin"])
img = open("/tmp/bench.bin", "rb").read()
syms = {}
for line in subprocess.check_output([NM, ELF]).decode().splitlines():
    p = line.split()
    if len(p) == 3:
        syms[p[2]] = int(p[0], 16)

DIV_FUNCS = ("__divsi3", "__udivsi3", "__aeabi_idiv", "__aeabi_uidiv", "__aeabi_idivmod", "__aeabi_uidivmod", "__modsi3", "__umodsi3")
div_ranges = []
for line in subprocess.check_output([NM, "-S", ELF]).decode().splitlines():
    p = line.split()
    if len(p) == 4 and p[3] in DIV_FUNCS:
        st = int(p[0], 16) & ~1
        div_ranges.append((st, st + int(p[1], 16)))
DIV_CALL_CYCLES = 26

uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
uc.mem_map(BASE, 4 * 1024 * 1024)
uc.mem_write(BASE, img)
uc.mem_map(STOP, 0x1000)
uc.mem_write(STOP, b"\x00\xbf" * 16)   # nops; we stop on arrival

cs = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
decoded = {}
state = {"cycles": 0, "instr": 0, "prev_addr": None, "prev_size": 0, "prev_branch": None, "on": False}

def cost(insn):
    m = insn.mnemonic.split(".")[0]
    if m in ("ldr", "ldrb", "ldrh", "ldrsb", "ldrsh", "str", "strb", "strh"):
        return 2, None
    if m in ("push", "pop", "ldm", "stm", "ldmia", "stmia"):
        n = insn.op_str.count(",") + 1
        extra = 0
        if m == "pop" and "pc" in insn.op_str:
            extra = 2
        return 1 + n + extra, None
    if m == "bl":
        return 3, None
    if m in ("bx", "blx"):
        return 2, None
    if m == "b" or (m.startswith("b") and len(m) == 3 and m not in ("bic",)) or m in ("cbz", "cbnz"):
        return 1, "cond"
    return 1, None

def hook(uc, addr, size, user):
    st = state
    if not st["on"]:
        return
    for lo, hi in div_ranges:
        if lo <= addr < hi:
            if addr == lo and not st.get("in_div"):
                st["cycles"] += DIV_CALL_CYCLES
                st["in_div"] = True
            st["prev_branch"] = None
            return
    st["in_div"] = False
    # Taken-branch penalty for the previous instruction.
    if st["prev_branch"] and addr != st["prev_addr"] + st["prev_size"]:
        st["cycles"] += 1
    d = decoded.get(addr)
    if d is None:
        code = uc.mem_read(addr, size)
        insn = next(cs.disasm(bytes(code), addr), None)
        d = cost(insn) if insn else (1, None)
        decoded[addr] = d
    st["cycles"] += d[0]
    st["instr"] += 1
    if st.get("prof") is not None:
        st["prof"][addr] += d[0]
    st["prev_addr"], st["prev_size"], st["prev_branch"] = addr, size, d[1]

uc.hook_add(UC_HOOK_CODE, hook)

def call(fn, arg=0, count=False):
    uc.reg_write(UC_ARM_REG_SP, BASE + 4 * 1024 * 1024 - 16)
    uc.reg_write(UC_ARM_REG_LR, STOP | 1)
    uc.reg_write(UC_ARM_REG_R0, arg)
    state["on"] = count
    state["prev_branch"] = None
    uc.emu_start(syms[fn] | 1, STOP)

def run(scenario, n):
    call("bench_init", scenario)
    call("bench_run", 4800)          # warm up: 100 ms, voices sounding
    per = []
    for _ in range(n):
        state["cycles"] = 0
        call("bench_one", 0, True)
        per.append(state["cycles"])
    per.sort()
    avg = sum(per) / len(per)
    return avg, per[len(per) // 2], per[int(len(per) * 0.99)], per[-1]

def main():
    names = {0: "idle (stopped)", 1: "demo song playing", 2: "worst case (4 voices, ratchets every row, all mixer FX)",
             3: "demo song + reverb", 4: "worst case + reverb from all 6 channels (max size/decay, wobble)",
             5: "stopped, reverb tail ringing"}
    budget = 192e6 / 48000
    print(f"samples per scenario: {N}, budget at 192 MHz: {budget:.0f} cycles/sample")
    scs = [int(a) for a in os.environ.get("SCENARIOS", "0,1,2,3,4,5").split(",")]
    for sc in scs:
        avg, med, p99, mx = run(sc, N)
        print(f"{names[sc]:58s} avg {avg:6.0f}  median {med:6.0f}  p99 {p99:6.0f}  max {mx:6.0f}  "
              f"({100*avg/budget:4.1f}% avg, {100*mx/budget:4.1f}% max)")


    if "--profile" in sys.argv:
        import bisect, collections
        funcs = []
        for line in subprocess.check_output([NM, "-S", "-C", ELF]).decode().splitlines():
            p = line.split(None, 3)
            if len(p) == 4 and p[2] in "tTwW":
                funcs.append((int(p[0], 16), int(p[1], 16), p[3]))
        funcs.sort()
        starts = [f[0] for f in funcs]
        for sc in (0, 2):
            call("bench_init", sc)
            call("bench_run", 4800)
            state["prof"] = collections.Counter()
            call("bench_run", 480, True)
            per_fn = collections.Counter()
            for addr, c in state["prof"].items():
                i = bisect.bisect_right(starts, addr) - 1
                name = funcs[i][2] if i >= 0 and addr < funcs[i][0] + max(funcs[i][1], 2) else hex(addr)
                per_fn[name] += c
            state["prof"] = None
            print(f"--- profile scenario {sc} (cycles per sample)")
            for name, c in per_fn.most_common(14):
                print(f"{c/480:8.0f}  {name[:110]}")


if __name__ == "__main__":
    main()
