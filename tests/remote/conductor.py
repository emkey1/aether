#!/usr/bin/env python3
"""
conductor.py — iSH-AOK differential / crash-resilient test conductor.

Builds each corpus test three ways (i386 ELF, x86_64 ELF, x86_64 macOS Mach-O
oracle), runs every (arch x engine) cell plus external x86 oracle(s), then does a
KEY-BASED comparison (not whole-file diff, so an i386 cell may legitimately omit
64-bit lines). Divergences and crashes are reported and minimized.

Oracles / cells:
  oracle           native x86_64 via `arch -x86_64` (Rosetta Mach-O) — x86_64 truth
  mint:x86_64      x86_64 ELF in mint's Lima Linux VM    — real-Linux x86_64 truth
  mint:i386        i386   ELF in mint's Lima Linux VM    — the i386 truth the M5 lacks
  amd64:interp     ish, /proc/ish/amd64_jit=0
  amd64:jit        ish, /proc/ish/amd64_jit=1
  i386:jit         ish, default i386 JIT
  i386:no_cache    ish, /proc/ish/i386_no_cache_comm=<comm>
  i386:single_step ish, /proc/ish/i386_single_step_comm=<comm>

mint cells are auto-skipped when mint is unreachable, so the M5 runs standalone.
Static ELFs run in the VM via the read-only virtiofs mount of mint's home — a
plain `scp` to mint makes them instantly visible+executable inside the VM.

Backends:
  local-fakefs   the host ./build/ish process IS the device; a JIT bug that kills
                 it surfaces as a signal exit -> CRASH.
  ssh / devicectl  iOS-device backend (journal + heartbeat) — see README, not built.

Config via env: ISH_MINT_HOST (mint), ISH_LIMA_INSTANCE (ish), ISH_MINT_BINDIR
(.ish-oracle/bin, relative to mint's home).

Usage:
  python3 conductor.py run [--tests a,b] [--cells ...] [--seed N] [--no-mint]
  python3 conductor.py minimize --test T --cell amd64:jit [--seed N]
"""
import argparse, json, os, re, shlex, shutil, subprocess, sys, tarfile, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
ISH = REPO / "build" / "ish"
FAKEFSIFY = REPO / "build" / "tools" / "fakefsify"
CORPUS = HERE / "corpus"
WORK = HERE / ".work"
TIMEOUT = 180  # seconds per cell; exceed => HANG

ZIG_TARGET = {"i386": "x86-linux-musl", "x86_64": "x86_64-linux-musl"}

# cell -> dict(arch, engine-spec, kind). kind: rosetta | ish | mint
CELLS = {
    "oracle":           dict(arch="x86_64", engine="oracle",           kind="rosetta"),
    "mint:x86_64":      dict(arch="x86_64", engine="oracle",           kind="mint"),
    "mint:i386":        dict(arch="i386",   engine="oracle",           kind="mint"),
    "amd64:interp":     dict(arch="x86_64", engine="amd64:interp",     kind="ish"),
    "amd64:jit":        dict(arch="x86_64", engine="amd64:jit",        kind="ish"),
    "i386:jit":         dict(arch="i386",   engine="i386:jit",         kind="ish"),
    "i386:no_cache":    dict(arch="i386",   engine="i386:no_cache",    kind="ish"),
    "i386:single_step": dict(arch="i386",   engine="i386:single_step", kind="ish"),
}
ORACLE_KINDS = {"rosetta", "mint"}
DEFAULT_CELLS = list(CELLS)

def discover_tests():
    return sorted(p.stem for p in CORPUS.glob("*.c"))

def sh(cmd, timeout=None):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


# ---------------------------------------------------------------- mint backend

def probe_mint():
    """Return mint config dict if reachable with a runnable Lima instance, else None."""
    host = os.environ.get("ISH_MINT_HOST", "mint")
    inst = os.environ.get("ISH_LIMA_INSTANCE", "ish")
    rb = os.environ.get("ISH_MINT_BINDIR", ".ish-oracle/bin")
    base = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", host]
    try:
        home = sh(base + ["pwd"], timeout=15).stdout.strip()
        if not home:
            return None
        chk = sh(base + ["bash", "-lc", f"limactl shell {shlex.quote(inst)} -- true"], timeout=30)
        if chk.returncode != 0:
            print(f"  mint up but lima '{inst}' not runnable — skipping mint cells", file=sys.stderr)
            return None
    except subprocess.TimeoutExpired:
        return None
    return dict(host=host, instance=inst, base=base,
                remote_bindir=rb, vm_bindir=f"{home}/{rb}")

def push_to_mint(tests, binmap, mint):
    host = mint["host"]
    sh(mint["base"] + ["mkdir", "-p", mint["remote_bindir"]], timeout=20)
    files = [str(binmap[t][a]) for t in tests for a in ZIG_TARGET]
    r = sh(["scp", "-p", "-o", "BatchMode=yes", *files, f"{host}:{mint['remote_bindir']}/"], timeout=60)
    if r.returncode != 0:
        raise SystemExit(f"scp to mint failed:\n{r.stderr}")

def mint_run_cmd(mint, test, arch, extra):
    vmbin = f"{mint['vm_bindir']}/{test}.{arch}"
    inner_args = " ".join(shlex.quote(x) for x in [vmbin, "--engine", "oracle", *extra])
    inner = f"limactl shell {shlex.quote(mint['instance'])} -- {inner_args}"
    remote = "bash -lc " + shlex.quote(inner)
    return ["ssh", "-o", "BatchMode=yes", mint["host"], remote]


# ---------------------------------------------------------------- build

def build_variants(test):
    src = CORPUS / f"{test}.c"
    out = {}
    for arch, triple in ZIG_TARGET.items():
        dst = WORK / "bin" / f"{test}.{arch}"
        r = sh(["zig", "cc", "-target", triple, "-static", "-O2",
                "-I", str(CORPUS), "-o", str(dst), str(src)])
        if r.returncode != 0:
            raise SystemExit(f"build {test}.{arch} failed:\n{r.stderr}")
        out[arch] = dst
    oracle = WORK / "bin" / f"{test}.oracle"
    r = sh(["cc", "-arch", "x86_64", "-O2", "-I", str(CORPUS), "-o", str(oracle), str(src)])
    if r.returncode != 0:
        raise SystemExit(f"build {test}.oracle failed:\n{r.stderr}")
    out["oracle"] = oracle
    return out

SUPERVISOR_SRC = HERE / "guest_supervisor.c"

def build_supervisor():
    """Build the in-guest batch runner for both arches (static)."""
    for arch, triple in ZIG_TARGET.items():
        dst = WORK / "bin" / f"guest_supervisor.{arch}"
        r = sh(["zig", "cc", "-target", triple, "-static", "-O2",
                "-o", str(dst), str(SUPERVISOR_SRC)])
        if r.returncode != 0:
            raise SystemExit(f"build supervisor {arch} failed:\n{r.stderr}")

def build_fakefs(tests):
    stage = WORK / "stage"
    if stage.exists():
        shutil.rmtree(stage)
    (stage / "bin").mkdir(parents=True)
    for test in tests:
        for arch in ZIG_TARGET:
            shutil.copy(WORK / "bin" / f"{test}.{arch}", stage / "bin" / f"{test}.{arch}")
    for arch in ZIG_TARGET:                       # include the supervisor if built
        sup = WORK / "bin" / f"guest_supervisor.{arch}"
        if sup.exists():
            shutil.copy(sup, stage / "bin" / sup.name)
    tar = WORK / "fs.tar.gz"
    with tarfile.open(tar, "w:gz") as t:
        t.add(stage, arcname=".")
    fs = WORK / "fs"
    if fs.exists():
        shutil.rmtree(fs)
    r = sh([str(FAKEFSIFY), str(tar), str(fs)])
    if r.returncode != 0:
        raise SystemExit(f"fakefsify failed:\n{r.stderr}")
    return fs


# ---------------------------------------------------------------- run a cell

class Result:
    def __init__(self, cell, status, rc, lines, stderr, secs):
        self.cell, self.status, self.rc = cell, status, rc
        self.lines, self.stderr, self.secs = lines, stderr, secs

def classify(rc, timed_out):
    if timed_out:
        return "HANG"
    if rc is not None and rc < 0:
        return "CRASH"          # killed by signal -> emulator/host death
    if rc == 0:
        return "OK"
    return "TEST_ERR"

def run_cell(cell, test, binaries, fakefs, mint=None, extra=None):
    spec = CELLS[cell]
    arch, engine, kind = spec["arch"], spec["engine"], spec["kind"]
    extra = extra or []
    t0 = time.time()
    timed_out = False
    if kind == "rosetta":
        cmd = ["arch", "-x86_64", str(binaries["oracle"]), "--engine", "oracle", *extra]
    elif kind == "mint":
        cmd = mint_run_cmd(mint, test, arch, extra)
    else:  # ish
        cmd = [str(ISH), "-f", str(fakefs), f"/bin/{test}.{arch}", "--engine", engine, *extra]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=TIMEOUT)
        rc, out, err = p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired as e:
        rc, out, err, timed_out = None, (e.stdout or b"").decode("utf8", "replace"), "", True
    return Result(cell, classify(rc, timed_out), rc, out.splitlines(), err, time.time() - t0)


# ------------------------------------------------ supervised (device) batch run

def parse_supervisor_stream(text):
    """Parse the SUPER-START/<output>/SUPER-END journal into per-test
    (output_lines, rc). A SUPER-START with no matching SUPER-END means the
    emulator died (or was killed on timeout) during that test -- it is the
    crasher, returned as `dangling`. This is exactly what survives on a real
    device: the app crash drops the ssh stream, but the fsync'd journal still
    shows the dangling START."""
    tests = {}
    cur = None
    for ln in text.splitlines():
        if ln.startswith("SUPER-START "):
            p = ln.split(maxsplit=2)
            cur = {"idx": p[1], "name": p[2] if len(p) > 2 else "?", "out": []}
        elif ln.startswith("SUPER-END ") and cur is not None:
            p = ln.split()
            if len(p) >= 3 and p[1] == cur["idx"]:
                tests[cur["name"]] = (cur["out"], int(p[2]))
                cur = None
        elif cur is not None:
            cur["out"].append(ln)
    return tests, (cur["name"] if cur else None)

def run_supervised(cell, tests, binmap, fakefs, seed=1, timeout=TIMEOUT):
    """Run the whole batch under the guest supervisor in one ish launch (the
    device model). Returns ({test: Result}, crasher_name, seconds)."""
    arch, engine = CELLS[cell]["arch"], CELLS[cell]["engine"]
    cmd = [str(ISH), "-f", str(fakefs), f"/bin/guest_supervisor.{arch}",
           "--engine", engine, "--seed", str(seed), "--journal", "/journal",
           *[f"/bin/{t}.{arch}" for t in tests]]
    t0 = time.time(); timed_out = False
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        out = p.stdout
    except subprocess.TimeoutExpired as e:
        out, timed_out = (e.stdout or b"").decode("utf8", "replace"), True
    secs = time.time() - t0
    parsed, dangling = parse_supervisor_stream(out)
    results = {}
    for t in tests:
        key = f"{t}.{arch}"   # the supervisor journals binaries by basename
        if key in parsed:
            lines, trc = parsed[key]
            st = "OK" if trc == 0 else ("CRASH" if trc < 0 else "TEST_ERR")
            results[t] = Result(cell, st, trc, lines, "", secs / max(len(tests), 1))
        elif key == dangling:
            results[t] = Result(cell, "HANG" if timed_out else "CRASH", None, [],
                                "supervisor journal truncated at this test", secs)
        else:
            results[t] = Result(cell, "SKIP", None, [], "not run (after crasher)", 0.0)
    return results, dangling, secs


# ---------------------------------------------------------------- compare

def parse(lines):
    table = {}
    for ln in lines:
        toks = ln.split()
        if not toks or not any(t.startswith("res=") or t.startswith("fl=") for t in toks):
            continue
        key = " ".join(t for t in toks if not (t.startswith("res=") or t.startswith("fl=")))
        val = " ".join(t for t in toks if t.startswith("res=") or t.startswith("fl="))
        table[key] = val
    return table

def compare(results):
    tables = {r.cell: parse(r.lines) for r in results if r.status == "OK"}
    all_keys = set().union(*tables.values()) if tables else set()
    mismatches = []
    for key in sorted(all_keys):
        vals = {c: t[key] for c, t in tables.items() if key in t}
        if len(set(vals.values())) > 1:
            mismatches.append((key, vals))
    return mismatches, tables

def oracle_value(vals):
    """Pick a ground-truth value for a key: prefer Rosetta, then mint."""
    for c in ("oracle", "mint:x86_64", "mint:i386"):
        if c in vals:
            return c, vals[c]
    return None, None


# ---------------------------------------------------------------- minimize

def minimize_crash(cell, test, binaries, fakefs):
    full = run_cell(cell, test, binaries, fakefs)
    if full.status not in ("CRASH", "HANG"):
        print("  full run did not crash; nothing to minimize.")
        return None
    r = sh(["arch", "-x86_64", str(binaries["oracle"]), "--list"])
    n = int(re.search(r"cases=(\d+)", r.stdout).group(1))
    print(f"  scanning {n} cases for the crasher in {cell} ...")
    for i in range(n):
        rr = run_cell(cell, test, binaries, fakefs, extra=["--case", str(i)])
        if rr.status in ("CRASH", "HANG"):
            print(f"  minimal crashing case: --case {i}  (status {rr.status})")
            return i
    print("  no single case reproduced the crash (state-dependent).")
    return None


# ---------------------------------------------------------------- report

def report(test, results, mismatches):
    print(f"\n=== {test} ===")
    for r in results:
        flag = {"OK": "ok", "CRASH": "CRASH", "HANG": "HANG", "TEST_ERR": "ERR",
                "SKIP": "skip"}[r.status]
        n = len(parse(r.lines)) if r.status == "OK" else 0
        oracle_tag = " (oracle)" if CELLS[r.cell]["kind"] in ORACLE_KINDS else ""
        print(f"  [{flag:5}] {r.cell:16}{oracle_tag:9} rc={r.rc} {n:5} cases {r.secs:6.2f}s")
        if r.status != "OK" and r.stderr.strip():
            print("    stderr:", r.stderr.strip().splitlines()[-1][:120])
    if not mismatches:
        print("  → all cells agree ✓")
        return
    print(f"  → {len(mismatches)} divergent keys")
    by_op = {}
    for key, _ in mismatches:
        by_op[key.split()[0]] = by_op.get(key.split()[0], 0) + 1
    print("    by op:", ", ".join(f"{k}={v}" for k, v in sorted(by_op.items(), key=lambda x: -x[1])))
    for key, vals in mismatches[:8]:
        oc, ov = oracle_value(vals)
        diffs = {c: v for c, v in vals.items() if v != ov and c not in ORACLE_CELLS}
        print(f"    {key}: {oc}[{ov}] vs " + "; ".join(f"{c}[{v}]" for c, v in diffs.items()))
    if len(mismatches) > 8:
        print(f"    ... +{len(mismatches)-8} more")

ORACLE_CELLS = {c for c, s in CELLS.items() if s["kind"] in ORACLE_KINDS}


# ---------------------------------------------------------------- main

def cmd_run(args):
    tests = args.tests.split(",") if args.tests else discover_tests()
    cells = args.cells.split(",") if args.cells else DEFAULT_CELLS[:]
    WORK.mkdir(exist_ok=True)
    (WORK / "bin").mkdir(exist_ok=True)

    mint = None if args.no_mint else probe_mint()
    if any(CELLS[c]["kind"] == "mint" for c in cells):
        if mint:
            print(f"mint oracle: {mint['host']} lima/{mint['instance']} → {mint['vm_bindir']}")
        else:
            print("mint oracle unavailable — running M5-local cells only")
            cells = [c for c in cells if CELLS[c]["kind"] != "mint"]

    print(f"building {len(tests)} test(s): {', '.join(tests)}")
    binmap = {t: build_variants(t) for t in tests}
    fakefs = build_fakefs(tests)
    if mint and any(CELLS[c]["kind"] == "mint" for c in cells):
        push_to_mint(tests, binmap, mint)

    summary, any_fail = {}, False
    for test in tests:
        results = [run_cell(c, test, binmap[test], fakefs, mint=mint,
                            extra=["--seed", str(args.seed)]) for c in cells]
        mism, _ = compare(results)
        report(test, results, mism)
        bad = [r.cell for r in results if r.status in ("CRASH", "HANG")]
        if bad or mism:
            any_fail = True
        summary[test] = {
            "cells": {r.cell: {"status": r.status, "rc": r.rc,
                               "cases": len(parse(r.lines)), "secs": round(r.secs, 2)}
                      for r in results},
            "divergent_keys": len(mism),
            "crashed_cells": bad,
        }
    (WORK / "results.json").write_text(json.dumps(summary, indent=2))
    print(f"\nwrote {WORK / 'results.json'}")
    sys.exit(1 if any_fail else 0)

def cmd_minimize(args):
    WORK.mkdir(exist_ok=True)
    (WORK / "bin").mkdir(exist_ok=True)
    binmap = build_variants(args.test)
    fakefs = build_fakefs([args.test])
    minimize_crash(args.cell, args.test, binmap, fakefs)

def cmd_supervise(args):
    """Device-model run: ish cells go through the guest supervisor (one launch,
    journaled) so a crash that drops the connection is still attributable; the
    oracle/mint cells run per-test as usual. Run locally it validates the same
    comparison and exercises the journal/crash-reconciliation path."""
    tests = args.tests.split(",") if args.tests else discover_tests()
    cells = args.cells.split(",") if args.cells else DEFAULT_CELLS[:]
    WORK.mkdir(exist_ok=True); (WORK / "bin").mkdir(exist_ok=True)
    mint = None if args.no_mint else probe_mint()
    if any(CELLS[c]["kind"] == "mint" for c in cells):
        if mint:
            print(f"mint oracle: {mint['host']} lima/{mint['instance']}")
        else:
            print("mint oracle unavailable — running M5-local cells only")
            cells = [c for c in cells if CELLS[c]["kind"] != "mint"]
    print(f"building {len(tests)} test(s) + supervisor: {', '.join(tests)}")
    binmap = {t: build_variants(t) for t in tests}
    build_supervisor()
    fakefs = build_fakefs(tests)
    if mint and any(CELLS[c]["kind"] == "mint" for c in cells):
        push_to_mint(tests, binmap, mint)

    per_cell = {}
    for cell in cells:
        if CELLS[cell]["kind"] == "ish":
            res, dangling, secs = run_supervised(cell, tests, binmap, fakefs, args.seed)
            per_cell[cell] = res
            print(f"  supervised {cell:16} {len(tests)} tests {secs:6.2f}s  "
                  + (f"CRASH→{dangling}" if dangling else "clean"))
        else:
            per_cell[cell] = {t: run_cell(cell, t, binmap[t], fakefs, mint=mint,
                                          extra=["--seed", str(args.seed)]) for t in tests}

    any_fail = False
    for test in tests:
        results = [per_cell[c][test] for c in cells]
        mism, _ = compare(results)
        report(test, results, mism)
        if mism or any(r.status in ("CRASH", "HANG") for r in results):
            any_fail = True
    sys.exit(1 if any_fail else 0)

def main():
    ap = argparse.ArgumentParser(description="iSH-AOK differential test conductor")
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run"); r.set_defaults(fn=cmd_run)
    r.add_argument("--tests"); r.add_argument("--cells")
    r.add_argument("--seed", type=int, default=1)
    r.add_argument("--no-mint", action="store_true", help="skip the mint VM oracle cells")
    m = sub.add_parser("minimize"); m.set_defaults(fn=cmd_minimize)
    m.add_argument("--test", required=True); m.add_argument("--cell", required=True)
    m.add_argument("--seed", type=int, default=1)
    sp = sub.add_parser("supervise"); sp.set_defaults(fn=cmd_supervise)
    sp.add_argument("--tests"); sp.add_argument("--cells")
    sp.add_argument("--seed", type=int, default=1)
    sp.add_argument("--no-mint", action="store_true", help="skip the mint VM oracle cells")
    for tool in (ISH, FAKEFSIFY):
        if not tool.exists():
            raise SystemExit(f"missing {tool}; run `ninja -C build` first")
    args = ap.parse_args()
    args.fn(args)

if __name__ == "__main__":
    main()
