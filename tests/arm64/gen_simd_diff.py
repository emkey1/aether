#!/usr/bin/env python3
"""Differential SIMD test generator: emits the same test body wrapped for
Linux (bare _start + svc, run under ish) and macOS (libc main, run natively
on the same Apple silicon = ground-truth oracle). Each (op, arrangement)
loads fixed inputs into V registers with movk chains (no relocations, so
one body assembles for both targets), executes one instruction, and stores
the 16-byte result to a stack buffer; the buffer is written to stdout at
the end. Byte-diff of native vs ish output localizes any bug to
offset/16 = test index (see the index dump the generator prints).

Usage: gen_simd_diff.py <family>... ; writes diff_<families>_{linux,macos}.s
"""
import sys

# Fixed inputs: bit patterns exercising sign/saturation/carry corners.
INT_A = (0xC3A55A3C80FF7F01, 0x0123456789ABCDEF)
INT_B = (0x00E1D2B37F80FF40, 0xFEDCBA9876543210)
INT_C = (0x1234567855AA55AA, 0x9ABCDEF011223344)
# FP inputs: f32 lanes {1.5, -2.25, 1e10, -0.5} / {3.0, -1e-10, +inf, 7.75},
# also reinterpreted as f64 for the .2d/.1d forms (finite-ish garbage is fine
# for a same-silicon diff). Accumulator: {0.5, 100.0, -3.0, 0.25}.
FP_A = (0xC01000003FC00000, 0xBF0000005014E718)
FP_B = (0xAEDBE6FF40400000, 0x40F800007F800000)
FP_C = (0x42C800003F000000, 0x3E800000C0400000)

def load_v(reg, pair):
    lo, hi = pair
    out = []
    for xr, val in (("x0", lo), ("x1", hi)):
        out.append(f"    movz {xr}, #0x{val & 0xffff:04x}")
        for sh in (16, 32, 48):
            out.append(f"    movk {xr}, #0x{(val >> sh) & 0xffff:04x}, lsl #{sh}")
    out.append("    stp x0, x1, [sp, #-16]!")
    out.append(f"    ldr q{reg}, [sp], #16")
    return out

ARR_D   = ["8b", "16b", "4h", "8h", "2s", "4s", "2d"]
ARR_NOD = ["8b", "16b", "4h", "8h", "2s", "4s"]
ARR_HS  = ["4h", "8h", "2s", "4s"]
ARR_B   = ["8b", "16b"]
ARR_FP  = ["2s", "4s", "2d"]

def three_same(op, arrs, acc=False, fp=False):
    a, b, c = (FP_A, FP_B, FP_C) if fp else (INT_A, INT_B, INT_C)
    tests = []
    for arr in arrs:
        # v0 is always preloaded: for accumulating ops it's the live
        # accumulator; otherwise it catches a gadget failing to write Vd.
        body = load_v(0, c if acc else a) + load_v(1, a) + load_v(2, b)
        body.append(f"    {op} v0.{arr}, v1.{arr}, v2.{arr}")
        body.append("    str q0, [x19], #16")
        tests.append((f"{op}.{arr}", body))
    return tests

def scalar(op, kinds, fp=False):
    a, b = (FP_A, FP_B) if fp else (INT_A, INT_B)
    tests = []
    for k in kinds:
        body = load_v(1, a) + load_v(2, b)
        body.append(f"    {op} {k}0, {k}1, {k}2")
        body.append("    str q0, [x19], #16")
        tests.append((f"{op}.{k}", body))
    return tests

FAMILIES = {}

FAMILIES["three_same"] = (
    [t for op, arrs in [
        ("add", ARR_D), ("sub", ARR_D), ("cmeq", ARR_D), ("cmtst", ARR_D),
        ("cmgt", ARR_D), ("cmge", ARR_D), ("cmhi", ARR_D), ("cmhs", ARR_D),
        ("sqadd", ARR_D), ("uqadd", ARR_D), ("sqsub", ARR_D), ("uqsub", ARR_D),
        ("sshl", ARR_D), ("ushl", ARR_D), ("srshl", ARR_D), ("urshl", ARR_D),
        ("sqshl", ARR_D), ("uqshl", ARR_D), ("sqrshl", ARR_D), ("uqrshl", ARR_D),
        ("addp", ARR_D),
        ("shadd", ARR_NOD), ("uhadd", ARR_NOD), ("srhadd", ARR_NOD),
        ("urhadd", ARR_NOD), ("shsub", ARR_NOD), ("uhsub", ARR_NOD),
        ("smax", ARR_NOD), ("smin", ARR_NOD), ("umax", ARR_NOD), ("umin", ARR_NOD),
        ("sabd", ARR_NOD), ("uabd", ARR_NOD), ("mul", ARR_NOD),
        ("smaxp", ARR_NOD), ("sminp", ARR_NOD), ("umaxp", ARR_NOD), ("uminp", ARR_NOD),
        ("sqdmulh", ARR_HS), ("sqrdmulh", ARR_HS), ("pmul", ARR_B),
    ] for t in three_same(op, arrs)]
    + [t for op in ["mla", "mls", "saba", "uaba"] for t in three_same(op, ARR_NOD, acc=True)]
    + [t for op, arrs in [
        ("fadd", ARR_FP), ("fsub", ARR_FP), ("fmul", ARR_FP), ("fdiv", ARR_FP),
        ("fmulx", ARR_FP), ("fmax", ARR_FP), ("fmin", ARR_FP),
        ("fmaxnm", ARR_FP), ("fminnm", ARR_FP), ("faddp", ARR_FP),
        ("fmaxp", ARR_FP), ("fminp", ARR_FP), ("fmaxnmp", ARR_FP), ("fminnmp", ARR_FP),
        ("frecps", ARR_FP), ("frsqrts", ARR_FP), ("fabd", ARR_FP),
        ("fcmeq", ARR_FP), ("fcmge", ARR_FP), ("fcmgt", ARR_FP),
        ("facge", ARR_FP), ("facgt", ARR_FP),
    ] for t in three_same(op, arrs, fp=True)]
    + [t for op in ["fmla", "fmls"] for t in three_same(op, ARR_FP, acc=True, fp=True)]
    + [t for op, kinds in [
        ("add", "d"), ("sub", "d"), ("cmeq", "d"), ("cmtst", "d"),
        ("cmgt", "d"), ("cmge", "d"), ("cmhi", "d"), ("cmhs", "d"),
        ("sshl", "d"), ("ushl", "d"), ("srshl", "d"), ("urshl", "d"),
        ("sqadd", "bhsd"), ("uqadd", "bhsd"), ("sqsub", "bhsd"), ("uqsub", "bhsd"),
        ("sqshl", "bhsd"), ("uqshl", "bhsd"), ("sqrshl", "bhsd"), ("uqrshl", "bhsd"),
        ("sqdmulh", "hs"), ("sqrdmulh", "hs"),
    ] for t in scalar(op, kinds)]
    + [t for op, kinds in [
        ("fabd", "sd"), ("fmulx", "sd"), ("frecps", "sd"), ("frsqrts", "sd"),
        ("fcmeq", "sd"), ("fcmge", "sd"), ("fcmgt", "sd"),
        ("facge", "sd"), ("facgt", "sd"),
    ] for t in scalar(op, kinds, fp=True)]
)

LINUX_PROLOGUE = """\
.text
.global _start
_start:
    sub sp, sp, #16384
    mov x19, sp
    mov x20, sp
"""
LINUX_EPILOGUE = """\
    mov x0, #1
    mov x1, x20
    sub x2, x19, x20
    mov x8, #64          // write
    svc #0
    mov x0, #0
    mov x8, #93          // exit
    svc #0
"""
MACOS_PROLOGUE = """\
.text
.global _main
.p2align 2
_main:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #16384
    mov x19, sp
    mov x20, sp
"""
MACOS_EPILOGUE = """\
    mov x0, #1
    mov x1, x20
    sub x2, x19, x20
    bl _write
    mov sp, x29
    ldp x29, x30, [sp], #16
    mov x0, #0
    ret
"""

def emit(tests, prologue, epilogue, path):
    with open(path, "w") as f:
        f.write(prologue)
        for i, (name, body) in enumerate(tests):
            f.write(f"// [{i}] {name} @ offset {i*16}\n")
            f.write("\n".join(body) + "\n")
        f.write(epilogue)

def main():
    fams = sys.argv[1:] or ["three_same"]
    tests = [t for f in fams for t in FAMILIES[f]]
    tag = "_".join(fams)
    emit(tests, LINUX_PROLOGUE, LINUX_EPILOGUE, f"diff_{tag}_linux.s")
    emit(tests, MACOS_PROLOGUE, MACOS_EPILOGUE, f"diff_{tag}_macos.s")
    with open(f"diff_{tag}_index.txt", "w") as f:
        for i, (name, _) in enumerate(tests):
            f.write(f"{i*16}\t{name}\n")
    print(f"{len(tests)} tests -> diff_{tag}_{{linux,macos}}.s")

if __name__ == "__main__":
    main()
