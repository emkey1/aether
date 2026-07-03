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

def load_x01(pair):
    lo, hi = pair
    out = []
    for xr, val in (("x0", lo), ("x1", hi)):
        out.append(f"    movz {xr}, #0x{val & 0xffff:04x}")
        for sh in (16, 32, 48):
            out.append(f"    movk {xr}, #0x{(val >> sh) & 0xffff:04x}, lsl #{sh}")
    return out

def load_v(reg, pair):
    out = load_x01(pair)
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

def raw_tests(items, fp=False, acc=False):
    """items: (name, instruction) pairs. v0 = dest (preloaded), v1/v2 = sources."""
    a, b, c = (FP_A, FP_B, FP_C) if fp else (INT_A, INT_B, INT_C)
    tests = []
    for name, ins in items:
        body = load_v(0, c if acc else a) + load_v(1, a) + load_v(2, b)
        body.append(f"    {ins}")
        body.append("    str q0, [x19], #16")
        tests.append((name, body))
    return tests

def unary(op, arrs, fp=False, zero=None):
    suffix = f", {zero}" if zero else ""
    return raw_tests([(f"{op}.{arr}", f"{op} v0.{arr}, v1.{arr}{suffix}") for arr in arrs], fp=fp)

WIDEN = [("8h","8b","16b"), ("4s","4h","8h"), ("2d","2s","4s")]
NARROW = [("8b","16b","8h"), ("4h","8h","4s"), ("2s","4s","2d")]

FAMILIES["two_misc"] = (
    # integer unary
    unary("abs", ARR_D) + unary("neg", ARR_D)
    + unary("sqabs", ARR_D) + unary("sqneg", ARR_D)
    + unary("cls", ARR_NOD) + unary("clz", ARR_NOD)
    + unary("rev64", ARR_NOD) + unary("rev32", ["8b","16b","4h","8h"])
    + unary("rev16", ARR_B) + unary("cnt", ARR_B)
    + unary("not", ARR_B) + unary("rbit", ARR_B)
    + unary("cmeq", ARR_D, zero="#0") + unary("cmgt", ARR_D, zero="#0")
    + unary("cmge", ARR_D, zero="#0") + unary("cmle", ARR_D, zero="#0")
    + unary("cmlt", ARR_D, zero="#0")
    # suqadd/usqadd accumulate into v0
    + raw_tests([(f"suqadd.{arr}", f"suqadd v0.{arr}, v1.{arr}") for arr in ARR_D], acc=True)
    + raw_tests([(f"usqadd.{arr}", f"usqadd v0.{arr}, v1.{arr}") for arr in ARR_D], acc=True)
    # narrows (2-forms merge into preloaded v0)
    + [t for op in ["xtn","sqxtn","uqxtn","sqxtun"] for t in
       raw_tests([(f"{op}.{n}", f"{op} v0.{n}, v1.{w}") for n,_,w in NARROW]
                 + [(f"{op}2.{w2}", f"{op}2 v0.{w2}, v1.{w}") for _,w2,w in NARROW],
                 acc=True)]
    # shll
    + raw_tests([("shll.8h","shll v0.8h, v1.8b, #8"), ("shll2.8h","shll2 v0.8h, v1.16b, #8"),
                 ("shll.4s","shll v0.4s, v1.4h, #16"), ("shll2.4s","shll2 v0.4s, v1.8h, #16"),
                 ("shll.2d","shll v0.2d, v1.2s, #32"), ("shll2.2d","shll2 v0.2d, v1.4s, #32")])
    # pairwise long (+acc)
    + raw_tests([(f"saddlp","saddlp v0.4h, v1.8b"),("saddlp2","saddlp v0.8h, v1.16b"),
                 ("saddlp3","saddlp v0.2s, v1.4h"),("saddlp4","saddlp v0.4s, v1.8h"),
                 ("saddlp5","saddlp v0.1d, v1.2s"),("saddlp6","saddlp v0.2d, v1.4s"),
                 ("uaddlp","uaddlp v0.4h, v1.8b"),("uaddlp2","uaddlp v0.4s, v1.8h"),
                 ("uaddlp3","uaddlp v0.2d, v1.4s")])
    + raw_tests([("sadalp","sadalp v0.4h, v1.8b"),("sadalp2","sadalp v0.4s, v1.8h"),
                 ("sadalp3","sadalp v0.2d, v1.4s"),("uadalp","uadalp v0.8h, v1.16b"),
                 ("uadalp2","uadalp v0.2s, v1.4h")], acc=True)
    # urecpe/ursqrte
    + raw_tests([("urecpe.2s","urecpe v0.2s, v1.2s"),("urecpe.4s","urecpe v0.4s, v1.4s"),
                 ("ursqrte.2s","ursqrte v0.2s, v1.2s"),("ursqrte.4s","ursqrte v0.4s, v1.4s")])
    # FP unary/convert/round
    + [t for op in ["fabs","fneg","fsqrt","frecpe","frsqrte","scvtf","ucvtf",
                    "fcvtzs","fcvtzu","fcvtns","fcvtnu","fcvtms","fcvtmu",
                    "fcvtps","fcvtpu","fcvtas","fcvtau","frintn","frinta",
                    "frintp","frintm","frintx","frintz","frinti"]
       for t in unary(op, ARR_FP, fp=True)]
    + [t for op in ["fcmeq","fcmge","fcmgt","fcmle","fcmlt"]
       for t in unary(op, ARR_FP, fp=True, zero="#0.0")]
    # fcvtn/fcvtl/fcvtxn
    + raw_tests([("fcvtn","fcvtn v0.2s, v1.2d"),("fcvtxn","fcvtxn v0.2s, v1.2d"),
                 ("fcvtl","fcvtl v0.2d, v1.2s")], fp=True)
    + raw_tests([("fcvtn2","fcvtn2 v0.4s, v1.2d"),("fcvtxn2","fcvtxn2 v0.4s, v1.2d"),
                 ("fcvtl2","fcvtl2 v0.2d, v1.4s")], fp=True, acc=True)
    # across-lanes
    + raw_tests([(f"{op}.{d}_{s}", f"{op} {d}0, v1.{s}")
                 for op, pairs in [
                     ("addv",   [("b","8b"),("b","16b"),("h","4h"),("h","8h"),("s","4s")]),
                     ("smaxv",  [("b","8b"),("b","16b"),("h","4h"),("h","8h"),("s","4s")]),
                     ("sminv",  [("b","16b"),("h","8h"),("s","4s")]),
                     ("umaxv",  [("b","16b"),("h","8h"),("s","4s")]),
                     ("uminv",  [("b","8b"),("h","4h"),("s","4s")]),
                     ("saddlv", [("h","8b"),("h","16b"),("s","4h"),("s","8h"),("d","4s")]),
                     ("uaddlv", [("h","16b"),("s","8h"),("d","4s")]),
                 ] for d, s in pairs])
    + raw_tests([("fmaxv","fmaxv s0, v1.4s"),("fminv","fminv s0, v1.4s"),
                 ("fmaxnmv","fmaxnmv s0, v1.4s"),("fminnmv","fminnmv s0, v1.4s")], fp=True)
    # permute
    + [t for op in ["uzp1","uzp2","trn1","trn2","zip1","zip2"]
       for t in three_same(op, ARR_D)]
    # scalar two-misc
    + raw_tests([("abs.d","abs d0, d1"),("neg.d","neg d0, d1"),
                 ("cmeq0.d","cmeq d0, d1, #0"),("cmgt0.d","cmgt d0, d1, #0"),
                 ("cmge0.d","cmge d0, d1, #0"),("cmle0.d","cmle d0, d1, #0"),
                 ("cmlt0.d","cmlt d0, d1, #0")]
                + [(f"sqabs.{k}",f"sqabs {k}0, {k}1") for k in "bhsd"]
                + [(f"sqneg.{k}",f"sqneg {k}0, {k}1") for k in "bhsd"]
                + [(f"sqxtn.{k}",f"sqxtn {k[0]}0, {k[1]}1") for k in ["bh","hs","sd"]]
                + [(f"uqxtn.{k}",f"uqxtn {k[0]}0, {k[1]}1") for k in ["bh","hs","sd"]]
                + [(f"sqxtun.{k}",f"sqxtun {k[0]}0, {k[1]}1") for k in ["bh","hs","sd"]])
    + raw_tests([(f"suqadd.s{k}",f"suqadd {k}0, {k}1") for k in "bhsd"]
                + [(f"usqadd.s{k}",f"usqadd {k}0, {k}1") for k in "bhsd"], acc=True)
    + raw_tests([("frecpe.s","frecpe s0, s1"),("frecpe.d","frecpe d0, d1"),
                 ("frsqrte.s","frsqrte s0, s1"),("frsqrte.d","frsqrte d0, d1"),
                 ("frecpx.s","frecpx s0, s1"),("frecpx.d","frecpx d0, d1"),
                 ("sscvtf.s","scvtf s0, s1"),("sscvtf.d","scvtf d0, d1"),
                 ("sucvtf.s","ucvtf s0, s1"),("sucvtf.d","ucvtf d0, d1"),
                 ("sfcvtzs.s","fcvtzs s0, s1"),("sfcvtzs.d","fcvtzs d0, d1"),
                 ("sfcvtzu.s","fcvtzu s0, s1"),("sfcvtzu.d","fcvtzu d0, d1"),
                 ("sfcvtns.s","fcvtns s0, s1"),("sfcvtnu.d","fcvtnu d0, d1"),
                 ("sfcvtms.s","fcvtms s0, s1"),("sfcvtmu.d","fcvtmu d0, d1"),
                 ("sfcvtps.s","fcvtps s0, s1"),("sfcvtpu.d","fcvtpu d0, d1"),
                 ("sfcvtas.s","fcvtas s0, s1"),("sfcvtau.d","fcvtau d0, d1"),
                 ("sfcvtxn","fcvtxn s0, d1"),
                 ("sfcmeq0.s","fcmeq s0, s1, #0.0"),("sfcmeq0.d","fcmeq d0, d1, #0.0"),
                 ("sfcmge0.s","fcmge s0, s1, #0.0"),("sfcmgt0.d","fcmgt d0, d1, #0.0"),
                 ("sfcmle0.s","fcmle s0, s1, #0.0"),("sfcmlt0.d","fcmlt d0, d1, #0.0"),
                 ("sfaddp.s","faddp s0, v1.2s"),("sfaddp.d","faddp d0, v1.2d"),
                 ("sfmaxp.s","fmaxp s0, v1.2s"),("sfmaxp.d","fmaxp d0, v1.2d"),
                 ("sfminp.s","fminp s0, v1.2s"),("sfminp.d","fminp d0, v1.2d"),
                 ("sfmaxnmp.s","fmaxnmp s0, v1.2s"),("sfmaxnmp.d","fmaxnmp d0, v1.2d"),
                 ("sfminnmp.s","fminnmp s0, v1.2s"),("sfminnmp.d","fminnmp d0, v1.2d"),
                 ("saddp.d","addp d0, v1.2d")], fp=True)
)

FAMILIES["three_diff"] = (
    # L-forms
    [t for op in ["saddl","uaddl","ssubl","usubl","sabdl","uabdl","smull","umull"]
     for t in raw_tests([(f"{op}.{w}", f"{op} v0.{w}, v1.{n}, v2.{n}") for w,n,_ in WIDEN]
                        + [(f"{op}2.{w}", f"{op}2 v0.{w}, v1.{n2}, v2.{n2}") for w,_,n2 in WIDEN])]
    # accumulating L-forms
    + [t for op in ["sabal","uabal","smlal","umlal","smlsl","umlsl"]
       for t in raw_tests([(f"{op}.{w}", f"{op} v0.{w}, v1.{n}, v2.{n}") for w,n,_ in WIDEN]
                          + [(f"{op}2.{w}", f"{op}2 v0.{w}, v1.{n2}, v2.{n2}") for w,_,n2 in WIDEN], acc=True)]
    # saturating doubling (h/s only)
    + raw_tests([("sqdmull.4s","sqdmull v0.4s, v1.4h, v2.4h"),
                 ("sqdmull2.4s","sqdmull2 v0.4s, v1.8h, v2.8h"),
                 ("sqdmull.2d","sqdmull v0.2d, v1.2s, v2.2s"),
                 ("sqdmull2.2d","sqdmull2 v0.2d, v1.4s, v2.4s")])
    + raw_tests([("sqdmlal.4s","sqdmlal v0.4s, v1.4h, v2.4h"),
                 ("sqdmlal2.2d","sqdmlal2 v0.2d, v1.4s, v2.4s"),
                 ("sqdmlsl.4s","sqdmlsl v0.4s, v1.4h, v2.4h"),
                 ("sqdmlsl2.2d","sqdmlsl2 v0.2d, v1.4s, v2.4s")], acc=True)
    # W-forms
    + [t for op in ["saddw","uaddw","ssubw","usubw"]
       for t in raw_tests([(f"{op}.{w}", f"{op} v0.{w}, v1.{w}, v2.{n}") for w,n,_ in WIDEN]
                          + [(f"{op}2.{w}", f"{op}2 v0.{w}, v1.{w}, v2.{n2}") for w,_,n2 in WIDEN])]
    # narrow-high forms
    + [t for op in ["addhn","raddhn","subhn","rsubhn"]
       for t in raw_tests([(f"{op}.{n}", f"{op} v0.{n}, v1.{w}, v2.{w}") for n,_,w in
                           [("8b","x","8h"),("4h","x","4s"),("2s","x","2d")]]
                          + [(f"{op}2.{n2}", f"{op}2 v0.{n2}, v1.{w}, v2.{w}") for n2,w in
                             [("16b","8h"),("8h","4s"),("4s","2d")]], acc=True)]
    # pmull
    + raw_tests([("pmull.8h","pmull v0.8h, v1.8b, v2.8b"),
                 ("pmull2.8h","pmull2 v0.8h, v1.16b, v2.16b"),
                 ("pmull.1q","pmull v0.1q, v1.1d, v2.1d"),
                 ("pmull2.1q","pmull2 v0.1q, v1.2d, v2.2d")])
)

def mem_tests(items):
    """Copy/memory ops. Convention per test: x9 = 64-byte scratch seeded
    with A,B,C,B; v0-v3 preloaded with A,B,C,B; x10 free (addressing).
    Each item's lines do their own result stores via [x19], #16 chunks."""
    tests = []
    for name, lines in items:
        body = ["    add x9, x20, #126976"]
        for i, pair in enumerate([INT_A, INT_B, INT_C, INT_B]):
            body += load_x01(pair) + [f"    stp x0, x1, [x9, #{16*i}]"]
        for r, pair in enumerate([INT_A, INT_B, INT_C, INT_B]):
            body += load_v(r, pair)
        # defined GPR sources for store tests (x2..x4)
        for reg, val in (("x2", INT_A[0]), ("x3", INT_B[1]), ("x4", INT_C[0])):
            body.append(f"    movz {reg}, #0x{val & 0xffff:04x}")
            for sh in (16, 32, 48):
                body.append(f"    movk {reg}, #0x{(val >> sh) & 0xffff:04x}, lsl #{sh}")
        body += [f"    {l}" for l in lines]
        tests.append((name, body))
    return tests

def _dump_scratch():
    return [f"ldr q6, [x9, #{o}]\n    str q6, [x19], #16" for o in (0, 16, 32, 48)]

FAMILIES["copy_mem"] = mem_tests(
    # LD2/3/4 multiple (de-interleave)
    [(f"ld{n}.{arr}", [f"ld{n} {{{', '.join(f'v{i}.{arr}' for i in range(n))}}}, [x9]"]
      + [f"str q{i}, [x19], #16" for i in range(n)])
     for n in (2, 3, 4) for arr in ("16b", "8b", "8h", "4s", "2d", "4h", "2s")]
    # ST2/3/4 multiple (interleave): dump scratch after
    + [(f"st{n}.{arr}", [f"st{n} {{{', '.join(f'v{i}.{arr}' for i in range(n))}}}, [x9]"]
        + _dump_scratch())
       for n in (2, 3, 4) for arr in ("16b", "8h", "4s", "2d", "8b", "4h", "2s")]
    # post-index writeback (immediate and register)
    + [("ld2.post", ["mov x10, x9", "ld2 {v0.16b, v1.16b}, [x10], #32",
                     "str q0, [x19], #16", "str q1, [x19], #16",
                     "sub x10, x10, x9", "stp x10, xzr, [x19], #16"]),
       ("ld3.postreg", ["mov x10, x9", "mov x11, #24",
                        "ld3 {v0.8b, v1.8b, v2.8b}, [x10], x11",
                        "str q0, [x19], #16", "str q1, [x19], #16", "str q2, [x19], #16",
                        "sub x10, x10, x9", "stp x10, xzr, [x19], #16"]),
       ("st4.post", ["mov x10, x9", "st4 {v0.4h, v1.4h, v2.4h, v3.4h}, [x10], #32",
                     "sub x10, x10, x9", "stp x10, xzr, [x19], #16"] + _dump_scratch())]
    # LD1R-LD4R replicate
    + [(f"ld{n}r.{arr}", [f"ld{n}r {{{', '.join(f'v{i}.{arr}' for i in range(n))}}}, [x9]"]
        + [f"str q{i}, [x19], #16" for i in range(n)])
       for n in (1, 2, 3, 4) for arr in ("16b", "8h", "4s", "2d", "8b", "2s")]
    + [("ld1r.post", ["mov x10, x9", "ld1r {v0.4s}, [x10], #4",
                      "str q0, [x19], #16", "sub x10, x10, x9", "stp x10, xzr, [x19], #16"])]
    # single-lane loads/stores (dest lanes preserved on load)
    + [(f"ld1lane.{k}{i}", [f"ld1 {{v0.{k}}}[{i}], [x9]", "str q0, [x19], #16"])
       for k, idxs in (("b", (0, 7, 15)), ("h", (0, 3, 7)), ("s", (1, 3)), ("d", (0, 1)))
       for i in idxs]
    + [(f"st1lane.{k}{i}", [f"st1 {{v0.{k}}}[{i}], [x9]"] + _dump_scratch())
       for k, i in (("b", 11), ("h", 5), ("s", 2), ("d", 1))]
    + [("ld2lane", ["ld2 {v0.s, v1.s}[2], [x9]", "str q0, [x19], #16", "str q1, [x19], #16"]),
       ("ld4lane", ["ld4 {v0.b, v1.b, v2.b, v3.b}[9], [x9]"]
        + [f"str q{i}, [x19], #16" for i in range(4)]),
       ("st3lane", ["st3 {v0.h, v1.h, v2.h}[6], [x9]"] + _dump_scratch()),
       ("ld1lane.post", ["mov x10, x9", "ld1 {v0.h}[5], [x10], #2", "str q0, [x19], #16",
                         "sub x10, x10, x9", "stp x10, xzr, [x19], #16"])]
    # TBL/TBX
    + [(f"tbl{n}.{arr}", [f"tbl v4.{arr}, {{{', '.join(f'v{i}.16b' for i in range(n))}}}, v3.{arr}",
                          "str q4, [x19], #16"])
       for n in (1, 2, 3, 4) for arr in ("8b", "16b")]
    + [(f"tbx{n}.{arr}", ["mov v4.16b, v2.16b",
                          f"tbx v4.{arr}, {{{', '.join(f'v{i}.16b' for i in range(n))}}}, v3.{arr}",
                          "str q4, [x19], #16"])
       for n in (1, 2, 4) for arr in ("8b", "16b")]
    # EXT
    + [(f"ext.16b.{i}", [f"ext v4.16b, v0.16b, v1.16b, #{i}", "str q4, [x19], #16"])
       for i in (0, 1, 7, 8, 15)]
    + [(f"ext.8b.{i}", [f"ext v4.8b, v0.8b, v1.8b, #{i}", "str q4, [x19], #16"])
       for i in (0, 3, 7)]
    # INS element->element, DUP element
    + [(f"ins.{k}", [f"ins v0.{k[0]}[{k[1]}], v1.{k[0]}[{k[2]}]", "str q0, [x19], #16"])
       for k in (("b", 15, 3), ("b", 0, 12), ("h", 7, 2), ("s", 3, 1), ("d", 1, 0))]
    + [(f"dupe.{arr}.{i}", [f"dup v4.{arr}, v1.{k}[{i}]", "str q4, [x19], #16"])
       for arr, k, i in (("16b", "b", 9), ("8b", "b", 3), ("8h", "h", 6), ("4h", "h", 1),
                         ("4s", "s", 2), ("2s", "s", 3), ("2d", "d", 1))]
    + [(f"dups.{k}{i}", [f"mov {k}4, v1.{k}[{i}]", "str q4, [x19], #16"])
       for k, i in (("b", 13), ("h", 5), ("s", 3), ("d", 1))]
)

GPR_VALS = [0x00007ffffd8a8ae8, 0xffffffff80000001, 0x0000000123456789,
            0x8000000000000000, 0x00000000fffffffe, 0x7fffffffffffffff]

def gpr_tests(items):
    """GPR data-processing. x2/x3 = two input values (all pairs of
    GPR_VALS), x4 = a third. Each test stores x0 (and flags via x1)."""
    tests = []
    for name, lines in items:
        for i, a in enumerate(GPR_VALS):
            for j in (0, 2, 4):
                b = GPR_VALS[(i + 1 + j) % len(GPR_VALS)]
                c = GPR_VALS[(i + 3) % len(GPR_VALS)]
                body = []
                for reg, val in (("x2", a), ("x3", b), ("x4", c)):
                    body.append(f"    movz {reg}, #0x{val & 0xffff:04x}")
                    for sh in (16, 32, 48):
                        body.append(f"    movk {reg}, #0x{(val >> sh) & 0xffff:04x}, lsl #{sh}")
                body.append("    mov x0, #0")
                body.append("    msr nzcv, xzr")
                body += [f"    {l}" for l in lines]
                body.append("    mrs x1, nzcv")
                body.append("    stp x0, x1, [x19], #16")
                tests.append((f"{name}[{i},{j}]", body))
    return tests

FAMILIES["gpr"] = gpr_tests(
    # add/sub extended register, every extend x a couple of shifts
    [(f"{op}.{ext}.{sh}", [f"{op} x0, x2, w3, {ext} #{sh}"])
     for op in ("add", "sub", "adds", "subs")
     for ext in ("uxtb", "uxth", "uxtw", "sxtb", "sxth", "sxtw")
     for sh in (0, 2)]
    + [(f"{op}.{ext}", [f"{op} x0, x2, x3, {ext} #3"])
       for op in ("add", "sub") for ext in ("uxtx", "sxtx")]
    # shifted register
    + [(f"{op}.{sh}.{amt}", [f"{op} x0, x2, x3, {sh} #{amt}"])
       for op in ("add", "sub", "adds", "subs", "and", "orr", "eor", "bic",
                  "orn", "eon", "ands", "bics")
       for sh in ("lsl", "lsr", "asr") for amt in (0, 13, 47)]
    + [(f"{op}.ror", [f"{op} x0, x2, x3, ror #21"])
       for op in ("and", "orr", "eor", "ands")]
    # 32-bit forms (upper-zeroing behavior)
    + [(f"{op}.w", [f"{op} w0, w2, w3"]) for op in
       ("add", "sub", "adds", "subs", "and", "orr", "eor", "ands")]
    # bitfield / extract
    + [(f"{op}.{immr}.{imms}", [f"{op} x0, x2, #{immr}, #{imms}"])
       for op in ("sbfm", "ubfm") for immr, imms in
       ((0, 7), (12, 51), (48, 15), (63, 63), (5, 4))]
    + [("bfm", ["mov x0, x4", "bfm x0, x2, #16, #47"]),
       ("bfi", ["mov x0, x4", "bfi x0, x2, #40, #16"]),
       ("bfxil", ["mov x0, x4", "bfxil x0, x2, #8, #24"]),
       ("extr", ["extr x0, x2, x3, #24"]),
       ("extr.w", ["extr w0, w2, w3, #9"])]
    # multiply family
    + [("madd", ["madd x0, x2, x3, x4"]), ("msub", ["msub x0, x2, x3, x4"]),
       ("smaddl", ["smaddl x0, w2, w3, x4"]), ("umaddl", ["umaddl x0, w2, w3, x4"]),
       ("smsubl", ["smsubl x0, w2, w3, x4"]), ("umsubl", ["umsubl x0, w2, w3, x4"]),
       ("smulh", ["smulh x0, x2, x3"]), ("umulh", ["umulh x0, x2, x3"]),
       ("mul.w", ["mul w0, w2, w3"])]
    # divide
    + [("udiv", ["udiv x0, x2, x3"]), ("sdiv", ["sdiv x0, x2, x3"]),
       ("udiv.w", ["udiv w0, w2, w3"]), ("sdiv.w", ["sdiv w0, w2, w3"]),
       ("sdiv.min", ["mov x3, #-1", "sdiv x0, x2, x3"]),
       ("udiv.z", ["mov x3, #0", "udiv x0, x2, x3"])]
    # adc/sbc with both carry states
    + [("adc.c0", ["adc x0, x2, x3"]), ("adcs.c0", ["adcs x0, x2, x3"]),
       ("adc.c1", ["cmp xzr, xzr", "adc x0, x2, x3"]),
       ("sbc.c0", ["sbc x0, x2, x3"]), ("sbcs.c1", ["cmp xzr, xzr", "sbcs x0, x2, x3"])]
    # conditional select family under various flag states
    + [(f"{op}.{cond}", [f"cmp x2, x3", f"{op} x0, x2, x3, {cond}"])
       for op in ("csel", "csinc", "csinv", "csneg")
       for cond in ("eq", "lt", "hi", "mi")]
    # ccmp/ccmn (imm and reg)
    + [("ccmp.r", ["cmp x2, x3", "ccmp x3, x4, #5, lt"]),
       ("ccmp.i", ["cmp x2, x3", "ccmp x2, #17, #10, ge"]),
       ("ccmn.r", ["cmp x2, x3", "ccmn x3, x4, #3, ne"])]
    # unary DP
    + [("clz", ["clz x0, x2"]), ("cls", ["cls x0, x2"]), ("rbit", ["rbit x0, x2"]),
       ("rev", ["rev x0, x2"]), ("rev32", ["rev32 x0, x2"]), ("rev16", ["rev16 x0, x2"]),
       ("clz.w", ["clz w0, w2"]), ("rev.w", ["rev w0, w2"]), ("rev16.w", ["rev16 w0, w2"])]
    # shifts by register
    + [(f"{op}v", [f"{op} x0, x2, x3"]) for op in ("lsl", "lsr", "asr", "ror")]
    + [(f"{op}v.w", [f"{op} w0, w2, w3"]) for op in ("lsl", "lsr", "asr", "ror")]
    # flag-materialization patterns (cset/csetm/cinc after cmp)
    + [("cset", ["cmp x2, x3", "cset x0, gt"]), ("csetm", ["cmp x2, x3", "csetm x0, ls"]),
       ("cmn.flags", ["cmn x2, x3"]), ("tst.flags", ["tst x2, x3"]),
       ("ngc", ["ngc x0, x2"]), ("ngcs", ["ngcs x0, x2"])]
)

FAMILIES["addr"] = mem_tests(
    # register-offset addressing: all extend forms, positive AND negative
    # 32-bit offsets (sxtw sign-extension is the classic bug site)
    [(f"ldr.{ext}.{amt}.{val:#x}",
      [f"movz x11, #0x{val & 0xffff:04x}" if val >= 0 else f"movn x11, #0x{(~val) & 0xffff:04x}",
       f"add x10, x9, #{off}",
       f"ldr x0, [x10, w11, {ext} #{amt}]" if ext in ("uxtw", "sxtw")
       else f"ldr x0, [x10, x11, {ext} #{amt}]",
       "stp x0, xzr, [x19], #16"])
     for ext, amt, val, off in (
         ("uxtw", 0, 8, 0), ("uxtw", 3, 3, 0),
         ("sxtw", 0, -16, 48), ("sxtw", 3, -2, 32),
         ("lsl", 0, 16, 0), ("lsl", 3, 2, 8), ("sxtx", 0, -8, 24))]
    # byte/half/word/signed variants through register-offset
    + [("ldrb.sxtw", ["movn w11, #0", "add x10, x9, #9",
                      "ldrb w0, [x10, w11, sxtw]", "stp x0, xzr, [x19], #16"]),
       ("ldrsb.off", ["mov x11, #5", "ldrsb x0, [x9, x11]", "stp x0, xzr, [x19], #16"]),
       ("ldrh.lsl", ["mov x11, #3", "ldrh w0, [x9, x11, lsl #1]", "stp x0, xzr, [x19], #16"]),
       ("ldrsh.sxtw", ["movn w11, #1", "add x10, x9, #12",
                       "ldrsh x0, [x10, w11, sxtw #1]", "stp x0, xzr, [x19], #16"]),
       ("ldrsw.off", ["mov x11, #4", "ldrsw x0, [x9, x11, lsl #2]", "stp x0, xzr, [x19], #16"]),
       ("ldr.w.uxtw", ["mov w11, #12", "ldr w0, [x9, w11, uxtw]", "stp x0, xzr, [x19], #16"])]
    # unscaled (ldur/stur) with negative offsets
    + [("ldur.neg", ["add x10, x9, #32", "ldur x0, [x10, #-24]", "stp x0, xzr, [x19], #16"]),
       ("ldur.pos", ["ldur x0, [x9, #17]", "stp x0, xzr, [x19], #16"]),
       ("stur.neg", ["add x10, x9, #48", "stur x2, [x10, #-40]"] + _dump_scratch()),
       ("ldurb", ["add x10, x9, #8", "ldurb w0, [x10, #-3]", "stp x0, xzr, [x19], #16"]),
       ("ldursw", ["add x10, x9, #16", "ldursw x0, [x10, #-12]", "stp x0, xzr, [x19], #16"])]
    # pre/post-index writeback: check BOTH the data and the updated base
    + [("ldr.post", ["mov x10, x9", "ldr x0, [x10], #24",
                     "sub x10, x10, x9", "stp x0, x10, [x19], #16"]),
       ("ldr.pre", ["mov x10, x9", "ldr x0, [x10, #16]!",
                    "sub x10, x10, x9", "stp x0, x10, [x19], #16"]),
       ("ldr.post.neg", ["add x10, x9, #32", "ldr x0, [x10], #-16",
                         "sub x10, x10, x9", "stp x0, x10, [x19], #16"]),
       ("ldr.pre.neg", ["add x10, x9, #32", "ldr x0, [x10, #-8]!",
                        "sub x10, x10, x9", "stp x0, x10, [x19], #16"]),
       ("str.post", ["mov x10, x9", "str x2, [x10], #40",
                     "sub x10, x10, x9", "stp x10, xzr, [x19], #16"] + _dump_scratch()),
       ("strb.pre", ["mov x10, x9", "strb w2, [x10, #21]!",
                     "sub x10, x10, x9", "stp x10, xzr, [x19], #16"] + _dump_scratch()),
       ("ldp.post", ["mov x10, x9", "ldp x0, x1, [x10], #32",
                     "sub x10, x10, x9", "stp x0, x1, [x19], #16",
                     "stp x10, xzr, [x19], #16"]),
       ("ldp.pre.neg", ["add x10, x9, #48", "ldp x0, x1, [x10, #-32]!",
                        "sub x10, x10, x9", "stp x0, x1, [x19], #16",
                        "stp x10, xzr, [x19], #16"]),
       ("stp.pre.neg", ["add x10, x9, #64", "stp x2, x3, [x10, #-48]!",
                        "sub x10, x10, x9", "stp x10, xzr, [x19], #16"] + _dump_scratch()),
       ("ldpsw.off", ["ldpsw x0, x1, [x9, #8]", "stp x0, x1, [x19], #16"]),
       ("ldp.w", ["ldp w0, w1, [x9, #4]", "stp x0, x1, [x19], #16"]),
       ("ldp.sign", ["add x10, x9, #40", "ldp x0, x1, [x10, #-40]", "stp x0, x1, [x19], #16"])]
    # SIMD register through addressing forms (vldst path is separate code)
    + [("ldr.q.roff", ["mov x11, #16", "ldr q4, [x9, x11]", "str q4, [x19], #16"]),
       ("ldr.q.sxtw", ["movn w11, #15", "add x10, x9, #32",
                       "ldr q4, [x10, w11, sxtw]", "str q4, [x19], #16"]),
       ("ldr.d.post", ["mov x10, x9", "ldr d4, [x10], #-8", "str q4, [x19], #16",
                       "sub x10, x10, x9", "stp x10, xzr, [x19], #16"]),
       ("str.q.pre", ["mov x10, x9", "str q2, [x10, #32]!",
                      "sub x10, x10, x9", "stp x10, xzr, [x19], #16"] + _dump_scratch()),
       ("ldr.s.off", ["ldr s4, [x9, #20]", "str q4, [x19], #16"]),
       ("ldr.h.off", ["ldr h4, [x9, #6]", "str q4, [x19], #16"]),
       ("ldr.b.roff", ["mov x11, #13", "ldr b4, [x9, x11]", "str q4, [x19], #16"]),
       ("ldp.q", ["ldp q4, q5, [x9, #16]", "str q4, [x19], #16", "str q5, [x19], #16"]),
       ("ldp.d.post", ["mov x10, x9", "ldp d4, d5, [x10], #16",
                       "str q4, [x19], #16", "str q5, [x19], #16",
                       "sub x10, x10, x9", "stp x10, xzr, [x19], #16"]),
       ("stp.q.pre", ["mov x10, x9", "stp q2, q3, [x10, #32]!",
                      "sub x10, x10, x9", "stp x10, xzr, [x19], #16"] + _dump_scratch()),
       ("ldp.s", ["ldp s4, s5, [x9, #24]", "str q4, [x19], #16", "str q5, [x19], #16"])]
    # crosspage-straddling access through the 64-byte scratch is not
    # possible here (scratch is page-interior); the dedicated crosspage
    # tests in tests/arm64/ cover that.
)

FAMILIES["shift_elem"] = (
    # accumulating / rounding / insert shifts
    [t for op, arrs, amts in [
        ("ssra", ARR_D, (1, 7)), ("usra", ARR_D, (3,)),
        ("srsra", ARR_D, (2,)), ("ursra", ARR_D, (5,)),
        ("srshr", ARR_D, (1, 4)), ("urshr", ARR_D, (6,)),
        ("sli", ARR_D, (0, 3)), ("sri", ARR_D, (1, 5)),
        ("sqshl", ARR_D, (2,)), ("uqshl", ARR_D, (3,)),
    ] for arr in arrs for amt in amts
      for t in raw_tests([(f"{op}.{arr}.{amt}",
                           f"{op} v0.{arr}, v1.{arr}, #{min(amt if op not in ('ssra','usra','srsra','ursra','srshr','urshr','sri') else amt+1, {'8b':7,'16b':7,'4h':15,'8h':15,'2s':31,'4s':31,'2d':63}[arr]) if op in ('sli','sqshl','uqshl') else min(amt+1, {'8b':8,'16b':8,'4h':16,'8h':16,'2s':32,'4s':32,'2d':64}[arr])}")],
                          acc=True)]
    # narrowing shifts (both halves)
    + [t for op in ["shrn", "rshrn", "sqshrn", "sqrshrn", "uqshrn", "uqrshrn",
                    "sqshrun", "sqrshrun"]
       for t in raw_tests([(f"{op}.8b", f"{op} v0.8b, v1.8h, #3"),
                           (f"{op}2.16b", f"{op}2 v0.16b, v1.8h, #5"),
                           (f"{op}.4h", f"{op} v0.4h, v1.4s, #7"),
                           (f"{op}2.8h", f"{op}2 v0.8h, v1.4s, #12"),
                           (f"{op}.2s", f"{op} v0.2s, v1.2d, #9"),
                           (f"{op}2.4s", f"{op}2 v0.4s, v1.2d, #23")], acc=True)]
    # fixed-point converts
    + raw_tests([("scvtf.4s.7", "scvtf v0.4s, v1.4s, #7"),
                 ("ucvtf.2s.11", "ucvtf v0.2s, v1.2s, #11"),
                 ("scvtf.2d.20", "scvtf v0.2d, v1.2d, #20"),
                 ("ucvtf.2d.1", "ucvtf v0.2d, v1.2d, #1")])
    + raw_tests([("fcvtzs.4s.5", "fcvtzs v0.4s, v1.4s, #5"),
                 ("fcvtzu.2s.3", "fcvtzu v0.2s, v1.2s, #3"),
                 ("fcvtzs.2d.10", "fcvtzs v0.2d, v1.2d, #10"),
                 ("fcvtzu.2d.30", "fcvtzu v0.2d, v1.2d, #30")], fp=True)
    # scalar shift-imm extension
    + raw_tests([("ssra.d", "ssra d0, d1, #5"), ("usra.d", "usra d0, d1, #17"),
                 ("srsra.d", "srsra d0, d1, #2"), ("ursra.d", "ursra d0, d1, #40"),
                 ("srshr.d", "srshr d0, d1, #9"), ("urshr.d", "urshr d0, d1, #33")], acc=True)
    + raw_tests([(f"sqshl.{k}", f"sqshl {k}0, {k}1, #{a}") for k, a in
                 (("b", 2), ("h", 7), ("s", 11), ("d", 30))]
                + [(f"uqshl.{k}", f"uqshl {k}0, {k}1, #{a}") for k, a in
                   (("b", 3), ("h", 1), ("s", 20), ("d", 5))]
                + [(f"{op}.{k}", f"{op} {k[0]}0, {k[1]}1, #{a}") for op in
                   ("sqshrn", "sqrshrn", "uqshrn", "uqrshrn", "sqshrun", "sqrshrun")
                   for k, a in (("bh", 3), ("hs", 9), ("sd", 15))])
    # by-element: vector int
    + [t for op, acc in [("mul", 0), ("mla", 1), ("mls", 1),
                         ("sqdmulh", 0), ("sqrdmulh", 0)]
       for t in raw_tests([(f"{op}e.4h", f"{op} v0.4h, v1.4h, v2.h[3]"),
                           (f"{op}e.8h", f"{op} v0.8h, v1.8h, v2.h[7]"),
                           (f"{op}e.2s", f"{op} v0.2s, v1.2s, v2.s[1]"),
                           (f"{op}e.4s", f"{op} v0.4s, v1.4s, v2.s[3]")], acc=bool(acc))]
    # by-element: widening
    + [t for op, acc in [("smull", 0), ("umull", 0), ("sqdmull", 0),
                         ("smlal", 1), ("umlal", 1), ("smlsl", 1), ("umlsl", 1),
                         ("sqdmlal", 1), ("sqdmlsl", 1)]
       for t in raw_tests([(f"{op}e.4s", f"{op} v0.4s, v1.4h, v2.h[2]"),
                           (f"{op}e2.4s", f"{op}2 v0.4s, v1.8h, v2.h[5]"),
                           (f"{op}e.2d", f"{op} v0.2d, v1.2s, v2.s[0]"),
                           (f"{op}e2.2d", f"{op}2 v0.2d, v1.4s, v2.s[2]")], acc=bool(acc))]
    # by-element: FP vector + scalar
    + [t for op, acc in [("fmul", 0), ("fmulx", 0), ("fmla", 1), ("fmls", 1)]
       for t in raw_tests([(f"{op}e.2s", f"{op} v0.2s, v1.2s, v2.s[3]"),
                           (f"{op}e.4s", f"{op} v0.4s, v1.4s, v2.s[1]"),
                           (f"{op}e.2d", f"{op} v0.2d, v1.2d, v2.d[1]"),
                           (f"{op}e.s", f"{op} s0, s1, v2.s[2]"),
                           (f"{op}e.d", f"{op} d0, d1, v2.d[0]")], acc=bool(acc), fp=True)]
)

FAMILIES["crypto"] = (
    # AES: state in v0, round key in v1; results merge/replace v0
    raw_tests([("aese", "aese v0.16b, v1.16b"),
               ("aesd", "aesd v0.16b, v1.16b")], acc=True)
    + raw_tests([("aesmc", "aesmc v0.16b, v1.16b"),
                 ("aesimc", "aesimc v0.16b, v1.16b")])
    # SHA1 two-reg and scalar
    + raw_tests([("sha1su1", "sha1su1 v0.4s, v1.4s")], acc=True)
    + raw_tests([("sha1h", "sha1h s0, s1")])
    # SHA1 three-reg
    + raw_tests([("sha1c", "sha1c q0, s1, v2.4s"),
                 ("sha1p", "sha1p q0, s1, v2.4s"),
                 ("sha1m", "sha1m q0, s1, v2.4s"),
                 ("sha1su0", "sha1su0 v0.4s, v1.4s, v2.4s")], acc=True)
    # SHA256
    + raw_tests([("sha256su0", "sha256su0 v0.4s, v1.4s")], acc=True)
    + raw_tests([("sha256h", "sha256h q0, q1, v2.4s"),
                 ("sha256h2", "sha256h2 q0, q1, v2.4s"),
                 ("sha256su1", "sha256su1 v0.4s, v1.4s, v2.4s")], acc=True)
    # SHA512
    + raw_tests([("sha512su0", "sha512su0 v0.2d, v1.2d")], acc=True)
    + raw_tests([("sha512h", "sha512h q0, q1, v2.2d"),
                 ("sha512h2", "sha512h2 q0, q1, v2.2d"),
                 ("sha512su1", "sha512su1 v0.2d, v1.2d, v2.2d")], acc=True)
    # SHA3
    + raw_tests([("rax1", "rax1 v0.2d, v1.2d, v2.2d")])
    + [t for name, ins in [
        ("eor3", "eor3 v0.16b, v1.16b, v2.16b, v0.16b"),
        ("bcax", "bcax v0.16b, v1.16b, v2.16b, v0.16b"),
        ("xar.1", "xar v0.2d, v1.2d, v2.2d, #1"),
        ("xar.13", "xar v0.2d, v1.2d, v2.2d, #13"),
        ("xar.63", "xar v0.2d, v1.2d, v2.2d, #63"),
      ] for t in raw_tests([(name, ins)], acc=True)]
)

def lse_tests():
    ops = [("ldadd","ldadd"),("ldclr","ldclr"),("ldeor","ldeor"),("ldset","ldset"),
           ("ldsmax","ldsmax"),("ldsmin","ldsmin"),("ldumax","ldumax"),("ldumin","ldumin"),
           ("swp","swp")]
    items = []
    for name, ins in ops:
        for w, xw, sz in [("x","x",8),("w","w",4)]:
            # operand = a distinctive value; seed memory from B, operand from A
            items.append((f"{name}.{w}",
                [f"mov {xw}12, {xw}2",                      # operand
                 f"{ins} {xw}12, {xw}13, [x9]",             # old -> w/x13
                 f"str x13, [x19], #16",                    # dump old (x-reg, zero-ext)
                 f"ldr x14, [x9]", "str x14, [x19], #16"])) # dump new memory
    return mem_tests(items)

FAMILIES["lse"] = lse_tests()

def fixconv_tests():
    """Scalar FP<->fixed-point conversions (the bit21=0 family): SCVTF/UCVTF
    Fd, Rn, #fbits and FCVTZS/FCVTZU Rd, Fn, #fbits. The JIT lowers these as
    register-form convert + power-of-two fmul; every op/width/precision is
    swept over several fbits (edges included) against int corner values and
    FP specials (inf/NaN/saturating magnitudes)."""
    ints = [0, 1, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF,
            0x123456789ABCDEF0, 0xFFFFFFFFFFFFFFFF, 0x8000000000000000]
    fps64 = [0x3FF8000000000000,  # 1.5
             0xC00921FB54442D18,  # -pi
             0x7FF0000000000000,  # +inf
             0xFFF0000000000000,  # -inf
             0x7FF8000000000001,  # NaN
             0x41DFFFFFFFC00000,  # 2147483647.0
             0xC1E0000000000000,  # -2^31
             0x3F50624DD2F1A9FC,  # ~0.001
             0x4690000000000000]  # 2^106: saturates at any fbits
    fps32 = [0x3FC00000, 0xC0490FDB, 0x7F800000, 0xFF800000,
             0x7FC00001, 0x4EFFFFFF, 0xCF000000, 0x3A83126F]
    def load_x2(v):
        out = [f"    movz x2, #0x{v & 0xffff:04x}"]
        for sh in (16, 32, 48):
            out.append(f"    movk x2, #0x{(v >> sh) & 0xffff:04x}, lsl #{sh}")
        return out
    tests = []
    # int -> FP: result dumped as the raw 64-bit V-register low half (the
    # scalar write zeroes the rest, so s-forms are deterministic too).
    for op in ("scvtf", "ucvtf"):
        for freg, greg, fmax in (("s0", "w2", 32), ("d0", "w2", 32),
                                 ("s0", "x2", 64), ("d0", "x2", 64)):
            for fb in (1, 7, fmax // 2, fmax):
                for i, v in enumerate(ints):
                    body = load_x2(v)
                    body.append("    movi v0.16b, #0xff")
                    body.append(f"    {op} {freg}, {greg}, #{fb}")
                    body.append("    fmov x0, d0")
                    body.append("    mov x1, #0")
                    body.append("    stp x0, x1, [x19], #16")
                    tests.append((f"{op}.{freg}.{greg}#{fb}[{i}]", body))
    # FP -> int: w-form results zero-extend into x0.
    for op in ("fcvtzs", "fcvtzu"):
        for gdst, fsrc, fmov_in, fmax, vals in (
                ("w0", "s1", "fmov s1, w2", 32, fps32),
                ("x0", "s1", "fmov s1, w2", 64, fps32),
                ("w0", "d1", "fmov d1, x2", 32, fps64),
                ("x0", "d1", "fmov d1, x2", 64, fps64)):
            for fb in (1, fmax // 2, fmax):
                for i, v in enumerate(vals):
                    body = load_x2(v)
                    body.append(f"    {fmov_in}")
                    body.append(f"    {op} {gdst}, {fsrc}, #{fb}")
                    body.append("    mov x1, #0")
                    body.append("    stp x0, x1, [x19], #16")
                    tests.append((f"{op}.{gdst}.{fsrc}#{fb}[{i}]", body))
    return tests

FAMILIES["fixconv"] = fixconv_tests()

LINUX_PROLOGUE = """\
.text
.global _start
_start:
    sub sp, sp, #131072
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
    sub sp, sp, #131072
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
        off = 0
        for name, body in tests:
            f.write(f"{off}\t{name}\n")
            chunks = sum(1 for l in body if "[x19], #16" in l)
            off += 16 * max(chunks, 1)
    print(f"{len(tests)} tests -> diff_{tag}_{{linux,macos}}.s")

if __name__ == "__main__":
    main()
